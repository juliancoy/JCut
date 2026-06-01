#include "null_preview_surface.h"

#include <QFontMetrics>
#include <QJsonArray>
#include <QPainter>
#include <QPaintEvent>
#include <QStringList>

namespace {
PreviewSurface::PlaybackTuning normalizedPlaybackTuning(const PreviewSurface::PlaybackTuning& tuning)
{
    PreviewSurface::PlaybackTuning normalized = tuning;
    normalized.visibleBacklogLimit = qBound(1, normalized.visibleBacklogLimit, 16);
    normalized.sourceLookaheadFrames = qBound(1, normalized.sourceLookaheadFrames, 32);
    normalized.proxyLookaheadFrames = qBound(1, normalized.proxyLookaheadFrames, 64);
    return normalized;
}
}

NullPreviewSurface::NullPreviewSurface(QWidget* parent)
    : QWidget(parent)
{
    m_playbackTuning = normalizedPlaybackTuning(m_playbackTuning);
    setAttribute(Qt::WA_OpaquePaintEvent, true);
    setAutoFillBackground(false);
    setMinimumSize(160, 120);
}

void NullPreviewSurface::setPlaybackState(bool playing) { m_playing = playing; requestRepaint(); }
void NullPreviewSurface::setCurrentFrame(int64_t frame)
{
    m_currentFrame = qMax<int64_t>(0, frame);
    if (!m_playing) {
        requestRepaint();
    }
}
void NullPreviewSurface::setCurrentPlaybackSample(int64_t samplePosition)
{
    m_currentSample = qMax<int64_t>(0, samplePosition);
    if (!m_playing) {
        requestRepaint();
    }
}
void NullPreviewSurface::setClipCount(int count) { m_clipCount = qMax(0, count); requestRepaint(); }
void NullPreviewSurface::setSelectedClipId(const QString& clipId) { m_selectedClipId = clipId; requestRepaint(); }
void NullPreviewSurface::setTimelineClips(const QVector<TimelineClip>& clips) { m_clips = clips; requestRepaint(); }
void NullPreviewSurface::setTimelineTracks(const QVector<TimelineTrack>& tracks) { m_tracks = tracks; requestRepaint(); }
void NullPreviewSurface::setRenderSyncMarkers(const QVector<RenderSyncMarker>& markers) { m_markers = markers; }
void NullPreviewSurface::setExportRanges(const QVector<ExportRangeSegment>& ranges) { m_exportRanges = ranges; }
void NullPreviewSurface::setUseProxyMedia(bool useProxyMedia) { m_useProxyMedia = useProxyMedia; }
void NullPreviewSurface::invalidateTranscriptOverlayCache(const QString&) {}
void NullPreviewSurface::beginBulkUpdate() { ++m_bulkUpdateDepth; }
void NullPreviewSurface::endBulkUpdate()
{
    if (m_bulkUpdateDepth > 0) {
        --m_bulkUpdateDepth;
    }
    if (m_bulkUpdateDepth == 0) {
        requestRepaint();
    }
}
QString NullPreviewSurface::backendName() const { return QStringLiteral("Offscreen Placeholder Preview"); }
void NullPreviewSurface::setRenderBackendPreference(const QString& backendName) { m_backendLabel = backendName.trimmed().isEmpty() ? QStringLiteral("offscreen-placeholder") : backendName.trimmed(); requestRepaint(); }
void NullPreviewSurface::setAudioMuted(bool muted) { m_audioMuted = muted; requestRepaint(); }
void NullPreviewSurface::setAudioVolume(qreal volume) { m_audioVolume = qBound<qreal>(0.0, volume, 1.0); requestRepaint(); }
void NullPreviewSurface::setOutputSize(const QSize& size) { m_outputSize = QSize(qMax(16, size.width()), qMax(16, size.height())); requestRepaint(); }
void NullPreviewSurface::setHideOutsideOutputWindow(bool hide) { m_hideOutsideOutputWindow = hide; requestRepaint(); }
void NullPreviewSurface::setBypassGrading(bool bypass) { m_bypassGrading = bypass; requestRepaint(); }
void NullPreviewSurface::setCorrectionsEnabled(bool enabled) { m_correctionsEnabled = enabled; requestRepaint(); }
void NullPreviewSurface::setShowCorrectionOverlays(bool show) { m_showCorrectionOverlays = show; requestRepaint(); }
void NullPreviewSurface::setSelectedCorrectionPolygon(int polygonIndex) { m_selectedCorrectionPolygon = polygonIndex; requestRepaint(); }
void NullPreviewSurface::setBackgroundColor(const QColor& color) { m_backgroundColor = color; requestRepaint(); }
void NullPreviewSurface::setPreviewZoom(qreal zoom) { m_previewZoom = qMax<qreal>(0.1, zoom); requestRepaint(); }
void NullPreviewSurface::setShowSpeakerTrackPoints(bool show) { m_showSpeakerTrackPoints = show; requestRepaint(); }
void NullPreviewSurface::setShowSpeakerTrackBoxes(bool show) { m_showSpeakerTrackBoxes = show; requestRepaint(); }
void NullPreviewSurface::setShowRawDetections(bool show) { m_showRawDetections = show; requestRepaint(); }
void NullPreviewSurface::setShowCurrentSpeakerName(bool show) { m_showCurrentSpeakerName = show; requestRepaint(); }
void NullPreviewSurface::setShowCurrentSpeakerOrganization(bool show) { m_showCurrentSpeakerOrganization = show; requestRepaint(); }
void NullPreviewSurface::setCurrentSpeakerNameTextScale(qreal scale) { m_currentSpeakerNameTextScale = qBound<qreal>(0.25, scale, 3.0); requestRepaint(); }
void NullPreviewSurface::setCurrentSpeakerOrganizationTextScale(qreal scale) { m_currentSpeakerOrganizationTextScale = qBound<qreal>(0.25, scale, 3.0); requestRepaint(); }
void NullPreviewSurface::setCurrentSpeakerNameVerticalPosition(qreal position) { m_currentSpeakerNameVerticalPosition = qBound<qreal>(0.0, position, 1.0); requestRepaint(); }
void NullPreviewSurface::setCurrentSpeakerOrganizationVerticalPosition(qreal position) { m_currentSpeakerOrganizationVerticalPosition = qBound<qreal>(0.0, position, 1.0); requestRepaint(); }
void NullPreviewSurface::setPlaybackStatusOverlayText(const QString& text) { m_playbackStatusOverlayText = text.trimmed(); requestRepaint(); }
void NullPreviewSurface::setPlaybackStatusOverlayProgress(qreal progress) { m_playbackStatusOverlayProgress = progress < 0.0 ? -1.0 : qBound<qreal>(0.0, progress, 1.0); requestRepaint(); }
void NullPreviewSurface::setFacestreamOverlaySource(const QString& source) { m_facedetectionsOverlaySource = source.trimmed().isEmpty() ? QStringLiteral("all") : source.trimmed(); requestRepaint(); }
void NullPreviewSurface::setSelectedSpeakerAssignedFaceTrackIds(const QSet<int>&) {}
void NullPreviewSurface::setAudioSpeakerHoverModalEnabled(bool enabled) { m_audioSpeakerHoverModalEnabled = enabled; }
void NullPreviewSurface::setAudioWaveformVisible(bool visible) { m_audioWaveformVisible = visible; requestRepaint(); }
void NullPreviewSurface::setAudioVisualizationMode(AudioVisualizationMode mode) { m_audioVisualizationMode = mode; requestRepaint(); }
void NullPreviewSurface::setLoiaconoSpectrumSettings(const LoiaconoSpectrumSettings& settings) { m_loiaconoSpectrumSettings = settings; }
bool NullPreviewSurface::audioSpeakerHoverModalEnabled() const { return m_audioSpeakerHoverModalEnabled; }
bool NullPreviewSurface::audioWaveformVisible() const { return m_audioWaveformVisible; }
void NullPreviewSurface::setViewMode(ViewMode mode) { m_viewMode = mode; requestRepaint(); }
PreviewSurface::ViewMode NullPreviewSurface::viewMode() const { return m_viewMode; }
void NullPreviewSurface::setAudioDynamicsSettings(const AudioDynamicsSettings& settings) { m_audioDynamics = settings; }
PreviewSurface::AudioDynamicsSettings NullPreviewSurface::audioDynamicsSettings() const { return m_audioDynamics; }
void NullPreviewSurface::setTranscriptOverlayInteractionEnabled(bool enabled) { m_transcriptOverlayInteractionEnabled = enabled; requestRepaint(); }
void NullPreviewSurface::setTitleOverlayInteractionOnly(bool enabled) { m_titleOverlayInteractionOnly = enabled; requestRepaint(); }
void NullPreviewSurface::setFaceDetectionsAssignmentInteractionEnabled(bool enabled) { m_faceStreamAssignmentInteractionEnabled = enabled; requestRepaint(); }
void NullPreviewSurface::setCorrectionDrawMode(bool enabled) { m_correctionDrawMode = enabled; requestRepaint(); }
bool NullPreviewSurface::correctionDrawMode() const { return m_correctionDrawMode; }
bool NullPreviewSurface::transcriptOverlayInteractionEnabled() const { return m_transcriptOverlayInteractionEnabled; }
bool NullPreviewSurface::titleOverlayInteractionOnly() const { return m_titleOverlayInteractionOnly; }
bool NullPreviewSurface::faceStreamAssignmentInteractionEnabled() const { return m_faceStreamAssignmentInteractionEnabled; }
void NullPreviewSurface::setCorrectionDraftPoints(const QVector<QPointF>& points) { m_correctionDraftPoints = points; requestRepaint(); }
qreal NullPreviewSurface::previewZoom() const { return m_previewZoom; }
void NullPreviewSurface::resetPreviewPan() {}
QSize NullPreviewSurface::outputSize() const { return m_outputSize; }
bool NullPreviewSurface::bypassGrading() const { return m_bypassGrading; }
bool NullPreviewSurface::correctionsEnabled() const { return m_correctionsEnabled; }
bool NullPreviewSurface::audioMuted() const { return m_audioMuted; }
int NullPreviewSurface::audioVolumePercent() const { return qRound(m_audioVolume * 100.0); }
QString NullPreviewSurface::activeAudioClipLabel() const
{
    for (const TimelineClip& clip : m_clips) {
        if (clip.id == m_selectedClipId) {
            return clip.id;
        }
    }
    return m_selectedClipId;
}
bool NullPreviewSurface::preparePlaybackAdvance(int64_t) { return true; }
bool NullPreviewSurface::preparePlaybackAdvanceSample(int64_t) { return true; }
bool NullPreviewSurface::warmPlaybackLookahead(int, int) { return true; }
void NullPreviewSurface::setPlaybackTuning(const PlaybackTuning& tuning) { m_playbackTuning = normalizedPlaybackTuning(tuning); }
PreviewSurface::PlaybackTuning NullPreviewSurface::playbackTuning() const { return m_playbackTuning; }
QImage NullPreviewSurface::latestPresentedFrameImageForClip(const QString&) const { return {}; }
QVector<PreviewSurface::PipelineStageSnapshot> NullPreviewSurface::livePipelineSnapshots() const { return {}; }
QJsonObject NullPreviewSurface::pipelineHealthSnapshot() const
{
    return QJsonObject{
        {QStringLiteral("ok"), true},
        {QStringLiteral("backend"), QStringLiteral("offscreen_placeholder")},
        {QStringLiteral("clip_count"), m_clips.size()},
        {QStringLiteral("selected_clip_id"), m_selectedClipId},
        {QStringLiteral("current_frame"), static_cast<qint64>(m_currentFrame)},
        {QStringLiteral("current_sample"), static_cast<qint64>(m_currentSample)},
        {QStringLiteral("playing"), m_playing},
        {QStringLiteral("pipeline_stages"), QJsonArray{}}
    };
}
QJsonObject NullPreviewSurface::profilingSnapshot() const
{
    return QJsonObject{
        {QStringLiteral("ok"), true},
        {QStringLiteral("backend"), QStringLiteral("offscreen_placeholder")},
        {QStringLiteral("clip_count"), m_clips.size()},
        {QStringLiteral("selected_clip_id"), m_selectedClipId},
        {QStringLiteral("playback_status_overlay_text"), m_playbackStatusOverlayText},
        {QStringLiteral("playback_status_overlay_progress"), m_playbackStatusOverlayProgress},
        {QStringLiteral("current_frame"), static_cast<qint64>(m_currentFrame)},
        {QStringLiteral("playing"), m_playing}
    };
}
void NullPreviewSurface::resetProfilingStats() {}
bool NullPreviewSurface::selectedOverlayIsTranscript() const { return false; }

void NullPreviewSurface::paintEvent(QPaintEvent* event)
{
    Q_UNUSED(event)

    QPainter painter(this);
    painter.fillRect(rect(), m_backgroundColor);

    QRect panel = rect().adjusted(16, 16, -16, -16);
    painter.setRenderHint(QPainter::Antialiasing, true);
    painter.setPen(Qt::NoPen);
    painter.setBrush(QColor(16, 22, 30, 220));
    painter.drawRoundedRect(panel, 14, 14);

    painter.setPen(QColor(QStringLiteral("#eef4fb")));
    QFont titleFont = painter.font();
    titleFont.setPointSize(qMax(10, titleFont.pointSize() + 2));
    titleFont.setBold(true);
    painter.setFont(titleFont);
    painter.drawText(panel.adjusted(18, 16, -18, -16),
                     Qt::AlignTop | Qt::AlignLeft,
                     QStringLiteral("Offscreen Preview Placeholder"));

    painter.setPen(QColor(QStringLiteral("#a8bbcf")));
    QFont bodyFont = titleFont;
    bodyFont.setBold(false);
    bodyFont.setPointSize(qMax(9, bodyFont.pointSize() - 1));
    painter.setFont(bodyFont);

    QStringList lines{
        QStringLiteral("Backend: %1").arg(m_backendLabel),
        QStringLiteral("Output: %1 x %2").arg(m_outputSize.width()).arg(m_outputSize.height()),
        QStringLiteral("Frame: %1").arg(m_currentFrame),
        QStringLiteral("Playback: %1").arg(m_playing ? QStringLiteral("playing") : QStringLiteral("stopped")),
        QStringLiteral("Clips: %1").arg(m_clips.size()),
        QStringLiteral("Selected Clip: %1").arg(m_selectedClipId.isEmpty() ? QStringLiteral("-") : m_selectedClipId),
        QStringLiteral("View: %1").arg(m_viewMode == ViewMode::Audio ? QStringLiteral("audio") : QStringLiteral("video")),
        QStringLiteral("FaceDetections Overlay: %1").arg(m_facedetectionsOverlaySource),
    };
    if (m_showSpeakerTrackBoxes || m_showSpeakerTrackPoints || m_showRawDetections ||
        m_showCurrentSpeakerName || m_showCurrentSpeakerOrganization) {
        QStringList overlays;
        if (m_showSpeakerTrackPoints) overlays.push_back(QStringLiteral("points"));
        if (m_showSpeakerTrackBoxes) overlays.push_back(QStringLiteral("boxes"));
        if (m_showRawDetections) overlays.push_back(QStringLiteral("raw-detections"));
        if (m_showCurrentSpeakerName) overlays.push_back(QStringLiteral("speaker-name"));
        if (m_showCurrentSpeakerOrganization) overlays.push_back(QStringLiteral("speaker-organization"));
        lines.push_back(QStringLiteral("Preview Overlays: %1").arg(overlays.join(QStringLiteral(", "))));
    }
    if (!m_playbackStatusOverlayText.isEmpty()) {
        lines.push_back(QStringLiteral("Playback Status: %1").arg(m_playbackStatusOverlayText));
    }

    painter.drawText(panel.adjusted(18, 52, -18, -18),
                     Qt::AlignTop | Qt::AlignLeft,
                     lines.join(QLatin1Char('\n')));

    if (!m_playbackStatusOverlayText.isEmpty()) {
        QFont badgeFont = painter.font();
        badgeFont.setBold(true);
        badgeFont.setPointSize(qMax(11, badgeFont.pointSize() + 1));
        painter.setFont(badgeFont);
        const QFontMetrics metrics(badgeFont);
        const QRectF badgeRect(
            panel.center().x() - qMin<qreal>(panel.width() - 40.0, metrics.horizontalAdvance(m_playbackStatusOverlayText) + 44.0) * 0.5,
            panel.top() + 18.0,
            qMin<qreal>(panel.width() - 40.0, metrics.horizontalAdvance(m_playbackStatusOverlayText) + 44.0),
            38.0);
        painter.setPen(QPen(QColor(255, 209, 102, 235), 2.0));
        painter.setBrush(QColor(18, 20, 24, 224));
        painter.drawRoundedRect(badgeRect, 8.0, 8.0);
        painter.setPen(QColor(255, 241, 190, 255));
        painter.drawText(badgeRect.adjusted(14.0, 0.0, -14.0, 0.0),
                         Qt::AlignCenter,
                         metrics.elidedText(m_playbackStatusOverlayText, Qt::ElideRight, qMax(1, qRound(badgeRect.width() - 28.0))));
        if (m_playbackStatusOverlayProgress >= 0.0) {
            const QRectF trackRect = badgeRect.adjusted(14.0, badgeRect.height() - 9.0, -14.0, -4.0);
            painter.setPen(Qt::NoPen);
            painter.setBrush(QColor(255, 244, 204, 56));
            painter.drawRoundedRect(trackRect, 2.0, 2.0);
            QRectF fillRect = trackRect;
            fillRect.setWidth(qMax<qreal>(1.0, trackRect.width() * qBound<qreal>(0.0, m_playbackStatusOverlayProgress, 1.0)));
            painter.setBrush(QColor(255, 209, 102, 235));
            painter.drawRoundedRect(fillRect, 2.0, 2.0);
        }
    }
}

void NullPreviewSurface::requestRepaint()
{
    if (m_bulkUpdateDepth <= 0) {
        update();
    }
}
