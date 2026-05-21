#include "speakers_table.h"

#include <QAction>
#include <QEvent>
#include <QHeaderView>
#include <QHelpEvent>
#include <QMenu>
#include <QMouseEvent>
#include <QTableWidgetItem>

namespace {
constexpr int kSpeakerIdColumn = 1;
}

SpeakersTable::SpeakersTable(QWidget* parent)
    : QTableWidget(parent)
{
    setMouseTracking(true);
    viewport()->setMouseTracking(true);
    if (QHeaderView* header = horizontalHeader()) {
        header->setContextMenuPolicy(Qt::CustomContextMenu);
        connect(header, &QHeaderView::customContextMenuRequested,
                this, [this](const QPoint& pos) { showHeaderContextMenu(pos); });
    }
}

QJsonArray SpeakersTable::hiddenColumnsState() const
{
    QJsonArray hiddenColumns;
    for (int column = 0; column < columnCount(); ++column) {
        hiddenColumns.push_back(isColumnHidden(column));
    }
    return hiddenColumns;
}

void SpeakersTable::applyHiddenColumns(const QJsonArray& hiddenColumns)
{
    const int limit = qMin(columnCount(), hiddenColumns.size());
    for (int column = 0; column < limit; ++column) {
        if (isAlwaysVisibleColumn(column)) {
            setColumnHidden(column, false);
            continue;
        }
        setColumnHidden(column, hiddenColumns.at(column).toBool(false));
    }
}

void SpeakersTable::showHeaderContextMenu(const QPoint& pos)
{
    QHeaderView* header = horizontalHeader();
    if (!header) {
        return;
    }

    QMenu menu(this);
    for (int column = 0; column < columnCount(); ++column) {
        const QString label = horizontalHeaderItem(column)
            ? horizontalHeaderItem(column)->text()
            : QStringLiteral("Column %1").arg(column + 1);
        QAction* action = menu.addAction(label);
        action->setCheckable(true);

        const bool alwaysVisible = isAlwaysVisibleColumn(column);
        action->setChecked(!isColumnHidden(column));
        if (alwaysVisible) {
            action->setEnabled(false);
            action->setToolTip(QStringLiteral("Speaker ID must remain visible."));
            continue;
        }

        connect(action, &QAction::toggled, this, [this, column](bool checked) {
            const bool changed = (isColumnHidden(column) == checked);
            setColumnHidden(column, !checked);
            if (changed) {
                emit columnVisibilityChanged();
            }
        });
    }

    menu.exec(header->viewport()->mapToGlobal(pos));
}

bool SpeakersTable::isAlwaysVisibleColumn(int column) const
{
    return column == kSpeakerIdColumn;
}

bool SpeakersTable::viewportEvent(QEvent* event)
{
    if (!event) {
        return QTableWidget::viewportEvent(event);
    }

    const auto updateAvatarHover = [this](const QPoint& pos) {
        const QTableWidgetItem* item = itemAt(pos);
        const QString speakerId =
            (item && item->column() == 0)
                ? item->data(Qt::UserRole).toString().trimmed()
                : QString();
        if (speakerId == m_hoveredSpeakerId) {
            if (!speakerId.isEmpty()) {
                emit avatarHoverRequested(speakerId, viewport()->mapToGlobal(pos));
            }
            return;
        }
        m_hoveredSpeakerId = speakerId;
        if (m_hoveredSpeakerId.isEmpty()) {
            emit avatarHoverCleared();
        } else {
            emit avatarHoverRequested(m_hoveredSpeakerId, viewport()->mapToGlobal(pos));
        }
    };

    switch (event->type()) {
    case QEvent::MouseMove: {
        auto* mouseEvent = static_cast<QMouseEvent*>(event);
        updateAvatarHover(mouseEvent->pos());
        // Passive hover is only used for avatar preview. Letting the base
        // QTableWidget handle no-button mouse moves can cause current-row
        // churn on some platforms/styles.
        if (mouseEvent->buttons() == Qt::NoButton) {
            return true;
        }
        break;
    }
    case QEvent::ToolTip: {
        const QPoint pos = static_cast<QHelpEvent*>(event)->pos();
        updateAvatarHover(pos);
        const QTableWidgetItem* item = itemAt(pos);
        if (item && item->column() == 0) {
            return true;
        }
        break;
    }
    case QEvent::Leave:
    case QEvent::Hide:
        if (!m_hoveredSpeakerId.isEmpty()) {
            m_hoveredSpeakerId.clear();
            emit avatarHoverCleared();
        }
        break;
    default:
        break;
    }

    return QTableWidget::viewportEvent(event);
}
