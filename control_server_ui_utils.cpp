#include "control_server_ui_utils.h"

#include <QAbstractButton>
#include <QAction>
#include <QApplication>
#include <QComboBox>
#include <QContextMenuEvent>
#include <QDoubleSpinBox>
#include <QLineEdit>
#include <QMenu>
#include <QMouseEvent>
#include <QPlainTextEdit>
#include <QSlider>
#include <QSpinBox>
#include <QTableWidget>

namespace control_server {

namespace {

QJsonObject widgetSnapshotRecursive(QWidget* widget,
                                    const QString& path,
                                    int absX,
                                    int absY,
                                    int indexInParent) {
    if (!widget) {
        return {};
    }

    QJsonObject object{
        {QStringLiteral("class"), QString::fromLatin1(widget->metaObject()->className())},
        {QStringLiteral("id"), widget->objectName()},
        {QStringLiteral("visible"), widget->isVisible()},
        {QStringLiteral("enabled"), widget->isEnabled()},
        {QStringLiteral("x"), widget->x()},
        {QStringLiteral("y"), widget->y()},
        {QStringLiteral("abs_x"), absX},
        {QStringLiteral("abs_y"), absY},
        {QStringLiteral("width"), widget->width()},
        {QStringLiteral("height"), widget->height()},
        {QStringLiteral("path"), path},
        {QStringLiteral("index"), indexInParent}
    };

    if (auto* button = qobject_cast<QAbstractButton*>(widget)) {
        object[QStringLiteral("text")] = button->text();
        object[QStringLiteral("clickable")] = true;
        object[QStringLiteral("checked")] = button->isChecked();
    } else if (auto* slider = qobject_cast<QSlider*>(widget)) {
        object[QStringLiteral("clickable")] = true;
        object[QStringLiteral("value")] = slider->value();
        object[QStringLiteral("minimum")] = slider->minimum();
        object[QStringLiteral("maximum")] = slider->maximum();
    } else if (auto* spinBox = qobject_cast<QSpinBox*>(widget)) {
        object[QStringLiteral("clickable")] = false;
        object[QStringLiteral("value")] = spinBox->value();
        object[QStringLiteral("minimum")] = spinBox->minimum();
        object[QStringLiteral("maximum")] = spinBox->maximum();
    } else if (auto* doubleSpinBox = qobject_cast<QDoubleSpinBox*>(widget)) {
        object[QStringLiteral("clickable")] = false;
        object[QStringLiteral("value")] = doubleSpinBox->value();
        object[QStringLiteral("minimum")] = doubleSpinBox->minimum();
        object[QStringLiteral("maximum")] = doubleSpinBox->maximum();
    } else if (auto* comboBox = qobject_cast<QComboBox*>(widget)) {
        object[QStringLiteral("clickable")] = false;
        object[QStringLiteral("currentIndex")] = comboBox->currentIndex();
        object[QStringLiteral("currentText")] = comboBox->currentText();
        object[QStringLiteral("count")] = comboBox->count();
    } else if (auto* lineEdit = qobject_cast<QLineEdit*>(widget)) {
        object[QStringLiteral("clickable")] = false;
        object[QStringLiteral("text")] = lineEdit->text();
    } else if (auto* plainTextEdit = qobject_cast<QPlainTextEdit*>(widget)) {
        object[QStringLiteral("clickable")] = false;
        object[QStringLiteral("text")] = plainTextEdit->toPlainText();
    } else if (auto* tableWidget = qobject_cast<QTableWidget*>(widget)) {
        object[QStringLiteral("clickable")] = false;
        object[QStringLiteral("rows")] = tableWidget->rowCount();
        object[QStringLiteral("columns")] = tableWidget->columnCount();
        object[QStringLiteral("currentRow")] = tableWidget->currentRow();
        object[QStringLiteral("currentColumn")] = tableWidget->currentColumn();
        QJsonArray headers;
        for (int column = 0; column < tableWidget->columnCount(); ++column) {
            const QTableWidgetItem* headerItem = tableWidget->horizontalHeaderItem(column);
            headers.push_back(headerItem ? headerItem->text() : QString());
        }
        object[QStringLiteral("headers")] = headers;
        QJsonArray selectedRows;
        if (tableWidget->selectionModel()) {
            const QModelIndexList rows = tableWidget->selectionModel()->selectedRows();
            for (const QModelIndex& rowIndex : rows) {
                if (rowIndex.isValid()) {
                    selectedRows.push_back(rowIndex.row());
                }
            }
        }
        object[QStringLiteral("selectedRows")] = selectedRows;
    } else {
        object[QStringLiteral("clickable")] = false;
    }

    QJsonArray children;
    const auto childWidgets = widget->findChildren<QWidget*>(QString(), Qt::FindDirectChildrenOnly);
    for (int childIndex = 0; childIndex < childWidgets.size(); ++childIndex) {
        QWidget* child = childWidgets.at(childIndex);
        if (!child) {
            continue;
        }
        const QString childPath = path.isEmpty()
            ? QString::number(childIndex)
            : QStringLiteral("%1.%2").arg(path).arg(childIndex);
        children.append(widgetSnapshotRecursive(child,
                                               childPath,
                                               absX + child->x(),
                                               absY + child->y(),
                                               childIndex));
    }
    object[QStringLiteral("children")] = children;
    return object;
}

} // namespace

QJsonObject widgetSnapshot(QWidget* widget) {
    return widgetSnapshotRecursive(widget,
                                   QStringLiteral("0"),
                                   widget ? widget->x() : 0,
                                   widget ? widget->y() : 0,
                                   0);
}

QJsonObject topLevelWindowSnapshot(QWidget* widget) {
    if (!widget) {
        return {};
    }

    const QRect geometry = widget->geometry();
    const QRect frameGeometry = widget->frameGeometry();
    return QJsonObject{
        {QStringLiteral("class"), QString::fromLatin1(widget->metaObject()->className())},
        {QStringLiteral("id"), widget->objectName()},
        {QStringLiteral("title"), widget->windowTitle()},
        {QStringLiteral("visible"), widget->isVisible()},
        {QStringLiteral("active"), widget->isActiveWindow()},
        {QStringLiteral("x"), geometry.x()},
        {QStringLiteral("y"), geometry.y()},
        {QStringLiteral("width"), geometry.width()},
        {QStringLiteral("height"), geometry.height()},
        {QStringLiteral("frame_x"), frameGeometry.x()},
        {QStringLiteral("frame_y"), frameGeometry.y()},
        {QStringLiteral("frame_width"), frameGeometry.width()},
        {QStringLiteral("frame_height"), frameGeometry.height()}
    };
}

QJsonArray topLevelWindowsSnapshot() {
    QJsonArray windows;
    const auto widgets = QApplication::topLevelWidgets();
    for (QWidget* widget : widgets) {
        if (!widget) {
            continue;
        }
        windows.append(topLevelWindowSnapshot(widget));
    }
    return windows;
}

QWidget* findWidgetByObjectName(QWidget* root, const QString& objectName) {
    if (!root || objectName.isEmpty()) {
        return nullptr;
    }
    if (root->objectName() == objectName) {
        return root;
    }
    const auto matches = root->findChildren<QWidget*>(objectName, Qt::FindChildrenRecursively);
    return matches.isEmpty() ? nullptr : matches.constFirst();
}

QWidget* findWidgetByHierarchyPath(QWidget* root, const QString& path) {
    if (!root) {
        return nullptr;
    }
    const QString trimmed = path.trimmed();
    if (trimmed.isEmpty()) {
        return nullptr;
    }

    QStringList segments = trimmed.split('.', Qt::SkipEmptyParts);
    if (segments.isEmpty()) {
        return nullptr;
    }
    if (segments.constFirst() == QStringLiteral("0")) {
        segments.removeFirst();
    }

    QWidget* current = root;
    for (const QString& segment : segments) {
        bool ok = false;
        const int childIndex = segment.toInt(&ok);
        if (!ok || childIndex < 0) {
            return nullptr;
        }
        const auto children =
            current->findChildren<QWidget*>(QString(), Qt::FindDirectChildrenOnly);
        if (childIndex >= children.size()) {
            return nullptr;
        }
        current = children.at(childIndex);
        if (!current) {
            return nullptr;
        }
    }
    return current;
}

Qt::MouseButton parseMouseButton(const QString& value) {
    const QString normalized = value.trimmed().toLower();
    if (normalized == QStringLiteral("right")) {
        return Qt::RightButton;
    }
    if (normalized == QStringLiteral("middle")) {
        return Qt::MiddleButton;
    }
    return Qt::LeftButton;
}

bool sendSyntheticClick(QWidget* window, const QPoint& pos, Qt::MouseButton button) {
    if (!window) {
        return false;
    }

    QWidget* target = window->childAt(pos);
    if (!target) {
        target = window;
    }

    const QPoint localPos = target->mapFrom(window, pos);
    const QPoint globalPos = target->mapToGlobal(localPos);
    QMouseEvent pressEvent(
        QEvent::MouseButtonPress,
        localPos,
        globalPos,
        button,
        button,
        Qt::NoModifier);
    QMouseEvent releaseEvent(
        QEvent::MouseButtonRelease,
        localPos,
        globalPos,
        button,
        Qt::NoButton,
        Qt::NoModifier);

    const bool pressOk = QApplication::sendEvent(target, &pressEvent);
    const bool releaseOk = QApplication::sendEvent(target, &releaseEvent);
    bool contextOk = true;
    if (button == Qt::RightButton) {
        QContextMenuEvent contextEvent(QContextMenuEvent::Mouse, localPos, globalPos);
        contextOk = QApplication::sendEvent(target, &contextEvent);
    }
    return pressOk && releaseOk && contextOk;
}

bool sendSyntheticClick(QWidget* window, const QPoint& pos) {
    return sendSyntheticClick(window, pos, Qt::LeftButton);
}

QJsonObject menuSnapshot(QMenu* menu) {
    QJsonArray actions;
    if (menu) {
        const auto menuActions = menu->actions();
        for (QAction* action : menuActions) {
            if (!action || action->isSeparator()) {
                continue;
            }
            actions.append(QJsonObject{
                {QStringLiteral("text"), action->text()},
                {QStringLiteral("enabled"), action->isEnabled()}
            });
        }
    }
    return QJsonObject{
        {QStringLiteral("ok"), menu != nullptr},
        {QStringLiteral("visible"), menu && menu->isVisible()},
        {QStringLiteral("actions"), actions}
    };
}

QMenu* activePopupMenu() {
    QWidget* widget = QApplication::activePopupWidget();
    return qobject_cast<QMenu*>(widget);
}

} // namespace control_server
