#include "qt_process_tree.h"

#include <QProcess>

#ifdef Q_OS_UNIX
#include <cerrno>
#include <csignal>
#include <sys/types.h>
#include <unistd.h>
#endif

namespace jcut::jobs {

void isolateQProcessTree(QProcess* process)
{
    if (!process) return;
#ifdef Q_OS_UNIX
    process->setChildProcessModifier([]() {
        if (::setpgid(0, 0) != 0) _exit(127);
    });
#endif
}

void terminateQProcessTree(QProcess* process, qint64 processTreeId)
{
#ifdef Q_OS_UNIX
    if (processTreeId > 1) {
        (void)::kill(-static_cast<pid_t>(processTreeId), SIGTERM);
        return;
    }
#else
    Q_UNUSED(processTreeId);
#endif
    if (process) process->terminate();
}

void killQProcessTree(QProcess* process, qint64 processTreeId)
{
#ifdef Q_OS_UNIX
    if (processTreeId > 1) {
        (void)::kill(-static_cast<pid_t>(processTreeId), SIGKILL);
        return;
    }
#else
    Q_UNUSED(processTreeId);
#endif
    if (process) process->kill();
}

bool qProcessTreeExists(qint64 processTreeId)
{
#ifdef Q_OS_UNIX
    if (processTreeId <= 1) return false;
    errno = 0;
    return ::kill(-static_cast<pid_t>(processTreeId), 0) == 0 || errno == EPERM;
#else
    Q_UNUSED(processTreeId);
    return false;
#endif
}

} // namespace jcut::jobs
