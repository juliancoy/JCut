#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <limits>
#include <string>
#include <vector>

namespace jcut::timeline_viewport {

constexpr float kDefaultPixelsPerFrame = 0.35f;
constexpr float kMaximumPixelsPerFrame = 24.0f;
constexpr std::int64_t kMinimumTimelineFrames = 300;

struct ClipSpan {
    std::int64_t startFrame = 0;
    std::int64_t durationFrames = 0;
};

inline std::int64_t totalFrames(
    const std::vector<ClipSpan>& clips,
    std::int64_t framesPerSecond = 30)
{
    std::int64_t result = kMinimumTimelineFrames;
    const std::int64_t tailFrames = std::max<std::int64_t>(
        1, framesPerSecond);
    for (const ClipSpan& clip : clips) {
        const std::int64_t start = std::max<std::int64_t>(
            0, clip.startFrame);
        const std::int64_t duration = std::max<std::int64_t>(
            0, clip.durationFrames);
        const std::int64_t maximumDuration =
            std::numeric_limits<std::int64_t>::max() - start - tailFrames;
        result = std::max(
            result,
            start + std::min(duration, maximumDuration) + tailFrames);
    }
    return result;
}

inline float fitPixelsPerFrame(
    float contentWidth,
    std::int64_t timelineFrames)
{
    if (contentWidth <= 0.0f || timelineFrames <= 0) {
        return 0.01f;
    }
    return std::clamp(
        contentWidth / static_cast<float>(timelineFrames),
        0.0001f,
        kMaximumPixelsPerFrame);
}

inline float minimumPixelsPerFrame(
    float contentWidth,
    std::int64_t timelineFrames)
{
    return std::min(
        0.25f,
        fitPixelsPerFrame(contentWidth, timelineFrames));
}

inline std::int64_t visibleFrameCount(
    float contentWidth,
    float pixelsPerFrame)
{
    if (contentWidth <= 0.0f || pixelsPerFrame <= 0.0f) {
        return 1;
    }
    return std::max<std::int64_t>(
        1,
        static_cast<std::int64_t>(
            std::ceil(contentWidth / pixelsPerFrame)));
}

inline std::int64_t maximumFrameOffset(
    std::int64_t timelineFrames,
    float contentWidth,
    float pixelsPerFrame)
{
    return std::max<std::int64_t>(
        0,
        timelineFrames -
            visibleFrameCount(contentWidth, pixelsPerFrame));
}

inline std::int64_t clampFrameOffset(
    std::int64_t frameOffset,
    std::int64_t timelineFrames,
    float contentWidth,
    float pixelsPerFrame)
{
    return std::clamp<std::int64_t>(
        frameOffset,
        0,
        maximumFrameOffset(
            timelineFrames, contentWidth, pixelsPerFrame));
}

inline std::int64_t frameFromX(
    float contentLeft,
    float x,
    std::int64_t frameOffset,
    float pixelsPerFrame)
{
    if (pixelsPerFrame <= 0.0f) {
        return std::max<std::int64_t>(0, frameOffset);
    }
    const double localX = std::max(
        0.0,
        static_cast<double>(x - contentLeft));
    const double frame = static_cast<double>(frameOffset) +
        localX / static_cast<double>(pixelsPerFrame);
    return std::max<std::int64_t>(
        0,
        static_cast<std::int64_t>(std::llround(frame)));
}

inline float xFromFrame(
    float contentLeft,
    std::int64_t frame,
    std::int64_t frameOffset,
    float pixelsPerFrame)
{
    return contentLeft +
        static_cast<float>(frame - frameOffset) * pixelsPerFrame;
}

inline float clipPixelWidth(
    std::int64_t durationFrames,
    float pixelsPerFrame)
{
    return std::max(
        1.0f,
        static_cast<float>(
            std::max<std::int64_t>(0, durationFrames)) *
            std::max(0.0f, pixelsPerFrame));
}

inline std::int64_t offsetKeepingFrameAtX(
    std::int64_t anchorFrame,
    float anchorLocalX,
    float pixelsPerFrame,
    std::int64_t timelineFrames,
    float contentWidth)
{
    const double unbounded =
        static_cast<double>(anchorFrame) -
        std::max(0.0f, anchorLocalX) /
            std::max(0.0001, static_cast<double>(pixelsPerFrame));
    return clampFrameOffset(
        static_cast<std::int64_t>(std::llround(unbounded)),
        timelineFrames,
        contentWidth,
        pixelsPerFrame);
}

inline std::int64_t rulerStepFrames(
    float pixelsPerFrame,
    std::int64_t framesPerSecond,
    float minimumTickSpacing = 10.0f)
{
    const std::int64_t fps = std::max<std::int64_t>(
        1, framesPerSecond);
    if (pixelsPerFrame <= 0.0f) {
        return fps;
    }
    const double minimumSeconds =
        std::max(
            1.0,
            std::ceil(
                minimumTickSpacing /
                (static_cast<double>(pixelsPerFrame) * fps)));
    double magnitude = 1.0;
    while (magnitude * 10.0 < minimumSeconds) {
        magnitude *= 10.0;
    }
    double niceSeconds = magnitude;
    if (minimumSeconds > magnitude * 5.0) {
        niceSeconds = magnitude * 10.0;
    } else if (minimumSeconds > magnitude * 2.0) {
        niceSeconds = magnitude * 5.0;
    } else if (minimumSeconds > magnitude) {
        niceSeconds = magnitude * 2.0;
    }
    return std::max<std::int64_t>(
        fps,
        static_cast<std::int64_t>(
            std::llround(niceSeconds * fps)));
}

inline std::string timecode(
    std::int64_t frame,
    std::int64_t framesPerSecond)
{
    const std::int64_t fps = std::max<std::int64_t>(
        1, framesPerSecond);
    const std::int64_t safeFrame = std::max<std::int64_t>(
        0, frame);
    const std::int64_t totalSeconds = safeFrame / fps;
    const std::int64_t hours = totalSeconds / 3600;
    const std::int64_t minutes = (totalSeconds / 60) % 60;
    const std::int64_t seconds = totalSeconds % 60;
    const std::int64_t frames = safeFrame % fps;
    char buffer[48];
    std::snprintf(
        buffer,
        sizeof(buffer),
        "%02lld:%02lld:%02lld:%02lld",
        static_cast<long long>(hours),
        static_cast<long long>(minutes),
        static_cast<long long>(seconds),
        static_cast<long long>(frames));
    return buffer;
}

} // namespace jcut::timeline_viewport
