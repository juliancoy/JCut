#pragma once

#include <QString>

namespace editor::audio {

inline QString sourceKeySeparator()
{
    return QStringLiteral("::audio_stream=");
}

inline QString makeSourceKey(const QString& path, int streamIndex)
{
    if (streamIndex < 0) {
        return path;
    }
    return path + sourceKeySeparator() + QString::number(streamIndex);
}

inline QString pathFromSourceKey(const QString& key)
{
    const QString separator = sourceKeySeparator();
    const int marker = key.lastIndexOf(separator);
    return marker >= 0 ? key.left(marker) : key;
}

inline int streamIndexFromSourceKey(const QString& key)
{
    const QString separator = sourceKeySeparator();
    const int marker = key.lastIndexOf(separator);
    if (marker < 0) {
        return -1;
    }
    bool ok = false;
    const int streamIndex = key.mid(marker + separator.size()).toInt(&ok);
    return ok ? streamIndex : -1;
}

} // namespace editor::audio
