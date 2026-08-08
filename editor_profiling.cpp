#include "editor.h"
#include "debug_controls.h"
#include "editor_shared_media.h"
#include "editor_shared_render_sync.h"
#include "editor_shared_timing.h"
#include "playback_debug.h"

#include <QDateTime>
#include <QFileInfo>
#include <QSet>
#include <QSignalBlocker>
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

QJsonObject EditorWindow::playbackStageMetricsSnapshot(
    const QJsonObject& previewSnapshot) const
{
    QJsonObject stages;
    stages.insert(QStringLiteral("clock_update"),
                  editor::playbackStageMetricToJson(m_playbackClockStageMetric,
                                            QStringLiteral("editor")));
    stages.insert(QStringLiteral("playback_sample_apply"),
                  editor::playbackStageMetricToJson(m_playbackSampleApplyStageMetric,
                                            QStringLiteral("editor")));
    mergePlaybackStageMetricObjects(
        &stages,
        previewSnapshot.value(
            QStringLiteral("playback_pipeline_stages")).toObject());
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
    return m_startupReadinessSnapshot;
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

    QJsonObject previewSnapshot;
    if (m_preview) {
        previewSnapshot = m_preview->profilingSnapshot();
        snapshot[QStringLiteral("preview")] = previewSnapshot;
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
    snapshot[QStringLiteral("playback_pipeline_stages")] =
        playbackStageMetricsSnapshot(previewSnapshot);
    snapshot[QStringLiteral("ui_actions")] = m_uiActionProfiler.snapshot();
    snapshot[QStringLiteral("speaker_tracking")] = transcriptSpeakerTrackingProfilingSnapshot();
    snapshot[QStringLiteral("speakers_refresh")] = m_speakersTab
        ? QJsonObject{
              {QStringLiteral("last_speakers_table_refresh_duration_ms"),
               m_speakersTab->lastSpeakersTableRefreshDurationMs()},
              {QStringLiteral("max_speakers_table_refresh_duration_ms"),
               m_speakersTab->maxSpeakersTableRefreshDurationMs()},
              {QStringLiteral("last_speaker_sections_table_refresh_duration_ms"),
               m_speakersTab->lastSpeakerSectionsTableRefreshDurationMs()},
              {QStringLiteral("max_speaker_sections_table_refresh_duration_ms"),
               m_speakersTab->maxSpeakerSectionsTableRefreshDurationMs()},
              {QStringLiteral("last_speaker_sections_table_row_count"),
               m_speakersTab->lastSpeakerSectionsTableRowCount()},
              {QStringLiteral("last_speaker_sections_table_refresh_skipped_reason"),
               m_speakersTab->lastSpeakerSectionsTableRefreshSkippedReason()},
              {QStringLiteral("speaker_sections_table_refresh_cache_hit_count"),
               m_speakersTab->speakerSectionsTableRefreshCacheHitCount()},
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

UiActionProfiler::Context EditorWindow::uiActionContext(const QString& selectedTab) const
{
    UiActionProfiler::Context context;
    context.playbackActive = m_playbackTimer.isActive();
    context.frame = m_timeline ? static_cast<qint64>(m_timeline->currentFrame()) : -1;
    context.sample = static_cast<qint64>(m_transportTimelineSample);
    context.stateRevision = m_stateRevision.load();
    context.selectedTab = selectedTab;
    if (context.selectedTab.isEmpty() && m_inspectorPane && m_inspectorPane->tabs()) {
        const int index = m_inspectorPane->tabs()->currentIndex();
        if (index >= 0) {
            context.selectedTab = m_inspectorPane->tabs()->tabText(index);
        }
    }
    if (m_timeline) {
        context.selectedClipId = m_timeline->selectedClipId();
    }
    return context;
}

QJsonObject EditorWindow::speakerUiPerformanceSnapshot() const
{
    QJsonObject snapshot{
        {QStringLiteral("ok"), true},
        {QStringLiteral("playback_active"), m_playbackTimer.isActive()},
        {QStringLiteral("last_inspector_refresh_duration_ms"), m_lastInspectorRefreshDurationMs.load()},
        {QStringLiteral("max_inspector_refresh_duration_ms"), m_maxInspectorRefreshDurationMs.load()},
        {QStringLiteral("slow_inspector_refresh_count"), m_inspectorRefreshSlowCount.load()}
    };
    if (!m_speakersTab) {
        snapshot[QStringLiteral("ok")] = false;
        snapshot[QStringLiteral("error")] = QStringLiteral("speakers tab not initialized");
        return snapshot;
    }
    snapshot[QStringLiteral("last_speakers_table_refresh_duration_ms")] =
        m_speakersTab->lastSpeakersTableRefreshDurationMs();
    snapshot[QStringLiteral("max_speakers_table_refresh_duration_ms")] =
        m_speakersTab->maxSpeakersTableRefreshDurationMs();
    snapshot[QStringLiteral("last_speaker_sections_table_refresh_duration_ms")] =
        m_speakersTab->lastSpeakerSectionsTableRefreshDurationMs();
    snapshot[QStringLiteral("max_speaker_sections_table_refresh_duration_ms")] =
        m_speakersTab->maxSpeakerSectionsTableRefreshDurationMs();
    snapshot[QStringLiteral("last_speaker_sections_table_row_count")] =
        m_speakersTab->lastSpeakerSectionsTableRowCount();
    snapshot[QStringLiteral("last_speaker_sections_table_refresh_skipped_reason")] =
        m_speakersTab->lastSpeakerSectionsTableRefreshSkippedReason();
    snapshot[QStringLiteral("speaker_sections_table_refresh_cache_hit_count")] =
        m_speakersTab->speakerSectionsTableRefreshCacheHitCount();
    snapshot[QStringLiteral("last_playhead_track_candidates_refresh_duration_ms")] =
        m_speakersTab->lastPlayheadTrackCandidatesRefreshDurationMs();
    snapshot[QStringLiteral("max_playhead_track_candidates_refresh_duration_ms")] =
        m_speakersTab->maxPlayheadTrackCandidatesRefreshDurationMs();
    snapshot[QStringLiteral("last_playhead_track_candidate_count")] =
        m_speakersTab->lastPlayheadTrackCandidateCount();
    snapshot[QStringLiteral("last_facedetections_panel_refresh_duration_ms")] =
        m_speakersTab->lastFaceDetectionsPanelRefreshDurationMs();
    snapshot[QStringLiteral("max_facedetections_panel_refresh_duration_ms")] =
        m_speakersTab->maxFaceDetectionsPanelRefreshDurationMs();
    snapshot[QStringLiteral("last_raw_detections_panel_refresh_duration_ms")] =
        m_speakersTab->lastRawDetectionsPanelRefreshDurationMs();
    snapshot[QStringLiteral("max_raw_detections_panel_refresh_duration_ms")] =
        m_speakersTab->maxRawDetectionsPanelRefreshDurationMs();
    snapshot[QStringLiteral("track_assignment")] =
        m_speakersTab->trackAssignmentTimingProfile();
    snapshot[QStringLiteral("section_selection")] =
        m_speakersTab->speakerSectionSelectionTimingProfile();
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
        int generatedTimingFollowerCount = 0;
        for (const TimelineClip& clip : clips) {
            if (clip.clipRole == ClipRole::MaskMatte &&
                clip.locked &&
                clip.sourceTransformLocked) {
                ++generatedTimingFollowerCount;
                continue;
            }
            const int64_t clipStartSample = clipTimelineStartSamples(clip);
            const int64_t clipDurationSamples = clipTimelineDurationSamples(clip);
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

            const bool renderUseProxyMedia =
                m_renderUseProxiesCheckBox && m_renderUseProxiesCheckBox->isChecked();
            TimelineClip effectivePreviewClip = clip;
            if (!renderUseProxyMedia) {
                effectivePreviewClip.useProxy = false;
                effectivePreviewClip.proxyPath.clear();
            }
            const QString configuredProxyPath = playbackProxyPathForClip(clip);
            const QString configuredPlaybackMediaPath = playbackMediaPathForClip(clip);
            const QString effectivePlaybackMediaPath = playbackMediaPathForClip(effectivePreviewClip);
            const bool effectiveProxyEnabled =
                !configuredProxyPath.isEmpty() &&
                QFileInfo(effectivePlaybackMediaPath).absoluteFilePath() ==
                    QFileInfo(configuredProxyPath).absoluteFilePath();

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
                {QStringLiteral("playback_media_path"), effectivePlaybackMediaPath},
                {QStringLiteral("configured_playback_media_path"), configuredPlaybackMediaPath},
                {QStringLiteral("configured_proxy_media_path"), configuredProxyPath},
                {QStringLiteral("render_use_proxy_media"), renderUseProxyMedia},
                {QStringLiteral("clip_use_proxy"), clip.useProxy},
                {QStringLiteral("proxy_available"), !configuredProxyPath.isEmpty()},
                {QStringLiteral("effective_proxy_enabled"), effectiveProxyEnabled},
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
        streams.prepend(QJsonObject{
            {QStringLiteral("diagnostic"), QStringLiteral("generated_timing_followers_skipped")},
            {QStringLiteral("count"), generatedTimingFollowerCount},
            {QStringLiteral("reason"),
             QStringLiteral("Locked source-transform mask mattes are child foreground markers, not independent A/V streams.")}
        });
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
    preview.insert(QStringLiteral("playback_pipeline_stages"),
                   playbackStageMetricsSnapshot(preview));
    return preview;
}

QJsonObject EditorWindow::throttleConfigSnapshot() const
{
    const QJsonObject speakerTrackingConfig = transcriptSpeakerTrackingConfigSnapshot();
    const PreviewSurface::PlaybackTuning previewTuning =
        m_preview ? m_preview->playbackTuning()
                  : defaultOptimizedPreviewProfile().previewTuning;
    return QJsonObject{
        {QStringLiteral("ok"), true},
        {QStringLiteral("playback_ui_sync_min_interval_ms"), m_playbackUiSyncMinIntervalMs},
        {QStringLiteral("playback_table_sync_min_interval_ms"), m_playbackTableSyncMinIntervalMs},
        {QStringLiteral("playback_state_save_min_interval_ms"), m_playbackStateSaveMinIntervalMs},
        {QStringLiteral("slow_seek_warn_threshold_ms"), m_slowSeekWarnThresholdMs},
        {QStringLiteral("playback_start_lookahead_frames"), m_playbackStartLookaheadFrames},
        {QStringLiteral("playback_start_lookahead_timeout_ms"), m_playbackStartLookaheadTimeoutMs},
        {QStringLiteral("main_thread_heartbeat_interval_ms"), m_mainThreadHeartbeatIntervalMs},
        {QStringLiteral("state_save_debounce_interval_ms"), m_stateSaveDebounceIntervalMs},
        {QStringLiteral("transcript_manual_selection_hold_ms"), m_transcriptManualSelectionHoldMs},
        {QStringLiteral("preview_visible_backlog_limit"),
         previewTuning.visibleBacklogLimit},
        {QStringLiteral("preview_source_lookahead_frames"),
         previewTuning.sourceLookaheadFrames},
        {QStringLiteral("preview_proxy_lookahead_frames"),
         previewTuning.proxyLookaheadFrames},
        {QStringLiteral("preview_prefetch_max_queue_depth"),
         previewTuning.prefetchMaxQueueDepth},
        {QStringLiteral("preview_prefetch_max_inflight"),
         previewTuning.prefetchMaxInflight},
        {QStringLiteral("preview_prefetch_max_per_tick"),
         previewTuning.prefetchMaxPerTick},
        {QStringLiteral("preview_visible_queue_reserve"),
         previewTuning.visibleQueueReserve},
        {QStringLiteral("preview_playback_window_ahead"),
         previewTuning.playbackWindowAhead},
        {QStringLiteral("preview_decode_autotune_enabled"),
         previewTuning.decodeAutotuneEnabled},
        {QStringLiteral("preview_decode_autotune_max_boost_level"),
         previewTuning.decodeAutotuneMaxBoostLevel},
        {QStringLiteral("preview_decode_autotune_min_adjust_interval_ms"),
         previewTuning.decodeAutotuneMinAdjustIntervalMs},
        {QStringLiteral("preview_decode_autotune_window_ms"),
         previewTuning.decodeAutotuneWindowMs},
        {QStringLiteral("preview_decode_autotune_min_samples"),
         previewTuning.decodeAutotuneMinSamples},
        {QStringLiteral("preview_decode_autotune_starved_late_rate_permille"),
         previewTuning.decodeAutotuneStarvedLateRatePermille},
        {QStringLiteral("preview_decode_autotune_starved_exact_hit_rate_permille"),
         previewTuning.decodeAutotuneStarvedExactHitRatePermille},
        {QStringLiteral("preview_decode_autotune_starved_avg_frame_lag"),
         previewTuning.decodeAutotuneStarvedAvgFrameLag},
        {QStringLiteral("preview_decode_autotune_recovered_late_rate_permille"),
         previewTuning.decodeAutotuneRecoveredLateRatePermille},
        {QStringLiteral("preview_decode_autotune_recovered_exact_hit_rate_permille"),
         previewTuning.decodeAutotuneRecoveredExactHitRatePermille},
        {QStringLiteral("preview_decode_autotune_recovered_avg_frame_lag"),
         previewTuning.decodeAutotuneRecoveredAvgFrameLag},
        {QStringLiteral("preview_decode_autotune_max_visible_backlog_limit"),
         previewTuning.decodeAutotuneMaxVisibleBacklogLimit},
        {QStringLiteral("preview_decode_autotune_max_source_lookahead_frames"),
         previewTuning.decodeAutotuneMaxSourceLookaheadFrames},
        {QStringLiteral("preview_decode_autotune_max_proxy_lookahead_frames"),
         previewTuning.decodeAutotuneMaxProxyLookaheadFrames},
        {QStringLiteral("preview_decode_autotune_visible_backlog_step"),
         previewTuning.decodeAutotuneVisibleBacklogStep},
        {QStringLiteral("preview_decode_autotune_lookahead_step"),
         previewTuning.decodeAutotuneLookaheadStep},
        {QStringLiteral("preview_decode_autotune_max_prefetch_max_queue_depth"),
         previewTuning.decodeAutotuneMaxPrefetchMaxQueueDepth},
        {QStringLiteral("preview_decode_autotune_max_prefetch_max_inflight"),
         previewTuning.decodeAutotuneMaxPrefetchMaxInflight},
        {QStringLiteral("preview_decode_autotune_max_prefetch_max_per_tick"),
         previewTuning.decodeAutotuneMaxPrefetchMaxPerTick},
        {QStringLiteral("preview_decode_autotune_max_visible_queue_reserve"),
         previewTuning.decodeAutotuneMaxVisibleQueueReserve},
        {QStringLiteral("preview_decode_autotune_max_playback_window_ahead"),
         previewTuning.decodeAutotuneMaxPlaybackWindowAhead},
        {QStringLiteral("preview_decode_autotune_prefetch_queue_depth_step"),
         previewTuning.decodeAutotunePrefetchQueueDepthStep},
        {QStringLiteral("preview_decode_autotune_prefetch_concurrency_step"),
         previewTuning.decodeAutotunePrefetchConcurrencyStep},
        {QStringLiteral("preview_decode_autotune_visible_queue_reserve_step"),
         previewTuning.decodeAutotuneVisibleQueueReserveStep},
        {QStringLiteral("preview_decode_autotune_playback_window_ahead_step"),
         previewTuning.decodeAutotunePlaybackWindowAheadStep},
        {QStringLiteral("prefetch_max_queue_depth"), editor::debugPrefetchMaxQueueDepth()},
        {QStringLiteral("prefetch_max_inflight"), editor::debugPrefetchMaxInflight()},
        {QStringLiteral("prefetch_max_per_tick"), editor::debugPrefetchMaxPerTick()},
        {QStringLiteral("prefetch_skip_visible_pending_threshold"),
         editor::debugPrefetchSkipVisiblePendingThreshold()},
        {QStringLiteral("visible_queue_reserve"), editor::debugVisibleQueueReserve()},
        {QStringLiteral("playback_window_ahead"), editor::debugPlaybackWindowAhead()},
        {QStringLiteral("file_video_playback_window_ahead"),
         editor::debugFileVideoPlaybackWindowAhead()},
        {QStringLiteral("visible_decode_keep_window"),
         editor::debugVisibleDecodeKeepWindow()},
        {QStringLiteral("sequence_visible_decode_keep_window"),
         editor::debugSequenceVisibleDecodeKeepWindow()},
        {QStringLiteral("visible_pending_retry_ms"),
         editor::debugVisiblePendingRetryMs()},
        {QStringLiteral("cancel_before_min_frame_advance"),
         editor::debugCancelBeforeMinFrameAdvance()},
        {QStringLiteral("cancel_before_min_interval_ms"),
         editor::debugCancelBeforeMinIntervalMs()},
        {QStringLiteral("supersede_slack_frames"),
         editor::debugSupersedeSlackFrames()},
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
    auto parseNonNegativeInt = [](const QJsonObject& obj, const QString& key, int* target, QString* error) -> bool {
        if (!obj.contains(key)) return true;
        bool ok = false;
        const qint64 value = obj.value(key).toVariant().toLongLong(&ok);
        if (!ok || value < 0 || value > std::numeric_limits<int>::max()) {
            if (error) *error = QStringLiteral("%1 must be a non-negative integer").arg(key);
            return false;
        }
        *target = static_cast<int>(value);
        return true;
    };
    auto parseBool = [](const QJsonObject& obj, const QString& key, bool* target, QString* error) -> bool {
        if (!obj.contains(key)) return true;
        if (!obj.value(key).isBool()) {
            if (error) *error = QStringLiteral("%1 must be a boolean").arg(key);
            return false;
        }
        *target = obj.value(key).toBool();
        return true;
    };
    auto parsePermille = [](const QJsonObject& obj, const QString& key, int* target, QString* error) -> bool {
        if (!obj.contains(key)) return true;
        bool ok = false;
        const qint64 value = obj.value(key).toVariant().toLongLong(&ok);
        if (!ok || value < 0 || value > 1000) {
            if (error) *error = QStringLiteral("%1 must be an integer in [0, 1000]").arg(key);
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
    int prefetchMaxQueueDepth = editor::debugPrefetchMaxQueueDepth();
    int prefetchMaxInflight = editor::debugPrefetchMaxInflight();
    int prefetchMaxPerTick = editor::debugPrefetchMaxPerTick();
    int prefetchSkipVisiblePendingThreshold = editor::debugPrefetchSkipVisiblePendingThreshold();
    int visibleQueueReserve = editor::debugVisibleQueueReserve();
    int playbackWindowAhead = editor::debugPlaybackWindowAhead();
    int fileVideoPlaybackWindowAhead = editor::debugFileVideoPlaybackWindowAhead();
    int visibleDecodeKeepWindow = editor::debugVisibleDecodeKeepWindow();
    int sequenceVisibleDecodeKeepWindow = editor::debugSequenceVisibleDecodeKeepWindow();
    int visiblePendingRetryMs = editor::debugVisiblePendingRetryMs();
    int cancelBeforeMinFrameAdvance = editor::debugCancelBeforeMinFrameAdvance();
    int supersedeSlackFrames = editor::debugSupersedeSlackFrames();
    qint64 cancelBeforeMinIntervalMs = editor::debugCancelBeforeMinIntervalMs();
    PreviewSurface::PlaybackTuning previewTuning =
        m_preview ? m_preview->playbackTuning() : defaultOptimizedPreviewProfile().previewTuning;
    if (!parsePositiveMs(patch, QStringLiteral("playback_ui_sync_min_interval_ms"), &m_playbackUiSyncMinIntervalMs, &error) ||
        !parsePositiveMs(patch, QStringLiteral("playback_table_sync_min_interval_ms"), &m_playbackTableSyncMinIntervalMs, &error) ||
        !parsePositiveMs(patch, QStringLiteral("playback_state_save_min_interval_ms"), &m_playbackStateSaveMinIntervalMs, &error) ||
        !parsePositiveMs(patch, QStringLiteral("slow_seek_warn_threshold_ms"), &m_slowSeekWarnThresholdMs, &error) ||
        !parsePositiveInt(patch, QStringLiteral("playback_start_lookahead_frames"), &m_playbackStartLookaheadFrames, &error) ||
        !parsePositiveInt(patch, QStringLiteral("playback_start_lookahead_timeout_ms"), &m_playbackStartLookaheadTimeoutMs, &error) ||
        !parsePositiveInt(patch, QStringLiteral("main_thread_heartbeat_interval_ms"), &m_mainThreadHeartbeatIntervalMs, &error) ||
        !parsePositiveInt(patch, QStringLiteral("state_save_debounce_interval_ms"), &m_stateSaveDebounceIntervalMs, &error) ||
        !parsePositiveInt(patch, QStringLiteral("transcript_manual_selection_hold_ms"), &m_transcriptManualSelectionHoldMs, &error) ||
        !parsePositiveInt(patch, QStringLiteral("preview_visible_backlog_limit"), &previewVisibleBacklogLimit, &error) ||
        !parsePositiveInt(patch, QStringLiteral("preview_source_lookahead_frames"), &previewSourceLookaheadFrames, &error) ||
        !parsePositiveInt(patch, QStringLiteral("preview_proxy_lookahead_frames"), &previewProxyLookaheadFrames, &error) ||
        !parseBool(patch, QStringLiteral("preview_decode_autotune_enabled"), &previewTuning.decodeAutotuneEnabled, &error) ||
        !parseNonNegativeInt(patch, QStringLiteral("preview_decode_autotune_max_boost_level"), &previewTuning.decodeAutotuneMaxBoostLevel, &error) ||
        !parsePositiveInt(patch, QStringLiteral("preview_decode_autotune_min_adjust_interval_ms"), &previewTuning.decodeAutotuneMinAdjustIntervalMs, &error) ||
        !parsePositiveInt(patch, QStringLiteral("preview_decode_autotune_window_ms"), &previewTuning.decodeAutotuneWindowMs, &error) ||
        !parsePositiveInt(patch, QStringLiteral("preview_decode_autotune_min_samples"), &previewTuning.decodeAutotuneMinSamples, &error) ||
        !parsePermille(patch, QStringLiteral("preview_decode_autotune_starved_late_rate_permille"), &previewTuning.decodeAutotuneStarvedLateRatePermille, &error) ||
        !parsePermille(patch, QStringLiteral("preview_decode_autotune_starved_exact_hit_rate_permille"), &previewTuning.decodeAutotuneStarvedExactHitRatePermille, &error) ||
        !parsePositiveInt(patch, QStringLiteral("preview_decode_autotune_starved_avg_frame_lag"), &previewTuning.decodeAutotuneStarvedAvgFrameLag, &error) ||
        !parsePermille(patch, QStringLiteral("preview_decode_autotune_recovered_late_rate_permille"), &previewTuning.decodeAutotuneRecoveredLateRatePermille, &error) ||
        !parsePermille(patch, QStringLiteral("preview_decode_autotune_recovered_exact_hit_rate_permille"), &previewTuning.decodeAutotuneRecoveredExactHitRatePermille, &error) ||
        !parsePositiveInt(patch, QStringLiteral("preview_decode_autotune_recovered_avg_frame_lag"), &previewTuning.decodeAutotuneRecoveredAvgFrameLag, &error) ||
        !parsePositiveInt(patch, QStringLiteral("preview_decode_autotune_max_visible_backlog_limit"), &previewTuning.decodeAutotuneMaxVisibleBacklogLimit, &error) ||
        !parsePositiveInt(patch, QStringLiteral("preview_decode_autotune_max_source_lookahead_frames"), &previewTuning.decodeAutotuneMaxSourceLookaheadFrames, &error) ||
        !parsePositiveInt(patch, QStringLiteral("preview_decode_autotune_max_proxy_lookahead_frames"), &previewTuning.decodeAutotuneMaxProxyLookaheadFrames, &error) ||
        !parsePositiveInt(patch, QStringLiteral("preview_decode_autotune_visible_backlog_step"), &previewTuning.decodeAutotuneVisibleBacklogStep, &error) ||
        !parsePositiveInt(patch, QStringLiteral("preview_decode_autotune_lookahead_step"), &previewTuning.decodeAutotuneLookaheadStep, &error) ||
        !parsePositiveInt(patch, QStringLiteral("preview_decode_autotune_max_prefetch_max_queue_depth"), &previewTuning.decodeAutotuneMaxPrefetchMaxQueueDepth, &error) ||
        !parsePositiveInt(patch, QStringLiteral("preview_decode_autotune_max_prefetch_max_inflight"), &previewTuning.decodeAutotuneMaxPrefetchMaxInflight, &error) ||
        !parsePositiveInt(patch, QStringLiteral("preview_decode_autotune_max_prefetch_max_per_tick"), &previewTuning.decodeAutotuneMaxPrefetchMaxPerTick, &error) ||
        !parseNonNegativeInt(patch, QStringLiteral("preview_decode_autotune_max_visible_queue_reserve"), &previewTuning.decodeAutotuneMaxVisibleQueueReserve, &error) ||
        !parsePositiveInt(patch, QStringLiteral("preview_decode_autotune_max_playback_window_ahead"), &previewTuning.decodeAutotuneMaxPlaybackWindowAhead, &error) ||
        !parsePositiveInt(patch, QStringLiteral("preview_decode_autotune_prefetch_queue_depth_step"), &previewTuning.decodeAutotunePrefetchQueueDepthStep, &error) ||
        !parsePositiveInt(patch, QStringLiteral("preview_decode_autotune_prefetch_concurrency_step"), &previewTuning.decodeAutotunePrefetchConcurrencyStep, &error) ||
        !parsePositiveInt(patch, QStringLiteral("preview_decode_autotune_visible_queue_reserve_step"), &previewTuning.decodeAutotuneVisibleQueueReserveStep, &error) ||
        !parsePositiveInt(patch, QStringLiteral("preview_decode_autotune_playback_window_ahead_step"), &previewTuning.decodeAutotunePlaybackWindowAheadStep, &error) ||
        !parsePositiveInt(patch, QStringLiteral("preview_prefetch_max_queue_depth"), &prefetchMaxQueueDepth, &error) ||
        !parsePositiveInt(patch, QStringLiteral("preview_prefetch_max_inflight"), &prefetchMaxInflight, &error) ||
        !parsePositiveInt(patch, QStringLiteral("preview_prefetch_max_per_tick"), &prefetchMaxPerTick, &error) ||
        !parseNonNegativeInt(patch, QStringLiteral("preview_visible_queue_reserve"), &visibleQueueReserve, &error) ||
        !parsePositiveInt(patch, QStringLiteral("preview_playback_window_ahead"), &playbackWindowAhead, &error) ||
        !parsePositiveInt(patch, QStringLiteral("prefetch_max_queue_depth"), &prefetchMaxQueueDepth, &error) ||
        !parsePositiveInt(patch, QStringLiteral("prefetch_max_inflight"), &prefetchMaxInflight, &error) ||
        !parsePositiveInt(patch, QStringLiteral("prefetch_max_per_tick"), &prefetchMaxPerTick, &error) ||
        !parseNonNegativeInt(patch, QStringLiteral("prefetch_skip_visible_pending_threshold"), &prefetchSkipVisiblePendingThreshold, &error) ||
        !parsePositiveInt(patch, QStringLiteral("visible_queue_reserve"), &visibleQueueReserve, &error) ||
        !parsePositiveInt(patch, QStringLiteral("playback_window_ahead"), &playbackWindowAhead, &error) ||
        !parsePositiveInt(patch, QStringLiteral("file_video_playback_window_ahead"), &fileVideoPlaybackWindowAhead, &error) ||
        !parsePositiveInt(patch, QStringLiteral("visible_decode_keep_window"), &visibleDecodeKeepWindow, &error) ||
        !parsePositiveInt(patch, QStringLiteral("sequence_visible_decode_keep_window"), &sequenceVisibleDecodeKeepWindow, &error) ||
        !parsePositiveInt(patch, QStringLiteral("visible_pending_retry_ms"), &visiblePendingRetryMs, &error) ||
        !parsePositiveInt(patch, QStringLiteral("cancel_before_min_frame_advance"), &cancelBeforeMinFrameAdvance, &error) ||
        !parsePositiveMs(patch, QStringLiteral("cancel_before_min_interval_ms"), &cancelBeforeMinIntervalMs, &error) ||
        !parsePositiveInt(patch, QStringLiteral("supersede_slack_frames"), &supersedeSlackFrames, &error)) {
        return QJsonObject{
            {QStringLiteral("ok"), false},
            {QStringLiteral("error"), error}
        };
    }
    if (m_preview) {
        previewTuning.visibleBacklogLimit = previewVisibleBacklogLimit;
        previewTuning.sourceLookaheadFrames = previewSourceLookaheadFrames;
        previewTuning.proxyLookaheadFrames = previewProxyLookaheadFrames;
        previewTuning.prefetchMaxQueueDepth = prefetchMaxQueueDepth;
        previewTuning.prefetchMaxInflight = prefetchMaxInflight;
        previewTuning.prefetchMaxPerTick = prefetchMaxPerTick;
        previewTuning.visibleQueueReserve = visibleQueueReserve;
        previewTuning.playbackWindowAhead = playbackWindowAhead;
        m_preview->setPlaybackTuning(previewTuning);
    }
    editor::setDebugPrefetchMaxQueueDepth(prefetchMaxQueueDepth);
    editor::setDebugPrefetchMaxInflight(prefetchMaxInflight);
    editor::setDebugPrefetchMaxPerTick(prefetchMaxPerTick);
    editor::setDebugPrefetchSkipVisiblePendingThreshold(prefetchSkipVisiblePendingThreshold);
    editor::setDebugVisibleQueueReserve(visibleQueueReserve);
    editor::setDebugPlaybackWindowAhead(playbackWindowAhead);
    editor::setDebugFileVideoPlaybackWindowAhead(fileVideoPlaybackWindowAhead);
    editor::setDebugVisibleDecodeKeepWindow(visibleDecodeKeepWindow);
    editor::setDebugSequenceVisibleDecodeKeepWindow(sequenceVisibleDecodeKeepWindow);
    editor::setDebugVisiblePendingRetryMs(visiblePendingRetryMs);
    editor::setDebugCancelBeforeMinFrameAdvance(cancelBeforeMinFrameAdvance);
    editor::setDebugCancelBeforeMinIntervalMs(cancelBeforeMinIntervalMs);
    editor::setDebugSupersedeSlackFrames(supersedeSlackFrames);
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
        {QStringLiteral("retiming_audio_for_playback"), m_retimingAudioForPlayback},
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
            QStringLiteral("time-stretch"),
            QStringLiteral("rubber_band"),
            QStringLiteral("rubber-band"),
            QStringLiteral("rubberband"),
            QStringLiteral("rubber_band_100"),
            QStringLiteral("rubberband_100"),
            QStringLiteral("rubber-band-100"),
            QStringLiteral("rubber_band_pass_through_frequency"),
            QStringLiteral("rubber-band-pass-through-frequency"),
            QStringLiteral("rubberband_pass_through_frequency"),
            QStringLiteral("rubber_band_50"),
            QStringLiteral("rubberband_50"),
            QStringLiteral("rubber-band-50")
        };
        if (!validValues.contains(raw)) {
            error = QStringLiteral("audio_warp_mode must be one of: disabled, varispeed, time_stretch, rubber_band, rubber_band_pass_through_frequency");
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

QJsonObject EditorWindow::applyAudioConfigPatch(const QJsonObject& patch)
{
    auto reject = [](const QString& error) {
        return QJsonObject{
            {QStringLiteral("ok"), false},
            {QStringLiteral("error"), error}
        };
    };

    auto readBool = [&patch, &reject](const QString& key, bool* target) -> QJsonObject {
        if (!patch.contains(key)) {
            return {};
        }
        if (!patch.value(key).isBool()) {
            return reject(QStringLiteral("%1 must be a boolean").arg(key));
        }
        *target = patch.value(key).toBool();
        return {};
    };

    auto readDouble = [&patch, &reject](const QString& key, qreal min, qreal max, qreal* target) -> QJsonObject {
        if (!patch.contains(key)) {
            return {};
        }
        bool ok = false;
        const qreal value = patch.value(key).toVariant().toDouble(&ok);
        if (!ok || value < min || value > max) {
            return reject(QStringLiteral("%1 must be a number in [%2, %3]")
                              .arg(key)
                              .arg(min)
                              .arg(max));
        }
        *target = value;
        return {};
    };

    auto readInt = [&patch, &reject](const QString& key, int min, int max, int* target) -> QJsonObject {
        if (!patch.contains(key)) {
            return {};
        }
        bool ok = false;
        const int value = patch.value(key).toVariant().toInt(&ok);
        if (!ok || value < min || value > max) {
            return reject(QStringLiteral("%1 must be an integer in [%2, %3]")
                              .arg(key)
                              .arg(min)
                              .arg(max));
        }
        *target = value;
        return {};
    };

    PreviewSurface::AudioDynamicsSettings next = m_previewAudioDynamics;
    QJsonObject error;
    if (!(error = readBool(QStringLiteral("audioAmplifyEnabled"), &next.amplifyEnabled)).isEmpty()) return error;
    if (!(error = readDouble(QStringLiteral("audioAmplifyDb"), -36.0, 36.0, &next.amplifyDb)).isEmpty()) return error;
    if (!(error = readBool(QStringLiteral("audioNormalizeEnabled"), &next.normalizeEnabled)).isEmpty()) return error;
    if (!(error = readDouble(QStringLiteral("audioNormalizeTargetDb"), -24.0, 0.0, &next.normalizeTargetDb)).isEmpty()) return error;
    if (!(error = readBool(QStringLiteral("audioStereoToMonoEnabled"), &next.stereoToMonoEnabled)).isEmpty()) return error;
    if (!(error = readBool(QStringLiteral("audioSelectiveNormalizeEnabled"), &next.selectiveNormalizeEnabled)).isEmpty()) return error;
    if (!(error = readDouble(QStringLiteral("audioSelectiveNormalizeMinSegmentSeconds"), 0.1, 30.0, &next.selectiveNormalizeMinSegmentSeconds)).isEmpty()) return error;
    if (!(error = readDouble(QStringLiteral("audioSelectiveNormalizePeakDb"), -36.0, 0.0, &next.selectiveNormalizePeakDb)).isEmpty()) return error;
    if (!(error = readInt(QStringLiteral("audioSelectiveNormalizePasses"), 1, 8, &next.selectiveNormalizePasses)).isEmpty()) return error;
    if (!(error = readBool(QStringLiteral("audioSelectiveNormalizeOverlayVisible"), &next.selectiveNormalizeOverlayVisible)).isEmpty()) return error;
    if (!(error = readBool(QStringLiteral("audioTranscriptNormalizeEnabled"), &next.transcriptNormalizeEnabled)).isEmpty()) return error;
    if (!(error = readBool(QStringLiteral("audioWaveformPreviewPostProcessing"), &next.waveformPreviewPostProcessing)).isEmpty()) return error;
    if (!(error = readBool(QStringLiteral("audioPeakReductionEnabled"), &next.peakReductionEnabled)).isEmpty()) return error;
    if (!(error = readDouble(QStringLiteral("audioPeakThresholdDb"), -24.0, 0.0, &next.peakThresholdDb)).isEmpty()) return error;
    if (!(error = readBool(QStringLiteral("audioLimiterEnabled"), &next.limiterEnabled)).isEmpty()) return error;
    if (!(error = readDouble(QStringLiteral("audioLimiterThresholdDb"), -12.0, 0.0, &next.limiterThresholdDb)).isEmpty()) return error;
    if (!(error = readBool(QStringLiteral("audioCompressorEnabled"), &next.compressorEnabled)).isEmpty()) return error;
    if (!(error = readDouble(QStringLiteral("audioCompressorThresholdDb"), -30.0, -1.0, &next.compressorThresholdDb)).isEmpty()) return error;
    if (!(error = readDouble(QStringLiteral("audioCompressorRatio"), 1.0, 20.0, &next.compressorRatio)).isEmpty()) return error;
    if (!(error = readBool(QStringLiteral("audioSoftClipEnabled"), &next.softClipEnabled)).isEmpty()) return error;

    m_previewAudioDynamics = next;

    auto setCheckedBlocked = [](QCheckBox* box, bool checked) {
        if (!box) return;
        QSignalBlocker block(box);
        box->setChecked(checked);
    };
    auto setDoubleBlocked = [](QDoubleSpinBox* spin, qreal value) {
        if (!spin) return;
        QSignalBlocker block(spin);
        spin->setValue(value);
    };
    auto setIntBlocked = [](QSpinBox* spin, int value) {
        if (!spin) return;
        QSignalBlocker block(spin);
        spin->setValue(value);
    };

    setCheckedBlocked(m_audioAmplifyEnabledCheckBox, next.amplifyEnabled);
    setDoubleBlocked(m_audioAmplifyDbSpin, next.amplifyDb);
    setCheckedBlocked(m_audioNormalizeEnabledCheckBox, next.normalizeEnabled);
    setDoubleBlocked(m_audioNormalizeTargetDbSpin, next.normalizeTargetDb);
    setCheckedBlocked(m_audioStereoToMonoCheckBox, next.stereoToMonoEnabled);
    setCheckedBlocked(m_audioSelectiveNormalizeEnabledCheckBox, next.selectiveNormalizeEnabled);
    setDoubleBlocked(m_audioSelectiveNormalizeMinSecondsSpin, next.selectiveNormalizeMinSegmentSeconds);
    setDoubleBlocked(m_audioSelectiveNormalizePeakDbSpin, next.selectiveNormalizePeakDb);
    setIntBlocked(m_audioSelectiveNormalizePassesSpin, next.selectiveNormalizePasses);
    setCheckedBlocked(m_audioSelectiveNormalizeOverlayVisibleCheckBox, next.selectiveNormalizeOverlayVisible);
    setCheckedBlocked(m_audioTranscriptNormalizeEnabledCheckBox, next.transcriptNormalizeEnabled);
    setCheckedBlocked(m_audioWaveformPreviewProcessedCheckBox, next.waveformPreviewPostProcessing);
    setCheckedBlocked(m_audioPeakReductionEnabledCheckBox, next.peakReductionEnabled);
    setDoubleBlocked(m_audioPeakThresholdDbSpin, next.peakThresholdDb);
    setCheckedBlocked(m_audioLimiterEnabledCheckBox, next.limiterEnabled);
    setDoubleBlocked(m_audioLimiterThresholdDbSpin, next.limiterThresholdDb);
    setCheckedBlocked(m_audioCompressorEnabledCheckBox, next.compressorEnabled);
    setDoubleBlocked(m_audioCompressorThresholdDbSpin, next.compressorThresholdDb);
    setDoubleBlocked(m_audioCompressorRatioSpin, next.compressorRatio);
    setCheckedBlocked(m_audioSoftClipEnabledCheckBox, next.softClipEnabled);

    if (m_preview) {
        m_preview->setAudioDynamicsSettings(m_previewAudioDynamics);
    }
    if (m_audioEngine) {
        m_audioEngine->setTranscriptNormalizeEnabled(m_previewAudioDynamics.transcriptNormalizeEnabled);
        m_audioEngine->setTranscriptNormalizeRanges(
            m_previewAudioDynamics.transcriptNormalizeEnabled
                ? effectiveTranscriptNormalizeRanges()
                : QVector<ExportRangeSegment>{});
        m_audioEngine->setAudioDynamicsSettings(m_previewAudioDynamics);
    }

    appendRuntimePatch(&m_runtimePatchLog,
                       &m_runtimePatchSequence,
                       QStringLiteral("audio"),
                       patch);

    QJsonObject audio{
        {QStringLiteral("audioAmplifyEnabled"), m_previewAudioDynamics.amplifyEnabled},
        {QStringLiteral("audioAmplifyDb"), m_previewAudioDynamics.amplifyDb},
        {QStringLiteral("audioNormalizeEnabled"), m_previewAudioDynamics.normalizeEnabled},
        {QStringLiteral("audioNormalizeTargetDb"), m_previewAudioDynamics.normalizeTargetDb},
        {QStringLiteral("audioStereoToMonoEnabled"), m_previewAudioDynamics.stereoToMonoEnabled},
        {QStringLiteral("audioSelectiveNormalizeEnabled"), m_previewAudioDynamics.selectiveNormalizeEnabled},
        {QStringLiteral("audioSelectiveNormalizeMinSegmentSeconds"), m_previewAudioDynamics.selectiveNormalizeMinSegmentSeconds},
        {QStringLiteral("audioSelectiveNormalizePeakDb"), m_previewAudioDynamics.selectiveNormalizePeakDb},
        {QStringLiteral("audioSelectiveNormalizePasses"), m_previewAudioDynamics.selectiveNormalizePasses},
        {QStringLiteral("audioSelectiveNormalizeOverlayVisible"), m_previewAudioDynamics.selectiveNormalizeOverlayVisible},
        {QStringLiteral("audioTranscriptNormalizeEnabled"), m_previewAudioDynamics.transcriptNormalizeEnabled},
        {QStringLiteral("audioWaveformPreviewPostProcessing"), m_previewAudioDynamics.waveformPreviewPostProcessing},
        {QStringLiteral("audioPeakReductionEnabled"), m_previewAudioDynamics.peakReductionEnabled},
        {QStringLiteral("audioPeakThresholdDb"), m_previewAudioDynamics.peakThresholdDb},
        {QStringLiteral("audioLimiterEnabled"), m_previewAudioDynamics.limiterEnabled},
        {QStringLiteral("audioLimiterThresholdDb"), m_previewAudioDynamics.limiterThresholdDb},
        {QStringLiteral("audioCompressorEnabled"), m_previewAudioDynamics.compressorEnabled},
        {QStringLiteral("audioCompressorThresholdDb"), m_previewAudioDynamics.compressorThresholdDb},
        {QStringLiteral("audioCompressorRatio"), m_previewAudioDynamics.compressorRatio},
        {QStringLiteral("audioSoftClipEnabled"), m_previewAudioDynamics.softClipEnabled}
    };

    return QJsonObject{
        {QStringLiteral("ok"), true},
        {QStringLiteral("audio"), audio}
    };
}
