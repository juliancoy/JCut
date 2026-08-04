#pragma once

#include <algorithm>
#include <cstdint>
#include <iterator>
#include <utility>

namespace jcut::keyframes {

template <typename Value>
struct Keyframe : Value {
    Keyframe() = default;

    template <typename... Args>
    Keyframe(std::int64_t frameValue, Args&&... args)
        : Value{std::forward<Args>(args)...}, frame(frameValue)
    {
    }

    std::int64_t frame = 0;
};

template <typename Collection>
void sortByFrame(Collection* keyframes)
{
    std::stable_sort(keyframes->begin(), keyframes->end(),
                     [](const auto& left, const auto& right) {
                         return left.frame < right.frame;
                     });
}

template <typename Collection>
int findFrameIndex(const Collection& keyframes, std::int64_t frame)
{
    const auto it = std::find_if(
        keyframes.cbegin(), keyframes.cend(),
        [frame](const auto& keyframe) { return keyframe.frame == frame; });
    return it == keyframes.cend()
               ? -1
               : static_cast<int>(std::distance(keyframes.cbegin(), it));
}

template <typename Collection, typename Sanitize>
void normalizeSequence(Collection* keyframes,
                       std::int64_t maxFrame,
                       Sanitize&& sanitize)
{
    if (!keyframes) {
        return;
    }

    sortByFrame(keyframes);

    Collection normalized;
    normalized.reserve(keyframes->size());
    for (auto keyframe : *keyframes) {
        keyframe.frame = std::clamp<std::int64_t>(keyframe.frame, 0, maxFrame);
        sanitize(keyframe);
        if (!normalized.empty() && normalized.back().frame == keyframe.frame) {
            // Stable frame ordering makes this replacement explicitly
            // last-input-wins for duplicate frames.
            normalized.back() = std::move(keyframe);
        } else {
            normalized.push_back(std::move(keyframe));
        }
    }
    *keyframes = std::move(normalized);
}

template <typename Collection>
void normalizeSequence(Collection* keyframes, std::int64_t maxFrame)
{
    normalizeSequence(keyframes, maxFrame, [](auto&) {});
}

template <typename Collection, typename Keyframe>
void upsertByFrame(Collection* keyframes, Keyframe keyframe)
{
    const int index = findFrameIndex(*keyframes, keyframe.frame);
    if (index >= 0) {
        (*keyframes)[index] = std::move(keyframe);
    } else {
        keyframes->push_back(std::move(keyframe));
    }
}

template <typename Collection, typename Keyframe>
void replaceAtFrame(Collection* keyframes,
                    std::int64_t originalFrame,
                    Keyframe keyframe)
{
    const int index = findFrameIndex(*keyframes, originalFrame);
    if (index >= 0) {
        (*keyframes)[index] = std::move(keyframe);
    } else {
        keyframes->push_back(std::move(keyframe));
    }
}

template <typename Collection, typename ShouldRemove>
bool removeIf(Collection* keyframes, ShouldRemove&& shouldRemove)
{
    const auto originalSize = keyframes->size();
    keyframes->erase(
        std::remove_if(keyframes->begin(), keyframes->end(),
                       std::forward<ShouldRemove>(shouldRemove)),
        keyframes->end());
    return keyframes->size() != originalSize;
}

template <typename Collection, typename Frame, typename Interpolate>
typename Collection::value_type evaluateTrackAt(
    const Collection& keyframes,
    Frame frame,
    typename Collection::value_type fallback,
    Interpolate&& interpolate)
{
    if (keyframes.empty()) {
        return fallback;
    }
    const auto upper = std::upper_bound(
        keyframes.cbegin(), keyframes.cend(), frame,
        [](Frame needle, const auto& keyframe) {
            return needle < static_cast<Frame>(keyframe.frame);
        });
    if (upper == keyframes.cbegin()) {
        return keyframes.front();
    }
    const auto previous = std::prev(upper);
    if (upper == keyframes.cend() ||
        frame == static_cast<Frame>(previous->frame)) {
        return *previous;
    }
    return std::forward<Interpolate>(interpolate)(*previous, *upper, frame);
}

} // namespace jcut::keyframes
