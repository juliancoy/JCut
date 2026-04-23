#pragma once

#include <QJsonObject>

namespace control_server {

QJsonObject enrichClipForApi(const QJsonObject& clipObject);
QJsonObject benchmarkDecodeRatesForState(const QJsonObject& state);
QJsonObject benchmarkSeekRatesForState(const QJsonObject& state);

} // namespace control_server
