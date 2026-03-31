#include "timeline_widget.h"

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

QRect TimelineWidget::drawRect() const {
    return rect().adjusted(kTimelineOuterMargin, kTimelineOuterMargin,
                           -kTimelineOuterMargin, -kTimelineOuterMargin);
}

QRect TimelineWidget::topBarRect() const {
    const QRect draw = drawRect();
    return QRect(draw.left(), draw.top(), draw.width(), kTimelineTopBarHeight);
}

QRect TimelineWidget::rulerRect() const {
    const QRect draw = drawRect();
    const QRect topBar = topBarRect();
    return QRect(draw.left(), topBar.bottom() + 8, draw.width(), kTimelineRulerHeight);
}

QRect TimelineWidget::trackRect() const {
    const QRect draw = drawRect();
    const QRect ruler = rulerRect();
    return QRect(draw.left(), ruler.bottom() + kTimelineTrackGap, draw.width(),
                 draw.height() - ((ruler.bottom() - draw.top() + 1) + kTimelineTrackGap));
}

QRect TimelineWidget::trackSidebarRect() const {
    const QRect tracks = trackRect();
    return QRect(tracks.left(), tracks.top(), kTimelineLabelWidth, tracks.height());
}

QRect TimelineWidget::timelineContentRect() const {
    const QRect tracks = trackRect();
    return QRect(tracks.left() + kTimelineLabelWidth + kTimelineLabelGap,
                 tracks.top(),
                 qMax(0, tracks.width() - kTimelineLabelWidth - kTimelineLabelGap),
                 tracks.height());
}

QRect TimelineWidget::exportRangeRect() const {
    const QRect topBar = topBarRect();
    const QRect content = timelineContentRect();
    return QRect(content.left(), topBar.top() + 26, content.width(), 20);
}

QRect TimelineWidget::exportSegmentRect(const ExportRangeSegment& segment) const {
    const QRect bar = exportRangeRect();
    const int left = xFromFrame(segment.startFrame);
    const int right = xFromFrame(segment.endFrame);
    return QRect(qMin(left, right),
                 bar.top(),
                 qMax(6, qAbs(right - left)),
                 bar.height());
}

QRect TimelineWidget::exportHandleRect(int segmentIndex, bool startHandle) const {
    if (segmentIndex < 0 || segmentIndex >= m_exportRanges.size()) {
        return QRect();
    }
    const QRect bar = exportRangeRect();
    const int64_t frame = startHandle ? m_exportRanges[segmentIndex].startFrame : m_exportRanges[segmentIndex].endFrame;
    const int x = xFromFrame(frame);
    return QRect(x - 5, bar.top() - 1, 10, bar.height() + 2);
}

QRect TimelineWidget::trackLabelRect(int trackIndex) const {
    return QRect(trackSidebarRect().left() + 4,
                 trackTop(trackIndex) + 2,
                 kTimelineLabelWidth - 8,
                 qMax(kTimelineClipHeight + 8, trackHeight(trackIndex) - 4));
}

QRect TimelineWidget::trackNameRect(int trackIndex) const {
    const QRect header = trackLabelRect(trackIndex);
    const QRect audioRect = trackAudioToggleRect(trackIndex);
    return QRect(header.left() + 10,
                 header.top(),
                 qMax(24, audioRect.left() - header.left() - 18),
                 header.height());
}

QRect TimelineWidget::trackVisualToggleRect(int trackIndex) const {
    const QRect header = trackLabelRect(trackIndex);
    return QRect(header.right() - 56,
                 header.center().y() - 11,
                 20,
                 22);
}

QRect TimelineWidget::trackAudioToggleRect(int trackIndex) const {
    const QRect header = trackLabelRect(trackIndex);
    return QRect(header.right() - 28,
                 header.center().y() - 11,
                 20,
                 22);
}

QRect TimelineWidget::clipRectFor(const TimelineClip& clip) const {
    const qreal clipFrame = samplesToFramePosition(clipTimelineStartSamples(clip));
    const qreal visibleFrame = clipFrame - static_cast<qreal>(m_frameOffset);
    const int clipX = timelineContentRect().left() + qRound(visibleFrame * m_pixelsPerFrame);
    const int clipW = qMax(40, widthForFrames(clip.durationFrames));
    const int visualHeight =
        qMax(kTimelineClipHeight, trackHeight(clip.trackIndex) - (kTimelineClipVerticalPadding * 2));
    const int clipY = trackTop(clip.trackIndex) + qMax(0, (trackHeight(clip.trackIndex) - visualHeight) / 2);
    return QRect(clipX, clipY, clipW, visualHeight);
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

const RenderSyncMarker* TimelineWidget::renderSyncMarkerAtPos(const QPoint& pos, int* clipIndexOut) const {
    for (int i = m_clips.size() - 1; i >= 0; --i) {
        const TimelineClip& clip = m_clips[i];
        for (const RenderSyncMarker& marker : m_renderSyncMarkers) {
            if (marker.clipId != clip.id) {
                continue;
            }
            if (renderSyncMarkerRect(clip, marker).contains(pos)) {
                if (clipIndexOut) {
                    *clipIndexOut = i;
                }
                return &marker;
            }
        }
    }
    if (clipIndexOut) {
        *clipIndexOut = -1;
    }
    return nullptr;
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

void TimelineWidget::openRenderSyncMarkerMenu(const QPoint& globalPos, const QString& clipId) {
    const RenderSyncMarker* currentSyncMarker = renderSyncMarkerAtFrame(clipId, m_currentFrame);
    QMenu menu(this);
    QAction* duplicateRenderFrameAction =
        menu.addAction(QStringLiteral("Render Sync: Duplicate Frames For Clip..."));
    QAction* skipRenderFrameAction =
        menu.addAction(QStringLiteral("Render Sync: Skip Frames For Clip..."));
    QAction* clearRenderSyncAction = menu.addAction(QStringLiteral("Clear Clip Render Sync At Playhead"));
    clearRenderSyncAction->setEnabled(currentSyncMarker != nullptr);

    QAction* selected = menu.exec(globalPos);
    if (!selected) {
        return;
    }

    if (selected == duplicateRenderFrameAction) {
        const int defaultCount =
            currentSyncMarker && currentSyncMarker->action == RenderSyncAction::DuplicateFrame ? currentSyncMarker->count : 1;
        bool ok = false;
        const int count = QInputDialog::getInt(this,
                                               QStringLiteral("Duplicate Frames"),
                                               QStringLiteral("How many extra frames should be duplicated for this clip?"),
                                               defaultCount,
                                               1,
                                               120,
                                               1,
                                               &ok);
        if (ok) {
            setRenderSyncMarkerAtCurrentFrame(clipId, RenderSyncAction::DuplicateFrame, count);
        }
        return;
    }

    if (selected == skipRenderFrameAction) {
        const int defaultCount =
            currentSyncMarker && currentSyncMarker->action == RenderSyncAction::SkipFrame ? currentSyncMarker->count : 1;
        bool ok = false;
        const int count = QInputDialog::getInt(this,
                                               QStringLiteral("Skip Frames"),
                                               QStringLiteral("How many frames should be skipped for this clip?"),
                                               defaultCount,
                                               1,
                                               120,
                                               1,
                                               &ok);
        if (ok) {
            setRenderSyncMarkerAtCurrentFrame(clipId, RenderSyncAction::SkipFrame, count);
        }
        return;
    }

    if (selected == clearRenderSyncAction) {
        clearRenderSyncMarkerAtCurrentFrame(clipId);
    }
}

int TimelineWidget::trackIndexAt(const QPoint& pos) const {
    for (int i = 0; i < trackCount(); ++i) {
        if (trackLabelRect(i).contains(pos)) {
            return i;
        }
    }
    return -1;
}

int TimelineWidget::clipIndexAt(const QPoint& pos) const {
    for (int i = 0; i < m_clips.size(); ++i) {
        if (clipRectFor(m_clips[i]).contains(pos)) {
            return i;
        }
    }
    return -1;
}

TimelineWidget::ClipDragMode TimelineWidget::clipDragModeAt(const TimelineClip& clip, const QPoint& pos) const {
    const QRect rect = clipRectFor(clip);
    if (!rect.contains(pos)) {
        return ClipDragMode::None;
    }
    const int edgeThreshold = qMin(10, qMax(4, rect.width() / 5));
    if (pos.x() <= rect.left() + edgeThreshold) {
        return ClipDragMode::TrimLeft;
    }
    if (pos.x() >= rect.right() - edgeThreshold) {
        return ClipDragMode::TrimRight;
    }
    return ClipDragMode::Move;
}

void TimelineWidget::updateHoverCursor(const QPoint& pos) {
    if (m_toolMode == ToolMode::Razor) {
        setCursor(clipIndexAt(pos) >= 0 ? Qt::CrossCursor : Qt::ArrowCursor);
        return;
    }
    if (trackDividerAt(pos) >= 0) {
        setCursor(Qt::SizeVerCursor);
        return;
    }
    const int clipIndex = clipIndexAt(pos);
    if (clipIndex < 0) {
        unsetCursor();
        return;
    }
    const ClipDragMode mode = clipDragModeAt(m_clips[clipIndex], pos);
    if (mode == ClipDragMode::TrimLeft || mode == ClipDragMode::TrimRight) {
        setCursor(Qt::SizeHorCursor);
        return;
    }
    setCursor(Qt::ArrowCursor);
}

