#include "debug_controls.h"

#include <QByteArray>
#include <QFile>
#include <QtGlobal>

#include <atomic>
#include <mutex>
#include <utility>

namespace editor {
namespace {

constexpr DebugLogLevel kDefaultPlaybackLevel = DebugLogLevel::Off;
constexpr DebugLogLevel kDefaultCacheLevel = DebugLogLevel::Off;
constexpr DebugLogLevel kDefaultDecodeLevel = DebugLogLevel::Off;
constexpr bool kDefaultLeadPrefetchEnabled = true;
constexpr int kDefaultLeadPrefetchCount = 8;
constexpr int kDefaultPrefetchMaxQueueDepth = 24;
constexpr int kDefaultPrefetchMaxInflight = 8;
constexpr int kDefaultPrefetchMaxPerTick = 8;
constexpr int kDefaultPrefetchSkipVisiblePendingThreshold = 2;
constexpr int kDefaultVisibleQueueReserve = 24;
constexpr int kDefaultPlaybackWindowAhead = 16;
constexpr int kDefaultDecoderLaneCount = 0; // 0 => auto lane count
constexpr DecodePreference kDefaultDecodePreference = DecodePreference::Hardware;

bool envFlagEnabled(const char* name) {
    return qEnvironmentVariableIntValue(name) == 1;
}

DebugLogLevel envLevel(const char* levelName, const char* legacyFlagName, DebugLogLevel defaultLevel) {
    DebugLogLevel parsed = DebugLogLevel::Off;
    if (parseDebugLogLevel(qEnvironmentVariable(levelName), &parsed)) {
        return parsed;
    }
    if (envFlagEnabled(legacyFlagName)) {
        return DebugLogLevel::Debug;
    }
    return defaultLevel;
}

std::atomic<int> g_debugPlayback{static_cast<int>(envLevel("EDITOR_DEBUG_PLAYBACK_LEVEL", "EDITOR_DEBUG_PLAYBACK", kDefaultPlaybackLevel))};
std::atomic<int> g_debugCache{static_cast<int>(envLevel("EDITOR_DEBUG_CACHE_LEVEL", "EDITOR_DEBUG_CACHE", kDefaultCacheLevel))};
std::atomic<int> g_debugDecode{static_cast<int>(envLevel("EDITOR_DEBUG_DECODE_LEVEL", "EDITOR_DEBUG_DECODE", kDefaultDecodeLevel))};
std::atomic<bool> g_debugLeadPrefetchEnabled{kDefaultLeadPrefetchEnabled};
std::atomic<int> g_debugLeadPrefetchCount{kDefaultLeadPrefetchCount};
std::atomic<int> g_debugPrefetchMaxQueueDepth{kDefaultPrefetchMaxQueueDepth};
std::atomic<int> g_debugPrefetchMaxInflight{kDefaultPrefetchMaxInflight};
std::atomic<int> g_debugPrefetchMaxPerTick{kDefaultPrefetchMaxPerTick};
std::atomic<int> g_debugPrefetchSkipVisiblePendingThreshold{kDefaultPrefetchSkipVisiblePendingThreshold};
std::atomic<int> g_debugVisibleQueueReserve{kDefaultVisibleQueueReserve};
std::atomic<int> g_debugPlaybackWindowAhead{kDefaultPlaybackWindowAhead};
std::atomic<int> g_debugDecoderLaneCount{kDefaultDecoderLaneCount};
std::atomic<int> g_decodePreference{static_cast<int>(kDefaultDecodePreference)};
std::atomic<bool> g_debugPlayheadNoRepaint{false};
std::atomic<bool> g_debugPlaybackCacheFallbackEnabled{true};
std::atomic<bool> g_debugDeterministicPipelineEnabled{false};
std::mutex g_decoderLaneCountCallbackMutex;
DecoderLaneCountChangedCallback g_decoderLaneCountChangedCallback;

}

QString debugLogLevelToString(DebugLogLevel level) {
    switch (level) {
    case DebugLogLevel::Off: return QStringLiteral("off");
    case DebugLogLevel::Warn: return QStringLiteral("warn");
    case DebugLogLevel::Info: return QStringLiteral("info");
    case DebugLogLevel::Debug: return QStringLiteral("debug");
    case DebugLogLevel::Verbose: return QStringLiteral("verbose");
    }
    return QStringLiteral("off");
}

bool parseDebugLogLevel(const QString& text, DebugLogLevel* levelOut) {
    if (!levelOut) {
        return false;
    }
    const QString normalized = text.trimmed().toLower();
    if (normalized.isEmpty()) {
        return false;
    }
    if (normalized == QStringLiteral("off") || normalized == QStringLiteral("false") || normalized == QStringLiteral("0")) {
        *levelOut = DebugLogLevel::Off;
        return true;
    }
    if (normalized == QStringLiteral("warn") ||
        normalized == QStringLiteral("warning") ||
        normalized == QStringLiteral("anomaly") ||
        normalized == QStringLiteral("anomalies")) {
        *levelOut = DebugLogLevel::Warn;
        return true;
    }
    if (normalized == QStringLiteral("info")) {
        *levelOut = DebugLogLevel::Info;
        return true;
    }
    if (normalized == QStringLiteral("debug") || normalized == QStringLiteral("true") || normalized == QStringLiteral("1")) {
        *levelOut = DebugLogLevel::Debug;
        return true;
    }
    if (normalized == QStringLiteral("verbose")) {
        *levelOut = DebugLogLevel::Verbose;
        return true;
    }
    return false;
}

QString decodePreferenceToString(DecodePreference preference) {
    switch (preference) {
    case DecodePreference::Auto: return QStringLiteral("auto");
    case DecodePreference::HardwareZeroCopy: return QStringLiteral("hardware_zero_copy");
    case DecodePreference::Hardware: return QStringLiteral("hardware");
    case DecodePreference::Software: return QStringLiteral("software");
    }
    return QStringLiteral("auto");
}

bool parseDecodePreference(const QString& text, DecodePreference* preferenceOut) {
    if (!preferenceOut) {
        return false;
    }
    const QString normalized = text.trimmed().toLower();
    if (normalized == QStringLiteral("auto")) {
        *preferenceOut = DecodePreference::Auto;
        return true;
    }
    if (normalized == QStringLiteral("hardware_zero_copy") ||
        normalized == QStringLiteral("zero_copy") ||
        normalized == QStringLiteral("zerocopy") ||
        normalized == QStringLiteral("cuda_gl")) {
        *preferenceOut = DecodePreference::HardwareZeroCopy;
        return true;
    }
    if (normalized == QStringLiteral("hardware") ||
        normalized == QStringLiteral("gpu") ||
        normalized == QStringLiteral("prefer_hardware")) {
        *preferenceOut = DecodePreference::Hardware;
        return true;
    }
    if (normalized == QStringLiteral("software") ||
        normalized == QStringLiteral("cpu") ||
        normalized == QStringLiteral("software_only")) {
        *preferenceOut = DecodePreference::Software;
        return true;
    }
    return false;
}

DebugLogLevel debugPlaybackLevel() {
    return static_cast<DebugLogLevel>(g_debugPlayback.load());
}

DebugLogLevel debugCacheLevel() {
    return static_cast<DebugLogLevel>(g_debugCache.load());
}

DebugLogLevel debugDecodeLevel() {
    return static_cast<DebugLogLevel>(g_debugDecode.load());
}

bool debugPlaybackEnabled() {
    return debugPlaybackLevel() >= DebugLogLevel::Debug;
}

bool debugCacheEnabled() {
    return debugCacheLevel() >= DebugLogLevel::Debug;
}

bool debugDecodeEnabled() {
    return debugDecodeLevel() >= DebugLogLevel::Debug;
}

bool debugPlaybackWarnEnabled() {
    return debugPlaybackLevel() >= DebugLogLevel::Warn;
}

bool debugCacheWarnEnabled() {
    return debugCacheLevel() >= DebugLogLevel::Warn;
}

bool debugDecodeWarnEnabled() {
    return debugDecodeLevel() >= DebugLogLevel::Warn;
}

bool debugPlaybackWarnOnlyEnabled() {
    return debugPlaybackLevel() == DebugLogLevel::Warn;
}

bool debugCacheWarnOnlyEnabled() {
    return debugCacheLevel() == DebugLogLevel::Warn;
}

bool debugDecodeWarnOnlyEnabled() {
    return debugDecodeLevel() == DebugLogLevel::Warn;
}

bool debugPlaybackVerboseEnabled() {
    return debugPlaybackLevel() >= DebugLogLevel::Verbose;
}

bool debugCacheVerboseEnabled() {
    return debugCacheLevel() >= DebugLogLevel::Verbose;
}

bool debugDecodeVerboseEnabled() {
    return debugDecodeLevel() >= DebugLogLevel::Verbose;
}

bool debugLeadPrefetchEnabled() {
    return g_debugLeadPrefetchEnabled.load();
}

int debugLeadPrefetchCount() {
    return g_debugLeadPrefetchCount.load();
}

int debugPrefetchMaxQueueDepth() {
    return g_debugPrefetchMaxQueueDepth.load();
}

int debugPrefetchMaxInflight() {
    return g_debugPrefetchMaxInflight.load();
}

int debugPrefetchMaxPerTick() {
    return g_debugPrefetchMaxPerTick.load();
}

int debugPrefetchSkipVisiblePendingThreshold() {
    return g_debugPrefetchSkipVisiblePendingThreshold.load();
}

int debugVisibleQueueReserve() {
    return g_debugVisibleQueueReserve.load();
}

int debugPlaybackWindowAhead() {
    return g_debugPlaybackWindowAhead.load();
}

int debugDecoderLaneCount() {
    return g_debugDecoderLaneCount.load();
}

DecodePreference debugDecodePreference() {
    return static_cast<DecodePreference>(g_decodePreference.load());
}

bool debugPlayheadNoRepaint() {
    return g_debugPlayheadNoRepaint.load();
}

bool debugPlaybackCacheFallbackEnabled() {
    return g_debugPlaybackCacheFallbackEnabled.load();
}

bool debugDeterministicPipelineEnabled() {
    return g_debugDeterministicPipelineEnabled.load();
}

void setDebugPlaybackEnabled(bool enabled) {
    g_debugPlayback.store(static_cast<int>(enabled ? DebugLogLevel::Debug : DebugLogLevel::Off));
}

void setDebugCacheEnabled(bool enabled) {
    g_debugCache.store(static_cast<int>(enabled ? DebugLogLevel::Debug : DebugLogLevel::Off));
}

void setDebugDecodeEnabled(bool enabled) {
    g_debugDecode.store(static_cast<int>(enabled ? DebugLogLevel::Debug : DebugLogLevel::Off));
}

void setDebugPlaybackLevel(DebugLogLevel level) {
    g_debugPlayback.store(static_cast<int>(level));
}

void setDebugCacheLevel(DebugLogLevel level) {
    g_debugCache.store(static_cast<int>(level));
}

void setDebugDecodeLevel(DebugLogLevel level) {
    g_debugDecode.store(static_cast<int>(level));
}

void setDebugLeadPrefetchEnabled(bool enabled) {
    g_debugLeadPrefetchEnabled.store(enabled);
}

void setDebugLeadPrefetchCount(int count) {
    g_debugLeadPrefetchCount.store(qBound(0, count, 8));
}

void setDebugPrefetchMaxQueueDepth(int depth) {
    g_debugPrefetchMaxQueueDepth.store(qBound(1, depth, 32));
}

void setDebugPrefetchMaxInflight(int inflight) {
    g_debugPrefetchMaxInflight.store(qBound(1, inflight, 16));
}

void setDebugPrefetchMaxPerTick(int perTick) {
    g_debugPrefetchMaxPerTick.store(qBound(1, perTick, 16));
}

void setDebugPrefetchSkipVisiblePendingThreshold(int threshold) {
    g_debugPrefetchSkipVisiblePendingThreshold.store(qBound(0, threshold, 16));
}

void setDebugVisibleQueueReserve(int reserve) {
    g_debugVisibleQueueReserve.store(qBound(0, reserve, 64));
}

void setDebugPlaybackWindowAhead(int ahead) {
    g_debugPlaybackWindowAhead.store(qBound(1, ahead, 24));
}

void setDebugDecoderLaneCount(int count) {
    const int clamped = qBound(0, count, 16);
    const int previous = g_debugDecoderLaneCount.exchange(clamped);
    if (previous == clamped) {
        return;
    }

    DecoderLaneCountChangedCallback callback;
    {
        std::lock_guard<std::mutex> lock(g_decoderLaneCountCallbackMutex);
        callback = g_decoderLaneCountChangedCallback;
    }
    if (callback) {
        callback(clamped);
    }
}

void setDebugDecodePreference(DecodePreference preference) {
    g_decodePreference.store(static_cast<int>(preference));
}

void setDebugPlayheadNoRepaint(bool enabled) {
    g_debugPlayheadNoRepaint.store(enabled);
}

void setDebugPlaybackCacheFallbackEnabled(bool enabled) {
    g_debugPlaybackCacheFallbackEnabled.store(enabled);
}

void setDebugDeterministicPipelineEnabled(bool enabled) {
    g_debugDeterministicPipelineEnabled.store(enabled);
}

RenderPipelineDefaults defaultRenderPipelineDefaultsForCurrentSystem() {
    RenderPipelineDefaults defaults;

    defaults.decodePreference = DecodePreference::Software;
#if defined(Q_OS_LINUX)
    const bool hasVaapiRenderNode =
        QFile::exists(QStringLiteral("/dev/dri/renderD128")) ||
        QFile::exists(QStringLiteral("/dev/dri/renderD129")) ||
        QFile::exists(QStringLiteral("/dev/dri/renderD130"));
    const bool hasNvidiaDriver = QFile::exists(QStringLiteral("/proc/driver/nvidia/version"));
    const bool headlessOffscreen =
        qEnvironmentVariable("QT_QPA_PLATFORM") == QStringLiteral("offscreen");
    if (!headlessOffscreen) {
        if (hasVaapiRenderNode) {
            defaults.decodePreference = DecodePreference::HardwareZeroCopy;
        } else if (hasNvidiaDriver) {
            defaults.decodePreference = DecodePreference::Hardware;
        }
    }
#elif defined(Q_OS_MACOS) || defined(Q_OS_WIN)
    defaults.decodePreference = DecodePreference::Hardware;
#endif

    defaults.deterministicPipeline = false;
    defaults.playbackCacheFallback = true;
    defaults.leadPrefetchEnabled = true;
    defaults.leadPrefetchCount = kDefaultLeadPrefetchCount;
    defaults.playbackWindowAhead = kDefaultPlaybackWindowAhead;
    defaults.visibleQueueReserve = kDefaultVisibleQueueReserve;
    defaults.prefetchMaxQueueDepth = kDefaultPrefetchMaxQueueDepth;
    defaults.prefetchMaxInflight = kDefaultPrefetchMaxInflight;
    defaults.prefetchMaxPerTick = kDefaultPrefetchMaxPerTick;
    defaults.prefetchSkipVisiblePendingThreshold = kDefaultPrefetchSkipVisiblePendingThreshold;
    defaults.decoderLaneCount = kDefaultDecoderLaneCount;
    return defaults;
}

QJsonObject debugControlsSnapshot() {
    return QJsonObject{
        {QStringLiteral("playback"), debugPlaybackEnabled()},
        {QStringLiteral("cache"), debugCacheEnabled()},
        {QStringLiteral("decode"), debugDecodeEnabled()},
        {QStringLiteral("playback_level"), debugLogLevelToString(debugPlaybackLevel())},
        {QStringLiteral("cache_level"), debugLogLevelToString(debugCacheLevel())},
        {QStringLiteral("decode_level"), debugLogLevelToString(debugDecodeLevel())},
        {QStringLiteral("lead_prefetch_enabled"), debugLeadPrefetchEnabled()},
        {QStringLiteral("lead_prefetch_count"), debugLeadPrefetchCount()},
        {QStringLiteral("prefetch_max_queue_depth"), debugPrefetchMaxQueueDepth()},
        {QStringLiteral("prefetch_max_inflight"), debugPrefetchMaxInflight()},
        {QStringLiteral("prefetch_max_per_tick"), debugPrefetchMaxPerTick()},
        {QStringLiteral("prefetch_skip_visible_pending_threshold"), debugPrefetchSkipVisiblePendingThreshold()},
        {QStringLiteral("visible_queue_reserve"), debugVisibleQueueReserve()},
        {QStringLiteral("playback_window_ahead"), debugPlaybackWindowAhead()},
        {QStringLiteral("decoder_lane_count"), debugDecoderLaneCount()},
        {QStringLiteral("decode_mode"), decodePreferenceToString(debugDecodePreference())},
        {QStringLiteral("playhead_no_repaint"), debugPlayheadNoRepaint()},
        {QStringLiteral("playback_cache_fallback"), debugPlaybackCacheFallbackEnabled()},
        {QStringLiteral("deterministic_pipeline"), debugDeterministicPipelineEnabled()}
    };
}

bool setDebugControl(const QString& name, bool enabled) {
    if (name == QStringLiteral("playback")) {
        setDebugPlaybackEnabled(enabled);
        return true;
    }
    if (name == QStringLiteral("cache")) {
        setDebugCacheEnabled(enabled);
        return true;
    }
    if (name == QStringLiteral("decode")) {
        setDebugDecodeEnabled(enabled);
        return true;
    }
    if (name == QStringLiteral("all")) {
        setDebugPlaybackEnabled(enabled);
        setDebugCacheEnabled(enabled);
        setDebugDecodeEnabled(enabled);
        return true;
    }
    return false;
}

bool setDebugControlLevel(const QString& name, DebugLogLevel level) {
    if (name == QStringLiteral("playback")) {
        setDebugPlaybackLevel(level);
        return true;
    }
    if (name == QStringLiteral("cache")) {
        setDebugCacheLevel(level);
        return true;
    }
    if (name == QStringLiteral("decode")) {
        setDebugDecodeLevel(level);
        return true;
    }
    if (name == QStringLiteral("all")) {
        setDebugPlaybackLevel(level);
        setDebugCacheLevel(level);
        setDebugDecodeLevel(level);
        return true;
    }
    return false;
}

bool setDebugOption(const QString& name, const QJsonValue& value) {
    if (name == QStringLiteral("lead_prefetch_enabled") && value.isBool()) {
        setDebugLeadPrefetchEnabled(value.toBool());
        return true;
    }
    if (name == QStringLiteral("lead_prefetch_count") && value.isDouble()) {
        setDebugLeadPrefetchCount(value.toInt());
        return true;
    }
    if (name == QStringLiteral("prefetch_max_queue_depth") && value.isDouble()) {
        setDebugPrefetchMaxQueueDepth(value.toInt());
        return true;
    }
    if (name == QStringLiteral("prefetch_max_inflight") && value.isDouble()) {
        setDebugPrefetchMaxInflight(value.toInt());
        return true;
    }
    if (name == QStringLiteral("prefetch_max_per_tick") && value.isDouble()) {
        setDebugPrefetchMaxPerTick(value.toInt());
        return true;
    }
    if (name == QStringLiteral("prefetch_skip_visible_pending_threshold") && value.isDouble()) {
        setDebugPrefetchSkipVisiblePendingThreshold(value.toInt());
        return true;
    }
    if (name == QStringLiteral("visible_queue_reserve") && value.isDouble()) {
        setDebugVisibleQueueReserve(value.toInt());
        return true;
    }
    if (name == QStringLiteral("playback_window_ahead") && value.isDouble()) {
        setDebugPlaybackWindowAhead(value.toInt());
        return true;
    }
    if (name == QStringLiteral("decoder_lane_count") && value.isDouble()) {
        setDebugDecoderLaneCount(value.toInt());
        return true;
    }
    if (name == QStringLiteral("decode_mode") && value.isString()) {
        DecodePreference preference = DecodePreference::Auto;
        if (!parseDecodePreference(value.toString(), &preference)) {
            return false;
        }
        setDebugDecodePreference(preference);
        return true;
    }
    if (name == QStringLiteral("playhead_no_repaint") && value.isBool()) {
        setDebugPlayheadNoRepaint(value.toBool());
        return true;
    }
    if (name == QStringLiteral("playback_cache_fallback") && value.isBool()) {
        setDebugPlaybackCacheFallbackEnabled(value.toBool());
        return true;
    }
    if (name == QStringLiteral("deterministic_pipeline") && value.isBool()) {
        setDebugDeterministicPipelineEnabled(value.toBool());
        return true;
    }
    return false;
}

void setDecoderLaneCountChangedCallback(DecoderLaneCountChangedCallback callback) {
    std::lock_guard<std::mutex> lock(g_decoderLaneCountCallbackMutex);
    g_decoderLaneCountChangedCallback = std::move(callback);
}

} // namespace editor
