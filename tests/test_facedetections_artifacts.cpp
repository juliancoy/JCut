#include "facedetections_assignment_services.h"
#include "facedetections_tracking.h"
#include "transcript_engine.h"

#include <QDir>
#include <QFileInfo>
#include <QJsonDocument>
#include <QTemporaryDir>
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
        {QStringLiteral("frame_width"), 1280},
        {QStringLiteral("frame_height"), 720},
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

facefind::Candidate cropSample(int trackId, int64_t frame, qreal score, const QString& cropPath)
{
    facefind::Candidate candidate;
    candidate.trackId = trackId;
    candidate.frame = frame;
    candidate.score = score;
    candidate.cropPath = cropPath;
    return candidate;
}

} // namespace

class FacestreamArtifactsTest : public QObject {
    Q_OBJECT

private slots:
    void transcriptAndArtifactRoundTrip()
    {
        QTemporaryDir tempDir;
        QVERIFY(tempDir.isValid());

        const QString transcriptPath =
            QDir(tempDir.path()).filePath(QStringLiteral("episode_01.json"));
        const QString clipId = QStringLiteral("clip_001");

        editor::TranscriptEngine engine;

        QJsonObject transcriptRoot{
            {QStringLiteral("speaker_profiles"), QJsonObject{
                 {QStringLiteral("SPEAKER_A"), QJsonObject{{QStringLiteral("name"), QStringLiteral("Host")}}},
                 {QStringLiteral("SPEAKER_B"), QJsonObject{{QStringLiteral("name"), QStringLiteral("Guest")}}}
             }},
            {QStringLiteral("segments"), QJsonArray{}}
        };
        QVERIFY(engine.saveTranscriptJson(transcriptPath, QJsonDocument(transcriptRoot)));

        const QJsonArray rawDetectionFrames{
            detectionFrame(0, {
                detection(100.0, 80.0, 60.0, 60.0, 0.95),
                detection(500.0, 90.0, 62.0, 62.0, 0.96)
            }),
            detectionFrame(1, {
                detection(108.0, 82.0, 60.0, 60.0, 0.94),
                detection(508.0, 92.0, 62.0, 62.0, 0.95)
            })
        };

        jcut::facedetections::ContinuityTrackingTuning trackingTuning;
        trackingTuning.tentativeTrackHitCount = 2;
        const QVector<jcut::facedetections::ContinuityTrack> tracks =
            jcut::facedetections::buildContinuityTracksFromDetectionFrames(
                rawDetectionFrames,
                trackingTuning);
        QCOMPARE(tracks.size(), 2);

        const QJsonArray trackRows = jcut::facedetections::buildContinuityTrackRows(tracks);
        QCOMPARE(trackRows.size(), 2);

        QJsonObject continuityRoot{
            {QStringLiteral("run_id"), QStringLiteral("run_001")},
            {QStringLiteral("only_dialogue"), false},
            {QStringLiteral("scan_start_frame"), 0},
            {QStringLiteral("scan_end_frame"), 1},
            {QStringLiteral("detector_mode"), QStringLiteral("scrfd")},
            {QStringLiteral("raw_tracks"), trackRows},
            {QStringLiteral("raw_frames"), rawDetectionFrames}
        };
        QJsonObject facedetectionsArtifact{
            {QStringLiteral("schema"), QStringLiteral("jcut_facedetections_v1")},
            {QStringLiteral("continuity_facedetections_by_clip"), QJsonObject{
                 {clipId, continuityRoot}
             }}
        };
        QVERIFY(engine.saveFacestreamArtifact(transcriptPath, facedetectionsArtifact));
        QVERIFY(QFileInfo::exists(engine.facedetectionsArtifactPath(transcriptPath)));

        jcut::facedetections_assignment::TrackIdentityEvidence identityA;
        identityA.trackId = trackRows.at(0).toObject().value(QStringLiteral("track_id")).toInt();
        identityA.cropSamples = {
            cropSample(identityA.trackId, 0, 0.90, QStringLiteral("track_a_0.png"))
        };
        identityA.embedding = {1.0f, 0.0f, 0.0f};
        identityA.hasEmbedding = true;

        jcut::facedetections_assignment::TrackIdentityEvidence identityB;
        identityB.trackId = trackRows.at(1).toObject().value(QStringLiteral("track_id")).toInt();
        identityB.cropSamples = {
            cropSample(identityB.trackId, 0, 0.92, QStringLiteral("track_b_0.png"))
        };
        identityB.embedding = {0.0f, 1.0f, 0.0f};
        identityB.hasEmbedding = true;

        const auto clusterResult =
            jcut::facedetections_assignment::clusterTrackIdentityEvidence(
                {identityA, identityB},
                0.70,
                0.55);
        QVERIFY(clusterResult.ok);
        QCOMPARE(clusterResult.clusterRows.size(), 2);

        QJsonArray assignmentRows;
        assignmentRows.push_back(QJsonObject{
            {QStringLiteral("decision"), QStringLiteral("accepted")},
            {QStringLiteral("resolved_speaker_id"), QStringLiteral("SPEAKER_A")},
            {QStringLiteral("cluster_id"), clusterResult.clusterCandidates.at(0).clusterId},
            {QStringLiteral("track_ids"), QJsonArray{clusterResult.clusterCandidates.at(0).trackId}}
        });
        assignmentRows.push_back(QJsonObject{
            {QStringLiteral("decision"), QStringLiteral("accepted")},
            {QStringLiteral("resolved_speaker_id"), QStringLiteral("SPEAKER_B")},
            {QStringLiteral("cluster_id"), clusterResult.clusterCandidates.at(1).clusterId},
            {QStringLiteral("track_ids"), QJsonArray{clusterResult.clusterCandidates.at(1).trackId}}
        });

        QVector<facefind::Candidate> trackCandidates;
        trackCandidates << identityA.cropSamples << identityB.cropSamples;
        const auto assignmentResult =
            jcut::facedetections_assignment::resolveTrackIdentityAssignments(
                assignmentRows,
                trackCandidates,
                QStringLiteral("2026-05-18T03:00:00Z"));
        QCOMPARE(assignmentResult.resolvedMap.size(), 2);

        QJsonObject identityRoot{
            {QStringLiteral("schema"), QStringLiteral("jcut_identity_v1")},
            {QStringLiteral("updated_at_utc"), QStringLiteral("2026-05-18T03:00:00Z")},
            {QStringLiteral("identity_clusters_by_clip"), QJsonObject{
                 {clipId, QJsonObject{
                      {QStringLiteral("clusters"), clusterResult.clusterRows}
                  }}
             }},
            {QStringLiteral("identity_assignments_by_clip"), QJsonObject{
                 {clipId, QJsonObject{
                      {QStringLiteral("assignment_table_rows"), assignmentRows},
                      {QStringLiteral("track_identity_overrides"), assignmentResult.overrides},
                      {QStringLiteral("track_identity_map"), assignmentResult.resolvedMap}
                  }}
             }}
        };
        QVERIFY(engine.saveIdentityArtifact(transcriptPath, identityRoot));
        QVERIFY(QFileInfo::exists(engine.identityArtifactPath(transcriptPath)));

        QJsonDocument reloadedTranscript;
        QVERIFY(engine.loadTranscriptJson(transcriptPath, &reloadedTranscript));
        QJsonObject reloadedRoot = reloadedTranscript.object();
        QJsonObject speakerFlow = reloadedRoot.value(QStringLiteral("speaker_flow")).toObject();
        speakerFlow[QStringLiteral("schema_version")] = QStringLiteral("1.0");
        QJsonObject clipsRoot = speakerFlow.value(QStringLiteral("clips")).toObject();
        QJsonObject clipRoot = clipsRoot.value(clipId).toObject();
        clipRoot[QStringLiteral("clip_id")] = clipId;
        clipRoot[QStringLiteral("resolved_current")] = QJsonObject{
            {QStringLiteral("run_id"), QStringLiteral("run_001")},
            {QStringLiteral("updated_at_utc"), QStringLiteral("2026-05-18T03:00:00Z")},
            {QStringLiteral("track_identity_map"), assignmentResult.resolvedMap}
        };
        clipsRoot[clipId] = clipRoot;
        speakerFlow[QStringLiteral("clips")] = clipsRoot;
        reloadedRoot[QStringLiteral("speaker_flow")] = speakerFlow;
        QVERIFY(engine.saveTranscriptJson(transcriptPath, QJsonDocument(reloadedRoot)));

        QJsonObject loadedFacestreamArtifact;
        QVERIFY(engine.loadFacestreamArtifact(transcriptPath, &loadedFacestreamArtifact));
        const QJsonObject loadedByClip =
            loadedFacestreamArtifact.value(QStringLiteral("continuity_facedetections_by_clip")).toObject();
        const QJsonObject loadedContinuityRoot = loadedByClip.value(clipId).toObject();
        QCOMPARE(loadedContinuityRoot.value(QStringLiteral("detector_mode")).toString(),
                 QStringLiteral("scrfd"));
        QCOMPARE(loadedContinuityRoot.value(QStringLiteral("raw_tracks")).toArray().size(), 2);
        QCOMPARE(loadedContinuityRoot.value(QStringLiteral("raw_frames")).toArray().size(), 2);

        QJsonObject loadedIdentityRoot;
        QVERIFY(engine.loadIdentityArtifact(transcriptPath, &loadedIdentityRoot));
        const QJsonObject clustersByClip =
            loadedIdentityRoot.value(QStringLiteral("identity_clusters_by_clip")).toObject();
        const QJsonObject assignmentsByClip =
            loadedIdentityRoot.value(QStringLiteral("identity_assignments_by_clip")).toObject();
        QCOMPARE(clustersByClip.value(clipId).toObject().value(QStringLiteral("clusters")).toArray().size(), 2);
        QCOMPARE(assignmentsByClip.value(clipId).toObject().value(QStringLiteral("track_identity_map")).toArray().size(), 2);

        QJsonDocument finalTranscript;
        QVERIFY(engine.loadTranscriptJson(transcriptPath, &finalTranscript));
        const QJsonObject finalRoot = finalTranscript.object();
        const QJsonObject finalSpeakerFlow = finalRoot.value(QStringLiteral("speaker_flow")).toObject();
        const QJsonObject finalClipRoot =
            finalSpeakerFlow.value(QStringLiteral("clips")).toObject().value(clipId).toObject();
        const QJsonArray resolvedMap =
            finalClipRoot.value(QStringLiteral("resolved_current")).toObject()
                .value(QStringLiteral("track_identity_map")).toArray();
        QCOMPARE(resolvedMap.size(), 2);
    }
};

QTEST_MAIN(FacestreamArtifactsTest)
#include "test_facedetections_artifacts.moc"
