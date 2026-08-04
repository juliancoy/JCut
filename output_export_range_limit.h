#pragma once

#include "editor_shared.h"
#include "timeline_fps.h"

#include <QVector>

#include <cmath>

inline QVector<ExportRangeSegment> limitExportRangesToOutputSeconds(
    const QVector<ExportRangeSegment>& ranges,
    double playbackSpeed,
    int outputSeconds)
{
    if (ranges.isEmpty() || outputSeconds <= 0) {
        return ranges;
    }

    const double speed = std::isfinite(playbackSpeed) && playbackSpeed > 0.001
        ? playbackSpeed
        : 1.0;
    int64_t remainingFrames = qMax<int64_t>(
        1,
        static_cast<int64_t>(std::floor(
            static_cast<double>(outputSeconds * kTimelineFps) * speed)));

    QVector<ExportRangeSegment> limited;
    limited.reserve(ranges.size());
    for (const ExportRangeSegment& range : ranges) {
        const int64_t startFrame = qMin(range.startFrame, range.endFrame);
        const int64_t endFrame = qMax(range.startFrame, range.endFrame);
        const int64_t frameCount = endFrame - startFrame + 1;
        if (frameCount <= remainingFrames) {
            limited.push_back(ExportRangeSegment{startFrame, endFrame});
            remainingFrames -= frameCount;
        } else {
            limited.push_back(ExportRangeSegment{
                startFrame,
                startFrame + remainingFrames - 1});
            remainingFrames = 0;
        }
        if (remainingFrames == 0) {
            break;
        }
    }
    return limited;
}
