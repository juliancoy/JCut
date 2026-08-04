#include "inspector_pane.h"
#include "audio_engine.h"
#include "editor_effect_presets.h"
#include "editor_shared_core.h"
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
#include <QStandardItemModel>
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
