#include "direct_vulkan_frame_handoff_pipeline.h"

#include "render_internal.h"
#include "vulkan_resources.h"

extern "C" {
#include <libavutil/pixdesc.h>
#include <libavutil/pixfmt.h>
}

#include <algorithm>

namespace {

QSize toQSize(const jcut::core::SizeI& size)
{
    return QSize(size.width, size.height);
}

} // namespace

DirectVulkanFrameHandoffPipeline::DirectVulkanFrameHandoffPipeline() = default;
DirectVulkanFrameHandoffPipeline::~DirectVulkanFrameHandoffPipeline() = default;

bool DirectVulkanFrameHandoffPipeline::initialize(
    const jcut::vulkan_detector::VulkanDeviceContext& context,
    QString* errorMessage)
{
    release();
    for (auto& handoff : m_handoffs) {
        handoff = std::make_unique<jcut::vulkan_detector::VulkanDetectorFrameHandoff>();
        std::string error;
        if (!handoff->initialize(context, &error)) {
            m_lastError = QString::fromStdString(error.empty()
                ? std::string("Failed to initialize Vulkan frame handoff.")
                : error);
            if (errorMessage) {
                *errorMessage = m_lastError;
            }
            release();
            return false;
        }
    }
    m_lastError.clear();
    return true;
}

void DirectVulkanFrameHandoffPipeline::release()
{
    for (auto& handoff : m_handoffs) {
        if (handoff) {
            handoff->release();
            handoff.reset();
        }
    }
    m_lastError.clear();
}

bool DirectVulkanFrameHandoffPipeline::isInitialized() const
{
    return std::all_of(m_handoffs.cbegin(), m_handoffs.cend(), [](const auto& handoff) {
        return handoff && handoff->isInitialized();
    });
}

jcut::vulkan_detector::VulkanDetectorFrameHandoff*
DirectVulkanFrameHandoffPipeline::handoffForFrameSlot(uint32_t frameSlot)
{
    if (!isInitialized()) {
        return nullptr;
    }
    return m_handoffs[static_cast<size_t>(frameSlot) % m_handoffs.size()].get();
}

DirectVulkanFrameHandoffPipeline::Result DirectVulkanFrameHandoffPipeline::record(
    VkCommandBuffer commandBuffer,
    uint32_t frameSlot,
    const VulkanPreviewClipFrameStatus& status,
    VulkanResources* resources,
    DirectVulkanPreviewStats* stats)
{
    Result result;
    if (commandBuffer == VK_NULL_HANDLE) {
        if (stats) {
            stats->lastHandoffMode = QStringLiteral("command_buffer_unavailable");
            stats->lastHandoffError = QStringLiteral("Direct Vulkan preview has no command buffer for frame handoff.");
        }
        return result;
    }
    if (!resources || !resources->isReady()) {
        if (stats) {
            stats->lastHandoffMode = QStringLiteral("texture_pipeline_unavailable");
            stats->lastHandoffError = QStringLiteral("Vulkan texture resources are unavailable.");
        }
        return result;
    }
    if (!status.hasFrame) {
        if (stats) {
            stats->lastHandoffMode = QStringLiteral("decoded_frame_unavailable");
            stats->lastHandoffError = status.missingReason.isEmpty()
                ? QStringLiteral("Active Vulkan clip has no usable decoded frame.")
                : status.missingReason;
        }
        return result;
    }
    if (!status.externalVulkanFrame && !status.frame.hasHardwareFrame()) {
        if (stats) {
            stats->lastHandoffMode = QStringLiteral("vulkan_handoff_required");
            stats->lastHandoffError = QStringLiteral(
                "Direct Vulkan preview requires a hardware or external Vulkan frame; CPU upload fallback is disabled.");
        }
        return result;
    }
    auto* handoff = handoffForFrameSlot(frameSlot);
    if (!handoff) {
        if (stats) {
            stats->lastHandoffMode = QStringLiteral("handoff_pipeline_unavailable");
            stats->lastHandoffError = m_lastError.isEmpty()
                ? QStringLiteral("Vulkan frame handoff pipeline is not initialized.")
                : m_lastError;
        }
        return result;
    }

    result.attempted = true;
    if (stats) {
        ++stats->handoffAttempts;
    }

    std::string error;
    double uploadMs = 0.0;
    bool ok = false;
    if (status.externalVulkanFrame) {
        render_detail::OffscreenVulkanFrame offscreenFrame;
        offscreenFrame.physicalDevice = status.externalPhysicalDevice;
        offscreenFrame.device = status.externalDevice;
        offscreenFrame.queue = status.externalQueue;
        offscreenFrame.queueFamilyIndex = status.externalQueueFamilyIndex;
        offscreenFrame.image = status.externalImage;
        offscreenFrame.imageView = status.externalImageView;
        offscreenFrame.imageMemory = status.externalImageMemory;
        offscreenFrame.imageLayout = status.externalImageLayout;
        offscreenFrame.imageFormat = status.externalImageFormat;
        offscreenFrame.readySemaphoreFd = status.externalReadySemaphoreFd;
        offscreenFrame.size = {status.frameSize.width(), status.frameSize.height()};
        offscreenFrame.valid = status.hasFrame;
        ok = handoff->recordImportedFrameCopy(commandBuffer, offscreenFrame, &error);
    } else {
        ok = handoff->recordHardwareFrameUpload(commandBuffer, status.frame, &uploadMs, &error);
    }

    if (!ok) {
        m_lastError = QString::fromStdString(error);
        if (stats) {
            ++stats->handoffFailures;
            stats->lastHandoffError = m_lastError;
            stats->lastHandoffMode = QStringLiteral("invalid");
            const auto& probe = handoff->lastProbe();
            stats->lastProbePath = probe.path;
            stats->lastProbeReason = probe.reason;
        }
        return result;
    }

    const jcut::vulkan_detector::VulkanExternalImage external = handoff->externalImage();
    result.sampledFrameReady = resources->setSampledImage(external.imageView, external.imageLayout);
    result.descriptorSet = result.sampledFrameReady ? resources->descriptorSet() : VK_NULL_HANDLE;
    result.descriptorSetIndex = static_cast<int>(resources->descriptorSetIndex());
    result.descriptorSetCount = static_cast<int>(resources->descriptorSetCount());
    result.image = handoff->image();
    result.imageView = external.imageView;
    result.layout = handoff->imageLayout();
    result.size = external.size;
    result.format = handoff->imageFormat();

    if (stats) {
        ++stats->handoffSuccesses;
        stats->lastUploadMs = uploadMs;
        stats->lastExternalImageSize = toQSize(external.size);
        stats->lastHandoffError.clear();
        stats->lastHandoffMode =
            handoff->lastMode() == jcut::vulkan_detector::FrameHandoffMode::HardwareDirect
                ? QStringLiteral("hardware_direct")
                : (handoff->lastMode() == jcut::vulkan_detector::FrameHandoffMode::ExternalMemoryImport
                       ? QStringLiteral("external_memory_import")
                       : QStringLiteral("invalid"));
        const auto& probe = handoff->lastProbe();
        stats->lastProbePath = probe.path;
        stats->lastProbeReason = probe.reason;
        stats->lastHardwareSwFormat = framePixelFormatName(status.frame.hardwareSwPixelFormat());
        stats->lastVulkanImageFormat = vulkanFormatName(handoff->imageFormat());
        stats->lastYuvRgbMatrix = handoff->lastYuvRgbMatrix();
        stats->descriptorSetIndex = static_cast<int>(resources->descriptorSetIndex());
        stats->descriptorSetCount = static_cast<int>(resources->descriptorSetCount());
        if (result.sampledFrameReady) {
            ++stats->sampledImageReady;
        }
    }

    return result;
}

QString DirectVulkanFrameHandoffPipeline::framePixelFormatName(int format)
{
    const char* name = av_get_pix_fmt_name(static_cast<AVPixelFormat>(format));
    return name ? QString::fromLatin1(name) : QStringLiteral("unknown:%1").arg(format);
}

QString DirectVulkanFrameHandoffPipeline::vulkanFormatName(VkFormat format)
{
    switch (format) {
    case VK_FORMAT_R8G8B8A8_UNORM: return QStringLiteral("VK_FORMAT_R8G8B8A8_UNORM");
    case VK_FORMAT_B8G8R8A8_UNORM: return QStringLiteral("VK_FORMAT_B8G8R8A8_UNORM");
    case VK_FORMAT_R8G8B8A8_SRGB: return QStringLiteral("VK_FORMAT_R8G8B8A8_SRGB");
    case VK_FORMAT_B8G8R8A8_SRGB: return QStringLiteral("VK_FORMAT_B8G8R8A8_SRGB");
    default: return QStringLiteral("VkFormat:%1").arg(static_cast<int>(format));
    }
}
