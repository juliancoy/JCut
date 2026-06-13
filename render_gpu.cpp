#include "render_internal.h"
#include "cpu_overlay_render_backend.h"
#include "visual_effects_shader.h"
#include "polygon_triangulation.h"
#include "titles.h"

#include <QByteArray>
#include <QPainter>

#include <cmath>

namespace render_detail {

QVector<TimelineClip> sortedVisualClips(const QVector<TimelineClip>& clips,
                                        const QVector<TimelineTrack>& tracks) {
    QVector<TimelineClip> visual;
    for (const TimelineClip& clip : clips) {
        if (clipVisualPlaybackEnabled(clip, tracks)) {
            visual.push_back(clip);
        }
    }
    std::sort(visual.begin(), visual.end(), [](const TimelineClip& a, const TimelineClip& b) {
        if (a.trackIndex == b.trackIndex) {
            return clipTimelineStartSamples(a) < clipTimelineStartSamples(b);
        }
        return a.trackIndex > b.trackIndex;
    });
    return visual;
}


bool renderTimelineFrameToOutput(const RenderRequest& request,
                                 qreal timelineFrame,
                                 QHash<QString, editor::DecoderContext*>& decoders,
                                 editor::AsyncDecoder* asyncDecoder,
                                 QHash<RenderAsyncFrameKey, editor::FrameHandle>* asyncFrameCache,
                                 const QVector<TimelineClip>& orderedClips,
                                 OffscreenRenderFrame* output,
                                 bool readbackToCpuImage,
                                 QHash<QString, RenderClipStageStats>* clipStageStats,
                                 qint64* decodeMs,
                                 qint64* textureMs,
                                 qint64* compositeMs,
                                 qint64* readbackMs,
                                 QJsonArray* skippedClips,
                                 QJsonObject* skippedReasonCounts)
{
    if (!output) {
        return false;
    }
    *output = OffscreenRenderFrame{};
    if (!readbackToCpuImage) {
        return false;
    }
    if (decodeMs) {
        *decodeMs = 0;
    }
    if (textureMs) {
        *textureMs = 0;
    }
    if (compositeMs) {
        *compositeMs = 0;
    }
    if (readbackMs) {
        *readbackMs = 0;
    }
    output->cpuImage = renderTimelineFrame(request,
                                           timelineFrame,
                                           decoders,
                                           asyncDecoder,
                                           asyncFrameCache,
                                           orderedClips,
                                           clipStageStats,
                                           skippedClips,
                                           skippedReasonCounts);
    return !output->cpuImage.isNull();
}

QImage renderTimelineFrame(const RenderRequest& request,
                           qreal timelineFrame,
                           QHash<QString, editor::DecoderContext*>& decoders,
                           editor::AsyncDecoder* asyncDecoder,
                           QHash<RenderAsyncFrameKey, editor::FrameHandle>* asyncFrameCache,
                           const QVector<TimelineClip>& orderedClips,
                           QHash<QString, RenderClipStageStats>* clipStageStats,
                           QJsonArray* skippedClips,
                           QJsonObject* skippedReasonCounts) {
    QImage canvas(request.outputSize, QImage::Format_ARGB32_Premultiplied);
    canvas.fill(QColor(QStringLiteral("#000000")));

    QPainter painter(&canvas);
    painter.setRenderHint(QPainter::Antialiasing, true);
    painter.setRenderHint(QPainter::SmoothPixmapTransform, true);
    bool backgroundFilled = false;

    for (const TimelineClip& clip : orderedClips) {
        if (timelineFrame < clip.startFrame || timelineFrame >= clip.startFrame + clip.durationFrames) {
            continue;
        }

        EffectiveVisualEffects effects = evaluateEffectiveVisualEffectsAtPosition(
            clip, request.tracks, timelineFrame, request.renderSyncMarkers);
        if (!request.correctionsEnabled) {
            effects.correctionPolygons.clear();
        }
        if (effects.grading.opacity <= 0.0001) {
            recordRenderSkip(skippedClips, skippedReasonCounts, clip, QStringLiteral("zero_opacity"), timelineFrame);
            continue;
        }

        // Handle title clips specially - they don't have video files to decode
        if (clip.mediaType == ClipMediaType::Title) {
            if (clip.titleKeyframes.isEmpty()) {
                // No title keyframes to render
                continue;
            }
            
            const int64_t localFrame =
                qMax<int64_t>(0, static_cast<int64_t>(std::floor(timelineFrame - clip.startFrame)));
            const EvaluatedTitle evaluatedTitle = evaluateTitleAtLocalFrame(clip, localFrame);
            const EvaluatedTitle title =
                composeTitleWithOpacity(evaluatedTitle, static_cast<qreal>(effects.grading.opacity));
            if (!title.valid || title.text.isEmpty() || title.opacity <= 0.001) {
                continue;
            }
            
            QElapsedTimer compositeTimer;
            compositeTimer.start();
            
            const OverlayImage titleImage =
                renderTitleOverlay(request.outputSize, title, request.outputSize);
            if (!titleImage.isNull()) {
                painter.drawImage(QPoint(0, 0), titleImage.asQImageView());
            }
            
            accumulateClipStageStats(clipStageStats, clip, 0, 0, compositeTimer.elapsed());
            continue;
        }

        // Regular video/image clip processing
        const QString path = clip.filePath;
        const int64_t localFrame =
            sourceFrameForClipAtTimelinePosition(clip, static_cast<qreal>(timelineFrame), request.renderSyncMarkers);
        QElapsedTimer decodeTimer;
        decodeTimer.start();
        const editor::FrameHandle frame =
            decodeRenderFrame(path, localFrame, decoders, asyncDecoder, asyncFrameCache);
        const qint64 decodeElapsed = decodeTimer.elapsed();
        if (frame.isNull()) {
            recordRenderSkip(skippedClips,
                             skippedReasonCounts,
                             clip,
                             QStringLiteral("frame_null_or_decoder_init_failed"),
                             timelineFrame,
                             localFrame);
            continue;
        }
        if (!frame.hasCpuImage()) {
            recordRenderSkip(skippedClips, skippedReasonCounts, clip, QStringLiteral("no_cpu_image"), timelineFrame, localFrame);
            continue;
        }

        QImage graded = applyEffectiveClipVisualEffectsToImage(frame.cpuImage(), effects);
        
        const QRect fitted = fitRect(graded.size(), request.outputSize);
        const TimelineClip::TransformKeyframe transform =
            evaluateClipRenderTransformAtPosition(
                clip, static_cast<qreal>(timelineFrame), request.outputSize);

        QElapsedTimer compositeTimer;
        compositeTimer.start();
        if (!backgroundFilled && shouldDrawBlurredFillBackground(graded.size(), request.outputSize)) {
            const QImage background = buildBlurredFillBackground(graded, request.outputSize);
            if (!background.isNull()) {
                painter.drawImage(QPoint(0, 0), background);
                backgroundFilled = true;
            }
        }
        painter.save();
        painter.translate(fitted.center().x() + transform.translationX,
                          fitted.center().y() + transform.translationY);
        painter.rotate(transform.rotation);
        painter.scale(transform.scaleX, transform.scaleY);
        const QRectF drawRect(-fitted.width() / 2.0,
                              -fitted.height() / 2.0,
                              fitted.width(),
                              fitted.height());
        painter.drawImage(drawRect, graded);
        painter.restore();
        accumulateClipStageStats(clipStageStats, clip, decodeElapsed, 0, compositeTimer.elapsed());
    }

    return canvas;
}

bool encodeFrame(AVCodecContext* codecCtx,
                 AVStream* stream,
                 AVFormatContext* formatCtx,
                 AVFrame* frame,
                 QString* errorMessage) {
    AVFrame* submittedFrame = frame;
    AVFrame* hwFrame = nullptr;
    if (frame &&
        codecCtx &&
        codecCtx->pix_fmt == AV_PIX_FMT_CUDA &&
        codecCtx->hw_frames_ctx &&
        frame->format != AV_PIX_FMT_CUDA) {
        hwFrame = av_frame_alloc();
        if (!hwFrame) {
            if (errorMessage) {
                *errorMessage = QStringLiteral("Failed to allocate CUDA encoder frame.");
            }
            return false;
        }
        hwFrame->format = AV_PIX_FMT_CUDA;
        hwFrame->width = frame->width;
        hwFrame->height = frame->height;
        hwFrame->pts = frame->pts;
        int ret = av_hwframe_get_buffer(codecCtx->hw_frames_ctx, hwFrame, 0);
        if (ret < 0) {
            av_frame_free(&hwFrame);
            if (errorMessage) {
                *errorMessage = QStringLiteral("Failed to allocate CUDA hardware frame: %1")
                                    .arg(avErrToString(ret));
            }
            return false;
        }
        ret = av_hwframe_transfer_data(hwFrame, frame, 0);
        if (ret < 0) {
            av_frame_free(&hwFrame);
            if (errorMessage) {
                *errorMessage = QStringLiteral("Failed to upload frame to CUDA encoder surface: %1")
                                    .arg(avErrToString(ret));
            }
            return false;
        }
        submittedFrame = hwFrame;
    }

    int ret = avcodec_send_frame(codecCtx, submittedFrame);
    av_frame_free(&hwFrame);
    if (ret < 0) {
        if (errorMessage) {
            *errorMessage = QStringLiteral("Failed to send frame to encoder: %1").arg(avErrToString(ret));
        }
        return false;
    }

    AVPacket* packet = av_packet_alloc();
    if (!packet) {
        if (errorMessage) {
            *errorMessage = QStringLiteral("Failed to allocate output packet.");
        }
        return false;
    }

    while (true) {
        ret = avcodec_receive_packet(codecCtx, packet);
        if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF) {
            break;
        }
        if (ret < 0) {
            av_packet_free(&packet);
            if (errorMessage) {
                *errorMessage = QStringLiteral("Failed to receive encoded packet: %1").arg(avErrToString(ret));
            }
            return false;
        }

        av_packet_rescale_ts(packet, codecCtx->time_base, stream->time_base);
        packet->stream_index = stream->index;
        ret = av_interleaved_write_frame(formatCtx, packet);
        av_packet_unref(packet);
        if (ret < 0) {
            av_packet_free(&packet);
            if (errorMessage) {
                *errorMessage = QStringLiteral("Failed to write output packet: %1").arg(avErrToString(ret));
            }
            return false;
        }
    }

    av_packet_free(&packet);
    return true;
}

} // namespace render_detail
