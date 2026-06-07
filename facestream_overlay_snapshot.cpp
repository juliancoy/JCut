#include "facestream_overlay_snapshot.h"

#include <QElapsedTimer>
#include <QJsonArray>

#include <algorithm>
#include <limits>

namespace jcut::preview_overlay {

namespace {

void appendUniqueIndices(QVector<int>& destination, const QVector<int>& source)
{
    for (int index : source) {
        if (index >= 0 && !destination.contains(index)) {
            destination.push_back(index);
        }
    }
}

} // namespace

QRectF facestreamKeyframeBoxNorm(const FacestreamResolvedKeyframe& keyframe,
                                 const QSize& clipFrameSize)
{
    if (keyframe.hasCenterBox) {
        if (!clipFrameSize.isValid()) {
            return QRectF();
        }
        return normalizedCenterBoxRect(
            keyframe.xNorm,
            keyframe.yNorm,
            keyframe.boxSizeNorm,
            QSizeF(clipFrameSize.width(), clipFrameSize.height()));
    }
    return keyframe.boxNorm;
}

QVector<VulkanPreviewFacestreamOverlay> rawDetectionsFromCacheEntry(
    const FacestreamOverlayCacheEntry& entry,
    int64_t sourceFrame)
{
    const auto exact = entry.rawDetectionsBySourceFrame.constFind(sourceFrame);
    if (exact != entry.rawDetectionsBySourceFrame.constEnd()) {
        return exact.value();
    }
    if (entry.rawDetectionSourceFrames.isEmpty()) {
        return {};
    }

    const auto nextIt = std::lower_bound(
        entry.rawDetectionSourceFrames.constBegin(),
        entry.rawDetectionSourceFrames.constEnd(),
        sourceFrame);
    const int64_t typicalStep = qMax<int64_t>(1, entry.rawDetectionTypicalFrameStep);
    const int64_t edgeHoldFrames = facedetectionsMaxEdgeHoldFrames(typicalStep);
    const int64_t* previous =
        (nextIt != entry.rawDetectionSourceFrames.constBegin()) ? &(*(nextIt - 1)) : nullptr;
    const int64_t* next =
        (nextIt != entry.rawDetectionSourceFrames.constEnd()) ? &(*nextIt) : nullptr;

    if (previous && next && facedetectionsShouldBridgeGap(*previous, *next, typicalStep)) {
        const int64_t previousDistance = qAbs(sourceFrame - *previous);
        const int64_t nextDistance = qAbs(*next - sourceFrame);
        return entry.rawDetectionsBySourceFrame.value(previousDistance <= nextDistance ? *previous : *next);
    }
    if (previous && qAbs(sourceFrame - *previous) <= edgeHoldFrames) {
        return entry.rawDetectionsBySourceFrame.value(*previous);
    }
    if (next && qAbs(*next - sourceFrame) <= edgeHoldFrames) {
        return entry.rawDetectionsBySourceFrame.value(*next);
    }
    return {};
}

void buildFacestreamTrackCandidateIndex(FacestreamOverlayCacheEntry& entry,
                                        const TimelineClip& clip,
                                        const QVector<RenderSyncMarker>& renderSyncMarkers)
{
    entry.trackIndicesBySourceFrame.clear();
    entry.trackIndexSourceFrames.clear();
    entry.trackIndexTypicalFrameStep = 1;
    if (entry.tracks.isEmpty()) {
        return;
    }

    for (int trackIndex = 0; trackIndex < entry.tracks.size(); ++trackIndex) {
        const FacestreamResolvedTrack& track = entry.tracks.at(trackIndex);
        if (!isSourceMediaFacestreamFrameDomain(track.frameDomain)) {
            continue;
        }
        for (const FacestreamResolvedKeyframe& keyframe : track.keyframes) {
            const int64_t sourceFrame = mapFacestreamFrameToSourceFrame(
                clip, keyframe.frame, track.frameDomain, renderSyncMarkers);
            if (sourceFrame < 0) {
                continue;
            }
            QVector<int>& indices = entry.trackIndicesBySourceFrame[sourceFrame];
            if (!indices.contains(trackIndex)) {
                indices.push_back(trackIndex);
            }
        }
    }

    entry.trackIndexSourceFrames.reserve(entry.trackIndicesBySourceFrame.size());
    for (auto it = entry.trackIndicesBySourceFrame.constBegin();
         it != entry.trackIndicesBySourceFrame.constEnd();
         ++it) {
        entry.trackIndexSourceFrames.push_back(it.key());
    }
    std::sort(entry.trackIndexSourceFrames.begin(), entry.trackIndexSourceFrames.end());
    entry.trackIndexTypicalFrameStep = facedetectionsTypicalFrameStep(entry.trackIndexSourceFrames);
}

QVector<int> facestreamTrackCandidateIndicesFromCacheEntry(
    const FacestreamOverlayCacheEntry& entry,
    int64_t sourceFrame)
{
    if (entry.trackIndexSourceFrames.isEmpty()) {
        QVector<int> allIndices;
        allIndices.reserve(entry.tracks.size());
        for (int i = 0; i < entry.tracks.size(); ++i) {
            allIndices.push_back(i);
        }
        return allIndices;
    }

    const auto exact = entry.trackIndicesBySourceFrame.constFind(sourceFrame);
    if (exact != entry.trackIndicesBySourceFrame.constEnd()) {
        return exact.value();
    }

    const auto nextIt = std::lower_bound(
        entry.trackIndexSourceFrames.constBegin(),
        entry.trackIndexSourceFrames.constEnd(),
        sourceFrame);
    const int64_t typicalStep = qMax<int64_t>(1, entry.trackIndexTypicalFrameStep);
    const int64_t edgeHoldFrames = facedetectionsMaxEdgeHoldFrames(typicalStep);
    const int64_t* previous =
        (nextIt != entry.trackIndexSourceFrames.constBegin()) ? &(*(nextIt - 1)) : nullptr;
    const int64_t* next =
        (nextIt != entry.trackIndexSourceFrames.constEnd()) ? &(*nextIt) : nullptr;

    QVector<int> candidates;
    if (previous && next && facedetectionsShouldBridgeGap(*previous, *next, typicalStep)) {
        appendUniqueIndices(candidates, entry.trackIndicesBySourceFrame.value(*previous));
        appendUniqueIndices(candidates, entry.trackIndicesBySourceFrame.value(*next));
        return candidates;
    }
    if (previous && qAbs(sourceFrame - *previous) <= edgeHoldFrames) {
        return entry.trackIndicesBySourceFrame.value(*previous);
    }
    if (next && qAbs(*next - sourceFrame) <= edgeHoldFrames) {
        return entry.trackIndicesBySourceFrame.value(*next);
    }
    return {};
}

FacestreamOverlaySnapshot buildFacestreamOverlaySnapshot(
    const FacestreamOverlaySnapshotRequest& request)
{
    QElapsedTimer timer;
    timer.start();

    FacestreamOverlaySnapshot snapshot;
    snapshot.requestId = request.requestId;
    snapshot.requestKey = request.requestKey;
    snapshot.currentSample = request.currentSample;
    snapshot.currentFrame = request.currentFrame;
    snapshot.requestClipCount = request.clips.size();

    for (const FacestreamOverlayRequestClip& requestClip : request.clips) {
        const TimelineClip& clip = requestClip.clip;
        const QVector<FacestreamResolvedTrack>& tracks = requestClip.cacheEntry.tracks;
        const bool needsTrackOverlays =
            request.showSpeakerTrackBoxes || request.assignmentInteractionEnabled;
        QVector<int> trackCandidateIndices;
        if (needsTrackOverlays) {
            trackCandidateIndices =
                facestreamTrackCandidateIndicesFromCacheEntry(requestClip.cacheEntry, requestClip.localFrame);
            snapshot.trackCandidateCount += trackCandidateIndices.size();
        }

        if (request.showRawDetections) {
            const QVector<VulkanPreviewFacestreamOverlay> clipDetections =
                rawDetectionsFromCacheEntry(requestClip.cacheEntry, requestClip.localFrame);
            for (const VulkanPreviewFacestreamOverlay& detection : clipDetections) {
                if (detection.boxNorm.isValid() && !detection.boxNorm.isEmpty()) {
                    snapshot.rawDetections.push_back(detection);
                }
            }
            if (clip.id == request.selectedClipId) {
                snapshot.rawDetectionMatchCount = clipDetections.size();
            }
        }

        int selectedClipOverlayCount = 0;
        QJsonArray selectedClipCandidateTrackIds;
        QJsonArray selectedClipOverlayTrackIds;
        if (needsTrackOverlays) {
            for (int trackIndex : trackCandidateIndices) {
                if (trackIndex < 0 || trackIndex >= tracks.size()) {
                    continue;
                }
                const FacestreamResolvedTrack& track = tracks.at(trackIndex);
                if (!isSourceMediaFacestreamFrameDomain(track.frameDomain)) {
                    continue;
                }
                if (clip.id == request.selectedClipId) {
                    selectedClipCandidateTrackIds.push_back(track.trackId);
                }
                if (track.keyframes.isEmpty()) {
                    continue;
                }
                if (!facedetectionsOverlaySourceMatches(request.sourceFilter, track.source, track.streamId)) {
                    continue;
                }
                FacestreamResolvedSelection selection;
                if (!resolveFacestreamTrackAtPlayhead(
                        clip,
                        track,
                        requestClip.renderSyncMarkers,
                        clip.startFrame + requestClip.localTimelineFrame,
                        requestClip.localFrame,
                        &selection)) {
                    continue;
                }

                VulkanPreviewFacestreamOverlay overlay;
                overlay.clipId = clip.id;
                overlay.streamId = track.streamId;
                overlay.source = selection.keyframe.source.isEmpty() ? track.source : selection.keyframe.source;
                overlay.trackId = track.trackId;
                overlay.sourceFrame = selection.sourceFrame;
                overlay.boxNorm = facestreamKeyframeBoxNorm(selection.keyframe, requestClip.clipFrameSize);
                if (!overlay.boxNorm.isValid() || overlay.boxNorm.isEmpty()) {
                    continue;
                }
                overlay.confidence = selection.keyframe.confidence;
                snapshot.overlays.push_back(overlay);
                ++selectedClipOverlayCount;
                if (clip.id == request.selectedClipId) {
                    selectedClipOverlayTrackIds.push_back(track.trackId);
                }
            }
        }

        if (clip.id == request.selectedClipId) {
            snapshot.overlayMatchCount = selectedClipOverlayCount;
            snapshot.debug = QJsonObject{
                {QStringLiteral("status"), QStringLiteral("playback_worker_overlay")},
                {QStringLiteral("clip_id"), clip.id},
                {QStringLiteral("playback_active"), true},
                {QStringLiteral("cached_track_count"), tracks.size()},
                {QStringLiteral("cached_track_index_source_frame_count"),
                 requestClip.cacheEntry.trackIndexSourceFrames.size()},
                {QStringLiteral("cached_raw_detection_frame_count"),
                 requestClip.cacheEntry.rawDetectionSourceFrames.size()},
                {QStringLiteral("requested_source_frame"), static_cast<qint64>(requestClip.localFrame)},
                {QStringLiteral("selected_clip_source_frame"), static_cast<qint64>(requestClip.localFrame)},
                {QStringLiteral("selected_clip_local_source_frame"),
                 static_cast<qint64>(requestClip.localSourceFrame)},
                {QStringLiteral("selected_clip_local_timeline_frame"),
                 static_cast<qint64>(requestClip.localTimelineFrame)},
                {QStringLiteral("selected_clip_track_candidates"), trackCandidateIndices.size()},
                {QStringLiteral("selected_clip_candidate_track_ids"), selectedClipCandidateTrackIds},
                {QStringLiteral("selected_clip_overlay_matches"), selectedClipOverlayCount},
                {QStringLiteral("selected_clip_overlay_track_ids"), selectedClipOverlayTrackIds},
                {QStringLiteral("selected_clip_raw_detection_matches"), snapshot.rawDetectionMatchCount},
                {QStringLiteral("show_speaker_track_boxes"), request.showSpeakerTrackBoxes},
                {QStringLiteral("show_raw_detections"), request.showRawDetections},
                {QStringLiteral("overlay_source_filter"), request.sourceFilter},
                {QStringLiteral("assignment_interaction_enabled"), request.assignmentInteractionEnabled},
                {QStringLiteral("worker_thread"), true}
            };
        }
    }

    snapshot.prepMs = timer.elapsed();
    if (snapshot.debug.isEmpty()) {
        snapshot.debug = QJsonObject{
            {QStringLiteral("status"), QStringLiteral("playback_worker_overlay")},
            {QStringLiteral("playback_active"), true},
            {QStringLiteral("request_clip_count"), snapshot.requestClipCount},
            {QStringLiteral("worker_thread"), true}
        };
    }
    snapshot.debug.insert(QStringLiteral("worker_prep_ms"), snapshot.prepMs);
    return snapshot;
}

FacestreamOverlaySnapshotApplyDecision facestreamOverlaySnapshotApplyDecision(
    bool playbackActive,
    const QString& snapshotKey,
    const QString& expectedKey,
    uint64_t snapshotRequestId,
    uint64_t latestAppliedRequestId)
{
    if (!playbackActive) {
        return FacestreamOverlaySnapshotApplyDecision::DropPaused;
    }
    if (snapshotKey != expectedKey) {
        return FacestreamOverlaySnapshotApplyDecision::DropStaleKey;
    }
    if (snapshotRequestId < latestAppliedRequestId) {
        return FacestreamOverlaySnapshotApplyDecision::DropStaleRequestId;
    }
    return FacestreamOverlaySnapshotApplyDecision::Apply;
}

} // namespace jcut::preview_overlay
