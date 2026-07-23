#pragma once

#include <string>

namespace jcut {

enum class DecodePreferenceCore {
    Auto = 0,
    HardwareZeroCopy = 1,
    Hardware = 2,
    Software = 3,
};

enum class H26xThreadingModeCore {
    Auto = 0,
    SingleThread = 1,
    SliceThreads = 2,
    FrameAndSliceThreads = 3,
};

enum class DecodeHardwareDeviceCore {
    Auto = 0,
    Cuda = 1,
    Vaapi = 2,
    VideoToolbox = 3,
    D3d11va = 4,
    Dxva2 = 5,
};

struct DecoderPolicySettingsCore {
    DecodePreferenceCore decodePreference = DecodePreferenceCore::Auto;
    H26xThreadingModeCore h26xThreadingMode =
        H26xThreadingModeCore::Auto;
    DecodeHardwareDeviceCore hardwareDevice =
        DecodeHardwareDeviceCore::Auto;
    bool deterministic = false;
    int decoderLaneCount = 0;

    bool operator==(const DecoderPolicySettingsCore&) const = default;
};

struct EffectiveDecoderPolicyCore {
    DecoderPolicySettingsCore requested;
    DecodePreferenceCore effectivePreference =
        DecodePreferenceCore::Software;
    int softwareThreadCount = 1;
    int softwareThreadType = 0;
    bool hardwareFallback = false;
};

std::string decodePreferenceCoreName(DecodePreferenceCore preference);
bool parseDecodePreferenceCore(const std::string& text,
                               DecodePreferenceCore* preferenceOut);
std::string h26xThreadingModeCoreName(H26xThreadingModeCore mode);
bool parseH26xThreadingModeCore(const std::string& text,
                                H26xThreadingModeCore* modeOut);
std::string decodeHardwareDeviceCoreName(
    DecodeHardwareDeviceCore device);
bool parseDecodeHardwareDeviceCore(
    const std::string& text,
    DecodeHardwareDeviceCore* deviceOut);

EffectiveDecoderPolicyCore effectiveDecoderPolicyCore(
    DecoderPolicySettingsCore requested,
    bool hardwareDecodeAvailable,
    bool zeroCopyAvailable,
    int logicalCpuCount);

} // namespace jcut
