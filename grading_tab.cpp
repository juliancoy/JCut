#include "grading_tab.h"
#include "clip_serialization.h"
#include "editor_shared.h"
#include "keyframe_table_shared.h"

#include <QMenu>
#include <QHeaderView>
#include <QSignalBlocker>
#include <QMessageBox>
#include <QInputDialog>
#include <QApplication>
#include <QDir>
#include <QColor>
#include <cmath>

GradingTab::GradingTab(const Widgets& widgets, const Dependencies& deps, QObject* parent)
    : KeyframeTabBase(deps, parent)
    , m_widgets(widgets)
{
    m_deferredSeekTimer.setSingleShot(true);
    connect(&m_deferredSeekTimer, &QTimer::timeout, this, [this]() {
        if (m_pendingSeekTimelineFrame < 0 || !m_deps.seekToTimelineFrame) {
            return;
        }
        m_deps.seekToTimelineFrame(m_pendingSeekTimelineFrame);
        m_pendingSeekTimelineFrame = -1;
    });
}

void GradingTab::wire()
{
    if (m_widgets.brightnessSpin) {
        connect(m_widgets.brightnessSpin, qOverload<double>(&QDoubleSpinBox::valueChanged),
                this, &GradingTab::onBrightnessChanged);
        connect(m_widgets.brightnessSpin, &QDoubleSpinBox::editingFinished,
                this, &GradingTab::onBrightnessEditingFinished);
    }
    if (m_widgets.contrastSpin) {
        connect(m_widgets.contrastSpin, qOverload<double>(&QDoubleSpinBox::valueChanged),
                this, &GradingTab::onContrastChanged);
        connect(m_widgets.contrastSpin, &QDoubleSpinBox::editingFinished,
                this, &GradingTab::onContrastEditingFinished);
    }
    if (m_widgets.saturationSpin) {
        connect(m_widgets.saturationSpin, qOverload<double>(&QDoubleSpinBox::valueChanged),
                this, &GradingTab::onSaturationChanged);
        connect(m_widgets.saturationSpin, &QDoubleSpinBox::editingFinished,
                this, &GradingTab::onSaturationEditingFinished);
    }
    if (m_widgets.opacitySpin) {
        connect(m_widgets.opacitySpin, qOverload<double>(&QDoubleSpinBox::valueChanged),
                this, &GradingTab::onOpacityChanged);
        connect(m_widgets.opacitySpin, &QDoubleSpinBox::editingFinished,
                this, &GradingTab::onOpacityEditingFinished);
    }
    // Shadows/Midtones/Highlights connections
    auto connectToneSpin = [this](QDoubleSpinBox* spin, void (GradingTab::*changedSlot)(double)) {
        if (spin) {
            connect(spin, qOverload<double>(&QDoubleSpinBox::valueChanged), this, changedSlot);
            connect(spin, &QDoubleSpinBox::editingFinished, this, &GradingTab::onBrightnessEditingFinished);
        }
    };
    connectToneSpin(m_widgets.shadowsRSpin, &GradingTab::onShadowsRChanged);
    connectToneSpin(m_widgets.shadowsGSpin, &GradingTab::onShadowsGChanged);
    connectToneSpin(m_widgets.shadowsBSpin, &GradingTab::onShadowsBChanged);
    connectToneSpin(m_widgets.midtonesRSpin, &GradingTab::onMidtonesRChanged);
    connectToneSpin(m_widgets.midtonesGSpin, &GradingTab::onMidtonesGChanged);
    connectToneSpin(m_widgets.midtonesBSpin, &GradingTab::onMidtonesBChanged);
    connectToneSpin(m_widgets.highlightsRSpin, &GradingTab::onHighlightsRChanged);
    connectToneSpin(m_widgets.highlightsGSpin, &GradingTab::onHighlightsGChanged);
    connectToneSpin(m_widgets.highlightsBSpin, &GradingTab::onHighlightsBChanged);
    
    if (m_widgets.gradingAutoScrollCheckBox) {
        connect(m_widgets.gradingAutoScrollCheckBox, &QCheckBox::toggled,
                this, &GradingTab::onAutoScrollToggled);
    }
    if (m_widgets.gradingFollowCurrentCheckBox) {
        connect(m_widgets.gradingFollowCurrentCheckBox, &QCheckBox::toggled,
                this, &GradingTab::onFollowCurrentToggled);
    }
    if (m_widgets.gradingKeyAtPlayheadButton) {
        connect(m_widgets.gradingKeyAtPlayheadButton, &QPushButton::clicked,
                this, &GradingTab::onKeyAtPlayheadClicked);
    }
    if (m_widgets.gradingFadeInButton) {
        connect(m_widgets.gradingFadeInButton, &QPushButton::clicked,
                this, &GradingTab::onFadeInClicked);
    }
    if (m_widgets.gradingFadeOutButton) {
        connect(m_widgets.gradingFadeOutButton, &QPushButton::clicked,
                this, &GradingTab::onFadeOutClicked);
    }
    if (m_widgets.gradingKeyframeTable) {
        connect(m_widgets.gradingKeyframeTable, &QTableWidget::itemSelectionChanged,
                this, &GradingTab::onTableSelectionChanged);
        connect(m_widgets.gradingKeyframeTable, &QTableWidget::itemChanged,
                this, &GradingTab::onTableItemChanged);
        connect(m_widgets.gradingKeyframeTable, &QTableWidget::itemClicked,
                this, &GradingTab::onTableItemClicked);
        connect(m_widgets.gradingKeyframeTable, &QTableWidget::itemDoubleClicked,
                this, [this](QTableWidgetItem*) {});
        m_widgets.gradingKeyframeTable->setContextMenuPolicy(Qt::CustomContextMenu);
        connect(m_widgets.gradingKeyframeTable, &QWidget::customContextMenuRequested,
                this, &GradingTab::onTableCustomContextMenu);
        installTableHandlers(m_widgets.gradingKeyframeTable);
    }
}

void GradingTab::refresh()
{
    if (!m_widgets.gradingPathLabel || !m_widgets.brightnessSpin || !m_widgets.contrastSpin ||
        !m_widgets.saturationSpin || !m_widgets.opacitySpin || !m_widgets.gradingKeyframeTable) {
        return;
    }
    
    // Skip repaint when keyframes are selected (to avoid disrupting multi-selection)
    if (m_widgets.gradingKeyframeTable->selectedItems().count() > 0) {
        return;
    }

    const TimelineClip* clip = m_deps.getSelectedClip();
    m_updating = true;

    QSignalBlocker brightnessBlock(m_widgets.brightnessSpin);
    QSignalBlocker contrastBlock(m_widgets.contrastSpin);
    QSignalBlocker saturationBlock(m_widgets.saturationSpin);
    QSignalBlocker opacityBlock(m_widgets.opacitySpin);
    QSignalBlocker shadowsRBlock(m_widgets.shadowsRSpin);
    QSignalBlocker shadowsGBlock(m_widgets.shadowsGSpin);
    QSignalBlocker shadowsBBlock(m_widgets.shadowsBSpin);
    QSignalBlocker midtonesRBlock(m_widgets.midtonesRSpin);
    QSignalBlocker midtonesGBlock(m_widgets.midtonesGSpin);
    QSignalBlocker midtonesBBlock(m_widgets.midtonesBSpin);
    QSignalBlocker highlightsRBlock(m_widgets.highlightsRSpin);
    QSignalBlocker highlightsGBlock(m_widgets.highlightsGSpin);
    QSignalBlocker highlightsBBlock(m_widgets.highlightsBSpin);
    QSignalBlocker tableBlocker(m_widgets.gradingKeyframeTable);

    m_widgets.gradingKeyframeTable->clearContents();
    m_widgets.gradingKeyframeTable->setRowCount(0);

    if (!clip || !m_deps.clipHasVisuals(*clip)) {
        m_widgets.gradingPathLabel->setText(QStringLiteral("No visual clip selected"));
        m_widgets.gradingPathLabel->setToolTip(QString());
        m_widgets.brightnessSpin->setValue(0.0);
        m_widgets.contrastSpin->setValue(1.0);
        m_widgets.saturationSpin->setValue(1.0);
        m_widgets.opacitySpin->setValue(1.0);
        // Reset shadows/midtones/highlights
        if (m_widgets.shadowsRSpin) m_widgets.shadowsRSpin->setValue(0.0);
        if (m_widgets.shadowsGSpin) m_widgets.shadowsGSpin->setValue(0.0);
        if (m_widgets.shadowsBSpin) m_widgets.shadowsBSpin->setValue(0.0);
        if (m_widgets.midtonesRSpin) m_widgets.midtonesRSpin->setValue(0.0);
        if (m_widgets.midtonesGSpin) m_widgets.midtonesGSpin->setValue(0.0);
        if (m_widgets.midtonesBSpin) m_widgets.midtonesBSpin->setValue(0.0);
        if (m_widgets.highlightsRSpin) m_widgets.highlightsRSpin->setValue(0.0);
        if (m_widgets.highlightsGSpin) m_widgets.highlightsGSpin->setValue(0.0);
        if (m_widgets.highlightsBSpin) m_widgets.highlightsBSpin->setValue(0.0);
        m_selectedKeyframeFrame = -1;
        m_selectedKeyframeFrames.clear();
        m_updating = false;
        return;
    }

    const QString nativePath = QDir::toNativeSeparators(m_deps.getClipFilePath(*clip));
    const QString sourceLabel = QStringLiteral("%1 | %2")
                                    .arg(clipMediaTypeLabel(clip->mediaType),
                                         mediaSourceKindLabel(clip->sourceKind));
    m_widgets.gradingPathLabel->setText(QStringLiteral("%1\n%2").arg(clip->label, sourceLabel));
    m_widgets.gradingPathLabel->setToolTip(nativePath);

    populateTable(*clip);

    if (m_selectedKeyframeFrame < 0) {
        const int selectedIndex = clip->gradingKeyframes.isEmpty() ? 0 : nearestKeyframeIndex(*clip);
        m_selectedKeyframeFrame = selectedIndex >= 0 && selectedIndex < clip->gradingKeyframes.size()
                                     ? clip->gradingKeyframes[selectedIndex].frame
                                     : 0;
        m_selectedKeyframeFrames = {m_selectedKeyframeFrame};
    } else if (m_selectedKeyframeFrames.isEmpty()) {
        m_selectedKeyframeFrames = {m_selectedKeyframeFrame};
    }

    GradingKeyframeDisplay displayed = evaluateDisplayedGrading(*clip, clip->startFrame);
    const int selectedIndex = selectedKeyframeIndex(*clip);
    if (selectedIndex >= 0) {
        const auto& keyframe = clip->gradingKeyframes[selectedIndex];
        displayed.frame = keyframe.frame;
        displayed.brightness = keyframe.brightness;
        displayed.contrast = keyframe.contrast;
        displayed.saturation = keyframe.saturation;
        displayed.opacity = keyframe.opacity;
        displayed.shadowsR = keyframe.shadowsR;
        displayed.shadowsG = keyframe.shadowsG;
        displayed.shadowsB = keyframe.shadowsB;
        displayed.midtonesR = keyframe.midtonesR;
        displayed.midtonesG = keyframe.midtonesG;
        displayed.midtonesB = keyframe.midtonesB;
        displayed.highlightsR = keyframe.highlightsR;
        displayed.highlightsG = keyframe.highlightsG;
        displayed.highlightsB = keyframe.highlightsB;
        displayed.linearInterpolation = keyframe.linearInterpolation;
    }
    updateSpinBoxesFromKeyframe(displayed);

    editor::restoreSelectionByFrameRole(m_widgets.gradingKeyframeTable, m_selectedKeyframeFrames);

    m_updating = false;
    syncTableToPlayhead();
}

void GradingTab::applyGradeFromInspector(bool pushHistory)
{
    if (m_updating) return;

    const TimelineClip* selectedClip = m_deps.getSelectedClip();
    if (!selectedClip || !m_deps.clipHasVisuals(*selectedClip)) return;

    int64_t targetFrame = m_selectedKeyframeFrame;
    if (targetFrame < 0) {
        targetFrame = qBound<int64_t>(0,
                                      m_deps.getCurrentTimelineFrame() - selectedClip->startFrame,
                                      qMax<int64_t>(0, selectedClip->durationFrames - 1));
    }

    const bool updated = m_deps.updateClipById(selectedClip->id, [this, targetFrame](TimelineClip& clip) {
        TimelineClip::GradingKeyframe keyframe;
        keyframe.frame = targetFrame;
        keyframe.brightness = m_widgets.brightnessSpin->value();
        keyframe.contrast = m_widgets.contrastSpin->value();
        keyframe.saturation = m_widgets.saturationSpin->value();
        keyframe.opacity = m_widgets.opacitySpin->value();
        // Shadows/Midtones/Highlights
        keyframe.shadowsR = m_widgets.shadowsRSpin ? m_widgets.shadowsRSpin->value() : 0.0;
        keyframe.shadowsG = m_widgets.shadowsGSpin ? m_widgets.shadowsGSpin->value() : 0.0;
        keyframe.shadowsB = m_widgets.shadowsBSpin ? m_widgets.shadowsBSpin->value() : 0.0;
        keyframe.midtonesR = m_widgets.midtonesRSpin ? m_widgets.midtonesRSpin->value() : 0.0;
        keyframe.midtonesG = m_widgets.midtonesGSpin ? m_widgets.midtonesGSpin->value() : 0.0;
        keyframe.midtonesB = m_widgets.midtonesBSpin ? m_widgets.midtonesBSpin->value() : 0.0;
        keyframe.highlightsR = m_widgets.highlightsRSpin ? m_widgets.highlightsRSpin->value() : 0.0;
        keyframe.highlightsG = m_widgets.highlightsGSpin ? m_widgets.highlightsGSpin->value() : 0.0;
        keyframe.highlightsB = m_widgets.highlightsBSpin ? m_widgets.highlightsBSpin->value() : 0.0;
        keyframe.linearInterpolation = true;

        bool replaced = false;
        for (TimelineClip::GradingKeyframe& existing : clip.gradingKeyframes) {
            if (existing.frame == targetFrame) {
                keyframe.linearInterpolation = existing.linearInterpolation;
                existing = keyframe;
                replaced = true;
                break;
            }
        }
        if (!replaced) {
            clip.gradingKeyframes.push_back(keyframe);
        }
        normalizeClipGradingKeyframes(clip);
    });

    if (!updated) return;

    m_selectedKeyframeFrame = targetFrame;
    m_selectedKeyframeFrames = {targetFrame};
    m_deps.setPreviewTimelineClips();
    m_deps.refreshInspector();
    m_deps.scheduleSaveState();
    if (pushHistory) {
        m_deps.pushHistorySnapshot();
    }
    emit gradeApplied();
}

void GradingTab::upsertKeyframeAtPlayhead()
{
    const TimelineClip* clip = m_deps.getSelectedClip();
    if (!clip || !m_deps.clipHasVisuals(*clip)) return;

    m_selectedKeyframeFrame = qBound<int64_t>(0,
                                              m_deps.getCurrentTimelineFrame() - clip->startFrame,
                                              qMax<int64_t>(0, clip->durationFrames - 1));
    m_selectedKeyframeFrames = {m_selectedKeyframeFrame};
    applyGradeFromInspector(true);
    emit keyframeAdded();
}

void GradingTab::fadeInFromPlayhead()
{
    applyOpacityFadeFromPlayhead(true);
}

void GradingTab::fadeOutFromPlayhead()
{
    applyOpacityFadeFromPlayhead(false);
}

void GradingTab::syncTableToPlayhead()
{
    if (shouldSkipSyncToPlayhead(m_widgets.gradingKeyframeTable, m_widgets.gradingFollowCurrentCheckBox)) {
        return;
    }

    const TimelineClip* clip = m_deps.getSelectedClip();
    if (!clip || !m_deps.clipHasVisuals(*clip) || m_widgets.gradingKeyframeTable->rowCount() <= 0) {
        m_widgets.gradingKeyframeTable->clearSelection();
        return;
    }

    const int64_t localFrame = calculateLocalFrame(clip);

    int matchingRow = -1;
    int64_t matchingFrame = -1;
    for (int row = 0; row < m_widgets.gradingKeyframeTable->rowCount(); ++row) {
        QTableWidgetItem* item = m_widgets.gradingKeyframeTable->item(row, 0);
        if (!item) continue;
        const int64_t frame = item->data(Qt::UserRole).toLongLong();
        if (frame <= localFrame && frame >= matchingFrame) {
            matchingFrame = frame;
            matchingRow = row;
        }
    }
    if (matchingRow < 0) {
        matchingRow = 0;
    }

    if (!m_widgets.gradingKeyframeTable->selectionModel()->isRowSelected(matchingRow, QModelIndex())) {
        m_syncingTableSelection = true;
        m_widgets.gradingKeyframeTable->setCurrentCell(matchingRow, 0);
        m_widgets.gradingKeyframeTable->selectRow(matchingRow);
        m_syncingTableSelection = false;
    }

    if (m_widgets.gradingAutoScrollCheckBox && m_widgets.gradingAutoScrollCheckBox->isChecked()) {
        if (QTableWidgetItem* item = m_widgets.gradingKeyframeTable->item(matchingRow, 0)) {
            m_widgets.gradingKeyframeTable->scrollToItem(item, QAbstractItemView::PositionAtCenter);
        }
    }
}

void GradingTab::onBrightnessChanged(double value)
{
    Q_UNUSED(value);
    if (m_updating) return;
    applyGradeFromInspector(false);
}

void GradingTab::onContrastChanged(double value)
{
    Q_UNUSED(value);
    if (m_updating) return;
    applyGradeFromInspector(false);
}

void GradingTab::onSaturationChanged(double value)
{
    Q_UNUSED(value);
    if (m_updating) return;
    applyGradeFromInspector(false);
}

void GradingTab::onOpacityChanged(double value)
{
    Q_UNUSED(value);
    if (m_updating) return;
    applyGradeFromInspector(false);
}

// Shadows/Midtones/Highlights slots
void GradingTab::onShadowsRChanged(double value) { Q_UNUSED(value); if (m_updating) return; applyGradeFromInspector(false); }
void GradingTab::onShadowsGChanged(double value) { Q_UNUSED(value); if (m_updating) return; applyGradeFromInspector(false); }
void GradingTab::onShadowsBChanged(double value) { Q_UNUSED(value); if (m_updating) return; applyGradeFromInspector(false); }
void GradingTab::onMidtonesRChanged(double value) { Q_UNUSED(value); if (m_updating) return; applyGradeFromInspector(false); }
void GradingTab::onMidtonesGChanged(double value) { Q_UNUSED(value); if (m_updating) return; applyGradeFromInspector(false); }
void GradingTab::onMidtonesBChanged(double value) { Q_UNUSED(value); if (m_updating) return; applyGradeFromInspector(false); }
void GradingTab::onHighlightsRChanged(double value) { Q_UNUSED(value); if (m_updating) return; applyGradeFromInspector(false); }
void GradingTab::onHighlightsGChanged(double value) { Q_UNUSED(value); if (m_updating) return; applyGradeFromInspector(false); }
void GradingTab::onHighlightsBChanged(double value) { Q_UNUSED(value); if (m_updating) return; applyGradeFromInspector(false); }

void GradingTab::onAutoScrollToggled(bool checked)
{
    Q_UNUSED(checked);
    if (m_updating) return;
    m_deps.scheduleSaveState();
}

void GradingTab::onFollowCurrentToggled(bool checked)
{
    Q_UNUSED(checked);
    if (m_updating) return;
    if (m_widgets.gradingFollowCurrentCheckBox && m_widgets.gradingFollowCurrentCheckBox->isChecked()) {
        syncTableToPlayhead();
    }
    m_deps.scheduleSaveState();
}

void GradingTab::onKeyAtPlayheadClicked()
{
    upsertKeyframeAtPlayhead();
}

void GradingTab::onFadeInClicked()
{
    fadeInFromPlayhead();
}

void GradingTab::onFadeOutClicked()
{
    fadeOutFromPlayhead();
}

void GradingTab::onTableSelectionChanged()
{
    if (m_updating || m_syncingTableSelection) return;

    // Use base class method for deferred seek to timeline frame
    onTableSelectionChangedBase(m_widgets.gradingKeyframeTable, &m_deferredSeekTimer, &m_pendingSeekTimelineFrame);
    // Note: Don't call refresh() here - it disrupts multi-selection. 
    // The UI update happens through other refresh mechanisms.

    const QSet<int64_t> selectedFrames =
        editor::collectSelectedFrameRoles(m_widgets.gradingKeyframeTable);
    const int64_t primaryFrame =
        editor::primarySelectedFrameRole(m_widgets.gradingKeyframeTable);

    if (primaryFrame < 0) return;

    m_selectedKeyframeFrame = primaryFrame;
    m_selectedKeyframeFrames = selectedFrames;
    
    // Suppress auto-sync for this timeline frame to prevent the table from
    // jumping immediately after user clicks a row (which seeks to that frame).
    const TimelineClip* selectedClip = m_deps.getSelectedClip();
    if (selectedClip) {
        m_suppressSyncForTimelineFrame = selectedClip->startFrame + primaryFrame;
    }

    const TimelineClip* clip = m_deps.getSelectedClip();
    if (clip && m_deps.clipHasVisuals(*clip)) {
        GradingKeyframeDisplay displayed = evaluateDisplayedGrading(*clip, clip->startFrame + primaryFrame);
        for (const TimelineClip::GradingKeyframe& keyframe : clip->gradingKeyframes) {
            if (keyframe.frame == primaryFrame) {
                displayed.frame = keyframe.frame;
                displayed.brightness = keyframe.brightness;
                displayed.contrast = keyframe.contrast;
                displayed.saturation = keyframe.saturation;
                displayed.opacity = keyframe.opacity;
                displayed.shadowsR = keyframe.shadowsR;
                displayed.shadowsG = keyframe.shadowsG;
                displayed.shadowsB = keyframe.shadowsB;
                displayed.midtonesR = keyframe.midtonesR;
                displayed.midtonesG = keyframe.midtonesG;
                displayed.midtonesB = keyframe.midtonesB;
                displayed.highlightsR = keyframe.highlightsR;
                displayed.highlightsG = keyframe.highlightsG;
                displayed.highlightsB = keyframe.highlightsB;
                displayed.linearInterpolation = keyframe.linearInterpolation;
                break;
            }
        }
        updateSpinBoxesFromKeyframe(displayed);
    }

    if (m_deps.onKeyframeSelectionChanged) {
        m_deps.onKeyframeSelectionChanged();
    }
}

void GradingTab::onTableItemChanged(QTableWidgetItem* changedItem)
{
    if (m_updating || !changedItem) {
        if (m_deps.onKeyframeItemChanged && changedItem) {
            m_deps.onKeyframeItemChanged(changedItem);
        }
        return;
    }

    const TimelineClip* selectedClip = m_deps.getSelectedClip();
    if (!selectedClip || !m_deps.clipHasVisuals(*selectedClip)) return;

    const int row = changedItem->row();
    if (row < 0 || row >= m_widgets.gradingKeyframeTable->rowCount()) return;

    auto tableText = [this, row](int column) -> QString {
        QTableWidgetItem* item = m_widgets.gradingKeyframeTable->item(row, column);
        return item ? item->text().trimmed() : QString();
    };

    bool ok = false;
    TimelineClip::GradingKeyframe edited;
    edited.frame = tableText(0).toLongLong(&ok);
    if (!ok) { refresh(); return; }
    edited.brightness = tableText(1).toDouble(&ok);
    if (!ok) { refresh(); return; }
    edited.contrast = tableText(2).toDouble(&ok);
    if (!ok) { refresh(); return; }
    edited.saturation = tableText(3).toDouble(&ok);
    if (!ok) { refresh(); return; }
    edited.opacity = tableText(4).toDouble(&ok);
    if (!ok) { refresh(); return; }
    if (!parseVideoInterpolationText(tableText(5), &edited.linearInterpolation)) {
        refresh();
        return;
    }

    edited.frame = qBound<int64_t>(0, edited.frame, qMax<int64_t>(0, selectedClip->durationFrames - 1));
    const int64_t originalFrame = changedItem->data(Qt::UserRole).toLongLong();

    const bool updated = m_deps.updateClipById(selectedClip->id, [edited, originalFrame](TimelineClip& clip) {
        TimelineClip::GradingKeyframe originalKeyframe;
        bool foundOriginal = false;
        for (const TimelineClip::GradingKeyframe& existing : clip.gradingKeyframes) {
            if (existing.frame == originalFrame) {
                originalKeyframe = existing;
                foundOriginal = true;
                break;
            }
        }

        bool replaced = false;
        for (TimelineClip::GradingKeyframe& existing : clip.gradingKeyframes) {
            if (existing.frame == originalFrame) {
                existing = edited;
                replaced = true;
                break;
            }
        }
        if (!replaced) {
            clip.gradingKeyframes.push_back(edited);
        }

        // Grading always maintains a frame-0 base state. If the edited keyframe
        // was the frame-0 key and the user moves it later in time, preserve the
        // original grade as the new base key instead of silently snapping back.
        if (foundOriginal && originalFrame == 0 && edited.frame > 0) {
            bool hasBaseAtZero = false;
            for (const TimelineClip::GradingKeyframe& existing : clip.gradingKeyframes) {
                if (existing.frame == 0 && !(existing.frame == edited.frame &&
                                             existing.brightness == edited.brightness &&
                                             existing.contrast == edited.contrast &&
                                             existing.saturation == edited.saturation &&
                                             existing.opacity == edited.opacity &&
                                             existing.linearInterpolation == edited.linearInterpolation)) {
                    hasBaseAtZero = true;
                    break;
                }
            }
            if (!hasBaseAtZero) {
                originalKeyframe.frame = 0;
                clip.gradingKeyframes.push_back(originalKeyframe);
            }
        }

        normalizeClipGradingKeyframes(clip);
    });

    if (!updated) {
        refresh();
        return;
    }

    m_selectedKeyframeFrame = edited.frame;
    m_selectedKeyframeFrames = {edited.frame};
    m_deps.setPreviewTimelineClips();
    if (m_deps.onKeyframeItemChanged) {
        m_deps.onKeyframeItemChanged(changedItem);
    }
    refresh();
    m_deps.scheduleSaveState();
    m_deps.pushHistorySnapshot();
}

void GradingTab::onTableItemClicked(QTableWidgetItem* item)
{
    if (m_updating || !item) return;
    if (item->column() != 5) return;
    item->setText(nextVideoInterpolationLabel(item->text()));
}

QString GradingTab::videoInterpolationLabel(bool linearInterpolation) const
{
    return linearInterpolation ? QStringLiteral("Linear") : QStringLiteral("Step");
}

QString GradingTab::nextVideoInterpolationLabel(const QString& text) const
{
    bool linearInterpolation = true;
    if (!parseVideoInterpolationText(text, &linearInterpolation)) {
        linearInterpolation = true;
    }
    return videoInterpolationLabel(!linearInterpolation);
}

bool GradingTab::parseVideoInterpolationText(const QString& text, bool* linearInterpolationOut) const
{
    const QString normalized = text.trimmed().toLower();
    if (normalized.isEmpty() || normalized == QStringLiteral("step")) {
        *linearInterpolationOut = false;
        return true;
    }
    if (normalized == QStringLiteral("linear") || normalized == QStringLiteral("smooth")) {
        *linearInterpolationOut = true;
        return true;
    }
    return false;
}

int GradingTab::selectedKeyframeIndex(const TimelineClip& clip) const
{
    for (int i = 0; i < clip.gradingKeyframes.size(); ++i) {
        if (clip.gradingKeyframes[i].frame == m_selectedKeyframeFrame) {
            return i;
        }
    }
    return -1;
}

QList<int64_t> GradingTab::selectedKeyframeFramesForClip(const TimelineClip& clip) const
{
    QList<int64_t> frames;
    for (const TimelineClip::GradingKeyframe& keyframe : clip.gradingKeyframes) {
        if (m_selectedKeyframeFrames.contains(keyframe.frame)) {
            frames.push_back(keyframe.frame);
        }
    }
    return frames;
}

int GradingTab::nearestKeyframeIndex(const TimelineClip& clip) const
{
    if (!m_deps.getSelectedClip() || clip.gradingKeyframes.isEmpty()) {
        return -1;
    }
    const int64_t localFrame = qBound<int64_t>(0,
                                               m_deps.getCurrentTimelineFrame() - clip.startFrame,
                                               qMax<int64_t>(0, clip.durationFrames - 1));
    int nearestIndex = 0;
    int64_t nearestDistance = std::numeric_limits<int64_t>::max();
    for (int i = 0; i < clip.gradingKeyframes.size(); ++i) {
        const int64_t distance = std::abs(clip.gradingKeyframes[i].frame - localFrame);
        if (distance < nearestDistance) {
            nearestDistance = distance;
            nearestIndex = i;
        }
    }
    return nearestIndex;
}

bool GradingTab::hasRemovableKeyframeSelection(const TimelineClip& clip) const
{
    for (const int64_t frame : selectedKeyframeFramesForClip(clip)) {
        if (frame > 0) {
            return true;
        }
    }
    return false;
}

GradingTab::GradingKeyframeDisplay GradingTab::evaluateDisplayedGrading(const TimelineClip& clip, int64_t localFrame) const
{
    GradingKeyframeDisplay result;
    result.brightness = 0.0;
    result.contrast = 1.0;
    result.saturation = 1.0;
    result.opacity = 1.0;
    result.shadowsR = 0.0; result.shadowsG = 0.0; result.shadowsB = 0.0;
    result.midtonesR = 0.0; result.midtonesG = 0.0; result.midtonesB = 0.0;
    result.highlightsR = 0.0; result.highlightsG = 0.0; result.highlightsB = 0.0;
    result.linearInterpolation = true;

    if (clip.gradingKeyframes.isEmpty()) {
        return result;
    }

    // Find the keyframe at or before localFrame
    int beforeIndex = -1;
    for (int i = clip.gradingKeyframes.size() - 1; i >= 0; --i) {
        if (clip.gradingKeyframes[i].frame <= localFrame) {
            beforeIndex = i;
            break;
        }
    }

    if (beforeIndex < 0) {
        // Use first keyframe
        const auto& kf = clip.gradingKeyframes[0];
        result.brightness = kf.brightness;
        result.contrast = kf.contrast;
        result.saturation = kf.saturation;
        result.opacity = kf.opacity;
        result.shadowsR = kf.shadowsR; result.shadowsG = kf.shadowsG; result.shadowsB = kf.shadowsB;
        result.midtonesR = kf.midtonesR; result.midtonesG = kf.midtonesG; result.midtonesB = kf.midtonesB;
        result.highlightsR = kf.highlightsR; result.highlightsG = kf.highlightsG; result.highlightsB = kf.highlightsB;
        result.linearInterpolation = kf.linearInterpolation;
        return result;
    }

    const auto& before = clip.gradingKeyframes[beforeIndex];
    
    // If this is the last keyframe or we're exactly at it
    if (beforeIndex == clip.gradingKeyframes.size() - 1 || before.frame == localFrame) {
        result.brightness = before.brightness;
        result.contrast = before.contrast;
        result.saturation = before.saturation;
        result.opacity = before.opacity;
        result.shadowsR = before.shadowsR; result.shadowsG = before.shadowsG; result.shadowsB = before.shadowsB;
        result.midtonesR = before.midtonesR; result.midtonesG = before.midtonesG; result.midtonesB = before.midtonesB;
        result.highlightsR = before.highlightsR; result.highlightsG = before.highlightsG; result.highlightsB = before.highlightsB;
        result.linearInterpolation = before.linearInterpolation;
        return result;
    }

    // Find the next keyframe
    int afterIndex = beforeIndex + 1;
    const auto& after = clip.gradingKeyframes[afterIndex];

    // If not interpolating, just use the before keyframe
    if (!before.linearInterpolation) {
        result.brightness = before.brightness;
        result.contrast = before.contrast;
        result.saturation = before.saturation;
        result.opacity = before.opacity;
        result.shadowsR = before.shadowsR; result.shadowsG = before.shadowsG; result.shadowsB = before.shadowsB;
        result.midtonesR = before.midtonesR; result.midtonesG = before.midtonesG; result.midtonesB = before.midtonesB;
        result.highlightsR = before.highlightsR; result.highlightsG = before.highlightsG; result.highlightsB = before.highlightsB;
        result.linearInterpolation = before.linearInterpolation;
        return result;
    }

    // Interpolate
    const int64_t range = after.frame - before.frame;
    if (range <= 0) {
        result.brightness = before.brightness;
        result.contrast = before.contrast;
        result.saturation = before.saturation;
        result.opacity = before.opacity;
        result.shadowsR = before.shadowsR; result.shadowsG = before.shadowsG; result.shadowsB = before.shadowsB;
        result.midtonesR = before.midtonesR; result.midtonesG = before.midtonesG; result.midtonesB = before.midtonesB;
        result.highlightsR = before.highlightsR; result.highlightsG = before.highlightsG; result.highlightsB = before.highlightsB;
        return result;
    }

    const double t = static_cast<double>(localFrame - before.frame) / static_cast<double>(range);
    result.brightness = before.brightness + (after.brightness - before.brightness) * t;
    result.contrast = before.contrast + (after.contrast - before.contrast) * t;
    result.saturation = before.saturation + (after.saturation - before.saturation) * t;
    result.opacity = before.opacity + (after.opacity - before.opacity) * t;
    result.shadowsR = before.shadowsR + (after.shadowsR - before.shadowsR) * t;
    result.shadowsG = before.shadowsG + (after.shadowsG - before.shadowsG) * t;
    result.shadowsB = before.shadowsB + (after.shadowsB - before.shadowsB) * t;
    result.midtonesR = before.midtonesR + (after.midtonesR - before.midtonesR) * t;
    result.midtonesG = before.midtonesG + (after.midtonesG - before.midtonesG) * t;
    result.midtonesB = before.midtonesB + (after.midtonesB - before.midtonesB) * t;
    result.highlightsR = before.highlightsR + (after.highlightsR - before.highlightsR) * t;
    result.highlightsG = before.highlightsG + (after.highlightsG - before.highlightsG) * t;
    result.highlightsB = before.highlightsB + (after.highlightsB - before.highlightsB) * t;
    result.linearInterpolation = after.linearInterpolation;

    return result;
}

void GradingTab::updateSpinBoxesFromKeyframe(const GradingKeyframeDisplay& keyframe)
{
    QSignalBlocker brightnessBlock(m_widgets.brightnessSpin);
    QSignalBlocker contrastBlock(m_widgets.contrastSpin);
    QSignalBlocker saturationBlock(m_widgets.saturationSpin);
    QSignalBlocker opacityBlock(m_widgets.opacitySpin);
    QSignalBlocker shadowsRBlock(m_widgets.shadowsRSpin);
    QSignalBlocker shadowsGBlock(m_widgets.shadowsGSpin);
    QSignalBlocker shadowsBBlock(m_widgets.shadowsBSpin);
    QSignalBlocker midtonesRBlock(m_widgets.midtonesRSpin);
    QSignalBlocker midtonesGBlock(m_widgets.midtonesGSpin);
    QSignalBlocker midtonesBBlock(m_widgets.midtonesBSpin);
    QSignalBlocker highlightsRBlock(m_widgets.highlightsRSpin);
    QSignalBlocker highlightsGBlock(m_widgets.highlightsGSpin);
    QSignalBlocker highlightsBBlock(m_widgets.highlightsBSpin);

    m_widgets.brightnessSpin->setValue(keyframe.brightness);
    m_widgets.contrastSpin->setValue(keyframe.contrast);
    m_widgets.saturationSpin->setValue(keyframe.saturation);
    m_widgets.opacitySpin->setValue(keyframe.opacity);
    
    if (m_widgets.shadowsRSpin) m_widgets.shadowsRSpin->setValue(keyframe.shadowsR);
    if (m_widgets.shadowsGSpin) m_widgets.shadowsGSpin->setValue(keyframe.shadowsG);
    if (m_widgets.shadowsBSpin) m_widgets.shadowsBSpin->setValue(keyframe.shadowsB);
    if (m_widgets.midtonesRSpin) m_widgets.midtonesRSpin->setValue(keyframe.midtonesR);
    if (m_widgets.midtonesGSpin) m_widgets.midtonesGSpin->setValue(keyframe.midtonesG);
    if (m_widgets.midtonesBSpin) m_widgets.midtonesBSpin->setValue(keyframe.midtonesB);
    if (m_widgets.highlightsRSpin) m_widgets.highlightsRSpin->setValue(keyframe.highlightsR);
    if (m_widgets.highlightsGSpin) m_widgets.highlightsGSpin->setValue(keyframe.highlightsG);
    if (m_widgets.highlightsBSpin) m_widgets.highlightsBSpin->setValue(keyframe.highlightsB);
}

void GradingTab::populateTable(const TimelineClip& clip)
{
    QList<int64_t> frames;
    if (clip.gradingKeyframes.isEmpty()) {
        frames.push_back(0);
    } else {
        frames.reserve(clip.gradingKeyframes.size());
        for (const TimelineClip::GradingKeyframe& keyframe : clip.gradingKeyframes) {
            frames.push_back(keyframe.frame);
        }
        std::sort(frames.begin(), frames.end());
    }

    m_widgets.gradingKeyframeTable->setRowCount(frames.size());
    
    for (int row = 0; row < frames.size(); ++row) {
        const int64_t frame = frames[row];
        TimelineClip::GradingKeyframe displayedFrame;
        
        if (clip.gradingKeyframes.isEmpty()) {
            displayedFrame = evaluateClipGradingAtFrame(clip, clip.startFrame);
            displayedFrame.frame = 0;
        } else {
            for (const TimelineClip::GradingKeyframe& keyframe : clip.gradingKeyframes) {
                if (keyframe.frame == frame) {
                    displayedFrame = keyframe;
                    break;
                }
            }
        }

        const QStringList rowValues = {
            QString::number(frame),
            QString::number(displayedFrame.brightness, 'f', 3),
            QString::number(displayedFrame.contrast, 'f', 3),
            QString::number(displayedFrame.saturation, 'f', 3),
            QString::number(displayedFrame.opacity, 'f', 3),
            videoInterpolationLabel(displayedFrame.linearInterpolation)
        };

        for (int column = 0; column < rowValues.size(); ++column) {
            auto* item = new QTableWidgetItem(rowValues[column]);
            item->setData(Qt::UserRole, QVariant::fromValue(static_cast<qint64>(frame)));
            m_widgets.gradingKeyframeTable->setItem(row, column, item);
        }
    }
}

void GradingTab::applyOpacityFadeFromPlayhead(bool fadeIn)
{
    const TimelineClip* clip = m_deps.getSelectedClip();
    if (!clip || !m_deps.clipHasVisuals(*clip)) {
        return;
    }

    const int64_t localStartFrame = qBound<int64_t>(0,
                                                    m_deps.getCurrentTimelineFrame() - clip->startFrame,
                                                    qMax<int64_t>(0, clip->durationFrames - 1));
    
    // Get fade duration in seconds and convert to frames
    double fadeDurationSeconds = 1.0;
    if (m_widgets.gradingFadeDurationSpin) {
        fadeDurationSeconds = m_widgets.gradingFadeDurationSpin->value();
    }
    const double fps = 30.0; // Assume 30fps, could be made configurable
    const int64_t fadeDurationFrames = static_cast<int64_t>(fadeDurationSeconds * fps);
    
    const int64_t localEndFrame = qMin<int64_t>(
        qMax<int64_t>(0, clip->durationFrames - 1),
        localStartFrame + fadeDurationFrames);
    
    if (localStartFrame >= localEndFrame) {
        QMessageBox::information(nullptr,
                                 QStringLiteral("Opacity Fade"),
                                 QStringLiteral("Move the playhead before the end of the clip to create a fade."));
        return;
    }

    const GradingKeyframeDisplay startDisplay = evaluateDisplayedGrading(*clip, clip->startFrame + localStartFrame);
    const GradingKeyframeDisplay endDisplay = evaluateDisplayedGrading(*clip, clip->startFrame + localEndFrame);
    // For fade in, always end at opacity=1.0 (full visibility)
    // For fade out, end at opacity=0.0
    const double targetVisibleOpacity = 1.0;

    const bool updated = m_deps.updateClipById(clip->id, [&](TimelineClip& updatedClip) {
        auto upsertFrame = [](QVector<TimelineClip::GradingKeyframe>& keyframes,
                              const TimelineClip::GradingKeyframe& keyframe) {
            for (TimelineClip::GradingKeyframe& existing : keyframes) {
                if (existing.frame == keyframe.frame) {
                    existing = keyframe;
                    return;
                }
            }
            keyframes.push_back(keyframe);
        };

        TimelineClip::GradingKeyframe startKeyframe;
        startKeyframe.frame = localStartFrame;
        startKeyframe.brightness = startDisplay.brightness;
        startKeyframe.contrast = startDisplay.contrast;
        startKeyframe.saturation = startDisplay.saturation;
        startKeyframe.opacity = fadeIn ? 0.0 : qBound(0.0, startDisplay.opacity, 1.0);
        startKeyframe.shadowsR = startDisplay.shadowsR; startKeyframe.shadowsG = startDisplay.shadowsG; startKeyframe.shadowsB = startDisplay.shadowsB;
        startKeyframe.midtonesR = startDisplay.midtonesR; startKeyframe.midtonesG = startDisplay.midtonesG; startKeyframe.midtonesB = startDisplay.midtonesB;
        startKeyframe.highlightsR = startDisplay.highlightsR; startKeyframe.highlightsG = startDisplay.highlightsG; startKeyframe.highlightsB = startDisplay.highlightsB;
        startKeyframe.linearInterpolation = true;

        TimelineClip::GradingKeyframe endKeyframe;
        endKeyframe.frame = localEndFrame;
        endKeyframe.brightness = endDisplay.brightness;
        endKeyframe.contrast = endDisplay.contrast;
        endKeyframe.saturation = endDisplay.saturation;
        endKeyframe.opacity = fadeIn ? targetVisibleOpacity : 0.0;
        endKeyframe.shadowsR = endDisplay.shadowsR; endKeyframe.shadowsG = endDisplay.shadowsG; endKeyframe.shadowsB = endDisplay.shadowsB;
        endKeyframe.midtonesR = endDisplay.midtonesR; endKeyframe.midtonesG = endDisplay.midtonesG; endKeyframe.midtonesB = endDisplay.midtonesB;
        endKeyframe.highlightsR = endDisplay.highlightsR; endKeyframe.highlightsG = endDisplay.highlightsG; endKeyframe.highlightsB = endDisplay.highlightsB;
        endKeyframe.linearInterpolation = true;

        upsertFrame(updatedClip.gradingKeyframes, startKeyframe);
        upsertFrame(updatedClip.gradingKeyframes, endKeyframe);
        normalizeClipGradingKeyframes(updatedClip);
    });

    if (!updated) {
        return;
    }

    m_selectedKeyframeFrame = localStartFrame;
    m_selectedKeyframeFrames = {localStartFrame, localEndFrame};
    m_deps.setPreviewTimelineClips();
    m_deps.refreshInspector();
    m_deps.scheduleSaveState();
    m_deps.pushHistorySnapshot();
}

void GradingTab::removeSelectedKeyframes()
{
    if (m_selectedKeyframeFrames.isEmpty()) return;

    const QString clipId = m_deps.getSelectedClipId();
    if (clipId.isEmpty()) return;

    const TimelineClip* selectedClip = m_deps.getSelectedClip();
    if (!selectedClip) return;

    QList<int64_t> selectedFrames = selectedKeyframeFramesFromTable(m_widgets.gradingKeyframeTable);
    selectedFrames.erase(std::remove_if(selectedFrames.begin(),
                                        selectedFrames.end(),
                                        [](int64_t frame) { return frame <= 0; }),
                         selectedFrames.end());

    if (selectedFrames.isEmpty()) {
        refresh();
        return;
    }

    const bool updated = m_deps.updateClipById(clipId, [selectedFrames](TimelineClip& clip) {
        clip.gradingKeyframes.erase(
            std::remove_if(clip.gradingKeyframes.begin(),
                           clip.gradingKeyframes.end(),
                           [&selectedFrames](const TimelineClip::GradingKeyframe& keyframe) {
                               return selectedFrames.contains(keyframe.frame);
                           }),
            clip.gradingKeyframes.end());
        normalizeClipGradingKeyframes(clip);
    });

    if (!updated) return;

    m_selectedKeyframeFrame = 0;
    m_selectedKeyframeFrames = {0};
    m_deps.setPreviewTimelineClips();
    refresh();
    m_deps.scheduleSaveState();
    m_deps.pushHistorySnapshot();
    emit keyframesRemoved();
}

void GradingTab::onBrightnessEditingFinished()
{
    if (m_updating) return;
    applyGradeFromInspector(true);
}

void GradingTab::onContrastEditingFinished()
{
    if (m_updating) return;
    applyGradeFromInspector(true);
}

void GradingTab::onSaturationEditingFinished()
{
    if (m_updating) return;
    applyGradeFromInspector(true);
}

void GradingTab::onOpacityEditingFinished()
{
    if (m_updating) return;
    applyGradeFromInspector(true);
}

void GradingTab::onTableCustomContextMenu(const QPoint& pos)
{
    if (!m_widgets.gradingKeyframeTable) return;

    int row = -1;
    QTableWidgetItem* item =
        editor::ensureContextRowSelected(m_widgets.gradingKeyframeTable, pos, &row);
    if (!item) return;

    const int64_t anchorFrame = item->data(Qt::UserRole).toLongLong();
    const int64_t previousFrame = editor::rowFrameRole(m_widgets.gradingKeyframeTable, row - 1);
    const int64_t nextFrame = editor::rowFrameRole(m_widgets.gradingKeyframeTable, row + 1);
    
    QMenu menu;
    ContextMenuActions actions = buildStandardContextMenu(menu, m_widgets.gradingKeyframeTable, row, m_deps.getSelectedClip());
    // Grading tab doesn't use copy actions, so we ignore those

    QAction* chosen = menu.exec(m_widgets.gradingKeyframeTable->viewport()->mapToGlobal(pos));
    if (chosen == actions.addAbove && actions.addAbove->isEnabled()) {
        const int64_t midpointFrame = previousFrame + ((anchorFrame - previousFrame) / 2);
        if (midpointFrame > previousFrame && midpointFrame < anchorFrame) {
            const auto findKeyframeAt = [](const TimelineClip& clip, int64_t frame) -> const TimelineClip::GradingKeyframe* {
                for (const TimelineClip::GradingKeyframe& keyframe : clip.gradingKeyframes) {
                    if (keyframe.frame == frame) return &keyframe;
                }
                return nullptr;
            };
            const TimelineClip* selectedClip = m_deps.getSelectedClip();
            if (selectedClip) {
                const TimelineClip::GradingKeyframe* earlier = findKeyframeAt(*selectedClip, previousFrame);
                const TimelineClip::GradingKeyframe* later = findKeyframeAt(*selectedClip, anchorFrame);
                if (earlier && later) {
                    const double t = static_cast<double>(midpointFrame - previousFrame) / static_cast<double>(anchorFrame - previousFrame);
                    TimelineClip::GradingKeyframe midpoint;
                    midpoint.frame = midpointFrame;
                    midpoint.brightness = earlier->brightness + ((later->brightness - earlier->brightness) * t);
                    midpoint.contrast = earlier->contrast + ((later->contrast - earlier->contrast) * t);
                    midpoint.saturation = earlier->saturation + ((later->saturation - earlier->saturation) * t);
                    midpoint.opacity = earlier->opacity + ((later->opacity - earlier->opacity) * t);
                    midpoint.shadowsR = earlier->shadowsR + ((later->shadowsR - earlier->shadowsR) * t);
                    midpoint.shadowsG = earlier->shadowsG + ((later->shadowsG - earlier->shadowsG) * t);
                    midpoint.shadowsB = earlier->shadowsB + ((later->shadowsB - earlier->shadowsB) * t);
                    midpoint.midtonesR = earlier->midtonesR + ((later->midtonesR - earlier->midtonesR) * t);
                    midpoint.midtonesG = earlier->midtonesG + ((later->midtonesG - earlier->midtonesG) * t);
                    midpoint.midtonesB = earlier->midtonesB + ((later->midtonesB - earlier->midtonesB) * t);
                    midpoint.highlightsR = earlier->highlightsR + ((later->highlightsR - earlier->highlightsR) * t);
                    midpoint.highlightsG = earlier->highlightsG + ((later->highlightsG - earlier->highlightsG) * t);
                    midpoint.highlightsB = earlier->highlightsB + ((later->highlightsB - earlier->highlightsB) * t);
                    midpoint.linearInterpolation = later->linearInterpolation;
                    const bool updated = m_deps.updateClipById(selectedClip->id, [midpoint](TimelineClip& clip) {
                        for (TimelineClip::GradingKeyframe& existing : clip.gradingKeyframes) {
                            if (existing.frame == midpoint.frame) {
                                existing = midpoint;
                                normalizeClipGradingKeyframes(clip);
                                return;
                            }
                        }
                        clip.gradingKeyframes.push_back(midpoint);
                        normalizeClipGradingKeyframes(clip);
                    });
                    if (updated) {
                        m_selectedKeyframeFrame = midpoint.frame;
                        m_selectedKeyframeFrames = {midpoint.frame};
                        m_deps.setPreviewTimelineClips();
                        refresh();
                        m_deps.scheduleSaveState();
                        m_deps.pushHistorySnapshot();
                        m_deps.seekToTimelineFrame(selectedClip->startFrame + midpoint.frame);
                    }
                }
            }
        }
    } else if (chosen == actions.addBelow && actions.addBelow->isEnabled()) {
        const int64_t midpointFrame = anchorFrame + ((nextFrame - anchorFrame) / 2);
        if (midpointFrame > anchorFrame && midpointFrame < nextFrame) {
            const auto findKeyframeAt = [](const TimelineClip& clip, int64_t frame) -> const TimelineClip::GradingKeyframe* {
                for (const TimelineClip::GradingKeyframe& keyframe : clip.gradingKeyframes) {
                    if (keyframe.frame == frame) return &keyframe;
                }
                return nullptr;
            };
            const TimelineClip* selectedClip = m_deps.getSelectedClip();
            if (selectedClip) {
                const TimelineClip::GradingKeyframe* earlier = findKeyframeAt(*selectedClip, anchorFrame);
                const TimelineClip::GradingKeyframe* later = findKeyframeAt(*selectedClip, nextFrame);
                if (earlier && later) {
                    const double t = static_cast<double>(midpointFrame - anchorFrame) / static_cast<double>(nextFrame - anchorFrame);
                    TimelineClip::GradingKeyframe midpoint;
                    midpoint.frame = midpointFrame;
                    midpoint.brightness = earlier->brightness + ((later->brightness - earlier->brightness) * t);
                    midpoint.contrast = earlier->contrast + ((later->contrast - earlier->contrast) * t);
                    midpoint.saturation = earlier->saturation + ((later->saturation - earlier->saturation) * t);
                    midpoint.opacity = earlier->opacity + ((later->opacity - earlier->opacity) * t);
                    midpoint.shadowsR = earlier->shadowsR + ((later->shadowsR - earlier->shadowsR) * t);
                    midpoint.shadowsG = earlier->shadowsG + ((later->shadowsG - earlier->shadowsG) * t);
                    midpoint.shadowsB = earlier->shadowsB + ((later->shadowsB - earlier->shadowsB) * t);
                    midpoint.midtonesR = earlier->midtonesR + ((later->midtonesR - earlier->midtonesR) * t);
                    midpoint.midtonesG = earlier->midtonesG + ((later->midtonesG - earlier->midtonesG) * t);
                    midpoint.midtonesB = earlier->midtonesB + ((later->midtonesB - earlier->midtonesB) * t);
                    midpoint.highlightsR = earlier->highlightsR + ((later->highlightsR - earlier->highlightsR) * t);
                    midpoint.highlightsG = earlier->highlightsG + ((later->highlightsG - earlier->highlightsG) * t);
                    midpoint.highlightsB = earlier->highlightsB + ((later->highlightsB - earlier->highlightsB) * t);
                    midpoint.linearInterpolation = later->linearInterpolation;
                    const bool updated = m_deps.updateClipById(selectedClip->id, [midpoint](TimelineClip& clip) {
                        for (TimelineClip::GradingKeyframe& existing : clip.gradingKeyframes) {
                            if (existing.frame == midpoint.frame) {
                                existing = midpoint;
                                normalizeClipGradingKeyframes(clip);
                                return;
                            }
                        }
                        clip.gradingKeyframes.push_back(midpoint);
                        normalizeClipGradingKeyframes(clip);
                    });
                    if (updated) {
                        m_selectedKeyframeFrame = midpoint.frame;
                        m_selectedKeyframeFrames = {midpoint.frame};
                        m_deps.setPreviewTimelineClips();
                        refresh();
                        m_deps.scheduleSaveState();
                        m_deps.pushHistorySnapshot();
                        m_deps.seekToTimelineFrame(selectedClip->startFrame + midpoint.frame);
                    }
                }
            }
        }
    } else if (chosen == actions.deleteRows && actions.deleteRows->isEnabled()) {
        removeSelectedKeyframes();
    }
}
