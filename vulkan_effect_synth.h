#pragma once

#include "render_vulkan_shared.h"

namespace render_detail {

struct VulkanSynth3DParams {
    QRectF outputRect;
    qreal sourceAspect = 1.0;
    int copyCount = 1;
    qreal scale = 1.0;
    qreal speed = 1.0;
    qreal timelineFrame = 0.0;
    bool alternateHandedness = true;
};

QVector<VulkanEffectPipelinePlan::DrawPass> vulkanSynth3DDrawPasses(
    const VulkanSynth3DParams& params);

} // namespace render_detail
