#pragma once

#include "opengl_preview.h"

// Vulkan Preview Path Wrapper
// This wrapper makes backend intent explicit at construction time.
class VulkanPreviewWindow final : public PreviewWindow {
    Q_OBJECT
public:
    explicit VulkanPreviewWindow(QWidget* parent = nullptr);
    ~VulkanPreviewWindow() override = default;
};
