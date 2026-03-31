#pragma once

#include <QObject>
#include <QLabel>
#include <QDoubleSpinBox>
#include <QCheckBox>
#include <QPushButton>
#include <functional>

#include "editor_shared.h"

class EffectsTab : public QObject
{
    Q_OBJECT

public:
    struct Widgets
    {
        QLabel* effectsPathLabel = nullptr;
        QDoubleSpinBox* maskFeatherSpin = nullptr;
        QDoubleSpinBox* maskFeatherGammaSpin = nullptr;
        QCheckBox* maskFeatherEnabledCheck = nullptr;
        QPushButton* applyButton = nullptr;
    };

    struct Dependencies
    {
        std::function<const TimelineClip*()> getSelectedClip;
        std::function<QString(const TimelineClip&)> getClipFilePath;
        std::function<bool(const QString&, const std::function<void(TimelineClip&)>&)> updateClipById;
        std::function<void()> setPreviewTimelineClips;
        std::function<void()> refreshInspector;
        std::function<void()> scheduleSaveState;
        std::function<void()> pushHistorySnapshot;
        std::function<bool(const TimelineClip&)> clipHasVisuals;
        std::function<bool(const TimelineClip&)> clipHasAlpha;
    };

    explicit EffectsTab(const Widgets& widgets, const Dependencies& deps, QObject* parent = nullptr);
    ~EffectsTab() override = default;

    void wire();
    void refresh();
    void applyMaskFeather(bool pushHistory = false);

signals:
    void effectsApplied();

private slots:
    void onMaskFeatherChanged(double value);
    void onMaskFeatherGammaChanged(double value);
    void onMaskFeatherEnabledChanged(bool enabled);
    void onApplyClicked();
    void onEditingFinished();

private:
    Widgets m_widgets;
    Dependencies m_deps;
    bool m_updating = false;
};
