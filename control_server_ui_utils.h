#pragma once

#include <QJsonArray>
#include <QJsonObject>
#include <QAction>
#include <QMenu>
#include <QPoint>
#include <QString>
#include <QStringList>
#include <QWidget>
#include <Qt>

namespace control_server {

QJsonObject widgetSnapshot(QWidget* widget);
QJsonObject topLevelWindowSnapshot(QWidget* widget);
QJsonArray topLevelWindowsSnapshot();
QWidget* findWidgetByObjectName(QWidget* root, const QString& objectName);
QWidget* findWidgetByHierarchyPath(QWidget* root, const QString& path);
Qt::MouseButton parseMouseButton(const QString& value);
QString normalizedActionText(const QString& text);
bool sendSyntheticClick(QWidget* window, const QPoint& pos, Qt::MouseButton button);
bool sendSyntheticClick(QWidget* window, const QPoint& pos);
QJsonObject menuSnapshot(QMenu* menu);
QMenu* activePopupMenu();
QStringList menuActionPathFromJson(const QJsonObject& body,
                                   const QString& textKey = QStringLiteral("text"),
                                   const QString& pathKey = QStringLiteral("path"));
QAction* findMenuAction(QMenu* menu,
                        const QString& text,
                        bool contains = false,
                        Qt::CaseSensitivity cs = Qt::CaseInsensitive);
QAction* findMenuActionByPath(QMenu* menu,
                              const QStringList& path,
                              QString* errorOut = nullptr,
                              QMenu** owningMenuOut = nullptr);
QJsonObject resolveSelectedClipState(const QJsonObject& stateObj);

} // namespace control_server
