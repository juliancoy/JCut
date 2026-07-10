#include "inspector_pane.h"
#include "audio_engine.h"
#include "editor_effect_presets.h"
#include "editor_shared.h"
#include "playback_timing_context.h"
#include "debug_controls.h"
#include "grading_histogram_widget.h"
#include "speakers_table.h"

#include <QAbstractButton>
#include <QBrush>
#include <QButtonGroup>
#include <QCheckBox>
#include <QColor>
#include <QComboBox>
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
#include <QSlider>
#include <QSplitter>
#include <QSpinBox>
#include <QTabBar>
#include <QTabWidget>
#include <QTableWidget>
#include <QPushButton>
#include <QPainter>
#include <QToolButton>
#include <QSize>
#include <QStyle>
#include <QStylePainter>
#include <QStyleOptionTab>
#include <QMouseEvent>
#include <QVBoxLayout>

namespace {
class HoverDockTabBar final : public QTabBar {
public:
    explicit HoverDockTabBar(QWidget* parent = nullptr)
        : QTabBar(parent)
    {
        setMouseTracking(true);
    }

    QSize sizeHint() const override {
        const QSize base = QTabBar::sizeHint();
        return QSize(railWidth(), qMin(base.height(), 520));
    }

    QSize minimumSizeHint() const override {
        return QSize(m_collapsedWidth, 120);
    }

    QSize tabSizeHint(int index) const override {
        const QSize base = QTabBar::tabSizeHint(index);
        const int tabHeight = qBound(34, base.height() + 8, 42);
        return QSize(railWidth(), tabHeight);
    }

protected:
    bool event(QEvent* event) override {
        if (event && event->type() == QEvent::Enter) {
            setExpanded(true);
        } else if (event && event->type() == QEvent::Leave) {
            setExpanded(false);
            setHoveredIndex(-1);
        }
        return QTabBar::event(event);
    }

    void paintEvent(QPaintEvent* event) override {
        Q_UNUSED(event);
        QStylePainter painter(this);
        QStyleOptionTab option;
        for (int i = 0; i < count(); ++i) {
            initStyleOption(&option, i);
            painter.drawControl(QStyle::CE_TabBarTabShape, option);

            const QRect tabBounds = tabRect(i);
            const bool hovered = i == m_hoveredIndex;
            const bool selected = i == currentIndex();
            if (hovered || selected) {
                painter.save();
                painter.setRenderHint(QPainter::Antialiasing, true);
                const QRect highlightRect = tabBounds.adjusted(5, 3, -5, -3);
                const QColor fill = selected
                    ? QColor(QStringLiteral("#26384a"))
                    : QColor(QStringLiteral("#213044"));
                const QColor border = hovered
                    ? QColor(QStringLiteral("#5d7590"))
                    : QColor(QStringLiteral("#43566c"));
                painter.setBrush(fill);
                painter.setPen(QPen(border, 1));
                painter.drawRoundedRect(highlightRect, 7, 7);
                if (hovered) {
                    painter.setPen(QPen(QColor(QStringLiteral("#8fb8e8")), 2));
                    painter.drawLine(highlightRect.right(), highlightRect.top() + 7,
                                     highlightRect.right(), highlightRect.bottom() - 7);
                }
                painter.restore();
            }

            const QRect rect = tabBounds.adjusted(m_expanded ? 10 : 0, 0, m_expanded ? -10 : 0, 0);
            QRect textRect = rect;
            const QIcon icon = tabIcon(i);
            const QSize iconExtent(20, 20);
            if (!icon.isNull()) {
                const QPixmap pixmap = icon.pixmap(iconExtent, isTabEnabled(i) ? QIcon::Normal : QIcon::Disabled);
                const int iconLeft = m_expanded
                    ? rect.left()
                    : tabBounds.left() + ((tabBounds.width() - iconExtent.width()) / 2);
                const QPoint iconTopLeft(iconLeft,
                                         tabBounds.top() + ((tabBounds.height() - iconExtent.height()) / 2));
                painter.drawPixmap(iconTopLeft, pixmap);
                textRect.setLeft(iconTopLeft.x() + iconExtent.width() + 9);
            }

            if (m_expanded) {
                painter.save();
                painter.setPen(isTabEnabled(i) ? Qt::white : QColor(QStringLiteral("#7f8b99")));
                painter.drawText(textRect, Qt::AlignVCenter | Qt::AlignLeft, tabText(i));
                painter.restore();
            }
        }
    }

    void mouseMoveEvent(QMouseEvent* event) override {
        if (!event) {
            return;
        }
        setExpanded(true);
        setHoveredIndex(tabAt(event->position().toPoint()));
        unsetCursor();
        QTabBar::mouseMoveEvent(event);
    }

private:
    int railWidth() const {
        return m_expanded ? m_expandedWidth : m_collapsedWidth;
    }

    void setExpanded(bool expanded) {
        if (m_expanded == expanded) {
            return;
        }
        m_expanded = expanded;
        updateGeometry();
        update();
        if (parentWidget()) {
            parentWidget()->updateGeometry();
            parentWidget()->update();
        }
    }

    void setHoveredIndex(int index) {
        if (m_hoveredIndex == index) {
            return;
        }
        m_hoveredIndex = index;
        update();
    }

    static constexpr int m_collapsedWidth = 48;
    static constexpr int m_expandedWidth = 156;
    bool m_expanded = false;
    int m_hoveredIndex = -1;
};

class InspectorTabWidget final : public QTabWidget {
public:
    explicit InspectorTabWidget(QWidget* parent = nullptr)
        : QTabWidget(parent)
    {
        setTabBar(new HoverDockTabBar(this));
        tabBar()->setUsesScrollButtons(true);
        tabBar()->setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Ignored);
        setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Expanding);
    }

    QSize minimumSizeHint() const override {
        return QSize(180, 180);
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
    m_gradingCurveThreePointLockCheckBox->setChecked(false);
    m_gradingCurveThreePointLockCheckBox->setToolTip(QStringLiteral(
        "When enabled, the RGB Lift/Gamma/Gain numbers and the current curve channel stay linked "
        "as a three-point correction."));
    m_gradingCurveSmoothingCheckBox =
        new QCheckBox(QStringLiteral("Smooth"), m_gradingCurvesPanel);
    m_gradingCurveSmoothingCheckBox->setChecked(true);
    m_gradingCurveSmoothingCheckBox->setToolTip(QStringLiteral("Smooth curve interpolation"));
    curveOptionsLayout->addWidget(m_gradingCurveThreePointLockCheckBox);
    curveOptionsLayout->addWidget(m_gradingCurveSmoothingCheckBox);
    curveOptionsLayout->addStretch();

    m_gradingHistogramWidget = new GradingHistogramWidget(m_gradingCurvesPanel);
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
    m_gradingAutoScrollCheckBox->setChecked(true);
    m_gradingFollowCurrentCheckBox->setChecked(true);
    m_gradingPreviewCheckBox->setChecked(true);
    m_gradingKeyAtPlayheadButton = new QPushButton(QStringLiteral("Key At Playhead"), content);
    m_gradingResetButton = new QPushButton(QStringLiteral("Reset Grading"), content);
    m_gradingResetButton->setToolTip(QStringLiteral("Reset the current grading values and curves to neutral."));
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

    auto *falloffRow = new QHBoxLayout;
    falloffRow->addWidget(new QLabel(QStringLiteral("Falloff:"), page));
    m_maskFeatherFalloffCombo = new QComboBox(page);
    m_maskFeatherFalloffCombo->addItem(QStringLiteral("Power"), 0);
    m_maskFeatherFalloffCombo->addItem(QStringLiteral("Linear"), 1);
    m_maskFeatherFalloffCombo->addItem(QStringLiteral("Smoothstep"), 2);
    m_maskFeatherFalloffCombo->addItem(QStringLiteral("Smootherstep"), 3);
    m_maskFeatherFalloffCombo->addItem(QStringLiteral("Cosine"), 4);
    m_maskFeatherFalloffCombo->addItem(QStringLiteral("Gaussian"), 5);
    m_maskFeatherFalloffCombo->setToolTip(
        QStringLiteral("Choose the edge-opacity falloff. Smoothstep and Smootherstep are motion-friendly; Cosine is natural; Gaussian is soft and photographic."));
    falloffRow->addWidget(m_maskFeatherFalloffCombo);
    falloffRow->addStretch();
    featherGroup->addLayout(falloffRow);
    
    auto *gammaRow = new QHBoxLayout;
    gammaRow->addWidget(new QLabel(QStringLiteral("Power:"), page));
    m_maskFeatherGammaSpin = new QDoubleSpinBox(page);
    m_maskFeatherGammaSpin->setRange(0.1, 5.0);
    m_maskFeatherGammaSpin->setDecimals(2);
    m_maskFeatherGammaSpin->setSingleStep(0.1);
    m_maskFeatherGammaSpin->setValue(2.0);
    m_maskFeatherGammaSpin->setToolTip(QStringLiteral("Power-law exponent. Available when Falloff is Power; 1.0 is linear, higher values retain a more opaque edge."));
    gammaRow->addWidget(m_maskFeatherGammaSpin);
    gammaRow->addStretch();
    featherGroup->addLayout(gammaRow);
    
    layout->addLayout(featherGroup);
    
    auto maskLayerSection = createDisclosureSection(page, QStringLiteral("Person Layer"), true);
    m_maskForegroundLayerCheck = new QCheckBox(QStringLiteral("SAM mask is foreground layer"), page);
    m_maskForegroundLayerCheck->setToolTip(QStringLiteral("Draw masked person pixels as a later Vulkan pass."));
    maskLayerSection.body->addWidget(m_maskForegroundLayerCheck);
    m_maskRepeatEnabledCheck = new QCheckBox(QStringLiteral("Repeat masked source"), page);
    m_maskRepeatEnabledCheck->setToolTip(
        QStringLiteral("Repeat the source image through the processed SAM mask channel."));
    maskLayerSection.body->addWidget(m_maskRepeatEnabledCheck);
    auto *maskRepeatForm = new QFormLayout;
    maskRepeatForm->setContentsMargins(0, 0, 0, 0);
    maskRepeatForm->setSpacing(6);
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
    m_maskRepeatDeltaXSpin->setToolTip(QStringLiteral("Screen-space X offset between masked repeats."));
    m_maskRepeatDeltaYSpin = makeRepeatDeltaSpin();
    m_maskRepeatDeltaYSpin->setValue(0.0);
    m_maskRepeatDeltaYSpin->setToolTip(QStringLiteral("Screen-space Y offset between masked repeats."));
    maskRepeatForm->addRow(QStringLiteral("Repeat X"), m_maskRepeatDeltaXSpin);
    maskRepeatForm->addRow(QStringLiteral("Repeat Y"), m_maskRepeatDeltaYSpin);
    maskLayerSection.body->addLayout(maskRepeatForm);
    layout->addWidget(maskLayerSection.container);

    auto presetSection = createDisclosureSection(page, QStringLiteral("Synthesis Effects"), true);
    auto *presetForm = new QFormLayout();
    presetForm->setContentsMargins(0, 0, 0, 0);
    presetForm->setSpacing(6);
    m_effectPresetCombo = new QComboBox(page);
    for (const EffectPresetUiOption& option : effectPresetUiOptions()) {
        m_effectPresetCombo->addItem(option.label, static_cast<int>(option.preset));
    }
    presetForm->addRow(QStringLiteral("Preset"), m_effectPresetCombo);

    m_effectRowsSpin = new QSpinBox(page);
    m_effectRowsSpin->setRange(1, 96);
    m_effectRowsSpin->setValue(32);
    m_effectRowsSpin->setToolTip(QStringLiteral("Rows, copies, or repeat steps."));
    presetForm->addRow(QStringLiteral("Copies"), m_effectRowsSpin);

    m_effectSpeedSpin = new QDoubleSpinBox(page);
    m_effectSpeedSpin->setRange(-8.0, 8.0);
    m_effectSpeedSpin->setDecimals(2);
    m_effectSpeedSpin->setSingleStep(0.25);
    m_effectSpeedSpin->setValue(1.0);
    presetForm->addRow(QStringLiteral("Speed"), m_effectSpeedSpin);

    m_effectScaleSpin = new QDoubleSpinBox(page);
    m_effectScaleSpin->setRange(0.1, 8.0);
    m_effectScaleSpin->setDecimals(2);
    m_effectScaleSpin->setSingleStep(0.1);
    m_effectScaleSpin->setValue(1.0);
    presetForm->addRow(QStringLiteral("Scale"), m_effectScaleSpin);

    m_effectAlternateDirectionCheck = new QCheckBox(QStringLiteral("Alternate direction"), page);
    m_effectAlternateDirectionCheck->setChecked(true);
    presetForm->addRow(QString(), m_effectAlternateDirectionCheck);

    m_effectSpeechSyncCheck = new QCheckBox(QStringLiteral("Synchronize motion with Speech Filter"), page);
    m_effectSpeechSyncCheck->setToolTip(
        QStringLiteral("Drive moving effect patterns from speech-filter timing so skipped gaps do not create visible jumps."));
    presetForm->addRow(QString(), m_effectSpeechSyncCheck);

    m_tilingPatternCombo = new QComboBox(page);
    for (const TilingPatternUiOption& option : tilingPatternUiOptions()) {
        m_tilingPatternCombo->addItem(option.label, static_cast<int>(option.pattern));
    }
    presetForm->addRow(QStringLiteral("Pattern"), m_tilingPatternCombo);

    m_tilingSpacingSpin = new QDoubleSpinBox(page);
    m_tilingSpacingSpin->setRange(0.1, 8.0);
    m_tilingSpacingSpin->setDecimals(2);
    m_tilingSpacingSpin->setSingleStep(0.1);
    m_tilingSpacingSpin->setValue(1.0);
    m_tilingSpacingSpin->setToolTip(QStringLiteral("Spacing multiplier between repeated source images."));
    presetForm->addRow(QStringLiteral("Tiling Spacing"), m_tilingSpacingSpin);

    m_tilingWrapCheck = new QCheckBox(QStringLiteral("Wrap across bounds"), page);
    m_tilingWrapCheck->setChecked(true);
    presetForm->addRow(QString(), m_tilingWrapCheck);
    presetSection.body->addLayout(presetForm);
    layout->addWidget(presetSection.container);

    // Info label
    auto *infoLabel = new QLabel(QStringLiteral(
        "Image presets render as repeated Vulkan draws from the clip texture. "
        "For SAM cutouts, enable the foreground layer on the masked clip and place effect images below it in the timeline."), page);
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

    m_maskEnabledCheck = new QCheckBox(QStringLiteral("Enable mask processing"), page);
    layout->addWidget(m_maskEnabledCheck);

    auto *sourceForm = new QFormLayout;
    auto *sourceRow = new QHBoxLayout;
    m_maskFramesDirEdit = new QLineEdit(page);
    m_maskFramesDirEdit->setClearButtonEnabled(true);
    m_maskFramesDirEdit->setPlaceholderText(QStringLiteral("SAM binary mask frames directory"));
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
    m_maskInvertCheck = new QCheckBox(QStringLiteral("Invert"), page);
    m_maskShowOnlyCheck = new QCheckBox(QStringLiteral("Show mask only"), page);
    m_maskShowOnlyCheck->setToolTip(QStringLiteral("Preview/export the processed mask instead of the source clip."));
    m_maskOpacitySpin = makeScalarSpin(0.0, 1.0, 1.0, 0.05);
    shapeForm->addRow(QStringLiteral("Dilate"), dilateControl.row);
    shapeForm->addRow(QStringLiteral("Erode"), erodeControl.row);
    shapeForm->addRow(QStringLiteral("Feather"), featherControl.row);
    shapeForm->addRow(QStringLiteral("Falloff"), m_maskShapeFeatherFalloffCombo);
    shapeForm->addRow(QStringLiteral("Power"), m_maskShapeFeatherPowerSpin);
    shapeForm->addRow(QStringLiteral("Blur"), blurControl.row);
    shapeForm->addRow(QStringLiteral("Invert"), m_maskInvertCheck);
    shapeForm->addRow(QStringLiteral("View"), m_maskShowOnlyCheck);
    shapeForm->addRow(QStringLiteral("Opacity"), m_maskOpacitySpin);
    layout->addLayout(shapeForm);

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
    m_transcriptShowSpeakerTitleCheckBox = new QCheckBox(QStringLiteral("Show Speaker Title"), settingsContainer);
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
    typographySection.body->addLayout(typographyForm);

    auto backgroundSection = createDisclosureSection(settingsContainer, QStringLiteral("Background & Effects"), false);
    auto* backgroundForm = makeSettingsForm();
    backgroundForm->addRow(QStringLiteral("Background"), m_transcriptBackgroundVisibleCheckBox);
    backgroundForm->addRow(QStringLiteral("Background Color"), m_transcriptBackgroundColorButton);
    backgroundForm->addRow(QStringLiteral("Background Opacity"), m_transcriptBackgroundOpacitySpin);
    backgroundForm->addRow(QStringLiteral("Corner Radius"), m_transcriptBackgroundCornerRadiusSpin);
    backgroundForm->addRow(QStringLiteral("Shadow"), m_transcriptShadowEnabledCheckBox);
    backgroundForm->addRow(QStringLiteral("Shadow Color"), m_transcriptShadowColorButton);
    backgroundForm->addRow(QStringLiteral("Shadow Opacity"), m_transcriptShadowOpacitySpin);
    backgroundForm->addRow(QStringLiteral("Shadow X"), m_transcriptShadowOffsetXSpin);
    backgroundForm->addRow(QStringLiteral("Shadow Y"), m_transcriptShadowOffsetYSpin);
    backgroundForm->addRow(QStringLiteral("Dilation"), m_transcriptOutlineEnabledCheckBox);
    backgroundForm->addRow(QStringLiteral("Dilation Color"), m_transcriptOutlineColorButton);
    backgroundForm->addRow(QStringLiteral("Dilation Size"), m_transcriptOutlineWidthSpin);
    backgroundForm->addRow(QStringLiteral("Dilation Opacity"), m_transcriptOutlineOpacitySpin);
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

QWidget *InspectorPane::buildSpeakersTab()
{
    auto *page = new QWidget;
    auto *layout = createTabLayout(page);
    layout->addWidget(createTabHeading(QStringLiteral("Speakers"), page));
    m_speakersSubtabs = nullptr;
    auto *scrollArea = new QScrollArea(page);
    scrollArea->setWidgetResizable(true);
    scrollArea->setFrameShape(QFrame::NoFrame);
    scrollArea->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    auto *content = new QWidget(scrollArea);
    content->setObjectName(QStringLiteral("speakers.combined_content"));
    content->setMinimumWidth(0);
    auto *mappingLayout = createTabLayout(content);
    auto createSectionFrame = [](QWidget *parent, const QString& objectName) {
        auto *frame = new QFrame(parent);
        frame->setObjectName(objectName);
        frame->setFrameShape(QFrame::StyledPanel);
        frame->setStyleSheet(QStringLiteral(
            "QFrame#%1 {"
            "  border: 1px solid #314459;"
            "  border-radius: 10px;"
            "  background: #112033;"
            "}"
            "QFrame#%1 QLabel { color: #d8e6f5; }")
                                 .arg(objectName));
        auto *frameLayout = new QVBoxLayout(frame);
        frameLayout->setContentsMargins(12, 12, 12, 12);
        frameLayout->setSpacing(8);
        return qMakePair(frame, frameLayout);
    };
    auto styleSectionTitle = [](QLabel *label) {
        label->setStyleSheet(QStringLiteral("font-weight: 600; color: #8fa3b8;"));
    };
    auto styleSectionHelp = [](QLabel *label) {
        label->setWordWrap(true);
        label->setStyleSheet(QStringLiteral("color: #8fa3b8; font-size: 11px;"));
    };
    m_speakersInspectorClipLabel = new QLabel(QStringLiteral("No transcript cut selected"), page);
    m_speakersInspectorDetailsLabel = new QLabel(QString(), page);
    m_speakersInspectorDetailsLabel->setWordWrap(true);

    m_speakersTable = new SpeakersTable(page);
    m_speakersTable->setObjectName(QStringLiteral("speakers.roster"));
    m_speakersTable->setColumnCount(7);
    m_speakersTable->setHorizontalHeaderLabels(
        {QStringLiteral("Avatar"),
         QStringLiteral("Speaker"),
         QStringLiteral("Organization"),
         QStringLiteral("X"),
         QStringLiteral("Y"),
         QStringLiteral("Assigned Tracks"),
         QStringLiteral("+")});
    m_speakersTable->setSelectionBehavior(QAbstractItemView::SelectRows);
    m_speakersTable->setSelectionMode(QAbstractItemView::ExtendedSelection);
    m_speakersTable->setEditTriggers(QAbstractItemView::DoubleClicked |
                                     QAbstractItemView::EditKeyPressed);
    m_speakersTable->setSizeAdjustPolicy(QAbstractScrollArea::AdjustIgnored);
    m_speakersTable->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
    m_speakersTable->setMinimumHeight(0);
    m_speakersTable->setVerticalScrollBarPolicy(Qt::ScrollBarAsNeeded);
    m_speakersTable->setHorizontalScrollBarPolicy(Qt::ScrollBarAsNeeded);
    m_speakersTable->setVerticalScrollMode(QAbstractItemView::ScrollPerPixel);
    m_speakersTable->setHorizontalScrollMode(QAbstractItemView::ScrollPerPixel);
    m_speakersTable->verticalHeader()->setVisible(false);
    m_speakersTable->horizontalHeader()->setStretchLastSection(false);
    m_speakersTable->horizontalHeader()->setSectionResizeMode(0, QHeaderView::ResizeToContents);
    m_speakersTable->horizontalHeader()->setSectionResizeMode(1, QHeaderView::ResizeToContents);
    m_speakersTable->horizontalHeader()->setSectionResizeMode(2, QHeaderView::ResizeToContents);
    m_speakersTable->horizontalHeader()->setSectionResizeMode(3, QHeaderView::ResizeToContents);
    m_speakersTable->horizontalHeader()->setSectionResizeMode(4, QHeaderView::ResizeToContents);
    m_speakersTable->horizontalHeader()->setSectionResizeMode(5, QHeaderView::ResizeToContents);
    m_speakersTable->horizontalHeader()->setSectionResizeMode(6, QHeaderView::ResizeToContents);
    m_speakerHideUnidentifiedCheckBox =
        new QCheckBox(QStringLiteral("Hide Unidentified"), page);
    m_speakerHideUnidentifiedCheckBox->setChecked(false);
    m_speakerHideUnidentifiedCheckBox->setToolTip(
        QStringLiteral("Hide speaker roster rows that do not have an identified profile name."));
    m_speakerShowContiguousSectionsCheckBox =
        new QCheckBox(QStringLiteral("Transcript Sections"), page);
    m_speakerShowContiguousSectionsCheckBox->setChecked(false);
    m_speakerShowContiguousSectionsCheckBox->hide();
    m_speakerShowContiguousSectionsCheckBox->setToolTip(
        QStringLiteral("Switch assignment rows from speakers to transcript-ordered contiguous sections."));
    m_speakerApplyTrackToAllMatchingSectionsCheckBox =
        new QCheckBox(QStringLiteral("Apply Track To All Matching Sections"), page);
    m_speakerApplyTrackToAllMatchingSectionsCheckBox->setChecked(false);
    m_speakerApplyTrackToAllMatchingSectionsCheckBox->hide();
    m_speakerApplyTrackToAllMatchingSectionsCheckBox->setToolTip(
        QStringLiteral("When enabled, a face-track click updates every matching contiguous transcript section at the track time. Off by default; otherwise only the active playhead section is updated."));
    m_speakerSectionMinimumWordsSpin = new QSpinBox(page);
    m_speakerSectionMinimumWordsSpin->setRange(0, 1000);
    m_speakerSectionMinimumWordsSpin->setValue(10);
    m_speakerSectionMinimumWordsSpin->setPrefix(QStringLiteral("Min words "));
    m_speakerSectionMinimumWordsSpin->setToolTip(
        QStringLiteral("Hide contiguous transcript sections below this word count in the table and playback."));
    m_speakerSectionMinimumWordsSpin->setMinimumWidth(128);
    m_speakerSectionMinimumWordsSpin->hide();
    m_speakerExportLongSectionsButton = new QPushButton(QStringLiteral("Export Sections"), page);
    m_speakerExportLongSectionsButton->setObjectName(QStringLiteral("speakers.export_long_sections"));
    m_speakerExportLongSectionsButton->setMinimumHeight(30);
    m_speakerExportLongSectionsButton->setSizePolicy(QSizePolicy::MinimumExpanding, QSizePolicy::Fixed);
    m_speakerExportLongSectionsButton->setEnabled(false);
    m_speakerCreateTitleClipsButton = new QPushButton(QStringLiteral("Apply lower-third fly-in"), page);
    m_speakerCreateTitleClipsButton->setObjectName(QStringLiteral("speakers.create_news_title_clips"));
    m_speakerCreateTitleClipsButton->setMinimumHeight(30);
    m_speakerCreateTitleClipsButton->setSizePolicy(QSizePolicy::MinimumExpanding, QSizePolicy::Fixed);
    m_speakerCreateTitleClipsButton->setEnabled(false);
    m_speakerCreateTitleClipsButton->setToolTip(
        QStringLiteral("Apply or refresh fly-in lower-third speaker titles on the selected source clip."));
    m_speakerOverlayCreateTitleClipsButton = new QCheckBox(QStringLiteral("Enable Speaker Title Fly-In"), page);
    m_speakerOverlayCreateTitleClipsButton->setObjectName(QStringLiteral("speakers.overlay_create_news_title_clips"));
    m_speakerOverlayCreateTitleClipsButton->setEnabled(false);
    m_speakerOverlayCreateTitleClipsButton->setToolTip(
        QStringLiteral("Automatically maintain animated speaker-title keyframes on the selected source clip. Turn off to remove them."));
    m_speakerOverlayFlyInStyleCombo = new QComboBox(page);
    m_speakerOverlayFlyInStyleCombo->addItem(QStringLiteral("Slide from left"), static_cast<int>(SpeakerTitleFlyInStyle::SlideFromLeft));
    m_speakerOverlayFlyInStyleCombo->addItem(QStringLiteral("Slide from right"), static_cast<int>(SpeakerTitleFlyInStyle::SlideFromRight));
    m_speakerOverlayFlyInStyleCombo->addItem(QStringLiteral("Rise from bottom"), static_cast<int>(SpeakerTitleFlyInStyle::RiseFromBottom));
    m_speakerOverlayFlyInStyleCombo->addItem(QStringLiteral("Drop from top"), static_cast<int>(SpeakerTitleFlyInStyle::DropFromTop));
    m_speakerOverlayFlyInStyleCombo->addItem(QStringLiteral("3D wrap around speaker"), static_cast<int>(SpeakerTitleFlyInStyle::WrapAroundSpeaker));
    m_speakerOverlayFlyInStyleCombo->setToolTip(QStringLiteral("Choose how the generated speaker title flies into frame."));
    auto makeFlyInSecondsSpin = [page](double value, double min, double max, const QString& tooltip) {
        auto *spin = new QDoubleSpinBox(page);
        spin->setRange(min, max);
        spin->setDecimals(2);
        spin->setSingleStep(0.05);
        spin->setSuffix(QStringLiteral(" s"));
        spin->setValue(value);
        spin->setKeyboardTracking(false);
        spin->setToolTip(tooltip);
        return spin;
    };
    m_speakerOverlayFlyInDelaySpin = makeFlyInSecondsSpin(
        0.35,
        0.0,
        3.0,
        QStringLiteral("Delay after each speaker change before the title starts."));
    m_speakerOverlayFlyInDurationSpin = makeFlyInSecondsSpin(
        3.0,
        1.0,
        12.0,
        QStringLiteral("Total length of each speaker title fly-in."));
    m_speakerOverlayFlyInTimeSpin = makeFlyInSecondsSpin(
        0.35,
        0.10,
        2.0,
        QStringLiteral("Time spent flying in and flying out."));
    auto makeWrapSpin = [page](double value, double min, double max, const QString& tooltip) {
        auto *spin = new QDoubleSpinBox(page);
        spin->setRange(min, max);
        spin->setDecimals(2);
        spin->setSingleStep(0.05);
        spin->setValue(value);
        spin->setKeyboardTracking(false);
        spin->setToolTip(tooltip);
        return spin;
    };
    m_speakerOverlayWrapRadiusSpin = makeWrapSpin(
        1.05,
        0.24,
        1.80,
        QStringLiteral("Horizontal orbit radius around the centered speaker mask."));
    m_speakerOverlayWrapDepthSpin = makeWrapSpin(
        0.70,
        0.0,
        1.0,
        QStringLiteral("How strongly the title shrinks and fades as it passes behind the centered speaker mask."));
    auto makeWrapAngleSpin = [page](double value, double min, double max, const QString& tooltip) {
        auto *spin = new QDoubleSpinBox(page);
        spin->setRange(min, max);
        spin->setDecimals(1);
        spin->setSingleStep(5.0);
        spin->setSuffix(QStringLiteral(" deg"));
        spin->setValue(value);
        spin->setKeyboardTracking(false);
        spin->setToolTip(tooltip);
        return spin;
    };
    m_speakerOverlayWrapStartAngleSpin = makeWrapAngleSpin(
        -110.0,
        -720.0,
        720.0,
        QStringLiteral("Start angle of the 3D orbit around the centered speaker mask."));
    m_speakerOverlayWrapEndAngleSpin = makeWrapAngleSpin(
        110.0,
        -720.0,
        720.0,
        QStringLiteral("End angle of the 3D orbit around the centered speaker mask."));
    m_speakerOverlayWrapPitchSpin = makeWrapAngleSpin(
        8.0,
        -80.0,
        80.0,
        QStringLiteral("Tilt the orbit path forward or backward in 3D space."));
    m_speakerOverlayWrapRollSpin = makeWrapAngleSpin(
        0.0,
        -180.0,
        180.0,
        QStringLiteral("Roll the projected orbit path clockwise or counter-clockwise."));
    m_speakerOverlayTitleFontSizeSpin = new QSpinBox(page);
    m_speakerOverlayTitleFontSizeSpin->setRange(12, 220);
    m_speakerOverlayTitleFontSizeSpin->setSingleStep(2);
    m_speakerOverlayTitleFontSizeSpin->setSuffix(QStringLiteral(" px"));
    m_speakerOverlayTitleFontSizeSpin->setValue(48);
    m_speakerOverlayTitleFontSizeSpin->setToolTip(
        QStringLiteral("Set the generated speaker title text size independently from the title box width."));
    m_speakerOverlayTitleBoxWidthSpin = new QSpinBox(page);
    m_speakerOverlayTitleBoxWidthSpin->setRange(0, 4000);
    m_speakerOverlayTitleBoxWidthSpin->setSingleStep(20);
    m_speakerOverlayTitleBoxWidthSpin->setSuffix(QStringLiteral(" px"));
    m_speakerOverlayTitleBoxWidthSpin->setSpecialValueText(QStringLiteral("Auto"));
    m_speakerOverlayTitleBoxWidthSpin->setValue(720);
    m_speakerOverlayTitleBoxWidthSpin->setToolTip(
        QStringLiteral("Set the generated speaker title background width. Auto fits the text."));
    auto makeTitleMaterialCombo = [page]() {
        auto *combo = new QComboBox(page);
        using MaterialStyle = TimelineClip::TitleKeyframe::MaterialStyle;
        combo->addItem(QStringLiteral("Solid"), static_cast<int>(MaterialStyle::Solid));
        combo->addItem(QStringLiteral("Neon glow"), static_cast<int>(MaterialStyle::Neon));
        combo->addItem(QStringLiteral("Diagonal stripes"), static_cast<int>(MaterialStyle::DiagonalStripes));
        combo->addItem(QStringLiteral("Grid"), static_cast<int>(MaterialStyle::Grid));
        combo->addItem(QStringLiteral("Image pattern"), static_cast<int>(MaterialStyle::ImagePattern));
        combo->setToolTip(QStringLiteral("Choose the procedural material style applied to generated speaker titles."));
        return combo;
    };
    m_speakerOverlayTitleTextMaterialCombo = makeTitleMaterialCombo();
    m_speakerOverlayTitleBorderMaterialCombo = makeTitleMaterialCombo();
    m_speakerOverlayTitleBorderMaterialCombo->setCurrentIndex(1);
    m_speakerOverlayTitleTextPatternPathEdit = new QLineEdit(page);
    m_speakerOverlayTitleTextPatternPathEdit->setPlaceholderText(QStringLiteral("Optional image path"));
    m_speakerOverlayTitleTextPatternPathEdit->setToolTip(
        QStringLiteral("Use this image as the title text material when Image pattern is selected."));
    m_speakerOverlayTitleBorderPatternPathEdit = new QLineEdit(page);
    m_speakerOverlayTitleBorderPatternPathEdit->setPlaceholderText(QStringLiteral("Optional image path"));
    m_speakerOverlayTitleBorderPatternPathEdit->setToolTip(
        QStringLiteral("Use this image as the border material when Image pattern is selected."));
    m_speakerOverlayTitlePatternScaleSpin = new QDoubleSpinBox(page);
    m_speakerOverlayTitlePatternScaleSpin->setRange(0.10, 8.0);
    m_speakerOverlayTitlePatternScaleSpin->setDecimals(2);
    m_speakerOverlayTitlePatternScaleSpin->setSingleStep(0.10);
    m_speakerOverlayTitlePatternScaleSpin->setValue(1.0);
    m_speakerOverlayTitlePatternScaleSpin->setKeyboardTracking(false);
    m_speakerOverlayTitlePatternScaleSpin->setToolTip(
        QStringLiteral("Scale procedural text and border patterns. Lower values make denser patterns."));
    m_speakerOverlayTitleExtrudeCheckBox = new QCheckBox(QStringLiteral("Extruded 3D Title Geometry"), page);
    m_speakerOverlayTitleExtrudeCheckBox->setChecked(false);
    m_speakerOverlayTitleExtrudeCheckBox->setToolTip(
        QStringLiteral("Mark generated wrap titles for the extruded 3D mesh pathway."));
    m_speakerOverlayTitleExtrudeDepthSpin = new QDoubleSpinBox(page);
    m_speakerOverlayTitleExtrudeDepthSpin->setRange(0.02, 2.0);
    m_speakerOverlayTitleExtrudeDepthSpin->setDecimals(2);
    m_speakerOverlayTitleExtrudeDepthSpin->setSingleStep(0.02);
    m_speakerOverlayTitleExtrudeDepthSpin->setValue(0.16);
    m_speakerOverlayTitleExtrudeDepthSpin->setKeyboardTracking(false);
    m_speakerOverlayTitleBevelScaleSpin = new QDoubleSpinBox(page);
    m_speakerOverlayTitleBevelScaleSpin->setRange(0.0, 2.0);
    m_speakerOverlayTitleBevelScaleSpin->setDecimals(2);
    m_speakerOverlayTitleBevelScaleSpin->setSingleStep(0.05);
    m_speakerOverlayTitleBevelScaleSpin->setValue(0.70);
    m_speakerOverlayTitleBevelScaleSpin->setKeyboardTracking(false);
    m_speakerShowCurrentSpeakerNameCheckBox =
        new QCheckBox(QStringLiteral("Show Current Speaker Name at Bottom"), page);
    m_speakerShowCurrentSpeakerNameCheckBox->setChecked(false);
    m_speakerShowCurrentSpeakerNameCheckBox->setToolTip(
        QStringLiteral("Draw the active transcript speaker name at the bottom of the preview."));
    m_speakerShowCurrentSpeakerOrganizationCheckBox =
        new QCheckBox(QStringLiteral("Show Current Speaker Organization at Bottom"), page);
    m_speakerShowCurrentSpeakerOrganizationCheckBox->setChecked(false);
    m_speakerShowCurrentSpeakerOrganizationCheckBox->setToolTip(
        QStringLiteral("Draw the active speaker organization at the bottom of the preview when it is stored in the speaker profile."));
    m_speakerCurrentSpeakerNameTextSizeSpin = new QSpinBox(page);
    m_speakerCurrentSpeakerNameTextSizeSpin->setRange(25, 300);
    m_speakerCurrentSpeakerNameTextSizeSpin->setSingleStep(5);
    m_speakerCurrentSpeakerNameTextSizeSpin->setSuffix(QStringLiteral("%"));
    m_speakerCurrentSpeakerNameTextSizeSpin->setValue(100);
    m_speakerCurrentSpeakerNameTextSizeSpin->setToolTip(
        QStringLiteral("Scale the active speaker name drawn at the bottom of the preview."));
    m_speakerCurrentSpeakerOrganizationTextSizeSpin = new QSpinBox(page);
    m_speakerCurrentSpeakerOrganizationTextSizeSpin->setRange(25, 300);
    m_speakerCurrentSpeakerOrganizationTextSizeSpin->setSingleStep(5);
    m_speakerCurrentSpeakerOrganizationTextSizeSpin->setSuffix(QStringLiteral("%"));
    m_speakerCurrentSpeakerOrganizationTextSizeSpin->setValue(100);
    m_speakerCurrentSpeakerOrganizationTextSizeSpin->setToolTip(
        QStringLiteral("Scale the active speaker organization drawn at the bottom of the preview."));
    m_speakerCurrentSpeakerNameYPositionSpin = new QSpinBox(page);
    m_speakerCurrentSpeakerNameYPositionSpin->setRange(0, 100);
    m_speakerCurrentSpeakerNameYPositionSpin->setSingleStep(1);
    m_speakerCurrentSpeakerNameYPositionSpin->setSuffix(QStringLiteral("%"));
    m_speakerCurrentSpeakerNameYPositionSpin->setValue(86);
    m_speakerCurrentSpeakerNameYPositionSpin->setToolTip(
        QStringLiteral("Set the active speaker name vertical position in the preview. 0% is top; 100% is bottom."));
    m_speakerCurrentSpeakerOrganizationYPositionSpin = new QSpinBox(page);
    m_speakerCurrentSpeakerOrganizationYPositionSpin->setRange(0, 100);
    m_speakerCurrentSpeakerOrganizationYPositionSpin->setSingleStep(1);
    m_speakerCurrentSpeakerOrganizationYPositionSpin->setSuffix(QStringLiteral("%"));
    m_speakerCurrentSpeakerOrganizationYPositionSpin->setValue(93);
    m_speakerCurrentSpeakerOrganizationYPositionSpin->setToolTip(
        QStringLiteral("Set the active speaker organization vertical position in the preview. 0% is top; 100% is bottom."));
    auto makeSpeakerColorButton = [page](const QString& color, const QString& tooltip) {
        auto* button = new QPushButton(color, page);
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
    m_speakerCurrentSpeakerNameColorButton = makeSpeakerColorButton(
        QStringLiteral("#f4f8fc"),
        QStringLiteral("Set the active speaker name text color."));
    m_speakerCurrentSpeakerOrganizationColorButton = makeSpeakerColorButton(
        QStringLiteral("#b9d0e5"),
        QStringLiteral("Set the active speaker organization text color."));
    m_speakerCurrentSpeakerBackgroundColorButton = makeSpeakerColorButton(
        QStringLiteral("#080d14"),
        QStringLiteral("Set the active speaker label background color."));
    m_speakerCurrentSpeakerBackgroundVisibleCheckBox =
        new QCheckBox(QStringLiteral("Show Background Box"), page);
    m_speakerCurrentSpeakerBackgroundVisibleCheckBox->setChecked(true);
    m_speakerCurrentSpeakerBackgroundVisibleCheckBox->setToolTip(
        QStringLiteral("Turn off the speaker-title background and border while keeping the title text and text shadow."));
    m_speakerCurrentSpeakerBackgroundOpacitySpin = new QSpinBox(page);
    m_speakerCurrentSpeakerBackgroundOpacitySpin->setRange(0, 100);
    m_speakerCurrentSpeakerBackgroundOpacitySpin->setSuffix(QStringLiteral("%"));
    m_speakerCurrentSpeakerBackgroundOpacitySpin->setValue(75);
    m_speakerCurrentSpeakerBackgroundOpacitySpin->setToolTip(
        QStringLiteral("Set the active speaker label background opacity."));
    m_speakerCurrentSpeakerBorderColorButton = makeSpeakerColorButton(
        QStringLiteral("#e1ecf7"),
        QStringLiteral("Set the active speaker label border color."));
    m_speakerCurrentSpeakerBorderOpacitySpin = new QSpinBox(page);
    m_speakerCurrentSpeakerBorderOpacitySpin->setRange(0, 100);
    m_speakerCurrentSpeakerBorderOpacitySpin->setSuffix(QStringLiteral("%"));
    m_speakerCurrentSpeakerBorderOpacitySpin->setValue(47);
    m_speakerCurrentSpeakerBorderOpacitySpin->setToolTip(
        QStringLiteral("Set the active speaker label border opacity."));
    m_speakerCurrentSpeakerBackgroundRadiusSpin = new QSpinBox(page);
    m_speakerCurrentSpeakerBackgroundRadiusSpin->setRange(0, 128);
    m_speakerCurrentSpeakerBackgroundRadiusSpin->setSuffix(QStringLiteral(" px"));
    m_speakerCurrentSpeakerBackgroundRadiusSpin->setValue(14);
    m_speakerCurrentSpeakerBackgroundRadiusSpin->setToolTip(
        QStringLiteral("Set the active speaker label background corner radius."));
    m_speakerCurrentSpeakerBorderWidthSpin = new QSpinBox(page);
    m_speakerCurrentSpeakerBorderWidthSpin->setRange(0, 16);
    m_speakerCurrentSpeakerBorderWidthSpin->setSuffix(QStringLiteral(" px"));
    m_speakerCurrentSpeakerBorderWidthSpin->setValue(1);
    m_speakerCurrentSpeakerBorderWidthSpin->setToolTip(
        QStringLiteral("Set the active speaker label border width."));
    m_speakerCurrentSpeakerShadowCheckBox = new QCheckBox(QStringLiteral("Speaker Label Shadow"), page);
    m_speakerCurrentSpeakerShadowCheckBox->setChecked(true);
    m_speakerCurrentSpeakerShadowCheckBox->setToolTip(
        QStringLiteral("Draw the active speaker label text shadow."));
    m_speakerCurrentSpeakerShadowColorButton = makeSpeakerColorButton(
        QStringLiteral("#000000"),
        QStringLiteral("Set the active speaker label shadow color."));
    m_speakerCurrentSpeakerShadowOpacitySpin = new QSpinBox(page);
    m_speakerCurrentSpeakerShadowOpacitySpin->setRange(0, 100);
    m_speakerCurrentSpeakerShadowOpacitySpin->setSuffix(QStringLiteral("%"));
    m_speakerCurrentSpeakerShadowOpacitySpin->setValue(75);
    m_speakerCurrentSpeakerShadowOpacitySpin->setToolTip(
        QStringLiteral("Set the active speaker label shadow opacity."));
    m_speakerSectionsTable = new QTableWidget(page);
    m_speakerSectionsTable->setColumnCount(8);
    m_speakerSectionsTable->setHorizontalHeaderLabels(
        {QStringLiteral("Avatar"),
         QStringLiteral("#"),
         QStringLiteral("Speaker"),
         QStringLiteral("Range"),
         QStringLiteral("Tracks"),
         QStringLiteral("Rotation"),
         QStringLiteral("Words"),
         QStringLiteral("Transcript")});
    m_speakerSectionsTable->setSelectionBehavior(QAbstractItemView::SelectRows);
    m_speakerSectionsTable->setSelectionMode(QAbstractItemView::SingleSelection);
    m_speakerSectionsTable->setEditTriggers(QAbstractItemView::EditKeyPressed);
    m_speakerSectionsTable->setSizeAdjustPolicy(QAbstractScrollArea::AdjustIgnored);
    m_speakerSectionsTable->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
    m_speakerSectionsTable->setMinimumHeight(0);
    m_speakerSectionsTable->setVerticalScrollBarPolicy(Qt::ScrollBarAsNeeded);
    m_speakerSectionsTable->setHorizontalScrollBarPolicy(Qt::ScrollBarAsNeeded);
    m_speakerSectionsTable->setVerticalScrollMode(QAbstractItemView::ScrollPerPixel);
    m_speakerSectionsTable->setHorizontalScrollMode(QAbstractItemView::ScrollPerPixel);
    m_speakerSectionsTable->setWordWrap(false);
    m_speakerSectionsTable->setTextElideMode(Qt::ElideRight);
    m_speakerSectionsTable->verticalHeader()->setVisible(false);
    QHeaderView* sectionsHeader = m_speakerSectionsTable->horizontalHeader();
    sectionsHeader->setStretchLastSection(false);
    sectionsHeader->setMinimumSectionSize(36);
    sectionsHeader->setSectionResizeMode(QHeaderView::Interactive);
    sectionsHeader->setSectionResizeMode(0, QHeaderView::ResizeToContents);
    sectionsHeader->setSectionResizeMode(1, QHeaderView::ResizeToContents);
    sectionsHeader->setSectionResizeMode(6, QHeaderView::ResizeToContents);
    sectionsHeader->setSectionResizeMode(7, QHeaderView::Stretch);
    m_speakerSectionsTable->setColumnWidth(2, 124);
    m_speakerSectionsTable->setColumnWidth(3, 116);
    m_speakerSectionsTable->setColumnWidth(4, 96);
    m_speakerSectionsTable->setColumnWidth(5, 96);
    m_speakerSectionsTable->horizontalHeaderItem(7)->setToolTip(
        QStringLiteral("Transcript excerpt; use the filter below to search visible sections."));
    m_speakerSectionsTable->setAccessibleName(QStringLiteral("Transcript sections"));

    auto *selectedSpeakerTitle = new QLabel(QStringLiteral("Selected Speaker"), page);
    styleSectionTitle(selectedSpeakerTitle);
    m_selectedSpeakerIdLabel = new QLabel(QStringLiteral("No speaker selected"), page);
    m_selectedSpeakerIdLabel->setObjectName(QStringLiteral("speakers.selected_speaker"));
    m_selectedSpeakerNameEdit = new QLineEdit(page);
    m_selectedSpeakerNameEdit->setObjectName(QStringLiteral("speakers.selected_speaker.name"));
    m_selectedSpeakerNameEdit->setPlaceholderText(QStringLiteral("Speaker name"));
    m_selectedSpeakerNameEdit->setClearButtonEnabled(true);
    m_selectedSpeakerNameEdit->setEnabled(false);
    m_selectedSpeakerOrganizationEdit = new QLineEdit(page);
    m_selectedSpeakerOrganizationEdit->setObjectName(QStringLiteral("speakers.selected_speaker.organization"));
    m_selectedSpeakerOrganizationEdit->setPlaceholderText(QStringLiteral("Organization"));
    m_selectedSpeakerOrganizationEdit->setClearButtonEnabled(true);
    m_selectedSpeakerOrganizationEdit->setEnabled(false);
    m_selectedSpeakerLogoPathEdit = new QLineEdit(page);
    m_selectedSpeakerLogoPathEdit->setObjectName(QStringLiteral("speakers.selected_speaker.logo_path"));
    m_selectedSpeakerLogoPathEdit->setPlaceholderText(QStringLiteral("Logo image path"));
    m_selectedSpeakerLogoPathEdit->setClearButtonEnabled(true);
    m_selectedSpeakerLogoPathEdit->setEnabled(false);
    auto makeSelectedSpeakerColorEdit = [page](const QString& objectName, const QString& placeholder) {
        auto *edit = new QLineEdit(page);
        edit->setObjectName(objectName);
        edit->setPlaceholderText(placeholder);
        edit->setClearButtonEnabled(true);
        edit->setMaxLength(9);
        edit->setEnabled(false);
        return edit;
    };
    m_selectedSpeakerPrimaryColorEdit = makeSelectedSpeakerColorEdit(
        QStringLiteral("speakers.selected_speaker.primary_color"),
        QStringLiteral("#f7fbff"));
    m_selectedSpeakerSecondaryColorEdit = makeSelectedSpeakerColorEdit(
        QStringLiteral("speakers.selected_speaker.secondary_color"),
        QStringLiteral("#07111d"));
    m_selectedSpeakerAccentColorEdit = makeSelectedSpeakerColorEdit(
        QStringLiteral("speakers.selected_speaker.accent_color"),
        QStringLiteral("#56c7ff"));
    auto *selectedSpeakerProfileForm = new QFormLayout;
    selectedSpeakerProfileForm->setContentsMargins(0, 0, 0, 0);
    selectedSpeakerProfileForm->setHorizontalSpacing(6);
    selectedSpeakerProfileForm->setVerticalSpacing(6);
    selectedSpeakerProfileForm->addRow(QStringLiteral("Name"), m_selectedSpeakerNameEdit);
    selectedSpeakerProfileForm->addRow(QStringLiteral("Organization"), m_selectedSpeakerOrganizationEdit);
    selectedSpeakerProfileForm->addRow(QStringLiteral("Logo"), m_selectedSpeakerLogoPathEdit);
    selectedSpeakerProfileForm->addRow(QStringLiteral("Primary Color"), m_selectedSpeakerPrimaryColorEdit);
    selectedSpeakerProfileForm->addRow(QStringLiteral("Secondary Color"), m_selectedSpeakerSecondaryColorEdit);
    selectedSpeakerProfileForm->addRow(QStringLiteral("Accent Color"), m_selectedSpeakerAccentColorEdit);
    auto *selectedFaceDetectionsTitle = new QLabel(QStringLiteral("Assigned Tracks"), page);
    styleSectionTitle(selectedFaceDetectionsTitle);
    m_selectedSpeakerFaceDetectionsList = new QListWidget(page);
    m_selectedSpeakerFaceDetectionsList->setObjectName(QStringLiteral("speakers.assigned_tracks"));
    m_selectedSpeakerFaceDetectionsList->setViewMode(QListView::IconMode);
    m_selectedSpeakerFaceDetectionsList->setFlow(QListView::LeftToRight);
    m_selectedSpeakerFaceDetectionsList->setWrapping(true);
    m_selectedSpeakerFaceDetectionsList->setResizeMode(QListView::Adjust);
    m_selectedSpeakerFaceDetectionsList->setMovement(QListView::Static);
    m_selectedSpeakerFaceDetectionsList->setSpacing(8);
    m_selectedSpeakerFaceDetectionsList->setIconSize(QSize(72, 72));
    m_selectedSpeakerFaceDetectionsList->setMinimumHeight(96);
    m_selectedSpeakerFaceDetectionsList->setMaximumHeight(220);
    m_selectedSpeakerFaceDetectionsList->setVerticalScrollMode(QAbstractItemView::ScrollPerPixel);
    m_selectedSpeakerFaceDetectionsList->setHorizontalScrollMode(QAbstractItemView::ScrollPerPixel);
    m_selectedSpeakerFaceDetectionsList->setStyleSheet(QStringLiteral(
        "QListWidget { border: 1px solid #314459; border-radius: 8px; background: #142234; color: #d8e6f5; padding: 6px; }"
        "QListWidget::item { padding: 4px; }"
        "QListWidget::item:selected { background: #1d324a; border: 1px solid #4ea1ff; }"));
    auto *playheadFaceDetectionsTitle = new QLabel(QStringLiteral("Tracks At Playhead"), page);
    styleSectionTitle(playheadFaceDetectionsTitle);
    auto *playheadFaceDetectionsHeaderRow = new QHBoxLayout;
    playheadFaceDetectionsHeaderRow->setContentsMargins(0, 0, 0, 0);
    playheadFaceDetectionsHeaderRow->setSpacing(6);
    m_speakerShowPlayheadFaceDetectionsCheckBox =
        new QCheckBox(QStringLiteral("Show"), page);
    m_speakerShowPlayheadFaceDetectionsCheckBox->setObjectName(
        QStringLiteral("speakers.show_playhead_tracks"));
    m_speakerShowPlayheadFaceDetectionsCheckBox->setChecked(true);
    m_speakerShowPlayheadFaceDetectionsCheckBox->setToolTip(
        QStringLiteral("Show or hide the Tracks At Playhead picker without affecting continuity overlays."));
    m_speakerPlayheadFaceDetectionsList = new QListWidget(page);
    m_speakerPlayheadFaceDetectionsList->setObjectName(QStringLiteral("speakers.playhead_tracks"));
    m_speakerPlayheadFaceDetectionsList->setViewMode(QListView::IconMode);
    m_speakerPlayheadFaceDetectionsList->setFlow(QListView::LeftToRight);
    m_speakerPlayheadFaceDetectionsList->setWrapping(true);
    m_speakerPlayheadFaceDetectionsList->setResizeMode(QListView::Adjust);
    m_speakerPlayheadFaceDetectionsList->setMovement(QListView::Static);
    m_speakerPlayheadFaceDetectionsList->setSelectionMode(QAbstractItemView::ExtendedSelection);
    m_speakerPlayheadFaceDetectionsList->setSpacing(8);
    m_speakerPlayheadFaceDetectionsList->setIconSize(QSize(72, 72));
    m_speakerPlayheadFaceDetectionsList->setMinimumHeight(96);
    m_speakerPlayheadFaceDetectionsList->setMaximumHeight(220);
    m_speakerPlayheadFaceDetectionsList->setVerticalScrollMode(QAbstractItemView::ScrollPerPixel);
    m_speakerPlayheadFaceDetectionsList->setHorizontalScrollMode(QAbstractItemView::ScrollPerPixel);
    m_speakerPlayheadFaceDetectionsList->setStyleSheet(QStringLiteral(
        "QListWidget { border: 1px solid #314459; border-radius: 8px; background: #142234; color: #d8e6f5; padding: 6px; }"
        "QListWidget::item { padding: 4px; }"
        "QListWidget::item:selected { background: #1d324a; border: 1px solid #4ea1ff; }"));
    auto *selectedActionsRow = new QHBoxLayout;
    m_selectedSpeakerPreviousSentenceButton = new QPushButton(QStringLiteral("Previous Sentence"), page);
    m_selectedSpeakerNextSentenceButton = new QPushButton(QStringLiteral("Next Sentence"), page);
    m_selectedSpeakerNextSectionButton = new QPushButton(QStringLiteral("Next Section"), page);
    m_selectedSpeakerRandomSentenceButton = new QPushButton(QStringLiteral("Random Sentence"), page);
    for (QPushButton* button :
         {m_selectedSpeakerPreviousSentenceButton,
          m_selectedSpeakerNextSentenceButton,
          m_selectedSpeakerNextSectionButton,
          m_selectedSpeakerRandomSentenceButton}) {
        button->setMinimumHeight(30);
        button->setSizePolicy(QSizePolicy::MinimumExpanding, QSizePolicy::Fixed);
    }
    selectedActionsRow->addWidget(m_selectedSpeakerPreviousSentenceButton);
    selectedActionsRow->addWidget(m_selectedSpeakerNextSentenceButton);
    selectedActionsRow->addWidget(m_selectedSpeakerNextSectionButton);
    selectedActionsRow->addWidget(m_selectedSpeakerRandomSentenceButton);
    m_speakerAiFindNamesButton = new QPushButton(QStringLiteral("Mine Transcript (AI)"), page);
    m_speakerPrecropFacesButton = new QPushButton(QStringLiteral("Add Selected Tracks"), page);
    m_speakerAiFindOrganizationsButton = new QPushButton(QStringLiteral("Find Organizations"), page);
    m_speakerAiCleanAssignmentsButton = new QPushButton(QStringLiteral("Clean Assignments"), page);
    m_speakerPrecropFacesButton->setObjectName(QStringLiteral("speakers.assign_facedetections"));
    m_speakerAiFindNamesButton->setObjectName(QStringLiteral("speakers.mine_transcript_ai"));
    m_speakerAiFindOrganizationsButton->setObjectName(QStringLiteral("speakers.find_organizations_ai"));
    m_speakerAiCleanAssignmentsButton->setObjectName(QStringLiteral("speakers.clean_assignments_ai"));
    for (QPushButton* button :
         {m_speakerPrecropFacesButton, m_speakerAiFindNamesButton, m_speakerAiFindOrganizationsButton, m_speakerAiCleanAssignmentsButton}) {
        button->setMinimumHeight(30);
        button->setSizePolicy(QSizePolicy::MinimumExpanding, QSizePolicy::Fixed);
    }
    playheadFaceDetectionsHeaderRow->addWidget(playheadFaceDetectionsTitle);
    playheadFaceDetectionsHeaderRow->addWidget(m_speakerShowPlayheadFaceDetectionsCheckBox);
    playheadFaceDetectionsHeaderRow->addStretch(1);
    playheadFaceDetectionsHeaderRow->addWidget(m_speakerPrecropFacesButton);

    auto *currentSentenceTitle = new QLabel(QStringLiteral("Current Speaker Sentence"), page);
    styleSectionTitle(currentSentenceTitle);
    m_speakerCurrentSentenceLabel = new QLabel(QStringLiteral("Select a speaker to view sentence context."), page);
    m_speakerCurrentSentenceLabel->setWordWrap(true);
    m_speakerCurrentSentenceLabel->setMinimumHeight(48);
    m_speakerCurrentSentenceLabel->setStyleSheet(QStringLiteral(
        "QLabel { border: 1px solid #314459; border-radius: 8px; background: #142234; color: #d8e6f5; padding: 8px; }"));

    m_speakerTranscriptTable = new QTableWidget(page);
    m_speakerTranscriptTable->setObjectName(QStringLiteral("speakers.embedded_transcript"));
    m_speakerTranscriptTable->setColumnCount(5);
    m_speakerTranscriptTable->setHorizontalHeaderLabels(
        {QStringLiteral("Source Start"),
         QStringLiteral("Source End"),
         QStringLiteral("Speaker"),
         QStringLiteral("Text"),
         QStringLiteral("Edits")});
    m_speakerTranscriptTable->setSelectionBehavior(QAbstractItemView::SelectRows);
    m_speakerTranscriptTable->setSelectionMode(QAbstractItemView::ExtendedSelection);
    m_speakerTranscriptTable->setEditTriggers(QAbstractItemView::DoubleClicked |
                                              QAbstractItemView::EditKeyPressed);
    m_speakerTranscriptTable->verticalHeader()->setVisible(false);
    m_speakerTranscriptTable->horizontalHeader()->setStretchLastSection(true);
    m_speakerTranscriptTable->horizontalHeader()->setSectionResizeMode(0, QHeaderView::ResizeToContents);
    m_speakerTranscriptTable->horizontalHeader()->setSectionResizeMode(1, QHeaderView::ResizeToContents);
    m_speakerTranscriptTable->horizontalHeader()->setSectionResizeMode(2, QHeaderView::ResizeToContents);
    m_speakerTranscriptTable->horizontalHeader()->setSectionResizeMode(3, QHeaderView::Stretch);
    m_speakerTranscriptTable->horizontalHeader()->setSectionResizeMode(4, QHeaderView::ResizeToContents);
    m_speakerTranscriptTable->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
    m_speakerTranscriptTable->setMinimumHeight(240);

    m_speakerTrackingStatusLabel = new QLabel(QString(), page);
    m_speakerTrackingStatusLabel->setObjectName(QStringLiteral("speakers.tracking_status"));
    m_speakerTrackingStatusLabel->setWordWrap(true);
    m_speakerTrackingStatusLabel->setStyleSheet(QStringLiteral("color: #8fa3b8; font-size: 11px;"));

    auto *debugTitle = new QLabel(QStringLiteral("Debug Artefacts"), page);
    styleSectionTitle(debugTitle);

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

    mappingLayout->addWidget(m_speakersInspectorClipLabel);
    mappingLayout->addWidget(m_speakersInspectorDetailsLabel);
    auto *selectedSpeakerPopupFrame = new QFrame(page, Qt::Popup);
    selectedSpeakerPopupFrame->setObjectName(QStringLiteral("speakers.selected_speaker_popup"));
    selectedSpeakerPopupFrame->setFrameShape(QFrame::StyledPanel);
    selectedSpeakerPopupFrame->setMinimumWidth(320);
    selectedSpeakerPopupFrame->setMaximumWidth(460);
    selectedSpeakerPopupFrame->setStyleSheet(QStringLiteral(
        "QFrame#speakers\\.selected_speaker_popup {"
        "  border: 1px solid #35506c;"
        "  border-radius: 8px;"
        "  background: #0f1824;"
        "  padding: 8px;"
        "}"));
    m_selectedSpeakerPopup = selectedSpeakerPopupFrame;
    auto *selectedSpeakerPopupLayout = new QVBoxLayout(selectedSpeakerPopupFrame);
    selectedSpeakerPopupLayout->setContentsMargins(10, 10, 10, 10);
    selectedSpeakerPopupLayout->setSpacing(8);
    auto *selectedSpeakerPage = new QWidget(selectedSpeakerPopupFrame);
    auto *selectedSpeakerPageLayout = new QVBoxLayout(selectedSpeakerPage);
    selectedSpeakerPageLayout->setContentsMargins(0, 0, 0, 0);
    selectedSpeakerPageLayout->setSpacing(6);
    selectedSpeakerPageLayout->addWidget(selectedSpeakerTitle);
    selectedSpeakerPageLayout->addWidget(m_selectedSpeakerIdLabel);
    selectedSpeakerPageLayout->addLayout(selectedSpeakerProfileForm);
    selectedSpeakerPageLayout->addWidget(selectedFaceDetectionsTitle);
    selectedSpeakerPageLayout->addWidget(m_selectedSpeakerFaceDetectionsList);
    selectedSpeakerPageLayout->addLayout(playheadFaceDetectionsHeaderRow);
    selectedSpeakerPageLayout->addWidget(m_speakerPlayheadFaceDetectionsList);
    selectedSpeakerPageLayout->addLayout(selectedActionsRow);
    selectedSpeakerPageLayout->addWidget(currentSentenceTitle);
    selectedSpeakerPageLayout->addWidget(m_speakerCurrentSentenceLabel);
    selectedSpeakerPageLayout->addWidget(m_speakerTrackingStatusLabel);
    auto *selectedSpeakerScroll = new QScrollArea(selectedSpeakerPopupFrame);
    selectedSpeakerScroll->setWidgetResizable(true);
    selectedSpeakerScroll->setFrameShape(QFrame::NoFrame);
    selectedSpeakerScroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    selectedSpeakerScroll->setWidget(selectedSpeakerPage);
    selectedSpeakerPopupLayout->addWidget(selectedSpeakerScroll);
    m_speakerTranscriptTable->hide();
    auto *mappingContentRow = new QHBoxLayout;
    mappingContentRow->setContentsMargins(0, 0, 0, 0);
    mappingContentRow->setSpacing(8);
    auto *speakerListPanel = new QWidget(content);
    auto *speakerListLayout = new QVBoxLayout(speakerListPanel);
    speakerListLayout->setContentsMargins(0, 0, 0, 0);
    speakerListLayout->setSpacing(6);
    m_speakersSubtabs = new QTabWidget(speakerListPanel);
    auto *speakerWorkTabs = m_speakersSubtabs;
    speakerWorkTabs->setObjectName(QStringLiteral("speakers.work_tabs"));
    speakerWorkTabs->setDocumentMode(true);
    speakerWorkTabs->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
    speakerWorkTabs->tabBar()->setUsesScrollButtons(true);
    speakerWorkTabs->tabBar()->setElideMode(Qt::ElideRight);
    speakerWorkTabs->setAccessibleName(QStringLiteral("Speaker workflow pages"));

    auto *speakerRosterPage = new QWidget(speakerWorkTabs);
    auto *speakerRosterLayout = new QVBoxLayout(speakerRosterPage);
    speakerRosterLayout->setContentsMargins(0, 0, 0, 0);
    speakerRosterLayout->setSpacing(6);
    auto *speakerRosterControlsGroup = new QGroupBox(QStringLiteral("Roster Options"), speakerRosterPage);
    speakerRosterControlsGroup->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Fixed);
    auto *speakerRosterControlsLayout = new QHBoxLayout(speakerRosterControlsGroup);
    speakerRosterControlsLayout->setContentsMargins(8, 6, 8, 6);
    speakerRosterControlsLayout->setSpacing(8);
    speakerRosterControlsLayout->addWidget(m_speakerHideUnidentifiedCheckBox);
    speakerRosterControlsLayout->addStretch(1);
    speakerRosterLayout->addWidget(m_speakersTable, 1);
    speakerRosterLayout->addWidget(speakerRosterControlsGroup);

    auto *speakerSectionsPage = new QWidget(speakerWorkTabs);
    auto *speakerSectionsLayout = new QVBoxLayout(speakerSectionsPage);
    speakerSectionsLayout->setContentsMargins(0, 0, 0, 0);
    speakerSectionsLayout->setSpacing(6);
    auto *speakerSectionsControlsGroup = new QGroupBox(QStringLiteral("Section Controls"), speakerSectionsPage);
    speakerSectionsControlsGroup->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Fixed);
    auto *speakerSectionsControlsLayout = new QGridLayout(speakerSectionsControlsGroup);
    speakerSectionsControlsLayout->setContentsMargins(8, 6, 8, 6);
    speakerSectionsControlsLayout->setHorizontalSpacing(8);
    speakerSectionsControlsLayout->setVerticalSpacing(4);
    m_speakerApplyTrackToAllMatchingSectionsCheckBox->show();
    m_speakerSectionMinimumWordsSpin->show();
    auto *speakerSectionsFilterRow = new QWidget(speakerSectionsPage);
    speakerSectionsFilterRow->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Fixed);
    auto *speakerSectionsFilterLayout = new QHBoxLayout(speakerSectionsFilterRow);
    speakerSectionsFilterLayout->setContentsMargins(0, 0, 0, 0);
    speakerSectionsFilterLayout->setSpacing(8);
    auto *speakerSectionsSearchEdit = new QLineEdit(speakerSectionsFilterRow);
    speakerSectionsSearchEdit->setObjectName(QStringLiteral("speakers.sections.search"));
    speakerSectionsSearchEdit->setPlaceholderText(QStringLiteral("Filter sections…"));
    speakerSectionsSearchEdit->setClearButtonEnabled(true);
    speakerSectionsSearchEdit->setAccessibleName(QStringLiteral("Filter transcript sections"));
    speakerSectionsSearchEdit->setMinimumWidth(96);
    auto *speakerSectionsSummaryLabel = new QLabel(QStringLiteral("0 sections"), speakerSectionsFilterRow);
    speakerSectionsSummaryLabel->setObjectName(QStringLiteral("speakers.sections.summary"));
    speakerSectionsSummaryLabel->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
    speakerSectionsSummaryLabel->setMinimumWidth(64);
    speakerSectionsSummaryLabel->setStyleSheet(QStringLiteral("color: #8fa3b8; font-size: 11px;"));
    speakerSectionsFilterLayout->addWidget(speakerSectionsSearchEdit, 1);
    speakerSectionsFilterLayout->addWidget(speakerSectionsSummaryLabel);
    speakerSectionsLayout->addWidget(speakerSectionsFilterRow);
    speakerSectionsLayout->addWidget(m_speakerSectionsTable, 1);
    speakerSectionsControlsLayout->addWidget(m_speakerSectionMinimumWordsSpin, 0, 0);
    speakerSectionsControlsLayout->addWidget(m_speakerExportLongSectionsButton, 0, 1);
    speakerSectionsControlsLayout->addWidget(
        m_speakerApplyTrackToAllMatchingSectionsCheckBox, 1, 0, 1, 2);
    speakerSectionsControlsLayout->setColumnStretch(0, 1);
    speakerSectionsControlsLayout->setColumnStretch(1, 1);
    speakerSectionsLayout->addWidget(speakerSectionsControlsGroup);
    speakerSectionsLayout->setStretch(0, 0);
    speakerSectionsLayout->setStretch(1, 1);
    speakerSectionsLayout->setStretch(2, 0);
    const auto filterSpeakerSections =
        [table = m_speakerSectionsTable, speakerSectionsSearchEdit, speakerSectionsSummaryLabel]() {
            const QString needle = speakerSectionsSearchEdit->text().trimmed();
            int visibleRows = 0;
            const int totalRows = table->rowCount();
            for (int row = 0; row < totalRows; ++row) {
                bool matches = needle.isEmpty();
                for (int column = 0; !matches && column < table->columnCount(); ++column) {
                    const QTableWidgetItem* item = table->item(row, column);
                    matches = item && item->text().contains(needle, Qt::CaseInsensitive);
                }
                table->setRowHidden(row, !matches);
                visibleRows += matches ? 1 : 0;
            }
            speakerSectionsSummaryLabel->setText(
                needle.isEmpty()
                    ? QStringLiteral("%1 sections").arg(totalRows)
                    : QStringLiteral("%1 of %2").arg(visibleRows).arg(totalRows));
        };
    connect(speakerSectionsSearchEdit, &QLineEdit::textChanged, page,
            [filterSpeakerSections]() { filterSpeakerSections(); });
    connect(m_speakerSectionsTable->model(), &QAbstractItemModel::modelReset, page,
            [filterSpeakerSections]() { filterSpeakerSections(); });
    connect(m_speakerSectionsTable->model(), &QAbstractItemModel::rowsInserted, page,
            [filterSpeakerSections](const QModelIndex&, int, int) { filterSpeakerSections(); });

    auto *speakerAiPage = new QWidget(speakerWorkTabs);
    auto *speakerAiLayout = new QVBoxLayout(speakerAiPage);
    speakerAiLayout->setContentsMargins(0, 0, 0, 0);
    speakerAiLayout->setSpacing(6);
    auto *speakerAiGroup = new QGroupBox(QStringLiteral("Transcript Cleanup"), speakerAiPage);
    auto *speakerAiGroupLayout = new QGridLayout(speakerAiGroup);
    speakerAiGroupLayout->setContentsMargins(8, 6, 8, 6);
    speakerAiGroupLayout->setHorizontalSpacing(6);
    speakerAiGroupLayout->setVerticalSpacing(6);
    auto *speakerAiHelp = new QLabel(
        QStringLiteral("Mine transcript speaker names, find organizations, then clean assignments."),
        speakerAiGroup);
    styleSectionHelp(speakerAiHelp);
    speakerAiGroupLayout->addWidget(speakerAiHelp, 0, 0, 1, 3);
    speakerAiGroupLayout->addWidget(m_speakerAiFindNamesButton, 1, 0);
    speakerAiGroupLayout->addWidget(m_speakerAiFindOrganizationsButton, 1, 1);
    speakerAiGroupLayout->addWidget(m_speakerAiCleanAssignmentsButton, 1, 2);
    speakerAiGroupLayout->setColumnStretch(0, 1);
    speakerAiGroupLayout->setColumnStretch(1, 1);
    speakerAiGroupLayout->setColumnStretch(2, 1);
    speakerAiLayout->addWidget(speakerAiGroup);
    speakerAiLayout->addStretch(1);

    auto *speakerTitlePage = new QWidget(speakerWorkTabs);
    auto *speakerTitleLayout = new QVBoxLayout(speakerTitlePage);
    speakerTitleLayout->setContentsMargins(0, 0, 0, 0);
    speakerTitleLayout->setSpacing(6);
    auto *speakerTitleTabs = new QTabWidget(speakerTitlePage);
    speakerTitleTabs->setObjectName(QStringLiteral("speakers.title_tabs"));
    speakerTitleTabs->setDocumentMode(true);
    speakerTitleTabs->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);

    auto *speakerFlyInPage = new QWidget(speakerTitleTabs);
    auto *speakerFlyInLayout = new QVBoxLayout(speakerFlyInPage);
    speakerFlyInLayout->setContentsMargins(0, 0, 0, 0);
    speakerFlyInLayout->setSpacing(6);

    auto *speakerLabelPage = new QWidget(speakerTitleTabs);
    auto *speakerLabelLayout = new QVBoxLayout(speakerLabelPage);
    speakerLabelLayout->setContentsMargins(0, 0, 0, 0);
    speakerLabelLayout->setSpacing(6);

    auto *speakerOverlayVisibilityGroup = new QGroupBox(QStringLiteral("Visible Fields"), speakerLabelPage);
    auto *speakerOverlayVisibilityLayout = new QVBoxLayout(speakerOverlayVisibilityGroup);
    speakerOverlayVisibilityLayout->setContentsMargins(8, 6, 8, 6);
    speakerOverlayVisibilityLayout->setSpacing(4);
    speakerOverlayVisibilityLayout->addWidget(m_speakerShowCurrentSpeakerNameCheckBox);
    speakerOverlayVisibilityLayout->addWidget(m_speakerShowCurrentSpeakerOrganizationCheckBox);
    speakerLabelLayout->addWidget(speakerOverlayVisibilityGroup);
    speakerLabelLayout->addStretch(1);

    auto *speakerOverlayFlyInGroup = new QGroupBox(QStringLiteral("Speaker Title Fly-In"), speakerFlyInPage);
    auto *speakerOverlayFlyInLayout = new QVBoxLayout(speakerOverlayFlyInGroup);
    speakerOverlayFlyInLayout->setContentsMargins(8, 6, 8, 6);
    speakerOverlayFlyInLayout->setSpacing(6);
    auto *speakerOverlayFlyInHelp = new QLabel(
        QStringLiteral("Automatically show an animated lower-third when each speaker is introduced."),
        speakerOverlayFlyInGroup);
    styleSectionHelp(speakerOverlayFlyInHelp);
    speakerOverlayFlyInLayout->addWidget(speakerOverlayFlyInHelp);
    auto *speakerOverlayFlyInForm = new QFormLayout;
    speakerOverlayFlyInForm->setContentsMargins(0, 0, 0, 0);
    speakerOverlayFlyInForm->setHorizontalSpacing(8);
    speakerOverlayFlyInForm->setVerticalSpacing(4);
    speakerOverlayFlyInForm->addRow(QStringLiteral("Fly Option"), m_speakerOverlayFlyInStyleCombo);
    speakerOverlayFlyInForm->addRow(QStringLiteral("Delay"), m_speakerOverlayFlyInDelaySpin);
    speakerOverlayFlyInForm->addRow(QStringLiteral("Duration"), m_speakerOverlayFlyInDurationSpin);
    speakerOverlayFlyInForm->addRow(QStringLiteral("Fly Time"), m_speakerOverlayFlyInTimeSpin);
    auto *wrapRadiusLabel = new QLabel(QStringLiteral("Wrap Radius"), speakerOverlayFlyInGroup);
    auto *wrapDepthLabel = new QLabel(QStringLiteral("Wrap Depth"), speakerOverlayFlyInGroup);
    auto *wrapStartAngleLabel = new QLabel(QStringLiteral("Start Angle"), speakerOverlayFlyInGroup);
    auto *wrapEndAngleLabel = new QLabel(QStringLiteral("End Angle"), speakerOverlayFlyInGroup);
    auto *wrapPitchLabel = new QLabel(QStringLiteral("Pitch"), speakerOverlayFlyInGroup);
    auto *wrapRollLabel = new QLabel(QStringLiteral("Roll"), speakerOverlayFlyInGroup);
    speakerOverlayFlyInForm->addRow(wrapRadiusLabel, m_speakerOverlayWrapRadiusSpin);
    speakerOverlayFlyInForm->addRow(wrapDepthLabel, m_speakerOverlayWrapDepthSpin);
    speakerOverlayFlyInForm->addRow(wrapStartAngleLabel, m_speakerOverlayWrapStartAngleSpin);
    speakerOverlayFlyInForm->addRow(wrapEndAngleLabel, m_speakerOverlayWrapEndAngleSpin);
    speakerOverlayFlyInForm->addRow(wrapPitchLabel, m_speakerOverlayWrapPitchSpin);
    speakerOverlayFlyInForm->addRow(wrapRollLabel, m_speakerOverlayWrapRollSpin);
    auto syncWrapControls = [this,
                             wrapRadiusLabel,
                             wrapDepthLabel,
                             wrapStartAngleLabel,
                             wrapEndAngleLabel,
                             wrapPitchLabel,
                             wrapRollLabel]() {
        const bool wrapSelected =
            m_speakerOverlayFlyInStyleCombo &&
            m_speakerOverlayFlyInStyleCombo->currentData().toInt() ==
                static_cast<int>(SpeakerTitleFlyInStyle::WrapAroundSpeaker);
        wrapRadiusLabel->setVisible(wrapSelected);
        wrapDepthLabel->setVisible(wrapSelected);
        wrapStartAngleLabel->setVisible(wrapSelected);
        wrapEndAngleLabel->setVisible(wrapSelected);
        wrapPitchLabel->setVisible(wrapSelected);
        wrapRollLabel->setVisible(wrapSelected);
        if (m_speakerOverlayWrapRadiusSpin) {
            m_speakerOverlayWrapRadiusSpin->setVisible(wrapSelected);
        }
        if (m_speakerOverlayWrapDepthSpin) {
            m_speakerOverlayWrapDepthSpin->setVisible(wrapSelected);
        }
        if (m_speakerOverlayWrapStartAngleSpin) {
            m_speakerOverlayWrapStartAngleSpin->setVisible(wrapSelected);
        }
        if (m_speakerOverlayWrapEndAngleSpin) {
            m_speakerOverlayWrapEndAngleSpin->setVisible(wrapSelected);
        }
        if (m_speakerOverlayWrapPitchSpin) {
            m_speakerOverlayWrapPitchSpin->setVisible(wrapSelected);
        }
        if (m_speakerOverlayWrapRollSpin) {
            m_speakerOverlayWrapRollSpin->setVisible(wrapSelected);
        }
    };
    connect(m_speakerOverlayFlyInStyleCombo,
            qOverload<int>(&QComboBox::currentIndexChanged),
            speakerOverlayFlyInGroup,
            [syncWrapControls](int) { syncWrapControls(); });
    syncWrapControls();
    speakerOverlayFlyInLayout->addWidget(m_speakerOverlayCreateTitleClipsButton);
    speakerOverlayFlyInLayout->addLayout(speakerOverlayFlyInForm);
    speakerFlyInLayout->addWidget(speakerOverlayFlyInGroup);
    speakerFlyInLayout->addStretch(1);

    auto *speakerStylePage = new QWidget(speakerTitleTabs);
    auto *speakerStyleLayout = new QVBoxLayout(speakerStylePage);
    speakerStyleLayout->setContentsMargins(0, 0, 0, 0);
    speakerStyleLayout->setSpacing(6);

    auto *speakerOverlayStyleGroup = new QGroupBox(QStringLiteral("Style"), speakerStylePage);
    auto *speakerOverlayStyleLayout = new QVBoxLayout(speakerOverlayStyleGroup);
    speakerOverlayStyleLayout->setContentsMargins(8, 6, 8, 6);
    speakerOverlayStyleLayout->setSpacing(4);
    auto *currentSpeakerTextSizeLayout = new QFormLayout;
    currentSpeakerTextSizeLayout->setContentsMargins(0, 0, 0, 0);
    currentSpeakerTextSizeLayout->setHorizontalSpacing(8);
    currentSpeakerTextSizeLayout->setVerticalSpacing(4);
    currentSpeakerTextSizeLayout->addRow(QStringLiteral("Title Font Size"), m_speakerOverlayTitleFontSizeSpin);
    currentSpeakerTextSizeLayout->addRow(QStringLiteral("Title Box Width"), m_speakerOverlayTitleBoxWidthSpin);
    currentSpeakerTextSizeLayout->addRow(QStringLiteral("Title Material"), m_speakerOverlayTitleTextMaterialCombo);
    currentSpeakerTextSizeLayout->addRow(QStringLiteral("Border Material"), m_speakerOverlayTitleBorderMaterialCombo);
    currentSpeakerTextSizeLayout->addRow(QStringLiteral("Text Pattern Image"), m_speakerOverlayTitleTextPatternPathEdit);
    currentSpeakerTextSizeLayout->addRow(QStringLiteral("Border Pattern Image"), m_speakerOverlayTitleBorderPatternPathEdit);
    currentSpeakerTextSizeLayout->addRow(QStringLiteral("Pattern Scale"), m_speakerOverlayTitlePatternScaleSpin);
    currentSpeakerTextSizeLayout->addRow(m_speakerOverlayTitleExtrudeCheckBox);
    currentSpeakerTextSizeLayout->addRow(QStringLiteral("Extrude Depth"), m_speakerOverlayTitleExtrudeDepthSpin);
    currentSpeakerTextSizeLayout->addRow(QStringLiteral("Bevel Scale"), m_speakerOverlayTitleBevelScaleSpin);
    currentSpeakerTextSizeLayout->addRow(QStringLiteral("Name Size"), m_speakerCurrentSpeakerNameTextSizeSpin);
    currentSpeakerTextSizeLayout->addRow(QStringLiteral("Organization Size"), m_speakerCurrentSpeakerOrganizationTextSizeSpin);
    currentSpeakerTextSizeLayout->addRow(QStringLiteral("Name Y Position"), m_speakerCurrentSpeakerNameYPositionSpin);
    currentSpeakerTextSizeLayout->addRow(QStringLiteral("Organization Y Position"), m_speakerCurrentSpeakerOrganizationYPositionSpin);
    currentSpeakerTextSizeLayout->addRow(QStringLiteral("Name Color"), m_speakerCurrentSpeakerNameColorButton);
    currentSpeakerTextSizeLayout->addRow(QStringLiteral("Organization Color"), m_speakerCurrentSpeakerOrganizationColorButton);
    currentSpeakerTextSizeLayout->addRow(m_speakerCurrentSpeakerBackgroundVisibleCheckBox);
    currentSpeakerTextSizeLayout->addRow(QStringLiteral("Background Color"), m_speakerCurrentSpeakerBackgroundColorButton);
    currentSpeakerTextSizeLayout->addRow(QStringLiteral("Background Opacity"), m_speakerCurrentSpeakerBackgroundOpacitySpin);
    currentSpeakerTextSizeLayout->addRow(QStringLiteral("Border Color"), m_speakerCurrentSpeakerBorderColorButton);
    currentSpeakerTextSizeLayout->addRow(QStringLiteral("Border Opacity"), m_speakerCurrentSpeakerBorderOpacitySpin);
    currentSpeakerTextSizeLayout->addRow(QStringLiteral("Corner Radius"), m_speakerCurrentSpeakerBackgroundRadiusSpin);
    currentSpeakerTextSizeLayout->addRow(QStringLiteral("Border Width"), m_speakerCurrentSpeakerBorderWidthSpin);
    currentSpeakerTextSizeLayout->addRow(m_speakerCurrentSpeakerShadowCheckBox);
    currentSpeakerTextSizeLayout->addRow(QStringLiteral("Shadow Color"), m_speakerCurrentSpeakerShadowColorButton);
    currentSpeakerTextSizeLayout->addRow(QStringLiteral("Shadow Opacity"), m_speakerCurrentSpeakerShadowOpacitySpin);
    speakerOverlayStyleLayout->addLayout(currentSpeakerTextSizeLayout);
    speakerStyleLayout->addWidget(speakerOverlayStyleGroup);
    speakerStyleLayout->addStretch(1);
    speakerTitleTabs->addTab(speakerFlyInPage, QStringLiteral("Fly-In"));
    speakerTitleTabs->addTab(speakerLabelPage, QStringLiteral("Label"));
    speakerTitleTabs->addTab(speakerStylePage, QStringLiteral("Style"));
    speakerTitleLayout->addWidget(speakerTitleTabs, 1);

    auto *speakerContinuityPage = buildSpeakersContinuityTab(speakerWorkTabs);

    auto *speakerDebugPage = new QWidget(speakerWorkTabs);
    auto *speakerDebugLayout = createTabLayout(speakerDebugPage);
    speakerDebugPage->setObjectName(QStringLiteral("speakers_debug_section"));
    speakerDebugLayout->addWidget(debugTitle);
    speakerDebugLayout->addWidget(m_speakerDebugCaptureCheckBox);
    speakerDebugLayout->addLayout(debugActionsRow);
    speakerDebugLayout->addWidget(m_speakerDebugStatusLabel);
    speakerDebugLayout->addStretch(1);

    const int rosterTabIndex = speakerWorkTabs->addTab(speakerRosterPage, QStringLiteral("Roster"));
    const int sectionsTabIndex = speakerWorkTabs->addTab(speakerSectionsPage, QStringLiteral("Sections"));
    speakerWorkTabs->addTab(speakerAiPage, QStringLiteral("AI Cleanup"));
    speakerWorkTabs->addTab(speakerTitlePage, QStringLiteral("Speaker Title"));
    speakerWorkTabs->addTab(speakerContinuityPage, QStringLiteral("Continuity Tracks"));
    speakerWorkTabs->addTab(speakerDebugPage, QStringLiteral("Debug"));
    const auto syncSectionModeFromWorkTab =
        [this, rosterTabIndex, sectionsTabIndex](int index) {
                if (!m_speakerShowContiguousSectionsCheckBox) {
                    return;
                }
                if (index == rosterTabIndex) {
                    m_speakerShowContiguousSectionsCheckBox->setChecked(false);
                } else if (index == sectionsTabIndex) {
                    m_speakerShowContiguousSectionsCheckBox->setChecked(true);
                }
        };
    connect(speakerWorkTabs,
            &QTabWidget::currentChanged,
            page,
            syncSectionModeFromWorkTab);
    connect(m_speakerShowContiguousSectionsCheckBox,
            &QCheckBox::toggled,
            speakerWorkTabs,
            [speakerWorkTabs, rosterTabIndex, sectionsTabIndex](bool showSections) {
                const int desiredIndex = showSections ? sectionsTabIndex : rosterTabIndex;
                if (speakerWorkTabs->currentIndex() == rosterTabIndex ||
                    speakerWorkTabs->currentIndex() == sectionsTabIndex) {
                    speakerWorkTabs->setCurrentIndex(desiredIndex);
                }
            });
    // addTab() selects the first page before currentChanged is connected.
    // Initialize the mode from the page that is actually active.
    syncSectionModeFromWorkTab(speakerWorkTabs->currentIndex());
    speakerListLayout->addWidget(speakerWorkTabs, 1);
    mappingContentRow->addWidget(speakerListPanel, 1);

    auto identitySection = createSectionFrame(content, QStringLiteral("speakers_identities_section"));
    identitySection.second->addLayout(mappingContentRow, 1);

    mappingLayout->addWidget(identitySection.first);
    mappingLayout->addStretch(1);

    scrollArea->setWidget(content);
    layout->addWidget(scrollArea, 1);
    return page;
}


QWidget *InspectorPane::buildPane()
{
    auto *pane = new QFrame;
    pane->setMinimumWidth(0);
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
    m_inspectorTabs->setObjectName(QStringLiteral("tabs.inspector"));
    m_inspectorTabs->addTab(buildGradingTab(), QStringLiteral("Grade"));
    m_inspectorTabs->addTab(buildOpacityTab(), QStringLiteral("Opacity"));
    m_inspectorTabs->addTab(buildEffectsTab(), QStringLiteral("Effects"));
    m_inspectorTabs->addTab(buildMasksTab(), QStringLiteral("Masks"));
    m_inspectorTabs->addTab(buildCorrectionsTab(), QStringLiteral("Corrections"));
    m_inspectorTabs->addTab(buildTitlesTab(), QStringLiteral("Titles"));
    m_inspectorTabs->addTab(buildSyncTab(), QStringLiteral("Sync"));
    m_inspectorTabs->addTab(buildKeyframesTab(), QStringLiteral("Transform"));
    m_inspectorTabs->addTab(buildTranscriptTab(), QStringLiteral("Transcript"));
    m_inspectorTabs->addTab(buildSpeakersTab(), QStringLiteral("Speakers"));
    m_inspectorTabs->addTab(buildClipTab(), QStringLiteral("Properties"));
    m_inspectorTabs->addTab(buildClipsTab(), QStringLiteral("Clips"));
    m_inspectorTabs->addTab(buildHistoryTab(), QStringLiteral("History"));
    m_inspectorTabs->addTab(buildTracksTab(), QStringLiteral("Tracks"));
    m_inspectorTabs->addTab(buildPreviewTab(), QStringLiteral("Preview"));
    m_inspectorTabs->addTab(buildAudioTab(), QStringLiteral("Audio"));
    m_inspectorTabs->addTab(buildProcessingJobsTab(), QStringLiteral("Jobs"));
    m_inspectorTabs->addTab(buildAiTab(), QStringLiteral("AI Assist"));
    m_inspectorTabs->addTab(buildAccessTab(), QStringLiteral("Access"));
    m_inspectorTabs->addTab(buildOutputTab(), QStringLiteral("Output"));
    m_inspectorTabs->addTab(buildPipelineTab(), QStringLiteral("Pipeline"));
    m_inspectorTabs->addTab(buildProfileTab(), QStringLiteral("System"));
    m_inspectorTabs->addTab(buildProjectsTab(), QStringLiteral("Projects"));
    m_inspectorTabs->addTab(buildPreferencesTab(), QStringLiteral("Preferences"));
    configureInspectorTabs();

    layout->addWidget(m_inspectorTabs, 1);
    return pane;
}
