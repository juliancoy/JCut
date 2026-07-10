#pragma once

#include <QObject>
#include <QLabel>
#include <QDoubleSpinBox>
#include <QCheckBox>
#include <QComboBox>
#include <QPushButton>
#include <QSpinBox>
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
        QComboBox* maskFeatherFalloffCombo = nullptr;
        QCheckBox* maskFeatherEnabledCheck = nullptr;
        QCheckBox* maskForegroundLayerCheck = nullptr;
        QCheckBox* maskRepeatEnabledCheck = nullptr;
        QDoubleSpinBox* maskRepeatDeltaXSpin = nullptr;
        QDoubleSpinBox* maskRepeatDeltaYSpin = nullptr;
        QComboBox* effectPresetCombo = nullptr;
        QSpinBox* effectRowsSpin = nullptr;
        QDoubleSpinBox* effectSpeedSpin = nullptr;
        QDoubleSpinBox* effectScaleSpin = nullptr;
        QCheckBox* effectAlternateDirectionCheck = nullptr;
        QCheckBox* effectSpeechSyncCheck = nullptr;
        QComboBox* tilingPatternCombo = nullptr;
        QDoubleSpinBox* tilingSpacingSpin = nullptr;
        QCheckBox* tilingWrapCheck = nullptr;
        QPushButton* applyButton = nullptr;
    };

    struct Dependencies
    {
        std::function<const TimelineClip*()> getSelectedClip;
        std::function<int()> getSelectedTrackIndex;
        std::function<const TimelineTrack*(int)> getTrackByIndex;
        std::function<QString(const TimelineClip&)> getClipFilePath;
        std::function<bool(const QString&, const std::function<void(TimelineClip&)>&)> updateClipById;
        std::function<bool(int, const std::function<void(TimelineTrack&)>&)> updateTrackByIndex;
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
    void applyEffectPreset(bool pushHistory = false);

signals:
    void effectsApplied();

private slots:
    void onMaskFeatherChanged(double value);
    void onMaskFeatherGammaChanged(double value);
    void onMaskFeatherFalloffChanged(int index);
    void onMaskFeatherEnabledChanged(bool enabled);
    void onApplyClicked();
    void onEditingFinished();
    void onEffectPresetChanged(int index);
    void onEffectControlChanged();

private:
    Widgets m_widgets;
    Dependencies m_deps;
    bool m_updating = false;
};
