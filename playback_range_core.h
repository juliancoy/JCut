#pragma once

#include <algorithm>
#include <cstdint>
#include <vector>

namespace jcut {

struct PlaybackRangeCore {
    std::int64_t startFrame = 0;
    std::int64_t endFrame = 0;
};

template <typename RangeContainer>
[[nodiscard]] inline std::vector<PlaybackRangeCore>
normalizedPlaybackRangesCore(
    const RangeContainer& source,
    std::int64_t timelineEndFrame)
{
    timelineEndFrame = std::max<std::int64_t>(0, timelineEndFrame);
    std::vector<PlaybackRangeCore> ranges;
    ranges.reserve(source.size());
    for (const auto& value : source) {
        if (value.endFrame < value.startFrame) continue;
        PlaybackRangeCore range{
            std::clamp<std::int64_t>(
                value.startFrame, 0, timelineEndFrame),
            std::clamp<std::int64_t>(
                value.endFrame, 0, timelineEndFrame),
        };
        if (range.endFrame >= range.startFrame) {
            ranges.push_back(range);
        }
    }
    std::sort(
        ranges.begin(), ranges.end(),
        [](const PlaybackRangeCore& lhs,
           const PlaybackRangeCore& rhs) {
            return lhs.startFrame == rhs.startFrame
                ? lhs.endFrame < rhs.endFrame
                : lhs.startFrame < rhs.startFrame;
        });
    std::vector<PlaybackRangeCore> merged;
    for (const PlaybackRangeCore& range : ranges) {
        if (merged.empty() ||
            range.startFrame > merged.back().endFrame + 1) {
            merged.push_back(range);
        } else {
            merged.back().endFrame =
                std::max(merged.back().endFrame, range.endFrame);
        }
    }
    return merged;
}

[[nodiscard]] inline std::int64_t stepPlaybackFrameCore(
    const std::vector<PlaybackRangeCore>& ranges,
    std::int64_t currentFrame,
    int direction,
    std::int64_t timelineEndFrame)
{
    timelineEndFrame = std::max<std::int64_t>(0, timelineEndFrame);
    currentFrame =
        std::clamp(currentFrame, std::int64_t{0}, timelineEndFrame);
    if (ranges.empty()) {
        return std::clamp(
            currentFrame + (direction < 0 ? -1 : 1),
            std::int64_t{0},
            timelineEndFrame);
    }
    if (direction < 0) {
        for (auto it = ranges.rbegin(); it != ranges.rend(); ++it) {
            if (currentFrame > it->endFrame) return it->endFrame;
            if (currentFrame > it->startFrame) return currentFrame - 1;
            if (currentFrame == it->startFrame) {
                const auto next = std::next(it);
                return next == ranges.rend() ? 0 : next->endFrame;
            }
        }
        return 0;
    }
    for (const PlaybackRangeCore& range : ranges) {
        if (currentFrame < range.startFrame) return range.startFrame;
        if (currentFrame < range.endFrame) return currentFrame + 1;
    }
    return std::min(timelineEndFrame, ranges.back().endFrame);
}

struct PlaybackAdvanceCore {
    std::int64_t frame = 0;
    bool reachedEnd = false;
};

[[nodiscard]] inline PlaybackAdvanceCore advancePlaybackFramesCore(
    const std::vector<PlaybackRangeCore>& ranges,
    std::int64_t currentFrame,
    std::int64_t deltaFrames,
    std::int64_t timelineEndFrame)
{
    timelineEndFrame = std::max<std::int64_t>(0, timelineEndFrame);
    currentFrame =
        std::clamp(currentFrame, std::int64_t{0}, timelineEndFrame);
    deltaFrames = std::max<std::int64_t>(0, deltaFrames);
    if (ranges.empty()) {
        const std::int64_t frame = std::min(
            timelineEndFrame, currentFrame + deltaFrames);
        return {frame, frame >= timelineEndFrame};
    }
    std::int64_t frame = currentFrame;
    std::int64_t remaining = deltaFrames;
    while (remaining > 0) {
        auto active = ranges.end();
        auto next = ranges.end();
        for (auto it = ranges.begin(); it != ranges.end(); ++it) {
            if (frame < it->startFrame) {
                next = it;
                break;
            }
            if (frame <= it->endFrame) {
                active = it;
                break;
            }
        }
        if (active == ranges.end()) {
            if (next == ranges.end()) {
                return {ranges.back().endFrame, true};
            }
            frame = next->startFrame;
            continue;
        }
        const std::int64_t available =
            active->endFrame - frame + 1;
        if (remaining < available) {
            return {frame + remaining, false};
        }
        remaining -= available;
        const auto following = std::next(active);
        if (following == ranges.end()) {
            return {active->endFrame, true};
        }
        frame = following->startFrame;
    }
    return {frame, false};
}

} // namespace jcut
