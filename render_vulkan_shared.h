#pragma once

#include "editor_shared.h"

#include <QByteArray>
#include <QMatrix4x4>
#include <QRectF>
#include <QSize>
#include <QTransform>

namespace render_detail {

QByteArray vulkanCurveLutRgbaBytes(const TimelineClip::GradingKeyframe& grade);
QByteArray vulkanIdentityCurveLutRgbaBytes();

void vulkanMvpForOutputRect(const QRectF& rect,
                            const QSize& outputSize,
                            qreal rotationDegrees,
                            float outMvp[16]);
void vulkanMvpForOutputRectMaybeFlippedY(const QRectF& rect,
                                         const QSize& outputSize,
                                         qreal rotationDegrees,
                                         bool flipY,
                                         float outMvp[16]);
void vulkanMvpForPreviewTransform(const QTransform& clipToSwapchain,
                                  const QRectF& localRect,
                                  const QSize& swapSize,
                                  float outMvp[16]);

struct VulkanDrawEffectState {
    float brightness = 0.0f;
    float contrast = 1.0f;
    float saturation = 1.0f;
    float opacity = 1.0f;
    float shadows[4] = {0.0f, 0.0f, 0.0f, 0.0f};
    float midtones[4] = {0.0f, 0.0f, 0.0f, 0.0f};
    float highlights[4] = {0.0f, 0.0f, 0.0f, 0.0f};
};

VulkanDrawEffectState vulkanDrawEffectStateForGrade(const TimelineClip::GradingKeyframe& grade);
VulkanDrawEffectState vulkanBlurredBackgroundEffectState(float opacity);

} // namespace render_detail
