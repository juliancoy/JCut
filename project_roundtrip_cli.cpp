#include "project_manager.h"

#include <QCoreApplication>
#include <QCryptographicHash>
#include <QFile>
#include <QTextStream>

namespace {
QByteArray readFileOrEmpty(const QString &path, bool *ok)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        if (ok) {
            *ok = false;
        }
        return {};
    }
    if (ok) {
        *ok = true;
    }
    return file.readAll();
}

QString sha256Hex(const QByteArray &bytes)
{
    return QString::fromLatin1(QCryptographicHash::hash(bytes, QCryptographicHash::Sha256).toHex());
}
} // namespace

int main(int argc, char *argv[])
{
    QCoreApplication app(argc, argv);
    QTextStream out(stdout);
    QTextStream err(stderr);

    QString projectId = QStringLiteral("suhas");
    QString rootOverride;
    const QStringList args = app.arguments();
    if (args.size() >= 2) {
        projectId = args.at(1).trimmed();
    }
    if (args.size() >= 3) {
        rootOverride = args.at(2).trimmed();
    }
    if (projectId.isEmpty()) {
        err << "Project id cannot be empty.\n";
        return 2;
    }

    ProjectManager pm;
    if (!rootOverride.isEmpty()) {
        pm.setRootDirPath(rootOverride);
    }
    pm.loadProjectsFromFolders();

    const QString statePath = pm.stateFilePathForProject(projectId);
    const QString historyPath = pm.historyFilePathForProject(projectId);

    bool stateReadOk = false;
    bool historyReadOk = false;
    const QByteArray stateBefore = readFileOrEmpty(statePath, &stateReadOk);
    const QByteArray historyBefore = readFileOrEmpty(historyPath, &historyReadOk);
    if (!stateReadOk || !historyReadOk) {
        err << "Failed to read input files.\n"
            << "state: " << statePath << "\n"
            << "history: " << historyPath << "\n";
        return 3;
    }

    if (!pm.saveProjectPayload(projectId, stateBefore, historyBefore)) {
        err << "saveProjectPayload failed for project '" << projectId << "'.\n";
        return 4;
    }

    bool stateAfterOk = false;
    bool historyAfterOk = false;
    const QByteArray stateAfter = readFileOrEmpty(statePath, &stateAfterOk);
    const QByteArray historyAfter = readFileOrEmpty(historyPath, &historyAfterOk);
    if (!stateAfterOk || !historyAfterOk) {
        err << "Failed to read output files.\n";
        return 5;
    }

    out << "project=" << projectId << "\n";
    out << "root=" << pm.rootDirPath() << "\n";
    out << "state_path=" << statePath << "\n";
    out << "history_path=" << historyPath << "\n";
    out << "state_size_before=" << stateBefore.size() << "\n";
    out << "state_size_after=" << stateAfter.size() << "\n";
    out << "state_sha256_before=" << sha256Hex(stateBefore) << "\n";
    out << "state_sha256_after=" << sha256Hex(stateAfter) << "\n";
    out << "history_size_before=" << historyBefore.size() << "\n";
    out << "history_size_after=" << historyAfter.size() << "\n";
    out << "history_sha256_before=" << sha256Hex(historyBefore) << "\n";
    out << "history_sha256_after=" << sha256Hex(historyAfter) << "\n";
    out << "state_changed=" << (stateBefore == stateAfter ? "no" : "yes") << "\n";
    out << "history_changed=" << (historyBefore == historyAfter ? "no" : "yes") << "\n";
    out.flush();

    return 0;
}
