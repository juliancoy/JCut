#pragma once

#include "frame_handle.h"

#include <QString>

#include <cstdint>
#include <functional>

namespace editor {

class PlaybackFramePipeline;
class TimelineCache;

inline constexpr int64_t kPreviewMaxHeldPresentationFrameDelta = 8;

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

PreviewFrameSelectionResult selectPreviewFrame(
    const PreviewFrameSelectionRequest& request,
    TimelineCache* cache,
    PlaybackFramePipeline* playbackPipeline,
    const FrameHandle& heldFrame = FrameHandle(),
    const std::function<bool(const FrameHandle&)>& stalePredicate = {});

}  // namespace editor
