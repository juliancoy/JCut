#include "video_decode_capabilities_core.h"

#include <algorithm>
#include <filesystem>

extern "C" {
#include <libavcodec/codec.h>
}

namespace jcut {
namespace {

std::string operatingSystemId()
{
#if defined(__linux__)
    return "linux";
#elif defined(_WIN32)
    return "windows";
#elif defined(__APPLE__)
    return "macos";
#else
    return "unknown";
#endif
}

std::string vaapiRenderNode()
{
#if defined(__linux__)
    for (int index = 128; index <= 191; ++index) {
        const std::filesystem::path candidate =
            "/dev/dri/renderD" + std::to_string(index);
        std::error_code error;
        if (std::filesystem::exists(candidate, error) && !error) {
            return candidate.string();
        }
    }
#endif
    return {};
}

bool decoderSupportsDevice(const AVCodec* decoder, AVHWDeviceType deviceType)
{
    if (!decoder) return false;
    for (int index = 0;; ++index) {
        const AVCodecHWConfig* config = avcodec_get_hw_config(decoder, index);
        if (!config) break;
        if (config->device_type == deviceType &&
            (config->methods & AV_CODEC_HW_CONFIG_METHOD_HW_DEVICE_CTX)) {
            return true;
        }
    }
    return false;
}

std::vector<VideoDecodeBackendCapability> probeVideoDecodeCapabilities()
{
    const std::string operatingSystem = operatingSystemId();
    std::vector<VideoDecodeBackendCapability> capabilities;
    const auto addCandidate = [&](AVHWDeviceType type,
                                  const char* id,
                                  const char* label,
                                  int preference,
                                  bool operatingSystemSupported) {
        VideoDecodeBackendCapability capability;
        capability.deviceType = type;
        capability.id = id;
        capability.label = label;
        capability.operatingSystem = operatingSystem;
        capability.preference = preference;
        capability.operatingSystemSupported = operatingSystemSupported;
        capability.compiled =
            av_hwdevice_find_type_by_name(id) == type;
        if (type == AV_HWDEVICE_TYPE_VAAPI) {
            capability.devicePath = vaapiRenderNode();
        }

        if (!capability.compiled) {
            capability.reason = "FFmpeg was built without this hardware device";
        } else if (!capability.operatingSystemSupported) {
            capability.reason = "backend is not supported by this operating system";
        } else if (type == AV_HWDEVICE_TYPE_VAAPI &&
                   capability.devicePath.empty()) {
            capability.reason = "no VAAPI render node is available";
        } else {
            AVBufferRef* deviceContext = nullptr;
            const char* device = capability.devicePath.empty()
                ? nullptr
                : capability.devicePath.c_str();
            const int result = av_hwdevice_ctx_create(
                &deviceContext, type, device, nullptr, 0);
            capability.available = result >= 0 && deviceContext;
            av_buffer_unref(&deviceContext);
            capability.reason = capability.available
                ? "hardware decoder device is available"
                : "hardware decoder device could not be opened";
        }

        void* iterator = nullptr;
        while (const AVCodec* candidate = av_codec_iterate(&iterator)) {
            if (!av_codec_is_decoder(candidate) ||
                candidate != avcodec_find_decoder(candidate->id) ||
                !decoderSupportsDevice(candidate, type)) {
                continue;
            }
            if (std::find(capability.codecIds.begin(),
                          capability.codecIds.end(),
                          candidate->id) == capability.codecIds.end()) {
                capability.codecIds.push_back(candidate->id);
            }
        }
        std::sort(capability.codecIds.begin(), capability.codecIds.end());
        capabilities.push_back(std::move(capability));
    };

#if defined(__APPLE__)
    addCandidate(AV_HWDEVICE_TYPE_VIDEOTOOLBOX,
                 "videotoolbox", "VideoToolbox", 100, true);
#elif defined(_WIN32)
    addCandidate(AV_HWDEVICE_TYPE_D3D11VA,
                 "d3d11va", "D3D11 Video Acceleration", 100, true);
    addCandidate(AV_HWDEVICE_TYPE_DXVA2,
                 "dxva2", "DXVA2", 80, true);
#elif defined(__linux__)
    addCandidate(AV_HWDEVICE_TYPE_CUDA,
                 "cuda", "NVIDIA CUDA/NVDEC", 100, true);
    addCandidate(AV_HWDEVICE_TYPE_VAAPI,
                 "vaapi", "VAAPI", 80, true);
#else
    addCandidate(AV_HWDEVICE_TYPE_CUDA,
                 "cuda", "NVIDIA CUDA/NVDEC", 100, false);
    addCandidate(AV_HWDEVICE_TYPE_VAAPI,
                 "vaapi", "VAAPI", 80, false);
#endif

    std::stable_sort(
        capabilities.begin(), capabilities.end(),
        [](const VideoDecodeBackendCapability& left,
           const VideoDecodeBackendCapability& right) {
            if (left.available != right.available) {
                return left.available > right.available;
            }
            return left.preference > right.preference;
        });
    return capabilities;
}

} // namespace

const std::vector<VideoDecodeBackendCapability>&
detectedVideoDecodeCapabilities()
{
    static const std::vector<VideoDecodeBackendCapability> capabilities =
        probeVideoDecodeCapabilities();
    return capabilities;
}

bool videoDecodeBackendSupportsCodec(
    const VideoDecodeBackendCapability& capability,
    AVCodecID codecId)
{
    return codecId == AV_CODEC_ID_NONE ||
        std::find(capability.codecIds.begin(), capability.codecIds.end(),
                  codecId) != capability.codecIds.end();
}

std::vector<AVHWDeviceType> preferredVideoDecodeDeviceOrder(AVCodecID codecId)
{
    std::vector<AVHWDeviceType> result;
    for (const VideoDecodeBackendCapability& capability :
         detectedVideoDecodeCapabilities()) {
        if (capability.available &&
            videoDecodeBackendSupportsCodec(capability, codecId)) {
            result.push_back(capability.deviceType);
        }
    }
    return result;
}

const VideoDecodeBackendCapability* videoDecodeCapability(
    AVHWDeviceType deviceType)
{
    const auto& capabilities = detectedVideoDecodeCapabilities();
    const auto found = std::find_if(
        capabilities.begin(), capabilities.end(),
        [deviceType](const VideoDecodeBackendCapability& capability) {
            return capability.deviceType == deviceType;
        });
    return found == capabilities.end() ? nullptr : &*found;
}

} // namespace jcut
