#pragma once

#include "preview_interaction_state.h"
#include "preview_surface.h"
#include "facestream_time_mapping.h"
#include "debug_controls.h"

#include <QJsonObject>
#include <QHash>
#include <QSet>
#include <QVector>

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
    void setUseProxyMedia(bool useProxyMedia) override;
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
    void setShowRawDetections(bool show) override;
    void setFacestreamOverlaySource(const QString& source) override;
    void setAudioSpeakerHoverModalEnabled(bool enabled) override;
    void setAudioWaveformVisible(bool visible) override;
    void setAudioVisualizationMode(AudioVisualizationMode mode) override;
    void setLoiaconoSpectrumSettings(const LoiaconoSpectrumSettings& settings) override;
    bool audioSpeakerHoverModalEnabled() const override;
    bool audioWaveformVisible() const override;
    void setViewMode(ViewMode mode) override;
    ViewMode viewMode() const override;
    void setAudioDynamicsSettings(const AudioDynamicsSettings& settings) override;
    AudioDynamicsSettings audioDynamicsSettings() const override;
    void setTranscriptOverlayInteractionEnabled(bool enabled) override;
    void setTitleOverlayInteractionOnly(bool enabled) override;
    void setFaceStreamAssignmentInteractionEnabled(bool enabled) override;
    void setCorrectionDrawMode(bool enabled) override;
    bool correctionDrawMode() const override;
    bool transcriptOverlayInteractionEnabled() const override;
    bool titleOverlayInteractionOnly() const override;
    bool faceStreamAssignmentInteractionEnabled() const override;
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
    void setPlaybackTuning(const PlaybackTuning& tuning) override;
    PlaybackTuning playbackTuning() const override;
    QImage latestPresentedFrameImageForClip(const QString& clipId) const override;
    QVector<PipelineStageSnapshot> livePipelineSnapshots() const override;
    QJsonObject profilingSnapshot() const override;
    void resetProfilingStats() override;
    bool selectedOverlayIsTranscript() const override;

private:
    struct FacestreamKeyframe {
        int64_t frame = -1;
        QRectF boxNorm;
        qreal xNorm = 0.5;
        qreal yNorm = 0.5;
        qreal boxSizeNorm = -1.0;
        bool hasCenterBox = false;
        qreal confidence = 0.0;
        QString source;
    };
    struct FacestreamTrack {
        QString streamId;
        QString source;
        int trackId = -1;
        FacestreamFrameDomain frameDomain = FacestreamFrameDomain::SourceRelative;
        int64_t typicalFrameStep = 1;
        QVector<FacestreamKeyframe> keyframes;
    };
    struct FacestreamOverlayCacheEntry {
        QString signature;
        QVector<FacestreamTrack> tracks;
        QVector<VulkanPreviewFacestreamOverlay> rawDetections;
        QHash<int64_t, QVector<VulkanPreviewFacestreamOverlay>> rawDetectionsBySourceFrame;
    };
    struct PlaybackSmoothnessSample {
        qint64 timestampMs = 0;
        int exactCount = 0;
        int approxCount = 0;
        int missingCount = 0;
        int64_t maxFrameLag = 0;
        double lastUploadMs = 0.0;
        qint64 visibleRequestAttempts = 0;
        qint64 visibleRequestDispatched = 0;
        qint64 visibleRequestBlocked = 0;
        qint64 handoffAttempts = 0;
        qint64 handoffSuccesses = 0;
        qint64 handoffFailures = 0;
        qint64 presentedFrames = 0;
        bool playing = false;
    };

    void requestNativeUpdate();
    void updateNativeTitle();
    void ensureFramePipeline();
    bool hasPlaybackLookaheadBuffered(int futureFrames) const;
    int effectivePlaybackLookaheadFrames() const;
    void registerVisibleClips();
    void requestFramesForCurrentPosition();
    void refreshVulkanFrameStatuses();
    bool loadedFrameAffectsCurrentView(const QString& clipId, int64_t frame) const;
    void queueFrameStatusRefresh(bool requestVisibleFrames);
    void applyAdaptivePlaybackTuning();
    void updateAdaptivePlaybackTuning(qint64 nowMs);
    void refreshFacestreamOverlays();
    QVector<FacestreamTrack> loadFacestreamTracksForClip(const TimelineClip& clip);
    QVector<VulkanPreviewFacestreamOverlay> rawDetectionsForClipFrame(const TimelineClip& clip,
                                                                      int64_t sourceFrame);
    QVector<FacestreamTrack> parseContinuityTracksForClip(const TimelineClip& clip,
                                                         const QJsonObject& artifactRoot) const;
    QVector<VulkanPreviewFacestreamOverlay> parseRawDetectionsForClip(const TimelineClip& clip,
                                                                      const QJsonObject& artifactRoot) const;
    bool isSampleWithinClip(const TimelineClip& clip, int64_t samplePosition) const;
    int64_t sourceFrameForSample(const TimelineClip& clip, int64_t samplePosition) const;
    void recordPlaybackSmoothnessSample(int exactCount,
                                        int approxCount,
                                        int missingCount,
                                        int64_t maxFrameLag);
    QJsonObject playbackSmoothnessSnapshot(const QJsonObject& presenterSnapshot) const;

    std::unique_ptr<DirectVulkanPreviewPresenter> m_presenter;
    std::unique_ptr<QObject> m_pipelineOwner;
    std::unique_ptr<editor::AsyncDecoder> m_decoder;
    std::unique_ptr<editor::TimelineCache> m_cache;
    QSet<QString> m_registeredClips;
    QHash<QString, QString> m_registeredClipRegistrationKeys;
    QHash<QString, FacestreamOverlayCacheEntry> m_facestreamOverlayCache;
    PreviewInteractionState m_interaction;
    AudioDynamicsSettings m_audioDynamics;
    LoiaconoSpectrumSettings m_loiaconoSpectrumSettings;
    AudioVisualizationMode m_audioVisualizationMode = AudioVisualizationMode::Waveform;
    QString m_failureReason;
    QString m_facestreamOverlaySource;
    QString m_activeAudioClipLabel;
    bool m_hideOutsideOutputWindow = true;
    bool m_bypassGrading = false;
    bool m_correctionsEnabled = true;
    bool m_showCorrectionOverlays = true;
    bool m_showSpeakerTrackPoints = true;
    bool m_showSpeakerTrackBoxes = true;
    bool m_showRawDetections = false;
    bool m_audioSpeakerHoverModalEnabled = true;
    bool m_audioWaveformVisible = true;
    bool m_useProxyMedia = false;
    PlaybackTuning m_configuredPlaybackTuning;
    PlaybackTuning m_playbackTuning;
    int m_adaptivePlaybackBoostLevel = 0;
    qint64 m_lastAdaptivePlaybackTuningAdjustMs = 0;
    bool m_forcedPreviewDecodePreference = false;
    editor::DecodePreference m_previousDecodePreference = editor::DecodePreference::Hardware;
    bool m_bulkUpdating = false;
    int m_bulkDepth = 0;
    int m_selectedCorrectionPolygon = -1;
    int m_frameStatusExactCount = 0;
    int m_frameStatusApproxCount = 0;
    int m_frameStatusMissingCount = 0;
    int m_frameStatusHardwareCount = 0;
    int m_frameStatusCpuCount = 0;
    int64_t m_lastVisibleRequestFrame = -1;
    QString m_lastVisibleRequestClipId;
    QString m_lastVisibleRequestDecision;
    QString m_lastVisibleRequestBlockReason;
    bool m_lastVisibleRequestCached = false;
    bool m_lastVisibleRequestPending = false;
    bool m_lastVisibleRequestForceRetry = false;
    int m_lastVisibleRequestBacklog = 0;
    int64_t m_visibleRequestAttempts = 0;
    int64_t m_visibleRequestDispatched = 0;
    int64_t m_visibleRequestBlocked = 0;
    int64_t m_visibleRequestCallbacks = 0;
    int64_t m_visibleRequestNullCallbacks = 0;
    QString m_lastVisibleRequestCallbackPayload;
    QVector<PlaybackSmoothnessSample> m_playbackSmoothnessSamples;
    bool m_frameStatusRefreshQueued = false;
    bool m_frameStatusRefreshNeedsVisibleRequest = false;
};
