#include "control_server_ui_utils.h"

#include <QAbstractButton>
#include <QAction>
#include <QApplication>
#include <QContextMenuEvent>
#include <QMenu>
#include <QMouseEvent>
#include <QSlider>

namespace control_server {

QJsonObject widgetSnapshot(QWidget* widget) {
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
        {QStringLiteral("width"), widget->width()},
        {QStringLiteral("height"), widget->height()}
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
    } else {
        object[QStringLiteral("clickable")] = false;
    }

    QJsonArray children;
    const auto childWidgets = widget->findChildren<QWidget*>(QString(), Qt::FindDirectChildrenOnly);
    for (QWidget* child : childWidgets) {
        children.append(widgetSnapshot(child));
    }
    object[QStringLiteral("children")] = children;
    return object;
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
