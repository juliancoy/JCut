#pragma once

#include "core/geometry.h"
#include "direct_vulkan_preview_presenter.h"
#include "preview_interaction_state.h"
#include "vulkan_detector_frame_handoff.h"

#include <QString>

#include <array>
#include <memory>

class VulkanResources;

class DirectVulkanFrameHandoffPipeline final {
public:
    struct Result {
        bool attempted = false;
        bool sampledFrameReady = false;
        VkImage image = VK_NULL_HANDLE;
        VkImageLayout layout = VK_IMAGE_LAYOUT_UNDEFINED;
        VkFormat format = VK_FORMAT_UNDEFINED;
        VkDescriptorSet descriptorSet = VK_NULL_HANDLE;
        int descriptorSetIndex = -1;
        int descriptorSetCount = 0;
        jcut::core::SizeI size;
    };

    DirectVulkanFrameHandoffPipeline();
    ~DirectVulkanFrameHandoffPipeline();

    DirectVulkanFrameHandoffPipeline(const DirectVulkanFrameHandoffPipeline&) = delete;
    DirectVulkanFrameHandoffPipeline& operator=(const DirectVulkanFrameHandoffPipeline&) = delete;

    bool initialize(const jcut::vulkan_detector::VulkanDeviceContext& context,
                    QString* errorMessage = nullptr);
    void release();
    bool isInitialized() const;

    Result record(VkCommandBuffer commandBuffer,
                  uint32_t frameSlot,
                  const VulkanPreviewClipFrameStatus& status,
                  VulkanResources* resources,
                  DirectVulkanPreviewStats* stats);

private:
    static constexpr size_t kInFlightHandoffCount = 16;

    static QString framePixelFormatName(int format);
    static QString vulkanFormatName(VkFormat format);

    jcut::vulkan_detector::VulkanDetectorFrameHandoff* handoffForFrameSlot(uint32_t frameSlot);

    std::array<std::unique_ptr<jcut::vulkan_detector::VulkanDetectorFrameHandoff>, kInFlightHandoffCount> m_handoffs;
    QString m_lastError;
};
