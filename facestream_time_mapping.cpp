#include "facestream_time_mapping.h"

#include <cmath>
#include <algorithm>

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

int64_t facestreamTypicalFrameStep(const QVector<int64_t>& sortedFrames)
{
    QVector<int64_t> deltas;
    deltas.reserve(sortedFrames.size());
    for (int i = 1; i < sortedFrames.size(); ++i) {
        const int64_t delta = sortedFrames.at(i) - sortedFrames.at(i - 1);
        if (delta > 0) {
            deltas.push_back(delta);
        }
    }
    if (deltas.isEmpty()) {
        return 1;
    }
    std::sort(deltas.begin(), deltas.end());
    return qMax<int64_t>(1, deltas.at(deltas.size() / 2));
}

bool facestreamShouldBridgeGap(int64_t previousFrame,
                               int64_t nextFrame,
                               int64_t typicalStep)
{
    if (previousFrame < 0 || nextFrame < 0 || nextFrame <= previousFrame) {
        return false;
    }
    const int64_t safeStep = qMax<int64_t>(1, typicalStep);
    const int64_t maxBridgeGap = qMax<int64_t>(2, safeStep * 2);
    return (nextFrame - previousFrame) <= maxBridgeGap;
}

int64_t facestreamMaxEdgeHoldFrames(int64_t typicalStep)
{
    const int64_t safeStep = qMax<int64_t>(1, typicalStep);
    return qMax<int64_t>(1, safeStep / 2);
}
