#include "render_internal.h"

namespace render_detail {

const AVCodec* codecForRequest(const QString& outputFormat, QString* codecLabel) {
    const QString format = outputFormat.toLower();
    if (format == QStringLiteral("mov")) {
        if (codecLabel) *codecLabel = QStringLiteral("prores_ks");
        if (const AVCodec* codec = avcodec_find_encoder_by_name("prores_ks")) {
            return codec;
        }
        return avcodec_find_encoder(AV_CODEC_ID_PRORES);
    }
    if (format == QStringLiteral("mkv")) {
        if (codecLabel) *codecLabel = QStringLiteral("ffv1");
        if (const AVCodec* codec = avcodec_find_encoder_by_name("ffv1")) {
            return codec;
        }
        return avcodec_find_encoder(AV_CODEC_ID_FFV1);
    }
    if (format == QStringLiteral("webm")) {
        if (codecLabel) *codecLabel = QStringLiteral("libvpx-vp9");
        if (const AVCodec* codec = avcodec_find_encoder_by_name("libvpx-vp9")) {
            return codec;
        }
        return avcodec_find_encoder(AV_CODEC_ID_VP9);
    }

    if (codecLabel) *codecLabel = QStringLiteral("libx264");
    if (const AVCodec* codec = avcodec_find_encoder_by_name("libx264")) {
        return codec;
    }
    if (const AVCodec* codec = avcodec_find_encoder(AV_CODEC_ID_H264)) {
        if (codecLabel) *codecLabel = QStringLiteral("h264");
        return codec;
    }
    if (const AVCodec* codec = avcodec_find_encoder_by_name("libopenh264")) {
        if (codecLabel) *codecLabel = QStringLiteral("libopenh264");
        return codec;
    }
    if (const AVCodec* codec = avcodec_find_encoder(AV_CODEC_ID_MPEG4)) {
        if (codecLabel) *codecLabel = QStringLiteral("mpeg4");
        return codec;
    }
    return nullptr;
}

QVector<VideoEncoderChoice> videoEncoderChoicesForRequest(const QString& outputFormat) {
    const QString format = outputFormat.toLower();
    QVector<VideoEncoderChoice> choices;
    if (format == QStringLiteral("mp4")) {
        choices.push_back({QStringLiteral("h264_nvenc"), AV_PIX_FMT_NV12});
#if defined(Q_OS_LINUX) || defined(__linux__)
        choices.push_back({QStringLiteral("h264_qsv"), AV_PIX_FMT_NV12});
        choices.push_back({QStringLiteral("h264_vaapi"), AV_PIX_FMT_NV12});
#endif
        choices.push_back({QStringLiteral("libx264"), AV_PIX_FMT_YUV420P});
        choices.push_back({QStringLiteral("libopenh264"), AV_PIX_FMT_YUV420P});
        choices.push_back({QStringLiteral("mpeg4"), AV_PIX_FMT_YUV420P});
    } else if (format == QStringLiteral("mov")) {
        choices.push_back({QStringLiteral("prores_ks"), AV_PIX_FMT_YUV422P10LE});
    } else if (format == QStringLiteral("webm")) {
        choices.push_back({QStringLiteral("libvpx-vp9"), AV_PIX_FMT_YUV420P});
    } else if (format == QStringLiteral("mkv")) {
        choices.push_back({QStringLiteral("ffv1"), AV_PIX_FMT_BGRA});
    }
    return choices;
}

const AVCodec* audioCodecForRequest(const QString& outputFormat, QString* codecLabel) {
    const QString format = outputFormat.toLower();
    if (format == QStringLiteral("webm")) {
        if (codecLabel) *codecLabel = QStringLiteral("libopus");
        if (const AVCodec* codec = avcodec_find_encoder_by_name("libopus")) {
            return codec;
        }
        return avcodec_find_encoder(AV_CODEC_ID_OPUS);
    }

    if (codecLabel) *codecLabel = QStringLiteral("aac");
    if (const AVCodec* codec = avcodec_find_encoder_by_name("aac")) {
        return codec;
    }
    return avcodec_find_encoder(AV_CODEC_ID_AAC);
}

AVPixelFormat pixelFormatForCodec(const AVCodec* codec, const QString& outputFormat) {
    const QString format = outputFormat.toLower();
    if (format == QStringLiteral("mov")) {
        return AV_PIX_FMT_YUV422P10LE;
    }
    if (format == QStringLiteral("mkv")) {
        return AV_PIX_FMT_BGRA;
    }
    Q_UNUSED(codec)
    return AV_PIX_FMT_YUV420P;
}

void configureCodecOptions(AVCodecContext* codecCtx, const QString& outputFormat, const QString& codecLabel) {
    const QString format = outputFormat.toLower();
    const QString loweredCodec = codecLabel.toLower();
    if (loweredCodec == QStringLiteral("h264_nvenc")) {
        av_opt_set(codecCtx->priv_data, "preset", "p5", 0);
        av_opt_set(codecCtx->priv_data, "rc", "vbr", 0);
        av_opt_set(codecCtx->priv_data, "cq", "19", 0);
        return;
    } else if (loweredCodec == QStringLiteral("h264_qsv")) {
        av_opt_set(codecCtx->priv_data, "preset", "medium", 0);
        av_opt_set(codecCtx->priv_data, "global_quality", "23", 0);
        return;
    } else if (loweredCodec == QStringLiteral("h264_vaapi")) {
        av_opt_set(codecCtx->priv_data, "rc_mode", "VBR", 0);
        av_opt_set(codecCtx->priv_data, "qp", "20", 0);
        return;
    }
    const bool h264Family =
        loweredCodec.contains(QStringLiteral("264")) ||
        loweredCodec.contains(QStringLiteral("openh264"));
    if (format == QStringLiteral("mp4") && h264Family) {
        av_opt_set(codecCtx->priv_data, "preset", "veryfast", 0);
        av_opt_set(codecCtx->priv_data, "crf", "18", 0);
    } else if (format == QStringLiteral("mov")) {
        av_opt_set(codecCtx->priv_data, "profile", "3", 0);
    } else if (format == QStringLiteral("webm")) {
        av_opt_set(codecCtx->priv_data, "deadline", "realtime", 0);
        av_opt_set(codecCtx->priv_data, "cpu-used", "4", 0);
        av_opt_set(codecCtx->priv_data, "crf", "30", 0);
        av_opt_set(codecCtx->priv_data, "b", "0", 0);
    }
}

void configureAudioCodecOptions(AVCodecContext* codecCtx, const QString& outputFormat) {
    const QString format = outputFormat.toLower();
    if (format == QStringLiteral("webm")) {
        av_opt_set(codecCtx->priv_data, "application", "audio", 0);
    }
}

AVSampleFormat audioSampleFormatForCodec(const AVCodec* codec) {
    if (!codec || !codec->sample_fmts) {
        return AV_SAMPLE_FMT_FLTP;
    }

    const AVSampleFormat preferredFormats[] = {
        AV_SAMPLE_FMT_FLTP,
        AV_SAMPLE_FMT_S16P,
        AV_SAMPLE_FMT_S16,
        AV_SAMPLE_FMT_FLT,
    };
    for (AVSampleFormat preferred : preferredFormats) {
        for (const AVSampleFormat* fmt = codec->sample_fmts; *fmt != AV_SAMPLE_FMT_NONE; ++fmt) {
            if (*fmt == preferred) {
                return preferred;
            }
        }
    }
    return codec->sample_fmts[0];
}

int audioSampleRateForCodec(const AVCodec* codec) {
    if (!codec || !codec->supported_samplerates) {
        return kRenderAudioSampleRate;
    }

    for (const int* sampleRate = codec->supported_samplerates; *sampleRate != 0; ++sampleRate) {
        if (*sampleRate == kRenderAudioSampleRate) {
            return *sampleRate;
        }
    }
    return codec->supported_samplerates[0];
}

} // namespace render_detail
