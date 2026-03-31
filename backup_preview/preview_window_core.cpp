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
#include <QTimer>

#include <algorithm>
#include <cmath>

using namespace editor;

PreviewWindow::PreviewWindow(QWidget* parent)
    : QOpenGLWidget(parent)
    , m_quadBuffer(QOpenGLBuffer::VertexBuffer)
{
    setMinimumSize(320, 180);
    setMouseTracking(true);
    m_lastPaintMs = nowMs();
    m_repaintTimer.setSingleShot(true);
    m_repaintTimer.setInterval(16);
    connect(&m_repaintTimer, &QTimer::timeout, this, [this]() { update(); });
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
    playbackTrace(QStringLiteral("PreviewWindow::setCurrentPlaybackSample"),
                  QStringLiteral("sample=%1 frame=%2 visible=%3 cache=%4")
                      .arg(sanitizedSample)
                      .arg(framePosition, 0, 'f', 3)
                      .arg(isVisible())
                      .arg(m_cache != nullptr));
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
        if (clipVisualPlaybackEnabled(clip)) visualClipIds.insert(clip.id);
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
        if (!clipVisualPlaybackEnabled(clip)) continue;
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

void PreviewWindow::setBypassGrading(bool bypass) {
    if (m_bypassGrading == bypass) return;
    m_bypassGrading = bypass;
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

QList<TimelineClip> PreviewWindow::getActiveClips() const {
    QList<TimelineClip> active;
    for (const TimelineClip& clip : m_clips) {
        if (isSampleWithinClip(clip, m_currentSample)) active.push_back(clip);
    }
    std::sort(active.begin(), active.end(), [](const TimelineClip& a, const TimelineClip& b) {
        if (a.trackIndex == b.trackIndex) return a.startFrame < b.startFrame;
        return a.trackIndex < b.trackIndex;
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
                         {QStringLiteral("last_frame_request_ms"), m_lastFrameRequestMs},
                         {QStringLiteral("last_frame_ready_ms"), m_lastFrameReadyMs},
                         {QStringLiteral("last_paint_ms"), m_lastPaintMs},
                         {QStringLiteral("last_repaint_schedule_ms"), m_lastRepaintScheduleMs},
                         {QStringLiteral("last_frame_request_age_ms"), m_lastFrameRequestMs > 0 ? now - m_lastFrameRequestMs : -1},
                         {QStringLiteral("last_frame_ready_age_ms"), m_lastFrameReadyMs > 0 ? now - m_lastFrameReadyMs : -1},
                         {QStringLiteral("last_paint_age_ms"), m_lastPaintMs > 0 ? now - m_lastPaintMs : -1},
                         {QStringLiteral("repaint_timer_active"), m_repaintTimer.isActive()},
                         {QStringLiteral("bypass_grading"), m_bypassGrading}};

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
                                                         {QStringLiteral("pending_visible_requests"), m_cache->pendingVisibleRequestCount()}};
    }

    if (m_playbackPipeline) {
        snapshot[QStringLiteral("playback_pipeline")] = QJsonObject{{QStringLiteral("active"), m_playing},
                                                                     {QStringLiteral("buffered_frames"), m_playbackPipeline->bufferedFrameCount()},
                                                                     {QStringLiteral("pending_visible_requests"), m_playbackPipeline->pendingVisibleRequestCount()},
                                                                     {QStringLiteral("dropped_presentation_frames"), m_playbackPipeline->droppedPresentationFrameCount()}};
    }

    // Kept intact from the original, but abbreviated here only in comments would lose behavior.
    if (!m_lastFrameSelectionStats.isEmpty()) {
        QJsonObject frameSelection = m_lastFrameSelectionStats;
        if (m_decoder) {
            QJsonArray enrichedClips;
            const QJsonArray clips = frameSelection.value(QStringLiteral("clips")).toArray();
            for (const QJsonValue& value : clips) {
                if (!value.isObject()) continue;
                QJsonObject clipObject = value.toObject();
                const QString clipId = clipObject.value(QStringLiteral("id")).toString();
                auto clipIt = std::find_if(m_clips.begin(), m_clips.end(), [&clipId](const TimelineClip& clip) {
                    return clip.id == clipId;
                });
                if (clipIt != m_clips.end()) {
                    const QString decodePath = interactivePreviewMediaPathForClip(*clipIt);
                    if (!decodePath.isEmpty()) {
                        const VideoStreamInfo info = m_decoder->getVideoInfo(decodePath);
                        if (info.isValid) {
                            clipObject[QStringLiteral("decode_path")] = info.decodePath;
                            clipObject[QStringLiteral("interop_path")] = info.interopPath;
                            clipObject[QStringLiteral("decode_mode_requested")] = info.requestedDecodeMode;
                            clipObject[QStringLiteral("hardware_accelerated")] = info.hardwareAccelerated;
                            clipObject[QStringLiteral("codec")] = info.codecName;
                        }
                    }
                }
                enrichedClips.append(clipObject);
            }
            frameSelection[QStringLiteral("clips")] = enrichedClips;
        }
        snapshot[QStringLiteral("frame_selection")] = frameSelection;
    }

    return snapshot;
}
void PreviewWindow::scheduleRepaint() {
    m_lastRepaintScheduleMs = nowMs();
    if (!m_repaintTimer.isActive()) {
        m_repaintTimer.start();
    }
}

void PreviewWindow::scheduleFrameRequest() {
    if (!m_frameRequestsArmed || !isVisible() || m_bulkUpdateDepth > 0) {
        return;
    }
    if (!m_frameRequestTimer.isActive()) {
        m_frameRequestTimer.start();
    }
}
