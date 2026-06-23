#include "project_manager.h"
#include "json_io_utils.h"
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QSaveFile>
#include <QJsonObject>
#include <QJsonArray>
#include <QInputDialog>
#include <QMessageBox>
#include <QRegularExpression>
#include <QStandardPaths>
#include <QCoreApplication>

namespace {

QString writeConfigPath(const QString &configPath, const QString &payload)
{
    if (configPath.isEmpty()) {
        return QStringLiteral("empty config path");
    }
    const QFileInfo configInfo(configPath);
    QDir().mkpath(configInfo.dir().absolutePath());
    QSaveFile config(configPath);
    if (!config.open(QIODevice::WriteOnly | QIODevice::Truncate | QIODevice::Text)) {
        return config.errorString();
    }
    const QByteArray bytes = payload.toUtf8();
    if (config.write(bytes) != bytes.size()) {
        config.cancelWriting();
        return QStringLiteral("failed to write config payload");
    }
    if (!config.commit()) {
        return config.errorString();
    }
    return {};
}

} // namespace

ProjectManager::ProjectManager(QObject *parent)
    : QObject(parent)
{
}

void ProjectManager::synchronizeRootIfNeeded() const
{
    const QString effectiveRoot = rootDirPath();
    if (!m_currentProjectId.isEmpty() && m_loadedRootDirPath == effectiveRoot) {
        return;
    }
    const_cast<ProjectManager*>(this)->loadProjectsFromFolders();
}

QString ProjectManager::applicationDirPath() const
{
    // Get the directory where the executable is located
    return QCoreApplication::applicationDirPath();
}

QString ProjectManager::configFilePath() const
{
    const QString configRoot = QStandardPaths::writableLocation(QStandardPaths::GenericConfigLocation);
    return QDir(configRoot).filePath(QStringLiteral("PanelTalkEditor/editor.config"));
}

QString ProjectManager::legacyConfigFilePath() const
{
    return QDir(applicationDirPath()).filePath(QStringLiteral("editor.config"));
}

QString ProjectManager::normalizedExistingDirPath(const QString &path) const
{
    const QString trimmed = path.trimmed();
    if (trimmed.isEmpty()) {
        return {};
    }
    const QFileInfo info(trimmed);
    if (!info.exists() || !info.isDir()) {
        return {};
    }
    const QString canonical = info.canonicalFilePath();
    if (!canonical.isEmpty()) {
        return canonical;
    }
    return QDir::cleanPath(info.absoluteFilePath());
}

QString ProjectManager::defaultRootDirPath() const
{
    QString appDir = normalizedExistingDirPath(applicationDirPath());
    if (appDir.isEmpty()) {
        return QDir::currentPath();
    }

    // On macOS the executable lives inside the bundle
    // (<build>/jcut.app/Contents/MacOS); walk up out of the bundle so the
    // build-dir rule below sees <build>, not the bundle internals.
    // Otherwise the editor silently creates its projects root inside the
    // .app and never finds the repository's projects/.
    {
        QDir walker(appDir);
        for (int i = 0; i < 3; ++i) {
            const QString name = walker.dirName();
            const bool bundleComponent =
                name.compare(QStringLiteral("MacOS"), Qt::CaseInsensitive) == 0 ||
                name.compare(QStringLiteral("Contents"), Qt::CaseInsensitive) == 0 ||
                name.endsWith(QStringLiteral(".app"), Qt::CaseInsensitive);
            if (!bundleComponent || !walker.cdUp()) {
                break;
            }
        }
        const QString unbundled = normalizedExistingDirPath(walker.absolutePath());
        if (!unbundled.isEmpty()) {
            appDir = unbundled;
        }
    }

    const QFileInfo appDirInfo(appDir);
    const QString baseName = appDirInfo.fileName().trimmed().toLower();
    if (baseName == QStringLiteral("build") || baseName.startsWith(QStringLiteral("build-"))) {
        // Parent of the build directory is the repository root. (The old
        // expression appended an extra ".." and landed one level above the
        // repo; it never ran in practice because a config file existed.)
        const QString parentDir = normalizedExistingDirPath(appDirInfo.dir().absolutePath());
        if (!parentDir.isEmpty()) {
            return parentDir;
        }
    }
    return appDir;
}

QString ProjectManager::rootDirPath() const
{
    const QString envRoot = normalizedExistingDirPath(qEnvironmentVariable("JCUT_PROJECT_ROOT"));
    if (!envRoot.isEmpty()) {
        return envRoot;
    }

    auto readConfig = [this](const QString &path) -> QString {
        QFile configFile(path);
        if (!configFile.open(QIODevice::ReadOnly | QIODevice::Text)) {
            return {};
        }
        return normalizedExistingDirPath(QString::fromUtf8(configFile.readAll()));
    };

    const QString primaryRoot = readConfig(configFilePath());
    if (!primaryRoot.isEmpty()) {
        return primaryRoot;
    }

    const QString legacyRoot = readConfig(legacyConfigFilePath());
    if (!legacyRoot.isEmpty()) {
        writeConfigPath(configFilePath(), legacyRoot + QLatin1Char('\n'));
        return legacyRoot;
    }

    return defaultRootDirPath();
}

void ProjectManager::setRootDirPath(const QString& path)
{
    const QString normalizedPath = normalizedExistingDirPath(path);
    if (normalizedPath.isEmpty()) {
        return;
    }

    const QString payload = normalizedPath + QLatin1Char('\n');
    writeConfigPath(configFilePath(), payload);
    const QString legacyPath = legacyConfigFilePath();
    if (legacyPath != configFilePath()) {
        writeConfigPath(legacyPath, payload);
    }
}

bool ProjectManager::changeRootDirPath(const QString& path)
{
    const QString normalizedPath = normalizedExistingDirPath(path);
    if (normalizedPath.isEmpty()) {
        return false;
    }

    const QString previousRoot = rootDirPath();
    setRootDirPath(normalizedPath);
    loadDefaultProjectFromFolders();

    return QDir(previousRoot).absolutePath() != QDir(rootDirPath()).absolutePath();
}

QString ProjectManager::projectsDirPath() const
{
    // Projects are stored in a "projects" subfolder of the Root directory
    return QDir(rootDirPath()).filePath(QStringLiteral("projects"));
}

QString ProjectManager::currentProjectMarkerPath() const
{
    return QDir(projectsDirPath()).filePath(QStringLiteral(".current_project"));
}

QString ProjectManager::currentProjectIdOrDefault() const
{
    synchronizeRootIfNeeded();
    return currentProjectIdOrDefaultWithoutSync();
}

QString ProjectManager::projectPath(const QString &projectId) const
{
    synchronizeRootIfNeeded();
    return projectPathWithoutSync(projectId);
}

QString ProjectManager::stateFilePathForProject(const QString &projectId) const
{
    synchronizeRootIfNeeded();
    return QDir(projectPathWithoutSync(projectId)).filePath(QStringLiteral("state.json"));
}

QString ProjectManager::historyFilePathForProject(const QString &projectId) const
{
    synchronizeRootIfNeeded();
    return QDir(projectPathWithoutSync(projectId)).filePath(QStringLiteral("history.json"));
}

QString ProjectManager::stateFilePath() const
{
    return stateFilePathForProject(currentProjectIdOrDefault());
}

QString ProjectManager::historyFilePath() const
{
    return historyFilePathForProject(currentProjectIdOrDefault());
}

QString ProjectManager::sanitizedProjectId(const QString &name) const
{
    synchronizeRootIfNeeded();
    QString id = name.trimmed().toLower();
    for (QChar &ch : id) {
        if (!(ch.isLetterOrNumber() || ch == QLatin1Char('_') || ch == QLatin1Char('-'))) {
            ch = QLatin1Char('-');
        }
    }
    while (id.contains(QStringLiteral("--"))) {
        id.replace(QStringLiteral("--"), QStringLiteral("-"));
    }
    id.remove(QRegularExpression(QStringLiteral("^-+|-+$")));
    if (id.isEmpty()) {
        id = QStringLiteral("project");
    }
    QString uniqueId = id;
    int suffix = 2;
    while (QFileInfo::exists(projectPath(uniqueId))) {
        uniqueId = QStringLiteral("%1-%2").arg(id).arg(suffix++);
    }
    return uniqueId;
}

QString ProjectManager::currentProjectIdOrDefaultWithoutSync() const
{
    return m_currentProjectId.isEmpty() ? QStringLiteral("default") : m_currentProjectId;
}

QString ProjectManager::projectPathWithoutSync(const QString &projectId) const
{
    return QDir(projectsDirPath()).filePath(projectId.isEmpty() ? QStringLiteral("default") : projectId);
}

void ProjectManager::ensureProjectsDirectory() const
{
    QDir().mkpath(projectsDirPath());
}

QStringList ProjectManager::availableProjectIds() const
{
    ensureProjectsDirectory();
    const QFileInfoList entries = QDir(projectsDirPath()).entryInfoList(QDir::Dirs | QDir::NoDotAndDotDot, QDir::Name | QDir::IgnoreCase);
    QStringList ids;
    ids.reserve(entries.size());
    for (const QFileInfo &entry : entries) {
        ids.push_back(entry.fileName());
    }
    return ids;
}

void ProjectManager::ensureDefaultProjectExists() const
{
    ensureProjectsDirectory();
    QDir().mkpath(projectPathWithoutSync(QStringLiteral("default")));
}

void ProjectManager::loadProjectsFromFolders()
{
    m_loadedRootDirPath = rootDirPath();
    ensureDefaultProjectExists();
    QFile markerFile(currentProjectMarkerPath());
    if (markerFile.open(QIODevice::ReadOnly)) {
        m_currentProjectId = QString::fromUtf8(markerFile.readAll()).trimmed();
    }
    const QStringList projectIds = availableProjectIds();
    if (projectIds.isEmpty()) {
        m_currentProjectId = QStringLiteral("default");
        return;
    }
    if (m_currentProjectId.isEmpty() || !projectIds.contains(m_currentProjectId)) {
        m_currentProjectId = projectIds.contains(QStringLiteral("default"))
                                 ? QStringLiteral("default")
                                 : projectIds.constFirst();
    }
    QSaveFile marker(currentProjectMarkerPath());
    if (marker.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        const QByteArray payload = m_currentProjectId.toUtf8();
        if (marker.write(payload) == payload.size()) {
            marker.commit();
        } else {
            marker.cancelWriting();
        }
    }
}

void ProjectManager::loadDefaultProjectFromFolders()
{
    m_loadedRootDirPath = rootDirPath();
    ensureDefaultProjectExists();
    m_currentProjectId = QStringLiteral("default");
    saveCurrentProjectMarker();
}

void ProjectManager::saveCurrentProjectMarker()
{
    ensureProjectsDirectory();
    QSaveFile file(currentProjectMarkerPath());
    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        return;
    }
    const QByteArray payload = currentProjectIdOrDefault().toUtf8();
    if (file.write(payload) != payload.size()) {
        file.cancelWriting();
        return;
    }
    file.commit();
}

QString ProjectManager::currentProjectName() const
{
    return currentProjectIdOrDefault();
}

void ProjectManager::refreshProjectsList()
{
    loadProjectsFromFolders();
    emit projectsListRefreshed();
}

void ProjectManager::switchToProject(const QString &projectId)
{
    synchronizeRootIfNeeded();
    if (projectId.isEmpty() || projectId == currentProjectIdOrDefault()) {
        refreshProjectsList();
        return;
    }
    
    m_currentProjectId = projectId;
    saveCurrentProjectMarker();
    emit projectChanged(projectId);
}

void ProjectManager::createProject()
{
    synchronizeRootIfNeeded();
    bool accepted = false;
    const QString name = QInputDialog::getText(nullptr,
                                               QStringLiteral("New Project"),
                                               QStringLiteral("Project name"),
                                               QLineEdit::Normal,
                                               QStringLiteral("Untitled Project"),
                                               &accepted).trimmed();
    if (!accepted || name.isEmpty()) {
        return;
    }
    const QString projectId = sanitizedProjectId(name);
    QDir().mkpath(projectPath(projectId));
    switchToProject(projectId);
}

bool ProjectManager::saveProjectPayload(const QString &projectId,
                                        const QByteArray &statePayload,
                                        const QByteArray &historyPayload)
{
    synchronizeRootIfNeeded();
    ensureProjectsDirectory();
    QDir().mkpath(projectPath(projectId));

    QSaveFile stateFile(stateFilePathForProject(projectId));
    if (!stateFile.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        return false;
    }
    if (stateFile.write(statePayload) != statePayload.size()) {
        stateFile.cancelWriting();
        return false;
    }
    if (!stateFile.commit()) {
        return false;
    }

    QSaveFile historyFile(historyFilePathForProject(projectId));
    if (!historyFile.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        return false;
    }
    if (historyFile.write(historyPayload) != historyPayload.size()) {
        historyFile.cancelWriting();
        return false;
    }
    return historyFile.commit();
}

void ProjectManager::saveProjectAs(const QString &currentName,
                                   std::function<QByteArray()> buildStateJson,
                                   const QJsonArray &historyEntries,
                                   int historyIndex)
{
    synchronizeRootIfNeeded();
    bool accepted = false;
    const QString name = QInputDialog::getText(nullptr,
                                               QStringLiteral("Save Project As"),
                                               QStringLiteral("Project name"),
                                               QLineEdit::Normal,
                                               currentName == QStringLiteral("Default Project")
                                                   ? QStringLiteral("Untitled Project")
                                                   : currentName,
                                               &accepted).trimmed();
    if (!accepted || name.isEmpty()) {
        return;
    }

    const QString newProjectId = sanitizedProjectId(name);
    const QByteArray statePayload = buildStateJson();
    QJsonObject historyRoot;
    historyRoot[QStringLiteral("index")] = historyIndex;
    historyRoot[QStringLiteral("entries")] = historyEntries;
    const QByteArray historyPayload = jcut::jsonio::serializeIndented(historyRoot);

    if (!saveProjectPayload(newProjectId, statePayload, historyPayload)) {
        QMessageBox::warning(nullptr,
                             QStringLiteral("Save Project As Failed"),
                             QStringLiteral("Could not write the new project files."));
        return;
    }

    switchToProject(newProjectId);
}

void ProjectManager::renameProject(const QString &projectId)
{
    synchronizeRootIfNeeded();
    if (projectId.isEmpty() || !QFileInfo::exists(projectPath(projectId))) {
        return;
    }
    bool accepted = false;
    const QString name = QInputDialog::getText(nullptr,
                                               QStringLiteral("Rename Project"),
                                               QStringLiteral("Project name"),
                                               QLineEdit::Normal,
                                               projectId,
                                               &accepted).trimmed();
    if (!accepted || name.isEmpty()) {
        return;
    }
    const QString renamedProjectId = sanitizedProjectId(name);
    if (renamedProjectId == projectId) {
        return;
    }
    QDir projectsDir(projectsDirPath());
    if (!projectsDir.rename(projectId, renamedProjectId)) {
        QMessageBox::warning(nullptr,
                             QStringLiteral("Rename Project Failed"),
                             QStringLiteral("Could not rename the project folder."));
        return;
    }
    if (m_currentProjectId == projectId) {
        m_currentProjectId = renamedProjectId;
        saveCurrentProjectMarker();
    }
    refreshProjectsList();
}
