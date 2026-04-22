#include "timeline_cache_seek_resync.h"

namespace editor {

void TimelineCacheSeekResyncTracker::reset() {
    m_targetFrames.clear();
    m_untilMs.clear();
}

void TimelineCacheSeekResyncTracker::begin(const QString& clipId,
                                           int64_t targetFrame,
                                           qint64 nowMs,
                                           qint64 windowMs) {
    m_targetFrames.insert(clipId, targetFrame);
    m_untilMs.insert(clipId, nowMs + windowMs);
}

bool TimelineCacheSeekResyncTracker::shouldAllowApproximate(const QString& clipId,
                                                            int64_t frameNumber,
                                                            qint64 nowMs) const {
    const auto untilIt = m_untilMs.constFind(clipId);
    if (untilIt == m_untilMs.constEnd() || nowMs > untilIt.value()) {
        return true;
    }
    const auto targetIt = m_targetFrames.constFind(clipId);
    if (targetIt == m_targetFrames.constEnd()) {
        return true;
    }
    return targetIt.value() != frameNumber;
}

void TimelineCacheSeekResyncTracker::satisfy(const QString& clipId,
                                             int64_t deliveredFrameNumber,
                                             qint64 nowMs) {
    const auto untilIt = m_untilMs.find(clipId);
    if (untilIt == m_untilMs.end()) {
        return;
    }
    if (nowMs > untilIt.value()) {
        m_untilMs.erase(untilIt);
        m_targetFrames.remove(clipId);
        return;
    }
    const auto targetIt = m_targetFrames.find(clipId);
    if (targetIt == m_targetFrames.end()) {
        m_untilMs.erase(untilIt);
        return;
    }
    if (deliveredFrameNumber >= targetIt.value() - 1) {
        m_targetFrames.erase(targetIt);
        m_untilMs.remove(clipId);
    }
}

bool TimelineCacheSeekResyncTracker::expireAndCheckAnyActive(qint64 nowMs) {
    for (auto it = m_untilMs.begin(); it != m_untilMs.end();) {
        if (nowMs > it.value()) {
            m_targetFrames.remove(it.key());
            it = m_untilMs.erase(it);
        } else {
            ++it;
        }
    }
    return !m_untilMs.isEmpty();
}

}  // namespace editor
