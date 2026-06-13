#include "render_internal.h"
#include <QByteArray>

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
