#pragma once

#include <algorithm>
#include <cstdint>
#include <iterator>
#include <utility>

namespace jcut::keyframes {

template <typename Collection>
void sortByFrame(Collection* keyframes)
{
    std::sort(keyframes->begin(), keyframes->end(),
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

} // namespace jcut::keyframes
