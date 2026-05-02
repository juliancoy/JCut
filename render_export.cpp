#include "render_internal.h"
#include "render_backend.h"
#include "vulkan_backend.h"

#include <QDebug>
#include <QDir>
#include <QFile>
#include <QFileInfo>
using namespace render_detail;

namespace {

bool configureCudaHardwareFrames(AVCodecContext* codecCtx,
                                 AVPixelFormat swFormat,
                                 QString* errorMessage)
{
    if (!codecCtx) {
        return false;
    }
    AVBufferRef* deviceRef = nullptr;
    int ret = av_hwdevice_ctx_create(&deviceRef, AV_HWDEVICE_TYPE_CUDA, nullptr, nullptr, 0);
    if (ret < 0) {
        if (errorMessage) {
            *errorMessage = render_detail::avErrToString(ret);
        }
        return false;
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

    ret = av_hwframe_ctx_init(framesRef);
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
    ctx->codec_id = codec->id;
    ctx->codec_type = AVMEDIA_TYPE_VIDEO;
    ctx->width = request.outputSize.width();
    ctx->height = request.outputSize.height();
    ctx->time_base = AVRational{1, kTimelineFps};
    ctx->framerate = AVRational{kTimelineFps, 1};
    ctx->gop_size = kTimelineFps;
    ctx->max_b_frames = 0;
    ctx->pix_fmt = pixelFormat;
    ctx->bit_rate = 8'000'000;

    if (formatCtx->oformat->flags & AVFMT_GLOBALHEADER) {
        ctx->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;
    }
}

} // namespace

RenderResult renderTimelineToFile(const RenderRequest& request,
                                  const std::function<bool(const RenderProgress&)>& progressCallback) {
    RenderResult result;
    const RenderBackend requestedBackend = desiredRenderBackendFromEnvironment();
    result.requestedRenderBackend = renderBackendName(requestedBackend);
    result.effectiveRenderBackend = QStringLiteral("none");
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

    // Create a directory with the same name as the output file (without extension)
    // for intermediate files or related assets
    const QString baseName = outputFileInfo.completeBaseName();
    QString namedDirPath;
    if (!baseName.isEmpty()) {
        namedDirPath = outputDir.filePath(baseName);
        QDir namedDir(namedDirPath);
        if (!namedDir.exists() && !namedDir.mkpath(".")) {
            result.message = QStringLiteral("Failed to create named output directory: %1").arg(namedDirPath);
            return result;
        }
    }

    struct ScopedRenderDecodeSafety {
        bool preferHardware = false;
        editor::DecodePreference previousDecodePreference = editor::debugDecodePreference();
        editor::H26xSoftwareThreadingMode previousThreadingMode =
            editor::debugH26xSoftwareThreadingMode();
        bool previousDeterministic = editor::debugDeterministicPipelineEnabled();
        explicit ScopedRenderDecodeSafety(bool preferHardwareDecode)
            : preferHardware(preferHardwareDecode) {
            const bool enableExperimentalHardwareDecode =
                preferHardware &&
                qEnvironmentVariableIntValue("JCUT_VULKAN_HW_DECODE_EXPERIMENTAL") == 1;
            if (enableExperimentalHardwareDecode) {
                editor::setDebugDecodePreference(editor::DecodePreference::Hardware);
                editor::setDebugH26xSoftwareThreadingMode(
                    editor::H26xSoftwareThreadingMode::SingleThread);
                editor::setDebugDeterministicPipelineEnabled(true);
            } else {
                editor::setDebugDecodePreference(editor::DecodePreference::Software);
                editor::setDebugH26xSoftwareThreadingMode(
                    editor::H26xSoftwareThreadingMode::SingleThread);
                editor::setDebugDeterministicPipelineEnabled(true);
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
    int64_t totalFramesToRender = 0;
    for (const ExportRangeSegment& range : exportRanges) {
        const int64_t exportStart = qMax<int64_t>(0, range.startFrame);
        const int64_t exportEnd = qMax(exportStart, range.endFrame);
        totalFramesToRender += (exportEnd - exportStart + 1);
    }
    const QVector<TimelineClip> orderedClips = sortedVisualClips(request.clips, request.tracks);
    QString gpuInitializationError;

    std::unique_ptr<OffscreenRenderer> activeRenderer;
    if (requestedBackend == RenderBackend::Vulkan) {
        const VulkanAvailabilityResult vulkan = probeVulkanBackendAvailability();
        if (!vulkan.available) {
            result.backendFallbackApplied = true;
            result.backendFallbackReason = vulkan.status;
            result.message = QStringLiteral(
                "Vulkan backend requested for export, but Vulkan is unavailable. "
                "OpenGL fallback is disabled.");
            return result;
        }
        activeRenderer = std::make_unique<OffscreenVulkanRenderer>();
        result.effectiveRenderBackend = QStringLiteral("vulkan");
    } else {
        activeRenderer = std::make_unique<OffscreenGpuRenderer>();
    }

    const bool gpuInitialized = activeRenderer && activeRenderer->initialize(request.outputSize, &gpuInitializationError);
    const bool useGpuRenderer = gpuInitialized;
    if (!useGpuRenderer) {
        result.backendFallbackApplied = true;
        result.backendFallbackReason = gpuInitializationError;
        if (requestedBackend == RenderBackend::Vulkan) {
            result.message = QStringLiteral(
                "Vulkan backend requested for export, but Vulkan renderer initialization failed. "
                "OpenGL fallback is disabled.");
            return result;
        }
        result.effectiveRenderBackend = QStringLiteral("cpu");
    }
    if (useGpuRenderer) {
        result.effectiveRenderBackend = activeRenderer->backendId();
    }
    if (requestedBackend == RenderBackend::Vulkan &&
        qEnvironmentVariable("JCUT_VULKAN_CUDA_EXTERNAL_MEMORY_REQUIRED", QStringLiteral("0")) != QStringLiteral("0") &&
        (!activeRenderer || !activeRenderer->supportsCudaExternalMemoryInterop())) {
        result.backendFallbackApplied = false;
        result.backendFallbackReason = QStringLiteral("Vulkan/CUDA external-memory interop is unavailable.");
        result.message = QStringLiteral(
            "Vulkan/CUDA external-memory interop was required, but the selected Vulkan device "
            "does not expose the required external memory/semaphore FD capability.");
        return result;
    }
    result.usedGpu = useGpuRenderer;

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
            if (configureCudaHardwareFrames(candidateCtx, candidateSwFormat, &cudaError)) {
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
    if (!initializeExportAudio(request, formatCtx, &audioState, &result.message)) {
        avcodec_free_context(&codecCtx);
        avformat_free_context(formatCtx);
        return result;
    }

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

    QHash<QString, QVector<TranscriptSection>> transcriptCache;
    bool hasTranscriptOverlay = false;
    for (const TimelineClip& clip : orderedClips) {
        if (clip.transcriptOverlay.enabled) {
            hasTranscriptOverlay = true;
            break;
        }
    }

    const bool cudaHardwareFrames =
        codecCtx->pix_fmt == AV_PIX_FMT_CUDA && codecCtx->hw_frames_ctx != nullptr;
    const AVPixelFormat encoderInputPixFmt =
        cudaHardwareFrames ? AV_PIX_FMT_NV12 : codecCtx->pix_fmt;
    const bool directNv12Conversion = encoderInputPixFmt == AV_PIX_FMT_NV12;
    const bool vulkanGpuRenderer =
        useGpuRenderer && activeRenderer && activeRenderer->backendId() == QStringLiteral("vulkan");
    const bool vulkanGpuNv12Conversion =
        vulkanGpuRenderer &&
        encoderInputPixFmt == AV_PIX_FMT_NV12 &&
        !hasTranscriptOverlay &&
        !request.createVideoFromImageSequence;
    const bool vulkanCudaExternalTransfer =
        vulkanGpuNv12Conversion &&
        cudaHardwareFrames &&
        activeRenderer &&
        activeRenderer->supportsCudaExternalMemoryInterop() &&
        qEnvironmentVariable("JCUT_VULKAN_CUDA_EXTERNAL_TRANSFER", QStringLiteral("1")) != QStringLiteral("0");
    if (vulkanCudaExternalTransfer) {
        qInfo().noquote() << QStringLiteral(
            "Vulkan export path: direct external-memory NV12 transfer into CUDA encoder frames enabled.");
    }
    const bool vulkanGpuYuv420pConversion =
        vulkanGpuRenderer &&
        encoderInputPixFmt == AV_PIX_FMT_YUV420P &&
        !hasTranscriptOverlay &&
        !request.createVideoFromImageSequence;
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
    constexpr int kAsyncGpuFrameCount = 3;
    constexpr int kAsyncGpuMaxPendingReadbacks = 2;
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
    editor::AsyncDecoder asyncDecoder;
    asyncDecoder.initialize();
    QHash<RenderAsyncFrameKey, editor::FrameHandle> asyncFrameCache;
    QObject::connect(&asyncDecoder,
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
    QString errorMessage;
    QElapsedTimer totalTimer;
    totalTimer.start();
    qint64 totalRenderStageMs = 0;
    qint64 totalRenderDecodeStageMs = 0;
    qint64 totalRenderTextureStageMs = 0;
    qint64 totalRenderCompositeStageMs = 0;
    qint64 totalRenderNv12StageMs = 0;
    qint64 totalGpuReadbackMs = 0;
    qint64 totalOverlayStageMs = 0;
    qint64 totalConvertStageMs = 0;
    qint64 totalEncodeStageMs = 0;
    qint64 totalAudioStageMs = 0;
    qint64 maxFrameRenderStageMs = 0;
    qint64 maxFrameDecodeStageMs = 0;
    qint64 maxFrameTextureStageMs = 0;
    qint64 maxFrameReadbackStageMs = 0;
    qint64 maxFrameConvertStageMs = 0;
    QHash<QString, RenderClipStageStats> clipStageStats;
    QVector<RenderFrameStageStats> worstFrames;
    QJsonArray lastSkippedClips;
    QJsonObject skippedReasonCounts;
    struct PendingAsyncGpuFrame {
        AVFrame* frame = nullptr;
        int64_t timelineFrame = 0;
        int segmentIndex = 0;
        qint64 frameRenderMs = 0;
        qint64 frameDecodeMs = 0;
        qint64 frameTextureMs = 0;
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
        if (!pending.frame ||
            (pending.frame->format != AV_PIX_FMT_CUDA && av_frame_make_writable(pending.frame) < 0)) {
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
        prewarmRenderSequenceSegment(request,
                                     exportStart,
                                     exportEnd,
                                     orderedClips,
                                     &asyncDecoder,
                                     asyncFrameCache);
        for (int64_t timelineFrame = exportStart; timelineFrame <= exportEnd; ++timelineFrame) {
            enqueueRenderSequenceLookahead(request,
                                          timelineFrame,
                                          orderedClips,
                                          &asyncDecoder,
                                          asyncFrameCache);
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
                progress.maxFrameRenderStageMs = maxFrameRenderStageMs;
                progress.maxFrameDecodeStageMs = maxFrameDecodeStageMs;
                progress.maxFrameTextureStageMs = maxFrameTextureStageMs;
                progress.maxFrameReadbackStageMs = maxFrameReadbackStageMs;
                progress.maxFrameConvertStageMs = maxFrameConvertStageMs;
                progress.skippedClips = lastSkippedClips;
                progress.skippedClipReasonCounts = skippedReasonCounts;
                progress.renderStageTable = buildRenderStageTable(clipStageStats, totalRenderStageMs, framesCompleted);
                progress.worstFrameTable = buildWorstFrameTable(worstFrames);
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
            qint64* frameReadbackMsPtr = &frameReadbackMs;
            const bool directGpuFrameReadback =
                useGpuRenderer && !hasTranscriptOverlay && !request.createVideoFromImageSequence;
            if (directGpuFrameReadback) {
                frameReadbackMsPtr = nullptr;
            }
            QJsonArray frameSkippedClips;
            QImage rendered = useGpuRenderer
                ? activeRenderer->renderFrame(request,
                                          timelineFrame,
                                          decoders,
                                          &asyncDecoder,
                                          &asyncFrameCache,
                                          orderedClips,
                                          &clipStageStats,
                                          &frameDecodeMs,
                                          &frameTextureMs,
                                          &frameCompositeMs,
                                          frameReadbackMsPtr,
                                          &frameSkippedClips,
                                          &skippedReasonCounts)
                : renderTimelineFrame(request,
                                      timelineFrame,
                                      decoders,
                                      &asyncDecoder,
                                      &asyncFrameCache,
                                      orderedClips,
                                      &clipStageStats,
                                      &frameSkippedClips,
                                      &skippedReasonCounts);
            lastSkippedClips = frameSkippedClips;
            totalRenderStageMs += renderStageTimer.elapsed();
            totalRenderDecodeStageMs += frameDecodeMs;
            totalRenderTextureStageMs += frameTextureMs;
            totalRenderCompositeStageMs += frameCompositeMs;
            totalGpuReadbackMs += frameReadbackMs;
            if (rendered.isNull() && !directGpuFrameReadback) {
                errorMessage = useGpuRenderer
                    ? QStringLiteral("Failed to render GPU timeline frame %1.").arg(timelineFrame)
                    : QStringLiteral("Failed to render timeline frame %1.").arg(timelineFrame);
                break;
            }

            if ((!directNv12Conversion || hasTranscriptOverlay) && !rendered.isNull()) {
                QElapsedTimer overlayTimer;
                overlayTimer.start();
                renderTranscriptOverlays(&rendered, request, timelineFrame, orderedClips, transcriptCache);
                totalOverlayStageMs += overlayTimer.elapsed();
            }

            // Save intermediate image files if requested
            if (request.createVideoFromImageSequence && !namedDirPath.isEmpty() && !request.imageSequenceFormat.isEmpty()) {
                QString imagePath;
                QString format = request.imageSequenceFormat.toLower();
                QString extension = format;
                
                if (format == "webp") {
                    imagePath = QString("%1/frame_%2.webp").arg(namedDirPath).arg(timelineFrame, 8, 10, QChar('0'));
                } else if (format == "jpeg" || format == "jpg") {
                    imagePath = QString("%1/frame_%2.jpg").arg(namedDirPath).arg(timelineFrame, 8, 10, QChar('0'));
                    format = "JPEG";  // QImage expects "JPEG" not "jpeg"
                } else {
                    qWarning() << "Unsupported image sequence format:" << request.imageSequenceFormat;
                    continue;
                }
                
                // Save the image with optional parallel write disabling
                bool saveSuccess = false;
                if (request.disableParallelImageWrite) {
                    // Force sequential write for debugging
                    saveSuccess = rendered.save(imagePath, format.toUtf8().constData(), 90);
                } else {
                    saveSuccess = rendered.save(imagePath, format.toUtf8().constData(), 90);
                }
                
                if (!saveSuccess) {
                    qWarning() << "Failed to save" << format << "frame:" << timelineFrame 
                               << "to:" << imagePath 
                               << "size:" << rendered.size()
                               << "format:" << rendered.format()
                               << "depth:" << rendered.depth();
                    
                    // Try to get more detailed error information
                    QFile file(imagePath);
                    if (file.open(QIODevice::WriteOnly)) {
                        qWarning() << "File opened successfully, but QImage::save failed";
                        file.close();
                    } else {
                        qWarning() << "Cannot open file for writing:" << file.errorString();
                    }
                }
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
                progress.maxFrameRenderStageMs = maxFrameRenderStageMs;
                progress.maxFrameDecodeStageMs = maxFrameDecodeStageMs;
                progress.maxFrameTextureStageMs = maxFrameTextureStageMs;
                progress.maxFrameReadbackStageMs = maxFrameReadbackStageMs;
                progress.maxFrameConvertStageMs = maxFrameConvertStageMs;
                progress.skippedClips = lastSkippedClips;
                progress.skippedClipReasonCounts = skippedReasonCounts;
                progress.renderStageTable = buildRenderStageTable(clipStageStats, totalRenderStageMs, framesCompleted);
                progress.worstFrameTable = buildWorstFrameTable(worstFrames);
                if (!directNv12Conversion || hasTranscriptOverlay) {
                    progress.previewFrame = rendered;
                }
                if (!progressCallback(progress)) {
                    result.cancelled = true;
                    errorMessage = QStringLiteral("Render cancelled.");
                    break;
                }
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
                    if (gpuFrame->format != AV_PIX_FMT_CUDA && av_frame_make_writable(gpuFrame) < 0) {
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
                if (!gpuNv12Converted && !fillNv12FrameFromImage(rendered, encodedFrame)) {
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
        encodeExportAudio(exportRanges, audioState, formatCtx, &errorMessage);
        totalAudioStageMs += audioTimer.elapsed();
    }

    av_write_trailer(formatCtx);
    asyncDecoder.shutdown();
    qDeleteAll(decoders);
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
    // Release Vulkan/CUDA external-memory imports while FFmpeg's CUDA device context is still alive.
    activeRenderer.reset();
    avcodec_free_context(&codecCtx);
    avformat_free_context(formatCtx);

    if (!errorMessage.isEmpty()) {
        result.message = errorMessage;
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
        result.maxFrameRenderStageMs = maxFrameRenderStageMs;
        result.maxFrameDecodeStageMs = maxFrameDecodeStageMs;
        result.maxFrameTextureStageMs = maxFrameTextureStageMs;
        result.maxFrameReadbackStageMs = maxFrameReadbackStageMs;
        result.maxFrameConvertStageMs = maxFrameConvertStageMs;
        result.skippedClips = lastSkippedClips;
        result.skippedClipReasonCounts = skippedReasonCounts;
        result.renderStageTable = buildRenderStageTable(clipStageStats, totalRenderStageMs, framesCompleted);
        result.worstFrameTable = buildWorstFrameTable(worstFrames);
        return result;
    }

    result.success = true;
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
    result.maxFrameRenderStageMs = maxFrameRenderStageMs;
    result.maxFrameDecodeStageMs = maxFrameDecodeStageMs;
    result.maxFrameTextureStageMs = maxFrameTextureStageMs;
    result.maxFrameReadbackStageMs = maxFrameReadbackStageMs;
    result.maxFrameConvertStageMs = maxFrameConvertStageMs;
    result.skippedClips = lastSkippedClips;
    result.skippedClipReasonCounts = skippedReasonCounts;
    result.renderStageTable = buildRenderStageTable(clipStageStats, totalRenderStageMs, framesCompleted);
    result.worstFrameTable = buildWorstFrameTable(worstFrames);
    QString renderPathSuffix;
    if (useGpuRenderer && activeRenderer) {
        renderPathSuffix = QStringLiteral("\nRender path: %1").arg(activeRenderer->backendId());
    } else if (!useGpuRenderer) {
        renderPathSuffix = QStringLiteral("\nGPU export path unavailable, used CPU fallback: %1")
                               .arg(gpuInitializationError);
    }
    result.message = QStringLiteral("Rendered %1 video frames to %2%3")
                         .arg(framesCompleted)
                         .arg(QDir::toNativeSeparators(request.outputPath))
                         .arg(renderPathSuffix);
    return result;
}
