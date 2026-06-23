#pragma once

#include "editor_shared_core.h"

int64_t frameToSamples(int64_t frame);
int64_t framePositionToSamples(qreal framePosition);
qreal samplesToFramePosition(int64_t samples);
QRect previewCanvasBaseRectForWidget(const QRect& widgetRect,
                                     const QSize& outputSize,
                                     int marginPx = 36);
QRectF previewCanvasBaseRectForWidgetF(const QRectF& widgetRect,
                                       const QSize& outputSize,
                                       qreal marginPx = 36.0);
QRect scaledPreviewCanvasRect(const QRect& baseRect,
                              qreal previewZoom,
                              const QPointF& previewPanOffset = QPointF());
QRectF scaledPreviewCanvasRectF(const QRectF& baseRect,
                                qreal previewZoom,
                                const QPointF& previewPanOffset = QPointF());
QPointF clampedPreviewPanOffset(const QRectF& baseRect,
                                qreal previewZoom,
                                const QPointF& previewPanOffset);
QPointF previewCanvasScaleForTargetRect(const QRect& targetRect,
                                        const QSize& outputSize);
QPointF previewCanvasScaleForTargetRectF(const QRectF& targetRect,
                                         const QSize& outputSize);
QRect previewFitRectToBounds(const QSize& source, const QRect& bounds);
QRectF previewFitRectToBoundsF(const QSize& source, const QRectF& bounds);
qreal resolvedSourceFps(const TimelineClip& clip);
qreal timelineFrameToSeconds(int64_t timelineFrame);
QRectF normalizedCenterBoxRect(qreal xNorm, qreal yNorm, qreal boxSizeNorm, const QSizeF& frameSizePx);
int64_t sourceFramesToSamples(const TimelineClip& clip, qreal sourceFrames);
int64_t clipTimelineStartSamples(const TimelineClip& clip);
int64_t clipTimelineDurationSamples(const TimelineClip& clip);
int64_t clipTimelineEndSamples(const TimelineClip& clip);
int64_t clipSourceInSamples(const TimelineClip& clip);
void normalizeSubframeTiming(int64_t& frame, int64_t& subframeSamples);
void normalizeClipTiming(TimelineClip& clip);
int64_t playableSampleAtOrAfter(int64_t samplePos,
                                const QVector<ExportRangeSegment>& ranges,
                                bool* atOrPastEnd = nullptr);
