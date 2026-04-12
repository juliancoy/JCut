#include "inspector_pane.h"
#include "editor_shared.h"
#include "debug_controls.h"

#include <QCheckBox>
#include <QComboBox>
#include <QDoubleSpinBox>
#include <QFontComboBox>
#include <QFormLayout>
#include <QFrame>
#include <QGroupBox>
#include <QHeaderView>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QScrollArea>
#include <QSplitter>
#include <QSpinBox>
#include <QTabBar>
#include <QTabWidget>
#include <QTableWidget>
#include <QPushButton>
#include <QSize>
#include <QStyle>
#include <QVBoxLayout>

namespace {
QLabel *createTabHeading(const QString &text, QWidget *parent = nullptr) {
    auto *label = new QLabel(text, parent);
    label->setStyleSheet(QStringLiteral(
        "QLabel { font-size: 13px; font-weight: 700; color: #8fa0b5; "
        "padding: 2px 0 6px 0; }"));
    return label;
}
} // namespace

InspectorPane::InspectorPane(QWidget *parent)
    : QWidget(parent)
{
    setStyleSheet(
        QStringLiteral(
            "QWidget { background: #0c1015; color: #edf2f7; }"
            "QPushButton, QToolButton { background: #1b2430; border: 1px solid #2e3b4a; border-radius: 7px; padding: 6px 12px; }"
            "QPushButton:hover, QToolButton:hover { background: #233142; }"
            "QDoubleSpinBox, QSpinBox, QLineEdit, QComboBox { background: #151b22; border: 1px solid #30363d; color: #c9d1d9; border-radius: 6px; padding: 4px; }"
            "QCheckBox { color: #edf2f7; }"
            "QLabel { color: #8fa0b5; }"
            "QGroupBox { border: 1px solid #30363d; border-radius: 6px; margin-top: 2ex; font-weight: bold; color: #8fa0b5; padding-top: 10px; }"
            "QGroupBox::title { subcontrol-origin: margin; subcontrol-position: top left; padding: 0 3px; }"
            "QTableWidget { background: #0c1015; alternate-background-color: #161b22; gridline-color: #30363d; border: 1px solid #30363d; border-radius: 6px; color: #c9d1d9; }"
            "QHeaderView::section { background-color: #161b22; color: #8fa0b5; border: 1px solid #30363d; padding: 4px; }"));

    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->addWidget(buildPane());
}

QWidget *InspectorPane::buildPane()
{
    auto *pane = new QFrame;
    pane->setMinimumWidth(200);
    pane->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Expanding);

    auto *layout = new QVBoxLayout(pane);
    layout->setContentsMargins(8, 8, 8, 8);
    layout->setSpacing(8);

    m_inspectorTabs = new QTabWidget(pane);
    m_inspectorTabs->addTab(buildGradingTab(), QStringLiteral("Grade"));
    m_inspectorTabs->addTab(buildOpacityTab(), QStringLiteral("Opacity"));
    m_inspectorTabs->addTab(buildEffectsTab(), QStringLiteral("Effects"));
    m_inspectorTabs->addTab(buildCorrectionsTab(), QStringLiteral("Corrections"));
    m_inspectorTabs->addTab(buildTitlesTab(), QStringLiteral("Titles"));
    m_inspectorTabs->addTab(buildSyncTab(), QStringLiteral("Sync"));
    m_inspectorTabs->addTab(buildKeyframesTab(), QStringLiteral("Keyframes"));
    m_inspectorTabs->addTab(buildTranscriptTab(), QStringLiteral("Transcript"));
    m_inspectorTabs->addTab(buildClipTab(), QStringLiteral("Properties"));
    m_inspectorTabs->addTab(buildClipsTab(), QStringLiteral("Clips"));
    m_inspectorTabs->addTab(buildTracksTab(), QStringLiteral("Tracks"));
    m_inspectorTabs->addTab(buildPreviewTab(), QStringLiteral("Preview"));
    m_inspectorTabs->addTab(buildOutputTab(), QStringLiteral("Output"));
    m_inspectorTabs->addTab(buildProfileTab(), QStringLiteral("System"));
    m_inspectorTabs->addTab(buildProjectsTab(), QStringLiteral("Projects"));
    configureInspectorTabs();

    layout->addWidget(m_inspectorTabs);
    return pane;
}

void InspectorPane::configureInspectorTabs()
{
    if (!m_inspectorTabs) {
        return;
    }

    auto *bar = m_inspectorTabs->tabBar();
    m_inspectorTabs->setTabPosition(QTabWidget::East);
    m_inspectorTabs->setDocumentMode(true);
    bar->setExpanding(false);
    bar->setUsesScrollButtons(true);
    bar->setIconSize(QSize(18, 18));
    bar->setDrawBase(false);

    struct TabSpec {
        int index;
        QStyle::StandardPixmap icon;
        const char* tooltip;
    };

    const TabSpec specs[] = {
        {0, QStyle::SP_DriveDVDIcon, "Grade: clip color and grading keyframes"},
        {1, QStyle::SP_BrowserStop, "Opacity: clip opacity keyframes and fades"},
        {2, QStyle::SP_DialogResetButton, "Effects: mask feathering and visual effects"},
        {3, QStyle::SP_DriveFDIcon, "Corrections: draw polygon erase masks for visual artifacts"},
        {4, QStyle::SP_FileDialogListView, "Titles: text overlay keyframes"},
        {5, QStyle::SP_BrowserReload, "Sync: render sync markers for the selected clip"},
        {6, QStyle::SP_FileDialogDetailedView, "Keyframes: transform keyframes for the selected clip"},
        {7, QStyle::SP_FileDialogContentsView, "Transcript: transcript editing and speech filter controls"},
        {8, QStyle::SP_FileDialogInfoView, "Properties: clip and track properties"},
        {9, QStyle::SP_FileDialogInfoView, "Tracks: track visibility and enable state controls"},
        {10, QStyle::SP_MediaPlay, "Preview: editor preview display controls"},
        {11, QStyle::SP_DialogSaveButton, "Output: render settings and export"},
        {12, QStyle::SP_ComputerIcon, "System: playback, decoder, cache, and benchmark information"},
        {13, QStyle::SP_DirHomeIcon, "Projects: browse, create, rename, and switch projects"},
    };

    for (const TabSpec& spec : specs) {
        m_inspectorTabs->setTabIcon(spec.index, style()->standardIcon(spec.icon));
        m_inspectorTabs->setTabText(spec.index, QString());
        bar->setTabToolTip(spec.index, QString::fromUtf8(spec.tooltip));
    }

    bar->setStyleSheet(QStringLiteral(
        "QTabBar::tab {"
        " width: 34px;"
        " height: 34px;"
        " margin: 2px 0;"
        " padding: 0px;"
        " }"
        "QTabBar::tab:selected {"
        " background: palette(base);"
        " border: 1px solid palette(mid);"
        " border-right: 0px;"
        " border-top-left-radius: 6px;"
        " border-bottom-left-radius: 6px;"
        " }"
        "QTabBar::tab:!selected {"
        " background: palette(button);"
        " border: 1px solid palette(midlight);"
        " border-right: 0px;"
        " border-top-left-radius: 6px;"
        " border-bottom-left-radius: 6px;"
        " opacity: 0.8;"
        " }"));
}

QWidget *InspectorPane::buildGradingTab()
{
    auto *page = new QWidget;
    auto *layout = new QVBoxLayout(page);
    layout->addWidget(createTabHeading(QStringLiteral("Grade"), page));

    m_gradingPathLabel = new QLabel(QStringLiteral("No visual clip selected"), page);
    m_gradingPathLabel->setWordWrap(true);
    layout->addWidget(m_gradingPathLabel);

    auto *form = new QFormLayout;
    m_brightnessSpin = new QDoubleSpinBox(page);
    m_contrastSpin = new QDoubleSpinBox(page);
    m_saturationSpin = new QDoubleSpinBox(page);
    m_opacitySpin = new QDoubleSpinBox(page);

    for (QDoubleSpinBox *spin : {m_brightnessSpin, m_contrastSpin, m_saturationSpin})
    {
        spin->setRange(-10.0, 10.0);
        spin->setDecimals(3);
        spin->setSingleStep(0.05);
    }
    form->addRow(QStringLiteral("Brightness"), m_brightnessSpin);
    form->addRow(QStringLiteral("Contrast"), m_contrastSpin);
    form->addRow(QStringLiteral("Saturation"), m_saturationSpin);

    // Shadows/Midtones/Highlights (Lift/Gamma/Gain)
    auto *shadowsGroup = new QGroupBox(QStringLiteral("Shadows (Lift)"), page);
    auto *shadowsLayout = new QHBoxLayout(shadowsGroup);
    m_shadowsRSpin = new QDoubleSpinBox(page);
    m_shadowsGSpin = new QDoubleSpinBox(page);
    m_shadowsBSpin = new QDoubleSpinBox(page);
    for (QDoubleSpinBox *spin : {m_shadowsRSpin, m_shadowsGSpin, m_shadowsBSpin}) {
        spin->setRange(-2.0, 2.0);
        spin->setDecimals(3);
        spin->setSingleStep(0.05);
        spin->setValue(0.0);
    }
    m_shadowsRSpin->setPrefix(QStringLiteral("R: "));
    m_shadowsGSpin->setPrefix(QStringLiteral("G: "));
    m_shadowsBSpin->setPrefix(QStringLiteral("B: "));
    shadowsLayout->addWidget(m_shadowsRSpin);
    shadowsLayout->addWidget(m_shadowsGSpin);
    shadowsLayout->addWidget(m_shadowsBSpin);

    auto *midtonesGroup = new QGroupBox(QStringLiteral("Midtones (Gamma)"), page);
    auto *midtonesLayout = new QHBoxLayout(midtonesGroup);
    m_midtonesRSpin = new QDoubleSpinBox(page);
    m_midtonesGSpin = new QDoubleSpinBox(page);
    m_midtonesBSpin = new QDoubleSpinBox(page);
    for (QDoubleSpinBox *spin : {m_midtonesRSpin, m_midtonesGSpin, m_midtonesBSpin}) {
        spin->setRange(-2.0, 2.0);
        spin->setDecimals(3);
        spin->setSingleStep(0.05);
        spin->setValue(0.0);
    }
    m_midtonesRSpin->setPrefix(QStringLiteral("R: "));
    m_midtonesGSpin->setPrefix(QStringLiteral("G: "));
    m_midtonesBSpin->setPrefix(QStringLiteral("B: "));
    midtonesLayout->addWidget(m_midtonesRSpin);
    midtonesLayout->addWidget(m_midtonesGSpin);
    midtonesLayout->addWidget(m_midtonesBSpin);

    auto *highlightsGroup = new QGroupBox(QStringLiteral("Highlights (Gain)"), page);
    auto *highlightsLayout = new QHBoxLayout(highlightsGroup);
    m_highlightsRSpin = new QDoubleSpinBox(page);
    m_highlightsGSpin = new QDoubleSpinBox(page);
    m_highlightsBSpin = new QDoubleSpinBox(page);
    for (QDoubleSpinBox *spin : {m_highlightsRSpin, m_highlightsGSpin, m_highlightsBSpin}) {
        spin->setRange(-2.0, 2.0);
        spin->setDecimals(3);
        spin->setSingleStep(0.05);
        spin->setValue(0.0);
    }
    m_highlightsRSpin->setPrefix(QStringLiteral("R: "));
    m_highlightsGSpin->setPrefix(QStringLiteral("G: "));
    m_highlightsBSpin->setPrefix(QStringLiteral("B: "));
    highlightsLayout->addWidget(m_highlightsRSpin);
    highlightsLayout->addWidget(m_highlightsGSpin);
    highlightsLayout->addWidget(m_highlightsBSpin);

    m_gradingAutoScrollCheckBox = new QCheckBox(QStringLiteral("Auto Scroll"), page);
    m_gradingFollowCurrentCheckBox = new QCheckBox(QStringLiteral("Follow Current Keyframe"), page);
    m_gradingAutoScrollCheckBox->setChecked(true);
    m_gradingFollowCurrentCheckBox->setChecked(true);
    m_gradingKeyAtPlayheadButton = new QPushButton(QStringLiteral("Key At Playhead"), page);
    m_gradingFadeInButton = new QPushButton(QStringLiteral("Fade In From Playhead"), page);
    m_gradingFadeOutButton = new QPushButton(QStringLiteral("Fade Out From Playhead"), page);
    
    m_gradingFadeDurationSpin = new QDoubleSpinBox(page);
    m_gradingFadeDurationSpin->setRange(0.1, 60.0);
    m_gradingFadeDurationSpin->setValue(1.0);
    m_gradingFadeDurationSpin->setSuffix(QStringLiteral(" s"));
    m_gradingFadeDurationSpin->setDecimals(1);
    m_gradingFadeDurationSpin->setSingleStep(0.5);
    m_gradingFadeDurationSpin->setToolTip(QStringLiteral("Fade duration in seconds"));

    m_gradingKeyframeTable = new QTableWidget(page);
    m_gradingKeyframeTable->setColumnCount(5);
    m_gradingKeyframeTable->setHorizontalHeaderLabels({QStringLiteral("Frame"),
                                                       QStringLiteral("Bright"),
                                                       QStringLiteral("Contrast"),
                                                       QStringLiteral("Sat"),
                                                       QStringLiteral("Interp")});
    m_gradingKeyframeTable->setSelectionBehavior(QAbstractItemView::SelectRows);
    m_gradingKeyframeTable->setSelectionMode(QAbstractItemView::ExtendedSelection);
    m_gradingKeyframeTable->setEditTriggers(QAbstractItemView::DoubleClicked |
                                            QAbstractItemView::EditKeyPressed);
    m_gradingKeyframeTable->verticalHeader()->setVisible(false);
    m_gradingKeyframeTable->horizontalHeader()->setStretchLastSection(true);
    m_gradingKeyframeTable->horizontalHeader()->setSectionResizeMode(QHeaderView::Stretch);

    layout->addLayout(form);
    layout->addWidget(shadowsGroup);
    layout->addWidget(midtonesGroup);
    layout->addWidget(highlightsGroup);
    layout->addWidget(m_gradingAutoScrollCheckBox);
    layout->addWidget(m_gradingFollowCurrentCheckBox);
    layout->addWidget(m_gradingKeyAtPlayheadButton);
    layout->addWidget(m_gradingKeyframeTable, 1);
    return page;
}

QWidget *InspectorPane::buildOpacityTab()
{
    auto *page = new QWidget;
    auto *layout = new QVBoxLayout(page);
    layout->addWidget(createTabHeading(QStringLiteral("Opacity"), page));

    m_opacityPathLabel = new QLabel(QStringLiteral("No visual clip selected"), page);
    m_opacityPathLabel->setWordWrap(true);
    layout->addWidget(m_opacityPathLabel);

    auto *form = new QFormLayout;
    m_opacitySpin->setRange(0.0, 1.0);
    m_opacitySpin->setDecimals(3);
    m_opacitySpin->setSingleStep(0.05);
    m_opacitySpin->setValue(1.0);
    form->addRow(QStringLiteral("Opacity"), m_opacitySpin);
    layout->addLayout(form);

    m_opacityAutoScrollCheckBox = new QCheckBox(QStringLiteral("Auto Scroll"), page);
    m_opacityFollowCurrentCheckBox = new QCheckBox(QStringLiteral("Follow Current Keyframe"), page);
    m_opacityAutoScrollCheckBox->setChecked(true);
    m_opacityFollowCurrentCheckBox->setChecked(true);
    m_opacityKeyAtPlayheadButton = new QPushButton(QStringLiteral("Key At Playhead"), page);
    m_opacityFadeInButton = new QPushButton(QStringLiteral("Fade In From Playhead"), page);
    m_opacityFadeOutButton = new QPushButton(QStringLiteral("Fade Out From Playhead"), page);

    m_opacityFadeDurationSpin = new QDoubleSpinBox(page);
    m_opacityFadeDurationSpin->setRange(0.1, 60.0);
    m_opacityFadeDurationSpin->setValue(1.0);
    m_opacityFadeDurationSpin->setSuffix(QStringLiteral(" s"));
    m_opacityFadeDurationSpin->setDecimals(1);
    m_opacityFadeDurationSpin->setSingleStep(0.5);

    m_opacityKeyframeTable = new QTableWidget(page);
    m_opacityKeyframeTable->setColumnCount(3);
    m_opacityKeyframeTable->setHorizontalHeaderLabels({QStringLiteral("Frame"),
                                                       QStringLiteral("Opacity"),
                                                       QStringLiteral("Interp")});
    m_opacityKeyframeTable->setSelectionBehavior(QAbstractItemView::SelectRows);
    m_opacityKeyframeTable->setSelectionMode(QAbstractItemView::ExtendedSelection);
    m_opacityKeyframeTable->setEditTriggers(QAbstractItemView::DoubleClicked |
                                            QAbstractItemView::EditKeyPressed);
    m_opacityKeyframeTable->verticalHeader()->setVisible(false);
    m_opacityKeyframeTable->horizontalHeader()->setStretchLastSection(true);
    m_opacityKeyframeTable->horizontalHeader()->setSectionResizeMode(QHeaderView::Stretch);

    layout->addWidget(m_opacityAutoScrollCheckBox);
    layout->addWidget(m_opacityFollowCurrentCheckBox);
    layout->addWidget(m_opacityKeyAtPlayheadButton);
    layout->addWidget(m_opacityFadeInButton);
    layout->addWidget(m_opacityFadeOutButton);

    auto *fadeDurationLayout = new QHBoxLayout();
    fadeDurationLayout->addWidget(new QLabel(QStringLiteral("Fade Duration:"), page));
    fadeDurationLayout->addWidget(m_opacityFadeDurationSpin);
    fadeDurationLayout->addStretch();
    layout->addLayout(fadeDurationLayout);

    layout->addWidget(m_opacityKeyframeTable, 1);
    return page;
}

QWidget *InspectorPane::buildEffectsTab()
{
    auto *page = new QWidget;
    auto *layout = new QVBoxLayout(page);
    layout->setContentsMargins(8, 8, 8, 8);
    layout->setSpacing(6);
    layout->addWidget(createTabHeading(QStringLiteral("Effects"), page));

    m_effectsPathLabel = new QLabel(QStringLiteral("No visual clip selected"), page);
    m_effectsPathLabel->setWordWrap(true);
    layout->addWidget(m_effectsPathLabel);

    // Mask feathering section
    auto *featherGroup = new QVBoxLayout;
    
    m_maskFeatherEnabledCheck = new QCheckBox(QStringLiteral("Enable Mask Feathering"), page);
    featherGroup->addWidget(m_maskFeatherEnabledCheck);
    
    auto *featherRow = new QHBoxLayout;
    featherRow->addWidget(new QLabel(QStringLiteral("Radius:"), page));
    m_maskFeatherSpin = new QDoubleSpinBox(page);
    m_maskFeatherSpin->setRange(0.0, 100.0);
    m_maskFeatherSpin->setDecimals(1);
    m_maskFeatherSpin->setSingleStep(0.5);
    m_maskFeatherSpin->setValue(0.0);
    m_maskFeatherSpin->setSuffix(QStringLiteral(" px"));
    m_maskFeatherSpin->setToolTip(QStringLiteral("Amount of feathering to apply to the alpha channel"));
    featherRow->addWidget(m_maskFeatherSpin);
    featherRow->addStretch();
    featherGroup->addLayout(featherRow);
    
    auto *gammaRow = new QHBoxLayout;
    gammaRow->addWidget(new QLabel(QStringLiteral("Curve Gamma:"), page));
    m_maskFeatherGammaSpin = new QDoubleSpinBox(page);
    m_maskFeatherGammaSpin->setRange(0.1, 5.0);
    m_maskFeatherGammaSpin->setDecimals(2);
    m_maskFeatherGammaSpin->setSingleStep(0.1);
    m_maskFeatherGammaSpin->setValue(2.0);
    m_maskFeatherGammaSpin->setToolTip(QStringLiteral("Feather curve gamma: 1.0=linear (soft), 2.0=default (smooth), 3.0+=sharper edges"));
    gammaRow->addWidget(m_maskFeatherGammaSpin);
    gammaRow->addStretch();
    featherGroup->addLayout(gammaRow);
    
    layout->addLayout(featherGroup);
    
    // Info label
    auto *infoLabel = new QLabel(QStringLiteral(
        "Mask feathering smooths the edges of transparent areas. "
        "Use Gamma to control edge sharpness: 1.0=soft, 2.0=default, 3.0+=sharp. "
        "Only applies to clips with alpha channels (PNG, ProRes, etc.)."), page);
    infoLabel->setWordWrap(true);
    infoLabel->setStyleSheet(QStringLiteral("QLabel { color: #8fa0b5; font-size: 11px; }"));
    layout->addWidget(infoLabel);

    layout->addStretch(1);
    return page;
}

QWidget *InspectorPane::buildCorrectionsTab()
{
    auto *page = new QWidget;
    auto *layout = new QVBoxLayout(page);
    layout->setContentsMargins(8, 8, 8, 8);
    layout->setSpacing(6);
    layout->addWidget(createTabHeading(QStringLiteral("Corrections"), page));

    m_correctionsClipLabel = new QLabel(QStringLiteral("No clip selected"), page);
    m_correctionsClipLabel->setWordWrap(true);
    layout->addWidget(m_correctionsClipLabel);

    m_correctionsStatusLabel = new QLabel(QStringLiteral("Select a visual clip to add erase polygons."), page);
    m_correctionsStatusLabel->setWordWrap(true);
    layout->addWidget(m_correctionsStatusLabel);

    m_correctionsDrawModeCheck = new QCheckBox(QStringLiteral("Draw Polygons In Preview"), page);
    m_correctionsDrawModeCheck->setChecked(false);
    m_correctionsDrawModeCheck->setToolTip(
        QStringLiteral("Click 3 or more points on the clip in preview, then close polygon."));
    layout->addWidget(m_correctionsDrawModeCheck);

    m_correctionsClosePolygonButton = new QPushButton(QStringLiteral("Close Polygon"), page);
    m_correctionsCancelDraftButton = new QPushButton(QStringLiteral("Cancel Draft"), page);
    m_correctionsDeleteLastButton = new QPushButton(QStringLiteral("Delete Last Polygon"), page);
    m_correctionsClearAllButton = new QPushButton(QStringLiteral("Clear All Polygons"), page);

    layout->addWidget(m_correctionsClosePolygonButton);
    layout->addWidget(m_correctionsCancelDraftButton);
    layout->addSpacing(8);
    layout->addWidget(m_correctionsDeleteLastButton);
    layout->addWidget(m_correctionsClearAllButton);

    auto *hintLabel = new QLabel(
        QStringLiteral("Polygons erase alpha inside their shape. Use for webp sequence cleanup artifacts."),
        page);
    hintLabel->setWordWrap(true);
    hintLabel->setStyleSheet(QStringLiteral("QLabel { color: #8fa0b5; font-size: 11px; }"));
    layout->addWidget(hintLabel);

    layout->addStretch(1);
    return page;
}

QWidget *InspectorPane::buildTitlesTab()
{
    auto *page = new QWidget;
    auto *layout = new QVBoxLayout(page);
    layout->setContentsMargins(8, 8, 8, 8);
    layout->setSpacing(6);
    layout->addWidget(createTabHeading(QStringLiteral("Titles"), page));

    m_titlesInspectorClipLabel = new QLabel(QStringLiteral("No clip selected"));
    m_titlesInspectorClipLabel->setWordWrap(true);
    layout->addWidget(m_titlesInspectorClipLabel);

    m_titlesInspectorDetailsLabel = new QLabel;
    layout->addWidget(m_titlesInspectorDetailsLabel);

    // Text input
    auto *textRow = new QHBoxLayout;
    textRow->addWidget(new QLabel(QStringLiteral("Text:")));
    m_titleTextEdit = new QLineEdit;
    m_titleTextEdit->setPlaceholderText(QStringLiteral("Enter title text..."));
    textRow->addWidget(m_titleTextEdit, 1);
    layout->addLayout(textRow);

    // Position row
    auto *posRow = new QHBoxLayout;
    posRow->addWidget(new QLabel(QStringLiteral("X:")));
    m_titleXSpin = new QDoubleSpinBox;
    m_titleXSpin->setRange(-9999, 9999);
    m_titleXSpin->setDecimals(1);
    posRow->addWidget(m_titleXSpin);
    posRow->addWidget(new QLabel(QStringLiteral("Y:")));
    m_titleYSpin = new QDoubleSpinBox;
    m_titleYSpin->setRange(-9999, 9999);
    m_titleYSpin->setDecimals(1);
    posRow->addWidget(m_titleYSpin);
    layout->addLayout(posRow);

    // Font row
    auto *fontRow = new QHBoxLayout;
    fontRow->addWidget(new QLabel(QStringLiteral("Font:")));
    m_titleFontCombo = new QFontComboBox;
    fontRow->addWidget(m_titleFontCombo, 1);
    m_titleFontSizeSpin = new QDoubleSpinBox;
    m_titleFontSizeSpin->setRange(6, 999);
    m_titleFontSizeSpin->setDecimals(1);
    m_titleFontSizeSpin->setValue(48.0);
    fontRow->addWidget(m_titleFontSizeSpin);
    layout->addLayout(fontRow);

    // Style row
    auto *styleRow = new QHBoxLayout;
    m_titleBoldCheck = new QCheckBox(QStringLiteral("Bold"));
    m_titleBoldCheck->setChecked(true);
    styleRow->addWidget(m_titleBoldCheck);
    m_titleItalicCheck = new QCheckBox(QStringLiteral("Italic"));
    styleRow->addWidget(m_titleItalicCheck);
    styleRow->addWidget(new QLabel(QStringLiteral("Opacity:")));
    m_titleOpacitySpin = new QDoubleSpinBox;
    m_titleOpacitySpin->setRange(0.0, 1.0);
    m_titleOpacitySpin->setDecimals(2);
    m_titleOpacitySpin->setSingleStep(0.05);
    m_titleOpacitySpin->setValue(1.0);
    styleRow->addWidget(m_titleOpacitySpin);
    layout->addLayout(styleRow);

    // Buttons
    auto *buttonRow = new QHBoxLayout;
    m_addTitleKeyframeButton = new QPushButton(QStringLiteral("Add Title At Playhead"));
    buttonRow->addWidget(m_addTitleKeyframeButton);
    m_removeTitleKeyframeButton = new QPushButton(QStringLiteral("Remove Selected"));
    buttonRow->addWidget(m_removeTitleKeyframeButton);
    layout->addLayout(buttonRow);

    auto *centerRow = new QHBoxLayout;
    m_titleCenterHorizontalButton = new QPushButton(QStringLiteral("Center H"));
    m_titleCenterHorizontalButton->setToolTip(QStringLiteral("Center title horizontally"));
    centerRow->addWidget(m_titleCenterHorizontalButton);
    m_titleCenterVerticalButton = new QPushButton(QStringLiteral("Center V"));
    m_titleCenterVerticalButton->setToolTip(QStringLiteral("Center title vertically"));
    centerRow->addWidget(m_titleCenterVerticalButton);
    layout->addLayout(centerRow);

    // Auto-scroll
    m_titleAutoScrollCheck = new QCheckBox(QStringLiteral("Auto-scroll to playhead"));
    m_titleAutoScrollCheck->setChecked(true);
    layout->addWidget(m_titleAutoScrollCheck);

    // Table
    m_titleKeyframeTable = new QTableWidget;
    m_titleKeyframeTable->setColumnCount(7);
    m_titleKeyframeTable->setHorizontalHeaderLabels(
        {QStringLiteral("Frame"), QStringLiteral("Text"), QStringLiteral("X"),
         QStringLiteral("Y"), QStringLiteral("Size"), QStringLiteral("Opacity"),
         QStringLiteral("Interp")});
    m_titleKeyframeTable->setSelectionBehavior(QAbstractItemView::SelectRows);
    m_titleKeyframeTable->setSelectionMode(QAbstractItemView::ExtendedSelection);
    m_titleKeyframeTable->setEditTriggers(
        QAbstractItemView::DoubleClicked | QAbstractItemView::EditKeyPressed);
    m_titleKeyframeTable->verticalHeader()->setVisible(false);
    m_titleKeyframeTable->horizontalHeader()->setStretchLastSection(true);
    m_titleKeyframeTable->horizontalHeader()->setSectionResizeMode(QHeaderView::Stretch);
    layout->addWidget(m_titleKeyframeTable, 1);

    return page;
}

QWidget *InspectorPane::buildSyncTab()
{
    auto *page = new QWidget;
    auto *layout = new QVBoxLayout(page);
    layout->addWidget(createTabHeading(QStringLiteral("Sync"), page));

    m_syncInspectorClipLabel = new QLabel(QStringLiteral("Sync"), page);
    m_syncInspectorDetailsLabel = new QLabel(QStringLiteral("No render sync markers in the timeline."), page);
    m_syncInspectorDetailsLabel->setWordWrap(true);

    m_syncTable = new QTableWidget(page);
    m_syncTable->setColumnCount(4);
    m_syncTable->setHorizontalHeaderLabels(
        {QStringLiteral("Clip"), QStringLiteral("Frame"), QStringLiteral("Count"), QStringLiteral("Action")});
    m_syncTable->horizontalHeader()->setStretchLastSection(true);

    layout->addWidget(m_syncInspectorClipLabel);
    layout->addWidget(m_syncInspectorDetailsLabel);
    m_clearAllSyncPointsButton = new QPushButton(QStringLiteral("Clear All Sync Points"), page);
    m_clearAllSyncPointsButton->setToolTip(
        QStringLiteral("Remove every render sync marker across all clips."));
    layout->addWidget(m_clearAllSyncPointsButton);
    layout->addWidget(m_syncTable, 1);

    return page;
}

QWidget *InspectorPane::buildKeyframesTab()
{
    auto *page = new QWidget;
    auto *layout = new QVBoxLayout(page);
    layout->addWidget(createTabHeading(QStringLiteral("Keyframes"), page));

    m_keyframesInspectorClipLabel = new QLabel(QStringLiteral("No visual clip selected"), page);
    m_keyframesInspectorDetailsLabel = new QLabel(QStringLiteral("Select a visual clip to inspect its keyframes."), page);
    m_keyframesInspectorDetailsLabel->setWordWrap(true);

    auto *form = new QFormLayout;
    m_videoTranslationXSpin = new QDoubleSpinBox(page);
    m_videoTranslationYSpin = new QDoubleSpinBox(page);
    m_videoRotationSpin = new QDoubleSpinBox(page);
    m_videoScaleXSpin = new QDoubleSpinBox(page);
    m_videoScaleYSpin = new QDoubleSpinBox(page);
    m_videoInterpolationCombo = new QComboBox(page);
    m_mirrorHorizontalCheckBox = new QCheckBox(QStringLiteral("Mirror Horizontal"), page);
    m_mirrorVerticalCheckBox = new QCheckBox(QStringLiteral("Mirror Vertical"), page);
    m_lockVideoScaleCheckBox = new QCheckBox(QStringLiteral("Lock Scale"), page);
    m_keyframeSpaceCheckBox = new QCheckBox(QStringLiteral("Clip-Relative Frames"), page);
    m_keyframeSkipAwareTimingCheckBox = new QCheckBox(QStringLiteral("Skip Aware Timing"), page);
    m_addVideoKeyframeButton = new QPushButton(QStringLiteral("Add Keyframe"), page);
    m_removeVideoKeyframeButton = new QPushButton(QStringLiteral("Remove Keyframe"), page);
    m_flipHorizontalButton = new QPushButton(QStringLiteral("Flip Horizontal"), page);

    m_videoInterpolationCombo->addItem(QStringLiteral("Step"));
    m_videoInterpolationCombo->addItem(QStringLiteral("Linear"));
    m_lockVideoScaleCheckBox->setChecked(false);
    m_keyframeSpaceCheckBox->setChecked(true);
    m_keyframeSkipAwareTimingCheckBox->setChecked(false);

    for (QDoubleSpinBox *spin : {
             m_videoTranslationXSpin, m_videoTranslationYSpin, m_videoRotationSpin,
             m_videoScaleXSpin, m_videoScaleYSpin})
    {
        spin->setDecimals(3);
        spin->setRange(-100000.0, 100000.0);
    }
    m_videoScaleXSpin->setValue(1.0);
    m_videoScaleYSpin->setValue(1.0);

    form->addRow(QStringLiteral("Translate X"), m_videoTranslationXSpin);
    form->addRow(QStringLiteral("Translate Y"), m_videoTranslationYSpin);
    form->addRow(QStringLiteral("Rotation"), m_videoRotationSpin);
    form->addRow(QStringLiteral("Scale X"), m_videoScaleXSpin);
    form->addRow(QStringLiteral("Scale Y"), m_videoScaleYSpin);
    form->addRow(QStringLiteral("Interpolation"), m_videoInterpolationCombo);

    auto *buttonRow = new QHBoxLayout;
    buttonRow->addWidget(m_addVideoKeyframeButton);
    buttonRow->addWidget(m_removeVideoKeyframeButton);
    buttonRow->addWidget(m_flipHorizontalButton);

    m_keyframesAutoScrollCheckBox = new QCheckBox(QStringLiteral("Auto Scroll"), page);
    m_keyframesFollowCurrentCheckBox = new QCheckBox(QStringLiteral("Follow Current Keyframe"), page);
    m_keyframesAutoScrollCheckBox->setChecked(true);
    m_keyframesFollowCurrentCheckBox->setChecked(true);

    m_videoKeyframeTable = new QTableWidget(page);
    m_videoKeyframeTable->setColumnCount(7);
    m_videoKeyframeTable->setHorizontalHeaderLabels({QStringLiteral("Frame"),
                                                     QStringLiteral("X"),
                                                     QStringLiteral("Y"),
                                                     QStringLiteral("Rot"),
                                                     QStringLiteral("Scale X"),
                                                     QStringLiteral("Scale Y"),
                                                     QStringLiteral("Interp")});
    m_videoKeyframeTable->setSelectionBehavior(QAbstractItemView::SelectRows);
    m_videoKeyframeTable->setSelectionMode(QAbstractItemView::ExtendedSelection);
    m_videoKeyframeTable->setEditTriggers(QAbstractItemView::DoubleClicked |
                                          QAbstractItemView::EditKeyPressed);
    m_videoKeyframeTable->verticalHeader()->setVisible(false);
    m_videoKeyframeTable->horizontalHeader()->setStretchLastSection(true);
    m_videoKeyframeTable->horizontalHeader()->setSectionResizeMode(QHeaderView::Stretch);

    layout->addWidget(m_keyframesInspectorClipLabel);
    layout->addWidget(m_keyframesInspectorDetailsLabel);
    layout->addLayout(form);
    layout->addWidget(m_lockVideoScaleCheckBox);
    layout->addWidget(m_keyframeSpaceCheckBox);
    layout->addWidget(m_keyframeSkipAwareTimingCheckBox);
    layout->addWidget(m_keyframesAutoScrollCheckBox);
    layout->addWidget(m_keyframesFollowCurrentCheckBox);
    layout->addWidget(m_mirrorHorizontalCheckBox);
    layout->addWidget(m_mirrorVerticalCheckBox);
    layout->addLayout(buttonRow);
    layout->addWidget(m_videoKeyframeTable, 1);

    return page;
}

QWidget *InspectorPane::buildTranscriptTab()
{
    auto *page = new QWidget;
    auto *layout = new QVBoxLayout(page);
    layout->addWidget(createTabHeading(QStringLiteral("Transcript"), page));
    layout->setContentsMargins(0, 0, 0, 0);

    auto *splitter = new QSplitter(Qt::Vertical, page);
    splitter->setChildrenCollapsible(false);

    auto *settingsContainer = new QWidget(splitter);
    auto *settingsLayout = new QVBoxLayout(settingsContainer);
    settingsLayout->setContentsMargins(8, 8, 8, 8);

    m_transcriptInspectorClipLabel = new QLabel(QStringLiteral("No transcript selected"), settingsContainer);
    m_transcriptInspectorDetailsLabel = new QLabel(QStringLiteral("Select a clip with a WhisperX JSON transcript."), settingsContainer);
    m_transcriptInspectorDetailsLabel->setWordWrap(true);

    auto *form = new QFormLayout;
    m_transcriptOverlayEnabledCheckBox = new QCheckBox(QStringLiteral("Enable Overlay"), settingsContainer);
    m_transcriptMaxLinesSpin = new QSpinBox(settingsContainer);
    m_transcriptMaxCharsSpin = new QSpinBox(settingsContainer);
    m_transcriptAutoScrollCheckBox = new QCheckBox(QStringLiteral("Auto Scroll"), settingsContainer);
    m_transcriptFollowCurrentWordCheckBox = new QCheckBox(QStringLiteral("Follow Current Word"), settingsContainer);
    m_transcriptOverlayXSpin = new QDoubleSpinBox(settingsContainer);
    m_transcriptOverlayYSpin = new QDoubleSpinBox(settingsContainer);
    m_transcriptOverlayWidthSpin = new QSpinBox(settingsContainer);
    m_transcriptOverlayHeightSpin = new QSpinBox(settingsContainer);
    m_transcriptFontFamilyCombo = new QFontComboBox(settingsContainer);
    m_transcriptFontSizeSpin = new QSpinBox(settingsContainer);
    m_transcriptBoldCheckBox = new QCheckBox(QStringLiteral("Bold"), settingsContainer);
    m_transcriptItalicCheckBox = new QCheckBox(QStringLiteral("Italic"), settingsContainer);

    m_transcriptMaxLinesSpin->setRange(1, 20);
    m_transcriptMaxCharsSpin->setRange(1, 200);
    m_transcriptOverlayWidthSpin->setRange(1, 10000);
    m_transcriptOverlayHeightSpin->setRange(1, 10000);
    m_transcriptFontSizeSpin->setRange(8, 256);

    form->addRow(QStringLiteral("Overlay"), m_transcriptOverlayEnabledCheckBox);
    form->addRow(QStringLiteral("Max Lines"), m_transcriptMaxLinesSpin);
    form->addRow(QStringLiteral("Max Chars"), m_transcriptMaxCharsSpin);
    form->addRow(QStringLiteral("Auto Scroll"), m_transcriptAutoScrollCheckBox);
    form->addRow(QStringLiteral("Follow Word"), m_transcriptFollowCurrentWordCheckBox);
    form->addRow(QStringLiteral("X"), m_transcriptOverlayXSpin);
    form->addRow(QStringLiteral("Y"), m_transcriptOverlayYSpin);
    form->addRow(QStringLiteral("Width"), m_transcriptOverlayWidthSpin);
    form->addRow(QStringLiteral("Height"), m_transcriptOverlayHeightSpin);
    form->addRow(QStringLiteral("Font"), m_transcriptFontFamilyCombo);
    form->addRow(QStringLiteral("Font Size"), m_transcriptFontSizeSpin);
    form->addRow(QStringLiteral("Bold"), m_transcriptBoldCheckBox);
    form->addRow(QStringLiteral("Italic"), m_transcriptItalicCheckBox);

    auto *speechSectionLabel = new QLabel(QStringLiteral("Speech Filter"), settingsContainer);
    speechSectionLabel->setStyleSheet(QStringLiteral("font-weight: 600; color: #8fa3b8;"));

    auto *speechForm = new QFormLayout;
    m_speechFilterEnabledCheckBox = new QCheckBox(QStringLiteral("Enable Speech Filter"), settingsContainer);
    m_transcriptPrependMsSpin = new QSpinBox(settingsContainer);
    m_transcriptPostpendMsSpin = new QSpinBox(settingsContainer);
    m_speechFilterFadeSamplesSpin = new QSpinBox(settingsContainer);

    m_transcriptPrependMsSpin->setRange(0, 10000);
    m_transcriptPrependMsSpin->setValue(0);
    m_transcriptPrependMsSpin->setSuffix(QStringLiteral(" ms"));
    m_transcriptPrependMsSpin->setToolTip(QStringLiteral("Milliseconds to add before each word"));

    m_transcriptPostpendMsSpin->setRange(0, 10000);
    m_transcriptPostpendMsSpin->setValue(0);
    m_transcriptPostpendMsSpin->setSuffix(QStringLiteral(" ms"));
    m_transcriptPostpendMsSpin->setToolTip(QStringLiteral("Milliseconds to add after each word"));

    m_speechFilterFadeSamplesSpin->setRange(0, 5000);
    m_speechFilterFadeSamplesSpin->setValue(250);
    m_speechFilterFadeSamplesSpin->setSuffix(QStringLiteral(" samples"));
    m_speechFilterFadeSamplesSpin->setToolTip(QStringLiteral("Crossfade duration at speech boundaries (0 = no fade)"));

    speechForm->addRow(QStringLiteral("Speech Filter"), m_speechFilterEnabledCheckBox);
    speechForm->addRow(QStringLiteral("Prepend Time"), m_transcriptPrependMsSpin);
    speechForm->addRow(QStringLiteral("Postpend Time"), m_transcriptPostpendMsSpin);
    speechForm->addRow(QStringLiteral("Fade Length"), m_speechFilterFadeSamplesSpin);

    m_transcriptTable = new QTableWidget(splitter);
    m_transcriptTable->setColumnCount(3);
    m_transcriptTable->setHorizontalHeaderLabels(
        {QStringLiteral("Start"), QStringLiteral("End"), QStringLiteral("Text")});
    m_transcriptTable->setSelectionBehavior(QAbstractItemView::SelectRows);
    m_transcriptTable->setSelectionMode(QAbstractItemView::ExtendedSelection);
    m_transcriptTable->setEditTriggers(QAbstractItemView::DoubleClicked |
                                       QAbstractItemView::EditKeyPressed);
    m_transcriptTable->verticalHeader()->setVisible(false);
    m_transcriptTable->horizontalHeader()->setStretchLastSection(true);

    settingsLayout->addWidget(m_transcriptInspectorClipLabel);
    settingsLayout->addWidget(m_transcriptInspectorDetailsLabel);
    settingsLayout->addLayout(form);
    settingsLayout->addWidget(speechSectionLabel);
    settingsLayout->addLayout(speechForm);
    settingsLayout->addStretch(1);

    auto *settingsScroll = new QScrollArea(page);
    settingsScroll->setWidgetResizable(true);
    settingsScroll->setFrameShape(QFrame::NoFrame);
    settingsScroll->setWidget(settingsContainer);

    splitter->addWidget(settingsScroll);
    splitter->addWidget(m_transcriptTable);
    splitter->setStretchFactor(0, 0);
    splitter->setStretchFactor(1, 1);
    splitter->setSizes({360, 420});

    layout->addWidget(splitter);

    return page;
}

QWidget *InspectorPane::buildClipsTab()
{
    auto *page = new QWidget;
    auto *layout = new QVBoxLayout(page);
    layout->setContentsMargins(8, 8, 8, 8);
    layout->setSpacing(6);

    m_clipsTable = new QTableWidget(page);
    m_clipsTable->setColumnCount(6);
    m_clipsTable->setHorizontalHeaderLabels({
        QStringLiteral("Name"),
        QStringLiteral("Track"),
        QStringLiteral("Type"),
        QStringLiteral("Start"),
        QStringLiteral("Duration"),
        QStringLiteral("File"),
    });
    m_clipsTable->setEditTriggers(QAbstractItemView::DoubleClicked | QAbstractItemView::EditKeyPressed);
    m_clipsTable->setSelectionBehavior(QAbstractItemView::SelectRows);
    m_clipsTable->setSelectionMode(QAbstractItemView::SingleSelection);
    m_clipsTable->setFocusPolicy(Qt::ClickFocus);
    m_clipsTable->setAlternatingRowColors(true);
    m_clipsTable->verticalHeader()->setVisible(false);
    m_clipsTable->horizontalHeader()->setStretchLastSection(true);
    m_clipsTable->horizontalHeader()->setSectionResizeMode(0, QHeaderView::Stretch);
    m_clipsTable->horizontalHeader()->setSectionResizeMode(1, QHeaderView::ResizeToContents);
    m_clipsTable->horizontalHeader()->setSectionResizeMode(2, QHeaderView::ResizeToContents);
    m_clipsTable->horizontalHeader()->setSectionResizeMode(3, QHeaderView::ResizeToContents);
    m_clipsTable->horizontalHeader()->setSectionResizeMode(4, QHeaderView::ResizeToContents);
    m_clipsTable->horizontalHeader()->setSectionResizeMode(5, QHeaderView::Stretch);

    layout->addWidget(m_clipsTable, 1);
    return page;
}

QWidget *InspectorPane::buildTracksTab()
{
    auto *page = new QWidget;
    auto *layout = new QVBoxLayout(page);
    layout->setContentsMargins(8, 8, 8, 8);
    layout->setSpacing(6);

    m_tracksTable = new QTableWidget(page);
    m_tracksTable->setColumnCount(3);
    m_tracksTable->setHorizontalHeaderLabels({
        QStringLiteral("Track"),
        QStringLiteral("Visual"),
        QStringLiteral("Audio"),
    });
    m_tracksTable->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_tracksTable->setSelectionBehavior(QAbstractItemView::SelectRows);
    m_tracksTable->setSelectionMode(QAbstractItemView::SingleSelection);
    m_tracksTable->setFocusPolicy(Qt::ClickFocus);
    m_tracksTable->setAlternatingRowColors(true);
    m_tracksTable->verticalHeader()->setVisible(false);
    m_tracksTable->horizontalHeader()->setStretchLastSection(true);
    m_tracksTable->horizontalHeader()->setSectionResizeMode(0, QHeaderView::Stretch);
    m_tracksTable->horizontalHeader()->setSectionResizeMode(1, QHeaderView::ResizeToContents);
    m_tracksTable->horizontalHeader()->setSectionResizeMode(2, QHeaderView::ResizeToContents);

    layout->addWidget(m_tracksTable, 1);
    return page;
}

QWidget *InspectorPane::buildOutputTab()
{
    auto *page = new QWidget;
    auto *layout = new QVBoxLayout(page);
    layout->addWidget(createTabHeading(QStringLiteral("Output"), page));

    m_outputRangeSummaryLabel = new QLabel(QStringLiteral("Timeline export range: 00:00:00:00 -> 00:00:10:00"), page);
    m_outputRangeSummaryLabel->setWordWrap(true);
    m_outputRangeSummaryLabel->setTextInteractionFlags(Qt::TextSelectableByMouse);

    auto *form = new QFormLayout;
    m_outputWidthSpin = new QSpinBox(page);
    m_outputHeightSpin = new QSpinBox(page);
    m_exportStartSpin = new QSpinBox(page);
    m_exportEndSpin = new QSpinBox(page);
    m_outputFormatCombo = new QComboBox(page);

    m_outputWidthSpin->setRange(16, 7680);
    m_outputWidthSpin->setValue(1080);
    m_outputHeightSpin->setRange(16, 4320);
    m_outputHeightSpin->setValue(1920);
    m_exportStartSpin->setRange(0, 999999);
    m_exportEndSpin->setRange(0, 999999);

    m_outputFormatCombo->addItem(QStringLiteral("MP4"), QStringLiteral("mp4"));
    m_outputFormatCombo->addItem(QStringLiteral("MOV"), QStringLiteral("mov"));
    m_outputFormatCombo->addItem(QStringLiteral("WebM"), QStringLiteral("webm"));
    m_renderUseProxiesCheckBox = new QCheckBox(QStringLiteral("Use Proxies For Render"), page);

    form->addRow(QStringLiteral("Output Width"), m_outputWidthSpin);
    form->addRow(QStringLiteral("Output Height"), m_outputHeightSpin);
    form->addRow(QStringLiteral("Export Start Frame"), m_exportStartSpin);
    form->addRow(QStringLiteral("Export End Frame"), m_exportEndSpin);
    form->addRow(QStringLiteral("Output Format"), m_outputFormatCombo);

    m_backgroundColorButton = new QPushButton(page);
    m_backgroundColorButton->setText(QStringLiteral("Black"));
    m_backgroundColorButton->setToolTip(QStringLiteral("Background color for the rendered output"));
    m_backgroundColorButton->setStyleSheet(
        QStringLiteral("QPushButton { background: #000000; color: #ffffff; "
                        "border: 1px solid #2e3b4a; border-radius: 4px; padding: 4px 8px; }"));
    form->addRow(QStringLiteral("Background"), m_backgroundColorButton);

    m_renderButton = new QPushButton(QStringLiteral("Render"), page);

    layout->addWidget(m_outputRangeSummaryLabel);
    layout->addLayout(form);
    layout->addWidget(m_renderUseProxiesCheckBox);
    layout->addWidget(m_renderButton);
    layout->addStretch(1);

    return page;
}

QWidget *InspectorPane::buildPreviewTab()
{
    auto *page = new QWidget;
    auto *layout = new QVBoxLayout(page);
    layout->addWidget(createTabHeading(QStringLiteral("Preview"), page));

    auto *summary = new QLabel(QStringLiteral("Preview controls affect only the editor preview."), page);
    summary->setWordWrap(true);

    m_previewHideOutsideOutputCheckBox =
        new QCheckBox(QStringLiteral("Hide Content Outside Output Window"), page);
    m_previewHideOutsideOutputCheckBox->setChecked(false);
    m_previewHideOutsideOutputCheckBox->setToolTip(
        QStringLiteral("Clip the preview to the current output frame so off-frame content is hidden."));

    // Zoom control section
    auto *zoomSectionLabel = new QLabel(QStringLiteral("Zoom"), page);
    zoomSectionLabel->setStyleSheet(QStringLiteral("font-weight: 600; color: #8fa3b8; margin-top: 8px;"));
    
    auto *zoomLayout = new QHBoxLayout();
    m_previewZoomSpin = new QDoubleSpinBox(page);
    m_previewZoomSpin->setDecimals(2);
    m_previewZoomSpin->setRange(0.1, 5.0);
    m_previewZoomSpin->setSingleStep(0.1);
    m_previewZoomSpin->setValue(1.0);
    m_previewZoomSpin->setSuffix(QStringLiteral("x"));
    m_previewZoomSpin->setToolTip(
        QStringLiteral("Preview zoom level (0.1x to 5.0x). Use mouse wheel over preview for smooth zoom."));
    
    m_previewZoomResetButton = new QPushButton(QStringLiteral("Reset"), page);
    m_previewZoomResetButton->setToolTip(QStringLiteral("Reset zoom to 1.0x and center the view"));
    
    zoomLayout->addWidget(m_previewZoomSpin);
    zoomLayout->addWidget(m_previewZoomResetButton);
    
    auto *zoomHelpLabel = new QLabel(
        QStringLiteral("Mouse wheel: zoom at cursor position. Drag to pan when zoomed."), page);
    zoomHelpLabel->setWordWrap(true);
    zoomHelpLabel->setStyleSheet(QStringLiteral("color: #6b7a8f; font-size: 11px;"));

    auto *bufferingSectionLabel = new QLabel(QStringLiteral("Playback Buffering"), page);
    bufferingSectionLabel->setStyleSheet(QStringLiteral("font-weight: 600; color: #8fa3b8; margin-top: 8px;"));

    m_previewPlaybackCacheFallbackCheckBox =
        new QCheckBox(QStringLiteral("Allow Cached-Frame Fallback During Playback"), page);
    m_previewPlaybackCacheFallbackCheckBox->setChecked(editor::debugPlaybackCacheFallbackEnabled());
    m_previewPlaybackCacheFallbackCheckBox->setToolTip(
        QStringLiteral("When playback buffers miss, reuse timeline cache frames so still images remain visible."));

    m_previewLeadPrefetchEnabledCheckBox = new QCheckBox(QStringLiteral("Enable Lead Prefetch"), page);
    m_previewLeadPrefetchEnabledCheckBox->setChecked(editor::debugLeadPrefetchEnabled());
    m_previewLeadPrefetchEnabledCheckBox->setToolTip(
        QStringLiteral("Prefetch frames ahead of playhead to reduce visible misses while playing."));

    auto *bufferingForm = new QFormLayout();
    m_previewLeadPrefetchCountSpin = new QSpinBox(page);
    m_previewLeadPrefetchCountSpin->setRange(0, 8);
    m_previewLeadPrefetchCountSpin->setValue(editor::debugLeadPrefetchCount());
    m_previewLeadPrefetchCountSpin->setToolTip(
        QStringLiteral("How many lead-prefetch requests to schedule (0-8)."));
    bufferingForm->addRow(QStringLiteral("Lead Prefetch Count"), m_previewLeadPrefetchCountSpin);

    m_previewPlaybackWindowAheadSpin = new QSpinBox(page);
    m_previewPlaybackWindowAheadSpin->setRange(1, 24);
    m_previewPlaybackWindowAheadSpin->setValue(editor::debugPlaybackWindowAhead());
    m_previewPlaybackWindowAheadSpin->setToolTip(
        QStringLiteral("Decode window ahead of playhead for playback pipeline clips (1-24)."));
    bufferingForm->addRow(QStringLiteral("Playback Window Ahead"), m_previewPlaybackWindowAheadSpin);

    m_previewVisibleQueueReserveSpin = new QSpinBox(page);
    m_previewVisibleQueueReserveSpin->setRange(0, 64);
    m_previewVisibleQueueReserveSpin->setValue(editor::debugVisibleQueueReserve());
    m_previewVisibleQueueReserveSpin->setToolTip(
        QStringLiteral("Reserved visible-request budget before aggressive prefetch throttling (0-64)."));
    bufferingForm->addRow(QStringLiteral("Visible Queue Reserve"), m_previewVisibleQueueReserveSpin);

    layout->addWidget(summary);
    layout->addWidget(m_previewHideOutsideOutputCheckBox);
    layout->addSpacing(12);
    layout->addWidget(zoomSectionLabel);
    layout->addLayout(zoomLayout);
    layout->addWidget(zoomHelpLabel);
    layout->addSpacing(12);
    layout->addWidget(bufferingSectionLabel);
    layout->addWidget(m_previewPlaybackCacheFallbackCheckBox);
    layout->addWidget(m_previewLeadPrefetchEnabledCheckBox);
    layout->addLayout(bufferingForm);
    layout->addStretch(1);
    return page;
}

QWidget *InspectorPane::buildClipTab()
{
    auto *page = new QWidget;
    auto *layout = new QVBoxLayout(page);
    layout->addWidget(createTabHeading(QStringLiteral("Properties"), page));
    auto *form = new QFormLayout;

    m_clipInspectorClipLabel = new QLabel(QStringLiteral("No clip selected"), page);
    m_clipProxyUsageLabel = new QLabel(QStringLiteral("Playback Source: None"), page);
    m_clipPlaybackSourceLabel = new QLabel(QStringLiteral("Proxy In Use: No"), page);
    m_clipOriginalInfoLabel = new QLabel(QStringLiteral("Original\nNo clip selected."), page);
    m_clipProxyInfoLabel = new QLabel(QStringLiteral("Proxy\nNo proxy configured."), page);
    m_clipPlaybackRateSpin = new QDoubleSpinBox(page);
    m_clipPlaybackRateSpin->setDecimals(4);
    m_clipPlaybackRateSpin->setRange(0.001, 4.0);
    m_clipPlaybackRateSpin->setSingleStep(0.0001);
    m_clipPlaybackRateSpin->setValue(1.0);
    m_clipPlaybackRateSpin->setToolTip(
        QStringLiteral("Adjust clip playback speed with fine precision. "
                       "Example: 0.9999 for a slight slow-down."));
    m_clipPlaybackRateSpin->setValue(1.0);
    auto *trackSectionLabel = new QLabel(QStringLiteral("Track"), page);
    trackSectionLabel->setStyleSheet(QStringLiteral("font-weight: 600; color: #8fa3b8;"));
    m_trackInspectorLabel = new QLabel(QStringLiteral("No track selected"), page);
    m_trackInspectorDetailsLabel = new QLabel(QStringLiteral("Select a track header to edit track-wide properties."), page);
    m_trackNameEdit = new QLineEdit(page);
    m_trackHeightSpin = new QSpinBox(page);
    m_trackHeightSpin->setRange(28, 240);
    m_trackVisualModeCombo = new QComboBox(page);
    m_trackVisualModeCombo->addItem(trackVisualModeLabel(TrackVisualMode::Enabled),
                                    static_cast<int>(TrackVisualMode::Enabled));
    m_trackVisualModeCombo->addItem(trackVisualModeLabel(TrackVisualMode::ForceOpaque),
                                    static_cast<int>(TrackVisualMode::ForceOpaque));
    m_trackVisualModeCombo->addItem(trackVisualModeLabel(TrackVisualMode::Hidden),
                                    static_cast<int>(TrackVisualMode::Hidden));
    m_trackAudioEnabledCheckBox = new QCheckBox(QStringLiteral("Track Audio Enabled"), page);
    m_trackCrossfadeSecondsSpin = new QDoubleSpinBox(page);
    m_trackCrossfadeSecondsSpin->setDecimals(2);
    m_trackCrossfadeSecondsSpin->setRange(0.01, 30.0);
    m_trackCrossfadeSecondsSpin->setSingleStep(0.05);
    m_trackCrossfadeSecondsSpin->setValue(0.50);
    m_trackCrossfadeButton = new QPushButton(QStringLiteral("Crossfade Consecutive Clips"), page);
    auto *audioSectionLabel = new QLabel(QStringLiteral("Audio"), page);
    audioSectionLabel->setStyleSheet(QStringLiteral("font-weight: 600; color: #8fa3b8;"));
    m_audioInspectorClipLabel = new QLabel(QStringLiteral("No audio clip selected"), page);
    m_audioInspectorDetailsLabel = new QLabel(QStringLiteral("Select an audio clip to inspect playback details."), page);

    for (QLabel *label : {m_clipInspectorClipLabel, m_clipProxyUsageLabel, m_clipPlaybackSourceLabel,
                          m_clipOriginalInfoLabel, m_clipProxyInfoLabel,
                          m_trackInspectorLabel, m_trackInspectorDetailsLabel,
                          m_audioInspectorClipLabel, m_audioInspectorDetailsLabel})
    {
        label->setWordWrap(true);
        label->setTextInteractionFlags(Qt::TextSelectableByMouse);
    }

    layout->addWidget(m_clipInspectorClipLabel);
    layout->addWidget(m_clipProxyUsageLabel);
    layout->addWidget(m_clipPlaybackSourceLabel);
    form->addRow(QStringLiteral("Playback Speed"), m_clipPlaybackRateSpin);
    layout->addLayout(form);
    layout->addWidget(m_clipOriginalInfoLabel);
    layout->addWidget(m_clipProxyInfoLabel);
    auto *trackForm = new QFormLayout;
    trackForm->addRow(QStringLiteral("Track Name"), m_trackNameEdit);
    trackForm->addRow(QStringLiteral("Track Height"), m_trackHeightSpin);
    trackForm->addRow(QStringLiteral("Track Visuals"), m_trackVisualModeCombo);
    trackForm->addRow(QStringLiteral("Crossfade"), m_trackCrossfadeSecondsSpin);
    layout->addWidget(trackSectionLabel);
    layout->addWidget(m_trackInspectorLabel);
    layout->addWidget(m_trackInspectorDetailsLabel);
    layout->addLayout(trackForm);
    layout->addWidget(m_trackAudioEnabledCheckBox);
    layout->addWidget(m_trackCrossfadeButton);
    layout->addWidget(audioSectionLabel);
    layout->addWidget(m_audioInspectorClipLabel);
    layout->addWidget(m_audioInspectorDetailsLabel);
    layout->addStretch(1);

    return page;
}

QWidget *InspectorPane::buildProfileTab()
{
    auto *page = new QWidget;
    auto *layout = new QVBoxLayout(page);
    layout->addWidget(createTabHeading(QStringLiteral("System"), page));

    m_profileSummaryTable = new QTableWidget(page);
    m_profileSummaryTable->setColumnCount(2);
    m_profileSummaryTable->setHorizontalHeaderLabels({QStringLiteral("Property"), QStringLiteral("Value")});
    m_profileSummaryTable->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_profileSummaryTable->setSelectionMode(QAbstractItemView::NoSelection);
    m_profileSummaryTable->setFocusPolicy(Qt::NoFocus);
    m_profileSummaryTable->verticalHeader()->setVisible(false);
    m_profileSummaryTable->horizontalHeader()->setStretchLastSection(true);
    m_profileSummaryTable->horizontalHeader()->setSectionResizeMode(0, QHeaderView::ResizeToContents);
    m_profileSummaryTable->horizontalHeader()->setSectionResizeMode(1, QHeaderView::Stretch);

    m_profileBenchmarkButton = new QPushButton(QStringLiteral("Run Decode Benchmark"), page);

    layout->addWidget(m_profileSummaryTable, 1);
    layout->addWidget(m_profileBenchmarkButton);
    return page;
}

QWidget *InspectorPane::buildProjectsTab()
{
    auto *page = new QWidget;
    auto *layout = new QVBoxLayout(page);
    layout->addWidget(createTabHeading(QStringLiteral("Projects"), page));

    m_projectSectionLabel = new QLabel(QStringLiteral("PROJECTS"), page);
    m_projectSectionLabel->setWordWrap(true);
    m_projectSectionLabel->setTextInteractionFlags(Qt::TextSelectableByMouse);
    layout->addWidget(m_projectSectionLabel);

    auto *buttonRow = new QHBoxLayout;
    m_newProjectButton = new QPushButton(QStringLiteral("New"), page);
    m_saveProjectAsButton = new QPushButton(QStringLiteral("Save As"), page);
    m_renameProjectButton = new QPushButton(QStringLiteral("Rename"), page);
    buttonRow->addWidget(m_newProjectButton);
    buttonRow->addWidget(m_saveProjectAsButton);
    buttonRow->addWidget(m_renameProjectButton);
    layout->addLayout(buttonRow);

    m_projectsList = new QListWidget(page);
    m_projectsList->setSelectionMode(QAbstractItemView::SingleSelection);
    m_projectsList->setAlternatingRowColors(true);
    layout->addWidget(m_projectsList, 1);

    return page;
}

void InspectorPane::refresh()
{
    emit refreshRequested();
}
