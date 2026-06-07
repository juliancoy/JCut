#pragma once

#include <QJsonObject>

class QFile;
class QString;

bool writeJson(const QString &path, const QJsonObject &object);
bool writeBinaryJsonObject(const QString &path, const QJsonObject &object);
bool appendBinaryCborRecord(QFile *file, const QJsonObject &object);
