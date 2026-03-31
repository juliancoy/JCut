#include "effects_tab.h"

#include <QSignalBlocker>
#include <QDir>

EffectsTab::EffectsTab(const Widgets& widgets, const Dependencies& deps, QObject* parent)
    : QObject(parent)
    , m_widgets(widgets)
    , m_deps(deps)
{
}

void EffectsTab::wire()
{
    if (m_widgets.maskFeatherSpin) {
        connect(m_widgets.maskFeatherSpin, qOverload<double>(&QDoubleSpinBox::valueChanged),
                this, &EffectsTab::onMaskFeatherChanged);
        connect(m_widgets.maskFeatherSpin, &QDoubleSpinBox::editingFinished,
                this, &EffectsTab::onEditingFinished);
    }
    if (m_widgets.maskFeatherEnabledCheck) {
        connect(m_widgets.maskFeatherEnabledCheck, &QCheckBox::toggled,
                this, &EffectsTab::onMaskFeatherEnabledChanged);
    }
    if (m_widgets.applyButton) {
        connect(m_widgets.applyButton, &QPushButton::clicked,
                this, &EffectsTab::onApplyClicked);
    }
}

void EffectsTab::refresh()
{
    if (!m_widgets.effectsPathLabel || !m_widgets.maskFeatherSpin) {
        return;
    }

    const TimelineClip* clip = m_deps.getSelectedClip();
    m_updating = true;

    QSignalBlocker featherBlock(m_widgets.maskFeatherSpin);
    QSignalBlocker enabledBlock(m_widgets.maskFeatherEnabledCheck);

    if (!clip || !m_deps.clipHasVisuals(*clip)) {
        m_widgets.effectsPathLabel->setText(QStringLiteral("No visual clip selected"));
        m_widgets.effectsPathLabel->setToolTip(QString());
        m_widgets.maskFeatherSpin->setValue(0.0);
        if (m_widgets.maskFeatherEnabledCheck) {
            m_widgets.maskFeatherEnabledCheck->setChecked(false);
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
    if (m_widgets.maskFeatherEnabledCheck) {
        m_widgets.maskFeatherEnabledCheck->setChecked(clip->maskFeather > 0.0);
    }

    // Disable mask feather controls if clip doesn't have alpha
    if (m_widgets.maskFeatherEnabledCheck) {
        m_widgets.maskFeatherEnabledCheck->setEnabled(hasAlpha);
    }
    m_widgets.maskFeatherSpin->setEnabled(hasAlpha && (!m_widgets.maskFeatherEnabledCheck || m_widgets.maskFeatherEnabledCheck->isChecked()));

    m_updating = false;
}

void EffectsTab::applyMaskFeather(bool pushHistory)
{
    if (m_updating) return;

    const TimelineClip* selectedClip = m_deps.getSelectedClip();
    if (!selectedClip || !m_deps.clipHasVisuals(*selectedClip)) return;

    const double featherValue = m_widgets.maskFeatherEnabledCheck && m_widgets.maskFeatherEnabledCheck->isChecked()
                                    ? m_widgets.maskFeatherSpin->value()
                                    : 0.0;

    const bool updated = m_deps.updateClipById(selectedClip->id, [featherValue](TimelineClip& clip) {
        clip.maskFeather = featherValue;
    });

    if (!updated) return;

    m_deps.setPreviewTimelineClips();
    m_deps.refreshInspector();
    m_deps.scheduleSaveState();
    if (pushHistory) {
        m_deps.pushHistorySnapshot();
    }
    emit effectsApplied();
}

void EffectsTab::onMaskFeatherChanged(double value)
{
    Q_UNUSED(value);
    if (m_updating) return;
    applyMaskFeather(false);
}

void EffectsTab::onMaskFeatherEnabledChanged(bool enabled)
{
    if (m_updating) return;
    
    if (m_widgets.maskFeatherSpin) {
        m_widgets.maskFeatherSpin->setEnabled(enabled);
    }
    
    applyMaskFeather(true);
}

void EffectsTab::onApplyClicked()
{
    applyMaskFeather(true);
}

void EffectsTab::onEditingFinished()
{
    if (m_updating) return;
    applyMaskFeather(true);
}
