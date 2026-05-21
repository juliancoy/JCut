#pragma once

#include "frame_handle.h"

#include <QHash>

#include <limits>

namespace editor {

template <typename FrameInfo>
inline FrameHandle closestBufferedFrame(const QHash<int64_t, FrameInfo>& frames, int64_t frameNumber) {
    auto exact = frames.find(frameNumber);
    if (exact != frames.end()) {
        return exact.value().frame;
    }

    qint64 bestDistance = std::numeric_limits<qint64>::max();
    qint64 bestInsertedAt = std::numeric_limits<qint64>::min();
    auto best = frames.end();
    for (auto it = frames.begin(); it != frames.end(); ++it) {
        const qint64 distance = qAbs(it.key() - frameNumber);
        if (distance < bestDistance ||
            (distance == bestDistance && it.value().insertedAt > bestInsertedAt)) {
            bestDistance = distance;
            bestInsertedAt = it.value().insertedAt;
            best = it;
        }
    }

    return best == frames.end() ? FrameHandle() : best.value().frame;
}

template <typename FrameInfo>
inline void trimOldestBufferedFrames(QHash<int64_t, FrameInfo>* frames, int maxFrames) {
    if (!frames) {
        return;
    }
    while (frames->size() > maxFrames) {
        auto oldest = frames->end();
        qint64 oldestInsertedAt = std::numeric_limits<qint64>::max();
        for (auto it = frames->begin(); it != frames->end(); ++it) {
            if (it.value().insertedAt < oldestInsertedAt) {
                oldestInsertedAt = it.value().insertedAt;
                oldest = it;
            }
        }
        if (oldest == frames->end()) {
            break;
        }
        frames->erase(oldest);
    }
}

} // namespace editor
