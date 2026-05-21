#pragma once

#include "editor_shared_core.h"

QImage applyClipGrade(const QImage& source, const TimelineClip& clip);
QImage applyClipGrade(const QImage& source, const TimelineClip::GradingKeyframe& grade);
QVector<QPointF> defaultGradingCurvePoints();
QVector<QPointF> sanitizeGradingCurvePoints(const QVector<QPointF>& points);
qreal sampleGradingCurveAt(const QVector<QPointF>& points,
                           qreal xNorm,
                           bool smoothingEnabled = true);
QVector<quint8> gradingCurveLut8(const QVector<QPointF>& points,
                                 int samples = TimelineClip::kGradingCurveLutSize,
                                 bool smoothingEnabled = true);
QImage applyEffectiveClipVisualEffectsToImage(const QImage& source, const EffectiveVisualEffects& effects);
QImage applyMaskFeather(const QImage& source, qreal featherRadius, qreal featherGamma = 1.0);
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
