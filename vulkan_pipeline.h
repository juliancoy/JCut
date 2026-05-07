#pragma once

#include <QString>

#include <vulkan/vulkan.h>

class QVulkanDeviceFunctions;

class VulkanPipeline final {
public:
    struct Push {
        float mvp[16] = {
            1.f, 0.f, 0.f, 0.f,
            0.f, 1.f, 0.f, 0.f,
            0.f, 0.f, 1.f, 0.f,
            0.f, 0.f, 0.f, 1.f
        };
        float brightness = 0.0f;
        float contrast = 1.0f;
        float saturation = 1.0f;
        float opacity = 1.0f;
        float shadows[3] = {0.0f, 0.0f, 0.0f};
        float midtones[3] = {0.0f, 0.0f, 0.0f};
        float highlights[3] = {0.0f, 0.0f, 0.0f};
    };

    VulkanPipeline() = default;
    ~VulkanPipeline();

    VulkanPipeline(const VulkanPipeline&) = delete;
    VulkanPipeline& operator=(const VulkanPipeline&) = delete;

    bool initialize(VkDevice device,
                    QVulkanDeviceFunctions* funcs,
                    VkRenderPass renderPass,
                    VkDescriptorSetLayout descriptorSetLayout,
                    QString* errorMessage = nullptr);
    void destroy();

    bool isReady() const { return m_ready; }
    void bindAndDraw(VkCommandBuffer commandBuffer,
                     const VkViewport& viewport,
                     const VkRect2D& scissor,
                     VkDescriptorSet descriptorSet,
                     const Push& push) const;

private:
    VkShaderModule createShaderModule(const QString& path, QString* errorMessage);

    VkDevice m_device = VK_NULL_HANDLE;
    QVulkanDeviceFunctions* m_funcs = nullptr;
    bool m_ready = false;

    VkPipelineLayout m_pipelineLayout = VK_NULL_HANDLE;
    VkPipeline m_pipeline = VK_NULL_HANDLE;
    VkShaderModule m_vertShader = VK_NULL_HANDLE;
    VkShaderModule m_fragShader = VK_NULL_HANDLE;
};

