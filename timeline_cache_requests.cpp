#include "timeline_cache.h"

#include "debug_controls.h"

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
constexpr int64_t kVisibleDecodeKeepWindow = 10;
constexpr int64_t kObsoleteVisibleFrameSlack = 0;
constexpr qint64 kVisiblePendingRetryMs = 250;

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
}  // namespace

void TimelineCache::requestFrame(const QString& clipId,
                                 int64_t frameNumber,
                                 std::function<void(FrameHandle)> callback) {
    m_requests++;
    const qint64 requestedAtTraceMs = cacheTraceMs();
    const qint64 requestedAtWallMs = QDateTime::currentMSecsSinceEpoch();
    const int64_t normalizedFrame = normalizeFrameNumber(clipId, frameNumber);

    FrameHandle cached = m_state.load() == PlaybackState::Playing
                             ? getPlaybackFrame(clipId, normalizedFrame)
                             : getCachedFrame(clipId, normalizedFrame);
    if (!cached.isNull()) {
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
            m_pendingVisibleRequests.insert(key, std::move(pending));
            m_pendingPrefetchRequests.remove(key);
        }
    }

    if (!m_decoder) {
        return;
    }

    if (m_state.load() == PlaybackState::Playing && !info.isSingleFrame) {
        const int64_t keepFromFrame = qMax<int64_t>(0, canonicalFrame - kVisibleDecodeKeepWindow);
        cancelDecoderBeforeThrottled(info.decodePath, keepFromFrame, requestedAtWallMs);
    }

    const int priority = calculatePriority(canonicalFrame);
    QPointer<TimelineCache> self(this);
    const std::shared_ptr<std::atomic<bool>> aliveToken = m_aliveToken;

    cacheTrace(QStringLiteral("TimelineCache::requestFrame.dispatch"),
               QStringLiteral("clip=%1 frame=%2 normalized=%3 priority=%4")
                   .arg(clipId)
                   .arg(frameNumber)
                   .arg(canonicalFrame)
                   .arg(priority));

    const uint64_t seqId = m_decoder->requestFrame(
        info.decodePath,
        canonicalFrame,
        priority,
        10000,
        DecodeRequestKind::Visible,
        [self, aliveToken, clipId, canonicalFrame, requestedAtTraceMs, key, requestGeneration](FrameHandle frame) {
            if (!aliveToken->load() || !self) {
                return;
            }

            QMetaObject::invokeMethod(
                self,
                [self, aliveToken, clipId, canonicalFrame, requestedAtTraceMs, key, frame, requestGeneration]() {
                    if (!aliveToken->load() || !self) {
                        return;
                    }

                    FrameHandle deliveredFrame = frame;

                    int64_t latestVisibleTarget = canonicalFrame;
                    bool obsoleteVisibleCompletion = false;
                    bool obsoleteVisibleRequest = false;
                    {
                        QMutexLocker pendingLock(&self->m_pendingMutex);
                        latestVisibleTarget = self->m_latestVisibleTargets.value(clipId, canonicalFrame);
                        obsoleteVisibleRequest =
                            self->m_state.load() == PlaybackState::Playing &&
                            canonicalFrame + kObsoleteVisibleFrameSlack < latestVisibleTarget;
                        obsoleteVisibleCompletion =
                            obsoleteVisibleRequest &&
                            !deliveredFrame.isNull() &&
                            canonicalFrame + kObsoleteVisibleFrameSlack < latestVisibleTarget;
                    }

                    if (obsoleteVisibleCompletion) {
                        cacheTrace(QStringLiteral("TimelineCache::requestFrame.obsolete-complete"),
                                   QStringLiteral("clip=%1 frame=%2 latest=%3 waitMs=%4")
                                       .arg(clipId)
                                       .arg(canonicalFrame)
                                       .arg(latestVisibleTarget)
                                       .arg(cacheTraceMs() - requestedAtTraceMs));
                        deliveredFrame = FrameHandle();
                    }

                    if (!deliveredFrame.isNull()) {
                        {
                            QMutexLocker pendingLock(&self->m_pendingMutex);
                            self->m_seekResync.satisfy(clipId,
                                                       deliveredFrame.frameNumber(),
                                                       QDateTime::currentMSecsSinceEpoch());
                        }
                        if (self->m_state.load() == PlaybackState::Playing) {
                            auto bufferIt = self->m_playbackBuffers.find(clipId);
                            if (bufferIt != self->m_playbackBuffers.end() && bufferIt.value()) {
                                bufferIt.value()->insert(canonicalFrame, deliveredFrame);
                            }
                        }
                        if (auto* cache = self->getOrCreateClipCache(clipId)) {
                            cache->insert(canonicalFrame, deliveredFrame);
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

                    cacheTrace(QStringLiteral("TimelineCache::requestFrame.complete"),
                               QStringLiteral("clip=%1 frame=%2 null=%3 waitMs=%4")
                                   .arg(clipId)
                                   .arg(canonicalFrame)
                                   .arg(deliveredFrame.isNull())
                                   .arg(cacheTraceMs() - requestedAtTraceMs));

                    const qint64 waitMs = cacheTraceMs() - requestedAtTraceMs;
                    if (debugCacheWarnOnlyEnabled()) {
                        if (deliveredFrame.isNull() && obsoleteVisibleRequest) {
                            cacheWarnTrace(QStringLiteral("TimelineCache::visible-cancelled"),
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
                                                  bool allowCacheFallback) {
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

    const auto isDisplayableCandidate = [this, frameNumber](const FrameHandle& frame) {
        if (frame.isNull()) {
            return false;
        }
        if (m_state.load() != PlaybackState::Playing) {
            return true;
        }
        const int64_t candidateFrame = frame.frameNumber();
        if (candidateFrame < 0) {
            return true;
        }
        constexpr int64_t kMaxPlaybackStaleFrameDelta = 4;
        return candidateFrame + kMaxPlaybackStaleFrameDelta >= frameNumber;
    };

    if (preferPlaybackBuffer && playbackBuffer) {
        const FrameHandle playbackExact = playbackBuffer->get(frameNumber);
        if (isDisplayableCandidate(playbackExact)) {
            return true;
        }
        if (allowApproximateFrame &&
            isDisplayableCandidate(playbackBuffer->getLatestAtOrBefore(frameNumber))) {
            return true;
        }
    }

    if (allowCacheFallback && cache) {
        const FrameHandle cacheExact = cache->get(frameNumber);
        if (isDisplayableCandidate(cacheExact)) {
            return true;
        }
        if (allowApproximateFrame &&
            isDisplayableCandidate(cache->getLatestAtOrBefore(frameNumber))) {
            return true;
        }
    }

    return false;
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
    const qint64 now = cacheTraceMs();
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

}  // namespace editor
