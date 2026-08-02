#include "effects_tab.h"
#include "editor_effect_presets.h"
#include "editor_shared_effects.h"
#include "editor_tab_edit_effects.h"

#include <QSignalBlocker>
#include <QDir>
#include <QFormLayout>
#include <QLabel>
#include <QStringList>
#include <QtGlobal>
#include <QStandardItemModel>
#include <algorithm>
#include <memory>

EffectsTab::EffectsTab(const Widgets& widgets, const Dependencies& deps, QObject* parent)
    : QObject(parent)
    , m_widgets(widgets)
    , m_deps(deps)
{
}

namespace {
TabEditCallbacks effectsEditCallbacks(const EffectsTab::Dependencies& deps) {
    return TabEditCallbacks{
        .updatePreview = deps.setPreviewTimelineClips,
        .refreshInspector = deps.refreshInspector,
        .scheduleSave = deps.scheduleSaveState,
        .pushHistory = deps.pushHistorySnapshot,
    };
}

ClipEffectPreset presetFromCombo(const QComboBox* combo)
{
    if (!combo) {
        return ClipEffectPreset::None;
    }
    bool ok = false;
    const int value = combo->currentData().toInt(&ok);
    return static_cast<ClipEffectPreset>(
        ok ? value : static_cast<int>(ClipEffectPreset::None));
}

int comboIndexForPreset(const QComboBox* combo, ClipEffectPreset preset)
{
    if (!combo) {
        return -1;
    }
    const int index = combo->findData(static_cast<int>(preset));
    return index >= 0 ? index : combo->findData(static_cast<int>(ClipEffectPreset::None));
}

void setMaskMattePresetAvailability(QComboBox* combo, bool maskMatteTarget)
{
    if (!combo) {
        return;
    }
    QStandardItemModel* model = qobject_cast<QStandardItemModel*>(combo->model());
    if (!model) {
        return;
    }
    const QString unavailableReason = QStringLiteral(
        "This source-history effect is unavailable for Mask Matte layers. "
        "The stored setting is retained but rendered inactive.");
    for (const ClipEffectPreset preset : {
             ClipEffectPreset::DifferenceMatte,
             ClipEffectPreset::TemporalEcho}) {
        const int index = comboIndexForPreset(combo, preset);
        if (index < 0) {
            continue;
        }
        if (QStandardItem* item = model->item(index)) {
            item->setEnabled(!maskMatteTarget);
            item->setToolTip(maskMatteTarget ? unavailableReason : QString());
        }
    }
    combo->setToolTip(maskMatteTarget ? unavailableReason : QString());
}

ClipTilingPattern tilingPatternFromCombo(const QComboBox* combo)
{
    if (!combo) {
        return ClipTilingPattern::Grid;
    }
    bool ok = false;
    const int value = combo->currentData().toInt(&ok);
    return static_cast<ClipTilingPattern>(
        ok ? value : static_cast<int>(ClipTilingPattern::Grid));
}

int comboIndexForTilingPattern(const QComboBox* combo, ClipTilingPattern pattern)
{
    if (!combo) {
        return -1;
    }
    const int index = combo->findData(static_cast<int>(pattern));
    return index >= 0 ? index : combo->findData(static_cast<int>(ClipTilingPattern::Grid));
}

template <typename Owner>
QJsonObject effectParameters(const Owner& owner)
{
    return QJsonObject{{QStringLiteral("rows"), owner.effectRows},
                       {QStringLiteral("speed"), owner.effectSpeed},
                       {QStringLiteral("scale"), owner.effectScale},
                       {QStringLiteral("alternate"), owner.effectAlternateDirection},
                       {QStringLiteral("differenceReference"), owner.differenceReferenceFrames},
                       {QStringLiteral("differenceThreshold"), owner.differenceThreshold},
                       {QStringLiteral("differenceSoftness"), owner.differenceSoftness},
                       {QStringLiteral("echoCount"), owner.temporalEchoCount},
                       {QStringLiteral("echoSpacing"), owner.temporalEchoSpacingFrames},
                       {QStringLiteral("echoDecay"), owner.temporalEchoDecay},
                       {QStringLiteral("pattern"), static_cast<int>(owner.tilingPattern)},
                       {QStringLiteral("spacing"), owner.tilingSpacing},
                       {QStringLiteral("wrap"), owner.tilingWrap}};
}

template <typename Owner>
void restoreEffectParameters(Owner& owner, const QJsonObject& values)
{
    if (values.isEmpty()) return;
    owner.effectRows = qBound(1, values.value(QStringLiteral("rows")).toInt(32), 96);
    owner.effectSpeed = qBound<qreal>(-8.0, values.value(QStringLiteral("speed")).toDouble(1.0), 8.0);
    owner.effectScale = qBound<qreal>(0.1, values.value(QStringLiteral("scale")).toDouble(1.0), 8.0);
    owner.effectAlternateDirection = values.value(QStringLiteral("alternate")).toBool(true);
    owner.differenceReferenceFrames = qBound(1, values.value(QStringLiteral("differenceReference")).toInt(1), 300);
    owner.differenceThreshold = qBound<qreal>(0.0, values.value(QStringLiteral("differenceThreshold")).toDouble(0.1), 1.0);
    owner.differenceSoftness = qBound<qreal>(0.0, values.value(QStringLiteral("differenceSoftness")).toDouble(0.05), 1.0);
    owner.temporalEchoCount = qBound(1, values.value(QStringLiteral("echoCount")).toInt(4), 12);
    owner.temporalEchoSpacingFrames = qBound(1, values.value(QStringLiteral("echoSpacing")).toInt(2), 120);
    owner.temporalEchoDecay = qBound<qreal>(0.0, values.value(QStringLiteral("echoDecay")).toDouble(0.65), 1.0);
    owner.tilingPattern = static_cast<ClipTilingPattern>(values.value(QStringLiteral("pattern")).toInt(0));
    owner.tilingSpacing = qBound<qreal>(0.1, values.value(QStringLiteral("spacing")).toDouble(1.0), 8.0);
    owner.tilingWrap = values.value(QStringLiteral("wrap")).toBool(true);
}

QString presetParameterKey(ClipEffectPreset preset)
{
    return QString::number(static_cast<int>(preset));
}

void setFormFieldVisible(QWidget* field, bool visible)
{
    if (!field) return;
    field->setVisible(visible);
    QWidget* root = field->window();
    const QList<QFormLayout*> forms = root->findChildren<QFormLayout*>();
    for (QFormLayout* form : forms) {
        if (QWidget* label = form->labelForField(field)) label->setVisible(visible);
    }
}

void setFormFieldLabel(QWidget* field, const QString& text)
{
    if (!field) return;
    const QList<QFormLayout*> forms = field->window()->findChildren<QFormLayout*>();
    for (QFormLayout* form : forms) {
        if (QLabel* label = qobject_cast<QLabel*>(form->labelForField(field))) label->setText(text);
    }
}

bool speakerMaskDilationPreset(ClipEffectPreset preset)
{
    return preset == ClipEffectPreset::SpeakerMaskDilation ||
           preset == ClipEffectPreset::SpeakerMaskDilationPulse ||
           preset == ClipEffectPreset::SpeakerMaskDilationRings;
}

bool mirrorGeometryPreset(ClipEffectPreset preset)
{
    switch (preset) {
    case ClipEffectPreset::MirrorRing:
    case ClipEffectPreset::Kaleidoscope:
    case ClipEffectPreset::QuadMirror:
    case ClipEffectPreset::InfiniteMirror:
    case ClipEffectPreset::Tessellation:
    case ClipEffectPreset::HexagonalPrism:
    case ClipEffectPreset::Droste:
    case ClipEffectPreset::RecursiveZoomTile:
    case ClipEffectPreset::RecursiveZoomTunnel:
    case ClipEffectPreset::RecursiveZoomMirrorBox:
    case ClipEffectPreset::RecursiveZoomSpiral:
    case ClipEffectPreset::RecursiveZoomKaleidoscope:
    case ClipEffectPreset::RecursiveZoomRadialRepeat:
    case ClipEffectPreset::RecursiveZoomPixelMosaic:
        return true;
    default:
        return false;
    }
}

void updatePresetParameterVisibility(const EffectsTab::Widgets& widgets,
                                     ClipEffectPreset preset,
                                     const QString& modulationMode)
{
    const bool mirrorGeometry = mirrorGeometryPreset(preset);
    const bool commonParameters =
        preset == ClipEffectPreset::NewsLogoTicker ||
        preset == ClipEffectPreset::PersonOrbit ||
        preset == ClipEffectPreset::AlternatingMotionBackground ||
        preset == ClipEffectPreset::FreezePattern ||
        preset == ClipEffectPreset::StepRepeat ||
        preset == ClipEffectPreset::DirectionalTrimTicker ||
        preset == ClipEffectPreset::SourceTile ||
        preset == ClipEffectPreset::Vulkan3DSynth ||
        preset == ClipEffectPreset::SobelEdges ||
        preset == ClipEffectPreset::NeonGlow ||
        speakerMaskDilationPreset(preset) ||
        mirrorGeometry;
    const bool edge = preset == ClipEffectPreset::SobelEdges;
    const bool neon = preset == ClipEffectPreset::NeonGlow;
    const bool speakerMask = speakerMaskDilationPreset(preset);
    const bool difference = preset == ClipEffectPreset::DifferenceMatte;
    const bool echo = preset == ClipEffectPreset::TemporalEcho;
    const bool tiling = preset == ClipEffectPreset::SourceTile;
    const bool spacing = tiling || mirrorGeometry || speakerMask;
    const bool sectorEffect =
        preset == ClipEffectPreset::MirrorRing ||
        preset == ClipEffectPreset::Kaleidoscope;
    const bool recursionEffect =
        preset == ClipEffectPreset::Droste ||
        preset == ClipEffectPreset::RecursiveZoomTile ||
        preset == ClipEffectPreset::RecursiveZoomTunnel ||
        preset == ClipEffectPreset::RecursiveZoomMirrorBox ||
        preset == ClipEffectPreset::RecursiveZoomSpiral ||
        preset == ClipEffectPreset::RecursiveZoomKaleidoscope ||
        preset == ClipEffectPreset::RecursiveZoomRadialRepeat ||
        preset == ClipEffectPreset::RecursiveZoomPixelMosaic ||
        preset == ClipEffectPreset::InfiniteMirror;
    const bool cellEffect =
        preset == ClipEffectPreset::QuadMirror ||
        preset == ClipEffectPreset::Tessellation ||
        preset == ClipEffectPreset::HexagonalPrism;
    if (widgets.effectRowsSpin) {
        widgets.effectRowsSpin->setRange(
            sectorEffect ? 2 : 1,
            edge || neon ? 4 : (speakerMask ? 8 : 96));
    }
    if (widgets.effectScaleSpin) {
        widgets.effectScaleSpin->setRange(0.1, speakerMask ? 1.0 : 8.0);
    }
    setFormFieldLabel(widgets.effectRowsSpin,
                      edge ? QStringLiteral("Sample radius")
                           : neon ? QStringLiteral("Glow radius")
                                  : speakerMask ? QStringLiteral("Dilation radius")
                                                : sectorEffect ? QStringLiteral("Mirror sectors")
                                                : recursionEffect ? QStringLiteral("Recursion density")
                                                : cellEffect ? QStringLiteral("Cells across")
                                                : QStringLiteral("Copies"));
    setFormFieldLabel(widgets.effectSpeedSpin,
                      neon ? QStringLiteral("Hue speed")
                           : speakerMask ? QStringLiteral("Color cycle speed")
                                        : mirrorGeometry ? QStringLiteral("Rotation speed")
                                         : QStringLiteral("Speed"));
    setFormFieldLabel(widgets.effectScaleSpin,
                      edge ? QStringLiteral("Edge strength")
                           : neon ? QStringLiteral("Glow intensity")
                                  : speakerMask ? QStringLiteral("Opacity")
                                                : mirrorGeometry ? QStringLiteral("Output grain size")
                                                : QStringLiteral("Scale"));
    setFormFieldLabel(widgets.tilingSpacingSpin,
                      speakerMask ? QStringLiteral("Color spacing")
                                  : recursionEffect ? QStringLiteral("Recursion spacing")
                                  : mirrorGeometry ? QStringLiteral("Geometry amount")
                                                   : QStringLiteral("Spacing"));
    if (mirrorGeometry) {
        if (widgets.effectRowsSpin) {
            widgets.effectRowsSpin->setToolTip(
                sectorEffect
                    ? QStringLiteral("Number of mirrored angular sectors.")
                    : recursionEffect
                          ? QStringLiteral("Density of nested recursive bands.")
                          : QStringLiteral("Number of repeated mirror cells across the output."));
        }
        if (widgets.effectScaleSpin) {
            widgets.effectScaleSpin->setToolTip(
                QStringLiteral("Size of the sampled source grain. Higher values produce larger image features."));
        }
        if (widgets.effectSpeedSpin) {
            widgets.effectSpeedSpin->setToolTip(
                QStringLiteral("Rotation rate. Negative values reverse direction; zero holds the geometry still."));
        }
        if (widgets.tilingSpacingSpin) {
            widgets.tilingSpacingSpin->setToolTip(
                recursionEffect
                    ? QStringLiteral("Spacing and twist between recursive levels.")
                    : QStringLiteral("Strength of the radial or cell geometry."));
        }
    }
    setFormFieldVisible(widgets.effectRowsSpin, commonParameters);
    setFormFieldVisible(widgets.effectSpeedSpin, commonParameters && !edge);
    setFormFieldVisible(widgets.effectScaleSpin, commonParameters);
    setFormFieldVisible(widgets.effectAlternateDirectionCheck,
                        commonParameters && !edge && !neon && !speakerMask &&
                            !mirrorGeometry);
    const bool steadyIncrease =
        modulationMode == QStringLiteral("steady_increase");
    setFormFieldVisible(
        widgets.effectSpeechSyncCheck,
        steadyIncrease ||
            (commonParameters && !edge && !neon && !speakerMask));
    setFormFieldVisible(widgets.differenceReferenceFramesSpin, difference);
    setFormFieldVisible(widgets.differenceThresholdSpin, difference);
    setFormFieldVisible(widgets.differenceSoftnessSpin, difference);
    setFormFieldVisible(widgets.temporalEchoCountSpin, echo);
    setFormFieldVisible(widgets.temporalEchoSpacingSpin, echo);
    setFormFieldVisible(widgets.temporalEchoDecaySpin, echo);
    setFormFieldVisible(widgets.tilingPatternCombo, tiling);
    setFormFieldVisible(widgets.tilingSpacingSpin, spacing);
    setFormFieldVisible(widgets.tilingWrapCheck, tiling);
}
}

void EffectsTab::wire()
{
    if (m_widgets.applyButton) {
        connect(m_widgets.applyButton, &QPushButton::clicked,
                this, &EffectsTab::onApplyClicked);
    }
    if (m_widgets.edgeFillEffectCombo) {
        connect(m_widgets.edgeFillEffectCombo,
                qOverload<int>(&QComboBox::currentIndexChanged),
                this,
                &EffectsTab::onEffectControlChanged);
    }
    if (m_widgets.edgeFillPixelsSpin) {
        connect(m_widgets.edgeFillPixelsSpin, qOverload<int>(&QSpinBox::valueChanged),
                this, &EffectsTab::onEffectControlChanged);
    }
    const QVector<QDoubleSpinBox*> edgeFillRealControls{
        m_widgets.edgeFillPowerSpin, m_widgets.edgeFillOpacitySpin,
        m_widgets.edgeFillBrightnessSpin, m_widgets.edgeFillSaturationSpin};
    for (QDoubleSpinBox* spin : edgeFillRealControls) {
        if (!spin) continue;
        connect(spin, qOverload<double>(&QDoubleSpinBox::valueChanged),
                this, &EffectsTab::onEffectControlChanged);
        connect(spin, &QDoubleSpinBox::editingFinished,
                this, &EffectsTab::onEditingFinished);
    }
    if (m_widgets.effectPresetCombo) {
        connect(m_widgets.effectPresetCombo, qOverload<int>(&QComboBox::currentIndexChanged),
                this, &EffectsTab::onEffectPresetChanged);
    }
    const QVector<QSpinBox*> integerEffectControls{
        m_widgets.differenceReferenceFramesSpin, m_widgets.temporalEchoCountSpin,
        m_widgets.temporalEchoSpacingSpin};
    for (QSpinBox* spin : integerEffectControls) {
        if (spin) connect(spin, qOverload<int>(&QSpinBox::valueChanged),
                          this, &EffectsTab::onEffectControlChanged);
    }
    const QVector<QDoubleSpinBox*> realEffectControls{
        m_widgets.differenceThresholdSpin, m_widgets.differenceSoftnessSpin,
        m_widgets.temporalEchoDecaySpin};
    for (QDoubleSpinBox* spin : realEffectControls) {
        if (spin) connect(spin, qOverload<double>(&QDoubleSpinBox::valueChanged),
                          this, &EffectsTab::onEffectControlChanged);
    }
    if (m_widgets.effectRowsSpin) {
        connect(m_widgets.effectRowsSpin, qOverload<int>(&QSpinBox::valueChanged),
                this, &EffectsTab::onEffectControlChanged);
    }
    if (m_widgets.effectSpeedSpin) {
        connect(m_widgets.effectSpeedSpin, qOverload<double>(&QDoubleSpinBox::valueChanged),
                this, &EffectsTab::onEffectControlChanged);
        connect(m_widgets.effectSpeedSpin, &QDoubleSpinBox::editingFinished,
                this, &EffectsTab::onEditingFinished);
    }
    if (m_widgets.effectScaleSpin) {
        connect(m_widgets.effectScaleSpin, qOverload<double>(&QDoubleSpinBox::valueChanged),
                this, &EffectsTab::onEffectControlChanged);
        connect(m_widgets.effectScaleSpin, &QDoubleSpinBox::editingFinished,
                this, &EffectsTab::onEditingFinished);
    }
    if (m_widgets.effectAlternateDirectionCheck) {
        connect(m_widgets.effectAlternateDirectionCheck, &QCheckBox::toggled,
                this, &EffectsTab::onEffectControlChanged);
    }
    if (m_widgets.effectSpeechSyncCheck) {
        connect(m_widgets.effectSpeechSyncCheck, &QCheckBox::toggled,
                this, &EffectsTab::onEffectControlChanged);
    }
    if (m_widgets.effectEnabledCheck) {
        connect(m_widgets.effectEnabledCheck, &QCheckBox::toggled,
                this, &EffectsTab::onEffectControlChanged);
    }
    if (m_widgets.effectKeyframeOnButton) {
        connect(m_widgets.effectKeyframeOnButton, &QPushButton::clicked,
                this, [this]() { setEffectEnabledKeyframe(true); });
    }
    if (m_widgets.effectKeyframeOffButton) {
        connect(m_widgets.effectKeyframeOffButton, &QPushButton::clicked,
                this, [this]() { setEffectEnabledKeyframe(false); });
    }
    if (m_widgets.effectParameterKeyframeButton) {
        connect(m_widgets.effectParameterKeyframeButton, &QPushButton::clicked,
                this, &EffectsTab::setEffectParameterKeyframe);
    }
    if (m_widgets.effectKeyframeRemoveButton) {
        connect(m_widgets.effectKeyframeRemoveButton, &QPushButton::clicked,
                this, &EffectsTab::removeEffectEnabledKeyframe);
    }
    if (m_widgets.effectModulationModeCombo) {
        connect(
            m_widgets.effectModulationModeCombo,
            qOverload<int>(&QComboBox::currentIndexChanged),
            this,
            [this]() {
                if (m_updating) return;
                applyEffectPreset(false);
                refresh();
            });
    }
    if (m_widgets.effectModulationTargetCombo) {
        connect(m_widgets.effectModulationTargetCombo,
                qOverload<int>(&QComboBox::currentIndexChanged),
                this, &EffectsTab::onEffectControlChanged);
    }
    for (QDoubleSpinBox* spin : {
             m_widgets.effectModulationAmountSpin,
             m_widgets.effectModulationRateSpin,
             m_widgets.effectModulationPhaseSpin}) {
        if (spin) {
            connect(spin, qOverload<double>(&QDoubleSpinBox::valueChanged),
                    this, &EffectsTab::onEffectControlChanged);
            connect(spin, &QDoubleSpinBox::editingFinished,
                    this, &EffectsTab::onEditingFinished);
        }
    }
    if (m_widgets.tilingPatternCombo) {
        connect(m_widgets.tilingPatternCombo, qOverload<int>(&QComboBox::currentIndexChanged),
                this, &EffectsTab::onEffectControlChanged);
    }
    if (m_widgets.tilingSpacingSpin) {
        connect(m_widgets.tilingSpacingSpin, qOverload<double>(&QDoubleSpinBox::valueChanged),
                this, &EffectsTab::onEffectControlChanged);
        connect(m_widgets.tilingSpacingSpin, &QDoubleSpinBox::editingFinished,
                this, &EffectsTab::onEditingFinished);
    }
    if (m_widgets.tilingWrapCheck) {
        connect(m_widgets.tilingWrapCheck, &QCheckBox::toggled,
                this, &EffectsTab::onEffectControlChanged);
    }
}

void EffectsTab::refresh()
{
    if (!m_widgets.effectsPathLabel) {
        return;
    }

    const TimelineClip* clip = m_deps.getSelectedClip();
    const int selectedTrackIndex =
        clip ? clip->trackIndex : (m_deps.getSelectedTrackIndex ? m_deps.getSelectedTrackIndex() : -1);
    const TimelineTrack* selectedTrack =
        m_deps.getTrackByIndex ? m_deps.getTrackByIndex(selectedTrackIndex) : nullptr;
    const bool maskMatteTarget =
        (clip && clip->clipRole == ClipRole::MaskMatte) ||
        (!clip && selectedTrack && selectedTrack->generatedChildTrack);
    setMaskMattePresetAvailability(m_widgets.effectPresetCombo, maskMatteTarget);
    m_updating = true;

    const std::unique_ptr<QSignalBlocker> presetBlock =
        m_widgets.effectPresetCombo
            ? std::make_unique<QSignalBlocker>(m_widgets.effectPresetCombo)
            : nullptr;
    const std::unique_ptr<QSignalBlocker> rowsBlock =
        m_widgets.effectRowsSpin ? std::make_unique<QSignalBlocker>(m_widgets.effectRowsSpin) : nullptr;
    const std::unique_ptr<QSignalBlocker> speedBlock =
        m_widgets.effectSpeedSpin ? std::make_unique<QSignalBlocker>(m_widgets.effectSpeedSpin) : nullptr;
    const std::unique_ptr<QSignalBlocker> scaleBlock =
        m_widgets.effectScaleSpin ? std::make_unique<QSignalBlocker>(m_widgets.effectScaleSpin) : nullptr;
    const std::unique_ptr<QSignalBlocker> alternateBlock =
        m_widgets.effectAlternateDirectionCheck
            ? std::make_unique<QSignalBlocker>(m_widgets.effectAlternateDirectionCheck)
            : nullptr;
    const std::unique_ptr<QSignalBlocker> speechSyncBlock =
        m_widgets.effectSpeechSyncCheck
            ? std::make_unique<QSignalBlocker>(m_widgets.effectSpeechSyncCheck)
            : nullptr;
    const std::unique_ptr<QSignalBlocker> tilingPatternBlock =
        m_widgets.tilingPatternCombo ? std::make_unique<QSignalBlocker>(m_widgets.tilingPatternCombo) : nullptr;
    const std::unique_ptr<QSignalBlocker> tilingSpacingBlock =
        m_widgets.tilingSpacingSpin ? std::make_unique<QSignalBlocker>(m_widgets.tilingSpacingSpin) : nullptr;
    const std::unique_ptr<QSignalBlocker> tilingWrapBlock =
        m_widgets.tilingWrapCheck ? std::make_unique<QSignalBlocker>(m_widgets.tilingWrapCheck) : nullptr;

    const bool selectedSynthClip = clip && clip->clipRole == ClipRole::EffectSynth;

    if (!clip || selectedSynthClip || !m_deps.clipHasVisuals(*clip)) {
        m_widgets.effectsPathLabel->setText(
            selectedSynthClip
                ? QStringLiteral("Generated effect clip\nEdit its source effect controls")
                : selectedTrack && selectedTrack->generatedChildTrack
                ? QStringLiteral("Mask Matte layer\nSelect its clip to edit child-owned effects")
                : selectedTrack
                ? QStringLiteral("Track effects\n%1").arg(selectedTrack->name)
                : QStringLiteral("No visual clip selected"));
        m_widgets.effectsPathLabel->setToolTip(QString());
        for (QWidget* widget : {
                 static_cast<QWidget*>(m_widgets.edgeFillEffectCombo),
                 static_cast<QWidget*>(m_widgets.edgeFillPixelsSpin),
                 static_cast<QWidget*>(m_widgets.edgeFillPowerSpin),
                 static_cast<QWidget*>(m_widgets.edgeFillOpacitySpin),
                 static_cast<QWidget*>(m_widgets.edgeFillBrightnessSpin),
                 static_cast<QWidget*>(m_widgets.edgeFillSaturationSpin)}) {
            if (widget) widget->setEnabled(false);
        }
        if (m_widgets.edgeFillEffectCombo) {
            m_widgets.edgeFillEffectCombo->setCurrentIndex(
                m_widgets.edgeFillEffectCombo->findData(
                    backgroundFillEffectToString(BackgroundFillEffect::None)));
        }
        if (m_widgets.effectPresetCombo) {
            m_widgets.effectPresetCombo->setCurrentIndex(comboIndexForPreset(
                m_widgets.effectPresetCombo,
                selectedTrack ? selectedTrack->effectPreset : ClipEffectPreset::None));
            m_widgets.effectPresetCombo->setEnabled(
                selectedTrack != nullptr && !selectedTrack->generatedChildTrack);
        }
        const ClipEffectPreset trackPreset =
            selectedTrack ? selectedTrack->effectPreset : ClipEffectPreset::None;
        const bool trackEffectActive = selectedTrack && trackPreset != ClipEffectPreset::None;
        if (m_widgets.effectRowsSpin) {
            m_widgets.effectRowsSpin->setValue(selectedTrack ? selectedTrack->effectRows : 32);
            m_widgets.effectRowsSpin->setEnabled(trackEffectActive);
            m_widgets.effectRowsSpin->setSuffix(QString());
            m_widgets.effectRowsSpin->setToolTip(
                QStringLiteral("Rows, copies, or repeat steps."));
        }
        if (m_widgets.effectSpeedSpin) {
            m_widgets.effectSpeedSpin->setValue(selectedTrack ? selectedTrack->effectSpeed : 1.0);
            m_widgets.effectSpeedSpin->setEnabled(trackEffectActive);
        }
        if (m_widgets.effectScaleSpin) {
            m_widgets.effectScaleSpin->setValue(selectedTrack ? selectedTrack->effectScale : 1.0);
            m_widgets.effectScaleSpin->setEnabled(trackEffectActive);
            m_widgets.effectScaleSpin->setToolTip(
                QStringLiteral("Scale multiplier for the selected effect."));
        }
        if (m_widgets.effectAlternateDirectionCheck) {
            m_widgets.effectAlternateDirectionCheck->setChecked(!selectedTrack || selectedTrack->effectAlternateDirection);
            m_widgets.effectAlternateDirectionCheck->setEnabled(trackEffectActive);
        }
        if (m_widgets.effectSpeechSyncCheck) {
            m_widgets.effectSpeechSyncCheck->setChecked(false);
            m_widgets.effectSpeechSyncCheck->setEnabled(false);
        }
        for (QWidget* widget : {
                 static_cast<QWidget*>(m_widgets.effectEnabledCheck),
                 static_cast<QWidget*>(m_widgets.effectKeyframeOnButton),
                 static_cast<QWidget*>(m_widgets.effectKeyframeOffButton),
                 static_cast<QWidget*>(m_widgets.effectParameterKeyframeButton),
                 static_cast<QWidget*>(m_widgets.effectKeyframeRemoveButton),
                 static_cast<QWidget*>(m_widgets.effectModulationModeCombo),
                 static_cast<QWidget*>(m_widgets.effectModulationTargetCombo),
                 static_cast<QWidget*>(m_widgets.effectModulationAmountSpin),
                 static_cast<QWidget*>(m_widgets.effectModulationRateSpin),
                 static_cast<QWidget*>(m_widgets.effectModulationPhaseSpin)}) {
            if (widget) widget->setEnabled(false);
        }
        if (m_widgets.effectKeyframesLabel) {
            m_widgets.effectKeyframesLabel->setText(
                QStringLiteral("Select a visual clip to animate its effect."));
        }
        if (m_widgets.differenceReferenceFramesSpin) {
            m_widgets.differenceReferenceFramesSpin->setValue(selectedTrack ? selectedTrack->differenceReferenceFrames : 1);
            m_widgets.differenceReferenceFramesSpin->setEnabled(trackEffectActive);
        }
        if (m_widgets.differenceThresholdSpin) {
            m_widgets.differenceThresholdSpin->setValue(selectedTrack ? selectedTrack->differenceThreshold : 0.10);
            m_widgets.differenceThresholdSpin->setEnabled(trackEffectActive);
        }
        if (m_widgets.differenceSoftnessSpin) {
            m_widgets.differenceSoftnessSpin->setValue(selectedTrack ? selectedTrack->differenceSoftness : 0.05);
            m_widgets.differenceSoftnessSpin->setEnabled(trackEffectActive);
        }
        if (m_widgets.temporalEchoCountSpin) {
            m_widgets.temporalEchoCountSpin->setValue(selectedTrack ? selectedTrack->temporalEchoCount : 4);
            m_widgets.temporalEchoCountSpin->setEnabled(trackEffectActive);
        }
        if (m_widgets.temporalEchoSpacingSpin) {
            m_widgets.temporalEchoSpacingSpin->setValue(selectedTrack ? selectedTrack->temporalEchoSpacingFrames : 2);
            m_widgets.temporalEchoSpacingSpin->setEnabled(trackEffectActive);
        }
        if (m_widgets.temporalEchoDecaySpin) {
            m_widgets.temporalEchoDecaySpin->setValue(selectedTrack ? selectedTrack->temporalEchoDecay : 0.65);
            m_widgets.temporalEchoDecaySpin->setEnabled(trackEffectActive);
        }
        if (m_widgets.tilingPatternCombo) {
            m_widgets.tilingPatternCombo->setCurrentIndex(comboIndexForTilingPattern(
                m_widgets.tilingPatternCombo,
                selectedTrack ? selectedTrack->tilingPattern : ClipTilingPattern::Grid));
            m_widgets.tilingPatternCombo->setEnabled(trackEffectActive);
        }
        if (m_widgets.tilingSpacingSpin) {
            m_widgets.tilingSpacingSpin->setValue(selectedTrack ? selectedTrack->tilingSpacing : 1.0);
            m_widgets.tilingSpacingSpin->setEnabled(trackEffectActive);
        }
        if (m_widgets.tilingWrapCheck) {
            m_widgets.tilingWrapCheck->setChecked(!selectedTrack || selectedTrack->tilingWrap);
            m_widgets.tilingWrapCheck->setEnabled(trackEffectActive);
        }
        updatePresetParameterVisibility(
            m_widgets, trackPreset, QStringLiteral("none"));
        m_updating = false;
        return;
    }

    const QString nativePath = QDir::toNativeSeparators(m_deps.getClipFilePath(*clip));
    const QString sourceLabel = QStringLiteral("%1 | %2")
                                    .arg(clipMediaTypeLabel(clip->mediaType),
                                         mediaSourceKindLabel(clip->sourceKind));
    m_widgets.effectsPathLabel->setText(QStringLiteral("%1\n%2").arg(clip->label, sourceLabel));
    m_widgets.effectsPathLabel->setToolTip(nativePath);

    if (m_widgets.edgeFillEffectCombo) {
        const int index = m_widgets.edgeFillEffectCombo->findData(
            backgroundFillEffectToString(clip->edgeFillEffect));
        m_widgets.edgeFillEffectCombo->setCurrentIndex(qMax(0, index));
        m_widgets.edgeFillEffectCombo->setEnabled(true);
    }
    const bool progressiveEdgeFill =
        clip->edgeFillEffect == BackgroundFillEffect::ProgressiveEdgeStretch ||
        clip->edgeFillEffect ==
            BackgroundFillEffect::ProgressiveBidirectionalEdgeStretch;
    const bool edgeFillEnabled =
        clip->edgeFillEffect != BackgroundFillEffect::None;
    if (m_widgets.edgeFillPixelsSpin) {
        m_widgets.edgeFillPixelsSpin->setValue(clip->edgeFillPixels);
        m_widgets.edgeFillPixelsSpin->setEnabled(
            edgeFillEnabled && progressiveEdgeFill);
    }
    if (m_widgets.edgeFillPowerSpin) {
        m_widgets.edgeFillPowerSpin->setValue(clip->edgeFillPower);
        m_widgets.edgeFillPowerSpin->setEnabled(
            edgeFillEnabled && progressiveEdgeFill);
    }
    if (m_widgets.edgeFillOpacitySpin) {
        m_widgets.edgeFillOpacitySpin->setValue(clip->edgeFillOpacity * 100.0);
        m_widgets.edgeFillOpacitySpin->setEnabled(edgeFillEnabled);
    }
    if (m_widgets.edgeFillBrightnessSpin) {
        m_widgets.edgeFillBrightnessSpin->setValue(clip->edgeFillBrightness * 100.0);
        m_widgets.edgeFillBrightnessSpin->setEnabled(edgeFillEnabled);
    }
    if (m_widgets.edgeFillSaturationSpin) {
        m_widgets.edgeFillSaturationSpin->setValue(clip->edgeFillSaturation * 100.0);
        m_widgets.edgeFillSaturationSpin->setEnabled(edgeFillEnabled);
    }
    if (m_widgets.effectPresetCombo) {
        m_widgets.effectPresetCombo->setCurrentIndex(comboIndexForPreset(
            m_widgets.effectPresetCombo,
            clip->effectPreset));
    }
    if (m_widgets.effectRowsSpin) {
        m_widgets.effectRowsSpin->setValue(clip->effectRows);
    }
    if (m_widgets.effectSpeedSpin) {
        m_widgets.effectSpeedSpin->setValue(clip->effectSpeed);
    }
    if (m_widgets.effectScaleSpin) {
        m_widgets.effectScaleSpin->setValue(clip->effectScale);
    }
    if (m_widgets.effectAlternateDirectionCheck) {
        m_widgets.effectAlternateDirectionCheck->setChecked(clip->effectAlternateDirection);
    }
    if (m_widgets.effectSpeechSyncCheck) {
        m_widgets.effectSpeechSyncCheck->setChecked(clip->effectSkipAwareTiming);
        const bool steadyIncrease =
            clip->effectModulationMode == QStringLiteral("steady_increase");
        m_widgets.effectSpeechSyncCheck->setText(
            steadyIncrease
                ? QStringLiteral("Transcript-aware steady increase")
                : QStringLiteral("Synchronize motion with Speech Filter"));
        m_widgets.effectSpeechSyncCheck->setToolTip(
            steadyIncrease
                ? QStringLiteral(
                      "Pause the steady-increase clock across transcript "
                      "ranges removed by the Speech Filter.")
                : QStringLiteral(
                      "Drive moving effect patterns from speech-filter timing "
                      "so skipped gaps do not create visible jumps."));
    }
    if (m_widgets.effectEnabledCheck) {
        m_widgets.effectEnabledCheck->setChecked(clip->effectEnabled);
        m_widgets.effectEnabledCheck->setEnabled(true);
    }
    const int64_t timelineFrame =
        m_deps.currentTimelineFrame ? m_deps.currentTimelineFrame() : clip->startFrame;
    const int64_t localFrame = qBound<int64_t>(
        0, timelineFrame - clip->startFrame,
        qMax<int64_t>(0, clip->durationFrames - 1));
    QStringList keyDescriptions;
    bool hasKeyAtPlayhead = false;
    for (const TimelineClip::BoolKeyframe& keyframe :
         clip->effectEnabledKeyframes) {
        keyDescriptions.push_back(
            QStringLiteral("%1:%2")
                .arg(keyframe.frame)
                .arg(keyframe.enabled ? QStringLiteral("On")
                                      : QStringLiteral("Off")));
        hasKeyAtPlayhead |= keyframe.frame == localFrame;
    }
    for (const TimelineClip::EffectParameterKeyframe& keyframe :
         clip->effectParameterKeyframes) {
        keyDescriptions.push_back(
            QStringLiteral("%1:Params rows=%2 speed=%3 scale=%4")
                .arg(keyframe.frame)
                .arg(keyframe.effectRows)
                .arg(keyframe.effectSpeed, 0, 'f', 2)
                .arg(keyframe.effectScale, 0, 'f', 2));
        hasKeyAtPlayhead |= keyframe.frame == localFrame;
    }
    if (m_widgets.effectKeyframesLabel) {
        m_widgets.effectKeyframesLabel->setText(
            keyDescriptions.isEmpty()
                ? QStringLiteral("No effect keyframes")
                : keyDescriptions.join(QStringLiteral("  •  ")));
    }
    for (QPushButton* button : {
             m_widgets.effectKeyframeOnButton,
             m_widgets.effectKeyframeOffButton,
             m_widgets.effectParameterKeyframeButton}) {
        if (button) button->setEnabled(true);
    }
    if (m_widgets.effectKeyframeRemoveButton) {
        m_widgets.effectKeyframeRemoveButton->setEnabled(hasKeyAtPlayhead);
    }
    auto setComboData = [](QComboBox* combo, const QString& value) {
        if (!combo) return;
        const int index = combo->findData(value);
        combo->setCurrentIndex(qMax(0, index));
    };
    setComboData(m_widgets.effectModulationModeCombo,
                 clip->effectModulationMode);
    setComboData(m_widgets.effectModulationTargetCombo,
                 clip->effectModulationTarget);
    if (m_widgets.effectModulationAmountSpin) {
        m_widgets.effectModulationAmountSpin->setValue(
            clip->effectModulationAmount);
    }
    if (m_widgets.effectModulationRateSpin) {
        m_widgets.effectModulationRateSpin->setValue(
            clip->effectModulationRate);
    }
    if (m_widgets.effectModulationPhaseSpin) {
        m_widgets.effectModulationPhaseSpin->setValue(
            clip->effectModulationPhaseDegrees);
    }
    const bool dynamicEnabled =
        clip->effectModulationMode != QStringLiteral("none");
    const bool lfoEnabled =
        clip->effectModulationMode == QStringLiteral("lfo");
    if (m_widgets.effectModulationModeCombo) {
        m_widgets.effectModulationModeCombo->setEnabled(true);
    }
    if (m_widgets.effectModulationTargetCombo) {
        m_widgets.effectModulationTargetCombo->setEnabled(dynamicEnabled);
    }
    if (m_widgets.effectModulationAmountSpin) {
        m_widgets.effectModulationAmountSpin->setEnabled(dynamicEnabled);
    }
    if (m_widgets.effectModulationRateSpin) {
        m_widgets.effectModulationRateSpin->setEnabled(lfoEnabled);
    }
    if (m_widgets.effectModulationPhaseSpin) {
        m_widgets.effectModulationPhaseSpin->setEnabled(lfoEnabled);
    }
    if (m_widgets.differenceReferenceFramesSpin) m_widgets.differenceReferenceFramesSpin->setValue(clip->differenceReferenceFrames);
    if (m_widgets.differenceThresholdSpin) m_widgets.differenceThresholdSpin->setValue(clip->differenceThreshold);
    if (m_widgets.differenceSoftnessSpin) m_widgets.differenceSoftnessSpin->setValue(clip->differenceSoftness);
    if (m_widgets.temporalEchoCountSpin) m_widgets.temporalEchoCountSpin->setValue(clip->temporalEchoCount);
    if (m_widgets.temporalEchoSpacingSpin) m_widgets.temporalEchoSpacingSpin->setValue(clip->temporalEchoSpacingFrames);
    if (m_widgets.temporalEchoDecaySpin) m_widgets.temporalEchoDecaySpin->setValue(clip->temporalEchoDecay);
    if (m_widgets.tilingPatternCombo) {
        m_widgets.tilingPatternCombo->setCurrentIndex(
            comboIndexForTilingPattern(
                m_widgets.tilingPatternCombo,
                clip->tilingPattern));
    }
    if (m_widgets.tilingSpacingSpin) {
        m_widgets.tilingSpacingSpin->setValue(clip->tilingSpacing);
    }
    if (m_widgets.tilingWrapCheck) {
        m_widgets.tilingWrapCheck->setChecked(clip->tilingWrap);
    }

    const bool imagePresetCapable = clip->mediaType == ClipMediaType::Image ||
                                    clip->mediaType == ClipMediaType::Video;
    const ClipEffectPreset clipPreset = clip->effectPreset;
    const bool imagePresetActive = clipPreset != ClipEffectPreset::None;
    if (m_widgets.effectPresetCombo) {
        m_widgets.effectPresetCombo->setEnabled(imagePresetCapable);
    }
    if (m_widgets.effectRowsSpin) {
        m_widgets.effectRowsSpin->setEnabled(imagePresetCapable && imagePresetActive);
        m_widgets.effectRowsSpin->setSuffix(QString());
        m_widgets.effectRowsSpin->setToolTip(
            QStringLiteral("Rows, copies, or repeat steps."));
    }
    if (m_widgets.effectSpeedSpin) {
        m_widgets.effectSpeedSpin->setEnabled(imagePresetCapable && imagePresetActive);
    }
    if (m_widgets.effectScaleSpin) {
        m_widgets.effectScaleSpin->setEnabled(imagePresetCapable && imagePresetActive);
        m_widgets.effectScaleSpin->setToolTip(
            QStringLiteral("Scale multiplier for the selected effect."));
    }
    if (m_widgets.effectAlternateDirectionCheck) {
        m_widgets.effectAlternateDirectionCheck->setEnabled(imagePresetCapable && imagePresetActive);
    }
    if (m_widgets.effectSpeechSyncCheck) {
        const bool steadyIncrease =
            clip->effectModulationMode == QStringLiteral("steady_increase");
        const bool progressiveEdgePreset =
            clip->edgeFillEffect == BackgroundFillEffect::ProgressiveEdgeStretch ||
            clip->edgeFillEffect ==
                BackgroundFillEffect::ProgressiveBidirectionalEdgeStretch;
        m_widgets.effectSpeechSyncCheck->setEnabled(
            imagePresetCapable && imagePresetActive &&
            (steadyIncrease || !progressiveEdgePreset));
    }
    if (m_widgets.differenceReferenceFramesSpin) m_widgets.differenceReferenceFramesSpin->setEnabled(imagePresetActive);
    if (m_widgets.differenceThresholdSpin) m_widgets.differenceThresholdSpin->setEnabled(imagePresetActive);
    if (m_widgets.differenceSoftnessSpin) m_widgets.differenceSoftnessSpin->setEnabled(imagePresetActive);
    if (m_widgets.temporalEchoCountSpin) m_widgets.temporalEchoCountSpin->setEnabled(imagePresetActive);
    if (m_widgets.temporalEchoSpacingSpin) m_widgets.temporalEchoSpacingSpin->setEnabled(imagePresetActive);
    if (m_widgets.temporalEchoDecaySpin) m_widgets.temporalEchoDecaySpin->setEnabled(imagePresetActive);
    const bool tilingControlsActive =
        imagePresetCapable && imagePresetActive;
    if (m_widgets.tilingPatternCombo) {
        m_widgets.tilingPatternCombo->setEnabled(tilingControlsActive);
    }
    if (m_widgets.tilingSpacingSpin) {
        m_widgets.tilingSpacingSpin->setEnabled(tilingControlsActive);
    }
    if (m_widgets.tilingWrapCheck) {
        m_widgets.tilingWrapCheck->setEnabled(tilingControlsActive);
    }
    updatePresetParameterVisibility(
        m_widgets, clipPreset, clip->effectModulationMode);
    m_updating = false;
}

void EffectsTab::applyEffectPreset(bool pushHistory)
{
    if (m_updating) return;

    const TimelineClip* selectedClip = m_deps.getSelectedClip();
    const int targetTrackIndex =
        selectedClip ? selectedClip->trackIndex : (m_deps.getSelectedTrackIndex ? m_deps.getSelectedTrackIndex() : -1);
    const TimelineTrack* targetTrack =
        m_deps.getTrackByIndex ? m_deps.getTrackByIndex(targetTrackIndex) : nullptr;

    const ClipEffectPreset preset = presetFromCombo(m_widgets.effectPresetCombo);
    if ((selectedClip && selectedClip->clipRole == ClipRole::MaskMatte &&
         !effectPresetSupportedForClipRole(preset, ClipRole::MaskMatte)) ||
        (!selectedClip && targetTrack && targetTrack->generatedChildTrack)) {
        // Generated rows are presentation bindings, not effect owners. Also
        // reject programmatic attempts to assign source-history effects to a
        // virtual matte even if a disabled combo item is forced current.
        refresh();
        return;
    }
    const int rows = m_widgets.effectRowsSpin ? m_widgets.effectRowsSpin->value() : 32;
    const double speed = m_widgets.effectSpeedSpin ? m_widgets.effectSpeedSpin->value() : 1.0;
    const double scale = m_widgets.effectScaleSpin ? m_widgets.effectScaleSpin->value() : 1.0;
    const ClipTilingPattern tilingPattern = tilingPatternFromCombo(m_widgets.tilingPatternCombo);
    const double tilingSpacing = m_widgets.tilingSpacingSpin ? m_widgets.tilingSpacingSpin->value() : 1.0;
    const bool tilingWrap = !m_widgets.tilingWrapCheck || m_widgets.tilingWrapCheck->isChecked();
    const bool alternate =
        !m_widgets.effectAlternateDirectionCheck || m_widgets.effectAlternateDirectionCheck->isChecked();
    const QString modulationMode =
        m_widgets.effectModulationModeCombo
            ? m_widgets.effectModulationModeCombo->currentData().toString()
            : QStringLiteral("none");
    const bool speechSync =
        (preset != ClipEffectPreset::None ||
         modulationMode == QStringLiteral("steady_increase")) &&
        m_widgets.effectSpeechSyncCheck &&
        m_widgets.effectSpeechSyncCheck->isChecked();
    const bool effectEnabled =
        !m_widgets.effectEnabledCheck ||
        m_widgets.effectEnabledCheck->isChecked();
    const QString modulationTarget =
        m_widgets.effectModulationTargetCombo
            ? m_widgets.effectModulationTargetCombo->currentData().toString()
            : QStringLiteral("scale");
    const double modulationAmount =
        m_widgets.effectModulationAmountSpin
            ? m_widgets.effectModulationAmountSpin->value() : 0.0;
    const double modulationRate =
        m_widgets.effectModulationRateSpin
            ? m_widgets.effectModulationRateSpin->value() : 1.0;
    const double modulationPhase =
        m_widgets.effectModulationPhaseSpin
            ? m_widgets.effectModulationPhaseSpin->value() : 0.0;
    const BackgroundFillEffect edgeFillEffect = backgroundFillEffectFromString(
        m_widgets.edgeFillEffectCombo
            ? m_widgets.edgeFillEffectCombo->currentData().toString()
            : QStringLiteral("none"));
    const int edgeFillPixels =
        m_widgets.edgeFillPixelsSpin ? m_widgets.edgeFillPixelsSpin->value() : 1;
    const double edgeFillPower =
        m_widgets.edgeFillPowerSpin ? m_widgets.edgeFillPowerSpin->value() : 2.0;
    const double edgeFillOpacity =
        m_widgets.edgeFillOpacitySpin ? m_widgets.edgeFillOpacitySpin->value() / 100.0 : 1.0;
    const double edgeFillBrightness =
        m_widgets.edgeFillBrightnessSpin ? m_widgets.edgeFillBrightnessSpin->value() / 100.0 : 0.0;
    const double edgeFillSaturation =
        m_widgets.edgeFillSaturationSpin ? m_widgets.edgeFillSaturationSpin->value() / 100.0 : 1.0;
    const int differenceReferenceFrames = m_widgets.differenceReferenceFramesSpin ? m_widgets.differenceReferenceFramesSpin->value() : 1;
    const double differenceThreshold = m_widgets.differenceThresholdSpin ? m_widgets.differenceThresholdSpin->value() : 0.10;
    const double differenceSoftness = m_widgets.differenceSoftnessSpin ? m_widgets.differenceSoftnessSpin->value() : 0.05;
    const int temporalEchoCount = m_widgets.temporalEchoCountSpin ? m_widgets.temporalEchoCountSpin->value() : 4;
    const int temporalEchoSpacingFrames = m_widgets.temporalEchoSpacingSpin ? m_widgets.temporalEchoSpacingSpin->value() : 2;
    const double temporalEchoDecay = m_widgets.temporalEchoDecaySpin ? m_widgets.temporalEchoDecaySpin->value() : 0.65;

    bool updated = false;
    if (selectedClip) {
        if (selectedClip->clipRole == ClipRole::EffectSynth ||
            !m_deps.clipHasVisuals(*selectedClip)) {
            refresh();
            return;
        }
        updated = m_deps.updateClipById(selectedClip->id, [=](TimelineClip& clip) {
            const ClipEffectPreset previousPreset = clip.effectPreset;
            clip.effectParameterSets[presetParameterKey(previousPreset)] = effectParameters(clip);
            clip.edgeFillEffect = edgeFillEffect;
            clip.edgeFillPixels = qBound(1, edgeFillPixels, 512);
            clip.edgeFillPower = qBound<qreal>(0.25, edgeFillPower, 8.0);
            clip.edgeFillOpacity = qBound<qreal>(0.0, edgeFillOpacity, 1.0);
            clip.edgeFillBrightness = qBound<qreal>(-1.0, edgeFillBrightness, 1.0);
            clip.edgeFillSaturation = qBound<qreal>(0.0, edgeFillSaturation, 3.0);
            clip.effectPreset = preset;
            clip.effectEnabled = effectEnabled;
            clip.effectModulationMode = modulationMode;
            clip.effectModulationTarget = modulationTarget;
            clip.effectModulationAmount =
                qBound<qreal>(-512.0, modulationAmount, 512.0);
            clip.effectModulationRate =
                qBound<qreal>(0.0, modulationRate, 20.0);
            clip.effectModulationPhaseDegrees =
                qBound<qreal>(-360.0, modulationPhase, 360.0);
            if (preset != previousPreset) {
                restoreEffectParameters(clip, clip.effectParameterSets.value(presetParameterKey(preset)).toObject());
            } else {
            clip.effectRows = qBound(1, rows, 96);
            clip.effectSpeed = qBound<qreal>(-8.0, speed, 8.0);
            clip.effectScale = qBound<qreal>(0.1, scale, 8.0);
            clip.effectAlternateDirection = alternate;
            clip.effectSkipAwareTiming = speechSync;
            clip.differenceReferenceFrames = qBound(1, differenceReferenceFrames, 300);
            clip.differenceThreshold = qBound<qreal>(0.0, differenceThreshold, 1.0);
            clip.differenceSoftness = qBound<qreal>(0.0, differenceSoftness, 1.0);
            clip.temporalEchoCount = qBound(1, temporalEchoCount, 12);
            clip.temporalEchoSpacingFrames = qBound(1, temporalEchoSpacingFrames, 120);
            clip.temporalEchoDecay = qBound<qreal>(0.0, temporalEchoDecay, 1.0);
            clip.tilingPattern = tilingPattern;
            clip.tilingSpacing = qBound<qreal>(0.1, tilingSpacing, 8.0);
            clip.tilingWrap = tilingWrap;
            }
            clip.effectParameterSets[presetParameterKey(preset)] = effectParameters(clip);
        });
    } else if (m_deps.updateTrackByIndex && targetTrackIndex >= 0 &&
               (!targetTrack || !targetTrack->generatedChildTrack)) {
        updated = m_deps.updateTrackByIndex(targetTrackIndex, [=](TimelineTrack& track) {
            const ClipEffectPreset previousPreset = track.effectPreset;
            track.effectParameterSets[presetParameterKey(previousPreset)] = effectParameters(track);
            track.effectPreset = preset;
            if (preset != previousPreset) {
                restoreEffectParameters(track, track.effectParameterSets.value(presetParameterKey(preset)).toObject());
            } else {
            track.effectRows = qBound(1, rows, 96);
            track.effectSpeed = qBound<qreal>(-8.0, speed, 8.0);
            track.effectScale = qBound<qreal>(0.1, scale, 8.0);
            track.effectAlternateDirection = alternate;
            track.differenceReferenceFrames = qBound(1, differenceReferenceFrames, 300);
            track.differenceThreshold = qBound<qreal>(0.0, differenceThreshold, 1.0);
            track.differenceSoftness = qBound<qreal>(0.0, differenceSoftness, 1.0);
            track.temporalEchoCount = qBound(1, temporalEchoCount, 12);
            track.temporalEchoSpacingFrames = qBound(1, temporalEchoSpacingFrames, 120);
            track.temporalEchoDecay = qBound<qreal>(0.0, temporalEchoDecay, 1.0);
            track.tilingPattern = tilingPattern;
            track.tilingSpacing = qBound<qreal>(0.1, tilingSpacing, 8.0);
            track.tilingWrap = tilingWrap;
            }
            track.effectParameterSets[presetParameterKey(preset)] = effectParameters(track);
        });
    }

    if (!updated) return;

    applyTabEditEffects(effectsEditCallbacks(m_deps),
                        TabEditEffects{.pushHistory = pushHistory});
    emit effectsApplied();
}

void EffectsTab::setEffectEnabledKeyframe(bool enabled)
{
    const TimelineClip* selectedClip = m_deps.getSelectedClip
        ? m_deps.getSelectedClip() : nullptr;
    if (!selectedClip || !m_deps.updateClipById) return;
    const int64_t timelineFrame =
        m_deps.currentTimelineFrame ? m_deps.currentTimelineFrame()
                                    : selectedClip->startFrame;
    const int64_t localFrame = qBound<int64_t>(
        0,
        timelineFrame - selectedClip->startFrame,
        qMax<int64_t>(0, selectedClip->durationFrames - 1));
    const QString clipId = selectedClip->id;
    if (!m_deps.updateClipById(
            clipId,
            [localFrame, enabled](TimelineClip& clip) {
                auto existing = std::find_if(
                    clip.effectEnabledKeyframes.begin(),
                    clip.effectEnabledKeyframes.end(),
                    [localFrame](const auto& keyframe) {
                        return keyframe.frame == localFrame;
                    });
                const TimelineClip::BoolKeyframe keyframe{
                    localFrame, enabled};
                if (existing == clip.effectEnabledKeyframes.end()) {
                    clip.effectEnabledKeyframes.push_back(keyframe);
                } else {
                    *existing = keyframe;
                }
                std::sort(
                    clip.effectEnabledKeyframes.begin(),
                    clip.effectEnabledKeyframes.end(),
                    [](const auto& left, const auto& right) {
                        return left.frame < right.frame;
                    });
            })) {
        return;
    }
    applyTabEditEffects(
        effectsEditCallbacks(m_deps),
        TabEditEffects{.pushHistory = true});
    emit effectsApplied();
    refresh();
}

void EffectsTab::setEffectParameterKeyframe()
{
    const TimelineClip* selectedClip = m_deps.getSelectedClip
        ? m_deps.getSelectedClip() : nullptr;
    if (!selectedClip || !m_deps.updateClipById) return;
    const int64_t timelineFrame =
        m_deps.currentTimelineFrame ? m_deps.currentTimelineFrame()
                                    : selectedClip->startFrame;
    const int64_t localFrame = qBound<int64_t>(
        0,
        timelineFrame - selectedClip->startFrame,
        qMax<int64_t>(0, selectedClip->durationFrames - 1));
    TimelineClip::EffectParameterKeyframe keyframe;
    keyframe.frame = localFrame;
    keyframe.effectRows = m_widgets.effectRowsSpin
        ? m_widgets.effectRowsSpin->value() : selectedClip->effectRows;
    keyframe.effectSpeed = m_widgets.effectSpeedSpin
        ? m_widgets.effectSpeedSpin->value() : selectedClip->effectSpeed;
    keyframe.effectScale = m_widgets.effectScaleSpin
        ? m_widgets.effectScaleSpin->value() : selectedClip->effectScale;
    keyframe.effectAlternateDirection = !m_widgets.effectAlternateDirectionCheck ||
        m_widgets.effectAlternateDirectionCheck->isChecked();
    keyframe.differenceReferenceFrames = m_widgets.differenceReferenceFramesSpin
        ? m_widgets.differenceReferenceFramesSpin->value()
        : selectedClip->differenceReferenceFrames;
    keyframe.differenceThreshold = m_widgets.differenceThresholdSpin
        ? m_widgets.differenceThresholdSpin->value()
        : selectedClip->differenceThreshold;
    keyframe.differenceSoftness = m_widgets.differenceSoftnessSpin
        ? m_widgets.differenceSoftnessSpin->value()
        : selectedClip->differenceSoftness;
    keyframe.temporalEchoCount = m_widgets.temporalEchoCountSpin
        ? m_widgets.temporalEchoCountSpin->value()
        : selectedClip->temporalEchoCount;
    keyframe.temporalEchoSpacingFrames = m_widgets.temporalEchoSpacingSpin
        ? m_widgets.temporalEchoSpacingSpin->value()
        : selectedClip->temporalEchoSpacingFrames;
    keyframe.temporalEchoDecay = m_widgets.temporalEchoDecaySpin
        ? m_widgets.temporalEchoDecaySpin->value()
        : selectedClip->temporalEchoDecay;
    keyframe.tilingPattern = m_widgets.tilingPatternCombo
        ? tilingPatternFromCombo(m_widgets.tilingPatternCombo)
        : selectedClip->tilingPattern;
    keyframe.tilingSpacing = m_widgets.tilingSpacingSpin
        ? m_widgets.tilingSpacingSpin->value()
        : selectedClip->tilingSpacing;
    keyframe.tilingWrap = !m_widgets.tilingWrapCheck ||
        m_widgets.tilingWrapCheck->isChecked();

    const QString clipId = selectedClip->id;
    if (!m_deps.updateClipById(
            clipId,
            [keyframe](TimelineClip& clip) {
                auto existing = std::find_if(
                    clip.effectParameterKeyframes.begin(),
                    clip.effectParameterKeyframes.end(),
                    [frame = keyframe.frame](const auto& value) {
                        return value.frame == frame;
                    });
                if (existing == clip.effectParameterKeyframes.end()) {
                    clip.effectParameterKeyframes.push_back(keyframe);
                } else {
                    *existing = keyframe;
                }
                std::sort(
                    clip.effectParameterKeyframes.begin(),
                    clip.effectParameterKeyframes.end(),
                    [](const auto& left, const auto& right) {
                        return left.frame < right.frame;
                    });
            })) {
        return;
    }
    applyTabEditEffects(
        effectsEditCallbacks(m_deps),
        TabEditEffects{.pushHistory = true});
    emit effectsApplied();
    refresh();
}

void EffectsTab::removeEffectEnabledKeyframe()
{
    const TimelineClip* selectedClip = m_deps.getSelectedClip
        ? m_deps.getSelectedClip() : nullptr;
    if (!selectedClip || !m_deps.updateClipById) return;
    const int64_t timelineFrame =
        m_deps.currentTimelineFrame ? m_deps.currentTimelineFrame()
                                    : selectedClip->startFrame;
    const int64_t localFrame = qBound<int64_t>(
        0,
        timelineFrame - selectedClip->startFrame,
        qMax<int64_t>(0, selectedClip->durationFrames - 1));
    const QString clipId = selectedClip->id;
    if (!m_deps.updateClipById(
            clipId,
            [localFrame](TimelineClip& clip) {
                clip.effectEnabledKeyframes.erase(
                    std::remove_if(
                        clip.effectEnabledKeyframes.begin(),
                        clip.effectEnabledKeyframes.end(),
                        [localFrame](const auto& keyframe) {
                            return keyframe.frame == localFrame;
                        }),
                    clip.effectEnabledKeyframes.end());
                clip.effectParameterKeyframes.erase(
                    std::remove_if(
                        clip.effectParameterKeyframes.begin(),
                        clip.effectParameterKeyframes.end(),
                        [localFrame](const auto& keyframe) {
                            return keyframe.frame == localFrame;
                        }),
                    clip.effectParameterKeyframes.end());
            })) {
        return;
    }
    applyTabEditEffects(
        effectsEditCallbacks(m_deps),
        TabEditEffects{.pushHistory = true});
    emit effectsApplied();
    refresh();
}

void EffectsTab::onApplyClicked()
{
    applyEffectPreset(true);
}

void EffectsTab::onEditingFinished()
{
    if (m_updating) return;
    applyEffectPreset(true);
}

void EffectsTab::onEffectPresetChanged(int index)
{
    Q_UNUSED(index);
    if (m_updating) return;
    applyEffectPreset(true);
    refresh();
}

void EffectsTab::onEffectControlChanged()
{
    if (m_updating) return;
    applyEffectPreset(false);
}
