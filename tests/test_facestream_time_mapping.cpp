#include "facestream_time_mapping.h"

#include <QtTest/QtTest>

class FacestreamTimeMappingTest : public QObject {
    Q_OBJECT

private slots:
    void typicalFrameStepUsesMedianDelta()
    {
        const QVector<int64_t> frames{0, 4, 8, 12, 24};
        QCOMPARE(facestreamTypicalFrameStep(frames), 4LL);
    }

    void bridgeGapAllowsExpectedStrideMiss()
    {
        QVERIFY(facestreamShouldBridgeGap(8, 16, 4));
        QVERIFY(!facestreamShouldBridgeGap(8, 24, 4));
    }

    void edgeHoldTracksHalfStride()
    {
        QCOMPARE(facestreamMaxEdgeHoldFrames(1), 1LL);
        QCOMPARE(facestreamMaxEdgeHoldFrames(4), 2LL);
        QCOMPARE(facestreamMaxEdgeHoldFrames(9), 4LL);
    }
};

QTEST_MAIN(FacestreamTimeMappingTest)
#include "test_facestream_time_mapping.moc"
