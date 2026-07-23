#include "ffmpeg_hardware_device_core.h"

#include <cstdint>
#include <filesystem>
#include <sstream>

extern "C" {
#include <libavutil/error.h>
}

namespace jcut {
namespace {

std::string ffmpegError(int code)
{
    char buffer[AV_ERROR_MAX_STRING_SIZE] = {};
    av_strerror(code, buffer, sizeof(buffer));
    return buffer;
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

} // namespace

std::vector<AVHWDeviceType> defaultFfmpegHardwareDeviceOrder()
{
    std::vector<AVHWDeviceType> result;
#if defined(__APPLE__)
    result.push_back(AV_HWDEVICE_TYPE_VIDEOTOOLBOX);
#elif defined(_WIN32)
    result.push_back(AV_HWDEVICE_TYPE_D3D11VA);
    result.push_back(AV_HWDEVICE_TYPE_DXVA2);
#else
    result.push_back(AV_HWDEVICE_TYPE_CUDA);
    result.push_back(AV_HWDEVICE_TYPE_VAAPI);
#endif
    return result;
}

std::vector<AVHWDeviceType> ffmpegHardwareDeviceOrder(
    DecodeHardwareDeviceCore preference)
{
    switch (preference) {
    case DecodeHardwareDeviceCore::Cuda:
        return {AV_HWDEVICE_TYPE_CUDA};
    case DecodeHardwareDeviceCore::Vaapi:
        return {AV_HWDEVICE_TYPE_VAAPI};
    case DecodeHardwareDeviceCore::VideoToolbox:
        return {AV_HWDEVICE_TYPE_VIDEOTOOLBOX};
    case DecodeHardwareDeviceCore::D3d11va:
        return {AV_HWDEVICE_TYPE_D3D11VA};
    case DecodeHardwareDeviceCore::Dxva2:
        return {AV_HWDEVICE_TYPE_DXVA2};
    case DecodeHardwareDeviceCore::Auto:
    default:
        return defaultFfmpegHardwareDeviceOrder();
    }
}

FfmpegHardwareDeviceSetup createFfmpegHardwareDeviceForDecoder(
    const AVCodec* decoder,
    const std::vector<AVHWDeviceType>& preferredTypes)
{
    FfmpegHardwareDeviceSetup result;
    if (!decoder) {
        result.error = "decoder is null";
        return result;
    }
    const std::vector<AVHWDeviceType> types = preferredTypes.empty()
        ? defaultFfmpegHardwareDeviceOrder()
        : preferredTypes;
    std::ostringstream failures;
    for (const AVHWDeviceType type : types) {
        const AVCodecHWConfig* selectedConfig = nullptr;
        for (int index = 0;; ++index) {
            const AVCodecHWConfig* config =
                avcodec_get_hw_config(decoder, index);
            if (!config) break;
            if (config->device_type == type &&
                (config->methods &
                 AV_CODEC_HW_CONFIG_METHOD_HW_DEVICE_CTX)) {
                selectedConfig = config;
                break;
            }
        }
        if (!selectedConfig) continue;

        std::string devicePath;
        if (type == AV_HWDEVICE_TYPE_VAAPI) {
            devicePath = vaapiRenderNode();
            if (devicePath.empty()) {
                failures << "vaapi: no render node; ";
                continue;
            }
        }
        AVBufferRef* deviceContext = nullptr;
        const int createResult = av_hwdevice_ctx_create(
            &deviceContext,
            type,
            devicePath.empty() ? nullptr : devicePath.c_str(),
            nullptr,
            0);
        if (createResult < 0 || !deviceContext) {
            const char* typeName = av_hwdevice_get_type_name(type);
            failures << (typeName ? typeName : "unknown") << ": "
                     << ffmpegError(createResult) << "; ";
            av_buffer_unref(&deviceContext);
            continue;
        }
        result.deviceContext = deviceContext;
        result.hardwarePixelFormat = selectedConfig->pix_fmt;
        result.deviceType = type;
        const char* typeName = av_hwdevice_get_type_name(type);
        result.deviceLabel = typeName ? typeName : "hardware";
        return result;
    }
    result.error = failures.str();
    if (result.error.empty()) {
        result.error = "decoder exposes no supported hardware device context";
    }
    return result;
}

AVPixelFormat selectFfmpegHardwarePixelFormat(
    AVCodecContext* context,
    const AVPixelFormat* formats)
{
    if (!context || !formats) return AV_PIX_FMT_NONE;
    const AVPixelFormat preferred = static_cast<AVPixelFormat>(
        reinterpret_cast<std::intptr_t>(context->opaque));
    for (const AVPixelFormat* format = formats;
         *format != AV_PIX_FMT_NONE;
         ++format) {
        if (*format == preferred) return *format;
    }
    return AV_PIX_FMT_NONE;
}

} // namespace jcut
