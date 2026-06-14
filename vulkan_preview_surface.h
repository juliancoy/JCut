#pragma once

#include "preview_interaction_state.h"
#include "preview_surface.h"
#include "facedetections_types.h"
#include "facedetections_time_mapping.h"
#include "facestream_overlay_snapshot.h"
#include "debug_controls.h"
#include "frame_handle.h"
#include "playback_stage_metrics.h"

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
class PlaybackFramePipeline;
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
    void setPlaybackSpeed(qreal speed) override;
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
    void setShowCurrentSpeakerName(bool show) override;
    void setShowCurrentSpeakerOrganization(bool show) override;
    void setCurrentSpeakerNameTextScale(qreal scale) override;
    void setCurrentSpeakerOrganizationTextScale(qreal scale) override;
    void setCurrentSpeakerNameVerticalPosition(qreal position) override;
    void setCurrentSpeakerOrganizationVerticalPosition(qreal position) override;
    void setCurrentSpeakerNameColor(const QColor& color) override;
    void setCurrentSpeakerOrganizationColor(const QColor& color) override;
    void setCurrentSpeakerBackgroundColor(const QColor& color) override;
    void setCurrentSpeakerBorderColor(const QColor& color) override;
    void setCurrentSpeakerBackgroundCornerRadius(qreal radius) override;
    void setCurrentSpeakerBorderWidth(qreal width) override;
    void setCurrentSpeakerShadowEnabled(bool enabled) override;
    void setCurrentSpeakerShadowColor(const QColor& color) override;
    void setPlaybackStatusOverlayText(const QString& text) override;
    void setPlaybackStatusOverlayProgress(qreal progress) override;
    void setFacestreamOverlaySource(const QString& source) override;
    void setSelectedSpeakerAssignedFaceTrackIds(const QSet<int>& trackIds) override;
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
    void setFaceDetectionsAssignmentInteractionEnabled(bool enabled) override;
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
    QJsonObject pipelineHealthSnapshot() const override;
    QJsonObject profilingSnapshot() const override;
    void resetProfilingStats() override;
    bool selectedOverlayIsTranscript() const override;

private:
    using FacestreamKeyframe = FacestreamResolvedKeyframe;
    using FacestreamTrack = FacestreamResolvedTrack;
    using FacestreamOverlayCacheEntry = jcut::preview_overlay::FacestreamOverlayCacheEntry;
    using FacestreamOverlayRequestClip = jcut::preview_overlay::FacestreamOverlayRequestClip;
    using FacestreamOverlaySnapshot = jcut::preview_overlay::FacestreamOverlaySnapshot;
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

    static QRectF facestreamKeyframeBoxNorm(const FacestreamKeyframe& keyframe,
                                            const QSize& clipFrameSize);
    static QVector<VulkanPreviewFacestreamOverlay> rawDetectionsFromCacheEntry(
        const FacestreamOverlayCacheEntry& entry,
        int64_t sourceFrame);

    void requestNativeUpdate();
    void updateNativeTitle();
    void ensureFramePipeline();
    bool hasPlaybackLookaheadBuffered(int futureFrames) const;
    bool currentPlaybackFrameReadyForStart() const;
    int effectivePlaybackLookaheadFrames() const;
    void registerVisibleClips();
    void requestFramesForCurrentPosition();
    void refreshVulkanFrameStatuses();
    bool loadedFrameAffectsCurrentView(const QString& clipId, int64_t frame) const;
    void queueFrameStatusRefresh(bool requestVisibleFrames);
    void applyAdaptivePlaybackTuning();
    void updateAdaptivePlaybackTuning(qint64 nowMs);
    void refreshFacestreamOverlays();
    void requestFacestreamOverlaySnapshotAsync(
        const QString& requestKey,
        const QVector<FacestreamOverlayRequestClip>& requestClips,
        const QString& selectedClipId,
        const QString& sourceFilter,
        bool showSpeakerTrackBoxes,
        bool showRawDetections,
        bool assignmentInteractionEnabled);
    void startFacestreamOverlaySnapshotWorker(
        const QString& requestKey,
        const QVector<FacestreamOverlayRequestClip>& requestClips,
        const QString& selectedClipId,
        const QString& sourceFilter,
        bool showSpeakerTrackBoxes,
        bool showRawDetections,
        bool assignmentInteractionEnabled);
    void applyFacestreamOverlaySnapshot(const FacestreamOverlaySnapshot& snapshot);
    void queueFacestreamOverlayCacheWarmup(const TimelineClip& clip,
                                           int64_t sourceFrame,
                                           const QString& cacheKey);
    bool warmFacestreamOverlayLookahead(int futureFrames, int timeoutMs);
    QVector<FacestreamTrack> loadFacestreamTracksForClip(const TimelineClip& clip,
                                                         int64_t sourceFrame);
    QVector<VulkanPreviewFacestreamOverlay> rawDetectionsForClipFrame(const TimelineClip& clip,
                                                                      int64_t sourceFrame);
    QVector<FacestreamTrack> parseContinuityTracksForClip(const TimelineClip& clip,
                                                          const QJsonArray& streams,
                                                          const QJsonObject& continuityRoot) const;
    QVector<FacestreamTrack> convertContinuityTrackModelsForClip(
        const TimelineClip& clip,
        const QVector<jcut::facedetections::FacestreamTrack>& tracks,
        const QString& frameDomain,
        const QString& detectorMode) const;
    QVector<VulkanPreviewFacestreamOverlay> convertRawDetectionModelsForClip(
        const TimelineClip& clip,
        const QVector<jcut::facedetections::FacestreamFrameDetections>& frames,
        FacestreamFrameDomain frameDomain) const;
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
    std::unique_ptr<editor::PlaybackFramePipeline> m_playbackPipeline;
    QSet<QString> m_registeredClips;
    QHash<QString, QString> m_registeredClipRegistrationKeys;
    QHash<QString, FacestreamOverlayCacheEntry> m_facedetectionsOverlayCache;
    QSet<QString> m_pendingFacestreamOverlayCacheWarmups;
    QHash<QString, QJsonObject> m_facedetectionsArtifactRootCache;
    QHash<QString, QJsonObject> m_facedetectionsProcessedArtifactRootCache;
    PreviewInteractionState m_interaction;
    AudioDynamicsSettings m_audioDynamics;
    LoiaconoSpectrumSettings m_loiaconoSpectrumSettings;
    AudioVisualizationMode m_audioVisualizationMode = AudioVisualizationMode::Waveform;
    QString m_failureReason;
    QString m_facedetectionsOverlaySource;
    QString m_activeAudioClipLabel;
    bool m_hideOutsideOutputWindow = true;
    bool m_bypassGrading = false;
    bool m_correctionsEnabled = true;
    bool m_showCorrectionOverlays = true;
    bool m_showSpeakerTrackPoints = true;
    bool m_showSpeakerTrackBoxes = false;
    bool m_showRawDetections = false;
    bool m_audioSpeakerHoverModalEnabled = true;
    bool m_audioWaveformVisible = true;
    bool m_useProxyMedia = false;
    PlaybackTuning m_configuredPlaybackTuning;
    PlaybackTuning m_playbackTuning;
    int m_adaptivePlaybackBoostLevel = 0;
    qint64 m_lastAdaptivePlaybackTuningAdjustMs = 0;
    qreal m_playbackSpeed = 1.0;
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
    QHash<QString, editor::FrameHandle> m_lastPresentedFrameByClip;
    int64_t m_lastVisibleRequestFrame = -1;
    QString m_lastVisibleRequestClipId;
    QString m_lastVisibleRequestDecision;
    QString m_lastVisibleRequestBlockReason;
    bool m_lastVisibleRequestCached = false;
    bool m_lastVisibleRequestExactCached = false;
    bool m_lastVisibleRequestDisplayableCached = false;
    bool m_lastVisibleRequestPending = false;
    bool m_lastVisibleRequestForceRetry = false;
    int m_lastVisibleRequestBacklog = 0;
    int64_t m_visibleRequestAttempts = 0;
    int64_t m_visibleRequestDispatched = 0;
    int64_t m_visibleRequestBlocked = 0;
    int64_t m_visibleRequestCallbacks = 0;
    int64_t m_visibleRequestNullCallbacks = 0;
    QString m_lastVisibleRequestCallbackPayload;
    editor::PlaybackStageMetric m_timelineInputStageMetric;
    editor::PlaybackStageMetric m_sourceMappingStageMetric;
    editor::PlaybackStageMetric m_visibleRequestStageMetric;
    editor::PlaybackStageMetric m_cacheLookupStageMetric;
    editor::PlaybackStageMetric m_decoderOutputStageMetric;
    editor::PlaybackStageMetric m_frameSelectionStageMetric;
    editor::PlaybackStageMetric m_correctionsMaskStageMetric;
    editor::PlaybackStageMetric m_effectsEvalStageMetric;
    editor::PlaybackStageMetric m_gradingShaderStageMetric;
    editor::PlaybackStageMetric m_transformStageMetric;
    editor::PlaybackStageMetric m_overlayPrepStageMetric;
    QVector<PlaybackSmoothnessSample> m_playbackSmoothnessSamples;
    QJsonObject m_lastFacedetectionsQueryDebug;
    QString m_appliedFacestreamOverlaySnapshotKey;
    QString m_pendingFacestreamOverlaySnapshotKey;
    QString m_queuedFacestreamOverlaySnapshotKey;
    QVector<FacestreamOverlayRequestClip> m_queuedFacestreamOverlayRequestClips;
    QString m_queuedFacestreamOverlaySelectedClipId;
    QString m_queuedFacestreamOverlaySourceFilter;
    bool m_queuedFacestreamOverlayShowSpeakerTrackBoxes = false;
    bool m_queuedFacestreamOverlayShowRawDetections = false;
    bool m_queuedFacestreamOverlayAssignmentInteractionEnabled = false;
    uint64_t m_nextFacestreamOverlayRequestId = 0;
    uint64_t m_latestAppliedFacestreamOverlayRequestId = 0;
    bool m_facedetectionsOverlayWorkerPending = false;
    qint64 m_lastFacedetectionsOverlayPrepMs = 0;
    qint64 m_lastFacedetectionsOverlayApplyLatencyMs = 0;
    qint64 m_lastFacedetectionsOverlayAppliedAtMs = 0;
    qint64 m_lastFacedetectionsOverlayQueuedAtMs = 0;
    int64_t m_facedetectionsOverlayWorkerStarted = 0;
    int64_t m_facedetectionsOverlayWorkerApplied = 0;
    int64_t m_facedetectionsOverlayWorkerDropped = 0;
    int64_t m_facedetectionsOverlayWorkerCoalesced = 0;
    int m_lastFacedetectionsOverlayRequestClipCount = 0;
    int m_lastFacedetectionsOverlayTrackCandidateCount = 0;
    int m_lastFacedetectionsOverlayMatchCount = 0;
    int m_lastFacedetectionsRawDetectionMatchCount = 0;
    bool m_frameStatusRefreshQueued = false;
    bool m_frameStatusRefreshNeedsVisibleRequest = false;
    qint64 m_lastFrameStatusTrimMs = 0;
    qint64 m_frameStatusRefreshCount = 0;
    qint64 m_lastFrameStatusRefreshMs = 0;
    qint64 m_maxFrameStatusRefreshMs = 0;
    qint64 m_lastFrameStatusRefreshWarnAtMs = 0;
};
