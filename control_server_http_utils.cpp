#include "control_server_http_utils.h"

#include <QJsonDocument>
#include <QJsonParseError>

namespace control_server {

bool queryBool(const QUrlQuery& query, const QString& key) {
    const QString value = query.queryItemValue(key).trimmed().toLower();
    return value == QStringLiteral("1") || value == QStringLiteral("true") ||
           value == QStringLiteral("yes") || value == QStringLiteral("on");
}

QString reasonPhrase(int statusCode) {
    switch (statusCode) {
    case 200: return QStringLiteral("OK");
    case 400: return QStringLiteral("Bad Request");
    case 404: return QStringLiteral("Not Found");
    case 405: return QStringLiteral("Method Not Allowed");
    case 429: return QStringLiteral("Too Many Requests");
    case 408: return QStringLiteral("Request Timeout");
    case 500: return QStringLiteral("Internal Server Error");
    case 503: return QStringLiteral("Service Unavailable");
    default: return QStringLiteral("Status");
    }
}

QByteArray jsonBytes(const QJsonObject& object) {
    return QJsonDocument(object).toJson(QJsonDocument::Compact);
}

QJsonObject parseJsonObject(const QByteArray& body, QString* error) {
    QJsonParseError parseError;
    const QJsonDocument document = QJsonDocument::fromJson(body, &parseError);
    if (parseError.error != QJsonParseError::NoError || !document.isObject()) {
        if (error) {
            *error = parseError.error != QJsonParseError::NoError
                ? parseError.errorString()
                : QStringLiteral("request body must be a JSON object");
        }
        return {};
    }
    return document.object();
}

std::optional<Request> tryParseRequest(const QByteArray& data) {
    const int headerEnd = data.indexOf("\r\n\r\n");
    if (headerEnd < 0) {
        return std::nullopt;
    }

    const QList<QByteArray> lines = data.left(headerEnd).split('\n');
    if (lines.isEmpty()) {
        return std::nullopt;
    }

    const QList<QByteArray> requestLine = lines.first().trimmed().split(' ');
    if (requestLine.size() < 2) {
        return std::nullopt;
    }

    int contentLength = 0;
    for (int i = 1; i < lines.size(); ++i) {
        const QByteArray line = lines.at(i).trimmed();
        const int colon = line.indexOf(':');
        if (colon <= 0) {
            continue;
        }
        const QByteArray key = line.left(colon).trimmed().toLower();
        const QByteArray value = line.mid(colon + 1).trimmed();
        if (key == "content-length") {
            contentLength = value.toInt();
        }
    }

    const int totalSize = headerEnd + 4 + contentLength;
    if (data.size() < totalSize) {
        return std::nullopt;
    }

    Request request;
    request.method = QString::fromLatin1(requestLine.at(0));
    request.path = QString::fromLatin1(requestLine.at(1));
    request.url = QUrl(QStringLiteral("http://127.0.0.1") + request.path);
    request.body = data.mid(headerEnd + 4, contentLength);
    return request;
}

} // namespace control_server
