#include "editor.h"
#include "keyframe_table_shared.h"
#include "clip_serialization.h"
#include "transform_skip_aware_timing.h"
#include "debug_controls.h"
#include "speaker_export_harness.h"
#include <cppmonetize/AuthIdentity.h>
#include <cppmonetize/MonetizeClient.h>
#include <cppmonetize/OAuthDesktopFlow.h>
#include <cppmonetize/TokenStore.h>

#include <QApplication>
#include <QCommandLineOption>
#include <QCommandLineParser>
#include <QCoreApplication>
#include <QCryptographicHash>
#include <QDateTime>
#include <QDebug>
#include <QDesktopServices>
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
#include <QPainter>
#include <QGridLayout>
#include <QPixmap>
#include <QRandomGenerator>
#include <QRegularExpression>
#include <QSaveFile>
#include <QShortcut>
#include <QSignalBlocker>
#include <QSet>
#include <QTcpSocket>
#include <QStandardPaths>
#include <QStyle>
#include <QTextCursor>
#include <QTextStream>
#include <QTemporaryFile>
#include <QUrl>
#include <QUrlQuery>
#include <QVBoxLayout>

#include <cmath>
#include <cstring>

using namespace editor;

#include "playback_debug.h"

namespace {

cppmonetize::MonetizeClient createJCutMonetizeClient(const QString& apiBaseUrl,
                                                      int timeoutMs,
                                                      const QString& contractPrefix = QStringLiteral("1."))
{
    cppmonetize::ClientConfig cfg;
    cfg.apiBaseUrl = apiBaseUrl;
    cfg.timeoutMs = timeoutMs;
    cfg.clientId = QStringLiteral("jcut-desktop");
    cfg.requiredContractPrefix = contractPrefix;
    cfg.telemetryHook = [](const cppmonetize::RequestTelemetryEvent& event) {
        if (event.success) {
            return;
        }
        qWarning().noquote() << "[CPPMonetize][JCut]"
                             << event.operation
                             << "status=" << event.statusCode
                             << "request_id=" << event.clientRequestId
                             << "message=" << event.message;
    };
    return cppmonetize::MonetizeClient(cfg);
}

QString aiDisplayIdentity(const QString& explicitIdentity, const QString& accessToken)
{
    const QString trimmed = explicitIdentity.trimmed();
    if (!trimmed.isEmpty()) {
        return trimmed;
    }
    return cppmonetize::parseAccessTokenIdentity(accessToken).displayIdentity();
}

QString initialsFromIdentity(const QString& identity)
{
    const QString token = identity.section(QLatin1Char('@'), 0, 0).trimmed();
    if (token.isEmpty()) {
        return QStringLiteral("U");
    }
    const QStringList pieces = token.split(QRegularExpression(QStringLiteral("[._\\-\\s]+")),
                                           Qt::SkipEmptyParts);
    QString initials;
    for (const QString& piece : pieces) {
        if (!piece.isEmpty()) {
            initials += piece.left(1).toUpper();
            if (initials.size() >= 2) {
                break;
            }
        }
    }
    if (initials.isEmpty()) {
        initials = token.left(1).toUpper();
    }
    return initials.left(2);
}

QPixmap buildFallbackAvatar(const QString& identity)
{
    constexpr int size = 28;
    QPixmap pix(size, size);
    pix.fill(Qt::transparent);

    const QByteArray hash = QCryptographicHash::hash(identity.toUtf8(), QCryptographicHash::Md5);
    QColor color(0x3d, 0x7c, 0xc9);
    if (hash.size() >= 3) {
        color = QColor(static_cast<uchar>(hash[0]), static_cast<uchar>(hash[1]), static_cast<uchar>(hash[2]));
        color = color.lighter(130);
    }

    QPainter painter(&pix);
    painter.setRenderHint(QPainter::Antialiasing);
    painter.setBrush(color);
    painter.setPen(Qt::NoPen);
    painter.drawEllipse(0, 0, size, size);
    painter.setPen(Qt::white);
    QFont font = painter.font();
    font.setBold(true);
    font.setPointSize(10);
    painter.setFont(font);
    painter.drawText(QRect(0, 0, size, size), Qt::AlignCenter, initialsFromIdentity(identity));
    return pix;
}

}  // namespace

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
        PlaybackRuntimeConfig{playbackSpeed, playbackClockSource, playbackAudioWarpMode});
    
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



bool EditorWindow::parseSyncActionText(const QString &text, RenderSyncAction *actionOut) const
{
    const QString normalized = text.trimmed().toLower();
    if (normalized == QStringLiteral("duplicate") || normalized == QStringLiteral("dup")) {
        *actionOut = RenderSyncAction::DuplicateFrame;
        return true;
    }
    if (normalized == QStringLiteral("skip")) {
        *actionOut = RenderSyncAction::SkipFrame;
        return true;
    }
    return false;
}

void EditorWindow::refreshSyncInspector()
{
    if (m_syncTab) {
        m_syncTab->refresh();
    }
}

void EditorWindow::onSyncTableSelectionChanged()
{
    // Sync table interactions are handled by SyncTab.
}

void EditorWindow::onSyncTableItemChanged(QTableWidgetItem* item)
{
    Q_UNUSED(item);
    // Sync table interactions are handled by SyncTab.
}

void EditorWindow::onSyncTableItemDoubleClicked(QTableWidgetItem* item)
{
    Q_UNUSED(item);
    // Sync table interactions are handled by SyncTab.
}

void EditorWindow::onSyncTableCustomContextMenu(const QPoint& pos)
{
    Q_UNUSED(pos);
    // Sync table interactions are handled by SyncTab.
}

void EditorWindow::refreshClipInspector()
{
    if (m_propertiesTab) {
        m_propertiesTab->refresh();
    }
}

void EditorWindow::refreshTracksTab()
{
    if (m_tracksTab) {
        m_tracksTab->refresh();
    }
}

void EditorWindow::onTrackTableItemChanged(QTableWidgetItem* item)
{
    Q_UNUSED(item);
    // Tracks table interactions are handled by TracksTab.
}

void EditorWindow::refreshOutputInspector()
{
    if (m_outputTab) {
        m_outputTab->refresh();
    }
}

void EditorWindow::applyOutputRangeFromInspector()
{
    if (m_outputTab) {
        m_outputTab->applyRangeFromInspector();
    }
}

void EditorWindow::renderFromOutputInspector()
{
    if (m_outputTab) {
        m_outputTab->renderFromInspector();
    }
}

RenderRequest EditorWindow::buildRenderRequestFromOutputControls() const
{
    RenderRequest request;
    request.outputFormat = m_outputFormatCombo
        ? m_outputFormatCombo->currentData().toString()
        : QStringLiteral("mp4");
    if (request.outputFormat.isEmpty()) {
        request.outputFormat = QStringLiteral("mp4");
    }

    request.outputSize = QSize(
        m_outputWidthSpin ? m_outputWidthSpin->value() : 1080,
        m_outputHeightSpin ? m_outputHeightSpin->value() : 1920);
    request.useProxyMedia = m_renderUseProxiesCheckBox &&
                            m_renderUseProxiesCheckBox->isChecked();
    request.createVideoFromImageSequence = m_createImageSequenceCheckBox &&
                                           m_createImageSequenceCheckBox->isChecked();
    if (request.createVideoFromImageSequence && m_imageSequenceFormatCombo) {
        request.imageSequenceFormat = m_imageSequenceFormatCombo->currentData().toString();
        if (request.imageSequenceFormat.isEmpty()) {
            request.imageSequenceFormat = QStringLiteral("jpeg");
        }
    }
    return request;
}

void EditorWindow::applyPreviewViewMode(const QString& modeText)
{
    if (!m_featureAudioPreviewMode) {
        m_previewViewMode = QStringLiteral("video");
        if (m_preview) {
            m_preview->setViewMode(PreviewWindow::ViewMode::Video);
        }
        return;
    }
    const QString normalized = modeText.trimmed().toLower();
    m_previewViewMode = (normalized.contains(QStringLiteral("audio")))
                            ? QStringLiteral("audio")
                            : QStringLiteral("video");
    if (m_preview) {
        m_preview->setViewMode(m_previewViewMode == QStringLiteral("audio")
                                   ? PreviewWindow::ViewMode::Audio
                                   : PreviewWindow::ViewMode::Video);
    }
    if (!m_loadingState) {
        scheduleSaveState();
    }
}

void EditorWindow::openAudioToolsDialog()
{
    if (!m_featureAudioDynamicsTools) {
        QMessageBox::information(this,
                                 QStringLiteral("Audio Dynamics"),
                                 QStringLiteral("Audio dynamics tools are disabled by feature flag."));
        return;
    }
    QDialog dialog(this);
    dialog.setWindowTitle(QStringLiteral("Audio Dynamics"));
    dialog.setModal(true);
    auto* layout = new QVBoxLayout(&dialog);
    layout->setContentsMargins(12, 12, 12, 12);
    layout->setSpacing(8);

    auto makeSpin = [&dialog](qreal min, qreal max, qreal step, int decimals, qreal value) {
        auto* spin = new QDoubleSpinBox(&dialog);
        spin->setRange(min, max);
        spin->setSingleStep(step);
        spin->setDecimals(decimals);
        spin->setValue(value);
        return spin;
    };

    auto* normalizeCheck = new QCheckBox(QStringLiteral("Normalize"), &dialog);
    normalizeCheck->setChecked(m_previewAudioDynamics.normalizeEnabled);
    auto* normalizeTarget = makeSpin(-24.0, 0.0, 0.5, 1, m_previewAudioDynamics.normalizeTargetDb);
    auto* peakReductionCheck = new QCheckBox(QStringLiteral("Peak Reduction"), &dialog);
    peakReductionCheck->setChecked(m_previewAudioDynamics.peakReductionEnabled);
    auto* peakThreshold = makeSpin(-24.0, 0.0, 0.5, 1, m_previewAudioDynamics.peakThresholdDb);
    auto* limiterCheck = new QCheckBox(QStringLiteral("Limiter"), &dialog);
    limiterCheck->setChecked(m_previewAudioDynamics.limiterEnabled);
    auto* limiterThreshold = makeSpin(-12.0, 0.0, 0.1, 1, m_previewAudioDynamics.limiterThresholdDb);
    auto* compressorCheck = new QCheckBox(QStringLiteral("Compressor"), &dialog);
    compressorCheck->setChecked(m_previewAudioDynamics.compressorEnabled);
    auto* compressorThreshold =
        makeSpin(-30.0, -1.0, 0.5, 1, m_previewAudioDynamics.compressorThresholdDb);
    auto* compressorRatio = makeSpin(1.0, 20.0, 0.1, 1, m_previewAudioDynamics.compressorRatio);

    auto* form = new QFormLayout;
    form->addRow(normalizeCheck, normalizeTarget);
    form->addRow(peakReductionCheck, peakThreshold);
    form->addRow(limiterCheck, limiterThreshold);
    form->addRow(compressorCheck, compressorThreshold);
    form->addRow(QStringLiteral("Compressor Ratio"), compressorRatio);
    layout->addLayout(form);

    auto* btnRow = new QHBoxLayout;
    btnRow->addStretch(1);
    auto* cancelBtn = new QPushButton(QStringLiteral("Cancel"), &dialog);
    auto* applyBtn = new QPushButton(QStringLiteral("Apply"), &dialog);
    btnRow->addWidget(cancelBtn);
    btnRow->addWidget(applyBtn);
    layout->addLayout(btnRow);
    QObject::connect(cancelBtn, &QPushButton::clicked, &dialog, &QDialog::reject);
    QObject::connect(applyBtn, &QPushButton::clicked, &dialog, &QDialog::accept);
    if (dialog.exec() != QDialog::Accepted) {
        return;
    }

    m_previewAudioDynamics.normalizeEnabled = normalizeCheck->isChecked();
    m_previewAudioDynamics.normalizeTargetDb = normalizeTarget->value();
    m_previewAudioDynamics.peakReductionEnabled = peakReductionCheck->isChecked();
    m_previewAudioDynamics.peakThresholdDb = peakThreshold->value();
    m_previewAudioDynamics.limiterEnabled = limiterCheck->isChecked();
    m_previewAudioDynamics.limiterThresholdDb = limiterThreshold->value();
    m_previewAudioDynamics.compressorEnabled = compressorCheck->isChecked();
    m_previewAudioDynamics.compressorThresholdDb = compressorThreshold->value();
    m_previewAudioDynamics.compressorRatio = compressorRatio->value();

    if (m_preview) {
        m_preview->setAudioDynamicsSettings(m_previewAudioDynamics);
    }
    scheduleSaveState();
}

QString EditorWindow::aiSecureStoreServiceName() const
{
    QString base = m_aiProxyBaseUrl.trimmed();
    if (base.isEmpty()) {
        base = QStringLiteral("unset-gateway");
    }
    QByteArray keyBytes = base.toUtf8().toHex();
    if (keyBytes.size() > 64) {
        keyBytes = keyBytes.left(64);
    }
    return QStringLiteral("jcut.ai.%1").arg(QString::fromLatin1(keyBytes));
}

bool EditorWindow::readAiTokenFromSecureStore(QString* tokenOut) const
{
    if (tokenOut) {
        tokenOut->clear();
    }
    if (m_aiProxyBaseUrl.trimmed().isEmpty()) {
        return false;
    }

    cppmonetize::TokenStoreConfig cfg;
    cfg.appName = QStringLiteral("jcut");
    cfg.orgName = QStringLiteral("jcut");
    cfg.serviceName = aiSecureStoreServiceName();
    auto store = cppmonetize::createDefaultTokenStore(cfg);
    const auto tokenResult = store->loadToken();
    if (!tokenResult.hasValue()) {
        return false;
    }
    const QString token = tokenResult.value().trimmed();
    if (token.isEmpty()) {
        return false;
    }
    if (tokenOut) {
        *tokenOut = token;
    }
    return true;
}

bool EditorWindow::writeAiTokenToSecureStore(const QString& token, QString* errorOut) const
{
    if (errorOut) {
        errorOut->clear();
    }
    if (token.trimmed().isEmpty()) {
        if (errorOut) {
            *errorOut = QStringLiteral("Cannot store empty token.");
        }
        return false;
    }
    if (m_aiProxyBaseUrl.trimmed().isEmpty()) {
        if (errorOut) {
            *errorOut = QStringLiteral("Gateway URL is required before storing token.");
        }
        return false;
    }

    cppmonetize::TokenStoreConfig cfg;
    cfg.appName = QStringLiteral("jcut");
    cfg.orgName = QStringLiteral("jcut");
    cfg.serviceName = aiSecureStoreServiceName();
    auto store = cppmonetize::createDefaultTokenStore(cfg);
    const auto writeResult = store->storeToken(token.trimmed(), m_aiUserId.trimmed());
    if (!writeResult.hasValue()) {
        if (errorOut) {
            *errorOut = writeResult.error().message;
        }
        return false;
    }
    return true;
}

bool EditorWindow::clearAiTokenFromSecureStore(QString* errorOut) const
{
    if (errorOut) {
        errorOut->clear();
    }
    if (m_aiProxyBaseUrl.trimmed().isEmpty()) {
        return true;
    }
    cppmonetize::TokenStoreConfig cfg;
    cfg.appName = QStringLiteral("jcut");
    cfg.orgName = QStringLiteral("jcut");
    cfg.serviceName = aiSecureStoreServiceName();
    auto store = cppmonetize::createDefaultTokenStore(cfg);
    const auto clearResult = store->clear();
    if (!clearResult.hasValue()) {
        if (errorOut) {
            *errorOut = clearResult.error().message;
        }
        return false;
    }
    return true;
}

void EditorWindow::updateProfileAvatarButton()
{
    if (!m_profileAvatarButton) {
        return;
    }

    const bool loggedIn = !m_aiAuthToken.trimmed().isEmpty();
    const QString displayIdentity = aiDisplayIdentity(m_aiUserId, m_aiAuthToken);

    if (!loggedIn) {
        m_profileAvatarButton->setText(QStringLiteral("Guest ▼"));
        m_profileAvatarButton->setIcon(QIcon());
        m_profileAvatarButton->setStyleSheet(QStringLiteral(
            "QPushButton#tabs\\.profile_avatar_button {"
            " background: #223041; border: 1px solid #3d5268; border-radius: 7px;"
            " color: #edf2f7; padding: 4px 10px; font-weight: 600; }"
            "QPushButton#tabs\\.profile_avatar_button:hover { background: #2b3c50; }"));
        m_profileAvatarButton->setMinimumWidth(112);
        m_profileAvatarButton->setMinimumHeight(26);
        m_profileAvatarButton->setMaximumSize(QWIDGETSIZE_MAX, QWIDGETSIZE_MAX);
        m_profileAvatarButton->setToolTip(QStringLiteral("Guest - click to sign in"));
    } else {
        const QString avatarSeed = displayIdentity.isEmpty() ? QStringLiteral("user@jcut") : displayIdentity;
        m_profileAvatarButton->setText(QString());
        m_profileAvatarButton->setIcon(QIcon(buildFallbackAvatar(avatarSeed)));
        m_profileAvatarButton->setIconSize(QSize(28, 28));
        m_profileAvatarButton->setFixedSize(34, 34);
        m_profileAvatarButton->setStyleSheet(QStringLiteral(
            "QPushButton#tabs\\.profile_avatar_button {"
            " border: 1px solid #3a4359; border-radius: 17px; padding: 0;"
            " background: #223041; }"
            "QPushButton#tabs\\.profile_avatar_button:hover { border-color: #6ea8ff; background: #2b3c50; }"));
        m_profileAvatarButton->setToolTip(
            displayIdentity.isEmpty()
                ? QStringLiteral("Signed in")
                : QStringLiteral("Signed in as %1").arg(displayIdentity));
    }
    m_profileAvatarButton->setVisible(true);
    m_profileAvatarButton->raise();
}

void EditorWindow::onProfileAvatarButtonClicked()
{
    if (m_aiAuthToken.trimmed().isEmpty()) {
        configureAiGatewayLogin();
        return;
    }

    const QString displayIdentity = aiDisplayIdentity(m_aiUserId, m_aiAuthToken);
    QMenu menu(this);
    QAction* profileAction = menu.addAction(
        displayIdentity.isEmpty()
            ? QStringLiteral("Signed in")
            : QStringLiteral("Signed in as %1").arg(displayIdentity));
    profileAction->setEnabled(false);
    menu.addSeparator();
    QAction* switchAction = menu.addAction(QStringLiteral("Switch Account"));
    QAction* logoutAction = menu.addAction(QStringLiteral("Log Out"));

    QAction* picked =
        menu.exec(m_profileAvatarButton->mapToGlobal(QPoint(0, m_profileAvatarButton->height())));
    if (picked == switchAction) {
        configureAiGatewayLogin();
    } else if (picked == logoutAction) {
        clearAiGatewayLogin();
    }
}

void EditorWindow::configureAiGatewayLogin()
{
    QDialog dialog(this);
    dialog.setWindowTitle(QStringLiteral("JCut - AI Login"));
    dialog.setModal(true);
    dialog.setMinimumSize(460, 420);
    dialog.setStyleSheet(
        QStringLiteral(
            "QDialog { background-color: #1c1f26; color: #f4f7ff; }"
            "QLabel { color: #f4f7ff; }"
            "QFrame#card { background-color: #222734; border-radius: 14px; border: 1px solid #2c3446; }"
            "QPushButton#actionButton { background-color: #3d7cc9; color: white; padding: 9px 20px; border-radius: 8px; font-weight: 700; }"
            "QPushButton#actionButton:hover { background-color: #4b8add; }"
            "QPushButton#switchButton { background-color: transparent; border: none; color: #8fc6ff; text-decoration: underline; }"
            "QPushButton#switchButton:hover { color: #b8daff; }"
            "QPushButton#altButton { background-color: transparent; border: 1px solid #4b5470; color: #a0a8c0; padding: 8px 12px; border-radius: 8px; }"
            "QPushButton#altButton:hover { border-color: #6b789b; color: #f4f7ff; }"
            "QLineEdit { background-color: #202635; color: #f4f7ff; border: 1px solid #323a4f; border-radius: 8px; padding: 8px 10px; }"
            "QLineEdit:focus { background-color: #242b3d; border-color: #5ea1ff; }"));

    auto* mainLayout = new QVBoxLayout(&dialog);
    mainLayout->setSpacing(12);
    mainLayout->setContentsMargins(18, 18, 18, 18);

    auto* titleLabel = new QLabel(QStringLiteral("<h2>Sign In to JCut AI</h2>"), &dialog);
    titleLabel->setAlignment(Qt::AlignCenter);
    mainLayout->addWidget(titleLabel);

    auto* card = new QFrame(&dialog);
    card->setObjectName(QStringLiteral("card"));
    auto* cardLayout = new QVBoxLayout(card);
    cardLayout->setSpacing(10);
    cardLayout->setContentsMargins(16, 16, 16, 16);

    auto* baseUrlLabel = new QLabel(QStringLiteral("Gateway URL"), &dialog);
    auto* baseUrlEdit = new QLineEdit(&dialog);
    baseUrlEdit->setPlaceholderText(QStringLiteral("https://your-gateway.example.com"));
    baseUrlEdit->setText(m_aiProxyBaseUrl);
    cardLayout->addWidget(baseUrlLabel);
    cardLayout->addWidget(baseUrlEdit);

    auto* emailLabel = new QLabel(QStringLiteral("Email"), &dialog);
    auto* emailEdit = new QLineEdit(&dialog);
    emailEdit->setPlaceholderText(QStringLiteral("you@example.com"));
    cardLayout->addWidget(emailLabel);
    cardLayout->addWidget(emailEdit);

    auto* passwordLabel = new QLabel(QStringLiteral("Password"), &dialog);
    auto* passwordEdit = new QLineEdit(&dialog);
    passwordEdit->setEchoMode(QLineEdit::Password);
    passwordEdit->setPlaceholderText(QStringLiteral("Enter password"));
    cardLayout->addWidget(passwordLabel);
    cardLayout->addWidget(passwordEdit);

    auto* confirmRow = new QWidget(&dialog);
    auto* confirmLayout = new QVBoxLayout(confirmRow);
    confirmLayout->setContentsMargins(0, 0, 0, 0);
    confirmLayout->setSpacing(6);
    auto* confirmLabel = new QLabel(QStringLiteral("Confirm Password"), &dialog);
    auto* confirmEdit = new QLineEdit(&dialog);
    confirmEdit->setEchoMode(QLineEdit::Password);
    confirmEdit->setPlaceholderText(QStringLiteral("Confirm password"));
    confirmLayout->addWidget(confirmLabel);
    confirmLayout->addWidget(confirmEdit);
    confirmRow->setVisible(false);
    cardLayout->addWidget(confirmRow);

    auto* statusLabel = new QLabel(QString(), &dialog);
    statusLabel->setWordWrap(true);
    statusLabel->setAlignment(Qt::AlignCenter);
    statusLabel->setStyleSheet(QStringLiteral("color: #ff6b6b;"));
    cardLayout->addWidget(statusLabel);

    auto* actionButton = new QPushButton(QStringLiteral("Sign In"), &dialog);
    actionButton->setObjectName(QStringLiteral("actionButton"));
    cardLayout->addWidget(actionButton);

    auto* switchButton = new QPushButton(QStringLiteral("Don't have an account? Register"), &dialog);
    switchButton->setObjectName(QStringLiteral("switchButton"));
    switchButton->setCursor(Qt::PointingHandCursor);
    cardLayout->addWidget(switchButton, 0, Qt::AlignCenter);

    auto* oauthLabel =
        new QLabel(QStringLiteral("<center style='color: #6b789b;'>- or sign in with -</center>"), &dialog);
    cardLayout->addWidget(oauthLabel);

    auto* oauthRow = new QHBoxLayout();
    oauthRow->setSpacing(10);
    auto makeOAuthButton = [&dialog, oauthRow](const QString& objectName,
                                                const QString& label,
                                                const QString& color,
                                                int doneCode) {
        auto* btn = new QPushButton(label, &dialog);
        btn->setObjectName(objectName);
        btn->setCursor(Qt::PointingHandCursor);
        btn->setStyleSheet(
            QStringLiteral("QPushButton { background-color: %1; color: white; padding: 8px 14px;"
                           " border-radius: 8px; border: none; font-weight: 700; }"
                           "QPushButton:hover { opacity: 0.9; }")
                .arg(color));
        QObject::connect(btn, &QPushButton::clicked, &dialog, [&dialog, doneCode]() { dialog.done(doneCode); });
        oauthRow->addWidget(btn);
    };
    makeOAuthButton(QStringLiteral("oauthGoogle"), QStringLiteral("Google"), QStringLiteral("#4285F4"), 100);
    makeOAuthButton(QStringLiteral("oauthGithub"), QStringLiteral("GitHub"), QStringLiteral("#333333"), 101);
    cardLayout->addLayout(oauthRow);

    auto* secondaryActions = new QHBoxLayout();
    auto* cancelButton = new QPushButton(QStringLiteral("Cancel"), &dialog);
    cancelButton->setObjectName(QStringLiteral("altButton"));
    auto* tokenButton = new QPushButton(QStringLiteral("Use Access Token"), &dialog);
    tokenButton->setObjectName(QStringLiteral("altButton"));
    secondaryActions->addWidget(tokenButton);
    secondaryActions->addStretch(1);
    secondaryActions->addWidget(cancelButton);
    cardLayout->addLayout(secondaryActions);

    connect(cancelButton, &QPushButton::clicked, &dialog, &QDialog::reject);
    connect(tokenButton, &QPushButton::clicked, &dialog, [&dialog]() { dialog.done(102); });

    mainLayout->addWidget(card);

    auto setStatus = [statusLabel](const QString& msg, bool error) {
        statusLabel->setStyleSheet(error ? QStringLiteral("color: #ff6b6b;")
                                         : QStringLiteral("color: #6bff8a;"));
        statusLabel->setText(msg);
    };

    bool registerMode = false;
    auto setRegisterMode = [&]() {
        if (registerMode) {
            titleLabel->setText(QStringLiteral("<h2>Create Account</h2>"));
            actionButton->setText(QStringLiteral("Register"));
            switchButton->setText(QStringLiteral("Already have an account? Sign In"));
            confirmRow->setVisible(true);
        } else {
            titleLabel->setText(QStringLiteral("<h2>Sign In to JCut AI</h2>"));
            actionButton->setText(QStringLiteral("Sign In"));
            switchButton->setText(QStringLiteral("Don't have an account? Register"));
            confirmRow->setVisible(false);
        }
        statusLabel->clear();
    };

    auto normalizedBaseUrl = [baseUrlEdit]() -> QString {
        QString base = baseUrlEdit->text().trimmed();
        while (base.endsWith(QLatin1Char('/'))) {
            base.chop(1);
        }
        return base;
    };
    auto apiBaseCandidates = [&](const QString& normalizedGatewayBase) -> QStringList {
        QStringList candidates;
        if (normalizedGatewayBase.isEmpty()) {
            return candidates;
        }
        candidates.push_back(normalizedGatewayBase);
        if (normalizedGatewayBase.endsWith(QStringLiteral("/api"))) {
            const QString withoutApi = normalizedGatewayBase.left(normalizedGatewayBase.size() - 4);
            if (!withoutApi.isEmpty()) {
                candidates.push_back(withoutApi);
            }
        } else {
            candidates.push_back(normalizedGatewayBase + QStringLiteral("/api"));
        }
        candidates.removeDuplicates();
        return candidates;
    };

    auto applyLoginResult = [&](const QString& normalizedGatewayBase,
                                const QString& accessToken,
                                const QString& userIdHint,
                                const QString& emailHint) {
        m_aiProxyBaseUrl = normalizedGatewayBase;
        m_aiAuthToken = accessToken.trimmed();
        QString bestUser = userIdHint.trimmed();
        if (bestUser.isEmpty()) {
            bestUser = emailHint.trimmed();
        }
        if (bestUser.isEmpty()) {
            for (const QString& base : apiBaseCandidates(normalizedGatewayBase)) {
                cppmonetize::MonetizeClient client = createJCutMonetizeClient(base, m_aiRequestTimeoutMs, QString());
                const auto whoResult = client.whoAmI(m_aiAuthToken);
                if (whoResult.hasValue()) {
                    bestUser = whoResult.value().userId.trimmed();
                    if (bestUser.isEmpty()) {
                        bestUser = whoResult.value().email.trimmed();
                    }
                    if (!bestUser.isEmpty()) {
                        break;
                    }
                }
            }
        }
        if (bestUser.isEmpty()) {
            bestUser = cppmonetize::parseAccessTokenIdentity(m_aiAuthToken).userId;
        }
        m_aiUserId = bestUser;

        QString secureStoreError;
        if (!writeAiTokenToSecureStore(m_aiAuthToken, &secureStoreError)) {
            QMessageBox::warning(this,
                                 QStringLiteral("AI Login"),
                                 QStringLiteral("Token saved for this session only. Secure storage failed: %1")
                                     .arg(secureStoreError));
        }
        refreshAiIntegrationState();
        scheduleSaveState();
    };

    connect(switchButton, &QPushButton::clicked, &dialog, [&]() {
        registerMode = !registerMode;
        setRegisterMode();
    });

    connect(actionButton, &QPushButton::clicked, &dialog, [&]() {
        const QString gatewayBase = normalizedBaseUrl();
        const QString email = emailEdit->text().trimmed();
        const QString password = passwordEdit->text();

        if (gatewayBase.isEmpty()) {
            setStatus(QStringLiteral("Gateway URL is required."), true);
            return;
        }
        if (email.isEmpty() || password.isEmpty()) {
            setStatus(QStringLiteral("Email and password are required."), true);
            return;
        }
        if (registerMode) {
            if (password != confirmEdit->text()) {
                setStatus(QStringLiteral("Passwords do not match."), true);
                return;
            }
            if (password.size() < 6) {
                setStatus(QStringLiteral("Password must be at least 6 characters."), true);
                return;
            }
        }

        setStatus(QStringLiteral("Connecting..."), false);
        QString errorText;
        QString token;
        QString userId;
        QString emailValue;
        bool authenticated = false;

        for (const QString& apiBase : apiBaseCandidates(gatewayBase)) {
            cppmonetize::OAuthDesktopFlow oauthFlow;
            const auto cfgResult = oauthFlow.fetchOAuthConfig(apiBase, m_aiRequestTimeoutMs);
            if (cfgResult.hasValue() && cfgResult.value().enabled && !cfgResult.value().supabaseAnonKey.isEmpty()) {
                const auto authResult =
                    oauthFlow.signInWithPassword(cfgResult.value(), email, password, registerMode, m_aiRequestTimeoutMs);
                if (authResult.hasValue()) {
                    token = authResult.value().token;
                    emailValue = authResult.value().email;
                    authenticated = true;
                    break;
                }
                errorText = authResult.error().message;
                continue;
            }

            cppmonetize::MonetizeClient client = createJCutMonetizeClient(apiBase, m_aiRequestTimeoutMs, QString());
            const auto authResult =
                registerMode ? client.registerUser(email, password) : client.signIn(email, password);
            if (authResult.hasValue()) {
                token = authResult.value().accessToken;
                userId = authResult.value().userId;
                emailValue = authResult.value().email;
                authenticated = true;
                break;
            }
            errorText = authResult.error().message;
        }

        if (!authenticated || token.trimmed().isEmpty()) {
            setStatus(errorText.isEmpty() ? QStringLiteral("Authentication failed.") : errorText, true);
            return;
        }

        applyLoginResult(gatewayBase, token, userId, emailValue);
        setStatus(QStringLiteral("Signed in."), false);
        dialog.accept();
    });

    setRegisterMode();
    const int result = dialog.exec();
    if (result == QDialog::Rejected) {
        return;
    }

    if (result == 102) {
        const QString gatewayBase = normalizedBaseUrl();
        if (gatewayBase.isEmpty()) {
            QMessageBox::warning(this,
                                 QStringLiteral("AI Login"),
                                 QStringLiteral("Gateway URL is required."));
            return;
        }
        bool ok = false;
        const QString token = QInputDialog::getText(this,
                                                    QStringLiteral("Use Access Token"),
                                                    QStringLiteral("Supabase access token (JWT)"),
                                                    QLineEdit::Password,
                                                    m_aiAuthToken,
                                                    &ok)
                                  .trimmed();
        if (!ok || token.isEmpty()) {
            return;
        }
        applyLoginResult(gatewayBase, token, QString(), QString());
        return;
    }

    if (result == 100 || result == 101) {
        const QString gatewayBase = normalizedBaseUrl();
        if (gatewayBase.isEmpty()) {
            QMessageBox::warning(this,
                                 QStringLiteral("AI Login"),
                                 QStringLiteral("Gateway URL is required."));
            return;
        }
        m_aiProxyBaseUrl = gatewayBase;
        startAiBrowserLogin(gatewayBase, result == 100 ? QStringLiteral("google") : QStringLiteral("github"));
    }
}

void EditorWindow::startAiBrowserLogin(const QString& gatewayBaseUrl, const QString& preferredProvider)
{
    QString normalizedBase = gatewayBaseUrl.trimmed();
    while (normalizedBase.endsWith(QLatin1Char('/'))) {
        normalizedBase.chop(1);
    }
    if (normalizedBase.isEmpty()) {
        QMessageBox::warning(this, QStringLiteral("AI Login"), QStringLiteral("Gateway URL is empty."));
        return;
    }

    auto randomToken = [](int bytes) -> QString {
        QByteArray data;
        data.resize(bytes);
        for (int i = 0; i < bytes; ++i) {
            data[i] = static_cast<char>(QRandomGenerator::global()->bounded(0, 256));
        }
        QString token = QString::fromLatin1(data.toBase64(QByteArray::Base64UrlEncoding |
                                                          QByteArray::OmitTrailingEquals));
        token.remove(QRegularExpression(QStringLiteral("[^A-Za-z0-9\\-_.~]")));
        return token;
    };
    auto base64UrlSha256 = [](const QString& input) -> QString {
        const QByteArray hash = QCryptographicHash::hash(input.toUtf8(), QCryptographicHash::Sha256);
        return QString::fromLatin1(hash.toBase64(QByteArray::Base64UrlEncoding |
                                                 QByteArray::OmitTrailingEquals));
    };

    m_aiAuthState = randomToken(24);
    m_aiAuthCodeVerifier = randomToken(48);
    const QString codeChallenge = base64UrlSha256(m_aiAuthCodeVerifier);

    m_aiAuthCallbackServer.reset(new QTcpServer(this));
    if (!m_aiAuthCallbackServer->listen(QHostAddress::LocalHost, 0)) {
        QMessageBox::warning(this,
                             QStringLiteral("AI Login"),
                             QStringLiteral("Failed to start local callback listener."));
        m_aiAuthCallbackServer.reset();
        return;
    }
    m_aiAuthCallbackPort = m_aiAuthCallbackServer->serverPort();
    m_aiAuthRedirectUri =
        QStringLiteral("http://127.0.0.1:%1/auth/callback").arg(m_aiAuthCallbackPort);

    connect(m_aiAuthCallbackServer.get(), &QTcpServer::newConnection, this, [this, normalizedBase]() {
        while (m_aiAuthCallbackServer && m_aiAuthCallbackServer->hasPendingConnections()) {
            QTcpSocket* socket = m_aiAuthCallbackServer->nextPendingConnection();
            connect(socket, &QTcpSocket::readyRead, this, [this, socket, normalizedBase]() {
                const QByteArray request = socket->readAll();
                const QList<QByteArray> lines = request.split('\n');
                const QByteArray requestLine = lines.isEmpty() ? QByteArray() : lines.constFirst().trimmed();
                const QList<QByteArray> parts = requestLine.split(' ');
                const QByteArray target = (parts.size() >= 2) ? parts.at(1) : QByteArray();
                const QUrl reqUrl(QStringLiteral("http://127.0.0.1") + QString::fromUtf8(target));
                const QUrlQuery query(reqUrl);

                QString html;
                QString statusText;
                bool success = false;
                if (query.queryItemValue(QStringLiteral("error")).trimmed().size() > 0) {
                    statusText = query.queryItemValue(QStringLiteral("error_description"));
                    if (statusText.isEmpty()) {
                        statusText = query.queryItemValue(QStringLiteral("error"));
                    }
                } else if (query.queryItemValue(QStringLiteral("state")) != m_aiAuthState) {
                    statusText = QStringLiteral("State validation failed.");
                } else {
                    const QString accessToken = query.queryItemValue(QStringLiteral("access_token")).trimmed();
                    if (!accessToken.isEmpty()) {
                        m_aiAuthToken = accessToken;
                        m_aiUserId = query.queryItemValue(QStringLiteral("user_id")).trimmed();
                        QString secureStoreError;
                        if (!writeAiTokenToSecureStore(m_aiAuthToken, &secureStoreError)) {
                            statusText = QStringLiteral("Logged in, but secure token storage failed: %1")
                                             .arg(secureStoreError);
                        }
                        success = true;
                    } else {
                        const QString code = query.queryItemValue(QStringLiteral("code")).trimmed();
                        if (code.isEmpty()) {
                            statusText = QStringLiteral("Missing auth code.");
                        } else {
                            QString exchangeError;
                            success = exchangeAiAuthCode(code, m_aiAuthState, &exchangeError);
                            if (!success) {
                                statusText = exchangeError;
                            }
                        }
                    }
                }

                if (success) {
                    refreshAiIntegrationState();
                    scheduleSaveState();
                    statusText = QStringLiteral("Login successful. You can return to JCut.");
                }
                html = QStringLiteral(
                           "<html><body style='font-family:sans-serif;background:#0d1117;color:#e6edf3;padding:24px;'>"
                           "<h2>%1</h2><p>%2</p></body></html>")
                           .arg(success ? QStringLiteral("JCut AI Login Complete")
                                        : QStringLiteral("JCut AI Login Failed"),
                                statusText.toHtmlEscaped());
                const QByteArray body = html.toUtf8();
                const QByteArray resp =
                    QByteArrayLiteral("HTTP/1.1 200 OK\r\nContent-Type: text/html; charset=utf-8\r\nContent-Length: ") +
                    QByteArray::number(body.size()) +
                    QByteArrayLiteral("\r\nConnection: close\r\n\r\n") + body;
                socket->write(resp);
                socket->disconnectFromHost();

                if (m_aiAuthCallbackServer) {
                    m_aiAuthCallbackServer->close();
                    m_aiAuthCallbackServer.reset();
                    m_aiAuthCallbackPort = 0;
                }
                QMessageBox::information(this,
                                         QStringLiteral("AI Login"),
                                         statusText.isEmpty()
                                             ? QStringLiteral("Login completed.")
                                             : statusText);
            });
            connect(socket, &QTcpSocket::disconnected, socket, &QObject::deleteLater);
        }
    });

    QUrl startUrl(normalizedBase + QStringLiteral("/api/auth/desktop/start"));
    QUrlQuery query;
    query.addQueryItem(QStringLiteral("redirect_uri"), m_aiAuthRedirectUri);
    query.addQueryItem(QStringLiteral("state"), m_aiAuthState);
    query.addQueryItem(QStringLiteral("code_challenge"), codeChallenge);
    query.addQueryItem(QStringLiteral("code_challenge_method"), QStringLiteral("S256"));
    query.addQueryItem(QStringLiteral("client"), QStringLiteral("jcut-desktop"));
    if (!preferredProvider.trimmed().isEmpty()) {
        query.addQueryItem(QStringLiteral("provider"), preferredProvider.trimmed().toLower());
    }
    startUrl.setQuery(query);
    if (!QDesktopServices::openUrl(startUrl)) {
        if (m_aiAuthCallbackServer) {
            m_aiAuthCallbackServer->close();
            m_aiAuthCallbackServer.reset();
            m_aiAuthCallbackPort = 0;
        }
        QMessageBox::warning(this,
                             QStringLiteral("AI Login"),
                             QStringLiteral("Failed to open browser for login."));
        return;
    }
    QMessageBox::information(
        this,
        QStringLiteral("AI Login"),
        QStringLiteral("Browser login started.\nIf it does not complete automatically, ensure your gateway supports:\n"
                       "GET /api/auth/desktop/start and POST /api/auth/desktop/exchange."));
}

bool EditorWindow::exchangeAiAuthCode(const QString& code, const QString& state, QString* errorOut)
{
    QString normalizedBase = m_aiProxyBaseUrl.trimmed();
    while (normalizedBase.endsWith(QLatin1Char('/'))) {
        normalizedBase.chop(1);
    }
    if (normalizedBase.isEmpty()) {
        if (errorOut) {
            *errorOut = QStringLiteral("Gateway URL missing for code exchange.");
        }
        return false;
    }
    cppmonetize::MonetizeClient client =
        createJCutMonetizeClient(normalizedBase, m_aiRequestTimeoutMs);
    const auto exchangeResult = client.exchangeDesktopCode(
        code,
        state,
        m_aiAuthCodeVerifier,
        m_aiAuthRedirectUri);
    if (!exchangeResult.hasValue()) {
        if (errorOut) {
            *errorOut = exchangeResult.error().message;
        }
        return false;
    }
    m_aiAuthToken = exchangeResult.value().accessToken.trimmed();
    m_aiUserId = exchangeResult.value().userId.trimmed();
    QString secureStoreError;
    if (!writeAiTokenToSecureStore(m_aiAuthToken, &secureStoreError)) {
        qWarning() << "Secure token storage failed after code exchange:" << secureStoreError;
    }
    return true;
}

void EditorWindow::clearAiGatewayLogin()
{
    if (m_aiAuthCallbackServer) {
        m_aiAuthCallbackServer->close();
        m_aiAuthCallbackServer.reset();
        m_aiAuthCallbackPort = 0;
    }
    QString secureStoreError;
    if (!clearAiTokenFromSecureStore(&secureStoreError)) {
        qWarning() << "Failed clearing AI token from secure store:" << secureStoreError;
    }
    m_aiAuthToken.clear();
    m_aiUserId.clear();
    refreshAiIntegrationState();
    scheduleSaveState();
}

void EditorWindow::refreshAiIntegrationState()
{
    m_aiContractVersion = QStringLiteral("unknown");
    QString status = QStringLiteral("AI disabled: login required");
    bool enabled = false;
    QStringList modelOptions{
        QStringLiteral("deepseek-chat"),
        QStringLiteral("gpt-4o-mini"),
        QStringLiteral("mistral-small"),
        QStringLiteral("qwen2.5-7b-instruct")};
    QStringList fallbackModels;
    QString serviceUrl;
    int rateLimit = qMax(1, m_aiRateLimitPerMinute);
    int budgetCap = qMax(1, m_aiUsageBudgetCap);
    int timeoutMs = qMax(1000, m_aiRequestTimeoutMs);
    int retries = qBound(0, m_aiRequestRetries, 3);
    const QString envBaseUrl = qEnvironmentVariable("JCUT_AI_PROXY_BASE_URL").trimmed();
    const QString envToken = qEnvironmentVariable("JCUT_AI_AUTH_TOKEN").trimmed();
    if (m_aiProxyBaseUrl.trimmed().isEmpty()) {
        m_aiProxyBaseUrl = envBaseUrl;
    }
    if (m_aiAuthToken.trimmed().isEmpty()) {
        QString secureToken;
        if (readAiTokenFromSecureStore(&secureToken)) {
            m_aiAuthToken = secureToken;
        } else {
            m_aiAuthToken = envToken;
        }
    }

    const QString base = m_aiProxyBaseUrl.trimmed();
    if (!m_featureAiPanel) {
        status = QStringLiteral("AI disabled: feature_ai_panel=false");
    } else if (base.isEmpty()) {
        status = QStringLiteral("AI disabled: configure gateway URL");
    } else if (m_aiAuthToken.trimmed().isEmpty()) {
        status = QStringLiteral("AI disabled: sign in (missing access token)");
    } else {
        QString normalizedBase = base;
        while (normalizedBase.endsWith(QLatin1Char('/'))) {
            normalizedBase.chop(1);
        }
        serviceUrl = normalizedBase + QStringLiteral("/api/ai/task");
        auto applyEntitlements = [&](const cppmonetize::AiEntitlements& ent) {
            const bool entitled = ent.entitled;
            m_aiContractVersion = ent.contractVersion.trimmed();
            const bool contractOk = m_aiContractVersion.startsWith(QStringLiteral("1."));
            m_aiUserId = ent.userId.trimmed();
            if (!ent.models.isEmpty()) {
                modelOptions = ent.models;
            }
            fallbackModels = ent.fallbackOrder;
            if (ent.requestsPerMinute > 0) {
                rateLimit = qMax(1, ent.requestsPerMinute);
            }
            if (ent.projectBudget > 0) {
                budgetCap = qMax(1, ent.projectBudget);
            }
            if (ent.timeoutMs > 0) {
                timeoutMs = qMax(1000, ent.timeoutMs);
            }
            retries = qBound(0, ent.retries, 3);

            enabled = entitled && contractOk;
            if (!contractOk) {
                status = QStringLiteral("AI disabled: unsupported contract '%1'").arg(m_aiContractVersion);
            } else if (!entitled) {
                status = QStringLiteral("AI disabled: user not entitled");
            } else {
                status = QStringLiteral("AI enabled for %1 (%2 req/min, budget %3)")
                             .arg(m_aiUserId.isEmpty() ? QStringLiteral("user") : m_aiUserId)
                             .arg(QString::number(rateLimit))
                             .arg(QString::number(budgetCap));
            }
        };

        cppmonetize::MonetizeClient client =
            createJCutMonetizeClient(normalizedBase, timeoutMs);
        const auto entResult = client.getAiEntitlements(m_aiAuthToken);
        if (!entResult.hasValue()) {
            status = QStringLiteral("AI disabled: entitlement check failed (%1)")
                         .arg(entResult.error().message);
        } else {
            const cppmonetize::AiEntitlements ent = entResult.value();
            applyEntitlements(ent);
        }
    }
    m_aiRateLimitPerMinute = rateLimit;
    m_aiUsageBudgetCap = budgetCap;
    m_aiRequestTimeoutMs = timeoutMs;
    m_aiRequestRetries = retries;
    m_aiFallbackModels = fallbackModels;
    m_aiIntegrationEnabled = enabled;
    m_aiIntegrationStatus = status;
    m_aiServiceUrl = serviceUrl;
    if (m_aiModelCombo) {
        QSignalBlocker blocker(m_aiModelCombo);
        m_aiModelCombo->clear();
        for (const QString& model : modelOptions) {
            m_aiModelCombo->addItem(model);
        }
        int preferred = m_aiModelCombo->findText(m_aiSelectedModel, Qt::MatchFixedString);
        if (preferred < 0) {
            preferred = m_aiModelCombo->findText(QStringLiteral("deepseek-chat"), Qt::MatchFixedString);
        }
        if (preferred < 0 && m_aiModelCombo->count() > 0) {
            preferred = 0;
        }
        if (preferred >= 0) {
            m_aiModelCombo->setCurrentIndex(preferred);
            m_aiSelectedModel = m_aiModelCombo->itemText(preferred);
        }
        m_aiModelCombo->setEnabled(enabled && m_featureAiPanel);
    }
    if (m_aiLoginButton) {
        m_aiLoginButton->setEnabled(m_featureAiPanel);
    }
    if (m_aiLogoutButton) {
        m_aiLogoutButton->setEnabled(!m_aiAuthToken.isEmpty());
    }
    updateProfileAvatarButton();
    if (m_aiStatusLabel) {
        m_aiStatusLabel->setText(QStringLiteral("%1 | Usage %2/%3 (fail %4)")
                                     .arg(status)
                                     .arg(m_aiUsageRequests)
                                     .arg(m_aiUsageBudgetCap)
                                     .arg(m_aiUsageFailures));
    }
    for (QPushButton* btn : {m_aiTranscribeButton,
                             m_aiFindSpeakerNamesButton,
                             m_aiFindOrganizationsButton,
                             m_aiCleanAssignmentsButton}) {
        if (btn) {
            const bool speakerCleanupAction =
                (btn == m_aiFindSpeakerNamesButton ||
                 btn == m_aiFindOrganizationsButton ||
                 btn == m_aiCleanAssignmentsButton);
            const bool allowed = enabled && (!speakerCleanupAction || m_featureAiSpeakerCleanup);
            btn->setEnabled(allowed);
            btn->setToolTip(allowed
                                ? QString()
                                : (!m_featureAiSpeakerCleanup && speakerCleanupAction
                                       ? QStringLiteral("AI speaker cleanup disabled by feature flag.")
                                       : status));
        }
    }
}

QJsonObject EditorWindow::buildAiProjectContext() const
{
    QJsonObject root;
    root[QStringLiteral("current_frame")] = static_cast<qint64>(m_timeline ? m_timeline->currentFrame() : 0);
    root[QStringLiteral("selected_clip_id")] = m_timeline ? m_timeline->selectedClipId() : QString();
    QJsonArray clips;
    if (m_timeline) {
        for (const TimelineClip& clip : m_timeline->clips()) {
            QJsonObject c;
            c[QStringLiteral("id")] = clip.id;
            c[QStringLiteral("label")] = clip.label;
            c[QStringLiteral("file_path")] = clip.filePath;
            c[QStringLiteral("track_index")] = clip.trackIndex;
            c[QStringLiteral("start_frame")] = static_cast<qint64>(clip.startFrame);
            c[QStringLiteral("duration_frames")] = static_cast<qint64>(clip.durationFrames);
            c[QStringLiteral("has_audio")] = clip.hasAudio;
            c[QStringLiteral("media_type")] = static_cast<int>(clip.mediaType);
            clips.push_back(c);
        }
    }
    root[QStringLiteral("clips")] = clips;
    return root;
}

QJsonObject EditorWindow::runAiAction(const QString& action,
                                      const QJsonObject& payload,
                                      bool* okOut,
                                      QString* errorOut)
{
    if (okOut) {
        *okOut = false;
    }
    if (!m_aiIntegrationEnabled) {
        if (errorOut) {
            *errorOut = m_aiIntegrationStatus;
        }
        return {};
    }
    if (m_aiServiceUrl.trimmed().isEmpty()) {
        if (errorOut) {
            *errorOut = QStringLiteral("AI service URL is not configured.");
        }
        return {};
    }
    if (m_aiAuthToken.trimmed().isEmpty()) {
        if (errorOut) {
            *errorOut = QStringLiteral("AI login required.");
        }
        return {};
    }
    if (m_aiUsageRequests >= m_aiUsageBudgetCap) {
        if (errorOut) {
            *errorOut = QStringLiteral("AI budget exhausted for this project (%1 requests).")
                            .arg(m_aiUsageBudgetCap);
        }
        return {};
    }

    const qint64 nowMs = QDateTime::currentMSecsSinceEpoch();
    QVector<qint64> recent;
    recent.reserve(m_aiRecentRequestEpochMs.size());
    for (qint64 ts : std::as_const(m_aiRecentRequestEpochMs)) {
        if (nowMs - ts < 60000) {
            recent.push_back(ts);
        }
    }
    m_aiRecentRequestEpochMs = recent;
    if (m_aiRecentRequestEpochMs.size() >= m_aiRateLimitPerMinute) {
        if (errorOut) {
            *errorOut = QStringLiteral("AI rate limit reached (%1 requests/min).")
                            .arg(m_aiRateLimitPerMinute);
        }
        return {};
    }

    QStringList modelCandidates;
    if (!m_aiSelectedModel.trimmed().isEmpty()) {
        modelCandidates.push_back(m_aiSelectedModel.trimmed());
    }
    for (const QString& fallback : std::as_const(m_aiFallbackModels)) {
        const QString normalized = fallback.trimmed();
        if (!normalized.isEmpty() && !modelCandidates.contains(normalized)) {
            modelCandidates.push_back(normalized);
        }
    }
    if (modelCandidates.isEmpty()) {
        modelCandidates.push_back(QStringLiteral("deepseek-chat"));
    }

    QString normalizedBase = m_aiProxyBaseUrl.trimmed();
    while (normalizedBase.endsWith(QLatin1Char('/'))) {
        normalizedBase.chop(1);
    }
    cppmonetize::MonetizeClient client =
        createJCutMonetizeClient(normalizedBase, m_aiRequestTimeoutMs);

    QString lastError;
    for (const QString& model : std::as_const(modelCandidates)) {
        for (int attempt = 0; attempt <= m_aiRequestRetries; ++attempt) {
            QJsonObject requestObj;
            requestObj[QStringLiteral("action")] = action;
            requestObj[QStringLiteral("model")] = model;
            requestObj[QStringLiteral("payload")] = payload;
            requestObj[QStringLiteral("context")] = buildAiProjectContext();

            const auto response = client.submitAiTask(m_aiAuthToken, requestObj);
            if (!response.hasValue()) {
                lastError = QStringLiteral("AI request failed on %1: %2")
                                .arg(model, response.error().message);
                continue;
            }
            const QJsonObject obj = response.value();
            const QJsonObject errObj = obj.value(QStringLiteral("error")).toObject();
            if (!errObj.isEmpty()) {
                const QString code = errObj.value(QStringLiteral("code")).toString().trimmed().toLower();
                const QString message = errObj.value(QStringLiteral("message")).toString().trimmed();
                lastError = QStringLiteral("AI error (%1): %2").arg(code, message);
                const bool retryable =
                    code == QStringLiteral("timeout") ||
                    code == QStringLiteral("rate_limit") ||
                    code == QStringLiteral("service_unavailable");
                if (retryable) {
                    continue;
                }
                break;
            }
            m_aiUsageRequests += 1;
            m_aiRecentRequestEpochMs.push_back(QDateTime::currentMSecsSinceEpoch());
            if (okOut) {
                *okOut = true;
            }
            if (m_aiStatusLabel) {
                m_aiStatusLabel->setText(QStringLiteral("%1 | Usage %2/%3 (fail %4)")
                                             .arg(m_aiIntegrationStatus)
                                             .arg(m_aiUsageRequests)
                                             .arg(m_aiUsageBudgetCap)
                                             .arg(m_aiUsageFailures));
            }
            scheduleSaveState();
            return obj;
        }
    }

    m_aiUsageFailures += 1;
    if (m_aiStatusLabel) {
        m_aiStatusLabel->setText(QStringLiteral("%1 | Usage %2/%3 (fail %4)")
                                     .arg(m_aiIntegrationStatus)
                                     .arg(m_aiUsageRequests)
                                     .arg(m_aiUsageBudgetCap)
                                     .arg(m_aiUsageFailures));
    }
    if (errorOut) {
        *errorOut = lastError.isEmpty() ? QStringLiteral("AI request failed.") : lastError;
    }
    scheduleSaveState();
    return {};
}

void EditorWindow::runAiTranscribeForSelection()
{
    if (!m_timeline || !m_timeline->selectedClip()) {
        QMessageBox::information(this, QStringLiteral("AI Transcribe"), QStringLiteral("Select a clip first."));
        return;
    }
    bool ok = false;
    QString error;
    QJsonObject payload;
    payload[QStringLiteral("clip_file_path")] = m_timeline->selectedClip()->filePath;
    payload[QStringLiteral("clip_label")] = m_timeline->selectedClip()->label;
    const QJsonObject response = runAiAction(QStringLiteral("transcribe_clip"), payload, &ok, &error);
    if (!ok) {
        QMessageBox::warning(this, QStringLiteral("AI Transcribe"), error);
        return;
    }
    QMessageBox::information(this,
                             QStringLiteral("AI Transcribe"),
                             response.value(QStringLiteral("message")).toString(
                                 QStringLiteral("Transcription request submitted.")));
}

void EditorWindow::runAiFindSpeakerNames()
{
    if (!m_featureAiSpeakerCleanup) {
        QMessageBox::information(this,
                                 QStringLiteral("Find Speaker Names (AI)"),
                                 QStringLiteral("Feature disabled: feature_ai_speaker_cleanup=false"));
        return;
    }
    if (!m_speakersTab) {
        return;
    }
    m_speakersTab->runAiFindSpeakerNames();
}

void EditorWindow::runAiFindOrganizations()
{
    if (!m_featureAiSpeakerCleanup) {
        QMessageBox::information(this,
                                 QStringLiteral("Find Organizations (AI)"),
                                 QStringLiteral("Feature disabled: feature_ai_speaker_cleanup=false"));
        return;
    }
    if (!m_speakersTab) {
        return;
    }
    m_speakersTab->runAiFindOrganizations();
}

void EditorWindow::runAiCleanAssignments()
{
    if (!m_featureAiSpeakerCleanup) {
        QMessageBox::information(this,
                                 QStringLiteral("Clean Assignments (AI)"),
                                 QStringLiteral("Feature disabled: feature_ai_speaker_cleanup=false"));
        return;
    }
    if (!m_speakersTab) {
        return;
    }
    m_speakersTab->runAiCleanSpuriousAssignments();
}

void EditorWindow::renderTimelineFromOutputRequest(const RenderRequest &request)
{
    RenderRequest effectiveRequest = request;
    effectiveRequest.correctionsEnabled = m_correctionsEnabled;
    if (effectiveRequest.useProxyMedia)
    {
        for (TimelineClip &clip : effectiveRequest.clips)
        {
            const QString proxyPath = playbackProxyPathForClip(clip);
            if (!proxyPath.isEmpty())
            {
                clip.filePath = proxyPath;
            }
        }
    }

    int64_t totalFramesToRender = 0;
    for (const ExportRangeSegment &range : std::as_const(effectiveRequest.exportRanges))
    {
        totalFramesToRender += qMax<int64_t>(0, range.endFrame - range.startFrame + 1);
    }
    if (totalFramesToRender <= 0)
    {
        totalFramesToRender = qMax<int64_t>(1, effectiveRequest.exportEndFrame - effectiveRequest.exportStartFrame + 1);
    }

    const bool verticalRenderOutput =
        effectiveRequest.outputSize.height() > effectiveRequest.outputSize.width();

    QDialog progressDialog(this);
    progressDialog.setWindowTitle(QStringLiteral("Render Export"));
    progressDialog.setWindowModality(Qt::ApplicationModal);
    progressDialog.setMinimumWidth(verticalRenderOutput ? 920 : 560);
    progressDialog.setStyleSheet(QStringLiteral(
        "QDialog { background: #f6f3ee; }"
        "QLabel { color: #1f2430; font-size: 13px; }"
        "QProgressBar { border: 1px solid #c9c2b8; border-radius: 6px; text-align: center; background: #ffffff; min-height: 20px; }"
        "QProgressBar::chunk { background: #2f7d67; border-radius: 5px; }"
        "QPushButton { min-width: 96px; padding: 6px 14px; }"));
    auto *progressLayout = new QVBoxLayout(&progressDialog);
    progressLayout->setContentsMargins(16, 16, 16, 16);
    progressLayout->setSpacing(10);

    auto *renderPreviewLabel = new QLabel(&progressDialog);
    renderPreviewLabel->setAlignment(Qt::AlignCenter);
    renderPreviewLabel->setMinimumSize(360, 202);
    renderPreviewLabel->setStyleSheet(QStringLiteral(
        "QLabel { background: #11151c; color: #d9e1ea; border: 1px solid #c9c2b8; border-radius: 6px; }"));
    renderPreviewLabel->setText(QStringLiteral("Waiting for first rendered frame..."));

    auto *renderStatusLabel = new QLabel(QStringLiteral("Preparing render..."), &progressDialog);
    renderStatusLabel->setWordWrap(true);
    renderStatusLabel->setAlignment(Qt::AlignCenter);

    auto *showRenderPreviewCheckBox = new QCheckBox(QStringLiteral("Show Visual Preview"), &progressDialog);
    showRenderPreviewCheckBox->setChecked(true);

    auto *renderSourcesLabel = new QLabel(QStringLiteral("Sources In Use (Current Frame)"), &progressDialog);
    renderSourcesLabel->setAlignment(Qt::AlignLeft | Qt::AlignVCenter);

    auto *renderSourcesList = new QPlainTextEdit(&progressDialog);
    renderSourcesList->setReadOnly(true);
    renderSourcesList->setMinimumHeight(140);
    renderSourcesList->setPlainText(QStringLiteral("Waiting for first rendered frame..."));

    if (verticalRenderOutput) {
        auto *contentRow = new QHBoxLayout;
        contentRow->setSpacing(12);

        auto *leftColumn = new QVBoxLayout;
        leftColumn->setSpacing(10);
        leftColumn->addWidget(renderStatusLabel);
        leftColumn->addWidget(showRenderPreviewCheckBox, 0, Qt::AlignLeft);
        leftColumn->addWidget(renderSourcesLabel);
        leftColumn->addWidget(renderSourcesList, 1);

        auto *rightColumn = new QVBoxLayout;
        rightColumn->setSpacing(10);
        rightColumn->addWidget(renderPreviewLabel, 1);

        contentRow->addLayout(leftColumn, 3);
        contentRow->addLayout(rightColumn, 2);
        progressLayout->addLayout(contentRow);
    } else {
        progressLayout->addWidget(renderStatusLabel);
        progressLayout->addWidget(showRenderPreviewCheckBox, 0, Qt::AlignLeft);
        progressLayout->addWidget(renderPreviewLabel);
        progressLayout->addWidget(renderSourcesLabel);
        progressLayout->addWidget(renderSourcesList);
    }

    auto *renderProgressBar = new QProgressBar(&progressDialog);
    renderProgressBar->setRange(0, static_cast<int>(qMin<int64_t>(totalFramesToRender, std::numeric_limits<int>::max())));
    renderProgressBar->setValue(0);
    progressLayout->addWidget(renderProgressBar);

    auto *buttonRow = new QHBoxLayout;
    buttonRow->addStretch(1);
    auto *cancelRenderButton = new QPushButton(QStringLiteral("Cancel"), &progressDialog);
    buttonRow->addWidget(cancelRenderButton);
    progressLayout->addLayout(buttonRow);

    bool renderCancelled = false;
    QObject::connect(cancelRenderButton, &QPushButton::clicked, &progressDialog, [&renderCancelled, cancelRenderButton]() {
        renderCancelled = true;
        cancelRenderButton->setEnabled(false);
    });
    QObject::connect(showRenderPreviewCheckBox, &QCheckBox::toggled, &progressDialog, [renderPreviewLabel](bool checked) {
        renderPreviewLabel->setVisible(checked);
    });
    progressDialog.show();

    const QString outputPath = effectiveRequest.outputPath;
    const auto formatEta = [](qint64 remainingMs) -> QString
    {
        if (remainingMs <= 0)
        {
            return QStringLiteral("calculating...");
        }
        const qint64 totalSeconds = remainingMs / 1000;
        const qint64 hours = totalSeconds / 3600;
        const qint64 minutes = (totalSeconds % 3600) / 60;
        const qint64 seconds = totalSeconds % 60;
        if (hours > 0)
        {
            return QStringLiteral("%1h %2m %3s").arg(hours).arg(minutes).arg(seconds);
        }
        if (minutes > 0)
        {
            return QStringLiteral("%1m %2s").arg(minutes).arg(seconds);
        }
        return QStringLiteral("%1s").arg(seconds);
    };
    const auto stageSummary = [](qint64 stageMs, int64_t completedFrames) -> QString
    {
        if (stageMs <= 0 || completedFrames <= 0)
        {
            return QStringLiteral("0 ms");
        }
        return QStringLiteral("%1 ms total (%2 ms/frame)")
            .arg(stageMs)
            .arg(QString::number(static_cast<double>(stageMs) / static_cast<double>(completedFrames), 'f', 2));
    };
    const auto renderProfileFromProgress = [&formatEta](const RenderProgress &progress) -> QJsonObject
    {
        const qint64 completedFrames = qMax<int64_t>(1, progress.framesCompleted);
        const double fps = progress.elapsedMs > 0
                               ? (1000.0 * static_cast<double>(progress.framesCompleted)) / static_cast<double>(progress.elapsedMs)
                               : 0.0;
        return QJsonObject{
            {QStringLiteral("status"), QStringLiteral("running")},
            {QStringLiteral("output_path"), QString()},
            {QStringLiteral("frames_completed"), static_cast<qint64>(progress.framesCompleted)},
            {QStringLiteral("total_frames"), static_cast<qint64>(progress.totalFrames)},
            {QStringLiteral("segment_index"), progress.segmentIndex},
            {QStringLiteral("segment_count"), progress.segmentCount},
            {QStringLiteral("timeline_frame"), static_cast<qint64>(progress.timelineFrame)},
            {QStringLiteral("segment_start_frame"), static_cast<qint64>(progress.segmentStartFrame)},
            {QStringLiteral("segment_end_frame"), static_cast<qint64>(progress.segmentEndFrame)},
            {QStringLiteral("using_gpu"), progress.usingGpu},
            {QStringLiteral("using_hardware_encode"), progress.usingHardwareEncode},
            {QStringLiteral("encoder_label"), progress.encoderLabel},
            {QStringLiteral("elapsed_ms"), progress.elapsedMs},
            {QStringLiteral("estimated_remaining_ms"), progress.estimatedRemainingMs},
            {QStringLiteral("eta_text"), formatEta(progress.estimatedRemainingMs)},
            {QStringLiteral("fps"), fps},
            {QStringLiteral("render_stage_ms"), progress.renderStageMs},
            {QStringLiteral("render_decode_stage_ms"), progress.renderDecodeStageMs},
            {QStringLiteral("render_texture_stage_ms"), progress.renderTextureStageMs},
            {QStringLiteral("render_composite_stage_ms"), progress.renderCompositeStageMs},
            {QStringLiteral("render_nv12_stage_ms"), progress.renderNv12StageMs},
            {QStringLiteral("gpu_readback_ms"), progress.gpuReadbackMs},
            {QStringLiteral("overlay_stage_ms"), progress.overlayStageMs},
            {QStringLiteral("convert_stage_ms"), progress.convertStageMs},
            {QStringLiteral("encode_stage_ms"), progress.encodeStageMs},
            {QStringLiteral("audio_stage_ms"), progress.audioStageMs},
            {QStringLiteral("max_frame_render_stage_ms"), progress.maxFrameRenderStageMs},
            {QStringLiteral("max_frame_decode_stage_ms"), progress.maxFrameDecodeStageMs},
            {QStringLiteral("max_frame_texture_stage_ms"), progress.maxFrameTextureStageMs},
            {QStringLiteral("max_frame_readback_stage_ms"), progress.maxFrameReadbackStageMs},
            {QStringLiteral("max_frame_convert_stage_ms"), progress.maxFrameConvertStageMs},
            {QStringLiteral("skipped_clips"), progress.skippedClips},
            {QStringLiteral("skipped_clip_reason_counts"), progress.skippedClipReasonCounts},
            {QStringLiteral("render_stage_table"), progress.renderStageTable},
            {QStringLiteral("worst_frame_table"), progress.worstFrameTable},
            {QStringLiteral("render_stage_per_frame_ms"), static_cast<double>(progress.renderStageMs) / static_cast<double>(completedFrames)},
            {QStringLiteral("render_decode_stage_per_frame_ms"), static_cast<double>(progress.renderDecodeStageMs) / static_cast<double>(completedFrames)},
            {QStringLiteral("render_texture_stage_per_frame_ms"), static_cast<double>(progress.renderTextureStageMs) / static_cast<double>(completedFrames)},
            {QStringLiteral("render_composite_stage_per_frame_ms"), static_cast<double>(progress.renderCompositeStageMs) / static_cast<double>(completedFrames)},
            {QStringLiteral("render_nv12_stage_per_frame_ms"), static_cast<double>(progress.renderNv12StageMs) / static_cast<double>(completedFrames)},
            {QStringLiteral("gpu_readback_per_frame_ms"), static_cast<double>(progress.gpuReadbackMs) / static_cast<double>(completedFrames)},
            {QStringLiteral("overlay_stage_per_frame_ms"), static_cast<double>(progress.overlayStageMs) / static_cast<double>(completedFrames)},
            {QStringLiteral("convert_stage_per_frame_ms"), static_cast<double>(progress.convertStageMs) / static_cast<double>(completedFrames)},
            {QStringLiteral("encode_stage_per_frame_ms"), static_cast<double>(progress.encodeStageMs) / static_cast<double>(completedFrames)}};
    };
    const auto renderProfileFromResult = [&formatEta, &outputPath](const RenderResult &result) -> QJsonObject
    {
        const qint64 completedFrames = qMax<int64_t>(1, result.framesRendered);
        const double fps = result.elapsedMs > 0
                               ? (1000.0 * static_cast<double>(result.framesRendered)) / static_cast<double>(result.elapsedMs)
                               : 0.0;
        return QJsonObject{
            {QStringLiteral("status"), result.success ? QStringLiteral("completed")
                                                      : (result.cancelled ? QStringLiteral("cancelled")
                                                                          : QStringLiteral("failed"))},
            {QStringLiteral("output_path"), QDir::toNativeSeparators(outputPath)},
            {QStringLiteral("frames_completed"), static_cast<qint64>(result.framesRendered)},
            {QStringLiteral("total_frames"), static_cast<qint64>(result.framesRendered)},
            {QStringLiteral("segment_index"), 0},
            {QStringLiteral("segment_count"), 0},
            {QStringLiteral("segment_start_frame"), static_cast<qint64>(0)},
            {QStringLiteral("segment_end_frame"), static_cast<qint64>(0)},
            {QStringLiteral("using_gpu"), result.usedGpu},
            {QStringLiteral("using_hardware_encode"), result.usedHardwareEncode},
            {QStringLiteral("encoder_label"), result.encoderLabel},
            {QStringLiteral("elapsed_ms"), result.elapsedMs},
            {QStringLiteral("estimated_remaining_ms"), static_cast<qint64>(0)},
            {QStringLiteral("eta_text"), formatEta(0)},
            {QStringLiteral("fps"), fps},
            {QStringLiteral("render_stage_ms"), result.renderStageMs},
            {QStringLiteral("render_decode_stage_ms"), result.renderDecodeStageMs},
            {QStringLiteral("render_texture_stage_ms"), result.renderTextureStageMs},
            {QStringLiteral("render_composite_stage_ms"), result.renderCompositeStageMs},
            {QStringLiteral("render_nv12_stage_ms"), result.renderNv12StageMs},
            {QStringLiteral("gpu_readback_ms"), result.gpuReadbackMs},
            {QStringLiteral("overlay_stage_ms"), result.overlayStageMs},
            {QStringLiteral("convert_stage_ms"), result.convertStageMs},
            {QStringLiteral("encode_stage_ms"), result.encodeStageMs},
            {QStringLiteral("audio_stage_ms"), result.audioStageMs},
            {QStringLiteral("max_frame_render_stage_ms"), result.maxFrameRenderStageMs},
            {QStringLiteral("max_frame_decode_stage_ms"), result.maxFrameDecodeStageMs},
            {QStringLiteral("max_frame_texture_stage_ms"), result.maxFrameTextureStageMs},
            {QStringLiteral("max_frame_readback_stage_ms"), result.maxFrameReadbackStageMs},
            {QStringLiteral("max_frame_convert_stage_ms"), result.maxFrameConvertStageMs},
            {QStringLiteral("skipped_clips"), result.skippedClips},
            {QStringLiteral("skipped_clip_reason_counts"), result.skippedClipReasonCounts},
            {QStringLiteral("render_stage_table"), result.renderStageTable},
            {QStringLiteral("worst_frame_table"), result.worstFrameTable},
            {QStringLiteral("render_stage_per_frame_ms"), static_cast<double>(result.renderStageMs) / static_cast<double>(completedFrames)},
            {QStringLiteral("render_decode_stage_per_frame_ms"), static_cast<double>(result.renderDecodeStageMs) / static_cast<double>(completedFrames)},
            {QStringLiteral("render_texture_stage_per_frame_ms"), static_cast<double>(result.renderTextureStageMs) / static_cast<double>(completedFrames)},
            {QStringLiteral("render_composite_stage_per_frame_ms"), static_cast<double>(result.renderCompositeStageMs) / static_cast<double>(completedFrames)},
            {QStringLiteral("render_nv12_stage_per_frame_ms"), static_cast<double>(result.renderNv12StageMs) / static_cast<double>(completedFrames)},
            {QStringLiteral("gpu_readback_per_frame_ms"), static_cast<double>(result.gpuReadbackMs) / static_cast<double>(completedFrames)},
            {QStringLiteral("overlay_stage_per_frame_ms"), static_cast<double>(result.overlayStageMs) / static_cast<double>(completedFrames)},
            {QStringLiteral("convert_stage_per_frame_ms"), static_cast<double>(result.convertStageMs) / static_cast<double>(completedFrames)},
            {QStringLiteral("encode_stage_per_frame_ms"), static_cast<double>(result.encodeStageMs) / static_cast<double>(completedFrames)},
            {QStringLiteral("message"), result.message}};
    };
    m_renderInProgress = true;
    m_liveRenderProfile = QJsonObject{
        {QStringLiteral("status"), QStringLiteral("starting")},
        {QStringLiteral("output_path"), QDir::toNativeSeparators(outputPath)},
        {QStringLiteral("frames_completed"), static_cast<qint64>(0)},
        {QStringLiteral("total_frames"), static_cast<qint64>(totalFramesToRender)}};
    refreshProfileInspector();

    const auto activeRenderSourcesText = [&effectiveRequest](int64_t timelineFrame) -> QString {
        QStringList lines;
        lines.reserve(effectiveRequest.clips.size() * 2);
        for (const TimelineClip& clip : effectiveRequest.clips) {
            if (timelineFrame < clip.startFrame ||
                timelineFrame >= clip.startFrame + qMax<int64_t>(1, clip.durationFrames)) {
                continue;
            }
            const QString clipLabel = clip.label.isEmpty() ? QStringLiteral("(unnamed clip)") : clip.label;
            if (clipVisualPlaybackEnabled(clip, effectiveRequest.tracks) && !clip.filePath.isEmpty()) {
                lines.push_back(QStringLiteral("V | %1 | %2")
                                    .arg(clipLabel, QDir::toNativeSeparators(clip.filePath)));
            }
            if (clipAudioPlaybackEnabled(clip)) {
                const QString audioPath = playbackAudioPathForClip(clip);
                if (!audioPath.isEmpty()) {
                    lines.push_back(QStringLiteral("A | %1 | %2")
                                        .arg(clipLabel, QDir::toNativeSeparators(audioPath)));
                }
            }
        }
        if (lines.isEmpty()) {
            return QStringLiteral("No active clip sources at this frame.");
        }
        lines.removeDuplicates();
        std::sort(lines.begin(), lines.end());
        return lines.join(QLatin1Char('\n'));
    };

    const RenderResult result = renderTimelineToFile(
        effectiveRequest,
        [this, &progressDialog, renderStatusLabel, renderProgressBar, renderPreviewLabel,
         renderSourcesList, showRenderPreviewCheckBox, &renderCancelled,
         formatEta, stageSummary, renderProfileFromProgress, outputPath,
         activeRenderSourcesText](const RenderProgress &progress)
        {
            renderProgressBar->setMaximum(qMax(1, static_cast<int>(qMin<int64_t>(progress.totalFrames, std::numeric_limits<int>::max()))));
            renderProgressBar->setValue(static_cast<int>(qMin<int64_t>(progress.framesCompleted, std::numeric_limits<int>::max())));
            const QString rendererMode = progress.usingGpu ? QStringLiteral("GPU render") : QStringLiteral("CPU render");
            const QString encoderMode = progress.usingHardwareEncode
                                            ? QStringLiteral("Hardware encode")
                                            : QStringLiteral("Software encode");
            const QString encoderLabel = progress.encoderLabel.isEmpty()
                                             ? QStringLiteral("unknown")
                                             : progress.encoderLabel;
            m_liveRenderProfile = renderProfileFromProgress(progress);
            m_liveRenderProfile[QStringLiteral("output_path")] = QDir::toNativeSeparators(outputPath);
            refreshProfileInspector();
            const QString metricsTable = QStringLiteral(
                "<table cellspacing='0' cellpadding='2' style='margin: 0 auto;'>"
                "<tr>"
                "<td align='right'><b>Render</b></td><td>%1</td>"
                "<td align='right'><b>Decode</b></td><td>%2</td>"
                "<td align='right'><b>Texture</b></td><td>%3</td>"
                "</tr>"
                "<tr>"
                "<td align='right'><b>Composite</b></td><td>%4</td>"
                "<td align='right'><b>GPU NV12</b></td><td>%5</td>"
                "<td align='right'><b>Readback</b></td><td>%6</td>"
                "</tr>"
                "<tr>"
                "<td align='right'><b>Convert</b></td><td>%7</td>"
                "<td align='right'><b>Encode</b></td><td>%8</td>"
                "<td></td><td></td>"
                "</tr>"
                "</table>")
                .arg(stageSummary(progress.renderStageMs, progress.framesCompleted))
                .arg(stageSummary(progress.renderDecodeStageMs, progress.framesCompleted))
                .arg(stageSummary(progress.renderTextureStageMs, progress.framesCompleted))
                .arg(stageSummary(progress.renderCompositeStageMs, progress.framesCompleted))
                .arg(stageSummary(progress.renderNv12StageMs, progress.framesCompleted))
                .arg(stageSummary(progress.gpuReadbackMs, progress.framesCompleted))
                .arg(stageSummary(progress.convertStageMs, progress.framesCompleted))
                .arg(stageSummary(progress.encodeStageMs, progress.framesCompleted));
            renderStatusLabel->setText(
                QStringLiteral("<b>Rendering frame %1 of %2</b><br>"
                               "Segment %3/%4: %5-%6<br>"
                               "%7 | %8 (%9)<br>"
                               "ETA: %10<br>%11")
                    .arg(progress.framesCompleted + 1)
                    .arg(qMax<int64_t>(1, progress.totalFrames))
                    .arg(progress.segmentIndex)
                    .arg(progress.segmentCount)
                    .arg(progress.segmentStartFrame)
                    .arg(progress.segmentEndFrame)
                    .arg(rendererMode)
                    .arg(encoderMode)
                    .arg(encoderLabel)
                    .arg(formatEta(progress.estimatedRemainingMs))
                    .arg(metricsTable));
            if (showRenderPreviewCheckBox->isChecked() && !progress.previewFrame.isNull())
            {
                const QPixmap pixmap = QPixmap::fromImage(progress.previewFrame).scaled(
                    renderPreviewLabel->size(),
                    Qt::KeepAspectRatio,
                    Qt::SmoothTransformation);
                renderPreviewLabel->setPixmap(pixmap);
                renderPreviewLabel->setText(QString());
            }
            renderSourcesList->setPlainText(activeRenderSourcesText(progress.timelineFrame));
            QCoreApplication::processEvents();
            return !renderCancelled;
        });
    renderProgressBar->setValue(renderProgressBar->maximum());
    progressDialog.close();
    m_renderInProgress = false;
    m_lastRenderProfile = renderProfileFromResult(result);
    m_liveRenderProfile = QJsonObject{};
    refreshProfileInspector();

    if (result.success)
    {
        QMessageBox::information(this, QStringLiteral("Render Complete"), result.message);
        return;
    }

    const QString message = result.message.isEmpty()
                                ? QStringLiteral("Render failed.")
                                : result.message;
    QMessageBox::warning(this,
                         result.cancelled ? QStringLiteral("Render Cancelled") : QStringLiteral("Render Failed"),
                         message);
}

void EditorWindow::exportVideoForSpeakersOnSelectedClip(const QStringList& speakerIds)
{
    if (!m_timeline || speakerIds.isEmpty()) {
        return;
    }
    const TimelineClip* clip = m_timeline->selectedClip();
    if (!clip || clip->durationFrames <= 0) {
        QMessageBox::information(this,
                                 QStringLiteral("Export Video"),
                                 QStringLiteral("Select a clip first."));
        return;
    }

    const QString transcriptPath = activeTranscriptPathForClipFile(clip->filePath);
    QFile transcriptFile(transcriptPath);
    if (!transcriptFile.open(QIODevice::ReadOnly)) {
        QMessageBox::warning(this,
                             QStringLiteral("Export Video"),
                             QStringLiteral("Transcript not found for the selected clip."));
        return;
    }

    QJsonParseError parseError;
    const QJsonDocument transcriptDoc =
        QJsonDocument::fromJson(transcriptFile.readAll(), &parseError);
    if (parseError.error != QJsonParseError::NoError || !transcriptDoc.isObject()) {
        QMessageBox::warning(this,
                             QStringLiteral("Export Video"),
                             QStringLiteral("Transcript JSON is invalid for the selected clip."));
        return;
    }

    auto appendMergedRange = [](QVector<ExportRangeSegment>& ranges, int64_t startFrame, int64_t endFrame) {
        if (endFrame < startFrame) {
            return;
        }
        if (ranges.isEmpty() || startFrame > ranges.constLast().endFrame + 1) {
            ranges.push_back(ExportRangeSegment{startFrame, endFrame});
            return;
        }
        ranges.last().endFrame = qMax(ranges.last().endFrame, endFrame);
    };

    QSet<QString> selectedSpeakerSet;
    selectedSpeakerSet.reserve(speakerIds.size());
    for (const QString& speakerId : speakerIds) {
        const QString trimmed = speakerId.trimmed();
        if (!trimmed.isEmpty()) {
            selectedSpeakerSet.insert(trimmed);
        }
    }
    if (selectedSpeakerSet.isEmpty()) {
        return;
    }

    QVector<ExportRangeSegment> sourceWordRanges;
    const QJsonArray segments = transcriptDoc.object().value(QStringLiteral("segments")).toArray();
    for (const QJsonValue& segmentValue : segments) {
        const QJsonObject segmentObj = segmentValue.toObject();
        const QString segmentSpeaker =
            segmentObj.value(QStringLiteral("speaker")).toString().trimmed();
        const QJsonArray words = segmentObj.value(QStringLiteral("words")).toArray();
        for (const QJsonValue& wordValue : words) {
            const QJsonObject wordObj = wordValue.toObject();
            if (wordObj.value(QStringLiteral("skipped")).toBool(false)) {
                continue;
            }
            const QString wordText = wordObj.value(QStringLiteral("word")).toString().trimmed();
            if (wordText.isEmpty()) {
                continue;
            }
            QString wordSpeaker = wordObj.value(QStringLiteral("speaker")).toString().trimmed();
            if (wordSpeaker.isEmpty()) {
                wordSpeaker = segmentSpeaker;
            }
            if (!selectedSpeakerSet.contains(wordSpeaker)) {
                continue;
            }
            const double startSeconds = wordObj.value(QStringLiteral("start")).toDouble(-1.0);
            const double endSeconds = wordObj.value(QStringLiteral("end")).toDouble(-1.0);
            if (startSeconds < 0.0 || endSeconds < startSeconds) {
                continue;
            }
            const int64_t startFrame =
                qMax<int64_t>(0, static_cast<int64_t>(std::floor(startSeconds * kTimelineFps)));
            const int64_t endFrame =
                qMax<int64_t>(startFrame, static_cast<int64_t>(std::ceil(endSeconds * kTimelineFps)) - 1);
            appendMergedRange(sourceWordRanges, startFrame, endFrame);
        }
    }
    if (sourceWordRanges.isEmpty()) {
        QMessageBox::information(this,
                                 QStringLiteral("Export Video"),
                                 QStringLiteral("No spoken words found for the selected speakers in this clip."));
        return;
    }

    const QVector<RenderSyncMarker> markers = m_timeline->renderSyncMarkers();
    QVector<ExportRangeSegment> timelineRanges;
    timelineRanges.reserve(sourceWordRanges.size());
    int sourceRangeIndex = 0;
    const int64_t clipStartFrame = clip->startFrame;
    const int64_t clipEndFrame = clip->startFrame + clip->durationFrames - 1;
    for (int64_t timelineFrame = clipStartFrame; timelineFrame <= clipEndFrame; ++timelineFrame) {
        const int64_t transcriptFrame = transcriptFrameForClipAtTimelineSample(
            *clip, frameToSamples(timelineFrame), markers);
        while (sourceRangeIndex < sourceWordRanges.size() &&
               sourceWordRanges.at(sourceRangeIndex).endFrame < transcriptFrame) {
            ++sourceRangeIndex;
        }
        if (sourceRangeIndex >= sourceWordRanges.size()) {
            break;
        }
        const ExportRangeSegment& sourceRange = sourceWordRanges.at(sourceRangeIndex);
        if (transcriptFrame < sourceRange.startFrame || transcriptFrame > sourceRange.endFrame) {
            continue;
        }
        appendMergedRange(timelineRanges, timelineFrame, timelineFrame);
    }
    if (timelineRanges.isEmpty()) {
        QMessageBox::information(
            this,
            QStringLiteral("Export Video"),
            QStringLiteral("Could not map selected speaker words to timeline frames."));
        return;
    }

    setPlaybackActive(false);

    RenderRequest request = buildRenderRequestFromOutputControls();
    const QString suggestedBase = QStringLiteral("speaker_export_%1")
        .arg(selectedSpeakerSet.size() == 1 ? *selectedSpeakerSet.constBegin()
                                            : QStringLiteral("multi"));
    const QString selectedPath = QFileDialog::getSaveFileName(
        this,
        QStringLiteral("Export Video"),
        QDir::current().filePath(QStringLiteral("%1.%2").arg(suggestedBase, request.outputFormat)),
        QStringLiteral("Video Files (*.%1);;All Files (*)").arg(request.outputFormat));
    if (selectedPath.isEmpty()) {
        return;
    }

    request.outputPath = selectedPath;
    m_lastRenderOutputPath = selectedPath;
    scheduleSaveState();

    request.clips = m_timeline->clips();
    request.tracks = m_timeline->tracks();
    request.renderSyncMarkers = markers;
    request.exportRanges = timelineRanges;
    request.exportStartFrame = timelineRanges.constFirst().startFrame;
    request.exportEndFrame = timelineRanges.constLast().endFrame;
    renderTimelineFromOutputRequest(request);
}


namespace {

bool zeroCopyPreferredEnvironmentDetected() {
#if defined(Q_OS_LINUX)
    return QFile::exists(QStringLiteral("/proc/driver/nvidia/version")) ||
           QFile::exists(QStringLiteral("/sys/module/nvidia")) ||
           QFile::exists(QStringLiteral("/dev/dri/renderD128"));
#else
    return false;
#endif
}

}

int main(int argc, char **argv)
{
    bool runHeadlessSpeakerHarness = false;
    for (int i = 1; i < argc; ++i) {
        if (qstrcmp(argv[i], "--speaker-export-harness") == 0) {
            runHeadlessSpeakerHarness = true;
            break;
        }
    }
    if (runHeadlessSpeakerHarness &&
        qEnvironmentVariableIsEmpty("QT_QPA_PLATFORM") &&
        qEnvironmentVariableIsEmpty("DISPLAY") &&
        qEnvironmentVariableIsEmpty("WAYLAND_DISPLAY")) {
        qputenv("QT_QPA_PLATFORM", "offscreen");
    }

    QApplication app(argc, argv);
    QApplication::setApplicationName(QStringLiteral("PanelTalkEditor"));
    qRegisterMetaType<editor::FrameHandle>();

    // Single instance enforcement via lock file
    const QString lockPath = QDir::tempPath() + QStringLiteral("/PanelTalkEditor.lock");
    QLockFile lockFile(lockPath);
    lockFile.setStaleLockTime(0);
    if (!lockFile.tryLock(100)) {
        qint64 pid = 0;
        QString hostname, appname;
        lockFile.getLockInfo(&pid, &hostname, &appname);
        fprintf(stderr, "Another instance is already running (pid %lld). Exiting.\n",
                static_cast<long long>(pid));
        return 1;
    }

    if (!zeroCopyPreferredEnvironmentDetected()) {
        qWarning().noquote() << QStringLiteral(
            "[STARTUP][WARN] Preferred zero-copy decode path requires Linux GPU interop "
            "(CUDA or VAAPI render node); falling back to hardware CPU-upload or software decode.");
    }

    QCommandLineParser parser;
    parser.setApplicationDescription(QStringLiteral("PanelVid2TikTok editor"));
    parser.addHelpOption();
    QCommandLineOption debugPlaybackOption(QStringLiteral("debug-playback"),
                                           QStringLiteral("Enable playback debug logging"));
    QCommandLineOption debugCacheOption(QStringLiteral("debug-cache"),
                                        QStringLiteral("Enable cache debug logging"));
    QCommandLineOption debugDecodeOption(QStringLiteral("debug-decode"),
                                         QStringLiteral("Enable decode debug logging"));
    QCommandLineOption debugAllOption(QStringLiteral("debug-all"),
                                      QStringLiteral("Enable all debug logging"));
    QCommandLineOption controlPortOption(
        QStringList{QStringLiteral("control-port")},
        QStringLiteral("Control server port."),
        QStringLiteral("port"));
    QCommandLineOption noRestOption(
        QStringList{QStringLiteral("no-rest")},
        QStringLiteral("Disable the local REST/control server."));
    QCommandLineOption speakerHarnessOption(
        QStringList{QStringLiteral("speaker-export-harness")},
        QStringLiteral("Run speaker export harness without showing the main window."));
    QCommandLineOption stateOption(
        QStringList{QStringLiteral("state")},
        QStringLiteral("Path to state JSON for harness mode."),
        QStringLiteral("path"));
    QCommandLineOption outputOption(
        QStringList{QStringLiteral("output")},
        QStringLiteral("Output path for harness mode."),
        QStringLiteral("path"));
    QCommandLineOption speakerOption(
        QStringList{QStringLiteral("speaker-id")},
        QStringLiteral("Speaker id(s) for harness mode. Repeat or use comma-separated values."),
        QStringLiteral("id"));
    QCommandLineOption clipOption(
        QStringList{QStringLiteral("clip-id")},
        QStringLiteral("Clip id override for harness mode."),
        QStringLiteral("id"));
    QCommandLineOption formatOption(
        QStringList{QStringLiteral("format")},
        QStringLiteral("Output format override for harness mode."),
        QStringLiteral("format"));
    QCommandLineOption widthOption(
        QStringList{QStringLiteral("width")},
        QStringLiteral("Output width override for harness mode."),
        QStringLiteral("pixels"));
    QCommandLineOption heightOption(
        QStringList{QStringLiteral("height")},
        QStringLiteral("Output height override for harness mode."),
        QStringLiteral("pixels"));
    QCommandLineOption useProxyOption(
        QStringList{QStringLiteral("use-proxy")},
        QStringLiteral("Force proxy rendering in harness mode."));
    QCommandLineOption noProxyOption(
        QStringList{QStringLiteral("no-proxy")},
        QStringLiteral("Disable proxy rendering in harness mode."));
    parser.addOption(debugPlaybackOption);
    parser.addOption(debugCacheOption);
    parser.addOption(debugDecodeOption);
    parser.addOption(debugAllOption);
    parser.addOption(controlPortOption);
    parser.addOption(noRestOption);
    parser.addOption(speakerHarnessOption);
    parser.addOption(stateOption);
    parser.addOption(outputOption);
    parser.addOption(speakerOption);
    parser.addOption(clipOption);
    parser.addOption(formatOption);
    parser.addOption(widthOption);
    parser.addOption(heightOption);
    parser.addOption(useProxyOption);
    parser.addOption(noProxyOption);
    parser.process(app);

    if (parser.isSet(debugAllOption)) {
        editor::setDebugPlaybackEnabled(true);
        editor::setDebugCacheEnabled(true);
        editor::setDebugDecodeEnabled(true);
    } else {
        if (parser.isSet(debugPlaybackOption)) {
            editor::setDebugPlaybackEnabled(true);
        }
        if (parser.isSet(debugCacheOption)) {
            editor::setDebugCacheEnabled(true);
        }
        if (parser.isSet(debugDecodeOption)) {
            editor::setDebugDecodeEnabled(true);
        }
    }

    bool portOk = false;
    quint16 controlPort = 40130;
    const QString optionValue = parser.value(controlPortOption);
    if (!optionValue.isEmpty()) {
        const uint parsed = optionValue.toUInt(&portOk);
        if (portOk && parsed <= std::numeric_limits<quint16>::max()) {
            controlPort = static_cast<quint16>(parsed);
        }
    } else {
        const QString envValue = qEnvironmentVariable("EDITOR_CONTROL_PORT");
        const uint parsed = envValue.toUInt(&portOk);
        if (portOk && parsed <= std::numeric_limits<quint16>::max()) {
            controlPort = static_cast<quint16>(parsed);
        }
    }

    if (parser.isSet(noRestOption)) {
        controlPort = 0;
    }

    if (parser.isSet(speakerHarnessOption)) {
        SpeakerExportHarnessConfig config;
        config.statePath = parser.value(stateOption);
        config.outputPath = parser.value(outputOption);
        config.outputFormat = parser.value(formatOption);
        config.clipId = parser.value(clipOption);
        config.speakerIds = parser.values(speakerOption);
        if (parser.isSet(widthOption) || parser.isSet(heightOption)) {
            bool widthOk = false;
            bool heightOk = false;
            const int parsedWidth = parser.value(widthOption).toInt(&widthOk);
            const int parsedHeight = parser.value(heightOption).toInt(&heightOk);
            config.outputSize = QSize(widthOk ? parsedWidth : 1080,
                                      heightOk ? parsedHeight : 1920);
            config.outputSizeOverride = true;
        }
        if (parser.isSet(useProxyOption) || parser.isSet(noProxyOption)) {
            config.useProxyOverride = true;
            config.useProxyMedia = parser.isSet(useProxyOption) && !parser.isSet(noProxyOption);
        }
        return runSpeakerExportHarness(config);
    }

    EditorWindow window(controlPort);
    window.show();
    return app.exec();
}
