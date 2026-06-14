#include "vulkan_preview_surface.h"

#include "async_decoder.h"
#include "debug_controls.h"
#include "direct_vulkan_preview_presenter.h"
#include "playback_frame_pipeline.h"
#include "preview_speaker_profiles.h"
#include "timeline_cache.h"

#include <QDateTime>
#include <QJsonArray>
#include <QJsonObject>

#include <algorithm>
#include <cmath>
#include <numeric>

namespace {

constexpr qint64 kAdaptivePlaybackTuningMinAdjustIntervalMs = 1200;

QJsonArray pipelineStageHealthJson(const QVector<PreviewSurface::PipelineStageSnapshot>& stages)
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
            {QStringLiteral("facts"), stage.facts}
        });
    }
    return array;
}

} // namespace

QJsonObject VulkanPreviewSurface::profilingSnapshot() const
{
    QJsonObject snapshot = m_presenter ? m_presenter->profilingSnapshot() : QJsonObject{};
    snapshot.insert(QStringLiteral("render_use_proxy_media"), m_useProxyMedia);
    snapshot.insert(QStringLiteral("playing"), m_interaction.playing);
    snapshot.insert(QStringLiteral("current_sample"), static_cast<qint64>(m_interaction.currentSample));
    snapshot.insert(QStringLiteral("show_current_speaker_name"), m_interaction.showCurrentSpeakerName);
    snapshot.insert(QStringLiteral("show_current_speaker_organization"), m_interaction.showCurrentSpeakerOrganization);
    snapshot.insert(QStringLiteral("current_speaker_name_text_scale"), m_interaction.currentSpeakerNameTextScale);
    snapshot.insert(QStringLiteral("current_speaker_organization_text_scale"),
                    m_interaction.currentSpeakerOrganizationTextScale);
    snapshot.insert(QStringLiteral("current_speaker_name_y_position"),
                    m_interaction.currentSpeakerNameVerticalPosition);
    snapshot.insert(QStringLiteral("current_speaker_organization_y_position"),
                    m_interaction.currentSpeakerOrganizationVerticalPosition);
    snapshot.insert(QStringLiteral("playback_status_overlay_text"),
                    m_interaction.playbackStatusOverlayText);
    snapshot.insert(QStringLiteral("temporal_debug_overlay_enabled"),
                    editor::debugTemporalDebugOverlayEnabled());
    snapshot.insert(QStringLiteral("temporal_debug_overlay_text"),
                    m_interaction.temporalDebugOverlayText);
    const CurrentSpeakerLabel currentSpeakerLabel = currentSpeakerLabelForState(&m_interaction);
    snapshot.insert(QStringLiteral("current_speaker_label"), QJsonObject{
        {QStringLiteral("speaker_id"), currentSpeakerLabel.speakerId},
        {QStringLiteral("name"), currentSpeakerLabel.name},
        {QStringLiteral("organization"), currentSpeakerLabel.organization},
        {QStringLiteral("has_name"), !currentSpeakerLabel.name.trimmed().isEmpty()},
        {QStringLiteral("has_organization"), !currentSpeakerLabel.organization.trimmed().isEmpty()}
    });
    snapshot.insert(QStringLiteral("current_speaker_label_debug"),
                    m_interaction.playing
                        ? QJsonObject{
                              {QStringLiteral("status"),
                               QStringLiteral("suppressed_during_playback")},
                              {QStringLiteral("reason"),
                               QStringLiteral("candidate transcript scans are kept out of playback profiling snapshots")}}
                        : currentSpeakerLabelDebugForState(&m_interaction));
    snapshot.insert(QStringLiteral("vulkan_decode_preference"), editor::decodePreferenceToString(editor::debugDecodePreference()));
    snapshot.insert(QStringLiteral("vulkan_visible_decode_requires_direct_vulkan_payload"), true);
    snapshot.insert(QStringLiteral("vulkan_visible_cpu_upload_fallback_enabled"), false);
    snapshot.insert(QStringLiteral("vulkan_visible_zero_copy_contract"),
                    QStringLiteral("visible video frames require hardware/external Vulkan payloads; CPU image upload is rejected on the direct-Vulkan visible path"));
    snapshot.insert(QStringLiteral("vulkan_text_overlay_cpu_rasterization_enabled"), false);
    snapshot.insert(QStringLiteral("vulkan_text_overlay_qt_painter_enabled"), false);
    snapshot.insert(QStringLiteral("vulkan_speaker_label_gpu_text_enabled"), true);
    snapshot.insert(QStringLiteral("vulkan_transcript_overlay_gpu_text_enabled"), true);
    snapshot.insert(QStringLiteral("vulkan_text_overlay_gpu_native"), true);
    snapshot.insert(QStringLiteral("vulkan_text_overlay_contract"),
                    QStringLiteral("direct Vulkan preview draws speaker labels and transcript subtitles through a Vulkan glyph-atlas text pass; Qt/QPainter and whole-label CPU image uploads are disabled"));
    snapshot.insert(QStringLiteral("vulkan_overlay_preparation_thread"),
                    m_interaction.playing ? QStringLiteral("worker") : QStringLiteral("ui"));
    snapshot.insert(QStringLiteral("vulkan_overlay_preparation_gpu_zero_copy"), false);
    snapshot.insert(QStringLiteral("vulkan_overlay_preparation_contract"),
                    QStringLiteral("speaker/face overlay lookup prepares CPU metadata; the presenter draws the resulting overlay primitives on the GPU"));
    snapshot.insert(QStringLiteral("vulkan_overlay_worker_pending"), m_facedetectionsOverlayWorkerPending);
    snapshot.insert(QStringLiteral("vulkan_overlay_worker_pending_key"), m_pendingFacestreamOverlaySnapshotKey);
    snapshot.insert(QStringLiteral("vulkan_overlay_worker_queued_key"), m_queuedFacestreamOverlaySnapshotKey);
    snapshot.insert(QStringLiteral("vulkan_overlay_worker_queued_clip_count"),
                    m_queuedFacestreamOverlayRequestClips.size());
    snapshot.insert(QStringLiteral("vulkan_overlay_worker_applied_key"), m_appliedFacestreamOverlaySnapshotKey);
    snapshot.insert(QStringLiteral("vulkan_overlay_worker_started"),
                    static_cast<double>(m_facedetectionsOverlayWorkerStarted));
    snapshot.insert(QStringLiteral("vulkan_overlay_worker_applied"),
                    static_cast<double>(m_facedetectionsOverlayWorkerApplied));
    snapshot.insert(QStringLiteral("vulkan_overlay_worker_dropped"),
                    static_cast<double>(m_facedetectionsOverlayWorkerDropped));
    snapshot.insert(QStringLiteral("vulkan_overlay_worker_coalesced"),
                    static_cast<double>(m_facedetectionsOverlayWorkerCoalesced));
    snapshot.insert(QStringLiteral("vulkan_overlay_last_prep_ms"), m_lastFacedetectionsOverlayPrepMs);
    snapshot.insert(QStringLiteral("vulkan_overlay_last_apply_latency_ms"),
                    m_lastFacedetectionsOverlayApplyLatencyMs);
    snapshot.insert(QStringLiteral("vulkan_overlay_snapshot_age_ms"),
                    m_lastFacedetectionsOverlayAppliedAtMs > 0
                        ? qMax<qint64>(0, QDateTime::currentMSecsSinceEpoch() - m_lastFacedetectionsOverlayAppliedAtMs)
                        : -1);
    snapshot.insert(QStringLiteral("vulkan_overlay_last_request_clip_count"),
                    m_lastFacedetectionsOverlayRequestClipCount);
    snapshot.insert(QStringLiteral("vulkan_overlay_last_track_candidate_count"),
                    m_lastFacedetectionsOverlayTrackCandidateCount);
    snapshot.insert(QStringLiteral("vulkan_overlay_last_match_count"),
                    m_lastFacedetectionsOverlayMatchCount);
    snapshot.insert(QStringLiteral("vulkan_overlay_last_raw_detection_match_count"),
                    m_lastFacedetectionsRawDetectionMatchCount);
    snapshot.insert(QStringLiteral("vulkan_visible_backlog_limit"), m_playbackTuning.visibleBacklogLimit);
    snapshot.insert(QStringLiteral("vulkan_effective_lookahead_frames"), effectivePlaybackLookaheadFrames());
    snapshot.insert(QStringLiteral("vulkan_source_lookahead_frames"), m_playbackTuning.sourceLookaheadFrames);
    snapshot.insert(QStringLiteral("vulkan_proxy_lookahead_frames"), m_playbackTuning.proxyLookaheadFrames);
    snapshot.insert(QStringLiteral("vulkan_configured_visible_backlog_limit"), m_configuredPlaybackTuning.visibleBacklogLimit);
    snapshot.insert(QStringLiteral("vulkan_configured_source_lookahead_frames"), m_configuredPlaybackTuning.sourceLookaheadFrames);
    snapshot.insert(QStringLiteral("vulkan_configured_proxy_lookahead_frames"), m_configuredPlaybackTuning.proxyLookaheadFrames);
    snapshot.insert(QStringLiteral("vulkan_adaptive_playback_boost_level"), m_adaptivePlaybackBoostLevel);
    if (m_decoder) {
        snapshot.insert(QStringLiteral("decoder_worker_count"), m_decoder->workerCount());
        snapshot.insert(QStringLiteral("decoder_pending_requests"), m_decoder->pendingRequestCount());
        snapshot.insert(QStringLiteral("decoder_diagnostics"), m_decoder->diagnosticsSnapshot());
    }
    if (m_cache) {
        const qint64 nowMs = QDateTime::currentMSecsSinceEpoch();
        const QJsonObject visibleDecodeRetentionPolicy =
            m_cache->visibleDecodeRetentionPolicySnapshot(nowMs);
        const QJsonObject visibleDecodeDiagnostics = m_cache->visibleDecodeDiagnostics(nowMs);
        snapshot.insert(QStringLiteral("cache_pending_visible_requests"), m_cache->pendingVisibleRequestCount());
        snapshot.insert(QStringLiteral("pending_visible_requests"),
                        m_cache->pendingVisibleDebugSnapshot(nowMs));
        snapshot.insert(QStringLiteral("visible_decode_diagnostics"),
                        visibleDecodeDiagnostics);
        snapshot.insert(QStringLiteral("visible_decode_retention_policy"),
                        visibleDecodeRetentionPolicy);
        QJsonObject cacheSnapshot{
            {QStringLiteral("hit_rate"), m_cache->cacheHitRate()},
            {QStringLiteral("total_memory_usage"), static_cast<qint64>(m_cache->totalMemoryUsage())},
            {QStringLiteral("total_cached_frames"), m_cache->totalCachedFrames()},
            {QStringLiteral("pending_visible_requests"), m_cache->pendingVisibleRequestCount()},
            {QStringLiteral("visible_decode"), visibleDecodeDiagnostics},
            {QStringLiteral("visible_decode_retention_policy"), visibleDecodeRetentionPolicy}
        };
        const QJsonObject residency = m_cache->cacheResidencySnapshot();
        for (auto it = residency.begin(); it != residency.end(); ++it) {
            cacheSnapshot.insert(it.key(), it.value());
        }
        snapshot.insert(QStringLiteral("cache"), cacheSnapshot);
    }
    QJsonObject memoryOwnership;
    if (m_cache) {
        const QJsonObject cacheResidency = m_cache->cacheResidencySnapshot();
        memoryOwnership.insert(QStringLiteral("timeline_cache"), QJsonObject{
            {QStringLiteral("frames"), cacheResidency.value(QStringLiteral("total_cached_frames")).toInteger()},
            {QStringLiteral("hardware_frames"), cacheResidency.value(QStringLiteral("hardware_frames")).toInteger()},
            {QStringLiteral("gpu_texture_frames"), cacheResidency.value(QStringLiteral("gpu_texture_frames")).toInteger()},
            {QStringLiteral("cpu_backed_frames"), cacheResidency.value(QStringLiteral("cpu_backed_frames")).toInteger()},
            {QStringLiteral("cpu_bytes"), cacheResidency.value(QStringLiteral("cpu_bytes")).toInteger()},
            {QStringLiteral("gpu_bytes"), cacheResidency.value(QStringLiteral("gpu_bytes")).toInteger()},
            {QStringLiteral("by_clip"), cacheResidency.value(QStringLiteral("by_clip")).toObject()}
        });
    }
    if (m_playbackPipeline) {
        snapshot.insert(QStringLiteral("playback_pending_visible_requests"),
                        m_playbackPipeline->pendingVisibleRequestCount());
        snapshot.insert(QStringLiteral("playback_buffered_frames"),
                        m_playbackPipeline->bufferedFrameCount());
        snapshot.insert(QStringLiteral("playback_dropped_presentation_frames"),
                        m_playbackPipeline->droppedPresentationFrameCount());
        snapshot.insert(QStringLiteral("playback_decode"), m_playbackPipeline->decodeDiagnostics());
        const bool includeFrameTrace =
            !m_interaction.playing || editor::debugTemporalDebugOverlayEnabled();
        snapshot.insert(QStringLiteral("playback_frame_trace"),
                        includeFrameTrace ? m_playbackPipeline->frameTraceSnapshot(200)
                                          : QJsonArray{});
        snapshot.insert(QStringLiteral("playback_frame_trace_suppressed"),
                        !includeFrameTrace);
        memoryOwnership.insert(QStringLiteral("playback_frame_pipeline"),
                               m_playbackPipeline->bufferedFrameResidencySnapshot());
    }
    if (!memoryOwnership.isEmpty()) {
        snapshot.insert(QStringLiteral("memory_ownership"), memoryOwnership);
    }
    snapshot.insert(QStringLiteral("visible_request_attempts"), static_cast<double>(m_visibleRequestAttempts));
    snapshot.insert(QStringLiteral("visible_request_dispatched"), static_cast<double>(m_visibleRequestDispatched));
    snapshot.insert(QStringLiteral("visible_request_blocked"), static_cast<double>(m_visibleRequestBlocked));
    snapshot.insert(QStringLiteral("visible_request_callbacks"), static_cast<double>(m_visibleRequestCallbacks));
    snapshot.insert(QStringLiteral("visible_request_null_callbacks"), static_cast<double>(m_visibleRequestNullCallbacks));
    snapshot.insert(QStringLiteral("last_visible_request_clip_id"), m_lastVisibleRequestClipId);
    snapshot.insert(QStringLiteral("last_visible_request_frame"), static_cast<double>(m_lastVisibleRequestFrame));
    snapshot.insert(QStringLiteral("last_visible_request_decision"), m_lastVisibleRequestDecision);
    snapshot.insert(QStringLiteral("last_visible_request_block_reason"), m_lastVisibleRequestBlockReason);
    snapshot.insert(QStringLiteral("last_visible_request_cached"), m_lastVisibleRequestCached);
    snapshot.insert(QStringLiteral("last_visible_request_exact_cached"), m_lastVisibleRequestExactCached);
    snapshot.insert(QStringLiteral("last_visible_request_displayable_cached"), m_lastVisibleRequestDisplayableCached);
    snapshot.insert(QStringLiteral("last_visible_request_pending"), m_lastVisibleRequestPending);
    snapshot.insert(QStringLiteral("last_visible_request_force_retry"), m_lastVisibleRequestForceRetry);
    snapshot.insert(QStringLiteral("last_visible_request_backlog"), m_lastVisibleRequestBacklog);
    snapshot.insert(QStringLiteral("last_visible_request_callback_payload"), m_lastVisibleRequestCallbackPayload);
    snapshot.insert(QStringLiteral("frame_status_refresh_count"),
                    static_cast<double>(m_frameStatusRefreshCount));
    snapshot.insert(QStringLiteral("frame_status_last_refresh_ms"), m_lastFrameStatusRefreshMs);
    snapshot.insert(QStringLiteral("frame_status_max_refresh_ms"), m_maxFrameStatusRefreshMs);
    if (!m_interaction.vulkanFrameStatuses.isEmpty()) {
        const VulkanPreviewClipFrameStatus& status = m_interaction.vulkanFrameStatuses.constFirst();
        snapshot.insert(QStringLiteral("active_frame_selection"), status.frameSelection);
        snapshot.insert(QStringLiteral("active_requested_source_frame"), static_cast<qint64>(status.requestedSourceFrame));
        snapshot.insert(QStringLiteral("active_presented_source_frame"), static_cast<qint64>(status.presentedSourceFrame));
        snapshot.insert(QStringLiteral("active_frame_exact"), status.exact);
        snapshot.insert(QStringLiteral("active_frame_up_to_date"), status.upToDate);
        snapshot.insert(QStringLiteral("active_frame_not_up_to_date_failure"), status.currentFrameFailure);
        snapshot.insert(QStringLiteral("active_frame_stale_rejected"), status.staleFrameRejected);
    }
    snapshot.insert(QStringLiteral("face_detections_query_debug"), m_lastFacedetectionsQueryDebug);
    snapshot.insert(QStringLiteral("playback_smoothness"), playbackSmoothnessSnapshot(snapshot));
    QJsonObject playbackStageMetrics =
        snapshot.value(QStringLiteral("playback_pipeline_stages")).toObject();
    playbackStageMetrics.insert(
        QStringLiteral("timeline_input"),
        editor::playbackStageMetricToJson(m_timelineInputStageMetric, QStringLiteral("preview")));
    playbackStageMetrics.insert(
        QStringLiteral("source_mapping"),
        editor::playbackStageMetricToJson(m_sourceMappingStageMetric, QStringLiteral("preview")));
    playbackStageMetrics.insert(
        QStringLiteral("visible_request"),
        editor::playbackStageMetricToJson(m_visibleRequestStageMetric, QStringLiteral("preview")));
    playbackStageMetrics.insert(
        QStringLiteral("cache_lookup"),
        editor::playbackStageMetricToJson(m_cacheLookupStageMetric, QStringLiteral("preview")));
    playbackStageMetrics.insert(
        QStringLiteral("decoder_output"),
        editor::playbackStageMetricToJson(m_decoderOutputStageMetric, QStringLiteral("preview")));
    playbackStageMetrics.insert(
        QStringLiteral("frame_selection"),
        editor::playbackStageMetricToJson(m_frameSelectionStageMetric, QStringLiteral("preview")));
    playbackStageMetrics.insert(
        QStringLiteral("corrections_mask"),
        editor::playbackStageMetricToJson(m_correctionsMaskStageMetric, QStringLiteral("preview")));
    playbackStageMetrics.insert(
        QStringLiteral("effects_eval"),
        editor::playbackStageMetricToJson(m_effectsEvalStageMetric, QStringLiteral("preview")));
    playbackStageMetrics.insert(
        QStringLiteral("grading_shader"),
        editor::playbackStageMetricToJson(m_gradingShaderStageMetric, QStringLiteral("preview")));
    playbackStageMetrics.insert(
        QStringLiteral("transform"),
        editor::playbackStageMetricToJson(m_transformStageMetric, QStringLiteral("preview")));
    playbackStageMetrics.insert(
        QStringLiteral("overlay_prep"),
        editor::playbackStageMetricToJson(m_overlayPrepStageMetric, QStringLiteral("preview")));
    snapshot.insert(QStringLiteral("playback_pipeline_stages"), playbackStageMetrics);
    if (m_decoder && m_decoder->memoryBudget()) {
        const editor::MemoryBudget* budget = m_decoder->memoryBudget();
        snapshot.insert(QStringLiteral("memory_budget"), QJsonObject{
            {QStringLiteral("cpu_usage"), static_cast<qint64>(budget->currentCpuUsage())},
            {QStringLiteral("gpu_usage"), static_cast<qint64>(budget->currentGpuUsage())},
            {QStringLiteral("cpu_pressure"), budget->cpuPressure()},
            {QStringLiteral("gpu_pressure"), budget->gpuPressure()},
            {QStringLiteral("cpu_max"), static_cast<qint64>(budget->maxCpuMemory())},
            {QStringLiteral("gpu_max"), static_cast<qint64>(budget->maxGpuMemory())},
            {QStringLiteral("cpu_peak"), static_cast<qint64>(budget->peakCpuUsage())},
            {QStringLiteral("gpu_peak"), static_cast<qint64>(budget->peakGpuUsage())}
        });
        snapshot.insert(QStringLiteral("memory_budget_cpu_max_bytes"), static_cast<qint64>(budget->maxCpuMemory()));
        snapshot.insert(QStringLiteral("memory_budget_gpu_max_bytes"), static_cast<qint64>(budget->maxGpuMemory()));
        snapshot.insert(QStringLiteral("memory_budget_cpu_used_bytes"), static_cast<qint64>(budget->currentCpuUsage()));
        snapshot.insert(QStringLiteral("memory_budget_gpu_used_bytes"), static_cast<qint64>(budget->currentGpuUsage()));
        snapshot.insert(QStringLiteral("memory_budget_cpu_peak_bytes"), static_cast<qint64>(budget->peakCpuUsage()));
        snapshot.insert(QStringLiteral("memory_budget_gpu_peak_bytes"), static_cast<qint64>(budget->peakGpuUsage()));
        snapshot.insert(QStringLiteral("memory_budget_cpu_pressure"), budget->cpuPressure());
        snapshot.insert(QStringLiteral("memory_budget_gpu_pressure"), budget->gpuPressure());
    }
    return snapshot;
}

QJsonObject VulkanPreviewSurface::pipelineHealthSnapshot() const
{
    QJsonObject snapshot =
        m_presenter ? m_presenter->pipelineHealthSnapshot() : QJsonObject{{QStringLiteral("backend"), QStringLiteral("vulkan")}};
    snapshot.insert(QStringLiteral("ok"), true);
    snapshot.insert(QStringLiteral("render_use_proxy_media"), m_useProxyMedia);
    snapshot.insert(QStringLiteral("playing"), m_interaction.playing);
    snapshot.insert(QStringLiteral("current_frame"), static_cast<qint64>(m_interaction.currentFrame));
    snapshot.insert(QStringLiteral("current_sample"), static_cast<qint64>(m_interaction.currentSample));
    snapshot.insert(QStringLiteral("clip_count"), m_interaction.clips.size());
    snapshot.insert(QStringLiteral("show_current_speaker_name"), m_interaction.showCurrentSpeakerName);
    snapshot.insert(QStringLiteral("show_current_speaker_organization"), m_interaction.showCurrentSpeakerOrganization);
    snapshot.insert(QStringLiteral("temporal_debug_overlay_enabled"),
                    editor::debugTemporalDebugOverlayEnabled());
    snapshot.insert(QStringLiteral("temporal_debug_overlay_text"),
                    m_interaction.temporalDebugOverlayText);
    snapshot.insert(QStringLiteral("vulkan_decode_preference"),
                    editor::decodePreferenceToString(editor::debugDecodePreference()));
    snapshot.insert(QStringLiteral("vulkan_visible_decode_requires_direct_vulkan_payload"), true);
    snapshot.insert(QStringLiteral("vulkan_visible_cpu_upload_fallback_enabled"), true);
    snapshot.insert(QStringLiteral("vulkan_text_overlay_cpu_rasterization_enabled"), false);
    snapshot.insert(QStringLiteral("vulkan_text_overlay_qt_painter_enabled"), false);
    snapshot.insert(QStringLiteral("vulkan_speaker_label_gpu_text_enabled"), true);
    snapshot.insert(QStringLiteral("vulkan_transcript_overlay_gpu_text_enabled"), true);
    snapshot.insert(QStringLiteral("vulkan_text_overlay_gpu_native"), true);
    snapshot.insert(QStringLiteral("vulkan_overlay_preparation_thread"),
                    m_interaction.playing ? QStringLiteral("worker") : QStringLiteral("ui"));
    snapshot.insert(QStringLiteral("vulkan_overlay_worker_pending"), m_facedetectionsOverlayWorkerPending);
    snapshot.insert(QStringLiteral("vulkan_overlay_worker_started"),
                    static_cast<double>(m_facedetectionsOverlayWorkerStarted));
    snapshot.insert(QStringLiteral("vulkan_overlay_worker_applied"),
                    static_cast<double>(m_facedetectionsOverlayWorkerApplied));
    snapshot.insert(QStringLiteral("vulkan_overlay_last_prep_ms"), m_lastFacedetectionsOverlayPrepMs);
    snapshot.insert(QStringLiteral("vulkan_overlay_last_apply_latency_ms"),
                    m_lastFacedetectionsOverlayApplyLatencyMs);
    snapshot.insert(QStringLiteral("vulkan_overlay_snapshot_age_ms"),
                    m_lastFacedetectionsOverlayAppliedAtMs > 0
                        ? qMax<qint64>(0, QDateTime::currentMSecsSinceEpoch() - m_lastFacedetectionsOverlayAppliedAtMs)
                        : -1);
    snapshot.insert(QStringLiteral("vulkan_overlay_last_match_count"),
                    m_lastFacedetectionsOverlayMatchCount);
    snapshot.insert(QStringLiteral("vulkan_overlay_last_raw_detection_match_count"),
                    m_lastFacedetectionsRawDetectionMatchCount);
    snapshot.insert(QStringLiteral("vulkan_visible_backlog_limit"), m_playbackTuning.visibleBacklogLimit);
    snapshot.insert(QStringLiteral("vulkan_effective_lookahead_frames"), effectivePlaybackLookaheadFrames());
    snapshot.insert(QStringLiteral("vulkan_source_lookahead_frames"), m_playbackTuning.sourceLookaheadFrames);
    snapshot.insert(QStringLiteral("vulkan_proxy_lookahead_frames"), m_playbackTuning.proxyLookaheadFrames);
    snapshot.insert(QStringLiteral("vulkan_adaptive_playback_boost_level"), m_adaptivePlaybackBoostLevel);
    if (m_decoder) {
        snapshot.insert(QStringLiteral("decoder_worker_count"), m_decoder->workerCount());
        snapshot.insert(QStringLiteral("decoder_pending_requests"), m_decoder->pendingRequestCount());
        snapshot.insert(QStringLiteral("decoder_diagnostics"), m_decoder->diagnosticsSnapshot());
    }
    if (m_cache) {
        const qint64 nowMs = QDateTime::currentMSecsSinceEpoch();
        const QJsonObject visibleDecodeRetentionPolicy =
            m_cache->visibleDecodeRetentionPolicySnapshot(nowMs);
        const QJsonObject visibleDecodeDiagnostics = m_cache->visibleDecodeDiagnostics(nowMs);
        snapshot.insert(QStringLiteral("cache_pending_visible_requests"), m_cache->pendingVisibleRequestCount());
        snapshot.insert(QStringLiteral("visible_decode_diagnostics"),
                        visibleDecodeDiagnostics);
        snapshot.insert(QStringLiteral("visible_decode_retention_policy"),
                        visibleDecodeRetentionPolicy);
        snapshot.insert(QStringLiteral("cache"), QJsonObject{
            {QStringLiteral("hit_rate"), m_cache->cacheHitRate()},
            {QStringLiteral("total_memory_usage"), static_cast<qint64>(m_cache->totalMemoryUsage())},
            {QStringLiteral("total_cached_frames"), m_cache->totalCachedFrames()},
            {QStringLiteral("pending_visible_requests"), m_cache->pendingVisibleRequestCount()},
            {QStringLiteral("visible_decode_retention_policy"), visibleDecodeRetentionPolicy}
        });
    }
    if (m_playbackPipeline) {
        snapshot.insert(QStringLiteral("playback_pending_visible_requests"),
                        m_playbackPipeline->pendingVisibleRequestCount());
        snapshot.insert(QStringLiteral("playback_buffered_frames"),
                        m_playbackPipeline->bufferedFrameCount());
        snapshot.insert(QStringLiteral("playback_dropped_presentation_frames"),
                        m_playbackPipeline->droppedPresentationFrameCount());
        snapshot.insert(QStringLiteral("playback_decode"), m_playbackPipeline->decodeDiagnostics());
        const bool includeFrameTrace =
            !m_interaction.playing || editor::debugTemporalDebugOverlayEnabled();
        snapshot.insert(QStringLiteral("playback_frame_trace"),
                        includeFrameTrace ? m_playbackPipeline->frameTraceSnapshot(200)
                                          : QJsonArray{});
        snapshot.insert(QStringLiteral("playback_frame_trace_suppressed"),
                        !includeFrameTrace);
    }
    snapshot.insert(QStringLiteral("visible_request_attempts"), static_cast<double>(m_visibleRequestAttempts));
    snapshot.insert(QStringLiteral("visible_request_dispatched"), static_cast<double>(m_visibleRequestDispatched));
    snapshot.insert(QStringLiteral("visible_request_blocked"), static_cast<double>(m_visibleRequestBlocked));
    snapshot.insert(QStringLiteral("visible_request_callbacks"), static_cast<double>(m_visibleRequestCallbacks));
    snapshot.insert(QStringLiteral("visible_request_null_callbacks"), static_cast<double>(m_visibleRequestNullCallbacks));
    snapshot.insert(QStringLiteral("last_visible_request_frame"), static_cast<double>(m_lastVisibleRequestFrame));
    snapshot.insert(QStringLiteral("last_visible_request_decision"), m_lastVisibleRequestDecision);
    snapshot.insert(QStringLiteral("last_visible_request_block_reason"), m_lastVisibleRequestBlockReason);
    snapshot.insert(QStringLiteral("last_visible_request_cached"), m_lastVisibleRequestCached);
    snapshot.insert(QStringLiteral("last_visible_request_exact_cached"), m_lastVisibleRequestExactCached);
    snapshot.insert(QStringLiteral("last_visible_request_displayable_cached"), m_lastVisibleRequestDisplayableCached);
    snapshot.insert(QStringLiteral("last_visible_request_pending"), m_lastVisibleRequestPending);
    snapshot.insert(QStringLiteral("last_visible_request_backlog"), m_lastVisibleRequestBacklog);
    snapshot.insert(QStringLiteral("last_visible_request_callback_payload"), m_lastVisibleRequestCallbackPayload);
    snapshot.insert(QStringLiteral("frame_status_refresh_count"),
                    static_cast<double>(m_frameStatusRefreshCount));
    snapshot.insert(QStringLiteral("frame_status_last_refresh_ms"), m_lastFrameStatusRefreshMs);
    snapshot.insert(QStringLiteral("frame_status_max_refresh_ms"), m_maxFrameStatusRefreshMs);
    if (!m_interaction.vulkanFrameStatuses.isEmpty()) {
        const VulkanPreviewClipFrameStatus& status = m_interaction.vulkanFrameStatuses.constFirst();
        snapshot.insert(QStringLiteral("active_frame_selection"), status.frameSelection);
        snapshot.insert(QStringLiteral("active_requested_source_frame"), static_cast<qint64>(status.requestedSourceFrame));
        snapshot.insert(QStringLiteral("active_presented_source_frame"), static_cast<qint64>(status.presentedSourceFrame));
        snapshot.insert(QStringLiteral("active_frame_exact"), status.exact);
        snapshot.insert(QStringLiteral("active_frame_up_to_date"), status.upToDate);
        snapshot.insert(QStringLiteral("active_frame_not_up_to_date_failure"), status.currentFrameFailure);
        snapshot.insert(QStringLiteral("active_frame_stale_rejected"), status.staleFrameRejected);
    }
    snapshot.insert(QStringLiteral("playback_smoothness"), playbackSmoothnessSnapshot(snapshot));
    QJsonObject playbackStageMetrics =
        snapshot.value(QStringLiteral("playback_pipeline_stages")).toObject();
    playbackStageMetrics.insert(
        QStringLiteral("timeline_input"),
        editor::playbackStageMetricToJson(m_timelineInputStageMetric, QStringLiteral("preview")));
    playbackStageMetrics.insert(
        QStringLiteral("source_mapping"),
        editor::playbackStageMetricToJson(m_sourceMappingStageMetric, QStringLiteral("preview")));
    playbackStageMetrics.insert(
        QStringLiteral("visible_request"),
        editor::playbackStageMetricToJson(m_visibleRequestStageMetric, QStringLiteral("preview")));
    playbackStageMetrics.insert(
        QStringLiteral("cache_lookup"),
        editor::playbackStageMetricToJson(m_cacheLookupStageMetric, QStringLiteral("preview")));
    playbackStageMetrics.insert(
        QStringLiteral("decoder_output"),
        editor::playbackStageMetricToJson(m_decoderOutputStageMetric, QStringLiteral("preview")));
    playbackStageMetrics.insert(
        QStringLiteral("frame_selection"),
        editor::playbackStageMetricToJson(m_frameSelectionStageMetric, QStringLiteral("preview")));
    playbackStageMetrics.insert(
        QStringLiteral("corrections_mask"),
        editor::playbackStageMetricToJson(m_correctionsMaskStageMetric, QStringLiteral("preview")));
    playbackStageMetrics.insert(
        QStringLiteral("effects_eval"),
        editor::playbackStageMetricToJson(m_effectsEvalStageMetric, QStringLiteral("preview")));
    playbackStageMetrics.insert(
        QStringLiteral("grading_shader"),
        editor::playbackStageMetricToJson(m_gradingShaderStageMetric, QStringLiteral("preview")));
    playbackStageMetrics.insert(
        QStringLiteral("transform"),
        editor::playbackStageMetricToJson(m_transformStageMetric, QStringLiteral("preview")));
    playbackStageMetrics.insert(
        QStringLiteral("overlay_prep"),
        editor::playbackStageMetricToJson(m_overlayPrepStageMetric, QStringLiteral("preview")));
    snapshot.insert(QStringLiteral("playback_pipeline_stages"), playbackStageMetrics);
    if (m_decoder && m_decoder->memoryBudget()) {
        const editor::MemoryBudget* budget = m_decoder->memoryBudget();
        snapshot.insert(QStringLiteral("memory_budget"), QJsonObject{
            {QStringLiteral("cpu_usage"), static_cast<qint64>(budget->currentCpuUsage())},
            {QStringLiteral("gpu_usage"), static_cast<qint64>(budget->currentGpuUsage())},
            {QStringLiteral("cpu_pressure"), budget->cpuPressure()},
            {QStringLiteral("gpu_pressure"), budget->gpuPressure()},
            {QStringLiteral("cpu_max"), static_cast<qint64>(budget->maxCpuMemory())},
            {QStringLiteral("gpu_max"), static_cast<qint64>(budget->maxGpuMemory())}
        });
    }
    snapshot.insert(QStringLiteral("pipeline_stages"), pipelineStageHealthJson(livePipelineSnapshots()));
    return snapshot;
}

void VulkanPreviewSurface::resetProfilingStats()
{
    if (m_presenter) {
        m_presenter->resetProfilingStats();
    }
    m_playbackSmoothnessSamples.clear();
    m_adaptivePlaybackBoostLevel = 0;
    m_lastAdaptivePlaybackTuningAdjustMs = 0;
    applyAdaptivePlaybackTuning();
}

void VulkanPreviewSurface::recordPlaybackSmoothnessSample(int exactCount,
                                                          int approxCount,
                                                          int missingCount,
                                                          int64_t maxFrameLag)
{
    const qint64 nowMs = QDateTime::currentMSecsSinceEpoch();
    PlaybackSmoothnessSample sample;
    sample.timestampMs = nowMs;
    sample.exactCount = exactCount;
    sample.approxCount = approxCount;
    sample.missingCount = missingCount;
    sample.maxFrameLag = maxFrameLag;
    sample.playing = m_interaction.playing;
    sample.visibleRequestAttempts = m_visibleRequestAttempts;
    sample.visibleRequestDispatched = m_visibleRequestDispatched;
    sample.visibleRequestBlocked = m_visibleRequestBlocked;
    if (m_presenter) {
        const QJsonObject presenterSnapshot = m_presenter->profilingSnapshot();
        sample.lastUploadMs = presenterSnapshot.value(QStringLiteral("last_handoff_upload_ms")).toDouble();
        sample.handoffAttempts =
            static_cast<qint64>(presenterSnapshot.value(QStringLiteral("handoff_attempts")).toDouble());
        sample.handoffSuccesses =
            static_cast<qint64>(presenterSnapshot.value(QStringLiteral("handoff_successes")).toDouble());
        sample.handoffFailures =
            static_cast<qint64>(presenterSnapshot.value(QStringLiteral("handoff_failures")).toDouble());
        sample.presentedFrames =
            static_cast<qint64>(presenterSnapshot.value(QStringLiteral("presented_frames")).toDouble());
    }
    m_playbackSmoothnessSamples.push_back(sample);

    constexpr qint64 kWindowMs = 5000;
    constexpr int kMaxSamples = 240;
    while (!m_playbackSmoothnessSamples.isEmpty() &&
           (m_playbackSmoothnessSamples.constFirst().timestampMs < nowMs - kWindowMs ||
            m_playbackSmoothnessSamples.size() > kMaxSamples)) {
        m_playbackSmoothnessSamples.removeFirst();
    }
    updateAdaptivePlaybackTuning(nowMs);
}

void VulkanPreviewSurface::applyAdaptivePlaybackTuning()
{
    PlaybackTuning effective = m_configuredPlaybackTuning;
    const int level = qBound(0, m_adaptivePlaybackBoostLevel, 3);
    effective.visibleBacklogLimit =
        qBound(1, effective.visibleBacklogLimit + level, 8);
    effective.sourceLookaheadFrames =
        qBound(1, effective.sourceLookaheadFrames + (level * 2), 16);
    effective.proxyLookaheadFrames =
        qBound(1, effective.proxyLookaheadFrames + (level * 2), 24);
    if (m_playbackTuning.visibleBacklogLimit == effective.visibleBacklogLimit &&
        m_playbackTuning.sourceLookaheadFrames == effective.sourceLookaheadFrames &&
        m_playbackTuning.proxyLookaheadFrames == effective.proxyLookaheadFrames) {
        return;
    }
    m_playbackTuning = effective;
    if (m_cache) {
        m_cache->setLookaheadFrames(effectivePlaybackLookaheadFrames());
    }
}

void VulkanPreviewSurface::updateAdaptivePlaybackTuning(qint64 nowMs)
{
    if (!m_interaction.playing) {
        return;
    }
    if (nowMs - m_lastAdaptivePlaybackTuningAdjustMs < kAdaptivePlaybackTuningMinAdjustIntervalMs) {
        return;
    }

    constexpr qint64 kWindowMs = 5000;
    int windowSamples = 0;
    qint64 exactTotal = 0;
    qint64 approxTotal = 0;
    qint64 missingTotal = 0;
    qint64 lateSamples = 0;
    qint64 frameLagTotal = 0;
    for (const PlaybackSmoothnessSample& sample : m_playbackSmoothnessSamples) {
        if (!sample.playing || sample.timestampMs < nowMs - kWindowMs) {
            continue;
        }
        ++windowSamples;
        exactTotal += sample.exactCount;
        approxTotal += sample.approxCount;
        missingTotal += sample.missingCount;
        frameLagTotal += sample.maxFrameLag;
        if (sample.maxFrameLag > 0) {
            ++lateSamples;
        }
    }
    if (windowSamples < 24) {
        return;
    }

    const qint64 frameSamples = exactTotal + approxTotal + missingTotal;
    const qint64 availableSamples = exactTotal + approxTotal;
    const double exactHitRate =
        frameSamples > 0 ? static_cast<double>(exactTotal) / static_cast<double>(frameSamples) : 1.0;
    const double lateSampleRate =
        windowSamples > 0 ? static_cast<double>(lateSamples) / static_cast<double>(windowSamples) : 0.0;
    const double avgFrameLag =
        availableSamples > 0 ? static_cast<double>(frameLagTotal) / static_cast<double>(availableSamples) : 0.0;
    const int decoderPending = m_decoder ? m_decoder->pendingRequestCount() : 0;
    const int workerCount = m_decoder ? qMax(1, m_decoder->workerCount()) : 1;
    const bool starved =
        lateSampleRate > 0.35 ||
        exactHitRate < 0.55 ||
        avgFrameLag > 8.0 ||
        decoderPending >= qMax(2, workerCount / 2);
    const bool recovered =
        lateSampleRate < 0.10 &&
        exactHitRate > 0.90 &&
        avgFrameLag < 2.0 &&
        decoderPending == 0;

    int nextLevel = m_adaptivePlaybackBoostLevel;
    if (starved && nextLevel < 3) {
        ++nextLevel;
    } else if (recovered && nextLevel > 0) {
        --nextLevel;
    } else {
        return;
    }

    m_adaptivePlaybackBoostLevel = nextLevel;
    m_lastAdaptivePlaybackTuningAdjustMs = nowMs;
    applyAdaptivePlaybackTuning();
}

QJsonObject VulkanPreviewSurface::playbackSmoothnessSnapshot(const QJsonObject& presenterSnapshot) const
{
    constexpr qint64 kWindowMs = 5000;
    const qint64 nowMs = QDateTime::currentMSecsSinceEpoch();
    QVector<PlaybackSmoothnessSample> window;
    window.reserve(m_playbackSmoothnessSamples.size());
    for (const PlaybackSmoothnessSample& sample : m_playbackSmoothnessSamples) {
        if (sample.timestampMs >= nowMs - kWindowMs) {
            window.push_back(sample);
        }
    }

    QJsonObject smoothness{
        {QStringLiteral("window_ms"), kWindowMs},
        {QStringLiteral("sample_count"), window.size()},
        {QStringLiteral("playing"), m_interaction.playing},
        {QStringLiteral("playing_sample_count"), 0},
        {QStringLiteral("exact_hit_rate"), 0.0},
        {QStringLiteral("approximate_hit_rate"), 0.0},
        {QStringLiteral("missing_frame_rate"), 0.0},
        {QStringLiteral("current_frame_failure_rate"), 0.0},
        {QStringLiteral("late_sample_rate"), 0.0},
        {QStringLiteral("avg_frame_lag"), 0.0},
        {QStringLiteral("max_frame_lag"), 0},
        {QStringLiteral("avg_handoff_upload_ms"), 0.0},
        {QStringLiteral("p95_handoff_upload_ms"), 0.0},
        {QStringLiteral("max_handoff_upload_ms"), 0.0},
        {QStringLiteral("visible_request_attempt_rate"), 0.0},
        {QStringLiteral("visible_request_dispatch_rate"), 0.0},
        {QStringLiteral("visible_request_block_rate"), 0.0},
        {QStringLiteral("visible_request_blocked_fraction"), 0.0},
        {QStringLiteral("handoff_success_rate"), 0.0},
        {QStringLiteral("presented_fps_estimate"), 0.0},
        {QStringLiteral("current_decoder_pending_requests"),
         m_decoder ? m_decoder->pendingRequestCount() : 0},
        {QStringLiteral("current_decoder_worker_count"),
         m_decoder ? m_decoder->workerCount() : 0},
        {QStringLiteral("current_last_handoff_upload_ms"),
         presenterSnapshot.value(QStringLiteral("last_handoff_upload_ms")).toDouble()},
        {QStringLiteral("current_visible_request_backlog"), m_lastVisibleRequestBacklog},
        {QStringLiteral("current_last_visible_request_block_reason"), m_lastVisibleRequestBlockReason}
    };

    if (window.isEmpty()) {
        return smoothness;
    }

    int playingSampleCount = 0;
    qint64 exactTotal = 0;
    qint64 approxTotal = 0;
    qint64 missingTotal = 0;
    qint64 lateSamples = 0;
    qint64 frameLagTotal = 0;
    qint64 maxFrameLag = 0;
    QVector<double> uploadSamples;
    uploadSamples.reserve(window.size());

    for (const PlaybackSmoothnessSample& sample : window) {
        if (sample.playing) {
            ++playingSampleCount;
        }
        exactTotal += sample.exactCount;
        approxTotal += sample.approxCount;
        missingTotal += sample.missingCount;
        frameLagTotal += sample.maxFrameLag;
        maxFrameLag = qMax(maxFrameLag, sample.maxFrameLag);
        if (sample.maxFrameLag > 0) {
            ++lateSamples;
        }
        if (sample.lastUploadMs > 0.0) {
            uploadSamples.push_back(sample.lastUploadMs);
        }
    }

    const qint64 frameSamples = exactTotal + approxTotal + missingTotal;
    const qint64 availableSamples = exactTotal + approxTotal;
    smoothness[QStringLiteral("playing_sample_count")] = playingSampleCount;
    if (frameSamples > 0) {
        smoothness[QStringLiteral("exact_hit_rate")] =
            static_cast<double>(exactTotal) / static_cast<double>(frameSamples);
        smoothness[QStringLiteral("approximate_hit_rate")] =
            static_cast<double>(approxTotal) / static_cast<double>(frameSamples);
        smoothness[QStringLiteral("missing_frame_rate")] =
            static_cast<double>(missingTotal) / static_cast<double>(frameSamples);
        smoothness[QStringLiteral("current_frame_failure_rate")] =
            static_cast<double>(approxTotal + missingTotal) / static_cast<double>(frameSamples);
    }
    if (availableSamples > 0) {
        smoothness[QStringLiteral("avg_frame_lag")] =
            static_cast<double>(frameLagTotal) / static_cast<double>(availableSamples);
    }
    smoothness[QStringLiteral("max_frame_lag")] = maxFrameLag;
    smoothness[QStringLiteral("late_sample_rate")] =
        static_cast<double>(lateSamples) / static_cast<double>(window.size());

    if (!uploadSamples.isEmpty()) {
        std::sort(uploadSamples.begin(), uploadSamples.end());
        const double uploadSum = std::accumulate(uploadSamples.begin(), uploadSamples.end(), 0.0);
        const int p95Index = qBound(0,
                                    static_cast<int>(std::ceil(uploadSamples.size() * 0.95)) - 1,
                                    uploadSamples.size() - 1);
        smoothness[QStringLiteral("avg_handoff_upload_ms")] =
            uploadSum / static_cast<double>(uploadSamples.size());
        smoothness[QStringLiteral("p95_handoff_upload_ms")] = uploadSamples.at(p95Index);
        smoothness[QStringLiteral("max_handoff_upload_ms")] = uploadSamples.constLast();
    }

    const PlaybackSmoothnessSample& first = window.constFirst();
    const PlaybackSmoothnessSample& last = window.constLast();
    const qint64 elapsedMs = qMax<qint64>(1, last.timestampMs - first.timestampMs);
    const double elapsedSeconds = static_cast<double>(elapsedMs) / 1000.0;
    const qint64 visibleAttemptsDelta =
        qMax<qint64>(0, last.visibleRequestAttempts - first.visibleRequestAttempts);
    const qint64 visibleDispatchedDelta =
        qMax<qint64>(0, last.visibleRequestDispatched - first.visibleRequestDispatched);
    const qint64 visibleBlockedDelta =
        qMax<qint64>(0, last.visibleRequestBlocked - first.visibleRequestBlocked);
    const qint64 handoffAttemptsDelta =
        qMax<qint64>(0, last.handoffAttempts - first.handoffAttempts);
    const qint64 handoffSuccessesDelta =
        qMax<qint64>(0, last.handoffSuccesses - first.handoffSuccesses);
    const qint64 presentedFramesDelta =
        qMax<qint64>(0, last.presentedFrames - first.presentedFrames);

    smoothness[QStringLiteral("visible_request_attempt_rate")] =
        static_cast<double>(visibleAttemptsDelta) / elapsedSeconds;
    smoothness[QStringLiteral("visible_request_dispatch_rate")] =
        static_cast<double>(visibleDispatchedDelta) / elapsedSeconds;
    smoothness[QStringLiteral("visible_request_block_rate")] =
        static_cast<double>(visibleBlockedDelta) / elapsedSeconds;
    if (visibleAttemptsDelta > 0) {
        smoothness[QStringLiteral("visible_request_blocked_fraction")] =
            static_cast<double>(visibleBlockedDelta) / static_cast<double>(visibleAttemptsDelta);
    }
    if (handoffAttemptsDelta > 0) {
        smoothness[QStringLiteral("handoff_success_rate")] =
            static_cast<double>(handoffSuccessesDelta) / static_cast<double>(handoffAttemptsDelta);
    }
    smoothness[QStringLiteral("presented_fps_estimate")] =
        static_cast<double>(presentedFramesDelta) / elapsedSeconds;

    return smoothness;
}
