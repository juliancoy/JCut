#pragma once

#include "editor_shared_core.h"

QString transformInterpolationLabel(bool linearInterpolation);
qreal sanitizeScaleValue(qreal value);
void normalizeClipTransformKeyframes(TimelineClip& clip);
void normalizeClipGradingKeyframes(TimelineClip& clip);
void normalizeClipOpacityKeyframes(TimelineClip& clip);
void normalizeClipTitleKeyframes(TimelineClip& clip);
TimelineClip::TransformKeyframe evaluateClipKeyframeOffsetAtFrame(const TimelineClip& clip, int64_t timelineFrame);
TimelineClip::TransformKeyframe evaluateClipTransformAtFrame(const TimelineClip& clip, int64_t timelineFrame);
TimelineClip::TransformKeyframe evaluateClipTransformAtPosition(const TimelineClip& clip, qreal timelineFramePosition);
TimelineClip::TransformKeyframe evaluateClipSpeakerFramingAtFrame(const TimelineClip& clip,
                                                                  int64_t timelineFrame,
                                                                  const QSize& outputSize = QSize());
TimelineClip::TransformKeyframe evaluateClipSpeakerFramingAtFrame(const TimelineClip& clip,
                                                                  int64_t timelineFrame,
                                                                  const QVector<RenderSyncMarker>& markers,
                                                                  const QSize& outputSize = QSize());
TimelineClip::TransformKeyframe evaluateClipSpeakerFramingAtPosition(const TimelineClip& clip,
                                                                     qreal timelineFramePosition,
                                                                     const QSize& outputSize = QSize());
TimelineClip::TransformKeyframe evaluateClipSpeakerFramingAtPosition(const TimelineClip& clip,
                                                                     qreal timelineFramePosition,
                                                                     const QVector<RenderSyncMarker>& markers,
                                                                     const QSize& outputSize = QSize());
TimelineClip::TransformKeyframe evaluateClipSpeakerFramingForFaceBoxAtPosition(
    const TimelineClip& clip,
    qreal timelineFramePosition,
    const QPointF& locationNorm,
    qreal boxSizeNorm,
    const QSize& outputSize = QSize());
void warmClipSpeakerFramingContinuityRuntime(const TimelineClip& clip);
void warmClipsSpeakerFramingContinuityRuntime(const QVector<TimelineClip>& clips);
void prepareClipSpeakerFramingContinuityRuntimeBlocking(const TimelineClip& clip);
TimelineClip::TransformKeyframe evaluateClipSpeakerFramingTargetAtFrame(const TimelineClip& clip,
                                                                        int64_t timelineFrame);
TimelineClip::TransformKeyframe evaluateClipSpeakerFramingTargetAtPosition(const TimelineClip& clip,
                                                                           qreal timelineFramePosition);
bool evaluateClipSpeakerFramingEnabledAtFrame(const TimelineClip& clip, int64_t timelineFrame);
bool evaluateClipSpeakerFramingEnabledAtPosition(const TimelineClip& clip, qreal timelineFramePosition);
TimelineClip::TransformKeyframe composeClipTransforms(const TimelineClip::TransformKeyframe& base,
                                                      const TimelineClip::TransformKeyframe& overlay);
TimelineClip::TransformKeyframe evaluateClipRenderTransformAtFrame(const TimelineClip& clip,
                                                                   int64_t timelineFrame,
                                                                   const QSize& outputSize = QSize());
TimelineClip::TransformKeyframe evaluateClipRenderTransformAtFrame(const TimelineClip& clip,
                                                                   int64_t timelineFrame,
                                                                   const QVector<RenderSyncMarker>& markers,
                                                                   const QSize& outputSize = QSize());
TimelineClip::TransformKeyframe evaluateClipRenderTransformAtPosition(const TimelineClip& clip,
                                                                      qreal timelineFramePosition,
                                                                      const QSize& outputSize = QSize());
TimelineClip::TransformKeyframe evaluateClipRenderTransformAtPosition(const TimelineClip& clip,
                                                                      qreal timelineFramePosition,
                                                                      const QVector<RenderSyncMarker>& markers,
                                                                      const QSize& outputSize = QSize());
TimelineClip::GradingKeyframe evaluateClipGradingAtFrame(const TimelineClip& clip, int64_t timelineFrame);
TimelineClip::GradingKeyframe evaluateClipGradingAtPosition(const TimelineClip& clip, qreal timelineFramePosition);
qreal evaluateClipOpacityAtFrame(const TimelineClip& clip, int64_t timelineFrame);
qreal evaluateClipOpacityAtPosition(const TimelineClip& clip, qreal timelineFramePosition);
qreal evaluateEffectiveClipOpacityAtFrame(const TimelineClip& clip,
                                          const QVector<TimelineTrack>& tracks,
                                          int64_t timelineFrame);
qreal evaluateEffectiveClipOpacityAtPosition(const TimelineClip& clip,
                                             const QVector<TimelineTrack>& tracks,
                                             qreal timelineFramePosition);
TimelineClip::GradingKeyframe evaluateEffectiveClipGradingAtFrame(const TimelineClip& clip,
                                                                  const QVector<TimelineTrack>& tracks,
                                                                  int64_t timelineFrame);
TimelineClip::GradingKeyframe evaluateEffectiveClipGradingAtPosition(const TimelineClip& clip,
                                                                     const QVector<TimelineTrack>& tracks,
                                                                     qreal timelineFramePosition);
TimelineClip::GradingKeyframe evaluateEffectiveClipGradingAtFrame(const TimelineClip& clip, int64_t timelineFrame);
TimelineClip::GradingKeyframe evaluateEffectiveClipGradingAtPosition(const TimelineClip& clip, qreal timelineFramePosition);
