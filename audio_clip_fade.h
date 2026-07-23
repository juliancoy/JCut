#pragma once

#include <algorithm>
#include <cstdint>

namespace editor::audio {

inline constexpr int kDefaultClipFadeSamples = 250;

inline int effectiveClipFadeSamples(int configuredFadeSamples)
{
    return configuredFadeSamples > 0
        ? configuredFadeSamples
        : kDefaultClipFadeSamples;
}

inline float clipFadeGain(int64_t samplePosition,
                          int64_t clipStartSample,
                          int64_t clipEndSample,
                          int fadeSamples)
{
    if (fadeSamples <= 0) {
        return 1.0f;
    }

    float gain = 1.0f;
    const int64_t samplesFromStart = samplePosition - clipStartSample;
    if (samplesFromStart >= 0 && samplesFromStart < fadeSamples) {
        gain *= static_cast<float>(samplesFromStart) /
            static_cast<float>(fadeSamples);
    }
    const int64_t samplesToEnd = clipEndSample - samplePosition;
    if (samplesToEnd >= 0 && samplesToEnd < fadeSamples) {
        gain *= static_cast<float>(samplesToEnd) /
            static_cast<float>(fadeSamples);
    }
    return std::clamp(gain, 0.0f, 1.0f);
}

} // namespace editor::audio
