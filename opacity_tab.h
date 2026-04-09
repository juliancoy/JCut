#pragma once

#include "keyframe_tab_base.h"

#include <QObject>
#include <QTableWidget>
#include <QLabel>
#include <QDoubleSpinBox>
#include <QCheckBox>
#include <QPushButton>
#include <QTimer>

#include "editor_shared.h"

class OpacityTab : public KeyframeTabBase
{
    Q_OBJECT

public:
    struct Widgets
    {
        QLabel* opacityPathLabel = nullptr;
        QDoubleSpinBox* opacitySpin = nullptr;
        QTableWidget* opacityKeyframeTable = nullptr;
        QCheckBox* opacityAutoScrollCheckBox = nullptr;
        QCheckBox* opacityFollowCurrentCheckBox = nullptr;
        QPushButton* opacityKeyAtPlayheadButton = nullptr;
        QPushButton* opacityFadeInButton = nullptr;
        QPushButton* opacityFadeOutButton = nullptr;
        QDoubleSpinBox* opacityFadeDurationSpin = nullptr;
    };

    struct Dependencies : public KeyframeTabBase::Dependencies
    {
    };

    explicit OpacityTab(const Widgets& widgets, const Dependencies& deps, QObject* parent = nullptr);
    ~OpacityTab() override = default;

    void wire();
    void refresh();
    void applyOpacityFromInspector(bool pushHistory = false);
    void upsertKeyframeAtPlayhead();
    void fadeInFromPlayhead();
    void fadeOutFromPlayhead();
    void removeSelectedKeyframes();
    void syncTableToPlayhead();

private slots:
    void onOpacityChanged(double value);
    void onOpacityEditingFinished();
    void onAutoScrollToggled(bool checked);
    void onFollowCurrentToggled(bool checked);
    void onKeyAtPlayheadClicked();
    void onFadeInClicked();
    void onFadeOutClicked();
    void onTableSelectionChanged();
    void onTableItemChanged(QTableWidgetItem* item);
    void onTableItemClicked(QTableWidgetItem* item);
    void onTableCustomContextMenu(const QPoint& pos);
    void removeSelectedKeyframesFromCurrentTable() override { removeSelectedKeyframes(); }

private:
    struct OpacityKeyframeDisplay
    {
        int64_t frame = 0;
        double opacity = 1.0;
        bool linearInterpolation = true;
    };

    QString interpolationLabel(bool linearInterpolation) const;
    QString nextInterpolationLabel(const QString& text) const;
    bool parseInterpolationText(const QString& text, bool* linearInterpolationOut) const;
    int selectedKeyframeIndex(const TimelineClip& clip) const;
    QList<int64_t> selectedKeyframeFramesForClip(const TimelineClip& clip) const;
    int nearestKeyframeIndex(const TimelineClip& clip) const;
    bool hasRemovableKeyframeSelection(const TimelineClip& clip) const;
    OpacityKeyframeDisplay evaluateDisplayedOpacity(const TimelineClip& clip, int64_t localFrame) const;
    void updateSpinBoxesFromKeyframe(const OpacityKeyframeDisplay& keyframe);
    void populateTable(const TimelineClip& clip);
    void applyOpacityFadeFromPlayhead(bool fadeIn);

    Widgets m_widgets;
    QTimer m_deferredSeekTimer;
    int64_t m_pendingSeekTimelineFrame = -1;
};
