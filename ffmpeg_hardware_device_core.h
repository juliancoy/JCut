#pragma once

#include "decoder_policy_core.h"

#include <string>
#include <vector>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/hwcontext.h>
}

namespace jcut {

struct FfmpegHardwareDeviceSetup {
    AVBufferRef* deviceContext = nullptr;
    AVPixelFormat hardwarePixelFormat = AV_PIX_FMT_NONE;
    AVHWDeviceType deviceType = AV_HWDEVICE_TYPE_NONE;
    std::string deviceLabel;
    std::string error;
};

// Returns the first usable device-context configuration supported by decoder.
// The caller owns setup.deviceContext and must release it with av_buffer_unref.
FfmpegHardwareDeviceSetup createFfmpegHardwareDeviceForDecoder(
    const AVCodec* decoder,
    const std::vector<AVHWDeviceType>& preferredTypes = {});

// FFmpeg get_format callback. Store the selected AVPixelFormat in
// AVCodecContext::opaque before assigning this callback.
AVPixelFormat selectFfmpegHardwarePixelFormat(
    AVCodecContext* context,
    const AVPixelFormat* formats);

std::vector<AVHWDeviceType> defaultFfmpegHardwareDeviceOrder();
std::vector<AVHWDeviceType> ffmpegHardwareDeviceOrder(
    DecodeHardwareDeviceCore preference);

} // namespace jcut
