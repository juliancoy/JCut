#include "vulkan_preview_window.h"

VulkanPreviewWindow::VulkanPreviewWindow(QWidget* parent)
    : PreviewWindow(parent)
{
    setRenderBackendPreference(QStringLiteral("vulkan"));
}
