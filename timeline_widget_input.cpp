#include "timeline_widget.h"
#include "titles.h"

#include <QApplication>
#include <QClipboard>

int TimelineWidget::trackIndexAt(const QPoint& pos) const {
    for (int i = 0; i < trackCount(); ++i) {
        if (trackLabelRect(i).contains(pos)) {
            return i;
        }
    }
    return -1;
}

int TimelineWidget::clipIndexAt(const QPoint& pos) const {
    for (int i = m_clips.size() - 1; i >= 0; --i) {
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

const RenderSyncMarker* TimelineWidget::renderSyncMarkerAtPos(const QPoint& pos, int* clipIndexOut) const {
    for (int i = m_clips.size() - 1; i >= 0; --i) {
        const TimelineClip& clip = m_clips[i];
        const QVector<RenderSyncMarker>* clipMarkers = renderSyncMarkersForClipId(clip.id);
        if (!clipMarkers) {
            continue;
        }
        for (const RenderSyncMarker& marker : *clipMarkers) {
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

bool TimelineWidget::clipHasProxyAvailable(const TimelineClip& clip) const {
    if (clip.mediaType != ClipMediaType::Video) {
        return false;
    }
    return !playbackProxyPathForClip(clip).isEmpty();
}

int64_t TimelineWidget::frameFromX(qreal x) const {
    const int left = timelineContentRect().left();
    const qreal normalized = qMax<qreal>(0.0, x - left);
    return m_frameOffset + static_cast<int64_t>(normalized / m_pixelsPerFrame);
}

int TimelineWidget::xFromFrame(int64_t frame) const {
    return timelineContentRect().left() + widthForFrames(frame - m_frameOffset);
}

int TimelineWidget::widthForFrames(int64_t frames) const {
    return static_cast<int>(frames * m_pixelsPerFrame);
}

QString TimelineWidget::timecodeForFrame(int64_t frame) const {
    const int64_t seconds = frame / kTimelineFps;
    const int64_t minutes = seconds / 60;
    const int64_t secs = seconds % 60;
    return QStringLiteral("%1:%2")
        .arg(minutes, 2, 10, QLatin1Char('0'))
        .arg(secs, 2, 10, QLatin1Char('0'));
}

int64_t TimelineWidget::mediaDurationFrames(const QFileInfo& info) const {
    const QString suffix = info.suffix().toLower();
    return probeMediaFile(info.absoluteFilePath(), guessDurationFrames(suffix)).durationFrames;
}

bool TimelineWidget::hasFileUrls(const QMimeData* mimeData) const {
    if (!mimeData || !mimeData->hasUrls()) return false;
    for (const QUrl& url : mimeData->urls()) {
        if (url.isLocalFile()) return true;
    }
    return false;
}

int64_t TimelineWidget::guessDurationFrames(const QString& suffix) const {
    static const QHash<QString, int64_t> kDurations = {
        {QStringLiteral("mp4"), 180},
        {QStringLiteral("mov"), 180},
        {QStringLiteral("mkv"), 180},
        {QStringLiteral("webm"), 180},
        {QStringLiteral("png"), 90},
        {QStringLiteral("jpg"), 90},
        {QStringLiteral("jpeg"), 90},
        {QStringLiteral("webp"), 90},
    };
    return kDurations.value(suffix, 120);
}

QColor TimelineWidget::colorForPath(const QString& path) const {
    const quint32 hash = qHash(path);
    return QColor::fromHsv(static_cast<int>(hash % 360), 160, 220, 220);
}

TimelineClip TimelineWidget::buildClipFromFile(const QString& filePath,
                                               int64_t startFrame,
                                               int trackIndex) const {
    const QFileInfo info(filePath);
    const MediaProbeResult probe = probeMediaFile(filePath, guessDurationFrames(info.suffix().toLower()));
    const qreal sourceFps = probe.fps > 0.001 ? probe.fps : static_cast<qreal>(kTimelineFps);
    const int64_t timelineDurationFrames = qMax<int64_t>(
        1,
        qRound64((static_cast<qreal>(qMax<int64_t>(1, probe.durationFrames)) / sourceFps) *
                 static_cast<qreal>(kTimelineFps)));

    TimelineClip clip;
    clip.id = QUuid::createUuid().toString(QUuid::WithoutBraces);
    clip.filePath = filePath;
    clip.label = isImageSequencePath(filePath) ? imageSequenceDisplayLabel(filePath) : info.fileName();
    clip.mediaType = probe.mediaType;
    clip.sourceKind = probe.sourceKind;
    clip.hasAudio = probe.hasAudio;
    clip.sourceFps = sourceFps;
    clip.sourceDurationFrames = probe.durationFrames;
    clip.startFrame = startFrame;
    clip.durationFrames = timelineDurationFrames;
    clip.trackIndex = trackIndex;
    clip.color = colorForPath(filePath);
    if (clip.mediaType == ClipMediaType::Audio) {
        clip.color = QColor(QStringLiteral("#2f7f93"));
    }
    normalizeClipTransformKeyframes(clip);
    normalizeClipGradingKeyframes(clip);
    normalizeClipOpacityKeyframes(clip);
    return clip;
}

void TimelineWidget::addClipFromFile(const QString& filePath, int64_t startFrame) {
    const QFileInfo info(filePath);
    if (!info.exists() || (!info.isFile() && !isImageSequencePath(filePath))) return;

    TimelineClip clip = buildClipFromFile(filePath,
                                          startFrame >= 0 ? startFrame : totalFrames(),
                                          nextTrackIndex());
    m_clips.push_back(clip);
    normalizeTrackIndices();
    sortClips();

    if (clipsChanged) clipsChanged();
    update();
}

void TimelineWidget::dragEnterEvent(QDragEnterEvent* event) {
    if (hasFileUrls(event->mimeData())) {
        event->acceptProposedAction();
        return;
    }
    QWidget::dragEnterEvent(event);
}

void TimelineWidget::dragMoveEvent(QDragMoveEvent* event) {
    if (hasFileUrls(event->mimeData())) {
        m_dropFrame = frameFromX(event->position().x());
        m_trackDropIndex = trackDropTargetAtY(event->position().toPoint().y(), &m_trackDropInGap);
        event->acceptProposedAction();
        update();
        return;
    }
    QWidget::dragMoveEvent(event);
}

void TimelineWidget::dragLeaveEvent(QDragLeaveEvent* event) {
    m_dropFrame = -1;
    m_trackDropIndex = -1;
    m_trackDropInGap = false;
    m_snapIndicatorFrame = -1;
    QWidget::dragLeaveEvent(event);
    update();
}

void TimelineWidget::dropEvent(QDropEvent* event) {
    if (!hasFileUrls(event->mimeData())) {
        QWidget::dropEvent(event);
        return;
    }

    int64_t insertFrame = frameFromX(event->position().x());
    bool insertsTrack = false;
    int targetTrack = trackDropTargetAtY(event->position().toPoint().y(), &insertsTrack);
    if (targetTrack < 0) {
        targetTrack = nextTrackIndex();
        insertsTrack = true;
    }
    if (insertsTrack) {
        insertTrackAt(targetTrack);
    }

    for (const QUrl& url : event->mimeData()->urls()) {
        if (!url.isLocalFile()) continue;

        const QString filePath = url.toLocalFile();
        const QFileInfo info(filePath);
        if (!info.exists() || (info.isDir() && !isImageSequencePath(filePath))) continue;

        ensureTrackCount(targetTrack + 1);
        TimelineClip clip = buildClipFromFile(filePath, insertFrame, targetTrack);
        
        // Check for conflict with existing clips on this track
        if (wouldClipConflictWithTrack(clip, targetTrack)) {
            // Find the next available track that doesn't have a conflict
            int newTrack = targetTrack;
            bool foundNonConflictingTrack = false;
            for (int t = 0; t <= trackCount(); ++t) {
                if (!wouldClipConflictWithTrack(clip, t)) {
                    newTrack = t;
                    foundNonConflictingTrack = true;
                    break;
                }
            }
            if (!foundNonConflictingTrack) {
                newTrack = trackCount();
                insertTrackAt(newTrack);
            }
            clip.trackIndex = newTrack;
            targetTrack = newTrack;
        }
        
        m_clips.push_back(clip);
        insertFrame += clip.durationFrames + 6;
    }

    normalizeTrackIndices();
    sortClips();
    m_dropFrame = -1;
    m_trackDropIndex = -1;
    m_trackDropInGap = false;
    event->acceptProposedAction();

    if (clipsChanged) clipsChanged();
    update();
}

void TimelineWidget::mouseDoubleClickEvent(QMouseEvent* event) {
    if (event->button() == Qt::LeftButton) {
        const int trackHit = trackIndexAt(event->position().toPoint());
        if (trackHit >= 0) {
            QMenu menu(this);
            QAction* renameAction = menu.addAction(QStringLiteral("Rename"));
            QAction* crossfadeAction = menu.addAction(QStringLiteral("Crossfade Consecutive Clips..."));
            QAction* chosen = menu.exec(event->globalPosition().toPoint());
            if (chosen == renameAction) {
                renameTrack(trackHit);
            } else if (chosen == crossfadeAction) {
                bool accepted = false;
                const double seconds = QInputDialog::getDouble(this,
                                                               QStringLiteral("Crossfade Consecutive Clips"),
                                                               QStringLiteral("Crossfade duration (seconds)"),
                                                               0.50,
                                                               0.01,
                                                               30.00,
                                                               2,
                                                               &accepted);
                if (accepted) {
                    applyCrossfadeToTrack(trackHit, seconds);
                }
            }
            event->accept();
            return;
        }
    }
    QWidget::mouseDoubleClickEvent(event);
}

void TimelineWidget::mousePressEvent(QMouseEvent* event) {
    if (event->button() == Qt::LeftButton) {
        int markerClipIndex = -1;
        if (const RenderSyncMarker* marker = renderSyncMarkerAtPos(event->position().toPoint(), &markerClipIndex)) {
            if (markerClipIndex >= 0) {
                setSelectedClipId(m_clips[markerClipIndex].id);
                m_currentFrame = marker->frame;
                if (seekRequested) seekRequested(marker->frame);
                openRenderSyncMarkerMenu(event->globalPosition().toPoint(), marker->clipId);
                update();
                event->accept();
                return;
            }
        }
        bool startHandle = false;
        const int exportHandleSegment = exportHandleAtPos(event->position().toPoint(), &startHandle);
        if (exportHandleSegment >= 0) {
            m_exportRangeDragSegmentIndex = exportHandleSegment;
            m_exportRangeDragMode = startHandle ? ExportRangeDragMode::Start : ExportRangeDragMode::End;
            event->accept();
            return;
        }
        if (exportRangeRect().contains(event->position().toPoint())) {
            const int64_t frame = frameFromX(event->position().x());
            if (exportSegmentIndexAtFrame(frame) >= 0) {
                m_currentFrame = frame;
                if (seekRequested) seekRequested(frame);
                update();
                event->accept();
                return;
            }
        }
        const int dividerHit = trackDividerAt(event->position().toPoint());
        if (dividerHit >= 0) {
            m_resizingTrackIndex = dividerHit;
            m_resizeOriginY = event->position().toPoint().y();
            m_resizeOriginHeight = trackHeight(dividerHit);
            setCursor(Qt::SizeVerCursor);
            event->accept();
            return;
        }
        const int trackHit = trackIndexAt(event->position().toPoint());
        if (trackHit >= 0) {
            if (trackVisualToggleRect(trackHit).contains(event->position().toPoint()) &&
                trackHasVisualClips(trackHit)) {
                TrackVisualMode nextMode = TrackVisualMode::Enabled;
                switch (trackVisualMode(trackHit)) {
                case TrackVisualMode::Enabled:
                    nextMode = TrackVisualMode::ForceOpaque;
                    break;
                case TrackVisualMode::ForceOpaque:
                    nextMode = TrackVisualMode::Hidden;
                    break;
                case TrackVisualMode::Hidden:
                default:
                    nextMode = TrackVisualMode::Enabled;
                    break;
                }
                setTrackVisualMode(trackHit, nextMode);
                event->accept();
                return;
            }
            if (trackAudioToggleRect(trackHit).contains(event->position().toPoint()) &&
                trackHasAudioClips(trackHit)) {
                setTrackAudioEnabled(trackHit, !trackAudioEnabled(trackHit));
                event->accept();
                return;
            }
            setSelectedTrackIndex(trackHit);
            m_draggedTrackIndex = trackHit;
            m_trackDropIndex = trackHit;
            update();
            return;
        }
        const int hitIndex = clipIndexAt(event->position().toPoint());
        if (hitIndex >= 0 && m_toolMode == ToolMode::Razor) {
            const QString clickedClipId = m_clips[hitIndex].id;
            if (!isClipSelected(clickedClipId)) {
                setSelectedClipId(clickedClipId);
            }
            const int64_t clickFrame = frameFromX(event->position().x());
            splitSelectedClipAtFrame(clickFrame);
            update();
            return;
        }
        if (hitIndex >= 0) {
            const QString clickedClipId = m_clips[hitIndex].id;
            const Qt::KeyboardModifiers modifiers = event->modifiers();
            if (modifiers.testFlag(Qt::ShiftModifier) ||
                modifiers.testFlag(Qt::ControlModifier) ||
                modifiers.testFlag(Qt::MetaModifier)) {
                selectClipWithModifiers(clickedClipId, modifiers);
                update();
                return;
            }
            setSelectedClipId(clickedClipId);
            if (!m_clips[hitIndex].locked) {
                m_dragMode = clipDragModeAt(m_clips[hitIndex], event->position().toPoint());
                m_draggedClipIndex = hitIndex;
                m_dragOriginalStartFrame = m_clips[hitIndex].startFrame;
                m_dragOriginalDurationFrames = m_clips[hitIndex].durationFrames;
                m_dragOriginalSourceInFrame = m_clips[hitIndex].sourceInFrame;
                m_dragOriginalTransformKeyframes = m_clips[hitIndex].transformKeyframes;
                m_dragOffsetFrames = frameFromX(event->position().x()) - m_clips[hitIndex].startFrame;
            }
            update();
            return;
        }

        const int64_t frame = frameFromX(event->position().x());
        m_currentFrame = frame;
        if (seekRequested) seekRequested(frame);
        update();
        return;
    }
    QWidget::mousePressEvent(event);
}

void TimelineWidget::mouseMoveEvent(QMouseEvent* event) {
    const int hoveredClipIndex = clipIndexAt(event->position().toPoint());
    const QString hoveredClipId =
        hoveredClipIndex >= 0 ? m_clips[hoveredClipIndex].id : QString();
    if (hoveredClipId != m_hoveredClipId) {
        m_hoveredClipId = hoveredClipId;
        update();
    }
    if (hoveredClipIndex >= 0) {
        const TimelineClip& hoveredClip = m_clips[hoveredClipIndex];
        const bool isSequence = hoveredClip.sourceKind == MediaSourceKind::ImageSequence;
        const QString typeLabel =
            hoveredClip.mediaType == ClipMediaType::Audio ? QStringLiteral("Audio")
            : (hoveredClip.mediaType == ClipMediaType::Image ? QStringLiteral("Image")
               : (isSequence ? QStringLiteral("Sequence") : QStringLiteral("Video")));
        const int64_t localTimelineFrame =
            qBound<int64_t>(0,
                            m_currentFrame - hoveredClip.startFrame,
                            qMax<int64_t>(0, hoveredClip.durationFrames - 1));
        const int64_t clipFrame =
            adjustedClipLocalFrameAtTimelineFrame(hoveredClip, localTimelineFrame, m_renderSyncMarkers);
        setToolTip(QStringLiteral("%1\n%2\nFrame %3")
                       .arg(hoveredClip.label, typeLabel)
                       .arg(clipFrame));
    } else {
        setToolTip(QString());
    }

    if (m_toolMode == ToolMode::Razor) {
        const int64_t hoverFrame = frameFromX(event->position().x());
        if (m_razorHoverFrame != hoverFrame) {
            m_razorHoverFrame = hoverFrame;
            update();
        }
    }

    if (m_exportRangeDragMode != ExportRangeDragMode::None && (event->buttons() & Qt::LeftButton)) {
        if (m_exportRangeDragSegmentIndex < 0 || m_exportRangeDragSegmentIndex >= m_exportRanges.size()) {
            m_exportRangeDragMode = ExportRangeDragMode::None;
            m_exportRangeDragSegmentIndex = -1;
            return;
        }
        const int64_t frame = qBound<int64_t>(0, frameFromX(event->position().x()), totalFrames());
        if (m_exportRangeDragMode == ExportRangeDragMode::Start) {
            m_exportRanges[m_exportRangeDragSegmentIndex].startFrame =
                qMin(frame, m_exportRanges[m_exportRangeDragSegmentIndex].endFrame);
        } else {
            m_exportRanges[m_exportRangeDragSegmentIndex].endFrame =
                qMax(frame, m_exportRanges[m_exportRangeDragSegmentIndex].startFrame);
        }
        normalizeExportRanges();
        if (exportRangeChanged) {
            exportRangeChanged();
        }
        update();
        return;
    }
    if (m_draggedTrackIndex >= 0 && (event->buttons() & Qt::LeftButton)) {
        const int proposed = qBound(0, trackIndexAtY(event->position().toPoint().y(), false), trackCount() - 1);
        if (proposed != m_trackDropIndex) {
            m_trackDropIndex = proposed;
            update();
        }
        return;
    }
    if (m_resizingTrackIndex >= 0 && (event->buttons() & Qt::LeftButton)) {
        ensureTrackCount(m_resizingTrackIndex + 1);
        const int delta = event->position().toPoint().y() - m_resizeOriginY;
        m_tracks[m_resizingTrackIndex].height = qMax(kMinTrackHeight, m_resizeOriginHeight + delta);
        updateMinimumTimelineHeight();
        if (trackLayoutChanged) {
            trackLayoutChanged();
        }
        update();
        return;
    }
    if (m_draggedClipIndex >= 0 && (event->buttons() & Qt::LeftButton)) {
        TimelineClip& clip = m_clips[m_draggedClipIndex];
        const int64_t pointerFrame = frameFromX(event->position().x());
        static constexpr int64_t kMinClipFrames = 1;
        const bool isImage = clip.mediaType == ClipMediaType::Image;
        m_snapIndicatorFrame = -1;
        if (m_dragMode == ClipDragMode::Move) {
            bool snapped = false;
            int64_t snappedBoundaryFrame = -1;
            const int64_t unsnappedStartFrame = qMax<int64_t>(0, pointerFrame - m_dragOffsetFrames);
            const int64_t newStartFrame =
                snapMoveStartFrame(clip, unsnappedStartFrame, &snapped, &snappedBoundaryFrame);
            bool insertsTrack = false;
            const int proposedTrack = trackDropTargetAtY(event->position().toPoint().y(), &insertsTrack);
            
            // Always update frame position
            clip.startFrame = newStartFrame;
            m_snapIndicatorFrame = snapped ? snappedBoundaryFrame : -1;
            m_currentFrame = newStartFrame;
            
            // Check for conflict when moving to a different track
            if (proposedTrack >= 0 && proposedTrack != clip.trackIndex) {
                TimelineClip tempClip = clip;
                tempClip.trackIndex = proposedTrack;
                if (!wouldClipConflictWithTrack(tempClip, proposedTrack, clip.id)) {
                    // No conflict - allow the track change
                    clip.trackIndex = proposedTrack;
                }
            }
            m_trackDropIndex = proposedTrack;
            m_trackDropInGap = insertsTrack;
        } else if (m_dragMode == ClipDragMode::TrimLeft) {
            const int64_t maxStartFrame = m_dragOriginalStartFrame + m_dragOriginalDurationFrames - kMinClipFrames;
            bool snapped = false;
            int64_t snappedBoundaryFrame = -1;
            const int64_t boundedPointerFrame = qBound<int64_t>(0, pointerFrame, maxStartFrame);
            const int64_t newStartFrame =
                qBound<int64_t>(0,
                                snapTrimLeftFrame(clip, boundedPointerFrame, &snapped, &snappedBoundaryFrame),
                                maxStartFrame);
            const int64_t trimDelta = newStartFrame - m_dragOriginalStartFrame;
            clip.startFrame = newStartFrame;
            m_snapIndicatorFrame = snapped ? snappedBoundaryFrame : -1;
            if (isImage) {
                clip.sourceInFrame = 0;
                clip.durationFrames = m_dragOriginalDurationFrames + (m_dragOriginalStartFrame - newStartFrame);
                clip.sourceDurationFrames = clip.durationFrames;
                clip.transformKeyframes = m_dragOriginalTransformKeyframes;
            } else {
                const qreal playbackRate = qBound<qreal>(0.001, clip.playbackRate, 4.0);
                const int64_t consumedSourceFrames =
                    static_cast<int64_t>(std::floor(static_cast<qreal>(trimDelta) * playbackRate));
                clip.sourceInFrame = m_dragOriginalSourceInFrame + consumedSourceFrames;
                clip.durationFrames = m_dragOriginalDurationFrames - trimDelta;
                clip.transformKeyframes.clear();
                for (const TimelineClip::TransformKeyframe& keyframe : m_dragOriginalTransformKeyframes) {
                    if (keyframe.frame < trimDelta) {
                        continue;
                    }
                    TimelineClip::TransformKeyframe shifted = keyframe;
                    shifted.frame -= trimDelta;
                    clip.transformKeyframes.push_back(shifted);
                }
                normalizeClipTransformKeyframes(clip);
            }
            m_currentFrame = newStartFrame;
        } else if (m_dragMode == ClipDragMode::TrimRight) {
            const int64_t minEndFrame = m_dragOriginalStartFrame + kMinClipFrames;
            const int64_t maxEndFrame = isImage
                ? std::numeric_limits<int64_t>::max()
                : m_dragOriginalStartFrame +
                      qMax<int64_t>(kMinClipFrames,
                                    static_cast<int64_t>(std::ceil(
                                        static_cast<qreal>(mediaDurationFrames(QFileInfo(clip.filePath)) - m_dragOriginalSourceInFrame) /
                                        qBound<qreal>(0.001, clip.playbackRate, 4.0))));
            bool snapped = false;
            int64_t snappedBoundaryFrame = -1;
            const int64_t boundedPointerFrame = isImage
                ? qMax<int64_t>(minEndFrame, pointerFrame)
                : qBound<int64_t>(minEndFrame, pointerFrame, maxEndFrame);
            const int64_t newEndFrame = isImage
                ? qMax<int64_t>(minEndFrame,
                                snapTrimRightFrame(clip, boundedPointerFrame, &snapped, &snappedBoundaryFrame))
                : qBound<int64_t>(minEndFrame,
                                  snapTrimRightFrame(clip, boundedPointerFrame, &snapped, &snappedBoundaryFrame),
                                  maxEndFrame);
            clip.durationFrames = newEndFrame - m_dragOriginalStartFrame;
            m_snapIndicatorFrame = snapped ? snappedBoundaryFrame : -1;
            if (isImage) {
                clip.sourceDurationFrames = clip.durationFrames;
                clip.transformKeyframes = m_dragOriginalTransformKeyframes;
            } else {
                clip.transformKeyframes.clear();
                for (const TimelineClip::TransformKeyframe& keyframe : m_dragOriginalTransformKeyframes) {
                    if (keyframe.frame < clip.durationFrames) {
                        clip.transformKeyframes.push_back(keyframe);
                    }
                }
                normalizeClipTransformKeyframes(clip);
            }
            m_currentFrame = newEndFrame;
        }
        update();
        return;
    }
    updateHoverCursor(event->position().toPoint());
    QWidget::mouseMoveEvent(event);
}

void TimelineWidget::leaveEvent(QEvent* event) {
    if (!m_hoveredClipId.isEmpty()) {
        m_hoveredClipId.clear();
        update();
    }
    if (m_razorHoverFrame >= 0) {
        m_razorHoverFrame = -1;
        update();
    }
    setToolTip(QString());
    QWidget::leaveEvent(event);
}

void TimelineWidget::mouseReleaseEvent(QMouseEvent* event) {
    if (event->button() == Qt::LeftButton && m_exportRangeDragMode != ExportRangeDragMode::None) {
        m_exportRangeDragMode = ExportRangeDragMode::None;
        m_exportRangeDragSegmentIndex = -1;
        update();
        return;
    }
    if (event->button() == Qt::LeftButton && m_resizingTrackIndex >= 0) {
        m_resizingTrackIndex = -1;
        m_resizeOriginY = 0;
        m_resizeOriginHeight = 0;
        if (clipsChanged) {
            clipsChanged();
        }
        updateHoverCursor(event->position().toPoint());
        update();
        return;
    }
    if (event->button() == Qt::LeftButton && m_draggedTrackIndex >= 0) {
        const int fromTrack = m_draggedTrackIndex;
        const int toTrack = qBound(0, m_trackDropIndex, trackCount() - 1);
        if (fromTrack != toTrack) {
            ensureTrackCount(trackCount());
            for (TimelineClip& clip : m_clips) {
                if (clip.trackIndex == fromTrack) {
                    clip.trackIndex = toTrack;
                } else if (fromTrack < toTrack && clip.trackIndex > fromTrack && clip.trackIndex <= toTrack) {
                    clip.trackIndex -= 1;
                } else if (fromTrack > toTrack && clip.trackIndex >= toTrack && clip.trackIndex < fromTrack) {
                    clip.trackIndex += 1;
                }
            }
            if (fromTrack >= 0 && fromTrack < m_tracks.size() && toTrack >= 0 && toTrack < m_tracks.size()) {
                TimelineTrack movedTrack = m_tracks.takeAt(fromTrack);
                m_tracks.insert(toTrack, movedTrack);
            }
            normalizeTrackIndices();
            sortClips();
            if (clipsChanged) clipsChanged();
        }
        m_draggedTrackIndex = -1;
        m_trackDropIndex = -1;
        m_trackDropInGap = false;
        m_snapIndicatorFrame = -1;
        update();
        return;
    }
    if (event->button() == Qt::LeftButton && m_draggedClipIndex >= 0) {
        if (m_dragMode == ClipDragMode::Move && m_trackDropInGap && m_trackDropIndex >= 0) {
            const QString movingClipId = m_clips[m_draggedClipIndex].id;
            insertTrackAt(m_trackDropIndex);
            for (TimelineClip& clip : m_clips) {
                if (clip.id == movingClipId) {
                    clip.trackIndex = m_trackDropIndex;
                    break;
                }
            }
        }
        normalizeTrackIndices();
        sortClips();
        m_draggedClipIndex = -1;
        m_dragMode = ClipDragMode::None;
        m_trackDropIndex = -1;
        m_trackDropInGap = false;
        m_snapIndicatorFrame = -1;
        m_dragOffsetFrames = 0;
        m_dragOriginalStartFrame = 0;
        m_dragOriginalDurationFrames = 0;
        m_dragOriginalSourceInFrame = 0;
        m_dragOriginalTransformKeyframes.clear();
        if (clipsChanged) clipsChanged();
        updateHoverCursor(event->position().toPoint());
        update();
        return;
    }
    QWidget::mouseReleaseEvent(event);
}

void TimelineWidget::contextMenuEvent(QContextMenuEvent* event) {
    const int clipIndex = clipIndexAt(event->pos());
    const QString clickedClipId = clipIndex >= 0 ? m_clips[clipIndex].id : QString();
    int markerClipIndex = -1;
    const RenderSyncMarker* clickedMarker = renderSyncMarkerAtPos(event->pos(), &markerClipIndex);
    if (clickedMarker && markerClipIndex >= 0) {
        setSelectedClipId(m_clips[markerClipIndex].id);
        m_currentFrame = clickedMarker->frame;
        if (seekRequested) {
            seekRequested(clickedMarker->frame);
        }
        openRenderSyncMarkerMenu(event->globalPos(), clickedMarker->clipId);
        return;
    }
    const QString targetClipId =
        clipIndex >= 0 ? m_clips[clipIndex].id : selectedClipId();
    QMenu menu(this);
    QAction* setExportStartAction = menu.addAction(QStringLiteral("Set Export Start At Playhead"));
    QAction* setExportEndAction = menu.addAction(QStringLiteral("Set Export End At Playhead"));
    QAction* splitExportRangeAction = menu.addAction(QStringLiteral("Split Export Range At Playhead"));
    QAction* resetExportRangeAction = menu.addAction(QStringLiteral("Reset Export Range"));
    menu.addSeparator();
    QAction* duplicateRenderFrameAction =
        menu.addAction(QStringLiteral("Render Sync: Duplicate Frames For Clip..."));
    QAction* skipRenderFrameAction =
        menu.addAction(QStringLiteral("Render Sync: Skip Frames For Clip..."));
    QAction* clearRenderSyncAction = menu.addAction(QStringLiteral("Clear Clip Render Sync At Playhead"));
    const RenderSyncMarker* currentSyncMarker = renderSyncMarkerAtFrame(targetClipId, m_currentFrame);
    const bool hasTargetClip = !targetClipId.isEmpty();
    const int splitSegmentIndex = exportSegmentIndexAtFrame(m_currentFrame);
    splitExportRangeAction->setEnabled(
        splitSegmentIndex >= 0 &&
        m_currentFrame > m_exportRanges[splitSegmentIndex].startFrame &&
        m_currentFrame <= m_exportRanges[splitSegmentIndex].endFrame);
    duplicateRenderFrameAction->setEnabled(hasTargetClip);
    skipRenderFrameAction->setEnabled(hasTargetClip);
    clearRenderSyncAction->setEnabled(currentSyncMarker != nullptr);

    menu.addSeparator();
    QAction* addTitleClipAction = menu.addAction(QStringLiteral("Add Title Clip"));

    QAction* syncAction = nullptr;
    QAction* nudgeLeftAction = nullptr;
    QAction* nudgeRightAction = nullptr;
    QAction* splitClipAction = nullptr;
    QAction* deleteAction = nullptr;
    QAction* copyClipNameAction = nullptr;
    QAction* gradingAction = nullptr;
    QAction* resetGradingAction = nullptr;
    QAction* scaleToFillAction = nullptr;
    QAction* propertiesAction = nullptr;
    QAction* refreshMetadataAction = nullptr;
    QAction* transcribeAction = nullptr;
    QAction* createProxyAction = nullptr;
    QAction* deleteProxyAction = nullptr;

    QSet<QString> contextSelection = selectedClipIds();
    if (clipIndex >= 0 && !clickedClipId.isEmpty() && !contextSelection.contains(clickedClipId)) {
        contextSelection = QSet<QString>{clickedClipId};
    }
    if (contextSelection.isEmpty() && !clickedClipId.isEmpty()) {
        contextSelection.insert(clickedClipId);
    }
    bool selectionHasVisual = false;
    bool selectionHasAudio = false;
    bool selectionHasCombined = false;
    for (const TimelineClip& clip : m_clips) {
        if (!contextSelection.contains(clip.id)) {
            continue;
        }
        const bool clipHasAudio = clip.hasAudio || clip.mediaType == ClipMediaType::Audio;
        const bool clipHasVideo = clipHasVisuals(clip);
        selectionHasVisual = selectionHasVisual || clipHasVideo;
        selectionHasAudio = selectionHasAudio || clipHasAudio;
        selectionHasCombined = selectionHasCombined || (clipHasVideo && clipHasAudio);
    }
    const bool canAutoSyncSelection = (selectionHasVisual && selectionHasAudio) || selectionHasCombined;

    if (clipIndex >= 0) {
        menu.addSeparator();
        if (!isClipSelected(clickedClipId)) {
            setSelectedClipId(clickedClipId);
        }
        syncAction = menu.addAction(QStringLiteral("Sync"));
        syncAction->setEnabled(canAutoSyncSelection);
        menu.addSeparator();
        const bool audioOnly = clipIsAudioOnly(m_clips[clipIndex]);
        nudgeLeftAction = menu.addAction(
            audioOnly ? QStringLiteral("Nudge -25ms\tAlt+Left") : QStringLiteral("Nudge -1 Frame\tAlt+Left"));
        nudgeRightAction = menu.addAction(
            audioOnly ? QStringLiteral("Nudge +25ms\tAlt+Right") : QStringLiteral("Nudge +1 Frame\tAlt+Right"));
        menu.addSeparator();
        splitClipAction = menu.addAction(QStringLiteral("Split Clip At Playhead\tCtrl+B"));
        {
            const auto& c = m_clips[clipIndex];
            splitClipAction->setEnabled(
                !c.locked &&
                m_currentFrame > c.startFrame &&
                m_currentFrame < c.startFrame + c.durationFrames);
        }
        menu.addSeparator();
        deleteAction = menu.addAction(QStringLiteral("Delete"));
        copyClipNameAction = menu.addAction(QStringLiteral("Copy Clip Name"));
        gradingAction = menu.addAction(QStringLiteral("Grading..."));
        resetGradingAction = menu.addAction(QStringLiteral("Reset Grading"));
        refreshMetadataAction = menu.addAction(QStringLiteral("Refresh"));
        auto *speedMenu = menu.addMenu(QStringLiteral("Playback Speed"));
        static const qreal kSpeeds[] = { 0.25, 0.5, 0.75, 1.0, 1.25, 1.5, 2.0, 3.0, 4.0 };
        for (qreal s : kSpeeds) {
            const QString label = (qAbs(s - 1.0) < 0.001)
                ? QStringLiteral("1x (Normal)")
                : QStringLiteral("%1x").arg(s, 0, 'g', 3);
            QAction *a = speedMenu->addAction(label);
            a->setCheckable(true);
            a->setChecked(qAbs(m_clips[clipIndex].playbackRate - s) < 0.001);
            a->setData(s);
        }
        speedMenu->setEnabled(!m_clips[clipIndex].locked);
        scaleToFillAction = menu.addAction(QStringLiteral("Scale to Fill Preview"));
        scaleToFillAction->setEnabled(
            clipHasVisuals(m_clips[clipIndex]) &&
            m_clips[clipIndex].mediaType != ClipMediaType::Title &&
            !m_clips[clipIndex].locked);
        transcribeAction = menu.addAction(QStringLiteral("Transcribe"));
        const QString detectedProxyPath = playbackProxyPathForClip(m_clips[clipIndex]);
        createProxyAction = menu.addAction(
            detectedProxyPath.isEmpty() ? QStringLiteral("Create Proxy...")
                                        : QStringLiteral("Recreate Proxy..."));
        createProxyAction->setEnabled(
            m_clips[clipIndex].mediaType == ClipMediaType::Video);
        if (!detectedProxyPath.isEmpty()) {
            deleteProxyAction = menu.addAction(QStringLiteral("Delete Proxy"));
            deleteProxyAction->setEnabled(
                m_clips[clipIndex].mediaType == ClipMediaType::Video);
        }
        propertiesAction = menu.addAction(QStringLiteral("Properties"));
        menu.addSeparator();
    }

    QAction* lockAction = nullptr;
    QAction* unlockAction = nullptr;
    if (clipIndex >= 0) {
        if (m_clips[clipIndex].locked) {
            unlockAction = menu.addAction(QStringLiteral("Unlock"));
        } else {
            lockAction = menu.addAction(QStringLiteral("Lock"));
        }
    }

    QAction* selected = menu.exec(event->globalPos());
    if (!selected) return;

    if (selected == addTitleClipAction) {
        const int64_t insertFrame = frameFromX(event->pos().x());
        const int track = trackIndexAt(event->pos());
        int targetTrack = track >= 0 ? track : (m_tracks.isEmpty() ? 0 : m_tracks.size());
        TimelineClip titleClip = createDefaultTitleClip(insertFrame, targetTrack);
        
        // Check for conflict with audio clips on this track
        if (wouldClipConflictWithTrack(titleClip, targetTrack)) {
            // Find the next available track that doesn't have a conflict
            bool foundNonConflictingTrack = false;
            for (int t = 0; t <= trackCount(); ++t) {
                if (!wouldClipConflictWithTrack(titleClip, t)) {
                    targetTrack = t;
                    foundNonConflictingTrack = true;
                    break;
                }
            }
            if (!foundNonConflictingTrack) {
                targetTrack = trackCount();
                insertTrackAt(targetTrack);
            }
            titleClip.trackIndex = targetTrack;
        }
        
        m_clips.push_back(titleClip);
        normalizeClipTiming(m_clips.last());
        normalizeTrackIndices();
        sortClips();
        setSelectedClipId(titleClip.id);
        if (clipsChanged) clipsChanged();
        update();
        return;
    }

    if (selected == setExportStartAction) {
        if (m_exportRanges.isEmpty()) {
            m_exportRanges = {ExportRangeSegment{0, totalFrames()}};
        }
        m_exportRanges.first().startFrame = qMin(m_currentFrame, m_exportRanges.first().endFrame);
        normalizeExportRanges();
        if (exportRangeChanged) {
            exportRangeChanged();
        }
        update();
        return;
    }

    if (selected == setExportEndAction) {
        if (m_exportRanges.isEmpty()) {
            m_exportRanges = {ExportRangeSegment{0, totalFrames()}};
        }
        m_exportRanges.last().endFrame = qMax(m_currentFrame, m_exportRanges.last().startFrame);
        normalizeExportRanges();
        if (exportRangeChanged) {
            exportRangeChanged();
        }
        update();
        return;
    }

    if (selected == splitExportRangeAction) {
        if (splitSegmentIndex < 0 || splitSegmentIndex >= m_exportRanges.size()) {
            return;
        }
        const ExportRangeSegment segment = m_exportRanges[splitSegmentIndex];
        if (m_currentFrame <= segment.startFrame || m_currentFrame > segment.endFrame) {
            return;
        }
        ExportRangeSegment left = segment;
        ExportRangeSegment right = segment;
        left.endFrame = m_currentFrame - 1;
        right.startFrame = m_currentFrame;
        m_exportRanges.removeAt(splitSegmentIndex);
        m_exportRanges.insert(splitSegmentIndex, right);
        m_exportRanges.insert(splitSegmentIndex, left);
        normalizeExportRanges();
        if (exportRangeChanged) {
            exportRangeChanged();
        }
        update();
        return;
    }

    if (selected == resetExportRangeAction) {
        m_exportRanges = {ExportRangeSegment{0, totalFrames()}};
        normalizeExportRanges();
        if (exportRangeChanged) {
            exportRangeChanged();
        }
        update();
        return;
    }

    if (selected == duplicateRenderFrameAction || selected == skipRenderFrameAction) {
        if (hasTargetClip) {
            openRenderSyncMarkerMenu(event->globalPos(), targetClipId);
        }
        return;
    }

    if (selected == clearRenderSyncAction) {
        clearRenderSyncMarkerAtCurrentFrame(targetClipId);
        return;
    }

    if (selected == nudgeLeftAction) {
        nudgeSelectedClip(-1);
        return;
    }

    if (selected == syncAction) {
        applyClipSelection(contextSelection, clickedClipId, false);
        if (selectionChanged) {
            selectionChanged();
        }
        if (syncRequested) {
            syncRequested(selectedClipIds());
        }
        return;
    }

    if (selected == nudgeRightAction) {
        nudgeSelectedClip(1);
        return;
    }

    if (selected == splitClipAction) {
        splitSelectedClipAtFrame(m_currentFrame);
        return;
    }

    if (selected == deleteAction) {
        if (clipIndex >= 0) {
            setSelectedClipId(m_clips[clipIndex].id);
            deleteSelectedClip();
        }
        return;
    }

    if (selected == copyClipNameAction) {
        if (clipIndex >= 0) {
            if (QClipboard* clipboard = QApplication::clipboard()) {
                clipboard->setText(m_clips[clipIndex].label);
            }
        }
        return;
    }

    if (selected == gradingAction) {
        if (gradingRequested) {
            gradingRequested();
        }
        return;
    }

    if (selected == resetGradingAction) {
        TimelineClip& clip = m_clips[clipIndex];
        clip.brightness = 0.0;
        clip.contrast = 1.0;
        clip.saturation = 1.0;
        clip.opacity = 1.0;
        clip.gradingKeyframes.clear();
        normalizeClipGradingKeyframes(clip);
        clip.opacityKeyframes.clear();
        normalizeClipOpacityKeyframes(clip);
        if (clipsChanged) clipsChanged();
        update();
        return;
    }

    if (selected && selected->data().isValid()) {
        bool ok = false;
        const qreal speed = selected->data().toDouble(&ok);
        if (ok && speed > 0.0 && clipIndex >= 0) {
            TimelineClip& clip = m_clips[clipIndex];
            const qreal oldRate = qBound<qreal>(0.001, clip.playbackRate, 100.0);
            const qreal newRate = qBound<qreal>(0.001, speed, 100.0);
            if (qAbs(oldRate - newRate) > 0.0001) {
                const int64_t oldDuration = clip.durationFrames;
                // Scale duration inversely with speed change: 2x speed = half duration
                const int64_t newDuration = qMax<int64_t>(1,
                    static_cast<int64_t>(std::round(
                        static_cast<qreal>(oldDuration) * oldRate / newRate)));
                const int64_t delta = oldDuration - newDuration;
                clip.playbackRate = newRate;
                clip.durationFrames = newDuration;
                normalizeClipTiming(clip);
                // Ripple: shift all later clips on the same track left to fill the gap
                if (delta != 0) {
                    const int64_t clipEnd = clip.startFrame + newDuration;
                    for (int i = 0; i < m_clips.size(); ++i) {
                        if (i == clipIndex) continue;
                        if (m_clips[i].trackIndex == clip.trackIndex &&
                            m_clips[i].startFrame >= clip.startFrame + oldDuration - 1) {
                            m_clips[i].startFrame -= delta;
                            normalizeClipTiming(m_clips[i]);
                        }
                    }
                }
                sortClips();
            }
            if (clipsChanged) clipsChanged();
            update();
            return;
        }
    }

    if (selected == scaleToFillAction) {
        if (scaleToFillRequested && clipIndex >= 0) {
            scaleToFillRequested(m_clips[clipIndex].id);
        }
        return;
    }

    if (selected == transcribeAction) {
        if (transcribeRequested) {
            const TimelineClip& clip = m_clips[clipIndex];
            transcribeRequested(clip.filePath, clip.label);
        }
        return;
    }

    if (selected == createProxyAction) {
        if (createProxyRequested && clipIndex >= 0) {
            createProxyRequested(m_clips[clipIndex].id);
        }
        return;
    }

    if (selected == deleteProxyAction) {
        if (deleteProxyRequested && clipIndex >= 0) {
            deleteProxyRequested(m_clips[clipIndex].id);
        }
        return;
    }

    if (selected == propertiesAction) {
        const TimelineClip& clip = m_clips[clipIndex];
        QMessageBox::information(this, QStringLiteral("Clip Properties"),
            QStringLiteral("Name: %1\nPath: %2\nProxy: %3\nType: %4\nSource Kind: %5\nStart: %6\nSource In: %7\nDuration: %8 frames\nAudio start offset: %9 ms\nBrightness: %10\nContrast: %11\nSaturation: %12\nOpacity: %13\nLocked: %14")
                .arg(clip.label)
                .arg(QDir::toNativeSeparators(clip.filePath))
                .arg(playbackProxyPathForClip(clip).isEmpty() ? QStringLiteral("None")
                                                               : QDir::toNativeSeparators(playbackProxyPathForClip(clip)))
                .arg(clipMediaTypeLabel(clip.mediaType))
                .arg(mediaSourceKindLabel(clip.sourceKind))
                .arg(timecodeForFrame(clip.startFrame))
                .arg(timecodeForFrame(clip.sourceInFrame))
                .arg(clip.durationFrames)
                .arg(qRound64((clip.startSubframeSamples * 1000.0) / kAudioSampleRate))
                .arg(clip.brightness, 0, 'f', 2)
                .arg(clip.contrast, 0, 'f', 2)
                .arg(clip.saturation, 0, 'f', 2)
                .arg(clip.opacity, 0, 'f', 2)
                .arg(clip.locked ? QStringLiteral("Yes") : QStringLiteral("No")));
        return;
    }

    if (selected == refreshMetadataAction) {
        QSet<QString> refreshIds = contextSelection;
        if (refreshIds.isEmpty() && clipIndex >= 0) {
            refreshIds.insert(m_clips[clipIndex].id);
        }
        if (refreshIds.isEmpty()) {
            return;
        }

        int refreshedCount = 0;
        int missingCount = 0;
        bool anyChanged = false;
        for (TimelineClip& clip : m_clips) {
            if (!refreshIds.contains(clip.id)) {
                continue;
            }

            const QFileInfo sourceInfo(clip.filePath);
            const bool sourceExists =
                sourceInfo.exists() &&
                (sourceInfo.isFile() || (sourceInfo.isDir() && isImageSequencePath(clip.filePath)));
            if (!sourceExists) {
                ++missingCount;
                continue;
            }

            const TimelineClip before = clip;
            const MediaProbeResult probe = probeMediaFile(
                clip.filePath,
                qMax<int64_t>(1, clip.sourceDurationFrames > 0 ? clip.sourceDurationFrames : clip.durationFrames));

            clip.mediaType = probe.mediaType;
            clip.sourceKind = probe.sourceKind;
            clip.hasAudio = probe.hasAudio;
            if (probe.fps > 0.001) {
                clip.sourceFps = probe.fps;
            }
            if (probe.durationFrames > 0) {
                clip.sourceDurationFrames = probe.durationFrames;
            }

            if (clip.sourceDurationFrames > 0) {
                clip.sourceInFrame = qBound<int64_t>(0, clip.sourceInFrame, clip.sourceDurationFrames - 1);
                const qreal sourceFps = clip.sourceFps > 0.001 ? clip.sourceFps : static_cast<qreal>(kTimelineFps);
                const int64_t availableSourceFrames =
                    qMax<int64_t>(1, clip.sourceDurationFrames - clip.sourceInFrame);
                const int64_t maxTimelineDuration = qMax<int64_t>(
                    1,
                    qRound64((static_cast<qreal>(availableSourceFrames) / sourceFps) *
                             static_cast<qreal>(kTimelineFps)));
                clip.durationFrames = qMin<int64_t>(clip.durationFrames, maxTimelineDuration);
            }

            const QString detectedProxyPath = playbackProxyPathForClip(clip);
            if (detectedProxyPath.isEmpty()) {
                clip.proxyPath.clear();
            } else {
                clip.proxyPath = detectedProxyPath;
            }

            normalizeClipTiming(clip);
            const bool changed =
                before.mediaType != clip.mediaType ||
                before.sourceKind != clip.sourceKind ||
                before.hasAudio != clip.hasAudio ||
                qAbs(before.sourceFps - clip.sourceFps) > 0.0001 ||
                before.sourceDurationFrames != clip.sourceDurationFrames ||
                before.sourceInFrame != clip.sourceInFrame ||
                before.durationFrames != clip.durationFrames ||
                before.proxyPath != clip.proxyPath;
            anyChanged = anyChanged || changed;
            ++refreshedCount;
        }

        if (anyChanged) {
            sortClips();
            if (clipsChanged) {
                clipsChanged();
            }
            update();
        }

        QMessageBox::information(
            this,
            QStringLiteral("Refresh Metadata"),
            QStringLiteral("Refreshed %1 clip(s).\nMissing source on disk: %2.")
                .arg(refreshedCount)
                .arg(missingCount));
        return;
    }

    if (selected == lockAction) {
        m_clips[clipIndex].locked = true;
        update();
        return;
    }

    if (selected == unlockAction) {
        m_clips[clipIndex].locked = false;
        update();
        return;
    }
}

bool TimelineWidget::handleWheelSteps(int steps,
                                      Qt::KeyboardModifiers modifiers,
                                      qreal cursorX,
                                      bool overTrackLabels) {
    if (steps == 0) {
        return false;
    }

    if (modifiers & Qt::AltModifier) {
        const qreal zoomFactor = steps > 0 ? 1.12 : (1.0 / 1.12);
        bool changed = false;
        for (TimelineTrack& track : m_tracks) {
            const int oldHeight = track.height;
            track.height = qBound(kMinTrackHeight,
                                  qRound(track.height * std::pow(zoomFactor, std::abs(steps))),
                                  240);
            changed = changed || track.height != oldHeight;
        }
        if (changed) {
            updateMinimumTimelineHeight();
            if (clipsChanged) {
                clipsChanged();
            }
            if (trackLayoutChanged) {
                trackLayoutChanged();
            }
            update();
        }
        return true;
    }

    if (modifiers & Qt::ShiftModifier) {
        const int visibleFrames = qMax(1, static_cast<int>(width() / m_pixelsPerFrame));
        const int panFrames = qMax(1, visibleFrames / 12);
        m_frameOffset = qMax<int64_t>(0, m_frameOffset - steps * panFrames);
        update();
        return true;
    }

    if ((modifiers & Qt::ControlModifier) || !overTrackLabels) {
        const qreal oldPixelsPerFrame = m_pixelsPerFrame;
        const qreal cursorFrame = frameFromX(cursorX);
        const qreal zoomFactor = steps > 0 ? 1.15 : (1.0 / 1.15);
        const QRect contentRect = timelineContentRect();
        const int64_t fullTimelineFrames = qMax<int64_t>(1, totalFrames());
        const qreal fitAllPixelsPerFrame =
            contentRect.width() > 0
                ? static_cast<qreal>(contentRect.width()) / static_cast<qreal>(fullTimelineFrames)
                : 0.01;
        const qreal minPixelsPerFrame = qMin<qreal>(0.25, fitAllPixelsPerFrame);
        m_pixelsPerFrame = qBound(minPixelsPerFrame, m_pixelsPerFrame * std::pow(zoomFactor, std::abs(steps)), 24.0);

        const qreal localX = cursorX - static_cast<qreal>(contentRect.left());
        if (m_pixelsPerFrame > 0.0) {
            const qreal newOffset = cursorFrame - qMax<qreal>(0.0, localX) / m_pixelsPerFrame;
            const int64_t visibleFrames = qMax<int64_t>(1, qRound(static_cast<qreal>(contentRect.width()) / m_pixelsPerFrame));
            const int64_t maxOffset = qMax<int64_t>(0, totalFrames() - visibleFrames);
            m_frameOffset = qBound<int64_t>(0, static_cast<int64_t>(qRound(newOffset)), maxOffset);
        }

        if (m_pixelsPerFrame <= fitAllPixelsPerFrame + 0.0001) {
            m_frameOffset = 0;
        }

        if (!qFuzzyCompare(oldPixelsPerFrame, m_pixelsPerFrame)) {
            update();
        }
        return true;
    }

    setVerticalScrollOffset(m_verticalScrollOffset - (steps * 36));
    return true;
}

void TimelineWidget::wheelEvent(QWheelEvent* event) {
    const QPoint numDegrees = event->angleDelta() / 8;
    if (numDegrees.isNull()) {
        QWidget::wheelEvent(event);
        return;
    }

    const int steps = numDegrees.y() / 15;
    if (steps == 0) {
        QWidget::wheelEvent(event);
        return;
    }

    const bool overTrackLabels = trackRect().contains(event->position().toPoint()) &&
                                 !timelineContentRect().contains(event->position().toPoint());
    if (handleWheelSteps(steps, event->modifiers(), event->position().x(), overTrackLabels)) {
        event->accept();
        return;
    }

    QWidget::wheelEvent(event);
}
