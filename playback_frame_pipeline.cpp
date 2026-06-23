#include "playback_frame_pipeline.h"
#include "debug_controls.h"
#include "editor_shared_timing.h"
#include "frame_buffer_utils.h"

#include <QDateTime>
#include <QElapsedTimer>
#include <QFileInfo>
#include <QPointer>
#include <algorithm>
#include <limits>

namespace editor {

namespace {

qint64 playbackPipelineTraceMs() {
    static QElapsedTimer timer;
    static bool started = false;
    if (!started) {
        timer.start();
        started = true;
    }
    return timer.elapsed();
}

void playbackPipelineTrace(const QString& stage, const QString& detail = QString()) {
    if (debugPlaybackLevel() < DebugLogLevel::Info) {
        return;
    }
    qDebug().noquote() << QStringLiteral("[PLAYBACK-PIPE %1 ms] %2%3")
                              .arg(playbackPipelineTraceMs(), 6)
                              .arg(stage)
                              .arg(detail.isEmpty() ? QString() : QStringLiteral(" | ") + detail);
}

bool isSingleFrameClip(const TimelineClip& clip) {
    return clip.mediaType == ClipMediaType::Image;
}

bool isImageSequencePlaybackClip(const TimelineClip& clip) {
    return clip.sourceKind == MediaSourceKind::ImageSequence;
}

QString framePayloadForDiagnostics(const FrameHandle& frame) {
    if (frame.isNull()) {
        return QStringLiteral("null");
    }
    if (frame.hasHardwareFrame()) {
        return QStringLiteral("hardware");
    }
    if (frame.hasGpuTexture()) {
        return QStringLiteral("gpu_texture");
    }
    if (frame.hasCpuImage()) {
        return QStringLiteral("cpu_image");
    }
    return QStringLiteral("unknown");
}

int64_t pendingFrameFromKey(const QString& key, const QString& clipId) {
    const QString prefix = clipId + QLatin1Char(':');
    if (!key.startsWith(prefix)) {
        return std::numeric_limits<int64_t>::max();
    }
    bool ok = false;
    const int64_t frame = key.mid(prefix.size()).toLongLong(&ok);
    return ok ? frame : std::numeric_limits<int64_t>::max();
}

bool pruneObsoletePendingFrameRequests(QSet<QString>* requests,
                                       const QHash<QString, int64_t>& activeLocalFrames,
                                       int64_t maxAheadFrame,
                                       QHash<QString, int64_t>* latestVisibleTargets) {
    if (!requests) {
        return false;
    }
    bool changed = false;
    for (auto it = requests->begin(); it != requests->end();) {
        const QString key = *it;
        const int separator = key.indexOf(QLatin1Char(':'));
        if (separator <= 0) {
            ++it;
            continue;
        }

        const QString clipId = key.left(separator);
        const auto activeIt = activeLocalFrames.find(clipId);
        if (activeIt == activeLocalFrames.end()) {
            ++it;
            continue;
        }

        bool ok = false;
        const int64_t pendingFrame = key.mid(separator + 1).toLongLong(&ok);
        if (!ok) {
            ++it;
            continue;
        }

        const int64_t minFrame = qMax<int64_t>(0, activeIt.value() - debugSequenceLateBufferSeedSlack());
        const int64_t maxFrame = activeIt.value() + maxAheadFrame;
        if (pendingFrame >= minFrame && pendingFrame <= maxFrame) {
            ++it;
            continue;
        }

        if (latestVisibleTargets) {
            latestVisibleTargets->remove(clipId);
        }
        it = requests->erase(it);
        changed = true;
    }
    return changed;
}

}

void PlaybackFramePipeline::PlaybackBuffer::clear() {
    QMutexLocker lock(&m_mutex);
    m_frames.clear();
}

void PlaybackFramePipeline::PlaybackBuffer::insert(int64_t frameNumber, const FrameHandle& frame) {
    if (frame.isNull()) {
        return;
    }
    QMutexLocker lock(&m_mutex);
    m_frames.insert(frameNumber, PlaybackFrameInfo{frame, QDateTime::currentMSecsSinceEpoch()});
    trimLocked();
}

FrameHandle PlaybackFramePipeline::PlaybackBuffer::get(int64_t frameNumber) const {
    QMutexLocker lock(&m_mutex);
    auto it = m_frames.find(frameNumber);
    return it == m_frames.end() ? FrameHandle() : it.value().frame;
}

FrameHandle PlaybackFramePipeline::PlaybackBuffer::getBest(int64_t frameNumber) const {
    QMutexLocker lock(&m_mutex);
    return closestBufferedFrame(m_frames, frameNumber);
}

FrameHandle PlaybackFramePipeline::PlaybackBuffer::getPresentation(int64_t frameNumber) const {
    QMutexLocker lock(&m_mutex);

    auto exact = m_frames.find(frameNumber);
    if (exact != m_frames.end()) {
        return exact.value().frame;
    }

    auto bestPast = m_frames.end();
    qint64 bestPastFrame = std::numeric_limits<qint64>::min();
    qint64 bestPastInsertedAt = std::numeric_limits<qint64>::min();
    auto bestFuture = m_frames.end();
    qint64 bestFutureDistance = std::numeric_limits<qint64>::max();

    for (auto it = m_frames.begin(); it != m_frames.end(); ++it) {
        if (it.key() <= frameNumber) {
            if (it.key() > bestPastFrame ||
                (it.key() == bestPastFrame && it.value().insertedAt > bestPastInsertedAt)) {
                bestPast = it;
                bestPastFrame = it.key();
                bestPastInsertedAt = it.value().insertedAt;
            }
            continue;
        }

        const qint64 futureDistance = it.key() - frameNumber;
        if (futureDistance > debugMaxPresentationFutureFrameDelta()) {
            continue;
        }
        if (futureDistance < bestFutureDistance) {
            bestFuture = it;
            bestFutureDistance = futureDistance;
        }
    }

    if (bestPast != m_frames.end() &&
        (frameNumber - bestPastFrame) <= debugMaxPresentationPastFrameDelta()) {
        return bestPast.value().frame;
    }
    return bestFuture == m_frames.end() ? FrameHandle() : bestFuture.value().frame;
}

bool PlaybackFramePipeline::PlaybackBuffer::contains(int64_t frameNumber) const {
    QMutexLocker lock(&m_mutex);
    return m_frames.contains(frameNumber);
}

int PlaybackFramePipeline::PlaybackBuffer::size() const {
    QMutexLocker lock(&m_mutex);
    return m_frames.size();
}

QJsonObject PlaybackFramePipeline::PlaybackBuffer::residencySnapshot() const {
    qint64 frameCount = 0;
    qint64 hardwareFrames = 0;
    qint64 gpuTextureFrames = 0;
    qint64 cpuBackedFrames = 0;
    qint64 cpuBytes = 0;
    qint64 gpuBytes = 0;

    QMutexLocker lock(&m_mutex);
    for (auto it = m_frames.cbegin(); it != m_frames.cend(); ++it) {
        const FrameHandle& frame = it.value().frame;
        if (frame.isNull()) {
            continue;
        }
        ++frameCount;
        hardwareFrames += frame.hasHardwareFrame() ? 1 : 0;
        gpuTextureFrames += frame.hasGpuTexture() ? 1 : 0;
        cpuBackedFrames += frame.hasCpuImage() ? 1 : 0;
        cpuBytes += static_cast<qint64>(frame.cpuMemoryUsage());
        gpuBytes += static_cast<qint64>(frame.gpuMemoryUsage());
    }

    return QJsonObject{
        {QStringLiteral("frames"), frameCount},
        {QStringLiteral("hardware_frames"), hardwareFrames},
        {QStringLiteral("gpu_texture_frames"), gpuTextureFrames},
        {QStringLiteral("cpu_backed_frames"), cpuBackedFrames},
        {QStringLiteral("cpu_bytes"), cpuBytes},
        {QStringLiteral("gpu_bytes"), gpuBytes}
    };
}

void PlaybackFramePipeline::PlaybackBuffer::trimLocked() {
    trimOldestBufferedFrames(&m_frames, kMaxFrames);
}

PlaybackFramePipeline::PlaybackFramePipeline(AsyncDecoder* decoder,
                                             QObject* parent)
    : QObject(parent), m_decoder(decoder) {
    if (m_decoder) {
        connect(m_decoder, &AsyncDecoder::frameReady, this, &PlaybackFramePipeline::onFrameReady);
    }
}

PlaybackFramePipeline::~PlaybackFramePipeline() {
    for (PlaybackBuffer* buffer : m_buffers) {
        delete buffer;
    }
    m_buffers.clear();
}

void PlaybackFramePipeline::setTimelineClips(const QVector<TimelineClip>& clips) {
    QMutexLocker lock(&m_clipsMutex);

    QHash<QString, ClipInfo> nextClips;
    QHash<QString, PlaybackBuffer*> nextBuffers;
    for (const TimelineClip& clip : clips) {
        if (!clipHasVisuals(clip)) {
            continue;
        }
        ClipInfo info;
        info.clip = clip;
        info.playbackPath = interactivePreviewMediaPathForClip(clip);
        info.isSingleFrame = isSingleFrameClip(clip);
        nextClips.insert(clip.id, info);
        if (m_buffers.contains(clip.id)) {
            nextBuffers.insert(clip.id, m_buffers.take(clip.id));
        } else {
            nextBuffers.insert(clip.id, new PlaybackBuffer());
        }
    }

    for (PlaybackBuffer* buffer : m_buffers) {
        delete buffer;
    }

    m_clips = std::move(nextClips);
    m_buffers = std::move(nextBuffers);
}

void PlaybackFramePipeline::setRenderSyncMarkers(const QVector<RenderSyncMarker>& markers) {
    QMutexLocker lock(&m_markersMutex);
    m_renderSyncMarkers = markers;
}

void PlaybackFramePipeline::setPlaybackActive(bool active) {
    const bool previous = m_active.exchange(active);
    if (previous && !active) {
        clearBuffers();
    }
}

void PlaybackFramePipeline::setPlayheadFrame(int64_t playheadFrame) {
    m_playheadFrame.store(playheadFrame);
    if (m_active.load()) {
        dropStaleRequestsForPlayhead(playheadFrame);
    }
}

void PlaybackFramePipeline::setPlaybackSpeed(qreal speed) {
    m_playbackSpeed.store(qBound<qreal>(0.1, std::abs(speed), 4.0));
}

void PlaybackFramePipeline::dropStaleRequestsForPlayhead(int64_t playheadFrame) {
    QHash<QString, int64_t> activeLocalFrames;
    QVector<RenderSyncMarker> markers;
    {
        QMutexLocker clipsLock(&m_clipsMutex);
        QMutexLocker markersLock(&m_markersMutex);
        markers = m_renderSyncMarkers;
        for (auto it = m_clips.cbegin(); it != m_clips.cend(); ++it) {
            const ClipInfo& info = it.value();
            if (info.isSingleFrame || info.playbackPath.isEmpty()) {
                continue;
            }
            if (playheadFrame < info.clip.startFrame ||
                playheadFrame >= info.clip.startFrame + info.clip.durationFrames) {
                continue;
            }
            const int64_t activeSourceFrame = normalizeFrameNumber(
                info,
                sourceFrameForClipAtTimelinePosition(info.clip,
                                                     static_cast<qreal>(playheadFrame),
                                                     markers));
            activeLocalFrames.insert(it.key(), activeSourceFrame);
        }
    }
    if (activeLocalFrames.isEmpty()) {
        return;
    }

    const int64_t maxAheadFrame =
        qMax<int64_t>(debugPlaybackWindowAhead(), debugMaxPresentationFutureFrameDelta());
    QMutexLocker pendingLock(&m_pendingMutex);
    pruneObsoletePendingFrameRequests(&m_pendingVisibleRequests,
                                      activeLocalFrames,
                                      maxAheadFrame,
                                      &m_latestVisibleTargets);
    pruneObsoletePendingFrameRequests(&m_pendingPrefetchRequests,
                                      activeLocalFrames,
                                      maxAheadFrame,
                                      nullptr);
}

void PlaybackFramePipeline::requestFramesForSample(int64_t samplePosition,
                                                   const std::function<void()>& onVisibleFrameReady) {
    if (!m_active.load()) {
        return;
    }
    if (!m_decoder) {
        return;
    }

    QVector<ClipInfo> activeClips;
    QVector<RenderSyncMarker> markers;
    {
        QMutexLocker clipsLock(&m_clipsMutex);
        QMutexLocker markersLock(&m_markersMutex);
        markers = m_renderSyncMarkers;
        for (auto it = m_clips.cbegin(); it != m_clips.cend(); ++it) {
            const TimelineClip& clip = it.value().clip;
            const int64_t clipStartSample = clipTimelineStartSamples(clip);
            const int64_t clipEndSample = clipTimelineEndSamples(clip);
            if (samplePosition >= clipStartSample && samplePosition < clipEndSample) {
                activeClips.push_back(it.value());
            }
        }
    }

    for (const ClipInfo& info : activeClips) {
        const int64_t requestedFrame = sourceFrameForClipAtTimelineSample(
            info.clip, samplePosition, markers);
        const int64_t canonicalFrame = normalizeFrameNumber(info, requestedFrame);
        schedulePlaybackWindow(info, samplePosition, canonicalFrame, markers, onVisibleFrameReady);
    }
}

FrameHandle PlaybackFramePipeline::getFrame(const QString& clipId, int64_t frameNumber) const {
    const int64_t normalizedFrame = normalizeFrameNumber(clipId, frameNumber);
    QMutexLocker lock(&m_clipsMutex);
    auto it = m_buffers.find(clipId);
    return (it == m_buffers.end() || !it.value()) ? FrameHandle() : it.value()->get(normalizedFrame);
}

FrameHandle PlaybackFramePipeline::getBestFrame(const QString& clipId, int64_t frameNumber) const {
    const int64_t normalizedFrame = normalizeFrameNumber(clipId, frameNumber);
    QMutexLocker lock(&m_clipsMutex);
    auto it = m_buffers.find(clipId);
    return (it == m_buffers.end() || !it.value()) ? FrameHandle() : it.value()->getBest(normalizedFrame);
}

FrameHandle PlaybackFramePipeline::getPresentationFrame(const QString& clipId, int64_t frameNumber) const {
    const int64_t normalizedFrame = normalizeFrameNumber(clipId, frameNumber);
    QMutexLocker lock(&m_clipsMutex);
    auto it = m_buffers.find(clipId);
    if (it == m_buffers.end() || !it.value()) {
        return FrameHandle();
    }

    const FrameHandle frame = it.value()->getPresentation(normalizedFrame);
    if (!frame.isNull() && frame.frameNumber() < normalizedFrame) {
        m_droppedPresentationFrames.fetch_add(static_cast<int>(normalizedFrame - frame.frameNumber()));
        playbackPipelineTrace(QStringLiteral("PlaybackFramePipeline::present.drop"),
                              QStringLiteral("clip=%1 requested=%2 presented=%3 delta=%4")
                                  .arg(clipId)
                                  .arg(normalizedFrame)
                                  .arg(frame.frameNumber())
                                  .arg(frame.frameNumber() - normalizedFrame));
    }
    return frame;
}

bool PlaybackFramePipeline::isFrameBuffered(const QString& clipId, int64_t frameNumber) const {
    const int64_t normalizedFrame = normalizeFrameNumber(clipId, frameNumber);
    QMutexLocker lock(&m_clipsMutex);
    auto it = m_buffers.find(clipId);
    return it != m_buffers.end() && it.value() && it.value()->contains(normalizedFrame);
}

int PlaybackFramePipeline::pendingVisibleRequestCount() const {
    QMutexLocker lock(&m_pendingMutex);
    return m_pendingVisibleRequests.size();
}

int PlaybackFramePipeline::bufferedFrameCount() const {
    QMutexLocker lock(&m_clipsMutex);
    int total = 0;
    for (auto it = m_buffers.cbegin(); it != m_buffers.cend(); ++it) {
        if (it.value()) {
            total += it.value()->size();
        }
    }
    return total;
}

QJsonObject PlaybackFramePipeline::decodeDiagnostics() const {
    QMutexLocker lock(&m_decodeDiagnosticsMutex);
    return QJsonObject{
        {QStringLiteral("visible_dispatched"), static_cast<qint64>(m_decodeDiagnostics.visibleDispatched)},
        {QStringLiteral("visible_completed"), static_cast<qint64>(m_decodeDiagnostics.visibleCompleted)},
        {QStringLiteral("visible_null_completed"), static_cast<qint64>(m_decodeDiagnostics.visibleNullCompleted)},
        {QStringLiteral("visible_obsolete_completed"),
         static_cast<qint64>(m_decodeDiagnostics.visibleObsoleteCompleted)},
        {QStringLiteral("visible_buffered_completed"),
         static_cast<qint64>(m_decodeDiagnostics.visibleBufferedCompleted)},
        {QStringLiteral("prefetch_dispatched"), static_cast<qint64>(m_decodeDiagnostics.prefetchDispatched)},
        {QStringLiteral("prefetch_completed"), static_cast<qint64>(m_decodeDiagnostics.prefetchCompleted)},
        {QStringLiteral("last_visible_clip_id"), m_decodeDiagnostics.lastVisibleClipId},
        {QStringLiteral("last_visible_frame"), m_decodeDiagnostics.lastVisibleFrame},
        {QStringLiteral("last_visible_payload"), m_decodeDiagnostics.lastVisiblePayload},
        {QStringLiteral("last_visible_outcome"), m_decodeDiagnostics.lastVisibleOutcome},
        {QStringLiteral("last_visible_wait_ms"), m_decodeDiagnostics.lastVisibleWaitMs},
        {QStringLiteral("max_visible_wait_ms"), m_decodeDiagnostics.maxVisibleWaitMs},
        {QStringLiteral("last_visible_retention_frames"), m_decodeDiagnostics.lastVisibleRetentionFrames},
        {QStringLiteral("last_visible_retention_latency_frames"), m_decodeDiagnostics.lastVisibleRetentionLatencyFrames},
        {QStringLiteral("last_configured_playback_window_ahead"),
         m_decodeDiagnostics.lastConfiguredPlaybackWindowAhead},
        {QStringLiteral("last_effective_playback_window_ahead"),
         m_decodeDiagnostics.lastEffectivePlaybackWindowAhead},
        {QStringLiteral("last_pending_visible_requests"),
         m_decodeDiagnostics.lastPendingVisibleRequests},
        {QStringLiteral("last_cancel_keep_from_frame"), m_decodeDiagnostics.lastCancelKeepFromFrame},
        {QStringLiteral("last_visible_qt_delivery_delay_ms"),
         m_decodeDiagnostics.lastVisibleQtDeliveryDelayMs},
        {QStringLiteral("max_visible_qt_delivery_delay_ms"),
         m_decodeDiagnostics.maxVisibleQtDeliveryDelayMs},
        {QStringLiteral("last_completed_age_ms"),
         m_decodeDiagnostics.lastCompletedAtMs > 0
             ? QDateTime::currentMSecsSinceEpoch() - m_decodeDiagnostics.lastCompletedAtMs
             : static_cast<qint64>(-1)}
    };
}

QJsonObject PlaybackFramePipeline::bufferedFrameResidencySnapshot() const {
    QJsonObject byClip;
    qint64 totalFrames = 0;
    qint64 totalHardwareFrames = 0;
    qint64 totalGpuTextureFrames = 0;
    qint64 totalCpuBackedFrames = 0;
    qint64 totalCpuBytes = 0;
    qint64 totalGpuBytes = 0;

    QMutexLocker lock(&m_clipsMutex);
    for (auto it = m_buffers.cbegin(); it != m_buffers.cend(); ++it) {
        if (!it.value()) {
            continue;
        }
        const QJsonObject clip = it.value()->residencySnapshot();
        byClip.insert(it.key(), clip);
        totalFrames += clip.value(QStringLiteral("frames")).toInteger();
        totalHardwareFrames += clip.value(QStringLiteral("hardware_frames")).toInteger();
        totalGpuTextureFrames += clip.value(QStringLiteral("gpu_texture_frames")).toInteger();
        totalCpuBackedFrames += clip.value(QStringLiteral("cpu_backed_frames")).toInteger();
        totalCpuBytes += clip.value(QStringLiteral("cpu_bytes")).toInteger();
        totalGpuBytes += clip.value(QStringLiteral("gpu_bytes")).toInteger();
    }

    return QJsonObject{
        {QStringLiteral("owner"), QStringLiteral("playback_frame_pipeline")},
        {QStringLiteral("frames"), totalFrames},
        {QStringLiteral("hardware_frames"), totalHardwareFrames},
        {QStringLiteral("gpu_texture_frames"), totalGpuTextureFrames},
        {QStringLiteral("cpu_backed_frames"), totalCpuBackedFrames},
        {QStringLiteral("cpu_bytes"), totalCpuBytes},
        {QStringLiteral("gpu_bytes"), totalGpuBytes},
        {QStringLiteral("by_clip"), byClip}
    };
}

void PlaybackFramePipeline::onFrameReady(FrameHandle frame) {
    if (!m_active.load() || frame.isNull()) {
        return;
    }

    const QString sourcePath = frame.sourcePath();
    if (sourcePath.isEmpty()) {
        return;
    }

    QVector<RenderSyncMarker> markers;
    {
        QMutexLocker lock(&m_markersMutex);
        markers = m_renderSyncMarkers;
    }

    struct SeedTarget {
        QString clipId;
        int64_t frameNumber = 0;
    };

    QVector<SeedTarget> seedTargets;
    const int64_t playheadFrame = m_playheadFrame.load();
    const int64_t maxAheadFrame =
        qMax<int64_t>(debugPlaybackWindowAhead(), debugMaxPresentationFutureFrameDelta());
    {
        QMutexLocker lock(&m_clipsMutex);
        for (auto it = m_clips.cbegin(); it != m_clips.cend(); ++it) {
            const ClipInfo& info = it.value();
            if (info.isSingleFrame || info.playbackPath != sourcePath) {
                continue;
            }

            if (playheadFrame < info.clip.startFrame ||
                playheadFrame >= info.clip.startFrame + info.clip.durationFrames) {
                continue;
            }

            const int64_t activeSourceFrame = normalizeFrameNumber(
                info,
                sourceFrameForClipAtTimelinePosition(info.clip,
                                                     static_cast<qreal>(playheadFrame),
                                                     markers));
            const int64_t decodedFrameNumber = normalizeFrameNumber(info, frame.frameNumber());
            const bool isSequenceClip = isImageSequencePlaybackClip(info.clip);
            const int64_t lateSeedSlack = isSequenceClip
                                              ? debugSequenceLateBufferSeedSlack()
                                              : debugObsoleteVisibleFrameSlack();
            const int64_t aheadSeedSlack = isSequenceClip
                                               ? maxAheadFrame
                                               : debugFileVideoPlaybackWindowAhead();
            const int64_t minSeedFrame = qMax<int64_t>(0, activeSourceFrame - lateSeedSlack);
            const int64_t maxSeedFrame = activeSourceFrame + aheadSeedSlack;
            if (decodedFrameNumber < minSeedFrame || decodedFrameNumber > maxSeedFrame) {
                continue;
            }

            seedTargets.push_back({it.key(), decodedFrameNumber});
        }
    }

    if (seedTargets.isEmpty()) {
        emit frameAvailable();
        return;
    }

    {
        QMutexLocker lock(&m_clipsMutex);
        for (const SeedTarget& target : seedTargets) {
            auto it = m_buffers.find(target.clipId);
            if (it == m_buffers.end() || !it.value()) {
                continue;
            }
            it.value()->insert(target.frameNumber, frame);
        }
    }

    emit frameAvailable();
}

QString PlaybackFramePipeline::requestKey(const QString& clipId, int64_t frameNumber) const {
    return clipId + QLatin1Char(':') + QString::number(frameNumber);
}

int64_t PlaybackFramePipeline::normalizeFrameNumber(const QString& clipId, int64_t frameNumber) const {
    QMutexLocker lock(&m_clipsMutex);
    const auto it = m_clips.find(clipId);
    return it == m_clips.end() ? frameNumber : normalizeFrameNumber(it.value(), frameNumber);
}

int64_t PlaybackFramePipeline::normalizeFrameNumber(const ClipInfo& info, int64_t frameNumber) const {
    return info.isSingleFrame ? 0 : frameNumber;
}

void PlaybackFramePipeline::clearBuffers() {
    {
        QMutexLocker lock(&m_clipsMutex);
        for (PlaybackBuffer* buffer : m_buffers) {
            if (buffer) {
                buffer->clear();
            }
        }
    }
    m_droppedPresentationFrames.store(0);
}

void PlaybackFramePipeline::cancelDecoderBeforeThrottled(const QString& playbackPath,
                                                         int64_t keepFromFrame,
                                                         qint64 nowMs) {
    if (!m_decoder || playbackPath.isEmpty()) {
        return;
    }

    const qint64 currentMs = nowMs >= 0 ? nowMs : QDateTime::currentMSecsSinceEpoch();
    QMutexLocker pendingLock(&m_pendingMutex);
    const int64_t previousKeepFrom =
        m_lastCancelKeepFromByPath.value(playbackPath, std::numeric_limits<int64_t>::min());
    const qint64 previousCancelAt = m_lastCancelAtMsByPath.value(playbackPath, 0);
    if (keepFromFrame <= previousKeepFrom) {
        return;
    }
    const bool advancedEnough =
        keepFromFrame >= previousKeepFrom + debugCancelBeforeMinFrameAdvance();
    const bool overdue =
        previousCancelAt <= 0 || (currentMs - previousCancelAt) >= debugCancelBeforeMinIntervalMs();
    if (!advancedEnough && !overdue) {
        return;
    }
    m_lastCancelKeepFromByPath.insert(playbackPath, keepFromFrame);
    m_lastCancelAtMsByPath.insert(playbackPath, currentMs);
    pendingLock.unlock();

    m_decoder->cancelForFileBefore(playbackPath, keepFromFrame);
    recordFrameTraceEvent(QStringLiteral("cancel_before"),
                          QString(),
                          keepFromFrame,
                          playbackPath);
}

void PlaybackFramePipeline::schedulePlaybackWindow(const ClipInfo& info,
                                                   int64_t samplePosition,
                                                   int64_t canonicalFrame,
                                                   const QVector<RenderSyncMarker>& markers,
                                                   const std::function<void()>& onFrameReady) {
    const bool isSequenceClip = isImageSequencePlaybackClip(info.clip);
    const int configuredPlaybackWindowAhead = isSequenceClip
                                                  ? debugPlaybackWindowAhead()
                                                  : debugFileVideoPlaybackWindowAhead();
    if (info.isSingleFrame || !m_active.load() || configuredPlaybackWindowAhead < 0) {
        return;
    }
    if (!m_decoder) {
        return;
    }

    const int64_t configuredKeepWindow = isSequenceClip
                                             ? debugSequenceVisibleDecodeKeepWindow()
                                             : debugVisibleDecodeKeepWindow();
    const int64_t obsoleteVisibleFrameSlack = isSequenceClip
                                                  ? debugSequenceObsoleteVisibleFrameSlack()
                                                  : debugObsoleteVisibleFrameSlack();
    const int64_t lateBufferSeedSlack = isSequenceClip
                                            ? debugSequenceLateBufferSeedSlack()
                                            : obsoleteVisibleFrameSlack;
    const qreal sourceFps = qMax<qreal>(1.0, resolvedSourceFps(info.clip));
    qint64 recentVisibleWaitMs = 0;
    {
        QMutexLocker diagnosticsLock(&m_decodeDiagnosticsMutex);
        recentVisibleWaitMs =
            qBound<qint64>(qint64{0}, m_decodeDiagnostics.lastVisibleWaitMs, qint64{500});
    }
    int pendingVisibleCount = 0;
    {
        QMutexLocker pendingLock(&m_pendingMutex);
        pendingVisibleCount = m_pendingVisibleRequests.size();
    }
    int playbackWindowAhead = configuredPlaybackWindowAhead;
    const qreal playbackSpeed = qBound<qreal>(0.1, m_playbackSpeed.load(), 4.0);
    const int latencyLeadFrames = configuredPlaybackWindowAhead > 0
        ? qBound(1,
                 static_cast<int>(std::ceil(((recentVisibleWaitMs + 20) * sourceFps * playbackSpeed) / 1000.0)),
                 configuredPlaybackWindowAhead)
        : 0;
    if (!isSequenceClip) {
        if (pendingVisibleCount >= qMax(1, debugMaxVisibleBacklog()) ||
            recentVisibleWaitMs > 33) {
            playbackWindowAhead = qMin(configuredPlaybackWindowAhead,
                                       qMax(1, latencyLeadFrames + 2));
        } else if (recentVisibleWaitMs > 16) {
            playbackWindowAhead = qMin(playbackWindowAhead,
                                       qMax(2, latencyLeadFrames));
        }
        if (playbackSpeed > 1.5) {
            const int speedCappedWindow =
                qBound(1,
                       static_cast<int>(std::ceil(playbackSpeed)) + 1,
                       configuredPlaybackWindowAhead);
            playbackWindowAhead = qMin(playbackWindowAhead, speedCappedWindow);
        }
    }
    const int64_t latencyRetentionFrames =
        qBound<int64_t>(int64_t{0},
                        static_cast<int64_t>(std::ceil((recentVisibleWaitMs * sourceFps) / 1000.0)),
                        int64_t{30});
    const int64_t effectiveKeepWindow = isSequenceClip
        ? configuredKeepWindow
        : qMax<int64_t>(configuredKeepWindow,
                        playbackWindowAhead +
                            lateBufferSeedSlack +
                            debugMaxPresentationPastFrameDelta() +
                            latencyRetentionFrames +
                            4);
    int64_t decoderKeepFromFrame =
        qMax<int64_t>(0, canonicalFrame - effectiveKeepWindow);
    {
        QMutexLocker pendingLock(&m_pendingMutex);
        int64_t earliestPendingFrame = std::numeric_limits<int64_t>::max();
        for (const QString& key : m_pendingVisibleRequests) {
            earliestPendingFrame = qMin(earliestPendingFrame,
                                        pendingFrameFromKey(key, info.clip.id));
        }
        for (const QString& key : m_pendingPrefetchRequests) {
            earliestPendingFrame = qMin(earliestPendingFrame,
                                        pendingFrameFromKey(key, info.clip.id));
        }
        if (earliestPendingFrame != std::numeric_limits<int64_t>::max()) {
            decoderKeepFromFrame = qMin(decoderKeepFromFrame, earliestPendingFrame);
        }
    }
    cancelDecoderBeforeThrottled(info.playbackPath,
                                 decoderKeepFromFrame,
                                 QDateTime::currentMSecsSinceEpoch());
    {
        QMutexLocker diagnosticsLock(&m_decodeDiagnosticsMutex);
        m_decodeDiagnostics.lastVisibleRetentionFrames = effectiveKeepWindow;
        m_decodeDiagnostics.lastVisibleRetentionLatencyFrames = latencyRetentionFrames;
        m_decodeDiagnostics.lastConfiguredPlaybackWindowAhead = configuredPlaybackWindowAhead;
        m_decodeDiagnostics.lastEffectivePlaybackWindowAhead = playbackWindowAhead;
        m_decodeDiagnostics.lastPendingVisibleRequests = pendingVisibleCount;
        m_decodeDiagnostics.lastCancelKeepFromFrame = decoderKeepFromFrame;
    }

    const int firstOffset =
        (!isSequenceClip && pendingVisibleCount >= qMax(1, debugMaxVisibleBacklog()))
            ? 1
            : 0;
    for (int offset = firstOffset; offset <= playbackWindowAhead; ++offset) {
        const int64_t targetFrame = offset == 0
                                        ? canonicalFrame
                                        : (isSequenceClip
                                               ? normalizeFrameNumber(
                                                     info,
                                                     sourceFrameForClipAtTimelineSample(
                                                         info.clip,
                                                         samplePosition + frameToSamples(offset),
                                                         markers))
                                               : normalizeFrameNumber(info, canonicalFrame + offset));
        const QString key = requestKey(info.clip.id, targetFrame);
        if (isFrameBuffered(info.clip.id, targetFrame)) {
            continue;
        }

        {
            QMutexLocker pendingLock(&m_pendingMutex);
            if (m_pendingVisibleRequests.contains(key) || m_pendingPrefetchRequests.contains(key)) {
                continue;
            }
            if (offset == 0) {
                m_pendingVisibleRequests.insert(key);
                m_latestVisibleTargets.insert(info.clip.id, targetFrame);
            } else {
                m_pendingPrefetchRequests.insert(key);
            }
        }

        QPointer<PlaybackFramePipeline> self(this);
        const qint64 requestedAt = playbackPipelineTraceMs();
        const qint64 requestedAtWallMs = QDateTime::currentMSecsSinceEpoch();
        const DecodeRequestKind kind = offset == 0 ? DecodeRequestKind::Visible
                                                   : DecodeRequestKind::Prefetch;
        const int priority = kind == DecodeRequestKind::Visible ? 100 : qMax(10, 60 - offset);
        {
            QMutexLocker diagnosticsLock(&m_decodeDiagnosticsMutex);
            if (kind == DecodeRequestKind::Visible) {
                ++m_decodeDiagnostics.visibleDispatched;
                m_decodeDiagnostics.lastVisibleClipId = info.clip.id;
                m_decodeDiagnostics.lastVisibleFrame = targetFrame;
                m_decodeDiagnostics.lastVisibleOutcome = QStringLiteral("dispatched");
                recordFrameTraceEvent(QStringLiteral("dispatch"),
                                      info.clip.id, targetFrame,
                                      QStringLiteral("canonical=%1").arg(canonicalFrame));
            } else {
                ++m_decodeDiagnostics.prefetchDispatched;
            }
        }

        if (debugCacheWarnOnlyEnabled() && offset == 0) {
            qDebug().noquote() << QStringLiteral("[CACHE][WARN] %1 PlaybackFramePipeline::visible-miss | clip=%2 frame=%3 normalized=%4")
                                      .arg(playbackPipelineTraceMs(), 6)
                                      .arg(info.clip.id)
                                      .arg(canonicalFrame)
                                      .arg(targetFrame);
        }

        // Build the completion callback that inserts into the playback buffer
        auto completionCallback = [self,
                                   clipId = info.clip.id,
                                   targetFrame,
                                   key,
                                   kind,
                                   requestedAt,
                                   requestedAtWallMs,
                                   onFrameReady,
                                   obsoleteVisibleFrameSlack,
                                   lateBufferSeedSlack](FrameHandle frame) {
            if (!self) {
                return;
            }
            const qint64 decoderCallbackAtMs = QDateTime::currentMSecsSinceEpoch();
            QMetaObject::invokeMethod(
                self,
                [self,
                 clipId,
                 targetFrame,
                 key,
                 kind,
                 requestedAt,
                 requestedAtWallMs,
                 decoderCallbackAtMs,
                 onFrameReady,
                 frame,
                 obsoleteVisibleFrameSlack,
                 lateBufferSeedSlack]() {
                if (!self) {
                    return;
                }
                int64_t latestTargetFrame = targetFrame;
                // Remove from local pending sets.
                {
                    QMutexLocker pendingLock(&self->m_pendingMutex);
                    latestTargetFrame = self->m_latestVisibleTargets.value(clipId, targetFrame);
                    if (kind == DecodeRequestKind::Visible) {
                        self->m_pendingVisibleRequests.remove(key);
                        const int64_t latest = self->m_latestVisibleTargets.value(clipId, targetFrame);
                        if (targetFrame >= latest) {
                            self->m_latestVisibleTargets.remove(clipId);
                        }
                    } else {
                        self->m_pendingPrefetchRequests.remove(key);
                    }
                }

                const int64_t playheadFrame = self->m_playheadFrame.load();
                Q_UNUSED(playheadFrame);
                const bool obsoleteForPresentation =
                    !frame.isNull() &&
                    targetFrame < qMax<int64_t>(0, latestTargetFrame - obsoleteVisibleFrameSlack);
                const bool bufferableLateCompletion =
                    !frame.isNull() &&
                    targetFrame >= qMax<int64_t>(0, latestTargetFrame - lateBufferSeedSlack);

                if (!frame.isNull() &&
                    (!obsoleteForPresentation || bufferableLateCompletion)) {
                    const int64_t bufferFrameNumber =
                        frame.frameNumber() >= 0 ? frame.frameNumber() : targetFrame;
                    QMutexLocker lock(&self->m_clipsMutex);
                    auto it = self->m_buffers.find(clipId);
                    if (it != self->m_buffers.end() && it.value()) {
                        it.value()->insert(bufferFrameNumber, frame);
                    }
                }

                {
                    QMutexLocker diagnosticsLock(&self->m_decodeDiagnosticsMutex);
                    auto& diagnostics = self->m_decodeDiagnostics;
                    const qint64 nowMs = QDateTime::currentMSecsSinceEpoch();
                    if (kind == DecodeRequestKind::Visible) {
                        ++diagnostics.visibleCompleted;
                        diagnostics.lastVisibleClipId = clipId;
                        diagnostics.lastVisibleFrame = targetFrame;
                        diagnostics.lastVisiblePayload = framePayloadForDiagnostics(frame);
                        diagnostics.lastVisibleWaitMs =
                            requestedAtWallMs > 0 ? nowMs - requestedAtWallMs : -1;
                        diagnostics.maxVisibleWaitMs =
                            qMax(diagnostics.maxVisibleWaitMs,
                                 diagnostics.lastVisibleWaitMs);
                        diagnostics.lastVisibleQtDeliveryDelayMs =
                            decoderCallbackAtMs > 0 ? nowMs - decoderCallbackAtMs : -1;
                        diagnostics.maxVisibleQtDeliveryDelayMs =
                            qMax(diagnostics.maxVisibleQtDeliveryDelayMs,
                                 diagnostics.lastVisibleQtDeliveryDelayMs);
                        diagnostics.lastCompletedAtMs = nowMs;
                        const qint64 waitMs =
                            requestedAtWallMs > 0 ? nowMs - requestedAtWallMs : -1;
                        if (frame.isNull()) {
                            ++diagnostics.visibleNullCompleted;
                            diagnostics.lastVisibleOutcome = QStringLiteral("null");
                            self->recordFrameTraceEvent(QStringLiteral("null"),
                                                        clipId, targetFrame,
                                                        QStringLiteral("waitMs=%1").arg(waitMs),
                                                        waitMs);
                        } else if (obsoleteForPresentation && !bufferableLateCompletion) {
                            ++diagnostics.visibleObsoleteCompleted;
                            diagnostics.lastVisibleOutcome = QStringLiteral("obsolete");
                            self->recordFrameTraceEvent(QStringLiteral("obsolete"),
                                                        clipId, targetFrame,
                                                        QStringLiteral("waitMs=%1").arg(waitMs),
                                                        waitMs);
                        } else {
                            ++diagnostics.visibleBufferedCompleted;
                            diagnostics.lastVisibleOutcome = QStringLiteral("buffered");
                            self->recordFrameTraceEvent(QStringLiteral("buffered"),
                                                        clipId, targetFrame,
                                                        QStringLiteral("waitMs=%1").arg(waitMs),
                                                        waitMs);
                        }
                    } else {
                        ++diagnostics.prefetchCompleted;
                    }
                }

                if (debugCacheWarnOnlyEnabled() &&
                    kind == DecodeRequestKind::Visible &&
                    (frame.isNull() ||
                     obsoleteForPresentation ||
                     playbackPipelineTraceMs() - requestedAt > 33)) {
                    qDebug().noquote() << QStringLiteral("[CACHE][WARN] %1 PlaybackFramePipeline::visible-complete | clip=%2 frame=%3 null=%4 waitMs=%5")
                                              .arg(playbackPipelineTraceMs(), 6)
                                              .arg(clipId)
                                              .arg(targetFrame)
                                              .arg(frame.isNull() || obsoleteForPresentation ? 1 : 0)
                                              .arg(playbackPipelineTraceMs() - requestedAt);
                }

                if (onFrameReady &&
                    ((!frame.isNull() &&
                      (!obsoleteForPresentation || bufferableLateCompletion)) ||
                     kind == DecodeRequestKind::Visible)) {
                    onFrameReady();
                }
            },
                Qt::QueuedConnection);
        };

        m_decoder->requestFrame(info.playbackPath,
                                targetFrame,
                                priority,
                                kind == DecodeRequestKind::Visible ? 30000 : 12000,
                                kind,
                                std::move(completionCallback));
    }
}

void PlaybackFramePipeline::recordFrameTraceEvent(const QString& event,
                                                   const QString& clipId,
                                                   int64_t targetFrame,
                                                   const QString& payload,
                                                   qint64 waitMs)
{
    QMutexLocker lock(&m_frameTraceMutex);
    if (m_frameTraceEvents.size() >= kMaxFrameTraceEvents) {
        m_frameTraceEvents.removeFirst();
    }
    m_frameTraceEvents.push_back({
        QDateTime::currentMSecsSinceEpoch(),
        m_playheadFrame.load(),
        targetFrame,
        clipId,
        event,
        payload,
        waitMs
    });
}

QJsonArray PlaybackFramePipeline::frameTraceSnapshot(int limit) const
{
    QMutexLocker lock(&m_frameTraceMutex);
    const int start = qMax(0, m_frameTraceEvents.size() - limit);
    QJsonArray result;
    for (int i = start; i < m_frameTraceEvents.size(); ++i) {
        const FrameTraceEvent& ev = m_frameTraceEvents.at(i);
        result.append(QJsonObject{
            {QStringLiteral("ts"), ev.timestampMs},
            {QStringLiteral("playhead"), static_cast<qint64>(ev.playheadFrame)},
            {QStringLiteral("frame"), static_cast<qint64>(ev.targetFrame)},
            {QStringLiteral("clip"), ev.clipId},
            {QStringLiteral("event"), ev.event},
            {QStringLiteral("payload"), ev.payload},
            {QStringLiteral("wait_ms"), ev.waitMs}
        });
    }
    return result;
}

}  // namespace editor
