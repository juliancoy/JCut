#pragma once

#include <cstdint>
#include <vector>

namespace jcut::audio {

struct WaveformSampleView {
    const float* interleavedSamples = nullptr;
    std::int64_t frameCount = 0;
    int channelCount = 0;
    std::int64_t sourceStartSample = 0;
    double sourceSampleScale = 1.0;
};

// Produces one min/max pair per output column for an absolute canonical
// source-sample range. This is the common envelope boundary used by the Qt
// comparison fixtures and the ImGui timeline.
[[nodiscard]] bool queryWaveformEnvelope(
    const WaveformSampleView& samples,
    std::int64_t sourceStartSample,
    std::int64_t sourceEndSampleExclusive,
    int columns,
    std::vector<float>* minimumOut,
    std::vector<float>* maximumOut);

} // namespace jcut::audio
