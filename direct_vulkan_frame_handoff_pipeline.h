#pragma once

#include "direct_vulkan_preview_presenter.h"
#include "preview_interaction_state.h"
#include "vulkan_detector_frame_handoff.h"

#include <QString>
#include <QSize>

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
        QSize size;
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
                  const VulkanPreviewClipFrameStatus& status,
                  VulkanResources* resources,
                  DirectVulkanPreviewStats* stats);

private:
    static QString framePixelFormatName(int format);
    static QString vulkanFormatName(VkFormat format);

    std::unique_ptr<jcut::vulkan_detector::VulkanDetectorFrameHandoff> m_handoff;
    QString m_lastError;
};
