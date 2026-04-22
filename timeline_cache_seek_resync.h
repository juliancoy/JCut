#pragma once

#include <QHash>
#include <QString>

namespace editor {

class TimelineCacheSeekResyncTracker {
public:
    void reset();
    void begin(const QString& clipId, int64_t targetFrame, qint64 nowMs, qint64 windowMs);
    bool shouldAllowApproximate(const QString& clipId, int64_t frameNumber, qint64 nowMs) const;
    void satisfy(const QString& clipId, int64_t deliveredFrameNumber, qint64 nowMs);
    bool expireAndCheckAnyActive(qint64 nowMs);

private:
    QHash<QString, int64_t> m_targetFrames;
    QHash<QString, qint64> m_untilMs;
};

}  // namespace editor
