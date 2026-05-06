#include "direct_vulkan_preview_presenter.h"

#include <QDebug>
#include <QHash>
#include <QLabel>
#include <QVersionNumber>
#include <QVBoxLayout>
#include <QVulkanFunctions>
#include <QVulkanWindow>
#include <QWidget>

#include <vulkan/vulkan.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <vector>

namespace {

class DirectVulkanPreviewRenderer final : public QVulkanWindowRenderer {
public:
    DirectVulkanPreviewRenderer(DirectVulkanPreviewWindow* owner, QVulkanWindow* window)
        : m_owner(owner), m_window(window) {}

    void initResources() override;
    void startNextFrame() override;
    void physicalDeviceLost() override;
    void logicalDeviceLost() override;

private:
    DirectVulkanPreviewWindow* m_owner = nullptr;
    QVulkanWindow* m_window = nullptr;
    QVulkanDeviceFunctions* m_devFuncs = nullptr;
};

int findGraphicsPresentDeviceIndex(QVulkanInstance* instance, QWindow* window)
{
    if (!instance || !window) {
        return -1;
    }
    auto enumeratePhysicalDevices = reinterpret_cast<PFN_vkEnumeratePhysicalDevices>(
        instance->getInstanceProcAddr("vkEnumeratePhysicalDevices"));
    auto getQueueFamilyProperties = reinterpret_cast<PFN_vkGetPhysicalDeviceQueueFamilyProperties>(
        instance->getInstanceProcAddr("vkGetPhysicalDeviceQueueFamilyProperties"));
    if (!enumeratePhysicalDevices || !getQueueFamilyProperties) {
        return -1;
    }

    uint32_t deviceCount = 0;
    if (enumeratePhysicalDevices(instance->vkInstance(), &deviceCount, nullptr) != VK_SUCCESS || deviceCount == 0) {
        return -1;
    }
    std::vector<VkPhysicalDevice> devices(deviceCount);
    if (enumeratePhysicalDevices(instance->vkInstance(), &deviceCount, devices.data()) != VK_SUCCESS) {
        return -1;
    }
    for (int deviceIndex = 0; deviceIndex < static_cast<int>(devices.size()); ++deviceIndex) {
        VkPhysicalDevice device = devices[static_cast<size_t>(deviceIndex)];
        uint32_t familyCount = 0;
        getQueueFamilyProperties(device, &familyCount, nullptr);
        std::vector<VkQueueFamilyProperties> families(familyCount);
        getQueueFamilyProperties(device, &familyCount, families.data());
        for (uint32_t family = 0; family < familyCount; ++family) {
            if ((families[family].queueFlags & VK_QUEUE_GRAPHICS_BIT) &&
                instance->supportsPresent(device, family, window)) {
                return deviceIndex;
            }
        }
    }
    return -1;
}

bool clipActiveAtFrame(const TimelineClip& clip,
                       const QVector<TimelineTrack>& tracks,
                       int64_t frame)
{
    return clipVisualPlaybackEnabled(clip, tracks) &&
           frame >= clip.startFrame &&
           frame < clip.startFrame + std::max<int64_t>(1, clip.durationFrames);
}

VkClearRect clipRectForSwapchain(const TimelineClip& clip,
                                 int ordinal,
                                 const QSize& swapSize)
{
    const int width = std::max(1, swapSize.width());
    const int height = std::max(1, swapSize.height());
    const double scaleX = std::clamp(static_cast<double>(clip.baseScaleX), 0.08, 4.0);
    const double scaleY = std::clamp(static_cast<double>(clip.baseScaleY), 0.08, 4.0);
    const int rectW = std::clamp(static_cast<int>(width * 0.34 * scaleX), 36, width);
    const int rectH = std::clamp(static_cast<int>(height * 0.34 * scaleY), 36, height);
    const int trackOffset = std::max(0, clip.trackIndex) * 34;
    int cx = width / 2 + static_cast<int>(clip.baseTranslationX) + ((ordinal % 3) - 1) * (width / 10);
    int cy = height / 2 + static_cast<int>(clip.baseTranslationY) + trackOffset - (ordinal / 3) * 28;
    int x = std::clamp(cx - rectW / 2, 0, std::max(0, width - rectW));
    int y = std::clamp(cy - rectH / 2, 0, std::max(0, height - rectH));

    VkClearRect clearRect{};
    clearRect.rect.offset = {x, y};
    clearRect.rect.extent = {static_cast<uint32_t>(rectW), static_cast<uint32_t>(rectH)};
    clearRect.baseArrayLayer = 0;
    clearRect.layerCount = 1;
    return clearRect;
}

VkClearValue clipColor(const TimelineClip& clip, int ordinal, bool selected)
{
    QColor color = clip.color.isValid() ? clip.color : QColor::fromHsv((ordinal * 47) % 360, 150, 230);
    if (selected) {
        color = color.lighter(145);
    }
    VkClearValue clear{};
    clear.color.float32[0] = static_cast<float>(color.redF());
    clear.color.float32[1] = static_cast<float>(color.greenF());
    clear.color.float32[2] = static_cast<float>(color.blueF());
    clear.color.float32[3] = static_cast<float>(std::clamp(static_cast<double>(clip.opacity), 0.18, 1.0));
    return clear;
}

const VulkanPreviewClipFrameStatus* frameStatusForClip(const PreviewInteractionState* state, const QString& clipId)
{
    if (!state) {
        return nullptr;
    }
    for (const VulkanPreviewClipFrameStatus& status : state->vulkanFrameStatuses) {
        if (status.clipId == clipId) {
            return &status;
        }
    }
    return nullptr;
}

VkClearValue clipColorForStatus(const TimelineClip& clip,
                                int ordinal,
                                bool selected,
                                const VulkanPreviewClipFrameStatus* status)
{
    VkClearValue clear = clipColor(clip, ordinal, selected);
    if (!status || !status->hasFrame) {
        clear.color.float32[0] = 0.75f;
        clear.color.float32[1] = 0.18f;
        clear.color.float32[2] = 0.08f;
        clear.color.float32[3] = 0.55f;
        return clear;
    }
    if (status->hardwareFrame || status->gpuTexture) {
        clear.color.float32[0] = 0.08f;
        clear.color.float32[1] = status->exact ? 0.72f : 0.52f;
        clear.color.float32[2] = 0.38f;
    } else if (status->cpuImage) {
        clear.color.float32[0] = 0.84f;
        clear.color.float32[1] = status->exact ? 0.58f : 0.42f;
        clear.color.float32[2] = 0.12f;
    }
    clear.color.float32[3] = std::clamp(clear.color.float32[3], 0.35f, 1.0f);
    return clear;
}

VkClearValue boxstreamOverlayColor(const VulkanPreviewBoxstreamOverlay& overlay)
{
    const uint hueHash = qHash(overlay.streamId.isEmpty()
                                   ? QString::number(overlay.trackId)
                                   : overlay.streamId);
    QColor color = QColor::fromHsv(static_cast<int>(hueHash % 360), 210, 255);
    VkClearValue clear{};
    clear.color.float32[0] = static_cast<float>(color.redF());
    clear.color.float32[1] = static_cast<float>(color.greenF());
    clear.color.float32[2] = static_cast<float>(color.blueF());
    clear.color.float32[3] = static_cast<float>(std::clamp(0.55 + (overlay.confidence * 0.35), 0.55, 0.95));
    return clear;
}

VkClearRect normalizedBoxToSwapchainRect(const QRectF& normalizedBox,
                                         const VkClearRect& clipRect,
                                         int lineInset = 0)
{
    const int clipX = clipRect.rect.offset.x;
    const int clipY = clipRect.rect.offset.y;
    const int clipW = static_cast<int>(clipRect.rect.extent.width);
    const int clipH = static_cast<int>(clipRect.rect.extent.height);
    const int x = clipX + std::clamp(static_cast<int>(std::round(normalizedBox.left() * clipW)), 0, clipW);
    const int y = clipY + std::clamp(static_cast<int>(std::round(normalizedBox.top() * clipH)), 0, clipH);
    const int r = clipX + std::clamp(static_cast<int>(std::round(normalizedBox.right() * clipW)), 0, clipW);
    const int b = clipY + std::clamp(static_cast<int>(std::round(normalizedBox.bottom() * clipH)), 0, clipH);
    VkClearRect rect{};
    rect.rect.offset = {std::max(clipX, x + lineInset), std::max(clipY, y + lineInset)};
    rect.rect.extent = {
        static_cast<uint32_t>(std::max(1, r - x - (lineInset * 2))),
        static_cast<uint32_t>(std::max(1, b - y - (lineInset * 2)))
    };
    rect.baseArrayLayer = 0;
    rect.layerCount = 1;
    return rect;
}

void clearRect(QVulkanDeviceFunctions* funcs,
               VkCommandBuffer cb,
               const VkClearValue& value,
               const VkClearRect& rect)
{
    VkClearAttachment attachment{};
    attachment.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    attachment.colorAttachment = 0;
    attachment.clearValue = value;
    funcs->vkCmdClearAttachments(cb, 1, &attachment, 1, &rect);
}

void clearBoxOutline(QVulkanDeviceFunctions* funcs,
                     VkCommandBuffer cb,
                     const VkClearValue& value,
                     const VkClearRect& boxRect,
                     int thickness)
{
    const int x = boxRect.rect.offset.x;
    const int y = boxRect.rect.offset.y;
    const int w = static_cast<int>(boxRect.rect.extent.width);
    const int h = static_cast<int>(boxRect.rect.extent.height);
    const int t = std::max(1, std::min({thickness, std::max(1, w), std::max(1, h)}));
    auto makeRect = [](int rx, int ry, int rw, int rh) {
        VkClearRect rect{};
        rect.rect.offset = {rx, ry};
        rect.rect.extent = {
            static_cast<uint32_t>(std::max(1, rw)),
            static_cast<uint32_t>(std::max(1, rh))
        };
        rect.baseArrayLayer = 0;
        rect.layerCount = 1;
        return rect;
    };
    clearRect(funcs, cb, value, makeRect(x, y, w, t));
    clearRect(funcs, cb, value, makeRect(x, y + h - t, w, t));
    clearRect(funcs, cb, value, makeRect(x, y, t, h));
    clearRect(funcs, cb, value, makeRect(x + w - t, y, t, h));
}

} // namespace

class DirectVulkanPreviewWindow final : public QVulkanWindow {
public:
    DirectVulkanPreviewWindow(PreviewInteractionState* state,
                              int64_t* presentedFrames,
                              bool* active,
                              QString* failureReason)
        : m_state(state),
          m_presentedFrames(presentedFrames),
          m_active(active),
          m_failureReason(failureReason)
    {
        setSurfaceType(QSurface::VulkanSurface);
        setTitle(QStringLiteral("JCut Direct Vulkan Preview"));
        setFlags(QVulkanWindow::PersistentResources);
    }

    QVulkanWindowRenderer* createRenderer() override
    {
        return new DirectVulkanPreviewRenderer(this, this);
    }

    PreviewInteractionState* state() const { return m_state; }
    void markPresented()
    {
        if (m_presentedFrames) {
            ++(*m_presentedFrames);
        }
    }
    void markFailure(const QString& reason)
    {
        if (m_active) {
            *m_active = false;
        }
        if (m_failureReason) {
            *m_failureReason = reason;
        }
        qWarning().noquote() << QStringLiteral("[vulkan-preview] %1").arg(reason);
    }

private:
    PreviewInteractionState* m_state = nullptr;
    int64_t* m_presentedFrames = nullptr;
    bool* m_active = nullptr;
    QString* m_failureReason = nullptr;
};

void DirectVulkanPreviewRenderer::initResources()
{
    m_devFuncs = m_window && m_window->vulkanInstance()
        ? m_window->vulkanInstance()->deviceFunctions(m_window->device())
        : nullptr;
    if (m_window && m_window->physicalDeviceProperties()) {
        const VkPhysicalDeviceProperties* props = m_window->physicalDeviceProperties();
        qInfo().noquote()
            << QStringLiteral("[vulkan-preview] direct presenter device=%1 vendor=0x%2 type=%3")
                   .arg(QString::fromLatin1(props->deviceName))
                   .arg(QString::number(props->vendorID, 16))
                   .arg(static_cast<int>(props->deviceType));
    }
}

void DirectVulkanPreviewRenderer::startNextFrame()
{
    if (!m_owner || !m_window || !m_devFuncs) {
        return;
    }

    const PreviewInteractionState* state = m_owner->state();
    QColor base = state ? state->backgroundColor : QColor(Qt::black);
    if (!base.isValid()) {
        base = QColor(Qt::black);
    }

    const double framePhase = state ? std::fmod(static_cast<double>(state->currentFrame), 240.0) / 240.0 : 0.0;
    const double clipSignal = state ? std::min(1.0, static_cast<double>(std::max(0, state->clipCount)) / 8.0) : 0.0;
    const double selectedSignal = state && !state->selectedClipId.isEmpty() ? 0.18 : 0.0;

    VkClearValue clearValues[2]{};
    clearValues[0].color.float32[0] = std::clamp(base.redF() * 0.55 + framePhase * 0.20 + selectedSignal, 0.0, 1.0);
    clearValues[0].color.float32[1] = std::clamp(base.greenF() * 0.55 + clipSignal * 0.26 + 0.05, 0.0, 1.0);
    clearValues[0].color.float32[2] = std::clamp(base.blueF() * 0.55 + (1.0 - framePhase) * 0.13 + 0.08, 0.0, 1.0);
    clearValues[0].color.float32[3] = 1.0f;
    clearValues[1].depthStencil.depth = 1.0f;
    clearValues[1].depthStencil.stencil = 0;

    VkRenderPassBeginInfo rp{};
    rp.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
    rp.renderPass = m_window->defaultRenderPass();
    rp.framebuffer = m_window->currentFramebuffer();
    const QSize swapSize = m_window->swapChainImageSize();
    rp.renderArea.offset = {0, 0};
    rp.renderArea.extent = {static_cast<uint32_t>(std::max(1, swapSize.width())),
                            static_cast<uint32_t>(std::max(1, swapSize.height()))};
    rp.clearValueCount = m_window->depthStencilFormat() == VK_FORMAT_UNDEFINED ? 1u : 2u;
    rp.pClearValues = clearValues;

    VkCommandBuffer cb = m_window->currentCommandBuffer();
    m_devFuncs->vkCmdBeginRenderPass(cb, &rp, VK_SUBPASS_CONTENTS_INLINE);
    if (state) {
        int ordinal = 0;
        QHash<QString, VkClearRect> activeClipRects;
        for (const TimelineClip& clip : state->clips) {
            if (!clipActiveAtFrame(clip, state->tracks, state->currentFrame)) {
                continue;
            }
            const bool selected = !state->selectedClipId.isEmpty() && clip.id == state->selectedClipId;
            const VulkanPreviewClipFrameStatus* status = frameStatusForClip(state, clip.id);
            VkClearAttachment attachment{};
            attachment.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
            attachment.colorAttachment = 0;
            attachment.clearValue = clipColorForStatus(clip, ordinal, selected, status);
            const VkClearRect rect = clipRectForSwapchain(clip, ordinal, swapSize);
            m_devFuncs->vkCmdClearAttachments(cb, 1, &attachment, 1, &rect);
            activeClipRects.insert(clip.id, rect);
            ++ordinal;
        }
        const int thickness = std::max(2, std::min(swapSize.width(), swapSize.height()) / 180);
        for (const VulkanPreviewBoxstreamOverlay& overlay : state->boxstreamOverlays) {
            const auto it = activeClipRects.constFind(overlay.clipId);
            if (it == activeClipRects.constEnd() || !overlay.boxNorm.isValid()) {
                continue;
            }
            const VkClearRect boxRect = normalizedBoxToSwapchainRect(overlay.boxNorm, it.value());
            clearBoxOutline(m_devFuncs, cb, boxstreamOverlayColor(overlay), boxRect, thickness);
        }
    }
    m_devFuncs->vkCmdEndRenderPass(cb);

    m_owner->markPresented();
    m_window->frameReady();
    if (state && state->playing) {
        m_window->requestUpdate();
    }
}

void DirectVulkanPreviewRenderer::physicalDeviceLost()
{
    if (m_owner) {
        m_owner->markFailure(QStringLiteral("Physical Vulkan device lost during direct preview presentation."));
    }
}

void DirectVulkanPreviewRenderer::logicalDeviceLost()
{
    if (m_owner) {
        m_owner->markFailure(QStringLiteral("Logical Vulkan device lost during direct preview presentation."));
    }
}

DirectVulkanPreviewPresenter::DirectVulkanPreviewPresenter(PreviewInteractionState* state, QWidget* parent)
    : m_state(state)
{
    m_instance = std::make_unique<QVulkanInstance>();
    m_instance->setApiVersion(QVersionNumber(1, 1));
    const QVulkanInfoVector<QVulkanExtension> supportedExtensions = m_instance->supportedExtensions();
    QByteArrayList requestedExtensions;
    auto requestExtension = [&](const char* name) {
        const QByteArray ext(name);
        if (supportedExtensions.contains(ext) && !requestedExtensions.contains(ext)) {
            requestedExtensions.push_back(ext);
        }
    };
    requestExtension(VK_KHR_SURFACE_EXTENSION_NAME);
    if (!qEnvironmentVariableIsEmpty("WAYLAND_DISPLAY")) {
        requestExtension("VK_KHR_wayland_surface");
    }
    if (!qEnvironmentVariableIsEmpty("DISPLAY")) {
        requestExtension("VK_KHR_xcb_surface");
        requestExtension("VK_KHR_xlib_surface");
    }
    if (!requestedExtensions.isEmpty()) {
        m_instance->setExtensions(requestedExtensions);
    }
    if (!m_instance->create()) {
        m_failureReason = QStringLiteral("QVulkanInstance::create() failed for direct Vulkan preview presenter: VkResult %1.")
                              .arg(static_cast<int>(m_instance->errorCode()));
        return;
    }

    QWindow probeWindow;
    probeWindow.setSurfaceType(QSurface::VulkanSurface);
    probeWindow.setVulkanInstance(m_instance.get());
    probeWindow.create();
    const VkSurfaceKHR probeSurface = QVulkanInstance::surfaceForWindow(&probeWindow);
    if (probeSurface == VK_NULL_HANDLE) {
        m_failureReason = QStringLiteral("Direct Vulkan preview presenter failed to create a platform VkSurfaceKHR.");
        return;
    }
    const int presentDeviceIndex = findGraphicsPresentDeviceIndex(m_instance.get(), &probeWindow);
    if (presentDeviceIndex < 0) {
        m_failureReason = QStringLiteral("Direct Vulkan preview presenter found no graphics queue that can present to this platform surface.");
        return;
    }

    m_window = new DirectVulkanPreviewWindow(m_state, &m_presentedFrames, &m_active, &m_failureReason);
    m_window->setVulkanInstance(m_instance.get());
    m_window->setPhysicalDeviceIndex(presentDeviceIndex);
    m_window->resize(960, 540);
    m_window->show();

    m_placeholder = std::make_unique<QWidget>(parent);
    if (!m_placeholder) {
        m_failureReason = QStringLiteral("Failed to create placeholder widget for direct Vulkan preview presenter.");
        return;
    }
    m_placeholder->setStyleSheet(QStringLiteral("background:#05080c; border:1px solid #223041;"));
    m_placeholder->setToolTip(QStringLiteral("Direct Vulkan preview is presented in a native QVulkanWindow."));
    m_placeholder->setFocusPolicy(Qt::StrongFocus);
    m_placeholder->setMinimumSize(160, 120);
    auto* layout = new QVBoxLayout(m_placeholder.get());
    layout->setContentsMargins(18, 14, 18, 14);
    auto* label = new QLabel(QStringLiteral("Direct Vulkan preview is running in a native swapchain window."), m_placeholder.get());
    label->setWordWrap(true);
    label->setAlignment(Qt::AlignCenter);
    label->setStyleSheet(QStringLiteral("color:#d8e7f2; font-weight:600;"));
    layout->addWidget(label, 1);
    m_active = true;
    updateTitle();
}

DirectVulkanPreviewPresenter::~DirectVulkanPreviewPresenter()
{
    delete m_window;
    m_window = nullptr;
}

QWidget* DirectVulkanPreviewPresenter::widget() const
{
    return m_placeholder.get();
}

bool DirectVulkanPreviewPresenter::isActive() const
{
    return m_active;
}

QString DirectVulkanPreviewPresenter::failureReason() const
{
    return m_failureReason;
}

QString DirectVulkanPreviewPresenter::backendName() const
{
    return m_active
        ? QStringLiteral("Vulkan Preview (Direct Swapchain Presenter)")
        : QStringLiteral("Vulkan Preview Unavailable");
}

void DirectVulkanPreviewPresenter::requestUpdate()
{
    if (m_window) {
        m_window->requestUpdate();
    }
}

void DirectVulkanPreviewPresenter::updateTitle()
{
    if (!m_window || !m_state) {
        return;
    }
    m_window->setTitle(QStringLiteral("JCut Direct Vulkan Preview - frame %1 clips %2")
                           .arg(m_state->currentFrame)
                           .arg(m_state->clipCount));
}

QJsonObject DirectVulkanPreviewPresenter::profilingSnapshot() const
{
    int activeStatuses = 0;
    int readyStatuses = 0;
    int exactStatuses = 0;
    int hardwareStatuses = 0;
    int cpuStatuses = 0;
    if (m_state) {
        for (const VulkanPreviewClipFrameStatus& status : m_state->vulkanFrameStatuses) {
            if (!status.active) {
                continue;
            }
            ++activeStatuses;
            readyStatuses += status.hasFrame ? 1 : 0;
            exactStatuses += status.exact ? 1 : 0;
            hardwareStatuses += (status.hardwareFrame || status.gpuTexture) ? 1 : 0;
            cpuStatuses += status.cpuImage ? 1 : 0;
        }
    }
    return QJsonObject{
        {QStringLiteral("backend"), QStringLiteral("vulkan")},
        {QStringLiteral("presenter"), QStringLiteral("qvulkanwindow_direct_swapchain")},
        {QStringLiteral("composition_path"), QStringLiteral("direct_swapchain_commands_with_decode_status")},
        {QStringLiteral("swapchain_present"), m_active && m_window != nullptr},
        {QStringLiteral("native_window_visible"), m_window ? m_window->isVisible() : false},
        {QStringLiteral("native_active"), m_active},
        {QStringLiteral("qimage_bridge"), false},
        {QStringLiteral("qimage_materialized"), false},
        {QStringLiteral("vulkan_path_uses_qimage"), false},
        {QStringLiteral("materialized_frame_path"), false},
        {QStringLiteral("current_frame"), m_state ? static_cast<double>(m_state->currentFrame) : 0.0},
        {QStringLiteral("clip_count"), m_state ? m_state->clipCount : 0},
        {QStringLiteral("active_decode_status_clips"), activeStatuses},
        {QStringLiteral("ready_decode_status_clips"), readyStatuses},
        {QStringLiteral("exact_decode_status_clips"), exactStatuses},
        {QStringLiteral("hardware_decode_status_clips"), hardwareStatuses},
        {QStringLiteral("cpu_decode_status_clips"), cpuStatuses},
        {QStringLiteral("boxstream_overlay_boxes"), m_state ? m_state->boxstreamOverlays.size() : 0},
        {QStringLiteral("timeline_texture_composition"), false},
        {QStringLiteral("presented_frames"), static_cast<double>(m_presentedFrames)},
        {QStringLiteral("failure_reason"), m_failureReason}
    };
}

void DirectVulkanPreviewPresenter::resetProfilingStats()
{
    m_presentedFrames = 0;
}
