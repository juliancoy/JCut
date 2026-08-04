#pragma once

#include <algorithm>

namespace jcut::subtitle {

inline constexpr int kDefaultMasterOutputOffsetMs = -150;
inline constexpr int kMinMasterOutputOffsetMs = -10000;
inline constexpr int kMaxMasterOutputOffsetMs = 10000;

inline int normalizedMasterOutputOffsetMs(int offsetMs)
{
    return std::clamp(
        offsetMs,
        kMinMasterOutputOffsetMs,
        kMaxMasterOutputOffsetMs);
}

// The transcript editor offset remains the source-level adjustment. This
// additional signed offset is applied only while resolving final-render text.
inline int finalRenderOffsetMs(int transcriptOffsetMs, int masterOutputOffsetMs)
{
    return transcriptOffsetMs +
        normalizedMasterOutputOffsetMs(masterOutputOffsetMs);
}

} // namespace jcut::subtitle
