#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <vector>

namespace jcut {

struct EditorGradeProbeSampleCore {
    std::int64_t localFrame = 0;
    double lumaMean = 0.0;
    double saturationMean = 0.0;
    double contrastSpread = 0.0;
};

struct EditorOpposeGradeEventCore {
    std::int64_t localFrame = 0;
    double brightnessDelta = 0.0;
    double contrastMul = 1.0;
    double saturationMul = 1.0;
};

struct EditorAutoOpposeSettingsCore {
    int sampleTarget = 140;
    int minEventGapFrames = 10;
    int maxEvents = 24;
    double jumpLumaThreshold = 0.07;
    double jumpSaturationThreshold = 0.08;
    double jumpContrastThreshold = 0.05;
    double brightnessStrength = 2.4;
};

inline bool probeEditorGradeStatsRgba(
    const std::uint8_t* bytes,
    int width,
    int height,
    int strideBytes,
    EditorGradeProbeSampleCore* sample)
{
    if (!bytes || !sample || width <= 0 || height <= 0 ||
        strideBytes < width * 4) {
        return false;
    }
    const int pixelCount = width * height;
    const int step = std::max(
        1,
        static_cast<int>(std::sqrt(
            static_cast<double>(std::max(1, pixelCount / 32000)))));
    double sumLuma = 0.0;
    double sumSaturation = 0.0;
    double sumLumaSquared = 0.0;
    int sampleCount = 0;
    for (int y = 0; y < height; y += step) {
        const std::uint8_t* row =
            bytes + static_cast<std::size_t>(y) * strideBytes;
        for (int x = 0; x < width; x += step) {
            const std::uint8_t* pixel = row + x * 4;
            const double red = static_cast<double>(pixel[0]) / 255.0;
            const double green = static_cast<double>(pixel[1]) / 255.0;
            const double blue = static_cast<double>(pixel[2]) / 255.0;
            const double luma =
                0.2126 * red + 0.7152 * green + 0.0722 * blue;
            const double maximum = std::max(red, std::max(green, blue));
            const double minimum = std::min(red, std::min(green, blue));
            sumLuma += luma;
            sumLumaSquared += luma * luma;
            sumSaturation += maximum - minimum;
            ++sampleCount;
        }
    }
    if (sampleCount <= 0) {
        return false;
    }
    const double inverseCount = 1.0 / static_cast<double>(sampleCount);
    sample->lumaMean = sumLuma * inverseCount;
    sample->saturationMean = sumSaturation * inverseCount;
    const double variance = std::max(
        0.0,
        sumLumaSquared * inverseCount -
            sample->lumaMean * sample->lumaMean);
    sample->contrastSpread = std::sqrt(variance);
    return true;
}

inline std::vector<EditorOpposeGradeEventCore>
detectEditorOpposeGradeEvents(
    const std::vector<EditorGradeProbeSampleCore>& samples,
    const EditorAutoOpposeSettingsCore& settings)
{
    std::vector<EditorOpposeGradeEventCore> events;
    if (samples.size() < 2) {
        return events;
    }
    const int gap = std::max(1, settings.minEventGapFrames);
    const int maximumEvents = std::max(1, settings.maxEvents);
    std::int64_t lastEventFrame = -1000000;
    double targetLuma = samples.front().lumaMean;
    double targetSaturation = samples.front().saturationMean;
    double targetSpread = samples.front().contrastSpread;
    for (std::size_t index = 1; index < samples.size(); ++index) {
        const EditorGradeProbeSampleCore& previous = samples[index - 1];
        const EditorGradeProbeSampleCore& current = samples[index];
        const bool majorJump =
            std::abs(current.lumaMean - previous.lumaMean) >=
                settings.jumpLumaThreshold ||
            std::abs(current.saturationMean - previous.saturationMean) >=
                settings.jumpSaturationThreshold ||
            std::abs(current.contrastSpread - previous.contrastSpread) >=
                settings.jumpContrastThreshold;
        if (!majorJump) {
            targetLuma = targetLuma * 0.96 + current.lumaMean * 0.04;
            targetSaturation =
                targetSaturation * 0.96 + current.saturationMean * 0.04;
            targetSpread =
                targetSpread * 0.96 + current.contrastSpread * 0.04;
            continue;
        }
        if (current.localFrame - lastEventFrame < gap) {
            continue;
        }
        EditorOpposeGradeEventCore event;
        event.localFrame = current.localFrame;
        event.brightnessDelta = std::clamp(
            -(current.lumaMean - targetLuma) * settings.brightnessStrength,
            -2.0,
            2.0);
        event.saturationMul = std::clamp(
            targetSaturation / std::max(0.03, current.saturationMean),
            0.45,
            2.2);
        event.contrastMul = std::clamp(
            targetSpread / std::max(0.02, current.contrastSpread),
            0.50,
            2.0);
        if (std::abs(event.brightnessDelta) < 0.04 &&
            std::abs(event.contrastMul - 1.0) < 0.06 &&
            std::abs(event.saturationMul - 1.0) < 0.06) {
            continue;
        }
        events.push_back(event);
        lastEventFrame = current.localFrame;
        if (static_cast<int>(events.size()) >= maximumEvents) {
            break;
        }
    }
    return events;
}

} // namespace jcut
