#pragma once

#include "editor_shared.h"
#include "timeline_widget.h"

#include <QColor>
#include <QImage>
#include <QJsonObject>
#include <QPointF>
#include <QSize>
#include <QString>
#include <QVector>

#include <functional>

class QWidget;

class PreviewSurface {
public:
    enum class ViewMode {
        Video = 0,
        Audio = 1,
    };

    struct AudioDynamicsSettings {
        bool amplifyEnabled = false;
        qreal amplifyDb = 0.0;
        bool normalizeEnabled = false;
        qreal normalizeTargetDb = -1.0;
        bool selectiveNormalizeEnabled = false;
        qreal selectiveNormalizeMinSegmentSeconds = 0.5;
        qreal selectiveNormalizePeakDb = -1.0;
        int selectiveNormalizePasses = 1;
        bool peakReductionEnabled = false;
        qreal peakThresholdDb = -6.0;
        bool limiterEnabled = false;
        qreal limiterThresholdDb = -1.0;
        bool compressorEnabled = false;
        qreal compressorThresholdDb = -18.0;
        qreal compressorRatio = 3.0;
        bool waveformPreviewPostProcessing = true;
    };

    virtual ~PreviewSurface() = default;
    virtual QWidget* asWidget() = 0;
    virtual const QWidget* asWidget() const = 0;

    virtual void setPlaybackState(bool playing) = 0;
    virtual void setCurrentFrame(int64_t frame) = 0;
    virtual void setCurrentPlaybackSample(int64_t samplePosition) = 0;
    virtual void setClipCount(int count) = 0;
    virtual void setSelectedClipId(const QString& clipId) = 0;
    virtual void setTimelineClips(const QVector<TimelineClip>& clips) = 0;
    virtual void setTimelineTracks(const QVector<TimelineTrack>& tracks) = 0;
    virtual void setRenderSyncMarkers(const QVector<RenderSyncMarker>& markers) = 0;
    virtual void setExportRanges(const QVector<ExportRangeSegment>& ranges) = 0;
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
    virtual void setPreviewZoom(qreal zoom) = 0;
    virtual void setShowSpeakerTrackPoints(bool show) = 0;
    virtual void setShowSpeakerTrackBoxes(bool show) = 0;
    virtual void setBoxstreamOverlaySource(const QString& source) = 0;
    virtual void setAudioSpeakerHoverModalEnabled(bool enabled) = 0;
    virtual void setAudioWaveformVisible(bool visible) = 0;
    virtual bool audioSpeakerHoverModalEnabled() const = 0;
    virtual bool audioWaveformVisible() const = 0;
    virtual void setViewMode(ViewMode mode) = 0;
    virtual ViewMode viewMode() const = 0;
    virtual void setAudioDynamicsSettings(const AudioDynamicsSettings& settings) = 0;
    virtual AudioDynamicsSettings audioDynamicsSettings() const = 0;
    virtual void setTranscriptOverlayInteractionEnabled(bool enabled) = 0;
    virtual void setTitleOverlayInteractionOnly(bool enabled) = 0;
    virtual void setCorrectionDrawMode(bool enabled) = 0;
    virtual bool correctionDrawMode() const = 0;
    virtual bool transcriptOverlayInteractionEnabled() const = 0;
    virtual bool titleOverlayInteractionOnly() const = 0;
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
    virtual QImage latestPresentedFrameImageForClip(const QString& clipId) const = 0;
    virtual QJsonObject profilingSnapshot() const = 0;
    virtual void resetProfilingStats() = 0;
    virtual bool selectedOverlayIsTranscript() const = 0;

    std::function<void(const QString&)> selectionRequested;
    std::function<void(const QString&, qreal, qreal, bool)> resizeRequested;
    std::function<void(const QString&, qreal, qreal, bool)> moveRequested;
    std::function<void(const QString&)> createKeyframeRequested;
    std::function<void(const QString&, qreal, qreal)> correctionPointRequested;
    std::function<void(const QString&, qreal, qreal)> speakerPointRequested;
    std::function<void(const QString&, qreal, qreal, qreal)> speakerBoxRequested;
};
