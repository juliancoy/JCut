#include "timeline_widget.h"

int TimelineWidget::trackCount() const {
    int maxTrack = -1;
    for (const TimelineClip& clip : m_clips) {
        maxTrack = qMax(maxTrack, clip.trackIndex);
    }
    return qMax(1, qMax(m_tracks.size(), maxTrack + 1));
}

int TimelineWidget::nextTrackIndex() const {
    return trackCount();
}

void TimelineWidget::normalizeTrackIndices() {
    int maxTrackIndex = -1;
    for (const TimelineClip& clip : m_clips) {
        maxTrackIndex = qMax(maxTrackIndex, clip.trackIndex);
    }

    for (TimelineClip& clip : m_clips) {
        clip.trackIndex = qMax(0, clip.trackIndex);
    }

    ensureTrackCount(qMax(1, qMax(m_tracks.size(), maxTrackIndex + 1)));
    updateMinimumTimelineHeight();
}

void TimelineWidget::ensureTrackCount(int count) {
    const int desired = qMax(1, count);
    while (m_tracks.size() < desired) {
        TimelineTrack track;
        track.name = defaultTrackName(m_tracks.size());
        track.height = kDefaultTrackHeight;
        m_tracks.push_back(track);
    }
    for (int i = 0; i < m_tracks.size(); ++i) {
        if (m_tracks[i].name.trimmed().isEmpty()) {
            m_tracks[i].name = defaultTrackName(i);
        }
        m_tracks[i].height = qMax(kMinTrackHeight, m_tracks[i].height);
    }
}

QString TimelineWidget::defaultTrackName(int trackIndex) const {
    return QStringLiteral("Track %1").arg(trackIndex + 1);
}

int TimelineWidget::trackTop(int trackIndex) const {
    const QRect tracks = trackRect();
    int y = tracks.top() + kTimelineTrackInnerPadding - m_verticalScrollOffset;
    for (int i = 0; i < trackIndex && i < m_tracks.size(); ++i) {
        y += trackHeight(i) + kTimelineTrackSpacing;
    }
    return y;
}

int TimelineWidget::trackHeight(int trackIndex) const {
    if (trackIndex >= 0 && trackIndex < m_tracks.size()) {
        return qMax(kMinTrackHeight, m_tracks[trackIndex].height);
    }
    return kDefaultTrackHeight;
}

int TimelineWidget::totalTrackAreaHeight() const {
    int trackAreaHeight = kTimelineTrackInnerPadding * 2;
    for (int i = 0; i < trackCount(); ++i) {
        trackAreaHeight += trackHeight(i);
    }
    trackAreaHeight += qMax(0, trackCount() - 1) * kTimelineTrackSpacing;
    return trackAreaHeight;
}

int TimelineWidget::maxVerticalScrollOffset() const {
    return qMax(0, totalTrackAreaHeight() - trackRect().height());
}

void TimelineWidget::setVerticalScrollOffset(int offset) {
    const int bounded = qBound(0, offset, maxVerticalScrollOffset());
    if (m_verticalScrollOffset == bounded) {
        return;
    }
    m_verticalScrollOffset = bounded;
    update();
}

void TimelineWidget::setTimelineZoom(qreal pixelsPerFrame) {
    const QRect contentRect = timelineContentRect();
    const int64_t fullTimelineFrames = qMax<int64_t>(1, totalFrames());
    const qreal fitAllPixelsPerFrame =
        contentRect.width() > 0
            ? static_cast<qreal>(contentRect.width()) / static_cast<qreal>(fullTimelineFrames)
            : 0.01;
    const qreal minPixelsPerFrame = qMin<qreal>(0.25, fitAllPixelsPerFrame);
    const qreal bounded = qBound(minPixelsPerFrame, pixelsPerFrame, 24.0);
    if (qFuzzyCompare(m_pixelsPerFrame, bounded)) {
        return;
    }
    m_pixelsPerFrame = bounded;
    const int64_t visibleFrames = qMax<int64_t>(1, qRound(static_cast<qreal>(contentRect.width()) / m_pixelsPerFrame));
    const int64_t maxOffset = qMax<int64_t>(0, totalFrames() - visibleFrames);
    m_frameOffset = qBound<int64_t>(0, m_frameOffset, maxOffset);
    if (m_pixelsPerFrame <= fitAllPixelsPerFrame + 0.0001) {
        m_frameOffset = 0;
    }
    update();
}

int TimelineWidget::trackIndexAtY(int y, bool allowAppendTrack) const {
    if (m_tracks.isEmpty()) {
        return 0;
    }

    for (int i = 0; i < trackCount(); ++i) {
        const int top = trackTop(i);
        const int bottom = top + trackHeight(i);
        if (y >= top && y < bottom) {
            return i;
        }
    }

    const int lastTrackBottom = trackTop(trackCount() - 1) + trackHeight(trackCount() - 1);
    if (allowAppendTrack && y >= lastTrackBottom) {
        return trackCount();
    }
    if (y < trackTop(0)) {
        return 0;
    }
    return trackCount() - 1;
}

int TimelineWidget::trackDropTargetAtY(int y, bool* insertsTrack) const {
    if (insertsTrack) {
        *insertsTrack = false;
    }
    if (m_tracks.isEmpty()) {
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

    for (int i = 0; i < trackCount(); ++i) {
        const int top = trackTop(i);
        const int bottom = top + trackHeight(i);
        if (y >= top && y < bottom) {
            return i;
        }
        const int nextTop = (i + 1 < trackCount()) ? trackTop(i + 1) : bottom + kTimelineTrackSpacing;
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
    return trackCount();
}

int TimelineWidget::trackDividerAt(const QPoint& pos) const {
    const QRect tracks = trackRect();
    if (!tracks.contains(pos)) {
        return -1;
    }

    for (int i = 0; i < trackCount(); ++i) {
        const int dividerY = trackTop(i) + trackHeight(i);
        if (std::abs(pos.y() - dividerY) <= kTrackResizeHandleHalfHeight) {
            return i;
        }
    }
    return -1;
}

void TimelineWidget::updateMinimumTimelineHeight() {
    setMinimumHeight(150);
    m_verticalScrollOffset = qBound(0, m_verticalScrollOffset, maxVerticalScrollOffset());
}

