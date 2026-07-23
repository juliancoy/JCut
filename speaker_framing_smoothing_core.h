#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <vector>

namespace jcut::speaker_framing_smoothing {

struct Sample {
    double value = 0.0;
    std::int64_t frame = 0;
    double confidence = 1.0;
};

inline double amountForStrength(double strength)
{
    const double bounded = std::clamp(strength, 0.0, 5.0);
    return bounded <= 0.0 ? 0.0 : 1.0 - std::exp(-bounded);
}

inline double median(std::vector<double> values)
{
    if (values.empty()) return 0.0;
    std::sort(values.begin(), values.end());
    const std::size_t middle = values.size() / 2;
    return values.size() % 2 == 1
        ? values[middle]
        : (values[middle - 1] + values[middle]) * 0.5;
}

inline double robustSmoothedScalar(
    double original,
    const std::vector<Sample>& samples,
    std::int64_t lookupFrame,
    int windowFrames,
    int smoothingMode,
    double strength,
    double minimumScale)
{
    if (samples.size() <= 1 || windowFrames <= 1) {
        return original;
    }
    const double amount = amountForStrength(strength);
    if (amount <= 0.0) return original;

    std::vector<double> values;
    values.reserve(samples.size());
    for (const Sample& sample : samples) {
        values.push_back(sample.value);
    }
    const double midpoint = median(std::move(values));
    std::vector<double> deviations;
    deviations.reserve(samples.size());
    for (const Sample& sample : samples) {
        deviations.push_back(
            std::abs(sample.value - midpoint));
    }
    const double mad = median(std::move(deviations));
    const double modeScale = smoothingMode == 1
        ? 3.25
        : (smoothingMode == 2 ? 2.75 : 2.25);
    const double robustScale = std::max(
        minimumScale,
        std::max(mad * modeScale, 0.000001));
    const double sigmaFrames = std::max(
        1.0, static_cast<double>(windowFrames) / 3.0);

    double weightedSum = 0.0;
    double weightSum = 0.0;
    for (const Sample& sample : samples) {
        const double normalizedResidual =
            std::abs(sample.value - midpoint) / robustScale;
        if (normalizedResidual >= 1.0) continue;
        const double residualWeight = std::pow(
            1.0 - normalizedResidual * normalizedResidual, 2.0);
        const double frameDistance = std::abs(
            static_cast<double>(sample.frame - lookupFrame));
        const double temporalWeight = std::exp(
            -0.5 * std::pow(frameDistance / sigmaFrames, 2.0));
        const double confidenceWeight =
            0.25 +
            0.75 * std::clamp(sample.confidence, 0.0, 1.0);
        const double weight =
            residualWeight * temporalWeight * confidenceWeight;
        weightedSum += sample.value * weight;
        weightSum += weight;
    }
    const double target = weightSum > 0.0
        ? weightedSum / weightSum
        : midpoint;
    return original + (target - original) * amount;
}

} // namespace jcut::speaker_framing_smoothing
