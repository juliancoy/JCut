#pragma once

#include <QString>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/pixfmt.h>
#include <libavutil/rational.h>
}

namespace editor {

QString avErrToString(int errnum);
AVPixelFormat get_hw_format(AVCodecContext* ctx, const AVPixelFormat* pix_fmts);
AVPixelFormat get_alpha_compatible_format(AVCodecContext* ctx, const AVPixelFormat* pix_fmts);
int64_t ptsToFrameNumber(int64_t pts, const AVRational& timeBase, double fps);

} // namespace editor
