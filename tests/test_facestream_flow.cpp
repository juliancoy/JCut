#include "facestream_assignment_services.h"
#include "facestream_tracking.h"

#include <QtTest/QtTest>

namespace {

QJsonObject detectionFrame(int frame, std::initializer_list<QJsonObject> detections)
{
    QJsonArray rows;
    for (const QJsonObject& row : detections) {
        rows.push_back(row);
    }
    return QJsonObject{
        {QStringLiteral("frame"), frame},
        {QStringLiteral("frame_width"), 1920},
        {QStringLiteral("frame_height"), 1080},
        {QStringLiteral("detections"), rows}
    };
}

QJsonObject detection(double x, double y, double w, double h, double confidence)
{
    return QJsonObject{
        {QStringLiteral("x"), x},
        {QStringLiteral("y"), y},
        {QStringLiteral("w"), w},
        {QStringLiteral("h"), h},
        {QStringLiteral("confidence"), confidence}
    };
}

facefind::Candidate cropSample(int trackId, int frame, qreal score, const QString& cropPath)
{
    facefind::Candidate candidate;
    candidate.trackId = trackId;
    candidate.frame = frame;
    candidate.score = score;
    candidate.cropPath = cropPath;
    return candidate;
}

} // namespace

class FacestreamFlowTest : public QObject {
    Q_OBJECT

private slots:
    void continuityTrackingToIdentityAssignmentFlow()
    {
        const QJsonArray rawDetectionFrames{
            detectionFrame(0, {
                detection(100.0, 100.0, 80.0, 80.0, 0.95),
                detection(600.0, 100.0, 82.0, 82.0, 0.96)
            }),
            detectionFrame(1, {
                detection(108.0, 100.0, 80.0, 80.0, 0.94),
                detection(608.0, 100.0, 82.0, 82.0, 0.95)
            }),
            detectionFrame(2, {
                detection(116.0, 100.0, 80.0, 80.0, 0.93),
                detection(616.0, 100.0, 82.0, 82.0, 0.94)
            })
        };

        jcut::facestream::ContinuityTrackingTuning trackingTuning;
        trackingTuning.tentativeTrackHitCount = 2;
        trackingTuning.trackMatchIouThreshold = 0.20f;

        const QVector<jcut::facestream::ContinuityTrack> tracks =
            jcut::facestream::buildContinuityTracksFromDetectionFrames(
                rawDetectionFrames,
                trackingTuning);
        QCOMPARE(tracks.size(), 2);
        QCOMPARE(tracks.at(0).detections.size(), 3);
        QCOMPARE(tracks.at(1).detections.size(), 3);

        const QJsonArray trackRows =
            jcut::facestream::buildContinuityTrackRows(tracks);
        QCOMPARE(trackRows.size(), 2);
        QCOMPARE(trackRows.at(0).toObject().value(QStringLiteral("state")).toString(),
                 QStringLiteral("confirmed"));
        QCOMPARE(trackRows.at(1).toObject().value(QStringLiteral("state")).toString(),
                 QStringLiteral("confirmed"));

        jcut::facestream_assignment::TrackIdentityEvidence track0;
        track0.trackId = trackRows.at(0).toObject().value(QStringLiteral("track_id")).toInt();
        track0.cropSamples = {
            cropSample(track0.trackId, 0, 0.90, QStringLiteral("track0_a.png")),
            cropSample(track0.trackId, 2, 0.88, QStringLiteral("track0_b.png"))
        };
        track0.embedding = {1.0f, 0.0f, 0.0f};
        track0.hasEmbedding = true;

        jcut::facestream_assignment::TrackIdentityEvidence track1;
        track1.trackId = trackRows.at(1).toObject().value(QStringLiteral("track_id")).toInt();
        track1.cropSamples = {
            cropSample(track1.trackId, 0, 0.92, QStringLiteral("track1_a.png")),
            cropSample(track1.trackId, 2, 0.89, QStringLiteral("track1_b.png"))
        };
        track1.embedding = {0.0f, 1.0f, 0.0f};
        track1.hasEmbedding = true;

        const auto clusterResult =
            jcut::facestream_assignment::clusterTrackIdentityEvidence(
                {track0, track1},
                0.70,
                0.55);
        QVERIFY(clusterResult.ok);
        QCOMPARE(clusterResult.clusterRows.size(), 2);
        QCOMPARE(clusterResult.clusterCandidates.size(), 2);

        QJsonArray assignmentRows;
        for (const facefind::Candidate& clusterCandidate : clusterResult.clusterCandidates) {
            const QString speakerId =
                clusterCandidate.trackId == track0.trackId
                    ? QStringLiteral("SPEAKER_A")
                    : QStringLiteral("SPEAKER_B");
            assignmentRows.push_back(QJsonObject{
                {QStringLiteral("decision"), QStringLiteral("accepted")},
                {QStringLiteral("resolved_speaker_id"), speakerId},
                {QStringLiteral("cluster_id"), clusterCandidate.clusterId},
                {QStringLiteral("track_ids"), QJsonArray{clusterCandidate.trackId}}
            });
        }

        QVector<facefind::Candidate> trackCandidates;
        trackCandidates << track0.cropSamples << track1.cropSamples;
        const auto assignmentResult =
            jcut::facestream_assignment::resolveTrackIdentityAssignments(
                assignmentRows,
                trackCandidates,
                QStringLiteral("2026-05-18T02:00:00Z"));

        QCOMPARE(assignmentResult.resolvedMap.size(), 2);
        QCOMPARE(assignmentResult.overrides.size(), 2);
        QCOMPARE(assignmentResult.assignmentsBySpeaker.size(), 2);

        const QJsonObject firstResolved = assignmentResult.resolvedMap.at(0).toObject();
        const QJsonObject secondResolved = assignmentResult.resolvedMap.at(1).toObject();
        QVERIFY(firstResolved.value(QStringLiteral("track_id")).toInt() !=
                 secondResolved.value(QStringLiteral("track_id")).toInt());
        QVERIFY(firstResolved.value(QStringLiteral("identity_id")).toString() !=
                 secondResolved.value(QStringLiteral("identity_id")).toString());
    }
};

QTEST_MAIN(FacestreamFlowTest)
#include "test_facestream_flow.moc"
