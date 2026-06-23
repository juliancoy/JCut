#pragma once

#include <QObject>
#include <QLabel>
#include <QLineEdit>
#include <QDoubleSpinBox>
#include <QCheckBox>
#include <QPushButton>
#include <functional>

#include "editor_shared.h"

class MaskTab : public QObject
{
    Q_OBJECT

public:
    struct Widgets
    {
        QLabel* clipLabel = nullptr;
        QCheckBox* enabledCheck = nullptr;
        QLineEdit* framesDirEdit = nullptr;
        QPushButton* browseButton = nullptr;
        QDoubleSpinBox* featherSpin = nullptr;
        QDoubleSpinBox* dilateSpin = nullptr;
        QDoubleSpinBox* erodeSpin = nullptr;
        QDoubleSpinBox* blurSpin = nullptr;
        QCheckBox* invertCheck = nullptr;
        QDoubleSpinBox* opacitySpin = nullptr;
        QCheckBox* gradeEnabledCheck = nullptr;
        QDoubleSpinBox* gradeBrightnessSpin = nullptr;
        QDoubleSpinBox* gradeContrastSpin = nullptr;
        QDoubleSpinBox* gradeSaturationSpin = nullptr;
        QCheckBox* shadowEnabledCheck = nullptr;
        QDoubleSpinBox* shadowRadiusSpin = nullptr;
        QDoubleSpinBox* shadowOffsetXSpin = nullptr;
        QDoubleSpinBox* shadowOffsetYSpin = nullptr;
        QDoubleSpinBox* shadowOpacitySpin = nullptr;
    };

    struct Dependencies
    {
        std::function<const TimelineClip*()> getSelectedClip;
        std::function<bool(const QString&, const std::function<void(TimelineClip&)>&)> updateClipById;
        std::function<void()> setPreviewTimelineClips;
        std::function<void()> refreshInspector;
        std::function<void()> scheduleSaveState;
        std::function<void()> pushHistorySnapshot;
        std::function<bool(const TimelineClip&)> clipHasVisuals;
        std::function<QString(QWidget*, const QString&)> chooseMaskDirectory;
    };

    explicit MaskTab(const Widgets& widgets, const Dependencies& deps, QObject* parent = nullptr);

    void wire();
    void refresh();
    void apply(bool pushHistory = false);

private:
    void setControlsEnabled(bool enabled);

    Widgets m_widgets;
    Dependencies m_deps;
    bool m_updating = false;
};
