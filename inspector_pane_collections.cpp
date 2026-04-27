#include "inspector_pane.h"

#include <QAbstractItemView>
#include <QHeaderView>
#include <QLabel>
#include <QTableWidget>
#include <QVBoxLayout>

namespace {

QLabel *createTabHeading(const QString &text, QWidget *parent = nullptr) {
    auto *label = new QLabel(text, parent);
    label->setStyleSheet(QStringLiteral(
        "QLabel { font-size: 13px; font-weight: 700; color: #8fa0b5; "
        "padding: 2px 0 6px 0; }"));
    return label;
}

QVBoxLayout *createTabLayout(QWidget *page)
{
    auto *layout = new QVBoxLayout(page);
    layout->setContentsMargins(8, 8, 8, 8);
    layout->setSpacing(6);
    return layout;
}

} // namespace

QWidget *InspectorPane::buildClipsTab()
{
    auto *page = new QWidget;
    auto *layout = createTabLayout(page);
    layout->addWidget(createTabHeading(QStringLiteral("Clips"), page));

    m_clipsTable = new QTableWidget(page);
    m_clipsTable->setColumnCount(6);
    m_clipsTable->setHorizontalHeaderLabels({
        QStringLiteral("Name"),
        QStringLiteral("Track"),
        QStringLiteral("Type"),
        QStringLiteral("Start"),
        QStringLiteral("Duration"),
        QStringLiteral("File"),
    });
    m_clipsTable->setEditTriggers(QAbstractItemView::DoubleClicked | QAbstractItemView::EditKeyPressed);
    m_clipsTable->setSelectionBehavior(QAbstractItemView::SelectRows);
    m_clipsTable->setSelectionMode(QAbstractItemView::SingleSelection);
    m_clipsTable->setFocusPolicy(Qt::ClickFocus);
    m_clipsTable->setAlternatingRowColors(true);
    m_clipsTable->verticalHeader()->setVisible(false);
    m_clipsTable->horizontalHeader()->setStretchLastSection(true);
    m_clipsTable->horizontalHeader()->setSectionResizeMode(0, QHeaderView::Stretch);
    m_clipsTable->horizontalHeader()->setSectionResizeMode(1, QHeaderView::ResizeToContents);
    m_clipsTable->horizontalHeader()->setSectionResizeMode(2, QHeaderView::ResizeToContents);
    m_clipsTable->horizontalHeader()->setSectionResizeMode(3, QHeaderView::ResizeToContents);
    m_clipsTable->horizontalHeader()->setSectionResizeMode(4, QHeaderView::ResizeToContents);
    m_clipsTable->horizontalHeader()->setSectionResizeMode(5, QHeaderView::Stretch);

    layout->addWidget(m_clipsTable, 1);
    return page;
}

QWidget *InspectorPane::buildHistoryTab()
{
    auto *page = new QWidget;
    auto *layout = createTabLayout(page);
    layout->addWidget(createTabHeading(QStringLiteral("History"), page));

    m_historyTable = new QTableWidget(page);
    m_historyTable->setColumnCount(2);
    m_historyTable->setHorizontalHeaderLabels({
        QStringLiteral("Index"),
        QStringLiteral("Summary"),
    });
    m_historyTable->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_historyTable->setSelectionBehavior(QAbstractItemView::SelectRows);
    m_historyTable->setSelectionMode(QAbstractItemView::SingleSelection);
    m_historyTable->setFocusPolicy(Qt::ClickFocus);
    m_historyTable->setAlternatingRowColors(true);
    m_historyTable->verticalHeader()->setVisible(false);
    m_historyTable->horizontalHeader()->setStretchLastSection(true);

    layout->addWidget(m_historyTable, 1);
    return page;
}

QWidget *InspectorPane::buildTracksTab()
{
    auto *page = new QWidget;
    auto *layout = createTabLayout(page);
    layout->addWidget(createTabHeading(QStringLiteral("Tracks"), page));

    m_tracksTable = new QTableWidget(page);
    m_tracksTable->setColumnCount(3);
    m_tracksTable->setHorizontalHeaderLabels({
        QStringLiteral("Track"),
        QStringLiteral("Visual"),
        QStringLiteral("Audio"),
    });
    m_tracksTable->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_tracksTable->setSelectionBehavior(QAbstractItemView::SelectRows);
    m_tracksTable->setSelectionMode(QAbstractItemView::SingleSelection);
    m_tracksTable->setFocusPolicy(Qt::ClickFocus);
    m_tracksTable->setAlternatingRowColors(true);
    m_tracksTable->verticalHeader()->setVisible(false);
    m_tracksTable->horizontalHeader()->setStretchLastSection(true);
    m_tracksTable->horizontalHeader()->setSectionResizeMode(0, QHeaderView::Stretch);
    m_tracksTable->horizontalHeader()->setSectionResizeMode(1, QHeaderView::ResizeToContents);
    m_tracksTable->horizontalHeader()->setSectionResizeMode(2, QHeaderView::ResizeToContents);

    layout->addWidget(m_tracksTable, 1);
    return page;
}

void InspectorPane::refresh()
{
    emit refreshRequested();
}
