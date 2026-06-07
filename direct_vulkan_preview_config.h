#pragma once

#include <QString>
#include <vulkan/vulkan.h>

namespace jcut::direct_vulkan_preview {

bool vulkanPreviewDebugChromeEnabled();
bool vulkanPreviewOptimalPresentEnabled();
bool vulkanPreviewReadbackMirrorEnabled();
bool vulkanPreviewDirectSwapchainVisible();
QString vulkanPreviewVisiblePathLabel();
int vulkanPreviewCanvasMarginPx();
QString pixelFormatName(int format);
QString vulkanFormatName(VkFormat format);

} // namespace jcut::direct_vulkan_preview
