#pragma once

#include "editor_shared.h"
#include <QElapsedTimer>
#include <QString>

namespace editor {

QElapsedTimer& decodeTraceTimer();
qint64 decodeTraceMs();
QString shortPath(const QString& path);
bool linuxNvidiaDetected();
bool zeroCopyInteropSupportedForCurrentBuild();
void decodeTrace(const QString& stage, const QString& detail = QString());
} // namespace editor
