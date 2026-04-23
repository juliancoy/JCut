#pragma once

#include "keyframe_tab_base.h"

#include "editor_shared.h"

#include <QCheckBox>
#include <QDoubleSpinBox>
#include <QFontComboBox>
#include <QLabel>
#include <QLineEdit>
#include <QObject>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QSet>
#include <QTableWidget>

#include <functional>

class TitlesTab final : public KeyframeTabBase
{
    Q_OBJECT

public:
    struct Widgets {
        QLabel *titlesInspectorClipLabel = nullptr;
        QLabel *titlesInspectorDetailsLabel = nullptr;
        QTableWidget *titleKeyframeTable = nullptr;
        QPlainTextEdit *titleTextEdit = nullptr;
        QDoubleSpinBox *titleXSpin = nullptr;
        QDoubleSpinBox *titleYSpin = nullptr;
        QDoubleSpinBox *titleFontSizeSpin = nullptr;
        QDoubleSpinBox *titleOpacitySpin = nullptr;
        QFontComboBox *titleFontCombo = nullptr;
        QCheckBox *titleBoldCheck = nullptr;
        QCheckBox *titleItalicCheck = nullptr;
        QPushButton *titleColorButton = nullptr;
        QCheckBox *titleShadowEnabledCheck = nullptr;
        QPushButton *titleShadowColorButton = nullptr;
        QDoubleSpinBox *titleShadowOpacitySpin = nullptr;
        QDoubleSpinBox *titleShadowOffsetXSpin = nullptr;
        QDoubleSpinBox *titleShadowOffsetYSpin = nullptr;
        QCheckBox *titleWindowEnabledCheck = nullptr;
        QPushButton *titleWindowColorButton = nullptr;
        QDoubleSpinBox *titleWindowOpacitySpin = nullptr;
        QDoubleSpinBox *titleWindowPaddingSpin = nullptr;
        QCheckBox *titleWindowFrameEnabledCheck = nullptr;
        QPushButton *titleWindowFrameColorButton = nullptr;
        QDoubleSpinBox *titleWindowFrameOpacitySpin = nullptr;
        QDoubleSpinBox *titleWindowFrameWidthSpin = nullptr;
        QDoubleSpinBox *titleWindowFrameGapSpin = nullptr;
        QCheckBox *titleAutoScrollCheck = nullptr;
        QPushButton *addTitleKeyframeButton = nullptr;
        QPushButton *removeTitleKeyframeButton = nullptr;
        QPushButton *centerHorizontalButton = nullptr;
        QPushButton *centerVerticalButton = nullptr;
    };

    struct Dependencies : public KeyframeTabBase::Dependencies
    {
    };

    TitlesTab(const Widgets &widgets, const Dependencies &deps, QObject *parent = nullptr);

    void wire();
    void refresh();
    void syncTableToPlayhead();
    void upsertKeyframeAtPlayhead();
    void removeSelectedKeyframes();
    void centerHorizontal();
    void centerVertical();

private:
    struct TitleKeyframeDisplay {
        int64_t frame = 0;
        QString text;
        double translationX = 0.0;
        double translationY = 0.0;
        double fontSize = 48.0;
        double opacity = 1.0;
        QString fontFamily = kDefaultFontFamily;
        bool bold = true;
        bool italic = false;
        QColor color = QColor(QStringLiteral("#ffffff"));
        bool dropShadowEnabled = true;
        QColor dropShadowColor = QColor(QStringLiteral("#000000"));
        double dropShadowOpacity = 0.6;
        double dropShadowOffsetX = 2.0;
        double dropShadowOffsetY = 2.0;
        bool windowEnabled = false;
        QColor windowColor = QColor(QStringLiteral("#000000"));
        double windowOpacity = 0.35;
        double windowPadding = 16.0;
        bool windowFrameEnabled = false;
        QColor windowFrameColor = QColor(QStringLiteral("#ffffffff"));
        double windowFrameOpacity = 1.0;
        double windowFrameWidth = 2.0;
        double windowFrameGap = 4.0;
        bool linearInterpolation = true;
    };

    void populateTable(const TimelineClip &clip);
    TitleKeyframeDisplay evaluateDisplayedTitle(const TimelineClip &clip, int64_t localFrame) const;
    void updateWidgetsFromKeyframe(const TitleKeyframeDisplay &display);
    int64_t preferredEditFrame(const TimelineClip& clip) const;
    void applyKeyframeFromInspector();
    void applyKeyframeFromInspectorLive();
    int selectedKeyframeIndex(const TimelineClip &clip) const;
    int nearestKeyframeIndex(const TimelineClip &clip, int64_t localFrame) const;
    bool hasRemovableKeyframeSelection() const;

    void onTableItemChanged(QTableWidgetItem *item);
    void onTableSelectionChanged();
    void onTableItemClicked(QTableWidgetItem *item);
    void onTableCustomContextMenu(const QPoint &pos);
    void removeSelectedKeyframesFromCurrentTable() override { removeSelectedKeyframes(); }
    
    // Event filter for Ctrl+Enter handling
    bool eventFilter(QObject *watched, QEvent *event) override;

    Widgets m_widgets;
    QColor m_selectedTitleColor = QColor(QStringLiteral("#ffffff"));
    QColor m_selectedShadowColor = QColor(QStringLiteral("#000000"));
    QColor m_selectedWindowColor = QColor(QStringLiteral("#000000"));
    QColor m_selectedWindowFrameColor = QColor(QStringLiteral("#ffffffff"));
};
