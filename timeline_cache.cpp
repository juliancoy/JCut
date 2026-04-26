#include "timeline_cache.h"
#include "debug_controls.h"
#include "media_pipeline_shared.h"

#include <QDateTime>
#include <QDebug>
#include <QElapsedTimer>
#include <QFileInfo>
#include <QHash>
#include <QMetaObject>
#include <QMutexLocker>
#include <QPointer>
#include <QSet>
#include <QTimer>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <limits>
#include <mutex>
#include <utility>

namespace editor {

namespace {
constexpr int64_t kVisibleDecodeKeepWindow = 10;
constexpr int64_t kObsoleteVisibleFrameSlack = 0;
constexpr int64_t kSeekGenerationDeltaFrames = 6;
constexpr qint64 kVisiblePendingRetryMs = 250;
constexpr qint64 kSeekResyncWindowMs = 400;
constexpr qint64 kCancelBeforeMinIntervalMs = 45;
constexpr int64_t kCancelBeforeMinFrameAdvance = 6;

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
        // Rate-limit high-frequency cache operations
        const bool isHighFreqStage = 
            stage.startsWith(QStringLiteral("TimelineCache::onPrefetchTimer")) ||
            stage.startsWith(QStringLiteral("TimelineCache::prefetch.skip")) ||
            stage.startsWith(QStringLiteral("TimelineCache::prefetch.dispatch")) ||
            stage.startsWith(QStringLiteral("TimelineCache::prefetch.complete")) ||
            stage.startsWith(QStringLiteral("TimelineCache::lead-prefetch.dispatch")) ||
            stage.startsWith(QStringLiteral("TimelineCache::lead-prefetch.complete")) ||
            stage.startsWith(QStringLiteral("TimelineCache::requestFrame.miss")) ||
            stage.startsWith(QStringLiteral("TimelineCache::requestFrame.dispatch")) ||
            stage.startsWith(QStringLiteral("TimelineCache::requestFrame.complete"));
        if (isHighFreqStage) {
            std::lock_guard<std::mutex> lock(logMutex);
            const qint64 last = lastLogByStage.value(stage, std::numeric_limits<qint64>::min());
            if (now - last < 500) {  // Increased to 500ms for less spam
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

bool isSingleFramePath(const QString& path) {
    static const QSet<QString> suffixes = {
        QStringLiteral("png"),
        QStringLiteral("jpg"),
        QStringLiteral("jpeg"),
        QStringLiteral("bmp"),
        QStringLiteral("gif"),
        QStringLiteral("webp"),
        QStringLiteral("tif"),
        QStringLiteral("tiff")
    };
    return suffixes.contains(QFileInfo(path).suffix().toLower());
}
} // namespace


// ============================================================================
// TimelineCache Implementation
// ============================================================================

TimelineCache::TimelineCache(AsyncDecoder* decoder, MemoryBudget* budget, QObject* parent)
    : QObject(parent), m_decoder(decoder), m_budget(budget) {
    m_prefetchTimer.setInterval(16);
    connect(&m_prefetchTimer, &QTimer::timeout, this, &TimelineCache::onPrefetchTimer);
    if (m_decoder) {
        connect(m_decoder, &AsyncDecoder::frameReady, this, &TimelineCache::onFrameDecoded);
    }

    if (m_budget) {
        connect(m_budget, &MemoryBudget::trimRequested, this, &TimelineCache::onMemoryPressure);
    }
}

TimelineCache::~TimelineCache() {
    m_aliveToken->store(false);
    stopPrefetching();

    size_t releasedMemory = 0;
    for (ClipCache* cache : m_caches) {
        releasedMemory += cache->memoryUsage();
        delete cache;
    }
    m_caches.clear();

    for (PlaybackBuffer* buffer : m_playbackBuffers) {
        delete buffer;
    }
    m_playbackBuffers.clear();

    Q_UNUSED(releasedMemory)
}

void TimelineCache::setMaxMemory(size_t bytes) {
    if (m_budget) {
        m_budget->setMaxCpuMemory((bytes * 3) / 4);
        m_budget->setMaxGpuMemory(bytes / 4);
    }
}

void TimelineCache::setPlayheadFrame(int64_t frame) {
    const int64_t previous = m_playhead.exchange(frame);
    const bool discontinuousJump = qAbs(frame - previous) > kSeekGenerationDeltaFrames;
    if (discontinuousJump) {
        m_visibleRequestGeneration.fetch_add(1);
        beginSeekResyncForPlayhead(frame);
    }

    if (m_state.load() == PlaybackState::Playing) {
        dropStaleRequestsForPlayhead(frame);
    }
}

void TimelineCache::setPlaybackState(PlaybackState state) {
    const PlaybackState previous = m_state.exchange(state);
    if (previous == state) {
        return;
    }

    if (state == PlaybackState::Playing) {
        return;
    }

    {
        QMutexLocker pendingLock(&m_pendingMutex);
        m_seekResync.reset();
    }

    QMutexLocker lock(&m_clipsMutex);
    for (PlaybackBuffer* buffer : m_playbackBuffers) {
        if (buffer) {
            buffer->clear();
        }
    }
}

void TimelineCache::setExportRanges(const QVector<ExportRangeSegment>& ranges) {
    QMutexLocker lock(&m_exportRangesMutex);
    m_exportRanges = ranges;
}

void TimelineCache::setRenderSyncMarkers(const QVector<RenderSyncMarker>& markers) {
    QMutexLocker lock(&m_exportRangesMutex);
    m_renderSyncMarkers = markers;
}

static int64_t nextValidFrame(int64_t currentFrame, int step, const QVector<ExportRangeSegment>& ranges) {
    if (ranges.isEmpty()) {
        return currentFrame + step;
    }

    if (step > 0) {
        for (const auto& range : ranges) {
            if (currentFrame < range.startFrame) {
                return range.startFrame;
            }
            if (currentFrame >= range.startFrame && currentFrame < range.endFrame) {
                return currentFrame + 1;
            }
        }
        return -1;
    }

    for (int i = ranges.size() - 1; i >= 0; --i) {
        const auto& range = ranges[i];
        if (currentFrame > range.endFrame) {
            return range.endFrame;
        }
        if (currentFrame > range.startFrame && currentFrame <= range.endFrame) {
            return currentFrame - 1;
        }
    }
    return -1;
}

void TimelineCache::registerClip(const TimelineClip& clip) {
    QMutexLocker lock(&m_clipsMutex);

    ClipInfo info;
    info.clip = clip;
    info.decodePath = interactivePreviewMediaPathForClip(clip);
    info.isSingleFrame = isSingleFramePath(info.decodePath.isEmpty() ? clip.filePath : info.decodePath);

    m_clips[clip.id] = info;
    m_caches[clip.id] = new ClipCache(info.decodePath.isEmpty() ? clip.filePath : info.decodePath,
                                      clip.durationFrames,
                                      m_budget);
    m_playbackBuffers[clip.id] = new PlaybackBuffer();

    // Pre-cache static images immediately
    // For static images, we always want frame 0
    if (info.isSingleFrame && m_decoder) {
        const int64_t frameNumber = 0;
        const QString key = requestKey(clip.id, frameNumber);

        // Check if already cached - we need to release the mutex first
        // because getCachedFrame will try to lock it again
        lock.unlock();
        FrameHandle cached = getCachedFrame(clip.id, frameNumber);
        lock.relock();
        
        if (cached.isNull()) {
            // Request the static image frame with high priority
            m_decoder->requestFrame(info.decodePath.isEmpty() ? clip.filePath : info.decodePath,
                                   frameNumber,
                                   95,  // Very high priority for static images (increased from 90)
                                   5000, // 5 second timeout (reduced from 10)
                                   DecodeRequestKind::Visible, // Use Visible instead of Prefetch for immediate attention
                                   [this, clipId = clip.id, frameNumber](FrameHandle frame) {
                if (!frame.isNull()) {
                    // Store in cache
                    QMutexLocker lock(&m_clipsMutex);
                    auto it = m_clips.find(clipId);
                    if (it != m_clips.end()) {
                        ClipCache* cache = m_caches.value(clipId);
                        if (cache) {
                            cache->insert(frameNumber, frame);
                            // Emit signal that a static image frame was loaded
                            // This will trigger repaint in preview windows
                            QMetaObject::invokeMethod(this, [this, clipId, frameNumber, frame]() {
                                emit frameLoaded(clipId, frameNumber, frame);
                            }, Qt::QueuedConnection);
                        }
                    }
                } else {
                    // Log warning if static image fails to load
                    qWarning() << "Failed to load static image for clip" << clipId << "frame" << frameNumber;
                }
            });
        }
    }
}

void TimelineCache::registerClip(const QString& id, const QString& path,
                                 int64_t startFrame, int64_t duration) {
    TimelineClip clip;
    clip.id = id;
    clip.filePath = path;
    clip.startFrame = startFrame;
    clip.durationFrames = duration;
    clip.sourceDurationFrames = duration;
    registerClip(clip);
}

void TimelineCache::unregisterClip(const QString& id) {
    QMutexLocker lock(&m_clipsMutex);

    const QString decodePath =
        m_clips.contains(id) ? m_clips.value(id).decodePath : QString();
    m_clips.remove(id);
    ClipCache* cache = m_caches.take(id);
    PlaybackBuffer* playbackBuffer = m_playbackBuffers.take(id);
    delete cache;
    delete playbackBuffer;
    lock.unlock();

    QMutexLocker pendingLock(&m_pendingMutex);
    m_latestVisibleTargets.remove(id);
    if (!decodePath.isEmpty()) {
        m_lastCancelKeepFromByPath.remove(decodePath);
        m_lastCancelAtMsByPath.remove(decodePath);
    }
}

void TimelineCache::clearClips() {
    QMutexLocker lock(&m_clipsMutex);

    m_clips.clear();
    for (ClipCache* cache : m_caches) {
        delete cache;
    }
    m_caches.clear();

    for (PlaybackBuffer* buffer : m_playbackBuffers) {
        delete buffer;
    }
    m_playbackBuffers.clear();
    lock.unlock();

    QMutexLocker pendingLock(&m_pendingMutex);
    m_latestVisibleTargets.clear();
    m_lastCancelKeepFromByPath.clear();
    m_lastCancelAtMsByPath.clear();
}

FrameHandle TimelineCache::getCachedFrame(const QString& clipId, int64_t frameNumber) {
    frameNumber = normalizeFrameNumber(clipId, frameNumber);
    QMutexLocker lock(&m_clipsMutex);

    auto it = m_caches.find(clipId);
    if (it == m_caches.end()) {
        return FrameHandle();
    }

    return it.value()->get(frameNumber);
}

FrameHandle TimelineCache::getBestCachedFrame(const QString& clipId, int64_t frameNumber) {
    frameNumber = normalizeFrameNumber(clipId, frameNumber);
    QMutexLocker lock(&m_clipsMutex);

    auto it = m_caches.find(clipId);
    if (it == m_caches.end()) {
        return FrameHandle();
    }

    return it.value()->getBest(frameNumber);
}

FrameHandle TimelineCache::getLatestCachedFrame(const QString& clipId, int64_t frameNumber) {
    frameNumber = normalizeFrameNumber(clipId, frameNumber);
    QMutexLocker lock(&m_clipsMutex);

    auto it = m_caches.find(clipId);
    if (it == m_caches.end()) {
        return FrameHandle();
    }

    return it.value()->getLatestAtOrBefore(frameNumber);
}

FrameHandle TimelineCache::getPlaybackFrame(const QString& clipId, int64_t frameNumber) {
    frameNumber = normalizeFrameNumber(clipId, frameNumber);
    QMutexLocker lock(&m_clipsMutex);

    auto it = m_playbackBuffers.find(clipId);
    if (it == m_playbackBuffers.end()) {
        return FrameHandle();
    }

    return it.value()->get(frameNumber);
}

FrameHandle TimelineCache::getBestPlaybackFrame(const QString& clipId, int64_t frameNumber) {
    frameNumber = normalizeFrameNumber(clipId, frameNumber);
    QMutexLocker lock(&m_clipsMutex);

    auto it = m_playbackBuffers.find(clipId);
    if (it == m_playbackBuffers.end()) {
        return FrameHandle();
    }

    return it.value()->getBest(frameNumber);
}

FrameHandle TimelineCache::getLatestPlaybackFrame(const QString& clipId, int64_t frameNumber) {
    frameNumber = normalizeFrameNumber(clipId, frameNumber);
    QMutexLocker lock(&m_clipsMutex);

    auto it = m_playbackBuffers.find(clipId);
    if (it == m_playbackBuffers.end()) {
        return FrameHandle();
    }

    return it.value()->getLatestAtOrBefore(frameNumber);
}

bool TimelineCache::isFrameCached(const QString& clipId, int64_t frameNumber) const {
    frameNumber = normalizeFrameNumber(clipId, frameNumber);
    QMutexLocker lock(&m_clipsMutex);

    auto it = m_caches.find(clipId);
    if (it == m_caches.end()) {
        return false;
    }

    return it.value()->contains(frameNumber);
}

bool TimelineCache::isPlaybackFrameBuffered(const QString& clipId, int64_t frameNumber) const {
    frameNumber = normalizeFrameNumber(clipId, frameNumber);
    QMutexLocker lock(&m_clipsMutex);

    auto it = m_playbackBuffers.find(clipId);
    if (it == m_playbackBuffers.end()) {
        return false;
    }

    return it.value()->contains(frameNumber);
}

void TimelineCache::startPrefetching() {
    m_prefetchTimer.start();
}

void TimelineCache::stopPrefetching() {
    m_prefetchTimer.stop();
}

int TimelineCache::totalCachedFrames() const {
    QMutexLocker lock(&m_clipsMutex);

    int total = 0;
    for (const auto& cache : m_caches) {
        total += cache->size();
    }
    return total;
}

size_t TimelineCache::totalMemoryUsage() const {
    size_t total = 0;

    QMutexLocker lock(&m_clipsMutex);
    for (ClipCache* cache : m_caches) {
        total += cache->memoryUsage();
    }

    return total;
}

double TimelineCache::cacheHitRate() const {
    const int req = m_requests.load();
    if (req == 0) {
        return 0.0;
    }
    return static_cast<double>(m_hits.load()) / req;
}

void TimelineCache::clearCache() {
    QMutexLocker lock(&m_clipsMutex);

    for (ClipCache* cache : m_caches) {
        delete cache;
    }
    m_caches.clear();

    for (PlaybackBuffer* buffer : m_playbackBuffers) {
        delete buffer;
    }
    m_playbackBuffers.clear();
}

void TimelineCache::trimCache() {
    const bool alreadyTrimming = m_trimInProgress.exchange(true);
    if (alreadyTrimming) {
        cacheTrace(QStringLiteral("TimelineCache::trimCache.skip"),
                   QStringLiteral("reason=reentrant"));
        return;
    }

    const size_t targetMemory =
        m_budget ? static_cast<size_t>(m_budget->maxCpuMemory() * 0.7) : (192 * 1024 * 1024);
    evictOldestFrames(targetMemory);
    m_trimInProgress.store(false);
}

void TimelineCache::preloadRange(const QString& clipId, int64_t startFrame, int64_t endFrame) {
    QMutexLocker lock(&m_clipsMutex);

    auto it = m_clips.find(clipId);
    if (it == m_clips.end()) {
        return;
    }

    const ClipInfo info = it.value();
    lock.unlock();

    constexpr int kStep = 5;

    for (int64_t f = startFrame; f < endFrame; f += kStep) {
        const int64_t normalizedFrame = normalizeFrameNumber(info, f);
        if (isFrameCached(clipId, normalizedFrame)) {
            continue;
        }
        if (info.decodePath.isEmpty()) {
            continue;
        }

        m_decoder->requestFrame(
            info.decodePath,
            normalizedFrame,
            5,
            30000,
            DecodeRequestKind::Preload,
            [this, clipId, normalizedFrame](FrameHandle frame) {
                QMetaObject::invokeMethod(
                    this,
                    [this, clipId, normalizedFrame, frame]() {
                        if (!frame.isNull()) {
                            if (auto* cache = getOrCreateClipCache(clipId)) {
                                cache->insert(normalizedFrame, frame);
                            }
                        }
                    },
                    Qt::QueuedConnection);
            });

        if (info.isSingleFrame) {
            break;
        }
    }
}

void TimelineCache::onPrefetchTimer() {
    if (m_state.load() != PlaybackState::Playing) {
        return;
    }
    schedulePredictiveLoads();
}

void TimelineCache::onFrameDecoded(FrameHandle frame) {
    if (frame.isNull()) {
        return;
    }

    const QString sourcePath = frame.sourcePath();
    if (sourcePath.isEmpty()) {
        return;
    }

    QVector<QPair<QString, int64_t>> targets;
    {
        QMutexLocker lock(&m_clipsMutex);
        for (auto it = m_clips.cbegin(); it != m_clips.cend(); ++it) {
            const ClipInfo& info = it.value();
            if (info.isSingleFrame || info.decodePath != sourcePath) {
                continue;
            }
            targets.push_back(qMakePair(it.key(), normalizeFrameNumber(info, frame.frameNumber())));
        }
    }

    for (const auto& target : targets) {
        if (m_state.load() == PlaybackState::Playing) {
            QMutexLocker lock(&m_clipsMutex);
            auto it = m_playbackBuffers.find(target.first);
            if (it != m_playbackBuffers.end() && it.value()) {
                it.value()->insert(target.second, frame);
            }
        }

        if (ClipCache* cache = getOrCreateClipCache(target.first)) {
            cache->insert(target.second, frame);
        }
        emit frameLoaded(target.first, target.second, frame);
    }
}

void TimelineCache::onMemoryPressure() {
    trimCache();
}

void TimelineCache::dropStaleRequestsForPlayhead(int64_t playheadFrame) {
    QHash<QString, int64_t> activeLocalFrames;
    {
        QMutexLocker lock(&m_clipsMutex);
        for (auto it = m_clips.cbegin(); it != m_clips.cend(); ++it) {
            const ClipInfo& info = it.value();
            if (playheadFrame < info.clip.startFrame ||
                playheadFrame >= info.clip.startFrame + info.clip.durationFrames) {
                continue;
            }

            const int64_t activeSourceFrame =
                sourceFrameForClipAtTimelinePosition(info.clip,
                                                     static_cast<qreal>(playheadFrame),
                                                     m_renderSyncMarkers);
            activeLocalFrames.insert(it.key(), normalizeFrameNumber(info, activeSourceFrame));
        }
    }

    if (activeLocalFrames.isEmpty()) {
        return;
    }

    QVector<QVector<std::function<void(FrameHandle)>>> callbacksToCancel;
    {
        QMutexLocker pendingLock(&m_pendingMutex);

        for (auto it = m_pendingVisibleRequests.begin(); it != m_pendingVisibleRequests.end();) {
            const QString key = it.key();
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
            if (!ok || pendingFrame >= activeIt.value()) {
                ++it;
                continue;
            }

            callbacksToCancel.push_back(it->callbacks);
            it = m_pendingVisibleRequests.erase(it);
        }

        for (auto it = m_pendingPrefetchRequests.begin(); it != m_pendingPrefetchRequests.end();) {
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
            if (!ok || pendingFrame >= activeIt.value()) {
                ++it;
                continue;
            }

            it = m_pendingPrefetchRequests.erase(it);
            m_inflightPrefetches.fetch_sub(1);
        }
    }

    for (const auto& callbacks : callbacksToCancel) {
        for (const auto& cb : callbacks) {
            if (cb) {
                cb(FrameHandle());
            }
        }
    }

    if (m_decoder) {
        QMutexLocker lock(&m_clipsMutex);
        for (auto it = activeLocalFrames.cbegin(); it != activeLocalFrames.cend(); ++it) {
            const auto clipIt = m_clips.find(it.key());
            if (clipIt == m_clips.end() || clipIt->decodePath.isEmpty()) {
                continue;
            }
            const int64_t keepFromFrame = qMax<int64_t>(0, it.value() - kVisibleDecodeKeepWindow);
            cancelDecoderBeforeThrottled(clipIt->decodePath, keepFromFrame);
        }
    }
}

void TimelineCache::cancelDecoderBeforeThrottled(const QString& decodePath,
                                                 int64_t keepFromFrame,
                                                 qint64 nowMs) {
    if (!m_decoder || decodePath.isEmpty()) {
        return;
    }
    if (nowMs < 0) {
        nowMs = QDateTime::currentMSecsSinceEpoch();
    }

    bool shouldCancel = false;
    {
        QMutexLocker pendingLock(&m_pendingMutex);
        const int64_t previousKeepFrom =
            m_lastCancelKeepFromByPath.value(decodePath, std::numeric_limits<int64_t>::min());
        const qint64 previousCancelAt = m_lastCancelAtMsByPath.value(decodePath, 0);
        const bool advancedEnough =
            keepFromFrame >= previousKeepFrom + kCancelBeforeMinFrameAdvance;
        const bool overdue = previousCancelAt <= 0 || (nowMs - previousCancelAt) >= kCancelBeforeMinIntervalMs;
        shouldCancel = advancedEnough || overdue;
        if (shouldCancel) {
            m_lastCancelKeepFromByPath.insert(decodePath, keepFromFrame);
            m_lastCancelAtMsByPath.insert(decodePath, nowMs);
        }
    }

    if (shouldCancel) {
        m_decoder->cancelForFileBefore(decodePath, keepFromFrame);
    }
}

void TimelineCache::beginSeekResyncForPlayhead(int64_t playheadFrame) {
    if (m_state.load() != PlaybackState::Playing || !m_decoder) {
        return;
    }

    struct ResyncTarget {
        QString clipId;
        QString decodePath;
        int64_t targetFrame = 0;
    };

    QVector<ResyncTarget> targets;
    {
        QMutexLocker lock(&m_clipsMutex);
        for (auto it = m_clips.cbegin(); it != m_clips.cend(); ++it) {
            const ClipInfo& info = it.value();
            if (info.isSingleFrame ||
                info.clip.sourceKind == MediaSourceKind::ImageSequence ||
                playheadFrame < info.clip.startFrame ||
                playheadFrame >= info.clip.startFrame + info.clip.durationFrames) {
                continue;
            }

            const int64_t activeSourceFrame =
                sourceFrameForClipAtTimelinePosition(info.clip,
                                                     static_cast<qreal>(playheadFrame),
                                                     m_renderSyncMarkers);
            ResyncTarget target;
            target.clipId = it.key();
            target.decodePath = info.decodePath;
            target.targetFrame = normalizeFrameNumber(info, activeSourceFrame);
            targets.push_back(target);
        }
    }

    if (targets.isEmpty()) {
        return;
    }

    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    {
        QMutexLocker pendingLock(&m_pendingMutex);
        for (const ResyncTarget& target : targets) {
            m_seekResync.begin(target.clipId, target.targetFrame, now, kSeekResyncWindowMs);

            for (auto it = m_pendingVisibleRequests.begin(); it != m_pendingVisibleRequests.end();) {
                if (it.key().startsWith(target.clipId + QLatin1Char(':'))) {
                    it = m_pendingVisibleRequests.erase(it);
                } else {
                    ++it;
                }
            }
            for (auto it = m_pendingPrefetchRequests.begin(); it != m_pendingPrefetchRequests.end();) {
                if (it->startsWith(target.clipId + QLatin1Char(':'))) {
                    it = m_pendingPrefetchRequests.erase(it);
                    m_inflightPrefetches.fetch_sub(1);
                } else {
                    ++it;
                }
            }
            m_latestVisibleTargets.remove(target.clipId);
        }
    }

    QMutexLocker lock(&m_clipsMutex);
    for (const ResyncTarget& target : targets) {
        auto bufferIt = m_playbackBuffers.find(target.clipId);
        if (bufferIt != m_playbackBuffers.end() && bufferIt.value()) {
            bufferIt.value()->clear();
        }
        if (!target.decodePath.isEmpty()) {
            m_decoder->cancelForFile(target.decodePath);
        }
    }
}

void TimelineCache::scheduleImmediateLeadPrefetch(const ClipInfo& info, int64_t canonicalFrame) {
    const int leadPrefetchCount = debugLeadPrefetchEnabled() ? debugLeadPrefetchCount() : 0;
    if (!m_decoder || info.isSingleFrame || m_state.load() != PlaybackState::Playing || leadPrefetchCount <= 0) {
        return;
    }

    if (!shouldAllowApproximatePreviewFrame(info.clip.id, canonicalFrame, QDateTime::currentMSecsSinceEpoch())) {
        return;
    }

    if (info.decodePath.isEmpty()) {
        return;
    }

    QPointer<TimelineCache> self(this);
    const std::shared_ptr<std::atomic<bool>> aliveToken = m_aliveToken;

    for (int offset = 1; offset <= leadPrefetchCount; ++offset) {
        const int64_t targetFrame = normalizeFrameNumber(info, canonicalFrame + offset);
        const QString key = requestKey(info.clip.id, targetFrame);

        {
            QMutexLocker pendingLock(&m_pendingMutex);
            if (m_pendingVisibleRequests.contains(key) || m_pendingPrefetchRequests.contains(key)) {
                continue;
            }
        }

        if (isFrameCached(info.clip.id, targetFrame)) {
            continue;
        }

        {
            QMutexLocker pendingLock(&m_pendingMutex);
            if (m_pendingVisibleRequests.contains(key) || m_pendingPrefetchRequests.contains(key)) {
                continue;
            }
            m_pendingPrefetchRequests.insert(key);
            m_inflightPrefetches.fetch_add(1);
        }

        const int priority = qMax(65, calculatePriority(canonicalFrame) - (offset * 5));
        cacheTrace(QStringLiteral("TimelineCache::lead-prefetch.dispatch"),
                   QStringLiteral("clip=%1 frame=%2 priority=%3")
                       .arg(info.clip.id)
                       .arg(targetFrame)
                       .arg(priority));

        const uint64_t seqId = m_decoder->requestFrame(
            info.decodePath,
            targetFrame,
            priority,
            5000,
            DecodeRequestKind::Prefetch,
            [self, aliveToken, clipId = info.clip.id, targetFrame, key](FrameHandle frame) {
                if (!aliveToken->load() || !self) {
                    return;
                }

                QMetaObject::invokeMethod(
                    self,
                    [self, aliveToken, clipId, targetFrame, key, frame]() {
                        if (!aliveToken->load() || !self) {
                            return;
                        }

                        {
                            QMutexLocker pendingLock(&self->m_pendingMutex);
                            self->m_pendingPrefetchRequests.remove(key);
                            self->m_inflightPrefetches.fetch_sub(1);
                        }

                        if (!frame.isNull()) {
                            if (self->m_state.load() == PlaybackState::Playing) {
                                QMutexLocker lock(&self->m_clipsMutex);
                                auto bufferIt = self->m_playbackBuffers.find(clipId);
                                if (bufferIt != self->m_playbackBuffers.end() && bufferIt.value()) {
                                    bufferIt.value()->insert(targetFrame, frame);
                                }
                            }
                            if (ClipCache* cache = self->getOrCreateClipCache(clipId)) {
                                cache->insert(targetFrame, frame);
                            }
                        }

                        cacheTrace(QStringLiteral("TimelineCache::lead-prefetch.complete"),
                                   QStringLiteral("clip=%1 frame=%2 null=%3")
                                       .arg(clipId)
                                       .arg(targetFrame)
                                       .arg(frame.isNull()));
                    },
                    Qt::QueuedConnection);
            });

        if (seqId == 0) {
            QMutexLocker pendingLock(&m_pendingMutex);
            if (m_pendingPrefetchRequests.remove(key) > 0) {
                m_inflightPrefetches.fetch_sub(1);
            }
        }
    }
}

void TimelineCache::schedulePredictiveLoads() {
    const int maxPrefetchQueueDepth = debugPrefetchMaxQueueDepth();
    const int maxInflightPrefetch = debugPrefetchMaxInflight();
    const int maxPrefetchPerTick = debugPrefetchMaxPerTick();

    const int64_t playhead = m_playhead.load();
    const Direction dir = m_direction.load();
    const double speed = m_speed.load();

    if (!m_decoder) {
        return;
    }

    {
        const qint64 now = QDateTime::currentMSecsSinceEpoch();
        QMutexLocker pendingLock(&m_pendingMutex);
        if (m_seekResync.expireAndCheckAnyActive(now)) {
            return;
        }
    }

    const int pendingVisible = pendingVisibleRequestCount();
    if (pendingVisible > debugPrefetchSkipVisiblePendingThreshold()) {
        cacheTrace(QStringLiteral("TimelineCache::prefetch.skip"),
                   QStringLiteral("reason=visible-pending count=%1").arg(pendingVisible));
        return;
    }

    if (m_decoder->pendingRequestCount() >= maxPrefetchQueueDepth ||
        m_inflightPrefetches.load() >= maxInflightPrefetch) {
        cacheTrace(QStringLiteral("TimelineCache::prefetch.skip"),
                   QStringLiteral("reason=queue-pressure pending=%1 inflight=%2")
                       .arg(m_decoder->pendingRequestCount())
                       .arg(m_inflightPrefetches.load()));
        return;
    }

    const int lookahead = qBound(6,
                                 static_cast<int>(std::ceil(qMax(1.0, speed) *
                                                            (pendingVisible > 0 ? 6.0 : 10.0))),
                                 qMin(m_lookaheadFrames, pendingVisible > 0 ? 12 : 20));
    int scheduledThisTick = 0;

    QVector<ExportRangeSegment> exportRanges;
    {
        QMutexLocker rangesLock(&m_exportRangesMutex);
        exportRanges = m_exportRanges;
    }

    const int step = dir == Direction::Forward ? 1 : -1;

    QVector<editor::SequencePrefetchClip> sequenceClips;
    QVector<ClipInfo> allClips;
    {
        QMutexLocker lock(&m_clipsMutex);
        sequenceClips.reserve(m_clips.size());
        allClips.reserve(m_clips.size());
        for (auto it = m_clips.cbegin(); it != m_clips.cend(); ++it) {
            sequenceClips.push_back(editor::SequencePrefetchClip{it.value().clip, it.value().decodePath});
            allClips.push_back(it.value());
        }
    }

    const QVector<int64_t> futureTimelineFrames =
        editor::collectLookaheadTimelineFrames(playhead, lookahead, step, exportRanges);

    for (int i = 0; i < futureTimelineFrames.size() && scheduledThisTick < maxPrefetchPerTick; ++i) {
        const int64_t currentTimelineFrame = futureTimelineFrames[i];

        QVector<ClipInfo> activeClips;
        activeClips.reserve(allClips.size());
        for (const ClipInfo& info : allClips) {
            if (currentTimelineFrame < info.clip.startFrame ||
                currentTimelineFrame >= info.clip.startFrame + info.clip.durationFrames) {
                continue;
            }
            activeClips.push_back(info);
        }

        std::sort(activeClips.begin(), activeClips.end(),
                  [currentTimelineFrame](const ClipInfo& a, const ClipInfo& b) {
                      const bool aSequence = a.clip.sourceKind == MediaSourceKind::ImageSequence;
                      const bool bSequence = b.clip.sourceKind == MediaSourceKind::ImageSequence;
                      if (aSequence != bSequence) {
                          return aSequence;
                      }
                      const int64_t aDistance = qAbs(currentTimelineFrame - a.clip.startFrame);
                      const int64_t bDistance = qAbs(currentTimelineFrame - b.clip.startFrame);
                      if (aDistance == bDistance) {
                          return a.clip.id < b.clip.id;
                      }
                      return aDistance < bDistance;
                  });

        const QVector<editor::SequencePrefetchRequest> requests =
            editor::collectSequencePrefetchRequestsAtTimelineFrame(sequenceClips,
                                                                   static_cast<qreal>(currentTimelineFrame),
                                                                   m_renderSyncMarkers,
                                                                   false,
                                                                   qMax(20, 44 - ((i + 1) * 2)));

        QSet<QString> scheduledSequenceClipIds;

        for (const editor::SequencePrefetchRequest& prefetch : requests) {
            if (scheduledThisTick >= maxPrefetchPerTick) {
                break;
            }

            const QString& id = prefetch.clipId;
            const int64_t targetFrame = prefetch.sourceFrame;

            if (m_decoder->pendingRequestCount() >= maxPrefetchQueueDepth ||
                m_inflightPrefetches.load() >= maxInflightPrefetch) {
                return;
            }

            {
                QMutexLocker lock(&m_clipsMutex);
                ClipCache* cache = m_caches.value(id);
                if (cache && cache->contains(targetFrame)) {
                    continue;
                }
            }

            const QString key = requestKey(id, targetFrame);
            {
                QMutexLocker pendingLock(&m_pendingMutex);
                if (m_pendingVisibleRequests.contains(key) || m_pendingPrefetchRequests.contains(key)) {
                    continue;
                }
                m_pendingPrefetchRequests.insert(key);
                m_inflightPrefetches.fetch_add(1);
            }

            if (prefetch.decodePath.isEmpty()) {
                QMutexLocker pendingLock(&m_pendingMutex);
                if (m_pendingPrefetchRequests.remove(key) > 0) {
                    m_inflightPrefetches.fetch_sub(1);
                }
                continue;
            }

            scheduledSequenceClipIds.insert(id);
            m_prefetches++;
            scheduledThisTick++;

            QPointer<TimelineCache> self(this);
            const std::shared_ptr<std::atomic<bool>> aliveToken = m_aliveToken;
            cacheTrace(QStringLiteral("TimelineCache::prefetch.dispatch"),
                       QStringLiteral("clip=%1 frame=%2 priority=%3")
                           .arg(id)
                           .arg(targetFrame)
                           .arg(prefetch.priority));

            const uint64_t seqId = m_decoder->requestFrame(
                prefetch.decodePath,
                targetFrame,
                prefetch.priority,
                5000,
                DecodeRequestKind::Prefetch,
                [self, aliveToken, id, targetFrame, key](FrameHandle frame) {
                    if (!aliveToken->load() || !self) {
                        return;
                    }

                    QMetaObject::invokeMethod(
                        self,
                        [self, aliveToken, id, targetFrame, key, frame]() {
                            if (!aliveToken->load() || !self) {
                                return;
                            }

                            {
                                QMutexLocker pendingLock(&self->m_pendingMutex);
                                self->m_pendingPrefetchRequests.remove(key);
                                self->m_inflightPrefetches.fetch_sub(1);
                            }

                            if (!frame.isNull()) {
                                if (ClipCache* cache = self->getOrCreateClipCache(id)) {
                                    cache->insert(targetFrame, frame);
                                }
                            }

                            cacheTrace(QStringLiteral("TimelineCache::prefetch.complete"),
                                       QStringLiteral("clip=%1 frame=%2 null=%3")
                                           .arg(id)
                                           .arg(targetFrame)
                                           .arg(frame.isNull()));
                        },
                        Qt::QueuedConnection);
                });

            if (seqId == 0) {
                QMutexLocker pendingLock(&m_pendingMutex);
                if (m_pendingPrefetchRequests.remove(key) > 0) {
                    m_inflightPrefetches.fetch_sub(1);
                }
            }
        }

        for (const ClipInfo& info : activeClips) {
            if (scheduledThisTick >= maxPrefetchPerTick) {
                break;
            }
            if (info.clip.sourceKind == MediaSourceKind::ImageSequence ||
                scheduledSequenceClipIds.contains(info.clip.id)) {
                continue;
            }

            const QString& id = info.clip.id;
            const int64_t targetFrame =
                sourceFrameForClipAtTimelinePosition(info.clip,
                                                     static_cast<qreal>(currentTimelineFrame),
                                                     m_renderSyncMarkers);

            if (m_decoder->pendingRequestCount() >= maxPrefetchQueueDepth ||
                m_inflightPrefetches.load() >= maxInflightPrefetch) {
                return;
            }

            {
                QMutexLocker lock(&m_clipsMutex);
                ClipCache* cache = m_caches.value(id);
                if (cache && cache->contains(targetFrame)) {
                    continue;
                }
            }

            const QString key = requestKey(id, targetFrame);
            {
                QMutexLocker pendingLock(&m_pendingMutex);
                if (m_pendingVisibleRequests.contains(key) || m_pendingPrefetchRequests.contains(key)) {
                    continue;
                }
                m_pendingPrefetchRequests.insert(key);
                m_inflightPrefetches.fetch_add(1);
            }

            if (info.decodePath.isEmpty()) {
                QMutexLocker pendingLock(&m_pendingMutex);
                if (m_pendingPrefetchRequests.remove(key) > 0) {
                    m_inflightPrefetches.fetch_sub(1);
                }
                continue;
            }

            const int priority = qMax(12, 30 - (i + 1));
            m_prefetches++;
            scheduledThisTick++;

            QPointer<TimelineCache> self(this);
            const std::shared_ptr<std::atomic<bool>> aliveToken = m_aliveToken;
            cacheTrace(QStringLiteral("TimelineCache::prefetch.dispatch"),
                       QStringLiteral("clip=%1 frame=%2 priority=%3")
                           .arg(id)
                           .arg(targetFrame)
                           .arg(priority));

            const uint64_t seqId = m_decoder->requestFrame(
                info.decodePath,
                targetFrame,
                priority,
                5000,
                DecodeRequestKind::Prefetch,
                [self, aliveToken, id, targetFrame, key](FrameHandle frame) {
                    if (!aliveToken->load() || !self) {
                        return;
                    }

                    QMetaObject::invokeMethod(
                        self,
                        [self, aliveToken, id, targetFrame, key, frame]() {
                            if (!aliveToken->load() || !self) {
                                return;
                            }

                            {
                                QMutexLocker pendingLock(&self->m_pendingMutex);
                                self->m_pendingPrefetchRequests.remove(key);
                                self->m_inflightPrefetches.fetch_sub(1);
                            }

                            if (!frame.isNull()) {
                                if (ClipCache* cache = self->getOrCreateClipCache(id)) {
                                    cache->insert(targetFrame, frame);
                                }
                            }

                            cacheTrace(QStringLiteral("TimelineCache::prefetch.complete"),
                                       QStringLiteral("clip=%1 frame=%2 null=%3")
                                           .arg(id)
                                           .arg(targetFrame)
                                           .arg(frame.isNull()));
                        },
                        Qt::QueuedConnection);
                });

            if (seqId == 0) {
                QMutexLocker pendingLock(&m_pendingMutex);
                if (m_pendingPrefetchRequests.remove(key) > 0) {
                    m_inflightPrefetches.fetch_sub(1);
                }
            }
        }
    }
}

int TimelineCache::calculatePriority(int64_t frameNumber) const {
    const int64_t playhead = m_playhead.load();
    const int64_t delta = qAbs(frameNumber - playhead);

    if (delta == 0) {
        return 100;
    }
    if (delta <= 5) {
        return 80;
    }
    if (delta <= 15) {
        return 60;
    }
    if (delta <= 30) {
        return 40;
    }
    return 20;
}

QString TimelineCache::requestKey(const QString& clipId, int64_t frameNumber) const {
    return clipId + QLatin1Char(':') + QString::number(frameNumber);
}

int64_t TimelineCache::normalizeFrameNumber(const QString& clipId, int64_t frameNumber) const {
    QMutexLocker lock(&m_clipsMutex);
    const auto it = m_clips.find(clipId);
    if (it == m_clips.end()) {
        return frameNumber;
    }
    return normalizeFrameNumber(it.value(), frameNumber);
}

int64_t TimelineCache::normalizeFrameNumber(const ClipInfo& info, int64_t frameNumber) const {
    if (info.isSingleFrame) {
        return 0;
    }
    return frameNumber;
}

ClipCache* TimelineCache::getOrCreateClipCache(const QString& clipId) {
    QMutexLocker lock(&m_clipsMutex);

    auto it = m_caches.find(clipId);
    if (it != m_caches.end()) {
        return it.value();
    }

    auto clipIt = m_clips.find(clipId);
    if (clipIt == m_clips.end()) {
        return nullptr;
    }

    ClipCache* cache = new ClipCache(
        clipIt->decodePath.isEmpty() ? clipIt->clip.filePath : clipIt->decodePath,
        clipIt->clip.durationFrames,
        m_budget);

    m_caches[clipId] = cache;
    if (!m_playbackBuffers.contains(clipId)) {
        m_playbackBuffers[clipId] = new PlaybackBuffer();
    }

    return cache;
}

void TimelineCache::evictOldestFrames(size_t targetMemory) {
    struct FrameEntry {
        QString clipId;
        int64_t frameNumber = 0;
        qint64 accessTime = 0;
        size_t memory = 0;
    };

    QList<FrameEntry> entries;
    size_t current = 0;

    {
        QMutexLocker lock(&m_clipsMutex);

        for (auto it = m_caches.begin(); it != m_caches.end(); ++it) {
            const QString& clipId = it.key();
            ClipCache* cache = it.value();

            for (const CacheEntryInfo& info : cache->entries()) {
                entries.append({clipId, info.frameNumber, info.lastAccessTime, info.memoryUsage});
                current += info.memoryUsage;
            }
        }
    }

    std::sort(entries.begin(), entries.end(),
              [](const FrameEntry& a, const FrameEntry& b) {
                  return a.accessTime < b.accessTime;
              });

    for (const FrameEntry& entry : entries) {
        if (current <= targetMemory) {
            break;
        }

        ClipCache* cache = nullptr;
        {
            QMutexLocker lock(&m_clipsMutex);
            auto it = m_caches.find(entry.clipId);
            if (it != m_caches.end()) {
                cache = it.value();
            }
        }

        if (!cache) {
            continue;
        }

        cache->remove(entry.frameNumber);
        current = (current > entry.memory) ? (current - entry.memory) : 0;
        emit frameEvicted(entry.clipId, entry.frameNumber);
    }
}

} // namespace editor
