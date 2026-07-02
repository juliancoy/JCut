// project_state.cpp
#include "editor.h"
#include "json_io_utils.h"
#include "speakers_table.h"
#include "clip_serialization.h"
#include "debug_controls.h"
#include "editor_shared_transcript.h"
#include "startup_project_state.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QInputDialog>
#include <QLineEdit>
#include <QMessageBox>
#include <QRegularExpression>
#include <QSaveFile>
#include <QDebug>
#include <QSet>
#include <QSignalBlocker>
#include <QStandardPaths>
#include <QCoreApplication>
#include <QtConcurrent/QtConcurrentRun>

#include <cmath>

using namespace editor;

namespace {

const QLatin1String kHistoryTranscriptDocumentsKey("__historyTranscriptDocuments");
constexpr int kFallbackHistoryMaxEntries = 100;
constexpr int kFallbackHistoryMaxMegabytes = 16;

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

QJsonObject diffHistoryObject(const QJsonObject& previous, const QJsonObject& current)
{
    QJsonObject set;
    QJsonArray remove;

    for (auto it = current.begin(); it != current.end(); ++it) {
        const QJsonValue previousValue = previous.value(it.key());
        if (previousValue == it.value()) {
            continue;
        }
        if (previousValue.isObject() && it.value().isObject()) {
            const QJsonObject nested =
                diffHistoryObject(previousValue.toObject(), it.value().toObject());
            if (!nested.value(QStringLiteral("set")).toObject().isEmpty() ||
                !nested.value(QStringLiteral("remove")).toArray().isEmpty()) {
                set[it.key()] = nested;
            }
        } else {
            set[it.key()] = it.value();
        }
    }

    for (auto it = previous.begin(); it != previous.end(); ++it) {
        if (!current.contains(it.key())) {
            remove.push_back(it.key());
        }
    }

    return QJsonObject{
        {QStringLiteral("set"), set},
        {QStringLiteral("remove"), remove},
    };
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

QJsonObject historyRootFromEntries(const QJsonArray& entries, int index)
{
    QJsonObject root;
    root[QStringLiteral("format")] = QStringLiteral("snapshot-delta-v1");
    root[QStringLiteral("index")] = index;
    if (entries.isEmpty()) {
        root[QStringLiteral("base")] = QJsonObject();
        root[QStringLiteral("patches")] = QJsonArray();
        return root;
    }

    QJsonObject previous = entries.at(0).toObject();
    root[QStringLiteral("base")] = previous;
    QJsonArray patches;
    for (int i = 1; i < entries.size(); ++i) {
        const QJsonObject current = entries.at(i).toObject();
        patches.push_back(diffHistoryObject(previous, current));
        previous = current;
    }
    root[QStringLiteral("patches")] = patches;
    return root;
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
    return jcut::jsonio::serializeCompact(historyRootFromEntries(entries, entries.size() - 1)).size();
}

void trimHistoryEntriesInPlace(QJsonArray* entries,
                               int* historyIndex,
                               int maxEntries,
                               int maxMegabytes)
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

    const int boundedMaxEntries = qBound(1, maxEntries, 500);
    const qsizetype maxCompactBytes =
        static_cast<qsizetype>(qBound(1, maxMegabytes, 256)) * 1024 * 1024;

    while (entries->size() > boundedMaxEntries) {
        removeOldest();
    }
    while (entries->size() > 1 && historyEntriesCompactSize(*entries) > maxCompactBytes) {
        removeOldest();
    }

    if (historyIndex) {
        *historyIndex = entries->isEmpty() ? -1 : qBound(0, *historyIndex, entries->size() - 1);
    }
}

bool writeJsonObjectAtomic(const QString& path, const QJsonObject& root)
{
    if (path.trimmed().isEmpty()) {
        return false;
    }
    QDir().mkpath(QFileInfo(path).absolutePath());
    const QByteArray payload = jcut::jsonio::serializeIndented(root);
    QSaveFile file(path);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        return false;
    }
    if (file.write(payload) != payload.size()) {
        file.cancelWriting();
        return false;
    }
    return file.commit();
}

QByteArray writeStateObjectAtomic(const QString& path,
                                  const QJsonObject& root,
                                  const QByteArray& lastSavedState)
{
    if (path.trimmed().isEmpty()) {
        return QByteArray();
    }
    const QByteArray serializedState = jcut::jsonio::serializeIndented(root);
    if (serializedState == lastSavedState) {
        return serializedState;
    }
    QDir().mkpath(QFileInfo(path).absolutePath());
    QSaveFile file(path);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        return QByteArray();
    }
    if (file.write(serializedState) != serializedState.size()) {
        file.cancelWriting();
        return QByteArray();
    }
    if (!file.commit()) {
        return QByteArray();
    }
    return serializedState;
}

QString relativeStatePath(const QString& path, const QString& rootPath)
{
    const QString trimmed = path.trimmed();
    if (trimmed.isEmpty() || QFileInfo(trimmed).isRelative() || rootPath.trimmed().isEmpty()) {
        return path;
    }

    const QDir rootDir(rootPath);
    if (!rootDir.exists()) {
        return path;
    }

    const QString relative = rootDir.relativeFilePath(QFileInfo(trimmed).absoluteFilePath());
    if (relative.isEmpty() ||
        relative == QStringLiteral(".") ||
        relative.startsWith(QStringLiteral("../")) ||
        relative == QStringLiteral("..")) {
        return path;
    }
    return QDir::cleanPath(relative);
}

TimelineClip clipWithRelativeStatePaths(TimelineClip clip, const QString& rootPath)
{
    clip.filePath = relativeStatePath(clip.filePath, rootPath);
    clip.proxyPath = relativeStatePath(clip.proxyPath, rootPath);
    clip.audioSourcePath = relativeStatePath(clip.audioSourcePath, rootPath);
    clip.audioSourceOriginalPath = relativeStatePath(clip.audioSourceOriginalPath, rootPath);
    return clip;
}

} // namespace

void EditorWindow::attachTranscriptDocumentsToHistorySnapshot(QJsonObject* snapshot) const
{
    if (!snapshot || !m_timeline) {
        return;
    }

    QString liveClipFilePath;
    QString liveTranscriptPath;
    QJsonDocument liveDocument;
    const bool hasLiveDocument =
        m_transcriptTab &&
        m_transcriptTab->activeTranscriptDocumentSnapshot(
            &liveClipFilePath, &liveTranscriptPath, &liveDocument) &&
        liveDocument.isObject();
    if (hasLiveDocument) {
        liveTranscriptPath = QFileInfo(liveTranscriptPath).absoluteFilePath();
    }

    QSet<QString> capturedPaths;
    QJsonArray documents;
    for (const TimelineClip& clip : m_timeline->clips()) {
        if (!(clip.mediaType == ClipMediaType::Audio || clip.hasAudio)) {
            continue;
        }
        QString transcriptPath = activeTranscriptPathForClip(clip).trimmed();
        if (transcriptPath.isEmpty()) {
            continue;
        }
        transcriptPath = QFileInfo(transcriptPath).absoluteFilePath();
        if (capturedPaths.contains(transcriptPath)) {
            continue;
        }

        QJsonDocument document;
        if (hasLiveDocument &&
            QFileInfo(liveClipFilePath).absoluteFilePath() == QFileInfo(clip.filePath).absoluteFilePath() &&
            liveTranscriptPath == transcriptPath) {
            document = liveDocument;
        } else if (!loadTranscriptJsonCached(transcriptPath, &document)) {
            continue;
        }
        if (!document.isObject()) {
            continue;
        }

        documents.push_back(QJsonObject{
            {QStringLiteral("clipFilePath"), clip.filePath},
            {QStringLiteral("transcriptPath"), transcriptPath},
            {QStringLiteral("document"), document.object()},
        });
        capturedPaths.insert(transcriptPath);
    }

    if (!documents.isEmpty()) {
        (*snapshot)[QString(kHistoryTranscriptDocumentsKey)] = documents;
    }
}

void EditorWindow::restoreTranscriptDocumentsFromHistorySnapshot(const QJsonObject& snapshot)
{
    const QJsonArray documents = snapshot.value(QString(kHistoryTranscriptDocumentsKey)).toArray();
    if (documents.isEmpty()) {
        return;
    }

    for (const QJsonValue& value : documents) {
        const QJsonObject entry = value.toObject();
        const QString clipFilePath = entry.value(QStringLiteral("clipFilePath")).toString();
        const QString transcriptPath =
            QFileInfo(entry.value(QStringLiteral("transcriptPath")).toString()).absoluteFilePath();
        const QJsonObject documentObject = entry.value(QStringLiteral("document")).toObject();
        if (transcriptPath.trimmed().isEmpty() || documentObject.isEmpty()) {
            continue;
        }

        QDir().mkpath(QFileInfo(transcriptPath).absolutePath());
        const QByteArray payload =
            jcut::jsonio::serializeIndented(QJsonDocument(documentObject).object());
        QSaveFile file(transcriptPath);
        if (file.open(QIODevice::WriteOnly | QIODevice::Truncate) &&
            file.write(payload) == payload.size() &&
            file.commit()) {
            invalidateTranscriptJsonCache(transcriptPath);
            invalidateTranscriptSpeakerProfileCache(transcriptPath);
            if (m_transcriptTab) {
                m_transcriptTab->restoreTranscriptDocumentSnapshot(
                    clipFilePath, transcriptPath, QJsonDocument(documentObject));
            }
        } else {
            file.cancelWriting();
        }
    }
}

void EditorWindow::scheduleDeferredHistoryLoad(const QString& projectId)
{
    if (projectId.trimmed().isEmpty() || m_deferredHistoryLoadWatcher.isRunning()) {
        return;
    }
    m_deferredHistoryLoadProjectId = projectId.trimmed();
    const QString historyPath = m_projectManager
        ? m_projectManager->historyFilePathForProject(m_deferredHistoryLoadProjectId)
        : QString();
    startupProfileMark(QStringLiteral("load_state.history_deferred.begin"),
                       QJsonObject{{QStringLiteral("project_id"), m_deferredHistoryLoadProjectId}});
    m_deferredHistoryLoadWatcher.setFuture(QtConcurrent::run([historyPath, projectId = m_deferredHistoryLoadProjectId]() {
        QJsonObject result{
            {QStringLiteral("project_id"), projectId},
            {QStringLiteral("entry_count"), 0},
            {QStringLiteral("history_index"), -1},
            {QStringLiteral("history_sanitized"), false}
        };
        QFile historyFile(historyPath);
        if (!historyFile.open(QIODevice::ReadOnly)) {
            return result;
        }
        QJsonObject historyRoot;
        jcut::jsonio::parseObjectBytes(historyFile.readAll(), &historyRoot);
        QJsonArray entries = historyEntriesFromRoot(historyRoot);
        const bool historySanitized = sanitizeHistoryEntriesInPlace(&entries);
        const int entryCountBeforeTrim = entries.size();
        int index = entryCountBeforeTrim > 0
            ? qBound(0,
                     historyRoot.value(QStringLiteral("index")).toInt(entryCountBeforeTrim - 1),
                     entryCountBeforeTrim - 1)
            : -1;
        trimHistoryEntriesInPlace(&entries,
                                  &index,
                                  kFallbackHistoryMaxEntries,
                                  kFallbackHistoryMaxMegabytes);
        result[QStringLiteral("entries")] = entries;
        result[QStringLiteral("entry_count")] = entries.size();
        result[QStringLiteral("history_index")] = index;
        result[QStringLiteral("history_sanitized")] =
            historySanitized || entries.size() != entryCountBeforeTrim;
        return result;
    }));
}

void EditorWindow::loadState()
{
    const bool startupMarking = !m_startupProfileCompleted;
    auto markStartup = [this, startupMarking](const QString& phase, const QJsonObject& extra = QJsonObject()) {
        if (!startupMarking) {
            return;
        }
        startupProfileMark(phase, extra);
    };
    markStartup(QStringLiteral("load_state.begin"));
    if (m_projectManager) {
        m_projectManager->loadProjectsFromFolders();
    }
    markStartup(QStringLiteral("load_state.projects_loaded"));
    const QString projectId = m_projectManager
        ? m_projectManager->currentProjectIdOrDefault()
        : QStringLiteral("default");
    const QString statePath = m_projectManager ? m_projectManager->stateFilePath() : QString();
    const QString historyPath = m_projectManager ? m_projectManager->historyFilePath() : QString();
    qDebug() << "[PROJECT] Loading project:" << projectId;
    qDebug() << "[PROJECT] State file:" << statePath;
    qDebug() << "[PROJECT] History file:" << historyPath;
    m_historyEntries = QJsonArray();
    m_historyIndex = -1;
    m_lastSavedState.clear();

    QJsonObject root;
    bool deferHistoryLoad = false;
    if (startupMarking) {
        markStartup(QStringLiteral("load_state.state_file_read.begin"));
        QFile file(statePath);
        if (file.open(QIODevice::ReadOnly)) {
            jcut::jsonio::parseObjectBytes(file.readAll(), &root);
        }
        markStartup(QStringLiteral("load_state.state_file_read.end"));
        if (!root.isEmpty()) {
            deferHistoryLoad = true;
            markStartup(QStringLiteral("load_state.history_read.deferred"),
                        QJsonObject{{QStringLiteral("project_id"), projectId}});
        }
    }

    if (root.isEmpty()) {
        QFile historyFile(historyPath);
        markStartup(QStringLiteral("load_state.history_read.begin"));
        if (historyFile.open(QIODevice::ReadOnly))
        {
            QJsonObject historyRoot;
            jcut::jsonio::parseObjectBytes(historyFile.readAll(), &historyRoot);
            m_historyEntries = historyEntriesFromRoot(historyRoot);
            const bool historySanitized = sanitizeHistoryEntriesInPlace(&m_historyEntries);
            m_historyIndex = historyRoot.value(QStringLiteral("index")).toInt(m_historyEntries.size() - 1);
            const int beforeTrimEntryCount = m_historyEntries.size();
            trimHistoryEntriesInPlace(&m_historyEntries,
                                      &m_historyIndex,
                                      m_historyMaxEntries,
                                      m_historyMaxMegabytes);
            if (!m_historyEntries.isEmpty())
            {
                m_historyIndex = qBound(0, m_historyIndex, m_historyEntries.size() - 1);
                root = m_historyEntries.at(m_historyIndex).toObject();
            }
            if (historySanitized || m_historyEntries.size() != beforeTrimEntryCount) {
                saveHistoryNow();
            }
        }
        markStartup(QStringLiteral("load_state.history_read.end"),
                    QJsonObject{
                        {QStringLiteral("history_entry_count"), m_historyEntries.size()},
                        {QStringLiteral("history_index"), m_historyIndex}
                    });
    }

    markStartup(QStringLiteral("load_state.apply_state.begin"));
    applyStateJson(root);
    markStartup(QStringLiteral("load_state.apply_state.end"));

    if (m_historyEntries.isEmpty() && !deferHistoryLoad)
    {
        pushHistorySnapshot();
    }

    if (m_pendingSaveAfterLoad)
    {
        m_pendingSaveAfterLoad = false;
        scheduleSaveState();
    }
    else
    {
        scheduleSaveState();
    }
    if (deferHistoryLoad) {
        QTimer::singleShot(0, this, [this, projectId]() {
            scheduleDeferredHistoryLoad(projectId);
        });
    }
    QTimer::singleShot(0, this, [this]() {
        scheduleTranscriptTextCompanionBackfill();
    });
    markStartup(QStringLiteral("load_state.end"));
}

void EditorWindow::refreshProjectsList()
{
    if (!m_projectsTab) {
        return;
    }
    if (m_projectManager) {
        m_projectManager->loadProjectsFromFolders();
    }
    m_projectsTab->refresh();
}

void EditorWindow::changeMediaRoot(const QString &path)
{
    if (!m_projectManager || path.trimmed().isEmpty()) {
        return;
    }

    const QString previousRoot = m_projectManager->rootDirPath();
    const QString requestedRoot = QFileInfo(path).absoluteFilePath();
    if (QDir(previousRoot).absolutePath() == QDir(requestedRoot).absolutePath()) {
        if (m_explorerPane) {
            m_explorerPane->setInitialRootPath(previousRoot);
        }
        refreshProjectsList();
        return;
    }

    stopPlaybackWithReason(QStringLiteral("media_root_changed"));
    flushStateSaveNow();
    flushHistorySaveNow();
    m_stateSaveTimer.stop();
    m_historySaveTimer.stop();
    m_pendingSaveAfterLoad = false;
    m_pendingSaveAfterPlayback = false;

    if (!m_projectManager->changeRootDirPath(path)) {
        refreshProjectsList();
        return;
    }

    m_lastSavedState.clear();
    m_historyEntries = QJsonArray();
    m_historyIndex = -1;

    loadState();
    refreshProjectsList();
    refreshCurrentInspectorTab();
}

void EditorWindow::switchToProject(const QString &projectId)
{
    if (projectId.isEmpty() ||
        projectId == (m_projectManager ? m_projectManager->currentProjectIdOrDefault()
                                       : QStringLiteral("default")))
    {
        refreshProjectsList();
        return;
    }

    flushStateSaveNow();
    flushHistorySaveNow();

    m_lastSavedState.clear();
    m_historyEntries = QJsonArray();
    m_historyIndex = -1;

    if (m_projectManager) {
        m_projectManager->switchToProject(projectId);
    }
    loadState();

    if (m_inspectorTabs) {
        for (int i = 0; i < m_inspectorTabs->count(); ++i) {
            if (m_inspectorTabs->tabText(i).compare(QStringLiteral("Projects"), Qt::CaseInsensitive) == 0) {
                m_inspectorTabs->setCurrentIndex(i);
                break;
            }
        }
    }

    refreshProjectsList();
    refreshCurrentInspectorTab();
}

void EditorWindow::createProject()
{
    if (!m_projectManager) {
        return;
    }
    const QString beforeProjectId = m_projectManager
        ? m_projectManager->currentProjectIdOrDefault()
        : QStringLiteral("default");
    m_projectManager->createProject();
    if ((m_projectManager ? m_projectManager->currentProjectIdOrDefault()
                          : QStringLiteral("default")) != beforeProjectId) {
        m_lastSavedState.clear();
        m_historyEntries = QJsonArray();
        m_historyIndex = -1;
        loadState();
        refreshCurrentInspectorTab();
    }
    refreshProjectsList();
}

bool EditorWindow::saveProjectPayload(const QString &projectId,
                                      const QByteArray &statePayload,
                                      const QByteArray &historyPayload)
{
    return m_projectManager &&
           m_projectManager->saveProjectPayload(projectId, statePayload, historyPayload);
}

void EditorWindow::saveProjectAs()
{
    if (!m_timeline)
    {
        return;
    }

    bool accepted = false;
    const QString name = QInputDialog::getText(this,
                                               QStringLiteral("Save Project As"),
                                               QStringLiteral("Project name"),
                                               QLineEdit::Normal,
                                               (m_projectManager ? m_projectManager->currentProjectName() : QString()) == QStringLiteral("Default Project")
                                                   ? QStringLiteral("Untitled Project")
                                                   : (m_projectManager ? m_projectManager->currentProjectName() : QString()),
                                               &accepted)
                             .trimmed();
    if (!accepted || name.isEmpty())
    {
        return;
    }

    flushStateSaveNow();
    flushHistorySaveNow();

    const QString newProjectId = m_projectManager ? m_projectManager->sanitizedProjectId(name) : QString();
    const QByteArray statePayload =
        jcut::jsonio::serializeIndented(buildStateJson());

    QJsonArray historyEntries = m_historyEntries;
    int historyIndex = m_historyIndex;
    sanitizeHistoryEntriesInPlace(&historyEntries);
    trimHistoryEntriesInPlace(&historyEntries,
                              &historyIndex,
                              m_historyMaxEntries,
                              m_historyMaxMegabytes);
    QJsonObject historyRoot = historyRootFromEntries(historyEntries, historyIndex);
    const QByteArray historyPayload =
        jcut::jsonio::serializeIndented(historyRoot);

    if (!saveProjectPayload(newProjectId, statePayload, historyPayload))
    {
        QMessageBox::warning(this,
                             QStringLiteral("Save Project As Failed"),
                             QStringLiteral("Could not write the new project files."));
        return;
    }

    switchToProject(newProjectId);
}

void EditorWindow::renameProject(const QString &projectId)
{
    if (!m_projectManager || projectId.isEmpty() || !QFileInfo::exists(m_projectManager->projectPath(projectId)))
    {
        return;
    }
    m_projectManager->renameProject(projectId);
    refreshProjectsList();
}

QJsonObject EditorWindow::buildStateJson() const
{
    QJsonObject root;
    const QString mediaRoot =
        m_explorerPane ? m_explorerPane->currentRootPath() : QString();
    const QString mediaGalleryPath =
        m_explorerPane ? m_explorerPane->galleryPath() : QString();
    root[QStringLiteral("mediaRoot")] = mediaRoot;
    root[QStringLiteral("mediaGalleryPath")] = mediaGalleryPath;
    // Backward compatibility for older state readers.
    root[QStringLiteral("explorerRoot")] = mediaRoot;
    root[QStringLiteral("explorerGalleryPath")] = mediaGalleryPath;
    root[QStringLiteral("currentFrame")] =
        static_cast<qint64>(m_timeline ? m_timeline->currentFrame() : 0);
    root[QStringLiteral("playing")] = m_playbackTimer.isActive();
    root[QStringLiteral("selectedClipId")] =
        m_timeline ? m_timeline->selectedClipId() : QString();
    root[QStringLiteral("selectedTrackIndex")] =
        m_timeline ? m_timeline->selectedTrackIndex() : -1;
    QJsonArray selectedClipIds;
    if (m_timeline)
    {
        for (const QString& id : m_timeline->selectedClipIds())
        {
            selectedClipIds.push_back(id);
        }
    }
    root[QStringLiteral("selectedClipIds")] = selectedClipIds;

    QJsonArray expandedFolders;
    if (m_explorerPane)
    {
        for (const QString &path : m_explorerPane->currentExpandedExplorerPaths())
        {
            expandedFolders.push_back(path);
        }
    }
    root[QStringLiteral("mediaExpandedFolders")] = expandedFolders;
    // Backward compatibility for older state readers.
    root[QStringLiteral("explorerExpandedFolders")] = expandedFolders;

    root[QStringLiteral("outputWidth")] =
        m_outputWidthSpin ? m_outputWidthSpin->value() : 1080;
    root[QStringLiteral("outputHeight")] =
        m_outputHeightSpin ? m_outputHeightSpin->value() : 1920;
    const double timelineFps =
        m_outputFpsSpin ? m_outputFpsSpin->value() : static_cast<double>(kTimelineFps);
    root[QStringLiteral("timelineFps")] = timelineFps;
    // Backward compatibility for older state readers and export helpers.
    root[QStringLiteral("outputFps")] = timelineFps;
    root[QStringLiteral("outputFormat")] =
        m_outputFormatCombo ? m_outputFormatCombo->currentData().toString()
                            : QStringLiteral("mp4");
    root[QStringLiteral("lastRenderOutputPath")] = m_lastRenderOutputPath;
    root[QStringLiteral("renderUseProxies")] =
        m_renderUseProxiesCheckBox ? m_renderUseProxiesCheckBox->isChecked() : false;
    root[QStringLiteral("createImageSequence")] =
        m_createImageSequenceCheckBox ? m_createImageSequenceCheckBox->isChecked() : false;
    root[QStringLiteral("imageSequenceFormat")] =
        m_imageSequenceFormatCombo ? m_imageSequenceFormatCombo->currentData().toString()
                                   : QStringLiteral("jpeg");
    root[QStringLiteral("backgroundFillEffect")] =
        m_backgroundFillEffectCombo
            ? m_backgroundFillEffectCombo->currentData().toString()
            : backgroundFillEffectToString(kDefaultBackgroundFillEffect);
    root[QStringLiteral("backgroundFillOpacity")] =
        m_backgroundFillOpacitySpin
            ? qBound(0.0, m_backgroundFillOpacitySpin->value() / 100.0, 1.0)
            : 1.0;
    root[QStringLiteral("backgroundFillBrightness")] =
        m_backgroundFillBrightnessSpin
            ? qBound(-1.0, m_backgroundFillBrightnessSpin->value() / 100.0, 1.0)
            : 0.0;
    root[QStringLiteral("backgroundFillSaturation")] =
        m_backgroundFillSaturationSpin
            ? qBound(0.0, m_backgroundFillSaturationSpin->value() / 100.0, 3.0)
            : 1.0;
    root[QStringLiteral("backgroundFillEdgePixels")] =
        m_backgroundFillEdgePixelsSlider ? qBound(1, m_backgroundFillEdgePixelsSlider->value(), 512) : 1;
    root[QStringLiteral("backgroundFillEdgeProgressive")] =
        m_backgroundFillEdgeProgressiveCheckBox ? m_backgroundFillEdgeProgressiveCheckBox->isChecked() : false;
    root[QStringLiteral("backgroundFillEdgePower")] =
        m_backgroundFillEdgePowerSpin
            ? qBound(0.25, m_backgroundFillEdgePowerSpin->value(), 8.0)
            : 2.0;
    root[QStringLiteral("previewHideOutsideOutput")] =
        m_previewHideOutsideOutputCheckBox ? m_previewHideOutsideOutputCheckBox->isChecked() : false;
    root[QStringLiteral("previewShowSpeakerTrackPoints")] =
        m_previewShowSpeakerTrackPointsCheckBox ? m_previewShowSpeakerTrackPointsCheckBox->isChecked() : false;
    root[QStringLiteral("previewShowSpeakerTrackBoxes")] =
        m_speakerShowFaceDetectionsBoxesCheckBox ? m_speakerShowFaceDetectionsBoxesCheckBox->isChecked() : false;
    root[QStringLiteral("speakerShowContiguousTranscriptSections")] =
        m_speakerShowContiguousSectionsCheckBox ? m_speakerShowContiguousSectionsCheckBox->isChecked() : false;
    root[QStringLiteral("speakerApplyTrackToAllMatchingSections")] =
        m_speakerApplyTrackToAllMatchingSectionsCheckBox
            ? m_speakerApplyTrackToAllMatchingSectionsCheckBox->isChecked()
            : false;
    root[QStringLiteral("previewShowRawDetections")] =
        m_speakerShowRawDetectionsCheckBox ? m_speakerShowRawDetectionsCheckBox->isChecked() : false;
    root[QStringLiteral("previewShowCurrentSpeakerName")] =
        m_speakerShowCurrentSpeakerNameCheckBox ? m_speakerShowCurrentSpeakerNameCheckBox->isChecked() : false;
    root[QStringLiteral("previewShowCurrentSpeakerOrganization")] =
        m_speakerShowCurrentSpeakerOrganizationCheckBox
            ? m_speakerShowCurrentSpeakerOrganizationCheckBox->isChecked()
            : false;
    root[QStringLiteral("previewCurrentSpeakerNameTextScalePercent")] =
        m_speakerCurrentSpeakerNameTextSizeSpin ? m_speakerCurrentSpeakerNameTextSizeSpin->value() : 100;
    root[QStringLiteral("previewCurrentSpeakerOrganizationTextScalePercent")] =
        m_speakerCurrentSpeakerOrganizationTextSizeSpin
            ? m_speakerCurrentSpeakerOrganizationTextSizeSpin->value()
            : 100;
    root[QStringLiteral("previewCurrentSpeakerNameYPositionPercent")] =
        m_speakerCurrentSpeakerNameYPositionSpin
            ? m_speakerCurrentSpeakerNameYPositionSpin->value()
            : 86;
    root[QStringLiteral("previewCurrentSpeakerOrganizationYPositionPercent")] =
        m_speakerCurrentSpeakerOrganizationYPositionSpin
            ? m_speakerCurrentSpeakerOrganizationYPositionSpin->value()
            : 93;
    root[QStringLiteral("previewCurrentSpeakerNameColor")] =
        m_speakerCurrentSpeakerNameColor.name(QColor::HexRgb);
    root[QStringLiteral("previewCurrentSpeakerOrganizationColor")] =
        m_speakerCurrentSpeakerOrganizationColor.name(QColor::HexRgb);
    root[QStringLiteral("previewCurrentSpeakerBackgroundColor")] =
        QColor(m_speakerCurrentSpeakerBackgroundColor.red(),
               m_speakerCurrentSpeakerBackgroundColor.green(),
               m_speakerCurrentSpeakerBackgroundColor.blue()).name(QColor::HexRgb);
    root[QStringLiteral("previewCurrentSpeakerBackgroundOpacityPercent")] =
        m_speakerCurrentSpeakerBackgroundOpacitySpin
            ? m_speakerCurrentSpeakerBackgroundOpacitySpin->value()
            : qRound(m_speakerCurrentSpeakerBackgroundColor.alphaF() * 100.0);
    root[QStringLiteral("previewCurrentSpeakerBorderColor")] =
        QColor(m_speakerCurrentSpeakerBorderColor.red(),
               m_speakerCurrentSpeakerBorderColor.green(),
               m_speakerCurrentSpeakerBorderColor.blue()).name(QColor::HexRgb);
    root[QStringLiteral("previewCurrentSpeakerBorderOpacityPercent")] =
        m_speakerCurrentSpeakerBorderOpacitySpin
            ? m_speakerCurrentSpeakerBorderOpacitySpin->value()
            : qRound(m_speakerCurrentSpeakerBorderColor.alphaF() * 100.0);
    root[QStringLiteral("previewCurrentSpeakerBackgroundRadiusPx")] =
        m_speakerCurrentSpeakerBackgroundRadiusSpin
            ? m_speakerCurrentSpeakerBackgroundRadiusSpin->value()
            : 14;
    root[QStringLiteral("previewCurrentSpeakerBorderWidthPx")] =
        m_speakerCurrentSpeakerBorderWidthSpin
            ? m_speakerCurrentSpeakerBorderWidthSpin->value()
            : 1;
    root[QStringLiteral("previewCurrentSpeakerShadowEnabled")] =
        m_speakerCurrentSpeakerShadowCheckBox
            ? m_speakerCurrentSpeakerShadowCheckBox->isChecked()
            : true;
    root[QStringLiteral("previewCurrentSpeakerShadowColor")] =
        QColor(m_speakerCurrentSpeakerShadowColor.red(),
               m_speakerCurrentSpeakerShadowColor.green(),
               m_speakerCurrentSpeakerShadowColor.blue()).name(QColor::HexRgb);
    root[QStringLiteral("previewCurrentSpeakerShadowOpacityPercent")] =
        m_speakerCurrentSpeakerShadowOpacitySpin
            ? m_speakerCurrentSpeakerShadowOpacitySpin->value()
            : qRound(m_speakerCurrentSpeakerShadowColor.alphaF() * 100.0);
    root[QStringLiteral("previewFacestreamOverlaySource")] = QStringLiteral("all");
    root[QStringLiteral("previewPlaybackCacheFallback")] = editor::debugPlaybackCacheFallbackEnabled();
    root[QStringLiteral("previewLeadPrefetchEnabled")] = editor::debugLeadPrefetchEnabled();
    root[QStringLiteral("previewLeadPrefetchCount")] = editor::debugLeadPrefetchCount();
    root[QStringLiteral("previewPlaybackWindowAhead")] = editor::debugPlaybackWindowAhead();
    root[QStringLiteral("previewVisibleQueueReserve")] = editor::debugVisibleQueueReserve();
    root[QStringLiteral("debugPrefetchMaxQueueDepth")] = editor::debugPrefetchMaxQueueDepth();
    root[QStringLiteral("debugPrefetchMaxInflight")] = editor::debugPrefetchMaxInflight();
    root[QStringLiteral("debugPrefetchMaxPerTick")] = editor::debugPrefetchMaxPerTick();
    root[QStringLiteral("debugPrefetchSkipVisiblePendingThreshold")] =
        editor::debugPrefetchSkipVisiblePendingThreshold();
    root[QStringLiteral("debugDecoderLaneCount")] = editor::debugDecoderLaneCount();
    root[QStringLiteral("timelineAudioEnvelopeGranularity")] =
        editor::debugTimelineAudioEnvelopeGranularity();
    root[QStringLiteral("debugH26xSoftwareThreadingMode")] =
        editor::h26xSoftwareThreadingModeToString(editor::debugH26xSoftwareThreadingMode());
    root[QStringLiteral("rubberBandEngine")] =
        editor::rubberBandEnginePreferenceToString(editor::rubberBandEnginePreference());
    root[QStringLiteral("rubberBandThreading")] =
        editor::rubberBandThreadingPreferenceToString(editor::rubberBandThreadingPreference());
    root[QStringLiteral("rubberBandWindow")] =
        editor::rubberBandWindowPreferenceToString(editor::rubberBandWindowPreference());
    root[QStringLiteral("rubberBandPitch")] =
        editor::rubberBandPitchPreferenceToString(editor::rubberBandPitchPreference());
    root[QStringLiteral("rubberBandChannelsTogether")] =
        editor::rubberBandChannelsTogether();
    root[QStringLiteral("debugDeterministicPipeline")] =
        editor::debugDeterministicPipelineEnabled();
    root[QStringLiteral("debugDeterministicPipelineExplicit")] = true;
    root[QStringLiteral("autosaveIntervalMinutes")] = m_autosaveIntervalMinutes;
    root[QStringLiteral("autosaveMaxBackups")] = m_autosaveMaxBackups;
    root[QStringLiteral("historyMaxEntries")] = m_historyMaxEntries;
    root[QStringLiteral("historyMaxMegabytes")] = m_historyMaxMegabytes;
    root[QStringLiteral("transcriptPrependMs")] = m_transcriptPrependMs;
    root[QStringLiteral("transcriptPostpendMs")] = m_transcriptPostpendMs;
    root[QStringLiteral("transcriptOffsetMs")] = m_transcriptOffsetMs;
    root[QStringLiteral("speechFilterFadeMode")] =
        m_speechFilterEnabled
            ? AudioEngine::speechFilterFadeModeToString(m_speechFilterFadeMode)
            : QStringLiteral("none");
    root[QStringLiteral("speechFilterFadeSamples")] = m_speechFilterFadeSamples;
    root[QStringLiteral("speechFilterCurveStrength")] = m_speechFilterCurveStrength;
    root[QStringLiteral("speechFilterRangeCrossfade")] = m_speechFilterRangeCrossfade;
    root[QStringLiteral("transcriptUnifiedEditColors")] =
        m_inspectorPane && m_inspectorPane->transcriptUnifiedEditModeCheckBox()
            ? m_inspectorPane->transcriptUnifiedEditModeCheckBox()->isChecked()
            : true;
    root[QStringLiteral("transcriptShowExcludedLines")] =
        m_inspectorPane && m_inspectorPane->transcriptShowExcludedLinesCheckBox()
            ? m_inspectorPane->transcriptShowExcludedLinesCheckBox()->isChecked()
            : false;
    root[QStringLiteral("transcriptSpeakerFilterValue")] =
        m_inspectorPane && m_inspectorPane->transcriptSpeakerFilterCombo()
            ? m_inspectorPane->transcriptSpeakerFilterCombo()->currentData().toString()
            : QString();
    root[QStringLiteral("transcriptActiveCutPath")] =
        m_inspectorPane && m_inspectorPane->transcriptScriptVersionCombo()
            ? m_inspectorPane->transcriptScriptVersionCombo()->currentData().toString()
            : QString();
    QJsonArray transcriptColumnHidden;
    if (m_transcriptTable) {
        transcriptColumnHidden = QJsonArray();
        for (int i = 0; i < m_transcriptTable->columnCount(); ++i) {
            transcriptColumnHidden.push_back(m_transcriptTable->isColumnHidden(i));
        }
    }
    root[QStringLiteral("transcriptColumnHidden")] = transcriptColumnHidden;
    QJsonArray speakersColumnHidden;
    if (m_inspectorPane) {
        if (const SpeakersTable* speakersTable =
                qobject_cast<SpeakersTable*>(m_inspectorPane->speakersTable())) {
            speakersColumnHidden = speakersTable->hiddenColumnsState();
        }
    }
    root[QStringLiteral("speakersColumnHidden")] = speakersColumnHidden;
    root[QStringLiteral("transcriptFollowCurrentWord")] =
        m_transcriptFollowCurrentWordCheckBox
            ? m_transcriptFollowCurrentWordCheckBox->isChecked()
            : true;
    root[QStringLiteral("gradingFollowCurrent")] =
        m_gradingFollowCurrentCheckBox ? m_gradingFollowCurrentCheckBox->isChecked() : true;
    root[QStringLiteral("gradingAutoScroll")] =
        m_gradingAutoScrollCheckBox ? m_gradingAutoScrollCheckBox->isChecked() : true;
    root[QStringLiteral("gradingPreview")] =
        m_bypassGradingCheckBox ? m_bypassGradingCheckBox->isChecked() : true;
    root[QStringLiteral("keyframesFollowCurrent")] =
        m_keyframesFollowCurrentCheckBox ? m_keyframesFollowCurrentCheckBox->isChecked() : true;
    root[QStringLiteral("keyframesAutoScroll")] =
        m_keyframesAutoScrollCheckBox ? m_keyframesAutoScrollCheckBox->isChecked() : true;
    root[QStringLiteral("correctionsEnabled")] = m_correctionsEnabled;
    root[QStringLiteral("selectedInspectorTab")] =
        m_inspectorTabs ? m_inspectorTabs->currentIndex() : 0;
    root[QStringLiteral("selectedInspectorTabLabel")] =
        (m_inspectorTabs && m_inspectorTabs->currentIndex() >= 0)
            ? m_inspectorTabs->tabText(m_inspectorTabs->currentIndex())
            : QString();
    const PlaybackRuntimeConfig playbackConfig = playbackRuntimeConfig();
    root[QStringLiteral("playbackSpeed")] = playbackConfig.speed;
    root[QStringLiteral("exportPlaybackSpeed")] =
        std::isfinite(m_exportPlaybackSpeed) && m_exportPlaybackSpeed > 0.001
            ? m_exportPlaybackSpeed
            : playbackConfig.speed;
    root[QStringLiteral("playbackClockSource")] =
        playbackClockSourceToString(playbackConfig.clockSource);
    root[QStringLiteral("playbackClockSourceExplicit")] = true;
    root[QStringLiteral("playbackAudioWarpMode")] =
        playbackAudioWarpModeToString(playbackConfig.audioWarpMode);
    root[QStringLiteral("playbackAudioWarpModeExplicit")] = true;
    root[QStringLiteral("playbackLoopEnabled")] = playbackConfig.loopEnabled;
    root[QStringLiteral("previewViewMode")] = m_previewViewMode;
    root[QStringLiteral("render_backend")] = m_renderBackendPreference;
    root[QStringLiteral("preview_vulkan_presenter")] = m_previewVulkanPresenterPreference;
    root[QStringLiteral("aiSelectedModel")] = m_aiSelectedModel;
    root[QStringLiteral("aiProxyBaseUrl")] = m_aiProxyBaseUrl;
    root[QStringLiteral("feature_ai_panel")] = m_featureAiPanel;
    root[QStringLiteral("feature_ai_speaker_cleanup")] = m_featureAiSpeakerCleanup;
    root[QStringLiteral("feature_audio_preview_mode")] = m_featureAudioPreviewMode;
    root[QStringLiteral("feature_audio_dynamics_tools")] = m_featureAudioDynamicsTools;
    root[QStringLiteral("aiUsageBudgetCap")] = m_aiUsageBudgetCap;
    root[QStringLiteral("aiUsageRequests")] = m_aiUsageRequests;
    root[QStringLiteral("aiUsageFailures")] = m_aiUsageFailures;
    root[QStringLiteral("aiRateLimitPerMinute")] = m_aiRateLimitPerMinute;
    root[QStringLiteral("aiRequestTimeoutMs")] = m_aiRequestTimeoutMs;
    root[QStringLiteral("aiRequestRetries")] = m_aiRequestRetries;
    root[QStringLiteral("audioAmplifyEnabled")] = m_previewAudioDynamics.amplifyEnabled;
    root[QStringLiteral("audioAmplifyDb")] = m_previewAudioDynamics.amplifyDb;
    root[QStringLiteral("audioSpeakerHoverModalEnabled")] = m_audioSpeakerHoverModalEnabled;
    root[QStringLiteral("audioWaveformVisible")] = m_audioWaveformVisible;
    root[QStringLiteral("audioVisualizationMode")] = static_cast<int>(m_audioVisualizationMode);
    root[QStringLiteral("loiaconoMultiple")] = m_loiaconoSpectrumSettings.multiple;
    root[QStringLiteral("loiaconoBins")] = m_loiaconoSpectrumSettings.bins;
    root[QStringLiteral("loiaconoFreqMin")] = m_loiaconoSpectrumSettings.freqMin;
    root[QStringLiteral("loiaconoFreqMax")] = m_loiaconoSpectrumSettings.freqMax;
    root[QStringLiteral("loiaconoSampleRate")] = m_loiaconoSpectrumSettings.sampleRate;
    root[QStringLiteral("loiaconoGain")] = m_loiaconoSpectrumSettings.gain;
    root[QStringLiteral("loiaconoGamma")] = m_loiaconoSpectrumSettings.gamma;
    root[QStringLiteral("loiaconoFloor")] = m_loiaconoSpectrumSettings.floor;
    root[QStringLiteral("loiaconoLeakiness")] = m_loiaconoSpectrumSettings.leakiness;
    root[QStringLiteral("loiaconoTemporalWeightingMode")] =
        m_loiaconoSpectrumSettings.temporalWeightingMode;
    root[QStringLiteral("loiaconoNormalizationMode")] = m_loiaconoSpectrumSettings.normalizationMode;
    root[QStringLiteral("loiaconoWindowLengthMode")] = m_loiaconoSpectrumSettings.windowLengthMode;
    root[QStringLiteral("loiaconoAlgorithmMode")] = m_loiaconoSpectrumSettings.algorithmMode;
    root[QStringLiteral("audioNormalizeEnabled")] = m_previewAudioDynamics.normalizeEnabled;
    root[QStringLiteral("audioNormalizeTargetDb")] = m_previewAudioDynamics.normalizeTargetDb;
    root[QStringLiteral("audioStereoToMonoEnabled")] = m_previewAudioDynamics.stereoToMonoEnabled;
    root[QStringLiteral("audioSelectiveNormalizeEnabled")] = m_previewAudioDynamics.selectiveNormalizeEnabled;
    root[QStringLiteral("audioSelectiveNormalizeMinSegmentSeconds")] =
        m_previewAudioDynamics.selectiveNormalizeMinSegmentSeconds;
    root[QStringLiteral("audioSelectiveNormalizePeakDb")] = m_previewAudioDynamics.selectiveNormalizePeakDb;
    root[QStringLiteral("audioSelectiveNormalizePasses")] = m_previewAudioDynamics.selectiveNormalizePasses;
    root[QStringLiteral("audioSelectiveNormalizeOverlayVisible")] =
        m_previewAudioDynamics.selectiveNormalizeOverlayVisible;
    root[QStringLiteral("audioTranscriptNormalizeEnabled")] =
        m_previewAudioDynamics.transcriptNormalizeEnabled;
    root[QStringLiteral("audioWaveformPreviewPostProcessing")] =
        m_previewAudioDynamics.waveformPreviewPostProcessing;
    root[QStringLiteral("audioPeakReductionEnabled")] = m_previewAudioDynamics.peakReductionEnabled;
    root[QStringLiteral("audioPeakThresholdDb")] = m_previewAudioDynamics.peakThresholdDb;
    root[QStringLiteral("audioLimiterEnabled")] = m_previewAudioDynamics.limiterEnabled;
    root[QStringLiteral("audioLimiterThresholdDb")] = m_previewAudioDynamics.limiterThresholdDb;
    root[QStringLiteral("audioCompressorEnabled")] = m_previewAudioDynamics.compressorEnabled;
    root[QStringLiteral("audioCompressorThresholdDb")] = m_previewAudioDynamics.compressorThresholdDb;
    root[QStringLiteral("audioCompressorRatio")] = m_previewAudioDynamics.compressorRatio;
    root[QStringLiteral("audioSoftClipEnabled")] = m_previewAudioDynamics.softClipEnabled;
    root[QStringLiteral("timelineZoom")] =
        m_timeline ? m_timeline->timelineZoom() : 4.0;
    root[QStringLiteral("timelineVerticalScroll")] =
        m_timeline ? m_timeline->verticalScrollOffset() : 0;
    root[QStringLiteral("exportStartFrame")] =
        m_timeline ? static_cast<qint64>(m_timeline->exportStartFrame()) : 0;
    root[QStringLiteral("exportEndFrame")] =
        m_timeline ? static_cast<qint64>(m_timeline->exportEndFrame()) : 0;

    QJsonArray exportRanges;
    if (m_timeline)
    {
        for (const ExportRangeSegment &range : m_timeline->exportRanges())
        {
            QJsonObject rangeObj;
            rangeObj[QStringLiteral("startFrame")] = static_cast<qint64>(range.startFrame);
            rangeObj[QStringLiteral("endFrame")] = static_cast<qint64>(range.endFrame);
            exportRanges.push_back(rangeObj);
        }
    }
    root[QStringLiteral("exportRanges")] = exportRanges;

    QJsonArray timeline;
    QJsonObject selectedClipObj;
    const QString selectedClipId =
        m_timeline ? m_timeline->selectedClipId() : QString();
    if (m_timeline)
    {
        for (const TimelineClip &clip : m_timeline->clips())
        {
            QJsonObject clipObj = clipToJson(clipWithRelativeStatePaths(clip, mediaRoot));
            // Keep project state lightweight: dense facial tracking keyframes live in transcript facedetections sidecars.
            clipObj.remove(QStringLiteral("speakerFramingKeyframes"));
            timeline.push_back(clipObj);
            if (!selectedClipId.isEmpty() && clip.id == selectedClipId)
            {
                selectedClipObj = clipObj;
            }
        }
    }
    root[QStringLiteral("timeline")] = timeline;
    root[QStringLiteral("selectedClip")] = selectedClipObj;

    QJsonArray renderSyncMarkers;
    if (m_timeline)
    {
        for (const RenderSyncMarker &marker : m_timeline->renderSyncMarkers())
        {
            QJsonObject markerObj;
            markerObj[QStringLiteral("clipId")] = marker.clipId;
            markerObj[QStringLiteral("frame")] = static_cast<qint64>(marker.frame);
            markerObj[QStringLiteral("action")] = renderSyncActionToString(marker.action);
            markerObj[QStringLiteral("count")] = marker.count;
            renderSyncMarkers.push_back(markerObj);
        }
    }
    root[QStringLiteral("renderSyncMarkers")] = renderSyncMarkers;

    QJsonArray tracks;
    if (m_timeline)
    {
        for (const TimelineTrack &track : m_timeline->tracks())
        {
            QJsonObject trackObj;
            trackObj[QStringLiteral("name")] = track.name;
            trackObj[QStringLiteral("height")] = track.height;
            trackObj[QStringLiteral("visualMode")] = trackVisualModeToString(track.visualMode);
            trackObj[QStringLiteral("audioEnabled")] = track.audioEnabled;
            trackObj[QStringLiteral("audioBusId")] = track.audioBusId;
            trackObj[QStringLiteral("audioGain")] = track.audioGain;
            trackObj[QStringLiteral("audioMuted")] = track.audioMuted;
            trackObj[QStringLiteral("audioSolo")] = track.audioSolo;
            trackObj[QStringLiteral("audioWaveformVisible")] = track.audioWaveformVisible;
            trackObj[QStringLiteral("effectPreset")] = effectPresetToJson(track.effectPreset);
            trackObj[QStringLiteral("effectRows")] = track.effectRows;
            trackObj[QStringLiteral("effectSpeed")] = track.effectSpeed;
            trackObj[QStringLiteral("effectScale")] = track.effectScale;
            trackObj[QStringLiteral("effectAlternateDirection")] = track.effectAlternateDirection;
            trackObj[QStringLiteral("tilingPattern")] = tilingPatternToJson(track.tilingPattern);
            trackObj[QStringLiteral("tilingSpacing")] = track.tilingSpacing;
            trackObj[QStringLiteral("tilingWrap")] = track.tilingWrap;
            tracks.push_back(trackObj);
        }
    }
    root[QStringLiteral("tracks")] = tracks;

    return root;
}

void EditorWindow::scheduleSaveState()
{
    if (m_loadingState || !m_timeline)
    {
        return;
    }
    if (playbackActive())
    {
        // Avoid synchronous state serialization work while the playback loop is active.
        m_pendingSaveAfterPlayback = true;
        return;
    }
    m_stateSaveTimer.start();
}

void EditorWindow::saveStateNow()
{
    if (!m_timeline)
    {
        return;
    }
    if (m_loadingState)
    {
        m_pendingSaveAfterLoad = true;
        return;
    }
    if (playbackActive())
    {
        m_pendingSaveAfterPlayback = true;
        return;
    }

    m_stateSaveTimer.stop();
    if (m_projectManager) {
        m_projectManager->ensureProjectsDirectory();
        QDir().mkpath(m_projectManager->projectPath(m_projectManager->currentProjectIdOrDefault()));
    }

    const QString statePath = m_projectManager ? m_projectManager->stateFilePath() : QString();
    if (statePath.trimmed().isEmpty()) {
        return;
    }

    if (m_stateSaveWatcher.isRunning()) {
        m_stateSavePending = true;
        return;
    }

    const QJsonObject state = buildStateJson();
    const QByteArray lastSavedState = m_lastSavedState;
    m_stateSaveWatcher.setFuture(QtConcurrent::run([statePath, state, lastSavedState]() {
        return writeStateObjectAtomic(statePath, state, lastSavedState);
    }));
}

void EditorWindow::flushStateSaveNow()
{
    if (m_stateSaveTimer.isActive()) {
        m_stateSaveTimer.stop();
    }
    saveStateNow();
    while (m_stateSaveWatcher.isRunning()) {
        m_stateSaveWatcher.waitForFinished();
        const QByteArray savedState = m_stateSaveWatcher.result();
        if (!savedState.isEmpty()) {
            m_lastSavedState = savedState;
            m_pendingSaveAfterPlayback = false;
        }
        if (m_stateSavePending) {
            m_stateSavePending = false;
            saveStateNow();
        }
    }
}

void EditorWindow::saveHistoryNow()
{
    if (m_projectManager) {
        m_projectManager->ensureProjectsDirectory();
        QDir().mkpath(m_projectManager->projectPath(m_projectManager->currentProjectIdOrDefault()));
    }

    QJsonObject root;
    QJsonArray sanitizedEntries = m_historyEntries;
    sanitizeHistoryEntriesInPlace(&sanitizedEntries);
    int sanitizedIndex = m_historyIndex;
    trimHistoryEntriesInPlace(&sanitizedEntries,
                              &sanitizedIndex,
                              m_historyMaxEntries,
                              m_historyMaxMegabytes);
    m_historyEntries = sanitizedEntries;
    m_historyIndex = sanitizedIndex;
    root = historyRootFromEntries(sanitizedEntries, m_historyIndex);

    const QString historyPath = m_projectManager ? m_projectManager->historyFilePath() : QString();
    if (historyPath.trimmed().isEmpty()) {
        return;
    }

    if (m_historySaveWatcher.isRunning()) {
        m_historySavePending = true;
        return;
    }

    m_historySaveWatcher.setFuture(QtConcurrent::run([historyPath, root]() {
        return writeJsonObjectAtomic(historyPath, root);
    }));
}

void EditorWindow::flushHistorySaveNow()
{
    if (m_historySaveTimer.isActive()) {
        m_historySaveTimer.stop();
    }
    saveHistoryNow();
    while (m_historySaveWatcher.isRunning()) {
        m_historySaveWatcher.waitForFinished();
        if (m_historySavePending) {
            m_historySavePending = false;
            saveHistoryNow();
        }
    }
}

void EditorWindow::trimHistoryToConfiguredLimits()
{
    const int beforeSize = m_historyEntries.size();
    const int beforeIndex = m_historyIndex;
    trimHistoryEntriesInPlace(&m_historyEntries,
                              &m_historyIndex,
                              m_historyMaxEntries,
                              m_historyMaxMegabytes);
    if (m_historyEntries.size() != beforeSize || m_historyIndex != beforeIndex) {
        saveHistoryNow();
        if (m_historyTab) {
            m_historyTab->refresh();
        }
    }
}

void EditorWindow::pushHistorySnapshot()
{
    if (m_loadingState || m_restoringHistory || m_suppressHistorySnapshots || !m_timeline)
    {
        return;
    }

    QJsonObject snapshot = buildStateJson();
    stripHeavyStateSnapshot(&snapshot);
    if (m_historyIndex >= 0 &&
        m_historyIndex < m_historyEntries.size() &&
        m_historyEntries.at(m_historyIndex).toObject() == snapshot)
    {
        return;
    }

    while (m_historyEntries.size() > m_historyIndex + 1)
    {
        m_historyEntries.removeAt(m_historyEntries.size() - 1);
    }

    m_historyEntries.append(snapshot);
    m_historyIndex = m_historyEntries.size() - 1;
    trimHistoryEntriesInPlace(&m_historyEntries,
                              &m_historyIndex,
                              m_historyMaxEntries,
                              m_historyMaxMegabytes);
    if (!m_historySaveTimer.isActive()) {
        m_historySaveTimer.start();
    }
}

void EditorWindow::setupAutosaveTimer()
{
    m_autosaveIntervalMinutes = qBound(1, m_autosaveIntervalMinutes, 120);
    m_autosaveMaxBackups = qBound(1, m_autosaveMaxBackups, 200);
    m_autosaveTimer.setSingleShot(false);
    m_autosaveTimer.setInterval(m_autosaveIntervalMinutes * 60 * 1000);
    connect(&m_autosaveTimer, &QTimer::timeout, this, [this]() { saveAutosaveBackup(); });
    m_autosaveTimer.start();
    if (m_startupProfileCompleted) {
        saveAutosaveBackup();
    } else {
        QTimer::singleShot(5000, this, [this]() { saveAutosaveBackup(); });
    }
}

void EditorWindow::saveAutosaveBackup()
{
    if (!m_timeline || m_loadingState)
    {
        return;
    }

    if (m_projectManager) {
        m_projectManager->ensureProjectsDirectory();
    }
    const QString projectDir = m_projectManager
        ? m_projectManager->projectPath(m_projectManager->currentProjectIdOrDefault())
        : QString();
    QDir().mkpath(projectDir);

    const QDateTime now = QDateTime::currentDateTime();
    const QString backupFileName = QStringLiteral("state_backup_%1.json").arg(now.toString(QStringLiteral("yyyy-MM-dd_HH-mm-ss")));
    const QString backupPath = QDir(projectDir).filePath(backupFileName);

    const QByteArray serializedState = jcut::jsonio::serializeIndented(buildStateJson());

    QSaveFile file(backupPath);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate))
    {
        return;
    }

    if (file.write(serializedState) != serializedState.size())
    {
        file.cancelWriting();
        return;
    }

    file.commit();
    qDebug() << "[AUTOSAVE] Backup saved:" << backupPath;

    QDir autosaveDir(projectDir);
    const QStringList backups = autosaveDir.entryList(QStringList(QStringLiteral("state_backup_*.json")), QDir::Files, QDir::Name);
    if (backups.size() > m_autosaveMaxBackups)
    {
        const int removeCount = backups.size() - m_autosaveMaxBackups;
        for (int i = 0; i < removeCount; ++i) {
            QDir(projectDir).remove(backups.at(i));
        }
    }
}
