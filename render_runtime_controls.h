#pragma once

#include <cstdint>

namespace render_detail {

inline constexpr int kDefaultRenderSegmentDecodeLookaheadFrames = 32;
inline constexpr int kMaximumRenderSegmentDecodeLookaheadFrames = 128;

int renderSegmentDecodeLookaheadFrames();
int effectiveRenderSegmentDecodeLookaheadFrames();
bool setRenderSegmentDecodeLookaheadFrames(int frames);
bool renderSegmentDecodeLookaheadAutotuneEnabled();
void setRenderSegmentDecodeLookaheadAutotuneEnabled(bool enabled);
void observeRenderSegmentBoundaryDecodeWait(std::int64_t decodeWaitMs,
                                            std::int64_t frameBudgetMs);
std::int64_t renderSegmentDecodeLookaheadLastBoundaryWaitMs();
std::int64_t renderSegmentDecodeLookaheadAdjustmentCount();
int renderSegmentDecodeLookaheadCleanBoundaryCount();

} // namespace render_detail
