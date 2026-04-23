#pragma once

#include <QByteArray>
#include <QJsonObject>
#include <QString>
#include <QUrl>
#include <QUrlQuery>

#include <optional>

namespace control_server {

struct Request {
    QString method;
    QString path;
    QUrl url;
    QByteArray body;
};

bool queryBool(const QUrlQuery& query, const QString& key);
QString reasonPhrase(int statusCode);
QByteArray jsonBytes(const QJsonObject& object);
QJsonObject parseJsonObject(const QByteArray& body, QString* error);
std::optional<Request> tryParseRequest(const QByteArray& data);

} // namespace control_server
