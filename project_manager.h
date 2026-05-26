#pragma once

#include <QObject>
#include <QString>
#include <QStringList>
#include <QByteArray>
#include <QJsonArray>
#include <functional>

class ProjectManager : public QObject
{
    Q_OBJECT

public:
    explicit ProjectManager(QObject *parent = nullptr);
    ~ProjectManager() override = default;

    // Root directory configuration (stable app config, overridable via JCUT_PROJECT_ROOT).
    QString applicationDirPath() const;
    QString configFilePath() const;
    QString legacyConfigFilePath() const;
    QString rootDirPath() const;
    void setRootDirPath(const QString& path);
    
    // Project path helpers
    QString projectsDirPath() const;
    QString currentProjectMarkerPath() const;
    QString currentProjectIdOrDefault() const;
    QString projectPath(const QString &projectId) const;
    QString stateFilePathForProject(const QString &projectId) const;
    QString historyFilePathForProject(const QString &projectId) const;
    QString stateFilePath() const;
    QString historyFilePath() const;
    
    QString sanitizedProjectId(const QString &name) const;
    void ensureProjectsDirectory() const;
    QStringList availableProjectIds() const;
    void ensureDefaultProjectExists() const;
    
    void loadProjectsFromFolders();
    void saveCurrentProjectMarker();
    QString currentProjectName() const;
    void refreshProjectsList();
    void switchToProject(const QString &projectId);
    void createProject();
    bool saveProjectPayload(const QString &projectId, const QByteArray &statePayload, const QByteArray &historyPayload);
    void saveProjectAs(const QString &currentName, std::function<QByteArray()> buildStateJson, 
                       const QJsonArray &historyEntries, int historyIndex);
    void renameProject(const QString &projectId);

signals:
    void projectChanged(const QString &projectId);
    void projectsListRefreshed();

private:
    QString defaultRootDirPath() const;
    QString normalizedExistingDirPath(const QString &path) const;
    void synchronizeRootIfNeeded() const;
    QString currentProjectIdOrDefaultWithoutSync() const;
    QString projectPathWithoutSync(const QString &projectId) const;

    QString m_currentProjectId;
    mutable QString m_loadedRootDirPath;
};
