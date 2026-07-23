#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <iterator>
#include <vector>

namespace jcut::speaker_framing {

struct Size {
    double width = 0.0;
    double height = 0.0;
};

struct Transform {
    std::int64_t frame = 0;
    double translationX = 0.0;
    double translationY = 0.0;
    double rotation = 0.0;
    double scaleX = 1.0;
    double scaleY = 1.0;
    bool linearInterpolation = true;
};

struct EnabledKeyframe {
    std::int64_t frame = 0;
    bool enabled = false;
};

struct State {
    bool enabled = false;
    double bakedTargetXNorm = 0.5;
    double bakedTargetYNorm = 0.35;
    double bakedTargetBoxNorm = -1.0;
    std::vector<EnabledKeyframe> enabledKeyframes;
    std::vector<Transform> framingKeyframes;
    std::vector<Transform> targetKeyframes;
};

inline double boundedScale(double value)
{
    if (!std::isfinite(value)) {
        return 1.0;
    }
    if (std::abs(value) >= 0.01) {
        return value;
    }
    return value < 0.0 ? -0.01 : 0.01;
}

inline bool enabledAt(const State& state, std::int64_t localFrame)
{
    if (state.enabledKeyframes.empty()) {
        return state.enabled;
    }
    const auto upper = std::upper_bound(
        state.enabledKeyframes.begin(),
        state.enabledKeyframes.end(),
        localFrame,
        [](std::int64_t frame, const EnabledKeyframe& keyframe) {
            return frame < keyframe.frame;
        });
    return upper == state.enabledKeyframes.begin()
        ? state.enabledKeyframes.front().enabled
        : std::prev(upper)->enabled;
}

inline Transform interpolate(
    const Transform& previous,
    const Transform& current,
    double localFrame,
    bool target)
{
    if (!current.linearInterpolation ||
        current.frame <= previous.frame) {
        return previous;
    }
    const double amount = std::clamp(
        (localFrame - static_cast<double>(previous.frame)) /
            static_cast<double>(current.frame - previous.frame),
        0.0,
        1.0);
    Transform result;
    result.frame = static_cast<std::int64_t>(
        std::llround(localFrame));
    result.translationX = previous.translationX +
        (current.translationX - previous.translationX) * amount;
    result.translationY = previous.translationY +
        (current.translationY - previous.translationY) * amount;
    result.rotation = target
        ? 0.0
        : previous.rotation +
            (current.rotation - previous.rotation) * amount;
    result.scaleX = previous.scaleX +
        (current.scaleX - previous.scaleX) * amount;
    result.scaleY = target
        ? result.scaleX
        : previous.scaleY +
            (current.scaleY - previous.scaleY) * amount;
    if (target) {
        result.scaleX = std::clamp(result.scaleX, -1.0, 1.0);
        result.scaleY = result.scaleX;
    } else {
        result.scaleX = boundedScale(result.scaleX);
        result.scaleY = boundedScale(result.scaleY);
    }
    result.linearInterpolation = current.linearInterpolation;
    return result;
}

inline Transform evaluate(
    const std::vector<Transform>& keyframes,
    double localFrame,
    bool target,
    Transform fallback)
{
    if (keyframes.empty()) {
        return fallback;
    }
    if (localFrame <= static_cast<double>(
            keyframes.front().frame)) {
        return keyframes.front();
    }
    const auto upper = std::upper_bound(
        keyframes.begin(),
        keyframes.end(),
        localFrame,
        [](double frame, const Transform& keyframe) {
            return frame < static_cast<double>(keyframe.frame);
        });
    if (upper == keyframes.end()) {
        return keyframes.back();
    }
    const Transform& previous = *std::prev(upper);
    return localFrame == static_cast<double>(previous.frame)
        ? previous
        : interpolate(previous, *upper, localFrame, target);
}

inline Transform retarget(
    const State& state,
    const Transform& framing,
    const Transform& target,
    Size sourceSize,
    Size outputSize)
{
    const double bakedBox = std::clamp(
        state.bakedTargetBoxNorm, -1.0, 1.0);
    const double targetBox = std::clamp(
        target.scaleX, -1.0, 1.0);
    if (targetBox <= 0.0 || bakedBox <= 0.0) {
        return framing;
    }
    outputSize.width = std::max(1.0, outputSize.width);
    outputSize.height = std::max(1.0, outputSize.height);
    sourceSize.width = std::max(1.0, sourceSize.width);
    sourceSize.height = std::max(1.0, sourceSize.height);
    const double fitScale = std::min(
        outputSize.width / sourceSize.width,
        outputSize.height / sourceSize.height);
    const double fittedWidth = sourceSize.width * fitScale;
    const double fittedHeight = sourceSize.height * fitScale;
    const double fittedCenterX = outputSize.width * 0.5;
    const double fittedCenterY = outputSize.height * 0.5;
    const double bakedX =
        std::clamp(state.bakedTargetXNorm, 0.0, 1.0) *
        outputSize.width;
    const double bakedY =
        std::clamp(state.bakedTargetYNorm, 0.0, 1.0) *
        outputSize.height;
    const double targetX =
        std::clamp(target.translationX, 0.0, 1.0) *
        outputSize.width;
    const double targetY =
        std::clamp(target.translationY, 0.0, 1.0) *
        outputSize.height;
    const double scaleFactor = std::max(0.01, targetBox / bakedBox);

    Transform result = framing;
    result.translationX = targetX - fittedCenterX -
        scaleFactor *
            (bakedX - fittedCenterX - framing.translationX);
    result.translationY = targetY - fittedCenterY -
        scaleFactor *
            (bakedY - fittedCenterY - framing.translationY);
    result.scaleX = boundedScale(
        framing.scaleX * scaleFactor);
    result.scaleY = boundedScale(
        framing.scaleY * scaleFactor);
    (void)fittedWidth;
    (void)fittedHeight;
    return result;
}

inline Transform evaluateBaked(
    const State& state,
    double localFrame,
    Size sourceSize,
    Size outputSize)
{
    Transform identity;
    if (!enabledAt(
            state,
            static_cast<std::int64_t>(
                std::floor(localFrame))) ||
        state.framingKeyframes.empty()) {
        return identity;
    }
    Transform target;
    target.translationX = 0.5;
    target.translationY = 0.35;
    target.scaleX = -1.0;
    target.scaleY = -1.0;
    const Transform framing = evaluate(
        state.framingKeyframes, localFrame, false, identity);
    target = evaluate(
        state.targetKeyframes, localFrame, true, target);
    return retarget(
        state, framing, target, sourceSize, outputSize);
}

inline Transform evaluateFaceBox(
    const State& state,
    double localFrame,
    double locationXNorm,
    double locationYNorm,
    double boxSizeNorm,
    double rotationDegrees,
    Size sourceSize,
    Size outputSize)
{
    Transform result;
    result.frame = static_cast<std::int64_t>(
        std::floor(std::max(0.0, localFrame)));
    result.rotation = std::clamp(
        rotationDegrees, -180.0, 180.0);
    const Transform target = evaluate(
        state.targetKeyframes,
        localFrame,
        true,
        Transform{0, 0.5, 0.35, 0.0, -1.0, -1.0, true});
    const double targetBox =
        std::clamp(target.scaleX, -1.0, 1.0);
    const double faceBox =
        std::clamp(boxSizeNorm, 0.0, 1.0);
    if (targetBox <= 0.0 || faceBox <= 0.0) {
        return result;
    }

    outputSize.width = std::max(1.0, outputSize.width);
    outputSize.height = std::max(1.0, outputSize.height);
    sourceSize.width = std::max(1.0, sourceSize.width);
    sourceSize.height = std::max(1.0, sourceSize.height);
    const double fitScale = std::min(
        outputSize.width / sourceSize.width,
        outputSize.height / sourceSize.height);
    const double fittedWidth = sourceSize.width * fitScale;
    const double fittedHeight = sourceSize.height * fitScale;
    const double fittedCenterX = outputSize.width * 0.5;
    const double fittedCenterY = outputSize.height * 0.5;
    const double outputMinSide =
        std::min(outputSize.width, outputSize.height);
    const double fittedMinSide =
        std::min(fittedWidth, fittedHeight);
    const double scale = boundedScale(
        std::max(1.0, targetBox * outputMinSide) /
        std::max(1.0, faceBox * fittedMinSide));
    const double localX =
        (std::clamp(locationXNorm, 0.0, 1.0) - 0.5) *
        fittedWidth;
    const double localY =
        (std::clamp(locationYNorm, 0.0, 1.0) - 0.5) *
        fittedHeight;
    constexpr double pi =
        3.141592653589793238462643383279502884;
    const double radians = result.rotation * (pi / 180.0);
    const double rotatedLocalX =
        std::cos(radians) * scale * localX -
        std::sin(radians) * scale * localY;
    const double rotatedLocalY =
        std::sin(radians) * scale * localX +
        std::cos(radians) * scale * localY;
    result.translationX =
        std::clamp(target.translationX, 0.0, 1.0) *
            outputSize.width -
        (fittedCenterX + rotatedLocalX);
    result.translationY =
        std::clamp(target.translationY, 0.0, 1.0) *
            outputSize.height -
        (fittedCenterY + rotatedLocalY);
    result.scaleX = scale;
    result.scaleY = scale;
    return result;
}

} // namespace jcut::speaker_framing
