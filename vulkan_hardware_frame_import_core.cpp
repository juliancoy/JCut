#include "vulkan_hardware_frame_import_core.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <utility>
#include <vector>

#if !defined(JCUT_HAS_CUDA_DRIVER)
#define JCUT_HAS_CUDA_DRIVER 0
#endif

#if JCUT_HAS_CUDA_DRIVER
#include <cuda.h>
#include <unistd.h>
#endif

extern "C" {
#include <libavutil/frame.h>
#include <libavutil/hwcontext.h>
#include <libavutil/hwcontext_cuda.h>
#include <libavutil/pixfmt.h>
}

namespace jcut::vulkan_import {

namespace {

bool hasExtension(VkPhysicalDevice device, const char* name)
{
    std::uint32_t count = 0;
    vkEnumerateDeviceExtensionProperties(device, nullptr, &count, nullptr);
    std::vector<VkExtensionProperties> properties(count);
    if (count) {
        vkEnumerateDeviceExtensionProperties(
            device, nullptr, &count, properties.data());
    }
    return std::any_of(
        properties.begin(), properties.end(),
        [name](const VkExtensionProperties& property) {
            return std::strcmp(property.extensionName, name) == 0;
        });
}

std::vector<std::uint32_t> readSpirv(
    const std::filesystem::path& path)
{
    std::ifstream input(path, std::ios::binary | std::ios::ate);
    if (!input) return {};
    const std::streamsize byteCount = input.tellg();
    if (byteCount <= 0 ||
        byteCount % static_cast<std::streamsize>(sizeof(std::uint32_t)) != 0) {
        return {};
    }
    input.seekg(0);
    std::vector<std::uint32_t> words(
        static_cast<std::size_t>(byteCount) / sizeof(std::uint32_t));
    if (!input.read(
            reinterpret_cast<char*>(words.data()), byteCount)) {
        return {};
    }
    return words;
}

} // namespace

class VulkanHardwareFrameImportCore::Impl {
public:
    ~Impl() { release(); }

    bool initialize(VkPhysicalDevice physical,
                    VkDevice logical,
                    VkQueue submitQueue,
                    std::uint32_t family,
                    std::string shaders,
                    std::string* error)
    {
        release();
        if (physical == VK_NULL_HANDLE ||
            logical == VK_NULL_HANDLE ||
            submitQueue == VK_NULL_HANDLE ||
            family == UINT32_MAX) {
            return fail(error, "invalid Vulkan hardware-frame import context");
        }
        physicalDevice = physical;
        device = logical;
        queue = submitQueue;
        queueFamily = family;
        shaderDirectory = std::move(shaders);

        if (!hasExtension(
                physicalDevice,
                VK_KHR_EXTERNAL_MEMORY_EXTENSION_NAME) ||
            !hasExtension(
                physicalDevice,
                VK_KHR_EXTERNAL_MEMORY_FD_EXTENSION_NAME)) {
            release();
            return fail(
                error,
                "Vulkan device lacks opaque-FD external-memory support");
        }
        getMemoryFd = reinterpret_cast<PFN_vkGetMemoryFdKHR>(
            vkGetDeviceProcAddr(device, "vkGetMemoryFdKHR"));
        if (!getMemoryFd) {
            release();
            return fail(error, "vkGetMemoryFdKHR is unavailable");
        }

        VkCommandPoolCreateInfo poolInfo{};
        poolInfo.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
        poolInfo.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
        poolInfo.queueFamilyIndex = queueFamily;
        if (vkCreateCommandPool(
                device, &poolInfo, nullptr, &commandPool) != VK_SUCCESS) {
            release();
            return fail(error, "failed to create hardware-frame command pool");
        }
        VkCommandBufferAllocateInfo commandInfo{};
        commandInfo.sType =
            VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
        commandInfo.commandPool = commandPool;
        commandInfo.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        commandInfo.commandBufferCount = 1;
        if (vkAllocateCommandBuffers(
                device, &commandInfo, &commandBuffer) != VK_SUCCESS) {
            release();
            return fail(error, "failed to allocate hardware-frame command buffer");
        }
        VkFenceCreateInfo fenceInfo{};
        fenceInfo.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
        fenceInfo.flags = VK_FENCE_CREATE_SIGNALED_BIT;
        if (vkCreateFence(
                device, &fenceInfo, nullptr, &fence) != VK_SUCCESS) {
            release();
            return fail(error, "failed to create hardware-frame fence");
        }
        initialized = true;
        return true;
    }

    VulkanHardwareFrameImportStatus probe(
        const core::FramePayloadCore& payload) const
    {
        VulkanHardwareFrameImportStatus result;
        result.hardwarePixelFormat = payload.hardwarePixelFormat();
        result.softwarePixelFormat =
            payload.hardwareSoftwarePixelFormat();
        if (!initialized) {
            result.reason = "hardware-frame importer is not initialized";
            return result;
        }
        if (!payload.hasHardwareFrame()) {
            result.reason = "decoder payload has no retained hardware frame";
            return result;
        }
        if (result.hardwarePixelFormat != AV_PIX_FMT_CUDA) {
            result.reason =
                result.hardwarePixelFormat == AV_PIX_FMT_VAAPI
                ? "VA-API Vulkan import is not implemented"
                : "unsupported FFmpeg hardware pixel format";
            return result;
        }
        result.path = "cuda_external_memory";
#if !JCUT_HAS_CUDA_DRIVER
        result.reason = "CUDA driver interop was not compiled";
        return result;
#else
        if (result.softwarePixelFormat != AV_PIX_FMT_NV12 &&
            result.softwarePixelFormat != AV_PIX_FMT_RGBA &&
            result.softwarePixelFormat != AV_PIX_FMT_BGRA &&
            result.softwarePixelFormat != AV_PIX_FMT_RGB0 &&
            result.softwarePixelFormat != AV_PIX_FMT_BGR0) {
            result.reason =
                "CUDA surface software format is not NV12 or RGBA/BGRA";
            return result;
        }
        result.supported = true;
        result.reason = "CUDA/Vulkan external-memory path available";
        return result;
#endif
    }

    bool importFrame(
        const core::FramePayloadCore& payload,
        const HardwareFrameColorGradeCore& grade,
        std::string* error)
    {
        status = probe(payload);
        if (!status.supported) {
            return fail(error, status.reason);
        }
#if !JCUT_HAS_CUDA_DRIVER
        return fail(error, "CUDA driver interop was not compiled");
#else
        if (!waitIdle(error)) return false;
        const AVFrame* frame = payload.hardwareFrame();
        if (!frame || frame->format != AV_PIX_FMT_CUDA ||
            !frame->hw_frames_ctx || !frame->hw_frames_ctx->data) {
            return fail(error, "invalid retained CUDA AVFrame");
        }
        auto* frames = reinterpret_cast<AVHWFramesContext*>(
            frame->hw_frames_ctx->data);
        auto* cuda = frames && frames->device_ctx
            ? reinterpret_cast<AVCUDADeviceContext*>(
                  frames->device_ctx->hwctx)
            : nullptr;
        if (!cuda || !cuda->cuda_ctx) {
            return fail(error, "retained frame has no CUDA device context");
        }
        const bool nv12 =
            payload.hardwareSoftwarePixelFormat() == AV_PIX_FMT_NV12;
        const bool bgra =
            payload.hardwareSoftwarePixelFormat() == AV_PIX_FMT_BGRA ||
            payload.hardwareSoftwarePixelFormat() == AV_PIX_FMT_BGR0;
        const core::SizeI size = payload.size();
        if (!size.valid()) {
            return fail(error, "retained hardware frame has invalid size");
        }
        if (!ensureImage(
                size,
                bgra ? VK_FORMAT_B8G8R8A8_UNORM
                     : VK_FORMAT_R8G8B8A8_UNORM,
                error)) {
            return false;
        }
        const bool colorGradeIdentity =
            !grade.curvesEnabled &&
            std::abs(grade.brightness) < 0.000001f &&
            std::abs(grade.contrast - 1.0f) < 0.000001f &&
            std::abs(grade.saturation - 1.0f) < 0.000001f &&
            std::abs(grade.shadowsR) < 0.000001f &&
            std::abs(grade.shadowsG) < 0.000001f &&
            std::abs(grade.shadowsB) < 0.000001f &&
            std::abs(grade.midtonesR) < 0.000001f &&
            std::abs(grade.midtonesG) < 0.000001f &&
            std::abs(grade.midtonesB) < 0.000001f &&
            std::abs(grade.highlightsR) < 0.000001f &&
            std::abs(grade.highlightsG) < 0.000001f &&
            std::abs(grade.highlightsB) < 0.000001f;
        if (!nv12 && !colorGradeIdentity) {
            return fail(
                error,
                "direct color grading currently requires an NV12 CUDA surface");
        }

        const int yPitch = nv12
            ? frame->linesize[0]
            : size.width * 4;
        const int uvPitch = frame->linesize[1] > 0
            ? frame->linesize[1]
            : frame->linesize[0];
        const VkDeviceSize yBytes = nv12
            ? static_cast<VkDeviceSize>(yPitch) * size.height
            : static_cast<VkDeviceSize>(size.width) * size.height * 4;
        const VkDeviceSize uvBytes = nv12
            ? static_cast<VkDeviceSize>(uvPitch) *
                  ((size.height + 1) / 2)
            : 0;
        if (!ensureCudaBuffer(
                yBytes, &yBuffer, &yMemory, &yAllocationSize, error) ||
            (nv12 && !ensureCudaBuffer(
                 uvBytes,
                 &uvBuffer,
                 &uvMemory,
                 &uvAllocationSize,
                 error))) {
            return false;
        }
        if (!activateCudaContext(cuda->cuda_ctx, error) ||
            !ensureCudaMapping(
                yMemory,
                yAllocationSize,
                cuda->cuda_ctx,
                &yExternalMemory,
                &yDevicePointer,
                error) ||
            (nv12 && !ensureCudaMapping(
                 uvMemory,
                 uvAllocationSize,
                 cuda->cuda_ctx,
                 &uvExternalMemory,
                 &uvDevicePointer,
                 error))) {
            popCudaContext();
            return false;
        }

        bool copied = false;
        if (nv12) {
            CUDA_MEMCPY2D yCopy{};
            yCopy.srcMemoryType = CU_MEMORYTYPE_DEVICE;
            yCopy.srcDevice =
                reinterpret_cast<CUdeviceptr>(frame->data[0]);
            yCopy.srcPitch = static_cast<std::size_t>(frame->linesize[0]);
            yCopy.dstMemoryType = CU_MEMORYTYPE_DEVICE;
            yCopy.dstDevice = yDevicePointer;
            yCopy.dstPitch = static_cast<std::size_t>(yPitch);
            yCopy.WidthInBytes = static_cast<std::size_t>(size.width);
            yCopy.Height = static_cast<std::size_t>(size.height);

            CUDA_MEMCPY2D uvCopy{};
            uvCopy.srcMemoryType = CU_MEMORYTYPE_DEVICE;
            uvCopy.srcDevice = frame->data[1]
                ? reinterpret_cast<CUdeviceptr>(frame->data[1])
                : reinterpret_cast<CUdeviceptr>(
                      frame->data[0] +
                      static_cast<std::size_t>(frame->linesize[0]) *
                          size.height);
            uvCopy.srcPitch = static_cast<std::size_t>(uvPitch);
            uvCopy.dstMemoryType = CU_MEMORYTYPE_DEVICE;
            uvCopy.dstDevice = uvDevicePointer;
            uvCopy.dstPitch = static_cast<std::size_t>(uvPitch);
            uvCopy.WidthInBytes = static_cast<std::size_t>(size.width);
            uvCopy.Height =
                static_cast<std::size_t>((size.height + 1) / 2);
            copied =
                cuMemcpy2DAsync(&yCopy, cuda->stream) == CUDA_SUCCESS &&
                cuMemcpy2DAsync(&uvCopy, cuda->stream) == CUDA_SUCCESS &&
                cuStreamSynchronize(cuda->stream) == CUDA_SUCCESS;
        } else {
            CUDA_MEMCPY2D copy{};
            copy.srcMemoryType = CU_MEMORYTYPE_DEVICE;
            copy.srcDevice =
                reinterpret_cast<CUdeviceptr>(frame->data[0]);
            copy.srcPitch =
                static_cast<std::size_t>(frame->linesize[0]);
            copy.dstMemoryType = CU_MEMORYTYPE_DEVICE;
            copy.dstDevice = yDevicePointer;
            copy.dstPitch =
                static_cast<std::size_t>(size.width) * 4;
            copy.WidthInBytes =
                static_cast<std::size_t>(size.width) * 4;
            copy.Height = static_cast<std::size_t>(size.height);
            copied =
                cuMemcpy2DAsync(&copy, cuda->stream) == CUDA_SUCCESS &&
                cuStreamSynchronize(cuda->stream) == CUDA_SUCCESS;
        }
        popCudaContext();
        if (!copied) {
            return fail(
                error,
                "CUDA device copy into Vulkan external memory failed");
        }
        if (nv12) {
            if (!recordNv12(
                    size,
                    yPitch,
                    uvPitch,
                    frame,
                    grade,
                    error)) {
                return false;
            }
        } else if (!recordRgba(size, error)) {
            return false;
        }
        status.hardwareDirect = true;
        status.reason =
            "CUDA frame presented through Vulkan external memory";
        return true;
#endif
    }

    ExternalImage externalImage() const
    {
        ExternalImage result;
        result.image = image;
        result.imageView = imageView;
        result.imageLayout = imageLayout;
        result.imageFormat = imageFormat;
        result.size = imageSize;
        return result;
    }

    const VulkanHardwareFrameImportStatus& lastStatus() const
    {
        return status;
    }

    void release()
    {
        if (device != VK_NULL_HANDLE) {
            vkDeviceWaitIdle(device);
        }
#if JCUT_HAS_CUDA_DRIVER
        destroyCudaMapping(&yExternalMemory, &yDevicePointer);
        destroyCudaMapping(&uvExternalMemory, &uvDevicePointer);
        cudaContext = nullptr;
#endif
        destroyBuffer(&yBuffer, &yMemory);
        destroyBuffer(&uvBuffer, &uvMemory);
        destroyBuffer(&curveBuffer, &curveMemory);
        yAllocationSize = 0;
        uvAllocationSize = 0;
        curveAllocationSize = 0;
        if (device != VK_NULL_HANDLE) {
            if (pipeline != VK_NULL_HANDLE)
                vkDestroyPipeline(device, pipeline, nullptr);
            if (pipelineLayout != VK_NULL_HANDLE)
                vkDestroyPipelineLayout(device, pipelineLayout, nullptr);
            if (descriptorPool != VK_NULL_HANDLE)
                vkDestroyDescriptorPool(device, descriptorPool, nullptr);
            if (descriptorLayout != VK_NULL_HANDLE)
                vkDestroyDescriptorSetLayout(
                    device, descriptorLayout, nullptr);
            if (imageView != VK_NULL_HANDLE)
                vkDestroyImageView(device, imageView, nullptr);
            if (image != VK_NULL_HANDLE)
                vkDestroyImage(device, image, nullptr);
            if (imageMemory != VK_NULL_HANDLE)
                vkFreeMemory(device, imageMemory, nullptr);
            if (fence != VK_NULL_HANDLE)
                vkDestroyFence(device, fence, nullptr);
            if (commandPool != VK_NULL_HANDLE)
                vkDestroyCommandPool(device, commandPool, nullptr);
        }
        physicalDevice = VK_NULL_HANDLE;
        device = VK_NULL_HANDLE;
        queue = VK_NULL_HANDLE;
        queueFamily = UINT32_MAX;
        commandPool = VK_NULL_HANDLE;
        commandBuffer = VK_NULL_HANDLE;
        fence = VK_NULL_HANDLE;
        image = VK_NULL_HANDLE;
        imageMemory = VK_NULL_HANDLE;
        imageView = VK_NULL_HANDLE;
        imageLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        imageFormat = VK_FORMAT_UNDEFINED;
        imageSize = {};
        descriptorLayout = VK_NULL_HANDLE;
        descriptorPool = VK_NULL_HANDLE;
        descriptorSet = VK_NULL_HANDLE;
        pipelineLayout = VK_NULL_HANDLE;
        pipeline = VK_NULL_HANDLE;
        getMemoryFd = nullptr;
        initialized = false;
        status = {};
    }

private:
    static bool fail(std::string* error, std::string message)
    {
        if (error) *error = std::move(message);
        return false;
    }

    std::uint32_t memoryType(
        std::uint32_t bits,
        VkMemoryPropertyFlags flags) const
    {
        VkPhysicalDeviceMemoryProperties properties{};
        vkGetPhysicalDeviceMemoryProperties(
            physicalDevice, &properties);
        for (std::uint32_t index = 0;
             index < properties.memoryTypeCount;
             ++index) {
            if ((bits & (1u << index)) &&
                (properties.memoryTypes[index].propertyFlags & flags) ==
                    flags) {
                return index;
            }
        }
        return UINT32_MAX;
    }

    bool waitIdle(std::string* error)
    {
        if (vkWaitForFences(
                device,
                1,
                &fence,
                VK_TRUE,
                5'000'000'000ull) != VK_SUCCESS) {
            return fail(error, "timed out waiting for hardware-frame import");
        }
        return true;
    }

    bool ensureImage(
        core::SizeI size,
        VkFormat format,
        std::string* error)
    {
        if (image != VK_NULL_HANDLE &&
            imageSize == size &&
            imageFormat == format) {
            return true;
        }
        if (imageView != VK_NULL_HANDLE)
            vkDestroyImageView(device, imageView, nullptr);
        if (image != VK_NULL_HANDLE)
            vkDestroyImage(device, image, nullptr);
        if (imageMemory != VK_NULL_HANDLE)
            vkFreeMemory(device, imageMemory, nullptr);
        imageView = VK_NULL_HANDLE;
        image = VK_NULL_HANDLE;
        imageMemory = VK_NULL_HANDLE;
        imageLayout = VK_IMAGE_LAYOUT_UNDEFINED;

        VkImageCreateInfo info{};
        info.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
        info.imageType = VK_IMAGE_TYPE_2D;
        info.extent = {
            static_cast<std::uint32_t>(size.width),
            static_cast<std::uint32_t>(size.height),
            1};
        info.mipLevels = 1;
        info.arrayLayers = 1;
        info.format = format;
        info.tiling = VK_IMAGE_TILING_OPTIMAL;
        info.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        info.usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT |
            VK_IMAGE_USAGE_STORAGE_BIT |
            VK_IMAGE_USAGE_SAMPLED_BIT;
        info.samples = VK_SAMPLE_COUNT_1_BIT;
        info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
        if (vkCreateImage(
                device, &info, nullptr, &image) != VK_SUCCESS) {
            return fail(error, "failed to create hardware-frame Vulkan image");
        }
        VkMemoryRequirements requirements{};
        vkGetImageMemoryRequirements(
            device, image, &requirements);
        const std::uint32_t type = memoryType(
            requirements.memoryTypeBits,
            VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
        VkMemoryAllocateInfo allocation{};
        allocation.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
        allocation.allocationSize = requirements.size;
        allocation.memoryTypeIndex = type;
        if (type == UINT32_MAX ||
            vkAllocateMemory(
                device,
                &allocation,
                nullptr,
                &imageMemory) != VK_SUCCESS ||
            vkBindImageMemory(
                device, image, imageMemory, 0) != VK_SUCCESS) {
            return fail(error, "failed to allocate hardware-frame Vulkan image");
        }
        VkImageViewCreateInfo view{};
        view.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
        view.image = image;
        view.viewType = VK_IMAGE_VIEW_TYPE_2D;
        view.format = format;
        view.subresourceRange.aspectMask =
            VK_IMAGE_ASPECT_COLOR_BIT;
        view.subresourceRange.levelCount = 1;
        view.subresourceRange.layerCount = 1;
        if (vkCreateImageView(
                device, &view, nullptr, &imageView) != VK_SUCCESS) {
            return fail(error, "failed to create hardware-frame Vulkan view");
        }
        imageSize = size;
        imageFormat = format;
        return true;
    }

    bool ensureCudaBuffer(
        VkDeviceSize requested,
        VkBuffer* buffer,
        VkDeviceMemory* memory,
        VkDeviceSize* allocationSize,
        std::string* error)
    {
        if (*buffer != VK_NULL_HANDLE &&
            *memory != VK_NULL_HANDLE &&
            *allocationSize >= requested) {
            return true;
        }
#if JCUT_HAS_CUDA_DRIVER
        if (buffer == &yBuffer)
            destroyCudaMapping(&yExternalMemory, &yDevicePointer);
        else
            destroyCudaMapping(&uvExternalMemory, &uvDevicePointer);
#endif
        destroyBuffer(buffer, memory);
        *allocationSize = 0;

        VkExternalMemoryBufferCreateInfo external{};
        external.sType =
            VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_BUFFER_CREATE_INFO;
        external.handleTypes =
            VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_FD_BIT;
        VkBufferCreateInfo info{};
        info.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
        info.pNext = &external;
        info.size = requested;
        info.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT |
            VK_BUFFER_USAGE_STORAGE_BUFFER_BIT;
        info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
        if (vkCreateBuffer(
                device, &info, nullptr, buffer) != VK_SUCCESS) {
            return fail(error, "failed to create CUDA-export Vulkan buffer");
        }
        VkMemoryRequirements requirements{};
        vkGetBufferMemoryRequirements(
            device, *buffer, &requirements);
        const std::uint32_t type = memoryType(
            requirements.memoryTypeBits,
            VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
        VkExportMemoryAllocateInfo exportInfo{};
        exportInfo.sType =
            VK_STRUCTURE_TYPE_EXPORT_MEMORY_ALLOCATE_INFO;
        exportInfo.handleTypes =
            VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_FD_BIT;
        VkMemoryAllocateInfo allocation{};
        allocation.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
        allocation.pNext = &exportInfo;
        allocation.allocationSize = requirements.size;
        allocation.memoryTypeIndex = type;
        if (type == UINT32_MAX ||
            vkAllocateMemory(
                device, &allocation, nullptr, memory) != VK_SUCCESS ||
            vkBindBufferMemory(
                device, *buffer, *memory, 0) != VK_SUCCESS) {
            destroyBuffer(buffer, memory);
            return fail(error, "failed to allocate CUDA-export Vulkan memory");
        }
        *allocationSize = requirements.size;
        return true;
    }

    bool ensureHostBuffer(
        VkDeviceSize requested,
        VkBuffer* buffer,
        VkDeviceMemory* memory,
        VkDeviceSize* allocationSize,
        std::string* error)
    {
        if (*buffer != VK_NULL_HANDLE &&
            *memory != VK_NULL_HANDLE &&
            *allocationSize >= requested) {
            return true;
        }
        destroyBuffer(buffer, memory);
        *allocationSize = 0;
        VkBufferCreateInfo info{};
        info.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
        info.size = requested;
        info.usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT;
        info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
        if (vkCreateBuffer(
                device, &info, nullptr, buffer) != VK_SUCCESS) {
            return fail(error, "failed to create grading LUT buffer");
        }
        VkMemoryRequirements requirements{};
        vkGetBufferMemoryRequirements(device, *buffer, &requirements);
        const std::uint32_t type = memoryType(
            requirements.memoryTypeBits,
            VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
        VkMemoryAllocateInfo allocation{};
        allocation.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
        allocation.allocationSize = requirements.size;
        allocation.memoryTypeIndex = type;
        if (type == UINT32_MAX ||
            vkAllocateMemory(
                device, &allocation, nullptr, memory) != VK_SUCCESS ||
            vkBindBufferMemory(
                device, *buffer, *memory, 0) != VK_SUCCESS) {
            destroyBuffer(buffer, memory);
            return fail(error, "failed to allocate grading LUT memory");
        }
        *allocationSize = requirements.size;
        return true;
    }

    void destroyBuffer(
        VkBuffer* buffer,
        VkDeviceMemory* memory)
    {
        if (device != VK_NULL_HANDLE && *buffer != VK_NULL_HANDLE)
            vkDestroyBuffer(device, *buffer, nullptr);
        if (device != VK_NULL_HANDLE && *memory != VK_NULL_HANDLE)
            vkFreeMemory(device, *memory, nullptr);
        *buffer = VK_NULL_HANDLE;
        *memory = VK_NULL_HANDLE;
    }

#if JCUT_HAS_CUDA_DRIVER
    bool activateCudaContext(CUcontext context, std::string* error)
    {
        if (cuInit(0) != CUDA_SUCCESS ||
            cuCtxPushCurrent(context) != CUDA_SUCCESS) {
            return fail(error, "failed to activate decoded frame CUDA context");
        }
        cudaContextPushed = true;
        cudaContext = context;
        return true;
    }

    void popCudaContext()
    {
        if (!cudaContextPushed) return;
        CUcontext previous = nullptr;
        cuCtxPopCurrent(&previous);
        cudaContextPushed = false;
    }

    bool ensureCudaMapping(
        VkDeviceMemory memory,
        VkDeviceSize size,
        CUcontext context,
        CUexternalMemory* externalMemory,
        CUdeviceptr* pointer,
        std::string* error)
    {
        if (*externalMemory && *pointer && cudaContext == context) {
            return true;
        }
        destroyCudaMapping(externalMemory, pointer);
        VkMemoryGetFdInfoKHR fdInfo{};
        fdInfo.sType = VK_STRUCTURE_TYPE_MEMORY_GET_FD_INFO_KHR;
        fdInfo.memory = memory;
        fdInfo.handleType =
            VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_FD_BIT;
        int fd = -1;
        if (getMemoryFd(
                device, &fdInfo, &fd) != VK_SUCCESS || fd < 0) {
            return fail(error, "failed to export Vulkan memory FD");
        }
        CUDA_EXTERNAL_MEMORY_HANDLE_DESC handle{};
        handle.type = CU_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_FD;
        handle.handle.fd = fd;
        handle.size = size;
        if (cuImportExternalMemory(
                externalMemory, &handle) != CUDA_SUCCESS) {
            close(fd);
            return fail(error, "cuImportExternalMemory failed");
        }
        CUDA_EXTERNAL_MEMORY_BUFFER_DESC mapping{};
        mapping.size = size;
        if (cuExternalMemoryGetMappedBuffer(
                pointer,
                *externalMemory,
                &mapping) != CUDA_SUCCESS) {
            cuDestroyExternalMemory(*externalMemory);
            *externalMemory = nullptr;
            return fail(error, "cuExternalMemoryGetMappedBuffer failed");
        }
        cudaContext = context;
        return true;
    }

    void destroyCudaMapping(
        CUexternalMemory* memory,
        CUdeviceptr* pointer)
    {
        if (*memory) {
            bool pushed = false;
            if (cudaContext && !cudaContextPushed) {
                pushed =
                    cuCtxPushCurrent(cudaContext) == CUDA_SUCCESS;
            }
#if !defined(JCUT_SKIP_CUDA_EXTERNAL_MEMORY_DESTROY) || \
    !JCUT_SKIP_CUDA_EXTERNAL_MEMORY_DESTROY
            cuDestroyExternalMemory(*memory);
#endif
            if (pushed) {
                CUcontext previous = nullptr;
                cuCtxPopCurrent(&previous);
            }
        }
        *memory = nullptr;
        *pointer = 0;
    }
#endif

    void transition(
        VkCommandBuffer commands,
        VkImageLayout oldLayout,
        VkImageLayout newLayout)
    {
        VkImageMemoryBarrier barrier{};
        barrier.sType =
            VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
        barrier.oldLayout = oldLayout;
        barrier.newLayout = newLayout;
        barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        barrier.image = image;
        barrier.subresourceRange.aspectMask =
            VK_IMAGE_ASPECT_COLOR_BIT;
        barrier.subresourceRange.levelCount = 1;
        barrier.subresourceRange.layerCount = 1;
        VkPipelineStageFlags source =
            VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT;
        VkPipelineStageFlags destination =
            VK_PIPELINE_STAGE_TRANSFER_BIT;
        if (oldLayout == VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL) {
            barrier.srcAccessMask = VK_ACCESS_SHADER_READ_BIT;
            source = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
        } else if (oldLayout == VK_IMAGE_LAYOUT_GENERAL) {
            barrier.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
            source = VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT;
        }
        if (newLayout == VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL) {
            barrier.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
            destination = VK_PIPELINE_STAGE_TRANSFER_BIT;
        } else if (newLayout == VK_IMAGE_LAYOUT_GENERAL) {
            barrier.dstAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
            destination = VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT;
        } else {
            barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
            destination = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
        }
        vkCmdPipelineBarrier(
            commands,
            source,
            destination,
            0,
            0,
            nullptr,
            0,
            nullptr,
            1,
            &barrier);
    }

    bool beginCommands(std::string* error)
    {
        vkResetFences(device, 1, &fence);
        vkResetCommandBuffer(commandBuffer, 0);
        VkCommandBufferBeginInfo begin{};
        begin.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
        begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        return vkBeginCommandBuffer(
                   commandBuffer, &begin) == VK_SUCCESS ||
            fail(error, "failed to begin hardware-frame commands");
    }

    bool submitCommands(std::string* error)
    {
        if (vkEndCommandBuffer(commandBuffer) != VK_SUCCESS) {
            return fail(error, "failed to finish hardware-frame commands");
        }
        VkSubmitInfo submit{};
        submit.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
        submit.commandBufferCount = 1;
        submit.pCommandBuffers = &commandBuffer;
        if (vkQueueSubmit(
                queue, 1, &submit, fence) != VK_SUCCESS) {
            return fail(error, "failed to submit hardware-frame commands");
        }
        return waitIdle(error);
    }

    bool recordRgba(core::SizeI size, std::string* error)
    {
        if (!beginCommands(error)) return false;
        VkBufferMemoryBarrier bufferBarrier{};
        bufferBarrier.sType =
            VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER;
        bufferBarrier.srcAccessMask = VK_ACCESS_MEMORY_WRITE_BIT;
        bufferBarrier.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
        bufferBarrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        bufferBarrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        bufferBarrier.buffer = yBuffer;
        bufferBarrier.size = VK_WHOLE_SIZE;
        vkCmdPipelineBarrier(
            commandBuffer,
            VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
            VK_PIPELINE_STAGE_TRANSFER_BIT,
            0,
            0,
            nullptr,
            1,
            &bufferBarrier,
            0,
            nullptr);
        transition(
            commandBuffer,
            imageLayout,
            VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
        VkBufferImageCopy copy{};
        copy.imageSubresource.aspectMask =
            VK_IMAGE_ASPECT_COLOR_BIT;
        copy.imageSubresource.layerCount = 1;
        copy.imageExtent = {
            static_cast<std::uint32_t>(size.width),
            static_cast<std::uint32_t>(size.height),
            1};
        vkCmdCopyBufferToImage(
            commandBuffer,
            yBuffer,
            image,
            VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
            1,
            &copy);
        transition(
            commandBuffer,
            VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
            VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
        imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        return submitCommands(error);
    }

    bool ensureNv12Pipeline(std::string* error)
    {
        if (pipeline != VK_NULL_HANDLE) return true;
        std::array<VkDescriptorSetLayoutBinding, 4> bindings{};
        bindings[0] = {
            0,
            VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
            1,
            VK_SHADER_STAGE_COMPUTE_BIT,
            nullptr};
        bindings[1] = bindings[0];
        bindings[1].binding = 1;
        bindings[2] = {
            2,
            VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,
            1,
            VK_SHADER_STAGE_COMPUTE_BIT,
            nullptr};
        bindings[3] = bindings[0];
        bindings[3].binding = 3;
        VkDescriptorSetLayoutCreateInfo layout{};
        layout.sType =
            VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
        layout.bindingCount =
            static_cast<std::uint32_t>(bindings.size());
        layout.pBindings = bindings.data();
        if (vkCreateDescriptorSetLayout(
                device,
                &layout,
                nullptr,
                &descriptorLayout) != VK_SUCCESS) {
            return fail(error, "failed to create NV12 descriptor layout");
        }
        std::array<VkDescriptorPoolSize, 2> sizes{{
            {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 3},
            {VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1}}};
        VkDescriptorPoolCreateInfo pool{};
        pool.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
        pool.maxSets = 1;
        pool.poolSizeCount = static_cast<std::uint32_t>(sizes.size());
        pool.pPoolSizes = sizes.data();
        if (vkCreateDescriptorPool(
                device,
                &pool,
                nullptr,
                &descriptorPool) != VK_SUCCESS) {
            return fail(error, "failed to create NV12 descriptor pool");
        }
        VkDescriptorSetAllocateInfo allocation{};
        allocation.sType =
            VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
        allocation.descriptorPool = descriptorPool;
        allocation.descriptorSetCount = 1;
        allocation.pSetLayouts = &descriptorLayout;
        if (vkAllocateDescriptorSets(
                device,
                &allocation,
                &descriptorSet) != VK_SUCCESS) {
            return fail(error, "failed to allocate NV12 descriptor set");
        }
        struct Push {
            int values[8];
            float grade[12];
        };
        VkPushConstantRange push{};
        push.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
        push.size = sizeof(Push);
        VkPipelineLayoutCreateInfo pipelineLayoutInfo{};
        pipelineLayoutInfo.sType =
            VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
        pipelineLayoutInfo.setLayoutCount = 1;
        pipelineLayoutInfo.pSetLayouts = &descriptorLayout;
        pipelineLayoutInfo.pushConstantRangeCount = 1;
        pipelineLayoutInfo.pPushConstantRanges = &push;
        if (vkCreatePipelineLayout(
                device,
                &pipelineLayoutInfo,
                nullptr,
                &pipelineLayout) != VK_SUCCESS) {
            return fail(error, "failed to create NV12 pipeline layout");
        }
        const auto words = readSpirv(
            std::filesystem::path(shaderDirectory) /
            "nv12_buffer_to_rgba.comp.spv");
        if (words.empty()) {
            return fail(error, "NV12 conversion shader is unavailable");
        }
        VkShaderModuleCreateInfo moduleInfo{};
        moduleInfo.sType =
            VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
        moduleInfo.codeSize = words.size() * sizeof(std::uint32_t);
        moduleInfo.pCode = words.data();
        VkShaderModule module = VK_NULL_HANDLE;
        if (vkCreateShaderModule(
                device,
                &moduleInfo,
                nullptr,
                &module) != VK_SUCCESS) {
            return fail(error, "failed to create NV12 shader module");
        }
        VkPipelineShaderStageCreateInfo stage{};
        stage.sType =
            VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
        stage.module = module;
        stage.pName = "main";
        VkComputePipelineCreateInfo pipelineInfo{};
        pipelineInfo.sType =
            VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO;
        pipelineInfo.stage = stage;
        pipelineInfo.layout = pipelineLayout;
        const VkResult created = vkCreateComputePipelines(
            device,
            VK_NULL_HANDLE,
            1,
            &pipelineInfo,
            nullptr,
            &pipeline);
        vkDestroyShaderModule(device, module, nullptr);
        return created == VK_SUCCESS ||
            fail(error, "failed to create NV12 compute pipeline");
    }

    bool recordNv12(
        core::SizeI size,
        int yPitch,
        int uvPitch,
        const AVFrame* frame,
        const HardwareFrameColorGradeCore& grade,
        std::string* error)
    {
        const VkDeviceSize curveBytes =
            sizeof(std::uint32_t) * grade.curveLut.size();
        if (!ensureNv12Pipeline(error) ||
            !ensureHostBuffer(
                curveBytes,
                &curveBuffer,
                &curveMemory,
                &curveAllocationSize,
                error)) {
            return false;
        }
        void* mapped = nullptr;
        if (vkMapMemory(
                device,
                curveMemory,
                0,
                curveBytes,
                0,
                &mapped) != VK_SUCCESS ||
            !mapped) {
            return fail(error, "failed to map grading LUT memory");
        }
        std::memcpy(mapped, grade.curveLut.data(), curveBytes);
        vkUnmapMemory(device, curveMemory);
        if (!beginCommands(error)) {
            return false;
        }
        VkDescriptorBufferInfo yInfo{
            yBuffer, 0, yAllocationSize};
        VkDescriptorBufferInfo uvInfo{
            uvBuffer, 0, uvAllocationSize};
        VkDescriptorBufferInfo curveInfo{
            curveBuffer, 0, curveBytes};
        VkDescriptorImageInfo imageInfo{};
        imageInfo.imageView = imageView;
        imageInfo.imageLayout = VK_IMAGE_LAYOUT_GENERAL;
        std::array<VkWriteDescriptorSet, 4> writes{};
        writes[0].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        writes[0].dstSet = descriptorSet;
        writes[0].dstBinding = 0;
        writes[0].descriptorCount = 1;
        writes[0].descriptorType =
            VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        writes[0].pBufferInfo = &yInfo;
        writes[1] = writes[0];
        writes[1].dstBinding = 1;
        writes[1].pBufferInfo = &uvInfo;
        writes[2].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        writes[2].dstSet = descriptorSet;
        writes[2].dstBinding = 2;
        writes[2].descriptorCount = 1;
        writes[2].descriptorType =
            VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
        writes[2].pImageInfo = &imageInfo;
        writes[3] = writes[0];
        writes[3].dstBinding = 3;
        writes[3].pBufferInfo = &curveInfo;
        vkUpdateDescriptorSets(
            device,
            static_cast<std::uint32_t>(writes.size()),
            writes.data(),
            0,
            nullptr);

        std::array<VkBufferMemoryBarrier, 3> barriers{};
        for (std::size_t index = 0; index < barriers.size(); ++index) {
            barriers[index].sType =
                VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER;
            barriers[index].srcAccessMask = VK_ACCESS_MEMORY_WRITE_BIT;
            barriers[index].dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
            barriers[index].srcQueueFamilyIndex =
                VK_QUEUE_FAMILY_IGNORED;
            barriers[index].dstQueueFamilyIndex =
                VK_QUEUE_FAMILY_IGNORED;
            barriers[index].buffer =
                index == 0 ? yBuffer
                           : index == 1 ? uvBuffer : curveBuffer;
            barriers[index].size = VK_WHOLE_SIZE;
        }
        barriers[2].srcAccessMask = VK_ACCESS_HOST_WRITE_BIT;
        vkCmdPipelineBarrier(
            commandBuffer,
            VK_PIPELINE_STAGE_ALL_COMMANDS_BIT |
                VK_PIPELINE_STAGE_HOST_BIT,
            VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
            0,
            0,
            nullptr,
            static_cast<std::uint32_t>(barriers.size()),
            barriers.data(),
            0,
            nullptr);
        transition(
            commandBuffer, imageLayout, VK_IMAGE_LAYOUT_GENERAL);
        int matrix = 0;
        if (frame->colorspace == AVCOL_SPC_BT709) matrix = 1;
        if (frame->colorspace == AVCOL_SPC_BT2020_NCL ||
            frame->colorspace == AVCOL_SPC_BT2020_CL) {
            matrix = 2;
        } else if (frame->colorspace == AVCOL_SPC_UNSPECIFIED &&
                   (size.height > 576 || size.width >= 1280)) {
            matrix = 1;
        }
        struct Push {
            int width;
            int height;
            int yPitch;
            int uvPitch;
            int fullRange;
            int colorMatrix;
            int chromaSwap;
            int curvesEnabled;
            float brightness;
            float contrast;
            float saturation;
            float shadowsR;
            float shadowsG;
            float shadowsB;
            float midtonesR;
            float midtonesG;
            float midtonesB;
            float highlightsR;
            float highlightsG;
            float highlightsB;
        } push{
            size.width,
            size.height,
            yPitch,
            uvPitch,
            frame->color_range == AVCOL_RANGE_JPEG ? 1 : 0,
            matrix,
            0,
            grade.curvesEnabled ? 1 : 0,
            grade.brightness,
            grade.contrast,
            grade.saturation,
            grade.shadowsR,
            grade.shadowsG,
            grade.shadowsB,
            grade.midtonesR,
            grade.midtonesG,
            grade.midtonesB,
            grade.highlightsR,
            grade.highlightsG,
            grade.highlightsB};
        vkCmdBindPipeline(
            commandBuffer,
            VK_PIPELINE_BIND_POINT_COMPUTE,
            pipeline);
        vkCmdBindDescriptorSets(
            commandBuffer,
            VK_PIPELINE_BIND_POINT_COMPUTE,
            pipelineLayout,
            0,
            1,
            &descriptorSet,
            0,
            nullptr);
        vkCmdPushConstants(
            commandBuffer,
            pipelineLayout,
            VK_SHADER_STAGE_COMPUTE_BIT,
            0,
            sizeof(push),
            &push);
        vkCmdDispatch(
            commandBuffer,
            static_cast<std::uint32_t>((size.width + 15) / 16),
            static_cast<std::uint32_t>((size.height + 15) / 16),
            1);
        transition(
            commandBuffer,
            VK_IMAGE_LAYOUT_GENERAL,
            VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
        imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        return submitCommands(error);
    }

    VkPhysicalDevice physicalDevice = VK_NULL_HANDLE;
    VkDevice device = VK_NULL_HANDLE;
    VkQueue queue = VK_NULL_HANDLE;
    std::uint32_t queueFamily = UINT32_MAX;
    std::string shaderDirectory;
    bool initialized = false;
    PFN_vkGetMemoryFdKHR getMemoryFd = nullptr;
    VkCommandPool commandPool = VK_NULL_HANDLE;
    VkCommandBuffer commandBuffer = VK_NULL_HANDLE;
    VkFence fence = VK_NULL_HANDLE;
    VkImage image = VK_NULL_HANDLE;
    VkDeviceMemory imageMemory = VK_NULL_HANDLE;
    VkImageView imageView = VK_NULL_HANDLE;
    VkImageLayout imageLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    VkFormat imageFormat = VK_FORMAT_UNDEFINED;
    core::SizeI imageSize;
    VkBuffer yBuffer = VK_NULL_HANDLE;
    VkDeviceMemory yMemory = VK_NULL_HANDLE;
    VkDeviceSize yAllocationSize = 0;
    VkBuffer uvBuffer = VK_NULL_HANDLE;
    VkDeviceMemory uvMemory = VK_NULL_HANDLE;
    VkDeviceSize uvAllocationSize = 0;
    VkBuffer curveBuffer = VK_NULL_HANDLE;
    VkDeviceMemory curveMemory = VK_NULL_HANDLE;
    VkDeviceSize curveAllocationSize = 0;
    VkDescriptorSetLayout descriptorLayout = VK_NULL_HANDLE;
    VkDescriptorPool descriptorPool = VK_NULL_HANDLE;
    VkDescriptorSet descriptorSet = VK_NULL_HANDLE;
    VkPipelineLayout pipelineLayout = VK_NULL_HANDLE;
    VkPipeline pipeline = VK_NULL_HANDLE;
    VulkanHardwareFrameImportStatus status;
#if JCUT_HAS_CUDA_DRIVER
    CUexternalMemory yExternalMemory = nullptr;
    CUexternalMemory uvExternalMemory = nullptr;
    CUdeviceptr yDevicePointer = 0;
    CUdeviceptr uvDevicePointer = 0;
    CUcontext cudaContext = nullptr;
    bool cudaContextPushed = false;
#endif
};

VulkanHardwareFrameImportCore::VulkanHardwareFrameImportCore()
    : m_impl(std::make_unique<Impl>())
{
}

VulkanHardwareFrameImportCore::~VulkanHardwareFrameImportCore() = default;

bool VulkanHardwareFrameImportCore::initialize(
    VkPhysicalDevice physicalDevice,
    VkDevice device,
    VkQueue queue,
    std::uint32_t queueFamilyIndex,
    std::string shaderDirectory,
    std::string* errorMessage)
{
    return m_impl->initialize(
        physicalDevice,
        device,
        queue,
        queueFamilyIndex,
        std::move(shaderDirectory),
        errorMessage);
}

void VulkanHardwareFrameImportCore::release()
{
    m_impl->release();
}

VulkanHardwareFrameImportStatus
VulkanHardwareFrameImportCore::probe(
    const core::FramePayloadCore& payload) const
{
    return m_impl->probe(payload);
}

bool VulkanHardwareFrameImportCore::importFrame(
    const core::FramePayloadCore& payload,
    std::string* errorMessage)
{
    return m_impl->importFrame(
        payload, HardwareFrameColorGradeCore{}, errorMessage);
}

bool VulkanHardwareFrameImportCore::importFrame(
    const core::FramePayloadCore& payload,
    const HardwareFrameColorGradeCore& grade,
    std::string* errorMessage)
{
    return m_impl->importFrame(payload, grade, errorMessage);
}

ExternalImage VulkanHardwareFrameImportCore::externalImage() const
{
    return m_impl->externalImage();
}

const VulkanHardwareFrameImportStatus&
VulkanHardwareFrameImportCore::lastStatus() const
{
    return m_impl->lastStatus();
}

} // namespace jcut::vulkan_import
