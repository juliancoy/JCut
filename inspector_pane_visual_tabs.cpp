#include "inspector_pane.h"
#include "inspector_pane_tab_helpers.h"
#include "editor_effect_presets.h"
#include "editor_shared.h"
#include "grading_histogram_widget.h"

#include <QAbstractButton>
#include <QBrush>
#include <QButtonGroup>
#include <QCheckBox>
#include <QColor>
#include <QComboBox>
#include <QDial>
#include <QDoubleSpinBox>
#include <QFontComboBox>
#include <QFormLayout>
#include <QFrame>
#include <QGridLayout>
#include <QGroupBox>
#include <QHeaderView>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QPlainTextEdit>
#include <QScrollArea>
#include <QSignalBlocker>
#include <QStandardItemModel>
#include <QSlider>
#include <QSpinBox>
#include <QTableWidget>
#include <QPushButton>
#include <QToolButton>
#include <QVBoxLayout>

using namespace jcut::inspector;

QWidget *InspectorPane::buildGradingTab()
{
    auto *page = new QWidget;
    auto *pageLayout = new QVBoxLayout(page);
    pageLayout->setContentsMargins(0, 0, 0, 0);
    pageLayout->setSpacing(0);

    auto *scrollArea = new QScrollArea(page);
    scrollArea->setWidgetResizable(true);
    scrollArea->setFrameShape(QFrame::NoFrame);
    scrollArea->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);

    auto *content = new QWidget(scrollArea);
    auto *layout = createTabLayout(content);
    layout->addWidget(createTabHeading(QStringLiteral("Grade"), content));

    m_gradingPathLabel = new QLabel(QStringLiteral("No visual clip selected"), content);
    m_gradingPathLabel->setWordWrap(true);
    layout->addWidget(m_gradingPathLabel);

    m_gradingEditModeCombo = new QComboBox(content);
    m_gradingEditModeCombo->addItem(QStringLiteral("Levels"));
    m_gradingEditModeCombo->addItem(QStringLiteral("Curves"));
    m_gradingEditModeCombo->setVisible(false);

    auto *commonForm = new QFormLayout;
    commonForm->setRowWrapPolicy(QFormLayout::WrapAllRows);
    commonForm->setFieldGrowthPolicy(QFormLayout::AllNonFixedFieldsGrow);
    m_brightnessSpin = new QDoubleSpinBox(content);
    m_contrastSpin = new QDoubleSpinBox(content);
    m_saturationSpin = new QDoubleSpinBox(content);
    m_opacitySpin = new QDoubleSpinBox(content);
    m_brightnessSpin->setObjectName(QStringLiteral("grading.brightness"));
    m_contrastSpin->setObjectName(QStringLiteral("grading.contrast"));
    m_saturationSpin->setObjectName(QStringLiteral("grading.saturation"));

    for (QDoubleSpinBox *spin : {m_brightnessSpin, m_contrastSpin, m_saturationSpin})
    {
        spin->setRange(-10.0, 10.0);
        spin->setDecimals(3);
        spin->setSingleStep(0.05);
    }
    commonForm->addRow(QStringLiteral("Saturation"), m_saturationSpin);
    layout->addLayout(commonForm);

    m_gradingLevelsPanel = new QWidget(content);
    auto *levelsLayout = new QFormLayout(m_gradingLevelsPanel);
    levelsLayout->setContentsMargins(0, 0, 0, 0);
    levelsLayout->setRowWrapPolicy(QFormLayout::WrapAllRows);
    levelsLayout->setFieldGrowthPolicy(QFormLayout::AllNonFixedFieldsGrow);
    levelsLayout->addRow(QStringLiteral("Brightness"), m_brightnessSpin);
    levelsLayout->addRow(QStringLiteral("Contrast"), m_contrastSpin);

    // Shadows/Midtones/Highlights (Lift/Gamma/Gain)
    m_gradingCurvesPanel = new QWidget(content);
    auto *curvesLayout = new QVBoxLayout(m_gradingCurvesPanel);
    curvesLayout->setContentsMargins(0, 0, 0, 0);
    curvesLayout->setSpacing(6);

    auto *shadowsGroup = new QGroupBox(QStringLiteral("Shadows (Lift)"), m_gradingCurvesPanel);
    auto *shadowsLayout = new QHBoxLayout(shadowsGroup);
    m_shadowsRSpin = new QDoubleSpinBox(shadowsGroup);
    m_shadowsGSpin = new QDoubleSpinBox(shadowsGroup);
    m_shadowsBSpin = new QDoubleSpinBox(shadowsGroup);
    m_shadowsRSpin->setObjectName(QStringLiteral("grading.shadows.r"));
    m_shadowsGSpin->setObjectName(QStringLiteral("grading.shadows.g"));
    m_shadowsBSpin->setObjectName(QStringLiteral("grading.shadows.b"));
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

    auto *midtonesGroup = new QGroupBox(QStringLiteral("Midtones (Gamma)"), m_gradingCurvesPanel);
    auto *midtonesLayout = new QHBoxLayout(midtonesGroup);
    m_midtonesRSpin = new QDoubleSpinBox(midtonesGroup);
    m_midtonesGSpin = new QDoubleSpinBox(midtonesGroup);
    m_midtonesBSpin = new QDoubleSpinBox(midtonesGroup);
    m_midtonesRSpin->setObjectName(QStringLiteral("grading.midtones.r"));
    m_midtonesGSpin->setObjectName(QStringLiteral("grading.midtones.g"));
    m_midtonesBSpin->setObjectName(QStringLiteral("grading.midtones.b"));
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

    auto *highlightsGroup = new QGroupBox(QStringLiteral("Highlights (Gain)"), m_gradingCurvesPanel);
    auto *highlightsLayout = new QHBoxLayout(highlightsGroup);
    m_highlightsRSpin = new QDoubleSpinBox(highlightsGroup);
    m_highlightsGSpin = new QDoubleSpinBox(highlightsGroup);
    m_highlightsBSpin = new QDoubleSpinBox(highlightsGroup);
    m_highlightsRSpin->setObjectName(QStringLiteral("grading.highlights.r"));
    m_highlightsGSpin->setObjectName(QStringLiteral("grading.highlights.g"));
    m_highlightsBSpin->setObjectName(QStringLiteral("grading.highlights.b"));
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

    auto *curveChannelLayout = new QHBoxLayout;
    auto* curveChannelLabel = new QLabel(QStringLiteral("Channel"), m_gradingCurvesPanel);
    curveChannelLabel->setToolTip(QStringLiteral("Curve channel"));
    curveChannelLayout->addWidget(curveChannelLabel);
    m_gradingCurveChannelTabs = new QTabBar(m_gradingCurvesPanel);
    m_gradingCurveChannelTabs->addTab(QStringLiteral("Red"));
    m_gradingCurveChannelTabs->addTab(QStringLiteral("Green"));
    m_gradingCurveChannelTabs->addTab(QStringLiteral("Blue"));
    m_gradingCurveChannelTabs->addTab(QStringLiteral("Brightness"));
    m_gradingCurveChannelTabs->setDrawBase(false);
    m_gradingCurveChannelTabs->setExpanding(false);
    m_gradingCurveChannelTabs->setSizePolicy(QSizePolicy::MinimumExpanding, QSizePolicy::Fixed);
    m_gradingCurveChannelTabs->setStyleSheet(QStringLiteral(
        "QTabBar::tab { background:#1a2028; color:#9fb0c2; padding:5px 10px; border:1px solid #2f3a46; border-bottom:0; }"
        "QTabBar::tab:selected { background:#223246; color:#dbe9f8; }"));
    curveChannelLayout->addWidget(m_gradingCurveChannelTabs, 1);
    m_gradingCurveChannelCombo = new QComboBox(m_gradingCurvesPanel);
    m_gradingCurveChannelCombo->setObjectName(QStringLiteral("grading.curve.channel"));
    m_gradingCurveChannelCombo->addItem(QStringLiteral("Red"));
    m_gradingCurveChannelCombo->addItem(QStringLiteral("Green"));
    m_gradingCurveChannelCombo->addItem(QStringLiteral("Blue"));
    m_gradingCurveChannelCombo->addItem(QStringLiteral("Brightness"));
    m_gradingCurveChannelCombo->setVisible(false);
    curveChannelLayout->addWidget(m_gradingCurveChannelCombo);
    curveChannelLayout->addStretch();

    connect(m_gradingCurveChannelTabs, &QTabBar::currentChanged, this, [this](int index) {
        if (!m_gradingCurveChannelCombo) {
            return;
        }
        if (index >= 0 && index < m_gradingCurveChannelCombo->count() &&
            m_gradingCurveChannelCombo->currentIndex() != index) {
            m_gradingCurveChannelCombo->setCurrentIndex(index);
        }
    });
    connect(m_gradingCurveChannelCombo, qOverload<int>(&QComboBox::currentIndexChanged), this, [this](int index) {
        if (!m_gradingCurveChannelTabs) {
            return;
        }
        if (index >= 0 && index < m_gradingCurveChannelTabs->count() &&
            m_gradingCurveChannelTabs->currentIndex() != index) {
            m_gradingCurveChannelTabs->setCurrentIndex(index);
        }
    });

    auto *curveOptionsLayout = new QHBoxLayout;
    m_gradingCurveThreePointLockCheckBox =
        new QCheckBox(QStringLiteral("Sync Lift/Gamma/Gain"), m_gradingCurvesPanel);
    m_gradingCurveThreePointLockCheckBox->setObjectName(QStringLiteral("grading.curve.three_point_lock"));
    m_gradingCurveThreePointLockCheckBox->setChecked(false);
    m_gradingCurveThreePointLockCheckBox->setToolTip(QStringLiteral(
        "When enabled, the RGB Lift/Gamma/Gain numbers and the current curve channel stay linked "
        "as a three-point correction."));
    m_gradingCurveSmoothingCheckBox =
        new QCheckBox(QStringLiteral("Smooth"), m_gradingCurvesPanel);
    m_gradingCurveSmoothingCheckBox->setObjectName(QStringLiteral("grading.curve.smoothing"));
    m_gradingCurveSmoothingCheckBox->setChecked(true);
    m_gradingCurveSmoothingCheckBox->setToolTip(QStringLiteral("Smooth curve interpolation"));
    curveOptionsLayout->addWidget(m_gradingCurveThreePointLockCheckBox);
    curveOptionsLayout->addWidget(m_gradingCurveSmoothingCheckBox);
    curveOptionsLayout->addStretch();

    m_gradingHistogramWidget = new GradingHistogramWidget(m_gradingCurvesPanel);
    m_gradingHistogramWidget->setObjectName(QStringLiteral("grading.curve.histogram"));
    m_gradingHistogramWidget->setMinimumHeight(180);
    m_gradingHistogramWidget->setMaximumHeight(220);
    m_gradingHistogramWidget->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    m_gradingHistogramWidget->setToolTip(QStringLiteral(
        "Current-frame histogram.\n"
        "Select a channel, click to add points, drag points to shape the curve, right-click a point to remove it."));

    curvesLayout->addWidget(shadowsGroup);
    curvesLayout->addWidget(midtonesGroup);
    curvesLayout->addWidget(highlightsGroup);
    curvesLayout->addLayout(curveChannelLayout);
    curvesLayout->addLayout(curveOptionsLayout);
    curvesLayout->addWidget(m_gradingHistogramWidget);

    layout->addWidget(m_gradingLevelsPanel);
    layout->addWidget(m_gradingCurvesPanel);
    m_gradingLevelsPanel->setVisible(true);
    m_gradingCurvesPanel->setVisible(true);

    m_gradingAutoScrollCheckBox = new QCheckBox(QStringLiteral("Auto Scroll"), content);
    m_gradingFollowCurrentCheckBox = new QCheckBox(QStringLiteral("Follow Current"), content);
    m_gradingPreviewCheckBox = new QCheckBox(QStringLiteral("Preview"), content);
    m_gradingPreviewCheckBox->setObjectName(QStringLiteral("grading.preview"));
    m_gradingAutoScrollCheckBox->setChecked(true);
    m_gradingFollowCurrentCheckBox->setChecked(true);
    m_gradingPreviewCheckBox->setChecked(true);
    m_gradingKeyAtPlayheadButton = new QPushButton(QStringLiteral("Key At Playhead"), content);
    m_gradingResetButton = new QPushButton(QStringLiteral("Reset Grading"), content);
    m_gradingResetButton->setObjectName(QStringLiteral("grading.reset"));
    m_gradingResetButton->setToolTip(QStringLiteral("Reset the current grading values and curves to neutral."));
    m_gradingNormalizeCurvesButton = new QPushButton(QStringLiteral("Distribute Brightness"), content);
    m_gradingNormalizeCurvesButton->setToolTip(QStringLiteral(
        "Fold the Brightness transfer into Red, Green, and Blue, then reset Brightness to passthrough. "
        "Each resulting channel is limited to 12 definitive points."));
    m_gradingAutoOpposeButton = new QPushButton(QStringLiteral("Auto Oppose"), content);
    m_gradingAutoOpposeButton->setToolTip(QStringLiteral(
        "Analyze the selected clip and add grading keyframes that oppose major exposure/color shifts."));
    m_gradingFadeInButton = new QPushButton(QStringLiteral("Fade In From Playhead"), content);
    m_gradingFadeOutButton = new QPushButton(QStringLiteral("Fade Out From Playhead"), content);
    
    m_gradingFadeDurationSpin = new QDoubleSpinBox(content);
    m_gradingFadeDurationSpin->setRange(0.1, 60.0);
    m_gradingFadeDurationSpin->setValue(1.0);
    m_gradingFadeDurationSpin->setSuffix(QStringLiteral(" s"));
    m_gradingFadeDurationSpin->setDecimals(1);
    m_gradingFadeDurationSpin->setSingleStep(0.5);
    m_gradingFadeDurationSpin->setToolTip(QStringLiteral("Fade duration in seconds"));

    m_gradingKeyframeTable = new QTableWidget(content);
    m_gradingKeyframeTable->setObjectName(QStringLiteral("grading.keyframes"));
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

    layout->addWidget(m_gradingAutoScrollCheckBox);
    layout->addWidget(m_gradingFollowCurrentCheckBox);
    layout->addWidget(m_gradingPreviewCheckBox);
    layout->addWidget(m_gradingKeyAtPlayheadButton);
    layout->addWidget(m_gradingResetButton);
    layout->addWidget(m_gradingNormalizeCurvesButton);
    layout->addWidget(m_gradingAutoOpposeButton);
    layout->addWidget(m_gradingKeyframeTable);

    scrollArea->setWidget(content);
    pageLayout->addWidget(scrollArea, 1);
    return page;
}

QWidget *InspectorPane::buildOpacityTab()
{
    auto *page = new QWidget;
    auto *layout = createTabLayout(page);
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
    auto *pageLayout = new QVBoxLayout(page);
    pageLayout->setContentsMargins(0, 0, 0, 0);
    auto *scrollArea = new QScrollArea(page);
    scrollArea->setWidgetResizable(true);
    scrollArea->setFrameShape(QFrame::NoFrame);
    scrollArea->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    pageLayout->addWidget(scrollArea);
    auto *content = new QWidget(scrollArea);
    auto *layout = createTabLayout(content);
    scrollArea->setWidget(content);
    layout->addWidget(createTabHeading(QStringLiteral("Effects"), page));

    m_effectsPathLabel = new QLabel(QStringLiteral("No visual clip selected"), page);
    m_effectsPathLabel->setWordWrap(true);
    layout->addWidget(m_effectsPathLabel);

    auto effectSelectionSection =
        createDisclosureSection(page, QStringLiteral("Effect Selection"), true);
    auto* edgeForm = new QFormLayout;
    edgeForm->setContentsMargins(0, 0, 0, 0);
    edgeForm->setSpacing(6);
    m_edgeFillEffectCombo = new QComboBox(page);
    m_edgeFillEffectCombo->addItem(
        QStringLiteral("None"),
        backgroundFillEffectToString(BackgroundFillEffect::None));
    m_edgeFillEffectCombo->addItem(
        QStringLiteral("Edge Stretch"),
        backgroundFillEffectToString(BackgroundFillEffect::EdgeStretch));
    m_edgeFillEffectCombo->addItem(
        QStringLiteral("Progressive Edge Stretch"),
        backgroundFillEffectToString(BackgroundFillEffect::ProgressiveEdgeStretch));
    m_edgeFillEffectCombo->addItem(
        QStringLiteral("Progressive Bidirectional Edge Stretch"),
        backgroundFillEffectToString(
            BackgroundFillEffect::ProgressiveBidirectionalEdgeStretch));
    m_edgeFillEffectCombo->addItem(
        QStringLiteral("Tile"),
        backgroundFillEffectToString(BackgroundFillEffect::Tile));
    m_edgeFillEffectCombo->addItem(
        QStringLiteral("Mirror"),
        backgroundFillEffectToString(BackgroundFillEffect::Mirror));
    m_edgeFillEffectCombo->addItem(
        QStringLiteral("Blur Cover"),
        backgroundFillEffectToString(BackgroundFillEffect::BlurCover));
    m_edgeFillEffectCombo->setToolTip(
        QStringLiteral("Choose how this clip fills the surrounding canvas."));
    edgeForm->addRow(QStringLiteral("Background fill"), m_edgeFillEffectCombo);
    m_edgeFillPixelsSpin = new QSpinBox(page);
    m_edgeFillPixelsSpin->setRange(1, 512);
    m_edgeFillPixelsSpin->setValue(1);
    m_edgeFillPixelsSpin->setSuffix(QStringLiteral(" px"));
    edgeForm->addRow(QStringLiteral("Edge width"), m_edgeFillPixelsSpin);
    m_edgeFillPowerSpin = new QDoubleSpinBox(page);
    m_edgeFillPowerSpin->setRange(0.25, 8.0);
    m_edgeFillPowerSpin->setDecimals(2);
    m_edgeFillPowerSpin->setSingleStep(0.25);
    m_edgeFillPowerSpin->setValue(2.0);
    edgeForm->addRow(QStringLiteral("Curve power"), m_edgeFillPowerSpin);
    auto makeEdgePercentSpin = [page](double minimum, double maximum, double value) {
        auto* spin = new QDoubleSpinBox(page);
        spin->setRange(minimum, maximum);
        spin->setDecimals(0);
        spin->setSingleStep(5.0);
        spin->setSuffix(QStringLiteral("%"));
        spin->setValue(value);
        return spin;
    };
    m_edgeFillOpacitySpin = makeEdgePercentSpin(0.0, 100.0, 100.0);
    edgeForm->addRow(QStringLiteral("Opacity"), m_edgeFillOpacitySpin);
    m_edgeFillBrightnessSpin = makeEdgePercentSpin(-100.0, 100.0, 0.0);
    edgeForm->addRow(QStringLiteral("Brightness"), m_edgeFillBrightnessSpin);
    m_edgeFillSaturationSpin = makeEdgePercentSpin(0.0, 300.0, 100.0);
    edgeForm->addRow(QStringLiteral("Saturation"), m_edgeFillSaturationSpin);

    auto *presetForm = new QFormLayout();
    presetForm->setContentsMargins(0, 0, 0, 0);
    presetForm->setSpacing(6);
    m_effectPresetCategoryCombo = new QComboBox(page);
    QStringList presetGroups;
    for (const EffectPresetUiOption& option : effectPresetUiOptions()) {
        if (!presetGroups.contains(option.group)) {
            presetGroups.push_back(option.group);
        }
    }
    for (const QString& group : presetGroups) {
        m_effectPresetCategoryCombo->addItem(group, group);
    }
    m_effectPresetCategoryCombo->setToolTip(
        QStringLiteral("Filter synthesis presets by professional effect family."));
    presetForm->addRow(QStringLiteral("Effect family"), m_effectPresetCategoryCombo);

    m_effectPresetCombo = new QComboBox(page);
    for (const EffectPresetUiOption& option : effectPresetUiOptions()) {
        m_effectPresetCombo->addItem(option.label, static_cast<int>(option.preset));
        m_effectPresetCombo->setItemData(m_effectPresetCombo->count() - 1,
                                         option.group,
                                         Qt::ToolTipRole);
    }
    m_effectPresetPreviousButton = new QPushButton(QStringLiteral("-"), page);
    m_effectPresetPreviousButton->setToolTip(QStringLiteral("Previous synthesis preset"));
    m_effectPresetPreviousButton->setFixedWidth(32);
    m_effectPresetNextButton = new QPushButton(QStringLiteral("+"), page);
    m_effectPresetNextButton->setToolTip(QStringLiteral("Next synthesis preset"));
    m_effectPresetNextButton->setFixedWidth(32);
    auto* presetRow = new QWidget(page);
    auto* presetRowLayout = new QHBoxLayout(presetRow);
    presetRowLayout->setContentsMargins(0, 0, 0, 0);
    presetRowLayout->setSpacing(6);
    presetRowLayout->addWidget(m_effectPresetPreviousButton);
    presetRowLayout->addWidget(m_effectPresetCombo, 1);
    presetRowLayout->addWidget(m_effectPresetNextButton);
    presetForm->addRow(QStringLiteral("Synthesis preset"), presetRow);
    effectSelectionSection.body->addLayout(presetForm);
    effectSelectionSection.body->addLayout(edgeForm);
    layout->addWidget(effectSelectionSection.container);

    auto presetSpecificSection =
        createDisclosureSection(page, QStringLiteral("Preset-Specific Settings"), true);
    auto *presetSpecificForm = new QFormLayout();
    presetSpecificForm->setContentsMargins(0, 0, 0, 0);
    presetSpecificForm->setSpacing(6);

    m_effectPresetSpecificHelpLabel = new QLabel(
        QStringLiteral("Choose a synthesis effect to show the controls that are meaningful for that preset."),
        page);
    m_effectPresetSpecificHelpLabel->setWordWrap(true);
    m_effectPresetSpecificHelpLabel->setStyleSheet(
        QStringLiteral("QLabel { color: #8fa0b5; font-size: 11px; }"));
    presetSpecificForm->addRow(QStringLiteral("Guidance"), m_effectPresetSpecificHelpLabel);

    m_effectRowsSpin = new QSpinBox(page);
    m_effectRowsSpin->setRange(1, 512);
    m_effectRowsSpin->setValue(32);
    m_effectRowsSpin->setToolTip(QStringLiteral("Rows, copies, repeat steps, or edge pixels for progressive edge stretch."));
    presetSpecificForm->addRow(QStringLiteral("Copies"), m_effectRowsSpin);

    m_effectSpeedSpin = new QDoubleSpinBox(page);
    m_effectSpeedSpin->setRange(-8.0, 8.0);
    m_effectSpeedSpin->setDecimals(2);
    m_effectSpeedSpin->setSingleStep(0.25);
    m_effectSpeedSpin->setValue(1.0);
    presetSpecificForm->addRow(QStringLiteral("Speed"), m_effectSpeedSpin);

    m_effectScaleSpin = new QDoubleSpinBox(page);
    m_effectScaleSpin->setRange(0.1, 8.0);
    m_effectScaleSpin->setDecimals(2);
    m_effectScaleSpin->setSingleStep(0.1);
    m_effectScaleSpin->setValue(1.0);
    presetSpecificForm->addRow(QStringLiteral("Scale"), m_effectScaleSpin);

    m_effectAlternateDirectionCheck = new QCheckBox(QStringLiteral("Alternate direction"), page);
    m_effectAlternateDirectionCheck->setChecked(true);
    presetSpecificForm->addRow(QString(), m_effectAlternateDirectionCheck);

    m_effectSpeechSyncCheck = new QCheckBox(QStringLiteral("Synchronize motion with Speech Filter"), page);
    m_effectSpeechSyncCheck->setToolTip(
        QStringLiteral("Drive moving effect patterns from speech-filter timing so skipped gaps do not create visible jumps."));
    presetSpecificForm->addRow(QString(), m_effectSpeechSyncCheck);

    m_differenceReferenceFramesSpin = new QSpinBox(page);
    m_differenceReferenceFramesSpin->setRange(1, 300);
    m_differenceReferenceFramesSpin->setValue(1);
    m_differenceReferenceFramesSpin->setSuffix(QStringLiteral(" frames"));
    presetSpecificForm->addRow(QStringLiteral("Difference reference"), m_differenceReferenceFramesSpin);
    auto makeUnitEffectSpin = [page](double value) {
        auto* spin = new QDoubleSpinBox(page);
        spin->setRange(0.0, 1.0);
        spin->setDecimals(3);
        spin->setSingleStep(0.01);
        spin->setValue(value);
        spin->setKeyboardTracking(false);
        return spin;
    };
    m_differenceThresholdSpin = makeUnitEffectSpin(0.10);
    presetSpecificForm->addRow(QStringLiteral("Difference threshold"), m_differenceThresholdSpin);
    m_differenceSoftnessSpin = makeUnitEffectSpin(0.05);
    presetSpecificForm->addRow(QStringLiteral("Difference softness"), m_differenceSoftnessSpin);
    m_temporalEchoCountSpin = new QSpinBox(page);
    m_temporalEchoCountSpin->setRange(1, 12);
    m_temporalEchoCountSpin->setValue(4);
    presetSpecificForm->addRow(QStringLiteral("Echo frames"), m_temporalEchoCountSpin);
    m_temporalEchoSpacingSpin = new QSpinBox(page);
    m_temporalEchoSpacingSpin->setRange(1, 120);
    m_temporalEchoSpacingSpin->setValue(2);
    m_temporalEchoSpacingSpin->setSuffix(QStringLiteral(" frames"));
    presetSpecificForm->addRow(QStringLiteral("Echo spacing"), m_temporalEchoSpacingSpin);
    m_temporalEchoDecaySpin = makeUnitEffectSpin(0.65);
    presetSpecificForm->addRow(QStringLiteral("Echo decay"), m_temporalEchoDecaySpin);

    m_tilingPatternCombo = new QComboBox(page);
    for (const TilingPatternUiOption& option : tilingPatternUiOptions()) {
        m_tilingPatternCombo->addItem(option.label, static_cast<int>(option.pattern));
    }
    presetSpecificForm->addRow(QStringLiteral("Pattern"), m_tilingPatternCombo);

    m_tilingSpacingSpin = new QDoubleSpinBox(page);
    m_tilingSpacingSpin->setRange(0.1, 8.0);
    m_tilingSpacingSpin->setDecimals(2);
    m_tilingSpacingSpin->setSingleStep(0.1);
    m_tilingSpacingSpin->setValue(1.0);
    m_tilingSpacingSpin->setToolTip(QStringLiteral("Spacing multiplier between repeated source images."));
    presetSpecificForm->addRow(QStringLiteral("Spacing"), m_tilingSpacingSpin);

    m_tilingWrapCheck = new QCheckBox(QStringLiteral("Wrap across bounds"), page);
    m_tilingWrapCheck->setChecked(true);
    presetSpecificForm->addRow(QString(), m_tilingWrapCheck);

    m_maskBoundingBoxSection = new QGroupBox(QStringLiteral("Mask Bounding Box"), page);
    auto* maskBoundingBoxForm = new QFormLayout(m_maskBoundingBoxSection);
    maskBoundingBoxForm->setContentsMargins(8, 8, 8, 8);
    auto* maskBoundingBoxHelp = new QLabel(
        QStringLiteral("Shared mask bounding-box controls for mask clips and mask-aware effects."),
        m_maskBoundingBoxSection);
    maskBoundingBoxHelp->setWordWrap(true);
    maskBoundingBoxHelp->setStyleSheet(QStringLiteral("color: palette(mid);"));
    maskBoundingBoxForm->addRow(maskBoundingBoxHelp);

    m_tilingUseMaskBoundsCheck = new QCheckBox(
        QStringLiteral("Use available mask bounding box"), m_maskBoundingBoxSection);
    m_tilingUseMaskBoundsCheck->setToolTip(QStringLiteral(
        "Use the clip mask's available foreground bounds for source tiling "
        "and mask-aware generated effects instead of the full clip bounds."));
    maskBoundingBoxForm->addRow(QString(), m_tilingUseMaskBoundsCheck);

    m_tilingMaskIslandSigmaSpin = new QDoubleSpinBox(m_maskBoundingBoxSection);
    m_tilingMaskIslandSigmaSpin->setRange(0.0, 100.0);
    m_tilingMaskIslandSigmaSpin->setDecimals(2);
    m_tilingMaskIslandSigmaSpin->setSingleStep(1.0);
    m_tilingMaskIslandSigmaSpin->setValue(0.0);
    m_tilingMaskIslandSigmaSpin->setSuffix(QStringLiteral("%"));
    m_tilingMaskIslandSigmaSpin->setToolTip(QStringLiteral(
        "Allow this percentage of foreground mask pixels to remain outside "
        "the calculated available mask bounding box. Use 0% to include every "
        "masked pixel; increase it to ignore small outlying islands."));
    maskBoundingBoxForm->addRow(QStringLiteral("Outside pixels"), m_tilingMaskIslandSigmaSpin);

    m_maskBoundingBoxPreviewCheck = new QCheckBox(
        QStringLiteral("Preview bounding box"), m_maskBoundingBoxSection);
    m_maskBoundingBoxPreviewCheck->setToolTip(QStringLiteral(
        "Draw the calculated mask bounding box in the preview."));
    maskBoundingBoxForm->addRow(QString(), m_maskBoundingBoxPreviewCheck);
    presetSpecificForm->addRow(m_maskBoundingBoxSection);

    m_directionalEchoControlsWidget = new QWidget(page);
    auto *directionalEchoLayout = new QGridLayout(m_directionalEchoControlsWidget);
    directionalEchoLayout->setContentsMargins(0, 0, 0, 0);
    directionalEchoLayout->setHorizontalSpacing(14);
    directionalEchoLayout->setVerticalSpacing(5);
    auto *directionalEchoIntro = new QLabel(
        QStringLiteral("Instanced frame repeats for masked or full-frame footage. "
                       "The center instance remains the source; outer instances "
                       "spread symmetrically and receive opposing hue offsets."),
        page);
    directionalEchoIntro->setWordWrap(true);
    directionalEchoIntro->setStyleSheet(QStringLiteral("color: palette(mid);"));
    directionalEchoLayout->addWidget(directionalEchoIntro, 0, 0, 1, 3);
    auto makeEchoDial = [page, directionalEchoLayout](const QString& name,
                                                      const QString& detail,
                                                      QDial*& dial,
                                                      QLabel*& valueLabel,
                                                      int column,
                                                      int minimum,
                                                      int maximum,
                                                      int value) {
        auto *title = new QLabel(name, page);
        title->setAlignment(Qt::AlignCenter);
        title->setStyleSheet(QStringLiteral("font-weight: 600;"));
        dial = new QDial(page);
        dial->setRange(minimum, maximum);
        dial->setValue(value);
        dial->setNotchesVisible(true);
        dial->setWrapping(minimum == 0 && maximum >= 359);
        dial->setFixedSize(72, 72);
        valueLabel = new QLabel(page);
        valueLabel->setAlignment(Qt::AlignCenter);
        auto *detailLabel = new QLabel(detail, page);
        detailLabel->setAlignment(Qt::AlignCenter);
        detailLabel->setWordWrap(true);
        detailLabel->setStyleSheet(QStringLiteral("color: palette(mid); font-size: 11px;"));
        directionalEchoLayout->addWidget(title, 1, column);
        directionalEchoLayout->addWidget(dial, 2, column, Qt::AlignCenter);
        directionalEchoLayout->addWidget(valueLabel, 3, column);
        directionalEchoLayout->addWidget(detailLabel, 4, column);
    };
    makeEchoDial(QStringLiteral("Direction"),
                 QStringLiteral("Axis of the instanced trail"),
                 m_directionalEchoDirectionDial,
                 m_directionalEchoDirectionValueLabel,
                 0, 0, 359, 0);
    makeEchoDial(QStringLiteral("Spread"),
                 QStringLiteral("Distance between copies"),
                 m_directionalEchoSpreadDial,
                 m_directionalEchoSpreadValueLabel,
                 1, 0, 100, 25);
    makeEchoDial(QStringLiteral("Hue balance"),
                 QStringLiteral("Opposing color offsets"),
                 m_directionalEchoHueDial,
                 m_directionalEchoHueValueLabel,
                 2, 0, 100, 25);
    m_directionalEchoSummaryLabel = new QLabel(page);
    m_directionalEchoSummaryLabel->setWordWrap(true);
    m_directionalEchoSummaryLabel->setStyleSheet(QStringLiteral(
        "QLabel {"
        "  border: 1px solid palette(mid);"
        "  border-radius: 6px;"
        "  padding: 6px;"
        "  background: palette(alternate-base);"
        "}"));
    directionalEchoLayout->addWidget(m_directionalEchoSummaryLabel, 5, 0, 1, 3);
    m_directionalEchoControlsWidget->setToolTip(QStringLiteral(
        "Directional Frame Echo draws instanced copies around the source. "
        "Direction sets the echo axis, Spread sets offset distance, and Hue "
        "Balance creates symmetric color offsets so the stack resolves back "
        "toward the original image when viewed small."));
    presetSpecificForm->addRow(QStringLiteral("Frame echo"), m_directionalEchoControlsWidget);

    m_stepRepeatFillControlsWidget = new QWidget(page);
    auto *stepRepeatFillLayout = new QGridLayout(m_stepRepeatFillControlsWidget);
    stepRepeatFillLayout->setContentsMargins(0, 0, 0, 0);
    stepRepeatFillLayout->setHorizontalSpacing(14);
    stepRepeatFillLayout->setVerticalSpacing(5);
    auto *stepRepeatFillIntro = new QLabel(
        QStringLiteral("Fills the frame with repeated source tiles, then recolors each "
                       "tile against a larger invisible version of the same source so "
                       "the field resolves toward the original image when viewed small."),
        page);
    stepRepeatFillIntro->setWordWrap(true);
    stepRepeatFillIntro->setStyleSheet(QStringLiteral("color: palette(mid);"));
    stepRepeatFillLayout->addWidget(stepRepeatFillIntro, 0, 0, 1, 3);
    auto makeFillDial = [page, stepRepeatFillLayout](const QString& name,
                                                     const QString& detail,
                                                     QDial*& dial,
                                                     QLabel*& valueLabel,
                                                     int column,
                                                     int value) {
        auto *title = new QLabel(name, page);
        title->setAlignment(Qt::AlignCenter);
        title->setStyleSheet(QStringLiteral("font-weight: 600;"));
        dial = new QDial(page);
        dial->setRange(0, 100);
        dial->setValue(value);
        dial->setNotchesVisible(true);
        dial->setFixedSize(72, 72);
        valueLabel = new QLabel(page);
        valueLabel->setAlignment(Qt::AlignCenter);
        auto *detailLabel = new QLabel(detail, page);
        detailLabel->setAlignment(Qt::AlignCenter);
        detailLabel->setWordWrap(true);
        detailLabel->setStyleSheet(QStringLiteral("color: palette(mid); font-size: 11px;"));
        stepRepeatFillLayout->addWidget(title, 1, column);
        stepRepeatFillLayout->addWidget(dial, 2, column, Qt::AlignCenter);
        stepRepeatFillLayout->addWidget(valueLabel, 3, column);
        stepRepeatFillLayout->addWidget(detailLabel, 4, column);
    };
    makeFillDial(QStringLiteral("Guide scale"),
                 QStringLiteral("Size of invisible source guide"),
                 m_stepRepeatFillGuideScaleDial,
                 m_stepRepeatFillGuideScaleValueLabel,
                 0,
                 50);
    makeFillDial(QStringLiteral("Luma match"),
                 QStringLiteral("Brightness adaptation strength"),
                 m_stepRepeatFillLumaMatchDial,
                 m_stepRepeatFillLumaMatchValueLabel,
                 1,
                 75);
    makeFillDial(QStringLiteral("Hue match"),
                 QStringLiteral("Color adaptation strength"),
                 m_stepRepeatFillHueMatchDial,
                 m_stepRepeatFillHueMatchValueLabel,
                 2,
                 50);
    m_stepRepeatFillSummaryLabel = new QLabel(page);
    m_stepRepeatFillSummaryLabel->setWordWrap(true);
    m_stepRepeatFillSummaryLabel->setStyleSheet(QStringLiteral(
        "QLabel {"
        "  border: 1px solid palette(mid);"
        "  border-radius: 6px;"
        "  padding: 6px;"
        "  background: palette(alternate-base);"
        "}"));
    stepRepeatFillLayout->addWidget(m_stepRepeatFillSummaryLabel, 5, 0, 1, 3);
    m_stepRepeatFillControlsWidget->setToolTip(QStringLiteral(
        "Step Repeat Fill uses repeated source tiles as texture detail and a "
        "larger superimposed source sample as the color guide."));
    presetSpecificForm->addRow(QStringLiteral("Repeat fill"), m_stepRepeatFillControlsWidget);
    presetSpecificSection.body->addLayout(presetSpecificForm);
    layout->addWidget(presetSpecificSection.container);

    auto animationSection =
        createDisclosureSection(content, QStringLiteral("Effect Animation"), true);
    auto *animationForm = new QFormLayout;
    animationForm->setContentsMargins(0, 0, 0, 0);
    animationForm->setSpacing(6);
    m_effectEnabledCheck =
        new QCheckBox(QStringLiteral("Enabled before the first keyframe"), page);
    m_effectEnabledCheck->setChecked(true);
    animationForm->addRow(QStringLiteral("Base state"), m_effectEnabledCheck);
    auto *keyframeButtons = new QHBoxLayout;
    m_effectKeyframeOnButton =
        new QPushButton(QStringLiteral("Key On"), page);
    m_effectKeyframeOffButton =
        new QPushButton(QStringLiteral("Key Off"), page);
    m_effectParameterKeyframeButton =
        new QPushButton(QStringLiteral("Key Parameters"), page);
    m_effectKeyframeRemoveButton =
        new QPushButton(QStringLiteral("Remove Key"), page);
    keyframeButtons->addWidget(m_effectKeyframeOnButton);
    keyframeButtons->addWidget(m_effectKeyframeOffButton);
    keyframeButtons->addWidget(m_effectParameterKeyframeButton);
    keyframeButtons->addWidget(m_effectKeyframeRemoveButton);
    animationForm->addRow(QStringLiteral("At playhead"), keyframeButtons);
    m_effectKeyframesLabel =
        new QLabel(QStringLiteral("No enable keyframes"), page);
    m_effectKeyframesLabel->setWordWrap(true);
    animationForm->addRow(QStringLiteral("Keys"), m_effectKeyframesLabel);

    m_effectKeyframeTable = new QTableWidget(page);
    m_effectKeyframeTable->setColumnCount(8);
    m_effectKeyframeTable->setHorizontalHeaderLabels(
        {QStringLiteral("Frame"),
         QStringLiteral("Type"),
         QStringLiteral("State"),
         QStringLiteral("Copies"),
         QStringLiteral("Speed"),
         QStringLiteral("Scale"),
         QStringLiteral("Pattern"),
         QStringLiteral("Other")});
    m_effectKeyframeTable->setSelectionBehavior(QAbstractItemView::SelectRows);
    m_effectKeyframeTable->setSelectionMode(QAbstractItemView::ExtendedSelection);
    m_effectKeyframeTable->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_effectKeyframeTable->verticalHeader()->setVisible(false);
    m_effectKeyframeTable->horizontalHeader()->setStretchLastSection(true);
    m_effectKeyframeTable->horizontalHeader()->setSectionResizeMode(QHeaderView::ResizeToContents);
    m_effectKeyframeTable->horizontalHeader()->setSectionResizeMode(7, QHeaderView::Stretch);
    m_effectKeyframeTable->setMinimumHeight(140);
    animationForm->addRow(QStringLiteral("Table"), m_effectKeyframeTable);

    m_effectModulationModeCombo = new QComboBox(page);
    m_effectModulationModeCombo->addItem(QStringLiteral("None"), QStringLiteral("none"));
    m_effectModulationModeCombo->addItem(QStringLiteral("LFO"), QStringLiteral("lfo"));
    m_effectModulationModeCombo->addItem(
        QStringLiteral("Steady increase"), QStringLiteral("steady_increase"));
    animationForm->addRow(QStringLiteral("Dynamic"), m_effectModulationModeCombo);
    m_effectModulationTargetCombo = new QComboBox(page);
    m_effectModulationTargetCombo->addItem(
        QStringLiteral("Copies / radius"), QStringLiteral("rows"));
    m_effectModulationTargetCombo->addItem(
        QStringLiteral("Speed"), QStringLiteral("speed"));
    m_effectModulationTargetCombo->addItem(
        QStringLiteral("Amount / strength"), QStringLiteral("scale"));
    m_effectModulationTargetCombo->addItem(
        QStringLiteral("Spacing"), QStringLiteral("spacing"));
    animationForm->addRow(QStringLiteral("Target"), m_effectModulationTargetCombo);
    m_effectModulationAmountSpin = new QDoubleSpinBox(page);
    m_effectModulationAmountSpin->setRange(-512.0, 512.0);
    m_effectModulationAmountSpin->setDecimals(3);
    m_effectModulationAmountSpin->setSingleStep(0.1);
    m_effectModulationAmountSpin->setToolTip(
        QStringLiteral("LFO amplitude, or units added per second for steady increase."));
    animationForm->addRow(QStringLiteral("Amplitude / rate"), m_effectModulationAmountSpin);
    m_effectModulationRateSpin = new QDoubleSpinBox(page);
    m_effectModulationRateSpin->setRange(0.0, 20.0);
    m_effectModulationRateSpin->setDecimals(3);
    m_effectModulationRateSpin->setSingleStep(0.1);
    m_effectModulationRateSpin->setValue(1.0);
    m_effectModulationRateSpin->setSuffix(QStringLiteral(" Hz"));
    animationForm->addRow(QStringLiteral("LFO frequency"), m_effectModulationRateSpin);
    m_effectModulationPhaseSpin = new QDoubleSpinBox(page);
    m_effectModulationPhaseSpin->setRange(-360.0, 360.0);
    m_effectModulationPhaseSpin->setDecimals(1);
    m_effectModulationPhaseSpin->setSingleStep(15.0);
    m_effectModulationPhaseSpin->setSuffix(QStringLiteral("°"));
    animationForm->addRow(QStringLiteral("LFO phase"), m_effectModulationPhaseSpin);
    animationSection.body->addLayout(animationForm);
    layout->addWidget(animationSection.container);

    // Info label
    auto *infoLabel = new QLabel(QStringLiteral(
        "Image presets render as repeated Vulkan draws from the clip texture. "
        "For rotoscoped cutouts, enable the foreground layer on the masked clip and place effect images below it in the timeline."), page);
    infoLabel->setWordWrap(true);
    infoLabel->setStyleSheet(QStringLiteral("QLabel { color: #8fa0b5; font-size: 11px; }"));
    layout->addWidget(infoLabel);

    layout->addStretch(1);
    return page;
}

QWidget *InspectorPane::buildMasksTab()
{
    auto *page = new QWidget;
    auto *pageLayout = new QVBoxLayout(page);
    pageLayout->setContentsMargins(0, 0, 0, 0);
    pageLayout->setSpacing(0);

    auto *scrollArea = new QScrollArea(page);
    scrollArea->setWidgetResizable(true);
    scrollArea->setFrameShape(QFrame::NoFrame);
    scrollArea->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    pageLayout->addWidget(scrollArea);

    auto *content = new QWidget(scrollArea);
    auto *layout = createTabLayout(content);
    layout->addWidget(createTabHeading(QStringLiteral("Masks"), page));

    m_maskClipLabel = new QLabel(QStringLiteral("Select a video clip to edit its mask."), page);
    m_maskClipLabel->setWordWrap(true);
    m_maskClipLabel->setTextInteractionFlags(Qt::TextSelectableByMouse);
    layout->addWidget(m_maskClipLabel);

    auto *layerForm = new QFormLayout;
    m_maskZLevelSpin = new QSpinBox(page);
    m_maskZLevelSpin->setObjectName(QStringLiteral("masks.z_level"));
    m_maskZLevelSpin->setRange(-10000, 10000);
    m_maskZLevelSpin->setKeyboardTracking(false);
    m_maskZLevelSpin->setToolTip(QStringLiteral(
        "Explicit compositing order. Higher Z-levels draw in front; timeline nesting does not change this value."));
    layerForm->addRow(QStringLiteral("Z-Level"), m_maskZLevelSpin);
    layout->addLayout(layerForm);

    m_maskEnabledCheck = new QCheckBox(QStringLiteral("Enable mask processing"), page);
    layout->addWidget(m_maskEnabledCheck);

    auto *sourceForm = new QFormLayout;
    auto *sourceRow = new QHBoxLayout;
    m_maskFramesDirEdit = new QLineEdit(page);
    m_maskFramesDirEdit->setClearButtonEnabled(true);
    m_maskFramesDirEdit->setPlaceholderText(QStringLiteral("Mask or continuous-alpha frames directory"));
    m_maskBrowseButton = new QPushButton(QStringLiteral("Browse"), page);
    m_maskFramesDirEdit->setVisible(false);
    m_maskSidecarCombo = new QComboBox(page);
    m_maskSidecarCombo->setSizeAdjustPolicy(QComboBox::AdjustToMinimumContentsLengthWithIcon);
    m_maskSidecarCombo->setToolTip(
        QStringLiteral("Choose from mask sidecars discovered beside the selected media file."));
    sourceRow->addWidget(m_maskSidecarCombo, 1);
    sourceRow->addWidget(m_maskBrowseButton);
    sourceForm->addRow(QStringLiteral("Mask Sidecar"), sourceRow);
    layout->addLayout(sourceForm);
    m_maskNewPromptButton = new QPushButton(QStringLiteral("New SAM Prompt Mask…"), page);
    m_maskNewPromptButton->setObjectName(QStringLiteral("masks.new_prompt"));
    m_maskNewPromptButton->setToolTip(QStringLiteral(
        "Generate a separate SAM mask sidecar and optionally union it with the current mask."));
    layout->addWidget(m_maskNewPromptButton);

    auto* refinementForm = new QFormLayout;
    m_maskBiRefNetGuideRadiusSpin = new QSpinBox(page);
    m_maskBiRefNetGuideRadiusSpin->setObjectName(
        QStringLiteral("masks.birefnet_guide_radius"));
    m_maskBiRefNetGuideRadiusSpin->setRange(0, 512);
    m_maskBiRefNetGuideRadiusSpin->setValue(24);
    m_maskBiRefNetGuideRadiusSpin->setSuffix(QStringLiteral(" px"));
    m_maskBiRefNetGuideRadiusSpin->setToolTip(QStringLiteral(
        "How far beyond the binary SAM boundary BiRefNet may recover soft edge detail."));
    refinementForm->addRow(
        QStringLiteral("Refinement reach"), m_maskBiRefNetGuideRadiusSpin);
    layout->addLayout(refinementForm);
    m_maskBiRefNetRefineButton = new QPushButton(
        QStringLiteral("Refine SAM Edges with BiRefNet…"), page);
    m_maskBiRefNetRefineButton->setObjectName(
        QStringLiteral("masks.birefnet_refine"));
    m_maskBiRefNetRefineButton->setToolTip(QStringLiteral(
        "Preprocess the selected binary SAM sidecar into a separate continuous-alpha "
        "matte. The original SAM masks remain unchanged."));
    layout->addWidget(m_maskBiRefNetRefineButton);

    auto *fuzzyForm = new QFormLayout;
    m_maskFuzzySpatialReachSpin = new QSpinBox(page);
    m_maskFuzzySpatialReachSpin->setRange(0, 128);
    m_maskFuzzySpatialReachSpin->setValue(12);
    m_maskFuzzySpatialReachSpin->setSuffix(QStringLiteral(" px"));
    m_maskFuzzySpatialReachSpin->setToolTip(
        QStringLiteral("Maximum movement between the selected region in adjacent mask frames."));
    m_maskFuzzyTemporalReachSpin = new QSpinBox(page);
    m_maskFuzzyTemporalReachSpin->setRange(0, 10000);
    m_maskFuzzyTemporalReachSpin->setValue(120);
    m_maskFuzzyTemporalReachSpin->setSuffix(QStringLiteral(" frames"));
    m_maskFuzzyTemporalReachSpin->setToolTip(
        QStringLiteral("Maximum number of frames to follow in each direction from the clicked frame."));
    fuzzyForm->addRow(QStringLiteral("Spatial reach"), m_maskFuzzySpatialReachSpin);
    fuzzyForm->addRow(QStringLiteral("Temporal reach"), m_maskFuzzyTemporalReachSpin);
    layout->addLayout(fuzzyForm);
    m_maskFuzzyRemoveButton = new QPushButton(QStringLiteral("Remove Extra Region"), page);
    m_maskFuzzyRemoveButton->setCheckable(true);
    m_maskFuzzyRemoveButton->setObjectName(QStringLiteral("masks.fuzzy_remove"));
    m_maskFuzzyRemoveButton->setToolTip(
        QStringLiteral("Arm the tool, then click unwanted mask foreground in the preview. "
                       "The original sidecar is preserved."));
    layout->addWidget(m_maskFuzzyRemoveButton);
    m_maskFuzzyStatusLabel = new QLabel(page);
    m_maskFuzzyStatusLabel->setWordWrap(true);
    layout->addWidget(m_maskFuzzyStatusLabel);

    auto makePixelsSpin = [page](double maxValue, double step) {
        auto *spin = new QDoubleSpinBox(page);
        spin->setRange(0.0, maxValue);
        spin->setDecimals(1);
        spin->setSingleStep(step);
        spin->setSuffix(QStringLiteral(" px"));
        spin->setAccelerated(true);
        spin->setKeyboardTracking(false);
        spin->setMinimumWidth(96);
        return spin;
    };
    struct PixelControl {
        QDoubleSpinBox* spin = nullptr;
        QWidget* row = nullptr;
    };
    auto makePixelsSliderControl = [page, makePixelsSpin](double maxValue, double step, const QString& tooltip) {
        PixelControl control;
        auto *row = new QWidget(page);
        auto *rowLayout = new QHBoxLayout(row);
        rowLayout->setContentsMargins(0, 0, 0, 0);
        rowLayout->setSpacing(6);

        auto *slider = new QSlider(Qt::Horizontal, row);
        slider->setRange(0, qRound(maxValue * 10.0));
        slider->setSingleStep(qMax(1, qRound(step * 10.0)));
        slider->setPageStep(qMax(10, qRound(step * 50.0)));
        slider->setToolTip(tooltip);
        slider->setMinimumWidth(120);

        auto *spin = makePixelsSpin(maxValue, step);
        spin->setParent(row);
        spin->setToolTip(tooltip);

        QObject::connect(slider, &QSlider::valueChanged, spin, [spin](int value) {
            const double nextValue = static_cast<double>(value) / 10.0;
            if (!qFuzzyCompare(spin->value() + 1.0, nextValue + 1.0)) {
                spin->setValue(nextValue);
            }
        });
        QObject::connect(spin, qOverload<double>(&QDoubleSpinBox::valueChanged), slider, [slider](double value) {
            const int nextValue = qRound(value * 10.0);
            if (slider->value() != nextValue) {
                QSignalBlocker blocker(slider);
                slider->setValue(nextValue);
            }
        });

        rowLayout->addWidget(slider, 1);
        rowLayout->addWidget(spin);
        control.spin = spin;
        control.row = row;
        return control;
    };
    auto makeScalarSpin = [page](double minValue, double maxValue, double value, double step) {
        auto *spin = new QDoubleSpinBox(page);
        spin->setRange(minValue, maxValue);
        spin->setDecimals(2);
        spin->setSingleStep(step);
        spin->setValue(value);
        return spin;
    };

    auto *shapeForm = new QFormLayout;
    const PixelControl dilateControl = makePixelsSliderControl(
        512.0,
        1.0,
        QStringLiteral("Expand the mask edge outward in pixels."));
    const PixelControl erodeControl = makePixelsSliderControl(
        512.0,
        1.0,
        QStringLiteral("Contract the mask edge inward in pixels."));
    const PixelControl featherControl = makePixelsSliderControl(
        512.0,
        0.5,
        QStringLiteral("Soften the processed mask edge in pixels."));
    const PixelControl blurControl = makePixelsSliderControl(
        512.0,
        0.5,
        QStringLiteral("Blur the mask matte in pixels before compositing."));
    m_maskDilateSpin = dilateControl.spin;
    m_maskErodeSpin = erodeControl.spin;
    m_maskShapeFeatherSpin = featherControl.spin;
    m_maskBlurSpin = blurControl.spin;
    m_maskShapeFeatherFalloffCombo = new QComboBox(page);
    m_maskShapeFeatherFalloffCombo->addItem(QStringLiteral("Power"), 0);
    m_maskShapeFeatherFalloffCombo->addItem(QStringLiteral("Linear"), 1);
    m_maskShapeFeatherFalloffCombo->addItem(QStringLiteral("Smoothstep"), 2);
    m_maskShapeFeatherFalloffCombo->addItem(QStringLiteral("Smootherstep"), 3);
    m_maskShapeFeatherFalloffCombo->addItem(QStringLiteral("Cosine"), 4);
    m_maskShapeFeatherFalloffCombo->addItem(QStringLiteral("Gaussian"), 5);
    m_maskShapeFeatherFalloffCombo->setToolTip(
        QStringLiteral("Opacity falloff across the feathered edge. Smootherstep is recommended for moving masks; Gaussian gives the softest photographic blend."));
    m_maskShapeFeatherPowerSpin = makeScalarSpin(0.1, 5.0, 2.0, 0.1);
    m_maskShapeFeatherPowerSpin->setToolTip(
        QStringLiteral("Power-law exponent. 1.0 is linear; higher values keep more opacity near the subject edge."));
    m_maskEdgeGrayAmountSpin = makeScalarSpin(0.0, 1.0, 0.0, 0.05);
    m_maskEdgeGrayAmountSpin->setToolTip(QStringLiteral(
        "Desaturate the feathered alpha boundary on the GPU to suppress green-screen color fringes. 0 disables it."));
    m_maskEdgeGrayWidthSpin = makeScalarSpin(0.001, 2.0, 0.25, 0.01);
    m_maskEdgeGrayWidthSpin->setToolTip(QStringLiteral(
        "Alpha-band width around the mask edge that receives grayscale treatment. Higher values affect more of the soft edge."));
    m_maskEdgeGrayGammaSpin = makeScalarSpin(0.1, 8.0, 1.0, 0.1);
    m_maskEdgeGrayGammaSpin->setToolTip(QStringLiteral(
        "Shape the grayscale falloff. Higher values concentrate desaturation closer to the exact mask edge."));
    m_maskTemporalStabilizeCheck = new QCheckBox(
        QStringLiteral("Motion-tolerant temporal median"), page);
    m_maskTemporalStabilizeCheck->setObjectName(
        QStringLiteral("masks.temporal_stabilize"));
    m_maskTemporalStabilizeCheck->setToolTip(QStringLiteral(
        "Stabilize one-frame mask flicker on the GPU using the adjacent mask frames. "
        "The original sidecar remains unchanged."));
    m_maskTemporalStabilizeStrengthSpin = makeScalarSpin(0.0, 1.0, 0.75, 0.05);
    m_maskTemporalStabilizeStrengthSpin->setSuffix(QStringLiteral(" strength"));
    m_maskTemporalStabilizeStrengthSpin->setToolTip(QStringLiteral(
        "Blend between the current alpha and the stabilized temporal result."));
    m_maskTemporalStabilizeMotionRadiusSpin = new QSpinBox(page);
    m_maskTemporalStabilizeMotionRadiusSpin->setRange(0, 32);
    m_maskTemporalStabilizeMotionRadiusSpin->setValue(4);
    m_maskTemporalStabilizeMotionRadiusSpin->setSuffix(QStringLiteral(" px"));
    m_maskTemporalStabilizeMotionRadiusSpin->setToolTip(QStringLiteral(
        "Maximum adjacent-frame mask movement tolerated before treating a change as flicker."));
    m_maskInvertCheck = new QCheckBox(QStringLiteral("Invert"), page);
    m_maskShowOnlyCheck = new QCheckBox(QStringLiteral("Show mask only"), page);
    m_maskShowOnlyCheck->setToolTip(QStringLiteral("Preview/export the processed mask instead of the source clip."));
    m_maskOpacitySpin = makeScalarSpin(0.0, 1.0, 1.0, 0.05);
    shapeForm->addRow(QStringLiteral("Dilate"), dilateControl.row);
    shapeForm->addRow(QStringLiteral("Erode"), erodeControl.row);
    shapeForm->addRow(QStringLiteral("Feather"), featherControl.row);
    shapeForm->addRow(QStringLiteral("Falloff"), m_maskShapeFeatherFalloffCombo);
    shapeForm->addRow(QStringLiteral("Power"), m_maskShapeFeatherPowerSpin);
    shapeForm->addRow(QStringLiteral("Edge grayscale"), m_maskEdgeGrayAmountSpin);
    shapeForm->addRow(QStringLiteral("Edge gray width"), m_maskEdgeGrayWidthSpin);
    shapeForm->addRow(QStringLiteral("Edge gray gamma"), m_maskEdgeGrayGammaSpin);
    shapeForm->addRow(QStringLiteral("Blur"), blurControl.row);
    shapeForm->addRow(QStringLiteral("Temporal stabilize"), m_maskTemporalStabilizeCheck);
    shapeForm->addRow(QStringLiteral("Stabilize strength"), m_maskTemporalStabilizeStrengthSpin);
    shapeForm->addRow(QStringLiteral("Motion tolerance"), m_maskTemporalStabilizeMotionRadiusSpin);
    shapeForm->addRow(QStringLiteral("Invert"), m_maskInvertCheck);
    shapeForm->addRow(QStringLiteral("View"), m_maskShowOnlyCheck);
    shapeForm->addRow(QStringLiteral("Opacity"), m_maskOpacitySpin);
    layout->addLayout(shapeForm);

    auto *compositingForm = new QFormLayout;
    m_maskForegroundLayerCheck =
        new QCheckBox(QStringLiteral("Draw as foreground layer"), page);
    m_maskForegroundLayerCheck->setToolTip(
        QStringLiteral("Draw the masked subject again as a later Vulkan pass."));
    m_maskRepeatEnabledCheck =
        new QCheckBox(QStringLiteral("Repeat masked source"), page);
    m_maskRepeatEnabledCheck->setToolTip(
        QStringLiteral("Repeat source pixels through this processed mask."));
    auto makeRepeatDeltaSpin = [page]() {
        auto *spin = new QDoubleSpinBox(page);
        spin->setRange(-100000.0, 100000.0);
        spin->setDecimals(2);
        spin->setSingleStep(10.0);
        spin->setSuffix(QStringLiteral(" px"));
        spin->setKeyboardTracking(false);
        return spin;
    };
    m_maskRepeatDeltaXSpin = makeRepeatDeltaSpin();
    m_maskRepeatDeltaXSpin->setValue(160.0);
    m_maskRepeatDeltaXSpin->setToolTip(
        QStringLiteral("Screen-space X offset between masked repeats."));
    m_maskRepeatDeltaYSpin = makeRepeatDeltaSpin();
    m_maskRepeatDeltaYSpin->setToolTip(
        QStringLiteral("Screen-space Y offset between masked repeats."));
    compositingForm->addRow(QStringLiteral("Foreground"), m_maskForegroundLayerCheck);
    compositingForm->addRow(QStringLiteral("Repeat"), m_maskRepeatEnabledCheck);
    compositingForm->addRow(QStringLiteral("Repeat X"), m_maskRepeatDeltaXSpin);
    compositingForm->addRow(QStringLiteral("Repeat Y"), m_maskRepeatDeltaYSpin);
    layout->addLayout(compositingForm);

    auto *shadowForm = new QFormLayout;
    m_maskShadowEnabledCheck = new QCheckBox(QStringLiteral("Drop shadow"), page);
    m_maskShadowRadiusSpin = makePixelsSpin(200.0, 1.0);
    m_maskShadowRadiusSpin->setValue(12.0);
    m_maskShadowOffsetXSpin = makeScalarSpin(-500.0, 500.0, 0.0, 1.0);
    m_maskShadowOffsetXSpin->setSuffix(QStringLiteral(" px"));
    m_maskShadowOffsetYSpin = makeScalarSpin(-500.0, 500.0, 4.0, 1.0);
    m_maskShadowOffsetYSpin->setSuffix(QStringLiteral(" px"));
    m_maskShadowOpacitySpin = makeScalarSpin(0.0, 1.0, 0.45, 0.05);
    shadowForm->addRow(QStringLiteral("Shadow"), m_maskShadowEnabledCheck);
    shadowForm->addRow(QStringLiteral("Radius"), m_maskShadowRadiusSpin);
    shadowForm->addRow(QStringLiteral("Offset X"), m_maskShadowOffsetXSpin);
    shadowForm->addRow(QStringLiteral("Offset Y"), m_maskShadowOffsetYSpin);
    shadowForm->addRow(QStringLiteral("Opacity"), m_maskShadowOpacitySpin);
    layout->addLayout(shadowForm);

    layout->addStretch(1);
    scrollArea->setWidget(content);
    return page;
}

QWidget *InspectorPane::buildCorrectionsTab()
{
    auto *page = new QWidget;
    auto *layout = createTabLayout(page);
    layout->addWidget(createTabHeading(QStringLiteral("Corrections"), page));

    m_correctionsClipLabel = new QLabel(QStringLiteral("No clip selected"), page);
    m_correctionsClipLabel->setWordWrap(true);
    layout->addWidget(m_correctionsClipLabel);

    m_correctionsStatusLabel = new QLabel(QStringLiteral("Select a visual clip to add erase polygons."), page);
    m_correctionsStatusLabel->setWordWrap(true);
    layout->addWidget(m_correctionsStatusLabel);

    m_correctionsEnabledCheck = new QCheckBox(QStringLiteral("Enable Corrections"), page);
    m_correctionsEnabledCheck->setChecked(true);
    m_correctionsEnabledCheck->setToolTip(
        QStringLiteral("Apply correction polygons in GPU preview and render."));
    layout->addWidget(m_correctionsEnabledCheck);

    auto *polygonLabel = new QLabel(QStringLiteral("Polygon Ranges"), page);
    polygonLabel->setStyleSheet(QStringLiteral("QLabel { color: #8fa0b5; font-weight: 600; }"));
    layout->addWidget(polygonLabel);

    m_correctionsPolygonTable = new QTableWidget(0, 4, page);
    m_correctionsPolygonTable->setHorizontalHeaderLabels(
        {QStringLiteral("On"), QStringLiteral("Start"), QStringLiteral("End"), QStringLiteral("Points")});
    m_correctionsPolygonTable->verticalHeader()->setVisible(false);
    m_correctionsPolygonTable->setSelectionBehavior(QAbstractItemView::SelectRows);
    m_correctionsPolygonTable->setSelectionMode(QAbstractItemView::SingleSelection);
    m_correctionsPolygonTable->setAlternatingRowColors(true);
    m_correctionsPolygonTable->horizontalHeader()->setSectionResizeMode(0, QHeaderView::ResizeToContents);
    m_correctionsPolygonTable->horizontalHeader()->setSectionResizeMode(1, QHeaderView::ResizeToContents);
    m_correctionsPolygonTable->horizontalHeader()->setSectionResizeMode(2, QHeaderView::ResizeToContents);
    m_correctionsPolygonTable->horizontalHeader()->setSectionResizeMode(3, QHeaderView::Stretch);
    m_correctionsPolygonTable->setMinimumHeight(130);
    layout->addWidget(m_correctionsPolygonTable);

    auto *vertexLabel = new QLabel(QStringLiteral("Selected Polygon Vertices"), page);
    vertexLabel->setStyleSheet(QStringLiteral("QLabel { color: #8fa0b5; font-weight: 600; }"));
    layout->addWidget(vertexLabel);

    m_correctionsVertexTable = new QTableWidget(0, 2, page);
    m_correctionsVertexTable->setHorizontalHeaderLabels({QStringLiteral("X"), QStringLiteral("Y")});
    m_correctionsVertexTable->verticalHeader()->setVisible(false);
    m_correctionsVertexTable->setSelectionBehavior(QAbstractItemView::SelectRows);
    m_correctionsVertexTable->setSelectionMode(QAbstractItemView::SingleSelection);
    m_correctionsVertexTable->setAlternatingRowColors(true);
    m_correctionsVertexTable->horizontalHeader()->setSectionResizeMode(0, QHeaderView::Stretch);
    m_correctionsVertexTable->horizontalHeader()->setSectionResizeMode(1, QHeaderView::Stretch);
    m_correctionsVertexTable->setMinimumHeight(120);
    layout->addWidget(m_correctionsVertexTable);

    m_correctionsDrawModeCheck = new QCheckBox(QStringLiteral("Draw Polygons In Preview"), page);
    m_correctionsDrawModeCheck->setChecked(false);
    m_correctionsDrawModeCheck->setToolTip(
        QStringLiteral("Click 3 or more points on the clip in preview, then close polygon."));
    layout->addWidget(m_correctionsDrawModeCheck);

    m_correctionsDrawPolygonButton = new QPushButton(QStringLiteral("Draw Polygon"), page);
    m_correctionsDrawPolygonButton->setCheckable(true);
    m_correctionsDrawPolygonButton->setToolTip(
        QStringLiteral("Enter draw mode to draw polygons on the clip in preview."));
    layout->addWidget(m_correctionsDrawPolygonButton);

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
    auto *layout = createTabLayout(page);
    layout->addWidget(createTabHeading(QStringLiteral("Titles"), page));

    m_titlesInspectorClipLabel = new QLabel(QStringLiteral("No clip selected"));
    m_titlesInspectorClipLabel->setWordWrap(true);
    layout->addWidget(m_titlesInspectorClipLabel);

    m_titlesInspectorDetailsLabel = new QLabel;
    layout->addWidget(m_titlesInspectorDetailsLabel);

    auto typographySection = createDisclosureSection(page, QStringLiteral("Typography"), false);
    auto effectsSection = createDisclosureSection(page, QStringLiteral("Background & Effects"), false);
    auto animationSection = createDisclosureSection(page, QStringLiteral("Lifetime Animation"), false);
    auto actionsSection = createDisclosureSection(page, QStringLiteral("Title Actions"), false);

    // Text input with splitter for resizable height
    auto *textGroup = new QGroupBox(QStringLiteral("Title Text"));
    auto *textLayout = new QVBoxLayout(textGroup);
    m_titleTextEdit = new QPlainTextEdit;
    m_titleTextEdit->setPlaceholderText(QStringLiteral("Enter title text..."));
    textLayout->addWidget(m_titleTextEdit);
    layout->addWidget(textGroup);

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
    typographySection.body->addLayout(fontRow);

    // Style row
    auto *styleRow = new QHBoxLayout;
    m_titleBoldCheck = new QCheckBox(QStringLiteral("Bold"));
    m_titleBoldCheck->setChecked(true);
    styleRow->addWidget(m_titleBoldCheck);
    m_titleItalicCheck = new QCheckBox(QStringLiteral("Italic"));
    styleRow->addWidget(m_titleItalicCheck);
    m_titleColorButton = new QPushButton(QStringLiteral("Color"));
    m_titleColorButton->setToolTip(QStringLiteral("Title text color"));
    m_titleColorButton->setStyleSheet(QStringLiteral("QPushButton { background: #ffffffff; color: #000000; }"));
    styleRow->addWidget(m_titleColorButton);
    styleRow->addWidget(new QLabel(QStringLiteral("Opacity:")));
    m_titleOpacitySpin = new QDoubleSpinBox;
    m_titleOpacitySpin->setRange(0.0, 1.0);
    m_titleOpacitySpin->setDecimals(2);
    m_titleOpacitySpin->setSingleStep(0.05);
    m_titleOpacitySpin->setValue(1.0);
    styleRow->addWidget(m_titleOpacitySpin);
    typographySection.body->addLayout(styleRow);

    // Drop shadow row
    auto *shadowRow = new QHBoxLayout;
    m_titleShadowEnabledCheck = new QCheckBox(QStringLiteral("Shadow"));
    m_titleShadowEnabledCheck->setChecked(true);
    shadowRow->addWidget(m_titleShadowEnabledCheck);
    m_titleShadowColorButton = new QPushButton(QStringLiteral("Shadow Color"));
    m_titleShadowColorButton->setStyleSheet(QStringLiteral("QPushButton { background: #ff000000; color: #ffffff; }"));
    shadowRow->addWidget(m_titleShadowColorButton);
    shadowRow->addWidget(new QLabel(QStringLiteral("Opacity:")));
    m_titleShadowOpacitySpin = new QDoubleSpinBox;
    m_titleShadowOpacitySpin->setRange(0.0, 1.0);
    m_titleShadowOpacitySpin->setDecimals(2);
    m_titleShadowOpacitySpin->setSingleStep(0.05);
    m_titleShadowOpacitySpin->setValue(0.6);
    shadowRow->addWidget(m_titleShadowOpacitySpin);
    shadowRow->addWidget(new QLabel(QStringLiteral("DX:")));
    m_titleShadowOffsetXSpin = new QDoubleSpinBox;
    m_titleShadowOffsetXSpin->setRange(-200.0, 200.0);
    m_titleShadowOffsetXSpin->setDecimals(1);
    m_titleShadowOffsetXSpin->setSingleStep(0.5);
    m_titleShadowOffsetXSpin->setValue(2.0);
    shadowRow->addWidget(m_titleShadowOffsetXSpin);
    shadowRow->addWidget(new QLabel(QStringLiteral("DY:")));
    m_titleShadowOffsetYSpin = new QDoubleSpinBox;
    m_titleShadowOffsetYSpin->setRange(-200.0, 200.0);
    m_titleShadowOffsetYSpin->setDecimals(1);
    m_titleShadowOffsetYSpin->setSingleStep(0.5);
    m_titleShadowOffsetYSpin->setValue(2.0);
    shadowRow->addWidget(m_titleShadowOffsetYSpin);
    effectsSection.body->addLayout(shadowRow);

    // Text window row
    auto *windowRow = new QHBoxLayout;
    m_titleWindowEnabledCheck = new QCheckBox(QStringLiteral("Window"));
    windowRow->addWidget(m_titleWindowEnabledCheck);
    m_titleWindowColorButton = new QPushButton(QStringLiteral("Window Color"));
    m_titleWindowColorButton->setStyleSheet(
        QStringLiteral("QPushButton { background: #ff000000; color: #ffffff; }"));
    windowRow->addWidget(m_titleWindowColorButton);
    windowRow->addWidget(new QLabel(QStringLiteral("Opacity:")));
    m_titleWindowOpacitySpin = new QDoubleSpinBox;
    m_titleWindowOpacitySpin->setRange(0.0, 1.0);
    m_titleWindowOpacitySpin->setDecimals(2);
    m_titleWindowOpacitySpin->setSingleStep(0.05);
    m_titleWindowOpacitySpin->setValue(0.35);
    windowRow->addWidget(m_titleWindowOpacitySpin);
    windowRow->addWidget(new QLabel(QStringLiteral("Pad:")));
    m_titleWindowPaddingSpin = new QDoubleSpinBox;
    m_titleWindowPaddingSpin->setRange(0.0, 400.0);
    m_titleWindowPaddingSpin->setDecimals(1);
    m_titleWindowPaddingSpin->setSingleStep(1.0);
    m_titleWindowPaddingSpin->setValue(16.0);
    windowRow->addWidget(m_titleWindowPaddingSpin);
    effectsSection.body->addLayout(windowRow);

    // Window frame row
    auto *windowFrameRow = new QHBoxLayout;
    m_titleWindowFrameEnabledCheck = new QCheckBox(QStringLiteral("Frame"));
    windowFrameRow->addWidget(m_titleWindowFrameEnabledCheck);
    m_titleWindowFrameColorButton = new QPushButton(QStringLiteral("Frame Color"));
    m_titleWindowFrameColorButton->setStyleSheet(
        QStringLiteral("QPushButton { background: #ffffffff; color: #000000; }"));
    windowFrameRow->addWidget(m_titleWindowFrameColorButton);
    windowFrameRow->addWidget(new QLabel(QStringLiteral("Opacity:")));
    m_titleWindowFrameOpacitySpin = new QDoubleSpinBox;
    m_titleWindowFrameOpacitySpin->setRange(0.0, 1.0);
    m_titleWindowFrameOpacitySpin->setDecimals(2);
    m_titleWindowFrameOpacitySpin->setSingleStep(0.05);
    m_titleWindowFrameOpacitySpin->setValue(1.0);
    windowFrameRow->addWidget(m_titleWindowFrameOpacitySpin);
    windowFrameRow->addWidget(new QLabel(QStringLiteral("W:")));
    m_titleWindowFrameWidthSpin = new QDoubleSpinBox;
    m_titleWindowFrameWidthSpin->setRange(0.0, 120.0);
    m_titleWindowFrameWidthSpin->setDecimals(1);
    m_titleWindowFrameWidthSpin->setSingleStep(0.5);
    m_titleWindowFrameWidthSpin->setValue(2.0);
    windowFrameRow->addWidget(m_titleWindowFrameWidthSpin);
    windowFrameRow->addWidget(new QLabel(QStringLiteral("Gap:")));
    m_titleWindowFrameGapSpin = new QDoubleSpinBox;
    m_titleWindowFrameGapSpin->setRange(0.0, 200.0);
    m_titleWindowFrameGapSpin->setDecimals(1);
    m_titleWindowFrameGapSpin->setSingleStep(1.0);
    m_titleWindowFrameGapSpin->setValue(4.0);
    windowFrameRow->addWidget(m_titleWindowFrameGapSpin);
    effectsSection.body->addLayout(windowFrameRow);

    auto *extrudeRow = new QHBoxLayout;
    m_titleTextExtrudeModeCombo = new QComboBox;
    m_titleTextExtrudeModeCombo->addItem(QStringLiteral("No Extrusion"), 0);
    m_titleTextExtrudeModeCombo->addItem(QStringLiteral("Stacked Copies"), 1);
    m_titleTextExtrudeModeCombo->addItem(QStringLiteral("Eroded Solid"), 2);
    extrudeRow->addWidget(m_titleTextExtrudeModeCombo);
    m_titleTextExtrudeDepthSpin = new QDoubleSpinBox;
    m_titleTextExtrudeDepthSpin->setRange(0.02, 2.0);
    m_titleTextExtrudeDepthSpin->setValue(0.16);
    m_titleTextExtrudeDepthSpin->setPrefix(QStringLiteral("Depth "));
    extrudeRow->addWidget(m_titleTextExtrudeDepthSpin);
    m_titleTextExtrudeBevelSpin = new QDoubleSpinBox;
    m_titleTextExtrudeBevelSpin->setRange(0.0, 2.0);
    m_titleTextExtrudeBevelSpin->setValue(0.7);
    m_titleTextExtrudeBevelSpin->setPrefix(QStringLiteral("Bevel "));
    extrudeRow->addWidget(m_titleTextExtrudeBevelSpin);
    effectsSection.body->addLayout(extrudeRow);

    auto *animationRow = new QHBoxLayout;
    m_titleLifetimeAnimationCombo = new QComboBox;
    m_titleLifetimeAnimationCombo->addItem(QStringLiteral("None"), 0);
    m_titleLifetimeAnimationCombo->addItem(QStringLiteral("News fly-in from left"), 1);
    m_titleLifetimeAnimationCombo->addItem(QStringLiteral("News fly-in from right"), 2);
    m_titleLifetimeAnimationCombo->addItem(QStringLiteral("Sports lower third"), 3);
    m_titleLifetimeAnimationCombo->addItem(QStringLiteral("Sports scorebug"), 4);
    m_titleLifetimeAnimationCombo->addItem(QStringLiteral("Sports stat card"), 5);
    m_titleLifetimeAnimationCombo->addItem(QStringLiteral("Sports matchup banner"), 6);
    m_titleLifetimeAnimationCombo->addItem(QStringLiteral("Sports replay tag"), 7);
    animationRow->addWidget(m_titleLifetimeAnimationCombo, 1);
    m_titleLifetimeAnimationAmountSpin = new QDoubleSpinBox;
    m_titleLifetimeAnimationAmountSpin->setRange(0.05, 10.0);
    m_titleLifetimeAnimationAmountSpin->setDecimals(2);
    m_titleLifetimeAnimationAmountSpin->setSingleStep(0.05);
    m_titleLifetimeAnimationAmountSpin->setValue(0.35);
    m_titleLifetimeAnimationAmountSpin->setSuffix(QStringLiteral(" sec"));
    m_titleLifetimeAnimationAmountSpin->setToolTip(
        QStringLiteral("Time spent flying in and flying out."));
    animationRow->addWidget(m_titleLifetimeAnimationAmountSpin);
    animationSection.body->addLayout(animationRow);

    m_applyTitleLifetimeAnimationButton = new QPushButton(QStringLiteral("Apply Lifetime Effect"));
    m_applyTitleLifetimeAnimationButton->setToolTip(
        QStringLiteral("Apply a clip-owned title effect without changing individual title keyframes."));
    animationSection.body->addWidget(m_applyTitleLifetimeAnimationButton);

    // Buttons
    auto *buttonRow = new QHBoxLayout;
    m_addTitleKeyframeButton = new QPushButton(QStringLiteral("Add Title At Playhead"));
    buttonRow->addWidget(m_addTitleKeyframeButton);
    m_removeTitleKeyframeButton = new QPushButton(QStringLiteral("Remove Selected"));
    buttonRow->addWidget(m_removeTitleKeyframeButton);
    actionsSection.body->addLayout(buttonRow);

    // Auto-scroll
    m_titleAutoScrollCheck = new QCheckBox(QStringLiteral("Auto-scroll to playhead"));
    m_titleAutoScrollCheck->setChecked(true);
    actionsSection.body->addWidget(m_titleAutoScrollCheck);

    layout->addWidget(typographySection.container);
    layout->addWidget(effectsSection.container);
    layout->addWidget(animationSection.container);
    layout->addWidget(actionsSection.container);

    // Table
    m_titleKeyframeTable = new QTableWidget;
    m_titleKeyframeTable->setColumnCount(7);
    m_titleKeyframeTable->setHorizontalHeaderLabels(
        {QStringLiteral("Start"), QStringLiteral("End"), QStringLiteral("Frame"), 
         QStringLiteral("Text"), QStringLiteral("Size"), QStringLiteral("Opacity"),
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
    auto *layout = createTabLayout(page);
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
    auto *layout = createTabLayout(page);
    layout->addWidget(createTabHeading(QStringLiteral("Transform"), page));

    m_keyframesInspectorClipLabel = new QLabel(QStringLiteral("No visual clip selected"), page);
    m_keyframesInspectorDetailsLabel = new QLabel(QStringLiteral("Select a visual clip to inspect its keyframes."), page);
    m_keyframesInspectorDetailsLabel->setWordWrap(true);

    m_videoTranslationXSpin = new QDoubleSpinBox(page);
    m_videoTranslationYSpin = new QDoubleSpinBox(page);
    m_videoRotationSpin = new QDoubleSpinBox(page);
    m_videoScaleXSpin = new QDoubleSpinBox(page);
    m_videoScaleYSpin = new QDoubleSpinBox(page);
    m_videoInterpolationCombo = new QComboBox(page);
    m_mirrorHorizontalCheckBox = new QCheckBox(QStringLiteral("Mirror Horizontal"), page);
    m_mirrorVerticalCheckBox = new QCheckBox(QStringLiteral("Mirror Vertical"), page);
    m_lockVideoScaleCheckBox = new QCheckBox(QStringLiteral("Lock Scale"), page);
    m_sourceTransformLockCheckBox = new QCheckBox(QStringLiteral("Lock To Source Transform"), page);
    m_sourceTransformLockCheckBox->setToolTip(
        QStringLiteral("Use the linked source clip's transform for this child clip."));
    m_keyframeSpaceCheckBox = new QCheckBox(QStringLiteral("Clip-Relative Frames"), page);
    m_keyframeSkipAwareTimingCheckBox = new QCheckBox(QStringLiteral("Skip Aware Timing"), page);
    m_addVideoKeyframeButton = new QPushButton(QStringLiteral("Add Keyframe"), page);
    m_removeVideoKeyframeButton = new QPushButton(QStringLiteral("Remove Keyframe"), page);
    m_flipHorizontalButton = new QPushButton(QStringLiteral("Flip Horizontal"), page);

    m_videoInterpolationCombo->addItem(QStringLiteral("Step"));
    m_videoInterpolationCombo->addItem(QStringLiteral("Linear"));
    m_lockVideoScaleCheckBox->setChecked(false);
    m_sourceTransformLockCheckBox->setChecked(false);
    m_keyframeSpaceCheckBox->setChecked(true);
    m_keyframeSkipAwareTimingCheckBox->setChecked(true);

    for (QDoubleSpinBox *spin : {
             m_videoTranslationXSpin, m_videoTranslationYSpin, m_videoRotationSpin,
             m_videoScaleXSpin, m_videoScaleYSpin})
    {
        spin->setDecimals(3);
        spin->setRange(-100000.0, 100000.0);
    }
    m_videoScaleXSpin->setValue(1.0);
    m_videoScaleYSpin->setValue(1.0);

    m_keyframesAutoScrollCheckBox = new QCheckBox(QStringLiteral("Auto Scroll"), page);
    m_keyframesFollowCurrentCheckBox = new QCheckBox(QStringLiteral("Follow Current Keyframe"), page);
    m_keyframesAutoScrollCheckBox->setChecked(true);
    m_keyframesFollowCurrentCheckBox->setChecked(true);

    auto makeTransformForm = [] {
        auto* form = new QFormLayout;
        form->setSpacing(4);
        form->setRowWrapPolicy(QFormLayout::WrapAllRows);
        form->setFieldGrowthPolicy(QFormLayout::AllNonFixedFieldsGrow);
        return form;
    };

    auto clipSection = createDisclosureSection(page, QStringLiteral("Clip Selection"), false);
    clipSection.body->addWidget(m_keyframesInspectorClipLabel);
    clipSection.body->addWidget(m_keyframesInspectorDetailsLabel);

    auto valuesSection = createDisclosureSection(page, QStringLiteral("Transform Values"), false);
    auto* valuesForm = makeTransformForm();
    valuesForm->addRow(QStringLiteral("Translate X"), m_videoTranslationXSpin);
    valuesForm->addRow(QStringLiteral("Translate Y"), m_videoTranslationYSpin);
    valuesForm->addRow(QStringLiteral("Rotation"), m_videoRotationSpin);
    valuesForm->addRow(QStringLiteral("Scale X"), m_videoScaleXSpin);
    valuesForm->addRow(QStringLiteral("Scale Y"), m_videoScaleYSpin);
    valuesForm->addRow(QStringLiteral("Interpolation"), m_videoInterpolationCombo);
    valuesSection.body->addLayout(valuesForm);

    auto lockSection = createDisclosureSection(page, QStringLiteral("Scale & Source Locks"), false);
    lockSection.body->addWidget(m_lockVideoScaleCheckBox);
    lockSection.body->addWidget(m_sourceTransformLockCheckBox);

    auto timingSection = createDisclosureSection(page, QStringLiteral("Keyframe Timing"), false);
    timingSection.body->addWidget(m_keyframeSpaceCheckBox);
    timingSection.body->addWidget(m_keyframeSkipAwareTimingCheckBox);

    auto navigationSection = createDisclosureSection(page, QStringLiteral("Table Navigation"), false);
    navigationSection.body->addWidget(m_keyframesAutoScrollCheckBox);
    navigationSection.body->addWidget(m_keyframesFollowCurrentCheckBox);

    auto mirrorSection = createDisclosureSection(page, QStringLiteral("Mirror & Flip"), false);
    mirrorSection.body->addWidget(m_mirrorHorizontalCheckBox);
    mirrorSection.body->addWidget(m_mirrorVerticalCheckBox);
    mirrorSection.body->addWidget(m_flipHorizontalButton);

    auto actionSection = createDisclosureSection(page, QStringLiteral("Keyframe Actions"), false);
    auto *buttonRow = new QHBoxLayout;
    buttonRow->setContentsMargins(0, 0, 0, 0);
    buttonRow->setSpacing(4);
    buttonRow->addWidget(m_addVideoKeyframeButton);
    buttonRow->addWidget(m_removeVideoKeyframeButton);
    actionSection.body->addLayout(buttonRow);

    m_videoKeyframeTable = new QTableWidget(page);
    m_videoKeyframeTable->setColumnCount(9);
    m_videoKeyframeTable->setHorizontalHeaderLabels({QStringLiteral("Frame"),
                                                     QStringLiteral("X"),
                                                     QStringLiteral("Y"),
                                                     QStringLiteral("Rot"),
                                                     QStringLiteral("Scale X"),
                                                     QStringLiteral("Scale Y"),
                                                     QStringLiteral("Repeat X"),
                                                     QStringLiteral("Repeat Y"),
                                                     QStringLiteral("Interp")});
    m_videoKeyframeTable->setSelectionBehavior(QAbstractItemView::SelectRows);
    m_videoKeyframeTable->setSelectionMode(QAbstractItemView::ExtendedSelection);
    m_videoKeyframeTable->setEditTriggers(QAbstractItemView::DoubleClicked |
                                          QAbstractItemView::EditKeyPressed);
    m_videoKeyframeTable->verticalHeader()->setVisible(false);
    m_videoKeyframeTable->horizontalHeader()->setStretchLastSection(true);
    m_videoKeyframeTable->horizontalHeader()->setSectionResizeMode(QHeaderView::Stretch);

    layout->addWidget(clipSection.container);
    layout->addWidget(valuesSection.container);
    layout->addWidget(lockSection.container);
    layout->addWidget(timingSection.container);
    layout->addWidget(navigationSection.container);
    layout->addWidget(mirrorSection.container);
    layout->addWidget(actionSection.container);
    layout->addWidget(m_videoKeyframeTable, 1);

    return page;
}
