#include "editor.h"
#include "background_fill_effect.h"
#include "mask_tab.h"
#include "speakers_table.h"
#include "keyframe_table_shared.h"
#include "clip_serialization.h"
#include "editor_effect_presets.h"
#include "editor_shared_effects.h"
#include "editor_shared_media.h"
#include "transform_skip_aware_timing.h"
#include "debug_controls.h"
#include "speaker_export_harness.h"

#include <QApplication>
#include <QCommandLineOption>
#include <QCommandLineParser>
#include <QCoreApplication>
#include <QDateTime>
#include <QDebug>
#include <QDialog>
#include <QDir>
#include <QFileDialog>
#include <QFileInfo>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QInputDialog>
#include <QKeyEvent>
#include <QLabel>
#include <QLineEdit>
#include <QMenu>
#include <QMessageBox>
#include <QPlainTextEdit>
#include <QProcess>
#include <QProgressBar>
#include <QProgressDialog>
#include <QGridLayout>
#include <QSaveFile>
#include <QShortcut>
#include <QSignalBlocker>
#include <QSet>
#include <QStandardPaths>
#include <QStyle>
#include <QTextCursor>
#include <QTextStream>
#include <QTemporaryFile>
#include <QUrl>
#include <QVBoxLayout>
#include <QtConcurrent/QtConcurrentRun>

#include <cmath>
#include <cstring>

using namespace editor;

#include "playback_debug.h"

namespace {
QColor loadedColorValue(const QJsonObject& root, const QString& key, const QColor& fallback)
{
    const QColor color(root.value(key).toString(fallback.name(QColor::HexRgb)));
    if (!color.isValid()) {
        return fallback;
    }
    return QColor(color.red(), color.green(), color.blue(), fallback.alpha());
}

void setEditorColorButtonSwatch(QPushButton* button, const QColor& color)
{
    if (!button || !color.isValid()) {
        return;
    }
    const QColor opaque(color.red(), color.green(), color.blue());
    button->setText(opaque.name(QColor::HexRgb));
    button->setStyleSheet(
        QStringLiteral("QPushButton { background: %1; color: %2; "
                       "border: 1px solid #2e3b4a; border-radius: 4px; padding: 3px 8px; }")
            .arg(opaque.name(QColor::HexRgb),
                 opaque.lightness() > 128 ? QStringLiteral("#000000")
                                          : QStringLiteral("#ffffff")));
}

bool restVulkanDiagnosticsModeEnabled()
{
    const QString value = qEnvironmentVariable("JCUT_REST_VULKAN_DIAGNOSTICS").trimmed().toLower();
    return value == QStringLiteral("1") ||
           value == QStringLiteral("true") ||
           value == QStringLiteral("yes") ||
           value == QStringLiteral("on");
}

QString resolveStatePath(const QString& path, const QString& rootPath)
{
    const QString trimmed = path.trimmed();
    if (trimmed.isEmpty() || !QFileInfo(trimmed).isRelative()) {
        return path;
    }
    const QString basePath = rootPath.trimmed().isEmpty() ? QDir::currentPath() : rootPath;
    return QFileInfo(QDir(basePath).filePath(trimmed)).absoluteFilePath();
}

void resolveClipStatePaths(TimelineClip* clip, const QString& rootPath)
{
    if (!clip) {
        return;
    }
    clip->filePath = resolveStatePath(clip->filePath, rootPath);
    clip->proxyPath = resolveStatePath(clip->proxyPath, rootPath);
    clip->audioSourcePath = resolveStatePath(clip->audioSourcePath, rootPath);
    clip->audioSourceOriginalPath = resolveStatePath(clip->audioSourceOriginalPath, rootPath);
}

QJsonObject resolveClipStateObjectPaths(QJsonObject obj, const QString& rootPath)
{
    const QStringList pathKeys = {
        QStringLiteral("filePath"),
        QStringLiteral("proxyPath"),
        QStringLiteral("audioSourcePath"),
        QStringLiteral("audioSourceOriginalPath"),
    };
    for (const QString& key : pathKeys) {
        const QString path = obj.value(key).toString();
        if (!path.isEmpty()) {
            obj[key] = resolveStatePath(path, rootPath);
        }
    }
    return obj;
}

bool clipSupportsProgressiveEdgeStretchMigration(const TimelineClip& clip,
                                                 const QVector<TimelineTrack>& tracks)
{
    if (clip.clipRole != ClipRole::Media ||
        (clip.mediaType != ClipMediaType::Image && clip.mediaType != ClipMediaType::Video) ||
        playbackMediaPathForClip(clip).trimmed().isEmpty()) {
        return false;
    }
    return clipVisualPlaybackEnabled(clip, tracks);
}

bool hasUsableConfiguredProxy(const QVector<TimelineClip>& clips)
{
    for (const TimelineClip& clip : clips) {
        if (clip.mediaType == ClipMediaType::Video &&
            clip.useProxy &&
            !playbackProxyPathForClip(clip).isEmpty()) {
            return true;
        }
    }
    return false;
}

bool migrateLegacyBackgroundProgressiveStretchToClipEffect(
    QVector<TimelineClip>* clips,
    const QVector<TimelineTrack>& tracks,
    const QString& requestedClipId,
    const QString& selectedClipId,
    QString* migratedClipId)
{
    if (migratedClipId) {
        migratedClipId->clear();
    }
    if (!clips || clips->isEmpty()) {
        return false;
    }
    const QVector<TimelineClip>& currentClips = *clips;
    for (const TimelineClip& clip : currentClips) {
        const TimelineClip effective = clipWithTrackEffectSettings(clip, tracks);
        if (effective.effectPreset == ClipEffectPreset::ProgressiveEdgeStretch &&
            clipSupportsProgressiveEdgeStretchMigration(clip, tracks)) {
            if (migratedClipId) {
                *migratedClipId = clip.id;
            }
            return false;
        }
    }

    auto findIndexById = [&](const QString& id) -> int {
        const QString trimmed = id.trimmed();
        if (trimmed.isEmpty()) {
            return -1;
        }
        for (int i = 0; i < clips->size(); ++i) {
            if (clips->at(i).id == trimmed) {
                return i;
            }
        }
        return -1;
    };
    auto eligibleIndex = [&](int index) -> int {
        if (index >= 0 && index < clips->size() &&
            clipSupportsProgressiveEdgeStretchMigration(clips->at(index), tracks)) {
            return index;
        }
        return -1;
    };

    int targetIndex = eligibleIndex(findIndexById(requestedClipId));
    if (targetIndex < 0) {
        const int selectedIndex = findIndexById(selectedClipId);
        if (selectedIndex >= 0) {
            targetIndex = eligibleIndex(selectedIndex);
            if (targetIndex < 0) {
                targetIndex = eligibleIndex(findIndexById(clips->at(selectedIndex).linkedSourceClipId));
            }
        }
    }
    if (targetIndex < 0) {
        for (int i = 0; i < clips->size(); ++i) {
            if (clipSupportsProgressiveEdgeStretchMigration(clips->at(i), tracks)) {
                targetIndex = i;
                break;
            }
        }
    }
    if (targetIndex < 0) {
        return false;
    }

    TimelineClip& target = (*clips)[targetIndex];
    target.effectPreset = ClipEffectPreset::ProgressiveEdgeStretch;
    target.maskRepeatEnabled = false;
    if (migratedClipId) {
        *migratedClipId = target.id;
    }
    return true;
}

}

bool EditorWindow::syncSpeakersPlayheadForAutomation()
{
    if (!m_speakersTab) {
        return false;
    }
    m_speakersTab->syncCurrentSpeakerSentenceToPlayhead();
    return true;
}

void EditorWindow::startupProfileMark(const QString& phase, const QJsonObject& extra)
{
    if (m_startupProfileCompleted) {
        return;
    }
    if (!m_startupProfileTimer.isValid()) {
        m_startupProfileTimer.start();
        m_startupProfileLastMarkMs = 0;
    }
    const qint64 nowMs = m_startupProfileTimer.elapsed();
    const qint64 deltaMs = qMax<qint64>(0, nowMs - m_startupProfileLastMarkMs);
    m_startupProfileLastMarkMs = nowMs;

    QJsonObject mark{
        {QStringLiteral("phase"), phase},
        {QStringLiteral("t_ms"), nowMs},
        {QStringLiteral("delta_ms"), deltaMs}
    };
    if (!extra.isEmpty()) {
        mark[QStringLiteral("extra")] = extra;
    }
    m_startupProfileEvents.push_back(mark);
    startupReadinessMark(phase, extra);
}

void EditorWindow::startupReadinessMark(const QString& phase, const QJsonObject& extra)
{
    if (!m_startupProfileTimer.isValid()) {
        m_startupProfileTimer.start();
    }
    const qint64 nowMs = m_startupProfileTimer.elapsed();
    QJsonObject readinessUpdate{
        {QStringLiteral("phase"), phase},
        {QStringLiteral("t_ms"), nowMs},
        {QStringLiteral("delta_ms"), 0}
    };
    if (!extra.isEmpty()) {
        readinessUpdate[QStringLiteral("extra")] = extra;
    }
    {
        std::lock_guard<std::mutex> lock(m_startupReadinessMutex);
        QJsonArray milestones = m_startupReadinessSnapshot.value(QStringLiteral("milestones")).toArray();
        milestones.push_back(readinessUpdate);
        constexpr int kStartupReadinessMilestoneLimit = 96;
        while (milestones.size() > kStartupReadinessMilestoneLimit) {
            milestones.removeAt(0);
        }
        m_startupReadinessSnapshot[QStringLiteral("started")] = true;
        m_startupReadinessSnapshot[QStringLiteral("completed")] = m_startupProfileCompleted;
        m_startupReadinessSnapshot[QStringLiteral("ready_to_play")] = false;
        m_startupReadinessSnapshot[QStringLiteral("last_phase")] = phase;
        m_startupReadinessSnapshot[QStringLiteral("elapsed_ms")] = nowMs;
        m_startupReadinessSnapshot[QStringLiteral("milestones")] = milestones;

        const QJsonObject previousReadiness =
            m_startupReadinessSnapshot.value(QStringLiteral("readiness")).toObject();
        QJsonObject readiness;
        readiness[QStringLiteral("ui_constructed")] =
            phase == QStringLiteral("ctor.sync_complete") ||
            previousReadiness.value(QStringLiteral("ui_constructed")).toBool(false);
        readiness[QStringLiteral("project_state_loaded")] =
            phase == QStringLiteral("startup_load.state_loaded") ||
            phase == QStringLiteral("startup_load.autosave_ready") ||
            phase == QStringLiteral("startup_load.complete") ||
            previousReadiness.value(QStringLiteral("project_state_loaded")).toBool(false);
        readiness[QStringLiteral("timeline_bound")] =
            phase == QStringLiteral("apply_state.timeline_bind.end") ||
            previousReadiness.value(QStringLiteral("timeline_bound")).toBool(false);
        readiness[QStringLiteral("audio_bind_scheduled")] =
            phase == QStringLiteral("apply_state.audio_bind.deferred") ||
            phase == QStringLiteral("apply_state.audio_bind.end") ||
            previousReadiness.value(QStringLiteral("audio_bind_scheduled")).toBool(false);
        readiness[QStringLiteral("preview_bind_scheduled")] =
            phase == QStringLiteral("apply_state.preview_bind.deferred") ||
            phase == QStringLiteral("apply_state.preview_bind.end") ||
            previousReadiness.value(QStringLiteral("preview_bind_scheduled")).toBool(false);
        readiness[QStringLiteral("startup_load_complete")] =
            phase == QStringLiteral("startup_load.complete") ||
            previousReadiness.value(QStringLiteral("startup_load_complete")).toBool(false);
        readiness[QStringLiteral("first_playback_tick")] =
            phase == QStringLiteral("playback.first_tick") ||
            previousReadiness.value(QStringLiteral("first_playback_tick")).toBool(false);
        readiness[QStringLiteral("audio_started")] =
            phase == QStringLiteral("audio.start.invoked") ||
            previousReadiness.value(QStringLiteral("audio_started")).toBool(false);
        readiness[QStringLiteral("video_playback_sample_applied")] =
            phase == QStringLiteral("video.playback_sample_applied") ||
            previousReadiness.value(QStringLiteral("video_playback_sample_applied")).toBool(false);
        readiness[QStringLiteral("ready_to_play")] =
            readiness.value(QStringLiteral("startup_load_complete")).toBool(false) &&
            readiness.value(QStringLiteral("video_playback_sample_applied")).toBool(false);
        m_startupReadinessSnapshot[QStringLiteral("readiness")] = readiness;
        m_startupReadinessSnapshot[QStringLiteral("ready_to_play")] =
            readiness.value(QStringLiteral("ready_to_play")).toBool(false);
    }
}

// ============================================================================
// EditorWindow - Main application window
// ============================================================================
EditorWindow::EditorWindow(quint16 controlPort)
{
    m_projectManager = std::make_unique<ProjectManager>(this);
    QElapsedTimer ctorTimer;
    ctorTimer.start();
    m_startupProfileTimer.start();
    m_startupProfileLastMarkMs = 0;
    startupProfileMark(QStringLiteral("ctor.begin"),
                       QJsonObject{{QStringLiteral("control_port"), static_cast<int>(controlPort)}});
    if (restVulkanDiagnosticsModeEnabled()) {
        qApp->setQuitOnLastWindowClosed(false);
        setAttribute(Qt::WA_DeleteOnClose, false);
        qDebug() << "[STARTUP] REST Vulkan diagnostics mode: keepalive enabled";
    }

    setupWindowChrome();
    startupProfileMark(QStringLiteral("setup.window_chrome.done"));
    setupMainLayout(ctorTimer);
    startupProfileMark(QStringLiteral("setup.main_layout.done"));
    bindInspectorWidgets();
    startupProfileMark(QStringLiteral("setup.bind_inspector_widgets.done"));

    setupPlaybackTimers();
    startupProfileMark(QStringLiteral("setup.playback_timers.done"));
    m_deferredInspectorRefreshTimer.setSingleShot(true);
    connect(&m_deferredInspectorRefreshTimer, &QTimer::timeout, this, [this]() {
        refreshCurrentInspectorTab();
    });
    m_transcriptNormalizeRefreshTimer.setSingleShot(true);
    m_transcriptNormalizeRefreshTimer.setTimerType(Qt::CoarseTimer);
    connect(&m_transcriptNormalizeRefreshTimer, &QTimer::timeout,
            this, &EditorWindow::startTranscriptNormalizeRangeRefresh);
    connect(&m_transcriptNormalizeRefreshWatcher, &QFutureWatcher<QVector<ExportRangeSegment>>::finished,
            this, [this]() {
                const qint64 completedGeneration =
                    m_transcriptNormalizeRefreshWatcher.property("generation").toLongLong();
                if (completedGeneration < m_appliedTranscriptNormalizeRefreshGeneration) {
                    if (m_transcriptNormalizeRefreshGeneration > completedGeneration &&
                        !m_transcriptNormalizeRefreshTimer.isActive()) {
                        startTranscriptNormalizeRangeRefresh();
                    }
                    return;
                }
                m_appliedTranscriptNormalizeRefreshGeneration = completedGeneration;
                const QString completedSignature =
                    m_transcriptNormalizeRefreshWatcher.property("signature").toString();
                if (!completedSignature.isEmpty()) {
                    m_effectiveTranscriptNormalizeRangesCacheSignature = completedSignature;
                    m_effectiveTranscriptNormalizeRangesCache =
                        m_transcriptNormalizeRefreshWatcher.result();
                }
                if (m_audioEngine) {
                    m_audioEngine->setTranscriptNormalizeRanges(
                        m_transcriptNormalizeRefreshWatcher.result());
                }
                if (m_transcriptNormalizeRefreshGeneration > completedGeneration &&
                    !m_transcriptNormalizeRefreshTimer.isActive()) {
                    startTranscriptNormalizeRangeRefresh();
                }
            });
    connect(&m_transcriptTextCompanionBackfillWatcher, &QFutureWatcher<QJsonObject>::finished, this, [this]() {
        const QJsonObject result = m_transcriptTextCompanionBackfillWatcher.result();
        startupProfileMark(QStringLiteral("load_state.transcript_txt_backfill.complete"), result);
    });
    connect(&m_deferredHistoryLoadWatcher, &QFutureWatcher<QJsonObject>::finished, this, [this]() {
        const QJsonObject result = m_deferredHistoryLoadWatcher.result();
        const QString projectId = result.value(QStringLiteral("project_id")).toString();
        const int entryCount = result.value(QStringLiteral("entry_count")).toInt(0);
        const int index = result.value(QStringLiteral("history_index")).toInt(-1);
        const QJsonArray entries = result.value(QStringLiteral("entries")).toArray();
        if (projectId.isEmpty() ||
            projectId != (m_projectManager ? m_projectManager->currentProjectIdOrDefault()
                                           : QStringLiteral("default")) ||
            projectId != m_deferredHistoryLoadProjectId) {
            return;
        }
        if (!m_historyEntries.isEmpty()) {
            startupProfileMark(QStringLiteral("load_state.history_deferred.skipped"),
                               QJsonObject{{QStringLiteral("project_id"), projectId},
                                           {QStringLiteral("reason"), QStringLiteral("history_already_initialized")},
                                           {QStringLiteral("entry_count"), entryCount}});
            return;
        }
        m_historyEntries = entries;
        m_historyIndex = (entryCount > 0) ? qBound(0, index, entryCount - 1) : -1;
        if (result.value(QStringLiteral("history_sanitized")).toBool(false)) {
            saveHistoryNow();
        }
        if (m_historyEntries.isEmpty() && m_timeline) {
            pushHistorySnapshot();
        }
        startupProfileMark(QStringLiteral("load_state.history_deferred.complete"),
                           QJsonObject{{QStringLiteral("project_id"), projectId},
                                       {QStringLiteral("entry_count"), entryCount},
                                       {QStringLiteral("history_index"), m_historyIndex}});
    });
    connect(&m_startupStateLoadWatcher, &QFutureWatcher<QJsonObject>::finished, this, [this]() {
        const QJsonObject result = m_startupStateLoadWatcher.result();
        const QString projectId = result.value(QStringLiteral("project_id")).toString();
        const QString activeProjectId = m_projectManager
            ? m_projectManager->currentProjectIdOrDefault()
            : QStringLiteral("default");
        if (projectId.isEmpty() || projectId != activeProjectId) {
            return;
        }

        startupProfileMark(QStringLiteral("startup_load.state_parse.complete"),
                           QJsonObject{
                               {QStringLiteral("project_id"), projectId},
                               {QStringLiteral("defer_history"), result.value(QStringLiteral("defer_history")).toBool(false)},
                               {QStringLiteral("history_entry_count"), result.value(QStringLiteral("history_entries")).toArray().size()}
                           });

        m_historyEntries = result.value(QStringLiteral("history_entries")).toArray();
        m_historyIndex = result.value(QStringLiteral("history_index")).toInt(-1);
        if (result.value(QStringLiteral("history_sanitized")).toBool(false)) {
            saveHistoryNow();
        }

        startupProfileMark(QStringLiteral("load_state.apply_state.begin"));
        applyStateJson(result.value(QStringLiteral("root")).toObject());
        startupProfileMark(QStringLiteral("load_state.apply_state.end"));

        const bool deferHistoryLoad = result.value(QStringLiteral("defer_history")).toBool(false);
        if (m_historyEntries.isEmpty() && !deferHistoryLoad) {
            pushHistorySnapshot();
        }

        if (m_pendingSaveAfterLoad) {
            m_pendingSaveAfterLoad = false;
            scheduleSaveState();
        } else {
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

        startupProfileMark(QStringLiteral("startup_load.state_loaded"));
        setupAutosaveTimer();
        startupProfileMark(QStringLiteral("startup_load.autosave_ready"));
        m_startupProfileCompletedMs = m_startupProfileTimer.isValid()
            ? m_startupProfileTimer.elapsed()
            : 0;
        startupProfileMark(QStringLiteral("startup_load.complete"),
                           QJsonObject{{QStringLiteral("total_ms"), m_startupProfileCompletedMs}});
        m_startupProfileCompleted = true;
        scheduleDeferredStartupUiWarmup(true);
        scheduleOptimizedProfileEnsure();
    });
    setupShortcuts();
    startupProfileMark(QStringLiteral("setup.shortcuts.done"));
    setupHeartbeat();
    startupProfileMark(QStringLiteral("setup.heartbeat.done"));
    setupStateSaveTimer();
    startupProfileMark(QStringLiteral("setup.state_save_timer.done"));
    setupDeferredSeekTimers();
    startupProfileMark(QStringLiteral("setup.deferred_seek_timers.done"));
    setupControlServer(controlPort, ctorTimer);
    startupProfileMark(QStringLiteral("setup.control_server.done"));
    setupAudioEngine();
    startupProfileMark(QStringLiteral("setup.audio_engine.done"));
    setupSpeechFilterControls();
    startupProfileMark(QStringLiteral("setup.speech_filter_controls.done"));
    setupTrackInspectorControls();
    startupProfileMark(QStringLiteral("setup.track_inspector_controls.done"));
    setupPreviewControls();
    startupProfileMark(QStringLiteral("setup.preview_controls.done"));
    setupTabs();
    startupProfileMark(QStringLiteral("setup.tabs.done"));
    setupInspectorRefreshRouting();
    startupProfileMark(QStringLiteral("setup.inspector_refresh_routing.done"));
    setupStartupLoad();
    startupProfileMark(QStringLiteral("setup.startup_load.scheduled"));
    refreshAiIntegrationState();
    startupProfileMark(QStringLiteral("setup.ai_integration_state.done"));
    startupProfileMark(QStringLiteral("ctor.sync_complete"));
}

EditorWindow::~EditorWindow()
{
    flushHistorySaveNow();
    flushStateSaveNow();
}

void EditorWindow::closeEvent(QCloseEvent *event)
{
    if (restVulkanDiagnosticsModeEnabled() &&
        qEnvironmentVariable("JCUT_ALLOW_DIAG_CLOSE").trimmed() != QStringLiteral("1")) {
        qWarning() << "[STARTUP] REST Vulkan diagnostics mode: ignoring close event";
        event->ignore();
        return;
    }
    m_playbackTimer.stop();
    m_fastPlaybackActive.store(false);
    if (m_preview) {
        m_preview->setPlaybackState(false);
    }
    if (m_audioEngine) {
        m_audioEngine->stop();
    }
    flushHistorySaveNow();
    flushStateSaveNow();
    QMainWindow::closeEvent(event);
}

void EditorWindow::bindTimelineMediaState(const QString& selectedClipId,
                                          const QVector<ExportRangeSegment>& playbackRanges,
                                          int64_t currentFrame,
                                          bool seekPlayback)
{
    if (!m_timeline || !m_preview) {
        return;
    }

    m_preview->beginBulkUpdate();
    m_preview->setClipCount(m_timeline->clips().size());
    m_preview->setTimelineTracks(m_timeline->tracks());
    m_preview->setTimelineClips(m_timeline->clips());
    m_preview->setPlaybackTimingContext(speechFilterPlaybackTimingContext(playbackRanges));
    m_preview->setExportRanges(playbackRanges);
    m_preview->setRenderSyncMarkers(m_timeline->renderSyncMarkers());
    m_preview->setSelectedClipId(selectedClipId);
    m_preview->endBulkUpdate();

    if (m_audioEngine) {
        m_audioEngine->setTimelineTracks(m_timeline->tracks());
        m_audioEngine->setTimelineClips(m_timeline->clips());
        m_audioEngine->setExportRanges(playbackRanges);
        m_audioEngine->setTranscriptNormalizeRanges(
            m_previewAudioDynamics.transcriptNormalizeEnabled
                ? effectiveTranscriptNormalizeRanges()
                : QVector<ExportRangeSegment>{});
        m_audioEngine->setRenderSyncMarkers(m_timeline->renderSyncMarkers());
        m_audioEngine->setSpeechFilterFadeSamples(m_speechFilterFadeSamples);
        m_audioEngine->setSpeechFilterFadeMode(m_speechFilterFadeMode);
        m_audioEngine->setSpeechFilterCurveStrength(m_speechFilterCurveStrength);
        m_audioEngine->setSpeechFilterRangeCrossfadeEnabled(m_speechFilterRangeCrossfade);
        m_audioEngine->setPlaybackWarpMode(m_playbackAudioWarpMode);
        m_audioEngine->setPlaybackRate(effectiveAudioWarpRate());
        m_audioEngine->setTranscriptNormalizeEnabled(m_previewAudioDynamics.transcriptNormalizeEnabled);
        m_audioEngine->setAudioDynamicsSettings(m_previewAudioDynamics);
        if (seekPlayback) {
            m_audioEngine->seek(currentFrame);
        }
    }

    if (seekPlayback) {
        setCurrentFrame(currentFrame);
    }
}

void EditorWindow::scheduleTranscriptTextCompanionBackfill()
{
    if (m_transcriptTextCompanionBackfillWatcher.isRunning() || !m_timeline) {
        return;
    }

    QSet<QString> transcriptPaths;
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

    startupProfileMark(QStringLiteral("load_state.transcript_txt_backfill.deferred"),
                       QJsonObject{{QStringLiteral("transcript_path_count"), transcriptPaths.size()}});
    if (transcriptPaths.isEmpty()) {
        startupProfileMark(QStringLiteral("load_state.transcript_txt_backfill.complete"),
                           QJsonObject{{QStringLiteral("transcript_path_count"), 0},
                                       {QStringLiteral("txt_created_count"), 0}});
        return;
    }

    const QStringList transcriptPathList = transcriptPaths.values();
    m_transcriptTextCompanionBackfillWatcher.setFuture(QtConcurrent::run([transcriptPathList]() {
        TranscriptEngine engine;
        int createdTxtCount = 0;
        for (const QString& transcriptPath : transcriptPathList) {
            const QFileInfo txtInfo(QFileInfo(transcriptPath).dir().filePath(
                QFileInfo(transcriptPath).completeBaseName() + QStringLiteral(".txt")));
            if (txtInfo.exists()) {
                continue;
            }
            if (engine.ensureTranscriptTextCompanion(transcriptPath)) {
                ++createdTxtCount;
            }
        }
        return QJsonObject{
            {QStringLiteral("transcript_path_count"), transcriptPathList.size()},
            {QStringLiteral("txt_created_count"), createdTxtCount}
        };
    }));
}

void EditorWindow::applyDeferredStartupPanelState(const QJsonObject& root,
                                                  const QStringList& expandedExplorerPaths)
{
    if (m_explorerPane) {
        m_explorerPane->restoreExpandedExplorerPaths(expandedExplorerPaths);
    }

    const QJsonArray transcriptColumnHidden =
        root.value(QStringLiteral("transcriptColumnHidden")).toArray();
    const QJsonArray speakersColumnHidden =
        root.value(QStringLiteral("speakersColumnHidden")).toArray();
    const QString transcriptSpeakerFilterValue =
        root.value(QStringLiteral("transcriptSpeakerFilterValue")).toString();
    const QString transcriptActiveCutPath =
        root.value(QStringLiteral("transcriptActiveCutPath")).toString();
    const int selectedInspectorTab = root.value(QStringLiteral("selectedInspectorTab")).toInt(0);
    const QString selectedInspectorTabLabel =
        root.value(QStringLiteral("selectedInspectorTabLabel")).toString().trimmed();

    if (m_inspectorPane && m_inspectorPane->transcriptSpeakerFilterCombo()) {
        m_inspectorPane->transcriptSpeakerFilterCombo()->setProperty(
            "pendingSpeakerFilterValue", transcriptSpeakerFilterValue);
    }
    if (m_inspectorPane && m_inspectorPane->transcriptScriptVersionCombo()) {
        m_inspectorPane->transcriptScriptVersionCombo()->setProperty(
            "pendingCutPath", transcriptActiveCutPath);
    }
    if (m_transcriptTable && !transcriptColumnHidden.isEmpty()) {
        const int limit = qMin(m_transcriptTable->columnCount(), transcriptColumnHidden.size());
        for (int i = 0; i < limit; ++i) {
            if (i == 5) {
                m_transcriptTable->setColumnHidden(i, false);
                continue;
            }
            m_transcriptTable->setColumnHidden(i, transcriptColumnHidden.at(i).toBool(false));
        }
    }
    if (m_inspectorPane) {
        if (SpeakersTable* speakersTable =
                qobject_cast<SpeakersTable*>(m_inspectorPane->speakersTable());
            speakersTable && !speakersColumnHidden.isEmpty()) {
            speakersTable->applyHiddenColumns(speakersColumnHidden);
        }
    }

    if (m_inspectorTabs && m_inspectorTabs->count() > 0) {
        const auto isTabNamed = [this](const QString& name) -> bool {
            if (!m_inspectorTabs) {
                return false;
            }
            const int index = m_inspectorTabs->currentIndex();
            return index >= 0 && m_inspectorTabs->tabText(index).compare(name, Qt::CaseInsensitive) == 0;
        };
        int targetInspectorTab = qBound(0, selectedInspectorTab, m_inspectorTabs->count() - 1);
        if (!selectedInspectorTabLabel.isEmpty()) {
            for (int i = 0; i < m_inspectorTabs->count(); ++i) {
                if (m_inspectorTabs->tabText(i).compare(selectedInspectorTabLabel, Qt::CaseInsensitive) == 0) {
                    targetInspectorTab = i;
                    break;
                }
            }
        }
        QSignalBlocker block(m_inspectorTabs);
        m_inspectorTabs->setCurrentIndex(targetInspectorTab);
        if (m_preview) {
            const bool showCorrectionOverlays = isTabNamed(QStringLiteral("Corrections"));
            const bool transcriptOverlayInteractive = isTabNamed(QStringLiteral("Transcript"));
            const bool titleOverlayOnly = isTabNamed(QStringLiteral("Titles"));
            const bool faceDetectionsAssignmentInteractive = isTabNamed(QStringLiteral("Speakers"));
            m_preview->setShowCorrectionOverlays(showCorrectionOverlays);
            m_preview->setTranscriptOverlayInteractionEnabled(transcriptOverlayInteractive);
            m_preview->setTitleOverlayInteractionOnly(titleOverlayOnly);
            m_preview->setFaceDetectionsAssignmentInteractionEnabled(faceDetectionsAssignmentInteractive);
            if (!showCorrectionOverlays && m_correctionsTab) {
                m_correctionsTab->stopDrawing();
            }
        }
    }
}

bool EditorWindow::reconcileMissingMediaForClips(QVector<TimelineClip>* clips,
                                                 const QString& rootPath)
{
    if (!clips || clips->isEmpty() || m_restoringHistory) {
        return false;
    }

    const auto clipSourceExists = [](const TimelineClip& clip) {
        if (clip.filePath.isEmpty() || clip.mediaType == ClipMediaType::Title) {
            return true;
        }
        const QFileInfo info(clip.filePath);
        return info.exists() &&
               (info.isFile() || (info.isDir() && isImageSequencePath(info.absoluteFilePath())));
    };

    QHash<QString, QString> relocatedByOriginalPath;
    QHash<QString, QString> relocatedDirByOriginalDir;
    bool skipRelocatePrompts = false;
    int relocatedCount = 0;
    int unresolvedCount = 0;

    for (TimelineClip& clip : *clips) {
        if (clipSourceExists(clip)) {
            continue;
        }

        const QString originalPath = QFileInfo(clip.filePath).absoluteFilePath();
        const QFileInfo originalInfo(originalPath);
        QString relocatedPath = relocatedByOriginalPath.value(originalPath);

        if (relocatedPath.isEmpty()) {
            const QString mappedDir = relocatedDirByOriginalDir.value(originalInfo.absolutePath());
            if (!mappedDir.isEmpty()) {
                const QString candidatePath = QDir(mappedDir).filePath(originalInfo.fileName());
                const QFileInfo candidateInfo(candidatePath);
                if (candidateInfo.exists() &&
                    (candidateInfo.isFile() ||
                     (candidateInfo.isDir() && isImageSequencePath(candidateInfo.absoluteFilePath())))) {
                    relocatedPath = candidateInfo.absoluteFilePath();
                }
            }
        }

        if (relocatedPath.isEmpty() && !skipRelocatePrompts) {
            QMessageBox prompt(this);
            prompt.setIcon(QMessageBox::Warning);
            prompt.setWindowTitle(QStringLiteral("Missing Media"));
            prompt.setText(QStringLiteral("Could not find media file:\n%1")
                               .arg(QDir::toNativeSeparators(originalPath)));
            prompt.setInformativeText(QStringLiteral("Relocate this clip to a new file or folder?"));
            QPushButton* relocateButton =
                prompt.addButton(QStringLiteral("Relocate"), QMessageBox::AcceptRole);
            QPushButton* skipButton =
                prompt.addButton(QStringLiteral("Skip"), QMessageBox::RejectRole);
            QPushButton* skipAllButton =
                prompt.addButton(QStringLiteral("Skip All"), QMessageBox::DestructiveRole);
            Q_UNUSED(skipButton);
            prompt.setDefaultButton(relocateButton);
            prompt.exec();

            if (prompt.clickedButton() == skipAllButton) {
                skipRelocatePrompts = true;
                ++unresolvedCount;
                continue;
            }
            if (prompt.clickedButton() != relocateButton) {
                ++unresolvedCount;
                continue;
            }

            const QString startDir = originalInfo.dir().exists()
                ? originalInfo.absolutePath()
                : rootPath;
            QString selectedPath;
            if (clip.sourceKind == MediaSourceKind::ImageSequence) {
                selectedPath = QFileDialog::getExistingDirectory(
                    this,
                    QStringLiteral("Locate Image Sequence Folder"),
                    startDir,
                    QFileDialog::ShowDirsOnly | QFileDialog::DontResolveSymlinks);
            } else {
                selectedPath = QFileDialog::getOpenFileName(
                    this,
                    QStringLiteral("Locate Media File"),
                    startDir);
            }

            if (selectedPath.isEmpty()) {
                ++unresolvedCount;
                continue;
            }
            relocatedPath = QFileInfo(selectedPath).absoluteFilePath();
        }

        if (relocatedPath.isEmpty()) {
            ++unresolvedCount;
            continue;
        }

        const QFileInfo relocatedInfo(relocatedPath);
        const bool relocatedValid =
            relocatedInfo.exists() &&
            (relocatedInfo.isFile() ||
             (relocatedInfo.isDir() && isImageSequencePath(relocatedInfo.absoluteFilePath())));
        if (!relocatedValid) {
            ++unresolvedCount;
            continue;
        }

        const QString oldFileName = originalInfo.fileName();
        if (clip.label.trimmed().isEmpty() || clip.label == oldFileName) {
            clip.label = relocatedInfo.fileName();
        }
        clip.filePath = relocatedInfo.absoluteFilePath();
        clip.proxyPath.clear();

        relocatedByOriginalPath.insert(originalPath, clip.filePath);
        relocatedDirByOriginalDir.insert(originalInfo.absolutePath(), relocatedInfo.absolutePath());
        ++relocatedCount;
    }

    if (relocatedCount > 0) {
        m_pendingSaveAfterLoad = true;
    }
    if (relocatedCount > 0 || unresolvedCount > 0) {
        QString summary = QStringLiteral("Relocated %1 clip source(s).").arg(relocatedCount);
        if (unresolvedCount > 0) {
            summary += QStringLiteral("\n%1 clip source(s) are still missing.")
                           .arg(unresolvedCount);
        }
        QMessageBox::information(this, QStringLiteral("Media Relocation"), summary);
    }

    return relocatedCount > 0;
}


void EditorWindow::syncTranscriptTableToPlayhead()
{
    if (!m_timeline || !m_transcriptTable || m_updatingTranscriptInspector) return;
    if (!m_transcriptFollowCurrentWordCheckBox || !m_transcriptFollowCurrentWordCheckBox->isChecked()) return;

    const TimelineClip *clip = m_timeline->selectedClip();
    if (!clip || !(clip->mediaType == ClipMediaType::Audio || clip->hasAudio)) {
        m_transcriptTable->clearSelection();
        return;
    }
    int64_t timelineSample = m_transportTimelineSample;
    qreal timelineFramePosition = samplesToFramePosition(timelineSample);
    const qreal clipStart = static_cast<qreal>(clip->startFrame);
    const qreal clipEnd = static_cast<qreal>(clip->startFrame + qMax<int64_t>(0, clip->durationFrames - 1));
    if (!playbackActive() && (timelineFramePosition < clipStart || timelineFramePosition > clipEnd)) {
        const int64_t currentFrameSample = frameToSamples(m_timeline->currentFrame());
        const qreal currentFramePosition = samplesToFramePosition(currentFrameSample);
        if (currentFramePosition >= clipStart && currentFramePosition <= clipEnd) {
            timelineSample = currentFrameSample;
            timelineFramePosition = currentFramePosition;
        }
    }
    if (timelineFramePosition < clipStart || timelineFramePosition > clipEnd) {
        m_transcriptTable->clearSelection();
        return;
    }

    const int64_t sourceSample = sourceSampleForClipAtTimelineSample(
        *clip,
        timelineSample,
        m_timeline->renderSyncMarkers());
    const double sourceSeconds = static_cast<double>(sourceSample) / static_cast<double>(kAudioSampleRate);
    const int64_t sourceFrame = transcriptFrameForClipAtTimelineSample(
        *clip,
        timelineSample,
        m_timeline->renderSyncMarkers());
    if (m_transcriptTab) {
        m_transcriptTab->syncTableToPlayhead(timelineSample, sourceSeconds, sourceFrame);
    }
    if (m_speakerTranscriptTab) {
        m_speakerTranscriptTab->syncTableToPlayhead(timelineSample, sourceSeconds, sourceFrame);
    }
    if (m_speakersTab) {
        m_speakersTab->syncIdentityToPlayhead(timelineSample, sourceSeconds, sourceFrame);
    }
}

void EditorWindow::syncKeyframeTableToPlayhead()
{
    if (m_videoKeyframeTab) {
        m_videoKeyframeTab->syncTableToPlayhead();
    }
}

void EditorWindow::syncGradingTableToPlayhead()
{
    if (m_gradingTab) {
        m_gradingTab->syncTableToPlayhead();
    }
    if (m_opacityTab) {
        m_opacityTab->syncTableToPlayhead();
    }
}

void EditorWindow::syncOpacityTableToPlayhead()
{
    if (m_opacityTab) {
        m_opacityTab->syncTableToPlayhead();
    }
}

bool EditorWindow::focusInTranscriptTable() const
{
    QWidget *focus = QApplication::focusWidget();
    return m_transcriptTable && focus && (focus == m_transcriptTable || m_transcriptTable->isAncestorOf(focus));
}

bool EditorWindow::focusInKeyframeTable() const
{
    QWidget *focus = QApplication::focusWidget();
    return m_videoKeyframeTable && focus && (focus == m_videoKeyframeTable || m_videoKeyframeTable->isAncestorOf(focus));
}

bool EditorWindow::focusInGradingTable() const
{
    QWidget *focus = QApplication::focusWidget();
    return m_gradingKeyframeTable && focus && (focus == m_gradingKeyframeTable || m_gradingKeyframeTable->isAncestorOf(focus));
}

bool EditorWindow::focusInOpacityTable() const
{
    QWidget *focus = QApplication::focusWidget();
    return m_opacityKeyframeTable && focus && (focus == m_opacityKeyframeTable || m_opacityKeyframeTable->isAncestorOf(focus));
}

bool EditorWindow::focusInEditableInput() const
{
    QWidget *focus = QApplication::focusWidget();
    if (!focus) return false;
    
    if (qobject_cast<QLineEdit *>(focus) ||
        qobject_cast<QTextEdit *>(focus) ||
        qobject_cast<QPlainTextEdit *>(focus) ||
        qobject_cast<QAbstractSpinBox *>(focus))
    {
        return true;
    }
    if (auto *combo = qobject_cast<QComboBox *>(focus))
    {
        if (combo->isEditable()) return true;
    }
    for (QWidget *parent = focus->parentWidget(); parent; parent = parent->parentWidget())
    {
        if (qobject_cast<QLineEdit *>(parent) ||
            qobject_cast<QTextEdit *>(parent) ||
            qobject_cast<QPlainTextEdit *>(parent) ||
            qobject_cast<QAbstractSpinBox *>(parent) ||
            qobject_cast<QAbstractItemView *>(parent))
        {
            return true;
        }
    }
    return false;
}

bool EditorWindow::shouldBlockGlobalEditorShortcuts() const
{
    return focusInEditableInput();
}

void EditorWindow::initializeDeferredTimelineSeek(QTimer *timer, int64_t *pendingFrame)
{
    if (!timer || !pendingFrame) return;
    timer->setSingleShot(true);
    connect(timer, &QTimer::timeout, this, [this, pendingFrame]()
    {
        if (!m_timeline || !pendingFrame || *pendingFrame < 0) return;
        setCurrentPlaybackSample(frameToSamples(*pendingFrame), false, true);
        *pendingFrame = -1;
    });
}

void EditorWindow::scheduleDeferredTimelineSeek(QTimer *timer, int64_t *pendingFrame, int64_t timelineFrame)
{
    if (!timer || !pendingFrame) return;
    *pendingFrame = timelineFrame;
    timer->start(QApplication::doubleClickInterval());
}

void EditorWindow::cancelDeferredTimelineSeek(QTimer *timer, int64_t *pendingFrame)
{
    if (timer) timer->stop();
    if (pendingFrame) *pendingFrame = -1;
}

void EditorWindow::undoHistory()
{
    if (m_historyIndex <= 0 || m_historyEntries.isEmpty())
    {
        return;
    }

    m_restoringHistory = true;
    m_historyIndex -= 1;
    const QJsonObject snapshot = m_historyEntries.at(m_historyIndex).toObject();
    restoreTranscriptDocumentsFromHistorySnapshot(snapshot);
    applyStateJson(snapshot);
    m_restoringHistory = false;
    saveHistoryNow();
    scheduleSaveState();
}

void EditorWindow::redoHistory()
{
    if (m_historyIndex >= m_historyEntries.size() - 1 || m_historyEntries.isEmpty())
    {
        return;
    }

    m_restoringHistory = true;
    m_historyIndex += 1;
    const QJsonObject snapshot = m_historyEntries.at(m_historyIndex).toObject();
    restoreTranscriptDocumentsFromHistorySnapshot(snapshot);
    applyStateJson(snapshot);
    m_restoringHistory = false;
    saveHistoryNow();
    scheduleSaveState();
}

void EditorWindow::restoreToHistoryIndex(int index)
{
    if (index < 0 || index >= m_historyEntries.size())
    {
        return;
    }

    m_restoringHistory = true;
    m_historyIndex = index;
    const QJsonObject snapshot = m_historyEntries.at(m_historyIndex).toObject();
    restoreTranscriptDocumentsFromHistorySnapshot(snapshot);
    applyStateJson(snapshot);
    m_restoringHistory = false;
    saveHistoryNow();
    scheduleSaveState();
}

void EditorWindow::applyStateJson(const QJsonObject &root)
{
    m_loadingState = true;
    const bool startupMarking = !m_startupProfileCompleted;
    auto markStartup = [this, startupMarking](const QString& phase, const QJsonObject& extra = QJsonObject()) {
        if (!startupMarking) {
            return;
        }
        startupProfileMark(phase, extra);
    };
    markStartup(QStringLiteral("apply_state.begin"));

    // Default to the projects root from editor.config, then fall back to saved state, then current dir
    const QString defaultRootPath =
        m_projectManager ? m_projectManager->rootDirPath() : QString();
    QString rootPath = root.value(QStringLiteral("mediaRoot")).toString(defaultRootPath);
    if (rootPath.isEmpty()) {
        rootPath = root.value(QStringLiteral("explorerRoot")).toString(defaultRootPath);
    }
    if (!rootPath.isEmpty() && QFileInfo(rootPath).isRelative() && !defaultRootPath.isEmpty()) {
        rootPath = QDir(defaultRootPath).filePath(rootPath);
    }
    if (rootPath.isEmpty() || !QDir(rootPath).exists()) {
        rootPath = QDir::currentPath();
    }
    setTranscriptSourceRootPath(rootPath);
    QString galleryFolderPath = root.value(QStringLiteral("mediaGalleryPath")).toString();
    if (galleryFolderPath.isEmpty()) {
        galleryFolderPath = root.value(QStringLiteral("explorerGalleryPath")).toString();
    }
    const int outputWidth = qMax(16, root.value(QStringLiteral("outputWidth")).toInt(1080));
    const int outputHeight = qMax(16, root.value(QStringLiteral("outputHeight")).toInt(1920));
    const double storedOutputFps = root.value(QStringLiteral("outputFps"))
                                       .toDouble(static_cast<double>(kTimelineFps));
    const double outputFps = qBound(1.0,
                                    root.value(QStringLiteral("timelineFps"))
                                        .toDouble(storedOutputFps),
                                    240.0);
    const QString outputFormat = root.value(QStringLiteral("outputFormat")).toString(QStringLiteral("mp4"));
    const QString renderBackendPreference = root.value(QStringLiteral("render_backend"))
                                                .toString(QStringLiteral("vulkan"))
                                                .trimmed()
                                                .toLower();
    QString previewVulkanPresenterPreference =
        root.value(QStringLiteral("preview_vulkan_presenter"))
            .toString(qEnvironmentVariable("JCUT_VULKAN_PREVIEW_PRESENTER", QStringLiteral("direct")))
            .trimmed()
            .toLower();
    if (renderBackendPreference == QStringLiteral("vulkan") && previewVulkanPresenterPreference.isEmpty()) {
        previewVulkanPresenterPreference = QStringLiteral("direct");
    }
    if (renderBackendPreference == QStringLiteral("vulkan") &&
        previewVulkanPresenterPreference == QStringLiteral("embedded") &&
        qEnvironmentVariableIsEmpty("JCUT_VULKAN_PREVIEW_PRESENTER")) {
        previewVulkanPresenterPreference = QStringLiteral("direct");
    }
    if (previewVulkanPresenterPreference != QStringLiteral("direct")) {
        previewVulkanPresenterPreference = QStringLiteral("embedded");
    }
    const QString lastRenderOutputPath = root.value(QStringLiteral("lastRenderOutputPath")).toString();
    bool renderUseProxies = root.value(QStringLiteral("renderUseProxies")).toBool(false);
    const bool createImageSequence = root.value(QStringLiteral("createImageSequence")).toBool(false);
    const QString imageSequenceFormat =
        root.value(QStringLiteral("imageSequenceFormat")).toString(QStringLiteral("jpeg"));
    const QString savedBackgroundFillEffect =
        root.value(QStringLiteral("backgroundFillEffect"))
            .toString(backgroundFillEffectToString(kDefaultBackgroundFillEffect));
    BackgroundFillEffect backgroundFillEffect =
        backgroundFillEffectFromString(savedBackgroundFillEffect);
    const QString backgroundFillStretchSourceClipId =
        root.value(QStringLiteral("backgroundFillStretchSourceClipId")).toString().trimmed();
    const qreal backgroundFillOpacity =
        qBound<qreal>(0.0, root.value(QStringLiteral("backgroundFillOpacity")).toDouble(1.0), 1.0);
    const qreal backgroundFillBrightness =
        qBound<qreal>(-1.0, root.value(QStringLiteral("backgroundFillBrightness")).toDouble(0.0), 1.0);
    const qreal backgroundFillSaturation =
        qBound<qreal>(0.0, root.value(QStringLiteral("backgroundFillSaturation")).toDouble(1.0), 3.0);
    bool backgroundFillEdgeProgressive =
        root.value(QStringLiteral("backgroundFillEdgeProgressive")).toBool(false);
    const QString normalizedSavedBackgroundFillEffect =
        savedBackgroundFillEffect.trimmed().toLower().replace(QLatin1Char('-'), QLatin1Char('_'));
    const bool legacyBackgroundProgressiveStretch =
        backgroundFillEdgeProgressive ||
        normalizedSavedBackgroundFillEffect == QStringLiteral("progressive_edge_stretch") ||
        normalizedSavedBackgroundFillEffect == QStringLiteral("progressive_stretch") ||
        normalizedSavedBackgroundFillEffect == QStringLiteral("edge_stretch_progressive");
    const bool previewHideOutsideOutput = root.value(QStringLiteral("previewHideOutsideOutput")).toBool(false);
    const bool previewShowSpeakerTrackPoints =
        root.value(QStringLiteral("previewShowSpeakerTrackPoints")).toBool(false);
    const bool previewShowSpeakerTrackBoxes =
        root.value(QStringLiteral("previewShowSpeakerTrackBoxes")).toBool(false);
    const bool speakerShowContiguousTranscriptSections =
        root.value(QStringLiteral("speakerShowContiguousTranscriptSections")).toBool(false);
    const bool speakerApplyTrackToAllMatchingSections =
        root.value(QStringLiteral("speakerApplyTrackToAllMatchingSections")).toBool(false);
    const bool previewShowRawDetections =
        root.value(QStringLiteral("previewShowRawDetections")).toBool(false);
    QJsonObject speakerTitleSettings =
        root.value(QStringLiteral("speakerTitleSettings")).toObject();
    if (!speakerTitleSettings.contains(QStringLiteral("showName")))
        speakerTitleSettings[QStringLiteral("showName")] = root.value(QStringLiteral("previewShowCurrentSpeakerName")).toBool(true);
    if (!speakerTitleSettings.contains(QStringLiteral("showOrganization")))
        speakerTitleSettings[QStringLiteral("showOrganization")] = root.value(QStringLiteral("previewShowCurrentSpeakerOrganization")).toBool(false);
    if (!speakerTitleSettings.contains(QStringLiteral("backgroundEnabled")))
        speakerTitleSettings[QStringLiteral("backgroundEnabled")] = root.value(QStringLiteral("previewCurrentSpeakerBackgroundVisible")).toBool(true);
    const bool previewShowCurrentSpeakerName = speakerTitleSettings.value(QStringLiteral("showName")).toBool(true);
    const bool previewShowCurrentSpeakerOrganization = speakerTitleSettings.value(QStringLiteral("showOrganization")).toBool(false);
    const int previewCurrentSpeakerNameTextScalePercent = qBound(
        25,
        root.value(QStringLiteral("previewCurrentSpeakerNameTextScalePercent")).toInt(100),
        300);
    const int previewCurrentSpeakerOrganizationTextScalePercent = qBound(
        25,
        root.value(QStringLiteral("previewCurrentSpeakerOrganizationTextScalePercent")).toInt(100),
        300);
    const int previewCurrentSpeakerNameYPositionPercent = qBound(
        0,
        root.value(QStringLiteral("previewCurrentSpeakerNameYPositionPercent")).toInt(86),
        100);
    const int previewCurrentSpeakerOrganizationYPositionPercent = qBound(
        0,
        root.value(QStringLiteral("previewCurrentSpeakerOrganizationYPositionPercent")).toInt(93),
        100);
    QColor previewCurrentSpeakerNameColor = loadedColorValue(
        root,
        QStringLiteral("previewCurrentSpeakerNameColor"),
        QColor(QStringLiteral("#f4f8fc")));
    QColor previewCurrentSpeakerOrganizationColor = loadedColorValue(
        root,
        QStringLiteral("previewCurrentSpeakerOrganizationColor"),
        QColor(QStringLiteral("#b9d0e5")));
    QColor previewCurrentSpeakerBackgroundColor = loadedColorValue(
        root,
        QStringLiteral("previewCurrentSpeakerBackgroundColor"),
        QColor(8, 13, 20, 190));
    const bool previewCurrentSpeakerBackgroundVisible =
        root.value(QStringLiteral("previewCurrentSpeakerBackgroundVisible")).toBool(true);
    const int previewCurrentSpeakerBackgroundOpacityPercent = qBound(
        0,
        root.value(QStringLiteral("previewCurrentSpeakerBackgroundOpacityPercent")).toInt(75),
        100);
    previewCurrentSpeakerBackgroundColor.setAlphaF(previewCurrentSpeakerBackgroundOpacityPercent / 100.0);
    QColor previewCurrentSpeakerBorderColor = loadedColorValue(
        root,
        QStringLiteral("previewCurrentSpeakerBorderColor"),
        QColor(225, 236, 247, 120));
    const int previewCurrentSpeakerBorderOpacityPercent = qBound(
        0,
        root.value(QStringLiteral("previewCurrentSpeakerBorderOpacityPercent")).toInt(47),
        100);
    previewCurrentSpeakerBorderColor.setAlphaF(previewCurrentSpeakerBorderOpacityPercent / 100.0);
    const int previewCurrentSpeakerBackgroundRadiusPx = qBound(
        0,
        root.value(QStringLiteral("previewCurrentSpeakerBackgroundRadiusPx")).toInt(14),
        128);
    const int previewCurrentSpeakerBorderWidthPx = qBound(
        0,
        root.value(QStringLiteral("previewCurrentSpeakerBorderWidthPx")).toInt(1),
        16);
    const bool previewCurrentSpeakerShadowEnabled =
        root.value(QStringLiteral("previewCurrentSpeakerShadowEnabled")).toBool(true);
    QColor previewCurrentSpeakerShadowColor = loadedColorValue(
        root,
        QStringLiteral("previewCurrentSpeakerShadowColor"),
        QColor(0, 0, 0, 190));
    const int previewCurrentSpeakerShadowOpacityPercent = qBound(
        0,
        root.value(QStringLiteral("previewCurrentSpeakerShadowOpacityPercent")).toInt(75),
        100);
    previewCurrentSpeakerShadowColor.setAlphaF(previewCurrentSpeakerShadowOpacityPercent / 100.0);
    const QString previewFacestreamOverlaySource = QStringLiteral("all");
    const int autosaveIntervalMinutes = qBound(
        1,
        root.value(QStringLiteral("autosaveIntervalMinutes"))
            .toInt(kDefaultAutosaveIntervalMinutes),
        120);
    const int autosaveMaxBackups = qBound(
        1,
        root.value(QStringLiteral("autosaveMaxBackups"))
            .toInt(kDefaultAutosaveMaxBackups),
        200);
    const int historyMaxEntries = qBound(
        10,
        root.value(QStringLiteral("historyMaxEntries"))
            .toInt(kDefaultHistoryMaxEntries),
        500);
    const int historyMaxMegabytes = qBound(
        1,
        root.value(QStringLiteral("historyMaxMegabytes"))
            .toInt(kDefaultHistoryMaxMegabytes),
        256);
    const bool previewPlaybackCacheFallback =
        root.value(QStringLiteral("previewPlaybackCacheFallback")).toBool(editor::debugPlaybackCacheFallbackEnabled());
    const bool previewLeadPrefetchEnabled =
        root.value(QStringLiteral("previewLeadPrefetchEnabled")).toBool(editor::debugLeadPrefetchEnabled());
    const int previewLeadPrefetchCount =
        qBound(0, root.value(QStringLiteral("previewLeadPrefetchCount")).toInt(editor::debugLeadPrefetchCount()), 8);
    const int previewPlaybackWindowAhead =
        qBound(1, root.value(QStringLiteral("previewPlaybackWindowAhead")).toInt(editor::debugPlaybackWindowAhead()), 24);
    const int previewVisibleQueueReserve =
        qBound(0, root.value(QStringLiteral("previewVisibleQueueReserve")).toInt(editor::debugVisibleQueueReserve()), 64);
    const int debugPrefetchMaxQueueDepth =
        qBound(1, root.value(QStringLiteral("debugPrefetchMaxQueueDepth")).toInt(editor::debugPrefetchMaxQueueDepth()), 32);
    const int debugPrefetchMaxInflight =
        qBound(1, root.value(QStringLiteral("debugPrefetchMaxInflight")).toInt(editor::debugPrefetchMaxInflight()), 16);
    const int debugPrefetchMaxPerTick =
        qBound(1, root.value(QStringLiteral("debugPrefetchMaxPerTick")).toInt(editor::debugPrefetchMaxPerTick()), 16);
    const int debugPrefetchSkipVisiblePendingThreshold =
        qBound(0, root.value(QStringLiteral("debugPrefetchSkipVisiblePendingThreshold"))
                     .toInt(editor::debugPrefetchSkipVisiblePendingThreshold()),
               16);
    const int debugDecoderLaneCount =
        qBound(0, root.value(QStringLiteral("debugDecoderLaneCount")).toInt(editor::debugDecoderLaneCount()), 16);
    const int timelineAudioEnvelopeGranularity =
        qBound(64,
               root.value(QStringLiteral("timelineAudioEnvelopeGranularity"))
                   .toInt(editor::debugTimelineAudioEnvelopeGranularity()),
               8192);
    editor::DecodePreference debugDecodePreference = editor::DecodePreference::Hardware;
    const QString debugDecodeModeText =
        root.value(QStringLiteral("debugDecodeMode"))
            .toString(editor::decodePreferenceToString(debugDecodePreference));
    editor::parseDecodePreference(debugDecodeModeText, &debugDecodePreference);
    if (debugDecodePreference == editor::DecodePreference::Software) {
        debugDecodePreference = editor::DecodePreference::Hardware;
    }
    if (renderBackendPreference == QStringLiteral("vulkan")) {
        debugDecodePreference = editor::DecodePreference::HardwareZeroCopy;
    }
    editor::setDebugDecodePreference(debugDecodePreference);
    editor::H26xSoftwareThreadingMode debugH26xSoftwareThreadingMode =
        editor::debugH26xSoftwareThreadingMode();
    const QString debugH26xSoftwareThreadingModeText =
        root.value(QStringLiteral("debugH26xSoftwareThreadingMode"))
            .toString(editor::h26xSoftwareThreadingModeToString(editor::debugH26xSoftwareThreadingMode()));
    editor::parseH26xSoftwareThreadingMode(debugH26xSoftwareThreadingModeText,
                                           &debugH26xSoftwareThreadingMode);
    editor::RubberBandEnginePreference rubberBandEnginePreference =
        editor::rubberBandEnginePreference();
    editor::parseRubberBandEnginePreference(
        root.value(QStringLiteral("rubberBandEngine"))
            .toString(editor::rubberBandEnginePreferenceToString(
                editor::rubberBandEnginePreference())),
        &rubberBandEnginePreference);
    editor::RubberBandThreadingPreference rubberBandThreadingPreference =
        editor::rubberBandThreadingPreference();
    editor::parseRubberBandThreadingPreference(
        root.value(QStringLiteral("rubberBandThreading"))
            .toString(editor::rubberBandThreadingPreferenceToString(
                editor::rubberBandThreadingPreference())),
        &rubberBandThreadingPreference);
    editor::RubberBandWindowPreference rubberBandWindowPreference =
        editor::rubberBandWindowPreference();
    editor::parseRubberBandWindowPreference(
        root.value(QStringLiteral("rubberBandWindow"))
            .toString(editor::rubberBandWindowPreferenceToString(
                editor::rubberBandWindowPreference())),
        &rubberBandWindowPreference);
    editor::RubberBandPitchPreference rubberBandPitchPreference =
        editor::rubberBandPitchPreference();
    editor::parseRubberBandPitchPreference(
        root.value(QStringLiteral("rubberBandPitch"))
            .toString(editor::rubberBandPitchPreferenceToString(
                editor::rubberBandPitchPreference())),
        &rubberBandPitchPreference);
    const bool rubberBandChannelsTogether =
        root.value(QStringLiteral("rubberBandChannelsTogether"))
            .toBool(editor::rubberBandChannelsTogether());
    const bool debugDeterministicPipelineExplicit =
        root.value(QStringLiteral("debugDeterministicPipelineExplicit")).toBool(false);
    const bool debugDeterministicPipeline =
        debugDeterministicPipelineExplicit
            ? root.value(QStringLiteral("debugDeterministicPipeline"))
                  .toBool(editor::debugDeterministicPipelineEnabled())
            : false;
    const bool legacySpeechFilterEnabled =
        root.value(QStringLiteral("speechFilterEnabled")).toBool(false);
    const int transcriptPrependMs = root.value(QStringLiteral("transcriptPrependMs")).toInt(150);
    const int transcriptPostpendMs = root.value(QStringLiteral("transcriptPostpendMs")).toInt(70);
    const int transcriptOffsetMs = root.value(QStringLiteral("transcriptOffsetMs")).toInt(0);
    const int speechFilterFadeSamples = root.value(QStringLiteral("speechFilterFadeSamples")).toInt(300);
    const qreal speechFilterCurveStrength =
        qBound<qreal>(0.25,
                      root.value(QStringLiteral("speechFilterCurveStrength")).toDouble(1.0),
                      4.0);
    const bool hasSpeechFilterFadeMode =
        root.contains(QStringLiteral("speechFilterFadeMode"));
    const QString speechFilterFadeModeValue =
        root.value(QStringLiteral("speechFilterFadeMode")).toString();
    const bool speechFilterRangeCrossfade =
        root.value(QStringLiteral("speechFilterRangeCrossfade")).toBool(false);
    const bool speechFilterFrameCrossfadeEnabled =
        root.value(QStringLiteral("speechFilterFrameCrossfadeEnabled")).toBool(false);
    const QString speechFilterFrameTransitionModeValue =
        root.value(QStringLiteral("speechFilterFrameTransitionMode")).toString();
    const int speechFilterFrameCrossfadeFrames =
        root.value(QStringLiteral("speechFilterFrameCrossfadeFrames")).toInt(6);
    const bool transcriptUnifiedEditColors =
        root.value(QStringLiteral("transcriptUnifiedEditColors")).toBool(true);
    const bool transcriptShowExcludedLines =
        root.value(QStringLiteral("transcriptShowExcludedLines")).toBool(false);
    const QString transcriptSpeakerFilterValue =
        root.value(QStringLiteral("transcriptSpeakerFilterValue")).toString();
    const QString transcriptActiveCutPath =
        root.value(QStringLiteral("transcriptActiveCutPath")).toString();
    const QJsonArray transcriptColumnHidden =
        root.value(QStringLiteral("transcriptColumnHidden")).toArray();
    const QJsonArray speakersColumnHidden =
        root.value(QStringLiteral("speakersColumnHidden")).toArray();
    const bool transcriptFollowCurrentWord = root.value(QStringLiteral("transcriptFollowCurrentWord")).toBool(true);
    const bool correctionsEnabled = root.value(QStringLiteral("correctionsEnabled")).toBool(true);
    const bool gradingFollowCurrent = root.value(QStringLiteral("gradingFollowCurrent")).toBool(true);
    const bool gradingAutoScroll = root.value(QStringLiteral("gradingAutoScroll")).toBool(true);
    const bool gradingPreview = root.value(QStringLiteral("gradingPreview")).toBool(true);
    const bool keyframesFollowCurrent = root.value(QStringLiteral("keyframesFollowCurrent")).toBool(true);
    const bool keyframesAutoScroll = root.value(QStringLiteral("keyframesAutoScroll")).toBool(true);
    const int selectedInspectorTab = root.value(QStringLiteral("selectedInspectorTab")).toInt(0);
    const QString selectedInspectorTabLabel =
        root.value(QStringLiteral("selectedInspectorTabLabel")).toString().trimmed();
    const qreal playbackSpeed = qBound<qreal>(0.1, root.value(QStringLiteral("playbackSpeed")).toDouble(1.0), 3.0);
    const qreal exportPlaybackSpeed =
        qBound<qreal>(0.1,
                      root.value(QStringLiteral("exportPlaybackSpeed")).toDouble(playbackSpeed),
                      8.0);
    const PlaybackClockSource playbackClockSource = playbackClockSourceFromString(
        root.value(QStringLiteral("playbackClockSource"))
            .toString(playbackClockSourceToString(PlaybackClockSource::Auto)));
    PlaybackAudioWarpMode playbackAudioWarpMode = playbackAudioWarpModeFromString(
        root.value(QStringLiteral("playbackAudioWarpMode"))
            .toString(playbackAudioWarpModeToString(PlaybackAudioWarpMode::Disabled)));
    const int roundedPlaybackSpeed = qRound(playbackSpeed);
    const bool hasPitchPreservingPreview =
        (roundedPlaybackSpeed == 2 || roundedPlaybackSpeed == 3) &&
        qAbs(playbackSpeed - static_cast<qreal>(roundedPlaybackSpeed)) < 0.0001;
    if (hasPitchPreservingPreview &&
        playbackAudioWarpMode == PlaybackAudioWarpMode::Varispeed &&
        !root.value(QStringLiteral("playbackAudioWarpModeExplicit")).toBool(false)) {
        playbackAudioWarpMode = PlaybackAudioWarpMode::TimeStretch;
    }
    PlaybackClockSource effectivePlaybackClockSource = playbackClockSource;
    if (!root.value(QStringLiteral("playbackClockSourceExplicit")).toBool(false) &&
        playbackClockSource == PlaybackClockSource::Timeline &&
        playbackAudioWarpModeUsesTimeStretch(playbackAudioWarpMode)) {
        effectivePlaybackClockSource = PlaybackClockSource::Auto;
    }
    const bool playbackLoopEnabled =
        root.value(QStringLiteral("playbackLoopEnabled")).toBool(false);
    const qreal timelineZoom = root.value(QStringLiteral("timelineZoom")).toDouble(4.0);
    const int timelineVerticalScroll = root.value(QStringLiteral("timelineVerticalScroll")).toInt(0);
    const int64_t exportStartFrame = root.value(QStringLiteral("exportStartFrame")).toVariant().toLongLong();
    const int64_t exportEndFrame = root.value(QStringLiteral("exportEndFrame")).toVariant().toLongLong();
    const QString previewViewMode = root.value(QStringLiteral("previewViewMode")).toString(QStringLiteral("video"));
    m_renderBackendPreference = renderBackendPreference.isEmpty()
                                    ? QStringLiteral("vulkan")
                                    : renderBackendPreference;
    m_previewVulkanPresenterPreference = previewVulkanPresenterPreference;
    qputenv("JCUT_RENDER_BACKEND", m_renderBackendPreference.toUtf8());
    qputenv("JCUT_VULKAN_PREVIEW_PRESENTER", m_previewVulkanPresenterPreference.toUtf8());
    const QString aiSelectedModel = root.value(QStringLiteral("aiSelectedModel")).toString(QStringLiteral("deepseek-chat"));
    const QString aiProxyBaseUrl = root.value(QStringLiteral("aiProxyBaseUrl")).toString();
    const QString aiAuthToken = root.value(QStringLiteral("aiAuthToken")).toString();
    const bool featureAiPanel = root.value(QStringLiteral("feature_ai_panel")).toBool(true);
    const bool featureAiSpeakerCleanup = root.value(QStringLiteral("feature_ai_speaker_cleanup")).toBool(true);
    const bool featureAudioPreviewMode = root.value(QStringLiteral("feature_audio_preview_mode")).toBool(true);
    const bool featureAudioDynamicsTools = root.value(QStringLiteral("feature_audio_dynamics_tools")).toBool(true);
    const int aiUsageBudgetCap = qMax(1, root.value(QStringLiteral("aiUsageBudgetCap")).toInt(200));
    const int aiUsageRequests = qMax(0, root.value(QStringLiteral("aiUsageRequests")).toInt(0));
    const int aiUsageFailures = qMax(0, root.value(QStringLiteral("aiUsageFailures")).toInt(0));
    const int aiRateLimitPerMinute = qMax(1, root.value(QStringLiteral("aiRateLimitPerMinute")).toInt(12));
    const int aiRequestTimeoutMs = qMax(1000, root.value(QStringLiteral("aiRequestTimeoutMs")).toInt(15000));
    const int aiRequestRetries = qBound(0, root.value(QStringLiteral("aiRequestRetries")).toInt(1), 3);
    PreviewSurface::AudioDynamicsSettings loadedAudioDynamics;
    loadedAudioDynamics.amplifyEnabled = root.value(QStringLiteral("audioAmplifyEnabled")).toBool(false);
    loadedAudioDynamics.amplifyDb = root.value(QStringLiteral("audioAmplifyDb")).toDouble(0.0);
    loadedAudioDynamics.normalizeEnabled = root.value(QStringLiteral("audioNormalizeEnabled")).toBool(false);
    loadedAudioDynamics.normalizeTargetDb = root.value(QStringLiteral("audioNormalizeTargetDb")).toDouble(-1.0);
    loadedAudioDynamics.stereoToMonoEnabled = root.value(QStringLiteral("audioStereoToMonoEnabled")).toBool(false);
    loadedAudioDynamics.selectiveNormalizeEnabled =
        root.value(QStringLiteral("audioSelectiveNormalizeEnabled")).toBool(false);
    loadedAudioDynamics.selectiveNormalizeMinSegmentSeconds =
        root.value(QStringLiteral("audioSelectiveNormalizeMinSegmentSeconds")).toDouble(0.5);
    const bool hasSelectivePeakDb =
        root.contains(QStringLiteral("audioSelectiveNormalizePeakDb"));
    loadedAudioDynamics.selectiveNormalizePeakDb =
        root.value(QStringLiteral("audioSelectiveNormalizePeakDb")).toDouble(-12.0);
    // Compatibility migration: older builds forced -0.5 dBFS, which is often too strict to
    // trigger selective segments. Migrate legacy defaults to a practical threshold.
    if (!hasSelectivePeakDb ||
        (loadedAudioDynamics.selectiveNormalizePeakDb > -1.0 &&
         loadedAudioDynamics.selectiveNormalizePeakDb <= 0.0)) {
        loadedAudioDynamics.selectiveNormalizePeakDb = -12.0;
    }
    loadedAudioDynamics.selectiveNormalizePasses =
        qBound(1, root.value(QStringLiteral("audioSelectiveNormalizePasses")).toInt(1), 8);
    loadedAudioDynamics.selectiveNormalizeOverlayVisible =
        root.value(QStringLiteral("audioSelectiveNormalizeOverlayVisible")).toBool(true);
    loadedAudioDynamics.transcriptNormalizeEnabled =
        root.value(QStringLiteral("audioTranscriptNormalizeEnabled")).toBool(false);
    loadedAudioDynamics.waveformPreviewPostProcessing =
        root.value(QStringLiteral("audioWaveformPreviewPostProcessing")).toBool(true);
    loadedAudioDynamics.peakReductionEnabled = root.value(QStringLiteral("audioPeakReductionEnabled")).toBool(false);
    loadedAudioDynamics.peakThresholdDb = root.value(QStringLiteral("audioPeakThresholdDb")).toDouble(-6.0);
    loadedAudioDynamics.limiterEnabled = root.value(QStringLiteral("audioLimiterEnabled")).toBool(false);
    loadedAudioDynamics.limiterThresholdDb = root.value(QStringLiteral("audioLimiterThresholdDb")).toDouble(-1.0);
    loadedAudioDynamics.compressorEnabled = root.value(QStringLiteral("audioCompressorEnabled")).toBool(false);
    loadedAudioDynamics.compressorThresholdDb = root.value(QStringLiteral("audioCompressorThresholdDb")).toDouble(-18.0);
    loadedAudioDynamics.compressorRatio = root.value(QStringLiteral("audioCompressorRatio")).toDouble(3.0);
    loadedAudioDynamics.softClipEnabled = root.value(QStringLiteral("audioSoftClipEnabled")).toBool(false);
    const bool audioSpeakerHoverModalEnabled =
        root.value(QStringLiteral("audioSpeakerHoverModalEnabled")).toBool(true);
    const bool audioWaveformVisible =
        root.value(QStringLiteral("audioWaveformVisible")).toBool(true);
    const PreviewSurface::AudioVisualizationMode audioVisualizationMode =
        root.value(QStringLiteral("audioVisualizationMode")).toInt(
            static_cast<int>(PreviewSurface::AudioVisualizationMode::Waveform)) == static_cast<int>(PreviewSurface::AudioVisualizationMode::Spectrum)
            ? PreviewSurface::AudioVisualizationMode::Spectrum
            : PreviewSurface::AudioVisualizationMode::Waveform;
    PreviewSurface::LoiaconoSpectrumSettings loiaconoSpectrumSettings;
    loiaconoSpectrumSettings.multiple =
        qBound(2, root.value(QStringLiteral("loiaconoMultiple")).toInt(loiaconoSpectrumSettings.multiple), 240);
    loiaconoSpectrumSettings.bins =
        qBound(32, root.value(QStringLiteral("loiaconoBins")).toInt(loiaconoSpectrumSettings.bins), 2400);
    loiaconoSpectrumSettings.freqMin =
        qBound(20, root.value(QStringLiteral("loiaconoFreqMin")).toInt(loiaconoSpectrumSettings.freqMin), 2000);
    loiaconoSpectrumSettings.freqMax =
        qBound(loiaconoSpectrumSettings.freqMin + 50,
               root.value(QStringLiteral("loiaconoFreqMax")).toInt(loiaconoSpectrumSettings.freqMax),
               12000);
    loiaconoSpectrumSettings.sampleRate =
        qBound(8000, root.value(QStringLiteral("loiaconoSampleRate")).toInt(loiaconoSpectrumSettings.sampleRate), 192000);
    loiaconoSpectrumSettings.gain =
        qBound(0.1, root.value(QStringLiteral("loiaconoGain")).toDouble(loiaconoSpectrumSettings.gain), 20.0);
    loiaconoSpectrumSettings.gamma =
        qBound(0.1, root.value(QStringLiteral("loiaconoGamma")).toDouble(loiaconoSpectrumSettings.gamma), 2.0);
    loiaconoSpectrumSettings.floor =
        qBound(0.0, root.value(QStringLiteral("loiaconoFloor")).toDouble(loiaconoSpectrumSettings.floor), 0.5);
    loiaconoSpectrumSettings.leakiness =
        qBound(0.99, root.value(QStringLiteral("loiaconoLeakiness")).toDouble(loiaconoSpectrumSettings.leakiness), 1.0);
    loiaconoSpectrumSettings.temporalWeightingMode =
        root.value(QStringLiteral("loiaconoTemporalWeightingMode"))
            .toInt(loiaconoSpectrumSettings.temporalWeightingMode);
    loiaconoSpectrumSettings.normalizationMode =
        root.value(QStringLiteral("loiaconoNormalizationMode"))
            .toInt(loiaconoSpectrumSettings.normalizationMode);
    loiaconoSpectrumSettings.windowLengthMode =
        root.value(QStringLiteral("loiaconoWindowLengthMode"))
            .toInt(loiaconoSpectrumSettings.windowLengthMode);
    loiaconoSpectrumSettings.algorithmMode =
        root.value(QStringLiteral("loiaconoAlgorithmMode"))
            .toInt(loiaconoSpectrumSettings.algorithmMode);
    
    QVector<ExportRangeSegment> loadedExportRanges;
    const QJsonArray exportRanges = root.value(QStringLiteral("exportRanges")).toArray();
    loadedExportRanges.reserve(exportRanges.size());
    for (const QJsonValue &value : exportRanges)
    {
        if (!value.isObject()) continue;
        const QJsonObject obj = value.toObject();
        ExportRangeSegment range;
        range.startFrame = qMax<int64_t>(0, obj.value(QStringLiteral("startFrame")).toVariant().toLongLong());
        range.endFrame = qMax<int64_t>(0, obj.value(QStringLiteral("endFrame")).toVariant().toLongLong());
        loadedExportRanges.push_back(range);
    }
    
    QStringList expandedExplorerPaths;
    QJsonArray expandedFoldersJson = root.value(QStringLiteral("mediaExpandedFolders")).toArray();
    if (expandedFoldersJson.isEmpty()) {
        expandedFoldersJson = root.value(QStringLiteral("explorerExpandedFolders")).toArray();
    }
    for (const QJsonValue &value : expandedFoldersJson)
    {
        const QString path = value.toString();
        if (!path.isEmpty()) expandedExplorerPaths.push_back(path);
    }
    
    QVector<TimelineClip> loadedClips;
    QVector<RenderSyncMarker> loadedRenderSyncMarkers;
    const int64_t currentFrame = root.value(QStringLiteral("currentFrame")).toVariant().toLongLong();
    const QString selectedClipId = root.value(QStringLiteral("selectedClipId")).toString();
    QVector<TimelineTrack> loadedTracks;

    markStartup(QStringLiteral("apply_state.timeline_parse.begin"));
    const QJsonArray clips = root.value(QStringLiteral("timeline")).toArray();
    loadedClips.reserve(clips.size());
    for (const QJsonValue &value : clips)
    {
        if (!value.isObject()) continue;
        TimelineClip clip = clipFromJson(resolveClipStateObjectPaths(value.toObject(), rootPath));
        resolveClipStatePaths(&clip, rootPath);
        if (clip.trackIndex < 0) clip.trackIndex = loadedClips.size();
        if (!clip.filePath.isEmpty() || clip.mediaType == ClipMediaType::Title)
            loadedClips.push_back(clip);
    }
    markStartup(QStringLiteral("apply_state.timeline_parse.end"),
                QJsonObject{{QStringLiteral("loaded_clip_count"), loadedClips.size()}});
    if (!renderUseProxies && hasUsableConfiguredProxy(loadedClips)) {
        renderUseProxies = true;
        markStartup(QStringLiteral("apply_state.proxy_playback_restored"),
                    QJsonObject{{QStringLiteral("reason"), QStringLiteral("usable_clip_proxy_configured")}});
    }
    constexpr int kCurrentMaskArchitectureVersion = 2;
    const int loadedMaskArchitectureVersion =
        root.value(QStringLiteral("maskArchitectureVersion")).toInt(1);
    if (loadedMaskArchitectureVersion < kCurrentMaskArchitectureVersion) {
        migrateLegacyMaskGradingToMattes(loadedClips);
    }
    normalizeMaskMatteClips(loadedClips);

    if (!startupMarking && !m_restoringHistory && !loadedClips.isEmpty()) {
        QElapsedTimer relocateTimer;
        relocateTimer.start();
        markStartup(QStringLiteral("apply_state.media_relocate.begin"),
                    QJsonObject{{QStringLiteral("candidate_clip_count"), loadedClips.size()}});
        const bool relocated = reconcileMissingMediaForClips(&loadedClips, rootPath);
        markStartup(QStringLiteral("apply_state.media_relocate.end"),
                    QJsonObject{
                        {QStringLiteral("elapsed_ms"), relocateTimer.elapsed()},
                        {QStringLiteral("relocated"), relocated}
                    });
    }

    markStartup(QStringLiteral("apply_state.tracks_parse.begin"));
    const QJsonArray tracks = root.value(QStringLiteral("tracks")).toArray();
    loadedTracks.reserve(tracks.size());
    for (int i = 0; i < tracks.size(); ++i)
    {
        const QJsonObject obj = tracks.at(i).toObject();
        TimelineTrack track;
        track.name = obj.value(QStringLiteral("name")).toString(QStringLiteral("Track %1").arg(i + 1));
        track.generatedChildTrack = obj.value(QStringLiteral("generatedChildTrack")).toBool(false);
        track.parentClipId = obj.value(QStringLiteral("parentClipId")).toString();
        track.childClipId = obj.value(QStringLiteral("childClipId")).toString();
        track.height = qMax(28, obj.value(QStringLiteral("height")).toInt(72));
        if (obj.contains(QStringLiteral("visualMode"))) {
            track.visualMode = trackVisualModeFromString(obj.value(QStringLiteral("visualMode")).toString());
        } else if (obj.contains(QStringLiteral("visualEnabled")) &&
                   !obj.value(QStringLiteral("visualEnabled")).toBool(true)) {
            track.visualMode = TrackVisualMode::Hidden;
        }
        track.gradingPreviewEnabled =
            obj.value(QStringLiteral("gradingPreviewEnabled")).toBool(gradingPreview);
        track.audioEnabled = obj.value(QStringLiteral("audioEnabled")).toBool(true);
        track.audioBusId = obj.value(QStringLiteral("audioBusId")).toString();
        track.audioGain = qBound<qreal>(0.0, obj.value(QStringLiteral("audioGain")).toDouble(1.0), 4.0);
        track.audioMuted = obj.value(QStringLiteral("audioMuted")).toBool(false);
        track.audioSolo = obj.value(QStringLiteral("audioSolo")).toBool(false);
        track.audioWaveformVisible = obj.value(QStringLiteral("audioWaveformVisible")).toBool(true);
        track.effectPreset =
            effectPresetFromJson(obj.value(QStringLiteral("effectPreset")).toString(QStringLiteral("none")));
        track.effectRows = qBound(1,
                                  obj.value(QStringLiteral("effectRows")).toInt(32),
                                  track.effectPreset == ClipEffectPreset::ProgressiveEdgeStretch ? 512 : 96);
        track.differenceReferenceFrames = qBound(1, obj.value(QStringLiteral("differenceReferenceFrames")).toInt(1), 300);
        track.differenceThreshold = qBound<qreal>(0.0, obj.value(QStringLiteral("differenceThreshold")).toDouble(0.10), 1.0);
        track.differenceSoftness = qBound<qreal>(0.0, obj.value(QStringLiteral("differenceSoftness")).toDouble(0.05), 1.0);
        track.temporalEchoCount = qBound(1, obj.value(QStringLiteral("temporalEchoCount")).toInt(4), 12);
        track.temporalEchoSpacingFrames = qBound(1, obj.value(QStringLiteral("temporalEchoSpacingFrames")).toInt(2), 120);
        track.temporalEchoDecay = qBound<qreal>(0.0, obj.value(QStringLiteral("temporalEchoDecay")).toDouble(0.65), 1.0);
        track.effectSpeed =
            qBound<qreal>(-8.0, obj.value(QStringLiteral("effectSpeed")).toDouble(1.0), 8.0);
        track.effectScale =
            qBound<qreal>(0.1, obj.value(QStringLiteral("effectScale")).toDouble(1.0), 8.0);
        track.effectAlternateDirection =
            obj.value(QStringLiteral("effectAlternateDirection")).toBool(true);
        track.tilingPattern =
            tilingPatternFromJson(obj.value(QStringLiteral("tilingPattern")).toString(QStringLiteral("grid")));
        track.tilingSpacing =
            qBound<qreal>(0.1, obj.value(QStringLiteral("tilingSpacing")).toDouble(1.0), 8.0);
        track.tilingWrap = obj.value(QStringLiteral("tilingWrap")).toBool(true);
        track.effectParameterSets = obj.value(QStringLiteral("effectParameterSets")).toObject();
        loadedTracks.push_back(track);
    }
    markStartup(QStringLiteral("apply_state.tracks_parse.end"),
                QJsonObject{{QStringLiteral("loaded_track_count"), loadedTracks.size()}});
    if (legacyBackgroundProgressiveStretch) {
        QString migratedClipId;
        const bool migrated = migrateLegacyBackgroundProgressiveStretchToClipEffect(
            &loadedClips,
            loadedTracks,
            backgroundFillStretchSourceClipId,
            selectedClipId,
            &migratedClipId);
        backgroundFillEffect = BackgroundFillEffect::EdgeStretch;
        backgroundFillEdgeProgressive = false;
        markStartup(QStringLiteral("apply_state.progressive_edge_stretch_migration"),
                    QJsonObject{
                        {QStringLiteral("migrated"), migrated},
                        {QStringLiteral("clip_id"), migratedClipId},
                        {QStringLiteral("source"), QStringLiteral("legacy_background_fill")}
                    });
    }
    markStartup(QStringLiteral("apply_state.render_sync_parse.begin"));
       const QJsonArray renderSyncMarkers = root.value(QStringLiteral("renderSyncMarkers")).toArray();
    loadedRenderSyncMarkers.reserve(renderSyncMarkers.size());
    for (const QJsonValue &value : renderSyncMarkers)
    {
        if (!value.isObject()) continue;
        const QJsonObject obj = value.toObject();
        RenderSyncMarker marker;
        marker.clipId = obj.value(QStringLiteral("clipId")).toString();
        marker.frame = qMax<int64_t>(0, obj.value(QStringLiteral("frame")).toVariant().toLongLong());
        marker.action = renderSyncActionFromString(obj.value(QStringLiteral("action")).toString());
        marker.count = qMax(1, obj.value(QStringLiteral("count")).toInt(1));
        loadedRenderSyncMarkers.push_back(marker);
    }
    markStartup(QStringLiteral("apply_state.render_sync_parse.end"),
                QJsonObject{{QStringLiteral("loaded_render_sync_count"), loadedRenderSyncMarkers.size()}});

    const QString resolvedRootPath = QDir(rootPath).absolutePath();
    if (m_explorerPane) {
        m_explorerPane->setInitialRootPath(resolvedRootPath);
        if (!startupMarking) {
            m_explorerPane->restoreExpandedExplorerPaths(expandedExplorerPaths);
        }
    }
    
    if (m_outputWidthSpin) { QSignalBlocker block(m_outputWidthSpin); m_outputWidthSpin->setValue(outputWidth); }
    if (m_outputHeightSpin) { QSignalBlocker block(m_outputHeightSpin); m_outputHeightSpin->setValue(outputHeight); }
    if (m_outputFpsSpin) { QSignalBlocker block(m_outputFpsSpin); m_outputFpsSpin->setValue(outputFps); }
    if (m_outputFormatCombo) {
        QSignalBlocker block(m_outputFormatCombo);
        const int formatIndex = m_outputFormatCombo->findData(outputFormat);
        if (formatIndex >= 0) m_outputFormatCombo->setCurrentIndex(formatIndex);
    }
    if (m_renderBackendCombo) {
        QSignalBlocker block(m_renderBackendCombo);
        const int backendIndex = m_renderBackendCombo->findData(m_renderBackendPreference);
        if (backendIndex >= 0) {
            m_renderBackendCombo->setCurrentIndex(backendIndex);
        }
    }
    m_lastRenderOutputPath = lastRenderOutputPath;
    m_exportPlaybackSpeed = exportPlaybackSpeed;
    m_aiSelectedModel = aiSelectedModel;
    m_aiProxyBaseUrl = aiProxyBaseUrl.trimmed();
    m_aiAuthToken = aiAuthToken.trimmed();
    if (!m_aiAuthToken.isEmpty()) {
        QString secureStoreError;
        if (writeAiTokenToSecureStore(m_aiAuthToken, QString(), &secureStoreError)) {
            m_aiAuthToken.clear();
            m_aiRefreshToken.clear();
        } else {
            qWarning() << "AI token migration to secure store failed:" << secureStoreError;
        }
    }
    m_featureAiPanel = featureAiPanel;
    m_featureAiSpeakerCleanup = featureAiSpeakerCleanup;
    m_featureAudioPreviewMode = featureAudioPreviewMode;
    m_featureAudioDynamicsTools = featureAudioDynamicsTools;
    m_aiUsageBudgetCap = aiUsageBudgetCap;
    m_aiUsageRequests = aiUsageRequests;
    m_aiUsageFailures = aiUsageFailures;
    m_aiRateLimitPerMinute = aiRateLimitPerMinute;
    m_aiRequestTimeoutMs = aiRequestTimeoutMs;
    m_aiRequestRetries = aiRequestRetries;
    if (m_renderUseProxiesCheckBox) {
        QSignalBlocker block(m_renderUseProxiesCheckBox);
        m_renderUseProxiesCheckBox->setChecked(renderUseProxies);
    }
    if (m_createImageSequenceCheckBox) {
        QSignalBlocker block(m_createImageSequenceCheckBox);
        m_createImageSequenceCheckBox->setChecked(createImageSequence);
    }
    if (m_imageSequenceFormatCombo) {
        QSignalBlocker block(m_imageSequenceFormatCombo);
        const int formatIndex = m_imageSequenceFormatCombo->findData(imageSequenceFormat);
        m_imageSequenceFormatCombo->setCurrentIndex(qMax(0, formatIndex));
        m_imageSequenceFormatCombo->setEnabled(createImageSequence);
    }
    if (m_backgroundFillEffectCombo) {
        QSignalBlocker block(m_backgroundFillEffectCombo);
        const int effectIndex = m_backgroundFillEffectCombo->findData(
            backgroundFillEffectToString(backgroundFillEffect));
        m_backgroundFillEffectCombo->setCurrentIndex(qMax(0, effectIndex));
    }
    if (m_backgroundFillOpacitySpin) {
        QSignalBlocker block(m_backgroundFillOpacitySpin);
        m_backgroundFillOpacitySpin->setValue(backgroundFillOpacity * 100.0);
    }
    if (m_backgroundFillBrightnessSpin) {
        QSignalBlocker block(m_backgroundFillBrightnessSpin);
        m_backgroundFillBrightnessSpin->setValue(backgroundFillBrightness * 100.0);
    }
    if (m_backgroundFillSaturationSpin) {
        QSignalBlocker block(m_backgroundFillSaturationSpin);
        m_backgroundFillSaturationSpin->setValue(backgroundFillSaturation * 100.0);
    }
    if (m_preview) {
        m_preview->setUseProxyMedia(renderUseProxies);
    }
    m_autosaveIntervalMinutes = autosaveIntervalMinutes;
    m_autosaveMaxBackups = autosaveMaxBackups;
    m_historyMaxEntries = historyMaxEntries;
    m_historyMaxMegabytes = historyMaxMegabytes;
    if (m_autosaveIntervalMinutesSpin) {
        QSignalBlocker block(m_autosaveIntervalMinutesSpin);
        m_autosaveIntervalMinutesSpin->setValue(m_autosaveIntervalMinutes);
    }
    if (m_autosaveMaxBackupsSpin) {
        QSignalBlocker block(m_autosaveMaxBackupsSpin);
        m_autosaveMaxBackupsSpin->setValue(m_autosaveMaxBackups);
    }
    if (m_inspectorPane && m_inspectorPane->historyMaxEntriesSpin()) {
        QSignalBlocker block(m_inspectorPane->historyMaxEntriesSpin());
        m_inspectorPane->historyMaxEntriesSpin()->setValue(m_historyMaxEntries);
    }
    if (m_inspectorPane && m_inspectorPane->historyMaxMegabytesSpin()) {
        QSignalBlocker block(m_inspectorPane->historyMaxMegabytesSpin());
        m_inspectorPane->historyMaxMegabytesSpin()->setValue(m_historyMaxMegabytes);
    }
    m_autosaveTimer.setInterval(m_autosaveIntervalMinutes * 60 * 1000);
    if (m_previewHideOutsideOutputCheckBox) {
        QSignalBlocker block(m_previewHideOutsideOutputCheckBox);
        m_previewHideOutsideOutputCheckBox->setChecked(previewHideOutsideOutput);
    }
    if (m_previewShowSpeakerTrackPointsCheckBox) {
        QSignalBlocker block(m_previewShowSpeakerTrackPointsCheckBox);
        m_previewShowSpeakerTrackPointsCheckBox->setChecked(previewShowSpeakerTrackPoints);
    }
    if (m_previewVulkanPresenterCombo) {
        QSignalBlocker block(m_previewVulkanPresenterCombo);
        const int presenterIndex = m_previewVulkanPresenterCombo->findData(m_previewVulkanPresenterPreference);
        if (presenterIndex >= 0) {
            m_previewVulkanPresenterCombo->setCurrentIndex(presenterIndex);
        }
    }
    if (m_speakerShowFaceDetectionsBoxesCheckBox) {
        QSignalBlocker block(m_speakerShowFaceDetectionsBoxesCheckBox);
        m_speakerShowFaceDetectionsBoxesCheckBox->setChecked(previewShowSpeakerTrackBoxes);
    }
    if (m_speakerShowContiguousSectionsCheckBox) {
        // This checkbox switches both the Speakers work page and the table
        // populated by SpeakersTab, so its signal is part of applying state.
        m_speakerShowContiguousSectionsCheckBox->setChecked(speakerShowContiguousTranscriptSections);
    }
    if (m_speakerApplyTrackToAllMatchingSectionsCheckBox) {
        QSignalBlocker block(m_speakerApplyTrackToAllMatchingSectionsCheckBox);
        m_speakerApplyTrackToAllMatchingSectionsCheckBox->setChecked(speakerApplyTrackToAllMatchingSections);
    }
    if (m_speakerShowRawDetectionsCheckBox) {
        QSignalBlocker block(m_speakerShowRawDetectionsCheckBox);
        m_speakerShowRawDetectionsCheckBox->setChecked(previewShowRawDetections);
    }
    if (m_speakerShowCurrentSpeakerNameCheckBox) {
        QSignalBlocker block(m_speakerShowCurrentSpeakerNameCheckBox);
        m_speakerShowCurrentSpeakerNameCheckBox->setChecked(previewShowCurrentSpeakerName);
    }
    if (m_speakerShowCurrentSpeakerOrganizationCheckBox) {
        QSignalBlocker block(m_speakerShowCurrentSpeakerOrganizationCheckBox);
        m_speakerShowCurrentSpeakerOrganizationCheckBox->setChecked(previewShowCurrentSpeakerOrganization);
    }
    if (m_inspectorPane && !speakerTitleSettings.isEmpty()) {
        auto setCombo = [&speakerTitleSettings](QComboBox* widget, const QString& key) {
            if (!widget || !speakerTitleSettings.contains(key)) return;
            const int index = widget->findData(speakerTitleSettings.value(key).toInt());
            if (index >= 0) widget->setCurrentIndex(index);
        };
        auto setDecimal = [&speakerTitleSettings](QDoubleSpinBox* widget, const QString& key) {
            if (!widget || !speakerTitleSettings.contains(key)) return;
            const QSignalBlocker blocker(widget);
            widget->setValue(speakerTitleSettings.value(key).toDouble(widget->value()));
        };
        auto setInteger = [&speakerTitleSettings](QSpinBox* widget, const QString& key) {
            if (!widget || !speakerTitleSettings.contains(key)) return;
            const QSignalBlocker blocker(widget);
            widget->setValue(speakerTitleSettings.value(key).toInt(widget->value()));
        };
        setCombo(m_inspectorPane->speakerOverlayFlyInStyleCombo(), QStringLiteral("flyInStyle"));
        setDecimal(m_inspectorPane->speakerOverlayFlyInDelaySpin(), QStringLiteral("delaySeconds"));
        setDecimal(m_inspectorPane->speakerOverlayFlyInDurationSpin(), QStringLiteral("durationSeconds"));
        setDecimal(m_inspectorPane->speakerOverlayFlyInTimeSpin(), QStringLiteral("flyTimeSeconds"));
        setDecimal(m_inspectorPane->speakerOverlayWrapRadiusSpin(), QStringLiteral("wrapRadius"));
        setDecimal(m_inspectorPane->speakerOverlayWrapDepthSpin(), QStringLiteral("wrapDepth"));
        setDecimal(m_inspectorPane->speakerOverlayWrapStartAngleSpin(), QStringLiteral("wrapStartAngle"));
        setDecimal(m_inspectorPane->speakerOverlayWrapEndAngleSpin(), QStringLiteral("wrapEndAngle"));
        setDecimal(m_inspectorPane->speakerOverlayWrapPitchSpin(), QStringLiteral("wrapPitch"));
        setDecimal(m_inspectorPane->speakerOverlayWrapRollSpin(), QStringLiteral("wrapRoll"));
        setDecimal(m_inspectorPane->speakerOverlayRotationXSpin(), QStringLiteral("rotationX"));
        setDecimal(m_inspectorPane->speakerOverlayRotationYSpin(), QStringLiteral("rotationY"));
        setDecimal(m_inspectorPane->speakerOverlayRotationZSpin(), QStringLiteral("rotationZ"));
        setInteger(m_inspectorPane->speakerOverlayTitleFontSizeSpin(), QStringLiteral("fontSize"));
        if (QCheckBox* autoFit = m_inspectorPane->speakerOverlayTitleAutoFitCheckBox()) {
            QSignalBlocker block(autoFit);
            autoFit->setChecked(speakerTitleSettings.value(QStringLiteral("autoFitToOutput")).toBool(true));
        }
        setInteger(m_inspectorPane->speakerOverlayTitleBoxWidthSpin(), QStringLiteral("boxWidth"));
        setCombo(m_inspectorPane->speakerOverlayTitleTextMaterialCombo(), QStringLiteral("textMaterial"));
        setCombo(m_inspectorPane->speakerOverlayTitleBorderMaterialCombo(), QStringLiteral("borderMaterial"));
        setDecimal(m_inspectorPane->speakerOverlayTitlePatternScaleSpin(), QStringLiteral("patternScale"));
        setCombo(m_inspectorPane->speakerOverlayTitleExtrudeModeCombo(), QStringLiteral("extrudeMode"));
        setDecimal(m_inspectorPane->speakerOverlayTitleExtrudeDepthSpin(), QStringLiteral("extrudeDepth"));
        setDecimal(m_inspectorPane->speakerOverlayTitleBevelScaleSpin(), QStringLiteral("bevelScale"));
        auto setText = [&speakerTitleSettings](QLineEdit* widget, const QString& key) {
            if (!widget || !speakerTitleSettings.contains(key)) return;
            const QSignalBlocker blocker(widget);
            widget->setText(speakerTitleSettings.value(key).toString());
        };
        setText(m_inspectorPane->speakerOverlayTitleTextPatternPathEdit(), QStringLiteral("textPatternPath"));
        setText(m_inspectorPane->speakerOverlayTitleBorderPatternPathEdit(), QStringLiteral("borderPatternPath"));
        if (QCheckBox* extrude = m_inspectorPane->speakerOverlayTitleExtrudeCheckBox()) {
            const QSignalBlocker blocker(extrude);
            extrude->setChecked(speakerTitleSettings.value(QStringLiteral("extrudeEnabled")).toBool(false));
            const bool enabled = extrude->isChecked();
            if (m_inspectorPane->speakerOverlayTitleExtrudeModeCombo())
                m_inspectorPane->speakerOverlayTitleExtrudeModeCombo()->setEnabled(enabled);
            if (m_inspectorPane->speakerOverlayTitleExtrudeDepthSpin())
                m_inspectorPane->speakerOverlayTitleExtrudeDepthSpin()->setEnabled(enabled);
            if (m_inspectorPane->speakerOverlayTitleBevelScaleSpin())
                m_inspectorPane->speakerOverlayTitleBevelScaleSpin()->setEnabled(enabled);
        }
    }
    if (m_speakerCurrentSpeakerNameTextSizeSpin) {
        QSignalBlocker block(m_speakerCurrentSpeakerNameTextSizeSpin);
        m_speakerCurrentSpeakerNameTextSizeSpin->setValue(previewCurrentSpeakerNameTextScalePercent);
    }
    if (m_speakerCurrentSpeakerOrganizationTextSizeSpin) {
        QSignalBlocker block(m_speakerCurrentSpeakerOrganizationTextSizeSpin);
        m_speakerCurrentSpeakerOrganizationTextSizeSpin->setValue(
            previewCurrentSpeakerOrganizationTextScalePercent);
    }
    if (m_speakerCurrentSpeakerNameYPositionSpin) {
        QSignalBlocker block(m_speakerCurrentSpeakerNameYPositionSpin);
        m_speakerCurrentSpeakerNameYPositionSpin->setValue(previewCurrentSpeakerNameYPositionPercent);
    }
    if (m_speakerCurrentSpeakerOrganizationYPositionSpin) {
        QSignalBlocker block(m_speakerCurrentSpeakerOrganizationYPositionSpin);
        m_speakerCurrentSpeakerOrganizationYPositionSpin->setValue(
            previewCurrentSpeakerOrganizationYPositionPercent);
    }
    m_speakerCurrentSpeakerNameColor = previewCurrentSpeakerNameColor;
    m_speakerCurrentSpeakerOrganizationColor = previewCurrentSpeakerOrganizationColor;
    m_speakerCurrentSpeakerBackgroundColor = previewCurrentSpeakerBackgroundColor;
    m_speakerCurrentSpeakerBackgroundVisible = previewCurrentSpeakerBackgroundVisible;
    m_speakerCurrentSpeakerBorderColor = previewCurrentSpeakerBorderColor;
    m_speakerCurrentSpeakerShadowColor = previewCurrentSpeakerShadowColor;
    setEditorColorButtonSwatch(m_speakerCurrentSpeakerNameColorButton, m_speakerCurrentSpeakerNameColor);
    setEditorColorButtonSwatch(m_speakerCurrentSpeakerOrganizationColorButton, m_speakerCurrentSpeakerOrganizationColor);
    setEditorColorButtonSwatch(m_speakerCurrentSpeakerBackgroundColorButton, m_speakerCurrentSpeakerBackgroundColor);
    setEditorColorButtonSwatch(m_speakerCurrentSpeakerBorderColorButton, m_speakerCurrentSpeakerBorderColor);
    setEditorColorButtonSwatch(m_speakerCurrentSpeakerShadowColorButton, m_speakerCurrentSpeakerShadowColor);
    if (m_speakerCurrentSpeakerBackgroundVisibleCheckBox) {
        QSignalBlocker block(m_speakerCurrentSpeakerBackgroundVisibleCheckBox);
        m_speakerCurrentSpeakerBackgroundVisibleCheckBox->setChecked(
            speakerTitleSettings.contains(QStringLiteral("backgroundEnabled"))
                ? speakerTitleSettings.value(QStringLiteral("backgroundEnabled")).toBool(true)
                : m_speakerCurrentSpeakerBackgroundVisible);
        m_speakerCurrentSpeakerBackgroundVisible =
            m_speakerCurrentSpeakerBackgroundVisibleCheckBox->isChecked();
    }
    for (QWidget* control : {
             static_cast<QWidget*>(m_speakerCurrentSpeakerBackgroundColorButton),
             static_cast<QWidget*>(m_speakerCurrentSpeakerBackgroundOpacitySpin),
             static_cast<QWidget*>(m_speakerCurrentSpeakerBorderColorButton),
             static_cast<QWidget*>(m_speakerCurrentSpeakerBorderOpacitySpin),
             static_cast<QWidget*>(m_speakerCurrentSpeakerBackgroundRadiusSpin),
             static_cast<QWidget*>(m_speakerCurrentSpeakerBorderWidthSpin)}) {
        if (control) control->setEnabled(m_speakerCurrentSpeakerBackgroundVisible);
    }
    if (m_speakerCurrentSpeakerBackgroundOpacitySpin) {
        QSignalBlocker block(m_speakerCurrentSpeakerBackgroundOpacitySpin);
        m_speakerCurrentSpeakerBackgroundOpacitySpin->setValue(previewCurrentSpeakerBackgroundOpacityPercent);
    }
    if (m_speakerCurrentSpeakerBorderOpacitySpin) {
        QSignalBlocker block(m_speakerCurrentSpeakerBorderOpacitySpin);
        m_speakerCurrentSpeakerBorderOpacitySpin->setValue(previewCurrentSpeakerBorderOpacityPercent);
    }
    if (m_speakerCurrentSpeakerBackgroundRadiusSpin) {
        QSignalBlocker block(m_speakerCurrentSpeakerBackgroundRadiusSpin);
        m_speakerCurrentSpeakerBackgroundRadiusSpin->setValue(previewCurrentSpeakerBackgroundRadiusPx);
    }
    if (m_speakerCurrentSpeakerBorderWidthSpin) {
        QSignalBlocker block(m_speakerCurrentSpeakerBorderWidthSpin);
        m_speakerCurrentSpeakerBorderWidthSpin->setValue(previewCurrentSpeakerBorderWidthPx);
    }
    if (m_speakerCurrentSpeakerShadowCheckBox) {
        QSignalBlocker block(m_speakerCurrentSpeakerShadowCheckBox);
        m_speakerCurrentSpeakerShadowCheckBox->setChecked(previewCurrentSpeakerShadowEnabled);
    }
    if (m_speakerCurrentSpeakerShadowOpacitySpin) {
        QSignalBlocker block(m_speakerCurrentSpeakerShadowOpacitySpin);
        m_speakerCurrentSpeakerShadowOpacitySpin->setValue(previewCurrentSpeakerShadowOpacityPercent);
    }
    if (m_previewPlaybackCacheFallbackCheckBox) {
        QSignalBlocker block(m_previewPlaybackCacheFallbackCheckBox);
        m_previewPlaybackCacheFallbackCheckBox->setChecked(previewPlaybackCacheFallback);
    }
    if (m_previewLeadPrefetchEnabledCheckBox) {
        QSignalBlocker block(m_previewLeadPrefetchEnabledCheckBox);
        m_previewLeadPrefetchEnabledCheckBox->setChecked(previewLeadPrefetchEnabled);
    }
    if (m_previewLeadPrefetchCountSpin) {
        QSignalBlocker block(m_previewLeadPrefetchCountSpin);
        m_previewLeadPrefetchCountSpin->setValue(previewLeadPrefetchCount);
        m_previewLeadPrefetchCountSpin->setEnabled(previewLeadPrefetchEnabled);
    }
    if (m_previewPlaybackWindowAheadSpin) {
        QSignalBlocker block(m_previewPlaybackWindowAheadSpin);
        m_previewPlaybackWindowAheadSpin->setValue(previewPlaybackWindowAhead);
    }
    if (m_previewVisibleQueueReserveSpin) {
        QSignalBlocker block(m_previewVisibleQueueReserveSpin);
        m_previewVisibleQueueReserveSpin->setValue(previewVisibleQueueReserve);
    }
    if (m_outputPlaybackCacheFallbackCheckBox) {
        QSignalBlocker block(m_outputPlaybackCacheFallbackCheckBox);
        m_outputPlaybackCacheFallbackCheckBox->setChecked(previewPlaybackCacheFallback);
    }
    if (m_outputLeadPrefetchEnabledCheckBox) {
        QSignalBlocker block(m_outputLeadPrefetchEnabledCheckBox);
        m_outputLeadPrefetchEnabledCheckBox->setChecked(previewLeadPrefetchEnabled);
    }
    if (m_outputLeadPrefetchCountSpin) {
        QSignalBlocker block(m_outputLeadPrefetchCountSpin);
        m_outputLeadPrefetchCountSpin->setValue(previewLeadPrefetchCount);
        m_outputLeadPrefetchCountSpin->setEnabled(previewLeadPrefetchEnabled);
    }
    if (m_outputPlaybackWindowAheadSpin) {
        QSignalBlocker block(m_outputPlaybackWindowAheadSpin);
        m_outputPlaybackWindowAheadSpin->setValue(previewPlaybackWindowAhead);
    }
    if (m_outputVisibleQueueReserveSpin) {
        QSignalBlocker block(m_outputVisibleQueueReserveSpin);
        m_outputVisibleQueueReserveSpin->setValue(previewVisibleQueueReserve);
    }
    if (m_outputPrefetchMaxQueueDepthSpin) {
        QSignalBlocker block(m_outputPrefetchMaxQueueDepthSpin);
        m_outputPrefetchMaxQueueDepthSpin->setValue(debugPrefetchMaxQueueDepth);
    }
    if (m_outputPrefetchMaxInflightSpin) {
        QSignalBlocker block(m_outputPrefetchMaxInflightSpin);
        m_outputPrefetchMaxInflightSpin->setValue(debugPrefetchMaxInflight);
    }
    if (m_outputPrefetchMaxPerTickSpin) {
        QSignalBlocker block(m_outputPrefetchMaxPerTickSpin);
        m_outputPrefetchMaxPerTickSpin->setValue(debugPrefetchMaxPerTick);
    }
    if (m_outputPrefetchSkipVisiblePendingThresholdSpin) {
        QSignalBlocker block(m_outputPrefetchSkipVisiblePendingThresholdSpin);
        m_outputPrefetchSkipVisiblePendingThresholdSpin->setValue(debugPrefetchSkipVisiblePendingThreshold);
    }
    if (m_outputDecoderLaneCountSpin) {
        QSignalBlocker block(m_outputDecoderLaneCountSpin);
        m_outputDecoderLaneCountSpin->setValue(debugDecoderLaneCount);
    }
    if (m_outputDecodeModeCombo) {
        QSignalBlocker block(m_outputDecodeModeCombo);
        const editor::DecodePreference visibleDecodePreference =
            debugDecodePreference == editor::DecodePreference::HardwareZeroCopy
                ? editor::DecodePreference::Hardware
                : debugDecodePreference;
        const int decodeModeIndex =
            m_outputDecodeModeCombo->findData(editor::decodePreferenceToString(visibleDecodePreference));
        if (decodeModeIndex >= 0) {
            m_outputDecodeModeCombo->setCurrentIndex(decodeModeIndex);
        }
    }
    if (m_outputDeterministicPipelineCheckBox) {
        QSignalBlocker block(m_outputDeterministicPipelineCheckBox);
        m_outputDeterministicPipelineCheckBox->setChecked(debugDeterministicPipeline);
    }
    if (m_timelineAudioEnvelopeGranularitySpin) {
        QSignalBlocker block(m_timelineAudioEnvelopeGranularitySpin);
        m_timelineAudioEnvelopeGranularitySpin->setValue(timelineAudioEnvelopeGranularity);
    }
    if (m_preferencesFeatureAiPanelCheckBox) {
        QSignalBlocker block(m_preferencesFeatureAiPanelCheckBox);
        m_preferencesFeatureAiPanelCheckBox->setChecked(m_featureAiPanel);
    }
    if (m_preferencesFeatureAiSpeakerCleanupCheckBox) {
        QSignalBlocker block(m_preferencesFeatureAiSpeakerCleanupCheckBox);
        m_preferencesFeatureAiSpeakerCleanupCheckBox->setChecked(m_featureAiSpeakerCleanup);
    }
    if (m_preferencesFeatureAudioPreviewModeCheckBox) {
        QSignalBlocker block(m_preferencesFeatureAudioPreviewModeCheckBox);
        m_preferencesFeatureAudioPreviewModeCheckBox->setChecked(m_featureAudioPreviewMode);
    }
    if (m_preferencesFeatureAudioDynamicsToolsCheckBox) {
        QSignalBlocker block(m_preferencesFeatureAudioDynamicsToolsCheckBox);
        m_preferencesFeatureAudioDynamicsToolsCheckBox->setChecked(m_featureAudioDynamicsTools);
    }
    m_transcriptPrependMs = transcriptPrependMs;
    m_transcriptPostpendMs = transcriptPostpendMs;
    m_transcriptOffsetMs = qBound(-10000, transcriptOffsetMs, 10000);
    if (m_preview) {
        m_preview->setTranscriptOverlayTimingPaddingMs(
            m_transcriptPrependMs, m_transcriptPostpendMs, m_transcriptOffsetMs);
    }
    m_speechFilterFadeSamples = qMax(0, speechFilterFadeSamples);
    const AudioEngine::SpeechFilterFadeMode legacyFallback =
        m_speechFilterFadeSamples <= 0
            ? AudioEngine::SpeechFilterFadeMode::JumpCut
            : AudioEngine::SpeechFilterFadeMode::Fade;
    m_speechFilterEnabled = hasSpeechFilterFadeMode
        ? speechFilterFadeModeValue != QStringLiteral("none")
        : legacySpeechFilterEnabled;
    m_speechFilterFadeMode = AudioEngine::speechFilterFadeModeFromString(
        speechFilterFadeModeValue, legacyFallback);
    m_speechFilterCurveStrength = speechFilterCurveStrength;
    if (speechFilterRangeCrossfade && m_speechFilterEnabled &&
        m_speechFilterFadeMode != AudioEngine::SpeechFilterFadeMode::JumpCut) {
        m_speechFilterFadeMode = AudioEngine::SpeechFilterFadeMode::Crossfade;
    }
    m_speechFilterRangeCrossfade = false;
    m_speechFilterFrameTransitionMode = playbackFrameTransitionModeFromString(
        speechFilterFrameTransitionModeValue,
        speechFilterFrameCrossfadeEnabled
            ? PlaybackFrameTransitionMode::Crossfade
            : PlaybackFrameTransitionMode::Cut);
    m_speechFilterFrameCrossfadeEnabled =
        m_speechFilterFrameTransitionMode == PlaybackFrameTransitionMode::Crossfade;
    m_speechFilterFrameCrossfadeFrames = qBound(0, speechFilterFrameCrossfadeFrames, 240);
    
    if (m_transcriptPrependMsSpin) { QSignalBlocker block(m_transcriptPrependMsSpin); m_transcriptPrependMsSpin->setValue(m_transcriptPrependMs); }
    if (m_transcriptPostpendMsSpin) { QSignalBlocker block(m_transcriptPostpendMsSpin); m_transcriptPostpendMsSpin->setValue(m_transcriptPostpendMs); }
    if (m_transcriptOffsetMsSpin) { QSignalBlocker block(m_transcriptOffsetMsSpin); m_transcriptOffsetMsSpin->setValue(m_transcriptOffsetMs); }
    if (m_speechFilterFadeModeCombo) {
        QSignalBlocker block(m_speechFilterFadeModeCombo);
        const QString restoredMode =
            m_speechFilterEnabled
                ? AudioEngine::speechFilterFadeModeToString(m_speechFilterFadeMode)
                : QStringLiteral("none");
        const int index = m_speechFilterFadeModeCombo->findData(restoredMode);
        if (index >= 0) {
            m_speechFilterFadeModeCombo->setCurrentIndex(index);
        }
    }
    if (m_speechFilterFadeSamplesSpin) { QSignalBlocker block(m_speechFilterFadeSamplesSpin); m_speechFilterFadeSamplesSpin->setValue(m_speechFilterFadeSamples); }
    if (m_speechFilterCurveStrengthSpin) {
        QSignalBlocker block(m_speechFilterCurveStrengthSpin);
        m_speechFilterCurveStrengthSpin->setValue(m_speechFilterCurveStrength);
    }
    if (m_speechFilterRangeCrossfadeCheckBox) {
        QSignalBlocker block(m_speechFilterRangeCrossfadeCheckBox);
        m_speechFilterRangeCrossfadeCheckBox->setChecked(m_speechFilterRangeCrossfade);
    }
    if (m_speechFilterFrameTransitionModeCombo) {
        QSignalBlocker block(m_speechFilterFrameTransitionModeCombo);
        const int index = m_speechFilterFrameTransitionModeCombo->findData(
            playbackFrameTransitionModeToString(m_speechFilterFrameTransitionMode));
        if (index >= 0) {
            m_speechFilterFrameTransitionModeCombo->setCurrentIndex(index);
        }
    }
    if (m_speechFilterFrameCrossfadeCheckBox) {
        QSignalBlocker block(m_speechFilterFrameCrossfadeCheckBox);
        m_speechFilterFrameCrossfadeCheckBox->setChecked(m_speechFilterFrameCrossfadeEnabled);
    }
    if (m_speechFilterFrameCrossfadeFramesSpin) {
        QSignalBlocker block(m_speechFilterFrameCrossfadeFramesSpin);
        m_speechFilterFrameCrossfadeFramesSpin->setValue(m_speechFilterFrameCrossfadeFrames);
    }
    refreshSpeechFilterFadeParameterVisibility();
    if (m_transcriptTab) {
        m_transcriptTab->syncSpeechFilterControlsFromWidgets();
    }
    invalidatePlaybackRangeCaches();
    if (m_inspectorPane && m_inspectorPane->transcriptUnifiedEditModeCheckBox()) {
        QSignalBlocker block(m_inspectorPane->transcriptUnifiedEditModeCheckBox());
        m_inspectorPane->transcriptUnifiedEditModeCheckBox()->setChecked(transcriptUnifiedEditColors);
    }
    if (m_inspectorPane && m_inspectorPane->transcriptShowExcludedLinesCheckBox()) {
        QSignalBlocker block(m_inspectorPane->transcriptShowExcludedLinesCheckBox());
        m_inspectorPane->transcriptShowExcludedLinesCheckBox()->setChecked(transcriptShowExcludedLines);
    }
    if (!startupMarking) {
        if (m_inspectorPane && m_inspectorPane->transcriptSpeakerFilterCombo()) {
            m_inspectorPane->transcriptSpeakerFilterCombo()->setProperty(
                "pendingSpeakerFilterValue", transcriptSpeakerFilterValue);
        }
        if (m_inspectorPane && m_inspectorPane->transcriptScriptVersionCombo()) {
            m_inspectorPane->transcriptScriptVersionCombo()->setProperty(
                "pendingCutPath", transcriptActiveCutPath);
        }
        if (m_transcriptTable && !transcriptColumnHidden.isEmpty()) {
            const int limit = qMin(m_transcriptTable->columnCount(), transcriptColumnHidden.size());
            for (int i = 0; i < limit; ++i) {
                if (i == 5) { // Text column must always be visible
                    m_transcriptTable->setColumnHidden(i, false);
                    continue;
                }
                m_transcriptTable->setColumnHidden(i, transcriptColumnHidden.at(i).toBool(false));
            }
        }
        if (m_inspectorPane) {
            if (SpeakersTable* speakersTable =
                    qobject_cast<SpeakersTable*>(m_inspectorPane->speakersTable());
                speakersTable && !speakersColumnHidden.isEmpty()) {
                speakersTable->applyHiddenColumns(speakersColumnHidden);
            }
        }
    }
    
    if (m_transcriptFollowCurrentWordCheckBox) { QSignalBlocker block(m_transcriptFollowCurrentWordCheckBox); m_transcriptFollowCurrentWordCheckBox->setChecked(transcriptFollowCurrentWord); }
    if (m_gradingFollowCurrentCheckBox) { QSignalBlocker block(m_gradingFollowCurrentCheckBox); m_gradingFollowCurrentCheckBox->setChecked(gradingFollowCurrent); }
    if (m_gradingAutoScrollCheckBox) { QSignalBlocker block(m_gradingAutoScrollCheckBox); m_gradingAutoScrollCheckBox->setChecked(gradingAutoScroll); }
    if (m_bypassGradingCheckBox) { QSignalBlocker block(m_bypassGradingCheckBox); m_bypassGradingCheckBox->setChecked(true); }
    if (m_keyframesFollowCurrentCheckBox) { QSignalBlocker block(m_keyframesFollowCurrentCheckBox); m_keyframesFollowCurrentCheckBox->setChecked(keyframesFollowCurrent); }
    if (m_keyframesAutoScrollCheckBox) { QSignalBlocker block(m_keyframesAutoScrollCheckBox); m_keyframesAutoScrollCheckBox->setChecked(keyframesAutoScroll); }
    if (m_inspectorPane && m_inspectorPane->correctionsEnabledCheck()) {
        QSignalBlocker block(m_inspectorPane->correctionsEnabledCheck());
        m_inspectorPane->correctionsEnabledCheck()->setChecked(correctionsEnabled);
    }
    m_correctionsEnabled = correctionsEnabled;
    if (m_preview) {
        m_preview->setCorrectionsEnabled(m_correctionsEnabled);
    }
    
    if (!startupMarking && m_inspectorTabs && m_inspectorTabs->count() > 0) {
        const auto isTabNamed = [this](const QString& name) -> bool {
            if (!m_inspectorTabs) {
                return false;
            }
            const int index = m_inspectorTabs->currentIndex();
            return index >= 0 && m_inspectorTabs->tabText(index).compare(name, Qt::CaseInsensitive) == 0;
        };
        int targetInspectorTab = qBound(0, selectedInspectorTab, m_inspectorTabs->count() - 1);
        if (!selectedInspectorTabLabel.isEmpty()) {
            for (int i = 0; i < m_inspectorTabs->count(); ++i) {
                if (m_inspectorTabs->tabText(i).compare(selectedInspectorTabLabel, Qt::CaseInsensitive) == 0) {
                    targetInspectorTab = i;
                    break;
                }
            }
        }
        QSignalBlocker block(m_inspectorTabs);
        m_inspectorTabs->setCurrentIndex(targetInspectorTab);
        if (m_preview) {
            const bool showCorrectionOverlays = isTabNamed(QStringLiteral("Corrections"));
            const bool transcriptOverlayInteractive = isTabNamed(QStringLiteral("Transcript"));
            const bool titleOverlayOnly = isTabNamed(QStringLiteral("Titles"));
            const bool faceDetectionsAssignmentInteractive = isTabNamed(QStringLiteral("Speakers"));
            m_preview->setShowCorrectionOverlays(showCorrectionOverlays);
            m_preview->setTranscriptOverlayInteractionEnabled(transcriptOverlayInteractive);
            m_preview->setTitleOverlayInteractionOnly(titleOverlayOnly);
            m_preview->setFaceDetectionsAssignmentInteractionEnabled(faceDetectionsAssignmentInteractive);
            if (!showCorrectionOverlays && m_correctionsTab) {
                m_correctionsTab->stopDrawing();
            }
        }
    }
    applyPlaybackRuntimeConfig(PlaybackRuntimeConfig{
        playbackSpeed,
        effectivePlaybackClockSource,
        playbackAudioWarpMode,
        playbackLoopEnabled});
    
    if (m_preview) {
        m_preview->setOutputSize(QSize(outputWidth, outputHeight));
        m_preview->setHideOutsideOutputWindow(previewHideOutsideOutput);
        m_preview->setBackgroundFillEffect(backgroundFillEffect);
        m_preview->setBackgroundFillOpacity(backgroundFillOpacity);
        m_preview->setBackgroundFillBrightness(backgroundFillBrightness);
        m_preview->setBackgroundFillSaturation(backgroundFillSaturation);
        m_preview->setBackgroundFillEdgePixels(1);
        m_preview->setBackgroundFillEdgeProgressive(false);
        m_preview->setBackgroundFillEdgePower(2.0);
        m_preview->setBackgroundFillStretchSourceClipId(QString());
        m_preview->setShowSpeakerTrackPoints(previewShowSpeakerTrackPoints);
        m_preview->setShowSpeakerTrackBoxes(previewShowSpeakerTrackBoxes);
        m_preview->setShowRawDetections(previewShowRawDetections);
        // Legacy current-speaker painting is retired. Transcript-linked title
        // clips are the sole speaker-introduction rendering path.
        m_preview->setShowCurrentSpeakerName(false);
        m_preview->setShowCurrentSpeakerOrganization(false);
        m_preview->setCurrentSpeakerNameTextScale(previewCurrentSpeakerNameTextScalePercent / 100.0);
        m_preview->setCurrentSpeakerOrganizationTextScale(
            previewCurrentSpeakerOrganizationTextScalePercent / 100.0);
        m_preview->setCurrentSpeakerNameVerticalPosition(
            previewCurrentSpeakerNameYPositionPercent / 100.0);
        m_preview->setCurrentSpeakerOrganizationVerticalPosition(
            previewCurrentSpeakerOrganizationYPositionPercent / 100.0);
        m_preview->setCurrentSpeakerNameColor(m_speakerCurrentSpeakerNameColor);
        m_preview->setCurrentSpeakerOrganizationColor(m_speakerCurrentSpeakerOrganizationColor);
        QColor effectiveSpeakerBackground = m_speakerCurrentSpeakerBackgroundColor;
        QColor effectiveSpeakerBorder = m_speakerCurrentSpeakerBorderColor;
        if (!m_speakerCurrentSpeakerBackgroundVisible) {
            effectiveSpeakerBackground.setAlpha(0);
            effectiveSpeakerBorder.setAlpha(0);
        }
        m_preview->setCurrentSpeakerBackgroundColor(effectiveSpeakerBackground);
        m_preview->setCurrentSpeakerBorderColor(effectiveSpeakerBorder);
        m_preview->setCurrentSpeakerBackgroundCornerRadius(previewCurrentSpeakerBackgroundRadiusPx);
        m_preview->setCurrentSpeakerBorderWidth(previewCurrentSpeakerBorderWidthPx);
        m_preview->setCurrentSpeakerShadowEnabled(previewCurrentSpeakerShadowEnabled);
        m_preview->setCurrentSpeakerShadowColor(m_speakerCurrentSpeakerShadowColor);
        m_preview->setFacestreamOverlaySource(previewFacestreamOverlaySource);
        // Grading preview is owned by each timeline track. Keep the legacy
        // runtime-wide bypass disabled so one track cannot suppress another.
        m_preview->setBypassGrading(false);
        m_previewAudioDynamics = loadedAudioDynamics;
        m_preview->setAudioDynamicsSettings(m_previewAudioDynamics);
    }
    applyPreviewViewMode(m_featureAudioPreviewMode ? previewViewMode : QStringLiteral("video"));
    if (m_previewModeCombo) {
        QSignalBlocker block(m_previewModeCombo);
        const int modeIndex =
            m_previewModeCombo->findData(m_previewViewMode, Qt::MatchFixedString);
        if (modeIndex >= 0) {
            m_previewModeCombo->setCurrentIndex(modeIndex);
        }
        m_previewModeCombo->setEnabled(m_featureAudioPreviewMode);
        m_previewModeCombo->setToolTip(m_featureAudioPreviewMode
                                           ? QStringLiteral("Switch preview between video composition and audio waveform view.")
                                           : QStringLiteral("Audio preview mode disabled by feature flag."));
    }
    if (m_audioToolsButton) {
        m_audioToolsButton->setEnabled(m_featureAudioDynamicsTools);
        m_audioToolsButton->setToolTip(m_featureAudioDynamicsTools
                                           ? QStringLiteral("Open the Audio tab.")
                                           : QStringLiteral("Audio dynamics tools disabled by feature flag."));
    }
    if (m_audioAmplifyEnabledCheckBox) {
        QSignalBlocker block(m_audioAmplifyEnabledCheckBox);
        m_audioAmplifyEnabledCheckBox->setChecked(m_previewAudioDynamics.amplifyEnabled);
        m_audioAmplifyEnabledCheckBox->setEnabled(m_featureAudioDynamicsTools);
    }
    if (m_audioAmplifyDbSpin) {
        QSignalBlocker block(m_audioAmplifyDbSpin);
        m_audioAmplifyDbSpin->setValue(m_previewAudioDynamics.amplifyDb);
        m_audioAmplifyDbSpin->setEnabled(m_featureAudioDynamicsTools);
    }
    m_audioSpeakerHoverModalEnabled = audioSpeakerHoverModalEnabled;
    m_audioWaveformVisible = audioWaveformVisible;
    m_audioVisualizationMode = audioVisualizationMode;
    m_loiaconoSpectrumSettings = loiaconoSpectrumSettings;
    if (m_audioSpeakerHoverModalCheckBox) {
        QSignalBlocker block(m_audioSpeakerHoverModalCheckBox);
        m_audioSpeakerHoverModalCheckBox->setChecked(m_audioSpeakerHoverModalEnabled);
    }
    if (m_audioShowWaveformCheckBox) {
        QSignalBlocker block(m_audioShowWaveformCheckBox);
        m_audioShowWaveformCheckBox->setChecked(m_audioWaveformVisible);
    }
    if (m_audioVisualizationModeCombo) {
        QSignalBlocker block(m_audioVisualizationModeCombo);
        const int visualizationIndex = m_audioVisualizationModeCombo->findData(
            static_cast<int>(m_audioVisualizationMode));
        if (visualizationIndex >= 0) {
            m_audioVisualizationModeCombo->setCurrentIndex(visualizationIndex);
        }
    }
    if (m_loiaconoSpectrumSettingsButton) {
        m_loiaconoSpectrumSettingsButton->setEnabled(
            m_audioVisualizationMode == PreviewSurface::AudioVisualizationMode::Spectrum);
    }
    if (m_loiaconoSpectrumSettingsDialog) {
        m_loiaconoSpectrumSettingsDialog->setSettings(m_loiaconoSpectrumSettings);
    }
    if (m_audioNormalizeEnabledCheckBox) {
        QSignalBlocker block(m_audioNormalizeEnabledCheckBox);
        m_audioNormalizeEnabledCheckBox->setChecked(m_previewAudioDynamics.normalizeEnabled);
        m_audioNormalizeEnabledCheckBox->setEnabled(m_featureAudioDynamicsTools);
    }
    if (m_audioNormalizeTargetDbSpin) {
        QSignalBlocker block(m_audioNormalizeTargetDbSpin);
        m_audioNormalizeTargetDbSpin->setValue(m_previewAudioDynamics.normalizeTargetDb);
        m_audioNormalizeTargetDbSpin->setEnabled(m_featureAudioDynamicsTools);
    }
    if (m_audioStereoToMonoCheckBox) {
        QSignalBlocker block(m_audioStereoToMonoCheckBox);
        m_audioStereoToMonoCheckBox->setChecked(m_previewAudioDynamics.stereoToMonoEnabled);
        m_audioStereoToMonoCheckBox->setEnabled(m_featureAudioDynamicsTools);
    }
    if (m_audioSelectiveNormalizeEnabledCheckBox) {
        QSignalBlocker block(m_audioSelectiveNormalizeEnabledCheckBox);
        m_audioSelectiveNormalizeEnabledCheckBox->setChecked(
            m_previewAudioDynamics.selectiveNormalizeEnabled);
        m_audioSelectiveNormalizeEnabledCheckBox->setEnabled(m_featureAudioDynamicsTools);
    }
    if (m_audioSelectiveNormalizeMinSecondsSpin) {
        QSignalBlocker block(m_audioSelectiveNormalizeMinSecondsSpin);
        m_audioSelectiveNormalizeMinSecondsSpin->setValue(
            m_previewAudioDynamics.selectiveNormalizeMinSegmentSeconds);
        m_audioSelectiveNormalizeMinSecondsSpin->setEnabled(m_featureAudioDynamicsTools);
    }
    if (m_audioSelectiveNormalizePeakDbSpin) {
        QSignalBlocker block(m_audioSelectiveNormalizePeakDbSpin);
        m_audioSelectiveNormalizePeakDbSpin->setValue(
            m_previewAudioDynamics.selectiveNormalizePeakDb);
        m_audioSelectiveNormalizePeakDbSpin->setEnabled(m_featureAudioDynamicsTools);
    }
    if (m_audioSelectiveNormalizePassesSpin) {
        QSignalBlocker block(m_audioSelectiveNormalizePassesSpin);
        m_audioSelectiveNormalizePassesSpin->setValue(
            m_previewAudioDynamics.selectiveNormalizePasses);
        m_audioSelectiveNormalizePassesSpin->setEnabled(m_featureAudioDynamicsTools);
    }
    if (m_audioSelectiveNormalizeOverlayVisibleCheckBox) {
        QSignalBlocker block(m_audioSelectiveNormalizeOverlayVisibleCheckBox);
        m_audioSelectiveNormalizeOverlayVisibleCheckBox->setChecked(
            m_previewAudioDynamics.selectiveNormalizeOverlayVisible);
        m_audioSelectiveNormalizeOverlayVisibleCheckBox->setEnabled(m_featureAudioDynamicsTools);
    }
    if (m_audioTranscriptNormalizeEnabledCheckBox) {
        QSignalBlocker block(m_audioTranscriptNormalizeEnabledCheckBox);
        m_audioTranscriptNormalizeEnabledCheckBox->setChecked(
            m_previewAudioDynamics.transcriptNormalizeEnabled);
        m_audioTranscriptNormalizeEnabledCheckBox->setEnabled(m_featureAudioDynamicsTools);
    }
    if (m_audioWaveformPreviewProcessedCheckBox) {
        QSignalBlocker block(m_audioWaveformPreviewProcessedCheckBox);
        m_audioWaveformPreviewProcessedCheckBox->setChecked(
            m_previewAudioDynamics.waveformPreviewPostProcessing);
        m_audioWaveformPreviewProcessedCheckBox->setEnabled(m_featureAudioDynamicsTools);
    }
    if (m_audioPeakReductionEnabledCheckBox) {
        QSignalBlocker block(m_audioPeakReductionEnabledCheckBox);
        m_audioPeakReductionEnabledCheckBox->setChecked(m_previewAudioDynamics.peakReductionEnabled);
        m_audioPeakReductionEnabledCheckBox->setEnabled(m_featureAudioDynamicsTools);
    }
    if (m_audioPeakThresholdDbSpin) {
        QSignalBlocker block(m_audioPeakThresholdDbSpin);
        m_audioPeakThresholdDbSpin->setValue(m_previewAudioDynamics.peakThresholdDb);
        m_audioPeakThresholdDbSpin->setEnabled(m_featureAudioDynamicsTools);
    }
    if (m_audioLimiterEnabledCheckBox) {
        QSignalBlocker block(m_audioLimiterEnabledCheckBox);
        m_audioLimiterEnabledCheckBox->setChecked(m_previewAudioDynamics.limiterEnabled);
        m_audioLimiterEnabledCheckBox->setEnabled(m_featureAudioDynamicsTools);
    }
    if (m_audioLimiterThresholdDbSpin) {
        QSignalBlocker block(m_audioLimiterThresholdDbSpin);
        m_audioLimiterThresholdDbSpin->setValue(m_previewAudioDynamics.limiterThresholdDb);
        m_audioLimiterThresholdDbSpin->setEnabled(m_featureAudioDynamicsTools);
    }
    if (m_audioCompressorEnabledCheckBox) {
        QSignalBlocker block(m_audioCompressorEnabledCheckBox);
        m_audioCompressorEnabledCheckBox->setChecked(m_previewAudioDynamics.compressorEnabled);
        m_audioCompressorEnabledCheckBox->setEnabled(m_featureAudioDynamicsTools);
    }
    if (m_audioCompressorThresholdDbSpin) {
        QSignalBlocker block(m_audioCompressorThresholdDbSpin);
        m_audioCompressorThresholdDbSpin->setValue(m_previewAudioDynamics.compressorThresholdDb);
        m_audioCompressorThresholdDbSpin->setEnabled(m_featureAudioDynamicsTools);
    }
    if (m_audioCompressorRatioSpin) {
        QSignalBlocker block(m_audioCompressorRatioSpin);
        m_audioCompressorRatioSpin->setValue(m_previewAudioDynamics.compressorRatio);
        m_audioCompressorRatioSpin->setEnabled(m_featureAudioDynamicsTools);
    }
    if (m_audioSoftClipEnabledCheckBox) {
        QSignalBlocker block(m_audioSoftClipEnabledCheckBox);
        m_audioSoftClipEnabledCheckBox->setChecked(m_previewAudioDynamics.softClipEnabled);
        m_audioSoftClipEnabledCheckBox->setEnabled(m_featureAudioDynamicsTools);
    }
    if (m_preview) {
        m_preview->setAudioSpeakerHoverModalEnabled(m_audioSpeakerHoverModalEnabled);
        m_preview->setAudioWaveformVisible(m_audioWaveformVisible);
        m_preview->setAudioVisualizationMode(m_audioVisualizationMode);
        m_preview->setLoiaconoSpectrumSettings(m_loiaconoSpectrumSettings);
    }
    editor::setDebugPlaybackCacheFallbackEnabled(previewPlaybackCacheFallback);
    editor::setDebugLeadPrefetchEnabled(previewLeadPrefetchEnabled);
    editor::setDebugLeadPrefetchCount(previewLeadPrefetchCount);
    editor::setDebugPlaybackWindowAhead(previewPlaybackWindowAhead);
    editor::setDebugVisibleQueueReserve(previewVisibleQueueReserve);
    editor::setDebugPrefetchMaxQueueDepth(debugPrefetchMaxQueueDepth);
    editor::setDebugPrefetchMaxInflight(debugPrefetchMaxInflight);
    editor::setDebugPrefetchMaxPerTick(debugPrefetchMaxPerTick);
    editor::setDebugPrefetchSkipVisiblePendingThreshold(debugPrefetchSkipVisiblePendingThreshold);
    editor::setDebugDecoderLaneCount(debugDecoderLaneCount);
    editor::setDebugDecodePreference(debugDecodePreference);
    editor::setDebugH26xSoftwareThreadingMode(debugH26xSoftwareThreadingMode);
    editor::setRubberBandEnginePreference(rubberBandEnginePreference);
    editor::setRubberBandThreadingPreference(rubberBandThreadingPreference);
    editor::setRubberBandWindowPreference(rubberBandWindowPreference);
    editor::setRubberBandPitchPreference(rubberBandPitchPreference);
    editor::setRubberBandChannelsTogether(rubberBandChannelsTogether);
    editor::setDebugDeterministicPipelineEnabled(debugDeterministicPipeline);
    editor::setDebugTimelineAudioEnvelopeGranularity(timelineAudioEnvelopeGranularity);
    if (m_timeline) {
        m_timeline->update();
    }

    markStartup(QStringLiteral("apply_state.timeline_bind.begin"));
    const auto savedTimelineClipsChanged = m_timeline->clipsChanged;
    const auto savedTimelineSelectionChanged = m_timeline->selectionChanged;
    const auto savedTimelineTrackLayoutChanged = m_timeline->trackLayoutChanged;
    const auto savedTimelineRenderSyncMarkersChanged = m_timeline->renderSyncMarkersChanged;
    const auto savedTimelineExportRangeChanged = m_timeline->exportRangeChanged;
    m_timeline->clipsChanged = nullptr;
    m_timeline->selectionChanged = nullptr;
    m_timeline->trackLayoutChanged = nullptr;
    m_timeline->renderSyncMarkersChanged = nullptr;
    m_timeline->exportRangeChanged = nullptr;
    m_timeline->setTracks(loadedTracks);
    m_timeline->setClips(loadedClips);
    m_timeline->setTimelineZoom(timelineZoom);
    m_timeline->setVerticalScrollOffset(timelineVerticalScroll);

    if (!loadedExportRanges.isEmpty()) {
        m_timeline->setExportRanges(loadedExportRanges);
    }

    m_timeline->setRenderSyncMarkers(loadedRenderSyncMarkers);
    m_timeline->setSelectedClipId(selectedClipId);
    syncSliderRange();
    m_timeline->clipsChanged = savedTimelineClipsChanged;
    m_timeline->selectionChanged = savedTimelineSelectionChanged;
    m_timeline->trackLayoutChanged = savedTimelineTrackLayoutChanged;
    m_timeline->renderSyncMarkersChanged = savedTimelineRenderSyncMarkersChanged;
    m_timeline->exportRangeChanged = savedTimelineExportRangeChanged;
    if (m_timeline->trackLayoutChanged) {
        m_timeline->trackLayoutChanged();
    }
    markStartup(QStringLiteral("apply_state.timeline_bind.end"),
                QJsonObject{
                    {QStringLiteral("timeline_clip_count"), m_timeline->clips().size()},
                    {QStringLiteral("timeline_track_count"), m_timeline->tracks().size()}
                });

    const QVector<ExportRangeSegment> playbackRanges =
        startupMarking
            ? m_timeline->exportRanges()
            : effectivePlaybackRanges();
    if (!startupMarking) {
        markStartup(QStringLiteral("apply_state.preview_bind.begin"));
        markStartup(QStringLiteral("apply_state.audio_bind.begin"));
        bindTimelineMediaState(selectedClipId, playbackRanges, currentFrame, false);
        markStartup(QStringLiteral("apply_state.preview_bind.end"));
        if (m_audioEngine) {
            markStartup(QStringLiteral("apply_state.audio_bind.end"));
        }
    } else {
        markStartup(QStringLiteral("apply_state.preview_bind.deferred"));
        if (m_audioEngine) {
            markStartup(QStringLiteral("apply_state.audio_bind.deferred"));
        }
    }
    
    markStartup(QStringLiteral("apply_state.seek.begin"),
                QJsonObject{{QStringLiteral("target_frame"), static_cast<qint64>(currentFrame)}});
    if (!startupMarking) {
        setCurrentFrame(currentFrame);
        if (m_audioEngine) {
            m_audioEngine->seek(currentFrame);
        }
        markStartup(QStringLiteral("apply_state.seek.end"));
    } else {
        markStartup(QStringLiteral("apply_state.seek.deferred"));
    }

    m_playbackTimer.stop();
    m_fastPlaybackActive.store(false);
    m_preview->setPlaybackState(false);
    if (m_audioEngine) {
        m_audioEngine->stop();
    }
    updateTransportLabels();

    m_loadingState = false;
    refreshAiIntegrationState();
    markStartup(QStringLiteral("apply_state.end"));
    
    // Media browsing is project state and is independent from the directory
    // where ProjectManager stores projects.  Preserve the user's saved media
    // root when restoring the project.
    const QString mediaRoot = resolvedRootPath;
    
    QTimer::singleShot(0, this, [this, mediaRoot, currentFrame, startupMarking, selectedClipId, playbackRanges, root, expandedExplorerPaths]() {
        if (m_explorerPane) {
            m_explorerPane->setInitialRootPath(mediaRoot);
        }
        if (!startupMarking) {
            refreshCurrentInspectorTab();
        }
        if (startupMarking) {
            applyDeferredStartupPanelState(root, expandedExplorerPaths);
            QTimer::singleShot(0, this, [this, mediaRoot]() {
                if (!m_timeline) {
                    return;
                }
                QVector<TimelineClip> clips = m_timeline->clips();
                if (!reconcileMissingMediaForClips(&clips, mediaRoot)) {
                    return;
                }
                m_loadingState = true;
                m_timeline->setClips(clips);
                m_loadingState = false;
                scheduleSaveState();
            });
            QTimer::singleShot(0, this, [this, currentFrame, selectedClipId]() {
                const QVector<ExportRangeSegment> deferredPlaybackRanges = effectivePlaybackRanges();
                bindTimelineMediaState(selectedClipId, deferredPlaybackRanges, currentFrame, true);
            });
        }
    });
}
