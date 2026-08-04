#include "effects_tab.h"
#include "editor_effect_presets.h"
#include "editor_shared_effects.h"
#include "editor_shared_media.h"
#include "editor_tab_edit_effects.h"
#include "keyframe_table_shared.h"
#include <QEvent>
#include <QDial>
#include <QSignalBlocker>
#include <QDir>
#include <QFormLayout>
#include <QLabel>
#include <QMenu>
#include <QSet>
#include <QStringList>
#include <QTableWidget>
#include <QTableWidgetItem>
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

int comboIndexForPreset(const QComboBox* combo, ClipEffectPreset preset);
void setMaskMattePresetAvailability(QComboBox* combo, bool maskMatteTarget);

QString groupForEffectPreset(ClipEffectPreset preset)
{
    for (const EffectPresetUiOption& option : effectPresetUiOptions()) {
        if (option.preset == preset) {
            return option.group;
        }
    }
    return QStringLiteral("General");
}

QString selectedEffectPresetCategory(const QComboBox* combo)
{
    if (!combo) {
        return QStringLiteral("General");
    }
    const QString category = combo->currentData().toString().trimmed();
    return category.isEmpty() ? QStringLiteral("General") : category;
}

void populateEffectPresetCategoryCombo(QComboBox* combo, ClipEffectPreset selectedPreset)
{
    if (!combo) {
        return;
    }
    const QSignalBlocker blocker(combo);
    const QString selectedGroup = groupForEffectPreset(selectedPreset);
    if (combo->count() <= 0) {
        QStringList groups;
        for (const EffectPresetUiOption& option : effectPresetUiOptions()) {
            if (!groups.contains(option.group)) {
                groups.push_back(option.group);
                combo->addItem(option.group, option.group);
            }
        }
    }
    const int index = combo->findData(selectedGroup);
    combo->setCurrentIndex(index >= 0 ? index : 0);
}

void populateEffectPresetCombo(QComboBox* combo,
                               const QString& category,
                               ClipEffectPreset selectedPreset)
{
    if (!combo) {
        return;
    }
    const QSignalBlocker blocker(combo);
    combo->clear();
    const QString effectiveCategory =
        category.trimmed().isEmpty() ? groupForEffectPreset(selectedPreset)
                                     : category.trimmed();
    for (const EffectPresetUiOption& option : effectPresetUiOptions()) {
        if (option.group != effectiveCategory) {
            continue;
        }
        combo->addItem(option.label, static_cast<int>(option.preset));
        combo->setItemData(combo->count() - 1, option.group, Qt::ToolTipRole);
    }
    int selectedIndex = comboIndexForPreset(combo, selectedPreset);
    if (selectedIndex < 0 && combo->count() > 0) {
        selectedIndex = 0;
    }
    combo->setCurrentIndex(selectedIndex);
}

int comboIndexForPreset(const QComboBox* combo, ClipEffectPreset preset)
{
    if (!combo) {
        return -1;
    }
    const int index = combo->findData(static_cast<int>(preset));
    return index >= 0 ? index : combo->findData(static_cast<int>(ClipEffectPreset::None));
}

void setEffectPresetSelectorToPreset(const EffectsTab::Widgets& widgets,
                                     ClipEffectPreset preset,
                                     bool maskMatteTarget)
{
    populateEffectPresetCategoryCombo(widgets.effectPresetCategoryCombo, preset);
    populateEffectPresetCombo(
        widgets.effectPresetCombo,
        selectedEffectPresetCategory(widgets.effectPresetCategoryCombo),
        preset);
    setMaskMattePresetAvailability(widgets.effectPresetCombo, maskMatteTarget);
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

QString tilingPatternLabel(ClipTilingPattern pattern)
{
    switch (pattern) {
    case ClipTilingPattern::Grid: return QStringLiteral("Grid");
    case ClipTilingPattern::Encircle: return QStringLiteral("Encircle");
    case ClipTilingPattern::SpiralXY: return QStringLiteral("Spiral XY");
    case ClipTilingPattern::SpiralXZ: return QStringLiteral("Spiral XZ");
    case ClipTilingPattern::SpiralYZ: return QStringLiteral("Spiral YZ");
    case ClipTilingPattern::Diamond: return QStringLiteral("Diamond");
    }
    return QStringLiteral("Pattern");
}

constexpr int kEffectKeyTypeRole = Qt::UserRole + 1;

QTableWidgetItem* tableItem(const QString& text,
                            const QVariant& sortValue = QVariant(),
                            bool editable = false)
{
    auto* item = new QTableWidgetItem(text);
    if (!editable) {
        item->setFlags(item->flags() & ~Qt::ItemIsEditable);
    }
    if (sortValue.isValid()) {
        item->setData(Qt::UserRole, sortValue);
    }
    return item;
}

ClipEffectPreset effectPresetFromLabel(const QString& text,
                                       ClipEffectPreset fallback)
{
    const QString normalized = text.trimmed().toLower();
    for (const EffectPresetUiOption& option : effectPresetUiOptions()) {
        if (option.label.trimmed().toLower() == normalized) {
            return option.preset;
        }
    }
    return fallback;
}

ClipTilingPattern tilingPatternFromLabel(const QString& text,
                                         ClipTilingPattern fallback)
{
    const QString normalized = text.trimmed().toLower();
    for (const TilingPatternUiOption& option : tilingPatternUiOptions()) {
        if (option.label.trimmed().toLower() == normalized) {
            return option.pattern;
        }
    }
    return fallback;
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
                       {QStringLiteral("wrap"), owner.tilingWrap},
                       {QStringLiteral("maskBounds"), owner.tilingUseMaskBounds},
                       {QStringLiteral("maskIslandSigma"), owner.tilingMaskIslandSigma}};
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
    owner.tilingUseMaskBounds = values.value(QStringLiteral("maskBounds")).toBool(false);
    owner.tilingMaskIslandSigma =
        qBound<qreal>(0.0, values.value(QStringLiteral("maskIslandSigma")).toDouble(0.0), 100.0);
}

int directionDegreesFromEffectSpeed(qreal speed)
{
    int degrees = qRound(qBound<qreal>(-8.0, speed, 8.0) * 45.0);
    degrees %= 360;
    if (degrees < 0) {
        degrees += 360;
    }
    return degrees;
}

qreal effectSpeedFromDirectionDegrees(int degrees)
{
    return qBound<qreal>(-8.0, static_cast<qreal>(degrees % 360) / 45.0, 8.0);
}

int spreadPercentFromTilingSpacing(qreal spacing)
{
    return qBound(0, qRound((qBound<qreal>(0.1, spacing, 8.0) - 0.1) / 7.9 * 100.0), 100);
}

qreal tilingSpacingFromSpreadPercent(int percent)
{
    return qBound<qreal>(0.1, 0.1 + (qBound(0, percent, 100) / 100.0) * 7.9, 8.0);
}

int huePercentFromEffectScale(qreal scale)
{
    return qBound(0, qRound(qBound<qreal>(0.1, scale, 8.0) / 8.0 * 100.0), 100);
}

qreal effectScaleFromHuePercent(int percent)
{
    return qBound<qreal>(0.1, (qBound(0, percent, 100) / 100.0) * 8.0, 8.0);
}

int guideScalePercentFromTilingSpacing(qreal spacing)
{
    return qBound(0, qRound((qBound<qreal>(0.5, spacing, 8.0) - 0.5) / 7.5 * 100.0), 100);
}

qreal tilingSpacingFromGuideScalePercent(int percent)
{
    return qBound<qreal>(0.5, 0.5 + (qBound(0, percent, 100) / 100.0) * 7.5, 8.0);
}

int matchPercentFromEffectValue(qreal value)
{
    return qBound(0, qRound(qBound<qreal>(0.0, value, 8.0) / 8.0 * 100.0), 100);
}

qreal effectValueFromMatchPercent(int percent)
{
    return qBound<qreal>(0.0, (qBound(0, percent, 100) / 100.0) * 8.0, 8.0);
}

bool sourceMosaicPreset(ClipEffectPreset preset)
{
    return preset == ClipEffectPreset::StepRepeatFill ||
           preset == ClipEffectPreset::SourceMosaicGrid ||
           preset == ClipEffectPreset::SourceMosaicStagger ||
           preset == ClipEffectPreset::SourceMosaicHex ||
           preset == ClipEffectPreset::SourceMosaicRadial ||
           preset == ClipEffectPreset::SourceMosaicFlow;
}

bool recursiveZoomPreset(ClipEffectPreset preset)
{
    return preset == ClipEffectPreset::RecursiveZoomTile ||
           preset == ClipEffectPreset::RecursiveZoomTunnel ||
           preset == ClipEffectPreset::RecursiveZoomMirrorBox ||
           preset == ClipEffectPreset::RecursiveZoomSpiral ||
           preset == ClipEffectPreset::RecursiveZoomKaleidoscope ||
           preset == ClipEffectPreset::RecursiveZoomRadialRepeat ||
           preset == ClipEffectPreset::RecursiveZoomPixelMosaic;
}

bool generatedMaskDomainPreset(ClipEffectPreset preset)
{
    return recursiveZoomPreset(preset) || sourceMosaicPreset(preset);
}

QString compassLabelForDegrees(int degrees)
{
    static const QStringList labels{
        QStringLiteral("right"),
        QStringLiteral("down-right"),
        QStringLiteral("down"),
        QStringLiteral("down-left"),
        QStringLiteral("left"),
        QStringLiteral("up-left"),
        QStringLiteral("up"),
        QStringLiteral("up-right")};
    return labels.at(qBound(0, qRound((degrees % 360) / 45.0), 8) % 8);
}

void updateDirectionalEchoLabels(const EffectsTab::Widgets& widgets)
{
    const int directionDegrees = widgets.directionalEchoDirectionDial
        ? widgets.directionalEchoDirectionDial->value()
        : 0;
    const int spreadPercent = widgets.directionalEchoSpreadDial
        ? widgets.directionalEchoSpreadDial->value()
        : 0;
    const int huePercent = widgets.directionalEchoHueDial
        ? widgets.directionalEchoHueDial->value()
        : 0;
    const int instanceCount = widgets.effectRowsSpin
        ? widgets.effectRowsSpin->value()
        : 1;
    if (widgets.directionalEchoDirectionValueLabel && widgets.directionalEchoDirectionDial) {
        widgets.directionalEchoDirectionValueLabel->setText(
            QStringLiteral("%1° %2")
                .arg(directionDegrees)
                .arg(compassLabelForDegrees(directionDegrees)));
    }
    if (widgets.directionalEchoSpreadValueLabel && widgets.directionalEchoSpreadDial) {
        widgets.directionalEchoSpreadValueLabel->setText(
            QStringLiteral("%1% spread").arg(spreadPercent));
    }
    if (widgets.directionalEchoHueValueLabel && widgets.directionalEchoHueDial) {
        widgets.directionalEchoHueValueLabel->setText(
            QStringLiteral("±%1% hue").arg(huePercent));
    }
    if (widgets.directionalEchoSummaryLabel) {
        widgets.directionalEchoSummaryLabel->setText(
            QStringLiteral("Frame Echo: %1 instanced copies on a %2° %3 axis, "
                           "%4% spread, symmetric ±%5% hue offsets. "
                           "Animate any knob with Key Parameters.")
                .arg(instanceCount)
                .arg(directionDegrees)
                .arg(compassLabelForDegrees(directionDegrees))
                .arg(spreadPercent)
                .arg(huePercent));
    }
}

void setDirectionalEchoValues(const EffectsTab::Widgets& widgets,
                              qreal speed,
                              qreal spacing,
                              qreal scale)
{
    if (widgets.directionalEchoDirectionDial) {
        widgets.directionalEchoDirectionDial->setValue(
            directionDegreesFromEffectSpeed(speed));
    }
    if (widgets.directionalEchoSpreadDial) {
        widgets.directionalEchoSpreadDial->setValue(
            spreadPercentFromTilingSpacing(spacing));
    }
    if (widgets.directionalEchoHueDial) {
        widgets.directionalEchoHueDial->setValue(huePercentFromEffectScale(scale));
    }
    updateDirectionalEchoLabels(widgets);
}

void updateStepRepeatFillLabels(const EffectsTab::Widgets& widgets)
{
    const int guideScale = widgets.stepRepeatFillGuideScaleDial
        ? widgets.stepRepeatFillGuideScaleDial->value()
        : 0;
    const int lumaMatch = widgets.stepRepeatFillLumaMatchDial
        ? widgets.stepRepeatFillLumaMatchDial->value()
        : 0;
    const int hueMatch = widgets.stepRepeatFillHueMatchDial
        ? widgets.stepRepeatFillHueMatchDial->value()
        : 0;
    const int density = widgets.effectRowsSpin
        ? widgets.effectRowsSpin->value()
        : 1;
    if (widgets.stepRepeatFillGuideScaleValueLabel) {
        widgets.stepRepeatFillGuideScaleValueLabel->setText(
            QStringLiteral("%1% guide").arg(guideScale));
    }
    if (widgets.stepRepeatFillLumaMatchValueLabel) {
        widgets.stepRepeatFillLumaMatchValueLabel->setText(
            QStringLiteral("%1% luma").arg(lumaMatch));
    }
    if (widgets.stepRepeatFillHueMatchValueLabel) {
        widgets.stepRepeatFillHueMatchValueLabel->setText(
            QStringLiteral("%1% hue").arg(hueMatch));
    }
    if (widgets.stepRepeatFillSummaryLabel) {
        widgets.stepRepeatFillSummaryLabel->setText(
            QStringLiteral("Source Mosaic: %1 cells across, %2% guide scale, "
                           "%3% brightness match, %4% hue match. "
                           "Animate density or any knob with Key Parameters.")
                .arg(density)
                .arg(guideScale)
                .arg(lumaMatch)
                .arg(hueMatch));
    }
}

void setStepRepeatFillValues(const EffectsTab::Widgets& widgets,
                             qreal speed,
                             qreal spacing,
                             qreal scale)
{
    if (widgets.stepRepeatFillGuideScaleDial) {
        widgets.stepRepeatFillGuideScaleDial->setValue(
            guideScalePercentFromTilingSpacing(spacing));
    }
    if (widgets.stepRepeatFillLumaMatchDial) {
        widgets.stepRepeatFillLumaMatchDial->setValue(
            matchPercentFromEffectValue(speed));
    }
    if (widgets.stepRepeatFillHueMatchDial) {
        widgets.stepRepeatFillHueMatchDial->setValue(
            matchPercentFromEffectValue(scale));
    }
    updateStepRepeatFillLabels(widgets);
}

QString presetParameterKey(ClipEffectPreset preset)
{
    return QString::number(static_cast<int>(preset));
}

QString presetSpecificHelpText(ClipEffectPreset preset)
{
    switch (preset) {
    case ClipEffectPreset::None:
        return QStringLiteral("Select a synthesis preset. This section then shows only the controls that affect that preset.");
    case ClipEffectPreset::NewsLogoTicker:
    case ClipEffectPreset::AlternatingMotionBackground:
    case ClipEffectPreset::FreezePattern:
    case ClipEffectPreset::StepRepeat:
        return QStringLiteral("Step Repeat controls. Copies sets the sequenced repeat count, speed advances the stepped pattern, and scale sets source size.");
    case ClipEffectPreset::StepRepeatFill:
        return QStringLiteral("Step Repeat Fill controls. Tile density sets the repeat field, and the dedicated knobs match repeated tiles to a larger superimposed source guide.");
    case ClipEffectPreset::SourceMosaicGrid:
        return QStringLiteral("Source Mosaic Grid controls. A single-pass photomosaic shader builds a regular field of source fragments and matches them to a macro guide.");
    case ClipEffectPreset::SourceMosaicStagger:
        return QStringLiteral("Source Mosaic Stagger controls. A single-pass photomosaic shader offsets alternating rows for a more broadcast-graphic tile field.");
    case ClipEffectPreset::SourceMosaicHex:
        return QStringLiteral("Source Mosaic Hex controls. A single-pass photomosaic shader uses a hex-style packing field for denser organic texture.");
    case ClipEffectPreset::SourceMosaicRadial:
        return QStringLiteral("Source Mosaic Radial controls. A single-pass photomosaic shader arranges source fragments in polar bands around the frame.");
    case ClipEffectPreset::SourceMosaicFlow:
        return QStringLiteral("Source Mosaic Flow controls. A single-pass photomosaic shader warps the tile field with a stable broadcast-style flow pattern.");
    case ClipEffectPreset::DirectionalFrameEcho:
        return QStringLiteral("Directional Frame Echo controls. Instances sets the stack count, and the dedicated knobs set direction, spread, and symmetric hue balance.");
    case ClipEffectPreset::DirectionalTrimTicker:
    case ClipEffectPreset::SourceTile:
        return QStringLiteral("Pattern/repetition controls. Copies sets density, speed drives motion, scale changes source size, and spacing/pattern are shown when the preset supports tiled layout.");
    case ClipEffectPreset::PersonOrbit:
        return QStringLiteral("Orbit controls. Copies sets orbiting instances, speed rotates them around the subject, and scale sets instance size.");
    case ClipEffectPreset::Vulkan3DSynth:
        return QStringLiteral("Procedural 3D controls. Copies sets structural density, speed animates the synth, and scale controls overall form size.");
    case ClipEffectPreset::DifferenceMatte:
        return QStringLiteral("Source-history controls. Reference chooses how far back to compare, threshold chooses what changes count, and softness feathers the detected difference.");
    case ClipEffectPreset::TemporalEcho:
        return QStringLiteral("Source-history controls. Echo frames sets the trail count, spacing sets temporal distance, and decay sets how quickly older echoes fade.");
    case ClipEffectPreset::SobelEdges:
        return QStringLiteral("Edge extraction controls. Sample radius changes edge sampling width and edge strength controls the rendered edge intensity.");
    case ClipEffectPreset::NeonGlow:
        return QStringLiteral("Glow controls. Glow radius sets spread, hue speed animates color drift, and glow intensity controls brightness.");
    case ClipEffectPreset::SpeakerMaskDilation:
    case ClipEffectPreset::SpeakerMaskDilationPulse:
    case ClipEffectPreset::SpeakerMaskDilationRings:
        return QStringLiteral("Speaker-mask controls. Dilation radius expands the mask, opacity controls overlay strength, and color spacing/speed tune the generated accent treatment.");
    case ClipEffectPreset::MirrorRing:
    case ClipEffectPreset::Kaleidoscope:
        return QStringLiteral("Angular mirror controls. Mirror sectors sets radial segmentation, rotation speed animates it, output grain size controls source sampling, and geometry amount shapes the mirrored layout.");
    case ClipEffectPreset::QuadMirror:
    case ClipEffectPreset::Tessellation:
    case ClipEffectPreset::HexagonalPrism:
        return QStringLiteral("Cell mirror controls. Cells across sets grid density, rotation speed animates the geometry, output grain size controls source sampling, and geometry amount shapes the cells.");
    case ClipEffectPreset::RecursiveZoomTile:
        return QStringLiteral("Object recursive tile controls. Object zoom ramps from exact mask-box repeats toward pixel-scale tile density before resolving at whole numbers. Use Mask Bounding Box to choose the object tile domain.");
    case ClipEffectPreset::Droste:
    case ClipEffectPreset::InfiniteMirror:
    case ClipEffectPreset::RecursiveZoomTunnel:
    case ClipEffectPreset::RecursiveZoomMirrorBox:
    case ClipEffectPreset::RecursiveZoomSpiral:
    case ClipEffectPreset::RecursiveZoomKaleidoscope:
    case ClipEffectPreset::RecursiveZoomRadialRepeat:
    case ClipEffectPreset::RecursiveZoomPixelMosaic:
        return QStringLiteral("Recursive mirror controls. Recursion density sets layer count, rotation speed animates the recursion, output grain size controls source sampling, and recursion spacing shapes depth/twist.");
    default:
        return QStringLiteral("Preset-specific controls. Only parameters used by the selected effect are shown.");
    }
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

void updateEdgeFillParameterVisibility(const EffectsTab::Widgets& widgets,
                                       BackgroundFillEffect effect)
{
    const bool enabled = effect != BackgroundFillEffect::None;
    const bool progressive =
        effect == BackgroundFillEffect::ProgressiveEdgeStretch ||
        effect == BackgroundFillEffect::ProgressiveBidirectionalEdgeStretch;
    setFormFieldVisible(widgets.edgeFillPixelsSpin, enabled && progressive);
    setFormFieldVisible(widgets.edgeFillPowerSpin, enabled && progressive);
    setFormFieldVisible(widgets.edgeFillOpacitySpin, enabled);
    setFormFieldVisible(widgets.edgeFillBrightnessSpin, enabled);
    setFormFieldVisible(widgets.edgeFillSaturationSpin, enabled);
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

QString effectPresetLabel(ClipEffectPreset preset)
{
    for (const EffectPresetUiOption& option : effectPresetUiOptions()) {
        if (option.preset == preset) {
            return option.label;
        }
    }
    return QStringLiteral("Off");
}

void updatePresetParameterVisibility(const EffectsTab::Widgets& widgets,
                                     ClipEffectPreset preset,
                                     const QString& modulationMode,
                                     bool maskClip)
{
    if (widgets.effectPresetSpecificHelpLabel) {
        widgets.effectPresetSpecificHelpLabel->setText(presetSpecificHelpText(preset));
    }
    const bool mirrorGeometry = mirrorGeometryPreset(preset);
    const bool commonParameters =
        preset == ClipEffectPreset::NewsLogoTicker ||
        preset == ClipEffectPreset::PersonOrbit ||
        preset == ClipEffectPreset::AlternatingMotionBackground ||
        preset == ClipEffectPreset::FreezePattern ||
        preset == ClipEffectPreset::StepRepeat ||
        preset == ClipEffectPreset::StepRepeatFill ||
        preset == ClipEffectPreset::SourceMosaicGrid ||
        preset == ClipEffectPreset::SourceMosaicStagger ||
        preset == ClipEffectPreset::SourceMosaicHex ||
        preset == ClipEffectPreset::SourceMosaicRadial ||
        preset == ClipEffectPreset::SourceMosaicFlow ||
        preset == ClipEffectPreset::DirectionalFrameEcho ||
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
    const bool objectRecursiveTile =
        preset == ClipEffectPreset::RecursiveZoomTile;
    const bool maskDomain = maskClip || tiling || generatedMaskDomainPreset(preset);
    const bool directionalEcho = preset == ClipEffectPreset::DirectionalFrameEcho;
    const bool stepRepeatFill = sourceMosaicPreset(preset);
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
    const bool hexagonalPrism = preset == ClipEffectPreset::HexagonalPrism;
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
                                                : objectRecursiveTile ? QStringLiteral("Tile density")
                                                : recursionEffect ? QStringLiteral("Recursion density")
                                                : cellEffect ? QStringLiteral("Cells across")
                                                : stepRepeatFill ? QStringLiteral("Tile density")
                                                : directionalEcho ? QStringLiteral("Instances")
                                                                  : QStringLiteral("Copies"));
    setFormFieldLabel(widgets.effectSpeedSpin,
                      neon ? QStringLiteral("Hue speed")
                           : speakerMask ? QStringLiteral("Color cycle speed")
                                        : objectRecursiveTile ? QStringLiteral("Tile drift")
                                        : recursionEffect ? QStringLiteral("Drift speed")
                                        : mirrorGeometry ? QStringLiteral("Rotation speed")
                                         : QStringLiteral("Speed"));
    setFormFieldLabel(widgets.effectScaleSpin,
                      edge ? QStringLiteral("Edge strength")
                           : neon ? QStringLiteral("Glow intensity")
                                  : speakerMask ? QStringLiteral("Opacity")
                                                : objectRecursiveTile ? QStringLiteral("Object zoom")
                                                : recursionEffect ? QStringLiteral("Effect zoom")
                                                : mirrorGeometry ? QStringLiteral("Source grain size")
                                                : QStringLiteral("Scale"));
    setFormFieldLabel(widgets.tilingSpacingSpin,
                      speakerMask ? QStringLiteral("Color spacing")
                                  : objectRecursiveTile ? QStringLiteral("Tile recursion spacing")
                                  : recursionEffect ? QStringLiteral("Recursion spacing")
                                  : mirrorGeometry ? QStringLiteral("Geometry amount")
                                                   : QStringLiteral("Spacing"));
    if (mirrorGeometry) {
        if (widgets.effectRowsSpin) {
            widgets.effectRowsSpin->setToolTip(
                sectorEffect
                    ? QStringLiteral("Number of mirrored angular sectors.")
                    : objectRecursiveTile
                          ? QStringLiteral("Density pressure for the object-domain recursive tiles. Higher values reach pixel-scale tiling sooner.")
                    : recursionEffect
                          ? QStringLiteral("Density of nested recursive bands.")
                          : QStringLiteral("Number of repeated mirror cells across the output."));
        }
        if (widgets.effectScaleSpin) {
            widgets.effectScaleSpin->setToolTip(
                objectRecursiveTile
                    ? QStringLiteral("Continuous object-domain zoom. The end of each period approaches pixel-scale tiles, and whole numbers resolve to exact repeated mask-box tiles.")
                    : recursionEffect
                    ? QStringLiteral("Continuous recursive zoom. Keyframe this value; whole numbers resolve back to the original source image.")
                    : QStringLiteral("Size of the sampled source grain. Higher values produce larger image features."));
        }
        if (widgets.effectSpeedSpin) {
            widgets.effectSpeedSpin->setToolTip(
                objectRecursiveTile
                    ? QStringLiteral("Independent tile drift in object space. This does not depend on transport or preview zoom.")
                    : recursionEffect
                    ? QStringLiteral("Independent drift/rotation rate for the recursive geometry. This does not drive zoom.")
                    : QStringLiteral("Rotation rate. Negative values reverse direction; zero holds the geometry still."));
        }
        if (widgets.tilingSpacingSpin) {
            widgets.tilingSpacingSpin->setToolTip(
                objectRecursiveTile
                    ? QStringLiteral("Spacing between recursive object tile levels.")
                    : recursionEffect
                    ? QStringLiteral("Spacing and twist between recursive levels.")
                    : QStringLiteral("Strength of the radial or cell geometry."));
        }
    }
    if (hexagonalPrism && widgets.effectRowsSpin) {
        widgets.effectRowsSpin->setToolTip(
            QStringLiteral("Hexagon density. This is the primary visible size control for Hexagonal Prism."));
    }
    setFormFieldVisible(widgets.effectRowsSpin, commonParameters);
    setFormFieldVisible(widgets.effectSpeedSpin, commonParameters && !edge && !directionalEcho && !stepRepeatFill);
    setFormFieldVisible(widgets.effectScaleSpin, commonParameters && !hexagonalPrism && !directionalEcho && !stepRepeatFill);
    setFormFieldVisible(widgets.effectAlternateDirectionCheck,
                        commonParameters && !edge && !neon && !speakerMask &&
                            !mirrorGeometry && !directionalEcho && !stepRepeatFill);
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
    setFormFieldVisible(widgets.tilingSpacingSpin, spacing && !directionalEcho && !stepRepeatFill);
    setFormFieldVisible(widgets.tilingWrapCheck, tiling);
    setFormFieldVisible(widgets.maskBoundingBoxSection, maskDomain);
    setFormFieldVisible(widgets.tilingUseMaskBoundsCheck, maskDomain);
    setFormFieldVisible(widgets.tilingMaskIslandSigmaSpin, maskDomain);
    setFormFieldVisible(widgets.maskBoundingBoxPreviewCheck, maskDomain);
    setFormFieldVisible(widgets.directionalEchoControlsWidget, directionalEcho);
    setFormFieldVisible(widgets.stepRepeatFillControlsWidget, stepRepeatFill);
    updateDirectionalEchoLabels(widgets);
    updateStepRepeatFillLabels(widgets);
}

void populateEffectKeyframeTable(const EffectsTab::Widgets& widgets,
                                 const TimelineClip* clip)
{
    QTableWidget* table = widgets.effectKeyframeTable;
    if (!table) {
        return;
    }

    QSignalBlocker blocker(table);
    if (table->columnCount() < 9) {
        table->setColumnCount(9);
        table->setHorizontalHeaderLabels(
            {QStringLiteral("Frame"),
             QStringLiteral("Type"),
             QStringLiteral("Preset"),
             QStringLiteral("State"),
             QStringLiteral("Copies"),
             QStringLiteral("Speed"),
             QStringLiteral("Scale"),
             QStringLiteral("Pattern"),
             QStringLiteral("Other")});
    }
    table->setRowCount(0);
    if (!clip) {
        table->setEnabled(false);
        return;
    }

    struct Row {
        int64_t frame = 0;
        QString type;
        QString preset;
        QString state;
        QString copies;
        QString speed;
        QString scale;
        QString pattern;
        QString other;
        QString keyType;
    };

    QVector<Row> rows;
    rows.reserve(clip->effectEnabledKeyframes.size() +
                 clip->effectParameterKeyframes.size());
    for (const TimelineClip::BoolKeyframe& keyframe :
         clip->effectEnabledKeyframes) {
        rows.push_back(Row{keyframe.frame,
                           QStringLiteral("Enabled"),
                           QString(),
                           keyframe.enabled ? QStringLiteral("On")
                                            : QStringLiteral("Off"),
                           QString(), QString(), QString(), QString(),
                           QString(),
                           QStringLiteral("enabled")});
    }
    for (const TimelineClip::EffectParameterKeyframe& keyframe :
         clip->effectParameterKeyframes) {
        rows.push_back(Row{keyframe.frame,
                           QStringLiteral("Parameters"),
                           keyframe.effectPresetKeyframed
                               ? effectPresetLabel(keyframe.effectPreset)
                               : QString(),
                           QString(),
                           QString::number(keyframe.effectRows),
                           QString::number(keyframe.effectSpeed, 'f', 2),
                           QString::number(keyframe.effectScale, 'f', 2),
                           tilingPatternLabel(keyframe.tilingPattern),
                           QStringLiteral(
                               "alt=%1 diff=%2/%3/%4 echo=%5/%6/%7 spacing=%8 wrap=%9 bounds=%10 outside=%11% mod=%12/%13/%14/%15/%16 speech=%17")
                               .arg(keyframe.effectAlternateDirection
                                        ? QStringLiteral("on")
                                        : QStringLiteral("off"))
                               .arg(keyframe.differenceReferenceFrames)
                               .arg(keyframe.differenceThreshold, 0, 'f', 3)
                               .arg(keyframe.differenceSoftness, 0, 'f', 3)
                               .arg(keyframe.temporalEchoCount)
                               .arg(keyframe.temporalEchoSpacingFrames)
                               .arg(keyframe.temporalEchoDecay, 0, 'f', 2)
                               .arg(keyframe.tilingSpacing, 0, 'f', 2)
                               .arg(keyframe.tilingWrap
                                        ? QStringLiteral("on")
                                        : QStringLiteral("off"))
                               .arg(keyframe.tilingUseMaskBounds
                                        ? QStringLiteral("mask")
                                        : QStringLiteral("clip"))
                               .arg(keyframe.tilingMaskIslandSigma, 0, 'f', 2)
                               .arg(keyframe.effectModulationMode)
                               .arg(keyframe.effectModulationTarget)
                               .arg(keyframe.effectModulationAmount, 0, 'f', 2)
                               .arg(keyframe.effectModulationRate, 0, 'f', 2)
                               .arg(keyframe.effectModulationPhaseDegrees, 0, 'f', 1)
                               .arg(keyframe.effectSkipAwareTiming
                                        ? QStringLiteral("on")
                                        : QStringLiteral("off")),
                           QStringLiteral("parameters")});
    }
    std::sort(rows.begin(), rows.end(), [](const Row& left, const Row& right) {
        if (left.frame != right.frame) {
            return left.frame < right.frame;
        }
        return left.type < right.type;
    });

    table->setEnabled(true);
    table->setRowCount(rows.size());
    for (int row = 0; row < rows.size(); ++row) {
        const Row& value = rows.at(row);
        const bool enabledRow = value.keyType == QStringLiteral("enabled");
        const bool parameterRow = value.keyType == QStringLiteral("parameters");
        table->setItem(row, 0, tableItem(
            QString::number(value.frame),
            QVariant::fromValue(static_cast<qlonglong>(value.frame)),
            true));
        table->setItem(row, 1, tableItem(value.type));
        table->setItem(row, 2, tableItem(value.preset, QVariant(), parameterRow));
        table->setItem(row, 3, tableItem(value.state, QVariant(), enabledRow));
        table->setItem(row, 4, tableItem(value.copies, QVariant(), parameterRow));
        table->setItem(row, 5, tableItem(value.speed, QVariant(), parameterRow));
        table->setItem(row, 6, tableItem(value.scale, QVariant(), parameterRow));
        table->setItem(row, 7, tableItem(value.pattern, QVariant(), parameterRow));
        table->setItem(row, 8, tableItem(value.other));
        for (int column = 0; column < table->columnCount(); ++column) {
            if (QTableWidgetItem* item = table->item(row, column)) {
                item->setData(
                    Qt::UserRole,
                    QVariant::fromValue(static_cast<qlonglong>(value.frame)));
                item->setData(kEffectKeyTypeRole, value.keyType);
            }
        }
        table->item(row, 1)->setData(Qt::UserRole, value.keyType);
        table->item(row, 1)->setData(kEffectKeyTypeRole, value.keyType);
    }
}

QSet<QPair<int64_t, QString>> selectedEffectKeyframeRows(QTableWidget* table)
{
    QSet<QPair<int64_t, QString>> selected;
    if (!table) {
        return selected;
    }
    auto addRow = [&](int row) {
        if (row < 0 || row >= table->rowCount()) {
            return;
        }
        const QTableWidgetItem* frameItem = table->item(row, 0);
        const QTableWidgetItem* typeItem = table->item(row, 1);
        if (!frameItem || !typeItem) {
            return;
        }
        selected.insert({frameItem->data(Qt::UserRole).toLongLong(),
                         typeItem->data(Qt::UserRole).toString()});
    };
    if (QItemSelectionModel* selection = table->selectionModel()) {
        for (const QModelIndex& index : selection->selectedRows()) {
            addRow(index.row());
        }
    }
    for (const QTableWidgetSelectionRange& range : table->selectedRanges()) {
        for (int row = range.topRow(); row <= range.bottomRow(); ++row) {
            addRow(row);
        }
    }
    return selected;
}

TimelineClip::EffectParameterKeyframe effectParameterKeyframeFromWidgets(
    const EffectsTab::Widgets& widgets,
    const TimelineClip& fallbackClip,
    int64_t localFrame)
{
    TimelineClip::EffectParameterKeyframe keyframe;
    keyframe.frame = localFrame;
    keyframe.effectPreset = presetFromCombo(widgets.effectPresetCombo);
    keyframe.effectPresetKeyframed = true;
    keyframe.effectRows = widgets.effectRowsSpin
        ? widgets.effectRowsSpin->value()
        : fallbackClip.effectRows;
    keyframe.effectSpeed = widgets.effectSpeedSpin
        ? widgets.effectSpeedSpin->value()
        : fallbackClip.effectSpeed;
    keyframe.effectScale = widgets.effectScaleSpin
        ? widgets.effectScaleSpin->value()
        : fallbackClip.effectScale;
    keyframe.effectAlternateDirection =
        !widgets.effectAlternateDirectionCheck ||
        widgets.effectAlternateDirectionCheck->isChecked();
    keyframe.differenceReferenceFrames = widgets.differenceReferenceFramesSpin
        ? widgets.differenceReferenceFramesSpin->value()
        : fallbackClip.differenceReferenceFrames;
    keyframe.differenceThreshold = widgets.differenceThresholdSpin
        ? widgets.differenceThresholdSpin->value()
        : fallbackClip.differenceThreshold;
    keyframe.differenceSoftness = widgets.differenceSoftnessSpin
        ? widgets.differenceSoftnessSpin->value()
        : fallbackClip.differenceSoftness;
    keyframe.temporalEchoCount = widgets.temporalEchoCountSpin
        ? widgets.temporalEchoCountSpin->value()
        : fallbackClip.temporalEchoCount;
    keyframe.temporalEchoSpacingFrames = widgets.temporalEchoSpacingSpin
        ? widgets.temporalEchoSpacingSpin->value()
        : fallbackClip.temporalEchoSpacingFrames;
    keyframe.temporalEchoDecay = widgets.temporalEchoDecaySpin
        ? widgets.temporalEchoDecaySpin->value()
        : fallbackClip.temporalEchoDecay;
    keyframe.tilingPattern = widgets.tilingPatternCombo
        ? tilingPatternFromCombo(widgets.tilingPatternCombo)
        : fallbackClip.tilingPattern;
    keyframe.tilingSpacing = widgets.tilingSpacingSpin
        ? widgets.tilingSpacingSpin->value()
        : fallbackClip.tilingSpacing;
    keyframe.tilingWrap =
        !widgets.tilingWrapCheck || widgets.tilingWrapCheck->isChecked();
    keyframe.tilingUseMaskBounds =
        widgets.tilingUseMaskBoundsCheck &&
        widgets.tilingUseMaskBoundsCheck->isChecked();
    keyframe.tilingMaskIslandSigma = widgets.tilingMaskIslandSigmaSpin
        ? widgets.tilingMaskIslandSigmaSpin->value()
        : fallbackClip.tilingMaskIslandSigma;
    if (keyframe.effectPreset == ClipEffectPreset::DirectionalFrameEcho) {
        keyframe.effectSpeed = widgets.directionalEchoDirectionDial
            ? effectSpeedFromDirectionDegrees(widgets.directionalEchoDirectionDial->value())
            : fallbackClip.effectSpeed;
        keyframe.effectScale = widgets.directionalEchoHueDial
            ? effectScaleFromHuePercent(widgets.directionalEchoHueDial->value())
            : fallbackClip.effectScale;
        keyframe.tilingSpacing = widgets.directionalEchoSpreadDial
            ? tilingSpacingFromSpreadPercent(widgets.directionalEchoSpreadDial->value())
            : fallbackClip.tilingSpacing;
    } else if (sourceMosaicPreset(keyframe.effectPreset)) {
        keyframe.effectSpeed = widgets.stepRepeatFillLumaMatchDial
            ? effectValueFromMatchPercent(widgets.stepRepeatFillLumaMatchDial->value())
            : fallbackClip.effectSpeed;
        keyframe.effectScale = widgets.stepRepeatFillHueMatchDial
            ? effectValueFromMatchPercent(widgets.stepRepeatFillHueMatchDial->value())
            : fallbackClip.effectScale;
        keyframe.tilingSpacing = widgets.stepRepeatFillGuideScaleDial
            ? tilingSpacingFromGuideScalePercent(widgets.stepRepeatFillGuideScaleDial->value())
            : fallbackClip.tilingSpacing;
    }
    keyframe.effectModulationMode = widgets.effectModulationModeCombo
        ? widgets.effectModulationModeCombo->currentData().toString()
        : fallbackClip.effectModulationMode;
    keyframe.effectModulationTarget = widgets.effectModulationTargetCombo
        ? widgets.effectModulationTargetCombo->currentData().toString()
        : fallbackClip.effectModulationTarget;
    keyframe.effectModulationAmount = widgets.effectModulationAmountSpin
        ? widgets.effectModulationAmountSpin->value()
        : fallbackClip.effectModulationAmount;
    keyframe.effectModulationRate = widgets.effectModulationRateSpin
        ? widgets.effectModulationRateSpin->value()
        : fallbackClip.effectModulationRate;
    keyframe.effectModulationPhaseDegrees = widgets.effectModulationPhaseSpin
        ? widgets.effectModulationPhaseSpin->value()
        : fallbackClip.effectModulationPhaseDegrees;
    keyframe.effectSkipAwareTiming =
        widgets.effectSpeechSyncCheck
            ? widgets.effectSpeechSyncCheck->isChecked()
            : fallbackClip.effectSkipAwareTiming;
    return keyframe;
}

void upsertClipEffectParameterKeyframe(
    TimelineClip& clip,
    const TimelineClip::EffectParameterKeyframe& keyframe)
{
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
    if (m_widgets.effectPresetCategoryCombo) {
        connect(m_widgets.effectPresetCategoryCombo,
                qOverload<int>(&QComboBox::currentIndexChanged),
                this,
                &EffectsTab::onEffectPresetCategoryChanged);
    }
    if (m_widgets.effectPresetCombo) {
        connect(m_widgets.effectPresetCombo, qOverload<int>(&QComboBox::currentIndexChanged),
                this, &EffectsTab::onEffectPresetChanged);
    }
    if (m_widgets.effectPresetPreviousButton) {
        connect(m_widgets.effectPresetPreviousButton, &QPushButton::clicked,
                this, [this]() { stepEffectPreset(-1); });
    }
    if (m_widgets.effectPresetNextButton) {
        connect(m_widgets.effectPresetNextButton, &QPushButton::clicked,
                this, [this]() { stepEffectPreset(1); });
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
    if (m_widgets.effectKeyframeTable) {
        m_widgets.effectKeyframeTable->setSelectionBehavior(
            QAbstractItemView::SelectRows);
        m_widgets.effectKeyframeTable->setSelectionMode(
            QAbstractItemView::ExtendedSelection);
        connect(m_widgets.effectKeyframeTable, &QTableWidget::itemChanged,
                this, &EffectsTab::onEffectKeyframeTableItemChanged);
        connect(m_widgets.effectKeyframeTable, &QTableWidget::itemDoubleClicked,
                this, &EffectsTab::onEffectKeyframeTableItemDoubleClicked);
        m_widgets.effectKeyframeTable->setContextMenuPolicy(Qt::CustomContextMenu);
        connect(m_widgets.effectKeyframeTable, &QWidget::customContextMenuRequested,
                this, &EffectsTab::onEffectKeyframeTableCustomContextMenu);
        m_widgets.effectKeyframeTable->installEventFilter(this);
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
    if (m_widgets.tilingUseMaskBoundsCheck) {
        connect(m_widgets.tilingUseMaskBoundsCheck, &QCheckBox::toggled,
                this, &EffectsTab::onEffectControlChanged);
    }
    if (m_widgets.tilingMaskIslandSigmaSpin) {
        connect(m_widgets.tilingMaskIslandSigmaSpin,
                qOverload<double>(&QDoubleSpinBox::valueChanged),
                this, &EffectsTab::onEffectControlChanged);
        connect(m_widgets.tilingMaskIslandSigmaSpin,
                &QDoubleSpinBox::editingFinished,
                this, &EffectsTab::onEditingFinished);
    }
    if (m_widgets.maskBoundingBoxPreviewCheck) {
        connect(m_widgets.maskBoundingBoxPreviewCheck, &QCheckBox::toggled,
                this, &EffectsTab::onEffectControlChanged);
    }
    for (QDial* dial : {
             m_widgets.directionalEchoDirectionDial,
             m_widgets.directionalEchoSpreadDial,
             m_widgets.directionalEchoHueDial}) {
        if (dial) {
            connect(dial, &QDial::valueChanged,
                    this, [this]() {
                        updateDirectionalEchoLabels(m_widgets);
                        onEffectControlChanged();
                    });
            connect(dial, &QDial::sliderReleased,
                    this, &EffectsTab::onEditingFinished);
        }
    }
    for (QDial* dial : {
             m_widgets.stepRepeatFillGuideScaleDial,
             m_widgets.stepRepeatFillLumaMatchDial,
             m_widgets.stepRepeatFillHueMatchDial}) {
        if (dial) {
            connect(dial, &QDial::valueChanged,
                    this, [this]() {
                        updateStepRepeatFillLabels(m_widgets);
                        onEffectControlChanged();
                    });
            connect(dial, &QDial::sliderReleased,
                    this, &EffectsTab::onEditingFinished);
        }
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
    m_updating = true;

    const std::unique_ptr<QSignalBlocker> presetCategoryBlock =
        m_widgets.effectPresetCategoryCombo
            ? std::make_unique<QSignalBlocker>(m_widgets.effectPresetCategoryCombo)
            : nullptr;
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
    const std::unique_ptr<QSignalBlocker> tilingUseMaskBoundsBlock =
        m_widgets.tilingUseMaskBoundsCheck ? std::make_unique<QSignalBlocker>(m_widgets.tilingUseMaskBoundsCheck) : nullptr;
    const std::unique_ptr<QSignalBlocker> tilingMaskIslandSigmaBlock =
        m_widgets.tilingMaskIslandSigmaSpin ? std::make_unique<QSignalBlocker>(m_widgets.tilingMaskIslandSigmaSpin) : nullptr;
    const std::unique_ptr<QSignalBlocker> directionalEchoDirectionBlock =
        m_widgets.directionalEchoDirectionDial ? std::make_unique<QSignalBlocker>(m_widgets.directionalEchoDirectionDial) : nullptr;
    const std::unique_ptr<QSignalBlocker> directionalEchoSpreadBlock =
        m_widgets.directionalEchoSpreadDial ? std::make_unique<QSignalBlocker>(m_widgets.directionalEchoSpreadDial) : nullptr;
    const std::unique_ptr<QSignalBlocker> directionalEchoHueBlock =
        m_widgets.directionalEchoHueDial ? std::make_unique<QSignalBlocker>(m_widgets.directionalEchoHueDial) : nullptr;
    const std::unique_ptr<QSignalBlocker> stepRepeatFillGuideScaleBlock =
        m_widgets.stepRepeatFillGuideScaleDial ? std::make_unique<QSignalBlocker>(m_widgets.stepRepeatFillGuideScaleDial) : nullptr;
    const std::unique_ptr<QSignalBlocker> stepRepeatFillLumaMatchBlock =
        m_widgets.stepRepeatFillLumaMatchDial ? std::make_unique<QSignalBlocker>(m_widgets.stepRepeatFillLumaMatchDial) : nullptr;
    const std::unique_ptr<QSignalBlocker> stepRepeatFillHueMatchBlock =
        m_widgets.stepRepeatFillHueMatchDial ? std::make_unique<QSignalBlocker>(m_widgets.stepRepeatFillHueMatchDial) : nullptr;

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
        updateEdgeFillParameterVisibility(m_widgets, BackgroundFillEffect::None);
        const ClipEffectPreset trackPreset =
            selectedTrack ? selectedTrack->effectPreset : ClipEffectPreset::None;
        setEffectPresetSelectorToPreset(m_widgets, trackPreset, maskMatteTarget);
        const bool trackPresetSelectable =
            selectedTrack != nullptr && !selectedTrack->generatedChildTrack;
        if (m_widgets.effectPresetCategoryCombo) {
            m_widgets.effectPresetCategoryCombo->setEnabled(trackPresetSelectable);
        }
        if (m_widgets.effectPresetCombo) {
            m_widgets.effectPresetCombo->setEnabled(
                trackPresetSelectable);
        }
        if (m_widgets.effectPresetPreviousButton) {
            m_widgets.effectPresetPreviousButton->setEnabled(trackPresetSelectable);
        }
        if (m_widgets.effectPresetNextButton) {
            m_widgets.effectPresetNextButton->setEnabled(trackPresetSelectable);
        }
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
        populateEffectKeyframeTable(m_widgets, nullptr);
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
        if (m_widgets.tilingUseMaskBoundsCheck) {
            m_widgets.tilingUseMaskBoundsCheck->setChecked(
                selectedTrack && selectedTrack->tilingUseMaskBounds);
            m_widgets.tilingUseMaskBoundsCheck->setEnabled(trackEffectActive);
        }
        if (m_widgets.tilingMaskIslandSigmaSpin) {
            m_widgets.tilingMaskIslandSigmaSpin->setValue(
                selectedTrack ? selectedTrack->tilingMaskIslandSigma : 0.0);
            m_widgets.tilingMaskIslandSigmaSpin->setEnabled(trackEffectActive);
        }
        if (m_widgets.maskBoundingBoxPreviewCheck) {
            m_widgets.maskBoundingBoxPreviewCheck->setChecked(false);
            m_widgets.maskBoundingBoxPreviewCheck->setEnabled(false);
        }
        setDirectionalEchoValues(
            m_widgets,
            selectedTrack ? selectedTrack->effectSpeed : 0.0,
            selectedTrack ? selectedTrack->tilingSpacing : 1.0,
            selectedTrack ? selectedTrack->effectScale : 2.0);
        setStepRepeatFillValues(
            m_widgets,
            selectedTrack ? selectedTrack->effectSpeed : 6.0,
            selectedTrack ? selectedTrack->tilingSpacing : 4.0,
            selectedTrack ? selectedTrack->effectScale : 4.0);
        for (QDial* dial : {
                 m_widgets.directionalEchoDirectionDial,
                 m_widgets.directionalEchoSpreadDial,
                 m_widgets.directionalEchoHueDial,
                 m_widgets.stepRepeatFillGuideScaleDial,
                 m_widgets.stepRepeatFillLumaMatchDial,
                 m_widgets.stepRepeatFillHueMatchDial}) {
            if (dial) dial->setEnabled(trackEffectActive);
        }
        updatePresetParameterVisibility(
            m_widgets, trackPreset, QStringLiteral("none"), false);
        m_updating = false;
        return;
    }

    const QString nativePath = QDir::toNativeSeparators(m_deps.getClipFilePath(*clip));
    const QString sourceLabel = QStringLiteral("%1 | %2")
                                    .arg(clipMediaTypeLabel(clip->mediaType),
                                         mediaSourceKindLabel(clip->sourceKind));
    m_widgets.effectsPathLabel->setText(QStringLiteral("%1\n%2").arg(clip->label, sourceLabel));
    m_widgets.effectsPathLabel->setToolTip(nativePath);
    const int64_t timelineFrame =
        m_deps.currentTimelineFrame ? m_deps.currentTimelineFrame()
                                    : clip->startFrame;
    const int64_t localFrame = qBound<int64_t>(
        0, timelineFrame - clip->startFrame,
        qMax<int64_t>(0, clip->durationFrames - 1));
    const TimelineClip displayedClip =
        evaluateClipEffectAnimationAtPosition(
            *clip, static_cast<qreal>(clip->startFrame + localFrame));
    const TimelineClip* effectClip = &displayedClip;

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
    updateEdgeFillParameterVisibility(m_widgets, clip->edgeFillEffect);
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
    setEffectPresetSelectorToPreset(m_widgets, effectClip->effectPreset, maskMatteTarget);
    if (m_widgets.effectRowsSpin) {
        m_widgets.effectRowsSpin->setValue(effectClip->effectRows);
    }
    if (m_widgets.effectSpeedSpin) {
        m_widgets.effectSpeedSpin->setValue(effectClip->effectSpeed);
    }
    if (m_widgets.effectScaleSpin) {
        m_widgets.effectScaleSpin->setValue(effectClip->effectScale);
    }
    if (m_widgets.effectAlternateDirectionCheck) {
        m_widgets.effectAlternateDirectionCheck->setChecked(effectClip->effectAlternateDirection);
    }
    if (m_widgets.effectSpeechSyncCheck) {
        m_widgets.effectSpeechSyncCheck->setChecked(effectClip->effectSkipAwareTiming);
        const bool steadyIncrease =
            effectClip->effectModulationMode == QStringLiteral("steady_increase");
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
    populateEffectKeyframeTable(m_widgets, clip);
    for (QPushButton* button : {
             m_widgets.effectKeyframeOnButton,
             m_widgets.effectKeyframeOffButton,
             m_widgets.effectParameterKeyframeButton}) {
        if (button) button->setEnabled(true);
    }
    if (m_widgets.effectKeyframeRemoveButton) {
        m_widgets.effectKeyframeRemoveButton->setEnabled(!keyDescriptions.isEmpty());
        m_widgets.effectKeyframeRemoveButton->setToolTip(
            hasKeyAtPlayhead
                ? QStringLiteral("Remove effect keys at the playhead, or selected rows if the table has a selection.")
                : QStringLiteral("Select rows in the table to remove them, or move the playhead to a keyed frame."));
    }
    auto setComboData = [](QComboBox* combo, const QString& value) {
        if (!combo) return;
        const int index = combo->findData(value);
        combo->setCurrentIndex(qMax(0, index));
    };
    setComboData(m_widgets.effectModulationModeCombo,
                 effectClip->effectModulationMode);
    setComboData(m_widgets.effectModulationTargetCombo,
                 effectClip->effectModulationTarget);
    if (m_widgets.effectModulationAmountSpin) {
        m_widgets.effectModulationAmountSpin->setValue(
            effectClip->effectModulationAmount);
    }
    if (m_widgets.effectModulationRateSpin) {
        m_widgets.effectModulationRateSpin->setValue(
            effectClip->effectModulationRate);
    }
    if (m_widgets.effectModulationPhaseSpin) {
        m_widgets.effectModulationPhaseSpin->setValue(
            effectClip->effectModulationPhaseDegrees);
    }
    const bool dynamicEnabled =
        effectClip->effectModulationMode != QStringLiteral("none");
    const bool lfoEnabled =
        effectClip->effectModulationMode == QStringLiteral("lfo");
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
    if (m_widgets.differenceReferenceFramesSpin) m_widgets.differenceReferenceFramesSpin->setValue(effectClip->differenceReferenceFrames);
    if (m_widgets.differenceThresholdSpin) m_widgets.differenceThresholdSpin->setValue(effectClip->differenceThreshold);
    if (m_widgets.differenceSoftnessSpin) m_widgets.differenceSoftnessSpin->setValue(effectClip->differenceSoftness);
    if (m_widgets.temporalEchoCountSpin) m_widgets.temporalEchoCountSpin->setValue(effectClip->temporalEchoCount);
    if (m_widgets.temporalEchoSpacingSpin) m_widgets.temporalEchoSpacingSpin->setValue(effectClip->temporalEchoSpacingFrames);
    if (m_widgets.temporalEchoDecaySpin) m_widgets.temporalEchoDecaySpin->setValue(effectClip->temporalEchoDecay);
    if (m_widgets.tilingPatternCombo) {
        m_widgets.tilingPatternCombo->setCurrentIndex(
            comboIndexForTilingPattern(
                m_widgets.tilingPatternCombo,
                effectClip->tilingPattern));
    }
    if (m_widgets.tilingSpacingSpin) {
        m_widgets.tilingSpacingSpin->setValue(effectClip->tilingSpacing);
    }
    if (m_widgets.tilingWrapCheck) {
        m_widgets.tilingWrapCheck->setChecked(effectClip->tilingWrap);
    }
    if (m_widgets.tilingUseMaskBoundsCheck) {
        m_widgets.tilingUseMaskBoundsCheck->setChecked(effectClip->tilingUseMaskBounds);
    }
    if (m_widgets.tilingMaskIslandSigmaSpin) {
        m_widgets.tilingMaskIslandSigmaSpin->setValue(effectClip->tilingMaskIslandSigma);
    }
    if (m_widgets.maskBoundingBoxPreviewCheck) {
        m_widgets.maskBoundingBoxPreviewCheck->setChecked(
            clip->maskBoundingBoxPreview);
    }
    setDirectionalEchoValues(m_widgets,
                             effectClip->effectSpeed,
                             effectClip->tilingSpacing,
                             effectClip->effectScale);
    setStepRepeatFillValues(m_widgets,
                            effectClip->effectSpeed,
                            effectClip->tilingSpacing,
                            effectClip->effectScale);

    const bool imagePresetCapable = clip->mediaType == ClipMediaType::Image ||
                                    clip->mediaType == ClipMediaType::Video;
    const ClipEffectPreset clipPreset = effectClip->effectPreset;
    const bool imagePresetActive = clipPreset != ClipEffectPreset::None;
    if (m_widgets.effectPresetCategoryCombo) {
        m_widgets.effectPresetCategoryCombo->setEnabled(imagePresetCapable);
    }
    if (m_widgets.effectPresetCombo) {
        m_widgets.effectPresetCombo->setEnabled(imagePresetCapable);
    }
    if (m_widgets.effectPresetPreviousButton) {
        m_widgets.effectPresetPreviousButton->setEnabled(imagePresetCapable);
    }
    if (m_widgets.effectPresetNextButton) {
        m_widgets.effectPresetNextButton->setEnabled(imagePresetCapable);
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
            effectClip->effectModulationMode == QStringLiteral("steady_increase");
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
    const bool maskClip = clip->clipRole == ClipRole::MaskMatte;
    const bool maskDomainControlsActive =
        maskClip ||
        (tilingControlsActive &&
         effectClip &&
         (effectClip->effectPreset == ClipEffectPreset::SourceTile ||
          generatedMaskDomainPreset(effectClip->effectPreset)));
    if (m_widgets.tilingUseMaskBoundsCheck) {
        m_widgets.tilingUseMaskBoundsCheck->setEnabled(maskDomainControlsActive);
    }
    if (m_widgets.tilingMaskIslandSigmaSpin) {
        m_widgets.tilingMaskIslandSigmaSpin->setEnabled(
            maskDomainControlsActive && effectClip->tilingUseMaskBounds);
    }
    if (m_widgets.maskBoundingBoxPreviewCheck) {
        m_widgets.maskBoundingBoxPreviewCheck->setEnabled(maskClip);
    }
    for (QDial* dial : {
             m_widgets.directionalEchoDirectionDial,
             m_widgets.directionalEchoSpreadDial,
             m_widgets.directionalEchoHueDial,
             m_widgets.stepRepeatFillGuideScaleDial,
             m_widgets.stepRepeatFillLumaMatchDial,
             m_widgets.stepRepeatFillHueMatchDial}) {
        if (dial) dial->setEnabled(imagePresetCapable && imagePresetActive);
    }
    updatePresetParameterVisibility(
        m_widgets,
        clipPreset,
        effectClip->effectModulationMode,
        clip->clipRole == ClipRole::MaskMatte);
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
    const double rawSpeed = m_widgets.effectSpeedSpin ? m_widgets.effectSpeedSpin->value() : 1.0;
    const double rawScale = m_widgets.effectScaleSpin ? m_widgets.effectScaleSpin->value() : 1.0;
    const ClipTilingPattern tilingPattern = tilingPatternFromCombo(m_widgets.tilingPatternCombo);
    const double rawTilingSpacing = m_widgets.tilingSpacingSpin ? m_widgets.tilingSpacingSpin->value() : 1.0;
    double speed = rawSpeed;
    double scale = rawScale;
    double tilingSpacing = rawTilingSpacing;
    if (preset == ClipEffectPreset::DirectionalFrameEcho) {
        speed = m_widgets.directionalEchoDirectionDial
            ? effectSpeedFromDirectionDegrees(m_widgets.directionalEchoDirectionDial->value())
            : rawSpeed;
        scale = m_widgets.directionalEchoHueDial
            ? effectScaleFromHuePercent(m_widgets.directionalEchoHueDial->value())
            : rawScale;
        tilingSpacing = m_widgets.directionalEchoSpreadDial
            ? tilingSpacingFromSpreadPercent(m_widgets.directionalEchoSpreadDial->value())
            : rawTilingSpacing;
    } else if (sourceMosaicPreset(preset)) {
        speed = m_widgets.stepRepeatFillLumaMatchDial
            ? effectValueFromMatchPercent(m_widgets.stepRepeatFillLumaMatchDial->value())
            : rawSpeed;
        scale = m_widgets.stepRepeatFillHueMatchDial
            ? effectValueFromMatchPercent(m_widgets.stepRepeatFillHueMatchDial->value())
            : rawScale;
        tilingSpacing = m_widgets.stepRepeatFillGuideScaleDial
            ? tilingSpacingFromGuideScalePercent(m_widgets.stepRepeatFillGuideScaleDial->value())
            : rawTilingSpacing;
    }
    const bool tilingWrap = !m_widgets.tilingWrapCheck || m_widgets.tilingWrapCheck->isChecked();
    const bool tilingUseMaskBounds =
        m_widgets.tilingUseMaskBoundsCheck &&
        m_widgets.tilingUseMaskBoundsCheck->isChecked();
    const double tilingMaskIslandSigma =
        m_widgets.tilingMaskIslandSigmaSpin ? m_widgets.tilingMaskIslandSigmaSpin->value() : 0.0;
    const bool maskBoundingBoxPreview =
        m_widgets.maskBoundingBoxPreviewCheck &&
        m_widgets.maskBoundingBoxPreviewCheck->isChecked();
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
        const int64_t timelineFrame =
            m_deps.currentTimelineFrame ? m_deps.currentTimelineFrame()
                                        : selectedClip->startFrame;
        const int64_t localFrame = qBound<int64_t>(
            0,
            timelineFrame - selectedClip->startFrame,
            qMax<int64_t>(0, selectedClip->durationFrames - 1));
        const TimelineClip::EffectParameterKeyframe keyframe =
            effectParameterKeyframeFromWidgets(
                m_widgets, *selectedClip, localFrame);
        updated = m_deps.updateClipById(selectedClip->id, [=](TimelineClip& clip) {
            clip.effectParameterSets[presetParameterKey(clip.effectPreset)] =
                effectParameters(clip);
            clip.edgeFillEffect = edgeFillEffect;
            clip.edgeFillPixels = qBound(1, edgeFillPixels, 512);
            clip.edgeFillPower = qBound<qreal>(0.25, edgeFillPower, 8.0);
            clip.edgeFillOpacity = qBound<qreal>(0.0, edgeFillOpacity, 1.0);
            clip.edgeFillBrightness = qBound<qreal>(-1.0, edgeFillBrightness, 1.0);
            clip.edgeFillSaturation = qBound<qreal>(0.0, edgeFillSaturation, 3.0);
            clip.effectEnabled = effectEnabled;
            if (clip.clipRole == ClipRole::MaskMatte) {
                clip.tilingUseMaskBounds = tilingUseMaskBounds;
                clip.tilingMaskIslandSigma =
                    qBound<qreal>(0.0, tilingMaskIslandSigma, 100.0);
                clip.maskBoundingBoxPreview = maskBoundingBoxPreview;
            }
            upsertClipEffectParameterKeyframe(clip, keyframe);
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
            track.tilingUseMaskBounds = tilingUseMaskBounds;
            track.tilingMaskIslandSigma =
                qBound<qreal>(0.0, tilingMaskIslandSigma, 100.0);
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
    upsertEffectParameterKeyframe(true, true);
}

void EffectsTab::upsertEffectParameterKeyframe(bool pushHistory, bool refreshAfter)
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
    const TimelineClip::EffectParameterKeyframe keyframe =
        effectParameterKeyframeFromWidgets(m_widgets, *selectedClip, localFrame);

    const QString clipId = selectedClip->id;
    if (!m_deps.updateClipById(
            clipId,
            [keyframe](TimelineClip& clip) {
                upsertClipEffectParameterKeyframe(clip, keyframe);
            })) {
        return;
    }
    applyTabEditEffects(
        effectsEditCallbacks(m_deps),
        TabEditEffects{.pushHistory = pushHistory});
    emit effectsApplied();
    if (refreshAfter) {
        refresh();
    }
}

void EffectsTab::removeEffectEnabledKeyframe()
{
    const TimelineClip* selectedClip = m_deps.getSelectedClip
        ? m_deps.getSelectedClip() : nullptr;
    if (!selectedClip || !m_deps.updateClipById) return;
    const QSet<QPair<int64_t, QString>> selectedRows =
        selectedEffectKeyframeRows(m_widgets.effectKeyframeTable);
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
            [localFrame, selectedRows](TimelineClip& clip) {
                clip.effectEnabledKeyframes.erase(
                    std::remove_if(
                        clip.effectEnabledKeyframes.begin(),
                        clip.effectEnabledKeyframes.end(),
                        [localFrame, selectedRows](const auto& keyframe) {
                            if (selectedRows.isEmpty()) {
                                return keyframe.frame == localFrame;
                            }
                            return selectedRows.contains({
                                keyframe.frame, QStringLiteral("enabled")});
                        }),
                    clip.effectEnabledKeyframes.end());
                clip.effectParameterKeyframes.erase(
                    std::remove_if(
                        clip.effectParameterKeyframes.begin(),
                        clip.effectParameterKeyframes.end(),
                        [localFrame, selectedRows](const auto& keyframe) {
                            if (selectedRows.isEmpty()) {
                                return keyframe.frame == localFrame;
                            }
                            return selectedRows.contains({
                                keyframe.frame, QStringLiteral("parameters")});
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

void EffectsTab::onEffectKeyframeTableItemClicked(QTableWidgetItem* item)
{
    Q_UNUSED(item);
}

void EffectsTab::onEffectKeyframeTableItemChanged(QTableWidgetItem* item)
{
    if (m_updating || !item || !m_widgets.effectKeyframeTable ||
        !m_deps.updateClipById) {
        return;
    }
    const TimelineClip* selectedClip = m_deps.getSelectedClip
        ? m_deps.getSelectedClip() : nullptr;
    if (!selectedClip) {
        return;
    }

    const int row = item->row();
    const int column = item->column();
    const QTableWidgetItem* frameItem =
        m_widgets.effectKeyframeTable->item(row, 0);
    const QTableWidgetItem* typeItem =
        m_widgets.effectKeyframeTable->item(row, 1);
    if (!frameItem || !typeItem) {
        refresh();
        return;
    }
    const int64_t originalFrame =
        frameItem->data(Qt::UserRole).toLongLong();
    const QString keyType =
        typeItem->data(kEffectKeyTypeRole).toString().isEmpty()
            ? typeItem->data(Qt::UserRole).toString()
            : typeItem->data(kEffectKeyTypeRole).toString();

    bool frameOk = false;
    const int64_t editedFrame = frameItem->text().toLongLong(&frameOk);
    if (!frameOk) {
        refresh();
        return;
    }
    const int64_t boundedFrame = qBound<int64_t>(
        0,
        editedFrame,
        qMax<int64_t>(0, selectedClip->durationFrames - 1));
    const QString clipId = selectedClip->id;
    bool updated = false;
    if (keyType == QStringLiteral("enabled")) {
        const bool enabled =
            m_widgets.effectKeyframeTable->item(row, 3) &&
            !m_widgets.effectKeyframeTable->item(row, 3)
                 ->text()
                 .trimmed()
                 .compare(QStringLiteral("on"), Qt::CaseInsensitive);
        updated = m_deps.updateClipById(clipId, [=](TimelineClip& clip) {
            auto existing = std::find_if(
                clip.effectEnabledKeyframes.begin(),
                clip.effectEnabledKeyframes.end(),
                [originalFrame](const auto& keyframe) {
                    return keyframe.frame == originalFrame;
                });
            if (existing == clip.effectEnabledKeyframes.end()) {
                return;
            }
            existing->frame = boundedFrame;
            existing->enabled = enabled;
            std::sort(
                clip.effectEnabledKeyframes.begin(),
                clip.effectEnabledKeyframes.end(),
                [](const auto& left, const auto& right) {
                    return left.frame < right.frame;
                });
        });
    } else if (keyType == QStringLiteral("parameters")) {
        updated = m_deps.updateClipById(clipId, [=](TimelineClip& clip) {
            auto existing = std::find_if(
                clip.effectParameterKeyframes.begin(),
                clip.effectParameterKeyframes.end(),
                [originalFrame](const auto& keyframe) {
                    return keyframe.frame == originalFrame;
                });
            if (existing == clip.effectParameterKeyframes.end()) {
                return;
            }
            existing->frame = boundedFrame;
            if (column == 2) {
                existing->effectPreset = effectPresetFromLabel(
                    item->text(), existing->effectPreset);
                existing->effectPresetKeyframed = true;
            } else if (column == 4) {
                existing->effectRows =
                    qBound(1, item->text().toInt(), 512);
            } else if (column == 5) {
                existing->effectSpeed =
                    qBound<qreal>(-8.0, item->text().toDouble(), 8.0);
            } else if (column == 6) {
                existing->effectScale =
                    qBound<qreal>(0.1, item->text().toDouble(), 8.0);
            } else if (column == 7) {
                existing->tilingPattern =
                    tilingPatternFromLabel(item->text(), existing->tilingPattern);
            }
            std::sort(
                clip.effectParameterKeyframes.begin(),
                clip.effectParameterKeyframes.end(),
                [](const auto& left, const auto& right) {
                    return left.frame < right.frame;
                });
        });
    }

    if (!updated) {
        refresh();
        return;
    }
    applyTabEditEffects(effectsEditCallbacks(m_deps),
                        TabEditEffects{.pushHistory = true});
    emit effectsApplied();
    refresh();
}

void EffectsTab::onEffectKeyframeTableItemDoubleClicked(QTableWidgetItem* item)
{
    editor::editItemIfEditable(m_widgets.effectKeyframeTable, item);
}

void EffectsTab::onEffectKeyframeTableCustomContextMenu(const QPoint& pos)
{
    if (!m_widgets.effectKeyframeTable) {
        return;
    }
    int row = -1;
    QTableWidgetItem* item =
        editor::ensureContextRowSelected(m_widgets.effectKeyframeTable, pos, &row);
    if (!item || row < 0) {
        return;
    }

    QMenu menu;
    const int deletableRowCount =
        editor::countSelectedFrameRoles(
            m_widgets.effectKeyframeTable,
            [](int64_t frame) { return frame >= 0; });
    QAction* deleteRows = menu.addAction(
        deletableRowCount == 1 ? QStringLiteral("Delete Row")
                               : QStringLiteral("Delete Rows"));
    deleteRows->setEnabled(deletableRowCount > 0);

    QAction* chosen =
        menu.exec(m_widgets.effectKeyframeTable->viewport()->mapToGlobal(pos));
    if (chosen == deleteRows && deleteRows->isEnabled()) {
        removeEffectEnabledKeyframe();
    }
}

bool EffectsTab::eventFilter(QObject* watched, QEvent* event)
{
    if (watched == m_widgets.effectKeyframeTable &&
        editor::isTableDeleteKeyEvent(event)) {
        removeEffectEnabledKeyframe();
        return true;
    }
    return QObject::eventFilter(watched, event);
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

void EffectsTab::onEffectPresetCategoryChanged(int index)
{
    Q_UNUSED(index);
    if (m_updating) return;
    const TimelineClip* clip = m_deps.getSelectedClip
        ? m_deps.getSelectedClip() : nullptr;
    const int selectedTrackIndex =
        clip ? clip->trackIndex
             : (m_deps.getSelectedTrackIndex ? m_deps.getSelectedTrackIndex() : -1);
    const TimelineTrack* selectedTrack =
        m_deps.getTrackByIndex ? m_deps.getTrackByIndex(selectedTrackIndex) : nullptr;
    const bool maskMatteTarget =
        (clip && clip->clipRole == ClipRole::MaskMatte) ||
        (!clip && selectedTrack && selectedTrack->generatedChildTrack);
    const ClipEffectPreset previousPreset = presetFromCombo(m_widgets.effectPresetCombo);
    populateEffectPresetCombo(
        m_widgets.effectPresetCombo,
        selectedEffectPresetCategory(m_widgets.effectPresetCategoryCombo),
        previousPreset);
    setMaskMattePresetAvailability(m_widgets.effectPresetCombo, maskMatteTarget);
    applyEffectPreset(true);
    refresh();
}

void EffectsTab::onEffectPresetChanged(int index)
{
    Q_UNUSED(index);
    if (m_updating) return;
    applyEffectPreset(true);
    refresh();
}

void EffectsTab::stepEffectPreset(int delta)
{
    if (m_updating || delta == 0) return;
    const QVector<EffectPresetUiOption> options = effectPresetUiOptions();
    if (options.isEmpty()) return;
    const ClipEffectPreset currentPreset = presetFromCombo(m_widgets.effectPresetCombo);
    int currentIndex = 0;
    for (int i = 0; i < options.size(); ++i) {
        if (options.at(i).preset == currentPreset) {
            currentIndex = i;
            break;
        }
    }
    const int nextIndex =
        (currentIndex + delta + options.size()) % options.size();
    const TimelineClip* clip = m_deps.getSelectedClip
        ? m_deps.getSelectedClip() : nullptr;
    const int selectedTrackIndex =
        clip ? clip->trackIndex
             : (m_deps.getSelectedTrackIndex ? m_deps.getSelectedTrackIndex() : -1);
    const TimelineTrack* selectedTrack =
        m_deps.getTrackByIndex ? m_deps.getTrackByIndex(selectedTrackIndex) : nullptr;
    const bool maskMatteTarget =
        (clip && clip->clipRole == ClipRole::MaskMatte) ||
        (!clip && selectedTrack && selectedTrack->generatedChildTrack);
    setEffectPresetSelectorToPreset(
        m_widgets, options.at(nextIndex).preset, maskMatteTarget);
    applyEffectPreset(true);
    refresh();
}

void EffectsTab::onEffectControlChanged()
{
    if (m_updating) return;
    updateDirectionalEchoLabels(m_widgets);
    updateStepRepeatFillLabels(m_widgets);
    applyEffectPreset(false);
    if (sender() == m_widgets.edgeFillEffectCombo && m_widgets.edgeFillEffectCombo) {
        updateEdgeFillParameterVisibility(
            m_widgets,
            backgroundFillEffectFromString(
                m_widgets.edgeFillEffectCombo->currentData().toString()));
    }
}
