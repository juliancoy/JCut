#include <QtTest/QtTest>

#include "facestream_overlay_snapshot.h"

using jcut::preview_overlay::FacestreamOverlayCacheEntry;
using jcut::preview_overlay::FacestreamOverlayRequestClip;
using jcut::preview_overlay::FacestreamOverlaySnapshotRequest;
using jcut::preview_overlay::FacestreamOverlaySnapshotApplyDecision;
using jcut::preview_overlay::buildFacestreamTrackCandidateIndex;
using jcut::preview_overlay::buildFacestreamOverlaySnapshot;
using jcut::preview_overlay::facestreamTrackCandidateIndicesFromCacheEntry;
using jcut::preview_overlay::facestreamOverlaySnapshotApplyDecision;
using jcut::preview_overlay::rawDetectionsFromCacheEntry;

class TestFacestreamOverlaySnapshot : public QObject {
    Q_OBJECT

private slots:
    void buildSnapshotPreservesDeterministicTrackOrder();
    void assignmentInteractionBuildsOverlaysWithoutVisibleTrackBoxes();
    void sourceFilterExcludesNonMatchingTracks();
    void indexedTrackCandidatesAvoidFullCacheScan();
    void rawOnlySnapshotDoesNotBuildTrackBoxes();
    void rawDetectionsUseSameBridgeAndEdgeHoldRules();
    void applyDecisionDropsPausedStaleAndOutOfOrderSnapshots();
};

namespace {

TimelineClip makeClip()
{
    TimelineClip clip;
    clip.id = QStringLiteral("clip-a");
    clip.filePath = QStringLiteral("/tmp/clip-a.mp4");
    clip.mediaType = ClipMediaType::Video;
    clip.startFrame = 100;
    clip.durationFrames = 300;
    clip.sourceInFrame = 0;
    clip.sourceDurationFrames = 300;
    return clip;
}

FacestreamResolvedKeyframe keyframe(int64_t frame, qreal x, qreal y, const QString& source)
{
    FacestreamResolvedKeyframe key;
    key.frame = frame;
    key.xNorm = x;
    key.yNorm = y;
    key.boxSizeNorm = 0.20;
    key.hasCenterBox = true;
    key.confidence = 0.75;
    key.source = source;
    return key;
}

FacestreamResolvedTrack track(int id, const QString& source)
{
    FacestreamResolvedTrack t;
    t.streamId = QStringLiteral("T%1").arg(id);
    t.trackId = id;
    t.source = source;
    t.frameDomain = FacestreamFrameDomain::SourceAbsolute;
    t.typicalFrameStep = 4;
    t.keyframes = QVector<FacestreamResolvedKeyframe>{
        keyframe(4, 0.25 + (id * 0.01), 0.40, source),
        keyframe(8, 0.35 + (id * 0.01), 0.45, source)
    };
    return t;
}

FacestreamResolvedTrack trackAtFrames(int id, int64_t firstFrame, int64_t secondFrame)
{
    FacestreamResolvedTrack t = track(id, QStringLiteral("manual"));
    t.keyframes = QVector<FacestreamResolvedKeyframe>{
        keyframe(firstFrame, 0.25, 0.40, QStringLiteral("manual")),
        keyframe(secondFrame, 0.35, 0.45, QStringLiteral("manual"))
    };
    return t;
}

VulkanPreviewFacestreamOverlay rawDetection(int64_t sourceFrame, int trackId)
{
    VulkanPreviewFacestreamOverlay overlay;
    overlay.clipId = QStringLiteral("clip-a");
    overlay.streamId = QStringLiteral("raw_detection");
    overlay.source = QStringLiteral("raw_detection");
    overlay.trackId = trackId;
    overlay.sourceFrame = sourceFrame;
    overlay.boxNorm = QRectF(0.1, 0.2, 0.3, 0.4);
    overlay.confidence = 0.9;
    return overlay;
}

FacestreamOverlayRequestClip requestClip(FacestreamOverlayCacheEntry cacheEntry)
{
    FacestreamOverlayRequestClip request;
    request.clip = makeClip();
    request.localFrame = 6;
    request.localSourceFrame = 6;
    request.localTimelineFrame = 6;
    request.clipFrameSize = QSize(1920, 1080);
    request.cacheEntry = cacheEntry;
    return request;
}

FacestreamOverlaySnapshotRequest snapshotRequest(FacestreamOverlayCacheEntry cacheEntry,
                                                 const QString& sourceFilter = QStringLiteral("all"))
{
    FacestreamOverlaySnapshotRequest request;
    request.requestId = 42;
    request.requestKey = QStringLiteral("sample-6");
    request.currentSample = frameToSamples(106);
    request.currentFrame = 106;
    request.clips = QVector<FacestreamOverlayRequestClip>{requestClip(cacheEntry)};
    request.selectedClipId = QStringLiteral("clip-a");
    request.sourceFilter = sourceFilter;
    request.showSpeakerTrackBoxes = true;
    request.showRawDetections = true;
    request.assignmentInteractionEnabled = false;
    return request;
}

} // namespace

void TestFacestreamOverlaySnapshot::buildSnapshotPreservesDeterministicTrackOrder()
{
    FacestreamOverlayCacheEntry cache;
    cache.signature = QStringLiteral("sig");
    cache.tracks = QVector<FacestreamResolvedTrack>{
        track(2, QStringLiteral("manual")),
        track(1, QStringLiteral("manual"))
    };

    const auto snapshot = buildFacestreamOverlaySnapshot(snapshotRequest(cache));
    QCOMPARE(snapshot.requestId, 42ULL);
    QCOMPARE(snapshot.requestKey, QStringLiteral("sample-6"));
    QCOMPARE(snapshot.requestClipCount, 1);
    QCOMPARE(snapshot.trackCandidateCount, 2);
    QCOMPARE(snapshot.overlayMatchCount, 2);
    QCOMPARE(snapshot.overlays.size(), 2);
    QCOMPARE(snapshot.overlays.at(0).trackId, 2);
    QCOMPARE(snapshot.overlays.at(1).trackId, 1);
    QCOMPARE(snapshot.debug.value(QStringLiteral("worker_thread")).toBool(), true);
}

void TestFacestreamOverlaySnapshot::assignmentInteractionBuildsOverlaysWithoutVisibleTrackBoxes()
{
    FacestreamOverlayCacheEntry cache;
    cache.tracks = QVector<FacestreamResolvedTrack>{track(7, QStringLiteral("manual"))};

    auto request = snapshotRequest(cache);
    request.showSpeakerTrackBoxes = false;
    request.assignmentInteractionEnabled = true;

    const auto snapshot = buildFacestreamOverlaySnapshot(request);
    QCOMPARE(snapshot.overlays.size(), 1);
    QCOMPARE(snapshot.overlays.first().trackId, 7);
    QCOMPARE(snapshot.debug.value(QStringLiteral("assignment_interaction_enabled")).toBool(), true);
}

void TestFacestreamOverlaySnapshot::sourceFilterExcludesNonMatchingTracks()
{
    FacestreamOverlayCacheEntry cache;
    cache.tracks = QVector<FacestreamResolvedTrack>{
        track(1, QStringLiteral("manual")),
        track(2, QStringLiteral("scrfd"))
    };

    const auto snapshot = buildFacestreamOverlaySnapshot(snapshotRequest(cache, QStringLiteral("manual")));
    QCOMPARE(snapshot.overlays.size(), 1);
    QCOMPARE(snapshot.overlays.first().trackId, 1);
}

void TestFacestreamOverlaySnapshot::indexedTrackCandidatesAvoidFullCacheScan()
{
    FacestreamOverlayCacheEntry cache;
    cache.tracks = QVector<FacestreamResolvedTrack>{
        trackAtFrames(1, 4, 8),
        trackAtFrames(2, 100, 104)
    };
    buildFacestreamTrackCandidateIndex(cache, makeClip(), {});

    const QVector<int> candidates = facestreamTrackCandidateIndicesFromCacheEntry(cache, 6);
    QCOMPARE(candidates.size(), 1);
    QCOMPARE(candidates.first(), 0);

    const auto snapshot = buildFacestreamOverlaySnapshot(snapshotRequest(cache));
    QCOMPARE(snapshot.trackCandidateCount, 1);
    QCOMPARE(snapshot.overlayMatchCount, 1);
    QCOMPARE(snapshot.overlays.size(), 1);
    QCOMPARE(snapshot.overlays.first().trackId, 1);
    QCOMPARE(snapshot.debug.value(QStringLiteral("cached_track_count")).toInt(), 2);
    QCOMPARE(snapshot.debug.value(QStringLiteral("selected_clip_track_candidates")).toInt(), 1);
}

void TestFacestreamOverlaySnapshot::rawOnlySnapshotDoesNotBuildTrackBoxes()
{
    FacestreamOverlayCacheEntry cache;
    cache.tracks = QVector<FacestreamResolvedTrack>{trackAtFrames(1, 4, 8)};
    cache.rawDetectionsBySourceFrame.insert(6, QVector<VulkanPreviewFacestreamOverlay>{rawDetection(6, 100)});
    cache.rawDetectionSourceFrames = QVector<int64_t>{6};
    buildFacestreamTrackCandidateIndex(cache, makeClip(), {});

    auto request = snapshotRequest(cache);
    request.showSpeakerTrackBoxes = false;
    request.assignmentInteractionEnabled = false;
    request.showRawDetections = true;

    const auto snapshot = buildFacestreamOverlaySnapshot(request);
    QCOMPARE(snapshot.trackCandidateCount, 0);
    QCOMPARE(snapshot.overlayMatchCount, 0);
    QCOMPARE(snapshot.overlays.size(), 0);
    QCOMPARE(snapshot.rawDetections.size(), 1);
    QCOMPARE(snapshot.debug.value(QStringLiteral("selected_clip_track_candidates")).toInt(), 0);
}

void TestFacestreamOverlaySnapshot::rawDetectionsUseSameBridgeAndEdgeHoldRules()
{
    FacestreamOverlayCacheEntry cache;
    cache.rawDetectionsBySourceFrame.insert(4, QVector<VulkanPreviewFacestreamOverlay>{rawDetection(4, 100)});
    cache.rawDetectionsBySourceFrame.insert(8, QVector<VulkanPreviewFacestreamOverlay>{rawDetection(8, 200)});
    cache.rawDetectionSourceFrames = QVector<int64_t>{4, 8};
    cache.rawDetectionTypicalFrameStep = 4;

    const QVector<VulkanPreviewFacestreamOverlay> bridged = rawDetectionsFromCacheEntry(cache, 6);
    QCOMPARE(bridged.size(), 1);
    QCOMPARE(bridged.first().sourceFrame, 4LL);
    QCOMPARE(bridged.first().trackId, 100);

    const QVector<VulkanPreviewFacestreamOverlay> edgeHeld = rawDetectionsFromCacheEntry(cache, 10);
    QCOMPARE(edgeHeld.size(), 1);
    QCOMPARE(edgeHeld.first().sourceFrame, 8LL);

    QVERIFY(rawDetectionsFromCacheEntry(cache, 14).isEmpty());
}

void TestFacestreamOverlaySnapshot::applyDecisionDropsPausedStaleAndOutOfOrderSnapshots()
{
    QCOMPARE(facestreamOverlaySnapshotApplyDecision(false,
                                                   QStringLiteral("sample-8"),
                                                   QStringLiteral("sample-8"),
                                                   10,
                                                   10),
             FacestreamOverlaySnapshotApplyDecision::DropPaused);
    QCOMPARE(facestreamOverlaySnapshotApplyDecision(true,
                                                   QStringLiteral("sample-4"),
                                                   QStringLiteral("sample-8"),
                                                   10,
                                                   10),
             FacestreamOverlaySnapshotApplyDecision::DropStaleKey);
    QCOMPARE(facestreamOverlaySnapshotApplyDecision(true,
                                                   QStringLiteral("sample-8"),
                                                   QStringLiteral("sample-8"),
                                                   9,
                                                   10),
             FacestreamOverlaySnapshotApplyDecision::DropStaleRequestId);
    QCOMPARE(facestreamOverlaySnapshotApplyDecision(true,
                                                   QStringLiteral("sample-8"),
                                                   QStringLiteral("sample-8"),
                                                   10,
                                                   10),
             FacestreamOverlaySnapshotApplyDecision::Apply);
    QCOMPARE(facestreamOverlaySnapshotApplyDecision(true,
                                                   QStringLiteral("sample-8"),
                                                   QStringLiteral("sample-8"),
                                                   11,
                                                   10),
             FacestreamOverlaySnapshotApplyDecision::Apply);
}

QTEST_MAIN(TestFacestreamOverlaySnapshot)
#include "test_facestream_overlay_snapshot.moc"
