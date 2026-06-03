#include "playback_frame_pipeline.h"
#include "debug_controls.h"
#include "frame_buffer_utils.h"
#include "frame_dispatcher.h"

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

void PlaybackFramePipeline::PlaybackBuffer::trimLocked() {
    trimOldestBufferedFrames(&m_frames, kMaxFrames);
}

PlaybackFramePipeline::PlaybackFramePipeline(AsyncDecoder* decoder,
                                             FrameDispatcher* dispatcher,
                                             QObject* parent)
    : QObject(parent), m_decoder(decoder), m_dispatcher(dispatcher) {
    if (m_dispatcher) {
        // Route completed frames from the dispatcher into our playback buffers
        connect(m_dispatcher, &FrameDispatcher::frameAvailable,
                this, [this](const QString& /*filePath*/, int64_t /*frameNumber*/) {
                    // The dispatcher's onFrameDecoded already invokes callbacks
                    // that insert into our buffers. This signal just notifies us
                    // that a new frame may be available.
                    emit frameAvailable();
                });
    } else if (m_decoder) {
        // Fallback: connect directly to decoder (legacy path)
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

void PlaybackFramePipeline::dropStaleRequestsForPlayhead(int64_t playheadFrame) {
    // Stale request pruning is now handled by FrameDispatcher::cancelBefore()
    // which is called from schedulePlaybackWindow().
    Q_UNUSED(playheadFrame);
}

void PlaybackFramePipeline::requestFramesForSample(int64_t samplePosition,
                                                   const std::function<void()>& onVisibleFrameReady) {
    if (!m_active.load()) {
        return;
    }
    if (!m_decoder && !m_dispatcher) {
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
            const int64_t clipStartSample = frameToSamples(clip.startFrame) + clip.startSubframeSamples;
            const int64_t clipEndSample = clipStartSample + frameToSamples(clip.durationFrames);
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
    if (m_dispatcher) {
        return m_dispatcher->pendingCount();
    }
    // Legacy fallback: no dispatcher configured, return 0
    return 0;
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
    if (m_dispatcher) {
        m_dispatcher->cancelAll();
    }
    m_droppedPresentationFrames.store(0);
}

void PlaybackFramePipeline::cancelDecoderBeforeThrottled(const QString& playbackPath,
                                                         int64_t keepFromFrame,
                                                         qint64 nowMs) {
    // Cancel-before is now handled by FrameDispatcher::cancelBefore()
    // which is called from schedulePlaybackWindow().
    Q_UNUSED(playbackPath);
    Q_UNUSED(keepFromFrame);
    Q_UNUSED(nowMs);
}

void PlaybackFramePipeline::schedulePlaybackWindow(const ClipInfo& info,
                                                   int64_t samplePosition,
                                                   int64_t canonicalFrame,
                                                   const QVector<RenderSyncMarker>& markers,
                                                   const std::function<void()>& onFrameReady) {
    const bool isSequenceClip = isImageSequencePlaybackClip(info.clip);
    const int playbackWindowAhead = isSequenceClip
                                        ? debugPlaybackWindowAhead()
                                        : static_cast<int>(debugFileVideoPlaybackWindowAhead());
    if (info.isSingleFrame || !m_active.load() || playbackWindowAhead < 0) {
        return;
    }
    if (!m_decoder && !m_dispatcher) {
        return;
    }

    const int64_t keepWindow = isSequenceClip
                                   ? debugSequenceVisibleDecodeKeepWindow()
                                   : debugVisibleDecodeKeepWindow();
    const int64_t obsoleteVisibleFrameSlack = isSequenceClip
                                                  ? debugSequenceObsoleteVisibleFrameSlack()
                                                  : debugObsoleteVisibleFrameSlack();
    const int64_t lateBufferSeedSlack = isSequenceClip
                                            ? debugSequenceLateBufferSeedSlack()
                                            : obsoleteVisibleFrameSlack;

    // Cancel-before via dispatcher (or legacy path)
    if (m_dispatcher) {
        m_dispatcher->cancelBefore(info.playbackPath,
                                   qMax<int64_t>(0, canonicalFrame - keepWindow));
    } else {
        cancelDecoderBeforeThrottled(info.playbackPath,
                                     qMax<int64_t>(0, canonicalFrame - keepWindow),
                                     QDateTime::currentMSecsSinceEpoch());
    }

    for (int offset = 0; offset <= playbackWindowAhead; ++offset) {
        const int64_t targetFrame = offset == 0
                                        ? canonicalFrame
                                        : normalizeFrameNumber(
                                              info,
                                              sourceFrameForClipAtTimelineSample(
                                                  info.clip,
                                                  samplePosition + frameToSamples(offset),
                                                  markers));
        const QString key = requestKey(info.clip.id, targetFrame);
        if (isFrameBuffered(info.clip.id, targetFrame)) {
            continue;
        }

        // Check if already pending (via dispatcher or legacy pending sets)
        if (m_dispatcher) {
            if (m_dispatcher->isPending(info.playbackPath, targetFrame)) {
                continue;
            }
            // Proximity dedup via dispatcher
            if (offset == 0 && obsoleteVisibleFrameSlack > 0 &&
                m_dispatcher->isNearbyPending(info.playbackPath, targetFrame,
                                              static_cast<int>(obsoleteVisibleFrameSlack))) {
                recordFrameTraceEvent(QStringLiteral("nearby_pending_skip"),
                                      info.clip.id, targetFrame,
                                      QStringLiteral("slack=%1").arg(obsoleteVisibleFrameSlack));
                playbackPipelineTrace(QStringLiteral("schedulePlaybackWindow.nearby-pending-skip"),
                                      QStringLiteral("clip=%1 target=%2 slack=%3")
                                          .arg(info.clip.id)
                                          .arg(targetFrame)
                                          .arg(obsoleteVisibleFrameSlack));
                continue;
            }
        } else {
            // Legacy path: check local pending sets
            QMutexLocker pendingLock(&m_pendingMutex);
            if (m_pendingVisibleRequests.contains(key) || m_pendingPrefetchRequests.contains(key)) {
                continue;
            }
            // Proximity dedup on local pending sets
            if (offset == 0 && obsoleteVisibleFrameSlack > 0) {
                const QString clipPrefix = info.clip.id + QLatin1Char(':');
                bool nearbyPending = false;
                for (auto it = m_pendingVisibleRequests.cbegin(); it != m_pendingVisibleRequests.cend(); ++it) {
                    if (!it->startsWith(clipPrefix)) {
                        continue;
                    }
                    const QStringView ref = QStringView(*it).mid(clipPrefix.length());
                    bool ok = false;
                    const int64_t pendingFrame = ref.toLongLong(&ok);
                    if (ok && qAbs(pendingFrame - targetFrame) <= obsoleteVisibleFrameSlack) {
                        nearbyPending = true;
                        break;
                    }
                }
                if (nearbyPending) {
                    recordFrameTraceEvent(QStringLiteral("nearby_pending_skip"),
                                          info.clip.id, targetFrame,
                                          QStringLiteral("slack=%1").arg(obsoleteVisibleFrameSlack));
                    playbackPipelineTrace(QStringLiteral("schedulePlaybackWindow.nearby-pending-skip"),
                                          QStringLiteral("clip=%1 target=%2 slack=%3")
                                              .arg(info.clip.id)
                                              .arg(targetFrame)
                                              .arg(obsoleteVisibleFrameSlack));
                    continue;
                }
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
        const int priority = kind == DecodeRequestKind::Visible ? 100 : qMax(80, 99 - offset);
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
                // Remove from local pending sets (used both in legacy path and
                // when the dispatcher rate-limited the request and we tracked it locally).
                {
                    QMutexLocker pendingLock(&self->m_pendingMutex);
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
                const bool obsoleteForPresentation =
                    !frame.isNull() &&
                    targetFrame < qMax<int64_t>(0, playheadFrame - obsoleteVisibleFrameSlack);
                const bool bufferableLateCompletion =
                    !frame.isNull() &&
                    targetFrame >= qMax<int64_t>(0, playheadFrame - lateBufferSeedSlack);

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

        // Dispatch via dispatcher or legacy decoder path
        if (m_dispatcher) {
            const bool accepted = m_dispatcher->requestFrame(
                info.playbackPath,
                targetFrame,
                priority,
                kind == DecodeRequestKind::Visible ? 30000 : 12000,
                kind,
                std::move(completionCallback));
            if (!accepted) {
                // Rate-limited or rejected — track locally so we don't
                // retry this frame on every tick.
                QMutexLocker pendingLock(&m_pendingMutex);
                if (kind == DecodeRequestKind::Visible) {
                    m_pendingVisibleRequests.insert(key);
                    m_latestVisibleTargets.insert(info.clip.id, targetFrame);
                } else {
                    m_pendingPrefetchRequests.insert(key);
                }
                recordFrameTraceEvent(QStringLiteral("rate_limited"),
                                      info.clip.id, targetFrame,
                                      QStringLiteral("canonical=%1").arg(canonicalFrame));
            }
        } else {
            m_decoder->requestFrame(info.playbackPath,
                                    targetFrame,
                                    priority,
                                    kind == DecodeRequestKind::Visible ? 30000 : 12000,
                                    kind,
                                    std::move(completionCallback));
        }
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
