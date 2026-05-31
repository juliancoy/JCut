#include "audio_time_stretch.h"

#include <QtGlobal>

#include <algorithm>
#include <cmath>
#include <functional>
#include <limits>

#ifndef JCUT_HAVE_RUBBERBAND
#define JCUT_HAVE_RUBBERBAND 0
#endif

#if JCUT_HAVE_RUBBERBAND
#include <rubberband/RubberBandStretcher.h>
#endif

namespace {
constexpr double kPi = 3.141592653589793238462643383279502884;

[[maybe_unused]] float sampleAt(const QVector<float>& samples, int frame, int channel, int channels)
{
    if (frame < 0) {
        return 0.0f;
    }
    const qsizetype index = static_cast<qsizetype>(frame) * channels + channel;
    if (index < 0 || index >= samples.size()) {
        return 0.0f;
    }
    return samples.at(index);
}

[[maybe_unused]] float hann(int index, int length)
{
    if (length <= 1) {
        return 1.0f;
    }
    return static_cast<float>(0.5 - (0.5 * std::cos((2.0 * kPi * index) / (length - 1))));
}

[[maybe_unused]] double overlapCorrelation(const QVector<float>& output,
                                           int outputFrame,
                                           const QVector<float>& input,
                                           int inputFrame,
                                           int overlapFrames,
                                           int channelCount)
{
    double dot = 0.0;
    double outEnergy = 0.0;
    double inEnergy = 0.0;
    for (int i = 0; i < overlapFrames; ++i) {
        for (int ch = 0; ch < channelCount; ++ch) {
            const float a = sampleAt(output, outputFrame + i, ch, channelCount);
            const float b = sampleAt(input, inputFrame + i, ch, channelCount);
            dot += static_cast<double>(a) * static_cast<double>(b);
            outEnergy += static_cast<double>(a) * static_cast<double>(a);
            inEnergy += static_cast<double>(b) * static_cast<double>(b);
        }
    }
    if (outEnergy <= 1.0e-12 || inEnergy <= 1.0e-12) {
        return 0.0;
    }
    return dot / std::sqrt(outEnergy * inEnergy);
}
}

QVector<float> timeStretchPreservePitch(const QVector<float>& interleavedSamples,
                                        int channelCount,
                                        int sampleRate,
                                        double speed,
                                        AudioTimeStretchBackend backend,
                                        const std::function<void(double)>& progressCallback,
                                        const AudioTimeStretchRubberBandSettings& rubberBandSettings)
{
    if (backend == AudioTimeStretchBackend::Sola) {
        return {};
    }

    if (backend == AudioTimeStretchBackend::RubberBand ||
        backend == AudioTimeStretchBackend::Default) {
        QVector<float> rubberBandOutput =
            timeStretchPreservePitchRubberBand(interleavedSamples,
                                               channelCount,
                                               sampleRate,
                                               speed,
                                               progressCallback,
                                               rubberBandSettings);
        return rubberBandOutput;
    }

    return {};
}

QVector<float> timeStretchPreservePitchRubberBand(const QVector<float>& interleavedSamples,
                                                  int channelCount,
                                                  int sampleRate,
                                                  double speed,
                                                  const std::function<void(double)>& progressCallback,
                                                  const AudioTimeStretchRubberBandSettings& settings)
{
#if JCUT_HAVE_RUBBERBAND
    auto reportProgress = [&](double value) {
        if (progressCallback) {
            progressCallback(std::clamp(value, 0.0, 1.0));
        }
    };
    const int channels = qMax(1, channelCount);
    const int effectiveSampleRate = qMax(1, sampleRate);
    if (interleavedSamples.isEmpty() || channels <= 0 || speed <= 0.0) {
        return {};
    }
    if (std::abs(speed - 1.0) < 0.0001) {
        reportProgress(1.0);
        return interleavedSamples;
    }

    const qsizetype inputFrames = interleavedSamples.size() / channels;
    if (inputFrames <= 0) {
        return {};
    }

    const double timeRatio = 1.0 / speed;
    RubberBand::RubberBandStretcher::Options options =
        RubberBand::RubberBandStretcher::OptionProcessOffline;
    options = static_cast<RubberBand::RubberBandStretcher::Options>(
        options |
        (settings.engine == RubberBandEngineMode::Faster
             ? RubberBand::RubberBandStretcher::OptionEngineFaster
             : RubberBand::RubberBandStretcher::OptionEngineFiner));
    switch (settings.threading) {
    case RubberBandThreadingMode::Never:
        options = static_cast<RubberBand::RubberBandStretcher::Options>(
            options | RubberBand::RubberBandStretcher::OptionThreadingNever);
        break;
    case RubberBandThreadingMode::Always:
        options = static_cast<RubberBand::RubberBandStretcher::Options>(
            options | RubberBand::RubberBandStretcher::OptionThreadingAlways);
        break;
    case RubberBandThreadingMode::Auto:
        break;
    }
    switch (settings.window) {
    case RubberBandWindowMode::Short:
        options = static_cast<RubberBand::RubberBandStretcher::Options>(
            options | RubberBand::RubberBandStretcher::OptionWindowShort);
        break;
    case RubberBandWindowMode::Long:
        options = static_cast<RubberBand::RubberBandStretcher::Options>(
            options | RubberBand::RubberBandStretcher::OptionWindowLong);
        break;
    case RubberBandWindowMode::Standard:
        break;
    }
    switch (settings.pitch) {
    case RubberBandPitchMode::HighSpeed:
        options = static_cast<RubberBand::RubberBandStretcher::Options>(
            options | RubberBand::RubberBandStretcher::OptionPitchHighSpeed);
        break;
    case RubberBandPitchMode::HighConsistency:
        options = static_cast<RubberBand::RubberBandStretcher::Options>(
            options | RubberBand::RubberBandStretcher::OptionPitchHighConsistency);
        break;
    case RubberBandPitchMode::HighQuality:
        options = static_cast<RubberBand::RubberBandStretcher::Options>(
            options | RubberBand::RubberBandStretcher::OptionPitchHighQuality);
        break;
    }
    if (settings.channelsTogether) {
        options = static_cast<RubberBand::RubberBandStretcher::Options>(
            options | RubberBand::RubberBandStretcher::OptionChannelsTogether);
    }
    RubberBand::RubberBandStretcher stretcher(
        static_cast<size_t>(effectiveSampleRate),
        static_cast<size_t>(channels),
        options,
        timeRatio,
        1.0);
    stretcher.setExpectedInputDuration(static_cast<size_t>(inputFrames));

    constexpr qsizetype kPreferredProcessFrames = 65536;
    const qsizetype processFrames =
        qMax<qsizetype>(1, qMin<qsizetype>(kPreferredProcessFrames,
                                           static_cast<qsizetype>(stretcher.getProcessSizeLimit())));
    stretcher.setMaxProcessSize(static_cast<size_t>(processFrames));

    QVector<QVector<float>> inputBlock(channels);
    QVector<const float*> inputPointers(channels);
    for (int ch = 0; ch < channels; ++ch) {
        inputBlock[ch].resize(processFrames);
        inputPointers[ch] = inputBlock[ch].constData();
    }

    auto fillInputBlock = [&](qsizetype startFrame, qsizetype frameCount) {
        for (int ch = 0; ch < channels; ++ch) {
            float* channelBlock = inputBlock[ch].data();
            for (qsizetype frame = 0; frame < frameCount; ++frame) {
                channelBlock[frame] =
                    interleavedSamples[(startFrame + frame) * channels + ch];
            }
        }
    };

    auto runInputPass = [&](double progressStart, double progressEnd, auto&& pass) {
        for (qsizetype startFrame = 0; startFrame < inputFrames; startFrame += processFrames) {
            const qsizetype frameCount = qMin(processFrames, inputFrames - startFrame);
            fillInputBlock(startFrame, frameCount);
            const bool final = (startFrame + frameCount) >= inputFrames;
            pass(inputPointers.constData(), static_cast<size_t>(frameCount), final);
            const double fraction =
                static_cast<double>(startFrame + frameCount) / static_cast<double>(inputFrames);
            reportProgress(progressStart + ((progressEnd - progressStart) * fraction));
        }
    };

    reportProgress(0.0);
    runInputPass(0.0, 0.20, [&](const float *const *block, size_t frameCount, bool final) {
        stretcher.study(block, frameCount, final);
    });

    const qsizetype expectedFrames =
        qMax<qsizetype>(1, static_cast<qsizetype>(std::llround(inputFrames / speed)));
    const qsizetype expectedSamples = expectedFrames * channels;
    if (expectedSamples <= 0 || expectedSamples > std::numeric_limits<qsizetype>::max() / 2) {
        return {};
    }

    constexpr qsizetype kRetrieveFrames = 65536;
    QVector<QVector<float>> planarOutput(channels);
    for (int ch = 0; ch < channels; ++ch) {
        planarOutput[ch].resize(kRetrieveFrames);
    }
    QVector<float*> outputPointers(channels);
    for (int ch = 0; ch < channels; ++ch) {
        outputPointers[ch] = planarOutput[ch].data();
    }

    QVector<float> output;
    output.reserve(expectedSamples);

    auto drainAvailableOutput = [&]() {
        while (true) {
            const int available = stretcher.available();
            if (available <= 0) {
                return available < 0;
            }
            const qsizetype framesToRead =
                qMin<qsizetype>(static_cast<qsizetype>(available), kRetrieveFrames);
            const qsizetype readFrames = static_cast<qsizetype>(
                stretcher.retrieve(outputPointers.constData(), static_cast<size_t>(framesToRead)));
            if (readFrames <= 0) {
                return false;
            }
            const qsizetype oldSize = output.size();
            output.resize(oldSize + readFrames * channels);
            for (qsizetype frame = 0; frame < readFrames; ++frame) {
                for (int ch = 0; ch < channels; ++ch) {
                    output[oldSize + frame * channels + ch] =
                        std::clamp(planarOutput[ch][frame], -1.0f, 1.0f);
                }
            }
        }
    };

    runInputPass(0.20, 0.95, [&](const float *const *block, size_t frameCount, bool final) {
        stretcher.process(block, frameCount, final);
        drainAvailableOutput();
    });

    for (int drainAttempts = 0; drainAttempts < 1024; ++drainAttempts) {
        if (drainAvailableOutput()) {
            break;
        }
    }

    if (output.isEmpty()) {
        return {};
    }
    reportProgress(1.0);
    return output;
#else
    Q_UNUSED(progressCallback);
    Q_UNUSED(sampleRate);
    Q_UNUSED(speed);
    Q_UNUSED(channelCount);
    Q_UNUSED(interleavedSamples);
    return {};
#endif
}

QVector<float> timeStretchPreservePitchSola(const QVector<float>& interleavedSamples,
                                            int channelCount,
                                            double speed)
{
    Q_UNUSED(interleavedSamples);
    Q_UNUSED(channelCount);
    Q_UNUSED(speed);
    return {};
}
