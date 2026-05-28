#include "facedetections_time_mapping.h"

#include <algorithm>
#include <cmath>

QString normalizedFacestreamOverlaySource(QString source)
{
    source = source.trimmed().toLower();
    if (source.startsWith(QStringLiteral("scrfd"))) {
        return QStringLiteral("scrfd");
    }
    return source.isEmpty() ? QStringLiteral("all") : source;
}

bool facedetectionsOverlaySourceMatches(const QString& sourceFilter,
                                    const QString& trackSource,
                                    const QString& streamId)
{
    if (sourceFilter == QStringLiteral("all")) {
        return true;
    }
    const QString normalizedTrackSource = trackSource.trimmed().toLower();
    const QString normalizedStreamId = streamId.trimmed().toLower();
    if (sourceFilter == QStringLiteral("scrfd")) {
        return normalizedTrackSource.startsWith(QStringLiteral("scrfd")) ||
               normalizedStreamId.startsWith(QStringLiteral("scrfd"));
    }
    return sourceFilter == normalizedTrackSource || sourceFilter == normalizedStreamId;
}

FacestreamFrameDomain inferFacestreamFrameDomain(const TimelineClip& clip,
                                               int64_t keyframeMin,
                                               int64_t keyframeMax)
{
    const int64_t sourceStart = qMax<int64_t>(0, clip.sourceInFrame);
    const int64_t sourceEnd =
        sourceStart + qMax<int64_t>(0, clip.sourceDurationFrames);
    const int64_t clipTimelineEnd = qMax<int64_t>(0, clip.durationFrames);
    const bool likelySourceAbsolute =
        keyframeMax >= 0 &&
        keyframeMin >= qMax<int64_t>(0, sourceStart - 2) &&
        keyframeMax <= (sourceEnd + 2);
    const bool likelyClipTimeline =
        keyframeMax >= 0 &&
        keyframeMin >= 0 &&
        keyframeMax <= (clipTimelineEnd + 2);

    if (likelySourceAbsolute && likelyClipTimeline) {
        const int64_t clipDistance = std::llabs(keyframeMax - clipTimelineEnd);
        const int64_t sourceDistance = std::llabs(keyframeMax - sourceEnd);
        if (clipDistance <= sourceDistance) {
            return FacestreamFrameDomain::ClipTimeline30Fps;
        }
        return FacestreamFrameDomain::SourceAbsolute;
    }
    if (likelySourceAbsolute) {
        return FacestreamFrameDomain::SourceAbsolute;
    }
    if (likelyClipTimeline) {
        return FacestreamFrameDomain::ClipTimeline30Fps;
    }
    return FacestreamFrameDomain::SourceRelative;
}

FacestreamSourceScanRange facedetectionsSourceAbsoluteScanRangeForClip(const TimelineClip& clip)
{
    FacestreamSourceScanRange range;
    range.startFrame = qMax<int64_t>(0, clip.sourceInFrame);
    range.endFrameExclusive = qMax<int64_t>(0, clip.sourceDurationFrames);
    if (range.endFrameExclusive <= range.startFrame) {
        range.error = QStringLiteral("Invalid source-frame scan range: sourceInFrame=%1 sourceDurationFrames=%2")
                          .arg(range.startFrame)
                          .arg(clip.sourceDurationFrames);
        return range;
    }
    range.frameCount = range.endFrameExclusive - range.startFrame;
    range.valid = true;
    return range;
}

int64_t facedetectionsLookupFrameForDomain(FacestreamFrameDomain domain,
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
                                       int64_t facedetectionsFrame,
                                       FacestreamFrameDomain domain,
                                       const QVector<RenderSyncMarker>& markers)
{
    const int64_t safeFrame = qMax<int64_t>(0, facedetectionsFrame);
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

int64_t facedetectionsTypicalFrameStep(const QVector<int64_t>& sortedFrames)
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

bool facedetectionsShouldBridgeGap(int64_t previousFrame,
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

int64_t facedetectionsMaxEdgeHoldFrames(int64_t typicalStep)
{
    const int64_t safeStep = qMax<int64_t>(1, typicalStep);
    return qMax<int64_t>(1, safeStep / 2);
}

bool resolveFacestreamTrackAtPlayhead(const TimelineClip& clip,
                                      const FacestreamResolvedTrack& track,
                                      const QVector<RenderSyncMarker>& markers,
                                      int64_t playheadTimelineFrame,
                                      int64_t playheadSourceFrame,
                                      FacestreamResolvedSelection* selectionOut)
{
    if (!selectionOut || track.trackId < 0 || track.keyframes.isEmpty()) {
        return false;
    }

    const int64_t localTimelineFrame = qMax<int64_t>(0, playheadTimelineFrame - clip.startFrame);
    const int64_t localSourceFrame =
        qMax<int64_t>(0, playheadSourceFrame - qMax<int64_t>(0, clip.sourceInFrame));
    const int64_t lookupFrame = facedetectionsLookupFrameForDomain(
        track.frameDomain, localTimelineFrame, localSourceFrame, playheadSourceFrame);

    const auto nextIt = std::lower_bound(
        track.keyframes.constBegin(),
        track.keyframes.constEnd(),
        lookupFrame,
        [](const FacestreamResolvedKeyframe& keyframe, int64_t frame) {
            return keyframe.frame < frame;
        });
    const FacestreamResolvedKeyframe* previous =
        (nextIt != track.keyframes.constBegin()) ? &(*(nextIt - 1)) : nullptr;
    const FacestreamResolvedKeyframe* next =
        (nextIt != track.keyframes.constEnd()) ? &(*nextIt) : nullptr;
    const int64_t edgeHoldFrames = facedetectionsMaxEdgeHoldFrames(track.typicalFrameStep);

    FacestreamResolvedSelection selection;
    selection.lookupFrame = lookupFrame;
    if (next && next->frame == lookupFrame) {
        selection.keyframe = *next;
    } else if (previous && previous->frame == lookupFrame) {
        selection.keyframe = *previous;
    } else if (previous && next &&
               facedetectionsShouldBridgeGap(previous->frame, next->frame, track.typicalFrameStep)) {
        const int64_t span = qMax<int64_t>(1, next->frame - previous->frame);
        const qreal t = qBound<qreal>(
            0.0,
            static_cast<qreal>(lookupFrame - previous->frame) / static_cast<qreal>(span),
            1.0);
        selection.keyframe.frame = lookupFrame;
        selection.keyframe.confidence =
            previous->confidence + ((next->confidence - previous->confidence) * t);
        selection.keyframe.source = next->source.isEmpty() ? previous->source : next->source;
        selection.interpolated = true;
        if (previous->hasCenterBox && next->hasCenterBox) {
            selection.keyframe.hasCenterBox = true;
            selection.keyframe.xNorm = previous->xNorm + ((next->xNorm - previous->xNorm) * t);
            selection.keyframe.yNorm = previous->yNorm + ((next->yNorm - previous->yNorm) * t);
            selection.keyframe.boxSizeNorm =
                previous->boxSizeNorm + ((next->boxSizeNorm - previous->boxSizeNorm) * t);
        } else if (previous->boxNorm.isValid() && !previous->boxNorm.isEmpty() &&
                   next->boxNorm.isValid() && !next->boxNorm.isEmpty()) {
            selection.keyframe.boxNorm = QRectF(
                previous->boxNorm.x() + ((next->boxNorm.x() - previous->boxNorm.x()) * t),
                previous->boxNorm.y() + ((next->boxNorm.y() - previous->boxNorm.y()) * t),
                previous->boxNorm.width() + ((next->boxNorm.width() - previous->boxNorm.width()) * t),
                previous->boxNorm.height() + ((next->boxNorm.height() - previous->boxNorm.height()) * t))
                                            .intersected(QRectF(0.0, 0.0, 1.0, 1.0));
            if (!selection.keyframe.boxNorm.isValid() || selection.keyframe.boxNorm.isEmpty()) {
                return false;
            }
        } else {
            return false;
        }
    } else if (previous && qAbs(previous->frame - lookupFrame) <= edgeHoldFrames) {
        selection.keyframe = *previous;
    } else if (next && qAbs(next->frame - lookupFrame) <= edgeHoldFrames) {
        selection.keyframe = *next;
    } else {
        return false;
    }

    selection.sourceFrame = mapFacestreamFrameToSourceFrame(
        clip, selection.keyframe.frame, track.frameDomain, markers);
    *selectionOut = selection;
    return true;
}
