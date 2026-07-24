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
    clip.edgeFillEnabled = true;
    clip.edgeFillProgressive = true;
    clip.edgeFillPixels = kEdgePixels;
    clip.edgeFillPower = kPower;
    return clip;
}

struct DiffStats {
    int maxChannelDelta = 0;
    int differingPixels = 0;
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
};

QTEST_MAIN(ProgressiveEdgeStretchRenderPathTest)
#include "test_progressive_edge_stretch_render_path.moc"
