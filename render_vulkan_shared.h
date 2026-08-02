#pragma once

#include "background_fill_effect_fwd.h"
#include "core/image_buffer.h"
#include "editor_shared.h"
#include "frame_handle.h"
#include "playback_timing_context.h"
#include "titles.h"

#include <QByteArray>
#include <QMatrix4x4>
#include <QPointF>
#include <QVector>
#include <QRectF>
#include <QSize>
#include <QTransform>

#include <memory>

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
inline constexpr float kVulkanEffectModeBackgroundProgressiveBidirectionalEdgeStretch = -6.0f;
inline constexpr float kVulkanEffectModeBackgroundTile = -7.0f;
inline constexpr float kVulkanMaskGradeUseSelectedCurveLut = -1.0f;

QByteArray vulkanCurveLutRgbaBytes(const TimelineClip::GradingKeyframe& grade);
QByteArray vulkanIdentityCurveLutRgbaBytes();
QByteArray vulkanMaskCorrectionStorageData(
    const QVector<TimelineClip::CorrectionPolygon>& polygons);

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

struct VulkanGradePayload {
    VulkanDrawEffectState effects;
    QByteArray curveLutRgba;
    bool curveLutApplied = false;
};

struct VulkanRenderTranscriptInput {
    TimelineClip clip;
    TranscriptOverlayLayout layout;
    QRectF outputRect;
    QString speakerTitle;
    qreal opacityMultiplier = 1.0;
};

struct VulkanRenderTextInputs {
    QVector<VulkanRenderTranscriptInput> transcripts;
    QVector<EvaluatedTitle> title3D;
    bool hasSpeakerLabel = false;
    SpeakerLabelOverlaySpec speakerLabel;
};

// Canonical renderer-neutral description of one visual layer. Preview and
// export inherit this packet and add only scheduling, diagnostics, or
// backend-resource state. Keep semantic render inputs here so the two Vulkan
// pathways cannot silently grow different ownership, mask, or effect rules.
struct VulkanRenderLayerPacket {
    QString clipId;
    QString mediaOwnerClipId;
    QString timingOwnerClipId;
    QString effectsOwnerClipId;
    QString matteOwnerClipId;

    editor::FrameHandle frame;
    QSize frameSize;
    bool frameCrossfadeActive = false;
    int64_t frameCrossfadeTimelineFrame = -1;
    int64_t frameCrossfadeRequestedSourceFrame = -1;
    int64_t frameCrossfadePresentedSourceFrame = -1;
    float frameCrossfadeOpacity = 0.0f;
    QSize frameCrossfadeFrameSize;
    editor::FrameHandle frameCrossfadeFrame;
    std::shared_ptr<const jcut::core::ImageBuffer>
        frameCrossfadeMaskBuffer;
    QString frameCrossfadeMaskIdentity;
    bool frameCrossfadeMaskTextureEnabled = false;
    QRectF targetRect;
    QRectF fittedRect;
    TimelineClip::TransformKeyframe transform;
    TimelineClip::GradingKeyframe grading;
    VulkanGradePayload gradePayload;

    QString maskIdentity;
    std::shared_ptr<const jcut::core::ImageBuffer> maskBuffer;
    std::shared_ptr<const jcut::core::ImageBuffer> previousMaskBuffer;
    std::shared_ptr<const jcut::core::ImageBuffer> nextMaskBuffer;
    QString temporalMaskIdentity;
    QSize maskSourceSize;
    QByteArray maskCorrectionStorage;
    bool maskTextureEnabled = false;
    bool maskClipSource = false;
    bool maskForegroundLayerEnabled = false;
    bool maskShowOnly = false;
    bool maskGradeEnabled = false;
    bool maskInvert = false;
    qreal maskErode = 0.0;
    qreal maskDilate = 0.0;
    qreal maskBlur = 0.0;
    bool maskTemporalStabilizeEnabled = false;
    qreal maskTemporalStabilizeStrength = 0.75;
    int maskTemporalStabilizeMotionRadius = 4;
    qreal maskFeather = 0.0;
    qreal maskFeatherGamma = 1.0;
    int maskFeatherFalloff = 0;
    qreal maskOpacity = 1.0;
    bool maskDropShadowEnabled = false;
    qreal maskDropShadowRadius = 12.0;
    qreal maskDropShadowOffsetX = 0.0;
    qreal maskDropShadowOffsetY = 4.0;
    qreal maskDropShadowOpacity = 0.45;
    TimelineClip::GradingKeyframe maskGrade;
    VulkanGradePayload maskGradePayload;

    QVector<TimelineClip::CorrectionPolygon> correctionPolygons;
    int correctionPolygonCount = 0;

    bool differenceMatteEnabled = false;
    qreal differenceThreshold = 0.10;
    qreal differenceSoftness = 0.05;
    editor::FrameHandle differenceReferenceFrame;
    QVector<editor::FrameHandle> temporalEchoFrames;
    qreal temporalEchoDecay = 0.65;

    VulkanEffectPipelinePlan effectPlan;
    VulkanRenderTextInputs textInputs;

    void setGrading(const TimelineClip::GradingKeyframe& value);
    void setMaskGrade(const TimelineClip::GradingKeyframe& value);
    void setCorrectionPolygons(
        const QVector<TimelineClip::CorrectionPolygon>& value);
};

// Stable identity for sharing one neutral decoded-frame import among linked
// layers in the same Vulkan composition. An empty key deliberately disables
// reuse when ownership or source identity is incomplete.
QString vulkanSourceFrameCacheKey(
    const QString& mediaOwnerClipId,
    const editor::FrameHandle& frame);

struct VulkanBackgroundFillMapping {
    float centerXNorm = 0.5f;
    float centerYNorm = 0.5f;
    // Reciprocals of the transformed full source dimensions. Keeping these
    // invariant divisions on the CPU saves work for every fill fragment.
    float outputHeightOverSourceWidth = 1.0f;
    float signedOutputHeightOverSourceHeight = 1.0f;
    float rotationRadians = 0.0f;
};

bool vulkanClipSupportsBackgroundFillSource(const TimelineClip& clip);

VulkanBackgroundFillMapping vulkanBackgroundFillMapping(
    const QTransform& sourceToOutput,
    const QRectF& localRect,
    const QSize& outputSize);
VulkanBackgroundFillMapping vulkanBackgroundFillMapping(
    const QTransform& sourceToOutput,
    const QRectF& localRect,
    const QRectF& outputRect);

VulkanDrawEffectState vulkanDrawEffectStateForGrade(const TimelineClip::GradingKeyframe& grade);
VulkanGradePayload vulkanGradePayloadForGrade(
    const TimelineClip::GradingKeyframe& grade);
VulkanDrawEffectState vulkanBlurredBackgroundEffectState(float opacity);
VulkanDrawEffectState vulkanBackgroundFillEffectState(BackgroundFillEffect effect,
                                                      float opacity,
                                                      float brightness = 0.0f,
                                                      float saturation = 1.0f,
                                                      int edgePixels = 1,
                                                      float edgePower = 2.0f,
                                                      const QRectF& validTextureRectNorm = QRectF(0.0, 0.0, 1.0, 1.0),
                                                      const VulkanBackgroundFillMapping& mapping = VulkanBackgroundFillMapping{});

} // namespace render_detail
