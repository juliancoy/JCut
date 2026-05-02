#pragma once

#include "preview_surface.h"
#include "vulkan_context.h"
#include "vulkan_renderer.h"

#include <memory>

class VulkanPreviewWindow;
class PreviewWindow;
class QWidget;

class VulkanPreviewSurface final : public PreviewSurface {
public:
    explicit VulkanPreviewSurface(QWidget* parent = nullptr);
    ~VulkanPreviewSurface() override;
    bool isNativeActive() const { return m_nativeActive; }
    QString nativeFailureReason() const { return m_nativeFailureReason; }

    QWidget* asWidget() override;
    const QWidget* asWidget() const override;

    void setPlaybackState(bool playing) override;
    void setCurrentFrame(int64_t frame) override;
    void setCurrentPlaybackSample(int64_t samplePosition) override;
    void setClipCount(int count) override;
    void setSelectedClipId(const QString& clipId) override;
    void setTimelineClips(const QVector<TimelineClip>& clips) override;
    void setTimelineTracks(const QVector<TimelineTrack>& tracks) override;
    void setRenderSyncMarkers(const QVector<RenderSyncMarker>& markers) override;
    void setExportRanges(const QVector<ExportRangeSegment>& ranges) override;
    void invalidateTranscriptOverlayCache(const QString& clipFilePath = QString()) override;
    void beginBulkUpdate() override;
    void endBulkUpdate() override;
    QString backendName() const override;
    void setRenderBackendPreference(const QString& backendName) override;
    void setAudioMuted(bool muted) override;
    void setAudioVolume(qreal volume) override;
    void setOutputSize(const QSize& size) override;
    void setHideOutsideOutputWindow(bool hide) override;
    void setBypassGrading(bool bypass) override;
    void setCorrectionsEnabled(bool enabled) override;
    void setShowCorrectionOverlays(bool show) override;
    void setSelectedCorrectionPolygon(int polygonIndex) override;
    void setBackgroundColor(const QColor& color) override;
    void setPreviewZoom(qreal zoom) override;
    void setShowSpeakerTrackPoints(bool show) override;
    void setShowSpeakerTrackBoxes(bool show) override;
    void setBoxstreamOverlaySource(const QString& source) override;
    void setAudioSpeakerHoverModalEnabled(bool enabled) override;
    void setAudioWaveformVisible(bool visible) override;
    bool audioSpeakerHoverModalEnabled() const override;
    bool audioWaveformVisible() const override;
    void setViewMode(ViewMode mode) override;
    ViewMode viewMode() const override;
    void setAudioDynamicsSettings(const AudioDynamicsSettings& settings) override;
    AudioDynamicsSettings audioDynamicsSettings() const override;
    void setTranscriptOverlayInteractionEnabled(bool enabled) override;
    void setTitleOverlayInteractionOnly(bool enabled) override;
    void setCorrectionDrawMode(bool enabled) override;
    bool correctionDrawMode() const override;
    bool transcriptOverlayInteractionEnabled() const override;
    bool titleOverlayInteractionOnly() const override;
    void setCorrectionDraftPoints(const QVector<QPointF>& points) override;
    qreal previewZoom() const override;
    void resetPreviewPan() override;
    QSize outputSize() const override;
    bool bypassGrading() const override;
    bool correctionsEnabled() const override;
    bool audioMuted() const override;
    int audioVolumePercent() const override;
    QString activeAudioClipLabel() const override;
    bool preparePlaybackAdvance(int64_t targetFrame) override;
    bool preparePlaybackAdvanceSample(int64_t targetSample) override;
    bool warmPlaybackLookahead(int futureFrames, int timeoutMs) override;
    QImage latestPresentedFrameImageForClip(const QString& clipId) const override;
    QJsonObject profilingSnapshot() const override;
    void resetProfilingStats() override;
    bool selectedOverlayIsTranscript() const override;

private:
    bool initializeNativeSurface(QWidget* parent);
    PreviewWindow* activeDelegate() const;
    void requestNativeUpdate();
    bool usingParityBridge() const;

    bool m_nativeActive = false;
    bool m_nativeParityBridgeActive = false;
    QString m_nativeFailureReason;
    std::unique_ptr<PreviewWindow> m_delegate;
    QWidget* m_nativeContainer = nullptr;
    VulkanContext m_vulkanContext;
    VulkanNativeWindow* m_vulkanWindow = nullptr;
    VulkanRendererState m_nativeState;
};
