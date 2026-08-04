#pragma once

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace jcut::audio {

inline constexpr int kDefaultMasterOutputAudioDelayMs = -150;
inline constexpr int kMinMasterOutputAudioDelayMs = -10000;
inline constexpr int kMaxMasterOutputAudioDelayMs = 10000;

inline int normalizedMasterOutputAudioDelayMs(int delayMs)
{
    return std::clamp(
        delayMs,
        kMinMasterOutputAudioDelayMs,
        kMaxMasterOutputAudioDelayMs);
}

// Applies a signed delay to a stream of interleaved floating-point samples
// without changing its total duration. Positive values delay the audio;
// negative values advance it. Call finish() once after the final input chunk.
class MasterOutputAudioDelay {
public:
    MasterOutputAudioDelay(int delayMs, int sampleRate, int channels)
        : m_channels(std::max(1, channels))
    {
        const int normalizedDelayMs =
            normalizedMasterOutputAudioDelayMs(delayMs);
        m_delayFrames = static_cast<std::int64_t>(std::llround(
            static_cast<long double>(normalizedDelayMs) *
            std::max(1, sampleRate) / 1000.0L));
        if (m_delayFrames > 0) {
            m_delayLine.assign(
                static_cast<std::size_t>(m_delayFrames) * m_channels,
                0.0f);
        } else if (m_delayFrames < 0) {
            m_advanceFramesRemaining = -m_delayFrames;
        }
    }

    bool active() const { return m_delayFrames != 0; }

    std::vector<float> process(const float* samples, std::size_t frameCount)
    {
        if (!samples || frameCount == 0) {
            return {};
        }
        if (m_delayFrames == 0) {
            return std::vector<float>(
                samples,
                samples + frameCount * static_cast<std::size_t>(m_channels));
        }
        if (m_delayFrames > 0) {
            std::vector<float> output(
                frameCount * static_cast<std::size_t>(m_channels));
            for (std::size_t frame = 0; frame < frameCount; ++frame) {
                const std::size_t delayBase =
                    m_delayCursor * static_cast<std::size_t>(m_channels);
                const std::size_t inputBase =
                    frame * static_cast<std::size_t>(m_channels);
                for (int channel = 0; channel < m_channels; ++channel) {
                    output[inputBase + channel] = m_delayLine[delayBase + channel];
                    m_delayLine[delayBase + channel] = samples[inputBase + channel];
                }
                m_delayCursor =
                    (m_delayCursor + 1) % static_cast<std::size_t>(m_delayFrames);
            }
            return output;
        }

        const std::size_t skippedFrames = std::min<std::size_t>(
            frameCount,
            static_cast<std::size_t>(m_advanceFramesRemaining));
        m_advanceFramesRemaining -= static_cast<std::int64_t>(skippedFrames);
        m_advanceFramesToPad += static_cast<std::int64_t>(skippedFrames);
        const std::size_t remainingFrames = frameCount - skippedFrames;
        const float* firstRemaining =
            samples + skippedFrames * static_cast<std::size_t>(m_channels);
        return std::vector<float>(
            firstRemaining,
            firstRemaining + remainingFrames * static_cast<std::size_t>(m_channels));
    }

    std::vector<float> finish()
    {
        if (m_delayFrames >= 0 || m_advanceFramesToPad <= 0) {
            return {};
        }
        std::vector<float> padding(
            static_cast<std::size_t>(m_advanceFramesToPad) * m_channels,
            0.0f);
        m_advanceFramesToPad = 0;
        return padding;
    }

private:
    int m_channels = 1;
    std::int64_t m_delayFrames = 0;
    std::int64_t m_advanceFramesRemaining = 0;
    std::int64_t m_advanceFramesToPad = 0;
    std::vector<float> m_delayLine;
    std::size_t m_delayCursor = 0;
};

} // namespace jcut::audio
