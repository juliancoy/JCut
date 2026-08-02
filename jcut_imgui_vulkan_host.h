#pragma once

struct VulkanShell {
    struct AuxiliaryTexture {
        VkImage image = VK_NULL_HANDLE;
        VkDeviceMemory memory = VK_NULL_HANDLE;
        VkImageView view = VK_NULL_HANDLE;
        jcut::core::SizeI size{};
        VkImageLayout layout = VK_IMAGE_LAYOUT_UNDEFINED;
        VkDescriptorSet descriptor = VK_NULL_HANDLE;
    };

    X11Platform* platform = nullptr;
    VkInstance instance = VK_NULL_HANDLE;
    VkSurfaceKHR surface = VK_NULL_HANDLE;
    VkPhysicalDevice physicalDevice = VK_NULL_HANDLE;
    VkDevice device = VK_NULL_HANDLE;
    uint32_t queueFamily = UINT32_MAX;
    VkQueue queue = VK_NULL_HANDLE;
    VkDescriptorPool descriptorPool = VK_NULL_HANDLE;
    VkSampler previewSampler = VK_NULL_HANDLE;
    VkCommandPool uploadCommandPool = VK_NULL_HANDLE;
    VkImage previewImage = VK_NULL_HANDLE;
    VkDeviceMemory previewImageMemory = VK_NULL_HANDLE;
    VkImageView previewImageView = VK_NULL_HANDLE;
    jcut::core::SizeI previewImageSize{};
    VkImageLayout previewImageLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    ImGui_ImplVulkanH_Window windowData{};
    uint32_t minImageCount = 2;
    bool swapchainRebuild = false;
    jcut::imgui::VulkanFrameImporter previewHandoff;
    bool previewHandoffInitialized = false;
    std::string previewHandoffStatus;
    jcut::vulkan_import::VulkanHardwareFrameImportCore
        hardwareFrameHandoff;
    bool hardwareFrameHandoffInitialized = false;
    std::string hardwareFrameHandoffStatus;
    VkDescriptorSet previewTextureSet = VK_NULL_HANDLE;
    VkImageView boundPreviewView = VK_NULL_HANDLE;
    VkImageLayout boundPreviewLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    AuxiliaryTexture birefnetLivePreviewTexture;
    AuxiliaryTexture mediaThumbnailTexture;
    AuxiliaryTexture aiProfileAvatarTexture;
    AuxiliaryTexture faceReferenceTexture;
    AuxiliaryTexture sectionAvatarTexture;
    AuxiliaryTexture previewOverlayTexture;

    bool queueSupportsPresent(VkPhysicalDevice candidate, uint32_t family) const
    {
        VkBool32 supported = VK_FALSE;
        if (vkGetPhysicalDeviceSurfaceSupportKHR(candidate, family, surface, &supported) != VK_SUCCESS) {
            return false;
        }
        return supported == VK_TRUE;
    }

    bool selectQueueFamily(VkPhysicalDevice candidate, uint32_t* familyOut) const
    {
        uint32_t familyCount = 0;
        vkGetPhysicalDeviceQueueFamilyProperties(candidate, &familyCount, nullptr);
        std::vector<VkQueueFamilyProperties> families(familyCount);
        if (familyCount > 0) {
            vkGetPhysicalDeviceQueueFamilyProperties(candidate, &familyCount, families.data());
        }
        for (uint32_t i = 0; i < familyCount; ++i) {
            if ((families[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) && queueSupportsPresent(candidate, i)) {
                *familyOut = i;
                return true;
            }
        }
        return false;
    }

    bool createInstance(std::string* error)
    {
        uint32_t propertyCount = 0;
        if (vkEnumerateInstanceExtensionProperties(nullptr, &propertyCount, nullptr) != VK_SUCCESS) {
            if (error) *error = "Failed to enumerate Vulkan instance extensions.";
            return false;
        }
        std::vector<VkExtensionProperties> properties(propertyCount);
        if (propertyCount > 0 &&
            vkEnumerateInstanceExtensionProperties(nullptr, &propertyCount, properties.data()) != VK_SUCCESS) {
            if (error) *error = "Failed to read Vulkan instance extensions.";
            return false;
        }

        if (!hasVulkanExtension(properties, VK_KHR_SURFACE_EXTENSION_NAME) ||
            !hasVulkanExtension(properties, VK_KHR_XLIB_SURFACE_EXTENSION_NAME)) {
            if (error) *error = "Required Vulkan Xlib surface extensions are unavailable.";
            return false;
        }

        std::vector<const char*> extensions{
            VK_KHR_SURFACE_EXTENSION_NAME,
            VK_KHR_XLIB_SURFACE_EXTENSION_NAME,
        };
        if (hasVulkanExtension(properties, VK_KHR_GET_PHYSICAL_DEVICE_PROPERTIES_2_EXTENSION_NAME)) {
            extensions.push_back(VK_KHR_GET_PHYSICAL_DEVICE_PROPERTIES_2_EXTENSION_NAME);
        }
        VkInstanceCreateFlags flags = 0;
#ifdef VK_KHR_PORTABILITY_ENUMERATION_EXTENSION_NAME
        if (hasVulkanExtension(properties, VK_KHR_PORTABILITY_ENUMERATION_EXTENSION_NAME)) {
            extensions.push_back(VK_KHR_PORTABILITY_ENUMERATION_EXTENSION_NAME);
            flags |= VK_INSTANCE_CREATE_ENUMERATE_PORTABILITY_BIT_KHR;
        }
#endif

        VkApplicationInfo appInfo{};
        appInfo.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
        appInfo.pApplicationName = "jcut-imgui";
        appInfo.apiVersion = VK_API_VERSION_1_1;

        VkInstanceCreateInfo info{};
        info.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
        info.flags = flags;
        info.pApplicationInfo = &appInfo;
        info.enabledExtensionCount = static_cast<uint32_t>(extensions.size());
        info.ppEnabledExtensionNames = extensions.data();
        if (vkCreateInstance(&info, nullptr, &instance) != VK_SUCCESS) {
            if (error) *error = "Failed to create Vulkan instance.";
            return false;
        }
        return true;
    }

    bool selectPhysicalDevice(std::string* error)
    {
        uint32_t count = 0;
        if (vkEnumeratePhysicalDevices(instance, &count, nullptr) != VK_SUCCESS || count == 0) {
            if (error) *error = "No Vulkan physical device available.";
            return false;
        }
        std::vector<VkPhysicalDevice> devices(count);
        if (vkEnumeratePhysicalDevices(instance, &count, devices.data()) != VK_SUCCESS) {
            if (error) *error = "Failed to enumerate Vulkan physical devices.";
            return false;
        }
        for (VkPhysicalDevice candidate : devices) {
            uint32_t family = UINT32_MAX;
            if (selectQueueFamily(candidate, &family)) {
                physicalDevice = candidate;
                queueFamily = family;
                return true;
            }
        }
        if (error) *error = "No Vulkan graphics queue can present to the X11 surface.";
        return false;
    }

    bool createDevice(std::string* error)
    {
        uint32_t extensionCount = 0;
        vkEnumerateDeviceExtensionProperties(physicalDevice, nullptr, &extensionCount, nullptr);
        std::vector<VkExtensionProperties> properties(extensionCount);
        if (extensionCount > 0) {
            vkEnumerateDeviceExtensionProperties(physicalDevice, nullptr, &extensionCount, properties.data());
        }
        auto optionalExtension = [&](const char* name, std::vector<const char*>* extensions) {
            if (hasVulkanExtension(properties, name)) {
                extensions->push_back(name);
            }
        };
        if (!hasVulkanExtension(properties, VK_KHR_SWAPCHAIN_EXTENSION_NAME)) {
            if (error) *error = "Vulkan swapchain extension is unavailable.";
            return false;
        }
        std::vector<const char*> extensions{VK_KHR_SWAPCHAIN_EXTENSION_NAME};
        optionalExtension(VK_KHR_EXTERNAL_MEMORY_EXTENSION_NAME, &extensions);
#ifdef __linux__
        optionalExtension(VK_KHR_EXTERNAL_MEMORY_FD_EXTENSION_NAME, &extensions);
        optionalExtension(VK_KHR_EXTERNAL_SEMAPHORE_EXTENSION_NAME, &extensions);
        optionalExtension(VK_KHR_EXTERNAL_SEMAPHORE_FD_EXTENSION_NAME, &extensions);
#endif
        optionalExtension(VK_KHR_BIND_MEMORY_2_EXTENSION_NAME, &extensions);
        optionalExtension(VK_KHR_GET_MEMORY_REQUIREMENTS_2_EXTENSION_NAME, &extensions);
        optionalExtension(VK_KHR_MAINTENANCE1_EXTENSION_NAME, &extensions);
        optionalExtension(VK_KHR_MAINTENANCE3_EXTENSION_NAME, &extensions);
#ifdef VK_KHR_PORTABILITY_SUBSET_EXTENSION_NAME
        optionalExtension(VK_KHR_PORTABILITY_SUBSET_EXTENSION_NAME, &extensions);
#endif

        const float priority = 1.0f;
        VkDeviceQueueCreateInfo queueInfo{};
        queueInfo.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
        queueInfo.queueFamilyIndex = queueFamily;
        queueInfo.queueCount = 1;
        queueInfo.pQueuePriorities = &priority;

        VkDeviceCreateInfo info{};
        info.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
        info.queueCreateInfoCount = 1;
        info.pQueueCreateInfos = &queueInfo;
        info.enabledExtensionCount = static_cast<uint32_t>(extensions.size());
        info.ppEnabledExtensionNames = extensions.data();
        if (vkCreateDevice(physicalDevice, &info, nullptr, &device) != VK_SUCCESS) {
            if (error) *error = "Failed to create Vulkan device.";
            return false;
        }
        vkGetDeviceQueue(device, queueFamily, 0, &queue);
        return true;
    }

    bool createDescriptorPool(std::string* error)
    {
        const std::array<VkDescriptorPoolSize, 2> poolSizes{{
            {VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, IMGUI_IMPL_VULKAN_MINIMUM_SAMPLED_IMAGE_POOL_SIZE + 64},
            {VK_DESCRIPTOR_TYPE_SAMPLER, IMGUI_IMPL_VULKAN_MINIMUM_SAMPLER_POOL_SIZE + 16},
        }};
        VkDescriptorPoolCreateInfo info{};
        info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
        info.flags = VK_DESCRIPTOR_POOL_CREATE_FREE_DESCRIPTOR_SET_BIT;
        info.poolSizeCount = static_cast<uint32_t>(poolSizes.size());
        info.pPoolSizes = poolSizes.data();
        for (const VkDescriptorPoolSize& size : poolSizes) {
            info.maxSets += size.descriptorCount;
        }
        if (vkCreateDescriptorPool(device, &info, nullptr, &descriptorPool) != VK_SUCCESS) {
            if (error) *error = "Failed to create Vulkan descriptor pool.";
            return false;
        }
        return true;
    }

    bool createPreviewSampler(std::string* error)
    {
        VkSamplerCreateInfo info{};
        info.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
        info.magFilter = VK_FILTER_LINEAR;
        info.minFilter = VK_FILTER_LINEAR;
        info.mipmapMode = VK_SAMPLER_MIPMAP_MODE_LINEAR;
        info.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        info.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        info.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        info.maxLod = 1.0f;
        if (vkCreateSampler(device, &info, nullptr, &previewSampler) != VK_SUCCESS) {
            if (error) *error = "Failed to create Vulkan preview sampler.";
            return false;
        }
        return true;
    }

    bool createUploadCommandPool(std::string* error)
    {
        VkCommandPoolCreateInfo info{};
        info.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
        info.flags = VK_COMMAND_POOL_CREATE_TRANSIENT_BIT;
        info.queueFamilyIndex = queueFamily;
        if (vkCreateCommandPool(device, &info, nullptr, &uploadCommandPool) != VK_SUCCESS) {
            if (error) *error = "Failed to create Vulkan preview upload command pool.";
            return false;
        }
        return true;
    }

    uint32_t findMemoryType(uint32_t typeBits, VkMemoryPropertyFlags properties) const
    {
        VkPhysicalDeviceMemoryProperties memoryProperties{};
        vkGetPhysicalDeviceMemoryProperties(physicalDevice, &memoryProperties);
        for (uint32_t i = 0; i < memoryProperties.memoryTypeCount; ++i) {
            if ((typeBits & (1u << i)) &&
                (memoryProperties.memoryTypes[i].propertyFlags & properties) == properties) {
                return i;
            }
        }
        return UINT32_MAX;
    }

    bool createBuffer(VkDeviceSize size,
                      VkBufferUsageFlags usage,
                      VkMemoryPropertyFlags properties,
                      VkBuffer* buffer,
                      VkDeviceMemory* memory,
                      std::string* error)
    {
        VkBufferCreateInfo bufferInfo{};
        bufferInfo.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
        bufferInfo.size = size;
        bufferInfo.usage = usage;
        bufferInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
        if (vkCreateBuffer(device, &bufferInfo, nullptr, buffer) != VK_SUCCESS) {
            if (error) *error = "Failed to create Vulkan preview upload buffer.";
            return false;
        }

        VkMemoryRequirements requirements{};
        vkGetBufferMemoryRequirements(device, *buffer, &requirements);
        const uint32_t memoryType = findMemoryType(requirements.memoryTypeBits, properties);
        if (memoryType == UINT32_MAX) {
            if (error) *error = "No suitable memory type for Vulkan preview upload buffer.";
            vkDestroyBuffer(device, *buffer, nullptr);
            *buffer = VK_NULL_HANDLE;
            return false;
        }

        VkMemoryAllocateInfo allocInfo{};
        allocInfo.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
        allocInfo.allocationSize = requirements.size;
        allocInfo.memoryTypeIndex = memoryType;
        if (vkAllocateMemory(device, &allocInfo, nullptr, memory) != VK_SUCCESS) {
            if (error) *error = "Failed to allocate Vulkan preview upload buffer memory.";
            vkDestroyBuffer(device, *buffer, nullptr);
            *buffer = VK_NULL_HANDLE;
            return false;
        }
        vkBindBufferMemory(device, *buffer, *memory, 0);
        return true;
    }

    bool createPreviewImage(const jcut::core::SizeI& size, std::string* error)
    {
        VkImageCreateInfo imageInfo{};
        imageInfo.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
        imageInfo.imageType = VK_IMAGE_TYPE_2D;
        imageInfo.extent = {
            static_cast<uint32_t>(size.width),
            static_cast<uint32_t>(size.height),
            1
        };
        imageInfo.mipLevels = 1;
        imageInfo.arrayLayers = 1;
        imageInfo.format = VK_FORMAT_R8G8B8A8_UNORM;
        imageInfo.tiling = VK_IMAGE_TILING_OPTIMAL;
        imageInfo.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        imageInfo.usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
        imageInfo.samples = VK_SAMPLE_COUNT_1_BIT;
        imageInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
        if (vkCreateImage(device, &imageInfo, nullptr, &previewImage) != VK_SUCCESS) {
            if (error) *error = "Failed to create Vulkan preview image.";
            return false;
        }

        VkMemoryRequirements requirements{};
        vkGetImageMemoryRequirements(device, previewImage, &requirements);
        const uint32_t memoryType =
            findMemoryType(requirements.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
        if (memoryType == UINT32_MAX) {
            if (error) *error = "No suitable memory type for Vulkan preview image.";
            releasePreviewTextureResources();
            return false;
        }

        VkMemoryAllocateInfo allocInfo{};
        allocInfo.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
        allocInfo.allocationSize = requirements.size;
        allocInfo.memoryTypeIndex = memoryType;
        if (vkAllocateMemory(device, &allocInfo, nullptr, &previewImageMemory) != VK_SUCCESS) {
            if (error) *error = "Failed to allocate Vulkan preview image memory.";
            releasePreviewTextureResources();
            return false;
        }
        vkBindImageMemory(device, previewImage, previewImageMemory, 0);

        VkImageViewCreateInfo viewInfo{};
        viewInfo.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
        viewInfo.image = previewImage;
        viewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
        viewInfo.format = VK_FORMAT_R8G8B8A8_UNORM;
        viewInfo.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        viewInfo.subresourceRange.baseMipLevel = 0;
        viewInfo.subresourceRange.levelCount = 1;
        viewInfo.subresourceRange.baseArrayLayer = 0;
        viewInfo.subresourceRange.layerCount = 1;
        if (vkCreateImageView(device, &viewInfo, nullptr, &previewImageView) != VK_SUCCESS) {
            if (error) *error = "Failed to create Vulkan preview image view.";
            releasePreviewTextureResources();
            return false;
        }

        previewImageSize = size;
        previewImageLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        return true;
    }

    VkCommandBuffer beginUploadCommands(std::string* error)
    {
        VkCommandBufferAllocateInfo allocInfo{};
        allocInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
        allocInfo.commandPool = uploadCommandPool;
        allocInfo.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        allocInfo.commandBufferCount = 1;
        VkCommandBuffer commandBuffer = VK_NULL_HANDLE;
        if (vkAllocateCommandBuffers(device, &allocInfo, &commandBuffer) != VK_SUCCESS) {
            if (error) *error = "Failed to allocate Vulkan preview upload command buffer.";
            return VK_NULL_HANDLE;
        }

        VkCommandBufferBeginInfo beginInfo{};
        beginInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
        beginInfo.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        if (vkBeginCommandBuffer(commandBuffer, &beginInfo) != VK_SUCCESS) {
            if (error) *error = "Failed to begin Vulkan preview upload command buffer.";
            vkFreeCommandBuffers(device, uploadCommandPool, 1, &commandBuffer);
            return VK_NULL_HANDLE;
        }
        return commandBuffer;
    }

    bool endUploadCommands(VkCommandBuffer commandBuffer, std::string* error)
    {
        if (vkEndCommandBuffer(commandBuffer) != VK_SUCCESS) {
            if (error) *error = "Failed to finish Vulkan preview upload command buffer.";
            vkFreeCommandBuffers(device, uploadCommandPool, 1, &commandBuffer);
            return false;
        }
        VkSubmitInfo submitInfo{};
        submitInfo.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
        submitInfo.commandBufferCount = 1;
        submitInfo.pCommandBuffers = &commandBuffer;
        if (vkQueueSubmit(queue, 1, &submitInfo, VK_NULL_HANDLE) != VK_SUCCESS ||
            vkQueueWaitIdle(queue) != VK_SUCCESS) {
            if (error) *error = "Failed to submit Vulkan preview upload commands.";
            vkFreeCommandBuffers(device, uploadCommandPool, 1, &commandBuffer);
            return false;
        }
        vkFreeCommandBuffers(device, uploadCommandPool, 1, &commandBuffer);
        return true;
    }

    void transitionImage(VkCommandBuffer commandBuffer,
                         VkImage image,
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

        VkPipelineStageFlags srcStage = VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT;
        VkPipelineStageFlags dstStage = VK_PIPELINE_STAGE_TRANSFER_BIT;
        if (oldLayout == VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL) {
            barrier.srcAccessMask = VK_ACCESS_SHADER_READ_BIT;
            srcStage = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
        }
        if (newLayout == VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL) {
            barrier.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
            dstStage = VK_PIPELINE_STAGE_TRANSFER_BIT;
        } else if (newLayout == VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL) {
            barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
            barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
            srcStage = VK_PIPELINE_STAGE_TRANSFER_BIT;
            dstStage = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
        }

        vkCmdPipelineBarrier(commandBuffer,
                             srcStage,
                             dstStage,
                             0,
                             0,
                             nullptr,
                             0,
                             nullptr,
                             1,
                             &barrier);
    }

    void transitionPreviewImage(VkCommandBuffer commandBuffer,
                                VkImageLayout oldLayout,
                                VkImageLayout newLayout)
    {
        transitionImage(
            commandBuffer, previewImage, oldLayout, newLayout);
    }

    void releasePreviewTextureResources()
    {
        if (device == VK_NULL_HANDLE) {
            return;
        }
        if (previewTextureSet != VK_NULL_HANDLE) {
            ImGui_ImplVulkan_RemoveTexture(previewTextureSet);
            previewTextureSet = VK_NULL_HANDLE;
        }
        if (previewImageView != VK_NULL_HANDLE) {
            vkDestroyImageView(device, previewImageView, nullptr);
            previewImageView = VK_NULL_HANDLE;
        }
        if (previewImage != VK_NULL_HANDLE) {
            vkDestroyImage(device, previewImage, nullptr);
            previewImage = VK_NULL_HANDLE;
        }
        if (previewImageMemory != VK_NULL_HANDLE) {
            vkFreeMemory(device, previewImageMemory, nullptr);
            previewImageMemory = VK_NULL_HANDLE;
        }
        previewImageSize = {};
        previewImageLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        boundPreviewView = VK_NULL_HANDLE;
        boundPreviewLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    }

    bool uploadPreviewImage(const jcut::core::ImageBuffer& image,
                            ImTextureID* textureOut,
                            std::string* error)
    {
        if (image.empty() || image.format != jcut::core::PixelFormat::Rgba8) {
            if (error) *error = "Invalid CPU preview image.";
            return false;
        }
        const VkDeviceSize byteCount =
            static_cast<VkDeviceSize>(image.size.width) *
            static_cast<VkDeviceSize>(image.size.height) * 4;
        std::vector<std::uint8_t> packed;
        const std::uint8_t* uploadBytes = image.bytes.data();
        if (image.strideBytes != image.size.width * 4) {
            packed.resize(static_cast<std::size_t>(byteCount));
            for (int y = 0; y < image.size.height; ++y) {
                std::memcpy(packed.data() + static_cast<std::size_t>(y * image.size.width * 4),
                            image.bytes.data() + static_cast<std::size_t>(y * image.strideBytes),
                            static_cast<std::size_t>(image.size.width * 4));
            }
            uploadBytes = packed.data();
        }

        VkBuffer stagingBuffer = VK_NULL_HANDLE;
        VkDeviceMemory stagingMemory = VK_NULL_HANDLE;
        if (!createBuffer(byteCount,
                          VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
                          VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                          &stagingBuffer,
                          &stagingMemory,
                          error)) {
            return false;
        }

        void* mapped = nullptr;
        if (vkMapMemory(device, stagingMemory, 0, byteCount, 0, &mapped) != VK_SUCCESS || !mapped) {
            if (error) *error = "Failed to map Vulkan preview upload memory.";
            vkDestroyBuffer(device, stagingBuffer, nullptr);
            vkFreeMemory(device, stagingMemory, nullptr);
            return false;
        }
        std::memcpy(mapped, uploadBytes, static_cast<std::size_t>(byteCount));
        vkUnmapMemory(device, stagingMemory);

        if (previewImage == VK_NULL_HANDLE ||
            previewImageSize.width != image.size.width ||
            previewImageSize.height != image.size.height) {
            vkDeviceWaitIdle(device);
            releasePreviewTextureResources();
            if (!createPreviewImage(image.size, error)) {
                vkDestroyBuffer(device, stagingBuffer, nullptr);
                vkFreeMemory(device, stagingMemory, nullptr);
                return false;
            }
        }

        VkCommandBuffer commandBuffer = beginUploadCommands(error);
        if (commandBuffer == VK_NULL_HANDLE) {
            vkDestroyBuffer(device, stagingBuffer, nullptr);
            vkFreeMemory(device, stagingMemory, nullptr);
            return false;
        }
        transitionPreviewImage(commandBuffer, previewImageLayout, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);

        VkBufferImageCopy copyRegion{};
        copyRegion.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        copyRegion.imageSubresource.mipLevel = 0;
        copyRegion.imageSubresource.baseArrayLayer = 0;
        copyRegion.imageSubresource.layerCount = 1;
        copyRegion.imageExtent = {
            static_cast<uint32_t>(image.size.width),
            static_cast<uint32_t>(image.size.height),
            1
        };
        vkCmdCopyBufferToImage(commandBuffer,
                               stagingBuffer,
                               previewImage,
                               VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                               1,
                               &copyRegion);
        transitionPreviewImage(commandBuffer,
                               VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                               VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
        const bool submitted = endUploadCommands(commandBuffer, error);
        vkDestroyBuffer(device, stagingBuffer, nullptr);
        vkFreeMemory(device, stagingMemory, nullptr);
        if (!submitted) {
            return false;
        }
        previewImageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

        if (previewTextureSet == VK_NULL_HANDLE ||
            boundPreviewView != previewImageView ||
            boundPreviewLayout != previewImageLayout) {
            if (previewTextureSet != VK_NULL_HANDLE) {
                ImGui_ImplVulkan_RemoveTexture(previewTextureSet);
            }
            previewTextureSet =
                ImGui_ImplVulkan_AddTexture(previewSampler, previewImageView, previewImageLayout);
            boundPreviewView = previewImageView;
            boundPreviewLayout = previewImageLayout;
        }
        if (textureOut) {
            *textureOut = reinterpret_cast<ImTextureID>(previewTextureSet);
        }
        return previewTextureSet != VK_NULL_HANDLE;
    }

    void releaseAuxiliaryTexture(AuxiliaryTexture* texture)
    {
        if (!texture || device == VK_NULL_HANDLE) return;
        if (texture->descriptor != VK_NULL_HANDLE) {
            ImGui_ImplVulkan_RemoveTexture(texture->descriptor);
            texture->descriptor = VK_NULL_HANDLE;
        }
        if (texture->view != VK_NULL_HANDLE) {
            vkDestroyImageView(device, texture->view, nullptr);
            texture->view = VK_NULL_HANDLE;
        }
        if (texture->image != VK_NULL_HANDLE) {
            vkDestroyImage(device, texture->image, nullptr);
            texture->image = VK_NULL_HANDLE;
        }
        if (texture->memory != VK_NULL_HANDLE) {
            vkFreeMemory(device, texture->memory, nullptr);
            texture->memory = VK_NULL_HANDLE;
        }
        texture->size = {};
        texture->layout = VK_IMAGE_LAYOUT_UNDEFINED;
    }

    bool createAuxiliaryTexture(
        AuxiliaryTexture* texture,
        const jcut::core::SizeI& size,
        std::string* error)
    {
        if (!texture || !size.valid()) {
            if (error) *error = "Invalid auxiliary texture size.";
            return false;
        }
        VkImageCreateInfo imageInfo{};
        imageInfo.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
        imageInfo.imageType = VK_IMAGE_TYPE_2D;
        imageInfo.extent = {
            static_cast<uint32_t>(size.width),
            static_cast<uint32_t>(size.height),
            1};
        imageInfo.mipLevels = 1;
        imageInfo.arrayLayers = 1;
        imageInfo.format = VK_FORMAT_R8G8B8A8_UNORM;
        imageInfo.tiling = VK_IMAGE_TILING_OPTIMAL;
        imageInfo.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        imageInfo.usage =
            VK_IMAGE_USAGE_TRANSFER_DST_BIT |
            VK_IMAGE_USAGE_SAMPLED_BIT;
        imageInfo.samples = VK_SAMPLE_COUNT_1_BIT;
        imageInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
        if (vkCreateImage(
                device, &imageInfo, nullptr,
                &texture->image) != VK_SUCCESS) {
            if (error) *error =
                "Failed to create Vulkan auxiliary image.";
            return false;
        }
        VkMemoryRequirements requirements{};
        vkGetImageMemoryRequirements(
            device, texture->image, &requirements);
        const uint32_t memoryType = findMemoryType(
            requirements.memoryTypeBits,
            VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
        if (memoryType == UINT32_MAX) {
            if (error) *error =
                "No suitable memory type for Vulkan auxiliary image.";
            releaseAuxiliaryTexture(texture);
            return false;
        }
        VkMemoryAllocateInfo allocInfo{};
        allocInfo.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
        allocInfo.allocationSize = requirements.size;
        allocInfo.memoryTypeIndex = memoryType;
        if (vkAllocateMemory(
                device, &allocInfo, nullptr,
                &texture->memory) != VK_SUCCESS) {
            if (error) *error =
                "Failed to allocate Vulkan auxiliary image memory.";
            releaseAuxiliaryTexture(texture);
            return false;
        }
        if (vkBindImageMemory(
                device, texture->image, texture->memory, 0) !=
            VK_SUCCESS) {
            if (error) *error =
                "Failed to bind Vulkan auxiliary image memory.";
            releaseAuxiliaryTexture(texture);
            return false;
        }
        VkImageViewCreateInfo viewInfo{};
        viewInfo.sType =
            VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
        viewInfo.image = texture->image;
        viewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
        viewInfo.format = VK_FORMAT_R8G8B8A8_UNORM;
        viewInfo.subresourceRange.aspectMask =
            VK_IMAGE_ASPECT_COLOR_BIT;
        viewInfo.subresourceRange.levelCount = 1;
        viewInfo.subresourceRange.layerCount = 1;
        if (vkCreateImageView(
                device, &viewInfo, nullptr,
                &texture->view) != VK_SUCCESS) {
            if (error) *error =
                "Failed to create Vulkan auxiliary image view.";
            releaseAuxiliaryTexture(texture);
            return false;
        }
        texture->size = size;
        texture->layout = VK_IMAGE_LAYOUT_UNDEFINED;
        return true;
    }

    bool uploadAuxiliaryImage(
        const jcut::core::ImageBuffer& image,
        AuxiliaryTexture* texture,
        ImTextureID* textureOut,
        std::string* error)
    {
        if (!texture || image.empty() ||
            image.format != jcut::core::PixelFormat::Rgba8) {
            if (error) *error = "Invalid CPU auxiliary image.";
            return false;
        }
        const VkDeviceSize byteCount =
            static_cast<VkDeviceSize>(image.size.width) *
            static_cast<VkDeviceSize>(image.size.height) * 4;
        std::vector<std::uint8_t> packed;
        const std::uint8_t* uploadBytes = image.bytes.data();
        if (image.strideBytes != image.size.width * 4) {
            packed.resize(static_cast<std::size_t>(byteCount));
            for (int y = 0; y < image.size.height; ++y) {
                std::memcpy(
                    packed.data() +
                        static_cast<std::size_t>(
                            y * image.size.width * 4),
                    image.bytes.data() +
                        static_cast<std::size_t>(
                            y * image.strideBytes),
                    static_cast<std::size_t>(
                        image.size.width * 4));
            }
            uploadBytes = packed.data();
        }
        VkBuffer stagingBuffer = VK_NULL_HANDLE;
        VkDeviceMemory stagingMemory = VK_NULL_HANDLE;
        if (!createBuffer(
                byteCount,
                VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
                VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                    VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                &stagingBuffer,
                &stagingMemory,
                error)) {
            return false;
        }
        const auto releaseStaging = [&]() {
            vkDestroyBuffer(device, stagingBuffer, nullptr);
            vkFreeMemory(device, stagingMemory, nullptr);
        };
        void* mapped = nullptr;
        if (vkMapMemory(
                device, stagingMemory, 0, byteCount, 0,
                &mapped) != VK_SUCCESS ||
            !mapped) {
            if (error) *error =
                "Failed to map Vulkan auxiliary upload memory.";
            releaseStaging();
            return false;
        }
        std::memcpy(
            mapped, uploadBytes,
            static_cast<std::size_t>(byteCount));
        vkUnmapMemory(device, stagingMemory);
        if (texture->image == VK_NULL_HANDLE ||
            texture->size.width != image.size.width ||
            texture->size.height != image.size.height) {
            vkDeviceWaitIdle(device);
            releaseAuxiliaryTexture(texture);
            if (!createAuxiliaryTexture(
                    texture, image.size, error)) {
                releaseStaging();
                return false;
            }
        }
        VkCommandBuffer commands = beginUploadCommands(error);
        if (commands == VK_NULL_HANDLE) {
            releaseStaging();
            return false;
        }
        transitionImage(
            commands,
            texture->image,
            texture->layout,
            VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
        VkBufferImageCopy copyRegion{};
        copyRegion.imageSubresource.aspectMask =
            VK_IMAGE_ASPECT_COLOR_BIT;
        copyRegion.imageSubresource.layerCount = 1;
        copyRegion.imageExtent = {
            static_cast<uint32_t>(image.size.width),
            static_cast<uint32_t>(image.size.height),
            1};
        vkCmdCopyBufferToImage(
            commands,
            stagingBuffer,
            texture->image,
            VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
            1,
            &copyRegion);
        transitionImage(
            commands,
            texture->image,
            VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
            VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
        const bool submitted =
            endUploadCommands(commands, error);
        releaseStaging();
        if (!submitted) return false;
        texture->layout =
            VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        if (texture->descriptor == VK_NULL_HANDLE) {
            texture->descriptor = ImGui_ImplVulkan_AddTexture(
                previewSampler,
                texture->view,
                texture->layout);
        }
        if (textureOut) {
            *textureOut = reinterpret_cast<ImTextureID>(
                texture->descriptor);
        }
        return texture->descriptor != VK_NULL_HANDLE;
    }

    bool setupSwapchain(int width, int height, std::string* error)
    {
        windowData.Surface = surface;
        const VkFormat formats[] = {
            VK_FORMAT_B8G8R8A8_UNORM,
            VK_FORMAT_R8G8B8A8_UNORM,
            VK_FORMAT_B8G8R8_UNORM,
            VK_FORMAT_R8G8B8_UNORM,
        };
        windowData.SurfaceFormat =
            ImGui_ImplVulkanH_SelectSurfaceFormat(physicalDevice,
                                                  surface,
                                                  formats,
                                                  IM_ARRAYSIZE(formats),
                                                  VK_COLORSPACE_SRGB_NONLINEAR_KHR);
        const VkPresentModeKHR presentModes[] = {VK_PRESENT_MODE_FIFO_KHR};
        windowData.PresentMode =
            ImGui_ImplVulkanH_SelectPresentMode(physicalDevice,
                                                surface,
                                                presentModes,
                                                IM_ARRAYSIZE(presentModes));
        ImGui_ImplVulkanH_CreateOrResizeWindow(instance,
                                               physicalDevice,
                                               device,
                                               &windowData,
                                               queueFamily,
                                               nullptr,
                                               width,
                                               height,
                                               minImageCount,
                                               0);
        windowData.ClearValue = makeVulkanClearValue();
        if (windowData.RenderPass == VK_NULL_HANDLE) {
            if (error) *error = "Failed to create Vulkan swapchain render pass.";
            return false;
        }
        return true;
    }

    bool initialize(X11Platform* x11Platform, std::string* error)
    {
        platform = x11Platform;
        if (!createInstance(error)) return false;
        VkXlibSurfaceCreateInfoKHR surfaceInfo{};
        surfaceInfo.sType = VK_STRUCTURE_TYPE_XLIB_SURFACE_CREATE_INFO_KHR;
        surfaceInfo.dpy = platform ? platform->display : nullptr;
        surfaceInfo.window = platform ? platform->window : 0;
        if (vkCreateXlibSurfaceKHR(instance, &surfaceInfo, nullptr, &surface) != VK_SUCCESS) {
            if (error) *error = "Failed to create Vulkan Xlib window surface.";
            return false;
        }
        if (!selectPhysicalDevice(error) ||
            !createDevice(error) ||
            !createDescriptorPool(error) ||
            !createPreviewSampler(error) ||
            !createUploadCommandPool(error)) {
            return false;
        }
        if (!platform) {
            if (error) *error = "X11 platform is unavailable.";
            return false;
        }
        if (!setupSwapchain(std::max(1, platform->width), std::max(1, platform->height), error)) {
            return false;
        }

        ImGui_ImplVulkan_InitInfo initInfo{};
        initInfo.Instance = instance;
        initInfo.PhysicalDevice = physicalDevice;
        initInfo.Device = device;
        initInfo.QueueFamily = queueFamily;
        initInfo.Queue = queue;
        initInfo.DescriptorPool = descriptorPool;
        initInfo.MinImageCount = minImageCount;
        initInfo.ImageCount = windowData.ImageCount;
        initInfo.PipelineInfoMain.RenderPass = windowData.RenderPass;
        initInfo.PipelineInfoMain.Subpass = 0;
        initInfo.PipelineInfoMain.MSAASamples = VK_SAMPLE_COUNT_1_BIT;
        initInfo.CheckVkResultFn = checkVulkanResult;
        if (!ImGui_ImplVulkan_Init(&initInfo)) {
            if (error) *error = "Failed to initialize ImGui Vulkan backend.";
            return false;
        }
        std::string handoffError;
        previewHandoffInitialized = previewHandoff.initialize(
            physicalDevice, device, queue, queueFamily, &handoffError);
        previewHandoffStatus = previewHandoffInitialized
            ? std::string("ready")
            : (handoffError.empty()
                ? std::string("Vulkan external frame import unavailable")
                : handoffError);
        handoffError.clear();
        hardwareFrameHandoffInitialized =
            hardwareFrameHandoff.initialize(
                physicalDevice,
                device,
                queue,
                queueFamily,
                JCUT_VULKAN_SHADER_DIR,
                &handoffError);
        hardwareFrameHandoffStatus =
            hardwareFrameHandoffInitialized
            ? std::string("ready")
            : (handoffError.empty()
                ? std::string("decoded hardware-frame import unavailable")
                : handoffError);
        return true;
    }

    bool bindPreviewFrame(const render_detail::OffscreenVulkanFrame& frame,
                          ImTextureID* textureOut,
                          std::string* error)
    {
        if (!frame.valid) {
            if (error) *error = "invalid offscreen Vulkan frame";
            return false;
        }
        if (!previewHandoffInitialized) {
            if (error) *error = previewHandoffStatus.empty()
                ? "Vulkan external frame import unavailable"
                : previewHandoffStatus;
            return false;
        }
        if (!previewHandoff.importFrame(frame, error)) {
            return false;
        }
        const jcut::imgui::VulkanExternalImage external =
            previewHandoff.externalImage();
        if (external.imageView == VK_NULL_HANDLE || !external.size.valid()) {
            if (error) *error = "imported Vulkan preview image is unavailable";
            return false;
        }
        if (previewTextureSet == VK_NULL_HANDLE ||
            boundPreviewView != external.imageView ||
            boundPreviewLayout != external.imageLayout) {
            if (previewTextureSet != VK_NULL_HANDLE) {
                ImGui_ImplVulkan_RemoveTexture(previewTextureSet);
                previewTextureSet = VK_NULL_HANDLE;
            }
            previewTextureSet =
                ImGui_ImplVulkan_AddTexture(previewSampler, external.imageView, external.imageLayout);
            boundPreviewView = external.imageView;
            boundPreviewLayout = external.imageLayout;
        }
        if (textureOut) {
            *textureOut = reinterpret_cast<ImTextureID>(previewTextureSet);
        }
        return previewTextureSet != VK_NULL_HANDLE;
    }

    bool bindHardwarePreviewFrame(
        const jcut::core::FramePayloadCore& frame,
        const jcut::EditorGradingKeyframe& grade,
        ImTextureID* textureOut,
        std::string* error)
    {
        if (!hardwareFrameHandoffInitialized) {
            if (error) {
                *error = hardwareFrameHandoffStatus.empty()
                    ? "decoded hardware-frame import unavailable"
                    : hardwareFrameHandoffStatus;
            }
            return false;
        }
        jcut::vulkan_import::HardwareFrameColorGradeCore hardwareGrade;
        hardwareGrade.brightness = static_cast<float>(grade.brightness);
        hardwareGrade.contrast = static_cast<float>(grade.contrast);
        hardwareGrade.saturation = static_cast<float>(grade.saturation);
        hardwareGrade.shadowsR = static_cast<float>(grade.shadowsR);
        hardwareGrade.shadowsG = static_cast<float>(grade.shadowsG);
        hardwareGrade.shadowsB = static_cast<float>(grade.shadowsB);
        hardwareGrade.midtonesR = static_cast<float>(grade.midtonesR);
        hardwareGrade.midtonesG = static_cast<float>(grade.midtonesG);
        hardwareGrade.midtonesB = static_cast<float>(grade.midtonesB);
        hardwareGrade.highlightsR =
            static_cast<float>(grade.highlightsR);
        hardwareGrade.highlightsG =
            static_cast<float>(grade.highlightsG);
        hardwareGrade.highlightsB =
            static_cast<float>(grade.highlightsB);
        hardwareGrade.curvesEnabled =
            !jcut::editorGradingCurveIsIdentity(grade.curvePointsR) ||
            !jcut::editorGradingCurveIsIdentity(grade.curvePointsG) ||
            !jcut::editorGradingCurveIsIdentity(grade.curvePointsB) ||
            !jcut::editorGradingCurveIsIdentity(grade.curvePointsLuma);
        if (hardwareGrade.curvesEnabled) {
            hardwareGrade.curveLut =
                jcut::editorPackedGradingCurveLut(grade);
        }
        if (!hardwareFrameHandoff.importFrame(
                frame, hardwareGrade, error)) {
            return false;
        }
        const jcut::vulkan_import::ExternalImage external =
            hardwareFrameHandoff.externalImage();
        if (external.imageView == VK_NULL_HANDLE ||
            !external.size.valid()) {
            if (error) {
                *error =
                    "decoded hardware-frame Vulkan image is unavailable";
            }
            return false;
        }
        if (previewTextureSet == VK_NULL_HANDLE ||
            boundPreviewView != external.imageView ||
            boundPreviewLayout != external.imageLayout) {
            if (previewTextureSet != VK_NULL_HANDLE) {
                ImGui_ImplVulkan_RemoveTexture(previewTextureSet);
                previewTextureSet = VK_NULL_HANDLE;
            }
            previewTextureSet = ImGui_ImplVulkan_AddTexture(
                previewSampler,
                external.imageView,
                external.imageLayout);
            boundPreviewView = external.imageView;
            boundPreviewLayout = external.imageLayout;
        }
        if (textureOut) {
            *textureOut =
                reinterpret_cast<ImTextureID>(previewTextureSet);
        }
        return previewTextureSet != VK_NULL_HANDLE;
    }

    void rebuildSwapchainIfNeeded()
    {
        if (!platform || platform->width <= 0 || platform->height <= 0) {
            return;
        }
        if (swapchainRebuild ||
            windowData.Width != static_cast<uint32_t>(platform->width) ||
            windowData.Height != static_cast<uint32_t>(platform->height)) {
            vkDeviceWaitIdle(device);
            ImGui_ImplVulkan_SetMinImageCount(minImageCount);
            ImGui_ImplVulkanH_CreateOrResizeWindow(instance,
                                                   physicalDevice,
                                                   device,
                                                   &windowData,
                                                   queueFamily,
                                                   nullptr,
                                                   platform->width,
                                                   platform->height,
                                                   minImageCount,
                                                   0);
            windowData.ClearValue = makeVulkanClearValue();
            windowData.FrameIndex = 0;
            swapchainRebuild = false;
        }
    }

    void renderDrawData(ImDrawData* drawData)
    {
        ImGui_ImplVulkanH_Window* wd = &windowData;
        VkSemaphore imageAcquiredSemaphore = wd->FrameSemaphores[wd->SemaphoreIndex].ImageAcquiredSemaphore;
        VkSemaphore renderCompleteSemaphore = wd->FrameSemaphores[wd->SemaphoreIndex].RenderCompleteSemaphore;
        VkResult err = vkAcquireNextImageKHR(device,
                                             wd->Swapchain,
                                             UINT64_MAX,
                                             imageAcquiredSemaphore,
                                             VK_NULL_HANDLE,
                                             &wd->FrameIndex);
        if (err == VK_ERROR_OUT_OF_DATE_KHR || err == VK_SUBOPTIMAL_KHR) {
            swapchainRebuild = true;
        }
        if (err == VK_ERROR_OUT_OF_DATE_KHR) {
            return;
        }
        checkVulkanResult(err);

        ImGui_ImplVulkanH_Frame* fd = &wd->Frames[wd->FrameIndex];
        vkWaitForFences(device, 1, &fd->Fence, VK_TRUE, UINT64_MAX);
        vkResetFences(device, 1, &fd->Fence);
        vkResetCommandPool(device, fd->CommandPool, 0);

        VkCommandBufferBeginInfo beginInfo{};
        beginInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
        beginInfo.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        vkBeginCommandBuffer(fd->CommandBuffer, &beginInfo);

        VkRenderPassBeginInfo renderPassInfo{};
        renderPassInfo.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
        renderPassInfo.renderPass = wd->RenderPass;
        renderPassInfo.framebuffer = fd->Framebuffer;
        renderPassInfo.renderArea.extent.width = wd->Width;
        renderPassInfo.renderArea.extent.height = wd->Height;
        renderPassInfo.clearValueCount = 1;
        renderPassInfo.pClearValues = &wd->ClearValue;
        vkCmdBeginRenderPass(fd->CommandBuffer, &renderPassInfo, VK_SUBPASS_CONTENTS_INLINE);
        ImGui_ImplVulkan_RenderDrawData(drawData, fd->CommandBuffer);
        vkCmdEndRenderPass(fd->CommandBuffer);
        vkEndCommandBuffer(fd->CommandBuffer);

        const VkPipelineStageFlags waitStage = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
        VkSubmitInfo submitInfo{};
        submitInfo.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
        submitInfo.waitSemaphoreCount = 1;
        submitInfo.pWaitSemaphores = &imageAcquiredSemaphore;
        submitInfo.pWaitDstStageMask = &waitStage;
        submitInfo.commandBufferCount = 1;
        submitInfo.pCommandBuffers = &fd->CommandBuffer;
        submitInfo.signalSemaphoreCount = 1;
        submitInfo.pSignalSemaphores = &renderCompleteSemaphore;
        checkVulkanResult(vkQueueSubmit(queue, 1, &submitInfo, fd->Fence));
    }

    void present()
    {
        if (swapchainRebuild) {
            return;
        }
        ImGui_ImplVulkanH_Window* wd = &windowData;
        VkSemaphore renderCompleteSemaphore = wd->FrameSemaphores[wd->SemaphoreIndex].RenderCompleteSemaphore;
        VkPresentInfoKHR presentInfo{};
        presentInfo.sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR;
        presentInfo.waitSemaphoreCount = 1;
        presentInfo.pWaitSemaphores = &renderCompleteSemaphore;
        presentInfo.swapchainCount = 1;
        presentInfo.pSwapchains = &wd->Swapchain;
        presentInfo.pImageIndices = &wd->FrameIndex;
        VkResult err = vkQueuePresentKHR(queue, &presentInfo);
        if (err == VK_ERROR_OUT_OF_DATE_KHR || err == VK_SUBOPTIMAL_KHR) {
            swapchainRebuild = true;
        } else {
            checkVulkanResult(err);
        }
        wd->SemaphoreIndex = (wd->SemaphoreIndex + 1) % wd->SemaphoreCount;
    }

    void shutdown()
    {
        if (device != VK_NULL_HANDLE) {
            vkDeviceWaitIdle(device);
        }
        releaseAuxiliaryTexture(
            &birefnetLivePreviewTexture);
        releaseAuxiliaryTexture(
            &mediaThumbnailTexture);
        releaseAuxiliaryTexture(
            &aiProfileAvatarTexture);
        releaseAuxiliaryTexture(
            &faceReferenceTexture);
        releaseAuxiliaryTexture(
            &sectionAvatarTexture);
        releaseAuxiliaryTexture(
            &previewOverlayTexture);
        releasePreviewTextureResources();
        hardwareFrameHandoff.release();
        hardwareFrameHandoffInitialized = false;
        previewHandoff.release();
        previewHandoffInitialized = false;
        ImGui_ImplVulkan_Shutdown();
        if (device != VK_NULL_HANDLE && windowData.Surface != VK_NULL_HANDLE) {
            ImGui_ImplVulkanH_DestroyWindow(instance, device, &windowData, nullptr);
        }
        if (previewSampler != VK_NULL_HANDLE) {
            vkDestroySampler(device, previewSampler, nullptr);
            previewSampler = VK_NULL_HANDLE;
        }
        if (uploadCommandPool != VK_NULL_HANDLE) {
            vkDestroyCommandPool(device, uploadCommandPool, nullptr);
            uploadCommandPool = VK_NULL_HANDLE;
        }
        if (descriptorPool != VK_NULL_HANDLE) {
            vkDestroyDescriptorPool(device, descriptorPool, nullptr);
            descriptorPool = VK_NULL_HANDLE;
        }
        if (device != VK_NULL_HANDLE) {
            vkDestroyDevice(device, nullptr);
            device = VK_NULL_HANDLE;
        }
        if (surface != VK_NULL_HANDLE) {
            vkDestroySurfaceKHR(instance, surface, nullptr);
            surface = VK_NULL_HANDLE;
        }
        if (instance != VK_NULL_HANDLE) {
            vkDestroyInstance(instance, nullptr);
            instance = VK_NULL_HANDLE;
        }
    }
};
