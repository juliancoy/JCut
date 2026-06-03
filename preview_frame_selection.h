#pragma once

#include "frame_handle.h"

#include <QString>
#include <QtGlobal>

#include <cstdint>
#include <cstdlib>
#include <cmath>
#include <functional>

namespace editor {

class PlaybackFramePipeline;
class TimelineCache;

inline constexpr int64_t kPreviewMaxHeldPresentationFrameDelta = 8;
inline constexpr qreal kPreviewMaxPlaybackStaleSeconds = 0.20;

inline int64_t previewMaxPlaybackStaleFrameDelta(qreal sourceFps)
{
    const qreal fps = std::isfinite(sourceFps) && sourceFps > 0.001 ? sourceFps : 30.0;
    return qBound<int64_t>(
        static_cast<int64_t>(4),
        static_cast<int64_t>(std::ceil(fps * kPreviewMaxPlaybackStaleSeconds)),
        static_cast<int64_t>(12));
}

inline bool previewFrameIsTooStaleForPlayback(const FrameHandle& frame,
                                              int64_t targetFrame,
                                              int64_t maxFrameDelta)
{
    if (frame.isNull()) {
        return false;
    }
    const int64_t frameNumber = frame.frameNumber();
    if (frameNumber < 0) {
        return false;
    }
    return frameNumber + qMax<int64_t>(0, maxFrameDelta) < targetFrame;
}

struct PreviewFrameSelectionRequest {
    QString clipId;
    int64_t frameNumber = -1;
    bool playing = false;
    bool usePlaybackPipeline = false;
    bool usePlaybackBuffer = false;
    bool allowApproximateFrame = false;
    bool allowStoppedBestFrame = false;
    bool allowDebugCacheFallback = false;
    int64_t maxHeldFrameDelta = -1;
};

struct PreviewFrameSelectionResult {
    FrameHandle exactFrame;
    FrameHandle frame;
    QString selection = QStringLiteral("none");
    bool usedPlaybackPipeline = false;
    bool selectedPresentation = false;
    bool selectedExact = false;
    bool selectedApproximate = false;
    bool selectedHeld = false;
    bool rejectedStale = false;
};

struct PreviewVisibleRequestInputs {
    bool exactCached = false;
    bool displayableCached = false;
    bool pending = false;
    bool forceRetry = false;
    int pendingBacklog = 0;
    int backlogLimit = 1;
};

struct PreviewVisibleRequestDecision {
    bool dispatch = false;
    QString decision;
    QString blockReason;
};

PreviewVisibleRequestDecision evaluatePreviewVisibleRequest(
    const PreviewVisibleRequestInputs& inputs);

PreviewFrameSelectionResult selectPreviewFrame(
    const PreviewFrameSelectionRequest& request,
    TimelineCache* cache,
    PlaybackFramePipeline* playbackPipeline,
    const FrameHandle& heldFrame = FrameHandle(),
    const std::function<bool(const FrameHandle&)>& stalePredicate = {});

}  // namespace editor
