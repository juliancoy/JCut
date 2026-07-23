#pragma once

#include "editor_shared.h"

#include <QHash>

#include <cstdint>

bool clipSupportsTranscriptOverlayPreviewEdits(const TimelineClip& clip);
QString previewTransformOwnerClipId(const QVector<TimelineClip>& clips,
                                    const QString& requestedClipId);
int64_t resolvePreviewDragKeyframeTimelineFrame(QHash<QString, int64_t>& anchorFrames,
                                                const QString& clipId,
                                                int64_t currentFrame,
                                                bool playing,
                                                bool finalize);
bool createPreviewKeyframeAtTimelineFrame(TimelineClip& clip, int64_t timelineFrame);
bool stagePreviewMove(TimelineClip& clip,
                      int64_t keyframeTimelineFrame,
                      qreal translationX,
                      qreal translationY);
bool commitPreviewMove(TimelineClip& clip,
                       int64_t keyframeTimelineFrame,
                       qreal translationX,
                       qreal translationY,
                       bool transcriptOverlaySelected);
bool commitPreviewTransform(TimelineClip& clip,
                            int64_t keyframeTimelineFrame,
                            qreal translationX,
                            qreal translationY,
                            qreal scaleX,
                            qreal scaleY,
                            bool transcriptOverlaySelected);
