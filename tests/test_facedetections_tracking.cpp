#include "facedetections_tracking.h"

#include <QtTest/QtTest>

class FacestreamTrackingTest : public QObject {
    Q_OBJECT

private slots:
    void lowConfidenceUnmatchedDetectionDoesNotCreateTrack()
    {
        QVector<jcut::facedetections::ContinuityTrack> tracks;
        const QVector<jcut::facedetections::Detection> detections{
            {QRectF(10.0, 10.0, 40.0, 40.0), 0.30f}
        };
        jcut::facedetections::ContinuityTrackingTuning tuning;
        tuning.newTrackMinConfidence = 0.35f;

        jcut::facedetections::updateContinuityTracks(&tracks, detections, 0, QSize(100, 100), tuning);

        QVERIFY(tracks.isEmpty());
    }

    void matchingDetectionExtendsExistingTrack()
    {
        QVector<jcut::facedetections::ContinuityTrack> tracks;
        jcut::facedetections::ContinuityTrack track;
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

        const QVector<jcut::facedetections::Detection> detections{
            {QRectF(12.0, 12.0, 40.0, 40.0), 0.85f}
        };
        jcut::facedetections::ContinuityTrackingTuning tuning;
        tuning.trackMatchIouThreshold = 0.2f;

        jcut::facedetections::updateContinuityTracks(&tracks, detections, 1, QSize(100, 100), tuning);

        QCOMPARE(tracks.size(), 1);
        QCOMPARE(tracks.first().id, 0);
        QCOMPARE(tracks.first().lastFrame, 1);
        QCOMPARE(tracks.first().detections.size(), 2);
        QCOMPARE(tracks.first().state, jcut::facedetections::ContinuityTrackState::Confirmed);
    }

    void staleTrackDoesNotGetReused()
    {
        QVector<jcut::facedetections::ContinuityTrack> tracks;
        jcut::facedetections::ContinuityTrack track;
        track.id = 0;
        track.box = QRectF(10.0, 10.0, 40.0, 40.0);
        track.predictedBox = track.box;
        track.firstFrame = 0;
        track.lastFrame = 0;
        track.hits = 2;
        track.state = jcut::facedetections::ContinuityTrackState::Confirmed;
        tracks.push_back(track);

        const QVector<jcut::facedetections::Detection> detections{
            {QRectF(11.0, 11.0, 40.0, 40.0), 0.9f}
        };
        jcut::facedetections::ContinuityTrackingTuning tuning;
        tuning.staleTrackFrameWindow = 4;

        jcut::facedetections::updateContinuityTracks(&tracks, detections, 10, QSize(100, 100), tuning);

        QCOMPARE(tracks.size(), 1);
        QCOMPARE(tracks.first().id, 1);
        QCOMPARE(tracks.first().lastFrame, 10);
        QCOMPARE(tracks.first().detections.size(), 1);
    }

    void shortMissKeepsConfirmedTrackAliveForReacquisition()
    {
        QVector<jcut::facedetections::ContinuityTrack> tracks;
        jcut::facedetections::ContinuityTrackingTuning tuning;
        tuning.trackMatchIouThreshold = 0.20f;
        tuning.tentativeTrackHitCount = 2;
        tuning.staleTrackFrameWindow = 5;

        jcut::facedetections::updateContinuityTracks(
            &tracks,
            {{QRectF(10.0, 10.0, 30.0, 30.0), 0.95f}},
            0,
            QSize(100, 100),
            tuning);
        jcut::facedetections::updateContinuityTracks(
            &tracks,
            {{QRectF(13.0, 10.0, 30.0, 30.0), 0.94f}},
            1,
            QSize(100, 100),
            tuning);
        QCOMPARE(tracks.size(), 1);
        QCOMPARE(tracks.first().state, jcut::facedetections::ContinuityTrackState::Confirmed);

        jcut::facedetections::updateContinuityTracks(
            &tracks,
            {},
            2,
            QSize(100, 100),
            tuning);
        QCOMPARE(tracks.size(), 1);
        QCOMPARE(tracks.first().state, jcut::facedetections::ContinuityTrackState::Lost);

        jcut::facedetections::updateContinuityTracks(
            &tracks,
            {{QRectF(16.0, 10.0, 30.0, 30.0), 0.93f}},
            3,
            QSize(100, 100),
            tuning);
        QCOMPARE(tracks.size(), 1);
        QCOMPARE(tracks.first().id, 0);
        QCOMPARE(tracks.first().lastFrame, 3);
        QCOMPARE(tracks.first().state, jcut::facedetections::ContinuityTrackState::Confirmed);
        QCOMPARE(tracks.first().detections.size(), 3);
    }

    void tentativeTrackGetsDroppedAfterMiss()
    {
        QVector<jcut::facedetections::ContinuityTrack> tracks;
        jcut::facedetections::ContinuityTrackingTuning tuning;
        tuning.tentativeTrackHitCount = 2;
        tuning.tentativeTrackMaxMisses = 0;

        jcut::facedetections::updateContinuityTracks(
            &tracks,
            {{QRectF(10.0, 10.0, 30.0, 30.0), 0.95f}},
            0,
            QSize(100, 100),
            tuning);
        QCOMPARE(tracks.size(), 1);
        QCOMPARE(tracks.first().state, jcut::facedetections::ContinuityTrackState::Tentative);

        jcut::facedetections::updateContinuityTracks(
            &tracks,
            {},
            1,
            QSize(100, 100),
            tuning);
        QVERIFY(tracks.isEmpty());
    }
};

QTEST_MAIN(FacestreamTrackingTest)
#include "test_facedetections_tracking.moc"
