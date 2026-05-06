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
constexpr int64_t kImageSequenceLaneShardSize = 8;
constexpr int64_t kVideoLaneShardSize = 24;
constexpr int kMaxDecoderLaneCount = 16;
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

    QVector<std::function<void(FrameHandle)>> droppedCallbacks;
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
                            droppedCallbacks.push_back(std::move(it->callback));
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
        invokeRequestCallback(std::move(droppedCallback), FrameHandle());
    }

    if (!accepted) {
        decodeTrace(QStringLiteral("AsyncDecoder::requestFrame.queue-full"),
                    QStringLiteral("seq=%1 file=%2 frame=%3 kind=%4")
                        .arg(seqId)
                        .arg(shortPath(path))
                        .arg(frameNumber)
                        .arg(static_cast<int>(kind)));
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

    QVector<std::function<void(FrameHandle)>> callbacks;

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
                    callbacks.push_back(std::move(it->callback));
                }
                it = lane->queue.erase(it);
            } else {
                ++it;
            }
        }
    }

    for (auto& callback : callbacks) {
        invokeRequestCallback(std::move(callback), FrameHandle());
    }

    emit queuePressureChanged(totalPendingRequests());
}

void AsyncDecoder::cancelForFileBefore(const QString& path, int64_t frameNumber) {
    const std::vector<LaneState*> lanes = lanesForPath(path);
    if (lanes.empty()) {
        return;
    }

    QVector<std::function<void(FrameHandle)>> callbacks;

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
                    callbacks.push_back(std::move(it->callback));
                }
                it = lane->queue.erase(it);
            } else {
                ++it;
            }
        }
    }

    for (auto& callback : callbacks) {
        invokeRequestCallback(std::move(callback), FrameHandle());
    }

    emit queuePressureChanged(totalPendingRequests());
}

void AsyncDecoder::cancelAll() {
    QVector<std::function<void(FrameHandle)>> callbacks;

    for (const auto& lane : m_lanes) {
        std::unique_lock<std::mutex> lock(lane->mutex);

        for (auto& state : lane->fileStates) {
            state->generation.fetch_add(1);
            state->cancelBeforeFrame.store(std::numeric_limits<int64_t>::min());
        }

        for (DecodeRequest& request : lane->queue) {
            if (request.callback) {
                callbacks.push_back(std::move(request.callback));
            }
        }
        lane->queue.clear();
    }

    for (auto& callback : callbacks) {
        invokeRequestCallback(std::move(callback), FrameHandle());
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

    // For regular video, keep nearby frames on the same lane while allowing
    // seeks/prefetch windows to fan out across workers.
    const bool shouldShardVideo =
        laneCount > 2 && kind != DecodeRequestKind::Visible;
    if (!shouldShardVideo) {
        return static_cast<int>(baseHash % laneCount);
    }

    const uint frameShard =
        static_cast<uint>(qMax<int64_t>(0, frameNumber) / kVideoLaneShardSize);
    return static_cast<int>((baseHash + frameShard) % laneCount);
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

        const bool cancelled =
            request.generation != state->generation.load() ||
            request.frameNumber < state->cancelBeforeFrame.load();

        if (!request.isExpired() && !cancelled) {
            if (!state->context) {
                // Pass the shared device map so the context borrows a ref
                // rather than calling av_hwdevice_ctx_create().
                QMutexLocker hwLock(&m_sharedHwMutex);
                const QHash<int, AVBufferRef*>* sharedDevices =
                    m_sharedHwDevices.isEmpty() ? nullptr : &m_sharedHwDevices;
                hwLock.unlock();

                state->context = std::make_unique<DecoderContext>(request.filePath, sharedDevices);
                if (!state->context->initialize()) {
                    errorMessage = QStringLiteral("Failed to create decoder context");
                    state->context.reset();
                }
            }

            if (state->context) {
                if (state->context->supportsSequenceBatchDecode() &&
                    request.kind != DecodeRequestKind::Preload) {
                    decodedFrames = state->context->decodeThroughFrame(request.frameNumber);
                    for (const FrameHandle& decodedFrame : decodedFrames) {
                        if (!decodedFrame.isNull() && decodedFrame.frameNumber() == request.frameNumber) {
                            frame = decodedFrame;
                            break;
                        }
                    }
                    if (frame.isNull() && !decodedFrames.isEmpty()) {
                        frame = decodedFrames.constFirst();
                    }
                } else {
                    frame = state->context->decodeFrame(request.frameNumber);
                }
            }
        }

        if (request.isExpired() ||
            request.generation != state->generation.load() ||
            request.frameNumber < state->cancelBeforeFrame.load()) {
            frame = FrameHandle();
            decodedFrames.clear();
        }

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
                                             QVector<std::function<void(FrameHandle)>>* droppedCallbacks) {
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
            req.kind != DecodeRequestKind::Visible) {
            continue;
        }

        if (queue[static_cast<size_t>(i)].callback) {
            droppedCallbacks->push_back(std::move(queue[static_cast<size_t>(i)].callback));
        }
        queue.erase(queue.begin() + i);
    }
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
