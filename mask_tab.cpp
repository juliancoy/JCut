#include "mask_tab.h"
#include "editor_tab_edit_effects.h"

#include <QDir>
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
                             m_widgets.gradeEnabledCheck,
                             m_widgets.shadowEnabledCheck}) {
        if (check) {
            connect(check, &QCheckBox::toggled, this, [this](bool) { apply(true); });
        }
    }
    connectApply(m_widgets.featherSpin);
    connectApply(m_widgets.dilateSpin);
    connectApply(m_widgets.erodeSpin);
    connectApply(m_widgets.blurSpin);
    connectApply(m_widgets.opacitySpin);
    connectApply(m_widgets.gradeBrightnessSpin);
    connectApply(m_widgets.gradeContrastSpin);
    connectApply(m_widgets.gradeSaturationSpin);
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
        QSignalBlocker blocker(spin);
        spin->setValue(value);
    };
    auto setCheck = [](QCheckBox* check, bool value) {
        if (!check) return;
        QSignalBlocker blocker(check);
        check->setChecked(value);
    };

    if (validClip) {
        setCheck(m_widgets.enabledCheck, clip->maskEnabled);
        if (m_widgets.framesDirEdit) {
            QSignalBlocker blocker(m_widgets.framesDirEdit);
            m_widgets.framesDirEdit->setText(clip->maskFramesDir);
        }
        setSpin(m_widgets.featherSpin, clip->maskFeather);
        setSpin(m_widgets.dilateSpin, clip->maskDilate);
        setSpin(m_widgets.erodeSpin, clip->maskErode);
        setSpin(m_widgets.blurSpin, clip->maskBlur);
        setCheck(m_widgets.invertCheck, clip->maskInvert);
        setSpin(m_widgets.opacitySpin, clip->maskOpacity);
        setCheck(m_widgets.gradeEnabledCheck, clip->maskGradeEnabled);
        setSpin(m_widgets.gradeBrightnessSpin, clip->maskGradeBrightness);
        setSpin(m_widgets.gradeContrastSpin, clip->maskGradeContrast);
        setSpin(m_widgets.gradeSaturationSpin, clip->maskGradeSaturation);
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
        clip.maskDilate = m_widgets.dilateSpin ? m_widgets.dilateSpin->value() : 0.0;
        clip.maskErode = m_widgets.erodeSpin ? m_widgets.erodeSpin->value() : 0.0;
        clip.maskBlur = m_widgets.blurSpin ? m_widgets.blurSpin->value() : 0.0;
        clip.maskInvert = m_widgets.invertCheck && m_widgets.invertCheck->isChecked();
        clip.maskOpacity = m_widgets.opacitySpin ? m_widgets.opacitySpin->value() : 1.0;
        clip.maskGradeEnabled = m_widgets.gradeEnabledCheck && m_widgets.gradeEnabledCheck->isChecked();
        clip.maskGradeBrightness = m_widgets.gradeBrightnessSpin ? m_widgets.gradeBrightnessSpin->value() : 0.0;
        clip.maskGradeContrast = m_widgets.gradeContrastSpin ? m_widgets.gradeContrastSpin->value() : 1.0;
        clip.maskGradeSaturation = m_widgets.gradeSaturationSpin ? m_widgets.gradeSaturationSpin->value() : 1.0;
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
                            static_cast<QWidget*>(m_widgets.browseButton),
                            static_cast<QWidget*>(m_widgets.featherSpin),
                            static_cast<QWidget*>(m_widgets.dilateSpin),
                            static_cast<QWidget*>(m_widgets.erodeSpin),
                            static_cast<QWidget*>(m_widgets.blurSpin),
                            static_cast<QWidget*>(m_widgets.invertCheck),
                            static_cast<QWidget*>(m_widgets.opacitySpin),
                            static_cast<QWidget*>(m_widgets.gradeEnabledCheck),
                            static_cast<QWidget*>(m_widgets.gradeBrightnessSpin),
                            static_cast<QWidget*>(m_widgets.gradeContrastSpin),
                            static_cast<QWidget*>(m_widgets.gradeSaturationSpin),
                            static_cast<QWidget*>(m_widgets.shadowEnabledCheck),
                            static_cast<QWidget*>(m_widgets.shadowRadiusSpin),
                            static_cast<QWidget*>(m_widgets.shadowOffsetXSpin),
                            static_cast<QWidget*>(m_widgets.shadowOffsetYSpin),
                            static_cast<QWidget*>(m_widgets.shadowOpacitySpin)}) {
        if (widget) {
            widget->setEnabled(enabled);
        }
    }
}
