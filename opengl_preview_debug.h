#pragma once

#include <QString>

namespace editor {
class FrameHandle;
}
using editor::FrameHandle;

struct TimelineClip;

qint64 nowMs();
qint64 playbackTraceMs();
void playbackTrace(const QString& event, const QString& detail);
void playbackWarnTrace(const QString& event, const QString& detail);
void playbackFrameSelectionTrace(const QString& stage,
                                 const TimelineClip& clip,
                                 int64_t requestedFrame,
                                 const FrameHandle& exactFrame,
                                 const FrameHandle& selectedFrame,
                                 qreal playheadFramePosition);
