#pragma once

#include "audio_time_stretch.h"
#include "audio_time_stretch_cache.h"
#include "editor_shared.h"
#include "preview_surface.h"

#include <QHash>
#include <QJsonObject>
#include <QSet>
#include <QString>
#include <QVector>

#include <array>
#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <memory>
#include <mutex>
#include <thread>

#include <RtAudio.h>

// Lock-free SPSC ring buffer for int16 audio samples.
// Single producer (mix thread) writes, single consumer (RtAudio callback)
// reads.
struct AudioRingBuffer {
  friend class TestAudioMixPolicy;

  static constexpr size_t kCapacity = 32768; // power of 2

  size_t available() const;

  size_t space() const;

  size_t write(const int16_t *data, size_t count);

  size_t read(int16_t *data, size_t count);

  void clear();

private:
  std::array<int16_t, kCapacity> m_buffer{};
  std::atomic<size_t> m_readPos{0};
  std::atomic<size_t> m_writePos{0};
  std::atomic<bool> m_resetting{false};
  std::atomic<bool> m_readerActive{false};
  std::mutex m_clearMutex;
};

class AudioEngine {
  friend class TestAudioMixPolicy;

public:
  enum class SpeechFilterFadeMode {
    JumpCut = 0,
    Fade = 1,
    SmoothStep = 2,
    SmootherStep = 3,
    Crossfade = 4,
  };

  static QString speechFilterFadeModeToString(SpeechFilterFadeMode mode);
  static QString speechFilterFadeModeLabel(SpeechFilterFadeMode mode);
  static SpeechFilterFadeMode
  speechFilterFadeModeFromString(const QString &value,
                                 SpeechFilterFadeMode fallback =
                                     SpeechFilterFadeMode::Fade);

  AudioEngine() = default;
  ~AudioEngine();

  void setTimelineClips(const QVector<TimelineClip> &clips);

  void setTimelineTracks(const QVector<TimelineTrack> &tracks);

  void setExportRanges(const QVector<ExportRangeSegment> &ranges);

  void setRenderSyncMarkers(const QVector<RenderSyncMarker> &markers);

  // Installs all timeline inputs as one mixer-visible snapshot. ImGui uses
  // this boundary so a project switch or edit cannot expose new tracks with
  // old clips/ranges/markers between separate setter calls.
  void setTimelineState(const QVector<TimelineTrack> &tracks,
                        const QVector<TimelineClip> &clips,
                        const QVector<ExportRangeSegment> &ranges,
                        const QVector<RenderSyncMarker> &markers);

  // Atomically replaces the timeline and repositions playback. This is the
  // live-edit/project-switch boundary: an in-flight mixer chunk from the old
  // generation is discarded and cannot refill the cleared ring buffer.
  void setTimelineStateAtFrame(const QVector<TimelineTrack> &tracks,
                               const QVector<TimelineClip> &clips,
                               const QVector<ExportRangeSegment> &ranges,
                               const QVector<RenderSyncMarker> &markers,
                               int64_t frame);

  void setSpeechFilterFadeSamples(int samples);

  void setSpeechFilterFadeMode(SpeechFilterFadeMode mode);

  void setSpeechFilterCurveStrength(qreal strength);

  void setSpeechFilterRangeCrossfadeEnabled(bool enabled);

  void setPlaybackWarpMode(PlaybackAudioWarpMode mode);

  void setPlaybackRate(qreal rate);

  void setPlaybackDriftRetimeRate(qreal rate);

  void setTranscriptNormalizeEnabled(bool enabled);

  void setTranscriptNormalizeRanges(const QVector<ExportRangeSegment> &ranges);

  void setAudioDynamicsSettings(
      const PreviewSurface::AudioDynamicsSettings &settings);

  void setBackgroundDecodeSuppressed(bool suppressed);

  void setBufferFrames(int frames);

  int bufferFrames() const;

  bool initialize();

  void shutdown();

  void setMuted(bool muted);

  void setVolume(qreal volume);

  bool muted() const;

  int volumePercent() const;

  void start(int64_t startFrame);

  void stop();

  void seek(int64_t frame);

  bool hasPlayableAudio() const;

  QVector<QString> scheduledAudioSourcePaths() const;

  // Drops decoded data derived from external sources while keeping the
  // current timeline installed. Decode work already in flight is versioned
  // and cannot repopulate the caches after this call returns.
  void invalidateAudioSourceCaches();

  bool warmPlaybackAudio(int64_t startFrame, int timeoutMs);

  // Returns true only when warming the source active at startFrame cannot
  // make progress without a source-identity change. Timed-out work that is
  // still decoding/generating remains retryable.
  bool playbackAudioWarmupPermanentlyFailed(int64_t startFrame) const;

  bool playbackAudioReadyForFrame(int64_t startFrame) const;

  bool playbackAudioBlocked() const;

  bool playbackAudioNeedsRetimingForFrame(int64_t startFrame) const;

  bool audioClockAvailable() const;

  bool audioOutputUnavailableForPlayback() const;

  QString audioOutputStatusText() const;

  bool playbackStarted() const;

  QJsonObject profilingSnapshot() const;

  int64_t currentSample() const;

  int64_t playbackClockSample() const;

  int64_t currentFrame() const;

  qreal timeStretchGenerationProgress() const;

  bool timeStretchGenerationActive() const;

  struct TimeStretchProgressSnapshot {
    bool visible = false;
    bool generationActive = false;
    QString currentPath;
    QString phase;
    int totalClips = 0;
    int completedClips = 0;
    int remainingClips = 0;
    qreal currentProgress = -1.0;
    qreal overallProgress = -1.0;
  };

  TimeStretchProgressSnapshot timeStretchProgressSnapshot() const;

private:
  struct DecodeTask {
    QString key;
    bool fullDecode = false;
    bool precomputeTimeStretch = false;
    int64_t sourceStartSample = 0;
    uint64_t sourceGeneration = 0;
  };

  struct AudioClipCacheEntry {
    QVector<float> samples;
    int64_t sourceStartSample = 0;
    int sampleRate = 48000;
    int channelCount = 2;
    bool valid = false;
    bool fullyDecoded = false;
  };

  struct SpeechSampleRange {
    int64_t startSample = 0;
    int64_t endSampleExclusive = 0;
  };

  static qreal normalizedTimeStretchSpeed(qreal playbackRate);

  static int precomputedTimeStretchSpeedKey(qreal playbackRate);
  static int precomputedTimeStretchSpeedKey(qreal playbackRate,
                                            PlaybackAudioWarpMode mode);

  static bool pitchPreservingTimeStretchActive(qreal playbackRate);
  static bool pitchPreservingTimeStretchActive(qreal playbackRate,
                                               PlaybackAudioWarpMode mode);

  static bool playbackWarpModeUsesTimeStretch(PlaybackAudioWarpMode mode);
  static bool playbackWarpModeForcesUnityTimeStretch(PlaybackAudioWarpMode mode);

  static int timeStretchRateKey(qreal playbackRate);

  enum TimeStretchGenerationPhase {
    TimeStretchGenerationIdle = 0,
    TimeStretchGenerationReadingSidecar = 1,
    TimeStretchGenerationRubberBand = 2,
    TimeStretchGenerationWritingSidecar = 3,
    TimeStretchGenerationFinished = 4,
    TimeStretchGenerationFailed = 5,
  };

  static QString timeStretchGenerationPhaseString(int phase);

  enum TimeStretchJobState {
    TimeStretchJobQueued = 0,
    TimeStretchJobDecoding = 1,
    TimeStretchJobReadingSidecar = 2,
    TimeStretchJobGenerating = 3,
    TimeStretchJobWritingSidecar = 4,
    TimeStretchJobComplete = 5,
    TimeStretchJobFailed = 6,
  };

  struct TimeStretchJobProgress {
    QString key;
    QString path;
    int speedKey = 0;
    int state = TimeStretchJobQueued;
    qreal progress = 0.0;
    qint64 updatedMs = 0;
    uint64_t sourceGeneration = 0;
  };

  static QString timeStretchJobStateString(int state);

  static QString timeStretchJobKey(const QString &path, int speedKey);

  void markTimeStretchJob(const QString &path, int speedKey, int state,
                          qreal progress);
  bool markTimeStretchJobForSourceGeneration(
      const QString &path, int speedKey, int state, qreal progress,
      uint64_t expectedSourceGeneration);
  bool timeStretchJobRecentlyFailed(const QString &path, int speedKey) const;
  bool publishRecentTimeStretchFailureForSourceGeneration(
      const QString &path, int speedKey,
      uint64_t expectedSourceGeneration);
  int beginTimeStretchJobAttempt(const QString &path, int speedKey,
                                 uint64_t expectedSourceGeneration);
  bool beginTimeStretchGenerationForSourceGeneration(
      const QString &path, int speedKey, int64_t sourceFrames,
      uint64_t expectedSourceGeneration);
  bool updateTimeStretchGenerationForSourceGeneration(
      const QString &path, int speedKey, int jobState, qreal progress,
      int generationPhase, uint64_t expectedSourceGeneration);
  bool finishTimeStretchGenerationForSourceGeneration(
      const QString &path, int speedKey, bool succeeded,
      int64_t outputFrames, const QString &endReason, const QString &error,
      uint64_t expectedSourceGeneration);
  void abandonTimeStretchGenerationForSourceGeneration(
      const QString &path, int speedKey,
      uint64_t expectedSourceGeneration);
  void markTimeStretchJobLocked(const QString &path, int speedKey, int state,
                                qreal progress,
                                uint64_t sourceGeneration);

  enum TimeStretchReadinessState {
    TimeStretchReadinessIdle = 0,
    TimeStretchReadinessNotNeeded = 1,
    TimeStretchReadinessReadyInMemory = 2,
    TimeStretchReadinessReadingSidecar = 3,
    TimeStretchReadinessReadyFromSidecar = 4,
    TimeStretchReadinessQueuedPrecompute = 5,
    TimeStretchReadinessMissing = 6,
  };

  static QString timeStretchReadinessStateString(int state);

  static int64_t timeStretchCacheSampleForSourceSample(int64_t sourceSample,
                                                       qreal playbackRate);

  static int64_t
  timeStretchCacheEndSampleForSourceEndSample(int64_t sourceEndSample,
                                              qreal playbackRate);

  static int64_t
  sourceSamplesCoveredByTimeStretchCacheSamples(int64_t cacheSamples,
                                                qreal playbackRate);

  static AudioTimeStretchRubberBandSettings
  rubberBandSettingsFromRuntimeControls();

  static bool audioEntryCoversSourceSample(const AudioClipCacheEntry &entry,
                                           int64_t sourceSample,
                                           int64_t minFrames = 48000 * 2);

  bool clipAndSourceSampleAtTimelineSampleLocked(
      int64_t timelineSample, const TimelineClip **clipOut,
      QString *audioPathOut, int64_t *sourceSampleOut) const;

  bool audioReadyForTimelineSampleLocked(int64_t timelineSample) const;

  bool ensureTimeStretchAudioReadyForTimelineSample(int64_t timelineSample);

  void requestAudioForTimelineSampleLocked(int64_t timelineSample);

  void installTimelineStateLocked(
      const QVector<TimelineTrack> &tracks,
      const QVector<TimelineClip> &clips,
      const QVector<ExportRangeSegment> &ranges,
      const QVector<RenderSyncMarker> &markers);

  bool commitMixedChunk(uint64_t generation, const int16_t *samples,
                        size_t sampleCount, int64_t chunkEndSample);

  struct MixContext {
    QVector<TimelineClip> clips;
    QVector<TimelineTrack> tracks;
    QVector<ExportRangeSegment> exportRanges;
    QVector<SpeechSampleRange> speechSampleRanges;
    QVector<RenderSyncMarker> renderSyncMarkers;
    bool muted = false;
    qreal volume = 0.8;
  };

  // --- RtAudio callback (called from OS audio thread) ---

  static int rtAudioCallback(void *outputBuffer, void * /*inputBuffer*/,
                             unsigned int nFrames, double /*streamTime*/,
                             rt::audio::RtAudioStreamStatus /*status*/,
                             void *userData);

  // --- Sample math ---

  int64_t timelineFrameToSamples(int64_t frame) const;

  int64_t samplesToTimelineFrame(int64_t samples) const;

  int64_t
  nextPlayableSampleAtOrAfter(int64_t samplePos,
                              const QVector<ExportRangeSegment> &ranges) const;

  // --- Decode scheduling ---

  void enqueueDecodePathLocked(const QString &audioPath, bool highPriority,
                               bool fullDecode,
                               bool precomputeTimeStretch = false,
                               int64_t sourceStartSample = 0,
                               bool force = false);

  void enqueueTimeStretchPrecomputeForScheduledPaths();

  void enqueueTimeStretchPrecomputeForPath(const QString &audioPath,
                                           int64_t sourceStartSample = 0);

  void enqueueTimeStretchPrecomputeForPathLocked(const QString &audioPath,
                                                 int64_t sourceStartSample,
                                                 bool highPriority);

  void enqueuePreviewDecodeForPath(const QString &audioPath,
                                   int64_t sourceStartSample);

  void removePendingDecodePathLocked(const QString &audioPath);

  QString clipAudioPathForScheduling(const TimelineClip &clip) const;

  QSet<QString>
  scheduledAudioPathsFromClips(const QVector<TimelineClip> &clips) const;

  void scheduleDecodesLocked(const QVector<TimelineClip> &clips);

  void prioritizeDecodesNearSampleLocked(int64_t focusSample);

  // --- Gain calculations ---

  struct SpeechRangeBlend {
    float primaryGain = 1.0f;
    float secondaryGain = 0.0f;
    int64_t secondaryTimelineSample = -1;
  };

  SpeechRangeBlend
  calculateSpeechRangeBlend(int64_t samplePos,
                            const QVector<SpeechSampleRange> &ranges,
                            int fadeSamples,
                            SpeechFilterFadeMode fadeMode,
                            qreal curveStrength,
                            bool crossfadeEnabled) const;

  float calculateClipCrossfadeGain(int64_t samplePos, const TimelineClip &clip,
                                   int64_t clipStartSample,
                                   int64_t clipEndSample,
                                   int fadeSamples) const;

  // --- FFmpeg full-file decode ---

  AudioClipCacheEntry decodeClipAudio(const QString &path,
                                      int64_t maxOutputFrames,
                                      int64_t sourceStartSample = 0,
                                      int audioStreamIndex = -1);

  AudioClipCacheEntry clipCacheForPathCopy(const QString &path) const;

  AudioClipCacheEntry
  timeStretchCacheForPathCopy(const QString &path, qreal speed,
                              int64_t sourceSample,
                              int64_t sourceEndSampleExclusive,
                              PlaybackAudioWarpMode mode);

  void insertTimeStretchSegmentsLocked(
      const QString &path, QHash<int, AudioClipCacheEntry> warpedBySpeed);

  bool timeStretchCacheHasFullyDecodedPathLocked(const QString &path,
                                                 qreal speed) const;

  static AudioClipCacheEntry
  audioCacheEntryFromTimeStretchEntry(const AudioTimeStretchCacheEntry &source);

  static AudioTimeStretchCacheEntry
  timeStretchEntryFromAudioCacheEntry(const AudioClipCacheEntry &source);

  static AudioTimeStretchCacheEntry
  readTimeStretchSidecarEntry(const QString &path, int speedKey);

  static QString timeStretchSidecarLoadKey(const QString &path, int speedKey);

  AudioTimeStretchCacheEntry
  readTimeStretchSidecarEntrySingleFlight(const QString &path, int speedKey);

  AudioClipCacheEntry
  buildTimeStretchCacheEntry(const QString &path,
                             const AudioClipCacheEntry &decoded, qreal speed,
                             PlaybackAudioWarpMode mode,
                             uint64_t expectedSourceGeneration);

  QHash<int, AudioClipCacheEntry>
  buildPrecomputedTimeStretchEntries(const QString &path,
                                     const AudioClipCacheEntry &decoded,
                                     bool precomputeRequested,
                                     uint64_t expectedSourceGeneration);

  bool sourceGenerationCurrent(uint64_t expectedGeneration) const;

  bool recordDecodeResultLocked(const QString &path,
                                uint64_t expectedSourceGeneration,
                                bool valid);

  QVector<ExportRangeSegment> exportRangesCopy() const;

  QVector<ExportRangeSegment> transcriptNormalizeRangesCopy() const;

  // --- Mix engine ---

  bool mixChunk(const MixContext &context, float *output, int frames,
                int64_t chunkStartSample, qreal playbackRate,
                qreal timelineRate);

  // --- Worker threads ---

  void decodeLoop();

  void mixLoop();

  // --- Member variables ---

  mutable std::mutex m_stateMutex;
  // Serializes the source-generation transition with the final atomic rename
  // of a time-stretch sidecar, closing the check/commit race without holding
  // the mixer state lock during the bulk file write.
  std::mutex m_sourceGenerationCommitMutex;
  mutable std::mutex m_exportRangesMutex;
  mutable std::mutex m_transcriptNormalizeRangesMutex;
  std::mutex m_mixMutex;
  std::condition_variable m_stateCondition;
  std::condition_variable m_decodeCondition;
  std::condition_variable m_mixCondition;

  QVector<TimelineClip> m_timelineClips;
  QVector<TimelineTrack> m_timelineTracks;
  QVector<RenderSyncMarker> m_renderSyncMarkers;
  QVector<ExportRangeSegment> m_exportRanges;
  QVector<ExportRangeSegment> m_transcriptNormalizeRanges;
  QHash<QString, AudioClipCacheEntry> m_audioCache;
  QHash<QString, QHash<int, QVector<AudioClipCacheEntry>>>
      m_timeStretchAudioCache;
  QHash<QString, AudioTimeStretchCacheEntry> m_timeStretchSidecarEntryCache;
  QSet<QString> m_timeStretchSidecarLoadsInFlight;
  std::deque<DecodeTask> m_pendingDecodePaths;
  QSet<QString> m_pendingDecodeSet;
  QHash<QString, bool> m_activeDecodeFullDecode;
  QSet<QString> m_fullDecodeRequestedWhileActive;
  QHash<QString, int64_t> m_timeStretchPrecomputeRequestedWhileActive;
  QSet<QString> m_scheduledDecodePaths;
  QSet<QString> m_failedDecodePaths;

  std::thread m_decodeWorker;
  std::thread m_mixWorker;

  std::unique_ptr<rt::audio::RtAudio> m_rtaudio;
  AudioRingBuffer m_ringBuffer;
  std::atomic<int64_t> m_ringBufferEndSample{0};

  bool m_initialized = false;
  std::atomic<bool> m_running{false};
  std::atomic<bool> m_playing{false};
  bool m_backgroundDecodeSuppressed = false;
  bool m_muted = false;
  qreal m_volume = 0.8;
  int64_t m_timelineSampleCursor = 0;
  uint64_t m_mixGeneration = 0;
  uint64_t m_sourceGeneration = 0;
  std::atomic<int64_t> m_audioClockSample{0};
  mutable std::atomic<int64_t> m_lastReportedCurrentSample{0};
  std::atomic<qint64> m_startCount{0};
  std::atomic<qint64> m_redundantStartCount{0};
  std::atomic<qint64> m_seekCount{0};
  std::atomic<int64_t> m_lastStartFrame{-1};
  std::atomic<int64_t> m_lastSeekFrame{-1};
  std::atomic<int> m_underrunCount{0};
  std::atomic<qint64> m_lastCallbackRequestedSamples{0};
  std::atomic<qint64> m_lastCallbackReadSamples{0};
  std::atomic<qint64> m_lastCallbackUnderrunSamples{0};
  std::atomic<int> m_lastOutputLeft{0};
  std::atomic<int> m_lastOutputRight{0};
  std::atomic<int> m_lastMixPreparedClipCount{0};
  std::atomic<int> m_lastMixCacheHitCount{0};
  std::atomic<int> m_lastMixCacheMissCount{0};
  std::atomic<int> m_lastMixInvalidAudioCount{0};
  std::atomic<int> m_lastMixPeakPermille{0};
  std::atomic<int> m_lastMixRmsPermille{0};
  std::atomic<int> m_lastMixNonzeroSampleCount{0};
  std::atomic<qint64> m_lastMixChunkStartSample{0};
  std::atomic<qint64> m_lastMixChunkEndSample{0};
  std::atomic<int> m_lastMixFramesWithActiveClip{0};
  std::atomic<int> m_lastMixFramesInputOutOfRange{0};
  std::atomic<int> m_lastMixFramesSpeechGainZero{0};
  std::atomic<int> m_lastMixFramesClipGainZero{0};
  std::atomic<int> m_lastMixFramesSourceNonzero{0};
  std::atomic<int> m_lastMixFramesOutputNonzero{0};
  std::atomic<int> m_lastMixSourcePeakPermille{0};
  std::atomic<int> m_lastMixPrimaryGainPeakPermille{0};
  std::atomic<int64_t> m_lastMixOutOfRangeTimelineSample{-1};
  std::atomic<int64_t> m_lastMixOutOfRangeSourceSample{-1};
  std::atomic<int64_t> m_lastMixOutOfRangeNormalizedSample{-1};
  std::atomic<int64_t> m_lastMixOutOfRangeAudioStartSample{-1};
  std::atomic<int64_t> m_lastMixOutOfRangeAudioEndSample{-1};
  std::atomic<int> m_lastMixTimeStretchSpeedPermille{1000};
  std::atomic<qint64> m_lastMixFirstClipStartSample{0};
  std::atomic<qint64> m_lastMixFirstClipEndSample{0};
  std::atomic<qint64> m_lastMixFirstAudioStartSample{0};
  std::atomic<qint64> m_lastMixFirstAudioFrameCount{0};
  std::atomic<qint64> m_lastMixFirstLocalSampleAtChunkStart{0};
  std::atomic<int> m_lastMixSilentReason{0};
  std::atomic<int> m_lastMixStarvedClipCount{0};
  std::atomic<qint64> m_mixDegradedChunkCount{0};
  QString m_lastMixStarvedClipPath; // guarded by m_stateMutex
  std::atomic<qint64> m_lastTimeStretchCacheMissWarningMs{0};
  std::atomic<qint64> m_timeStretchCacheMissCount{0};
  std::atomic<int> m_lastTimeStretchCacheMissSpeed{0};
  std::atomic<bool> m_pitchPreservingAudioBlocked{false};
  std::atomic<bool> m_audioPlaybackBlocked{false};
  std::atomic<bool> m_timeStretchPrecomputeBlocked{false};
  std::atomic<int> m_timeStretchReadinessState{TimeStretchReadinessIdle};
  mutable std::mutex m_timeStretchGenerationMutex;
  mutable QHash<QString, TimeStretchJobProgress> m_timeStretchJobs;
  mutable QHash<QString, qint64> m_timeStretchFailedJobs;
  mutable QHash<QString, int> m_timeStretchJobAttemptCounts;
  mutable QHash<QString, qint64> m_timeStretchRetrySuppressedMs;
  mutable QString m_timeStretchGenerationPath;
  mutable QString m_timeStretchGenerationSidecarPath;
  mutable QString m_timeStretchGenerationLastError;
  mutable QString m_timeStretchGenerationLastEndReason;
  mutable uint64_t m_timeStretchGenerationSourceGeneration = 0;
  mutable std::atomic<int> m_timeStretchGenerationAttempt{0};
  mutable std::atomic<qint64> m_timeStretchGenerationRetrySuppressedMs{0};
  mutable std::atomic<bool> m_timeStretchGenerationActive{false};
  mutable std::atomic<int> m_timeStretchGenerationPhase{
      TimeStretchGenerationIdle};
  mutable std::atomic<int> m_timeStretchGenerationSpeedKey{0};
  mutable std::atomic<int64_t> m_timeStretchGenerationSourceFrames{0};
  mutable std::atomic<int64_t> m_timeStretchGenerationOutputFrames{0};
  mutable std::atomic<int> m_timeStretchGenerationProgressPermille{0};
  mutable std::atomic<qint64> m_timeStretchGenerationStartedMs{0};
  mutable std::atomic<qint64> m_timeStretchGenerationLastFinishMs{0};
  mutable std::atomic<bool> m_timeStretchGenerationLastSucceeded{false};
  qint64 m_timeStretchPrecomputeRequestCount = 0;
  QString m_lastTimeStretchCacheMissPath;
  std::atomic<qreal> m_playbackRate{1.0};
  std::atomic<qreal> m_playbackDriftRetimeRate{1.0};
  std::atomic<int> m_playbackWarpMode{
      static_cast<int>(PlaybackAudioWarpMode::Disabled)};

  static constexpr int m_sampleRate = 48000;
  static constexpr int m_channelCount = 2;
  int m_periodFrames = 1024; // guarded by m_stateMutex; immutable while initialized
  static constexpr int m_initialDecodeSeconds = 2;
  static constexpr int64_t kInitialDecodeFrames =
      static_cast<int64_t>(m_sampleRate) * m_initialDecodeSeconds;
  static constexpr int64_t kPreviewDecodeFrames =
      static_cast<int64_t>(m_sampleRate) * 8;
  static constexpr int64_t kTimeStretchPreviewDecodeFrames =
      static_cast<int64_t>(m_sampleRate) * 60;
  static constexpr int64_t kPreviewDecodePrerollFrames =
      static_cast<int64_t>(m_sampleRate);
  static constexpr int64_t kPlaybackWarmupFrames =
      static_cast<int64_t>(m_sampleRate) * 2;
  static constexpr int m_mixLowWaterSamples = 8192 * m_channelCount;
  static constexpr int m_defaultFadeSamples = 250;
  static constexpr int kShutdownFadeFrames = 1024;
  static constexpr qint64 kAudioInitBackoffMs = 10000;
  static constexpr qint64 kAudioInitWarningThrottleMs = 10000;
  std::atomic<int> m_speechFilterFadeSamples{m_defaultFadeSamples};
  std::atomic<int> m_speechFilterFadeMode{
      static_cast<int>(SpeechFilterFadeMode::Fade)};
  std::atomic<qreal> m_speechFilterCurveStrength{1.0};
  std::atomic<bool> m_speechFilterRangeCrossfadeEnabled{false};
  std::atomic<bool> m_transcriptNormalizeEnabled{false};
  std::atomic<bool> m_amplifyEnabled{false};
  std::atomic<qreal> m_amplifyDb{0.0};
  std::atomic<bool> m_normalizeEnabled{false};
  std::atomic<qreal> m_normalizeTargetDb{-1.0};
  std::atomic<bool> m_selectiveNormalizeEnabled{false};
  std::atomic<qreal> m_selectiveNormalizeMinSegmentSeconds{0.5};
  std::atomic<qreal> m_selectiveNormalizePeakDb{-12.0};
  std::atomic<int> m_selectiveNormalizePasses{1};
  std::atomic<bool> m_peakReductionEnabled{false};
  std::atomic<qreal> m_peakThresholdDb{-6.0};
  std::atomic<bool> m_limiterEnabled{false};
  std::atomic<qreal> m_limiterThresholdDb{-1.0};
  std::atomic<bool> m_compressorEnabled{false};
  std::atomic<qreal> m_compressorThresholdDb{-18.0};
  std::atomic<qreal> m_compressorRatio{3.0};
  std::atomic<bool> m_softClipEnabled{false};
  std::atomic<bool> m_stereoToMonoEnabled{false};
  qint64 m_audioInitBackoffUntilMs = 0;
  qint64 m_lastAudioInitWarningMs = 0;
  qint64 m_lastKnownDeviceCount = 0;
  bool m_lastKnownDefaultOutputValid = false;
  unsigned int m_lastKnownDefaultOutputId = 0;
  QString m_lastKnownDefaultOutputName;
  int m_lastKnownDefaultOutputChannels = 0;
  QString m_lastDeviceInfoError;
};
