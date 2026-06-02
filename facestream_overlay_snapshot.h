#pragma once

#include "facedetections_time_mapping.h"
#include "preview_interaction_state.h"

#include <QJsonObject>
#include <QHash>
#include <QSize>
#include <QString>
#include <QVector>

namespace jcut::preview_overlay {

struct FacestreamOverlayCacheEntry {
    QString signature;
    QVector<FacestreamResolvedTrack> tracks;
    QHash<int64_t, QVector<int>> trackIndicesBySourceFrame;
    QVector<int64_t> trackIndexSourceFrames;
    int64_t trackIndexTypicalFrameStep = 1;
    QVector<VulkanPreviewFacestreamOverlay> rawDetections;
    QHash<int64_t, QVector<VulkanPreviewFacestreamOverlay>> rawDetectionsBySourceFrame;
    QVector<int64_t> rawDetectionSourceFrames;
    int64_t rawDetectionTypicalFrameStep = 1;
};

struct FacestreamOverlayRequestClip {
    TimelineClip clip;
    int64_t localFrame = 0;
    int64_t localSourceFrame = 0;
    int64_t localTimelineFrame = 0;
    QSize clipFrameSize;
    QVector<RenderSyncMarker> renderSyncMarkers;
    FacestreamOverlayCacheEntry cacheEntry;
};

struct FacestreamOverlaySnapshotRequest {
    uint64_t requestId = 0;
    QString requestKey;
    int64_t currentSample = 0;
    int64_t currentFrame = 0;
    QVector<FacestreamOverlayRequestClip> clips;
    QString selectedClipId;
    QString sourceFilter;
    bool showSpeakerTrackBoxes = false;
    bool showRawDetections = false;
    bool assignmentInteractionEnabled = false;
};

struct FacestreamOverlaySnapshot {
    uint64_t requestId = 0;
    QString requestKey;
    int64_t currentSample = 0;
    int64_t currentFrame = 0;
    QVector<VulkanPreviewFacestreamOverlay> overlays;
    QVector<VulkanPreviewFacestreamOverlay> rawDetections;
    QJsonObject debug;
    qint64 prepMs = 0;
    int requestClipCount = 0;
    int trackCandidateCount = 0;
    int overlayMatchCount = 0;
    int rawDetectionMatchCount = 0;
};

enum class FacestreamOverlaySnapshotApplyDecision {
    Apply,
    DropPaused,
    DropStaleKey,
    DropStaleRequestId,
};

QRectF facestreamKeyframeBoxNorm(const FacestreamResolvedKeyframe& keyframe,
                                 const QSize& clipFrameSize);

QVector<VulkanPreviewFacestreamOverlay> rawDetectionsFromCacheEntry(
    const FacestreamOverlayCacheEntry& entry,
    int64_t sourceFrame);

void buildFacestreamTrackCandidateIndex(FacestreamOverlayCacheEntry& entry,
                                        const TimelineClip& clip,
                                        const QVector<RenderSyncMarker>& renderSyncMarkers);

QVector<int> facestreamTrackCandidateIndicesFromCacheEntry(
    const FacestreamOverlayCacheEntry& entry,
    int64_t sourceFrame);

FacestreamOverlaySnapshot buildFacestreamOverlaySnapshot(
    const FacestreamOverlaySnapshotRequest& request);

FacestreamOverlaySnapshotApplyDecision facestreamOverlaySnapshotApplyDecision(
    bool playbackActive,
    const QString& snapshotKey,
    const QString& expectedKey,
    uint64_t snapshotRequestId,
    uint64_t latestAppliedRequestId);

} // namespace jcut::preview_overlay
