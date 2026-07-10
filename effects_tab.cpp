#include "effects_tab.h"
#include "editor_effect_presets.h"
#include "editor_tab_edit_effects.h"

#include <QSignalBlocker>
#include <QDir>
#include <QtGlobal>
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
}

void EffectsTab::wire()
{
    if (m_widgets.maskFeatherSpin) {
        connect(m_widgets.maskFeatherSpin, qOverload<double>(&QDoubleSpinBox::valueChanged),
                this, &EffectsTab::onMaskFeatherChanged);
        connect(m_widgets.maskFeatherSpin, &QDoubleSpinBox::editingFinished,
                this, &EffectsTab::onEditingFinished);
    }
    if (m_widgets.maskFeatherGammaSpin) {
        connect(m_widgets.maskFeatherGammaSpin, qOverload<double>(&QDoubleSpinBox::valueChanged),
                this, &EffectsTab::onMaskFeatherGammaChanged);
        connect(m_widgets.maskFeatherGammaSpin, &QDoubleSpinBox::editingFinished,
                this, &EffectsTab::onEditingFinished);
    }
    if (m_widgets.maskFeatherFalloffCombo) {
        connect(m_widgets.maskFeatherFalloffCombo,
                qOverload<int>(&QComboBox::currentIndexChanged),
                this, &EffectsTab::onMaskFeatherFalloffChanged);
    }
    if (m_widgets.maskFeatherEnabledCheck) {
        connect(m_widgets.maskFeatherEnabledCheck, &QCheckBox::toggled,
                this, &EffectsTab::onMaskFeatherEnabledChanged);
    }
    if (m_widgets.applyButton) {
        connect(m_widgets.applyButton, &QPushButton::clicked,
                this, &EffectsTab::onApplyClicked);
    }
    if (m_widgets.maskForegroundLayerCheck) {
        connect(m_widgets.maskForegroundLayerCheck, &QCheckBox::toggled,
                this, &EffectsTab::onEffectControlChanged);
    }
    if (m_widgets.maskRepeatEnabledCheck) {
        connect(m_widgets.maskRepeatEnabledCheck, &QCheckBox::toggled,
                this, &EffectsTab::onEffectControlChanged);
    }
    if (m_widgets.maskRepeatDeltaXSpin) {
        connect(m_widgets.maskRepeatDeltaXSpin, qOverload<double>(&QDoubleSpinBox::valueChanged),
                this, &EffectsTab::onEffectControlChanged);
        connect(m_widgets.maskRepeatDeltaXSpin, &QDoubleSpinBox::editingFinished,
                this, &EffectsTab::onEditingFinished);
    }
    if (m_widgets.maskRepeatDeltaYSpin) {
        connect(m_widgets.maskRepeatDeltaYSpin, qOverload<double>(&QDoubleSpinBox::valueChanged),
                this, &EffectsTab::onEffectControlChanged);
        connect(m_widgets.maskRepeatDeltaYSpin, &QDoubleSpinBox::editingFinished,
                this, &EffectsTab::onEditingFinished);
    }
    if (m_widgets.effectPresetCombo) {
        connect(m_widgets.effectPresetCombo, qOverload<int>(&QComboBox::currentIndexChanged),
                this, &EffectsTab::onEffectPresetChanged);
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
    if (!m_widgets.effectsPathLabel || !m_widgets.maskFeatherSpin) {
        return;
    }

    const TimelineClip* clip = m_deps.getSelectedClip();
    const int selectedTrackIndex =
        clip ? clip->trackIndex : (m_deps.getSelectedTrackIndex ? m_deps.getSelectedTrackIndex() : -1);
    const TimelineTrack* selectedTrack =
        m_deps.getTrackByIndex ? m_deps.getTrackByIndex(selectedTrackIndex) : nullptr;
    m_updating = true;

    QSignalBlocker featherBlock(m_widgets.maskFeatherSpin);
    QSignalBlocker enabledBlock(m_widgets.maskFeatherEnabledCheck);
    const std::unique_ptr<QSignalBlocker> falloffBlock =
        m_widgets.maskFeatherFalloffCombo
            ? std::make_unique<QSignalBlocker>(m_widgets.maskFeatherFalloffCombo)
            : nullptr;
    const std::unique_ptr<QSignalBlocker> foregroundBlock =
        m_widgets.maskForegroundLayerCheck
            ? std::make_unique<QSignalBlocker>(m_widgets.maskForegroundLayerCheck)
            : nullptr;
    const std::unique_ptr<QSignalBlocker> repeatEnabledBlock =
        m_widgets.maskRepeatEnabledCheck
            ? std::make_unique<QSignalBlocker>(m_widgets.maskRepeatEnabledCheck)
            : nullptr;
    const std::unique_ptr<QSignalBlocker> repeatXBlock =
        m_widgets.maskRepeatDeltaXSpin
            ? std::make_unique<QSignalBlocker>(m_widgets.maskRepeatDeltaXSpin)
            : nullptr;
    const std::unique_ptr<QSignalBlocker> repeatYBlock =
        m_widgets.maskRepeatDeltaYSpin
            ? std::make_unique<QSignalBlocker>(m_widgets.maskRepeatDeltaYSpin)
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

    const bool selectedVirtualClip =
        clip && (clip->clipRole == ClipRole::MaskMatte ||
                 clip->clipRole == ClipRole::EffectSynth);

    if (!clip || selectedVirtualClip || !m_deps.clipHasVisuals(*clip)) {
        m_widgets.effectsPathLabel->setText(
            selectedVirtualClip
                ? QStringLiteral("Virtual clip\nEffects are edited on the source clip")
                : selectedTrack
                ? QStringLiteral("Track effects\n%1").arg(selectedTrack->name)
                : QStringLiteral("No visual clip selected"));
        m_widgets.effectsPathLabel->setToolTip(QString());
        m_widgets.maskFeatherSpin->setValue(0.0);
        if (m_widgets.maskFeatherEnabledCheck) {
            m_widgets.maskFeatherEnabledCheck->setChecked(false);
        }
        if (m_widgets.maskForegroundLayerCheck) {
            m_widgets.maskForegroundLayerCheck->setChecked(false);
            m_widgets.maskForegroundLayerCheck->setEnabled(false);
        }
        if (m_widgets.maskRepeatEnabledCheck) {
            m_widgets.maskRepeatEnabledCheck->setChecked(false);
            m_widgets.maskRepeatEnabledCheck->setEnabled(false);
        }
        if (m_widgets.maskRepeatDeltaXSpin) {
            m_widgets.maskRepeatDeltaXSpin->setValue(160.0);
            m_widgets.maskRepeatDeltaXSpin->setEnabled(false);
        }
        if (m_widgets.maskRepeatDeltaYSpin) {
            m_widgets.maskRepeatDeltaYSpin->setValue(0.0);
            m_widgets.maskRepeatDeltaYSpin->setEnabled(false);
        }
        if (m_widgets.effectPresetCombo) {
            m_widgets.effectPresetCombo->setCurrentIndex(comboIndexForPreset(
                m_widgets.effectPresetCombo,
                selectedTrack ? selectedTrack->effectPreset : ClipEffectPreset::None));
            m_widgets.effectPresetCombo->setEnabled(selectedTrack != nullptr);
        }
        const ClipEffectPreset trackPreset =
            selectedTrack ? selectedTrack->effectPreset : ClipEffectPreset::None;
        const bool trackEffectActive = selectedTrack && trackPreset != ClipEffectPreset::None;
        if (m_widgets.effectRowsSpin) {
            m_widgets.effectRowsSpin->setValue(selectedTrack ? selectedTrack->effectRows : 32);
            m_widgets.effectRowsSpin->setEnabled(trackEffectActive);
        }
        if (m_widgets.effectSpeedSpin) {
            m_widgets.effectSpeedSpin->setValue(selectedTrack ? selectedTrack->effectSpeed : 1.0);
            m_widgets.effectSpeedSpin->setEnabled(trackEffectActive);
        }
        if (m_widgets.effectScaleSpin) {
            m_widgets.effectScaleSpin->setValue(selectedTrack ? selectedTrack->effectScale : 1.0);
            m_widgets.effectScaleSpin->setEnabled(trackEffectActive);
        }
        if (m_widgets.effectAlternateDirectionCheck) {
            m_widgets.effectAlternateDirectionCheck->setChecked(!selectedTrack || selectedTrack->effectAlternateDirection);
            m_widgets.effectAlternateDirectionCheck->setEnabled(
                selectedTrack && effectPresetUsesDirectionalControl(trackPreset));
        }
        if (m_widgets.effectSpeechSyncCheck) {
            m_widgets.effectSpeechSyncCheck->setChecked(false);
            m_widgets.effectSpeechSyncCheck->setEnabled(false);
        }
        if (m_widgets.tilingPatternCombo) {
            m_widgets.tilingPatternCombo->setCurrentIndex(comboIndexForTilingPattern(
                m_widgets.tilingPatternCombo,
                selectedTrack ? selectedTrack->tilingPattern : ClipTilingPattern::Grid));
            m_widgets.tilingPatternCombo->setEnabled(
                selectedTrack && effectPresetUsesTilingControls(trackPreset));
        }
        if (m_widgets.tilingSpacingSpin) {
            m_widgets.tilingSpacingSpin->setValue(selectedTrack ? selectedTrack->tilingSpacing : 1.0);
            m_widgets.tilingSpacingSpin->setEnabled(
                selectedTrack && effectPresetUsesTilingControls(trackPreset));
        }
        if (m_widgets.tilingWrapCheck) {
            m_widgets.tilingWrapCheck->setChecked(!selectedTrack || selectedTrack->tilingWrap);
            m_widgets.tilingWrapCheck->setEnabled(
                selectedTrack && effectPresetUsesTilingControls(trackPreset));
        }
        m_updating = false;
        return;
    }

    const bool hasAlpha = m_deps.clipHasAlpha(*clip);
    const QString nativePath = QDir::toNativeSeparators(m_deps.getClipFilePath(*clip));
    const QString sourceLabel = QStringLiteral("%1 | %2%3")
                                    .arg(clipMediaTypeLabel(clip->mediaType),
                                         mediaSourceKindLabel(clip->sourceKind),
                                         hasAlpha ? QStringLiteral(" | Alpha") : QStringLiteral(""));
    m_widgets.effectsPathLabel->setText(QStringLiteral("%1\n%2").arg(clip->label, sourceLabel));
    m_widgets.effectsPathLabel->setToolTip(nativePath);

    m_widgets.maskFeatherSpin->setValue(clip->maskFeather);
    if (m_widgets.maskFeatherGammaSpin) {
        m_widgets.maskFeatherGammaSpin->setValue(clip->maskFeatherGamma);
    }
    if (m_widgets.maskFeatherFalloffCombo) {
        const int index = m_widgets.maskFeatherFalloffCombo->findData(clip->maskFeatherFalloff);
        m_widgets.maskFeatherFalloffCombo->setCurrentIndex(index >= 0 ? index : 0);
    }
    if (m_widgets.maskFeatherEnabledCheck) {
        m_widgets.maskFeatherEnabledCheck->setChecked(clip->maskFeather > 0.0);
    }
    if (m_widgets.maskForegroundLayerCheck) {
        m_widgets.maskForegroundLayerCheck->setChecked(clip->maskForegroundLayerEnabled);
    }
    if (m_widgets.maskRepeatEnabledCheck) {
        m_widgets.maskRepeatEnabledCheck->setChecked(clip->maskRepeatEnabled);
    }
    if (m_widgets.maskRepeatDeltaXSpin) {
        m_widgets.maskRepeatDeltaXSpin->setValue(clip->maskRepeatDeltaX);
    }
    if (m_widgets.maskRepeatDeltaYSpin) {
        m_widgets.maskRepeatDeltaYSpin->setValue(clip->maskRepeatDeltaY);
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
    }
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

    // Disable mask feather controls if clip doesn't have alpha
    if (m_widgets.maskFeatherEnabledCheck) {
        m_widgets.maskFeatherEnabledCheck->setEnabled(hasAlpha);
        if (!hasAlpha) {
            m_widgets.maskFeatherEnabledCheck->setToolTip(
                QStringLiteral("Mask feathering requires an alpha channel.\n\n"
                               "To use this feature:\n"
                               "• Use PNG, TGA, TIFF, or EXR formats\n"
                               "• Ensure your source images have transparency\n"
                               "• For video, use ProRes 4444 or similar alpha-capable codecs\n\n"
                               "Current clip format does not support alpha."));
        } else {
            m_widgets.maskFeatherEnabledCheck->setToolTip(
                QStringLiteral("Enable mask feathering for this alpha-capable clip."));
        }
    }
    m_widgets.maskFeatherSpin->setEnabled(hasAlpha && (!m_widgets.maskFeatherEnabledCheck || m_widgets.maskFeatherEnabledCheck->isChecked()));
    if (m_widgets.maskFeatherSpin && !hasAlpha) {
        m_widgets.maskFeatherSpin->setToolTip(
            QStringLiteral("Disabled: Selected clip does not have an alpha channel."));
    } else if (m_widgets.maskFeatherSpin) {
        m_widgets.maskFeatherSpin->setToolTip(
            QStringLiteral("Feather radius in pixels for the mask edge."));
    }
    if (m_widgets.maskFeatherGammaSpin) {
        const bool powerFalloff = !m_widgets.maskFeatherFalloffCombo ||
            m_widgets.maskFeatherFalloffCombo->currentData().toInt() == 0;
        m_widgets.maskFeatherGammaSpin->setEnabled(hasAlpha && powerFalloff &&
            (!m_widgets.maskFeatherEnabledCheck || m_widgets.maskFeatherEnabledCheck->isChecked()));
        if (!hasAlpha) {
            m_widgets.maskFeatherGammaSpin->setToolTip(
                QStringLiteral("Disabled: Selected clip does not have an alpha channel."));
        } else {
            m_widgets.maskFeatherGammaSpin->setToolTip(
                QStringLiteral("Power-law exponent. 1.0 is linear; higher values retain a more opaque edge."));
        }
    }
    if (m_widgets.maskFeatherFalloffCombo) {
        m_widgets.maskFeatherFalloffCombo->setEnabled(
            hasAlpha && (!m_widgets.maskFeatherEnabledCheck ||
                         m_widgets.maskFeatherEnabledCheck->isChecked()));
    }
    const bool hasSamMask = clip->maskEnabled && !clip->maskFramesDir.trimmed().isEmpty();
    if (m_widgets.maskForegroundLayerCheck) {
        m_widgets.maskForegroundLayerCheck->setEnabled(hasSamMask);
        m_widgets.maskForegroundLayerCheck->setToolTip(
            hasSamMask
                ? QStringLiteral("Draw the SAM-selected person again as a Vulkan foreground layer.")
                : QStringLiteral("Requires an enabled SAM mask in the Masks tab."));
    }
    const bool maskRepeatActive = hasSamMask && clip->maskRepeatEnabled;
    if (m_widgets.maskRepeatEnabledCheck) {
        m_widgets.maskRepeatEnabledCheck->setEnabled(hasSamMask);
        m_widgets.maskRepeatEnabledCheck->setToolTip(
            hasSamMask
                ? QStringLiteral("Repeat source pixels through the processed SAM mask channel.")
                : QStringLiteral("Requires an enabled SAM mask in the Masks tab."));
    }
    if (m_widgets.maskRepeatDeltaXSpin) {
        m_widgets.maskRepeatDeltaXSpin->setEnabled(maskRepeatActive);
    }
    if (m_widgets.maskRepeatDeltaYSpin) {
        m_widgets.maskRepeatDeltaYSpin->setEnabled(maskRepeatActive);
    }
    const bool imagePresetCapable = clip->mediaType == ClipMediaType::Image ||
                                    clip->mediaType == ClipMediaType::Video;
    const ClipEffectPreset clipPreset = clip->effectPreset;
    const bool imagePresetActive = clipPreset != ClipEffectPreset::None;
    if (m_widgets.effectPresetCombo) {
        m_widgets.effectPresetCombo->setEnabled(imagePresetCapable);
    }
    if (m_widgets.effectRowsSpin) {
        m_widgets.effectRowsSpin->setEnabled((imagePresetCapable && imagePresetActive) || maskRepeatActive);
    }
    if (m_widgets.effectSpeedSpin) {
        m_widgets.effectSpeedSpin->setEnabled(imagePresetCapable && imagePresetActive);
    }
    if (m_widgets.effectScaleSpin) {
        m_widgets.effectScaleSpin->setEnabled(imagePresetCapable && imagePresetActive);
    }
    if (m_widgets.effectAlternateDirectionCheck) {
        m_widgets.effectAlternateDirectionCheck->setEnabled(
            imagePresetCapable && effectPresetUsesDirectionalControl(clipPreset));
    }
    if (m_widgets.effectSpeechSyncCheck) {
        m_widgets.effectSpeechSyncCheck->setEnabled(imagePresetCapable && imagePresetActive);
    }
    const bool tilingControlsActive =
        imagePresetCapable && effectPresetUsesTilingControls(clipPreset);
    if (m_widgets.tilingPatternCombo) {
        m_widgets.tilingPatternCombo->setEnabled(tilingControlsActive);
    }
    if (m_widgets.tilingSpacingSpin) {
        m_widgets.tilingSpacingSpin->setEnabled(tilingControlsActive);
    }
    if (m_widgets.tilingWrapCheck) {
        m_widgets.tilingWrapCheck->setEnabled(tilingControlsActive);
    }
    m_updating = false;
}

void EffectsTab::applyMaskFeather(bool pushHistory)
{
    if (m_updating) return;

    const TimelineClip* selectedClip = m_deps.getSelectedClip();
    if (!selectedClip ||
        selectedClip->clipRole == ClipRole::MaskMatte ||
        selectedClip->clipRole == ClipRole::EffectSynth ||
        !m_deps.clipHasVisuals(*selectedClip)) return;

    const double featherValue = m_widgets.maskFeatherEnabledCheck && m_widgets.maskFeatherEnabledCheck->isChecked()
                                    ? m_widgets.maskFeatherSpin->value()
                                    : 0.0;
    const double featherGamma = m_widgets.maskFeatherGammaSpin ? m_widgets.maskFeatherGammaSpin->value() : 2.0;
    const int featherFalloff = m_widgets.maskFeatherFalloffCombo
        ? m_widgets.maskFeatherFalloffCombo->currentData().toInt() : 0;

    const bool updated = m_deps.updateClipById(selectedClip->id, [featherValue, featherGamma, featherFalloff](TimelineClip& clip) {
        clip.maskFeather = featherValue;
        clip.maskFeatherGamma = featherGamma;
        clip.maskFeatherFalloff = qBound(0, featherFalloff, 5);
    });

    if (!updated) return;

    applyTabEditEffects(effectsEditCallbacks(m_deps),
                        TabEditEffects{.pushHistory = pushHistory});
    emit effectsApplied();
}

void EffectsTab::applyEffectPreset(bool pushHistory)
{
    if (m_updating) return;

    const TimelineClip* selectedClip = m_deps.getSelectedClip();
    const int targetTrackIndex =
        selectedClip ? selectedClip->trackIndex : (m_deps.getSelectedTrackIndex ? m_deps.getSelectedTrackIndex() : -1);

    const ClipEffectPreset preset = presetFromCombo(m_widgets.effectPresetCombo);
    const int rows = m_widgets.effectRowsSpin ? m_widgets.effectRowsSpin->value() : 32;
    const double speed = m_widgets.effectSpeedSpin ? m_widgets.effectSpeedSpin->value() : 1.0;
    const double scale = m_widgets.effectScaleSpin ? m_widgets.effectScaleSpin->value() : 1.0;
    const ClipTilingPattern tilingPattern = tilingPatternFromCombo(m_widgets.tilingPatternCombo);
    const double tilingSpacing = m_widgets.tilingSpacingSpin ? m_widgets.tilingSpacingSpin->value() : 1.0;
    const bool tilingWrap = !m_widgets.tilingWrapCheck || m_widgets.tilingWrapCheck->isChecked();
    const bool alternate =
        !m_widgets.effectAlternateDirectionCheck || m_widgets.effectAlternateDirectionCheck->isChecked();
    const bool speechSync =
        preset != ClipEffectPreset::None &&
        m_widgets.effectSpeechSyncCheck &&
        m_widgets.effectSpeechSyncCheck->isChecked();
    const bool foreground =
        m_widgets.maskForegroundLayerCheck && m_widgets.maskForegroundLayerCheck->isChecked();
    const bool maskRepeatEnabled =
        m_widgets.maskRepeatEnabledCheck && m_widgets.maskRepeatEnabledCheck->isChecked();
    const double maskRepeatDeltaX =
        m_widgets.maskRepeatDeltaXSpin ? m_widgets.maskRepeatDeltaXSpin->value() : 160.0;
    const double maskRepeatDeltaY =
        m_widgets.maskRepeatDeltaYSpin ? m_widgets.maskRepeatDeltaYSpin->value() : 0.0;

    bool updated = false;
    if (selectedClip &&
        selectedClip->clipRole != ClipRole::MaskMatte &&
        selectedClip->clipRole != ClipRole::EffectSynth &&
        m_deps.clipHasVisuals(*selectedClip)) {
        updated = m_deps.updateClipById(selectedClip->id, [=](TimelineClip& clip) {
            clip.maskForegroundLayerEnabled = foreground;
            clip.maskRepeatEnabled = maskRepeatEnabled;
            clip.maskRepeatDeltaX = qBound<qreal>(-100000.0, maskRepeatDeltaX, 100000.0);
            clip.maskRepeatDeltaY = qBound<qreal>(-100000.0, maskRepeatDeltaY, 100000.0);
            clip.effectPreset = preset;
            clip.effectRows = qBound(1, rows, 96);
            clip.effectSpeed = qBound<qreal>(-8.0, speed, 8.0);
            clip.effectScale = qBound<qreal>(0.1, scale, 8.0);
            clip.effectAlternateDirection = alternate;
            clip.effectSkipAwareTiming = speechSync;
            clip.tilingPattern = tilingPattern;
            clip.tilingSpacing = qBound<qreal>(0.1, tilingSpacing, 8.0);
            clip.tilingWrap = tilingWrap;
        });
    } else if (m_deps.updateTrackByIndex && targetTrackIndex >= 0) {
        updated = m_deps.updateTrackByIndex(targetTrackIndex, [=](TimelineTrack& track) {
            track.effectPreset = preset;
            track.effectRows = qBound(1, rows, 96);
            track.effectSpeed = qBound<qreal>(-8.0, speed, 8.0);
            track.effectScale = qBound<qreal>(0.1, scale, 8.0);
            track.effectAlternateDirection = alternate;
            track.tilingPattern = tilingPattern;
            track.tilingSpacing = qBound<qreal>(0.1, tilingSpacing, 8.0);
            track.tilingWrap = tilingWrap;
        });
    }

    if (!updated) return;

    applyTabEditEffects(effectsEditCallbacks(m_deps),
                        TabEditEffects{.pushHistory = pushHistory});
    emit effectsApplied();
}

void EffectsTab::onMaskFeatherChanged(double value)
{
    Q_UNUSED(value);
    if (m_updating) return;
    applyMaskFeather(false);
}

void EffectsTab::onMaskFeatherGammaChanged(double value)
{
    Q_UNUSED(value);
    if (m_updating) return;
    applyMaskFeather(false);
}

void EffectsTab::onMaskFeatherFalloffChanged(int index)
{
    Q_UNUSED(index);
    if (m_updating) return;
    if (m_widgets.maskFeatherGammaSpin && m_widgets.maskFeatherFalloffCombo) {
        m_widgets.maskFeatherGammaSpin->setEnabled(
            m_widgets.maskFeatherFalloffCombo->currentData().toInt() == 0 &&
            (!m_widgets.maskFeatherEnabledCheck || m_widgets.maskFeatherEnabledCheck->isChecked()));
    }
    applyMaskFeather(true);
}

void EffectsTab::onMaskFeatherEnabledChanged(bool enabled)
{
    if (m_updating) return;
    
    if (m_widgets.maskFeatherSpin) {
        m_widgets.maskFeatherSpin->setEnabled(enabled);
        // Set default value if enabling and current value is 0
        if (enabled && qFuzzyIsNull(m_widgets.maskFeatherSpin->value())) {
            m_widgets.maskFeatherSpin->setValue(5.0);  // Default 5px feather
        }
    }
    if (m_widgets.maskFeatherGammaSpin) {
        m_widgets.maskFeatherGammaSpin->setEnabled(enabled);
        // Set default gamma if enabling and current value is at minimum
        if (enabled && qFuzzyCompare(m_widgets.maskFeatherGammaSpin->value(), m_widgets.maskFeatherGammaSpin->minimum())) {
            m_widgets.maskFeatherGammaSpin->setValue(2.0);  // Default gamma 2.0
        }
    }
    if (m_widgets.maskFeatherFalloffCombo) {
        m_widgets.maskFeatherFalloffCombo->setEnabled(enabled);
    }
    
    applyMaskFeather(true);
}

void EffectsTab::onApplyClicked()
{
    applyMaskFeather(true);
    applyEffectPreset(true);
}

void EffectsTab::onEditingFinished()
{
    if (m_updating) return;
    applyMaskFeather(true);
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
