#include <QtTest/QtTest>

#include <QFile>
#include <QGuiApplication>
#include <QHash>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QDir>
#include <QTemporaryDir>

#include "../editor_shared.h"
#include "../render_internal.h"

namespace {

bool writeTranscript(const QString& path)
{
    QJsonArray words;
    QJsonObject w1;
    w1[QStringLiteral("word")] = QStringLiteral("VISIBLE");
    w1[QStringLiteral("start")] = 0.0;
    w1[QStringLiteral("end")] = 0.5;
    w1[QStringLiteral("speaker")] = QStringLiteral("S1");
    words.push_back(w1);

    QJsonObject w2;
    w2[QStringLiteral("word")] = QStringLiteral("TEXT");
    w2[QStringLiteral("start")] = 0.5;
    w2[QStringLiteral("end")] = 1.0;
    w2[QStringLiteral("speaker")] = QStringLiteral("S1");
    words.push_back(w2);

    QJsonObject segment;
    segment[QStringLiteral("speaker")] = QStringLiteral("S1");
    segment[QStringLiteral("start")] = 0.0;
    segment[QStringLiteral("end")] = 1.0;
    segment[QStringLiteral("text")] = QStringLiteral("VISIBLE TEXT");
    segment[QStringLiteral("words")] = words;

    QJsonObject root;
    root[QStringLiteral("segments")] = QJsonArray{segment};

    QFile file(path);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        return false;
    }
    const QByteArray payload = QJsonDocument(root).toJson(QJsonDocument::Compact);
    return file.write(payload) == payload.size();
}

TimelineClip makeTranscriptClip(const QString& clipPath, const QSize& outputSize)
{
    TimelineClip clip;
    clip.id = QStringLiteral("vulkan-subtitle-clip");
    clip.label = QStringLiteral("Vulkan subtitle clip");
    clip.filePath = clipPath;
    clip.mediaType = ClipMediaType::Title;
    clip.hasAudio = true;
    clip.videoEnabled = true;
    clip.audioEnabled = true;
    clip.startFrame = 0;
    clip.durationFrames = 60;
    clip.sourceInFrame = 0;
    clip.sourceDurationFrames = 60;
    clip.sourceFps = 30.0;
    clip.trackIndex = 0;
    clip.transcriptOverlay.enabled = true;
    clip.transcriptOverlay.showBackground = true;
    clip.transcriptOverlay.backgroundOpacity = 0.62;
    clip.transcriptOverlay.backgroundCornerRadius = 22.0;
    clip.transcriptOverlay.showSpeakerTitle = false;
    clip.transcriptOverlay.autoScroll = true;
    clip.transcriptOverlay.useManualPlacement = true;
    clip.transcriptOverlay.translationX = 0.0;
    clip.transcriptOverlay.translationY = 0.0;
    const qreal base = qMax<qreal>(1.0, qMin(outputSize.width(), outputSize.height()));
    clip.transcriptOverlay.boxWidth = qBound<qreal>(280.0, outputSize.width() * 0.72, outputSize.width() - 32.0);
    clip.transcriptOverlay.boxHeight = qBound<qreal>(110.0, base * 0.25, outputSize.height() * 0.42);
    clip.transcriptOverlay.maxLines = 2;
    clip.transcriptOverlay.maxCharsPerLine = 32;
    clip.transcriptOverlay.fontFamily = QStringLiteral("DejaVu Sans");
    clip.transcriptOverlay.fontPointSize = qBound<qreal>(28.0, base * 0.078, 64.0);
    clip.transcriptOverlay.bold = true;
    clip.transcriptOverlay.italic = false;
    clip.transcriptOverlay.textColor = QColor(QStringLiteral("#ffffff"));
    return clip;
}

struct PixelCounts {
    int opaqueDark = 0;
    int brightText = 0;
    int yellowHighlight = 0;
    int nonBlack = 0;
};

PixelCounts countSubtitlePixels(const QImage& frame)
{
    PixelCounts counts;
    const QRect roi(frame.width() * 0.08,
                    frame.height() * 0.22,
                    frame.width() * 0.84,
                    frame.height() * 0.56);
    for (int y = qMax(0, roi.top()); y < qMin(frame.height(), roi.bottom()); ++y) {
        for (int x = qMax(0, roi.left()); x < qMin(frame.width(), roi.right()); ++x) {
            const QColor c = frame.pixelColor(x, y);
            if (c.alpha() > 180 && c.red() < 60 && c.green() < 60 && c.blue() < 60) {
                ++counts.opaqueDark;
            }
            if (c.red() > 210 && c.green() > 210 && c.blue() > 210) {
                ++counts.brightText;
            }
            if (c.red() > 200 && c.green() > 180 && c.blue() > 80 && c.blue() < 210) {
                ++counts.yellowHighlight;
            }
            if (c.red() > 8 || c.green() > 8 || c.blue() > 8) {
                ++counts.nonBlack;
            }
        }
    }
    return counts;
}

} // namespace

class TestVulkanSubtitleRender : public QObject {
    Q_OBJECT

private slots:
    void init() { clearAllActiveTranscriptPaths(); }
    void cleanup() { clearAllActiveTranscriptPaths(); }
    void testOffscreenVulkanSubtitleTextPixels_data();
    void testOffscreenVulkanSubtitleTextPixels();
};

void TestVulkanSubtitleRender::testOffscreenVulkanSubtitleTextPixels_data()
{
    QTest::addColumn<QSize>("outputSize");
    QTest::addColumn<QString>("artifactSuffix");

    QTest::newRow("square-720") << QSize(720, 720) << QStringLiteral("720x720");
    QTest::newRow("portrait-1080x1920") << QSize(1080, 1920) << QStringLiteral("1080x1920");
    QTest::newRow("landscape-1920x1080") << QSize(1920, 1080) << QStringLiteral("1920x1080");
    QTest::newRow("preview-608x1080") << QSize(608, 1080) << QStringLiteral("608x1080");
    QTest::newRow("preview-512x512") << QSize(512, 512) << QStringLiteral("512x512");
}

void TestVulkanSubtitleRender::testOffscreenVulkanSubtitleTextPixels()
{
    QFETCH(QSize, outputSize);
    QFETCH(QString, artifactSuffix);

    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString clipPath = dir.filePath(QStringLiteral("clip.mp4"));
    QVERIFY(QFile(clipPath).open(QIODevice::WriteOnly));
    const QString transcriptPath = dir.filePath(QStringLiteral("clip_editable.json"));
    QVERIFY(writeTranscript(transcriptPath));
    setActiveTranscriptPathForClipFile(clipPath, transcriptPath);

    render_detail::OffscreenVulkanRenderer renderer;
    QString error;
    if (!renderer.initialize(outputSize, &error)) {
        QSKIP(qPrintable(QStringLiteral("Vulkan unavailable: %1").arg(error)));
    }

    RenderRequest request;
    request.outputPath = QStringLiteral("test://vulkan-subtitle");
    request.outputFormat = QStringLiteral("preview");
    request.outputSize = outputSize;
    request.correctionsEnabled = true;
    request.clips = QVector<TimelineClip>{makeTranscriptClip(clipPath, outputSize)};
    request.exportStartFrame = 15;
    request.exportEndFrame = 15;

    QVector<TimelineClip> orderedClips = request.clips;
    QHash<QString, editor::DecoderContext*> decoders;
    QHash<render_detail::RenderAsyncFrameKey, editor::FrameHandle> asyncCache;
    qint64 decodeMs = 0;
    qint64 textureMs = 0;
    qint64 compositeMs = 0;
    qint64 readbackMs = 0;
    const QImage frame = renderer.renderFrame(request,
                                              15,
                                              decoders,
                                              nullptr,
                                              &asyncCache,
                                              orderedClips,
                                              nullptr,
                                              &decodeMs,
                                              &textureMs,
                                              &compositeMs,
                                              &readbackMs,
                                              nullptr,
                                              nullptr);
    QVERIFY2(!frame.isNull(), "Offscreen Vulkan renderer returned a null frame");
    QCOMPARE(frame.size(), outputSize);
    const QString artifactPath =
        QDir(QStringLiteral(QT_TESTCASE_BUILDDIR)).filePath(
            QStringLiteral("vulkan_subtitle_render_%1.png").arg(artifactSuffix));
    QVERIFY2(frame.save(artifactPath), qPrintable(QStringLiteral("Failed to save %1").arg(artifactPath)));

    const PixelCounts counts = countSubtitlePixels(frame);
    const int framePixels = frame.width() * frame.height();
    QVERIFY2(counts.opaqueDark > framePixels * 0.012,
             qPrintable(QStringLiteral("Expected subtitle background pixels, got %1").arg(counts.opaqueDark)));
    QVERIFY2(counts.brightText > framePixels * 0.00018,
             qPrintable(QStringLiteral("Expected bright subtitle glyph pixels, got %1; yellow=%2 nonBlack=%3 dark=%4")
                            .arg(counts.brightText)
                            .arg(counts.yellowHighlight)
                            .arg(counts.nonBlack)
                            .arg(counts.opaqueDark)));
    QVERIFY2(counts.yellowHighlight > framePixels * 0.00018,
             qPrintable(QStringLiteral("Expected active-word highlight pixels, got %1").arg(counts.yellowHighlight)));
}

QTEST_MAIN(TestVulkanSubtitleRender)
#include "test_vulkan_subtitle_render.moc"
