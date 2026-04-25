#include <QtTest/QtTest>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTemporaryDir>
#include <cmath>

#include "../editor_shared.h"
#include "../transcript_engine.h"

using namespace editor;

namespace {

bool writeTranscriptJson(const QString& path, const QJsonArray& words) {
    QJsonObject segment;
    segment[QStringLiteral("words")] = words;
    QJsonArray segments;
    segments.push_back(segment);
    QJsonObject root;
    root[QStringLiteral("segments")] = segments;

    QFile file(path);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        return false;
    }
    const QByteArray payload = QJsonDocument(root).toJson(QJsonDocument::Indented);
    return file.write(payload) == payload.size();
}

bool writeTranscriptDocument(const QString& path, const QJsonObject& root) {
    QFile file(path);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        return false;
    }
    const QByteArray payload = QJsonDocument(root).toJson(QJsonDocument::Indented);
    return file.write(payload) == payload.size();
}

QJsonObject wordObj(double start, double end, bool skipped) {
    QJsonObject obj;
    obj[QStringLiteral("word")] = QStringLiteral("hello");
    obj[QStringLiteral("start")] = start;
    obj[QStringLiteral("end")] = end;
    obj[QStringLiteral("skipped")] = skipped;
    return obj;
}

TimelineClip makeAudioClip(const QString& id,
                           const QString& filePath,
                           int64_t startFrame,
                           int64_t durationFrames) {
    TimelineClip clip;
    clip.id = id;
    clip.filePath = filePath;
    clip.mediaType = ClipMediaType::Audio;
    clip.hasAudio = true;
    clip.startFrame = startFrame;
    clip.durationFrames = durationFrames;
    clip.sourceInFrame = 0;
    clip.sourceDurationFrames = durationFrames;
    return clip;
}

}

class TestTranscriptLogic : public QObject {
    Q_OBJECT

private slots:
    void init() {
        clearAllActiveTranscriptPaths();
    }

    void cleanup() {
        clearAllActiveTranscriptPaths();
    }

    void testSpeechFilterUsesActiveTranscriptCut();
    void testAllSkippedWordsYieldNoSpeechRanges();
    void testEmptyBaseRangesUseInclusiveClipEnd();
    void testSpeakerTrackingInterpolatesOverlayLocation();
    void testSpeakerLocationFallsBackToProfileLocation();
    void testSpeakerManualModeDoesNotOverrideOverlayTranslation();
    void testTranscriptFrameMappingUsesSourceSeconds();
};

void TestTranscriptLogic::testSpeechFilterUsesActiveTranscriptCut() {
    QTemporaryDir dir;
    QVERIFY(dir.isValid());

    const QString clipPath = dir.filePath(QStringLiteral("clip.wav"));
    QVERIFY(QFile(clipPath).open(QIODevice::WriteOnly));

    const QString editablePath = transcriptEditablePathForClipFile(clipPath);
    const QString v2Path = dir.filePath(QStringLiteral("clip_editable_v2.json"));

    QJsonArray skippedOnlyWords;
    skippedOnlyWords.push_back(wordObj(0.0, 0.5, true));
    QVERIFY(writeTranscriptJson(editablePath, skippedOnlyWords));

    QJsonArray activeCutWords;
    activeCutWords.push_back(wordObj(0.0, 0.5, false));
    QVERIFY(writeTranscriptJson(v2Path, activeCutWords));

    setActiveTranscriptPathForClipFile(clipPath, v2Path);

    TranscriptEngine engine;
    const TimelineClip clip = makeAudioClip(QStringLiteral("clip1"), clipPath, 100, 30);
    const QVector<ExportRangeSegment> baseRanges{ExportRangeSegment{100, 129}};
    const QVector<ExportRangeSegment> ranges =
        engine.transcriptWordExportRanges(baseRanges, {clip}, {}, 0, 0);

    QVERIFY(!ranges.isEmpty());
    QCOMPARE(ranges.constFirst().startFrame, int64_t(100));
    QCOMPARE(ranges.constFirst().endFrame, int64_t(114));
}

void TestTranscriptLogic::testAllSkippedWordsYieldNoSpeechRanges() {
    QTemporaryDir dir;
    QVERIFY(dir.isValid());

    const QString clipPath = dir.filePath(QStringLiteral("clip.wav"));
    QVERIFY(QFile(clipPath).open(QIODevice::WriteOnly));

    const QString editablePath = transcriptEditablePathForClipFile(clipPath);
    QJsonArray skippedOnlyWords;
    skippedOnlyWords.push_back(wordObj(0.0, 0.5, true));
    QVERIFY(writeTranscriptJson(editablePath, skippedOnlyWords));
    setActiveTranscriptPathForClipFile(clipPath, editablePath);

    TranscriptEngine engine;
    const TimelineClip clip = makeAudioClip(QStringLiteral("clip2"), clipPath, 200, 30);
    const QVector<ExportRangeSegment> baseRanges{ExportRangeSegment{200, 229}};
    const QVector<ExportRangeSegment> ranges =
        engine.transcriptWordExportRanges(baseRanges, {clip}, {}, 0, 0);

    QVERIFY(ranges.isEmpty());
}

void TestTranscriptLogic::testEmptyBaseRangesUseInclusiveClipEnd() {
    QTemporaryDir dir;
    QVERIFY(dir.isValid());

    const QString clipPath = dir.filePath(QStringLiteral("clip.wav"));
    QVERIFY(QFile(clipPath).open(QIODevice::WriteOnly));

    TranscriptEngine engine;
    const TimelineClip clip = makeAudioClip(QStringLiteral("clip3"), clipPath, 0, 10);
    const QVector<ExportRangeSegment> ranges =
        engine.transcriptWordExportRanges({}, {clip}, {}, 0, 0);

    QCOMPARE(ranges.size(), 1);
    QCOMPARE(ranges.constFirst().startFrame, int64_t(0));
    QCOMPARE(ranges.constFirst().endFrame, int64_t(9));
}

void TestTranscriptLogic::testSpeakerTrackingInterpolatesOverlayLocation() {
    QTemporaryDir dir;
    QVERIFY(dir.isValid());

    const QString transcriptPath = dir.filePath(QStringLiteral("clip.json"));
    QJsonObject word;
    word[QStringLiteral("word")] = QStringLiteral("hello");
    word[QStringLiteral("start")] = 0.0;
    word[QStringLiteral("end")] = 1.0;
    word[QStringLiteral("speaker")] = QStringLiteral("S1");
    QJsonObject segment;
    segment[QStringLiteral("words")] = QJsonArray{word};
    QJsonObject root;
    root[QStringLiteral("segments")] = QJsonArray{segment};

    QJsonObject kf1;
    kf1[QStringLiteral("frame")] = 0;
    kf1[QStringLiteral("x")] = 0.2;
    kf1[QStringLiteral("y")] = 0.4;
    QJsonObject kf2;
    kf2[QStringLiteral("frame")] = 30;
    kf2[QStringLiteral("x")] = 0.8;
    kf2[QStringLiteral("y")] = 0.7;
    QJsonObject tracking;
    tracking[QStringLiteral("mode")] = QStringLiteral("AutoTrackLinear");
    tracking[QStringLiteral("keyframes")] = QJsonArray{kf1, kf2};
    QJsonObject location;
    location[QStringLiteral("x")] = 0.5;
    location[QStringLiteral("y")] = 0.5;
    QJsonObject profile;
    profile[QStringLiteral("location")] = location;
    profile[QStringLiteral("tracking")] = tracking;
    QJsonObject profiles;
    profiles[QStringLiteral("S1")] = profile;
    root[QStringLiteral("speaker_profiles")] = profiles;

    QVERIFY(writeTranscriptDocument(transcriptPath, root));
    invalidateTranscriptSpeakerProfileCache(transcriptPath);

    const QVector<TranscriptSection> sections = loadTranscriptSections(transcriptPath);
    QVERIFY(!sections.isEmpty());

    bool ok = false;
    const QPointF pos = transcriptSpeakerLocationForSourceFrame(transcriptPath, sections, 15, &ok);
    QVERIFY(ok);
    QVERIFY(std::abs(pos.x() - 0.5) < 0.001);
    QVERIFY(std::abs(pos.y() - 0.55) < 0.001);
}

void TestTranscriptLogic::testSpeakerLocationFallsBackToProfileLocation() {
    QTemporaryDir dir;
    QVERIFY(dir.isValid());

    const QString transcriptPath = dir.filePath(QStringLiteral("clip.json"));
    QJsonObject word;
    word[QStringLiteral("word")] = QStringLiteral("hello");
    word[QStringLiteral("start")] = 0.0;
    word[QStringLiteral("end")] = 1.0;
    word[QStringLiteral("speaker")] = QStringLiteral("S2");
    QJsonObject segment;
    segment[QStringLiteral("words")] = QJsonArray{word};
    QJsonObject root;
    root[QStringLiteral("segments")] = QJsonArray{segment};

    QJsonObject location;
    location[QStringLiteral("x")] = 0.33;
    location[QStringLiteral("y")] = 0.77;
    QJsonObject profile;
    profile[QStringLiteral("location")] = location;
    QJsonObject tracking;
    tracking[QStringLiteral("mode")] = QStringLiteral("AnchorHold");
    profile[QStringLiteral("tracking")] = tracking;
    QJsonObject profiles;
    profiles[QStringLiteral("S2")] = profile;
    root[QStringLiteral("speaker_profiles")] = profiles;

    QVERIFY(writeTranscriptDocument(transcriptPath, root));
    invalidateTranscriptSpeakerProfileCache(transcriptPath);

    const QVector<TranscriptSection> sections = loadTranscriptSections(transcriptPath);
    QVERIFY(!sections.isEmpty());

    bool ok = false;
    const QPointF pos = transcriptSpeakerLocationForSourceFrame(transcriptPath, sections, 10, &ok);
    QVERIFY(ok);
    QVERIFY(std::abs(pos.x() - 0.33) < 0.001);
    QVERIFY(std::abs(pos.y() - 0.77) < 0.001);
}

void TestTranscriptLogic::testSpeakerManualModeDoesNotOverrideOverlayTranslation() {
    QTemporaryDir dir;
    QVERIFY(dir.isValid());

    const QString transcriptPath = dir.filePath(QStringLiteral("clip.json"));
    QJsonObject word;
    word[QStringLiteral("word")] = QStringLiteral("hello");
    word[QStringLiteral("start")] = 0.0;
    word[QStringLiteral("end")] = 1.0;
    word[QStringLiteral("speaker")] = QStringLiteral("S3");
    QJsonObject segment;
    segment[QStringLiteral("words")] = QJsonArray{word};
    QJsonObject root;
    root[QStringLiteral("segments")] = QJsonArray{segment};

    QJsonObject location;
    location[QStringLiteral("x")] = 0.22;
    location[QStringLiteral("y")] = 0.66;
    QJsonObject tracking;
    tracking[QStringLiteral("mode")] = QStringLiteral("Manual");
    QJsonObject profile;
    profile[QStringLiteral("location")] = location;
    profile[QStringLiteral("tracking")] = tracking;
    QJsonObject profiles;
    profiles[QStringLiteral("S3")] = profile;
    root[QStringLiteral("speaker_profiles")] = profiles;

    QVERIFY(writeTranscriptDocument(transcriptPath, root));
    invalidateTranscriptSpeakerProfileCache(transcriptPath);

    const QVector<TranscriptSection> sections = loadTranscriptSections(transcriptPath);
    QVERIFY(!sections.isEmpty());

    bool ok = false;
    const QPointF pos = transcriptSpeakerLocationForSourceFrame(transcriptPath, sections, 10, &ok);
    QVERIFY(!ok);
    QCOMPARE(pos, QPointF());
}

void TestTranscriptLogic::testTranscriptFrameMappingUsesSourceSeconds() {
    TimelineClip clip;
    clip.id = QStringLiteral("clip-non30fps");
    clip.mediaType = ClipMediaType::Video;
    clip.hasAudio = true;
    clip.startFrame = 100;
    clip.durationFrames = 300;
    clip.sourceInFrame = 120;      // 2.0s at 60fps
    clip.sourceDurationFrames = 900;
    clip.sourceFps = 60.0;
    clip.playbackRate = 1.0;

    const int64_t timelineSample = frameToSamples(clip.startFrame + 15); // 0.5s into clip on timeline
    const int64_t transcriptFrame =
        transcriptFrameForClipAtTimelineSample(clip, timelineSample, {});

    // 2.0s source-in + 0.5s timeline offset => 2.5s transcript time => frame 75 at 30fps.
    QCOMPARE(transcriptFrame, int64_t(75));
}

QTEST_MAIN(TestTranscriptLogic)
#include "test_transcript_logic.moc"
