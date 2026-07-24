#include "../background_fill_effect.h"
#include "../editor_timeline_types.h"
#include "../render_internal.h"
#include "../render_vulkan_shared.h"

#include <QtTest/QtTest>

#include <QColor>
#include <QDir>
#include <QImage>
#include <QPointF>
#include <QVector>

#include <algorithm>
#include <cmath>
#include <limits>

namespace {

constexpr int kSourceW = 160;
constexpr int kSourceH = 96;
constexpr int kOutputW = 384;
constexpr int kOutputH = 320;
constexpr int kClipLeft = 112;
constexpr int kClipTop = 112;
constexpr int kEdgePixels = 18;
constexpr double kPower = 1.35;
constexpr int kCell = 16;

double clamp01(double value)
{
    return std::clamp(value, 0.0, 1.0);
}

QColor lerp(const QColor& a, const QColor& b, double t)
{
    const double u = clamp01(t);
    return QColor(qRound(a.red() + (b.red() - a.red()) * u),
                  qRound(a.green() + (b.green() - a.green()) * u),
                  qRound(a.blue() + (b.blue() - a.blue()) * u),
                  qRound(a.alpha() + (b.alpha() - a.alpha()) * u));
}

QColor bilinearSample(const QImage& image, double u, double v)
{
    const double sx = clamp01(u) * image.width() - 0.5;
    const double sy = clamp01(v) * image.height() - 0.5;
    const int x0 = std::clamp(static_cast<int>(std::floor(sx)), 0, image.width() - 1);
    const int y0 = std::clamp(static_cast<int>(std::floor(sy)), 0, image.height() - 1);
    const int x1 = std::clamp(x0 + 1, 0, image.width() - 1);
    const int y1 = std::clamp(y0 + 1, 0, image.height() - 1);
    const double tx = sx - std::floor(sx);
    const double ty = sy - std::floor(sy);
    const QColor top = lerp(image.pixelColor(x0, y0), image.pixelColor(x1, y0), tx);
    const QColor bottom = lerp(image.pixelColor(x0, y1), image.pixelColor(x1, y1), tx);
    return lerp(top, bottom, ty);
}

QImage makeCheckerboardSource()
{
    QImage image(kSourceW, kSourceH, QImage::Format_RGBA8888);
    for (int y = 0; y < image.height(); ++y) {
        for (int x = 0; x < image.width(); ++x) {
            const bool light = ((x / kCell) + (y / kCell)) % 2 == 0;
            QColor c = light ? QColor(236, 236, 236, 255) : QColor(32, 32, 32, 255);
            if (y < 6) {
                c = QColor(235, 32, 32, 255);
            } else if (y >= image.height() - 6) {
                c = QColor(32, 210, 72, 255);
            }
            if (x < 6) {
                c = QColor(32, 92, 235, 255);
            } else if (x >= image.width() - 6) {
                c = QColor(245, 220, 45, 255);
            }
            image.setPixelColor(x, y, c);
        }
    }
    return image;
}

QImage makeShaderReference(const QImage& source)
{
    const QImage renderTexture =
        source.scaled(QSize(kOutputW, kOutputH), Qt::IgnoreAspectRatio, Qt::SmoothTransformation)
            .convertToFormat(QImage::Format_RGBA8888);
    QImage expected(kOutputW, kOutputH, QImage::Format_ARGB32_Premultiplied);
    expected.fill(Qt::transparent);

    const double centerX = (kClipLeft + kSourceW * 0.5) / kOutputW;
    const double centerY = (kClipTop + kSourceH * 0.5) / kOutputH;
    const double outputAspect = static_cast<double>(kOutputW) / kOutputH;
    const double inverseWidth = static_cast<double>(kOutputH) / kSourceW;
    const double signedInverseHeight = static_cast<double>(kOutputH) / kSourceH;
    const double halfTexelX = 0.5 / renderTexture.width();
    const double halfTexelY = 0.5 / renderTexture.height();
    const double edgeSpanX = std::clamp(static_cast<double>(kEdgePixels) / renderTexture.width(),
                                        halfTexelX,
                                        1.0 - halfTexelX);
    const double edgeSpanY = std::clamp(static_cast<double>(kEdgePixels) / renderTexture.height(),
                                        halfTexelY,
                                        1.0 - halfTexelY);

    for (int y = 0; y < expected.height(); ++y) {
        for (int x = 0; x < expected.width(); ++x) {
            if (x >= kClipLeft && x < kClipLeft + kSourceW &&
                y >= kClipTop && y < kClipTop + kSourceH) {
                expected.setPixelColor(x, y, bilinearSample(renderTexture,
                                                            (x - kClipLeft + 0.5) / kSourceW,
                                                            (y - kClipTop + 0.5) / kSourceH));
                continue;
            }

            const double uvX = (x + 0.5) / kOutputW;
            const double uvY = (y + 0.5) / kOutputH;
            const double deltaX = (uvX - centerX) * outputAspect;
            const double deltaY = uvY - centerY;
            double sourceUvX = deltaX * inverseWidth + 0.5;
            double sourceUvY = deltaY * signedInverseHeight + 0.5;

            if (sourceUvX < 0.0 || sourceUvX > 1.0 ||
                sourceUvY < 0.0 || sourceUvY > 1.0) {
                const double edgeRatioX = std::abs(deltaX) * 2.0 * inverseWidth;
                const double edgeRatioY = std::abs(deltaY) * 2.0 * std::abs(signedInverseHeight);
                const double outsideScale = std::max(edgeRatioX, edgeRatioY);
                const double mediaEdgeScale = 1.0 / std::max(1.0, outsideScale);
                const double mediaEdgeLocalX = deltaX * mediaEdgeScale;
                const double mediaEdgeLocalY = deltaY * mediaEdgeScale;

                const double canvasScaleX = deltaX > 0.0
                    ? ((1.0 - centerX) * outputAspect) / std::max(0.0001, deltaX)
                    : (-centerX * outputAspect) / std::min(-0.0001, deltaX);
                const double canvasScaleY = deltaY > 0.0
                    ? (1.0 - centerY) / std::max(0.0001, deltaY)
                    : (-centerY) / std::min(-0.0001, deltaY);
                const double canvasEdgeScale = std::min(canvasScaleX, canvasScaleY);
                const double fillT = clamp01((1.0 - mediaEdgeScale) /
                                             std::max(0.0001, canvasEdgeScale - mediaEdgeScale));
                const double scanT = std::pow(fillT, kPower);
                sourceUvX = mediaEdgeLocalX * inverseWidth + 0.5;
                sourceUvY = mediaEdgeLocalY * signedInverseHeight + 0.5;

                const double mediaEdgeRatioX = std::abs(mediaEdgeLocalX) * 2.0 * inverseWidth;
                const double mediaEdgeRatioY =
                    std::abs(mediaEdgeLocalY) * 2.0 * std::abs(signedInverseHeight);
                if (mediaEdgeRatioX > mediaEdgeRatioY + 0.000001) {
                    sourceUvX = mediaEdgeLocalX < 0.0
                        ? (halfTexelX + (edgeSpanX - halfTexelX) * scanT)
                        : ((1.0 - halfTexelX) +
                           ((1.0 - edgeSpanX) - (1.0 - halfTexelX)) * scanT);
                } else {
                    sourceUvY = mediaEdgeLocalY * signedInverseHeight < 0.0
                        ? (halfTexelY + (edgeSpanY - halfTexelY) * scanT)
                        : ((1.0 - halfTexelY) +
                           ((1.0 - edgeSpanY) - (1.0 - halfTexelY)) * scanT);
                }
            }

            expected.setPixelColor(x, y, bilinearSample(renderTexture, sourceUvX, sourceUvY));
        }
    }
    return expected;
}

QColor sourceOver(const QColor& foreground, double opacity, const QColor& background)
{
    const double sourceAlpha = clamp01(foreground.alphaF() * opacity);
    const double destinationAlpha = clamp01(background.alphaF());
    const double outputAlpha =
        sourceAlpha + destinationAlpha * (1.0 - sourceAlpha);
    if (outputAlpha <= 0.0) {
        return Qt::transparent;
    }
    const auto channel = [&](double foregroundChannel, double backgroundChannel) {
        return (foregroundChannel * sourceAlpha +
                backgroundChannel * destinationAlpha * (1.0 - sourceAlpha)) /
            outputAlpha;
    };
    return QColor::fromRgbF(
        clamp01(channel(foreground.redF(), background.redF())),
        clamp01(channel(foreground.greenF(), background.greenF())),
        clamp01(channel(foreground.blueF(), background.blueF())),
        clamp01(outputAlpha));
}

QImage makeBidirectionalShaderReference(const QImage& source, double clipScale)
{
    const QImage renderTexture =
        source.scaled(QSize(kOutputW, kOutputH), Qt::IgnoreAspectRatio, Qt::SmoothTransformation)
            .convertToFormat(QImage::Format_RGBA8888);
    const double clipWidth = kSourceW * clipScale;
    const double clipHeight = kSourceH * clipScale;
    const double left = (kOutputW - clipWidth) * 0.5;
    const double top = (kOutputH - clipHeight) * 0.5;
    const double centerX = kOutputW * 0.5;
    const double centerY = kOutputH * 0.5;
    const double halfWidth = clipWidth * 0.5;
    const double halfHeight = clipHeight * 0.5;

    QImage expected(kOutputW, kOutputH, QImage::Format_RGBA8888);
    expected.fill(Qt::transparent);
    for (int y = 0; y < expected.height(); ++y) {
        for (int x = 0; x < expected.width(); ++x) {
            const double pixelX = x + 0.5;
            const double pixelY = y + 0.5;
            const bool insideClip =
                pixelX >= left && pixelX <= left + clipWidth &&
                pixelY >= top && pixelY <= top + clipHeight;
            QColor base = Qt::transparent;
            if (insideClip) {
                base = bilinearSample(
                    renderTexture,
                    (pixelX - left) / clipWidth,
                    (pixelY - top) / clipHeight);
            }

            const double dx = pixelX - centerX;
            const double dy = pixelY - centerY;
            const double normalizedX = dx / std::max(0.5, halfWidth);
            const double normalizedY = dy / std::max(0.5, halfHeight);
            const double radius = std::pow(
                std::pow(std::abs(normalizedX), 4.0) +
                    std::pow(std::abs(normalizedY), 4.0),
                0.25);
            if (radius <= 0.000001) {
                expected.setPixelColor(x, y, base);
                continue;
            }
            const double directionX = normalizedX / radius;
            const double directionY = normalizedY / radius;
            const double boundaryPixelRadius = std::hypot(
                directionX * renderTexture.width() * 0.5,
                directionY * renderTexture.height() * 0.5);
            const double coreRadius = std::clamp(
                1.0 - kEdgePixels / std::max(1.0, boundaryPixelRadius),
                0.05,
                0.995);
            if (radius <= coreRadius) {
                expected.setPixelColor(x, y, base);
                continue;
            }

            const double canvasScaleX = std::abs(dx) < 0.0001
                ? std::numeric_limits<double>::infinity()
                : (dx > 0.0 ? (kOutputW - centerX) / dx : -centerX / dx);
            const double canvasScaleY = std::abs(dy) < 0.0001
                ? std::numeric_limits<double>::infinity()
                : (dy > 0.0 ? (kOutputH - centerY) / dy : -centerY / dy);
            const double canvasRadius = std::max(
                radius,
                radius * std::min(canvasScaleX, canvasScaleY));
            const double stretch = clamp01(
                (radius - coreRadius) /
                std::max(0.0001, canvasRadius - coreRadius));
            const double sampleRadius =
                coreRadius + (1.0 - coreRadius) * std::pow(stretch, kPower);
            const double polarLength =
                std::max(0.000001, std::hypot(normalizedX, normalizedY));
            const double polarX = normalizedX / polarLength;
            const double polarY = normalizedY / polarLength;
            const auto sampleRing = [&](double angle) {
                const double cosine = std::cos(angle);
                const double sine = std::sin(angle);
                const double rotatedX = cosine * polarX - sine * polarY;
                const double rotatedY = sine * polarX + cosine * polarY;
                const double norm = std::pow(
                    std::pow(std::abs(rotatedX), 4.0) +
                        std::pow(std::abs(rotatedY), 4.0),
                    0.25);
                return bilinearSample(
                    renderTexture,
                    0.5 + (rotatedX / norm) * sampleRadius * 0.5,
                    0.5 + (rotatedY / norm) * sampleRadius * 0.5);
            };
            QColor warped = sampleRing(0.0);
            const double minClipPixels = std::min(clipWidth, clipHeight);
            if (minClipPixels < 128.0) {
                const double angularStep =
                    std::clamp(2.0 / std::max(1.0, minClipPixels), 0.0, 0.18);
                const QColor positive = sampleRing(angularStep);
                const QColor negative = sampleRing(-angularStep);
                const QColor positiveWide = sampleRing(2.0 * angularStep);
                const QColor negativeWide = sampleRing(-2.0 * angularStep);
                warped = QColor::fromRgbF(
                    warped.redF() * 0.4 +
                        (positive.redF() + negative.redF()) * 0.2 +
                        (positiveWide.redF() + negativeWide.redF()) * 0.1,
                    warped.greenF() * 0.4 +
                        (positive.greenF() + negative.greenF()) * 0.2 +
                        (positiveWide.greenF() + negativeWide.greenF()) * 0.1,
                    warped.blueF() * 0.4 +
                        (positive.blueF() + negative.blueF()) * 0.2 +
                        (positiveWide.blueF() + negativeWide.blueF()) * 0.1,
                    warped.alphaF() * 0.4 +
                        (positive.alphaF() + negative.alphaF()) * 0.2 +
                        (positiveWide.alphaF() + negativeWide.alphaF()) * 0.1);
            }
            const double featherWidth =
                std::max(0.002, (1.0 - coreRadius) * 0.25);
            const double featherT = clamp01(
                (radius - coreRadius) / featherWidth);
            const double overlayAlpha =
                featherT * featherT * (3.0 - 2.0 * featherT);
            expected.setPixelColor(
                x,
                y,
                sourceOver(warped, overlayAlpha, base));
        }
    }
    return expected;
}

TimelineClip makeProgressiveStretchClip(const QString& sourcePath)
{
    TimelineClip clip;
    clip.id = QStringLiteral("checkerboard-progressive-stretch");
    clip.label = QStringLiteral("Checkerboard progressive stretch");
    clip.filePath = sourcePath;
    clip.mediaType = ClipMediaType::Image;
    clip.sourceKind = MediaSourceKind::File;
    clip.clipRole = ClipRole::Media;
    clip.videoEnabled = true;
    clip.audioEnabled = false;
    clip.hasAudio = false;
    clip.startFrame = 0;
    clip.durationFrames = 1;
    clip.sourceDurationFrames = 1;
    clip.trackIndex = 0;
    clip.sourceFrameSize = QSize(kSourceW, kSourceH);
    clip.baseTranslationX = 0.0;
    clip.baseTranslationY = 0.0;
    clip.baseRotation = 0.0;
    clip.baseScaleX = static_cast<qreal>(kSourceW) / kOutputW;
    clip.baseScaleY = static_cast<qreal>(kSourceH) / ((static_cast<qreal>(kOutputW) / kSourceW) * kSourceH);
    clip.edgeFillEffect = BackgroundFillEffect::ProgressiveEdgeStretch;
    clip.edgeFillPixels = kEdgePixels;
    clip.edgeFillPower = kPower;
    return clip;
}

TimelineClip makeBidirectionalStretchClip(const QString& sourcePath, double clipScale)
{
    TimelineClip clip = makeProgressiveStretchClip(sourcePath);
    clip.id = QStringLiteral("checkerboard-progressive-bidirectional-stretch");
    clip.label = QStringLiteral("Checkerboard progressive bidirectional stretch");
    clip.baseScaleX *= clipScale;
    clip.baseScaleY *= clipScale;
    clip.edgeFillEffect =
        BackgroundFillEffect::ProgressiveBidirectionalEdgeStretch;
    return clip;
}

struct DiffStats {
    int maxChannelDelta = 0;
    int differingPixels = 0;
    int materiallyDifferingPixels = 0;
};

DiffStats compareImages(const QImage& actual, const QImage& expected, QImage* diff)
{
    DiffStats stats;
    *diff = QImage(actual.size(), QImage::Format_RGBA8888);
    diff->fill(Qt::black);
    for (int y = 0; y < actual.height(); ++y) {
        for (int x = 0; x < actual.width(); ++x) {
            const QColor a = actual.pixelColor(x, y);
            const QColor e = expected.pixelColor(x, y);
            const int dr = std::abs(a.red() - e.red());
            const int dg = std::abs(a.green() - e.green());
            const int db = std::abs(a.blue() - e.blue());
            const int da = std::abs(a.alpha() - e.alpha());
            const int d = std::max({dr, dg, db, da});
            stats.maxChannelDelta = std::max(stats.maxChannelDelta, d);
            if (d != 0) {
                ++stats.differingPixels;
            }
            if (d > 4) {
                ++stats.materiallyDifferingPixels;
            }
            diff->setPixelColor(x, y, QColor(d, d, d, 255));
        }
    }
    return stats;
}

} // namespace

class ProgressiveEdgeStretchRenderPathTest : public QObject {
    Q_OBJECT

private slots:
    void shaderRenderPathMatchesCheckerboardReference()
    {
        const QString artifactDirPath =
            QDir(QStringLiteral(JCUT_BINARY_DIR)).filePath(
                QStringLiteral("test_artifacts/progressive_edge_stretch_render_path"));
        QDir artifactDir(artifactDirPath);
        QVERIFY2(artifactDir.mkpath(QStringLiteral(".")),
                 qPrintable(QStringLiteral("Failed to create %1").arg(artifactDirPath)));

        const QImage source = makeCheckerboardSource();
        const QString sourcePath = artifactDir.filePath(QStringLiteral("checkerboard_source.png"));
        QVERIFY2(source.save(sourcePath),
                 qPrintable(QStringLiteral("Failed to save %1").arg(sourcePath)));

        const QImage expected = makeShaderReference(source);
        const QString expectedPath =
            artifactDir.filePath(QStringLiteral("progressive_edge_stretch_expected.png"));
        QVERIFY2(expected.save(expectedPath),
                 qPrintable(QStringLiteral("Failed to save %1").arg(expectedPath)));

        render_detail::OffscreenVulkanRenderer renderer;
        QString error;
        if (!renderer.initialize(QSize(kOutputW, kOutputH), &error)) {
            QSKIP(qPrintable(QStringLiteral("Vulkan unavailable: %1").arg(error)));
        }

        RenderRequest request;
        request.outputPath = QStringLiteral("test://progressive-edge-stretch-render-path");
        request.outputFormat = QStringLiteral("preview");
        request.outputSize = QSize(kOutputW, kOutputH);
        request.correctionsEnabled = true;
        request.backgroundFillEffect = BackgroundFillEffect::None;
        request.backgroundFillOpacity = 1.0;
        request.backgroundFillBrightness = 0.0;
        request.backgroundFillSaturation = 1.0;
        request.backgroundFillEdgePixels = 1;
        request.backgroundFillEdgePower = 2.0;
        request.clips = QVector<TimelineClip>{makeProgressiveStretchClip(sourcePath)};
        request.exportStartFrame = 0;
        request.exportEndFrame = 0;
        const render_detail::VulkanProgressiveEdgeStretchLayerPolicy policy =
            render_detail::vulkanProgressiveEdgeStretchLayerPolicy(request.clips.first(), request.tracks);
        QVERIFY(policy.sourceEligible);
        QVERIFY(!policy.presetActive);
        QVERIFY(!policy.drawBackground);

        QVector<TimelineClip> orderedClips = request.clips;
        QHash<QString, editor::DecoderContext*> decoders;
        QHash<render_detail::RenderAsyncFrameKey, editor::FrameHandle> asyncCache;
        qint64 decodeMs = 0;
        qint64 textureMs = 0;
        qint64 compositeMs = 0;
        qint64 readbackMs = 0;
        const QImage actual = renderer.renderFrame(request,
                                                   0,
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
        QVERIFY2(!actual.isNull(), "Offscreen Vulkan renderer returned a null frame");
        QCOMPARE(actual.size(), expected.size());

        const QImage actualRepeat = renderer.renderFrame(request,
                                                         0,
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
        QVERIFY2(!actualRepeat.isNull(), "Offscreen Vulkan renderer returned a null repeat frame");
        QImage repeatDiff;
        const DiffStats repeatStats = compareImages(actual, actualRepeat, &repeatDiff);
        QCOMPARE(repeatStats.maxChannelDelta, 0);
        QCOMPARE(repeatStats.differingPixels, 0);

        const QString actualPath =
            artifactDir.filePath(QStringLiteral("progressive_edge_stretch_vulkan.png"));
        QVERIFY2(actual.save(actualPath),
                 qPrintable(QStringLiteral("Failed to save %1").arg(actualPath)));

        QImage diff;
        const DiffStats stats = compareImages(actual, expected, &diff);
        const QString diffPath =
            artifactDir.filePath(QStringLiteral("progressive_edge_stretch_diff.png"));
        QVERIFY2(diff.save(diffPath),
                 qPrintable(QStringLiteral("Failed to save %1").arg(diffPath)));

        QVERIFY2(stats.differingPixels <= 2500,
                 qPrintable(QStringLiteral("CPU oracle boundary mismatch too large: %1 pixels, max delta %2")
                                .arg(stats.differingPixels)
                                .arg(stats.maxChannelDelta)));
    }

    void bidirectionalStretchStaysContinuousAtLowScale()
    {
        constexpr double kLowScale = 0.125;
        const QString artifactDirPath =
            QDir(QStringLiteral(JCUT_BINARY_DIR)).filePath(
                QStringLiteral("test_artifacts/progressive_edge_stretch_render_path"));
        QDir artifactDir(artifactDirPath);
        QVERIFY2(artifactDir.mkpath(QStringLiteral(".")),
                 qPrintable(QStringLiteral("Failed to create %1").arg(artifactDirPath)));

        const QImage source = makeCheckerboardSource();
        const QString sourcePath =
            artifactDir.filePath(QStringLiteral("bidirectional_source.png"));
        QVERIFY(source.save(sourcePath));
        const QImage expected =
            makeBidirectionalShaderReference(source, kLowScale);
        QVERIFY(expected.save(
            artifactDir.filePath(QStringLiteral("bidirectional_low_scale_expected.png"))));

        render_detail::OffscreenVulkanRenderer renderer;
        QString error;
        if (!renderer.initialize(QSize(kOutputW, kOutputH), &error)) {
            QSKIP(qPrintable(QStringLiteral("Vulkan unavailable: %1").arg(error)));
        }

        RenderRequest request;
        request.outputPath =
            QStringLiteral("test://progressive-bidirectional-edge-stretch");
        request.outputFormat = QStringLiteral("preview");
        request.outputSize = QSize(kOutputW, kOutputH);
        request.correctionsEnabled = true;
        request.backgroundFillEffect = BackgroundFillEffect::None;
        request.clips =
            QVector<TimelineClip>{makeBidirectionalStretchClip(sourcePath, kLowScale)};
        request.exportStartFrame = 0;
        request.exportEndFrame = 0;

        QVector<TimelineClip> orderedClips = request.clips;
        QHash<QString, editor::DecoderContext*> decoders;
        QHash<render_detail::RenderAsyncFrameKey, editor::FrameHandle> asyncCache;
        qint64 decodeMs = 0;
        qint64 textureMs = 0;
        qint64 compositeMs = 0;
        qint64 readbackMs = 0;
        const QImage actual = renderer.renderFrame(
            request,
            0,
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
        QVERIFY2(!actual.isNull(), "Offscreen Vulkan renderer returned a null frame");
        QCOMPARE(actual.size(), expected.size());
        QVERIFY(actual.save(
            artifactDir.filePath(QStringLiteral("bidirectional_low_scale_vulkan.png"))));

        QImage diff;
        const DiffStats stats = compareImages(actual, expected, &diff);
        QVERIFY(diff.save(
            artifactDir.filePath(QStringLiteral("bidirectional_low_scale_diff.png"))));
        QVERIFY2(
            stats.materiallyDifferingPixels <= 500,
            qPrintable(
                QStringLiteral(
                    "Low-scale bidirectional CPU oracle mismatch too large: %1 material pixels "
                    "(%2 total), max delta %3")
                    .arg(stats.materiallyDifferingPixels)
                    .arg(stats.differingPixels)
                    .arg(stats.maxChannelDelta)));
    }

    void tileAndMirrorFillTheCanvas()
    {
        const QString artifactDirPath =
            QDir(QStringLiteral(JCUT_BINARY_DIR)).filePath(
                QStringLiteral("test_artifacts/progressive_edge_stretch_render_path"));
        QDir artifactDir(artifactDirPath);
        QVERIFY(artifactDir.mkpath(QStringLiteral(".")));
        const QString sourcePath =
            artifactDir.filePath(QStringLiteral("edge_fill_modes_source.png"));
        QVERIFY(makeCheckerboardSource().save(sourcePath));

        render_detail::OffscreenVulkanRenderer renderer;
        QString error;
        if (!renderer.initialize(QSize(kOutputW, kOutputH), &error)) {
            QSKIP(qPrintable(QStringLiteral("Vulkan unavailable: %1").arg(error)));
        }

        const auto renderMode = [&](BackgroundFillEffect effect, const QString& name) {
            TimelineClip clip = makeProgressiveStretchClip(sourcePath);
            clip.edgeFillEffect = effect;
            RenderRequest request;
            request.outputPath = QStringLiteral("test://") + name;
            request.outputFormat = QStringLiteral("preview");
            request.outputSize = QSize(kOutputW, kOutputH);
            request.correctionsEnabled = true;
            request.backgroundFillEffect = BackgroundFillEffect::None;
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
            const QImage image = renderer.renderFrame(
                request,
                0,
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
            image.save(artifactDir.filePath(name + QStringLiteral(".png")));
            return image;
        };

        const QImage tile = renderMode(
            BackgroundFillEffect::Tile, QStringLiteral("edge_fill_tile"));
        const QImage mirror = renderMode(
            BackgroundFillEffect::Mirror, QStringLiteral("edge_fill_mirror"));
        QVERIFY(!tile.isNull());
        QVERIFY(!mirror.isNull());
        QVERIFY(tile.pixelColor(2, 2).alpha() > 240);
        QVERIFY(mirror.pixelColor(2, 2).alpha() > 240);
        QImage diff;
        const DiffStats stats = compareImages(tile, mirror, &diff);
        QVERIFY2(
            stats.materiallyDifferingPixels > 1000,
            "Tile and Mirror must produce distinct full-canvas sampling.");
    }
};

QTEST_MAIN(ProgressiveEdgeStretchRenderPathTest)
#include "test_progressive_edge_stretch_render_path.moc"
