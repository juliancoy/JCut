#include <QtTest/QtTest>

#include <QImage>
#include <QProcess>
#include <QStandardPaths>
#include <QTemporaryDir>

#include "../render.h"
#include "../render_runtime.h"

#include <cmath>
#include <unistd.h>

namespace {

bool writePreviewFixture(const QString& path)
{
    QImage image(160, 90, QImage::Format_RGBA8888);
    for (int y = 0; y < image.height(); ++y) {
        for (int x = 0; x < image.width(); ++x) {
            image.setPixelColor(
                x, y,
                x < image.width() / 2
                    ? QColor(230, 28 + y / 3, 24, 255)
                    : QColor(20, 45 + y / 4, 230, 255));
        }
    }
    return image.save(path);
}

} // namespace

class TestVulkanPreviewOfflineExport final : public QObject {
    Q_OBJECT

private slots:
    void exportsPreviewCompositionHeadlesslyWithoutPreviewConsumer();
    void imageSequenceExportPublishesGpuPreviewFrames();
    void previewLossCancellationResumeAndFinalProbeRemainBounded();
};

void TestVulkanPreviewOfflineExport::
    exportsPreviewCompositionHeadlesslyWithoutPreviewConsumer()
{
    qputenv("JCUT_RENDER_BACKEND", "vulkan");

    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    const QString fixturePath =
        directory.filePath(QStringLiteral("preview-fixture.png"));
    QVERIFY(writePreviewFixture(fixturePath));

    RenderRequest request;
    request.outputPath =
        directory.filePath(QStringLiteral("offline-preview.mp4"));
    request.outputFormat = QStringLiteral("mp4");
    request.outputSize = QSize(160, 90);
    request.outputFps = 30.0;
    request.exportStartFrame = 0;
    request.exportEndFrame = 64;
    request.exportRanges = {ExportRangeSegment{0, 64}};
    request.incrementalExport = false;
    request.gpuExportPreviewEnabled = false;

    TimelineClip clip;
    clip.id = QStringLiteral("offline-preview-image");
    clip.label = QStringLiteral("Offline preview fixture");
    clip.filePath = fixturePath;
    clip.mediaType = ClipMediaType::Image;
    clip.videoEnabled = true;
    clip.audioEnabled = false;
    clip.hasAudio = false;
    clip.startFrame = 0;
    clip.durationFrames = 65;
    clip.sourceDurationFrames = 65;
    clip.sourceFps = 30.0;
    clip.sourceFrameSize = request.outputSize;
    clip.trackIndex = 0;
    request.clips = {clip};

    jcut::render::RenderRequestCore previewRequest;
    previewRequest.outputPath = "test://headless-preview";
    previewRequest.outputFormat = "preview";
    previewRequest.outputSize = {160, 90};
    previewRequest.outputFps = 30.0;
    previewRequest.exportStartFrame = 0;
    previewRequest.exportEndFrame = 0;
    jcut::render::TimelineRenderData previewTimeline;
    previewTimeline.clips = {clip};
    TimelineTrack previewTrack;
    previewTrack.name = QStringLiteral("Preview");
    previewTimeline.tracks = {previewTrack};
    previewTimeline.exportRanges = {
        ExportRangeSegment{0, 0},
    };
    const jcut::render::PreviewFrameResultCore preview =
        jcut::render::renderPreviewFrameCore(
            previewRequest,
            previewTimeline,
            0,
            true,
            true);
    QVERIFY2(preview.success, preview.message.c_str());
    QVERIFY(preview.usedGpu);
    QCOMPARE(preview.effectiveRenderBackend, std::string("vulkan"));
    QCOMPARE(preview.image.size.width, 160);
    QCOMPARE(preview.image.size.height, 90);

    int progressUpdates = 0;
    int borrowedPreviewFrames = 0;
    const RenderResult result = renderTimelineToFile(
        request,
        [&](const RenderProgress& progress) {
            ++progressUpdates;
            if (progress.gpuPreviewFrame.valid) {
                ++borrowedPreviewFrames;
            }
            return true;
        });

    QVERIFY2(result.success, qPrintable(result.message));
    QVERIFY2(result.usedGpu, qPrintable(result.message));
    QVERIFY2(result.usedHardwareEncode, qPrintable(result.message));
    QVERIFY2(result.cudaExternalTransfer, qPrintable(result.message));
    QCOMPARE(result.exportPipeline,
             QStringLiteral(
                 "vulkan_cuda_external_memory_nv12_nvenc"));
    QCOMPARE(result.framesRendered, int64_t{65});
    QVERIFY(progressUpdates > 0);
    QCOMPARE(borrowedPreviewFrames, 0);

    const QString ffprobe =
        QStandardPaths::findExecutable(QStringLiteral("ffprobe"));
    QVERIFY2(!ffprobe.isEmpty(), "ffprobe is required");
    QProcess probe;
    probe.start(
        ffprobe,
        {QStringLiteral("-v"), QStringLiteral("error"),
         QStringLiteral("-count_frames"),
         QStringLiteral("-select_streams"), QStringLiteral("v:0"),
         QStringLiteral("-show_entries"),
         QStringLiteral("stream=nb_read_frames"),
         QStringLiteral("-of"), QStringLiteral("default=nw=1:nk=1"),
         request.outputPath});
    QVERIFY(probe.waitForFinished(30000));
    QCOMPARE(probe.exitCode(), 0);
    QCOMPARE(QString::fromUtf8(probe.readAllStandardOutput()).trimmed(),
             QStringLiteral("65"));

    const QString ffmpeg =
        QStandardPaths::findExecutable(QStringLiteral("ffmpeg"));
    QVERIFY2(!ffmpeg.isEmpty(), "ffmpeg is required");
    QProcess decode;
    decode.start(
        ffmpeg,
        {QStringLiteral("-v"), QStringLiteral("error"),
         QStringLiteral("-i"), request.outputPath,
         QStringLiteral("-frames:v"), QStringLiteral("1"),
         QStringLiteral("-f"), QStringLiteral("rawvideo"),
         QStringLiteral("-pix_fmt"), QStringLiteral("rgb24"),
         QStringLiteral("-")});
    QVERIFY(decode.waitForFinished(30000));
    QCOMPARE(decode.exitCode(), 0);
    const QByteArray pixels = decode.readAllStandardOutput();
    QCOMPARE(pixels.size(), 160 * 90 * 3);
    const auto sample = [&pixels](int x, int y, int channel) {
        return static_cast<unsigned char>(
            pixels.at(((y * 160 + x) * 3) + channel));
    };
    const auto previewSample = [&preview](int x, int y, int channel) {
        return preview.image.bytes.at(
            static_cast<std::size_t>(
                y * preview.image.strideBytes + x * 4 + channel));
    };
    QVERIFY(sample(30, 45, 0) > sample(30, 45, 2) + 100);
    QVERIFY(sample(130, 45, 2) > sample(130, 45, 0) + 100);
    for (const int x : {30, 130}) {
        for (int channel = 0; channel < 3; ++channel) {
            QVERIFY2(
                std::abs(static_cast<int>(sample(x, 45, channel)) -
                         static_cast<int>(
                             previewSample(x, 45, channel))) <= 20,
                "decoded export diverged from the shared headless preview "
                "compositor");
        }
    }
}

void TestVulkanPreviewOfflineExport::
    imageSequenceExportPublishesGpuPreviewFrames()
{
    qputenv("JCUT_RENDER_BACKEND", "vulkan");

    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    const QString fixturePath =
        directory.filePath(QStringLiteral("image-sequence-fixture.png"));
    QVERIFY(writePreviewFixture(fixturePath));

    RenderRequest request;
    request.outputPath =
        directory.filePath(QStringLiteral("image-sequence-preview.mp4"));
    request.outputFormat = QStringLiteral("mp4");
    request.outputSize = QSize(160, 90);
    request.outputFps = 30.0;
    request.exportStartFrame = 0;
    request.exportEndFrame = 30;
    request.exportRanges = {ExportRangeSegment{0, 30}};
    request.incrementalExport = false;
    request.createVideoFromImageSequence = true;
    request.imageSequenceFormat = QStringLiteral("jpg");
    request.gpuExportPreviewEnabled = true;

    TimelineClip clip;
    clip.id = QStringLiteral("image-sequence-preview-image");
    clip.label = QStringLiteral("Image sequence preview fixture");
    clip.filePath = fixturePath;
    clip.mediaType = ClipMediaType::Image;
    clip.videoEnabled = true;
    clip.audioEnabled = false;
    clip.hasAudio = false;
    clip.startFrame = 0;
    clip.durationFrames = 31;
    clip.sourceDurationFrames = 31;
    clip.sourceFps = 30.0;
    clip.sourceFrameSize = request.outputSize;
    clip.trackIndex = 0;
    request.clips = {clip};

    int gpuPreviewFrames = 0;
    const RenderResult result = renderTimelineToFile(
        request,
        [&gpuPreviewFrames](const RenderProgress& progress) {
            if (!progress.gpuPreviewFrame.valid) {
                return true;
            }
            ++gpuPreviewFrames;
            if (progress.gpuPreviewFrame.readySemaphoreFd >= 0) {
                ::close(progress.gpuPreviewFrame.readySemaphoreFd);
            }
            if (progress.gpuPreviewFrame.consumedSemaphoreFd >= 0) {
                ::close(progress.gpuPreviewFrame.consumedSemaphoreFd);
            }
            return true;
        });

    QVERIFY2(result.success, qPrintable(result.message));
    QVERIFY(result.usedGpu);
    QCOMPARE(result.framesRendered, int64_t{31});
    QCOMPARE(gpuPreviewFrames, 2);
}

void TestVulkanPreviewOfflineExport::
    previewLossCancellationResumeAndFinalProbeRemainBounded()
{
    qputenv("JCUT_RENDER_BACKEND", "vulkan");

    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    const QString fixturePath =
        directory.filePath(QStringLiteral("lifecycle-fixture.png"));
    QVERIFY(writePreviewFixture(fixturePath));

    RenderRequest request;
    request.outputPath =
        directory.filePath(QStringLiteral("lifecycle.mp4"));
    request.outputFormat = QStringLiteral("mp4");
    request.outputSize = QSize(160, 90);
    request.outputFps = 30.0;
    request.exportStartFrame = 0;
    request.exportEndFrame = 129;
    request.exportRanges = {
        ExportRangeSegment{0, 64},
        ExportRangeSegment{65, 129},
    };
    request.incrementalExport = true;
    request.incrementalChunkFrames = 65;
    request.gpuExportPreviewEnabled = true;

    TimelineClip clip;
    clip.id = QStringLiteral("preview-lifecycle-image");
    clip.label = QStringLiteral("Preview lifecycle fixture");
    clip.filePath = fixturePath;
    clip.mediaType = ClipMediaType::Image;
    clip.videoEnabled = true;
    clip.audioEnabled = false;
    clip.hasAudio = false;
    clip.startFrame = 0;
    clip.durationFrames = 130;
    clip.sourceDurationFrames = 130;
    clip.sourceFps = 30.0;
    clip.sourceFrameSize = request.outputSize;
    clip.trackIndex = 0;
    request.clips = {clip};

    int abandonedPreviewFrames = 0;
    const auto abandonPreviewFrame =
        [&abandonedPreviewFrames](const RenderProgress& progress) {
            if (!progress.gpuPreviewFrame.valid) {
                return;
            }
            ++abandonedPreviewFrames;
            if (progress.gpuPreviewFrame.readySemaphoreFd >= 0) {
                ::close(progress.gpuPreviewFrame.readySemaphoreFd);
            }
            if (progress.gpuPreviewFrame.consumedSemaphoreFd >= 0) {
                ::close(progress.gpuPreviewFrame.consumedSemaphoreFd);
            }
            // Deliberately do not acknowledge consumption. This models a
            // minimized/closed presentation surface after publication.
        };

    const RenderResult interrupted = renderTimelineToFile(
        request,
        [&](const RenderProgress& progress) {
            abandonPreviewFrame(progress);
            return progress.incrementalChunksCompleted < 1;
        });
    QVERIFY2(interrupted.cancelled, qPrintable(interrupted.message));
    QCOMPARE(interrupted.incrementalChunksCompleted, 1);
    QCOMPARE(interrupted.framesRendered, int64_t{65});
    QCOMPARE(abandonedPreviewFrames, 2);

    const RenderResult resumed = renderTimelineToFile(
        request,
        [&](const RenderProgress& progress) {
            abandonPreviewFrame(progress);
            return true;
        });
    QVERIFY2(resumed.success, qPrintable(resumed.message));
    QVERIFY(resumed.usedGpu);
    QVERIFY(resumed.usedHardwareEncode);
    QVERIFY(!resumed.cudaExternalTransfer);
    QCOMPARE(resumed.exportPipeline,
             QStringLiteral("gpu_render_cpu_transfer_encode"));
    QCOMPARE(resumed.incrementalFramesReused, int64_t{65});
    QCOMPARE(resumed.framesRendered, int64_t{130});
    QCOMPARE(abandonedPreviewFrames, 4);

    const QString ffprobe =
        QStandardPaths::findExecutable(QStringLiteral("ffprobe"));
    QVERIFY2(!ffprobe.isEmpty(), "ffprobe is required");
    QProcess probe;
    probe.start(
        ffprobe,
        {QStringLiteral("-v"), QStringLiteral("error"),
         QStringLiteral("-count_frames"),
         QStringLiteral("-select_streams"), QStringLiteral("v:0"),
         QStringLiteral("-show_entries"),
         QStringLiteral("stream=nb_read_frames"),
         QStringLiteral("-of"), QStringLiteral("default=nw=1:nk=1"),
         request.outputPath});
    QVERIFY(probe.waitForFinished(30000));
    QCOMPARE(probe.exitCode(), 0);
    QCOMPARE(QString::fromUtf8(probe.readAllStandardOutput()).trimmed(),
             QStringLiteral("130"));
}

QTEST_GUILESS_MAIN(TestVulkanPreviewOfflineExport)
#include "test_vulkan_preview_offline_export.moc"
