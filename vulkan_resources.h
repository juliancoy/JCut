#pragma once

#include <QSize>

#include <vulkan/vulkan.h>

class QVulkanDeviceFunctions;

class VulkanResources final {
public:
    VulkanResources() = default;
    ~VulkanResources();

    VulkanResources(const VulkanResources&) = delete;
    VulkanResources& operator=(const VulkanResources&) = delete;

    bool initialize(VkPhysicalDevice physicalDevice,
                    VkDevice device,
                    QVulkanDeviceFunctions* funcs);
    void destroy();

    bool ensureCheckerTextureUploaded(VkCommandBuffer commandBuffer);

    VkDescriptorSetLayout descriptorSetLayout() const { return m_descriptorSetLayout; }
    VkDescriptorSet descriptorSet() const { return m_descriptorSet; }
    bool isReady() const { return m_initialized; }

private:
    bool createTextureResources();
    uint32_t findMemoryType(uint32_t typeBits, VkMemoryPropertyFlags properties) const;
    void transitionImage(VkCommandBuffer cb,
                         VkImageLayout oldLayout,
                         VkImageLayout newLayout,
                         VkPipelineStageFlags srcStage,
                         VkPipelineStageFlags dstStage,
                         VkAccessFlags srcAccess,
                         VkAccessFlags dstAccess);

    VkPhysicalDevice m_physicalDevice = VK_NULL_HANDLE;
    VkDevice m_device = VK_NULL_HANDLE;
    QVulkanDeviceFunctions* m_funcs = nullptr;
    bool m_initialized = false;

    VkSampler m_sampler = VK_NULL_HANDLE;
    VkDescriptorSetLayout m_descriptorSetLayout = VK_NULL_HANDLE;
    VkDescriptorPool m_descriptorPool = VK_NULL_HANDLE;
    VkDescriptorSet m_descriptorSet = VK_NULL_HANDLE;

    VkImage m_textureImage = VK_NULL_HANDLE;
    VkDeviceMemory m_textureMemory = VK_NULL_HANDLE;
    VkImageView m_textureView = VK_NULL_HANDLE;
    VkImageLayout m_textureLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    bool m_textureUploaded = false;

    VkBuffer m_stagingBuffer = VK_NULL_HANDLE;
    VkDeviceMemory m_stagingMemory = VK_NULL_HANDLE;
};

