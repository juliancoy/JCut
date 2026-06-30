#include <QtTest/QtTest>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTemporaryDir>
#include <cmath>

#include "../editor_shared.h"
#include "../editor_shared_keyframes.h"
#include "../editor_shared_transcript.h"
#include "../clip_serialization.h"
#include "../transcript_overlay_cache_key.h"
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
        setTranscriptSourceRootPath(QString());
    }

    void cleanup() {
        clearAllActiveTranscriptPaths();
        setTranscriptSourceRootPath(QString());
    }

    void testSpeechFilterUsesActiveTranscriptCut();
    void testAllSkippedWordsYieldNoSpeechRanges();
    void testSpeechFilterIgnoresMalformedWordTiming();
    void testSpeechFilterRangesHonorPlaybackRateWithoutMarkers();
    void testEmptyBaseRangesUseInclusiveClipEnd();
    void testRuntimeSidecarPathPrefersEditableLegacyArtifact();
    void testTranscriptPathsAreIndependentPerAudioStream();
    void testTranscriptSourceRootResolvesRelativeClipPaths();
    void testSpeakerTrackingInterpolatesOverlayLocation();
    void testSpeakerTrackingIgnoresBoxSizeForPositionInterpolation();
    void testSpeakerLocationFallsBackToProfileLocation();
    void testSpeakerManualModeDoesNotOverrideOverlayTranslation();
    void testSpeakerTrackingExplicitDisabledOverridesKeyframes();
    void testSpeakerTrackingReferencePointsModeStaysDisabled();
    void testSpeakerFramingRotatesAroundDetectedFaceBox();
    void testTranscriptFrameMappingUsesSourceSeconds();
    void testTranscriptFrameMappingFromPresentedSourceFrame();
    void testAudioVideoCaptionMappingStaysSampleAccurateAtOnePointFiveX();
    void testTranscriptOverlaySizingHelpersClampToBox();
    void testTranscriptOverlayLayoutHelperMatchesSectionLayout();
    void testTranscriptOverlayRespectsWordPadding();
    void testTranscriptOverlayGuardsWordPaddingEdges();
    void testTranscriptOverlaySpeakerLookupReturnsActiveRange();
    void testTranscriptSpeakerTitleUsesOverlayWordPadding();
    void testTranscriptOverlayHtmlUsesQtRichTextRgbColors();
    void testTranscriptOverlayHtmlCanDisableCurrentWordHighlight();
    void testTranscriptOverlayRectInOutputSpaceUsesSpeakerLocation();
    void testTranscriptOverlayRectInOutputSpaceFallsBackToManualTranslation();
    void testTranscriptOverlayManualTranslationUsesNormalizedOffsets();
    void testTranscriptOverlayManualPlacementOverridesSpeakerTracking();
    void testTranscriptOverlayStyleCacheMaterialIncludesTransformFields();
    void testTranscriptOverlayProjectLoadNormalizesUnreadableGeometry();
    void testSpeakerFramingEnabledKeyframesOverrideGlobalFallback();
    void testSpeakerFramingRuntimeSpeakerDescendsFromTranscript();
    void testSpeakerFramingGapHoldPersistsTranscriptSpeaker();
    void testSpeakerFramingSmoothingAppliesToAssignedContinuityTracks();
    void testDynamicSpeakerFramingInterpolatesFractionalPlaybackPosition();
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

void TestTranscriptLogic::testSpeechFilterIgnoresMalformedWordTiming() {
    QTemporaryDir dir;
    QVERIFY(dir.isValid());

    const QString clipPath = dir.filePath(QStringLiteral("clip.wav"));
    QVERIFY(QFile(clipPath).open(QIODevice::WriteOnly));

    QJsonArray words;
    words.push_back(wordObj(10.0, 10.2, false));
    QJsonObject malformed = wordObj(1.0, 20.0, false);
    malformed[QStringLiteral("word")] = QStringLiteral("bad-timing");
    words.push_back(malformed);
    words.push_back(wordObj(20.0, 20.2, false));

    QJsonObject segment;
    segment[QStringLiteral("start")] = 9.8;
    segment[QStringLiteral("end")] = 20.4;
    segment[QStringLiteral("words")] = words;
    QJsonArray segments;
    segments.push_back(segment);
    QJsonObject root;
    root[QStringLiteral("segments")] = segments;

    const QString editablePath = transcriptEditablePathForClipFile(clipPath);
    QVERIFY(writeTranscriptDocument(editablePath, root));
    setActiveTranscriptPathForClipFile(clipPath, editablePath);

    TranscriptEngine engine;
    const TimelineClip clip = makeAudioClip(QStringLiteral("clip-malformed"), clipPath, 0, 700);
    const QVector<ExportRangeSegment> ranges =
        engine.transcriptWordExportRanges({ExportRangeSegment{0, 699}}, {clip}, {}, 0, 0);

    QCOMPARE(ranges.size(), 2);
    QCOMPARE(ranges.at(0).startFrame, int64_t(300));
    QCOMPARE(ranges.at(0).endFrame, int64_t(305));
    QCOMPARE(ranges.at(1).startFrame, int64_t(600));
    QCOMPARE(ranges.at(1).endFrame, int64_t(605));
}

void TestTranscriptLogic::testSpeechFilterRangesHonorPlaybackRateWithoutMarkers() {
    QTemporaryDir dir;
    QVERIFY(dir.isValid());

    const QString clipPath = dir.filePath(QStringLiteral("clip.wav"));
    QVERIFY(QFile(clipPath).open(QIODevice::WriteOnly));

    const QString editablePath = transcriptEditablePathForClipFile(clipPath);
    QJsonArray words;
    words.push_back(wordObj(0.5, 1.0, false));
    QVERIFY(writeTranscriptJson(editablePath, words));
    setActiveTranscriptPathForClipFile(clipPath, editablePath);

    TranscriptEngine engine;
    TimelineClip clip = makeAudioClip(QStringLiteral("clip-rate"), clipPath, 100, 60);
    clip.sourceFps = 60.0;
    clip.sourceDurationFrames = 120;
    clip.playbackRate = 1.5;

    const QVector<ExportRangeSegment> baseRanges{ExportRangeSegment{100, 159}};
    const QVector<ExportRangeSegment> ranges =
        engine.transcriptWordExportRanges(baseRanges, {clip}, {}, 0, 0);

    QCOMPARE(ranges.size(), 1);
    QCOMPARE(ranges.constFirst().startFrame, int64_t(110));
    QCOMPARE(ranges.constFirst().endFrame, int64_t(119));
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

void TestTranscriptLogic::testRuntimeSidecarPathPrefersEditableLegacyArtifact() {
    QTemporaryDir dir;
    QVERIFY(dir.isValid());

    const QString clipPath = dir.filePath(QStringLiteral("clip.wav"));
    QVERIFY(QFile(clipPath).open(QIODevice::WriteOnly));

    const QString originalPath = transcriptPathForClipFile(clipPath);
    const QString editablePath = transcriptEditablePathForClipFile(clipPath);
    QVERIFY(writeTranscriptJson(originalPath, QJsonArray{}));
    QVERIFY(writeTranscriptJson(editablePath, QJsonArray{}));

    const QFileInfo editableInfo(editablePath);
    const QString legacyArtifactPath =
        editableInfo.dir().filePath(editableInfo.completeBaseName() + QStringLiteral("_facestream.bin"));
    QFile legacyArtifact(legacyArtifactPath);
    QVERIFY(legacyArtifact.open(QIODevice::WriteOnly));
    legacyArtifact.write("legacy");
    legacyArtifact.close();

    setActiveTranscriptPathForClipFile(clipPath, originalPath);

    QCOMPARE(transcriptPathForRuntimeSidecarForClipFile(clipPath, originalPath), editablePath);
    QVERIFY(facedetectionsSidecarExistsForClipFile(clipPath));
}

void TestTranscriptLogic::testTranscriptPathsAreIndependentPerAudioStream() {
    QTemporaryDir dir;
    QVERIFY(dir.isValid());

    const QString clipPath = dir.filePath(QStringLiteral("clip.mov"));
    QVERIFY(QFile(clipPath).open(QIODevice::WriteOnly));

    TimelineClip legacyClip = makeAudioClip(QStringLiteral("legacy"), clipPath, 0, 30);
    legacyClip.audioStreamIndex = -1;

    TimelineClip streamOne = legacyClip;
    streamOne.id = QStringLiteral("stream-one");
    streamOne.audioSourceMode = QStringLiteral("embedded");
    streamOne.audioSourceStatus = QStringLiteral("ok");
    streamOne.audioSourcePath = clipPath;
    streamOne.audioStreamIndex = 1;

    TimelineClip streamTwo = streamOne;
    streamTwo.id = QStringLiteral("stream-two");
    streamTwo.audioStreamIndex = 2;

    const TranscriptSourceKey sourceOne = transcriptSourceKeyFromClip(streamOne);
    const TranscriptSourceKey sourceTwo = transcriptSourceKeyFromClip(streamTwo);
    QVERIFY(sourceOne.usesAudioStream());
    QCOMPARE(sourceOne.audioStreamIndex, 1);
    QVERIFY(sourceOne.canonicalKey() != sourceTwo.canonicalKey());
    QCOMPARE(sourceOne.toJson().value(QStringLiteral("audio_stream_index")).toInt(), 1);

    QCOMPARE(transcriptPathForClip(legacyClip), transcriptPathForClipFile(clipPath));
    QVERIFY(transcriptPathForClip(streamOne).endsWith(QStringLiteral("clip_audio_stream_1.json")));
    QVERIFY(transcriptPathForClip(streamTwo).endsWith(QStringLiteral("clip_audio_stream_2.json")));
    QVERIFY(transcriptPathForClip(streamOne) != transcriptPathForClip(streamTwo));
    QVERIFY(transcriptEditablePathForClip(streamOne) != transcriptEditablePathForClip(streamTwo));
    QCOMPARE(transcriptPathForSource(sourceOne), transcriptPathForClip(streamOne));

    const QString streamOneCut = dir.filePath(QStringLiteral("stream_one_cut.json"));
    const QString streamTwoCut = dir.filePath(QStringLiteral("stream_two_cut.json"));
    QVERIFY(writeTranscriptJson(streamOneCut, QJsonArray{}));
    QVERIFY(writeTranscriptJson(streamTwoCut, QJsonArray{}));

    setActiveTranscriptPathForClip(streamOne, streamOneCut);
    setActiveTranscriptPathForClip(streamTwo, streamTwoCut);

    QCOMPARE(activeTranscriptPathForClip(streamOne), streamOneCut);
    QCOMPARE(activeTranscriptPathForClip(streamTwo), streamTwoCut);
}

void TestTranscriptLogic::testTranscriptSourceRootResolvesRelativeClipPaths() {
    QTemporaryDir dir;
    QVERIFY(dir.isValid());

    setTranscriptSourceRootPath(dir.path());

    TimelineClip clip = makeAudioClip(QStringLiteral("relative"), QStringLiteral("clip.wav"), 0, 30);
    clip.audioSourceMode = QStringLiteral("explicit_file");
    clip.audioSourceStatus = QStringLiteral("ok");
    clip.audioSourcePath = QStringLiteral("clip.wav");

    const QString expectedSource = QFileInfo(dir.filePath(QStringLiteral("clip.wav"))).absoluteFilePath();
    QCOMPARE(transcriptSourceKeyFromClip(clip).sourcePath, expectedSource);
    QCOMPARE(transcriptPathForClip(clip), dir.filePath(QStringLiteral("clip.json")));
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

void TestTranscriptLogic::testSpeakerFramingRotatesAroundDetectedFaceBox() {
    TimelineClip clip;
    clip.id = QStringLiteral("clip");
    clip.mediaType = ClipMediaType::Video;
    clip.startFrame = 0;
    clip.durationFrames = 30;
    clip.sourceDurationFrames = 30;
    clip.sourceFrameSize = QSize(1000, 1000);
    clip.speakerFramingTargetKeyframes.push_back(
        TimelineClip::TransformKeyframe{0, QString(), 0.50, 0.35, 0.0, 0.20, 0.20, true});
    normalizeClipTransformKeyframes(clip);

    const TimelineClip::TransformKeyframe unrotated =
        evaluateClipSpeakerFramingForFaceBoxAtPosition(
            clip, 10.0, QPointF(0.60, 0.50), 0.20, 0.0, QSize(1000, 1000));
    const TimelineClip::TransformKeyframe rotated =
        evaluateClipSpeakerFramingForFaceBoxAtPosition(
            clip, 10.0, QPointF(0.60, 0.50), 0.20, 90.0, QSize(1000, 1000));

    QCOMPARE(unrotated.translationX, -100.0);
    QCOMPARE(unrotated.translationY, -150.0);
    QCOMPARE(rotated.rotation, 90.0);
    QVERIFY(std::abs(rotated.translationX) < 0.000001);
    QCOMPARE(rotated.translationY, -250.0);
    QCOMPARE(rotated.scaleX, 1.0);
    QCOMPARE(rotated.scaleY, 1.0);
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

void TestTranscriptLogic::testTranscriptFrameMappingFromPresentedSourceFrame() {
    TimelineClip clip;
    clip.id = QStringLiteral("clip-presented-frame");
    clip.mediaType = ClipMediaType::Video;
    clip.hasAudio = true;
    clip.sourceFps = 60.0;

    QCOMPARE(transcriptFrameForClipSourceFrame(clip, 550441), int64_t(275220));
    QCOMPARE(transcriptFrameForClipSourceFrame(clip, -12), int64_t(0));
}

void TestTranscriptLogic::testAudioVideoCaptionMappingStaysSampleAccurateAtOnePointFiveX() {
    TimelineClip clip;
    clip.id = QStringLiteral("clip-sync-150");
    clip.mediaType = ClipMediaType::Video;
    clip.hasAudio = true;
    clip.startFrame = 100;
    clip.durationFrames = 300;
    clip.sourceInFrame = 120;      // 2.0s at 60fps
    clip.sourceDurationFrames = 900;
    clip.sourceFps = 60.0;
    clip.playbackRate = 1.5;

    const int64_t timelineSample =
        frameToSamples(clip.startFrame + 10) + (kSamplesPerFrame / 2);

    const int64_t audioSourceSample =
        sourceSampleForClipAtTimelineSample(clip, timelineSample, {});
    const int64_t videoSourceFrame =
        sourceFrameForClipAtTimelineSample(clip, timelineSample, {});
    const int64_t captionTranscriptFrame =
        transcriptFrameForClipAtTimelineSample(clip, timelineSample, {});

    const qreal sourceSeconds =
        static_cast<qreal>(audioSourceSample) / static_cast<qreal>(kAudioSampleRate);
    const int64_t sourceFrameFromAudio =
        static_cast<int64_t>(std::floor(sourceSeconds * resolvedSourceFps(clip)));
    const int64_t transcriptFrameFromAudio =
        static_cast<int64_t>(std::floor(sourceSeconds * static_cast<qreal>(kTimelineFps)));

    QCOMPARE(videoSourceFrame, sourceFrameFromAudio);
    QCOMPARE(captionTranscriptFrame, transcriptFrameFromAudio);

    // At 150%, this sub-frame sample lands half-way between source frames 151 and 152.
    // A frame-floored video lookup would incorrectly request frame 150 here.
    QCOMPARE(videoSourceFrame, int64_t(151));
    QCOMPARE(captionTranscriptFrame, int64_t(75));
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

    TimelineClip titledClip;
    titledClip.transcriptOverlay.maxLines = 6;
    titledClip.transcriptOverlay.fontPointSize = 42;
    titledClip.transcriptOverlay.boxHeight = 260.0;
    titledClip.transcriptOverlay.showSpeakerTitle = false;
    const int bodyOnlyLines = transcriptOverlayEffectiveLinesForBox(titledClip);
    titledClip.transcriptOverlay.showSpeakerTitle = true;
    const int titledLines = transcriptOverlayEffectiveLinesForBox(titledClip);
    QVERIFY(titledLines >= 1);
    QVERIFY(titledLines < bodyOnlyLines);

    TimelineClip crowdedClip;
    crowdedClip.transcriptOverlay.maxLines = 3;
    crowdedClip.transcriptOverlay.fontPointSize = 42;
    crowdedClip.transcriptOverlay.boxHeight = 120.0;
    crowdedClip.transcriptOverlay.showShadow = true;
    crowdedClip.transcriptOverlay.showSpeakerTitle = false;
    QCOMPARE(transcriptOverlayEffectiveLinesForBox(crowdedClip), 1);
}

void TestTranscriptLogic::testTranscriptOverlayLayoutHelperMatchesSectionLayout() {
    const TranscriptOverlayTiming noPadding{0, 0};

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
        transcriptOverlayLayoutAtSourceFrame(clip, sections, sourceFrame, noPadding);

    QCOMPARE(actual.lines.size(), expected.lines.size());
    QCOMPARE(actual.truncatedTop, expected.truncatedTop);
    QCOMPARE(actual.truncatedBottom, expected.truncatedBottom);
    QVERIFY(!actual.lines.isEmpty());
    for (int i = 0; i < actual.lines.size(); ++i) {
        QCOMPARE(actual.lines.at(i).words, expected.lines.at(i).words);
        QCOMPARE(actual.lines.at(i).activeWord, expected.lines.at(i).activeWord);
    }
}

void TestTranscriptLogic::testTranscriptOverlayRespectsWordPadding() {
    TimelineClip clip;
    clip.transcriptOverlay.maxLines = 2;
    clip.transcriptOverlay.maxCharsPerLine = 24;

    TranscriptSection first;
    first.startFrame = 0;
    first.endFrame = 9;
    first.text = QStringLiteral("alpha");
    TranscriptWord firstWord;
    firstWord.startFrame = 0;
    firstWord.endFrame = 9;
    firstWord.text = QStringLiteral("alpha");
    firstWord.speaker = QStringLiteral("S1");
    first.words.push_back(firstWord);

    TranscriptSection second;
    second.startFrame = 20;
    second.endFrame = 29;
    second.text = QStringLiteral("beta");
    TranscriptWord secondWord;
    secondWord.startFrame = 20;
    secondWord.endFrame = 29;
    secondWord.text = QStringLiteral("beta");
    secondWord.speaker = QStringLiteral("S1");
    second.words.push_back(secondWord);

    const QVector<TranscriptSection> sections{first, second};

    QVERIFY(transcriptOverlayLayoutAtSourceFrame(clip, sections, 12, TranscriptOverlayTiming{0, 0}).lines.isEmpty());

    const TranscriptOverlayLayout heldLayout =
        transcriptOverlayLayoutAtSourceFrame(clip, sections, 12, TranscriptOverlayTiming{0, 150});
    QVERIFY(!heldLayout.lines.isEmpty());
    QCOMPARE(heldLayout.lines.constFirst().words, QStringList{QStringLiteral("alpha")});
    QCOMPARE(heldLayout.lines.constFirst().activeWord, 0);

    const TranscriptOverlayLayout earlyLayout =
        transcriptOverlayLayoutAtSourceFrame(clip, sections, 16, TranscriptOverlayTiming{150, 0});
    QVERIFY(!earlyLayout.lines.isEmpty());
    QCOMPARE(earlyLayout.lines.constFirst().words, QStringList{QStringLiteral("beta")});
    QCOMPARE(earlyLayout.lines.constFirst().activeWord, 0);

    TranscriptSection joined;
    joined.startFrame = 0;
    joined.endFrame = 29;
    joined.text = QStringLiteral("alpha beta");
    joined.words = {firstWord, secondWord};

    const TranscriptOverlayLayout joinedEarlyLayout =
        transcriptOverlayLayoutAtSourceFrame(
            clip,
            QVector<TranscriptSection>{joined},
            16,
            TranscriptOverlayTiming{150, 0});
    QVERIFY(!joinedEarlyLayout.lines.isEmpty());
    QCOMPARE(joinedEarlyLayout.lines.constFirst().words,
             QStringList({QStringLiteral("alpha"), QStringLiteral("beta")}));
    QCOMPARE(joinedEarlyLayout.lines.constFirst().activeWord, 1);

}

void TestTranscriptLogic::testTranscriptOverlayGuardsWordPaddingEdges() {
    TimelineClip clip;
    clip.transcriptOverlay.maxLines = 2;
    clip.transcriptOverlay.maxCharsPerLine = 24;

    TranscriptSection section;
    section.startFrame = 20;
    section.endFrame = 29;
    section.text = QStringLiteral("beta");
    TranscriptWord word;
    word.startFrame = 20;
    word.endFrame = 29;
    word.text = QStringLiteral("beta");
    word.speaker = QStringLiteral("S2");
    section.words.push_back(word);

    const QVector<TranscriptSection> sections{section};
    const TranscriptOverlayTiming timing{150, 70};

    // 150 ms prepend at 30 fps floors to 4 frames, so the speech-filter word
    // range starts at 16. Frame 15 is the one-frame guard against
    // sample/frame quantization at the edge.
    const TranscriptOverlayLayout guardedStart =
        transcriptOverlayLayoutAtSourceFrame(clip, sections, 15, timing);
    QVERIFY(!guardedStart.lines.isEmpty());
    QCOMPARE(transcriptOverlaySpeakerAtSourceFrame(sections, 15, nullptr, timing), QStringLiteral("S2"));

    ExportRangeSegment activeRange{-1, -1};
    QCOMPARE(transcriptOverlaySpeakerAtSourceFrame(sections, 15, &activeRange, timing),
             QStringLiteral("S2"));
    QCOMPARE(activeRange.startFrame, int64_t(15));
    QCOMPARE(activeRange.endFrame, int64_t(33));

    QVERIFY(transcriptOverlayLayoutAtSourceFrame(clip, sections, 14, timing).lines.isEmpty());
    QVERIFY(transcriptOverlaySpeakerAtSourceFrame(sections, 14, nullptr, timing).isEmpty());
}

void TestTranscriptLogic::testTranscriptOverlaySpeakerLookupReturnsActiveRange() {
    TranscriptSection first;
    first.startFrame = 0;
    first.endFrame = 9;
    first.text = QStringLiteral("alpha");
    TranscriptWord firstWord;
    firstWord.startFrame = 0;
    firstWord.endFrame = 9;
    firstWord.text = QStringLiteral("alpha");
    firstWord.speaker = QStringLiteral("S1");
    first.words.push_back(firstWord);

    TranscriptSection second;
    second.startFrame = 20;
    second.endFrame = 29;
    second.text = QStringLiteral("beta");
    TranscriptWord secondWord;
    secondWord.startFrame = 20;
    secondWord.endFrame = 29;
    secondWord.text = QStringLiteral("beta");
    secondWord.speaker = QStringLiteral("S2");
    second.words.push_back(secondWord);

    const QVector<TranscriptSection> sections{first, second};
    const TranscriptOverlayTiming timing{150, 70};

    ExportRangeSegment activeRange{-1, -1};
    QCOMPARE(transcriptOverlaySpeakerAtSourceFrame(sections, 16, &activeRange, timing),
             QStringLiteral("S2"));
    QCOMPARE(activeRange.startFrame, int64_t(15));
    QCOMPARE(activeRange.endFrame, int64_t(33));

    activeRange = ExportRangeSegment{123, 456};
    QVERIFY(transcriptOverlaySpeakerAtSourceFrame(sections, 14, &activeRange, timing).isEmpty());
    QCOMPARE(activeRange.startFrame, int64_t(-1));
    QCOMPARE(activeRange.endFrame, int64_t(-1));
}

void TestTranscriptLogic::testTranscriptSpeakerTitleUsesOverlayWordPadding() {
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString transcriptPath = dir.filePath(QStringLiteral("clip.json"));

    QJsonObject profile;
    profile[QStringLiteral("name")] = QStringLiteral("Julian Jones");
    profile[QStringLiteral("organization")] = QStringLiteral("Baltimore County Council");
    QJsonObject profiles;
    profiles[QStringLiteral("S2")] = profile;
    QJsonObject root;
    root[QStringLiteral("speaker_profiles")] = profiles;
    QVERIFY(writeTranscriptDocument(transcriptPath, root));

    TranscriptSection section;
    section.startFrame = 20;
    section.endFrame = 29;
    section.text = QStringLiteral("beta");
    TranscriptWord word;
    word.startFrame = 20;
    word.endFrame = 29;
    word.text = QStringLiteral("beta");
    word.speaker = QStringLiteral("S2");
    section.words.push_back(word);

    const QVector<TranscriptSection> sections{section};
    const TranscriptOverlayTiming timing{150, 70};
    QVERIFY(!transcriptOverlayLayoutAtSourceFrame(TimelineClip{}, sections, 16, timing).lines.isEmpty());
    QCOMPARE(transcriptOverlaySpeakerAtSourceFrame(sections, 16, nullptr, timing), QStringLiteral("S2"));
    QCOMPARE(transcriptSpeakerTitleForSourceFrame(transcriptPath, sections, 16, timing),
             QStringLiteral("Julian Jones - Baltimore County Council"));
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

void TestTranscriptLogic::testTranscriptOverlayHtmlCanDisableCurrentWordHighlight() {
    TranscriptOverlayLayout layout;
    TranscriptOverlayLine line;
    line.words = {QStringLiteral("hello"), QStringLiteral("world")};
    line.activeWord = 1;
    layout.lines.push_back(line);

    const QString html = transcriptOverlayHtml(layout,
                                               QColor(QStringLiteral("#ffffffff")),
                                               QColor(QStringLiteral("#ff181818")),
                                               QColor(QStringLiteral("#fffff2a8")),
                                               false);

    QVERIFY(!html.contains(QStringLiteral("background:")));
    QVERIFY(!html.contains(QStringLiteral("#181818")));
    QVERIFY(!html.contains(QStringLiteral("#fff2a8")));
    QVERIFY(html.contains(QStringLiteral("world")));
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
    clip.transcriptOverlay.translationX = 0.25;
    clip.transcriptOverlay.translationY = -0.5;

    const QSize outputSize(1080, 1920);
    const QRectF rect = transcriptOverlayRectInOutputSpace(
        clip, outputSize, QString(), QVector<TranscriptSection>{}, /*sourceFrame=*/0);

    QVERIFY(std::abs(rect.center().x() - (540.0 + (0.25 * 540.0))) < 0.001);
    QVERIFY(std::abs(rect.center().y() - (960.0 - (0.5 * 960.0))) < 0.001);
    QVERIFY(std::abs(rect.width() - 500.0) < 0.001);
    QVERIFY(std::abs(rect.height() - 250.0) < 0.001);
}

void TestTranscriptLogic::testTranscriptOverlayManualTranslationUsesNormalizedOffsets() {
    TimelineClip clip;
    clip.transcriptOverlay.boxWidth = 500.0;
    clip.transcriptOverlay.boxHeight = 250.0;
    clip.transcriptOverlay.translationX = 0.5;
    clip.transcriptOverlay.translationY = -0.25;
    clip.transcriptOverlay.useManualPlacement = true;

    const QSize outputSize(1080, 1920);
    const QRectF rect = transcriptOverlayRectInOutputSpace(
        clip, outputSize, QString(), QVector<TranscriptSection>{}, /*sourceFrame=*/0);

    QVERIFY(std::abs(rect.center().x() - (540.0 + (0.5 * 540.0))) < 0.001);
    QVERIFY(std::abs(rect.center().y() - (960.0 - (0.25 * 960.0))) < 0.001);
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
    clip.transcriptOverlay.translationX = -0.25;
    clip.transcriptOverlay.translationY = 0.1;
    clip.transcriptOverlay.useManualPlacement = true;
    const QSize outputSize(1080, 1920);

    const QRectF rect = transcriptOverlayRectInOutputSpace(
        clip, outputSize, transcriptPath, sections, /*sourceFrame=*/15);

    QVERIFY(std::abs(rect.center().x() - (540.0 - (0.25 * 540.0))) < 0.001);
    QVERIFY(std::abs(rect.center().y() - (960.0 + (0.1 * 960.0))) < 0.001);
}

void TestTranscriptLogic::testTranscriptOverlayStyleCacheMaterialIncludesTransformFields() {
    TimelineClip clip;
    clip.transcriptOverlay.useManualPlacement = true;
    clip.transcriptOverlay.autoScroll = true;
    clip.transcriptOverlay.maxLines = 2;
    clip.transcriptOverlay.maxCharsPerLine = 28;
    clip.transcriptOverlay.translationX = 0.0;
    clip.transcriptOverlay.translationY = 0.0;
    clip.transcriptOverlay.boxWidth = 900.0;
    clip.transcriptOverlay.boxHeight = 220.0;

    const QString baseMaterial = transcriptOverlayStyleCacheMaterial(clip);

    TimelineClip moved = clip;
    moved.transcriptOverlay.translationX = 0.25;
    QVERIFY2(transcriptOverlayStyleCacheMaterial(moved) != baseMaterial,
             "Transcript overlay cache material must change when X translation changes.");

    TimelineClip resized = clip;
    resized.transcriptOverlay.boxWidth = 1100.0;
    QVERIFY2(transcriptOverlayStyleCacheMaterial(resized) != baseMaterial,
             "Transcript overlay cache material must change when box width changes.");

    TimelineClip relaidOut = clip;
    relaidOut.transcriptOverlay.maxCharsPerLine = 42;
    QVERIFY2(transcriptOverlayStyleCacheMaterial(relaidOut) != baseMaterial,
             "Transcript overlay cache material must change when line layout limits change.");

    TimelineClip highlightToggled = clip;
    highlightToggled.transcriptOverlay.highlightCurrentWord = !clip.transcriptOverlay.highlightCurrentWord;
    QVERIFY2(transcriptOverlayStyleCacheMaterial(highlightToggled) != baseMaterial,
             "Transcript overlay cache material must change when current-word highlighting changes.");
}

void TestTranscriptLogic::testTranscriptOverlayProjectLoadNormalizesUnreadableGeometry() {
    TimelineClip source = makeAudioClip(QStringLiteral("clip-1"),
                                        QStringLiteral("/tmp/clip.wav"),
                                        0,
                                        90);
    source.transcriptOverlay.enabled = true;
    source.transcriptOverlay.useManualPlacement = true;
    source.transcriptOverlay.boxWidth = 1.0;
    source.transcriptOverlay.boxHeight = 1.0;
    source.transcriptOverlay.maxCharsPerLine = 1;
    source.transcriptOverlay.fontPointSize = 8;
    source.transcriptOverlay.highlightCurrentWord = false;

    const QJsonObject json = clipToJson(source);
    QCOMPARE(json.value(QStringLiteral("transcriptOverlay"))
                 .toObject()
                 .value(QStringLiteral("highlightCurrentWord"))
                 .toBool(true),
             false);
    TimelineClip loaded = clipFromJson(json);

    QCOMPARE(loaded.transcriptOverlay.boxWidth,
             TimelineClip::TranscriptOverlaySettings::kMinReadableBoxWidth);
    QCOMPARE(loaded.transcriptOverlay.boxHeight,
             TimelineClip::TranscriptOverlaySettings::kMinReadableBoxHeight);
    QCOMPARE(loaded.transcriptOverlay.maxCharsPerLine,
             TimelineClip::TranscriptOverlaySettings::kMinReadableCharsPerLine);
    QCOMPARE(loaded.transcriptOverlay.fontPointSize,
             TimelineClip::TranscriptOverlaySettings::kMinReadableFontPointSize);
    QCOMPARE(loaded.transcriptOverlay.highlightCurrentWord, false);
}

void TestTranscriptLogic::testSpeakerFramingEnabledKeyframesOverrideGlobalFallback() {
    TimelineClip clip;
    clip.id = QStringLiteral("clip");
    clip.mediaType = ClipMediaType::Video;
    clip.startFrame = 100;
    clip.durationFrames = 90;
    clip.sourceDurationFrames = 90;
    clip.speakerFramingEnabled = false;

    QVERIFY(!evaluateClipSpeakerFramingEnabledAtFrame(clip, 100));
    QVERIFY(!evaluateClipSpeakerFramingEnabledAtFrame(clip, 130));

    clip.speakerFramingEnabledKeyframes.push_back(TimelineClip::BoolKeyframe{0, false});
    clip.speakerFramingEnabledKeyframes.push_back(TimelineClip::BoolKeyframe{15, true});
    clip.speakerFramingEnabledKeyframes.push_back(TimelineClip::BoolKeyframe{45, false});
    normalizeClipTransformKeyframes(clip);

    QVERIFY(!evaluateClipSpeakerFramingEnabledAtFrame(clip, 100));
    QVERIFY(!evaluateClipSpeakerFramingEnabledAtFrame(clip, 114));
    QVERIFY(evaluateClipSpeakerFramingEnabledAtFrame(clip, 115));
    QVERIFY(evaluateClipSpeakerFramingEnabledAtFrame(clip, 144));
    QVERIFY(!evaluateClipSpeakerFramingEnabledAtFrame(clip, 145));
}

void TestTranscriptLogic::testSpeakerFramingRuntimeSpeakerDescendsFromTranscript() {
    QTemporaryDir dir;
    QVERIFY(dir.isValid());

    const QString clipPath = dir.filePath(QStringLiteral("clip.mp4"));
    QVERIFY(QFile(clipPath).open(QIODevice::WriteOnly));
    const QString transcriptPath = dir.filePath(QStringLiteral("clip_editable.json"));

    QJsonObject word;
    word[QStringLiteral("word")] = QStringLiteral("hello");
    word[QStringLiteral("start")] = 0.0;
    word[QStringLiteral("end")] = 1.0;
    word[QStringLiteral("speaker")] = QStringLiteral("S1");
    QJsonObject segment;
    segment[QStringLiteral("words")] = QJsonArray{word};

    QJsonObject trackingKeyframe;
    trackingKeyframe[QStringLiteral("frame")] = 0;
    trackingKeyframe[QStringLiteral("x")] = 0.25;
    trackingKeyframe[QStringLiteral("y")] = 0.50;
    trackingKeyframe[QStringLiteral("box_size")] = 0.20;
    trackingKeyframe[QStringLiteral("confidence")] = 1.0;
    QJsonObject framing;
    framing[QStringLiteral("enabled")] = true;
    framing[QStringLiteral("mode")] = QStringLiteral("AutoTrackLinear");
    framing[QStringLiteral("keyframes")] = QJsonArray{trackingKeyframe};
    QJsonObject profile;
    profile[QStringLiteral("framing")] = framing;
    QJsonObject profiles;
    profiles[QStringLiteral("S1")] = profile;

    QJsonObject root;
    root[QStringLiteral("segments")] = QJsonArray{segment};
    root[QStringLiteral("speaker_profiles")] = profiles;
    QVERIFY(writeTranscriptDocument(transcriptPath, root));

    setActiveTranscriptPathForClipFile(clipPath, transcriptPath);
    invalidateTranscriptJsonCache(transcriptPath);
    invalidateTranscriptSpeakerProfileCache(transcriptPath);

    TimelineClip clip;
    clip.id = QStringLiteral("clip");
    clip.filePath = clipPath;
    clip.mediaType = ClipMediaType::Video;
    clip.startFrame = 0;
    clip.durationFrames = 30;
    clip.sourceDurationFrames = 30;
    clip.sourceFrameSize = QSize(1000, 1000);
    clip.speakerFramingEnabled = true;
    clip.speakerFramingMinConfidence = 0.08;
    clip.speakerFramingTargetKeyframes.push_back(
        TimelineClip::TransformKeyframe{0, QString(), 0.50, 0.35, 0.0, 0.20, 0.20, true});
    normalizeClipTransformKeyframes(clip);
    prepareClipSpeakerFramingContinuityRuntimeBlocking(clip);

    QCOMPARE(transcriptActiveSpeakerForClipFileAtSourceFrame(clipPath, 15), QStringLiteral("S1"));
    const TimelineClip::TransformKeyframe transform =
        evaluateClipSpeakerFramingAtFrame(clip, 15, QSize(1000, 1000));
    QVERIFY(std::abs(transform.translationX) > 0.001);
    QVERIFY(std::abs(transform.scaleX - 1.0) < 0.001);
}

void TestTranscriptLogic::testSpeakerFramingGapHoldPersistsTranscriptSpeaker() {
    QTemporaryDir dir;
    QVERIFY(dir.isValid());

    const QString clipPath = dir.filePath(QStringLiteral("clip.mp4"));
    QVERIFY(QFile(clipPath).open(QIODevice::WriteOnly));
    const QString transcriptPath = dir.filePath(QStringLiteral("clip_editable.json"));

    QJsonObject word;
    word[QStringLiteral("word")] = QStringLiteral("hello");
    word[QStringLiteral("start")] = 0.0;
    word[QStringLiteral("end")] = 1.0;
    word[QStringLiteral("speaker")] = QStringLiteral("S1");
    QJsonObject segment;
    segment[QStringLiteral("words")] = QJsonArray{word};

    QJsonObject trackingKeyframe;
    trackingKeyframe[QStringLiteral("frame")] = 0;
    trackingKeyframe[QStringLiteral("x")] = 0.25;
    trackingKeyframe[QStringLiteral("y")] = 0.50;
    trackingKeyframe[QStringLiteral("box_size")] = 0.20;
    trackingKeyframe[QStringLiteral("confidence")] = 1.0;
    QJsonObject framing;
    framing[QStringLiteral("enabled")] = true;
    framing[QStringLiteral("mode")] = QStringLiteral("AutoTrackLinear");
    framing[QStringLiteral("keyframes")] = QJsonArray{trackingKeyframe};
    QJsonObject profile;
    profile[QStringLiteral("framing")] = framing;
    QJsonObject profiles;
    profiles[QStringLiteral("S1")] = profile;

    QJsonObject root;
    root[QStringLiteral("segments")] = QJsonArray{segment};
    root[QStringLiteral("speaker_profiles")] = profiles;
    QVERIFY(writeTranscriptDocument(transcriptPath, root));

    setActiveTranscriptPathForClipFile(clipPath, transcriptPath);
    invalidateTranscriptJsonCache(transcriptPath);
    invalidateTranscriptSpeakerProfileCache(transcriptPath);

    TimelineClip clip;
    clip.id = QStringLiteral("clip");
    clip.filePath = clipPath;
    clip.mediaType = ClipMediaType::Video;
    clip.startFrame = 0;
    clip.durationFrames = 90;
    clip.sourceDurationFrames = 90;
    clip.sourceFrameSize = QSize(1000, 1000);
    clip.speakerFramingEnabled = true;
    clip.speakerFramingMinConfidence = 0.08;
    clip.speakerFramingTargetKeyframes.push_back(
        TimelineClip::TransformKeyframe{0, QString(), 0.50, 0.35, 0.0, 0.20, 0.20, true});
    normalizeClipTransformKeyframes(clip);
    prepareClipSpeakerFramingContinuityRuntimeBlocking(clip);

    QCOMPARE(transcriptActiveSpeakerForClipFileAtSourceFrame(clipPath, 45), QString());
    TimelineClip::TransformKeyframe transform =
        evaluateClipSpeakerFramingAtFrame(clip, 45, QSize(1000, 1000));
    QVERIFY(std::abs(transform.translationX) < 0.001);

    clip.speakerFramingGapHoldFrames = 20;
    normalizeClipTransformKeyframes(clip);
    prepareClipSpeakerFramingContinuityRuntimeBlocking(clip);
    transform = evaluateClipSpeakerFramingAtFrame(clip, 45, QSize(1000, 1000));
    QVERIFY(std::abs(transform.translationX) > 0.001);
}

void TestTranscriptLogic::testSpeakerFramingSmoothingAppliesToAssignedContinuityTracks() {
    QTemporaryDir dir;
    QVERIFY(dir.isValid());

    const QString clipPath = dir.filePath(QStringLiteral("clip.mp4"));
    QVERIFY(QFile(clipPath).open(QIODevice::WriteOnly));
    const QString transcriptPath = dir.filePath(QStringLiteral("clip_editable.json"));

    QJsonObject word;
    word[QStringLiteral("word")] = QStringLiteral("hello");
    word[QStringLiteral("start")] = 0.0;
    word[QStringLiteral("end")] = 1.0;
    word[QStringLiteral("speaker")] = QStringLiteral("S1");
    QJsonObject segment;
    segment[QStringLiteral("words")] = QJsonArray{word};
    QJsonObject root;
    root[QStringLiteral("segments")] = QJsonArray{segment};

    TimelineClip clip;
    clip.id = QStringLiteral("clip");
    clip.filePath = clipPath;
    clip.mediaType = ClipMediaType::Video;
    clip.startFrame = 0;
    clip.durationFrames = 30;
    clip.sourceDurationFrames = 30;
    clip.sourceFrameSize = QSize(1000, 1000);
    clip.speakerFramingEnabled = true;
    clip.speakerFramingTargetKeyframes.push_back(
        TimelineClip::TransformKeyframe{0, QString(), 0.50, 0.35, 0.0, 0.20, 0.20, true});
    normalizeClipTransformKeyframes(clip);

    QJsonObject identityRow;
    identityRow[QStringLiteral("identity_id")] = QStringLiteral("S1");
    identityRow[QStringLiteral("track_id")] = 1;
    identityRow[QStringLiteral("stream_id")] = QStringLiteral("T1");
    QJsonObject resolvedCurrent;
    resolvedCurrent[QStringLiteral("track_identity_map")] = QJsonArray{identityRow};
    QJsonObject clipFlow;
    clipFlow[QStringLiteral("resolved_current")] = resolvedCurrent;
    QJsonObject flowClips;
    flowClips[clip.id] = clipFlow;
    QJsonObject speakerFlow;
    speakerFlow[QStringLiteral("clips")] = flowClips;
    root[QStringLiteral("speaker_flow")] = speakerFlow;
    QVERIFY(writeTranscriptDocument(transcriptPath, root));

    setActiveTranscriptPathForClipFile(clipPath, transcriptPath);
    invalidateTranscriptJsonCache(transcriptPath);
    invalidateTranscriptSpeakerProfileCache(transcriptPath);

    TranscriptEngine engine;

    auto keyframe = [](int frame, double x, double box = 0.20) {
        QJsonObject obj;
        obj[QStringLiteral("frame")] = frame;
        obj[QStringLiteral("x")] = x;
        obj[QStringLiteral("y")] = 0.50;
        obj[QStringLiteral("box")] = box;
        obj[QStringLiteral("box_size")] = box;
        obj[QStringLiteral("score")] = 1.0;
        obj[QStringLiteral("confidence")] = 1.0;
        return obj;
    };
    QJsonObject stream;
    stream[QStringLiteral("track_id")] = 1;
    stream[QStringLiteral("stream_id")] = QStringLiteral("T1");
    stream[QStringLiteral("frame_domain")] = QStringLiteral("source_absolute");
    const QJsonArray detections{
        keyframe(0, 0.20, 0.20),
        keyframe(10, 0.80, 0.50),
        keyframe(20, 0.20, 0.20),
    };
    stream[QStringLiteral("keyframes")] = detections;
    stream[QStringLiteral("detections")] = detections;
    QJsonObject continuityRoot;
    continuityRoot[QStringLiteral("raw_tracks")] = QJsonArray{stream};
    continuityRoot[QStringLiteral("raw_tracks_frame_domain")] = QStringLiteral("source_absolute");
    QJsonObject byClip;
    byClip[clip.id] = continuityRoot;
    QJsonObject processedRoot;
    processedRoot[QStringLiteral("continuity_facedetections_by_clip")] = byClip;
    QVERIFY(engine.saveFacestreamProcessedArtifact(transcriptPath, processedRoot));
    prepareClipSpeakerFramingContinuityRuntimeBlocking(clip);

    TimelineClip unsmoothed = clip;
    unsmoothed.speakerFramingCenterSmoothingFrames = 0;
    normalizeClipTransformKeyframes(unsmoothed);
    const TimelineClip::TransformKeyframe unsmoothedTransform =
        evaluateClipSpeakerFramingAtFrame(unsmoothed, 10, QSize(1000, 1000));

    TimelineClip smoothed = clip;
    smoothed.speakerFramingCenterSmoothingFrames = 21;
    smoothed.speakerFramingSmoothingMode = 0;
    normalizeClipTransformKeyframes(smoothed);
    const TimelineClip::TransformKeyframe smoothedTransform =
        evaluateClipSpeakerFramingAtFrame(smoothed, 10, QSize(1000, 1000));

    QVERIFY(std::abs(unsmoothedTransform.translationX - smoothedTransform.translationX) > 100.0);
    QVERIFY(smoothedTransform.translationX > unsmoothedTransform.translationX);

    TimelineClip rawBlend = smoothed;
    rawBlend.speakerFramingCenterSmoothingStrength = 0.0;
    normalizeClipTransformKeyframes(rawBlend);
    const TimelineClip::TransformKeyframe rawBlendTransform =
        evaluateClipSpeakerFramingAtFrame(rawBlend, 10, QSize(1000, 1000));
    QVERIFY(std::abs(rawBlendTransform.translationX - unsmoothedTransform.translationX) < 0.001);

    TimelineClip halfBlend = smoothed;
    halfBlend.speakerFramingCenterSmoothingStrength = 0.5;
    normalizeClipTransformKeyframes(halfBlend);
    const TimelineClip::TransformKeyframe halfBlendTransform =
        evaluateClipSpeakerFramingAtFrame(halfBlend, 10, QSize(1000, 1000));
    QVERIFY(halfBlendTransform.translationX > unsmoothedTransform.translationX);
    QVERIFY(halfBlendTransform.translationX < smoothedTransform.translationX);

    TimelineClip amplifiedBlend = smoothed;
    amplifiedBlend.speakerFramingCenterSmoothingStrength = 3.0;
    normalizeClipTransformKeyframes(amplifiedBlend);
    QCOMPARE(amplifiedBlend.speakerFramingCenterSmoothingStrength, 3.0);
    const TimelineClip::TransformKeyframe amplifiedBlendTransform =
        evaluateClipSpeakerFramingAtFrame(amplifiedBlend, 10, QSize(1000, 1000));
    QVERIFY(amplifiedBlendTransform.translationX > smoothedTransform.translationX);

    TimelineClip zoomOnly = clip;
    zoomOnly.speakerFramingCenterSmoothingFrames = 0;
    zoomOnly.speakerFramingZoomSmoothingFrames = 21;
    zoomOnly.speakerFramingZoomSmoothingStrength = 1.0;
    normalizeClipTransformKeyframes(zoomOnly);
    const TimelineClip::TransformKeyframe zoomOnlyTransform =
        evaluateClipSpeakerFramingAtFrame(zoomOnly, 10, QSize(1000, 1000));
    QVERIFY(zoomOnlyTransform.scaleX > unsmoothedTransform.scaleX);

    TimelineClip rawZoom = zoomOnly;
    rawZoom.speakerFramingZoomSmoothingStrength = 0.0;
    normalizeClipTransformKeyframes(rawZoom);
    const TimelineClip::TransformKeyframe rawZoomTransform =
        evaluateClipSpeakerFramingAtFrame(rawZoom, 10, QSize(1000, 1000));
    QVERIFY(std::abs(rawZoomTransform.scaleX - unsmoothedTransform.scaleX) < 0.001);
}

void TestTranscriptLogic::testDynamicSpeakerFramingInterpolatesFractionalPlaybackPosition() {
    QTemporaryDir dir;
    QVERIFY(dir.isValid());

    const QString clipPath = dir.filePath(QStringLiteral("clip.mp4"));
    QVERIFY(QFile(clipPath).open(QIODevice::WriteOnly));
    const QString transcriptPath = dir.filePath(QStringLiteral("clip_editable.json"));

    QJsonObject word;
    word[QStringLiteral("word")] = QStringLiteral("hello");
    word[QStringLiteral("start")] = 0.0;
    word[QStringLiteral("end")] = 1.0;
    word[QStringLiteral("speaker")] = QStringLiteral("S1");
    QJsonObject segment;
    segment[QStringLiteral("words")] = QJsonArray{word};

    QJsonObject identityRow;
    identityRow[QStringLiteral("identity_id")] = QStringLiteral("S1");
    identityRow[QStringLiteral("track_id")] = 1;
    identityRow[QStringLiteral("stream_id")] = QStringLiteral("T1");
    QJsonObject resolvedCurrent;
    resolvedCurrent[QStringLiteral("track_identity_map")] = QJsonArray{identityRow};
    QJsonObject clipFlow;
    clipFlow[QStringLiteral("resolved_current")] = resolvedCurrent;
    QJsonObject flowClips;
    flowClips[QStringLiteral("clip")] = clipFlow;
    QJsonObject speakerFlow;
    speakerFlow[QStringLiteral("clips")] = flowClips;

    QJsonObject root;
    root[QStringLiteral("segments")] = QJsonArray{segment};
    root[QStringLiteral("speaker_flow")] = speakerFlow;
    QVERIFY(writeTranscriptDocument(transcriptPath, root));

    setActiveTranscriptPathForClipFile(clipPath, transcriptPath);
    invalidateTranscriptJsonCache(transcriptPath);
    invalidateTranscriptSpeakerProfileCache(transcriptPath);

    auto keyframe = [](int frame, double x) {
        QJsonObject obj;
        obj[QStringLiteral("frame")] = frame;
        obj[QStringLiteral("x")] = x;
        obj[QStringLiteral("y")] = 0.50;
        obj[QStringLiteral("box")] = 0.20;
        obj[QStringLiteral("box_size")] = 0.20;
        obj[QStringLiteral("confidence")] = 1.0;
        return obj;
    };
    QJsonObject stream;
    stream[QStringLiteral("track_id")] = 1;
    stream[QStringLiteral("stream_id")] = QStringLiteral("T1");
    stream[QStringLiteral("frame_domain")] = QStringLiteral("source_absolute");
    const QJsonArray detections{
        keyframe(10, 0.20),
        keyframe(11, 0.80),
    };
    stream[QStringLiteral("keyframes")] = detections;
    stream[QStringLiteral("detections")] = detections;
    QJsonObject continuityRoot;
    continuityRoot[QStringLiteral("raw_tracks")] = QJsonArray{stream};
    continuityRoot[QStringLiteral("raw_tracks_frame_domain")] = QStringLiteral("source_absolute");
    QJsonObject byClip;
    byClip[QStringLiteral("clip")] = continuityRoot;
    QJsonObject processedRoot;
    processedRoot[QStringLiteral("continuity_facedetections_by_clip")] = byClip;

    TranscriptEngine engine;
    QVERIFY(engine.saveFacestreamProcessedArtifact(transcriptPath, processedRoot));

    TimelineClip clip;
    clip.id = QStringLiteral("clip");
    clip.filePath = clipPath;
    clip.mediaType = ClipMediaType::Video;
    clip.startFrame = 0;
    clip.durationFrames = 30;
    clip.sourceDurationFrames = 30;
    clip.sourceFrameSize = QSize(1000, 1000);
    clip.speakerFramingEnabled = true;
    clip.speakerFramingCenterSmoothingFrames = 0;
    clip.speakerFramingZoomSmoothingFrames = 0;
    clip.speakerFramingTargetKeyframes.push_back(
        TimelineClip::TransformKeyframe{0, QString(), 0.50, 0.35, 0.0, 0.20, 0.20, true});
    normalizeClipTransformKeyframes(clip);

    const TimelineClip::TransformKeyframe lower =
        evaluateClipSpeakerFramingAtFrame(clip, 10, QSize(1000, 1000));
    const TimelineClip::TransformKeyframe upper =
        evaluateClipSpeakerFramingAtFrame(clip, 11, QSize(1000, 1000));
    const TimelineClip::TransformKeyframe fractional =
        evaluateClipSpeakerFramingAtPosition(clip, 10.5, QSize(1000, 1000));

    QCOMPARE(fractional.translationX, (lower.translationX + upper.translationX) * 0.5);
    QCOMPARE(fractional.translationY, (lower.translationY + upper.translationY) * 0.5);
    QCOMPARE(fractional.scaleX, (lower.scaleX + upper.scaleX) * 0.5);
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
