#include "opengl_preview.h"
#include "opengl_preview_debug.h"

#include "frame_handle.h"
#include "async_decoder.h"
#include "timeline_cache.h"
#include "playback_frame_pipeline.h"
#include "memory_budget.h"
#include "media_pipeline_shared.h"
#include "waveform_service.h"
#include "render_backend.h"
#include "render_internal.h"

#include <QElapsedTimer>
#include <QJsonArray>
#include <QJsonObject>
#include <QMessageBox>
#include <QOpenGLWidget>
#include <QPointer>
#include <QThread>
#include <QTimer>

#include <algorithm>
#include <cmath>
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

void PreviewWindow::setBoxstreamOverlaySource(const QString& source) {
    const QString normalized = source.trimmed().isEmpty()
        ? QStringLiteral("all")
        : source.trimmed().toLower();
    if (m_boxstreamOverlaySource == normalized) {
        return;
    }
    m_boxstreamOverlaySource = normalized;
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
    if (m_interaction.viewMode == ViewMode::Video) {
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
    const qint64 now = nowMs();
    QJsonObject snapshot{{QStringLiteral("backend"), backendName()},
                         {QStringLiteral("playing"), m_interaction.playing},
                         {QStringLiteral("current_frame"), static_cast<qint64>(m_interaction.currentFrame)},
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
        snapshot[QStringLiteral("cache")] = QJsonObject{{QStringLiteral("hit_rate"), m_cache->cacheHitRate()},
                                                         {QStringLiteral("total_memory_usage"), static_cast<qint64>(m_cache->totalMemoryUsage())},
                                                         {QStringLiteral("total_cached_frames"), m_cache->totalCachedFrames()},
                                                         {QStringLiteral("pending_visible_requests"), m_cache->pendingVisibleRequestCount()},
                                                         {QStringLiteral("pending_visible_debug"), m_cache->pendingVisibleDebugSnapshot(now)}};
    }

    if (m_playbackPipeline) {
        snapshot[QStringLiteral("playback_pipeline")] = QJsonObject{{QStringLiteral("active"), m_interaction.playing},
                                                                     {QStringLiteral("buffered_frames"), m_playbackPipeline->bufferedFrameCount()},
                                                                     {QStringLiteral("pending_visible_requests"), m_playbackPipeline->pendingVisibleRequestCount()},
                                                                     {QStringLiteral("dropped_presentation_frames"), m_playbackPipeline->droppedPresentationFrameCount()}};
    }

    if (!m_lastFrameSelectionStats.isEmpty()) {
        // Keep REST profiling snapshots UI-thread cheap. Per-clip metadata
        // enrichment can touch decoder state and cause the control server's
        // UI-thread profile callback to time out during active playback.
        QJsonObject frameSelection = m_lastFrameSelectionStats;
        frameSelection[QStringLiteral("metadata_enriched")] = false;
        snapshot[QStringLiteral("frame_selection")] = frameSelection;
    }

    return snapshot;
}

void PreviewWindow::resetProfilingStats() {
    m_maxRenderDurationMs = 0;
    m_renderCount = 0;
    m_totalRenderDurationMs = 0;
    m_renderTimeHistory.clear();
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
