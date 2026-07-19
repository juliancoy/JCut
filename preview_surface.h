#pragma once

#include "background_fill_effect_fwd.h"
#include "loiacono/spectrum_settings_dialog.h"
#include "playback_timing_context.h"

#include <QColor>
#include <QImage>
#include <QJsonObject>
#include <QPointF>
#include <QSet>
#include <QSize>
#include <QString>
#include <QVector>
#include <QtGlobal>

#include <functional>

class QWidget;
struct ExportRangeSegment;
struct RenderSyncMarker;
struct TimelineClip;
struct TimelineTrack;

class PreviewSurface {
public:
    using LoiaconoSpectrumSettings = loiacono::SpectrumSettings;
    enum class ViewMode {
        Video = 0,
        Audio = 1,
    };

    enum class AudioVisualizationMode {
        Waveform = 0,
        Spectrum = 1,
    };

    struct AudioDynamicsSettings {
        bool amplifyEnabled = false;
        qreal amplifyDb = 0.0;
        bool normalizeEnabled = false;
        qreal normalizeTargetDb = -1.0;
        bool selectiveNormalizeEnabled = false;
        qreal selectiveNormalizeMinSegmentSeconds = 0.5;
        qreal selectiveNormalizePeakDb = -12.0;
        int selectiveNormalizePasses = 1;
        bool selectiveNormalizeOverlayVisible = true;
        bool transcriptNormalizeEnabled = false;
        bool peakReductionEnabled = false;
        qreal peakThresholdDb = -6.0;
        bool limiterEnabled = false;
        qreal limiterThresholdDb = -1.0;
        bool compressorEnabled = false;
        qreal compressorThresholdDb = -18.0;
        qreal compressorRatio = 3.0;
        bool softClipEnabled = false;
        bool stereoToMonoEnabled = false;
        bool waveformPreviewPostProcessing = true;
    };

    struct PipelineStageSnapshot {
        QString label;
        QString detail;
        QImage image;
        QString kind;
        bool exact = false;
        bool active = false;
        QString state;
        QJsonObject facts;
    };

    struct PlaybackTuning {
        int visibleBacklogLimit = 4;
        int sourceLookaheadFrames = 5;
        int proxyLookaheadFrames = 8;
    };

    virtual ~PreviewSurface() = default;
    virtual QWidget* asWidget() = 0;
    virtual const QWidget* asWidget() const = 0;

    virtual void setPlaybackState(bool playing) = 0;
    virtual void setPlaybackSpeed(qreal speed) = 0;
    virtual void setCurrentFrame(int64_t frame) = 0;
    virtual void setCurrentPlaybackSample(int64_t samplePosition) = 0;
    virtual void setClipCount(int count) = 0;
    virtual void setSelectedClipId(const QString& clipId) = 0;
    virtual void setTimelineClips(const QVector<TimelineClip>& clips) = 0;
    virtual void setTimelineTracks(const QVector<TimelineTrack>& tracks) = 0;
    virtual void setRenderSyncMarkers(const QVector<RenderSyncMarker>& markers) = 0;
    virtual void setExportRanges(const QVector<ExportRangeSegment>& ranges) = 0;
    virtual void setPlaybackTimingContext(const PlaybackTimingContext& timing) = 0;
    virtual void setUseProxyMedia(bool useProxyMedia) = 0;
    virtual void invalidateTranscriptOverlayCache(const QString& clipFilePath = QString()) = 0;
    virtual void beginBulkUpdate() = 0;
    virtual void endBulkUpdate() = 0;
    virtual QString backendName() const = 0;
    virtual void setRenderBackendPreference(const QString& backendName) = 0;
    virtual void setAudioMuted(bool muted) = 0;
    virtual void setAudioVolume(qreal volume) = 0;
    virtual void setOutputSize(const QSize& size) = 0;
    virtual void setHideOutsideOutputWindow(bool hide) = 0;
    virtual void setBypassGrading(bool bypass) = 0;
    virtual void setCorrectionsEnabled(bool enabled) = 0;
    virtual void setShowCorrectionOverlays(bool show) = 0;
    virtual void setSelectedCorrectionPolygon(int polygonIndex) = 0;
    virtual void setBackgroundColor(const QColor& color) = 0;
    virtual void setBackgroundFillEffect(BackgroundFillEffect effect) = 0;
    virtual void setBackgroundFillOpacity(qreal opacity) = 0;
    virtual void setBackgroundFillBrightness(qreal brightness) = 0;
    virtual void setBackgroundFillSaturation(qreal saturation) = 0;
    virtual void setBackgroundFillEdgePixels(int pixels) = 0;
    virtual void setBackgroundFillEdgeProgressive(bool progressive) = 0;
    virtual void setBackgroundFillEdgePower(qreal power) = 0;
    virtual void setBackgroundFillStretchSourceClipId(const QString& clipId) = 0;
    virtual void setPreviewZoom(qreal zoom) = 0;
    virtual void setShowSpeakerTrackPoints(bool show) = 0;
    virtual void setShowSpeakerTrackBoxes(bool show) = 0;
    virtual void setShowRawDetections(bool show) = 0;
    virtual void setShowCurrentSpeakerName(bool show) = 0;
    virtual void setShowCurrentSpeakerOrganization(bool show) = 0;
    virtual void setCurrentSpeakerNameTextScale(qreal scale) = 0;
    virtual void setCurrentSpeakerOrganizationTextScale(qreal scale) = 0;
    virtual void setCurrentSpeakerNameVerticalPosition(qreal position) = 0;
    virtual void setCurrentSpeakerOrganizationVerticalPosition(qreal position) = 0;
    virtual void setCurrentSpeakerNameColor(const QColor& color) = 0;
    virtual void setCurrentSpeakerOrganizationColor(const QColor& color) = 0;
    virtual void setCurrentSpeakerBackgroundColor(const QColor& color) = 0;
    virtual void setCurrentSpeakerBorderColor(const QColor& color) = 0;
    virtual void setCurrentSpeakerBackgroundCornerRadius(qreal radius) = 0;
    virtual void setCurrentSpeakerBorderWidth(qreal width) = 0;
    virtual void setCurrentSpeakerShadowEnabled(bool enabled) = 0;
    virtual void setCurrentSpeakerShadowColor(const QColor& color) = 0;
    virtual void setTranscriptOverlayTimingPaddingMs(int prependMs, int postpendMs, int offsetMs = 0) = 0;
    virtual void setPlaybackStatusOverlayText(const QString& text) = 0;
    virtual void setPlaybackStatusOverlayProgress(qreal progress) = 0;
    virtual void setFacestreamOverlaySource(const QString& source) = 0;
    virtual void setSelectedSpeakerAssignedFaceTrackIds(const QSet<int>& trackIds) = 0;
    virtual void setAudioSpeakerHoverModalEnabled(bool enabled) = 0;
    virtual void setAudioWaveformVisible(bool visible) = 0;
    virtual void setAudioVisualizationMode(AudioVisualizationMode mode) = 0;
    virtual void setLoiaconoSpectrumSettings(const LoiaconoSpectrumSettings& settings) = 0;
    virtual bool audioSpeakerHoverModalEnabled() const = 0;
    virtual bool audioWaveformVisible() const = 0;
    virtual void setViewMode(ViewMode mode) = 0;
    virtual ViewMode viewMode() const = 0;
    virtual void setAudioDynamicsSettings(const AudioDynamicsSettings& settings) = 0;
    virtual AudioDynamicsSettings audioDynamicsSettings() const = 0;
    virtual void setTranscriptOverlayInteractionEnabled(bool enabled) = 0;
    virtual void setTitleOverlayInteractionOnly(bool enabled) = 0;
    virtual void setFaceDetectionsAssignmentInteractionEnabled(bool enabled) = 0;
    virtual void setCorrectionDrawMode(bool enabled) = 0;
    virtual bool correctionDrawMode() const = 0;
    virtual bool transcriptOverlayInteractionEnabled() const = 0;
    virtual bool titleOverlayInteractionOnly() const = 0;
    virtual bool faceStreamAssignmentInteractionEnabled() const = 0;
    virtual void setCorrectionDraftPoints(const QVector<QPointF>& points) = 0;
    virtual qreal previewZoom() const = 0;
    virtual void resetPreviewPan() = 0;
    virtual QSize outputSize() const = 0;
    virtual bool bypassGrading() const = 0;
    virtual bool correctionsEnabled() const = 0;
    virtual bool audioMuted() const = 0;
    virtual int audioVolumePercent() const = 0;
    virtual QString activeAudioClipLabel() const = 0;
    virtual bool preparePlaybackAdvance(int64_t targetFrame) = 0;
    virtual bool preparePlaybackAdvanceSample(int64_t targetSample) = 0;
    virtual bool warmPlaybackLookahead(int futureFrames, int timeoutMs) = 0;
    virtual void setPlaybackTuning(const PlaybackTuning& tuning) = 0;
    virtual PlaybackTuning playbackTuning() const = 0;
    virtual QImage latestPresentedFrameImageForClip(const QString& clipId) const = 0;
    virtual QVector<PipelineStageSnapshot> livePipelineSnapshots() const = 0;
    virtual QJsonObject pipelineHealthSnapshot() const = 0;
    virtual QJsonObject profilingSnapshot() const = 0;
    virtual void resetProfilingStats() = 0;
    virtual bool selectedOverlayIsTranscript() const = 0;

    std::function<void(const QString&)> selectionRequested;
    std::function<void(const QString&, qreal, qreal, bool)> moveRequested;
    std::function<void(const QString&, qreal, qreal, qreal, qreal, bool)> transformRequested;
    std::function<void(const QString&)> createKeyframeRequested;
    std::function<void(const QString&)> hardwareDecodeConversionRequested;
    std::function<void(int64_t)> playbackSampleRequested;
    std::function<void(const QString&, qreal, qreal)> correctionPointRequested;
    std::function<void(const QString&, qreal, qreal)> speakerPointRequested;
    std::function<void(const QString&, qreal, qreal, qreal)> speakerBoxRequested;
    std::function<void(const QString&, int, const QString&, int64_t, qreal, qreal, qreal)> faceStreamBoxRequested;
    std::function<void(const QString&, int, const QString&, int64_t, qreal, qreal, qreal)> faceStreamBoxFocusClearRequested;
    std::function<void(const QString&)> faceStreamBoxClickStatus;
};
