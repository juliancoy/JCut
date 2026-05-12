#include "control_server_ui_utils.h"

#include <QAbstractButton>
#include <QAbstractItemView>
#include <QAbstractScrollArea>
#include <QAction>
#include <QApplication>
#include <QComboBox>
#include <QContextMenuEvent>
#include <QDoubleSpinBox>
#include <QHeaderView>
#include <QLineEdit>
#include <QMenu>
#include <QMouseEvent>
#include <QPlainTextEdit>
#include <QScrollArea>
#include <QSlider>
#include <QSpinBox>
#include <QSplitter>
#include <QTabWidget>
#include <QTableWidget>

namespace control_server {

namespace {

QJsonObject menuActionSnapshot(QAction* action) {
    if (!action || action->isSeparator()) {
        return {};
    }

    QJsonObject object{
        {QStringLiteral("text"), action->text()},
        {QStringLiteral("normalized_text"), normalizedActionText(action->text())},
        {QStringLiteral("enabled"), action->isEnabled()},
        {QStringLiteral("checkable"), action->isCheckable()},
        {QStringLiteral("checked"), action->isChecked()},
        {QStringLiteral("has_submenu"), action->menu() != nullptr}
    };

    QJsonArray children;
    if (QMenu* submenu = action->menu()) {
        for (QAction* childAction : submenu->actions()) {
            const QJsonObject child = menuActionSnapshot(childAction);
            if (!child.isEmpty()) {
                children.append(child);
            }
        }
    }
    if (!children.isEmpty()) {
        object[QStringLiteral("children")] = children;
    }
    return object;
}

QString widgetRole(QWidget* widget) {
    if (!widget) {
        return QStringLiteral("none");
    }
    if (qobject_cast<QAbstractButton*>(widget)) {
        return QStringLiteral("button");
    }
    if (qobject_cast<QSlider*>(widget)) {
        return QStringLiteral("slider");
    }
    if (qobject_cast<QSpinBox*>(widget) || qobject_cast<QDoubleSpinBox*>(widget)) {
        return QStringLiteral("spinbox");
    }
    if (qobject_cast<QTabWidget*>(widget)) {
        return QStringLiteral("tab_widget");
    }
    if (qobject_cast<QComboBox*>(widget)) {
        return QStringLiteral("combo_box");
    }
    if (qobject_cast<QLineEdit*>(widget)) {
        return QStringLiteral("line_edit");
    }
    if (qobject_cast<QPlainTextEdit*>(widget)) {
        return QStringLiteral("plain_text_edit");
    }
    if (qobject_cast<QTableWidget*>(widget)) {
        return QStringLiteral("table_widget");
    }
    if (qobject_cast<QAbstractItemView*>(widget)) {
        return QStringLiteral("item_view");
    }
    if (qobject_cast<QSplitter*>(widget)) {
        return QStringLiteral("splitter");
    }
    if (qobject_cast<QScrollArea*>(widget)) {
        return QStringLiteral("scroll_area");
    }
    if (qobject_cast<QMenu*>(widget)) {
        return QStringLiteral("menu");
    }
    return QStringLiteral("widget");
}

QString focusPolicyName(Qt::FocusPolicy policy) {
    switch (policy) {
    case Qt::NoFocus:
        return QStringLiteral("none");
    case Qt::TabFocus:
        return QStringLiteral("tab");
    case Qt::ClickFocus:
        return QStringLiteral("click");
    case Qt::StrongFocus:
        return QStringLiteral("strong");
    case Qt::WheelFocus:
        return QStringLiteral("wheel");
    default:
        return QStringLiteral("unknown");
    }
}

QString contextMenuPolicyName(Qt::ContextMenuPolicy policy) {
    switch (policy) {
    case Qt::NoContextMenu:
        return QStringLiteral("none");
    case Qt::DefaultContextMenu:
        return QStringLiteral("default");
    case Qt::ActionsContextMenu:
        return QStringLiteral("actions");
    case Qt::CustomContextMenu:
        return QStringLiteral("custom");
    case Qt::PreventContextMenu:
        return QStringLiteral("prevent");
    default:
        return QStringLiteral("unknown");
    }
}

QString orientationName(Qt::Orientation orientation) {
    return orientation == Qt::Horizontal
        ? QStringLiteral("horizontal")
        : QStringLiteral("vertical");
}

QJsonArray widgetActionsSnapshot(QWidget* widget) {
    QJsonArray actions;
    if (!widget) {
        return actions;
    }
    for (QAction* action : widget->actions()) {
        const QJsonObject actionObject = menuActionSnapshot(action);
        if (!actionObject.isEmpty()) {
            actions.append(actionObject);
        }
    }
    return actions;
}

QString stableWidgetId(QWidget* widget, const QString& path) {
    if (!widget) {
        return QString();
    }
    const QString objectName = widget->objectName().trimmed();
    if (!objectName.isEmpty()) {
        return objectName;
    }
    return QStringLiteral("%1@%2")
        .arg(QString::fromLatin1(widget->metaObject()->className()), path);
}

QJsonObject widgetGeometrySnapshot(QWidget* widget, int absX, int absY) {
    if (!widget) {
        return {};
    }

    const QRect localGeometry = widget->geometry();
    const QRect frameGeometry = widget->frameGeometry();
    const QPoint globalTopLeft = widget->mapToGlobal(QPoint(0, 0));
    return QJsonObject{
        {QStringLiteral("local"), QJsonObject{
             {QStringLiteral("x"), localGeometry.x()},
             {QStringLiteral("y"), localGeometry.y()},
             {QStringLiteral("width"), localGeometry.width()},
             {QStringLiteral("height"), localGeometry.height()}
         }},
        {QStringLiteral("absolute"), QJsonObject{
             {QStringLiteral("x"), absX},
             {QStringLiteral("y"), absY},
             {QStringLiteral("width"), widget->width()},
             {QStringLiteral("height"), widget->height()}
         }},
        {QStringLiteral("global"), QJsonObject{
             {QStringLiteral("x"), globalTopLeft.x()},
             {QStringLiteral("y"), globalTopLeft.y()},
             {QStringLiteral("width"), widget->width()},
             {QStringLiteral("height"), widget->height()}
         }},
        {QStringLiteral("frame"), QJsonObject{
             {QStringLiteral("x"), frameGeometry.x()},
             {QStringLiteral("y"), frameGeometry.y()},
             {QStringLiteral("width"), frameGeometry.width()},
             {QStringLiteral("height"), frameGeometry.height()}
         }},
        {QStringLiteral("minimum"), QJsonObject{
             {QStringLiteral("width"), widget->minimumWidth()},
             {QStringLiteral("height"), widget->minimumHeight()}
         }},
        {QStringLiteral("maximum"), QJsonObject{
             {QStringLiteral("width"), widget->maximumWidth()},
             {QStringLiteral("height"), widget->maximumHeight()}
         }}
    };
}

QJsonObject widgetStateSnapshot(QWidget* widget) {
    if (!widget) {
        return {};
    }

    return QJsonObject{
        {QStringLiteral("visible"), widget->isVisible()},
        {QStringLiteral("enabled"), widget->isEnabled()},
        {QStringLiteral("hidden"), widget->isHidden()},
        {QStringLiteral("window"), widget->isWindow()},
        {QStringLiteral("active_window"), widget->isActiveWindow()},
        {QStringLiteral("has_focus"), widget->hasFocus()},
        {QStringLiteral("focus_policy"), focusPolicyName(widget->focusPolicy())},
        {QStringLiteral("context_menu_policy"), contextMenuPolicyName(widget->contextMenuPolicy())},
        {QStringLiteral("supports_context_menu"), widget->contextMenuPolicy() != Qt::NoContextMenu}
    };
}

QJsonObject itemViewSnapshot(QAbstractItemView* itemView) {
    if (!itemView) {
        return {};
    }

    const QAbstractItemModel* model = itemView->model();
    QJsonArray selectedRows;
    QJsonArray selectedIndexes;
    if (itemView->selectionModel()) {
        const QModelIndexList rows = itemView->selectionModel()->selectedRows();
        for (const QModelIndex& rowIndex : rows) {
            if (rowIndex.isValid()) {
                selectedRows.push_back(rowIndex.row());
            }
        }
        const QModelIndexList indexes = itemView->selectionModel()->selectedIndexes();
        for (const QModelIndex& index : indexes) {
            if (!index.isValid()) {
                continue;
            }
            selectedIndexes.push_back(QJsonObject{
                {QStringLiteral("row"), index.row()},
                {QStringLiteral("column"), index.column()}
            });
        }
    }

    const QModelIndex current = itemView->currentIndex();
    QJsonObject object{
        {QStringLiteral("rows"), model ? model->rowCount() : 0},
        {QStringLiteral("columns"), model ? model->columnCount() : 0},
        {QStringLiteral("currentRow"), current.isValid() ? current.row() : -1},
        {QStringLiteral("currentColumn"), current.isValid() ? current.column() : -1},
        {QStringLiteral("selectedRows"), selectedRows},
        {QStringLiteral("selectedIndexes"), selectedIndexes}
    };

    if (const auto* tableView = qobject_cast<QTableView*>(itemView)) {
        const auto* header = tableView->horizontalHeader();
        QJsonArray headers;
        for (int column = 0; header && column < header->count(); ++column) {
            headers.push_back(header->model()
                                  ? header->model()->headerData(column, Qt::Horizontal).toString()
                                  : QString());
        }
        object[QStringLiteral("headers")] = headers;
    }

    if (const auto* scrollArea = qobject_cast<QAbstractScrollArea*>(itemView)) {
        object[QStringLiteral("viewport")] = QJsonObject{
            {QStringLiteral("width"), scrollArea->viewport() ? scrollArea->viewport()->width() : 0},
            {QStringLiteral("height"), scrollArea->viewport() ? scrollArea->viewport()->height() : 0}
        };
    }

    return object;
}

QJsonObject widgetSnapshotRecursive(QWidget* widget,
                                    const QString& path,
                                    int absX,
                                    int absY,
                                    int indexInParent,
                                    const QString& parentPath = QString()) {
    if (!widget) {
        return {};
    }

    const QString role = widgetRole(widget);
    const QString stableId = stableWidgetId(widget, path);
    const bool clickable = qobject_cast<QAbstractButton*>(widget) != nullptr ||
                           qobject_cast<QSlider*>(widget) != nullptr;

    QJsonObject object{
        {QStringLiteral("class"), QString::fromLatin1(widget->metaObject()->className())},
        {QStringLiteral("id"), widget->objectName()},
        {QStringLiteral("stable_id"), stableId},
        {QStringLiteral("role"), role},
        {QStringLiteral("visible"), widget->isVisible()},
        {QStringLiteral("enabled"), widget->isEnabled()},
        {QStringLiteral("x"), widget->x()},
        {QStringLiteral("y"), widget->y()},
        {QStringLiteral("abs_x"), absX},
        {QStringLiteral("abs_y"), absY},
        {QStringLiteral("width"), widget->width()},
        {QStringLiteral("height"), widget->height()},
        {QStringLiteral("minimum_width"), widget->minimumWidth()},
        {QStringLiteral("minimum_height"), widget->minimumHeight()},
        {QStringLiteral("maximum_width"), widget->maximumWidth()},
        {QStringLiteral("maximum_height"), widget->maximumHeight()},
        {QStringLiteral("path"), path},
        {QStringLiteral("parent_path"), parentPath},
        {QStringLiteral("index"), indexInParent},
        {QStringLiteral("clickable"), clickable},
        {QStringLiteral("state"), widgetStateSnapshot(widget)},
        {QStringLiteral("geometry"), widgetGeometrySnapshot(widget, absX, absY)}
    };

    const QJsonArray actions = widgetActionsSnapshot(widget);
    object[QStringLiteral("action_count")] = actions.size();
    if (!actions.isEmpty()) {
        object[QStringLiteral("actions")] = actions;
    }

    if (auto* button = qobject_cast<QAbstractButton*>(widget)) {
        object[QStringLiteral("text")] = button->text();
        object[QStringLiteral("checked")] = button->isChecked();
    } else if (auto* slider = qobject_cast<QSlider*>(widget)) {
        object[QStringLiteral("value")] = slider->value();
        object[QStringLiteral("minimum")] = slider->minimum();
        object[QStringLiteral("maximum")] = slider->maximum();
    } else if (auto* spinBox = qobject_cast<QSpinBox*>(widget)) {
        object[QStringLiteral("value")] = spinBox->value();
        object[QStringLiteral("minimum")] = spinBox->minimum();
        object[QStringLiteral("maximum")] = spinBox->maximum();
    } else if (auto* doubleSpinBox = qobject_cast<QDoubleSpinBox*>(widget)) {
        object[QStringLiteral("value")] = doubleSpinBox->value();
        object[QStringLiteral("minimum")] = doubleSpinBox->minimum();
        object[QStringLiteral("maximum")] = doubleSpinBox->maximum();
    } else if (auto* tabWidget = qobject_cast<QTabWidget*>(widget)) {
        object[QStringLiteral("currentIndex")] = tabWidget->currentIndex();
        object[QStringLiteral("count")] = tabWidget->count();
        object[QStringLiteral("currentText")] =
            tabWidget->tabText(qBound(0, tabWidget->currentIndex(), qMax(0, tabWidget->count() - 1)));
        QJsonArray tabs;
        for (int i = 0; i < tabWidget->count(); ++i) {
            QWidget* page = tabWidget->widget(i);
            tabs.push_back(QJsonObject{
                {QStringLiteral("index"), i},
                {QStringLiteral("label"), tabWidget->tabText(i)},
                {QStringLiteral("enabled"), tabWidget->isTabEnabled(i)},
                {QStringLiteral("pageId"), page ? page->objectName() : QString()},
                {QStringLiteral("selected"), i == tabWidget->currentIndex()}
            });
        }
        object[QStringLiteral("tabs")] = tabs;
    } else if (auto* comboBox = qobject_cast<QComboBox*>(widget)) {
        object[QStringLiteral("currentIndex")] = comboBox->currentIndex();
        object[QStringLiteral("currentText")] = comboBox->currentText();
        object[QStringLiteral("count")] = comboBox->count();
        QJsonArray items;
        for (int i = 0; i < comboBox->count(); ++i) {
            items.push_back(comboBox->itemText(i));
        }
        object[QStringLiteral("items")] = items;
    } else if (auto* lineEdit = qobject_cast<QLineEdit*>(widget)) {
        object[QStringLiteral("text")] = lineEdit->text();
        object[QStringLiteral("placeholder")] = lineEdit->placeholderText();
        object[QStringLiteral("readOnly")] = lineEdit->isReadOnly();
    } else if (auto* plainTextEdit = qobject_cast<QPlainTextEdit*>(widget)) {
        object[QStringLiteral("text")] = plainTextEdit->toPlainText();
        object[QStringLiteral("readOnly")] = plainTextEdit->isReadOnly();
    } else if (auto* tableWidget = qobject_cast<QTableWidget*>(widget)) {
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
    } else if (auto* itemView = qobject_cast<QAbstractItemView*>(widget)) {
        const QJsonObject view = itemViewSnapshot(itemView);
        for (auto it = view.begin(); it != view.end(); ++it) {
            object.insert(it.key(), it.value());
        }
    } else if (auto* splitter = qobject_cast<QSplitter*>(widget)) {
        object[QStringLiteral("orientation")] = orientationName(splitter->orientation());
        object[QStringLiteral("handleWidth")] = splitter->handleWidth();
        QJsonArray sizes;
        const QList<int> splitterSizes = splitter->sizes();
        for (int size : splitterSizes) {
            sizes.push_back(size);
        }
        object[QStringLiteral("sizes")] = sizes;
    } else if (auto* scrollArea = qobject_cast<QScrollArea*>(widget)) {
        object[QStringLiteral("widgetResizable")] = scrollArea->widgetResizable();
        if (QWidget* content = scrollArea->widget()) {
            object[QStringLiteral("content_id")] = content->objectName();
            object[QStringLiteral("content_class")] =
                QString::fromLatin1(content->metaObject()->className());
        }
    } else {
        object[QStringLiteral("clickable")] = clickable;
    }

    QJsonArray children;
    const auto childWidgets = widget->findChildren<QWidget*>(QString(), Qt::FindDirectChildrenOnly);
    object[QStringLiteral("child_count")] = childWidgets.size();
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
                                               childIndex,
                                               path));
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

QString normalizedActionText(const QString& text) {
    QString normalized = text;
    normalized.remove('&');
    return normalized.trimmed();
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
            const QJsonObject actionObject = menuActionSnapshot(action);
            if (!actionObject.isEmpty()) {
                actions.append(actionObject);
            }
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

QStringList menuActionPathFromJson(const QJsonObject& body,
                                   const QString& textKey,
                                   const QString& pathKey) {
    QStringList path;
    const QJsonArray pathArray = body.value(pathKey).toArray();
    for (const QJsonValue& value : pathArray) {
        const QString segment = value.toString().trimmed();
        if (!segment.isEmpty()) {
            path.push_back(segment);
        }
    }
    if (!path.isEmpty()) {
        return path;
    }

    const QString single = body.value(textKey).toString().trimmed();
    if (!single.isEmpty()) {
        path.push_back(single);
    }
    return path;
}

QAction* findMenuAction(QMenu* menu,
                        const QString& text,
                        bool contains,
                        Qt::CaseSensitivity cs) {
    if (!menu) {
        return nullptr;
    }
    const QString wanted = normalizedActionText(text);
    for (QAction* action : menu->actions()) {
        if (!action || action->isSeparator()) {
            continue;
        }
        const QString candidate = normalizedActionText(action->text());
        const bool matched = contains
            ? candidate.contains(wanted, cs)
            : QString::compare(candidate, wanted, cs) == 0;
        if (matched) {
            return action;
        }
    }
    return nullptr;
}

QAction* findMenuActionByPath(QMenu* menu,
                              const QStringList& path,
                              QString* errorOut,
                              QMenu** owningMenuOut) {
    if (owningMenuOut) {
        *owningMenuOut = nullptr;
    }
    if (!menu) {
        if (errorOut) {
            *errorOut = QStringLiteral("no active popup menu");
        }
        return nullptr;
    }
    if (path.isEmpty()) {
        if (errorOut) {
            *errorOut = QStringLiteral("empty menu action path");
        }
        return nullptr;
    }

    QMenu* currentMenu = menu;
    QAction* matchedAction = nullptr;
    for (int i = 0; i < path.size(); ++i) {
        matchedAction = findMenuAction(currentMenu, path.at(i), false, Qt::CaseSensitive);
        if (!matchedAction) {
            matchedAction = findMenuAction(currentMenu, path.at(i), false, Qt::CaseInsensitive);
        }
        if (!matchedAction) {
            if (errorOut) {
                *errorOut = QStringLiteral("menu action not found: %1").arg(path.at(i));
            }
            return nullptr;
        }
        if (i + 1 < path.size()) {
            currentMenu = matchedAction->menu();
            if (!currentMenu) {
                if (errorOut) {
                    *errorOut = QStringLiteral("menu action has no submenu: %1").arg(path.at(i));
                }
                return nullptr;
            }
        }
    }

    if (owningMenuOut) {
        *owningMenuOut = currentMenu;
    }
    return matchedAction;
}

QJsonObject resolveSelectedClipState(const QJsonObject& stateObj) {
    const QJsonObject directClip = stateObj.value(QStringLiteral("selectedClip")).toObject();
    const QString selectedClipId = stateObj.value(QStringLiteral("selectedClipId")).toString().trimmed();
    const QJsonArray selectedClipIds = stateObj.value(QStringLiteral("selectedClipIds")).toArray();
    const QJsonArray timeline = stateObj.value(QStringLiteral("timeline")).toArray();

    auto clipById = [&timeline](const QString& id) -> QJsonObject {
        if (id.trimmed().isEmpty()) {
            return {};
        }
        for (const QJsonValue& value : timeline) {
            const QJsonObject clipObj = value.toObject();
            if (clipObj.value(QStringLiteral("id")).toString().trimmed() == id.trimmed()) {
                return clipObj;
            }
        }
        return {};
    };

    QString resolvedId;
    QJsonObject resolvedClip;
    QString source = QStringLiteral("none");

    const QString directId = directClip.value(QStringLiteral("id")).toString().trimmed();
    if (!directClip.isEmpty() && !directId.isEmpty()) {
        resolvedClip = directClip;
        resolvedId = directId;
        source = QStringLiteral("selectedClip");
    }

    if (resolvedClip.isEmpty() && !selectedClipId.isEmpty()) {
        const QJsonObject bySelectedId = clipById(selectedClipId);
        if (!bySelectedId.isEmpty()) {
            resolvedClip = bySelectedId;
            resolvedId = selectedClipId;
            source = QStringLiteral("selectedClipId");
        }
    }

    if (resolvedClip.isEmpty() && !selectedClipIds.isEmpty()) {
        const QString firstSelectedId = selectedClipIds.at(0).toString().trimmed();
        const QJsonObject bySelectedIds = clipById(firstSelectedId);
        if (!bySelectedIds.isEmpty()) {
            resolvedClip = bySelectedIds;
            resolvedId = firstSelectedId;
            source = QStringLiteral("selectedClipIds");
        }
    }

    if (resolvedClip.isEmpty() && !timeline.isEmpty()) {
        const QJsonObject firstClip = timeline.at(0).toObject();
        const QString firstId = firstClip.value(QStringLiteral("id")).toString().trimmed();
        if (!firstClip.isEmpty() && !firstId.isEmpty()) {
            resolvedClip = firstClip;
            resolvedId = firstId;
            source = QStringLiteral("timeline_first");
        }
    }

    bool consistent = true;
    if (!selectedClipId.isEmpty() && !resolvedId.isEmpty() && selectedClipId != resolvedId) {
        consistent = false;
    }
    if (!directClip.isEmpty() && !directId.isEmpty() && !resolvedId.isEmpty() && directId != resolvedId) {
        consistent = false;
    }

    return QJsonObject{
        {QStringLiteral("selectedClip"), resolvedClip},
        {QStringLiteral("selectedClipId"), resolvedId},
        {QStringLiteral("selectedClipResolutionSource"), source},
        {QStringLiteral("selectedClipResolutionConsistent"), consistent}
    };
}

} // namespace control_server
