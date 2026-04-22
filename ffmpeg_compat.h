#pragma once

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/channel_layout.h>
#include <libavutil/frame.h>
#include <libavutil/opt.h>
#include <libavutil/version.h>
#include <libswresample/swresample.h>
}

namespace ffmpeg_compat {

#if LIBAVUTIL_VERSION_MAJOR >= 57

using ChannelLayoutHandle = AVChannelLayout;

inline void defaultChannelLayout(ChannelLayoutHandle* layout, int channels) {
    av_channel_layout_default(layout, channels);
}

inline void uninitChannelLayout(ChannelLayoutHandle* layout) {
    av_channel_layout_uninit(layout);
}

inline int copyFrameChannelLayout(AVFrame* frame, const AVCodecContext* codecCtx) {
    return av_channel_layout_copy(&frame->ch_layout, &codecCtx->ch_layout);
}

inline int codecContextChannelCount(const AVCodecContext* codecCtx) {
    return codecCtx->ch_layout.nb_channels;
}

inline int setSwrInputLayout(SwrContext* swr, const AVCodecContext* codecCtx) {
    return av_opt_set_chlayout(swr, "in_chlayout", &codecCtx->ch_layout, 0);
}

inline int setSwrOutputLayout(SwrContext* swr, const ChannelLayoutHandle* layout) {
    return av_opt_set_chlayout(swr, "out_chlayout", layout, 0);
}

#else

using ChannelLayoutHandle = uint64_t;

inline uint64_t channelLayoutForCodecContext(const AVCodecContext* codecCtx) {
    if (codecCtx->channel_layout != 0) {
        return codecCtx->channel_layout;
    }
    const int channels = codecCtx->channels > 0 ? codecCtx->channels : 2;
    return static_cast<uint64_t>(av_get_default_channel_layout(channels));
}

inline void defaultChannelLayout(ChannelLayoutHandle* layout, int channels) {
    *layout = static_cast<uint64_t>(av_get_default_channel_layout(channels));
}

inline void uninitChannelLayout(ChannelLayoutHandle*) {}

inline int copyFrameChannelLayout(AVFrame* frame, const AVCodecContext* codecCtx) {
    frame->channel_layout = channelLayoutForCodecContext(codecCtx);
    frame->channels = codecCtx->channels > 0
        ? codecCtx->channels
        : av_get_channel_layout_nb_channels(frame->channel_layout);
    return 0;
}

inline int codecContextChannelCount(const AVCodecContext* codecCtx) {
    if (codecCtx->channels > 0) {
        return codecCtx->channels;
    }
    if (codecCtx->channel_layout != 0) {
        return av_get_channel_layout_nb_channels(codecCtx->channel_layout);
    }
    return 0;
}

inline int setSwrInputLayout(SwrContext* swr, const AVCodecContext* codecCtx) {
    return av_opt_set_int(swr, "in_channel_layout",
                          static_cast<int64_t>(channelLayoutForCodecContext(codecCtx)), 0);
}

inline int setSwrOutputLayout(SwrContext* swr, const ChannelLayoutHandle* layout) {
    return av_opt_set_int(swr, "out_channel_layout", static_cast<int64_t>(*layout), 0);
}

#endif

} // namespace ffmpeg_compat
