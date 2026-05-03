#include "editor.h"
#include "keyframe_table_shared.h"
#include "clip_serialization.h"
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

#include <cmath>
#include <cstring>

using namespace editor;

#include "playback_debug.h"

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
}

// ============================================================================
// EditorWindow - Main application window
// ============================================================================
EditorWindow::EditorWindow(quint16 controlPort)
{
    QElapsedTimer ctorTimer;
    ctorTimer.start();
    m_startupProfileTimer.start();
    m_startupProfileLastMarkMs = 0;
    startupProfileMark(QStringLiteral("ctor.begin"),
                       QJsonObject{{QStringLiteral("control_port"), static_cast<int>(controlPort)}});

    setupWindowChrome();
    startupProfileMark(QStringLiteral("setup.window_chrome.done"));
    setupMainLayout(ctorTimer);
    startupProfileMark(QStringLiteral("setup.main_layout.done"));
    bindInspectorWidgets();
    startupProfileMark(QStringLiteral("setup.bind_inspector_widgets.done"));

    setupPlaybackTimers();
    startupProfileMark(QStringLiteral("setup.playback_timers.done"));
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
    saveStateNow();
}

void EditorWindow::closeEvent(QCloseEvent *event)
{
    m_playbackTimer.stop();
    m_fastPlaybackActive.store(false);
    if (m_preview) {
        m_preview->setPlaybackState(false);
    }
    if (m_audioEngine) {
        m_audioEngine->stop();
    }
    saveStateNow();
    QMainWindow::closeEvent(event);
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
    const qreal timelineFramePosition = samplesToFramePosition(m_absolutePlaybackSample);
    const qreal clipStart = static_cast<qreal>(clip->startFrame);
    const qreal clipEnd = static_cast<qreal>(clip->startFrame + qMax<int64_t>(0, clip->durationFrames - 1));
    if (timelineFramePosition < clipStart || timelineFramePosition > clipEnd) {
        m_transcriptTable->clearSelection();
        return;
    }

    const int64_t sourceSample = sourceSampleForClipAtTimelineSample(
        *clip,
        m_absolutePlaybackSample,
        m_timeline->renderSyncMarkers());
    const double sourceSeconds = static_cast<double>(sourceSample) / static_cast<double>(kAudioSampleRate);
    const int64_t sourceFrame = transcriptFrameForClipAtTimelineSample(
        *clip,
        m_absolutePlaybackSample,
        m_timeline->renderSyncMarkers());
    if (m_transcriptTab) {
        m_transcriptTab->syncTableToPlayhead(m_absolutePlaybackSample, sourceSeconds, sourceFrame);
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
    applyStateJson(m_historyEntries.at(m_historyIndex).toObject());
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
    applyStateJson(m_historyEntries.at(m_historyIndex).toObject());
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
    applyStateJson(m_historyEntries.at(m_historyIndex).toObject());
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
    QString rootPath = root.value(QStringLiteral("mediaRoot")).toString(rootDirPath());
    if (rootPath.isEmpty()) {
        rootPath = root.value(QStringLiteral("explorerRoot")).toString(rootDirPath());
    }
    if (rootPath.isEmpty() || !QDir(rootPath).exists()) {
        rootPath = QDir::currentPath();
    }
    QString galleryFolderPath = root.value(QStringLiteral("mediaGalleryPath")).toString();
    if (galleryFolderPath.isEmpty()) {
        galleryFolderPath = root.value(QStringLiteral("explorerGalleryPath")).toString();
    }
    const int outputWidth = qMax(16, root.value(QStringLiteral("outputWidth")).toInt(1080));
    const int outputHeight = qMax(16, root.value(QStringLiteral("outputHeight")).toInt(1920));
    const QString outputFormat = root.value(QStringLiteral("outputFormat")).toString(QStringLiteral("mp4"));
    const QString lastRenderOutputPath = root.value(QStringLiteral("lastRenderOutputPath")).toString();
    const bool renderUseProxies = root.value(QStringLiteral("renderUseProxies")).toBool(false);
    const bool previewHideOutsideOutput = root.value(QStringLiteral("previewHideOutsideOutput")).toBool(false);
    const bool previewShowSpeakerTrackPoints =
        root.value(QStringLiteral("previewShowSpeakerTrackPoints")).toBool(false);
    const bool previewShowSpeakerTrackBoxes =
        root.value(QStringLiteral("previewShowSpeakerTrackBoxes")).toBool(false);
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
    editor::DecodePreference debugDecodePreference = editor::debugDecodePreference();
    const QString debugDecodeModeText =
        root.value(QStringLiteral("debugDecodeMode"))
            .toString(editor::decodePreferenceToString(editor::debugDecodePreference()));
    editor::parseDecodePreference(debugDecodeModeText, &debugDecodePreference);
    editor::H26xSoftwareThreadingMode debugH26xSoftwareThreadingMode =
        editor::debugH26xSoftwareThreadingMode();
    const QString debugH26xSoftwareThreadingModeText =
        root.value(QStringLiteral("debugH26xSoftwareThreadingMode"))
            .toString(editor::h26xSoftwareThreadingModeToString(editor::debugH26xSoftwareThreadingMode()));
    editor::parseH26xSoftwareThreadingMode(debugH26xSoftwareThreadingModeText,
                                           &debugH26xSoftwareThreadingMode);
    const bool debugDeterministicPipeline =
        root.value(QStringLiteral("debugDeterministicPipeline"))
            .toBool(editor::debugDeterministicPipelineEnabled());
    const bool speechFilterEnabled = root.value(QStringLiteral("speechFilterEnabled")).toBool(false);
    const int transcriptPrependMs = root.value(QStringLiteral("transcriptPrependMs")).toInt(150);
    const int transcriptPostpendMs = root.value(QStringLiteral("transcriptPostpendMs")).toInt(70);
    const int speechFilterFadeSamples = root.value(QStringLiteral("speechFilterFadeSamples")).toInt(300);
    const bool speechFilterRangeCrossfade =
        root.value(QStringLiteral("speechFilterRangeCrossfade")).toBool(false);
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
    const PlaybackClockSource playbackClockSource = playbackClockSourceFromString(
        root.value(QStringLiteral("playbackClockSource"))
            .toString(playbackClockSourceToString(PlaybackClockSource::Auto)));
    const PlaybackAudioWarpMode playbackAudioWarpMode = playbackAudioWarpModeFromString(
        root.value(QStringLiteral("playbackAudioWarpMode"))
            .toString(playbackAudioWarpModeToString(PlaybackAudioWarpMode::Disabled)));
    const bool playbackLoopEnabled =
        root.value(QStringLiteral("playbackLoopEnabled")).toBool(false);
    const qreal timelineZoom = root.value(QStringLiteral("timelineZoom")).toDouble(4.0);
    const int timelineVerticalScroll = root.value(QStringLiteral("timelineVerticalScroll")).toInt(0);
    const int64_t exportStartFrame = root.value(QStringLiteral("exportStartFrame")).toVariant().toLongLong();
    const int64_t exportEndFrame = root.value(QStringLiteral("exportEndFrame")).toVariant().toLongLong();
    const QString previewViewMode = root.value(QStringLiteral("previewViewMode")).toString(QStringLiteral("video"));
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
    PreviewWindow::AudioDynamicsSettings loadedAudioDynamics;
    loadedAudioDynamics.amplifyEnabled = root.value(QStringLiteral("audioAmplifyEnabled")).toBool(false);
    loadedAudioDynamics.amplifyDb = root.value(QStringLiteral("audioAmplifyDb")).toDouble(0.0);
    loadedAudioDynamics.normalizeEnabled = root.value(QStringLiteral("audioNormalizeEnabled")).toBool(false);
    loadedAudioDynamics.normalizeTargetDb = root.value(QStringLiteral("audioNormalizeTargetDb")).toDouble(-1.0);
    loadedAudioDynamics.peakReductionEnabled = root.value(QStringLiteral("audioPeakReductionEnabled")).toBool(false);
    loadedAudioDynamics.peakThresholdDb = root.value(QStringLiteral("audioPeakThresholdDb")).toDouble(-6.0);
    loadedAudioDynamics.limiterEnabled = root.value(QStringLiteral("audioLimiterEnabled")).toBool(false);
    loadedAudioDynamics.limiterThresholdDb = root.value(QStringLiteral("audioLimiterThresholdDb")).toDouble(-1.0);
    loadedAudioDynamics.compressorEnabled = root.value(QStringLiteral("audioCompressorEnabled")).toBool(false);
    loadedAudioDynamics.compressorThresholdDb = root.value(QStringLiteral("audioCompressorThresholdDb")).toDouble(-18.0);
    loadedAudioDynamics.compressorRatio = root.value(QStringLiteral("audioCompressorRatio")).toDouble(3.0);
    const bool audioSpeakerHoverModalEnabled =
        root.value(QStringLiteral("audioSpeakerHoverModalEnabled")).toBool(true);
    const bool audioWaveformVisible =
        root.value(QStringLiteral("audioWaveformVisible")).toBool(true);
    
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
        TimelineClip clip = clipFromJson(value.toObject());
        if (clip.trackIndex < 0) clip.trackIndex = loadedClips.size();
        if (!clip.filePath.isEmpty() || clip.mediaType == ClipMediaType::Title)
            loadedClips.push_back(clip);
    }
    markStartup(QStringLiteral("apply_state.timeline_parse.end"),
                QJsonObject{{QStringLiteral("loaded_clip_count"), loadedClips.size()}});

    if (!m_restoringHistory && !loadedClips.isEmpty()) {
        QElapsedTimer relocateTimer;
        relocateTimer.start();
        markStartup(QStringLiteral("apply_state.media_relocate.begin"),
                    QJsonObject{{QStringLiteral("candidate_clip_count"), loadedClips.size()}});
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

        for (TimelineClip& clip : loadedClips) {
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
        markStartup(QStringLiteral("apply_state.media_relocate.end"),
                    QJsonObject{
                        {QStringLiteral("elapsed_ms"), relocateTimer.elapsed()},
                        {QStringLiteral("relocated_count"), relocatedCount},
                        {QStringLiteral("unresolved_count"), unresolvedCount}
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
        track.height = qMax(28, obj.value(QStringLiteral("height")).toInt(44));
        if (obj.contains(QStringLiteral("visualMode"))) {
            track.visualMode = trackVisualModeFromString(obj.value(QStringLiteral("visualMode")).toString());
        } else if (obj.contains(QStringLiteral("visualEnabled")) &&
                   !obj.value(QStringLiteral("visualEnabled")).toBool(true)) {
            track.visualMode = TrackVisualMode::Hidden;
        }
        track.audioEnabled = obj.value(QStringLiteral("audioEnabled")).toBool(true);
        loadedTracks.push_back(track);
    }
    markStartup(QStringLiteral("apply_state.tracks_parse.end"),
                QJsonObject{{QStringLiteral("loaded_track_count"), loadedTracks.size()}});
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
        m_explorerPane->restoreExpandedExplorerPaths(expandedExplorerPaths);
    }
    
    if (m_outputWidthSpin) { QSignalBlocker block(m_outputWidthSpin); m_outputWidthSpin->setValue(outputWidth); }
    if (m_outputHeightSpin) { QSignalBlocker block(m_outputHeightSpin); m_outputHeightSpin->setValue(outputHeight); }
    if (m_outputFormatCombo) {
        QSignalBlocker block(m_outputFormatCombo);
        const int formatIndex = m_outputFormatCombo->findData(outputFormat);
        if (formatIndex >= 0) m_outputFormatCombo->setCurrentIndex(formatIndex);
    }
    m_lastRenderOutputPath = lastRenderOutputPath;
    m_aiSelectedModel = aiSelectedModel;
    m_aiProxyBaseUrl = aiProxyBaseUrl.trimmed();
    m_aiAuthToken = aiAuthToken.trimmed();
    if (!m_aiAuthToken.isEmpty()) {
        QString secureStoreError;
        if (writeAiTokenToSecureStore(m_aiAuthToken, &secureStoreError)) {
            m_aiAuthToken.clear();
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
    m_autosaveIntervalMinutes = autosaveIntervalMinutes;
    m_autosaveMaxBackups = autosaveMaxBackups;
    if (m_autosaveIntervalMinutesSpin) {
        QSignalBlocker block(m_autosaveIntervalMinutesSpin);
        m_autosaveIntervalMinutesSpin->setValue(m_autosaveIntervalMinutes);
    }
    if (m_autosaveMaxBackupsSpin) {
        QSignalBlocker block(m_autosaveMaxBackupsSpin);
        m_autosaveMaxBackupsSpin->setValue(m_autosaveMaxBackups);
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
    if (m_speakerShowBoxStreamBoxesCheckBox) {
        QSignalBlocker block(m_speakerShowBoxStreamBoxesCheckBox);
        m_speakerShowBoxStreamBoxesCheckBox->setChecked(previewShowSpeakerTrackBoxes);
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
        const int decodeModeIndex =
            m_outputDecodeModeCombo->findData(editor::decodePreferenceToString(debugDecodePreference));
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
    if (m_speechFilterEnabledCheckBox) { QSignalBlocker block(m_speechFilterEnabledCheckBox); m_speechFilterEnabledCheckBox->setChecked(speechFilterEnabled); }
    
    m_transcriptPrependMs = transcriptPrependMs;
    m_transcriptPostpendMs = transcriptPostpendMs;
    m_speechFilterFadeSamples = qMax(0, speechFilterFadeSamples);
    m_speechFilterRangeCrossfade = speechFilterRangeCrossfade;
    
    if (m_transcriptPrependMsSpin) { QSignalBlocker block(m_transcriptPrependMsSpin); m_transcriptPrependMsSpin->setValue(m_transcriptPrependMs); }
    if (m_transcriptPostpendMsSpin) { QSignalBlocker block(m_transcriptPostpendMsSpin); m_transcriptPostpendMsSpin->setValue(m_transcriptPostpendMs); }
    if (m_speechFilterFadeSamplesSpin) { QSignalBlocker block(m_speechFilterFadeSamplesSpin); m_speechFilterFadeSamplesSpin->setValue(m_speechFilterFadeSamples); }
    if (m_speechFilterRangeCrossfadeCheckBox) {
        QSignalBlocker block(m_speechFilterRangeCrossfadeCheckBox);
        m_speechFilterRangeCrossfadeCheckBox->setChecked(m_speechFilterRangeCrossfade);
    }
    if (m_inspectorPane && m_inspectorPane->transcriptUnifiedEditModeCheckBox()) {
        QSignalBlocker block(m_inspectorPane->transcriptUnifiedEditModeCheckBox());
        m_inspectorPane->transcriptUnifiedEditModeCheckBox()->setChecked(transcriptUnifiedEditColors);
    }
    if (m_inspectorPane && m_inspectorPane->transcriptShowExcludedLinesCheckBox()) {
        QSignalBlocker block(m_inspectorPane->transcriptShowExcludedLinesCheckBox());
        m_inspectorPane->transcriptShowExcludedLinesCheckBox()->setChecked(transcriptShowExcludedLines);
    }
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
    
    if (m_transcriptFollowCurrentWordCheckBox) { QSignalBlocker block(m_transcriptFollowCurrentWordCheckBox); m_transcriptFollowCurrentWordCheckBox->setChecked(transcriptFollowCurrentWord); }
    if (m_gradingFollowCurrentCheckBox) { QSignalBlocker block(m_gradingFollowCurrentCheckBox); m_gradingFollowCurrentCheckBox->setChecked(gradingFollowCurrent); }
    if (m_gradingAutoScrollCheckBox) { QSignalBlocker block(m_gradingAutoScrollCheckBox); m_gradingAutoScrollCheckBox->setChecked(gradingAutoScroll); }
    if (m_bypassGradingCheckBox) { QSignalBlocker block(m_bypassGradingCheckBox); m_bypassGradingCheckBox->setChecked(gradingPreview); }
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
            m_preview->setShowCorrectionOverlays(showCorrectionOverlays);
            m_preview->setTranscriptOverlayInteractionEnabled(transcriptOverlayInteractive);
            m_preview->setTitleOverlayInteractionOnly(titleOverlayOnly);
            if (!showCorrectionOverlays && m_correctionsTab) {
                m_correctionsTab->stopDrawing();
            }
        }
    }
    applyPlaybackRuntimeConfig(
        PlaybackRuntimeConfig{playbackSpeed, playbackClockSource, playbackAudioWarpMode, playbackLoopEnabled});
    
    if (m_preview) {
        m_preview->setOutputSize(QSize(outputWidth, outputHeight));
        m_preview->setHideOutsideOutputWindow(previewHideOutsideOutput);
        m_preview->setShowSpeakerTrackPoints(previewShowSpeakerTrackPoints);
        m_preview->setShowSpeakerTrackBoxes(previewShowSpeakerTrackBoxes);
        m_preview->setBypassGrading(!gradingPreview);
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
    if (m_inspectorTabs) {
        const int index = m_inspectorTabs->currentIndex();
        if (index >= 0 &&
            m_inspectorTabs->tabText(index).compare(QStringLiteral("Audio"), Qt::CaseInsensitive) == 0) {
            applyPreviewViewMode(QStringLiteral("audio"));
            if (m_previewModeCombo) {
                QSignalBlocker block(m_previewModeCombo);
                const int audioIndex =
                    m_previewModeCombo->findData(QStringLiteral("audio"), Qt::MatchFixedString);
                if (audioIndex >= 0) {
                    m_previewModeCombo->setCurrentIndex(audioIndex);
                }
            }
        }
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
    if (m_audioSpeakerHoverModalCheckBox) {
        QSignalBlocker block(m_audioSpeakerHoverModalCheckBox);
        m_audioSpeakerHoverModalCheckBox->setChecked(m_audioSpeakerHoverModalEnabled);
    }
    if (m_audioShowWaveformCheckBox) {
        QSignalBlocker block(m_audioShowWaveformCheckBox);
        m_audioShowWaveformCheckBox->setChecked(m_audioWaveformVisible);
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
    if (m_preview) {
        m_preview->setAudioSpeakerHoverModalEnabled(m_audioSpeakerHoverModalEnabled);
        m_preview->setAudioWaveformVisible(m_audioWaveformVisible);
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

    const QVector<ExportRangeSegment> playbackRanges = effectivePlaybackRanges();
    setTransformSkipAwareTimelineRanges(
        speechFilterPlaybackEnabled() ? playbackRanges : QVector<ExportRangeSegment>{});
    
    markStartup(QStringLiteral("apply_state.preview_bind.begin"));
    m_preview->beginBulkUpdate();
    m_preview->setClipCount(m_timeline->clips().size());
    m_preview->setTimelineTracks(m_timeline->tracks());
    m_preview->setTimelineClips(m_timeline->clips());
    m_preview->setExportRanges(playbackRanges);
    m_preview->setRenderSyncMarkers(m_timeline->renderSyncMarkers());
    m_preview->setSelectedClipId(selectedClipId);
    m_preview->endBulkUpdate();
    markStartup(QStringLiteral("apply_state.preview_bind.end"));
    
    if (m_audioEngine) {
        markStartup(QStringLiteral("apply_state.audio_bind.begin"));
        m_audioEngine->setTimelineClips(m_timeline->clips());
        m_audioEngine->setExportRanges(playbackRanges);
        m_audioEngine->setRenderSyncMarkers(m_timeline->renderSyncMarkers());
        m_audioEngine->setSpeechFilterFadeSamples(m_speechFilterFadeSamples);
        m_audioEngine->setSpeechFilterRangeCrossfadeEnabled(m_speechFilterRangeCrossfade);
        m_audioEngine->setPlaybackWarpMode(m_playbackAudioWarpMode);
        m_audioEngine->setPlaybackRate(effectiveAudioWarpRate());
        if (!startupMarking) {
            m_audioEngine->seek(currentFrame);
        }
        markStartup(QStringLiteral("apply_state.audio_bind.end"));
    }
    
    markStartup(QStringLiteral("apply_state.seek.begin"),
                QJsonObject{{QStringLiteral("target_frame"), static_cast<qint64>(currentFrame)}});
    if (!startupMarking) {
        setCurrentFrame(currentFrame);
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
    
    // Use the projects root from editor.config if available, otherwise use the saved media root
    const QString projectsRoot = rootDirPath();
    const QString mediaRoot = (!projectsRoot.isEmpty() && QDir(projectsRoot).exists()) 
        ? projectsRoot 
        : resolvedRootPath;
    
    QTimer::singleShot(0, this, [this, mediaRoot, currentFrame, startupMarking]() {
        if (startupMarking) {
            if (m_audioEngine) {
                m_audioEngine->seek(currentFrame);
            }
            setCurrentFrame(currentFrame);
        }
        if (m_explorerPane) {
            m_explorerPane->setInitialRootPath(mediaRoot);
        }
        // Ensure projects root is set to match media root.
        setRootDirPath(mediaRoot);
        loadProjectsFromFolders();
        refreshProjectsList();
        m_inspectorPane->refresh();
    });
}
