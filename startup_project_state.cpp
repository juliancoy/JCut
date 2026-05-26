#include "startup_project_state.h"

#include "clip_serialization.h"
#include "json_io_utils.h"
#include "project_manager.h"

#include <QFile>
#include <QJsonArray>
#include <QSignalBlocker>

namespace {

void stripHeavyClipState(QJsonObject* clipObj)
{
    if (!clipObj) {
        return;
    }
    clipObj->remove(QStringLiteral("speakerFramingKeyframes"));
}

void stripHeavyStateSnapshot(QJsonObject* snapshot)
{
    if (!snapshot) {
        return;
    }

    QJsonArray timeline = snapshot->value(QStringLiteral("timeline")).toArray();
    for (int i = 0; i < timeline.size(); ++i) {
        QJsonObject clipObj = timeline.at(i).toObject();
        stripHeavyClipState(&clipObj);
        timeline[i] = clipObj;
    }
    (*snapshot)[QStringLiteral("timeline")] = timeline;

    QJsonObject selectedClip = snapshot->value(QStringLiteral("selectedClip")).toObject();
    stripHeavyClipState(&selectedClip);
    (*snapshot)[QStringLiteral("selectedClip")] = selectedClip;
}

bool sanitizeHistoryEntriesInPlace(QJsonArray* entries)
{
    if (!entries) {
        return false;
    }

    bool changed = false;
    for (int i = 0; i < entries->size(); ++i) {
        QJsonObject snapshot = entries->at(i).toObject();
        const QJsonObject before = snapshot;
        stripHeavyStateSnapshot(&snapshot);
        if (snapshot != before) {
            (*entries)[i] = snapshot;
            changed = true;
        }
    }
    return changed;
}

} // namespace

namespace editor_startup {

QJsonObject loadStartupStatePayload(const QString& projectId,
                                    const QString& statePath,
                                    const QString& historyPath)
{
    QJsonObject result{
        {QStringLiteral("project_id"), projectId},
        {QStringLiteral("history_index"), -1},
        {QStringLiteral("defer_history"), false},
        {QStringLiteral("history_sanitized"), false}
    };

    QJsonObject root;
    QFile file(statePath);
    if (file.open(QIODevice::ReadOnly)) {
        jcut::jsonio::parseObjectBytes(file.readAll(), &root);
    }
    if (!root.isEmpty()) {
        result[QStringLiteral("root")] = root;
        result[QStringLiteral("defer_history")] = true;
        return result;
    }

    QFile historyFile(historyPath);
    if (!historyFile.open(QIODevice::ReadOnly)) {
        result[QStringLiteral("root")] = QJsonObject{};
        result[QStringLiteral("history_entries")] = QJsonArray{};
        return result;
    }

    QJsonObject historyRoot;
    jcut::jsonio::parseObjectBytes(historyFile.readAll(), &historyRoot);
    QJsonArray entries = historyRoot.value(QStringLiteral("entries")).toArray();
    const bool historySanitized = sanitizeHistoryEntriesInPlace(&entries);
    const int entryCount = entries.size();
    const int historyIndex = entryCount > 0
        ? qBound(0, historyRoot.value(QStringLiteral("index")).toInt(entryCount - 1), entryCount - 1)
        : -1;
    if (entryCount > 0) {
        root = entries.at(historyIndex).toObject();
    }

    result[QStringLiteral("root")] = root;
    result[QStringLiteral("history_entries")] = entries;
    result[QStringLiteral("history_index")] = historyIndex;
    result[QStringLiteral("history_sanitized")] = historySanitized;
    return result;
}

QJsonObject loadActiveProjectStartupStatePayload(QString* projectIdOut,
                                                 QString* statePathOut,
                                                 QString* historyPathOut,
                                                 ProjectManager* projectManager)
{
    ProjectManager ownedManager;
    ProjectManager* manager = projectManager ? projectManager : &ownedManager;
    manager->loadProjectsFromFolders();

    const QString projectId = manager->currentProjectIdOrDefault();
    const QString statePath = manager->stateFilePathForProject(projectId);
    const QString historyPath = manager->historyFilePathForProject(projectId);

    if (projectIdOut) {
        *projectIdOut = projectId;
    }
    if (statePathOut) {
        *statePathOut = statePath;
    }
    if (historyPathOut) {
        *historyPathOut = historyPath;
    }

    return loadStartupStatePayload(projectId, statePath, historyPath);
}

QVector<TimelineClip> startupTimelineClips(const QJsonObject& root)
{
    QVector<TimelineClip> clips;
    const QJsonArray timeline = root.value(QStringLiteral("timeline")).toArray();
    clips.reserve(timeline.size());
    for (const QJsonValue& value : timeline) {
        const QJsonObject clipObject = value.toObject();
        if (clipObject.isEmpty()) {
            continue;
        }
        clips.push_back(editor::clipFromJson(clipObject));
    }
    return clips;
}

bool startupClipById(const QJsonObject& root,
                     const QString& clipId,
                     TimelineClip* clipOut)
{
    if (!clipOut || clipId.trimmed().isEmpty()) {
        return false;
    }

    const QVector<TimelineClip> clips = startupTimelineClips(root);
    for (const TimelineClip& clip : clips) {
        if (clip.id.trimmed() == clipId.trimmed()) {
            *clipOut = clip;
            return true;
        }
    }
    return false;
}

bool startupSelectedClip(const QJsonObject& root,
                         TimelineClip* clipOut)
{
    if (!clipOut) {
        return false;
    }

    const QString selectedClipId = root.value(QStringLiteral("selectedClipId")).toString().trimmed();
    if (!selectedClipId.isEmpty() && startupClipById(root, selectedClipId, clipOut)) {
        return true;
    }

    const QJsonObject selectedClipObject = root.value(QStringLiteral("selectedClip")).toObject();
    if (!selectedClipObject.isEmpty()) {
        *clipOut = editor::clipFromJson(selectedClipObject);
        if (!clipOut->id.trimmed().isEmpty()) {
            return true;
        }
    }

    const QVector<TimelineClip> clips = startupTimelineClips(root);
    if (!clips.isEmpty()) {
        *clipOut = clips.first();
        return true;
    }

    return false;
}

} // namespace editor_startup
