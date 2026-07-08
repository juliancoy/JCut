#include "mask_tab.h"
#include "editor_tab_edit_effects.h"
#include "grading_histogram_widget.h"

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
    if (m_widgets.resetGradeButton) {
        connect(m_widgets.resetGradeButton, &QPushButton::clicked, this, [this]() {
            resetGrade();
        });
    }
    if (m_widgets.curveChannelCombo) {
        connect(m_widgets.curveChannelCombo, qOverload<int>(&QComboBox::currentIndexChanged), this, [this](int) {
            updateCurveWidget();
        });
    }
    if (m_widgets.curveSmoothingCheck) {
        connect(m_widgets.curveSmoothingCheck, &QCheckBox::toggled, this, [this](bool checked) {
            if (m_updating) {
                return;
            }
            m_curveSmoothingEnabled = checked;
            updateCurveWidget();
            apply(true);
        });
    }
    if (m_widgets.histogramWidget) {
        connect(m_widgets.histogramWidget, &GradingHistogramWidget::curvePointsAdjusted,
                this, [this](const QVector<QPointF>& points, bool finalized) {
                    if (m_updating) {
                        return;
                    }
                    applyCurvePointsToCurrentChannel(points);
                    apply(finalized);
                });
    }
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
        setSpin(m_widgets.featherSpin, clip->maskFeather);
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
        setCheck(m_widgets.gradeEnabledCheck, clip->maskGradeEnabled);
        setSpin(m_widgets.gradeBrightnessSpin, clip->maskGradeBrightness);
        setSpin(m_widgets.gradeContrastSpin, clip->maskGradeContrast);
        setSpin(m_widgets.gradeSaturationSpin, clip->maskGradeSaturation);
        m_curvePointsR = sanitizeGradingCurvePoints(clip->maskGradeCurvePointsR);
        m_curvePointsG = sanitizeGradingCurvePoints(clip->maskGradeCurvePointsG);
        m_curvePointsB = sanitizeGradingCurvePoints(clip->maskGradeCurvePointsB);
        m_curvePointsLuma = sanitizeGradingCurvePoints(clip->maskGradeCurvePointsLuma);
        m_curveSmoothingEnabled = clip->maskGradeCurveSmoothingEnabled;
        setCheck(m_widgets.curveSmoothingCheck, m_curveSmoothingEnabled);
        updateCurveWidget();
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
        m_curvePointsR = defaultGradingCurvePoints();
        m_curvePointsG = defaultGradingCurvePoints();
        m_curvePointsB = defaultGradingCurvePoints();
        m_curvePointsLuma = defaultGradingCurvePoints();
        m_curveSmoothingEnabled = true;
        setCheck(m_widgets.curveSmoothingCheck, m_curveSmoothingEnabled);
        updateCurveWidget();
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
        clip.maskShowOnly =
            !clip.maskForegroundLayerEnabled &&
            m_widgets.showOnlyCheck &&
            m_widgets.showOnlyCheck->isChecked();
        clip.maskOpacity = m_widgets.opacitySpin ? m_widgets.opacitySpin->value() : 1.0;
        clip.maskGradeEnabled = m_widgets.gradeEnabledCheck && m_widgets.gradeEnabledCheck->isChecked();
        clip.maskGradeBrightness = m_widgets.gradeBrightnessSpin ? m_widgets.gradeBrightnessSpin->value() : 0.0;
        clip.maskGradeContrast = m_widgets.gradeContrastSpin ? m_widgets.gradeContrastSpin->value() : 1.0;
        clip.maskGradeSaturation = m_widgets.gradeSaturationSpin ? m_widgets.gradeSaturationSpin->value() : 1.0;
        clip.maskGradeCurvePointsR = sanitizeGradingCurvePoints(m_curvePointsR);
        clip.maskGradeCurvePointsG = sanitizeGradingCurvePoints(m_curvePointsG);
        clip.maskGradeCurvePointsB = sanitizeGradingCurvePoints(m_curvePointsB);
        clip.maskGradeCurvePointsLuma = sanitizeGradingCurvePoints(m_curvePointsLuma);
        clip.maskGradeCurveSmoothingEnabled = m_curveSmoothingEnabled;
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
                            static_cast<QWidget*>(m_widgets.showOnlyCheck),
                            static_cast<QWidget*>(m_widgets.opacitySpin),
                            static_cast<QWidget*>(m_widgets.gradeEnabledCheck),
                            static_cast<QWidget*>(m_widgets.gradeBrightnessSpin),
                            static_cast<QWidget*>(m_widgets.gradeContrastSpin),
                            static_cast<QWidget*>(m_widgets.gradeSaturationSpin),
                            static_cast<QWidget*>(m_widgets.resetGradeButton),
                            static_cast<QWidget*>(m_widgets.curveChannelCombo),
                            static_cast<QWidget*>(m_widgets.histogramWidget),
                            static_cast<QWidget*>(m_widgets.curveSmoothingCheck),
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

void MaskTab::resetGrade()
{
    if (m_updating) {
        return;
    }

    auto setSpin = [](QDoubleSpinBox* spin, double value) {
        if (!spin) {
            return;
        }
        QSignalBlocker blocker(spin);
        spin->setValue(value);
    };
    auto setCheck = [](QCheckBox* check, bool value) {
        if (!check) {
            return;
        }
        QSignalBlocker blocker(check);
        check->setChecked(value);
    };

    setCheck(m_widgets.gradeEnabledCheck, false);
    setSpin(m_widgets.gradeBrightnessSpin, 0.0);
    setSpin(m_widgets.gradeContrastSpin, 1.0);
    setSpin(m_widgets.gradeSaturationSpin, 1.0);
    m_curvePointsR = defaultGradingCurvePoints();
    m_curvePointsG = defaultGradingCurvePoints();
    m_curvePointsB = defaultGradingCurvePoints();
    m_curvePointsLuma = defaultGradingCurvePoints();
    m_curveSmoothingEnabled = true;
    setCheck(m_widgets.curveSmoothingCheck, m_curveSmoothingEnabled);
    updateCurveWidget();
    apply(true);
}

QVector<QPointF> MaskTab::currentCurvePoints() const
{
    const int channelIndex = m_widgets.curveChannelCombo
                                 ? qBound(0, m_widgets.curveChannelCombo->currentIndex(), 3)
                                 : 0;
    if (channelIndex == 1) {
        return sanitizeGradingCurvePoints(m_curvePointsG);
    }
    if (channelIndex == 2) {
        return sanitizeGradingCurvePoints(m_curvePointsB);
    }
    if (channelIndex == 3) {
        return sanitizeGradingCurvePoints(m_curvePointsLuma);
    }
    return sanitizeGradingCurvePoints(m_curvePointsR);
}

void MaskTab::applyCurvePointsToCurrentChannel(const QVector<QPointF>& points)
{
    const QVector<QPointF> sanitized = sanitizeGradingCurvePoints(points);
    const int channelIndex = m_widgets.curveChannelCombo
                                 ? qBound(0, m_widgets.curveChannelCombo->currentIndex(), 3)
                                 : 0;
    if (channelIndex == 1) {
        m_curvePointsG = sanitized;
    } else if (channelIndex == 2) {
        m_curvePointsB = sanitized;
    } else if (channelIndex == 3) {
        m_curvePointsLuma = sanitized;
    } else {
        m_curvePointsR = sanitized;
    }
}

void MaskTab::updateCurveWidget()
{
    if (!m_widgets.histogramWidget) {
        return;
    }
    const QSignalBlocker blocker(m_widgets.histogramWidget);
    const int channelIndex = m_widgets.curveChannelCombo
                                 ? qBound(0, m_widgets.curveChannelCombo->currentIndex(), 3)
                                 : 0;
    m_widgets.histogramWidget->setSelectedChannel(
        channelIndex == 1 ? GradingHistogramWidget::Channel::Green
        : channelIndex == 2 ? GradingHistogramWidget::Channel::Blue
        : channelIndex == 3 ? GradingHistogramWidget::Channel::Brightness
                            : GradingHistogramWidget::Channel::Red);
    const QColor channelBackground =
        channelIndex == 0 ? QColor(54, 21, 24, 220)
        : channelIndex == 1 ? QColor(20, 48, 28, 220)
        : channelIndex == 2 ? QColor(18, 30, 58, 220)
                            : QColor(52, 42, 13, 220);
    m_widgets.histogramWidget->setChartBackgroundColor(channelBackground);
    m_widgets.histogramWidget->setCurveSmoothingEnabled(m_curveSmoothingEnabled);
    m_widgets.histogramWidget->setThreePointLockEnabled(false);
    m_widgets.histogramWidget->setCurvePoints(currentCurvePoints());
}
