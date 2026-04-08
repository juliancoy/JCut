#pragma once

#include "editor_shared.h"
#include <QPoint>
#include <QRect>

class TimelineWidget;

class TimelineLayout {
public:
    explicit TimelineLayout(TimelineWidget* widget);

    QRect drawRect() const;
    QRect topBarRect() const;
    QRect rulerRect() const;
    QRect trackRect() const;
    QRect trackSidebarRect() const;
    QRect timelineContentRect() const;
    QRect exportRangeRect() const;
    QRect exportSegmentRect(const ExportRangeSegment& segment) const;
    QRect exportHandleRect(int segmentIndex, bool startHandle) const;

    QRect trackLabelRect(int trackIndex) const;
    QRect trackNameRect(int trackIndex) const;
    QRect trackVisualToggleRect(int trackIndex) const;
    QRect trackAudioToggleRect(int trackIndex) const;
    QRect clipRectFor(const TimelineClip& clip) const;

    int trackTop(int trackIndex) const;
    int trackHeight(int trackIndex) const;
    int totalTrackAreaHeight() const;
    int maxVerticalScrollOffset() const;
    int trackIndexAtY(int y, bool allowAppendTrack = false) const;
    int trackDropTargetAtY(int y, bool* insertsTrack) const;
    int trackDividerAt(const QPoint& pos) const;

private:
    TimelineWidget* m_widget = nullptr;
};
