#pragma once

#include "core/image_buffer.h"
#include "editor_shared_effect_types.h"
#include "editor_shared_render_sync.h"
#include "transform_skip_aware_timing.h"

#include <QImage>
#include <memory>

#include <QSet>

namespace editor {
class FrameHandle;
}

QImage applyClipGrade(const QImage& source, const TimelineClip& clip);
QImage applyClipGrade(const QImage& source, const TimelineClip::GradingKeyframe& grade);
QImage applyClipMaskEffectsToImage(const QImage& source,
                                   const TimelineClip& clip,
                                   int64_t sourceFrame);
QImage applyClipMaskEffectsToImage(const QImage& source,
                                   const TimelineClip& clip,
                                   int64_t sourceFrame,
                                   const TimelineClip::GradingKeyframe& clipGrade);
QImage rawClipMaskImage(const TimelineClip& clip, int64_t sourceFrame);
QImage rawClipMaskImage(const TimelineClip& clip,
                        const editor::FrameHandle& presentedFrame);
QImage rawClipMaskImageBlocking(const TimelineClip& clip,
                                const editor::FrameHandle& presentedFrame);
std::shared_ptr<const jcut::core::ImageBuffer> rawClipMaskBuffer(
    const TimelineClip& clip,
    int64_t sourceFrame);
std::shared_ptr<const jcut::core::ImageBuffer> rawClipMaskBuffer(
    const TimelineClip& clip,
    int64_t sourceFrame,
    QString* stableIdentity,
    bool blocking = false);
std::shared_ptr<const jcut::core::ImageBuffer> rawClipMaskBuffer(
    const TimelineClip& clip,
    const editor::FrameHandle& presentedFrame,
    QString* stableIdentity = nullptr);
std::shared_ptr<const jcut::core::ImageBuffer> rawClipMaskBufferWaitFor(
    const TimelineClip& clip,
    const editor::FrameHandle& presentedFrame,
    int waitMs,
    QString* stableIdentity = nullptr);
std::shared_ptr<const jcut::core::ImageBuffer> rawClipMaskBufferBlocking(
    const TimelineClip& clip,
    const editor::FrameHandle& presentedFrame,
    QString* stableIdentity = nullptr);
void prefetchClipMaskBuffers(const TimelineClip& clip, int64_t sourceFrame);
void prefetchClipMaskBuffers(const TimelineClip& clip,
                             const editor::FrameHandle& presentedFrame);
bool clipUsesRenderableSidecarMask(const TimelineClip& clip);
bool visualClipActiveAtTimelineClock(const TimelineClip& clip,
                                     const QVector<TimelineTrack>& tracks,
                                     const RenderFrameClock& clock,
                                     bool bypassGrading);
int prefetchRenderableClipMaskBuffersForClock(
    const QVector<TimelineClip>& clips,
    const QVector<TimelineTrack>& tracks,
    const QVector<RenderSyncMarker>& renderSyncMarkers,
    const RenderFrameClock& clock,
    QSet<QString>* nextWindowKeys = nullptr,
    const QSet<QString>* previousWindowKeys = nullptr,
    bool bypassGrading = false);
int prefetchRenderableClipMaskBuffersAtTimelinePosition(
    const QVector<TimelineClip>& clips,
    const QVector<TimelineTrack>& tracks,
    const QVector<RenderSyncMarker>& renderSyncMarkers,
    qreal timelineFramePosition,
    QSet<QString>* nextWindowKeys = nullptr,
    const QSet<QString>* previousWindowKeys = nullptr,
    bool bypassGrading = false);
// Applies correction polygons that have already been filtered for the current
// timeline position. The result is a grayscale mask with corrected regions
// erased to zero, ready for either preview or export upload.
QImage applyCorrectionPolygonsToMaskImage(
    const QImage& source,
    const QVector<TimelineClip::CorrectionPolygon>& activePolygons);
QImage preparedClipMaskImage(const TimelineClip& clip, int64_t sourceFrame, const QSize& size);
QVector<QPointF> defaultGradingCurvePoints();
QVector<QPointF> sanitizeGradingCurvePoints(const QVector<QPointF>& points);
qreal sampleGradingCurveAt(const QVector<QPointF>& points,
                           qreal xNorm,
                           bool smoothingEnabled = true);
QVector<quint8> gradingCurveLut8(const QVector<QPointF>& points,
                                 int samples = TimelineClip::kGradingCurveLutSize,
                                 bool smoothingEnabled = true);
bool gradingCurveDiffersFromIdentity(const QVector<QPointF>& points,
                                     bool smoothingEnabled = true);
bool gradingUsesCurveLut(const TimelineClip::GradingKeyframe& grade);
TimelineClip::GradingKeyframe gradingWithSpeakerOverride(
    const TimelineClip::GradingKeyframe& clipGrade,
    const TimelineClip::GradingKeyframe& speakerGrade);
QImage applyEffectiveClipVisualEffectsToImage(const QImage& source, const EffectiveVisualEffects& effects);
QImage applyMaskFeather(const QImage& source, qreal featherRadius,
                        qreal featherGamma = 1.0, int featherFalloff = 0);
EffectiveVisualEffects evaluateEffectiveVisualEffectsAtFrame(const TimelineClip& clip,
                                                             const QVector<TimelineTrack>& tracks,
                                                             int64_t timelineFrame);
EffectiveVisualEffects evaluateEffectiveVisualEffectsAtPosition(const TimelineClip& clip,
                                                                const QVector<TimelineTrack>& tracks,
                                                                qreal timelineFramePosition);
EffectiveVisualEffects evaluateEffectiveVisualEffectsAtFrame(const TimelineClip& clip,
                                                             const QVector<TimelineTrack>& tracks,
                                                             int64_t timelineFrame,
                                                             const QVector<RenderSyncMarker>& markers);
EffectiveVisualEffects evaluateEffectiveVisualEffectsAtPosition(const TimelineClip& clip,
                                                                const QVector<TimelineTrack>& tracks,
                                                                qreal timelineFramePosition,
                                                                const QVector<RenderSyncMarker>& markers);
EffectiveVisualEffects evaluateEffectiveVisualEffectsAtPosition(const TimelineClip& clip,
                                                                const QVector<TimelineTrack>& tracks,
                                                                qreal timelineFramePosition,
                                                                const QVector<RenderSyncMarker>& markers,
                                                                const PlaybackTimingContext& timing);
bool trackHasEffectPreset(const TimelineTrack& track);
TimelineClip clipWithTrackEffectSettings(const TimelineClip& clip, const QVector<TimelineTrack>& tracks);
bool effectPresetSupportedForClipRole(ClipEffectPreset preset, ClipRole role);
bool clipShouldApplySpeechFilterFrameCrossfade(const TimelineClip& clip);
// Source-history presets need independent decoded frames. Mask mattes are
// virtual views of their parent, so those presets are preserved in the model
// but rendered inactive to keep the matte fail-closed.
TimelineClip clipWithRenderableEffectSettings(const TimelineClip& clip,
                                              const QVector<TimelineTrack>& tracks);
TimelineClip evaluateClipEffectAnimationAtPosition(const TimelineClip& clip,
                                                   qreal timelineFramePosition);
TimelineClip evaluateClipEffectAnimationAtPosition(
    const TimelineClip& clip,
    qreal timelineFramePosition,
    const QVector<RenderSyncMarker>& markers,
    const PlaybackTimingContext& timing);
