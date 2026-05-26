#pragma once

#include "editor_shared.h"

#include <QJsonObject>
#include <QString>
#include <QVector>

class ProjectManager;

namespace editor_startup {

QJsonObject loadStartupStatePayload(const QString& projectId,
                                    const QString& statePath,
                                    const QString& historyPath);

QJsonObject loadActiveProjectStartupStatePayload(QString* projectIdOut = nullptr,
                                                 QString* statePathOut = nullptr,
                                                 QString* historyPathOut = nullptr,
                                                 ProjectManager* projectManager = nullptr);

QVector<TimelineClip> startupTimelineClips(const QJsonObject& root);
bool startupClipById(const QJsonObject& root,
                     const QString& clipId,
                     TimelineClip* clipOut);
bool startupSelectedClip(const QJsonObject& root,
                         TimelineClip* clipOut);

} // namespace editor_startup
