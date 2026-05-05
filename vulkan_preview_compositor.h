#pragma once

#include "vulkan_preview_state.h"

#include <QImage>
#include <QString>

namespace vulkan_preview {

bool composeFrame(VulkanRendererState* state, int64_t timelineFrame, QImage* outFrame, QString* errorMessage = nullptr);

} // namespace vulkan_preview
