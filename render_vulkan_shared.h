#pragma once

#include "background_fill_effect.h"
#include "editor_shared.h"

#include <QByteArray>
#include <QMatrix4x4>
#include <QPointF>
#include <QVector>
#include <QRectF>
#include <QSize>
#include <QTransform>

namespace render_detail {

inline constexpr float kVulkanEffectModeNormal = 0.0f;
inline constexpr float kVulkanEffectModeCurve = 1.0f;
inline constexpr float kVulkanEffectModeMaskGrade = 2.0f;
inline constexpr float kVulkanEffectModeMaskOnly = 3.0f;
inline constexpr float kVulkanEffectModeBackgroundBlur = -1.0f;
inline constexpr float kVulkanEffectModeBackgroundEdgeStretch = -2.0f;
inline constexpr float kVulkanEffectModeBackgroundMirror = -3.0f;
inline constexpr float kVulkanMaskGradeUseSelectedCurveLut = -1.0f;

QByteArray vulkanCurveLutRgbaBytes(const TimelineClip::GradingKeyframe& grade);
QByteArray vulkanIdentityCurveLutRgbaBytes();

struct VulkanEffectPipelinePlan {
    enum class Mode {
        PassThrough,
        GeneratedDraws,
    };

    struct DrawPass {
        QRectF outputRect;
        qreal rotationDegrees = 0.0;
        float opacityMultiplier = 1.0f;
        float shaderMode = kVulkanEffectModeNormal;
    };

    Mode mode = Mode::PassThrough;
    QVector<DrawPass> generatedDraws;

    bool usesGeneratedDraws() const { return mode == Mode::GeneratedDraws && !generatedDraws.isEmpty(); }
    QVector<QRectF> generatedDrawRects() const;
};

VulkanEffectPipelinePlan vulkanEffectPipelinePlan(const TimelineClip& clip,
                                                  const QRectF& outputRect,
                                                  const QSize& textureSize,
                                                  qreal timelineFrame,
                                                  qreal effectFrame = -1.0);
QVector<QRectF> vulkanPresetEffectRects(const TimelineClip& clip,
                                        const QRectF& outputRect,
                                        const QSize& textureSize,
                                        qreal timelineFrame);
qreal clipEffectPlaybackFramePosition(const TimelineClip& clip,
                                      const QVector<TimelineClip>& timelineClips,
                                      qreal timelineFramePosition);

void vulkanMvpForOutputRect(const QRectF& rect,
                            const QSize& outputSize,
                            qreal rotationDegrees,
                            float outMvp[16]);
void vulkanMvpForOutputRectMaybeFlippedY(const QRectF& rect,
                                         const QSize& outputSize,
                                         qreal rotationDegrees,
                                         bool flipY,
                                         float outMvp[16]);
void vulkanMvpForExportVideoLayer(const QRectF& fittedRect,
                                  const QPointF& translation,
                                  const QSize& outputSize,
                                  qreal rotationDegrees,
                                  const QPointF& scale,
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
VulkanDrawEffectState vulkanBackgroundFillEffectState(BackgroundFillEffect effect,
                                                      const VulkanDrawEffectState& baseEffects,
                                                      float opacity,
                                                      float brightness = 0.0f,
                                                      float saturation = 1.0f,
                                                      int edgePixels = 1,
                                                      bool progressiveEdge = false,
                                                      float edgePower = 2.0f,
                                                      const QRectF& sourceRectNorm = QRectF());

} // namespace render_detail
