#pragma once

#include "preview_surface.h"

#include <QColor>
#include <QImage>
#include <QPointF>
#include <QSize>
#include <QString>
#include <QVector>
#include <QWidget>

class NullPreviewSurface final : public QWidget, public PreviewSurface {
public:
    explicit NullPreviewSurface(QWidget* parent = nullptr);
    ~NullPreviewSurface() override = default;

    QWidget* asWidget() override { return this; }
    const QWidget* asWidget() const override { return this; }

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

protected:
    void paintEvent(QPaintEvent* event) override;

private:
    void requestRepaint();

    bool m_playing = false;
    qreal m_playbackSpeed = 1.0;
    int64_t m_currentFrame = 0;
    int64_t m_currentSample = 0;
    int m_clipCount = 0;
    QString m_selectedClipId;
    QVector<TimelineClip> m_clips;
    QVector<TimelineTrack> m_tracks;
    QVector<RenderSyncMarker> m_markers;
    QVector<ExportRangeSegment> m_exportRanges;
    QSize m_outputSize = QSize(1080, 1920);
    QColor m_backgroundColor = QColor(QStringLiteral("#11161c"));
    QString m_backendLabel = QStringLiteral("offscreen-placeholder");
    QString m_facedetectionsOverlaySource = QStringLiteral("all");
    bool m_useProxyMedia = false;
    bool m_hideOutsideOutputWindow = false;
    bool m_bypassGrading = false;
    bool m_correctionsEnabled = true;
    bool m_showCorrectionOverlays = false;
    int m_selectedCorrectionPolygon = -1;
    bool m_showSpeakerTrackPoints = false;
    bool m_showSpeakerTrackBoxes = false;
    bool m_showRawDetections = false;
    bool m_showCurrentSpeakerName = false;
    bool m_showCurrentSpeakerOrganization = false;
    qreal m_currentSpeakerNameTextScale = 1.0;
    qreal m_currentSpeakerOrganizationTextScale = 1.0;
    qreal m_currentSpeakerNameVerticalPosition = 0.86;
    qreal m_currentSpeakerOrganizationVerticalPosition = 0.93;
    QString m_playbackStatusOverlayText;
    qreal m_playbackStatusOverlayProgress = -1.0;
    bool m_audioSpeakerHoverModalEnabled = false;
    bool m_audioWaveformVisible = true;
    bool m_audioMuted = false;
    qreal m_audioVolume = 1.0;
    qreal m_previewZoom = 1.0;
    QPointF m_correctionDraftAnchor;
    QVector<QPointF> m_correctionDraftPoints;
    ViewMode m_viewMode = ViewMode::Video;
    AudioVisualizationMode m_audioVisualizationMode = AudioVisualizationMode::Waveform;
    AudioDynamicsSettings m_audioDynamics;
    LoiaconoSpectrumSettings m_loiaconoSpectrumSettings;
    PlaybackTuning m_playbackTuning;
    bool m_transcriptOverlayInteractionEnabled = false;
    bool m_titleOverlayInteractionOnly = false;
    bool m_faceStreamAssignmentInteractionEnabled = false;
    bool m_correctionDrawMode = false;
    int m_bulkUpdateDepth = 0;
};
