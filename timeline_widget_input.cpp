#include "timeline_widget.h"
#include "titles.h"

#include <QApplication>
#include <QClipboard>
#include <QDialog>
#include <QVBoxLayout>
#include <QLabel>
#include <QDoubleSpinBox>
#include <QCheckBox>
#include <QDialogButtonBox>

namespace {

bool exportRangesEqual(const QVector<ExportRangeSegment>& a, const QVector<ExportRangeSegment>& b) {
    if (a.size() != b.size()) {
        return false;
    }
    for (int i = 0; i < a.size(); ++i) {
        if (a[i].startFrame != b[i].startFrame || a[i].endFrame != b[i].endFrame) {
            return false;
        }
    }
    return true;
}

} // namespace

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
    bool exportStartHandle = false;
    if (exportHandleAtPos(pos, &exportStartHandle) >= 0) {
        setCursor(Qt::SizeHorCursor);
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
    TimelineClip probeClip = clip;
    probeClip.useProxy = true;
    return !playbackProxyPathForClip(probeClip).isEmpty();
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
    return probeMediaFile(info.absoluteFilePath(), guessDurationSeconds(suffix)).durationFrames;
}

bool TimelineWidget::hasFileUrls(const QMimeData* mimeData) const {
    if (!mimeData || !mimeData->hasUrls()) return false;
    for (const QUrl& url : mimeData->urls()) {
        if (url.isLocalFile()) return true;
    }
    return false;
}

qreal TimelineWidget::guessDurationSeconds(const QString& suffix) const {
    static const QHash<QString, qreal> kDurations = {
        {QStringLiteral("mp4"), 60.0},
        {QStringLiteral("mov"), 60.0},
        {QStringLiteral("mkv"), 60.0},
        {QStringLiteral("webm"), 60.0},
        {QStringLiteral("avi"), 60.0},
        {QStringLiteral("webc"), 60.0},
        {QStringLiteral("png"), 3.0},
        {QStringLiteral("jpg"), 3.0},
        {QStringLiteral("jpeg"), 3.0},
        {QStringLiteral("webp"), 3.0},
    };
    return kDurations.value(suffix, 60.0);
}

QColor TimelineWidget::colorForPath(const QString& path) const {
    const quint32 hash = qHash(path);
    return QColor::fromHsv(static_cast<int>(hash % 360), 160, 220, 220);
}

TimelineClip TimelineWidget::buildClipFromFile(const QString& filePath,
                                               int64_t startFrame,
                                               int trackIndex) const {
    const QFileInfo info(filePath);
    const MediaProbeResult probe = probeMediaFile(filePath, guessDurationSeconds(info.suffix().toLower()));
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
    refreshClipAudioSource(clip);
    normalizeClipTransformKeyframes(clip);
    normalizeClipGradingKeyframes(clip);
    normalizeClipOpacityKeyframes(clip);
    return clip;
}

void TimelineWidget::addClipFromFile(const QString& filePath, int64_t startFrame) {
    const QFileInfo info(filePath);
    if (!info.exists() || (!info.isFile() && !isImageSequencePath(filePath))) return;

    const QVector<ExportRangeSegment> beforeRanges = m_exportRanges;
    TimelineClip clip = buildClipFromFile(filePath,
                                          startFrame >= 0 ? startFrame : totalFrames(),
                                          nextTrackIndex());
    m_clips.push_back(clip);
    normalizeTrackIndices();
    sortClips();
    normalizeExportRange();
    if (exportRangeChanged && !exportRangesEqual(beforeRanges, m_exportRanges)) {
        exportRangeChanged();
    }

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

    const QVector<ExportRangeSegment> beforeRanges = m_exportRanges;
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
    normalizeExportRange();
    m_dropFrame = -1;
    m_trackDropIndex = -1;
    m_trackDropInGap = false;
    event->acceptProposedAction();

    if (exportRangeChanged && !exportRangesEqual(beforeRanges, m_exportRanges)) {
        exportRangeChanged();
    }
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
                auto *dialog = new QDialog(this);
                dialog->setWindowTitle(QStringLiteral("Crossfade Consecutive Clips"));
                auto *layout = new QVBoxLayout(dialog);
                layout->addWidget(new QLabel(QStringLiteral("Crossfade duration (seconds)")));
                auto *spinBox = new QDoubleSpinBox(dialog);
                spinBox->setRange(0.01, 30.00);
                spinBox->setValue(0.50);
                spinBox->setDecimals(2);
                layout->addWidget(spinBox);
                auto *checkBox = new QCheckBox(QStringLiteral("Move clips to overlap (uncheck to keep positions)"), dialog);
                checkBox->setChecked(false);
                layout->addWidget(checkBox);
                auto *buttonBox = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel);
                layout->addWidget(buttonBox);
                connect(buttonBox, &QDialogButtonBox::accepted, dialog, &QDialog::accept);
                connect(buttonBox, &QDialogButtonBox::rejected, dialog, &QDialog::reject);
                if (dialog->exec() == QDialog::Accepted) {
                    const double seconds = spinBox->value();
                    const bool moveClips = checkBox->isChecked();
                    applyCrossfadeToTrack(trackHit, seconds, moveClips);
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
            if (!m_exportRangeMouseGrabbed) {
                grabMouse();
                m_exportRangeMouseGrabbed = true;
            }
            setCursor(Qt::SizeHorCursor);
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
            const bool clickedWasSelected = isClipSelected(clickedClipId);
            if (!clickedWasSelected) {
                setSelectedClipId(clickedClipId);
            }
            m_dragMode = clipDragModeAt(m_clips[hitIndex], event->position().toPoint());
            m_draggedClipIndex = hitIndex;
            m_dragOriginalStartFrame = m_clips[hitIndex].startFrame;
            m_dragOriginalDurationFrames = m_clips[hitIndex].durationFrames;
            m_dragOriginalSourceInFrame = m_clips[hitIndex].sourceInFrame;
            m_dragOriginalTransformKeyframes = m_clips[hitIndex].transformKeyframes;
            m_dragOriginalTitleKeyframes = m_clips[hitIndex].titleKeyframes;
            m_dragOffsetFrames = frameFromX(event->position().x()) - m_clips[hitIndex].startFrame;
            m_dragMoveClipIds.clear();
            m_dragMoveOriginalStartFrames.clear();
            m_dragLockedTrackOnly = false;
            if (m_clips[hitIndex].locked) {
                // Locked clips may move between tracks, but not in time.
                m_dragMode = ClipDragMode::Move;
                m_dragLockedTrackOnly = true;
            }
            if (m_dragMode == ClipDragMode::Move) {
                QSet<QString> movingIds = selectedClipIds();
                if (movingIds.isEmpty()) {
                    movingIds.insert(clickedClipId);
                } else if (!movingIds.contains(clickedClipId)) {
                    movingIds = QSet<QString>{clickedClipId};
                }
                QSet<QString> unlockedMovingIds;
                unlockedMovingIds.reserve(movingIds.size());
                for (const QString& candidateId : movingIds) {
                    for (const TimelineClip& candidateClip : m_clips) {
                        if (candidateClip.id == candidateId && !candidateClip.locked) {
                            unlockedMovingIds.insert(candidateId);
                            break;
                        }
                    }
                }
                movingIds = unlockedMovingIds;
                if (movingIds.isEmpty() || m_dragLockedTrackOnly) {
                    movingIds = QSet<QString>{clickedClipId};
                }
                m_dragMoveClipIds = movingIds;
                for (const TimelineClip& clip : m_clips) {
                    if (m_dragMoveClipIds.contains(clip.id)) {
                        m_dragMoveOriginalStartFrames.insert(clip.id, clip.startFrame);
                    }
                }
            }
            update();
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
        const bool proxyVideoAvailable = clipHasProxyAvailable(hoveredClip);
        const bool proxyAudioAvailable = playbackUsesAlternateAudioSource(hoveredClip);
        const QString transcriptPath = transcriptWorkingPathForClipFile(hoveredClip.filePath);
        const bool transcriptAvailable =
            !transcriptPath.isEmpty() && QFileInfo::exists(transcriptPath);
        const int64_t localTimelineFrame =
            qBound<int64_t>(0,
                            m_currentFrame - hoveredClip.startFrame,
                            qMax<int64_t>(0, hoveredClip.durationFrames - 1));
        const int64_t clipFrame =
            adjustedClipLocalFrameAtTimelineFrame(hoveredClip, localTimelineFrame, m_renderSyncMarkers);
        setToolTip(QStringLiteral("%1\n%2\nFrame %3\nProxy Video: %4\nProxy Audio: %5\nTranscript: %6")
                       .arg(hoveredClip.label, typeLabel)
                       .arg(clipFrame)
                       .arg(proxyVideoAvailable ? QStringLiteral("Yes") : QStringLiteral("No"))
                       .arg(proxyAudioAvailable ? QStringLiteral("Yes") : QStringLiteral("No"))
                       .arg(transcriptAvailable ? QStringLiteral("Yes") : QStringLiteral("No")));
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
            if (m_exportRangeMouseGrabbed) {
                releaseMouse();
                m_exportRangeMouseGrabbed = false;
            }
            updateHoverCursor(event->position().toPoint());
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
        const QRect contentRect = timelineContentRect();
        constexpr int kAutoScrollEdgePixels = 24;
        const qreal stablePixelsPerFrame = qMax<qreal>(0.25, m_pixelsPerFrame);
        if (!contentRect.isEmpty()) {
            int64_t autoScrollFrames = 0;
            const qreal leftEdge = static_cast<qreal>(contentRect.left() + kAutoScrollEdgePixels);
            const qreal rightEdge = static_cast<qreal>(contentRect.right() - kAutoScrollEdgePixels);
            if (event->position().x() < leftEdge) {
                const qreal pixelsBeyond = leftEdge - event->position().x();
                autoScrollFrames = -qMax<int64_t>(1, qRound64(pixelsBeyond / stablePixelsPerFrame));
            } else if (event->position().x() > rightEdge) {
                const qreal pixelsBeyond = event->position().x() - rightEdge;
                autoScrollFrames = qMax<int64_t>(1, qRound64(pixelsBeyond / stablePixelsPerFrame));
            }
            if (autoScrollFrames != 0) {
                const int64_t visibleFrames =
                    qMax<int64_t>(1, qRound64(static_cast<qreal>(contentRect.width()) / stablePixelsPerFrame));
                const int64_t currentMaxOffset = qMax<int64_t>(0, totalFrames() - visibleFrames);
                const int64_t expandedMaxOffset = qMax<int64_t>(currentMaxOffset, m_frameOffset + autoScrollFrames);
                m_frameOffset = qBound<int64_t>(0, m_frameOffset + autoScrollFrames, expandedMaxOffset);
            }
        }
        const int64_t pointerFrame = frameFromX(event->position().x());
        static constexpr int64_t kMinClipFrames = 1;
        const bool isImage = clip.mediaType == ClipMediaType::Image;
        const bool isTitle = clip.mediaType == ClipMediaType::Title;
        const bool isElasticDuration = isImage || isTitle;
        m_snapIndicatorFrame = -1;
        if (m_dragMode == ClipDragMode::Move) {
            if (m_dragLockedTrackOnly) {
                bool insertsTrack = false;
                const int proposedTrack = trackDropTargetAtY(event->position().toPoint().y(), &insertsTrack);
                if (proposedTrack >= 0 && proposedTrack != clip.trackIndex) {
                    TimelineClip tempClip = clip;
                    tempClip.trackIndex = proposedTrack;
                    if (!wouldClipConflictWithTrack(tempClip, proposedTrack, clip.id)) {
                        clip.trackIndex = proposedTrack;
                    }
                }
                m_trackDropIndex = proposedTrack;
                m_trackDropInGap = insertsTrack;
                update();
                return;
            }
            bool snapped = false;
            int64_t snappedBoundaryFrame = -1;
            const int64_t unsnappedStartFrame = qMax<int64_t>(0, pointerFrame - m_dragOffsetFrames);
            const int64_t newStartFrame =
                snapMoveStartFrame(clip, unsnappedStartFrame, &snapped, &snappedBoundaryFrame);
            int64_t moveDelta = newStartFrame - m_dragOriginalStartFrame;
            int64_t minOriginalStartFrame = std::numeric_limits<int64_t>::max();
            for (auto it = m_dragMoveOriginalStartFrames.constBegin(); it != m_dragMoveOriginalStartFrames.constEnd(); ++it) {
                minOriginalStartFrame = qMin(minOriginalStartFrame, it.value());
            }
            if (minOriginalStartFrame != std::numeric_limits<int64_t>::max()) {
                moveDelta = qMax<int64_t>(moveDelta, -minOriginalStartFrame);
            }
            bool insertsTrack = false;
            const int proposedTrack = trackDropTargetAtY(event->position().toPoint().y(), &insertsTrack);

            const bool movingMultipleClips = m_dragMoveOriginalStartFrames.size() > 1;
            if (movingMultipleClips) {
                for (TimelineClip& movingClip : m_clips) {
                    const auto it = m_dragMoveOriginalStartFrames.constFind(movingClip.id);
                    if (it == m_dragMoveOriginalStartFrames.constEnd()) {
                        continue;
                    }
                    movingClip.startFrame = qMax<int64_t>(0, it.value() + moveDelta);
                }
            } else {
                clip.startFrame = qMax<int64_t>(0, m_dragOriginalStartFrame + moveDelta);
            }
            m_snapIndicatorFrame = snapped ? snappedBoundaryFrame : -1;
            m_currentFrame = qMax<int64_t>(0, m_dragOriginalStartFrame + moveDelta);

            if (!movingMultipleClips) {
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
            } else {
                m_trackDropIndex = clip.trackIndex;
                m_trackDropInGap = false;
            }
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
            } else if (isTitle) {
                clip.sourceInFrame = 0;
                clip.durationFrames = m_dragOriginalDurationFrames + (m_dragOriginalStartFrame - newStartFrame);
                clip.sourceDurationFrames = clip.durationFrames;
                clip.titleKeyframes.clear();
                for (const TimelineClip::TitleKeyframe& keyframe : m_dragOriginalTitleKeyframes) {
                    if (keyframe.frame < trimDelta) {
                        continue;
                    }
                    TimelineClip::TitleKeyframe shifted = keyframe;
                    shifted.frame -= trimDelta;
                    clip.titleKeyframes.push_back(shifted);
                }
                normalizeClipTitleKeyframes(clip);
            } else {
                const qreal playbackRate = qBound<qreal>(0.001, clip.playbackRate, 4.0);
                const qreal sourceFps = clip.sourceFps > 0.001 ? clip.sourceFps : static_cast<qreal>(kTimelineFps);
                const int64_t consumedSourceFrames =
                    static_cast<int64_t>(std::floor(static_cast<qreal>(trimDelta) * playbackRate * sourceFps / static_cast<qreal>(kTimelineFps)));
                // Prevent extending clip before the beginning of the media
                // Clamp sourceInFrame to not go below 0
                clip.sourceInFrame = qBound<int64_t>(0, m_dragOriginalSourceInFrame + consumedSourceFrames, clip.sourceDurationFrames - 1);
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
            const int64_t maxEndFrame = isElasticDuration
                ? std::numeric_limits<int64_t>::max()
                : m_dragOriginalStartFrame +
                      qMax<int64_t>(kMinClipFrames,
                                    static_cast<int64_t>(std::ceil(
                                        static_cast<qreal>(mediaDurationFrames(QFileInfo(clip.filePath)) - m_dragOriginalSourceInFrame) /
                                        qBound<qreal>(0.001, clip.playbackRate, 4.0))));
            bool snapped = false;
            int64_t snappedBoundaryFrame = -1;
            const int64_t boundedPointerFrame = isElasticDuration
                ? qMax<int64_t>(minEndFrame, pointerFrame)
                : qBound<int64_t>(minEndFrame, pointerFrame, maxEndFrame);
            const int64_t newEndFrame = isElasticDuration
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
            } else if (isTitle) {
                clip.sourceDurationFrames = clip.durationFrames;
                normalizeClipTitleKeyframes(clip);
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
        if (m_exportRangeMouseGrabbed) {
            releaseMouse();
            m_exportRangeMouseGrabbed = false;
        }
        updateHoverCursor(event->position().toPoint());
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
            removeEmptyTracks();
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
        removeEmptyTracks();
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
        m_dragOriginalTitleKeyframes.clear();
        m_dragMoveClipIds.clear();
        m_dragMoveOriginalStartFrames.clear();
        m_dragLockedTrackOnly = false;
        if (clipsChanged) clipsChanged();
        updateHoverCursor(event->position().toPoint());
        update();
        return;
    }
    QWidget::mouseReleaseEvent(event);
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
