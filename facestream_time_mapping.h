#pragma once

#include "editor_shared.h"

#include <QVector>

enum class FacestreamFrameDomain {
    ClipTimeline30Fps,
    SourceRelative,
    SourceAbsolute
};

FacestreamFrameDomain inferFacestreamFrameDomain(const TimelineClip& clip,
                                               int64_t keyframeMin,
                                               int64_t keyframeMax);

int64_t facestreamLookupFrameForDomain(FacestreamFrameDomain domain,
                                      int64_t localTimelineFrame,
                                      int64_t localSourceFrame,
                                      int64_t absoluteSourceFrame);

int64_t mapFacestreamFrameToSourceFrame(const TimelineClip& clip,
                                       int64_t facestreamFrame,
                                       FacestreamFrameDomain domain,
                                       const QVector<RenderSyncMarker>& markers);

int64_t facestreamTypicalFrameStep(const QVector<int64_t>& sortedFrames);

bool facestreamShouldBridgeGap(int64_t previousFrame,
                               int64_t nextFrame,
                               int64_t typicalStep);

int64_t facestreamMaxEdgeHoldFrames(int64_t typicalStep);
