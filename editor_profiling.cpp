#include "editor.h"
#include "debug_controls.h"
#include "playback_debug.h"

#include <QDateTime>
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

QJsonObject EditorWindow::playbackStageMetricsSnapshot() const
{
    QJsonObject stages;
    stages.insert(QStringLiteral("clock_update"),
                  editor::playbackStageMetricToJson(m_playbackClockStageMetric,
                                            QStringLiteral("editor")));
    stages.insert(QStringLiteral("playback_sample_apply"),
                  editor::playbackStageMetricToJson(m_playbackSampleApplyStageMetric,
                                            QStringLiteral("editor")));
    if (m_preview) {
        mergePlaybackStageMetricObjects(
            &stages,
            m_preview->profilingSnapshot().value(QStringLiteral("playback_pipeline_stages")).toObject());
    }
    return stages;
}

void appendRuntimePatch(QJsonArray* log,
                        qint64* sequence,
                        const QString& domain,
                        const QJsonObject& patch)
{
    if (!log || !sequence || patch.isEmpty()) {
        return;
    }
    ++(*sequence);
    log->push_back(QJsonObject{
        {QStringLiteral("sequence"), *sequence},
        {QStringLiteral("domain"), domain},
        {QStringLiteral("applied_utc_ms"), QDateTime::currentDateTimeUtc().toMSecsSinceEpoch()},
        {QStringLiteral("patch"), patch}
    });
    constexpr int kRuntimePatchLogLimit = 32;
    while (log->size() > kRuntimePatchLogLimit) {
        log->removeAt(0);
    }
}

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

QJsonObject EditorWindow::startupReadinessSnapshot() const
{
    std::lock_guard<std::mutex> lock(m_startupReadinessMutex);
    QJsonObject snapshot = m_startupReadinessSnapshot;
    const qint64 elapsedMs =
        m_startupProfileCompleted && m_startupProfileCompletedMs >= 0
            ? m_startupProfileCompletedMs
            : (m_startupProfileTimer.isValid() ? m_startupProfileTimer.elapsed() : 0);
    snapshot[QStringLiteral("completed")] = m_startupProfileCompleted;
    snapshot[QStringLiteral("elapsed_ms")] = elapsedMs;
    snapshot[QStringLiteral("total_ms")] = elapsedMs;
    snapshot[QStringLiteral("ready_to_play")] =
        snapshot.value(QStringLiteral("readiness")).toObject()
            .value(QStringLiteral("startup_load_complete")).toBool(false);
    snapshot[QStringLiteral("note")] = QStringLiteral(
        "Cheap startup/readiness trace. It is safe for REST fast snapshots and does not require a live UI profile.");
    return snapshot;
}

QJsonObject EditorWindow::profilingSnapshot() const
{
    const qint64 now = nowMs();
    QJsonObject snapshot{
        {QStringLiteral("playback_active"), m_playbackTimer.isActive()},
        {QStringLiteral("timeline_clip_count"), m_timeline ? m_timeline->clips().size() : 0},
        {QStringLiteral("current_frame"), m_timeline ? static_cast<qint64>(m_timeline->currentFrame()) : 0},
        {QStringLiteral("transport_timeline_sample"), static_cast<qint64>(m_transportTimelineSample)},
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
        QJsonObject audio = m_audioEngine->profilingSnapshot();
        const int64_t projectedAudioSample =
            timelineSampleForAudioFeedbackSample(qMax<int64_t>(0, m_audioEngine->playbackClockSample()));
        audio[QStringLiteral("projected_audio_feedback_timeline_sample")] =
            static_cast<qint64>(projectedAudioSample);
        audio[QStringLiteral("projected_audio_feedback_timeline_frame")] =
            static_cast<qint64>(std::floor(samplesToFramePosition(projectedAudioSample)));
        audio[QStringLiteral("audio_video_drift_samples")] =
            static_cast<qint64>(m_transportTimelineSample - projectedAudioSample);
        audio[QStringLiteral("audio_video_drift_frames")] =
            static_cast<qint64>(std::floor(samplesToFramePosition(m_transportTimelineSample)) -
                                std::floor(samplesToFramePosition(projectedAudioSample)));
        snapshot[QStringLiteral("audio")] = audio;
    }

    snapshot[QStringLiteral("stream_timing")] = streamTimingSnapshot();
    snapshot[QStringLiteral("startup")] = startupProfileSnapshot();
    snapshot[QStringLiteral("startup_readiness")] = startupReadinessSnapshot();
    snapshot[QStringLiteral("startup_optimization")] = startupOptimizationSnapshot();
    snapshot[QStringLiteral("runtime_patches")] = runtimePatchesSnapshot();
    snapshot[QStringLiteral("optimized_profile")] = optimizedProfileSnapshot();
    snapshot[QStringLiteral("export")] = QJsonObject{
        {QStringLiteral("active"), m_renderInProgress},
        {QStringLiteral("live"), m_liveRenderProfile},
        {QStringLiteral("last"), m_lastRenderProfile}};
    snapshot[QStringLiteral("playback_pipeline_stages")] = playbackStageMetricsSnapshot();
    snapshot[QStringLiteral("speaker_tracking")] = transcriptSpeakerTrackingProfilingSnapshot();
    snapshot[QStringLiteral("speakers_refresh")] = m_speakersTab
        ? QJsonObject{
              {QStringLiteral("last_speakers_table_refresh_duration_ms"),
               m_speakersTab->lastSpeakersTableRefreshDurationMs()},
              {QStringLiteral("max_speakers_table_refresh_duration_ms"),
               m_speakersTab->maxSpeakersTableRefreshDurationMs()},
              {QStringLiteral("last_facedetections_panel_refresh_duration_ms"),
               m_speakersTab->lastFaceDetectionsPanelRefreshDurationMs()},
              {QStringLiteral("max_facedetections_panel_refresh_duration_ms"),
               m_speakersTab->maxFaceDetectionsPanelRefreshDurationMs()},
              {QStringLiteral("last_playhead_track_candidates_refresh_duration_ms"),
               m_speakersTab->lastPlayheadTrackCandidatesRefreshDurationMs()},
              {QStringLiteral("max_playhead_track_candidates_refresh_duration_ms"),
               m_speakersTab->maxPlayheadTrackCandidatesRefreshDurationMs()},
              {QStringLiteral("last_playhead_track_candidate_count"),
               m_speakersTab->lastPlayheadTrackCandidateCount()},
              {QStringLiteral("last_raw_detections_panel_refresh_duration_ms"),
               m_speakersTab->lastRawDetectionsPanelRefreshDurationMs()},
              {QStringLiteral("max_raw_detections_panel_refresh_duration_ms"),
               m_speakersTab->maxRawDetectionsPanelRefreshDurationMs()},
              {QStringLiteral("face_detections_debug"),
               m_speakersTab->faceDetectionsDebugSnapshot()},
              {QStringLiteral("track_assignment"),
               m_speakersTab->trackAssignmentTimingProfile()},
              {QStringLiteral("section_selection"),
               m_speakersTab->speakerSectionSelectionTimingProfile()}}
        : QJsonObject{};

    return snapshot;
}

QJsonObject EditorWindow::streamTimingSnapshot() const
{
    const qint64 now = nowMs();
    const bool playing = m_playbackTimer.isActive();
    const int64_t masterSample = qMax<int64_t>(0, m_transportTimelineSample);
    const qreal masterFramePosition = samplesToFramePosition(masterSample);
    const int64_t masterFrame =
        m_timeline
            ? qBound<int64_t>(0,
                              static_cast<int64_t>(std::floor(masterFramePosition)),
                              m_timeline->totalFrames())
            : qMax<int64_t>(0, static_cast<int64_t>(std::floor(masterFramePosition)));
    const qreal speed = normalizedPlaybackSpeed(m_playbackSpeed);
    const int64_t sessionStartSample = qMax<int64_t>(0, m_playbackSessionStartTimelineSample);
    const qint64 sessionStartWallMs = m_playbackSessionStartWallMs;
    const qint64 wallRuntimeMs =
        (playing && sessionStartWallMs > 0) ? qMax<qint64>(0, now - sessionStartWallMs) : 0;
    const int64_t masterRuntimeSamples =
        playing ? qMax<int64_t>(0, masterSample - sessionStartSample) : 0;
    const qreal expectedMasterRuntimeSamples =
        playing
            ? (static_cast<qreal>(wallRuntimeMs) / 1000.0) *
                  static_cast<qreal>(kAudioSampleRate) * speed
            : 0.0;
    const qreal masterRuntimeMs =
        (static_cast<qreal>(masterRuntimeSamples) * 1000.0) /
        static_cast<qreal>(kAudioSampleRate);

    QJsonArray streams;
    int activeCount = 0;
    if (m_timeline) {
        const QVector<TimelineClip> clips = m_timeline->clips();
        const QVector<RenderSyncMarker> markers = m_timeline->renderSyncMarkers();
        for (const TimelineClip& clip : clips) {
            const int64_t clipStartSample = clipTimelineStartSamples(clip);
            const int64_t clipDurationSamples = frameToSamples(qMax<int64_t>(0, clip.durationFrames));
            const int64_t clipEndSample = clipStartSample + clipDurationSamples;
            const bool active =
                masterSample >= clipStartSample && masterSample < clipEndSample;
            if (active) {
                ++activeCount;
            }

            const int64_t clampedTimelineSample =
                qBound<int64_t>(clipStartSample,
                                masterSample,
                                qMax<int64_t>(clipStartSample, clipEndSample - 1));
            const int64_t localTimelineSample =
                qMax<int64_t>(0, clampedTimelineSample - clipStartSample);
            const qreal localTimelineRuntimeMs =
                (static_cast<qreal>(localTimelineSample) * 1000.0) /
                static_cast<qreal>(kAudioSampleRate);
            const int64_t sourceStartSample = clipSourceInSamples(clip);
            const int64_t sourceSample =
                sourceSampleForClipAtTimelineSample(clip, clampedTimelineSample, markers);
            const int64_t sourceRuntimeSamples =
                qMax<int64_t>(0, sourceSample - sourceStartSample);
            const qreal sourceRuntimeMs =
                (static_cast<qreal>(sourceRuntimeSamples) * 1000.0) /
                static_cast<qreal>(kAudioSampleRate);
            const int64_t sourceFrame =
                sourceFrameForClipAtTimelineSample(clip, clampedTimelineSample, markers);
            const int64_t transcriptFrame =
                transcriptFrameForClipSourceFrame(clip, sourceFrame);

            qint64 projectedStreamStartWallMs = -1;
            qreal projectedWallRuntimeMs = 0.0;
            bool projectedWallValid = false;
            if (playing && sessionStartWallMs > 0 && speed > 0.0) {
                const int64_t streamStartOffsetSamples =
                    qMax<int64_t>(0, clipStartSample - sessionStartSample);
                const qreal streamStartOffsetMs =
                    (static_cast<qreal>(streamStartOffsetSamples) * 1000.0) /
                    (static_cast<qreal>(kAudioSampleRate) * speed);
                projectedStreamStartWallMs =
                    sessionStartWallMs + static_cast<qint64>(std::llround(streamStartOffsetMs));
                if (masterSample >= clipStartSample) {
                    projectedStreamStartWallMs = qMax<qint64>(sessionStartWallMs,
                                                              projectedStreamStartWallMs);
                    projectedWallRuntimeMs =
                        qMax<qreal>(0.0,
                                    static_cast<qreal>(now - projectedStreamStartWallMs) *
                                        speed);
                    projectedWallValid = true;
                }
            }

            QJsonArray roles;
            if (clipHasVisuals(clip)) {
                roles.push_back(QStringLiteral("video"));
            }
            if (clipAudioPlaybackEnabled(clip)) {
                roles.push_back(QStringLiteral("audio"));
            }
            if (clip.mediaType == ClipMediaType::Title) {
                roles.push_back(QStringLiteral("title"));
            }

            streams.push_back(QJsonObject{
                {QStringLiteral("clip_id"), clip.id},
                {QStringLiteral("label"), clip.label},
                {QStringLiteral("file_path"), clip.filePath},
                {QStringLiteral("playback_media_path"), playbackMediaPathForClip(clip)},
                {QStringLiteral("playback_audio_path"), playbackAudioPathForClip(clip)},
                {QStringLiteral("track_index"), clip.trackIndex},
                {QStringLiteral("roles"), roles},
                {QStringLiteral("active"), active},
                {QStringLiteral("timeline_start_sample"), static_cast<qint64>(clipStartSample)},
                {QStringLiteral("timeline_end_sample_exclusive"), static_cast<qint64>(clipEndSample)},
                {QStringLiteral("timeline_duration_samples"), static_cast<qint64>(clipDurationSamples)},
                {QStringLiteral("timeline_start_frame"), static_cast<qint64>(clip.startFrame)},
                {QStringLiteral("timeline_duration_frames"), static_cast<qint64>(clip.durationFrames)},
                {QStringLiteral("timeline_local_sample"), static_cast<qint64>(localTimelineSample)},
                {QStringLiteral("timeline_local_frame_position"),
                 samplesToFramePosition(localTimelineSample)},
                {QStringLiteral("timeline_runtime_ms"), localTimelineRuntimeMs},
                {QStringLiteral("source_fps"), resolvedSourceFps(clip)},
                {QStringLiteral("source_start_frame"), static_cast<qint64>(clip.sourceInFrame)},
                {QStringLiteral("source_start_sample"), static_cast<qint64>(sourceStartSample)},
                {QStringLiteral("source_sample"), static_cast<qint64>(sourceSample)},
                {QStringLiteral("source_frame"), static_cast<qint64>(sourceFrame)},
                {QStringLiteral("source_runtime_samples"), static_cast<qint64>(sourceRuntimeSamples)},
                {QStringLiteral("source_runtime_ms"), sourceRuntimeMs},
                {QStringLiteral("transcript_frame"), static_cast<qint64>(transcriptFrame)},
                {QStringLiteral("playback_rate"), clip.playbackRate},
                {QStringLiteral("projected_wall_valid"), projectedWallValid},
                {QStringLiteral("projected_stream_start_wall_ms"),
                 projectedWallValid
                     ? QJsonValue(static_cast<qint64>(projectedStreamStartWallMs))
                     : QJsonValue()},
                {QStringLiteral("projected_stream_wall_runtime_ms"),
                 projectedWallValid ? QJsonValue(projectedWallRuntimeMs) : QJsonValue()},
                {QStringLiteral("timeline_vs_projected_wall_drift_ms"),
                 projectedWallValid
                     ? QJsonValue(localTimelineRuntimeMs - projectedWallRuntimeMs)
                     : QJsonValue()},
                {QStringLiteral("source_vs_projected_wall_drift_ms"),
                 projectedWallValid
                     ? QJsonValue(sourceRuntimeMs - projectedWallRuntimeMs)
                     : QJsonValue()}
            });
        }
    }

    QJsonObject audio;
    if (m_audioEngine) {
        const int64_t audioSample =
            timelineSampleForAudioFeedbackSample(qMax<int64_t>(0, m_audioEngine->playbackClockSample()));
        audio = QJsonObject{
            {QStringLiteral("available"), m_audioEngine->audioClockAvailable()},
            {QStringLiteral("started"), m_audioEngine->playbackStarted()},
            {QStringLiteral("timeline_sample"), static_cast<qint64>(audioSample)},
            {QStringLiteral("timeline_frame"),
             static_cast<qint64>(std::floor(samplesToFramePosition(audioSample)))},
            {QStringLiteral("drift_from_master_samples"),
             static_cast<qint64>(masterSample - audioSample)},
            {QStringLiteral("drift_from_master_frames"),
             static_cast<qint64>(std::floor(samplesToFramePosition(masterSample)) -
                                 std::floor(samplesToFramePosition(audioSample)))}
        };
    }

    return QJsonObject{
        {QStringLiteral("snapshot_wall_ms"), now},
        {QStringLiteral("playback_active"), playing},
        {QStringLiteral("playback_speed"), speed},
        {QStringLiteral("master_timeline_sample"), static_cast<qint64>(masterSample)},
        {QStringLiteral("master_timeline_frame"), static_cast<qint64>(masterFrame)},
        {QStringLiteral("master_timeline_frame_position"), masterFramePosition},
        {QStringLiteral("session_start_wall_ms"), sessionStartWallMs},
        {QStringLiteral("session_start_timeline_sample"), static_cast<qint64>(sessionStartSample)},
        {QStringLiteral("session_wall_runtime_ms"), wallRuntimeMs},
        {QStringLiteral("master_runtime_samples"), static_cast<qint64>(masterRuntimeSamples)},
        {QStringLiteral("master_runtime_ms"), masterRuntimeMs},
        {QStringLiteral("expected_master_runtime_samples_from_wall"),
         playing ? QJsonValue(expectedMasterRuntimeSamples) : QJsonValue()},
        {QStringLiteral("master_vs_wall_drift_samples"),
         playing
             ? QJsonValue(static_cast<qreal>(masterRuntimeSamples) -
                          expectedMasterRuntimeSamples)
             : QJsonValue()},
        {QStringLiteral("active_stream_count"), activeCount},
        {QStringLiteral("stream_count"), streams.size()},
        {QStringLiteral("streams"), streams},
        {QStringLiteral("audio_feedback"), audio}
    };
}

QJsonObject EditorWindow::audioDebugSnapshot() const
{
    QJsonObject audio = m_audioEngine ? m_audioEngine->profilingSnapshot() : QJsonObject{};
    audio[QStringLiteral("ok")] = m_audioEngine != nullptr;
    if (!m_audioEngine) {
        audio[QStringLiteral("error")] = QStringLiteral("audio engine is not initialized");
    }
    audio[QStringLiteral("playback")] = playbackConfigSnapshot();
    audio[QStringLiteral("editor_playback_active")] = m_playbackTimer.isActive();
    audio[QStringLiteral("editor_current_frame")] =
        m_timeline ? static_cast<qint64>(m_timeline->currentFrame()) : 0;
    audio[QStringLiteral("transport_timeline_sample")] =
        static_cast<qint64>(m_transportTimelineSample);
    audio[QStringLiteral("filtered_playback_sample")] =
        static_cast<qint64>(m_filteredPlaybackSample);
    if (m_audioEngine) {
        const int64_t projectedAudioSample =
            timelineSampleForAudioFeedbackSample(qMax<int64_t>(0, m_audioEngine->playbackClockSample()));
        audio[QStringLiteral("projected_audio_feedback_timeline_sample")] =
            static_cast<qint64>(projectedAudioSample);
        audio[QStringLiteral("projected_audio_feedback_timeline_frame")] =
            static_cast<qint64>(std::floor(samplesToFramePosition(projectedAudioSample)));
        audio[QStringLiteral("audio_video_drift_samples")] =
            static_cast<qint64>(m_transportTimelineSample - projectedAudioSample);
        audio[QStringLiteral("audio_video_drift_frames")] =
            static_cast<qint64>(std::floor(samplesToFramePosition(m_transportTimelineSample)) -
                                std::floor(samplesToFramePosition(projectedAudioSample)));
    }
    audio[QStringLiteral("last_playback_stop_reason")] = m_lastPlaybackStopReason;
    return audio;
}

QJsonObject EditorWindow::runtimePatchesSnapshot() const
{
    QJsonArray active;
    for (const QJsonValue& value : m_runtimePatchLog) {
        const QJsonObject event = value.toObject();
        active.push_back(QJsonObject{
            {QStringLiteral("sequence"), event.value(QStringLiteral("sequence"))},
            {QStringLiteral("domain"), event.value(QStringLiteral("domain"))},
            {QStringLiteral("applied_utc_ms"), event.value(QStringLiteral("applied_utc_ms"))},
            {QStringLiteral("patch"), event.value(QStringLiteral("patch"))}
        });
    }
    return QJsonObject{
        {QStringLiteral("active_count"), active.size()},
        {QStringLiteral("active_patches"), active},
        {QStringLiteral("has_active_patches"), !active.isEmpty()},
        {QStringLiteral("note"), active.isEmpty()
             ? QStringLiteral("No runtime REST patches have been applied in this process.")
             : QStringLiteral("Runtime patches are process-local overrides applied through REST endpoints.")}
    };
}

QJsonObject EditorWindow::pipelineSnapshot(bool verbose) const
{
    if (!m_preview) {
        return QJsonObject{};
    }
    QJsonObject preview = verbose
        ? m_preview->profilingSnapshot()
        : m_preview->pipelineHealthSnapshot();
    if (verbose) {
        preview.insert(QStringLiteral("pipeline_stages"),
                       pipelineStagesToJson(m_preview->livePipelineSnapshots()));
    } else {
        preview.insert(QStringLiteral("diagnostic_detail"),
                       QStringLiteral("compact stage state; use /pipeline?verbose=1 for full overlay dumps"));
    }
    preview.insert(QStringLiteral("playback_pipeline_stages"), playbackStageMetricsSnapshot());
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
        {QStringLiteral("preview_visible_backlog_limit"),
         m_preview ? m_preview->playbackTuning().visibleBacklogLimit : defaultOptimizedPreviewProfile().previewTuning.visibleBacklogLimit},
        {QStringLiteral("preview_source_lookahead_frames"),
         m_preview ? m_preview->playbackTuning().sourceLookaheadFrames : defaultOptimizedPreviewProfile().previewTuning.sourceLookaheadFrames},
        {QStringLiteral("preview_proxy_lookahead_frames"),
         m_preview ? m_preview->playbackTuning().proxyLookaheadFrames : defaultOptimizedPreviewProfile().previewTuning.proxyLookaheadFrames},
        {QStringLiteral("startup_optimization"), startupOptimizationSnapshot()},
        {QStringLiteral("runtime_patches"), runtimePatchesSnapshot()},
        {QStringLiteral("optimized_profile_path"), optimizedProfilePath()},
        {QStringLiteral("optimized_profile_loaded"), m_optimizedProfileLoaded},
        {QStringLiteral("optimized_profile_generated_this_run"), m_optimizedProfileGeneratedThisRun},
        {QStringLiteral("optimized_profile_deprecated_alias"), true},
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
    appendRuntimePatch(&m_runtimePatchLog,
                       &m_runtimePatchSequence,
                       QStringLiteral("editor_throttles"),
                       patch);
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
        {QStringLiteral("playback_audio_warmup_pending"), m_playbackAudioWarmupPending},
        {QStringLiteral("playback_video_warmup_pending"), m_playbackVideoWarmupPending},
        {QStringLiteral("retiming_audio_for_playback"), m_retimingAudioForPlayback},
        {QStringLiteral("start_playback_after_audio_warmup"), m_startPlaybackAfterAudioWarmup},
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

    appendRuntimePatch(&m_runtimePatchLog,
                       &m_runtimePatchSequence,
                       QStringLiteral("playback"),
                       patch);
    return playbackConfigSnapshot();
}
