#pragma once

#include <functional>
#include <vector>

namespace jcut::audio {

enum class StretchEngine { Faster, Finer };
enum class StretchThreading { Auto, Never, Always };
enum class StretchWindow { Standard, Short, Long };
enum class StretchPitch { HighSpeed, HighQuality, HighConsistency };

struct StretchSettings {
    StretchEngine engine = StretchEngine::Faster;
    StretchThreading threading = StretchThreading::Always;
    StretchWindow window = StretchWindow::Standard;
    StretchPitch pitch = StretchPitch::HighSpeed;
    bool channelsTogether = true;
};

// Returns interleaved samples whose duration is input/speed while retaining
// the original pitch. An empty result means the backend is unavailable or the
// input/settings are invalid.
[[nodiscard]] std::vector<float> timeStretchPreservePitch(
    const std::vector<float>& interleavedSamples,
    int channelCount,
    int sampleRate,
    double speed,
    const std::function<void(double)>& progressCallback = {},
    const StretchSettings& settings = {});

} // namespace jcut::audio
