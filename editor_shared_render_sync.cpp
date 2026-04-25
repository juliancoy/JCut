#include "editor_shared.h"

#include <QHash>
#include <QtGlobal>

#include <algorithm>
#include <cmath>

namespace {
struct RenderSyncClipLookup {
    QVector<int64_t> timelineFrames;
    QVector<int> cumulativeDelta;
};

struct RenderSyncLookupCache {
    const QVector<RenderSyncMarker>* source = nullptr;
    int size = -1;
    quint64 signature = 0;
    QHash<QString, RenderSyncClipLookup> byClip;
};

quint64 markerQuickSignature(const QVector<RenderSyncMarker>& markers) {
    quint64 sig = static_cast<quint64>(markers.size()) * 1469598103934665603ULL;
    if (markers.isEmpty()) {
        return sig;
    }
    auto mixMarker = [&sig](const RenderSyncMarker& marker) {
        sig ^= static_cast<quint64>(qHash(marker.clipId));
        sig = (sig * 1099511628211ULL) ^ static_cast<quint64>(marker.frame);
        sig = (sig * 1099511628211ULL) ^ static_cast<quint64>(marker.count);
        sig = (sig * 1099511628211ULL) ^ static_cast<quint64>(static_cast<int>(marker.action));
    };
    mixMarker(markers.constFirst());
    if (markers.size() > 1) {
        mixMarker(markers[markers.size() / 2]);
        mixMarker(markers.constLast());
    }
    return sig;
}

int markerDelta(const RenderSyncMarker& marker) {
    const int magnitude = qMax(1, marker.count);
    return marker.action == RenderSyncAction::DuplicateFrame ? -magnitude : magnitude;
}

void rebuildRenderSyncLookupCache(RenderSyncLookupCache& cache,
                                  const QVector<RenderSyncMarker>& markers) {
    cache.byClip.clear();
    if (markers.isEmpty()) {
        return;
    }

    QHash<QString, QVector<RenderSyncMarker>> grouped;
    grouped.reserve(markers.size());
    for (const RenderSyncMarker& marker : markers) {
        grouped[marker.clipId].push_back(marker);
    }

    for (auto it = grouped.begin(); it != grouped.end(); ++it) {
        QVector<RenderSyncMarker>& clipMarkers = it.value();
        std::sort(clipMarkers.begin(), clipMarkers.end(),
                  [](const RenderSyncMarker& a, const RenderSyncMarker& b) {
                      if (a.frame == b.frame) {
                          return static_cast<int>(a.action) < static_cast<int>(b.action);
                      }
                      return a.frame < b.frame;
                  });
        RenderSyncClipLookup lookup;
        lookup.timelineFrames.reserve(clipMarkers.size());
        lookup.cumulativeDelta.reserve(clipMarkers.size());
        int runningDelta = 0;
        for (const RenderSyncMarker& marker : clipMarkers) {
            runningDelta += markerDelta(marker);
            lookup.timelineFrames.push_back(marker.frame);
            lookup.cumulativeDelta.push_back(runningDelta);
        }
        cache.byClip.insert(it.key(), std::move(lookup));
    }
}

const RenderSyncClipLookup* lookupForClip(const QVector<RenderSyncMarker>& markers,
                                          const QString& clipId) {
    thread_local RenderSyncLookupCache cache;
    const quint64 signature = markerQuickSignature(markers);
    if (cache.source != &markers || cache.size != markers.size() || cache.signature != signature) {
        cache.source = &markers;
        cache.size = markers.size();
        cache.signature = signature;
        rebuildRenderSyncLookupCache(cache, markers);
    }
    auto it = cache.byClip.constFind(clipId);
    if (it == cache.byClip.constEnd()) {
        return nullptr;
    }
    return &it.value();
}
}

int64_t adjustedClipLocalFrameAtTimelineFrame(const TimelineClip& clip,
                                              int64_t localTimelineFrame,
                                              const QVector<RenderSyncMarker>& markers) {
    const int64_t boundedLocalFrame = qMax<int64_t>(0, localTimelineFrame);
    const RenderSyncClipLookup* lookup = lookupForClip(markers, clip.id);
    if (!lookup || lookup->timelineFrames.isEmpty()) {
        return boundedLocalFrame;
    }
    const int64_t timelineFrame = clip.startFrame + boundedLocalFrame;
    const auto endIt =
        std::lower_bound(lookup->timelineFrames.begin(), lookup->timelineFrames.end(), timelineFrame);
    if (endIt == lookup->timelineFrames.begin()) {
        return boundedLocalFrame;
    }
    const int index = static_cast<int>(std::distance(lookup->timelineFrames.begin(), endIt) - 1);
    const int delta = lookup->cumulativeDelta[index];
    return qMax<int64_t>(0, boundedLocalFrame + static_cast<int64_t>(delta));
}

int64_t sourceFrameForClipAtTimelinePosition(const TimelineClip& clip,
                                             qreal timelineFramePosition,
                                             const QVector<RenderSyncMarker>& markers) {
    const qreal maxFrame = static_cast<qreal>(qMax<int64_t>(0, clip.durationFrames - 1));
    const qreal localTimelineFramePosition =
        qBound<qreal>(0.0, timelineFramePosition - static_cast<qreal>(clip.startFrame), maxFrame);
    const int64_t steppedLocalTimelineFrame =
        qMax<int64_t>(0, static_cast<int64_t>(std::floor(localTimelineFramePosition)));
    const int64_t adjustedLocalFrame =
        adjustedClipLocalFrameAtTimelineFrame(clip, steppedLocalTimelineFrame, markers);
    
    // Use fixed-point arithmetic for FPS scaling and playback rate
    const qreal sourceFps = resolvedSourceFps(clip);
    const int64_t sourceFpsScaled = qMax<int64_t>(1, static_cast<int64_t>(sourceFps * 1000.0));
    const int64_t timelineFpsScaled = static_cast<int64_t>(kTimelineFps * 1000.0);
    const int64_t playbackRateScaled = qMax<int64_t>(1, static_cast<int64_t>(clip.playbackRate * 1000.0));
    
    // Calculate source frame offset using 64-bit integer arithmetic
    // (adjustedLocalFrame * playbackRate * sourceFps) / timelineFps
    const int64_t numerator = adjustedLocalFrame * playbackRateScaled * sourceFpsScaled;
    const int64_t denominator = timelineFpsScaled * 1000LL; // Extra 1000 for playbackRate scaling
    const int64_t sourceFrameOffset = numerator / denominator;
    
    return qMax<int64_t>(0,
                         qMin<int64_t>(qMax<int64_t>(0, clip.sourceDurationFrames - 1),
                                       clip.sourceInFrame + sourceFrameOffset));
}

int64_t sourceSampleForClipAtTimelineSample(const TimelineClip& clip,
                                            int64_t timelineSample,
                                            const QVector<RenderSyncMarker>& markers) {
    const int64_t clipStartSample = clipTimelineStartSamples(clip);
    const int64_t localTimelineSample = qMax<int64_t>(0, timelineSample - clipStartSample);
    const int64_t maxLocalTimelineSample =
        qMax<int64_t>(0, frameToSamples(qMax<int64_t>(0, clip.durationFrames)) - 1);
    const int64_t boundedLocalTimelineSample = qMin<int64_t>(localTimelineSample, maxLocalTimelineSample);
    
    // Use integer division for frame position to avoid floating-point errors
    const int64_t steppedLocalTimelineFrame = boundedLocalTimelineSample / kSamplesPerFrame;
    const int64_t sampleOffsetWithinFrame = boundedLocalTimelineSample % kSamplesPerFrame;
    
    const int64_t adjustedLocalFrame =
        adjustedClipLocalFrameAtTimelineFrame(clip, steppedLocalTimelineFrame, markers);
    
    // Calculate source sample offset using integer arithmetic
    // Convert adjusted frame to samples, add sample offset, then apply playback rate
    const int64_t adjustedLocalSamples = frameToSamples(adjustedLocalFrame) + sampleOffsetWithinFrame;
    
    // Apply playback rate with fixed-point arithmetic (scaled by 1000 for precision)
    const int64_t playbackRateScaled = qMax<int64_t>(1, static_cast<int64_t>(clip.playbackRate * 1000.0));
    const int64_t sourceSampleOffset = (adjustedLocalSamples * playbackRateScaled) / 1000;
    
    const int64_t sourceSample = clipSourceInSamples(clip) + sourceSampleOffset;
    const int64_t maxSourceSample =
        clipSourceInSamples(clip) +
        qMax<int64_t>(0,
                      sourceFramesToSamples(clip, static_cast<qreal>(qMax<int64_t>(0, clip.sourceDurationFrames))) - 1);
    return qMax<int64_t>(0, qMin<int64_t>(sourceSample, maxSourceSample));
}

int64_t transcriptFrameForClipAtTimelineSample(const TimelineClip& clip,
                                               int64_t timelineSample,
                                               const QVector<RenderSyncMarker>& markers) {
    const int64_t sourceSample = sourceSampleForClipAtTimelineSample(clip, timelineSample, markers);
    const qreal sourceSeconds =
        static_cast<qreal>(sourceSample) / static_cast<qreal>(kAudioSampleRate);
    return qMax<int64_t>(0, static_cast<int64_t>(std::floor(sourceSeconds * static_cast<qreal>(kTimelineFps))));
}
