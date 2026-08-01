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
class QSpinBox;
class QTimer;
namespace editor::masks {
struct FuzzyRemoveAnalysis;
struct FuzzyRemoveResult;
}

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
        QPushButton* biRefNetRefineButton = nullptr;
        QSpinBox* biRefNetGuideRadiusSpin = nullptr;
        QPushButton* fuzzyRemoveButton = nullptr;
        QSpinBox* fuzzySpatialReachSpin = nullptr;
        QSpinBox* fuzzyTemporalReachSpin = nullptr;
        QLabel* fuzzyStatusLabel = nullptr;
        QSpinBox* zLevelSpin = nullptr;
        QDoubleSpinBox* featherSpin = nullptr;
        QComboBox* featherFalloffCombo = nullptr;
        QDoubleSpinBox* featherPowerSpin = nullptr;
        QDoubleSpinBox* dilateSpin = nullptr;
        QDoubleSpinBox* erodeSpin = nullptr;
        QDoubleSpinBox* blurSpin = nullptr;
        QCheckBox* invertCheck = nullptr;
        QCheckBox* showOnlyCheck = nullptr;
        QDoubleSpinBox* opacitySpin = nullptr;
        QCheckBox* foregroundLayerCheck = nullptr;
        QCheckBox* repeatEnabledCheck = nullptr;
        QDoubleSpinBox* repeatDeltaXSpin = nullptr;
        QDoubleSpinBox* repeatDeltaYSpin = nullptr;
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
        std::function<void(const QString&, const QString&, int)>
            refineMaskWithBiRefNet;
        std::function<QString(const QString&, const QString&)> findMaskMatteChildForSidecar;
        std::function<QString(const QString&, const QString&)> materializeMaskMatteForSidecar;
        std::function<void(const QString&)> selectClipById;
        std::function<bool()> isMaskInspectorActive;
        std::function<void(bool)> setMaskFuzzyRemoveMode;
        std::function<bool(const editor::masks::FuzzyRemoveAnalysis&)>
            confirmFuzzyRemoveAnalysis;
    };

    explicit MaskTab(const Widgets& widgets, const Dependencies& deps, QObject* parent = nullptr);

    void wire();
    void refresh();
    void apply(bool pushHistory = false, bool zLevelEdited = false);
    void handlePreviewPoint(const QString& clipId,
                            int64_t sourceFrame,
                            int64_t sourcePresentationTimestamp,
                            qreal xNorm,
                            qreal yNorm);

private:
    void scheduleTreatmentEdit(bool zLevelEdited = false);
    void applyTreatmentEdit(bool commit, bool zLevelEdited = false);
    void setControlsEnabled(bool enabled);
    void setTreatmentControlsEnabled(bool enabled);
    bool confirmFuzzyRemoveAnalysis(
        const editor::masks::FuzzyRemoveAnalysis& analysis) const;
    void materializeFuzzyRemoveAnalysis(
        const QString& selectedId,
        editor::masks::FuzzyRemoveAnalysis analysis);

    Widgets m_widgets;
    Dependencies m_deps;
    bool m_updating = false;
    bool m_pendingTreatmentZLevelEdited = false;
    QTimer* m_treatmentEditTimer = nullptr;
};
