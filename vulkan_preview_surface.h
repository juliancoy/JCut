#pragma once

#include "preview_interaction_state.h"
#include "preview_surface.h"
#include "debug_controls.h"

#include <QJsonObject>
#include <QHash>
#include <QSet>

#include <memory>

class QWidget;
class QObject;
class DirectVulkanPreviewPresenter;

namespace editor {
class AsyncDecoder;
class TimelineCache;
}

class VulkanPreviewSurface final : public PreviewSurface {
public:
    explicit VulkanPreviewSurface(QWidget* parent = nullptr);
    ~VulkanPreviewSurface() override;
    bool isNativeActive() const;
    bool isNativePresentationActive() const;
    QString nativeFailureReason() const { return m_failureReason; }

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
    struct BoxstreamKeyframe {
        int64_t frame = -1;
        QRectF boxNorm;
        qreal confidence = 0.0;
        QString source;
    };
    struct BoxstreamTrack {
        QString streamId;
        QString source;
        int trackId = -1;
        QVector<BoxstreamKeyframe> keyframes;
    };
    struct BoxstreamOverlayCacheEntry {
        QString signature;
        QVector<BoxstreamTrack> tracks;
    };

    void requestNativeUpdate();
    void updateNativeTitle();
    void ensureFramePipeline();
    void registerVisibleClips();
    void requestFramesForCurrentPosition();
    void refreshVulkanFrameStatuses();
    void refreshBoxstreamOverlays();
    QVector<BoxstreamTrack> loadBoxstreamTracksForClip(const TimelineClip& clip);
    QVector<BoxstreamTrack> parseContinuityTracksForClip(const TimelineClip& clip,
                                                         const QJsonObject& artifactRoot) const;
    bool isSampleWithinClip(const TimelineClip& clip, int64_t samplePosition) const;
    int64_t sourceFrameForSample(const TimelineClip& clip, int64_t samplePosition) const;

    std::unique_ptr<DirectVulkanPreviewPresenter> m_presenter;
    std::unique_ptr<QObject> m_pipelineOwner;
    std::unique_ptr<editor::AsyncDecoder> m_decoder;
    std::unique_ptr<editor::TimelineCache> m_cache;
    QSet<QString> m_registeredClips;
    QHash<QString, BoxstreamOverlayCacheEntry> m_boxstreamOverlayCache;
    PreviewInteractionState m_interaction;
    AudioDynamicsSettings m_audioDynamics;
    QString m_failureReason;
    QString m_boxstreamOverlaySource;
    QString m_activeAudioClipLabel;
    bool m_hideOutsideOutputWindow = true;
    bool m_bypassGrading = false;
    bool m_correctionsEnabled = true;
    bool m_showCorrectionOverlays = true;
    bool m_showSpeakerTrackPoints = true;
    bool m_showSpeakerTrackBoxes = true;
    bool m_audioSpeakerHoverModalEnabled = true;
    bool m_audioWaveformVisible = true;
    bool m_forcedZeroCopyDecodePreference = false;
    editor::DecodePreference m_previousDecodePreference = editor::DecodePreference::Hardware;
    bool m_bulkUpdating = false;
    int m_bulkDepth = 0;
    int m_selectedCorrectionPolygon = -1;
    int m_frameStatusExactCount = 0;
    int m_frameStatusApproxCount = 0;
    int m_frameStatusMissingCount = 0;
    int m_frameStatusHardwareCount = 0;
    int m_frameStatusCpuCount = 0;
};
