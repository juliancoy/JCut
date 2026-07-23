#pragma once

#include "decoder_policy_core.h"

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

using DecodePreference = jcut::DecodePreferenceCore;
using H26xSoftwareThreadingMode = jcut::H26xThreadingModeCore;

enum class RubberBandEnginePreference : int {
    Faster = 0,
    Finer = 1,
};

enum class RubberBandThreadingPreference : int {
    Auto = 0,
    Never = 1,
    Always = 2,
};

enum class RubberBandWindowPreference : int {
    Standard = 0,
    Short = 1,
    Long = 2,
};

enum class RubberBandPitchPreference : int {
    HighSpeed = 0,
    HighQuality = 1,
    HighConsistency = 2,
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
int debugSupersedeSlackFrames();
int debugVisibleDecodeKeepWindow();
int debugObsoleteVisibleFrameSlack();
int debugCancelBeforeMinFrameAdvance();
qint64 debugCancelBeforeMinIntervalMs();
int debugMaxPresentationPastFrameDelta();
int debugMaxPresentationFutureFrameDelta();
int debugFileVideoPlaybackWindowAhead();
int debugVisiblePendingRetryMs();
int debugCacheObsoleteVisibleFrameSlack();
int debugMaxVisibleBacklog();
int debugSequenceVisibleDecodeKeepWindow();
int debugSequenceObsoleteVisibleFrameSlack();
int debugSequenceLateBufferSeedSlack();
DecodePreference debugDecodePreference();
H26xSoftwareThreadingMode debugH26xSoftwareThreadingMode();
bool debugPlayheadNoRepaint();
bool debugPlaybackCacheFallbackEnabled();
bool debugDeterministicPipelineEnabled();
bool debugTemporalDebugOverlayEnabled();
int debugTimelineAudioEnvelopeGranularity();
RubberBandEnginePreference rubberBandEnginePreference();
RubberBandThreadingPreference rubberBandThreadingPreference();
RubberBandWindowPreference rubberBandWindowPreference();
RubberBandPitchPreference rubberBandPitchPreference();
bool rubberBandChannelsTogether();

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
void setDebugSupersedeSlackFrames(int slack);
void setDebugVisibleDecodeKeepWindow(int window);
void setDebugObsoleteVisibleFrameSlack(int slack);
void setDebugCancelBeforeMinFrameAdvance(int advance);
void setDebugCancelBeforeMinIntervalMs(qint64 ms);
void setDebugMaxPresentationPastFrameDelta(int delta);
void setDebugMaxPresentationFutureFrameDelta(int delta);
void setDebugFileVideoPlaybackWindowAhead(int ahead);
void setDebugVisiblePendingRetryMs(int ms);
void setDebugCacheObsoleteVisibleFrameSlack(int slack);
void setDebugMaxVisibleBacklog(int backlog);
void setDebugSequenceVisibleDecodeKeepWindow(int window);
void setDebugSequenceObsoleteVisibleFrameSlack(int slack);
void setDebugSequenceLateBufferSeedSlack(int slack);
void setDebugDecodePreference(DecodePreference preference);
void setDebugH26xSoftwareThreadingMode(H26xSoftwareThreadingMode mode);
void setDebugPlayheadNoRepaint(bool enabled);
void setDebugPlaybackCacheFallbackEnabled(bool enabled);
void setDebugDeterministicPipelineEnabled(bool enabled);
void setDebugTemporalDebugOverlayEnabled(bool enabled);
void setDebugTimelineAudioEnvelopeGranularity(int granularity);
void setRubberBandEnginePreference(RubberBandEnginePreference preference);
void setRubberBandThreadingPreference(RubberBandThreadingPreference preference);
void setRubberBandWindowPreference(RubberBandWindowPreference preference);
void setRubberBandPitchPreference(RubberBandPitchPreference preference);
void setRubberBandChannelsTogether(bool enabled);

struct RenderPipelineDefaults {
    DecodePreference decodePreference = DecodePreference::Hardware;
    H26xSoftwareThreadingMode h26xSoftwareThreadingMode = H26xSoftwareThreadingMode::Auto;
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
QString h26xSoftwareThreadingModeToString(H26xSoftwareThreadingMode mode);
bool parseH26xSoftwareThreadingMode(const QString& text, H26xSoftwareThreadingMode* modeOut);
QString rubberBandEnginePreferenceToString(RubberBandEnginePreference preference);
bool parseRubberBandEnginePreference(const QString& text, RubberBandEnginePreference* preferenceOut);
QString rubberBandThreadingPreferenceToString(RubberBandThreadingPreference preference);
bool parseRubberBandThreadingPreference(const QString& text, RubberBandThreadingPreference* preferenceOut);
QString rubberBandWindowPreferenceToString(RubberBandWindowPreference preference);
bool parseRubberBandWindowPreference(const QString& text, RubberBandWindowPreference* preferenceOut);
QString rubberBandPitchPreferenceToString(RubberBandPitchPreference preference);
bool parseRubberBandPitchPreference(const QString& text, RubberBandPitchPreference* preferenceOut);

using DecoderLaneCountChangedCallback = std::function<void(int)>;
void setDecoderLaneCountChangedCallback(DecoderLaneCountChangedCallback callback);

} // namespace editor
