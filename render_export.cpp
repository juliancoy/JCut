#include "render_internal.h"

#include <QDebug>
#include <QDir>
#include <QFile>
#include <QFileInfo>

using namespace render_detail;

RenderResult renderTimelineToFile(const RenderRequest& request,
                                  const std::function<bool(const RenderProgress&)>& progressCallback) {
    RenderResult result;
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
    OffscreenGpuRenderer gpuRenderer;
    const bool gpuInitialized = gpuRenderer.initialize(request.outputSize, &gpuInitializationError);
    const bool useGpuRenderer = gpuInitialized;
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
            continue;
        }

        AVCodecContext* candidateCtx = avcodec_alloc_context3(candidate);
        if (!candidateCtx) {
            attemptedEncoders.push_back(choice.label + QStringLiteral(" (alloc failed)"));
            continue;
        }

        candidateCtx->codec_id = candidate->id;
        candidateCtx->codec_type = AVMEDIA_TYPE_VIDEO;
        candidateCtx->width = request.outputSize.width();
        candidateCtx->height = request.outputSize.height();
        candidateCtx->time_base = AVRational{1, kTimelineFps};
        candidateCtx->framerate = AVRational{kTimelineFps, 1};
        candidateCtx->gop_size = kTimelineFps;
        candidateCtx->max_b_frames = 0;
        candidateCtx->pix_fmt = choice.pixelFormat == AV_PIX_FMT_NONE
                                    ? pixelFormatForCodec(candidate, request.outputFormat)
                                    : choice.pixelFormat;
        candidateCtx->bit_rate = 8'000'000;

        if (formatCtx->oformat->flags & AVFMT_GLOBALHEADER) {
            candidateCtx->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;
        }

        configureCodecOptions(candidateCtx, request.outputFormat, choice.label);
        if (avcodec_open2(candidateCtx, candidate, nullptr) >= 0) {
            codec = candidate;
            codecCtx = candidateCtx;
            codecLabel = choice.label;
            break;
        }

        attemptedEncoders.push_back(choice.label + QStringLiteral(" (open failed)"));
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

        codecCtx->codec_id = codec->id;
        codecCtx->codec_type = AVMEDIA_TYPE_VIDEO;
        codecCtx->width = request.outputSize.width();
        codecCtx->height = request.outputSize.height();
        codecCtx->time_base = AVRational{1, kTimelineFps};
        codecCtx->framerate = AVRational{kTimelineFps, 1};
        codecCtx->gop_size = kTimelineFps;
        codecCtx->max_b_frames = 0;
        codecCtx->pix_fmt = pixelFormatForCodec(codec, request.outputFormat);
        codecCtx->bit_rate = 8'000'000;

        if (formatCtx->oformat->flags & AVFMT_GLOBALHEADER) {
            codecCtx->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;
        }

        configureCodecOptions(codecCtx, request.outputFormat, codecLabel);

        if (avcodec_open2(codecCtx, codec, nullptr) < 0) {
            avcodec_free_context(&codecCtx);
            avformat_free_context(formatCtx);
            result.message = QStringLiteral("Failed to open encoder %1.").arg(codecLabel);
            return result;
        }
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

    const bool directNv12Conversion = codecCtx->pix_fmt == AV_PIX_FMT_NV12;
    SwsContext* swsCtx = nullptr;
    if (!directNv12Conversion) {
        swsCtx = sws_getContext(codecCtx->width,
                                codecCtx->height,
                                AV_PIX_FMT_BGRA,
                                codecCtx->width,
                                codecCtx->height,
                                codecCtx->pix_fmt,
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

    AVFrame* sourceFrame = directNv12Conversion ? nullptr : av_frame_alloc();
    AVFrame* encodedFrame = av_frame_alloc();
    if ((!directNv12Conversion && !sourceFrame) || !encodedFrame) {
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
    encodedFrame->format = codecCtx->pix_fmt;
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
    QHash<QString, QVector<TranscriptSection>> transcriptCache;
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
            QJsonArray frameSkippedClips;
            QImage rendered = useGpuRenderer
                ? gpuRenderer.renderFrame(request,
                                          timelineFrame,
                                          decoders,
                                          &asyncDecoder,
                                          &asyncFrameCache,
                                          orderedClips,
                                          &clipStageStats,
                                          &frameDecodeMs,
                                          &frameTextureMs,
                                          &frameCompositeMs,
                                          &frameReadbackMs,
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
            if (rendered.isNull()) {
                errorMessage = useGpuRenderer
                    ? QStringLiteral("Failed to render GPU timeline frame %1.").arg(timelineFrame)
                    : QStringLiteral("Failed to render timeline frame %1.").arg(timelineFrame);
                break;
            }

            QElapsedTimer overlayTimer;
            overlayTimer.start();
            renderTranscriptOverlays(&rendered, request, timelineFrame, orderedClips, transcriptCache);
            totalOverlayStageMs += overlayTimer.elapsed();

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
                progress.previewFrame = rendered;
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
                const bool gpuNv12Converted =
                    useGpuRenderer && gpuRenderer.convertLastFrameToNv12(encodedFrame,
                                                                         &totalRenderNv12StageMs,
                                                                         &totalGpuReadbackMs);
                if (!gpuNv12Converted && !fillNv12FrameFromImage(rendered, encodedFrame)) {
                    errorMessage = QStringLiteral("Failed to convert rendered frame %1 to NV12 for encoding.").arg(timelineFrame);
                    break;
                }
            } else {
                const int copyBytesPerRow = qMin(static_cast<int>(rendered.bytesPerLine()), sourceFrame->linesize[0]);
                for (int y = 0; y < rendered.height(); ++y) {
                    memcpy(sourceFrame->data[0] + (y * sourceFrame->linesize[0]),
                           rendered.constScanLine(y),
                           copyBytesPerRow);
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
    av_frame_free(&sourceFrame);
    av_frame_free(&encodedFrame);
    sws_freeContext(swsCtx);
    if (!(formatCtx->oformat->flags & AVFMT_NOFILE)) {
        avio_closep(&formatCtx->pb);
    }
    if (audioState.codecCtx) {
        avcodec_free_context(&audioState.codecCtx);
    }
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
    result.message = QStringLiteral("Rendered %1 video frames to %2%3")
                         .arg(framesCompleted)
                         .arg(QDir::toNativeSeparators(request.outputPath))
                         .arg(useGpuRenderer ? QString() : QStringLiteral("\nGPU export path unavailable, used CPU fallback: %1").arg(gpuInitializationError));
    return result;
}
