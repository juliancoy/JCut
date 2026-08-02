#include "render_runtime_controls.h"

#include <algorithm>
#include <atomic>

namespace render_detail {
namespace {

std::atomic<int> g_segmentDecodeLookaheadFrames{
    kDefaultRenderSegmentDecodeLookaheadFrames};
std::atomic<int> g_effectiveSegmentDecodeLookaheadFrames{
    kDefaultRenderSegmentDecodeLookaheadFrames};
std::atomic<bool> g_segmentDecodeLookaheadAutotune{false};
std::atomic<std::int64_t> g_lastBoundaryDecodeWaitMs{-1};
std::atomic<std::int64_t> g_lookaheadAdjustmentCount{0};
std::atomic<int> g_cleanBoundaryCount{0};

} // namespace

int renderSegmentDecodeLookaheadFrames()
{
    return g_segmentDecodeLookaheadFrames.load(std::memory_order_relaxed);
}

int effectiveRenderSegmentDecodeLookaheadFrames()
{
    return g_effectiveSegmentDecodeLookaheadFrames.load(
        std::memory_order_relaxed);
}

bool setRenderSegmentDecodeLookaheadFrames(int frames)
{
    if (frames < 0 || frames > kMaximumRenderSegmentDecodeLookaheadFrames) {
        return false;
    }
    g_segmentDecodeLookaheadFrames.store(frames, std::memory_order_relaxed);
    g_effectiveSegmentDecodeLookaheadFrames.store(frames,
                                                  std::memory_order_relaxed);
    g_cleanBoundaryCount.store(0, std::memory_order_relaxed);
    return true;
}

bool renderSegmentDecodeLookaheadAutotuneEnabled()
{
    return g_segmentDecodeLookaheadAutotune.load(std::memory_order_relaxed);
}

void setRenderSegmentDecodeLookaheadAutotuneEnabled(bool enabled)
{
    g_segmentDecodeLookaheadAutotune.store(enabled, std::memory_order_relaxed);
    g_effectiveSegmentDecodeLookaheadFrames.store(
        renderSegmentDecodeLookaheadFrames(), std::memory_order_relaxed);
    g_cleanBoundaryCount.store(0, std::memory_order_relaxed);
}

void observeRenderSegmentBoundaryDecodeWait(std::int64_t decodeWaitMs,
                                            std::int64_t frameBudgetMs)
{
    const std::int64_t boundedWaitMs = decodeWaitMs < 0 ? 0 : decodeWaitMs;
    const std::int64_t boundedBudgetMs = std::max<std::int64_t>(1,
                                                                frameBudgetMs);
    g_lastBoundaryDecodeWaitMs.store(boundedWaitMs, std::memory_order_relaxed);
    if (!renderSegmentDecodeLookaheadAutotuneEnabled()) {
        return;
    }

    constexpr int kMinimumIncreaseFrames = 8;
    constexpr int kMaximumIncreaseFrames = 32;
    constexpr int kDecreaseFrames = 4;
    constexpr int kCleanBoundariesBeforeDecrease = 3;
    if (boundedWaitMs > boundedBudgetMs) {
        g_cleanBoundaryCount.store(0, std::memory_order_relaxed);
        const int current = effectiveRenderSegmentDecodeLookaheadFrames();
        const std::int64_t missedBudgets =
            (boundedWaitMs + boundedBudgetMs - 1) / boundedBudgetMs;
        const int increase = std::clamp(
            static_cast<int>(std::min<std::int64_t>(missedBudgets, 8) * 4),
            kMinimumIncreaseFrames,
            kMaximumIncreaseFrames);
        const int increased = std::min(
            kMaximumRenderSegmentDecodeLookaheadFrames,
            current + increase);
        if (increased != current) {
            g_effectiveSegmentDecodeLookaheadFrames.store(
                increased, std::memory_order_relaxed);
            g_lookaheadAdjustmentCount.fetch_add(1, std::memory_order_relaxed);
        }
        return;
    }

    if (boundedWaitMs > boundedBudgetMs / 4) {
        g_cleanBoundaryCount.store(0, std::memory_order_relaxed);
        return;
    }
    const int cleanCount =
        g_cleanBoundaryCount.fetch_add(1, std::memory_order_relaxed) + 1;
    if (cleanCount < kCleanBoundariesBeforeDecrease) {
        return;
    }
    g_cleanBoundaryCount.store(0, std::memory_order_relaxed);
    const int current = effectiveRenderSegmentDecodeLookaheadFrames();
    const int decreased = std::max(renderSegmentDecodeLookaheadFrames(),
                                   current - kDecreaseFrames);
    if (decreased != current) {
        g_effectiveSegmentDecodeLookaheadFrames.store(
            decreased, std::memory_order_relaxed);
        g_lookaheadAdjustmentCount.fetch_add(1, std::memory_order_relaxed);
    }
}

std::int64_t renderSegmentDecodeLookaheadLastBoundaryWaitMs()
{
    return g_lastBoundaryDecodeWaitMs.load(std::memory_order_relaxed);
}

std::int64_t renderSegmentDecodeLookaheadAdjustmentCount()
{
    return g_lookaheadAdjustmentCount.load(std::memory_order_relaxed);
}

int renderSegmentDecodeLookaheadCleanBoundaryCount()
{
    return g_cleanBoundaryCount.load(std::memory_order_relaxed);
}

} // namespace render_detail
