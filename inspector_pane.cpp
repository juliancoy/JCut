#include "inspector_pane.h"
#include "editor_shared.h"
#include "debug_controls.h"
#include "grading_histogram_widget.h"

#include <QCheckBox>
#include <QBrush>
#include <QColor>
#include <QComboBox>
#include <QDoubleSpinBox>
#include <QFontComboBox>
#include <QFormLayout>
#include <QFrame>
#include <QGroupBox>
#include <QHeaderView>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QPlainTextEdit>
#include <QScrollArea>
#include <QSplitter>
#include <QSpinBox>
#include <QTabBar>
#include <QTabWidget>
#include <QTableWidget>
#include <QPushButton>
#include <QToolButton>
#include <QSize>
#include <QStyle>
#include <QStylePainter>
#include <QStyleOptionTab>
#include <QMouseEvent>
#include <QVBoxLayout>

namespace {
class HorizontalTextTabBar final : public QTabBar {
public:
    using QTabBar::QTabBar;

    QSize tabSizeHint(int index) const override {
        const QSize base = QTabBar::tabSizeHint(index);
        const int tabWidth = qBound(120, m_columnWidth, 260);
        const int tabHeight = qBound(24, base.height() + 2, 36);
        return QSize(tabWidth, tabHeight);
    }

protected:
    void paintEvent(QPaintEvent* event) override {
        Q_UNUSED(event);
        QStylePainter painter(this);
        QStyleOptionTab option;
        for (int i = 0; i < count(); ++i) {
            initStyleOption(&option, i);
            painter.drawControl(QStyle::CE_TabBarTabShape, option);
            QStyleOptionTab labelOption(option);
            labelOption.shape = QTabBar::RoundedNorth;
            painter.drawControl(QStyle::CE_TabBarTabLabel, labelOption);
        }
    }

    void mousePressEvent(QMouseEvent* event) override {
        if (event && event->button() == Qt::LeftButton && isNearResizeEdge(event->position().toPoint())) {
            m_resizing = true;
            m_resizeStartGlobalX = static_cast<int>(event->globalPosition().x());
            m_resizeStartWidth = width();
            event->accept();
            return;
        }
        QTabBar::mousePressEvent(event);
    }

    void mouseMoveEvent(QMouseEvent* event) override {
        if (!event) {
            return;
        }
        if (m_resizing) {
            const int currentGlobalX = static_cast<int>(event->globalPosition().x());
            const int delta = m_resizeStartGlobalX - currentGlobalX;
            const int newWidth = qBound(120, m_resizeStartWidth + delta, 260);
            if (newWidth != m_columnWidth) {
                m_columnWidth = newWidth;
                updateGeometry();
                if (parentWidget()) {
                    parentWidget()->updateGeometry();
                    parentWidget()->update();
                }
            }
            event->accept();
            return;
        }

        const bool nearEdge = isNearResizeEdge(event->position().toPoint());
        setCursor(nearEdge ? Qt::SizeHorCursor : Qt::ArrowCursor);
        QTabBar::mouseMoveEvent(event);
    }

    void mouseReleaseEvent(QMouseEvent* event) override {
        if (m_resizing && event && event->button() == Qt::LeftButton) {
            m_resizing = false;
            setCursor(Qt::ArrowCursor);
            event->accept();
            return;
        }
        QTabBar::mouseReleaseEvent(event);
    }

    void leaveEvent(QEvent* event) override {
        if (!m_resizing) {
            unsetCursor();
        }
        QTabBar::leaveEvent(event);
    }

private:
    bool isNearResizeEdge(const QPoint& pos) const {
        return pos.x() >= 0 && pos.x() <= 8;
    }

    int m_columnWidth = 180;
    bool m_resizing = false;
    int m_resizeStartGlobalX = 0;
    int m_resizeStartWidth = 0;
};

class InspectorTabWidget final : public QTabWidget {
public:
    explicit InspectorTabWidget(QWidget* parent = nullptr)
        : QTabWidget(parent)
    {
        setTabBar(new HorizontalTextTabBar(this));
    }
};

QLabel *createTabHeading(const QString &text, QWidget *parent = nullptr) {
    auto *label = new QLabel(text, parent);
    label->setStyleSheet(QStringLiteral(
        "QLabel { font-size: 13px; font-weight: 700; color: #8fa0b5; "
        "padding: 2px 0 6px 0; }"));
    return label;
}

QVBoxLayout *createTabLayout(QWidget *page)
{
    auto *layout = new QVBoxLayout(page);
    layout->setContentsMargins(8, 8, 8, 8);
    layout->setSpacing(6);
    return layout;
}

struct DisclosureSection {
    QWidget* container = nullptr;
    QVBoxLayout* body = nullptr;
};

DisclosureSection createDisclosureSection(QWidget* parent,
                                          const QString& title,
                                          bool expanded = true)
{
    DisclosureSection section;
    auto* container = new QWidget(parent);
    auto* outer = new QVBoxLayout(container);
    outer->setContentsMargins(0, 0, 0, 0);
    outer->setSpacing(4);

    auto* toggle = new QToolButton(container);
    toggle->setCheckable(true);
    toggle->setChecked(expanded);
    toggle->setToolButtonStyle(Qt::ToolButtonTextOnly);
    toggle->setText(QStringLiteral("%1 %2").arg(expanded ? QStringLiteral("▼") : QStringLiteral("▶"), title));
    toggle->setStyleSheet(QStringLiteral(
        "QToolButton { color: #9fb3c8; font-weight: 700; border: none; text-align: left; padding: 2px 0; }"
        "QToolButton:hover { color: #d6dee8; }"));
    outer->addWidget(toggle);

    auto* content = new QWidget(container);
    content->setVisible(expanded);
    auto* body = new QVBoxLayout(content);
    body->setContentsMargins(14, 2, 0, 2);
    body->setSpacing(6);
    outer->addWidget(content);

    QObject::connect(toggle, &QToolButton::toggled, content, [toggle, title, content](bool checked) {
        toggle->setText(QStringLiteral("%1 %2").arg(checked ? QStringLiteral("▼") : QStringLiteral("▶"), title));
        content->setVisible(checked);
    });

    section.container = container;
    section.body = body;
    return section;
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

QWidget *InspectorPane::buildGradingTab()
{
    auto *page = new QWidget;
    auto *layout = createTabLayout(page);
    layout->addWidget(createTabHeading(QStringLiteral("Grade"), page));

    m_gradingPathLabel = new QLabel(QStringLiteral("No visual clip selected"), page);
    m_gradingPathLabel->setWordWrap(true);
    layout->addWidget(m_gradingPathLabel);

    m_gradingEditModeCombo = new QComboBox(page);
    m_gradingEditModeCombo->addItem(QStringLiteral("Levels"));
    m_gradingEditModeCombo->addItem(QStringLiteral("Curves"));
    m_gradingEditModeCombo->setVisible(false);

    auto *commonForm = new QFormLayout;
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
    commonForm->addRow(QStringLiteral("Saturation"), m_saturationSpin);
    layout->addLayout(commonForm);

    m_gradingLevelsPanel = new QWidget(page);
    auto *levelsLayout = new QFormLayout(m_gradingLevelsPanel);
    levelsLayout->setContentsMargins(0, 0, 0, 0);
    levelsLayout->addRow(QStringLiteral("Brightness"), m_brightnessSpin);
    levelsLayout->addRow(QStringLiteral("Contrast"), m_contrastSpin);

    // Shadows/Midtones/Highlights (Lift/Gamma/Gain)
    m_gradingCurvesPanel = new QWidget(page);
    auto *curvesLayout = new QVBoxLayout(m_gradingCurvesPanel);
    curvesLayout->setContentsMargins(0, 0, 0, 0);
    curvesLayout->setSpacing(6);

    auto *shadowsGroup = new QGroupBox(QStringLiteral("Shadows (Lift)"), m_gradingCurvesPanel);
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

    auto *midtonesGroup = new QGroupBox(QStringLiteral("Midtones (Gamma)"), m_gradingCurvesPanel);
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

    auto *highlightsGroup = new QGroupBox(QStringLiteral("Highlights (Gain)"), m_gradingCurvesPanel);
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

    auto *curveChannelLayout = new QHBoxLayout;
    curveChannelLayout->addWidget(new QLabel(QStringLiteral("Curve Channel"), m_gradingCurvesPanel));
    m_gradingCurveChannelTabs = new QTabWidget(m_gradingCurvesPanel);
    m_gradingCurveChannelTabs->addTab(new QWidget(m_gradingCurveChannelTabs), QStringLiteral("Red"));
    m_gradingCurveChannelTabs->addTab(new QWidget(m_gradingCurveChannelTabs), QStringLiteral("Green"));
    m_gradingCurveChannelTabs->addTab(new QWidget(m_gradingCurveChannelTabs), QStringLiteral("Blue"));
    m_gradingCurveChannelTabs->addTab(new QWidget(m_gradingCurveChannelTabs), QStringLiteral("Brightness"));
    m_gradingCurveChannelTabs->setDocumentMode(true);
    m_gradingCurveChannelTabs->setStyleSheet(QStringLiteral(
        "QTabWidget::pane { border: 0; }"
        "QTabBar::tab { background:#1a2028; color:#9fb0c2; padding:5px 10px; border:1px solid #2f3a46; border-bottom:0; }"
        "QTabBar::tab:selected { background:#223246; color:#dbe9f8; }"));
    curveChannelLayout->addWidget(m_gradingCurveChannelTabs, 1);
    m_gradingCurveChannelCombo = new QComboBox(m_gradingCurvesPanel);
    m_gradingCurveChannelCombo->addItem(QStringLiteral("Red"));
    m_gradingCurveChannelCombo->addItem(QStringLiteral("Green"));
    m_gradingCurveChannelCombo->addItem(QStringLiteral("Blue"));
    m_gradingCurveChannelCombo->addItem(QStringLiteral("Brightness"));
    m_gradingCurveChannelCombo->setVisible(false);
    curveChannelLayout->addWidget(m_gradingCurveChannelCombo);
    curveChannelLayout->addStretch();

    connect(m_gradingCurveChannelTabs, &QTabWidget::currentChanged, this, [this](int index) {
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
        new QCheckBox(QStringLiteral("Link Numbers + 3-Point Curve"), m_gradingCurvesPanel);
    m_gradingCurveThreePointLockCheckBox->setChecked(true);
    m_gradingCurveThreePointLockCheckBox->setToolTip(QStringLiteral(
        "When enabled, Shadows/Midtones/Highlights numbers and the 3-point curve stay linked "
        "and represent one grading control."));
    m_gradingCurveSmoothingCheckBox =
        new QCheckBox(QStringLiteral("Curve Smoothing"), m_gradingCurvesPanel);
    m_gradingCurveSmoothingCheckBox->setChecked(true);
    curveOptionsLayout->addWidget(m_gradingCurveThreePointLockCheckBox);
    curveOptionsLayout->addWidget(m_gradingCurveSmoothingCheckBox);
    curveOptionsLayout->addStretch();

    m_gradingHistogramWidget = new GradingHistogramWidget(m_gradingCurvesPanel);
    m_gradingHistogramWidget->setToolTip(QStringLiteral(
        "Current-frame histogram.\n"
        "Select a channel and drag curve points to adjust Shadows/Midtones/Highlights."));

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

    m_gradingAutoScrollCheckBox = new QCheckBox(QStringLiteral("Auto Scroll"), page);
    m_gradingFollowCurrentCheckBox = new QCheckBox(QStringLiteral("Follow Current Keyframe"), page);
    m_gradingPreviewCheckBox = new QCheckBox(QStringLiteral("Preview"), page);
    m_gradingAutoScrollCheckBox->setChecked(true);
    m_gradingFollowCurrentCheckBox->setChecked(true);
    m_gradingPreviewCheckBox->setChecked(true);
    m_gradingKeyAtPlayheadButton = new QPushButton(QStringLiteral("Key At Playhead"), page);
    m_gradingAutoOpposeButton = new QPushButton(QStringLiteral("Auto Oppose Grade Changes"), page);
    m_gradingAutoOpposeButton->setToolTip(QStringLiteral(
        "Analyze the selected clip and add grading keyframes that oppose major exposure/color shifts."));
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

    layout->addWidget(m_gradingAutoScrollCheckBox);
    layout->addWidget(m_gradingFollowCurrentCheckBox);
    layout->addWidget(m_gradingPreviewCheckBox);
    layout->addWidget(m_gradingKeyAtPlayheadButton);
    layout->addWidget(m_gradingAutoOpposeButton);
    layout->addWidget(m_gradingKeyframeTable, 1);
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
    auto *layout = createTabLayout(page);
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

    // Text input with splitter for resizable height
    auto *textGroup = new QGroupBox(QStringLiteral("Title Text"));
    auto *textLayout = new QVBoxLayout(textGroup);
    m_titleTextEdit = new QPlainTextEdit;
    m_titleTextEdit->setPlaceholderText(QStringLiteral("Enter title text..."));
    textLayout->addWidget(m_titleTextEdit);
    layout->addWidget(textGroup);

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
    layout->addLayout(styleRow);

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
    layout->addLayout(shadowRow);

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
    layout->addLayout(windowRow);

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
    layout->addLayout(windowFrameRow);

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
    m_titleKeyframeTable->setColumnCount(9);
    m_titleKeyframeTable->setHorizontalHeaderLabels(
        {QStringLiteral("Start"), QStringLiteral("End"), QStringLiteral("Frame"), 
         QStringLiteral("Text"), QStringLiteral("X"), QStringLiteral("Y"), 
         QStringLiteral("Size"), QStringLiteral("Opacity"), QStringLiteral("Interp")});
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
    versionButtonsLayout->addWidget(m_transcriptNewVersionButton);
    versionButtonsLayout->addWidget(m_transcriptDeleteVersionButton);

    // --- Separator ---
    auto *separator = new QFrame(settingsContainer);
    separator->setFrameShape(QFrame::HLine);
    separator->setFrameShadow(QFrame::Sunken);
    separator->setStyleSheet(QStringLiteral("color: #2a3a4a;"));

    // --- Overlay settings section ---
    auto *overlaySectionLabel = new QLabel(QStringLiteral("Overlay Settings"), settingsContainer);
    overlaySectionLabel->setStyleSheet(QStringLiteral("font-weight: 600; color: #8fa3b8; font-size: 12px; padding-top: 4px;"));

    auto *form = new QFormLayout;
    form->setSpacing(4);
    m_transcriptOverlayEnabledCheckBox = new QCheckBox(QStringLiteral("Enable Overlay"), settingsContainer);
    m_transcriptBackgroundVisibleCheckBox = new QCheckBox(QStringLiteral("Show Window"), settingsContainer);
    m_transcriptShadowEnabledCheckBox = new QCheckBox(QStringLiteral("Show Shadow"), settingsContainer);
    m_transcriptShowSpeakerTitleCheckBox = new QCheckBox(QStringLiteral("Show Speaker Title"), settingsContainer);
    m_transcriptMaxLinesSpin = new QSpinBox(settingsContainer);
    m_transcriptMaxCharsSpin = new QSpinBox(settingsContainer);
    m_transcriptFollowCurrentWordCheckBox = new QCheckBox(QStringLiteral("Follow Current Word"), settingsContainer);
    m_transcriptFollowCurrentWordCheckBox->setToolTip(
        QStringLiteral("Highlight and auto-scroll transcript rows during playback."));
    m_transcriptAutoScrollCheckBox = nullptr;
    m_transcriptOverlayXSpin = new QDoubleSpinBox(settingsContainer);
    m_transcriptOverlayYSpin = new QDoubleSpinBox(settingsContainer);
    m_transcriptCenterHorizontalButton = new QPushButton(QStringLiteral("Center H"), settingsContainer);
    m_transcriptCenterVerticalButton = new QPushButton(QStringLiteral("Center V"), settingsContainer);
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
        QStringLiteral("Filter transcript rows by word text. Clicking a matching row clears this filter."));
    m_transcriptSpeakerFilterCombo = new QComboBox(settingsContainer);
    m_transcriptSpeakerFilterCombo->addItem(QStringLiteral("All Speakers"));
    m_transcriptSpeakerFilterCombo->setToolTip(
        QStringLiteral("Filter transcript rows by speaker label from the transcript JSON."));
    m_transcriptShowExcludedLinesCheckBox =
        new QCheckBox(QStringLiteral("Show Lines Not In Active Cut"), settingsContainer);

    m_transcriptMaxLinesSpin->setRange(1, 20);
    m_transcriptMaxCharsSpin->setRange(1, 200);
    m_transcriptOverlayXSpin->setDecimals(3);
    m_transcriptOverlayYSpin->setDecimals(3);
    m_transcriptOverlayXSpin->setRange(-1.0, 1.0);
    m_transcriptOverlayYSpin->setRange(-1.0, 1.0);
    m_transcriptOverlayXSpin->setSingleStep(0.01);
    m_transcriptOverlayYSpin->setSingleStep(0.01);
    m_transcriptOverlayXSpin->setToolTip(
        QStringLiteral("Normalized horizontal offset from center (-1.0 left, +1.0 right)."));
    m_transcriptOverlayYSpin->setToolTip(
        QStringLiteral("Normalized vertical offset from center (-1.0 up, +1.0 down)."));
    m_transcriptOverlayWidthSpin->setRange(1, 10000);
    m_transcriptOverlayHeightSpin->setRange(1, 10000);
    m_transcriptFontSizeSpin->setRange(8, 256);

    form->addRow(QStringLiteral("Overlay"), m_transcriptOverlayEnabledCheckBox);
    form->addRow(QStringLiteral("Window"), m_transcriptBackgroundVisibleCheckBox);
    form->addRow(QStringLiteral("Shadow"), m_transcriptShadowEnabledCheckBox);
    form->addRow(QStringLiteral("Title"), m_transcriptShowSpeakerTitleCheckBox);
    form->addRow(QStringLiteral("Max Lines"), m_transcriptMaxLinesSpin);
    form->addRow(QStringLiteral("Max Chars"), m_transcriptMaxCharsSpin);
    form->addRow(QStringLiteral("Follow Word"), m_transcriptFollowCurrentWordCheckBox);
    form->addRow(QStringLiteral("X (Norm)"), m_transcriptOverlayXSpin);
    form->addRow(QStringLiteral("Y (Norm)"), m_transcriptOverlayYSpin);
    auto *centerButtonsLayout = new QHBoxLayout;
    centerButtonsLayout->setContentsMargins(0, 0, 0, 0);
    centerButtonsLayout->setSpacing(4);
    centerButtonsLayout->addWidget(m_transcriptCenterHorizontalButton);
    centerButtonsLayout->addWidget(m_transcriptCenterVerticalButton);
    auto *centerButtonsContainer = new QWidget(settingsContainer);
    centerButtonsContainer->setLayout(centerButtonsLayout);
    form->addRow(QStringLiteral("Center"), centerButtonsContainer);
    form->addRow(QStringLiteral("Width"), m_transcriptOverlayWidthSpin);
    form->addRow(QStringLiteral("Height"), m_transcriptOverlayHeightSpin);
    form->addRow(QStringLiteral("Font"), m_transcriptFontFamilyCombo);
    form->addRow(QStringLiteral("Font Size"), m_transcriptFontSizeSpin);
    form->addRow(QStringLiteral("Bold"), m_transcriptBoldCheckBox);
    form->addRow(QStringLiteral("Italic"), m_transcriptItalicCheckBox);
    form->addRow(QStringLiteral("Edit Colors"), m_transcriptUnifiedEditModeCheckBox);
    form->addRow(QStringLiteral("Search"), m_transcriptSearchFilterLineEdit);
    form->addRow(QStringLiteral("Speaker"), m_transcriptSpeakerFilterCombo);
    form->addRow(QStringLiteral("Visibility"), m_transcriptShowExcludedLinesCheckBox);

    // --- Speech filter section ---
    auto *speechSectionLabel = new QLabel(QStringLiteral("Speech Filter"), settingsContainer);
    speechSectionLabel->setStyleSheet(QStringLiteral("font-weight: 600; color: #8fa3b8; font-size: 12px; padding-top: 4px;"));

    auto *speechForm = new QFormLayout;
    speechForm->setSpacing(4);
    m_speechFilterEnabledCheckBox = new QCheckBox(QStringLiteral("Enable Speech Filter"), settingsContainer);
    m_transcriptPrependMsSpin = new QSpinBox(settingsContainer);
    m_transcriptPostpendMsSpin = new QSpinBox(settingsContainer);
    m_speechFilterFadeSamplesSpin = new QSpinBox(settingsContainer);
    m_speechFilterRangeCrossfadeCheckBox =
        new QCheckBox(QStringLiteral("Boundary Crossfade"), settingsContainer);
    m_playbackClockSourceCombo = new QComboBox(settingsContainer);
    m_playbackAudioWarpModeCombo = new QComboBox(settingsContainer);
    m_playbackClockSourceCombo->setToolTip(
        QStringLiteral("Choose whether preview time is driven by audio or by the timeline clock."));
    m_playbackAudioWarpModeCombo->setToolTip(
        QStringLiteral("Audio behavior when preview speed is not 1x."));
    m_playbackClockSourceCombo->addItem(playbackClockSourceLabel(PlaybackClockSource::Auto),
                                        playbackClockSourceToString(PlaybackClockSource::Auto));
    m_playbackClockSourceCombo->addItem(playbackClockSourceLabel(PlaybackClockSource::Audio),
                                        playbackClockSourceToString(PlaybackClockSource::Audio));
    m_playbackClockSourceCombo->addItem(playbackClockSourceLabel(PlaybackClockSource::Timeline),
                                        playbackClockSourceToString(PlaybackClockSource::Timeline));
    m_playbackAudioWarpModeCombo->addItem(playbackAudioWarpModeLabel(PlaybackAudioWarpMode::Disabled),
                                          playbackAudioWarpModeToString(PlaybackAudioWarpMode::Disabled));
    m_playbackAudioWarpModeCombo->addItem(playbackAudioWarpModeLabel(PlaybackAudioWarpMode::Varispeed),
                                          playbackAudioWarpModeToString(PlaybackAudioWarpMode::Varispeed));
    m_playbackAudioWarpModeCombo->addItem(playbackAudioWarpModeLabel(PlaybackAudioWarpMode::TimeStretch),
                                          playbackAudioWarpModeToString(PlaybackAudioWarpMode::TimeStretch));

    m_transcriptPrependMsSpin->setRange(0, 10000);
    m_transcriptPrependMsSpin->setValue(150);
    m_transcriptPrependMsSpin->setSuffix(QStringLiteral(" ms"));
    m_transcriptPrependMsSpin->setToolTip(QStringLiteral("Milliseconds to add before each word"));

    m_transcriptPostpendMsSpin->setRange(0, 10000);
    m_transcriptPostpendMsSpin->setValue(70);
    m_transcriptPostpendMsSpin->setSuffix(QStringLiteral(" ms"));
    m_transcriptPostpendMsSpin->setToolTip(QStringLiteral("Milliseconds to add after each word"));

    m_speechFilterFadeSamplesSpin->setRange(0, 5000);
    m_speechFilterFadeSamplesSpin->setValue(300);
    m_speechFilterFadeSamplesSpin->setSuffix(QStringLiteral(" samples"));
    m_speechFilterFadeSamplesSpin->setToolTip(
        QStringLiteral("Fade/crossfade window at speech range boundaries (0 = no transition)."));
    m_speechFilterRangeCrossfadeCheckBox->setChecked(false);
    m_speechFilterRangeCrossfadeCheckBox->setToolTip(
        QStringLiteral("Blend adjacent speech ranges instead of fading to silence. "
                       "Does not change audio duration."));

    speechForm->addRow(QStringLiteral("Speech Filter"), m_speechFilterEnabledCheckBox);
    speechForm->addRow(QStringLiteral("Prepend Time"), m_transcriptPrependMsSpin);
    speechForm->addRow(QStringLiteral("Postpend Time"), m_transcriptPostpendMsSpin);
    speechForm->addRow(QStringLiteral("Fade Length"), m_speechFilterFadeSamplesSpin);
    speechForm->addRow(QStringLiteral("Transition"), m_speechFilterRangeCrossfadeCheckBox);
    speechForm->addRow(QStringLiteral("Clock Source"), m_playbackClockSourceCombo);
    speechForm->addRow(QStringLiteral("Audio Warp"), m_playbackAudioWarpModeCombo);

    // --- Assemble settings layout ---
    settingsLayout->addWidget(m_transcriptInspectorClipLabel);
    settingsLayout->addWidget(m_transcriptInspectorDetailsLabel);
    settingsLayout->addLayout(cutHeaderLayout);
    settingsLayout->addLayout(versionButtonsLayout);
    settingsLayout->addWidget(separator);
    settingsLayout->addWidget(overlaySectionLabel);
    settingsLayout->addLayout(form);
    settingsLayout->addWidget(speechSectionLabel);
    settingsLayout->addLayout(speechForm);
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
    m_transcriptTable->verticalHeader()->setVisible(false);
    m_transcriptTable->horizontalHeader()->setStretchLastSection(true);
    m_transcriptTable->horizontalHeader()->setSectionResizeMode(0, QHeaderView::ResizeToContents);
    m_transcriptTable->horizontalHeader()->setSectionResizeMode(1, QHeaderView::ResizeToContents);
    m_transcriptTable->horizontalHeader()->setSectionResizeMode(2, QHeaderView::ResizeToContents);
    m_transcriptTable->horizontalHeader()->setSectionResizeMode(3, QHeaderView::Stretch);
    m_transcriptTable->horizontalHeader()->setSectionResizeMode(4, QHeaderView::ResizeToContents);

    splitter->addWidget(settingsScroll);
    splitter->addWidget(m_transcriptTable);
    splitter->setStretchFactor(0, 0);
    splitter->setStretchFactor(1, 1);
    splitter->setSizes({360, 420});

    layout->addWidget(splitter);

    return page;
}

QWidget *InspectorPane::buildSpeakersTab()
{
    auto *page = new QWidget;
    auto *layout = createTabLayout(page);
    layout->addWidget(createTabHeading(QStringLiteral("Speakers"), page));
    auto *speakersSubtabs = new QTabWidget(page);
    auto *overviewPage = new QWidget(speakersSubtabs);
    auto *overviewLayout = createTabLayout(overviewPage);
    auto *generatePage = new QWidget(speakersSubtabs);
    auto *generateLayout = createTabLayout(generatePage);
    auto *mappingPage = new QWidget(speakersSubtabs);
    auto *mappingLayout = createTabLayout(mappingPage);
    auto *identityPage = new QWidget(speakersSubtabs);
    auto *identityLayout = createTabLayout(identityPage);
    auto *trackingPage = new QWidget(speakersSubtabs);
    auto *trackingLayout = createTabLayout(trackingPage);
    auto *sentencePage = new QWidget(speakersSubtabs);
    auto *sentenceLayout = createTabLayout(sentencePage);
    auto *debugPage = new QWidget(speakersSubtabs);
    auto *debugLayout = createTabLayout(debugPage);

    m_speakersInspectorClipLabel = new QLabel(QStringLiteral("No transcript cut selected"), page);
    m_speakersInspectorDetailsLabel = new QLabel(QString(), page);
    m_speakersInspectorDetailsLabel->setWordWrap(true);

    m_speakersTable = new QTableWidget(page);
    m_speakersTable->setColumnCount(8);
    m_speakersTable->setHorizontalHeaderLabels(
        {QStringLiteral("Avatar"),
         QStringLiteral("Speaker ID"),
         QStringLiteral("Name"),
         QStringLiteral("X"),
         QStringLiteral("Y"),
         QStringLiteral("Subtitle Face Tracking"),
         QStringLiteral("Ref1 Avatar"),
         QStringLiteral("Ref2 Avatar")});
    m_speakersTable->setSelectionBehavior(QAbstractItemView::SelectRows);
    m_speakersTable->setSelectionMode(QAbstractItemView::ExtendedSelection);
    m_speakersTable->setEditTriggers(QAbstractItemView::DoubleClicked |
                                     QAbstractItemView::EditKeyPressed);
    m_speakersTable->verticalHeader()->setVisible(false);
    m_speakersTable->horizontalHeader()->setStretchLastSection(false);
    m_speakersTable->horizontalHeader()->setSectionResizeMode(0, QHeaderView::ResizeToContents);
    m_speakersTable->horizontalHeader()->setSectionResizeMode(1, QHeaderView::ResizeToContents);
    m_speakersTable->horizontalHeader()->setSectionResizeMode(2, QHeaderView::ResizeToContents);
    m_speakersTable->horizontalHeader()->setSectionResizeMode(3, QHeaderView::ResizeToContents);
    m_speakersTable->horizontalHeader()->setSectionResizeMode(4, QHeaderView::ResizeToContents);
    m_speakersTable->horizontalHeader()->setSectionResizeMode(5, QHeaderView::ResizeToContents);
    m_speakersTable->horizontalHeader()->setSectionResizeMode(6, QHeaderView::ResizeToContents);
    m_speakersTable->horizontalHeader()->setSectionResizeMode(7, QHeaderView::ResizeToContents);

    auto *selectedSpeakerTitle = new QLabel(QStringLiteral("Selected Speaker"), page);
    selectedSpeakerTitle->setStyleSheet(QStringLiteral("font-weight: 600; color: #8fa3b8;"));
    m_selectedSpeakerIdLabel = new QLabel(QStringLiteral("No speaker selected"), page);
    m_selectedSpeakerRef1ImageLabel = new QLabel(page);
    m_selectedSpeakerRef2ImageLabel = new QLabel(page);
    for (QLabel* imageLabel : {m_selectedSpeakerRef1ImageLabel, m_selectedSpeakerRef2ImageLabel}) {
        imageLabel->setMinimumSize(120, 120);
        imageLabel->setMaximumHeight(160);
        imageLabel->setAlignment(Qt::AlignCenter);
        imageLabel->setStyleSheet(QStringLiteral(
            "QLabel { border: 1px solid #314459; border-radius: 8px; background: #142234; color: #8fa3b8; }"));
        imageLabel->setText(QStringLiteral("Unset"));
    }
    auto *selectedImagesRow = new QHBoxLayout;
    selectedImagesRow->setSpacing(8);
    auto *selectedRef1Col = new QVBoxLayout;
    auto *selectedRef2Col = new QVBoxLayout;
    selectedRef1Col->addWidget(new QLabel(QStringLiteral("Ref1"), page));
    selectedRef1Col->addWidget(m_selectedSpeakerRef1ImageLabel);
    selectedRef2Col->addWidget(new QLabel(QStringLiteral("Ref2"), page));
    selectedRef2Col->addWidget(m_selectedSpeakerRef2ImageLabel);
    selectedImagesRow->addLayout(selectedRef1Col, 1);
    selectedImagesRow->addLayout(selectedRef2Col, 1);
    auto *selectedActionsRow = new QHBoxLayout;
    m_selectedSpeakerPreviousSentenceButton = new QPushButton(QStringLiteral("Previous Sentence"), page);
    m_selectedSpeakerNextSentenceButton = new QPushButton(QStringLiteral("Next Sentence"), page);
    m_selectedSpeakerRandomSentenceButton = new QPushButton(QStringLiteral("Random Sentence"), page);
    for (QPushButton* button :
         {m_selectedSpeakerPreviousSentenceButton,
          m_selectedSpeakerNextSentenceButton,
          m_selectedSpeakerRandomSentenceButton}) {
        button->setMinimumHeight(30);
        button->setSizePolicy(QSizePolicy::MinimumExpanding, QSizePolicy::Fixed);
    }
    selectedActionsRow->addWidget(m_selectedSpeakerPreviousSentenceButton);
    selectedActionsRow->addWidget(m_selectedSpeakerNextSentenceButton);
    selectedActionsRow->addWidget(m_selectedSpeakerRandomSentenceButton);
    auto *boxstreamActionRow = new QHBoxLayout;
    m_speakerRunAutoTrackButton = new QPushButton(QStringLiteral("Generate FaceStream"), page);
    m_speakerRunAutoTrackButton->setObjectName(QStringLiteral("speakers.generate_boxstream"));
    m_speakerRunAutoTrackButton->setMinimumHeight(32);
    m_speakerRunAutoTrackButton->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    m_speakerBoxstreamSettingsButton = new QPushButton(QStringLiteral("FaceStream Settings"), page);
    m_speakerBoxstreamSettingsButton->setObjectName(QStringLiteral("speakers.boxstream_settings"));
    m_speakerBoxstreamSettingsButton->setMinimumHeight(32);
    m_speakerBoxstreamSettingsButton->setSizePolicy(QSizePolicy::MinimumExpanding, QSizePolicy::Fixed);
    boxstreamActionRow->addWidget(m_speakerRunAutoTrackButton);
    boxstreamActionRow->addWidget(m_speakerBoxstreamSettingsButton);

    auto *speakerAiRow = new QHBoxLayout;
    speakerAiRow->setContentsMargins(0, 0, 0, 0);
    speakerAiRow->setSpacing(6);
    auto *speakerIdentifyButton = new QPushButton(QStringLiteral("Identify Speakers"), page);
    speakerIdentifyButton->setObjectName(QStringLiteral("speakers.identify_subscription"));
    speakerIdentifyButton->setEnabled(false);
    speakerIdentifyButton->setAttribute(Qt::WA_AlwaysShowToolTips, true);
    speakerIdentifyButton->setToolTip(QStringLiteral("Requires Subscription"));
    speakerIdentifyButton->setStyleSheet(
        QStringLiteral("QPushButton:disabled {"
                       " color: #8a94a1;"
                       " background: #1b2129;"
                       " border: 1px solid #353d47;"
                       "}"));
    speakerIdentifyButton->setMinimumHeight(30);
    speakerIdentifyButton->setSizePolicy(QSizePolicy::MinimumExpanding, QSizePolicy::Fixed);
    m_speakerAiFindNamesButton = new QPushButton(QStringLiteral("Mine Transcript (AI)"), page);
    m_speakerPrecropFacesButton = new QPushButton(QStringLiteral("Pre-crop Faces"), page);
    m_speakerAiFindOrganizationsButton = new QPushButton(QStringLiteral("Find Organizations"), page);
    m_speakerAiCleanAssignmentsButton = new QPushButton(QStringLiteral("Clean Assignments"), page);
    for (QPushButton* button :
         {m_speakerPrecropFacesButton, m_speakerAiFindNamesButton, m_speakerAiFindOrganizationsButton, m_speakerAiCleanAssignmentsButton}) {
        button->setMinimumHeight(30);
        button->setSizePolicy(QSizePolicy::MinimumExpanding, QSizePolicy::Fixed);
    }
    speakerAiRow->addWidget(speakerIdentifyButton);
    speakerAiRow->addWidget(m_speakerPrecropFacesButton);
    speakerAiRow->addWidget(m_speakerAiFindNamesButton);
    speakerAiRow->addWidget(m_speakerAiFindOrganizationsButton);
    speakerAiRow->addWidget(m_speakerAiCleanAssignmentsButton);

    auto *stateTitle = new QLabel(QStringLiteral("State"), page);
    stateTitle->setStyleSheet(QStringLiteral("font-weight: 600; color: #8fa3b8;"));
    auto makeStateChip = [page](const QString& text) {
        auto *chip = new QLabel(text, page);
        chip->setStyleSheet(QStringLiteral(
            "QLabel {"
            " border: 1px solid #314459;"
            " border-radius: 9px;"
            " background: #142234;"
            " color: #d8e6f5;"
            " padding: 3px 8px;"
            " font-size: 11px;"
            "}"));
        return chip;
    };
    auto makeToggleChip = [page](const QString& text) {
        auto *chip = new QPushButton(text, page);
        chip->setCheckable(true);
        chip->setCursor(Qt::PointingHandCursor);
        chip->setStyleSheet(QStringLiteral(
            "QPushButton {"
            " border: 1px solid #314459;"
            " border-radius: 9px;"
            " background: #142234;"
            " color: #d8e6f5;"
            " padding: 3px 8px;"
            " font-size: 11px;"
            "}"
            "QPushButton:hover { background: #1b2b3f; }"
            "QPushButton:checked { border-color: #4ea1ff; background: #1d324a; color: #eef6ff; }"));
        return chip;
    };
    m_speakerRefsChipLabel = makeStateChip(QStringLiteral("Refs: 0/2"));
    m_speakerPointstreamChipLabel = makeStateChip(QStringLiteral("FaceStream: None"));
    m_speakerTrackingChipButton = makeToggleChip(QStringLiteral("Tracking: OFF"));
    m_speakerStabilizeChipButton = makeToggleChip(QStringLiteral("Face Stabilize: OFF"));
    m_speakerTrackingChipButton->setObjectName(QStringLiteral("speakers.tracking_toggle"));
    m_speakerStabilizeChipButton->setObjectName(QStringLiteral("speakers.stabilize_toggle"));
    auto *stateChipsRow = new QHBoxLayout;
    stateChipsRow->setContentsMargins(0, 0, 0, 0);
    stateChipsRow->setSpacing(6);
    stateChipsRow->addWidget(m_speakerRefsChipLabel);
    stateChipsRow->addWidget(m_speakerPointstreamChipLabel);
    stateChipsRow->addWidget(m_speakerTrackingChipButton);
    stateChipsRow->addStretch(1);

    auto *targetTitle = new QLabel(QStringLiteral("FaceBox (Yellow Box)"), page);
    targetTitle->setStyleSheet(QStringLiteral("font-weight: 600; color: #8fa3b8;"));
    m_speakerFramingZoomEnabledCheckBox =
        new QCheckBox(QStringLiteral("Show FaceBox"), page);
    m_speakerShowBoxStreamBoxesCheckBox =
        new QCheckBox(QStringLiteral("Show All FaceStreams in Preview"), page);
    m_speakerShowBoxStreamBoxesCheckBox->setChecked(false);
    m_speakerShowBoxStreamBoxesCheckBox->setToolTip(
        QStringLiteral("Draw all FaceStream face boxes for active clips in Preview."));
    auto *faceboxControlsRow = new QHBoxLayout;
    faceboxControlsRow->setContentsMargins(0, 0, 0, 0);
    faceboxControlsRow->setSpacing(8);
    faceboxControlsRow->addWidget(m_speakerFramingZoomEnabledCheckBox);
    faceboxControlsRow->addWidget(m_speakerStabilizeChipButton);
    faceboxControlsRow->addStretch(1);
    m_speakerFramingTargetXSpin = new QDoubleSpinBox(page);
    m_speakerFramingTargetXSpin->setDecimals(3);
    m_speakerFramingTargetXSpin->setRange(0.0, 1.0);
    m_speakerFramingTargetXSpin->setSingleStep(0.01);
    m_speakerFramingTargetXSpin->setValue(0.5);
    m_speakerFramingTargetXSpin->setToolTip(
        QStringLiteral("Target horizontal face position in output space (0.0 left, 1.0 right)."));
    m_speakerFramingTargetYSpin = new QDoubleSpinBox(page);
    m_speakerFramingTargetYSpin->setDecimals(3);
    m_speakerFramingTargetYSpin->setRange(0.0, 1.0);
    m_speakerFramingTargetYSpin->setSingleStep(0.01);
    m_speakerFramingTargetYSpin->setValue(0.35);
    m_speakerFramingTargetYSpin->setToolTip(
        QStringLiteral("Target vertical face position in output space (0.0 top, 1.0 bottom)."));
    m_speakerFramingTargetBoxSpin = new QDoubleSpinBox(page);
    m_speakerFramingTargetBoxSpin->setDecimals(3);
    m_speakerFramingTargetBoxSpin->setRange(0.01, 1.0);
    m_speakerFramingTargetBoxSpin->setSingleStep(0.01);
    m_speakerFramingTargetBoxSpin->setValue(0.20);
    m_speakerFramingTargetBoxSpin->setToolTip(
        QStringLiteral("FaceBox normalized side length used by Generate FaceStream zoom."));
    auto *targetForm = new QFormLayout;
    targetForm->addRow(QStringLiteral("Yellow Box X"), m_speakerFramingTargetXSpin);
    targetForm->addRow(QStringLiteral("Yellow Box Y"), m_speakerFramingTargetYSpin);
    targetForm->addRow(QStringLiteral("Yellow Box Size"), m_speakerFramingTargetBoxSpin);
    m_speakerApplyFramingToClipCheckBox = nullptr;
    m_speakerClipFramingStatusLabel = new QLabel(QStringLiteral("Face Stabilize: OFF | 0 keys"), page);
    m_speakerClipFramingStatusLabel->setStyleSheet(QStringLiteral("color: #8fa3b8; font-size: 11px;"));

    auto *currentSentenceTitle = new QLabel(QStringLiteral("Current Speaker Sentence"), page);
    currentSentenceTitle->setStyleSheet(QStringLiteral("font-weight: 600; color: #8fa3b8;"));
    m_speakerCurrentSentenceLabel = new QLabel(QStringLiteral("Select a speaker to view sentence context."), page);
    m_speakerCurrentSentenceLabel->setWordWrap(true);
    m_speakerCurrentSentenceLabel->setMinimumHeight(48);
    m_speakerCurrentSentenceLabel->setStyleSheet(QStringLiteral(
        "QLabel { border: 1px solid #314459; border-radius: 8px; background: #142234; color: #d8e6f5; padding: 8px; }"));

    m_speakerTrackingStatusLabel = new QLabel(QString(), page);
    m_speakerTrackingStatusLabel->setWordWrap(true);
    m_speakerTrackingStatusLabel->setStyleSheet(QStringLiteral("color: #8fa3b8; font-size: 11px;"));

    auto *boxstreamStartTitle = new QLabel(QStringLiteral("Step 1: Identify Unique Faces + Generate FaceStream"), page);
    boxstreamStartTitle->setStyleSheet(QStringLiteral("font-weight: 600; color: #8fa3b8;"));
    auto *boxstreamStepsLabel = new QLabel(
        QStringLiteral("Tracker pipeline:\n"
                       "1) Detect faces\n"
                       "2) Associate detections to tracks\n"
                       "3) Smooth track center/size\n"
                       "4) Persist continuity FaceStreams\n"
                       "5) Preview superimposed by tracker source"),
        page);
    boxstreamStepsLabel->setWordWrap(true);
    boxstreamStepsLabel->setStyleSheet(QStringLiteral("color: #8fa3b8; font-size: 11px;"));
    auto *mappingTitle = new QLabel(QStringLiteral("Step 2: Speaker to FaceStream Mapping"), page);
    mappingTitle->setStyleSheet(QStringLiteral("font-weight: 600; color: #8fa3b8;"));
    auto *mappingHelp = new QLabel(
        QStringLiteral("First resolve all unique faces in FaceFind, then use this matrix to assign and refine each speaker path."),
        page);
    mappingHelp->setWordWrap(true);
    mappingHelp->setStyleSheet(QStringLiteral("color: #8fa3b8; font-size: 11px;"));

    auto *identityTitle = new QLabel(QStringLiteral("Identity + References"), page);
    identityTitle->setStyleSheet(QStringLiteral("font-weight: 600; color: #8fa3b8;"));

    auto *trackingTitle = new QLabel(QStringLiteral("Tracking + FaceBox"), page);
    trackingTitle->setStyleSheet(QStringLiteral("font-weight: 600; color: #8fa3b8;"));

    auto *debugTitle = new QLabel(QStringLiteral("Speaker Flow Debug"), page);
    debugTitle->setStyleSheet(QStringLiteral("font-weight: 600; color: #8fa3b8;"));

    m_speakerDebugCaptureCheckBox = new QCheckBox(QStringLiteral("Enable Debug Capture"), page);
    m_speakerDebugCaptureCheckBox->setChecked(true);
    m_speakerOpenLatestDebugRunButton = new QPushButton(QStringLiteral("Open Latest Debug Run"), page);
    m_speakerExportDebugBundleButton = new QPushButton(QStringLiteral("Export Debug Bundle"), page);
    auto *debugActionsRow = new QHBoxLayout;
    debugActionsRow->setContentsMargins(0, 0, 0, 0);
    debugActionsRow->setSpacing(6);
    debugActionsRow->addWidget(m_speakerOpenLatestDebugRunButton);
    debugActionsRow->addWidget(m_speakerExportDebugBundleButton);
    debugActionsRow->addStretch(1);
    m_speakerDebugStatusLabel = new QLabel(
        QStringLiteral("Run ID: - | Last failed stage: none"),
        page);
    m_speakerDebugStatusLabel->setWordWrap(true);
    m_speakerDebugStatusLabel->setStyleSheet(QStringLiteral("color: #8fa3b8; font-size: 11px;"));
    overviewLayout->addWidget(m_speakersInspectorClipLabel);
    overviewLayout->addWidget(m_speakersInspectorDetailsLabel);
    overviewLayout->addStretch(1);

    generateLayout->addWidget(boxstreamStartTitle);
    generateLayout->addWidget(boxstreamStepsLabel);
    generateLayout->addLayout(boxstreamActionRow);
    generateLayout->addStretch(1);

    mappingLayout->addWidget(mappingTitle);
    mappingLayout->addWidget(mappingHelp);
    mappingLayout->addWidget(m_speakersTable, 1);

    identityLayout->addWidget(identityTitle);
    identityLayout->addWidget(selectedSpeakerTitle);
    identityLayout->addWidget(m_selectedSpeakerIdLabel);
    identityLayout->addLayout(selectedImagesRow);
    identityLayout->addLayout(selectedActionsRow);
    identityLayout->addLayout(speakerAiRow);
    identityLayout->addStretch(1);

    trackingLayout->addWidget(trackingTitle);
    trackingLayout->addWidget(stateTitle);
    trackingLayout->addLayout(stateChipsRow);
    trackingLayout->addWidget(targetTitle);
    trackingLayout->addLayout(faceboxControlsRow);
    trackingLayout->addLayout(targetForm);
    trackingLayout->addWidget(m_speakerClipFramingStatusLabel);
    trackingLayout->addStretch(1);

    sentenceLayout->addWidget(currentSentenceTitle);
    sentenceLayout->addWidget(m_speakerCurrentSentenceLabel);
    sentenceLayout->addWidget(m_speakerTrackingStatusLabel);
    sentenceLayout->addStretch(1);

    debugLayout->addWidget(debugTitle);
    debugLayout->addWidget(m_speakerDebugCaptureCheckBox);
    debugLayout->addLayout(debugActionsRow);
    debugLayout->addWidget(m_speakerDebugStatusLabel);
    debugLayout->addStretch(1);

    auto *boxstreamPage = new QWidget(speakersSubtabs);
    auto *boxstreamLayout = createTabLayout(boxstreamPage);
    auto *boxstreamTitle = new QLabel(QStringLiteral("FaceStream Paths"), boxstreamPage);
    boxstreamTitle->setStyleSheet(QStringLiteral("font-weight: 600; color: #8fa3b8;"));
    auto *boxstreamHelp = new QLabel(
        QStringLiteral("View generated continuity tracks (paths) for the selected clip from speaker_flow.continuity_boxstreams."),
        boxstreamPage);
    boxstreamHelp->setWordWrap(true);
    boxstreamHelp->setStyleSheet(QStringLiteral("color: #8fa3b8; font-size: 11px;"));
    m_speakerBoxStreamTable = new QTableWidget(boxstreamPage);
    m_speakerBoxStreamTable->setColumnCount(5);
    m_speakerBoxStreamTable->setHorizontalHeaderLabels(
        {QStringLiteral("Stream"),
         QStringLiteral("Track"),
         QStringLiteral("Frames"),
         QStringLiteral("Range"),
         QStringLiteral("Source")});
    m_speakerBoxStreamTable->setSelectionBehavior(QAbstractItemView::SelectRows);
    m_speakerBoxStreamTable->setSelectionMode(QAbstractItemView::SingleSelection);
    m_speakerBoxStreamTable->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_speakerBoxStreamTable->verticalHeader()->setVisible(false);
    m_speakerBoxStreamTable->horizontalHeader()->setStretchLastSection(true);
    m_speakerBoxStreamTable->horizontalHeader()->setSectionResizeMode(0, QHeaderView::ResizeToContents);
    m_speakerBoxStreamTable->horizontalHeader()->setSectionResizeMode(1, QHeaderView::ResizeToContents);
    m_speakerBoxStreamTable->horizontalHeader()->setSectionResizeMode(2, QHeaderView::ResizeToContents);
    m_speakerBoxStreamTable->horizontalHeader()->setSectionResizeMode(3, QHeaderView::ResizeToContents);
    m_speakerBoxStreamTable->horizontalHeader()->setSectionResizeMode(4, QHeaderView::Stretch);
    m_speakerBoxStreamDetailsEdit = new QPlainTextEdit(boxstreamPage);
    m_speakerBoxStreamDetailsEdit->setReadOnly(true);
    m_speakerBoxStreamDetailsEdit->setPlaceholderText(QStringLiteral("Select a FaceStream path row to inspect full JSON."));
    m_speakerBoxStreamDetailsEdit->setMinimumHeight(160);
    auto *overlaySourceRow = new QHBoxLayout;
    overlaySourceRow->setContentsMargins(0, 0, 0, 0);
    overlaySourceRow->setSpacing(8);
    auto *overlaySourceLabel = new QLabel(QStringLiteral("Preview Tracker"), boxstreamPage);
    m_speakerBoxStreamOverlaySourceCombo = new QComboBox(boxstreamPage);
    m_speakerBoxStreamOverlaySourceCombo->addItem(QStringLiteral("All Trackers"), QStringLiteral("all"));
    m_speakerBoxStreamOverlaySourceCombo->addItem(QStringLiteral("OpenCV Haar (Balanced)"), QStringLiteral("opencv_haar_v1"));
    m_speakerBoxStreamOverlaySourceCombo->addItem(QStringLiteral("OpenCV Haar (Small Faces)"), QStringLiteral("opencv_haar_smallfaces_v1"));
    m_speakerBoxStreamOverlaySourceCombo->addItem(QStringLiteral("OpenCV Haar (Precision)"), QStringLiteral("opencv_haar_precision_v1"));
    m_speakerBoxStreamOverlaySourceCombo->addItem(QStringLiteral("OpenCV LBP (Small Faces)"), QStringLiteral("opencv_lbp_smallfaces_v1"));
    m_speakerBoxStreamOverlaySourceCombo->addItem(QStringLiteral("OpenCV DNN Auto"), QStringLiteral("opencv_dnn_auto_v1"));
    m_speakerBoxStreamOverlaySourceCombo->addItem(QStringLiteral("OpenCV Python-Compatible"), QStringLiteral("opencv_python_compatible_v1"));
    m_speakerBoxStreamOverlaySourceCombo->addItem(QStringLiteral("Legacy Python Haar"), QStringLiteral("python_legacy_v1"));
    m_speakerBoxStreamOverlaySourceCombo->addItem(QStringLiteral("Local Production Hybrid"), QStringLiteral("local_insightface_hybrid_v1"));
    m_speakerBoxStreamOverlaySourceCombo->addItem(QStringLiteral("Native Production Hybrid (C++)"), QStringLiteral("native_hybrid_v1"));
    m_speakerBoxStreamOverlaySourceCombo->addItem(QStringLiteral("Docker DNN Res10"), QStringLiteral("docker_dnn_v1"));
    m_speakerBoxStreamOverlaySourceCombo->addItem(QStringLiteral("Docker InsightFace RetinaFace"), QStringLiteral("docker_insightface_retinaface_v1"));
    m_speakerBoxStreamOverlaySourceCombo->addItem(QStringLiteral("Docker Production Hybrid"), QStringLiteral("docker_insightface_hybrid_v1"));
    m_speakerBoxStreamOverlaySourceCombo->addItem(QStringLiteral("Docker YOLOv8 Face"), QStringLiteral("docker_yolov8_face_v1"));
    m_speakerBoxStreamOverlaySourceCombo->addItem(QStringLiteral("Docker MTCNN"), QStringLiteral("docker_mtcnn_v1"));
    m_speakerBoxStreamOverlaySourceCombo->addItem(QStringLiteral("SAM3 Face"), QStringLiteral("sam3_face_v1"));
    m_speakerBoxStreamOverlaySourceCombo->addItem(QStringLiteral("OpenCV Contrib CSRT"), QStringLiteral("opencv_contrib_csrt_v1"));
    m_speakerBoxStreamOverlaySourceCombo->addItem(QStringLiteral("OpenCV Contrib KCF"), QStringLiteral("opencv_contrib_kcf_v1"));
    const QColor dockerBg(QStringLiteral("#dff1ff"));
    const QColor nativeBg(QStringLiteral("#e7f7e7"));
    for (int i = 0; i < m_speakerBoxStreamOverlaySourceCombo->count(); ++i) {
        const QString source = m_speakerBoxStreamOverlaySourceCombo->itemData(i).toString();
        if (source.startsWith(QStringLiteral("docker_"))) {
            m_speakerBoxStreamOverlaySourceCombo->setItemData(i, QBrush(dockerBg), Qt::BackgroundRole);
        } else if (source.startsWith(QStringLiteral("native_"))) {
            m_speakerBoxStreamOverlaySourceCombo->setItemData(i, QBrush(nativeBg), Qt::BackgroundRole);
        }
    }
    m_speakerBoxStreamOverlaySourceCombo->setCurrentIndex(0);
    m_speakerBoxStreamOverlaySourceCombo->setToolTip(
        QStringLiteral("Choose which tracker output is superimposed in Preview for post comparison."));
    m_speakerBoxStreamOverlaySourceCombo->setStyleSheet(QStringLiteral(
        "QComboBox QAbstractItemView::item { padding: 3px 6px; }"));
    overlaySourceRow->addWidget(overlaySourceLabel);
    overlaySourceRow->addWidget(m_speakerBoxStreamOverlaySourceCombo, 1);
    boxstreamLayout->addWidget(boxstreamTitle);
    boxstreamLayout->addWidget(boxstreamHelp);
    boxstreamLayout->addLayout(overlaySourceRow);
    boxstreamLayout->addWidget(m_speakerShowBoxStreamBoxesCheckBox);
    boxstreamLayout->addWidget(m_speakerBoxStreamTable, 1);
    boxstreamLayout->addWidget(m_speakerBoxStreamDetailsEdit);

    speakersSubtabs->addTab(overviewPage, QStringLiteral("Overview"));
    speakersSubtabs->addTab(generatePage, QStringLiteral("Face ID + Generate"));
    speakersSubtabs->addTab(mappingPage, QStringLiteral("Mapping"));
    speakersSubtabs->addTab(identityPage, QStringLiteral("Identity"));
    speakersSubtabs->addTab(trackingPage, QStringLiteral("Tracking"));
    speakersSubtabs->addTab(sentencePage, QStringLiteral("Sentence"));
    speakersSubtabs->addTab(boxstreamPage, QStringLiteral("FaceStream Paths"));
    speakersSubtabs->addTab(debugPage, QStringLiteral("Debug"));
    layout->addWidget(speakersSubtabs, 1);
    return page;
}


QWidget *InspectorPane::buildPane()
{
    auto *pane = new QFrame;
    pane->setMinimumWidth(120);
    pane->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Expanding);

    auto *layout = new QVBoxLayout(pane);
    layout->setContentsMargins(8, 8, 8, 8);
    layout->setSpacing(8);

    auto *headerRow = new QWidget(pane);
    headerRow->setObjectName(QStringLiteral("inspector.header_row"));
    headerRow->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    headerRow->setMinimumHeight(32);
    m_headerLayout = new QHBoxLayout(headerRow);
    m_headerLayout->setContentsMargins(0, 0, 0, 0);
    m_headerLayout->setSpacing(6);
    m_headerLayout->addStretch(1);
    layout->addWidget(headerRow);

    m_inspectorTabs = new InspectorTabWidget(pane);
    m_inspectorTabs->addTab(buildGradingTab(), QStringLiteral("Grade"));
    m_inspectorTabs->addTab(buildOpacityTab(), QStringLiteral("Opacity"));
    m_inspectorTabs->addTab(buildEffectsTab(), QStringLiteral("Effects"));
    m_inspectorTabs->addTab(buildCorrectionsTab(), QStringLiteral("Corrections"));
    m_inspectorTabs->addTab(buildTitlesTab(), QStringLiteral("Titles"));
    m_inspectorTabs->addTab(buildSyncTab(), QStringLiteral("Sync"));
    m_inspectorTabs->addTab(buildKeyframesTab(), QStringLiteral("Keyframes"));
    m_inspectorTabs->addTab(buildTranscriptTab(), QStringLiteral("Transcript"));
    m_inspectorTabs->addTab(buildSpeakersTab(), QStringLiteral("Speakers"));
    m_inspectorTabs->addTab(buildClipTab(), QStringLiteral("Properties"));
    m_inspectorTabs->addTab(buildClipsTab(), QStringLiteral("Clips"));
    m_inspectorTabs->addTab(buildHistoryTab(), QStringLiteral("History"));
    m_inspectorTabs->addTab(buildTracksTab(), QStringLiteral("Tracks"));
    m_inspectorTabs->addTab(buildPreviewTab(), QStringLiteral("Preview"));
    m_inspectorTabs->addTab(buildAudioTab(), QStringLiteral("Audio"));
    m_inspectorTabs->addTab(buildAiTab(), QStringLiteral("AI Assist"));
    m_inspectorTabs->addTab(buildAccessTab(), QStringLiteral("Access"));
    m_inspectorTabs->addTab(buildOutputTab(), QStringLiteral("Output"));
    m_inspectorTabs->addTab(buildProfileTab(), QStringLiteral("System"));
    m_inspectorTabs->addTab(buildProjectsTab(), QStringLiteral("Projects"));
    m_inspectorTabs->addTab(buildPreferencesTab(), QStringLiteral("Preferences"));
    configureInspectorTabs();

    layout->addWidget(m_inspectorTabs, 1);
    return pane;
}
