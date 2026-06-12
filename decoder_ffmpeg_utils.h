#pragma once

#include <QString>

#include <mutex>

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
// Aborts with a diagnostic if the FFmpeg headers this binary was compiled
// against belong to a different major version than the libraries it loaded.
// Struct layouts differ across majors, so a mismatch corrupts every direct
// field read (sample_rate, width/height) instead of failing visibly.
void enforceFfmpegHeaderRuntimeMatch();
std::mutex& ffmpegDecodeMutex();
AVPixelFormat get_hw_format(AVCodecContext* ctx, const AVPixelFormat* pix_fmts);
AVPixelFormat get_alpha_compatible_format(AVCodecContext* ctx, const AVPixelFormat* pix_fmts);
int64_t ptsToFrameNumber(int64_t pts, const AVRational& timeBase, double fps);
VideoDecoderThreadingPolicy applyVideoDecoderThreadingPolicy(AVCodecContext* codecCtx,
                                                             const AVCodec* decoder,
                                                             AVCodecID codecId,
                                                             bool hardwareEnabled,
                                                             bool deterministicPipeline);

} // namespace editor
