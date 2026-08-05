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
std::atomic<std::int64_t> g_lastObservedFrames{0};
std::atomic<std::int64_t> g_lastObservedRenderStageMs{0};
std::atomic<std::int64_t> g_lastObservedDecodeWaitMs{0};
std::atomic<std::int64_t> g_lastObservedMaxDecodeWaitMs{0};
std::atomic<int> g_lastWaitSharePermille{0};
std::atomic<std::int64_t> g_lastSpikeMs{0};
std::atomic<int> g_lastAutotuneReason{0};
std::atomic<int> g_pressureWindowCount{0};
std::atomic<int> g_cooldownWindowsRemaining{0};

constexpr int kAutotuneCooldownWindowsAfterAdjustment = 2;

enum RenderDecodeLookaheadAutotuneReason {
    kAutotuneReasonIdle = 0,
    kAutotuneReasonBoundaryDecodeWait = 1,
    kAutotuneReasonCleanBoundaryDecrease = 2,
    kAutotuneReasonDecodeWaitShareHigh = 3,
    kAutotuneReasonDecodeSpikeHigh = 4,
    kAutotuneReasonAggregateCleanDecrease = 5,
    kAutotuneReasonDisabled = 6,
};

void resetAggregateTelemetry()
{
    g_lastObservedFrames.store(0, std::memory_order_relaxed);
    g_lastObservedRenderStageMs.store(0, std::memory_order_relaxed);
    g_lastObservedDecodeWaitMs.store(0, std::memory_order_relaxed);
    g_lastObservedMaxDecodeWaitMs.store(0, std::memory_order_relaxed);
    g_lastWaitSharePermille.store(0, std::memory_order_relaxed);
    g_lastSpikeMs.store(0, std::memory_order_relaxed);
}

void resetDecisionState()
{
    g_cleanBoundaryCount.store(0, std::memory_order_relaxed);
    g_pressureWindowCount.store(0, std::memory_order_relaxed);
    g_cooldownWindowsRemaining.store(0, std::memory_order_relaxed);
}

void storeReason(RenderDecodeLookaheadAutotuneReason reason)
{
    g_lastAutotuneReason.store(static_cast<int>(reason),
                               std::memory_order_relaxed);
}

bool consumeAdjustmentCooldown()
{
    const int remaining =
        g_cooldownWindowsRemaining.load(std::memory_order_relaxed);
    if (remaining <= 0) {
        return false;
    }
    g_cooldownWindowsRemaining.store(remaining - 1,
                                     std::memory_order_relaxed);
    return true;
}

void recordAutotuneAdjustment(RenderDecodeLookaheadAutotuneReason reason)
{
    g_lookaheadAdjustmentCount.fetch_add(1, std::memory_order_relaxed);
    g_cooldownWindowsRemaining.store(kAutotuneCooldownWindowsAfterAdjustment,
                                     std::memory_order_relaxed);
    g_pressureWindowCount.store(0, std::memory_order_relaxed);
    g_cleanBoundaryCount.store(0, std::memory_order_relaxed);
    storeReason(reason);
}

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
    resetDecisionState();
    resetAggregateTelemetry();
    storeReason(kAutotuneReasonIdle);
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
    resetDecisionState();
    resetAggregateTelemetry();
    storeReason(enabled ? kAutotuneReasonIdle : kAutotuneReasonDisabled);
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
        g_pressureWindowCount.store(0, std::memory_order_relaxed);
        if (consumeAdjustmentCooldown()) {
            return;
        }
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
            recordAutotuneAdjustment(kAutotuneReasonBoundaryDecodeWait);
        }
        return;
    }

    if (boundedWaitMs > boundedBudgetMs / 4) {
        g_cleanBoundaryCount.store(0, std::memory_order_relaxed);
        g_pressureWindowCount.store(0, std::memory_order_relaxed);
        return;
    }
    g_pressureWindowCount.store(0, std::memory_order_relaxed);
    const int cleanCount =
        g_cleanBoundaryCount.fetch_add(1, std::memory_order_relaxed) + 1;
    if (cleanCount < kCleanBoundariesBeforeDecrease) {
        return;
    }
    if (consumeAdjustmentCooldown()) {
        return;
    }
    const int current = effectiveRenderSegmentDecodeLookaheadFrames();
    const int decreased = std::max(renderSegmentDecodeLookaheadFrames(),
                                   current - kDecreaseFrames);
    if (decreased != current) {
        g_effectiveSegmentDecodeLookaheadFrames.store(
            decreased, std::memory_order_relaxed);
        recordAutotuneAdjustment(kAutotuneReasonCleanBoundaryDecrease);
    }
}

void observeRenderDecodeWaitTelemetry(std::int64_t completedFrames,
                                      std::int64_t totalRenderStageMs,
                                      std::int64_t totalDecodeWaitMs,
                                      std::int64_t maxFrameDecodeWaitMs,
                                      std::int64_t frameBudgetMs)
{
    if (!renderSegmentDecodeLookaheadAutotuneEnabled()) {
        return;
    }

    constexpr std::int64_t kMinimumSampleFrames = 120;
    constexpr int kHighWaitSharePermille = 150;
    constexpr int kVeryHighWaitSharePermille = 250;
    constexpr int kCleanWaitSharePermille = 40;
    constexpr int kIncreaseFrames = 8;
    constexpr int kLargeIncreaseFrames = 16;
    constexpr int kDecreaseFrames = 4;
    constexpr int kCleanWindowsBeforeDecrease = 3;
    constexpr int kPressureWindowsBeforeIncrease = 2;

    const std::int64_t boundedFrames = std::max<std::int64_t>(0,
                                                              completedFrames);
    const std::int64_t boundedRenderMs =
        std::max<std::int64_t>(0, totalRenderStageMs);
    const std::int64_t boundedDecodeMs =
        std::max<std::int64_t>(0, totalDecodeWaitMs);
    const std::int64_t boundedMaxDecodeMs =
        std::max<std::int64_t>(0, maxFrameDecodeWaitMs);
    const std::int64_t boundedBudgetMs =
        std::max<std::int64_t>(1, frameBudgetMs);

    const std::int64_t previousFrames =
        g_lastObservedFrames.load(std::memory_order_relaxed);
    const std::int64_t previousRenderMs =
        g_lastObservedRenderStageMs.load(std::memory_order_relaxed);
    const std::int64_t previousDecodeMs =
        g_lastObservedDecodeWaitMs.load(std::memory_order_relaxed);
    const std::int64_t previousMaxDecodeMs =
        g_lastObservedMaxDecodeWaitMs.load(std::memory_order_relaxed);

    if (boundedFrames < previousFrames ||
        boundedRenderMs < previousRenderMs ||
        boundedDecodeMs < previousDecodeMs ||
        boundedMaxDecodeMs < previousMaxDecodeMs) {
        resetAggregateTelemetry();
        g_lastObservedFrames.store(boundedFrames, std::memory_order_relaxed);
        g_lastObservedRenderStageMs.store(boundedRenderMs, std::memory_order_relaxed);
        g_lastObservedDecodeWaitMs.store(boundedDecodeMs, std::memory_order_relaxed);
        g_lastObservedMaxDecodeWaitMs.store(boundedMaxDecodeMs, std::memory_order_relaxed);
        return;
    }

    const std::int64_t frameDelta = boundedFrames - previousFrames;
    if (frameDelta < kMinimumSampleFrames) {
        return;
    }

    const std::int64_t renderDelta =
        std::max<std::int64_t>(1, boundedRenderMs - previousRenderMs);
    const std::int64_t decodeDelta =
        std::max<std::int64_t>(0, boundedDecodeMs - previousDecodeMs);
    const std::int64_t spikeDelta =
        std::max<std::int64_t>(0, boundedMaxDecodeMs - previousMaxDecodeMs);
    const int waitSharePermille = static_cast<int>(
        std::min<std::int64_t>(1000, (decodeDelta * 1000) / renderDelta));
    const std::int64_t spikeMs = spikeDelta > 0 ? boundedMaxDecodeMs : 0;

    g_lastObservedFrames.store(boundedFrames, std::memory_order_relaxed);
    g_lastObservedRenderStageMs.store(boundedRenderMs, std::memory_order_relaxed);
    g_lastObservedDecodeWaitMs.store(boundedDecodeMs, std::memory_order_relaxed);
    g_lastObservedMaxDecodeWaitMs.store(boundedMaxDecodeMs, std::memory_order_relaxed);
    g_lastWaitSharePermille.store(waitSharePermille, std::memory_order_relaxed);
    g_lastSpikeMs.store(spikeMs, std::memory_order_relaxed);

    const bool shareHigh = waitSharePermille >= kHighWaitSharePermille;
    const bool shareVeryHigh = waitSharePermille >= kVeryHighWaitSharePermille;
    const bool spikeHigh = spikeMs >= boundedBudgetMs * 3;
    if (shareHigh || spikeHigh) {
        g_cleanBoundaryCount.store(0, std::memory_order_relaxed);
        if (shareHigh) {
            const int pressureCount =
                g_pressureWindowCount.fetch_add(1, std::memory_order_relaxed) + 1;
            if (pressureCount < kPressureWindowsBeforeIncrease) {
                return;
            }
        } else {
            g_pressureWindowCount.store(0, std::memory_order_relaxed);
        }
        if (consumeAdjustmentCooldown()) {
            return;
        }
        const int current = effectiveRenderSegmentDecodeLookaheadFrames();
        const int step = (shareVeryHigh || spikeMs >= boundedBudgetMs * 4)
                             ? kLargeIncreaseFrames
                             : kIncreaseFrames;
        const int increased = std::min(
            kMaximumRenderSegmentDecodeLookaheadFrames,
            current + step);
        if (increased != current) {
            g_effectiveSegmentDecodeLookaheadFrames.store(
                increased, std::memory_order_relaxed);
            recordAutotuneAdjustment(
                shareHigh ? kAutotuneReasonDecodeWaitShareHigh
                          : kAutotuneReasonDecodeSpikeHigh);
        }
        return;
    }

    if (waitSharePermille > kCleanWaitSharePermille ||
        spikeMs > boundedBudgetMs) {
        g_cleanBoundaryCount.store(0, std::memory_order_relaxed);
        g_pressureWindowCount.store(0, std::memory_order_relaxed);
        return;
    }

    g_pressureWindowCount.store(0, std::memory_order_relaxed);
    const int cleanCount =
        g_cleanBoundaryCount.fetch_add(1, std::memory_order_relaxed) + 1;
    if (cleanCount < kCleanWindowsBeforeDecrease) {
        return;
    }
    if (consumeAdjustmentCooldown()) {
        return;
    }
    const int current = effectiveRenderSegmentDecodeLookaheadFrames();
    const int decreased = std::max(renderSegmentDecodeLookaheadFrames(),
                                   current - kDecreaseFrames);
    if (decreased != current) {
        g_effectiveSegmentDecodeLookaheadFrames.store(
            decreased, std::memory_order_relaxed);
        recordAutotuneAdjustment(kAutotuneReasonAggregateCleanDecrease);
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

const char* renderSegmentDecodeLookaheadLastAutotuneReason()
{
    switch (g_lastAutotuneReason.load(std::memory_order_relaxed)) {
    case kAutotuneReasonBoundaryDecodeWait:
        return "boundary_decode_wait";
    case kAutotuneReasonCleanBoundaryDecrease:
        return "clean_boundary_decrease";
    case kAutotuneReasonDecodeWaitShareHigh:
        return "decode_wait_share_high";
    case kAutotuneReasonDecodeSpikeHigh:
        return "decode_spike_high";
    case kAutotuneReasonAggregateCleanDecrease:
        return "aggregate_clean_decrease";
    case kAutotuneReasonDisabled:
        return "disabled";
    case kAutotuneReasonIdle:
    default:
        return "idle";
    }
}

int renderSegmentDecodeLookaheadLastWaitSharePermille()
{
    return g_lastWaitSharePermille.load(std::memory_order_relaxed);
}

std::int64_t renderSegmentDecodeLookaheadLastSpikeMs()
{
    return g_lastSpikeMs.load(std::memory_order_relaxed);
}

int renderSegmentDecodeLookaheadPressureWindowCount()
{
    return g_pressureWindowCount.load(std::memory_order_relaxed);
}

int renderSegmentDecodeLookaheadCooldownWindowsRemaining()
{
    return g_cooldownWindowsRemaining.load(std::memory_order_relaxed);
}

} // namespace render_detail
