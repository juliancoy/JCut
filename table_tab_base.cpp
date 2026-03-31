#include "table_tab_base.h"

#include <QApplication>

TableTabBase::TableTabBase(const Dependencies& deps, QObject* parent)
    : QObject(parent)
    , m_deps(deps)
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

bool TableTabBase::shouldSkipRefresh(QTableWidget* table) const
{
    // Skip refresh when table items are selected (to avoid disrupting multi-selection
    // and prevent unintended playhead movement)
    return table && table->selectedItems().count() > 0;
}

bool TableTabBase::shouldSkipSyncToPlayhead(QTableWidget* table, QCheckBox* followCheckBox) const
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
    // the user clicks a row, which would fight their manual selection.
    if (m_suppressSyncForTimelineFrame >= 0 && m_deps.getCurrentTimelineFrame) {
        const int64_t currentTimelineFrame = m_deps.getCurrentTimelineFrame();
        if (currentTimelineFrame == m_suppressSyncForTimelineFrame) {
            return true;
        }
    }
    
    return false;
}

void TableTabBase::requestDeferredSeek(int64_t timelineFrame)
{
    m_pendingSeekTimelineFrame = timelineFrame;
    m_deferredSeekTimer.start(QApplication::doubleClickInterval());
}

void TableTabBase::cancelDeferredSeek()
{
    m_deferredSeekTimer.stop();
    m_pendingSeekTimelineFrame = -1;
}
