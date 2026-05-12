#include "facestream_time_mapping.h"

#include <cmath>

FacestreamFrameDomain inferFacestreamFrameDomain(const TimelineClip& clip,
                                               int64_t keyframeMin,
                                               int64_t keyframeMax)
{
    const int64_t sourceStart = qMax<int64_t>(0, clip.sourceInFrame);
    const int64_t sourceEnd =
        sourceStart + qMax<int64_t>(0, clip.sourceDurationFrames) + 2;
    const bool likelySourceAbsolute =
        keyframeMax >= 0 &&
        keyframeMin >= qMax<int64_t>(0, sourceStart - 2) &&
        keyframeMax <= sourceEnd;
    if (likelySourceAbsolute) {
        return FacestreamFrameDomain::SourceAbsolute;
    }

    const bool likelyClipTimeline =
        keyframeMax >= 0 &&
        keyframeMin >= 0 &&
        keyframeMax <= (qMax<int64_t>(0, clip.durationFrames) + 2);
    return likelyClipTimeline ? FacestreamFrameDomain::ClipTimeline30Fps
                              : FacestreamFrameDomain::SourceRelative;
}

int64_t facestreamLookupFrameForDomain(FacestreamFrameDomain domain,
                                      int64_t localTimelineFrame,
                                      int64_t localSourceFrame,
                                      int64_t absoluteSourceFrame)
{
    if (domain == FacestreamFrameDomain::ClipTimeline30Fps) {
        return qMax<int64_t>(0, localTimelineFrame);
    }
    if (domain == FacestreamFrameDomain::SourceAbsolute) {
        return qMax<int64_t>(0, absoluteSourceFrame);
    }
    return qMax<int64_t>(0, localSourceFrame);
}

int64_t mapFacestreamFrameToSourceFrame(const TimelineClip& clip,
                                       int64_t facestreamFrame,
                                       FacestreamFrameDomain domain,
                                       const QVector<RenderSyncMarker>& markers)
{
    const int64_t safeFrame = qMax<int64_t>(0, facestreamFrame);
    if (domain == FacestreamFrameDomain::ClipTimeline30Fps) {
        const int64_t timelineFrame = clip.startFrame + safeFrame;
        return sourceFrameForClipAtTimelinePosition(
            clip,
            static_cast<qreal>(timelineFrame),
            markers);
    }
    if (domain == FacestreamFrameDomain::SourceAbsolute) {
        return safeFrame;
    }
    return qMax<int64_t>(0, clip.sourceInFrame + safeFrame);
}
