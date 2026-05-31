#include "facestream_overlay_snapshot.h"

#include <QElapsedTimer>

#include <algorithm>

namespace jcut::preview_overlay {

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
        snapshot.trackCandidateCount += tracks.size();

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
        if (request.showSpeakerTrackBoxes || request.assignmentInteractionEnabled) {
            for (const FacestreamResolvedTrack& track : tracks) {
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
            }
        }

        if (clip.id == request.selectedClipId) {
            snapshot.overlayMatchCount = selectedClipOverlayCount;
            snapshot.debug = QJsonObject{
                {QStringLiteral("status"), QStringLiteral("playback_worker_overlay")},
                {QStringLiteral("clip_id"), clip.id},
                {QStringLiteral("playback_active"), true},
                {QStringLiteral("cached_track_count"), tracks.size()},
                {QStringLiteral("cached_raw_detection_frame_count"),
                 requestClip.cacheEntry.rawDetectionSourceFrames.size()},
                {QStringLiteral("requested_source_frame"), static_cast<qint64>(requestClip.localFrame)},
                {QStringLiteral("selected_clip_source_frame"), static_cast<qint64>(requestClip.localFrame)},
                {QStringLiteral("selected_clip_local_source_frame"),
                 static_cast<qint64>(requestClip.localSourceFrame)},
                {QStringLiteral("selected_clip_local_timeline_frame"),
                 static_cast<qint64>(requestClip.localTimelineFrame)},
                {QStringLiteral("selected_clip_track_candidates"), tracks.size()},
                {QStringLiteral("selected_clip_overlay_matches"), selectedClipOverlayCount},
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

} // namespace jcut::preview_overlay
