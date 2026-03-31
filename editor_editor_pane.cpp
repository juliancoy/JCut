#include "editor.h"

#include <QStyle>
#include <QToolButton>

using namespace editor;

void EditorWindow::bindEditorPaneWidgets(EditorPane *pane)
{
    m_editorPane = pane;
    m_preview = pane->previewWindow();
    m_timeline = pane->timelineWidget();
    m_playButton = pane->playButton();
    m_seekSlider = pane->seekSlider();
    m_timecodeLabel = pane->timecodeLabel();
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
    connect(pane, &EditorPane::endClicked, this, [this]() {
        setCurrentFrame(m_timeline ? m_timeline->totalFrames() : 0);
    });
    connect(pane, &EditorPane::seekValueChanged, this, [this](int value) {
        if (m_ignoreSeekSignal) return;
        setCurrentFrame(value);
    });
    connect(pane, &EditorPane::audioMuteClicked, this, [this]() {
        const bool nextMuted = !m_preview->audioMuted();
        m_preview->setAudioMuted(nextMuted);
        if (m_audioEngine) m_audioEngine->setMuted(nextMuted);
        m_inspectorPane->refresh();
        scheduleSaveState();
    });
    connect(pane, &EditorPane::audioVolumeChanged, this, [this](int value) {
        m_preview->setAudioVolume(value / 100.0);
        if (m_audioEngine) m_audioEngine->setVolume(value / 100.0);
        m_inspectorPane->refresh();
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
        m_preview->setTimelineClips(m_timeline->clips());
        m_preview->setExportRanges(effectivePlaybackRanges());
        m_preview->setRenderSyncMarkers(m_timeline->renderSyncMarkers());
        m_preview->setSelectedClipId(m_timeline->selectedClipId());
        m_preview->endBulkUpdate();
        if (m_audioEngine) {
            m_audioEngine->setTimelineClips(m_timeline->clips());
            m_audioEngine->setExportRanges(effectivePlaybackRanges());
            m_audioEngine->setRenderSyncMarkers(m_timeline->renderSyncMarkers());
            m_audioEngine->setSpeechFilterFadeSamples(m_speechFilterFadeSamples);
        }
        refreshClipInspector();
        m_inspectorPane->refresh();
        scheduleSaveState();
        pushHistorySnapshot();
    };
    m_timeline->selectionChanged = [this]() {
        m_preview->setSelectedClipId(m_timeline->selectedClipId());
        refreshSyncInspector();
        m_transcriptTab->refresh();
        refreshClipInspector();
        m_outputTab->refresh();
        m_profileTab->refresh();
        m_gradingTab->refresh();
        m_effectsTab->refresh();
        m_titlesTab->refresh();
        m_videoKeyframeTab->refresh();
        m_inspectorPane->refresh();
    };
    m_timeline->renderSyncMarkersChanged = [this]() {
        m_preview->setRenderSyncMarkers(m_timeline->renderSyncMarkers());
        if (m_audioEngine) {
            m_audioEngine->setRenderSyncMarkers(m_timeline->renderSyncMarkers());
        }
        refreshSyncInspector();
        m_inspectorPane->refresh();
        scheduleSaveState();
        pushHistorySnapshot();
    };
    m_timeline->exportRangeChanged = [this]() {
        m_outputTab->refresh();
        m_inspectorPane->refresh();
        scheduleSaveState();
        pushHistorySnapshot();
    };
    m_timeline->gradingRequested = [this]() {
        focusGradingTab();
        m_inspectorPane->refresh();
    };
    m_timeline->transcribeRequested = [this](const QString &filePath, const QString &label) {
        openTranscriptionWindow(filePath, label);
    };
    m_timeline->createProxyRequested = [this](const QString &clipId) { createProxyForClip(clipId); };
    m_timeline->deleteProxyRequested = [this](const QString &clipId) { deleteProxyForClip(clipId); };
    m_timeline->scaleToFillRequested = [this](const QString &clipId) {
        if (!m_timeline) return;
        const TimelineClip *clip = nullptr;
        for (const TimelineClip &c : m_timeline->clips()) {
            if (c.id == clipId) { clip = &c; break; }
        }
        if (!clip || !clipHasVisuals(*clip)) return;

        const QString mediaPath = playbackMediaPathForClip(*clip);
        const MediaProbeResult probe = probeMediaFile(mediaPath, clip->durationFrames);
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
        m_preview->setTimelineClips(m_timeline->clips());
        m_inspectorPane->refresh();
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
    m_preview->resizeRequested = [this](const QString &clipId, qreal scaleX, qreal scaleY, bool finalize) {
        if (!m_timeline) return;
        const int64_t currentFrame = m_timeline->currentFrame();
        const bool updated = m_timeline->updateClipById(clipId, [this, currentFrame, scaleX, scaleY](TimelineClip &clip) {
            if (clip.mediaType == ClipMediaType::Audio && clip.transcriptOverlay.enabled) {
                clip.transcriptOverlay.boxWidth = qMax<qreal>(80.0, scaleX);
                clip.transcriptOverlay.boxHeight = qMax<qreal>(40.0, scaleY);
                return;
            }
            if (!clipHasVisuals(clip)) return;
            const TimelineClip::TransformKeyframe offset = evaluateClipKeyframeOffsetAtFrame(clip, currentFrame);
            const int64_t keyframeFrame = qBound<int64_t>(0, currentFrame - clip.startFrame, qMax<int64_t>(0, clip.durationFrames - 1));
            TimelineClip::TransformKeyframe keyframe = offset;
            keyframe.frame = keyframeFrame;
            keyframe.scaleX = sanitizeScaleValue(scaleX / sanitizeScaleValue(clip.baseScaleX));
            keyframe.scaleY = sanitizeScaleValue(scaleY / sanitizeScaleValue(clip.baseScaleY));
            bool replaced = false;
            for (TimelineClip::TransformKeyframe &existing : clip.transformKeyframes) {
                if (existing.frame == keyframeFrame) {
                    existing = keyframe;
                    replaced = true;
                    break;
                }
            }
            if (!replaced) {
                clip.transformKeyframes.push_back(keyframe);
            }
            normalizeClipTransformKeyframes(clip);
        });
        if (!updated) return;
        m_preview->setTimelineClips(m_timeline->clips());
        m_preview->setRenderSyncMarkers(m_timeline->renderSyncMarkers());
        m_inspectorPane->refresh();
        scheduleSaveState();
        if (finalize) pushHistorySnapshot();
    };
    m_preview->moveRequested = [this](const QString &clipId, qreal translationX, qreal translationY, bool finalize) {
        if (!m_timeline) return;
        const int64_t currentFrame = m_timeline->currentFrame();
        const bool updated = m_timeline->updateClipById(clipId, [this, currentFrame, translationX, translationY](TimelineClip &clip) {
            if (clip.mediaType == ClipMediaType::Audio && clip.transcriptOverlay.enabled) {
                clip.transcriptOverlay.translationX = translationX;
                clip.transcriptOverlay.translationY = translationY;
                return;
            }
            if (clip.mediaType == ClipMediaType::Title) {
                const int64_t localFrame = qBound<int64_t>(0, currentFrame - clip.startFrame,
                    qMax<int64_t>(0, clip.durationFrames - 1));
                bool replaced = false;
                for (auto &kf : clip.titleKeyframes) {
                    if (kf.frame == localFrame) {
                        kf.translationX = translationX;
                        kf.translationY = translationY;
                        replaced = true;
                        break;
                    }
                }
                if (!replaced && !clip.titleKeyframes.isEmpty()) {
                    int bestIdx = 0;
                    for (int i = 0; i < clip.titleKeyframes.size(); ++i) {
                        if (clip.titleKeyframes[i].frame <= localFrame) bestIdx = i;
                    }
                    clip.titleKeyframes[bestIdx].translationX = translationX;
                    clip.titleKeyframes[bestIdx].translationY = translationY;
                }
                normalizeClipTitleKeyframes(clip);
                return;
            }
            if (!clipHasVisuals(clip)) return;
            const TimelineClip::TransformKeyframe offset = evaluateClipKeyframeOffsetAtFrame(clip, currentFrame);
            const int64_t keyframeFrame = qBound<int64_t>(0, currentFrame - clip.startFrame, qMax<int64_t>(0, clip.durationFrames - 1));
            TimelineClip::TransformKeyframe keyframe = offset;
            keyframe.frame = keyframeFrame;
            keyframe.translationX = translationX - clip.baseTranslationX;
            keyframe.translationY = translationY - clip.baseTranslationY;
            bool replaced = false;
            for (TimelineClip::TransformKeyframe& existing : clip.transformKeyframes) {
                if (existing.frame == keyframeFrame) {
                    existing = keyframe;
                    replaced = true;
                    break;
                }
            }
            if (!replaced) {
                clip.transformKeyframes.push_back(keyframe);
            }
            normalizeClipTransformKeyframes(clip);
        });
        if (!updated) return;
        m_preview->setTimelineClips(m_timeline->clips());
        m_preview->setRenderSyncMarkers(m_timeline->renderSyncMarkers());
        m_inspectorPane->refresh();
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

void EditorWindow::addFileToTimeline(const QString &filePath)
{
    if (m_timeline) {
        m_timeline->addClipFromFile(filePath);
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
    m_previewInfo->setText(QStringLiteral("Professional pipeline with libavcodec\nBackend: %1\nSeek: %2\nAudio: %3\nGrading: %4")
                               .arg(m_preview ? m_preview->backendName() : QStringLiteral("unknown"))
                               .arg(frameToTimecode(m_timeline ? m_timeline->currentFrame() : 0))
                               .arg(activeAudio.isEmpty() ? QStringLiteral("idle") : activeAudio)
                               .arg(m_preview && m_preview->bypassGrading() ? QStringLiteral("bypassed") : QStringLiteral("on")));
    m_playButton->setText(playing ? QStringLiteral("Pause") : QStringLiteral("Play"));
    m_playButton->setIcon(style()->standardIcon(playing ? QStyle::SP_MediaPause : QStyle::SP_MediaPlay));
    m_audioMuteButton->setText(m_preview && m_preview->audioMuted() ? QStringLiteral("Unmute") : QStringLiteral("Mute"));
    m_audioNowPlayingLabel->setText(activeAudio.isEmpty() ? QStringLiteral("Audio idle") : QStringLiteral("Audio  %1").arg(activeAudio));
}

QString EditorWindow::frameToTimecode(int64_t frame) const
{
    const int fps = 30;
    const int64_t totalSeconds = frame / fps;
    const int64_t minutes = totalSeconds / 60;
    const int64_t seconds = totalSeconds % 60;
    const int64_t frames = frame % fps;

    return QStringLiteral("%1:%2:%3")
        .arg(minutes, 2, 10, QLatin1Char('0'))
        .arg(seconds, 2, 10, QLatin1Char('0'))
        .arg(frames, 2, 10, QLatin1Char('0'));
}
