#include "vulkan_preview_surface.h"

#include "async_decoder.h"
#include "debug_controls.h"
#include "direct_vulkan_preview_presenter.h"
#include "timeline_cache.h"

#include <QDateTime>
#include <QJsonObject>

#include <algorithm>
#include <cmath>
#include <numeric>

namespace {

constexpr qint64 kAdaptivePlaybackTuningMinAdjustIntervalMs = 1200;

} // namespace

QJsonObject VulkanPreviewSurface::profilingSnapshot() const
{
    QJsonObject snapshot = m_presenter ? m_presenter->profilingSnapshot() : QJsonObject{};
    snapshot.insert(QStringLiteral("render_use_proxy_media"), m_useProxyMedia);
    snapshot.insert(QStringLiteral("vulkan_decode_preference"), editor::decodePreferenceToString(editor::debugDecodePreference()));
    snapshot.insert(QStringLiteral("vulkan_cpu_upload_permitted"), true);
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
    }
    if (m_cache) {
        snapshot.insert(QStringLiteral("cache_pending_visible_requests"), m_cache->pendingVisibleRequestCount());
        snapshot.insert(QStringLiteral("pending_visible_requests"),
                        m_cache->pendingVisibleDebugSnapshot(QDateTime::currentMSecsSinceEpoch()));
        QJsonObject cacheSnapshot{
            {QStringLiteral("hit_rate"), m_cache->cacheHitRate()},
            {QStringLiteral("total_memory_usage"), static_cast<qint64>(m_cache->totalMemoryUsage())},
            {QStringLiteral("total_cached_frames"), m_cache->totalCachedFrames()},
            {QStringLiteral("pending_visible_requests"), m_cache->pendingVisibleRequestCount()}
        };
        const QJsonObject residency = m_cache->cacheResidencySnapshot();
        for (auto it = residency.begin(); it != residency.end(); ++it) {
            cacheSnapshot.insert(it.key(), it.value());
        }
        snapshot.insert(QStringLiteral("cache"), cacheSnapshot);
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
    snapshot.insert(QStringLiteral("last_visible_request_pending"), m_lastVisibleRequestPending);
    snapshot.insert(QStringLiteral("last_visible_request_force_retry"), m_lastVisibleRequestForceRetry);
    snapshot.insert(QStringLiteral("last_visible_request_backlog"), m_lastVisibleRequestBacklog);
    snapshot.insert(QStringLiteral("last_visible_request_callback_payload"), m_lastVisibleRequestCallbackPayload);
    snapshot.insert(QStringLiteral("face_detections_query_debug"), m_lastFacedetectionsQueryDebug);
    snapshot.insert(QStringLiteral("playback_smoothness"), playbackSmoothnessSnapshot(snapshot));
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
