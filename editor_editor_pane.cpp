#include "editor.h"
#include "editor_preview_edit_helpers.h"
#include "timeline_fps.h"
#include "transport_icons.h"

#include <QFile>
#include <QFileInfo>
#include <QDebug>
#include <QElapsedTimer>
#include <QMessageBox>
#include <QSignalBlocker>
#include <QToolButton>

using namespace editor;

namespace {
void refreshPreviewTimeline(PreviewSurface* preview,
                            TimelineWidget* timeline,
                            const QVector<TimelineClip>& clips) {
    if (!preview || !timeline) {
        return;
    }
    preview->setTimelineTracks(timeline->tracks());
    preview->setTimelineClips(clips);
    preview->setRenderSyncMarkers(timeline->renderSyncMarkers());
}

}

void EditorWindow::bindEditorPaneWidgets(EditorPane *pane)
{
    m_editorPane = pane;
    m_preview = pane->previewWindow();
    m_timeline = pane->timelineWidget();
    m_playButton = pane->playButton();
    m_seekSlider = pane->seekSlider();
    m_timecodeLabel = pane->timecodeLabel();
    if (m_timecodeLabel) {
        m_timecodeLabel->setCursor(Qt::PointingHandCursor);
        m_timecodeLabel->setToolTip(QStringLiteral("Click to copy current frame number"));
        m_timecodeLabel->installEventFilter(this);
    }
    m_playbackSpeedCombo = pane->playbackSpeedCombo();
    m_previewModeCombo = pane->previewModeCombo();
    m_playbackLoopButton = pane->playbackLoopButton();
    m_audioToolsButton = pane->audioToolsButton();
    m_audioMuteButton = pane->audioMuteButton();
    m_audioVolumeSlider = pane->audioVolumeSlider();
    m_audioNowPlayingLabel = pane->audioNowPlayingLabel();
    m_statusBadge = pane->statusBadge();
    m_previewInfo = pane->previewInfo();
}

void EditorWindow::connectTransportControls(EditorPane *pane)
{
    connect(pane, &EditorPane::playClicked, this, [this]() { togglePlayback(); });
    connect(pane, &EditorPane::startClicked, this, [this]() { setCurrentFrame(0); });
    connect(pane, &EditorPane::prevFrameClicked, this, [this]() {
        if (!m_timeline) return;
        setCurrentFrame(stepBackwardFrame(m_timeline->currentFrame()));
    });
    connect(pane, &EditorPane::nextFrameClicked, this, [this]() {
        if (!m_timeline) return;
        setCurrentFrame(stepForwardFrame(m_timeline->currentFrame()));
    });
    connect(pane, &EditorPane::endClicked, this, [this]() {
        setCurrentFrame(m_timeline ? m_timeline->totalFrames() : 0);
    });
    connect(pane, &EditorPane::seekValueChanged, this, [this](int value) {
        if (m_ignoreSeekSignal) return;
        setCurrentFrame(value);
    });
    connect(pane, &EditorPane::playbackSpeedChanged, this, [this](double speed) {
        setPlaybackSpeed(speed);
    });
    connect(pane, &EditorPane::playbackLoopToggled, this, [this](bool enabled) {
        PlaybackRuntimeConfig config = playbackRuntimeConfig();
        config.loopEnabled = enabled;
        applyPlaybackRuntimeConfig(config);
    });
    connect(pane, &EditorPane::previewModeChanged, this, [this](const QString& mode) {
        applyPreviewViewMode(mode);
    });
    connect(pane, &EditorPane::zoomFitClicked, this, [this]() {
        if (!m_preview) {
            return;
        }
        m_preview->setPreviewZoom(1.0);
        m_preview->resetPreviewPan();
        if (m_previewZoomSpin) {
            const QSignalBlocker block(m_previewZoomSpin);
            m_previewZoomSpin->setValue(1.0);
        }
        scheduleSaveState();
    });
    connect(pane, &EditorPane::audioToolsClicked, this, [this]() {
        if (m_inspectorTabs) {
            for (int i = 0; i < m_inspectorTabs->count(); ++i) {
                if (m_inspectorTabs->tabText(i).compare(QStringLiteral("Audio"), Qt::CaseInsensitive) == 0) {
                    m_inspectorTabs->setCurrentIndex(i);
                    return;
                }
            }
        }
        openAudioToolsDialog();
    });
    connect(pane, &EditorPane::audioMuteClicked, this, [this]() {
        const bool nextMuted = !m_preview->audioMuted();
        m_preview->setAudioMuted(nextMuted);
        if (m_audioEngine) m_audioEngine->setMuted(nextMuted);
        m_inspectorPane->refreshTab(QStringLiteral("Audio"));
        scheduleSaveState();
    });
    connect(pane, &EditorPane::audioVolumeChanged, this, [this](int value) {
        m_preview->setAudioVolume(value / 100.0);
        if (m_audioEngine) m_audioEngine->setVolume(value / 100.0);
        m_inspectorPane->refreshTab(QStringLiteral("Audio"));
    });
    connect(pane->razorButton(), &QToolButton::toggled, this, [this](bool checked) {
        if (m_timeline)
            m_timeline->setToolMode(checked ? TimelineWidget::ToolMode::Razor
                                            : TimelineWidget::ToolMode::Select);
    });

}

void EditorWindow::connectTimelineSignals()
{
    m_timeline->seekRequested = [this](int64_t frame) { setCurrentFrame(frame); };
    m_timeline->clipsChanged = [this]() {
        syncSliderRange();
        m_preview->beginBulkUpdate();
        m_preview->setClipCount(m_timeline->clips().size());
        m_preview->setTimelineTracks(m_timeline->tracks());
        m_preview->setTimelineClips(m_timeline->clips());
        m_preview->setExportRanges(effectivePlaybackRanges());
        m_preview->setRenderSyncMarkers(m_timeline->renderSyncMarkers());
        m_preview->setSelectedClipId(m_timeline->selectedClipId());
        m_preview->endBulkUpdate();
        if (m_audioEngine) {
            m_audioEngine->setTimelineTracks(m_timeline->tracks());
            m_audioEngine->setTimelineClips(m_timeline->clips());
            m_audioEngine->setExportRanges(effectivePlaybackRanges());
            m_audioEngine->setTranscriptNormalizeRanges(
                m_previewAudioDynamics.transcriptNormalizeEnabled
                    ? effectiveTranscriptNormalizeRanges()
                    : QVector<ExportRangeSegment>{});
            m_audioEngine->setRenderSyncMarkers(m_timeline->renderSyncMarkers());
            m_audioEngine->setSpeechFilterFadeSamples(m_speechFilterFadeSamples);
            m_audioEngine->setSpeechFilterRangeCrossfadeEnabled(m_speechFilterRangeCrossfade);
            m_audioEngine->setPlaybackWarpMode(m_playbackAudioWarpMode);
            m_audioEngine->setPlaybackRate(effectiveAudioWarpRate());
            m_audioEngine->setTranscriptNormalizeEnabled(
                m_previewAudioDynamics.transcriptNormalizeEnabled);
            m_audioEngine->setAudioDynamicsSettings(m_previewAudioDynamics);
        }
        refreshClipInspector();
        refreshTimelineStructureInspectorViews();
        scheduleSaveState();
        pushHistorySnapshot();
    };
    m_timeline->selectionChanged = [this]() {
        const TimelineClip* selectedClip = m_timeline ? m_timeline->selectedClip() : nullptr;
        const QString selectedClipId = selectedClip ? selectedClip->id : QString();
        const bool selectionChanged = selectedClipId != m_lastAutoTranscriptSwitchClipId;
        m_lastAutoTranscriptSwitchClipId = selectedClipId;

        m_preview->setSelectedClipId(m_timeline->selectedClipId());
        refreshSyncInspector();
        m_transcriptTab->refresh();
        refreshClipInspector();
        if (!m_loadingState && selectionChanged && selectedClip && !selectedClip->filePath.isEmpty()) {
            const QString transcriptPath = transcriptWorkingPathForClip(*selectedClip);
            if (QFileInfo::exists(transcriptPath) && m_inspectorTabs) {
                for (int i = 0; i < m_inspectorTabs->count(); ++i) {
                    if (m_inspectorTabs->tabText(i) == QStringLiteral("Transcript")) {
                        m_inspectorTabs->setCurrentIndex(i);
                        break;
                    }
                }
            }
        }
        refreshTimelineSelectionInspectorViews();
    };
    m_timeline->renderSyncMarkersChanged = [this]() {
        m_preview->setRenderSyncMarkers(m_timeline->renderSyncMarkers());
        if (m_audioEngine) {
            m_audioEngine->setRenderSyncMarkers(m_timeline->renderSyncMarkers());
        }
        refreshSyncInspector();
        m_inspectorPane->refreshTab(QStringLiteral("Sync"));
        scheduleSaveState();
        pushHistorySnapshot();
    };
    m_timeline->exportRangeChanged = [this]() {
        m_outputTab->refresh();
        m_inspectorPane->refreshTab(QStringLiteral("Output"));
        scheduleSaveState();
        pushHistorySnapshot();
    };
    m_timeline->gradingRequested = [this]() {
        focusGradingTab();
        m_inspectorPane->refreshTab(QStringLiteral("Grade"));
    };
    m_timeline->transcribeRequested = [this](const QString &filePath, const QString &label) {
        openTranscriptionWindow(filePath, label);
    };
    m_timeline->deleteTranscriptRequested = [this](const QString& filePath) {
        if (filePath.isEmpty()) {
            return;
        }
        const QStringList transcriptPaths = transcriptCutPathsForClipFile(filePath);
        int deletedCount = 0;
        int failedCount = 0;
        for (const QString& path : transcriptPaths) {
            if (!QFileInfo::exists(path)) {
                continue;
            }
            if (QFile::remove(path)) {
                ++deletedCount;
            } else {
                ++failedCount;
            }
        }
        clearActiveTranscriptPathForClipFile(filePath);
        m_transcriptEngine.invalidateCache();
        invalidatePlaybackRangeCaches();
        if (m_preview) {
            m_preview->invalidateTranscriptOverlayCache(filePath);
        }
        if (m_transcriptTab) {
            m_transcriptTab->refresh();
        }
        if (m_outputTab) {
            m_outputTab->refresh();
        }
        if (m_inspectorPane) {
            m_inspectorPane->refreshTab(QStringLiteral("Transcript"));
        }
        scheduleSaveState();
        pushHistorySnapshot();

        if (failedCount > 0) {
            QMessageBox::warning(
                this,
                QStringLiteral("Delete Transcript"),
                QStringLiteral("Some transcript files could not be deleted.\nDeleted: %1\nFailed: %2")
                    .arg(deletedCount)
                    .arg(failedCount));
        }
    };
    m_timeline->syncRequested = [this](const QSet<QString>& selectedClipIds) {
        requestAutoSyncForSelection(selectedClipIds);
    };
    m_timeline->createProxyRequested = [this](const QString &clipId) { createProxyForClip(clipId); };
    m_timeline->continueProxyRequested = [this](const QString &clipId) { continueProxyForClip(clipId); };
    m_timeline->deleteProxyRequested = [this](const QString &clipId) { deleteProxyForClip(clipId); };
    m_timeline->generateFaceDetectionsRequested = [this](const QString& clipId) {
        if (!m_timeline || !m_speakersTab) {
            return;
        }
        m_timeline->setSelectedClipId(clipId);
        m_speakersTab->refresh();
        m_speakersTab->generateFaceDetectionsForSelectedClip();
        if (m_inspectorPane) {
            m_inspectorPane->refreshTab(QStringLiteral("Speakers"));
        }
    };
    m_timeline->deleteFaceDetectionsRequested = [this](const QString& clipId) {
        if (!m_timeline || !m_speakersTab) {
            return;
        }
        m_timeline->setSelectedClipId(clipId);
        m_speakersTab->refresh();
        m_speakersTab->deleteFaceDetectionsForSelectedClip(true);
        if (m_inspectorPane) {
            m_inspectorPane->refreshTab(QStringLiteral("Speakers"));
        }
    };
    m_timeline->detectRequested = [this](const QString& clipId) {
        openSamDetectorWindow(clipId);
    };
    m_timeline->scaleToFillRequested = [this](const QString &clipId) {
        if (!m_timeline) return;
        const TimelineClip *clip = nullptr;
        for (const TimelineClip &c : m_timeline->clips()) {
            if (c.id == clipId) { clip = &c; break; }
        }
        if (!clip || !clipHasVisuals(*clip)) return;

        const QString mediaPath = playbackMediaPathForClip(*clip);
        const MediaProbeResult probe = probeMediaFile(mediaPath, clip->durationFrames / kTimelineFps);
        if (!probe.hasVideo || probe.frameSize.isEmpty()) return;

        const QSize outputSize = m_preview->outputSize();
        if (outputSize.isEmpty()) return;

        const qreal fitScaleX = static_cast<qreal>(outputSize.width()) / probe.frameSize.width();
        const qreal fitScaleY = static_cast<qreal>(outputSize.height()) / probe.frameSize.height();
        const qreal fillScale = qMax(fitScaleX, fitScaleY) / qMin(fitScaleX, fitScaleY);

        m_timeline->updateClipById(clipId, [fillScale](TimelineClip &c) {
            c.baseScaleX = fillScale;
            c.baseScaleY = fillScale;
            c.baseTranslationX = 0.0;
            c.baseTranslationY = 0.0;
            normalizeClipTransformKeyframes(c);
        });
        m_preview->setTimelineTracks(m_timeline->tracks());
        m_preview->setTimelineClips(m_timeline->clips());
        m_inspectorPane->refreshTab(QStringLiteral("Transform"));
        scheduleSaveState();
        pushHistorySnapshot();
    };
    m_timeline->toolModeChanged = [this]() {
        if (m_editorPane) {
            m_editorPane->razorButton()->setChecked(
                m_timeline->toolMode() == TimelineWidget::ToolMode::Razor);
        }
    };
}

void EditorWindow::connectPreviewSignals()
{
    m_preview->selectionRequested = [this](const QString &clipId) {
        if (m_timeline) m_timeline->setSelectedClipId(clipId);
    };
    m_preview->playbackSampleRequested = [this](int64_t samplePosition) {
        setCurrentPlaybackSample(samplePosition, true, playbackActive());
    };
    m_preview->correctionPointRequested = [this](const QString& clipId, qreal xNorm, qreal yNorm) {
        if (m_correctionsTab) {
            m_correctionsTab->handlePreviewPoint(clipId, xNorm, yNorm);
        }
    };
    m_preview->speakerPointRequested = [this](const QString& clipId, qreal xNorm, qreal yNorm) {
        if (m_speakersTab) {
            m_speakersTab->handlePreviewPoint(clipId, xNorm, yNorm);
        }
    };
    m_preview->speakerBoxRequested = [this](const QString& clipId,
                                            qreal xNorm,
                                            qreal yNorm,
                                            qreal boxSizeNorm) {
        if (m_speakersTab) {
            m_speakersTab->handlePreviewBox(clipId, xNorm, yNorm, boxSizeNorm);
        }
    };
    m_preview->faceStreamBoxRequested = [this](const QString& clipId,
                                               int trackId,
                                               const QString& streamId,
                                               int64_t sourceFrame,
                                               qreal xNorm,
                                               qreal yNorm,
                                               qreal boxSizeNorm) {
        QElapsedTimer timer;
        timer.start();
        if (m_speakersTab) {
            m_speakersTab->handlePreviewFaceDetectionsBox(
                clipId, trackId, streamId, sourceFrame, xNorm, yNorm, boxSizeNorm);
        }
        qInfo().noquote()
            << QStringLiteral("Face box click timing: phase=editor_callback elapsed_ms=%1")
                   .arg(timer.elapsed());
    };
    m_preview->faceStreamBoxFocusClearRequested = [this](const QString& clipId,
                                                         int trackId,
                                                         const QString& streamId,
                                                         int64_t sourceFrame,
                                                         qreal xNorm,
                                                         qreal yNorm,
                                                         qreal boxSizeNorm) {
        QElapsedTimer timer;
        timer.start();
        if (m_speakersTab) {
            m_speakersTab->handlePreviewFaceDetectionsBoxFocusClear(
                clipId, trackId, streamId, sourceFrame, xNorm, yNorm, boxSizeNorm);
        }
        qInfo().noquote()
            << QStringLiteral("Face box right-click timing: phase=editor_callback elapsed_ms=%1")
                   .arg(timer.elapsed());
    };
    m_preview->faceStreamBoxClickStatus = [this](const QString& message) {
        qInfo().noquote() << message;
        if (m_speakersTab) {
            m_speakersTab->showPreviewFaceDetectionsClickStatus(message);
        }
    };
    m_preview->createKeyframeRequested = [this](const QString &clipId) {
        if (!m_timeline) return;

        const int64_t currentFrame = m_timeline->currentFrame();
        const bool updated =
            m_timeline->updateClipById(clipId, [currentFrame](TimelineClip& clip) {
                createPreviewKeyframeAtTimelineFrame(clip, currentFrame);
            });
        if (!updated) return;
        m_preview->setTimelineTracks(m_timeline->tracks());
        m_preview->setTimelineClips(m_timeline->clips());
        m_preview->setRenderSyncMarkers(m_timeline->renderSyncMarkers());
        refreshPreviewTransformInspectorViews();
        scheduleSaveState();
        pushHistorySnapshot();
    };
    m_preview->resizeRequested = [this](const QString &clipId, qreal scaleX, qreal scaleY, bool finalize) {
        if (!m_timeline) return;
        const int64_t currentFrame = m_timeline->currentFrame();
        const bool playing = playbackActive();
        const int64_t keyframeTimelineFrame =
            resolvePreviewDragKeyframeTimelineFrame(
                m_previewDragAnchorFrameByClip, clipId, currentFrame, playing, finalize);
        const bool transcriptOverlaySelected = m_preview && m_preview->selectedOverlayIsTranscript();
        if (playing && !finalize && !transcriptOverlaySelected) {
            QVector<TimelineClip> previewClips = m_timeline->clips();
            bool previewUpdated = false;
            for (TimelineClip& clip : previewClips) {
                if (clip.id != clipId) {
                    continue;
                }
                previewUpdated = stagePreviewResize(
                    clip, keyframeTimelineFrame, scaleX, scaleY);
                break;
            }
            if (!previewUpdated) return;
            refreshPreviewTimeline(m_preview, m_timeline, previewClips);
            return;
        }

        const bool updated = m_timeline->updateClipById(clipId, [keyframeTimelineFrame, scaleX, scaleY, transcriptOverlaySelected](TimelineClip &clip) {
            commitPreviewResize(
                clip,
                keyframeTimelineFrame,
                scaleX,
                scaleY,
                transcriptOverlaySelected);
        });
        if (!updated) return;
        refreshPreviewTimeline(m_preview, m_timeline, m_timeline->clips());
        refreshPreviewTransformInspectorViews();
        scheduleSaveState();
        if (finalize) pushHistorySnapshot();
    };
    m_preview->moveRequested = [this](const QString &clipId, qreal translationX, qreal translationY, bool finalize) {
        if (!m_timeline) return;
        const int64_t currentFrame = m_timeline->currentFrame();
        const bool playing = playbackActive();
        const int64_t keyframeTimelineFrame =
            resolvePreviewDragKeyframeTimelineFrame(
                m_previewDragAnchorFrameByClip, clipId, currentFrame, playing, finalize);
        const bool transcriptOverlaySelected = m_preview && m_preview->selectedOverlayIsTranscript();
        if (playing && !finalize && !transcriptOverlaySelected) {
            QVector<TimelineClip> previewClips = m_timeline->clips();
            bool previewUpdated = false;
            for (TimelineClip& clip : previewClips) {
                if (clip.id != clipId) {
                    continue;
                }
                previewUpdated = stagePreviewMove(
                    clip, keyframeTimelineFrame, translationX, translationY);
                break;
            }
            if (!previewUpdated) return;
            refreshPreviewTimeline(m_preview, m_timeline, previewClips);
            return;
        }

        const bool updated = m_timeline->updateClipById(clipId, [keyframeTimelineFrame, translationX, translationY, transcriptOverlaySelected](TimelineClip &clip) {
            commitPreviewMove(
                clip,
                keyframeTimelineFrame,
                translationX,
                translationY,
                transcriptOverlaySelected);
        });
        if (!updated) return;
        refreshPreviewTimeline(m_preview, m_timeline, m_timeline->clips());
        refreshPreviewTransformInspectorViews();
        scheduleSaveState();
        if (finalize) pushHistorySnapshot();
    };
}

QWidget *EditorWindow::buildEditorPane()
{
    auto *pane = new EditorPane;
    bindEditorPaneWidgets(pane);
    connectTransportControls(pane);
    connectTimelineSignals();
    connectPreviewSignals();
    return pane;
}

void EditorWindow::addFileToTimeline(const QString &filePath, int64_t startFrame)
{
    if (m_timeline) {
        m_timeline->addClipFromFile(filePath, startFrame);
        m_preview->setTimelineTracks(m_timeline->tracks());
        m_preview->setTimelineClips(m_timeline->clips());
    }
}

void EditorWindow::syncSliderRange()
{
    const int64_t maxFrame = m_timeline->totalFrames();
    m_seekSlider->setRange(0, static_cast<int>(qMin<int64_t>(maxFrame, INT_MAX)));
}

void EditorWindow::focusGradingTab()
{
    if (m_inspectorTabs) {
        const int currentIndex = m_inspectorTabs->currentIndex();
        if (currentIndex >= 0 &&
            m_inspectorTabs->tabText(currentIndex).compare(QStringLiteral("Jobs"), Qt::CaseInsensitive) == 0) {
            return;
        }
        m_inspectorTabs->setCurrentIndex(0);
    }
}

void EditorWindow::updateTransportLabels()
{
    const bool playing = playbackActive();
    const QString state = playing ? QStringLiteral("PLAYING") : QStringLiteral("PAUSED");
    const int clipCount = m_timeline ? m_timeline->clips().size() : 0;
    const QString activeAudio = m_preview ? m_preview->activeAudioClipLabel() : QString();

    m_statusBadge->setText(QStringLiteral("%1  |  %2 clips").arg(state).arg(clipCount));
    if (m_preview && m_timeline) {
        m_preview->setSelectedClipId(m_timeline->selectedClipId());
    }
    m_playButton->setText(QString());
    m_playButton->setToolTip(playing ? QStringLiteral("Pause") : QStringLiteral("Play"));
    m_playButton->setIcon(editor::playPauseTransportIcon(playing));
    if (m_playbackLoopButton) {
        QSignalBlocker blocker(m_playbackLoopButton);
        m_playbackLoopButton->setChecked(m_playbackLoopEnabled);
        m_playbackLoopButton->setToolTip(m_playbackLoopEnabled
                                             ? QStringLiteral("Loop playback range (On)")
                                             : QStringLiteral("Loop playback range (Off)"));
    }
    const bool muted = m_preview && m_preview->audioMuted();
    m_audioMuteButton->setText(QString());
    m_audioMuteButton->setToolTip(muted ? QStringLiteral("Unmute") : QStringLiteral("Mute"));
    m_audioMuteButton->setIcon(editor::volumeTransportIcon(muted));
    if (m_retimingAudioForPlayback) {
        m_audioNowPlayingLabel->setText(QStringLiteral("Re-timing audio"));
    } else if (m_playbackAudioWarmupPending) {
        m_audioNowPlayingLabel->setText(QStringLiteral("Loading re-timed audio"));
    } else if (m_playbackVideoWarmupPending) {
        m_audioNowPlayingLabel->setText(QStringLiteral("Buffering video"));
    } else {
        m_audioNowPlayingLabel->setText(activeAudio.isEmpty()
                                            ? QStringLiteral("Audio idle")
                                            : QStringLiteral("Audio  %1").arg(activeAudio));
    }
    updatePlaybackStatusOverlay();
}

QString EditorWindow::frameToTimecode(int64_t frame) const
{
    const int fps = kTimelineFps;
    const int64_t totalSeconds = frame / fps;
    const int64_t minutes = totalSeconds / 60;
    const int64_t seconds = totalSeconds % 60;
    const int64_t frames = frame % fps;

    return QStringLiteral("%1:%2:%3")
        .arg(minutes, 2, 10, QLatin1Char('0'))
        .arg(seconds, 2, 10, QLatin1Char('0'))
        .arg(frames, 2, 10, QLatin1Char('0'));
}
