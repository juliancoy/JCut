#include <QtTest/QtTest>

#include <QImage>
#include <QDir>
#include <QHash>
#include <QJsonArray>
#include <QJsonObject>
#include <QProcess>
#include <QStandardPaths>
#include <QTemporaryDir>

#include "../render.h"
#include "../render_runtime.h"

#include <cmath>
#include <atomic>
#include <thread>
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
    void linkedMaskLayersShareOneHardwareSourceImport();
    void ordinaryVideoAsyncExportIsStableOffGuiThread();
    void speechFilterFrameCrossfadeStaysInVulkanComposition();
    void imageSequenceExportPublishesGpuPreviewFrames();
    void previewLossCancellationResumeAndFinalProbeRemainBounded();
};

void TestVulkanPreviewOfflineExport::
    ordinaryVideoAsyncExportIsStableOffGuiThread()
{
    qputenv("JCUT_RENDER_BACKEND", "vulkan");

    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    const QString ffmpeg =
        QStandardPaths::findExecutable(QStringLiteral("ffmpeg"));
    QVERIFY2(!ffmpeg.isEmpty(), "ffmpeg is required");
    const QString sourcePath = directory.filePath(QStringLiteral("worker-source.mp4"));
    QProcess makeSource;
    makeSource.start(
        ffmpeg,
        {QStringLiteral("-v"), QStringLiteral("error"),
         QStringLiteral("-f"), QStringLiteral("lavfi"),
         QStringLiteral("-i"), QStringLiteral("testsrc2=size=160x90:rate=30"),
         QStringLiteral("-frames:v"), QStringLiteral("180"),
         QStringLiteral("-c:v"), QStringLiteral("h264_nvenc"),
         QStringLiteral("-pix_fmt"), QStringLiteral("yuv420p"),
         QStringLiteral("-y"), sourcePath});
    QVERIFY(makeSource.waitForFinished(30000));
    QVERIFY2(makeSource.exitCode() == 0,
             makeSource.readAllStandardError().constData());

    RenderRequest request;
    request.outputPath = directory.filePath(QStringLiteral("worker-output.mp4"));
    request.outputFormat = QStringLiteral("mp4");
    request.outputSize = QSize(160, 90);
    request.outputFps = 30.0;
    request.exportStartFrame = 0;
    request.exportEndFrame = 179;
    request.exportRanges = {
        ExportRangeSegment{0, 89},
        ExportRangeSegment{120, 179},
    };
    request.incrementalExport = true;
    request.incrementalChunkFrames = 60;
    request.gpuExportPreviewEnabled = false;

    TimelineClip clip;
    clip.id = QStringLiteral("worker-video");
    clip.filePath = sourcePath;
    clip.mediaType = ClipMediaType::Video;
    clip.clipRole = ClipRole::Media;
    clip.videoEnabled = true;
    clip.audioEnabled = false;
    clip.startFrame = 0;
    clip.durationFrames = 180;
    clip.sourceDurationFrames = 180;
    clip.sourceFps = 30.0;
    clip.sourceFrameSize = request.outputSize;
    request.clips = {clip};

    std::atomic_bool completed{false};
    std::atomic_bool cancel{false};
    int decodeUnavailableCount = 0;
    RenderResult result;
    std::thread renderThread([&]() {
        result = renderTimelineToFile(
            request,
            [&](const RenderProgress& progress) {
                decodeUnavailableCount = qMax(
                    decodeUnavailableCount,
                    progress.skippedClipReasonCounts
                        .value(QStringLiteral("decode_frame_unavailable"))
                        .toInt());
                return !cancel.load(std::memory_order_acquire);
            });
        completed.store(true, std::memory_order_release);
    });
    QElapsedTimer timeout;
    timeout.start();
    while (!completed.load(std::memory_order_acquire) && timeout.elapsed() < 60000) {
        QCoreApplication::processEvents(QEventLoop::AllEvents, 10);
        QThread::msleep(2);
    }
    const bool finishedWithinDeadline = completed.load(std::memory_order_acquire);
    if (!finishedWithinDeadline) {
        cancel.store(true, std::memory_order_release);
        while (!completed.load(std::memory_order_acquire)) {
            QCoreApplication::processEvents(QEventLoop::AllEvents, 10);
            QThread::msleep(2);
        }
    }
    renderThread.join();
    QVERIFY2(finishedWithinDeadline, "worker-thread export timed out");
    QVERIFY2(result.success, qPrintable(result.message));
    QVERIFY(result.usedGpu);
    QVERIFY(result.usedHardwareEncode);
    QCOMPARE(decodeUnavailableCount, 0);
}

void TestVulkanPreviewOfflineExport::
    linkedMaskLayersShareOneHardwareSourceImport()
{
    qputenv("JCUT_RENDER_BACKEND", "vulkan");

    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    const QString fixturePath =
        directory.filePath(QStringLiteral("shared-source.png"));
    QImage fixture(160, 90, QImage::Format_RGBA8888);
    fixture.fill(QColor(38, 174, 224));
    QVERIFY(fixture.save(fixturePath));

    const QString ffmpeg =
        QStandardPaths::findExecutable(QStringLiteral("ffmpeg"));
    QVERIFY2(!ffmpeg.isEmpty(), "ffmpeg is required");
    const QString sourcePath =
        directory.filePath(QStringLiteral("shared-source.mp4"));
    QProcess makeSource;
    makeSource.start(
        ffmpeg,
        {QStringLiteral("-v"), QStringLiteral("error"),
         QStringLiteral("-loop"), QStringLiteral("1"),
         QStringLiteral("-i"), fixturePath,
         QStringLiteral("-frames:v"), QStringLiteral("2"),
         QStringLiteral("-c:v"), QStringLiteral("h264_nvenc"),
         QStringLiteral("-pix_fmt"), QStringLiteral("yuv420p"),
         QStringLiteral("-y"), sourcePath});
    QVERIFY(makeSource.waitForFinished(30000));
    QVERIFY2(makeSource.exitCode() == 0,
             makeSource.readAllStandardError().constData());

    RenderRequest request;
    request.outputPath =
        directory.filePath(QStringLiteral("shared-source-output.mp4"));
    request.outputFormat = QStringLiteral("mp4");
    request.outputSize = QSize(160, 90);
    request.outputFps = 30.0;
    request.exportStartFrame = 0;
    request.exportEndFrame = 1;
    request.exportRanges = {ExportRangeSegment{0, 1}};
    request.incrementalExport = false;
    request.gpuExportPreviewEnabled = false;

    TimelineClip source;
    source.id = QStringLiteral("shared-source-owner");
    source.label = QStringLiteral("Shared source owner");
    source.filePath = sourcePath;
    source.mediaType = ClipMediaType::Video;
    source.clipRole = ClipRole::Media;
    source.videoEnabled = true;
    source.audioEnabled = false;
    source.hasAudio = false;
    source.startFrame = 0;
    source.durationFrames = 2;
    source.sourceDurationFrames = 2;
    source.sourceFps = 30.0;
    source.sourceFrameSize = request.outputSize;
    source.trackIndex = 0;
    request.clips.push_back(source);
    for (int index = 0; index < 3; ++index) {
        const QString maskDirectory = directory.filePath(
            QStringLiteral("shared-mask-%1_alpha").arg(index));
        QVERIFY(QDir().mkpath(maskDirectory));
        for (int frame = 1; frame <= 2; ++frame) {
            QImage mask(160, 90, QImage::Format_Grayscale8);
            mask.fill(255);
            QVERIFY(mask.save(
                QDir(maskDirectory).filePath(
                    QStringLiteral("frame_%1.png")
                        .arg(frame, 6, 10, QChar('0')))));
        }
        TimelineClip child = source;
        child.id = QStringLiteral("shared-mask-%1").arg(index);
        child.label = QStringLiteral("Shared mask %1").arg(index);
        child.clipRole = ClipRole::MaskMatte;
        child.linkedSourceClipId = source.id;
        child.maskEnabled = true;
        child.maskFramesDir = maskDirectory;
        child.trackIndex = index + 1;
        request.clips.push_back(child);
    }

    QJsonObject lastStageTable;
    const RenderResult result = renderTimelineToFile(
        request,
        [&](const RenderProgress& progress) {
            lastStageTable = progress.renderStageTable;
            return true;
        });
    QVERIFY2(result.success, qPrintable(result.message));
    QVERIFY2(result.usedGpu, qPrintable(result.message));
    QVERIFY2(result.usedHardwareEncode, qPrintable(result.message));

    QHash<QString, int> counts;
    for (const QJsonValue& value :
         lastStageTable.value(QStringLiteral("rows")).toArray()) {
        const QJsonObject row = value.toObject();
        counts.insert(
            row.value(QStringLiteral("id")).toString(),
            row.value(QStringLiteral("frames")).toInt());
    }
    QCOMPARE(
        counts.value(QStringLiteral(
            "__render_frame_hardware_source_import__")),
        2);
    QCOMPARE(
        counts.value(QStringLiteral(
            "__render_frame_hardware_source_reuse__")),
        6);
}

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
    speechFilterFrameCrossfadeStaysInVulkanComposition()
{
    qputenv("JCUT_RENDER_BACKEND", "vulkan");

    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    for (int frame = 0; frame <= 10; ++frame) {
        QImage image(160, 90, QImage::Format_RGBA8888);
        image.fill(frame >= 9
                       ? QColor(12, 20, 235)
                       : QColor(235, 20, 12));
        QVERIFY(image.save(
            directory.filePath(
                QStringLiteral("source_%1.png")
                    .arg(frame, 2, 10, QChar('0')))));
    }
    const QString ffmpeg =
        QStandardPaths::findExecutable(QStringLiteral("ffmpeg"));
    QVERIFY2(!ffmpeg.isEmpty(), "ffmpeg is required");
    const QString sourcePath =
        directory.filePath(QStringLiteral("crossfade-source.mkv"));
    QProcess makeSource;
    makeSource.start(
        ffmpeg,
        {QStringLiteral("-v"), QStringLiteral("error"),
         QStringLiteral("-framerate"), QStringLiteral("30"),
         QStringLiteral("-i"),
         directory.filePath(QStringLiteral("source_%02d.png")),
         QStringLiteral("-frames:v"), QStringLiteral("11"),
         QStringLiteral("-c:v"), QStringLiteral("ffv1"),
         sourcePath});
    QVERIFY(makeSource.waitForFinished(30000));
    QCOMPARE(makeSource.exitCode(), 0);

    RenderRequest request;
    request.outputPath =
        directory.filePath(QStringLiteral("crossfade-output.mp4"));
    request.outputFormat = QStringLiteral("mp4");
    request.outputSize = QSize(160, 90);
    request.outputFps = 30.0;
    request.exportStartFrame = 0;
    request.exportEndFrame = 10;
    request.exportRanges = {
        ExportRangeSegment{0, 0},
        ExportRangeSegment{10, 10}};
    request.playbackTiming.playbackRanges =
        request.exportRanges;
    request.playbackTiming.frameTransitionMode =
        PlaybackFrameTransitionMode::Crossfade;
    request.playbackTiming.frameCrossfadeEnabled = true;
    request.playbackTiming.frameCrossfadeFrames = 1;
    request.incrementalExport = false;

    TimelineClip clip;
    clip.id = QStringLiteral("crossfade-video");
    clip.label = QStringLiteral("Crossfade video");
    clip.filePath = sourcePath;
    clip.mediaType = ClipMediaType::Video;
    clip.videoEnabled = true;
    clip.audioEnabled = false;
    clip.hasAudio = false;
    clip.startFrame = 0;
    clip.durationFrames = 11;
    clip.sourceDurationFrames = 11;
    clip.sourceFps = 30.0;
    clip.sourceFrameSize = request.outputSize;
    clip.trackIndex = 0;
    request.clips = {clip};

    const RenderResult result =
        renderTimelineToFile(request, {});
    QVERIFY2(result.success, qPrintable(result.message));
    QVERIFY(result.usedGpu);
    QCOMPARE(result.framesRendered, int64_t{2});

    QProcess decode;
    decode.start(
        ffmpeg,
        {QStringLiteral("-v"), QStringLiteral("error"),
         QStringLiteral("-i"), request.outputPath,
         QStringLiteral("-frames:v"), QStringLiteral("2"),
         QStringLiteral("-f"), QStringLiteral("rawvideo"),
         QStringLiteral("-pix_fmt"), QStringLiteral("rgb24"),
         QStringLiteral("-")});
    QVERIFY(decode.waitForFinished(30000));
    QCOMPARE(decode.exitCode(), 0);
    const QByteArray pixels = decode.readAllStandardOutput();
    QCOMPARE(pixels.size(), 160 * 90 * 3 * 2);
    for (int frame = 0; frame < 2; ++frame) {
        const int offset =
            ((frame * 160 * 90) + (45 * 160 + 80)) * 3;
        const int red =
            static_cast<unsigned char>(pixels.at(offset));
        const int green =
            static_cast<unsigned char>(pixels.at(offset + 1));
        const int blue =
            static_cast<unsigned char>(pixels.at(offset + 2));
        QVERIFY2(red > 70 && blue > 70 && green < 70,
                 qPrintable(QStringLiteral(
                     "expected GPU-composited red/blue crossfade at output "
                     "frame %1, got rgb(%2,%3,%4)")
                                .arg(frame)
                                .arg(red)
                                .arg(green)
                                .arg(blue)));
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
    QCOMPARE(gpuPreviewFrames, 3);
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
    QCOMPARE(abandonedPreviewFrames, 3);

    const RenderResult resumed = renderTimelineToFile(
        request,
        [&](const RenderProgress& progress) {
            abandonPreviewFrame(progress);
            return true;
        });
    QVERIFY2(resumed.success, qPrintable(resumed.message));
    QVERIFY(resumed.usedGpu);
    QVERIFY(resumed.usedHardwareEncode);
    QVERIFY(resumed.cudaExternalTransfer);
    QCOMPARE(resumed.exportPipeline,
             QStringLiteral(
                 "vulkan_cuda_external_memory_nv12_nvenc"));
    QCOMPARE(resumed.incrementalFramesReused, int64_t{65});
    QCOMPARE(resumed.framesRendered, int64_t{130});
    QCOMPARE(abandonedPreviewFrames, 6);

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
