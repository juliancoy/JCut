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
    return m_layout->trackTop(trackIndex);
}

int TimelineWidget::trackTopInTrackArea(int trackIndex) const {
    return m_layout->trackTop(trackIndex) - trackRect().top();
}

int TimelineWidget::trackHeight(int trackIndex) const {
    return m_layout->trackHeight(trackIndex);
}

int TimelineWidget::totalTrackAreaHeight() const {
    return m_layout->totalTrackAreaHeight();
}

int TimelineWidget::maxVerticalScrollOffset() const {
    return m_layout->maxVerticalScrollOffset();
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
    return m_layout->trackIndexAtY(y, allowAppendTrack);
}

int TimelineWidget::trackDropTargetAtY(int y, bool* insertsTrack) const {
    return m_layout->trackDropTargetAtY(y, insertsTrack);
}

int TimelineWidget::trackDividerAt(const QPoint& pos) const {
    return m_layout->trackDividerAt(pos);
}

void TimelineWidget::updateMinimumTimelineHeight() {
    setMinimumHeight(150);
    m_verticalScrollOffset = qBound(0, m_verticalScrollOffset, maxVerticalScrollOffset());
}


bool TimelineWidget::trackHasVisualClips(int trackIndex) const {
    for (const TimelineClip& clip : m_clips) {
        if (clip.trackIndex == trackIndex && clipHasVisuals(clip)) {
            return true;
        }
    }
    return false;
}

bool TimelineWidget::trackHasAudioClips(int trackIndex) const {
    for (const TimelineClip& clip : m_clips) {
        if (clip.trackIndex == trackIndex && clip.hasAudio) {
            return true;
        }
    }
    return false;
}

bool TimelineWidget::trackVisualEnabled(int trackIndex) const {
    bool sawVisual = false;
    for (const TimelineClip& clip : m_clips) {
        if (clip.trackIndex != trackIndex || !clipHasVisuals(clip)) {
            continue;
        }
        sawVisual = true;
        if (!clip.videoEnabled) {
            return false;
        }
    }
    return sawVisual;
}

bool TimelineWidget::trackAudioEnabled(int trackIndex) const {
    bool sawAudio = false;
    for (const TimelineClip& clip : m_clips) {
        if (clip.trackIndex != trackIndex || !clip.hasAudio) {
            continue;
        }
        sawAudio = true;
        if (!clip.audioEnabled) {
            return false;
        }
    }
    return sawAudio;
}

bool TimelineWidget::setTrackVisualEnabled(int trackIndex, bool enabled) {
    bool changed = false;
    for (TimelineClip& clip : m_clips) {
        if (clip.trackIndex == trackIndex && clipHasVisuals(clip) && clip.videoEnabled != enabled) {
            clip.videoEnabled = enabled;
            changed = true;
        }
    }
    if (changed && clipsChanged) {
        clipsChanged();
    }
    if (changed) {
        update();
    }
    return changed;
}

bool TimelineWidget::setTrackAudioEnabled(int trackIndex, bool enabled) {
    bool changed = false;
    for (TimelineClip& clip : m_clips) {
        if (clip.trackIndex == trackIndex && clip.hasAudio && clip.audioEnabled != enabled) {
            clip.audioEnabled = enabled;
            changed = true;
        }
    }
    if (changed && clipsChanged) {
        clipsChanged();
    }
    if (changed) {
        update();
    }
    return changed;
}

void TimelineWidget::insertTrackAt(int trackIndex) {
    const int insertAt = qBound(0, trackIndex, trackCount());
    ensureTrackCount(trackCount());
    for (TimelineClip& clip : m_clips) {
        if (clip.trackIndex >= insertAt) {
            clip.trackIndex += 1;
        }
    }
    TimelineTrack track;
    track.name = defaultTrackName(insertAt);
    track.height = kDefaultTrackHeight;
    m_tracks.insert(insertAt, track);
    for (int i = insertAt + 1; i < m_tracks.size(); ++i) {
        if (m_tracks[i].name.startsWith(QStringLiteral("Track "))) {
            m_tracks[i].name = defaultTrackName(i);
        }
    }
    updateMinimumTimelineHeight();
}
