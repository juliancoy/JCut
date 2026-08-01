#pragma once

#include <QVector>
#include <QtGlobal>

#include <algorithm>
#include <cmath>
#include <cstdint>

namespace editor::speech {

enum class FadeMode {
    JumpCut = 0,
    Fade = 1,
    SmoothStep = 2,
    SmootherStep = 3,
    Crossfade = 4,
};

struct SampleRange {
    int64_t startSample = 0;
    int64_t endSampleExclusive = 0;
};

struct RangeBlend {
    float primaryGain = 1.0f;
    float secondaryGain = 0.0f;
    int64_t secondaryTimelineSample = -1;
};

inline RangeBlend rangeBlendAtSample(
    int64_t samplePos,
    const QVector<SampleRange>& ranges,
    int fadeSamples,
    FadeMode fadeMode,
    qreal curveStrength)
{
    RangeBlend blend;
    if (ranges.isEmpty()) {
        return blend;
    }
    const auto current = std::upper_bound(
        ranges.cbegin(), ranges.cend(), samplePos,
        [](int64_t sample, const SampleRange& range) {
            return sample < range.endSampleExclusive;
        });
    if (current == ranges.cend() ||
        samplePos < current->startSample) {
        blend.primaryGain = 0.0f;
        return blend;
    }
    if (fadeMode == FadeMode::JumpCut || fadeSamples <= 0) {
        return blend;
    }

    const int rangeIndex =
        static_cast<int>(std::distance(ranges.cbegin(), current));
    const int64_t rangeLength = qMax<int64_t>(
        1, current->endSampleExclusive - current->startSample);
    const qreal boundedStrength =
        qBound<qreal>(0.25, curveStrength, 4.0);
    const auto shaped = [fadeMode, boundedStrength](float t) {
        t = qBound(0.0f, t, 1.0f);
        float value = t;
        if (fadeMode == FadeMode::SmoothStep) {
            value = t * t * (3.0f - 2.0f * t);
        } else if (fadeMode == FadeMode::SmootherStep) {
            value =
                t * t * t * (t * (t * 6.0f - 15.0f) + 10.0f);
        }
        if (fadeMode == FadeMode::SmoothStep ||
            fadeMode == FadeMode::SmootherStep) {
            value = std::pow(
                qBound(0.0f, value, 1.0f),
                static_cast<float>(boundedStrength));
        }
        return qBound(0.0f, value, 1.0f);
    };

    if (fadeMode != FadeMode::Crossfade) {
        const int64_t fromStart =
            samplePos - current->startSample;
        const int64_t toEnd =
            current->endSampleExclusive - samplePos;
        float gain = 1.0f;
        if (fromStart < fadeSamples) {
            gain = qMin(
                gain,
                shaped(static_cast<float>(fromStart) /
                       static_cast<float>(fadeSamples)));
        }
        if (toEnd < fadeSamples) {
            gain = qMin(
                gain,
                shaped(static_cast<float>(toEnd) /
                       static_cast<float>(fadeSamples)));
        }
        blend.primaryGain = qBound(0.0f, gain, 1.0f);
        return blend;
    }

    static constexpr float kHalfPi =
        1.57079632679489661923f;
    if (rangeIndex > 0) {
        const SampleRange& previous = ranges.at(rangeIndex - 1);
        const int64_t previousLength = qMax<int64_t>(
            1, previous.endSampleExclusive - previous.startSample);
        const int64_t window = qMax<int64_t>(
            1, qMin<int64_t>(
                   fadeSamples,
                   qMin<int64_t>(previousLength, rangeLength)));
        const int64_t offset =
            samplePos - current->startSample;
        if (offset >= 0 && offset < window) {
            const float t =
                (static_cast<float>(offset) + 0.5f) /
                static_cast<float>(window);
            blend.primaryGain = std::sin(t * kHalfPi);
            blend.secondaryGain = std::cos(t * kHalfPi);
            blend.secondaryTimelineSample =
                previous.endSampleExclusive - window + offset;
            return blend;
        }
    }

    if (rangeIndex + 1 < ranges.size()) {
        const SampleRange& next = ranges.at(rangeIndex + 1);
        const int64_t nextLength = qMax<int64_t>(
            1, next.endSampleExclusive - next.startSample);
        const int64_t window = qMax<int64_t>(
            1, qMin<int64_t>(
                   fadeSamples,
                   qMin<int64_t>(rangeLength, nextLength)));
        const int64_t offset =
            samplePos - (current->endSampleExclusive - window);
        if (offset >= 0 && offset < window) {
            const float t =
                (static_cast<float>(offset) + 0.5f) /
                static_cast<float>(window);
            blend.primaryGain = std::cos(t * kHalfPi);
            blend.secondaryGain = std::sin(t * kHalfPi);
            blend.secondaryTimelineSample =
                next.startSample + offset;
        }
    }
    return blend;
}

} // namespace editor::speech
