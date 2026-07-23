#pragma once

#include "editor_shared_core.h"
#include "transform_skip_aware_timing.h"

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
// Source-history presets need independent decoded frames. Mask mattes are
// virtual views of their parent, so those presets are preserved in the model
// but rendered inactive to keep the matte fail-closed.
TimelineClip clipWithRenderableEffectSettings(const TimelineClip& clip,
                                              const QVector<TimelineTrack>& tracks);
