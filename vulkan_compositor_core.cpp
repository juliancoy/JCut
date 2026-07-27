#include "vulkan_compositor_core.h"

#include <algorithm>
#include <array>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <limits>
#include <vector>

#include <vulkan/vulkan.h>

namespace jcut::vulkan {
namespace {

constexpr std::uint64_t kHeadlessCompositorWaitTimeoutNs =
    5'000'000'000ull;

bool fail(std::string* errorOut, const char* message)
{
    if (errorOut) {
        *errorOut = message;
    }
    return false;
}

std::uint32_t findMemoryType(
    VkPhysicalDevice physicalDevice,
    std::uint32_t typeBits,
    VkMemoryPropertyFlags required)
{
    VkPhysicalDeviceMemoryProperties properties{};
    vkGetPhysicalDeviceMemoryProperties(
        physicalDevice, &properties);
    for (std::uint32_t index = 0;
         index < properties.memoryTypeCount;
         ++index) {
        if ((typeBits & (1U << index)) != 0 &&
            (properties.memoryTypes[index].propertyFlags &
             required) == required) {
            return index;
        }
    }
    return UINT32_MAX;
}

bool supportsDeviceExtension(
    VkPhysicalDevice physicalDevice,
    const char* name)
{
    std::uint32_t count = 0;
    vkEnumerateDeviceExtensionProperties(
        physicalDevice, nullptr, &count, nullptr);
    std::vector<VkExtensionProperties> extensions(count);
    vkEnumerateDeviceExtensionProperties(
        physicalDevice, nullptr, &count, extensions.data());
    return std::any_of(
        extensions.begin(),
        extensions.end(),
        [name](const VkExtensionProperties& extension) {
            return std::strcmp(extension.extensionName, name) == 0;
        });
}

std::vector<std::uint32_t> readSpirv(
    const std::filesystem::path& path)
{
    std::ifstream input(path, std::ios::binary | std::ios::ate);
    if (!input) return {};
    const std::streamsize bytes = input.tellg();
    if (bytes <= 0 ||
        bytes % static_cast<std::streamsize>(
                    sizeof(std::uint32_t)) != 0) {
        return {};
    }
    std::vector<std::uint32_t> words(
        static_cast<std::size_t>(bytes) /
        sizeof(std::uint32_t));
    input.seekg(0);
    input.read(
        reinterpret_cast<char*>(words.data()), bytes);
    return input ? words : std::vector<std::uint32_t>{};
}

} // namespace

class VulkanCompositorCore::Impl {
public:
    ~Impl()
    {
        release();
    }

    bool upload(
        const core::ImageBuffer& image,
        render_detail::OffscreenVulkanFrame* frameOut,
        std::string* errorOut)
    {
        if (!frameOut || image.empty() ||
            image.format != core::PixelFormat::Rgba8 ||
            image.strideBytes < image.size.width * 4) {
            return fail(errorOut, "invalid neutral RGBA preview frame");
        }
        *frameOut = {};
        if (!initialize(errorOut) ||
            !ensureResources(image.size, errorOut)) {
            return false;
        }

        void* mapped = nullptr;
        if (vkMapMemory(
                device, stagingMemory, 0,
                stagingCapacity, 0, &mapped) != VK_SUCCESS) {
            return fail(errorOut, "failed to map Vulkan upload memory");
        }
        const std::size_t rowBytes =
            static_cast<std::size_t>(image.size.width) * 4;
        auto* destination =
            static_cast<std::uint8_t*>(mapped);
        for (int y = 0; y < image.size.height; ++y) {
            std::memcpy(
                destination + rowBytes * y,
                image.bytes.data() +
                    static_cast<std::size_t>(
                        image.strideBytes) * y,
                rowBytes);
        }
        vkUnmapMemory(device, stagingMemory);

        vkResetCommandBuffer(commandBuffer, 0);
        VkCommandBufferBeginInfo begin{};
        begin.sType =
            VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
        begin.flags =
            VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        if (vkBeginCommandBuffer(
                commandBuffer, &begin) != VK_SUCCESS) {
            return fail(errorOut, "failed to begin Vulkan upload");
        }
        transition(
            imageLayout,
            VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
            imageLayout == VK_IMAGE_LAYOUT_UNDEFINED
                ? VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT
                : VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
            VK_PIPELINE_STAGE_TRANSFER_BIT,
            imageLayout == VK_IMAGE_LAYOUT_UNDEFINED
                ? 0
                : VK_ACCESS_SHADER_READ_BIT,
            VK_ACCESS_TRANSFER_WRITE_BIT);

        VkBufferImageCopy copy{};
        copy.imageSubresource.aspectMask =
            VK_IMAGE_ASPECT_COLOR_BIT;
        copy.imageSubresource.layerCount = 1;
        copy.imageExtent = {
            static_cast<std::uint32_t>(image.size.width),
            static_cast<std::uint32_t>(image.size.height),
            1};
        vkCmdCopyBufferToImage(
            commandBuffer,
            stagingBuffer,
            outputImage,
            VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
            1,
            &copy);
        transition(
            VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
            VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
            VK_PIPELINE_STAGE_TRANSFER_BIT,
            VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
            VK_ACCESS_TRANSFER_WRITE_BIT,
            VK_ACCESS_SHADER_READ_BIT);
        if (vkEndCommandBuffer(commandBuffer) != VK_SUCCESS) {
            return fail(errorOut, "failed to finish Vulkan upload");
        }
        vkResetFences(device, 1, &fence);
        VkSubmitInfo submit{};
        submit.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
        submit.commandBufferCount = 1;
        submit.pCommandBuffers = &commandBuffer;
        if (vkQueueSubmit(queue, 1, &submit, fence) != VK_SUCCESS ||
            vkWaitForFences(
                device, 1, &fence, VK_TRUE,
                kHeadlessCompositorWaitTimeoutNs) !=
                VK_SUCCESS) {
            return fail(errorOut, "Vulkan upload submission failed");
        }
        imageLayout =
            VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        frameOut->physicalDevice = physicalDevice;
        frameOut->device = device;
        frameOut->queue = queue;
        frameOut->queueFamilyIndex = queueFamilyIndex;
        frameOut->image = outputImage;
        frameOut->imageView = outputView;
        frameOut->imageMemory = outputMemory;
        frameOut->imageLayout = imageLayout;
        frameOut->imageFormat = VK_FORMAT_R8G8B8A8_UNORM;
        frameOut->size = image.size;
        frameOut->queueSupportsCompute = true;
        frameOut->valid = true;
        return true;
    }

    bool compose(
        const std::vector<core::ImageBuffer>& layers,
        render_detail::OffscreenVulkanFrame* frameOut,
        std::string* errorOut)
    {
        if (layers.empty()) {
            return fail(errorOut, "neutral compositor has no layers");
        }
        const core::SizeI size = layers.front().size;
        for (const core::ImageBuffer& layer : layers) {
            if (layer.empty() ||
                layer.format != core::PixelFormat::Rgba8 ||
                layer.size.width != size.width ||
                layer.size.height != size.height) {
                return fail(
                    errorOut,
                    "neutral compositor layers have incompatible dimensions");
            }
        }
        if (!upload(layers.front(), frameOut, errorOut)) {
            return false;
        }
        if (layers.size() == 1) return true;
        if (!ensureCompositionResources(size, errorOut)) {
            return false;
        }
        for (std::size_t index = 1;
             index < layers.size();
             ++index) {
            if (!compositeLayer(layers[index], errorOut)) {
                return false;
            }
        }
        frameOut->imageLayout = imageLayout;
        frameOut->valid = true;
        return true;
    }

    bool readback(
        core::ImageBuffer* imageOut,
        std::string* errorOut)
    {
        if (!imageOut || !outputImage ||
            !outputSize.valid()) {
            return fail(
                errorOut,
                "neutral Vulkan readback has no frame");
        }
        vkResetCommandBuffer(commandBuffer, 0);
        VkCommandBufferBeginInfo begin{};
        begin.sType =
            VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
        begin.flags =
            VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        if (vkBeginCommandBuffer(
                commandBuffer, &begin) != VK_SUCCESS) {
            return fail(
                errorOut,
                "failed to begin neutral Vulkan readback");
        }
        transition(
            imageLayout,
            VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
            VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
            VK_PIPELINE_STAGE_TRANSFER_BIT,
            VK_ACCESS_SHADER_READ_BIT,
            VK_ACCESS_TRANSFER_READ_BIT);
        VkBufferImageCopy copy{};
        copy.imageSubresource.aspectMask =
            VK_IMAGE_ASPECT_COLOR_BIT;
        copy.imageSubresource.layerCount = 1;
        copy.imageExtent = {
            static_cast<std::uint32_t>(outputSize.width),
            static_cast<std::uint32_t>(outputSize.height), 1};
        vkCmdCopyImageToBuffer(
            commandBuffer,
            outputImage,
            VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
            stagingBuffer,
            1,
            &copy);
        transition(
            VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
            VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
            VK_PIPELINE_STAGE_TRANSFER_BIT,
            VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
            VK_ACCESS_TRANSFER_READ_BIT,
            VK_ACCESS_SHADER_READ_BIT);
        if (vkEndCommandBuffer(commandBuffer) != VK_SUCCESS) {
            return fail(
                errorOut,
                "failed to finish neutral Vulkan readback");
        }
        vkResetFences(device, 1, &fence);
        VkSubmitInfo submit{};
        submit.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
        submit.commandBufferCount = 1;
        submit.pCommandBuffers = &commandBuffer;
        if (vkQueueSubmit(queue, 1, &submit, fence) != VK_SUCCESS ||
            vkWaitForFences(
                device, 1, &fence, VK_TRUE,
                kHeadlessCompositorWaitTimeoutNs) !=
                VK_SUCCESS) {
            return fail(
                errorOut,
                "neutral Vulkan readback submission failed");
        }
        void* mapped = nullptr;
        if (vkMapMemory(
                device, stagingMemory, 0,
                stagingCapacity, 0, &mapped) != VK_SUCCESS) {
            return fail(
                errorOut,
                "failed to map neutral Vulkan readback");
        }
        imageOut->format = core::PixelFormat::Rgba8;
        imageOut->size = outputSize;
        imageOut->strideBytes = outputSize.width * 4;
        imageOut->bytes.resize(
            static_cast<std::size_t>(
                imageOut->strideBytes) *
            outputSize.height);
        std::memcpy(
            imageOut->bytes.data(),
            mapped,
            imageOut->bytes.size());
        vkUnmapMemory(device, stagingMemory);
        imageLayout =
            VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        return true;
    }

    void release()
    {
        if (!device) {
            if (instance) {
                vkDestroyInstance(instance, nullptr);
                instance = VK_NULL_HANDLE;
            }
            return;
        }
        vkDeviceWaitIdle(device);
        destroyCompositionResources();
        destroyFrameResources();
        if (fence) vkDestroyFence(device, fence, nullptr);
        if (commandPool) {
            vkDestroyCommandPool(device, commandPool, nullptr);
        }
        vkDestroyDevice(device, nullptr);
        device = VK_NULL_HANDLE;
        if (instance) vkDestroyInstance(instance, nullptr);
        instance = VK_NULL_HANDLE;
    }

private:
    bool initialize(std::string* errorOut)
    {
        if (device) return true;
        VkApplicationInfo application{};
        application.sType =
            VK_STRUCTURE_TYPE_APPLICATION_INFO;
        application.pApplicationName =
            "jcut-imgui-neutral-renderer";
        application.apiVersion = VK_API_VERSION_1_1;
        VkInstanceCreateInfo instanceInfo{};
        instanceInfo.sType =
            VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
        instanceInfo.pApplicationInfo = &application;
        if (vkCreateInstance(
                &instanceInfo, nullptr, &instance) != VK_SUCCESS) {
            return fail(errorOut, "failed to create Vulkan instance");
        }
        std::uint32_t physicalCount = 0;
        vkEnumeratePhysicalDevices(
            instance, &physicalCount, nullptr);
        std::vector<VkPhysicalDevice> devices(physicalCount);
        vkEnumeratePhysicalDevices(
            instance, &physicalCount, devices.data());
        int bestScore = -1;
        for (VkPhysicalDevice candidate : devices) {
            if (!supportsDeviceExtension(
                    candidate,
                    VK_KHR_EXTERNAL_MEMORY_FD_EXTENSION_NAME)) {
                continue;
            }
            std::uint32_t queueCount = 0;
            vkGetPhysicalDeviceQueueFamilyProperties(
                candidate, &queueCount, nullptr);
            std::vector<VkQueueFamilyProperties>
                queues(queueCount);
            vkGetPhysicalDeviceQueueFamilyProperties(
                candidate, &queueCount, queues.data());
            for (std::uint32_t index = 0;
                 index < queueCount;
                 ++index) {
                if ((queues[index].queueFlags &
                     VK_QUEUE_GRAPHICS_BIT) == 0) {
                    continue;
                }
                VkPhysicalDeviceProperties properties{};
                vkGetPhysicalDeviceProperties(
                    candidate, &properties);
                const int score =
                    properties.deviceType ==
                            VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU
                    ? 2
                    : 1;
                if (score > bestScore) {
                    bestScore = score;
                    physicalDevice = candidate;
                    queueFamilyIndex = index;
                }
                break;
            }
        }
        if (!physicalDevice) {
            return fail(
                errorOut,
                "no Vulkan device supports external-memory frames");
        }
        const float priority = 1.0f;
        VkDeviceQueueCreateInfo queueInfo{};
        queueInfo.sType =
            VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
        queueInfo.queueFamilyIndex = queueFamilyIndex;
        queueInfo.queueCount = 1;
        queueInfo.pQueuePriorities = &priority;
        const char* extensions[] = {
            VK_KHR_EXTERNAL_MEMORY_EXTENSION_NAME,
            VK_KHR_EXTERNAL_MEMORY_FD_EXTENSION_NAME};
        VkDeviceCreateInfo deviceInfo{};
        deviceInfo.sType =
            VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
        deviceInfo.queueCreateInfoCount = 1;
        deviceInfo.pQueueCreateInfos = &queueInfo;
        deviceInfo.enabledExtensionCount = 2;
        deviceInfo.ppEnabledExtensionNames = extensions;
        if (vkCreateDevice(
                physicalDevice,
                &deviceInfo,
                nullptr,
                &device) != VK_SUCCESS) {
            return fail(errorOut, "failed to create Vulkan device");
        }
        vkGetDeviceQueue(
            device, queueFamilyIndex, 0, &queue);
        VkCommandPoolCreateInfo pool{};
        pool.sType =
            VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
        pool.flags =
            VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
        pool.queueFamilyIndex = queueFamilyIndex;
        if (vkCreateCommandPool(
                device, &pool, nullptr,
                &commandPool) != VK_SUCCESS) {
            return fail(
                errorOut,
                "failed to create Vulkan command pool");
        }
        VkCommandBufferAllocateInfo commands{};
        commands.sType =
            VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
        commands.commandPool = commandPool;
        commands.level =
            VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        commands.commandBufferCount = 1;
        if (vkAllocateCommandBuffers(
                device, &commands,
                &commandBuffer) != VK_SUCCESS) {
            return fail(
                errorOut,
                "failed to allocate Vulkan command buffer");
        }
        VkFenceCreateInfo fenceInfo{};
        fenceInfo.sType =
            VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
        if (vkCreateFence(
                device, &fenceInfo, nullptr,
                &fence) != VK_SUCCESS) {
            return fail(errorOut, "failed to create Vulkan fence");
        }
        return true;
    }

    bool ensureResources(
        core::SizeI size,
        std::string* errorOut)
    {
        if (outputImage && outputSize.width == size.width &&
            outputSize.height == size.height) {
            return true;
        }
        vkDeviceWaitIdle(device);
        destroyCompositionResources();
        destroyFrameResources();
        const VkDeviceSize bytes =
            static_cast<VkDeviceSize>(size.width) *
            static_cast<VkDeviceSize>(size.height) * 4;

        VkExternalMemoryImageCreateInfo externalImage{};
        externalImage.sType =
            VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_IMAGE_CREATE_INFO;
        externalImage.handleTypes =
            VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_FD_BIT;
        VkImageCreateInfo imageInfo{};
        imageInfo.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
        imageInfo.pNext = &externalImage;
        imageInfo.imageType = VK_IMAGE_TYPE_2D;
        imageInfo.format = VK_FORMAT_R8G8B8A8_UNORM;
        imageInfo.extent = {
            static_cast<std::uint32_t>(size.width),
            static_cast<std::uint32_t>(size.height), 1};
        imageInfo.mipLevels = 1;
        imageInfo.arrayLayers = 1;
        imageInfo.samples = VK_SAMPLE_COUNT_1_BIT;
        imageInfo.tiling = VK_IMAGE_TILING_OPTIMAL;
        imageInfo.usage =
            VK_IMAGE_USAGE_TRANSFER_DST_BIT |
            VK_IMAGE_USAGE_TRANSFER_SRC_BIT |
            VK_IMAGE_USAGE_SAMPLED_BIT |
            VK_IMAGE_USAGE_STORAGE_BIT;
        imageInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
        imageInfo.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        if (vkCreateImage(
                device, &imageInfo, nullptr,
                &outputImage) != VK_SUCCESS) {
            return fail(
                errorOut,
                "failed to create exportable Vulkan image");
        }
        VkMemoryRequirements imageRequirements{};
        vkGetImageMemoryRequirements(
            device, outputImage, &imageRequirements);
        const std::uint32_t imageMemoryType =
            findMemoryType(
                physicalDevice,
                imageRequirements.memoryTypeBits,
                VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
        if (imageMemoryType == UINT32_MAX) {
            return fail(
                errorOut,
                "no Vulkan image memory type is available");
        }
        VkExportMemoryAllocateInfo exportMemory{};
        exportMemory.sType =
            VK_STRUCTURE_TYPE_EXPORT_MEMORY_ALLOCATE_INFO;
        exportMemory.handleTypes =
            VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_FD_BIT;
        VkMemoryAllocateInfo imageAllocation{};
        imageAllocation.sType =
            VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
        imageAllocation.pNext = &exportMemory;
        imageAllocation.allocationSize =
            imageRequirements.size;
        imageAllocation.memoryTypeIndex =
            imageMemoryType;
        if (vkAllocateMemory(
                device, &imageAllocation, nullptr,
                &outputMemory) != VK_SUCCESS ||
            vkBindImageMemory(
                device, outputImage,
                outputMemory, 0) != VK_SUCCESS) {
            return fail(
                errorOut,
                "failed to allocate exportable Vulkan image");
        }
        VkImageViewCreateInfo view{};
        view.sType =
            VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
        view.image = outputImage;
        view.viewType = VK_IMAGE_VIEW_TYPE_2D;
        view.format = VK_FORMAT_R8G8B8A8_UNORM;
        view.subresourceRange.aspectMask =
            VK_IMAGE_ASPECT_COLOR_BIT;
        view.subresourceRange.levelCount = 1;
        view.subresourceRange.layerCount = 1;
        if (vkCreateImageView(
                device, &view, nullptr,
                &outputView) != VK_SUCCESS) {
            return fail(
                errorOut,
                "failed to create Vulkan output view");
        }

        VkBufferCreateInfo bufferInfo{};
        bufferInfo.sType =
            VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
        bufferInfo.size = bytes;
        bufferInfo.usage =
            VK_BUFFER_USAGE_TRANSFER_SRC_BIT |
            VK_BUFFER_USAGE_TRANSFER_DST_BIT;
        bufferInfo.sharingMode =
            VK_SHARING_MODE_EXCLUSIVE;
        if (vkCreateBuffer(
                device, &bufferInfo, nullptr,
                &stagingBuffer) != VK_SUCCESS) {
            return fail(
                errorOut,
                "failed to create Vulkan upload buffer");
        }
        VkMemoryRequirements bufferRequirements{};
        vkGetBufferMemoryRequirements(
            device, stagingBuffer,
            &bufferRequirements);
        const std::uint32_t bufferMemoryType =
            findMemoryType(
                physicalDevice,
                bufferRequirements.memoryTypeBits,
                VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                    VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
        if (bufferMemoryType == UINT32_MAX) {
            return fail(
                errorOut,
                "no host-visible Vulkan memory type is available");
        }
        VkMemoryAllocateInfo bufferAllocation{};
        bufferAllocation.sType =
            VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
        bufferAllocation.allocationSize =
            bufferRequirements.size;
        bufferAllocation.memoryTypeIndex =
            bufferMemoryType;
        if (vkAllocateMemory(
                device, &bufferAllocation, nullptr,
                &stagingMemory) != VK_SUCCESS ||
            vkBindBufferMemory(
                device, stagingBuffer,
                stagingMemory, 0) != VK_SUCCESS) {
            return fail(
                errorOut,
                "failed to allocate Vulkan upload memory");
        }
        stagingCapacity = bytes;
        outputSize = size;
        imageLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        return true;
    }

    void transition(
        VkImageLayout oldLayout,
        VkImageLayout newLayout,
        VkPipelineStageFlags sourceStage,
        VkPipelineStageFlags destinationStage,
        VkAccessFlags sourceAccess,
        VkAccessFlags destinationAccess)
    {
        transitionImage(
            outputImage,
            oldLayout,
            newLayout,
            sourceStage,
            destinationStage,
            sourceAccess,
            destinationAccess);
    }

    void transitionImage(
        VkImage targetImage,
        VkImageLayout oldLayout,
        VkImageLayout newLayout,
        VkPipelineStageFlags sourceStage,
        VkPipelineStageFlags destinationStage,
        VkAccessFlags sourceAccess,
        VkAccessFlags destinationAccess)
    {
        VkImageMemoryBarrier barrier{};
        barrier.sType =
            VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
        barrier.oldLayout = oldLayout;
        barrier.newLayout = newLayout;
        barrier.srcQueueFamilyIndex =
            VK_QUEUE_FAMILY_IGNORED;
        barrier.dstQueueFamilyIndex =
            VK_QUEUE_FAMILY_IGNORED;
        barrier.image = targetImage;
        barrier.subresourceRange.aspectMask =
            VK_IMAGE_ASPECT_COLOR_BIT;
        barrier.subresourceRange.levelCount = 1;
        barrier.subresourceRange.layerCount = 1;
        barrier.srcAccessMask = sourceAccess;
        barrier.dstAccessMask = destinationAccess;
        vkCmdPipelineBarrier(
            commandBuffer,
            sourceStage,
            destinationStage,
            0, 0, nullptr, 0, nullptr, 1, &barrier);
    }

    bool ensureCompositionResources(
        core::SizeI size,
        std::string* errorOut)
    {
        if (layerImage &&
            layerSize.width == size.width &&
            layerSize.height == size.height &&
            compositePipeline) {
            return true;
        }
        vkDeviceWaitIdle(device);
        destroyCompositionResources();

        VkImageCreateInfo imageInfo{};
        imageInfo.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
        imageInfo.imageType = VK_IMAGE_TYPE_2D;
        imageInfo.format = VK_FORMAT_R8G8B8A8_UNORM;
        imageInfo.extent = {
            static_cast<std::uint32_t>(size.width),
            static_cast<std::uint32_t>(size.height), 1};
        imageInfo.mipLevels = 1;
        imageInfo.arrayLayers = 1;
        imageInfo.samples = VK_SAMPLE_COUNT_1_BIT;
        imageInfo.tiling = VK_IMAGE_TILING_OPTIMAL;
        imageInfo.usage =
            VK_IMAGE_USAGE_TRANSFER_DST_BIT |
            VK_IMAGE_USAGE_STORAGE_BIT;
        imageInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
        imageInfo.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        if (vkCreateImage(
                device, &imageInfo, nullptr,
                &layerImage) != VK_SUCCESS) {
            return fail(
                errorOut,
                "failed to create neutral compositor layer image");
        }
        VkMemoryRequirements requirements{};
        vkGetImageMemoryRequirements(
            device, layerImage, &requirements);
        const std::uint32_t type = findMemoryType(
            physicalDevice,
            requirements.memoryTypeBits,
            VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
        VkMemoryAllocateInfo allocation{};
        allocation.sType =
            VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
        allocation.allocationSize = requirements.size;
        allocation.memoryTypeIndex = type;
        if (type == UINT32_MAX ||
            vkAllocateMemory(
                device, &allocation, nullptr,
                &layerMemory) != VK_SUCCESS ||
            vkBindImageMemory(
                device, layerImage,
                layerMemory, 0) != VK_SUCCESS) {
            return fail(
                errorOut,
                "failed to allocate neutral compositor layer");
        }
        VkImageViewCreateInfo view{};
        view.sType =
            VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
        view.image = layerImage;
        view.viewType = VK_IMAGE_VIEW_TYPE_2D;
        view.format = VK_FORMAT_R8G8B8A8_UNORM;
        view.subresourceRange.aspectMask =
            VK_IMAGE_ASPECT_COLOR_BIT;
        view.subresourceRange.levelCount = 1;
        view.subresourceRange.layerCount = 1;
        if (vkCreateImageView(
                device, &view, nullptr,
                &layerView) != VK_SUCCESS) {
            return fail(
                errorOut,
                "failed to create neutral compositor layer view");
        }

        std::array<VkDescriptorSetLayoutBinding, 2>
            bindings{};
        for (std::uint32_t index = 0;
             index < bindings.size();
             ++index) {
            bindings[index].binding = index;
            bindings[index].descriptorType =
                VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
            bindings[index].descriptorCount = 1;
            bindings[index].stageFlags =
                VK_SHADER_STAGE_COMPUTE_BIT;
        }
        VkDescriptorSetLayoutCreateInfo layoutInfo{};
        layoutInfo.sType =
            VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
        layoutInfo.bindingCount = bindings.size();
        layoutInfo.pBindings = bindings.data();
        if (vkCreateDescriptorSetLayout(
                device, &layoutInfo, nullptr,
                &compositeDescriptorLayout) != VK_SUCCESS) {
            return fail(
                errorOut,
                "failed to create neutral compositor descriptor layout");
        }
        VkDescriptorPoolSize poolSize{
            VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 2};
        VkDescriptorPoolCreateInfo poolInfo{};
        poolInfo.sType =
            VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
        poolInfo.maxSets = 1;
        poolInfo.poolSizeCount = 1;
        poolInfo.pPoolSizes = &poolSize;
        if (vkCreateDescriptorPool(
                device, &poolInfo, nullptr,
                &compositeDescriptorPool) != VK_SUCCESS) {
            return fail(
                errorOut,
                "failed to create neutral compositor descriptor pool");
        }
        VkDescriptorSetAllocateInfo setAllocation{};
        setAllocation.sType =
            VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
        setAllocation.descriptorPool =
            compositeDescriptorPool;
        setAllocation.descriptorSetCount = 1;
        setAllocation.pSetLayouts =
            &compositeDescriptorLayout;
        if (vkAllocateDescriptorSets(
                device, &setAllocation,
                &compositeDescriptor) != VK_SUCCESS) {
            return fail(
                errorOut,
                "failed to allocate neutral compositor descriptors");
        }
        VkDescriptorImageInfo destinationInfo{};
        destinationInfo.imageView = outputView;
        destinationInfo.imageLayout =
            VK_IMAGE_LAYOUT_GENERAL;
        VkDescriptorImageInfo sourceInfo{};
        sourceInfo.imageView = layerView;
        sourceInfo.imageLayout =
            VK_IMAGE_LAYOUT_GENERAL;
        std::array<VkWriteDescriptorSet, 2> writes{};
        writes[0].sType =
            VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        writes[0].dstSet = compositeDescriptor;
        writes[0].dstBinding = 0;
        writes[0].descriptorCount = 1;
        writes[0].descriptorType =
            VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
        writes[0].pImageInfo = &destinationInfo;
        writes[1] = writes[0];
        writes[1].dstBinding = 1;
        writes[1].pImageInfo = &sourceInfo;
        vkUpdateDescriptorSets(
            device, writes.size(), writes.data(),
            0, nullptr);

        VkPipelineLayoutCreateInfo pipelineLayoutInfo{};
        pipelineLayoutInfo.sType =
            VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
        pipelineLayoutInfo.setLayoutCount = 1;
        pipelineLayoutInfo.pSetLayouts =
            &compositeDescriptorLayout;
        if (vkCreatePipelineLayout(
                device, &pipelineLayoutInfo, nullptr,
                &compositePipelineLayout) != VK_SUCCESS) {
            return fail(
                errorOut,
                "failed to create neutral compositor pipeline layout");
        }
        const std::vector<std::uint32_t> words =
            readSpirv(
                std::filesystem::path(
                    JCUT_VULKAN_SHADER_DIR) /
                "neutral_composite.comp.spv");
        if (words.empty()) {
            return fail(
                errorOut,
                "neutral compositor shader is unavailable");
        }
        VkShaderModuleCreateInfo moduleInfo{};
        moduleInfo.sType =
            VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
        moduleInfo.codeSize =
            words.size() * sizeof(std::uint32_t);
        moduleInfo.pCode = words.data();
        VkShaderModule module = VK_NULL_HANDLE;
        if (vkCreateShaderModule(
                device, &moduleInfo, nullptr,
                &module) != VK_SUCCESS) {
            return fail(
                errorOut,
                "failed to create neutral compositor shader");
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
        pipelineInfo.layout =
            compositePipelineLayout;
        const VkResult pipelineResult =
            vkCreateComputePipelines(
                device, VK_NULL_HANDLE, 1,
                &pipelineInfo, nullptr,
                &compositePipeline);
        vkDestroyShaderModule(device, module, nullptr);
        if (pipelineResult != VK_SUCCESS) {
            return fail(
                errorOut,
                "failed to create neutral compositor pipeline");
        }
        layerSize = size;
        layerLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        return true;
    }

    bool compositeLayer(
        const core::ImageBuffer& layer,
        std::string* errorOut)
    {
        void* mapped = nullptr;
        if (vkMapMemory(
                device, stagingMemory, 0,
                stagingCapacity, 0, &mapped) != VK_SUCCESS) {
            return fail(
                errorOut,
                "failed to map neutral compositor layer");
        }
        const std::size_t rowBytes =
            static_cast<std::size_t>(layer.size.width) * 4;
        auto* destination =
            static_cast<std::uint8_t*>(mapped);
        for (int y = 0; y < layer.size.height; ++y) {
            std::memcpy(
                destination + rowBytes * y,
                layer.bytes.data() +
                    static_cast<std::size_t>(
                        layer.strideBytes) * y,
                rowBytes);
        }
        vkUnmapMemory(device, stagingMemory);
        vkResetCommandBuffer(commandBuffer, 0);
        VkCommandBufferBeginInfo begin{};
        begin.sType =
            VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
        begin.flags =
            VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        if (vkBeginCommandBuffer(
                commandBuffer, &begin) != VK_SUCCESS) {
            return fail(
                errorOut,
                "failed to begin neutral composition");
        }
        transitionImage(
            layerImage,
            layerLayout,
            VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
            layerLayout == VK_IMAGE_LAYOUT_UNDEFINED
                ? VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT
                : VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
            VK_PIPELINE_STAGE_TRANSFER_BIT,
            layerLayout == VK_IMAGE_LAYOUT_UNDEFINED
                ? 0
                : VK_ACCESS_SHADER_READ_BIT,
            VK_ACCESS_TRANSFER_WRITE_BIT);
        VkBufferImageCopy copy{};
        copy.imageSubresource.aspectMask =
            VK_IMAGE_ASPECT_COLOR_BIT;
        copy.imageSubresource.layerCount = 1;
        copy.imageExtent = {
            static_cast<std::uint32_t>(layer.size.width),
            static_cast<std::uint32_t>(layer.size.height), 1};
        vkCmdCopyBufferToImage(
            commandBuffer,
            stagingBuffer,
            layerImage,
            VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
            1, &copy);
        transitionImage(
            layerImage,
            VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
            VK_IMAGE_LAYOUT_GENERAL,
            VK_PIPELINE_STAGE_TRANSFER_BIT,
            VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
            VK_ACCESS_TRANSFER_WRITE_BIT,
            VK_ACCESS_SHADER_READ_BIT);
        transition(
            imageLayout,
            VK_IMAGE_LAYOUT_GENERAL,
            VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
            VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
            VK_ACCESS_SHADER_READ_BIT,
            VK_ACCESS_SHADER_READ_BIT |
                VK_ACCESS_SHADER_WRITE_BIT);
        vkCmdBindPipeline(
            commandBuffer,
            VK_PIPELINE_BIND_POINT_COMPUTE,
            compositePipeline);
        vkCmdBindDescriptorSets(
            commandBuffer,
            VK_PIPELINE_BIND_POINT_COMPUTE,
            compositePipelineLayout,
            0, 1, &compositeDescriptor,
            0, nullptr);
        vkCmdDispatch(
            commandBuffer,
            static_cast<std::uint32_t>(
                (layer.size.width + 15) / 16),
            static_cast<std::uint32_t>(
                (layer.size.height + 15) / 16),
            1);
        transition(
            VK_IMAGE_LAYOUT_GENERAL,
            VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
            VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
            VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
            VK_ACCESS_SHADER_WRITE_BIT,
            VK_ACCESS_SHADER_READ_BIT);
        if (vkEndCommandBuffer(commandBuffer) != VK_SUCCESS) {
            return fail(
                errorOut,
                "failed to finish neutral composition");
        }
        vkResetFences(device, 1, &fence);
        VkSubmitInfo submit{};
        submit.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
        submit.commandBufferCount = 1;
        submit.pCommandBuffers = &commandBuffer;
        if (vkQueueSubmit(queue, 1, &submit, fence) != VK_SUCCESS ||
            vkWaitForFences(
                device, 1, &fence, VK_TRUE,
                kHeadlessCompositorWaitTimeoutNs) !=
                VK_SUCCESS) {
            return fail(
                errorOut,
                "neutral composition submission failed");
        }
        layerLayout = VK_IMAGE_LAYOUT_GENERAL;
        imageLayout =
            VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        return true;
    }

    void destroyCompositionResources()
    {
        if (!device) return;
        if (compositePipeline) {
            vkDestroyPipeline(
                device, compositePipeline, nullptr);
        }
        if (compositePipelineLayout) {
            vkDestroyPipelineLayout(
                device,
                compositePipelineLayout,
                nullptr);
        }
        if (compositeDescriptorPool) {
            vkDestroyDescriptorPool(
                device,
                compositeDescriptorPool,
                nullptr);
        }
        if (compositeDescriptorLayout) {
            vkDestroyDescriptorSetLayout(
                device,
                compositeDescriptorLayout,
                nullptr);
        }
        if (layerView) {
            vkDestroyImageView(device, layerView, nullptr);
        }
        if (layerImage) {
            vkDestroyImage(device, layerImage, nullptr);
        }
        if (layerMemory) {
            vkFreeMemory(device, layerMemory, nullptr);
        }
        compositePipeline = VK_NULL_HANDLE;
        compositePipelineLayout = VK_NULL_HANDLE;
        compositeDescriptorPool = VK_NULL_HANDLE;
        compositeDescriptorLayout = VK_NULL_HANDLE;
        compositeDescriptor = VK_NULL_HANDLE;
        layerView = VK_NULL_HANDLE;
        layerImage = VK_NULL_HANDLE;
        layerMemory = VK_NULL_HANDLE;
        layerSize = {};
        layerLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    }

    void destroyFrameResources()
    {
        if (outputView) {
            vkDestroyImageView(device, outputView, nullptr);
        }
        if (outputImage) {
            vkDestroyImage(device, outputImage, nullptr);
        }
        if (outputMemory) {
            vkFreeMemory(device, outputMemory, nullptr);
        }
        if (stagingBuffer) {
            vkDestroyBuffer(device, stagingBuffer, nullptr);
        }
        if (stagingMemory) {
            vkFreeMemory(device, stagingMemory, nullptr);
        }
        outputView = VK_NULL_HANDLE;
        outputImage = VK_NULL_HANDLE;
        outputMemory = VK_NULL_HANDLE;
        stagingBuffer = VK_NULL_HANDLE;
        stagingMemory = VK_NULL_HANDLE;
        stagingCapacity = 0;
        outputSize = {};
        imageLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    }

    VkInstance instance = VK_NULL_HANDLE;
    VkPhysicalDevice physicalDevice = VK_NULL_HANDLE;
    VkDevice device = VK_NULL_HANDLE;
    VkQueue queue = VK_NULL_HANDLE;
    std::uint32_t queueFamilyIndex = UINT32_MAX;
    VkCommandPool commandPool = VK_NULL_HANDLE;
    VkCommandBuffer commandBuffer = VK_NULL_HANDLE;
    VkFence fence = VK_NULL_HANDLE;
    VkImage outputImage = VK_NULL_HANDLE;
    VkDeviceMemory outputMemory = VK_NULL_HANDLE;
    VkImageView outputView = VK_NULL_HANDLE;
    VkImageLayout imageLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    core::SizeI outputSize;
    VkBuffer stagingBuffer = VK_NULL_HANDLE;
    VkDeviceMemory stagingMemory = VK_NULL_HANDLE;
    VkDeviceSize stagingCapacity = 0;
    VkImage layerImage = VK_NULL_HANDLE;
    VkDeviceMemory layerMemory = VK_NULL_HANDLE;
    VkImageView layerView = VK_NULL_HANDLE;
    VkImageLayout layerLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    core::SizeI layerSize;
    VkDescriptorSetLayout compositeDescriptorLayout =
        VK_NULL_HANDLE;
    VkDescriptorPool compositeDescriptorPool =
        VK_NULL_HANDLE;
    VkDescriptorSet compositeDescriptor = VK_NULL_HANDLE;
    VkPipelineLayout compositePipelineLayout =
        VK_NULL_HANDLE;
    VkPipeline compositePipeline = VK_NULL_HANDLE;
};

VulkanCompositorCore::VulkanCompositorCore()
    : impl_(std::make_unique<Impl>())
{
}

VulkanCompositorCore::~VulkanCompositorCore() = default;

bool VulkanCompositorCore::upload(
    const core::ImageBuffer& image,
    render_detail::OffscreenVulkanFrame* frameOut,
    std::string* errorOut)
{
    return impl_->upload(image, frameOut, errorOut);
}

bool VulkanCompositorCore::compose(
    const std::vector<core::ImageBuffer>& layers,
    render_detail::OffscreenVulkanFrame* frameOut,
    std::string* errorOut)
{
    return impl_->compose(layers, frameOut, errorOut);
}

bool VulkanCompositorCore::readback(
    core::ImageBuffer* imageOut,
    std::string* errorOut)
{
    return impl_->readback(imageOut, errorOut);
}

void VulkanCompositorCore::release()
{
    impl_->release();
}

} // namespace jcut::vulkan
