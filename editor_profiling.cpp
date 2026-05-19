#include "editor.h"
#include "opengl_preview_debug.h"
#include "debug_controls.h"

#include <QSet>
#include <limits>
#include <algorithm>

using namespace editor;

namespace {

QJsonArray pipelineStagesToJson(const QVector<PreviewSurface::PipelineStageSnapshot>& stages)
{
    QJsonArray array;
    for (int i = 0; i < stages.size(); ++i) {
        const PreviewSurface::PipelineStageSnapshot& stage = stages.at(i);
        array.push_back(QJsonObject{
            {QStringLiteral("index"), i},
            {QStringLiteral("label"), stage.label},
            {QStringLiteral("detail"), stage.detail},
            {QStringLiteral("kind"), stage.kind},
            {QStringLiteral("exact"), stage.exact},
            {QStringLiteral("active"), stage.active},
            {QStringLiteral("state"), stage.state.isEmpty()
                 ? (stage.active
                        ? (stage.exact ? QStringLiteral("ready") : QStringLiteral("approximate"))
                        : QStringLiteral("waiting"))
                 : stage.state},
            {QStringLiteral("has_image"), !stage.image.isNull()},
            {QStringLiteral("image_size"), stage.image.isNull()
                 ? QString()
                 : QStringLiteral("%1x%2").arg(stage.image.width()).arg(stage.image.height())},
            {QStringLiteral("facts"), stage.facts}
        });
    }
    return array;
}

} // namespace

QJsonObject EditorWindow::startupProfileSnapshot() const
{
    const qint64 elapsedMs =
        m_startupProfileCompleted && m_startupProfileCompletedMs >= 0
            ? m_startupProfileCompletedMs
            : (m_startupProfileTimer.isValid() ? m_startupProfileTimer.elapsed() : 0);

    QVector<QPair<qint64, QString>> phaseCosts;
    phaseCosts.reserve(m_startupProfileEvents.size());
    for (const QJsonValue& markValue : m_startupProfileEvents) {
        const QJsonObject mark = markValue.toObject();
        const qint64 deltaMs = mark.value(QStringLiteral("delta_ms")).toInteger(-1);
        const QString phase = mark.value(QStringLiteral("phase")).toString();
        if (deltaMs >= 0 && !phase.isEmpty()) {
            phaseCosts.push_back(qMakePair(deltaMs, phase));
        }
    }
    std::sort(phaseCosts.begin(), phaseCosts.end(),
              [](const QPair<qint64, QString>& a, const QPair<qint64, QString>& b) {
                  if (a.first != b.first) {
                      return a.first > b.first;
                  }
                  return a.second < b.second;
              });
    QJsonArray topPhases;
    const int topCount = qMin(8, phaseCosts.size());
    for (int i = 0; i < topCount; ++i) {
        topPhases.push_back(QJsonObject{
            {QStringLiteral("phase"), phaseCosts.at(i).second},
            {QStringLiteral("delta_ms"), phaseCosts.at(i).first}
        });
    }

    return QJsonObject{
        {QStringLiteral("completed"), m_startupProfileCompleted},
        {QStringLiteral("total_ms"), elapsedMs},
        {QStringLiteral("mark_count"), m_startupProfileEvents.size()},
        {QStringLiteral("marks"), m_startupProfileEvents},
        {QStringLiteral("top_phases"), topPhases}
    };
}

QJsonObject EditorWindow::profilingSnapshot() const
{
    const qint64 now = nowMs();
    QJsonObject snapshot{
        {QStringLiteral("playback_active"), m_playbackTimer.isActive()},
        {QStringLiteral("timeline_clip_count"), m_timeline ? m_timeline->clips().size() : 0},
        {QStringLiteral("current_frame"), m_timeline ? static_cast<qint64>(m_timeline->currentFrame()) : 0},
        {QStringLiteral("absolute_playback_sample"), static_cast<qint64>(m_absolutePlaybackSample)},
        {QStringLiteral("filtered_playback_sample"), static_cast<qint64>(m_filteredPlaybackSample)},
        {QStringLiteral("explorer_root"), m_explorerPane ? m_explorerPane->currentRootPath() : QString()},
        {QStringLiteral("debug"), debugControlsSnapshot()},
        {QStringLiteral("main_thread_heartbeat_ms"), m_lastMainThreadHeartbeatMs.load()},
        {QStringLiteral("last_playhead_advance_ms"), m_lastPlayheadAdvanceMs.load()},
        {QStringLiteral("main_thread_heartbeat_age_ms"), m_lastMainThreadHeartbeatMs.load() > 0 ? now - m_lastMainThreadHeartbeatMs.load() : -1},
        {QStringLiteral("last_playhead_advance_age_ms"), m_lastPlayheadAdvanceMs.load() > 0 ? now - m_lastPlayheadAdvanceMs.load() : -1},
        {QStringLiteral("last_seek_update_duration_ms"), m_lastSetCurrentPlaybackSampleDurationMs.load()},
        {QStringLiteral("max_seek_update_duration_ms"), m_maxSetCurrentPlaybackSampleDurationMs.load()},
        {QStringLiteral("slow_seek_update_count"), m_setCurrentPlaybackSampleSlowCount.load()},
        {QStringLiteral("last_inspector_refresh_duration_ms"), m_lastInspectorRefreshDurationMs.load()},
        {QStringLiteral("max_inspector_refresh_duration_ms"), m_maxInspectorRefreshDurationMs.load()},
        {QStringLiteral("slow_inspector_refresh_count"), m_inspectorRefreshSlowCount.load()},
        {QStringLiteral("last_playback_stop_reason"), m_lastPlaybackStopReason}};

    if (m_preview) {
        snapshot[QStringLiteral("preview")] = m_preview->profilingSnapshot();
    }

    if (m_audioEngine) {
        snapshot[QStringLiteral("audio")] = m_audioEngine->profilingSnapshot();
    }

    snapshot[QStringLiteral("startup")] = startupProfileSnapshot();
    snapshot[QStringLiteral("optimized_profile")] = optimizedProfileSnapshot();
    snapshot[QStringLiteral("export")] = QJsonObject{
        {QStringLiteral("active"), m_renderInProgress},
        {QStringLiteral("live"), m_liveRenderProfile},
        {QStringLiteral("last"), m_lastRenderProfile}};
    snapshot[QStringLiteral("speaker_tracking")] = transcriptSpeakerTrackingProfilingSnapshot();
    snapshot[QStringLiteral("speakers_refresh")] = m_speakersTab
        ? QJsonObject{
              {QStringLiteral("last_speakers_table_refresh_duration_ms"),
               m_speakersTab->lastSpeakersTableRefreshDurationMs()},
              {QStringLiteral("max_speakers_table_refresh_duration_ms"),
               m_speakersTab->maxSpeakersTableRefreshDurationMs()},
              {QStringLiteral("last_facestream_panel_refresh_duration_ms"),
               m_speakersTab->lastFaceStreamPanelRefreshDurationMs()},
              {QStringLiteral("max_facestream_panel_refresh_duration_ms"),
               m_speakersTab->maxFaceStreamPanelRefreshDurationMs()},
              {QStringLiteral("last_raw_detections_panel_refresh_duration_ms"),
               m_speakersTab->lastRawDetectionsPanelRefreshDurationMs()},
              {QStringLiteral("max_raw_detections_panel_refresh_duration_ms"),
               m_speakersTab->maxRawDetectionsPanelRefreshDurationMs()}}
        : QJsonObject{};

    return snapshot;
}

QJsonObject EditorWindow::pipelineSnapshot() const
{
    if (!m_preview) {
        return QJsonObject{};
    }
    QJsonObject preview = m_preview->profilingSnapshot();
    preview.insert(QStringLiteral("pipeline_stages"),
                   pipelineStagesToJson(m_preview->livePipelineSnapshots()));
    return preview;
}

QJsonObject EditorWindow::throttleConfigSnapshot() const
{
    const QJsonObject speakerTrackingConfig = transcriptSpeakerTrackingConfigSnapshot();
    return QJsonObject{
        {QStringLiteral("ok"), true},
        {QStringLiteral("playback_ui_sync_min_interval_ms"), m_playbackUiSyncMinIntervalMs},
        {QStringLiteral("playback_state_save_min_interval_ms"), m_playbackStateSaveMinIntervalMs},
        {QStringLiteral("slow_seek_warn_threshold_ms"), m_slowSeekWarnThresholdMs},
        {QStringLiteral("playback_start_lookahead_frames"), m_playbackStartLookaheadFrames},
        {QStringLiteral("playback_start_lookahead_timeout_ms"), m_playbackStartLookaheadTimeoutMs},
        {QStringLiteral("main_thread_heartbeat_interval_ms"), m_mainThreadHeartbeatIntervalMs},
        {QStringLiteral("state_save_debounce_interval_ms"), m_stateSaveDebounceIntervalMs},
        {QStringLiteral("transcript_manual_selection_hold_ms"), m_transcriptManualSelectionHoldMs},
        {QStringLiteral("audio_clock_stall_threshold_ticks"), m_audioClockStallThresholdTicks},
        {QStringLiteral("preview_visible_backlog_limit"),
         m_preview ? m_preview->playbackTuning().visibleBacklogLimit : defaultOptimizedPreviewProfile().previewTuning.visibleBacklogLimit},
        {QStringLiteral("preview_source_lookahead_frames"),
         m_preview ? m_preview->playbackTuning().sourceLookaheadFrames : defaultOptimizedPreviewProfile().previewTuning.sourceLookaheadFrames},
        {QStringLiteral("preview_proxy_lookahead_frames"),
         m_preview ? m_preview->playbackTuning().proxyLookaheadFrames : defaultOptimizedPreviewProfile().previewTuning.proxyLookaheadFrames},
        {QStringLiteral("optimized_profile_path"), optimizedProfilePath()},
        {QStringLiteral("optimized_profile_loaded"), m_optimizedProfileLoaded},
        {QStringLiteral("optimized_profile_generated_this_run"), m_optimizedProfileGeneratedThisRun},
        {QStringLiteral("speaker_tracking_max_speed_permille_per_frame"),
         speakerTrackingConfig.value(QStringLiteral("max_speed_permille_per_frame")).toInt(40)},
        {QStringLiteral("speaker_tracking_smoothing_permille"),
         speakerTrackingConfig.value(QStringLiteral("smoothing_permille")).toInt(800)},
        {QStringLiteral("speaker_tracking_kalman_enabled"),
         speakerTrackingConfig.value(QStringLiteral("kalman_enabled")).toBool(false)},
        {QStringLiteral("speaker_tracking_kalman_process_noise_permille"),
         speakerTrackingConfig.value(QStringLiteral("kalman_process_noise_permille")).toInt(120)},
        {QStringLiteral("speaker_tracking_kalman_measurement_noise_permille"),
         speakerTrackingConfig.value(QStringLiteral("kalman_measurement_noise_permille")).toInt(350)},
        {QStringLiteral("speaker_tracking_auto_track_step_frames"),
         speakerTrackingConfig.value(QStringLiteral("auto_track_step_frames")).toInt(6)}
    };
}

QJsonObject EditorWindow::applyThrottleConfigPatch(const QJsonObject& patch)
{
    auto parsePositiveInt = [](const QJsonObject& obj, const QString& key, int* target, QString* error) -> bool {
        if (!obj.contains(key)) return true;
        bool ok = false;
        const qint64 value = obj.value(key).toVariant().toLongLong(&ok);
        if (!ok || value <= 0 || value > std::numeric_limits<int>::max()) {
            if (error) *error = QStringLiteral("%1 must be a positive integer").arg(key);
            return false;
        }
        *target = static_cast<int>(value);
        return true;
    };
    auto parsePositiveMs = [](const QJsonObject& obj, const QString& key, qint64* target, QString* error) -> bool {
        if (!obj.contains(key)) return true;
        bool ok = false;
        const qint64 value = obj.value(key).toVariant().toLongLong(&ok);
        if (!ok || value <= 0) {
            if (error) *error = QStringLiteral("%1 must be a positive integer").arg(key);
            return false;
        }
        *target = value;
        return true;
    };

    QString error;
    int previewVisibleBacklogLimit =
        m_preview ? m_preview->playbackTuning().visibleBacklogLimit : defaultOptimizedPreviewProfile().previewTuning.visibleBacklogLimit;
    int previewSourceLookaheadFrames =
        m_preview ? m_preview->playbackTuning().sourceLookaheadFrames : defaultOptimizedPreviewProfile().previewTuning.sourceLookaheadFrames;
    int previewProxyLookaheadFrames =
        m_preview ? m_preview->playbackTuning().proxyLookaheadFrames : defaultOptimizedPreviewProfile().previewTuning.proxyLookaheadFrames;
    if (!parsePositiveMs(patch, QStringLiteral("playback_ui_sync_min_interval_ms"), &m_playbackUiSyncMinIntervalMs, &error) ||
        !parsePositiveMs(patch, QStringLiteral("playback_state_save_min_interval_ms"), &m_playbackStateSaveMinIntervalMs, &error) ||
        !parsePositiveMs(patch, QStringLiteral("slow_seek_warn_threshold_ms"), &m_slowSeekWarnThresholdMs, &error) ||
        !parsePositiveInt(patch, QStringLiteral("playback_start_lookahead_frames"), &m_playbackStartLookaheadFrames, &error) ||
        !parsePositiveInt(patch, QStringLiteral("playback_start_lookahead_timeout_ms"), &m_playbackStartLookaheadTimeoutMs, &error) ||
        !parsePositiveInt(patch, QStringLiteral("main_thread_heartbeat_interval_ms"), &m_mainThreadHeartbeatIntervalMs, &error) ||
        !parsePositiveInt(patch, QStringLiteral("state_save_debounce_interval_ms"), &m_stateSaveDebounceIntervalMs, &error) ||
        !parsePositiveInt(patch, QStringLiteral("transcript_manual_selection_hold_ms"), &m_transcriptManualSelectionHoldMs, &error) ||
        !parsePositiveInt(patch, QStringLiteral("audio_clock_stall_threshold_ticks"), &m_audioClockStallThresholdTicks, &error) ||
        !parsePositiveInt(patch, QStringLiteral("preview_visible_backlog_limit"), &previewVisibleBacklogLimit, &error) ||
        !parsePositiveInt(patch, QStringLiteral("preview_source_lookahead_frames"), &previewSourceLookaheadFrames, &error) ||
        !parsePositiveInt(patch, QStringLiteral("preview_proxy_lookahead_frames"), &previewProxyLookaheadFrames, &error)) {
        return QJsonObject{
            {QStringLiteral("ok"), false},
            {QStringLiteral("error"), error}
        };
    }
    if (m_preview) {
        PreviewSurface::PlaybackTuning tuning = m_preview->playbackTuning();
        tuning.visibleBacklogLimit = previewVisibleBacklogLimit;
        tuning.sourceLookaheadFrames = previewSourceLookaheadFrames;
        tuning.proxyLookaheadFrames = previewProxyLookaheadFrames;
        m_preview->setPlaybackTuning(tuning);
    }
    QJsonObject speakerTrackingPatch;
    if (patch.contains(QStringLiteral("speaker_tracking_max_speed_permille_per_frame"))) {
        speakerTrackingPatch[QStringLiteral("max_speed_permille_per_frame")] =
            patch.value(QStringLiteral("speaker_tracking_max_speed_permille_per_frame"));
    }
    if (patch.contains(QStringLiteral("speaker_tracking_smoothing_permille"))) {
        speakerTrackingPatch[QStringLiteral("smoothing_permille")] =
            patch.value(QStringLiteral("speaker_tracking_smoothing_permille"));
    }
    if (patch.contains(QStringLiteral("speaker_tracking_kalman_enabled"))) {
        speakerTrackingPatch[QStringLiteral("kalman_enabled")] =
            patch.value(QStringLiteral("speaker_tracking_kalman_enabled"));
    }
    if (patch.contains(QStringLiteral("speaker_tracking_kalman_process_noise_permille"))) {
        speakerTrackingPatch[QStringLiteral("kalman_process_noise_permille")] =
            patch.value(QStringLiteral("speaker_tracking_kalman_process_noise_permille"));
    }
    if (patch.contains(QStringLiteral("speaker_tracking_kalman_measurement_noise_permille"))) {
        speakerTrackingPatch[QStringLiteral("kalman_measurement_noise_permille")] =
            patch.value(QStringLiteral("speaker_tracking_kalman_measurement_noise_permille"));
    }
    if (patch.contains(QStringLiteral("speaker_tracking_auto_track_step_frames"))) {
        speakerTrackingPatch[QStringLiteral("auto_track_step_frames")] =
            patch.value(QStringLiteral("speaker_tracking_auto_track_step_frames"));
    }
    if (!speakerTrackingPatch.isEmpty() &&
        !applyTranscriptSpeakerTrackingConfigPatch(speakerTrackingPatch, &error)) {
        return QJsonObject{
            {QStringLiteral("ok"), false},
            {QStringLiteral("error"), error}
        };
    }

    m_mainThreadHeartbeatTimer.setInterval(m_mainThreadHeartbeatIntervalMs);
    m_stateSaveTimer.setInterval(m_stateSaveDebounceIntervalMs);
    if (m_transcriptTab) {
        m_transcriptTab->setManualSelectionHoldMs(m_transcriptManualSelectionHoldMs);
    }
    return throttleConfigSnapshot();
}

QJsonObject EditorWindow::playbackConfigSnapshot() const
{
    const PlaybackRuntimeConfig config = playbackRuntimeConfig();
    return QJsonObject{
        {QStringLiteral("ok"), true},
        {QStringLiteral("playback_speed"), config.speed},
        {QStringLiteral("clock_source"), playbackClockSourceToString(config.clockSource)},
        {QStringLiteral("audio_warp_mode"), playbackAudioWarpModeToString(config.audioWarpMode)},
        {QStringLiteral("playback_loop_enabled"), config.loopEnabled},
        {QStringLiteral("last_playback_stop_reason"), m_lastPlaybackStopReason}
    };
}

QJsonObject EditorWindow::applyPlaybackConfigPatch(const QJsonObject& patch)
{
    QString error;

    if (patch.contains(QStringLiteral("playback_speed"))) {
        bool ok = false;
        const qreal speed = patch.value(QStringLiteral("playback_speed")).toVariant().toDouble(&ok);
        if (!ok || speed <= 0.0) {
            error = QStringLiteral("playback_speed must be a positive number");
            return QJsonObject{
                {QStringLiteral("ok"), false},
                {QStringLiteral("error"), error}
            };
        }
        setPlaybackSpeed(speed);
    }

    if (patch.contains(QStringLiteral("clock_source"))) {
        const QString raw = patch.value(QStringLiteral("clock_source")).toString().trimmed().toLower();
        static const QSet<QString> validValues = {
            QStringLiteral("auto"),
            QStringLiteral("audio"),
            QStringLiteral("timeline")
        };
        if (!validValues.contains(raw)) {
            error = QStringLiteral("clock_source must be one of: auto, audio, timeline");
            return QJsonObject{
                {QStringLiteral("ok"), false},
                {QStringLiteral("error"), error}
            };
        }
        setPlaybackClockSource(playbackClockSourceFromString(raw));
    }

    if (patch.contains(QStringLiteral("audio_warp_mode"))) {
        const QString raw = patch.value(QStringLiteral("audio_warp_mode")).toString().trimmed().toLower();
        static const QSet<QString> validValues = {
            QStringLiteral("disabled"),
            QStringLiteral("varispeed"),
            QStringLiteral("time_stretch"),
            QStringLiteral("time-stretch")
        };
        if (!validValues.contains(raw)) {
            error = QStringLiteral("audio_warp_mode must be one of: disabled, varispeed, time_stretch");
            return QJsonObject{
                {QStringLiteral("ok"), false},
                {QStringLiteral("error"), error}
            };
        }
        setPlaybackAudioWarpMode(playbackAudioWarpModeFromString(raw));
    }

    if (patch.contains(QStringLiteral("playback_loop_enabled"))) {
        if (!patch.value(QStringLiteral("playback_loop_enabled")).isBool()) {
            error = QStringLiteral("playback_loop_enabled must be a boolean");
            return QJsonObject{
                {QStringLiteral("ok"), false},
                {QStringLiteral("error"), error}
            };
        }
        PlaybackRuntimeConfig config = playbackRuntimeConfig();
        config.loopEnabled = patch.value(QStringLiteral("playback_loop_enabled")).toBool();
        applyPlaybackRuntimeConfig(config);
    }

    return playbackConfigSnapshot();
}
