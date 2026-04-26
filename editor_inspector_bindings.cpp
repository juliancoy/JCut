#include "editor.h"

#include <QCheckBox>
#include <QComboBox>
#include <QColorDialog>
#include <QDoubleSpinBox>
#include <QLineEdit>
#include <QMessageBox>
#include <QPushButton>
#include <QSignalBlocker>
#include <QSpinBox>
#include <QTableWidget>

#include "debug_controls.h"
#include "transform_skip_aware_timing.h"

using namespace editor;

void EditorWindow::bindInspectorWidgets()
{
    m_transcriptTable = m_inspectorPane->transcriptTable();
    m_transcriptInspectorClipLabel = m_inspectorPane->transcriptInspectorClipLabel();
    m_transcriptInspectorDetailsLabel = m_inspectorPane->transcriptInspectorDetailsLabel();
    m_clipInspectorClipLabel = m_inspectorPane->clipInspectorClipLabel();
    m_clipProxyUsageLabel = m_inspectorPane->clipProxyUsageLabel();
    m_clipPlaybackSourceLabel = m_inspectorPane->clipPlaybackSourceLabel();
    m_clipOriginalInfoLabel = m_inspectorPane->clipOriginalInfoLabel();
    m_clipProxyInfoLabel = m_inspectorPane->clipProxyInfoLabel();
    m_clipPlaybackRateSpin = m_inspectorPane->clipPlaybackRateSpin();
    m_trackInspectorLabel = m_inspectorPane->trackInspectorLabel();
    m_trackInspectorDetailsLabel = m_inspectorPane->trackInspectorDetailsLabel();
    m_trackNameEdit = m_inspectorPane->trackNameEdit();
    m_trackHeightSpin = m_inspectorPane->trackHeightSpin();
    m_trackVisualModeCombo = m_inspectorPane->trackVisualModeCombo();
    m_trackAudioEnabledCheckBox = m_inspectorPane->trackAudioEnabledCheckBox();
    m_trackCrossfadeSecondsSpin = m_inspectorPane->trackCrossfadeSecondsSpin();
    m_trackCrossfadeButton = m_inspectorPane->trackCrossfadeButton();
    m_previewHideOutsideOutputCheckBox = m_inspectorPane->previewHideOutsideOutputCheckBox();
    m_previewShowSpeakerTrackPointsCheckBox = m_inspectorPane->previewShowSpeakerTrackPointsCheckBox();
    m_speakerShowBoxStreamBoxesCheckBox = m_inspectorPane->speakerShowBoxStreamBoxesCheckBox();
    m_previewZoomSpin = m_inspectorPane->previewZoomSpin();
    m_previewZoomResetButton = m_inspectorPane->previewZoomResetButton();
    m_previewPlaybackCacheFallbackCheckBox = m_inspectorPane->previewPlaybackCacheFallbackCheckBox();
    m_previewLeadPrefetchEnabledCheckBox = m_inspectorPane->previewLeadPrefetchEnabledCheckBox();
    m_previewLeadPrefetchCountSpin = m_inspectorPane->previewLeadPrefetchCountSpin();
    m_previewPlaybackWindowAheadSpin = m_inspectorPane->previewPlaybackWindowAheadSpin();
    m_previewVisibleQueueReserveSpin = m_inspectorPane->previewVisibleQueueReserveSpin();
    m_transcriptOverlayEnabledCheckBox = m_inspectorPane->transcriptOverlayEnabledCheckBox();
    m_transcriptMaxLinesSpin = m_inspectorPane->transcriptMaxLinesSpin();
    m_transcriptMaxCharsSpin = m_inspectorPane->transcriptMaxCharsSpin();
    m_transcriptAutoScrollCheckBox = m_inspectorPane->transcriptAutoScrollCheckBox();
    m_transcriptFollowCurrentWordCheckBox = m_inspectorPane->transcriptFollowCurrentWordCheckBox();
    m_transcriptOverlayXSpin = m_inspectorPane->transcriptOverlayXSpin();
    m_transcriptOverlayYSpin = m_inspectorPane->transcriptOverlayYSpin();
    m_transcriptOverlayWidthSpin = m_inspectorPane->transcriptOverlayWidthSpin();
    m_transcriptOverlayHeightSpin = m_inspectorPane->transcriptOverlayHeightSpin();
    m_transcriptFontFamilyCombo = m_inspectorPane->transcriptFontFamilyCombo();
    m_transcriptFontSizeSpin = m_inspectorPane->transcriptFontSizeSpin();
    m_transcriptBoldCheckBox = m_inspectorPane->transcriptBoldCheckBox();
    m_transcriptItalicCheckBox = m_inspectorPane->transcriptItalicCheckBox();
    m_syncTable = m_inspectorPane->syncTable();
    m_syncInspectorClipLabel = m_inspectorPane->syncInspectorClipLabel();
    m_syncInspectorDetailsLabel = m_inspectorPane->syncInspectorDetailsLabel();
    m_clearAllSyncPointsButton = m_inspectorPane->clearAllSyncPointsButton();

    // Sync/Tracks tab signal wiring now lives in SyncTab/TracksTab controllers.

    m_gradingPathLabel = m_inspectorPane->gradingPathLabel();
    m_brightnessSpin = m_inspectorPane->brightnessSpin();
    m_contrastSpin = m_inspectorPane->contrastSpin();
    m_saturationSpin = m_inspectorPane->saturationSpin();
    m_opacitySpin = m_inspectorPane->opacitySpin();
    m_opacityKeyframeTable = m_inspectorPane->opacityKeyframeTable();
    // Shadows/Midtones/Highlights
    m_shadowsRSpin = m_inspectorPane->shadowsRSpin();
    m_shadowsGSpin = m_inspectorPane->shadowsGSpin();
    m_shadowsBSpin = m_inspectorPane->shadowsBSpin();
    m_midtonesRSpin = m_inspectorPane->midtonesRSpin();
    m_midtonesGSpin = m_inspectorPane->midtonesGSpin();
    m_midtonesBSpin = m_inspectorPane->midtonesBSpin();
    m_highlightsRSpin = m_inspectorPane->highlightsRSpin();
    m_highlightsGSpin = m_inspectorPane->highlightsGSpin();
    m_highlightsBSpin = m_inspectorPane->highlightsBSpin();
    m_gradingKeyframeTable = m_inspectorPane->gradingKeyframeTable();
    m_gradingAutoScrollCheckBox = m_inspectorPane->gradingAutoScrollCheckBox();
    m_gradingFollowCurrentCheckBox = m_inspectorPane->gradingFollowCurrentCheckBox();
    m_bypassGradingCheckBox = m_inspectorPane->gradingPreviewCheckBox();
    m_gradingKeyAtPlayheadButton = m_inspectorPane->gradingKeyAtPlayheadButton();
    m_gradingFadeInButton = m_inspectorPane->gradingFadeInButton();
    m_gradingFadeOutButton = m_inspectorPane->gradingFadeOutButton();
    m_gradingFadeDurationSpin = m_inspectorPane->gradingFadeDurationSpin();
    m_keyframesInspectorClipLabel = m_inspectorPane->keyframesInspectorClipLabel();
    m_keyframesInspectorDetailsLabel = m_inspectorPane->keyframesInspectorDetailsLabel();
    m_keyframesAutoScrollCheckBox = m_inspectorPane->keyframesAutoScrollCheckBox();
    m_keyframesFollowCurrentCheckBox = m_inspectorPane->keyframesFollowCurrentCheckBox();
    m_audioInspectorClipLabel = m_inspectorPane->audioInspectorClipLabel();
    m_audioInspectorDetailsLabel = m_inspectorPane->audioInspectorDetailsLabel();
    m_videoTranslationXSpin = m_inspectorPane->videoTranslationXSpin();
    m_videoTranslationYSpin = m_inspectorPane->videoTranslationYSpin();
    m_videoRotationSpin = m_inspectorPane->videoRotationSpin();
    m_videoScaleXSpin = m_inspectorPane->videoScaleXSpin();
    m_videoScaleYSpin = m_inspectorPane->videoScaleYSpin();
    m_videoKeyframeTable = m_inspectorPane->videoKeyframeTable();
    m_videoInterpolationCombo = m_inspectorPane->videoInterpolationCombo();
    m_mirrorHorizontalCheckBox = m_inspectorPane->mirrorHorizontalCheckBox();
    m_mirrorVerticalCheckBox = m_inspectorPane->mirrorVerticalCheckBox();
    m_lockVideoScaleCheckBox = m_inspectorPane->lockVideoScaleCheckBox();
    m_keyframeSpaceCheckBox = m_inspectorPane->keyframeSpaceCheckBox();
    m_keyframeSkipAwareTimingCheckBox = m_inspectorPane->keyframeSkipAwareTimingCheckBox();
    m_addVideoKeyframeButton = m_inspectorPane->addVideoKeyframeButton();
    m_removeVideoKeyframeButton = m_inspectorPane->removeVideoKeyframeButton();
    m_flipHorizontalButton = m_inspectorPane->flipHorizontalButton();
    m_outputWidthSpin = m_inspectorPane->outputWidthSpin();
    m_outputHeightSpin = m_inspectorPane->outputHeightSpin();
    m_exportStartSpin = m_inspectorPane->exportStartSpin();
    m_exportEndSpin = m_inspectorPane->exportEndSpin();
    m_outputFormatCombo = m_inspectorPane->outputFormatCombo();
    m_outputRangeSummaryLabel = m_inspectorPane->outputRangeSummaryLabel();
    m_renderUseProxiesCheckBox = m_inspectorPane->renderUseProxiesCheckBox();
    m_outputPlaybackCacheFallbackCheckBox = m_inspectorPane->outputPlaybackCacheFallbackCheckBox();
    m_outputLeadPrefetchEnabledCheckBox = m_inspectorPane->outputLeadPrefetchEnabledCheckBox();
    m_outputLeadPrefetchCountSpin = m_inspectorPane->outputLeadPrefetchCountSpin();
    m_outputPlaybackWindowAheadSpin = m_inspectorPane->outputPlaybackWindowAheadSpin();
    m_outputVisibleQueueReserveSpin = m_inspectorPane->outputVisibleQueueReserveSpin();
    m_outputPrefetchMaxQueueDepthSpin = m_inspectorPane->outputPrefetchMaxQueueDepthSpin();
    m_outputPrefetchMaxInflightSpin = m_inspectorPane->outputPrefetchMaxInflightSpin();
    m_outputPrefetchMaxPerTickSpin = m_inspectorPane->outputPrefetchMaxPerTickSpin();
    m_outputPrefetchSkipVisiblePendingThresholdSpin =
        m_inspectorPane->outputPrefetchSkipVisiblePendingThresholdSpin();
    m_outputDecoderLaneCountSpin = m_inspectorPane->outputDecoderLaneCountSpin();
    m_outputDecodeModeCombo = m_inspectorPane->outputDecodeModeCombo();
    m_outputDeterministicPipelineCheckBox = m_inspectorPane->outputDeterministicPipelineCheckBox();
    m_outputResetPipelineDefaultsButton = m_inspectorPane->outputResetPipelineDefaultsButton();
    m_createImageSequenceCheckBox = m_inspectorPane->renderCreateVideoFromSequenceCheckBox();
    m_autosaveIntervalMinutesSpin = m_inspectorPane->autosaveIntervalMinutesSpin();
    m_autosaveMaxBackupsSpin = m_inspectorPane->autosaveMaxBackupsSpin();
    m_renderButton = m_inspectorPane->renderButton();
    m_profileSummaryTable = m_inspectorPane->profileSummaryTable();
    m_profileBenchmarkButton = m_inspectorPane->profileBenchmarkButton();
    m_projectSectionLabel = m_inspectorPane->projectSectionLabel();
    m_projectsList = m_inspectorPane->projectsList();
    m_newProjectButton = m_inspectorPane->newProjectButton();
    m_saveProjectAsButton = m_inspectorPane->saveProjectAsButton();
    m_renameProjectButton = m_inspectorPane->renameProjectButton();
    m_transcriptPrependMsSpin = m_inspectorPane->transcriptPrependMsSpin();
    m_transcriptPostpendMsSpin = m_inspectorPane->transcriptPostpendMsSpin();
    m_speechFilterEnabledCheckBox = m_inspectorPane->speechFilterEnabledCheckBox();
    m_speechFilterFadeSamplesSpin = m_inspectorPane->speechFilterFadeSamplesSpin();
    m_speechFilterRangeCrossfadeCheckBox = m_inspectorPane->speechFilterRangeCrossfadeCheckBox();
    m_playbackClockSourceCombo = m_inspectorPane->playbackClockSourceCombo();
    m_playbackAudioWarpModeCombo = m_inspectorPane->playbackAudioWarpModeCombo();
    m_aiStatusLabel = m_inspectorPane->aiStatusLabel();
    m_aiModelCombo = m_inspectorPane->aiModelCombo();
    m_aiTranscribeButton = m_inspectorPane->aiTranscribeButton();
    m_aiFindSpeakerNamesButton = m_inspectorPane->aiFindSpeakerNamesButton();
    m_aiFindOrganizationsButton = m_inspectorPane->aiFindOrganizationsButton();
    m_aiCleanAssignmentsButton = m_inspectorPane->aiCleanAssignmentsButton();
    m_aiLoginButton = m_inspectorPane->aiLoginButton();
    m_aiLogoutButton = m_inspectorPane->aiLogoutButton();

    if (m_aiModelCombo) {
        connect(m_aiModelCombo, &QComboBox::currentTextChanged, this, [this](const QString& model) {
            m_aiSelectedModel = model.trimmed();
            scheduleSaveState();
        });
    }
    if (m_aiTranscribeButton) {
        connect(m_aiTranscribeButton, &QPushButton::clicked, this, [this]() { runAiTranscribeForSelection(); });
    }
    if (m_aiFindSpeakerNamesButton) {
        connect(m_aiFindSpeakerNamesButton, &QPushButton::clicked, this, [this]() { runAiFindSpeakerNames(); });
    }
    if (m_aiFindOrganizationsButton) {
        connect(m_aiFindOrganizationsButton, &QPushButton::clicked, this, [this]() { runAiFindOrganizations(); });
    }
    if (m_aiCleanAssignmentsButton) {
        connect(m_aiCleanAssignmentsButton, &QPushButton::clicked, this, [this]() { runAiCleanAssignments(); });
    }
    if (m_aiLoginButton) {
        connect(m_aiLoginButton, &QPushButton::clicked, this, [this]() { configureAiGatewayLogin(); });
    }
    if (m_aiLogoutButton) {
        connect(m_aiLogoutButton, &QPushButton::clicked, this, [this]() { clearAiGatewayLogin(); });
    }
}

void EditorWindow::setupSpeechFilterControls()
{
    const auto refreshSpeechFilterRouting = [this](bool pushHistory = false) {
        const QVector<ExportRangeSegment> ranges = effectivePlaybackRanges();
        setTransformSkipAwareTimelineRanges(
            speechFilterPlaybackEnabled() ? ranges : QVector<ExportRangeSegment>{});
        if (m_preview) m_preview->setExportRanges(ranges);
        if (m_audioEngine) {
            m_audioEngine->setExportRanges(ranges);
            m_audioEngine->setSpeechFilterFadeSamples(m_speechFilterFadeSamples);
            m_audioEngine->setSpeechFilterRangeCrossfadeEnabled(m_speechFilterRangeCrossfade);
            m_audioEngine->setPlaybackWarpMode(m_playbackAudioWarpMode);
            m_audioEngine->setPlaybackRate(effectiveAudioWarpRate());
        }
        m_inspectorPane->refresh();
        scheduleSaveState();
        if (pushHistory) pushHistorySnapshot();
    };

    connect(m_speechFilterEnabledCheckBox, &QCheckBox::toggled, this,
            [refreshSpeechFilterRouting](bool) { refreshSpeechFilterRouting(true); });
    connect(m_transcriptPrependMsSpin, qOverload<int>(&QSpinBox::valueChanged), this,
            [this, refreshSpeechFilterRouting](int value) {
                m_transcriptPrependMs = qMax(0, value);
                refreshSpeechFilterRouting(true);
            });
    connect(m_transcriptPostpendMsSpin, qOverload<int>(&QSpinBox::valueChanged), this,
            [this, refreshSpeechFilterRouting](int value) {
                m_transcriptPostpendMs = qMax(0, value);
                refreshSpeechFilterRouting(true);
            });
    connect(m_speechFilterFadeSamplesSpin, qOverload<int>(&QSpinBox::valueChanged), this,
            [this, refreshSpeechFilterRouting](int value) {
                m_speechFilterFadeSamples = qMax(0, value);
                refreshSpeechFilterRouting(true);
            });
    connect(m_speechFilterRangeCrossfadeCheckBox, &QCheckBox::toggled, this,
            [this, refreshSpeechFilterRouting](bool checked) {
                m_speechFilterRangeCrossfade = checked;
                refreshSpeechFilterRouting(true);
            });
    if (m_playbackClockSourceCombo) {
        connect(m_playbackClockSourceCombo, &QComboBox::currentIndexChanged, this, [this](int index) {
            if (index < 0) {
                return;
            }
            const PlaybackClockSource source = playbackClockSourceFromString(
                m_playbackClockSourceCombo->itemData(index).toString());
            setPlaybackClockSource(source);
        });
    }
    if (m_playbackAudioWarpModeCombo) {
        connect(m_playbackAudioWarpModeCombo, &QComboBox::currentIndexChanged, this, [this](int index) {
            if (index < 0) {
                return;
            }
            const PlaybackAudioWarpMode mode = playbackAudioWarpModeFromString(
                m_playbackAudioWarpModeCombo->itemData(index).toString());
            setPlaybackAudioWarpMode(mode);
        });
    }

    connect(m_clipPlaybackRateSpin, qOverload<double>(&QDoubleSpinBox::valueChanged), this, [this](double value) {
        if (!m_timeline || !m_clipPlaybackRateSpin) {
            return;
        }
        const TimelineClip *clip = m_timeline->selectedClip();
        if (!clip || !clipHasVisuals(*clip)) {
            return;
        }
        const qreal playbackRate = qBound<qreal>(0.001, value, 4.0);
        if (qFuzzyCompare(clip->playbackRate, playbackRate)) {
            return;
        }
        if (!m_timeline->updateClipById(clip->id, [playbackRate](TimelineClip &editableClip) {
                editableClip.playbackRate = playbackRate;
                normalizeClipTiming(editableClip);
            })) {
            return;
        }
        if (m_preview) {
            m_preview->setTimelineClips(m_timeline->clips());
        }
        m_inspectorPane->refresh();
        scheduleSaveState();
        pushHistorySnapshot();
    });
}

void EditorWindow::setupTrackInspectorControls()
{
    connect(m_trackNameEdit, &QLineEdit::editingFinished, this, [this]() {
        if (!m_timeline || !m_trackNameEdit) {
            return;
        }
        const int trackIndex = m_timeline->selectedTrackIndex();
        const TimelineTrack *track = m_timeline->selectedTrack();
        if (trackIndex < 0 || !track) {
            return;
        }
        const QString nextName = m_trackNameEdit->text().trimmed().isEmpty()
            ? QStringLiteral("Track %1").arg(trackIndex + 1)
            : m_trackNameEdit->text().trimmed();
        if (track->name == nextName) {
            return;
        }
        if (!m_timeline->updateTrackByIndex(trackIndex, [nextName](TimelineTrack &editableTrack) {
                editableTrack.name = nextName;
            })) {
            return;
        }
        refreshClipInspector();
    });

    connect(m_trackHeightSpin, qOverload<int>(&QSpinBox::valueChanged), this, [this](int value) {
        if (!m_timeline || !m_trackHeightSpin) {
            return;
        }
        const int trackIndex = m_timeline->selectedTrackIndex();
        const TimelineTrack *track = m_timeline->selectedTrack();
        if (trackIndex < 0 || !track || track->height == value) {
            return;
        }
        if (!m_timeline->updateTrackByIndex(trackIndex, [value](TimelineTrack &editableTrack) {
                editableTrack.height = value;
            })) {
            return;
        }
        refreshClipInspector();
    });

    connect(m_trackVisualModeCombo, &QComboBox::currentIndexChanged, this, [this](int) {
        if (!m_timeline || !m_trackVisualModeCombo) {
            return;
        }
        const int trackIndex = m_timeline->selectedTrackIndex();
        if (trackIndex < 0) {
            return;
        }
        const TrackVisualMode mode = static_cast<TrackVisualMode>(
            m_trackVisualModeCombo->currentData().toInt());
        if (m_timeline->updateTrackVisualMode(trackIndex, mode)) {
            refreshClipInspector();
        }
    });

    connect(m_trackAudioEnabledCheckBox, &QCheckBox::toggled, this, [this](bool checked) {
        if (!m_timeline || !m_trackAudioEnabledCheckBox) {
            return;
        }
        const int trackIndex = m_timeline->selectedTrackIndex();
        if (trackIndex < 0) {
            return;
        }
        if (m_timeline->updateTrackAudioEnabled(trackIndex, checked)) {
            refreshClipInspector();
        }
    });

    connect(m_trackCrossfadeButton, &QPushButton::clicked, this, [this]() {
        if (!m_timeline || !m_trackCrossfadeButton || !m_trackCrossfadeSecondsSpin) {
            return;
        }
        const int trackIndex = m_timeline->selectedTrackIndex();
        if (trackIndex < 0) {
            return;
        }
        if (m_timeline->crossfadeTrack(trackIndex, m_trackCrossfadeSecondsSpin->value())) {
            refreshClipInspector();
        }
    });
}

void EditorWindow::setupPreviewControls()
{
    const auto persistPreviewBufferingSettings = [this]() {
        scheduleSaveState();
    };

    if (m_bypassGradingCheckBox) {
        connect(m_bypassGradingCheckBox, &QCheckBox::toggled, this, [this](bool checked) {
            if (m_preview) {
                m_preview->setBypassGrading(!checked);
            }
            updateTransportLabels();
            scheduleSaveState();
            pushHistorySnapshot();
        });
        if (m_preview) {
            m_preview->setBypassGrading(!m_bypassGradingCheckBox->isChecked());
        }
    }

    connect(m_previewHideOutsideOutputCheckBox, &QCheckBox::toggled, this, [this](bool checked) {
        if (m_preview) {
            m_preview->setHideOutsideOutputWindow(checked);
        }
        scheduleSaveState();
        pushHistorySnapshot();
    });

    if (m_previewShowSpeakerTrackPointsCheckBox) {
        connect(m_previewShowSpeakerTrackPointsCheckBox, &QCheckBox::toggled, this, [this](bool checked) {
            if (m_preview) {
                m_preview->setShowSpeakerTrackPoints(checked);
            }
            scheduleSaveState();
            pushHistorySnapshot();
        });
    }
    if (m_speakerShowBoxStreamBoxesCheckBox) {
        connect(m_speakerShowBoxStreamBoxesCheckBox, &QCheckBox::toggled, this, [this](bool checked) {
            if (m_preview) {
                m_preview->setShowSpeakerTrackBoxes(checked);
            }
            scheduleSaveState();
            pushHistorySnapshot();
        });
    }

    // Preview zoom controls
    if (m_previewZoomSpin) {
        connect(m_previewZoomSpin, qOverload<double>(&QDoubleSpinBox::valueChanged), this, [this](double value) {
            if (m_preview) {
                m_preview->setPreviewZoom(value);
            }
        });
    }
    
    if (m_previewZoomResetButton) {
        connect(m_previewZoomResetButton, &QPushButton::clicked, this, [this]() {
            if (m_preview) {
                m_preview->setPreviewZoom(1.0);
                m_preview->resetPreviewPan();
                if (m_previewZoomSpin) {
                    QSignalBlocker block(m_previewZoomSpin);
                    m_previewZoomSpin->setValue(1.0);
                }
            }
            scheduleSaveState();
        });
    }

    if (m_previewPlaybackCacheFallbackCheckBox) {
        connect(m_previewPlaybackCacheFallbackCheckBox, &QCheckBox::toggled, this, [persistPreviewBufferingSettings](bool checked) {
            editor::setDebugPlaybackCacheFallbackEnabled(checked);
            persistPreviewBufferingSettings();
        });
    }

    if (m_previewLeadPrefetchEnabledCheckBox) {
        connect(m_previewLeadPrefetchEnabledCheckBox, &QCheckBox::toggled, this, [this, persistPreviewBufferingSettings](bool checked) {
            editor::setDebugLeadPrefetchEnabled(checked);
            if (m_previewLeadPrefetchCountSpin) {
                m_previewLeadPrefetchCountSpin->setEnabled(checked);
            }
            persistPreviewBufferingSettings();
        });
    }

    if (m_previewLeadPrefetchCountSpin) {
        connect(m_previewLeadPrefetchCountSpin, qOverload<int>(&QSpinBox::valueChanged), this, [persistPreviewBufferingSettings](int value) {
            editor::setDebugLeadPrefetchCount(value);
            persistPreviewBufferingSettings();
        });
    }

    if (m_previewPlaybackWindowAheadSpin) {
        connect(m_previewPlaybackWindowAheadSpin, qOverload<int>(&QSpinBox::valueChanged), this, [persistPreviewBufferingSettings](int value) {
            editor::setDebugPlaybackWindowAhead(value);
            persistPreviewBufferingSettings();
        });
    }

    if (m_previewVisibleQueueReserveSpin) {
        connect(m_previewVisibleQueueReserveSpin, qOverload<int>(&QSpinBox::valueChanged), this, [persistPreviewBufferingSettings](int value) {
            editor::setDebugVisibleQueueReserve(value);
            persistPreviewBufferingSettings();
        });
    }

    if (m_previewLeadPrefetchCountSpin && m_previewLeadPrefetchEnabledCheckBox) {
        m_previewLeadPrefetchCountSpin->setEnabled(m_previewLeadPrefetchEnabledCheckBox->isChecked());
    }

    connect(m_inspectorPane->backgroundColorButton(), &QPushButton::clicked, this, [this]() {
        const QColor chosen = QColorDialog::getColor(m_backgroundColor, this, QStringLiteral("Background Color"));
        if (!chosen.isValid()) {
            return;
        }
        m_backgroundColor = chosen;
        auto *btn = m_inspectorPane->backgroundColorButton();
        btn->setText(chosen.name());
        btn->setStyleSheet(
            QStringLiteral("QPushButton { background: %1; color: %2; "
                           "border: 1px solid #2e3b4a; border-radius: 4px; padding: 4px 8px; }")
                .arg(chosen.name(),
                     chosen.lightness() > 128 ? QStringLiteral("#000000")
                                              : QStringLiteral("#ffffff")));
        if (m_preview) {
            m_preview->setBackgroundColor(m_backgroundColor);
        }
        scheduleSaveState();
    });

    if (m_inspectorPane->restartDecodersButton()) {
        connect(m_inspectorPane->restartDecodersButton(), &QPushButton::clicked,
                this, &EditorWindow::onRestartDecodersRequested);
    }
}
