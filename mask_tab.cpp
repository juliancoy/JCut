#include "mask_tab.h"
#include "editor_effect_presets.h"
#include "editor_tab_edit_effects.h"
#include "mask_sidecar.h"

#include <QComboBox>
#include <QDir>
#include <QFileInfo>
#include <QSignalBlocker>
#include <QSpinBox>

#include <algorithm>

MaskTab::MaskTab(const Widgets& widgets, const Dependencies& deps, QObject* parent)
    : QObject(parent)
    , m_widgets(widgets)
    , m_deps(deps)
{
}

namespace {
constexpr int kSidecarIdRole = Qt::UserRole + 1;
constexpr int kSidecarReadyRole = Qt::UserRole + 2;

TabEditCallbacks maskEditCallbacks(const MaskTab::Dependencies& deps)
{
    return TabEditCallbacks{
        .updatePreview = deps.setPreviewTimelineClips,
        .refreshInspector = deps.refreshInspector,
        .scheduleSave = deps.scheduleSaveState,
        .pushHistory = deps.pushHistorySnapshot,
    };
}

editor::masks::MaskSidecar autoMaskSidecarForClip(const TimelineClip& clip)
{
    const QVector<editor::masks::MaskSidecar> sidecars =
        editor::masks::discoverMaskSidecars(clip);
    const auto ready = std::find_if(
        sidecars.cbegin(), sidecars.cend(), [](const editor::masks::MaskSidecar& sidecar) {
            return sidecar.isReadyForTimeline();
        });
    return ready == sidecars.cend() ? editor::masks::MaskSidecar{} : *ready;
}

QString maskSourceIdForClip(const TimelineClip& clip)
{
    if (clip.clipRole == ClipRole::MaskMatte) {
        return clip.linkedSourceClipId.trimmed();
    }
    return clip.clipRole == ClipRole::Media ? clip.id.trimmed() : QString();
}

QString maskSidecarIdForClip(const TimelineClip& clip)
{
    const QString persistedId = clip.generatedFromMaskId.trimmed();
    return !persistedId.isEmpty()
        ? persistedId
        : editor::masks::stableMaskSidecarId(clip.maskFramesDir);
}

}

void MaskTab::wire()
{
    auto connectApply = [this](auto* widget) {
        if (widget) {
            connect(widget, qOverload<double>(&QDoubleSpinBox::valueChanged), this, [this](double) {
                apply(false);
            });
            connect(widget, &QDoubleSpinBox::editingFinished, this, [this]() {
                apply(true);
            });
        }
    };

    if (m_widgets.enabledCheck) {
        connect(m_widgets.enabledCheck, &QCheckBox::toggled, this, [this](bool) { apply(true); });
    }
    if (m_widgets.framesDirEdit) {
        connect(m_widgets.framesDirEdit, &QLineEdit::editingFinished, this, [this]() { apply(true); });
    }
    if (m_widgets.sidecarCombo) {
        connect(m_widgets.sidecarCombo, qOverload<int>(&QComboBox::currentIndexChanged),
                this, [this](int index) {
            if (m_updating || index < 0 || !m_widgets.framesDirEdit) return;
            m_widgets.framesDirEdit->setText(
                m_widgets.sidecarCombo->itemData(index).toString());
            if (m_widgets.enabledCheck) {
                QSignalBlocker blocker(m_widgets.enabledCheck);
                m_widgets.enabledCheck->setChecked(true);
            }
            apply(true);
        });
    }
    if (m_widgets.browseButton) {
        connect(m_widgets.browseButton, &QPushButton::clicked, this, [this]() {
            if (!m_deps.chooseMaskDirectory || !m_widgets.framesDirEdit) {
                return;
            }
            const QString selected = m_deps.chooseMaskDirectory(
                m_widgets.browseButton,
                m_widgets.framesDirEdit->text().trimmed());
            if (selected.isEmpty()) {
                return;
            }
            m_widgets.framesDirEdit->setText(selected);
            if (m_widgets.enabledCheck) {
                QSignalBlocker blocker(m_widgets.enabledCheck);
                m_widgets.enabledCheck->setChecked(true);
            }
            apply(true);
        });
    }
    if (m_widgets.newPromptButton) {
        connect(m_widgets.newPromptButton, &QPushButton::clicked, this, [this]() {
            const TimelineClip* clip = m_deps.getSelectedClip ? m_deps.getSelectedClip() : nullptr;
            if (clip && m_deps.generatePromptMask) {
                const QString sourceId = clip->clipRole == ClipRole::MaskMatte
                    ? clip->linkedSourceClipId.trimmed()
                    : clip->id;
                if (!sourceId.isEmpty()) {
                    m_deps.generatePromptMask(sourceId);
                }
            }
        });
    }
    if (m_widgets.zLevelSpin) {
        connect(m_widgets.zLevelSpin, qOverload<int>(&QSpinBox::valueChanged),
                this, [this](int) { apply(false, true); });
        connect(m_widgets.zLevelSpin, &QSpinBox::editingFinished,
                this, [this]() { apply(true, true); });
    }
    for (QCheckBox* check : {m_widgets.invertCheck,
                             m_widgets.showOnlyCheck,
                             m_widgets.foregroundLayerCheck,
                             m_widgets.repeatEnabledCheck,
                             m_widgets.shadowEnabledCheck}) {
        if (check) {
            connect(check, &QCheckBox::toggled, this, [this](bool) { apply(true); });
        }
    }
    connectApply(m_widgets.featherSpin);
    connectApply(m_widgets.featherPowerSpin);
    if (m_widgets.featherFalloffCombo) {
        connect(m_widgets.featherFalloffCombo,
                qOverload<int>(&QComboBox::currentIndexChanged), this, [this](int) {
            if (m_updating) return;
            if (m_widgets.featherPowerSpin) {
                m_widgets.featherPowerSpin->setEnabled(
                    m_widgets.featherFalloffCombo->currentData().toInt() == 0);
            }
            apply(true);
        });
    }
    connectApply(m_widgets.dilateSpin);
    connectApply(m_widgets.erodeSpin);
    connectApply(m_widgets.blurSpin);
    connectApply(m_widgets.opacitySpin);
    connectApply(m_widgets.repeatDeltaXSpin);
    connectApply(m_widgets.repeatDeltaYSpin);
    connectApply(m_widgets.shadowRadiusSpin);
    connectApply(m_widgets.shadowOffsetXSpin);
    connectApply(m_widgets.shadowOffsetYSpin);
    connectApply(m_widgets.shadowOpacitySpin);
}

void MaskTab::refresh()
{
    const TimelineClip* clip = m_deps.getSelectedClip ? m_deps.getSelectedClip() : nullptr;
    const bool validClip = clip &&
                           (clip->clipRole == ClipRole::Media ||
                            clip->clipRole == ClipRole::MaskMatte) &&
                           (!m_deps.clipHasVisuals || m_deps.clipHasVisuals(*clip)) &&
                           clip->mediaType == ClipMediaType::Video;
    const bool maskInspectorActive =
        m_deps.isMaskInspectorActive && m_deps.isMaskInspectorActive();

    // The Masks tab edits child-owned state. A source selection is only a
    // discovery context: resolve its chosen sidecar to a materialized child
    // before populating or enabling any treatment controls.
    if (validClip &&
        clip->clipRole == ClipRole::Media &&
        maskInspectorActive) {
        const QString sourceId = clip->id.trimmed();
        QString directory = clip->maskFramesDir.trimmed();
        QString sidecarId = directory.isEmpty()
            ? QString()
            : editor::masks::stableMaskSidecarId(directory);
        if (directory.isEmpty()) {
            const editor::masks::MaskSidecar discoveredSidecar = autoMaskSidecarForClip(*clip);
            if (discoveredSidecar.isValid()) {
                directory = discoveredSidecar.directory;
                sidecarId = discoveredSidecar.id;
            }
        }

        QString childId = m_deps.findMaskMatteChildForSidecar && !sidecarId.isEmpty()
            ? m_deps.findMaskMatteChildForSidecar(sourceId, sidecarId)
            : QString();
        if (childId.isEmpty() &&
            !directory.isEmpty() &&
            m_deps.materializeMaskMatteForSidecar) {
            childId = m_deps.materializeMaskMatteForSidecar(sourceId, directory);
        }
        if (!childId.isEmpty() && childId != sourceId && m_deps.selectClipById) {
            m_deps.selectClipById(childId);
            return;
        }
    }

    m_updating = true;

    if (m_widgets.clipLabel) {
        m_widgets.clipLabel->setText(validClip
            ? QStringLiteral("%1\n%2").arg(clip->label, QDir::toNativeSeparators(clip->filePath))
            : QStringLiteral("Select a video clip to edit its mask."));
        m_widgets.clipLabel->setToolTip(validClip ? QDir::toNativeSeparators(clip->filePath) : QString());
    }

    if (m_widgets.enabledCheck) QSignalBlocker blocker(m_widgets.enabledCheck);
    setControlsEnabled(validClip);
    setTreatmentControlsEnabled(validClip && clip->clipRole == ClipRole::MaskMatte);

    auto setSpin = [](QDoubleSpinBox* spin, double value) {
        if (!spin) return;
        spin->setValue(value);
    };
    auto setCheck = [](QCheckBox* check, bool value) {
        if (!check) return;
        QSignalBlocker blocker(check);
        check->setChecked(value);
    };

    const bool effectiveMaskEnabled = validClip ? clip->maskEnabled : false;
    const QString effectiveMaskFramesDir = validClip ? clip->maskFramesDir : QString();

    if (validClip) {
        const bool treatmentActive = clip->clipRole == ClipRole::MaskMatte;
        if (m_widgets.zLevelSpin) {
            QSignalBlocker blocker(m_widgets.zLevelSpin);
            m_widgets.zLevelSpin->setValue(effectiveClipZLevel(*clip));
        }
        setCheck(m_widgets.enabledCheck, effectiveMaskEnabled);
        if (m_widgets.framesDirEdit) {
            QSignalBlocker blocker(m_widgets.framesDirEdit);
            m_widgets.framesDirEdit->setText(effectiveMaskFramesDir);
        }
        if (m_widgets.sidecarCombo && maskInspectorActive) {
            QSignalBlocker blocker(m_widgets.sidecarCombo);
            m_widgets.sidecarCombo->clear();
            const QVector<editor::masks::MaskSidecar> sidecars =
                editor::masks::discoverMaskSidecars(*clip);
            for (const editor::masks::MaskSidecar& sidecar : sidecars) {
                const QString kind = sidecar.sourceType.contains(
                                         QStringLiteral("continuous_alpha"),
                                         Qt::CaseInsensitive)
                    ? QStringLiteral("soft alpha")
                    : QStringLiteral("binary");
                const QString coverage = sidecar.firstFrame >= 0
                    ? QStringLiteral("%1 · %2 frames · %3–%4")
                          .arg(kind).arg(sidecar.frameCount).arg(sidecar.firstFrame).arg(sidecar.lastFrame)
                    : QStringLiteral("%1 · %2 frames").arg(kind).arg(sidecar.frameCount);
                const QString readiness = sidecar.isReadyForTimeline()
                    ? QString()
                    : QStringLiteral(" — %1").arg(sidecar.readinessIssue);
                m_widgets.sidecarCombo->addItem(
                    QStringLiteral("%1 (%2)%3").arg(
                        sidecar.displayName, coverage, readiness),
                    sidecar.directory);
                m_widgets.sidecarCombo->setItemData(
                    m_widgets.sidecarCombo->count() - 1, sidecar.id, kSidecarIdRole);
                m_widgets.sidecarCombo->setItemData(
                    m_widgets.sidecarCombo->count() - 1,
                    sidecar.isReadyForTimeline(),
                    kSidecarReadyRole);
                if (!sidecar.isReadyForTimeline()) {
                    m_widgets.sidecarCombo->setItemData(
                        m_widgets.sidecarCombo->count() - 1,
                        sidecar.readinessIssue,
                        Qt::ToolTipRole);
                }
            }
            int selected = m_widgets.sidecarCombo->findData(effectiveMaskFramesDir);
            if (selected < 0 && !effectiveMaskFramesDir.trimmed().isEmpty()) {
                const QString issue = clip->maskSidecarAvailabilityIssue.trimmed();
                m_widgets.sidecarCombo->addItem(
                    issue.isEmpty()
                        ? QStringLiteral("Current sidecar (unavailable)")
                        : QStringLiteral("Current sidecar — %1").arg(issue),
                    effectiveMaskFramesDir);
                m_widgets.sidecarCombo->setItemData(
                    m_widgets.sidecarCombo->count() - 1,
                    maskSidecarIdForClip(*clip),
                    kSidecarIdRole);
                m_widgets.sidecarCombo->setItemData(
                    m_widgets.sidecarCombo->count() - 1,
                    false,
                    kSidecarReadyRole);
                m_widgets.sidecarCombo->setItemData(
                    m_widgets.sidecarCombo->count() - 1,
                    issue.isEmpty() ? QStringLiteral("Sidecar unavailable") : issue,
                    Qt::ToolTipRole);
                selected = m_widgets.sidecarCombo->count() - 1;
            } else if (sidecars.isEmpty()) {
                m_widgets.sidecarCombo->addItem(QStringLiteral("No sidecar masks found"), QString());
            }
            m_widgets.sidecarCombo->setCurrentIndex(selected >= 0 ? selected : 0);
        }
        setSpin(m_widgets.featherSpin, clip->maskFeather);
        setSpin(m_widgets.featherPowerSpin, clip->maskFeatherGamma);
        if (m_widgets.featherFalloffCombo) {
            QSignalBlocker blocker(m_widgets.featherFalloffCombo);
            const int index = m_widgets.featherFalloffCombo->findData(clip->maskFeatherFalloff);
            m_widgets.featherFalloffCombo->setCurrentIndex(index >= 0 ? index : 0);
        }
        if (m_widgets.featherPowerSpin) {
            m_widgets.featherPowerSpin->setEnabled(
                treatmentActive && clip->maskFeatherFalloff == 0);
        }
        setSpin(m_widgets.dilateSpin, clip->maskDilate);
        setSpin(m_widgets.erodeSpin, clip->maskErode);
        setSpin(m_widgets.blurSpin, clip->maskBlur);
        setCheck(m_widgets.invertCheck, clip->maskInvert);
        const bool showOnlyAvailable =
            treatmentActive && !clip->maskForegroundLayerEnabled;
        setCheck(m_widgets.showOnlyCheck, showOnlyAvailable && clip->maskShowOnly);
        if (m_widgets.showOnlyCheck) {
            m_widgets.showOnlyCheck->setEnabled(showOnlyAvailable);
        }
        setSpin(m_widgets.opacitySpin, clip->maskOpacity);
        setCheck(m_widgets.foregroundLayerCheck, clip->maskForegroundLayerEnabled);
        setCheck(m_widgets.repeatEnabledCheck, clip->maskRepeatEnabled);
        setSpin(m_widgets.repeatDeltaXSpin, clip->maskRepeatDeltaX);
        setSpin(m_widgets.repeatDeltaYSpin, clip->maskRepeatDeltaY);
        setCheck(m_widgets.shadowEnabledCheck, clip->maskDropShadowEnabled);
        setSpin(m_widgets.shadowRadiusSpin, clip->maskDropShadowRadius);
        setSpin(m_widgets.shadowOffsetXSpin, clip->maskDropShadowOffsetX);
        setSpin(m_widgets.shadowOffsetYSpin, clip->maskDropShadowOffsetY);
        setSpin(m_widgets.shadowOpacitySpin, clip->maskDropShadowOpacity);
    } else {
        setCheck(m_widgets.enabledCheck, false);
        if (m_widgets.framesDirEdit) {
            QSignalBlocker blocker(m_widgets.framesDirEdit);
            m_widgets.framesDirEdit->clear();
        }
    }

    m_updating = false;
}

void MaskTab::apply(bool pushHistory, bool zLevelEdited)
{
    if (m_updating || !m_deps.updateClipById) {
        return;
    }
    const TimelineClip* selectedClip = m_deps.getSelectedClip ? m_deps.getSelectedClip() : nullptr;
    if (!selectedClip ||
        (selectedClip->clipRole != ClipRole::Media &&
         selectedClip->clipRole != ClipRole::MaskMatte) ||
        selectedClip->mediaType != ClipMediaType::Video) {
        return;
    }

    QString requestedDirectory =
        m_widgets.framesDirEdit ? m_widgets.framesDirEdit->text().trimmed() : QString();
    const QString sourceId = maskSourceIdForClip(*selectedClip);
    const editor::masks::MaskSidecar requestedSidecar =
        editor::masks::inspectMaskSidecar(
            requestedDirectory,
            QFileInfo(selectedClip->filePath).completeBaseName(),
            selectedClip->filePath);
    const bool requestedSidecarReady =
        requestedDirectory.isEmpty() || requestedSidecar.isReadyForTimeline();

    if (selectedClip->clipRole == ClipRole::Media) {
        // A source parent is only a discovery/materialization context. Never
        // copy child-owned sidecar or mask-treatment fields onto it.
        QString childId;
        const QString requestedSidecarId = requestedDirectory.isEmpty()
            ? QString()
            : editor::masks::stableMaskSidecarId(requestedDirectory);
        if (!requestedSidecarId.isEmpty() && m_deps.findMaskMatteChildForSidecar) {
            childId = m_deps.findMaskMatteChildForSidecar(sourceId, requestedSidecarId);
        }
        if (requestedSidecarReady && childId.isEmpty() &&
            !requestedDirectory.isEmpty() &&
            m_deps.materializeMaskMatteForSidecar) {
            childId = m_deps.materializeMaskMatteForSidecar(sourceId, requestedDirectory);
        }
        if (!childId.isEmpty() && m_deps.selectClipById) {
            m_deps.selectClipById(childId);
        }
        return;
    }

    const bool clearedChildAssociation =
        requestedDirectory.isEmpty();
    if (clearedChildAssociation) {
        // A materialized child has a durable sidecar identity. Clearing the
        // text field disables it through maskEnabled; it must not turn a UI
        // edit into an implicit association deletion that disk discovery will
        // immediately recreate.
        requestedDirectory = selectedClip->maskFramesDir.trimmed();
    }
    const QString requestedSidecarId = requestedDirectory.isEmpty()
        ? QString()
        : editor::masks::stableMaskSidecarId(requestedDirectory);
    const QString currentSidecarId = maskSidecarIdForClip(*selectedClip);
    if (!requestedSidecarId.isEmpty() &&
        !sourceId.isEmpty() &&
        requestedSidecarId != currentSidecarId) {
        QString childId = m_deps.findMaskMatteChildForSidecar
            ? m_deps.findMaskMatteChildForSidecar(sourceId, requestedSidecarId)
            : QString();
        if (childId.isEmpty() && m_deps.materializeMaskMatteForSidecar) {
            childId = m_deps.materializeMaskMatteForSidecar(sourceId, requestedDirectory);
        }
        if (!childId.isEmpty() && m_deps.selectClipById) {
            // Switching associations means switching children. It must not
            // retarget the selected child's effects, grading, or mask treatment.
            m_deps.selectClipById(childId);
        }
        return;
    }
    if (!requestedSidecarId.isEmpty() &&
        !sourceId.isEmpty() &&
        m_deps.findMaskMatteChildForSidecar) {
        const QString existingChildId =
            m_deps.findMaskMatteChildForSidecar(sourceId, requestedSidecarId);
        if (!existingChildId.isEmpty() && existingChildId != selectedClip->id) {
            // Every discovered sidecar has its own materialized child. Switching
            // the inspector therefore switches selection instead of retargeting
            // another child's visual state to the same association.
            if (m_deps.selectClipById) {
                m_deps.selectClipById(existingChildId);
            }
            return;
        }
    }

    const QString id = selectedClip->id;
    const bool updated = m_deps.updateClipById(
        id,
        [this, requestedDirectory, clearedChildAssociation, zLevelEdited](TimelineClip& clip) {
        if (zLevelEdited && m_widgets.zLevelSpin) {
            clip.zLevel = m_widgets.zLevelSpin->value();
            clip.zLevelUserSet = true;
        }
        clip.maskEnabled = !clearedChildAssociation &&
                           m_widgets.enabledCheck &&
                           m_widgets.enabledCheck->isChecked();
        setMaskSidecarAssociation(clip, requestedDirectory);
        clip.maskFeather = m_widgets.featherSpin ? m_widgets.featherSpin->value() : 0.0;
        clip.maskFeatherGamma = m_widgets.featherPowerSpin ? m_widgets.featherPowerSpin->value() : 2.0;
        clip.maskFeatherFalloff = m_widgets.featherFalloffCombo
            ? qBound(0, m_widgets.featherFalloffCombo->currentData().toInt(), 5) : 0;
        clip.maskDilate = m_widgets.dilateSpin ? m_widgets.dilateSpin->value() : 0.0;
        clip.maskErode = m_widgets.erodeSpin ? m_widgets.erodeSpin->value() : 0.0;
        clip.maskBlur = m_widgets.blurSpin ? m_widgets.blurSpin->value() : 0.0;
        clip.maskInvert = m_widgets.invertCheck && m_widgets.invertCheck->isChecked();
        clip.maskShowOnly =
            !clip.maskForegroundLayerEnabled &&
            m_widgets.showOnlyCheck &&
            m_widgets.showOnlyCheck->isChecked();
        clip.maskOpacity = m_widgets.opacitySpin ? m_widgets.opacitySpin->value() : 1.0;
        clip.maskForegroundLayerEnabled =
            m_widgets.foregroundLayerCheck &&
            m_widgets.foregroundLayerCheck->isChecked();
        clip.maskRepeatEnabled =
            m_widgets.repeatEnabledCheck &&
            m_widgets.repeatEnabledCheck->isChecked();
        clip.maskRepeatDeltaX =
            m_widgets.repeatDeltaXSpin ? m_widgets.repeatDeltaXSpin->value() : 160.0;
        clip.maskRepeatDeltaY =
            m_widgets.repeatDeltaYSpin ? m_widgets.repeatDeltaYSpin->value() : 0.0;
        if (clip.maskForegroundLayerEnabled) {
            clip.maskShowOnly = false;
        }
        clip.maskDropShadowEnabled = m_widgets.shadowEnabledCheck && m_widgets.shadowEnabledCheck->isChecked();
        clip.maskDropShadowRadius = m_widgets.shadowRadiusSpin ? m_widgets.shadowRadiusSpin->value() : 12.0;
        clip.maskDropShadowOffsetX = m_widgets.shadowOffsetXSpin ? m_widgets.shadowOffsetXSpin->value() : 0.0;
        clip.maskDropShadowOffsetY = m_widgets.shadowOffsetYSpin ? m_widgets.shadowOffsetYSpin->value() : 4.0;
        clip.maskDropShadowOpacity = m_widgets.shadowOpacitySpin ? m_widgets.shadowOpacitySpin->value() : 0.45;
        });
    if (!updated) {
        return;
    }
    applyTabEditEffects(maskEditCallbacks(m_deps), TabEditEffects{.pushHistory = pushHistory});
}

void MaskTab::setControlsEnabled(bool enabled)
{
    for (QWidget* widget : {static_cast<QWidget*>(m_widgets.enabledCheck),
                            static_cast<QWidget*>(m_widgets.framesDirEdit),
                            static_cast<QWidget*>(m_widgets.sidecarCombo),
                            static_cast<QWidget*>(m_widgets.browseButton),
                            static_cast<QWidget*>(m_widgets.newPromptButton),
                            static_cast<QWidget*>(m_widgets.zLevelSpin),
                            static_cast<QWidget*>(m_widgets.featherSpin),
                            static_cast<QWidget*>(m_widgets.featherFalloffCombo),
                            static_cast<QWidget*>(m_widgets.featherPowerSpin),
                            static_cast<QWidget*>(m_widgets.dilateSpin),
                            static_cast<QWidget*>(m_widgets.erodeSpin),
                            static_cast<QWidget*>(m_widgets.blurSpin),
                            static_cast<QWidget*>(m_widgets.invertCheck),
                            static_cast<QWidget*>(m_widgets.showOnlyCheck),
                            static_cast<QWidget*>(m_widgets.opacitySpin),
                            static_cast<QWidget*>(m_widgets.foregroundLayerCheck),
                            static_cast<QWidget*>(m_widgets.repeatEnabledCheck),
                            static_cast<QWidget*>(m_widgets.repeatDeltaXSpin),
                            static_cast<QWidget*>(m_widgets.repeatDeltaYSpin),
                            static_cast<QWidget*>(m_widgets.shadowEnabledCheck),
                            static_cast<QWidget*>(m_widgets.shadowRadiusSpin),
                            static_cast<QWidget*>(m_widgets.shadowOffsetXSpin),
                            static_cast<QWidget*>(m_widgets.shadowOffsetYSpin),
                            static_cast<QWidget*>(m_widgets.shadowOpacitySpin)}) {
        if (widget) {
            widget->setEnabled(enabled);
        }
    }
    if (enabled && m_widgets.showOnlyCheck) {
        m_widgets.showOnlyCheck->setEnabled(true);
    }
}

void MaskTab::setTreatmentControlsEnabled(bool enabled)
{
    for (QWidget* widget : {static_cast<QWidget*>(m_widgets.enabledCheck),
                            static_cast<QWidget*>(m_widgets.zLevelSpin),
                            static_cast<QWidget*>(m_widgets.featherSpin),
                            static_cast<QWidget*>(m_widgets.featherFalloffCombo),
                            static_cast<QWidget*>(m_widgets.featherPowerSpin),
                            static_cast<QWidget*>(m_widgets.dilateSpin),
                            static_cast<QWidget*>(m_widgets.erodeSpin),
                            static_cast<QWidget*>(m_widgets.blurSpin),
                            static_cast<QWidget*>(m_widgets.invertCheck),
                            static_cast<QWidget*>(m_widgets.showOnlyCheck),
                            static_cast<QWidget*>(m_widgets.opacitySpin),
                            static_cast<QWidget*>(m_widgets.foregroundLayerCheck),
                            static_cast<QWidget*>(m_widgets.repeatEnabledCheck),
                            static_cast<QWidget*>(m_widgets.repeatDeltaXSpin),
                            static_cast<QWidget*>(m_widgets.repeatDeltaYSpin),
                            static_cast<QWidget*>(m_widgets.shadowEnabledCheck),
                            static_cast<QWidget*>(m_widgets.shadowRadiusSpin),
                            static_cast<QWidget*>(m_widgets.shadowOffsetXSpin),
                            static_cast<QWidget*>(m_widgets.shadowOffsetYSpin),
                            static_cast<QWidget*>(m_widgets.shadowOpacitySpin)}) {
        if (widget) {
            widget->setEnabled(enabled);
        }
    }
    if (enabled && m_widgets.featherPowerSpin && m_widgets.featherFalloffCombo) {
        m_widgets.featherPowerSpin->setEnabled(
            m_widgets.featherFalloffCombo->currentData().toInt() == 0);
    }
    if (enabled && m_widgets.showOnlyCheck) {
        const TimelineClip* clip = m_deps.getSelectedClip ? m_deps.getSelectedClip() : nullptr;
        m_widgets.showOnlyCheck->setEnabled(clip && !clip->maskForegroundLayerEnabled);
    }
    if (enabled) {
        const bool repeatEnabled =
            m_widgets.repeatEnabledCheck && m_widgets.repeatEnabledCheck->isChecked();
        if (m_widgets.repeatDeltaXSpin) {
            m_widgets.repeatDeltaXSpin->setEnabled(repeatEnabled);
        }
        if (m_widgets.repeatDeltaYSpin) {
            m_widgets.repeatDeltaYSpin->setEnabled(repeatEnabled);
        }
    }
}
