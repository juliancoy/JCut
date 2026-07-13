#pragma once

#include "preview_interaction_state.h"

#include <QList>
#include <QSize>

#include <functional>
#include <memory>
#include <vector>

#include <vulkan/vulkan.h>

class QWidget;
class QLabel;
class QFrame;
class QVulkanDeviceFunctions;

namespace jcut {
class VulkanAudioTab;
}

struct DirectVulkanAudioRenderContext {
    const PreviewInteractionState* state = nullptr;
    QVulkanDeviceFunctions* deviceFunctions = nullptr;
    jcut::VulkanAudioTab* audioTab = nullptr;
    VkCommandBuffer commandBuffer = VK_NULL_HANDLE;
    QSize swapchainSize;
    std::function<void()> beginRenderPass;
};

struct DirectVulkanAudioOverlayWidgets {
    QWidget* host = nullptr;
    QWidget* infoPanel = nullptr;
    QLabel* titleLabel = nullptr;
    QLabel* summaryLabel = nullptr;
    QLabel* footerLabel = nullptr;
    QFrame* hoverCard = nullptr;
    QLabel* hoverAvatarLabel = nullptr;
    QLabel* hoverNameLabel = nullptr;
    QLabel* hoverOrgLabel = nullptr;
    QLabel* hoverMetaLabel = nullptr;
    QLabel* hoverDescLabel = nullptr;
};

bool renderDirectVulkanAudioFrame(const DirectVulkanAudioRenderContext& context,
                                  bool* waitingForWaveformOut = nullptr);

void updateDirectVulkanAudioOverlay(const PreviewInteractionState* state,
                                    const DirectVulkanAudioOverlayWidgets& widgets);
