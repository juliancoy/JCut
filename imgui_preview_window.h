#pragma once

#include "core/geometry.h"

#include <memory>
#include <span>
#include <string>

#include <cstdint>

namespace render_detail {
struct OffscreenVulkanFrame;
}

namespace jcut::imgui_preview {

enum class OverlayTrackState {
    Tentative,
    Confirmed,
    Lost,
    Removed
};

struct DetectionOverlay {
    jcut::core::RectF box;
};

struct TrackOverlay {
    int id = -1;
    jcut::core::RectF box;
    int firstFrame = -1;
    int lastFrame = -1;
    int hits = 0;
    int misses = 0;
    OverlayTrackState state = OverlayTrackState::Tentative;
};

} // namespace jcut::imgui_preview

class ImGuiPreviewWindow final {
public:
    struct Impl;
    struct RenderMonitorStatus {
        int64_t framesCompleted = 0;
        int64_t totalFrames = 0;
        int segmentIndex = 0;
        int segmentCount = 0;
        int64_t timelineFrame = 0;
        int64_t segmentStartFrame = 0;
        int64_t segmentEndFrame = 0;
        int incrementalChunksCompleted = 0;
        int incrementalChunksTotal = 0;
        int64_t incrementalFramesReused = 0;
        int64_t elapsedMs = 0;
        int64_t estimatedRemainingMs = -1;
        int64_t renderStageMs = 0;
        int64_t decodeStageMs = 0;
        int64_t textureStageMs = 0;
        int64_t compositeStageMs = 0;
        int64_t readbackStageMs = 0;
        int64_t encodeStageMs = 0;
        bool usingGpu = false;
        bool usingHardwareEncode = false;
        bool createVideoFromImageSequence = false;
        std::string activity;
        std::string encoderLabel;
        std::string exportPipeline;
        std::string gpuTransferLabel;
        std::string cachePath;
    };

    ImGuiPreviewWindow();
    ~ImGuiPreviewWindow();

    ImGuiPreviewWindow(const ImGuiPreviewWindow&) = delete;
    ImGuiPreviewWindow& operator=(const ImGuiPreviewWindow&) = delete;

    bool initialize(const std::string& title, jcut::core::SizeI initialSize);
    bool isActive() const;
    bool hasFailed() const;
    bool updatePending() const;
    bool isVisible() const;
    int64_t lastPresentedSourceFrame() const;
    std::string failureReason() const;

    void setStatusText(const std::string& text);
    void setWindowTitle(const std::string& title);
    void setTimelineRange(int minFrame, int maxFrame, int latestProcessedFrame);
    void setProcessingPaused(bool paused);
    void setFollowLatest(bool followLatest);
    void setRequestedPreviewFrame(int frameNumber);
    void setPreviewPlaybackActive(bool active);
    void setPreviewPlaybackSpeed(float speed);
    void setShowDetections(bool show);
    void setShowTracks(bool show);
    void setShowLabels(bool show);
    void setShowConfirmedTracks(bool show);
    void setShowTentativeTracks(bool show);
    void setShowLostTracks(bool show);
    void setDetectionLineThickness(float value);
    void setTrackLineThickness(float value);
    void setOverlayOpacity(float value);
    bool processingPausedRequested() const;
    bool followLatest() const;
    bool previewPlaybackActive() const;
    float previewPlaybackSpeed() const;
    bool showDetections() const;
    bool showTracks() const;
    bool showLabels() const;
    bool showConfirmedTracks() const;
    bool showTentativeTracks() const;
    bool showLostTracks() const;
    float detectionLineThickness() const;
    float trackLineThickness() const;
    float overlayOpacity() const;
    int requestedPreviewFrame() const;
    bool previewRefreshRequested() const;
    void pumpEvents();
    bool presentFrame(const render_detail::OffscreenVulkanFrame& frame,
                      int64_t frameNumber,
                      std::span<const jcut::imgui_preview::TrackOverlay> tracks,
                      std::span<const jcut::imgui_preview::DetectionOverlay> detections,
                      const jcut::core::RectF& roiRect,
                      int detectionCount);
    bool presentRenderMonitorFrame(const render_detail::OffscreenVulkanFrame& frame,
                                   const RenderMonitorStatus& status);
    bool discardRenderMonitorFrame(
        const render_detail::OffscreenVulkanFrame& frame);
    bool renderMonitorCancelRequested() const;

private:
    void shutdown();
    void markFailure(const std::string& reason);

    std::unique_ptr<Impl> m_impl;
};
