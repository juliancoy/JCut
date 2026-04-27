#pragma once

#include <QPixmap>
#include <QString>

QString normalizeBaseUrl(QString value);
QPixmap buildFallbackAvatar(const QString& identity);
