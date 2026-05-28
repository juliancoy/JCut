#include "facedetections_runtime.h"
#include "facedetections_time_mapping.h"
#include "transcript_engine.h"

#include <QDir>
#include <QJsonDocument>
#include <QTemporaryDir>
#include <QtTest/QtTest>

namespace {

QJsonObject rawTrackDetection(qint64 frame, double x, double y, double box, double score)
{
    return QJsonObject{
        {QStringLiteral("frame"), frame},
        {QStringLiteral("x"), x},
        {QStringLiteral("y"), y},
        {QStringLiteral("box"), box},
        {QStringLiteral("score"), score}
    };
}

} // namespace

class FacestreamProcessedArtifactTest : public QObject {
    Q_OBJECT

private slots:
    void processedArtifactRoundTripFiltersDialogueAndBuildsStreams()
    {
        QTemporaryDir tempDir;
        QVERIFY(tempDir.isValid());

        const QString transcriptPath =
            QDir(tempDir.path()).filePath(QStringLiteral("session_01.json"));
        const QString clipId = QStringLiteral("clip_dialogue");

        editor::TranscriptEngine engine;
        const QJsonObject transcriptRoot{
            {QStringLiteral("segments"), QJsonArray{
                 QJsonObject{
                     {QStringLiteral("speaker"), QStringLiteral("SPEAKER_A")},
                     {QStringLiteral("words"), QJsonArray{
                          QJsonObject{
                              {QStringLiteral("word"), QStringLiteral("hello")},
                              {QStringLiteral("speaker"), QStringLiteral("SPEAKER_A")},
                              {QStringLiteral("start"), 0.0},
                              {QStringLiteral("end"), 0.20}
                          },
                          QJsonObject{
                              {QStringLiteral("word"), QStringLiteral("there")},
                              {QStringLiteral("speaker"), QStringLiteral("SPEAKER_A")},
                              {QStringLiteral("start"), 0.30},
                              {QStringLiteral("end"), 0.50}
                          }
                      }}
                 }
             }}
        };
        QVERIFY(engine.saveTranscriptJson(transcriptPath, QJsonDocument(transcriptRoot)));

        const QJsonArray rawTracks{
            QJsonObject{
                {QStringLiteral("track_id"), 3},
                {QStringLiteral("last_frame"), 18},
                {QStringLiteral("length"), 3},
                {QStringLiteral("detections"), QJsonArray{
                     rawTrackDetection(0, 0.25, 0.35, 0.18, 0.95),
                     rawTrackDetection(12, 0.26, 0.36, 0.18, 0.93),
                     rawTrackDetection(18, 0.27, 0.37, 0.18, 0.92)
                 }}
            },
            QJsonObject{
                {QStringLiteral("track_id"), 8},
                {QStringLiteral("last_frame"), 9},
                {QStringLiteral("length"), 1},
                {QStringLiteral("detections"), QJsonArray{
                     rawTrackDetection(9, 0.72, 0.40, 0.16, 0.90)
                 }}
            }
        };
        const QJsonArray rawFrames{
            QJsonObject{{QStringLiteral("frame"), 0}},
            QJsonObject{{QStringLiteral("frame"), 12}},
            QJsonObject{{QStringLiteral("frame"), 18}}
        };

        const QJsonObject rawContinuityRoot =
            jcut::facedetections::buildContinuityRoot(
                QStringLiteral("run_dialogue"),
                true,
                0,
                18,
                QJsonArray{},
                rawTracks,
                rawFrames,
                QStringLiteral("scrfd"));

        QJsonObject savedRawArtifact;
        QVERIFY(jcut::facedetections::saveContinuityArtifact(
            transcriptPath,
            clipId,
            rawContinuityRoot,
            &savedRawArtifact));
        QVERIFY(engine.loadFacestreamArtifact(transcriptPath, &savedRawArtifact));
        const QJsonObject rawByClip =
            savedRawArtifact.value(QStringLiteral("continuity_facedetections_by_clip")).toObject();
        QVERIFY(rawByClip.contains(clipId));

        QJsonObject savedProcessedArtifact;
        QVERIFY(jcut::facedetections::saveProcessedContinuityArtifact(
            transcriptPath,
            clipId,
            rawContinuityRoot,
            transcriptRoot,
            &savedProcessedArtifact));
        QVERIFY(engine.loadFacestreamProcessedArtifact(transcriptPath, &savedProcessedArtifact));

        const QJsonObject processedByClip =
            savedProcessedArtifact.value(QStringLiteral("continuity_facedetections_by_clip")).toObject();
        const QJsonObject processedRoot = processedByClip.value(clipId).toObject();
        QVERIFY(jcut::facedetections::continuityRootHasStoredPayload(processedRoot));
        QVERIFY(jcut::facedetections::continuityRootHasTracks(processedRoot, transcriptRoot));
        QCOMPARE(processedRoot.value(QStringLiteral("streams_frame_domain")).toString(),
                 QStringLiteral("source_absolute"));

        const QJsonArray processedStreams =
            processedRoot.value(QStringLiteral("streams")).toArray();
        QCOMPARE(processedStreams.size(), 2);

        const QJsonObject stream0 = processedStreams.at(0).toObject();
        QCOMPARE(stream0.value(QStringLiteral("track_id")).toInt(), 3);
        QCOMPARE(stream0.value(QStringLiteral("frame_domain")).toString(),
                 QStringLiteral("source_absolute"));
        const QJsonArray keyframes0 = stream0.value(QStringLiteral("keyframes")).toArray();
        QCOMPARE(keyframes0.size(), 2);
        QCOMPARE(keyframes0.at(0).toObject().value(QStringLiteral("frame")).toInt(), 0);
        QCOMPARE(keyframes0.at(1).toObject().value(QStringLiteral("frame")).toInt(), 12);

        const QJsonObject stream1 = processedStreams.at(1).toObject();
        QCOMPARE(stream1.value(QStringLiteral("track_id")).toInt(), 8);
        const QJsonArray keyframes1 = stream1.value(QStringLiteral("keyframes")).toArray();
        QCOMPARE(keyframes1.size(), 1);
        QCOMPARE(keyframes1.at(0).toObject().value(QStringLiteral("frame")).toInt(), 9);

        const QJsonObject rawRootForRuntime = rawByClip.value(clipId).toObject();
        const QJsonArray derivedStreams =
            jcut::facedetections::continuityStreamsForRoot(rawRootForRuntime, transcriptRoot);
        QCOMPARE(derivedStreams.size(), 2);
        QCOMPARE(derivedStreams.at(0).toObject().value(QStringLiteral("keyframes")).toArray().size(), 2);
    }

    void sourceAbsoluteScanRangeUsesSourceDurationNotTimelineDuration()
    {
        TimelineClip clip;
        clip.sourceInFrame = 0;
        clip.durationFrames = 398252;
        clip.sourceDurationFrames = 796503;

        const auto range = facedetectionsSourceAbsoluteScanRangeForClip(clip);

        QVERIFY(range.valid);
        QCOMPARE(range.startFrame, 0LL);
        QCOMPARE(range.endFrameExclusive, 796503LL);
        QCOMPARE(range.frameCount, 796503LL);
    }

    void sourceAbsoluteScanRangeRejectsInvalidSourceDuration()
    {
        TimelineClip clip;
        clip.sourceInFrame = 100;
        clip.durationFrames = 398252;
        clip.sourceDurationFrames = 100;

        const auto range = facedetectionsSourceAbsoluteScanRangeForClip(clip);

        QVERIFY(!range.valid);
        QVERIFY(range.error.contains(QStringLiteral("sourceInFrame=100")));
    }
};

QTEST_MAIN(FacestreamProcessedArtifactTest)
#include "test_facedetections_processed_artifact.moc"
