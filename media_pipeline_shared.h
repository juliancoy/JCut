#pragma once

#include "editor_shared.h"

#include <QVector>

namespace editor {

struct DecodePrefetchClip {
    TimelineClip clip;
    QString decodePath;
};

struct DecodePrefetchRequest {
    QString clipId;
    QString decodePath;
    int64_t timelineFrame = -1;
    int64_t sourceFrame = -1;
    int priority = 0;
};

bool clipIsActiveAtTimelineFrame(const TimelineClip& clip,
                                 const QVector<TimelineTrack>& tracks,
                                 qreal timelineFrame,
                                 bool bypassGrading);

QVector<int64_t> collectLookaheadTimelineFrames(int64_t startTimelineFrame,
                                                int lookaheadFrames,
                                                int step,
                                                const QVector<ExportRangeSegment>& exportRanges);

QVector<int64_t> collectSequenceLookaheadSourceFrames(const TimelineClip& clip,
                                                      qreal startTimelineFrame,
                                                      int lookaheadFrames,
                                                      const QVector<RenderSyncMarker>& renderSyncMarkers,
                                                      bool bypassGrading);

QVector<DecodePrefetchRequest> collectDecodePrefetchRequestsAtTimelineFrame(
    const QVector<DecodePrefetchClip>& clips,
    qreal timelineFrame,
    const QVector<RenderSyncMarker>& renderSyncMarkers,
    bool bypassGrading,
    int priority,
    bool imageSequencesOnly);

} // namespace editor
