#pragma once

#include "editor_shared.h"

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
