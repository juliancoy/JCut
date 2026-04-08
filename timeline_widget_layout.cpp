#include "timeline_layout.h"
#include "timeline_widget.h"

QRect TimelineWidget::drawRect() const {
    return m_layout->drawRect();
}

QRect TimelineWidget::topBarRect() const {
    return m_layout->topBarRect();
}

QRect TimelineWidget::rulerRect() const {
    return m_layout->rulerRect();
}

QRect TimelineWidget::trackRect() const {
    return m_layout->trackRect();
}

QRect TimelineWidget::trackSidebarRect() const {
    return m_layout->trackSidebarRect();
}

QRect TimelineWidget::timelineContentRect() const {
    return m_layout->timelineContentRect();
}

QRect TimelineWidget::exportRangeRect() const {
    return m_layout->exportRangeRect();
}

QRect TimelineWidget::exportSegmentRect(const ExportRangeSegment& segment) const {
    return m_layout->exportSegmentRect(segment);
}

QRect TimelineWidget::exportHandleRect(int segmentIndex, bool startHandle) const {
    return m_layout->exportHandleRect(segmentIndex, startHandle);
}

QRect TimelineWidget::trackLabelRect(int trackIndex) const {
    return m_layout->trackLabelRect(trackIndex);
}

QRect TimelineWidget::trackNameRect(int trackIndex) const {
    return m_layout->trackNameRect(trackIndex);
}

QRect TimelineWidget::trackVisualToggleRect(int trackIndex) const {
    return m_layout->trackVisualToggleRect(trackIndex);
}

QRect TimelineWidget::trackAudioToggleRect(int trackIndex) const {
    return m_layout->trackAudioToggleRect(trackIndex);
}

QRect TimelineWidget::clipRectFor(const TimelineClip& clip) const {
    return m_layout->clipRectFor(clip);
}

QRect TimelineWidget::renderSyncMarkerRect(const TimelineClip& clip, const RenderSyncMarker& marker) const {
    const QRect clipRect = clipRectFor(clip);
    const int left = xFromFrame(marker.frame);
    const int right = xFromFrame(marker.frame + 1);
    const int width = qMax(6, right - left);
    return QRect(left,
                 clipRect.top() + 2,
                 qMin(width, qMax(6, clipRect.right() - left)),
                 clipRect.height() - 4);
}
