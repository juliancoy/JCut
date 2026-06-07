#pragma once

#include <QDebug>
#include <QJsonDocument>
#include <QJsonObject>
#include <QString>

namespace jcut::facedetections {

inline bool debugLoggingEnabled()
{
    const QString value =
        qEnvironmentVariable("JCUT_FACEDETECTIONS_DEBUG").trimmed().toLower();
    return value == QStringLiteral("1") ||
           value == QStringLiteral("true") ||
           value == QStringLiteral("yes") ||
           value == QStringLiteral("on");
}

inline void debugLogJson(const QString& marker, const QJsonObject& payload)
{
    if (!debugLoggingEnabled()) {
        return;
    }
    qInfo().noquote()
        << QStringLiteral("[%1] %2")
               .arg(marker,
                    QString::fromUtf8(
                        QJsonDocument(payload).toJson(QJsonDocument::Compact)));
}

} // namespace jcut::facedetections
