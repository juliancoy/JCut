#pragma once

#include "timeline_fps.h"

#include <algorithm>
#include <cmath>
#include <cstdint>

namespace jcut::export_timing {

struct ExportFrameTiming {
    std::int64_t outputFrame = 0;
    double outputTimeSeconds = 0.0;
    double timelineFramePosition = 0.0;
    std::int64_t timelineFrame = 0;
};

inline double normalizedOutputFps(double outputFps)
{
    return std::isfinite(outputFps) && outputFps > 0.001
        ? outputFps
        : static_cast<double>(kTimelineFps);
}

inline double normalizedPlaybackSpeed(double playbackSpeed)
{
    return std::isfinite(playbackSpeed) && playbackSpeed > 0.001
        ? playbackSpeed
        : 1.0;
}

inline std::int64_t outputFrameCountForTimelineRange(std::int64_t startFrame,
                                                     std::int64_t endFrame,
                                                     double outputFps,
                                                     double playbackSpeed)
{
    const std::int64_t boundedStart = std::max<std::int64_t>(0, startFrame);
    const std::int64_t boundedEnd = std::max<std::int64_t>(boundedStart, endFrame);
    const double safeOutputFps = normalizedOutputFps(outputFps);
    const double safePlaybackSpeed = normalizedPlaybackSpeed(playbackSpeed);
    const double timelineDurationSeconds =
        static_cast<double>(boundedEnd - boundedStart + 1) / static_cast<double>(kTimelineFps);
    const double outputDurationSeconds = timelineDurationSeconds / safePlaybackSpeed;
    return std::max<std::int64_t>(
        1,
        static_cast<std::int64_t>(std::ceil(outputDurationSeconds * safeOutputFps)));
}

inline ExportFrameTiming frameTimingForOutputFrame(std::int64_t outputFrame,
                                                   std::int64_t startFrame,
                                                   std::int64_t endFrame,
                                                   double outputFps,
                                                   double playbackSpeed)
{
    const std::int64_t boundedStart = std::max<std::int64_t>(0, startFrame);
    const std::int64_t boundedEnd = std::max<std::int64_t>(boundedStart, endFrame);
    const std::int64_t boundedOutputFrame = std::max<std::int64_t>(0, outputFrame);
    const double safeOutputFps = normalizedOutputFps(outputFps);
    const double safePlaybackSpeed = normalizedPlaybackSpeed(playbackSpeed);

    ExportFrameTiming timing;
    timing.outputFrame = boundedOutputFrame;
    timing.outputTimeSeconds = static_cast<double>(boundedOutputFrame) / safeOutputFps;
    timing.timelineFramePosition = std::min(
        static_cast<double>(boundedEnd),
        static_cast<double>(boundedStart) +
            timing.outputTimeSeconds * static_cast<double>(kTimelineFps) * safePlaybackSpeed);
    timing.timelineFrame = std::clamp<std::int64_t>(
        static_cast<std::int64_t>(std::floor(timing.timelineFramePosition)),
        boundedStart,
        boundedEnd);
    return timing;
}

} // namespace jcut::export_timing
