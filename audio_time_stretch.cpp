#include "audio_time_stretch.h"

#include <QtGlobal>

#include <algorithm>
#include <cmath>

#ifndef JCUT_HAVE_RUBBERBAND
#define JCUT_HAVE_RUBBERBAND 0
#endif

#if JCUT_HAVE_RUBBERBAND
#include <rubberband/RubberBandStretcher.h>
#endif

namespace {
constexpr double kPi = 3.141592653589793238462643383279502884;

float sampleAt(const QVector<float>& samples, int frame, int channel, int channels)
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

float hann(int index, int length)
{
    if (length <= 1) {
        return 1.0f;
    }
    return static_cast<float>(0.5 - (0.5 * std::cos((2.0 * kPi * index) / (length - 1))));
}

double overlapCorrelation(const QVector<float>& output,
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
                                        AudioTimeStretchBackend backend)
{
    if (backend == AudioTimeStretchBackend::Sola) {
        return timeStretchPreservePitchSola(interleavedSamples, channelCount, speed);
    }

    if (backend == AudioTimeStretchBackend::RubberBand ||
        backend == AudioTimeStretchBackend::Default) {
        QVector<float> rubberBandOutput =
            timeStretchPreservePitchRubberBand(interleavedSamples, channelCount, sampleRate, speed);
        if (!rubberBandOutput.isEmpty() ||
            interleavedSamples.isEmpty() ||
            speed <= 0.0) {
            return rubberBandOutput;
        }
    }

    return timeStretchPreservePitchSola(interleavedSamples, channelCount, speed);
}

QVector<float> timeStretchPreservePitchRubberBand(const QVector<float>& interleavedSamples,
                                                  int channelCount,
                                                  int sampleRate,
                                                  double speed)
{
#if JCUT_HAVE_RUBBERBAND
    const int channels = qMax(1, channelCount);
    const int effectiveSampleRate = qMax(1, sampleRate);
    if (interleavedSamples.isEmpty() || channels <= 0 || speed <= 0.0) {
        return {};
    }
    if (std::abs(speed - 1.0) < 0.0001) {
        return interleavedSamples;
    }

    const int inputFrames = static_cast<int>(interleavedSamples.size() / channels);
    if (inputFrames <= 0) {
        return {};
    }

    QVector<QVector<float>> planarInput(channels);
    for (int ch = 0; ch < channels; ++ch) {
        planarInput[ch].resize(inputFrames);
    }
    for (int frame = 0; frame < inputFrames; ++frame) {
        for (int ch = 0; ch < channels; ++ch) {
            planarInput[ch][frame] = interleavedSamples[static_cast<qsizetype>(frame) * channels + ch];
        }
    }

    QVector<const float*> inputPointers(channels);
    for (int ch = 0; ch < channels; ++ch) {
        inputPointers[ch] = planarInput[ch].constData();
    }

    const double timeRatio = 1.0 / speed;
    RubberBand::RubberBandStretcher::Options options =
        RubberBand::RubberBandStretcher::OptionProcessOffline |
        RubberBand::RubberBandStretcher::OptionEngineFiner |
        RubberBand::RubberBandStretcher::OptionChannelsTogether;
    RubberBand::RubberBandStretcher stretcher(
        static_cast<size_t>(effectiveSampleRate),
        static_cast<size_t>(channels),
        options,
        timeRatio,
        1.0);

    stretcher.study(inputPointers.constData(), static_cast<size_t>(inputFrames), true);
    stretcher.process(inputPointers.constData(), static_cast<size_t>(inputFrames), true);

    const int expectedFrames = qMax(1, static_cast<int>(std::llround(inputFrames / speed)));
    QVector<QVector<float>> planarOutput(channels);
    for (int ch = 0; ch < channels; ++ch) {
        planarOutput[ch].resize(expectedFrames);
    }
    QVector<float*> outputPointers(channels);
    for (int ch = 0; ch < channels; ++ch) {
        outputPointers[ch] = planarOutput[ch].data();
    }

    int retrievedFrames = 0;
    while (retrievedFrames < expectedFrames) {
        const int available = stretcher.available();
        if (available <= 0) {
            break;
        }
        const int framesToRead = qMin(available, expectedFrames - retrievedFrames);
        QVector<float*> blockPointers(channels);
        for (int ch = 0; ch < channels; ++ch) {
            blockPointers[ch] = outputPointers[ch] + retrievedFrames;
        }
        const int readFrames = static_cast<int>(
            stretcher.retrieve(blockPointers.constData(), static_cast<size_t>(framesToRead)));
        if (readFrames <= 0) {
            break;
        }
        retrievedFrames += readFrames;
    }

    if (retrievedFrames <= 0) {
        return {};
    }

    QVector<float> output(static_cast<qsizetype>(retrievedFrames) * channels);
    for (int frame = 0; frame < retrievedFrames; ++frame) {
        for (int ch = 0; ch < channels; ++ch) {
            output[static_cast<qsizetype>(frame) * channels + ch] =
                std::clamp(planarOutput[ch][frame], -1.0f, 1.0f);
        }
    }
    return output;
#else
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
    const int channels = qMax(1, channelCount);
    if (interleavedSamples.isEmpty() || channels <= 0 || speed <= 0.0) {
        return {};
    }
    if (std::abs(speed - 1.0) < 0.0001) {
        return interleavedSamples;
    }

    const int inputFrames = static_cast<int>(interleavedSamples.size() / channels);
    if (inputFrames <= 0) {
        return {};
    }
    const int outputFrames = qMax(1, static_cast<int>(std::llround(inputFrames / speed)));
    QVector<float> output(static_cast<qsizetype>(outputFrames) * channels, 0.0f);
    QVector<float> weights(static_cast<qsizetype>(outputFrames) * channels, 0.0f);

    const int windowFrames = qBound(256, inputFrames / 8, 2048);
    const int overlapFrames = qMax(64, windowFrames / 2);
    const int hopOut = qMax(64, windowFrames - overlapFrames);
    const double hopIn = static_cast<double>(hopOut) * speed;
    const int searchRadius = qMin(256, qMax(32, overlapFrames / 2));

    auto addWindow = [&](int outputStartFrame, int inputStartFrame) {
        const int frames = qMin(windowFrames, qMin(outputFrames - outputStartFrame, inputFrames - inputStartFrame));
        if (frames <= 0) {
            return;
        }
        for (int i = 0; i < frames; ++i) {
            float w = 1.0f;
            if (i < overlapFrames) {
                w = hann(i, overlapFrames * 2);
            } else if (i >= frames - overlapFrames) {
                w = hann((frames - 1 - i), overlapFrames * 2);
            }
            for (int ch = 0; ch < channels; ++ch) {
                const qsizetype outIndex = static_cast<qsizetype>(outputStartFrame + i) * channels + ch;
                const qsizetype inIndex = static_cast<qsizetype>(inputStartFrame + i) * channels + ch;
                output[outIndex] += interleavedSamples[inIndex] * w;
                weights[outIndex] += w;
            }
        }
    };

    addWindow(0, 0);
    for (int outputStart = hopOut; outputStart < outputFrames; outputStart += hopOut) {
        const int predictedInput = qBound(0,
                                          static_cast<int>(std::llround(
                                              (static_cast<double>(outputStart) / hopOut) * hopIn)),
                                          qMax(0, inputFrames - 1));
        int bestInput = predictedInput;
        double bestScore = -2.0;
        const int compareOutput = qMax(0, outputStart - overlapFrames);
        const int minInput = qMax(0, predictedInput - searchRadius);
        const int maxInput = qMin(qMax(0, inputFrames - overlapFrames), predictedInput + searchRadius);
        for (int candidate = minInput; candidate <= maxInput; ++candidate) {
            const double score = overlapCorrelation(output,
                                                    compareOutput,
                                                    interleavedSamples,
                                                    candidate,
                                                    qMin(overlapFrames, outputFrames - compareOutput),
                                                    channels);
            if (score > bestScore) {
                bestScore = score;
                bestInput = candidate;
            }
        }
        addWindow(outputStart, bestInput);
    }

    for (qsizetype i = 0; i < output.size(); ++i) {
        if (weights.at(i) > 0.000001f) {
            output[i] = std::clamp(output.at(i) / weights.at(i), -1.0f, 1.0f);
        }
    }
    return output;
}
