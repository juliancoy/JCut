#include "preview_frame_selection.h"

#include "playback_frame_pipeline.h"
#include "timeline_cache.h"

#include <cstdlib>

namespace editor {
namespace {

bool heldFrameMatches(const FrameHandle& heldFrame, int64_t frameNumber, int64_t maxDelta)
{
    return !heldFrame.isNull() &&
           maxDelta >= 0 &&
           std::llabs(heldFrame.frameNumber() - frameNumber) <= maxDelta;
}

void markCacheSelection(PreviewFrameSelectionResult* result,
                        bool playing,
                        const FrameHandle& exactFrame)
{
    if (!result || result->frame.isNull()) {
        return;
    }
    if (!exactFrame.isNull() && result->frame == exactFrame) {
        result->selectedExact = true;
        result->selection = QStringLiteral("exact");
    } else {
        result->selectedApproximate = true;
        result->selection = playing ? QStringLiteral("latest") : QStringLiteral("best");
    }
}

}  // namespace

PreviewVisibleRequestDecision evaluatePreviewVisibleRequest(
    const PreviewVisibleRequestInputs& inputs)
{
    PreviewVisibleRequestDecision decision;

    if (inputs.exactCached) {
        decision.decision = QStringLiteral("skipped");
        decision.blockReason = QStringLiteral("exact_frame_already_cached");
        return decision;
    }

    if (inputs.pending && !inputs.forceRetry) {
        decision.decision = QStringLiteral("skipped");
        decision.blockReason = QStringLiteral("visible_request_already_pending");
        return decision;
    }

    if (inputs.pendingNearby && !inputs.forceRetry) {
        decision.decision = QStringLiteral("skipped");
        decision.blockReason = QStringLiteral("nearby_frame_already_pending");
        return decision;
    }

    decision.dispatch = true;
    decision.decision =
        inputs.pendingBacklog >= qMax(1, inputs.backlogLimit)
            ? QStringLiteral("dispatch_current_over_backlog")
            : QStringLiteral("dispatch");
    return decision;
}

PreviewFrameSelectionResult selectPreviewFrame(
    const PreviewFrameSelectionRequest& request,
    TimelineCache* cache,
    PlaybackFramePipeline* playbackPipeline,
    const FrameHandle& heldFrame,
    const std::function<bool(const FrameHandle&)>& stalePredicate)
{
    PreviewFrameSelectionResult result;
    result.usedPlaybackPipeline = request.usePlaybackPipeline;

    if (request.usePlaybackPipeline && playbackPipeline) {
        result.exactFrame = playbackPipeline->getFrame(request.clipId, request.frameNumber);
    } else if (request.usePlaybackBuffer && cache) {
        result.exactFrame = cache->getPlaybackFrame(request.clipId, request.frameNumber);
    } else if (cache) {
        result.exactFrame = cache->getCachedFrame(request.clipId, request.frameNumber);
    }

    if (request.usePlaybackPipeline && playbackPipeline) {
        result.frame = playbackPipeline->getPresentationFrame(request.clipId, request.frameNumber);
        if (!result.frame.isNull()) {
            result.selectedPresentation = true;
            result.selection = QStringLiteral("presentation");
        }
    } else {
        result.frame = result.exactFrame;
        if (result.frame.isNull() && cache && request.allowApproximateFrame) {
            if (request.usePlaybackBuffer) {
                result.frame = cache->getLatestPlaybackFrame(request.clipId, request.frameNumber);
                if (result.frame.isNull() && request.allowDebugCacheFallback) {
                    const FrameHandle cacheExact = cache->getCachedFrame(request.clipId, request.frameNumber);
                    result.frame = !cacheExact.isNull()
                                       ? cacheExact
                                       : cache->getLatestCachedFrame(request.clipId, request.frameNumber);
                }
            } else if (request.playing) {
                result.frame = cache->getLatestCachedFrame(request.clipId, request.frameNumber);
                if (result.frame.isNull() && request.allowDebugCacheFallback) {
                    result.frame = cache->getBestCachedFrame(request.clipId, request.frameNumber);
                }
            } else if (request.allowStoppedBestFrame) {
                result.frame = cache->getBestCachedFrame(request.clipId, request.frameNumber);
            }
        }
        markCacheSelection(&result, request.playing, result.exactFrame);
    }

    if (request.usePlaybackPipeline && playbackPipeline && result.frame.isNull()) {
        result.frame = !result.exactFrame.isNull()
                           ? result.exactFrame
                           : (request.playing
                                  ? FrameHandle()
                                  : playbackPipeline->getBestFrame(request.clipId, request.frameNumber));
        markCacheSelection(&result, false, result.exactFrame);
    }

    if (request.usePlaybackPipeline && result.frame.isNull() && cache) {
        const FrameHandle cacheExact = cache->getCachedFrame(request.clipId, request.frameNumber);
        result.frame = !cacheExact.isNull()
                           ? cacheExact
                           : (request.playing
                                  ? cache->getLatestCachedFrame(request.clipId, request.frameNumber)
                                  : cache->getBestCachedFrame(request.clipId, request.frameNumber));
        markCacheSelection(&result, request.playing, cacheExact);
    }

    if (request.usePlaybackPipeline && result.frame.isNull() &&
        heldFrameMatches(heldFrame, request.frameNumber, request.maxHeldFrameDelta)) {
        result.frame = heldFrame;
        result.selectedHeld = true;
        result.selection = QStringLiteral("held");
    }

    if (stalePredicate && stalePredicate(result.frame)) {
        result.frame = FrameHandle();
        result.selection = QStringLiteral("stale");
        result.rejectedStale = true;
        result.selectedPresentation = false;
        result.selectedExact = false;
        result.selectedApproximate = false;
        result.selectedHeld = false;
        if (heldFrameMatches(heldFrame, request.frameNumber, request.maxHeldFrameDelta) &&
            !stalePredicate(heldFrame)) {
            result.frame = heldFrame;
            result.selectedHeld = true;
            result.selection = QStringLiteral("held");
        }
    }

    return result;
}

}  // namespace editor
