#include "vulkan_external_frame_import_core.h"

#include <algorithm>
#include <chrono>
#include <unistd.h>

namespace jcut::vulkan_import {
namespace {

void setError(std::string* output, const char* message)
{
    if (output) {
        *output = message;
    }
}

bool validFrame(const render_detail::OffscreenVulkanFrame& frame)
{
    return frame.valid &&
        frame.device != VK_NULL_HANDLE &&
        frame.image != VK_NULL_HANDLE &&
        frame.imageView != VK_NULL_HANDLE &&
        frame.imageMemory != VK_NULL_HANDLE &&
        frame.size.valid() &&
        frame.imageFormat != VK_FORMAT_UNDEFINED;
}

} // namespace

class VulkanExternalFrameImportCore::Impl {
public:
    bool initialize(const DeviceContext& value, std::string* errorMessage)
    {
        release();
        if (value.physicalDevice == VK_NULL_HANDLE ||
            value.device == VK_NULL_HANDLE ||
            value.queue == VK_NULL_HANDLE ||
            value.queueFamilyIndex == UINT32_MAX) {
            setError(errorMessage,
                     "Invalid Vulkan device context for frame handoff.");
            return false;
        }
        context = value;

        VkCommandPoolCreateInfo poolInfo{};
        poolInfo.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
        poolInfo.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
        poolInfo.queueFamilyIndex = context.queueFamilyIndex;
        if (vkCreateCommandPool(
                context.device, &poolInfo, nullptr, &commandPool) != VK_SUCCESS) {
            setError(errorMessage,
                     "Failed to create frame handoff command pool.");
            release();
            return false;
        }

        VkCommandBufferAllocateInfo allocInfo{};
        allocInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
        allocInfo.commandPool = commandPool;
        allocInfo.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        allocInfo.commandBufferCount = 1;
        if (vkAllocateCommandBuffers(
                context.device, &allocInfo, &commandBuffer) != VK_SUCCESS) {
            setError(errorMessage,
                     "Failed to allocate frame handoff command buffer.");
            release();
            return false;
        }

        VkFenceCreateInfo fenceInfo{};
        fenceInfo.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
        if (vkCreateFence(context.device, &fenceInfo, nullptr, &fence) !=
            VK_SUCCESS) {
            setError(errorMessage, "Failed to create frame handoff fence.");
            release();
            return false;
        }

        initialized = true;
        return true;
    }

    void release()
    {
        (void)finishPendingCopy(nullptr, nullptr);
        if (context.device != VK_NULL_HANDLE) {
            vkDeviceWaitIdle(context.device);
            if (importedImageView != VK_NULL_HANDLE) {
                vkDestroyImageView(context.device, importedImageView, nullptr);
            }
            if (importedImage != VK_NULL_HANDLE) {
                vkDestroyImage(context.device, importedImage, nullptr);
            }
            if (importedImageMemory != VK_NULL_HANDLE) {
                vkFreeMemory(context.device, importedImageMemory, nullptr);
                ++stats.importedMemoryFrees;
            }
            if (imageView != VK_NULL_HANDLE) {
                vkDestroyImageView(context.device, imageView, nullptr);
            }
            if (image != VK_NULL_HANDLE) {
                vkDestroyImage(context.device, image, nullptr);
            }
            if (imageMemory != VK_NULL_HANDLE) {
                vkFreeMemory(context.device, imageMemory, nullptr);
                ++stats.imageMemoryFrees;
            }
            if (fence != VK_NULL_HANDLE) {
                vkDestroyFence(context.device, fence, nullptr);
            }
            if (commandPool != VK_NULL_HANDLE) {
                vkDestroyCommandPool(context.device, commandPool, nullptr);
            }
        }

        context = {};
        initialized = false;
        commandPool = VK_NULL_HANDLE;
        commandBuffer = VK_NULL_HANDLE;
        fence = VK_NULL_HANDLE;
        image = VK_NULL_HANDLE;
        imageMemory = VK_NULL_HANDLE;
        imageView = VK_NULL_HANDLE;
        imageLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        imageFormat = VK_FORMAT_UNDEFINED;
        imageSize = {};
        importSourceDevice = VK_NULL_HANDLE;
        importSourceMemory = VK_NULL_HANDLE;
        importedImage = VK_NULL_HANDLE;
        importedImageMemory = VK_NULL_HANDLE;
        importedImageView = VK_NULL_HANDLE;
        importedImageLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        importedImageFormat = VK_FORMAT_UNDEFINED;
        importedImageSize = {};
        copyPending = false;
    }

    bool importFrame(const render_detail::OffscreenVulkanFrame& frame,
                     std::string* errorMessage)
    {
        if (!initialized) {
            setError(errorMessage, "Vulkan handoff is not initialized.");
            return false;
        }
        if (!validFrame(frame)) {
            setError(errorMessage,
                     "Invalid offscreen Vulkan frame for external-memory import.");
            return false;
        }
        if (!prepareFrame(frame, errorMessage)) {
            return false;
        }
        importedImageLayout = frame.imageLayout;
        return copyImportedFrameToLocal(frame.imageLayout, errorMessage);
    }

    bool recordFrameCopy(VkCommandBuffer targetCommandBuffer,
                         const render_detail::OffscreenVulkanFrame& frame,
                         std::string* errorMessage)
    {
        if (!initialized || targetCommandBuffer == VK_NULL_HANDLE) {
            setError(
                errorMessage,
                "Vulkan handoff is not initialized for command-buffer import.");
            return false;
        }
        if (!validFrame(frame)) {
            setError(errorMessage,
                     "Invalid offscreen Vulkan frame for external-memory import.");
            return false;
        }
        if (!prepareFrame(frame, errorMessage)) {
            return false;
        }
        importedImageLayout = frame.imageLayout;
        return recordImportedFrameCopyToLocal(
            targetCommandBuffer, frame.imageLayout, errorMessage);
    }

    ExternalImage externalImage() const
    {
        ExternalImage result;
        result.image = image;
        result.imageView = imageView;
        result.imageLayout = imageLayout;
        result.imageFormat = imageFormat;
        result.size = imageSize;
        result.sourceIsSrgb = false;
        return result;
    }

private:
    bool prepareFrame(const render_detail::OffscreenVulkanFrame& frame,
                      std::string* errorMessage)
    {
        // Preserve the existing borrowed-frame contract: readySemaphoreFd
        // remains caller-owned and is neither waited nor closed here. The
        // producer keeps the frame alive until the consumer acknowledges it.
        // Finish our prior immediate copy before replacing either source or
        // destination resources that its submitted command buffer may use.
        if (!finishPendingCopy(nullptr, errorMessage)) {
            return false;
        }
        if (!ensureImageResources(frame.size, frame.imageFormat, errorMessage) ||
            !ensureImportedImageResources(
                frame.size, frame.imageFormat, errorMessage)) {
            return false;
        }
        if (importSourceDevice == frame.device &&
            importSourceMemory == frame.imageMemory &&
            importedImage != VK_NULL_HANDLE &&
            importedImageView != VK_NULL_HANDLE &&
            importedImageMemory != VK_NULL_HANDLE) {
            return true;
        }

        if (importedImageMemory != VK_NULL_HANDLE) {
            // Vulkan image-memory binding is immutable. A new producer
            // allocation with identical geometry still requires a new
            // imported image/view before the new memory can be bound.
            if (!ensureImportedImageResources(
                    frame.size, frame.imageFormat, errorMessage, true)) {
                return false;
            }
        }

        PFN_vkGetMemoryFdKHR sourceGetMemoryFdKHR =
            reinterpret_cast<PFN_vkGetMemoryFdKHR>(
                vkGetDeviceProcAddr(frame.device, "vkGetMemoryFdKHR"));
        if (!sourceGetMemoryFdKHR) {
            setError(
                errorMessage,
                "Source Vulkan device does not expose vkGetMemoryFdKHR.");
            return false;
        }

        VkMemoryGetFdInfoKHR fdInfo{};
        fdInfo.sType = VK_STRUCTURE_TYPE_MEMORY_GET_FD_INFO_KHR;
        fdInfo.memory = frame.imageMemory;
        fdInfo.handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_FD_BIT;
        int fd = -1;
        if (sourceGetMemoryFdKHR(frame.device, &fdInfo, &fd) != VK_SUCCESS ||
            fd < 0) {
            setError(
                errorMessage,
                "Failed to export offscreen Vulkan image memory FD.");
            return false;
        }

        VkMemoryRequirements requirements{};
        vkGetImageMemoryRequirements(
            context.device, importedImage, &requirements);
        const std::uint32_t memoryType = findMemoryType(
            requirements.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
        if (memoryType == UINT32_MAX) {
            close(fd);
            setError(errorMessage, "No imported Vulkan image memory type.");
            return false;
        }

        VkImportMemoryFdInfoKHR importInfo{};
        importInfo.sType = VK_STRUCTURE_TYPE_IMPORT_MEMORY_FD_INFO_KHR;
        importInfo.handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_FD_BIT;
        importInfo.fd = fd;
        VkMemoryAllocateInfo allocInfo{};
        allocInfo.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
        allocInfo.pNext = &importInfo;
        allocInfo.allocationSize = requirements.size;
        allocInfo.memoryTypeIndex = memoryType;
        const VkResult allocationResult = vkAllocateMemory(
            context.device, &allocInfo, nullptr, &importedImageMemory);
        if (allocationResult != VK_SUCCESS) {
            // A successful Vulkan import consumes the opaque FD. On failure
            // ownership remains with this process, so close it exactly once.
            close(fd);
            setError(
                errorMessage,
                "Failed to import/bind offscreen Vulkan image memory.");
            return false;
        }
        ++stats.importedMemoryAllocations;
        if (vkBindImageMemory(
                context.device, importedImage, importedImageMemory, 0) !=
            VK_SUCCESS) {
            vkFreeMemory(context.device, importedImageMemory, nullptr);
            ++stats.importedMemoryFrees;
            importedImageMemory = VK_NULL_HANDLE;
            setError(
                errorMessage,
                "Failed to import/bind offscreen Vulkan image memory.");
            return false;
        }

        VkImageViewCreateInfo viewInfo{};
        viewInfo.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
        viewInfo.image = importedImage;
        viewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
        viewInfo.format = frame.imageFormat;
        viewInfo.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        viewInfo.subresourceRange.levelCount = 1;
        viewInfo.subresourceRange.layerCount = 1;
        if (importedImageView != VK_NULL_HANDLE) {
            vkDestroyImageView(context.device, importedImageView, nullptr);
            importedImageView = VK_NULL_HANDLE;
        }
        if (vkCreateImageView(
                context.device, &viewInfo, nullptr, &importedImageView) !=
            VK_SUCCESS) {
            setError(errorMessage,
                     "Failed to create imported Vulkan image view.");
            return false;
        }

        importSourceDevice = frame.device;
        importSourceMemory = frame.imageMemory;
        return true;
    }

    bool ensureImageResources(const core::SizeI& size,
                              VkFormat format,
                              std::string* errorMessage)
    {
        if (image != VK_NULL_HANDLE && imageMemory != VK_NULL_HANDLE &&
            imageView != VK_NULL_HANDLE && imageSize == size &&
            imageFormat == format) {
            return true;
        }
        if (imageView != VK_NULL_HANDLE) {
            vkDestroyImageView(context.device, imageView, nullptr);
        }
        if (image != VK_NULL_HANDLE) {
            vkDestroyImage(context.device, image, nullptr);
        }
        if (imageMemory != VK_NULL_HANDLE) {
            vkFreeMemory(context.device, imageMemory, nullptr);
            ++stats.imageMemoryFrees;
        }
        image = VK_NULL_HANDLE;
        imageMemory = VK_NULL_HANDLE;
        imageView = VK_NULL_HANDLE;
        imageLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        imageSize = size;
        imageFormat = format;

        VkImageCreateInfo imageInfo{};
        imageInfo.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
        imageInfo.imageType = VK_IMAGE_TYPE_2D;
        imageInfo.extent = {
            static_cast<std::uint32_t>(size.width),
            static_cast<std::uint32_t>(size.height),
            1};
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
        if (vkCreateImage(context.device, &imageInfo, nullptr, &image) !=
            VK_SUCCESS) {
            setError(errorMessage,
                     "Failed to create Vulkan handoff image.");
            return false;
        }

        VkMemoryRequirements requirements{};
        vkGetImageMemoryRequirements(context.device, image, &requirements);
        const std::uint32_t memoryType = findMemoryType(
            requirements.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
        if (memoryType == UINT32_MAX) {
            setError(errorMessage, "No Vulkan handoff image memory type.");
            return false;
        }
        VkMemoryAllocateInfo allocInfo{};
        allocInfo.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
        allocInfo.allocationSize = requirements.size;
        allocInfo.memoryTypeIndex = memoryType;
        if (vkAllocateMemory(
                context.device, &allocInfo, nullptr, &imageMemory) !=
                VK_SUCCESS ||
            vkBindImageMemory(context.device, image, imageMemory, 0) !=
                VK_SUCCESS) {
            setError(
                errorMessage,
                "Failed to allocate/bind Vulkan handoff image memory.");
            return false;
        }
        ++stats.imageMemoryAllocations;

        VkImageViewCreateInfo viewInfo{};
        viewInfo.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
        viewInfo.image = image;
        viewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
        viewInfo.format = format;
        viewInfo.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        viewInfo.subresourceRange.levelCount = 1;
        viewInfo.subresourceRange.layerCount = 1;
        if (vkCreateImageView(context.device, &viewInfo, nullptr, &imageView) !=
            VK_SUCCESS) {
            setError(errorMessage,
                     "Failed to create Vulkan handoff image view.");
            return false;
        }
        return true;
    }

    bool ensureImportedImageResources(const core::SizeI& size,
                                      VkFormat format,
                                      std::string* errorMessage,
                                      bool forceRecreate = false)
    {
        if (!forceRecreate && importedImage != VK_NULL_HANDLE &&
            importedImageMemory != VK_NULL_HANDLE &&
            importedImageView != VK_NULL_HANDLE &&
            importedImageSize == size && importedImageFormat == format) {
            return true;
        }
        if (importedImageView != VK_NULL_HANDLE) {
            vkDestroyImageView(context.device, importedImageView, nullptr);
        }
        if (importedImage != VK_NULL_HANDLE) {
            vkDestroyImage(context.device, importedImage, nullptr);
        }
        if (importedImageMemory != VK_NULL_HANDLE) {
            vkFreeMemory(context.device, importedImageMemory, nullptr);
            ++stats.importedMemoryFrees;
        }
        importedImage = VK_NULL_HANDLE;
        importedImageMemory = VK_NULL_HANDLE;
        importedImageView = VK_NULL_HANDLE;
        importedImageLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        importedImageSize = size;
        importedImageFormat = format;
        importSourceDevice = VK_NULL_HANDLE;
        importSourceMemory = VK_NULL_HANDLE;

        VkExternalMemoryImageCreateInfo externalInfo{};
        externalInfo.sType =
            VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_IMAGE_CREATE_INFO;
        externalInfo.handleTypes =
            VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_FD_BIT;
        VkImageCreateInfo imageInfo{};
        imageInfo.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
        imageInfo.pNext = &externalInfo;
        imageInfo.imageType = VK_IMAGE_TYPE_2D;
        imageInfo.extent = {
            static_cast<std::uint32_t>(size.width),
            static_cast<std::uint32_t>(size.height),
            1};
        imageInfo.mipLevels = 1;
        imageInfo.arrayLayers = 1;
        imageInfo.format = format;
        imageInfo.tiling = VK_IMAGE_TILING_OPTIMAL;
        imageInfo.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        imageInfo.usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT |
            VK_IMAGE_USAGE_TRANSFER_SRC_BIT |
            VK_IMAGE_USAGE_STORAGE_BIT |
            VK_IMAGE_USAGE_SAMPLED_BIT |
            VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
        imageInfo.samples = VK_SAMPLE_COUNT_1_BIT;
        imageInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
        if (vkCreateImage(
                context.device, &imageInfo, nullptr, &importedImage) !=
            VK_SUCCESS) {
            setError(
                errorMessage,
                "Failed to create imported Vulkan handoff image.");
            return false;
        }
        return true;
    }

    bool copyImportedFrameToLocal(VkImageLayout sourceLayout,
                                  std::string* errorMessage)
    {
        if (!finishPendingCopy(nullptr, errorMessage)) {
            return false;
        }
        vkResetFences(context.device, 1, &fence);
        vkResetCommandBuffer(commandBuffer, 0);
        VkCommandBufferBeginInfo beginInfo{};
        beginInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
        beginInfo.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        if (vkBeginCommandBuffer(commandBuffer, &beginInfo) != VK_SUCCESS) {
            setError(
                errorMessage,
                "Failed to begin Vulkan preview sync command buffer.");
            return false;
        }
        recordImageCopy(commandBuffer, sourceLayout);
        if (vkEndCommandBuffer(commandBuffer) != VK_SUCCESS) {
            setError(
                errorMessage,
                "Failed to end Vulkan preview sync command buffer.");
            return false;
        }
        VkSubmitInfo submitInfo{};
        submitInfo.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
        submitInfo.commandBufferCount = 1;
        submitInfo.pCommandBuffers = &commandBuffer;
        if (vkQueueSubmit(context.queue, 1, &submitInfo, fence) != VK_SUCCESS) {
            setError(errorMessage,
                     "Failed to copy imported Vulkan preview image.");
            return false;
        }
        imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        copyPending = true;
        copyStart = std::chrono::steady_clock::now();
        return true;
    }

    bool recordImportedFrameCopyToLocal(VkCommandBuffer targetCommandBuffer,
                                        VkImageLayout sourceLayout,
                                        std::string* errorMessage)
    {
        if (targetCommandBuffer == VK_NULL_HANDLE ||
            importedImage == VK_NULL_HANDLE || image == VK_NULL_HANDLE) {
            setError(
                errorMessage,
                "Invalid Vulkan image state for command-buffer imported frame copy.");
            return false;
        }
        if (!finishPendingCopy(nullptr, errorMessage)) {
            return false;
        }
        recordImageCopy(targetCommandBuffer, sourceLayout);
        imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        return true;
    }

    void recordImageCopy(VkCommandBuffer targetCommandBuffer,
                         VkImageLayout sourceLayout)
    {
        transitionSpecificImage(
            targetCommandBuffer,
            importedImage,
            sourceLayout,
            VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
            0,
            VK_ACCESS_TRANSFER_READ_BIT,
            VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
            VK_PIPELINE_STAGE_TRANSFER_BIT);
        transitionImage(
            targetCommandBuffer,
            imageLayout,
            VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
        VkImageCopy copyRegion{};
        copyRegion.srcSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        copyRegion.srcSubresource.layerCount = 1;
        copyRegion.dstSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        copyRegion.dstSubresource.layerCount = 1;
        copyRegion.extent.width =
            static_cast<std::uint32_t>(std::max(1, imageSize.width));
        copyRegion.extent.height =
            static_cast<std::uint32_t>(std::max(1, imageSize.height));
        copyRegion.extent.depth = 1;
        vkCmdCopyImage(
            targetCommandBuffer,
            importedImage,
            VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
            image,
            VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
            1,
            &copyRegion);
        transitionImage(
            targetCommandBuffer,
            VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
            VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
        transitionSpecificImage(
            targetCommandBuffer,
            importedImage,
            VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
            sourceLayout,
            VK_ACCESS_TRANSFER_READ_BIT,
            0,
            VK_PIPELINE_STAGE_TRANSFER_BIT,
            VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT);
    }

public:
    bool finishPendingCopy(double* copyMs, std::string* errorMessage)
    {
        if (copyMs) {
            *copyMs = 0.0;
        }
        if (!copyPending) {
            return true;
        }
        if (vkWaitForFences(
                context.device, 1, &fence, VK_TRUE, 5'000'000'000ull) !=
            VK_SUCCESS) {
            setError(
                errorMessage,
                "timed out waiting for Vulkan frame handoff upload");
            return false;
        }
        copyPending = false;
        if (copyMs) {
            *copyMs = std::chrono::duration<double, std::milli>(
                std::chrono::steady_clock::now() - copyStart).count();
        }
        return true;
    }

    bool isInitialized() const { return initialized; }
    ResourceStats resourceStats() const { return stats; }
    void resetResourceStats() { stats = {}; }

private:
    void transitionImage(VkCommandBuffer targetCommandBuffer,
                         VkImageLayout oldLayout,
                         VkImageLayout newLayout)
    {
        VkImageMemoryBarrier barrier{};
        barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
        barrier.oldLayout = oldLayout;
        barrier.newLayout = newLayout;
        barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        barrier.image = image;
        barrier.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        barrier.subresourceRange.baseMipLevel = 0;
        barrier.subresourceRange.levelCount = 1;
        barrier.subresourceRange.baseArrayLayer = 0;
        barrier.subresourceRange.layerCount = 1;
        vkCmdPipelineBarrier(
            targetCommandBuffer,
            VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
            VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
            0,
            0,
            nullptr,
            0,
            nullptr,
            1,
            &barrier);
    }

    static void transitionSpecificImage(
        VkCommandBuffer targetCommandBuffer,
        VkImage targetImage,
        VkImageLayout oldLayout,
        VkImageLayout newLayout,
        VkAccessFlags srcAccessMask,
        VkAccessFlags dstAccessMask,
        VkPipelineStageFlags srcStageMask,
        VkPipelineStageFlags dstStageMask)
    {
        VkImageMemoryBarrier barrier{};
        barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
        barrier.oldLayout = oldLayout;
        barrier.newLayout = newLayout;
        barrier.srcAccessMask = srcAccessMask;
        barrier.dstAccessMask = dstAccessMask;
        barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        barrier.image = targetImage;
        barrier.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        barrier.subresourceRange.baseMipLevel = 0;
        barrier.subresourceRange.levelCount = 1;
        barrier.subresourceRange.baseArrayLayer = 0;
        barrier.subresourceRange.layerCount = 1;
        vkCmdPipelineBarrier(
            targetCommandBuffer,
            srcStageMask,
            dstStageMask,
            0,
            0,
            nullptr,
            0,
            nullptr,
            1,
            &barrier);
    }

    std::uint32_t findMemoryType(
        std::uint32_t typeBits,
        VkMemoryPropertyFlags properties) const
    {
        VkPhysicalDeviceMemoryProperties memoryProperties{};
        vkGetPhysicalDeviceMemoryProperties(
            context.physicalDevice, &memoryProperties);
        for (std::uint32_t index = 0;
             index < memoryProperties.memoryTypeCount;
             ++index) {
            if ((typeBits & (1u << index)) != 0 &&
                (memoryProperties.memoryTypes[index].propertyFlags &
                 properties) == properties) {
                return index;
            }
        }
        return UINT32_MAX;
    }

    DeviceContext context;
    bool initialized = false;
    VkCommandPool commandPool = VK_NULL_HANDLE;
    VkCommandBuffer commandBuffer = VK_NULL_HANDLE;
    VkFence fence = VK_NULL_HANDLE;

    VkImage image = VK_NULL_HANDLE;
    VkDeviceMemory imageMemory = VK_NULL_HANDLE;
    VkImageView imageView = VK_NULL_HANDLE;
    VkImageLayout imageLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    VkFormat imageFormat = VK_FORMAT_UNDEFINED;
    core::SizeI imageSize;

    VkDevice importSourceDevice = VK_NULL_HANDLE;
    VkDeviceMemory importSourceMemory = VK_NULL_HANDLE;
    VkImage importedImage = VK_NULL_HANDLE;
    VkDeviceMemory importedImageMemory = VK_NULL_HANDLE;
    VkImageView importedImageView = VK_NULL_HANDLE;
    VkImageLayout importedImageLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    VkFormat importedImageFormat = VK_FORMAT_UNDEFINED;
    core::SizeI importedImageSize;

    bool copyPending = false;
    std::chrono::steady_clock::time_point copyStart{};
    ResourceStats stats;
};

VulkanExternalFrameImportCore::VulkanExternalFrameImportCore()
    : m_impl(std::make_unique<Impl>())
{
}

VulkanExternalFrameImportCore::~VulkanExternalFrameImportCore()
{
    release();
}

bool VulkanExternalFrameImportCore::initialize(
    const DeviceContext& context,
    std::string* errorMessage)
{
    return m_impl->initialize(context, errorMessage);
}

void VulkanExternalFrameImportCore::release()
{
    m_impl->release();
}

bool VulkanExternalFrameImportCore::isInitialized() const
{
    return m_impl->isInitialized();
}

bool VulkanExternalFrameImportCore::importFrame(
    const render_detail::OffscreenVulkanFrame& frame,
    std::string* errorMessage)
{
    return m_impl->importFrame(frame, errorMessage);
}

bool VulkanExternalFrameImportCore::recordFrameCopy(
    VkCommandBuffer commandBuffer,
    const render_detail::OffscreenVulkanFrame& frame,
    std::string* errorMessage)
{
    return m_impl->recordFrameCopy(commandBuffer, frame, errorMessage);
}

bool VulkanExternalFrameImportCore::finishPendingCopy(
    double* copyMs,
    std::string* errorMessage)
{
    return m_impl->finishPendingCopy(copyMs, errorMessage);
}

ExternalImage VulkanExternalFrameImportCore::externalImage() const
{
    return m_impl->externalImage();
}

ResourceStats VulkanExternalFrameImportCore::resourceStats() const
{
    return m_impl->resourceStats();
}

void VulkanExternalFrameImportCore::resetResourceStats()
{
    m_impl->resetResourceStats();
}

} // namespace jcut::vulkan_import
