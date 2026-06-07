#include "facedetections_time_mapping.h"

#include <QtTest/QtTest>

class FacestreamTimeMappingTest : public QObject {
    Q_OBJECT

private slots:
    void sourceAbsoluteScanRangeAddsSourceInFrame()
    {
        TimelineClip clip;
        clip.sourceInFrame = 120;
        clip.sourceDurationFrames = 600;

        const FacestreamSourceScanRange range = facedetectionsSourceAbsoluteScanRangeForClip(clip);
        QVERIFY(range.valid);
        QCOMPARE(range.startFrame, 120LL);
        QCOMPARE(range.endFrameExclusive, 720LL);
        QCOMPARE(range.frameCount, 600LL);
    }

    void ambiguousFrameRangePrefersSourceAbsolute()
    {
        TimelineClip clip;
        clip.startFrame = 0;
        clip.durationFrames = 300;
        clip.sourceInFrame = 0;
        clip.sourceDurationFrames = 600;

        QCOMPARE(inferFacestreamFrameDomain(clip, 0, 300),
                 FacestreamFrameDomain::SourceAbsolute);
    }

    void inferenceFallsBackToSourceRelativeNotTimelineClock()
    {
        TimelineClip clip;
        clip.startFrame = 0;
        clip.durationFrames = 300;
        clip.sourceInFrame = 1000;
        clip.sourceDurationFrames = 600;

        QCOMPARE(inferFacestreamFrameDomain(clip, 0, 299),
                 FacestreamFrameDomain::SourceRelative);
        QVERIFY(isSourceMediaFacestreamFrameDomain(FacestreamFrameDomain::SourceRelative));
        QVERIFY(isSourceMediaFacestreamFrameDomain(FacestreamFrameDomain::SourceAbsolute));
        QVERIFY(!isSourceMediaFacestreamFrameDomain(FacestreamFrameDomain::ClipTimeline30Fps));
    }

    void typicalFrameStepUsesMedianDelta()
    {
        const QVector<int64_t> frames{0, 4, 8, 12, 24};
        QCOMPARE(facedetectionsTypicalFrameStep(frames), 4LL);
    }

    void bridgeGapAllowsExpectedStrideMiss()
    {
        QVERIFY(facedetectionsShouldBridgeGap(8, 16, 4));
        QVERIFY(!facedetectionsShouldBridgeGap(8, 24, 4));
    }

    void edgeHoldTracksFullDetectedStride()
    {
        QCOMPARE(facedetectionsMaxEdgeHoldFrames(1), 1LL);
        QCOMPARE(facedetectionsMaxEdgeHoldFrames(4), 4LL);
        QCOMPARE(facedetectionsMaxEdgeHoldFrames(9), 9LL);
    }

    void sourceAbsoluteDetectorStrideDoesNotHalvePlaybackVisibility()
    {
        TimelineClip clip;
        clip.startFrame = 0;
        clip.durationFrames = 300;
        clip.sourceInFrame = 100;
        clip.sourceDurationFrames = 600;
        clip.sourceFps = 60.0;

        FacestreamResolvedTrack track;
        track.trackId = 7;
        track.frameDomain = FacestreamFrameDomain::SourceAbsolute;
        track.typicalFrameStep = 4;

        FacestreamResolvedKeyframe first;
        first.frame = 104;
        first.hasCenterBox = true;
        first.xNorm = 0.20;
        first.yNorm = 0.40;
        first.boxSizeNorm = 0.20;
        first.confidence = 1.0;

        FacestreamResolvedKeyframe second = first;
        second.frame = 108;
        second.xNorm = 0.40;

        track.keyframes = QVector<FacestreamResolvedKeyframe>{first, second};

        FacestreamResolvedSelection selection;
        QVERIFY(resolveFacestreamTrackAtPlayhead(clip, track, {}, 1, 106, &selection));
        QVERIFY(selection.interpolated);
        QCOMPARE(selection.lookupFrame, 106LL);
        QCOMPARE(selection.sourceFrame, 106LL);

        QVERIFY(resolveFacestreamTrackAtPlayhead(clip, track, {}, 1, 112, &selection));
        QVERIFY(!selection.interpolated);
        QCOMPARE(selection.lookupFrame, 112LL);
        QCOMPARE(selection.sourceFrame, 108LL);

        QVERIFY(!resolveFacestreamTrackAtPlayhead(clip, track, {}, 1, 113, &selection));
    }
};

QTEST_MAIN(FacestreamTimeMappingTest)
#include "test_facedetections_time_mapping.moc"
