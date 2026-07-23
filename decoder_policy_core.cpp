#include "decoder_policy_core.h"

#include <algorithm>
#include <cctype>

namespace jcut {
namespace {

std::string normalized(std::string value)
{
    value.erase(
        value.begin(),
        std::find_if(value.begin(), value.end(), [](unsigned char character) {
            return !std::isspace(character);
        }));
    value.erase(
        std::find_if(
            value.rbegin(), value.rend(), [](unsigned char character) {
                return !std::isspace(character);
            }).base(),
        value.end());
    std::transform(
        value.begin(), value.end(), value.begin(), [](unsigned char character) {
            return static_cast<char>(std::tolower(character));
        });
    return value;
}

} // namespace

std::string decodePreferenceCoreName(DecodePreferenceCore preference)
{
    switch (preference) {
    case DecodePreferenceCore::Auto: return "auto";
    case DecodePreferenceCore::HardwareZeroCopy: return "hardware_zero_copy";
    case DecodePreferenceCore::Hardware: return "hardware";
    case DecodePreferenceCore::Software: return "software";
    }
    return "auto";
}

bool parseDecodePreferenceCore(const std::string& text,
                               DecodePreferenceCore* preferenceOut)
{
    if (!preferenceOut) return false;
    const std::string value = normalized(text);
    if (value == "auto") {
        *preferenceOut = DecodePreferenceCore::Auto;
    } else if (value == "hardware_zero_copy" || value == "zero_copy" ||
               value == "zerocopy" || value == "cuda_gl") {
        *preferenceOut = DecodePreferenceCore::HardwareZeroCopy;
    } else if (value == "hardware" || value == "gpu" ||
               value == "prefer_hardware") {
        *preferenceOut = DecodePreferenceCore::Hardware;
    } else if (value == "software" || value == "cpu" ||
               value == "software_only") {
        *preferenceOut = DecodePreferenceCore::Software;
    } else {
        return false;
    }
    return true;
}

std::string h26xThreadingModeCoreName(H26xThreadingModeCore mode)
{
    switch (mode) {
    case H26xThreadingModeCore::Auto: return "auto";
    case H26xThreadingModeCore::SingleThread: return "single_thread";
    case H26xThreadingModeCore::SliceThreads: return "slice_threads";
    case H26xThreadingModeCore::FrameAndSliceThreads:
        return "frame_and_slice_threads";
    }
    return "auto";
}

bool parseH26xThreadingModeCore(const std::string& text,
                                H26xThreadingModeCore* modeOut)
{
    if (!modeOut) return false;
    const std::string value = normalized(text);
    if (value == "auto") {
        *modeOut = H26xThreadingModeCore::Auto;
    } else if (value == "single_thread" || value == "single" ||
               value == "stability") {
        *modeOut = H26xThreadingModeCore::SingleThread;
    } else if (value == "slice_threads" || value == "slice" ||
               value == "balanced") {
        *modeOut = H26xThreadingModeCore::SliceThreads;
    } else if (value == "frame_and_slice_threads" ||
               value == "frame_slice" || value == "performance") {
        *modeOut = H26xThreadingModeCore::FrameAndSliceThreads;
    } else {
        return false;
    }
    return true;
}

std::string decodeHardwareDeviceCoreName(
    DecodeHardwareDeviceCore device)
{
    switch (device) {
    case DecodeHardwareDeviceCore::Auto: return "auto";
    case DecodeHardwareDeviceCore::Cuda: return "cuda";
    case DecodeHardwareDeviceCore::Vaapi: return "vaapi";
    case DecodeHardwareDeviceCore::VideoToolbox: return "videotoolbox";
    case DecodeHardwareDeviceCore::D3d11va: return "d3d11va";
    case DecodeHardwareDeviceCore::Dxva2: return "dxva2";
    }
    return "auto";
}

bool parseDecodeHardwareDeviceCore(
    const std::string& text,
    DecodeHardwareDeviceCore* deviceOut)
{
    if (!deviceOut) return false;
    const std::string value = normalized(text);
    if (value.empty() || value == "auto" || value == "default") {
        *deviceOut = DecodeHardwareDeviceCore::Auto;
    } else if (value == "cuda" || value == "nvidia") {
        *deviceOut = DecodeHardwareDeviceCore::Cuda;
    } else if (value == "vaapi" || value == "va-api") {
        *deviceOut = DecodeHardwareDeviceCore::Vaapi;
    } else if (value == "videotoolbox" ||
               value == "video_toolbox") {
        *deviceOut = DecodeHardwareDeviceCore::VideoToolbox;
    } else if (value == "d3d11va" || value == "d3d11") {
        *deviceOut = DecodeHardwareDeviceCore::D3d11va;
    } else if (value == "dxva2" || value == "dxva") {
        *deviceOut = DecodeHardwareDeviceCore::Dxva2;
    } else {
        return false;
    }
    return true;
}

EffectiveDecoderPolicyCore effectiveDecoderPolicyCore(
    DecoderPolicySettingsCore requested,
    bool hardwareDecodeAvailable,
    bool zeroCopyAvailable,
    int logicalCpuCount)
{
    requested.decoderLaneCount =
        std::clamp(requested.decoderLaneCount, 0, 16);
    EffectiveDecoderPolicyCore effective;
    effective.requested = requested;

    const bool wantsZeroCopy =
        requested.decodePreference ==
        DecodePreferenceCore::HardwareZeroCopy;
    const bool wantsHardware =
        wantsZeroCopy ||
        requested.decodePreference == DecodePreferenceCore::Hardware ||
        requested.decodePreference == DecodePreferenceCore::Auto;
    if (wantsZeroCopy && zeroCopyAvailable) {
        effective.effectivePreference =
            DecodePreferenceCore::HardwareZeroCopy;
    } else if (wantsHardware && hardwareDecodeAvailable) {
        effective.effectivePreference = DecodePreferenceCore::Hardware;
        effective.hardwareFallback = wantsZeroCopy;
    } else {
        effective.effectivePreference = DecodePreferenceCore::Software;
        effective.hardwareFallback =
            requested.decodePreference != DecodePreferenceCore::Software;
    }

    const int threads = std::clamp(logicalCpuCount, 2, 8);
    if (requested.deterministic ||
        requested.h26xThreadingMode ==
            H26xThreadingModeCore::SingleThread ||
        effective.effectivePreference != DecodePreferenceCore::Software) {
        effective.softwareThreadCount = 1;
        effective.softwareThreadType = 0;
    } else if (requested.h26xThreadingMode ==
               H26xThreadingModeCore::SliceThreads) {
        effective.softwareThreadCount = threads;
        effective.softwareThreadType = 1;
    } else if (requested.h26xThreadingMode ==
               H26xThreadingModeCore::FrameAndSliceThreads) {
        effective.softwareThreadCount = threads;
        effective.softwareThreadType = 3;
    } else {
        // Match Qt's stability-first H.264/H.265 software default.
        effective.softwareThreadCount = 1;
        effective.softwareThreadType = 0;
    }
    return effective;
}

} // namespace jcut
