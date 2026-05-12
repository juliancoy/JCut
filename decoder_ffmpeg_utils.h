#pragma once

#include <QString>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/pixfmt.h>
#include <libavutil/rational.h>
}

namespace editor {

struct VideoDecoderThreadingPolicy {
    bool serializeH26xSoftwareDecode = false;
};

QString avErrToString(int errnum);
void installFfmpegLogFilter();
AVPixelFormat get_hw_format(AVCodecContext* ctx, const AVPixelFormat* pix_fmts);
AVPixelFormat get_alpha_compatible_format(AVCodecContext* ctx, const AVPixelFormat* pix_fmts);
int64_t ptsToFrameNumber(int64_t pts, const AVRational& timeBase, double fps);
VideoDecoderThreadingPolicy applyVideoDecoderThreadingPolicy(AVCodecContext* codecCtx,
                                                             const AVCodec* decoder,
                                                             AVCodecID codecId,
                                                             bool hardwareEnabled,
                                                             bool deterministicPipeline);

} // namespace editor
