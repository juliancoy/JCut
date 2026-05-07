#include "vulkan_detector_frame_handoff.h"

#include <QElapsedTimer>
#include <QScopeGuard>

#include <algorithm>
#include <cstring>
#include <vector>

#if !defined(JCUT_HAS_CUDA_DRIVER)
#define JCUT_HAS_CUDA_DRIVER 0
#endif

extern "C" {
#include <libavutil/hwcontext.h>
#include <libavutil/hwcontext_cuda.h>
#include <libavutil/pixfmt.h>
}

#if JCUT_HAS_CUDA_DRIVER
#include <cuda.h>
#include <unistd.h>
#endif

namespace jcut::vulkan_detector {

VulkanDetectorFrameHandoff::VulkanDetectorFrameHandoff() = default;

VulkanDetectorFrameHandoff::~VulkanDetectorFrameHandoff()
{
    release();
}

bool VulkanDetectorFrameHandoff::initialize(const VulkanDeviceContext& context, QString* errorMessage)
{
    release();
    if (context.physicalDevice == VK_NULL_HANDLE ||
        context.device == VK_NULL_HANDLE ||
        context.queue == VK_NULL_HANDLE ||
        context.queueFamilyIndex == UINT32_MAX) {
        if (errorMessage) *errorMessage = QStringLiteral("Invalid Vulkan device context for frame handoff.");
        return false;
    }
    m_context = context;

    VkCommandPoolCreateInfo poolInfo{};
    poolInfo.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
    poolInfo.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    poolInfo.queueFamilyIndex = m_context.queueFamilyIndex;
    if (vkCreateCommandPool(m_context.device, &poolInfo, nullptr, &m_commandPool) != VK_SUCCESS) {
        if (errorMessage) *errorMessage = QStringLiteral("Failed to create frame handoff command pool.");
        release();
        return false;
    }

    VkCommandBufferAllocateInfo allocInfo{};
    allocInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    allocInfo.commandPool = m_commandPool;
    allocInfo.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    allocInfo.commandBufferCount = 1;
    if (vkAllocateCommandBuffers(m_context.device, &allocInfo, &m_commandBuffer) != VK_SUCCESS) {
        if (errorMessage) *errorMessage = QStringLiteral("Failed to allocate frame handoff command buffer.");
        release();
        return false;
    }

    VkFenceCreateInfo fenceInfo{};
    fenceInfo.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
    if (vkCreateFence(m_context.device, &fenceInfo, nullptr, &m_fence) != VK_SUCCESS) {
        if (errorMessage) *errorMessage = QStringLiteral("Failed to create frame handoff fence.");
        release();
        return false;
    }

    m_initialized = true;
    return true;
}

void VulkanDetectorFrameHandoff::release()
{
    if (m_context.device != VK_NULL_HANDLE) {
        if (m_cudaExternalMemory) {
#if JCUT_HAS_CUDA_DRIVER
            CUcontext previous = nullptr;
            if (m_cudaImportContext) {
                cuCtxPushCurrent(reinterpret_cast<CUcontext>(m_cudaImportContext));
            }
            cuDestroyExternalMemory(reinterpret_cast<CUexternalMemory>(m_cudaExternalMemory));
            if (m_cudaImportContext) {
                cuCtxPopCurrent(&previous);
            }
#endif
            m_cudaExternalMemory = nullptr;
            m_cudaExternalDevicePtr = 0;
            m_cudaImportContext = nullptr;
        }
        destroyBuffer(m_cudaExportBuffer, m_cudaExportMemory);
        m_cudaExportSize = 0;
        if (m_imageView != VK_NULL_HANDLE) vkDestroyImageView(m_context.device, m_imageView, nullptr);
        if (m_image != VK_NULL_HANDLE) vkDestroyImage(m_context.device, m_image, nullptr);
        if (m_imageMemory != VK_NULL_HANDLE) vkFreeMemory(m_context.device, m_imageMemory, nullptr);
        destroyBuffer(m_stagingBuffer, m_stagingMemory);
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
    m_imageSize = QSize();
    m_stagingSize = 0;
    m_initialized = false;
    m_lastMode = FrameHandoffMode::Invalid;
    m_context = VulkanDeviceContext{};
}

bool VulkanDetectorFrameHandoff::ensureImageResources(const QSize& size, QString* errorMessage)
{
    if (m_image != VK_NULL_HANDLE && m_imageSize == size) {
        return true;
    }
    if (m_imageView != VK_NULL_HANDLE) vkDestroyImageView(m_context.device, m_imageView, nullptr);
    if (m_image != VK_NULL_HANDLE) vkDestroyImage(m_context.device, m_image, nullptr);
    if (m_imageMemory != VK_NULL_HANDLE) vkFreeMemory(m_context.device, m_imageMemory, nullptr);
    m_image = VK_NULL_HANDLE;
    m_imageMemory = VK_NULL_HANDLE;
    m_imageView = VK_NULL_HANDLE;
    m_imageLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    m_imageSize = size;

    VkImageCreateInfo imageInfo{};
    imageInfo.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    imageInfo.imageType = VK_IMAGE_TYPE_2D;
    imageInfo.extent = {static_cast<uint32_t>(size.width()), static_cast<uint32_t>(size.height()), 1};
    imageInfo.mipLevels = 1;
    imageInfo.arrayLayers = 1;
    imageInfo.format = VK_FORMAT_R8G8B8A8_UNORM;
    imageInfo.tiling = VK_IMAGE_TILING_OPTIMAL;
    imageInfo.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    imageInfo.usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
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
    VkImageViewCreateInfo viewInfo{};
    viewInfo.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    viewInfo.image = m_image;
    viewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
    viewInfo.format = VK_FORMAT_R8G8B8A8_UNORM;
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
    destroyBuffer(m_stagingBuffer, m_stagingMemory);
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
    m_stagingSize = bytes;
    return true;
}

void VulkanDetectorFrameHandoff::transitionImage(VkImageLayout oldLayout, VkImageLayout newLayout)
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
        src = VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT;
    }
    if (newLayout == VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL) {
        barrier.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    } else if (newLayout == VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL) {
        barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
        src = VK_PIPELINE_STAGE_TRANSFER_BIT;
        dst = VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT;
    }
    vkCmdPipelineBarrier(m_commandBuffer, src, dst, 0, 0, nullptr, 0, nullptr, 1, &barrier);
}

bool VulkanDetectorFrameHandoff::uploadFrame(const editor::FrameHandle& frame,
                                             bool allowCpuUploadFallback,
                                             double* uploadMs,
                                             QString* errorMessage)
{
    if (!m_initialized || frame.isNull()) {
        if (errorMessage) *errorMessage = QStringLiteral("Invalid frame handoff upload state.");
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
            *errorMessage = QStringLiteral("Hardware direct handoff attempt failed: %1").arg(directError);
        } else if (errorMessage) {
            *errorMessage = QStringLiteral("Hardware direct handoff attempt failed.");
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
            *errorMessage = QStringLiteral(
                "CPU upload fallback is disabled; hardware-direct handoff was not used: %1").arg(reason);
        }
        m_lastMode = FrameHandoffMode::Invalid;
        return false;
    }
    if (!frame.hasCpuImage()) {
        if (errorMessage) {
            *errorMessage = QStringLiteral(
                "Direct hardware frame handoff is not implemented for this path; CPU image is required.");
        }
        m_lastMode = FrameHandoffMode::Invalid;
        return false;
    }
    const QImage rgba = frame.cpuImage().convertToFormat(QImage::Format_RGBA8888);
    if (rgba.isNull()) {
        if (errorMessage) *errorMessage = QStringLiteral("Failed to materialize RGBA upload image.");
        m_lastMode = FrameHandoffMode::Invalid;
        return false;
    }
    const VkDeviceSize bytes = static_cast<VkDeviceSize>(rgba.width()) * rgba.height() * 4;
    if (!ensureStagingBuffer(bytes, errorMessage) || !ensureImageResources(rgba.size(), errorMessage)) {
        m_lastMode = FrameHandoffMode::Invalid;
        return false;
    }
    QElapsedTimer timer;
    timer.start();
    void* mapped = nullptr;
    if (vkMapMemory(m_context.device, m_stagingMemory, 0, bytes, 0, &mapped) != VK_SUCCESS || !mapped) {
        if (errorMessage) *errorMessage = QStringLiteral("Failed to map Vulkan handoff staging memory.");
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
    vkQueueSubmit(m_context.queue, 1, &submit, m_fence);
    vkWaitForFences(m_context.device, 1, &m_fence, VK_TRUE, 5'000'000'000ull);
    m_imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    m_lastMode = FrameHandoffMode::CpuUpload;
    if (uploadMs) {
        *uploadMs = static_cast<double>(timer.nsecsElapsed()) / 1'000'000.0;
    }
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
    const QSize size = frame.size();
    if (!size.isValid()) {
        if (errorMessage) *errorMessage = QStringLiteral("invalid frame size for hardware-direct");
        return false;
    }
    if (!ensureImageResources(size, errorMessage)) {
        return false;
    }

    const VkDeviceSize bytes = static_cast<VkDeviceSize>(size.width()) * size.height() * 4;
    PFN_vkGetMemoryFdKHR vkGetMemoryFdKHR = reinterpret_cast<PFN_vkGetMemoryFdKHR>(
        vkGetDeviceProcAddr(m_context.device, "vkGetMemoryFdKHR"));
    if (!vkGetMemoryFdKHR) {
        if (errorMessage) *errorMessage = QStringLiteral("vkGetMemoryFdKHR unavailable");
        return false;
    }
    if (m_cudaExportBuffer == VK_NULL_HANDLE || m_cudaExportSize < bytes) {
        if (m_cudaExternalMemory) {
            CUcontext previous = nullptr;
            if (m_cudaImportContext) {
                cuCtxPushCurrent(reinterpret_cast<CUcontext>(m_cudaImportContext));
            }
            cuDestroyExternalMemory(reinterpret_cast<CUexternalMemory>(m_cudaExternalMemory));
            if (m_cudaImportContext) {
                cuCtxPopCurrent(&previous);
            }
            m_cudaExternalMemory = nullptr;
            m_cudaExternalDevicePtr = 0;
            m_cudaImportContext = nullptr;
        }
        destroyBuffer(m_cudaExportBuffer, m_cudaExportMemory);
        VkExternalMemoryBufferCreateInfo extBuf{};
        extBuf.sType = VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_BUFFER_CREATE_INFO;
        extBuf.handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_FD_BIT;
        VkBufferCreateInfo bi{};
        bi.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
        bi.pNext = &extBuf;
        bi.size = bytes;
        bi.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
        bi.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
        if (vkCreateBuffer(m_context.device, &bi, nullptr, &m_cudaExportBuffer) != VK_SUCCESS) {
            if (errorMessage) *errorMessage = QStringLiteral("failed to create CUDA-export Vulkan buffer");
            return false;
        }
        VkMemoryRequirements req{};
        vkGetBufferMemoryRequirements(m_context.device, m_cudaExportBuffer, &req);
        const uint32_t memType = findMemoryType(req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
        if (memType == UINT32_MAX) {
            if (errorMessage) *errorMessage = QStringLiteral("no device-local memory for CUDA-export Vulkan buffer");
            return false;
        }
        VkExportMemoryAllocateInfo exportAllocInfo{};
        exportAllocInfo.sType = VK_STRUCTURE_TYPE_EXPORT_MEMORY_ALLOCATE_INFO;
        exportAllocInfo.handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_FD_BIT;
        VkMemoryAllocateInfo ai{};
        ai.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
        ai.pNext = &exportAllocInfo;
        ai.allocationSize = req.size;
        ai.memoryTypeIndex = memType;
        if (vkAllocateMemory(m_context.device, &ai, nullptr, &m_cudaExportMemory) != VK_SUCCESS ||
            vkBindBufferMemory(m_context.device, m_cudaExportBuffer, m_cudaExportMemory, 0) != VK_SUCCESS) {
            if (errorMessage) *errorMessage = QStringLiteral("failed to allocate/bind CUDA-export Vulkan memory");
            return false;
        }
        m_cudaExportSize = req.size;
    }

    CUcontext previous = nullptr;
    if (cuInit(0) != CUDA_SUCCESS || cuCtxPushCurrent(cudaContext) != CUDA_SUCCESS) {
        if (errorMessage) *errorMessage = QStringLiteral("failed to activate CUDA context for hardware-direct");
        return false;
    }
    auto pop = qScopeGuard([&]() { cuCtxPopCurrent(&previous); });

    if (m_cudaExternalMemory && m_cudaImportContext != cudaContext) {
        cuDestroyExternalMemory(reinterpret_cast<CUexternalMemory>(m_cudaExternalMemory));
        m_cudaExternalMemory = nullptr;
        m_cudaExternalDevicePtr = 0;
        m_cudaImportContext = nullptr;
    }
    if (!m_cudaExternalMemory || !m_cudaExternalDevicePtr) {
        VkMemoryGetFdInfoKHR fdInfo{};
        fdInfo.sType = VK_STRUCTURE_TYPE_MEMORY_GET_FD_INFO_KHR;
        fdInfo.memory = m_cudaExportMemory;
        fdInfo.handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_FD_BIT;
        int fd = -1;
        if (vkGetMemoryFdKHR(m_context.device, &fdInfo, &fd) != VK_SUCCESS || fd < 0) {
            if (errorMessage) *errorMessage = QStringLiteral("failed to export Vulkan memory FD for CUDA import");
            return false;
        }
        CUDA_EXTERNAL_MEMORY_HANDLE_DESC handleDesc{};
        handleDesc.type = CU_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_FD;
        handleDesc.handle.fd = fd;
        handleDesc.size = m_cudaExportSize;
        CUexternalMemory extMem = nullptr;
        if (cuImportExternalMemory(&extMem, &handleDesc) != CUDA_SUCCESS) {
            close(fd);
            if (errorMessage) *errorMessage = QStringLiteral("cuImportExternalMemory failed");
            return false;
        }
        CUDA_EXTERNAL_MEMORY_BUFFER_DESC bufDesc{};
        bufDesc.offset = 0;
        bufDesc.size = m_cudaExportSize;
        CUdeviceptr devPtr = 0;
        if (cuExternalMemoryGetMappedBuffer(&devPtr, extMem, &bufDesc) != CUDA_SUCCESS) {
            cuDestroyExternalMemory(extMem);
            if (errorMessage) *errorMessage = QStringLiteral("cuExternalMemoryGetMappedBuffer failed");
            return false;
        }
        m_cudaExternalMemory = extMem;
        m_cudaExternalDevicePtr = static_cast<quint64>(devPtr);
        m_cudaImportContext = cudaContext;
    }

    if (isNv12Like) {
        if (errorMessage) {
            *errorMessage = QStringLiteral(
                "NV12/P010 CUDA hardware-direct path is pending: conversion shaders exist (nv12_*/yuv420p_*) but are not yet wired into VulkanDetectorFrameHandoff.");
        }
        return false;
    }

    const size_t widthBytes = static_cast<size_t>(size.width()) * 4;
    CUDA_MEMCPY2D copy{};
    copy.srcMemoryType = CU_MEMORYTYPE_DEVICE;
    copy.srcDevice = reinterpret_cast<CUdeviceptr>(hw->data[0]);
    copy.srcPitch = static_cast<size_t>(hw->linesize[0]);
    copy.dstMemoryType = CU_MEMORYTYPE_DEVICE;
    copy.dstDevice = static_cast<CUdeviceptr>(m_cudaExternalDevicePtr);
    copy.dstPitch = widthBytes;
    copy.WidthInBytes = widthBytes;
    copy.Height = static_cast<size_t>(size.height());
    QElapsedTimer timer;
    timer.start();
    if (cuMemcpy2DAsync(&copy, cudaDevice->stream) != CUDA_SUCCESS ||
        cuStreamSynchronize(cudaDevice->stream) != CUDA_SUCCESS) {
        if (errorMessage) *errorMessage = QStringLiteral("CUDA device copy into Vulkan-export buffer failed");
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
    transitionImage(m_imageLayout, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
    VkBufferImageCopy bi{};
    bi.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    bi.imageSubresource.layerCount = 1;
    bi.imageExtent = {static_cast<uint32_t>(size.width()), static_cast<uint32_t>(size.height()), 1};
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
    VkSubmitInfo submit{};
    submit.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    submit.commandBufferCount = 1;
    submit.pCommandBuffers = &m_commandBuffer;
    if (vkQueueSubmit(m_context.queue, 1, &submit, m_fence) != VK_SUCCESS ||
        vkWaitForFences(m_context.device, 1, &m_fence, VK_TRUE, 5'000'000'000ull) != VK_SUCCESS) {
        if (errorMessage) *errorMessage = QStringLiteral("Vulkan submit/wait failed for hardware-direct");
        return false;
    }
    m_imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    if (uploadMs) {
        *uploadMs = static_cast<double>(timer.nsecsElapsed()) / 1'000'000.0;
    }
    return true;
#endif
}

VulkanExternalImage VulkanDetectorFrameHandoff::externalImage() const
{
    VulkanExternalImage image;
    image.imageView = m_imageView;
    image.imageLayout = m_imageLayout;
    image.size = m_imageSize;
    image.sourceIsSrgb = false;
    return image;
}

void VulkanDetectorFrameHandoff::destroyBuffer(VkBuffer& buffer, VkDeviceMemory& memory)
{
    if (buffer != VK_NULL_HANDLE) vkDestroyBuffer(m_context.device, buffer, nullptr);
    if (memory != VK_NULL_HANDLE) vkFreeMemory(m_context.device, memory, nullptr);
    buffer = VK_NULL_HANDLE;
    memory = VK_NULL_HANDLE;
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
#endif
    if (!hasExt(VK_KHR_BIND_MEMORY_2_EXTENSION_NAME)) {
        result.reason = QStringLiteral("missing VK_KHR_bind_memory2");
        return result;
    }
    const int swFmt = frame.hardwareSwPixelFormat();
    const bool directRgbaSampleable =
        (swFmt == AV_PIX_FMT_RGBA || swFmt == AV_PIX_FMT_BGRA || swFmt == AV_PIX_FMT_RGB0 || swFmt == AV_PIX_FMT_BGR0);
    if (!directRgbaSampleable) {
        result.reason = QStringLiteral("hardware interop available but detector direct-sampling format is unsupported (sw_fmt=%1)")
                            .arg(swFmt);
        return result;
    }
    result.supported = true;
    result.reason = QStringLiteral("hardware interop prerequisites satisfied");
    return result;
}

} // namespace jcut::vulkan_detector
