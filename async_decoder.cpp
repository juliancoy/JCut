#include "async_decoder.h"

#include "debug_controls.h"
#include "decode_trace.h"
#include "decoder_context.h"
#include "decoder_image_io.h"

#include <QCoreApplication>
#include <QDateTime>
#include <QDeadlineTimer>
#include <QDebug>
#include <QFile>
#include <QJsonObject>
#include <QMetaObject>
#include <QMutexLocker>
#include <QThread>

extern "C" {
#include <libavutil/hwcontext.h>
}

#include <atomic>
#include <condition_variable>
#include <deque>
#include <limits>
#include <memory>
#include <mutex>
#include <thread>
#include <utility>
#include <vector>

namespace editor {

namespace {
constexpr int kMaxDecoderLaneCount = 16;

void storeAtomicMax(std::atomic<int64_t>* target, int64_t value) {
    if (!target) {
        return;
    }
    int64_t current = target->load(std::memory_order_relaxed);
    while (value > current &&
           !target->compare_exchange_weak(current, value, std::memory_order_relaxed)) {
    }
}
} // namespace

struct AsyncDecoder::LaneState {
    struct FileDecodeState {
        std::atomic<int64_t> cancelBeforeFrame{std::numeric_limits<int64_t>::min()};
        std::atomic<uint64_t> generation{0};
        std::unique_ptr<DecoderContext> context;
    };

    explicit LaneState(int laneIndex)
        : index(laneIndex) {}

    int index = 0;
    mutable std::mutex mutex;
    std::condition_variable condition;
    std::deque<DecodeRequest> queue;
    QHash<QString, std::shared_ptr<FileDecodeState>> fileStates;
    std::unique_ptr<std::thread> thread;
    bool shuttingDown = false;
    bool running = false;
    int activeRequests = 0;

    // Increased from 4 to 12 to handle projects with many clips from few source files
    // This reduces decoder recreation when switching between clips from the same file
    static constexpr int kMaxContexts = 12;
};

AsyncDecoder::AsyncDecoder(QObject* parent)
    : QObject(parent) {
    m_budget = new MemoryBudget(this);
    setupTrimCallback();
}

AsyncDecoder::~AsyncDecoder() {
    shutdown();
}

bool AsyncDecoder::initialize() {
    if (m_initialized) {
        return true;
    }

    m_workerCount = resolveWorkerCount(debugDecoderLaneCount());
    m_lanes.reserve(m_workerCount);
    m_shuttingDown = false;

    setDecoderLaneCountChangedCallback([this](int laneCount) {
        QMetaObject::invokeMethod(
            this,
            [this, laneCount]() {
                setWorkerCount(laneCount);
            },
            Qt::QueuedConnection);
    });

    // Create one shared hw device context per supported type.
    // Worker threads borrow a ref instead of creating their own.
    initSharedHwDevices();

    for (int i = 0; i < m_workerCount; ++i) {
        auto lane = std::make_unique<LaneState>(i);
        startLane(lane.get());
        m_lanes.push_back(std::move(lane));
    }

    m_initialized = true;
    qDebug() << "AsyncDecoder initialized with" << m_workerCount << "workers";
    return true;
}

void AsyncDecoder::shutdown() {
    setDecoderLaneCountChangedCallback({});
    m_shuttingDown = true;
    for (const auto& lane : m_lanes) {
        stopLane(lane.get());
    }
    m_lanes.clear();
    releaseSharedHwDevices();
    m_initialized = false;
    m_workerCount = 0;
}

void AsyncDecoder::setWorkerCount(int workerCount) {
    const int targetWorkerCount = resolveWorkerCount(workerCount);
    if (targetWorkerCount == m_workerCount) {
        return;
    }

    if (!m_initialized) {
        m_workerCount = targetWorkerCount;
        return;
    }

    cancelAll();
    for (const auto& lane : m_lanes) {
        stopLane(lane.get());
    }
    m_lanes.clear();

    m_workerCount = targetWorkerCount;
    m_lanes.reserve(m_workerCount);
    for (int i = 0; i < m_workerCount; ++i) {
        auto lane = std::make_unique<LaneState>(i);
        startLane(lane.get());
        m_lanes.push_back(std::move(lane));
    }

    qDebug() << "AsyncDecoder reconfigured to" << m_workerCount << "workers";
    emit queuePressureChanged(totalPendingRequests());
}

uint64_t AsyncDecoder::requestFrame(const QString& path,
                                    int64_t frameNumber,
                                    int priority,
                                    int timeoutMs,
                                    std::function<void(FrameHandle)> callback) {
    return requestFrame(path,
                        frameNumber,
                        priority,
                        timeoutMs,
                        DecodeRequestKind::Visible,
                        std::move(callback));
}

uint64_t AsyncDecoder::requestFrame(const QString& path,
                                    int64_t frameNumber,
                                    int priority,
                                    int timeoutMs,
                                    DecodeRequestKind kind,
                                    std::function<void(FrameHandle)> callback) {
    LaneState* lane = laneForRequest(path, frameNumber, kind);
    if (!lane || m_shuttingDown) {
        recordNullCallback(kind, "lane_unavailable");
        invokeRequestCallback(std::move(callback), FrameHandle());
        return 0;
    }

    const uint64_t seqId = m_nextSequenceId++;
    DecodeRequest req;
    req.sequenceId = seqId;
    req.filePath = path;
    req.frameNumber = frameNumber;
    req.priority = priority;
    req.kind = kind;
    req.deadline = QDeadlineTimer(timeoutMs);
    req.callback = callback;
    req.submittedAt = QDateTime::currentMSecsSinceEpoch();

    QVector<DroppedCallback> droppedCallbacks;
    int pendingBefore = 0;
    bool accepted = false;

    {
        std::unique_lock<std::mutex> lock(lane->mutex);

        auto stateIt = lane->fileStates.find(path);
        if (stateIt == lane->fileStates.end()) {
            stateIt = lane->fileStates.insert(path, std::make_shared<LaneState::FileDecodeState>());
        }
        const std::shared_ptr<LaneState::FileDecodeState> state = stateIt.value();

        if (frameNumber < state->cancelBeforeFrame.load()) {
            state->cancelBeforeFrame.store(std::numeric_limits<int64_t>::min());
        }
        req.generation = state->generation.load();
        pendingBefore = static_cast<int>(lane->queue.size()) + lane->activeRequests;

        collectSupersededRequests(req, lane->queue, &droppedCallbacks);

        constexpr int kMaxPendingRequests = 128;
        const int visibleReserve = qBound(0, debugVisibleQueueReserve(), kMaxPendingRequests - 1);
        const int nonVisibleLimit = kMaxPendingRequests - visibleReserve;

        if (req.kind != DecodeRequestKind::Visible &&
            static_cast<int>(lane->queue.size()) >= nonVisibleLimit) {
            accepted = false;
        } else {
            if (static_cast<int>(lane->queue.size()) >= kMaxPendingRequests) {
                bool dropped = false;
                for (auto it = lane->queue.end(); it != lane->queue.begin();) {
                    --it;
                    const bool kindFavored =
                        req.kind == DecodeRequestKind::Visible &&
                        it->kind != DecodeRequestKind::Visible;
                    if (kindFavored || it->priority < req.priority) {
                        if (it->callback) {
                            droppedCallbacks.push_back(DroppedCallback{it->kind, std::move(it->callback)});
                        }
                        lane->queue.erase(it);
                        dropped = true;
                        break;
                    }
                }

                if (!dropped) {
                    accepted = false;
                } else {
                    insertByPriority(lane->queue, req);
                    accepted = true;
                }
            } else {
                insertByPriority(lane->queue, req);
                accepted = true;
            }
        }

        if (accepted) {
            lane->condition.notify_one();
        }
    }

    decodeTrace(QStringLiteral("AsyncDecoder::requestFrame"),
                QStringLiteral("seq=%1 file=%2 frame=%3 priority=%4 kind=%5 timeoutMs=%6 pending=%7 lane=%8")
                    .arg(seqId)
                    .arg(shortPath(path))
                    .arg(frameNumber)
                    .arg(priority)
                    .arg(static_cast<int>(kind))
                    .arg(timeoutMs)
                    .arg(pendingBefore)
                    .arg(lane->index));

    for (auto& droppedCallback : droppedCallbacks) {
        recordNullCallback(droppedCallback.kind, "superseded");
        invokeRequestCallback(std::move(droppedCallback.callback), FrameHandle());
    }

    if (!accepted) {
        decodeTrace(QStringLiteral("AsyncDecoder::requestFrame.queue-full"),
                    QStringLiteral("seq=%1 file=%2 frame=%3 kind=%4")
                        .arg(seqId)
                        .arg(shortPath(path))
                        .arg(frameNumber)
                        .arg(static_cast<int>(kind)));
        recordNullCallback(kind, "queue_full");
        invokeRequestCallback(std::move(callback), FrameHandle());
        return 0;
    }

    emit queuePressureChanged(totalPendingRequests());
    return seqId;
}

void AsyncDecoder::cancelForFile(const QString& path) {
    const std::vector<LaneState*> lanes = lanesForPath(path);
    if (lanes.empty()) {
        return;
    }

    QVector<DroppedCallback> callbacks;

    for (LaneState* lane : lanes) {
        if (!lane) {
            continue;
        }

        std::unique_lock<std::mutex> lock(lane->mutex);

        auto stateIt = lane->fileStates.find(path);
        if (stateIt == lane->fileStates.end()) {
            stateIt = lane->fileStates.insert(path, std::make_shared<LaneState::FileDecodeState>());
        }

        stateIt.value()->generation.fetch_add(1);
        stateIt.value()->cancelBeforeFrame.store(std::numeric_limits<int64_t>::min());

        for (auto it = lane->queue.begin(); it != lane->queue.end();) {
            if (it->filePath == path) {
                if (it->callback) {
                    callbacks.push_back(DroppedCallback{it->kind, std::move(it->callback)});
                }
                it = lane->queue.erase(it);
            } else {
                ++it;
            }
        }
    }

    for (auto& callback : callbacks) {
        recordNullCallback(callback.kind, "cancel_file");
        invokeRequestCallback(std::move(callback.callback), FrameHandle());
    }

    emit queuePressureChanged(totalPendingRequests());
}

void AsyncDecoder::cancelForFileBefore(const QString& path, int64_t frameNumber) {
    const std::vector<LaneState*> lanes = lanesForPath(path);
    if (lanes.empty()) {
        return;
    }

    QVector<DroppedCallback> callbacks;

    for (LaneState* lane : lanes) {
        if (!lane) {
            continue;
        }

        std::unique_lock<std::mutex> lock(lane->mutex);

        auto stateIt = lane->fileStates.find(path);
        if (stateIt == lane->fileStates.end()) {
            stateIt = lane->fileStates.insert(path, std::make_shared<LaneState::FileDecodeState>());
        }

        const auto& state = stateIt.value();
        state->cancelBeforeFrame.store(qMax(state->cancelBeforeFrame.load(), frameNumber));

        for (auto it = lane->queue.begin(); it != lane->queue.end();) {
            if (it->filePath == path && it->frameNumber < frameNumber) {
                if (it->callback) {
                    callbacks.push_back(DroppedCallback{it->kind, std::move(it->callback)});
                }
                it = lane->queue.erase(it);
            } else {
                ++it;
            }
        }
    }

    for (auto& callback : callbacks) {
        recordNullCallback(callback.kind, "cancel_before");
        invokeRequestCallback(std::move(callback.callback), FrameHandle());
    }

    emit queuePressureChanged(totalPendingRequests());
}

void AsyncDecoder::cancelAll() {
    QVector<DroppedCallback> callbacks;

    for (const auto& lane : m_lanes) {
        std::unique_lock<std::mutex> lock(lane->mutex);

        for (auto& state : lane->fileStates) {
            state->generation.fetch_add(1);
            state->cancelBeforeFrame.store(std::numeric_limits<int64_t>::min());
        }

        for (DecodeRequest& request : lane->queue) {
            if (request.callback) {
                callbacks.push_back(DroppedCallback{request.kind, std::move(request.callback)});
            }
        }
        lane->queue.clear();
    }

    for (auto& callback : callbacks) {
        recordNullCallback(callback.kind, "cancel_all");
        invokeRequestCallback(std::move(callback.callback), FrameHandle());
    }

    emit queuePressureChanged(totalPendingRequests());
}

VideoStreamInfo AsyncDecoder::getVideoInfo(const QString& path) {
    QMutexLocker lock(&m_infoCacheMutex);
    const QString requestedDecodeMode = decodePreferenceToString(debugDecodePreference());

    auto it = m_infoCache.find(path);
    if (it != m_infoCache.end() && it.value().requestedDecodeMode == requestedDecodeMode) {
        return it.value();
    }

    DecoderContext ctx(path);
    if (ctx.initialize()) {
        VideoStreamInfo info = ctx.info();
        info.requestedDecodeMode = requestedDecodeMode;
        m_infoCache[path] = info;
        return info;
    }

    return VideoStreamInfo();
}

void AsyncDecoder::preloadFile(const QString& path) {
    getVideoInfo(path);
}

void AsyncDecoder::setupTrimCallback() {
    if (m_budget) {
        m_budget->setTrimCallback([this]() {
            trimCaches();
        });
    }
}

void AsyncDecoder::trimCaches() {
    {
        QMutexLocker lock(&m_infoCacheMutex);
        m_infoCache.clear();
    }

    for (const auto& lane : m_lanes) {
        std::unique_lock<std::mutex> lock(lane->mutex);

        for (auto it = lane->fileStates.begin(); it != lane->fileStates.end();) {
            const QString& path = it.key();

            bool queuedForPath = false;
            for (const DecodeRequest& req : lane->queue) {
                if (req.filePath == path) {
                    queuedForPath = true;
                    break;
                }
            }

            if (!queuedForPath && lane->activeRequests == 0) {
                it = lane->fileStates.erase(it);
            } else {
                ++it;
            }
        }
    }
}

int AsyncDecoder::resolveWorkerCount(int requestedCount) const {
    if (requestedCount > 0) {
        return qBound(1, requestedCount, kMaxDecoderLaneCount);
    }
    return qBound(2, QThread::idealThreadCount(), 6);
}

int AsyncDecoder::totalPendingRequests() const {
    int total = 0;
    for (const auto& lane : m_lanes) {
        std::unique_lock<std::mutex> lock(lane->mutex);
        total += static_cast<int>(lane->queue.size()) + lane->activeRequests;
    }
    return total;
}

int AsyncDecoder::laneIndexForRequest(const QString& path,
                                      int64_t frameNumber,
                                      DecodeRequestKind kind) const {
    if (m_lanes.empty()) {
        return -1;
    }

    const uint laneCount = static_cast<uint>(m_lanes.size());
    const uint baseHash = qHash(path);

    if (laneCount == 1 || isStillImagePath(path)) {
        return static_cast<int>(baseHash % laneCount);
    }

    if (isImageSequencePath(path)) {
        // Image sequences benefit from aggressive striping because random access
        // cost is dominated by per-frame file I/O and decode.
        const uint frameHash = static_cast<uint>(qMax<int64_t>(0, frameNumber));
        return static_cast<int>((baseHash + frameHash) % laneCount);
    }

    Q_UNUSED(frameNumber);
    Q_UNUSED(kind);
    // Regular video decode is stateful and currently serialized inside
    // DecoderContext. Keep one lane/context per file so playback advances
    // sequentially instead of making multiple workers seek the same stream.
    return static_cast<int>(baseHash % laneCount);
}

AsyncDecoder::LaneState* AsyncDecoder::laneForRequest(const QString& path,
                                                      int64_t frameNumber,
                                                      DecodeRequestKind kind) const {
    const int index = laneIndexForRequest(path, frameNumber, kind);
    if (index < 0 || index >= m_lanes.size()) {
        return nullptr;
    }
    return m_lanes[index].get();
}

std::vector<AsyncDecoder::LaneState*> AsyncDecoder::lanesForPath(const QString& path) const {
    std::vector<LaneState*> lanes;
    if (m_lanes.empty()) {
        return lanes;
    }

    const bool shardedPath =
        isImageSequencePath(path) || (!isStillImagePath(path) && m_lanes.size() > 2);
    if (!shardedPath) {
        if (LaneState* lane = laneForRequest(path, 0, DecodeRequestKind::Visible)) {
            lanes.push_back(lane);
        }
        return lanes;
    }

    lanes.reserve(m_lanes.size());
    for (const auto& lane : m_lanes) {
        lanes.push_back(lane.get());
    }
    return lanes;
}


void AsyncDecoder::startLane(LaneState* lane) {
    if (!lane || lane->thread) {
        return;
    }

    lane->running = true;
    lane->thread = std::make_unique<std::thread>([this, lane]() {
        runLane(lane);
    });
}

void AsyncDecoder::stopLane(LaneState* lane) {
    if (!lane) {
        return;
    }

    {
        std::unique_lock<std::mutex> lock(lane->mutex);
        lane->shuttingDown = true;
        lane->condition.notify_all();
    }

    if (lane->thread && lane->thread->joinable()) {
        lane->thread->join();
    }

    lane->thread.reset();
    lane->running = false;
}

void AsyncDecoder::runLane(LaneState* lane) {
    while (true) {
        DecodeRequest request;
        std::shared_ptr<LaneState::FileDecodeState> state;

        {
            std::unique_lock<std::mutex> lock(lane->mutex);
            lane->condition.wait(lock, [lane]() {
                return lane->shuttingDown || !lane->queue.empty();
            });

            if (lane->shuttingDown) {
                return;
            }

            request = std::move(lane->queue.front());
            lane->queue.pop_front();
            ++lane->activeRequests;

            auto stateIt = lane->fileStates.find(request.filePath);
            if (stateIt == lane->fileStates.end()) {
                stateIt = lane->fileStates.insert(request.filePath, std::make_shared<LaneState::FileDecodeState>());
            }
            state = stateIt.value();
        }

        const qint64 startedAt = decodeTraceMs();
        const qint64 startedAtWallMs = QDateTime::currentMSecsSinceEpoch();
        const qint64 queueWaitMs =
            request.submittedAt > 0 ? startedAtWallMs - request.submittedAt : -1;
        decodeTrace(QStringLiteral("AsyncDecoder::runLane.begin"),
                    QStringLiteral("lane=%1 seq=%2 file=%3 frame=%4 priority=%5 thread=%6")
                        .arg(lane->index)
                        .arg(request.sequenceId)
                        .arg(shortPath(request.filePath))
                        .arg(request.frameNumber)
                        .arg(request.priority)
                        .arg(reinterpret_cast<quintptr>(QThread::currentThreadId()), 0, 16));

        FrameHandle frame;
        QVector<FrameHandle> decodedFrames;
        QString errorMessage;
        bool visibleExactMiss = false;

        const bool cancelled =
            request.generation != state->generation.load() ||
            (request.kind != DecodeRequestKind::Visible &&
             request.frameNumber < state->cancelBeforeFrame.load());

        if (!request.isExpired() && !cancelled) {
            if (!state->context) {
                // Pass the shared device map so the context borrows a ref
                // rather than calling av_hwdevice_ctx_create().
                QMutexLocker hwLock(&m_sharedHwMutex);
                const QHash<int, AVBufferRef*>* sharedDevices = &m_sharedHwDevices;
                hwLock.unlock();

                state->context = std::make_unique<DecoderContext>(request.filePath, sharedDevices);
                if (!state->context->initialize()) {
                    errorMessage = QStringLiteral("Failed to create decoder context");
                    state->context.reset();
                }
            }

            if (state->context) {
                const bool publishDecodedBatch =
                    request.kind != DecodeRequestKind::Preload &&
                    (state->context->supportsSequenceBatchDecode() ||
                     request.kind == DecodeRequestKind::Visible ||
                     request.kind == DecodeRequestKind::Prefetch);
                if (publishDecodedBatch) {
                    decodedFrames = state->context->decodeThroughFrame(request.frameNumber);
                    for (const FrameHandle& decodedFrame : decodedFrames) {
                        if (!decodedFrame.isNull() && decodedFrame.frameNumber() == request.frameNumber) {
                            frame = decodedFrame;
                            break;
                        }
                    }
                    if (frame.isNull() && !decodedFrames.isEmpty()) {
                        visibleExactMiss = request.kind == DecodeRequestKind::Visible;
                    }
                } else {
                    frame = state->context->decodeFrame(request.frameNumber);
                }
            }
        }

        const bool staleAfterDecode = request.frameNumber < state->cancelBeforeFrame.load();
        const bool expiredAfterDecode = request.isExpired();
        const bool discardAfterDecode =
            (expiredAfterDecode && frame.isNull()) ||
            request.generation != state->generation.load() ||
            (staleAfterDecode && frame.isNull());
        if (discardAfterDecode) {
            if (request.isExpired()) {
                recordNullCallback(request.kind, "expired");
            } else if (request.generation != state->generation.load()) {
                recordNullCallback(request.kind, "generation_cancelled");
            } else if (staleAfterDecode) {
                recordNullCallback(request.kind, "stale_after_decode");
            }
            frame = FrameHandle();
            decodedFrames.clear();
        } else if (frame.isNull()) {
            recordNullCallback(request.kind,
                               visibleExactMiss ? "visible_exact_miss"
                                                : (errorMessage.isEmpty() ? "decode_returned_null"
                                                                          : "decoder_context_error"));
        }

        const qint64 decodeMs = decodeTraceMs() - startedAt;
        recordDecodeTiming(request, frame, queueWaitMs, decodeMs);

        invokeRequestCallback(std::move(request.callback), frame);

        QMetaObject::invokeMethod(
            this,
            [this, frame, decodedFrames, path = request.filePath, errorMessage]() {
                if (!decodedFrames.isEmpty()) {
                    for (const FrameHandle& decodedFrame : decodedFrames) {
                        emit frameReady(decodedFrame);
                    }
                } else {
                    emit frameReady(frame);
                }

                if (!errorMessage.isEmpty()) {
                    emit error(path, errorMessage);
                }
            },
            Qt::QueuedConnection);

        decodeTrace(QStringLiteral("AsyncDecoder::runLane.end"),
                    QStringLiteral("lane=%1 seq=%2 file=%3 frame=%4 null=%5 waitMs=%6")
                        .arg(lane->index)
                        .arg(request.sequenceId)
                        .arg(shortPath(request.filePath))
                        .arg(request.frameNumber)
                        .arg(frame.isNull())
                        .arg(decodeTraceMs() - startedAt));

        {
            std::unique_lock<std::mutex> lock(lane->mutex);
            lane->activeRequests = qMax(0, lane->activeRequests - 1);

            if (lane->fileStates.size() > LaneState::kMaxContexts) {
                qint64 oldestTime = std::numeric_limits<qint64>::max();
                QString oldestPath;

                for (auto it = lane->fileStates.begin(); it != lane->fileStates.end(); ++it) {
                    if (!it.value()->context) {
                        oldestPath = it.key();
                        break;
                    }

                    bool queuedForPath = false;
                    for (const DecodeRequest& req : lane->queue) {
                        if (req.filePath == it.key()) {
                            queuedForPath = true;
                            break;
                        }
                    }
                    if (queuedForPath) {
                        continue;
                    }

                    const qint64 accessTime = it.value()->context->lastAccessTime();
                    if (accessTime < oldestTime) {
                        oldestTime = accessTime;
                        oldestPath = it.key();
                    }
                }

                if (!oldestPath.isEmpty()) {
                    lane->fileStates.remove(oldestPath);
                }
            }
        }

        emit queuePressureChanged(totalPendingRequests());
    }
}

void AsyncDecoder::insertByPriority(std::deque<DecodeRequest>& queue, const DecodeRequest& req) {
    auto insertAt = queue.begin();
    for (; insertAt != queue.end(); ++insertAt) {
        if (insertAt->priority < req.priority) {
            break;
        }
    }
    queue.insert(insertAt, req);
}

void AsyncDecoder::collectSupersededRequests(const DecodeRequest& req,
                                             std::deque<DecodeRequest>& queue,
                                             QVector<DroppedCallback>* droppedCallbacks) {
    if (!droppedCallbacks) {
        return;
    }

    for (int i = static_cast<int>(queue.size()) - 1; i >= 0; --i) {
        const DecodeRequest& queued = queue[static_cast<size_t>(i)];
        if (queued.filePath != req.filePath) {
            continue;
        }
        if (queued.frameNumber >= req.frameNumber) {
            continue;
        }
        if (queued.priority > req.priority) {
            continue;
        }
        if (queued.kind == DecodeRequestKind::Visible &&
            req.kind == DecodeRequestKind::Visible) {
            continue;
        }
        if (queued.kind == DecodeRequestKind::Visible &&
            req.kind != DecodeRequestKind::Visible) {
            continue;
        }

        if (queue[static_cast<size_t>(i)].callback) {
            droppedCallbacks->push_back(
                DroppedCallback{queue[static_cast<size_t>(i)].kind,
                                std::move(queue[static_cast<size_t>(i)].callback)});
        }
        queue.erase(queue.begin() + i);
    }
}

void AsyncDecoder::recordNullCallback(DecodeRequestKind kind, const char* reason) {
    m_nullCallbackCounters.total.fetch_add(1, std::memory_order_relaxed);
    if (kind == DecodeRequestKind::Visible) {
        m_nullCallbackCounters.visible.fetch_add(1, std::memory_order_relaxed);
    }

    const QString reasonText = QString::fromLatin1(reason ? reason : "unknown");
    if (reasonText == QLatin1String("superseded")) {
        m_nullCallbackCounters.superseded.fetch_add(1, std::memory_order_relaxed);
    } else if (reasonText == QLatin1String("queue_full")) {
        m_nullCallbackCounters.queueFull.fetch_add(1, std::memory_order_relaxed);
    } else if (reasonText == QLatin1String("cancel_file")) {
        m_nullCallbackCounters.cancelFile.fetch_add(1, std::memory_order_relaxed);
    } else if (reasonText == QLatin1String("cancel_before")) {
        m_nullCallbackCounters.cancelBefore.fetch_add(1, std::memory_order_relaxed);
    } else if (reasonText == QLatin1String("cancel_all")) {
        m_nullCallbackCounters.cancelAll.fetch_add(1, std::memory_order_relaxed);
    } else if (reasonText == QLatin1String("lane_unavailable")) {
        m_nullCallbackCounters.laneUnavailable.fetch_add(1, std::memory_order_relaxed);
    } else if (reasonText == QLatin1String("expired")) {
        m_nullCallbackCounters.expired.fetch_add(1, std::memory_order_relaxed);
    } else if (reasonText == QLatin1String("generation_cancelled")) {
        m_nullCallbackCounters.generationCancelled.fetch_add(1, std::memory_order_relaxed);
    } else if (reasonText == QLatin1String("stale_after_decode")) {
        m_nullCallbackCounters.staleAfterDecode.fetch_add(1, std::memory_order_relaxed);
    } else if (reasonText == QLatin1String("decode_returned_null")) {
        m_nullCallbackCounters.decodeReturnedNull.fetch_add(1, std::memory_order_relaxed);
    } else if (reasonText == QLatin1String("decoder_context_error")) {
        m_nullCallbackCounters.decoderContextError.fetch_add(1, std::memory_order_relaxed);
    } else if (reasonText == QLatin1String("visible_exact_miss")) {
        m_nullCallbackCounters.visibleExactMiss.fetch_add(1, std::memory_order_relaxed);
    }
}

void AsyncDecoder::recordDecodeTiming(const DecodeRequest& request,
                                      const FrameHandle& frame,
                                      qint64 queueWaitMs,
                                      qint64 decodeMs) {
    m_decodeTimingCounters.totalCompleted.fetch_add(1, std::memory_order_relaxed);
    if (request.kind == DecodeRequestKind::Visible) {
        m_decodeTimingCounters.visibleCompleted.fetch_add(1, std::memory_order_relaxed);
    }
    if (frame.isNull()) {
        m_decodeTimingCounters.nullCompleted.fetch_add(1, std::memory_order_relaxed);
    } else if (frame.hasHardwareFrame()) {
        m_decodeTimingCounters.hardwareCompleted.fetch_add(1, std::memory_order_relaxed);
    } else if (frame.hasGpuTexture()) {
        m_decodeTimingCounters.gpuTextureCompleted.fetch_add(1, std::memory_order_relaxed);
    } else {
        m_decodeTimingCounters.cpuImageCompleted.fetch_add(1, std::memory_order_relaxed);
    }

    const qint64 completedAtMs = QDateTime::currentMSecsSinceEpoch();
    const qint64 totalLatencyMs =
        request.submittedAt > 0 ? completedAtMs - request.submittedAt : -1;
    m_decodeTimingCounters.lastFrame.store(request.frameNumber, std::memory_order_relaxed);
    m_decodeTimingCounters.lastQueueWaitMs.store(queueWaitMs, std::memory_order_relaxed);
    m_decodeTimingCounters.lastDecodeMs.store(decodeMs, std::memory_order_relaxed);
    m_decodeTimingCounters.lastTotalLatencyMs.store(totalLatencyMs, std::memory_order_relaxed);
    m_decodeTimingCounters.lastCompletedAtMs.store(completedAtMs, std::memory_order_relaxed);
    storeAtomicMax(&m_decodeTimingCounters.maxQueueWaitMs, queueWaitMs);
    storeAtomicMax(&m_decodeTimingCounters.maxDecodeMs, decodeMs);
    storeAtomicMax(&m_decodeTimingCounters.maxTotalLatencyMs, totalLatencyMs);
}

QJsonObject AsyncDecoder::diagnosticsSnapshot() const {
    QJsonObject nullCallbacks{
        {QStringLiteral("total"), static_cast<qint64>(m_nullCallbackCounters.total.load(std::memory_order_relaxed))},
        {QStringLiteral("visible"), static_cast<qint64>(m_nullCallbackCounters.visible.load(std::memory_order_relaxed))},
        {QStringLiteral("superseded"), static_cast<qint64>(m_nullCallbackCounters.superseded.load(std::memory_order_relaxed))},
        {QStringLiteral("queue_full"), static_cast<qint64>(m_nullCallbackCounters.queueFull.load(std::memory_order_relaxed))},
        {QStringLiteral("cancel_file"), static_cast<qint64>(m_nullCallbackCounters.cancelFile.load(std::memory_order_relaxed))},
        {QStringLiteral("cancel_before"), static_cast<qint64>(m_nullCallbackCounters.cancelBefore.load(std::memory_order_relaxed))},
        {QStringLiteral("cancel_all"), static_cast<qint64>(m_nullCallbackCounters.cancelAll.load(std::memory_order_relaxed))},
        {QStringLiteral("lane_unavailable"), static_cast<qint64>(m_nullCallbackCounters.laneUnavailable.load(std::memory_order_relaxed))},
        {QStringLiteral("expired"), static_cast<qint64>(m_nullCallbackCounters.expired.load(std::memory_order_relaxed))},
        {QStringLiteral("generation_cancelled"), static_cast<qint64>(m_nullCallbackCounters.generationCancelled.load(std::memory_order_relaxed))},
        {QStringLiteral("stale_after_decode"), static_cast<qint64>(m_nullCallbackCounters.staleAfterDecode.load(std::memory_order_relaxed))},
        {QStringLiteral("decode_returned_null"), static_cast<qint64>(m_nullCallbackCounters.decodeReturnedNull.load(std::memory_order_relaxed))},
        {QStringLiteral("decoder_context_error"), static_cast<qint64>(m_nullCallbackCounters.decoderContextError.load(std::memory_order_relaxed))},
        {QStringLiteral("visible_exact_miss"), static_cast<qint64>(m_nullCallbackCounters.visibleExactMiss.load(std::memory_order_relaxed))}
    };

    const qint64 nowMs = QDateTime::currentMSecsSinceEpoch();
    const qint64 lastCompletedAtMs =
        m_decodeTimingCounters.lastCompletedAtMs.load(std::memory_order_relaxed);
    QJsonObject decodeTiming{
        {QStringLiteral("total_completed"),
         static_cast<qint64>(m_decodeTimingCounters.totalCompleted.load(std::memory_order_relaxed))},
        {QStringLiteral("visible_completed"),
         static_cast<qint64>(m_decodeTimingCounters.visibleCompleted.load(std::memory_order_relaxed))},
        {QStringLiteral("hardware_completed"),
         static_cast<qint64>(m_decodeTimingCounters.hardwareCompleted.load(std::memory_order_relaxed))},
        {QStringLiteral("gpu_texture_completed"),
         static_cast<qint64>(m_decodeTimingCounters.gpuTextureCompleted.load(std::memory_order_relaxed))},
        {QStringLiteral("cpu_image_completed"),
         static_cast<qint64>(m_decodeTimingCounters.cpuImageCompleted.load(std::memory_order_relaxed))},
        {QStringLiteral("null_completed"),
         static_cast<qint64>(m_decodeTimingCounters.nullCompleted.load(std::memory_order_relaxed))},
        {QStringLiteral("last_frame"),
         static_cast<qint64>(m_decodeTimingCounters.lastFrame.load(std::memory_order_relaxed))},
        {QStringLiteral("last_queue_wait_ms"),
         static_cast<qint64>(m_decodeTimingCounters.lastQueueWaitMs.load(std::memory_order_relaxed))},
        {QStringLiteral("max_queue_wait_ms"),
         static_cast<qint64>(m_decodeTimingCounters.maxQueueWaitMs.load(std::memory_order_relaxed))},
        {QStringLiteral("last_decode_ms"),
         static_cast<qint64>(m_decodeTimingCounters.lastDecodeMs.load(std::memory_order_relaxed))},
        {QStringLiteral("max_decode_ms"),
         static_cast<qint64>(m_decodeTimingCounters.maxDecodeMs.load(std::memory_order_relaxed))},
        {QStringLiteral("last_total_latency_ms"),
         static_cast<qint64>(m_decodeTimingCounters.lastTotalLatencyMs.load(std::memory_order_relaxed))},
        {QStringLiteral("max_total_latency_ms"),
         static_cast<qint64>(m_decodeTimingCounters.maxTotalLatencyMs.load(std::memory_order_relaxed))},
        {QStringLiteral("last_completed_age_ms"),
         lastCompletedAtMs > 0 ? nowMs - lastCompletedAtMs : static_cast<qint64>(-1)}
    };

    return QJsonObject{
        {QStringLiteral("pending_requests"), totalPendingRequests()},
        {QStringLiteral("worker_count"), m_workerCount},
        {QStringLiteral("null_callbacks"), nullCallbacks},
        {QStringLiteral("decode_timing"), decodeTiming}
    };
}

void AsyncDecoder::initSharedHwDevices() {
    // Attempt to create one device context per supported hardware type.
    // On success, all DecoderContexts borrow a ref via acquireSharedHwDevice()
    // instead of calling av_hwdevice_ctx_create() themselves.
    // This reduces CUDA context count from O(workers × files) → O(1).

    static const AVHWDeviceType kTypes[] = {
#ifdef __APPLE__
        AV_HWDEVICE_TYPE_VIDEOTOOLBOX,
#elif defined(_WIN32)
        AV_HWDEVICE_TYPE_D3D11VA,
        AV_HWDEVICE_TYPE_DXVA2,
#endif
        AV_HWDEVICE_TYPE_CUDA,
        AV_HWDEVICE_TYPE_VAAPI,
    };

    QMutexLocker lock(&m_sharedHwMutex);

    for (AVHWDeviceType type : kTypes) {
        const char* deviceName = nullptr;

#if defined(Q_OS_LINUX)
        QByteArray devicePath;
        if (type == AV_HWDEVICE_TYPE_VAAPI) {
            static const char* kRenderNodes[] = {
                "/dev/dri/renderD128",
                "/dev/dri/renderD129",
                "/dev/dri/renderD130",
            };
            for (const char* candidate : kRenderNodes) {
                if (QFile::exists(QString::fromLatin1(candidate))) {
                    devicePath = QByteArray(candidate);
                    deviceName = devicePath.constData();
                    break;
                }
            }
            if (!deviceName) {
                continue;
            }
        }
#endif

        AVBufferRef* hwCtx = nullptr;
        const int ret = av_hwdevice_ctx_create(&hwCtx, type, deviceName, nullptr, 0);
        const char* typeName = av_hwdevice_get_type_name(type);
        const QString typeLabel = typeName
            ? QString::fromLatin1(typeName)
            : QStringLiteral("unknown");
        if (ret >= 0 && hwCtx) {
            m_sharedHwDevices.insert(static_cast<int>(type), hwCtx);
            qDebug() << "AsyncDecoder: shared hw device created for type" << type
                     << "(" << typeLabel << ")";
        } else {
            qDebug() << "AsyncDecoder: shared hw device unavailable for type" << type
                     << "(" << typeLabel << ")"
                     << "— decoders requiring this type will fall back";
        }
    }
}

void AsyncDecoder::releaseSharedHwDevices() {
    QMutexLocker lock(&m_sharedHwMutex);
    for (auto it = m_sharedHwDevices.begin(); it != m_sharedHwDevices.end(); ++it) {
        if (it.value()) {
            av_buffer_unref(&it.value());
        }
    }
    m_sharedHwDevices.clear();
}

AVBufferRef* AsyncDecoder::acquireSharedHwDevice(AVHWDeviceType type) const {
    QMutexLocker lock(&m_sharedHwMutex);
    auto it = m_sharedHwDevices.find(static_cast<int>(type));
    if (it == m_sharedHwDevices.end() || !it.value()) {
        return nullptr;
    }
    return av_buffer_ref(it.value());
}

} // namespace editor
