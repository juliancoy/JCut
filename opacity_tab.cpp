#include "opacity_tab.h"

#include "editor_shared.h"
#include "keyframe_table_shared.h"

#include <QDir>
#include <QHeaderView>
#include <QMenu>
#include <QMessageBox>
#include <QSignalBlocker>

OpacityTab::OpacityTab(const Widgets& widgets, const Dependencies& deps, QObject* parent)
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

void OpacityTab::wire()
{
    if (m_widgets.opacitySpin) {
        connect(m_widgets.opacitySpin, qOverload<double>(&QDoubleSpinBox::valueChanged),
                this, &OpacityTab::onOpacityChanged);
        connect(m_widgets.opacitySpin, &QDoubleSpinBox::editingFinished,
                this, &OpacityTab::onOpacityEditingFinished);
    }
    if (m_widgets.opacityAutoScrollCheckBox) {
        connect(m_widgets.opacityAutoScrollCheckBox, &QCheckBox::toggled,
                this, &OpacityTab::onAutoScrollToggled);
    }
    if (m_widgets.opacityFollowCurrentCheckBox) {
        connect(m_widgets.opacityFollowCurrentCheckBox, &QCheckBox::toggled,
                this, &OpacityTab::onFollowCurrentToggled);
    }
    if (m_widgets.opacityKeyAtPlayheadButton) {
        connect(m_widgets.opacityKeyAtPlayheadButton, &QPushButton::clicked,
                this, &OpacityTab::onKeyAtPlayheadClicked);
    }
    if (m_widgets.opacityFadeInButton) {
        connect(m_widgets.opacityFadeInButton, &QPushButton::clicked,
                this, &OpacityTab::onFadeInClicked);
    }
    if (m_widgets.opacityFadeOutButton) {
        connect(m_widgets.opacityFadeOutButton, &QPushButton::clicked,
                this, &OpacityTab::onFadeOutClicked);
    }
    if (m_widgets.opacityKeyframeTable) {
        connect(m_widgets.opacityKeyframeTable, &QTableWidget::itemSelectionChanged,
                this, &OpacityTab::onTableSelectionChanged);
        connect(m_widgets.opacityKeyframeTable, &QTableWidget::itemChanged,
                this, &OpacityTab::onTableItemChanged);
        connect(m_widgets.opacityKeyframeTable, &QTableWidget::itemClicked,
                this, &OpacityTab::onTableItemClicked);
        m_widgets.opacityKeyframeTable->setContextMenuPolicy(Qt::CustomContextMenu);
        connect(m_widgets.opacityKeyframeTable, &QWidget::customContextMenuRequested,
                this, &OpacityTab::onTableCustomContextMenu);
        installTableHandlers(m_widgets.opacityKeyframeTable);
    }
}

void OpacityTab::refresh()
{
    if (!m_widgets.opacityPathLabel || !m_widgets.opacitySpin || !m_widgets.opacityKeyframeTable) {
        return;
    }

    const TimelineClip* clip = m_deps.getSelectedClip();
    m_updating = true;

    QSignalBlocker opacityBlock(m_widgets.opacitySpin);
    QSignalBlocker tableBlocker(m_widgets.opacityKeyframeTable);
    m_widgets.opacityKeyframeTable->clearContents();
    m_widgets.opacityKeyframeTable->setRowCount(0);

    if (!clip || !m_deps.clipHasVisuals(*clip)) {
        m_widgets.opacityPathLabel->setText(QStringLiteral("No visual clip selected"));
        m_widgets.opacityPathLabel->setToolTip(QString());
        m_widgets.opacitySpin->setValue(1.0);
        m_selectedKeyframeFrame = -1;
        m_selectedKeyframeFrames.clear();
        m_updating = false;
        return;
    }

    const QString nativePath = QDir::toNativeSeparators(m_deps.getClipFilePath(*clip));
    const QString sourceLabel = QStringLiteral("%1 | %2")
                                    .arg(clipMediaTypeLabel(clip->mediaType),
                                         mediaSourceKindLabel(clip->sourceKind));
    m_widgets.opacityPathLabel->setText(QStringLiteral("%1\n%2").arg(clip->label, sourceLabel));
    m_widgets.opacityPathLabel->setToolTip(nativePath);

    populateTable(*clip);

    if (m_selectedKeyframeFrame < 0) {
        const int selectedIndex = clip->opacityKeyframes.isEmpty() ? 0 : nearestKeyframeIndex(*clip);
        m_selectedKeyframeFrame = selectedIndex >= 0 && selectedIndex < clip->opacityKeyframes.size()
            ? clip->opacityKeyframes[selectedIndex].frame
            : 0;
        m_selectedKeyframeFrames = {m_selectedKeyframeFrame};
    } else if (m_selectedKeyframeFrames.isEmpty()) {
        m_selectedKeyframeFrames = {m_selectedKeyframeFrame};
    }

    m_suppressSyncForTimelineFrame = -1;

    OpacityKeyframeDisplay displayed = evaluateDisplayedOpacity(*clip, clip->startFrame);
    const int selectedIndex = selectedKeyframeIndex(*clip);
    if (selectedIndex >= 0) {
        displayed.frame = clip->opacityKeyframes[selectedIndex].frame;
        displayed.opacity = clip->opacityKeyframes[selectedIndex].opacity;
        displayed.linearInterpolation = clip->opacityKeyframes[selectedIndex].linearInterpolation;
    }
    updateSpinBoxesFromKeyframe(displayed);

    editor::restoreSelectionByFrameRole(m_widgets.opacityKeyframeTable, m_selectedKeyframeFrames);
    m_updating = false;
    syncTableToPlayhead();
}

void OpacityTab::applyOpacityFromInspector(bool pushHistory)
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
        TimelineClip::OpacityKeyframe keyframe;
        keyframe.frame = targetFrame;
        keyframe.opacity = m_widgets.opacitySpin->value();
        keyframe.linearInterpolation = true;

        bool replaced = false;
        for (TimelineClip::OpacityKeyframe& existing : clip.opacityKeyframes) {
            if (existing.frame == targetFrame) {
                keyframe.linearInterpolation = existing.linearInterpolation;
                existing = keyframe;
                replaced = true;
                break;
            }
        }
        if (!replaced) {
            for (const TimelineClip::OpacityKeyframe& existing : clip.opacityKeyframes) {
                if (existing.frame > targetFrame) {
                    keyframe.linearInterpolation = existing.linearInterpolation;
                    break;
                }
            }
            clip.opacityKeyframes.push_back(keyframe);
        }
        normalizeClipOpacityKeyframes(clip);
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
}

void OpacityTab::upsertKeyframeAtPlayhead()
{
    const TimelineClip* clip = m_deps.getSelectedClip();
    if (!clip || !m_deps.clipHasVisuals(*clip)) return;
    m_selectedKeyframeFrame = qBound<int64_t>(0,
                                              m_deps.getCurrentTimelineFrame() - clip->startFrame,
                                              qMax<int64_t>(0, clip->durationFrames - 1));
    m_selectedKeyframeFrames = {m_selectedKeyframeFrame};
    applyOpacityFromInspector(true);
}

void OpacityTab::fadeInFromPlayhead() { applyOpacityFadeFromPlayhead(true); }
void OpacityTab::fadeOutFromPlayhead() { applyOpacityFadeFromPlayhead(false); }

void OpacityTab::syncTableToPlayhead()
{
    if (shouldSkipSyncToPlayhead(m_widgets.opacityKeyframeTable, m_widgets.opacityFollowCurrentCheckBox)) {
        return;
    }
    const TimelineClip* clip = m_deps.getSelectedClip();
    if (!clip || !m_deps.clipHasVisuals(*clip) || m_widgets.opacityKeyframeTable->rowCount() <= 0) {
        m_widgets.opacityKeyframeTable->clearSelection();
        return;
    }
    const int64_t localFrame = calculateLocalFrame(clip);
    int matchingRow = -1;
    int64_t matchingFrame = -1;
    for (int row = 0; row < m_widgets.opacityKeyframeTable->rowCount(); ++row) {
        QTableWidgetItem* item = m_widgets.opacityKeyframeTable->item(row, 0);
        if (!item) continue;
        const int64_t frame = item->data(Qt::UserRole).toLongLong();
        if (frame <= localFrame && frame >= matchingFrame) {
            matchingFrame = frame;
            matchingRow = row;
        }
    }
    if (matchingRow < 0) matchingRow = 0;
    applySyncedRowSelection(m_widgets.opacityKeyframeTable,
                            matchingRow,
                            m_widgets.opacityAutoScrollCheckBox &&
                                m_widgets.opacityAutoScrollCheckBox->isChecked());
}

void OpacityTab::onOpacityChanged(double) { if (!m_updating) applyOpacityFromInspector(false); }
void OpacityTab::onOpacityEditingFinished() { if (!m_updating) applyOpacityFromInspector(true); }
void OpacityTab::onAutoScrollToggled(bool) { if (!m_updating) m_deps.scheduleSaveState(); }
void OpacityTab::onFollowCurrentToggled(bool) { if (!m_updating) { syncTableToPlayhead(); m_deps.scheduleSaveState(); } }
void OpacityTab::onKeyAtPlayheadClicked() { upsertKeyframeAtPlayhead(); }
void OpacityTab::onFadeInClicked() { fadeInFromPlayhead(); }
void OpacityTab::onFadeOutClicked() { fadeOutFromPlayhead(); }

void OpacityTab::onTableSelectionChanged()
{
    if (m_updating || m_syncingTableSelection) return;
    onTableSelectionChangedBase(m_widgets.opacityKeyframeTable, &m_deferredSeekTimer, &m_pendingSeekTimelineFrame);
    const QSet<int64_t> selectedFrames = editor::collectSelectedFrameRoles(m_widgets.opacityKeyframeTable);
    const int64_t primaryFrame = editor::primarySelectedFrameRole(m_widgets.opacityKeyframeTable);
    if (primaryFrame < 0) {
        m_selectedKeyframeFrame = -1;
        m_selectedKeyframeFrames.clear();
        return;
    }
    m_selectedKeyframeFrame = primaryFrame;
    m_selectedKeyframeFrames = selectedFrames;
    const TimelineClip* clip = m_deps.getSelectedClip();
    if (clip) {
        m_suppressSyncForTimelineFrame = clip->startFrame + primaryFrame;
        updateSpinBoxesFromKeyframe(evaluateDisplayedOpacity(*clip, primaryFrame));
    }
    if (m_deps.onKeyframeSelectionChanged) {
        m_deps.onKeyframeSelectionChanged();
    }
}

void OpacityTab::onTableItemChanged(QTableWidgetItem* changedItem)
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
    if (row < 0 || row >= m_widgets.opacityKeyframeTable->rowCount()) return;
    auto tableText = [this, row](int column) -> QString {
        QTableWidgetItem* item = m_widgets.opacityKeyframeTable->item(row, column);
        return item ? item->text().trimmed() : QString();
    };

    bool ok = false;
    int64_t frame = tableText(0).toLongLong(&ok);
    if (!ok) { refresh(); return; }
    double opacity = tableText(1).toDouble(&ok);
    if (!ok) { refresh(); return; }
    bool linearInterpolation = true;
    if (!parseInterpolationText(tableText(2), &linearInterpolation)) {
        refresh();
        return;
    }

    frame = qBound<int64_t>(0, frame, qMax<int64_t>(0, selectedClip->durationFrames - 1));
    const int64_t originalFrame = changedItem->data(Qt::UserRole).toLongLong();
    const bool updated = m_deps.updateClipById(selectedClip->id, [frame, opacity, linearInterpolation, originalFrame](TimelineClip& clip) {
        TimelineClip::OpacityKeyframe keyframe;
        keyframe.frame = frame;
        keyframe.opacity = opacity;
        keyframe.linearInterpolation = linearInterpolation;
        bool replaced = false;
        for (TimelineClip::OpacityKeyframe& existing : clip.opacityKeyframes) {
            if (existing.frame == originalFrame) {
                existing = keyframe;
                replaced = true;
                break;
            }
        }
        if (!replaced) {
            clip.opacityKeyframes.push_back(keyframe);
        }
        normalizeClipOpacityKeyframes(clip);
    });
    if (!updated) {
        refresh();
        return;
    }
    m_selectedKeyframeFrame = frame;
    m_selectedKeyframeFrames = {frame};
    m_deps.setPreviewTimelineClips();
    if (m_deps.onKeyframeItemChanged) {
        m_deps.onKeyframeItemChanged(changedItem);
    }
    refresh();
    m_deps.scheduleSaveState();
    m_deps.pushHistorySnapshot();
}

void OpacityTab::onTableItemClicked(QTableWidgetItem* item)
{
    if (m_updating || !item || item->column() != 2) return;
    item->setText(nextInterpolationLabel(item->text()));
}

void OpacityTab::onTableCustomContextMenu(const QPoint& pos)
{
    if (!m_widgets.opacityKeyframeTable) return;
    int row = -1;
    QTableWidgetItem* item = editor::ensureContextRowSelected(m_widgets.opacityKeyframeTable, pos, &row);
    if (!item) return;

    QMenu menu;
    ContextMenuActions actions = buildStandardContextMenu(menu, m_widgets.opacityKeyframeTable, row, m_deps.getSelectedClip());
    QAction* chosen = menu.exec(m_widgets.opacityKeyframeTable->viewport()->mapToGlobal(pos));
    if (chosen == actions.deleteRows && actions.deleteRows->isEnabled()) {
        removeSelectedKeyframes();
    }
}

QString OpacityTab::interpolationLabel(bool linearInterpolation) const
{
    return linearInterpolation ? QStringLiteral("Linear") : QStringLiteral("Step");
}

QString OpacityTab::nextInterpolationLabel(const QString& text) const
{
    bool linearInterpolation = true;
    if (!parseInterpolationText(text, &linearInterpolation)) {
        linearInterpolation = true;
    }
    return interpolationLabel(!linearInterpolation);
}

bool OpacityTab::parseInterpolationText(const QString& text, bool* linearInterpolationOut) const
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

int OpacityTab::selectedKeyframeIndex(const TimelineClip& clip) const
{
    for (int i = 0; i < clip.opacityKeyframes.size(); ++i) {
        if (clip.opacityKeyframes[i].frame == m_selectedKeyframeFrame) return i;
    }
    return -1;
}

QList<int64_t> OpacityTab::selectedKeyframeFramesForClip(const TimelineClip& clip) const
{
    QList<int64_t> frames;
    for (const TimelineClip::OpacityKeyframe& keyframe : clip.opacityKeyframes) {
        if (m_selectedKeyframeFrames.contains(keyframe.frame)) {
            frames.push_back(keyframe.frame);
        }
    }
    return frames;
}

int OpacityTab::nearestKeyframeIndex(const TimelineClip& clip) const
{
    if (clip.opacityKeyframes.isEmpty()) return -1;
    const int64_t localFrame = qBound<int64_t>(0,
                                               m_deps.getCurrentTimelineFrame() - clip.startFrame,
                                               qMax<int64_t>(0, clip.durationFrames - 1));
    int nearestIndex = 0;
    int64_t nearestDistance = std::numeric_limits<int64_t>::max();
    for (int i = 0; i < clip.opacityKeyframes.size(); ++i) {
        const int64_t distance = std::abs(clip.opacityKeyframes[i].frame - localFrame);
        if (distance < nearestDistance) {
            nearestDistance = distance;
            nearestIndex = i;
        }
    }
    return nearestIndex;
}

bool OpacityTab::hasRemovableKeyframeSelection(const TimelineClip& clip) const
{
    for (const int64_t frame : selectedKeyframeFramesForClip(clip)) {
        if (frame > 0) return true;
    }
    return false;
}

OpacityTab::OpacityKeyframeDisplay OpacityTab::evaluateDisplayedOpacity(const TimelineClip& clip, int64_t localFrame) const
{
    OpacityKeyframeDisplay result;
    result.frame = qBound<int64_t>(0, localFrame, qMax<int64_t>(0, clip.durationFrames - 1));
    result.opacity = evaluateClipOpacityAtFrame(clip, clip.startFrame + result.frame);
    result.linearInterpolation = true;
    for (const TimelineClip::OpacityKeyframe& keyframe : clip.opacityKeyframes) {
        if (keyframe.frame == result.frame) {
            result.linearInterpolation = keyframe.linearInterpolation;
            break;
        }
        if (keyframe.frame > result.frame) {
            result.linearInterpolation = keyframe.linearInterpolation;
            break;
        }
    }
    return result;
}

void OpacityTab::updateSpinBoxesFromKeyframe(const OpacityKeyframeDisplay& keyframe)
{
    QSignalBlocker opacityBlock(m_widgets.opacitySpin);
    m_widgets.opacitySpin->setValue(keyframe.opacity);
}

void OpacityTab::populateTable(const TimelineClip& clip)
{
    m_widgets.opacityKeyframeTable->setRowCount(clip.opacityKeyframes.size());
    for (int row = 0; row < clip.opacityKeyframes.size(); ++row) {
        const TimelineClip::OpacityKeyframe& keyframe = clip.opacityKeyframes[row];
        auto* frameItem = new QTableWidgetItem(QString::number(keyframe.frame));
        frameItem->setData(Qt::UserRole, QVariant::fromValue(static_cast<qint64>(keyframe.frame)));
        auto* opacityItem = new QTableWidgetItem(QString::number(keyframe.opacity, 'f', 3));
        opacityItem->setData(Qt::UserRole, QVariant::fromValue(static_cast<qint64>(keyframe.frame)));
        auto* interpItem = new QTableWidgetItem(interpolationLabel(keyframe.linearInterpolation));
        interpItem->setData(Qt::UserRole, QVariant::fromValue(static_cast<qint64>(keyframe.frame)));
        m_widgets.opacityKeyframeTable->setItem(row, 0, frameItem);
        m_widgets.opacityKeyframeTable->setItem(row, 1, opacityItem);
        m_widgets.opacityKeyframeTable->setItem(row, 2, interpItem);
    }
}

void OpacityTab::applyOpacityFadeFromPlayhead(bool fadeIn)
{
    const TimelineClip* clip = m_deps.getSelectedClip();
    if (!clip || !m_deps.clipHasVisuals(*clip) || !m_widgets.opacityFadeDurationSpin) {
        return;
    }

    const int64_t localStartFrame = qBound<int64_t>(0,
                                                    m_deps.getCurrentTimelineFrame() - clip->startFrame,
                                                    qMax<int64_t>(0, clip->durationFrames - 1));
    const int64_t fadeFrames = qMax<int64_t>(1, qRound64(m_widgets.opacityFadeDurationSpin->value() * kTimelineFps));
    const int64_t localEndFrame = qBound<int64_t>(0, localStartFrame + fadeFrames, qMax<int64_t>(0, clip->durationFrames - 1));

    const double startOpacity = evaluateDisplayedOpacity(*clip, localStartFrame).opacity;
    const double endOpacity = evaluateDisplayedOpacity(*clip, localEndFrame).opacity;

    const bool updated = m_deps.updateClipById(clip->id, [=](TimelineClip& updatedClip) {
        auto upsertFrame = [](QVector<TimelineClip::OpacityKeyframe>& keyframes,
                              const TimelineClip::OpacityKeyframe& keyframe) {
            for (TimelineClip::OpacityKeyframe& existing : keyframes) {
                if (existing.frame == keyframe.frame) {
                    existing = keyframe;
                    return;
                }
            }
            keyframes.push_back(keyframe);
        };

        TimelineClip::OpacityKeyframe startKeyframe;
        startKeyframe.frame = localStartFrame;
        startKeyframe.opacity = fadeIn ? 0.0 : qBound(0.0, startOpacity, 1.0);
        startKeyframe.linearInterpolation = true;

        TimelineClip::OpacityKeyframe endKeyframe;
        endKeyframe.frame = localEndFrame;
        endKeyframe.opacity = fadeIn ? 1.0 : 0.0;
        endKeyframe.linearInterpolation = true;

        upsertFrame(updatedClip.opacityKeyframes, startKeyframe);
        upsertFrame(updatedClip.opacityKeyframes, endKeyframe);
        normalizeClipOpacityKeyframes(updatedClip);
    });

    if (!updated) return;
    m_deps.setPreviewTimelineClips();
    m_deps.refreshInspector();
    m_deps.scheduleSaveState();
    m_deps.pushHistorySnapshot();
}

void OpacityTab::removeSelectedKeyframes()
{
    const TimelineClip* clip = m_deps.getSelectedClip();
    if (!clip || !hasRemovableKeyframeSelection(*clip)) return;

    const QList<int64_t> selectedFrames = selectedKeyframeFramesForClip(*clip);
    const bool updated = m_deps.updateClipById(clip->id, [&selectedFrames](TimelineClip& editableClip) {
        editableClip.opacityKeyframes.erase(
            std::remove_if(editableClip.opacityKeyframes.begin(),
                           editableClip.opacityKeyframes.end(),
                           [&selectedFrames](const TimelineClip::OpacityKeyframe& keyframe) {
                               return keyframe.frame > 0 && selectedFrames.contains(keyframe.frame);
                           }),
            editableClip.opacityKeyframes.end());
        normalizeClipOpacityKeyframes(editableClip);
    });

    if (!updated) return;
    m_selectedKeyframeFrame = 0;
    m_selectedKeyframeFrames = {0};
    m_deps.setPreviewTimelineClips();
    refresh();
    m_deps.scheduleSaveState();
    m_deps.pushHistorySnapshot();
}
