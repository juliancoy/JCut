// project_state.cpp
#include "editor.h"
#include "clip_serialization.h"
#include "debug_controls.h"

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

using namespace editor;

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
    loadProjectsFromFolders();
    markStartup(QStringLiteral("load_state.projects_loaded"));
    qDebug() << "[PROJECT] Loading project:" << currentProjectIdOrDefault();
    qDebug() << "[PROJECT] State file:" << stateFilePath();
    qDebug() << "[PROJECT] History file:" << historyFilePath();
    m_historyEntries = QJsonArray();
    m_historyIndex = -1;
    m_lastSavedState.clear();

    QJsonObject root;
    QFile historyFile(historyFilePath());
    markStartup(QStringLiteral("load_state.history_read.begin"));
    if (historyFile.open(QIODevice::ReadOnly))
    {
        const QJsonObject historyRoot = QJsonDocument::fromJson(historyFile.readAll()).object();
        m_historyEntries = historyRoot.value(QStringLiteral("entries")).toArray();
        const bool historySanitized = sanitizeHistoryEntriesInPlace(&m_historyEntries);
        m_historyIndex = historyRoot.value(QStringLiteral("index")).toInt(m_historyEntries.size() - 1);
        if (!m_historyEntries.isEmpty())
        {
            m_historyIndex = qBound(0, m_historyIndex, m_historyEntries.size() - 1);
            root = m_historyEntries.at(m_historyIndex).toObject();
        }
        if (historySanitized) {
            saveHistoryNow();
        }
    }
    markStartup(QStringLiteral("load_state.history_read.end"),
                QJsonObject{
                    {QStringLiteral("history_entry_count"), m_historyEntries.size()},
                    {QStringLiteral("history_index"), m_historyIndex}
                });

    if (root.isEmpty())
    {
        markStartup(QStringLiteral("load_state.state_file_read.begin"));
        QFile file(stateFilePath());
        if (file.open(QIODevice::ReadOnly))
        {
            root = QJsonDocument::fromJson(file.readAll()).object();
        }
        markStartup(QStringLiteral("load_state.state_file_read.end"));
    }

    markStartup(QStringLiteral("load_state.apply_state.begin"));
    applyStateJson(root);
    markStartup(QStringLiteral("load_state.apply_state.end"));

    {
        QSet<QString> transcriptPaths;
        if (m_timeline) {
            const QVector<TimelineClip> clips = m_timeline->clips();
            for (const TimelineClip& clip : clips) {
                const QString clipPath = clip.filePath.trimmed();
                if (clipPath.isEmpty()) {
                    continue;
                }
                const QString originalPath = transcriptPathForClipFile(clipPath);
                const QString editablePath = transcriptEditablePathForClipFile(clipPath);
                const QString activePath = activeTranscriptPathForClipFile(clipPath);
                if (!originalPath.isEmpty()) {
                    transcriptPaths.insert(originalPath);
                }
                if (!editablePath.isEmpty()) {
                    transcriptPaths.insert(editablePath);
                }
                if (!activePath.isEmpty()) {
                    transcriptPaths.insert(activePath);
                }
            }
        }

        int createdTxtCount = 0;
        for (const QString& transcriptPath : transcriptPaths) {
            const QFileInfo txtInfo(QFileInfo(transcriptPath).dir().filePath(
                QFileInfo(transcriptPath).completeBaseName() + QStringLiteral(".txt")));
            if (txtInfo.exists()) {
                continue;
            }
            if (m_transcriptEngine.ensureTranscriptTextCompanion(transcriptPath)) {
                ++createdTxtCount;
            }
        }
        markStartup(QStringLiteral("load_state.transcript_txt_backfill"),
                    QJsonObject{
                        {QStringLiteral("transcript_path_count"), transcriptPaths.size()},
                        {QStringLiteral("txt_created_count"), createdTxtCount}
                    });
    }

    if (m_historyEntries.isEmpty())
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
    markStartup(QStringLiteral("load_state.end"));
}

QString EditorWindow::configFilePath() const
{
    // Config file stored near the executable
    return QDir(QCoreApplication::applicationDirPath()).filePath(QStringLiteral("editor.config"));
}

QString EditorWindow::rootDirPath() const
{
    // Read root directory from config file, or default to executable directory
    QFile configFile(configFilePath());
    if (configFile.open(QIODevice::ReadOnly | QIODevice::Text)) {
        const QString path = QString::fromUtf8(configFile.readAll()).trimmed();
        if (!path.isEmpty() && QDir(path).exists()) {
            return path;
        }
    }
    // Default to executable directory if no valid config
    return QCoreApplication::applicationDirPath();
}

void EditorWindow::setRootDirPath(const QString& path)
{
    if (path.isEmpty()) {
        return;
    }
    QSaveFile config(configFilePath());
    if (config.open(QIODevice::WriteOnly | QIODevice::Truncate | QIODevice::Text)) {
        config.write(path.toUtf8());
        config.commit();
    }
}

QString EditorWindow::projectsDirPath() const
{
    // Projects are stored in a "projects" subfolder of the Root directory
    return QDir(rootDirPath()).filePath(QStringLiteral("projects"));
}

QString EditorWindow::currentProjectMarkerPath() const
{
    return QDir(projectsDirPath()).filePath(QStringLiteral(".current_project"));
}

QString EditorWindow::currentProjectIdOrDefault() const
{
    return m_currentProjectId.isEmpty() ? QStringLiteral("default") : m_currentProjectId;
}

QString EditorWindow::projectPath(const QString &projectId) const
{
    return QDir(projectsDirPath()).filePath(projectId.isEmpty() ? QStringLiteral("default") : projectId);
}

QString EditorWindow::stateFilePathForProject(const QString &projectId) const
{
    return QDir(projectPath(projectId)).filePath(QStringLiteral("state.json"));
}

QString EditorWindow::historyFilePathForProject(const QString &projectId) const
{
    return QDir(projectPath(projectId)).filePath(QStringLiteral("history.json"));
}

QString EditorWindow::stateFilePath() const
{
    return stateFilePathForProject(currentProjectIdOrDefault());
}

QString EditorWindow::historyFilePath() const
{
    return historyFilePathForProject(currentProjectIdOrDefault());
}

QString EditorWindow::sanitizedProjectId(const QString &name) const
{
    QString id = name.trimmed().toLower();
    for (QChar &ch : id)
    {
        if (!(ch.isLetterOrNumber() || ch == QLatin1Char('_') || ch == QLatin1Char('-')))
        {
            ch = QLatin1Char('-');
        }
    }

    while (id.contains(QStringLiteral("--")))
    {
        id.replace(QStringLiteral("--"), QStringLiteral("-"));
    }

    id.remove(QRegularExpression(QStringLiteral("^-+|-+$")));
    if (id.isEmpty())
    {
        id = QStringLiteral("project");
    }

    QString uniqueId = id;
    int suffix = 2;
    while (QFileInfo::exists(projectPath(uniqueId)))
    {
        uniqueId = QStringLiteral("%1-%2").arg(id).arg(suffix++);
    }
    return uniqueId;
}

void EditorWindow::ensureProjectsDirectory() const
{
    QDir().mkpath(projectsDirPath());
}

QStringList EditorWindow::availableProjectIds() const
{
    ensureProjectsDirectory();
    const QFileInfoList entries =
        QDir(projectsDirPath()).entryInfoList(QDir::Dirs | QDir::NoDotAndDotDot,
                                             QDir::Name | QDir::IgnoreCase);

    QStringList ids;
    ids.reserve(entries.size());
    for (const QFileInfo &entry : entries)
    {
        ids.push_back(entry.fileName());
    }
    return ids;
}

void EditorWindow::ensureDefaultProjectExists() const
{
    ensureProjectsDirectory();
    QDir().mkpath(projectPath(QStringLiteral("default")));
}

void EditorWindow::loadProjectsFromFolders()
{
    ensureDefaultProjectExists();

    QFile markerFile(currentProjectMarkerPath());
    if (markerFile.open(QIODevice::ReadOnly))
    {
        m_currentProjectId = QString::fromUtf8(markerFile.readAll()).trimmed();
    }

    const QStringList projectIds = availableProjectIds();
    if (projectIds.isEmpty())
    {
        m_currentProjectId = QStringLiteral("default");
        return;
    }

    if (m_currentProjectId.isEmpty() || !projectIds.contains(m_currentProjectId))
    {
        m_currentProjectId = projectIds.contains(QStringLiteral("default"))
                                 ? QStringLiteral("default")
                                 : projectIds.constFirst();
    }

    QSaveFile marker(currentProjectMarkerPath());
    if (marker.open(QIODevice::WriteOnly | QIODevice::Truncate))
    {
        const QByteArray payload = m_currentProjectId.toUtf8();
        if (marker.write(payload) == payload.size())
        {
            marker.commit();
        }
        else
        {
            marker.cancelWriting();
        }
    }
}

void EditorWindow::saveCurrentProjectMarker()
{
    ensureProjectsDirectory();

    QSaveFile file(currentProjectMarkerPath());
    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate))
    {
        return;
    }

    const QByteArray payload = currentProjectIdOrDefault().toUtf8();
    if (file.write(payload) != payload.size())
    {
        file.cancelWriting();
        return;
    }

    file.commit();
}

QString EditorWindow::currentProjectName() const
{
    return currentProjectIdOrDefault();
}

void EditorWindow::refreshProjectsList()
{
    if (!m_projectsTab) {
        return;
    }
    loadProjectsFromFolders();
    m_projectsTab->refresh();
}

void EditorWindow::switchToProject(const QString &projectId)
{
    if (projectId.isEmpty() || projectId == currentProjectIdOrDefault())
    {
        refreshProjectsList();
        return;
    }

    saveStateNow();
    saveHistoryNow();

    m_currentProjectId = projectId;
    m_lastSavedState.clear();
    m_historyEntries = QJsonArray();
    m_historyIndex = -1;

    saveCurrentProjectMarker();
    loadState();
    refreshProjectsList();
    m_inspectorPane->refresh();
}

void EditorWindow::createProject()
{
    bool accepted = false;
    const QString name = QInputDialog::getText(this,
                                               QStringLiteral("New Project"),
                                               QStringLiteral("Project name"),
                                               QLineEdit::Normal,
                                               QStringLiteral("Untitled Project"),
                                               &accepted)
                             .trimmed();
    if (!accepted || name.isEmpty())
    {
        return;
    }

    const QString projectId = sanitizedProjectId(name);
    QDir().mkpath(projectPath(projectId));
    switchToProject(projectId);
}

bool EditorWindow::saveProjectPayload(const QString &projectId,
                                      const QByteArray &statePayload,
                                      const QByteArray &historyPayload)
{
    ensureProjectsDirectory();
    QDir().mkpath(projectPath(projectId));

    QSaveFile stateFile(stateFilePathForProject(projectId));
    if (!stateFile.open(QIODevice::WriteOnly | QIODevice::Truncate))
    {
        return false;
    }
    if (stateFile.write(statePayload) != statePayload.size())
    {
        stateFile.cancelWriting();
        return false;
    }
    if (!stateFile.commit())
    {
        return false;
    }

    QSaveFile historyFile(historyFilePathForProject(projectId));
    if (!historyFile.open(QIODevice::WriteOnly | QIODevice::Truncate))
    {
        return false;
    }
    if (historyFile.write(historyPayload) != historyPayload.size())
    {
        historyFile.cancelWriting();
        return false;
    }

    return historyFile.commit();
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
                                               currentProjectName() == QStringLiteral("Default Project")
                                                   ? QStringLiteral("Untitled Project")
                                                   : currentProjectName(),
                                               &accepted)
                             .trimmed();
    if (!accepted || name.isEmpty())
    {
        return;
    }

    saveStateNow();
    saveHistoryNow();

    const QString newProjectId = sanitizedProjectId(name);
    const QByteArray statePayload =
        QJsonDocument(buildStateJson()).toJson(QJsonDocument::Indented);

    QJsonObject historyRoot;
    historyRoot[QStringLiteral("index")] = m_historyIndex;
    historyRoot[QStringLiteral("entries")] = m_historyEntries;
    const QByteArray historyPayload =
        QJsonDocument(historyRoot).toJson(QJsonDocument::Indented);

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
    if (projectId.isEmpty() || !QFileInfo::exists(projectPath(projectId)))
    {
        return;
    }

    bool accepted = false;
    const QString name = QInputDialog::getText(this,
                                               QStringLiteral("Rename Project"),
                                               QStringLiteral("Project name"),
                                               QLineEdit::Normal,
                                               projectId,
                                               &accepted)
                             .trimmed();
    if (!accepted || name.isEmpty())
    {
        return;
    }

    const QString renamedProjectId = sanitizedProjectId(name);
    if (renamedProjectId == projectId)
    {
        return;
    }

    QDir projectsDir(projectsDirPath());
    if (!projectsDir.rename(projectId, renamedProjectId))
    {
        QMessageBox::warning(this,
                             QStringLiteral("Rename Project Failed"),
                             QStringLiteral("Could not rename the project folder."));
        return;
    }

    if (m_currentProjectId == projectId)
    {
        m_currentProjectId = renamedProjectId;
        saveCurrentProjectMarker();
    }

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
    root[QStringLiteral("outputFormat")] =
        m_outputFormatCombo ? m_outputFormatCombo->currentData().toString()
                            : QStringLiteral("mp4");
    root[QStringLiteral("lastRenderOutputPath")] = m_lastRenderOutputPath;
    root[QStringLiteral("renderUseProxies")] =
        m_renderUseProxiesCheckBox ? m_renderUseProxiesCheckBox->isChecked() : false;
    root[QStringLiteral("previewHideOutsideOutput")] =
        m_previewHideOutsideOutputCheckBox ? m_previewHideOutsideOutputCheckBox->isChecked() : false;
    root[QStringLiteral("previewShowSpeakerTrackPoints")] =
        m_previewShowSpeakerTrackPointsCheckBox ? m_previewShowSpeakerTrackPointsCheckBox->isChecked() : false;
    root[QStringLiteral("previewShowSpeakerTrackBoxes")] =
        m_speakerShowBoxStreamBoxesCheckBox ? m_speakerShowBoxStreamBoxesCheckBox->isChecked() : false;
    root[QStringLiteral("previewBoxstreamOverlaySource")] =
        m_speakerBoxStreamOverlaySourceCombo
            ? m_speakerBoxStreamOverlaySourceCombo->currentData().toString()
            : QStringLiteral("all");
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
    root[QStringLiteral("debugDecodeMode")] =
        editor::decodePreferenceToString(editor::debugDecodePreference());
    root[QStringLiteral("debugH26xSoftwareThreadingMode")] =
        editor::h26xSoftwareThreadingModeToString(editor::debugH26xSoftwareThreadingMode());
    root[QStringLiteral("debugDeterministicPipeline")] =
        editor::debugDeterministicPipelineEnabled();
    root[QStringLiteral("autosaveIntervalMinutes")] = m_autosaveIntervalMinutes;
    root[QStringLiteral("autosaveMaxBackups")] = m_autosaveMaxBackups;
    root[QStringLiteral("speechFilterEnabled")] =
        m_speechFilterEnabledCheckBox ? m_speechFilterEnabledCheckBox->isChecked() : false;
    root[QStringLiteral("transcriptPrependMs")] = m_transcriptPrependMs;
    root[QStringLiteral("transcriptPostpendMs")] = m_transcriptPostpendMs;
    root[QStringLiteral("speechFilterFadeSamples")] = m_speechFilterFadeSamples;
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
    root[QStringLiteral("playbackClockSource")] =
        playbackClockSourceToString(playbackConfig.clockSource);
    root[QStringLiteral("playbackAudioWarpMode")] =
        playbackAudioWarpModeToString(playbackConfig.audioWarpMode);
    root[QStringLiteral("playbackLoopEnabled")] = playbackConfig.loopEnabled;
    root[QStringLiteral("previewViewMode")] = m_previewViewMode;
    root[QStringLiteral("render_backend")] = m_renderBackendPreference;
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
    root[QStringLiteral("audioNormalizeEnabled")] = m_previewAudioDynamics.normalizeEnabled;
    root[QStringLiteral("audioNormalizeTargetDb")] = m_previewAudioDynamics.normalizeTargetDb;
    root[QStringLiteral("audioSelectiveNormalizeEnabled")] = m_previewAudioDynamics.selectiveNormalizeEnabled;
    root[QStringLiteral("audioSelectiveNormalizeMinSegmentSeconds")] =
        m_previewAudioDynamics.selectiveNormalizeMinSegmentSeconds;
    root[QStringLiteral("audioSelectiveNormalizePeakDb")] = m_previewAudioDynamics.selectiveNormalizePeakDb;
    root[QStringLiteral("audioSelectiveNormalizePasses")] = m_previewAudioDynamics.selectiveNormalizePasses;
    root[QStringLiteral("audioWaveformPreviewPostProcessing")] =
        m_previewAudioDynamics.waveformPreviewPostProcessing;
    root[QStringLiteral("audioPeakReductionEnabled")] = m_previewAudioDynamics.peakReductionEnabled;
    root[QStringLiteral("audioPeakThresholdDb")] = m_previewAudioDynamics.peakThresholdDb;
    root[QStringLiteral("audioLimiterEnabled")] = m_previewAudioDynamics.limiterEnabled;
    root[QStringLiteral("audioLimiterThresholdDb")] = m_previewAudioDynamics.limiterThresholdDb;
    root[QStringLiteral("audioCompressorEnabled")] = m_previewAudioDynamics.compressorEnabled;
    root[QStringLiteral("audioCompressorThresholdDb")] = m_previewAudioDynamics.compressorThresholdDb;
    root[QStringLiteral("audioCompressorRatio")] = m_previewAudioDynamics.compressorRatio;
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
            QJsonObject clipObj = clipToJson(clip);
            // Keep project state lightweight: dense facial tracking keyframes live in transcript boxstream sidecars.
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
    ensureProjectsDirectory();
    QDir().mkpath(projectPath(currentProjectIdOrDefault()));

    const QByteArray serializedState =
        QJsonDocument(buildStateJson()).toJson(QJsonDocument::Indented);
    if (serializedState == m_lastSavedState)
    {
        return;
    }

    QSaveFile file(stateFilePath());
    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate))
    {
        return;
    }

    if (file.write(serializedState) != serializedState.size())
    {
        file.cancelWriting();
        return;
    }

    if (!file.commit())
    {
        return;
    }

    m_lastSavedState = serializedState;
    m_pendingSaveAfterPlayback = false;
}

void EditorWindow::saveHistoryNow()
{
    ensureProjectsDirectory();
    QDir().mkpath(projectPath(currentProjectIdOrDefault()));

    QJsonObject root;
    root[QStringLiteral("index")] = m_historyIndex;
    QJsonArray sanitizedEntries = m_historyEntries;
    sanitizeHistoryEntriesInPlace(&sanitizedEntries);
    m_historyEntries = sanitizedEntries;
    root[QStringLiteral("entries")] = sanitizedEntries;

    QSaveFile file(historyFilePath());
    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate))
    {
        return;
    }

    const QByteArray payload =
        QJsonDocument(root).toJson(QJsonDocument::Indented);
    if (file.write(payload) != payload.size())
    {
        file.cancelWriting();
        return;
    }

    file.commit();
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
    if (m_historyEntries.size() > 200)
    {
        m_historyEntries.removeAt(0);
    }

    m_historyIndex = m_historyEntries.size() - 1;
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
    saveAutosaveBackup();
}

void EditorWindow::saveAutosaveBackup()
{
    if (!m_timeline || m_loadingState)
    {
        return;
    }

    ensureProjectsDirectory();
    const QString projectDir = projectPath(currentProjectIdOrDefault());
    QDir().mkpath(projectDir);

    const QDateTime now = QDateTime::currentDateTime();
    const QString backupFileName = QStringLiteral("state_backup_%1.json").arg(now.toString(QStringLiteral("yyyy-MM-dd_HH-mm-ss")));
    const QString backupPath = QDir(projectDir).filePath(backupFileName);

    const QByteArray serializedState = QJsonDocument(buildStateJson()).toJson(QJsonDocument::Indented);

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
