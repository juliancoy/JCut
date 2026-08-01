#pragma once

#ifndef __STDC_CONSTANT_MACROS
#define __STDC_CONSTANT_MACROS
#endif

#include <string>
#include <vector>

extern "C" {
#include <libavcodec/codec_id.h>
#include <libavutil/hwcontext.h>
}

namespace jcut {

struct VideoDecodeBackendCapability {
    AVHWDeviceType deviceType = AV_HWDEVICE_TYPE_NONE;
    std::string id;
    std::string label;
    std::string operatingSystem;
    std::string devicePath;
    int preference = 0;
    bool compiled = false;
    bool operatingSystemSupported = false;
    bool available = false;
    std::vector<AVCodecID> codecIds;
    std::string reason;
};

// Process-stable hardware probe shared by every decoder frontend.
const std::vector<VideoDecodeBackendCapability>&
detectedVideoDecodeCapabilities();

std::vector<AVHWDeviceType> preferredVideoDecodeDeviceOrder(
    AVCodecID codecId = AV_CODEC_ID_NONE);

const VideoDecodeBackendCapability* videoDecodeCapability(
    AVHWDeviceType deviceType);

bool videoDecodeBackendSupportsCodec(
    const VideoDecodeBackendCapability& capability,
    AVCodecID codecId);

} // namespace jcut
