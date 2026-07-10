#include "mask_tab.h"
#include "editor_tab_edit_effects.h"
#include "mask_sidecar.h"

#include <QComboBox>
#include <QDir>
#include <QFileInfo>
#include <QSignalBlocker>

MaskTab::MaskTab(const Widgets& widgets, const Dependencies& deps, QObject* parent)
    : QObject(parent)
    , m_widgets(widgets)
    , m_deps(deps)
{
}

namespace {
TabEditCallbacks maskEditCallbacks(const MaskTab::Dependencies& deps)
{
    return TabEditCallbacks{
        .updatePreview = deps.setPreviewTimelineClips,
        .refreshInspector = deps.refreshInspector,
        .scheduleSave = deps.scheduleSaveState,
        .pushHistory = deps.pushHistorySnapshot,
    };
}

bool maskDirHasFrames(const QFileInfo& info)
{
    if (!info.exists() || !info.isDir()) {
        return false;
    }
    QDir dir(info.absoluteFilePath());
    return !dir.entryList(QStringList{QStringLiteral("frame_*.png")}, QDir::Files, QDir::Name)
                .isEmpty();
}

QString autoMaskFramesDirForClip(const TimelineClip& clip)
{
    const QFileInfo mediaInfo(clip.filePath);
    const QString stem = mediaInfo.completeBaseName();
    if (stem.trimmed().isEmpty()) {
        return QString();
    }

    const QDir mediaDir = mediaInfo.dir();
    const QString exactPerson = mediaDir.absoluteFilePath(
        QStringLiteral("%1_sam3_person_binary_masks").arg(stem));
    QFileInfo exactPersonInfo(exactPerson);
    if (maskDirHasFrames(exactPersonInfo)) {
        return exactPersonInfo.absoluteFilePath();
    }

    const QFileInfoList candidates = mediaDir.entryInfoList(
        QStringList{QStringLiteral("%1_sam3_*_binary_masks").arg(stem)},
        QDir::Dirs | QDir::NoDotAndDotDot,
        QDir::Time);
    for (const QFileInfo& candidate : candidates) {
        if (maskDirHasFrames(candidate)) {
            return candidate.absoluteFilePath();
        }
    }

    return QString();
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
            if (m_widgets.enabledCheck) m_widgets.enabledCheck->setChecked(true);
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
                m_widgets.enabledCheck->setChecked(true);
            }
            apply(true);
        });
    }
    for (QCheckBox* check : {m_widgets.invertCheck,
                             m_widgets.showOnlyCheck,
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
    connectApply(m_widgets.shadowRadiusSpin);
    connectApply(m_widgets.shadowOffsetXSpin);
    connectApply(m_widgets.shadowOffsetYSpin);
    connectApply(m_widgets.shadowOpacitySpin);
}

void MaskTab::refresh()
{
    const TimelineClip* clip = m_deps.getSelectedClip ? m_deps.getSelectedClip() : nullptr;
    const bool validClip = clip && (!m_deps.clipHasVisuals || m_deps.clipHasVisuals(*clip)) &&
                           clip->mediaType == ClipMediaType::Video;
    m_updating = true;

    if (m_widgets.clipLabel) {
        m_widgets.clipLabel->setText(validClip
            ? QStringLiteral("%1\n%2").arg(clip->label, QDir::toNativeSeparators(clip->filePath))
            : QStringLiteral("Select a video clip to edit its mask."));
        m_widgets.clipLabel->setToolTip(validClip ? QDir::toNativeSeparators(clip->filePath) : QString());
    }

    if (m_widgets.enabledCheck) QSignalBlocker blocker(m_widgets.enabledCheck);
    setControlsEnabled(validClip);

    auto setSpin = [](QDoubleSpinBox* spin, double value) {
        if (!spin) return;
        spin->setValue(value);
    };
    auto setCheck = [](QCheckBox* check, bool value) {
        if (!check) return;
        QSignalBlocker blocker(check);
        check->setChecked(value);
    };

    bool effectiveMaskEnabled = validClip ? clip->maskEnabled : false;
    QString effectiveMaskFramesDir = validClip ? clip->maskFramesDir : QString();
    if (validClip && effectiveMaskFramesDir.trimmed().isEmpty()) {
        const QString discoveredMaskDir = autoMaskFramesDirForClip(*clip);
        if (!discoveredMaskDir.isEmpty() && m_deps.updateClipById) {
            const QString clipId = clip->id;
            if (m_deps.updateClipById(clipId, [discoveredMaskDir](TimelineClip& timelineClip) {
                    timelineClip.maskEnabled = true;
                    timelineClip.maskFramesDir = discoveredMaskDir;
                })) {
                effectiveMaskEnabled = true;
                effectiveMaskFramesDir = discoveredMaskDir;
                if (m_deps.setPreviewTimelineClips) {
                    m_deps.setPreviewTimelineClips();
                }
                if (m_deps.scheduleSaveState) {
                    m_deps.scheduleSaveState();
                }
            }
        }
    }

    if (validClip) {
        setCheck(m_widgets.enabledCheck, effectiveMaskEnabled);
        if (m_widgets.framesDirEdit) {
            QSignalBlocker blocker(m_widgets.framesDirEdit);
            m_widgets.framesDirEdit->setText(effectiveMaskFramesDir);
        }
        if (m_widgets.sidecarCombo) {
            QSignalBlocker blocker(m_widgets.sidecarCombo);
            m_widgets.sidecarCombo->clear();
            const QVector<editor::masks::MaskSidecar> sidecars =
                editor::masks::discoverMaskSidecars(*clip);
            for (const editor::masks::MaskSidecar& sidecar : sidecars) {
                const QString coverage = sidecar.firstFrame >= 0
                    ? QStringLiteral("%1 frames · %2–%3")
                          .arg(sidecar.frameCount).arg(sidecar.firstFrame).arg(sidecar.lastFrame)
                    : QStringLiteral("%1 frames").arg(sidecar.frameCount);
                m_widgets.sidecarCombo->addItem(
                    QStringLiteral("%1 (%2)").arg(sidecar.displayName, coverage),
                    sidecar.directory);
                m_widgets.sidecarCombo->setItemData(
                    m_widgets.sidecarCombo->count() - 1, sidecar.id, Qt::UserRole + 1);
            }
            if (sidecars.isEmpty()) {
                m_widgets.sidecarCombo->addItem(QStringLiteral("No sidecar masks found"), QString());
            }
            const int selected = m_widgets.sidecarCombo->findData(effectiveMaskFramesDir);
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
            m_widgets.featherPowerSpin->setEnabled(clip->maskFeatherFalloff == 0);
        }
        setSpin(m_widgets.dilateSpin, clip->maskDilate);
        setSpin(m_widgets.erodeSpin, clip->maskErode);
        setSpin(m_widgets.blurSpin, clip->maskBlur);
        setCheck(m_widgets.invertCheck, clip->maskInvert);
        const bool showOnlyAvailable = !clip->maskForegroundLayerEnabled;
        setCheck(m_widgets.showOnlyCheck, showOnlyAvailable && clip->maskShowOnly);
        if (m_widgets.showOnlyCheck) {
            m_widgets.showOnlyCheck->setEnabled(showOnlyAvailable);
        }
        setSpin(m_widgets.opacitySpin, clip->maskOpacity);
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

void MaskTab::apply(bool pushHistory)
{
    if (m_updating || !m_deps.updateClipById) {
        return;
    }
    const TimelineClip* selectedClip = m_deps.getSelectedClip ? m_deps.getSelectedClip() : nullptr;
    if (!selectedClip || selectedClip->mediaType != ClipMediaType::Video) {
        return;
    }

    const QString id = selectedClip->id;
    const bool updated = m_deps.updateClipById(id, [this](TimelineClip& clip) {
        clip.maskEnabled = m_widgets.enabledCheck && m_widgets.enabledCheck->isChecked();
        clip.maskFramesDir = m_widgets.framesDirEdit ? m_widgets.framesDirEdit->text().trimmed() : QString();
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
                            static_cast<QWidget*>(m_widgets.featherSpin),
                            static_cast<QWidget*>(m_widgets.featherFalloffCombo),
                            static_cast<QWidget*>(m_widgets.featherPowerSpin),
                            static_cast<QWidget*>(m_widgets.dilateSpin),
                            static_cast<QWidget*>(m_widgets.erodeSpin),
                            static_cast<QWidget*>(m_widgets.blurSpin),
                            static_cast<QWidget*>(m_widgets.invertCheck),
                            static_cast<QWidget*>(m_widgets.showOnlyCheck),
                            static_cast<QWidget*>(m_widgets.opacitySpin),
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
