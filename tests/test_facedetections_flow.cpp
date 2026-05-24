#include "facedetections_assignment_services.h"
#include "facedetections_tracking.h"
#include "speaker_flow_debug.h"
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

QJsonObject transcriptRootWithProfiles()
{
    return QJsonObject{
        {QStringLiteral("speaker_profiles"), QJsonObject{
             {QStringLiteral("SPEAKER_A"), QJsonObject{{QStringLiteral("name"), QStringLiteral("Host")}}},
             {QStringLiteral("SPEAKER_B"), QJsonObject{{QStringLiteral("name"), QStringLiteral("Guest")}}}
         }},
        {QStringLiteral("segments"), QJsonArray{}}
    };
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

        jcut::facedetections::ContinuityTrackingTuning trackingTuning;
        trackingTuning.tentativeTrackHitCount = 2;
        trackingTuning.trackMatchIouThreshold = 0.20f;

        const QVector<jcut::facedetections::ContinuityTrack> tracks =
            jcut::facedetections::buildContinuityTracksFromDetectionFrames(
                rawDetectionFrames,
                trackingTuning);
        QCOMPARE(tracks.size(), 2);
        QCOMPARE(tracks.at(0).detections.size(), 3);
        QCOMPARE(tracks.at(1).detections.size(), 3);

        const QJsonArray trackRows =
            jcut::facedetections::buildContinuityTrackRows(tracks);
        QCOMPARE(trackRows.size(), 2);
        QCOMPARE(trackRows.at(0).toObject().value(QStringLiteral("state")).toString(),
                 QStringLiteral("confirmed"));
        QCOMPARE(trackRows.at(1).toObject().value(QStringLiteral("state")).toString(),
                 QStringLiteral("confirmed"));

        jcut::facedetections_assignment::TrackIdentityEvidence track0;
        track0.trackId = trackRows.at(0).toObject().value(QStringLiteral("track_id")).toInt();
        track0.cropSamples = {
            cropSample(track0.trackId, 0, 0.90, QStringLiteral("track0_a.png")),
            cropSample(track0.trackId, 2, 0.88, QStringLiteral("track0_b.png"))
        };
        track0.embedding = {1.0f, 0.0f, 0.0f};
        track0.hasEmbedding = true;

        jcut::facedetections_assignment::TrackIdentityEvidence track1;
        track1.trackId = trackRows.at(1).toObject().value(QStringLiteral("track_id")).toInt();
        track1.cropSamples = {
            cropSample(track1.trackId, 0, 0.92, QStringLiteral("track1_a.png")),
            cropSample(track1.trackId, 2, 0.89, QStringLiteral("track1_b.png"))
        };
        track1.embedding = {0.0f, 1.0f, 0.0f};
        track1.hasEmbedding = true;

        const auto clusterResult =
            jcut::facedetections_assignment::clusterTrackIdentityEvidence(
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
            jcut::facedetections_assignment::resolveTrackIdentityAssignments(
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

    void speakerFlowDocumentContractPersistsSidecarsDebugRunAndResolvedState()
    {
        QTemporaryDir tempDir;
        QVERIFY(tempDir.isValid());

        const QString projectRoot =
            QDir(tempDir.path()).filePath(QStringLiteral("projects/project_speaker_flow"));
        QVERIFY(QDir().mkpath(projectRoot));
        const QString transcriptPath =
            QDir(projectRoot).filePath(QStringLiteral("episode_01.json"));
        const QString clipId = QStringLiteral("clip_001");
        const QString videoFilename = QStringLiteral("episode_01.mp4");
        const QString videoStem = QStringLiteral("episode_01");
        const QString timestampUtc = QStringLiteral("2026-05-21T12:00:00Z");

        editor::TranscriptEngine engine;
        QVERIFY(engine.saveTranscriptJson(
            transcriptPath,
            QJsonDocument(transcriptRootWithProfiles())));

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

        jcut::facedetections::ContinuityTrackingTuning trackingTuning;
        trackingTuning.tentativeTrackHitCount = 2;
        trackingTuning.trackMatchIouThreshold = 0.20f;
        const QVector<jcut::facedetections::ContinuityTrack> tracks =
            jcut::facedetections::buildContinuityTracksFromDetectionFrames(
                rawDetectionFrames,
                trackingTuning);
        QCOMPARE(tracks.size(), 2);

        const QJsonArray trackRows =
            jcut::facedetections::buildContinuityTrackRows(tracks);
        QCOMPARE(trackRows.size(), 2);

        QJsonObject facedetectionsRoot{
            {QStringLiteral("schema"), QStringLiteral("jcut_facedetections_v1")},
            {QStringLiteral("continuity_facedetections_by_clip"), QJsonObject{
                 {clipId, QJsonObject{
                      {QStringLiteral("run_id"), QStringLiteral("run_001")},
                      {QStringLiteral("only_dialogue"), false},
                      {QStringLiteral("scan_start_frame"), 0},
                      {QStringLiteral("scan_end_frame"), 2},
                      {QStringLiteral("detector_mode"), QStringLiteral("scrfd")},
                      {QStringLiteral("raw_tracks"), trackRows},
                      {QStringLiteral("raw_frames"), rawDetectionFrames}
                  }}
             }}
        };
        QVERIFY(engine.saveFacestreamArtifact(transcriptPath, facedetectionsRoot));
        QVERIFY(QFileInfo::exists(engine.facedetectionsArtifactPath(transcriptPath)));

        auto debugRun = speaker_flow_debug::openLatestOrCreateRun(
            transcriptPath, clipId, videoStem);
        QVERIFY(!debugRun.projectRoot.isEmpty());
        QVERIFY(!debugRun.clipDebugRoot.isEmpty());
        QVERIFY(!debugRun.runDir.isEmpty());
        QVERIFY(QFileInfo::exists(debugRun.runDir));

        const QString cropDir =
            QDir(debugRun.runDir).filePath(QStringLiteral("%1_facedetections_track_crops").arg(videoStem));
        QVERIFY(QDir().mkpath(cropDir));
        const QString trackCandidatesPath =
            QDir(debugRun.runDir).filePath(QStringLiteral("%1_facedetections_track_candidates.json").arg(videoStem));
        const QString assignmentTablePath =
            QDir(debugRun.runDir).filePath(QStringLiteral("%1_assignment_table.json").arg(videoStem));
        const QString assignmentDecisionsPath =
            QDir(debugRun.runDir).filePath(QStringLiteral("%1_assignment_decisions.json").arg(videoStem));
        const QString indexPath = QDir(debugRun.runDir).filePath(QStringLiteral("index.json"));
        const QString artifactDir =
            QDir(debugRun.runDir).filePath(QStringLiteral("facedetections_artifact"));
        QVERIFY(QDir().mkpath(artifactDir));
        const QString artifactSummaryPath =
            QDir(artifactDir).filePath(QStringLiteral("summary.json"));

        jcut::facedetections_assignment::TrackIdentityEvidence track0;
        track0.trackId = trackRows.at(0).toObject().value(QStringLiteral("track_id")).toInt();
        track0.cropSamples = {
            cropSample(track0.trackId, 0, 0.90, QStringLiteral("track0_a.png")),
            cropSample(track0.trackId, 2, 0.88, QStringLiteral("track0_b.png"))
        };
        track0.embedding = {1.0f, 0.0f, 0.0f};
        track0.hasEmbedding = true;

        jcut::facedetections_assignment::TrackIdentityEvidence track1;
        track1.trackId = trackRows.at(1).toObject().value(QStringLiteral("track_id")).toInt();
        track1.cropSamples = {
            cropSample(track1.trackId, 0, 0.92, QStringLiteral("track1_a.png")),
            cropSample(track1.trackId, 2, 0.89, QStringLiteral("track1_b.png"))
        };
        track1.embedding = {0.0f, 1.0f, 0.0f};
        track1.hasEmbedding = true;

        QJsonArray trackCandidateRows;
        for (const facefind::Candidate& candidate : track0.cropSamples) {
            trackCandidateRows.push_back(QJsonObject{
                {QStringLiteral("track_id"), candidate.trackId},
                {QStringLiteral("frame"), static_cast<qint64>(candidate.frame)},
                {QStringLiteral("score"), candidate.score},
                {QStringLiteral("crop_path"), candidate.cropPath}
            });
        }
        for (const facefind::Candidate& candidate : track1.cropSamples) {
            trackCandidateRows.push_back(QJsonObject{
                {QStringLiteral("track_id"), candidate.trackId},
                {QStringLiteral("frame"), static_cast<qint64>(candidate.frame)},
                {QStringLiteral("score"), candidate.score},
                {QStringLiteral("crop_path"), candidate.cropPath}
            });
        }
        QFile trackCandidatesFile(trackCandidatesPath);
        QVERIFY(trackCandidatesFile.open(QIODevice::WriteOnly | QIODevice::Truncate));
        trackCandidatesFile.write(QJsonDocument(trackCandidateRows).toJson(QJsonDocument::Indented));
        trackCandidatesFile.close();

        const auto clusterResult =
            jcut::facedetections_assignment::clusterTrackIdentityEvidence(
                {track0, track1},
                0.70,
                0.55);
        QVERIFY(clusterResult.ok);
        QCOMPARE(clusterResult.clusterRows.size(), 2);

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

        QFile assignmentTableFile(assignmentTablePath);
        QVERIFY(assignmentTableFile.open(QIODevice::WriteOnly | QIODevice::Truncate));
        assignmentTableFile.write(QJsonDocument(assignmentRows).toJson(QJsonDocument::Indented));
        assignmentTableFile.close();

        QVector<facefind::Candidate> trackCandidates;
        trackCandidates << track0.cropSamples << track1.cropSamples;
        const auto assignmentResult =
            jcut::facedetections_assignment::resolveTrackIdentityAssignments(
                assignmentRows,
                trackCandidates,
                timestampUtc);

        QCOMPARE(assignmentResult.resolvedMap.size(), 2);
        QCOMPARE(assignmentResult.auditLog.size(), 2);
        QCOMPARE(assignmentResult.overrides.size(), 2);

        QFile assignmentDecisionsFile(assignmentDecisionsPath);
        QVERIFY(assignmentDecisionsFile.open(QIODevice::WriteOnly | QIODevice::Truncate));
        assignmentDecisionsFile.write(
            QJsonDocument(QJsonObject{
                {QStringLiteral("audit_log"), assignmentResult.auditLog},
                {QStringLiteral("track_identity_map"), assignmentResult.resolvedMap}
            }).toJson(QJsonDocument::Indented));
        assignmentDecisionsFile.close();

        QFile artifactSummaryFile(artifactSummaryPath);
        QVERIFY(artifactSummaryFile.open(QIODevice::WriteOnly | QIODevice::Truncate));
        artifactSummaryFile.write(
            QJsonDocument(QJsonObject{
                {QStringLiteral("clip_id"), clipId},
                {QStringLiteral("track_count"), trackRows.size()}
            }).toJson(QJsonDocument::Indented));
        artifactSummaryFile.close();

        speaker_flow_debug::persistIndex(
            indexPath,
            debugRun.runId,
            clipId,
            videoFilename,
            transcriptPath,
            QStringLiteral("stage_4_crop_extraction"),
            QStringLiteral("ok"),
            QStringLiteral("Representative crops recorded."),
            {trackCandidatesPath});
        speaker_flow_debug::persistIndex(
            indexPath,
            debugRun.runId,
            clipId,
            videoFilename,
            transcriptPath,
            QStringLiteral("stage_5_identity_clustering"),
            QStringLiteral("ok"),
            QStringLiteral("Identity clusters recorded."),
            {assignmentTablePath});
        speaker_flow_debug::persistIndex(
            indexPath,
            debugRun.runId,
            clipId,
            videoFilename,
            transcriptPath,
            QStringLiteral("stage_6_assignment_review"),
            QStringLiteral("ok"),
            QStringLiteral("Assignment review recorded."),
            {assignmentDecisionsPath, artifactSummaryPath});

        QJsonObject identityRoot{
            {QStringLiteral("schema"), QStringLiteral("jcut_identity_v1")},
            {QStringLiteral("updated_at_utc"), timestampUtc},
            {QStringLiteral("identity_clusters_by_clip"), QJsonObject{
                 {clipId, QJsonObject{
                      {QStringLiteral("run_id"), debugRun.runId},
                      {QStringLiteral("clusters"), clusterResult.clusterRows},
                      {QStringLiteral("pairwise_diagnostics"), clusterResult.clusterDiagnosticsRows},
                      {QStringLiteral("embedded_track_count"), clusterResult.embeddedTrackCount},
                      {QStringLiteral("auto_cluster_threshold"), clusterResult.autoClusterThreshold},
                      {QStringLiteral("review_threshold"), clusterResult.reviewThreshold}
                  }}
             }},
            {QStringLiteral("identity_assignments_by_clip"), QJsonObject{
                 {clipId, QJsonObject{
                      {QStringLiteral("run_id"), debugRun.runId},
                      {QStringLiteral("assignment_table_rows"), assignmentRows},
                      {QStringLiteral("track_identity_overrides"), assignmentResult.overrides},
                      {QStringLiteral("track_identity_map"), assignmentResult.resolvedMap}
                  }}
             }}
        };
        QVERIFY(engine.saveIdentityArtifact(transcriptPath, identityRoot));
        QVERIFY(QFileInfo::exists(engine.identityArtifactPath(transcriptPath)));

        QJsonDocument transcriptDoc;
        QVERIFY(engine.loadTranscriptJson(transcriptPath, &transcriptDoc));
        QJsonObject transcriptRoot = transcriptDoc.object();
        QJsonObject speakerFlow{
            {QStringLiteral("schema_version"), QStringLiteral("1.0")}
        };
        QJsonObject clipRoot{
            {QStringLiteral("clip_id"), clipId},
            {QStringLiteral("machine_runs"), QJsonObject{
                 {debugRun.runId, QJsonObject{
                      {QStringLiteral("run_id"), debugRun.runId},
                      {QStringLiteral("updated_at_utc"), timestampUtc},
                      {QStringLiteral("cluster_rows"), clusterResult.clusterRows},
                      {QStringLiteral("cluster_diagnostics"), clusterResult.clusterDiagnosticsRows}
                  }}
             }},
            {QStringLiteral("human_runs"), QJsonObject{
                 {debugRun.runId, QJsonObject{
                      {QStringLiteral("run_id"), debugRun.runId},
                      {QStringLiteral("updated_at_utc"), timestampUtc},
                      {QStringLiteral("assignment_table_rows"), assignmentRows},
                      {QStringLiteral("audit_log"), assignmentResult.auditLog}
                  }}
             }},
            {QStringLiteral("resolved_current"), QJsonObject{
                 {QStringLiteral("run_id"), debugRun.runId},
                 {QStringLiteral("updated_at_utc"), timestampUtc},
                 {QStringLiteral("track_identity_map"), assignmentResult.resolvedMap}
             }}
        };
        speakerFlow[QStringLiteral("clips")] = QJsonObject{{clipId, clipRoot}};
        transcriptRoot[QStringLiteral("speaker_flow")] = speakerFlow;
        QVERIFY(engine.saveTranscriptJson(transcriptPath, QJsonDocument(transcriptRoot)));

        QCOMPARE(speaker_flow_debug::latestRunId(debugRun.clipDebugRoot), debugRun.runId);
        QCOMPARE(speaker_flow_debug::latestRunIdWithArtifact(debugRun.clipDebugRoot), debugRun.runId);

        QJsonObject loadedFacestreamRoot;
        QVERIFY(engine.loadFacestreamArtifact(transcriptPath, &loadedFacestreamRoot));
        QCOMPARE(
            loadedFacestreamRoot.value(QStringLiteral("continuity_facedetections_by_clip"))
                .toObject()
                .value(clipId)
                .toObject()
                .value(QStringLiteral("raw_tracks"))
                .toArray()
                .size(),
            2);

        QJsonObject loadedIdentityRoot;
        QVERIFY(engine.loadIdentityArtifact(transcriptPath, &loadedIdentityRoot));
        QCOMPARE(
            loadedIdentityRoot.value(QStringLiteral("identity_assignments_by_clip"))
                .toObject()
                .value(clipId)
                .toObject()
                .value(QStringLiteral("track_identity_map"))
                .toArray()
                .size(),
            2);

        QJsonDocument finalTranscriptDoc;
        QVERIFY(engine.loadTranscriptJson(transcriptPath, &finalTranscriptDoc));
        const QJsonObject finalTranscriptRoot = finalTranscriptDoc.object();
        const QJsonObject finalSpeakerFlow =
            finalTranscriptRoot.value(QStringLiteral("speaker_flow")).toObject();
        const QJsonObject finalClipRoot =
            finalSpeakerFlow.value(QStringLiteral("clips")).toObject().value(clipId).toObject();
        const QJsonObject resolvedCurrent =
            finalClipRoot.value(QStringLiteral("resolved_current")).toObject();
        const QJsonArray resolvedMap =
            resolvedCurrent.value(QStringLiteral("track_identity_map")).toArray();

        QCOMPARE(finalSpeakerFlow.value(QStringLiteral("schema_version")).toString(),
                 QStringLiteral("1.0"));
        QVERIFY(finalClipRoot.value(QStringLiteral("machine_runs")).toObject().contains(debugRun.runId));
        QVERIFY(finalClipRoot.value(QStringLiteral("human_runs")).toObject().contains(debugRun.runId));
        QCOMPARE(resolvedCurrent.value(QStringLiteral("run_id")).toString(), debugRun.runId);
        QCOMPARE(resolvedMap.size(), 2);

        const QJsonObject firstResolved = resolvedMap.at(0).toObject();
        const QJsonObject secondResolved = resolvedMap.at(1).toObject();
        QVERIFY(firstResolved.value(QStringLiteral("track_id")).toInt() !=
                 secondResolved.value(QStringLiteral("track_id")).toInt());
        QVERIFY(firstResolved.value(QStringLiteral("identity_id")).toString() !=
                 secondResolved.value(QStringLiteral("identity_id")).toString());

        QVERIFY(QFileInfo::exists(indexPath));
        QVERIFY(QFileInfo::exists(trackCandidatesPath));
        QVERIFY(QFileInfo::exists(assignmentTablePath));
        QVERIFY(QFileInfo::exists(assignmentDecisionsPath));
        QVERIFY(QFileInfo::exists(artifactSummaryPath));
    }
};

QTEST_MAIN(FacestreamFlowTest)
#include "test_facedetections_flow.moc"
