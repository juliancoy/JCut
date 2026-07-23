#include "audio_time_stretch_core.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <limits>

#ifndef JCUT_HAVE_RUBBERBAND
#define JCUT_HAVE_RUBBERBAND 0
#endif

#if JCUT_HAVE_RUBBERBAND
#include <rubberband/RubberBandStretcher.h>
#endif

namespace jcut::audio {

std::vector<float> timeStretchPreservePitch(
    const std::vector<float>& interleavedSamples,
    int channelCount,
    int sampleRate,
    double speed,
    const std::function<void(double)>& progressCallback,
    const StretchSettings& settings)
{
#if JCUT_HAVE_RUBBERBAND
    auto reportProgress = [&](double value) {
        if (progressCallback) {
            progressCallback(std::clamp(value, 0.0, 1.0));
        }
    };
    const int channels = std::max(1, channelCount);
    const int effectiveSampleRate = std::max(1, sampleRate);
    if (interleavedSamples.empty() ||
        interleavedSamples.size() % static_cast<std::size_t>(channels) != 0 ||
        !std::isfinite(speed) || speed <= 0.0) {
        return {};
    }
    if (std::abs(speed - 1.0) < 0.0001) {
        reportProgress(1.0);
        return interleavedSamples;
    }
    const std::size_t inputFrames =
        interleavedSamples.size() / static_cast<std::size_t>(channels);
    if (inputFrames == 0) {
        return {};
    }

    RubberBand::RubberBandStretcher::Options options =
        RubberBand::RubberBandStretcher::OptionProcessOffline;
    options = static_cast<RubberBand::RubberBandStretcher::Options>(
        options | (settings.engine == StretchEngine::Faster
            ? RubberBand::RubberBandStretcher::OptionEngineFaster
            : RubberBand::RubberBandStretcher::OptionEngineFiner));
    if (settings.threading == StretchThreading::Never) {
        options = static_cast<RubberBand::RubberBandStretcher::Options>(
            options | RubberBand::RubberBandStretcher::OptionThreadingNever);
    } else if (settings.threading == StretchThreading::Always) {
        options = static_cast<RubberBand::RubberBandStretcher::Options>(
            options | RubberBand::RubberBandStretcher::OptionThreadingAlways);
    }
    if (settings.window == StretchWindow::Short) {
        options = static_cast<RubberBand::RubberBandStretcher::Options>(
            options | RubberBand::RubberBandStretcher::OptionWindowShort);
    } else if (settings.window == StretchWindow::Long) {
        options = static_cast<RubberBand::RubberBandStretcher::Options>(
            options | RubberBand::RubberBandStretcher::OptionWindowLong);
    }
    if (settings.pitch == StretchPitch::HighSpeed) {
        options = static_cast<RubberBand::RubberBandStretcher::Options>(
            options | RubberBand::RubberBandStretcher::OptionPitchHighSpeed);
    } else if (settings.pitch == StretchPitch::HighConsistency) {
        options = static_cast<RubberBand::RubberBandStretcher::Options>(
            options | RubberBand::RubberBandStretcher::OptionPitchHighConsistency);
    } else {
        options = static_cast<RubberBand::RubberBandStretcher::Options>(
            options | RubberBand::RubberBandStretcher::OptionPitchHighQuality);
    }
    if (settings.channelsTogether) {
        options = static_cast<RubberBand::RubberBandStretcher::Options>(
            options | RubberBand::RubberBandStretcher::OptionChannelsTogether);
    }

    RubberBand::RubberBandStretcher stretcher(
        static_cast<std::size_t>(effectiveSampleRate),
        static_cast<std::size_t>(channels), options, 1.0 / speed, 1.0);
    stretcher.setExpectedInputDuration(inputFrames);
    constexpr std::size_t kBlockFrames = 65536;
    const std::size_t processFrames = std::max<std::size_t>(
        1, std::min(kBlockFrames, stretcher.getProcessSizeLimit()));
    stretcher.setMaxProcessSize(processFrames);

    std::vector<std::vector<float>> inputBlock(
        static_cast<std::size_t>(channels),
        std::vector<float>(processFrames));
    std::vector<const float*> inputPointers(static_cast<std::size_t>(channels));
    for (int channel = 0; channel < channels; ++channel) {
        inputPointers[static_cast<std::size_t>(channel)] =
            inputBlock[static_cast<std::size_t>(channel)].data();
    }
    auto runPass = [&](double progressStart, double progressEnd, auto&& pass) {
        for (std::size_t start = 0; start < inputFrames; start += processFrames) {
            const std::size_t count = std::min(processFrames, inputFrames - start);
            for (int channel = 0; channel < channels; ++channel) {
                float* destination = inputBlock[static_cast<std::size_t>(channel)].data();
                for (std::size_t frame = 0; frame < count; ++frame) {
                    destination[frame] = interleavedSamples[
                        (start + frame) * static_cast<std::size_t>(channels) +
                        static_cast<std::size_t>(channel)];
                }
            }
            pass(inputPointers.data(), count, start + count >= inputFrames);
            reportProgress(progressStart + (progressEnd - progressStart) *
                static_cast<double>(start + count) /
                static_cast<double>(inputFrames));
        }
    };

    reportProgress(0.0);
    runPass(0.0, 0.20, [&](const float* const* block,
                            std::size_t count, bool final) {
        stretcher.study(block, count, final);
    });

    const long double expectedFramesValue =
        static_cast<long double>(inputFrames) / speed;
    if (expectedFramesValue <= 0.0L ||
        expectedFramesValue > static_cast<long double>(
            std::numeric_limits<std::size_t>::max() /
            static_cast<std::size_t>(channels))) {
        return {};
    }
    const std::size_t expectedFrames = std::max<std::size_t>(
        1, static_cast<std::size_t>(std::llround(expectedFramesValue)));
    std::vector<std::vector<float>> planarOutput(
        static_cast<std::size_t>(channels),
        std::vector<float>(kBlockFrames));
    std::vector<float*> outputPointers(static_cast<std::size_t>(channels));
    for (int channel = 0; channel < channels; ++channel) {
        outputPointers[static_cast<std::size_t>(channel)] =
            planarOutput[static_cast<std::size_t>(channel)].data();
    }
    std::vector<float> output;
    output.reserve(expectedFrames * static_cast<std::size_t>(channels));
    auto drain = [&]() {
        while (true) {
            const int available = stretcher.available();
            if (available <= 0) {
                return available < 0;
            }
            const std::size_t count = std::min<std::size_t>(
                static_cast<std::size_t>(available), kBlockFrames);
            const std::size_t read = stretcher.retrieve(
                outputPointers.data(), count);
            if (read == 0) {
                return false;
            }
            const std::size_t oldSize = output.size();
            output.resize(oldSize + read * static_cast<std::size_t>(channels));
            for (std::size_t frame = 0; frame < read; ++frame) {
                for (int channel = 0; channel < channels; ++channel) {
                    output[oldSize + frame * static_cast<std::size_t>(channels) +
                           static_cast<std::size_t>(channel)] = std::clamp(
                        planarOutput[static_cast<std::size_t>(channel)][frame],
                        -1.0f, 1.0f);
                }
            }
        }
    };
    runPass(0.20, 0.95, [&](const float* const* block,
                             std::size_t count, bool final) {
        stretcher.process(block, count, final);
        drain();
    });
    for (int attempt = 0; attempt < 1024 && !drain(); ++attempt) {
    }
    if (output.empty()) {
        return {};
    }
    // Rubber Band may retain a backend/window-dependent tail or latency.
    // The editor's transport contract is deterministic input/speed duration,
    // so normalize the offline result at the service boundary.
    output.resize(
        expectedFrames * static_cast<std::size_t>(channels), 0.0f);
    reportProgress(1.0);
    return output;
#else
    (void)interleavedSamples;
    (void)channelCount;
    (void)sampleRate;
    (void)speed;
    (void)progressCallback;
    (void)settings;
    return {};
#endif
}

} // namespace jcut::audio
