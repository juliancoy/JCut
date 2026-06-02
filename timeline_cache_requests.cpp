#include "timeline_cache.h"

#include "debug_controls.h"
#include "editor_shared_timing.h"
#include "preview_frame_selection.h"

#include <QDateTime>
#include <QElapsedTimer>
#include <QHash>
#include <QMetaObject>
#include <QMutexLocker>
#include <QPointer>

#include <algorithm>
#include <atomic>
#include <limits>
#include <memory>
#include <mutex>

namespace editor {

namespace {
constexpr int64_t kObsoleteVisibleFrameSlack = 0;
constexpr qint64 kVisiblePendingRetryMs = 2000;

QElapsedTimer& cacheTraceTimer() {
    static QElapsedTimer timer = []() {
        QElapsedTimer t;
        t.start();
        return t;
    }();
    return timer;
}

qint64 cacheTraceMs() {
    return cacheTraceTimer().elapsed();
}

void cacheTrace(const QString& stage, const QString& detail = QString()) {
    if (debugCacheLevel() < DebugLogLevel::Info) {
        return;
    }

    static std::mutex logMutex;
    static QHash<QString, qint64> lastLogByStage;

    const qint64 now = cacheTraceMs();
    if (!debugCacheVerboseEnabled()) {
        const bool isHighFreqStage =
            stage.startsWith(QStringLiteral("TimelineCache::requestFrame.miss")) ||
            stage.startsWith(QStringLiteral("TimelineCache::requestFrame.dispatch")) ||
            stage.startsWith(QStringLiteral("TimelineCache::requestFrame.complete"));
        if (isHighFreqStage) {
            std::lock_guard<std::mutex> lock(logMutex);
            const qint64 last = lastLogByStage.value(stage, std::numeric_limits<qint64>::min());
            if (now - last < 500) {
                return;
            }
            lastLogByStage.insert(stage, now);
        }
    }

    qDebug().noquote() << QStringLiteral("[CACHE %1 ms] %2%3")
                              .arg(now, 6)
                              .arg(stage)
                              .arg(detail.isEmpty() ? QString() : QStringLiteral(" | ") + detail);
}

void cacheWarnTrace(const QString& stage, const QString& detail = QString()) {
    if (!debugCacheWarnEnabled()) {
        return;
    }
    const qint64 now = cacheTraceMs();
    qDebug().noquote() << QStringLiteral("[CACHE][WARN] %1 %2%3")
                              .arg(now, 6)
                              .arg(stage)
                              .arg(detail.isEmpty() ? QString() : QStringLiteral(" | ") + detail);
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
}  // namespace

void TimelineCache::requestFrame(const QString& clipId,
                                 int64_t frameNumber,
                                 std::function<void(FrameHandle)> callback,
                                 bool requireHardwareOrGpuPayload) {
    m_requests++;
    const qint64 requestedAtTraceMs = cacheTraceMs();
    const qint64 requestedAtWallMs = QDateTime::currentMSecsSinceEpoch();
    const int64_t normalizedFrame = normalizeFrameNumber(clipId, frameNumber);

    FrameHandle cached;
    if (m_state.load() == PlaybackState::Playing) {
        cached = getPlaybackFrame(clipId, normalizedFrame);
        if (cached.isNull()) {
            cached = getCachedFrame(clipId, normalizedFrame);
        }
    } else {
        cached = getCachedFrame(clipId, normalizedFrame);
    }
    if (!cached.isNull() &&
        (!requireHardwareOrGpuPayload || cached.hasHardwareFrame() || cached.hasGpuTexture())) {
        m_hits++;
        cacheTrace(QStringLiteral("TimelineCache::requestFrame.hit"),
                   QStringLiteral("clip=%1 frame=%2 normalized=%3")
                       .arg(clipId)
                       .arg(frameNumber)
                       .arg(normalizedFrame));
        callback(cached);
        return;
    }

    cacheTrace(QStringLiteral("TimelineCache::requestFrame.miss"),
               QStringLiteral("clip=%1 frame=%2 normalized=%3")
                   .arg(clipId)
                   .arg(frameNumber)
                   .arg(normalizedFrame));
    if (debugCacheWarnOnlyEnabled()) {
        cacheWarnTrace(QStringLiteral("TimelineCache::visible-miss"),
                       QStringLiteral("clip=%1 frame=%2 normalized=%3")
                           .arg(clipId)
                           .arg(frameNumber)
                           .arg(normalizedFrame));
    }

    QMutexLocker lock(&m_clipsMutex);
    auto it = m_clips.find(clipId);
    if (it == m_clips.end()) {
        cacheTrace(QStringLiteral("TimelineCache::requestFrame.missing-clip"),
                   QStringLiteral("clip=%1 frame=%2").arg(clipId).arg(normalizedFrame));
        callback(FrameHandle());
        return;
    }

    const ClipInfo info = it.value();
    lock.unlock();

    const int64_t canonicalFrame = normalizeFrameNumber(info, normalizedFrame);
    if (info.decodePath.isEmpty()) {
        callback(FrameHandle());
        return;
    }

    const QString key = requestKey(clipId, canonicalFrame);
    const uint64_t requestGeneration = m_visibleRequestGeneration.load();
    constexpr qint64 kPendingRequestStaleMs = kVisiblePendingRetryMs;

    {
        QMutexLocker pendingLock(&m_pendingMutex);
        m_latestVisibleTargets.insert(clipId, canonicalFrame);

        auto existing = m_pendingVisibleRequests.find(key);
        if (existing != m_pendingVisibleRequests.end()) {
            const bool generationChanged = existing->generation != requestGeneration;
            const bool staleByAge =
                existing->requestedAtMs > 0 &&
                (requestedAtWallMs - existing->requestedAtMs) > kPendingRequestStaleMs;
            if (!generationChanged && !staleByAge) {
                existing->callbacks.push_back(std::move(callback));
                cacheTrace(QStringLiteral("TimelineCache::requestFrame.dedup"),
                           QStringLiteral("clip=%1 frame=%2 normalized=%3 listeners=%4")
                               .arg(clipId)
                               .arg(frameNumber)
                               .arg(canonicalFrame)
                               .arg(existing->callbacks.size()));
                return;
            }

            QVector<std::function<void(FrameHandle)>> mergedCallbacks = std::move(existing->callbacks);
            mergedCallbacks.push_back(std::move(callback));
            m_pendingVisibleRequests.erase(existing);

            PendingVisibleRequest pending;
            pending.callbacks = std::move(mergedCallbacks);
            pending.generation = requestGeneration;
            pending.requestedAtMs = requestedAtWallMs;
            pending.dispatchedAtMs = requestedAtWallMs;
            m_pendingVisibleRequests.insert(key, std::move(pending));
            m_pendingPrefetchRequests.remove(key);

            cacheTrace(QStringLiteral("TimelineCache::requestFrame.refresh"),
                       QStringLiteral("clip=%1 frame=%2 normalized=%3 generationChanged=%4 staleByAge=%5")
                           .arg(clipId)
                           .arg(frameNumber)
                           .arg(canonicalFrame)
                           .arg(generationChanged)
                           .arg(staleByAge));
        } else {
            PendingVisibleRequest pending;
            pending.callbacks.push_back(std::move(callback));
            pending.generation = requestGeneration;
            pending.requestedAtMs = requestedAtWallMs;
            pending.dispatchedAtMs = requestedAtWallMs;
            m_pendingVisibleRequests.insert(key, std::move(pending));
            m_pendingPrefetchRequests.remove(key);
        }
    }

    if (!m_decoder) {
        return;
    }

    if (m_state.load() == PlaybackState::Playing && !info.isSingleFrame) {
        const int64_t keepFromFrame = qMax<int64_t>(0, canonicalFrame - effectiveVisibleDecodeKeepWindow());
        cancelDecoderBeforeThrottled(info.decodePath, keepFromFrame, requestedAtWallMs);
    }

    const int priority = calculatePriority(info, canonicalFrame);
    QPointer<TimelineCache> self(this);
    const std::shared_ptr<std::atomic<bool>> aliveToken = m_aliveToken;

    cacheTrace(QStringLiteral("TimelineCache::requestFrame.dispatch"),
               QStringLiteral("clip=%1 frame=%2 normalized=%3 priority=%4")
                   .arg(clipId)
                   .arg(frameNumber)
                   .arg(canonicalFrame)
                   .arg(priority));
    {
        QMutexLocker diagnosticsLock(&m_visibleDecodeDiagnosticsMutex);
        ++m_visibleDecodeDiagnostics.dispatched;
        m_visibleDecodeDiagnostics.lastClipId = clipId;
        m_visibleDecodeDiagnostics.lastRequestFrame = canonicalFrame;
        m_visibleDecodeDiagnostics.lastOutcome = QStringLiteral("dispatched");
    }

    const uint64_t seqId = m_decoder->requestFrame(
        info.decodePath,
        canonicalFrame,
        priority,
        30000,
        DecodeRequestKind::Visible,
        [self,
         aliveToken,
         clipId,
         canonicalFrame,
         requestedAtTraceMs,
         requestedAtWallMs,
         key,
         requestGeneration,
         requireHardwareOrGpuPayload](FrameHandle frame) {
            if (!aliveToken->load() || !self) {
                return;
            }
            const qint64 decoderCallbackAtMs = QDateTime::currentMSecsSinceEpoch();

            QMetaObject::invokeMethod(
                self,
                [self,
                 aliveToken,
                 clipId,
                 canonicalFrame,
                 requestedAtTraceMs,
                 requestedAtWallMs,
                 decoderCallbackAtMs,
                 key,
                 frame,
                 requestGeneration,
                 requireHardwareOrGpuPayload]() {
                    if (!aliveToken->load() || !self) {
                        return;
                    }

                    FrameHandle deliveredFrame = frame;
                    const QString rawPayload = framePayloadForDiagnostics(deliveredFrame);
                    if (requireHardwareOrGpuPayload &&
                        !deliveredFrame.isNull() &&
                        !deliveredFrame.hasHardwareFrame() &&
                        !deliveredFrame.hasGpuTexture()) {
                        cacheWarnTrace(
                            QStringLiteral("TimelineCache::visible-strict-payload-rejected"),
                            QStringLiteral("clip=%1 frame=%2 payload=cpu_image")
                                .arg(clipId)
                                .arg(canonicalFrame));
                        deliveredFrame = FrameHandle();
                        QMutexLocker diagnosticsLock(&self->m_visibleDecodeDiagnosticsMutex);
                        ++self->m_visibleDecodeDiagnostics.strictPayloadRejected;
                    }

                    int64_t latestVisibleTarget = canonicalFrame;
                    bool obsoleteVisibleRequest = false;
                    {
                        QMutexLocker pendingLock(&self->m_pendingMutex);
                        latestVisibleTarget = self->m_latestVisibleTargets.value(clipId, canonicalFrame);
                        obsoleteVisibleRequest =
                            self->m_state.load() == PlaybackState::Playing &&
                            canonicalFrame + kObsoleteVisibleFrameSlack < latestVisibleTarget;
                    }

                    if (!deliveredFrame.isNull()) {
                        const int64_t deliveredFrameNumber =
                            deliveredFrame.frameNumber() >= 0 ? deliveredFrame.frameNumber() : canonicalFrame;
                        {
                            QMutexLocker pendingLock(&self->m_pendingMutex);
                            self->m_seekResync.satisfy(clipId,
                                                       deliveredFrameNumber,
                                                       QDateTime::currentMSecsSinceEpoch());
                        }
                        if (self->m_state.load() == PlaybackState::Playing) {
                            auto bufferIt = self->m_playbackBuffers.find(clipId);
                            if (bufferIt != self->m_playbackBuffers.end() && bufferIt.value()) {
                                bufferIt.value()->insert(deliveredFrameNumber, deliveredFrame);
                            }
                        }
                        if (auto* cache = self->getOrCreateClipCache(clipId)) {
                            cache->insert(deliveredFrameNumber, deliveredFrame);
                        }
                        if (deliveredFrame.hasHardwareFrame()) {
                            self->enforceHardwareFrameResidencyPolicy();
                        }
                    }

                    QVector<std::function<void(FrameHandle)>> callbacks;
                    bool completionStaleByGeneration = false;
                    {
                        QMutexLocker pendingLock(&self->m_pendingMutex);
                        auto it = self->m_pendingVisibleRequests.find(key);
                        if (it != self->m_pendingVisibleRequests.end()) {
                            if (it->generation == requestGeneration) {
                                callbacks = std::move(it->callbacks);
                                self->m_pendingVisibleRequests.erase(it);
                            } else {
                                completionStaleByGeneration = true;
                            }
                        }
                        if (canonicalFrame >= latestVisibleTarget) {
                            self->m_latestVisibleTargets.remove(clipId);
                        }
                    }

                    if (completionStaleByGeneration) {
                        deliveredFrame = FrameHandle();
                    }

                    const qint64 completedAtMs = QDateTime::currentMSecsSinceEpoch();
                    const qint64 wallWaitMs =
                        requestedAtWallMs > 0 ? completedAtMs - requestedAtWallMs : -1;
                    const qint64 qtDeliveryDelayMs =
                        decoderCallbackAtMs > 0 ? completedAtMs - decoderCallbackAtMs : -1;
                    {
                        QMutexLocker diagnosticsLock(&self->m_visibleDecodeDiagnosticsMutex);
                        auto& diagnostics = self->m_visibleDecodeDiagnostics;
                        ++diagnostics.completed;
                        diagnostics.lastClipId = clipId;
                        diagnostics.lastRequestFrame = canonicalFrame;
                        diagnostics.lastCompletedFrame = deliveredFrame.isNull()
                                                             ? static_cast<qint64>(-1)
                                                             : static_cast<qint64>(deliveredFrame.frameNumber());
                        diagnostics.lastCallbackWaitMs = wallWaitMs;
                        diagnostics.maxCallbackWaitMs = qMax(diagnostics.maxCallbackWaitMs, wallWaitMs);
                        diagnostics.lastQtDeliveryDelayMs = qtDeliveryDelayMs;
                        diagnostics.maxQtDeliveryDelayMs =
                            qMax(diagnostics.maxQtDeliveryDelayMs, qtDeliveryDelayMs);
                        diagnostics.lastCompletedAtMs = completedAtMs;
                        diagnostics.lastPayload = completionStaleByGeneration
                                                      ? QStringLiteral("stale_generation")
                                                      : rawPayload;
                        if (completionStaleByGeneration) {
                            ++diagnostics.staleGenerationCompleted;
                            diagnostics.lastOutcome = QStringLiteral("stale_generation");
                        } else if (deliveredFrame.isNull()) {
                            ++diagnostics.nullCompleted;
                            diagnostics.lastOutcome = QStringLiteral("null");
                        } else if (deliveredFrame.hasHardwareFrame()) {
                            ++diagnostics.hardwareCompleted;
                            diagnostics.lastOutcome = QStringLiteral("stored");
                        } else if (deliveredFrame.hasGpuTexture()) {
                            ++diagnostics.gpuTextureCompleted;
                            diagnostics.lastOutcome = QStringLiteral("stored");
                        } else {
                            ++diagnostics.cpuCompleted;
                            diagnostics.lastOutcome = QStringLiteral("stored");
                        }
                    }

                    cacheTrace(QStringLiteral("TimelineCache::requestFrame.complete"),
                               QStringLiteral("clip=%1 frame=%2 null=%3 waitMs=%4")
                                   .arg(clipId)
                                   .arg(canonicalFrame)
                                   .arg(deliveredFrame.isNull())
                                   .arg(cacheTraceMs() - requestedAtTraceMs));

                    const qint64 waitMs = cacheTraceMs() - requestedAtTraceMs;
                    if (debugCacheWarnOnlyEnabled()) {
                        if (obsoleteVisibleRequest && !deliveredFrame.isNull()) {
                            cacheWarnTrace(QStringLiteral("TimelineCache::visible-late-kept"),
                                           QStringLiteral("clip=%1 frame=%2 latest=%3 waitMs=%4 listeners=%5")
                                               .arg(clipId)
                                               .arg(canonicalFrame)
                                               .arg(latestVisibleTarget)
                                               .arg(waitMs)
                                               .arg(callbacks.size()));
                        } else if (deliveredFrame.isNull() || waitMs > 33) {
                            cacheWarnTrace(QStringLiteral("TimelineCache::visible-complete"),
                                           QStringLiteral("clip=%1 frame=%2 null=%3 waitMs=%4 listeners=%5")
                                               .arg(clipId)
                                               .arg(canonicalFrame)
                                               .arg(deliveredFrame.isNull())
                                               .arg(waitMs)
                                               .arg(callbacks.size()));
                        }
                    }

                    for (const auto& cb : callbacks) {
                        if (cb) {
                            cb(deliveredFrame);
                        }
                    }
                },
                Qt::QueuedConnection);
        });

    if (seqId == 0 && debugCacheWarnOnlyEnabled()) {
        cacheWarnTrace(QStringLiteral("TimelineCache::visible-rejected"),
                       QStringLiteral("clip=%1 frame=%2 normalized=%3 priority=%4 pending=%5")
                           .arg(clipId)
                           .arg(frameNumber)
                           .arg(canonicalFrame)
                           .arg(priority)
                           .arg(m_decoder->pendingRequestCount()));
    }
    if (seqId == 0) {
        QVector<std::function<void(FrameHandle)>> callbacks;
        {
            QMutexLocker diagnosticsLock(&m_visibleDecodeDiagnosticsMutex);
            ++m_visibleDecodeDiagnostics.decoderRejected;
            m_visibleDecodeDiagnostics.lastClipId = clipId;
            m_visibleDecodeDiagnostics.lastRequestFrame = canonicalFrame;
            m_visibleDecodeDiagnostics.lastPayload = QStringLiteral("null");
            m_visibleDecodeDiagnostics.lastOutcome = QStringLiteral("decoder_rejected");
        }
        {
            QMutexLocker pendingLock(&m_pendingMutex);
            auto it = m_pendingVisibleRequests.find(key);
            if (it != m_pendingVisibleRequests.end() && it->generation == requestGeneration) {
                callbacks = std::move(it->callbacks);
                m_pendingVisibleRequests.erase(it);
            }
        }
        for (const auto& cb : callbacks) {
            if (cb) {
                cb(FrameHandle());
            }
        }
    }

    scheduleImmediateLeadPrefetch(info, canonicalFrame);
}

bool TimelineCache::hasDisplayableFrameForPreview(const QString& clipId,
                                                  int64_t frameNumber,
                                                  bool preferPlaybackBuffer,
                                                  bool allowCacheFallback,
                                                  bool requireHardwareOrGpuPayload) {
    frameNumber = normalizeFrameNumber(clipId, frameNumber);
    const bool allowApproximateFrame =
        shouldAllowApproximatePreviewFrame(clipId, frameNumber, QDateTime::currentMSecsSinceEpoch());
    QMutexLocker lock(&m_clipsMutex);

    PlaybackBuffer* playbackBuffer = nullptr;
    auto playbackIt = m_playbackBuffers.find(clipId);
    if (playbackIt != m_playbackBuffers.end()) {
        playbackBuffer = playbackIt.value();
    }

    ClipCache* cache = nullptr;
    auto cacheIt = m_caches.find(clipId);
    if (cacheIt != m_caches.end()) {
        cache = cacheIt.value();
    }

    qreal sourceFps = static_cast<qreal>(kTimelineFps);
    const auto clipInfoIt = m_clips.find(clipId);
    if (clipInfoIt != m_clips.end()) {
        sourceFps = resolvedSourceFps(clipInfoIt->clip);
    }
    const int64_t maxStaleFrameDelta = previewMaxPlaybackStaleFrameDelta(sourceFps);

    const auto isDisplayableCandidate = [this,
                                         frameNumber,
                                         requireHardwareOrGpuPayload,
                                         maxStaleFrameDelta](const FrameHandle& frame) {
        if (frame.isNull()) {
            return false;
        }
        if (requireHardwareOrGpuPayload && !frame.hasHardwareFrame() && !frame.hasGpuTexture()) {
            return false;
        }
        if (m_state.load() != PlaybackState::Playing) {
            return true;
        }
        return !previewFrameIsTooStaleForPlayback(frame, frameNumber, maxStaleFrameDelta);
    };

    if (preferPlaybackBuffer && playbackBuffer) {
        const FrameHandle playbackExact = playbackBuffer->get(frameNumber);
        if (isDisplayableCandidate(playbackExact)) {
            return true;
        }
        if ((!requireHardwareOrGpuPayload || m_state.load() == PlaybackState::Playing) &&
            allowApproximateFrame &&
            isDisplayableCandidate(playbackBuffer->getLatestAtOrBefore(frameNumber))) {
            return true;
        }
    }

    if (allowCacheFallback && cache) {
        const FrameHandle cacheExact = cache->get(frameNumber);
        if (isDisplayableCandidate(cacheExact)) {
            return true;
        }
        if ((!requireHardwareOrGpuPayload || m_state.load() == PlaybackState::Playing) &&
            allowApproximateFrame &&
            isDisplayableCandidate(cache->getLatestAtOrBefore(frameNumber))) {
            return true;
        }
    }

    return false;
}

bool TimelineCache::hasExactFrameForPreview(const QString& clipId,
                                            int64_t frameNumber,
                                            bool preferPlaybackBuffer,
                                            bool allowCacheFallback,
                                            bool requireHardwareOrGpuPayload) {
    frameNumber = normalizeFrameNumber(clipId, frameNumber);

    const auto isUsableExactFrame = [frameNumber, requireHardwareOrGpuPayload](const FrameHandle& frame) {
        if (frame.isNull()) {
            return false;
        }
        if (frame.frameNumber() >= 0 && frame.frameNumber() != frameNumber) {
            return false;
        }
        return !requireHardwareOrGpuPayload || frame.hasHardwareFrame() || frame.hasGpuTexture();
    };

    if (preferPlaybackBuffer && isUsableExactFrame(getPlaybackFrame(clipId, frameNumber))) {
        return true;
    }
    return allowCacheFallback && isUsableExactFrame(getCachedFrame(clipId, frameNumber));
}

int TimelineCache::pendingVisibleRequestCount() const {
    QMutexLocker pendingLock(&m_pendingMutex);
    return m_pendingVisibleRequests.size();
}

bool TimelineCache::isVisibleRequestPending(const QString& clipId, int64_t frameNumber) const {
    frameNumber = normalizeFrameNumber(clipId, frameNumber);
    QMutexLocker pendingLock(&m_pendingMutex);
    return m_pendingVisibleRequests.contains(requestKey(clipId, frameNumber));
}

bool TimelineCache::shouldForceVisibleRequestRetry(const QString& clipId,
                                                   int64_t frameNumber,
                                                   qint64 staleAfterMs) const {
    frameNumber = normalizeFrameNumber(clipId, frameNumber);
    const QString key = requestKey(clipId, frameNumber);
    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    QMutexLocker pendingLock(&m_pendingMutex);
    const auto it = m_pendingVisibleRequests.constFind(key);
    if (it == m_pendingVisibleRequests.constEnd()) {
        return true;
    }
    if (it->requestedAtMs <= 0) {
        return true;
    }
    return (now - it->requestedAtMs) >= qMax<qint64>(1, staleAfterMs);
}

bool TimelineCache::shouldAllowApproximatePreviewFrame(const QString& clipId,
                                                       int64_t frameNumber,
                                                       qint64 nowMs) const {
    frameNumber = normalizeFrameNumber(clipId, frameNumber);
    QMutexLocker pendingLock(&m_pendingMutex);
    return m_seekResync.shouldAllowApproximate(clipId, frameNumber, nowMs);
}

QJsonArray TimelineCache::pendingVisibleDebugSnapshot(qint64 nowMs, int limit) const {
    struct PendingDebugEntry {
        QString key;
        int callbackCount = 0;
        uint64_t generation = 0;
        qint64 requestedAtMs = 0;
    };

    QVector<PendingDebugEntry> entries;
    {
        QMutexLocker pendingLock(&m_pendingMutex);
        entries.reserve(m_pendingVisibleRequests.size());
        for (auto it = m_pendingVisibleRequests.cbegin(); it != m_pendingVisibleRequests.cend(); ++it) {
            PendingDebugEntry entry;
            entry.key = it.key();
            entry.callbackCount = it->callbacks.size();
            entry.generation = it->generation;
            entry.requestedAtMs = it->requestedAtMs;
            entries.push_back(entry);
        }
    }

    std::sort(entries.begin(), entries.end(), [](const PendingDebugEntry& a, const PendingDebugEntry& b) {
        return a.requestedAtMs < b.requestedAtMs;
    });

    QJsonArray snapshot;
    const int cappedLimit = qMax(0, limit);
    for (const PendingDebugEntry& entry : entries) {
        if (snapshot.size() >= cappedLimit) {
            break;
        }
        const int separator = entry.key.indexOf(QLatin1Char(':'));
        const QString clipId = separator > 0 ? entry.key.left(separator) : QString();
        bool ok = false;
        const int64_t frameNumber =
            separator > 0 ? entry.key.mid(separator + 1).toLongLong(&ok) : -1;
        snapshot.append(QJsonObject{
            {QStringLiteral("key"), entry.key},
            {QStringLiteral("clip_id"), clipId},
            {QStringLiteral("frame_number"), ok ? static_cast<qint64>(frameNumber) : static_cast<qint64>(-1)},
            {QStringLiteral("callback_count"), entry.callbackCount},
            {QStringLiteral("generation"), static_cast<qint64>(entry.generation)},
            {QStringLiteral("requested_at_ms"), entry.requestedAtMs},
            {QStringLiteral("age_ms"), entry.requestedAtMs > 0 ? nowMs - entry.requestedAtMs : static_cast<qint64>(-1)}
        });
    }
    return snapshot;
}

QJsonObject TimelineCache::visibleDecodeDiagnostics(qint64 nowMs) const {
    int pendingCount = 0;
    qint64 oldestPendingAgeMs = -1;
    {
        QMutexLocker pendingLock(&m_pendingMutex);
        pendingCount = m_pendingVisibleRequests.size();
        for (auto it = m_pendingVisibleRequests.cbegin(); it != m_pendingVisibleRequests.cend(); ++it) {
            if (it->requestedAtMs <= 0) {
                continue;
            }
            const qint64 ageMs = nowMs - it->requestedAtMs;
            if (oldestPendingAgeMs < 0 || ageMs > oldestPendingAgeMs) {
                oldestPendingAgeMs = ageMs;
            }
        }
    }

    const QJsonObject retentionPolicy = visibleDecodeRetentionPolicySnapshot(nowMs);
    QMutexLocker diagnosticsLock(&m_visibleDecodeDiagnosticsMutex);
    return QJsonObject{
        {QStringLiteral("pending_count"), pendingCount},
        {QStringLiteral("oldest_pending_age_ms"), oldestPendingAgeMs},
        {QStringLiteral("dispatched"), static_cast<qint64>(m_visibleDecodeDiagnostics.dispatched)},
        {QStringLiteral("completed"), static_cast<qint64>(m_visibleDecodeDiagnostics.completed)},
        {QStringLiteral("null_completed"), static_cast<qint64>(m_visibleDecodeDiagnostics.nullCompleted)},
        {QStringLiteral("hardware_completed"), static_cast<qint64>(m_visibleDecodeDiagnostics.hardwareCompleted)},
        {QStringLiteral("gpu_texture_completed"), static_cast<qint64>(m_visibleDecodeDiagnostics.gpuTextureCompleted)},
        {QStringLiteral("cpu_completed"), static_cast<qint64>(m_visibleDecodeDiagnostics.cpuCompleted)},
        {QStringLiteral("strict_payload_rejected"),
         static_cast<qint64>(m_visibleDecodeDiagnostics.strictPayloadRejected)},
        {QStringLiteral("stale_generation_completed"),
         static_cast<qint64>(m_visibleDecodeDiagnostics.staleGenerationCompleted)},
        {QStringLiteral("decoder_rejected"), static_cast<qint64>(m_visibleDecodeDiagnostics.decoderRejected)},
        {QStringLiteral("last_clip_id"), m_visibleDecodeDiagnostics.lastClipId},
        {QStringLiteral("last_request_frame"), m_visibleDecodeDiagnostics.lastRequestFrame},
        {QStringLiteral("last_completed_frame"), m_visibleDecodeDiagnostics.lastCompletedFrame},
        {QStringLiteral("last_payload"), m_visibleDecodeDiagnostics.lastPayload},
        {QStringLiteral("last_outcome"), m_visibleDecodeDiagnostics.lastOutcome},
        {QStringLiteral("last_callback_wait_ms"), m_visibleDecodeDiagnostics.lastCallbackWaitMs},
        {QStringLiteral("max_callback_wait_ms"), m_visibleDecodeDiagnostics.maxCallbackWaitMs},
        {QStringLiteral("last_qt_delivery_delay_ms"), m_visibleDecodeDiagnostics.lastQtDeliveryDelayMs},
        {QStringLiteral("max_qt_delivery_delay_ms"), m_visibleDecodeDiagnostics.maxQtDeliveryDelayMs},
        {QStringLiteral("retention_policy"), retentionPolicy},
        {QStringLiteral("last_completed_age_ms"),
         m_visibleDecodeDiagnostics.lastCompletedAtMs > 0
             ? nowMs - m_visibleDecodeDiagnostics.lastCompletedAtMs
             : static_cast<qint64>(-1)}
    };
}

}  // namespace editor
