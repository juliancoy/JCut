#include "render_internal.h"
#include "clip_serialization.h"
#include "cpu_overlay_render_backend.h"
#include "export_timing.h"
#include "render_backend.h"

#include <QCoreApplication>
#include <QCryptographicHash>
#include <QDateTime>
#include <QDebug>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QImageWriter>
#include <QPainter>
#include <QProcess>
#include <QSaveFile>
#include <QSet>
#include <QStandardPaths>

#include <cmath>
#include <cstdio>
#include <limits>
#include <memory>
#include <optional>
using namespace render_detail;

namespace {

QString normalizedImageSequenceFormat(const QString& requested)
{
    const QString format = requested.trimmed().toLower();
    if (format == QStringLiteral("jpg") ||
        format == QStringLiteral("jpeg")) {
        return QStringLiteral("jpg");
    }
    if (format == QStringLiteral("webp")) {
        return QStringLiteral("webp");
    }
    if (format == QStringLiteral("png")) {
        return QStringLiteral("png");
    }
    return QStringLiteral("jpg");
}

bool saveImageAtomically(const QImage& image,
                         const QString& path,
                         const QString& format,
                         QString* errorMessage)
{
    QSaveFile file(path);
    if (!file.open(QIODevice::WriteOnly)) {
        if (errorMessage) {
            *errorMessage = file.errorString();
        }
        return false;
    }
    QImageWriter writer(&file, format.toLatin1());
    if (format != QStringLiteral("png")) {
        writer.setQuality(90);
    }
    if (!writer.write(image)) {
        if (errorMessage) {
            *errorMessage = writer.errorString();
        }
        file.cancelWriting();
        return false;
    }
    if (!file.commit()) {
        if (errorMessage) {
            *errorMessage = file.errorString();
        }
        return false;
    }
    return true;
}

inline int clampByte(int value) {
    return value < 0 ? 0 : (value > 255 ? 255 : value);
}

bool exportNeedsAsyncDecode(const QVector<TimelineClip>& orderedClips) {
    for (const TimelineClip& clip : orderedClips) {
        const QString decodePath = playbackMediaPathForClip(clip);
        if (!decodePath.isEmpty() && isImageSequencePath(decodePath)) {
            return true;
        }
    }
    return false;
}

bool fillNv12FrameFromRenderedImage(const QImage& image, AVFrame* frame) {
    if (!frame || frame->format != AV_PIX_FMT_NV12 || image.isNull()) {
        return false;
    }

    QImage argb = image;
    if (argb.format() != QImage::Format_ARGB32 && argb.format() != QImage::Format_ARGB32_Premultiplied) {
        argb = image.convertToFormat(QImage::Format_ARGB32);
    }
    if (argb.isNull()) {
        return false;
    }

    const int width = qMin(argb.width(), frame->width);
    const int height = qMin(argb.height(), frame->height);
    uint8_t* yPlane = frame->data[0];
    uint8_t* uvPlane = frame->data[1];
    const int yStride = frame->linesize[0];
    const int uvStride = frame->linesize[1];

    for (int y = 0; y < height; ++y) {
        const uint8_t* src = argb.constScanLine(y);
        uint8_t* dstY = yPlane + y * yStride;
        for (int x = 0; x < width; ++x) {
            const int b = src[x * 4 + 0];
            const int g = src[x * 4 + 1];
            const int r = src[x * 4 + 2];
            const int yValue = ((66 * r + 129 * g + 25 * b + 128) >> 8) + 16;
            dstY[x] = static_cast<uint8_t>(clampByte(yValue));
        }
    }

    for (int y = 0; y < height; y += 2) {
        const uint8_t* src0 = argb.constScanLine(y);
        const uint8_t* src1 = argb.constScanLine(qMin(y + 1, height - 1));
        uint8_t* dstUV = uvPlane + (y / 2) * uvStride;
        for (int x = 0; x < width; x += 2) {
            int sumU = 0;
            int sumV = 0;
            int sampleCount = 0;
            for (int dy = 0; dy < 2; ++dy) {
                const uint8_t* src = (dy == 0) ? src0 : src1;
                for (int dx = 0; dx < 2; ++dx) {
                    const int xx = qMin(x + dx, width - 1);
                    const int b = src[xx * 4 + 0];
                    const int g = src[xx * 4 + 1];
                    const int r = src[xx * 4 + 2];
                    const int uValue = ((-38 * r - 74 * g + 112 * b + 128) >> 8) + 128;
                    const int vValue = ((112 * r - 94 * g - 18 * b + 128) >> 8) + 128;
                    sumU += uValue;
                    sumV += vValue;
                    ++sampleCount;
                }
            }
            dstUV[x + 0] = static_cast<uint8_t>(clampByte(sumU / qMax(1, sampleCount)));
            if (x + 1 < uvStride) {
                dstUV[x + 1] = static_cast<uint8_t>(clampByte(sumV / qMax(1, sampleCount)));
            }
        }
    }

    return true;
}

struct ExportPathDecision {
    bool gpuNv12Conversion = false;
    bool gpuYuv420pConversion = false;
    bool cudaExternalTransfer = false;
    bool cudaExternalMemorySupported = false;
    QString pipeline;
    QString gpuTransferLabel;
    QString fallbackReason;
};

struct ScopedAvBufferRef {
    AVBufferRef* ref = nullptr;

    explicit ScopedAvBufferRef(AVBufferRef* value = nullptr) : ref(value) {}
    ~ScopedAvBufferRef() { av_buffer_unref(&ref); }

    ScopedAvBufferRef(const ScopedAvBufferRef&) = delete;
    ScopedAvBufferRef& operator=(const ScopedAvBufferRef&) = delete;

    AVBufferRef* get() const { return ref; }
};

ExportPathDecision chooseExportPath(bool useGpuRenderer,
                                    const render_detail::HeadlessVulkanCompositor* renderer,
                                    AVPixelFormat encoderInputPixFmt,
                                    bool cudaHardwareFrames,
                                    bool imageSequenceExport,
                                    bool externalTransferEnabled)
{
    ExportPathDecision decision;
    decision.cudaExternalMemorySupported =
        renderer && renderer->supportsCudaExternalMemoryInterop();
    decision.gpuNv12Conversion =
        useGpuRenderer && renderer && encoderInputPixFmt == AV_PIX_FMT_NV12 &&
        !imageSequenceExport;
    decision.gpuYuv420pConversion =
        useGpuRenderer && renderer && encoderInputPixFmt == AV_PIX_FMT_YUV420P &&
        !imageSequenceExport;

    if (decision.gpuNv12Conversion && cudaHardwareFrames &&
        externalTransferEnabled && renderer->supportsNv12CudaTransfer()) {
        decision.cudaExternalTransfer = true;
        decision.pipeline = QStringLiteral("vulkan_cuda_external_memory_nv12_nvenc");
        decision.gpuTransferLabel = QStringLiteral("Vulkan->CUDA device transfer");
        decision.fallbackReason = QStringLiteral("optimal path active");
        return decision;
    }

    if (decision.gpuNv12Conversion) {
        decision.pipeline = QStringLiteral("vulkan_gpu_nv12_cpu_frame_encode");
    } else if (decision.gpuYuv420pConversion) {
        decision.pipeline = QStringLiteral("vulkan_gpu_yuv420p_cpu_frame_encode");
    } else if (useGpuRenderer) {
        decision.pipeline = QStringLiteral("gpu_render_cpu_transfer_encode");
    } else {
        decision.pipeline = QStringLiteral("cpu_render_encode");
    }
    decision.gpuTransferLabel =
        useGpuRenderer ? QStringLiteral("GPU readback") : QStringLiteral("CPU path");

    if (!useGpuRenderer || !renderer) {
        decision.fallbackReason = QStringLiteral("GPU renderer unavailable");
    } else if (imageSequenceExport) {
        decision.fallbackReason = QStringLiteral("image sequence export requires CPU image output");
    } else if (encoderInputPixFmt != AV_PIX_FMT_NV12) {
        decision.fallbackReason =
            QStringLiteral("encoder software pixel format is %1, not nv12")
                .arg(QString::fromLatin1(av_get_pix_fmt_name(encoderInputPixFmt)));
    } else if (!cudaHardwareFrames) {
        decision.fallbackReason = QStringLiteral("encoder is not using CUDA hardware frames");
    } else if (!externalTransferEnabled) {
        decision.fallbackReason =
            QStringLiteral("JCUT_VULKAN_CUDA_EXTERNAL_TRANSFER disabled");
    } else if (!renderer->supportsNv12CudaTransfer()) {
        decision.fallbackReason =
            QStringLiteral("renderer cannot transfer NV12 frames to CUDA: %1")
                .arg(renderer->cudaExternalMemoryStatus());
    } else {
        decision.fallbackReason = QStringLiteral("direct transfer path not selected");
    }

    return decision;
}

bool configureCudaHardwareFrames(AVCodecContext* codecCtx,
                                 AVPixelFormat swFormat,
                                 AVBufferRef* sharedDeviceRef,
                                 QString* errorMessage)
{
    if (!codecCtx) {
        return false;
    }
    AVBufferRef* deviceRef = sharedDeviceRef ? av_buffer_ref(sharedDeviceRef) : nullptr;
    if (!deviceRef) {
        const int ret = av_hwdevice_ctx_create(&deviceRef, AV_HWDEVICE_TYPE_CUDA, nullptr, nullptr, 0);
        if (ret < 0) {
            if (errorMessage) {
                *errorMessage = render_detail::avErrToString(ret);
            }
            return false;
        }
    }

    AVBufferRef* framesRef = av_hwframe_ctx_alloc(deviceRef);
    if (!framesRef) {
        av_buffer_unref(&deviceRef);
        if (errorMessage) {
            *errorMessage = QStringLiteral("av_hwframe_ctx_alloc failed");
        }
        return false;
    }

    auto* frames = reinterpret_cast<AVHWFramesContext*>(framesRef->data);
    frames->format = AV_PIX_FMT_CUDA;
    frames->sw_format = swFormat;
    frames->width = codecCtx->width;
    frames->height = codecCtx->height;
    frames->initial_pool_size = 8;

    const int ret = av_hwframe_ctx_init(framesRef);
    if (ret < 0) {
        if (errorMessage) {
            *errorMessage = render_detail::avErrToString(ret);
        }
        av_buffer_unref(&framesRef);
        av_buffer_unref(&deviceRef);
        return false;
    }

    codecCtx->pix_fmt = AV_PIX_FMT_CUDA;
    codecCtx->hw_device_ctx = av_buffer_ref(deviceRef);
    codecCtx->hw_frames_ctx = av_buffer_ref(framesRef);
    av_buffer_unref(&framesRef);
    av_buffer_unref(&deviceRef);

    if (!codecCtx->hw_device_ctx || !codecCtx->hw_frames_ctx) {
        av_buffer_unref(&codecCtx->hw_device_ctx);
        av_buffer_unref(&codecCtx->hw_frames_ctx);
        if (errorMessage) {
            *errorMessage = QStringLiteral("Failed to retain CUDA hardware frame references.");
        }
        return false;
    }
    return true;
}

void configureVideoCodecContext(AVCodecContext* ctx,
                                const AVCodec* codec,
                                const RenderRequest& request,
                                AVPixelFormat pixelFormat,
                                AVFormatContext* formatCtx)
{
    const double outputFps = std::isfinite(request.outputFps) && request.outputFps > 0.001
        ? request.outputFps
        : static_cast<double>(kTimelineFps);
    const AVRational frameRate = av_d2q(outputFps, 1001000);
    ctx->codec_id = codec->id;
    ctx->codec_type = AVMEDIA_TYPE_VIDEO;
    ctx->width = request.outputSize.width();
    ctx->height = request.outputSize.height();
    ctx->time_base = av_inv_q(frameRate);
    ctx->framerate = frameRate;
    ctx->gop_size = qMax(1, static_cast<int>(std::lround(outputFps)));
    ctx->max_b_frames = 0;
    ctx->pix_fmt = pixelFormat;
    ctx->bit_rate = 8'000'000;

    if (formatCtx->oformat->flags & AVFMT_GLOBALHEADER) {
        ctx->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;
    }
}

} // namespace

static RenderResult renderTimelineSingleFile(
    const RenderRequest& request,
    const std::function<bool(const RenderProgress&)>& progressCallback,
    HeadlessVulkanCompositor* persistentRenderer = nullptr) {
    RenderResult result;
    const RenderBackend requestedBackend = desiredRenderBackendFromEnvironment();
    const qreal playbackSpeed = std::isfinite(request.playbackSpeed) && request.playbackSpeed > 0.001
        ? request.playbackSpeed
        : 1.0;
    result.requestedRenderBackend = renderBackendName(requestedBackend);
    result.effectiveRenderBackend = QStringLiteral("none");
    QElapsedTimer totalTimer;
    totalTimer.start();
    if (request.outputPath.isEmpty()) {
        result.message = QStringLiteral("No output path selected.");
        return result;
    }

    if (request.outputSize.width() <= 0 || request.outputSize.height() <= 0) {
        result.message = QStringLiteral("Invalid output size.");
        return result;
    }

    // Ensure the output directory exists
    const QFileInfo outputFileInfo(request.outputPath);
    const QDir outputDir = outputFileInfo.dir();
    if (!outputDir.exists() && !outputDir.mkpath(".")) {
        result.message = QStringLiteral("Failed to create output directory: %1").arg(outputDir.path());
        return result;
    }

    QString namedDirPath;
    if (request.createVideoFromImageSequence && !request.imageSequenceFormat.isEmpty()) {
        const QString baseName = outputFileInfo.completeBaseName();
        if (baseName.isEmpty()) {
            result.message = QStringLiteral("Image sequence export requires a named output file.");
            return result;
        }
        namedDirPath = outputDir.filePath(baseName + QStringLiteral("_frames"));
        QDir namedDir(namedDirPath);
        if (!namedDir.exists() && !namedDir.mkpath(".")) {
            result.message = QStringLiteral("Failed to create image sequence directory: %1").arg(namedDirPath);
            return result;
        }
        result.namedOutputDir = namedDirPath;
    }

    struct ScopedRenderDecodeSafety {
        bool preferHardware = false;
        editor::DecodePreference previousDecodePreference = editor::debugDecodePreference();
        editor::H26xSoftwareThreadingMode previousThreadingMode =
            editor::debugH26xSoftwareThreadingMode();
        bool previousDeterministic = editor::debugDeterministicPipelineEnabled();
        explicit ScopedRenderDecodeSafety(bool preferHardwareDecode)
            : preferHardware(preferHardwareDecode) {
            const bool disableVulkanHardwareDecode =
                preferHardware &&
                qEnvironmentVariableIntValue("JCUT_VULKAN_HW_DECODE_DISABLE") == 1;
            if (preferHardware && !disableVulkanHardwareDecode) {
                editor::setDebugDecodePreference(editor::DecodePreference::HardwareZeroCopy);
                editor::setDebugH26xSoftwareThreadingMode(
                    editor::H26xSoftwareThreadingMode::SingleThread);
            } else {
                editor::setDebugDecodePreference(editor::DecodePreference::Hardware);
                editor::setDebugH26xSoftwareThreadingMode(
                    editor::H26xSoftwareThreadingMode::SingleThread);
            }
        }
        ~ScopedRenderDecodeSafety() {
            editor::setDebugDecodePreference(previousDecodePreference);
            editor::setDebugH26xSoftwareThreadingMode(previousThreadingMode);
            editor::setDebugDeterministicPipelineEnabled(previousDeterministic);
        }
    } scopedDecodeSafety(requestedBackend == RenderBackend::Vulkan);

    QVector<ExportRangeSegment> exportRanges = request.exportRanges;
    if (exportRanges.isEmpty()) {
        const int64_t exportStart = qMax<int64_t>(0, request.exportStartFrame);
        const int64_t exportEnd = qMax(exportStart, request.exportEndFrame);
        exportRanges.push_back(ExportRangeSegment{exportStart, exportEnd});
    }
    std::sort(exportRanges.begin(), exportRanges.end(), [](const ExportRangeSegment& a, const ExportRangeSegment& b) {
        if (a.startFrame == b.startFrame) {
            return a.endFrame < b.endFrame;
        }
        return a.startFrame < b.startFrame;
    });
    const double outputFps = jcut::export_timing::normalizedOutputFps(request.outputFps);
    int64_t totalFramesToRender = 0;
    for (const ExportRangeSegment& range : exportRanges) {
        const int64_t exportStart = qMax<int64_t>(0, range.startFrame);
        const int64_t exportEnd = qMax(exportStart, range.endFrame);
        totalFramesToRender += jcut::export_timing::outputFrameCountForTimelineRange(
            exportStart,
            exportEnd,
            outputFps,
            playbackSpeed);
    }
    const QVector<TimelineClip> orderedClips = sortedVisualClips(request.clips, request.tracks);
    const QVector<TimelineClip> transcriptOverlayClips =
        sortedTranscriptOverlayClips(request.clips, request.tracks);
    QString gpuInitializationError;

    std::unique_ptr<HeadlessVulkanCompositor> ownedRenderer;
    HeadlessVulkanCompositor* activeRenderer = persistentRenderer;
    if (requestedBackend == RenderBackend::Vulkan) {
        if (!activeRenderer) {
            ownedRenderer = std::make_unique<OffscreenVulkanRenderer>();
            activeRenderer = ownedRenderer.get();
        }
        result.effectiveRenderBackend = QStringLiteral("vulkan");
    } else {
        result.message = QStringLiteral("Unsupported render backend. Vulkan is required.");
        return result;
    }

    qInfo().noquote()
        << QStringLiteral("[render-export-backend] begin app=\"%1\" pid=%2 requested=%3 env_JCUT_RENDER_BACKEND=\"%4\" "
                          "renderer_backend_id=\"%5\" output=\"%6\" size=%7x%8 fps=%9 speed=%10 ranges=%11 frames=%12")
               .arg(QCoreApplication::applicationFilePath())
               .arg(QCoreApplication::applicationPid())
               .arg(result.requestedRenderBackend,
                    qEnvironmentVariable("JCUT_RENDER_BACKEND"),
                    activeRenderer ? activeRenderer->backendId() : QStringLiteral("none"),
                    request.outputPath)
               .arg(request.outputSize.width())
               .arg(request.outputSize.height())
               .arg(outputFps, 0, 'f', 3)
               .arg(playbackSpeed, 0, 'f', 3)
               .arg(exportRanges.size())
               .arg(totalFramesToRender);
    const bool gpuInitialized =
        activeRenderer &&
        (persistentRenderer ||
         activeRenderer->initialize(
             request.outputSize, &gpuInitializationError));
    const bool useGpuRenderer = gpuInitialized;
    if (!useGpuRenderer) {
        qWarning().noquote()
            << QStringLiteral("[render-export-backend] init_failed app=\"%1\" pid=%2 requested=%3 renderer_backend_id=\"%4\" "
                              "output=\"%5\" size=%6x%7 fps=%8 speed=%9 message=\"%10\"")
                   .arg(QCoreApplication::applicationFilePath())
                   .arg(QCoreApplication::applicationPid())
                   .arg(result.requestedRenderBackend,
                        activeRenderer ? activeRenderer->backendId() : QStringLiteral("none"),
                        request.outputPath)
                   .arg(request.outputSize.width())
                   .arg(request.outputSize.height())
                   .arg(outputFps, 0, 'f', 3)
                   .arg(playbackSpeed, 0, 'f', 3)
                   .arg(gpuInitializationError);
        result.message = gpuInitializationError.trimmed().isEmpty()
            ? QStringLiteral("Vulkan export renderer initialization failed.")
            : gpuInitializationError.trimmed();
        return result;
    }
    if (useGpuRenderer) {
        result.effectiveRenderBackend = activeRenderer->backendId();
        qInfo().noquote()
            << QStringLiteral("[render-export-backend] init_ok effective=%1 cuda_external_memory=%2")
                   .arg(result.effectiveRenderBackend,
                        activeRenderer->supportsCudaExternalMemoryInterop()
                            ? QStringLiteral("yes")
                            : QStringLiteral("no"));
    }
    if (requestedBackend == RenderBackend::Vulkan &&
        qEnvironmentVariable("JCUT_VULKAN_CUDA_EXTERNAL_MEMORY_REQUIRED", QStringLiteral("0")) != QStringLiteral("0") &&
        (!activeRenderer || !activeRenderer->supportsCudaExternalMemoryInterop())) {
        result.message = QStringLiteral(
            "Vulkan/CUDA external-memory interop was required, but the selected Vulkan device "
            "does not expose the required external memory/semaphore FD capability.");
        return result;
    }
    result.usedGpu = useGpuRenderer;

    std::unique_ptr<editor::AsyncDecoder> asyncDecoder;
    if (exportNeedsAsyncDecode(orderedClips)) {
        asyncDecoder = std::make_unique<editor::AsyncDecoder>();
        asyncDecoder->initialize();
    }
    ScopedAvBufferRef sharedCudaDeviceForEncoder(
        asyncDecoder ? asyncDecoder->acquireSharedHwDevice(AV_HWDEVICE_TYPE_CUDA) : nullptr);

    AVFormatContext* formatCtx = nullptr;
    const QByteArray outputPathBytes = QFile::encodeName(request.outputPath);
    if (avformat_alloc_output_context2(&formatCtx, nullptr, nullptr, outputPathBytes.constData()) < 0 || !formatCtx) {
        result.message = QStringLiteral("Failed to create output format context.");
        return result;
    }

    AVStream* stream = avformat_new_stream(formatCtx, nullptr);
    if (!stream) {
        avformat_free_context(formatCtx);
        result.message = QStringLiteral("Failed to create output stream.");
        return result;
    }

    QString codecLabel;
    const AVCodec* codec = nullptr;
    AVCodecContext* codecCtx = nullptr;
    AVPixelFormat selectedEncoderSwFormat = AV_PIX_FMT_NONE;
    bool selectedEncoderCudaHardwareFrames = false;
    QStringList attemptedEncoders;
    const QVector<VideoEncoderChoice> encoderChoices = videoEncoderChoicesForRequest(request.outputFormat);
    for (const VideoEncoderChoice &choice : encoderChoices) {
        const AVCodec* candidate = avcodec_find_encoder_by_name(choice.label.toUtf8().constData());
        if (!candidate) {
            attemptedEncoders.push_back(choice.label + QStringLiteral(" (unavailable)"));
            qWarning().noquote()
                << QStringLiteral("Video encoder unavailable: %1 not registered in linked FFmpeg")
                       .arg(choice.label);
            continue;
        }

        AVCodecContext* candidateCtx = avcodec_alloc_context3(candidate);
        if (!candidateCtx) {
            attemptedEncoders.push_back(choice.label + QStringLiteral(" (alloc failed)"));
            continue;
        }

        const AVPixelFormat candidateSwFormat =
            choice.pixelFormat == AV_PIX_FMT_NONE
                ? pixelFormatForCodec(candidate, request.outputFormat)
                : choice.pixelFormat;
        configureVideoCodecContext(candidateCtx, candidate, request, candidateSwFormat, formatCtx);

        configureCodecOptions(candidateCtx, request.outputFormat, choice.label);
        const bool tryCudaFrames =
            choice.label == QStringLiteral("h264_nvenc") &&
            qEnvironmentVariable("JCUT_NVENC_CUDA_HWFRAMES", QStringLiteral("1")) != QStringLiteral("0");
        if (tryCudaFrames) {
            QString cudaError;
            if (configureCudaHardwareFrames(candidateCtx,
                                            candidateSwFormat,
                                            sharedCudaDeviceForEncoder.get(),
                                            &cudaError)) {
                const int cudaOpenResult = avcodec_open2(candidateCtx, candidate, nullptr);
                if (cudaOpenResult >= 0) {
                    qInfo().noquote()
                        << QStringLiteral("Video encoder selected: %1 pix_fmt=%2 sw_pix_fmt=%3")
                               .arg(choice.label,
                                    QString::fromLatin1(av_get_pix_fmt_name(candidateCtx->pix_fmt)),
                                    QString::fromLatin1(av_get_pix_fmt_name(candidateSwFormat)));
                    codec = candidate;
                    codecCtx = candidateCtx;
                    codecLabel = choice.label;
                    selectedEncoderSwFormat = candidateSwFormat;
                    selectedEncoderCudaHardwareFrames = true;
                    break;
                }
                const QString openError = avErrToString(cudaOpenResult);
                attemptedEncoders.push_back(choice.label + QStringLiteral(" cuda (open failed: %1)").arg(openError));
                qWarning().noquote()
                    << QStringLiteral("Video encoder unavailable: %1 pix_fmt=cuda sw_pix_fmt=%2 error=%3")
                           .arg(choice.label,
                                QString::fromLatin1(av_get_pix_fmt_name(candidateSwFormat)),
                                openError);
            } else {
                attemptedEncoders.push_back(choice.label + QStringLiteral(" cuda (setup failed: %1)").arg(cudaError));
                qWarning().noquote()
                    << QStringLiteral("Video encoder unavailable: %1 cuda setup failed: %2")
                           .arg(choice.label, cudaError);
            }
            avcodec_free_context(&candidateCtx);
            candidateCtx = avcodec_alloc_context3(candidate);
            if (!candidateCtx) {
                attemptedEncoders.push_back(choice.label + QStringLiteral(" cpu fallback (alloc failed)"));
                continue;
            }
            configureVideoCodecContext(candidateCtx, candidate, request, candidateSwFormat, formatCtx);
            configureCodecOptions(candidateCtx, request.outputFormat, choice.label);
        }
        const int openResult = avcodec_open2(candidateCtx, candidate, nullptr);
        if (openResult >= 0) {
            qInfo().noquote()
                << QStringLiteral("Video encoder selected: %1 pix_fmt=%2")
                       .arg(choice.label,
                            QString::fromLatin1(av_get_pix_fmt_name(candidateCtx->pix_fmt)));
            codec = candidate;
            codecCtx = candidateCtx;
            codecLabel = choice.label;
            selectedEncoderSwFormat = candidateSwFormat;
            selectedEncoderCudaHardwareFrames = false;
            break;
        }

        const QString openError = avErrToString(openResult);
        attemptedEncoders.push_back(choice.label + QStringLiteral(" (open failed: %1)").arg(openError));
        qWarning().noquote()
            << QStringLiteral("Video encoder unavailable: %1 pix_fmt=%2 error=%3")
                   .arg(choice.label,
                        QString::fromLatin1(av_get_pix_fmt_name(candidateCtx->pix_fmt)),
                        openError);
        avcodec_free_context(&candidateCtx);
    }

    if (!codec || !codecCtx) {
        codec = codecForRequest(request.outputFormat, &codecLabel);
        if (!codec) {
            avformat_free_context(formatCtx);
            result.message = QStringLiteral("No encoder available for format %1. Tried: %2")
                                 .arg(request.outputFormat, attemptedEncoders.join(QStringLiteral(", ")));
            return result;
        }

        codecCtx = avcodec_alloc_context3(codec);
        if (!codecCtx) {
            avformat_free_context(formatCtx);
            result.message = QStringLiteral("Failed to allocate encoder context.");
            return result;
        }

        configureVideoCodecContext(codecCtx,
                                   codec,
                                   request,
                                   pixelFormatForCodec(codec, request.outputFormat),
                                   formatCtx);
        selectedEncoderSwFormat = pixelFormatForCodec(codec, request.outputFormat);
        selectedEncoderCudaHardwareFrames = false;

        configureCodecOptions(codecCtx, request.outputFormat, codecLabel);

        if (avcodec_open2(codecCtx, codec, nullptr) < 0) {
            avcodec_free_context(&codecCtx);
            avformat_free_context(formatCtx);
            result.message = QStringLiteral("Failed to open encoder %1.").arg(codecLabel);
            return result;
        }
        qInfo().noquote()
            << QStringLiteral("Video encoder selected: %1 pix_fmt=%2")
                   .arg(codecLabel,
                        QString::fromLatin1(av_get_pix_fmt_name(codecCtx->pix_fmt)));
    }

    const bool usingHardwareEncode = isHardwareEncoderLabel(codecLabel);
    result.usedHardwareEncode = usingHardwareEncode;
    result.encoderLabel = codecLabel;

    if (avcodec_parameters_from_context(stream->codecpar, codecCtx) < 0) {
        avcodec_free_context(&codecCtx);
        avformat_free_context(formatCtx);
        result.message = QStringLiteral("Failed to copy encoder parameters.");
        return result;
    }
    stream->time_base = codecCtx->time_base;

    AudioExportState audioState;
    QElapsedTimer audioSetupTimer;
    audioSetupTimer.start();
    if (!initializeExportAudio(request, formatCtx, &audioState, &result.message)) {
        avcodec_free_context(&codecCtx);
        avformat_free_context(formatCtx);
        return result;
    }
    const qint64 audioSetupMs = audioSetupTimer.elapsed();
    result.audioSetupMs = audioSetupMs;

    if (!(formatCtx->oformat->flags & AVFMT_NOFILE)) {
        if (avio_open(&formatCtx->pb, outputPathBytes.constData(), AVIO_FLAG_WRITE) < 0) {
            if (audioState.codecCtx) {
                avcodec_free_context(&audioState.codecCtx);
            }
            avcodec_free_context(&codecCtx);
            avformat_free_context(formatCtx);
            result.message = QStringLiteral("Failed to open output file %1.").arg(QDir::toNativeSeparators(request.outputPath));
            return result;
        }
    }

    if (avformat_write_header(formatCtx, nullptr) < 0) {
        if (!(formatCtx->oformat->flags & AVFMT_NOFILE)) {
            avio_closep(&formatCtx->pb);
        }
        if (audioState.codecCtx) {
            avcodec_free_context(&audioState.codecCtx);
        }
        avcodec_free_context(&codecCtx);
        avformat_free_context(formatCtx);
        result.message = QStringLiteral("Failed to write output header.");
        return result;
    }

    const bool cudaHardwareFrames =
        selectedEncoderCudaHardwareFrames &&
        codecCtx->pix_fmt == AV_PIX_FMT_CUDA &&
        codecCtx->hw_frames_ctx != nullptr;
    const AVPixelFormat encoderInputPixFmt =
        selectedEncoderSwFormat == AV_PIX_FMT_NONE
            ? (cudaHardwareFrames ? AV_PIX_FMT_NV12 : codecCtx->pix_fmt)
            : selectedEncoderSwFormat;
    const bool directNv12Conversion = encoderInputPixFmt == AV_PIX_FMT_NV12;
    const ExportPathDecision exportPath =
        chooseExportPath(useGpuRenderer,
                         activeRenderer,
                         encoderInputPixFmt,
                         cudaHardwareFrames,
                         request.createVideoFromImageSequence,
                         qEnvironmentVariable("JCUT_VULKAN_CUDA_EXTERNAL_TRANSFER",
                                              QStringLiteral("1")) != QStringLiteral("0"));
    const bool vulkanGpuNv12Conversion = exportPath.gpuNv12Conversion;
    const bool vulkanCudaExternalTransfer = exportPath.cudaExternalTransfer;
    const bool cudaExternalMemorySupported = exportPath.cudaExternalMemorySupported;
    const QString cudaExternalMemoryStatus =
        activeRenderer ? activeRenderer->cudaExternalMemoryStatus()
                       : QStringLiteral("renderer unavailable");
    const QString encoderPixelFormatName =
        QString::fromLatin1(av_get_pix_fmt_name(codecCtx->pix_fmt));
    const QString encoderSoftwarePixelFormatName =
        QString::fromLatin1(av_get_pix_fmt_name(encoderInputPixFmt));
    qInfo().noquote()
        << QStringLiteral("[render-export-path] encoder=%1 pix_fmt=%2 sw_pix_fmt=%3 hw_frames=%4 vulkan_nv12=%5 cuda_external=%6 interop=%7")
               .arg(codecLabel,
                    encoderPixelFormatName,
                    encoderSoftwarePixelFormatName,
                    cudaHardwareFrames ? QStringLiteral("yes") : QStringLiteral("no"),
                    vulkanGpuNv12Conversion ? QStringLiteral("yes") : QStringLiteral("no"),
                    vulkanCudaExternalTransfer ? QStringLiteral("yes") : QStringLiteral("no"),
                    cudaExternalMemorySupported ? QStringLiteral("yes") : QStringLiteral("no"))
               << QStringLiteral("status=\"%1\" reason=\"%2\"")
                      .arg(cudaExternalMemoryStatus, exportPath.fallbackReason);
    if (vulkanCudaExternalTransfer) {
        qInfo().noquote() << QStringLiteral(
            "Vulkan export path: direct external-memory NV12 transfer into CUDA encoder frames enabled.");
    }
    const bool vulkanGpuYuv420pConversion =
        exportPath.gpuYuv420pConversion;
    if (requestedBackend == RenderBackend::Vulkan &&
        qEnvironmentVariable("JCUT_VULKAN_CUDA_EXTERNAL_MEMORY_REQUIRED", QStringLiteral("0")) != QStringLiteral("0") &&
        !vulkanCudaExternalTransfer) {
        result.message = QStringLiteral("Required Vulkan/CUDA direct export path is unavailable: %1")
                             .arg(exportPath.fallbackReason);
        if (audioState.codecCtx) {
            avcodec_free_context(&audioState.codecCtx);
        }
        av_write_trailer(formatCtx);
        if (!(formatCtx->oformat->flags & AVFMT_NOFILE)) {
            avio_closep(&formatCtx->pb);
        }
        avcodec_free_context(&codecCtx);
        avformat_free_context(formatCtx);
        return result;
    }
    const bool needsSoftwareColorConverter =
        !directNv12Conversion && !vulkanGpuYuv420pConversion;
    SwsContext* swsCtx = nullptr;
    if (needsSoftwareColorConverter) {
        swsCtx = sws_getContext(codecCtx->width,
                                codecCtx->height,
                                AV_PIX_FMT_BGRA,
                                codecCtx->width,
                                codecCtx->height,
                                encoderInputPixFmt,
                                SWS_BILINEAR,
                                nullptr,
                                nullptr,
                                nullptr);
        if (!swsCtx) {
            av_write_trailer(formatCtx);
            if (!(formatCtx->oformat->flags & AVFMT_NOFILE)) {
                avio_closep(&formatCtx->pb);
            }
            if (audioState.codecCtx) {
                avcodec_free_context(&audioState.codecCtx);
            }
            avcodec_free_context(&codecCtx);
            avformat_free_context(formatCtx);
            result.message = QStringLiteral("Failed to create render color converter.");
            return result;
        }
    }

    AVFrame* sourceFrame = needsSoftwareColorConverter ? av_frame_alloc() : nullptr;
    AVFrame* encodedFrame = av_frame_alloc();
    if ((needsSoftwareColorConverter && !sourceFrame) || !encodedFrame) {
        if (sourceFrame) av_frame_free(&sourceFrame);
        if (encodedFrame) av_frame_free(&encodedFrame);
        sws_freeContext(swsCtx);
        av_write_trailer(formatCtx);
        if (!(formatCtx->oformat->flags & AVFMT_NOFILE)) {
            avio_closep(&formatCtx->pb);
        }
        if (audioState.codecCtx) {
            avcodec_free_context(&audioState.codecCtx);
        }
        avcodec_free_context(&codecCtx);
        avformat_free_context(formatCtx);
        result.message = QStringLiteral("Failed to allocate render frames.");
        return result;
    }

    if (sourceFrame) {
        sourceFrame->format = AV_PIX_FMT_BGRA;
        sourceFrame->width = codecCtx->width;
        sourceFrame->height = codecCtx->height;
    }
    encodedFrame->format = encoderInputPixFmt;
    encodedFrame->width = codecCtx->width;
    encodedFrame->height = codecCtx->height;

    if ((!sourceFrame || av_frame_get_buffer(sourceFrame, 32) >= 0) &&
        av_frame_get_buffer(encodedFrame, 32) >= 0) {
    } else {
        av_frame_free(&sourceFrame);
        av_frame_free(&encodedFrame);
        sws_freeContext(swsCtx);
        av_write_trailer(formatCtx);
        if (!(formatCtx->oformat->flags & AVFMT_NOFILE)) {
            avio_closep(&formatCtx->pb);
        }
        if (audioState.codecCtx) {
            avcodec_free_context(&audioState.codecCtx);
        }
        avcodec_free_context(&codecCtx);
        avformat_free_context(formatCtx);
        result.message = QStringLiteral("Failed to allocate render frame buffers.");
        return result;
    }

    constexpr int kAsyncGpuFrameCount = 5;
    constexpr int kAsyncGpuMaxPendingReadbacks = 4;
    QVector<AVFrame*> asyncGpuFrames;
    QString asyncFrameAllocationError;
    if (vulkanGpuNv12Conversion || vulkanGpuYuv420pConversion) {
        asyncGpuFrames.reserve(kAsyncGpuFrameCount);
        for (int i = 0; i < kAsyncGpuFrameCount; ++i) {
            AVFrame* frame = av_frame_alloc();
            if (!frame) {
                asyncFrameAllocationError = QStringLiteral("Failed to allocate async Vulkan encoder frame.");
                break;
            }
            frame->format = vulkanCudaExternalTransfer ? AV_PIX_FMT_CUDA : encoderInputPixFmt;
            frame->width = codecCtx->width;
            frame->height = codecCtx->height;
            const int frameAllocResult = vulkanCudaExternalTransfer
                ? av_hwframe_get_buffer(codecCtx->hw_frames_ctx, frame, 0)
                : av_frame_get_buffer(frame, 32);
            if (frameAllocResult < 0) {
                av_frame_free(&frame);
                asyncFrameAllocationError = QStringLiteral("Failed to allocate async Vulkan encoder frame buffer: %1")
                                                .arg(avErrToString(frameAllocResult));
                break;
            }
            asyncGpuFrames.push_back(frame);
        }
        if (!asyncFrameAllocationError.isEmpty()) {
            for (AVFrame* frame : asyncGpuFrames) {
                av_frame_free(&frame);
            }
            av_frame_free(&sourceFrame);
            av_frame_free(&encodedFrame);
            sws_freeContext(swsCtx);
            av_write_trailer(formatCtx);
            if (!(formatCtx->oformat->flags & AVFMT_NOFILE)) {
                avio_closep(&formatCtx->pb);
            }
            if (audioState.codecCtx) {
                avcodec_free_context(&audioState.codecCtx);
            }
            avcodec_free_context(&codecCtx);
            avformat_free_context(formatCtx);
            result.message = asyncFrameAllocationError;
            return result;
        }
    }

    int64_t outputPts = 0;
    int64_t framesCompleted = 0;
    QHash<QString, editor::DecoderContext*> decoders;
    QHash<RenderAsyncFrameKey, editor::FrameHandle> asyncFrameCache;
    if (asyncDecoder) {
        QObject::connect(asyncDecoder.get(),
                         &editor::AsyncDecoder::frameReady,
                         [&](editor::FrameHandle frame) {
                             if (frame.isNull() || frame.sourcePath().isEmpty()) {
                                 return;
                             }
                             asyncFrameCache.insert(RenderAsyncFrameKey{frame.sourcePath(), frame.frameNumber()}, frame);
                             while (asyncFrameCache.size() > 256) {
                                 asyncFrameCache.erase(asyncFrameCache.begin());
                             }
                         });
    }
    QString errorMessage;
    qint64 totalRenderStageMs = 0;
    qint64 totalRenderDecodeStageMs = 0;
    qint64 totalRenderTextureStageMs = 0;
    qint64 totalRenderCompositeStageMs = 0;
    qint64 totalRenderNv12StageMs = 0;
    qint64 totalGpuReadbackMs = 0;
    qint64 totalOverlayStageMs = 0;
    qint64 totalConvertStageMs = 0;
    qint64 totalEncodeStageMs = 0;
    qint64 totalAudioStageMs = audioSetupMs;
    qint64 maxFrameRenderStageMs = 0;
    qint64 maxFrameDecodeStageMs = 0;
    qint64 maxFrameTextureStageMs = 0;
    qint64 maxFrameReadbackStageMs = 0;
    qint64 maxFrameConvertStageMs = 0;
    QHash<QString, RenderClipStageStats> clipStageStats;
    QVector<RenderFrameStageStats> worstFrames;
    QJsonArray lastSkippedClips;
    QJsonObject skippedReasonCounts;
    QJsonObject lastExportFaceTransformDiagnostics;
    QImage lastExportPreviewFrame;
    bool gpuPreviewBusyLogged = false;
    const QString exportPipeline = exportPath.pipeline;
    const QString gpuTransferLabel = exportPath.gpuTransferLabel;
    const QString exportPathFallbackReason = exportPath.fallbackReason;
    struct PendingAsyncGpuFrame {
        AVFrame* frame = nullptr;
        int64_t timelineFrame = 0;
        int segmentIndex = 0;
        qint64 frameRenderMs = 0;
        qint64 frameDecodeMs = 0;
        qint64 frameTextureMs = 0;
        qint64 frameLayerPlanMs = 0;
        qint64 frameTextPrepMs = 0;
        qint64 frameGuideOverlayMs = 0;
        qint64 frameGpuCompositeMs = 0;
        qint64 frameReadbackMs = 0;
        qint64 frameConvertStartMs = 0;
        int64_t pts = 0;
    };
    QVector<PendingAsyncGpuFrame> pendingAsyncGpuFrames;
    int asyncGpuFrameCursor = 0;
    auto asyncFrameIsPending = [&](AVFrame* frame) {
        for (const PendingAsyncGpuFrame& pending : pendingAsyncGpuFrames) {
            if (pending.frame == frame) {
                return true;
            }
        }
        return false;
    };
    auto finishOldestAsyncGpuFrame = [&]() -> bool {
        if (pendingAsyncGpuFrames.isEmpty()) {
            return true;
        }
        PendingAsyncGpuFrame pending = pendingAsyncGpuFrames.takeFirst();
        if (!pending.frame || av_frame_make_writable(pending.frame) < 0) {
            errorMessage = QStringLiteral("Failed to make async Vulkan encoder frame writable.");
            return false;
        }
        QElapsedTimer convertTimer;
        convertTimer.start();
        if (pending.frame->format == AV_PIX_FMT_CUDA) {
            if (!activeRenderer->finishLastFrameToNv12CudaTransfer(pending.frame,
                                                                   &totalRenderNv12StageMs,
                                                                   &totalGpuReadbackMs)) {
                errorMessage = QStringLiteral("Failed to finish Vulkan frame %1 direct CUDA transfer.")
                                   .arg(pending.timelineFrame);
                return false;
            }
        } else if (pending.frame->format == AV_PIX_FMT_NV12) {
            if (!activeRenderer->finishLastFrameToNv12Readback(pending.frame,
                                                               &totalRenderNv12StageMs,
                                                               &totalGpuReadbackMs)) {
                errorMessage = QStringLiteral("Failed to finish Vulkan frame %1 NV12 GPU readback.")
                                   .arg(pending.timelineFrame);
                return false;
            }
        } else {
            if (!activeRenderer->finishLastFrameToYuv420pReadback(pending.frame,
                                                                  &totalRenderNv12StageMs,
                                                                  &totalGpuReadbackMs)) {
                errorMessage = QStringLiteral("Failed to finish Vulkan frame %1 YUV420P GPU readback.")
                                   .arg(pending.timelineFrame);
                return false;
            }
        }
        const qint64 frameConvertMs = pending.frameConvertStartMs + convertTimer.elapsed();
        totalConvertStageMs += frameConvertMs;
        pending.frame->pts = pending.pts;
        QElapsedTimer encodeTimer;
        encodeTimer.start();
        if (!encodeFrame(codecCtx, stream, formatCtx, pending.frame, &errorMessage)) {
            return false;
        }
        totalEncodeStageMs += encodeTimer.elapsed();
        maxFrameRenderStageMs = qMax(maxFrameRenderStageMs, pending.frameRenderMs);
        maxFrameDecodeStageMs = qMax(maxFrameDecodeStageMs, pending.frameDecodeMs);
        maxFrameTextureStageMs = qMax(maxFrameTextureStageMs, pending.frameTextureMs);
        maxFrameReadbackStageMs = qMax(maxFrameReadbackStageMs, pending.frameReadbackMs);
        maxFrameConvertStageMs = qMax(maxFrameConvertStageMs, frameConvertMs);
        recordWorstFrame(&worstFrames,
                         RenderFrameStageStats{
                             pending.timelineFrame,
                             pending.segmentIndex + 1,
                             pending.frameRenderMs,
                             pending.frameDecodeMs,
                             pending.frameTextureMs,
                             pending.frameLayerPlanMs,
                             pending.frameTextPrepMs,
                             pending.frameGuideOverlayMs,
                             pending.frameGpuCompositeMs,
                             pending.frameReadbackMs,
                             frameConvertMs
                         });
        ++framesCompleted;
        return true;
    };

    for (int segmentIndex = 0; segmentIndex < exportRanges.size(); ++segmentIndex) {
        const ExportRangeSegment& range = exportRanges[segmentIndex];
        const int64_t exportStart = qMax<int64_t>(0, range.startFrame);
        const int64_t exportEnd = qMax(exportStart, range.endFrame);
        const int64_t segmentOutputFrames =
            jcut::export_timing::outputFrameCountForTimelineRange(
                exportStart,
                exportEnd,
                outputFps,
                playbackSpeed);
        prewarmRenderSequenceSegment(request,
                                     exportStart,
                                     exportEnd,
                                     orderedClips,
                                     asyncDecoder.get(),
                                     asyncFrameCache);
        prewarmRenderMaskSegment(request,
                                 exportStart,
                                 exportEnd,
                                 orderedClips);
        for (int64_t segmentOutputFrame = 0;
             segmentOutputFrame < segmentOutputFrames;
             ++segmentOutputFrame) {
            const jcut::export_timing::ExportFrameTiming exportFrameTiming =
                jcut::export_timing::frameTimingForOutputFrame(
                    segmentOutputFrame,
                    exportStart,
                    exportEnd,
                    outputFps,
                    playbackSpeed);
            const qreal timelineFramePosition =
                static_cast<qreal>(exportFrameTiming.timelineFramePosition);
            const int64_t timelineFrame = exportFrameTiming.timelineFrame;
            const int64_t outputFrameNumber = framesCompleted;
            enqueueRenderSequenceLookahead(request,
                                          timelineFrame,
                                          orderedClips,
                                          asyncDecoder.get(),
                                          asyncFrameCache);
            enqueueRenderMaskLookahead(request,
                                       timelineFrame,
                                       orderedClips);
            if (progressCallback) {
                RenderProgress progress;
                progress.framesCompleted = framesCompleted;
                progress.totalFrames = totalFramesToRender;
                progress.segmentIndex = segmentIndex + 1;
                progress.segmentCount = exportRanges.size();
                progress.timelineFrame = timelineFrame;
                progress.segmentStartFrame = exportStart;
                progress.segmentEndFrame = exportEnd;
                progress.usingGpu = useGpuRenderer;
                progress.usingHardwareEncode = usingHardwareEncode;
                progress.encoderLabel = codecLabel;
                progress.exportPipeline = exportPipeline;
                progress.gpuTransferLabel = gpuTransferLabel;
                progress.encoderPixelFormat = encoderPixelFormatName;
                progress.encoderSoftwarePixelFormat = encoderSoftwarePixelFormatName;
                progress.cudaExternalMemoryStatus = cudaExternalMemoryStatus;
                progress.exportPathFallbackReason = exportPathFallbackReason;
                progress.cudaExternalTransfer = vulkanCudaExternalTransfer;
                progress.cudaExternalMemorySupported = cudaExternalMemorySupported;
                progress.encoderHardwareFrames = cudaHardwareFrames;
                progress.elapsedMs = totalTimer.elapsed();
                progress.estimatedRemainingMs =
                    progress.framesCompleted > 0
                        ? (progress.elapsedMs * qMax<int64_t>(0, progress.totalFrames - progress.framesCompleted)) /
                              qMax<int64_t>(1, progress.framesCompleted)
                        : -1;
                progress.renderStageMs = totalRenderStageMs;
                progress.renderDecodeStageMs = totalRenderDecodeStageMs;
                progress.renderTextureStageMs = totalRenderTextureStageMs;
                progress.renderCompositeStageMs = totalRenderCompositeStageMs;
                progress.renderNv12StageMs = totalRenderNv12StageMs;
                progress.gpuReadbackMs = totalGpuReadbackMs;
                progress.overlayStageMs = totalOverlayStageMs;
                progress.convertStageMs = totalConvertStageMs;
                progress.encodeStageMs = totalEncodeStageMs;
                progress.audioStageMs = totalAudioStageMs;
                progress.audioSetupMs = audioSetupMs;
                progress.maxFrameRenderStageMs = maxFrameRenderStageMs;
                progress.maxFrameDecodeStageMs = maxFrameDecodeStageMs;
                progress.maxFrameTextureStageMs = maxFrameTextureStageMs;
                progress.maxFrameReadbackStageMs = maxFrameReadbackStageMs;
                progress.maxFrameConvertStageMs = maxFrameConvertStageMs;
                progress.skippedClips = lastSkippedClips;
                progress.skippedClipReasonCounts = skippedReasonCounts;
                progress.renderStageTable = buildRenderStageTable(clipStageStats, totalRenderStageMs, framesCompleted);
                progress.worstFrameTable = buildWorstFrameTable(worstFrames);
                progress.exportFaceTransformDiagnostics = lastExportFaceTransformDiagnostics;
                progress.previewFrame = lastExportPreviewFrame;
                if (!progressCallback(progress)) {
                    result.cancelled = true;
                    errorMessage = QStringLiteral("Render cancelled.");
                    break;
                }
            }
            QElapsedTimer renderStageTimer;
            renderStageTimer.start();
            qint64 frameDecodeMs = 0;
            qint64 frameTextureMs = 0;
            qint64 frameCompositeMs = 0;
            qint64 frameReadbackMs = 0;
            qint64 frameLayerPlanMs = 0;
            qint64 frameTextPrepMs = 0;
            qint64 frameGuideOverlayMs = 0;
            qint64 frameGpuCompositeMs = 0;
            qint64* frameReadbackMsPtr = &frameReadbackMs;
            const PlaybackTimelineFrameClocks frameClocks =
                playbackTimelineFrameClocks(timelineFramePosition, request.playbackTiming);
            const PlaybackFrameCrossfade frameCrossfade =
                playbackFrameCrossfadeAtTimelineFrame(
                    frameClocks.transportTimelineFrame,
                    request.playbackTiming);
            const bool directGpuFrameReadback =
                useGpuRenderer && !request.createVideoFromImageSequence && !frameCrossfade.active;
            if (directGpuFrameReadback) {
                frameReadbackMsPtr = nullptr;
            }
            QJsonArray frameSkippedClips;
            render_detail::OffscreenRenderFrame renderedFrame;
            render_detail::OffscreenVulkanFrame gpuPreviewFrame;
            QString gpuPreviewError;
            const bool publishGpuPreview =
                progressCallback &&
                request.gpuExportPreviewEnabled &&
                (!request.gpuExportPreviewReady ||
                 request.gpuExportPreviewReady->load(
                     std::memory_order_acquire));
            QJsonObject frameExportFaceTransformDiagnostics;
            const bool renderedOk =
                activeRenderer->renderFrameToOutput(request,
                                                    frameClocks.visualTimelineFrame,
                                                    decoders,
                                                    asyncDecoder.get(),
                                                    &asyncFrameCache,
                                                    orderedClips,
                                                    &renderedFrame,
                                                    !directGpuFrameReadback,
                                                    &clipStageStats,
                                                    &frameDecodeMs,
                                                    &frameTextureMs,
                                                    &frameCompositeMs,
                                                    frameReadbackMsPtr,
                                                    &frameSkippedClips,
                                                    &skippedReasonCounts,
                                                    &frameExportFaceTransformDiagnostics,
                                                    frameClocks.transportTimelineFrame,
                                                    false,
                                                    false,
                                                    false,
                                                    &frameLayerPlanMs,
                                                    &frameTextPrepMs,
                                                    &frameGuideOverlayMs,
                                                    &frameGpuCompositeMs,
                                                    publishGpuPreview
                                                        ? &gpuPreviewFrame
                                                        : nullptr,
                                                    publishGpuPreview
                                                        ? &gpuPreviewError
                                                        : nullptr);
            QImage rendered = renderedFrame.cpuImage;
            lastSkippedClips = frameSkippedClips;
            if (!frameExportFaceTransformDiagnostics.isEmpty()) {
                lastExportFaceTransformDiagnostics = frameExportFaceTransformDiagnostics;
            }
            totalRenderStageMs += renderStageTimer.elapsed();
            totalRenderDecodeStageMs += frameDecodeMs;
            totalRenderTextureStageMs += frameTextureMs;
            totalRenderCompositeStageMs += frameCompositeMs;
            totalGpuReadbackMs += frameReadbackMs;
            if ((!renderedOk || rendered.isNull()) && !directGpuFrameReadback) {
                const QString frameFailure = renderedFrame.failureReason.trimmed();
                errorMessage = frameFailure.isEmpty()
                    ? QStringLiteral("Failed to render Vulkan timeline frame %1.").arg(timelineFrame)
                    : QStringLiteral("Failed to render Vulkan timeline frame %1: %2")
                          .arg(timelineFrame)
                          .arg(frameFailure);
                break;
            }
            if (frameCrossfade.active && !rendered.isNull()) {
                RenderRequest secondaryRequest = request;
                secondaryRequest.playbackTiming.frameCrossfadeEnabled = false;
                render_detail::OffscreenRenderFrame secondaryFrame;
                QJsonArray secondarySkippedClips;
                QJsonObject secondaryFaceDiagnostics;
                qint64 secondaryDecodeMs = 0;
                qint64 secondaryTextureMs = 0;
                qint64 secondaryCompositeMs = 0;
                qint64 secondaryReadbackMs = 0;
                const bool secondaryOk = activeRenderer->renderFrameToOutput(
                    secondaryRequest,
                    static_cast<qreal>(frameCrossfade.secondaryTimelineFrame),
                    decoders,
                    asyncDecoder.get(),
                    &asyncFrameCache,
                    orderedClips,
                    &secondaryFrame,
                    true,
                    &clipStageStats,
                    &secondaryDecodeMs,
                    &secondaryTextureMs,
                    &secondaryCompositeMs,
                    &secondaryReadbackMs,
                    &secondarySkippedClips,
                    &skippedReasonCounts,
                    &secondaryFaceDiagnostics,
                    static_cast<qreal>(frameCrossfade.secondaryTimelineFrame));
                totalRenderDecodeStageMs += secondaryDecodeMs;
                totalRenderTextureStageMs += secondaryTextureMs;
                totalRenderCompositeStageMs += secondaryCompositeMs;
                totalGpuReadbackMs += secondaryReadbackMs;
                if (secondaryOk && !secondaryFrame.cpuImage.isNull()) {
                    QImage blended = rendered.convertToFormat(QImage::Format_ARGB32_Premultiplied);
                    QImage incoming = secondaryFrame.cpuImage.convertToFormat(QImage::Format_ARGB32_Premultiplied);
                    QPainter painter(&blended);
                    painter.setCompositionMode(QPainter::CompositionMode_SourceOver);
                    painter.setOpacity(qBound(0.0f, frameCrossfade.secondaryOpacity, 1.0f));
                    painter.drawImage(QPoint(0, 0), incoming);
                    painter.end();
                    rendered = blended;
                    renderedFrame.cpuImage = rendered;
                }
            }

            // Save intermediate image files if requested
            if (request.createVideoFromImageSequence && !namedDirPath.isEmpty() && !request.imageSequenceFormat.isEmpty()) {
                const QString format =
                    normalizedImageSequenceFormat(request.imageSequenceFormat);
                const QString imagePath =
                    QStringLiteral("%1/frame_%2.%3")
                        .arg(namedDirPath)
                        .arg(outputFrameNumber, 8, 10, QChar('0'))
                        .arg(format);
                QString saveError;
                if (!saveImageAtomically(
                        rendered, imagePath, format, &saveError)) {
                    errorMessage =
                        QStringLiteral("Failed to atomically checkpoint "
                                       "rendered frame %1 to %2: %3")
                            .arg(timelineFrame)
                            .arg(imagePath, saveError);
                    break;
                }
            }

            if (!gpuPreviewError.isEmpty()) {
                const bool optionalSlotBusy =
                    gpuPreviewError.contains(
                        QStringLiteral("all optional preview slots are busy"));
                if (!optionalSlotBusy || !gpuPreviewBusyLogged) {
                    qWarning().noquote()
                        << QStringLiteral("GPU export preview unavailable: %1")
                               .arg(gpuPreviewError);
                }
                gpuPreviewBusyLogged = optionalSlotBusy;
            }
            if (gpuPreviewFrame.valid) {
                gpuPreviewBusyLogged = false;
            }
            if (progressCallback && !rendered.isNull()) {
                lastExportPreviewFrame = rendered;
            }
            if (progressCallback) {
                RenderProgress progress;
                progress.framesCompleted = framesCompleted;
                progress.totalFrames = totalFramesToRender;
                progress.segmentIndex = segmentIndex + 1;
                progress.segmentCount = exportRanges.size();
                progress.timelineFrame = timelineFrame;
                progress.segmentStartFrame = exportStart;
                progress.segmentEndFrame = exportEnd;
                progress.usingGpu = useGpuRenderer;
                progress.usingHardwareEncode = usingHardwareEncode;
                progress.encoderLabel = codecLabel;
                progress.exportPipeline = exportPipeline;
                progress.gpuTransferLabel = gpuTransferLabel;
                progress.encoderPixelFormat = encoderPixelFormatName;
                progress.encoderSoftwarePixelFormat = encoderSoftwarePixelFormatName;
                progress.cudaExternalMemoryStatus = cudaExternalMemoryStatus;
                progress.exportPathFallbackReason = exportPathFallbackReason;
                progress.cudaExternalTransfer = vulkanCudaExternalTransfer;
                progress.cudaExternalMemorySupported = cudaExternalMemorySupported;
                progress.encoderHardwareFrames = cudaHardwareFrames;
                progress.elapsedMs = totalTimer.elapsed();
                progress.estimatedRemainingMs =
                    progress.framesCompleted > 0
                        ? (progress.elapsedMs * qMax<int64_t>(0, progress.totalFrames - progress.framesCompleted)) /
                              qMax<int64_t>(1, progress.framesCompleted)
                        : -1;
                progress.renderStageMs = totalRenderStageMs;
                progress.renderDecodeStageMs = totalRenderDecodeStageMs;
                progress.renderTextureStageMs = totalRenderTextureStageMs;
                progress.renderCompositeStageMs = totalRenderCompositeStageMs;
                progress.renderNv12StageMs = totalRenderNv12StageMs;
                progress.gpuReadbackMs = totalGpuReadbackMs;
                progress.overlayStageMs = totalOverlayStageMs;
                progress.convertStageMs = totalConvertStageMs;
                progress.encodeStageMs = totalEncodeStageMs;
                progress.audioStageMs = totalAudioStageMs;
                progress.audioSetupMs = audioSetupMs;
                progress.maxFrameRenderStageMs = maxFrameRenderStageMs;
                progress.maxFrameDecodeStageMs = maxFrameDecodeStageMs;
                progress.maxFrameTextureStageMs = maxFrameTextureStageMs;
                progress.maxFrameReadbackStageMs = maxFrameReadbackStageMs;
                progress.maxFrameConvertStageMs = maxFrameConvertStageMs;
                progress.skippedClips = lastSkippedClips;
                progress.skippedClipReasonCounts = skippedReasonCounts;
                progress.renderStageTable = buildRenderStageTable(clipStageStats, totalRenderStageMs, framesCompleted);
                progress.worstFrameTable = buildWorstFrameTable(worstFrames);
                progress.exportFaceTransformDiagnostics = lastExportFaceTransformDiagnostics;
                if (!lastExportPreviewFrame.isNull()) {
                    progress.previewFrame = lastExportPreviewFrame;
                } else if (!directNv12Conversion) {
                    progress.previewFrame = rendered;
                }
                progress.gpuPreviewFrame = gpuPreviewFrame;
                if (!progressCallback(progress)) {
                    result.cancelled = true;
                    errorMessage = QStringLiteral("Render cancelled.");
                    break;
                }
            }

            const bool willQueueAsyncGpuFrame =
                directGpuFrameReadback &&
                rendered.isNull() &&
                ((directNv12Conversion && vulkanGpuNv12Conversion) ||
                 (!directNv12Conversion && vulkanGpuYuv420pConversion));
            while (!willQueueAsyncGpuFrame &&
                   errorMessage.isEmpty() &&
                   !pendingAsyncGpuFrames.isEmpty()) {
                if (!finishOldestAsyncGpuFrame()) {
                    break;
                }
            }
            if (!errorMessage.isEmpty()) {
                break;
            }

            if ((!sourceFrame || av_frame_make_writable(sourceFrame) >= 0) &&
                av_frame_make_writable(encodedFrame) >= 0) {
            } else {
                errorMessage = QStringLiteral("Failed to make render frame writable.");
                break;
            }

            QElapsedTimer convertTimer;
            convertTimer.start();
            if (directNv12Conversion) {
                if (directGpuFrameReadback &&
                    rendered.isNull() &&
                    vulkanGpuNv12Conversion) {
                    if (pendingAsyncGpuFrames.size() >= kAsyncGpuMaxPendingReadbacks &&
                        !finishOldestAsyncGpuFrame()) {
                        break;
                    }
                    if (asyncGpuFrames.isEmpty()) {
                        errorMessage = QStringLiteral("No async Vulkan encoder frames are available.");
                        break;
                    }
                    AVFrame* gpuFrame = asyncGpuFrames[asyncGpuFrameCursor % asyncGpuFrames.size()];
                    ++asyncGpuFrameCursor;
                    while (asyncFrameIsPending(gpuFrame)) {
                        if (!finishOldestAsyncGpuFrame()) {
                            break;
                        }
                        gpuFrame = asyncGpuFrames[asyncGpuFrameCursor % asyncGpuFrames.size()];
                        ++asyncGpuFrameCursor;
                    }
                    if (!errorMessage.isEmpty()) {
                        break;
                    }
                    // NVENC can retain a CUDA surface after avcodec_send_frame().
                    // Detach the pooled AVFrame before Vulkan writes the next image
                    // or the encoder can observe overwritten frames out of order.
                    if (av_frame_make_writable(gpuFrame) < 0) {
                        errorMessage = QStringLiteral("Failed to make async Vulkan encoder frame writable.");
                        break;
                    }
                    const bool beginNv12 = vulkanCudaExternalTransfer
                        ? activeRenderer->beginLastFrameToNv12CudaTransfer(&totalRenderNv12StageMs,
                                                                           &totalGpuReadbackMs)
                        : activeRenderer->beginLastFrameToNv12Readback(&totalRenderNv12StageMs,
                                                                       &totalGpuReadbackMs);
                    if (!beginNv12) {
                        errorMessage = QStringLiteral(
                            "Failed to convert Vulkan frame %1 to NV12 on GPU.")
                                           .arg(timelineFrame);
                        break;
                    }
                    const qint64 frameConvertMs = convertTimer.elapsed();
                    pendingAsyncGpuFrames.push_back(PendingAsyncGpuFrame{
                        gpuFrame,
                        timelineFrame,
                        segmentIndex,
                        renderStageTimer.elapsed(),
                        frameDecodeMs,
                        frameTextureMs,
                        frameLayerPlanMs,
                        frameTextPrepMs,
                        frameGuideOverlayMs,
                        frameGpuCompositeMs,
                        frameReadbackMs,
                        frameConvertMs,
                        outputPts++
                    });
                    if (pendingAsyncGpuFrames.size() > kAsyncGpuMaxPendingReadbacks &&
                        !finishOldestAsyncGpuFrame()) {
                        break;
                    }
                    if (!errorMessage.isEmpty()) {
                        break;
                    }
                    continue;
                }
                const bool gpuNv12Converted =
                    useGpuRenderer && activeRenderer->convertLastFrameToNv12(encodedFrame,
                                                                         &totalRenderNv12StageMs,
                                                                         &totalGpuReadbackMs);
                if (!gpuNv12Converted && !fillNv12FrameFromRenderedImage(rendered, encodedFrame)) {
                    errorMessage = QStringLiteral("Failed to convert rendered frame %1 to NV12 for encoding.").arg(timelineFrame);
                    break;
                }
            } else {
                if (directGpuFrameReadback &&
                    rendered.isNull() &&
                    vulkanGpuYuv420pConversion) {
                    if (pendingAsyncGpuFrames.size() >= kAsyncGpuMaxPendingReadbacks &&
                        !finishOldestAsyncGpuFrame()) {
                        break;
                    }
                    if (asyncGpuFrames.isEmpty()) {
                        errorMessage = QStringLiteral("No async Vulkan encoder frames are available.");
                        break;
                    }
                    AVFrame* gpuFrame = asyncGpuFrames[asyncGpuFrameCursor % asyncGpuFrames.size()];
                    ++asyncGpuFrameCursor;
                    while (asyncFrameIsPending(gpuFrame)) {
                        if (!finishOldestAsyncGpuFrame()) {
                            break;
                        }
                        gpuFrame = asyncGpuFrames[asyncGpuFrameCursor % asyncGpuFrames.size()];
                        ++asyncGpuFrameCursor;
                    }
                    if (!errorMessage.isEmpty()) {
                        break;
                    }
                    if (av_frame_make_writable(gpuFrame) < 0) {
                        errorMessage = QStringLiteral("Failed to make async Vulkan encoder frame writable.");
                        break;
                    }
                    if (!activeRenderer->beginLastFrameToYuv420pReadback(&totalRenderNv12StageMs,
                                                                         &totalGpuReadbackMs)) {
                        errorMessage = QStringLiteral(
                            "Failed to convert Vulkan frame %1 to YUV420P on GPU. "
                            "CPU swscale fallback is disabled for Vulkan.")
                                           .arg(timelineFrame);
                        break;
                    }
                    const qint64 frameConvertMs = convertTimer.elapsed();
                    pendingAsyncGpuFrames.push_back(PendingAsyncGpuFrame{
                        gpuFrame,
                        timelineFrame,
                        segmentIndex,
                        renderStageTimer.elapsed(),
                        frameDecodeMs,
                        frameTextureMs,
                        frameLayerPlanMs,
                        frameTextPrepMs,
                        frameGuideOverlayMs,
                        frameGpuCompositeMs,
                        frameReadbackMs,
                        frameConvertMs,
                        outputPts++
                    });
                    // Keep a small backlog so GPU readback for the newest frames can overlap CPU encode of older frames.
                    if (pendingAsyncGpuFrames.size() > kAsyncGpuMaxPendingReadbacks &&
                        !finishOldestAsyncGpuFrame()) {
                        break;
                    }
                    if (!errorMessage.isEmpty()) {
                        break;
                    }
                    continue;
                }
                if (directGpuFrameReadback &&
                    rendered.isNull() &&
                    encoderInputPixFmt == AV_PIX_FMT_YUV420P &&
                    activeRenderer->convertLastFrameToYuv420p(encodedFrame,
                                                              &totalRenderNv12StageMs,
                                                              &totalGpuReadbackMs)) {
                    const qint64 frameConvertMs = convertTimer.elapsed();
                    totalConvertStageMs += frameConvertMs;
                    encodedFrame->pts = outputPts++;
                    QElapsedTimer encodeTimer;
                    encodeTimer.start();
                    if (!encodeFrame(codecCtx, stream, formatCtx, encodedFrame, &errorMessage)) {
                        break;
                    }
                    totalEncodeStageMs += encodeTimer.elapsed();
                    const qint64 frameRenderMs = renderStageTimer.elapsed();
                    maxFrameRenderStageMs = qMax(maxFrameRenderStageMs, frameRenderMs);
                    maxFrameDecodeStageMs = qMax(maxFrameDecodeStageMs, frameDecodeMs);
                    maxFrameTextureStageMs = qMax(maxFrameTextureStageMs, frameTextureMs);
                    maxFrameReadbackStageMs = qMax(maxFrameReadbackStageMs, frameReadbackMs);
                    maxFrameConvertStageMs = qMax(maxFrameConvertStageMs, frameConvertMs);
                    recordWorstFrame(&worstFrames,
                                     RenderFrameStageStats{
                                         timelineFrame,
                                         segmentIndex + 1,
                                         frameRenderMs,
                                         frameDecodeMs,
                                         frameTextureMs,
                                         frameLayerPlanMs,
                                         frameTextPrepMs,
                                         frameGuideOverlayMs,
                                         frameGpuCompositeMs,
                                         frameReadbackMs,
                                         frameConvertMs
                                     });
                    ++framesCompleted;
                    if (!errorMessage.isEmpty()) {
                        break;
                    }
                    continue;
                }
                if (directGpuFrameReadback && rendered.isNull()) {
                    if (!activeRenderer->copyLastFrameToBgra(sourceFrame, &totalGpuReadbackMs)) {
                        errorMessage = QStringLiteral("Failed to read back Vulkan frame %1 for encoding.").arg(timelineFrame);
                        break;
                    }
                } else {
                    const int copyBytesPerRow = qMin(static_cast<int>(rendered.bytesPerLine()), sourceFrame->linesize[0]);
                    for (int y = 0; y < rendered.height(); ++y) {
                        memcpy(sourceFrame->data[0] + (y * sourceFrame->linesize[0]),
                               rendered.constScanLine(y),
                               copyBytesPerRow);
                    }
                }

                if (sws_scale(swsCtx,
                              sourceFrame->data,
                              sourceFrame->linesize,
                              0,
                              sourceFrame->height,
                              encodedFrame->data,
                              encodedFrame->linesize) <= 0) {
                    errorMessage = QStringLiteral("Failed to convert rendered frame %1 for encoding.").arg(timelineFrame);
                    break;
                }
            }
            const qint64 frameConvertMs = convertTimer.elapsed();
            totalConvertStageMs += frameConvertMs;

            encodedFrame->pts = outputPts++;
            QElapsedTimer encodeTimer;
            encodeTimer.start();
            if (!encodeFrame(codecCtx, stream, formatCtx, encodedFrame, &errorMessage)) {
                break;
            }
            totalEncodeStageMs += encodeTimer.elapsed();
            const qint64 frameRenderMs = renderStageTimer.elapsed();
            maxFrameRenderStageMs = qMax(maxFrameRenderStageMs, frameRenderMs);
            maxFrameDecodeStageMs = qMax(maxFrameDecodeStageMs, frameDecodeMs);
            maxFrameTextureStageMs = qMax(maxFrameTextureStageMs, frameTextureMs);
            maxFrameReadbackStageMs = qMax(maxFrameReadbackStageMs, frameReadbackMs);
            maxFrameConvertStageMs = qMax(maxFrameConvertStageMs, frameConvertMs);
            recordWorstFrame(&worstFrames,
                             RenderFrameStageStats{
                                 timelineFrame,
                                 segmentIndex + 1,
                                 frameRenderMs,
                                 frameDecodeMs,
                                 frameTextureMs,
                                 frameLayerPlanMs,
                                 frameTextPrepMs,
                                 frameGuideOverlayMs,
                                 frameGpuCompositeMs,
                                 frameReadbackMs,
                                 frameConvertMs
                             });
            ++framesCompleted;
            if (!errorMessage.isEmpty()) {
                break;
            }
        }
        if (!errorMessage.isEmpty()) {
            break;
        }
    }

    while (errorMessage.isEmpty() && !pendingAsyncGpuFrames.isEmpty()) {
        if (!finishOldestAsyncGpuFrame()) {
            break;
        }
    }

    if (errorMessage.isEmpty()) {
        encodeFrame(codecCtx, stream, formatCtx, nullptr, &errorMessage);
    }

    if (errorMessage.isEmpty() && audioState.enabled) {
        QElapsedTimer audioTimer;
        audioTimer.start();
        encodeExportAudio(exportRanges, audioState, formatCtx, playbackSpeed, &errorMessage);
        totalAudioStageMs += audioTimer.elapsed();
    }

    av_write_trailer(formatCtx);
    qDeleteAll(decoders);
    decoders.clear();
    if (asyncDecoder) {
        asyncDecoder->shutdown();
    }
    for (AVFrame* frame : asyncGpuFrames) {
        av_frame_free(&frame);
    }
    av_frame_free(&sourceFrame);
    av_frame_free(&encodedFrame);
    sws_freeContext(swsCtx);
    if (!(formatCtx->oformat->flags & AVFMT_NOFILE)) {
        avio_closep(&formatCtx->pb);
    }
    if (audioState.codecCtx) {
        avcodec_free_context(&audioState.codecCtx);
    }
    if (!persistentRenderer && progressCallback && useGpuRenderer &&
        request.gpuExportPreviewEnabled) {
        activeRenderer->finishGpuPreviewPublication();
        RenderProgress releasePreview;
        releasePreview.releaseGpuPreview = true;
        progressCallback(releasePreview);
    }
    // Release Vulkan/CUDA external-memory imports while FFmpeg's CUDA device context is still alive.
    if (ownedRenderer) {
        ownedRenderer.reset();
        activeRenderer = nullptr;
    }
    avcodec_free_context(&codecCtx);
    avformat_free_context(formatCtx);

    if (!errorMessage.isEmpty()) {
        result.message = errorMessage;
        result.exportPipeline = exportPipeline;
        result.gpuTransferLabel = gpuTransferLabel;
        result.encoderPixelFormat = encoderPixelFormatName;
        result.encoderSoftwarePixelFormat = encoderSoftwarePixelFormatName;
        result.cudaExternalMemoryStatus = cudaExternalMemoryStatus;
        result.exportPathFallbackReason = exportPathFallbackReason;
        result.cudaExternalTransfer = vulkanCudaExternalTransfer;
        result.cudaExternalMemorySupported = cudaExternalMemorySupported;
        result.encoderHardwareFrames = cudaHardwareFrames;
        result.framesRendered = framesCompleted;
        result.elapsedMs = totalTimer.elapsed();
        result.renderStageMs = totalRenderStageMs;
        result.renderDecodeStageMs = totalRenderDecodeStageMs;
        result.renderTextureStageMs = totalRenderTextureStageMs;
        result.renderCompositeStageMs = totalRenderCompositeStageMs;
        result.renderNv12StageMs = totalRenderNv12StageMs;
        result.gpuReadbackMs = totalGpuReadbackMs;
        result.overlayStageMs = totalOverlayStageMs;
        result.convertStageMs = totalConvertStageMs;
        result.encodeStageMs = totalEncodeStageMs;
        result.audioStageMs = totalAudioStageMs;
        result.audioSetupMs = audioSetupMs;
        result.maxFrameRenderStageMs = maxFrameRenderStageMs;
        result.maxFrameDecodeStageMs = maxFrameDecodeStageMs;
        result.maxFrameTextureStageMs = maxFrameTextureStageMs;
        result.maxFrameReadbackStageMs = maxFrameReadbackStageMs;
        result.maxFrameConvertStageMs = maxFrameConvertStageMs;
        result.skippedClips = lastSkippedClips;
        result.skippedClipReasonCounts = skippedReasonCounts;
        result.renderStageTable = buildRenderStageTable(clipStageStats, totalRenderStageMs, framesCompleted);
        result.worstFrameTable = buildWorstFrameTable(worstFrames);
        result.exportFaceTransformDiagnostics = lastExportFaceTransformDiagnostics;
        return result;
    }

    result.success = true;
    result.exportPipeline = exportPipeline;
    result.gpuTransferLabel = gpuTransferLabel;
    result.encoderPixelFormat = encoderPixelFormatName;
    result.encoderSoftwarePixelFormat = encoderSoftwarePixelFormatName;
    result.cudaExternalMemoryStatus = cudaExternalMemoryStatus;
    result.exportPathFallbackReason = exportPathFallbackReason;
    result.cudaExternalTransfer = vulkanCudaExternalTransfer;
    result.cudaExternalMemorySupported = cudaExternalMemorySupported;
    result.encoderHardwareFrames = cudaHardwareFrames;
    result.framesRendered = framesCompleted;
    result.elapsedMs = totalTimer.elapsed();
    result.renderStageMs = totalRenderStageMs;
    result.renderDecodeStageMs = totalRenderDecodeStageMs;
    result.renderTextureStageMs = totalRenderTextureStageMs;
    result.renderCompositeStageMs = totalRenderCompositeStageMs;
    result.renderNv12StageMs = totalRenderNv12StageMs;
    result.gpuReadbackMs = totalGpuReadbackMs;
    result.overlayStageMs = totalOverlayStageMs;
    result.convertStageMs = totalConvertStageMs;
    result.encodeStageMs = totalEncodeStageMs;
    result.audioStageMs = totalAudioStageMs;
    result.audioSetupMs = audioSetupMs;
    result.maxFrameRenderStageMs = maxFrameRenderStageMs;
    result.maxFrameDecodeStageMs = maxFrameDecodeStageMs;
    result.maxFrameTextureStageMs = maxFrameTextureStageMs;
    result.maxFrameReadbackStageMs = maxFrameReadbackStageMs;
    result.maxFrameConvertStageMs = maxFrameConvertStageMs;
    result.skippedClips = lastSkippedClips;
    result.skippedClipReasonCounts = skippedReasonCounts;
    result.renderStageTable = buildRenderStageTable(clipStageStats, totalRenderStageMs, framesCompleted);
    result.worstFrameTable = buildWorstFrameTable(worstFrames);
    result.exportFaceTransformDiagnostics = lastExportFaceTransformDiagnostics;
    QString renderPathSuffix;
    if (useGpuRenderer && activeRenderer) {
        renderPathSuffix = QStringLiteral("\nRender path: %1").arg(activeRenderer->backendId());
    }
    result.message = QStringLiteral("Rendered %1 video frames to %2%3")
                         .arg(framesCompleted)
                         .arg(QDir::toNativeSeparators(request.outputPath))
                         .arg(renderPathSuffix);
    return result;
}

namespace {

struct IncrementalRenderChunk {
    int index = 0;
    QVector<ExportRangeSegment> ranges;
    int64_t frameCount = 0;
    QString path;
};

inline constexpr int kIncrementalRenderSchema = 4;

std::optional<int64_t> encodedVideoFrameCount(const QString& path)
{
    const QString ffprobe =
        QStandardPaths::findExecutable(QStringLiteral("ffprobe"));
    if (ffprobe.isEmpty()) {
        return std::nullopt;
    }
    QProcess probe;
    probe.start(
        ffprobe,
        {QStringLiteral("-v"), QStringLiteral("error"),
         QStringLiteral("-count_frames"),
         QStringLiteral("-select_streams"), QStringLiteral("v:0"),
         QStringLiteral("-show_entries"),
         QStringLiteral("stream=nb_read_frames"),
         QStringLiteral("-of"), QStringLiteral("default=nw=1:nk=1"),
         path});
    if (!probe.waitForFinished(30000) ||
        probe.exitStatus() != QProcess::NormalExit ||
        probe.exitCode() != 0) {
        return std::nullopt;
    }
    bool ok = false;
    const int64_t frames =
        QString::fromUtf8(probe.readAllStandardOutput())
            .trimmed()
            .toLongLong(&ok);
    if (!ok || frames < 0) {
        return std::nullopt;
    }
    return frames;
}

bool hasCompleteIncrementalEncodedChunk(
    const IncrementalRenderChunk& chunk,
    const RenderRequest& request)
{
    Q_UNUSED(request);
    const QFileInfo info(chunk.path);
    if (!info.isFile() || info.size() <= 0) {
        return false;
    }
    const std::optional<int64_t> frames =
        encodedVideoFrameCount(info.absoluteFilePath());
    if (frames && *frames != chunk.frameCount) {
        return false;
    }
    return true;
}

QString incrementalChunkAttemptPath(const IncrementalRenderChunk& chunk)
{
    const QFileInfo info(chunk.path);
    return info.dir().filePath(
        QStringLiteral("%1.tmp.%2.%3.%4")
            .arg(info.completeBaseName())
            .arg(QCoreApplication::applicationPid())
            .arg(QDateTime::currentMSecsSinceEpoch())
            .arg(info.suffix().isEmpty() ? QStringLiteral("mkv")
                                          : info.suffix()));
}

QString incrementalChunkFailurePath(const IncrementalRenderChunk& chunk)
{
    const QFileInfo info(chunk.path);
    return info.dir().filePath(
        QStringLiteral("%1.failed.%2")
            .arg(info.completeBaseName())
            .arg(info.suffix().isEmpty() ? QStringLiteral("mkv")
                                          : info.suffix()));
}

bool writeIncrementalChunkFailureDiagnostic(
    const IncrementalRenderChunk& chunk,
    const QString& failedAttemptPath,
    const RenderResult& chunkResult,
    QString* errorMessage)
{
    const QString diagnosticPath = chunk.path + QStringLiteral(".failure.json");
    const QJsonObject diagnostic{
        {QStringLiteral("chunkIndex"), chunk.index},
        {QStringLiteral("chunkFile"), QFileInfo(chunk.path).fileName()},
        {QStringLiteral("failedAttemptFile"),
         QFileInfo(failedAttemptPath).fileName()},
        {QStringLiteral("expectedFrames"),
         static_cast<qint64>(chunk.frameCount)},
        {QStringLiteral("framesRendered"),
         static_cast<qint64>(chunkResult.framesRendered)},
        {QStringLiteral("cancelled"), chunkResult.cancelled},
        {QStringLiteral("message"), chunkResult.message},
        {QStringLiteral("updatedUtc"),
         QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs)}};
    QSaveFile file(diagnosticPath);
    if (!file.open(QIODevice::WriteOnly) ||
        file.write(QJsonDocument(diagnostic).toJson(QJsonDocument::Indented)) < 0 ||
        !file.commit()) {
        if (errorMessage) {
            *errorMessage =
                QStringLiteral("Failed to write incremental chunk failure "
                               "diagnostic: %1")
                    .arg(file.errorString());
        }
        return false;
    }
    return true;
}

bool publishIncrementalEncodedChunk(
    const IncrementalRenderChunk& chunk,
    const QString& attemptPath,
    const RenderRequest& request,
    QString* errorMessage)
{
    IncrementalRenderChunk attemptChunk = chunk;
    attemptChunk.path = attemptPath;
    if (!hasCompleteIncrementalEncodedChunk(attemptChunk, request)) {
        if (errorMessage) {
            *errorMessage =
                QStringLiteral("Encoded checkpoint chunk %1 produced an "
                               "incomplete temporary file.")
                    .arg(chunk.index + 1);
        }
        return false;
    }
    QFile::remove(chunk.path);
    const QByteArray attemptName = QFile::encodeName(attemptPath);
    const QByteArray chunkName = QFile::encodeName(chunk.path);
    if (std::rename(attemptName.constData(), chunkName.constData()) != 0) {
        if (errorMessage) {
            *errorMessage =
                QStringLiteral("Could not atomically publish checkpoint "
                               "chunk %1: %2")
                    .arg(chunk.index + 1)
                    .arg(QDir::toNativeSeparators(chunk.path));
        }
        return false;
    }
    return true;
}

QVector<ExportRangeSegment> normalizedExportRanges(const RenderRequest& request)
{
    QVector<ExportRangeSegment> ranges = request.exportRanges;
    if (ranges.isEmpty()) {
        const int64_t start = qMax<int64_t>(0, request.exportStartFrame);
        ranges.push_back(
            ExportRangeSegment{start, qMax(start, request.exportEndFrame)});
    }
    std::sort(
        ranges.begin(), ranges.end(),
        [](const ExportRangeSegment& left, const ExportRangeSegment& right) {
            return left.startFrame == right.startFrame
                ? left.endFrame < right.endFrame
                : left.startFrame < right.startFrame;
        });
    return ranges;
}

int64_t outputFramesForRange(const ExportRangeSegment& range,
                             double outputFps,
                             qreal playbackSpeed)
{
    const int64_t start = qMax<int64_t>(0, range.startFrame);
    return jcut::export_timing::outputFrameCountForTimelineRange(
        start,
        qMax(start, range.endFrame),
        outputFps,
        playbackSpeed);
}

QJsonObject trackSignatureJson(const TimelineTrack& track)
{
    return QJsonObject{
        {QStringLiteral("name"), track.name},
        {QStringLiteral("generatedChildTrack"), track.generatedChildTrack},
        {QStringLiteral("parentClipId"), track.parentClipId},
        {QStringLiteral("childClipId"), track.childClipId},
        {QStringLiteral("visualMode"), static_cast<int>(track.visualMode)},
        {QStringLiteral("gradingPreviewEnabled"), track.gradingPreviewEnabled},
        {QStringLiteral("audioEnabled"), track.audioEnabled},
        {QStringLiteral("audioBusId"), track.audioBusId},
        {QStringLiteral("audioGain"), track.audioGain},
        {QStringLiteral("audioMuted"), track.audioMuted},
        {QStringLiteral("audioSolo"), track.audioSolo},
        {QStringLiteral("effectPreset"), static_cast<int>(track.effectPreset)},
        {QStringLiteral("effectRows"), track.effectRows},
        {QStringLiteral("effectSpeed"), track.effectSpeed},
        {QStringLiteral("effectScale"), track.effectScale},
        {QStringLiteral("effectAlternateDirection"),
         track.effectAlternateDirection},
        {QStringLiteral("differenceReferenceFrames"),
         track.differenceReferenceFrames},
        {QStringLiteral("differenceThreshold"), track.differenceThreshold},
        {QStringLiteral("differenceSoftness"), track.differenceSoftness},
        {QStringLiteral("temporalEchoCount"), track.temporalEchoCount},
        {QStringLiteral("temporalEchoSpacingFrames"),
         track.temporalEchoSpacingFrames},
        {QStringLiteral("temporalEchoDecay"), track.temporalEchoDecay},
        {QStringLiteral("tilingPattern"),
         static_cast<int>(track.tilingPattern)},
        {QStringLiteral("tilingSpacing"), track.tilingSpacing},
        {QStringLiteral("tilingWrap"), track.tilingWrap},
        {QStringLiteral("effectParameterSets"), track.effectParameterSets},
    };
}

QByteArray incrementalRenderSignature(const RenderRequest& request,
                                      int chunkFrames)
{
    QJsonObject root{
        {QStringLiteral("schema"), kIncrementalRenderSchema},
        {QStringLiteral("outputFormat"), request.outputFormat},
        {QStringLiteral("width"), request.outputSize.width()},
        {QStringLiteral("height"), request.outputSize.height()},
        {QStringLiteral("outputFps"), request.outputFps},
        {QStringLiteral("playbackSpeed"), request.playbackSpeed},
        {QStringLiteral("useProxyMedia"), request.useProxyMedia},
        {QStringLiteral("bypassGrading"), request.bypassGrading},
        {QStringLiteral("correctionsEnabled"), request.correctionsEnabled},
        {QStringLiteral("checkpointMode"),
         QStringLiteral("encoded-chunk")},
        {QStringLiteral("showCurrentSpeakerName"),
         request.showCurrentSpeakerName},
        {QStringLiteral("showCurrentSpeakerOrganization"),
         request.showCurrentSpeakerOrganization},
        {QStringLiteral("transcriptPrependMs"), request.transcriptPrependMs},
        {QStringLiteral("transcriptPostpendMs"), request.transcriptPostpendMs},
        {QStringLiteral("transcriptOffsetMs"), request.transcriptOffsetMs},
        {QStringLiteral("frameTransitionMode"),
         static_cast<int>(request.playbackTiming.frameTransitionMode)},
        {QStringLiteral("frameCrossfadeEnabled"),
         request.playbackTiming.frameCrossfadeEnabled},
        {QStringLiteral("frameCrossfadeFrames"),
         request.playbackTiming.frameCrossfadeFrames},
        {QStringLiteral("chunkFrames"), chunkFrames},
    };

    QJsonArray clips;
    QSet<QString> sourcePaths;
    for (const TimelineClip& clip : request.clips) {
        QJsonObject clipSignature = editor::clipToJson(clip);
        // Health-check bookkeeping changes every time a project is opened,
        // but it cannot change a rendered pixel or sample.
        clipSignature.remove(QStringLiteral("audioSourceLastVerifiedMs"));
        clips.push_back(clipSignature);
        for (const QString& path :
             {clip.filePath, clip.proxyPath, clip.audioSourcePath,
              clip.maskFramesDir}) {
            if (!path.trimmed().isEmpty()) {
                sourcePaths.insert(QFileInfo(path).absoluteFilePath());
            }
        }
    }
    root.insert(QStringLiteral("clips"), clips);

    QJsonArray tracks;
    for (const TimelineTrack& track : request.tracks) {
        tracks.push_back(trackSignatureJson(track));
    }
    root.insert(QStringLiteral("tracks"), tracks);

    QJsonArray ranges;
    for (const ExportRangeSegment& range : normalizedExportRanges(request)) {
        ranges.push_back(QJsonObject{
            {QStringLiteral("start"), static_cast<qint64>(range.startFrame)},
            {QStringLiteral("end"), static_cast<qint64>(range.endFrame)}});
    }
    root.insert(QStringLiteral("ranges"), ranges);

    QJsonArray playbackRanges;
    for (const ExportRangeSegment& range :
         request.playbackTiming.playbackRanges) {
        playbackRanges.push_back(QJsonObject{
            {QStringLiteral("start"), static_cast<qint64>(range.startFrame)},
            {QStringLiteral("end"), static_cast<qint64>(range.endFrame)}});
    }
    root.insert(QStringLiteral("playbackRanges"), playbackRanges);

    QJsonArray markers;
    for (const RenderSyncMarker& marker : request.renderSyncMarkers) {
        markers.push_back(QJsonObject{
            {QStringLiteral("clipId"), marker.clipId},
            {QStringLiteral("frame"), static_cast<qint64>(marker.frame)},
            {QStringLiteral("action"), static_cast<int>(marker.action)},
            {QStringLiteral("count"), marker.count}});
    }
    root.insert(QStringLiteral("renderSyncMarkers"), markers);

    QStringList sortedPaths(sourcePaths.begin(), sourcePaths.end());
    std::sort(sortedPaths.begin(), sortedPaths.end());
    QJsonArray sources;
    for (const QString& path : std::as_const(sortedPaths)) {
        const QFileInfo info(path);
        sources.push_back(QJsonObject{
            {QStringLiteral("path"), path},
            {QStringLiteral("exists"), info.exists()},
            {QStringLiteral("size"), static_cast<qint64>(info.size())},
            {QStringLiteral("modifiedMs"),
             static_cast<qint64>(info.lastModified().toMSecsSinceEpoch())}});
    }
    root.insert(QStringLiteral("sources"), sources);

    return QCryptographicHash::hash(
               QJsonDocument(root).toJson(QJsonDocument::Compact),
               QCryptographicHash::Sha256)
        .toHex();
}

QVector<IncrementalRenderChunk> buildIncrementalChunks(
    const RenderRequest& request,
    const QString& cacheDirectory,
    int targetFrames)
{
    const double outputFps =
        jcut::export_timing::normalizedOutputFps(request.outputFps);
    const qreal playbackSpeed =
        std::isfinite(request.playbackSpeed) && request.playbackSpeed > 0.001
        ? request.playbackSpeed
        : 1.0;
    QVector<ExportRangeSegment> checkpointRanges;
    for (const ExportRangeSegment& range : normalizedExportRanges(request)) {
        int64_t cursor = qMax<int64_t>(0, range.startFrame);
        const int64_t rangeEnd = qMax(cursor, range.endFrame);
        while (cursor <= rangeEnd) {
            int64_t low = cursor;
            int64_t high = rangeEnd;
            int64_t checkpointEnd = cursor;
            while (low <= high) {
                const int64_t candidate =
                    low + ((high - low) / 2);
                const int64_t candidateFrames =
                    outputFramesForRange(
                        ExportRangeSegment{cursor, candidate},
                        outputFps,
                        playbackSpeed);
                if (candidateFrames <= targetFrames ||
                    candidate == cursor) {
                    checkpointEnd = candidate;
                    low = candidate + 1;
                } else {
                    high = candidate - 1;
                }
            }
            checkpointRanges.push_back(
                ExportRangeSegment{cursor, checkpointEnd});
            if (checkpointEnd == std::numeric_limits<int64_t>::max()) {
                break;
            }
            cursor = checkpointEnd + 1;
        }
    }

    QVector<IncrementalRenderChunk> chunks;
    IncrementalRenderChunk current;
    for (const ExportRangeSegment& range : checkpointRanges) {
        const int64_t rangeFrames =
            outputFramesForRange(range, outputFps, playbackSpeed);
        if (!current.ranges.isEmpty() &&
            current.frameCount + rangeFrames > targetFrames) {
            current.index = chunks.size();
            current.path = QDir(cacheDirectory).filePath(
                QStringLiteral("chunk_%1.mkv")
                    .arg(current.index, 5, 10, QLatin1Char('0')));
            chunks.push_back(current);
            current = {};
        }
        current.ranges.push_back(range);
        current.frameCount += rangeFrames;
        if (current.frameCount >= targetFrames) {
            current.index = chunks.size();
            current.path = QDir(cacheDirectory).filePath(
                QStringLiteral("chunk_%1.mkv")
                    .arg(current.index, 5, 10, QLatin1Char('0')));
            chunks.push_back(current);
            current = {};
        }
    }
    if (!current.ranges.isEmpty()) {
        current.index = chunks.size();
        current.path = QDir(cacheDirectory).filePath(
            QStringLiteral("chunk_%1.mkv")
                .arg(current.index, 5, 10, QLatin1Char('0')));
        chunks.push_back(current);
    }
    return chunks;
}

QJsonArray rangesToJson(const QVector<ExportRangeSegment>& ranges)
{
    QJsonArray array;
    for (const ExportRangeSegment& range : ranges) {
        array.push_back(QJsonObject{
            {QStringLiteral("start"), static_cast<qint64>(range.startFrame)},
            {QStringLiteral("end"), static_cast<qint64>(range.endFrame)}});
    }
    return array;
}

bool writeIncrementalManifest(
    const QString& manifestPath,
    const QByteArray& signature,
    int targetFrames,
    int64_t totalFrames,
    const QVector<IncrementalRenderChunk>& chunks,
    const QSet<int>& completedChunks,
    const RenderResult& aggregate,
    int failedChunkIndex,
    const QString& failedMessage,
    QString* errorMessage)
{
    QJsonArray chunkArray;
    for (const IncrementalRenderChunk& chunk : chunks) {
        chunkArray.push_back(QJsonObject{
            {QStringLiteral("index"), chunk.index},
            {QStringLiteral("file"), QFileInfo(chunk.path).fileName()},
            {QStringLiteral("frames"), static_cast<qint64>(chunk.frameCount)},
            {QStringLiteral("ranges"), rangesToJson(chunk.ranges)},
            {QStringLiteral("complete"),
             completedChunks.contains(chunk.index)}});
    }
    QJsonObject manifest{
        {QStringLiteral("schema"), kIncrementalRenderSchema},
        {QStringLiteral("signature"), QString::fromLatin1(signature)},
        {QStringLiteral("chunkFrames"), targetFrames},
        {QStringLiteral("totalFrames"), static_cast<qint64>(totalFrames)},
        {QStringLiteral("updatedUtc"),
         QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs)},
        {QStringLiteral("usedGpu"), aggregate.usedGpu},
        {QStringLiteral("usedHardwareEncode"),
         aggregate.usedHardwareEncode},
        {QStringLiteral("encoderLabel"), aggregate.encoderLabel},
        {QStringLiteral("exportPipeline"), aggregate.exportPipeline},
        {QStringLiteral("chunks"), chunkArray},
    };
    if (failedChunkIndex >= 0 || !failedMessage.trimmed().isEmpty()) {
        manifest.insert(
            QStringLiteral("lastFailure"),
            QJsonObject{
                {QStringLiteral("chunkIndex"), failedChunkIndex},
                {QStringLiteral("message"), failedMessage.trimmed()},
                {QStringLiteral("updatedUtc"),
                 QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs)}});
    }
    QSaveFile file(manifestPath);
    if (!file.open(QIODevice::WriteOnly) ||
        file.write(QJsonDocument(manifest).toJson(QJsonDocument::Indented)) < 0 ||
        !file.commit()) {
        if (errorMessage) {
            *errorMessage =
                QStringLiteral("Failed to atomically write incremental render "
                               "manifest: %1")
                    .arg(file.errorString());
        }
        return false;
    }
    return true;
}

QSet<int> reusableChunksFromManifest(
    const QString& manifestPath,
    const QByteArray& signature,
    const QVector<IncrementalRenderChunk>& chunks,
    const RenderRequest& request,
    RenderResult* aggregate)
{
    QFile file(manifestPath);
    if (!file.open(QIODevice::ReadOnly)) {
        return {};
    }
    QJsonParseError parseError;
    const QJsonDocument document =
        QJsonDocument::fromJson(file.readAll(), &parseError);
    if (parseError.error != QJsonParseError::NoError ||
        !document.isObject()) {
        return {};
    }
    const QJsonObject root = document.object();
    if (root.value(QStringLiteral("schema")).toInt() !=
            kIncrementalRenderSchema ||
        root.value(QStringLiteral("signature")).toString() !=
            QString::fromLatin1(signature)) {
        return {};
    }
    if (aggregate) {
        aggregate->usedGpu =
            root.value(QStringLiteral("usedGpu")).toBool();
        aggregate->usedHardwareEncode =
            root.value(QStringLiteral("usedHardwareEncode")).toBool();
        aggregate->encoderLabel =
            root.value(QStringLiteral("encoderLabel")).toString();
        aggregate->exportPipeline =
            root.value(QStringLiteral("exportPipeline")).toString();
    }
    QSet<int> reusable;
    const QJsonArray entries = root.value(QStringLiteral("chunks")).toArray();
    for (const QJsonValue& value : entries) {
        const QJsonObject entry = value.toObject();
        const int index = entry.value(QStringLiteral("index")).toInt(-1);
        if (index < 0 || index >= chunks.size() ||
            !entry.value(QStringLiteral("complete")).toBool() ||
            entry.value(QStringLiteral("frames")).toVariant().toLongLong() !=
                chunks.at(index).frameCount) {
            continue;
        }
        if (hasCompleteIncrementalEncodedChunk(chunks.at(index), request)) {
            reusable.insert(index);
        }
    }
    return reusable;
}

void accumulateIncrementalResult(RenderResult* aggregate,
                                 const RenderResult& chunk)
{
    if (!aggregate) {
        return;
    }
    aggregate->usedGpu = aggregate->usedGpu || chunk.usedGpu;
    aggregate->usedHardwareEncode =
        aggregate->usedHardwareEncode || chunk.usedHardwareEncode;
    aggregate->encoderLabel = chunk.encoderLabel;
    aggregate->exportPipeline = chunk.exportPipeline;
    aggregate->gpuTransferLabel = chunk.gpuTransferLabel;
    aggregate->encoderPixelFormat = chunk.encoderPixelFormat;
    aggregate->encoderSoftwarePixelFormat =
        chunk.encoderSoftwarePixelFormat;
    aggregate->cudaExternalMemoryStatus =
        chunk.cudaExternalMemoryStatus;
    aggregate->exportPathFallbackReason =
        chunk.exportPathFallbackReason;
    aggregate->cudaExternalTransfer =
        aggregate->cudaExternalTransfer || chunk.cudaExternalTransfer;
    aggregate->cudaExternalMemorySupported =
        aggregate->cudaExternalMemorySupported ||
        chunk.cudaExternalMemorySupported;
    aggregate->encoderHardwareFrames =
        aggregate->encoderHardwareFrames || chunk.encoderHardwareFrames;
    aggregate->requestedRenderBackend = chunk.requestedRenderBackend;
    aggregate->effectiveRenderBackend = chunk.effectiveRenderBackend;
    aggregate->renderStageMs += chunk.renderStageMs;
    aggregate->renderDecodeStageMs += chunk.renderDecodeStageMs;
    aggregate->renderTextureStageMs += chunk.renderTextureStageMs;
    aggregate->renderCompositeStageMs += chunk.renderCompositeStageMs;
    aggregate->renderNv12StageMs += chunk.renderNv12StageMs;
    aggregate->gpuReadbackMs += chunk.gpuReadbackMs;
    aggregate->overlayStageMs += chunk.overlayStageMs;
    aggregate->convertStageMs += chunk.convertStageMs;
    aggregate->encodeStageMs += chunk.encodeStageMs;
    aggregate->audioStageMs += chunk.audioStageMs;
    aggregate->audioSetupMs += chunk.audioSetupMs;
    aggregate->maxFrameRenderStageMs =
        qMax(aggregate->maxFrameRenderStageMs,
             chunk.maxFrameRenderStageMs);
    aggregate->maxFrameDecodeStageMs =
        qMax(aggregate->maxFrameDecodeStageMs,
             chunk.maxFrameDecodeStageMs);
    aggregate->maxFrameTextureStageMs =
        qMax(aggregate->maxFrameTextureStageMs,
             chunk.maxFrameTextureStageMs);
    aggregate->maxFrameReadbackStageMs =
        qMax(aggregate->maxFrameReadbackStageMs,
             chunk.maxFrameReadbackStageMs);
    aggregate->maxFrameConvertStageMs =
        qMax(aggregate->maxFrameConvertStageMs,
             chunk.maxFrameConvertStageMs);
    aggregate->skippedClips = chunk.skippedClips;
    aggregate->skippedClipReasonCounts =
        chunk.skippedClipReasonCounts;
    aggregate->renderStageTable = chunk.renderStageTable;
    aggregate->worstFrameTable = chunk.worstFrameTable;
    aggregate->exportFaceTransformDiagnostics =
        chunk.exportFaceTransformDiagnostics;
}

bool assembleIncrementalChunks(
    const RenderRequest& request,
    const QVector<IncrementalRenderChunk>& chunks,
    const QString& cacheDirectory,
    QString* errorMessage)
{
    const QString ffmpeg =
        QStandardPaths::findExecutable(QStringLiteral("ffmpeg"));
    if (ffmpeg.isEmpty()) {
        if (errorMessage) {
            *errorMessage =
                QStringLiteral("Incremental export requires the ffmpeg "
                               "executable for final lossless remuxing.");
        }
        return false;
    }
    const QString concatPath =
        QDir(cacheDirectory).filePath(QStringLiteral("concat.txt"));
    QSaveFile concatFile(concatPath);
    if (!concatFile.open(QIODevice::WriteOnly | QIODevice::Text)) {
        if (errorMessage) {
            *errorMessage = concatFile.errorString();
        }
        return false;
    }
    for (const IncrementalRenderChunk& chunk : chunks) {
        QString escaped = QFileInfo(chunk.path).absoluteFilePath();
        escaped.replace(QLatin1Char('\''), QStringLiteral("'\\''"));
        concatFile.write(
            QStringLiteral("file '%1'\n").arg(escaped).toUtf8());
    }
    if (!concatFile.commit()) {
        if (errorMessage) {
            *errorMessage = concatFile.errorString();
        }
        return false;
    }

    for (const IncrementalRenderChunk& chunk : chunks) {
        if (!hasCompleteIncrementalEncodedChunk(chunk, request)) {
            if (errorMessage) {
                *errorMessage =
                    QStringLiteral("Encoded checkpoint chunk %1 is "
                                   "missing or corrupt.")
                        .arg(chunk.index + 1);
            }
            return false;
        }
    }

    const QFileInfo outputInfo(request.outputPath);
    const QString assemblingPath = outputInfo.dir().filePath(
        outputInfo.completeBaseName() +
        QStringLiteral(".assembling.") + outputInfo.suffix());
    QFile::remove(assemblingPath);
    QStringList arguments{
        QStringLiteral("-hide_banner"),
        QStringLiteral("-loglevel"), QStringLiteral("error"),
        QStringLiteral("-f"), QStringLiteral("concat"),
        QStringLiteral("-safe"), QStringLiteral("0"),
        QStringLiteral("-i"), concatPath,
        QStringLiteral("-map"), QStringLiteral("0:v:0"),
        QStringLiteral("-map"), QStringLiteral("0:a:0?"),
        QStringLiteral("-c:v"), QStringLiteral("copy"),
    };
    const QString suffix = outputInfo.suffix().toLower();
    if (suffix == QStringLiteral("webm")) {
        arguments << QStringLiteral("-c:a") << QStringLiteral("libopus");
    } else {
        arguments << QStringLiteral("-c:a") << QStringLiteral("aac")
                  << QStringLiteral("-b:a") << QStringLiteral("192k");
    }
    if (suffix == QStringLiteral("mp4") ||
        suffix == QStringLiteral("mov")) {
        arguments << QStringLiteral("-movflags")
                  << QStringLiteral("+faststart");
    }
    arguments << QStringLiteral("-y") << assemblingPath;

    QProcess process;
    process.setProgram(ffmpeg);
    process.setArguments(arguments);
    process.setProcessChannelMode(QProcess::MergedChannels);
    process.start();
    if (!process.waitForStarted() ||
        !process.waitForFinished(-1) ||
        process.exitStatus() != QProcess::NormalExit ||
        process.exitCode() != 0 ||
        QFileInfo(assemblingPath).size() <= 1024) {
        if (errorMessage) {
            *errorMessage =
                QStringLiteral("Failed to assemble incremental render: %1")
                    .arg(QString::fromUtf8(process.readAll()).trimmed());
        }
        return false;
    }
    const QByteArray assemblingName = QFile::encodeName(assemblingPath);
    const QByteArray outputName = QFile::encodeName(request.outputPath);
    if (std::rename(assemblingName.constData(), outputName.constData()) != 0) {
        if (errorMessage) {
            *errorMessage =
                QStringLiteral("Could not atomically publish assembled "
                               "output: %1")
                    .arg(QDir::toNativeSeparators(request.outputPath));
        }
        return false;
    }
    return true;
}

} // namespace

QString incrementalRenderCacheRootForOutputPath(const QString& outputPath)
{
    const QFileInfo outputInfo(outputPath);
    return outputInfo.dir().filePath(
        outputInfo.completeBaseName() +
        QStringLiteral(".jcut-render-cache"));
}

RenderResult renderTimelineToFile(
    const RenderRequest& request,
    const std::function<bool(const RenderProgress&)>& progressCallback)
{
    if (!request.incrementalExport) {
        return renderTimelineSingleFile(request, progressCallback);
    }

    RenderResult result;
    QElapsedTimer totalTimer;
    totalTimer.start();
    const int targetFrames =
        qBound(60, request.incrementalChunkFrames, 18000);
    const QByteArray signature =
        incrementalRenderSignature(request, targetFrames);
    const QString cacheRoot =
        incrementalRenderCacheRootForOutputPath(request.outputPath);
    const QString cacheDirectory =
        QDir(cacheRoot).filePath(QString::fromLatin1(signature.left(16)));
    if (!QDir().mkpath(cacheDirectory)) {
        result.message =
            QStringLiteral("Failed to create incremental render cache: %1")
                .arg(QDir::toNativeSeparators(cacheDirectory));
        return result;
    }
    const QVector<IncrementalRenderChunk> chunks =
        buildIncrementalChunks(request, cacheDirectory, targetFrames);
    if (chunks.isEmpty()) {
        result.message = QStringLiteral("Incremental render has no frames.");
        return result;
    }
    int64_t totalFrames = 0;
    for (const IncrementalRenderChunk& chunk : chunks) {
        totalFrames += chunk.frameCount;
    }
    const QString manifestPath =
        QDir(cacheDirectory).filePath(QStringLiteral("manifest.json"));
    QSet<int> completed =
        reusableChunksFromManifest(
            manifestPath, signature, chunks, request, &result);
    int64_t reusedFrames = 0;
    for (const IncrementalRenderChunk& chunk : chunks) {
        if (completed.contains(chunk.index)) {
            reusedFrames += chunk.frameCount;
        }
    }
    int64_t completedFrames = 0;
    result.incrementalChunksTotal = chunks.size();
    result.incrementalChunksCompleted = completed.size();
    result.incrementalFramesReused = reusedFrames;
    result.incrementalCachePath = cacheDirectory;

    QString manifestError;
    if (!writeIncrementalManifest(
            manifestPath, signature, targetFrames, totalFrames, chunks,
            completed, result, -1, QString(), &manifestError)) {
        result.message = manifestError;
        return result;
    }

    struct IncrementalRendererSession {
        std::unique_ptr<HeadlessVulkanCompositor> renderer;
        bool previewEnabled = false;
        const std::function<bool(const RenderProgress&)>* progress = nullptr;

        void finish()
        {
            if (!renderer) {
                return;
            }
            if (previewEnabled) {
                renderer->finishGpuPreviewPublication();
                if (progress && *progress) {
                    RenderProgress releasePreview;
                    releasePreview.releaseGpuPreview = true;
                    (*progress)(releasePreview);
                }
            }
            renderer.reset();
        }

        ~IncrementalRendererSession()
        {
            finish();
        }
    } rendererSession{
        nullptr, request.gpuExportPreviewEnabled, &progressCallback};

    for (const IncrementalRenderChunk& chunk : chunks) {
        if (completed.contains(chunk.index)) {
            completedFrames += chunk.frameCount;
            if (progressCallback) {
                RenderProgress progress;
                progress.framesCompleted = completedFrames;
                progress.totalFrames = totalFrames;
                progress.segmentIndex = completed.size();
                progress.segmentCount = chunks.size();
                progress.timelineFrame = chunk.ranges.constLast().endFrame;
                progress.incrementalChunksCompleted = completed.size();
                progress.incrementalChunksTotal = chunks.size();
                progress.incrementalFramesReused = reusedFrames;
                progress.incrementalCachePath = cacheDirectory;
                progress.elapsedMs = totalTimer.elapsed();
                const int64_t workFrames =
                    qMax<int64_t>(
                        0, completedFrames - reusedFrames);
                progress.estimatedRemainingMs =
                    workFrames > 0
                    ? (progress.elapsedMs *
                       qMax<int64_t>(0, totalFrames - completedFrames)) /
                          workFrames
                    : -1;
                if (!progressCallback(progress)) {
                    result.cancelled = true;
                    result.framesRendered = completedFrames;
                    result.elapsedMs = totalTimer.elapsed();
                    result.message = QStringLiteral("Render cancelled.");
                    return result;
                }
            }
            continue;
        }

        RenderRequest chunkRequest = request;
        chunkRequest.incrementalExport = false;
        chunkRequest.losslessIntermediateAudio = true;
        chunkRequest.createVideoFromImageSequence = false;
        chunkRequest.imageSequenceFormat.clear();
        const QString attemptPath = incrementalChunkAttemptPath(chunk);
        QFile::remove(attemptPath);
        chunkRequest.outputPath = attemptPath;
        chunkRequest.exportRanges = chunk.ranges;
        chunkRequest.exportStartFrame =
            chunk.ranges.constFirst().startFrame;
        chunkRequest.exportEndFrame =
            chunk.ranges.constLast().endFrame;
        if (!rendererSession.renderer) {
            if (desiredRenderBackendFromEnvironment() !=
                RenderBackend::Vulkan) {
                result.message =
                    QStringLiteral(
                        "Incremental export requires the Vulkan renderer.");
                return result;
            }
            rendererSession.renderer =
                std::make_unique<OffscreenVulkanRenderer>();
            QString rendererError;
            if (!rendererSession.renderer->initialize(
                    request.outputSize, &rendererError)) {
                result.message = rendererError.trimmed().isEmpty()
                    ? QStringLiteral(
                          "Vulkan export renderer initialization failed.")
                    : rendererError.trimmed();
                return result;
            }
            qInfo().noquote()
                << QStringLiteral(
                       "[render-incremental] persistent Vulkan compositor "
                       "initialized once for %1 pending chunk(s)")
                       .arg(chunks.size() - completed.size());
        }
        const int64_t framesBeforeChunk = completedFrames;
        const RenderResult chunkResult = renderTimelineSingleFile(
            chunkRequest,
            [&, framesBeforeChunk](const RenderProgress& localProgress) {
                if (!progressCallback) {
                    return true;
                }
                if (localProgress.releaseGpuPreview) {
                    return progressCallback(localProgress);
                }
                RenderProgress progress = localProgress;
                progress.framesCompleted =
                    framesBeforeChunk + localProgress.framesCompleted;
                progress.totalFrames = totalFrames;
                progress.segmentIndex = chunk.index + 1;
                progress.segmentCount = chunks.size();
                progress.incrementalChunksCompleted = completed.size();
                progress.incrementalChunksTotal = chunks.size();
                progress.incrementalFramesReused = reusedFrames;
                progress.incrementalCachePath = cacheDirectory;
                progress.elapsedMs = totalTimer.elapsed();
                const int64_t workFrames =
                    qMax<int64_t>(
                        0, progress.framesCompleted - reusedFrames);
                progress.estimatedRemainingMs =
                    workFrames > 0
                    ? (progress.elapsedMs *
                       qMax<int64_t>(
                           0, totalFrames - progress.framesCompleted)) /
                          workFrames
                    : -1;
                return progressCallback(progress);
            },
            rendererSession.renderer.get());
        accumulateIncrementalResult(&result, chunkResult);
        if (!chunkResult.success) {
            const QString failedAttemptPath = incrementalChunkFailurePath(chunk);
            QFile::remove(failedAttemptPath);
            if (QFileInfo(attemptPath).isFile()) {
                QFile::rename(attemptPath, failedAttemptPath);
            }
            result.success = false;
            result.cancelled = chunkResult.cancelled;
            result.framesRendered =
                framesBeforeChunk + chunkResult.framesRendered;
            result.elapsedMs = totalTimer.elapsed();
            result.message =
                QStringLiteral("Incremental chunk %1/%2 failed: %3")
                    .arg(chunk.index + 1)
                    .arg(chunks.size())
                    .arg(chunkResult.message);
            QString failureDiagnosticError;
            writeIncrementalChunkFailureDiagnostic(
                chunk, failedAttemptPath, chunkResult,
                &failureDiagnosticError);
            QString failureManifestError;
            writeIncrementalManifest(
                manifestPath, signature, targetFrames, totalFrames, chunks,
                completed, result, chunk.index, result.message,
                &failureManifestError);
            return result;
        }
        QString publishError;
        if (!publishIncrementalEncodedChunk(
                chunk, attemptPath, request, &publishError)) {
            const QString failedAttemptPath = incrementalChunkFailurePath(chunk);
            QFile::remove(failedAttemptPath);
            if (QFileInfo(attemptPath).isFile()) {
                QFile::rename(attemptPath, failedAttemptPath);
            }
            result.success = false;
            result.cancelled = false;
            result.framesRendered =
                framesBeforeChunk + chunkResult.framesRendered;
            result.elapsedMs = totalTimer.elapsed();
            result.message =
                QStringLiteral("Incremental chunk %1/%2 failed validation: %3")
                    .arg(chunk.index + 1)
                    .arg(chunks.size())
                    .arg(publishError);
            QString failureDiagnosticError;
            writeIncrementalChunkFailureDiagnostic(
                chunk, failedAttemptPath, result,
                &failureDiagnosticError);
            QString failureManifestError;
            writeIncrementalManifest(
                manifestPath, signature, targetFrames, totalFrames, chunks,
                completed, result, chunk.index, result.message,
                &failureManifestError);
            return result;
        }
        QFile::remove(chunk.path + QStringLiteral(".failure.json"));
        completedFrames += chunk.frameCount;
        completed.insert(chunk.index);
        result.incrementalChunksCompleted = completed.size();
        if (!writeIncrementalManifest(
                manifestPath, signature, targetFrames, totalFrames, chunks,
                completed, result, -1, QString(), &manifestError)) {
            result.message = manifestError;
            result.framesRendered = completedFrames;
            result.elapsedMs = totalTimer.elapsed();
            return result;
        }
    }

    rendererSession.finish();
    QString assemblyError;
    if (!assembleIncrementalChunks(
            request, chunks, cacheDirectory, &assemblyError)) {
        result.message = assemblyError;
        result.framesRendered = completedFrames;
        result.elapsedMs = totalTimer.elapsed();
        return result;
    }
    result.success = true;
    result.framesRendered = totalFrames;
    result.elapsedMs = totalTimer.elapsed();
    result.incrementalChunksCompleted = chunks.size();
    result.message =
        QStringLiteral("Rendered %1 frames to %2 from %3 resumable "
                       "image-sequence checkpoints (%4 frames reused).")
            .arg(totalFrames)
            .arg(QDir::toNativeSeparators(request.outputPath))
            .arg(chunks.size())
            .arg(reusedFrames);
    return result;
}
