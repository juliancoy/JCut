#include "timeline_layout.h"
#include "timeline_widget.h"

#include <algorithm>

TimelineLayout::TimelineLayout(TimelineWidget* widget)
    : m_widget(widget) {
}

QRect TimelineLayout::drawRect() const {
    return m_widget->rect().adjusted(TimelineWidget::kTimelineOuterMargin,
                                     TimelineWidget::kTimelineOuterMargin,
                                     -TimelineWidget::kTimelineOuterMargin,
                                     -TimelineWidget::kTimelineOuterMargin);
}

QRect TimelineLayout::topBarRect() const {
    const QRect draw = drawRect();
    return QRect(draw.left(), draw.top(), draw.width(), TimelineWidget::kTimelineTopBarHeight);
}

QRect TimelineLayout::rulerRect() const {
    const QRect draw = drawRect();
    const QRect topBar = topBarRect();
    return QRect(draw.left(), topBar.bottom() + 8, draw.width(), TimelineWidget::kTimelineRulerHeight);
}

QRect TimelineLayout::trackRect() const {
    const QRect draw = drawRect();
    const QRect ruler = rulerRect();
    return QRect(draw.left(), ruler.bottom() + TimelineWidget::kTimelineTrackGap, draw.width(),
                 draw.height() - ((ruler.bottom() - draw.top() + 1) + TimelineWidget::kTimelineTrackGap));
}

QRect TimelineLayout::trackSidebarRect() const {
    return QRect();
}

QRect TimelineLayout::timelineContentRect() const {
    return trackRect();
}

QRect TimelineLayout::exportRangeRect() const {
    const QRect topBar = topBarRect();
    const QRect content = timelineContentRect();
    return QRect(content.left(), topBar.top() + 26, content.width(), 20);
}

QRect TimelineLayout::exportSegmentRect(const ExportRangeSegment& segment) const {
    const QRect bar = exportRangeRect();
    const int left = m_widget->xFromFrame(segment.startFrame);
    const int right = m_widget->xFromFrame(segment.endFrame);
    return QRect(qMin(left, right),
                 bar.top(),
                 qMax(6, qAbs(right - left)),
                 bar.height());
}

QRect TimelineLayout::exportHandleRect(int segmentIndex, bool startHandle) const {
    if (segmentIndex < 0 || segmentIndex >= m_widget->m_exportRanges.size()) {
        return QRect();
    }
    const QRect bar = exportRangeRect();
    const int64_t frame = startHandle ? m_widget->m_exportRanges[segmentIndex].startFrame
                                      : m_widget->m_exportRanges[segmentIndex].endFrame;
    const int x = m_widget->xFromFrame(frame);
    return QRect(x - 5, bar.top() - 1, 10, bar.height() + 2);
}

int TimelineLayout::trackTop(int trackIndex) const {
    const QRect tracks = trackRect();
    int y = tracks.top() + TimelineWidget::kTimelineTrackInnerPadding - m_widget->m_verticalScrollOffset;
    for (int i = 0; i < trackIndex && i < m_widget->m_tracks.size(); ++i) {
        y += trackHeight(i) + TimelineWidget::kTimelineTrackSpacing;
    }
    return y;
}

int TimelineLayout::trackHeight(int trackIndex) const {
    if (trackIndex >= 0 && trackIndex < m_widget->m_tracks.size()) {
        return qMax(TimelineWidget::kMinTrackHeight, m_widget->m_tracks[trackIndex].height);
    }
    return TimelineWidget::kDefaultTrackHeight;
}

int TimelineLayout::totalTrackAreaHeight() const {
    int trackAreaHeight = TimelineWidget::kTimelineTrackInnerPadding * 2;
    for (int i = 0; i < m_widget->trackCount(); ++i) {
        trackAreaHeight += trackHeight(i);
    }
    trackAreaHeight += qMax(0, m_widget->trackCount() - 1) * TimelineWidget::kTimelineTrackSpacing;
    return trackAreaHeight;
}

int TimelineLayout::maxVerticalScrollOffset() const {
    return qMax(0, totalTrackAreaHeight() - trackRect().height());
}

int TimelineLayout::trackIndexAtY(int y, bool allowAppendTrack) const {
    if (m_widget->m_tracks.isEmpty()) {
        return 0;
    }

    for (int i = 0; i < m_widget->trackCount(); ++i) {
        const int top = trackTop(i);
        const int bottom = top + trackHeight(i);
        if (y >= top && y < bottom) {
            return i;
        }
    }

    const int lastTrackBottom = trackTop(m_widget->trackCount() - 1) + trackHeight(m_widget->trackCount() - 1);
    if (allowAppendTrack && y >= lastTrackBottom) {
        return m_widget->trackCount();
    }
    if (y < trackTop(0)) {
        return 0;
    }
    return m_widget->trackCount() - 1;
}

int TimelineLayout::trackDropTargetAtY(int y, bool* insertsTrack) const {
    if (insertsTrack) {
        *insertsTrack = false;
    }
    if (m_widget->m_tracks.isEmpty()) {
        if (insertsTrack) {
            *insertsTrack = true;
        }
        return 0;
    }

    const int firstTop = trackTop(0);
    if (y < firstTop) {
        if (insertsTrack) {
            *insertsTrack = true;
        }
        return 0;
    }

    for (int i = 0; i < m_widget->trackCount(); ++i) {
        const int top = trackTop(i);
        const int bottom = top + trackHeight(i);
        if (y >= top && y < bottom) {
            return i;
        }
        const int nextTop = (i + 1 < m_widget->trackCount()) ? trackTop(i + 1) : bottom + TimelineWidget::kTimelineTrackSpacing;
        if (y >= bottom && y < nextTop) {
            if (insertsTrack) {
                *insertsTrack = true;
            }
            return i + 1;
        }
    }

    if (insertsTrack) {
        *insertsTrack = true;
    }
    return m_widget->trackCount();
}

int TimelineLayout::trackDividerAt(const QPoint& pos) const {
    const QRect tracks = trackRect();
    if (!tracks.contains(pos)) {
        return -1;
    }

    for (int i = 0; i < m_widget->trackCount(); ++i) {
        const int dividerY = trackTop(i) + trackHeight(i);
        if (std::abs(pos.y() - dividerY) <= TimelineWidget::kTrackResizeHandleHalfHeight) {
            return i;
        }
    }
    return -1;
}

QRect TimelineLayout::trackLabelRect(int trackIndex) const {
    return QRect(trackSidebarRect().left() + 4,
                 trackTop(trackIndex) + 2,
                 TimelineWidget::kTimelineLabelWidth - 8,
                 qMax(TimelineWidget::kTimelineClipHeight + 8, trackHeight(trackIndex) - 4));
}

QRect TimelineLayout::trackNameRect(int trackIndex) const {
    const QRect header = trackLabelRect(trackIndex);
    const QRect audioRect = trackAudioToggleRect(trackIndex);
    return QRect(header.left() + 10,
                 header.top(),
                 qMax(24, audioRect.left() - header.left() - 18),
                 header.height());
}

QRect TimelineLayout::trackVisualToggleRect(int trackIndex) const {
    const QRect header = trackLabelRect(trackIndex);
    return QRect(header.right() - 56,
                 header.center().y() - 11,
                 20,
                 22);
}

QRect TimelineLayout::trackAudioToggleRect(int trackIndex) const {
    const QRect header = trackLabelRect(trackIndex);
    return QRect(header.right() - 28,
                 header.center().y() - 11,
                 20,
                 22);
}

QRect TimelineLayout::clipRectFor(const TimelineClip& clip) const {
    const qreal clipFrame = samplesToFramePosition(clipTimelineStartSamples(clip));
    const qreal visibleFrame = clipFrame - static_cast<qreal>(m_widget->m_frameOffset);
    const int clipX = timelineContentRect().left() + qRound(visibleFrame * m_widget->m_pixelsPerFrame);
    const int clipW = qMax(40, m_widget->widthForFrames(clip.durationFrames));
    const int visualHeight =
        qMax(TimelineWidget::kTimelineClipHeight,
             trackHeight(clip.trackIndex) - (TimelineWidget::kTimelineClipVerticalPadding * 2));
    const int clipY = trackTop(clip.trackIndex) + qMax(0, (trackHeight(clip.trackIndex) - visualHeight) / 2);
    return QRect(clipX, clipY, clipW, visualHeight);
}
