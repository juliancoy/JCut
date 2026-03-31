#pragma once

#include "editor_shared.h"
#include "frame_handle.h"

#include <QElapsedTimer>
#include <QString>
#include <functional>

namespace editor {

QElapsedTimer& decodeTraceTimer();
qint64 decodeTraceMs();
QString shortPath(const QString& path);
bool linuxNvidiaDetected();
bool zeroCopyInteropSupportedForCurrentBuild();
void decodeTrace(const QString& stage, const QString& detail = QString());
void invokeRequestCallback(std::function<void(FrameHandle)> callback, FrameHandle frame);

} // namespace editor
