#pragma once

#include <QJsonObject>
#include <QJsonValue>
#include <QString>
#include <functional>

namespace editor {

enum class DebugLogLevel : int {
    Off = 0,
    Warn = 1,
    Info = 2,
    Debug = 3,
    Verbose = 4,
};

enum class DecodePreference : int {
    Auto = 0,
    HardwareZeroCopy = 1,
    Hardware = 2,
    Software = 3,
};

DebugLogLevel debugPlaybackLevel();
DebugLogLevel debugCacheLevel();
DebugLogLevel debugDecodeLevel();

bool debugPlaybackEnabled();
bool debugCacheEnabled();
bool debugDecodeEnabled();
bool debugPlaybackWarnEnabled();
bool debugCacheWarnEnabled();
bool debugDecodeWarnEnabled();
bool debugPlaybackWarnOnlyEnabled();
bool debugCacheWarnOnlyEnabled();
bool debugDecodeWarnOnlyEnabled();
bool debugPlaybackVerboseEnabled();
bool debugCacheVerboseEnabled();
bool debugDecodeVerboseEnabled();

bool debugLeadPrefetchEnabled();
int debugLeadPrefetchCount();
int debugPrefetchMaxQueueDepth();
int debugPrefetchMaxInflight();
int debugPrefetchMaxPerTick();
int debugPrefetchSkipVisiblePendingThreshold();
int debugVisibleQueueReserve();
int debugPlaybackWindowAhead();
int debugDecoderLaneCount();
DecodePreference debugDecodePreference();
bool debugPlayheadNoRepaint();
bool debugPlaybackCacheFallbackEnabled();
bool debugDeterministicPipelineEnabled();

void setDebugPlaybackEnabled(bool enabled);
void setDebugCacheEnabled(bool enabled);
void setDebugDecodeEnabled(bool enabled);
void setDebugPlaybackLevel(DebugLogLevel level);
void setDebugCacheLevel(DebugLogLevel level);
void setDebugDecodeLevel(DebugLogLevel level);
void setDebugLeadPrefetchEnabled(bool enabled);
void setDebugLeadPrefetchCount(int count);
void setDebugPrefetchMaxQueueDepth(int depth);
void setDebugPrefetchMaxInflight(int inflight);
void setDebugPrefetchMaxPerTick(int perTick);
void setDebugPrefetchSkipVisiblePendingThreshold(int threshold);
void setDebugVisibleQueueReserve(int reserve);
void setDebugPlaybackWindowAhead(int ahead);
void setDebugDecoderLaneCount(int count);
void setDebugDecodePreference(DecodePreference preference);
void setDebugPlayheadNoRepaint(bool enabled);
void setDebugPlaybackCacheFallbackEnabled(bool enabled);
void setDebugDeterministicPipelineEnabled(bool enabled);

struct RenderPipelineDefaults {
    DecodePreference decodePreference = DecodePreference::Software;
    bool deterministicPipeline = false;
    bool playbackCacheFallback = true;
    bool leadPrefetchEnabled = true;
    int leadPrefetchCount = 8;
    int playbackWindowAhead = 16;
    int visibleQueueReserve = 24;
    int prefetchMaxQueueDepth = 24;
    int prefetchMaxInflight = 8;
    int prefetchMaxPerTick = 8;
    int prefetchSkipVisiblePendingThreshold = 2;
    int decoderLaneCount = 0;
};

RenderPipelineDefaults defaultRenderPipelineDefaultsForCurrentSystem();

QJsonObject debugControlsSnapshot();
bool setDebugControl(const QString& name, bool enabled);
bool setDebugControlLevel(const QString& name, DebugLogLevel level);
bool setDebugOption(const QString& name, const QJsonValue& value);
QString debugLogLevelToString(DebugLogLevel level);
bool parseDebugLogLevel(const QString& text, DebugLogLevel* levelOut);
QString decodePreferenceToString(DecodePreference preference);
bool parseDecodePreference(const QString& text, DecodePreference* preferenceOut);

using DecoderLaneCountChangedCallback = std::function<void(int)>;
void setDecoderLaneCountChangedCallback(DecoderLaneCountChangedCallback callback);

} // namespace editor
