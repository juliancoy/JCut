#include <QtTest/QtTest>

#include <QFile>
#include <QGuiApplication>
#include <QHash>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QDir>
#include <QTemporaryDir>

#include <cmath>
#include <limits>

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

struct ImageDelta {
    qreal meanAbsoluteChannelError = 0.0;
    int largeDifferencePixels = 0;
};

QImage overlayCompositedOverBlack(const render_detail::OverlayImage& overlay)
{
    QImage composited(overlay.width, overlay.height, QImage::Format_ARGB32_Premultiplied);
    composited.fill(Qt::black);
    const uchar* src = reinterpret_cast<const uchar*>(overlay.rgbaPremultiplied.constData());
    for (int y = 0; y < overlay.height; ++y) {
        QRgb* dst = reinterpret_cast<QRgb*>(composited.scanLine(y));
        for (int x = 0; x < overlay.width; ++x) {
            const int offset = ((y * overlay.width) + x) * 4;
            const int alpha = src[offset + 3];
            const int red = (src[offset + 0] * alpha) / 255;
            const int green = (src[offset + 1] * alpha) / 255;
            const int blue = (src[offset + 2] * alpha) / 255;
            dst[x] = qRgba(qBound(0, red, 255), qBound(0, green, 255), qBound(0, blue, 255), 255);
        }
    }
    return composited;
}

ImageDelta compareImages(const QImage& expected, const QImage& actual)
{
    ImageDelta delta;
    if (expected.size() != actual.size() || expected.isNull() || actual.isNull()) {
        delta.meanAbsoluteChannelError = std::numeric_limits<qreal>::infinity();
        delta.largeDifferencePixels = std::numeric_limits<int>::max();
        return delta;
    }
    qint64 channelError = 0;
    for (int y = 0; y < expected.height(); ++y) {
        for (int x = 0; x < expected.width(); ++x) {
            const QColor e = expected.pixelColor(x, y);
            const QColor a = actual.pixelColor(x, y);
            const int dr = qAbs(e.red() - a.red());
            const int dg = qAbs(e.green() - a.green());
            const int db = qAbs(e.blue() - a.blue());
            const int da = qAbs(e.alpha() - a.alpha());
            channelError += dr + dg + db + da;
            if (qMax(qMax(dr, dg), qMax(db, da)) > 24) {
                ++delta.largeDifferencePixels;
            }
        }
    }
    delta.meanAbsoluteChannelError =
        static_cast<qreal>(channelError) / static_cast<qreal>(expected.width() * expected.height() * 4);
    return delta;
}

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
    QHash<QString, QVector<TranscriptSection>> transcriptCache;
    const render_detail::OverlayImage overlay =
        render_detail::renderTranscriptOverlay(outputSize,
                                               request,
                                               15,
                                               orderedClips,
                                               transcriptCache);
    QVERIFY2(!overlay.isNull(), "Transcript overlay helper returned a null overlay");
    QCOMPARE(overlay.width, outputSize.width());
    QCOMPARE(overlay.height, outputSize.height());
    QImage cpuOverlay(reinterpret_cast<const uchar*>(overlay.rgbaPremultiplied.constData()),
                      overlay.width,
                      overlay.height,
                      overlay.width * 4,
                      QImage::Format_RGBA8888_Premultiplied);
    const QString cpuArtifactPath =
        QDir(QStringLiteral(QT_TESTCASE_BUILDDIR)).filePath(
            QStringLiteral("cpu_subtitle_overlay_%1.png").arg(artifactSuffix));
    QVERIFY2(cpuOverlay.copy().save(cpuArtifactPath),
             qPrintable(QStringLiteral("Failed to save %1").arg(cpuArtifactPath)));
    const QImage expectedFrame = overlayCompositedOverBlack(overlay).mirrored();
    const QString expectedArtifactPath =
        QDir(QStringLiteral(QT_TESTCASE_BUILDDIR)).filePath(
            QStringLiteral("expected_vulkan_subtitle_render_%1.png").arg(artifactSuffix));
    QVERIFY2(expectedFrame.save(expectedArtifactPath),
             qPrintable(QStringLiteral("Failed to save %1").arg(expectedArtifactPath)));

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

    const ImageDelta delta = compareImages(expectedFrame, frame);
    const int allowedLargeDifferencePixels =
        qMax(1, static_cast<int>(std::ceil(frame.width() * frame.height() * 0.006)));
    QVERIFY2(delta.meanAbsoluteChannelError < 1.0 && delta.largeDifferencePixels < allowedLargeDifferencePixels,
             qPrintable(QStringLiteral("Vulkan subtitle render diverged from CPU reference: mean_abs_channel_error=%1 large_difference_pixels=%2")
                            .arg(delta.meanAbsoluteChannelError, 0, 'f', 3)
                            .arg(delta.largeDifferencePixels)));

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
