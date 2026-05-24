#include "facedetections_assignment_services.h"

#include <QtTest/QtTest>

namespace {

QJsonObject keyframeRow(qint64 frame, double score)
{
    return QJsonObject{
        {QStringLiteral("frame"), frame},
        {QStringLiteral("confidence"), score},
        {QStringLiteral("x"), 0.5},
        {QStringLiteral("y"), 0.5},
        {QStringLiteral("box_size"), 0.2}
    };
}

facefind::Candidate cropSample(int trackId, qreal score, const QString& cropPath)
{
    facefind::Candidate candidate;
    candidate.trackId = trackId;
    candidate.score = score;
    candidate.cropPath = cropPath;
    return candidate;
}

} // namespace

class FacestreamAssignmentTest : public QObject {
    Q_OBJECT

private slots:
    void representativeKeyframesPreferHighConfidenceWithSpacing()
    {
        const QJsonArray keyframes{
            keyframeRow(0, 0.95),
            keyframeRow(10, 0.90),
            keyframeRow(40, 0.85),
            keyframeRow(80, 0.80)
        };

        const QVector<QJsonObject> selected =
            jcut::facedetections_assignment::selectRepresentativeKeyframesForIdentity(
                keyframes, 3, 24);

        QCOMPARE(selected.size(), 3);
        QCOMPARE(selected.at(0).value(QStringLiteral("frame")).toInt(), 0);
        QCOMPARE(selected.at(1).value(QStringLiteral("frame")).toInt(), 40);
        QCOMPARE(selected.at(2).value(QStringLiteral("frame")).toInt(), 80);
    }

    void representativeKeyframesFallBackToFillWhenSpacingIsTooTight()
    {
        const QJsonArray keyframes{
            keyframeRow(0, 0.95),
            keyframeRow(5, 0.90),
            keyframeRow(10, 0.85)
        };

        const QVector<QJsonObject> selected =
            jcut::facedetections_assignment::selectRepresentativeKeyframesForIdentity(
                keyframes, 3, 24);

        QCOMPARE(selected.size(), 3);
        QCOMPARE(selected.at(0).value(QStringLiteral("frame")).toInt(), 0);
        QCOMPARE(selected.at(1).value(QStringLiteral("frame")).toInt(), 5);
        QCOMPARE(selected.at(2).value(QStringLiteral("frame")).toInt(), 10);
    }

    void clusteringAutoClustersSimilarTracksFromAggregatedEvidence()
    {
        jcut::facedetections_assignment::TrackIdentityEvidence track1;
        track1.trackId = 1;
        track1.cropSamples = {
            cropSample(1, 0.91, QStringLiteral("t1_a.png")),
            cropSample(1, 0.87, QStringLiteral("t1_b.png"))
        };
        track1.embedding = {1.0f, 0.0f, 0.0f};
        track1.hasEmbedding = true;

        jcut::facedetections_assignment::TrackIdentityEvidence track2;
        track2.trackId = 2;
        track2.cropSamples = {
            cropSample(2, 0.93, QStringLiteral("t2_a.png"))
        };
        track2.embedding = {0.98f, 0.05f, 0.0f};
        track2.hasEmbedding = true;

        jcut::facedetections_assignment::TrackIdentityEvidence track3;
        track3.trackId = 3;
        track3.cropSamples = {
            cropSample(3, 0.88, QStringLiteral("t3_a.png"))
        };
        track3.embedding = {0.0f, 1.0f, 0.0f};
        track3.hasEmbedding = true;

        const auto result =
            jcut::facedetections_assignment::clusterTrackIdentityEvidence(
                {track1, track2, track3}, 0.70, 0.55);

        QVERIFY(result.ok);
        QCOMPARE(result.clusterRows.size(), 2);
        QCOMPARE(result.autoClusterPairCount, 1);
        QCOMPARE(result.reviewPairCount, 0);
        QCOMPARE(result.embeddedTrackCount, 3);

        const QJsonObject firstCluster = result.clusterRows.at(0).toObject();
        QCOMPARE(firstCluster.value(QStringLiteral("cluster_id")).toString(), QStringLiteral("person_001"));
        QCOMPARE(firstCluster.value(QStringLiteral("representative_track_id")).toInt(), 2);
        QCOMPARE(firstCluster.value(QStringLiteral("representative_crop")).toString(), QStringLiteral("t2_a.png"));
        QCOMPARE(firstCluster.value(QStringLiteral("representative_crop_count")).toInt(), 1);
        const QJsonArray trackIds = firstCluster.value(QStringLiteral("track_ids")).toArray();
        QCOMPARE(trackIds.size(), 2);
        QCOMPARE(trackIds.at(0).toInt(), 1);
        QCOMPARE(trackIds.at(1).toInt(), 2);
    }

    void clusteringFallsBackToSingletonsWithoutEmbeddings()
    {
        jcut::facedetections_assignment::TrackIdentityEvidence track1;
        track1.trackId = 4;
        track1.cropSamples = {cropSample(4, 0.81, QStringLiteral("t4_a.png"))};

        jcut::facedetections_assignment::TrackIdentityEvidence track2;
        track2.trackId = 5;
        track2.cropSamples = {cropSample(5, 0.82, QStringLiteral("t5_a.png"))};

        const auto result =
            jcut::facedetections_assignment::clusterTrackIdentityEvidence(
                {track1, track2}, 0.70, 0.55);

        QVERIFY(result.ok);
        QCOMPARE(result.clusterRows.size(), 2);
        QCOMPARE(result.embeddedTrackCount, 0);
        QCOMPARE(result.autoClusterPairCount, 0);
        QCOMPARE(result.reviewPairCount, 0);
        QCOMPARE(result.clusterRows.at(0).toObject().value(QStringLiteral("status")).toString(),
                 QStringLiteral("singleton_no_embedding"));
        QCOMPARE(result.clusterRows.at(1).toObject().value(QStringLiteral("status")).toString(),
                 QStringLiteral("singleton_no_embedding"));
    }

    void assignmentResolutionExpandsAcceptedClusterTracks()
    {
        QVector<facefind::Candidate> trackCandidates{
            cropSample(1, 0.80, QStringLiteral("t1.png")),
            cropSample(2, 0.90, QStringLiteral("t2.png")),
            cropSample(3, 0.70, QStringLiteral("t3.png"))
        };

        const QJsonArray rows{
            QJsonObject{
                {QStringLiteral("decision"), QStringLiteral("accepted")},
                {QStringLiteral("resolved_speaker_id"), QStringLiteral("SPEAKER_00")},
                {QStringLiteral("cluster_id"), QStringLiteral("person_001")},
                {QStringLiteral("track_ids"), QJsonArray{1, 2}}
            },
            QJsonObject{
                {QStringLiteral("decision"), QStringLiteral("rejected")},
                {QStringLiteral("resolved_speaker_id"), QStringLiteral("SPEAKER_01")},
                {QStringLiteral("cluster_id"), QStringLiteral("person_002")},
                {QStringLiteral("track_ids"), QJsonArray{3}}
            }
        };

        const auto result =
            jcut::facedetections_assignment::resolveTrackIdentityAssignments(
                rows,
                trackCandidates,
                QStringLiteral("2026-05-18T01:00:00Z"));

        QCOMPARE(result.overrides.size(), 2);
        QCOMPARE(result.auditLog.size(), 2);
        QCOMPARE(result.resolvedMap.size(), 2);
        QCOMPARE(result.assignmentsBySpeaker.size(), 1);
        QCOMPARE(result.assignmentsBySpeaker.value(QStringLiteral("SPEAKER_00")).size(), 2);

        const QJsonObject firstResolved = result.resolvedMap.at(0).toObject();
        QCOMPARE(firstResolved.value(QStringLiteral("track_id")).toInt(), 1);
        QCOMPARE(firstResolved.value(QStringLiteral("identity_id")).toString(), QStringLiteral("SPEAKER_00"));
        QCOMPARE(firstResolved.value(QStringLiteral("resolution_source")).toString(), QStringLiteral("auto_selected"));
        const QJsonObject secondResolved = result.resolvedMap.at(1).toObject();
        QCOMPARE(secondResolved.value(QStringLiteral("track_id")).toInt(), 2);
        QCOMPARE(secondResolved.value(QStringLiteral("identity_id")).toString(), QStringLiteral("SPEAKER_00"));
    }

    void assignmentResolutionUsesFallbackTrackAndManualOverrideSource()
    {
        QVector<facefind::Candidate> trackCandidates{
            cropSample(7, 0.95, QStringLiteral("t7_low.png")),
            cropSample(7, 0.98, QStringLiteral("t7_high.png"))
        };

        const QJsonArray rows{
            QJsonObject{
                {QStringLiteral("decision"), QStringLiteral("accepted")},
                {QStringLiteral("resolved_speaker_id"), QStringLiteral("HOST")},
                {QStringLiteral("cluster_id"), QStringLiteral("person_007")},
                {QStringLiteral("manual_override"), QStringLiteral("HOST")},
                {QStringLiteral("track_id"), 7}
            }
        };

        const auto result =
            jcut::facedetections_assignment::resolveTrackIdentityAssignments(
                rows,
                trackCandidates,
                QStringLiteral("2026-05-18T01:05:00Z"));

        QCOMPARE(result.overrides.size(), 1);
        QCOMPARE(result.resolvedMap.size(), 1);
        QCOMPARE(result.assignmentsBySpeaker.size(), 1);
        QCOMPARE(result.assignmentsBySpeaker.value(QStringLiteral("HOST")).size(), 1);
        QCOMPARE(result.assignmentsBySpeaker.value(QStringLiteral("HOST")).first().cropPath,
                 QStringLiteral("t7_high.png"));

        const QJsonObject overrideRow = result.overrides.at(0).toObject();
        QCOMPARE(overrideRow.value(QStringLiteral("track_id")).toInt(), 7);
        QCOMPARE(overrideRow.value(QStringLiteral("source")).toString(), QStringLiteral("human_override"));
        QCOMPARE(overrideRow.value(QStringLiteral("manual_override")).toBool(), true);

        const QJsonObject auditRow = result.auditLog.at(0).toObject();
        QCOMPARE(auditRow.value(QStringLiteral("timestamp_utc")).toString(),
                 QStringLiteral("2026-05-18T01:05:00Z"));
    }
};

QTEST_MAIN(FacestreamAssignmentTest)
#include "test_facedetections_assignment.moc"
