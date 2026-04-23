#pragma once

#include "qt_compat.h"  // Qt 6.4/GCC 13 compatibility
#include "frame_handle.h"
#include "memory_budget.h"

#include <QDateTime>
#include <QDeadlineTimer>
#include <QHash>
#include <QImage>
#include <QMutex>
#include <QObject>
#include <QVector>
#include <functional>
#include <atomic>
#include <condition_variable>
#include <deque>
#include <memory>
#include <mutex>
#include <set>
#include <thread>
#include <vector>

// AVHWDeviceType is an enum, available via hwcontext.h.
// We include it here so the shared-device map type is visible in the header.
extern "C" {
#include <libavutil/hwcontext.h>
}

// FFmpeg forward declarations (actual includes in .cpp)
extern "C" {
struct AVCodecContext;
struct AVCodec;
struct AVFormatContext;
struct AVFrame;
struct AVPacket;
struct AVBufferRef;
struct AVCodecHWConfig;
struct SwsContext;
// AVPixelFormat is an enum - we store it as int in the header
}

namespace editor {

enum class DecodeRequestKind : int {
    Visible = 0,
    Prefetch = 1,
    Preload = 2,
};

// Forward declaration - full definition in decoder_context.h
class DecoderContext;

// ============================================================================
// DecodeRequest - Single frame decode request
// ============================================================================
struct DecodeRequest {
    uint64_t sequenceId;
    QString filePath;
    int64_t frameNumber;
    int priority;  // Higher = more urgent (0-255)
    DecodeRequestKind kind = DecodeRequestKind::Visible;
    uint64_t generation = 0;
    QDeadlineTimer deadline;
    std::function<void(FrameHandle)> callback;
    qint64 submittedAt;
    
    bool isExpired() const { return deadline.hasExpired(); }
    int64_t ageMs() const { return QDateTime::currentMSecsSinceEpoch() - submittedAt; }
};

// ============================================================================
// VideoStreamInfo - Metadata about a video file
// ============================================================================
struct VideoStreamInfo {
    QString path;
    int64_t durationFrames = 0;
    double fps = 30.0;
    QSize frameSize;
    int64_t bitrate = 0;
    QString codecName;
    QString decodePath;
    QString interopPath;
    QString requestedDecodeMode;
    bool hasAudio = false;
    bool hasAlpha = false;
    bool hardwareAccelerated = false;
    bool isValid = false;
};

// Forward declaration - full definition in decoder_context.h
class DecoderContext;

// ============================================================================
// AsyncDecoder - Main API for async frame decoding
// ============================================================================
class AsyncDecoder : public QObject {
    Q_OBJECT
public:
    explicit AsyncDecoder(QObject* parent = nullptr);
    ~AsyncDecoder();
    
    bool initialize();
    void shutdown();
    
    // Request a frame decode (non-blocking)
    // callback is invoked on the decoder thread - use QMetaObject::invokeMethod 
    // to get back to main thread if needed
    uint64_t requestFrame(const QString& path,
                          int64_t frameNumber,
                          int priority,
                          int timeoutMs,
                          std::function<void(FrameHandle)> callback);
    uint64_t requestFrame(const QString& path, 
                          int64_t frameNumber,
                          int priority,
                          int timeoutMs,
                          DecodeRequestKind kind,
                          std::function<void(FrameHandle)> callback);
    
    // Cancel pending requests
    void cancelForFile(const QString& path);
    void cancelForFileBefore(const QString& path, int64_t frameNumber);
    void cancelAll();
    
    // Get video info (cached)
    VideoStreamInfo getVideoInfo(const QString& path);
    
    // Preload a file (warm up decoder)
    void preloadFile(const QString& path);
    
    // Statistics
    int pendingRequestCount() const { return totalPendingRequests(); }
    int workerCount() const { return m_workerCount; }
    void setWorkerCount(int workerCount);
    
    // Memory budget access
    MemoryBudget* memoryBudget() const { return m_budget; }

    // Returns a shared (ref-bumped) AVBufferRef* for the given hw device type,
    // or nullptr if the device isn't available. The caller is responsible for
    // av_buffer_unref()-ing their reference when done.
    AVBufferRef* acquireSharedHwDevice(AVHWDeviceType type) const;

signals:
    void frameReady(FrameHandle frame);
    void error(QString path, QString message);
    void queuePressureChanged(int pendingCount);
    

private:
    struct LaneState;

    void setupTrimCallback();
    void trimCaches();
    void initSharedHwDevices();
    void releaseSharedHwDevices();
    int resolveWorkerCount(int requestedCount) const;
    int totalPendingRequests() const;
    int laneIndexForRequest(const QString& path,
                            int64_t frameNumber,
                            DecodeRequestKind kind) const;
    LaneState* laneForRequest(const QString& path,
                              int64_t frameNumber,
                              DecodeRequestKind kind) const;
    std::vector<LaneState*> lanesForPath(const QString& path) const;
    void startLane(LaneState* lane);
    void stopLane(LaneState* lane);
    void runLane(LaneState* lane);
    void insertByPriority(std::deque<DecodeRequest>& queue, const DecodeRequest& req);
    void collectSupersededRequests(const DecodeRequest& req,
                                   std::deque<DecodeRequest>& queue,
                                   QVector<std::function<void(FrameHandle)>>* droppedCallbacks);
    
    MemoryBudget* m_budget = nullptr;
    int m_workerCount = 0;
    std::vector<std::unique_ptr<LaneState>> m_lanes;
    bool m_initialized = false;
    bool m_shuttingDown = false;
    std::atomic<uint64_t> m_nextSequenceId{1};

    // One shared hw device context per device type, ref-counted by FFmpeg.
    // All DecoderContexts borrow a reference rather than creating their own.
    mutable QMutex m_sharedHwMutex;
    QHash<int, AVBufferRef*> m_sharedHwDevices; // key = AVHWDeviceType cast to int

    QMutex m_infoCacheMutex;
    QHash<QString, VideoStreamInfo> m_infoCache;
};

} // namespace editor
