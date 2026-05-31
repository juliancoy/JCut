// project_state.cpp
#include "editor.h"
#include "json_io_utils.h"
#include "speakers_table.h"
#include "clip_serialization.h"
#include "debug_controls.h"
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
            {QStringLiteral("history_index"), -1}
        };
        QFile historyFile(historyPath);
        if (!historyFile.open(QIODevice::ReadOnly)) {
            return result;
        }
        QJsonObject historyRoot;
        jcut::jsonio::parseObjectBytes(historyFile.readAll(), &historyRoot);
        QJsonArray entries = historyRoot.value(QStringLiteral("entries")).toArray();
        sanitizeHistoryEntriesInPlace(&entries);
        const int entryCount = entries.size();
        const int index = entryCount > 0
            ? qBound(0, historyRoot.value(QStringLiteral("index")).toInt(entryCount - 1), entryCount - 1)
            : -1;
        result[QStringLiteral("entries")] = entries;
        result[QStringLiteral("entry_count")] = entryCount;
        result[QStringLiteral("history_index")] = index;
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

void EditorWindow::switchToProject(const QString &projectId)
{
    if (projectId.isEmpty() ||
        projectId == (m_projectManager ? m_projectManager->currentProjectIdOrDefault()
                                       : QStringLiteral("default")))
    {
        refreshProjectsList();
        return;
    }

    saveStateNow();
    saveHistoryNow();

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

    saveStateNow();
    saveHistoryNow();

    const QString newProjectId = m_projectManager ? m_projectManager->sanitizedProjectId(name) : QString();
    const QByteArray statePayload =
        jcut::jsonio::serializeIndented(buildStateJson());

    QJsonObject historyRoot;
    historyRoot[QStringLiteral("index")] = m_historyIndex;
    historyRoot[QStringLiteral("entries")] = m_historyEntries;
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
        m_speakerShowFaceDetectionsBoxesCheckBox ? m_speakerShowFaceDetectionsBoxesCheckBox->isChecked() : false;
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
    root[QStringLiteral("debugDecodeMode")] =
        editor::decodePreferenceToString(editor::debugDecodePreference());
    root[QStringLiteral("debugH26xSoftwareThreadingMode")] =
        editor::h26xSoftwareThreadingModeToString(editor::debugH26xSoftwareThreadingMode());
    root[QStringLiteral("debugDeterministicPipeline")] =
        editor::debugDeterministicPipelineEnabled();
    root[QStringLiteral("autosaveIntervalMinutes")] = m_autosaveIntervalMinutes;
    root[QStringLiteral("autosaveMaxBackups")] = m_autosaveMaxBackups;
    root[QStringLiteral("speechFilterEnabled")] = m_speechFilterEnabled;
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
    root[QStringLiteral("playbackClockSource")] =
        playbackClockSourceToString(playbackConfig.clockSource);
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

    const QByteArray serializedState =
        jcut::jsonio::serializeIndented(buildStateJson());
    if (serializedState == m_lastSavedState)
    {
        return;
    }

    QSaveFile file(m_projectManager ? m_projectManager->stateFilePath() : QString());
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
    if (m_projectManager) {
        m_projectManager->ensureProjectsDirectory();
        QDir().mkpath(m_projectManager->projectPath(m_projectManager->currentProjectIdOrDefault()));
    }

    QJsonObject root;
    root[QStringLiteral("index")] = m_historyIndex;
    QJsonArray sanitizedEntries = m_historyEntries;
    sanitizeHistoryEntriesInPlace(&sanitizedEntries);
    m_historyEntries = sanitizedEntries;
    root[QStringLiteral("entries")] = sanitizedEntries;

    QSaveFile file(m_projectManager ? m_projectManager->historyFilePath() : QString());
    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate))
    {
        return;
    }

    const QByteArray payload =
        jcut::jsonio::serializeIndented(root);
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
