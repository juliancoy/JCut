#include "transcript_tab.h"

#include "editor_tab_edit_effects.h"

#include <QFont>
#include <QSignalBlocker>

#include <cmath>

namespace {

TabEditCallbacks transcriptOverlayEditCallbacks(const TranscriptTab::Dependencies& deps)
{
    return TabEditCallbacks{
        .updatePreview = deps.setPreviewTimelineClips,
        .refreshInspector = deps.refreshInspector,
        .scheduleSave = deps.scheduleSaveState,
        .pushHistory = deps.pushHistorySnapshot,
    };
}

} // namespace

void TranscriptTab::applyOverlayFromInspector(bool pushHistory)
{
    if (m_updating || !m_deps.getSelectedClip || !m_deps.updateClipById) return;

    const TimelineClip* selectedClip = m_deps.getSelectedClip();
    if (!selectedClip) return;

    const bool updated = m_deps.updateClipById(selectedClip->id, [this](TimelineClip& clip) {
        clip.transcriptOverlay.enabled = m_widgets.transcriptOverlayEnabledCheckBox &&
                                         m_widgets.transcriptOverlayEnabledCheckBox->isChecked();
        clip.transcriptOverlay.showBackground = m_widgets.transcriptBackgroundVisibleCheckBox &&
                                                m_widgets.transcriptBackgroundVisibleCheckBox->isChecked();
        clip.transcriptOverlay.backgroundOpacity = m_widgets.transcriptBackgroundOpacitySpin
            ? qBound<qreal>(0.0, m_widgets.transcriptBackgroundOpacitySpin->value() / 100.0, 1.0)
            : 120.0 / 255.0;
        clip.transcriptOverlay.backgroundCornerRadius = m_widgets.transcriptBackgroundCornerRadiusSpin
            ? qBound<qreal>(0.0, m_widgets.transcriptBackgroundCornerRadiusSpin->value(), 128.0)
            : 14.0;
        clip.transcriptOverlay.showShadow = m_widgets.transcriptShadowEnabledCheckBox &&
                                            m_widgets.transcriptShadowEnabledCheckBox->isChecked();
        clip.transcriptOverlay.showSpeakerTitle = m_widgets.transcriptShowSpeakerTitleCheckBox &&
                                                  m_widgets.transcriptShowSpeakerTitleCheckBox->isChecked();
        clip.transcriptOverlay.maxLines = m_widgets.transcriptMaxLinesSpin
            ? m_widgets.transcriptMaxLinesSpin->value()
            : 2;
        clip.transcriptOverlay.maxCharsPerLine = qMax(
            TimelineClip::TranscriptOverlaySettings::kMinReadableCharsPerLine,
            m_widgets.transcriptMaxCharsSpin ? m_widgets.transcriptMaxCharsSpin->value() : 28);
        if (m_widgets.transcriptAutoScrollCheckBox) {
            clip.transcriptOverlay.autoScroll =
                m_widgets.transcriptAutoScrollCheckBox->isChecked();
        }
        clip.transcriptOverlay.translationX = m_widgets.transcriptOverlayXSpin
            ? m_widgets.transcriptOverlayXSpin->value()
            : 0.0;
        clip.transcriptOverlay.translationY = m_widgets.transcriptOverlayYSpin
            ? m_widgets.transcriptOverlayYSpin->value()
            : 0.0;
        const bool requestedManualPlacement = m_widgets.transcriptPlacementModeCombo
            ? m_widgets.transcriptPlacementModeCombo->currentData().toBool()
            : clip.transcriptOverlay.useManualPlacement;
        clip.transcriptOverlay.useManualPlacement = requestedManualPlacement;
        clip.transcriptOverlay.boxWidth = qMax<qreal>(
            TimelineClip::TranscriptOverlaySettings::kMinReadableBoxWidth,
            m_widgets.transcriptOverlayWidthSpin ? m_widgets.transcriptOverlayWidthSpin->value() : 900.0);
        clip.transcriptOverlay.boxHeight = qMax<qreal>(
            TimelineClip::TranscriptOverlaySettings::kMinReadableBoxHeight,
            m_widgets.transcriptOverlayHeightSpin ? m_widgets.transcriptOverlayHeightSpin->value() : 220.0);
        clip.transcriptOverlay.fontFamily = m_widgets.transcriptFontFamilyCombo
            ? m_widgets.transcriptFontFamilyCombo->currentFont().family()
            : kDefaultFontFamily;
        clip.transcriptOverlay.fontPointSize = qMax<qreal>(
            TimelineClip::TranscriptOverlaySettings::kMinReadableFontPointSize,
            m_widgets.transcriptFontSizeSpin ? m_widgets.transcriptFontSizeSpin->value() : 42);
        clip.transcriptOverlay.bold = m_widgets.transcriptBoldCheckBox &&
                                      m_widgets.transcriptBoldCheckBox->isChecked();
        clip.transcriptOverlay.italic = m_widgets.transcriptItalicCheckBox &&
                                        m_widgets.transcriptItalicCheckBox->isChecked();
    });

    if (!updated) return;

    applyTabEditEffects(transcriptOverlayEditCallbacks(m_deps),
                        TabEditEffects{.pushHistory = pushHistory});
}

void TranscriptTab::onOverlaySettingChanged()
{
    const bool transformEdited =
        sender() == m_widgets.transcriptOverlayXSpin ||
        sender() == m_widgets.transcriptOverlayYSpin ||
        sender() == m_widgets.transcriptOverlayWidthSpin ||
        sender() == m_widgets.transcriptOverlayHeightSpin;
    if (transformEdited) {
        setTranscriptPlacementMode(true);
    }
    applyOverlayFromInspector(true);
}

void TranscriptTab::onCenterHorizontalClicked()
{
    if (m_updating || !m_widgets.transcriptOverlayXSpin) {
        return;
    }
    if (std::abs(m_widgets.transcriptOverlayXSpin->value()) >= 0.0001) {
        QSignalBlocker block(m_widgets.transcriptOverlayXSpin);
        m_widgets.transcriptOverlayXSpin->setValue(0.0);
    }
    setTranscriptPlacementMode(true);
    applyOverlayFromInspector(true);
}

void TranscriptTab::onCenterVerticalClicked()
{
    if (m_updating || !m_widgets.transcriptOverlayYSpin) {
        return;
    }
    if (std::abs(m_widgets.transcriptOverlayYSpin->value()) >= 0.0001) {
        QSignalBlocker block(m_widgets.transcriptOverlayYSpin);
        m_widgets.transcriptOverlayYSpin->setValue(0.0);
    }
    setTranscriptPlacementMode(true);
    applyOverlayFromInspector(true);
}

void TranscriptTab::setTranscriptPlacementMode(bool manual)
{
    if (!m_widgets.transcriptPlacementModeCombo) {
        return;
    }
    const int index = m_widgets.transcriptPlacementModeCombo->findData(manual);
    if (index < 0 || m_widgets.transcriptPlacementModeCombo->currentIndex() == index) {
        return;
    }
    QSignalBlocker block(m_widgets.transcriptPlacementModeCombo);
    m_widgets.transcriptPlacementModeCombo->setCurrentIndex(index);
}

void TranscriptTab::updateOverlayWidgetsFromClip(const TimelineClip& clip)
{
    if (!m_widgets.transcriptOverlayEnabledCheckBox) return;

    QSignalBlocker enabledBlock(m_widgets.transcriptOverlayEnabledCheckBox);
    QSignalBlocker placementBlock(m_widgets.transcriptPlacementModeCombo);
    QSignalBlocker backgroundBlock(m_widgets.transcriptBackgroundVisibleCheckBox);
    QSignalBlocker backgroundOpacityBlock(m_widgets.transcriptBackgroundOpacitySpin);
    QSignalBlocker backgroundCornerRadiusBlock(m_widgets.transcriptBackgroundCornerRadiusSpin);
    QSignalBlocker shadowBlock(m_widgets.transcriptShadowEnabledCheckBox);
    QSignalBlocker titleBlock(m_widgets.transcriptShowSpeakerTitleCheckBox);
    QSignalBlocker maxLinesBlock(m_widgets.transcriptMaxLinesSpin);
    QSignalBlocker maxCharsBlock(m_widgets.transcriptMaxCharsSpin);
    QSignalBlocker autoScrollBlock(m_widgets.transcriptAutoScrollCheckBox);
    QSignalBlocker xBlock(m_widgets.transcriptOverlayXSpin);
    QSignalBlocker yBlock(m_widgets.transcriptOverlayYSpin);
    QSignalBlocker widthBlock(m_widgets.transcriptOverlayWidthSpin);
    QSignalBlocker heightBlock(m_widgets.transcriptOverlayHeightSpin);
    QSignalBlocker fontBlock(m_widgets.transcriptFontFamilyCombo);
    QSignalBlocker fontSizeBlock(m_widgets.transcriptFontSizeSpin);
    QSignalBlocker boldBlock(m_widgets.transcriptBoldCheckBox);
    QSignalBlocker italicBlock(m_widgets.transcriptItalicCheckBox);

    m_widgets.transcriptOverlayEnabledCheckBox->setChecked(clip.transcriptOverlay.enabled);
    if (m_widgets.transcriptPlacementModeCombo) {
        const int placementIndex =
            m_widgets.transcriptPlacementModeCombo->findData(clip.transcriptOverlay.useManualPlacement);
        if (placementIndex >= 0) {
            m_widgets.transcriptPlacementModeCombo->setCurrentIndex(placementIndex);
        }
    }
    if (m_widgets.transcriptBackgroundVisibleCheckBox) {
        m_widgets.transcriptBackgroundVisibleCheckBox->setChecked(clip.transcriptOverlay.showBackground);
    }
    if (m_widgets.transcriptBackgroundOpacitySpin) {
        m_widgets.transcriptBackgroundOpacitySpin->setValue(
            static_cast<int>(std::round(qBound<qreal>(0.0, clip.transcriptOverlay.backgroundOpacity, 1.0) * 100.0)));
    }
    if (m_widgets.transcriptBackgroundCornerRadiusSpin) {
        m_widgets.transcriptBackgroundCornerRadiusSpin->setValue(
            static_cast<int>(std::round(qBound<qreal>(0.0, clip.transcriptOverlay.backgroundCornerRadius, 128.0))));
    }
    if (m_widgets.transcriptShadowEnabledCheckBox) {
        m_widgets.transcriptShadowEnabledCheckBox->setChecked(clip.transcriptOverlay.showShadow);
    }
    if (m_widgets.transcriptShowSpeakerTitleCheckBox) {
        m_widgets.transcriptShowSpeakerTitleCheckBox->setChecked(clip.transcriptOverlay.showSpeakerTitle);
    }
    if (m_widgets.transcriptMaxLinesSpin) {
        m_widgets.transcriptMaxLinesSpin->setValue(clip.transcriptOverlay.maxLines);
    }
    if (m_widgets.transcriptMaxCharsSpin) {
        m_widgets.transcriptMaxCharsSpin->setValue(
            qMax(TimelineClip::TranscriptOverlaySettings::kMinReadableCharsPerLine,
                 clip.transcriptOverlay.maxCharsPerLine));
    }
    if (m_widgets.transcriptAutoScrollCheckBox) {
        m_widgets.transcriptAutoScrollCheckBox->setChecked(clip.transcriptOverlay.autoScroll);
    }
    if (m_widgets.transcriptOverlayXSpin) {
        m_widgets.transcriptOverlayXSpin->setValue(clip.transcriptOverlay.translationX);
    }
    if (m_widgets.transcriptOverlayYSpin) {
        m_widgets.transcriptOverlayYSpin->setValue(clip.transcriptOverlay.translationY);
    }
    if (m_widgets.transcriptOverlayWidthSpin) {
        m_widgets.transcriptOverlayWidthSpin->setValue(
            qMax(static_cast<int>(TimelineClip::TranscriptOverlaySettings::kMinReadableBoxWidth),
                 static_cast<int>(clip.transcriptOverlay.boxWidth)));
    }
    if (m_widgets.transcriptOverlayHeightSpin) {
        m_widgets.transcriptOverlayHeightSpin->setValue(
            qMax(static_cast<int>(TimelineClip::TranscriptOverlaySettings::kMinReadableBoxHeight),
                 static_cast<int>(clip.transcriptOverlay.boxHeight)));
    }
    if (m_widgets.transcriptFontFamilyCombo) {
        m_widgets.transcriptFontFamilyCombo->setCurrentFont(QFont(clip.transcriptOverlay.fontFamily));
    }
    if (m_widgets.transcriptFontSizeSpin) {
        m_widgets.transcriptFontSizeSpin->setValue(
            qMax(TimelineClip::TranscriptOverlaySettings::kMinReadableFontPointSize,
                 static_cast<int>(clip.transcriptOverlay.fontPointSize)));
    }
    if (m_widgets.transcriptBoldCheckBox) {
        m_widgets.transcriptBoldCheckBox->setChecked(clip.transcriptOverlay.bold);
    }
    if (m_widgets.transcriptItalicCheckBox) {
        m_widgets.transcriptItalicCheckBox->setChecked(clip.transcriptOverlay.italic);
    }
}
