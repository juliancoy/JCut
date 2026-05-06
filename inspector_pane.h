#pragma once

#include <QWidget>

class QLabel;
class QTabWidget;
class QHBoxLayout;
class QDoubleSpinBox;
class QSpinBox;
class QComboBox;
class QCheckBox;
class QFontComboBox;
class QTableWidget;
class QPushButton;
class QLineEdit;
class QPlainTextEdit;
class QTextBrowser;
class QIcon;
class QListWidget;
class GradingHistogramWidget;

class InspectorPane final : public QWidget
{
    Q_OBJECT

public:
    explicit InspectorPane(QWidget *parent = nullptr);
    void setHeaderWidget(QWidget *widget);

    QTabWidget *tabs() const { return m_inspectorTabs; }

    QDoubleSpinBox *brightnessSpin() const { return m_brightnessSpin; }
    QDoubleSpinBox *contrastSpin() const { return m_contrastSpin; }
    QDoubleSpinBox *saturationSpin() const { return m_saturationSpin; }
    QDoubleSpinBox *opacitySpin() const { return m_opacitySpin; }
    QLabel *opacityPathLabel() const { return m_opacityPathLabel; }
    QTableWidget *opacityKeyframeTable() const { return m_opacityKeyframeTable; }
    QCheckBox *opacityAutoScrollCheckBox() const { return m_opacityAutoScrollCheckBox; }
    QCheckBox *opacityFollowCurrentCheckBox() const { return m_opacityFollowCurrentCheckBox; }
    QPushButton *opacityKeyAtPlayheadButton() const { return m_opacityKeyAtPlayheadButton; }
    QPushButton *opacityFadeInButton() const { return m_opacityFadeInButton; }
    QPushButton *opacityFadeOutButton() const { return m_opacityFadeOutButton; }
    QDoubleSpinBox *opacityFadeDurationSpin() const { return m_opacityFadeDurationSpin; }
    
    // Shadows/Midtones/Highlights (Lift/Gamma/Gain)
    QDoubleSpinBox *shadowsRSpin() const { return m_shadowsRSpin; }
    QDoubleSpinBox *shadowsGSpin() const { return m_shadowsGSpin; }
    QDoubleSpinBox *shadowsBSpin() const { return m_shadowsBSpin; }
    QDoubleSpinBox *midtonesRSpin() const { return m_midtonesRSpin; }
    QDoubleSpinBox *midtonesGSpin() const { return m_midtonesGSpin; }
    QDoubleSpinBox *midtonesBSpin() const { return m_midtonesBSpin; }
    QDoubleSpinBox *highlightsRSpin() const { return m_highlightsRSpin; }
    QDoubleSpinBox *highlightsGSpin() const { return m_highlightsGSpin; }
    QDoubleSpinBox *highlightsBSpin() const { return m_highlightsBSpin; }

    QDoubleSpinBox *videoTranslationXSpin() const { return m_videoTranslationXSpin; }
    QDoubleSpinBox *videoTranslationYSpin() const { return m_videoTranslationYSpin; }
    QDoubleSpinBox *videoRotationSpin() const { return m_videoRotationSpin; }
    QDoubleSpinBox *videoScaleXSpin() const { return m_videoScaleXSpin; }
    QDoubleSpinBox *videoScaleYSpin() const { return m_videoScaleYSpin; }
    QComboBox *videoInterpolationCombo() const { return m_videoInterpolationCombo; }
    QCheckBox *mirrorHorizontalCheckBox() const { return m_mirrorHorizontalCheckBox; }
    QCheckBox *mirrorVerticalCheckBox() const { return m_mirrorVerticalCheckBox; }
    QCheckBox *lockVideoScaleCheckBox() const { return m_lockVideoScaleCheckBox; }
    QCheckBox *keyframeSpaceCheckBox() const { return m_keyframeSpaceCheckBox; }
    QCheckBox *keyframeSkipAwareTimingCheckBox() const { return m_keyframeSkipAwareTimingCheckBox; }

    QCheckBox *transcriptOverlayEnabledCheckBox() const { return m_transcriptOverlayEnabledCheckBox; }
    QCheckBox *transcriptBackgroundVisibleCheckBox() const { return m_transcriptBackgroundVisibleCheckBox; }
    QCheckBox *transcriptShadowEnabledCheckBox() const { return m_transcriptShadowEnabledCheckBox; }
    QCheckBox *transcriptShowSpeakerTitleCheckBox() const { return m_transcriptShowSpeakerTitleCheckBox; }
    QSpinBox *transcriptMaxLinesSpin() const { return m_transcriptMaxLinesSpin; }
    QSpinBox *transcriptMaxCharsSpin() const { return m_transcriptMaxCharsSpin; }
    QCheckBox *transcriptAutoScrollCheckBox() const { return m_transcriptAutoScrollCheckBox; }
    QDoubleSpinBox *transcriptOverlayXSpin() const { return m_transcriptOverlayXSpin; }
    QDoubleSpinBox *transcriptOverlayYSpin() const { return m_transcriptOverlayYSpin; }
    QPushButton *transcriptCenterHorizontalButton() const { return m_transcriptCenterHorizontalButton; }
    QPushButton *transcriptCenterVerticalButton() const { return m_transcriptCenterVerticalButton; }
    QSpinBox *transcriptOverlayWidthSpin() const { return m_transcriptOverlayWidthSpin; }
    QSpinBox *transcriptOverlayHeightSpin() const { return m_transcriptOverlayHeightSpin; }
    QFontComboBox *transcriptFontFamilyCombo() const { return m_transcriptFontFamilyCombo; }
    QSpinBox *transcriptFontSizeSpin() const { return m_transcriptFontSizeSpin; }
    QCheckBox *transcriptBoldCheckBox() const { return m_transcriptBoldCheckBox; }
    QCheckBox *transcriptItalicCheckBox() const { return m_transcriptItalicCheckBox; }
    QCheckBox *transcriptFollowCurrentWordCheckBox() const { return m_transcriptFollowCurrentWordCheckBox; }
    QCheckBox *transcriptUnifiedEditModeCheckBox() const { return m_transcriptUnifiedEditModeCheckBox; }
    QLineEdit *transcriptSearchFilterLineEdit() const { return m_transcriptSearchFilterLineEdit; }
    QComboBox *transcriptSpeakerFilterCombo() const { return m_transcriptSpeakerFilterCombo; }
    QComboBox *transcriptScriptVersionCombo() const { return m_transcriptScriptVersionCombo; }
    QPushButton *transcriptNewVersionButton() const { return m_transcriptNewVersionButton; }
    QPushButton *transcriptDeleteVersionButton() const { return m_transcriptDeleteVersionButton; }
    QCheckBox *transcriptShowExcludedLinesCheckBox() const { return m_transcriptShowExcludedLinesCheckBox; }
    QLabel *speakersInspectorClipLabel() const { return m_speakersInspectorClipLabel; }
    QLabel *speakersInspectorDetailsLabel() const { return m_speakersInspectorDetailsLabel; }
    QTableWidget *speakersTable() const { return m_speakersTable; }
    QLabel *selectedSpeakerIdLabel() const { return m_selectedSpeakerIdLabel; }
    QLabel *selectedSpeakerRef1ImageLabel() const { return m_selectedSpeakerRef1ImageLabel; }
    QLabel *selectedSpeakerRef2ImageLabel() const { return m_selectedSpeakerRef2ImageLabel; }
    QPushButton *selectedSpeakerPreviousSentenceButton() const { return m_selectedSpeakerPreviousSentenceButton; }
    QPushButton *selectedSpeakerNextSentenceButton() const { return m_selectedSpeakerNextSentenceButton; }
    QPushButton *selectedSpeakerRandomSentenceButton() const { return m_selectedSpeakerRandomSentenceButton; }
    QLabel *speakerCurrentSentenceLabel() const { return m_speakerCurrentSentenceLabel; }
    QPushButton *speakerSetReference1Button() const { return m_speakerSetReference1Button; }
    QPushButton *speakerSetReference2Button() const { return m_speakerSetReference2Button; }
    QPushButton *speakerPickReference1Button() const { return m_speakerPickReference1Button; }
    QPushButton *speakerPickReference2Button() const { return m_speakerPickReference2Button; }
    QPushButton *speakerClearReferencesButton() const { return m_speakerClearReferencesButton; }
    QPushButton *speakerRunAutoTrackButton() const { return m_speakerRunAutoTrackButton; }
    QPushButton *speakerBoxstreamSettingsButton() const { return m_speakerBoxstreamSettingsButton; }
    QPushButton *speakerEnableTrackingButton() const { return m_speakerEnableTrackingButton; }
    QPushButton *speakerDisableTrackingButton() const { return m_speakerDisableTrackingButton; }
    QPushButton *speakerDeletePointstreamButton() const { return m_speakerDeletePointstreamButton; }
    QPushButton *speakerGuideButton() const { return m_speakerGuideButton; }
    QPushButton *speakerPrecropFacesButton() const { return m_speakerPrecropFacesButton; }
    QPushButton *speakerAiFindNamesButton() const { return m_speakerAiFindNamesButton; }
    QPushButton *speakerAiFindOrganizationsButton() const { return m_speakerAiFindOrganizationsButton; }
    QPushButton *speakerAiCleanAssignmentsButton() const { return m_speakerAiCleanAssignmentsButton; }
    QLabel *speakerTrackingStatusLabel() const { return m_speakerTrackingStatusLabel; }
    QDoubleSpinBox *speakerFramingTargetXSpin() const { return m_speakerFramingTargetXSpin; }
    QDoubleSpinBox *speakerFramingTargetYSpin() const { return m_speakerFramingTargetYSpin; }
    QDoubleSpinBox *speakerFramingTargetBoxSpin() const { return m_speakerFramingTargetBoxSpin; }
    QCheckBox *speakerFramingZoomEnabledCheckBox() const { return m_speakerFramingZoomEnabledCheckBox; }
    QCheckBox *speakerShowBoxStreamBoxesCheckBox() const { return m_speakerShowBoxStreamBoxesCheckBox; }
    QCheckBox *speakerApplyFramingToClipCheckBox() const { return m_speakerApplyFramingToClipCheckBox; }
    QLabel *speakerClipFramingStatusLabel() const { return m_speakerClipFramingStatusLabel; }
    QLabel *speakerRefsChipLabel() const { return m_speakerRefsChipLabel; }
    QLabel *speakerPointstreamChipLabel() const { return m_speakerPointstreamChipLabel; }
    QPushButton *speakerTrackingChipButton() const { return m_speakerTrackingChipButton; }
    QPushButton *speakerStabilizeChipButton() const { return m_speakerStabilizeChipButton; }
    QTableWidget *speakerBoxStreamTable() const { return m_speakerBoxStreamTable; }
    QPlainTextEdit *speakerBoxStreamDetailsEdit() const { return m_speakerBoxStreamDetailsEdit; }
    QComboBox *speakerBoxStreamOverlaySourceCombo() const { return m_speakerBoxStreamOverlaySourceCombo; }

    QLabel *gradingPathLabel() const { return m_gradingPathLabel; }
    QTableWidget *gradingKeyframeTable() const { return m_gradingKeyframeTable; }
    QCheckBox *gradingAutoScrollCheckBox() const { return m_gradingAutoScrollCheckBox; }
    QCheckBox *gradingFollowCurrentCheckBox() const { return m_gradingFollowCurrentCheckBox; }
    QCheckBox *gradingPreviewCheckBox() const { return m_gradingPreviewCheckBox; }
    QPushButton *gradingKeyAtPlayheadButton() const { return m_gradingKeyAtPlayheadButton; }
    QPushButton *gradingFadeInButton() const { return m_gradingFadeInButton; }
    QPushButton *gradingFadeOutButton() const { return m_gradingFadeOutButton; }
    QPushButton *gradingAutoOpposeButton() const { return m_gradingAutoOpposeButton; }
    QDoubleSpinBox *gradingFadeDurationSpin() const { return m_gradingFadeDurationSpin; }
    QComboBox *gradingCurveChannelCombo() const { return m_gradingCurveChannelCombo; }
    QComboBox *gradingEditModeCombo() const { return m_gradingEditModeCombo; }
    GradingHistogramWidget *gradingHistogramWidget() const { return m_gradingHistogramWidget; }
    QCheckBox *gradingCurveThreePointLockCheckBox() const { return m_gradingCurveThreePointLockCheckBox; }
    QCheckBox *gradingCurveSmoothingCheckBox() const { return m_gradingCurveSmoothingCheckBox; }
    
    QLabel *effectsPathLabel() const { return m_effectsPathLabel; }
    QDoubleSpinBox *maskFeatherSpin() const { return m_maskFeatherSpin; }
    QDoubleSpinBox *maskFeatherGammaSpin() const { return m_maskFeatherGammaSpin; }
    QCheckBox *maskFeatherEnabledCheck() const { return m_maskFeatherEnabledCheck; }
    QLabel *correctionsClipLabel() const { return m_correctionsClipLabel; }
    QLabel *correctionsStatusLabel() const { return m_correctionsStatusLabel; }
    QCheckBox *correctionsEnabledCheck() const { return m_correctionsEnabledCheck; }
    QTableWidget *correctionsPolygonTable() const { return m_correctionsPolygonTable; }
    QTableWidget *correctionsVertexTable() const { return m_correctionsVertexTable; }
    QCheckBox *correctionsDrawModeCheck() const { return m_correctionsDrawModeCheck; }
    QPushButton *correctionsDrawPolygonButton() const { return m_correctionsDrawPolygonButton; }
    QPushButton *correctionsClosePolygonButton() const { return m_correctionsClosePolygonButton; }
    QPushButton *correctionsCancelDraftButton() const { return m_correctionsCancelDraftButton; }
    QPushButton *correctionsDeleteLastButton() const { return m_correctionsDeleteLastButton; }
    QPushButton *correctionsClearAllButton() const { return m_correctionsClearAllButton; }
    
    QLabel *syncInspectorClipLabel() const { return m_syncInspectorClipLabel; }
    QLabel *syncInspectorDetailsLabel() const { return m_syncInspectorDetailsLabel; }
    QTableWidget *syncTable() const { return m_syncTable; }
    QPushButton *clearAllSyncPointsButton() const { return m_clearAllSyncPointsButton; }

    QLabel *keyframesInspectorClipLabel() const { return m_keyframesInspectorClipLabel; }
    QLabel *keyframesInspectorDetailsLabel() const { return m_keyframesInspectorDetailsLabel; }
    QTableWidget *videoKeyframeTable() const { return m_videoKeyframeTable; }
    QCheckBox *keyframesAutoScrollCheckBox() const { return m_keyframesAutoScrollCheckBox; }
    QCheckBox *keyframesFollowCurrentCheckBox() const { return m_keyframesFollowCurrentCheckBox; }
    QPushButton *addVideoKeyframeButton() const { return m_addVideoKeyframeButton; }
    QPushButton *removeVideoKeyframeButton() const { return m_removeVideoKeyframeButton; }
    QPushButton *flipHorizontalButton() const { return m_flipHorizontalButton; }

    QLabel *titlesInspectorClipLabel() const { return m_titlesInspectorClipLabel; }
    QLabel *titlesInspectorDetailsLabel() const { return m_titlesInspectorDetailsLabel; }
    QTableWidget *titleKeyframeTable() const { return m_titleKeyframeTable; }
    QPlainTextEdit *titleTextEdit() const { return m_titleTextEdit; }
    QDoubleSpinBox *titleXSpin() const { return m_titleXSpin; }
    QDoubleSpinBox *titleYSpin() const { return m_titleYSpin; }
    QDoubleSpinBox *titleFontSizeSpin() const { return m_titleFontSizeSpin; }
    QDoubleSpinBox *titleOpacitySpin() const { return m_titleOpacitySpin; }
    QFontComboBox *titleFontCombo() const { return m_titleFontCombo; }
    QCheckBox *titleBoldCheck() const { return m_titleBoldCheck; }
    QCheckBox *titleItalicCheck() const { return m_titleItalicCheck; }
    QPushButton *titleColorButton() const { return m_titleColorButton; }
    QCheckBox *titleShadowEnabledCheck() const { return m_titleShadowEnabledCheck; }
    QPushButton *titleShadowColorButton() const { return m_titleShadowColorButton; }
    QDoubleSpinBox *titleShadowOpacitySpin() const { return m_titleShadowOpacitySpin; }
    QDoubleSpinBox *titleShadowOffsetXSpin() const { return m_titleShadowOffsetXSpin; }
    QDoubleSpinBox *titleShadowOffsetYSpin() const { return m_titleShadowOffsetYSpin; }
    QCheckBox *titleWindowEnabledCheck() const { return m_titleWindowEnabledCheck; }
    QPushButton *titleWindowColorButton() const { return m_titleWindowColorButton; }
    QDoubleSpinBox *titleWindowOpacitySpin() const { return m_titleWindowOpacitySpin; }
    QDoubleSpinBox *titleWindowPaddingSpin() const { return m_titleWindowPaddingSpin; }
    QCheckBox *titleWindowFrameEnabledCheck() const { return m_titleWindowFrameEnabledCheck; }
    QPushButton *titleWindowFrameColorButton() const { return m_titleWindowFrameColorButton; }
    QDoubleSpinBox *titleWindowFrameOpacitySpin() const { return m_titleWindowFrameOpacitySpin; }
    QDoubleSpinBox *titleWindowFrameWidthSpin() const { return m_titleWindowFrameWidthSpin; }
    QDoubleSpinBox *titleWindowFrameGapSpin() const { return m_titleWindowFrameGapSpin; }
    QCheckBox *titleAutoScrollCheck() const { return m_titleAutoScrollCheck; }
    QPushButton *addTitleKeyframeButton() const { return m_addTitleKeyframeButton; }
    QPushButton *removeTitleKeyframeButton() const { return m_removeTitleKeyframeButton; }
    QPushButton *titleCenterHorizontalButton() const { return m_titleCenterHorizontalButton; }
    QPushButton *titleCenterVerticalButton() const { return m_titleCenterVerticalButton; }

    QLabel *audioInspectorClipLabel() const { return m_audioInspectorClipLabel; }
    QLabel *audioInspectorDetailsLabel() const { return m_audioInspectorDetailsLabel; }

    QLineEdit *transcriptInspectorClipLabel() const { return m_transcriptInspectorClipLabel; }
    QLabel *transcriptInspectorDetailsLabel() const { return m_transcriptInspectorDetailsLabel; }
    QTableWidget *transcriptTable() const { return m_transcriptTable; }
    QLabel *clipInspectorClipLabel() const { return m_clipInspectorClipLabel; }
    QLabel *clipProxyUsageLabel() const { return m_clipProxyUsageLabel; }
    QLabel *clipPlaybackSourceLabel() const { return m_clipPlaybackSourceLabel; }
    QLabel *clipOriginalInfoLabel() const { return m_clipOriginalInfoLabel; }
    QLabel *clipProxyInfoLabel() const { return m_clipProxyInfoLabel; }
    QDoubleSpinBox *clipPlaybackRateSpin() const { return m_clipPlaybackRateSpin; }
    QLabel *trackInspectorLabel() const { return m_trackInspectorLabel; }
    QLabel *trackInspectorDetailsLabel() const { return m_trackInspectorDetailsLabel; }
    QLineEdit *trackNameEdit() const { return m_trackNameEdit; }
    QSpinBox *trackHeightSpin() const { return m_trackHeightSpin; }
    QComboBox *trackVisualModeCombo() const { return m_trackVisualModeCombo; }
    QCheckBox *trackAudioEnabledCheckBox() const { return m_trackAudioEnabledCheckBox; }
    QDoubleSpinBox *trackCrossfadeSecondsSpin() const { return m_trackCrossfadeSecondsSpin; }
    QPushButton *trackCrossfadeButton() const { return m_trackCrossfadeButton; }
    QCheckBox *previewHideOutsideOutputCheckBox() const { return m_previewHideOutsideOutputCheckBox; }
    QCheckBox *previewShowSpeakerTrackPointsCheckBox() const { return m_previewShowSpeakerTrackPointsCheckBox; }
    QComboBox *previewVulkanPresenterCombo() const { return m_previewVulkanPresenterCombo; }
    QDoubleSpinBox *previewZoomSpin() const { return m_previewZoomSpin; }
    QPushButton *previewZoomResetButton() const { return m_previewZoomResetButton; }
    QCheckBox *previewPlaybackCacheFallbackCheckBox() const { return m_previewPlaybackCacheFallbackCheckBox; }
    QCheckBox *previewLeadPrefetchEnabledCheckBox() const { return m_previewLeadPrefetchEnabledCheckBox; }
    QSpinBox *previewLeadPrefetchCountSpin() const { return m_previewLeadPrefetchCountSpin; }
    QSpinBox *previewPlaybackWindowAheadSpin() const { return m_previewPlaybackWindowAheadSpin; }
    QSpinBox *previewVisibleQueueReserveSpin() const { return m_previewVisibleQueueReserveSpin; }
    QSpinBox *timelineAudioEnvelopeGranularitySpin() const { return m_timelineAudioEnvelopeGranularitySpin; }
    QCheckBox *preferencesFeatureAiPanelCheckBox() const { return m_preferencesFeatureAiPanelCheckBox; }
    QCheckBox *preferencesFeatureAiSpeakerCleanupCheckBox() const { return m_preferencesFeatureAiSpeakerCleanupCheckBox; }
    QCheckBox *preferencesFeatureAudioPreviewModeCheckBox() const { return m_preferencesFeatureAudioPreviewModeCheckBox; }
    QCheckBox *preferencesFeatureAudioDynamicsToolsCheckBox() const { return m_preferencesFeatureAudioDynamicsToolsCheckBox; }
    QCheckBox *audioAmplifyEnabledCheckBox() const { return m_audioAmplifyEnabledCheckBox; }
    QDoubleSpinBox *audioAmplifyDbSpin() const { return m_audioAmplifyDbSpin; }
    QCheckBox *audioSpeakerHoverModalCheckBox() const { return m_audioSpeakerHoverModalCheckBox; }
    QCheckBox *audioShowWaveformCheckBox() const { return m_audioShowWaveformCheckBox; }
    QCheckBox *audioWaveformPreviewProcessedCheckBox() const { return m_audioWaveformPreviewProcessedCheckBox; }
    QCheckBox *audioNormalizeEnabledCheckBox() const { return m_audioNormalizeEnabledCheckBox; }
    QDoubleSpinBox *audioNormalizeTargetDbSpin() const { return m_audioNormalizeTargetDbSpin; }
    QCheckBox *audioSelectiveNormalizeEnabledCheckBox() const { return m_audioSelectiveNormalizeEnabledCheckBox; }
    QDoubleSpinBox *audioSelectiveNormalizeMinSecondsSpin() const { return m_audioSelectiveNormalizeMinSecondsSpin; }
    QDoubleSpinBox *audioSelectiveNormalizePeakDbSpin() const { return m_audioSelectiveNormalizePeakDbSpin; }
    QSpinBox *audioSelectiveNormalizePassesSpin() const { return m_audioSelectiveNormalizePassesSpin; }
    QCheckBox *audioSelectiveNormalizeOverlayVisibleCheckBox() const { return m_audioSelectiveNormalizeOverlayVisibleCheckBox; }
    QCheckBox *audioTranscriptNormalizeEnabledCheckBox() const { return m_audioTranscriptNormalizeEnabledCheckBox; }
    QCheckBox *audioPeakReductionEnabledCheckBox() const { return m_audioPeakReductionEnabledCheckBox; }
    QDoubleSpinBox *audioPeakThresholdDbSpin() const { return m_audioPeakThresholdDbSpin; }
    QCheckBox *audioLimiterEnabledCheckBox() const { return m_audioLimiterEnabledCheckBox; }
    QDoubleSpinBox *audioLimiterThresholdDbSpin() const { return m_audioLimiterThresholdDbSpin; }
    QCheckBox *audioCompressorEnabledCheckBox() const { return m_audioCompressorEnabledCheckBox; }
    QDoubleSpinBox *audioCompressorThresholdDbSpin() const { return m_audioCompressorThresholdDbSpin; }
    QDoubleSpinBox *audioCompressorRatioSpin() const { return m_audioCompressorRatioSpin; }
    QTableWidget *profileSummaryTable() const { return m_profileSummaryTable; }
    QComboBox *profileH26xThreadingModeCombo() const { return m_profileH26xThreadingModeCombo; }
    QTableWidget *clipsTable() const { return m_clipsTable; }
    QTableWidget *historyTable() const { return m_historyTable; }
    QTableWidget *tracksTable() const { return m_tracksTable; }
    QPushButton *profileBenchmarkButton() const { return m_profileBenchmarkButton; }
    QLabel *projectSectionLabel() const { return m_projectSectionLabel; }
    QListWidget *projectsList() const { return m_projectsList; }
    QPushButton *newProjectButton() const { return m_newProjectButton; }
    QPushButton *saveProjectAsButton() const { return m_saveProjectAsButton; }
    QPushButton *renameProjectButton() const { return m_renameProjectButton; }
    QLabel *aiStatusLabel() const { return m_aiStatusLabel; }
    QComboBox *aiModelCombo() const { return m_aiModelCombo; }
    QPushButton *aiTranscribeButton() const { return m_aiTranscribeButton; }
    QPushButton *aiFindSpeakerNamesButton() const { return m_aiFindSpeakerNamesButton; }
    QPushButton *aiFindOrganizationsButton() const { return m_aiFindOrganizationsButton; }
    QPushButton *aiCleanAssignmentsButton() const { return m_aiCleanAssignmentsButton; }
    QPushButton *aiSubscribeButton() const { return m_aiSubscribeButton; }
    QTextBrowser *aiChatHistoryEdit() const { return m_aiChatHistoryEdit; }
    QPlainTextEdit *aiChatInputLineEdit() const { return m_aiChatInputLineEdit; }
    QPushButton *aiChatSendButton() const { return m_aiChatSendButton; }
    QPushButton *aiChatClearButton() const { return m_aiChatClearButton; }
    QLabel *accessStatusLabel() const { return m_accessStatusLabel; }
    QTableWidget *accessTable() const { return m_accessTable; }
    QPushButton *accessRefreshButton() const { return m_accessRefreshButton; }

    QSpinBox *outputWidthSpin() const { return m_outputWidthSpin; }
    QSpinBox *outputHeightSpin() const { return m_outputHeightSpin; }
    QSpinBox *exportStartSpin() const { return m_exportStartSpin; }
    QSpinBox *exportEndSpin() const { return m_exportEndSpin; }
    QComboBox *outputFormatCombo() const { return m_outputFormatCombo; }
    QComboBox *renderBackendCombo() const { return m_renderBackendCombo; }
    QLabel *outputRangeSummaryLabel() const { return m_outputRangeSummaryLabel; }
    QCheckBox *renderUseProxiesCheckBox() const { return m_renderUseProxiesCheckBox; }
    QCheckBox *renderCreateVideoFromSequenceCheckBox() const { return m_renderCreateVideoFromSequenceCheckBox; }
    QCheckBox *outputPlaybackCacheFallbackCheckBox() const { return m_outputPlaybackCacheFallbackCheckBox; }
    QCheckBox *outputLeadPrefetchEnabledCheckBox() const { return m_outputLeadPrefetchEnabledCheckBox; }
    QSpinBox *outputLeadPrefetchCountSpin() const { return m_outputLeadPrefetchCountSpin; }
    QSpinBox *outputPlaybackWindowAheadSpin() const { return m_outputPlaybackWindowAheadSpin; }
    QSpinBox *outputVisibleQueueReserveSpin() const { return m_outputVisibleQueueReserveSpin; }
    QSpinBox *outputPrefetchMaxQueueDepthSpin() const { return m_outputPrefetchMaxQueueDepthSpin; }
    QSpinBox *outputPrefetchMaxInflightSpin() const { return m_outputPrefetchMaxInflightSpin; }
    QSpinBox *outputPrefetchMaxPerTickSpin() const { return m_outputPrefetchMaxPerTickSpin; }
    QSpinBox *outputPrefetchSkipVisiblePendingThresholdSpin() const { return m_outputPrefetchSkipVisiblePendingThresholdSpin; }
    QSpinBox *outputDecoderLaneCountSpin() const { return m_outputDecoderLaneCountSpin; }
    QComboBox *outputDecodeModeCombo() const { return m_outputDecodeModeCombo; }
    QCheckBox *outputDeterministicPipelineCheckBox() const { return m_outputDeterministicPipelineCheckBox; }
    QPushButton *outputResetPipelineDefaultsButton() const { return m_outputResetPipelineDefaultsButton; }
    QSpinBox *autosaveIntervalMinutesSpin() const { return m_autosaveIntervalMinutesSpin; }
    QSpinBox *autosaveMaxBackupsSpin() const { return m_autosaveMaxBackupsSpin; }
    QPushButton *renderButton() const { return m_renderButton; }
    QPushButton *backgroundColorButton() const { return m_backgroundColorButton; }
    QPushButton *restartDecodersButton() const { return m_restartDecodersButton; }

    QCheckBox *speechFilterEnabledCheckBox() const { return m_speechFilterEnabledCheckBox; }
    QSpinBox *transcriptPrependMsSpin() const { return m_transcriptPrependMsSpin; }
    QSpinBox *transcriptPostpendMsSpin() const { return m_transcriptPostpendMsSpin; }
    QSpinBox *speechFilterFadeSamplesSpin() const { return m_speechFilterFadeSamplesSpin; }
    QCheckBox *speechFilterRangeCrossfadeCheckBox() const { return m_speechFilterRangeCrossfadeCheckBox; }
    QComboBox *playbackClockSourceCombo() const { return m_playbackClockSourceCombo; }
    QComboBox *playbackAudioWarpModeCombo() const { return m_playbackAudioWarpModeCombo; }

    void refresh();

signals:
    void refreshRequested();
    void restartDecodersRequested();

private:
    QWidget *buildPane();
    QWidget *buildGradingTab();
    QWidget *buildHistoryTab();
    QWidget *buildOpacityTab();
    QWidget *buildEffectsTab();
    QWidget *buildCorrectionsTab();
    QWidget *buildTitlesTab();
    QWidget *buildSyncTab();
    QWidget *buildKeyframesTab();
    QWidget *buildTranscriptTab();
    QWidget *buildSpeakersTab();
    QWidget *buildClipTab();
    QWidget *buildClipsTab();
    QWidget *buildTracksTab();
    QWidget *buildOutputTab();
    QWidget *buildPreviewTab();
    QWidget *buildAudioTab();
    QWidget *buildPreferencesTab();
    QWidget *buildProfileTab();
    QWidget *buildProjectsTab();
    QWidget *buildAiTab();
    QWidget *buildAccessTab();
    void configureInspectorTabs();

private:
    QTabWidget *m_inspectorTabs = nullptr;
    QHBoxLayout *m_headerLayout = nullptr;

    QLabel *m_gradingPathLabel = nullptr;
    QLabel *m_opacityPathLabel = nullptr;
    QDoubleSpinBox *m_brightnessSpin = nullptr;
    QDoubleSpinBox *m_contrastSpin = nullptr;
    QDoubleSpinBox *m_saturationSpin = nullptr;
    QDoubleSpinBox *m_opacitySpin = nullptr;
    // Shadows/Midtones/Highlights (Lift/Gamma/Gain)
    QDoubleSpinBox *m_shadowsRSpin = nullptr;
    QDoubleSpinBox *m_shadowsGSpin = nullptr;
    QDoubleSpinBox *m_shadowsBSpin = nullptr;
    QDoubleSpinBox *m_midtonesRSpin = nullptr;
    QDoubleSpinBox *m_midtonesGSpin = nullptr;
    QDoubleSpinBox *m_midtonesBSpin = nullptr;
    QDoubleSpinBox *m_highlightsRSpin = nullptr;
    QDoubleSpinBox *m_highlightsGSpin = nullptr;
    QDoubleSpinBox *m_highlightsBSpin = nullptr;
    QTableWidget *m_gradingKeyframeTable = nullptr;
    QTableWidget *m_opacityKeyframeTable = nullptr;
    QCheckBox *m_gradingAutoScrollCheckBox = nullptr;
    QCheckBox *m_gradingFollowCurrentCheckBox = nullptr;
    QCheckBox *m_gradingPreviewCheckBox = nullptr;
    QCheckBox *m_opacityAutoScrollCheckBox = nullptr;
    QCheckBox *m_opacityFollowCurrentCheckBox = nullptr;
    QPushButton *m_gradingKeyAtPlayheadButton = nullptr;
    QPushButton *m_gradingFadeInButton = nullptr;
    QPushButton *m_gradingFadeOutButton = nullptr;
    QPushButton *m_gradingAutoOpposeButton = nullptr;
    QPushButton *m_opacityKeyAtPlayheadButton = nullptr;
    QPushButton *m_opacityFadeInButton = nullptr;
    QPushButton *m_opacityFadeOutButton = nullptr;
    QDoubleSpinBox *m_gradingFadeDurationSpin = nullptr;
    QComboBox *m_gradingEditModeCombo = nullptr;
    QWidget *m_gradingLevelsPanel = nullptr;
    QWidget *m_gradingCurvesPanel = nullptr;
    QTabWidget *m_gradingCurveChannelTabs = nullptr;
    QComboBox *m_gradingCurveChannelCombo = nullptr;
    GradingHistogramWidget *m_gradingHistogramWidget = nullptr;
    QCheckBox *m_gradingCurveThreePointLockCheckBox = nullptr;
    QCheckBox *m_gradingCurveSmoothingCheckBox = nullptr;
    QDoubleSpinBox *m_opacityFadeDurationSpin = nullptr;

    QLabel *m_effectsPathLabel = nullptr;
    QDoubleSpinBox *m_maskFeatherSpin = nullptr;
    QDoubleSpinBox *m_maskFeatherGammaSpin = nullptr;
    QCheckBox *m_maskFeatherEnabledCheck = nullptr;
    QLabel *m_correctionsClipLabel = nullptr;
    QLabel *m_correctionsStatusLabel = nullptr;
    QCheckBox *m_correctionsEnabledCheck = nullptr;
    QTableWidget *m_correctionsPolygonTable = nullptr;
    QTableWidget *m_correctionsVertexTable = nullptr;
    QCheckBox *m_correctionsDrawModeCheck = nullptr;
    QPushButton *m_correctionsDrawPolygonButton = nullptr;
    QPushButton *m_correctionsClosePolygonButton = nullptr;
    QPushButton *m_correctionsCancelDraftButton = nullptr;
    QPushButton *m_correctionsDeleteLastButton = nullptr;
    QPushButton *m_correctionsClearAllButton = nullptr;

    QLabel *m_syncInspectorClipLabel = nullptr;
    QLabel *m_syncInspectorDetailsLabel = nullptr;
    QTableWidget *m_syncTable = nullptr;
    QPushButton *m_clearAllSyncPointsButton = nullptr;

    QLabel *m_keyframesInspectorClipLabel = nullptr;
    QLabel *m_keyframesInspectorDetailsLabel = nullptr;
    QTableWidget *m_videoKeyframeTable = nullptr;
    QCheckBox *m_keyframesAutoScrollCheckBox = nullptr;
    QCheckBox *m_keyframesFollowCurrentCheckBox = nullptr;
    QDoubleSpinBox *m_videoTranslationXSpin = nullptr;
    QDoubleSpinBox *m_videoTranslationYSpin = nullptr;
    QDoubleSpinBox *m_videoRotationSpin = nullptr;
    QDoubleSpinBox *m_videoScaleXSpin = nullptr;
    QDoubleSpinBox *m_videoScaleYSpin = nullptr;
    QComboBox *m_videoInterpolationCombo = nullptr;
    QCheckBox *m_mirrorHorizontalCheckBox = nullptr;
    QCheckBox *m_mirrorVerticalCheckBox = nullptr;
    QCheckBox *m_lockVideoScaleCheckBox = nullptr;
    QCheckBox *m_keyframeSpaceCheckBox = nullptr;
    QCheckBox *m_keyframeSkipAwareTimingCheckBox = nullptr;
    QPushButton *m_addVideoKeyframeButton = nullptr;
    QPushButton *m_removeVideoKeyframeButton = nullptr;
    QPushButton *m_flipHorizontalButton = nullptr;

    QLabel *m_titlesInspectorClipLabel = nullptr;
    QLabel *m_titlesInspectorDetailsLabel = nullptr;
    QTableWidget *m_titleKeyframeTable = nullptr;
    QPlainTextEdit *m_titleTextEdit = nullptr;
    QDoubleSpinBox *m_titleXSpin = nullptr;
    QDoubleSpinBox *m_titleYSpin = nullptr;
    QDoubleSpinBox *m_titleFontSizeSpin = nullptr;
    QDoubleSpinBox *m_titleOpacitySpin = nullptr;
    QFontComboBox *m_titleFontCombo = nullptr;
    QCheckBox *m_titleBoldCheck = nullptr;
    QCheckBox *m_titleItalicCheck = nullptr;
    QPushButton *m_titleColorButton = nullptr;
    QCheckBox *m_titleShadowEnabledCheck = nullptr;
    QPushButton *m_titleShadowColorButton = nullptr;
    QDoubleSpinBox *m_titleShadowOpacitySpin = nullptr;
    QDoubleSpinBox *m_titleShadowOffsetXSpin = nullptr;
    QDoubleSpinBox *m_titleShadowOffsetYSpin = nullptr;
    QCheckBox *m_titleWindowEnabledCheck = nullptr;
    QPushButton *m_titleWindowColorButton = nullptr;
    QDoubleSpinBox *m_titleWindowOpacitySpin = nullptr;
    QDoubleSpinBox *m_titleWindowPaddingSpin = nullptr;
    QCheckBox *m_titleWindowFrameEnabledCheck = nullptr;
    QPushButton *m_titleWindowFrameColorButton = nullptr;
    QDoubleSpinBox *m_titleWindowFrameOpacitySpin = nullptr;
    QDoubleSpinBox *m_titleWindowFrameWidthSpin = nullptr;
    QDoubleSpinBox *m_titleWindowFrameGapSpin = nullptr;
    QCheckBox *m_titleAutoScrollCheck = nullptr;
    QPushButton *m_addTitleKeyframeButton = nullptr;
    QPushButton *m_removeTitleKeyframeButton = nullptr;
    QPushButton *m_titleCenterHorizontalButton = nullptr;
    QPushButton *m_titleCenterVerticalButton = nullptr;

    QLabel *m_audioInspectorClipLabel = nullptr;
    QLabel *m_audioInspectorDetailsLabel = nullptr;

    QLineEdit *m_transcriptInspectorClipLabel = nullptr;
    QLabel *m_transcriptInspectorDetailsLabel = nullptr;
    QTableWidget *m_transcriptTable = nullptr;
    QLabel *m_clipInspectorClipLabel = nullptr;
    QLabel *m_clipProxyUsageLabel = nullptr;
    QLabel *m_clipPlaybackSourceLabel = nullptr;
    QLabel *m_clipOriginalInfoLabel = nullptr;
    QLabel *m_clipProxyInfoLabel = nullptr;
    QDoubleSpinBox *m_clipPlaybackRateSpin = nullptr;
    QLabel *m_trackInspectorLabel = nullptr;
    QLabel *m_trackInspectorDetailsLabel = nullptr;
    QLineEdit *m_trackNameEdit = nullptr;
    QSpinBox *m_trackHeightSpin = nullptr;
    QComboBox *m_trackVisualModeCombo = nullptr;
    QCheckBox *m_trackAudioEnabledCheckBox = nullptr;
    QDoubleSpinBox *m_trackCrossfadeSecondsSpin = nullptr;
    QPushButton *m_trackCrossfadeButton = nullptr;
    QCheckBox *m_previewHideOutsideOutputCheckBox = nullptr;
    QCheckBox *m_previewShowSpeakerTrackPointsCheckBox = nullptr;
    QComboBox *m_previewVulkanPresenterCombo = nullptr;
    QDoubleSpinBox *m_previewZoomSpin = nullptr;
    QPushButton *m_previewZoomResetButton = nullptr;
    QCheckBox *m_previewPlaybackCacheFallbackCheckBox = nullptr;
    QCheckBox *m_previewLeadPrefetchEnabledCheckBox = nullptr;
    QSpinBox *m_previewLeadPrefetchCountSpin = nullptr;
    QSpinBox *m_previewPlaybackWindowAheadSpin = nullptr;
    QSpinBox *m_previewVisibleQueueReserveSpin = nullptr;
    QSpinBox *m_timelineAudioEnvelopeGranularitySpin = nullptr;
    QCheckBox *m_preferencesFeatureAiPanelCheckBox = nullptr;
    QCheckBox *m_preferencesFeatureAiSpeakerCleanupCheckBox = nullptr;
    QCheckBox *m_preferencesFeatureAudioPreviewModeCheckBox = nullptr;
    QCheckBox *m_preferencesFeatureAudioDynamicsToolsCheckBox = nullptr;
    QCheckBox *m_audioAmplifyEnabledCheckBox = nullptr;
    QDoubleSpinBox *m_audioAmplifyDbSpin = nullptr;
    QCheckBox *m_audioSpeakerHoverModalCheckBox = nullptr;
    QCheckBox *m_audioShowWaveformCheckBox = nullptr;
    QCheckBox *m_audioWaveformPreviewProcessedCheckBox = nullptr;
    QCheckBox *m_audioNormalizeEnabledCheckBox = nullptr;
    QDoubleSpinBox *m_audioNormalizeTargetDbSpin = nullptr;
    QCheckBox *m_audioSelectiveNormalizeEnabledCheckBox = nullptr;
    QDoubleSpinBox *m_audioSelectiveNormalizeMinSecondsSpin = nullptr;
    QDoubleSpinBox *m_audioSelectiveNormalizePeakDbSpin = nullptr;
    QSpinBox *m_audioSelectiveNormalizePassesSpin = nullptr;
    QCheckBox *m_audioSelectiveNormalizeOverlayVisibleCheckBox = nullptr;
    QCheckBox *m_audioTranscriptNormalizeEnabledCheckBox = nullptr;
    QCheckBox *m_audioPeakReductionEnabledCheckBox = nullptr;
    QDoubleSpinBox *m_audioPeakThresholdDbSpin = nullptr;
    QCheckBox *m_audioLimiterEnabledCheckBox = nullptr;
    QDoubleSpinBox *m_audioLimiterThresholdDbSpin = nullptr;
    QCheckBox *m_audioCompressorEnabledCheckBox = nullptr;
    QDoubleSpinBox *m_audioCompressorThresholdDbSpin = nullptr;
    QDoubleSpinBox *m_audioCompressorRatioSpin = nullptr;
    QTableWidget *m_profileSummaryTable = nullptr;
    QComboBox *m_profileH26xThreadingModeCombo = nullptr;
    QTableWidget *m_clipsTable = nullptr;
    QTableWidget *m_historyTable = nullptr;
    QTableWidget *m_tracksTable = nullptr;
    QPushButton *m_profileBenchmarkButton = nullptr;
    QPushButton *m_restartDecodersButton = nullptr;
    QLabel *m_projectSectionLabel = nullptr;
    QListWidget *m_projectsList = nullptr;
    QPushButton *m_newProjectButton = nullptr;
    QPushButton *m_saveProjectAsButton = nullptr;
    QPushButton *m_renameProjectButton = nullptr;
    QLabel *m_aiStatusLabel = nullptr;
    QComboBox *m_aiModelCombo = nullptr;
    QPushButton *m_aiTranscribeButton = nullptr;
    QPushButton *m_aiFindSpeakerNamesButton = nullptr;
    QPushButton *m_aiFindOrganizationsButton = nullptr;
    QPushButton *m_aiCleanAssignmentsButton = nullptr;
    QPushButton *m_aiSubscribeButton = nullptr;
    QTextBrowser *m_aiChatHistoryEdit = nullptr;
    QPlainTextEdit *m_aiChatInputLineEdit = nullptr;
    QPushButton *m_aiChatSendButton = nullptr;
    QPushButton *m_aiChatClearButton = nullptr;
    QLabel *m_accessStatusLabel = nullptr;
    QTableWidget *m_accessTable = nullptr;
    QPushButton *m_accessRefreshButton = nullptr;
    QCheckBox *m_transcriptOverlayEnabledCheckBox = nullptr;
    QCheckBox *m_transcriptBackgroundVisibleCheckBox = nullptr;
    QCheckBox *m_transcriptShadowEnabledCheckBox = nullptr;
    QCheckBox *m_transcriptShowSpeakerTitleCheckBox = nullptr;
    QSpinBox *m_transcriptMaxLinesSpin = nullptr;
    QSpinBox *m_transcriptMaxCharsSpin = nullptr;
    QCheckBox *m_transcriptAutoScrollCheckBox = nullptr;
    QDoubleSpinBox *m_transcriptOverlayXSpin = nullptr;
    QDoubleSpinBox *m_transcriptOverlayYSpin = nullptr;
    QPushButton *m_transcriptCenterHorizontalButton = nullptr;
    QPushButton *m_transcriptCenterVerticalButton = nullptr;
    QSpinBox *m_transcriptOverlayWidthSpin = nullptr;
    QSpinBox *m_transcriptOverlayHeightSpin = nullptr;
    QFontComboBox *m_transcriptFontFamilyCombo = nullptr;
    QSpinBox *m_transcriptFontSizeSpin = nullptr;
    QCheckBox *m_transcriptBoldCheckBox = nullptr;
    QCheckBox *m_transcriptItalicCheckBox = nullptr;
    QCheckBox *m_transcriptFollowCurrentWordCheckBox = nullptr;
    QCheckBox *m_transcriptUnifiedEditModeCheckBox = nullptr;
    QLineEdit *m_transcriptSearchFilterLineEdit = nullptr;
    QComboBox *m_transcriptSpeakerFilterCombo = nullptr;
    QComboBox *m_transcriptScriptVersionCombo = nullptr;
    QPushButton *m_transcriptNewVersionButton = nullptr;
    QPushButton *m_transcriptDeleteVersionButton = nullptr;
    QCheckBox *m_transcriptShowExcludedLinesCheckBox = nullptr;
    QLabel *m_speakersInspectorClipLabel = nullptr;
    QLabel *m_speakersInspectorDetailsLabel = nullptr;
    QTableWidget *m_speakersTable = nullptr;
    QLabel *m_selectedSpeakerIdLabel = nullptr;
    QLabel *m_selectedSpeakerRef1ImageLabel = nullptr;
    QLabel *m_selectedSpeakerRef2ImageLabel = nullptr;
    QPushButton *m_selectedSpeakerPreviousSentenceButton = nullptr;
    QPushButton *m_selectedSpeakerNextSentenceButton = nullptr;
    QPushButton *m_selectedSpeakerRandomSentenceButton = nullptr;
    QLabel *m_speakerCurrentSentenceLabel = nullptr;
    QPushButton *m_speakerSetReference1Button = nullptr;
    QPushButton *m_speakerSetReference2Button = nullptr;
    QPushButton *m_speakerPickReference1Button = nullptr;
    QPushButton *m_speakerPickReference2Button = nullptr;
    QPushButton *m_speakerClearReferencesButton = nullptr;
    QPushButton *m_speakerRunAutoTrackButton = nullptr;
    QPushButton *m_speakerBoxstreamSettingsButton = nullptr;
    QPushButton *m_speakerEnableTrackingButton = nullptr;
    QPushButton *m_speakerDisableTrackingButton = nullptr;
    QPushButton *m_speakerDeletePointstreamButton = nullptr;
    QPushButton *m_speakerGuideButton = nullptr;
    QPushButton *m_speakerPrecropFacesButton = nullptr;
    QPushButton *m_speakerAiFindNamesButton = nullptr;
    QPushButton *m_speakerAiFindOrganizationsButton = nullptr;
    QPushButton *m_speakerAiCleanAssignmentsButton = nullptr;
    QCheckBox *m_speakerDebugCaptureCheckBox = nullptr;
    QPushButton *m_speakerOpenLatestDebugRunButton = nullptr;
    QPushButton *m_speakerExportDebugBundleButton = nullptr;
    QLabel *m_speakerDebugStatusLabel = nullptr;
    QLabel *m_speakerTrackingStatusLabel = nullptr;
    QDoubleSpinBox *m_speakerFramingTargetXSpin = nullptr;
    QDoubleSpinBox *m_speakerFramingTargetYSpin = nullptr;
    QDoubleSpinBox *m_speakerFramingTargetBoxSpin = nullptr;
    QCheckBox *m_speakerFramingZoomEnabledCheckBox = nullptr;
    QCheckBox *m_speakerShowBoxStreamBoxesCheckBox = nullptr;
    QCheckBox *m_speakerApplyFramingToClipCheckBox = nullptr;
    QLabel *m_speakerClipFramingStatusLabel = nullptr;
    QLabel *m_speakerRefsChipLabel = nullptr;
    QLabel *m_speakerPointstreamChipLabel = nullptr;
    QPushButton *m_speakerTrackingChipButton = nullptr;
    QPushButton *m_speakerStabilizeChipButton = nullptr;
    QTableWidget *m_speakerBoxStreamTable = nullptr;
    QPlainTextEdit *m_speakerBoxStreamDetailsEdit = nullptr;
    QComboBox *m_speakerBoxStreamOverlaySourceCombo = nullptr;

    QSpinBox *m_outputWidthSpin = nullptr;
    QSpinBox *m_outputHeightSpin = nullptr;
    QSpinBox *m_exportStartSpin = nullptr;
    QSpinBox *m_exportEndSpin = nullptr;
    QComboBox *m_outputFormatCombo = nullptr;
    QComboBox *m_renderBackendCombo = nullptr;
    QLabel *m_outputRangeSummaryLabel = nullptr;
    QCheckBox *m_renderUseProxiesCheckBox = nullptr;
    QCheckBox *m_renderCreateVideoFromSequenceCheckBox = nullptr;
    QCheckBox *m_outputPlaybackCacheFallbackCheckBox = nullptr;
    QCheckBox *m_outputLeadPrefetchEnabledCheckBox = nullptr;
    QSpinBox *m_outputLeadPrefetchCountSpin = nullptr;
    QSpinBox *m_outputPlaybackWindowAheadSpin = nullptr;
    QSpinBox *m_outputVisibleQueueReserveSpin = nullptr;
    QSpinBox *m_outputPrefetchMaxQueueDepthSpin = nullptr;
    QSpinBox *m_outputPrefetchMaxInflightSpin = nullptr;
    QSpinBox *m_outputPrefetchMaxPerTickSpin = nullptr;
    QSpinBox *m_outputPrefetchSkipVisiblePendingThresholdSpin = nullptr;
    QSpinBox *m_outputDecoderLaneCountSpin = nullptr;
    QComboBox *m_outputDecodeModeCombo = nullptr;
    QCheckBox *m_outputDeterministicPipelineCheckBox = nullptr;
    QPushButton *m_outputResetPipelineDefaultsButton = nullptr;
    QSpinBox *m_autosaveIntervalMinutesSpin = nullptr;
    QSpinBox *m_autosaveMaxBackupsSpin = nullptr;
    QPushButton *m_renderButton = nullptr;
    QPushButton *m_backgroundColorButton = nullptr;

    QCheckBox *m_speechFilterEnabledCheckBox = nullptr;
    QSpinBox *m_transcriptPrependMsSpin = nullptr;
    QSpinBox *m_transcriptPostpendMsSpin = nullptr;
    QSpinBox *m_speechFilterFadeSamplesSpin = nullptr;
    QCheckBox *m_speechFilterRangeCrossfadeCheckBox = nullptr;
    QComboBox *m_playbackClockSourceCombo = nullptr;
    QComboBox *m_playbackAudioWarpModeCombo = nullptr;
};
