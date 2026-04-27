#include "editor.h"
#include "titles.h"

#include <QFile>
#include <QFileInfo>
#include <QMessageBox>
#include <QStyle>
#include <QToolButton>

using namespace editor;

namespace {
bool clipSupportsTranscriptOverlay(const TimelineClip& clip) {
    return (clip.mediaType == ClipMediaType::Audio || clip.hasAudio) && clip.transcriptOverlay.enabled;
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
    connect(pane, &EditorPane::previewModeChanged, this, [this](const QString& mode) {
        applyPreviewViewMode(mode);
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
        m_preview->setTimelineTracks(m_timeline->tracks());
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
            m_audioEngine->setSpeechFilterRangeCrossfadeEnabled(m_speechFilterRangeCrossfade);
            m_audioEngine->setPlaybackWarpMode(m_playbackAudioWarpMode);
            m_audioEngine->setPlaybackRate(effectiveAudioWarpRate());
        }
        refreshClipInspector();
        m_inspectorPane->refresh();
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
        if (selectionChanged && selectedClip && !selectedClip->filePath.isEmpty()) {
            const QString transcriptPath = transcriptWorkingPathForClipFile(selectedClip->filePath);
            if (QFileInfo::exists(transcriptPath) && m_inspectorTabs) {
                for (int i = 0; i < m_inspectorTabs->count(); ++i) {
                    if (m_inspectorTabs->tabText(i) == QStringLiteral("Transcript")) {
                        m_inspectorTabs->setCurrentIndex(i);
                        break;
                    }
                }
            }
        }
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
            m_inspectorPane->refresh();
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
    m_preview->createKeyframeRequested = [this](const QString &clipId) {
        if (!m_timeline) return;

        const int64_t currentFrame = m_timeline->currentFrame();
        const bool updated = m_timeline->updateClipById(clipId, [currentFrame](TimelineClip &clip) {
            const int64_t localFrame = qBound<int64_t>(
                0,
                currentFrame - clip.startFrame,
                qMax<int64_t>(0, clip.durationFrames - 1));

            if (clip.mediaType == ClipMediaType::Title) {
                if (clip.titleKeyframes.isEmpty()) {
                    return;
                }

                const EvaluatedTitle evaluated = evaluateTitleAtLocalFrame(clip, localFrame);
                if (!evaluated.valid) {
                    return;
                }

                TimelineClip::TitleKeyframe keyframe = clip.titleKeyframes.constFirst();
                keyframe.frame = localFrame;
                keyframe.text = evaluated.text;
                keyframe.translationX = evaluated.x;
                keyframe.translationY = evaluated.y;
                keyframe.fontSize = evaluated.fontSize;
                keyframe.opacity = evaluated.opacity;
                keyframe.fontFamily = evaluated.fontFamily;
                keyframe.bold = evaluated.bold;
                keyframe.italic = evaluated.italic;
                keyframe.color = evaluated.color;
                keyframe.dropShadowEnabled = evaluated.dropShadowEnabled;
                keyframe.dropShadowColor = evaluated.dropShadowColor;
                keyframe.dropShadowOpacity = evaluated.dropShadowOpacity;
                keyframe.dropShadowOffsetX = evaluated.dropShadowOffsetX;
                keyframe.dropShadowOffsetY = evaluated.dropShadowOffsetY;
                keyframe.windowEnabled = evaluated.windowEnabled;
                keyframe.windowColor = evaluated.windowColor;
                keyframe.windowOpacity = evaluated.windowOpacity;
                keyframe.windowPadding = evaluated.windowPadding;
                keyframe.windowFrameEnabled = evaluated.windowFrameEnabled;
                keyframe.windowFrameColor = evaluated.windowFrameColor;
                keyframe.windowFrameOpacity = evaluated.windowFrameOpacity;
                keyframe.windowFrameWidth = evaluated.windowFrameWidth;
                keyframe.windowFrameGap = evaluated.windowFrameGap;
                for (const TimelineClip::TitleKeyframe &existing : clip.titleKeyframes) {
                    if (existing.frame > localFrame) {
                        keyframe.linearInterpolation = existing.linearInterpolation;
                        break;
                    }
                }

                bool replaced = false;
                for (TimelineClip::TitleKeyframe &existing : clip.titleKeyframes) {
                    if (existing.frame == localFrame) {
                        existing = keyframe;
                        replaced = true;
                        break;
                    }
                }
                if (!replaced) {
                    clip.titleKeyframes.push_back(keyframe);
                }
                normalizeClipTitleKeyframes(clip);
                return;
            }

            if (!clipHasVisuals(clip)) return;

            const TimelineClip::TransformKeyframe evaluated =
                evaluateClipTransformAtFrame(clip, currentFrame);
            TimelineClip::TransformKeyframe keyframe;
            keyframe.frame = localFrame;
            keyframe.translationX = evaluated.translationX - clip.baseTranslationX;
            keyframe.translationY = evaluated.translationY - clip.baseTranslationY;
            keyframe.rotation = evaluated.rotation - clip.baseRotation;
            keyframe.scaleX =
                sanitizeScaleValue(evaluated.scaleX / sanitizeScaleValue(clip.baseScaleX));
            keyframe.scaleY =
                sanitizeScaleValue(evaluated.scaleY / sanitizeScaleValue(clip.baseScaleY));
            for (const TimelineClip::TransformKeyframe &existing : clip.transformKeyframes) {
                if (existing.frame > localFrame) {
                    keyframe.linearInterpolation = existing.linearInterpolation;
                    break;
                }
            }

            bool replaced = false;
            for (TimelineClip::TransformKeyframe &existing : clip.transformKeyframes) {
                if (existing.frame == localFrame) {
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
        m_preview->setTimelineTracks(m_timeline->tracks());
        m_preview->setTimelineClips(m_timeline->clips());
        m_preview->setRenderSyncMarkers(m_timeline->renderSyncMarkers());
        m_inspectorPane->refresh();
        scheduleSaveState();
        pushHistorySnapshot();
    };
    m_preview->resizeRequested = [this](const QString &clipId, qreal scaleX, qreal scaleY, bool finalize) {
        if (!m_timeline) return;
        const int64_t currentFrame = m_timeline->currentFrame();
        const bool playing = playbackActive();
        int64_t keyframeTimelineFrame = currentFrame;
        if (playing) {
            if (finalize) {
                keyframeTimelineFrame = m_previewDragAnchorFrameByClip.value(clipId, currentFrame);
                m_previewDragAnchorFrameByClip.remove(clipId);
            } else {
                keyframeTimelineFrame = m_previewDragAnchorFrameByClip.value(clipId, currentFrame);
                m_previewDragAnchorFrameByClip.insert(clipId, keyframeTimelineFrame);
            }
        } else if (finalize) {
            m_previewDragAnchorFrameByClip.remove(clipId);
        }
        const bool transcriptOverlaySelected = m_preview && m_preview->selectedOverlayIsTranscript();
        if (playing && !finalize && !transcriptOverlaySelected) {
            QVector<TimelineClip> previewClips = m_timeline->clips();
            bool previewUpdated = false;
            for (TimelineClip& clip : previewClips) {
                if (clip.id != clipId) {
                    continue;
                }
                if (!clipHasVisuals(clip)) {
                    break;
                }
                const TimelineClip::TransformKeyframe offset =
                    evaluateClipKeyframeOffsetAtFrame(clip, keyframeTimelineFrame);
                const int64_t keyframeFrame =
                    qBound<int64_t>(0,
                                    keyframeTimelineFrame - clip.startFrame,
                                    qMax<int64_t>(0, clip.durationFrames - 1));
                TimelineClip::TransformKeyframe keyframe = offset;
                keyframe.frame = keyframeFrame;
                keyframe.scaleX = sanitizeScaleValue(scaleX / sanitizeScaleValue(clip.baseScaleX));
                keyframe.scaleY = sanitizeScaleValue(scaleY / sanitizeScaleValue(clip.baseScaleY));

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
                previewUpdated = true;
                break;
            }
            if (!previewUpdated) return;
            m_preview->setTimelineTracks(m_timeline->tracks());
            m_preview->setTimelineClips(previewClips);
            m_preview->setRenderSyncMarkers(m_timeline->renderSyncMarkers());
            return;
        }

        const bool updated = m_timeline->updateClipById(clipId, [this, keyframeTimelineFrame, scaleX, scaleY, transcriptOverlaySelected](TimelineClip &clip) {
            if (transcriptOverlaySelected && clipSupportsTranscriptOverlay(clip)) {
                clip.transcriptOverlay.boxWidth = qMax<qreal>(80.0, scaleX);
                clip.transcriptOverlay.boxHeight = qMax<qreal>(40.0, scaleY);
                return;
            }
            if (!clipHasVisuals(clip)) return;
            const TimelineClip::TransformKeyframe offset = evaluateClipKeyframeOffsetAtFrame(clip, keyframeTimelineFrame);
            const int64_t keyframeFrame = qBound<int64_t>(0, keyframeTimelineFrame - clip.startFrame, qMax<int64_t>(0, clip.durationFrames - 1));
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
        m_preview->setTimelineTracks(m_timeline->tracks());
        m_preview->setTimelineClips(m_timeline->clips());
        m_preview->setRenderSyncMarkers(m_timeline->renderSyncMarkers());
        m_inspectorPane->refresh();
        scheduleSaveState();
        if (finalize) pushHistorySnapshot();
    };
    m_preview->moveRequested = [this](const QString &clipId, qreal translationX, qreal translationY, bool finalize) {
        if (!m_timeline) return;
        const int64_t currentFrame = m_timeline->currentFrame();
        const bool playing = playbackActive();
        int64_t keyframeTimelineFrame = currentFrame;
        if (playing) {
            if (finalize) {
                keyframeTimelineFrame = m_previewDragAnchorFrameByClip.value(clipId, currentFrame);
                m_previewDragAnchorFrameByClip.remove(clipId);
            } else {
                keyframeTimelineFrame = m_previewDragAnchorFrameByClip.value(clipId, currentFrame);
                m_previewDragAnchorFrameByClip.insert(clipId, keyframeTimelineFrame);
            }
        } else if (finalize) {
            m_previewDragAnchorFrameByClip.remove(clipId);
        }
        const bool transcriptOverlaySelected = m_preview && m_preview->selectedOverlayIsTranscript();
        if (playing && !finalize && !transcriptOverlaySelected) {
            QVector<TimelineClip> previewClips = m_timeline->clips();
            bool previewUpdated = false;
            for (TimelineClip& clip : previewClips) {
                if (clip.id != clipId) {
                    continue;
                }
                if (clip.mediaType == ClipMediaType::Title) {
                    const int64_t localFrame =
                        qBound<int64_t>(0,
                                        keyframeTimelineFrame - clip.startFrame,
                                        qMax<int64_t>(0, clip.durationFrames - 1));
                    bool replaced = false;
                    for (TimelineClip::TitleKeyframe& kf : clip.titleKeyframes) {
                        if (kf.frame == localFrame) {
                            kf.translationX = translationX;
                            kf.translationY = translationY;
                            replaced = true;
                            break;
                        }
                    }
                    if (!replaced && !clip.titleKeyframes.isEmpty()) {
                        TimelineClip::TitleKeyframe keyframe = clip.titleKeyframes.constFirst();
                        keyframe.frame = localFrame;
                        keyframe.translationX = translationX;
                        keyframe.translationY = translationY;
                        clip.titleKeyframes.push_back(keyframe);
                    }
                    normalizeClipTitleKeyframes(clip);
                    previewUpdated = true;
                    break;
                }
                if (!clipHasVisuals(clip)) {
                    break;
                }
                const TimelineClip::TransformKeyframe offset =
                    evaluateClipKeyframeOffsetAtFrame(clip, keyframeTimelineFrame);
                const int64_t keyframeFrame =
                    qBound<int64_t>(0,
                                    keyframeTimelineFrame - clip.startFrame,
                                    qMax<int64_t>(0, clip.durationFrames - 1));
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
                previewUpdated = true;
                break;
            }
            if (!previewUpdated) return;
            m_preview->setTimelineTracks(m_timeline->tracks());
            m_preview->setTimelineClips(previewClips);
            m_preview->setRenderSyncMarkers(m_timeline->renderSyncMarkers());
            return;
        }

        const bool updated = m_timeline->updateClipById(clipId, [this, keyframeTimelineFrame, translationX, translationY, transcriptOverlaySelected](TimelineClip &clip) {
            if (transcriptOverlaySelected && clipSupportsTranscriptOverlay(clip)) {
                clip.transcriptOverlay.translationX = translationX;
                clip.transcriptOverlay.translationY = translationY;
                clip.transcriptOverlay.useManualPlacement = true;
                return;
            }
            if (clip.mediaType == ClipMediaType::Title) {
                const int64_t localFrame = qBound<int64_t>(0, keyframeTimelineFrame - clip.startFrame,
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
            const TimelineClip::TransformKeyframe offset = evaluateClipKeyframeOffsetAtFrame(clip, keyframeTimelineFrame);
            const int64_t keyframeFrame = qBound<int64_t>(0, keyframeTimelineFrame - clip.startFrame, qMax<int64_t>(0, clip.durationFrames - 1));
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
        m_preview->setTimelineTracks(m_timeline->tracks());
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
    if (m_previewInfo) {
        m_previewInfo->clear();
        m_previewInfo->setVisible(false);
    }
    m_playButton->setText(QString());
    m_playButton->setToolTip(playing ? QStringLiteral("Pause") : QStringLiteral("Play"));
    m_playButton->setIcon(style()->standardIcon(playing ? QStyle::SP_MediaPause : QStyle::SP_MediaPlay));
    const bool muted = m_preview && m_preview->audioMuted();
    m_audioMuteButton->setText(QString());
    m_audioMuteButton->setToolTip(muted ? QStringLiteral("Unmute") : QStringLiteral("Mute"));
    m_audioMuteButton->setIcon(style()->standardIcon(muted ? QStyle::SP_MediaVolumeMuted : QStyle::SP_MediaVolume));
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
