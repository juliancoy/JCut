#pragma once

#include "cpu_overlay_render_backend.h"

#include <QByteArray>
#include <QImage>
#include <QSize>

#include <vulkan/vulkan.h>

#include <array>

class QVulkanDeviceFunctions;

class VulkanResources final {
public:
    static constexpr size_t kDescriptorSetCount = 3;

    VulkanResources() = default;
    ~VulkanResources();

    VulkanResources(const VulkanResources&) = delete;
    VulkanResources& operator=(const VulkanResources&) = delete;

    bool initialize(VkPhysicalDevice physicalDevice,
                    VkDevice device,
                    QVulkanDeviceFunctions* funcs);
    void destroy();

    bool ensureCheckerTextureUploaded(VkCommandBuffer commandBuffer);
    bool setSampledImage(VkImageView imageView, VkImageLayout imageLayout);
    bool uploadImageTexture(VkCommandBuffer commandBuffer, const render_detail::OverlayImage& image);
    bool uploadImageTexture(VkCommandBuffer commandBuffer, const QImage& image);
    bool uploadCurveLut(VkCommandBuffer commandBuffer, const QByteArray& rgbaLut);

    VkDescriptorSetLayout descriptorSetLayout() const { return m_descriptorSetLayout; }
    VkDescriptorSet descriptorSet() const { return m_descriptorSets[m_descriptorSetIndex]; }
    size_t descriptorSetIndex() const { return m_descriptorSetIndex; }
    size_t descriptorSetCount() const { return m_descriptorSets.size(); }
    bool isReady() const { return m_initialized; }

private:
    bool createTextureResources();
    bool createTextureImage(const QSize& size);
    void destroyTextureImage();
    bool ensureTextureSize(const QSize& size);
    bool ensureStagingCapacity(VkDeviceSize bytes);
    uint32_t findMemoryType(uint32_t typeBits, VkMemoryPropertyFlags properties) const;
    void transitionTextureImage(VkCommandBuffer cb,
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
    std::array<VkDescriptorSet, kDescriptorSetCount> m_descriptorSets{};
    size_t m_descriptorSetIndex = 0;

    VkImage m_textureImage = VK_NULL_HANDLE;
    VkDeviceMemory m_textureMemory = VK_NULL_HANDLE;
    VkImageView m_textureView = VK_NULL_HANDLE;
    VkImageLayout m_textureLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    bool m_textureUploaded = false;
    QSize m_textureSize;

    VkImage m_curveLutImage = VK_NULL_HANDLE;
    VkDeviceMemory m_curveLutMemory = VK_NULL_HANDLE;
    VkImageView m_curveLutView = VK_NULL_HANDLE;
    VkImageLayout m_curveLutLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    QByteArray m_curveLutBytes;

    VkBuffer m_stagingBuffer = VK_NULL_HANDLE;
    VkDeviceMemory m_stagingMemory = VK_NULL_HANDLE;
    VkDeviceSize m_stagingCapacity = 0;
};
