#pragma once

#include <QDataStream>
#include <QFile>
#include <QJsonArray>
#include <QJsonObject>
#include <QJsonValue>
#include <QString>

#include <nlohmann/json.hpp>

#include <cmath>
#include <cstdint>
#include <limits>

namespace jcut::jsonio {

using Json = nlohmann::json;

inline Json toJson(const QJsonValue& value)
{
    switch (value.type()) {
    case QJsonValue::Null:
    case QJsonValue::Undefined:
        return nullptr;
    case QJsonValue::Bool:
        return value.toBool();
    case QJsonValue::Double: {
        const double number = value.toDouble();
        if (std::isfinite(number) &&
            std::floor(number) == number &&
            number >= static_cast<double>(std::numeric_limits<long long>::min()) &&
            number <= static_cast<double>(std::numeric_limits<long long>::max())) {
            return static_cast<long long>(number);
        }
        return number;
    }
    case QJsonValue::String:
        return value.toString().toStdString();
    case QJsonValue::Array: {
        Json out = Json::array();
        const QJsonArray array = value.toArray();
        for (const QJsonValue& element : array) {
            out.push_back(toJson(element));
        }
        return out;
    }
    case QJsonValue::Object: {
        Json out = Json::object();
        const QJsonObject object = value.toObject();
        for (auto it = object.begin(); it != object.end(); ++it) {
            out[it.key().toStdString()] = toJson(it.value());
        }
        return out;
    }
    }
    return nullptr;
}

inline QJsonValue fromJson(const Json& value)
{
    if (value.is_null()) {
        return QJsonValue();
    }
    if (value.is_boolean()) {
        return QJsonValue(value.get<bool>());
    }
    if (value.is_number_integer()) {
        return QJsonValue(static_cast<qint64>(value.get<long long>()));
    }
    if (value.is_number_unsigned()) {
        return QJsonValue(static_cast<qint64>(value.get<unsigned long long>()));
    }
    if (value.is_number_float()) {
        return QJsonValue(value.get<double>());
    }
    if (value.is_string()) {
        return QJsonValue(QString::fromStdString(value.get<std::string>()));
    }
    if (value.is_array()) {
        QJsonArray array;
        for (const Json& element : value) {
            array.append(fromJson(element));
        }
        return array;
    }
    if (value.is_object()) {
        QJsonObject object;
        for (auto it = value.begin(); it != value.end(); ++it) {
            object.insert(QString::fromStdString(it.key()), fromJson(it.value()));
        }
        return object;
    }
    return QJsonValue();
}

inline QByteArray serializeCompact(const QJsonObject& object)
{
    const Json json = toJson(object);
    const std::string dumped = json.dump();
    return QByteArray(dumped.data(), static_cast<int>(dumped.size()));
}

inline QByteArray serializeIndented(const QJsonObject& object)
{
    const Json json = toJson(object);
    const std::string dumped = json.dump(4);
    return QByteArray(dumped.data(), static_cast<int>(dumped.size()));
}

inline bool parseObjectBytes(const QByteArray& payload, QJsonObject* objectOut, QString* errorOut = nullptr)
{
    if (objectOut) {
        *objectOut = QJsonObject{};
    }
    try {
        const Json json = Json::parse(payload.constData(), payload.constData() + payload.size());
        if (!json.is_object()) {
            if (errorOut) {
                *errorOut = QStringLiteral("JSON payload root is not an object.");
            }
            return false;
        }
        if (objectOut) {
            *objectOut = fromJson(json).toObject();
        }
        return true;
    } catch (const std::exception& ex) {
        if (errorOut) {
            *errorOut = QStringLiteral("Failed to parse JSON: %1")
                            .arg(QString::fromLocal8Bit(ex.what()));
        }
        return false;
    }
}

inline bool writeJsonFile(const QString& path,
                          const QJsonObject& object,
                          bool pretty = true,
                          QString* errorOut = nullptr)
{
    QFile file(path);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        if (errorOut) {
            *errorOut = QStringLiteral("Failed to open %1 for writing.").arg(path);
        }
        return false;
    }
    const QByteArray payload = pretty ? serializeIndented(object) : serializeCompact(object);
    if (file.write(payload) != payload.size()) {
        if (errorOut) {
            *errorOut = QStringLiteral("Failed to write JSON file %1.").arg(path);
        }
        return false;
    }
    return true;
}

inline bool readJsonFile(const QString& path, QJsonObject* objectOut, QString* errorOut = nullptr)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        if (errorOut) {
            *errorOut = QStringLiteral("Failed to open %1").arg(path);
        }
        return false;
    }
    return parseObjectBytes(file.readAll(), objectOut, errorOut);
}

inline bool writeBinaryJsonObject(const QString& path,
                                  const QJsonObject& object,
                                  quint32 magic = 0x4A435554,
                                  quint32 version = 1,
                                  QString* errorOut = nullptr)
{
    QFile file(path);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        if (errorOut) {
            *errorOut = QStringLiteral("Failed to open %1 for writing.").arg(path);
        }
        return false;
    }
    QDataStream stream(&file);
    stream.setVersion(QDataStream::Qt_6_0);
    stream << magic;
    stream << version;
    stream << qCompress(serializeCompact(object), 6);
    if (stream.status() != QDataStream::Ok) {
        if (errorOut) {
            *errorOut = QStringLiteral("Failed to write binary JSON object to %1.").arg(path);
        }
        return false;
    }
    return true;
}

inline bool readBinaryJsonObject(const QString& path,
                                 QJsonObject* objectOut,
                                 quint32 expectedMagic = 0x4A435554,
                                 quint32 expectedVersion = 1,
                                 QString* errorOut = nullptr)
{
    if (objectOut) {
        *objectOut = QJsonObject{};
    }
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        if (errorOut) {
            *errorOut = QStringLiteral("Failed to open %1").arg(path);
        }
        return false;
    }
    QDataStream stream(&file);
    stream.setVersion(QDataStream::Qt_6_0);
    quint32 magic = 0;
    quint32 version = 0;
    QByteArray compressed;
    stream >> magic;
    stream >> version;
    stream >> compressed;
    if (stream.status() != QDataStream::Ok || magic != expectedMagic || version != expectedVersion) {
        if (errorOut) {
            *errorOut = QStringLiteral("Invalid binary artifact header in %1").arg(path);
        }
        return false;
    }
    return parseObjectBytes(qUncompress(compressed), objectOut, errorOut);
}

inline bool appendBinaryJsonRecord(QFile* file,
                                   const QJsonObject& object,
                                   quint32 magic = 0x4A465352,
                                   quint32 version = 1,
                                   QString* errorOut = nullptr)
{
    if (!file || !file->isOpen()) {
        if (errorOut) {
            *errorOut = QStringLiteral("Binary record file handle is not open.");
        }
        return false;
    }
    const QByteArray compressed = qCompress(serializeCompact(object), 6);
    QDataStream stream(file);
    stream.setVersion(QDataStream::Qt_6_0);
    stream << magic;
    stream << version;
    stream << quint32(compressed.size());
    if (!compressed.isEmpty()) {
        const qint64 written = file->write(compressed);
        if (written != compressed.size()) {
            if (errorOut) {
                *errorOut = QStringLiteral("Failed to append binary JSON record.");
            }
            return false;
        }
    }
    if (stream.status() != QDataStream::Ok || !file->flush()) {
        if (errorOut) {
            *errorOut = QStringLiteral("Failed to finalize binary JSON record append.");
        }
        return false;
    }
    return true;
}

inline bool parseRecordPayload(const QByteArray& payload, QJsonObject* objectOut, QString* errorOut = nullptr)
{
    return parseObjectBytes(payload, objectOut, errorOut);
}

} // namespace jcut::jsonio
