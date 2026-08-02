#pragma once

#include <QtGlobal>

class QProcess;

namespace jcut::jobs {

// Configure a QProcess before start() so the worker and all descendants live
// in a process group that can be canceled without signaling JCut itself.
void isolateQProcessTree(QProcess* process);

// Signal the isolated process group. The QProcess fallback is used on
// platforms without Unix process groups.
void terminateQProcessTree(QProcess* process, qint64 processTreeId);
void killQProcessTree(QProcess* process, qint64 processTreeId);

bool qProcessTreeExists(qint64 processTreeId);

} // namespace jcut::jobs
