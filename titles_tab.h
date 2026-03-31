#pragma once

#include "keyframe_tab_base.h"

#include "editor_shared.h"

#include <QCheckBox>
#include <QDoubleSpinBox>
#include <QFontComboBox>
#include <QLabel>
#include <QLineEdit>
#include <QObject>
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
        QLineEdit *titleTextEdit = nullptr;
        QDoubleSpinBox *titleXSpin = nullptr;
        QDoubleSpinBox *titleYSpin = nullptr;
        QDoubleSpinBox *titleFontSizeSpin = nullptr;
        QDoubleSpinBox *titleOpacitySpin = nullptr;
        QFontComboBox *titleFontCombo = nullptr;
        QCheckBox *titleBoldCheck = nullptr;
        QCheckBox *titleItalicCheck = nullptr;
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
        bool linearInterpolation = true;
    };

    void populateTable(const TimelineClip &clip);
    TitleKeyframeDisplay evaluateDisplayedTitle(const TimelineClip &clip, int64_t localFrame) const;
    void updateWidgetsFromKeyframe(const TitleKeyframeDisplay &display);
    void applyKeyframeFromInspector();
    int selectedKeyframeIndex(const TimelineClip &clip) const;
    int nearestKeyframeIndex(const TimelineClip &clip, int64_t localFrame) const;
    bool hasRemovableKeyframeSelection() const;

    void onTableItemChanged(QTableWidgetItem *item);
    void onTableSelectionChanged();
    void onTableItemClicked(QTableWidgetItem *item);
    void onTableCustomContextMenu(const QPoint &pos);
    void removeSelectedKeyframesFromCurrentTable() override { removeSelectedKeyframes(); }

    Widgets m_widgets;
};
