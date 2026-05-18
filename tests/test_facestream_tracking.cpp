#include "facestream_tracking.h"

#include <QtTest/QtTest>

class FacestreamTrackingTest : public QObject {
    Q_OBJECT

private slots:
    void lowConfidenceUnmatchedDetectionDoesNotCreateTrack()
    {
        QVector<jcut::facestream::ContinuityTrack> tracks;
        const QVector<jcut::facestream::Detection> detections{
            {QRectF(10.0, 10.0, 40.0, 40.0), 0.30f}
        };
        jcut::facestream::ContinuityTrackingTuning tuning;
        tuning.newTrackMinConfidence = 0.45f;

        jcut::facestream::updateContinuityTracks(&tracks, detections, 0, QSize(100, 100), tuning);

        QVERIFY(tracks.isEmpty());
    }

    void matchingDetectionExtendsExistingTrack()
    {
        QVector<jcut::facestream::ContinuityTrack> tracks;
        jcut::facestream::ContinuityTrack track;
        track.id = 0;
        track.box = QRectF(10.0, 10.0, 40.0, 40.0);
        track.predictedBox = track.box;
        track.firstFrame = 0;
        track.lastFrame = 0;
        track.hits = 1;
        track.detections.append(QJsonObject{
            {QStringLiteral("frame"), 0},
            {QStringLiteral("x"), 0.3},
            {QStringLiteral("y"), 0.3},
            {QStringLiteral("box"), 0.4},
            {QStringLiteral("score"), 0.9}
        });
        tracks.push_back(track);

        const QVector<jcut::facestream::Detection> detections{
            {QRectF(12.0, 12.0, 40.0, 40.0), 0.85f}
        };
        jcut::facestream::ContinuityTrackingTuning tuning;
        tuning.trackMatchIouThreshold = 0.2f;

        jcut::facestream::updateContinuityTracks(&tracks, detections, 1, QSize(100, 100), tuning);

        QCOMPARE(tracks.size(), 1);
        QCOMPARE(tracks.first().id, 0);
        QCOMPARE(tracks.first().lastFrame, 1);
        QCOMPARE(tracks.first().detections.size(), 2);
        QCOMPARE(tracks.first().state, jcut::facestream::ContinuityTrackState::Confirmed);
    }

    void staleTrackDoesNotGetReused()
    {
        QVector<jcut::facestream::ContinuityTrack> tracks;
        jcut::facestream::ContinuityTrack track;
        track.id = 0;
        track.box = QRectF(10.0, 10.0, 40.0, 40.0);
        track.predictedBox = track.box;
        track.firstFrame = 0;
        track.lastFrame = 0;
        track.hits = 2;
        track.state = jcut::facestream::ContinuityTrackState::Confirmed;
        tracks.push_back(track);

        const QVector<jcut::facestream::Detection> detections{
            {QRectF(11.0, 11.0, 40.0, 40.0), 0.9f}
        };
        jcut::facestream::ContinuityTrackingTuning tuning;
        tuning.staleTrackFrameWindow = 4;

        jcut::facestream::updateContinuityTracks(&tracks, detections, 10, QSize(100, 100), tuning);

        QCOMPARE(tracks.size(), 1);
        QCOMPARE(tracks.first().id, 1);
        QCOMPARE(tracks.first().lastFrame, 10);
        QCOMPARE(tracks.first().detections.size(), 1);
    }

    void shortMissKeepsConfirmedTrackAliveForReacquisition()
    {
        QVector<jcut::facestream::ContinuityTrack> tracks;
        jcut::facestream::ContinuityTrackingTuning tuning;
        tuning.trackMatchIouThreshold = 0.20f;
        tuning.tentativeTrackHitCount = 2;
        tuning.staleTrackFrameWindow = 5;

        jcut::facestream::updateContinuityTracks(
            &tracks,
            {{QRectF(10.0, 10.0, 30.0, 30.0), 0.95f}},
            0,
            QSize(100, 100),
            tuning);
        jcut::facestream::updateContinuityTracks(
            &tracks,
            {{QRectF(13.0, 10.0, 30.0, 30.0), 0.94f}},
            1,
            QSize(100, 100),
            tuning);
        QCOMPARE(tracks.size(), 1);
        QCOMPARE(tracks.first().state, jcut::facestream::ContinuityTrackState::Confirmed);

        jcut::facestream::updateContinuityTracks(
            &tracks,
            {},
            2,
            QSize(100, 100),
            tuning);
        QCOMPARE(tracks.size(), 1);
        QCOMPARE(tracks.first().state, jcut::facestream::ContinuityTrackState::Lost);

        jcut::facestream::updateContinuityTracks(
            &tracks,
            {{QRectF(16.0, 10.0, 30.0, 30.0), 0.93f}},
            3,
            QSize(100, 100),
            tuning);
        QCOMPARE(tracks.size(), 1);
        QCOMPARE(tracks.first().id, 0);
        QCOMPARE(tracks.first().lastFrame, 3);
        QCOMPARE(tracks.first().state, jcut::facestream::ContinuityTrackState::Confirmed);
        QCOMPARE(tracks.first().detections.size(), 3);
    }

    void tentativeTrackGetsDroppedAfterMiss()
    {
        QVector<jcut::facestream::ContinuityTrack> tracks;
        jcut::facestream::ContinuityTrackingTuning tuning;
        tuning.tentativeTrackHitCount = 2;
        tuning.tentativeTrackMaxMisses = 0;

        jcut::facestream::updateContinuityTracks(
            &tracks,
            {{QRectF(10.0, 10.0, 30.0, 30.0), 0.95f}},
            0,
            QSize(100, 100),
            tuning);
        QCOMPARE(tracks.size(), 1);
        QCOMPARE(tracks.first().state, jcut::facestream::ContinuityTrackState::Tentative);

        jcut::facestream::updateContinuityTracks(
            &tracks,
            {},
            1,
            QSize(100, 100),
            tuning);
        QVERIFY(tracks.isEmpty());
    }
};

QTEST_MAIN(FacestreamTrackingTest)
#include "test_facestream_tracking.moc"
