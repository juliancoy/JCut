#include "decoder_ffmpeg_utils.h"
#include "debug_controls.h"

#include <QThread>

extern "C" {
#include <libavformat/avformat.h>
#include <libavutil/avutil.h>
#include <libavutil/error.h>
#include <libavutil/log.h>
#include <libavutil/pixdesc.h>
#include <libswresample/swresample.h>
#include <libswscale/swscale.h>
}

#include <atomic>
#include <array>
#include <cstdarg>
#include <cstdio>
#include <cstring>

namespace editor {

namespace {

void jcutFfmpegLogCallback(void* avcl, int level, const char* fmt, va_list vl)
{
    if (level > av_log_get_level() || !fmt) {
        return;
    }

    // FFmpeg emits this for some valid H.264 files with late SEI metadata.
    // It is not actionable for frame decode and can flood realtime logs.
    if (std::strstr(fmt, "Late SEI") ||
        std::strstr(fmt, "Update your FFmpeg version to the newest one from Git") ||
        std::strstr(fmt, "If the problem still occurs, it means that your file has a feature") ||
        std::strstr(fmt, "If you want to help, upload a sample of this file")) {
            return;
    }
    av_log_default_callback(avcl, level, fmt, vl);
}

} // namespace

QString avErrToString(int errnum) {
    char errbuf[AV_ERROR_MAX_STRING_SIZE];
    av_strerror(errnum, errbuf, AV_ERROR_MAX_STRING_SIZE);
    return QString::fromUtf8(errbuf);
}

void enforceFfmpegHeaderRuntimeMatch()
{
    static std::atomic_bool checked{false};
    bool expected = false;
    if (!checked.compare_exchange_strong(expected, true)) {
        return;
    }

    const struct {
        const char* name;
        unsigned builtAgainst;
        unsigned runtime;
    } libs[] = {
        {"libavcodec", LIBAVCODEC_VERSION_INT, avcodec_version()},
        {"libavformat", LIBAVFORMAT_VERSION_INT, avformat_version()},
        {"libavutil", LIBAVUTIL_VERSION_INT, avutil_version()},
        {"libswresample", LIBSWRESAMPLE_VERSION_INT, swresample_version()},
        {"libswscale", LIBSWSCALE_VERSION_INT, swscale_version()},
    };
    for (const auto& lib : libs) {
        if (AV_VERSION_MAJOR(lib.builtAgainst) != AV_VERSION_MAJOR(lib.runtime)) {
            qFatal("FFmpeg header/runtime mismatch for %s: compiled against %u.%u.%u "
                   "but loaded %u.%u.%u. Struct layouts differ across major versions, "
                   "so decoded metadata reads are corrupted (e.g. sample_rate=0, 0x0 "
                   "frames). A system FFmpeg's headers likely shadowed "
                   "ffmpeg-install/include at compile time; fix the include order and "
                   "rebuild.",
                   lib.name,
                   AV_VERSION_MAJOR(lib.builtAgainst), AV_VERSION_MINOR(lib.builtAgainst),
                   AV_VERSION_MICRO(lib.builtAgainst),
                   AV_VERSION_MAJOR(lib.runtime), AV_VERSION_MINOR(lib.runtime),
                   AV_VERSION_MICRO(lib.runtime));
        }
    }
}

void installFfmpegLogFilter()
{
    static std::atomic_bool installed{false};
    bool expected = false;
    if (installed.compare_exchange_strong(expected, true)) {
        enforceFfmpegHeaderRuntimeMatch();
        // Keep production playback lean by avoiding verbose FFmpeg logging
        // unless decode diagnostics are explicitly enabled.
        av_log_set_level(debugDecodeEnabled() ? AV_LOG_WARNING : AV_LOG_ERROR);
        av_log_set_callback(jcutFfmpegLogCallback);
    }
}

std::mutex& ffmpegDecodeMutex()
{
    static std::mutex mutex;
    return mutex;
}

AVPixelFormat get_hw_format(AVCodecContext* ctx, const AVPixelFormat* pix_fmts) {
    const AVPixelFormat preferred =
        static_cast<AVPixelFormat>(reinterpret_cast<intptr_t>(ctx->opaque));
    for (const AVPixelFormat* p = pix_fmts; *p != AV_PIX_FMT_NONE; ++p) {
        if (*p == preferred) {
            return *p;
        }
    }
    return AV_PIX_FMT_NONE;
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
        // Hardware decoders provide their own parallelism. FFmpeg frame
        // threading clones the codec context, redundantly initializes the
        // same hardware stream once per CPU thread, and amplifies a single
        // capability rejection into a log/error storm.
        applySingleThreadPolicy();
        return policy;
    }

    if (softwareH26xCodec) {
        // Stability-first policy for software H.264/H.265 decode:
        // serialize decode operations and default to single-thread decode.
        // This avoids intermittent crashes seen in multithreaded decode paths.
        policy.serializeH26xSoftwareDecode = true;
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
            applySingleThreadPolicy();
            break;
        }
        return policy;
    }

    // Stability-first default for all software video decode paths.
    // FFmpeg frame/slice threading has shown intermittent crashes across codecs
    // in this environment, so keep software decode single-threaded.
    applySingleThreadPolicy();
    return policy;
}

} // namespace editor
