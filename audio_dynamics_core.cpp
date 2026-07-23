#include "audio_dynamics_core.h"

#include <algorithm>
#include <cmath>
#include <vector>

namespace jcut::audio {

DynamicsSettingsCore normalizedDynamicsSettingsCore(
    DynamicsSettingsCore settings)
{
    settings.amplifyDb = std::clamp(settings.amplifyDb, -24.0, 24.0);
    settings.normalizeTargetDb =
        std::clamp(settings.normalizeTargetDb, -24.0, 0.0);
    settings.selectiveNormalizeMinSegmentSeconds = std::clamp(
        settings.selectiveNormalizeMinSegmentSeconds, 0.1, 30.0);
    settings.selectiveNormalizePeakDb =
        std::clamp(settings.selectiveNormalizePeakDb, -36.0, 0.0);
    settings.selectiveNormalizePasses =
        std::clamp(settings.selectiveNormalizePasses, 1, 8);
    settings.peakThresholdDb =
        std::clamp(settings.peakThresholdDb, -24.0, 0.0);
    settings.limiterThresholdDb =
        std::clamp(settings.limiterThresholdDb, -12.0, 0.0);
    settings.compressorThresholdDb =
        std::clamp(settings.compressorThresholdDb, -30.0, -1.0);
    settings.compressorRatio =
        std::clamp(settings.compressorRatio, 1.0, 20.0);
    return settings;
}

void processAudioDynamicsCore(float* samples,
                              int frames,
                              int channelCount,
                              int sampleRate,
                              const DynamicsSettingsCore& requested)
{
    if (!samples || frames <= 0 || channelCount <= 0) return;
    const DynamicsSettingsCore settings =
        normalizedDynamicsSettingsCore(requested);
    const auto dbToAmplitude = [](double db) {
        return static_cast<float>(std::pow(10.0, db / 20.0));
    };
    const float amplifyGain = settings.amplifyEnabled
        ? dbToAmplitude(settings.amplifyDb) : 1.0f;
    const float normalizeTarget =
        dbToAmplitude(settings.normalizeTargetDb);
    const float selectiveThreshold =
        dbToAmplitude(settings.selectiveNormalizePeakDb);
    const float peakThreshold =
        dbToAmplitude(settings.peakThresholdDb);
    const float limiterThreshold =
        dbToAmplitude(settings.limiterThresholdDb);
    const float compressorThreshold =
        dbToAmplitude(settings.compressorThresholdDb);
    const float compressorRatio =
        static_cast<float>(settings.compressorRatio);

    const int sampleCount = frames * channelCount;
    for (int index = 0; index < sampleCount; ++index) {
        const float sign = samples[index] < 0.0f ? -1.0f : 1.0f;
        float value = std::abs(samples[index]) * amplifyGain;
        if (settings.compressorEnabled &&
            value > compressorThreshold) {
            value = compressorThreshold +
                (value - compressorThreshold) / compressorRatio;
        }
        if (settings.peakReductionEnabled &&
            value > peakThreshold) {
            value = peakThreshold +
                (value - peakThreshold) * 0.35f;
        }
        if (settings.softClipEnabled) {
            constexpr float kDrive = 1.75f;
            constexpr float kNormalization = 1.0f / 0.94137555f;
            value = std::tanh(value * kDrive) * kNormalization;
        }
        if (settings.limiterEnabled) {
            value = std::min(value, limiterThreshold);
        }
        samples[index] = std::clamp(sign * value, -1.0f, 1.0f);
    }

    if (settings.selectiveNormalizeEnabled) {
        constexpr int kWindowFrames = 256;
        constexpr float kTarget = 0.95f;
        const int binCount = std::max(
            1, static_cast<int>(std::ceil(
                static_cast<double>(frames) / kWindowFrames)));
        std::vector<float> peaks(static_cast<std::size_t>(binCount));
        const auto rebuildPeaks = [&]() {
            std::fill(peaks.begin(), peaks.end(), 0.0f);
            for (int frame = 0; frame < frames; ++frame) {
                float peak = 0.0f;
                for (int channel = 0; channel < channelCount; ++channel) {
                    peak = std::max(
                        peak,
                        std::abs(samples[frame * channelCount + channel]));
                }
                peaks[static_cast<std::size_t>(
                    std::min(binCount - 1, frame / kWindowFrames))] =
                    std::max(
                        peaks[static_cast<std::size_t>(
                            std::min(binCount - 1, frame / kWindowFrames))],
                        peak);
            }
        };
        rebuildPeaks();
        const int minimumBins = std::max(
            1, static_cast<int>(std::ceil(
                settings.selectiveNormalizeMinSegmentSeconds *
                std::max(1, sampleRate) / kWindowFrames)));
        for (int pass = 0;
             pass < settings.selectiveNormalizePasses;
             ++pass) {
            std::vector<int> localPeaks;
            for (int bin = 0; bin < binCount; ++bin) {
                const float value = peaks[static_cast<std::size_t>(bin)];
                if (value < selectiveThreshold) continue;
                const float left = bin > 0
                    ? peaks[static_cast<std::size_t>(bin - 1)] : value;
                const float right = bin + 1 < binCount
                    ? peaks[static_cast<std::size_t>(bin + 1)] : value;
                if (value >= left && value >= right) {
                    localPeaks.push_back(bin);
                }
            }
            if (binCount >= 2) {
                if (localPeaks.empty() || localPeaks.front() != 0) {
                    localPeaks.insert(localPeaks.begin(), 0);
                }
                if (localPeaks.back() != binCount - 1) {
                    localPeaks.push_back(binCount - 1);
                }
            }
            if (localPeaks.size() < 2) break;
            for (std::size_t peakIndex = 0;
                 peakIndex + 1 < localPeaks.size();
                 ++peakIndex) {
                const int firstBin = localPeaks[peakIndex];
                const int lastBin = localPeaks[peakIndex + 1];
                if (lastBin - firstBin + 1 < minimumBins) continue;
                float segmentPeak = 0.0f;
                bool aboveThreshold = false;
                for (int bin = firstBin; bin <= lastBin; ++bin) {
                    segmentPeak = std::max(
                        segmentPeak, peaks[static_cast<std::size_t>(bin)]);
                    aboveThreshold |=
                        peaks[static_cast<std::size_t>(bin)] >=
                        selectiveThreshold;
                }
                if (!aboveThreshold || segmentPeak <= 0.000001f) continue;
                const float gain = kTarget / segmentPeak;
                const int firstFrame = firstBin * kWindowFrames;
                const int lastFrame = std::min(
                    frames, (lastBin + 1) * kWindowFrames);
                for (int frame = firstFrame;
                     frame < lastFrame;
                     ++frame) {
                    for (int channel = 0;
                         channel < channelCount;
                         ++channel) {
                        const int index =
                            frame * channelCount + channel;
                        samples[index] = std::clamp(
                            samples[index] * gain, -1.0f, 1.0f);
                    }
                }
            }
            rebuildPeaks();
        }
    }

    if (settings.normalizeEnabled) {
        float peak = 0.0f;
        for (int index = 0; index < sampleCount; ++index) {
            peak = std::max(peak, std::abs(samples[index]));
        }
        if (peak > 0.000001f) {
            const float gain = normalizeTarget / peak;
            for (int index = 0; index < sampleCount; ++index) {
                samples[index] =
                    std::clamp(samples[index] * gain, -1.0f, 1.0f);
            }
        }
    }

    if (settings.stereoToMonoEnabled && channelCount > 1) {
        for (int frame = 0; frame < frames; ++frame) {
            float mono = 0.0f;
            for (int channel = 0; channel < channelCount; ++channel) {
                mono += samples[frame * channelCount + channel];
            }
            mono /= static_cast<float>(channelCount);
            for (int channel = 0; channel < channelCount; ++channel) {
                samples[frame * channelCount + channel] = mono;
            }
        }
    }
}

} // namespace jcut::audio
