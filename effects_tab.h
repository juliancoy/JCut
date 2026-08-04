#pragma once

#include <QObject>
#include <QLabel>
#include <QDoubleSpinBox>
#include <QCheckBox>
#include <QComboBox>
#include <QDial>
#include <QPushButton>
#include <QPoint>
#include <QSpinBox>
#include <QTableWidget>
#include <QWidget>
#include <functional>

#include "editor_shared_core.h"

class EffectsTab : public QObject
{
    Q_OBJECT

public:
    struct Widgets
    {
        QLabel* effectsPathLabel = nullptr;
        QComboBox* edgeFillEffectCombo = nullptr;
        QSpinBox* edgeFillPixelsSpin = nullptr;
        QDoubleSpinBox* edgeFillPowerSpin = nullptr;
        QDoubleSpinBox* edgeFillOpacitySpin = nullptr;
        QDoubleSpinBox* edgeFillBrightnessSpin = nullptr;
        QDoubleSpinBox* edgeFillSaturationSpin = nullptr;
        QComboBox* effectPresetCategoryCombo = nullptr;
        QComboBox* effectPresetCombo = nullptr;
        QPushButton* effectPresetPreviousButton = nullptr;
        QPushButton* effectPresetNextButton = nullptr;
        QLabel* effectPresetSpecificHelpLabel = nullptr;
        QSpinBox* effectRowsSpin = nullptr;
        QDoubleSpinBox* effectSpeedSpin = nullptr;
        QDoubleSpinBox* effectScaleSpin = nullptr;
        QCheckBox* effectAlternateDirectionCheck = nullptr;
        QCheckBox* effectSpeechSyncCheck = nullptr;
        QCheckBox* effectEnabledCheck = nullptr;
        QPushButton* effectKeyframeOnButton = nullptr;
        QPushButton* effectKeyframeOffButton = nullptr;
        QPushButton* effectParameterKeyframeButton = nullptr;
        QPushButton* effectKeyframeRemoveButton = nullptr;
        QLabel* effectKeyframesLabel = nullptr;
        QTableWidget* effectKeyframeTable = nullptr;
        QComboBox* effectModulationModeCombo = nullptr;
        QComboBox* effectModulationTargetCombo = nullptr;
        QDoubleSpinBox* effectModulationAmountSpin = nullptr;
        QDoubleSpinBox* effectModulationRateSpin = nullptr;
        QDoubleSpinBox* effectModulationPhaseSpin = nullptr;
        QSpinBox* differenceReferenceFramesSpin = nullptr;
        QDoubleSpinBox* differenceThresholdSpin = nullptr;
        QDoubleSpinBox* differenceSoftnessSpin = nullptr;
        QSpinBox* temporalEchoCountSpin = nullptr;
        QSpinBox* temporalEchoSpacingSpin = nullptr;
        QDoubleSpinBox* temporalEchoDecaySpin = nullptr;
        QComboBox* tilingPatternCombo = nullptr;
        QDoubleSpinBox* tilingSpacingSpin = nullptr;
        QCheckBox* tilingWrapCheck = nullptr;
        QWidget* maskBoundingBoxSection = nullptr;
        QCheckBox* tilingUseMaskBoundsCheck = nullptr;
        QDoubleSpinBox* tilingMaskIslandSigmaSpin = nullptr;
        QCheckBox* maskBoundingBoxPreviewCheck = nullptr;
        QWidget* directionalEchoControlsWidget = nullptr;
        QDial* directionalEchoDirectionDial = nullptr;
        QLabel* directionalEchoDirectionValueLabel = nullptr;
        QDial* directionalEchoSpreadDial = nullptr;
        QLabel* directionalEchoSpreadValueLabel = nullptr;
        QDial* directionalEchoHueDial = nullptr;
        QLabel* directionalEchoHueValueLabel = nullptr;
        QLabel* directionalEchoSummaryLabel = nullptr;
        QWidget* stepRepeatFillControlsWidget = nullptr;
        QDial* stepRepeatFillGuideScaleDial = nullptr;
        QLabel* stepRepeatFillGuideScaleValueLabel = nullptr;
        QDial* stepRepeatFillLumaMatchDial = nullptr;
        QLabel* stepRepeatFillLumaMatchValueLabel = nullptr;
        QDial* stepRepeatFillHueMatchDial = nullptr;
        QLabel* stepRepeatFillHueMatchValueLabel = nullptr;
        QLabel* stepRepeatFillSummaryLabel = nullptr;
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
        std::function<int64_t()> currentTimelineFrame;
        std::function<void(int64_t)> seekToTimelineFrame;
    };

    explicit EffectsTab(const Widgets& widgets, const Dependencies& deps, QObject* parent = nullptr);
    ~EffectsTab() override = default;

    void wire();
    void refresh();
    void applyEffectPreset(bool pushHistory = false);
    void setEffectEnabledKeyframe(bool enabled);
    void setEffectParameterKeyframe();
    void upsertEffectParameterKeyframe(bool pushHistory, bool refreshAfter);
    void removeEffectEnabledKeyframe();

signals:
    void effectsApplied();

private slots:
    void onApplyClicked();
    void onEditingFinished();
    void onEffectPresetCategoryChanged(int index);
    void onEffectPresetChanged(int index);
    void onEffectControlChanged();
    void onEffectKeyframeTableItemClicked(QTableWidgetItem* item);
    void onEffectKeyframeTableItemChanged(QTableWidgetItem* item);
    void onEffectKeyframeTableItemDoubleClicked(QTableWidgetItem* item);
    void onEffectKeyframeTableCustomContextMenu(const QPoint& pos);

private:
    bool eventFilter(QObject* watched, QEvent* event) override;
    void stepEffectPreset(int delta);
    Widgets m_widgets;
    Dependencies m_deps;
    bool m_updating = false;
};
