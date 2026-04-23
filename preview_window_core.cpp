#include "preview.h"
#include "preview_debug.h"

#include "frame_handle.h"
#include "async_decoder.h"
#include "timeline_cache.h"
#include "playback_frame_pipeline.h"
#include "memory_budget.h"
#include "media_pipeline_shared.h"

#include <QElapsedTimer>
#include <QJsonArray>
#include <QJsonObject>
#include <QOpenGLWidget>
#include <QThread>
#include <QTimer>

#include <algorithm>
#include <cmath>
#include <vector>

using namespace editor;

PreviewWindow::PreviewWindow(QWidget* parent)
    : QOpenGLWidget(parent)
    , m_quadBuffer(QOpenGLBuffer::VertexBuffer)
    , m_polygonBuffer(QOpenGLBuffer::VertexBuffer)
{
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

        if (stalledPresentation && (m_playing || pendingDecodeWork)) {
            update();
        }

        if (!m_playing && !pendingDecodeWork) {
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
                      .arg(m_clips.size())
                      .arg(m_cache != nullptr));
    m_playing = playing;
    if (playing && !m_clips.isEmpty()) {
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
        m_playing && qAbs(displayFrame - m_currentFrame) > 2;
    playbackTrace(QStringLiteral("PreviewWindow::setCurrentPlaybackSample"),
                  QStringLiteral("sample=%1 frame=%2 visible=%3 cache=%4")
                      .arg(sanitizedSample)
                      .arg(framePosition, 0, 'f', 3)
                      .arg(isVisible())
                      .arg(m_cache != nullptr));
    if (discontinuousFrameJump) {
        m_lastPresentedFrames.clear();
    }
    m_currentSample = sanitizedSample;
    m_currentFramePosition = framePosition;
    m_currentFrame = displayFrame;
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

void PreviewWindow::setClipCount(int count) { m_clipCount = count; scheduleRepaint(); }

void PreviewWindow::setSelectedClipId(const QString& clipId) {
    if (m_selectedClipId == clipId) return;
    m_selectedClipId = clipId;
    scheduleRepaint();
}

void PreviewWindow::setTimelineClips(const QVector<TimelineClip>& clips) {
    playbackTrace(QStringLiteral("PreviewWindow::setTimelineClips"),
                  QStringLiteral("clips=%1 cache=%2").arg(clips.size()).arg(m_cache != nullptr));
    m_clips = clips;
    m_transcriptSectionsCache.clear();
    QSet<QString> visualClipIds;
    for (const auto& clip : clips) {
        if (clipVisualPlaybackEnabled(clip, m_tracks)) visualClipIds.insert(clip.id);
    }
    for (auto it = m_lastPresentedFrames.begin(); it != m_lastPresentedFrames.end();) {
        if (!visualClipIds.contains(it.key())) it = m_lastPresentedFrames.erase(it);
        else ++it;
    }
    if (m_playbackPipeline) m_playbackPipeline->setTimelineClips(clips);
    if (!m_cache) {
        m_registeredClips.clear();
        scheduleRepaint();
        return;
    }

    QSet<QString> registeredIds;
    for (const auto& clip : clips) {
        if (!clipVisualPlaybackEnabled(clip, m_tracks)) continue;
        registeredIds.insert(clip.id);
        if (!m_registeredClips.contains(clip.id)) {
            m_cache->registerClip(clip);
            m_registeredClips.insert(clip.id);
        }
    }
    for (const QString& id : m_registeredClips) {
        if (!registeredIds.contains(id)) m_cache->unregisterClip(id);
    }
    m_registeredClips = registeredIds;

    if (m_bulkUpdateDepth > 0) m_pendingFrameRequest = true;
    else if (m_frameRequestsArmed) { m_pendingFrameRequest = true; scheduleFrameRequest(); }
    else m_pendingFrameRequest = true;
    scheduleRepaint();
}

void PreviewWindow::setTimelineTracks(const QVector<TimelineTrack>& tracks) {
    m_tracks = tracks;
    setTimelineClips(m_clips);
}

void PreviewWindow::setRenderSyncMarkers(const QVector<RenderSyncMarker>& markers) {
    m_renderSyncMarkers = markers;
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
    return usingCpuFallback() ? QStringLiteral("CPU Preview Fallback")
                              : QStringLiteral("OpenGL Shader Preview");
}

void PreviewWindow::setAudioMuted(bool muted) { m_audioMuted = muted; }
void PreviewWindow::setAudioVolume(qreal volume) { m_audioVolume = qBound<qreal>(0.0, volume, 1.0); }

void PreviewWindow::setOutputSize(const QSize& size) {
    const QSize sanitized(qMax(16, size.width()), qMax(16, size.height()));
    if (m_outputSize == sanitized) return;
    m_outputSize = sanitized;
    scheduleRepaint();
}

void PreviewWindow::setHideOutsideOutputWindow(bool hide) {
    if (m_hideOutsideOutputWindow == hide) return;
    m_hideOutsideOutputWindow = hide;
    scheduleRepaint();
}

void PreviewWindow::setBackgroundColor(const QColor& color) {
    if (m_backgroundColor == color) return;
    m_backgroundColor = color;
    scheduleRepaint();
}

void PreviewWindow::setPreviewZoom(qreal zoom) {
    // Clamp to valid range: 0.1x to 5.0x
    m_previewZoom = qBound<qreal>(0.1, zoom, 5.0);
    scheduleRepaint();
}

void PreviewWindow::setTranscriptOverlayInteractionEnabled(bool enabled) {
    if (m_transcriptOverlayInteractionEnabled == enabled) {
        return;
    }
    m_transcriptOverlayInteractionEnabled = enabled;
    if (!enabled && m_dragMode != PreviewDragMode::None) {
        const PreviewOverlayInfo selectedInfo = m_overlayInfo.value(m_selectedClipId);
        if (selectedInfo.kind == PreviewOverlayKind::TranscriptOverlay) {
            m_dragMode = PreviewDragMode::None;
            m_dragOriginBounds = QRectF();
        }
    }
    scheduleRepaint();
}

void PreviewWindow::setTitleOverlayInteractionOnly(bool enabled) {
    if (m_titleOverlayInteractionOnly == enabled) {
        return;
    }
    m_titleOverlayInteractionOnly = enabled;
    if (m_titleOverlayInteractionOnly && !clipIdIsTitle(m_selectedClipId)) {
        m_dragMode = PreviewDragMode::None;
        m_dragOriginBounds = QRectF();
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
bool PreviewWindow::audioMuted() const { return m_audioMuted; }
int PreviewWindow::audioVolumePercent() const { return qRound(m_audioVolume * 100.0); }

QString PreviewWindow::activeAudioClipLabel() const {
    for (const TimelineClip& clip : m_clips) {
        if (clipAudioPlaybackEnabled(clip) && isSampleWithinClip(clip, m_currentSample)) {
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

    for (const TimelineClip& clip : m_clips) {
        if (clip.id != clipId || clip.durationFrames <= 0) {
            continue;
        }
        const int64_t localFrame = qBound<int64_t>(
            0,
            static_cast<int64_t>(std::floor(m_currentFramePosition)) - clip.startFrame,
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
    for (const TimelineClip& clip : m_clips) {
        if (clip.id == clipId) {
            return clip.mediaType == ClipMediaType::Title;
        }
    }
    return false;
}

QList<TimelineClip> PreviewWindow::getActiveClips() const {
    QList<TimelineClip> active;
    for (const TimelineClip& clip : m_clips) {
        if (isSampleWithinClip(clip, m_currentSample)) active.push_back(clip);
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
                         {QStringLiteral("playing"), m_playing},
                         {QStringLiteral("current_frame"), static_cast<qint64>(m_currentFrame)},
                         {QStringLiteral("clip_count"), m_clips.size()},
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

    // Render timing statistics
    QJsonObject renderTiming;
    renderTiming[QStringLiteral("last_ms")] = static_cast<qint64>(m_lastRenderDurationMs);
    renderTiming[QStringLiteral("max_ms")] = static_cast<qint64>(m_maxRenderDurationMs);
    renderTiming[QStringLiteral("count")] = static_cast<qint64>(m_renderCount);
    renderTiming[QStringLiteral("avg_ms")] = m_renderCount > 0 
        ? static_cast<double>(m_totalRenderDurationMs) / static_cast<double>(m_renderCount) 
        : 0.0;
    renderTiming[QStringLiteral("target_ms")] = m_playing ? 33.33 : 16.67; // 30fps vs 60fps target
    renderTiming[QStringLiteral("slow_frame")] = m_lastRenderDurationMs > (m_playing ? 33 : 16);
    
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
        snapshot[QStringLiteral("playback_pipeline")] = QJsonObject{{QStringLiteral("active"), m_playing},
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
        (m_playing || m_pendingFrameRequest || (m_cache && m_cache->pendingVisibleRequestCount() > 0))) {
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
