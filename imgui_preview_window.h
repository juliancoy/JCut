#pragma once

#include "core/geometry.h"

#include <memory>
#include <span>
#include <string>

#include <cstdint>

namespace render_detail {
struct OffscreenVulkanFrame;
}

namespace jcut::facedetections {
struct ContinuityTrack;
struct Detection;
}

class ImGuiPreviewWindow final {
public:
    struct Impl;

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
                      std::span<const jcut::facedetections::ContinuityTrack> tracks,
                      std::span<const jcut::facedetections::Detection> detections,
                      const jcut::core::RectF& roiRect,
                      int detectionCount);

private:
    void shutdown();
    void markFailure(const std::string& reason);

    std::unique_ptr<Impl> m_impl;
};
