#include "editor_shared_render_sync.h"
#include "editor_shared_timing.h"

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
    bool initialized = false;
    QVector<RenderSyncMarker> markers;
    QHash<QString, RenderSyncClipLookup> byClip;
};

bool renderSyncMarkersEqual(const QVector<RenderSyncMarker>& left,
                            const QVector<RenderSyncMarker>& right)
{
    if (left.size() != right.size()) {
        return false;
    }
    for (qsizetype index = 0; index < left.size(); ++index) {
        const RenderSyncMarker& leftMarker = left.at(index);
        const RenderSyncMarker& rightMarker = right.at(index);
        if (leftMarker.clipId != rightMarker.clipId ||
            leftMarker.frame != rightMarker.frame ||
            leftMarker.count != rightMarker.count ||
            leftMarker.action != rightMarker.action) {
            return false;
        }
    }
    return true;
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
    if (!cache.initialized || !renderSyncMarkersEqual(cache.markers, markers)) {
        cache.markers = markers;
        // QVector is implicitly shared. Detach so even callers that retain and
        // mutate a raw element pointer cannot mutate the cache's comparison
        // snapshot along with the source vector.
        cache.markers.detach();
        cache.initialized = true;
        rebuildRenderSyncLookupCache(cache, markers);
    }
    auto it = cache.byClip.constFind(clipId);
    if (it == cache.byClip.constEnd()) {
        return nullptr;
    }
    return &it.value();
}
}

RenderFrameClock renderFrameClockForTimelinePosition(qreal timelineFramePosition)
{
    RenderFrameClock clock;
    clock.timelineFramePosition = std::isfinite(timelineFramePosition)
        ? qMax<qreal>(0.0, timelineFramePosition)
        : 0.0;
    clock.timelineSample = framePositionToSamples(clock.timelineFramePosition);
    clock.timelineFrame =
        qMax<int64_t>(0, static_cast<int64_t>(std::floor(clock.timelineFramePosition)));
    return clock;
}

RenderFrameClock renderFrameClockForTimelineSample(int64_t timelineSample)
{
    RenderFrameClock clock;
    clock.timelineSample = qMax<int64_t>(0, timelineSample);
    clock.timelineFramePosition = samplesToFramePosition(clock.timelineSample);
    clock.timelineFrame =
        qMax<int64_t>(0, static_cast<int64_t>(std::floor(clock.timelineFramePosition)));
    return clock;
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

qreal sourceFramePositionForClipAtTimelinePosition(const TimelineClip& clip,
                                                  qreal timelineFramePosition,
                                                  const QVector<RenderSyncMarker>& markers) {
    return sourceFramePositionForClipAtTimelineSample(
        clip,
        framePositionToSamples(timelineFramePosition),
        markers);
}

int64_t sourceFrameForClipAtTimelinePosition(const TimelineClip& clip,
                                             qreal timelineFramePosition,
                                             const QVector<RenderSyncMarker>& markers) {
    return qMax<int64_t>(
        0,
        static_cast<int64_t>(
            std::floor(sourceFramePositionForClipAtTimelinePosition(
                clip, timelineFramePosition, markers))));
}

int64_t approximateTimelineFrameForClipSourceFrame(const TimelineClip& clip,
                                                   int64_t sourceFrame) {
    const qreal sourceFps = resolvedSourceFps(clip);
    const qreal playbackRate = qMax<qreal>(0.001, clip.playbackRate);
    const qreal sourceOffset =
        static_cast<qreal>(qMax<int64_t>(0, sourceFrame - clip.sourceInFrame));
    const qreal localTimeline =
        sourceOffset * static_cast<qreal>(kTimelineFps) / (playbackRate * sourceFps);
    const qreal maxLocalTimeline = static_cast<qreal>(qMax<int64_t>(0, clip.durationFrames - 1));
    return clip.startFrame +
           static_cast<int64_t>(std::floor(qBound<qreal>(0.0, localTimeline, maxLocalTimeline)));
}

int64_t sourceSampleForClipAtTimelineSample(const TimelineClip& clip,
                                            int64_t timelineSample,
                                            const QVector<RenderSyncMarker>& markers) {
    const int64_t clipStartSample = clipTimelineStartSamples(clip);
    const int64_t localTimelineSample = qMax<int64_t>(0, timelineSample - clipStartSample);
    const int64_t maxLocalTimelineSample =
        qMax<int64_t>(0, clipTimelineDurationSamples(clip) - 1);
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

qreal sourceFramePositionForClipAtTimelineSample(const TimelineClip& clip,
                                                 int64_t timelineSample,
                                                 const QVector<RenderSyncMarker>& markers) {
    const int64_t sourceSample =
        sourceSampleForClipAtTimelineSample(clip, timelineSample, markers);
    const qreal sourceSeconds =
        static_cast<qreal>(sourceSample) / static_cast<qreal>(kAudioSampleRate);
    const qreal maxSourceFrame =
        static_cast<qreal>(qMax<int64_t>(0, clip.sourceDurationFrames - 1));
    return qBound<qreal>(0.0,
                         sourceSeconds * resolvedSourceFps(clip),
                         maxSourceFrame);
}

ClipFrameMapping clipFrameMappingForClock(const TimelineClip& clip,
                                          const RenderFrameClock& clock,
                                          const QVector<RenderSyncMarker>& markers)
{
    ClipFrameMapping mapping;
    mapping.clock = clock;
    mapping.sourceSample =
        sourceSampleForClipAtTimelineSample(clip, clock.timelineSample, markers);
    mapping.sourceFramePosition =
        sourceFramePositionForClipAtTimelineSample(clip, clock.timelineSample, markers);
    mapping.sourceFrame =
        qMax<int64_t>(0, static_cast<int64_t>(std::floor(mapping.sourceFramePosition)));
    mapping.transcriptFrame =
        transcriptFrameForClipAtTimelineSample(clip, clock.timelineSample, markers);
    return mapping;
}

const TimelineClip& resolvedClipTimingSource(
    const TimelineClip& clip,
    const QVector<TimelineClip>& timelineClips)
{
    // Mask mattes are generated followers by definition. Treat the role as a
    // timing lock as well so legacy documents that predate the persisted lock
    // flag cannot silently acquire an independent export clock.
    if ((!clip.syncLockedToSource && clip.clipRole != ClipRole::MaskMatte) ||
        clip.linkedSourceClipId.trimmed().isEmpty()) {
        return clip;
    }

    const QString sourceId = clip.linkedSourceClipId.trimmed();
    for (const TimelineClip& candidate : timelineClips) {
        if (&candidate != &clip && candidate.id.trimmed() == sourceId) {
            return candidate;
        }
    }
    return clip;
}

TimelineClip clipWithResolvedTimingOwner(
    const TimelineClip& clip,
    const QVector<TimelineClip>& timelineClips)
{
    const TimelineClip& timingOwner = resolvedClipTimingSource(clip, timelineClips);
    if (&timingOwner == &clip) {
        return clip;
    }

    TimelineClip resolved = clip;
    resolved.id = timingOwner.id;
    resolved.startFrame = timingOwner.startFrame;
    resolved.startSubframeSamples = timingOwner.startSubframeSamples;
    resolved.durationFrames = timingOwner.durationFrames;
    resolved.durationSubframeSamples = timingOwner.durationSubframeSamples;
    resolved.sourceFps = timingOwner.sourceFps;
    resolved.sourceDurationFrames = timingOwner.sourceDurationFrames;
    resolved.sourceInFrame = timingOwner.sourceInFrame;
    resolved.sourceInSubframeSamples = timingOwner.sourceInSubframeSamples;
    resolved.playbackRate = timingOwner.playbackRate;
    return resolved;
}

ClipFrameMapping clipFrameMappingForClock(
    const TimelineClip& clip,
    const QVector<TimelineClip>& timelineClips,
    const RenderFrameClock& clock,
    const QVector<RenderSyncMarker>& markers)
{
    return clipFrameMappingForClock(
        resolvedClipTimingSource(clip, timelineClips), clock, markers);
}

int64_t requestedSourceFrameForGeneratedMaskPreview(
    const TimelineClip& clip,
    const QVector<TimelineClip>& timelineClips,
    qreal timelineFramePosition,
    const QVector<RenderSyncMarker>& markers)
{
    return clipFrameMappingForClock(
               clip,
               timelineClips,
               renderFrameClockForTimelinePosition(timelineFramePosition),
               markers)
        .sourceFrame;
}

int64_t sourceFrameForClipAtTimelineSample(const TimelineClip& clip,
                                           int64_t timelineSample,
                                           const QVector<RenderSyncMarker>& markers) {
    return qMax<int64_t>(
        0,
        static_cast<int64_t>(std::floor(
            sourceFramePositionForClipAtTimelineSample(clip, timelineSample, markers))));
}

int64_t transcriptFrameForClipSourceFrame(const TimelineClip& clip,
                                          int64_t mediaSourceFrame) {
    const qreal fps = qMax<qreal>(1.0, resolvedSourceFps(clip));
    const qreal sourceSeconds =
        static_cast<qreal>(qMax<int64_t>(0, mediaSourceFrame)) / fps;
    return qMax<int64_t>(
        0,
        static_cast<int64_t>(std::floor(sourceSeconds * static_cast<qreal>(kTimelineFps))));
}

int64_t transcriptFrameForClipAtTimelineSample(const TimelineClip& clip,
                                               int64_t timelineSample,
                                               const QVector<RenderSyncMarker>& markers) {
    const int64_t sourceSample = sourceSampleForClipAtTimelineSample(clip, timelineSample, markers);
    const qreal sourceSeconds =
        static_cast<qreal>(sourceSample) / static_cast<qreal>(kAudioSampleRate);
    return qMax<int64_t>(0, static_cast<int64_t>(std::floor(sourceSeconds * static_cast<qreal>(kTimelineFps))));
}

int64_t timelineFrameForClipTranscriptFrame(const TimelineClip& clip,
                                            int64_t transcriptFrame,
                                            const QVector<RenderSyncMarker>& markers) {
    const int64_t clipStart = clip.startFrame;
    const int64_t clipEnd = clipStart + qMax<int64_t>(0, clip.durationFrames - 1);
    if (clip.durationFrames <= 0 || transcriptFrame <=
            transcriptFrameForClipAtTimelineSample(clip, frameToSamples(clipStart), markers)) {
        return clipStart;
    }
    if (transcriptFrame >=
            transcriptFrameForClipAtTimelineSample(clip, frameToSamples(clipEnd), markers)) {
        return clipEnd;
    }

    // Find the earliest displayed frame whose source clock has reached the
    // requested transcript frame. This preserves exact section boundaries and
    // honors trims, playback rate, source FPS, and render-sync adjustments.
    int64_t low = clipStart;
    int64_t high = clipEnd;
    while (low < high) {
        const int64_t mid = low + (high - low) / 2;
        const int64_t mapped =
            transcriptFrameForClipAtTimelineSample(clip, frameToSamples(mid), markers);
        if (mapped < transcriptFrame) {
            low = mid + 1;
        } else {
            high = mid;
        }
    }
    return low;
}
