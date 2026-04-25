#include "sync_tab.h"

#include "keyframe_table_shared.h"

#include <QApplication>
#include <QLabel>
#include <QMenu>
#include <QPushButton>
#include <QSignalBlocker>

#include <algorithm>

SyncTab::SyncTab(const Widgets& widgets, const Dependencies& deps, QObject* parent)
    : QObject(parent)
    , m_widgets(widgets)
    , m_deps(deps)
{
    m_deferredSeekTimer.setSingleShot(true);
    connect(&m_deferredSeekTimer, &QTimer::timeout, this, [this]() {
        if (m_pendingSeekFrame < 0 || !m_deps.seekToTimelineFrame) {
            return;
        }
        m_deps.seekToTimelineFrame(m_pendingSeekFrame);
        m_pendingSeekFrame = -1;
    });
}

void SyncTab::wire()
{
    if (m_widgets.syncTable) {
        connect(m_widgets.syncTable, &QTableWidget::itemSelectionChanged,
                this, &SyncTab::onTableSelectionChanged);
        connect(m_widgets.syncTable, &QTableWidget::itemChanged,
                this, &SyncTab::onTableItemChanged);
        connect(m_widgets.syncTable, &QTableWidget::itemDoubleClicked,
                this, &SyncTab::onTableItemDoubleClicked);
        m_widgets.syncTable->setContextMenuPolicy(Qt::CustomContextMenu);
        connect(m_widgets.syncTable, &QWidget::customContextMenuRequested,
                this, &SyncTab::onTableCustomContextMenu);
    }

    if (m_widgets.clearAllSyncPointsButton) {
        connect(m_widgets.clearAllSyncPointsButton, &QPushButton::clicked,
                this, &SyncTab::onClearAllSyncPointsClicked);
    }
}

bool SyncTab::parseSyncActionText(const QString& text, RenderSyncAction* actionOut)
{
    if (!actionOut) {
        return false;
    }
    const QString normalized = text.trimmed().toLower();
    if (normalized == QStringLiteral("duplicate") || normalized == QStringLiteral("dup")) {
        *actionOut = RenderSyncAction::DuplicateFrame;
        return true;
    }
    if (normalized == QStringLiteral("skip")) {
        *actionOut = RenderSyncAction::SkipFrame;
        return true;
    }
    return false;
}

void SyncTab::refresh()
{
    if (!m_widgets.syncTable || !m_deps.getRenderSyncMarkers || !m_deps.getSelectedClip) {
        return;
    }

    if (m_widgets.syncInspectorClipLabel) {
        m_widgets.syncInspectorClipLabel->setText(QStringLiteral("Sync"));
    }

    m_updating = true;
    const int allSyncMarkerCount = m_deps.getRenderSyncMarkers().size();
    if (m_widgets.clearAllSyncPointsButton) {
        m_widgets.clearAllSyncPointsButton->setEnabled(allSyncMarkerCount > 0);
    }

    const QSet<int64_t> selectedFrames = editor::collectSelectedFrameRoles(m_widgets.syncTable);
    m_widgets.syncTable->clearContents();
    m_widgets.syncTable->setRowCount(0);

    const TimelineClip* selectedClip = m_deps.getSelectedClip();
    if (!selectedClip) {
        if (m_widgets.syncInspectorDetailsLabel) {
            m_widgets.syncInspectorDetailsLabel->setText(
                QStringLiteral("Select a clip to inspect its sync markers."));
        }
        m_updating = false;
        return;
    }

    if (m_widgets.syncInspectorClipLabel) {
        m_widgets.syncInspectorClipLabel->setText(QStringLiteral("Sync\n%1").arg(selectedClip->label));
    }

    QVector<RenderSyncMarker> markers;
    const QVector<RenderSyncMarker> allMarkers = m_deps.getRenderSyncMarkers();
    markers.reserve(allMarkers.size());
    for (const RenderSyncMarker& marker : allMarkers) {
        if (marker.clipId == selectedClip->id) {
            markers.push_back(marker);
        }
    }

    if (markers.isEmpty()) {
        if (m_widgets.syncInspectorDetailsLabel) {
            m_widgets.syncInspectorDetailsLabel->setText(
                QStringLiteral("No render sync markers for the selected clip."));
        }
        m_updating = false;
        return;
    }

    if (m_widgets.syncInspectorDetailsLabel) {
        m_widgets.syncInspectorDetailsLabel->setText(
            QStringLiteral("%1 sync markers for the selected clip. Edit Frame, Count, or Action directly.")
                .arg(markers.size()));
    }

    m_widgets.syncTable->setRowCount(markers.size());

    for (int i = 0; i < markers.size(); ++i) {
        const RenderSyncMarker& marker = markers[i];
        const QColor clipColor = m_deps.clipColorForId ? m_deps.clipColorForId(marker.clipId)
                                                       : QColor(QStringLiteral("#24303c"));
        const QColor rowBackground = QColor(clipColor.red(), clipColor.green(), clipColor.blue(), 72);
        const QColor rowForeground = QColor(QStringLiteral("#f4f7fb"));
        const QString clipLabel = m_deps.clipLabelForId ? m_deps.clipLabelForId(marker.clipId) : marker.clipId;

        auto* clipItem = new QTableWidgetItem(QString());
        clipItem->setFlags(clipItem->flags() & ~Qt::ItemIsEditable);
        clipItem->setToolTip(clipLabel);

        auto* frameItem = new QTableWidgetItem(QString::number(marker.frame));
        auto* countItem = new QTableWidgetItem(QString::number(marker.count));
        auto* actionItem = new QTableWidgetItem(
            marker.action == RenderSyncAction::DuplicateFrame ? QStringLiteral("Duplicate")
                                                              : QStringLiteral("Skip"));

        for (QTableWidgetItem* item : {clipItem, frameItem, countItem, actionItem}) {
            item->setData(Qt::UserRole, QVariant::fromValue(static_cast<qint64>(marker.frame)));
            item->setData(Qt::UserRole + 1, marker.clipId);
            item->setBackground(rowBackground);
            item->setForeground(rowForeground);
        }

        m_widgets.syncTable->setItem(i, 0, clipItem);
        m_widgets.syncTable->setItem(i, 1, frameItem);
        m_widgets.syncTable->setItem(i, 2, countItem);
        m_widgets.syncTable->setItem(i, 3, actionItem);
    }

    editor::restoreSelectionByFrameRole(m_widgets.syncTable, selectedFrames);
    m_updating = false;
}

void SyncTab::scheduleDeferredSeek(int64_t frame)
{
    if (frame < 0 || !m_deps.seekToTimelineFrame) {
        return;
    }
    m_pendingSeekFrame = frame;
    m_deferredSeekTimer.start(QApplication::doubleClickInterval());
}

void SyncTab::cancelDeferredSeek()
{
    m_deferredSeekTimer.stop();
    m_pendingSeekFrame = -1;
}

void SyncTab::onTableSelectionChanged()
{
    if (m_updating || !m_widgets.syncTable) {
        return;
    }
    const int64_t primaryFrame = editor::primarySelectedFrameRole(m_widgets.syncTable);
    if (primaryFrame >= 0) {
        scheduleDeferredSeek(primaryFrame);
    }
}

void SyncTab::onTableItemChanged(QTableWidgetItem* item)
{
    if (m_updating || !item || !m_widgets.syncTable || !m_deps.getRenderSyncMarkers ||
        !m_deps.setRenderSyncMarkers) {
        return;
    }

    const int row = item->row();
    if (row < 0 || row >= m_widgets.syncTable->rowCount()) {
        return;
    }

    auto tableText = [this, row](int column) -> QString {
        QTableWidgetItem* tableItem = m_widgets.syncTable->item(row, column);
        return tableItem ? tableItem->text().trimmed() : QString();
    };

    const QString clipId = m_widgets.syncTable->item(row, 0)
        ? m_widgets.syncTable->item(row, 0)->data(Qt::UserRole + 1).toString()
        : QString();
    const int64_t originalFrame = m_widgets.syncTable->item(row, 0)
        ? m_widgets.syncTable->item(row, 0)->data(Qt::UserRole).toLongLong()
        : -1;
    if (clipId.isEmpty() || originalFrame < 0) {
        refresh();
        return;
    }

    bool ok = false;
    RenderSyncMarker edited;
    edited.clipId = clipId;
    edited.frame = tableText(1).toLongLong(&ok);
    if (!ok) { refresh(); return; }
    edited.count = tableText(2).toInt(&ok);
    if (!ok) { refresh(); return; }
    edited.count = qMax(1, edited.count);
    if (!parseSyncActionText(tableText(3), &edited.action)) {
        refresh();
        return;
    }

    QVector<RenderSyncMarker> markers = m_deps.getRenderSyncMarkers();
    bool replaced = false;
    for (RenderSyncMarker& marker : markers) {
        if (marker.clipId == clipId && marker.frame == originalFrame) {
            marker = edited;
            replaced = true;
            break;
        }
    }
    if (!replaced) {
        refresh();
        return;
    }

    std::sort(markers.begin(), markers.end(), [](const RenderSyncMarker& a, const RenderSyncMarker& b) {
        if (a.frame == b.frame) {
            return a.clipId < b.clipId;
        }
        return a.frame < b.frame;
    });

    m_deps.setRenderSyncMarkers(markers);
    refresh();
    if (m_deps.scheduleSaveState) {
        m_deps.scheduleSaveState();
    }
}

void SyncTab::onTableItemDoubleClicked(QTableWidgetItem* item)
{
    Q_UNUSED(item);
    cancelDeferredSeek();
}

void SyncTab::onTableCustomContextMenu(const QPoint& pos)
{
    if (!m_widgets.syncTable || !m_deps.getRenderSyncMarkers || !m_deps.setRenderSyncMarkers ||
        !m_deps.getCurrentTimelineFrame) {
        return;
    }

    int row = -1;
    QTableWidgetItem* item = editor::ensureContextRowSelected(m_widgets.syncTable, pos, &row);
    if (!item) {
        return;
    }

    const QString clipId = item->data(Qt::UserRole + 1).toString();
    const int64_t frame = item->data(Qt::UserRole).toLongLong();
    if (clipId.isEmpty() || frame < 0) {
        return;
    }

    QMenu menu;
    QAction* copyToCurrentPlayheadAction = menu.addAction(QStringLiteral("Copy to Current Playhead"));
    copyToCurrentPlayheadAction->setEnabled(frame != m_deps.getCurrentTimelineFrame());
    QAction* deleteAction = menu.addAction(QStringLiteral("Delete"));
    QAction* chosen = menu.exec(m_widgets.syncTable->viewport()->mapToGlobal(pos));
    if (!chosen) {
        return;
    }

    QVector<RenderSyncMarker> markers = m_deps.getRenderSyncMarkers();

    if (chosen == deleteAction) {
        const auto newEnd = std::remove_if(markers.begin(), markers.end(), [&](const RenderSyncMarker& marker) {
            return marker.clipId == clipId && marker.frame == frame;
        });
        if (newEnd == markers.end()) {
            return;
        }
        markers.erase(newEnd, markers.end());
        m_deps.setRenderSyncMarkers(markers);
        refresh();
        if (m_deps.scheduleSaveState) {
            m_deps.scheduleSaveState();
        }
        return;
    }

    if (chosen != copyToCurrentPlayheadAction || !copyToCurrentPlayheadAction->isEnabled()) {
        return;
    }

    RenderSyncMarker sourceMarker;
    bool foundSource = false;
    for (const RenderSyncMarker& marker : markers) {
        if (marker.clipId == clipId && marker.frame == frame) {
            sourceMarker = marker;
            foundSource = true;
            break;
        }
    }
    if (!foundSource) {
        return;
    }

    sourceMarker.frame = m_deps.getCurrentTimelineFrame();
    bool replaced = false;
    for (RenderSyncMarker& marker : markers) {
        if (marker.clipId == sourceMarker.clipId && marker.frame == sourceMarker.frame) {
            marker = sourceMarker;
            replaced = true;
            break;
        }
    }
    if (!replaced) {
        markers.push_back(sourceMarker);
    }

    std::sort(markers.begin(), markers.end(), [](const RenderSyncMarker& a, const RenderSyncMarker& b) {
        if (a.frame == b.frame) {
            return a.clipId < b.clipId;
        }
        return a.frame < b.frame;
    });

    m_deps.setRenderSyncMarkers(markers);
    refresh();
    if (m_deps.scheduleSaveState) {
        m_deps.scheduleSaveState();
    }
}

void SyncTab::onClearAllSyncPointsClicked()
{
    if (!m_deps.getRenderSyncMarkers || !m_deps.setRenderSyncMarkers) {
        return;
    }

    const QVector<RenderSyncMarker> markers = m_deps.getRenderSyncMarkers();
    if (markers.isEmpty()) {
        if (m_deps.showNoSyncPointsInfo) {
            m_deps.showNoSyncPointsInfo();
        }
        return;
    }

    if (m_deps.confirmClearAllSyncPoints && !m_deps.confirmClearAllSyncPoints(markers.size())) {
        return;
    }

    m_deps.setRenderSyncMarkers({});
    refresh();
    if (m_deps.scheduleSaveState) {
        m_deps.scheduleSaveState();
    }
    if (m_deps.pushHistorySnapshot) {
        m_deps.pushHistorySnapshot();
    }
}
