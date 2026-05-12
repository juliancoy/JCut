#include <QtTest/QtTest>

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QGuiApplication>
#include <QHash>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QPainter>

#include "../editor_shared.h"
#include "../render_internal.h"

namespace {

QImage makeSourceImage(const QSize& size)
{
    QImage image(size, QImage::Format_RGBA8888);
    for (int y = 0; y < size.height(); ++y) {
        uchar* row = image.scanLine(y);
        for (int x = 0; x < size.width(); ++x) {
            const int idx = x * 4;
            row[idx + 0] = static_cast<uchar>((x * 9 + y * 3) & 0xff);
            row[idx + 1] = static_cast<uchar>((x * 5 + y * 11) & 0xff);
            row[idx + 2] = static_cast<uchar>(((x ^ y) * 13) & 0xff);
            row[idx + 3] = 255;
        }
    }
    QPainter painter(&image);
    painter.setPen(Qt::NoPen);
    painter.setBrush(QColor(245, 180, 25, 255));
    painter.drawRect(QRect(4, 5, 17, 13));
    painter.setBrush(QColor(20, 225, 160, 255));
    painter.drawEllipse(QRect(28, 18, 19, 21));
    painter.end();
    return image;
}

TimelineClip makeImageClip(const QString& imagePath, const QString& id)
{
    TimelineClip clip;
    clip.id = id;
    clip.label = id;
    clip.filePath = imagePath;
    clip.useProxy = false;
    clip.mediaType = ClipMediaType::Image;
    clip.sourceKind = MediaSourceKind::File;
    clip.videoEnabled = true;
    clip.audioEnabled = false;
    clip.hasAudio = false;
    clip.sourceFps = 30.0;
    clip.sourceDurationFrames = 12;
    clip.sourceInFrame = 0;
    clip.startFrame = 0;
    clip.durationFrames = 12;
    clip.trackIndex = 0;
    clip.opacity = 1.0;
    return clip;
}

TimelineClip::GradingKeyframe makeParityGrade()
{
    TimelineClip::GradingKeyframe grade;
    grade.frame = 0;
    grade.brightness = 0.035;
    grade.contrast = 1.08;
    grade.saturation = 1.12;
    grade.opacity = 1.0;
    grade.shadowsR = 0.035;
    grade.shadowsG = -0.025;
    grade.shadowsB = 0.015;
    grade.midtonesR = 0.04;
    grade.midtonesG = 0.015;
    grade.midtonesB = -0.02;
    grade.highlightsR = 0.02;
    grade.highlightsG = 0.035;
    grade.highlightsB = -0.015;
    grade.curveSmoothingEnabled = false;
    grade.curvePointsR = QVector<QPointF>{{0.0, 0.0}, {0.45, 0.40}, {1.0, 1.0}};
    grade.curvePointsG = QVector<QPointF>{{0.0, 0.0}, {0.50, 0.54}, {1.0, 1.0}};
    grade.curvePointsB = QVector<QPointF>{{0.0, 0.0}, {0.55, 0.50}, {1.0, 1.0}};
    grade.curvePointsLuma = QVector<QPointF>{{0.0, 0.0}, {0.25, 0.22}, {0.75, 0.80}, {1.0, 1.0}};
    return grade;
}

RenderRequest makeRequest(const TimelineClip& clip, const QSize& outputSize, const QString& projectDir)
{
    RenderRequest request;
    request.outputPath = QDir(projectDir).filePath(QStringLiteral("parity.mov"));
    request.outputFormat = QStringLiteral("preview");
    request.outputSize = outputSize;
    request.useProxyMedia = false;
    request.bypassGrading = false;
    request.correctionsEnabled = false;
    request.clips = QVector<TimelineClip>{clip};
    request.tracks = QVector<TimelineTrack>{TimelineTrack{QStringLiteral("V1")}};
    request.exportStartFrame = 0;
    request.exportEndFrame = 1;
    return request;
}

bool writeProjectState(const QString& path, const TimelineClip& clip, const QSize& outputSize)
{
    QJsonObject clipJson;
    clipJson[QStringLiteral("id")] = clip.id;
    clipJson[QStringLiteral("label")] = clip.label;
    clipJson[QStringLiteral("filePath")] = clip.filePath;
    clipJson[QStringLiteral("mediaType")] = QStringLiteral("image");
    clipJson[QStringLiteral("startFrame")] = QString::number(clip.startFrame);
    clipJson[QStringLiteral("durationFrames")] = QString::number(clip.durationFrames);
    clipJson[QStringLiteral("trackIndex")] = clip.trackIndex;

    QJsonObject root;
    root[QStringLiteral("schema")] = QStringLiteral("jcut-render-parity-test-v1");
    root[QStringLiteral("outputWidth")] = outputSize.width();
    root[QStringLiteral("outputHeight")] = outputSize.height();
    root[QStringLiteral("clips")] = QJsonArray{clipJson};

    QFile file(path);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        return false;
    }
    const QByteArray payload = QJsonDocument(root).toJson(QJsonDocument::Indented);
    return file.write(payload) == payload.size();
}

struct DiffStats {
    int differingPixels = 0;
    int maxDelta = 0;
    double meanDelta = 0.0;
};

QImage canonicalImage(const QImage& image)
{
    return image.convertToFormat(QImage::Format_RGBA8888);
}

DiffStats compareImages(const QImage& a, const QImage& b, QImage* diff)
{
    DiffStats stats;
    if (diff) {
        *diff = QImage(a.size(), QImage::Format_RGBA8888);
        diff->fill(Qt::black);
    }
    qint64 sumDelta = 0;
    const int channels = a.width() * a.height() * 4;
    for (int y = 0; y < a.height(); ++y) {
        const uchar* ar = a.constScanLine(y);
        const uchar* br = b.constScanLine(y);
        uchar* dr = diff ? diff->scanLine(y) : nullptr;
        for (int x = 0; x < a.width(); ++x) {
            int pixelMax = 0;
            int pixelSum = 0;
            for (int c = 0; c < 4; ++c) {
                const int delta = qAbs(int(ar[x * 4 + c]) - int(br[x * 4 + c]));
                pixelMax = qMax(pixelMax, delta);
                pixelSum += delta;
            }
            if (pixelMax > 0) {
                ++stats.differingPixels;
            }
            stats.maxDelta = qMax(stats.maxDelta, pixelMax);
            sumDelta += pixelSum;
            if (dr) {
                dr[x * 4 + 0] = static_cast<uchar>(qMin(255, pixelMax * 16));
                dr[x * 4 + 1] = static_cast<uchar>(qMin(255, pixelMax * 16));
                dr[x * 4 + 2] = static_cast<uchar>(qMin(255, pixelMax * 16));
                dr[x * 4 + 3] = 255;
            }
        }
    }
    stats.meanDelta = channels > 0 ? double(sumDelta) / double(channels) : 0.0;
    return stats;
}

struct RenderedPair {
    QImage opengl;
    QImage vulkan;
    qint64 openglDecodeMs = 0;
    qint64 openglTextureMs = 0;
    qint64 openglCompositeMs = 0;
    qint64 openglReadbackMs = 0;
    qint64 vulkanDecodeMs = 0;
    qint64 vulkanTextureMs = 0;
    qint64 vulkanCompositeMs = 0;
    qint64 vulkanReadbackMs = 0;
};

bool renderPair(const RenderRequest& request, RenderedPair* pair, QString* skipReason)
{
    if (!pair) {
        return false;
    }
    render_detail::OffscreenGpuRenderer opengl;
    render_detail::OffscreenVulkanRenderer vulkan;
    QString error;
    if (!opengl.initialize(request.outputSize, &error)) {
        if (skipReason) *skipReason = QStringLiteral("OpenGL offscreen unavailable: %1").arg(error);
        return false;
    }
    if (!vulkan.initialize(request.outputSize, &error)) {
        if (skipReason) *skipReason = QStringLiteral("Vulkan offscreen unavailable: %1").arg(error);
        return false;
    }

    QVector<TimelineClip> orderedClips = render_detail::sortedVisualClips(request.clips, request.tracks);
    QHash<QString, editor::DecoderContext*> openglDecoders;
    QHash<QString, editor::DecoderContext*> vulkanDecoders;
    QHash<render_detail::RenderAsyncFrameKey, editor::FrameHandle> openglAsyncCache;
    QHash<render_detail::RenderAsyncFrameKey, editor::FrameHandle> vulkanAsyncCache;
    auto cleanup = qScopeGuard([&] {
        qDeleteAll(openglDecoders);
        qDeleteAll(vulkanDecoders);
    });

    pair->opengl = opengl.renderFrame(request,
                                     0,
                                     openglDecoders,
                                     nullptr,
                                     &openglAsyncCache,
                                     orderedClips,
                                     nullptr,
                                     &pair->openglDecodeMs,
                                     &pair->openglTextureMs,
                                     &pair->openglCompositeMs,
                                     &pair->openglReadbackMs,
                                     nullptr,
                                     nullptr);
    pair->vulkan = vulkan.renderFrame(request,
                                     0,
                                     vulkanDecoders,
                                     nullptr,
                                     &vulkanAsyncCache,
                                     orderedClips,
                                     nullptr,
                                     &pair->vulkanDecodeMs,
                                     &pair->vulkanTextureMs,
                                     &pair->vulkanCompositeMs,
                                     &pair->vulkanReadbackMs,
                                     nullptr,
                                     nullptr);
    return true;
}

} // namespace

class TestOpenGLVulkanRenderExactness : public QObject {
    Q_OBJECT

private slots:
    void testOffscreenOpenGLAndVulkanRenderParity();
};

void TestOpenGLVulkanRenderExactness::testOffscreenOpenGLAndVulkanRenderParity()
{
    const QSize outputSize(64, 64);
    QDir artifactRoot(QCoreApplication::applicationDirPath());
    QVERIFY2(artifactRoot.mkpath(QStringLiteral("opengl_vulkan_render_exactness_project")),
             "Failed to create render parity artifact directory");
    artifactRoot.cd(QStringLiteral("opengl_vulkan_render_exactness_project"));
    const QString sourcePath = artifactRoot.filePath(QStringLiteral("source.png"));
    QVERIFY2(makeSourceImage(outputSize).save(sourcePath), "Failed to write deterministic source image");

    TimelineClip clip = makeImageClip(sourcePath, QStringLiteral("identity_clip"));
    QVERIFY2(writeProjectState(artifactRoot.filePath(QStringLiteral("state.json")), clip, outputSize),
             "Failed to write test project state.json");

    RenderRequest request = makeRequest(clip, outputSize, artifactRoot.absolutePath());
    RenderedPair identity;
    QString skipReason;
    if (!renderPair(request, &identity, &skipReason)) {
        QSKIP(qPrintable(skipReason));
    }
    QVERIFY2(!identity.opengl.isNull(), "OpenGL renderer returned a null identity frame");
    QVERIFY2(!identity.vulkan.isNull(), "Vulkan renderer returned a null identity frame");
    QCOMPARE(identity.opengl.size(), outputSize);
    QCOMPARE(identity.vulkan.size(), outputSize);

    QImage glIdentity = canonicalImage(identity.opengl);
    QImage vkIdentity = canonicalImage(identity.vulkan);
    QImage identityDiff;
    const DiffStats identityStats = compareImages(glIdentity, vkIdentity, &identityDiff);
    glIdentity.save(artifactRoot.filePath(QStringLiteral("identity_opengl.png")));
    vkIdentity.save(artifactRoot.filePath(QStringLiteral("identity_vulkan.png")));
    identityDiff.save(artifactRoot.filePath(QStringLiteral("identity_diff.png")));
    QVERIFY2(identityStats.differingPixels == 0,
             qPrintable(QStringLiteral("Identity render mismatch: differingPixels=%1 maxDelta=%2 meanDelta=%3 artifacts=%4")
                            .arg(identityStats.differingPixels)
                            .arg(identityStats.maxDelta)
                            .arg(identityStats.meanDelta, 0, 'f', 6)
                            .arg(artifactRoot.absolutePath())));

    clip.id = QStringLiteral("graded_clip");
    clip.label = QStringLiteral("graded_clip");
    clip.gradingKeyframes = QVector<TimelineClip::GradingKeyframe>{makeParityGrade()};
    QVERIFY2(writeProjectState(artifactRoot.filePath(QStringLiteral("graded_state.json")), clip, outputSize),
             "Failed to write graded test project state.json");
    request = makeRequest(clip, outputSize, artifactRoot.absolutePath());
    RenderedPair graded;
    if (!renderPair(request, &graded, &skipReason)) {
        QSKIP(qPrintable(skipReason));
    }
    QVERIFY2(!graded.opengl.isNull(), "OpenGL renderer returned a null graded frame");
    QVERIFY2(!graded.vulkan.isNull(), "Vulkan renderer returned a null graded frame");

    QImage glGraded = canonicalImage(graded.opengl);
    QImage vkGraded = canonicalImage(graded.vulkan);
    QImage gradedDiff;
    const DiffStats gradedStats = compareImages(glGraded, vkGraded, &gradedDiff);
    glGraded.save(artifactRoot.filePath(QStringLiteral("graded_opengl.png")));
    vkGraded.save(artifactRoot.filePath(QStringLiteral("graded_vulkan.png")));
    gradedDiff.save(artifactRoot.filePath(QStringLiteral("graded_diff.png")));
    QVERIFY2(gradedStats.maxDelta <= 1,
             qPrintable(QStringLiteral("Graded render mismatch beyond one LSB: differingPixels=%1 maxDelta=%2 meanDelta=%3 artifacts=%4")
                            .arg(gradedStats.differingPixels)
                            .arg(gradedStats.maxDelta)
                            .arg(gradedStats.meanDelta, 0, 'f', 6)
                            .arg(artifactRoot.absolutePath())));
}

QTEST_MAIN(TestOpenGLVulkanRenderExactness)
#include "test_opengl_vulkan_render_exactness.moc"
