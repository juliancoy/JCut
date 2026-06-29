#include "startup_project_state.h"

#include "clip_serialization.h"
#include "json_io_utils.h"
#include "project_manager.h"

#include <QFile>
#include <QJsonArray>
#include <QSignalBlocker>

namespace {

const QLatin1String kHistoryTranscriptDocumentsKey("__historyTranscriptDocuments");
constexpr int kMaxHistoryEntries = 100;
constexpr qsizetype kMaxHistoryCompactBytes = 16 * 1024 * 1024;

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

    snapshot->remove(QString(kHistoryTranscriptDocumentsKey));

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

QJsonObject applyHistoryObjectPatch(QJsonObject base, const QJsonObject& patch)
{
    const QJsonObject set = patch.value(QStringLiteral("set")).toObject();
    for (auto it = set.begin(); it != set.end(); ++it) {
        const QJsonValue existing = base.value(it.key());
        if (existing.isObject() && it.value().isObject()) {
            const QJsonObject patchObject = it.value().toObject();
            if (patchObject.contains(QStringLiteral("set")) ||
                patchObject.contains(QStringLiteral("remove"))) {
                base[it.key()] = applyHistoryObjectPatch(existing.toObject(), patchObject);
                continue;
            }
        }
        base[it.key()] = it.value();
    }

    const QJsonArray remove = patch.value(QStringLiteral("remove")).toArray();
    for (const QJsonValue& value : remove) {
        base.remove(value.toString());
    }
    return base;
}

QJsonArray historyEntriesFromRoot(const QJsonObject& root)
{
    if (root.value(QStringLiteral("format")).toString() != QStringLiteral("snapshot-delta-v1")) {
        return root.value(QStringLiteral("entries")).toArray();
    }

    QJsonArray entries;
    QJsonObject current = root.value(QStringLiteral("base")).toObject();
    if (current.isEmpty() && root.value(QStringLiteral("patches")).toArray().isEmpty()) {
        return entries;
    }
    entries.push_back(current);
    const QJsonArray patches = root.value(QStringLiteral("patches")).toArray();
    for (const QJsonValue& value : patches) {
        current = applyHistoryObjectPatch(current, value.toObject());
        entries.push_back(current);
    }
    return entries;
}

qsizetype historyEntriesCompactSize(const QJsonArray& entries)
{
    QJsonObject root;
    root[QStringLiteral("entries")] = entries;
    return jcut::jsonio::serializeCompact(root).size();
}

void trimHistoryEntriesInPlace(QJsonArray* entries, int* historyIndex)
{
    if (!entries || entries->isEmpty()) {
        if (historyIndex) {
            *historyIndex = -1;
        }
        return;
    }

    if (historyIndex) {
        *historyIndex = qBound(0, *historyIndex, entries->size() - 1);
    }

    auto removeOldest = [&]() {
        entries->removeAt(0);
        if (historyIndex) {
            *historyIndex = qMax(-1, *historyIndex - 1);
        }
    };

    while (entries->size() > kMaxHistoryEntries) {
        removeOldest();
    }
    while (entries->size() > 1 && historyEntriesCompactSize(*entries) > kMaxHistoryCompactBytes) {
        removeOldest();
    }

    if (historyIndex) {
        *historyIndex = entries->isEmpty() ? -1 : qBound(0, *historyIndex, entries->size() - 1);
    }
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
    QJsonArray entries = historyEntriesFromRoot(historyRoot);
    const bool historySanitized = sanitizeHistoryEntriesInPlace(&entries);
    const int entryCountBeforeTrim = entries.size();
    int historyIndex = entryCountBeforeTrim > 0
        ? qBound(0,
                 historyRoot.value(QStringLiteral("index")).toInt(entryCountBeforeTrim - 1),
                 entryCountBeforeTrim - 1)
        : -1;
    trimHistoryEntriesInPlace(&entries, &historyIndex);
    if (!entries.isEmpty() && historyIndex >= 0) {
        root = entries.at(historyIndex).toObject();
    }

    result[QStringLiteral("root")] = root;
    result[QStringLiteral("history_entries")] = entries;
    result[QStringLiteral("history_index")] = historyIndex;
    result[QStringLiteral("history_sanitized")] =
        historySanitized || entries.size() != entryCountBeforeTrim;
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
