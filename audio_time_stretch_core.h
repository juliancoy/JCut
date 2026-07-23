#pragma once

#include <cstddef>
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

using StretchInputProvider = std::function<bool(
    std::size_t startFrame,
    std::size_t frameCount,
    float* interleavedOutput)>;
using StretchOutputConsumer = std::function<bool(
    const float* interleavedSamples,
    std::size_t frameCount)>;

// Two-pass, bounded-memory offline stretch. The provider must return the same
// input for the study and process passes. Output is normalized to exactly
// round(inputFrameCount / speed) frames before it reaches the consumer.
[[nodiscard]] bool streamTimeStretchPreservePitch(
    std::size_t inputFrameCount,
    int channelCount,
    int sampleRate,
    double speed,
    const StretchInputProvider& inputProvider,
    const StretchOutputConsumer& outputConsumer,
    const std::function<void(double)>& progressCallback = {},
    const StretchSettings& settings = {});

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

// Qt's "Harmonic Speech Isolation" treatment: a two-stage Rubber Band pass
// that preserves the requested transport duration while attenuating
// non-harmonic content.
inline constexpr int kSpeechHarmonicIsolationAlgorithmVersion = 1;

[[nodiscard]] std::vector<float> isolateSpeechHarmonics(
    const std::vector<float>& interleavedSamples,
    int channelCount,
    int sampleRate,
    double transportSpeed,
    const std::function<void(double)>& progressCallback = {},
    const StretchSettings& settings = {});

} // namespace jcut::audio
