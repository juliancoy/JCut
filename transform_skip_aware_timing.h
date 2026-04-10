#pragma once

#include "editor_shared.h"

void setTransformSkipAwareTimelineRanges(const QVector<ExportRangeSegment>& ranges);

qreal interpolationFactorForTransformFrames(const TimelineClip& clip,
                                            qreal startLocalFrame,
                                            qreal endLocalFrame,
                                            qreal localFrame);
