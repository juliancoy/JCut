#pragma once

#include <algorithm>
#include <cstdint>
#include <limits>
#include <vector>

namespace jcut::timeline {

struct SnapClip {
    int id = 0;
    std::int64_t startFrame = 0;
    std::int64_t durationFrames = 1;
    bool selected = false;
};

struct GroupMoveSnap {
    std::int64_t anchorStartFrame = 0;
    std::int64_t boundaryFrame = -1;
};

// Snaps every moving edge in a rigid selected group against boundaries of
// clips outside the selection. The returned anchor position also keeps the
// earliest selected clip at or after frame zero.
inline GroupMoveSnap snapSelectedGroupMove(
    const std::vector<SnapClip>& clips,
    int anchorClipId,
    std::int64_t proposedAnchorStartFrame,
    std::int64_t thresholdFrames)
{
    const auto anchor = std::find_if(
        clips.begin(), clips.end(),
        [anchorClipId](const SnapClip& clip) {
            return clip.id == anchorClipId;
        });
    if (anchor == clips.end() || !anchor->selected) {
        return {std::max<std::int64_t>(0, proposedAnchorStartFrame), -1};
    }
    const std::int64_t threshold =
        std::max<std::int64_t>(0, thresholdFrames);
    const std::int64_t proposedDelta =
        proposedAnchorStartFrame - anchor->startFrame;
    std::int64_t earliestProposed =
        std::numeric_limits<std::int64_t>::max();
    for (const SnapClip& clip : clips) {
        if (clip.selected) {
            earliestProposed = std::min(
                earliestProposed,
                clip.startFrame + proposedDelta);
        }
    }
    std::int64_t boundaryCorrection =
        earliestProposed < 0 ? -earliestProposed : 0;
    std::int64_t bestDistance = boundaryCorrection > 0
        ? boundaryCorrection
        : threshold + 1;
    std::int64_t bestBoundary =
        boundaryCorrection > 0 ? 0 : -1;

    std::vector<std::int64_t> fixedBoundaries{0};
    for (const SnapClip& clip : clips) {
        if (!clip.selected) {
            fixedBoundaries.push_back(clip.startFrame);
            fixedBoundaries.push_back(
                clip.startFrame +
                std::max<std::int64_t>(1, clip.durationFrames));
        }
    }
    for (const SnapClip& moving : clips) {
        if (!moving.selected) continue;
        const std::int64_t movingStart =
            moving.startFrame + proposedDelta;
        const std::int64_t movingEnd =
            movingStart +
            std::max<std::int64_t>(1, moving.durationFrames);
        for (const std::int64_t boundary : fixedBoundaries) {
            for (const std::int64_t movingEdge :
                 {movingStart, movingEnd}) {
                const std::int64_t correction =
                    boundary - movingEdge;
                const std::int64_t distance =
                    correction >= 0 ? correction : -correction;
                if (distance > threshold ||
                    distance >= bestDistance ||
                    earliestProposed + correction < 0) {
                    continue;
                }
                bestDistance = distance;
                boundaryCorrection = correction;
                bestBoundary = boundary;
            }
        }
    }
    return {
        std::max<std::int64_t>(
            0, proposedAnchorStartFrame + boundaryCorrection),
        bestBoundary};
}

} // namespace jcut::timeline
