#pragma once

#include "background_fill_effect_fwd.h"
#include "editor_shared.h"
#include "playback_timing_context.h"

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
inline constexpr float kVulkanEffectModeSynth3D = 4.0f;
inline constexpr float kVulkanEffectModeDifferenceMatte = 5.0f;
inline constexpr float kVulkanEffectModeMirrorRing = 6.0f;
inline constexpr float kVulkanEffectModeTessellation = 7.0f;
inline constexpr float kVulkanEffectModeKaleidoscope = 8.0f;
inline constexpr float kVulkanEffectModeHexagonalPrism = 9.0f;
inline constexpr float kVulkanEffectModeDroste = 10.0f;
inline constexpr float kVulkanEffectModePolarTunnel = 11.0f;
inline constexpr float kVulkanEffectModeTinyPlanet = 12.0f;
inline constexpr float kVulkanEffectModeInfiniteMirror = 13.0f;
inline constexpr float kVulkanEffectModeQuadMirror = 14.0f;
inline constexpr float kVulkanEffectModeSlitScan = 15.0f;
inline constexpr float kVulkanEffectModeDisplacementMap = 16.0f;
inline constexpr float kVulkanEffectModeTwirlVortex = 17.0f;
inline constexpr float kVulkanEffectModeRippleShockwave = 18.0f;
inline constexpr float kVulkanEffectModePixelSorting = 19.0f;
inline constexpr float kVulkanEffectModeDatamoshGlitch = 20.0f;
inline constexpr float kVulkanEffectModeRgbSplit = 21.0f;
inline constexpr float kVulkanEffectModeHalftoneMosaic = 22.0f;
inline constexpr float kVulkanEffectModeGlassRefraction = 23.0f;
inline constexpr float kVulkanEffectModeSobelEdges = 24.0f;
inline constexpr float kVulkanEffectModeNeonGlow = 25.0f;
inline constexpr float kVulkanEffectModeSpeakerMaskDilation = 26.0f;
inline constexpr float kVulkanEffectModeSpeakerMaskDilationPulse = 27.0f;
inline constexpr float kVulkanEffectModeSpeakerMaskDilationRings = 28.0f;
inline constexpr float kVulkanEffectModeMaskShadow = 29.0f;
inline constexpr float kVulkanEffectModeBackgroundBlur = -1.0f;
inline constexpr float kVulkanEffectModeBackgroundEdgeStretch = -2.0f;
inline constexpr float kVulkanEffectModeBackgroundProgressiveEdgeStretch = -3.0f;
inline constexpr float kVulkanEffectModeBackgroundMirror = -4.0f;
inline constexpr float kVulkanEffectModeFinalCompositeProgressiveEdgeStretch = -5.0f;
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
        qreal depthSortKey = 0.0;
        float palette[9] = {1.0f, 0.0f, 0.0f,
                            0.0f, 1.0f, 0.0f,
                            1.0f, 1.0f, 0.0f};
        float effectParams[4] = {0.0f, 0.0f, 0.0f, 0.0f};
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
                                                  qreal effectFrame = -1.0,
                                                  const PlaybackTimingContext& timing = {});
QVector<QRectF> vulkanPresetEffectRects(const TimelineClip& clip,
                                        const QRectF& outputRect,
                                        const QSize& textureSize,
                                        qreal timelineFrame);
qreal clipEffectPlaybackFramePosition(const TimelineClip& clip,
                                      const QVector<TimelineClip>& timelineClips,
                                      qreal timelineFramePosition,
                                      const QVector<TimelineTrack>& tracks = {});
qreal clipEffectPlaybackFramePosition(const TimelineClip& clip,
                                      const QVector<TimelineClip>& timelineClips,
                                      qreal timelineFramePosition,
                                      const PlaybackTimingContext& timing,
                                      const QVector<TimelineTrack>& tracks = {});

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

struct VulkanBackgroundFillMapping {
    float centerXNorm = 0.5f;
    float centerYNorm = 0.5f;
    // Reciprocals of the transformed full source dimensions. Keeping these
    // invariant divisions on the CPU saves work for every fill fragment.
    float outputHeightOverSourceWidth = 1.0f;
    float signedOutputHeightOverSourceHeight = 1.0f;
    float rotationRadians = 0.0f;
};

bool vulkanClipSupportsProgressiveEdgeStretchSource(const TimelineClip& clip);

struct VulkanProgressiveEdgeStretchLayerPolicy {
    bool presetActive = false;
    bool sourceEligible = false;
    bool drawBackground = false;
};

VulkanProgressiveEdgeStretchLayerPolicy vulkanProgressiveEdgeStretchLayerPolicy(
    const TimelineClip& clip,
    const QVector<TimelineTrack>& tracks);

VulkanBackgroundFillMapping vulkanBackgroundFillMapping(
    const QTransform& sourceToOutput,
    const QRectF& localRect,
    const QSize& outputSize);
VulkanBackgroundFillMapping vulkanBackgroundFillMapping(
    const QTransform& sourceToOutput,
    const QRectF& localRect,
    const QRectF& outputRect);

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
                                                      const QRectF& validTextureRectNorm = QRectF(0.0, 0.0, 1.0, 1.0),
                                                      const VulkanBackgroundFillMapping& mapping = VulkanBackgroundFillMapping{});

} // namespace render_detail
