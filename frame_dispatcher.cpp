#include "frame_dispatcher.h"
#include "debug_controls.h"

#include <QDateTime>
#include <QJsonArray>
#include <QJsonObject>
#include <QMutexLocker>
#include <limits>

namespace editor {

// ============================================================================
// Construction / Destruction
// ============================================================================

FrameDispatcher::FrameDispatcher(AsyncDecoder* decoder, QObject* parent)
    : QObject(parent)
    , m_decoder(decoder)
{
}

FrameDispatcher::~FrameDispatcher() = default;

// ============================================================================
// requestFrame - Single entry point for all frame decode requests
//
// 1. Check if the exact frame is already pending → chain callback, return true
// 2. Check rate limit → return false if exceeded
// 3. Dispatch to AsyncDecoder, record in pending set
// ============================================================================

bool FrameDispatcher::requestFrame(const QString& filePath,
                                   int64_t frameNumber,
                                   int priority,
                                   int timeoutMs,
                                   DecodeRequestKind kind,
                                   std::function<void(FrameHandle)> callback)
{
    if (!m_decoder || filePath.isEmpty()) {
        return false;
    }

    QMutexLocker lock(&m_mutex);

    // --- Step 1: Check if exact frame is already pending → coalesce ---
    PendingKey key{filePath, frameNumber};
    auto it = m_pending.find(key);
    if (it != m_pending.end()) {
        // Chain the new callback onto the existing pending request.
        // Use the higher priority.
        it.value().priority = qMax(it.value().priority, priority);
        it.value().callbacks.append(std::move(callback));
        m_totalCoalesced.fetch_add(1, std::memory_order_relaxed);
        return true;
    }

    // --- Step 2: Rate limiting (time-window-based) ---
    const qint64 nowMs = QDateTime::currentMSecsSinceEpoch();
    if (nowMs - m_rateLimitWindowStartMs >= m_rateLimitWindowMs) {
        // Window expired — reset counter
        m_requestsThisWindow = 0;
        m_rateLimitWindowStartMs = nowMs;
    }
    if (m_requestsThisWindow >= m_maxRequestsPerWindow) {
        m_totalRateLimited.fetch_add(1, std::memory_order_relaxed);
        return false;
    }

    // --- Step 3: Dispatch to AsyncDecoder ---
    const uint64_t seqId = m_decoder->requestFrame(
        filePath, frameNumber, priority, timeoutMs, kind,
        [this, filePath, frameNumber](FrameHandle frame) {
            onFrameDecoded(filePath, frameNumber, frame);
        });

    PendingRequest pending;
    pending.filePath = filePath;
    pending.frameNumber = frameNumber;
    pending.priority = priority;
    pending.kind = kind;
    pending.callbacks.append(std::move(callback));
    pending.requestedAtMs = nowMs;
    pending.sequenceId = seqId;

    m_pending.insert(key, std::move(pending));
    m_latestTargets.insert(filePath, frameNumber);
    m_requestsThisWindow++;
    m_totalDispatched.fetch_add(1, std::memory_order_relaxed);

    return true;
}

// ============================================================================
// onFrameDecoded - Called by AsyncDecoder when a frame completes
//
// Routes the frame to all chained callbacks and removes the pending entry.
// ============================================================================

void FrameDispatcher::onFrameDecoded(const QString& filePath,
                                     int64_t frameNumber,
                                     FrameHandle frame)
{
    QVector<std::function<void(FrameHandle)>> callbacks;
    {
        QMutexLocker lock(&m_mutex);
        PendingKey key{filePath, frameNumber};
        auto it = m_pending.find(key);
        if (it == m_pending.end()) {
            // Frame wasn't in our pending set — could be a spontaneous decode
            // (e.g. from a preload or a request that was already cancelled).
            // Still emit the signal so consumers can pick it up.
            lock.unlock();
            emit frameAvailable(filePath, frameNumber);
            return;
        }
        callbacks = std::move(it.value().callbacks);
        m_pending.erase(it);
    }

    m_totalCompleted.fetch_add(1, std::memory_order_relaxed);
    if (frame.isNull()) {
        m_totalNullCompleted.fetch_add(1, std::memory_order_relaxed);
    }

    // Invoke all chained callbacks
    for (auto& cb : callbacks) {
        if (cb) {
            cb(frame);
        }
    }

    emit frameAvailable(filePath, frameNumber);
}

// ============================================================================
// cancelBefore - Cancel pending requests before a given frame
//
// Uses the same throttle logic as the original cancelDecoderBeforeThrottled
// to avoid excessive cancel/reissue churn.
// ============================================================================

void FrameDispatcher::cancelBefore(const QString& filePath, int64_t keepFromFrame)
{
    if (!m_decoder || filePath.isEmpty()) {
        return;
    }

    const qint64 nowMs = QDateTime::currentMSecsSinceEpoch();

    bool shouldCancel = false;
    {
        QMutexLocker lock(&m_mutex);
        const int64_t previousKeepFrom =
            m_lastCancelKeepFromByPath.value(filePath, std::numeric_limits<int64_t>::min());
        const qint64 previousCancelAt = m_lastCancelAtMsByPath.value(filePath, 0);
        const bool advancedEnough =
            keepFromFrame >= previousKeepFrom + debugCancelBeforeMinFrameAdvance();
        const bool overdue =
            previousCancelAt <= 0 || (nowMs - previousCancelAt) >= debugCancelBeforeMinIntervalMs();
        shouldCancel = advancedEnough || overdue;
        if (shouldCancel) {
            m_lastCancelKeepFromByPath.insert(filePath, keepFromFrame);
            m_lastCancelAtMsByPath.insert(filePath, nowMs);
        }
    }

    if (shouldCancel) {
        // Collect callbacks from removed pending entries so we can
        // notify callers that their requests were cancelled.
        QVector<std::function<void(FrameHandle)>> cancelledCallbacks;
        {
            QMutexLocker lock(&m_mutex);
            QList<PendingKey> toRemove;
            for (auto it = m_pending.begin(); it != m_pending.end(); ++it) {
                if (it.key().filePath == filePath && it.key().frameNumber < keepFromFrame) {
                    toRemove.append(it.key());
                }
            }
            for (const PendingKey& key : toRemove) {
                auto it = m_pending.find(key);
                if (it != m_pending.end()) {
                    for (auto& cb : it.value().callbacks) {
                        if (cb) {
                            cancelledCallbacks.append(std::move(cb));
                        }
                    }
                    m_pending.erase(it);
                }
            }
        }

        // Invoke cancelled callbacks with null frame so callers know
        // their requests were dropped and can react accordingly.
        for (auto& cb : cancelledCallbacks) {
            if (cb) {
                cb(FrameHandle());
            }
        }

        m_totalCancelled.fetch_add(1, std::memory_order_relaxed);
        m_decoder->cancelForFileBefore(filePath, keepFromFrame);
    }
}

// ============================================================================
// cancelForFile - Cancel all pending requests for a file
// ============================================================================

void FrameDispatcher::cancelForFile(const QString& filePath)
{
    if (!m_decoder || filePath.isEmpty()) {
        return;
    }

    QVector<std::function<void(FrameHandle)>> cancelledCallbacks;
    {
        QMutexLocker lock(&m_mutex);
        QList<PendingKey> toRemove;
        for (auto it = m_pending.begin(); it != m_pending.end(); ++it) {
            if (it.key().filePath == filePath) {
                toRemove.append(it.key());
            }
        }
        for (const PendingKey& key : toRemove) {
            auto it = m_pending.find(key);
            if (it != m_pending.end()) {
                for (auto& cb : it.value().callbacks) {
                    if (cb) {
                        cancelledCallbacks.append(std::move(cb));
                    }
                }
                m_pending.erase(it);
            }
        }
    }

    for (auto& cb : cancelledCallbacks) {
        if (cb) {
            cb(FrameHandle());
        }
    }

    m_totalCancelled.fetch_add(1, std::memory_order_relaxed);
    m_decoder->cancelForFile(filePath);
}

// ============================================================================
// cancelAll - Cancel all pending requests
// ============================================================================

void FrameDispatcher::cancelAll()
{
    if (!m_decoder) {
        return;
    }

    QVector<std::function<void(FrameHandle)>> cancelledCallbacks;
    {
        QMutexLocker lock(&m_mutex);
        for (auto it = m_pending.begin(); it != m_pending.end(); ++it) {
            for (auto& cb : it.value().callbacks) {
                if (cb) {
                    cancelledCallbacks.append(std::move(cb));
                }
            }
        }
        m_pending.clear();
    }

    for (auto& cb : cancelledCallbacks) {
        if (cb) {
            cb(FrameHandle());
        }
    }

    m_totalCancelled.fetch_add(1, std::memory_order_relaxed);
    m_decoder->cancelAll();
}

// ============================================================================
// Query methods
// ============================================================================

bool FrameDispatcher::isPending(const QString& filePath, int64_t frameNumber) const
{
    QMutexLocker lock(&m_mutex);
    PendingKey key{filePath, frameNumber};
    return m_pending.contains(key);
}

bool FrameDispatcher::isNearbyPending(const QString& filePath,
                                      int64_t frameNumber,
                                      int slackFrames) const
{
    QMutexLocker lock(&m_mutex);
    for (auto it = m_pending.begin(); it != m_pending.end(); ++it) {
        if (it.key().filePath == filePath) {
            const int64_t delta = qAbs(it.key().frameNumber - frameNumber);
            if (delta <= slackFrames) {
                return true;
            }
        }
    }
    return false;
}

int FrameDispatcher::pendingCount() const
{
    QMutexLocker lock(&m_mutex);
    return m_pending.size();
}

int FrameDispatcher::pendingCountForFile(const QString& filePath) const
{
    QMutexLocker lock(&m_mutex);
    int count = 0;
    for (auto it = m_pending.begin(); it != m_pending.end(); ++it) {
        if (it.key().filePath == filePath) {
            count++;
        }
    }
    return count;
}

// ============================================================================
// Rate limiting — time-window-based
// ============================================================================

void FrameDispatcher::setMaxRequestsPerWindow(int max)
{
    m_maxRequestsPerWindow = qBound(1, max, 128);
}

int FrameDispatcher::maxRequestsPerWindow() const
{
    return m_maxRequestsPerWindow;
}

void FrameDispatcher::setRateLimitWindowMs(qint64 ms)
{
    m_rateLimitWindowMs = qBound<qint64>(qint64(1), ms, qint64(1000));
}

qint64 FrameDispatcher::rateLimitWindowMs() const
{
    return m_rateLimitWindowMs;
}

// ============================================================================
// Diagnostics
// ============================================================================

QJsonObject FrameDispatcher::diagnosticsSnapshot() const
{
    QJsonObject obj;

    obj[QStringLiteral("total_dispatched")] = static_cast<qint64>(
        m_totalDispatched.load(std::memory_order_relaxed));
    obj[QStringLiteral("total_coalesced")] = static_cast<qint64>(
        m_totalCoalesced.load(std::memory_order_relaxed));
    obj[QStringLiteral("total_rate_limited")] = static_cast<qint64>(
        m_totalRateLimited.load(std::memory_order_relaxed));
    obj[QStringLiteral("total_cancelled")] = static_cast<qint64>(
        m_totalCancelled.load(std::memory_order_relaxed));
    obj[QStringLiteral("total_completed")] = static_cast<qint64>(
        m_totalCompleted.load(std::memory_order_relaxed));
    obj[QStringLiteral("total_null_completed")] = static_cast<qint64>(
        m_totalNullCompleted.load(std::memory_order_relaxed));

    {
        QMutexLocker lock(&m_mutex);
        obj[QStringLiteral("pending_count")] = m_pending.size();
        obj[QStringLiteral("max_requests_per_window")] = m_maxRequestsPerWindow;
        obj[QStringLiteral("requests_this_window")] = m_requestsThisWindow;
        obj[QStringLiteral("rate_limit_window_ms")] = m_rateLimitWindowMs;
    }

    return obj;
}

QJsonArray FrameDispatcher::pendingRequestsSnapshot(int limit) const
{
    QJsonArray result;
    QMutexLocker lock(&m_mutex);

    int count = 0;
    for (auto it = m_pending.begin(); it != m_pending.end() && count < limit; ++it, ++count) {
        QJsonObject entry;
        entry[QStringLiteral("file_path")] = it.key().filePath;
        entry[QStringLiteral("frame_number")] = static_cast<qint64>(it.key().frameNumber);
        entry[QStringLiteral("priority")] = it.value().priority;
        entry[QStringLiteral("kind")] = static_cast<int>(it.value().kind);
        entry[QStringLiteral("callback_count")] = it.value().callbacks.size();
        entry[QStringLiteral("requested_at_ms")] = it.value().requestedAtMs;
        entry[QStringLiteral("sequence_id")] = static_cast<qint64>(it.value().sequenceId);
        result.append(entry);
    }

    return result;
}

// ============================================================================
// Private helpers
// ============================================================================

QVector<std::function<void(FrameHandle)>> FrameDispatcher::takeCallbacks(
    const QString& filePath, int64_t frameNumber)
{
    QMutexLocker lock(&m_mutex);
    PendingKey key{filePath, frameNumber};
    auto it = m_pending.find(key);
    if (it == m_pending.end()) {
        return {};
    }
    QVector<std::function<void(FrameHandle)>> callbacks = std::move(it.value().callbacks);
    m_pending.erase(it);
    return callbacks;
}

} // namespace editor
