#pragma once

#include "async_decoder.h"
#include "editor_shared.h"
#include "frame_handle.h"

#include <QObject>
#include <QHash>
#include <QJsonArray>
#include <QJsonObject>
#include <QMutex>
#include <QSet>
#include <QVector>
#include <functional>

namespace editor {

// Forward declaration
class FrameDispatcher;

class PlaybackFramePipeline : public QObject {
    Q_OBJECT

public:
    explicit PlaybackFramePipeline(AsyncDecoder* decoder,
                                   FrameDispatcher* dispatcher = nullptr,
                                   QObject* parent = nullptr);
    ~PlaybackFramePipeline() override;

    void setTimelineClips(const QVector<TimelineClip>& clips);
    void setRenderSyncMarkers(const QVector<RenderSyncMarker>& markers);
    void setPlaybackActive(bool active);
    void setPlayheadFrame(int64_t playheadFrame);

    void requestFramesForSample(int64_t samplePosition, const std::function<void()>& onFrameReady);

    FrameHandle getFrame(const QString& clipId, int64_t frameNumber) const;
    FrameHandle getBestFrame(const QString& clipId, int64_t frameNumber) const;
    FrameHandle getPresentationFrame(const QString& clipId, int64_t frameNumber) const;
    bool isFrameBuffered(const QString& clipId, int64_t frameNumber) const;

    int pendingVisibleRequestCount() const;
    int bufferedFrameCount() const;
    int droppedPresentationFrameCount() const { return m_droppedPresentationFrames.load(); }
    QJsonObject decodeDiagnostics() const;
    QJsonArray frameTraceSnapshot(int limit = 200) const;

signals:
    void frameAvailable();

private slots:
    void onFrameReady(FrameHandle frame);

private:
    struct ClipInfo {
        TimelineClip clip;
        QString playbackPath;
        bool isSingleFrame = false;
    };

    struct PlaybackFrameInfo {
        FrameHandle frame;
        qint64 insertedAt = 0;
    };

    class PlaybackBuffer {
    public:
        void clear();
        void insert(int64_t frameNumber, const FrameHandle& frame);
        FrameHandle get(int64_t frameNumber) const;
        FrameHandle getBest(int64_t frameNumber) const;
        FrameHandle getPresentation(int64_t frameNumber) const;
        bool contains(int64_t frameNumber) const;
        int size() const;

    private:
        void trimLocked();

        mutable QMutex m_mutex;
        QHash<int64_t, PlaybackFrameInfo> m_frames;
        static constexpr int kMaxFrames = 96;
    };

    QString requestKey(const QString& clipId, int64_t frameNumber) const;
    int64_t normalizeFrameNumber(const QString& clipId, int64_t frameNumber) const;
    int64_t normalizeFrameNumber(const ClipInfo& info, int64_t frameNumber) const;
    void clearBuffers();
    void dropStaleRequestsForPlayhead(int64_t playheadFrame);
    void cancelDecoderBeforeThrottled(const QString& playbackPath,
                                      int64_t keepFromFrame,
                                      qint64 nowMs);
    void schedulePlaybackWindow(const ClipInfo& info,
                                int64_t samplePosition,
                                int64_t canonicalFrame,
                                const QVector<RenderSyncMarker>& markers,
                                const std::function<void()>& onFrameReady);

    struct DecodeDiagnostics {
        uint64_t visibleDispatched = 0;
        uint64_t visibleCompleted = 0;
        uint64_t visibleNullCompleted = 0;
        uint64_t visibleObsoleteCompleted = 0;
        uint64_t visibleBufferedCompleted = 0;
        uint64_t prefetchDispatched = 0;
        uint64_t prefetchCompleted = 0;
        qint64 lastVisibleFrame = -1;
        qint64 lastVisibleWaitMs = -1;
        qint64 maxVisibleWaitMs = 0;
        qint64 lastVisibleQtDeliveryDelayMs = -1;
        qint64 maxVisibleQtDeliveryDelayMs = 0;
        qint64 lastCompletedAtMs = 0;
        QString lastVisibleClipId;
        QString lastVisiblePayload;
        QString lastVisibleOutcome;
    };

    struct FrameTraceEvent {
        qint64 timestampMs = 0;
        qint64 playheadFrame = 0;
        int64_t targetFrame = 0;
        QString clipId;
        QString event;  // "dispatch", "null", "buffered", "obsolete", "nearby_pending_skip", "cancel_before"
        QString payload;
        qint64 waitMs = 0;
    };

    AsyncDecoder* m_decoder = nullptr;
    FrameDispatcher* m_dispatcher = nullptr;

    // Legacy pending tracking (used when no dispatcher is configured)
    mutable QMutex m_pendingMutex;
    QSet<QString> m_pendingVisibleRequests;
    QSet<QString> m_pendingPrefetchRequests;
    QHash<QString, int64_t> m_latestVisibleTargets;

    mutable QMutex m_clipsMutex;
    QHash<QString, ClipInfo> m_clips;
    QHash<QString, PlaybackBuffer*> m_buffers;

    mutable QMutex m_decodeDiagnosticsMutex;
    DecodeDiagnostics m_decodeDiagnostics;

    mutable QMutex m_markersMutex;
    QVector<RenderSyncMarker> m_renderSyncMarkers;

    // Frame-level trace ring buffer
    void recordFrameTraceEvent(const QString& event,
                               const QString& clipId,
                               int64_t targetFrame,
                               const QString& payload = QString(),
                               qint64 waitMs = 0);
    mutable QMutex m_frameTraceMutex;
    QVector<FrameTraceEvent> m_frameTraceEvents;
    static constexpr int kMaxFrameTraceEvents = 1000;

    std::atomic<bool> m_active{false};
    std::atomic<int64_t> m_playheadFrame{0};
    mutable std::atomic<int> m_droppedPresentationFrames{0};
};

}  // namespace editor
