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
    void testSpeakerTrackingIgnoresBoxSizeForPositionInterpolation();
    void testSpeakerLocationFallsBackToProfileLocation();
    void testSpeakerManualModeDoesNotOverrideOverlayTranslation();
    void testSpeakerTrackingExplicitDisabledOverridesKeyframes();
    void testSpeakerTrackingReferencePointsModeStaysDisabled();
    void testTranscriptFrameMappingUsesSourceSeconds();
    void testTranscriptOverlaySizingHelpersClampToBox();
    void testTranscriptOverlayLayoutHelperMatchesSectionLayout();
    void testTranscriptOverlayHtmlUsesQtRichTextRgbColors();
    void testTranscriptOverlayRectInOutputSpaceUsesSpeakerLocation();
    void testTranscriptOverlayRectInOutputSpaceFallsBackToManualTranslation();
    void testTranscriptOverlayManualPlacementOverridesSpeakerTracking();
    void testSpeakerTrackingConfigIncludesAutoTrackStepFrames();
    void testSpeakerTrackingConfigPatchValidatesAutoTrackStepFrames();
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
    tracking[QStringLiteral("enabled")] = true;
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

void TestTranscriptLogic::testSpeakerTrackingIgnoresBoxSizeForPositionInterpolation() {
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
    kf1[QStringLiteral("x")] = 0.1;
    kf1[QStringLiteral("y")] = 0.2;
    kf1[QStringLiteral("box_size")] = 0.14;
    QJsonObject kf2;
    kf2[QStringLiteral("frame")] = 30;
    kf2[QStringLiteral("x")] = 0.7;
    kf2[QStringLiteral("y")] = 0.8;
    kf2[QStringLiteral("box_size")] = 0.22;
    QJsonObject tracking;
    tracking[QStringLiteral("mode")] = QStringLiteral("AutoTrackLinear");
    tracking[QStringLiteral("enabled")] = true;
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
    QVERIFY(std::abs(pos.x() - 0.4) < 0.001);
    QVERIFY(std::abs(pos.y() - 0.5) < 0.001);
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
    tracking[QStringLiteral("enabled")] = true;
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

void TestTranscriptLogic::testSpeakerTrackingExplicitDisabledOverridesKeyframes() {
    QTemporaryDir dir;
    QVERIFY(dir.isValid());

    const QString transcriptPath = dir.filePath(QStringLiteral("clip.json"));
    QJsonObject word;
    word[QStringLiteral("word")] = QStringLiteral("hello");
    word[QStringLiteral("start")] = 0.0;
    word[QStringLiteral("end")] = 1.0;
    word[QStringLiteral("speaker")] = QStringLiteral("S9");
    QJsonObject segment;
    segment[QStringLiteral("words")] = QJsonArray{word};
    QJsonObject root;
    root[QStringLiteral("segments")] = QJsonArray{segment};

    QJsonObject kf;
    kf[QStringLiteral("frame")] = 0;
    kf[QStringLiteral("x")] = 0.2;
    kf[QStringLiteral("y")] = 0.8;
    QJsonObject tracking;
    tracking[QStringLiteral("mode")] = QStringLiteral("AutoTrackLinear");
    tracking[QStringLiteral("enabled")] = false;
    tracking[QStringLiteral("keyframes")] = QJsonArray{kf};
    QJsonObject profile;
    profile[QStringLiteral("tracking")] = tracking;
    QJsonObject profiles;
    profiles[QStringLiteral("S9")] = profile;
    root[QStringLiteral("speaker_profiles")] = profiles;

    QVERIFY(writeTranscriptDocument(transcriptPath, root));
    invalidateTranscriptSpeakerProfileCache(transcriptPath);
    const QVector<TranscriptSection> sections = loadTranscriptSections(transcriptPath);
    QVERIFY(!sections.isEmpty());

    bool ok = true;
    const QPointF pos = transcriptSpeakerLocationForSourceFrame(transcriptPath, sections, 10, &ok);
    QVERIFY(!ok);
    QCOMPARE(pos, QPointF());
}

void TestTranscriptLogic::testSpeakerTrackingReferencePointsModeStaysDisabled() {
    QTemporaryDir dir;
    QVERIFY(dir.isValid());

    const QString transcriptPath = dir.filePath(QStringLiteral("clip.json"));
    QJsonObject word;
    word[QStringLiteral("word")] = QStringLiteral("hello");
    word[QStringLiteral("start")] = 0.0;
    word[QStringLiteral("end")] = 1.0;
    word[QStringLiteral("speaker")] = QStringLiteral("S10");
    QJsonObject segment;
    segment[QStringLiteral("words")] = QJsonArray{word};
    QJsonObject root;
    root[QStringLiteral("segments")] = QJsonArray{segment};

    QJsonObject tracking;
    tracking[QStringLiteral("mode")] = QStringLiteral("ReferencePoints");
    tracking[QStringLiteral("enabled")] = true;
    QJsonObject profile;
    profile[QStringLiteral("tracking")] = tracking;
    QJsonObject profiles;
    profiles[QStringLiteral("S10")] = profile;
    root[QStringLiteral("speaker_profiles")] = profiles;

    QVERIFY(writeTranscriptDocument(transcriptPath, root));
    invalidateTranscriptSpeakerProfileCache(transcriptPath);
    const QVector<TranscriptSection> sections = loadTranscriptSections(transcriptPath);
    QVERIFY(!sections.isEmpty());

    bool ok = true;
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

void TestTranscriptLogic::testTranscriptOverlaySizingHelpersClampToBox() {
    TimelineClip clip;
    clip.transcriptOverlay.maxLines = 6;
    clip.transcriptOverlay.maxCharsPerLine = 40;
    clip.transcriptOverlay.fontPointSize = 42;
    clip.transcriptOverlay.boxWidth = 120.0;
    clip.transcriptOverlay.boxHeight = 60.0;

    const int lines = transcriptOverlayEffectiveLinesForBox(clip);
    const int chars = transcriptOverlayEffectiveCharsForBox(clip);

    QVERIFY(lines >= 1);
    QVERIFY(chars >= 1);
    QVERIFY(lines <= clip.transcriptOverlay.maxLines);
    QVERIFY(chars <= clip.transcriptOverlay.maxCharsPerLine);

    // With a tiny box at large font size, helper must clamp far below requested max.
    QVERIFY(lines < clip.transcriptOverlay.maxLines);
    QVERIFY(chars < clip.transcriptOverlay.maxCharsPerLine);
}

void TestTranscriptLogic::testTranscriptOverlayLayoutHelperMatchesSectionLayout() {
    TimelineClip clip;
    clip.transcriptOverlay.maxLines = 3;
    clip.transcriptOverlay.maxCharsPerLine = 24;
    clip.transcriptOverlay.fontPointSize = 24;
    clip.transcriptOverlay.boxWidth = 640.0;
    clip.transcriptOverlay.boxHeight = 180.0;
    clip.transcriptOverlay.autoScroll = true;

    TranscriptSection section;
    section.startFrame = 0;
    section.endFrame = 29;
    section.text = QStringLiteral("alpha beta");

    TranscriptWord w1;
    w1.startFrame = 0;
    w1.endFrame = 9;
    w1.text = QStringLiteral("alpha");
    w1.speaker = QStringLiteral("S1");
    section.words.push_back(w1);

    TranscriptWord w2;
    w2.startFrame = 10;
    w2.endFrame = 19;
    w2.text = QStringLiteral("beta");
    w2.speaker = QStringLiteral("S1");
    section.words.push_back(w2);

    const QVector<TranscriptSection> sections{section};
    const int sourceFrame = 12;

    const TranscriptOverlayLayout expected = layoutTranscriptSection(
        section,
        sourceFrame,
        transcriptOverlayEffectiveCharsForBox(clip),
        transcriptOverlayEffectiveLinesForBox(clip),
        clip.transcriptOverlay.autoScroll);
    const TranscriptOverlayLayout actual =
        transcriptOverlayLayoutAtSourceFrame(clip, sections, sourceFrame);

    QCOMPARE(actual.lines.size(), expected.lines.size());
    QCOMPARE(actual.truncatedTop, expected.truncatedTop);
    QCOMPARE(actual.truncatedBottom, expected.truncatedBottom);
    QVERIFY(!actual.lines.isEmpty());
    for (int i = 0; i < actual.lines.size(); ++i) {
        QCOMPARE(actual.lines.at(i).words, expected.lines.at(i).words);
        QCOMPARE(actual.lines.at(i).activeWord, expected.lines.at(i).activeWord);
    }
}

void TestTranscriptLogic::testTranscriptOverlayHtmlUsesQtRichTextRgbColors() {
    TranscriptOverlayLayout layout;
    TranscriptOverlayLine line;
    line.words = {QStringLiteral("hello"), QStringLiteral("world")};
    line.activeWord = 1;
    layout.lines.push_back(line);

    const QString html = transcriptOverlayHtml(layout,
                                               QColor(QStringLiteral("#ffffffff")),
                                               QColor(QStringLiteral("#ff181818")),
                                               QColor(QStringLiteral("#fffff2a8")));

    QVERIFY(html.contains(QStringLiteral("#ffffff")));
    QVERIFY(html.contains(QStringLiteral("#181818")));
    QVERIFY(html.contains(QStringLiteral("#fff2a8")));
    QVERIFY(!html.contains(QStringLiteral("#ffffffff")));
    QVERIFY(!html.contains(QStringLiteral("#ff181818")));
    QVERIFY(!html.contains(QStringLiteral("#fffff2a8")));
}

void TestTranscriptLogic::testTranscriptOverlayRectInOutputSpaceUsesSpeakerLocation() {
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

    QJsonObject kf;
    kf[QStringLiteral("frame")] = 0;
    kf[QStringLiteral("x")] = 0.25;
    kf[QStringLiteral("y")] = 0.75;
    QJsonObject tracking;
    tracking[QStringLiteral("mode")] = QStringLiteral("AutoTrackLinear");
    tracking[QStringLiteral("enabled")] = true;
    tracking[QStringLiteral("keyframes")] = QJsonArray{kf};
    QJsonObject profile;
    profile[QStringLiteral("tracking")] = tracking;
    QJsonObject profiles;
    profiles[QStringLiteral("S1")] = profile;
    root[QStringLiteral("speaker_profiles")] = profiles;

    QVERIFY(writeTranscriptDocument(transcriptPath, root));
    invalidateTranscriptSpeakerProfileCache(transcriptPath);
    const QVector<TranscriptSection> sections = loadTranscriptSections(transcriptPath);
    QVERIFY(!sections.isEmpty());

    TimelineClip clip;
    clip.transcriptOverlay.boxWidth = 400.0;
    clip.transcriptOverlay.boxHeight = 200.0;
    clip.transcriptOverlay.translationX = 999.0; // ignored when speaker location resolves
    clip.transcriptOverlay.translationY = 999.0;
    const QSize outputSize(1080, 1920);
    const QRectF rect = transcriptOverlayRectInOutputSpace(
        clip, outputSize, transcriptPath, sections, /*sourceFrame=*/15);

    QVERIFY(std::abs(rect.center().x() - 270.0) < 0.001);
    QVERIFY(std::abs(rect.center().y() - 1440.0) < 0.001);
    QVERIFY(std::abs(rect.width() - 400.0) < 0.001);
    QVERIFY(std::abs(rect.height() - 200.0) < 0.001);
}

void TestTranscriptLogic::testTranscriptOverlayRectInOutputSpaceFallsBackToManualTranslation() {
    TimelineClip clip;
    clip.transcriptOverlay.boxWidth = 500.0;
    clip.transcriptOverlay.boxHeight = 250.0;
    clip.transcriptOverlay.translationX = 120.0;
    clip.transcriptOverlay.translationY = -300.0;

    const QSize outputSize(1080, 1920);
    const QRectF rect = transcriptOverlayRectInOutputSpace(
        clip, outputSize, QString(), QVector<TranscriptSection>{}, /*sourceFrame=*/0);

    QVERIFY(std::abs(rect.center().x() - (540.0 + 120.0)) < 0.001);
    QVERIFY(std::abs(rect.center().y() - (960.0 - 300.0)) < 0.001);
    QVERIFY(std::abs(rect.width() - 500.0) < 0.001);
    QVERIFY(std::abs(rect.height() - 250.0) < 0.001);
}

void TestTranscriptLogic::testTranscriptOverlayManualPlacementOverridesSpeakerTracking() {
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

    QJsonObject kf;
    kf[QStringLiteral("frame")] = 0;
    kf[QStringLiteral("x")] = 0.1;
    kf[QStringLiteral("y")] = 0.9;
    QJsonObject tracking;
    tracking[QStringLiteral("mode")] = QStringLiteral("AutoTrackLinear");
    tracking[QStringLiteral("enabled")] = true;
    tracking[QStringLiteral("keyframes")] = QJsonArray{kf};
    QJsonObject profile;
    profile[QStringLiteral("tracking")] = tracking;
    QJsonObject profiles;
    profiles[QStringLiteral("S1")] = profile;
    root[QStringLiteral("speaker_profiles")] = profiles;

    QVERIFY(writeTranscriptDocument(transcriptPath, root));
    invalidateTranscriptSpeakerProfileCache(transcriptPath);
    const QVector<TranscriptSection> sections = loadTranscriptSections(transcriptPath);
    QVERIFY(!sections.isEmpty());

    TimelineClip clip;
    clip.transcriptOverlay.boxWidth = 400.0;
    clip.transcriptOverlay.boxHeight = 200.0;
    clip.transcriptOverlay.translationX = -220.0;
    clip.transcriptOverlay.translationY = 50.0;
    clip.transcriptOverlay.useManualPlacement = true;
    const QSize outputSize(1080, 1920);

    const QRectF rect = transcriptOverlayRectInOutputSpace(
        clip, outputSize, transcriptPath, sections, /*sourceFrame=*/15);

    QVERIFY(std::abs(rect.center().x() - (540.0 - 220.0)) < 0.001);
    QVERIFY(std::abs(rect.center().y() - (960.0 + 50.0)) < 0.001);
}

void TestTranscriptLogic::testSpeakerTrackingConfigIncludesAutoTrackStepFrames() {
    const QJsonObject config = transcriptSpeakerTrackingConfigSnapshot();
    QVERIFY(config.contains(QStringLiteral("auto_track_step_frames")));
    QVERIFY(config.value(QStringLiteral("auto_track_step_frames")).toInt(0) >= 1);
}

void TestTranscriptLogic::testSpeakerTrackingConfigPatchValidatesAutoTrackStepFrames() {
    QJsonObject patch;
    patch[QStringLiteral("auto_track_step_frames")] = 4;
    QString error;
    QVERIFY(applyTranscriptSpeakerTrackingConfigPatch(patch, &error));
    QCOMPARE(transcriptSpeakerTrackingConfigSnapshot()
                 .value(QStringLiteral("auto_track_step_frames"))
                 .toInt(),
             4);

    QJsonObject invalidPatch;
    invalidPatch[QStringLiteral("auto_track_step_frames")] = 0;
    error.clear();
    QVERIFY(!applyTranscriptSpeakerTrackingConfigPatch(invalidPatch, &error));
    QVERIFY(!error.isEmpty());
}

QTEST_MAIN(TestTranscriptLogic)
#include "test_transcript_logic.moc"
