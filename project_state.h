// project_state.h
#pragma once

#include <QByteArray>
#include <QJsonObject>
#include <QString>
#include <QStringList>

// Paste these into the EditorWindow class declaration in editor.h.

void loadState();

// Root directory configuration (stored near executable in editor.config)
QString configFilePath() const;
QString rootDirPath() const;
void setRootDirPath(const QString& path);

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
bool saveProjectPayload(const QString &projectId,
                        const QByteArray &statePayload,
                        const QByteArray &historyPayload);
void saveProjectAs();
void renameProject(const QString &projectId);

QJsonObject buildStateJson() const;
void scheduleSaveState();
void saveStateNow();
void saveHistoryNow();
void pushHistorySnapshot();
void setupAutosaveTimer();
void saveAutosaveBackup();