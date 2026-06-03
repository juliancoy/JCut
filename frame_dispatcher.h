#pragma once

#include "async_decoder.h"
#include "frame_handle.h"

#include <QHash>
#include <QJsonArray>
#include <QJsonObject>
#include <QMutex>
#include <QObject>
#include <QVector>
#include <atomic>
#include <functional>

namespace editor {

// ============================================================================
// FrameDispatcher - Unified dispatch point for all frame decode requests
//
// Replaces the split dispatch logic across TimelineCache and
// PlaybackFramePipeline with a single pending set, rate limiting,
// smart cancel-before, and unified diagnostics.
//
// Both TimelineCache and PlaybackFramePipeline call requestFrame()
// instead of going directly to AsyncDecoder. Completed frames are
// routed to the appropriate consumer via callbacks.
// ============================================================================
class FrameDispatcher : public QObject {
    Q_OBJECT
public:
    struct PendingRequest {
        QString filePath;
        int64_t frameNumber = 0;
        int priority = 0;
        DecodeRequestKind kind = DecodeRequestKind::Prefetch;
        QVector<std::function<void(FrameHandle)>> callbacks;
        qint64 requestedAtMs = 0;
        uint64_t sequenceId = 0;
    };

    explicit FrameDispatcher(AsyncDecoder* decoder, QObject* parent = nullptr);
    ~FrameDispatcher() override;

    // ========================================================================
    // Core API
    // ========================================================================

    // Single entry point for all frame decode requests.
    // Returns true if the request was accepted (queued or coalesced with an
    // existing pending request for the same frame).
    // Returns false if rate-limited (caller should retry later).
    bool requestFrame(const QString& filePath,
                      int64_t frameNumber,
                      int priority,
                      int timeoutMs,
                      DecodeRequestKind kind,
                      std::function<void(FrameHandle)> callback);

    // Called by AsyncDecoder when a frame completes decoding.
    // Routes the frame to all chained callbacks.
    void onFrameDecoded(const QString& filePath,
                        int64_t frameNumber,
                        FrameHandle frame);

    // Cancel all pending requests for a file before the given frame.
    void cancelBefore(const QString& filePath, int64_t keepFromFrame);

    // Cancel all pending requests for a file.
    void cancelForFile(const QString& filePath);

    // Cancel all pending requests.
    void cancelAll();

    // ========================================================================
    // Query methods
    // ========================================================================

    bool isPending(const QString& filePath, int64_t frameNumber) const;
    bool isNearbyPending(const QString& filePath,
                         int64_t frameNumber,
                         int slackFrames) const;
    int pendingCount() const;
    int pendingCountForFile(const QString& filePath) const;

    // ========================================================================
    // Rate limiting — time-window-based
    // ========================================================================

    void setMaxRequestsPerWindow(int max);
    int maxRequestsPerWindow() const;
    void setRateLimitWindowMs(qint64 ms);
    qint64 rateLimitWindowMs() const;

    // ========================================================================
    // Diagnostics
    // ========================================================================

    QJsonObject diagnosticsSnapshot() const;
    QJsonArray pendingRequestsSnapshot(int limit = 64) const;

signals:
    void frameAvailable(const QString& filePath, int64_t frameNumber);

private:
    struct PendingKey {
        QString filePath;
        int64_t frameNumber;

        bool operator==(const PendingKey& o) const {
            return filePath == o.filePath && frameNumber == o.frameNumber;
        }

        friend uint qHash(const PendingKey& key, uint seed) {
            return qHash(key.filePath, seed) ^ qHash(key.frameNumber);
        }
    };

    // Remove a pending request and return its callbacks.
    QVector<std::function<void(FrameHandle)>> takeCallbacks(
        const QString& filePath, int64_t frameNumber);

    AsyncDecoder* m_decoder = nullptr;

    mutable QMutex m_mutex;
    QHash<PendingKey, PendingRequest> m_pending;

    // Per-file latest target frame (for cancel-before decisions)
    QHash<QString, int64_t> m_latestTargets;

    // Rate limiting — time-window-based
    int m_requestsThisWindow = 0;
    int m_maxRequestsPerWindow = 8;
    qint64 m_rateLimitWindowStartMs = 0;
    qint64 m_rateLimitWindowMs = 16; // 16ms window (~1 frame at 60fps)

    // Cancel-before throttle state
    QHash<QString, int64_t> m_lastCancelKeepFromByPath;
    QHash<QString, qint64> m_lastCancelAtMsByPath;

    // Diagnostics counters
    std::atomic<uint64_t> m_totalDispatched{0};
    std::atomic<uint64_t> m_totalCoalesced{0};
    std::atomic<uint64_t> m_totalRateLimited{0};
    std::atomic<uint64_t> m_totalCancelled{0};
    std::atomic<uint64_t> m_totalCompleted{0};
    std::atomic<uint64_t> m_totalNullCompleted{0};
};

} // namespace editor
