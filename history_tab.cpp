#include "history_tab.h"

#include <QHeaderView>
#include <QJsonArray>
#include <QJsonObject>

HistoryTab::HistoryTab(const Widgets& widgets, const Dependencies& deps, QObject* parent)
    : QObject(parent)
    , m_widgets(widgets)
    , m_deps(deps) {
}

void HistoryTab::wire() {
    if (!m_widgets.historyTable) {
        return;
    }
    m_widgets.historyTable->setColumnCount(2);
    m_widgets.historyTable->setHorizontalHeaderLabels(QStringList() << QStringLiteral("Index") << QStringLiteral("Summary"));
    m_widgets.historyTable->horizontalHeader()->setStretchLastSection(true);
    m_widgets.historyTable->setSelectionBehavior(QAbstractItemView::SelectRows);
    m_widgets.historyTable->setSelectionMode(QAbstractItemView::SingleSelection);
    m_widgets.historyTable->setEditTriggers(QAbstractItemView::NoEditTriggers);
    connect(m_widgets.historyTable, &QTableWidget::doubleClicked, this, [this](const QModelIndex& index) {
        onRowDoubleClicked(index.row());
    });
}

void HistoryTab::refresh() {
    if (!m_widgets.historyTable || !m_deps.getHistoryEntries || !m_deps.getHistoryIndex) {
        return;
    }

    const QJsonArray entries = m_deps.getHistoryEntries();
    const int currentIndex = m_deps.getHistoryIndex();

    m_widgets.historyTable->setRowCount(entries.size());

    for (int i = 0; i < entries.size(); ++i) {
        const QJsonObject entry = entries.at(i).toObject();
        const bool isCurrent = (i == currentIndex);

        QTableWidgetItem* timeItem = new QTableWidgetItem(QString::number(i));
        timeItem->setFlags(timeItem->flags() & ~Qt::ItemIsEditable);
        if (isCurrent) {
            timeItem->setFont(QFont(timeItem->font().family(), timeItem->font().pointSize(), QFont::Bold));
        }
        m_widgets.historyTable->setItem(i, 0, timeItem);

        const int clipCount = entry.value(QStringLiteral("timeline")).toArray().size();
        const int trackCount = entry.value(QStringLiteral("tracks")).toArray().size();
        QString summary = QStringLiteral("%1 clips, %2 tracks").arg(clipCount).arg(trackCount);

        if (isCurrent) {
            summary += QStringLiteral(" (current)");
        }

        QTableWidgetItem* stateItem = new QTableWidgetItem(summary);
        stateItem->setFlags(stateItem->flags() & ~Qt::ItemIsEditable);
        m_widgets.historyTable->setItem(i, 1, stateItem);
    }

    if (currentIndex >= 0 && currentIndex < entries.size()) {
        m_widgets.historyTable->selectRow(currentIndex);
    }
}

void HistoryTab::onRowDoubleClicked(int row) {
    if (m_deps.restoreToHistoryIndex) {
        m_deps.restoreToHistoryIndex(row);
        refresh();
    }
}