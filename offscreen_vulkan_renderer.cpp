#include "render_internal.h"

#include <QDebug>
#include <QDateTime>
#include <QFile>
#include <QImage>
#include <QScopeGuard>

#if JCUT_HAS_CUDA_DRIVER
extern "C" {
#include <libavutil/hwcontext.h>
#include <libavutil/hwcontext_cuda.h>
}
#include <cuda.h>
#endif
#include <vulkan/vulkan.h>
#include <cstring>
#include <unistd.h>

namespace render_detail {

namespace {

uint32_t findMemoryType(VkPhysicalDevice physicalDevice,
                        uint32_t typeFilter,
                        VkMemoryPropertyFlags properties)
{
    VkPhysicalDeviceMemoryProperties memProperties{};
    vkGetPhysicalDeviceMemoryProperties(physicalDevice, &memProperties);
    for (uint32_t i = 0; i < memProperties.memoryTypeCount; ++i) {
        if ((typeFilter & (1u << i)) &&
            (memProperties.memoryTypes[i].propertyFlags & properties) == properties) {
            return i;
        }
    }
    return UINT32_MAX;
}

uint32_t findMemoryTypePreferred(VkPhysicalDevice physicalDevice,
                                 uint32_t typeFilter,
                                 VkMemoryPropertyFlags required,
                                 VkMemoryPropertyFlags preferred,
                                 VkMemoryPropertyFlags* selectedFlags = nullptr)
{
    VkPhysicalDeviceMemoryProperties memProperties{};
    vkGetPhysicalDeviceMemoryProperties(physicalDevice, &memProperties);
    uint32_t fallback = UINT32_MAX;
    VkMemoryPropertyFlags fallbackFlags = 0;
    for (uint32_t i = 0; i < memProperties.memoryTypeCount; ++i) {
        if (!(typeFilter & (1u << i))) {
            continue;
        }
        const VkMemoryPropertyFlags flags = memProperties.memoryTypes[i].propertyFlags;
        if ((flags & required) != required) {
            continue;
        }
        if ((flags & preferred) == preferred) {
            if (selectedFlags) {
                *selectedFlags = flags;
            }
            return i;
        }
        if (fallback == UINT32_MAX) {
            fallback = i;
            fallbackFlags = flags;
        }
    }
    if (selectedFlags && fallback != UINT32_MAX) {
        *selectedFlags = fallbackFlags;
    }
    return fallback;
}

bool physicalDeviceSupportsExtension(VkPhysicalDevice device, const char* extensionName)
{
    uint32_t extensionCount = 0;
    if (vkEnumerateDeviceExtensionProperties(device, nullptr, &extensionCount, nullptr) != VK_SUCCESS) {
        return false;
    }
    QVector<VkExtensionProperties> extensions(static_cast<int>(extensionCount));
    if (vkEnumerateDeviceExtensionProperties(device, nullptr, &extensionCount, extensions.data()) != VK_SUCCESS) {
        return false;
    }
    for (const VkExtensionProperties& extension : extensions) {
        if (qstrcmp(extension.extensionName, extensionName) == 0) {
            return true;
        }
    }
    return false;
}

QByteArray readBinaryFile(const QString& path)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        return {};
    }
    return file.readAll();
}

VkShaderModule createShaderModule(VkDevice device, const QByteArray& bytes)
{
    if (device == VK_NULL_HANDLE || bytes.isEmpty() || (bytes.size() % 4) != 0) {
        return VK_NULL_HANDLE;
    }
    VkShaderModuleCreateInfo info{};
    info.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
    info.codeSize = static_cast<size_t>(bytes.size());
    info.pCode = reinterpret_cast<const uint32_t*>(bytes.constData());
    VkShaderModule module = VK_NULL_HANDLE;
    if (vkCreateShaderModule(device, &info, nullptr, &module) != VK_SUCCESS) {
        return VK_NULL_HANDLE;
    }
    return module;
}

void transitionImageLayout(VkCommandBuffer cmd,
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

    if (oldLayout == VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL &&
        newLayout == VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL) {
        barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
        srcStage = VK_PIPELINE_STAGE_TRANSFER_BIT;
        dstStage = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
    } else if (oldLayout == VK_IMAGE_LAYOUT_UNDEFINED &&
               newLayout == VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL) {
        barrier.srcAccessMask = 0;
        barrier.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        srcStage = VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT;
        dstStage = VK_PIPELINE_STAGE_TRANSFER_BIT;
    } else if (oldLayout == VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL &&
               newLayout == VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL) {
        barrier.srcAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
        barrier.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        srcStage = VK_PIPELINE_STAGE_TRANSFER_BIT;
        dstStage = VK_PIPELINE_STAGE_TRANSFER_BIT;
    } else if (oldLayout == VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL &&
               newLayout == VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL) {
        barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        barrier.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
        srcStage = VK_PIPELINE_STAGE_TRANSFER_BIT;
        dstStage = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    } else if (oldLayout == VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL &&
               newLayout == VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL) {
        barrier.srcAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
        barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
        srcStage = VK_PIPELINE_STAGE_TRANSFER_BIT;
        dstStage = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
    } else if (oldLayout == VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL &&
               newLayout == VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL) {
        barrier.srcAccessMask = VK_ACCESS_SHADER_READ_BIT;
        barrier.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
        srcStage = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
        dstStage = VK_PIPELINE_STAGE_TRANSFER_BIT;
    } else if (oldLayout == VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL &&
               newLayout == VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL) {
        barrier.srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
        barrier.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
        srcStage = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
        dstStage = VK_PIPELINE_STAGE_TRANSFER_BIT;
    } else if (oldLayout == VK_IMAGE_LAYOUT_UNDEFINED &&
               newLayout == VK_IMAGE_LAYOUT_GENERAL) {
        barrier.srcAccessMask = 0;
        barrier.dstAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
        srcStage = VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT;
        dstStage = VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT;
    } else if (oldLayout == VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL &&
               newLayout == VK_IMAGE_LAYOUT_GENERAL) {
        barrier.srcAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
        barrier.dstAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
        srcStage = VK_PIPELINE_STAGE_TRANSFER_BIT;
        dstStage = VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT;
    } else if (oldLayout == VK_IMAGE_LAYOUT_GENERAL &&
               newLayout == VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL) {
        barrier.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
        barrier.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
        srcStage = VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT;
        dstStage = VK_PIPELINE_STAGE_TRANSFER_BIT;
    }

    vkCmdPipelineBarrier(cmd,
                         srcStage,
                         dstStage,
                         0,
                         0, nullptr,
                         0, nullptr,
                         1, &barrier);
}

} // namespace

class OffscreenVulkanRendererPrivate {
public:
    static constexpr int kMaxLayerTextures = 1;
    static constexpr int kFrameSlots = 3;
    struct FrameSlot {
        VkCommandBuffer commandBuffer = VK_NULL_HANDLE;
        VkBuffer stagingBuffer = VK_NULL_HANDLE;
        VkDeviceMemory stagingMemory = VK_NULL_HANDLE;
        VkDeviceSize stagingAllocationSize = 0;
        void* stagingMapped = nullptr;
        VkBuffer cudaExportBuffer = VK_NULL_HANDLE;
        VkDeviceMemory cudaExportMemory = VK_NULL_HANDLE;
        VkDeviceSize cudaExportAllocationSize = 0;
#if JCUT_HAS_CUDA_DRIVER
        CUexternalMemory cudaExternalMemory = nullptr;
        CUdeviceptr cudaExternalDevicePtr = 0;
        CUcontext cudaImportContext = nullptr;
#endif
        VkFence fence = VK_NULL_HANDLE;
        bool stagingHostCoherent = false;
        bool inFlight = false;
    };
    ~OffscreenVulkanRendererPrivate() { release(); }

    bool initialize(const QSize& outputSize, QString* errorMessage)
    {
        release();

        m_outputSize = QSize(qMax(16, outputSize.width()), qMax(16, outputSize.height()));

        VkApplicationInfo appInfo{};
        appInfo.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
        appInfo.pApplicationName = "JCut Offscreen Vulkan Renderer";
        appInfo.applicationVersion = VK_MAKE_VERSION(1, 0, 0);
        appInfo.pEngineName = "JCut";
        appInfo.engineVersion = VK_MAKE_VERSION(1, 0, 0);
        appInfo.apiVersion = VK_API_VERSION_1_1;

        VkInstanceCreateInfo instanceInfo{};
        instanceInfo.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
        instanceInfo.pApplicationInfo = &appInfo;

        if (vkCreateInstance(&instanceInfo, nullptr, &m_instance) != VK_SUCCESS) {
            if (errorMessage) {
                *errorMessage = QStringLiteral("Failed to create Vulkan instance.");
            }
            return false;
        }

        uint32_t physicalDeviceCount = 0;
        vkEnumeratePhysicalDevices(m_instance, &physicalDeviceCount, nullptr);
        if (physicalDeviceCount == 0) {
            if (errorMessage) {
                *errorMessage = QStringLiteral("No Vulkan physical devices found.");
            }
            return false;
        }

        QVector<VkPhysicalDevice> devices(static_cast<int>(physicalDeviceCount));
        vkEnumeratePhysicalDevices(m_instance, &physicalDeviceCount, devices.data());

        int bestScore = -1;
        VkPhysicalDevice bestDevice = VK_NULL_HANDLE;
        uint32_t bestQueueFamily = UINT32_MAX;
        VkPhysicalDeviceProperties bestProperties{};
        for (VkPhysicalDevice candidate : devices) {
            VkPhysicalDeviceProperties properties{};
            vkGetPhysicalDeviceProperties(candidate, &properties);
            uint32_t queueFamilyCount = 0;
            vkGetPhysicalDeviceQueueFamilyProperties(candidate, &queueFamilyCount, nullptr);
            QVector<VkQueueFamilyProperties> queueFamilies(static_cast<int>(queueFamilyCount));
            vkGetPhysicalDeviceQueueFamilyProperties(candidate, &queueFamilyCount, queueFamilies.data());
            for (uint32_t i = 0; i < queueFamilyCount; ++i) {
                if (queueFamilies[static_cast<int>(i)].queueFlags & VK_QUEUE_GRAPHICS_BIT) {
                    int score = 0;
                    if (properties.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU) {
                        score += 1000;
                    } else if (properties.deviceType == VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU) {
                        score += 500;
                    } else if (properties.deviceType == VK_PHYSICAL_DEVICE_TYPE_CPU) {
                        score -= 1000;
                    }
                    if (properties.vendorID == 0x10de) {
                        score += 100;
                    }
                    if (score > bestScore) {
                        bestScore = score;
                        bestDevice = candidate;
                        bestQueueFamily = i;
                        bestProperties = properties;
                    }
                }
            }
        }
        m_physicalDevice = bestDevice;
        m_graphicsQueueFamily = bestQueueFamily;

        if (m_physicalDevice == VK_NULL_HANDLE) {
            if (errorMessage) {
                *errorMessage = QStringLiteral("No Vulkan graphics queue family found.");
            }
            return false;
        }
        qInfo().noquote() << QStringLiteral("Offscreen Vulkan device: %1 vendor=0x%2 type=%3")
                                 .arg(QString::fromUtf8(bestProperties.deviceName))
                                 .arg(bestProperties.vendorID, 0, 16)
                                 .arg(static_cast<int>(bestProperties.deviceType));
        m_externalMemoryFdSupported =
            physicalDeviceSupportsExtension(m_physicalDevice, VK_KHR_EXTERNAL_MEMORY_EXTENSION_NAME) &&
            physicalDeviceSupportsExtension(m_physicalDevice, VK_KHR_EXTERNAL_MEMORY_FD_EXTENSION_NAME);
        m_externalSemaphoreFdSupported =
            physicalDeviceSupportsExtension(m_physicalDevice, VK_KHR_EXTERNAL_SEMAPHORE_EXTENSION_NAME) &&
            physicalDeviceSupportsExtension(m_physicalDevice, VK_KHR_EXTERNAL_SEMAPHORE_FD_EXTENSION_NAME);

        const float queuePriority = 1.0f;
        VkDeviceQueueCreateInfo queueInfo{};
        queueInfo.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
        queueInfo.queueFamilyIndex = m_graphicsQueueFamily;
        queueInfo.queueCount = 1;
        queueInfo.pQueuePriorities = &queuePriority;

        VkDeviceCreateInfo deviceInfo{};
        deviceInfo.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
        deviceInfo.queueCreateInfoCount = 1;
        deviceInfo.pQueueCreateInfos = &queueInfo;
        QVector<const char*> enabledDeviceExtensions;
        if (m_externalMemoryFdSupported) {
            enabledDeviceExtensions.push_back(VK_KHR_EXTERNAL_MEMORY_EXTENSION_NAME);
            enabledDeviceExtensions.push_back(VK_KHR_EXTERNAL_MEMORY_FD_EXTENSION_NAME);
        }
        if (m_externalSemaphoreFdSupported) {
            enabledDeviceExtensions.push_back(VK_KHR_EXTERNAL_SEMAPHORE_EXTENSION_NAME);
            enabledDeviceExtensions.push_back(VK_KHR_EXTERNAL_SEMAPHORE_FD_EXTENSION_NAME);
        }
        deviceInfo.enabledExtensionCount = static_cast<uint32_t>(enabledDeviceExtensions.size());
        deviceInfo.ppEnabledExtensionNames = enabledDeviceExtensions.isEmpty()
                                                ? nullptr
                                                : enabledDeviceExtensions.constData();

        if (vkCreateDevice(m_physicalDevice, &deviceInfo, nullptr, &m_device) != VK_SUCCESS) {
            if (errorMessage) {
                *errorMessage = QStringLiteral("Failed to create Vulkan logical device.");
            }
            return false;
        }

        vkGetDeviceQueue(m_device, m_graphicsQueueFamily, 0, &m_graphicsQueue);
        m_vkGetMemoryFdKHR = reinterpret_cast<PFN_vkGetMemoryFdKHR>(
            vkGetDeviceProcAddr(m_device, "vkGetMemoryFdKHR"));
        qInfo().noquote()
            << QStringLiteral("Vulkan/CUDA interop capability: external_memory_fd=%1 external_semaphore_fd=%2 get_memory_fd=%3")
                   .arg(m_externalMemoryFdSupported ? QStringLiteral("yes") : QStringLiteral("no"),
                        m_externalSemaphoreFdSupported ? QStringLiteral("yes") : QStringLiteral("no"),
                        m_vkGetMemoryFdKHR ? QStringLiteral("yes") : QStringLiteral("no"));

        VkCommandPoolCreateInfo poolInfo{};
        poolInfo.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
        poolInfo.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
        poolInfo.queueFamilyIndex = m_graphicsQueueFamily;

        if (vkCreateCommandPool(m_device, &poolInfo, nullptr, &m_commandPool) != VK_SUCCESS) {
            if (errorMessage) {
                *errorMessage = QStringLiteral("Failed to create Vulkan command pool.");
            }
            return false;
        }

        m_frameSlots.resize(kFrameSlots);
        QVector<VkCommandBuffer> commandBuffers(kFrameSlots);
        VkCommandBufferAllocateInfo allocInfo{};
        allocInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
        allocInfo.commandPool = m_commandPool;
        allocInfo.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        allocInfo.commandBufferCount = kFrameSlots;

        if (vkAllocateCommandBuffers(m_device, &allocInfo, commandBuffers.data()) != VK_SUCCESS) {
            if (errorMessage) {
                *errorMessage = QStringLiteral("Failed to allocate Vulkan command buffers.");
            }
            return false;
        }
        for (int i = 0; i < kFrameSlots; ++i) {
            m_frameSlots[i].commandBuffer = commandBuffers[i];
        }

        VkImageCreateInfo imageInfo{};
        imageInfo.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
        imageInfo.imageType = VK_IMAGE_TYPE_2D;
        imageInfo.extent.width = static_cast<uint32_t>(m_outputSize.width());
        imageInfo.extent.height = static_cast<uint32_t>(m_outputSize.height());
        imageInfo.extent.depth = 1;
        imageInfo.mipLevels = 1;
        imageInfo.arrayLayers = 1;
        imageInfo.format = VK_FORMAT_B8G8R8A8_UNORM;
        imageInfo.tiling = VK_IMAGE_TILING_OPTIMAL;
        imageInfo.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        imageInfo.usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
        imageInfo.samples = VK_SAMPLE_COUNT_1_BIT;
        imageInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;

        if (vkCreateImage(m_device, &imageInfo, nullptr, &m_colorImage) != VK_SUCCESS) {
            if (errorMessage) {
                *errorMessage = QStringLiteral("Failed to create Vulkan offscreen image.");
            }
            return false;
        }

        VkMemoryRequirements memRequirements{};
        vkGetImageMemoryRequirements(m_device, m_colorImage, &memRequirements);

        const uint32_t memoryTypeIndex = findMemoryType(
            m_physicalDevice,
            memRequirements.memoryTypeBits,
            VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
        if (memoryTypeIndex == UINT32_MAX) {
            if (errorMessage) {
                *errorMessage = QStringLiteral("No suitable Vulkan memory type for offscreen image.");
            }
            return false;
        }

        VkMemoryAllocateInfo allocMemoryInfo{};
        allocMemoryInfo.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
        allocMemoryInfo.allocationSize = memRequirements.size;
        allocMemoryInfo.memoryTypeIndex = memoryTypeIndex;

        if (vkAllocateMemory(m_device, &allocMemoryInfo, nullptr, &m_colorImageMemory) != VK_SUCCESS) {
            if (errorMessage) {
                *errorMessage = QStringLiteral("Failed to allocate Vulkan image memory.");
            }
            return false;
        }

        if (vkBindImageMemory(m_device, m_colorImage, m_colorImageMemory, 0) != VK_SUCCESS) {
            if (errorMessage) {
                *errorMessage = QStringLiteral("Failed to bind Vulkan image memory.");
            }
            return false;
        }

        VkImageViewCreateInfo viewInfo{};
        viewInfo.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
        viewInfo.image = m_colorImage;
        viewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
        viewInfo.format = VK_FORMAT_B8G8R8A8_UNORM;
        viewInfo.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        viewInfo.subresourceRange.baseMipLevel = 0;
        viewInfo.subresourceRange.levelCount = 1;
        viewInfo.subresourceRange.baseArrayLayer = 0;
        viewInfo.subresourceRange.layerCount = 1;

        if (vkCreateImageView(m_device, &viewInfo, nullptr, &m_colorImageView) != VK_SUCCESS) {
            if (errorMessage) {
                *errorMessage = QStringLiteral("Failed to create Vulkan image view.");
            }
            return false;
        }

        VkSamplerCreateInfo samplerInfo{};
        samplerInfo.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
        samplerInfo.magFilter = VK_FILTER_LINEAR;
        samplerInfo.minFilter = VK_FILTER_LINEAR;
        samplerInfo.mipmapMode = VK_SAMPLER_MIPMAP_MODE_LINEAR;
        samplerInfo.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        samplerInfo.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        samplerInfo.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        samplerInfo.maxAnisotropy = 1.0f;

        if (vkCreateSampler(m_device, &samplerInfo, nullptr, &m_sampler) != VK_SUCCESS) {
            if (errorMessage) {
                *errorMessage = QStringLiteral("Failed to create Vulkan sampler.");
            }
            return false;
        }

        VkDescriptorSetLayoutBinding textureBinding{};
        textureBinding.binding = 0;
        textureBinding.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        textureBinding.descriptorCount = 1;
        textureBinding.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;

        VkDescriptorSetLayoutCreateInfo descriptorSetLayoutInfo{};
        descriptorSetLayoutInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
        descriptorSetLayoutInfo.bindingCount = 1;
        descriptorSetLayoutInfo.pBindings = &textureBinding;

        if (vkCreateDescriptorSetLayout(m_device,
                                        &descriptorSetLayoutInfo,
                                        nullptr,
                                        &m_descriptorSetLayout) != VK_SUCCESS) {
            if (errorMessage) {
                *errorMessage = QStringLiteral("Failed to create Vulkan descriptor set layout.");
            }
            return false;
        }

        VkDescriptorPoolSize poolSize{};
        poolSize.type = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        poolSize.descriptorCount = 1 + kMaxLayerTextures;

        VkDescriptorPoolCreateInfo descriptorPoolInfo{};
        descriptorPoolInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
        descriptorPoolInfo.poolSizeCount = 1;
        descriptorPoolInfo.pPoolSizes = &poolSize;
        descriptorPoolInfo.maxSets = 1 + kMaxLayerTextures;

        if (vkCreateDescriptorPool(m_device, &descriptorPoolInfo, nullptr, &m_descriptorPool) != VK_SUCCESS) {
            if (errorMessage) {
                *errorMessage = QStringLiteral("Failed to create Vulkan descriptor pool.");
            }
            return false;
        }

        VkDescriptorSetAllocateInfo descriptorSetAllocInfo{};
        descriptorSetAllocInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
        descriptorSetAllocInfo.descriptorPool = m_descriptorPool;
        descriptorSetAllocInfo.descriptorSetCount = 1;
        descriptorSetAllocInfo.pSetLayouts = &m_descriptorSetLayout;

        if (vkAllocateDescriptorSets(m_device, &descriptorSetAllocInfo, &m_descriptorSet) != VK_SUCCESS) {
            if (errorMessage) {
                *errorMessage = QStringLiteral("Failed to allocate Vulkan descriptor set.");
            }
            return false;
        }

        auto createLayerSlot = [&](QString* err) -> bool {
            LayerTextureSlot slot;
            VkImageCreateInfo layerImageInfo{};
            layerImageInfo.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
            layerImageInfo.imageType = VK_IMAGE_TYPE_2D;
            layerImageInfo.extent.width = static_cast<uint32_t>(m_outputSize.width());
            layerImageInfo.extent.height = static_cast<uint32_t>(m_outputSize.height());
            layerImageInfo.extent.depth = 1;
            layerImageInfo.mipLevels = 1;
            layerImageInfo.arrayLayers = 1;
            layerImageInfo.format = VK_FORMAT_R8G8B8A8_UNORM;
            layerImageInfo.tiling = VK_IMAGE_TILING_OPTIMAL;
            layerImageInfo.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
            layerImageInfo.usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
            layerImageInfo.samples = VK_SAMPLE_COUNT_1_BIT;
            layerImageInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
            if (vkCreateImage(m_device, &layerImageInfo, nullptr, &slot.image) != VK_SUCCESS) {
                if (err) *err = QStringLiteral("Failed to create Vulkan layer image.");
                return false;
            }
            VkMemoryRequirements req{};
            vkGetImageMemoryRequirements(m_device, slot.image, &req);
            const uint32_t memType = findMemoryType(
                m_physicalDevice, req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
            if (memType == UINT32_MAX) {
                if (err) *err = QStringLiteral("No memory type for Vulkan layer image.");
                return false;
            }
            VkMemoryAllocateInfo ai{};
            ai.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
            ai.allocationSize = req.size;
            ai.memoryTypeIndex = memType;
            if (vkAllocateMemory(m_device, &ai, nullptr, &slot.memory) != VK_SUCCESS ||
                vkBindImageMemory(m_device, slot.image, slot.memory, 0) != VK_SUCCESS) {
                if (err) *err = QStringLiteral("Failed to allocate/bind Vulkan layer image memory.");
                return false;
            }
            VkImageViewCreateInfo vi{};
            vi.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
            vi.image = slot.image;
            vi.viewType = VK_IMAGE_VIEW_TYPE_2D;
            vi.format = VK_FORMAT_R8G8B8A8_UNORM;
            vi.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
            vi.subresourceRange.levelCount = 1;
            vi.subresourceRange.layerCount = 1;
            if (vkCreateImageView(m_device, &vi, nullptr, &slot.view) != VK_SUCCESS) {
                if (err) *err = QStringLiteral("Failed to create Vulkan layer image view.");
                return false;
            }
            VkDescriptorSetAllocateInfo dsi{};
            dsi.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
            dsi.descriptorPool = m_descriptorPool;
            dsi.descriptorSetCount = 1;
            dsi.pSetLayouts = &m_descriptorSetLayout;
            if (vkAllocateDescriptorSets(m_device, &dsi, &slot.descriptorSet) != VK_SUCCESS) {
                if (err) *err = QStringLiteral("Failed to allocate Vulkan layer descriptor set.");
                return false;
            }
            VkDescriptorImageInfo di{};
            di.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
            di.imageView = slot.view;
            di.sampler = m_sampler;
            VkWriteDescriptorSet w{};
            w.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
            w.dstSet = slot.descriptorSet;
            w.dstBinding = 0;
            w.descriptorCount = 1;
            w.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
            w.pImageInfo = &di;
            vkUpdateDescriptorSets(m_device, 1, &w, 0, nullptr);
            m_layerSlots.push_back(slot);
            return true;
        };
        for (int i = 0; i < kMaxLayerTextures; ++i) {
            if (!createLayerSlot(errorMessage)) {
                return false;
            }
        }

        VkAttachmentDescription colorAttachment{};
        colorAttachment.format = VK_FORMAT_B8G8R8A8_UNORM;
        colorAttachment.samples = VK_SAMPLE_COUNT_1_BIT;
        colorAttachment.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
        colorAttachment.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
        colorAttachment.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
        colorAttachment.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
        colorAttachment.initialLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        colorAttachment.finalLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;

        VkAttachmentReference colorAttachmentRef{};
        colorAttachmentRef.attachment = 0;
        colorAttachmentRef.layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;

        VkSubpassDescription subpass{};
        subpass.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
        subpass.colorAttachmentCount = 1;
        subpass.pColorAttachments = &colorAttachmentRef;

        VkRenderPassCreateInfo renderPassInfo{};
        renderPassInfo.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO;
        renderPassInfo.attachmentCount = 1;
        renderPassInfo.pAttachments = &colorAttachment;
        renderPassInfo.subpassCount = 1;
        renderPassInfo.pSubpasses = &subpass;

        if (vkCreateRenderPass(m_device, &renderPassInfo, nullptr, &m_renderPass) != VK_SUCCESS) {
            if (errorMessage) {
                *errorMessage = QStringLiteral("Failed to create Vulkan render pass.");
            }
            return false;
        }

        VkImageView attachments[] = {m_colorImageView};
        VkFramebufferCreateInfo framebufferInfo{};
        framebufferInfo.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
        framebufferInfo.renderPass = m_renderPass;
        framebufferInfo.attachmentCount = 1;
        framebufferInfo.pAttachments = attachments;
        framebufferInfo.width = static_cast<uint32_t>(m_outputSize.width());
        framebufferInfo.height = static_cast<uint32_t>(m_outputSize.height());
        framebufferInfo.layers = 1;

        if (vkCreateFramebuffer(m_device, &framebufferInfo, nullptr, &m_framebuffer) != VK_SUCCESS) {
            if (errorMessage) {
                *errorMessage = QStringLiteral("Failed to create Vulkan framebuffer.");
            }
            return false;
        }

        const VkDeviceSize stagingSize =
            static_cast<VkDeviceSize>(m_outputSize.width()) *
            static_cast<VkDeviceSize>(m_outputSize.height()) * 4;
        VkBufferCreateInfo stagingBufInfo{};
        stagingBufInfo.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
        stagingBufInfo.size = stagingSize;
        stagingBufInfo.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT;
        stagingBufInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
        VkFenceCreateInfo fenceInfo{};
        fenceInfo.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
        for (int i = 0; i < m_frameSlots.size(); ++i) {
            FrameSlot& slot = m_frameSlots[i];
            if (vkCreateBuffer(m_device, &stagingBufInfo, nullptr, &slot.stagingBuffer) != VK_SUCCESS) {
                if (errorMessage) {
                    *errorMessage = QStringLiteral("Failed to create Vulkan staging buffer.");
                }
                return false;
            }
            VkMemoryRequirements stagingReq{};
            vkGetBufferMemoryRequirements(m_device, slot.stagingBuffer, &stagingReq);
            VkMemoryPropertyFlags stagingFlags = 0;
            uint32_t stagingType = findMemoryTypePreferred(
                m_physicalDevice,
                stagingReq.memoryTypeBits,
                VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT,
                VK_MEMORY_PROPERTY_HOST_CACHED_BIT,
                &stagingFlags);
            if (stagingType == UINT32_MAX) {
                if (errorMessage) {
                    *errorMessage = QStringLiteral("No suitable Vulkan staging memory type.");
                }
                return false;
            }
            VkMemoryAllocateInfo stagingAlloc{};
            stagingAlloc.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
            stagingAlloc.allocationSize = stagingReq.size;
            stagingAlloc.memoryTypeIndex = stagingType;
            slot.stagingAllocationSize = stagingReq.size;
            slot.stagingHostCoherent = (stagingFlags & VK_MEMORY_PROPERTY_HOST_COHERENT_BIT) != 0;
            if (vkAllocateMemory(m_device, &stagingAlloc, nullptr, &slot.stagingMemory) != VK_SUCCESS ||
                vkBindBufferMemory(m_device, slot.stagingBuffer, slot.stagingMemory, 0) != VK_SUCCESS) {
                if (errorMessage) {
                    *errorMessage = QStringLiteral("Failed to allocate/bind Vulkan staging memory.");
                }
                return false;
            }
            if (vkMapMemory(m_device, slot.stagingMemory, 0, VK_WHOLE_SIZE, 0, &slot.stagingMapped) != VK_SUCCESS) {
                if (errorMessage) {
                    *errorMessage = QStringLiteral("Failed to persistently map Vulkan staging memory.");
                }
                return false;
            }
            if (m_externalMemoryFdSupported && m_externalSemaphoreFdSupported && m_vkGetMemoryFdKHR) {
                VkExternalMemoryBufferCreateInfo externalBufferInfo{};
                externalBufferInfo.sType = VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_BUFFER_CREATE_INFO;
                externalBufferInfo.handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_FD_BIT;
                VkBufferCreateInfo exportBufferInfo = stagingBufInfo;
                exportBufferInfo.pNext = &externalBufferInfo;
                exportBufferInfo.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT;
                if (vkCreateBuffer(m_device, &exportBufferInfo, nullptr, &slot.cudaExportBuffer) == VK_SUCCESS) {
                    VkMemoryRequirements exportReq{};
                    vkGetBufferMemoryRequirements(m_device, slot.cudaExportBuffer, &exportReq);
                    const uint32_t exportType = findMemoryType(
                        m_physicalDevice,
                        exportReq.memoryTypeBits,
                        VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
                    VkExportMemoryAllocateInfo exportAllocInfo{};
                    exportAllocInfo.sType = VK_STRUCTURE_TYPE_EXPORT_MEMORY_ALLOCATE_INFO;
                    exportAllocInfo.handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_FD_BIT;
                    VkMemoryAllocateInfo exportAlloc{};
                    exportAlloc.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
                    exportAlloc.pNext = &exportAllocInfo;
                    exportAlloc.allocationSize = exportReq.size;
                    exportAlloc.memoryTypeIndex = exportType;
                    if (exportType != UINT32_MAX &&
                        vkAllocateMemory(m_device, &exportAlloc, nullptr, &slot.cudaExportMemory) == VK_SUCCESS &&
                        vkBindBufferMemory(m_device, slot.cudaExportBuffer, slot.cudaExportMemory, 0) == VK_SUCCESS) {
                        slot.cudaExportAllocationSize = exportReq.size;
                        m_cudaExportBuffersReady = true;
                    } else {
                        if (slot.cudaExportMemory != VK_NULL_HANDLE) {
                            vkFreeMemory(m_device, slot.cudaExportMemory, nullptr);
                            slot.cudaExportMemory = VK_NULL_HANDLE;
                        }
                        vkDestroyBuffer(m_device, slot.cudaExportBuffer, nullptr);
                        slot.cudaExportBuffer = VK_NULL_HANDLE;
                    }
                }
            }
            if (vkCreateFence(m_device, &fenceInfo, nullptr, &slot.fence) != VK_SUCCESS) {
                if (errorMessage) {
                    *errorMessage = QStringLiteral("Failed to create Vulkan submit fence.");
                }
                return false;
            }
        }
        if (supportsCudaExternalMemoryInterop() && !m_cudaExportBuffersReady) {
            qWarning().noquote() << QStringLiteral(
                "Vulkan/CUDA interop capability present but exportable Vulkan buffers could not be allocated.");
        }
        useSlot(0);

        auto createAttachment = [&](VkFormat format,
                                    uint32_t width,
                                    uint32_t height,
                                    VkImage* image,
                                    VkDeviceMemory* memory,
                                    VkImageView* view,
                                    QString* err) -> bool {
            VkImageCreateInfo ci{};
            ci.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
            ci.imageType = VK_IMAGE_TYPE_2D;
            ci.extent = {width, height, 1};
            ci.mipLevels = 1;
            ci.arrayLayers = 1;
            ci.format = format;
            ci.tiling = VK_IMAGE_TILING_OPTIMAL;
            ci.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
            ci.usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT |
                       VK_IMAGE_USAGE_TRANSFER_SRC_BIT |
                       VK_IMAGE_USAGE_SAMPLED_BIT |
                       VK_IMAGE_USAGE_STORAGE_BIT;
            ci.samples = VK_SAMPLE_COUNT_1_BIT;
            ci.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
            if (vkCreateImage(m_device, &ci, nullptr, image) != VK_SUCCESS) {
                if (err) *err = QStringLiteral("Failed to create Vulkan NV12 attachment image.");
                return false;
            }
            VkMemoryRequirements req{};
            vkGetImageMemoryRequirements(m_device, *image, &req);
            const uint32_t memType = findMemoryType(m_physicalDevice, req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
            if (memType == UINT32_MAX) {
                if (err) *err = QStringLiteral("No Vulkan memory type for NV12 attachment.");
                return false;
            }
            VkMemoryAllocateInfo ai{};
            ai.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
            ai.allocationSize = req.size;
            ai.memoryTypeIndex = memType;
            if (vkAllocateMemory(m_device, &ai, nullptr, memory) != VK_SUCCESS ||
                vkBindImageMemory(m_device, *image, *memory, 0) != VK_SUCCESS) {
                if (err) *err = QStringLiteral("Failed to allocate/bind Vulkan NV12 attachment memory.");
                return false;
            }
            VkImageViewCreateInfo vi{};
            vi.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
            vi.image = *image;
            vi.viewType = VK_IMAGE_VIEW_TYPE_2D;
            vi.format = format;
            vi.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
            vi.subresourceRange.levelCount = 1;
            vi.subresourceRange.layerCount = 1;
            if (vkCreateImageView(m_device, &vi, nullptr, view) != VK_SUCCESS) {
                if (err) *err = QStringLiteral("Failed to create Vulkan NV12 attachment view.");
                return false;
            }
            return true;
        };
        if (!createAttachment(VK_FORMAT_R8_UNORM,
                              static_cast<uint32_t>(m_outputSize.width()),
                              static_cast<uint32_t>(m_outputSize.height()),
                              &m_nv12YImage,
                              &m_nv12YImageMemory,
                              &m_nv12YImageView,
                              errorMessage)) {
            return false;
        }
        if (!createAttachment(VK_FORMAT_R8G8_UNORM,
                              static_cast<uint32_t>(qMax(1, m_outputSize.width() / 2)),
                              static_cast<uint32_t>(qMax(1, m_outputSize.height() / 2)),
                              &m_nv12UvImage,
                              &m_nv12UvImageMemory,
                              &m_nv12UvImageView,
                              errorMessage)) {
            return false;
        }
        if (!createAttachment(VK_FORMAT_R8_UNORM,
                              static_cast<uint32_t>(qMax(1, m_outputSize.width() / 2)),
                              static_cast<uint32_t>(qMax(1, m_outputSize.height() / 2)),
                              &m_yuv420pUImage,
                              &m_yuv420pUImageMemory,
                              &m_yuv420pUImageView,
                              errorMessage)) {
            return false;
        }
        if (!createAttachment(VK_FORMAT_R8_UNORM,
                              static_cast<uint32_t>(qMax(1, m_outputSize.width() / 2)),
                              static_cast<uint32_t>(qMax(1, m_outputSize.height() / 2)),
                              &m_yuv420pVImage,
                              &m_yuv420pVImageMemory,
                              &m_yuv420pVImageView,
                              errorMessage)) {
            return false;
        }

        auto createColorRenderPass = [&](VkFormat format, VkRenderPass* pass, QString* err) -> bool {
            VkAttachmentDescription a{};
            a.format = format;
            a.samples = VK_SAMPLE_COUNT_1_BIT;
            a.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
            a.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
            a.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
            a.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
            a.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
            a.finalLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
            VkAttachmentReference ref{};
            ref.attachment = 0;
            ref.layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
            VkSubpassDescription sub{};
            sub.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
            sub.colorAttachmentCount = 1;
            sub.pColorAttachments = &ref;
            VkRenderPassCreateInfo rp{};
            rp.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO;
            rp.attachmentCount = 1;
            rp.pAttachments = &a;
            rp.subpassCount = 1;
            rp.pSubpasses = &sub;
            if (vkCreateRenderPass(m_device, &rp, nullptr, pass) != VK_SUCCESS) {
                if (err) *err = QStringLiteral("Failed to create Vulkan NV12 render pass.");
                return false;
            }
            return true;
        };
        if (!createColorRenderPass(VK_FORMAT_R8_UNORM, &m_nv12YRenderPass, errorMessage) ||
            !createColorRenderPass(VK_FORMAT_R8G8_UNORM, &m_nv12UvRenderPass, errorMessage)) {
            return false;
        }
        VkImageView yAtt[] = {m_nv12YImageView};
        VkFramebufferCreateInfo yFb{};
        yFb.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
        yFb.renderPass = m_nv12YRenderPass;
        yFb.attachmentCount = 1;
        yFb.pAttachments = yAtt;
        yFb.width = static_cast<uint32_t>(m_outputSize.width());
        yFb.height = static_cast<uint32_t>(m_outputSize.height());
        yFb.layers = 1;
        if (vkCreateFramebuffer(m_device, &yFb, nullptr, &m_nv12YFramebuffer) != VK_SUCCESS) {
            if (errorMessage) *errorMessage = QStringLiteral("Failed to create Vulkan NV12 Y framebuffer.");
            return false;
        }
        VkImageView uvAtt[] = {m_nv12UvImageView};
        VkFramebufferCreateInfo uvFb{};
        uvFb.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
        uvFb.renderPass = m_nv12UvRenderPass;
        uvFb.attachmentCount = 1;
        uvFb.pAttachments = uvAtt;
        uvFb.width = static_cast<uint32_t>(qMax(1, m_outputSize.width() / 2));
        uvFb.height = static_cast<uint32_t>(qMax(1, m_outputSize.height() / 2));
        uvFb.layers = 1;
        if (vkCreateFramebuffer(m_device, &uvFb, nullptr, &m_nv12UvFramebuffer) != VK_SUCCESS) {
            if (errorMessage) *errorMessage = QStringLiteral("Failed to create Vulkan NV12 UV framebuffer.");
            return false;
        }
        VkImageView uAtt[] = {m_yuv420pUImageView};
        VkFramebufferCreateInfo uFb = uvFb;
        uFb.renderPass = m_nv12YRenderPass;
        uFb.pAttachments = uAtt;
        if (vkCreateFramebuffer(m_device, &uFb, nullptr, &m_yuv420pUFramebuffer) != VK_SUCCESS) {
            if (errorMessage) *errorMessage = QStringLiteral("Failed to create Vulkan YUV420P U framebuffer.");
            return false;
        }
        VkImageView vAtt[] = {m_yuv420pVImageView};
        VkFramebufferCreateInfo vFb = uvFb;
        vFb.renderPass = m_nv12YRenderPass;
        vFb.pAttachments = vAtt;
        if (vkCreateFramebuffer(m_device, &vFb, nullptr, &m_yuv420pVFramebuffer) != VK_SUCCESS) {
            if (errorMessage) *errorMessage = QStringLiteral("Failed to create Vulkan YUV420P V framebuffer.");
            return false;
        }

        VkDescriptorSetLayoutBinding yuvBindings[4]{};
        yuvBindings[0].binding = 0;
        yuvBindings[0].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        yuvBindings[0].descriptorCount = 1;
        yuvBindings[0].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
        for (uint32_t i = 1; i < 4; ++i) {
            yuvBindings[i].binding = i;
            yuvBindings[i].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
            yuvBindings[i].descriptorCount = 1;
            yuvBindings[i].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
        }
        VkDescriptorSetLayoutCreateInfo yuvLayoutInfo{};
        yuvLayoutInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
        yuvLayoutInfo.bindingCount = 4;
        yuvLayoutInfo.pBindings = yuvBindings;
        if (vkCreateDescriptorSetLayout(m_device, &yuvLayoutInfo, nullptr, &m_yuvComputeDescriptorSetLayout) != VK_SUCCESS) {
            if (errorMessage) *errorMessage = QStringLiteral("Failed to create Vulkan YUV compute descriptor layout.");
            return false;
        }
        VkDescriptorPoolSize yuvPoolSizes[2]{};
        yuvPoolSizes[0].type = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        yuvPoolSizes[0].descriptorCount = 1;
        yuvPoolSizes[1].type = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
        yuvPoolSizes[1].descriptorCount = 3;
        VkDescriptorPoolCreateInfo yuvPoolInfo{};
        yuvPoolInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
        yuvPoolInfo.poolSizeCount = 2;
        yuvPoolInfo.pPoolSizes = yuvPoolSizes;
        yuvPoolInfo.maxSets = 1;
        if (vkCreateDescriptorPool(m_device, &yuvPoolInfo, nullptr, &m_yuvComputeDescriptorPool) != VK_SUCCESS) {
            if (errorMessage) *errorMessage = QStringLiteral("Failed to create Vulkan YUV compute descriptor pool.");
            return false;
        }
        VkDescriptorSetAllocateInfo yuvSetInfo{};
        yuvSetInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
        yuvSetInfo.descriptorPool = m_yuvComputeDescriptorPool;
        yuvSetInfo.descriptorSetCount = 1;
        yuvSetInfo.pSetLayouts = &m_yuvComputeDescriptorSetLayout;
        if (vkAllocateDescriptorSets(m_device, &yuvSetInfo, &m_yuvComputeDescriptorSet) != VK_SUCCESS) {
            if (errorMessage) *errorMessage = QStringLiteral("Failed to allocate Vulkan YUV compute descriptor set.");
            return false;
        }
        VkDescriptorImageInfo yuvSrc{};
        yuvSrc.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        yuvSrc.imageView = m_colorImageView;
        yuvSrc.sampler = m_sampler;
        VkDescriptorImageInfo yuvImages[3]{};
        yuvImages[0].imageLayout = VK_IMAGE_LAYOUT_GENERAL;
        yuvImages[0].imageView = m_nv12YImageView;
        yuvImages[1].imageLayout = VK_IMAGE_LAYOUT_GENERAL;
        yuvImages[1].imageView = m_yuv420pUImageView;
        yuvImages[2].imageLayout = VK_IMAGE_LAYOUT_GENERAL;
        yuvImages[2].imageView = m_yuv420pVImageView;
        VkWriteDescriptorSet yuvWrites[4]{};
        yuvWrites[0].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        yuvWrites[0].dstSet = m_yuvComputeDescriptorSet;
        yuvWrites[0].dstBinding = 0;
        yuvWrites[0].descriptorCount = 1;
        yuvWrites[0].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        yuvWrites[0].pImageInfo = &yuvSrc;
        for (uint32_t i = 1; i < 4; ++i) {
            yuvWrites[i].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
            yuvWrites[i].dstSet = m_yuvComputeDescriptorSet;
            yuvWrites[i].dstBinding = i;
            yuvWrites[i].descriptorCount = 1;
            yuvWrites[i].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
            yuvWrites[i].pImageInfo = &yuvImages[i - 1];
        }
        vkUpdateDescriptorSets(m_device, 4, yuvWrites, 0, nullptr);

        VkPushConstantRange effectsPushConstantRange{};
        effectsPushConstantRange.stageFlags = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;
        effectsPushConstantRange.offset = 0;
        effectsPushConstantRange.size = 128;

        VkPipelineLayoutCreateInfo effectsLayoutInfo{};
        effectsLayoutInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
        effectsLayoutInfo.setLayoutCount = 1;
        effectsLayoutInfo.pSetLayouts = &m_descriptorSetLayout;
        effectsLayoutInfo.pushConstantRangeCount = 1;
        effectsLayoutInfo.pPushConstantRanges = &effectsPushConstantRange;
        if (vkCreatePipelineLayout(m_device, &effectsLayoutInfo, nullptr, &m_effectsPipelineLayout) != VK_SUCCESS) {
            if (errorMessage) {
                *errorMessage = QStringLiteral("Failed to create Vulkan effects pipeline layout.");
            }
            return false;
        }

        VkPushConstantRange maskPushConstantRange{};
        maskPushConstantRange.stageFlags = VK_SHADER_STAGE_VERTEX_BIT;
        maskPushConstantRange.offset = 0;
        maskPushConstantRange.size = 64;
        VkPipelineLayoutCreateInfo maskLayoutInfo{};
        maskLayoutInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
        maskLayoutInfo.pushConstantRangeCount = 1;
        maskLayoutInfo.pPushConstantRanges = &maskPushConstantRange;
        if (vkCreatePipelineLayout(m_device, &maskLayoutInfo, nullptr, &m_maskPipelineLayout) != VK_SUCCESS) {
            if (errorMessage) {
                *errorMessage = QStringLiteral("Failed to create Vulkan mask pipeline layout.");
            }
            return false;
        }

        VkPipelineLayoutCreateInfo nv12LayoutInfo{};
        nv12LayoutInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
        nv12LayoutInfo.setLayoutCount = 1;
        nv12LayoutInfo.pSetLayouts = &m_descriptorSetLayout;
        if (vkCreatePipelineLayout(m_device, &nv12LayoutInfo, nullptr, &m_nv12PipelineLayout) != VK_SUCCESS) {
            if (errorMessage) {
                *errorMessage = QStringLiteral("Failed to create Vulkan NV12 pipeline layout.");
            }
            return false;
        }
        VkPipelineLayoutCreateInfo yuvComputeLayoutInfo{};
        yuvComputeLayoutInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
        yuvComputeLayoutInfo.setLayoutCount = 1;
        yuvComputeLayoutInfo.pSetLayouts = &m_yuvComputeDescriptorSetLayout;
        if (vkCreatePipelineLayout(m_device, &yuvComputeLayoutInfo, nullptr, &m_yuvComputePipelineLayout) != VK_SUCCESS) {
            if (errorMessage) {
                *errorMessage = QStringLiteral("Failed to create Vulkan YUV compute pipeline layout.");
            }
            return false;
        }

        const QString shaderDir = QStringLiteral(JCUT_VULKAN_SHADER_DIR);
        m_effectsVertModule = createShaderModule(m_device, readBinaryFile(shaderDir + QStringLiteral("/effects.vert.spv")));
        m_effectsFragModule = createShaderModule(m_device, readBinaryFile(shaderDir + QStringLiteral("/effects.frag.spv")));
        m_maskVertModule = createShaderModule(m_device, readBinaryFile(shaderDir + QStringLiteral("/mask.vert.spv")));
        m_maskFragModule = createShaderModule(m_device, readBinaryFile(shaderDir + QStringLiteral("/mask.frag.spv")));
        m_nv12VertModule = createShaderModule(m_device, readBinaryFile(shaderDir + QStringLiteral("/nv12.vert.spv")));
        m_nv12YFragModule = createShaderModule(m_device, readBinaryFile(shaderDir + QStringLiteral("/nv12_y.frag.spv")));
        m_nv12UvFragModule = createShaderModule(m_device, readBinaryFile(shaderDir + QStringLiteral("/nv12_uv.frag.spv")));
        m_yuv420pUFragModule = createShaderModule(m_device, readBinaryFile(shaderDir + QStringLiteral("/yuv420p_u.frag.spv")));
        m_yuv420pVFragModule = createShaderModule(m_device, readBinaryFile(shaderDir + QStringLiteral("/yuv420p_v.frag.spv")));
        m_yuv420pComputeModule = createShaderModule(m_device, readBinaryFile(shaderDir + QStringLiteral("/yuv420p.comp.spv")));

        if (m_effectsVertModule == VK_NULL_HANDLE || m_effectsFragModule == VK_NULL_HANDLE ||
            m_maskVertModule == VK_NULL_HANDLE || m_maskFragModule == VK_NULL_HANDLE ||
            m_nv12VertModule == VK_NULL_HANDLE || m_nv12YFragModule == VK_NULL_HANDLE ||
            m_nv12UvFragModule == VK_NULL_HANDLE ||
            m_yuv420pUFragModule == VK_NULL_HANDLE || m_yuv420pVFragModule == VK_NULL_HANDLE ||
            m_yuv420pComputeModule == VK_NULL_HANDLE) {
            if (errorMessage) {
                *errorMessage = QStringLiteral(
                    "Failed to load Vulkan SPIR-V shader modules. Ensure shader build step succeeded.");
            }
            return false;
        }

        VkDescriptorImageInfo imageInfoDesc{};
        imageInfoDesc.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        imageInfoDesc.imageView = m_colorImageView;
        imageInfoDesc.sampler = m_sampler;
        VkWriteDescriptorSet write{};
        write.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        write.dstSet = m_descriptorSet;
        write.dstBinding = 0;
        write.descriptorCount = 1;
        write.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        write.pImageInfo = &imageInfoDesc;
        vkUpdateDescriptorSets(m_device, 1, &write, 0, nullptr);

        auto createPipeline = [&](VkShaderModule vert,
                                  VkShaderModule frag,
                                  VkPipelineLayout layout,
                                  VkRenderPass renderPass,
                                  VkPipeline* outPipeline) -> bool {
            VkPipelineShaderStageCreateInfo shaderStages[2]{};
            shaderStages[0].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
            shaderStages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
            shaderStages[0].module = vert;
            shaderStages[0].pName = "main";
            shaderStages[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
            shaderStages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
            shaderStages[1].module = frag;
            shaderStages[1].pName = "main";

            VkPipelineVertexInputStateCreateInfo vertexInput{};
            vertexInput.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;
            vertexInput.vertexBindingDescriptionCount = 0;
            vertexInput.vertexAttributeDescriptionCount = 0;

            VkPipelineInputAssemblyStateCreateInfo inputAssembly{};
            inputAssembly.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
            inputAssembly.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP;

            VkPipelineViewportStateCreateInfo viewportState{};
            viewportState.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
            viewportState.viewportCount = 1;
            viewportState.scissorCount = 1;

            VkPipelineRasterizationStateCreateInfo rasterizer{};
            rasterizer.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
            rasterizer.polygonMode = VK_POLYGON_MODE_FILL;
            rasterizer.lineWidth = 1.0f;
            rasterizer.cullMode = VK_CULL_MODE_NONE;
            rasterizer.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;

            VkPipelineMultisampleStateCreateInfo multisampling{};
            multisampling.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
            multisampling.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;

            VkPipelineColorBlendAttachmentState colorBlendAttachment{};
            colorBlendAttachment.colorWriteMask =
                VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
                VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
            colorBlendAttachment.blendEnable = VK_TRUE;
            colorBlendAttachment.srcColorBlendFactor = VK_BLEND_FACTOR_ONE;
            colorBlendAttachment.dstColorBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
            colorBlendAttachment.colorBlendOp = VK_BLEND_OP_ADD;
            colorBlendAttachment.srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
            colorBlendAttachment.dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
            colorBlendAttachment.alphaBlendOp = VK_BLEND_OP_ADD;
            VkPipelineColorBlendStateCreateInfo colorBlending{};
            colorBlending.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
            colorBlending.attachmentCount = 1;
            colorBlending.pAttachments = &colorBlendAttachment;
            VkDynamicState dynamicStates[] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
            VkPipelineDynamicStateCreateInfo dynamicState{};
            dynamicState.sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO;
            dynamicState.dynamicStateCount = 2;
            dynamicState.pDynamicStates = dynamicStates;

            VkGraphicsPipelineCreateInfo pipelineInfo{};
            pipelineInfo.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
            pipelineInfo.stageCount = 2;
            pipelineInfo.pStages = shaderStages;
            pipelineInfo.pVertexInputState = &vertexInput;
            pipelineInfo.pInputAssemblyState = &inputAssembly;
            pipelineInfo.pViewportState = &viewportState;
            pipelineInfo.pRasterizationState = &rasterizer;
            pipelineInfo.pMultisampleState = &multisampling;
            pipelineInfo.pColorBlendState = &colorBlending;
            pipelineInfo.pDynamicState = &dynamicState;
            pipelineInfo.layout = layout;
            pipelineInfo.renderPass = renderPass;
            pipelineInfo.subpass = 0;

            return vkCreateGraphicsPipelines(m_device, VK_NULL_HANDLE, 1, &pipelineInfo, nullptr, outPipeline) == VK_SUCCESS;
        };

        VkComputePipelineCreateInfo computeInfo{};
        computeInfo.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO;
        computeInfo.stage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        computeInfo.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
        computeInfo.stage.module = m_yuv420pComputeModule;
        computeInfo.stage.pName = "main";
        computeInfo.layout = m_yuvComputePipelineLayout;
        if (vkCreateComputePipelines(m_device, VK_NULL_HANDLE, 1, &computeInfo, nullptr, &m_yuv420pComputePipeline) != VK_SUCCESS) {
            if (errorMessage) {
                *errorMessage = QStringLiteral("Failed to create Vulkan YUV420P compute pipeline.");
            }
            return false;
        }

        if (!createPipeline(m_effectsVertModule, m_effectsFragModule, m_effectsPipelineLayout, m_renderPass, &m_effectsPipeline) ||
            !createPipeline(m_maskVertModule, m_maskFragModule, m_maskPipelineLayout, m_renderPass, &m_maskPipeline) ||
            !createPipeline(m_nv12VertModule, m_nv12YFragModule, m_nv12PipelineLayout, m_nv12YRenderPass, &m_nv12YPipeline) ||
            !createPipeline(m_nv12VertModule, m_nv12UvFragModule, m_nv12PipelineLayout, m_nv12UvRenderPass, &m_nv12UvPipeline) ||
            !createPipeline(m_nv12VertModule, m_yuv420pUFragModule, m_nv12PipelineLayout, m_nv12YRenderPass, &m_yuv420pUPipeline) ||
            !createPipeline(m_nv12VertModule, m_yuv420pVFragModule, m_nv12PipelineLayout, m_nv12YRenderPass, &m_yuv420pVPipeline)) {
            if (errorMessage) {
                *errorMessage = QStringLiteral("Failed to create Vulkan graphics pipelines.");
            }
            return false;
        }

        m_initialized = true;
        return true;
    }

    void release()
    {
        if (m_nv12ScratchFrame) {
            av_frame_free(&m_nv12ScratchFrame);
        }
        if (m_device != VK_NULL_HANDLE) {
            vkDeviceWaitIdle(m_device);
        }
        for (FrameSlot& slot : m_frameSlots) {
#if JCUT_HAS_CUDA_DRIVER
            if (slot.cudaExternalMemory) {
                CUcontext previous = nullptr;
                if (slot.cudaImportContext) {
                    cuCtxPushCurrent(slot.cudaImportContext);
                }
                cuDestroyExternalMemory(slot.cudaExternalMemory);
                if (slot.cudaImportContext) {
                    cuCtxPopCurrent(&previous);
                }
                slot.cudaExternalMemory = nullptr;
                slot.cudaExternalDevicePtr = 0;
                slot.cudaImportContext = nullptr;
            }
#endif
            if (slot.stagingMapped && slot.stagingMemory != VK_NULL_HANDLE) {
                vkUnmapMemory(m_device, slot.stagingMemory);
                slot.stagingMapped = nullptr;
            }
            if (slot.fence != VK_NULL_HANDLE) {
                vkDestroyFence(m_device, slot.fence, nullptr);
                slot.fence = VK_NULL_HANDLE;
            }
            if (slot.stagingBuffer != VK_NULL_HANDLE) {
                vkDestroyBuffer(m_device, slot.stagingBuffer, nullptr);
                slot.stagingBuffer = VK_NULL_HANDLE;
            }
            if (slot.stagingMemory != VK_NULL_HANDLE) {
                vkFreeMemory(m_device, slot.stagingMemory, nullptr);
                slot.stagingMemory = VK_NULL_HANDLE;
            }
            if (slot.cudaExportBuffer != VK_NULL_HANDLE) {
                vkDestroyBuffer(m_device, slot.cudaExportBuffer, nullptr);
                slot.cudaExportBuffer = VK_NULL_HANDLE;
            }
            if (slot.cudaExportMemory != VK_NULL_HANDLE) {
                vkFreeMemory(m_device, slot.cudaExportMemory, nullptr);
                slot.cudaExportMemory = VK_NULL_HANDLE;
            }
            slot.commandBuffer = VK_NULL_HANDLE;
            slot.inFlight = false;
        }
        m_frameSlots.clear();
        m_activeSlotIndex = -1;
        m_pendingNv12SlotIndices.clear();
        m_pendingNv12CudaSlotIndices.clear();
        m_pendingYuvSlotIndices.clear();
        m_commandBuffer = VK_NULL_HANDLE;
        m_stagingBuffer = VK_NULL_HANDLE;
        m_stagingMemory = VK_NULL_HANDLE;
        m_stagingMapped = nullptr;
        m_submitFence = VK_NULL_HANDLE;
        m_cudaExportBuffersReady = false;
        for (LayerTextureSlot& slot : m_layerSlots) {
            if (slot.view != VK_NULL_HANDLE) {
                vkDestroyImageView(m_device, slot.view, nullptr);
                slot.view = VK_NULL_HANDLE;
            }
            if (slot.image != VK_NULL_HANDLE) {
                vkDestroyImage(m_device, slot.image, nullptr);
                slot.image = VK_NULL_HANDLE;
            }
            if (slot.memory != VK_NULL_HANDLE) {
                vkFreeMemory(m_device, slot.memory, nullptr);
                slot.memory = VK_NULL_HANDLE;
            }
        }
        m_layerSlots.clear();
        if (m_effectsPipeline != VK_NULL_HANDLE) {
            vkDestroyPipeline(m_device, m_effectsPipeline, nullptr);
            m_effectsPipeline = VK_NULL_HANDLE;
        }
        if (m_maskPipeline != VK_NULL_HANDLE) {
            vkDestroyPipeline(m_device, m_maskPipeline, nullptr);
            m_maskPipeline = VK_NULL_HANDLE;
        }
        if (m_nv12YPipeline != VK_NULL_HANDLE) {
            vkDestroyPipeline(m_device, m_nv12YPipeline, nullptr);
            m_nv12YPipeline = VK_NULL_HANDLE;
        }
        if (m_nv12UvPipeline != VK_NULL_HANDLE) {
            vkDestroyPipeline(m_device, m_nv12UvPipeline, nullptr);
            m_nv12UvPipeline = VK_NULL_HANDLE;
        }
        if (m_yuv420pUPipeline != VK_NULL_HANDLE) {
            vkDestroyPipeline(m_device, m_yuv420pUPipeline, nullptr);
            m_yuv420pUPipeline = VK_NULL_HANDLE;
        }
        if (m_yuv420pVPipeline != VK_NULL_HANDLE) {
            vkDestroyPipeline(m_device, m_yuv420pVPipeline, nullptr);
            m_yuv420pVPipeline = VK_NULL_HANDLE;
        }
        if (m_yuv420pComputePipeline != VK_NULL_HANDLE) {
            vkDestroyPipeline(m_device, m_yuv420pComputePipeline, nullptr);
            m_yuv420pComputePipeline = VK_NULL_HANDLE;
        }
        if (m_nv12YFramebuffer != VK_NULL_HANDLE) {
            vkDestroyFramebuffer(m_device, m_nv12YFramebuffer, nullptr);
            m_nv12YFramebuffer = VK_NULL_HANDLE;
        }
        if (m_nv12UvFramebuffer != VK_NULL_HANDLE) {
            vkDestroyFramebuffer(m_device, m_nv12UvFramebuffer, nullptr);
            m_nv12UvFramebuffer = VK_NULL_HANDLE;
        }
        if (m_yuv420pUFramebuffer != VK_NULL_HANDLE) {
            vkDestroyFramebuffer(m_device, m_yuv420pUFramebuffer, nullptr);
            m_yuv420pUFramebuffer = VK_NULL_HANDLE;
        }
        if (m_yuv420pVFramebuffer != VK_NULL_HANDLE) {
            vkDestroyFramebuffer(m_device, m_yuv420pVFramebuffer, nullptr);
            m_yuv420pVFramebuffer = VK_NULL_HANDLE;
        }
        if (m_nv12YRenderPass != VK_NULL_HANDLE) {
            vkDestroyRenderPass(m_device, m_nv12YRenderPass, nullptr);
            m_nv12YRenderPass = VK_NULL_HANDLE;
        }
        if (m_nv12UvRenderPass != VK_NULL_HANDLE) {
            vkDestroyRenderPass(m_device, m_nv12UvRenderPass, nullptr);
            m_nv12UvRenderPass = VK_NULL_HANDLE;
        }
        if (m_nv12YImageView != VK_NULL_HANDLE) {
            vkDestroyImageView(m_device, m_nv12YImageView, nullptr);
            m_nv12YImageView = VK_NULL_HANDLE;
        }
        if (m_nv12UvImageView != VK_NULL_HANDLE) {
            vkDestroyImageView(m_device, m_nv12UvImageView, nullptr);
            m_nv12UvImageView = VK_NULL_HANDLE;
        }
        if (m_yuv420pUImageView != VK_NULL_HANDLE) {
            vkDestroyImageView(m_device, m_yuv420pUImageView, nullptr);
            m_yuv420pUImageView = VK_NULL_HANDLE;
        }
        if (m_yuv420pVImageView != VK_NULL_HANDLE) {
            vkDestroyImageView(m_device, m_yuv420pVImageView, nullptr);
            m_yuv420pVImageView = VK_NULL_HANDLE;
        }
        if (m_nv12YImage != VK_NULL_HANDLE) {
            vkDestroyImage(m_device, m_nv12YImage, nullptr);
            m_nv12YImage = VK_NULL_HANDLE;
        }
        if (m_nv12UvImage != VK_NULL_HANDLE) {
            vkDestroyImage(m_device, m_nv12UvImage, nullptr);
            m_nv12UvImage = VK_NULL_HANDLE;
        }
        if (m_yuv420pUImage != VK_NULL_HANDLE) {
            vkDestroyImage(m_device, m_yuv420pUImage, nullptr);
            m_yuv420pUImage = VK_NULL_HANDLE;
        }
        if (m_yuv420pVImage != VK_NULL_HANDLE) {
            vkDestroyImage(m_device, m_yuv420pVImage, nullptr);
            m_yuv420pVImage = VK_NULL_HANDLE;
        }
        if (m_nv12YImageMemory != VK_NULL_HANDLE) {
            vkFreeMemory(m_device, m_nv12YImageMemory, nullptr);
            m_nv12YImageMemory = VK_NULL_HANDLE;
        }
        if (m_nv12UvImageMemory != VK_NULL_HANDLE) {
            vkFreeMemory(m_device, m_nv12UvImageMemory, nullptr);
            m_nv12UvImageMemory = VK_NULL_HANDLE;
        }
        if (m_yuv420pUImageMemory != VK_NULL_HANDLE) {
            vkFreeMemory(m_device, m_yuv420pUImageMemory, nullptr);
            m_yuv420pUImageMemory = VK_NULL_HANDLE;
        }
        if (m_yuv420pVImageMemory != VK_NULL_HANDLE) {
            vkFreeMemory(m_device, m_yuv420pVImageMemory, nullptr);
            m_yuv420pVImageMemory = VK_NULL_HANDLE;
        }
        if (m_effectsPipelineLayout != VK_NULL_HANDLE) {
            vkDestroyPipelineLayout(m_device, m_effectsPipelineLayout, nullptr);
            m_effectsPipelineLayout = VK_NULL_HANDLE;
        }
        if (m_maskPipelineLayout != VK_NULL_HANDLE) {
            vkDestroyPipelineLayout(m_device, m_maskPipelineLayout, nullptr);
            m_maskPipelineLayout = VK_NULL_HANDLE;
        }
        if (m_nv12PipelineLayout != VK_NULL_HANDLE) {
            vkDestroyPipelineLayout(m_device, m_nv12PipelineLayout, nullptr);
            m_nv12PipelineLayout = VK_NULL_HANDLE;
        }
        if (m_yuvComputePipelineLayout != VK_NULL_HANDLE) {
            vkDestroyPipelineLayout(m_device, m_yuvComputePipelineLayout, nullptr);
            m_yuvComputePipelineLayout = VK_NULL_HANDLE;
        }
        if (m_effectsVertModule != VK_NULL_HANDLE) {
            vkDestroyShaderModule(m_device, m_effectsVertModule, nullptr);
            m_effectsVertModule = VK_NULL_HANDLE;
        }
        if (m_effectsFragModule != VK_NULL_HANDLE) {
            vkDestroyShaderModule(m_device, m_effectsFragModule, nullptr);
            m_effectsFragModule = VK_NULL_HANDLE;
        }
        if (m_maskVertModule != VK_NULL_HANDLE) {
            vkDestroyShaderModule(m_device, m_maskVertModule, nullptr);
            m_maskVertModule = VK_NULL_HANDLE;
        }
        if (m_maskFragModule != VK_NULL_HANDLE) {
            vkDestroyShaderModule(m_device, m_maskFragModule, nullptr);
            m_maskFragModule = VK_NULL_HANDLE;
        }
        if (m_nv12VertModule != VK_NULL_HANDLE) {
            vkDestroyShaderModule(m_device, m_nv12VertModule, nullptr);
            m_nv12VertModule = VK_NULL_HANDLE;
        }
        if (m_nv12YFragModule != VK_NULL_HANDLE) {
            vkDestroyShaderModule(m_device, m_nv12YFragModule, nullptr);
            m_nv12YFragModule = VK_NULL_HANDLE;
        }
        if (m_nv12UvFragModule != VK_NULL_HANDLE) {
            vkDestroyShaderModule(m_device, m_nv12UvFragModule, nullptr);
            m_nv12UvFragModule = VK_NULL_HANDLE;
        }
        if (m_yuv420pUFragModule != VK_NULL_HANDLE) {
            vkDestroyShaderModule(m_device, m_yuv420pUFragModule, nullptr);
            m_yuv420pUFragModule = VK_NULL_HANDLE;
        }
        if (m_yuv420pVFragModule != VK_NULL_HANDLE) {
            vkDestroyShaderModule(m_device, m_yuv420pVFragModule, nullptr);
            m_yuv420pVFragModule = VK_NULL_HANDLE;
        }
        if (m_yuv420pComputeModule != VK_NULL_HANDLE) {
            vkDestroyShaderModule(m_device, m_yuv420pComputeModule, nullptr);
            m_yuv420pComputeModule = VK_NULL_HANDLE;
        }

        if (m_framebuffer != VK_NULL_HANDLE) {
            vkDestroyFramebuffer(m_device, m_framebuffer, nullptr);
            m_framebuffer = VK_NULL_HANDLE;
        }
        if (m_renderPass != VK_NULL_HANDLE) {
            vkDestroyRenderPass(m_device, m_renderPass, nullptr);
            m_renderPass = VK_NULL_HANDLE;
        }
        if (m_descriptorPool != VK_NULL_HANDLE) {
            vkDestroyDescriptorPool(m_device, m_descriptorPool, nullptr);
            m_descriptorPool = VK_NULL_HANDLE;
        }
        if (m_yuvComputeDescriptorPool != VK_NULL_HANDLE) {
            vkDestroyDescriptorPool(m_device, m_yuvComputeDescriptorPool, nullptr);
            m_yuvComputeDescriptorPool = VK_NULL_HANDLE;
        }
        if (m_descriptorSetLayout != VK_NULL_HANDLE) {
            vkDestroyDescriptorSetLayout(m_device, m_descriptorSetLayout, nullptr);
            m_descriptorSetLayout = VK_NULL_HANDLE;
        }
        if (m_yuvComputeDescriptorSetLayout != VK_NULL_HANDLE) {
            vkDestroyDescriptorSetLayout(m_device, m_yuvComputeDescriptorSetLayout, nullptr);
            m_yuvComputeDescriptorSetLayout = VK_NULL_HANDLE;
        }
        if (m_sampler != VK_NULL_HANDLE) {
            vkDestroySampler(m_device, m_sampler, nullptr);
            m_sampler = VK_NULL_HANDLE;
        }
        if (m_colorImageView != VK_NULL_HANDLE) {
            vkDestroyImageView(m_device, m_colorImageView, nullptr);
            m_colorImageView = VK_NULL_HANDLE;
        }
        if (m_colorImage != VK_NULL_HANDLE) {
            vkDestroyImage(m_device, m_colorImage, nullptr);
            m_colorImage = VK_NULL_HANDLE;
        }
        if (m_colorImageMemory != VK_NULL_HANDLE) {
            vkFreeMemory(m_device, m_colorImageMemory, nullptr);
            m_colorImageMemory = VK_NULL_HANDLE;
        }
        if (m_commandPool != VK_NULL_HANDLE) {
            vkDestroyCommandPool(m_device, m_commandPool, nullptr);
            m_commandPool = VK_NULL_HANDLE;
        }
        if (m_device != VK_NULL_HANDLE) {
            vkDestroyDevice(m_device, nullptr);
            m_device = VK_NULL_HANDLE;
        }
        if (m_instance != VK_NULL_HANDLE) {
            vkDestroyInstance(m_instance, nullptr);
            m_instance = VK_NULL_HANDLE;
        }

        m_physicalDevice = VK_NULL_HANDLE;
        m_graphicsQueue = VK_NULL_HANDLE;
        m_commandBuffer = VK_NULL_HANDLE;
        m_descriptorSet = VK_NULL_HANDLE;
        m_yuvComputeDescriptorSet = VK_NULL_HANDLE;
        m_graphicsQueueFamily = UINT32_MAX;
        m_initialized = false;
        m_yuv420pPlanesPrimed = false;
    }

    struct LayerInput {
        QImage image;
        QString cacheKey;
        float opacity = 1.0f;
        float mvp[16] = {
            1.f,0.f,0.f,0.f,
            0.f,1.f,0.f,0.f,
            0.f,0.f,1.f,0.f,
            0.f,0.f,0.f,1.f
        };
    };

    bool submitAndWait()
    {
        if (!submitActiveSlot()) {
            return false;
        }
        return waitSlot(m_activeSlotIndex);
    }

    bool submitActiveSlot()
    {
        if (m_activeSlotIndex < 0 || m_activeSlotIndex >= m_frameSlots.size() ||
            m_submitFence == VK_NULL_HANDLE) {
            return false;
        }
        FrameSlot& slot = m_frameSlots[m_activeSlotIndex];
        if (slot.commandBuffer == VK_NULL_HANDLE || slot.fence == VK_NULL_HANDLE) {
            return false;
        }
        vkResetFences(m_device, 1, &slot.fence);
        VkSubmitInfo submitInfo{};
        submitInfo.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
        submitInfo.commandBufferCount = 1;
        submitInfo.pCommandBuffers = &slot.commandBuffer;
        if (vkQueueSubmit(m_graphicsQueue, 1, &submitInfo, slot.fence) != VK_SUCCESS) {
            return false;
        }
        slot.inFlight = true;
        return true;
    }

    bool waitSlot(int slotIndex)
    {
        if (slotIndex < 0 || slotIndex >= m_frameSlots.size()) {
            return false;
        }
        FrameSlot& slot = m_frameSlots[slotIndex];
        if (slot.inFlight) {
            if (vkWaitForFences(m_device, 1, &slot.fence, VK_TRUE, UINT64_MAX) != VK_SUCCESS) {
                return false;
            }
            slot.inFlight = false;
        }
        return true;
    }

    bool invalidateSlotForHostRead(FrameSlot& slot)
    {
        if (slot.stagingHostCoherent || slot.stagingMemory == VK_NULL_HANDLE) {
            return true;
        }
        VkMappedMemoryRange range{};
        range.sType = VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE;
        range.memory = slot.stagingMemory;
        range.offset = 0;
        range.size = slot.stagingAllocationSize;
        return vkInvalidateMappedMemoryRanges(m_device, 1, &range) == VK_SUCCESS;
    }

    void useSlot(int slotIndex)
    {
        FrameSlot& slot = m_frameSlots[slotIndex];
        m_activeSlotIndex = slotIndex;
        m_commandBuffer = slot.commandBuffer;
        m_stagingBuffer = slot.stagingBuffer;
        m_stagingMemory = slot.stagingMemory;
        m_stagingMapped = slot.stagingMapped;
        m_submitFence = slot.fence;
    }

    bool selectNextSlot()
    {
        if (m_frameSlots.isEmpty()) {
            return false;
        }
        const int next = (m_activeSlotIndex + 1 + m_frameSlots.size()) % m_frameSlots.size();
        // Staging memory/fences are per-slot. Render targets are still queue-ordered shared images.
        if (!waitSlot(next)) {
            return false;
        }
        useSlot(next);
        return true;
    }

    QImage renderFrameFromLayers(const QVector<LayerInput>& layers, bool readbackToImage)
    {
        if (!m_initialized || m_device == VK_NULL_HANDLE || m_commandBuffer == VK_NULL_HANDLE) {
            return QImage();
        }
        if (!selectNextSlot()) {
            return QImage();
        }

        vkResetCommandBuffer(m_commandBuffer, 0);

        VkCommandBufferBeginInfo beginInfo{};
        beginInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
        if (vkBeginCommandBuffer(m_commandBuffer, &beginInfo) != VK_SUCCESS) {
            return QImage();
        }

        struct Push {
            float mvp[16];
            float opacity;
        } push{};
        transitionImageLayout(m_commandBuffer,
                              m_colorImage,
                              m_colorImagePrimed ? VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL
                                                 : VK_IMAGE_LAYOUT_UNDEFINED,
                              VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
        VkClearColorValue clearColor{};
        clearColor.float32[0] = 0.0f;
        clearColor.float32[1] = 0.0f;
        clearColor.float32[2] = 0.0f;
        clearColor.float32[3] = 1.0f;
        VkImageSubresourceRange clearRange{};
        clearRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        clearRange.baseMipLevel = 0;
        clearRange.levelCount = 1;
        clearRange.baseArrayLayer = 0;
        clearRange.layerCount = 1;
        vkCmdClearColorImage(m_commandBuffer,
                             m_colorImage,
                             VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                             &clearColor,
                             1,
                             &clearRange);
        transitionImageLayout(m_commandBuffer,
                              m_colorImage,
                              VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                              VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);

        VkClearValue clearValue{};
        clearValue.color = {{0.0f, 0.0f, 0.0f, 1.0f}};
        VkRenderPassBeginInfo renderPassBeginInfo{};
        renderPassBeginInfo.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
        renderPassBeginInfo.renderPass = m_renderPass;
        renderPassBeginInfo.framebuffer = m_framebuffer;
        renderPassBeginInfo.renderArea.offset = {0, 0};
        renderPassBeginInfo.renderArea.extent = {
            static_cast<uint32_t>(m_outputSize.width()),
            static_cast<uint32_t>(m_outputSize.height())
        };
        renderPassBeginInfo.clearValueCount = 1;
        renderPassBeginInfo.pClearValues = &clearValue;
        VkViewport fullViewport{};
        fullViewport.x = 0.0f;
        fullViewport.y = 0.0f;
        fullViewport.width = static_cast<float>(m_outputSize.width());
        fullViewport.height = static_cast<float>(m_outputSize.height());
        fullViewport.minDepth = 0.0f;
        fullViewport.maxDepth = 1.0f;
        VkRect2D fullScissor{};
        fullScissor.offset = {0, 0};
        fullScissor.extent = {static_cast<uint32_t>(m_outputSize.width()),
                              static_cast<uint32_t>(m_outputSize.height())};
        int layerIndex = 0;
        while (layerIndex < layers.size()) {
            const int batchCount = qMin(kMaxLayerTextures, layers.size() - layerIndex);
            for (int i = 0; i < batchCount; ++i) {
                const LayerInput& layer = layers.at(layerIndex + i);
                if (layer.image.isNull()) {
                    continue;
                }
                QImage rgba;
                if (!layer.cacheKey.isEmpty()) {
                    rgba = m_preparedImageCache.value(layer.cacheKey);
                }
                if (rgba.isNull()) {
                    rgba = layer.image;
                    if (rgba.format() != QImage::Format_RGBA8888) {
                        rgba = rgba.convertToFormat(QImage::Format_RGBA8888);
                    }
                    if (rgba.size() != m_outputSize) {
                        rgba = rgba.scaled(m_outputSize, Qt::IgnoreAspectRatio, Qt::SmoothTransformation);
                    }
                    if (!layer.cacheKey.isEmpty()) {
                        m_preparedImageCache.insert(layer.cacheKey, rgba);
                    }
                }
                if (!m_stagingMapped) {
                    return QImage();
                }
                const size_t bytes = static_cast<size_t>(rgba.sizeInBytes());
                std::memcpy(m_stagingMapped, rgba.constBits(), bytes);

                LayerTextureSlot& slot = m_layerSlots[i];
                transitionImageLayout(
                    m_commandBuffer,
                    slot.image,
                    slot.uploaded ? VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL : VK_IMAGE_LAYOUT_UNDEFINED,
                    VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
                VkBufferImageCopy uploadRegion{};
                uploadRegion.bufferOffset = 0;
                uploadRegion.bufferRowLength = 0;
                uploadRegion.bufferImageHeight = 0;
                uploadRegion.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
                uploadRegion.imageSubresource.mipLevel = 0;
                uploadRegion.imageSubresource.baseArrayLayer = 0;
                uploadRegion.imageSubresource.layerCount = 1;
                uploadRegion.imageExtent = {
                    static_cast<uint32_t>(m_outputSize.width()),
                    static_cast<uint32_t>(m_outputSize.height()),
                    1
                };
                vkCmdCopyBufferToImage(
                    m_commandBuffer,
                    m_stagingBuffer,
                    slot.image,
                    VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                    1,
                    &uploadRegion);
                transitionImageLayout(
                    m_commandBuffer,
                    slot.image,
                    VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                    VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
                slot.uploaded = true;
            }

            vkCmdBeginRenderPass(m_commandBuffer, &renderPassBeginInfo, VK_SUBPASS_CONTENTS_INLINE);
            vkCmdBindPipeline(m_commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS, m_effectsPipeline);
            vkCmdSetViewport(m_commandBuffer, 0, 1, &fullViewport);
            vkCmdSetScissor(m_commandBuffer, 0, 1, &fullScissor);
            for (int i = 0; i < batchCount; ++i) {
                const LayerInput& layer = layers.at(layerIndex + i);
                if (layer.image.isNull()) {
                    continue;
                }
                LayerTextureSlot& slot = m_layerSlots[i];
                vkCmdBindDescriptorSets(m_commandBuffer,
                                        VK_PIPELINE_BIND_POINT_GRAPHICS,
                                        m_effectsPipelineLayout,
                                        0,
                                        1,
                                        &slot.descriptorSet,
                                        0,
                                        nullptr);
                std::memcpy(push.mvp, layer.mvp, sizeof(push.mvp));
                push.opacity = qBound(0.0f, layer.opacity, 1.0f);
                vkCmdPushConstants(m_commandBuffer,
                                   m_effectsPipelineLayout,
                                   VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT,
                                   0,
                                   sizeof(Push),
                                   &push);
                vkCmdDraw(m_commandBuffer, 4, 1, 0, 0);
            }
            vkCmdEndRenderPass(m_commandBuffer);
            layerIndex += batchCount;
        }

        transitionImageLayout(m_commandBuffer,
                              m_colorImage,
                              VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                              VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
        if (readbackToImage) {
            VkBufferImageCopy readbackRegion{};
            readbackRegion.bufferOffset = 0;
            readbackRegion.bufferRowLength = 0;
            readbackRegion.bufferImageHeight = 0;
            readbackRegion.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
            readbackRegion.imageSubresource.mipLevel = 0;
            readbackRegion.imageSubresource.baseArrayLayer = 0;
            readbackRegion.imageSubresource.layerCount = 1;
            readbackRegion.imageExtent = {
                static_cast<uint32_t>(m_outputSize.width()),
                static_cast<uint32_t>(m_outputSize.height()),
                1
            };
            vkCmdCopyImageToBuffer(m_commandBuffer,
                                   m_colorImage,
                                   VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                                   m_stagingBuffer,
                                   1,
                                   &readbackRegion);
        }

        if (!readbackToImage) {
            m_commandBufferOpenForConversion = true;
            m_colorImagePrimed = true;
            return QImage();
        }

        if (vkEndCommandBuffer(m_commandBuffer) != VK_SUCCESS) {
            return QImage();
        }

        if (!submitAndWait()) {
            return QImage();
        }

        QImage out;
        if (readbackToImage) {
            if (!m_stagingMapped) {
                return QImage();
            }
            if (m_activeSlotIndex < 0 ||
                m_activeSlotIndex >= m_frameSlots.size() ||
                !invalidateSlotForHostRead(m_frameSlots[m_activeSlotIndex])) {
                return QImage();
            }
            QImage readbackRgba(reinterpret_cast<const uchar*>(m_stagingMapped),
                               m_outputSize.width(),
                               m_outputSize.height(),
                               m_outputSize.width() * 4,
                               QImage::Format_ARGB32);
            out = readbackRgba.copy().convertToFormat(QImage::Format_ARGB32_Premultiplied);
        }
        m_colorImagePrimed = true;
        return out;
    }

    bool convertLastFrameToNv12(AVFrame* frame, qint64* nv12ConvertMs, qint64* readbackMs)
    {
        return beginLastFrameToNv12Readback(nv12ConvertMs, readbackMs) &&
               finishLastFrameToNv12Readback(frame, nv12ConvertMs, readbackMs);
    }

    bool beginLastFrameToNv12Copy(VkBuffer targetBuffer,
                                  QVector<int>* pendingSlots,
                                  qint64* convertMs,
                                  qint64* transferMs)
    {
        if (!m_initialized || m_device == VK_NULL_HANDLE || m_commandBuffer == VK_NULL_HANDLE ||
            targetBuffer == VK_NULL_HANDLE || !pendingSlots) {
            return false;
        }
        const qint64 startMs = QDateTime::currentMSecsSinceEpoch();
        VkCommandBufferBeginInfo beginInfo{};
        beginInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
        if (!m_commandBufferOpenForConversion) {
            vkResetCommandBuffer(m_commandBuffer, 0);
            if (vkBeginCommandBuffer(m_commandBuffer, &beginInfo) != VK_SUCCESS) {
                return false;
            }
        }
        transitionImageLayout(m_commandBuffer,
                              m_colorImage,
                              VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                              VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
        VkDescriptorImageInfo srcImageDesc{};
        srcImageDesc.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        srcImageDesc.imageView = m_colorImageView;
        srcImageDesc.sampler = m_sampler;
        VkWriteDescriptorSet srcWrite{};
        srcWrite.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        srcWrite.dstSet = m_descriptorSet;
        srcWrite.dstBinding = 0;
        srcWrite.descriptorCount = 1;
        srcWrite.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        srcWrite.pImageInfo = &srcImageDesc;
        vkUpdateDescriptorSets(m_device, 1, &srcWrite, 0, nullptr);

        VkViewport yViewport{};
        yViewport.x = 0.0f;
        yViewport.y = 0.0f;
        yViewport.width = static_cast<float>(m_outputSize.width());
        yViewport.height = static_cast<float>(m_outputSize.height());
        yViewport.minDepth = 0.0f;
        yViewport.maxDepth = 1.0f;
        VkRect2D yScissor{};
        yScissor.offset = {0, 0};
        yScissor.extent = {
            static_cast<uint32_t>(m_outputSize.width()),
            static_cast<uint32_t>(m_outputSize.height())
        };
        VkClearValue clearY{};
        clearY.color = {{0.0625f, 0.0f, 0.0f, 1.0f}};
        VkRenderPassBeginInfo yPass{};
        yPass.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
        yPass.renderPass = m_nv12YRenderPass;
        yPass.framebuffer = m_nv12YFramebuffer;
        yPass.renderArea.offset = {0, 0};
        yPass.renderArea.extent = yScissor.extent;
        yPass.clearValueCount = 1;
        yPass.pClearValues = &clearY;
        vkCmdBeginRenderPass(m_commandBuffer, &yPass, VK_SUBPASS_CONTENTS_INLINE);
        vkCmdBindPipeline(m_commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS, m_nv12YPipeline);
        vkCmdSetViewport(m_commandBuffer, 0, 1, &yViewport);
        vkCmdSetScissor(m_commandBuffer, 0, 1, &yScissor);
        vkCmdBindDescriptorSets(m_commandBuffer,
                                VK_PIPELINE_BIND_POINT_GRAPHICS,
                                m_nv12PipelineLayout,
                                0,
                                1,
                                &m_descriptorSet,
                                0,
                                nullptr);
        vkCmdDraw(m_commandBuffer, 4, 1, 0, 0);
        vkCmdEndRenderPass(m_commandBuffer);

        VkViewport uvViewport{};
        uvViewport.x = 0.0f;
        uvViewport.y = 0.0f;
        uvViewport.width = static_cast<float>(qMax(1, m_outputSize.width() / 2));
        uvViewport.height = static_cast<float>(qMax(1, m_outputSize.height() / 2));
        uvViewport.minDepth = 0.0f;
        uvViewport.maxDepth = 1.0f;
        VkRect2D uvScissor{};
        uvScissor.offset = {0, 0};
        uvScissor.extent = {
            static_cast<uint32_t>(qMax(1, m_outputSize.width() / 2)),
            static_cast<uint32_t>(qMax(1, m_outputSize.height() / 2))
        };
        VkClearValue clearUv{};
        clearUv.color = {{0.5f, 0.5f, 0.0f, 1.0f}};
        VkRenderPassBeginInfo uvPass{};
        uvPass.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
        uvPass.renderPass = m_nv12UvRenderPass;
        uvPass.framebuffer = m_nv12UvFramebuffer;
        uvPass.renderArea.offset = {0, 0};
        uvPass.renderArea.extent = uvScissor.extent;
        uvPass.clearValueCount = 1;
        uvPass.pClearValues = &clearUv;
        vkCmdBeginRenderPass(m_commandBuffer, &uvPass, VK_SUBPASS_CONTENTS_INLINE);
        vkCmdBindPipeline(m_commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS, m_nv12UvPipeline);
        vkCmdSetViewport(m_commandBuffer, 0, 1, &uvViewport);
        vkCmdSetScissor(m_commandBuffer, 0, 1, &uvScissor);
        vkCmdBindDescriptorSets(m_commandBuffer,
                                VK_PIPELINE_BIND_POINT_GRAPHICS,
                                m_nv12PipelineLayout,
                                0,
                                1,
                                &m_descriptorSet,
                                0,
                                nullptr);
        vkCmdDraw(m_commandBuffer, 4, 1, 0, 0);
        vkCmdEndRenderPass(m_commandBuffer);

        const VkDeviceSize yPlaneBytes =
            static_cast<VkDeviceSize>(m_outputSize.width()) *
            static_cast<VkDeviceSize>(m_outputSize.height());
        const VkDeviceSize uvPlaneOffset = (yPlaneBytes + 255u) & ~VkDeviceSize(255u);
        VkBufferImageCopy yRegion{};
        yRegion.bufferOffset = 0;
        yRegion.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        yRegion.imageSubresource.mipLevel = 0;
        yRegion.imageSubresource.baseArrayLayer = 0;
        yRegion.imageSubresource.layerCount = 1;
        yRegion.imageExtent = {
            static_cast<uint32_t>(m_outputSize.width()),
            static_cast<uint32_t>(m_outputSize.height()),
            1
        };
        vkCmdCopyImageToBuffer(m_commandBuffer,
                               m_nv12YImage,
                               VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                               targetBuffer,
                               1,
                               &yRegion);

        VkBufferImageCopy uvRegion{};
        uvRegion.bufferOffset = uvPlaneOffset;
        uvRegion.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        uvRegion.imageSubresource.mipLevel = 0;
        uvRegion.imageSubresource.baseArrayLayer = 0;
        uvRegion.imageSubresource.layerCount = 1;
        uvRegion.imageExtent = {
            static_cast<uint32_t>(qMax(1, m_outputSize.width() / 2)),
            static_cast<uint32_t>(qMax(1, m_outputSize.height() / 2)),
            1
        };
        vkCmdCopyImageToBuffer(m_commandBuffer,
                               m_nv12UvImage,
                               VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                               targetBuffer,
                               1,
                               &uvRegion);
        transitionImageLayout(m_commandBuffer,
                              m_colorImage,
                              VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                              VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
        if (vkEndCommandBuffer(m_commandBuffer) != VK_SUCCESS) {
            return false;
        }
        if (!submitActiveSlot()) {
            return false;
        }
        m_commandBufferOpenForConversion = false;
        pendingSlots->push_back(m_activeSlotIndex);
        if (convertMs) {
            *convertMs += QDateTime::currentMSecsSinceEpoch() - startMs;
        }
        Q_UNUSED(transferMs)
        return true;
    }

    bool beginLastFrameToNv12Readback(qint64* convertMs, qint64* readbackMs)
    {
        return beginLastFrameToNv12Copy(m_stagingBuffer,
                                        &m_pendingNv12SlotIndices,
                                        convertMs,
                                        readbackMs);
    }

    bool beginLastFrameToNv12CudaTransfer(qint64* convertMs, qint64* transferMs)
    {
        if (!supportsCudaExternalMemoryInterop() ||
            m_activeSlotIndex < 0 ||
            m_activeSlotIndex >= m_frameSlots.size()) {
            return false;
        }
        FrameSlot& slot = m_frameSlots[m_activeSlotIndex];
        return beginLastFrameToNv12Copy(slot.cudaExportBuffer,
                                        &m_pendingNv12CudaSlotIndices,
                                        convertMs,
                                        transferMs);
    }

    bool finishLastFrameToNv12Readback(AVFrame* frame, qint64* convertMs, qint64* readbackMs)
    {
        if (!frame || frame->format != AV_PIX_FMT_NV12 || frame->width <= 0 || frame->height <= 0 ||
            m_pendingNv12SlotIndices.isEmpty()) {
            return false;
        }
        const qint64 startMs = QDateTime::currentMSecsSinceEpoch();
        const int slotIndex = m_pendingNv12SlotIndices.takeFirst();
        if (slotIndex < 0 || slotIndex >= m_frameSlots.size()) {
            return false;
        }
        if (!waitSlot(slotIndex)) {
            return false;
        }
        FrameSlot& slot = m_frameSlots[slotIndex];
        if (!slot.stagingMapped || !invalidateSlotForHostRead(slot)) {
            return false;
        }
        const uint8_t* bytes = reinterpret_cast<const uint8_t*>(slot.stagingMapped);
        const VkDeviceSize yPlaneBytes =
            static_cast<VkDeviceSize>(m_outputSize.width()) *
            static_cast<VkDeviceSize>(m_outputSize.height());
        const VkDeviceSize uvPlaneOffset = (yPlaneBytes + 255u) & ~VkDeviceSize(255u);
        for (int y = 0; y < frame->height; ++y) {
            memcpy(frame->data[0] + y * frame->linesize[0],
                   bytes + y * m_outputSize.width(),
                   frame->width);
        }
        const int uvWidthBytes = qMax(1, frame->width / 2) * 2;
        const uint8_t* uvMapped = bytes + uvPlaneOffset;
        for (int y = 0; y < qMax(1, frame->height / 2); ++y) {
            memcpy(frame->data[1] + y * frame->linesize[1],
                   uvMapped + y * uvWidthBytes,
                   uvWidthBytes);
        }
        if (convertMs) {
            *convertMs += QDateTime::currentMSecsSinceEpoch() - startMs;
        }
        if (readbackMs) {
            *readbackMs += QDateTime::currentMSecsSinceEpoch() - startMs;
        }
        return true;
    }

#if JCUT_HAS_CUDA_DRIVER
    bool ensureCudaExternalMemoryForSlot(FrameSlot& slot, AVFrame* cudaFrame)
    {
        if (!cudaFrame ||
            cudaFrame->format != AV_PIX_FMT_CUDA ||
            !cudaFrame->hw_frames_ctx ||
            slot.cudaExportMemory == VK_NULL_HANDLE ||
            slot.cudaExportAllocationSize == 0 ||
            !m_vkGetMemoryFdKHR) {
            return false;
        }
        auto* framesCtx = reinterpret_cast<AVHWFramesContext*>(cudaFrame->hw_frames_ctx->data);
        if (!framesCtx || !framesCtx->device_ctx || !framesCtx->device_ctx->hwctx) {
            return false;
        }
        auto* cudaDevice = reinterpret_cast<AVCUDADeviceContext*>(framesCtx->device_ctx->hwctx);
        CUcontext cudaContext = cudaDevice->cuda_ctx;
        if (!cudaContext) {
            return false;
        }

        CUcontext previous = nullptr;
        CUresult cuResult = cuInit(0);
        if (cuResult != CUDA_SUCCESS ||
            cuCtxPushCurrent(cudaContext) != CUDA_SUCCESS) {
            return false;
        }

        auto popContext = qScopeGuard([&]() {
            cuCtxPopCurrent(&previous);
        });

        if (slot.cudaExternalMemory && slot.cudaImportContext != cudaContext) {
            cuDestroyExternalMemory(slot.cudaExternalMemory);
            slot.cudaExternalMemory = nullptr;
            slot.cudaExternalDevicePtr = 0;
            slot.cudaImportContext = nullptr;
        }
        if (slot.cudaExternalMemory && slot.cudaExternalDevicePtr) {
            return true;
        }

        VkMemoryGetFdInfoKHR fdInfo{};
        fdInfo.sType = VK_STRUCTURE_TYPE_MEMORY_GET_FD_INFO_KHR;
        fdInfo.memory = slot.cudaExportMemory;
        fdInfo.handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_FD_BIT;
        int memoryFd = -1;
        if (m_vkGetMemoryFdKHR(m_device, &fdInfo, &memoryFd) != VK_SUCCESS || memoryFd < 0) {
            return false;
        }

        CUDA_EXTERNAL_MEMORY_HANDLE_DESC handleDesc{};
        handleDesc.type = CU_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_FD;
        handleDesc.handle.fd = memoryFd;
        handleDesc.size = slot.cudaExportAllocationSize;
        cuResult = cuImportExternalMemory(&slot.cudaExternalMemory, &handleDesc);
        if (cuResult != CUDA_SUCCESS) {
            close(memoryFd);
            slot.cudaExternalMemory = nullptr;
            return false;
        }

        CUDA_EXTERNAL_MEMORY_BUFFER_DESC bufferDesc{};
        bufferDesc.offset = 0;
        bufferDesc.size = slot.cudaExportAllocationSize;
        cuResult = cuExternalMemoryGetMappedBuffer(&slot.cudaExternalDevicePtr,
                                                   slot.cudaExternalMemory,
                                                   &bufferDesc);
        if (cuResult != CUDA_SUCCESS) {
            cuDestroyExternalMemory(slot.cudaExternalMemory);
            slot.cudaExternalMemory = nullptr;
            slot.cudaExternalDevicePtr = 0;
            return false;
        }
        slot.cudaImportContext = cudaContext;
        return true;
    }
#endif

    bool finishLastFrameToNv12CudaTransfer(AVFrame* cudaFrame, qint64* convertMs, qint64* transferMs)
    {
#if JCUT_HAS_CUDA_DRIVER
        if (!cudaFrame ||
            cudaFrame->format != AV_PIX_FMT_CUDA ||
            m_pendingNv12CudaSlotIndices.isEmpty()) {
            return false;
        }
        const qint64 startMs = QDateTime::currentMSecsSinceEpoch();
        const int slotIndex = m_pendingNv12CudaSlotIndices.takeFirst();
        if (slotIndex < 0 || slotIndex >= m_frameSlots.size()) {
            return false;
        }
        if (!waitSlot(slotIndex)) {
            return false;
        }
        FrameSlot& slot = m_frameSlots[slotIndex];
        if (!ensureCudaExternalMemoryForSlot(slot, cudaFrame)) {
            return false;
        }

        auto* framesCtx = reinterpret_cast<AVHWFramesContext*>(cudaFrame->hw_frames_ctx->data);
        auto* cudaDevice = reinterpret_cast<AVCUDADeviceContext*>(framesCtx->device_ctx->hwctx);
        CUcontext cudaContext = cudaDevice->cuda_ctx;
        CUcontext previous = nullptr;
        if (cuCtxPushCurrent(cudaContext) != CUDA_SUCCESS) {
            return false;
        }
        auto popContext = qScopeGuard([&]() {
            cuCtxPopCurrent(&previous);
        });

        const int width = qMin(cudaFrame->width, m_outputSize.width());
        const int height = qMin(cudaFrame->height, m_outputSize.height());
        const VkDeviceSize yPlaneBytes =
            static_cast<VkDeviceSize>(m_outputSize.width()) *
            static_cast<VkDeviceSize>(m_outputSize.height());
        const VkDeviceSize uvPlaneOffset = (yPlaneBytes + 255u) & ~VkDeviceSize(255u);

        CUDA_MEMCPY2D yCopy{};
        yCopy.srcMemoryType = CU_MEMORYTYPE_DEVICE;
        yCopy.srcDevice = slot.cudaExternalDevicePtr;
        yCopy.srcPitch = static_cast<size_t>(m_outputSize.width());
        yCopy.dstMemoryType = CU_MEMORYTYPE_DEVICE;
        yCopy.dstDevice = reinterpret_cast<CUdeviceptr>(cudaFrame->data[0]);
        yCopy.dstPitch = static_cast<size_t>(cudaFrame->linesize[0]);
        yCopy.WidthInBytes = static_cast<size_t>(width);
        yCopy.Height = static_cast<size_t>(height);
        if (cuMemcpy2DAsync(&yCopy, cudaDevice->stream) != CUDA_SUCCESS) {
            return false;
        }

        CUDA_MEMCPY2D uvCopy{};
        uvCopy.srcMemoryType = CU_MEMORYTYPE_DEVICE;
        uvCopy.srcDevice = slot.cudaExternalDevicePtr + uvPlaneOffset;
        uvCopy.srcPitch = static_cast<size_t>(m_outputSize.width());
        uvCopy.dstMemoryType = CU_MEMORYTYPE_DEVICE;
        uvCopy.dstDevice = reinterpret_cast<CUdeviceptr>(cudaFrame->data[1]);
        uvCopy.dstPitch = static_cast<size_t>(cudaFrame->linesize[1]);
        uvCopy.WidthInBytes = static_cast<size_t>(width);
        uvCopy.Height = static_cast<size_t>(qMax(1, height / 2));
        if (cuMemcpy2DAsync(&uvCopy, cudaDevice->stream) != CUDA_SUCCESS ||
            cuStreamSynchronize(cudaDevice->stream) != CUDA_SUCCESS) {
            return false;
        }
        const qint64 elapsed = QDateTime::currentMSecsSinceEpoch() - startMs;
        if (convertMs) {
            *convertMs += elapsed;
        }
        if (transferMs) {
            *transferMs += elapsed;
        }
        return true;
#else
        Q_UNUSED(cudaFrame)
        Q_UNUSED(convertMs)
        Q_UNUSED(transferMs)
        return false;
#endif
    }

    bool beginLastFrameToYuv420pReadback(qint64* convertMs, qint64* readbackMs)
    {
        if (!m_initialized || m_device == VK_NULL_HANDLE || m_commandBuffer == VK_NULL_HANDLE) {
            return false;
        }
        const qint64 startMs = QDateTime::currentMSecsSinceEpoch();
        VkCommandBufferBeginInfo beginInfo{};
        beginInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
        if (!m_commandBufferOpenForConversion) {
            vkResetCommandBuffer(m_commandBuffer, 0);
            if (vkBeginCommandBuffer(m_commandBuffer, &beginInfo) != VK_SUCCESS) {
                return false;
            }
        }

        transitionImageLayout(m_commandBuffer,
                              m_colorImage,
                              VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                              VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
        VkDescriptorImageInfo srcImageDesc{};
        srcImageDesc.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        srcImageDesc.imageView = m_colorImageView;
        srcImageDesc.sampler = m_sampler;
        VkWriteDescriptorSet srcWrite{};
        srcWrite.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        srcWrite.dstSet = m_descriptorSet;
        srcWrite.dstBinding = 0;
        srcWrite.descriptorCount = 1;
        srcWrite.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        srcWrite.pImageInfo = &srcImageDesc;
        vkUpdateDescriptorSets(m_device, 1, &srcWrite, 0, nullptr);

        const uint32_t yWidth = static_cast<uint32_t>(m_outputSize.width());
        const uint32_t yHeight = static_cast<uint32_t>(m_outputSize.height());
        const uint32_t chromaWidth = static_cast<uint32_t>(qMax(1, m_outputSize.width() / 2));
        const uint32_t chromaHeight = static_cast<uint32_t>(qMax(1, m_outputSize.height() / 2));
        const VkImageLayout oldYuvLayout =
            m_yuv420pPlanesPrimed ? VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL : VK_IMAGE_LAYOUT_UNDEFINED;
        transitionImageLayout(m_commandBuffer, m_nv12YImage, oldYuvLayout, VK_IMAGE_LAYOUT_GENERAL);
        transitionImageLayout(m_commandBuffer, m_yuv420pUImage, oldYuvLayout, VK_IMAGE_LAYOUT_GENERAL);
        transitionImageLayout(m_commandBuffer, m_yuv420pVImage, oldYuvLayout, VK_IMAGE_LAYOUT_GENERAL);
        vkCmdBindPipeline(m_commandBuffer, VK_PIPELINE_BIND_POINT_COMPUTE, m_yuv420pComputePipeline);
        vkCmdBindDescriptorSets(m_commandBuffer,
                                VK_PIPELINE_BIND_POINT_COMPUTE,
                                m_yuvComputePipelineLayout,
                                0,
                                1,
                                &m_yuvComputeDescriptorSet,
                                0,
                                nullptr);
        vkCmdDispatch(m_commandBuffer, (yWidth + 15u) / 16u, (yHeight + 15u) / 16u, 1);
        transitionImageLayout(m_commandBuffer, m_nv12YImage, VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
        transitionImageLayout(m_commandBuffer, m_yuv420pUImage, VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
        transitionImageLayout(m_commandBuffer, m_yuv420pVImage, VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);

        const VkDeviceSize yPlaneBytes =
            static_cast<VkDeviceSize>(m_outputSize.width()) *
            static_cast<VkDeviceSize>(m_outputSize.height());
        const VkDeviceSize uPlaneBytes =
            static_cast<VkDeviceSize>(chromaWidth) *
            static_cast<VkDeviceSize>(chromaHeight);
        const VkDeviceSize uPlaneOffset = (yPlaneBytes + 255u) & ~VkDeviceSize(255u);
        const VkDeviceSize vPlaneOffset = (uPlaneOffset + uPlaneBytes + 255u) & ~VkDeviceSize(255u);
        VkBufferImageCopy yRegion{};
        yRegion.bufferOffset = 0;
        yRegion.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        yRegion.imageSubresource.mipLevel = 0;
        yRegion.imageSubresource.baseArrayLayer = 0;
        yRegion.imageSubresource.layerCount = 1;
        yRegion.imageExtent = {yWidth, yHeight, 1};
        vkCmdCopyImageToBuffer(m_commandBuffer,
                               m_nv12YImage,
                               VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                               m_stagingBuffer,
                               1,
                               &yRegion);

        VkBufferImageCopy uRegion{};
        uRegion.bufferOffset = uPlaneOffset;
        uRegion.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        uRegion.imageSubresource.mipLevel = 0;
        uRegion.imageSubresource.baseArrayLayer = 0;
        uRegion.imageSubresource.layerCount = 1;
        uRegion.imageExtent = {chromaWidth, chromaHeight, 1};
        vkCmdCopyImageToBuffer(m_commandBuffer,
                               m_yuv420pUImage,
                               VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                               m_stagingBuffer,
                               1,
                               &uRegion);

        VkBufferImageCopy vRegion = uRegion;
        vRegion.bufferOffset = vPlaneOffset;
        vkCmdCopyImageToBuffer(m_commandBuffer,
                               m_yuv420pVImage,
                               VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                               m_stagingBuffer,
                               1,
                               &vRegion);
        transitionImageLayout(m_commandBuffer,
                              m_colorImage,
                              VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                              VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
        if (vkEndCommandBuffer(m_commandBuffer) != VK_SUCCESS) {
            return false;
        }
        if (!submitActiveSlot()) {
            return false;
        }
        m_commandBufferOpenForConversion = false;
        m_yuv420pPlanesPrimed = true;
        m_pendingYuvSlotIndices.push_back(m_activeSlotIndex);
        if (convertMs) {
            *convertMs += QDateTime::currentMSecsSinceEpoch() - startMs;
        }
        Q_UNUSED(readbackMs)
        return true;
    }

    bool finishLastFrameToYuv420pReadback(AVFrame* frame, qint64* convertMs, qint64* readbackMs)
    {
        if (!frame || frame->format != AV_PIX_FMT_YUV420P || frame->width <= 0 || frame->height <= 0 ||
            m_pendingYuvSlotIndices.isEmpty()) {
            return false;
        }
        const qint64 startMs = QDateTime::currentMSecsSinceEpoch();
        const int slotIndex = m_pendingYuvSlotIndices.takeFirst();
        if (slotIndex < 0 || slotIndex >= m_frameSlots.size()) {
            return false;
        }
        if (!waitSlot(slotIndex)) {
            return false;
        }
        FrameSlot& slot = m_frameSlots[slotIndex];
        if (!slot.stagingMapped) {
            return false;
        }
        if (!invalidateSlotForHostRead(slot)) {
            return false;
        }
        const uint8_t* bytes = reinterpret_cast<const uint8_t*>(slot.stagingMapped);
        const int chromaWidth = qMax(1, m_outputSize.width() / 2);
        const int chromaHeight = qMax(1, m_outputSize.height() / 2);
        const VkDeviceSize yPlaneBytes =
            static_cast<VkDeviceSize>(m_outputSize.width()) *
            static_cast<VkDeviceSize>(m_outputSize.height());
        const VkDeviceSize uPlaneBytes =
            static_cast<VkDeviceSize>(chromaWidth) *
            static_cast<VkDeviceSize>(chromaHeight);
        const VkDeviceSize uPlaneOffset = (yPlaneBytes + 255u) & ~VkDeviceSize(255u);
        const VkDeviceSize vPlaneOffset = (uPlaneOffset + uPlaneBytes + 255u) & ~VkDeviceSize(255u);
        for (int y = 0; y < frame->height; ++y) {
            memcpy(frame->data[0] + y * frame->linesize[0],
                   bytes + y * m_outputSize.width(),
                   frame->width);
        }
        const int frameChromaWidth = qMax(1, frame->width / 2);
        const int frameChromaHeight = qMax(1, frame->height / 2);
        for (int y = 0; y < frameChromaHeight; ++y) {
            memcpy(frame->data[1] + y * frame->linesize[1],
                   bytes + uPlaneOffset + y * chromaWidth,
                   frameChromaWidth);
            memcpy(frame->data[2] + y * frame->linesize[2],
                   bytes + vPlaneOffset + y * chromaWidth,
                   frameChromaWidth);
        }
        if (convertMs) {
            *convertMs += QDateTime::currentMSecsSinceEpoch() - startMs;
        }
        if (readbackMs) {
            *readbackMs += QDateTime::currentMSecsSinceEpoch() - startMs;
        }
        return true;
    }

    bool convertLastFrameToYuv420p(AVFrame* frame, qint64* convertMs, qint64* readbackMs)
    {
        return beginLastFrameToYuv420pReadback(convertMs, readbackMs) &&
               finishLastFrameToYuv420pReadback(frame, convertMs, readbackMs);
    }

    bool copyLastFrameToBgra(AVFrame* frame, qint64* readbackMs)
    {
        if (!frame || frame->width <= 0 || frame->height <= 0 ||
            !m_initialized || m_device == VK_NULL_HANDLE || m_commandBuffer == VK_NULL_HANDLE) {
            return false;
        }
        const qint64 startMs = QDateTime::currentMSecsSinceEpoch();
        VkCommandBufferBeginInfo beginInfo{};
        beginInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
        if (!m_commandBufferOpenForConversion) {
            vkResetCommandBuffer(m_commandBuffer, 0);
            if (vkBeginCommandBuffer(m_commandBuffer, &beginInfo) != VK_SUCCESS) {
                return false;
            }
        }
        VkBufferImageCopy region{};
        region.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        region.imageSubresource.mipLevel = 0;
        region.imageSubresource.baseArrayLayer = 0;
        region.imageSubresource.layerCount = 1;
        region.imageExtent = {
            static_cast<uint32_t>(m_outputSize.width()),
            static_cast<uint32_t>(m_outputSize.height()),
            1
        };
        vkCmdCopyImageToBuffer(m_commandBuffer,
                               m_colorImage,
                               VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                               m_stagingBuffer,
                               1,
                               &region);
        if (vkEndCommandBuffer(m_commandBuffer) != VK_SUCCESS) {
            return false;
        }
        if (!submitAndWait()) {
            return false;
        }
        m_commandBufferOpenForConversion = false;

        if (!m_stagingMapped) {
            return false;
        }
        if (m_activeSlotIndex < 0 ||
            m_activeSlotIndex >= m_frameSlots.size() ||
            !invalidateSlotForHostRead(m_frameSlots[m_activeSlotIndex])) {
            return false;
        }
        const int width = qMin(frame->width, m_outputSize.width());
        const int height = qMin(frame->height, m_outputSize.height());
        const int srcStride = m_outputSize.width() * 4;
        for (int y = 0; y < height; ++y) {
            memcpy(frame->data[0] + y * frame->linesize[0],
                   reinterpret_cast<uint8_t*>(m_stagingMapped) + y * srcStride,
                   static_cast<size_t>(width) * 4);
        }
        if (readbackMs) {
            *readbackMs += QDateTime::currentMSecsSinceEpoch() - startMs;
        }
        return true;
    }

    bool supportsCudaExternalMemoryInterop() const
    {
        return m_externalMemoryFdSupported &&
               m_externalSemaphoreFdSupported &&
               m_vkGetMemoryFdKHR != nullptr &&
               m_cudaExportBuffersReady;
    }

private:
    QSize m_outputSize;
    bool m_initialized = false;

    VkInstance m_instance = VK_NULL_HANDLE;
    VkPhysicalDevice m_physicalDevice = VK_NULL_HANDLE;
    VkDevice m_device = VK_NULL_HANDLE;
    uint32_t m_graphicsQueueFamily = UINT32_MAX;
    VkQueue m_graphicsQueue = VK_NULL_HANDLE;
    bool m_externalMemoryFdSupported = false;
    bool m_externalSemaphoreFdSupported = false;
    PFN_vkGetMemoryFdKHR m_vkGetMemoryFdKHR = nullptr;

    VkCommandPool m_commandPool = VK_NULL_HANDLE;
    VkCommandBuffer m_commandBuffer = VK_NULL_HANDLE;
    VkFence m_submitFence = VK_NULL_HANDLE;
    QVector<FrameSlot> m_frameSlots;
    int m_activeSlotIndex = -1;
    QVector<int> m_pendingNv12SlotIndices;
    QVector<int> m_pendingNv12CudaSlotIndices;
    QVector<int> m_pendingYuvSlotIndices;
    bool m_cudaExportBuffersReady = false;

    VkImage m_colorImage = VK_NULL_HANDLE;
    VkDeviceMemory m_colorImageMemory = VK_NULL_HANDLE;
    VkImageView m_colorImageView = VK_NULL_HANDLE;
    VkSampler m_sampler = VK_NULL_HANDLE;

    VkDescriptorSetLayout m_descriptorSetLayout = VK_NULL_HANDLE;
    VkDescriptorPool m_descriptorPool = VK_NULL_HANDLE;
    VkDescriptorSet m_descriptorSet = VK_NULL_HANDLE;
    VkDescriptorSetLayout m_yuvComputeDescriptorSetLayout = VK_NULL_HANDLE;
    VkDescriptorPool m_yuvComputeDescriptorPool = VK_NULL_HANDLE;
    VkDescriptorSet m_yuvComputeDescriptorSet = VK_NULL_HANDLE;

    VkRenderPass m_renderPass = VK_NULL_HANDLE;
    VkFramebuffer m_framebuffer = VK_NULL_HANDLE;
    struct LayerTextureSlot {
        VkImage image = VK_NULL_HANDLE;
        VkDeviceMemory memory = VK_NULL_HANDLE;
        VkImageView view = VK_NULL_HANDLE;
        VkDescriptorSet descriptorSet = VK_NULL_HANDLE;
        bool uploaded = false;
    };
    QVector<LayerTextureSlot> m_layerSlots;
    QHash<QString, QImage> m_preparedImageCache;
    VkBuffer m_stagingBuffer = VK_NULL_HANDLE;
    VkDeviceMemory m_stagingMemory = VK_NULL_HANDLE;
    void* m_stagingMapped = nullptr;
    VkPipelineLayout m_effectsPipelineLayout = VK_NULL_HANDLE;
    VkPipelineLayout m_maskPipelineLayout = VK_NULL_HANDLE;
    VkPipelineLayout m_nv12PipelineLayout = VK_NULL_HANDLE;
    VkPipelineLayout m_yuvComputePipelineLayout = VK_NULL_HANDLE;
    VkShaderModule m_effectsVertModule = VK_NULL_HANDLE;
    VkShaderModule m_effectsFragModule = VK_NULL_HANDLE;
    VkShaderModule m_maskVertModule = VK_NULL_HANDLE;
    VkShaderModule m_maskFragModule = VK_NULL_HANDLE;
    VkShaderModule m_nv12VertModule = VK_NULL_HANDLE;
    VkShaderModule m_nv12YFragModule = VK_NULL_HANDLE;
    VkShaderModule m_nv12UvFragModule = VK_NULL_HANDLE;
    VkShaderModule m_yuv420pUFragModule = VK_NULL_HANDLE;
    VkShaderModule m_yuv420pVFragModule = VK_NULL_HANDLE;
    VkShaderModule m_yuv420pComputeModule = VK_NULL_HANDLE;
    VkPipeline m_effectsPipeline = VK_NULL_HANDLE;
    VkPipeline m_maskPipeline = VK_NULL_HANDLE;
    VkPipeline m_nv12YPipeline = VK_NULL_HANDLE;
    VkPipeline m_nv12UvPipeline = VK_NULL_HANDLE;
    VkPipeline m_yuv420pUPipeline = VK_NULL_HANDLE;
    VkPipeline m_yuv420pVPipeline = VK_NULL_HANDLE;
    VkPipeline m_yuv420pComputePipeline = VK_NULL_HANDLE;
    VkImage m_nv12YImage = VK_NULL_HANDLE;
    VkDeviceMemory m_nv12YImageMemory = VK_NULL_HANDLE;
    VkImageView m_nv12YImageView = VK_NULL_HANDLE;
    VkImage m_nv12UvImage = VK_NULL_HANDLE;
    VkDeviceMemory m_nv12UvImageMemory = VK_NULL_HANDLE;
    VkImageView m_nv12UvImageView = VK_NULL_HANDLE;
    VkImage m_yuv420pUImage = VK_NULL_HANDLE;
    VkDeviceMemory m_yuv420pUImageMemory = VK_NULL_HANDLE;
    VkImageView m_yuv420pUImageView = VK_NULL_HANDLE;
    VkImage m_yuv420pVImage = VK_NULL_HANDLE;
    VkDeviceMemory m_yuv420pVImageMemory = VK_NULL_HANDLE;
    VkImageView m_yuv420pVImageView = VK_NULL_HANDLE;
    VkRenderPass m_nv12YRenderPass = VK_NULL_HANDLE;
    VkRenderPass m_nv12UvRenderPass = VK_NULL_HANDLE;
    VkFramebuffer m_nv12YFramebuffer = VK_NULL_HANDLE;
    VkFramebuffer m_nv12UvFramebuffer = VK_NULL_HANDLE;
    VkFramebuffer m_yuv420pUFramebuffer = VK_NULL_HANDLE;
    VkFramebuffer m_yuv420pVFramebuffer = VK_NULL_HANDLE;
    AVFrame* m_nv12ScratchFrame = nullptr;
    bool m_colorImagePrimed = false;
    bool m_yuv420pPlanesPrimed = false;
    bool m_commandBufferOpenForConversion = false;
};

OffscreenVulkanRenderer::OffscreenVulkanRenderer()
    : d(std::make_unique<OffscreenVulkanRendererPrivate>())
{
}

OffscreenVulkanRenderer::~OffscreenVulkanRenderer() = default;

bool OffscreenVulkanRenderer::initialize(const QSize& outputSize, QString* errorMessage)
{
    return d->initialize(outputSize, errorMessage);
}

QImage OffscreenVulkanRenderer::renderFrame(const RenderRequest& request,
                                            int64_t timelineFrame,
                                            QHash<QString, editor::DecoderContext*>& decoders,
                                            editor::AsyncDecoder* asyncDecoder,
                                            QHash<RenderAsyncFrameKey, editor::FrameHandle>* asyncFrameCache,
                                            const QVector<TimelineClip>& orderedClips,
                                            QHash<QString, RenderClipStageStats>* clipStageStats,
                                            qint64* decodeMs,
                                            qint64* textureMs,
                                            qint64* compositeMs,
                                            qint64* readbackMs,
                                            QJsonArray* skippedClips,
                                            QJsonObject* skippedReasonCounts)
{
    Q_UNUSED(asyncDecoder);
    Q_UNUSED(clipStageStats);
    Q_UNUSED(skippedClips);
    Q_UNUSED(skippedReasonCounts);

    if (decodeMs) {
        *decodeMs = 0;
    }
    if (textureMs) {
        *textureMs = 0;
    }
    if (compositeMs) {
        *compositeMs = 0;
    }
    if (readbackMs) {
        *readbackMs = 0;
    }
    QVector<OffscreenVulkanRendererPrivate::LayerInput> layers;
    layers.reserve(orderedClips.size());
    for (const TimelineClip& clip : orderedClips) {
        if (timelineFrame < clip.startFrame || timelineFrame >= clip.startFrame + clip.durationFrames) {
            continue;
        }
        if (!clipVisualPlaybackEnabled(clip, request.tracks) ||
            clip.mediaType == ClipMediaType::Title ||
            clip.filePath.isEmpty()) {
            continue;
        }
        const TimelineClip::GradingKeyframe grade =
            evaluateClipGradingAtPosition(clip, static_cast<qreal>(timelineFrame));
        if (grade.opacity <= 0.001) {
            continue;
        }
        const int64_t localFrame =
            sourceFrameForClipAtTimelinePosition(
                clip, static_cast<qreal>(timelineFrame), request.renderSyncMarkers);
        const qint64 decodeStart = QDateTime::currentMSecsSinceEpoch();
        const editor::FrameHandle frame =
            decodeRenderFrame(clip.filePath, localFrame, decoders, asyncDecoder, asyncFrameCache);
        if (decodeMs) {
            *decodeMs += (QDateTime::currentMSecsSinceEpoch() - decodeStart);
        }
        if (!frame.isNull() && frame.hasCpuImage()) {
            OffscreenVulkanRendererPrivate::LayerInput layer;
            layer.image = frame.cpuImage();
            if (clip.mediaType == ClipMediaType::Image) {
                layer.cacheKey = clip.id + QStringLiteral(":prepared_rgba");
            }
            layer.opacity = static_cast<float>(grade.opacity);
            const TimelineClip::TransformKeyframe transform =
                evaluateClipRenderTransformAtPosition(
                    clip, static_cast<qreal>(timelineFrame), request.outputSize);
            const QRect fitted = fitRect(layer.image.size(), request.outputSize);
            const float fitScaleX = static_cast<float>(fitted.width()) / qMax(1, request.outputSize.width());
            const float fitScaleY = static_cast<float>(fitted.height()) / qMax(1, request.outputSize.height());
            QMatrix4x4 mvp;
            mvp.setToIdentity();
            const float tx = static_cast<float>((2.0 * transform.translationX) / qMax(1, request.outputSize.width()));
            const float ty = static_cast<float>((-2.0 * transform.translationY) / qMax(1, request.outputSize.height()));
            mvp.translate(tx, ty, 0.0f);
            mvp.rotate(static_cast<float>(transform.rotation), 0.0f, 0.0f, 1.0f);
            mvp.scale(fitScaleX * static_cast<float>(transform.scaleX),
                      fitScaleY * static_cast<float>(transform.scaleY),
                      1.0f);
            const float* mvpData = mvp.constData();
            for (int i = 0; i < 16; ++i) {
                layer.mvp[i] = mvpData[i];
            }
            layers.push_back(layer);
        }
    }
    if (layers.isEmpty()) {
        OffscreenVulkanRendererPrivate::LayerInput black;
        black.image = QImage(request.outputSize, QImage::Format_RGBA8888);
        black.image.fill(Qt::black);
        black.opacity = 1.0f;
        layers.push_back(black);
    }
    const qint64 renderStartMs = QDateTime::currentMSecsSinceEpoch();
    const bool shouldReadbackToImage = (readbackMs != nullptr);
    const QImage output = d->renderFrameFromLayers(layers, shouldReadbackToImage);
    if (compositeMs) {
        *compositeMs = (QDateTime::currentMSecsSinceEpoch() - renderStartMs);
    }
    if (!shouldReadbackToImage) {
        return QImage();
    }
    return output;
}

bool OffscreenVulkanRenderer::convertLastFrameToNv12(AVFrame* frame,
                                                     qint64* nv12ConvertMs,
                                                     qint64* readbackMs)
{
    return d && d->convertLastFrameToNv12(frame, nv12ConvertMs, readbackMs);
}

bool OffscreenVulkanRenderer::beginLastFrameToNv12Readback(qint64* convertMs,
                                                           qint64* readbackMs)
{
    return d && d->beginLastFrameToNv12Readback(convertMs, readbackMs);
}

bool OffscreenVulkanRenderer::finishLastFrameToNv12Readback(AVFrame* frame,
                                                            qint64* convertMs,
                                                            qint64* readbackMs)
{
    return d && d->finishLastFrameToNv12Readback(frame, convertMs, readbackMs);
}

bool OffscreenVulkanRenderer::beginLastFrameToNv12CudaTransfer(qint64* convertMs,
                                                               qint64* transferMs)
{
    return d && d->beginLastFrameToNv12CudaTransfer(convertMs, transferMs);
}

bool OffscreenVulkanRenderer::finishLastFrameToNv12CudaTransfer(AVFrame* cudaFrame,
                                                                qint64* convertMs,
                                                                qint64* transferMs)
{
    return d && d->finishLastFrameToNv12CudaTransfer(cudaFrame, convertMs, transferMs);
}

bool OffscreenVulkanRenderer::convertLastFrameToYuv420p(AVFrame* frame,
                                                        qint64* convertMs,
                                                        qint64* readbackMs)
{
    return d && d->convertLastFrameToYuv420p(frame, convertMs, readbackMs);
}

bool OffscreenVulkanRenderer::beginLastFrameToYuv420pReadback(qint64* convertMs,
                                                              qint64* readbackMs)
{
    return d && d->beginLastFrameToYuv420pReadback(convertMs, readbackMs);
}

bool OffscreenVulkanRenderer::finishLastFrameToYuv420pReadback(AVFrame* frame,
                                                               qint64* convertMs,
                                                               qint64* readbackMs)
{
    return d && d->finishLastFrameToYuv420pReadback(frame, convertMs, readbackMs);
}

bool OffscreenVulkanRenderer::copyLastFrameToBgra(AVFrame* frame,
                                                  qint64* readbackMs)
{
    return d && d->copyLastFrameToBgra(frame, readbackMs);
}

bool OffscreenVulkanRenderer::supportsCudaExternalMemoryInterop() const
{
    return d && d->supportsCudaExternalMemoryInterop();
}

QString OffscreenVulkanRenderer::backendId() const
{
    return QStringLiteral("vulkan");
}

} // namespace render_detail
