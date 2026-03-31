#include "decoder_ffmpeg_utils.h"

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

} // namespace editor
