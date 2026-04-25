#include <QtTest/QtTest>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTemporaryDir>
#include <cmath>

#include "../editor_shared.h"
#include "../transform_skip_aware_timing.h"

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

QJsonObject wordObj(double startSeconds, double endSeconds, bool skipped = false) {
    QJsonObject word;
    word[QStringLiteral("word")] = QStringLiteral("w");
    word[QStringLiteral("start")] = startSeconds;
    word[QStringLiteral("end")] = endSeconds;
    word[QStringLiteral("skipped")] = skipped;
    return word;
}

TimelineClip makeClip(const QString& id, const QString& path) {
    TimelineClip clip;
    clip.id = id;
    clip.filePath = path;
    clip.mediaType = ClipMediaType::Audio;
    clip.hasAudio = true;
    clip.startFrame = 100;
    clip.durationFrames = 60;
    clip.sourceInFrame = 0;
    clip.sourceDurationFrames = 120;
    clip.transformSkipAwareTiming = true;
    return clip;
}

} // namespace

class TestTransformSkipAwareTiming : public QObject {
    Q_OBJECT

private slots:
    void init() {
        clearAllActiveTranscriptPaths();
        setTransformSkipAwareTimelineRanges({});
    }

    void cleanup() {
        clearAllActiveTranscriptPaths();
        setTransformSkipAwareTimelineRanges({});
    }

    void testUsesGlobalTimelineRangesWhenProvided();
    void testFallsBackToTranscriptRangesWhenGlobalRangesEmpty();
    void testDisabledSkipAwareReturnsBaseInterpolation();
};

void TestTransformSkipAwareTiming::testUsesGlobalTimelineRangesWhenProvided() {
    TimelineClip clip = makeClip(QStringLiteral("clip-a"), QStringLiteral("/tmp/nonexistent-a.wav"));

    QVector<ExportRangeSegment> ranges;
    ranges.push_back(ExportRangeSegment{100, 109});
    ranges.push_back(ExportRangeSegment{120, 129});
    setTransformSkipAwareTimelineRanges(ranges);

    // local [0,30] maps to timeline [100,130]
    // effective spoken duration over span = 20 frames.
    // local 15 (timeline 115, in gap) has accumulated effective 10 frames.
    const qreal tMidGap = interpolationFactorForTransformFrames(clip, 0.0, 30.0, 15.0);
    QVERIFY(std::abs(tMidGap - 0.5) < 0.0001);

    // local 25 (timeline 125) has accumulated effective 15/20 => 0.75
    const qreal tLate = interpolationFactorForTransformFrames(clip, 0.0, 30.0, 25.0);
    QVERIFY(std::abs(tLate - 0.75) < 0.0001);
}

void TestTransformSkipAwareTiming::testFallsBackToTranscriptRangesWhenGlobalRangesEmpty() {
    QTemporaryDir dir;
    QVERIFY(dir.isValid());

    const QString clipPath = dir.filePath(QStringLiteral("clip.wav"));
    QVERIFY(QFile(clipPath).open(QIODevice::WriteOnly));

    // Ensure no global active ranges so fallback uses transcript speech ranges.
    setTransformSkipAwareTimelineRanges({});

    const QString editablePath = transcriptEditablePathForClipFile(clipPath);
    QJsonArray words;
    // 0-10 frames and 20-30 frames at 30fps
    words.push_back(wordObj(0.0, 10.0 / 30.0, false));
    words.push_back(wordObj(20.0 / 30.0, 30.0 / 30.0, false));
    QVERIFY(writeTranscriptJson(editablePath, words));

    TimelineClip clip = makeClip(QStringLiteral("clip-b"), clipPath);
    clip.startFrame = 0;
    clip.sourceInFrame = 0;
    clip.durationFrames = 40;
    clip.sourceDurationFrames = 40;

    // local [0,30] => effective span = 20 frames
    // local 15 has effective 10 -> 0.5
    const qreal tMidGap = interpolationFactorForTransformFrames(clip, 0.0, 30.0, 15.0);
    QVERIFY(std::abs(tMidGap - 0.5) < 0.001);

    // local 25 has effective 15 -> 0.75
    const qreal tLate = interpolationFactorForTransformFrames(clip, 0.0, 30.0, 25.0);
    QVERIFY(std::abs(tLate - 0.75) < 0.001);
}

void TestTransformSkipAwareTiming::testDisabledSkipAwareReturnsBaseInterpolation() {
    TimelineClip clip = makeClip(QStringLiteral("clip-c"), QStringLiteral("/tmp/nonexistent-c.wav"));
    clip.transformSkipAwareTiming = false;

    setTransformSkipAwareTimelineRanges({ExportRangeSegment{100, 109}});
    const qreal t = interpolationFactorForTransformFrames(clip, 0.0, 40.0, 10.0);
    QVERIFY(std::abs(t - 0.25) < 0.0001);
}

QTEST_MAIN(TestTransformSkipAwareTiming)
#include "test_transform_skip_aware_timing.moc"
