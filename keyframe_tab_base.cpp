#include "keyframe_tab_base.h"

#include "keyframe_table_shared.h"

#include <QApplication>
#include <QCheckBox>
#include <QKeyEvent>
#include <QTimer>

KeyframeTabBase::KeyframeTabBase(const Dependencies& deps, QObject* parent)
    : QObject(parent)
    , m_deps(deps)
{
}

bool KeyframeTabBase::shouldSkipSyncToPlayhead(QTableWidget* table, QCheckBox* followCheckBox)
{
    if (!table || m_updating) {
        return true;
    }
    
    // Skip if follow checkbox is unchecked
    if (followCheckBox && !followCheckBox->isChecked()) {
        return true;
    }
    
    // Skip if table has focus (user is manually selecting a row)
    QWidget* focus = QApplication::focusWidget();
    if (focus && table->isAncestorOf(focus)) {
        return true;
    }
    
    // Skip if the current timeline frame matches the one we just manually selected.
    // This prevents the table from jumping to follow the playhead immediately after
    // the user clicks a row (which seeks to that frame).
    if (m_suppressSyncForTimelineFrame >= 0) {
        const int64_t currentTimelineFrame = m_deps.getCurrentTimelineFrame();
        if (currentTimelineFrame == m_suppressSyncForTimelineFrame) {
            return true;
        }
        // Clear suppression once playhead moves to a different frame
        m_suppressSyncForTimelineFrame = -1;
    }
    
    return false;
}

int64_t KeyframeTabBase::calculateLocalFrame(const TimelineClip* clip) const
{
    if (!clip) {
        return 0;
    }
    return qBound<int64_t>(0,
                           m_deps.getCurrentTimelineFrame() - clip->startFrame,
                           qMax<int64_t>(0, clip->durationFrames - 1));
}

QList<int64_t> KeyframeTabBase::selectedKeyframeFramesFromTable(QTableWidget* table) const
{
    QList<int64_t> frames;
    if (!table) return frames;
    
    const QList<QTableWidgetItem*> selected = table->selectedItems();
    QSet<int> selectedRows;
    for (QTableWidgetItem* item : selected) {
        selectedRows.insert(item->row());
    }
    
    for (int row : selectedRows) {
        if (QTableWidgetItem* item = table->item(row, 0)) {
            const int64_t frame = item->data(Qt::UserRole).toLongLong();
            if (frame >= 0) {
                frames.append(frame);
            }
        }
    }
    return frames;
}

int KeyframeTabBase::nearestKeyframeIndexFromTable(QTableWidget* table, int64_t targetFrame) const
{
    if (!table || table->rowCount() <= 0) return -1;
    
    int bestIndex = -1;
    int64_t bestDistance = std::numeric_limits<int64_t>::max();
    
    for (int i = 0; i < table->rowCount(); ++i) {
        if (QTableWidgetItem* item = table->item(i, 0)) {
            const int64_t frame = item->data(Qt::UserRole).toLongLong();
            const int64_t distance = qAbs(frame - targetFrame);
            if (distance < bestDistance) {
                bestDistance = distance;
                bestIndex = i;
            }
        }
    }
    return bestIndex;
}

void KeyframeTabBase::onTableSelectionChangedBase(QTableWidget* table, QTimer* deferredSeekTimer,
                                                  int64_t* pendingSeekFrame)
{
    if (m_updating || m_syncingTableSelection) return;
    
    const QSet<int64_t> selectedFrames = editor::collectSelectedFrameRoles(table);
    const int64_t primaryFrame = editor::primarySelectedFrameRole(table);
    
    if (primaryFrame < 0) return;
    
    m_selectedKeyframeFrame = primaryFrame;
    m_selectedKeyframeFrames = selectedFrames;
    
    // Suppress auto-sync for this timeline frame to prevent the table from
    // jumping immediately after user clicks a row (which seeks to that frame).
    const TimelineClip* selectedClip = m_deps.getSelectedClip();
    if (selectedClip) {
        m_suppressSyncForTimelineFrame = selectedClip->startFrame + primaryFrame;
    }
    
    // Optional: deferred seek to timeline frame
    if (deferredSeekTimer && pendingSeekFrame) {
        if (selectedClip && m_deps.seekToTimelineFrame) {
            *pendingSeekFrame = selectedClip->startFrame + primaryFrame;
            deferredSeekTimer->start(QApplication::doubleClickInterval());
        }
    }
    
    if (m_deps.onKeyframeSelectionChanged) {
        m_deps.onKeyframeSelectionChanged();
    }
    
    emit keyframeSelectionChanged();
}

void KeyframeTabBase::installTableHandlers(QTableWidget* table)
{
    if (!table) return;
    table->installEventFilter(this);
}

bool KeyframeTabBase::handleTableKeyPress(QKeyEvent* event)
{
    if (event->key() == Qt::Key_Delete || event->key() == Qt::Key_Backspace) {
        removeSelectedKeyframesFromCurrentTable();
        return true;
    }
    return false;
}

KeyframeTabBase::ContextMenuActions KeyframeTabBase::buildStandardContextMenu(
    QMenu& menu, QTableWidget* table, int row, const TimelineClip* clip)
{
    ContextMenuActions actions;
    
    if (!table || row < 0) return actions;
    
    QTableWidgetItem* item = table->item(row, 0);
    if (!item) return actions;
    
    const int64_t anchorFrame = item->data(Qt::UserRole).toLongLong();
    const int64_t previousFrame = editor::rowFrameRole(table, row - 1);
    const int64_t nextFrame = editor::rowFrameRole(table, row + 1);
    const int64_t localFrame = clip ? calculateLocalFrame(clip) : -1;
    
    const int deletableRowCount =
        editor::countSelectedFrameRoles(table, [](int64_t frame) { return frame > 0; });
    
    // Build menu
    actions.addAbove = menu.addAction(QStringLiteral("Add Keyframe Above"));
    actions.addAbove->setEnabled(previousFrame >= 0);
    
    actions.addBelow = menu.addAction(QStringLiteral("Add Keyframe Below"));
    actions.addBelow->setEnabled(nextFrame >= 0);
    
    actions.copyToNext = menu.addAction(QStringLiteral("Copy to Next Frame"));
    const bool canCopyToNext = clip && m_deps.clipHasVisuals(*clip) &&
                               anchorFrame >= 0 &&
                               anchorFrame < qMax<int64_t>(0, clip->durationFrames - 1);
    actions.copyToNext->setEnabled(canCopyToNext);
    
    actions.copyToPlayhead = menu.addAction(QStringLiteral("Copy to Current Playhead"));
    const bool canCopyToPlayhead = clip && m_deps.clipHasVisuals(*clip) &&
                                   localFrame >= 0 && localFrame != anchorFrame;
    actions.copyToPlayhead->setEnabled(canCopyToPlayhead);
    
    menu.addSeparator();
    
    actions.deleteRows = menu.addAction(deletableRowCount == 1
                                            ? QStringLiteral("Delete Row")
                                            : QStringLiteral("Delete Rows"));
    actions.deleteRows->setEnabled(deletableRowCount > 0);
    
    return actions;
}

bool KeyframeTabBase::eventFilter(QObject* watched, QEvent* event)
{
    if (event->type() == QEvent::KeyPress) {
        auto* keyEvent = static_cast<QKeyEvent*>(event);
        if (handleTableKeyPress(keyEvent)) {
            return true;
        }
    }
    return QObject::eventFilter(watched, event);
}
