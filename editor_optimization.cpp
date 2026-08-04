#include "editor.h"
#include "debug_controls.h"

#include <QCoreApplication>
#include <QDateTime>
#include <QScopeGuard>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QGuiApplication>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSaveFile>
#include <QSysInfo>
#include <QThread>

#include <limits>

using namespace editor;

namespace {

struct OptimizationCandidate {
    QString name;
    EditorWindow::OptimizedPreviewProfile profile;
};

QString backendFamilyForName(const QString& backendName)
{
    const QString lowered = backendName.trimmed().toLower();
    if (lowered.contains(QStringLiteral("vulkan"))) {
        return QStringLiteral("vulkan");
    }
    return lowered.isEmpty() ? QStringLiteral("unknown") : lowered;
}

int positiveIntOr(const QJsonObject& object, const QString& key, int fallback)
{
    const int value = object.value(key).toInt(fallback);
    return value > 0 ? value : fallback;
}

QJsonObject tuningToJson(const PreviewSurface::PlaybackTuning& tuning)
{
    return QJsonObject{
        {QStringLiteral("visible_backlog_limit"), tuning.visibleBacklogLimit},
        {QStringLiteral("source_lookahead_frames"), tuning.sourceLookaheadFrames},
        {QStringLiteral("proxy_lookahead_frames"), tuning.proxyLookaheadFrames},
        {QStringLiteral("prefetch_max_queue_depth"), tuning.prefetchMaxQueueDepth},
        {QStringLiteral("prefetch_max_inflight"), tuning.prefetchMaxInflight},
        {QStringLiteral("prefetch_max_per_tick"), tuning.prefetchMaxPerTick},
        {QStringLiteral("visible_queue_reserve"), tuning.visibleQueueReserve},
        {QStringLiteral("playback_window_ahead"), tuning.playbackWindowAhead},
        {QStringLiteral("decode_autotune_enabled"), tuning.decodeAutotuneEnabled},
        {QStringLiteral("decode_autotune_max_boost_level"), tuning.decodeAutotuneMaxBoostLevel},
        {QStringLiteral("decode_autotune_min_adjust_interval_ms"), tuning.decodeAutotuneMinAdjustIntervalMs},
        {QStringLiteral("decode_autotune_window_ms"), tuning.decodeAutotuneWindowMs},
        {QStringLiteral("decode_autotune_min_samples"), tuning.decodeAutotuneMinSamples},
        {QStringLiteral("decode_autotune_starved_late_rate_permille"), tuning.decodeAutotuneStarvedLateRatePermille},
        {QStringLiteral("decode_autotune_starved_exact_hit_rate_permille"), tuning.decodeAutotuneStarvedExactHitRatePermille},
        {QStringLiteral("decode_autotune_starved_avg_frame_lag"), tuning.decodeAutotuneStarvedAvgFrameLag},
        {QStringLiteral("decode_autotune_recovered_late_rate_permille"), tuning.decodeAutotuneRecoveredLateRatePermille},
        {QStringLiteral("decode_autotune_recovered_exact_hit_rate_permille"), tuning.decodeAutotuneRecoveredExactHitRatePermille},
        {QStringLiteral("decode_autotune_recovered_avg_frame_lag"), tuning.decodeAutotuneRecoveredAvgFrameLag},
        {QStringLiteral("decode_autotune_max_visible_backlog_limit"), tuning.decodeAutotuneMaxVisibleBacklogLimit},
        {QStringLiteral("decode_autotune_max_source_lookahead_frames"), tuning.decodeAutotuneMaxSourceLookaheadFrames},
        {QStringLiteral("decode_autotune_max_proxy_lookahead_frames"), tuning.decodeAutotuneMaxProxyLookaheadFrames},
        {QStringLiteral("decode_autotune_visible_backlog_step"), tuning.decodeAutotuneVisibleBacklogStep},
        {QStringLiteral("decode_autotune_lookahead_step"), tuning.decodeAutotuneLookaheadStep},
        {QStringLiteral("decode_autotune_max_prefetch_max_queue_depth"), tuning.decodeAutotuneMaxPrefetchMaxQueueDepth},
        {QStringLiteral("decode_autotune_max_prefetch_max_inflight"), tuning.decodeAutotuneMaxPrefetchMaxInflight},
        {QStringLiteral("decode_autotune_max_prefetch_max_per_tick"), tuning.decodeAutotuneMaxPrefetchMaxPerTick},
        {QStringLiteral("decode_autotune_max_visible_queue_reserve"), tuning.decodeAutotuneMaxVisibleQueueReserve},
        {QStringLiteral("decode_autotune_max_playback_window_ahead"), tuning.decodeAutotuneMaxPlaybackWindowAhead},
        {QStringLiteral("decode_autotune_prefetch_queue_depth_step"), tuning.decodeAutotunePrefetchQueueDepthStep},
        {QStringLiteral("decode_autotune_prefetch_concurrency_step"), tuning.decodeAutotunePrefetchConcurrencyStep},
        {QStringLiteral("decode_autotune_visible_queue_reserve_step"), tuning.decodeAutotuneVisibleQueueReserveStep},
        {QStringLiteral("decode_autotune_playback_window_ahead_step"), tuning.decodeAutotunePlaybackWindowAheadStep}
    };
}

PreviewSurface::PlaybackTuning tuningFromJson(const QJsonObject& object,
                                              const PreviewSurface::PlaybackTuning& fallback)
{
    PreviewSurface::PlaybackTuning tuning = fallback;
    tuning.visibleBacklogLimit =
        positiveIntOr(object, QStringLiteral("visible_backlog_limit"), fallback.visibleBacklogLimit);
    tuning.sourceLookaheadFrames =
        positiveIntOr(object, QStringLiteral("source_lookahead_frames"), fallback.sourceLookaheadFrames);
    tuning.proxyLookaheadFrames =
        positiveIntOr(object, QStringLiteral("proxy_lookahead_frames"), fallback.proxyLookaheadFrames);
    tuning.prefetchMaxQueueDepth =
        positiveIntOr(object, QStringLiteral("prefetch_max_queue_depth"), fallback.prefetchMaxQueueDepth);
    tuning.prefetchMaxInflight =
        positiveIntOr(object, QStringLiteral("prefetch_max_inflight"), fallback.prefetchMaxInflight);
    tuning.prefetchMaxPerTick =
        positiveIntOr(object, QStringLiteral("prefetch_max_per_tick"), fallback.prefetchMaxPerTick);
    tuning.visibleQueueReserve =
        qMax(0, object.value(QStringLiteral("visible_queue_reserve")).toInt(fallback.visibleQueueReserve));
    tuning.playbackWindowAhead =
        positiveIntOr(object, QStringLiteral("playback_window_ahead"), fallback.playbackWindowAhead);
    if (object.contains(QStringLiteral("decode_autotune_enabled")) &&
        object.value(QStringLiteral("decode_autotune_enabled")).isBool()) {
        tuning.decodeAutotuneEnabled =
            object.value(QStringLiteral("decode_autotune_enabled")).toBool();
    }
    tuning.decodeAutotuneMaxBoostLevel =
        qBound(0,
               object.value(QStringLiteral("decode_autotune_max_boost_level"))
                   .toInt(fallback.decodeAutotuneMaxBoostLevel),
               8);
    tuning.decodeAutotuneMinAdjustIntervalMs =
        positiveIntOr(object,
                      QStringLiteral("decode_autotune_min_adjust_interval_ms"),
                      fallback.decodeAutotuneMinAdjustIntervalMs);
    tuning.decodeAutotuneWindowMs =
        positiveIntOr(object,
                      QStringLiteral("decode_autotune_window_ms"),
                      fallback.decodeAutotuneWindowMs);
    tuning.decodeAutotuneMinSamples =
        positiveIntOr(object,
                      QStringLiteral("decode_autotune_min_samples"),
                      fallback.decodeAutotuneMinSamples);
    tuning.decodeAutotuneStarvedLateRatePermille =
        qBound(0,
               object.value(QStringLiteral("decode_autotune_starved_late_rate_permille"))
                   .toInt(fallback.decodeAutotuneStarvedLateRatePermille),
               1000);
    tuning.decodeAutotuneStarvedExactHitRatePermille =
        qBound(0,
               object.value(QStringLiteral("decode_autotune_starved_exact_hit_rate_permille"))
                   .toInt(fallback.decodeAutotuneStarvedExactHitRatePermille),
               1000);
    tuning.decodeAutotuneStarvedAvgFrameLag =
        positiveIntOr(object,
                      QStringLiteral("decode_autotune_starved_avg_frame_lag"),
                      fallback.decodeAutotuneStarvedAvgFrameLag);
    tuning.decodeAutotuneRecoveredLateRatePermille =
        qBound(0,
               object.value(QStringLiteral("decode_autotune_recovered_late_rate_permille"))
                   .toInt(fallback.decodeAutotuneRecoveredLateRatePermille),
               1000);
    tuning.decodeAutotuneRecoveredExactHitRatePermille =
        qBound(0,
               object.value(QStringLiteral("decode_autotune_recovered_exact_hit_rate_permille"))
                   .toInt(fallback.decodeAutotuneRecoveredExactHitRatePermille),
               1000);
    tuning.decodeAutotuneRecoveredAvgFrameLag =
        positiveIntOr(object,
                      QStringLiteral("decode_autotune_recovered_avg_frame_lag"),
                      fallback.decodeAutotuneRecoveredAvgFrameLag);
    tuning.decodeAutotuneMaxVisibleBacklogLimit =
        positiveIntOr(object,
                      QStringLiteral("decode_autotune_max_visible_backlog_limit"),
                      fallback.decodeAutotuneMaxVisibleBacklogLimit);
    tuning.decodeAutotuneMaxSourceLookaheadFrames =
        positiveIntOr(object,
                      QStringLiteral("decode_autotune_max_source_lookahead_frames"),
                      fallback.decodeAutotuneMaxSourceLookaheadFrames);
    tuning.decodeAutotuneMaxProxyLookaheadFrames =
        positiveIntOr(object,
                      QStringLiteral("decode_autotune_max_proxy_lookahead_frames"),
                      fallback.decodeAutotuneMaxProxyLookaheadFrames);
    tuning.decodeAutotuneVisibleBacklogStep =
        positiveIntOr(object,
                      QStringLiteral("decode_autotune_visible_backlog_step"),
                      fallback.decodeAutotuneVisibleBacklogStep);
    tuning.decodeAutotuneLookaheadStep =
        positiveIntOr(object,
                      QStringLiteral("decode_autotune_lookahead_step"),
                      fallback.decodeAutotuneLookaheadStep);
    tuning.decodeAutotuneMaxPrefetchMaxQueueDepth =
        positiveIntOr(object,
                      QStringLiteral("decode_autotune_max_prefetch_max_queue_depth"),
                      fallback.decodeAutotuneMaxPrefetchMaxQueueDepth);
    tuning.decodeAutotuneMaxPrefetchMaxInflight =
        positiveIntOr(object,
                      QStringLiteral("decode_autotune_max_prefetch_max_inflight"),
                      fallback.decodeAutotuneMaxPrefetchMaxInflight);
    tuning.decodeAutotuneMaxPrefetchMaxPerTick =
        positiveIntOr(object,
                      QStringLiteral("decode_autotune_max_prefetch_max_per_tick"),
                      fallback.decodeAutotuneMaxPrefetchMaxPerTick);
    tuning.decodeAutotuneMaxVisibleQueueReserve =
        qMax(0,
             object.value(QStringLiteral("decode_autotune_max_visible_queue_reserve"))
                 .toInt(fallback.decodeAutotuneMaxVisibleQueueReserve));
    tuning.decodeAutotuneMaxPlaybackWindowAhead =
        positiveIntOr(object,
                      QStringLiteral("decode_autotune_max_playback_window_ahead"),
                      fallback.decodeAutotuneMaxPlaybackWindowAhead);
    tuning.decodeAutotunePrefetchQueueDepthStep =
        positiveIntOr(object,
                      QStringLiteral("decode_autotune_prefetch_queue_depth_step"),
                      fallback.decodeAutotunePrefetchQueueDepthStep);
    tuning.decodeAutotunePrefetchConcurrencyStep =
        positiveIntOr(object,
                      QStringLiteral("decode_autotune_prefetch_concurrency_step"),
                      fallback.decodeAutotunePrefetchConcurrencyStep);
    tuning.decodeAutotuneVisibleQueueReserveStep =
        positiveIntOr(object,
                      QStringLiteral("decode_autotune_visible_queue_reserve_step"),
                      fallback.decodeAutotuneVisibleQueueReserveStep);
    tuning.decodeAutotunePlaybackWindowAheadStep =
        positiveIntOr(object,
                      QStringLiteral("decode_autotune_playback_window_ahead_step"),
                      fallback.decodeAutotunePlaybackWindowAheadStep);
    return tuning;
}

PreviewSurface::PlaybackTuning minimumPreviewTuningForBackend(const QString& backendFamily)
{
    PreviewSurface::PlaybackTuning tuning;
    if (backendFamily == QStringLiteral("vulkan")) {
        tuning.visibleBacklogLimit = 2;
        tuning.sourceLookaheadFrames = 4;
        tuning.proxyLookaheadFrames = 10;
        return tuning;
    }

    tuning.visibleBacklogLimit = 4;
    tuning.sourceLookaheadFrames = 5;
    tuning.proxyLookaheadFrames = 8;
    return tuning;
}

PreviewSurface::PlaybackTuning normalizedPreviewTuningForBackend(
    const QString& backendFamily,
    const PreviewSurface::PlaybackTuning& tuning)
{
    PreviewSurface::PlaybackTuning normalized = tuning;
    const PreviewSurface::PlaybackTuning minimum = minimumPreviewTuningForBackend(backendFamily);
    normalized.visibleBacklogLimit =
        qMax(minimum.visibleBacklogLimit, normalized.visibleBacklogLimit);
    normalized.sourceLookaheadFrames =
        qMax(minimum.sourceLookaheadFrames, normalized.sourceLookaheadFrames);
    normalized.proxyLookaheadFrames =
        qMax(minimum.proxyLookaheadFrames, normalized.proxyLookaheadFrames);
    return normalized;
}

EditorWindow::OptimizedPreviewProfile profileFromJson(
    const QJsonObject& object,
    const EditorWindow::OptimizedPreviewProfile& fallback)
{
    EditorWindow::OptimizedPreviewProfile profile = fallback;
    profile.playbackStartLookaheadFrames =
        positiveIntOr(object,
                      QStringLiteral("playback_start_lookahead_frames"),
                      fallback.playbackStartLookaheadFrames);
    profile.playbackStartLookaheadTimeoutMs =
        positiveIntOr(object,
                      QStringLiteral("playback_start_lookahead_timeout_ms"),
                      fallback.playbackStartLookaheadTimeoutMs);
    profile.decoderWorkerCount =
        qBound(0,
               object.value(QStringLiteral("decoder_worker_count"))
                   .toInt(fallback.decoderWorkerCount),
               16);
    profile.previewTuning =
        tuningFromJson(object.value(QStringLiteral("preview_tuning")).toObject(), fallback.previewTuning);
    return profile;
}

QJsonObject profileToJson(const EditorWindow::OptimizedPreviewProfile& profile)
{
    return QJsonObject{
        {QStringLiteral("playback_start_lookahead_frames"), profile.playbackStartLookaheadFrames},
        {QStringLiteral("playback_start_lookahead_timeout_ms"), profile.playbackStartLookaheadTimeoutMs},
        {QStringLiteral("decoder_worker_count"), profile.decoderWorkerCount},
        {QStringLiteral("preview_tuning"), tuningToJson(profile.previewTuning)}
    };
}

QVector<OptimizationCandidate> candidatesForBackend(const QString& backendFamily, int coreCount)
{
    QVector<OptimizationCandidate> candidates;
    if (backendFamily == QStringLiteral("vulkan")) {
        candidates.push_back({QStringLiteral("conservative"),
                              {3, 900, qMin(coreCount, 8), {2, 4, 10}}});
        candidates.push_back({QStringLiteral("balanced"),
                              {4, 1100, qMin(coreCount, 12), {3, 6, 12}}});
        candidates.push_back({QStringLiteral("throughput"),
                              {6, 1300, qMin(coreCount, 16), {4, 8, 14}}});
        if (coreCount >= 8) {
            candidates.push_back({QStringLiteral("aggressive"),
                                  {8, 1600, qMin(coreCount, 16), {5, 10, 16}}});
        }
    } else {
        candidates.push_back({QStringLiteral("balanced"),
                              {5, 1100, qMin(coreCount, 8), {4, 5, 8}}});
        candidates.push_back({QStringLiteral("throughput"),
                              {6, 1300, qMin(coreCount, 12), {4, 6, 10}}});
        if (coreCount >= 8) {
            candidates.push_back({QStringLiteral("aggressive"),
                                  {8, 1600, qMin(coreCount, 16), {5, 8, 12}}});
        }
    }
    return candidates;
}

} // namespace

QString EditorWindow::optimizedProfilePath() const
{
    const QString projectId = m_projectManager
        ? m_projectManager->currentProjectIdOrDefault()
        : QStringLiteral("default");
    const QString projectPath = m_projectManager ? m_projectManager->projectPath(projectId) : QString();
    return QDir(projectPath).filePath(QStringLiteral("optimized_profile.json"));
}

QJsonObject EditorWindow::optimizedProfileSnapshot() const
{
    const QFileInfo info(optimizedProfilePath());
    return QJsonObject{
        {QStringLiteral("deprecated_alias"), true},
        {QStringLiteral("not_runtime_patch"), true},
        {QStringLiteral("replacement"), QStringLiteral("startup_optimization")},
        {QStringLiteral("loaded"), m_optimizedProfileLoaded},
        {QStringLiteral("generated_this_run"), m_optimizedProfileGeneratedThisRun},
        {QStringLiteral("path"), info.absoluteFilePath()},
        {QStringLiteral("exists"), info.exists()},
        {QStringLiteral("backend"), m_preview ? backendFamilyForName(m_preview->backendName()) : QString()},
        {QStringLiteral("profile"), m_optimizedProfile}
    };
}

QJsonObject EditorWindow::startupOptimizationSnapshot() const
{
    const QFileInfo info(optimizedProfilePath());
    return QJsonObject{
        {QStringLiteral("loaded"), m_optimizedProfileLoaded},
        {QStringLiteral("generated_this_run"), m_optimizedProfileGeneratedThisRun},
        {QStringLiteral("path"), info.absoluteFilePath()},
        {QStringLiteral("exists"), info.exists()},
        {QStringLiteral("backend"), m_preview ? backendFamilyForName(m_preview->backendName()) : QString()},
        {QStringLiteral("source"), m_optimizedProfileLoaded
             ? (m_optimizedProfileGeneratedThisRun ? QStringLiteral("generated") : QStringLiteral("disk"))
             : QStringLiteral("none")},
        {QStringLiteral("not_runtime_patch"), true},
        {QStringLiteral("profile"), m_optimizedProfile}
    };
}

EditorWindow::OptimizedPreviewProfile EditorWindow::defaultOptimizedPreviewProfile() const
{
    OptimizedPreviewProfile profile;
    profile.playbackStartLookaheadFrames = 5;
    profile.playbackStartLookaheadTimeoutMs = 1200;
    profile.decoderWorkerCount = qBound(1, QThread::idealThreadCount(), 16);
    const QString backendFamily =
        m_preview ? backendFamilyForName(m_preview->backendName()) : QStringLiteral("unknown");
    if (m_preview) {
        profile.previewTuning = m_preview->playbackTuning();
    } else {
        profile.previewTuning = minimumPreviewTuningForBackend(backendFamily);
    }
    profile.previewTuning = normalizedPreviewTuningForBackend(backendFamily, profile.previewTuning);
    return profile;
}

void EditorWindow::applyOptimizedProfile(const OptimizedPreviewProfile& profile)
{
    OptimizedPreviewProfile normalized = profile;
    normalized.playbackStartLookaheadFrames = qMax(1, normalized.playbackStartLookaheadFrames);
    normalized.playbackStartLookaheadTimeoutMs = qMax(100, normalized.playbackStartLookaheadTimeoutMs);
    normalized.decoderWorkerCount = qBound(0, normalized.decoderWorkerCount, 16);
    const QString backendFamily =
        m_preview ? backendFamilyForName(m_preview->backendName()) : QStringLiteral("unknown");
    normalized.previewTuning =
        normalizedPreviewTuningForBackend(backendFamily, normalized.previewTuning);
    m_playbackStartLookaheadFrames = normalized.playbackStartLookaheadFrames;
    m_playbackStartLookaheadTimeoutMs = normalized.playbackStartLookaheadTimeoutMs;
    editor::setDebugDecoderLaneCount(normalized.decoderWorkerCount);
    if (m_preview) {
        m_preview->setPlaybackTuning(normalized.previewTuning);
    }
}

bool EditorWindow::loadOptimizedProfileFromDisk(QJsonObject* loadedProfile)
{
    QFile file(optimizedProfilePath());
    if (!file.exists() || !file.open(QIODevice::ReadOnly)) {
        return false;
    }

    const QJsonDocument document = QJsonDocument::fromJson(file.readAll());
    if (!document.isObject()) {
        return false;
    }

    const QJsonObject root = document.object();
    const QString storedBackend =
        backendFamilyForName(root.value(QStringLiteral("backend")).toString());
    const QString currentBackend =
        m_preview ? backendFamilyForName(m_preview->backendName()) : QStringLiteral("unknown");
    if (!storedBackend.isEmpty() &&
        storedBackend != QStringLiteral("unknown") &&
        currentBackend != QStringLiteral("unknown") &&
        storedBackend != currentBackend) {
        return false;
    }

    QJsonObject selectedProfileObject = root.value(QStringLiteral("selected_profile")).toObject();
    if (!selectedProfileObject.contains(QStringLiteral("decoder_worker_count"))) {
        const QString selectedName = root.value(QStringLiteral("selected_name")).toString();
        const QJsonArray results = root.value(QStringLiteral("results")).toArray();
        for (const QJsonValue& value : results) {
            const QJsonObject result = value.toObject();
            if (result.value(QStringLiteral("name")).toString() != selectedName) {
                continue;
            }
            const int workerCount = qBound(
                0,
                result.value(QStringLiteral("decoder_worker_count")).toInt(0),
                16);
            if (workerCount > 0) {
                selectedProfileObject[QStringLiteral("decoder_worker_count")] = workerCount;
            }
            break;
        }
    }
    const OptimizedPreviewProfile profile =
        profileFromJson(selectedProfileObject, defaultOptimizedPreviewProfile());
    applyOptimizedProfile(profile);
    m_optimizedProfile = root;
    m_optimizedProfileLoaded = true;
    m_optimizedProfileGeneratedThisRun = false;
    if (loadedProfile) {
        *loadedProfile = root;
    }
    return true;
}

QJsonObject EditorWindow::runStartupOptimizationPass()
{
    const QString backendFamily =
        m_preview ? backendFamilyForName(m_preview->backendName()) : QStringLiteral("unknown");
    const int coreCount = qMax(1, QThread::idealThreadCount());
    QVector<OptimizationCandidate> candidates = candidatesForBackend(backendFamily, coreCount);
    if (candidates.isEmpty()) {
        candidates.push_back({QStringLiteral("default"), defaultOptimizedPreviewProfile()});
    }

    QJsonArray results;
    double bestScore = std::numeric_limits<double>::lowest();
    int bestIndex = 0;

    if (!m_preview) {
        const OptimizedPreviewProfile fallback = defaultOptimizedPreviewProfile();
        const QJsonObject root{
            {QStringLiteral("version"), 1},
            {QStringLiteral("generated_at_utc"), QDateTime::currentDateTimeUtc().toString(Qt::ISODate)},
            {QStringLiteral("project_id"), m_projectManager ? m_projectManager->currentProjectIdOrDefault() : QStringLiteral("default")},
            {QStringLiteral("backend"), backendFamily},
            {QStringLiteral("reason"), QStringLiteral("preview_unavailable")},
            {QStringLiteral("selected_name"), QStringLiteral("default")},
            {QStringLiteral("selected_profile"), profileToJson(fallback)},
            {QStringLiteral("results"), results}
        };
        applyOptimizedProfile(fallback);
        return root;
    }

    for (int i = 0; i < candidates.size(); ++i) {
        const OptimizationCandidate& candidate = candidates.at(i);
        fprintf(stderr, "[opt-pass] candidate %d/%d '%s' begin\n",
                i + 1, int(candidates.size()), qPrintable(candidate.name));
        applyOptimizedProfile(candidate.profile);
        m_preview->resetProfilingStats();
        QCoreApplication::processEvents(QEventLoop::AllEvents, 10);

        QElapsedTimer timer;
        timer.start();
        const bool warmed = m_preview->warmPlaybackLookahead(
            candidate.profile.playbackStartLookaheadFrames,
            candidate.profile.playbackStartLookaheadTimeoutMs);
        const qint64 elapsedMs = timer.elapsed();
        fprintf(stderr, "[opt-pass] candidate '%s' warmed=%d elapsed_ms=%lld\n",
                qPrintable(candidate.name), warmed ? 1 : 0,
                static_cast<long long>(elapsedMs));
        QCoreApplication::processEvents(QEventLoop::AllEvents, 10);

        const QJsonObject snapshot = m_preview->profilingSnapshot();
        fprintf(stderr, "[opt-pass] candidate '%s' snapshot done\n",
                qPrintable(candidate.name));
        const QJsonObject cache = snapshot.value(QStringLiteral("cache")).toObject();
        const QJsonObject decoder = snapshot.value(QStringLiteral("decoder")).toObject();
        const int totalCachedFrames = cache.value(QStringLiteral("total_cached_frames")).toInt(0);
        const int hardwareFrames = cache.value(QStringLiteral("hardware_frames")).toInt(0);
        const int pendingVisible = cache.value(QStringLiteral("pending_visible_requests")).toInt(0);
        const int decoderPending = decoder.value(QStringLiteral("pending_requests")).toInt(0);
        const int workerCount = decoder.value(QStringLiteral("worker_count")).toInt(coreCount);

        double score = warmed ? 5000.0 : 0.0;
        score += static_cast<double>(qMin(totalCachedFrames,
                                          candidate.profile.playbackStartLookaheadFrames + 1)) * 180.0;
        score += static_cast<double>(qMin(hardwareFrames,
                                          candidate.profile.playbackStartLookaheadFrames + 1)) * 60.0;
        score -= static_cast<double>(elapsedMs) * 4.0;
        score -= static_cast<double>(pendingVisible) * 250.0;
        score -= static_cast<double>(qMax(0, decoderPending - workerCount)) * 80.0;
        score -= static_cast<double>(candidate.profile.previewTuning.visibleBacklogLimit) * 18.0;
        score -= static_cast<double>(candidate.profile.previewTuning.sourceLookaheadFrames) * 7.0;

        results.push_back(QJsonObject{
            {QStringLiteral("name"), candidate.name},
            {QStringLiteral("score"), score},
            {QStringLiteral("warm_success"), warmed},
            {QStringLiteral("warm_elapsed_ms"), elapsedMs},
            {QStringLiteral("total_cached_frames"), totalCachedFrames},
            {QStringLiteral("hardware_frames"), hardwareFrames},
            {QStringLiteral("pending_visible_requests"), pendingVisible},
            {QStringLiteral("decoder_pending_requests"), decoderPending},
            {QStringLiteral("decoder_worker_count"), workerCount},
            {QStringLiteral("profile"), profileToJson(candidate.profile)}
        });

        if (score > bestScore) {
            bestScore = score;
            bestIndex = i;
        }
    }

    const OptimizationCandidate& selected = candidates.at(bestIndex);
    applyOptimizedProfile(selected.profile);

    const QJsonObject root{
        {QStringLiteral("version"), 1},
        {QStringLiteral("generated_at_utc"), QDateTime::currentDateTimeUtc().toString(Qt::ISODate)},
        {QStringLiteral("project_id"), m_projectManager ? m_projectManager->currentProjectIdOrDefault() : QStringLiteral("default")},
        {QStringLiteral("backend"), backendFamily},
        {QStringLiteral("system"), QJsonObject{
             {QStringLiteral("ideal_thread_count"), coreCount},
             {QStringLiteral("cpu_arch"), QSysInfo::currentCpuArchitecture()},
             {QStringLiteral("kernel_type"), QSysInfo::kernelType()},
             {QStringLiteral("kernel_version"), QSysInfo::kernelVersion()},
             {QStringLiteral("product_type"), QSysInfo::productType()},
             {QStringLiteral("product_version"), QSysInfo::productVersion()}
         }},
        {QStringLiteral("selected_name"), selected.name},
        {QStringLiteral("selected_score"), bestScore},
        {QStringLiteral("selected_profile"), profileToJson(selected.profile)},
        {QStringLiteral("results"), results}
    };
    return root;
}

QJsonObject EditorWindow::ensureOptimizedProfile()
{
    // The optimization pass below spins a nested event loop (processEvents
    // inside warmPlaybackLookahead). Any ensure scheduled while the pass is
    // running would be delivered INTO that nested loop and recurse into a
    // second pass, which re-arms again before the first ever finishes — a
    // first-run livelock (no cached profile on disk yet). Refuse re-entry.
    if (m_optimizedProfileEnsureRunning) {
        return QJsonObject{
            {QStringLiteral("ok"), false},
            {QStringLiteral("busy"), true},
            {QStringLiteral("reason"), QStringLiteral("optimization_pass_already_running")}
        };
    }
    m_optimizedProfileEnsureRunning = true;
    const auto ensureRunningReset = qScopeGuard([this]() {
        m_optimizedProfileEnsureRunning = false;
    });

    m_optimizedProfileEnsureScheduled = false;
    m_optimizedProfileLoaded = false;
    m_optimizedProfileGeneratedThisRun = false;

    QJsonObject loaded;
    if (loadOptimizedProfileFromDisk(&loaded)) {
        return QJsonObject{
            {QStringLiteral("ok"), true},
            {QStringLiteral("generated"), false},
            {QStringLiteral("path"), optimizedProfilePath()}
        };
    }

    const QJsonObject generated = runStartupOptimizationPass();
    const QString projectId = m_projectManager
        ? m_projectManager->currentProjectIdOrDefault()
        : QStringLiteral("default");
    if (m_projectManager) {
        QDir().mkpath(m_projectManager->projectPath(projectId));
    }
    QSaveFile file(optimizedProfilePath());
    bool saved = false;
    QString error;
    if (!file.open(QIODevice::WriteOnly)) {
        error = file.errorString();
    } else {
        file.write(QJsonDocument(generated).toJson(QJsonDocument::Indented));
        saved = file.commit();
        if (!saved) {
            error = file.errorString();
        }
    }

    m_optimizedProfile = generated;
    m_optimizedProfileLoaded = saved;
    m_optimizedProfileGeneratedThisRun = true;

    return QJsonObject{
        {QStringLiteral("ok"), saved},
        {QStringLiteral("generated"), true},
        {QStringLiteral("path"), optimizedProfilePath()},
        {QStringLiteral("error"), error}
    };
}

void EditorWindow::scheduleOptimizedProfileEnsure()
{
    if (m_optimizedProfileEnsureScheduled) {
        return;
    }
    if (QGuiApplication::platformName().compare(QStringLiteral("offscreen"), Qt::CaseInsensitive) == 0) {
        startupProfileMark(QStringLiteral("deferred_optimization.skipped"),
                           QJsonObject{{QStringLiteral("reason"), QStringLiteral("offscreen_platform")}});
        return;
    }
    m_optimizedProfileEnsureScheduled = true;
    QTimer::singleShot(0, this, [this]() {
        startupProfileMark(QStringLiteral("deferred_optimization.begin"));
        const QJsonObject optimizationResult = ensureOptimizedProfile();
        startupProfileMark(QStringLiteral("deferred_optimization.end"), optimizationResult);
    });
}
