#include "editor.h"
#include "background_fill_effect.h"

#include <QCheckBox>
#include <QComboBox>
#include <QColorDialog>
#include <QDoubleSpinBox>
#include <QFormLayout>
#include <QLineEdit>
#include <QList>
#include <QMessageBox>
#include <QPushButton>
#include <QSlider>
#include <QSignalBlocker>
#include <QShortcut>
#include <QSpinBox>
#include <QTableWidget>

#include "debug_controls.h"
#include "transform_skip_aware_timing.h"

using namespace editor;

namespace {

void setColorButtonSwatch(QPushButton* button, const QColor& color)
{
    if (!button || !color.isValid()) {
        return;
    }
    const QColor opaque(color.red(), color.green(), color.blue());
    button->setText(opaque.name());
    button->setStyleSheet(
        QStringLiteral("QPushButton { background: %1; color: %2; "
                       "border: 1px solid #2e3b4a; border-radius: 4px; padding: 3px 8px; }")
            .arg(opaque.name(),
                 opaque.lightness() > 128 ? QStringLiteral("#000000")
                                          : QStringLiteral("#ffffff")));
}

} // namespace

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
    m_previewVulkanPresenterCombo = m_inspectorPane->previewVulkanPresenterCombo();
    m_speakerShowContiguousSectionsCheckBox =
        m_inspectorPane->speakerShowContiguousSectionsCheckBox();
    m_speakerApplyTrackToAllMatchingSectionsCheckBox =
        m_inspectorPane->speakerApplyTrackToAllMatchingSectionsCheckBox();
    m_speakerShowFaceDetectionsBoxesCheckBox = m_inspectorPane->speakerShowFaceDetectionsBoxesCheckBox();
    m_speakerShowRawDetectionsCheckBox = m_inspectorPane->speakerShowRawDetectionsCheckBox();
    m_speakerShowCurrentSpeakerNameCheckBox = m_inspectorPane->speakerShowCurrentSpeakerNameCheckBox();
    m_speakerShowCurrentSpeakerOrganizationCheckBox =
        m_inspectorPane->speakerShowCurrentSpeakerOrganizationCheckBox();
    m_speakerCurrentSpeakerNameTextSizeSpin = m_inspectorPane->speakerCurrentSpeakerNameTextSizeSpin();
    m_speakerCurrentSpeakerOrganizationTextSizeSpin =
        m_inspectorPane->speakerCurrentSpeakerOrganizationTextSizeSpin();
    m_speakerCurrentSpeakerNameYPositionSpin =
        m_inspectorPane->speakerCurrentSpeakerNameYPositionSpin();
    m_speakerCurrentSpeakerOrganizationYPositionSpin =
        m_inspectorPane->speakerCurrentSpeakerOrganizationYPositionSpin();
    m_speakerCurrentSpeakerNameColorButton = m_inspectorPane->speakerCurrentSpeakerNameColorButton();
    m_speakerCurrentSpeakerOrganizationColorButton =
        m_inspectorPane->speakerCurrentSpeakerOrganizationColorButton();
    m_speakerCurrentSpeakerBackgroundColorButton =
        m_inspectorPane->speakerCurrentSpeakerBackgroundColorButton();
    m_speakerCurrentSpeakerBackgroundVisibleCheckBox =
        m_inspectorPane->speakerCurrentSpeakerBackgroundVisibleCheckBox();
    m_speakerCurrentSpeakerBackgroundOpacitySpin =
        m_inspectorPane->speakerCurrentSpeakerBackgroundOpacitySpin();
    m_speakerCurrentSpeakerBorderColorButton =
        m_inspectorPane->speakerCurrentSpeakerBorderColorButton();
    m_speakerCurrentSpeakerBorderOpacitySpin =
        m_inspectorPane->speakerCurrentSpeakerBorderOpacitySpin();
    m_speakerCurrentSpeakerBackgroundRadiusSpin =
        m_inspectorPane->speakerCurrentSpeakerBackgroundRadiusSpin();
    m_speakerCurrentSpeakerBorderWidthSpin =
        m_inspectorPane->speakerCurrentSpeakerBorderWidthSpin();
    m_speakerCurrentSpeakerShadowCheckBox =
        m_inspectorPane->speakerCurrentSpeakerShadowCheckBox();
    m_speakerCurrentSpeakerShadowColorButton =
        m_inspectorPane->speakerCurrentSpeakerShadowColorButton();
    m_speakerCurrentSpeakerShadowOpacitySpin =
        m_inspectorPane->speakerCurrentSpeakerShadowOpacitySpin();
    m_previewZoomSpin = m_inspectorPane->previewZoomSpin();
    m_previewZoomResetButton = m_inspectorPane->previewZoomResetButton();
    m_previewPlaybackCacheFallbackCheckBox = m_inspectorPane->previewPlaybackCacheFallbackCheckBox();
    m_previewLeadPrefetchEnabledCheckBox = m_inspectorPane->previewLeadPrefetchEnabledCheckBox();
    m_previewLeadPrefetchCountSpin = m_inspectorPane->previewLeadPrefetchCountSpin();
    m_previewPlaybackWindowAheadSpin = m_inspectorPane->previewPlaybackWindowAheadSpin();
    m_previewVisibleQueueReserveSpin = m_inspectorPane->previewVisibleQueueReserveSpin();
    m_timelineAudioEnvelopeGranularitySpin = m_inspectorPane->timelineAudioEnvelopeGranularitySpin();
    m_preferencesFeatureAiPanelCheckBox = m_inspectorPane->preferencesFeatureAiPanelCheckBox();
    m_preferencesFeatureAiSpeakerCleanupCheckBox =
        m_inspectorPane->preferencesFeatureAiSpeakerCleanupCheckBox();
    m_preferencesFeatureAudioPreviewModeCheckBox =
        m_inspectorPane->preferencesFeatureAudioPreviewModeCheckBox();
    m_preferencesFeatureAudioDynamicsToolsCheckBox =
        m_inspectorPane->preferencesFeatureAudioDynamicsToolsCheckBox();
    m_audioAmplifyEnabledCheckBox = m_inspectorPane->audioAmplifyEnabledCheckBox();
    m_audioAmplifyDbSpin = m_inspectorPane->audioAmplifyDbSpin();
    m_audioSpeakerHoverModalCheckBox = m_inspectorPane->audioSpeakerHoverModalCheckBox();
    m_audioShowWaveformCheckBox = m_inspectorPane->audioShowWaveformCheckBox();
    m_audioVisualizationModeCombo = m_inspectorPane->audioVisualizationModeCombo();
    m_loiaconoSpectrumSettingsButton = m_inspectorPane->loiaconoSpectrumSettingsButton();
    m_audioWaveformPreviewProcessedCheckBox = m_inspectorPane->audioWaveformPreviewProcessedCheckBox();
    m_audioNormalizeEnabledCheckBox = m_inspectorPane->audioNormalizeEnabledCheckBox();
    m_audioNormalizeTargetDbSpin = m_inspectorPane->audioNormalizeTargetDbSpin();
    m_audioStereoToMonoCheckBox = m_inspectorPane->audioStereoToMonoCheckBox();
    m_audioSelectiveNormalizeEnabledCheckBox = m_inspectorPane->audioSelectiveNormalizeEnabledCheckBox();
    m_audioSelectiveNormalizeMinSecondsSpin = m_inspectorPane->audioSelectiveNormalizeMinSecondsSpin();
    m_audioSelectiveNormalizePeakDbSpin = m_inspectorPane->audioSelectiveNormalizePeakDbSpin();
    m_audioSelectiveNormalizePassesSpin = m_inspectorPane->audioSelectiveNormalizePassesSpin();
    m_audioSelectiveNormalizeOverlayVisibleCheckBox =
        m_inspectorPane->audioSelectiveNormalizeOverlayVisibleCheckBox();
    m_audioTranscriptNormalizeEnabledCheckBox = m_inspectorPane->audioTranscriptNormalizeEnabledCheckBox();
    m_audioPeakReductionEnabledCheckBox = m_inspectorPane->audioPeakReductionEnabledCheckBox();
    m_audioPeakThresholdDbSpin = m_inspectorPane->audioPeakThresholdDbSpin();
    m_audioLimiterEnabledCheckBox = m_inspectorPane->audioLimiterEnabledCheckBox();
    m_audioLimiterThresholdDbSpin = m_inspectorPane->audioLimiterThresholdDbSpin();
    m_audioCompressorEnabledCheckBox = m_inspectorPane->audioCompressorEnabledCheckBox();
    m_audioCompressorThresholdDbSpin = m_inspectorPane->audioCompressorThresholdDbSpin();
    m_audioCompressorRatioSpin = m_inspectorPane->audioCompressorRatioSpin();
    m_audioSoftClipEnabledCheckBox = m_inspectorPane->audioSoftClipEnabledCheckBox();
    m_transcriptOverlayEnabledCheckBox = m_inspectorPane->transcriptOverlayEnabledCheckBox();
    m_transcriptPlacementModeCombo = m_inspectorPane->transcriptPlacementModeCombo();
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
    m_audioCurrentSpeakerTitleLabel = m_inspectorPane->audioCurrentSpeakerTitleLabel();
    m_audioCurrentSpeakerDetailsLabel = m_inspectorPane->audioCurrentSpeakerDetailsLabel();
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
    m_outputFpsSpin = m_inspectorPane->outputFpsSpin();
    m_exportStartSpin = m_inspectorPane->exportStartSpin();
    m_exportEndSpin = m_inspectorPane->exportEndSpin();
    m_outputFormatCombo = m_inspectorPane->outputFormatCombo();
    m_renderBackendCombo = m_inspectorPane->renderBackendCombo();
    m_backgroundFillEffectCombo = m_inspectorPane->backgroundFillEffectCombo();
    m_backgroundFillOpacitySpin = m_inspectorPane->backgroundFillOpacitySpin();
    m_backgroundFillBrightnessSpin = m_inspectorPane->backgroundFillBrightnessSpin();
    m_backgroundFillSaturationSpin = m_inspectorPane->backgroundFillSaturationSpin();
    m_backgroundFillEdgePixelsSlider = m_inspectorPane->backgroundFillEdgePixelsSlider();
    m_backgroundFillEdgeProgressiveCheckBox = m_inspectorPane->backgroundFillEdgeProgressiveCheckBox();
    m_backgroundFillEdgePowerSpin = m_inspectorPane->backgroundFillEdgePowerSpin();
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
    m_transcriptOffsetMsSpin = m_inspectorPane->transcriptOffsetMsSpin();
    m_speechFilterFadeModeCombo = m_inspectorPane->speechFilterFadeModeCombo();
    m_speechFilterFadeSamplesSpin = m_inspectorPane->speechFilterFadeSamplesSpin();
    m_speechFilterCurveStrengthSpin = m_inspectorPane->speechFilterCurveStrengthSpin();
    m_speechFilterRangeCrossfadeCheckBox = m_inspectorPane->speechFilterRangeCrossfadeCheckBox();
    m_speechFilterFrameTransitionModeCombo = m_inspectorPane->speechFilterFrameTransitionModeCombo();
    m_speechFilterFrameCrossfadeCheckBox = m_inspectorPane->speechFilterFrameCrossfadeCheckBox();
    m_speechFilterFrameCrossfadeFramesSpin = m_inspectorPane->speechFilterFrameCrossfadeFramesSpin();
    m_playbackClockSourceCombo = m_inspectorPane->playbackClockSourceCombo();
    m_playbackAudioWarpModeCombo = m_inspectorPane->playbackAudioWarpModeCombo();
    m_aiStatusLabel = m_inspectorPane->aiStatusLabel();
    m_aiModelCombo = m_inspectorPane->aiModelCombo();
    m_aiTranscribeButton = m_inspectorPane->aiTranscribeButton();
    m_aiFindSpeakerNamesButton = m_inspectorPane->aiFindSpeakerNamesButton();
    m_aiFindOrganizationsButton = m_inspectorPane->aiFindOrganizationsButton();
    m_aiCleanAssignmentsButton = m_inspectorPane->aiCleanAssignmentsButton();
    m_aiSubscribeButton = m_inspectorPane->aiSubscribeButton();
    m_aiChatHistoryEdit = m_inspectorPane->aiChatHistoryEdit();
    m_aiChatInputLineEdit = m_inspectorPane->aiChatInputLineEdit();
    m_aiChatSendButton = m_inspectorPane->aiChatSendButton();
    m_aiChatClearButton = m_inspectorPane->aiChatClearButton();
    m_accessStatusLabel = m_inspectorPane->accessStatusLabel();
    m_accessTable = m_inspectorPane->accessTable();
    m_accessRefreshButton = m_inspectorPane->accessRefreshButton();

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
    if (m_aiSubscribeButton) {
        connect(m_aiSubscribeButton, &QPushButton::clicked, this, [this]() { runAiSubscribeCheckout(); });
    }
    if (m_aiChatSendButton) {
        connect(m_aiChatSendButton, &QPushButton::clicked, this, [this]() { runAiChatPrompt(); });
    }
    if (m_aiChatInputLineEdit) {
        auto* sendShortcut = new QShortcut(QKeySequence(QStringLiteral("Ctrl+Return")), m_aiChatInputLineEdit);
        connect(sendShortcut, &QShortcut::activated, this, [this]() { runAiChatPrompt(); });
        auto* sendShortcut2 = new QShortcut(QKeySequence(QStringLiteral("Ctrl+Enter")), m_aiChatInputLineEdit);
        connect(sendShortcut2, &QShortcut::activated, this, [this]() { runAiChatPrompt(); });
    }
    if (m_aiChatClearButton && m_aiChatHistoryEdit) {
        connect(m_aiChatClearButton, &QPushButton::clicked, this, [this]() {
            m_aiChatHistoryEdit->clear();
            m_aiChatMessages.clear();
            m_aiChatClearButton->setEnabled(false);
        });
    }
    if (m_accessRefreshButton) {
        connect(m_accessRefreshButton, &QPushButton::clicked, this, [this]() { refreshAccessTabData(); });
    }

    if (m_preferencesFeatureAiPanelCheckBox) {
        connect(m_preferencesFeatureAiPanelCheckBox, &QCheckBox::toggled, this, [this](bool checked) {
            m_featureAiPanel = checked;
            refreshAiIntegrationState();
            scheduleSaveState();
        });
    }
    if (m_preferencesFeatureAiSpeakerCleanupCheckBox) {
        connect(m_preferencesFeatureAiSpeakerCleanupCheckBox, &QCheckBox::toggled, this, [this](bool checked) {
            m_featureAiSpeakerCleanup = checked;
            refreshAiIntegrationState();
            scheduleSaveState();
        });
    }
    if (m_preferencesFeatureAudioPreviewModeCheckBox) {
        connect(m_preferencesFeatureAudioPreviewModeCheckBox, &QCheckBox::toggled, this, [this](bool checked) {
            m_featureAudioPreviewMode = checked;
            if (!checked) {
                applyPreviewViewMode(QStringLiteral("video"));
            } else {
                applyPreviewViewMode(m_previewViewMode);
            }
            if (m_previewModeCombo) {
                m_previewModeCombo->setEnabled(m_featureAudioPreviewMode);
                m_previewModeCombo->setToolTip(m_featureAudioPreviewMode
                                                   ? QStringLiteral("Switch preview between video composition and audio waveform view.")
                                                   : QStringLiteral("Audio preview mode disabled by feature flag."));
            }
            scheduleSaveState();
        });
    }
    if (m_preferencesFeatureAudioDynamicsToolsCheckBox) {
        connect(m_preferencesFeatureAudioDynamicsToolsCheckBox, &QCheckBox::toggled, this, [this](bool checked) {
            m_featureAudioDynamicsTools = checked;
            if (m_audioToolsButton) {
                m_audioToolsButton->setEnabled(m_featureAudioDynamicsTools);
                m_audioToolsButton->setToolTip(m_featureAudioDynamicsTools
                                                   ? QStringLiteral("Open the Audio tab.")
                                                   : QStringLiteral("Audio dynamics tools disabled by feature flag."));
            }
            const QList<QWidget*> controls{
                m_audioAmplifyEnabledCheckBox, m_audioAmplifyDbSpin, m_audioNormalizeEnabledCheckBox,
                m_audioNormalizeTargetDbSpin, m_audioStereoToMonoCheckBox,
                m_audioSelectiveNormalizeEnabledCheckBox,
                m_audioSelectiveNormalizeMinSecondsSpin, m_audioSelectiveNormalizePeakDbSpin,
                m_audioSelectiveNormalizePassesSpin, m_audioWaveformPreviewProcessedCheckBox,
                m_audioPeakReductionEnabledCheckBox, m_audioPeakThresholdDbSpin,
                m_audioLimiterEnabledCheckBox, m_audioLimiterThresholdDbSpin, m_audioCompressorEnabledCheckBox,
                m_audioCompressorThresholdDbSpin, m_audioCompressorRatioSpin, m_audioSoftClipEnabledCheckBox};
            for (QWidget* control : controls) {
                if (control) {
                    control->setEnabled(m_featureAudioDynamicsTools);
                }
            }
            scheduleSaveState();
        });
    }
}

void EditorWindow::setupSpeechFilterControls()
{
    const auto refreshSpeechFilterRouting = [this](bool pushHistory = false) {
        const QVector<ExportRangeSegment> ranges = effectivePlaybackRanges();
        if (m_preview) {
            m_preview->setPlaybackTimingContext(speechFilterPlaybackTimingContext(ranges));
            m_preview->setExportRanges(ranges);
        }
        if (m_audioEngine) {
            m_audioEngine->setExportRanges(ranges);
            m_audioEngine->setTranscriptNormalizeRanges(
                m_previewAudioDynamics.transcriptNormalizeEnabled
                    ? effectiveTranscriptNormalizeRanges()
                    : QVector<ExportRangeSegment>{});
            m_audioEngine->setSpeechFilterFadeSamples(m_speechFilterFadeSamples);
            m_audioEngine->setSpeechFilterFadeMode(m_speechFilterFadeMode);
            m_audioEngine->setSpeechFilterCurveStrength(m_speechFilterCurveStrength);
            m_audioEngine->setSpeechFilterRangeCrossfadeEnabled(m_speechFilterRangeCrossfade);
            m_audioEngine->setPlaybackWarpMode(m_playbackAudioWarpMode);
            m_audioEngine->setPlaybackRate(effectiveAudioWarpRate());
            m_audioEngine->setTranscriptNormalizeEnabled(
                m_previewAudioDynamics.transcriptNormalizeEnabled);
            m_audioEngine->setAudioDynamicsSettings(m_previewAudioDynamics);
        }
        m_inspectorPane->refreshTab(QStringLiteral("Transcript"));
        scheduleSaveState();
        if (pushHistory) pushHistorySnapshot();
    };

    refreshSpeechFilterFadeParameterVisibility();
    if (m_speechFilterFadeModeCombo) {
        connect(m_speechFilterFadeModeCombo, &QComboBox::currentIndexChanged, this,
                [this, refreshSpeechFilterRouting](int index) {
                    if (index < 0 || !m_speechFilterFadeModeCombo) {
                        return;
                    }
                    const QString mode = m_speechFilterFadeModeCombo->itemData(index).toString();
                    m_speechFilterEnabled = mode != QStringLiteral("none");
                    if (m_speechFilterEnabled) {
                        m_speechFilterFadeMode =
                            AudioEngine::speechFilterFadeModeFromString(mode);
                    }
                    m_speechFilterRangeCrossfade = false;
                    refreshSpeechFilterFadeParameterVisibility();
                    refreshSpeechFilterRouting(true);
                });
    }

    connect(m_speechFilterRangeCrossfadeCheckBox, &QCheckBox::toggled, this,
            [this, refreshSpeechFilterRouting](bool checked) {
                m_speechFilterRangeCrossfade = checked;
                refreshSpeechFilterRouting(true);
            });
    if (m_speechFilterFrameTransitionModeCombo) {
        connect(m_speechFilterFrameTransitionModeCombo, &QComboBox::currentIndexChanged, this,
                [this, refreshSpeechFilterRouting](int index) {
                    if (index < 0 || !m_speechFilterFrameTransitionModeCombo) {
                        return;
                    }
                    m_speechFilterFrameTransitionMode =
                        playbackFrameTransitionModeFromString(
                            m_speechFilterFrameTransitionModeCombo->itemData(index).toString());
                    m_speechFilterFrameCrossfadeEnabled =
                        m_speechFilterFrameTransitionMode == PlaybackFrameTransitionMode::Crossfade;
                    refreshSpeechFilterFadeParameterVisibility();
                    refreshSpeechFilterRouting(true);
                });
    }
    if (m_speechFilterFrameCrossfadeCheckBox) {
        connect(m_speechFilterFrameCrossfadeCheckBox, &QCheckBox::toggled, this,
                [this, refreshSpeechFilterRouting](bool checked) {
                    m_speechFilterFrameCrossfadeEnabled = checked;
                    m_speechFilterFrameTransitionMode =
                        checked ? PlaybackFrameTransitionMode::Crossfade
                                : PlaybackFrameTransitionMode::Cut;
                    refreshSpeechFilterRouting(true);
                });
    }
    if (m_speechFilterFrameCrossfadeFramesSpin) {
        connect(m_speechFilterFrameCrossfadeFramesSpin, qOverload<int>(&QSpinBox::valueChanged),
                this, [this, refreshSpeechFilterRouting](int value) {
                    m_speechFilterFrameCrossfadeFrames = qBound(0, value, 240);
                    refreshSpeechFilterRouting(true);
                });
    }
    if (m_speechFilterCurveStrengthSpin) {
        connect(m_speechFilterCurveStrengthSpin, qOverload<double>(&QDoubleSpinBox::valueChanged),
                this, [this, refreshSpeechFilterRouting](double value) {
                    m_speechFilterCurveStrength = qBound<qreal>(0.25, value, 4.0);
                    refreshSpeechFilterRouting(true);
                });
    }
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
        m_inspectorPane->refreshTab(QStringLiteral("Properties"));
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
    const auto applyAudioDynamicsFromInspector = [this]() {
        if (!m_audioAmplifyEnabledCheckBox || !m_audioAmplifyDbSpin ||
            !m_audioNormalizeEnabledCheckBox || !m_audioNormalizeTargetDbSpin ||
            !m_audioStereoToMonoCheckBox ||
            !m_audioSelectiveNormalizeEnabledCheckBox || !m_audioSelectiveNormalizeMinSecondsSpin ||
            !m_audioSelectiveNormalizePassesSpin || !m_audioSelectiveNormalizeOverlayVisibleCheckBox ||
            !m_audioTranscriptNormalizeEnabledCheckBox ||
            !m_audioPeakReductionEnabledCheckBox || !m_audioPeakThresholdDbSpin ||
            !m_audioLimiterEnabledCheckBox || !m_audioLimiterThresholdDbSpin ||
            !m_audioCompressorEnabledCheckBox || !m_audioCompressorThresholdDbSpin ||
            !m_audioCompressorRatioSpin || !m_audioSoftClipEnabledCheckBox ||
            !m_audioWaveformPreviewProcessedCheckBox) {
            return;
        }
        m_previewAudioDynamics.amplifyEnabled = m_audioAmplifyEnabledCheckBox->isChecked();
        m_previewAudioDynamics.amplifyDb = m_audioAmplifyDbSpin->value();
        m_previewAudioDynamics.normalizeEnabled = m_audioNormalizeEnabledCheckBox->isChecked();
        m_previewAudioDynamics.normalizeTargetDb = m_audioNormalizeTargetDbSpin->value();
        m_previewAudioDynamics.stereoToMonoEnabled = m_audioStereoToMonoCheckBox->isChecked();
        m_previewAudioDynamics.selectiveNormalizeEnabled = m_audioSelectiveNormalizeEnabledCheckBox->isChecked();
        m_previewAudioDynamics.selectiveNormalizeMinSegmentSeconds =
            m_audioSelectiveNormalizeMinSecondsSpin->value();
        m_previewAudioDynamics.selectiveNormalizePeakDb =
            m_audioSelectiveNormalizePeakDbSpin ? m_audioSelectiveNormalizePeakDbSpin->value() : -12.0;
        m_previewAudioDynamics.selectiveNormalizePasses =
            m_audioSelectiveNormalizePassesSpin->value();
        m_previewAudioDynamics.selectiveNormalizeOverlayVisible =
            m_audioSelectiveNormalizeOverlayVisibleCheckBox->isChecked();
        m_previewAudioDynamics.transcriptNormalizeEnabled =
            m_audioTranscriptNormalizeEnabledCheckBox->isChecked();
        m_previewAudioDynamics.peakReductionEnabled = m_audioPeakReductionEnabledCheckBox->isChecked();
        m_previewAudioDynamics.peakThresholdDb = m_audioPeakThresholdDbSpin->value();
        m_previewAudioDynamics.limiterEnabled = m_audioLimiterEnabledCheckBox->isChecked();
        m_previewAudioDynamics.limiterThresholdDb = m_audioLimiterThresholdDbSpin->value();
        m_previewAudioDynamics.compressorEnabled = m_audioCompressorEnabledCheckBox->isChecked();
        m_previewAudioDynamics.compressorThresholdDb = m_audioCompressorThresholdDbSpin->value();
        m_previewAudioDynamics.compressorRatio = m_audioCompressorRatioSpin->value();
        m_previewAudioDynamics.softClipEnabled = m_audioSoftClipEnabledCheckBox->isChecked();
        m_previewAudioDynamics.waveformPreviewPostProcessing =
            m_audioWaveformPreviewProcessedCheckBox->isChecked();
        if (m_preview) {
            m_preview->setAudioDynamicsSettings(m_previewAudioDynamics);
        }
        if (m_audioEngine) {
            m_audioEngine->setTranscriptNormalizeEnabled(
                m_previewAudioDynamics.transcriptNormalizeEnabled);
            m_audioEngine->setAudioDynamicsSettings(m_previewAudioDynamics);
            scheduleTranscriptNormalizeRangeRefresh(0);
        }
        scheduleSaveState();
    };

    if (m_bypassGradingCheckBox) {
        connect(m_bypassGradingCheckBox, &QCheckBox::toggled, this, [this](bool checked) {
            if (!m_timeline) {
                return;
            }
            const QString clipId = m_timeline->selectedClipId();
            if (clipId.isEmpty()) return;
            m_timeline->updateClipById(clipId, [checked](TimelineClip& clip) {
                clip.gradingPreviewEnabled = checked;
            });
            if (m_preview) m_preview->setTimelineClips(m_timeline->clips());
            updateTransportLabels();
            scheduleSaveState();
            pushHistorySnapshot();
        });
        if (m_preview) m_preview->setBypassGrading(false);
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
    if (m_speakerShowContiguousSectionsCheckBox) {
        connect(m_speakerShowContiguousSectionsCheckBox, &QCheckBox::toggled, this, [this]() {
            scheduleSaveState();
            pushHistorySnapshot();
        });
    }
    if (m_speakerApplyTrackToAllMatchingSectionsCheckBox) {
        connect(m_speakerApplyTrackToAllMatchingSectionsCheckBox, &QCheckBox::toggled, this, [this]() {
            scheduleSaveState();
            pushHistorySnapshot();
        });
    }
    if (m_speakerShowFaceDetectionsBoxesCheckBox) {
        connect(m_speakerShowFaceDetectionsBoxesCheckBox, &QCheckBox::toggled, this, [this](bool checked) {
            if (m_preview) {
                m_preview->setShowSpeakerTrackBoxes(checked);
            }
            scheduleSaveState();
            pushHistorySnapshot();
        });
    }
    if (m_speakerShowRawDetectionsCheckBox) {
        connect(m_speakerShowRawDetectionsCheckBox, &QCheckBox::toggled, this, [this](bool checked) {
            if (m_preview) {
                m_preview->setShowRawDetections(checked);
            }
            scheduleSaveState();
            pushHistorySnapshot();
        });
    }
    if (m_speakerShowCurrentSpeakerNameCheckBox) {
        connect(m_speakerShowCurrentSpeakerNameCheckBox, &QCheckBox::toggled, this, [this](bool checked) {
            if (m_preview) {
                m_preview->setShowCurrentSpeakerName(checked);
            }
            scheduleSaveState();
            pushHistorySnapshot();
        });
    }
    if (m_speakerShowCurrentSpeakerOrganizationCheckBox) {
        connect(m_speakerShowCurrentSpeakerOrganizationCheckBox, &QCheckBox::toggled, this, [this](bool checked) {
            if (m_preview) {
                m_preview->setShowCurrentSpeakerOrganization(checked);
            }
            scheduleSaveState();
            pushHistorySnapshot();
        });
    }
    if (m_speakerCurrentSpeakerNameTextSizeSpin) {
        connect(m_speakerCurrentSpeakerNameTextSizeSpin,
                qOverload<int>(&QSpinBox::valueChanged),
                this,
                [this](int value) {
                    if (m_preview) {
                        m_preview->setCurrentSpeakerNameTextScale(value / 100.0);
                    }
                    scheduleSaveState();
                    pushHistorySnapshot();
                });
    }
    if (m_speakerCurrentSpeakerOrganizationTextSizeSpin) {
        connect(m_speakerCurrentSpeakerOrganizationTextSizeSpin,
                qOverload<int>(&QSpinBox::valueChanged),
                this,
                [this](int value) {
                    if (m_preview) {
                        m_preview->setCurrentSpeakerOrganizationTextScale(value / 100.0);
                    }
                    scheduleSaveState();
                    pushHistorySnapshot();
                });
    }
    if (m_speakerCurrentSpeakerNameYPositionSpin) {
        connect(m_speakerCurrentSpeakerNameYPositionSpin,
                qOverload<int>(&QSpinBox::valueChanged),
                this,
                [this](int value) {
                    if (m_preview) {
                        m_preview->setCurrentSpeakerNameVerticalPosition(value / 100.0);
                    }
                    scheduleSaveState();
                    pushHistorySnapshot();
                });
    }
    if (m_speakerCurrentSpeakerOrganizationYPositionSpin) {
        connect(m_speakerCurrentSpeakerOrganizationYPositionSpin,
                qOverload<int>(&QSpinBox::valueChanged),
                this,
                [this](int value) {
                    if (m_preview) {
                        m_preview->setCurrentSpeakerOrganizationVerticalPosition(value / 100.0);
                    }
                    scheduleSaveState();
                    pushHistorySnapshot();
                });
    }
    auto applySpeakerLabelStyle = [this]() {
        QColor background = m_speakerCurrentSpeakerBackgroundColor;
        background.setAlphaF((m_speakerCurrentSpeakerBackgroundOpacitySpin
                                  ? m_speakerCurrentSpeakerBackgroundOpacitySpin->value()
                                  : 75) /
                             100.0);
        QColor border = m_speakerCurrentSpeakerBorderColor;
        border.setAlphaF((m_speakerCurrentSpeakerBorderOpacitySpin
                              ? m_speakerCurrentSpeakerBorderOpacitySpin->value()
                              : 47) /
                         100.0);
        QColor shadow = m_speakerCurrentSpeakerShadowColor;
        shadow.setAlphaF((m_speakerCurrentSpeakerShadowOpacitySpin
                              ? m_speakerCurrentSpeakerShadowOpacitySpin->value()
                              : 75) /
                         100.0);
        m_speakerCurrentSpeakerBackgroundColor = background;
        m_speakerCurrentSpeakerBorderColor = border;
        m_speakerCurrentSpeakerShadowColor = shadow;
        m_speakerCurrentSpeakerBackgroundVisible =
            !m_speakerCurrentSpeakerBackgroundVisibleCheckBox ||
            m_speakerCurrentSpeakerBackgroundVisibleCheckBox->isChecked();
        QColor effectiveBackground = background;
        QColor effectiveBorder = border;
        if (!m_speakerCurrentSpeakerBackgroundVisible) {
            effectiveBackground.setAlpha(0);
            effectiveBorder.setAlpha(0);
        }
        if (m_preview) {
            m_preview->setCurrentSpeakerNameColor(m_speakerCurrentSpeakerNameColor);
            m_preview->setCurrentSpeakerOrganizationColor(m_speakerCurrentSpeakerOrganizationColor);
            m_preview->setCurrentSpeakerBackgroundColor(effectiveBackground);
            m_preview->setCurrentSpeakerBorderColor(effectiveBorder);
            m_preview->setCurrentSpeakerBackgroundCornerRadius(
                m_speakerCurrentSpeakerBackgroundRadiusSpin
                    ? m_speakerCurrentSpeakerBackgroundRadiusSpin->value()
                    : 14.0);
            m_preview->setCurrentSpeakerBorderWidth(
                m_speakerCurrentSpeakerBorderWidthSpin
                    ? m_speakerCurrentSpeakerBorderWidthSpin->value()
                    : 1.0);
            m_preview->setCurrentSpeakerShadowEnabled(
                m_speakerCurrentSpeakerShadowCheckBox
                    ? m_speakerCurrentSpeakerShadowCheckBox->isChecked()
                    : true);
            m_preview->setCurrentSpeakerShadowColor(m_speakerCurrentSpeakerShadowColor);
        }
        scheduleSaveState();
        pushHistorySnapshot();
    };
    auto connectSpeakerColorButton = [this, applySpeakerLabelStyle](QPushButton* button,
                                                                    QColor* target,
                                                                    const QString& title) {
        if (!button || !target) {
            return;
        }
        setColorButtonSwatch(button, *target);
        connect(button, &QPushButton::clicked, this, [this, button, target, title, applySpeakerLabelStyle]() {
            const QColor chosen = QColorDialog::getColor(*target, this, title);
            if (!chosen.isValid()) {
                return;
            }
            target->setRgb(chosen.red(), chosen.green(), chosen.blue(), target->alpha());
            setColorButtonSwatch(button, *target);
            applySpeakerLabelStyle();
        });
    };
    connectSpeakerColorButton(m_speakerCurrentSpeakerNameColorButton,
                              &m_speakerCurrentSpeakerNameColor,
                              QStringLiteral("Speaker Name Color"));
    connectSpeakerColorButton(m_speakerCurrentSpeakerOrganizationColorButton,
                              &m_speakerCurrentSpeakerOrganizationColor,
                              QStringLiteral("Speaker Organization Color"));
    connectSpeakerColorButton(m_speakerCurrentSpeakerBackgroundColorButton,
                              &m_speakerCurrentSpeakerBackgroundColor,
                              QStringLiteral("Speaker Label Background Color"));
    connectSpeakerColorButton(m_speakerCurrentSpeakerBorderColorButton,
                              &m_speakerCurrentSpeakerBorderColor,
                              QStringLiteral("Speaker Label Border Color"));
    connectSpeakerColorButton(m_speakerCurrentSpeakerShadowColorButton,
                              &m_speakerCurrentSpeakerShadowColor,
                              QStringLiteral("Speaker Label Shadow Color"));
    for (QSpinBox* spin : {m_speakerCurrentSpeakerBackgroundOpacitySpin,
                           m_speakerCurrentSpeakerBorderOpacitySpin,
                           m_speakerCurrentSpeakerBackgroundRadiusSpin,
                           m_speakerCurrentSpeakerBorderWidthSpin,
                           m_speakerCurrentSpeakerShadowOpacitySpin}) {
        if (spin) {
            connect(spin, qOverload<int>(&QSpinBox::valueChanged), this, [applySpeakerLabelStyle]() {
                applySpeakerLabelStyle();
            });
        }
    }
    if (m_speakerCurrentSpeakerShadowCheckBox) {
        connect(m_speakerCurrentSpeakerShadowCheckBox,
                &QCheckBox::toggled,
                this,
                [applySpeakerLabelStyle]() { applySpeakerLabelStyle(); });
    }
    if (m_speakerCurrentSpeakerBackgroundVisibleCheckBox) {
        connect(m_speakerCurrentSpeakerBackgroundVisibleCheckBox, &QCheckBox::toggled,
                this, [this, applySpeakerLabelStyle](bool visible) {
            for (QWidget* control : {
                     static_cast<QWidget*>(m_speakerCurrentSpeakerBackgroundColorButton),
                     static_cast<QWidget*>(m_speakerCurrentSpeakerBackgroundOpacitySpin),
                     static_cast<QWidget*>(m_speakerCurrentSpeakerBorderColorButton),
                     static_cast<QWidget*>(m_speakerCurrentSpeakerBorderOpacitySpin),
                     static_cast<QWidget*>(m_speakerCurrentSpeakerBackgroundRadiusSpin),
                     static_cast<QWidget*>(m_speakerCurrentSpeakerBorderWidthSpin)}) {
                if (control) control->setEnabled(visible);
            }
            applySpeakerLabelStyle();
        });
    }
    if (m_renderBackendCombo) {
        connect(m_renderBackendCombo,
                &QComboBox::currentIndexChanged,
                this,
                [this](int index) {
                    const QString backend = m_renderBackendCombo->itemData(index).toString().trimmed().toLower();
                    if (backend.isEmpty()) {
                        return;
                    }
                    m_renderBackendPreference = backend;
                    qputenv("JCUT_RENDER_BACKEND", m_renderBackendPreference.toUtf8());
                    scheduleSaveState();
                    pushHistorySnapshot();
                });
    }
    if (m_previewVulkanPresenterCombo) {
        connect(m_previewVulkanPresenterCombo,
                &QComboBox::currentIndexChanged,
                this,
                [this](int index) {
                    const QString presenterMode =
                        m_previewVulkanPresenterCombo->itemData(index).toString().trimmed().toLower();
                    if (presenterMode.isEmpty()) {
                        return;
                    }
                    m_previewVulkanPresenterPreference = presenterMode;
                    qputenv("JCUT_VULKAN_PREVIEW_PRESENTER", m_previewVulkanPresenterPreference.toUtf8());
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

    if (m_timelineAudioEnvelopeGranularitySpin) {
        connect(m_timelineAudioEnvelopeGranularitySpin, qOverload<int>(&QSpinBox::valueChanged), this, [this](int value) {
            editor::setDebugTimelineAudioEnvelopeGranularity(value);
            if (m_timeline) {
                m_timeline->update();
            }
            scheduleSaveState();
        });
    }

    if (m_audioAmplifyEnabledCheckBox) {
        connect(m_audioAmplifyEnabledCheckBox, &QCheckBox::toggled, this,
                [applyAudioDynamicsFromInspector](bool) { applyAudioDynamicsFromInspector(); });
    }
    if (m_audioAmplifyDbSpin) {
        connect(m_audioAmplifyDbSpin, qOverload<double>(&QDoubleSpinBox::valueChanged), this,
                [applyAudioDynamicsFromInspector](double) { applyAudioDynamicsFromInspector(); });
    }
    if (m_audioSpeakerHoverModalCheckBox) {
        connect(m_audioSpeakerHoverModalCheckBox, &QCheckBox::toggled, this, [this](bool checked) {
            m_audioSpeakerHoverModalEnabled = checked;
            if (m_preview) {
                m_preview->setAudioSpeakerHoverModalEnabled(checked);
            }
            scheduleSaveState();
        });
    }
    if (m_audioShowWaveformCheckBox) {
        connect(m_audioShowWaveformCheckBox, &QCheckBox::toggled, this, [this](bool checked) {
            m_audioWaveformVisible = checked;
            if (m_timeline) {
                const int selectedTrackIndex = m_timeline->selectedTrackIndex();
                if (selectedTrackIndex >= 0) {
                    m_timeline->updateTrackByIndex(selectedTrackIndex, [checked](TimelineTrack& track) {
                        track.audioWaveformVisible = checked;
                    });
                }
            }
            if (m_preview) {
                m_preview->setAudioWaveformVisible(checked);
            }
            scheduleSaveState();
        });
    }
    if (m_audioVisualizationModeCombo) {
        connect(m_audioVisualizationModeCombo, qOverload<int>(&QComboBox::currentIndexChanged), this,
                [this](int index) {
                    if (!m_audioVisualizationModeCombo || index < 0) {
                        return;
                    }
                    m_audioVisualizationMode = static_cast<PreviewSurface::AudioVisualizationMode>(
                        m_audioVisualizationModeCombo->itemData(index).toInt());
                    if (m_preview) {
                        m_preview->setAudioVisualizationMode(m_audioVisualizationMode);
                    }
                    if (m_loiaconoSpectrumSettingsButton) {
                        m_loiaconoSpectrumSettingsButton->setEnabled(
                            m_audioVisualizationMode == PreviewSurface::AudioVisualizationMode::Spectrum);
                    }
                    scheduleSaveState();
                });
    }
    if (m_loiaconoSpectrumSettingsButton) {
        connect(m_loiaconoSpectrumSettingsButton, &QPushButton::clicked, this, [this]() {
            if (!m_loiaconoSpectrumSettingsDialog) {
                m_loiaconoSpectrumSettingsDialog = new loiacono::SpectrumSettingsDialog(this);
                connect(m_loiaconoSpectrumSettingsDialog, &loiacono::SpectrumSettingsDialog::settingsChanged,
                        this, [this](const PreviewSurface::LoiaconoSpectrumSettings& settings) {
                            m_loiaconoSpectrumSettings = settings;
                            if (m_preview) {
                                m_preview->setLoiaconoSpectrumSettings(m_loiaconoSpectrumSettings);
                            }
                            scheduleSaveState();
                        });
            }
            m_loiaconoSpectrumSettingsDialog->setSettings(m_loiaconoSpectrumSettings);
            m_loiaconoSpectrumSettingsDialog->show();
            m_loiaconoSpectrumSettingsDialog->raise();
            m_loiaconoSpectrumSettingsDialog->activateWindow();
        });
    }
    if (m_audioNormalizeEnabledCheckBox) {
        connect(m_audioNormalizeEnabledCheckBox, &QCheckBox::toggled, this,
                [applyAudioDynamicsFromInspector](bool) { applyAudioDynamicsFromInspector(); });
    }
    if (m_audioNormalizeTargetDbSpin) {
        connect(m_audioNormalizeTargetDbSpin, qOverload<double>(&QDoubleSpinBox::valueChanged), this,
                [applyAudioDynamicsFromInspector](double) { applyAudioDynamicsFromInspector(); });
    }
    if (m_audioStereoToMonoCheckBox) {
        connect(m_audioStereoToMonoCheckBox, &QCheckBox::toggled, this,
                [applyAudioDynamicsFromInspector](bool) { applyAudioDynamicsFromInspector(); });
    }
    if (m_audioSelectiveNormalizeEnabledCheckBox) {
        connect(m_audioSelectiveNormalizeEnabledCheckBox, &QCheckBox::toggled, this,
                [applyAudioDynamicsFromInspector](bool) { applyAudioDynamicsFromInspector(); });
    }
    if (m_audioSelectiveNormalizeMinSecondsSpin) {
        connect(m_audioSelectiveNormalizeMinSecondsSpin, qOverload<double>(&QDoubleSpinBox::valueChanged), this,
                [applyAudioDynamicsFromInspector](double) { applyAudioDynamicsFromInspector(); });
    }
    if (m_audioSelectiveNormalizePeakDbSpin) {
        connect(m_audioSelectiveNormalizePeakDbSpin, qOverload<double>(&QDoubleSpinBox::valueChanged), this,
                [applyAudioDynamicsFromInspector](double) { applyAudioDynamicsFromInspector(); });
    }
    if (m_audioSelectiveNormalizePassesSpin) {
        connect(m_audioSelectiveNormalizePassesSpin, qOverload<int>(&QSpinBox::valueChanged), this,
                [applyAudioDynamicsFromInspector](int) { applyAudioDynamicsFromInspector(); });
    }
    if (m_audioSelectiveNormalizeOverlayVisibleCheckBox) {
        connect(m_audioSelectiveNormalizeOverlayVisibleCheckBox, &QCheckBox::toggled, this,
                [applyAudioDynamicsFromInspector](bool) { applyAudioDynamicsFromInspector(); });
    }
    if (m_audioTranscriptNormalizeEnabledCheckBox) {
        connect(m_audioTranscriptNormalizeEnabledCheckBox, &QCheckBox::toggled, this,
                [applyAudioDynamicsFromInspector](bool) { applyAudioDynamicsFromInspector(); });
    }
    if (m_audioWaveformPreviewProcessedCheckBox) {
        connect(m_audioWaveformPreviewProcessedCheckBox, &QCheckBox::toggled, this,
                [applyAudioDynamicsFromInspector](bool) { applyAudioDynamicsFromInspector(); });
    }
    if (m_audioPeakReductionEnabledCheckBox) {
        connect(m_audioPeakReductionEnabledCheckBox, &QCheckBox::toggled, this,
                [applyAudioDynamicsFromInspector](bool) { applyAudioDynamicsFromInspector(); });
    }
    if (m_audioPeakThresholdDbSpin) {
        connect(m_audioPeakThresholdDbSpin, qOverload<double>(&QDoubleSpinBox::valueChanged), this,
                [applyAudioDynamicsFromInspector](double) { applyAudioDynamicsFromInspector(); });
    }
    if (m_audioLimiterEnabledCheckBox) {
        connect(m_audioLimiterEnabledCheckBox, &QCheckBox::toggled, this,
                [applyAudioDynamicsFromInspector](bool) { applyAudioDynamicsFromInspector(); });
    }
    if (m_audioLimiterThresholdDbSpin) {
        connect(m_audioLimiterThresholdDbSpin, qOverload<double>(&QDoubleSpinBox::valueChanged), this,
                [applyAudioDynamicsFromInspector](double) { applyAudioDynamicsFromInspector(); });
    }
    if (m_audioCompressorEnabledCheckBox) {
        connect(m_audioCompressorEnabledCheckBox, &QCheckBox::toggled, this,
                [applyAudioDynamicsFromInspector](bool) { applyAudioDynamicsFromInspector(); });
    }
    if (m_audioCompressorThresholdDbSpin) {
        connect(m_audioCompressorThresholdDbSpin, qOverload<double>(&QDoubleSpinBox::valueChanged), this,
                [applyAudioDynamicsFromInspector](double) { applyAudioDynamicsFromInspector(); });
    }
    if (m_audioCompressorRatioSpin) {
        connect(m_audioCompressorRatioSpin, qOverload<double>(&QDoubleSpinBox::valueChanged), this,
                [applyAudioDynamicsFromInspector](double) { applyAudioDynamicsFromInspector(); });
    }
    if (m_audioSoftClipEnabledCheckBox) {
        connect(m_audioSoftClipEnabledCheckBox, &QCheckBox::toggled, this,
                [applyAudioDynamicsFromInspector](bool) { applyAudioDynamicsFromInspector(); });
    }
    if (m_preview) {
        m_preview->setAudioSpeakerHoverModalEnabled(m_audioSpeakerHoverModalEnabled);
        m_preview->setAudioWaveformVisible(m_audioWaveformVisible);
        m_preview->setAudioVisualizationMode(m_audioVisualizationMode);
        m_preview->setLoiaconoSpectrumSettings(m_loiaconoSpectrumSettings);
        if (m_speakerShowCurrentSpeakerNameCheckBox) {
            m_preview->setShowCurrentSpeakerName(m_speakerShowCurrentSpeakerNameCheckBox->isChecked());
        }
        if (m_speakerShowCurrentSpeakerOrganizationCheckBox) {
            m_preview->setShowCurrentSpeakerOrganization(
                m_speakerShowCurrentSpeakerOrganizationCheckBox->isChecked());
        }
        if (m_speakerCurrentSpeakerNameTextSizeSpin) {
            m_preview->setCurrentSpeakerNameTextScale(
                m_speakerCurrentSpeakerNameTextSizeSpin->value() / 100.0);
        }
        if (m_speakerCurrentSpeakerOrganizationTextSizeSpin) {
            m_preview->setCurrentSpeakerOrganizationTextScale(
                m_speakerCurrentSpeakerOrganizationTextSizeSpin->value() / 100.0);
        }
        if (m_speakerCurrentSpeakerNameYPositionSpin) {
            m_preview->setCurrentSpeakerNameVerticalPosition(
                m_speakerCurrentSpeakerNameYPositionSpin->value() / 100.0);
        }
        if (m_speakerCurrentSpeakerOrganizationYPositionSpin) {
            m_preview->setCurrentSpeakerOrganizationVerticalPosition(
                m_speakerCurrentSpeakerOrganizationYPositionSpin->value() / 100.0);
        }
    }
    if (m_loiaconoSpectrumSettingsButton) {
        m_loiaconoSpectrumSettingsButton->setEnabled(
            m_audioVisualizationMode == PreviewSurface::AudioVisualizationMode::Spectrum);
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

    if (m_backgroundFillEffectCombo) {
        connect(m_backgroundFillEffectCombo, &QComboBox::currentIndexChanged, this, [this]() {
            const BackgroundFillEffect effect =
                backgroundFillEffectFromString(m_backgroundFillEffectCombo->currentData().toString());
            const bool progressive = effect == BackgroundFillEffect::ProgressiveEdgeStretch;
            if (m_backgroundFillEdgeProgressiveCheckBox) {
                QSignalBlocker block(m_backgroundFillEdgeProgressiveCheckBox);
                m_backgroundFillEdgeProgressiveCheckBox->setChecked(progressive);
            }
            if (m_backgroundFillEdgePowerSpin) {
                m_backgroundFillEdgePowerSpin->setEnabled(progressive);
            }
            if (m_preview) {
                m_preview->setBackgroundFillEffect(effect);
                m_preview->setBackgroundFillEdgeProgressive(progressive);
            }
            scheduleSaveState();
            pushHistorySnapshot();
        });
        if (m_preview) {
            const BackgroundFillEffect effect =
                backgroundFillEffectFromString(m_backgroundFillEffectCombo->currentData().toString());
            m_preview->setBackgroundFillEffect(effect);
            m_preview->setBackgroundFillEdgeProgressive(
                effect == BackgroundFillEffect::ProgressiveEdgeStretch);
        }
    }

    if (m_backgroundFillOpacitySpin) {
        connect(m_backgroundFillOpacitySpin, qOverload<double>(&QDoubleSpinBox::valueChanged), this, [this](double value) {
            if (m_preview) {
                m_preview->setBackgroundFillOpacity(qBound<qreal>(0.0, value / 100.0, 1.0));
            }
            scheduleSaveState();
            pushHistorySnapshot();
        });
        if (m_preview) {
            m_preview->setBackgroundFillOpacity(
                qBound<qreal>(0.0, m_backgroundFillOpacitySpin->value() / 100.0, 1.0));
        }
    }

    if (m_backgroundFillBrightnessSpin) {
        connect(m_backgroundFillBrightnessSpin, qOverload<double>(&QDoubleSpinBox::valueChanged), this, [this](double value) {
            if (m_preview) {
                m_preview->setBackgroundFillBrightness(qBound<qreal>(-1.0, value / 100.0, 1.0));
            }
            scheduleSaveState();
            pushHistorySnapshot();
        });
        if (m_preview) {
            m_preview->setBackgroundFillBrightness(
                qBound<qreal>(-1.0, m_backgroundFillBrightnessSpin->value() / 100.0, 1.0));
        }
    }

    if (m_backgroundFillSaturationSpin) {
        connect(m_backgroundFillSaturationSpin, qOverload<double>(&QDoubleSpinBox::valueChanged), this, [this](double value) {
            if (m_preview) {
                m_preview->setBackgroundFillSaturation(qBound<qreal>(0.0, value / 100.0, 3.0));
            }
            scheduleSaveState();
            pushHistorySnapshot();
        });
        if (m_preview) {
            m_preview->setBackgroundFillSaturation(
                qBound<qreal>(0.0, m_backgroundFillSaturationSpin->value() / 100.0, 3.0));
        }
    }

    if (m_backgroundFillEdgePixelsSlider) {
        connect(m_backgroundFillEdgePixelsSlider, &QSlider::valueChanged, this, [this](int value) {
            if (m_preview) {
                m_preview->setBackgroundFillEdgePixels(qBound(1, value, 512));
            }
            scheduleSaveState();
            pushHistorySnapshot();
        });
        if (m_preview) {
            m_preview->setBackgroundFillEdgePixels(qBound(1, m_backgroundFillEdgePixelsSlider->value(), 512));
        }
    }

    if (m_backgroundFillEdgeProgressiveCheckBox) {
        connect(m_backgroundFillEdgeProgressiveCheckBox, &QCheckBox::toggled, this, [this](bool checked) {
            if (m_backgroundFillEffectCombo && checked) {
                const int progressiveIndex = m_backgroundFillEffectCombo->findData(
                    backgroundFillEffectToString(BackgroundFillEffect::ProgressiveEdgeStretch));
                if (progressiveIndex >= 0 &&
                    m_backgroundFillEffectCombo->currentIndex() != progressiveIndex) {
                    QSignalBlocker block(m_backgroundFillEffectCombo);
                    m_backgroundFillEffectCombo->setCurrentIndex(progressiveIndex);
                }
            }
            if (m_preview) {
                m_preview->setBackgroundFillEdgeProgressive(checked);
                if (checked) {
                    m_preview->setBackgroundFillEffect(BackgroundFillEffect::ProgressiveEdgeStretch);
                }
            }
            if (m_backgroundFillEdgePowerSpin) {
                m_backgroundFillEdgePowerSpin->setEnabled(checked);
            }
            scheduleSaveState();
            pushHistorySnapshot();
        });
        if (m_preview) {
            const bool progressive =
                m_backgroundFillEdgeProgressiveCheckBox->isChecked() ||
                (m_backgroundFillEffectCombo &&
                 backgroundFillEffectFromString(m_backgroundFillEffectCombo->currentData().toString()) ==
                     BackgroundFillEffect::ProgressiveEdgeStretch);
            m_preview->setBackgroundFillEdgeProgressive(progressive);
        }
        if (m_backgroundFillEdgePowerSpin) {
            m_backgroundFillEdgePowerSpin->setEnabled(
                m_backgroundFillEdgeProgressiveCheckBox->isChecked() ||
                (m_backgroundFillEffectCombo &&
                 backgroundFillEffectFromString(m_backgroundFillEffectCombo->currentData().toString()) ==
                     BackgroundFillEffect::ProgressiveEdgeStretch));
        }
    }

    if (m_backgroundFillEdgePowerSpin) {
        connect(m_backgroundFillEdgePowerSpin, qOverload<double>(&QDoubleSpinBox::valueChanged), this, [this](double value) {
            if (m_preview) {
                m_preview->setBackgroundFillEdgePower(qBound<qreal>(0.25, value, 8.0));
            }
            scheduleSaveState();
            pushHistorySnapshot();
        });
        if (m_preview) {
            m_preview->setBackgroundFillEdgePower(qBound<qreal>(0.25, m_backgroundFillEdgePowerSpin->value(), 8.0));
        }
    }

    if (m_inspectorPane->restartDecodersButton()) {
        connect(m_inspectorPane->restartDecodersButton(), &QPushButton::clicked,
                this, &EditorWindow::onRestartDecodersRequested);
    }
}

void EditorWindow::refreshSpeechFilterFadeParameterVisibility()
{
    const bool showFadeParameters =
        m_speechFilterEnabled &&
        m_speechFilterFadeMode != AudioEngine::SpeechFilterFadeMode::JumpCut;
    const bool showCurveParameters =
        m_speechFilterEnabled &&
        (m_speechFilterFadeMode == AudioEngine::SpeechFilterFadeMode::SmoothStep ||
         m_speechFilterFadeMode == AudioEngine::SpeechFilterFadeMode::SmootherStep);
    const QList<QWidget*> widgets{
        m_speechFilterFadeSamplesSpin,
    };
    for (QWidget* widget : widgets) {
        if (!widget) {
            continue;
        }
        widget->setVisible(showFadeParameters);
        if (QWidget* parent = widget->parentWidget()) {
            const auto forms = parent->findChildren<QFormLayout*>();
            for (QFormLayout* form : forms) {
                if (QWidget* rowLabel = form->labelForField(widget)) {
                    rowLabel->setVisible(showFadeParameters);
                }
            }
        }
    }
    const QList<QWidget*> curveWidgets{m_speechFilterCurveStrengthSpin};
    for (QWidget* widget : curveWidgets) {
        if (!widget) {
            continue;
        }
        widget->setVisible(showCurveParameters);
        if (QWidget* parent = widget->parentWidget()) {
            const auto forms = parent->findChildren<QFormLayout*>();
            for (QFormLayout* form : forms) {
                if (QWidget* rowLabel = form->labelForField(widget)) {
                    rowLabel->setVisible(showCurveParameters);
                }
            }
        }
    }
    const bool showFrameParameters = m_speechFilterEnabled;
    const QList<QWidget*> frameWidgets{
        m_speechFilterFrameTransitionModeCombo,
        m_speechFilterFrameCrossfadeFramesSpin,
    };
    for (QWidget* widget : frameWidgets) {
        if (!widget) {
            continue;
        }
        widget->setVisible(showFrameParameters);
        if (QWidget* parent = widget->parentWidget()) {
            const auto forms = parent->findChildren<QFormLayout*>();
            for (QFormLayout* form : forms) {
                if (QWidget* rowLabel = form->labelForField(widget)) {
                    rowLabel->setVisible(showFrameParameters);
                }
            }
        }
    }
}
