#include "inspector_pane.h"
#include "inspector_pane_tab_helpers.h"
#include "audio_engine.h"
#include "editor_shared_core.h"
#include "playback_timing_context.h"

#include <QCheckBox>
#include <QColor>
#include <QComboBox>
#include <QDoubleSpinBox>
#include <QFontComboBox>
#include <QFormLayout>
#include <QFrame>
#include <QGridLayout>
#include <QHeaderView>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QScrollArea>
#include <QSpinBox>
#include <QSplitter>
#include <QTableWidget>
#include <QVBoxLayout>

using namespace jcut::inspector;

QWidget *InspectorPane::buildTranscriptTab()
{
    auto *page = new QWidget;
    auto *layout = createTabLayout(page);
    layout->addWidget(createTabHeading(QStringLiteral("Transcript"), page));

    auto *splitter = new QSplitter(Qt::Vertical, page);
    splitter->setChildrenCollapsible(false);

    auto *settingsContainer = new QWidget(splitter);
    auto *settingsLayout = new QVBoxLayout(settingsContainer);
    settingsLayout->setContentsMargins(8, 8, 8, 8);
    settingsLayout->setSpacing(6);

    // --- Prominent, editable cut title ---
    m_transcriptInspectorClipLabel = new QLineEdit(settingsContainer);
    m_transcriptInspectorClipLabel->setPlaceholderText(QStringLiteral("No transcript selected"));
    m_transcriptInspectorClipLabel->setStyleSheet(
        QStringLiteral("QLineEdit {"
                       "  font-size: 16px;"
                       "  font-weight: 700;"
                       "  padding: 6px 8px;"
                       "  border: 1px solid #3a4a5a;"
                       "  border-radius: 4px;"
                       "  background: #1e2a36;"
                       "  color: #e0e8f0;"
                       "}"
                       "QLineEdit:focus {"
                       "  border-color: #5a8ab5;"
                       "  background: #243240;"
                       "}"));
    m_transcriptInspectorClipLabel->setToolTip(
        QStringLiteral("Edit the clip label for this transcript cut. Changes are saved automatically."));

    m_transcriptInspectorDetailsLabel = new QLabel(QStringLiteral("Select a clip with a WhisperX JSON transcript."), settingsContainer);
    m_transcriptInspectorDetailsLabel->setWordWrap(true);
    m_transcriptInspectorDetailsLabel->setStyleSheet(
        QStringLiteral("font-size: 11px; color: #8fa3b8; padding: 0 4px 4px 4px;"));

    // --- Cut version controls (prominent, right below title) ---
    auto *cutHeaderLayout = new QHBoxLayout;
    cutHeaderLayout->setContentsMargins(0, 0, 0, 0);
    auto *cutLabel = new QLabel(QStringLiteral("Cut Version:"), settingsContainer);
    cutLabel->setStyleSheet(QStringLiteral("font-weight: 600; color: #b0c4d8;"));
    m_transcriptScriptVersionCombo = new QComboBox(settingsContainer);
    m_transcriptScriptVersionCombo->setEditable(true);
    m_transcriptScriptVersionCombo->setInsertPolicy(QComboBox::NoInsert);
    m_transcriptScriptVersionCombo->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    m_transcriptScriptVersionCombo->lineEdit()->setPlaceholderText(QStringLiteral("Cut version name"));
    m_transcriptScriptVersionCombo->lineEdit()->setStyleSheet(
        QStringLiteral("QLineEdit {"
                       "  padding: 4px 6px;"
                       "  background: #151b22;"
                       "  border: 1px solid #30363d;"
                       "  color: #c9d1d9;"
                       "  border-radius: 4px;"
                       "}"));
    cutHeaderLayout->addWidget(cutLabel);
    cutHeaderLayout->addWidget(m_transcriptScriptVersionCombo, 1);

    auto *versionButtonsLayout = new QHBoxLayout;
    versionButtonsLayout->setContentsMargins(0, 0, 0, 0);
    versionButtonsLayout->setSpacing(4);
    m_transcriptNewVersionButton = new QPushButton(QStringLiteral("+ New Cut"), settingsContainer);
    m_transcriptNewVersionButton->setStyleSheet(
        QStringLiteral("QPushButton { padding: 4px 12px; font-weight: 600; }"));
    m_transcriptDeleteVersionButton = new QPushButton(QStringLiteral("Delete"), settingsContainer);
    m_transcriptDeleteVersionButton->setStyleSheet(
        QStringLiteral("QPushButton { padding: 4px 12px; color: #d47a7a; }"));
    m_transcriptExportTextButton = new QPushButton(QStringLiteral("Export TXT"), settingsContainer);
    m_transcriptExportTextButton->setStyleSheet(
        QStringLiteral("QPushButton { padding: 4px 12px; font-weight: 600; }"));
    m_transcriptExportTextButton->setToolTip(
        QStringLiteral("Export contiguous speaker transcript sections as a text file."));
    versionButtonsLayout->addWidget(m_transcriptNewVersionButton);
    versionButtonsLayout->addWidget(m_transcriptDeleteVersionButton);
    versionButtonsLayout->addWidget(m_transcriptExportTextButton);

    m_transcriptOverlayEnabledCheckBox = new QCheckBox(QStringLiteral("Enable Overlay"), settingsContainer);
    m_transcriptPlacementModeCombo = new QComboBox(settingsContainer);
    m_transcriptPlacementModeCombo->addItem(QStringLiteral("Manual"), true);
    m_transcriptPlacementModeCombo->addItem(QStringLiteral("Follow Speaker"), false);
    m_transcriptPlacementModeCombo->setToolTip(
        QStringLiteral("Choose whether transcript overlay X/Y are manual or derived from active speaker tracking."));
    m_transcriptBackgroundVisibleCheckBox = new QCheckBox(QStringLiteral("Show Background"), settingsContainer);
    m_transcriptBackgroundOpacitySpin = new QSpinBox(settingsContainer);
    m_transcriptBackgroundCornerRadiusSpin = new QSpinBox(settingsContainer);
    m_transcriptTextOpacitySpin = new QSpinBox(settingsContainer);
    m_transcriptBackgroundPaddingSpin = new QSpinBox(settingsContainer);
    m_transcriptBackgroundFrameCheckBox = new QCheckBox(QStringLiteral("Show Frame"), settingsContainer);
    auto makeTranscriptColorButton = [settingsContainer](const QString& color, const QString& tooltip) {
        auto* button = new QPushButton(color, settingsContainer);
        button->setMinimumHeight(24);
        button->setToolTip(tooltip);
        button->setStyleSheet(
            QStringLiteral("QPushButton { background: %1; color: %2; "
                           "border: 1px solid #2e3b4a; border-radius: 4px; padding: 3px 8px; }")
                .arg(color,
                     QColor(color).lightness() > 128 ? QStringLiteral("#000000")
                                                     : QStringLiteral("#ffffff")));
        return button;
    };
    m_transcriptTextColorButton = makeTranscriptColorButton(
        QStringLiteral("#ffffff"),
        QStringLiteral("Set the transcript overlay text color."));
    m_transcriptBackgroundColorButton = makeTranscriptColorButton(
        QStringLiteral("#000000"),
        QStringLiteral("Set the transcript overlay window background color."));
    m_transcriptBackgroundFrameColorButton = makeTranscriptColorButton(
        QStringLiteral("#ffffff"), QStringLiteral("Set the transcript window frame color."));
    m_transcriptBackgroundFrameOpacitySpin = new QSpinBox(settingsContainer);
    m_transcriptBackgroundFrameWidthSpin = new QSpinBox(settingsContainer);
    m_transcriptBackgroundFrameGapSpin = new QSpinBox(settingsContainer);
    m_transcriptHighlightColorButton = makeTranscriptColorButton(
        QStringLiteral("#fff2a8"),
        QStringLiteral("Set the active word highlight color."));
    m_transcriptShadowEnabledCheckBox = new QCheckBox(QStringLiteral("Show Shadow"), settingsContainer);
    m_transcriptShadowColorButton = makeTranscriptColorButton(
        QStringLiteral("#000000"),
        QStringLiteral("Set the subtitle drop shadow color."));
    m_transcriptShadowOpacitySpin = new QSpinBox(settingsContainer);
    m_transcriptShadowOffsetXSpin = new QSpinBox(settingsContainer);
    m_transcriptShadowOffsetYSpin = new QSpinBox(settingsContainer);
    m_transcriptOutlineEnabledCheckBox = new QCheckBox(QStringLiteral("Show Dilation"), settingsContainer);
    m_transcriptOutlineColorButton = makeTranscriptColorButton(
        QStringLiteral("#000000"),
        QStringLiteral("Set the subtitle text dilation/outline color."));
    m_transcriptOutlineWidthSpin = new QSpinBox(settingsContainer);
    m_transcriptOutlineOpacitySpin = new QSpinBox(settingsContainer);
    m_transcriptTextExtrudeModeCombo = new QComboBox(settingsContainer);
    m_transcriptTextExtrudeModeCombo->addItem(QStringLiteral("No Extrusion"), 0);
    m_transcriptTextExtrudeModeCombo->addItem(QStringLiteral("Stacked Copies"), 1);
    m_transcriptTextExtrudeModeCombo->addItem(QStringLiteral("Eroded Solid"), 2);
    m_transcriptTextExtrudeDepthSpin = new QDoubleSpinBox(settingsContainer);
    m_transcriptTextExtrudeDepthSpin->setRange(0.02, 2.0);
    m_transcriptTextExtrudeDepthSpin->setValue(0.16);
    m_transcriptTextExtrudeBevelSpin = new QDoubleSpinBox(settingsContainer);
    m_transcriptTextExtrudeBevelSpin->setRange(0.0, 2.0);
    m_transcriptTextExtrudeBevelSpin->setValue(0.7);
    m_transcriptShowSpeakerTitleCheckBox = new QCheckBox(QStringLiteral("Show Inline Speaker Label"), settingsContainer);
    m_transcriptShowSpeakerTitleCheckBox->setToolTip(
        QStringLiteral("Show a static speaker label inside the transcript caption. Animated speaker introductions are configured below."));
    m_transcriptHighlightCurrentWordCheckBox = new QCheckBox(QStringLiteral("Highlight Current Word"), settingsContainer);
    m_transcriptHighlightCurrentWordCheckBox->setToolTip(
        QStringLiteral("Highlight the active transcript word in the video overlay."));
    m_transcriptMaxLinesSpin = new QSpinBox(settingsContainer);
    m_transcriptMaxCharsSpin = new QSpinBox(settingsContainer);
    m_transcriptFollowCurrentWordCheckBox = new QCheckBox(QStringLiteral("Follow Current Word"), settingsContainer);
    m_transcriptFollowCurrentWordCheckBox->setToolTip(
        QStringLiteral("Highlight and auto-scroll transcript rows during playback."));
    m_transcriptAutoScrollCheckBox = nullptr;
    m_transcriptOverlayXSpin = new QDoubleSpinBox(settingsContainer);
    m_transcriptOverlayYSpin = new QDoubleSpinBox(settingsContainer);
    m_transcriptCenterHorizontalButton = new QPushButton(QStringLiteral("Center X"), settingsContainer);
    m_transcriptCenterVerticalButton = new QPushButton(QStringLiteral("Center Y"), settingsContainer);
    m_transcriptOverlayWidthSpin = new QSpinBox(settingsContainer);
    m_transcriptOverlayHeightSpin = new QSpinBox(settingsContainer);
    m_transcriptFontFamilyCombo = new QFontComboBox(settingsContainer);
    m_transcriptFontSizeSpin = new QSpinBox(settingsContainer);
    m_transcriptBoldCheckBox = new QCheckBox(QStringLiteral("Bold"), settingsContainer);
    m_transcriptItalicCheckBox = new QCheckBox(QStringLiteral("Italic"), settingsContainer);
    m_transcriptUnifiedEditModeCheckBox = new QCheckBox(QStringLiteral("Unified Edit Colors"), settingsContainer);
    m_transcriptUnifiedEditModeCheckBox->setChecked(true);
    m_transcriptSearchFilterLineEdit = new QLineEdit(settingsContainer);
    m_transcriptSearchFilterLineEdit->setPlaceholderText(QStringLiteral("Search transcript text..."));
    m_transcriptSearchFilterLineEdit->setToolTip(
        QStringLiteral("Fuzzy-search transcript words. Press Enter to jump to the best match."));
    m_transcriptSpeakerFilterCombo = new QComboBox(settingsContainer);
    m_transcriptSpeakerFilterCombo->addItem(QStringLiteral("All Speakers"));
    m_transcriptSpeakerFilterCombo->setToolTip(
        QStringLiteral("Filter transcript rows by speaker label from the transcript JSON."));
    m_transcriptShowExcludedLinesCheckBox =
        new QCheckBox(QStringLiteral("Show Lines Not In Active Cut"), settingsContainer);

    m_transcriptMaxLinesSpin->setRange(1, 20);
    m_transcriptBackgroundOpacitySpin->setRange(0, 100);
    m_transcriptBackgroundOpacitySpin->setSuffix(QStringLiteral("%"));
    m_transcriptBackgroundOpacitySpin->setToolTip(
        QStringLiteral("Opacity of the subtitle background window."));
    m_transcriptBackgroundCornerRadiusSpin->setRange(0, 128);
    m_transcriptBackgroundCornerRadiusSpin->setSuffix(QStringLiteral(" px"));
    m_transcriptBackgroundCornerRadiusSpin->setToolTip(
        QStringLiteral("Corner radius of the subtitle background."));
    for (QSpinBox* spin : {m_transcriptTextOpacitySpin, m_transcriptBackgroundFrameOpacitySpin}) {
        spin->setRange(0, 100); spin->setSuffix(QStringLiteral("%")); spin->setValue(100);
    }
    m_transcriptBackgroundPaddingSpin->setRange(0, 400);
    m_transcriptBackgroundPaddingSpin->setSuffix(QStringLiteral(" px"));
    m_transcriptBackgroundPaddingSpin->setValue(16);
    m_transcriptBackgroundFrameWidthSpin->setRange(0, 120);
    m_transcriptBackgroundFrameWidthSpin->setSuffix(QStringLiteral(" px"));
    m_transcriptBackgroundFrameWidthSpin->setValue(2);
    m_transcriptBackgroundFrameGapSpin->setRange(0, 200);
    m_transcriptBackgroundFrameGapSpin->setSuffix(QStringLiteral(" px"));
    m_transcriptBackgroundFrameGapSpin->setValue(4);
    m_transcriptShadowOpacitySpin->setRange(0, 100);
    m_transcriptShadowOpacitySpin->setSuffix(QStringLiteral("%"));
    m_transcriptShadowOpacitySpin->setValue(78);
    m_transcriptShadowOpacitySpin->setToolTip(
        QStringLiteral("Opacity of the subtitle drop shadow."));
    m_transcriptShadowOffsetXSpin->setRange(-128, 128);
    m_transcriptShadowOffsetXSpin->setSuffix(QStringLiteral(" px"));
    m_transcriptShadowOffsetXSpin->setValue(5);
    m_transcriptShadowOffsetXSpin->setToolTip(
        QStringLiteral("Horizontal drop shadow offset."));
    m_transcriptShadowOffsetYSpin->setRange(-128, 128);
    m_transcriptShadowOffsetYSpin->setSuffix(QStringLiteral(" px"));
    m_transcriptShadowOffsetYSpin->setValue(5);
    m_transcriptShadowOffsetYSpin->setToolTip(
        QStringLiteral("Vertical drop shadow offset."));
    m_transcriptOutlineWidthSpin->setRange(0, 24);
    m_transcriptOutlineWidthSpin->setSuffix(QStringLiteral(" px"));
    m_transcriptOutlineWidthSpin->setValue(0);
    m_transcriptOutlineWidthSpin->setToolTip(
        QStringLiteral("Text dilation radius. This expands glyphs evenly behind the subtitle text."));
    m_transcriptOutlineOpacitySpin->setRange(0, 100);
    m_transcriptOutlineOpacitySpin->setSuffix(QStringLiteral("%"));
    m_transcriptOutlineOpacitySpin->setValue(80);
    m_transcriptOutlineOpacitySpin->setToolTip(
        QStringLiteral("Opacity of the text dilation/outline pass."));
    m_transcriptMaxCharsSpin->setRange(
        TimelineClip::TranscriptOverlaySettings::kMinReadableCharsPerLine,
        200);
    m_transcriptOverlayXSpin->setDecimals(3);
    m_transcriptOverlayYSpin->setDecimals(3);
    m_transcriptOverlayXSpin->setRange(-1.0, 1.0);
    m_transcriptOverlayYSpin->setRange(-1.0, 1.0);
    m_transcriptOverlayXSpin->setSingleStep(0.01);
    m_transcriptOverlayYSpin->setSingleStep(0.01);
    m_transcriptOverlayXSpin->setToolTip(
        QStringLiteral("Normalized horizontal center offset (-1.0 left, 0 center, +1.0 right). Editing this switches placement to Manual."));
    m_transcriptOverlayYSpin->setToolTip(
        QStringLiteral("Normalized vertical center offset (-1.0 up, 0 center, +1.0 down). Editing this switches placement to Manual."));
    m_transcriptCenterHorizontalButton->setToolTip(
        QStringLiteral("Set the overlay center X offset to 0 and switch placement to Manual."));
    m_transcriptCenterVerticalButton->setToolTip(
        QStringLiteral("Set the overlay center Y offset to 0 and switch placement to Manual."));
    m_transcriptOverlayWidthSpin->setRange(
        static_cast<int>(TimelineClip::TranscriptOverlaySettings::kMinReadableBoxWidth),
        10000);
    m_transcriptOverlayWidthSpin->setToolTip(
        QStringLiteral("Overlay box width in output pixels. Size changes keep the current center position."));
    m_transcriptOverlayHeightSpin->setRange(
        static_cast<int>(TimelineClip::TranscriptOverlaySettings::kMinReadableBoxHeight),
        10000);
    m_transcriptOverlayHeightSpin->setToolTip(
        QStringLiteral("Overlay box height in output pixels. Size changes keep the current center position."));
    m_transcriptFontSizeSpin->setRange(
        TimelineClip::TranscriptOverlaySettings::kMinReadableFontPointSize,
        256);

    m_transcriptPrependMsSpin = new QSpinBox(settingsContainer);
    m_transcriptPostpendMsSpin = new QSpinBox(settingsContainer);
    m_transcriptOffsetMsSpin = new QSpinBox(settingsContainer);
    m_speechFilterFadeModeCombo = new QComboBox(settingsContainer);
    m_speechFilterFadeSamplesSpin = new QSpinBox(settingsContainer);
    m_speechFilterCurveStrengthSpin = new QDoubleSpinBox(settingsContainer);
    m_speechFilterRangeCrossfadeCheckBox =
        new QCheckBox(QStringLiteral("Boundary Crossfade"), settingsContainer);
    m_speechFilterFrameTransitionModeCombo = new QComboBox(settingsContainer);
    m_speechFilterFrameCrossfadeCheckBox =
        new QCheckBox(QStringLiteral("Frame Crossfade"), settingsContainer);
    m_speechFilterFrameCrossfadeFramesSpin = new QSpinBox(settingsContainer);

    m_transcriptPrependMsSpin->setRange(0, 10000);
    m_transcriptPrependMsSpin->setValue(150);
    m_transcriptPrependMsSpin->setSuffix(QStringLiteral(" ms"));
    m_transcriptPrependMsSpin->setToolTip(QStringLiteral("Milliseconds to add before each word"));

    m_transcriptPostpendMsSpin->setRange(0, 10000);
    m_transcriptPostpendMsSpin->setValue(70);
    m_transcriptPostpendMsSpin->setSuffix(QStringLiteral(" ms"));
    m_transcriptPostpendMsSpin->setToolTip(QStringLiteral("Milliseconds to add after each word"));

    m_transcriptOffsetMsSpin->setRange(-10000, 10000);
    m_transcriptOffsetMsSpin->setValue(0);
    m_transcriptOffsetMsSpin->setSuffix(QStringLiteral(" ms"));
    m_transcriptOffsetMsSpin->setToolTip(
        QStringLiteral("Signed timing offset applied to transcript word windows. Positive values display later; negative values display earlier."));

    m_speechFilterFadeModeCombo->addItem(QStringLiteral("Passthrough"), QStringLiteral("none"));
    m_speechFilterFadeModeCombo->addItem(
        AudioEngine::speechFilterFadeModeLabel(AudioEngine::SpeechFilterFadeMode::JumpCut),
        AudioEngine::speechFilterFadeModeToString(AudioEngine::SpeechFilterFadeMode::JumpCut));
    m_speechFilterFadeModeCombo->addItem(
        AudioEngine::speechFilterFadeModeLabel(AudioEngine::SpeechFilterFadeMode::Fade),
        AudioEngine::speechFilterFadeModeToString(AudioEngine::SpeechFilterFadeMode::Fade));
    m_speechFilterFadeModeCombo->addItem(
        AudioEngine::speechFilterFadeModeLabel(AudioEngine::SpeechFilterFadeMode::SmoothStep),
        AudioEngine::speechFilterFadeModeToString(AudioEngine::SpeechFilterFadeMode::SmoothStep));
    m_speechFilterFadeModeCombo->addItem(
        AudioEngine::speechFilterFadeModeLabel(AudioEngine::SpeechFilterFadeMode::SmootherStep),
        AudioEngine::speechFilterFadeModeToString(AudioEngine::SpeechFilterFadeMode::SmootherStep));
    m_speechFilterFadeModeCombo->addItem(
        AudioEngine::speechFilterFadeModeLabel(AudioEngine::SpeechFilterFadeMode::Crossfade),
        AudioEngine::speechFilterFadeModeToString(AudioEngine::SpeechFilterFadeMode::Crossfade));
    m_speechFilterFadeModeCombo->setCurrentIndex(0);
    m_speechFilterFadeModeCombo->setToolTip(
        QStringLiteral("Speech filter mode; Passthrough leaves playback unchanged."));
    m_speechFilterFadeSamplesSpin->setRange(0, 5000);
    m_speechFilterFadeSamplesSpin->setValue(300);
    m_speechFilterFadeSamplesSpin->setSuffix(QStringLiteral(" samples"));
    m_speechFilterFadeSamplesSpin->setToolTip(
        QStringLiteral("Fade/crossfade window at speech range boundaries (0 = no transition)."));
    m_speechFilterCurveStrengthSpin->setRange(0.25, 4.0);
    m_speechFilterCurveStrengthSpin->setDecimals(2);
    m_speechFilterCurveStrengthSpin->setSingleStep(0.05);
    m_speechFilterCurveStrengthSpin->setValue(1.0);
    m_speechFilterCurveStrengthSpin->setToolTip(
        QStringLiteral("Curve exponent applied to Smooth Step and Smoother Step transitions."));
    m_speechFilterRangeCrossfadeCheckBox->setChecked(false);
    m_speechFilterRangeCrossfadeCheckBox->setToolTip(
        QStringLiteral("Blend adjacent speech ranges instead of fading to silence. "
                       "Does not change audio duration."));
    m_speechFilterFrameTransitionModeCombo->addItem(
        playbackFrameTransitionModeLabel(PlaybackFrameTransitionMode::Cut),
        playbackFrameTransitionModeToString(PlaybackFrameTransitionMode::Cut));
    m_speechFilterFrameTransitionModeCombo->addItem(
        playbackFrameTransitionModeLabel(PlaybackFrameTransitionMode::Crossfade),
        playbackFrameTransitionModeToString(PlaybackFrameTransitionMode::Crossfade));
    m_speechFilterFrameTransitionModeCombo->addItem(
        playbackFrameTransitionModeLabel(PlaybackFrameTransitionMode::SmoothStepSpeedThrough),
        playbackFrameTransitionModeToString(PlaybackFrameTransitionMode::SmoothStepSpeedThrough));
    m_speechFilterFrameTransitionModeCombo->addItem(
        playbackFrameTransitionModeLabel(PlaybackFrameTransitionMode::SmootherStepSpeedThrough),
        playbackFrameTransitionModeToString(PlaybackFrameTransitionMode::SmootherStepSpeedThrough));
    m_speechFilterFrameTransitionModeCombo->setToolTip(
        QStringLiteral("Visual transition for speech-filter segment gaps."));
    m_speechFilterFrameCrossfadeCheckBox->setChecked(false);
    m_speechFilterFrameCrossfadeCheckBox->setToolTip(
        QStringLiteral("Blend the outgoing speech-filter video frames into the incoming segment frames."));
    m_speechFilterFrameCrossfadeFramesSpin->setRange(0, 240);
    m_speechFilterFrameCrossfadeFramesSpin->setValue(6);
    m_speechFilterFrameCrossfadeFramesSpin->setSuffix(QStringLiteral(" frames"));
    m_speechFilterFrameCrossfadeFramesSpin->setToolTip(
        QStringLiteral("Visual transition length in rendered frames at speech-filter segment boundaries."));

    auto makeSettingsForm = [] {
        auto* form = new QFormLayout;
        form->setSpacing(4);
        form->setRowWrapPolicy(QFormLayout::WrapAllRows);
        form->setFieldGrowthPolicy(QFormLayout::AllNonFixedFieldsGrow);
        return form;
    };

    auto sourceSection = createDisclosureSection(settingsContainer, QStringLiteral("Transcript Source"), false);
    sourceSection.body->addWidget(m_transcriptInspectorClipLabel);
    sourceSection.body->addWidget(m_transcriptInspectorDetailsLabel);
    sourceSection.body->addLayout(cutHeaderLayout);
    sourceSection.body->addLayout(versionButtonsLayout);

    auto placementSection = createDisclosureSection(settingsContainer, QStringLiteral("Overlay Placement"), false);
    auto* placementForm = makeSettingsForm();
    placementForm->addRow(QStringLiteral("Overlay"), m_transcriptOverlayEnabledCheckBox);
    placementForm->addRow(QStringLiteral("Placement"), m_transcriptPlacementModeCombo);
    placementForm->addRow(QStringLiteral("Center X"), m_transcriptOverlayXSpin);
    placementForm->addRow(QStringLiteral("Center Y"), m_transcriptOverlayYSpin);
    auto *centerButtonsLayout = new QHBoxLayout;
    centerButtonsLayout->setContentsMargins(0, 0, 0, 0);
    centerButtonsLayout->setSpacing(4);
    centerButtonsLayout->addWidget(m_transcriptCenterHorizontalButton);
    centerButtonsLayout->addWidget(m_transcriptCenterVerticalButton);
    auto *centerButtonsContainer = new QWidget(settingsContainer);
    centerButtonsContainer->setLayout(centerButtonsLayout);
    placementForm->addRow(QStringLiteral("Center"), centerButtonsContainer);
    placementForm->addRow(QStringLiteral("Width"), m_transcriptOverlayWidthSpin);
    placementForm->addRow(QStringLiteral("Height"), m_transcriptOverlayHeightSpin);
    placementSection.body->addLayout(placementForm);

    auto typographySection = createDisclosureSection(settingsContainer, QStringLiteral("Typography"), false);
    auto* typographyForm = makeSettingsForm();
    typographyForm->addRow(QStringLiteral("Font"), m_transcriptFontFamilyCombo);
    typographyForm->addRow(QStringLiteral("Font Size"), m_transcriptFontSizeSpin);
    typographyForm->addRow(QStringLiteral("Bold"), m_transcriptBoldCheckBox);
    typographyForm->addRow(QStringLiteral("Italic"), m_transcriptItalicCheckBox);
    typographyForm->addRow(QStringLiteral("Text Color"), m_transcriptTextColorButton);
    typographyForm->addRow(QStringLiteral("Text Opacity"), m_transcriptTextOpacitySpin);
    typographySection.body->addLayout(typographyForm);

    auto backgroundSection = createDisclosureSection(settingsContainer, QStringLiteral("Background & Effects"), false);
    auto* backgroundForm = makeSettingsForm();
    backgroundForm->addRow(QStringLiteral("Background"), m_transcriptBackgroundVisibleCheckBox);
    backgroundForm->addRow(QStringLiteral("Background Color"), m_transcriptBackgroundColorButton);
    backgroundForm->addRow(QStringLiteral("Background Opacity"), m_transcriptBackgroundOpacitySpin);
    backgroundForm->addRow(QStringLiteral("Corner Radius"), m_transcriptBackgroundCornerRadiusSpin);
    backgroundForm->addRow(QStringLiteral("Padding"), m_transcriptBackgroundPaddingSpin);
    backgroundForm->addRow(QStringLiteral("Frame"), m_transcriptBackgroundFrameCheckBox);
    backgroundForm->addRow(QStringLiteral("Frame Color"), m_transcriptBackgroundFrameColorButton);
    backgroundForm->addRow(QStringLiteral("Frame Opacity"), m_transcriptBackgroundFrameOpacitySpin);
    backgroundForm->addRow(QStringLiteral("Frame Width"), m_transcriptBackgroundFrameWidthSpin);
    backgroundForm->addRow(QStringLiteral("Frame Gap"), m_transcriptBackgroundFrameGapSpin);
    backgroundForm->addRow(QStringLiteral("Shadow"), m_transcriptShadowEnabledCheckBox);
    backgroundForm->addRow(QStringLiteral("Shadow Color"), m_transcriptShadowColorButton);
    backgroundForm->addRow(QStringLiteral("Shadow Opacity"), m_transcriptShadowOpacitySpin);
    backgroundForm->addRow(QStringLiteral("Shadow X"), m_transcriptShadowOffsetXSpin);
    backgroundForm->addRow(QStringLiteral("Shadow Y"), m_transcriptShadowOffsetYSpin);
    backgroundForm->addRow(QStringLiteral("Dilation"), m_transcriptOutlineEnabledCheckBox);
    backgroundForm->addRow(QStringLiteral("Dilation Color"), m_transcriptOutlineColorButton);
    backgroundForm->addRow(QStringLiteral("Dilation Size"), m_transcriptOutlineWidthSpin);
    backgroundForm->addRow(QStringLiteral("Dilation Opacity"), m_transcriptOutlineOpacitySpin);
    backgroundForm->addRow(QStringLiteral("Text Mode"), m_transcriptTextExtrudeModeCombo);
    backgroundForm->addRow(QStringLiteral("Extrude Depth"), m_transcriptTextExtrudeDepthSpin);
    backgroundForm->addRow(QStringLiteral("Extrude Bevel"), m_transcriptTextExtrudeBevelSpin);
    backgroundSection.body->addLayout(backgroundForm);

    auto contentSection = createDisclosureSection(settingsContainer, QStringLiteral("Transcript Behavior"), false);
    auto* contentForm = makeSettingsForm();
    contentForm->addRow(QStringLiteral("Title"), m_transcriptShowSpeakerTitleCheckBox);
    contentForm->addRow(QStringLiteral("Word Highlight"), m_transcriptHighlightCurrentWordCheckBox);
    contentForm->addRow(QStringLiteral("Highlight Color"), m_transcriptHighlightColorButton);
    contentForm->addRow(QStringLiteral("Max Lines"), m_transcriptMaxLinesSpin);
    contentForm->addRow(QStringLiteral("Max Chars"), m_transcriptMaxCharsSpin);
    contentForm->addRow(QStringLiteral("Follow Word"), m_transcriptFollowCurrentWordCheckBox);
    contentForm->addRow(QStringLiteral("Edit Colors"), m_transcriptUnifiedEditModeCheckBox);
    contentForm->addRow(QStringLiteral("Search"), m_transcriptSearchFilterLineEdit);
    contentForm->addRow(QStringLiteral("Speaker"), m_transcriptSpeakerFilterCombo);
    contentForm->addRow(QStringLiteral("Visibility"), m_transcriptShowExcludedLinesCheckBox);
    contentSection.body->addLayout(contentForm);

    auto speakerTitlesSection = createDisclosureSection(
        settingsContainer, QStringLiteral("Animated Speaker Introductions"), false);
    auto* speakerTitlesHelp = new QLabel(
        QStringLiteral("Transcript-owned title events generated at speaker introductions. They remain linked to this transcript clip and use the shared title renderer."),
        speakerTitlesSection.container);
    speakerTitlesHelp->setWordWrap(true);
    speakerTitlesHelp->setStyleSheet(QStringLiteral("color: #9aabc0; font-size: 11px;"));
    speakerTitlesSection.body->addWidget(speakerTitlesHelp);
    m_transcriptSpeakerTitlesContainer = new QWidget(speakerTitlesSection.container);
    auto* transcriptSpeakerTitlesLayout = new QVBoxLayout(m_transcriptSpeakerTitlesContainer);
    transcriptSpeakerTitlesLayout->setContentsMargins(0, 0, 0, 0);
    transcriptSpeakerTitlesLayout->setSpacing(6);
    speakerTitlesSection.body->addWidget(m_transcriptSpeakerTitlesContainer);

    auto speechTimingSection = createDisclosureSection(settingsContainer, QStringLiteral("Speech Filter Timing"), false);
    auto* speechTimingForm = makeSettingsForm();
    speechTimingForm->addRow(QStringLiteral("Mode"), m_speechFilterFadeModeCombo);
    speechTimingForm->addRow(QStringLiteral("Time Offset"), m_transcriptOffsetMsSpin);
    speechTimingForm->addRow(QStringLiteral("Prepend Time"), m_transcriptPrependMsSpin);
    speechTimingForm->addRow(QStringLiteral("Postpend Time"), m_transcriptPostpendMsSpin);
    speechTimingSection.body->addLayout(speechTimingForm);

    auto audioTransitionSection = createDisclosureSection(settingsContainer, QStringLiteral("Speech Filter Audio"), false);
    auto* audioTransitionForm = makeSettingsForm();
    audioTransitionForm->addRow(QStringLiteral("Audio Fade"), m_speechFilterFadeSamplesSpin);
    audioTransitionForm->addRow(QStringLiteral("Curve Strength"), m_speechFilterCurveStrengthSpin);
    audioTransitionSection.body->addLayout(audioTransitionForm);

    auto frameTransitionSection = createDisclosureSection(settingsContainer, QStringLiteral("Frame Transition"), false);
    auto* frameTransitionForm = makeSettingsForm();
    frameTransitionForm->addRow(QStringLiteral("Mode"), m_speechFilterFrameTransitionModeCombo);
    frameTransitionForm->addRow(QStringLiteral("Length"), m_speechFilterFrameCrossfadeFramesSpin);
    frameTransitionSection.body->addLayout(frameTransitionForm);

    // --- Assemble settings layout ---
    settingsLayout->addWidget(sourceSection.container);
    settingsLayout->addWidget(placementSection.container);
    settingsLayout->addWidget(typographySection.container);
    settingsLayout->addWidget(backgroundSection.container);
    settingsLayout->addWidget(contentSection.container);
    settingsLayout->addWidget(speakerTitlesSection.container);
    settingsLayout->addWidget(speechTimingSection.container);
    settingsLayout->addWidget(frameTransitionSection.container);
    settingsLayout->addWidget(audioTransitionSection.container);
    settingsLayout->addStretch(1);

    auto *settingsScroll = new QScrollArea(page);
    settingsScroll->setWidgetResizable(true);
    settingsScroll->setFrameShape(QFrame::NoFrame);
    settingsScroll->setWidget(settingsContainer);

    // --- Transcript table ---
    m_transcriptTable = new QTableWidget(splitter);
    m_transcriptTable->setColumnCount(5);
    m_transcriptTable->setHorizontalHeaderLabels(
        {QStringLiteral("Source Start"),
         QStringLiteral("Source End"),
         QStringLiteral("Speaker"),
         QStringLiteral("Text"),
         QStringLiteral("Edits")});
    m_transcriptTable->setSelectionBehavior(QAbstractItemView::SelectRows);
    m_transcriptTable->setSelectionMode(QAbstractItemView::ExtendedSelection);
    m_transcriptTable->setEditTriggers(QAbstractItemView::DoubleClicked |
                                       QAbstractItemView::EditKeyPressed);
    m_transcriptTable->setWordWrap(false);
    m_transcriptTable->setTextElideMode(Qt::ElideRight);
    m_transcriptTable->setAlternatingRowColors(true);
    m_transcriptTable->setShowGrid(false);
    m_transcriptTable->setVerticalScrollMode(QAbstractItemView::ScrollPerPixel);
    m_transcriptTable->setHorizontalScrollMode(QAbstractItemView::ScrollPerPixel);
    m_transcriptTable->verticalHeader()->setVisible(false);
    m_transcriptTable->verticalHeader()->setSectionResizeMode(QHeaderView::Fixed);
    m_transcriptTable->verticalHeader()->setDefaultSectionSize(30);
    m_transcriptTable->verticalHeader()->setMinimumSectionSize(30);
    m_transcriptTable->horizontalHeader()->setHighlightSections(false);
    m_transcriptTable->horizontalHeader()->setStretchLastSection(false);
    m_transcriptTable->horizontalHeader()->setSectionResizeMode(QHeaderView::Interactive);
    m_transcriptTable->horizontalHeader()->resizeSection(0, 108);
    m_transcriptTable->horizontalHeader()->resizeSection(1, 108);
    m_transcriptTable->horizontalHeader()->resizeSection(2, 96);
    m_transcriptTable->horizontalHeader()->setSectionResizeMode(3, QHeaderView::Stretch);
    m_transcriptTable->horizontalHeader()->resizeSection(4, 92);

    splitter->addWidget(settingsScroll);
    splitter->addWidget(m_transcriptTable);
    splitter->setStretchFactor(0, 0);
    splitter->setStretchFactor(1, 1);
    splitter->setSizes({360, 420});

    layout->addWidget(splitter);

    return page;
}
