#include "audio_engine.h"
#include "audio_engine_internal.h"
#include "audio_mix_readiness.h"
#include "audio_speech_harmonic_isolator.h"
#include "audio_source_key.h"
#include "debug_controls.h"

#include <QDateTime>
#include <QDebug>
#include <QFileInfo>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <limits>

using namespace jcut::audio_internal;

QString AudioEngine::timeStretchJobKey(const QString &path, int speedKey) {
  return QStringLiteral("%1|%2").arg(path, QString::number(speedKey));
}

void AudioEngine::markTimeStretchJob(const QString &path, int speedKey,
                                     int state, qreal progress) {
  uint64_t sourceGeneration = 0;
  {
    std::lock_guard<std::mutex> stateLock(m_stateMutex);
    sourceGeneration = m_sourceGeneration;
  }
  (void)markTimeStretchJobForSourceGeneration(
      path, speedKey, state, progress, sourceGeneration);
}

bool AudioEngine::markTimeStretchJobForSourceGeneration(
    const QString &path, int speedKey, int state, qreal progress,
    uint64_t expectedSourceGeneration) {
  if (path.isEmpty() || speedKey <= 1) {
    return false;
  }
  std::scoped_lock lock(m_stateMutex, m_timeStretchGenerationMutex);
  if (expectedSourceGeneration != m_sourceGeneration) {
    return false;
  }
  markTimeStretchJobLocked(path, speedKey, state, progress,
                           expectedSourceGeneration);
  return true;
}

void AudioEngine::markTimeStretchJobLocked(const QString &path, int speedKey,
                                           int state, qreal progress,
                                           uint64_t sourceGeneration) {
  const QString key = timeStretchJobKey(path, speedKey);
  TimeStretchJobProgress job = m_timeStretchJobs.value(key);
  job.key = key;
  job.path = path;
  job.speedKey = speedKey;
  job.state = state;
  job.progress = qBound<qreal>(0.0, progress, 1.0);
  job.updatedMs = QDateTime::currentMSecsSinceEpoch();
  job.sourceGeneration = sourceGeneration;
  m_timeStretchJobs.insert(key, job);
  if (state == TimeStretchJobFailed) {
    m_timeStretchFailedJobs.insert(key, job.updatedMs);
  } else if (state == TimeStretchJobComplete) {
    m_timeStretchFailedJobs.remove(key);
    m_timeStretchRetrySuppressedMs.remove(key);
  }
}

bool AudioEngine::timeStretchJobRecentlyFailed(const QString &path,
                                               int speedKey) const {
  if (path.isEmpty() || speedKey <= 1) {
    return false;
  }
  uint64_t sourceGeneration = 0;
  {
    std::lock_guard<std::mutex> stateLock(m_stateMutex);
    sourceGeneration = m_sourceGeneration;
  }
  const qint64 nowMs = QDateTime::currentMSecsSinceEpoch();
  std::lock_guard<std::mutex> generationLock(m_timeStretchGenerationMutex);
  const QString key = timeStretchJobKey(path, speedKey);
  const auto jobIt = m_timeStretchJobs.constFind(key);
  if (jobIt == m_timeStretchJobs.constEnd() ||
      jobIt->sourceGeneration != sourceGeneration ||
      jobIt->state != TimeStretchJobFailed) {
    return false;
  }
  const auto it = m_timeStretchFailedJobs.constFind(key);
  if (it == m_timeStretchFailedJobs.constEnd()) {
    return false;
  }
  const bool recent = nowMs - it.value() < kTimeStretchFailedJobRetryDelayMs;
  if (recent) {
    m_timeStretchRetrySuppressedMs.insert(key, nowMs);
    m_timeStretchGenerationRetrySuppressedMs.store(nowMs,
                                                   std::memory_order_release);
    m_timeStretchGenerationLastEndReason =
        QStringLiteral("retry_suppressed_after_recent_failure");
  }
  return recent;
}

bool AudioEngine::publishRecentTimeStretchFailureForSourceGeneration(
    const QString &path, int speedKey, uint64_t expectedSourceGeneration) {
  if (path.isEmpty() || speedKey <= 1) {
    return false;
  }
  const qint64 nowMs = QDateTime::currentMSecsSinceEpoch();
  std::scoped_lock lock(m_stateMutex, m_timeStretchGenerationMutex);
  if (expectedSourceGeneration != m_sourceGeneration) {
    return false;
  }
  const QString key = timeStretchJobKey(path, speedKey);
  const auto jobIt = m_timeStretchJobs.constFind(key);
  const auto failedIt = m_timeStretchFailedJobs.constFind(key);
  if (jobIt == m_timeStretchJobs.constEnd() ||
      jobIt->sourceGeneration != expectedSourceGeneration ||
      jobIt->state != TimeStretchJobFailed ||
      failedIt == m_timeStretchFailedJobs.constEnd() ||
      nowMs - failedIt.value() >= kTimeStretchFailedJobRetryDelayMs) {
    return false;
  }
  m_timeStretchRetrySuppressedMs.insert(key, nowMs);
  m_timeStretchGenerationRetrySuppressedMs.store(nowMs,
                                                 std::memory_order_release);
  m_timeStretchGenerationLastEndReason =
      QStringLiteral("retry_suppressed_after_recent_failure");
  m_audioPlaybackBlocked.store(true, std::memory_order_release);
  m_pitchPreservingAudioBlocked.store(true, std::memory_order_release);
  m_timeStretchPrecomputeBlocked.store(false, std::memory_order_release);
  m_timeStretchReadinessState.store(TimeStretchReadinessMissing,
                                    std::memory_order_release);
  markTimeStretchJobLocked(path, speedKey, TimeStretchJobFailed, 0.0,
                           expectedSourceGeneration);
  return true;
}

int AudioEngine::beginTimeStretchJobAttempt(const QString &path,
                                            int speedKey,
                                            uint64_t expectedSourceGeneration) {
  if (path.isEmpty() || speedKey <= 1) {
    return 0;
  }
  std::scoped_lock lock(m_stateMutex, m_timeStretchGenerationMutex);
  if (expectedSourceGeneration != m_sourceGeneration) {
    return 0;
  }
  const QString key = timeStretchJobKey(path, speedKey);
  const int attempt = m_timeStretchJobAttemptCounts.value(key, 0) + 1;
  m_timeStretchJobAttemptCounts.insert(key, attempt);
  m_timeStretchRetrySuppressedMs.remove(key);
  m_timeStretchGenerationAttempt.store(attempt, std::memory_order_release);
  m_timeStretchGenerationRetrySuppressedMs.store(0, std::memory_order_release);
  m_timeStretchGenerationLastEndReason.clear();
  return attempt;
}

bool AudioEngine::beginTimeStretchGenerationForSourceGeneration(
    const QString &path, int speedKey, int64_t sourceFrames,
    uint64_t expectedSourceGeneration) {
  if (path.isEmpty() || speedKey <= 1) {
    return false;
  }
  std::scoped_lock lock(m_stateMutex, m_timeStretchGenerationMutex);
  if (expectedSourceGeneration != m_sourceGeneration) {
    return false;
  }
  m_timeStretchGenerationSourceGeneration = expectedSourceGeneration;
  m_timeStretchGenerationPath = path;
  m_timeStretchGenerationSidecarPath =
      audioTimeStretchSidecarPathForSource(path, speedKey);
  m_timeStretchGenerationLastError.clear();
  m_timeStretchGenerationLastEndReason.clear();
  m_timeStretchGenerationStartedMs.store(QDateTime::currentMSecsSinceEpoch(),
                                         std::memory_order_release);
  m_timeStretchGenerationLastFinishMs.store(0, std::memory_order_release);
  m_timeStretchGenerationSpeedKey.store(speedKey, std::memory_order_release);
  m_timeStretchGenerationSourceFrames.store(sourceFrames,
                                            std::memory_order_release);
  m_timeStretchGenerationOutputFrames.store(0, std::memory_order_release);
  m_timeStretchGenerationProgressPermille.store(0, std::memory_order_release);
  m_timeStretchGenerationLastSucceeded.store(false,
                                             std::memory_order_release);
  m_timeStretchGenerationPhase.store(TimeStretchGenerationReadingSidecar,
                                     std::memory_order_release);
  m_timeStretchGenerationActive.store(true, std::memory_order_release);
  markTimeStretchJobLocked(path, speedKey, TimeStretchJobReadingSidecar, 0.0,
                           expectedSourceGeneration);
  return true;
}

bool AudioEngine::updateTimeStretchGenerationForSourceGeneration(
    const QString &path, int speedKey, int jobState, qreal progress,
    int generationPhase, uint64_t expectedSourceGeneration) {
  std::scoped_lock lock(m_stateMutex, m_timeStretchGenerationMutex);
  if (expectedSourceGeneration != m_sourceGeneration ||
      !m_timeStretchGenerationActive.load(std::memory_order_acquire) ||
      m_timeStretchGenerationSourceGeneration != expectedSourceGeneration ||
      m_timeStretchGenerationPath != path ||
      m_timeStretchGenerationSpeedKey.load(std::memory_order_acquire) !=
          speedKey) {
    return false;
  }
  const qreal boundedProgress = qBound<qreal>(0.0, progress, 1.0);
  m_timeStretchGenerationProgressPermille.store(
      qBound(0, qRound(boundedProgress * 1000.0), 1000),
      std::memory_order_release);
  m_timeStretchGenerationPhase.store(generationPhase,
                                     std::memory_order_release);
  markTimeStretchJobLocked(path, speedKey, jobState, boundedProgress,
                           expectedSourceGeneration);
  return true;
}

bool AudioEngine::finishTimeStretchGenerationForSourceGeneration(
    const QString &path, int speedKey, bool succeeded, int64_t outputFrames,
    const QString &endReason, const QString &error,
    uint64_t expectedSourceGeneration) {
  std::scoped_lock lock(m_stateMutex, m_timeStretchGenerationMutex);
  if (expectedSourceGeneration != m_sourceGeneration ||
      !m_timeStretchGenerationActive.load(std::memory_order_acquire) ||
      m_timeStretchGenerationSourceGeneration != expectedSourceGeneration ||
      m_timeStretchGenerationPath != path ||
      m_timeStretchGenerationSpeedKey.load(std::memory_order_acquire) !=
          speedKey) {
    return false;
  }
  m_timeStretchGenerationOutputFrames.store(qMax<int64_t>(0, outputFrames),
                                            std::memory_order_release);
  m_timeStretchGenerationLastError = error;
  m_timeStretchGenerationLastEndReason = endReason;
  m_timeStretchGenerationLastSucceeded.store(succeeded,
                                             std::memory_order_release);
  m_timeStretchGenerationProgressPermille.store(succeeded ? 1000 : 0,
                                                std::memory_order_release);
  m_timeStretchGenerationPhase.store(
      succeeded ? TimeStretchGenerationFinished : TimeStretchGenerationFailed,
      std::memory_order_release);
  m_timeStretchGenerationLastFinishMs.store(QDateTime::currentMSecsSinceEpoch(),
                                            std::memory_order_release);
  m_timeStretchGenerationActive.store(false, std::memory_order_release);
  markTimeStretchJobLocked(
      path, speedKey,
      succeeded ? TimeStretchJobComplete : TimeStretchJobFailed,
      succeeded ? 1.0 : 0.0, expectedSourceGeneration);
  m_timeStretchGenerationSourceGeneration = 0;
  return true;
}

void AudioEngine::abandonTimeStretchGenerationForSourceGeneration(
    const QString &path, int speedKey, uint64_t expectedSourceGeneration) {
  std::lock_guard<std::mutex> generationLock(m_timeStretchGenerationMutex);
  if (!m_timeStretchGenerationActive.load(std::memory_order_acquire) ||
      m_timeStretchGenerationSourceGeneration != expectedSourceGeneration ||
      m_timeStretchGenerationPath != path ||
      m_timeStretchGenerationSpeedKey.load(std::memory_order_acquire) !=
          speedKey) {
    return;
  }
  m_timeStretchGenerationActive.store(false, std::memory_order_release);
  m_timeStretchGenerationPhase.store(TimeStretchGenerationIdle,
                                     std::memory_order_release);
  m_timeStretchGenerationProgressPermille.store(0, std::memory_order_release);
  m_timeStretchGenerationLastSucceeded.store(false,
                                             std::memory_order_release);
  m_timeStretchGenerationLastFinishMs.store(QDateTime::currentMSecsSinceEpoch(),
                                            std::memory_order_release);
  m_timeStretchGenerationLastError.clear();
  m_timeStretchGenerationLastEndReason =
      QStringLiteral("source_generation_invalidated");
  m_timeStretchGenerationSourceGeneration = 0;
}

AudioEngine::TimeStretchProgressSnapshot
AudioEngine::timeStretchProgressSnapshot() const {
  TimeStretchProgressSnapshot snapshot;
  const PlaybackAudioWarpMode warpMode = static_cast<PlaybackAudioWarpMode>(
      m_playbackWarpMode.load(std::memory_order_acquire));
  const int speedKey = precomputedTimeStretchSpeedKey(
      m_playbackRate.load(std::memory_order_acquire), warpMode);

  QVector<TimeStretchJobProgress> jobs;
  {
    std::lock_guard<std::mutex> generationLock(m_timeStretchGenerationMutex);
    jobs.reserve(m_timeStretchJobs.size());
    for (auto it = m_timeStretchJobs.cbegin(); it != m_timeStretchJobs.cend();
         ++it) {
      if (speedKey <= 1 || it.value().speedKey == speedKey) {
        jobs.push_back(it.value());
      }
    }
  }

  if (jobs.isEmpty()) {
    return snapshot;
  }

  auto statePriority = [](int state) {
    switch (state) {
    case TimeStretchJobGenerating:
      return 0;
    case TimeStretchJobWritingSidecar:
      return 1;
    case TimeStretchJobReadingSidecar:
      return 2;
    case TimeStretchJobDecoding:
      return 3;
    case TimeStretchJobQueued:
      return 4;
    case TimeStretchJobFailed:
      return 5;
    case TimeStretchJobComplete:
    default:
      return 6;
    }
  };

  int currentIndex = -1;
  int currentPriority = 99;
  int completed = 0;
  int remaining = 0;
  for (int i = 0; i < jobs.size(); ++i) {
    const TimeStretchJobProgress &job = jobs.at(i);
    // Reading an existing sidecar is cache validation/loading, not artifact
    // generation. Do not surface the generation dialog for a cache hit. If
    // the sidecar is missing or invalid, the job transitions to Generating
    // below and becomes visible on the next progress snapshot.
    if (job.state == TimeStretchJobComplete ||
        job.state == TimeStretchJobReadingSidecar) {
      ++completed;
      continue;
    }
    ++remaining;
    const int priority = statePriority(job.state);
    if (currentIndex < 0 || priority < currentPriority ||
        (priority == currentPriority &&
         job.updatedMs > jobs.at(currentIndex).updatedMs)) {
      currentIndex = i;
      currentPriority = priority;
    }
  }

  snapshot.totalClips = jobs.size();
  snapshot.completedClips = completed;
  snapshot.remainingClips = remaining;
  snapshot.visible = remaining > 0;
  if (currentIndex >= 0) {
    const TimeStretchJobProgress &current = jobs.at(currentIndex);
    snapshot.generationActive =
        current.state == TimeStretchJobGenerating ||
        current.state == TimeStretchJobWritingSidecar ||
        current.state == TimeStretchJobReadingSidecar;
    snapshot.currentPath = current.path;
    snapshot.phase = timeStretchJobStateString(current.state);
    snapshot.currentProgress = current.progress;
  }
  if (snapshot.totalClips > 0) {
    const qreal currentContribution =
        currentIndex >= 0 && snapshot.currentProgress >= 0.0
            ? snapshot.currentProgress
            : 0.0;
    snapshot.overallProgress = qBound<qreal>(
        0.0,
        (static_cast<qreal>(snapshot.completedClips) + currentContribution) /
            static_cast<qreal>(snapshot.totalClips),
        1.0);
  }
  return snapshot;
}

qreal AudioEngine::normalizedTimeStretchSpeed(qreal playbackRate) {
  return qBound<qreal>(0.1, playbackRate, 3.0);
}

int AudioEngine::precomputedTimeStretchSpeedKey(qreal playbackRate) {
  return precomputedTimeStretchSpeedKey(playbackRate,
                                        PlaybackAudioWarpMode::TimeStretch);
}

int AudioEngine::precomputedTimeStretchSpeedKey(qreal playbackRate,
                                                PlaybackAudioWarpMode mode) {
  const qreal speed = normalizedTimeStretchSpeed(playbackRate);
  if (!pitchPreservingTimeStretchActive(speed, mode)) {
    return 0;
  }
  const int engine = editor::rubberBandEnginePreference() ==
                             editor::RubberBandEnginePreference::Faster
                         ? 0
                         : 1;
  int threading = 0;
  switch (editor::rubberBandThreadingPreference()) {
  case editor::RubberBandThreadingPreference::Never:
    threading = 1;
    break;
  case editor::RubberBandThreadingPreference::Always:
    threading = 2;
    break;
  case editor::RubberBandThreadingPreference::Auto:
    threading = 0;
    break;
  }
  int window = 0;
  switch (editor::rubberBandWindowPreference()) {
  case editor::RubberBandWindowPreference::Short:
    window = 1;
    break;
  case editor::RubberBandWindowPreference::Long:
    window = 2;
    break;
  case editor::RubberBandWindowPreference::Standard:
    window = 0;
    break;
  }
  int pitch = 0;
  switch (editor::rubberBandPitchPreference()) {
  case editor::RubberBandPitchPreference::HighQuality:
    pitch = 1;
    break;
  case editor::RubberBandPitchPreference::HighConsistency:
    pitch = 2;
    break;
  case editor::RubberBandPitchPreference::HighSpeed:
    pitch = 0;
    break;
  }
  const int channels = editor::rubberBandChannelsTogether() ? 1 : 0;
  const int settingsOrdinal =
      engine + (threading * 2) + (window * 6) + (pitch * 18) + (channels * 54);
  // Sidecars are persistent. Include the algorithm in the key so the
  // two-pass speech treatment can never alias an ordinary stretch generated
  // at the same transport speed and with the same Rubber Band settings.
  const int modeOrdinal =
      mode == PlaybackAudioWarpMode::RubberBandPassThroughFrequency
          ? editor::audio::SpeechHarmonicIsolator::kAlgorithmVersion
          : 0;
  return qMax(1, qRound(speed * 1000.0) * 100000 +
                     modeOrdinal * 10000 + settingsOrdinal);
}

bool AudioEngine::pitchPreservingTimeStretchActive(qreal playbackRate) {
  return pitchPreservingTimeStretchActive(playbackRate,
                                          PlaybackAudioWarpMode::TimeStretch);
}

bool AudioEngine::pitchPreservingTimeStretchActive(qreal playbackRate,
                                                   PlaybackAudioWarpMode mode) {
  if (!playbackWarpModeUsesTimeStretch(mode)) {
    return false;
  }
  if (playbackWarpModeForcesUnityTimeStretch(mode)) {
    return true;
  }
  return qAbs(normalizedTimeStretchSpeed(playbackRate) - 1.0) >= 0.0001;
}

bool AudioEngine::playbackWarpModeUsesTimeStretch(PlaybackAudioWarpMode mode) {
  return mode == PlaybackAudioWarpMode::TimeStretch ||
         mode == PlaybackAudioWarpMode::RubberBand ||
         mode == PlaybackAudioWarpMode::RubberBandPassThroughFrequency;
}

bool AudioEngine::playbackWarpModeForcesUnityTimeStretch(
    PlaybackAudioWarpMode mode) {
  return mode == PlaybackAudioWarpMode::RubberBand;
}

int AudioEngine::timeStretchRateKey(qreal playbackRate) {
  return qRound(normalizedTimeStretchSpeed(playbackRate) * 1000.0);
}

QString AudioEngine::timeStretchGenerationPhaseString(int phase) {
  switch (phase) {
  case TimeStretchGenerationReadingSidecar:
    return QStringLiteral("reading_sidecar");
  case TimeStretchGenerationRubberBand:
    return QStringLiteral("rubberband");
  case TimeStretchGenerationWritingSidecar:
    return QStringLiteral("writing_sidecar");
  case TimeStretchGenerationFinished:
    return QStringLiteral("finished");
  case TimeStretchGenerationFailed:
    return QStringLiteral("failed");
  case TimeStretchGenerationIdle:
  default:
    return QStringLiteral("idle");
  }
}

QString AudioEngine::timeStretchReadinessStateString(int state) {
  switch (state) {
  case TimeStretchReadinessNotNeeded:
    return QStringLiteral("not_needed");
  case TimeStretchReadinessReadyInMemory:
    return QStringLiteral("ready_in_memory");
  case TimeStretchReadinessReadingSidecar:
    return QStringLiteral("reading_sidecar");
  case TimeStretchReadinessReadyFromSidecar:
    return QStringLiteral("ready_from_sidecar");
  case TimeStretchReadinessQueuedPrecompute:
    return QStringLiteral("queued_precompute");
  case TimeStretchReadinessMissing:
    return QStringLiteral("missing");
  case TimeStretchReadinessIdle:
  default:
    return QStringLiteral("idle");
  }
}

int64_t AudioEngine::timeStretchCacheSampleForSourceSample(int64_t sourceSample,
                                                           qreal playbackRate) {
  return audioTimeStretchCacheSampleForSourceSample(sourceSample, playbackRate);
}

int64_t AudioEngine::timeStretchCacheEndSampleForSourceEndSample(
    int64_t sourceEndSample, qreal playbackRate) {
  return audioTimeStretchCacheEndSampleForSourceEndSample(sourceEndSample,
                                                          playbackRate);
}

int64_t
AudioEngine::sourceSamplesCoveredByTimeStretchCacheSamples(int64_t cacheSamples,
                                                           qreal playbackRate) {
  return audioTimeStretchSourceSamplesCoveredByCacheSamples(cacheSamples,
                                                            playbackRate);
}

AudioTimeStretchRubberBandSettings
AudioEngine::rubberBandSettingsFromRuntimeControls() {
  AudioTimeStretchRubberBandSettings settings;
  settings.engine = editor::rubberBandEnginePreference() ==
                            editor::RubberBandEnginePreference::Faster
                        ? RubberBandEngineMode::Faster
                        : RubberBandEngineMode::Finer;
  switch (editor::rubberBandThreadingPreference()) {
  case editor::RubberBandThreadingPreference::Never:
    settings.threading = RubberBandThreadingMode::Never;
    break;
  case editor::RubberBandThreadingPreference::Always:
    settings.threading = RubberBandThreadingMode::Always;
    break;
  case editor::RubberBandThreadingPreference::Auto:
    settings.threading = RubberBandThreadingMode::Auto;
    break;
  }
  switch (editor::rubberBandWindowPreference()) {
  case editor::RubberBandWindowPreference::Short:
    settings.window = RubberBandWindowMode::Short;
    break;
  case editor::RubberBandWindowPreference::Long:
    settings.window = RubberBandWindowMode::Long;
    break;
  case editor::RubberBandWindowPreference::Standard:
    settings.window = RubberBandWindowMode::Standard;
    break;
  }
  switch (editor::rubberBandPitchPreference()) {
  case editor::RubberBandPitchPreference::HighSpeed:
    settings.pitch = RubberBandPitchMode::HighSpeed;
    break;
  case editor::RubberBandPitchPreference::HighConsistency:
    settings.pitch = RubberBandPitchMode::HighConsistency;
    break;
  case editor::RubberBandPitchPreference::HighQuality:
    settings.pitch = RubberBandPitchMode::HighQuality;
    break;
  }
  settings.channelsTogether = editor::rubberBandChannelsTogether();
  return settings;
}

bool AudioEngine::audioEntryCoversSourceSample(const AudioClipCacheEntry &entry,
                                               int64_t sourceSample,
                                               int64_t minFrames) {
  if (!entry.valid || entry.samples.isEmpty()) {
    return false;
  }
  const int64_t frameCount =
      static_cast<int64_t>(entry.samples.size() / qMax(1, entry.channelCount));
  const int64_t localSample = sourceSample - entry.sourceStartSample;
  return localSample >= 0 &&
         localSample + qMax<int64_t>(1, minFrames) <= frameCount;
}

bool AudioEngine::clipAndSourceSampleAtTimelineSampleLocked(
    int64_t timelineSample, const TimelineClip **clipOut, QString *audioPathOut,
    int64_t *sourceSampleOut) const {
  for (const TimelineClip &clip : m_timelineClips) {
    if (!clipAudioPlaybackEnabled(clip)) {
      continue;
    }
    const int64_t clipStart = clipTimelineStartSamples(clip);
    const int64_t clipEnd = clipTimelineEndSamples(clip);
    if (timelineSample < clipStart || timelineSample >= clipEnd) {
      continue;
    }
    const QString audioPath = clipAudioPathForScheduling(clip);
    if (audioPath.isEmpty()) {
      continue;
    }
    if (clipOut) {
      *clipOut = &clip;
    }
    if (audioPathOut) {
      *audioPathOut = audioPath;
    }
    if (sourceSampleOut) {
      *sourceSampleOut = sourceSampleForClipAtTimelineSample(
          clip, timelineSample, m_renderSyncMarkers);
    }
    return true;
  }
  return false;
}

bool AudioEngine::audioReadyForTimelineSampleLocked(
    int64_t timelineSample) const {
  const TimelineClip *clip = nullptr;
  QString audioPath;
  int64_t sourceSample = 0;
  if (!clipAndSourceSampleAtTimelineSampleLocked(timelineSample, &clip,
                                                 &audioPath, &sourceSample)) {
    return true;
  }

  const qreal playbackRate =
      qBound<qreal>(0.1, m_playbackRate.load(std::memory_order_acquire), 3.0);
  const PlaybackAudioWarpMode warpMode = static_cast<PlaybackAudioWarpMode>(
      m_playbackWarpMode.load(std::memory_order_acquire));
  if (playbackWarpModeUsesTimeStretch(warpMode) &&
      pitchPreservingTimeStretchActive(playbackRate, warpMode)) {
    const auto pathIt = m_timeStretchAudioCache.constFind(audioPath);
    if (pathIt == m_timeStretchAudioCache.cend()) {
      return false;
    }
    const int64_t stretchedSourceSample =
        timeStretchCacheSampleForSourceSample(sourceSample, playbackRate);
    const auto speedIt =
        pathIt.value().constFind(timeStretchRateKey(playbackRate));
    if (speedIt == pathIt.value().cend()) {
      return false;
    }
    for (const AudioClipCacheEntry &entry : speedIt.value()) {
      if (audioEntryCoversSourceSample(entry, stretchedSourceSample,
                                       kPlaybackWarmupFrames)) {
        return true;
      }
    }
    return false;
  }

  return audioEntryCoversSourceSample(m_audioCache.value(audioPath),
                                      sourceSample, 1);
}

bool AudioEngine::ensureTimeStretchAudioReadyForTimelineSample(
    int64_t timelineSample) {
  QString audioPath;
  qreal playbackRate = 1.0;
  PlaybackAudioWarpMode warpMode = PlaybackAudioWarpMode::Disabled;
  int64_t sourceSample = 0;
  uint64_t sourceGeneration = 0;
  {
    std::lock_guard<std::mutex> lock(m_stateMutex);
    if (audioReadyForTimelineSampleLocked(timelineSample)) {
      m_timeStretchReadinessState.store(TimeStretchReadinessReadyInMemory,
                                        std::memory_order_release);
      return true;
    }
    if (!clipAndSourceSampleAtTimelineSampleLocked(timelineSample, nullptr,
                                                   &audioPath, &sourceSample)) {
      m_timeStretchReadinessState.store(TimeStretchReadinessNotNeeded,
                                        std::memory_order_release);
      return true;
    }
    playbackRate =
        qBound<qreal>(0.1, m_playbackRate.load(std::memory_order_acquire), 3.0);
    warpMode = static_cast<PlaybackAudioWarpMode>(
        m_playbackWarpMode.load(std::memory_order_acquire));
    sourceGeneration = m_sourceGeneration;
    if (!playbackWarpModeUsesTimeStretch(warpMode) ||
        !pitchPreservingTimeStretchActive(playbackRate, warpMode)) {
      m_timeStretchReadinessState.store(TimeStretchReadinessNotNeeded,
                                        std::memory_order_release);
      return false;
    }
  }

  const int sidecarSpeedKey =
      precomputedTimeStretchSpeedKey(playbackRate, warpMode);
  if (sidecarSpeedKey > 1 &&
      publishRecentTimeStretchFailureForSourceGeneration(
          audioPath, sidecarSpeedKey, sourceGeneration)) {
    return false;
  }

  m_timeStretchReadinessState.store(TimeStretchReadinessReadingSidecar,
                                    std::memory_order_release);
  const AudioClipCacheEntry entry = timeStretchCacheForPathCopy(
      audioPath, playbackRate, sourceSample,
      sourceSample + qMax<int64_t>(1, kPlaybackWarmupFrames), warpMode);
  if (entry.valid) {
    m_audioPlaybackBlocked.store(false, std::memory_order_release);
    m_pitchPreservingAudioBlocked.store(false, std::memory_order_release);
    m_timeStretchPrecomputeBlocked.store(false, std::memory_order_release);
    m_timeStretchReadinessState.store(TimeStretchReadinessReadyFromSidecar,
                                      std::memory_order_release);
    m_stateCondition.notify_all();
    m_mixCondition.notify_all();
    return true;
  }

  {
    std::lock_guard<std::mutex> lock(m_stateMutex);
    enqueueTimeStretchPrecomputeForPathLocked(audioPath, 0, false);
  }
  m_timeStretchReadinessState.store(TimeStretchReadinessQueuedPrecompute,
                                    std::memory_order_release);
  m_timeStretchPrecomputeBlocked.store(true, std::memory_order_release);
  m_decodeCondition.notify_one();
  return false;
}

void AudioEngine::requestAudioForTimelineSampleLocked(int64_t timelineSample) {
  const TimelineClip *clip = nullptr;
  QString audioPath;
  int64_t sourceSample = 0;
  if (!clipAndSourceSampleAtTimelineSampleLocked(timelineSample, &clip,
                                                 &audioPath, &sourceSample)) {
    return;
  }
  const qreal playbackRate =
      qBound<qreal>(0.1, m_playbackRate.load(std::memory_order_acquire), 3.0);
  const PlaybackAudioWarpMode warpMode = static_cast<PlaybackAudioWarpMode>(
      m_playbackWarpMode.load(std::memory_order_acquire));
  if (playbackWarpModeUsesTimeStretch(warpMode) &&
      pitchPreservingTimeStretchActive(playbackRate, warpMode)) {
    enqueueTimeStretchPrecomputeForPathLocked(audioPath, 0, true);
    return;
  }
  enqueueDecodePathLocked(
      audioPath, true, false, false,
      qMax<int64_t>(0, sourceSample - kPreviewDecodePrerollFrames), true);
}

int AudioEngine::rtAudioCallback(void *outputBuffer, void * /*inputBuffer*/,
                                 unsigned int nFrames, double /*streamTime*/,
                                 unsigned int status,
                                 void *userData) {
  auto *engine = static_cast<AudioEngine *>(userData);
  auto *out = static_cast<int16_t *>(outputBuffer);
  const size_t samplesNeeded =
      static_cast<size_t>(nFrames) * engine->m_channelCount;
  const size_t read = engine->m_ringBuffer.read(out, samplesNeeded);
  engine->m_lastCallbackRequestedSamples.store(
      static_cast<qint64>(samplesNeeded), std::memory_order_release);
  engine->m_lastCallbackReadSamples.store(static_cast<qint64>(read),
                                          std::memory_order_release);
  engine->m_lastCallbackUnderrunSamples.store(
      static_cast<qint64>(samplesNeeded > read ? samplesNeeded - read : 0),
      std::memory_order_release);

  if (nFrames > 0) {
    const int64_t sinkFrames = static_cast<int64_t>(nFrames);
    const qreal playbackRate = qBound<qreal>(
        0.1, engine->m_playbackRate.load(std::memory_order_acquire), 3.0);
    const qreal driftRetimeRate = qBound<qreal>(
        0.92,
        engine->m_playbackDriftRetimeRate.load(std::memory_order_acquire),
        1.08);
    const int64_t timelineAdvance =
        qMax<int64_t>(1, static_cast<int64_t>(std::llround(
                             static_cast<long double>(sinkFrames) *
                             static_cast<long double>(playbackRate * driftRetimeRate))));
    engine->m_audioClockSample.fetch_add(timelineAdvance,
                                         std::memory_order_release);
  }
  // Fill remainder with silence on underrun
  if (read < samplesNeeded) {
    std::memset(out + read, 0, (samplesNeeded - read) * sizeof(int16_t));
    engine->m_underrunCount.fetch_add(1, std::memory_order_relaxed);
  }
  if (status != 0 || read < samplesNeeded) {
    engine->m_mixCondition.notify_one();
  }
  if (samplesNeeded >= static_cast<size_t>(engine->m_channelCount)) {
    const size_t lastIndex =
        samplesNeeded - static_cast<size_t>(engine->m_channelCount);
    engine->m_lastOutputLeft.store(out[lastIndex], std::memory_order_release);
    engine->m_lastOutputRight.store(out[lastIndex + 1],
                                    std::memory_order_release);
  }
  return 0;
}

int64_t AudioEngine::timelineFrameToSamples(int64_t frame) const {
  return frameToSamples(frame);
}

int64_t AudioEngine::samplesToTimelineFrame(int64_t samples) const {
  return qMax<int64_t>(
      0, static_cast<int64_t>(std::floor(
             (static_cast<double>(samples) * kTimelineFps) / m_sampleRate)));
}

int64_t AudioEngine::nextPlayableSampleAtOrAfter(
    int64_t samplePos, const QVector<ExportRangeSegment> &ranges) const {
  if (ranges.isEmpty()) {
    return qMax<int64_t>(0, samplePos);
  }
  for (const ExportRangeSegment &range : ranges) {
    const int64_t rangeStartSample = timelineFrameToSamples(range.startFrame);
    const int64_t rangeEndSampleExclusive =
        timelineFrameToSamples(range.endFrame + 1);
    if (samplePos < rangeStartSample) {
      return rangeStartSample;
    }
    if (samplePos >= rangeStartSample && samplePos < rangeEndSampleExclusive) {
      return samplePos;
    }
  }
  return qMax<int64_t>(0, samplePos);
}

void AudioEngine::enqueueDecodePathLocked(const QString &audioPath,
                                          bool highPriority, bool fullDecode,
                                          bool precomputeTimeStretch,
                                          int64_t sourceStartSample,
                                          bool force) {
  if (audioPath.isEmpty()) {
    return;
  }
  const int64_t boundedSourceStartSample =
      fullDecode ? 0 : qMax<int64_t>(0, sourceStartSample);
  const auto activeIt = m_activeDecodeFullDecode.constFind(audioPath);
  if (activeIt != m_activeDecodeFullDecode.cend()) {
    if (fullDecode && !activeIt.value()) {
      m_fullDecodeRequestedWhileActive.insert(audioPath);
    }
    if (precomputeTimeStretch) {
      m_timeStretchPrecomputeRequestedWhileActive.insert(
          audioPath, boundedSourceStartSample);
    }
    return;
  }
  auto cacheIt = m_audioCache.constFind(audioPath);
  if (!force && cacheIt != m_audioCache.cend()) {
    if (cacheIt->fullyDecoded || !fullDecode) {
      return;
    }
  }
  if (m_pendingDecodeSet.contains(audioPath)) {
    for (auto it = m_pendingDecodePaths.begin();
         it != m_pendingDecodePaths.end(); ++it) {
      if (it->key == audioPath) {
        if (fullDecode && !it->fullDecode) {
          it->fullDecode = true;
        }
        it->precomputeTimeStretch =
            it->precomputeTimeStretch || precomputeTimeStretch;
        if (!it->fullDecode) {
          it->sourceStartSample = boundedSourceStartSample;
        }
        if (!highPriority) {
          return;
        }
        DecodeTask updated = *it;
        m_pendingDecodePaths.erase(it);
        m_pendingDecodePaths.push_front(updated);
        return;
      }
    }
    m_pendingDecodeSet.remove(audioPath);
  }
  DecodeTask task;
  task.key = audioPath;
  task.fullDecode = fullDecode;
  task.precomputeTimeStretch = precomputeTimeStretch;
  task.sourceStartSample = boundedSourceStartSample;
  task.sourceGeneration = m_sourceGeneration;
  if (highPriority) {
    m_pendingDecodePaths.push_front(task);
  } else {
    m_pendingDecodePaths.push_back(task);
  }
  m_pendingDecodeSet.insert(audioPath);
}

void AudioEngine::enqueueTimeStretchPrecomputeForScheduledPaths() {
  const qreal speed =
      qBound<qreal>(0.1, m_playbackRate.load(std::memory_order_acquire), 3.0);
  const PlaybackAudioWarpMode warpMode = static_cast<PlaybackAudioWarpMode>(
      m_playbackWarpMode.load(std::memory_order_acquire));
  if (!playbackWarpModeUsesTimeStretch(warpMode) ||
      !pitchPreservingTimeStretchActive(speed, warpMode)) {
    return;
  }

  const int speedKey = precomputedTimeStretchSpeedKey(speed, warpMode);
  QVector<QString> queuedPaths;
  QVector<QString> completedPaths;
  {
    std::lock_guard<std::mutex> lock(m_stateMutex);
    for (const QString &audioPath : m_scheduledDecodePaths) {
      if (timeStretchCacheHasFullyDecodedPathLocked(audioPath, speed)) {
        completedPaths.push_back(audioPath);
        continue;
      }
      queuedPaths.push_back(audioPath);
      enqueueDecodePathLocked(audioPath, false, true, true, 0, true);
      ++m_timeStretchPrecomputeRequestCount;
    }
  }
  for (const QString &audioPath : std::as_const(completedPaths)) {
    markTimeStretchJob(audioPath, speedKey, TimeStretchJobComplete, 1.0);
  }
  for (const QString &audioPath : std::as_const(queuedPaths)) {
    markTimeStretchJob(audioPath, speedKey, TimeStretchJobQueued, 0.0);
  }
  if (queuedPaths.isEmpty()) {
    m_timeStretchPrecomputeBlocked.store(false, std::memory_order_release);
    return;
  }
  m_timeStretchPrecomputeBlocked.store(true, std::memory_order_release);
  m_decodeCondition.notify_one();
}

void AudioEngine::enqueueTimeStretchPrecomputeForPath(
    const QString &audioPath, int64_t sourceStartSample) {
  if (!audioPath.isEmpty()) {
    const PlaybackAudioWarpMode warpMode = static_cast<PlaybackAudioWarpMode>(
        m_playbackWarpMode.load(std::memory_order_acquire));
    const int speedKey = precomputedTimeStretchSpeedKey(
        m_playbackRate.load(std::memory_order_acquire), warpMode);
    if (timeStretchJobRecentlyFailed(audioPath, speedKey)) {
      m_timeStretchPrecomputeBlocked.store(false, std::memory_order_release);
      m_audioPlaybackBlocked.store(true, std::memory_order_release);
      m_pitchPreservingAudioBlocked.store(true, std::memory_order_release);
      return;
    }
    markTimeStretchJob(audioPath, speedKey, TimeStretchJobQueued, 0.0);
    std::lock_guard<std::mutex> lock(m_stateMutex);
    m_lastTimeStretchCacheMissPath = audioPath;
    enqueueTimeStretchPrecomputeForPathLocked(audioPath, sourceStartSample,
                                              true);
  }
  m_timeStretchPrecomputeBlocked.store(true, std::memory_order_release);
  m_decodeCondition.notify_one();
}

void AudioEngine::enqueueTimeStretchPrecomputeForPathLocked(
    const QString &audioPath, int64_t sourceStartSample, bool highPriority) {
  if (audioPath.isEmpty()) {
    return;
  }
  m_lastTimeStretchCacheMissPath = audioPath;
  ++m_timeStretchPrecomputeRequestCount;
  enqueueDecodePathLocked(audioPath, highPriority, true, true,
                          sourceStartSample, true);
}

void AudioEngine::enqueuePreviewDecodeForPath(const QString &audioPath,
                                              int64_t sourceStartSample) {
  if (audioPath.isEmpty()) {
    return;
  }
  const int64_t segmentStart =
      qMax<int64_t>(0, sourceStartSample - kPreviewDecodePrerollFrames);
  {
    std::lock_guard<std::mutex> lock(m_stateMutex);
    enqueueDecodePathLocked(audioPath, true, false, false, segmentStart, true);
  }
  m_decodeCondition.notify_one();
}

void AudioEngine::removePendingDecodePathLocked(const QString &audioPath) {
  if (audioPath.isEmpty() || !m_pendingDecodeSet.contains(audioPath)) {
    return;
  }
  m_pendingDecodeSet.remove(audioPath);
  for (auto it = m_pendingDecodePaths.begin();
       it != m_pendingDecodePaths.end();) {
    if (it->key == audioPath) {
      it = m_pendingDecodePaths.erase(it);
    } else {
      ++it;
    }
  }
}

QString
AudioEngine::clipAudioPathForScheduling(const TimelineClip &clip) const {
  if (!clipAudioPlaybackEnabled(clip) ||
      clip.audioSourceStatus == QStringLiteral("missing")) {
    return QString();
  }
  QString audioPath;
  if (clip.audioSourceStatus == QStringLiteral("ok") &&
      !clip.audioSourcePath.trimmed().isEmpty()) {
    audioPath = QFileInfo(clip.audioSourcePath).absoluteFilePath();
  } else if (clip.filePath.isEmpty()) {
    return QString();
  } else if (clip.audioSourceMode == QStringLiteral("embedded")) {
    audioPath = QFileInfo(clip.filePath).absoluteFilePath();
  } else {
    audioPath = playbackAudioPathForClip(clip);
  }
  return editor::audio::makeSourceKey(audioPath, clip.audioStreamIndex);
}

QSet<QString> AudioEngine::scheduledAudioPathsFromClips(
    const QVector<TimelineClip> &clips) const {
  QSet<QString> paths;
  for (const TimelineClip &clip : clips) {
    const QString audioPath = clipAudioPathForScheduling(clip);
    if (!audioPath.isEmpty()) {
      paths.insert(audioPath);
    }
  }
  return paths;
}

void AudioEngine::scheduleDecodesLocked(const QVector<TimelineClip> &clips) {
  for (const TimelineClip &clip : clips) {
    const QString audioPath = clipAudioPathForScheduling(clip);
    if (audioPath.isEmpty()) {
      continue;
    }
    enqueueDecodePathLocked(audioPath, false, false);
  }
}

void AudioEngine::prioritizeDecodesNearSampleLocked(int64_t focusSample) {
  if (m_timelineClips.isEmpty() || m_pendingDecodePaths.empty()) {
    return;
  }

  struct Candidate {
    int64_t distance = std::numeric_limits<int64_t>::max();
    QString key;
  };
  QVector<Candidate> candidates;
  candidates.reserve(m_timelineClips.size());

  for (const TimelineClip &clip : m_timelineClips) {
    const QString audioPath = clipAudioPathForScheduling(clip);
    if (audioPath.isEmpty() || m_audioCache.contains(audioPath)) {
      continue;
    }
    const int64_t clipStart = clipTimelineStartSamples(clip);
    const int64_t clipLenSamples = clipTimelineDurationSamples(clip);
    const int64_t clipEndExclusive = clipStart + clipLenSamples;
    int64_t distance = 0;
    if (focusSample < clipStart) {
      distance = clipStart - focusSample;
    } else if (focusSample >= clipEndExclusive) {
      distance = focusSample - clipEndExclusive;
    }
    candidates.push_back({distance, audioPath});
  }

  if (candidates.isEmpty()) {
    return;
  }

  std::sort(candidates.begin(), candidates.end(),
            [](const Candidate &a, const Candidate &b) {
              if (a.distance != b.distance) {
                return a.distance < b.distance;
              }
              return a.key < b.key;
            });

  constexpr int kHighPriorityDecodeCount = 4;
  QSet<QString> promoted;
  int promotedCount = 0;
  for (const Candidate &candidate : std::as_const(candidates)) {
    if (promoted.contains(candidate.key)) {
      continue;
    }
    int64_t sourceStartSample = 0;
    for (const TimelineClip &clip : std::as_const(m_timelineClips)) {
      if (clipAudioPathForScheduling(clip) != candidate.key) {
        continue;
      }
      const int64_t clipStart = clipTimelineStartSamples(clip);
      const int64_t clipEnd = clipTimelineEndSamples(clip);
      const int64_t timelineSample =
          qBound<int64_t>(clipStart, focusSample, clipEnd - 1);
      sourceStartSample = sourceSampleForClipAtTimelineSample(
          clip, timelineSample, m_renderSyncMarkers);
      break;
    }
    enqueueDecodePathLocked(candidate.key, true, false, false,
                            sourceStartSample, true);
    promoted.insert(candidate.key);
    if (++promotedCount >= kHighPriorityDecodeCount) {
      break;
    }
  }
}
