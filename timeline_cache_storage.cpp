#include "timeline_cache.h"
#include "frame_buffer_utils.h"

#include <QDateTime>
#include <QMutexLocker>

#include <algorithm>
#include <limits>

namespace editor {

void TimelineCache::PlaybackBuffer::clear() {
    QMutexLocker lock(&m_mutex);
    m_frames.clear();
}

void TimelineCache::PlaybackBuffer::insert(int64_t frameNumber, const FrameHandle& frame) {
    if (frame.isNull() || frame.hasHardwareFrame()) {
        return;
    }

    QMutexLocker lock(&m_mutex);
    PlaybackFrameInfo info;
    info.frame = frame;
    info.insertedAt = QDateTime::currentMSecsSinceEpoch();
    m_frames.insert(frameNumber, info);
    trimLocked();
}

FrameHandle TimelineCache::PlaybackBuffer::get(int64_t frameNumber) {
    QMutexLocker lock(&m_mutex);
    auto it = m_frames.find(frameNumber);
    if (it == m_frames.end()) {
        return FrameHandle();
    }
    return it.value().frame;
}

FrameHandle TimelineCache::PlaybackBuffer::getBest(int64_t frameNumber) {
    QMutexLocker lock(&m_mutex);
    return closestBufferedFrame(m_frames, frameNumber);
}

FrameHandle TimelineCache::PlaybackBuffer::getLatestAtOrBefore(int64_t frameNumber) {
    QMutexLocker lock(&m_mutex);

    auto best = m_frames.end();
    for (auto it = m_frames.begin(); it != m_frames.end(); ++it) {
        if (it.key() <= frameNumber && (best == m_frames.end() || it.key() > best.key())) {
            best = it;
        }
    }
    if (best != m_frames.end()) {
        return best.value().frame;
    }

    return FrameHandle();
}

bool TimelineCache::PlaybackBuffer::contains(int64_t frameNumber) const {
    QMutexLocker lock(&m_mutex);
    return m_frames.contains(frameNumber);
}

void TimelineCache::PlaybackBuffer::trimLocked() {
    trimOldestBufferedFrames(&m_frames, kMaxFrames);
}

// ============================================================================
// ClipCache Implementation
// ============================================================================

ClipCache::ClipCache(const QString& path, int64_t duration, MemoryBudget* budget)
    : m_path(path), m_duration(duration), m_budget(budget) {}

ClipCache::~ClipCache() {
    if (m_budget && m_memoryUsage > 0) {
        size_t cpuBytes = 0;
        size_t gpuBytes = 0;
        {
            QMutexLocker lock(&m_mutex);
            for (auto it = m_frames.cbegin(); it != m_frames.cend(); ++it) {
                cpuBytes += it.value().frame.cpuMemoryUsage();
                gpuBytes += it.value().frame.gpuMemoryUsage();
            }
        }
        if (cpuBytes > 0) {
            m_budget->deallocateCpu(cpuBytes);
        }
        if (gpuBytes > 0) {
            m_budget->deallocateGpu(gpuBytes);
        }
    }
}

ClipCache::FrameMemoryUse ClipCache::frameMemoryUse(const FrameHandle& frame) const {
    return FrameMemoryUse{
        frame.cpuMemoryUsage(),
        frame.gpuMemoryUsage()
    };
}

void ClipCache::insert(int64_t frameNumber, const FrameHandle& frame) {
    if (frame.isNull()) {
        return;
    }

    FrameMemoryUse replacedUse;
    FrameMemoryUse insertedUse = frameMemoryUse(frame);
    bool hasExisting = false;

    {
        QMutexLocker lock(&m_mutex);
        auto it = m_frames.find(frameNumber);
        if (it != m_frames.end()) {
            replacedUse = frameMemoryUse(it.value().frame);
            hasExisting = true;
        }
    }

    const size_t cpuDeltaUp = insertedUse.cpuBytes > replacedUse.cpuBytes
        ? insertedUse.cpuBytes - replacedUse.cpuBytes : 0;
    const size_t gpuDeltaUp = insertedUse.gpuBytes > replacedUse.gpuBytes
        ? insertedUse.gpuBytes - replacedUse.gpuBytes : 0;

    if (m_budget) {
        if (cpuDeltaUp > 0 &&
            !m_budget->allocateCpu(cpuDeltaUp, MemoryBudget::Priority::Normal)) {
            return;
        }
        if (gpuDeltaUp > 0 &&
            !m_budget->allocateGpu(gpuDeltaUp, MemoryBudget::Priority::Normal)) {
            if (cpuDeltaUp > 0) {
                m_budget->deallocateCpu(cpuDeltaUp);
            }
            return;
        }
    }

    CachedFrame cf;
    cf.frame = frame;
    cf.lastAccessTime = QDateTime::currentMSecsSinceEpoch();
    cf.accessCount = 1;

    {
        QMutexLocker lock(&m_mutex);
        if (hasExisting) {
            m_memoryUsage -= (replacedUse.cpuBytes + replacedUse.gpuBytes);
        }
        m_frames[frameNumber] = cf;
        m_memoryUsage += (insertedUse.cpuBytes + insertedUse.gpuBytes);
    }

    if (m_budget) {
        if (insertedUse.cpuBytes < replacedUse.cpuBytes) {
            m_budget->deallocateCpu(replacedUse.cpuBytes - insertedUse.cpuBytes);
        }
        if (insertedUse.gpuBytes < replacedUse.gpuBytes) {
            m_budget->deallocateGpu(replacedUse.gpuBytes - insertedUse.gpuBytes);
        }
    }
}

FrameHandle ClipCache::get(int64_t frameNumber) {
    QMutexLocker lock(&m_mutex);

    auto it = m_frames.find(frameNumber);
    if (it == m_frames.end()) {
        return FrameHandle();
    }

    it.value().lastAccessTime = QDateTime::currentMSecsSinceEpoch();
    it.value().accessCount++;
    return it.value().frame;
}

FrameHandle ClipCache::getBest(int64_t frameNumber) {
    QMutexLocker lock(&m_mutex);

    auto exact = m_frames.find(frameNumber);
    if (exact != m_frames.end()) {
        exact.value().lastAccessTime = QDateTime::currentMSecsSinceEpoch();
        exact.value().accessCount++;
        return exact.value().frame;
    }

    qint64 bestDistance = std::numeric_limits<qint64>::max();
    auto best = m_frames.end();
    for (auto it = m_frames.begin(); it != m_frames.end(); ++it) {
        if (best == m_frames.end()) {
            bestDistance = qAbs(it.key() - frameNumber);
            best = it;
            continue;
        }

        const qint64 distance = qAbs(it.key() - frameNumber);
        if (distance < bestDistance || (distance == bestDistance && it.key() < best.key())) {
            bestDistance = distance;
            best = it;
        }
    }

    if (best == m_frames.end()) {
        return FrameHandle();
    }

    best.value().lastAccessTime = QDateTime::currentMSecsSinceEpoch();
    best.value().accessCount++;
    return best.value().frame;
}

FrameHandle ClipCache::getLatestAtOrBefore(int64_t frameNumber) {
    QMutexLocker lock(&m_mutex);

    auto best = m_frames.end();
    for (auto it = m_frames.begin(); it != m_frames.end(); ++it) {
        if (it.key() <= frameNumber && (best == m_frames.end() || it.key() > best.key())) {
            best = it;
        }
    }
    if (best != m_frames.end()) {
        best.value().lastAccessTime = QDateTime::currentMSecsSinceEpoch();
        best.value().accessCount++;
        return best.value().frame;
    }

    return FrameHandle();
}

bool ClipCache::contains(int64_t frameNumber) const {
    QMutexLocker lock(&m_mutex);
    return m_frames.contains(frameNumber);
}

void ClipCache::remove(int64_t frameNumber) {
    FrameMemoryUse releasedUse;

    QMutexLocker lock(&m_mutex);
    auto it = m_frames.find(frameNumber);
    if (it != m_frames.end()) {
        releasedUse = frameMemoryUse(it.value().frame);
        m_memoryUsage -= (releasedUse.cpuBytes + releasedUse.gpuBytes);
        m_frames.erase(it);
    }
    lock.unlock();

    if (m_budget && releasedUse.cpuBytes > 0) {
        m_budget->deallocateCpu(releasedUse.cpuBytes);
    }
    if (m_budget && releasedUse.gpuBytes > 0) {
        m_budget->deallocateGpu(releasedUse.gpuBytes);
    }
}

void ClipCache::evictToFit(size_t maxMemory) {
    struct EvictCandidate {
        int64_t key = 0;
        qint64 lastAccessTime = 0;
    };

    QVector<EvictCandidate> candidates;
    QVector<FrameMemoryUse> releasedUses;

    {
        QMutexLocker lock(&m_mutex);

        if (m_memoryUsage <= maxMemory) {
            return;
        }

        candidates.reserve(m_frames.size());
        for (auto it = m_frames.cbegin(); it != m_frames.cend(); ++it) {
            candidates.push_back(EvictCandidate{it.key(), it.value().lastAccessTime});
        }

        std::sort(candidates.begin(), candidates.end(),
                  [](const EvictCandidate& a, const EvictCandidate& b) {
                      return a.lastAccessTime < b.lastAccessTime;
                  });

        for (const EvictCandidate& candidate : candidates) {
            if (m_memoryUsage <= maxMemory) {
                break;
            }

            auto it = m_frames.find(candidate.key);
            if (it == m_frames.end()) {
                continue;
            }

            const FrameMemoryUse use = frameMemoryUse(it.value().frame);
            releasedUses.push_back(use);
            m_memoryUsage -= (use.cpuBytes + use.gpuBytes);
            m_frames.erase(it);
        }
    }

    if (m_budget) {
        for (const FrameMemoryUse& use : releasedUses) {
            if (use.cpuBytes > 0) {
                m_budget->deallocateCpu(use.cpuBytes);
            }
            if (use.gpuBytes > 0) {
                m_budget->deallocateGpu(use.gpuBytes);
            }
        }
    }
}

QList<int64_t> ClipCache::cachedFrames() const {
    QMutexLocker lock(&m_mutex);
    return m_frames.keys();
}

QList<CacheEntryInfo> ClipCache::entries() const {
    QMutexLocker lock(&m_mutex);

    QList<CacheEntryInfo> result;
    result.reserve(m_frames.size());
    for (auto it = m_frames.cbegin(); it != m_frames.cend(); ++it) {
        const FrameHandle& frame = it.value().frame;
        result.append({it.key(),
                       it.value().lastAccessTime,
                       frame.cpuMemoryUsage() + frame.gpuMemoryUsage()});
    }
    return result;
}

ClipCache::ResidencyStats ClipCache::residencyStats() const {
    ResidencyStats stats;
    QMutexLocker lock(&m_mutex);
    for (auto it = m_frames.cbegin(); it != m_frames.cend(); ++it) {
        const FrameHandle& frame = it.value().frame;
        if (frame.isNull()) {
            continue;
        }
        ++stats.totalFrames;
        stats.hardwareFrames += frame.hasHardwareFrame() ? 1 : 0;
        stats.cpuBackedFrames += frame.hasCpuImage() ? 1 : 0;
        stats.gpuTextureFrames += frame.hasGpuTexture() ? 1 : 0;
        stats.cpuBytes += frame.cpuMemoryUsage();
        stats.gpuBytes += frame.gpuMemoryUsage();
    }
    return stats;
}

} // namespace editor
