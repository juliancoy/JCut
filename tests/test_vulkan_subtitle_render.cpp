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

extern "C" {
#include <libavutil/pixfmt.h>
}

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

TimelineClip makeTitleClip(const QSize& outputSize)
{
    TimelineClip clip;
    clip.id = QStringLiteral("vulkan-title-clip");
    clip.label = QStringLiteral("Vulkan title clip");
    clip.mediaType = ClipMediaType::Title;
    clip.videoEnabled = true;
    clip.audioEnabled = false;
    clip.hasAudio = false;
    clip.startFrame = 0;
    clip.durationFrames = 60;
    clip.sourceDurationFrames = 60;
    clip.trackIndex = 0;

    TimelineClip::TitleKeyframe keyframe;
    keyframe.frame = 0;
    keyframe.text = QStringLiteral("TITLE TEST");
    keyframe.fontFamily = QStringLiteral("DejaVu Sans");
    keyframe.fontSize = qBound<qreal>(36.0, qMin(outputSize.width(), outputSize.height()) * 0.11, 96.0);
    keyframe.bold = true;
    keyframe.color = QColor(QStringLiteral("#ffffff"));
    keyframe.opacity = 1.0;
    keyframe.windowEnabled = false;
    keyframe.dropShadowEnabled = false;
    clip.titleKeyframes.push_back(keyframe);
    return clip;
}

TimelineClip makeImageClip(const QString& imagePath)
{
    TimelineClip clip;
    clip.id = QStringLiteral("vulkan-image-clip");
    clip.label = QStringLiteral("Vulkan image clip");
    clip.filePath = imagePath;
    clip.mediaType = ClipMediaType::Image;
    clip.videoEnabled = true;
    clip.audioEnabled = false;
    clip.hasAudio = false;
    clip.startFrame = 0;
    clip.durationFrames = 60;
    clip.sourceDurationFrames = 60;
    clip.trackIndex = 0;
    return clip;
}

bool writeOrientationImage(const QString& path)
{
    QImage image(320, 240, QImage::Format_RGBA8888);
    image.fill(Qt::black);
    for (int y = 0; y < image.height(); ++y) {
        for (int x = 0; x < image.width(); ++x) {
            image.setPixelColor(x, y, y < image.height() / 2
                                      ? QColor(235, 32, 32, 255)
                                      : QColor(32, 80, 235, 255));
        }
    }
    return image.save(path);
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
    void testOffscreenVulkanTitleTextPixels();
    void testOffscreenVulkanImageTextureOrientation();
    void testOffscreenVulkanTickerPresetDrawsRepeatedImagePixels();
    void testOffscreenVulkanContinuousMaskOpacityAndShadow();
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

    render_detail::OffscreenRenderFrame rawOutput;
    QVERIFY2(renderer.renderFrameToOutput(request,
                                          15,
                                          decoders,
                                          nullptr,
                                          &asyncCache,
                                          orderedClips,
                                          &rawOutput,
                                          false),
             "Offscreen Vulkan renderer failed to render subtitle frame for raw export readback");
    QByteArray bgra(outputSize.width() * outputSize.height() * 4, '\0');
    AVFrame rawFrame{};
    rawFrame.format = AV_PIX_FMT_BGRA;
    rawFrame.width = outputSize.width();
    rawFrame.height = outputSize.height();
    rawFrame.data[0] = reinterpret_cast<uint8_t*>(bgra.data());
    rawFrame.linesize[0] = outputSize.width() * 4;
    QVERIFY2(renderer.copyLastFrameToBgra(&rawFrame, &readbackMs),
             "Offscreen Vulkan renderer failed to copy raw subtitle BGRA frame");
    QImage rawImage(reinterpret_cast<const uchar*>(bgra.constData()),
                    outputSize.width(),
                    outputSize.height(),
                    outputSize.width() * 4,
                    QImage::Format_ARGB32);
    const QString rawArtifactPath =
        QDir(QStringLiteral(QT_TESTCASE_BUILDDIR)).filePath(
            QStringLiteral("vulkan_subtitle_raw_bgra_%1.png").arg(artifactSuffix));
    QVERIFY2(rawImage.copy().save(rawArtifactPath),
             qPrintable(QStringLiteral("Failed to save %1").arg(rawArtifactPath)));
}

void TestVulkanSubtitleRender::testOffscreenVulkanTitleTextPixels()
{
    const QSize outputSize(720, 720);
    render_detail::OffscreenVulkanRenderer renderer;
    QString error;
    if (!renderer.initialize(outputSize, &error)) {
        QSKIP(qPrintable(QStringLiteral("Vulkan unavailable: %1").arg(error)));
    }

    RenderRequest request;
    request.outputPath = QStringLiteral("test://vulkan-title");
    request.outputFormat = QStringLiteral("preview");
    request.outputSize = outputSize;
    request.correctionsEnabled = true;
    request.clips = QVector<TimelineClip>{makeTitleClip(outputSize)};
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
    QVERIFY2(!frame.isNull(), "Offscreen Vulkan renderer returned a null title frame");
    QCOMPARE(frame.size(), outputSize);
    const QString artifactPath =
        QDir(QStringLiteral(QT_TESTCASE_BUILDDIR)).filePath(QStringLiteral("vulkan_title_render_720x720.png"));
    QVERIFY2(frame.save(artifactPath), qPrintable(QStringLiteral("Failed to save %1").arg(artifactPath)));

    const PixelCounts counts = countSubtitlePixels(frame);
    const int framePixels = frame.width() * frame.height();
    QVERIFY2(counts.brightText > framePixels * 0.00024,
             qPrintable(QStringLiteral("Expected bright title glyph pixels, got %1").arg(counts.brightText)));

    render_detail::OffscreenRenderFrame rawOutput;
    QVERIFY2(renderer.renderFrameToOutput(request,
                                          15,
                                          decoders,
                                          nullptr,
                                          &asyncCache,
                                          orderedClips,
                                          &rawOutput,
                                          false),
             "Offscreen Vulkan renderer failed to render title frame for raw export readback");
    QByteArray bgra(outputSize.width() * outputSize.height() * 4, '\0');
    AVFrame rawFrame{};
    rawFrame.format = AV_PIX_FMT_BGRA;
    rawFrame.width = outputSize.width();
    rawFrame.height = outputSize.height();
    rawFrame.data[0] = reinterpret_cast<uint8_t*>(bgra.data());
    rawFrame.linesize[0] = outputSize.width() * 4;
    QVERIFY2(renderer.copyLastFrameToBgra(&rawFrame, &readbackMs),
             "Offscreen Vulkan renderer failed to copy raw export-facing BGRA frame");
    QImage rawImage(reinterpret_cast<const uchar*>(bgra.constData()),
                    outputSize.width(),
                    outputSize.height(),
                    outputSize.width() * 4,
                    QImage::Format_ARGB32);
    const QString rawArtifactPath =
        QDir(QStringLiteral(QT_TESTCASE_BUILDDIR)).filePath(QStringLiteral("vulkan_title_raw_bgra_720x720.png"));
    QVERIFY2(rawImage.copy().save(rawArtifactPath),
             qPrintable(QStringLiteral("Failed to save %1").arg(rawArtifactPath)));
}

void TestVulkanSubtitleRender::testOffscreenVulkanImageTextureOrientation()
{
    const QSize outputSize(720, 720);
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString imagePath = dir.filePath(QStringLiteral("orientation.png"));
    QVERIFY2(writeOrientationImage(imagePath), "Failed to write Vulkan orientation image fixture");

    render_detail::OffscreenVulkanRenderer renderer;
    QString error;
    if (!renderer.initialize(outputSize, &error)) {
        QSKIP(qPrintable(QStringLiteral("Vulkan unavailable: %1").arg(error)));
    }

    RenderRequest request;
    request.outputPath = QStringLiteral("test://vulkan-image-orientation");
    request.outputFormat = QStringLiteral("preview");
    request.outputSize = outputSize;
    request.correctionsEnabled = true;
    request.clips = QVector<TimelineClip>{makeImageClip(imagePath)};
    request.exportStartFrame = 15;
    request.exportEndFrame = 15;

    QVector<TimelineClip> orderedClips = request.clips;
    QHash<QString, editor::DecoderContext*> decoders;
    QHash<render_detail::RenderAsyncFrameKey, editor::FrameHandle> asyncCache;
    qint64 decodeMs = 0;
    qint64 textureMs = 0;
    qint64 compositeMs = 0;
    qint64 readbackMs = 0;
    render_detail::OffscreenRenderFrame output;
    QVERIFY2(renderer.renderFrameToOutput(request,
                                          15,
                                          decoders,
                                          nullptr,
                                          &asyncCache,
                                          orderedClips,
                                          &output,
                                          true,
                                          nullptr,
                                          &decodeMs,
                                          &textureMs,
                                          &compositeMs,
                                          &readbackMs),
             "Offscreen Vulkan renderer failed to render image orientation frame");

    QByteArray bgra(outputSize.width() * outputSize.height() * 4, '\0');
    AVFrame rawFrame{};
    rawFrame.format = AV_PIX_FMT_BGRA;
    rawFrame.width = outputSize.width();
    rawFrame.height = outputSize.height();
    rawFrame.data[0] = reinterpret_cast<uint8_t*>(bgra.data());
    rawFrame.linesize[0] = outputSize.width() * 4;
    QVERIFY2(renderer.copyLastFrameToBgra(&rawFrame, &readbackMs),
             "Offscreen Vulkan renderer failed to copy raw image orientation BGRA frame");

    QImage rawImage(reinterpret_cast<const uchar*>(bgra.constData()),
                    outputSize.width(),
                    outputSize.height(),
                    outputSize.width() * 4,
                    QImage::Format_ARGB32);
    const QString rawArtifactPath =
        QDir(QStringLiteral(QT_TESTCASE_BUILDDIR)).filePath(QStringLiteral("vulkan_image_raw_bgra_720x720.png"));
    QVERIFY2(rawImage.copy().save(rawArtifactPath),
             qPrintable(QStringLiteral("Failed to save %1").arg(rawArtifactPath)));

    const QColor top = rawImage.pixelColor(outputSize.width() / 2, outputSize.height() / 4);
    const QColor bottom = rawImage.pixelColor(outputSize.width() / 2, (outputSize.height() * 3) / 4);
    QVERIFY2(top.red() > top.blue() + 80,
             qPrintable(QStringLiteral("Expected red top half, got rgb=(%1,%2,%3)")
                            .arg(top.red()).arg(top.green()).arg(top.blue())));
    QVERIFY2(bottom.blue() > bottom.red() + 80,
             qPrintable(QStringLiteral("Expected blue bottom half, got rgb=(%1,%2,%3)")
                            .arg(bottom.red()).arg(bottom.green()).arg(bottom.blue())));
}

void TestVulkanSubtitleRender::testOffscreenVulkanTickerPresetDrawsRepeatedImagePixels()
{
    const QSize outputSize(256, 256);
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString imagePath = dir.filePath(QStringLiteral("ticker_logo.png"));
    QImage logo(32, 16, QImage::Format_RGBA8888);
    logo.fill(QColor(235, 24, 24, 255));
    QVERIFY2(logo.save(imagePath), "Failed to write ticker logo fixture");

    render_detail::OffscreenVulkanRenderer renderer;
    QString error;
    if (!renderer.initialize(outputSize, &error)) {
        QSKIP(qPrintable(QStringLiteral("Vulkan unavailable: %1").arg(error)));
    }

    TimelineClip clip = makeImageClip(imagePath);
    clip.effectPreset = ClipEffectPreset::NewsLogoTicker;
    clip.effectRows = 8;
    clip.effectSpeed = 0.0;
    clip.effectScale = 1.0;
    clip.effectAlternateDirection = true;

    RenderRequest request;
    request.outputPath = QStringLiteral("test://vulkan-ticker-preset");
    request.outputFormat = QStringLiteral("preview");
    request.outputSize = outputSize;
    request.correctionsEnabled = true;
    request.clips = QVector<TimelineClip>{clip};
    request.exportStartFrame = 0;
    request.exportEndFrame = 0;

    QVector<TimelineClip> orderedClips = request.clips;
    QHash<QString, editor::DecoderContext*> decoders;
    QHash<render_detail::RenderAsyncFrameKey, editor::FrameHandle> asyncCache;
    qint64 decodeMs = 0;
    qint64 textureMs = 0;
    qint64 compositeMs = 0;
    qint64 readbackMs = 0;
    render_detail::OffscreenRenderFrame output;
    QVERIFY2(renderer.renderFrameToOutput(request,
                                          0,
                                          decoders,
                                          nullptr,
                                          &asyncCache,
                                          orderedClips,
                                          &output,
                                          true,
                                          nullptr,
                                          &decodeMs,
                                          &textureMs,
                                          &compositeMs,
                                          &readbackMs),
             "Offscreen Vulkan renderer failed to render ticker preset frame");

    QByteArray bgra(outputSize.width() * outputSize.height() * 4, '\0');
    AVFrame rawFrame{};
    rawFrame.format = AV_PIX_FMT_BGRA;
    rawFrame.width = outputSize.width();
    rawFrame.height = outputSize.height();
    rawFrame.data[0] = reinterpret_cast<uint8_t*>(bgra.data());
    rawFrame.linesize[0] = outputSize.width() * 4;
    QVERIFY2(renderer.copyLastFrameToBgra(&rawFrame, &readbackMs),
             "Offscreen Vulkan renderer failed to copy ticker preset BGRA frame");

    QImage rawImage(reinterpret_cast<const uchar*>(bgra.constData()),
                    outputSize.width(),
                    outputSize.height(),
                    outputSize.width() * 4,
                    QImage::Format_ARGB32);

    int rowsWithLogoPixels = 0;
    const int rowHeight = outputSize.height() / clip.effectRows;
    for (int row = 0; row < clip.effectRows; ++row) {
        int redPixels = 0;
        const int y0 = row * rowHeight;
        const int y1 = qMin(outputSize.height(), y0 + rowHeight);
        for (int y = y0; y < y1; ++y) {
            for (int x = 0; x < outputSize.width(); ++x) {
                const QColor c = rawImage.pixelColor(x, y);
                if (c.red() > 180 && c.green() < 80 && c.blue() < 80) {
                    ++redPixels;
                }
            }
        }
        if (redPixels > 80) {
            ++rowsWithLogoPixels;
        }
    }

    QVERIFY2(rowsWithLogoPixels >= 6,
             qPrintable(QStringLiteral("Expected ticker logo pixels across rows, got %1 rows")
                            .arg(rowsWithLogoPixels)));
}

void TestVulkanSubtitleRender::testOffscreenVulkanContinuousMaskOpacityAndShadow()
{
    const QSize outputSize(128, 128);
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString backgroundPath = dir.filePath(QStringLiteral("background.png"));
    const QString subjectPath = dir.filePath(QStringLiteral("subject.png"));
    QImage background(64, 64, QImage::Format_RGBA8888);
    background.fill(QColor(255, 255, 255, 255));
    QImage subject(64, 64, QImage::Format_RGBA8888);
    subject.fill(QColor(240, 32, 32, 255));
    QVERIFY(background.save(backgroundPath));
    QVERIFY(subject.save(subjectPath));

    const QString maskDir = dir.filePath(QStringLiteral("subject_birefnet_alpha_masks"));
    QVERIFY(QDir().mkpath(maskDir));
    QImage mask(64, 64, QImage::Format_Grayscale8);
    mask.fill(0);
    for (int y = 16; y < 32; ++y) {
        uchar* row = mask.scanLine(y);
        for (int x = 16; x < 24; ++x) row[x] = 255;
        for (int x = 24; x < 32; ++x) row[x] = 128;
    }
    QVERIFY(mask.save(QDir(maskDir).filePath(QStringLiteral("frame_000001.png"))));

    TimelineClip backgroundClip = makeImageClip(backgroundPath);
    backgroundClip.id = QStringLiteral("mask-shadow-background");
    backgroundClip.trackIndex = 0;
    TimelineClip sourceClip = makeImageClip(subjectPath);
    sourceClip.id = QStringLiteral("mask-shadow-source");
    sourceClip.trackIndex = 1;
    sourceClip.videoEnabled = false;
    sourceClip.maskEnabled = true;
    sourceClip.maskFramesDir = maskDir;
    sourceClip.maskForegroundLayerEnabled = true;
    sourceClip.maskOpacity = 0.5;
    sourceClip.maskDropShadowEnabled = true;
    sourceClip.maskDropShadowRadius = 0.0;
    sourceClip.maskDropShadowOffsetX = 24.0;
    sourceClip.maskDropShadowOffsetY = 0.0;
    sourceClip.maskDropShadowOpacity = 0.8;
    TimelineClip markerClip = sourceClip;
    markerClip.id = QStringLiteral("mask-shadow-marker");
    markerClip.clipRole = ClipRole::MaskMatte;
    markerClip.linkedSourceClipId = sourceClip.id;
    markerClip.locked = true;
    markerClip.sourceTransformLocked = true;
    markerClip.videoEnabled = true;
    markerClip.maskForegroundLayerEnabled = false;
    markerClip.trackIndex = 2;

    render_detail::OffscreenVulkanRenderer renderer;
    QString error;
    if (!renderer.initialize(outputSize, &error)) {
        QSKIP(qPrintable(QStringLiteral("Vulkan unavailable: %1").arg(error)));
    }
    RenderRequest request;
    request.outputPath = QStringLiteral("test://vulkan-continuous-mask-shadow");
    request.outputFormat = QStringLiteral("preview");
    request.outputSize = outputSize;
    request.correctionsEnabled = true;
    request.clips = QVector<TimelineClip>{backgroundClip, markerClip, sourceClip};
    request.exportStartFrame = 0;
    request.exportEndFrame = 0;

    QVector<TimelineClip> orderedClips = request.clips;
    QHash<QString, editor::DecoderContext*> decoders;
    QHash<render_detail::RenderAsyncFrameKey, editor::FrameHandle> asyncCache;
    qint64 decodeMs = 0;
    qint64 textureMs = 0;
    qint64 compositeMs = 0;
    qint64 readbackMs = 0;
    render_detail::OffscreenRenderFrame output;
    QVERIFY2(renderer.renderFrameToOutput(request, 0, decoders, nullptr, &asyncCache,
                                          orderedClips, &output, true, nullptr,
                                          &decodeMs, &textureMs, &compositeMs, &readbackMs),
             "Offscreen Vulkan renderer failed to render continuous mask fixture");

    QByteArray bgra(outputSize.width() * outputSize.height() * 4, '\0');
    AVFrame rawFrame{};
    rawFrame.format = AV_PIX_FMT_BGRA;
    rawFrame.width = outputSize.width();
    rawFrame.height = outputSize.height();
    rawFrame.data[0] = reinterpret_cast<uint8_t*>(bgra.data());
    rawFrame.linesize[0] = outputSize.width() * 4;
    QVERIFY(renderer.copyLastFrameToBgra(&rawFrame, &readbackMs));
    QImage rendered(reinterpret_cast<const uchar*>(bgra.constData()),
                    outputSize.width(), outputSize.height(), outputSize.width() * 4,
                    QImage::Format_ARGB32);

    const QColor backgroundPixel = rendered.pixelColor(8, 8);
    const QColor foregroundPixel = rendered.pixelColor(40, 40);
    const QColor softAlphaPixel = rendered.pixelColor(54, 40);
    const QColor shadowPixel = rendered.pixelColor(68, 40);
    QVERIFY2(backgroundPixel.red() > 245 && backgroundPixel.green() > 245,
             "unmasked background must remain white");
    QVERIFY2(foregroundPixel.red() > 230 &&
                 foregroundPixel.green() > 100 && foregroundPixel.green() < 180,
             qPrintable(QStringLiteral("mask opacity did not blend foreground: %1,%2,%3")
                            .arg(foregroundPixel.red()).arg(foregroundPixel.green())
                            .arg(foregroundPixel.blue())));
    QVERIFY2(softAlphaPixel.red() > 235 &&
                 softAlphaPixel.green() > 175 && softAlphaPixel.green() < 225,
             qPrintable(QStringLiteral("continuous alpha was quantized: %1,%2,%3")
                            .arg(softAlphaPixel.red()).arg(softAlphaPixel.green())
                            .arg(softAlphaPixel.blue())));
    QVERIFY2(shadowPixel.red() > 110 && shadowPixel.red() < 200 &&
                 qAbs(shadowPixel.red() - shadowPixel.green()) < 6,
             qPrintable(QStringLiteral("mask shadow was not composited: %1,%2,%3")
                            .arg(shadowPixel.red()).arg(shadowPixel.green())
                            .arg(shadowPixel.blue())));
}

QTEST_MAIN(TestVulkanSubtitleRender)
#include "test_vulkan_subtitle_render.moc"
