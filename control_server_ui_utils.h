#pragma once

#include <QJsonArray>
#include <QJsonObject>
#include <QMenu>
#include <QPoint>
#include <QString>
#include <QWidget>
#include <Qt>

namespace control_server {

QJsonObject widgetSnapshot(QWidget* widget);
QJsonObject topLevelWindowSnapshot(QWidget* widget);
QJsonArray topLevelWindowsSnapshot();
QWidget* findWidgetByObjectName(QWidget* root, const QString& objectName);
QWidget* findWidgetByHierarchyPath(QWidget* root, const QString& path);
Qt::MouseButton parseMouseButton(const QString& value);
bool sendSyntheticClick(QWidget* window, const QPoint& pos, Qt::MouseButton button);
bool sendSyntheticClick(QWidget* window, const QPoint& pos);
QJsonObject menuSnapshot(QMenu* menu);
QMenu* activePopupMenu();
QJsonObject resolveSelectedClipState(const QJsonObject& stateObj);

} // namespace control_server
