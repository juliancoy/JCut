#pragma once

#include <QObject>
#include <QLabel>
#include <QLineEdit>
#include <QDoubleSpinBox>
#include <QCheckBox>
#include <QPushButton>
#include <functional>

#include "editor_shared.h"

class QComboBox;

class MaskTab : public QObject
{
    Q_OBJECT

public:
    struct Widgets
    {
        QLabel* clipLabel = nullptr;
        QCheckBox* enabledCheck = nullptr;
        QLineEdit* framesDirEdit = nullptr;
        QComboBox* sidecarCombo = nullptr;
        QPushButton* browseButton = nullptr;
        QPushButton* newPromptButton = nullptr;
        QDoubleSpinBox* featherSpin = nullptr;
        QComboBox* featherFalloffCombo = nullptr;
        QDoubleSpinBox* featherPowerSpin = nullptr;
        QDoubleSpinBox* dilateSpin = nullptr;
        QDoubleSpinBox* erodeSpin = nullptr;
        QDoubleSpinBox* blurSpin = nullptr;
        QCheckBox* invertCheck = nullptr;
        QCheckBox* showOnlyCheck = nullptr;
        QDoubleSpinBox* opacitySpin = nullptr;
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
        std::function<void(const QString&)> generatePromptMask;
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
