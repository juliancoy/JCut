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
constexpr int kDefaultSupersedeSlackFrames = 12;
constexpr int kDefaultVisibleDecodeKeepWindow = 16;
constexpr int kDefaultObsoleteVisibleFrameSlack = 4;
constexpr int kDefaultCancelBeforeMinFrameAdvance = 2;
constexpr qint64 kDefaultCancelBeforeMinIntervalMs = 16;
constexpr int kDefaultMaxPresentationPastFrameDelta = 4;
constexpr int kDefaultMaxPresentationFutureFrameDelta = 4;
constexpr int kDefaultFileVideoPlaybackWindowAhead = 4;
constexpr int kDefaultVisiblePendingRetryMs = 2000;
constexpr int kDefaultCacheObsoleteVisibleFrameSlack = 4;
constexpr int kDefaultMaxVisibleBacklog = 1;
constexpr int kDefaultSequenceVisibleDecodeKeepWindow = 32;
constexpr int kDefaultSequenceObsoleteVisibleFrameSlack = 8;
constexpr int kDefaultSequenceLateBufferSeedSlack = 16;
constexpr int kDefaultTimelineAudioEnvelopeGranularity = 256;
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
std::atomic<int> g_debugSupersedeSlackFrames{kDefaultSupersedeSlackFrames};
std::atomic<int> g_debugVisibleDecodeKeepWindow{kDefaultVisibleDecodeKeepWindow};
std::atomic<int> g_debugObsoleteVisibleFrameSlack{kDefaultObsoleteVisibleFrameSlack};
std::atomic<int> g_debugCancelBeforeMinFrameAdvance{kDefaultCancelBeforeMinFrameAdvance};
std::atomic<qint64> g_debugCancelBeforeMinIntervalMs{kDefaultCancelBeforeMinIntervalMs};
std::atomic<int> g_debugMaxPresentationPastFrameDelta{kDefaultMaxPresentationPastFrameDelta};
std::atomic<int> g_debugMaxPresentationFutureFrameDelta{kDefaultMaxPresentationFutureFrameDelta};
std::atomic<int> g_debugFileVideoPlaybackWindowAhead{kDefaultFileVideoPlaybackWindowAhead};
std::atomic<int> g_debugVisiblePendingRetryMs{kDefaultVisiblePendingRetryMs};
std::atomic<int> g_debugCacheObsoleteVisibleFrameSlack{kDefaultCacheObsoleteVisibleFrameSlack};
std::atomic<int> g_debugMaxVisibleBacklog{kDefaultMaxVisibleBacklog};
std::atomic<int> g_debugSequenceVisibleDecodeKeepWindow{kDefaultSequenceVisibleDecodeKeepWindow};
std::atomic<int> g_debugSequenceObsoleteVisibleFrameSlack{kDefaultSequenceObsoleteVisibleFrameSlack};
std::atomic<int> g_debugSequenceLateBufferSeedSlack{kDefaultSequenceLateBufferSeedSlack};
std::atomic<int> g_decodePreference{static_cast<int>(kDefaultDecodePreference)};
std::atomic<int> g_h26xSoftwareThreadingMode{static_cast<int>(H26xSoftwareThreadingMode::Auto)};
std::atomic<bool> g_debugPlayheadNoRepaint{false};
std::atomic<bool> g_debugPlaybackCacheFallbackEnabled{true};
std::atomic<bool> g_debugDeterministicPipelineEnabled{false};
std::atomic<bool> g_debugTemporalDebugOverlayEnabled{envFlagEnabled("JCUT_TEMPORAL_DEBUG_OVERLAY")};
std::atomic<int> g_debugTimelineAudioEnvelopeGranularity{kDefaultTimelineAudioEnvelopeGranularity};
std::atomic<int> g_rubberBandEnginePreference{static_cast<int>(RubberBandEnginePreference::Faster)};
std::atomic<int> g_rubberBandThreadingPreference{static_cast<int>(RubberBandThreadingPreference::Always)};
std::atomic<int> g_rubberBandWindowPreference{static_cast<int>(RubberBandWindowPreference::Standard)};
std::atomic<int> g_rubberBandPitchPreference{static_cast<int>(RubberBandPitchPreference::HighSpeed)};
std::atomic<bool> g_rubberBandChannelsTogether{true};
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

QString h26xSoftwareThreadingModeToString(H26xSoftwareThreadingMode mode) {
    switch (mode) {
    case H26xSoftwareThreadingMode::Auto: return QStringLiteral("auto");
    case H26xSoftwareThreadingMode::SingleThread: return QStringLiteral("single_thread");
    case H26xSoftwareThreadingMode::SliceThreads: return QStringLiteral("slice_threads");
    case H26xSoftwareThreadingMode::FrameAndSliceThreads: return QStringLiteral("frame_and_slice_threads");
    }
    return QStringLiteral("auto");
}

bool parseH26xSoftwareThreadingMode(const QString& text, H26xSoftwareThreadingMode* modeOut) {
    if (!modeOut) {
        return false;
    }
    const QString normalized = text.trimmed().toLower();
    if (normalized == QStringLiteral("auto")) {
        *modeOut = H26xSoftwareThreadingMode::Auto;
        return true;
    }
    if (normalized == QStringLiteral("single_thread") ||
        normalized == QStringLiteral("single") ||
        normalized == QStringLiteral("stability")) {
        *modeOut = H26xSoftwareThreadingMode::SingleThread;
        return true;
    }
    if (normalized == QStringLiteral("slice_threads") ||
        normalized == QStringLiteral("slice") ||
        normalized == QStringLiteral("balanced")) {
        *modeOut = H26xSoftwareThreadingMode::SliceThreads;
        return true;
    }
    if (normalized == QStringLiteral("frame_and_slice_threads") ||
        normalized == QStringLiteral("frame_slice") ||
        normalized == QStringLiteral("performance")) {
        *modeOut = H26xSoftwareThreadingMode::FrameAndSliceThreads;
        return true;
    }
    return false;
}

QString rubberBandEnginePreferenceToString(RubberBandEnginePreference preference) {
    switch (preference) {
    case RubberBandEnginePreference::Faster: return QStringLiteral("faster");
    case RubberBandEnginePreference::Finer: return QStringLiteral("finer");
    }
    return QStringLiteral("finer");
}

bool parseRubberBandEnginePreference(const QString& text, RubberBandEnginePreference* preferenceOut) {
    if (!preferenceOut) {
        return false;
    }
    const QString normalized = text.trimmed().toLower();
    if (normalized == QStringLiteral("faster") || normalized == QStringLiteral("r2") ||
        normalized == QStringLiteral("interactive")) {
        *preferenceOut = RubberBandEnginePreference::Faster;
        return true;
    }
    if (normalized == QStringLiteral("finer") || normalized == QStringLiteral("r3") ||
        normalized == QStringLiteral("quality")) {
        *preferenceOut = RubberBandEnginePreference::Finer;
        return true;
    }
    return false;
}

QString rubberBandThreadingPreferenceToString(RubberBandThreadingPreference preference) {
    switch (preference) {
    case RubberBandThreadingPreference::Auto: return QStringLiteral("auto");
    case RubberBandThreadingPreference::Never: return QStringLiteral("never");
    case RubberBandThreadingPreference::Always: return QStringLiteral("always");
    }
    return QStringLiteral("auto");
}

bool parseRubberBandThreadingPreference(const QString& text, RubberBandThreadingPreference* preferenceOut) {
    if (!preferenceOut) {
        return false;
    }
    const QString normalized = text.trimmed().toLower();
    if (normalized == QStringLiteral("auto")) {
        *preferenceOut = RubberBandThreadingPreference::Auto;
        return true;
    }
    if (normalized == QStringLiteral("never") || normalized == QStringLiteral("single_thread")) {
        *preferenceOut = RubberBandThreadingPreference::Never;
        return true;
    }
    if (normalized == QStringLiteral("always") || normalized == QStringLiteral("threaded")) {
        *preferenceOut = RubberBandThreadingPreference::Always;
        return true;
    }
    return false;
}

QString rubberBandWindowPreferenceToString(RubberBandWindowPreference preference) {
    switch (preference) {
    case RubberBandWindowPreference::Standard: return QStringLiteral("standard");
    case RubberBandWindowPreference::Short: return QStringLiteral("short");
    case RubberBandWindowPreference::Long: return QStringLiteral("long");
    }
    return QStringLiteral("standard");
}

bool parseRubberBandWindowPreference(const QString& text, RubberBandWindowPreference* preferenceOut) {
    if (!preferenceOut) {
        return false;
    }
    const QString normalized = text.trimmed().toLower();
    if (normalized == QStringLiteral("standard") || normalized == QStringLiteral("default")) {
        *preferenceOut = RubberBandWindowPreference::Standard;
        return true;
    }
    if (normalized == QStringLiteral("short")) {
        *preferenceOut = RubberBandWindowPreference::Short;
        return true;
    }
    if (normalized == QStringLiteral("long")) {
        *preferenceOut = RubberBandWindowPreference::Long;
        return true;
    }
    return false;
}

QString rubberBandPitchPreferenceToString(RubberBandPitchPreference preference) {
    switch (preference) {
    case RubberBandPitchPreference::HighSpeed: return QStringLiteral("high_speed");
    case RubberBandPitchPreference::HighQuality: return QStringLiteral("high_quality");
    case RubberBandPitchPreference::HighConsistency: return QStringLiteral("high_consistency");
    }
    return QStringLiteral("high_quality");
}

bool parseRubberBandPitchPreference(const QString& text, RubberBandPitchPreference* preferenceOut) {
    if (!preferenceOut) {
        return false;
    }
    const QString normalized = text.trimmed().toLower();
    if (normalized == QStringLiteral("high_speed") || normalized == QStringLiteral("speed")) {
        *preferenceOut = RubberBandPitchPreference::HighSpeed;
        return true;
    }
    if (normalized == QStringLiteral("high_quality") || normalized == QStringLiteral("quality")) {
        *preferenceOut = RubberBandPitchPreference::HighQuality;
        return true;
    }
    if (normalized == QStringLiteral("high_consistency") || normalized == QStringLiteral("consistency")) {
        *preferenceOut = RubberBandPitchPreference::HighConsistency;
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

int debugSupersedeSlackFrames() {
    return g_debugSupersedeSlackFrames.load();
}

int debugVisibleDecodeKeepWindow() {
    return g_debugVisibleDecodeKeepWindow.load();
}

int debugObsoleteVisibleFrameSlack() {
    return g_debugObsoleteVisibleFrameSlack.load();
}

int debugCancelBeforeMinFrameAdvance() {
    return g_debugCancelBeforeMinFrameAdvance.load();
}

qint64 debugCancelBeforeMinIntervalMs() {
    return g_debugCancelBeforeMinIntervalMs.load();
}

int debugMaxPresentationPastFrameDelta() {
    return g_debugMaxPresentationPastFrameDelta.load();
}

int debugMaxPresentationFutureFrameDelta() {
    return g_debugMaxPresentationFutureFrameDelta.load();
}

int debugFileVideoPlaybackWindowAhead() {
    return g_debugFileVideoPlaybackWindowAhead.load();
}

int debugVisiblePendingRetryMs() {
    return g_debugVisiblePendingRetryMs.load();
}

int debugCacheObsoleteVisibleFrameSlack() {
    return g_debugCacheObsoleteVisibleFrameSlack.load();
}

int debugMaxVisibleBacklog() {
    return g_debugMaxVisibleBacklog.load();
}

int debugSequenceVisibleDecodeKeepWindow() {
    return g_debugSequenceVisibleDecodeKeepWindow.load();
}

int debugSequenceObsoleteVisibleFrameSlack() {
    return g_debugSequenceObsoleteVisibleFrameSlack.load();
}

int debugSequenceLateBufferSeedSlack() {
    return g_debugSequenceLateBufferSeedSlack.load();
}

DecodePreference debugDecodePreference() {
    return static_cast<DecodePreference>(g_decodePreference.load());
}

H26xSoftwareThreadingMode debugH26xSoftwareThreadingMode() {
    return static_cast<H26xSoftwareThreadingMode>(g_h26xSoftwareThreadingMode.load());
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

bool debugTemporalDebugOverlayEnabled() {
    return g_debugTemporalDebugOverlayEnabled.load();
}

int debugTimelineAudioEnvelopeGranularity() {
    return g_debugTimelineAudioEnvelopeGranularity.load();
}

RubberBandEnginePreference rubberBandEnginePreference() {
    return static_cast<RubberBandEnginePreference>(g_rubberBandEnginePreference.load());
}

RubberBandThreadingPreference rubberBandThreadingPreference() {
    return static_cast<RubberBandThreadingPreference>(g_rubberBandThreadingPreference.load());
}

RubberBandWindowPreference rubberBandWindowPreference() {
    return static_cast<RubberBandWindowPreference>(g_rubberBandWindowPreference.load());
}

RubberBandPitchPreference rubberBandPitchPreference() {
    return static_cast<RubberBandPitchPreference>(g_rubberBandPitchPreference.load());
}

bool rubberBandChannelsTogether() {
    return g_rubberBandChannelsTogether.load();
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

void setDebugSupersedeSlackFrames(int slack) {
    g_debugSupersedeSlackFrames.store(qBound(1, slack, 64));
}

void setDebugVisibleDecodeKeepWindow(int window) {
    g_debugVisibleDecodeKeepWindow.store(qBound(1, window, 128));
}

void setDebugObsoleteVisibleFrameSlack(int slack) {
    g_debugObsoleteVisibleFrameSlack.store(qBound(0, slack, 32));
}

void setDebugCancelBeforeMinFrameAdvance(int advance) {
    g_debugCancelBeforeMinFrameAdvance.store(qBound(1, advance, 64));
}

void setDebugCancelBeforeMinIntervalMs(qint64 ms) {
    g_debugCancelBeforeMinIntervalMs.store(qBound<qint64>(qint64(1), ms, qint64(1000)));
}

void setDebugMaxPresentationPastFrameDelta(int delta) {
    g_debugMaxPresentationPastFrameDelta.store(qBound(0, delta, 64));
}

void setDebugMaxPresentationFutureFrameDelta(int delta) {
    g_debugMaxPresentationFutureFrameDelta.store(qBound(0, delta, 64));
}

void setDebugFileVideoPlaybackWindowAhead(int ahead) {
    g_debugFileVideoPlaybackWindowAhead.store(qBound(0, ahead, 32));
}

void setDebugVisiblePendingRetryMs(int ms) {
    g_debugVisiblePendingRetryMs.store(qBound(100, ms, 30000));
}

void setDebugCacheObsoleteVisibleFrameSlack(int slack) {
    g_debugCacheObsoleteVisibleFrameSlack.store(qBound(0, slack, 32));
}

void setDebugMaxVisibleBacklog(int backlog) {
    g_debugMaxVisibleBacklog.store(qBound(0, backlog, 16));
}

void setDebugSequenceVisibleDecodeKeepWindow(int window) {
    g_debugSequenceVisibleDecodeKeepWindow.store(qBound(1, window, 256));
}

void setDebugSequenceObsoleteVisibleFrameSlack(int slack) {
    g_debugSequenceObsoleteVisibleFrameSlack.store(qBound(0, slack, 64));
}

void setDebugSequenceLateBufferSeedSlack(int slack) {
    g_debugSequenceLateBufferSeedSlack.store(qBound(0, slack, 64));
}

void setDebugDecodePreference(DecodePreference preference) {
    g_decodePreference.store(static_cast<int>(preference));
}

void setDebugH26xSoftwareThreadingMode(H26xSoftwareThreadingMode mode) {
    g_h26xSoftwareThreadingMode.store(static_cast<int>(mode));
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

void setDebugTemporalDebugOverlayEnabled(bool enabled) {
    g_debugTemporalDebugOverlayEnabled.store(enabled);
}

void setDebugTimelineAudioEnvelopeGranularity(int granularity) {
    g_debugTimelineAudioEnvelopeGranularity.store(qBound(64, granularity, 8192));
}

void setRubberBandEnginePreference(RubberBandEnginePreference preference) {
    g_rubberBandEnginePreference.store(static_cast<int>(preference));
}

void setRubberBandThreadingPreference(RubberBandThreadingPreference preference) {
    g_rubberBandThreadingPreference.store(static_cast<int>(preference));
}

void setRubberBandWindowPreference(RubberBandWindowPreference preference) {
    g_rubberBandWindowPreference.store(static_cast<int>(preference));
}

void setRubberBandPitchPreference(RubberBandPitchPreference preference) {
    g_rubberBandPitchPreference.store(static_cast<int>(preference));
}

void setRubberBandChannelsTogether(bool enabled) {
    g_rubberBandChannelsTogether.store(enabled);
}

RenderPipelineDefaults defaultRenderPipelineDefaultsForCurrentSystem() {
    RenderPipelineDefaults defaults;

    defaults.decodePreference = DecodePreference::Software;
    defaults.h26xSoftwareThreadingMode = H26xSoftwareThreadingMode::Auto;
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
            // Prefer the hardware zero-copy path on NVIDIA as well.
            // Runtime already falls back when interop cannot be sustained.
            defaults.decodePreference = DecodePreference::HardwareZeroCopy;
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
        {QStringLiteral("supersede_slack_frames"), debugSupersedeSlackFrames()},
        {QStringLiteral("visible_decode_keep_window"), debugVisibleDecodeKeepWindow()},
        {QStringLiteral("obsolete_visible_frame_slack"), debugObsoleteVisibleFrameSlack()},
        {QStringLiteral("cancel_before_min_frame_advance"), debugCancelBeforeMinFrameAdvance()},
        {QStringLiteral("cancel_before_min_interval_ms"), debugCancelBeforeMinIntervalMs()},
        {QStringLiteral("max_presentation_past_frame_delta"), debugMaxPresentationPastFrameDelta()},
        {QStringLiteral("max_presentation_future_frame_delta"), debugMaxPresentationFutureFrameDelta()},
        {QStringLiteral("file_video_playback_window_ahead"), debugFileVideoPlaybackWindowAhead()},
        {QStringLiteral("visible_pending_retry_ms"), debugVisiblePendingRetryMs()},
        {QStringLiteral("cache_obsolete_visible_frame_slack"), debugCacheObsoleteVisibleFrameSlack()},
        {QStringLiteral("max_visible_backlog"), debugMaxVisibleBacklog()},
        {QStringLiteral("sequence_visible_decode_keep_window"), debugSequenceVisibleDecodeKeepWindow()},
        {QStringLiteral("decode_mode"), decodePreferenceToString(debugDecodePreference())},
        {QStringLiteral("h26x_software_threading_mode"),
         h26xSoftwareThreadingModeToString(debugH26xSoftwareThreadingMode())},
        {QStringLiteral("playhead_no_repaint"), debugPlayheadNoRepaint()},
        {QStringLiteral("playback_cache_fallback"), debugPlaybackCacheFallbackEnabled()},
        {QStringLiteral("deterministic_pipeline"), debugDeterministicPipelineEnabled()},
        {QStringLiteral("temporal_debug_overlay"), debugTemporalDebugOverlayEnabled()},
        {QStringLiteral("timeline_audio_envelope_granularity"),
         debugTimelineAudioEnvelopeGranularity()},
        {QStringLiteral("rubberband_engine"),
         rubberBandEnginePreferenceToString(rubberBandEnginePreference())},
        {QStringLiteral("rubberband_threading"),
         rubberBandThreadingPreferenceToString(rubberBandThreadingPreference())},
        {QStringLiteral("rubberband_window"),
         rubberBandWindowPreferenceToString(rubberBandWindowPreference())},
        {QStringLiteral("rubberband_pitch"),
         rubberBandPitchPreferenceToString(rubberBandPitchPreference())},
        {QStringLiteral("rubberband_channels_together"), rubberBandChannelsTogether()}
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
    if (name == QStringLiteral("supersede_slack_frames") && value.isDouble()) {
        setDebugSupersedeSlackFrames(value.toInt());
        return true;
    }
    if (name == QStringLiteral("visible_decode_keep_window") && value.isDouble()) {
        setDebugVisibleDecodeKeepWindow(value.toInt());
        return true;
    }
    if (name == QStringLiteral("obsolete_visible_frame_slack") && value.isDouble()) {
        setDebugObsoleteVisibleFrameSlack(value.toInt());
        return true;
    }
    if (name == QStringLiteral("cancel_before_min_frame_advance") && value.isDouble()) {
        setDebugCancelBeforeMinFrameAdvance(value.toInt());
        return true;
    }
    if (name == QStringLiteral("cancel_before_min_interval_ms") && value.isDouble()) {
        setDebugCancelBeforeMinIntervalMs(static_cast<qint64>(value.toDouble()));
        return true;
    }
    if (name == QStringLiteral("max_presentation_past_frame_delta") && value.isDouble()) {
        setDebugMaxPresentationPastFrameDelta(value.toInt());
        return true;
    }
    if (name == QStringLiteral("max_presentation_future_frame_delta") && value.isDouble()) {
        setDebugMaxPresentationFutureFrameDelta(value.toInt());
        return true;
    }
    if (name == QStringLiteral("file_video_playback_window_ahead") && value.isDouble()) {
        setDebugFileVideoPlaybackWindowAhead(value.toInt());
        return true;
    }
    if (name == QStringLiteral("visible_pending_retry_ms") && value.isDouble()) {
        setDebugVisiblePendingRetryMs(value.toInt());
        return true;
    }
    if (name == QStringLiteral("cache_obsolete_visible_frame_slack") && value.isDouble()) {
        setDebugCacheObsoleteVisibleFrameSlack(value.toInt());
        return true;
    }
    if (name == QStringLiteral("max_visible_backlog") && value.isDouble()) {
        setDebugMaxVisibleBacklog(value.toInt());
        return true;
    }
    if (name == QStringLiteral("sequence_visible_decode_keep_window") && value.isDouble()) {
        setDebugSequenceVisibleDecodeKeepWindow(value.toInt());
        return true;
    }
    if (name == QStringLiteral("sequence_obsolete_visible_frame_slack") && value.isDouble()) {
        setDebugSequenceObsoleteVisibleFrameSlack(value.toInt());
        return true;
    }
    if (name == QStringLiteral("sequence_late_buffer_seed_slack") && value.isDouble()) {
        setDebugSequenceLateBufferSeedSlack(value.toInt());
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
    if (name == QStringLiteral("h26x_software_threading_mode") && value.isString()) {
        H26xSoftwareThreadingMode mode = H26xSoftwareThreadingMode::Auto;
        if (!parseH26xSoftwareThreadingMode(value.toString(), &mode)) {
            return false;
        }
        setDebugH26xSoftwareThreadingMode(mode);
        return true;
    }
    if (name == QStringLiteral("timeline_audio_envelope_granularity") && value.isDouble()) {
        setDebugTimelineAudioEnvelopeGranularity(value.toInt());
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
    if (name == QStringLiteral("temporal_debug_overlay") && value.isBool()) {
        setDebugTemporalDebugOverlayEnabled(value.toBool());
        return true;
    }
    if (name == QStringLiteral("rubberband_engine") && value.isString()) {
        RubberBandEnginePreference preference = RubberBandEnginePreference::Finer;
        if (!parseRubberBandEnginePreference(value.toString(), &preference)) {
            return false;
        }
        setRubberBandEnginePreference(preference);
        return true;
    }
    if (name == QStringLiteral("rubberband_threading") && value.isString()) {
        RubberBandThreadingPreference preference = RubberBandThreadingPreference::Always;
        if (!parseRubberBandThreadingPreference(value.toString(), &preference)) {
            return false;
        }
        setRubberBandThreadingPreference(preference);
        return true;
    }
    if (name == QStringLiteral("rubberband_window") && value.isString()) {
        RubberBandWindowPreference preference = RubberBandWindowPreference::Standard;
        if (!parseRubberBandWindowPreference(value.toString(), &preference)) {
            return false;
        }
        setRubberBandWindowPreference(preference);
        return true;
    }
    if (name == QStringLiteral("rubberband_pitch") && value.isString()) {
        RubberBandPitchPreference preference = RubberBandPitchPreference::HighQuality;
        if (!parseRubberBandPitchPreference(value.toString(), &preference)) {
            return false;
        }
        setRubberBandPitchPreference(preference);
        return true;
    }
    if (name == QStringLiteral("rubberband_channels_together") && value.isBool()) {
        setRubberBandChannelsTogether(value.toBool());
        return true;
    }
    return false;
}

void setDecoderLaneCountChangedCallback(DecoderLaneCountChangedCallback callback) {
    std::lock_guard<std::mutex> lock(g_decoderLaneCountCallbackMutex);
    g_decoderLaneCountChangedCallback = std::move(callback);
}

} // namespace editor
