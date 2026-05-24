#include "facedetections_time_mapping.h"

#include <QtTest/QtTest>

class FacestreamTimeMappingTest : public QObject {
    Q_OBJECT

private slots:
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

    void edgeHoldTracksHalfStride()
    {
        QCOMPARE(facedetectionsMaxEdgeHoldFrames(1), 1LL);
        QCOMPARE(facedetectionsMaxEdgeHoldFrames(4), 2LL);
        QCOMPARE(facedetectionsMaxEdgeHoldFrames(9), 4LL);
    }
};

QTEST_MAIN(FacestreamTimeMappingTest)
#include "test_facedetections_time_mapping.moc"
