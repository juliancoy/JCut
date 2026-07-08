#pragma once

#include "editor_timeline_fwd.h"
#include "playback_timing_context.h"

#include <QtGlobal>
#include <QVector>

void setTransformSkipAwareTimelineRanges(const QVector<ExportRangeSegment>& ranges);
PlaybackTimingContext activePlaybackTimingContext();

qreal clipPlaybackFramePositionForTimelineFrame(const TimelineClip& clip,
                                                qreal timelineFramePosition);
qreal clipPlaybackFramePositionForTimelineFrame(const TimelineClip& clip,
                                                qreal timelineFramePosition,
                                                const PlaybackTimingContext& timing);
qreal clipPlaybackDurationFrames(const TimelineClip& clip);
qreal clipPlaybackDurationFrames(const TimelineClip& clip,
                                 const PlaybackTimingContext& timing);

qreal interpolationFactorForTransformFrames(const TimelineClip& clip,
                                            qreal startLocalFrame,
                                            qreal endLocalFrame,
                                            qreal localFrame);
qreal interpolationFactorForTransformFrames(const TimelineClip& clip,
                                            qreal startLocalFrame,
                                            qreal endLocalFrame,
                                            qreal localFrame,
                                            const PlaybackTimingContext& timing);
