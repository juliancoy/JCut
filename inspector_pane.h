#pragma once

#include "speakers_table.h"

#include <QWidget>

class QLabel;
class QTabBar;
class QTabWidget;
class QHBoxLayout;
class QDoubleSpinBox;
class QSpinBox;
class QSlider;
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
    QTabWidget *speakersSubtabs() const { return m_speakersSubtabs; }

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
    QCheckBox *sourceTransformLockCheckBox() const { return m_sourceTransformLockCheckBox; }
    QCheckBox *keyframeSpaceCheckBox() const { return m_keyframeSpaceCheckBox; }
    QCheckBox *keyframeSkipAwareTimingCheckBox() const { return m_keyframeSkipAwareTimingCheckBox; }

    QCheckBox *transcriptOverlayEnabledCheckBox() const { return m_transcriptOverlayEnabledCheckBox; }
    QComboBox *transcriptPlacementModeCombo() const { return m_transcriptPlacementModeCombo; }
    QCheckBox *transcriptBackgroundVisibleCheckBox() const { return m_transcriptBackgroundVisibleCheckBox; }
    QSpinBox *transcriptBackgroundOpacitySpin() const { return m_transcriptBackgroundOpacitySpin; }
    QSpinBox *transcriptBackgroundCornerRadiusSpin() const { return m_transcriptBackgroundCornerRadiusSpin; }
    QSpinBox *transcriptTextOpacitySpin() const { return m_transcriptTextOpacitySpin; }
    QSpinBox *transcriptBackgroundPaddingSpin() const { return m_transcriptBackgroundPaddingSpin; }
    QCheckBox *transcriptBackgroundFrameCheckBox() const { return m_transcriptBackgroundFrameCheckBox; }
    QPushButton *transcriptBackgroundFrameColorButton() const { return m_transcriptBackgroundFrameColorButton; }
    QSpinBox *transcriptBackgroundFrameOpacitySpin() const { return m_transcriptBackgroundFrameOpacitySpin; }
    QSpinBox *transcriptBackgroundFrameWidthSpin() const { return m_transcriptBackgroundFrameWidthSpin; }
    QSpinBox *transcriptBackgroundFrameGapSpin() const { return m_transcriptBackgroundFrameGapSpin; }
    QPushButton *transcriptTextColorButton() const { return m_transcriptTextColorButton; }
    QPushButton *transcriptBackgroundColorButton() const { return m_transcriptBackgroundColorButton; }
    QPushButton *transcriptHighlightColorButton() const { return m_transcriptHighlightColorButton; }
    QCheckBox *transcriptShadowEnabledCheckBox() const { return m_transcriptShadowEnabledCheckBox; }
    QPushButton *transcriptShadowColorButton() const { return m_transcriptShadowColorButton; }
    QSpinBox *transcriptShadowOpacitySpin() const { return m_transcriptShadowOpacitySpin; }
    QSpinBox *transcriptShadowOffsetXSpin() const { return m_transcriptShadowOffsetXSpin; }
    QSpinBox *transcriptShadowOffsetYSpin() const { return m_transcriptShadowOffsetYSpin; }
    QCheckBox *transcriptOutlineEnabledCheckBox() const { return m_transcriptOutlineEnabledCheckBox; }
    QPushButton *transcriptOutlineColorButton() const { return m_transcriptOutlineColorButton; }
    QSpinBox *transcriptOutlineWidthSpin() const { return m_transcriptOutlineWidthSpin; }
    QSpinBox *transcriptOutlineOpacitySpin() const { return m_transcriptOutlineOpacitySpin; }
    QComboBox *transcriptTextExtrudeModeCombo() const { return m_transcriptTextExtrudeModeCombo; }
    QDoubleSpinBox *transcriptTextExtrudeDepthSpin() const { return m_transcriptTextExtrudeDepthSpin; }
    QDoubleSpinBox *transcriptTextExtrudeBevelSpin() const { return m_transcriptTextExtrudeBevelSpin; }
    QCheckBox *transcriptShowSpeakerTitleCheckBox() const { return m_transcriptShowSpeakerTitleCheckBox; }
    QCheckBox *transcriptHighlightCurrentWordCheckBox() const { return m_transcriptHighlightCurrentWordCheckBox; }
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
    QPushButton *transcriptExportTextButton() const { return m_transcriptExportTextButton; }
    QCheckBox *transcriptShowExcludedLinesCheckBox() const { return m_transcriptShowExcludedLinesCheckBox; }
    QLabel *speakersInspectorClipLabel() const { return m_speakersInspectorClipLabel; }
    QLabel *speakersInspectorDetailsLabel() const { return m_speakersInspectorDetailsLabel; }
    QTableWidget *speakersTable() const { return m_speakersTable; }
    QCheckBox *speakerHideUnidentifiedCheckBox() const { return m_speakerHideUnidentifiedCheckBox; }
    QCheckBox *speakerShowContiguousSectionsCheckBox() const { return m_speakerShowContiguousSectionsCheckBox; }
    QCheckBox *speakerApplyTrackToAllMatchingSectionsCheckBox() const { return m_speakerApplyTrackToAllMatchingSectionsCheckBox; }
    QSpinBox *speakerSectionMinimumWordsSpin() const { return m_speakerSectionMinimumWordsSpin; }
    QPushButton *speakerExportLongSectionsButton() const { return m_speakerExportLongSectionsButton; }
    QPushButton *speakerCreateTitleClipsButton() const { return m_speakerCreateTitleClipsButton; }
    QCheckBox *speakerOverlayCreateTitleClipsButton() const { return m_speakerOverlayCreateTitleClipsButton; }
    QComboBox *speakerOverlayFlyInStyleCombo() const { return m_speakerOverlayFlyInStyleCombo; }
    QDoubleSpinBox *speakerOverlayFlyInDelaySpin() const { return m_speakerOverlayFlyInDelaySpin; }
    QDoubleSpinBox *speakerOverlayFlyInDurationSpin() const { return m_speakerOverlayFlyInDurationSpin; }
    QDoubleSpinBox *speakerOverlayFlyInTimeSpin() const { return m_speakerOverlayFlyInTimeSpin; }
    QDoubleSpinBox *speakerOverlayWrapRadiusSpin() const { return m_speakerOverlayWrapRadiusSpin; }
    QDoubleSpinBox *speakerOverlayWrapDepthSpin() const { return m_speakerOverlayWrapDepthSpin; }
    QDoubleSpinBox *speakerOverlayWrapStartAngleSpin() const { return m_speakerOverlayWrapStartAngleSpin; }
    QDoubleSpinBox *speakerOverlayWrapEndAngleSpin() const { return m_speakerOverlayWrapEndAngleSpin; }
    QDoubleSpinBox *speakerOverlayWrapPitchSpin() const { return m_speakerOverlayWrapPitchSpin; }
    QDoubleSpinBox *speakerOverlayWrapRollSpin() const { return m_speakerOverlayWrapRollSpin; }
    QDoubleSpinBox *speakerOverlayRotationXSpin() const { return m_speakerOverlayRotationXSpin; }
    QDoubleSpinBox *speakerOverlayRotationYSpin() const { return m_speakerOverlayRotationYSpin; }
    QDoubleSpinBox *speakerOverlayRotationZSpin() const { return m_speakerOverlayRotationZSpin; }
    QSpinBox *speakerOverlayTitleFontSizeSpin() const { return m_speakerOverlayTitleFontSizeSpin; }
    QCheckBox *speakerOverlayTitleAutoFitCheckBox() const { return m_speakerOverlayTitleAutoFitCheckBox; }
    QSpinBox *speakerOverlayTitleBoxWidthSpin() const { return m_speakerOverlayTitleBoxWidthSpin; }
    QComboBox *speakerOverlayTitleTextMaterialCombo() const { return m_speakerOverlayTitleTextMaterialCombo; }
    QComboBox *speakerOverlayTitleBorderMaterialCombo() const { return m_speakerOverlayTitleBorderMaterialCombo; }
    QLineEdit *speakerOverlayTitleTextPatternPathEdit() const { return m_speakerOverlayTitleTextPatternPathEdit; }
    QLineEdit *speakerOverlayTitleBorderPatternPathEdit() const { return m_speakerOverlayTitleBorderPatternPathEdit; }
    QDoubleSpinBox *speakerOverlayTitlePatternScaleSpin() const { return m_speakerOverlayTitlePatternScaleSpin; }
    QCheckBox *speakerOverlayTitleExtrudeCheckBox() const { return m_speakerOverlayTitleExtrudeCheckBox; }
    QComboBox *speakerOverlayTitleExtrudeModeCombo() const { return m_speakerOverlayTitleExtrudeModeCombo; }
    QDoubleSpinBox *speakerOverlayTitleExtrudeDepthSpin() const { return m_speakerOverlayTitleExtrudeDepthSpin; }
    QDoubleSpinBox *speakerOverlayTitleBevelScaleSpin() const { return m_speakerOverlayTitleBevelScaleSpin; }
    QCheckBox *speakerShowCurrentSpeakerNameCheckBox() const { return m_speakerShowCurrentSpeakerNameCheckBox; }
    QCheckBox *speakerShowCurrentSpeakerOrganizationCheckBox() const { return m_speakerShowCurrentSpeakerOrganizationCheckBox; }
    QSpinBox *speakerCurrentSpeakerNameTextSizeSpin() const { return m_speakerCurrentSpeakerNameTextSizeSpin; }
    QSpinBox *speakerCurrentSpeakerOrganizationTextSizeSpin() const { return m_speakerCurrentSpeakerOrganizationTextSizeSpin; }
    QSpinBox *speakerCurrentSpeakerNameYPositionSpin() const { return m_speakerCurrentSpeakerNameYPositionSpin; }
    QSpinBox *speakerCurrentSpeakerOrganizationYPositionSpin() const { return m_speakerCurrentSpeakerOrganizationYPositionSpin; }
    QPushButton *speakerCurrentSpeakerNameColorButton() const { return m_speakerCurrentSpeakerNameColorButton; }
    QPushButton *speakerCurrentSpeakerOrganizationColorButton() const { return m_speakerCurrentSpeakerOrganizationColorButton; }
    QPushButton *speakerCurrentSpeakerBackgroundColorButton() const { return m_speakerCurrentSpeakerBackgroundColorButton; }
    QCheckBox *speakerCurrentSpeakerBackgroundVisibleCheckBox() const { return m_speakerCurrentSpeakerBackgroundVisibleCheckBox; }
    QSpinBox *speakerCurrentSpeakerBackgroundOpacitySpin() const { return m_speakerCurrentSpeakerBackgroundOpacitySpin; }
    QPushButton *speakerCurrentSpeakerBorderColorButton() const { return m_speakerCurrentSpeakerBorderColorButton; }
    QSpinBox *speakerCurrentSpeakerBorderOpacitySpin() const { return m_speakerCurrentSpeakerBorderOpacitySpin; }
    QSpinBox *speakerCurrentSpeakerBackgroundRadiusSpin() const { return m_speakerCurrentSpeakerBackgroundRadiusSpin; }
    QSpinBox *speakerCurrentSpeakerBorderWidthSpin() const { return m_speakerCurrentSpeakerBorderWidthSpin; }
    QCheckBox *speakerCurrentSpeakerShadowCheckBox() const { return m_speakerCurrentSpeakerShadowCheckBox; }
    QPushButton *speakerCurrentSpeakerShadowColorButton() const { return m_speakerCurrentSpeakerShadowColorButton; }
    QSpinBox *speakerCurrentSpeakerShadowOpacitySpin() const { return m_speakerCurrentSpeakerShadowOpacitySpin; }
    QTableWidget *speakerSectionsTable() const { return m_speakerSectionsTable; }
    QWidget *selectedSpeakerPopup() const { return m_selectedSpeakerPopup; }
    QLabel *selectedSpeakerIdLabel() const { return m_selectedSpeakerIdLabel; }
    QLineEdit *selectedSpeakerNameEdit() const { return m_selectedSpeakerNameEdit; }
    QLineEdit *selectedSpeakerOrganizationEdit() const { return m_selectedSpeakerOrganizationEdit; }
    QLineEdit *selectedSpeakerLogoPathEdit() const { return m_selectedSpeakerLogoPathEdit; }
    QLineEdit *selectedSpeakerPrimaryColorEdit() const { return m_selectedSpeakerPrimaryColorEdit; }
    QLineEdit *selectedSpeakerSecondaryColorEdit() const { return m_selectedSpeakerSecondaryColorEdit; }
    QLineEdit *selectedSpeakerAccentColorEdit() const { return m_selectedSpeakerAccentColorEdit; }
    QCheckBox *selectedSpeakerGradingEnabledCheckBox() const { return m_selectedSpeakerGradingEnabledCheckBox; }
    QDoubleSpinBox *selectedSpeakerBrightnessSpin() const { return m_selectedSpeakerBrightnessSpin; }
    QDoubleSpinBox *selectedSpeakerContrastSpin() const { return m_selectedSpeakerContrastSpin; }
    QDoubleSpinBox *selectedSpeakerSaturationSpin() const { return m_selectedSpeakerSaturationSpin; }
    QListWidget *selectedSpeakerFaceDetectionsList() const { return m_selectedSpeakerFaceDetectionsList; }
    QListWidget *speakerPlayheadFaceDetectionsList() const { return m_speakerPlayheadFaceDetectionsList; }
    QCheckBox *speakerShowPlayheadFaceDetectionsCheckBox() const { return m_speakerShowPlayheadFaceDetectionsCheckBox; }
    QPushButton *selectedSpeakerPreviousSentenceButton() const { return m_selectedSpeakerPreviousSentenceButton; }
    QPushButton *selectedSpeakerNextSentenceButton() const { return m_selectedSpeakerNextSentenceButton; }
    QPushButton *selectedSpeakerNextSectionButton() const { return m_selectedSpeakerNextSectionButton; }
    QPushButton *selectedSpeakerRandomSentenceButton() const { return m_selectedSpeakerRandomSentenceButton; }
    QLabel *speakerCurrentSentenceLabel() const { return m_speakerCurrentSentenceLabel; }
    QTableWidget *speakerTranscriptTable() const { return m_speakerTranscriptTable; }
    QPushButton *speakerRunAutoTrackButton() const { return m_speakerRunAutoTrackButton; }
    QPushButton *speakerViewFacestreamButton() const { return m_speakerViewFacestreamButton; }
    QPushButton *speakerFacestreamSettingsButton() const { return m_speakerFacestreamSettingsButton; }
    QPushButton *speakerRefreshTrackAvatarsButton() const { return m_speakerRefreshTrackAvatarsButton; }
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
    QDoubleSpinBox *speakerSectionRotationSpin() const { return m_speakerSectionRotationSpin; }
    QCheckBox *speakerFramingZoomEnabledCheckBox() const { return m_speakerFramingZoomEnabledCheckBox; }
    QSpinBox *speakerFramingCenterSmoothingFramesSpin() const { return m_speakerFramingCenterSmoothingFramesSpin; }
    QSpinBox *speakerFramingZoomSmoothingFramesSpin() const { return m_speakerFramingZoomSmoothingFramesSpin; }
    QComboBox *speakerFramingSmoothingModeCombo() const { return m_speakerFramingSmoothingModeCombo; }
    QDoubleSpinBox *speakerFramingCenterSmoothingStrengthSpin() const { return m_speakerFramingCenterSmoothingStrengthSpin; }
    QDoubleSpinBox *speakerFramingZoomSmoothingStrengthSpin() const { return m_speakerFramingZoomSmoothingStrengthSpin; }
    QSpinBox *speakerFramingGapHoldFramesSpin() const { return m_speakerFramingGapHoldFramesSpin; }
    QCheckBox *speakerShowFaceDetectionsBoxesCheckBox() const { return m_speakerShowFaceDetectionsBoxesCheckBox; }
    QCheckBox *speakerShowRawDetectionsCheckBox() const { return m_speakerShowRawDetectionsCheckBox; }
    QCheckBox *speakerApplyFramingToClipCheckBox() const { return m_speakerApplyFramingToClipCheckBox; }
    QTableWidget *speakerFramingEnabledKeyframeTable() const { return m_speakerFramingEnabledKeyframeTable; }
    QLabel *speakerClipFramingStatusLabel() const { return m_speakerClipFramingStatusLabel; }
    QLabel *speakerRefsChipLabel() const { return m_speakerRefsChipLabel; }
    QLabel *speakerPointstreamChipLabel() const { return m_speakerPointstreamChipLabel; }
    QPushButton *speakerTrackingChipButton() const { return m_speakerTrackingChipButton; }
    QPushButton *speakerStabilizeChipButton() const { return m_speakerStabilizeChipButton; }
    QTableWidget *speakerFaceDetectionsTable() const { return m_speakerFaceDetectionsTable; }
    QPlainTextEdit *speakerFaceDetectionsDetailsEdit() const { return m_speakerFaceDetectionsDetailsEdit; }
    QCheckBox *speakerDetectionsAvailableCheckBox() const { return m_speakerDetectionsAvailableCheckBox; }
    QCheckBox *speakerTracksAvailableCheckBox() const { return m_speakerTracksAvailableCheckBox; }
    QTableWidget *speakerRawDetectionTable() const { return m_speakerRawDetectionTable; }
    QPlainTextEdit *speakerRawDetectionDetailsEdit() const { return m_speakerRawDetectionDetailsEdit; }
    QLabel *processingJobsSummaryLabel() const { return m_processingJobsSummaryLabel; }
    QTableWidget *processingJobsTable() const { return m_processingJobsTable; }

    QLabel *gradingPathLabel() const { return m_gradingPathLabel; }
    QTableWidget *gradingKeyframeTable() const { return m_gradingKeyframeTable; }
    QCheckBox *gradingAutoScrollCheckBox() const { return m_gradingAutoScrollCheckBox; }
    QCheckBox *gradingFollowCurrentCheckBox() const { return m_gradingFollowCurrentCheckBox; }
    QCheckBox *gradingPreviewCheckBox() const { return m_gradingPreviewCheckBox; }
    QPushButton *gradingKeyAtPlayheadButton() const { return m_gradingKeyAtPlayheadButton; }
    QPushButton *gradingResetButton() const { return m_gradingResetButton; }
    QPushButton *gradingNormalizeCurvesButton() const { return m_gradingNormalizeCurvesButton; }
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
    QComboBox *maskFeatherFalloffCombo() const { return m_maskFeatherFalloffCombo; }
    QCheckBox *maskFeatherEnabledCheck() const { return m_maskFeatherEnabledCheck; }
    QCheckBox *maskForegroundLayerCheck() const { return m_maskForegroundLayerCheck; }
    QCheckBox *maskRepeatEnabledCheck() const { return m_maskRepeatEnabledCheck; }
    QDoubleSpinBox *maskRepeatDeltaXSpin() const { return m_maskRepeatDeltaXSpin; }
    QDoubleSpinBox *maskRepeatDeltaYSpin() const { return m_maskRepeatDeltaYSpin; }
    QComboBox *effectPresetCombo() const { return m_effectPresetCombo; }
    QSpinBox *differenceReferenceFramesSpin() const { return m_differenceReferenceFramesSpin; }
    QDoubleSpinBox *differenceThresholdSpin() const { return m_differenceThresholdSpin; }
    QDoubleSpinBox *differenceSoftnessSpin() const { return m_differenceSoftnessSpin; }
    QSpinBox *temporalEchoCountSpin() const { return m_temporalEchoCountSpin; }
    QSpinBox *temporalEchoSpacingSpin() const { return m_temporalEchoSpacingSpin; }
    QDoubleSpinBox *temporalEchoDecaySpin() const { return m_temporalEchoDecaySpin; }
    QSpinBox *effectRowsSpin() const { return m_effectRowsSpin; }
    QDoubleSpinBox *effectSpeedSpin() const { return m_effectSpeedSpin; }
    QDoubleSpinBox *effectScaleSpin() const { return m_effectScaleSpin; }
    QCheckBox *effectAlternateDirectionCheck() const { return m_effectAlternateDirectionCheck; }
    QCheckBox *effectSpeechSyncCheck() const { return m_effectSpeechSyncCheck; }
    QComboBox *tilingPatternCombo() const { return m_tilingPatternCombo; }
    QDoubleSpinBox *tilingSpacingSpin() const { return m_tilingSpacingSpin; }
    QCheckBox *tilingWrapCheck() const { return m_tilingWrapCheck; }
    QLabel *maskClipLabel() const { return m_maskClipLabel; }
    QCheckBox *maskEnabledCheck() const { return m_maskEnabledCheck; }
    QLineEdit *maskFramesDirEdit() const { return m_maskFramesDirEdit; }
    QComboBox *maskSidecarCombo() const { return m_maskSidecarCombo; }
    QPushButton *maskBrowseButton() const { return m_maskBrowseButton; }
    QPushButton *maskNewPromptButton() const { return m_maskNewPromptButton; }
    QSpinBox *maskZLevelSpin() const { return m_maskZLevelSpin; }
    QDoubleSpinBox *maskShapeFeatherSpin() const { return m_maskShapeFeatherSpin; }
    QDoubleSpinBox *maskDilateSpin() const { return m_maskDilateSpin; }
    QDoubleSpinBox *maskErodeSpin() const { return m_maskErodeSpin; }
    QDoubleSpinBox *maskBlurSpin() const { return m_maskBlurSpin; }
    QComboBox *maskShapeFeatherFalloffCombo() const { return m_maskShapeFeatherFalloffCombo; }
    QDoubleSpinBox *maskShapeFeatherPowerSpin() const { return m_maskShapeFeatherPowerSpin; }
    QCheckBox *maskInvertCheck() const { return m_maskInvertCheck; }
    QCheckBox *maskShowOnlyCheck() const { return m_maskShowOnlyCheck; }
    QDoubleSpinBox *maskOpacitySpin() const { return m_maskOpacitySpin; }
    QCheckBox *maskShadowEnabledCheck() const { return m_maskShadowEnabledCheck; }
    QDoubleSpinBox *maskShadowRadiusSpin() const { return m_maskShadowRadiusSpin; }
    QDoubleSpinBox *maskShadowOffsetXSpin() const { return m_maskShadowOffsetXSpin; }
    QDoubleSpinBox *maskShadowOffsetYSpin() const { return m_maskShadowOffsetYSpin; }
    QDoubleSpinBox *maskShadowOpacitySpin() const { return m_maskShadowOpacitySpin; }
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
    QComboBox *titleTextExtrudeModeCombo() const { return m_titleTextExtrudeModeCombo; }
    QDoubleSpinBox *titleTextExtrudeDepthSpin() const { return m_titleTextExtrudeDepthSpin; }
    QDoubleSpinBox *titleTextExtrudeBevelSpin() const { return m_titleTextExtrudeBevelSpin; }
    QCheckBox *titleAutoScrollCheck() const { return m_titleAutoScrollCheck; }
    QPushButton *addTitleKeyframeButton() const { return m_addTitleKeyframeButton; }
    QPushButton *removeTitleKeyframeButton() const { return m_removeTitleKeyframeButton; }
    QPushButton *titleCenterHorizontalButton() const { return m_titleCenterHorizontalButton; }
    QPushButton *titleCenterVerticalButton() const { return m_titleCenterVerticalButton; }

    QLabel *audioInspectorClipLabel() const { return m_audioInspectorClipLabel; }
    QLabel *audioInspectorDetailsLabel() const { return m_audioInspectorDetailsLabel; }
    QLabel *audioTrackTitleLabel() const { return m_audioTrackTitleLabel; }
    QLabel *audioTrackDetailsLabel() const { return m_audioTrackDetailsLabel; }
    QLabel *audioCurrentSpeakerTitleLabel() const { return m_audioCurrentSpeakerTitleLabel; }
    QLabel *audioCurrentSpeakerDetailsLabel() const { return m_audioCurrentSpeakerDetailsLabel; }
    QWidget *pipelinePreviewHost() const { return m_pipelinePreviewHost; }
    QListWidget *pipelineStageList() const { return m_pipelineStageList; }

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
    QComboBox *audioVisualizationModeCombo() const { return m_audioVisualizationModeCombo; }
    QComboBox *audioBufferFramesCombo() const { return m_audioBufferFramesCombo; }
    QPushButton *loiaconoSpectrumSettingsButton() const { return m_loiaconoSpectrumSettingsButton; }
    QCheckBox *audioNormalizeEnabledCheckBox() const { return m_audioNormalizeEnabledCheckBox; }
    QDoubleSpinBox *audioNormalizeTargetDbSpin() const { return m_audioNormalizeTargetDbSpin; }
    QCheckBox *audioStereoToMonoCheckBox() const { return m_audioStereoToMonoCheckBox; }
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
    QCheckBox *audioSoftClipEnabledCheckBox() const { return m_audioSoftClipEnabledCheckBox; }
    QTableWidget *profileSummaryTable() const { return m_profileSummaryTable; }
    QComboBox *profileH26xThreadingModeCombo() const { return m_profileH26xThreadingModeCombo; }
    QTableWidget *clipsTable() const { return m_clipsTable; }
    QTableWidget *historyTable() const { return m_historyTable; }
    QTableWidget *tracksTable() const { return m_tracksTable; }
    QPushButton *profileBenchmarkButton() const { return m_profileBenchmarkButton; }
    QLabel *projectSectionLabel() const { return m_projectSectionLabel; }
    QLabel *projectPathLabel() const { return m_projectPathLabel; }
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
    QDoubleSpinBox *outputFpsSpin() const { return m_outputFpsSpin; }
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
    QSpinBox *historyMaxEntriesSpin() const { return m_historyMaxEntriesSpin; }
    QSpinBox *historyMaxMegabytesSpin() const { return m_historyMaxMegabytesSpin; }
    QPushButton *renderButton() const { return m_renderButton; }
    QPushButton *backgroundColorButton() const { return m_backgroundColorButton; }
    QComboBox *backgroundFillEffectCombo() const { return m_backgroundFillEffectCombo; }
    QDoubleSpinBox *backgroundFillOpacitySpin() const { return m_backgroundFillOpacitySpin; }
    QDoubleSpinBox *backgroundFillBrightnessSpin() const { return m_backgroundFillBrightnessSpin; }
    QDoubleSpinBox *backgroundFillSaturationSpin() const { return m_backgroundFillSaturationSpin; }
    QPushButton *restartDecodersButton() const { return m_restartDecodersButton; }

    QSpinBox *transcriptPrependMsSpin() const { return m_transcriptPrependMsSpin; }
    QSpinBox *transcriptPostpendMsSpin() const { return m_transcriptPostpendMsSpin; }
    QSpinBox *transcriptOffsetMsSpin() const { return m_transcriptOffsetMsSpin; }
    QComboBox *speechFilterFadeModeCombo() const { return m_speechFilterFadeModeCombo; }
    QSpinBox *speechFilterFadeSamplesSpin() const { return m_speechFilterFadeSamplesSpin; }
    QDoubleSpinBox *speechFilterCurveStrengthSpin() const { return m_speechFilterCurveStrengthSpin; }
    QCheckBox *speechFilterRangeCrossfadeCheckBox() const { return m_speechFilterRangeCrossfadeCheckBox; }
    QComboBox *speechFilterFrameTransitionModeCombo() const { return m_speechFilterFrameTransitionModeCombo; }
    QCheckBox *speechFilterFrameCrossfadeCheckBox() const { return m_speechFilterFrameCrossfadeCheckBox; }
    QSpinBox *speechFilterFrameCrossfadeFramesSpin() const { return m_speechFilterFrameCrossfadeFramesSpin; }
    QComboBox *playbackClockSourceCombo() const { return m_playbackClockSourceCombo; }
    QComboBox *playbackAudioWarpModeCombo() const { return m_playbackAudioWarpModeCombo; }

    void refresh();
    void refreshCurrentTab();
    void refreshTab(const QString& tabName);

signals:
    void refreshCurrentTabRequested();
    void refreshTabRequested(const QString& tabName);
    void restartDecodersRequested();

private:
    QWidget *buildPane();
    QWidget *buildGradingTab();
    QWidget *buildHistoryTab();
    QWidget *buildOpacityTab();
    QWidget *buildEffectsTab();
    QWidget *buildMasksTab();
    QWidget *buildCorrectionsTab();
    QWidget *buildTitlesTab();
    QWidget *buildSyncTab();
    QWidget *buildKeyframesTab();
    QWidget *buildTranscriptTab();
    QWidget *buildSpeakersTab();
    QWidget *buildSpeakersContinuityTab(QWidget *parent);
    QWidget *buildClipTab();
    QWidget *buildClipsTab();
    QWidget *buildTracksTab();
    QWidget *buildOutputTab();
    QWidget *buildPreviewTab();
    QWidget *buildAudioTab();
    QWidget *buildProcessingJobsTab();
    QWidget *buildPreferencesTab();
    QWidget *buildProfileTab();
    QWidget *buildPipelineTab();
    QWidget *buildProjectsTab();
    QWidget *buildAiTab();
    QWidget *buildAccessTab();
    void configureInspectorTabs();

private:
    QTabWidget *m_inspectorTabs = nullptr;
    QTabWidget *m_speakersSubtabs = nullptr;
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
    QPushButton *m_gradingResetButton = nullptr;
    QPushButton *m_gradingNormalizeCurvesButton = nullptr;
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
    QTabBar *m_gradingCurveChannelTabs = nullptr;
    QComboBox *m_gradingCurveChannelCombo = nullptr;
    GradingHistogramWidget *m_gradingHistogramWidget = nullptr;
    QCheckBox *m_gradingCurveThreePointLockCheckBox = nullptr;
    QCheckBox *m_gradingCurveSmoothingCheckBox = nullptr;
    QDoubleSpinBox *m_opacityFadeDurationSpin = nullptr;

    QLabel *m_effectsPathLabel = nullptr;
    QDoubleSpinBox *m_maskFeatherSpin = nullptr;
    QDoubleSpinBox *m_maskFeatherGammaSpin = nullptr;
    QComboBox *m_maskFeatherFalloffCombo = nullptr;
    QCheckBox *m_maskFeatherEnabledCheck = nullptr;
    QCheckBox *m_maskForegroundLayerCheck = nullptr;
    QCheckBox *m_maskRepeatEnabledCheck = nullptr;
    QDoubleSpinBox *m_maskRepeatDeltaXSpin = nullptr;
    QDoubleSpinBox *m_maskRepeatDeltaYSpin = nullptr;
    QComboBox *m_effectPresetCombo = nullptr;
    QSpinBox *m_effectRowsSpin = nullptr;
    QDoubleSpinBox *m_effectSpeedSpin = nullptr;
    QDoubleSpinBox *m_effectScaleSpin = nullptr;
    QCheckBox *m_effectAlternateDirectionCheck = nullptr;
    QCheckBox *m_effectSpeechSyncCheck = nullptr;
    QSpinBox *m_differenceReferenceFramesSpin = nullptr;
    QDoubleSpinBox *m_differenceThresholdSpin = nullptr;
    QDoubleSpinBox *m_differenceSoftnessSpin = nullptr;
    QSpinBox *m_temporalEchoCountSpin = nullptr;
    QSpinBox *m_temporalEchoSpacingSpin = nullptr;
    QDoubleSpinBox *m_temporalEchoDecaySpin = nullptr;
    QComboBox *m_tilingPatternCombo = nullptr;
    QDoubleSpinBox *m_tilingSpacingSpin = nullptr;
    QCheckBox *m_tilingWrapCheck = nullptr;
    QLabel *m_maskClipLabel = nullptr;
    QCheckBox *m_maskEnabledCheck = nullptr;
    QLineEdit *m_maskFramesDirEdit = nullptr;
    QComboBox *m_maskSidecarCombo = nullptr;
    QPushButton *m_maskBrowseButton = nullptr;
    QPushButton *m_maskNewPromptButton = nullptr;
    QSpinBox *m_maskZLevelSpin = nullptr;
    QDoubleSpinBox *m_maskShapeFeatherSpin = nullptr;
    QDoubleSpinBox *m_maskDilateSpin = nullptr;
    QDoubleSpinBox *m_maskErodeSpin = nullptr;
    QDoubleSpinBox *m_maskBlurSpin = nullptr;
    QComboBox *m_maskShapeFeatherFalloffCombo = nullptr;
    QDoubleSpinBox *m_maskShapeFeatherPowerSpin = nullptr;
    QCheckBox *m_maskInvertCheck = nullptr;
    QCheckBox *m_maskShowOnlyCheck = nullptr;
    QDoubleSpinBox *m_maskOpacitySpin = nullptr;
    QCheckBox *m_maskShadowEnabledCheck = nullptr;
    QDoubleSpinBox *m_maskShadowRadiusSpin = nullptr;
    QDoubleSpinBox *m_maskShadowOffsetXSpin = nullptr;
    QDoubleSpinBox *m_maskShadowOffsetYSpin = nullptr;
    QDoubleSpinBox *m_maskShadowOpacitySpin = nullptr;
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
    QCheckBox *m_sourceTransformLockCheckBox = nullptr;
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
    QComboBox *m_titleTextExtrudeModeCombo = nullptr;
    QDoubleSpinBox *m_titleTextExtrudeDepthSpin = nullptr;
    QDoubleSpinBox *m_titleTextExtrudeBevelSpin = nullptr;
    QCheckBox *m_titleAutoScrollCheck = nullptr;
    QPushButton *m_addTitleKeyframeButton = nullptr;
    QPushButton *m_removeTitleKeyframeButton = nullptr;
    QPushButton *m_titleCenterHorizontalButton = nullptr;
    QPushButton *m_titleCenterVerticalButton = nullptr;

    QLabel *m_audioInspectorClipLabel = nullptr;
    QLabel *m_audioInspectorDetailsLabel = nullptr;
    QLabel *m_audioCurrentSpeakerTitleLabel = nullptr;
    QLabel *m_audioCurrentSpeakerDetailsLabel = nullptr;
    QWidget *m_pipelinePreviewHost = nullptr;
    QListWidget *m_pipelineStageList = nullptr;

    QLineEdit *m_transcriptInspectorClipLabel = nullptr;
    QLabel *m_transcriptInspectorDetailsLabel = nullptr;
    QTableWidget *m_transcriptTable = nullptr;
    QWidget *m_transcriptSpeakerTitlesContainer = nullptr;
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
    QLabel *m_audioTrackTitleLabel = nullptr;
    QLabel *m_audioTrackDetailsLabel = nullptr;
    QCheckBox *m_audioSpeakerHoverModalCheckBox = nullptr;
    QCheckBox *m_audioShowWaveformCheckBox = nullptr;
    QCheckBox *m_audioWaveformPreviewProcessedCheckBox = nullptr;
    QComboBox *m_audioVisualizationModeCombo = nullptr;
    QComboBox *m_audioBufferFramesCombo = nullptr;
    QPushButton *m_loiaconoSpectrumSettingsButton = nullptr;
    QCheckBox *m_audioNormalizeEnabledCheckBox = nullptr;
    QDoubleSpinBox *m_audioNormalizeTargetDbSpin = nullptr;
    QCheckBox *m_audioStereoToMonoCheckBox = nullptr;
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
    QCheckBox *m_audioSoftClipEnabledCheckBox = nullptr;
    QTableWidget *m_profileSummaryTable = nullptr;
    QComboBox *m_profileH26xThreadingModeCombo = nullptr;
    QTableWidget *m_clipsTable = nullptr;
    QTableWidget *m_historyTable = nullptr;
    QTableWidget *m_tracksTable = nullptr;
    QPushButton *m_profileBenchmarkButton = nullptr;
    QPushButton *m_restartDecodersButton = nullptr;
    QLabel *m_projectSectionLabel = nullptr;
    QLabel *m_projectPathLabel = nullptr;
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
    QComboBox *m_transcriptPlacementModeCombo = nullptr;
    QCheckBox *m_transcriptBackgroundVisibleCheckBox = nullptr;
    QSpinBox *m_transcriptBackgroundOpacitySpin = nullptr;
    QSpinBox *m_transcriptBackgroundCornerRadiusSpin = nullptr;
    QSpinBox *m_transcriptTextOpacitySpin = nullptr;
    QSpinBox *m_transcriptBackgroundPaddingSpin = nullptr;
    QCheckBox *m_transcriptBackgroundFrameCheckBox = nullptr;
    QPushButton *m_transcriptBackgroundFrameColorButton = nullptr;
    QSpinBox *m_transcriptBackgroundFrameOpacitySpin = nullptr;
    QSpinBox *m_transcriptBackgroundFrameWidthSpin = nullptr;
    QSpinBox *m_transcriptBackgroundFrameGapSpin = nullptr;
    QPushButton *m_transcriptTextColorButton = nullptr;
    QPushButton *m_transcriptBackgroundColorButton = nullptr;
    QPushButton *m_transcriptHighlightColorButton = nullptr;
    QCheckBox *m_transcriptShadowEnabledCheckBox = nullptr;
    QPushButton *m_transcriptShadowColorButton = nullptr;
    QSpinBox *m_transcriptShadowOpacitySpin = nullptr;
    QSpinBox *m_transcriptShadowOffsetXSpin = nullptr;
    QSpinBox *m_transcriptShadowOffsetYSpin = nullptr;
    QCheckBox *m_transcriptOutlineEnabledCheckBox = nullptr;
    QPushButton *m_transcriptOutlineColorButton = nullptr;
    QSpinBox *m_transcriptOutlineWidthSpin = nullptr;
    QSpinBox *m_transcriptOutlineOpacitySpin = nullptr;
    QComboBox *m_transcriptTextExtrudeModeCombo = nullptr;
    QDoubleSpinBox *m_transcriptTextExtrudeDepthSpin = nullptr;
    QDoubleSpinBox *m_transcriptTextExtrudeBevelSpin = nullptr;
    QCheckBox *m_transcriptShowSpeakerTitleCheckBox = nullptr;
    QCheckBox *m_transcriptHighlightCurrentWordCheckBox = nullptr;
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
    QPushButton *m_transcriptExportTextButton = nullptr;
    QCheckBox *m_transcriptShowExcludedLinesCheckBox = nullptr;
    QLabel *m_speakersInspectorClipLabel = nullptr;
    QLabel *m_speakersInspectorDetailsLabel = nullptr;
    SpeakersTable *m_speakersTable = nullptr;
    QCheckBox *m_speakerHideUnidentifiedCheckBox = nullptr;
    QCheckBox *m_speakerShowContiguousSectionsCheckBox = nullptr;
    QCheckBox *m_speakerApplyTrackToAllMatchingSectionsCheckBox = nullptr;
    QSpinBox *m_speakerSectionMinimumWordsSpin = nullptr;
    QPushButton *m_speakerExportLongSectionsButton = nullptr;
    QPushButton *m_speakerCreateTitleClipsButton = nullptr;
    QCheckBox *m_speakerOverlayCreateTitleClipsButton = nullptr;
    QComboBox *m_speakerOverlayFlyInStyleCombo = nullptr;
    QDoubleSpinBox *m_speakerOverlayFlyInDelaySpin = nullptr;
    QDoubleSpinBox *m_speakerOverlayFlyInDurationSpin = nullptr;
    QDoubleSpinBox *m_speakerOverlayFlyInTimeSpin = nullptr;
    QDoubleSpinBox *m_speakerOverlayWrapRadiusSpin = nullptr;
    QDoubleSpinBox *m_speakerOverlayWrapDepthSpin = nullptr;
    QDoubleSpinBox *m_speakerOverlayWrapStartAngleSpin = nullptr;
    QDoubleSpinBox *m_speakerOverlayWrapEndAngleSpin = nullptr;
    QDoubleSpinBox *m_speakerOverlayWrapPitchSpin = nullptr;
    QDoubleSpinBox *m_speakerOverlayWrapRollSpin = nullptr;
    QDoubleSpinBox *m_speakerOverlayRotationXSpin = nullptr;
    QDoubleSpinBox *m_speakerOverlayRotationYSpin = nullptr;
    QDoubleSpinBox *m_speakerOverlayRotationZSpin = nullptr;
    QSpinBox *m_speakerOverlayTitleFontSizeSpin = nullptr;
    QCheckBox *m_speakerOverlayTitleAutoFitCheckBox = nullptr;
    QSpinBox *m_speakerOverlayTitleBoxWidthSpin = nullptr;
    QComboBox *m_speakerOverlayTitleTextMaterialCombo = nullptr;
    QComboBox *m_speakerOverlayTitleBorderMaterialCombo = nullptr;
    QLineEdit *m_speakerOverlayTitleTextPatternPathEdit = nullptr;
    QLineEdit *m_speakerOverlayTitleBorderPatternPathEdit = nullptr;
    QDoubleSpinBox *m_speakerOverlayTitlePatternScaleSpin = nullptr;
    QCheckBox *m_speakerOverlayTitleExtrudeCheckBox = nullptr;
    QComboBox *m_speakerOverlayTitleExtrudeModeCombo = nullptr;
    QDoubleSpinBox *m_speakerOverlayTitleExtrudeDepthSpin = nullptr;
    QDoubleSpinBox *m_speakerOverlayTitleBevelScaleSpin = nullptr;
    QCheckBox *m_speakerShowCurrentSpeakerNameCheckBox = nullptr;
    QCheckBox *m_speakerShowCurrentSpeakerOrganizationCheckBox = nullptr;
    QSpinBox *m_speakerCurrentSpeakerNameTextSizeSpin = nullptr;
    QSpinBox *m_speakerCurrentSpeakerOrganizationTextSizeSpin = nullptr;
    QSpinBox *m_speakerCurrentSpeakerNameYPositionSpin = nullptr;
    QSpinBox *m_speakerCurrentSpeakerOrganizationYPositionSpin = nullptr;
    QPushButton *m_speakerCurrentSpeakerNameColorButton = nullptr;
    QPushButton *m_speakerCurrentSpeakerOrganizationColorButton = nullptr;
    QPushButton *m_speakerCurrentSpeakerBackgroundColorButton = nullptr;
    QCheckBox *m_speakerCurrentSpeakerBackgroundVisibleCheckBox = nullptr;
    QSpinBox *m_speakerCurrentSpeakerBackgroundOpacitySpin = nullptr;
    QPushButton *m_speakerCurrentSpeakerBorderColorButton = nullptr;
    QSpinBox *m_speakerCurrentSpeakerBorderOpacitySpin = nullptr;
    QSpinBox *m_speakerCurrentSpeakerBackgroundRadiusSpin = nullptr;
    QSpinBox *m_speakerCurrentSpeakerBorderWidthSpin = nullptr;
    QCheckBox *m_speakerCurrentSpeakerShadowCheckBox = nullptr;
    QPushButton *m_speakerCurrentSpeakerShadowColorButton = nullptr;
    QSpinBox *m_speakerCurrentSpeakerShadowOpacitySpin = nullptr;
    QTableWidget *m_speakerSectionsTable = nullptr;
    QWidget *m_selectedSpeakerPopup = nullptr;
    QLabel *m_selectedSpeakerIdLabel = nullptr;
    QLineEdit *m_selectedSpeakerNameEdit = nullptr;
    QLineEdit *m_selectedSpeakerOrganizationEdit = nullptr;
    QLineEdit *m_selectedSpeakerLogoPathEdit = nullptr;
    QLineEdit *m_selectedSpeakerPrimaryColorEdit = nullptr;
    QLineEdit *m_selectedSpeakerSecondaryColorEdit = nullptr;
    QLineEdit *m_selectedSpeakerAccentColorEdit = nullptr;
    QCheckBox *m_selectedSpeakerGradingEnabledCheckBox = nullptr;
    QDoubleSpinBox *m_selectedSpeakerBrightnessSpin = nullptr;
    QDoubleSpinBox *m_selectedSpeakerContrastSpin = nullptr;
    QDoubleSpinBox *m_selectedSpeakerSaturationSpin = nullptr;
    QListWidget *m_selectedSpeakerFaceDetectionsList = nullptr;
    QListWidget *m_speakerPlayheadFaceDetectionsList = nullptr;
    QCheckBox *m_speakerShowPlayheadFaceDetectionsCheckBox = nullptr;
    QPushButton *m_selectedSpeakerPreviousSentenceButton = nullptr;
    QPushButton *m_selectedSpeakerNextSentenceButton = nullptr;
    QPushButton *m_selectedSpeakerNextSectionButton = nullptr;
    QPushButton *m_selectedSpeakerRandomSentenceButton = nullptr;
    QLabel *m_speakerCurrentSentenceLabel = nullptr;
    QTableWidget *m_speakerTranscriptTable = nullptr;
    QPushButton *m_speakerRunAutoTrackButton = nullptr;
    QPushButton *m_speakerViewFacestreamButton = nullptr;
    QPushButton *m_speakerFacestreamSettingsButton = nullptr;
    QPushButton *m_speakerRefreshTrackAvatarsButton = nullptr;
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
    QDoubleSpinBox *m_speakerSectionRotationSpin = nullptr;
    QCheckBox *m_speakerFramingZoomEnabledCheckBox = nullptr;
    QSpinBox *m_speakerFramingCenterSmoothingFramesSpin = nullptr;
    QSpinBox *m_speakerFramingZoomSmoothingFramesSpin = nullptr;
    QComboBox *m_speakerFramingSmoothingModeCombo = nullptr;
    QDoubleSpinBox *m_speakerFramingCenterSmoothingStrengthSpin = nullptr;
    QDoubleSpinBox *m_speakerFramingZoomSmoothingStrengthSpin = nullptr;
    QSpinBox *m_speakerFramingGapHoldFramesSpin = nullptr;
    QCheckBox *m_speakerShowFaceDetectionsBoxesCheckBox = nullptr;
    QCheckBox *m_speakerShowRawDetectionsCheckBox = nullptr;
    QCheckBox *m_speakerApplyFramingToClipCheckBox = nullptr;
    QTableWidget *m_speakerFramingEnabledKeyframeTable = nullptr;
    QLabel *m_speakerClipFramingStatusLabel = nullptr;
    QLabel *m_speakerRefsChipLabel = nullptr;
    QLabel *m_speakerPointstreamChipLabel = nullptr;
    QPushButton *m_speakerTrackingChipButton = nullptr;
    QPushButton *m_speakerStabilizeChipButton = nullptr;
    QTableWidget *m_speakerFaceDetectionsTable = nullptr;
    QPlainTextEdit *m_speakerFaceDetectionsDetailsEdit = nullptr;
    QCheckBox *m_speakerDetectionsAvailableCheckBox = nullptr;
    QCheckBox *m_speakerTracksAvailableCheckBox = nullptr;
    QTableWidget *m_speakerRawDetectionTable = nullptr;
    QPlainTextEdit *m_speakerRawDetectionDetailsEdit = nullptr;

    QSpinBox *m_outputWidthSpin = nullptr;
    QSpinBox *m_outputHeightSpin = nullptr;
    QDoubleSpinBox *m_outputFpsSpin = nullptr;
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
    QSpinBox *m_historyMaxEntriesSpin = nullptr;
    QSpinBox *m_historyMaxMegabytesSpin = nullptr;
    QPushButton *m_renderButton = nullptr;
    QPushButton *m_backgroundColorButton = nullptr;
    QComboBox *m_backgroundFillEffectCombo = nullptr;
    QDoubleSpinBox *m_backgroundFillOpacitySpin = nullptr;
    QDoubleSpinBox *m_backgroundFillBrightnessSpin = nullptr;
    QDoubleSpinBox *m_backgroundFillSaturationSpin = nullptr;

    QSpinBox *m_transcriptPrependMsSpin = nullptr;
    QSpinBox *m_transcriptPostpendMsSpin = nullptr;
    QSpinBox *m_transcriptOffsetMsSpin = nullptr;
    QComboBox *m_speechFilterFadeModeCombo = nullptr;
    QSpinBox *m_speechFilterFadeSamplesSpin = nullptr;
    QDoubleSpinBox *m_speechFilterCurveStrengthSpin = nullptr;
    QCheckBox *m_speechFilterRangeCrossfadeCheckBox = nullptr;
    QComboBox *m_speechFilterFrameTransitionModeCombo = nullptr;
    QCheckBox *m_speechFilterFrameCrossfadeCheckBox = nullptr;
    QSpinBox *m_speechFilterFrameCrossfadeFramesSpin = nullptr;
    QComboBox *m_playbackClockSourceCombo = nullptr;
    QComboBox *m_playbackAudioWarpModeCombo = nullptr;
    QLabel *m_processingJobsSummaryLabel = nullptr;
    QTableWidget *m_processingJobsTable = nullptr;
};
