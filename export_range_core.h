#pragma once

#include <algorithm>
#include <cstdint>
#include <utility>
#include <vector>

namespace jcut::export_range {

struct Range {
    std::int64_t startFrame = 0;
    std::int64_t endFrame = 0;

    friend bool operator==(const Range&, const Range&) = default;
};

enum class Edit {
    SetStartAtPlayhead,
    SetEndAtPlayhead,
    SplitAtPlayhead,
    Reset
};

inline void normalize(std::vector<Range>* ranges, std::int64_t extent)
{
    if (!ranges) {
        return;
    }
    extent = std::max<std::int64_t>(0, extent);
    if (ranges->empty()) {
        ranges->push_back({0, extent});
    }
    for (Range& range : *ranges) {
        range.startFrame = std::clamp<std::int64_t>(
            range.startFrame, 0, extent);
        range.endFrame = std::clamp<std::int64_t>(
            range.endFrame, 0, extent);
        if (range.endFrame < range.startFrame) {
            std::swap(range.startFrame, range.endFrame);
        }
    }
    std::sort(
        ranges->begin(),
        ranges->end(),
        [](const Range& lhs, const Range& rhs) {
            if (lhs.startFrame == rhs.startFrame) {
                return lhs.endFrame < rhs.endFrame;
            }
            return lhs.startFrame < rhs.startFrame;
        });
    ranges->erase(
        std::unique(ranges->begin(), ranges->end()),
        ranges->end());
}

template <typename RangeContainer>
[[nodiscard]] inline bool canSplitAt(
    const RangeContainer& ranges,
    std::int64_t frame)
{
    return std::any_of(
        ranges.begin(),
        ranges.end(),
        [&](const auto& range) {
            return frame > range.startFrame &&
                frame <= range.endFrame;
        });
}

// Applies the Qt timeline semantics in place. False means that the requested
// split has no containing segment; every other edit is accepted after bounded
// normalization.
[[nodiscard]] inline bool apply(
    std::vector<Range>* ranges,
    std::int64_t extent,
    Edit edit,
    std::int64_t frame)
{
    if (!ranges) {
        return false;
    }
    extent = std::max<std::int64_t>(0, extent);
    normalize(ranges, extent);
    frame = std::clamp<std::int64_t>(frame, 0, extent);

    if (edit == Edit::SetStartAtPlayhead) {
        ranges->front().startFrame =
            std::min(frame, ranges->front().endFrame);
    } else if (edit == Edit::SetEndAtPlayhead) {
        ranges->back().endFrame =
            std::max(frame, ranges->back().startFrame);
    } else if (edit == Edit::SplitAtPlayhead) {
        const auto segment = std::find_if(
            ranges->begin(),
            ranges->end(),
            [&](const Range& range) {
                return frame > range.startFrame &&
                    frame <= range.endFrame;
            });
        if (segment == ranges->end()) {
            return false;
        }
        const auto index = std::distance(ranges->begin(), segment);
        const Range original = *segment;
        ranges->erase(segment);
        ranges->insert(
            ranges->begin() + index,
            {
                {original.startFrame, frame - 1},
                {frame, original.endFrame}});
    } else {
        *ranges = {{0, extent}};
    }
    normalize(ranges, extent);
    return true;
}

} // namespace jcut::export_range
