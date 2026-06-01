#include "opengl_preview.h"
#include "audio_preview_support.h"
#include "preview_view_transform.h"
#include "opengl_preview_debug.h"

#include "frame_handle.h"
#include "async_decoder.h"
#include "timeline_cache.h"
#include "playback_frame_pipeline.h"
#include "memory_budget.h"
#include "media_pipeline_shared.h"
#include "preview_speaker_profiles.h"
#include "waveform_service.h"
#include "render_backend.h"
#include "render_internal.h"

#include <QElapsedTimer>
#include <QCoreApplication>
#include <QJsonArray>
#include <QJsonObject>
#include <QMessageBox>
#include <QOpenGLWidget>
#include <QPointer>
#include <QStringList>
#include <QThread>
#include <QTimer>

#include <algorithm>
#include <cmath>
#include <numeric>
#include <vector>

using namespace editor;

namespace {
QString cacheRegistrationKeyForClip(const TimelineClip& clip) {
    const QString decodePath = interactivePreviewMediaPathForClip(clip);
    return QStringLiteral("%1|%2|%3|%4|%5|%6|%7|%8|%9")
        .arg(decodePath)
        .arg(clip.filePath)
        .arg(static_cast<int>(clip.sourceKind))
        .arg(clip.startFrame)
        .arg(clip.durationFrames)
        .arg(clip.sourceInFrame)
        .arg(clip.sourceDurationFrames)
        .arg(QString::number(clip.playbackRate, 'f', 6))
        .arg(QString::number(clip.sourceFps, 'f', 6));
}

bool suppressBridgeFallbackLogs() {
    return qEnvironmentVariableIntValue("JCUT_PREVIEW_SUPPRESS_BRIDGE_LOG") == 1;
}
} // namespace

PreviewWindow::PreviewWindow(QWidget* parent)
    : QOpenGLWidget(parent)
    , m_quadBuffer(QOpenGLBuffer::VertexBuffer)
    , m_polygonBuffer(QOpenGLBuffer::VertexBuffer)
{
    const RenderBackend requestedBackend = desiredRenderBackendFromEnvironment();
    configurePreviewBackend(requestedBackend, false);
    m_playbackTuning.visibleBacklogLimit = 4;
    m_playbackTuning.sourceLookaheadFrames = 5;
    m_playbackTuning.proxyLookaheadFrames = 8;

    setMinimumSize(320, 180);
    setMouseTracking(true);
    m_lastPaintMs = nowMs();
    m_repaintTimer.setSingleShot(false);
    m_repaintTimer.setInterval(16);
    connect(&m_repaintTimer, &QTimer::timeout, this, [this]() {
        if (!isVisible() || m_bulkUpdateDepth > 0) {
            return;
        }

        const qint64 now = nowMs();
        const bool pendingDecodeWork =
            (m_cache && m_cache->pendingVisibleRequestCount() > 0) ||
            (m_decoder && m_decoder->pendingRequestCount() > 0) ||
            m_pendingFrameRequest;
        const bool stalledPresentation =
            m_lastPaintMs <= 0 || (now - m_lastPaintMs) > 100;

        if (m_frameRequestsArmed && m_pendingFrameRequest && !m_frameRequestTimer.isActive()) {
            m_frameRequestTimer.start();
        }

        if (stalledPresentation && (m_interaction.playing || pendingDecodeWork)) {
            update();
        }

        if (!m_interaction.playing && !pendingDecodeWork) {
            m_repaintTimer.stop();
        }
    });
    m_frameRequestTimer.setSingleShot(true);
    m_frameRequestTimer.setInterval(0);
    connect(&m_frameRequestTimer, &QTimer::timeout, this, [this]() {
        if (!m_frameRequestsArmed || !isVisible() || m_bulkUpdateDepth > 0 || !m_pendingFrameRequest) {
            return;
        }
        m_pendingFrameRequest = false;
        requestFramesForCurrentPosition();
    });

    QPointer<PreviewWindow> self(this);
    editor::WaveformService::instance().setReadyCallback([self]() {
        if (!self) {
            return;
        }
        QMetaObject::invokeMethod(self, [self]() {
            if (!self || self->viewMode() != PreviewWindow::ViewMode::Audio) {
                return;
            }
            self->scheduleRepaint();
        }, Qt::QueuedConnection);
    });
}

PreviewWindow::~PreviewWindow() {
    if (m_cache) {
        m_cache->stopPrefetching();
    }
    if (context()) {
        makeCurrent();
        releaseGlResources();
        doneCurrent();
    }
}

void PreviewWindow::setPlaybackState(bool playing) {
    playbackTrace(QStringLiteral("PreviewWindow::setPlaybackState"),
                  QStringLiteral("playing=%1 clips=%2 cache=%3")
                      .arg(playing)
                      .arg(m_interaction.clips.size())
                      .arg(m_cache != nullptr));
    m_interaction.playing = playing;
    if (playing && !m_interaction.clips.isEmpty()) {
        ensurePipeline();
    }
    if (m_cache) {
        m_cache->setPlaybackState(playing ? TimelineCache::PlaybackState::Playing
                                          : TimelineCache::PlaybackState::Stopped);
    }
    if (m_playbackPipeline) {
        m_playbackPipeline->setPlaybackActive(playing);
    }
    if (!playing) {
        m_lastPresentedFrames.clear();
    }
    if (playing && isVisible() && !m_repaintTimer.isActive()) {
        m_repaintTimer.start();
    } else if (!playing && !m_pendingFrameRequest && m_repaintTimer.isActive()) {
        m_repaintTimer.stop();
    }
}

void PreviewWindow::setPlaybackSpeed(qreal speed)
{
    m_playbackSpeed = qBound<qreal>(0.1, speed, 4.0);
    if (m_cache) {
        m_cache->setPlaybackSpeed(m_playbackSpeed);
    }
}

void PreviewWindow::setCurrentFrame(int64_t frame) {
    playbackTrace(QStringLiteral("PreviewWindow::setCurrentFrame"),
                  QStringLiteral("frame=%1 visible=%2 cache=%3")
                      .arg(frame)
                      .arg(isVisible())
                      .arg(m_cache != nullptr));
    setCurrentPlaybackSample(frameToSamples(frame));
}

void PreviewWindow::setCurrentPlaybackSample(int64_t samplePosition) {
    const int64_t sanitizedSample = qMax<int64_t>(0, samplePosition);
    const qreal framePosition = samplesToFramePosition(sanitizedSample);
    const int64_t displayFrame = qMax<int64_t>(0, static_cast<int64_t>(std::floor(framePosition)));
    const bool discontinuousFrameJump =
        m_interaction.playing && qAbs(displayFrame - m_interaction.currentFrame) > 2;
    playbackTrace(QStringLiteral("PreviewWindow::setCurrentPlaybackSample"),
                  QStringLiteral("sample=%1 frame=%2 visible=%3 cache=%4")
                      .arg(sanitizedSample)
                      .arg(framePosition, 0, 'f', 3)
                      .arg(isVisible())
                      .arg(m_cache != nullptr));
    if (discontinuousFrameJump) {
        m_lastPresentedFrames.clear();
    }
    m_interaction.currentSample = sanitizedSample;
    m_interaction.currentFramePosition = framePosition;
    m_interaction.currentFrame = displayFrame;
    syncAudioPreviewPanToPlayhead(&m_interaction);
    if (m_cache) {
        m_cache->setPlayheadFrame(displayFrame);
    }
    if (m_playbackPipeline) {
        m_playbackPipeline->setPlayheadFrame(displayFrame);
    }
    if (m_bulkUpdateDepth > 0) {
        m_pendingFrameRequest = true;
    } else if (isVisible() && m_frameRequestsArmed) {
        m_pendingFrameRequest = true;
        scheduleFrameRequest();
    } else if (isVisible()) {
        m_pendingFrameRequest = true;
    }
    if (isVisible() && !m_repaintTimer.isActive()) {
        m_repaintTimer.start();
    }
    scheduleRepaint();
}

void PreviewWindow::setClipCount(int count) { m_interaction.clipCount = count; scheduleRepaint(); }

void PreviewWindow::setSelectedClipId(const QString& clipId) {
    if (m_interaction.selectedClipId == clipId) return;
    m_interaction.selectedClipId = clipId;
    scheduleRepaint();
}

void PreviewWindow::setTimelineClips(const QVector<TimelineClip>& clips) {
    playbackTrace(QStringLiteral("PreviewWindow::setTimelineClips"),
                  QStringLiteral("clips=%1 cache=%2").arg(clips.size()).arg(m_cache != nullptr));
    m_interaction.clips = clips;
    m_audioDisplayPeakCache.clear();
    m_transcriptSectionsCache.clear();
    QSet<QString> visualClipIds;
    for (const auto& clip : clips) {
        if (clipVisualPlaybackEnabled(clip, m_interaction.tracks)) visualClipIds.insert(clip.id);
    }
    for (auto it = m_lastPresentedFrames.begin(); it != m_lastPresentedFrames.end();) {
        if (!visualClipIds.contains(it.key())) it = m_lastPresentedFrames.erase(it);
        else ++it;
    }
    if (m_playbackPipeline) m_playbackPipeline->setTimelineClips(clips);
    if (!m_cache) {
        m_registeredClips.clear();
        m_registeredClipRegistrationKeys.clear();
        scheduleRepaint();
        return;
    }

    QSet<QString> registeredIds;
    QHash<QString, QString> nextRegisteredClipRegistrationKeys;
    for (const auto& clip : clips) {
        if (!clipVisualPlaybackEnabled(clip, m_interaction.tracks)) continue;
        registeredIds.insert(clip.id);

        const QString registrationKey = cacheRegistrationKeyForClip(clip);
        nextRegisteredClipRegistrationKeys.insert(clip.id, registrationKey);

        const bool alreadyRegistered = m_registeredClips.contains(clip.id);
        const bool registrationChanged =
            m_registeredClipRegistrationKeys.value(clip.id) != registrationKey;

        if (!alreadyRegistered || registrationChanged) {
            if (alreadyRegistered) {
                m_cache->unregisterClip(clip.id);
                m_lastPresentedFrames.remove(clip.id);
            }
            m_cache->registerClip(clip);
        }
    }
    for (const QString& id : m_registeredClips) {
        if (!registeredIds.contains(id)) m_cache->unregisterClip(id);
    }
    m_registeredClips = registeredIds;
    m_registeredClipRegistrationKeys = nextRegisteredClipRegistrationKeys;

    if (m_bulkUpdateDepth > 0) m_pendingFrameRequest = true;
    else if (m_frameRequestsArmed) { m_pendingFrameRequest = true; scheduleFrameRequest(); }
    else m_pendingFrameRequest = true;
    scheduleRepaint();
}

void PreviewWindow::setTimelineTracks(const QVector<TimelineTrack>& tracks) {
    m_interaction.tracks = tracks;
    setTimelineClips(m_interaction.clips);
}

void PreviewWindow::setRenderSyncMarkers(const QVector<RenderSyncMarker>& markers) {
    m_interaction.renderSyncMarkers = markers;
    if (m_cache) m_cache->setRenderSyncMarkers(markers);
    if (m_playbackPipeline) m_playbackPipeline->setRenderSyncMarkers(markers);
    if (m_bulkUpdateDepth > 0) m_pendingFrameRequest = true;
    else if (m_frameRequestsArmed) { m_pendingFrameRequest = true; scheduleFrameRequest(); }
    else m_pendingFrameRequest = true;
    scheduleRepaint();
}

void PreviewWindow::beginBulkUpdate() { ++m_bulkUpdateDepth; }

void PreviewWindow::endBulkUpdate() {
    if (m_bulkUpdateDepth <= 0) { m_bulkUpdateDepth = 0; return; }
    --m_bulkUpdateDepth;
    if (m_bulkUpdateDepth == 0 && m_pendingFrameRequest) {
        if (isVisible() && m_frameRequestsArmed) scheduleFrameRequest();
        scheduleRepaint();
    }
}

void PreviewWindow::setExportRanges(const QVector<ExportRangeSegment>& ranges) {
    if (m_cache) m_cache->setExportRanges(ranges);
}

void PreviewWindow::setUseProxyMedia(bool useProxyMedia) {
    m_useProxyMedia = useProxyMedia;
}

void PreviewWindow::setPlaybackTuning(const PlaybackTuning& tuning)
{
    PlaybackTuning normalized;
    normalized.visibleBacklogLimit = qBound(1, tuning.visibleBacklogLimit, 16);
    normalized.sourceLookaheadFrames = qBound(1, tuning.sourceLookaheadFrames, 32);
    normalized.proxyLookaheadFrames = qBound(1, tuning.proxyLookaheadFrames, 64);
    m_playbackTuning = normalized;
}

PreviewSurface::PlaybackTuning PreviewWindow::playbackTuning() const
{
    return m_playbackTuning;
}

int PreviewWindow::effectivePlaybackLookaheadFrames() const
{
    return m_useProxyMedia ? m_playbackTuning.proxyLookaheadFrames
                           : m_playbackTuning.sourceLookaheadFrames;
}

QString PreviewWindow::backendName() const {
    if (usingCpuFallback()) {
        if (m_vulkanPreviewActive) {
            return QStringLiteral("Vulkan Preview");
        }
        if (m_forceCpuPreviewForVulkan) {
            return QStringLiteral("Vulkan Preview (Fallback Pending)");
        }
        return QStringLiteral("CPU Preview Fallback");
    }
    QString label = QStringLiteral("OpenGL Shader Preview");
    if (m_requestedRenderBackend != m_effectiveRenderBackend) {
        label += QStringLiteral(" (requested %1, using %2)")
                     .arg(m_requestedRenderBackend, m_effectiveRenderBackend);
    }
    return label;
}

void PreviewWindow::setRenderBackendPreference(const QString& backendName)
{
    const RenderBackend requestedBackend = parseRenderBackend(backendName);
    configurePreviewBackend(requestedBackend, true);
    update();
}

void PreviewWindow::setAudioMuted(bool muted) { m_interaction.audioMuted = muted; }
void PreviewWindow::setAudioVolume(qreal volume) { m_interaction.audioVolume = qBound<qreal>(0.0, volume, 1.0); }

void PreviewWindow::setOutputSize(const QSize& size) {
    const QSize sanitized(qMax(16, size.width()), qMax(16, size.height()));
    if (m_interaction.outputSize == sanitized) return;
    m_interaction.outputSize = sanitized;
    if (m_requestedRenderBackend == QStringLiteral("vulkan")) {
        ensureVulkanPreviewRenderer(false);
    }
    scheduleRepaint();
}

bool PreviewWindow::configurePreviewBackend(RenderBackend requestedBackend, bool promptOnFallback)
{
    m_requestedRenderBackend = renderBackendName(requestedBackend);
    m_effectiveRenderBackend = QStringLiteral("opengl");
    m_forceCpuPreviewForVulkan = false;
    m_vulkanPreviewActive = false;
    m_renderBackendFallbackReason.clear();
    if (requestedBackend == RenderBackend::Vulkan) {
        m_forceCpuPreviewForVulkan = true;
        if (!ensureVulkanPreviewRenderer(promptOnFallback)) {
            return false;
        }
        m_effectiveRenderBackend = QStringLiteral("vulkan");
    } else {
        m_vulkanPreviewRenderer.reset();
        m_vulkanPreviewDecoders.clear();
        m_vulkanPreviewAsyncFrameCache.clear();
    }
    return true;
}

bool PreviewWindow::ensureVulkanPreviewRenderer(bool promptOnFallback)
{
    m_vulkanPreviewRenderer.reset();
    m_vulkanPreviewDecoders.clear();
    m_vulkanPreviewAsyncFrameCache.clear();
    m_vulkanPreviewActive = false;
    m_forceCpuPreviewForVulkan = false;
    m_effectiveRenderBackend = QStringLiteral("opengl");
    m_renderBackendFallbackReason =
        QStringLiteral("Vulkan preview requires a direct Vulkan presenter; the QImage offscreen bridge is disabled.");
    ++m_renderBackendFallbackCount;
    m_lastRenderBackendFallbackMs = nowMs();

    bool allowFallback = true;
    if (promptOnFallback && !qEnvironmentVariable("QT_QPA_PLATFORM").contains("offscreen", Qt::CaseInsensitive)) {
        const QString prompt = QStringLiteral(
            "Vulkan preview initialization failed:\n\n%1\n\nFall back to OpenGL preview?")
                                   .arg(m_renderBackendFallbackReason);
        allowFallback = QMessageBox::question(this,
                                              QStringLiteral("Vulkan Preview Unavailable"),
                                              prompt,
                                              QMessageBox::Yes | QMessageBox::No,
                                              QMessageBox::Yes) == QMessageBox::Yes;
    }
    if (!allowFallback) {
        m_forceCpuPreviewForVulkan = true;
        m_vulkanPreviewActive = false;
        m_effectiveRenderBackend = QStringLiteral("vulkan");
        QCoreApplication::exit(2);
    } else if (!suppressBridgeFallbackLogs()) {
        qWarning().noquote()
            << "[render-backend-fallback] requested=vulkan effective=opengl reason=\""
            << m_renderBackendFallbackReason << "\"";
    }
    return allowFallback;
}

void PreviewWindow::setHideOutsideOutputWindow(bool hide) {
    if (m_hideOutsideOutputWindow == hide) return;
    m_hideOutsideOutputWindow = hide;
    scheduleRepaint();
}

void PreviewWindow::setBackgroundColor(const QColor& color) {
    if (m_interaction.backgroundColor == color) return;
    m_interaction.backgroundColor = color;
    scheduleRepaint();
}

void PreviewWindow::setPreviewZoom(qreal zoom) {
    // Keep a wide global bound for REST/UI control; interaction paths clamp per mode.
    m_interaction.previewZoom = qBound<qreal>(0.1, zoom, 100000.0);
    if (m_interaction.viewMode == ViewMode::Video) {
        m_interaction.previewPanOffset = PreviewViewTransform::clampedPanOffset(
            QRectF(previewCanvasBaseRect()),
            qBound<qreal>(0.1, m_interaction.previewZoom, 20.0),
            m_interaction.previewPanOffset);
    }
    scheduleRepaint();
}

void PreviewWindow::setShowSpeakerTrackPoints(bool show) {
    if (m_showSpeakerTrackPoints == show) {
        return;
    }
    m_showSpeakerTrackPoints = show;
    scheduleRepaint();
}

void PreviewWindow::setShowSpeakerTrackBoxes(bool show) {
    if (m_showSpeakerTrackBoxes == show) {
        return;
    }
    m_showSpeakerTrackBoxes = show;
    scheduleRepaint();
}

void PreviewWindow::setShowRawDetections(bool show) {
    if (m_showRawDetections == show) {
        return;
    }
    m_showRawDetections = show;
    m_rawDetectionPointsCache.clear();
    scheduleRepaint();
}

void PreviewWindow::setShowCurrentSpeakerName(bool show) {
    if (m_interaction.showCurrentSpeakerName == show) {
        return;
    }
    m_interaction.showCurrentSpeakerName = show;
    scheduleRepaint();
}

void PreviewWindow::setShowCurrentSpeakerOrganization(bool show) {
    if (m_interaction.showCurrentSpeakerOrganization == show) {
        return;
    }
    m_interaction.showCurrentSpeakerOrganization = show;
    scheduleRepaint();
}

void PreviewWindow::setCurrentSpeakerNameTextScale(qreal scale) {
    const qreal normalized = qBound<qreal>(0.25, scale, 3.0);
    if (qFuzzyCompare(m_interaction.currentSpeakerNameTextScale, normalized)) {
        return;
    }
    m_interaction.currentSpeakerNameTextScale = normalized;
    scheduleRepaint();
}

void PreviewWindow::setCurrentSpeakerOrganizationTextScale(qreal scale) {
    const qreal normalized = qBound<qreal>(0.25, scale, 3.0);
    if (qFuzzyCompare(m_interaction.currentSpeakerOrganizationTextScale, normalized)) {
        return;
    }
    m_interaction.currentSpeakerOrganizationTextScale = normalized;
    scheduleRepaint();
}

void PreviewWindow::setCurrentSpeakerNameVerticalPosition(qreal position) {
    const qreal normalized = qBound<qreal>(0.0, position, 1.0);
    if (qFuzzyCompare(m_interaction.currentSpeakerNameVerticalPosition, normalized)) {
        return;
    }
    m_interaction.currentSpeakerNameVerticalPosition = normalized;
    scheduleRepaint();
}

void PreviewWindow::setCurrentSpeakerOrganizationVerticalPosition(qreal position) {
    const qreal normalized = qBound<qreal>(0.0, position, 1.0);
    if (qFuzzyCompare(m_interaction.currentSpeakerOrganizationVerticalPosition, normalized)) {
        return;
    }
    m_interaction.currentSpeakerOrganizationVerticalPosition = normalized;
    scheduleRepaint();
}

void PreviewWindow::setPlaybackStatusOverlayText(const QString& text) {
    const QString normalized = text.trimmed();
    if (m_interaction.playbackStatusOverlayText == normalized) {
        return;
    }
    m_interaction.playbackStatusOverlayText = normalized;
    scheduleRepaint();
}

void PreviewWindow::setPlaybackStatusOverlayProgress(qreal progress) {
    const qreal normalized = progress < 0.0 ? -1.0 : qBound<qreal>(0.0, progress, 1.0);
    if (qFuzzyCompare(m_interaction.playbackStatusOverlayProgress + 1.0, normalized + 1.0)) {
        return;
    }
    m_interaction.playbackStatusOverlayProgress = normalized;
    scheduleRepaint();
}

void PreviewWindow::setFacestreamOverlaySource(const QString& source) {
    const QString normalized = source.trimmed().isEmpty()
        ? QStringLiteral("all")
        : source.trimmed().toLower();
    if (m_facedetectionsOverlaySource == normalized) {
        return;
    }
    m_facedetectionsOverlaySource = normalized;
    m_speakerTrackPointsCache.clear();
    scheduleRepaint();
}

void PreviewWindow::setAudioSpeakerHoverModalEnabled(bool enabled) {
    if (m_audioSpeakerHoverModalEnabled == enabled) {
        return;
    }
    m_audioSpeakerHoverModalEnabled = enabled;
    scheduleRepaint();
}

void PreviewWindow::setAudioWaveformVisible(bool visible) {
    if (m_audioWaveformVisible == visible) {
        return;
    }
    m_audioWaveformVisible = visible;
    scheduleRepaint();
}

void PreviewWindow::setViewMode(ViewMode mode) {
    if (m_interaction.viewMode == mode) {
        return;
    }
    m_interaction.viewMode = mode;
    if (m_interaction.viewMode != ViewMode::Audio) {
        m_interaction.previewZoom = qBound<qreal>(0.1, m_interaction.previewZoom, 20.0);
    }
    scheduleRepaint();
}

void PreviewWindow::setAudioDynamicsSettings(const AudioDynamicsSettings& settings) {
    m_audioDynamics = settings;
    m_audioDisplayPeakCache.clear();
    scheduleRepaint();
}

void PreviewWindow::setTranscriptOverlayInteractionEnabled(bool enabled) {
    if (m_interaction.transcriptOverlayInteractionEnabled == enabled) {
        return;
    }
    m_interaction.transcriptOverlayInteractionEnabled = enabled;
    if (!enabled && m_interaction.transient.dragMode != PreviewDragMode::None) {
        const PreviewOverlayInfo selectedInfo = m_overlayModel.overlays.value(m_interaction.selectedClipId);
        if (selectedInfo.kind == PreviewOverlayKind::TranscriptOverlay) {
            m_interaction.transient.dragMode = PreviewDragMode::None;
            m_interaction.transient.dragOriginBounds = QRectF();
        }
    }
    scheduleRepaint();
}

void PreviewWindow::setTitleOverlayInteractionOnly(bool enabled) {
    if (m_interaction.titleOverlayInteractionOnly == enabled) {
        return;
    }
    m_interaction.titleOverlayInteractionOnly = enabled;
    if (m_interaction.titleOverlayInteractionOnly && !clipIdIsTitle(m_interaction.selectedClipId)) {
        m_interaction.transient.dragMode = PreviewDragMode::None;
        m_interaction.transient.dragOriginBounds = QRectF();
    }
    scheduleRepaint();
}

void PreviewWindow::setFaceDetectionsAssignmentInteractionEnabled(bool enabled) {
    if (m_interaction.faceStreamAssignmentInteractionEnabled == enabled) {
        return;
    }
    m_interaction.faceStreamAssignmentInteractionEnabled = enabled;
    clearHoveredFaceDetectionsBox();
    if (m_interaction.faceStreamAssignmentInteractionEnabled) {
        m_interaction.transient.dragMode = PreviewDragMode::None;
        m_interaction.transient.dragOriginBounds = QRectF();
        unsetCursor();
    }
    scheduleRepaint();
}

void PreviewWindow::setBypassGrading(bool bypass) {
    if (m_bypassGrading == bypass) return;
    m_bypassGrading = bypass;
    scheduleRepaint();
}

void PreviewWindow::setCorrectionsEnabled(bool enabled) {
    if (m_correctionsEnabled == enabled) {
        return;
    }
    m_correctionsEnabled = enabled;
    scheduleRepaint();
}

bool PreviewWindow::bypassGrading() const { return m_bypassGrading; }
bool PreviewWindow::audioMuted() const { return m_interaction.audioMuted; }
int PreviewWindow::audioVolumePercent() const { return qRound(m_interaction.audioVolume * 100.0); }

QString PreviewWindow::activeAudioClipLabel() const {
    for (const TimelineClip& clip : m_interaction.clips) {
        if (clipAudioPlaybackEnabled(clip) && isSampleWithinClip(clip, m_interaction.currentSample)) {
            return clip.label;
        }
    }
    return QString();
}

QImage PreviewWindow::latestPresentedFrameImageForClip(const QString& clipId) const
{
    if (clipId.isEmpty()) {
        return QImage();
    }

    const FrameHandle presented = m_lastPresentedFrames.value(clipId);
    if (!presented.isNull() && presented.hasCpuImage()) {
        return presented.cpuImage();
    }

    if (!m_cache) {
        return QImage();
    }

    for (const TimelineClip& clip : m_interaction.clips) {
        if (clip.id != clipId || clip.durationFrames <= 0) {
            continue;
        }
        const int64_t localFrame = qBound<int64_t>(
            0,
            static_cast<int64_t>(std::floor(m_interaction.currentFramePosition)) - clip.startFrame,
            qMax<int64_t>(0, clip.durationFrames - 1));
        const FrameHandle cached = m_cache->getCachedFrame(clip.id, localFrame);
        if (!cached.isNull() && cached.hasCpuImage()) {
            return cached.cpuImage();
        }
        break;
    }

    return QImage();
}

QVector<PreviewSurface::PipelineStageSnapshot> PreviewWindow::livePipelineSnapshots() const
{
    QVector<PipelineStageSnapshot> snapshots;
    QImage previewImage;
    if (m_glInitialized && isValid()) {
        previewImage = const_cast<PreviewWindow*>(this)->grabFramebuffer();
    }
    auto addStage = [&snapshots](const QString& label,
                                 const QString& detail,
                                 const QImage& image,
                                 const QString& kind,
                                 bool exact,
                                 bool active) {
        snapshots.push_back(PipelineStageSnapshot{label, detail, image, kind, exact, active});
    };

    addStage(QStringLiteral("00 Preview State"),
             QStringLiteral("%1 | timeline frame %2 | sample %3 | clips %4")
                 .arg(backendName())
                 .arg(m_interaction.currentFrame)
                 .arg(m_interaction.currentSample)
                 .arg(m_interaction.clipCount),
             previewImage,
             QStringLiteral("surface"),
             true,
             true);

    QHash<QString, QJsonObject> selectionByClip;
    const QJsonArray selections = m_lastFrameSelectionStats.value(QStringLiteral("clips")).toArray();
    for (const QJsonValue& value : selections) {
        const QJsonObject object = value.toObject();
        const QString id = object.value(QStringLiteral("id")).toString();
        if (!id.isEmpty()) {
            selectionByClip.insert(id, object);
        }
    }

    for (const TimelineClip& clip : m_interaction.clips) {
        if (!clipVisualPlaybackEnabled(clip, m_interaction.tracks) ||
            clip.mediaType == ClipMediaType::Title ||
            !isSampleWithinClip(clip, m_interaction.currentSample)) {
            continue;
        }

        const int64_t localFrame = sourceFrameForSample(clip, m_interaction.currentSample);
        const bool usePlaybackPipeline =
            m_interaction.playing &&
            clip.sourceKind == MediaSourceKind::ImageSequence &&
            clip.mediaType != ClipMediaType::Image;
        FrameHandle frame = m_lastPresentedFrames.value(clip.id);
        if ((frame.isNull() || !frame.hasCpuImage()) && m_cache) {
            const FrameHandle exact = m_cache->getCachedFrame(clip.id, localFrame);
            if (!exact.isNull()) {
                frame = exact;
            } else {
                frame = m_interaction.playing ? m_cache->getLatestCachedFrame(clip.id, localFrame)
                                              : m_cache->getBestCachedFrame(clip.id, localFrame);
            }
        }

        const QJsonObject selection = selectionByClip.value(clip.id);
        const QString selectionText =
            selection.value(QStringLiteral("selection")).toString(frame.isNull() ? QStringLiteral("missing")
                                                                                 : QStringLiteral("live"));
        const QString storageText =
            frame.hasHardwareFrame() ? QStringLiteral("hardware")
                                     : (frame.hasGpuTexture() ? QStringLiteral("gpu")
                                                              : (frame.hasCpuImage() ? QStringLiteral("cpu")
                                                                                     : QStringLiteral("none")));
        const QString clipLabel = clip.label.isEmpty() ? clip.id : clip.label;
        const EffectiveVisualEffects effects = evaluateEffectiveVisualEffectsAtPosition(
            clip, m_interaction.tracks, m_interaction.currentFramePosition, m_interaction.renderSyncMarkers);
        const TimelineClip::GradingKeyframe grade =
            m_bypassGrading ? TimelineClip::GradingKeyframe{} : effects.grading;
        const TimelineClip::TransformKeyframe transform =
            evaluateClipRenderTransformAtPosition(clip, m_interaction.currentFramePosition, m_interaction.outputSize);
        const QVector<QPointF> identityCurve = defaultGradingCurvePoints();
        const bool curveLutActive =
            effects.grading.curvePointsR != identityCurve ||
            effects.grading.curvePointsG != identityCurve ||
            effects.grading.curvePointsB != identityCurve ||
            effects.grading.curvePointsLuma != identityCurve;

        addStage(QStringLiteral("01 Timeline Input"),
                 QStringLiteral("%1 | %2 | %3 | timeline %4-%5")
                     .arg(clipLabel,
                          clipMediaTypeLabel(clip.mediaType),
                          mediaSourceKindLabel(clip.sourceKind))
                     .arg(clip.startFrame)
                     .arg(clip.startFrame + qMax<int64_t>(0, clip.durationFrames - 1)),
                 QImage(),
                 QStringLiteral("timeline"),
                 true,
                 true);
        addStage(QStringLiteral("02 Source Mapping"),
                 QStringLiteral("sample %1 -> source frame %2 | source in %3 | playback pipeline %4")
                     .arg(m_interaction.currentSample)
                     .arg(localFrame)
                     .arg(clip.sourceInFrame)
                     .arg(usePlaybackPipeline ? QStringLiteral("yes") : QStringLiteral("no")),
                 QImage(),
                 QStringLiteral("mapping"),
                 true,
                 true);
        addStage(QStringLiteral("03 Decoder Output"),
                 QStringLiteral("presented frame %1 | storage %2 | size %3x%4")
                     .arg(frame.frameNumber())
                     .arg(storageText)
                     .arg(frame.size().width())
                     .arg(frame.size().height()),
                 frame.hasCpuImage() ? frame.cpuImage() : QImage(),
                 QStringLiteral("decoder"),
                 !frame.isNull() && frame.frameNumber() == localFrame,
                 !frame.isNull());
        addStage(QStringLiteral("04 Frame Selection"),
                 QStringLiteral("%1 | exact %2 | stale rejected %3")
                     .arg(selectionText)
                     .arg(!frame.isNull() && frame.frameNumber() == localFrame ? QStringLiteral("yes") : QStringLiteral("no"))
                     .arg(isFrameTooStaleForPlayback(clip, localFrame, frame) ? QStringLiteral("yes") : QStringLiteral("no")),
                 frame.hasCpuImage() ? frame.cpuImage() : QImage(),
                 QStringLiteral("selection"),
                 !frame.isNull() && frame.frameNumber() == localFrame,
                 !frame.isNull());
        addStage(QStringLiteral("05 Corrections / Mask"),
                 QStringLiteral("polygons %1 | enabled %2 | mask feather %3 gamma %4")
                     .arg(effects.correctionPolygons.size())
                     .arg(m_correctionsEnabled ? QStringLiteral("yes") : QStringLiteral("no"))
                     .arg(effects.maskFeather)
                     .arg(effects.maskFeatherGamma),
                 QImage(),
                 QStringLiteral("mask"),
                 true,
                 true);
        addStage(QStringLiteral("06 Effects Evaluation"),
                 QStringLiteral("opacity %1 | bypass grading %2 | curve LUT %3")
                     .arg(grade.opacity)
                     .arg(m_bypassGrading ? QStringLiteral("yes") : QStringLiteral("no"))
                     .arg(curveLutActive ? QStringLiteral("yes") : QStringLiteral("no")),
                 QImage(),
                 QStringLiteral("effects"),
                 true,
                 true);
        addStage(QStringLiteral("07 Grading Shader Output"),
                 QStringLiteral("brightness %1 | contrast %2 | saturation %3 | H/M/S %4,%5,%6")
                     .arg(grade.brightness)
                     .arg(grade.contrast)
                     .arg(grade.saturation)
                     .arg(grade.highlightsR)
                     .arg(grade.midtonesR)
                     .arg(grade.shadowsR),
                 previewImage,
                 QStringLiteral("shader"),
                 !frame.isNull() && frame.frameNumber() == localFrame,
                 !frame.isNull());
        addStage(QStringLiteral("08 Transform"),
                 QStringLiteral("translate %1,%2 | scale %3,%4 | rotate %5")
                     .arg(transform.translationX)
                     .arg(transform.translationY)
                     .arg(transform.scaleX)
                     .arg(transform.scaleY)
                     .arg(transform.rotation),
                 QImage(),
                 QStringLiteral("transform"),
                 true,
                 true);
        addStage(QStringLiteral("09 Composite Layer"),
                 QStringLiteral("OpenGL visual shader | output %1x%2 | selection %3")
                     .arg(m_interaction.outputSize.width())
                     .arg(m_interaction.outputSize.height())
                     .arg(selectionText),
                 previewImage,
                 QStringLiteral("composite"),
                 !frame.isNull() && frame.frameNumber() == localFrame,
                 !frame.isNull());
    }

    if (snapshots.size() == 1 && !m_interaction.selectedClipId.isEmpty()) {
        const QImage selectedImage = latestPresentedFrameImageForClip(m_interaction.selectedClipId);
        snapshots.push_back(PipelineStageSnapshot{
            QStringLiteral("Selected Clip"),
            selectedImage.isNull() ? QStringLiteral("No CPU image in the live preview cache")
                                   : QStringLiteral("Latest live CPU image for selected clip"),
            selectedImage,
            QStringLiteral("decoder"),
            false,
            !selectedImage.isNull()});
    }

    addStage(QStringLiteral("10 Presented Surface"),
             QStringLiteral("widget %1x%2 | final preview surface")
                 .arg(width())
                 .arg(height()),
             previewImage,
             QStringLiteral("surface"),
             true,
             !previewImage.isNull());

    return snapshots;
}

bool PreviewWindow::clipIdIsTitle(const QString& clipId) const {
    if (clipId.isEmpty()) {
        return false;
    }
    for (const TimelineClip& clip : m_interaction.clips) {
        if (clip.id == clipId) {
            return clip.mediaType == ClipMediaType::Title;
        }
    }
    return false;
}

QList<TimelineClip> PreviewWindow::getActiveClips() const {
    QList<TimelineClip> active;
    for (const TimelineClip& clip : m_interaction.clips) {
        if (isSampleWithinClip(clip, m_interaction.currentSample)) active.push_back(clip);
    }
    std::sort(active.begin(), active.end(), [](const TimelineClip& a, const TimelineClip& b) {
        if (a.trackIndex == b.trackIndex) return a.startFrame < b.startFrame;
        return a.trackIndex > b.trackIndex;
    });
    return active;
}

QJsonObject PreviewWindow::profilingSnapshot() const {
    const CurrentSpeakerLabel currentSpeakerLabel = currentSpeakerLabelForState(&m_interaction);
    const qint64 now = nowMs();
    QJsonObject snapshot{{QStringLiteral("backend"), backendName()},
                         {QStringLiteral("playing"), m_interaction.playing},
                         {QStringLiteral("current_frame"), static_cast<qint64>(m_interaction.currentFrame)},
                         {QStringLiteral("current_sample"), static_cast<qint64>(m_interaction.currentSample)},
                         {QStringLiteral("show_current_speaker_name"), m_interaction.showCurrentSpeakerName},
                         {QStringLiteral("show_current_speaker_organization"), m_interaction.showCurrentSpeakerOrganization},
                         {QStringLiteral("current_speaker_name_text_scale"), m_interaction.currentSpeakerNameTextScale},
                         {QStringLiteral("current_speaker_organization_text_scale"),
                          m_interaction.currentSpeakerOrganizationTextScale},
                         {QStringLiteral("current_speaker_name_y_position"),
                          m_interaction.currentSpeakerNameVerticalPosition},
                         {QStringLiteral("current_speaker_organization_y_position"),
                          m_interaction.currentSpeakerOrganizationVerticalPosition},
                         {QStringLiteral("playback_status_overlay_text"),
                          m_interaction.playbackStatusOverlayText},
                         {QStringLiteral("current_speaker_label"), QJsonObject{
                             {QStringLiteral("speaker_id"), currentSpeakerLabel.speakerId},
                             {QStringLiteral("name"), currentSpeakerLabel.name},
                             {QStringLiteral("organization"), currentSpeakerLabel.organization},
                             {QStringLiteral("has_name"), !currentSpeakerLabel.name.trimmed().isEmpty()},
                             {QStringLiteral("has_organization"), !currentSpeakerLabel.organization.trimmed().isEmpty()}
                         }},
                         {QStringLiteral("current_speaker_label_debug"), currentSpeakerLabelDebugForState(&m_interaction)},
                         {QStringLiteral("clip_count"), m_interaction.clips.size()},
                         {QStringLiteral("pipeline_initialized"), m_cache != nullptr},
                         {QStringLiteral("repaint_strategy"), QStringLiteral("direct_update")},
                         {QStringLiteral("last_frame_request_ms"), m_lastFrameRequestMs},
                         {QStringLiteral("last_frame_ready_ms"), m_lastFrameReadyMs},
                         {QStringLiteral("last_paint_ms"), m_lastPaintMs},
                         {QStringLiteral("last_repaint_schedule_ms"), m_lastRepaintScheduleMs},
                         {QStringLiteral("last_frame_request_age_ms"), m_lastFrameRequestMs > 0 ? now - m_lastFrameRequestMs : -1},
                         {QStringLiteral("last_frame_ready_age_ms"), m_lastFrameReadyMs > 0 ? now - m_lastFrameReadyMs : -1},
                         {QStringLiteral("last_paint_age_ms"), m_lastPaintMs > 0 ? now - m_lastPaintMs : -1},
                         {QStringLiteral("last_repaint_schedule_age_ms"), m_lastRepaintScheduleMs > 0 ? now - m_lastRepaintScheduleMs : -1},
                         {QStringLiteral("repaint_timer_active"), m_repaintTimer.isActive()},
                         {QStringLiteral("frame_request_timer_active"), m_frameRequestTimer.isActive()},
                         {QStringLiteral("frame_requests_armed"), m_frameRequestsArmed},
                         {QStringLiteral("pending_frame_request"), m_pendingFrameRequest},
                         {QStringLiteral("widget_visible"), isVisible()},
                         {QStringLiteral("updates_enabled"), updatesEnabled()},
                         {QStringLiteral("bypass_grading"), m_bypassGrading}};
    snapshot[QStringLiteral("render_backend")] = QJsonObject{
        {QStringLiteral("requested"), m_requestedRenderBackend},
        {QStringLiteral("effective"), m_effectiveRenderBackend},
        {QStringLiteral("vulkan_preview_active"), m_vulkanPreviewActive},
        {QStringLiteral("fallback_reason"), m_renderBackendFallbackReason},
        {QStringLiteral("fallback_count"), m_renderBackendFallbackCount},
        {QStringLiteral("last_fallback_ms"), m_lastRenderBackendFallbackMs},
        {QStringLiteral("vulkan_compose_success_count"), m_vulkanComposeSuccessCount},
        {QStringLiteral("vulkan_compose_failure_count"), m_vulkanComposeFailureCount},
        {QStringLiteral("last_vulkan_compose_success_ms"), m_lastVulkanComposeSuccessMs},
        {QStringLiteral("last_vulkan_compose_failure_ms"), m_lastVulkanComposeFailureMs}
    };

    // Render timing statistics
    QJsonObject renderTiming;
    renderTiming[QStringLiteral("last_ms")] = static_cast<qint64>(m_lastRenderDurationMs);
    renderTiming[QStringLiteral("max_ms")] = static_cast<qint64>(m_maxRenderDurationMs);
    renderTiming[QStringLiteral("count")] = static_cast<qint64>(m_renderCount);
    renderTiming[QStringLiteral("avg_ms")] = m_renderCount > 0 
        ? static_cast<double>(m_totalRenderDurationMs) / static_cast<double>(m_renderCount) 
        : 0.0;
    renderTiming[QStringLiteral("target_ms")] = m_interaction.playing ? 33.33 : 16.67; // 30fps vs 60fps target
    renderTiming[QStringLiteral("slow_frame")] = m_lastRenderDurationMs > (m_interaction.playing ? 33 : 16);
    
    // Calculate 95th percentile
    if (!m_renderTimeHistory.empty()) {
        std::vector<qint64> sorted(m_renderTimeHistory.begin(), m_renderTimeHistory.end());
        std::sort(sorted.begin(), sorted.end());
        const size_t p95Index = static_cast<size_t>(sorted.size() * 0.95);
        renderTiming[QStringLiteral("p95_ms")] = static_cast<qint64>(sorted[p95Index]);
    }
    
    QJsonArray history;
    for (qint64 t : m_renderTimeHistory) {
        history.append(static_cast<qint64>(t));
    }
    renderTiming[QStringLiteral("history_ms")] = history;
    
    snapshot[QStringLiteral("render_timing")] = renderTiming;

    if (m_decoder) {
        snapshot[QStringLiteral("decoder")] = QJsonObject{{QStringLiteral("worker_count"), m_decoder->workerCount()},
                                                           {QStringLiteral("pending_requests"), m_decoder->pendingRequestCount()}};
        if (MemoryBudget* budget = m_decoder->memoryBudget()) {
            snapshot[QStringLiteral("memory_budget")] = QJsonObject{{QStringLiteral("cpu_usage"), static_cast<qint64>(budget->currentCpuUsage())},
                                                                     {QStringLiteral("gpu_usage"), static_cast<qint64>(budget->currentGpuUsage())},
                                                                     {QStringLiteral("cpu_pressure"), budget->cpuPressure()},
                                                                     {QStringLiteral("gpu_pressure"), budget->gpuPressure()},
                                                                     {QStringLiteral("cpu_max"), static_cast<qint64>(budget->maxCpuMemory())},
                                                                     {QStringLiteral("gpu_max"), static_cast<qint64>(budget->maxGpuMemory())}};
        }
    }

    if (m_cache) {
        QJsonObject cacheSnapshot{{QStringLiteral("hit_rate"), m_cache->cacheHitRate()},
                                  {QStringLiteral("total_memory_usage"), static_cast<qint64>(m_cache->totalMemoryUsage())},
                                  {QStringLiteral("total_cached_frames"), m_cache->totalCachedFrames()},
                                  {QStringLiteral("pending_visible_requests"), m_cache->pendingVisibleRequestCount()},
                                  {QStringLiteral("pending_visible_debug"), m_cache->pendingVisibleDebugSnapshot(now)},
                                  {QStringLiteral("visible_decode"), m_cache->visibleDecodeDiagnostics(now)}};
        const QJsonObject residency = m_cache->cacheResidencySnapshot();
        for (auto it = residency.begin(); it != residency.end(); ++it) {
            cacheSnapshot.insert(it.key(), it.value());
        }
        snapshot[QStringLiteral("cache")] = cacheSnapshot;
        snapshot[QStringLiteral("visible_decode_diagnostics")] = m_cache->visibleDecodeDiagnostics(now);
    }

    if (m_playbackPipeline) {
        snapshot[QStringLiteral("playback_pipeline")] = QJsonObject{{QStringLiteral("active"), m_interaction.playing},
                                                                     {QStringLiteral("buffered_frames"), m_playbackPipeline->bufferedFrameCount()},
                                                                     {QStringLiteral("pending_visible_requests"), m_playbackPipeline->pendingVisibleRequestCount()},
                                                                     {QStringLiteral("dropped_presentation_frames"), m_playbackPipeline->droppedPresentationFrameCount()},
                                                                     {QStringLiteral("decode"), m_playbackPipeline->decodeDiagnostics()}};
    }

    if (!m_lastFrameSelectionStats.isEmpty()) {
        // Keep REST profiling snapshots UI-thread cheap. Per-clip metadata
        // enrichment can touch decoder state and cause the control server's
        // UI-thread profile callback to time out during active playback.
        QJsonObject frameSelection = m_lastFrameSelectionStats;
        frameSelection[QStringLiteral("metadata_enriched")] = false;
        snapshot[QStringLiteral("frame_selection")] = frameSelection;
    }

    snapshot[QStringLiteral("playback_smoothness")] = playbackSmoothnessSnapshot();
    snapshot[QStringLiteral("preview_visible_backlog_limit")] = m_playbackTuning.visibleBacklogLimit;
    snapshot[QStringLiteral("preview_source_lookahead_frames")] = m_playbackTuning.sourceLookaheadFrames;
    snapshot[QStringLiteral("preview_proxy_lookahead_frames")] = m_playbackTuning.proxyLookaheadFrames;
    snapshot[QStringLiteral("preview_effective_lookahead_frames")] = effectivePlaybackLookaheadFrames();

    return snapshot;
}

QJsonObject PreviewWindow::pipelineHealthSnapshot() const {
    QJsonObject snapshot{
        {QStringLiteral("ok"), true},
        {QStringLiteral("backend"), backendName()},
        {QStringLiteral("playing"), m_interaction.playing},
        {QStringLiteral("current_frame"), static_cast<qint64>(m_interaction.currentFrame)},
        {QStringLiteral("current_sample"), static_cast<qint64>(m_interaction.currentSample)},
        {QStringLiteral("clip_count"), m_interaction.clips.size()},
        {QStringLiteral("pipeline_initialized"), m_cache != nullptr},
        {QStringLiteral("widget_visible"), isVisible()},
        {QStringLiteral("updates_enabled"), updatesEnabled()},
        {QStringLiteral("pipeline_stages"), QJsonArray{}}
    };
    if (m_decoder) {
        snapshot.insert(QStringLiteral("decoder_worker_count"), m_decoder->workerCount());
        snapshot.insert(QStringLiteral("decoder_pending_requests"), m_decoder->pendingRequestCount());
    }
    if (m_cache) {
        const qint64 now = nowMs();
        snapshot.insert(QStringLiteral("cache_pending_visible_requests"), m_cache->pendingVisibleRequestCount());
        snapshot.insert(QStringLiteral("visible_decode_diagnostics"), m_cache->visibleDecodeDiagnostics(now));
    }
    if (m_playbackPipeline) {
        snapshot.insert(QStringLiteral("playback_pipeline"), QJsonObject{
            {QStringLiteral("active"), m_interaction.playing},
            {QStringLiteral("buffered_frames"), m_playbackPipeline->bufferedFrameCount()},
            {QStringLiteral("pending_visible_requests"), m_playbackPipeline->pendingVisibleRequestCount()},
            {QStringLiteral("dropped_presentation_frames"), m_playbackPipeline->droppedPresentationFrameCount()}
        });
    }
    snapshot.insert(QStringLiteral("playback_smoothness"), playbackSmoothnessSnapshot());
    return snapshot;
}

void PreviewWindow::resetProfilingStats() {
    m_maxRenderDurationMs = 0;
    m_renderCount = 0;
    m_totalRenderDurationMs = 0;
    m_renderTimeHistory.clear();
    m_playbackSmoothnessSamples.clear();
}

void PreviewWindow::recordPlaybackSmoothnessSample(const QJsonObject& frameSelectionStats)
{
    const qint64 now = nowMs();
    PlaybackSmoothnessSample sample;
    sample.timestampMs = now;
    sample.playing = m_interaction.playing;
    sample.presentationCount = frameSelectionStats.value(QStringLiteral("presentation")).toInt(0);
    sample.exactCount = frameSelectionStats.value(QStringLiteral("exact")).toInt(0);
    sample.bestCount = frameSelectionStats.value(QStringLiteral("best")).toInt(0);
    sample.heldCount = frameSelectionStats.value(QStringLiteral("held")).toInt(0);
    sample.staleRejectedCount = frameSelectionStats.value(QStringLiteral("stale_rejected")).toInt(0);
    sample.nullCount = frameSelectionStats.value(QStringLiteral("null")).toInt(0);
    sample.renderDurationMs = m_lastRenderDurationMs;
    sample.droppedPresentationFrames =
        m_playbackPipeline ? m_playbackPipeline->droppedPresentationFrameCount() : 0;

    int64_t maxFrameLag = 0;
    const QJsonArray clips = frameSelectionStats.value(QStringLiteral("clips")).toArray();
    for (const QJsonValue& value : clips) {
        const QJsonObject clip = value.toObject();
        const int64_t localFrame = clip.value(QStringLiteral("local_frame")).toInteger(-1);
        const int64_t frameNumber = clip.value(QStringLiteral("frame_number")).toInteger(-1);
        if (localFrame >= 0 && frameNumber >= 0) {
            maxFrameLag = qMax(maxFrameLag, qAbs(localFrame - frameNumber));
        }
    }
    sample.maxFrameLag = maxFrameLag;

    m_playbackSmoothnessSamples.push_back(sample);
    constexpr qint64 kWindowMs = 5000;
    constexpr size_t kMaxSamples = 240;
    while (!m_playbackSmoothnessSamples.empty() &&
           (m_playbackSmoothnessSamples.front().timestampMs < now - kWindowMs ||
            m_playbackSmoothnessSamples.size() > kMaxSamples)) {
        m_playbackSmoothnessSamples.pop_front();
    }
}

QJsonObject PreviewWindow::playbackSmoothnessSnapshot() const
{
    constexpr qint64 kWindowMs = 5000;
    const qint64 now = nowMs();
    QVector<PlaybackSmoothnessSample> window;
    window.reserve(static_cast<int>(m_playbackSmoothnessSamples.size()));
    for (const PlaybackSmoothnessSample& sample : m_playbackSmoothnessSamples) {
        if (sample.timestampMs >= now - kWindowMs) {
            window.push_back(sample);
        }
    }

    QJsonObject smoothness{
        {QStringLiteral("window_ms"), kWindowMs},
        {QStringLiteral("sample_count"), window.size()},
        {QStringLiteral("playing"), m_interaction.playing},
        {QStringLiteral("playing_sample_count"), 0},
        {QStringLiteral("exact_hit_rate"), 0.0},
        {QStringLiteral("approximate_hit_rate"), 0.0},
        {QStringLiteral("missing_frame_rate"), 0.0},
        {QStringLiteral("late_sample_rate"), 0.0},
        {QStringLiteral("avg_frame_lag"), 0.0},
        {QStringLiteral("max_frame_lag"), 0},
        {QStringLiteral("avg_handoff_upload_ms"), 0.0},
        {QStringLiteral("p95_handoff_upload_ms"), 0.0},
        {QStringLiteral("max_handoff_upload_ms"), 0.0},
        {QStringLiteral("visible_request_attempt_rate"), 0.0},
        {QStringLiteral("visible_request_dispatch_rate"), 0.0},
        {QStringLiteral("visible_request_block_rate"), 0.0},
        {QStringLiteral("visible_request_blocked_fraction"), 0.0},
        {QStringLiteral("handoff_success_rate"), 1.0},
        {QStringLiteral("presented_fps_estimate"), 0.0},
        {QStringLiteral("current_decoder_pending_requests"),
         m_decoder ? m_decoder->pendingRequestCount() : 0},
        {QStringLiteral("current_decoder_worker_count"),
         m_decoder ? m_decoder->workerCount() : 0},
        {QStringLiteral("current_last_handoff_upload_ms"), 0.0},
        {QStringLiteral("current_visible_request_backlog"),
         m_cache ? m_cache->pendingVisibleRequestCount() : 0},
        {QStringLiteral("current_last_visible_request_block_reason"), QString()},
        {QStringLiteral("dropped_presentation_frames"),
         m_playbackPipeline ? m_playbackPipeline->droppedPresentationFrameCount() : 0},
        {QStringLiteral("upload_metrics_available"), false}
    };

    if (window.isEmpty()) {
        return smoothness;
    }

    int playingSampleCount = 0;
    qint64 presentationTotal = 0;
    qint64 exactTotal = 0;
    qint64 approxTotal = 0;
    qint64 missingTotal = 0;
    qint64 lateSamples = 0;
    qint64 frameLagTotal = 0;
    qint64 maxFrameLag = 0;
    QVector<double> renderSamples;
    renderSamples.reserve(window.size());

    for (const PlaybackSmoothnessSample& sample : window) {
        if (sample.playing) {
            ++playingSampleCount;
        }
        presentationTotal += sample.presentationCount;
        exactTotal += sample.exactCount;
        approxTotal += sample.bestCount + sample.heldCount;
        missingTotal += sample.nullCount;
        frameLagTotal += sample.maxFrameLag;
        maxFrameLag = qMax(maxFrameLag, sample.maxFrameLag);
        if (sample.maxFrameLag > 0 || sample.staleRejectedCount > 0) {
            ++lateSamples;
        }
        if (sample.renderDurationMs > 0) {
            renderSamples.push_back(static_cast<double>(sample.renderDurationMs));
        }
    }

    smoothness[QStringLiteral("playing_sample_count")] = playingSampleCount;
    const qint64 totalOutcomes = exactTotal + approxTotal + missingTotal;
    if (totalOutcomes > 0) {
        smoothness[QStringLiteral("exact_hit_rate")] =
            static_cast<double>(exactTotal) / static_cast<double>(totalOutcomes);
        smoothness[QStringLiteral("approximate_hit_rate")] =
            static_cast<double>(approxTotal) / static_cast<double>(totalOutcomes);
        smoothness[QStringLiteral("missing_frame_rate")] =
            static_cast<double>(missingTotal) / static_cast<double>(totalOutcomes);
    }
    if (presentationTotal > 0) {
        smoothness[QStringLiteral("avg_frame_lag")] =
            static_cast<double>(frameLagTotal) / static_cast<double>(presentationTotal);
    }
    smoothness[QStringLiteral("max_frame_lag")] = maxFrameLag;
    smoothness[QStringLiteral("late_sample_rate")] =
        static_cast<double>(lateSamples) / static_cast<double>(window.size());

    if (!renderSamples.isEmpty()) {
        std::sort(renderSamples.begin(), renderSamples.end());
        const double renderSum = std::accumulate(renderSamples.begin(), renderSamples.end(), 0.0);
        const int p95Index = qBound(0,
                                    static_cast<int>(std::ceil(renderSamples.size() * 0.95)) - 1,
                                    renderSamples.size() - 1);
        smoothness[QStringLiteral("render_avg_ms")] =
            renderSum / static_cast<double>(renderSamples.size());
        smoothness[QStringLiteral("render_p95_ms")] = renderSamples.at(p95Index);
        smoothness[QStringLiteral("render_max_ms")] = renderSamples.constLast();
    }

    const PlaybackSmoothnessSample& first = window.constFirst();
    const PlaybackSmoothnessSample& last = window.constLast();
    const qint64 elapsedMs = qMax<qint64>(1, last.timestampMs - first.timestampMs);
    const double elapsedSeconds = static_cast<double>(elapsedMs) / 1000.0;
    const qint64 presentedDelta = qMax<qint64>(0, last.presentationCount - first.presentationCount);
    smoothness[QStringLiteral("presented_fps_estimate")] =
        static_cast<double>(presentedDelta) / elapsedSeconds;

    return smoothness;
}

void PreviewWindow::scheduleRepaint() {
    m_lastRepaintScheduleMs = nowMs();
    if (isVisible() && !m_repaintTimer.isActive() &&
        (m_interaction.playing || m_pendingFrameRequest || (m_cache && m_cache->pendingVisibleRequestCount() > 0))) {
        m_repaintTimer.start();
    }
    if (QThread::currentThread() == thread()) {
        update();
        return;
    }

    QMetaObject::invokeMethod(this, [this]() { update(); }, Qt::QueuedConnection);
}

void PreviewWindow::scheduleFrameRequest() {
    if (!m_frameRequestsArmed || !isVisible() || m_bulkUpdateDepth > 0) {
        return;
    }
    if (!m_frameRequestTimer.isActive()) {
        m_frameRequestTimer.start();
    }
}
