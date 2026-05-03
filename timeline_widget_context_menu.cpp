#include "timeline_widget.h"
#include "transcript_engine.h"
#include "titles.h"

#include <QApplication>
#include <QClipboard>
#include <QJsonArray>

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
    QAction* addTitleClipAction = menu.addAction(QStringLiteral("Create Title"));

    QAction* syncAction = nullptr;
    QAction* nudgeLeftAction = nullptr;
    QAction* nudgeRightAction = nullptr;
    QAction* splitClipAction = nullptr;
    QAction* deleteAction = nullptr;
    QAction* copyTitleAction = nullptr;
    QAction* copyClipNameAction = nullptr;
    QAction* gradingAction = nullptr;
    QAction* resetGradingAction = nullptr;
    QAction* scaleToFillAction = nullptr;
    QAction* propertiesAction = nullptr;
    QAction* refreshMetadataAction = nullptr;
    QAction* transcribeAction = nullptr;
    QAction* deleteTranscriptAction = nullptr;
    QAction* useProxyAction = nullptr;
    QAction* createProxyAction = nullptr;
    QAction* continueProxyAction = nullptr;
    QAction* deleteProxyAction = nullptr;
    QAction* generateBoxStreamAction = nullptr;
    QAction* deleteBoxStreamAction = nullptr;

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
        copyTitleAction = menu.addAction(QStringLiteral("Copy title"));
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
        QMenu* transcriptMenu = menu.addMenu(QStringLiteral("Transcript"));
        transcribeAction = transcriptMenu->addAction(QStringLiteral("Transcribe"));
        deleteTranscriptAction = transcriptMenu->addAction(QStringLiteral("Delete Transcript..."));
        bool transcriptExists = false;
        const QStringList transcriptPaths = transcriptCutPathsForClipFile(m_clips[clipIndex].filePath);
        for (const QString& path : transcriptPaths) {
            if (QFileInfo::exists(path)) {
                transcriptExists = true;
                break;
            }
        }
        deleteTranscriptAction->setEnabled(transcriptExists);
        TimelineClip proxyDetectionClip = m_clips[clipIndex];
        proxyDetectionClip.useProxy = true;
        const QString detectedProxyPath = !m_clips[clipIndex].proxyPath.isEmpty()
                                              ? m_clips[clipIndex].proxyPath
                                              : playbackProxyPathForClip(proxyDetectionClip);
        const bool canProxy = m_clips[clipIndex].mediaType == ClipMediaType::Video;
        QMenu* proxyMenu = menu.addMenu(QStringLiteral("Proxy"));
        useProxyAction = proxyMenu->addAction(QStringLiteral("Use Proxy"));
        useProxyAction->setCheckable(true);
        useProxyAction->setChecked(m_clips[clipIndex].useProxy);
        useProxyAction->setEnabled(canProxy);
        proxyMenu->addSeparator();
        createProxyAction = proxyMenu->addAction(
            detectedProxyPath.isEmpty() ? QStringLiteral("Create Proxy...")
                                        : QStringLiteral("Recreate Proxy..."));
        createProxyAction->setEnabled(canProxy);
        const QFileInfo proxyInfo(detectedProxyPath);
        continueProxyAction = proxyMenu->addAction(QStringLiteral("Continue Proxy Gen"));
        // Avoid synchronous directory scans when opening the context menu.
        // Large proxy/image-sequence directories can contain tens of thousands
        // of files and entryList() blocks the UI while enumerating all names.
        continueProxyAction->setEnabled(canProxy && proxyInfo.isDir());
        if (!detectedProxyPath.isEmpty()) {
            deleteProxyAction = proxyMenu->addAction(QStringLiteral("Delete Proxy"));
            deleteProxyAction->setEnabled(canProxy);
        }
        const bool canBoxStream =
            m_clips[clipIndex].mediaType == ClipMediaType::Audio || m_clips[clipIndex].hasAudio;
        bool hasBoxStream = false;
        const QString transcriptPath = activeTranscriptPathForClipFile(m_clips[clipIndex].filePath);
        if (!transcriptPath.trimmed().isEmpty()) {
            editor::TranscriptEngine transcriptEngine;
            QJsonObject artifactRoot;
            if (transcriptEngine.loadBoxstreamArtifact(transcriptPath, &artifactRoot)) {
                const QJsonObject byClip =
                    artifactRoot.value(QStringLiteral("continuity_boxstreams_by_clip")).toObject();
                const QJsonObject continuityRoot =
                    byClip.value(m_clips[clipIndex].id.trimmed()).toObject();
                hasBoxStream =
                    !continuityRoot.value(QStringLiteral("streams")).toArray().isEmpty();
            }
        }
        QMenu* boxStreamMenu = menu.addMenu(QStringLiteral("BoxStream"));
        generateBoxStreamAction = boxStreamMenu->addAction(QStringLiteral("Generate BoxStream..."));
        generateBoxStreamAction->setEnabled(canBoxStream);
        deleteBoxStreamAction = boxStreamMenu->addAction(QStringLiteral("Delete BoxStream..."));
        deleteBoxStreamAction->setEnabled(canBoxStream && hasBoxStream);
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
        auto isTitleTrack = [this](int trackIndex) {
            if (trackIndex < 0 || trackIndex >= m_tracks.size()) {
                return false;
            }
            return m_tracks[trackIndex].name.trimmed().startsWith(
                QStringLiteral("Titles"), Qt::CaseInsensitive);
        };
        auto nextTitleTrackName = [this]() {
            int titleTrackCount = 0;
            for (const TimelineTrack& track : m_tracks) {
                if (track.name.trimmed().startsWith(QStringLiteral("Titles"), Qt::CaseInsensitive)) {
                    ++titleTrackCount;
                }
            }
            return titleTrackCount <= 0
                ? QStringLiteral("Titles")
                : QStringLiteral("Titles %1").arg(titleTrackCount + 1);
        };

        int preferredTitleTrack = -1;
        for (int i = 0; i < m_tracks.size(); ++i) {
            if (m_tracks[i].name.trimmed().compare(QStringLiteral("Titles"), Qt::CaseInsensitive) == 0) {
                preferredTitleTrack = i;
                break;
            }
        }
        if (preferredTitleTrack < 0) {
            preferredTitleTrack = m_tracks.size();
            insertTrackAt(preferredTitleTrack);
            m_tracks[preferredTitleTrack].name = QStringLiteral("Titles");
            m_tracks[preferredTitleTrack].audioEnabled = false;
        }

        int targetTrack = preferredTitleTrack;
        TimelineClip titleClip = createDefaultTitleClip(insertFrame, targetTrack);

        if (wouldClipConflictWithTrack(titleClip, targetTrack)) {
            bool placedOnTitleTrack = false;
            for (int t = 0; t < m_tracks.size(); ++t) {
                if (!isTitleTrack(t)) {
                    continue;
                }
                if (!wouldClipConflictWithTrack(titleClip, t)) {
                    targetTrack = t;
                    placedOnTitleTrack = true;
                    break;
                }
            }
            if (!placedOnTitleTrack) {
                targetTrack = trackCount();
                insertTrackAt(targetTrack);
                m_tracks[targetTrack].name = nextTitleTrackName();
                m_tracks[targetTrack].audioEnabled = false;
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

    if (selected == copyTitleAction) {
        if (clipIndex >= 0) {
            if (QClipboard* clipboard = QApplication::clipboard()) {
                clipboard->setText(m_clips[clipIndex].label);
            }
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

    if (selected == deleteTranscriptAction) {
        if (deleteTranscriptRequested && clipIndex >= 0) {
            const TimelineClip& clip = m_clips[clipIndex];
            const auto confirmation = QMessageBox::warning(
                this,
                QStringLiteral("Delete Transcript"),
                QStringLiteral("This will delete the transcript and all Cuts for this clip.\n\nContinue?"),
                QMessageBox::Yes | QMessageBox::Cancel,
                QMessageBox::Cancel);
            if (confirmation == QMessageBox::Yes) {
                deleteTranscriptRequested(clip.filePath);
            }
        }
        return;
    }

    if (selected == useProxyAction) {
        if (clipIndex >= 0) {
            m_clips[clipIndex].useProxy = useProxyAction->isChecked();
            if (clipsChanged) {
                clipsChanged();
            }
            update();
        }
        return;
    }

    if (selected == createProxyAction) {
        if (createProxyRequested && clipIndex >= 0) {
            createProxyRequested(m_clips[clipIndex].id);
        }
        return;
    }

    if (selected == continueProxyAction) {
        if (continueProxyRequested && clipIndex >= 0) {
            continueProxyRequested(m_clips[clipIndex].id);
        }
        return;
    }

    if (selected == deleteProxyAction) {
        if (deleteProxyRequested && clipIndex >= 0) {
            deleteProxyRequested(m_clips[clipIndex].id);
        }
        return;
    }

    if (selected == generateBoxStreamAction) {
        if (generateBoxStreamRequested && clipIndex >= 0) {
            generateBoxStreamRequested(m_clips[clipIndex].id);
        }
        return;
    }

    if (selected == deleteBoxStreamAction) {
        if (deleteBoxStreamRequested && clipIndex >= 0) {
            deleteBoxStreamRequested(m_clips[clipIndex].id);
        }
        return;
    }

    if (selected == propertiesAction) {
        const TimelineClip& clip = m_clips[clipIndex];
        TimelineClip proxyDisplayClip = clip;
        proxyDisplayClip.useProxy = true;
        const QString displayProxyPath = !clip.proxyPath.isEmpty()
                                             ? clip.proxyPath
                                             : playbackProxyPathForClip(proxyDisplayClip);
        QMessageBox::information(this, QStringLiteral("Clip Properties"),
            QStringLiteral("Name: %1\nPath: %2\nProxy: %3\nUse Proxy: %4\nType: %5\nSource Kind: %6\nStart: %7\nSource In: %8\nDuration: %9 frames\nAudio start offset: %10 ms\nBrightness: %11\nContrast: %12\nSaturation: %13\nOpacity: %14\nLocked: %15")
                .arg(clip.label)
                .arg(QDir::toNativeSeparators(clip.filePath))
                .arg(displayProxyPath.isEmpty() ? QStringLiteral("None")
                                                : QDir::toNativeSeparators(displayProxyPath))
                .arg(clip.useProxy ? QStringLiteral("Yes") : QStringLiteral("No"))
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
        auto timelineFramesFromSourceFrames = [](int64_t sourceFrames, qreal sourceFps) {
            const qreal resolvedSourceFps =
                sourceFps > 0.001 ? sourceFps : static_cast<qreal>(kTimelineFps);
            return qMax<int64_t>(
                1,
                qRound64((static_cast<qreal>(qMax<int64_t>(1, sourceFrames)) / resolvedSourceFps) *
                         static_cast<qreal>(kTimelineFps)));
        };
        auto clipLooksLikeFullSourceDuration = [&timelineFramesFromSourceFrames](const TimelineClip& clip) {
            if (clip.sourceInFrame != 0 || clip.sourceDurationFrames <= 0 || clip.durationFrames <= 0) {
                return false;
            }
            const int64_t expectedDuration =
                timelineFramesFromSourceFrames(clip.sourceDurationFrames, clip.sourceFps);
            return qAbs(clip.durationFrames - expectedDuration) <= 1;
        };

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
            const bool lookedLikeFullSourceDuration = clipLooksLikeFullSourceDuration(clip);
            const MediaProbeResult probe = probeMediaFile(
                clip.filePath,
                4.0);

            clip.mediaType = probe.mediaType;
            clip.sourceKind = probe.sourceKind;
            clip.hasAudio = probe.hasAudio;
            if (probe.fps > 0.001) {
                clip.sourceFps = probe.fps;
            }
            if (probe.durationFrames > 0) {
                clip.sourceDurationFrames = probe.durationFrames;
            }
            if (lookedLikeFullSourceDuration && clip.sourceDurationFrames > 0) {
                clip.durationFrames =
                    timelineFramesFromSourceFrames(clip.sourceDurationFrames, clip.sourceFps);
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

            TimelineClip proxyDetectionClip = clip;
            proxyDetectionClip.useProxy = true;
            const QString detectedProxyPath = playbackProxyPathForClip(proxyDetectionClip);
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
