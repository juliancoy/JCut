#include "vulkan_detector_frame_handoff.h"
#include "vulkan_shader_paths.h"
#include "render_internal.h"

#include <QElapsedTimer>
#include <QFile>
#include <QScopeGuard>

#include <algorithm>
#include <cstring>
#include <unistd.h>
#include <vector>

#if !defined(JCUT_HAS_CUDA_DRIVER)
#define JCUT_HAS_CUDA_DRIVER 0
#endif

#if !defined(JCUT_SKIP_CUDA_EXTERNAL_MEMORY_DESTROY)
#define JCUT_SKIP_CUDA_EXTERNAL_MEMORY_DESTROY 0
#endif

extern "C" {
#include <libavutil/hwcontext.h>
#include <libavutil/pixfmt.h>
}

#if JCUT_HAS_CUDA_DRIVER
#include <libavutil/hwcontext_cuda.h>
#include <cuda.h>
#include <unistd.h>
#endif

namespace jcut::vulkan_detector {
namespace {

void setStdError(std::string* out, const QString& value)
{
    if (!out) {
        return;
    }
    *out = value.toStdString();
}

QString toQStringError(const std::string* value)
{
    if (!value) {
        return {};
    }
    return QString::fromStdString(*value);
}

jcut::core::SizeI toSizeI(const QSize& size)
{
    return {size.width(), size.height()};
}

QByteArray readShader(const QString& name)
{
    QFile file(QDir(jcutVulkanShaderDirectory()).filePath(name));
    if (!file.open(QIODevice::ReadOnly)) {
        return {};
    }
    return file.readAll();
}

VkShaderModule createShaderModule(VkDevice device, const QByteArray& spirv)
{
    if (device == VK_NULL_HANDLE || spirv.isEmpty()) {
        return VK_NULL_HANDLE;
    }
    VkShaderModuleCreateInfo info{};
    info.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
    info.codeSize = static_cast<size_t>(spirv.size());
    info.pCode = reinterpret_cast<const uint32_t*>(spirv.constData());
    VkShaderModule module = VK_NULL_HANDLE;
    if (vkCreateShaderModule(device, &info, nullptr, &module) != VK_SUCCESS) {
        return VK_NULL_HANDLE;
    }
    return module;
}

struct Nv12ColorConversion {
    int fullRange = 0;
    int colorMatrix = 1;
    int chromaSwap = 0;
};

QString nv12ColorConversionLabel(const Nv12ColorConversion& conversion)
{
    const QString matrix = conversion.colorMatrix == 0
        ? QStringLiteral("bt601")
        : (conversion.colorMatrix == 2
               ? QStringLiteral("bt2020_ncl")
               : QStringLiteral("bt709"));
    const QString range = conversion.fullRange != 0
        ? QStringLiteral("full")
        : QStringLiteral("limited");
    const QString chroma = conversion.chromaSwap != 0
        ? QStringLiteral("vu")
        : QStringLiteral("uv");
    return QStringLiteral("%1_%2_%3").arg(matrix, range, chroma);
}

Nv12ColorConversion nv12ColorConversionForFrame(const editor::FrameHandle& frame)
{
    Nv12ColorConversion conversion;
    const AVFrame* avFrame = frame.hardwareFrame();
    if (!avFrame) {
        return conversion;
    }

    // FFmpeg's JPEG value means full/PC range for every codec, not just JPEG.
    // Unspecified range follows the professional video default: limited range.
    conversion.fullRange = avFrame->color_range == AVCOL_RANGE_JPEG ? 1 : 0;
    switch (avFrame->colorspace) {
        case AVCOL_SPC_SMPTE170M:
        case AVCOL_SPC_BT470BG:
        case AVCOL_SPC_FCC:
            conversion.colorMatrix = 0;
            break;
        case AVCOL_SPC_BT2020_NCL:
        case AVCOL_SPC_BT2020_CL:
            conversion.colorMatrix = 2;
            break;
        case AVCOL_SPC_BT709:
            conversion.colorMatrix = 1;
            break;
        case AVCOL_SPC_UNSPECIFIED:
        default:
            // ITU/FFmpeg convention for untagged material: SD uses BT.601;
            // HD and UHD use BT.709 unless explicitly tagged as BT.2020.
            conversion.colorMatrix = avFrame->height > 576 || avFrame->width >= 1280 ? 1 : 0;
            break;
    }
    return conversion;
}

#if JCUT_HAS_CUDA_DRIVER
    void destroyCudaExternalMemory(void*& memory, quint64& devicePtr, void* context)
    {
        if (!memory) {
            devicePtr = 0;
            return;
        }
#if JCUT_SKIP_CUDA_EXTERNAL_MEMORY_DESTROY
        memory = nullptr;
        devicePtr = 0;
        Q_UNUSED(context);
        return;
#else
        CUcontext previous = nullptr;
        if (context) {
            cuCtxPushCurrent(reinterpret_cast<CUcontext>(context));
        }
        cuDestroyExternalMemory(reinterpret_cast<CUexternalMemory>(memory));
    if (context) {
        cuCtxPopCurrent(&previous);
        }
        memory = nullptr;
        devicePtr = 0;
#endif
    }

void destroyCudaExternalSemaphore(void*& semaphore, void* context)
{
    if (!semaphore) {
        return;
    }
    CUcontext previous = nullptr;
    if (context) {
        cuCtxPushCurrent(reinterpret_cast<CUcontext>(context));
    }
    cuDestroyExternalSemaphore(reinterpret_cast<CUexternalSemaphore>(semaphore));
    if (context) {
        cuCtxPopCurrent(&previous);
    }
    semaphore = nullptr;
}

CUresult synchronizeCudaStream(CUstream stream, FrameHandoffResourceStats* stats)
{
    QElapsedTimer timer;
    if (stats) {
        timer.start();
    }
    const CUresult result = cuStreamSynchronize(stream);
    if (stats) {
        ++stats->cudaStreamSynchronizeCalls;
        stats->cudaStreamSynchronizeMs +=
            static_cast<double>(timer.nsecsElapsed()) / 1'000'000.0;
    }
    return result;
}

CUresult signalCudaExternalSemaphore(CUexternalSemaphore semaphore,
                                     CUstream stream,
                                     FrameHandoffResourceStats* stats)
{
    CUDA_EXTERNAL_SEMAPHORE_SIGNAL_PARAMS params{};
    QElapsedTimer timer;
    if (stats) {
        timer.start();
    }
    CUexternalSemaphore semaphores[] = {semaphore};
    const CUresult result =
        cuSignalExternalSemaphoresAsync(semaphores, &params, 1, stream);
    if (stats) {
        ++stats->cudaExternalSemaphoreSignals;
        stats->cudaExternalSemaphoreSignalMs +=
            static_cast<double>(timer.nsecsElapsed()) / 1'000'000.0;
    }
    return result;
}
#endif

} // namespace

VulkanDetectorFrameHandoff::VulkanDetectorFrameHandoff() = default;

VulkanDetectorFrameHandoff::~VulkanDetectorFrameHandoff()
{
    release();
}

void VulkanDetectorFrameHandoff::resetResourceStats()
{
    m_resourceStats = {};
    m_externalFrameImporter.resetResourceStats();
}

bool VulkanDetectorFrameHandoff::initialize(const VulkanDeviceContext& context, std::string* errorMessage)
{
    release();
    if (context.physicalDevice == VK_NULL_HANDLE ||
        context.device == VK_NULL_HANDLE ||
        context.queue == VK_NULL_HANDLE ||
        context.queueFamilyIndex == UINT32_MAX) {
        setStdError(errorMessage, QStringLiteral("Invalid Vulkan device context for frame handoff."));
        return false;
    }
    m_context = context;

    VkCommandPoolCreateInfo poolInfo{};
    poolInfo.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
    poolInfo.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    poolInfo.queueFamilyIndex = m_context.queueFamilyIndex;
    if (vkCreateCommandPool(m_context.device, &poolInfo, nullptr, &m_commandPool) != VK_SUCCESS) {
        setStdError(errorMessage, QStringLiteral("Failed to create frame handoff command pool."));
        release();
        return false;
    }

    VkCommandBufferAllocateInfo allocInfo{};
    allocInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    allocInfo.commandPool = m_commandPool;
    allocInfo.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    allocInfo.commandBufferCount = 1;
    if (vkAllocateCommandBuffers(m_context.device, &allocInfo, &m_commandBuffer) != VK_SUCCESS) {
        setStdError(errorMessage, QStringLiteral("Failed to allocate frame handoff command buffer."));
        release();
        return false;
    }

    VkFenceCreateInfo fenceInfo{};
    fenceInfo.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
    if (vkCreateFence(m_context.device, &fenceInfo, nullptr, &m_fence) != VK_SUCCESS) {
        setStdError(errorMessage, QStringLiteral("Failed to create frame handoff fence."));
        release();
        return false;
    }

    if (!m_externalFrameImporter.initialize(
            jcut::vulkan_import::DeviceContext{
                context.physicalDevice,
                context.device,
                context.queue,
                context.queueFamilyIndex},
            errorMessage)) {
        release();
        return false;
    }

    m_initialized = true;
    return true;
}

void VulkanDetectorFrameHandoff::release()
{
    finishPendingUpload(nullptr, nullptr);
    m_externalFrameImporter.release();
    if (m_context.device != VK_NULL_HANDLE) {
        vkDeviceWaitIdle(m_context.device);
#if JCUT_HAS_CUDA_DRIVER
        destroyCudaExternalMemory(m_cudaExternalMemory, m_cudaExternalDevicePtr, m_cudaImportContext);
        destroyCudaExternalMemory(m_cudaExternalUvMemory, m_cudaExternalUvDevicePtr, m_cudaImportContext);
        destroyCudaExternalSemaphore(m_cudaExternalReadySemaphore, m_cudaSemaphoreImportContext);
#endif
        m_cudaImportContext = nullptr;
        m_cudaSemaphoreImportContext = nullptr;
        destroyBuffer(m_cudaExportBuffer, m_cudaExportMemory, &m_resourceStats.stagingBufferFrees);
        destroyBuffer(m_cudaExportUvBuffer, m_cudaExportUvMemory, &m_resourceStats.stagingBufferFrees);
        destroyRetiredCudaExportBuffers();
        m_cudaExportSize = 0;
        m_cudaExportUvSize = 0;
        if (m_nv12Pipeline != VK_NULL_HANDLE) vkDestroyPipeline(m_context.device, m_nv12Pipeline, nullptr);
        if (m_nv12PipelineLayout != VK_NULL_HANDLE) vkDestroyPipelineLayout(m_context.device, m_nv12PipelineLayout, nullptr);
        if (m_reusableNv12DescriptorSet != VK_NULL_HANDLE &&
            m_nv12DescriptorPool != VK_NULL_HANDLE) {
            vkFreeDescriptorSets(m_context.device, m_nv12DescriptorPool, 1, &m_reusableNv12DescriptorSet);
            ++m_resourceStats.descriptorFrees;
            m_reusableNv12DescriptorSet = VK_NULL_HANDLE;
        }
        if (m_nv12DescriptorPool != VK_NULL_HANDLE) vkDestroyDescriptorPool(m_context.device, m_nv12DescriptorPool, nullptr);
        if (m_nv12DescriptorSetLayout != VK_NULL_HANDLE) vkDestroyDescriptorSetLayout(m_context.device, m_nv12DescriptorSetLayout, nullptr);
        if (m_imageView != VK_NULL_HANDLE) vkDestroyImageView(m_context.device, m_imageView, nullptr);
        if (m_image != VK_NULL_HANDLE) vkDestroyImage(m_context.device, m_image, nullptr);
        if (m_imageMemory != VK_NULL_HANDLE) {
            vkFreeMemory(m_context.device, m_imageMemory, nullptr);
            ++m_resourceStats.imageMemoryFrees;
        }
        destroyBuffer(m_stagingBuffer, m_stagingMemory, &m_resourceStats.stagingBufferFrees);
        if (m_cudaReadySemaphore != VK_NULL_HANDLE) vkDestroySemaphore(m_context.device, m_cudaReadySemaphore, nullptr);
        if (m_fence != VK_NULL_HANDLE) vkDestroyFence(m_context.device, m_fence, nullptr);
        if (m_commandPool != VK_NULL_HANDLE) vkDestroyCommandPool(m_context.device, m_commandPool, nullptr);
    }
    m_commandBuffer = VK_NULL_HANDLE;
    m_commandPool = VK_NULL_HANDLE;
    m_fence = VK_NULL_HANDLE;
    m_image = VK_NULL_HANDLE;
    m_imageMemory = VK_NULL_HANDLE;
    m_imageView = VK_NULL_HANDLE;
    m_imageLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    m_imageFormat = VK_FORMAT_UNDEFINED;
    m_imageSize = {};
    m_stagingSize = 0;
    m_cudaExportSize = 0;
    m_cudaExportUvSize = 0;
    m_retiredCudaExportBuffers.clear();
    m_cudaExternalMemory = nullptr;
    m_cudaExternalUvMemory = nullptr;
    m_cudaExternalReadySemaphore = nullptr;
    m_cudaExternalDevicePtr = 0;
    m_cudaExternalUvDevicePtr = 0;
    m_cudaImportContext = nullptr;
    m_cudaSemaphoreImportContext = nullptr;
    m_cudaReadySemaphore = VK_NULL_HANDLE;
    m_nv12DescriptorSetLayout = VK_NULL_HANDLE;
    m_nv12DescriptorPool = VK_NULL_HANDLE;
    m_nv12PipelineLayout = VK_NULL_HANDLE;
    m_nv12Pipeline = VK_NULL_HANDLE;
    m_pendingNv12DescriptorSet = VK_NULL_HANDLE;
    m_reusableNv12DescriptorSet = VK_NULL_HANDLE;
    m_uploadPending = false;
    m_initialized = false;
    m_lastMode = FrameHandoffMode::Invalid;
    m_context = VulkanDeviceContext{};
}

bool VulkanDetectorFrameHandoff::ensureImageResources(const jcut::core::SizeI& size,
                                                      VkFormat format,
                                                      QString* errorMessage)
{
    if (m_image != VK_NULL_HANDLE && m_imageMemory != VK_NULL_HANDLE &&
        m_imageView != VK_NULL_HANDLE && m_imageSize == size &&
        m_imageFormat == format) {
        return true;
    }
    if (m_imageView != VK_NULL_HANDLE) vkDestroyImageView(m_context.device, m_imageView, nullptr);
    if (m_image != VK_NULL_HANDLE) vkDestroyImage(m_context.device, m_image, nullptr);
    if (m_imageMemory != VK_NULL_HANDLE) {
        vkFreeMemory(m_context.device, m_imageMemory, nullptr);
        ++m_resourceStats.imageMemoryFrees;
    }
    m_image = VK_NULL_HANDLE;
    m_imageMemory = VK_NULL_HANDLE;
    m_imageView = VK_NULL_HANDLE;
    m_imageLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    m_imageSize = size;
    m_imageFormat = format;

    VkImageCreateInfo imageInfo{};
    imageInfo.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    imageInfo.imageType = VK_IMAGE_TYPE_2D;
    imageInfo.extent = {static_cast<uint32_t>(size.width), static_cast<uint32_t>(size.height), 1};
    imageInfo.mipLevels = 1;
    imageInfo.arrayLayers = 1;
    imageInfo.format = format;
    imageInfo.tiling = VK_IMAGE_TILING_OPTIMAL;
    imageInfo.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    imageInfo.usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT |
                      VK_IMAGE_USAGE_TRANSFER_SRC_BIT |
                      VK_IMAGE_USAGE_STORAGE_BIT |
                      VK_IMAGE_USAGE_SAMPLED_BIT;
    imageInfo.samples = VK_SAMPLE_COUNT_1_BIT;
    imageInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    if (vkCreateImage(m_context.device, &imageInfo, nullptr, &m_image) != VK_SUCCESS) {
        if (errorMessage) *errorMessage = QStringLiteral("Failed to create Vulkan handoff image.");
        return false;
    }
    VkMemoryRequirements req{};
    vkGetImageMemoryRequirements(m_context.device, m_image, &req);
    const uint32_t type = findMemoryType(req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    if (type == UINT32_MAX) {
        if (errorMessage) *errorMessage = QStringLiteral("No Vulkan handoff image memory type.");
        return false;
    }
    VkMemoryAllocateInfo alloc{};
    alloc.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    alloc.allocationSize = req.size;
    alloc.memoryTypeIndex = type;
    if (vkAllocateMemory(m_context.device, &alloc, nullptr, &m_imageMemory) != VK_SUCCESS ||
        vkBindImageMemory(m_context.device, m_image, m_imageMemory, 0) != VK_SUCCESS) {
        if (errorMessage) *errorMessage = QStringLiteral("Failed to allocate/bind Vulkan handoff image memory.");
        return false;
    }
    ++m_resourceStats.imageMemoryAllocations;
    VkImageViewCreateInfo viewInfo{};
    viewInfo.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    viewInfo.image = m_image;
    viewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
    viewInfo.format = format;
    viewInfo.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    viewInfo.subresourceRange.levelCount = 1;
    viewInfo.subresourceRange.layerCount = 1;
    if (vkCreateImageView(m_context.device, &viewInfo, nullptr, &m_imageView) != VK_SUCCESS) {
        if (errorMessage) *errorMessage = QStringLiteral("Failed to create Vulkan handoff image view.");
        return false;
    }
    return true;
}

bool VulkanDetectorFrameHandoff::ensureStagingBuffer(VkDeviceSize bytes, QString* errorMessage)
{
    if (bytes <= m_stagingSize && m_stagingBuffer != VK_NULL_HANDLE && m_stagingMemory != VK_NULL_HANDLE) {
        return true;
    }
    destroyBuffer(m_stagingBuffer, m_stagingMemory, &m_resourceStats.stagingBufferFrees);
    VkBufferCreateInfo info{};
    info.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    info.size = bytes;
    info.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
    info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    if (vkCreateBuffer(m_context.device, &info, nullptr, &m_stagingBuffer) != VK_SUCCESS) {
        if (errorMessage) *errorMessage = QStringLiteral("Failed to create Vulkan handoff staging buffer.");
        return false;
    }
    VkMemoryRequirements req{};
    vkGetBufferMemoryRequirements(m_context.device, m_stagingBuffer, &req);
    const uint32_t type = findMemoryType(
        req.memoryTypeBits,
        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    if (type == UINT32_MAX) {
        if (errorMessage) *errorMessage = QStringLiteral("No Vulkan handoff staging memory type.");
        return false;
    }
    VkMemoryAllocateInfo alloc{};
    alloc.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    alloc.allocationSize = req.size;
    alloc.memoryTypeIndex = type;
    if (vkAllocateMemory(m_context.device, &alloc, nullptr, &m_stagingMemory) != VK_SUCCESS ||
        vkBindBufferMemory(m_context.device, m_stagingBuffer, m_stagingMemory, 0) != VK_SUCCESS) {
        if (errorMessage) *errorMessage = QStringLiteral("Failed to allocate/bind Vulkan handoff staging memory.");
        return false;
    }
    ++m_resourceStats.stagingBufferAllocations;
    m_stagingSize = bytes;
    return true;
}

bool VulkanDetectorFrameHandoff::ensureCudaExportBuffer(VkDeviceSize bytes,
                                                        VkBuffer& buffer,
                                                        VkDeviceMemory& memory,
                                                        VkDeviceSize& size,
                                                        QString* errorMessage)
{
    if (buffer != VK_NULL_HANDLE && memory != VK_NULL_HANDLE && size >= bytes) {
        return true;
    }

    VkExternalMemoryBufferCreateInfo extBuf{};
    extBuf.sType = VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_BUFFER_CREATE_INFO;
    extBuf.handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_FD_BIT;
    VkBufferCreateInfo info{};
    info.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    info.pNext = &extBuf;
    info.size = bytes;
    info.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_STORAGE_BUFFER_BIT;
    info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    if (vkCreateBuffer(m_context.device, &info, nullptr, &buffer) != VK_SUCCESS) {
        if (errorMessage) *errorMessage = QStringLiteral("failed to create CUDA-export Vulkan buffer");
        return false;
    }
    VkMemoryRequirements req{};
    vkGetBufferMemoryRequirements(m_context.device, buffer, &req);
    const uint32_t memType = findMemoryType(req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    if (memType == UINT32_MAX) {
        destroyBuffer(buffer, memory, nullptr);
        size = 0;
        if (errorMessage) *errorMessage = QStringLiteral("no device-local memory for CUDA-export Vulkan buffer");
        return false;
    }
    VkExportMemoryAllocateInfo exportAllocInfo{};
    exportAllocInfo.sType = VK_STRUCTURE_TYPE_EXPORT_MEMORY_ALLOCATE_INFO;
    exportAllocInfo.handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_FD_BIT;
    VkMemoryAllocateInfo alloc{};
    alloc.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    alloc.pNext = &exportAllocInfo;
    alloc.allocationSize = req.size;
    alloc.memoryTypeIndex = memType;
    if (vkAllocateMemory(m_context.device, &alloc, nullptr, &memory) != VK_SUCCESS ||
        vkBindBufferMemory(m_context.device, buffer, memory, 0) != VK_SUCCESS) {
        destroyBuffer(buffer, memory, nullptr);
        size = 0;
        if (errorMessage) *errorMessage = QStringLiteral("failed to allocate/bind CUDA-export Vulkan memory");
        return false;
    }
    ++m_resourceStats.stagingBufferAllocations;
    size = req.size;
    return true;
}

bool VulkanDetectorFrameHandoff::createNv12ConversionPipeline(QString* errorMessage)
{
    if (m_nv12Pipeline != VK_NULL_HANDLE) {
        return true;
    }

    std::array<VkDescriptorSetLayoutBinding, 3> bindings{};
    bindings[0].binding = 0;
    bindings[0].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    bindings[0].descriptorCount = 1;
    bindings[0].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
    bindings[1] = bindings[0];
    bindings[1].binding = 1;
    bindings[2].binding = 2;
    bindings[2].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
    bindings[2].descriptorCount = 1;
    bindings[2].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;

    VkDescriptorSetLayoutCreateInfo layoutInfo{};
    layoutInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    layoutInfo.bindingCount = static_cast<uint32_t>(bindings.size());
    layoutInfo.pBindings = bindings.data();
    if (vkCreateDescriptorSetLayout(m_context.device, &layoutInfo, nullptr, &m_nv12DescriptorSetLayout) != VK_SUCCESS) {
        if (errorMessage) *errorMessage = QStringLiteral("failed to create NV12 conversion descriptor layout");
        return false;
    }

    std::array<VkDescriptorPoolSize, 2> poolSizes{};
    poolSizes[0].type = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    poolSizes[0].descriptorCount = 8;
    poolSizes[1].type = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
    poolSizes[1].descriptorCount = 4;
    VkDescriptorPoolCreateInfo poolInfo{};
    poolInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    poolInfo.flags = VK_DESCRIPTOR_POOL_CREATE_FREE_DESCRIPTOR_SET_BIT;
    poolInfo.maxSets = 4;
    poolInfo.poolSizeCount = static_cast<uint32_t>(poolSizes.size());
    poolInfo.pPoolSizes = poolSizes.data();
    if (vkCreateDescriptorPool(m_context.device, &poolInfo, nullptr, &m_nv12DescriptorPool) != VK_SUCCESS) {
        if (errorMessage) *errorMessage = QStringLiteral("failed to create NV12 conversion descriptor pool");
        return false;
    }

    struct Push {
        int width;
        int height;
        int yPitch;
        int uvPitch;
        int fullRange;
        int colorMatrix;
        int chromaSwap;
    };
    VkPushConstantRange pushRange{};
    pushRange.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
    pushRange.size = sizeof(Push);

    VkPipelineLayoutCreateInfo pipelineLayoutInfo{};
    pipelineLayoutInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    pipelineLayoutInfo.setLayoutCount = 1;
    pipelineLayoutInfo.pSetLayouts = &m_nv12DescriptorSetLayout;
    pipelineLayoutInfo.pushConstantRangeCount = 1;
    pipelineLayoutInfo.pPushConstantRanges = &pushRange;
    if (vkCreatePipelineLayout(m_context.device, &pipelineLayoutInfo, nullptr, &m_nv12PipelineLayout) != VK_SUCCESS) {
        if (errorMessage) *errorMessage = QStringLiteral("failed to create NV12 conversion pipeline layout");
        return false;
    }

    VkShaderModule shader = createShaderModule(m_context.device, readShader(QStringLiteral("nv12_buffer_to_rgba.comp.spv")));
    if (shader == VK_NULL_HANDLE) {
        if (errorMessage) *errorMessage = QStringLiteral("failed to load nv12_buffer_to_rgba.comp.spv");
        return false;
    }
    VkPipelineShaderStageCreateInfo stage{};
    stage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
    stage.module = shader;
    stage.pName = "main";
    VkComputePipelineCreateInfo pipelineInfo{};
    pipelineInfo.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO;
    pipelineInfo.stage = stage;
    pipelineInfo.layout = m_nv12PipelineLayout;
    const VkResult result = vkCreateComputePipelines(m_context.device, VK_NULL_HANDLE, 1, &pipelineInfo, nullptr, &m_nv12Pipeline);
    vkDestroyShaderModule(m_context.device, shader, nullptr);
    if (result != VK_SUCCESS) {
        if (errorMessage) *errorMessage = QStringLiteral("failed to create NV12 conversion compute pipeline");
        return false;
    }
    ++m_resourceStats.computePipelineCreations;
    return true;
}

bool VulkanDetectorFrameHandoff::ensureNv12ConversionResources(QString* errorMessage)
{
    return createNv12ConversionPipeline(errorMessage);
}

bool VulkanDetectorFrameHandoff::ensureCudaReadySemaphore(void* cudaContext, QString* errorMessage)
{
#if !JCUT_HAS_CUDA_DRIVER
    Q_UNUSED(cudaContext)
    if (errorMessage) *errorMessage = QStringLiteral("CUDA driver interop is not compiled in (JCUT_HAS_CUDA_DRIVER=0)");
    return false;
#else
    if (!cudaContext) {
        if (errorMessage) *errorMessage = QStringLiteral("missing CUDA context for external semaphore handoff");
        return false;
    }
    PFN_vkGetSemaphoreFdKHR vkGetSemaphoreFdKHR =
        reinterpret_cast<PFN_vkGetSemaphoreFdKHR>(
            vkGetDeviceProcAddr(m_context.device, "vkGetSemaphoreFdKHR"));
    if (!vkGetSemaphoreFdKHR) {
        if (errorMessage) *errorMessage = QStringLiteral("vkGetSemaphoreFdKHR unavailable for CUDA/Vulkan semaphore handoff");
        return false;
    }
    if (m_cudaReadySemaphore == VK_NULL_HANDLE) {
        VkExportSemaphoreCreateInfo exportInfo{};
        exportInfo.sType = VK_STRUCTURE_TYPE_EXPORT_SEMAPHORE_CREATE_INFO;
        exportInfo.handleTypes = VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_OPAQUE_FD_BIT;
        VkSemaphoreCreateInfo createInfo{};
        createInfo.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;
        createInfo.pNext = &exportInfo;
        if (vkCreateSemaphore(m_context.device, &createInfo, nullptr, &m_cudaReadySemaphore) != VK_SUCCESS) {
            if (errorMessage) *errorMessage = QStringLiteral("failed to create exportable CUDA/Vulkan semaphore");
            return false;
        }
    }
    if (m_cudaExternalReadySemaphore && m_cudaSemaphoreImportContext == cudaContext) {
        return true;
    }
    if (m_cudaExternalReadySemaphore) {
        destroyCudaExternalSemaphore(m_cudaExternalReadySemaphore, m_cudaSemaphoreImportContext);
        m_cudaSemaphoreImportContext = nullptr;
    }
    VkSemaphoreGetFdInfoKHR fdInfo{};
    fdInfo.sType = VK_STRUCTURE_TYPE_SEMAPHORE_GET_FD_INFO_KHR;
    fdInfo.semaphore = m_cudaReadySemaphore;
    fdInfo.handleType = VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_OPAQUE_FD_BIT;
    int fd = -1;
    if (vkGetSemaphoreFdKHR(m_context.device, &fdInfo, &fd) != VK_SUCCESS || fd < 0) {
        if (errorMessage) *errorMessage = QStringLiteral("failed to export Vulkan semaphore FD for CUDA import");
        return false;
    }
    CUcontext previous = nullptr;
    if (cuCtxPushCurrent(reinterpret_cast<CUcontext>(cudaContext)) != CUDA_SUCCESS) {
        close(fd);
        if (errorMessage) *errorMessage = QStringLiteral("failed to activate CUDA context for semaphore import");
        return false;
    }
    auto pop = qScopeGuard([&]() { cuCtxPopCurrent(&previous); });
    CUDA_EXTERNAL_SEMAPHORE_HANDLE_DESC desc{};
    desc.type = CU_EXTERNAL_SEMAPHORE_HANDLE_TYPE_OPAQUE_FD;
    desc.handle.fd = fd;
    CUexternalSemaphore semaphore = nullptr;
    if (cuImportExternalSemaphore(&semaphore, &desc) != CUDA_SUCCESS) {
        close(fd);
        if (errorMessage) *errorMessage = QStringLiteral("cuImportExternalSemaphore failed for CUDA/Vulkan handoff");
        return false;
    }
    m_cudaExternalReadySemaphore = semaphore;
    m_cudaSemaphoreImportContext = cudaContext;
    return true;
#endif
}

bool VulkanDetectorFrameHandoff::signalCudaReadySemaphore(void* cudaStream, QString* errorMessage)
{
#if !JCUT_HAS_CUDA_DRIVER
    Q_UNUSED(cudaStream)
    if (errorMessage) *errorMessage = QStringLiteral("CUDA driver interop is not compiled in (JCUT_HAS_CUDA_DRIVER=0)");
    return false;
#else
    if (!m_cudaExternalReadySemaphore) {
        if (errorMessage) *errorMessage = QStringLiteral("CUDA/Vulkan semaphore handoff is not initialized");
        return false;
    }
    if (signalCudaExternalSemaphore(
            reinterpret_cast<CUexternalSemaphore>(m_cudaExternalReadySemaphore),
            reinterpret_cast<CUstream>(cudaStream),
            &m_resourceStats) != CUDA_SUCCESS) {
        if (errorMessage) *errorMessage = QStringLiteral("CUDA failed to signal Vulkan handoff semaphore");
        return false;
    }
    return true;
#endif
}

bool VulkanDetectorFrameHandoff::submitCommandBufferWaitingOnCuda(VkPipelineStageFlags waitStage,
                                                                  QString* errorMessage)
{
    VkSubmitInfo submit{};
    submit.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    if (m_cudaReadySemaphore != VK_NULL_HANDLE) {
        submit.waitSemaphoreCount = 1;
        submit.pWaitSemaphores = &m_cudaReadySemaphore;
        submit.pWaitDstStageMask = &waitStage;
    }
    submit.commandBufferCount = 1;
    submit.pCommandBuffers = &m_commandBuffer;
    if (vkQueueSubmit(m_context.queue, 1, &submit, m_fence) != VK_SUCCESS) {
        if (errorMessage) *errorMessage = QStringLiteral("Vulkan submit/wait failed for CUDA/Vulkan handoff");
        return false;
    }
    return true;
}

bool VulkanDetectorFrameHandoff::convertNv12BuffersToImage(int width,
                                                           int height,
                                                           int yPitch,
                                                           int uvPitch,
                                                           int fullRange,
                                                           int colorMatrix,
                                                           int chromaSwap,
                                                           QString* errorMessage)
{
    std::string finishError;
    if (!finishPendingUpload(nullptr, &finishError)) {
        if (errorMessage) {
            *errorMessage = QString::fromStdString(finishError);
        }
        return false;
    }
    if (!ensureNv12ConversionResources(errorMessage)) {
        return false;
    }
    if (m_reusableNv12DescriptorSet == VK_NULL_HANDLE) {
        VkDescriptorSetAllocateInfo allocInfo{};
        allocInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
        allocInfo.descriptorPool = m_nv12DescriptorPool;
        allocInfo.descriptorSetCount = 1;
        allocInfo.pSetLayouts = &m_nv12DescriptorSetLayout;
        if (vkAllocateDescriptorSets(m_context.device, &allocInfo, &m_reusableNv12DescriptorSet) != VK_SUCCESS) {
            if (errorMessage) *errorMessage = QStringLiteral("failed to allocate NV12 conversion descriptor set");
            return false;
        }
        ++m_resourceStats.descriptorAllocations;
    }
    VkDescriptorSet set = m_reusableNv12DescriptorSet;

    VkDescriptorBufferInfo yInfo{};
    yInfo.buffer = m_cudaExportBuffer;
    yInfo.range = m_cudaExportSize;
    VkDescriptorBufferInfo uvInfo{};
    uvInfo.buffer = m_cudaExportUvBuffer;
    uvInfo.range = m_cudaExportUvSize;
    VkDescriptorImageInfo imageInfo{};
    imageInfo.imageView = m_imageView;
    imageInfo.imageLayout = VK_IMAGE_LAYOUT_GENERAL;

    std::array<VkWriteDescriptorSet, 3> writes{};
    writes[0].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    writes[0].dstSet = set;
    writes[0].dstBinding = 0;
    writes[0].descriptorCount = 1;
    writes[0].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    writes[0].pBufferInfo = &yInfo;
    writes[1] = writes[0];
    writes[1].dstBinding = 1;
    writes[1].pBufferInfo = &uvInfo;
    writes[2].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    writes[2].dstSet = set;
    writes[2].dstBinding = 2;
    writes[2].descriptorCount = 1;
    writes[2].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
    writes[2].pImageInfo = &imageInfo;
    vkUpdateDescriptorSets(m_context.device, static_cast<uint32_t>(writes.size()), writes.data(), 0, nullptr);

    vkResetFences(m_context.device, 1, &m_fence);
    vkResetCommandBuffer(m_commandBuffer, 0);
    VkCommandBufferBeginInfo begin{};
    begin.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    if (vkBeginCommandBuffer(m_commandBuffer, &begin) != VK_SUCCESS) {
        if (errorMessage) *errorMessage = QStringLiteral("vkBeginCommandBuffer failed for NV12 conversion");
        return false;
    }

    std::array<VkBufferMemoryBarrier, 2> cudaWriteBarriers{};
    cudaWriteBarriers[0].sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER;
    cudaWriteBarriers[0].srcAccessMask = VK_ACCESS_MEMORY_WRITE_BIT;
    cudaWriteBarriers[0].dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
    cudaWriteBarriers[0].srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    cudaWriteBarriers[0].dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    cudaWriteBarriers[0].buffer = m_cudaExportBuffer;
    cudaWriteBarriers[0].offset = 0;
    cudaWriteBarriers[0].size = VK_WHOLE_SIZE;
    cudaWriteBarriers[1] = cudaWriteBarriers[0];
    cudaWriteBarriers[1].buffer = m_cudaExportUvBuffer;
    vkCmdPipelineBarrier(m_commandBuffer,
                         VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
                         VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                         0,
                         0,
                         nullptr,
                         static_cast<uint32_t>(cudaWriteBarriers.size()),
                         cudaWriteBarriers.data(),
                         0,
                         nullptr);

    transitionImage(m_imageLayout, VK_IMAGE_LAYOUT_GENERAL);
    struct Push {
        int width;
        int height;
        int yPitch;
        int uvPitch;
        int fullRange;
        int colorMatrix;
        int chromaSwap;
    } push{width, height, yPitch, uvPitch, fullRange, colorMatrix, chromaSwap};
    vkCmdBindPipeline(m_commandBuffer, VK_PIPELINE_BIND_POINT_COMPUTE, m_nv12Pipeline);
    vkCmdBindDescriptorSets(m_commandBuffer,
                            VK_PIPELINE_BIND_POINT_COMPUTE,
                            m_nv12PipelineLayout,
                            0,
                            1,
                            &set,
                            0,
                            nullptr);
    vkCmdPushConstants(m_commandBuffer,
                       m_nv12PipelineLayout,
                       VK_SHADER_STAGE_COMPUTE_BIT,
                       0,
                       sizeof(push),
                       &push);
    vkCmdDispatch(m_commandBuffer,
                  static_cast<uint32_t>((width + 15) / 16),
                  static_cast<uint32_t>((height + 15) / 16),
                  1);

    VkImageMemoryBarrier barrier{};
    barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    barrier.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
    barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
    barrier.oldLayout = VK_IMAGE_LAYOUT_GENERAL;
    barrier.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.image = m_image;
    barrier.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    barrier.subresourceRange.levelCount = 1;
    barrier.subresourceRange.layerCount = 1;
    vkCmdPipelineBarrier(m_commandBuffer,
                         VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                         VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
                         0,
                         0,
                         nullptr,
                         0,
                         nullptr,
                         1,
                         &barrier);
    if (vkEndCommandBuffer(m_commandBuffer) != VK_SUCCESS) {
        if (errorMessage) *errorMessage = QStringLiteral("vkEndCommandBuffer failed for NV12 conversion");
        return false;
    }
    if (!submitCommandBufferWaitingOnCuda(VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, errorMessage)) {
        return false;
    }
    m_imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    m_pendingNv12DescriptorSet = set;
    m_uploadPending = true;
    m_pendingUploadTimer.start();
    return true;
}

void VulkanDetectorFrameHandoff::transitionImage(VkImageLayout oldLayout, VkImageLayout newLayout)
{
    transitionImage(m_commandBuffer, oldLayout, newLayout);
}

void VulkanDetectorFrameHandoff::transitionImage(VkCommandBuffer commandBuffer,
                                                 VkImageLayout oldLayout,
                                                 VkImageLayout newLayout)
{
    VkImageMemoryBarrier barrier{};
    barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    barrier.oldLayout = oldLayout;
    barrier.newLayout = newLayout;
    barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.image = m_image;
    barrier.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    barrier.subresourceRange.levelCount = 1;
    barrier.subresourceRange.layerCount = 1;
    VkPipelineStageFlags src = VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT;
    VkPipelineStageFlags dst = VK_PIPELINE_STAGE_TRANSFER_BIT;
    if (oldLayout == VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL) {
        barrier.srcAccessMask = VK_ACCESS_SHADER_READ_BIT;
        src = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
    } else if (oldLayout == VK_IMAGE_LAYOUT_GENERAL) {
        barrier.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
        src = VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT;
    } else if (oldLayout == VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL) {
        barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        src = VK_PIPELINE_STAGE_TRANSFER_BIT;
    }
    if (newLayout == VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL) {
        barrier.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        dst = VK_PIPELINE_STAGE_TRANSFER_BIT;
    } else if (newLayout == VK_IMAGE_LAYOUT_GENERAL) {
        barrier.dstAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
        dst = VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT;
    } else if (newLayout == VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL) {
        barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
        dst = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
    }
    vkCmdPipelineBarrier(commandBuffer, src, dst, 0, 0, nullptr, 0, nullptr, 1, &barrier);
}

bool VulkanDetectorFrameHandoff::uploadFrame(const editor::FrameHandle& frame,
                                             bool allowCpuUploadFallback,
                                             double* uploadMs,
                                             std::string* errorMessage)
{
    QString qtError;
    m_lastYuvRgbMatrix.clear();
    if (!m_initialized || frame.isNull()) {
        setStdError(errorMessage, QStringLiteral("Invalid frame handoff upload state."));
        return false;
    }
    m_lastHardwareDirectAttemptReason.clear();
    m_lastProbe = probeHardwareInterop(frame);
    if (m_lastProbe.supported && frame.hasHardwareFrame()) {
        QString directError;
        if (tryHardwareDirect(frame, uploadMs, &directError)) {
            m_lastMode = FrameHandoffMode::HardwareDirect;
            return true;
        }
        m_lastHardwareDirectAttemptReason = directError;
        if (errorMessage && !directError.isEmpty()) {
            setStdError(errorMessage,
                        QStringLiteral("Hardware direct handoff attempt failed: %1").arg(directError));
        } else if (errorMessage) {
            setStdError(errorMessage, QStringLiteral("Hardware direct handoff attempt failed."));
        }
        if (!allowCpuUploadFallback) {
            m_lastMode = FrameHandoffMode::Invalid;
            return false;
        }
    }
    if (!allowCpuUploadFallback) {
        if (errorMessage) {
            const QString reason = m_lastHardwareDirectAttemptReason.isEmpty()
                ? QStringLiteral("No supported hardware-direct decoder-to-Vulkan handoff path is available.")
                : m_lastHardwareDirectAttemptReason;
            setStdError(errorMessage,
                        QStringLiteral(
                            "CPU upload fallback is disabled; hardware-direct handoff was not used: %1")
                            .arg(reason));
        }
        m_lastMode = FrameHandoffMode::Invalid;
        return false;
    }
    if (!frame.hasCpuImage()) {
        setStdError(errorMessage,
                    QStringLiteral(
                        "Direct hardware frame handoff is not implemented for this path; CPU image is required."));
        m_lastMode = FrameHandoffMode::Invalid;
        return false;
    }
    const QImage rgba = frame.cpuImage().convertToFormat(QImage::Format_RGBA8888);
    if (rgba.isNull()) {
        setStdError(errorMessage, QStringLiteral("Failed to materialize RGBA upload image."));
        m_lastMode = FrameHandoffMode::Invalid;
        return false;
    }
    if (!finishPendingUpload(nullptr, errorMessage)) {
        m_lastMode = FrameHandoffMode::Invalid;
        return false;
    }
    const VkDeviceSize bytes = static_cast<VkDeviceSize>(rgba.width()) * rgba.height() * 4;
    if (!ensureStagingBuffer(bytes, &qtError) ||
        !ensureImageResources(toSizeI(rgba.size()), VK_FORMAT_R8G8B8A8_UNORM, &qtError)) {
        setStdError(errorMessage, qtError);
        m_lastMode = FrameHandoffMode::Invalid;
        return false;
    }
    QElapsedTimer timer;
    timer.start();
    void* mapped = nullptr;
    if (vkMapMemory(m_context.device, m_stagingMemory, 0, bytes, 0, &mapped) != VK_SUCCESS || !mapped) {
        setStdError(errorMessage, QStringLiteral("Failed to map Vulkan handoff staging memory."));
        m_lastMode = FrameHandoffMode::Invalid;
        return false;
    }
    for (int y = 0; y < rgba.height(); ++y) {
        std::memcpy(static_cast<unsigned char*>(mapped) + y * rgba.width() * 4,
                    rgba.constScanLine(y),
                    static_cast<size_t>(rgba.width() * 4));
    }
    vkUnmapMemory(m_context.device, m_stagingMemory);

    vkResetFences(m_context.device, 1, &m_fence);
    vkResetCommandBuffer(m_commandBuffer, 0);
    VkCommandBufferBeginInfo begin{};
    begin.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    vkBeginCommandBuffer(m_commandBuffer, &begin);
    transitionImage(m_imageLayout, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
    VkBufferImageCopy copy{};
    copy.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    copy.imageSubresource.layerCount = 1;
    copy.imageExtent = {static_cast<uint32_t>(rgba.width()), static_cast<uint32_t>(rgba.height()), 1};
    vkCmdCopyBufferToImage(m_commandBuffer,
                           m_stagingBuffer,
                           m_image,
                           VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                           1,
                           &copy);
    transitionImage(VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    vkEndCommandBuffer(m_commandBuffer);
    VkSubmitInfo submit{};
    submit.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    submit.commandBufferCount = 1;
    submit.pCommandBuffers = &m_commandBuffer;
    if (vkQueueSubmit(m_context.queue, 1, &submit, m_fence) != VK_SUCCESS) {
        setStdError(errorMessage, QStringLiteral("Vulkan submit failed for frame handoff upload."));
        m_lastMode = FrameHandoffMode::Invalid;
        return false;
    }
    m_imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    m_lastMode = FrameHandoffMode::CpuUpload;
    m_uploadPending = true;
    m_pendingUploadTimer.start();
    if (uploadMs) {
        *uploadMs = static_cast<double>(timer.nsecsElapsed()) / 1'000'000.0;
    }
    return true;
}

bool VulkanDetectorFrameHandoff::finishPendingUpload(double* uploadMs, std::string* errorMessage)
{
    double externalCopyMs = 0.0;
    if (!m_externalFrameImporter.finishPendingCopy(
            &externalCopyMs, errorMessage)) {
        return false;
    }
    if (uploadMs) {
        *uploadMs = externalCopyMs;
    }
    if (!m_uploadPending) {
        m_pendingNv12DescriptorSet = VK_NULL_HANDLE;
        return true;
    }
    if (vkWaitForFences(m_context.device, 1, &m_fence, VK_TRUE, 5'000'000'000ull) != VK_SUCCESS) {
        setStdError(errorMessage, QStringLiteral("timed out waiting for Vulkan frame handoff upload"));
        return false;
    }
    m_pendingNv12DescriptorSet = VK_NULL_HANDLE;
    m_uploadPending = false;
    if (uploadMs) {
        *uploadMs += static_cast<double>(m_pendingUploadTimer.nsecsElapsed()) / 1'000'000.0;
    }
    return true;
}

bool VulkanDetectorFrameHandoff::recordHardwareFrameUpload(VkCommandBuffer commandBuffer,
                                                           const editor::FrameHandle& frame,
                                                           double* uploadMs,
                                                           std::string* errorMessage)
{
    QString qtError;
    m_lastYuvRgbMatrix.clear();
    if (!m_initialized || commandBuffer == VK_NULL_HANDLE || frame.isNull()) {
        setStdError(errorMessage, QStringLiteral("Invalid frame handoff command-buffer upload state."));
        m_lastMode = FrameHandoffMode::Invalid;
        return false;
    }
    bool isNv12 = false;
    jcut::core::SizeI size;
    int yPitch = 0;
    int uvPitch = 0;
    if (!prepareCudaHardwareFrame(frame, &isNv12, &size, &yPitch, &uvPitch, uploadMs, &qtError)) {
        setStdError(errorMessage, qtError);
        m_lastMode = FrameHandoffMode::Invalid;
        return false;
    }
    const Nv12ColorConversion colorConversion = nv12ColorConversionForFrame(frame);
    m_lastYuvRgbMatrix = isNv12 ? nv12ColorConversionLabel(colorConversion) : QString();
    const bool recorded = isNv12
        ? recordNv12Conversion(commandBuffer,
                               size.width,
                               size.height,
                               yPitch,
                               uvPitch,
                               colorConversion.fullRange,
                               colorConversion.colorMatrix,
                               colorConversion.chromaSwap,
                               &qtError)
        : recordCudaHardwareFrameCopy(commandBuffer, size, &qtError);
    if (!recorded) {
        setStdError(errorMessage, qtError);
        m_lastMode = FrameHandoffMode::Invalid;
        return false;
    }
    m_lastMode = FrameHandoffMode::HardwareDirect;
    return true;
}

bool VulkanDetectorFrameHandoff::prepareCudaHardwareFrame(const editor::FrameHandle& frame,
                                                          bool* isNv12Out,
                                                          jcut::core::SizeI* sizeOut,
                                                          int* yPitchOut,
                                                          int* uvPitchOut,
                                                          double* uploadMs,
                                                          QString* errorMessage)
{
#if !JCUT_HAS_CUDA_DRIVER
    Q_UNUSED(frame)
    Q_UNUSED(isNv12Out)
    Q_UNUSED(sizeOut)
    Q_UNUSED(yPitchOut)
    Q_UNUSED(uvPitchOut)
    Q_UNUSED(uploadMs)
    if (errorMessage) *errorMessage = QStringLiteral("CUDA driver interop is not compiled in (JCUT_HAS_CUDA_DRIVER=0)");
    return false;
#else
    if (frame.isNull() || !frame.hasHardwareFrame()) {
        if (errorMessage) *errorMessage = QStringLiteral("no hardware frame present");
        return false;
    }
    m_lastHardwareDirectAttemptReason.clear();
    m_lastProbe = probeHardwareInterop(frame);
    if (!m_lastProbe.supported) {
        if (errorMessage) *errorMessage = m_lastProbe.reason;
        return false;
    }
    if (m_lastProbe.path != QStringLiteral("cuda")) {
        if (errorMessage) *errorMessage = QStringLiteral("only CUDA hardware-direct path is currently implemented");
        return false;
    }

    const int swFmt = frame.hardwareSwPixelFormat();
    const bool isRgbaLike = (swFmt == AV_PIX_FMT_RGBA || swFmt == AV_PIX_FMT_BGRA ||
                             swFmt == AV_PIX_FMT_RGB0 || swFmt == AV_PIX_FMT_BGR0);
    const bool isNv12 = (swFmt == AV_PIX_FMT_NV12);
    if (!isRgbaLike && !isNv12) {
        if (errorMessage) *errorMessage = QStringLiteral("only RGBA/BGRA and NV12 CUDA hardware-direct formats are currently implemented");
        return false;
    }

    const AVFrame* hw = frame.hardwareFrame();
    if (!hw || hw->format != AV_PIX_FMT_CUDA || !hw->hw_frames_ctx) {
        if (errorMessage) *errorMessage = QStringLiteral("invalid CUDA hardware frame context");
        return false;
    }
    auto* framesCtx = reinterpret_cast<AVHWFramesContext*>(hw->hw_frames_ctx->data);
    if (!framesCtx || !framesCtx->device_ctx || !framesCtx->device_ctx->hwctx) {
        if (errorMessage) *errorMessage = QStringLiteral("missing CUDA device context on hardware frame");
        return false;
    }
    auto* cudaDevice = reinterpret_cast<AVCUDADeviceContext*>(framesCtx->device_ctx->hwctx);
    CUcontext cudaContext = cudaDevice->cuda_ctx;
    if (!cudaContext) {
        if (errorMessage) *errorMessage = QStringLiteral("missing CUDA context on frame");
        return false;
    }
    const jcut::core::SizeI size = toSizeI(frame.size());
    if (!size.valid()) {
        if (errorMessage) *errorMessage = QStringLiteral("invalid frame size for hardware-direct");
        return false;
    }

    const VkFormat targetFormat =
        (swFmt == AV_PIX_FMT_BGRA || swFmt == AV_PIX_FMT_BGR0)
            ? VK_FORMAT_B8G8R8A8_UNORM
            : VK_FORMAT_R8G8B8A8_UNORM;
    if (!ensureImageResources(size, targetFormat, errorMessage)) {
        return false;
    }
    std::string finishError;
    if (!finishPendingUpload(nullptr, &finishError)) {
        if (errorMessage) {
            *errorMessage = QString::fromStdString(finishError);
        }
        return false;
    }

    const VkDeviceSize bytes = isNv12
        ? static_cast<VkDeviceSize>(hw->linesize[0]) * static_cast<VkDeviceSize>(size.height)
        : static_cast<VkDeviceSize>(size.width) * size.height * 4;
    const int uvPitch = (hw->linesize[1] > 0) ? hw->linesize[1] : hw->linesize[0];
    const VkDeviceSize uvBytes = isNv12
        ? static_cast<VkDeviceSize>(uvPitch) * static_cast<VkDeviceSize>((size.height + 1) / 2)
        : 0;

    PFN_vkGetMemoryFdKHR vkGetMemoryFdKHR = reinterpret_cast<PFN_vkGetMemoryFdKHR>(
        vkGetDeviceProcAddr(m_context.device, "vkGetMemoryFdKHR"));
    if (!vkGetMemoryFdKHR) {
        if (errorMessage) *errorMessage = QStringLiteral("vkGetMemoryFdKHR unavailable");
        return false;
    }

    const bool recreateY = m_cudaExportBuffer == VK_NULL_HANDLE || m_cudaExportSize < bytes;
    const bool recreateUv = isNv12 && (m_cudaExportUvBuffer == VK_NULL_HANDLE || m_cudaExportUvSize < uvBytes);
    if (recreateY || recreateUv) {
        if (recreateY) {
            retireCudaExportBuffer(m_cudaExportBuffer,
                                   m_cudaExportMemory,
                                   m_cudaExportSize,
                                   m_cudaExternalMemory,
                                   m_cudaExternalDevicePtr,
                                   m_cudaImportContext);
        }
        if (recreateY && !ensureCudaExportBuffer(bytes, m_cudaExportBuffer, m_cudaExportMemory, m_cudaExportSize, errorMessage)) {
            return false;
        }
        if (isNv12) {
            if (recreateUv) {
                retireCudaExportBuffer(m_cudaExportUvBuffer,
                                       m_cudaExportUvMemory,
                                       m_cudaExportUvSize,
                                       m_cudaExternalUvMemory,
                                       m_cudaExternalUvDevicePtr,
                                       m_cudaImportContext);
            }
            if (!ensureCudaExportBuffer(uvBytes, m_cudaExportUvBuffer, m_cudaExportUvMemory, m_cudaExportUvSize, errorMessage)) {
                return false;
            }
        }
    }

    CUcontext previous = nullptr;
    if (cuInit(0) != CUDA_SUCCESS || cuCtxPushCurrent(cudaContext) != CUDA_SUCCESS) {
        if (errorMessage) *errorMessage = QStringLiteral("failed to activate CUDA context for hardware-direct");
        return false;
    }
    auto pop = qScopeGuard([&]() { cuCtxPopCurrent(&previous); });

    if ((m_cudaExternalMemory || m_cudaExternalUvMemory) && m_cudaImportContext != cudaContext) {
        destroyCudaExternalMemory(m_cudaExternalMemory, m_cudaExternalDevicePtr, m_cudaImportContext);
        destroyCudaExternalMemory(m_cudaExternalUvMemory, m_cudaExternalUvDevicePtr, m_cudaImportContext);
        m_cudaImportContext = nullptr;
    }
    auto importExternalBuffer = [&](VkDeviceMemory memory,
                                    VkDeviceSize allocationSize,
                                    void*& externalMemory,
                                    quint64& devicePtr,
                                    const QString& label) -> bool {
        if (externalMemory && devicePtr) {
            return true;
        }
        VkMemoryGetFdInfoKHR fdInfo{};
        fdInfo.sType = VK_STRUCTURE_TYPE_MEMORY_GET_FD_INFO_KHR;
        fdInfo.memory = memory;
        fdInfo.handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_FD_BIT;
        int fd = -1;
        if (vkGetMemoryFdKHR(m_context.device, &fdInfo, &fd) != VK_SUCCESS || fd < 0) {
            if (errorMessage) *errorMessage = QStringLiteral("failed to export Vulkan memory FD for CUDA import (%1)").arg(label);
            return false;
        }
        CUDA_EXTERNAL_MEMORY_HANDLE_DESC handleDesc{};
        handleDesc.type = CU_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_FD;
        handleDesc.handle.fd = fd;
        handleDesc.size = allocationSize;
        CUexternalMemory extMem = nullptr;
        if (cuImportExternalMemory(&extMem, &handleDesc) != CUDA_SUCCESS) {
            close(fd);
            if (errorMessage) *errorMessage = QStringLiteral("cuImportExternalMemory failed (%1)").arg(label);
            return false;
        }
        CUDA_EXTERNAL_MEMORY_BUFFER_DESC bufDesc{};
        bufDesc.offset = 0;
        bufDesc.size = allocationSize;
        CUdeviceptr devPtr = 0;
        if (cuExternalMemoryGetMappedBuffer(&devPtr, extMem, &bufDesc) != CUDA_SUCCESS) {
            cuDestroyExternalMemory(extMem);
            if (errorMessage) *errorMessage = QStringLiteral("cuExternalMemoryGetMappedBuffer failed (%1)").arg(label);
            return false;
        }
        externalMemory = extMem;
        devicePtr = static_cast<quint64>(devPtr);
        m_cudaImportContext = cudaContext;
        return true;
    };
    if (!importExternalBuffer(m_cudaExportMemory,
                              m_cudaExportSize,
                              m_cudaExternalMemory,
                              m_cudaExternalDevicePtr,
                              QStringLiteral("y/rgba"))) {
        return false;
    }
    if (isNv12 && !importExternalBuffer(m_cudaExportUvMemory,
                                        m_cudaExportUvSize,
                                        m_cudaExternalUvMemory,
                                        m_cudaExternalUvDevicePtr,
                                        QStringLiteral("uv"))) {
        return false;
    }

    QElapsedTimer timer;
    timer.start();
    if (isNv12) {
        CUDA_MEMCPY2D yCopy{};
        yCopy.srcMemoryType = CU_MEMORYTYPE_DEVICE;
        yCopy.srcDevice = reinterpret_cast<CUdeviceptr>(hw->data[0]);
        yCopy.srcPitch = static_cast<size_t>(hw->linesize[0]);
        yCopy.dstMemoryType = CU_MEMORYTYPE_DEVICE;
        yCopy.dstDevice = static_cast<CUdeviceptr>(m_cudaExternalDevicePtr);
        yCopy.dstPitch = static_cast<size_t>(hw->linesize[0]);
        yCopy.WidthInBytes = static_cast<size_t>(size.width);
        yCopy.Height = static_cast<size_t>(size.height);

        CUDA_MEMCPY2D uvCopy{};
        uvCopy.srcMemoryType = CU_MEMORYTYPE_DEVICE;
        uvCopy.srcDevice = hw->data[1]
            ? reinterpret_cast<CUdeviceptr>(hw->data[1])
            : reinterpret_cast<CUdeviceptr>(hw->data[0] + (static_cast<size_t>(hw->linesize[0]) * static_cast<size_t>(size.height)));
        uvCopy.srcPitch = static_cast<size_t>(uvPitch);
        uvCopy.dstMemoryType = CU_MEMORYTYPE_DEVICE;
        uvCopy.dstDevice = static_cast<CUdeviceptr>(m_cudaExternalUvDevicePtr);
        uvCopy.dstPitch = static_cast<size_t>(uvPitch);
        uvCopy.WidthInBytes = static_cast<size_t>(size.width);
        uvCopy.Height = static_cast<size_t>((size.height + 1) / 2);
        if (cuMemcpy2DAsync(&yCopy, cudaDevice->stream) != CUDA_SUCCESS ||
            cuMemcpy2DAsync(&uvCopy, cudaDevice->stream) != CUDA_SUCCESS ||
            synchronizeCudaStream(cudaDevice->stream, &m_resourceStats) != CUDA_SUCCESS) {
            if (errorMessage) *errorMessage = QStringLiteral("CUDA NV12 device copy into Vulkan-export buffers failed");
            return false;
        }
    } else {
        const size_t widthBytes = static_cast<size_t>(size.width) * 4;
        CUDA_MEMCPY2D copy{};
        copy.srcMemoryType = CU_MEMORYTYPE_DEVICE;
        copy.srcDevice = reinterpret_cast<CUdeviceptr>(hw->data[0]);
        copy.srcPitch = static_cast<size_t>(hw->linesize[0]);
        copy.dstMemoryType = CU_MEMORYTYPE_DEVICE;
        copy.dstDevice = static_cast<CUdeviceptr>(m_cudaExternalDevicePtr);
        copy.dstPitch = widthBytes;
        copy.WidthInBytes = widthBytes;
        copy.Height = static_cast<size_t>(size.height);
        if (cuMemcpy2DAsync(&copy, cudaDevice->stream) != CUDA_SUCCESS ||
            synchronizeCudaStream(cudaDevice->stream, &m_resourceStats) != CUDA_SUCCESS) {
            if (errorMessage) *errorMessage = QStringLiteral("CUDA device copy into Vulkan-export buffer failed");
            return false;
        }
    }

    if (isNv12Out) *isNv12Out = isNv12;
    if (sizeOut) *sizeOut = size;
    if (yPitchOut) *yPitchOut = hw->linesize[0];
    if (uvPitchOut) *uvPitchOut = uvPitch;
    if (uploadMs) {
        *uploadMs = static_cast<double>(timer.nsecsElapsed()) / 1'000'000.0;
    }
    return true;
#endif
}

bool VulkanDetectorFrameHandoff::recordCudaHardwareFrameCopy(VkCommandBuffer commandBuffer,
                                                             const jcut::core::SizeI& size,
                                                             QString* errorMessage)
{
    if (commandBuffer == VK_NULL_HANDLE || m_cudaExportBuffer == VK_NULL_HANDLE || m_image == VK_NULL_HANDLE) {
        if (errorMessage) *errorMessage = QStringLiteral("Invalid Vulkan resources for command-buffer hardware handoff.");
        return false;
    }
    VkBufferMemoryBarrier cudaWriteBarrier{};
    cudaWriteBarrier.sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER;
    cudaWriteBarrier.srcAccessMask = VK_ACCESS_MEMORY_WRITE_BIT;
    cudaWriteBarrier.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
    cudaWriteBarrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    cudaWriteBarrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    cudaWriteBarrier.buffer = m_cudaExportBuffer;
    cudaWriteBarrier.offset = 0;
    cudaWriteBarrier.size = VK_WHOLE_SIZE;
    vkCmdPipelineBarrier(commandBuffer,
                         VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
                         VK_PIPELINE_STAGE_TRANSFER_BIT,
                         0,
                         0,
                         nullptr,
                         1,
                         &cudaWriteBarrier,
                         0,
                         nullptr);
    transitionImage(commandBuffer, m_imageLayout, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
    VkBufferImageCopy bi{};
    bi.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    bi.imageSubresource.layerCount = 1;
    bi.imageExtent = {static_cast<uint32_t>(size.width), static_cast<uint32_t>(size.height), 1};
    vkCmdCopyBufferToImage(commandBuffer,
                           m_cudaExportBuffer,
                           m_image,
                           VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                           1,
                           &bi);
    transitionImage(commandBuffer, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    m_imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    return true;
}

bool VulkanDetectorFrameHandoff::recordNv12Conversion(VkCommandBuffer commandBuffer,
                                                      int width,
                                                      int height,
                                                      int yPitch,
                                                      int uvPitch,
                                                      int fullRange,
                                                      int colorMatrix,
                                                      int chromaSwap,
                                                      QString* errorMessage)
{
    if (commandBuffer == VK_NULL_HANDLE ||
        m_cudaExportBuffer == VK_NULL_HANDLE ||
        m_cudaExportUvBuffer == VK_NULL_HANDLE ||
        m_image == VK_NULL_HANDLE) {
        if (errorMessage) *errorMessage = QStringLiteral("Invalid Vulkan resources for command-buffer NV12 handoff.");
        return false;
    }
    if (!ensureNv12ConversionResources(errorMessage)) {
        return false;
    }
    VkDescriptorSet set = m_reusableNv12DescriptorSet;
    if (set == VK_NULL_HANDLE) {
        VkDescriptorSetAllocateInfo alloc{};
        alloc.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
        alloc.descriptorPool = m_nv12DescriptorPool;
        alloc.descriptorSetCount = 1;
        alloc.pSetLayouts = &m_nv12DescriptorSetLayout;
        if (vkAllocateDescriptorSets(m_context.device, &alloc, &set) != VK_SUCCESS) {
            if (errorMessage) *errorMessage = QStringLiteral("Failed to allocate NV12 conversion descriptor set.");
            return false;
        }
        m_reusableNv12DescriptorSet = set;
        ++m_resourceStats.descriptorAllocations;
    }

    VkDescriptorBufferInfo yInfo{m_cudaExportBuffer, 0, VK_WHOLE_SIZE};
    VkDescriptorBufferInfo uvInfo{m_cudaExportUvBuffer, 0, VK_WHOLE_SIZE};
    VkDescriptorImageInfo imageInfo{};
    imageInfo.imageView = m_imageView;
    imageInfo.imageLayout = VK_IMAGE_LAYOUT_GENERAL;

    std::array<VkWriteDescriptorSet, 3> writes{};
    writes[0].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    writes[0].dstSet = set;
    writes[0].dstBinding = 0;
    writes[0].descriptorCount = 1;
    writes[0].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    writes[0].pBufferInfo = &yInfo;
    writes[1] = writes[0];
    writes[1].dstBinding = 1;
    writes[1].pBufferInfo = &uvInfo;
    writes[2].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    writes[2].dstSet = set;
    writes[2].dstBinding = 2;
    writes[2].descriptorCount = 1;
    writes[2].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
    writes[2].pImageInfo = &imageInfo;
    vkUpdateDescriptorSets(m_context.device, static_cast<uint32_t>(writes.size()), writes.data(), 0, nullptr);

    std::array<VkBufferMemoryBarrier, 2> cudaWriteBarriers{};
    cudaWriteBarriers[0].sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER;
    cudaWriteBarriers[0].srcAccessMask = VK_ACCESS_MEMORY_WRITE_BIT;
    cudaWriteBarriers[0].dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
    cudaWriteBarriers[0].srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    cudaWriteBarriers[0].dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    cudaWriteBarriers[0].buffer = m_cudaExportBuffer;
    cudaWriteBarriers[0].offset = 0;
    cudaWriteBarriers[0].size = VK_WHOLE_SIZE;
    cudaWriteBarriers[1] = cudaWriteBarriers[0];
    cudaWriteBarriers[1].buffer = m_cudaExportUvBuffer;
    vkCmdPipelineBarrier(commandBuffer,
                         VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
                         VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                         0,
                         0,
                         nullptr,
                         static_cast<uint32_t>(cudaWriteBarriers.size()),
                         cudaWriteBarriers.data(),
                         0,
                         nullptr);

    transitionImage(commandBuffer, m_imageLayout, VK_IMAGE_LAYOUT_GENERAL);
    struct Push {
        int width;
        int height;
        int yPitch;
        int uvPitch;
        int fullRange;
        int colorMatrix;
        int chromaSwap;
    } push{width, height, yPitch, uvPitch, fullRange, colorMatrix, chromaSwap};
    vkCmdBindPipeline(commandBuffer, VK_PIPELINE_BIND_POINT_COMPUTE, m_nv12Pipeline);
    vkCmdBindDescriptorSets(commandBuffer,
                            VK_PIPELINE_BIND_POINT_COMPUTE,
                            m_nv12PipelineLayout,
                            0,
                            1,
                            &set,
                            0,
                            nullptr);
    vkCmdPushConstants(commandBuffer,
                       m_nv12PipelineLayout,
                       VK_SHADER_STAGE_COMPUTE_BIT,
                       0,
                       sizeof(push),
                       &push);
    vkCmdDispatch(commandBuffer,
                  static_cast<uint32_t>((width + 15) / 16),
                  static_cast<uint32_t>((height + 15) / 16),
                  1);

    VkImageMemoryBarrier barrier{};
    barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    barrier.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
    barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
    barrier.oldLayout = VK_IMAGE_LAYOUT_GENERAL;
    barrier.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.image = m_image;
    barrier.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    barrier.subresourceRange.levelCount = 1;
    barrier.subresourceRange.layerCount = 1;
    vkCmdPipelineBarrier(commandBuffer,
                         VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                         VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
                         0,
                         0,
                         nullptr,
                         0,
                         nullptr,
                         1,
                         &barrier);
    m_imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    m_pendingNv12DescriptorSet = set;
    return true;
}

bool VulkanDetectorFrameHandoff::tryHardwareDirect(const editor::FrameHandle& frame,
                                                   double* uploadMs,
                                                   QString* errorMessage)
{
#if !JCUT_HAS_CUDA_DRIVER
    Q_UNUSED(frame)
    Q_UNUSED(uploadMs)
    if (errorMessage) *errorMessage = QStringLiteral("CUDA driver interop is not compiled in (JCUT_HAS_CUDA_DRIVER=0)");
    return false;
#else
    if (frame.isNull() || !frame.hasHardwareFrame()) {
        if (errorMessage) *errorMessage = QStringLiteral("no hardware frame present");
        return false;
    }
    const auto probe = probeHardwareInterop(frame);
    if (!probe.supported) {
        if (errorMessage) *errorMessage = probe.reason;
        return false;
    }
    if (probe.path != QStringLiteral("cuda")) {
        if (errorMessage) *errorMessage = QStringLiteral("only CUDA hardware-direct path is currently implemented");
        return false;
    }
    const int swFmt = frame.hardwareSwPixelFormat();
    const bool isRgbaLike = (swFmt == AV_PIX_FMT_RGBA || swFmt == AV_PIX_FMT_BGRA ||
                             swFmt == AV_PIX_FMT_RGB0 || swFmt == AV_PIX_FMT_BGR0);
    const bool isNv12Like = (swFmt == AV_PIX_FMT_NV12 || swFmt == AV_PIX_FMT_P010 || swFmt == AV_PIX_FMT_P016);
    if (!isRgbaLike && !isNv12Like) {
        if (errorMessage) *errorMessage = QStringLiteral("unsupported CUDA sw format for hardware-direct: %1").arg(swFmt);
        return false;
    }
    const AVFrame* hw = frame.hardwareFrame();
    if (!hw || hw->format != AV_PIX_FMT_CUDA || !hw->hw_frames_ctx) {
        if (errorMessage) *errorMessage = QStringLiteral("invalid CUDA hardware frame context");
        return false;
    }
    auto* framesCtx = reinterpret_cast<AVHWFramesContext*>(hw->hw_frames_ctx->data);
    if (!framesCtx || !framesCtx->device_ctx || !framesCtx->device_ctx->hwctx) {
        if (errorMessage) *errorMessage = QStringLiteral("missing CUDA device context on hardware frame");
        return false;
    }
    auto* cudaDevice = reinterpret_cast<AVCUDADeviceContext*>(framesCtx->device_ctx->hwctx);
    CUcontext cudaContext = cudaDevice->cuda_ctx;
    if (!cudaContext) {
        if (errorMessage) *errorMessage = QStringLiteral("missing CUDA context on frame");
        return false;
    }
    const jcut::core::SizeI size = toSizeI(frame.size());
    if (!size.valid()) {
        if (errorMessage) *errorMessage = QStringLiteral("invalid frame size for hardware-direct");
        return false;
    }
    const bool isNv12 = (swFmt == AV_PIX_FMT_NV12);
    if (!isRgbaLike && !isNv12) {
        if (errorMessage) *errorMessage = QStringLiteral("only RGBA/BGRA and NV12 CUDA hardware-direct formats are currently implemented");
        return false;
    }
    const VkFormat targetFormat =
        (swFmt == AV_PIX_FMT_BGRA || swFmt == AV_PIX_FMT_BGR0)
            ? VK_FORMAT_B8G8R8A8_UNORM
            : VK_FORMAT_R8G8B8A8_UNORM;
    if (!ensureImageResources(size, targetFormat, errorMessage)) {
        return false;
    }
    std::string finishError;
    if (!finishPendingUpload(nullptr, &finishError)) {
        if (errorMessage) {
            *errorMessage = QString::fromStdString(finishError);
        }
        return false;
    }

    const VkDeviceSize bytes = isNv12
        ? static_cast<VkDeviceSize>(hw->linesize[0]) * static_cast<VkDeviceSize>(size.height)
        : static_cast<VkDeviceSize>(size.width) * size.height * 4;
    const int uvPitch = (hw->linesize[1] > 0) ? hw->linesize[1] : hw->linesize[0];
    const VkDeviceSize uvBytes = isNv12
        ? static_cast<VkDeviceSize>(uvPitch) * static_cast<VkDeviceSize>((size.height + 1) / 2)
        : 0;
    PFN_vkGetMemoryFdKHR vkGetMemoryFdKHR = reinterpret_cast<PFN_vkGetMemoryFdKHR>(
        vkGetDeviceProcAddr(m_context.device, "vkGetMemoryFdKHR"));
    if (!vkGetMemoryFdKHR) {
        if (errorMessage) *errorMessage = QStringLiteral("vkGetMemoryFdKHR unavailable");
        return false;
    }
    const bool recreateY = m_cudaExportBuffer == VK_NULL_HANDLE || m_cudaExportSize < bytes;
    const bool recreateUv = isNv12 && (m_cudaExportUvBuffer == VK_NULL_HANDLE || m_cudaExportUvSize < uvBytes);
    if (recreateY || recreateUv) {
        if (recreateY) {
            retireCudaExportBuffer(m_cudaExportBuffer,
                                   m_cudaExportMemory,
                                   m_cudaExportSize,
                                   m_cudaExternalMemory,
                                   m_cudaExternalDevicePtr,
                                   m_cudaImportContext);
        }
        if (recreateY && !ensureCudaExportBuffer(bytes, m_cudaExportBuffer, m_cudaExportMemory, m_cudaExportSize, errorMessage)) {
            return false;
        }
        if (isNv12) {
            if (recreateUv) {
                retireCudaExportBuffer(m_cudaExportUvBuffer,
                                       m_cudaExportUvMemory,
                                       m_cudaExportUvSize,
                                       m_cudaExternalUvMemory,
                                       m_cudaExternalUvDevicePtr,
                                       m_cudaImportContext);
            }
            if (!ensureCudaExportBuffer(uvBytes, m_cudaExportUvBuffer, m_cudaExportUvMemory, m_cudaExportUvSize, errorMessage)) {
                return false;
            }
        }
    }

    CUcontext previous = nullptr;
    if (cuInit(0) != CUDA_SUCCESS || cuCtxPushCurrent(cudaContext) != CUDA_SUCCESS) {
        if (errorMessage) *errorMessage = QStringLiteral("failed to activate CUDA context for hardware-direct");
        return false;
    }
    auto pop = qScopeGuard([&]() { cuCtxPopCurrent(&previous); });

    if ((m_cudaExternalMemory || m_cudaExternalUvMemory) && m_cudaImportContext != cudaContext) {
        destroyCudaExternalMemory(m_cudaExternalMemory, m_cudaExternalDevicePtr, m_cudaImportContext);
        destroyCudaExternalMemory(m_cudaExternalUvMemory, m_cudaExternalUvDevicePtr, m_cudaImportContext);
        m_cudaImportContext = nullptr;
    }
    auto importExternalBuffer = [&](VkDeviceMemory memory,
                                    VkDeviceSize allocationSize,
                                    void*& externalMemory,
                                    quint64& devicePtr,
                                    const QString& label) -> bool {
        if (externalMemory && devicePtr) {
            return true;
        }
        VkMemoryGetFdInfoKHR fdInfo{};
        fdInfo.sType = VK_STRUCTURE_TYPE_MEMORY_GET_FD_INFO_KHR;
        fdInfo.memory = memory;
        fdInfo.handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_FD_BIT;
        int fd = -1;
        if (vkGetMemoryFdKHR(m_context.device, &fdInfo, &fd) != VK_SUCCESS || fd < 0) {
            if (errorMessage) *errorMessage = QStringLiteral("failed to export Vulkan memory FD for CUDA import (%1)").arg(label);
            return false;
        }
        CUDA_EXTERNAL_MEMORY_HANDLE_DESC handleDesc{};
        handleDesc.type = CU_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_FD;
        handleDesc.handle.fd = fd;
        handleDesc.size = allocationSize;
        CUexternalMemory extMem = nullptr;
        if (cuImportExternalMemory(&extMem, &handleDesc) != CUDA_SUCCESS) {
            close(fd);
            if (errorMessage) *errorMessage = QStringLiteral("cuImportExternalMemory failed (%1)").arg(label);
            return false;
        }
        CUDA_EXTERNAL_MEMORY_BUFFER_DESC bufDesc{};
        bufDesc.offset = 0;
        bufDesc.size = allocationSize;
        CUdeviceptr devPtr = 0;
        if (cuExternalMemoryGetMappedBuffer(&devPtr, extMem, &bufDesc) != CUDA_SUCCESS) {
            cuDestroyExternalMemory(extMem);
            if (errorMessage) *errorMessage = QStringLiteral("cuExternalMemoryGetMappedBuffer failed (%1)").arg(label);
            return false;
        }
        externalMemory = extMem;
        devicePtr = static_cast<quint64>(devPtr);
        m_cudaImportContext = cudaContext;
        return true;
    };
    if (!importExternalBuffer(m_cudaExportMemory,
                              m_cudaExportSize,
                              m_cudaExternalMemory,
                              m_cudaExternalDevicePtr,
                              QStringLiteral("y/rgba"))) {
        return false;
    }
    if (isNv12 && !importExternalBuffer(m_cudaExportUvMemory,
                                        m_cudaExportUvSize,
                                        m_cudaExternalUvMemory,
                                        m_cudaExternalUvDevicePtr,
                                        QStringLiteral("uv"))) {
        return false;
    }
    if (!ensureCudaReadySemaphore(cudaContext, errorMessage)) {
        return false;
    }

    QElapsedTimer timer;
    timer.start();

    if (isNv12) {
        CUDA_MEMCPY2D yCopy{};
        yCopy.srcMemoryType = CU_MEMORYTYPE_DEVICE;
        yCopy.srcDevice = reinterpret_cast<CUdeviceptr>(hw->data[0]);
        yCopy.srcPitch = static_cast<size_t>(hw->linesize[0]);
        yCopy.dstMemoryType = CU_MEMORYTYPE_DEVICE;
        yCopy.dstDevice = static_cast<CUdeviceptr>(m_cudaExternalDevicePtr);
        yCopy.dstPitch = static_cast<size_t>(hw->linesize[0]);
        yCopy.WidthInBytes = static_cast<size_t>(size.width);
        yCopy.Height = static_cast<size_t>(size.height);

        CUDA_MEMCPY2D uvCopy{};
        uvCopy.srcMemoryType = CU_MEMORYTYPE_DEVICE;
        uvCopy.srcDevice = hw->data[1]
            ? reinterpret_cast<CUdeviceptr>(hw->data[1])
            : reinterpret_cast<CUdeviceptr>(hw->data[0] + (static_cast<size_t>(hw->linesize[0]) * static_cast<size_t>(size.height)));
        uvCopy.srcPitch = static_cast<size_t>(uvPitch);
        uvCopy.dstMemoryType = CU_MEMORYTYPE_DEVICE;
        uvCopy.dstDevice = static_cast<CUdeviceptr>(m_cudaExternalUvDevicePtr);
        uvCopy.dstPitch = static_cast<size_t>(uvPitch);
        uvCopy.WidthInBytes = static_cast<size_t>(size.width);
        uvCopy.Height = static_cast<size_t>((size.height + 1) / 2);

        if (cuMemcpy2DAsync(&yCopy, cudaDevice->stream) != CUDA_SUCCESS ||
            cuMemcpy2DAsync(&uvCopy, cudaDevice->stream) != CUDA_SUCCESS) {
            if (errorMessage) *errorMessage = QStringLiteral("CUDA NV12 device copy into Vulkan-export buffers failed");
            return false;
        }
        if (!signalCudaReadySemaphore(cudaDevice->stream, errorMessage)) {
            return false;
        }
        const Nv12ColorConversion colorConversion = nv12ColorConversionForFrame(frame);
        m_lastYuvRgbMatrix = nv12ColorConversionLabel(colorConversion);
        if (!convertNv12BuffersToImage(size.width,
                                       size.height,
                                       hw->linesize[0],
                                       uvPitch,
                                       colorConversion.fullRange,
                                       colorConversion.colorMatrix,
                                       colorConversion.chromaSwap,
                                       errorMessage)) {
            return false;
        }
        if (uploadMs) {
            *uploadMs = static_cast<double>(timer.nsecsElapsed()) / 1'000'000.0;
        }
        return true;
    }

    const size_t widthBytes = static_cast<size_t>(size.width) * 4;
    CUDA_MEMCPY2D copy{};
    copy.srcMemoryType = CU_MEMORYTYPE_DEVICE;
    copy.srcDevice = reinterpret_cast<CUdeviceptr>(hw->data[0]);
    copy.srcPitch = static_cast<size_t>(hw->linesize[0]);
    copy.dstMemoryType = CU_MEMORYTYPE_DEVICE;
    copy.dstDevice = static_cast<CUdeviceptr>(m_cudaExternalDevicePtr);
    copy.dstPitch = widthBytes;
    copy.WidthInBytes = widthBytes;
    copy.Height = static_cast<size_t>(size.height);
    if (cuMemcpy2DAsync(&copy, cudaDevice->stream) != CUDA_SUCCESS) {
        if (errorMessage) *errorMessage = QStringLiteral("CUDA device copy into Vulkan-export buffer failed");
        return false;
    }
    if (!signalCudaReadySemaphore(cudaDevice->stream, errorMessage)) {
        return false;
    }

    vkResetFences(m_context.device, 1, &m_fence);
    vkResetCommandBuffer(m_commandBuffer, 0);
    VkCommandBufferBeginInfo begin{};
    begin.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    if (vkBeginCommandBuffer(m_commandBuffer, &begin) != VK_SUCCESS) {
        if (errorMessage) *errorMessage = QStringLiteral("vkBeginCommandBuffer failed for hardware-direct");
        return false;
    }
    VkBufferMemoryBarrier cudaWriteBarrier{};
    cudaWriteBarrier.sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER;
    cudaWriteBarrier.srcAccessMask = VK_ACCESS_MEMORY_WRITE_BIT;
    cudaWriteBarrier.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
    cudaWriteBarrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    cudaWriteBarrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    cudaWriteBarrier.buffer = m_cudaExportBuffer;
    cudaWriteBarrier.offset = 0;
    cudaWriteBarrier.size = VK_WHOLE_SIZE;
    vkCmdPipelineBarrier(m_commandBuffer,
                         VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
                         VK_PIPELINE_STAGE_TRANSFER_BIT,
                         0,
                         0,
                         nullptr,
                         1,
                         &cudaWriteBarrier,
                         0,
                         nullptr);
    transitionImage(m_imageLayout, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
    VkBufferImageCopy bi{};
    bi.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    bi.imageSubresource.layerCount = 1;
    bi.imageExtent = {static_cast<uint32_t>(size.width), static_cast<uint32_t>(size.height), 1};
    vkCmdCopyBufferToImage(m_commandBuffer,
                           m_cudaExportBuffer,
                           m_image,
                           VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                           1,
                           &bi);
    transitionImage(VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    if (vkEndCommandBuffer(m_commandBuffer) != VK_SUCCESS) {
        if (errorMessage) *errorMessage = QStringLiteral("vkEndCommandBuffer failed for hardware-direct");
        return false;
    }
    if (!submitCommandBufferWaitingOnCuda(VK_PIPELINE_STAGE_TRANSFER_BIT, errorMessage)) {
        return false;
    }
    m_imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    m_uploadPending = true;
    m_pendingUploadTimer.start();
    if (uploadMs) {
        *uploadMs = static_cast<double>(timer.nsecsElapsed()) / 1'000'000.0;
    }
    return true;
#endif
}

bool VulkanDetectorFrameHandoff::importOffscreenFrame(
    const render_detail::OffscreenVulkanFrame& frame,
    std::string* errorMessage)
{
    const bool imported =
        m_externalFrameImporter.importFrame(frame, errorMessage);
    m_lastMode = imported
        ? FrameHandoffMode::ExternalMemoryImport
        : FrameHandoffMode::Invalid;
    return imported;
}

bool VulkanDetectorFrameHandoff::recordImportedFrameCopy(
    VkCommandBuffer commandBuffer,
    const render_detail::OffscreenVulkanFrame& frame,
    std::string* errorMessage)
{
    const bool imported = m_externalFrameImporter.recordFrameCopy(
        commandBuffer, frame, errorMessage);
    m_lastMode = imported
        ? FrameHandoffMode::ExternalMemoryImport
        : FrameHandoffMode::Invalid;
    return imported;
}

VkImage VulkanDetectorFrameHandoff::image() const
{
    return m_lastMode == FrameHandoffMode::ExternalMemoryImport
        ? m_externalFrameImporter.externalImage().image
        : m_image;
}

VkImageLayout VulkanDetectorFrameHandoff::imageLayout() const
{
    return m_lastMode == FrameHandoffMode::ExternalMemoryImport
        ? m_externalFrameImporter.externalImage().imageLayout
        : m_imageLayout;
}

VkFormat VulkanDetectorFrameHandoff::imageFormat() const
{
    return m_lastMode == FrameHandoffMode::ExternalMemoryImport
        ? m_externalFrameImporter.externalImage().imageFormat
        : m_imageFormat;
}

FrameHandoffResourceStats VulkanDetectorFrameHandoff::resourceStats() const
{
    FrameHandoffResourceStats result = m_resourceStats;
    const jcut::vulkan_import::ResourceStats importStats =
        m_externalFrameImporter.resourceStats();
    result.imageMemoryAllocations += importStats.imageMemoryAllocations;
    result.imageMemoryFrees += importStats.imageMemoryFrees;
    result.importedMemoryAllocations += importStats.importedMemoryAllocations;
    result.importedMemoryFrees += importStats.importedMemoryFrees;
    return result;
}

VulkanExternalImage VulkanDetectorFrameHandoff::externalImage() const
{
    if (m_lastMode == FrameHandoffMode::ExternalMemoryImport) {
        const jcut::vulkan_import::ExternalImage imported =
            m_externalFrameImporter.externalImage();
        VulkanExternalImage image;
        image.imageView = imported.imageView;
        image.imageLayout = imported.imageLayout;
        image.size = imported.size;
        image.sourceIsSrgb = imported.sourceIsSrgb;
        image.sourceX = imported.sourceX;
        image.sourceY = imported.sourceY;
        image.sourceWidth = imported.sourceWidth;
        image.sourceHeight = imported.sourceHeight;
        return image;
    }
    VulkanExternalImage image;
    image.imageView = m_imageView;
    image.imageLayout = m_imageLayout;
    image.size = m_imageSize;
    image.sourceIsSrgb = false;
    return image;
}

void VulkanDetectorFrameHandoff::destroyBuffer(VkBuffer& buffer, VkDeviceMemory& memory, quint64* freeCounter)
{
    if (buffer != VK_NULL_HANDLE) vkDestroyBuffer(m_context.device, buffer, nullptr);
    if (memory != VK_NULL_HANDLE) {
        vkFreeMemory(m_context.device, memory, nullptr);
        if (freeCounter) {
            ++(*freeCounter);
        }
    }
    buffer = VK_NULL_HANDLE;
    memory = VK_NULL_HANDLE;
}

void VulkanDetectorFrameHandoff::retireCudaExportBuffer(VkBuffer& buffer,
                                                        VkDeviceMemory& memory,
                                                        VkDeviceSize& size,
                                                        void*& externalMemory,
                                                        quint64& externalDevicePtr,
                                                        void* importContext)
{
    if (buffer != VK_NULL_HANDLE || memory != VK_NULL_HANDLE || externalMemory) {
        RetiredCudaExportBuffer retired;
        retired.buffer = buffer;
        retired.memory = memory;
        retired.externalMemory = externalMemory;
        retired.externalDevicePtr = externalDevicePtr;
        retired.importContext = importContext;
        m_retiredCudaExportBuffers.push_back(retired);
    }
    buffer = VK_NULL_HANDLE;
    memory = VK_NULL_HANDLE;
    size = 0;
    externalMemory = nullptr;
    externalDevicePtr = 0;
}

void VulkanDetectorFrameHandoff::destroyRetiredCudaExportBuffers()
{
    for (RetiredCudaExportBuffer& retired : m_retiredCudaExportBuffers) {
#if JCUT_HAS_CUDA_DRIVER
        destroyCudaExternalMemory(retired.externalMemory,
                                  retired.externalDevicePtr,
                                  retired.importContext);
#else
        retired.externalMemory = nullptr;
        retired.externalDevicePtr = 0;
        Q_UNUSED(retired);
#endif
        destroyBuffer(retired.buffer, retired.memory, &m_resourceStats.stagingBufferFrees);
    }
    m_retiredCudaExportBuffers.clear();
}

uint32_t VulkanDetectorFrameHandoff::findMemoryType(uint32_t typeBits, VkMemoryPropertyFlags properties) const
{
    VkPhysicalDeviceMemoryProperties memProps{};
    vkGetPhysicalDeviceMemoryProperties(m_context.physicalDevice, &memProps);
    for (uint32_t i = 0; i < memProps.memoryTypeCount; ++i) {
        if ((typeBits & (1u << i)) &&
            (memProps.memoryTypes[i].propertyFlags & properties) == properties) {
            return i;
        }
    }
    return UINT32_MAX;
}

HardwareInteropProbeResult VulkanDetectorFrameHandoff::probeHardwareInterop(const editor::FrameHandle& frame) const
{
    HardwareInteropProbeResult result;
    if (!m_initialized) {
        result.reason = QStringLiteral("handoff module is not initialized");
        return result;
    }
    if (frame.isNull() || !frame.hasHardwareFrame()) {
        result.reason = QStringLiteral("decoder frame has no hardware surface");
        return result;
    }
    const int hwFmt = frame.hardwarePixelFormat();
    if (hwFmt == AV_PIX_FMT_CUDA) {
        result.path = QStringLiteral("cuda");
    } else if (hwFmt == AV_PIX_FMT_VAAPI) {
        result.path = QStringLiteral("vaapi");
    } else {
        result.reason = QStringLiteral("unsupported hardware pixel format: %1").arg(hwFmt);
        return result;
    }

    uint32_t extensionCount = 0;
    vkEnumerateDeviceExtensionProperties(m_context.physicalDevice, nullptr, &extensionCount, nullptr);
    std::vector<VkExtensionProperties> exts(extensionCount);
    if (extensionCount) {
        vkEnumerateDeviceExtensionProperties(m_context.physicalDevice, nullptr, &extensionCount, exts.data());
    }
    auto hasExt = [&](const char* name) {
        return std::any_of(exts.begin(), exts.end(), [&](const VkExtensionProperties& ext) {
            return std::strcmp(ext.extensionName, name) == 0;
        });
    };
    if (!hasExt(VK_KHR_EXTERNAL_MEMORY_EXTENSION_NAME)) {
        result.reason = QStringLiteral("missing VK_KHR_external_memory");
        return result;
    }
#ifdef Q_OS_LINUX
    if (!hasExt(VK_KHR_EXTERNAL_MEMORY_FD_EXTENSION_NAME)) {
        result.reason = QStringLiteral("missing VK_KHR_external_memory_fd");
        return result;
    }
    if (!hasExt(VK_KHR_EXTERNAL_SEMAPHORE_EXTENSION_NAME)) {
        result.reason = QStringLiteral("missing VK_KHR_external_semaphore");
        return result;
    }
    if (!hasExt(VK_KHR_EXTERNAL_SEMAPHORE_FD_EXTENSION_NAME)) {
        result.reason = QStringLiteral("missing VK_KHR_external_semaphore_fd");
        return result;
    }
#endif
    if (!hasExt(VK_KHR_BIND_MEMORY_2_EXTENSION_NAME)) {
        result.reason = QStringLiteral("missing VK_KHR_bind_memory2");
        return result;
    }
    const int swFmt = frame.hardwareSwPixelFormat();
    const bool directSampleable =
        (swFmt == AV_PIX_FMT_RGBA || swFmt == AV_PIX_FMT_BGRA || swFmt == AV_PIX_FMT_RGB0 ||
         swFmt == AV_PIX_FMT_BGR0 || swFmt == AV_PIX_FMT_NV12);
    if (!directSampleable) {
        result.reason = QStringLiteral("hardware interop available but detector direct path is unsupported (sw_fmt=%1)")
                            .arg(swFmt);
        return result;
    }
    result.supported = true;
    result.reason = QStringLiteral("hardware interop prerequisites satisfied");
    return result;
}

} // namespace jcut::vulkan_detector
