#include "decoder_ffmpeg_utils.h"
#include "debug_controls.h"

#include <QThread>

extern "C" {
#include <libavutil/error.h>
#include <libavutil/pixdesc.h>
}

namespace editor {

QString avErrToString(int errnum) {
    char errbuf[AV_ERROR_MAX_STRING_SIZE];
    av_strerror(errnum, errbuf, AV_ERROR_MAX_STRING_SIZE);
    return QString::fromUtf8(errbuf);
}

AVPixelFormat get_hw_format(AVCodecContext* ctx, const AVPixelFormat* pix_fmts) {
    const AVPixelFormat preferred =
        static_cast<AVPixelFormat>(reinterpret_cast<intptr_t>(ctx->opaque));
    for (const AVPixelFormat* p = pix_fmts; *p != AV_PIX_FMT_NONE; ++p) {
        if (*p == preferred) {
            return *p;
        }
    }
    return pix_fmts[0];
}

AVPixelFormat get_alpha_compatible_format(AVCodecContext* ctx, const AVPixelFormat* pix_fmts) {
    Q_UNUSED(ctx)

    for (const AVPixelFormat* p = pix_fmts; *p != AV_PIX_FMT_NONE; ++p) {
        const AVPixFmtDescriptor* desc = av_pix_fmt_desc_get(*p);
        if (desc && (desc->flags & AV_PIX_FMT_FLAG_ALPHA)) {
            return *p;
        }
    }

    return pix_fmts[0];
}

int64_t ptsToFrameNumber(int64_t pts, const AVRational& timeBase, double fps) {
    if (pts == AV_NOPTS_VALUE || fps <= 0.0) {
        return -1;
    }
    const double seconds = pts * av_q2d(timeBase);
    return static_cast<int64_t>(seconds * fps + 0.5);
}

VideoDecoderThreadingPolicy applyVideoDecoderThreadingPolicy(AVCodecContext* codecCtx,
                                                             const AVCodec* decoder,
                                                             AVCodecID codecId,
                                                             bool hardwareEnabled,
                                                             bool deterministicPipeline) {
    VideoDecoderThreadingPolicy policy;
    if (!codecCtx) {
        return policy;
    }

    const int defaultSoftwareThreadCount = qBound(2, QThread::idealThreadCount(), 8);
    const bool softwareH26xCodec =
        !hardwareEnabled &&
        (codecId == AV_CODEC_ID_H264 || codecId == AV_CODEC_ID_HEVC);
    const H26xSoftwareThreadingMode h26xThreadingMode = debugH26xSoftwareThreadingMode();
    const bool decoderSupportsSliceThreads =
#ifdef AV_CODEC_CAP_SLICE_THREADS
        decoder && ((decoder->capabilities & AV_CODEC_CAP_SLICE_THREADS) != 0);
#else
        true;
#endif

    const auto applySingleThreadPolicy = [codecCtx]() {
        codecCtx->thread_count = 1;
        codecCtx->thread_type = 0;
        codecCtx->flags2 &= ~AV_CODEC_FLAG2_FAST;
    };
    const auto applySliceThreadPolicy = [codecCtx, defaultSoftwareThreadCount]() {
        codecCtx->thread_count = defaultSoftwareThreadCount;
        codecCtx->thread_type = FF_THREAD_SLICE;
        codecCtx->flags2 |= AV_CODEC_FLAG2_FAST;
    };
    const auto applyFrameAndSliceThreadPolicy = [codecCtx, defaultSoftwareThreadCount]() {
        codecCtx->thread_count = defaultSoftwareThreadCount;
        codecCtx->thread_type = FF_THREAD_FRAME | FF_THREAD_SLICE;
        codecCtx->flags2 |= AV_CODEC_FLAG2_FAST;
    };

    if (deterministicPipeline) {
        applySingleThreadPolicy();
        return policy;
    }

    if (hardwareEnabled) {
        codecCtx->thread_count = 0;
        codecCtx->thread_type = FF_THREAD_FRAME;
        return policy;
    }

    if (softwareH26xCodec) {
#if LIBAVCODEC_VERSION_MAJOR >= 61
        policy.serializeH26xSoftwareDecode = true;
#endif
        switch (h26xThreadingMode) {
        case H26xSoftwareThreadingMode::SingleThread:
            applySingleThreadPolicy();
            break;
        case H26xSoftwareThreadingMode::SliceThreads:
            if (decoderSupportsSliceThreads) {
                applySliceThreadPolicy();
            } else {
                applySingleThreadPolicy();
            }
            break;
        case H26xSoftwareThreadingMode::FrameAndSliceThreads:
            applyFrameAndSliceThreadPolicy();
            break;
        case H26xSoftwareThreadingMode::Auto:
        default:
#if LIBAVCODEC_VERSION_MAJOR >= 61
            applySingleThreadPolicy();
#else
            applyFrameAndSliceThreadPolicy();
#endif
            break;
        }
        return policy;
    }

    applyFrameAndSliceThreadPolicy();
    return policy;
}

} // namespace editor
