#include "audio_engine.h"
#include "audio_dynamics_core.h"
#include "capabilities_detector.h"
#include "audio_speech_harmonic_isolator.h"

#include "audio_clip_fade.h"
#include "audio_mix_readiness.h"
#include "audio_source_key.h"
#include "debug_controls.h"
#include "decoder_ffmpeg_utils.h"
#include "ffmpeg_compat.h"

#include <QByteArray>
#include <QDateTime>
#include <QDebug>
#include <QFile>
#include <QFileInfo>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>
#include <limits>
#include <tuple>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/avutil.h>
#include <libavutil/channel_layout.h>
#include <libavutil/opt.h>
#include <libavutil/samplefmt.h>
#include <libswresample/swresample.h>
}


size_t AudioRingBuffer::available() const {
  // Read the consumer position first. During clear(), readPos may advance to
  // a writePos that the producer has just published; loading writePos first
  // could pair that newer readPos with an older writePos and wrap the
  // subtraction, making space() report more than the buffer capacity.
  const size_t rp = m_readPos.load(std::memory_order_acquire);
  const size_t wp = m_writePos.load(std::memory_order_acquire);
  return std::min(wp - rp, kCapacity);
}

size_t AudioRingBuffer::space() const { return kCapacity - available(); }

size_t AudioRingBuffer::write(const int16_t *data, size_t count) {
  const size_t avail = space();
  count = std::min(count, avail);
  const size_t wp = m_writePos.load(std::memory_order_relaxed);
  for (size_t i = 0; i < count; ++i)
    m_buffer[(wp + i) & (kCapacity - 1)] = data[i];
  m_writePos.store(wp + count, std::memory_order_release);
  return count;
}

size_t AudioRingBuffer::read(int16_t *data, size_t count) {
  // A controller-side clear must not release slots while the real-time
  // consumer is copying them. The double-check closes the race where reset
  // begins between the first flag read and publishing reader activity.
  // These flag operations form a two-party rendezvous with clear(). They
  // must share one total order: acquire/release alone permits the reader and
  // clearer to miss each other's store on weakly ordered CPUs.
  if (m_resetting.load(std::memory_order_seq_cst)) {
    return 0;
  }
  m_readerActive.store(true, std::memory_order_seq_cst);
  if (m_resetting.load(std::memory_order_seq_cst)) {
    m_readerActive.store(false, std::memory_order_seq_cst);
    return 0;
  }

  const size_t rp = m_readPos.load(std::memory_order_relaxed);
  const size_t wp = m_writePos.load(std::memory_order_acquire);
  const size_t avail = wp - rp;
  count = std::min(count, avail);
  for (size_t i = 0; i < count; ++i)
    data[i] = m_buffer[(rp + i) & (kCapacity - 1)];
  m_readPos.store(rp + count, std::memory_order_release);
  m_readerActive.store(false, std::memory_order_seq_cst);
  return count;
}

void AudioRingBuffer::clear() {
  std::lock_guard<std::mutex> clearLock(m_clearMutex);
  m_resetting.store(true, std::memory_order_seq_cst);
  while (m_readerActive.load(std::memory_order_seq_cst)) {
    std::this_thread::yield();
  }
  // Keep monotonically increasing positions; resetting both counters would
  // let producer/consumer observations underflow during the transition.
  m_readPos.store(m_writePos.load(std::memory_order_acquire),
                  std::memory_order_release);
  m_resetting.store(false, std::memory_order_seq_cst);
}

AudioEngine::~AudioEngine() { shutdown(); }

QString AudioEngine::speechFilterFadeModeToString(SpeechFilterFadeMode mode) {
  switch (mode) {
  case SpeechFilterFadeMode::JumpCut:
    return QStringLiteral("jumpCut");
  case SpeechFilterFadeMode::Fade:
    return QStringLiteral("fade");
  case SpeechFilterFadeMode::SmoothStep:
    return QStringLiteral("smoothStep");
  case SpeechFilterFadeMode::SmootherStep:
    return QStringLiteral("smootherStep");
  case SpeechFilterFadeMode::Crossfade:
    return QStringLiteral("crossfade");
  }
  return QStringLiteral("fade");
}

QString AudioEngine::speechFilterFadeModeLabel(SpeechFilterFadeMode mode) {
  switch (mode) {
  case SpeechFilterFadeMode::JumpCut:
    return QStringLiteral("Jump Cut");
  case SpeechFilterFadeMode::Fade:
    return QStringLiteral("Fade");
  case SpeechFilterFadeMode::SmoothStep:
    return QStringLiteral("Smooth Step");
  case SpeechFilterFadeMode::SmootherStep:
    return QStringLiteral("Smoother Step");
  case SpeechFilterFadeMode::Crossfade:
    return QStringLiteral("Crossfade");
  }
  return QStringLiteral("Fade");
}

AudioEngine::SpeechFilterFadeMode
AudioEngine::speechFilterFadeModeFromString(
    const QString &value, SpeechFilterFadeMode fallback) {
  const QString normalized = value.trimmed();
  if (normalized == QStringLiteral("jumpCut")) {
    return SpeechFilterFadeMode::JumpCut;
  }
  if (normalized == QStringLiteral("fade")) {
    return SpeechFilterFadeMode::Fade;
  }
  if (normalized == QStringLiteral("smoothStep")) {
    return SpeechFilterFadeMode::SmoothStep;
  }
  if (normalized == QStringLiteral("smootherStep")) {
    return SpeechFilterFadeMode::SmootherStep;
  }
  if (normalized == QStringLiteral("crossfade")) {
    return SpeechFilterFadeMode::Crossfade;
  }
  return fallback;
}

void AudioEngine::setTimelineClips(const QVector<TimelineClip> &clips) {
  bool queueChanged = false;
  {
    std::lock_guard<std::mutex> lock(m_stateMutex);
    const QSet<QString> nextScheduledPaths =
        scheduledAudioPathsFromClips(clips);
    if (nextScheduledPaths != m_scheduledDecodePaths) {
      const QSet<QString> addedPaths =
          nextScheduledPaths - m_scheduledDecodePaths;
      const QSet<QString> removedPaths =
          m_scheduledDecodePaths - nextScheduledPaths;
      for (const QString &path : removedPaths) {
        removePendingDecodePathLocked(path);
      }
      for (const QString &path : addedPaths) {
        enqueueDecodePathLocked(path, false, false);
      }
      m_scheduledDecodePaths = nextScheduledPaths;
      queueChanged = !addedPaths.isEmpty() || !removedPaths.isEmpty();
    }
    m_timelineClips = clips;
    prioritizeDecodesNearSampleLocked(m_timelineSampleCursor);
  }
  if (queueChanged) {
    m_decodeCondition.notify_one();
  }
}

void AudioEngine::setTimelineTracks(const QVector<TimelineTrack> &tracks) {
  {
    std::lock_guard<std::mutex> lock(m_stateMutex);
    m_timelineTracks = tracks;
  }
  m_mixCondition.notify_one();
}

void AudioEngine::setExportRanges(const QVector<ExportRangeSegment> &ranges) {
  std::lock_guard<std::mutex> lock(m_exportRangesMutex);
  m_exportRanges = ranges;
}

void AudioEngine::setRenderSyncMarkers(
    const QVector<RenderSyncMarker> &markers) {
  std::lock_guard<std::mutex> lock(m_stateMutex);
  m_renderSyncMarkers = markers;
}

void AudioEngine::setTimelineState(
    const QVector<TimelineTrack> &tracks,
    const QVector<TimelineClip> &clips,
    const QVector<ExportRangeSegment> &ranges,
    const QVector<RenderSyncMarker> &markers) {
  {
    // mixLoop takes these locks in this order while copying its context.
    std::lock_guard<std::mutex> stateLock(m_stateMutex);
    std::lock_guard<std::mutex> rangesLock(m_exportRangesMutex);
    ++m_mixGeneration;
    installTimelineStateLocked(tracks, clips, ranges, markers);
  }
  m_decodeCondition.notify_one();
  m_mixCondition.notify_one();
}

void AudioEngine::setTimelineStateAtFrame(
    const QVector<TimelineTrack> &tracks,
    const QVector<TimelineClip> &clips,
    const QVector<ExportRangeSegment> &ranges,
    const QVector<RenderSyncMarker> &markers,
    int64_t frame) {
  {
    std::lock_guard<std::mutex> stateLock(m_stateMutex);
    std::lock_guard<std::mutex> rangesLock(m_exportRangesMutex);
    ++m_mixGeneration;
    pauseOutputStreamForRefillLocked();
    const int64_t sample = timelineFrameToSamples(frame);
    m_timelineSampleCursor = sample;
    m_authoritativeTransportSample.store(sample, std::memory_order_release);
    installTimelineStateLocked(tracks, clips, ranges, markers);
    m_audioClockSample.store(sample, std::memory_order_release);
    m_lastReportedCurrentSample.store(sample, std::memory_order_release);
    m_ringBufferEndSample.store(sample, std::memory_order_release);
    m_ringBuffer.clear();
  }
  m_stateCondition.notify_all();
  m_decodeCondition.notify_one();
  m_mixCondition.notify_all();
}

void AudioEngine::installTimelineStateLocked(
    const QVector<TimelineTrack> &tracks,
    const QVector<TimelineClip> &clips,
    const QVector<ExportRangeSegment> &ranges,
    const QVector<RenderSyncMarker> &markers) {
  const QSet<QString> nextScheduledPaths = scheduledAudioPathsFromClips(clips);
  if (nextScheduledPaths != m_scheduledDecodePaths) {
    const QSet<QString> removedPaths =
        m_scheduledDecodePaths - nextScheduledPaths;
    for (const QString &path : removedPaths) {
      removePendingDecodePathLocked(path);
    }
    m_scheduledDecodePaths = nextScheduledPaths;
  }

  m_timelineTracks = tracks;
  m_timelineClips = clips;
  m_exportRanges = ranges;
  m_renderSyncMarkers = markers;
  // Requeue unresolved paths even when the path set itself did not change;
  // this lets a previously missing external source become available.
  scheduleDecodesLocked(m_timelineClips);
  prioritizeDecodesNearSampleLocked(m_timelineSampleCursor);
}

void AudioEngine::setSpeechFilterFadeSamples(int samples) {
  m_speechFilterFadeSamples.store(qMax(0, samples), std::memory_order_release);
}

void AudioEngine::setSpeechFilterFadeMode(SpeechFilterFadeMode mode) {
  m_speechFilterFadeMode.store(static_cast<int>(mode), std::memory_order_release);
}

void AudioEngine::setSpeechFilterCurveStrength(qreal strength) {
  m_speechFilterCurveStrength.store(qBound<qreal>(0.25, strength, 4.0),
                                    std::memory_order_release);
}

void AudioEngine::setSpeechFilterRangeCrossfadeEnabled(bool enabled) {
  m_speechFilterRangeCrossfadeEnabled.store(enabled, std::memory_order_release);
}

void AudioEngine::setPlaybackWarpMode(PlaybackAudioWarpMode mode) {
  const int newMode = static_cast<int>(mode);
  const int oldMode =
      m_playbackWarpMode.exchange(newMode, std::memory_order_acq_rel);
  if (oldMode == newMode) {
    return;
  }
  // The in-memory cache is indexed by rate for fast callback-side lookup.
  // A rate-equivalent entry produced by another algorithm is not
  // interchangeable with the two-pass speech treatment.
  {
    std::lock_guard<std::mutex> lock(m_stateMutex);
    m_timeStretchAudioCache.clear();
  }
  if (playbackWarpModeUsesTimeStretch(mode) &&
      pitchPreservingTimeStretchActive(
          m_playbackRate.load(std::memory_order_acquire), mode)) {
    enqueueTimeStretchPrecomputeForScheduledPaths();
  }
}

void AudioEngine::setPlaybackRate(qreal rate) {
  const qreal clampedRate = qBound<qreal>(0.1, rate, 3.0);
  const int oldRateKey =
      timeStretchRateKey(m_playbackRate.load(std::memory_order_acquire));
  const int newRateKey = timeStretchRateKey(clampedRate);
  m_playbackRate.store(clampedRate, std::memory_order_release);
  if (oldRateKey == newRateKey) {
    return;
  }
  const PlaybackAudioWarpMode mode = static_cast<PlaybackAudioWarpMode>(
      m_playbackWarpMode.load(std::memory_order_acquire));
  if (playbackWarpModeUsesTimeStretch(mode) &&
      pitchPreservingTimeStretchActive(clampedRate, mode)) {
    enqueueTimeStretchPrecomputeForScheduledPaths();
  }
}

void AudioEngine::setPlaybackDriftRetimeRate(qreal rate) {
  m_playbackDriftRetimeRate.store(qBound<qreal>(0.92, rate, 1.08),
                                  std::memory_order_release);
}

void AudioEngine::setTranscriptNormalizeEnabled(bool enabled) {
  m_transcriptNormalizeEnabled.store(enabled, std::memory_order_release);
}

void AudioEngine::setTranscriptNormalizeRanges(
    const QVector<ExportRangeSegment> &ranges) {
  std::lock_guard<std::mutex> lock(m_transcriptNormalizeRangesMutex);
  m_transcriptNormalizeRanges = ranges;
}

void AudioEngine::setAudioDynamicsSettings(
    const PreviewSurface::AudioDynamicsSettings &settings) {
  m_amplifyEnabled.store(settings.amplifyEnabled, std::memory_order_release);
  m_amplifyDb.store(settings.amplifyDb, std::memory_order_release);
  m_normalizeEnabled.store(settings.normalizeEnabled,
                           std::memory_order_release);
  m_normalizeTargetDb.store(settings.normalizeTargetDb,
                            std::memory_order_release);
  m_selectiveNormalizeEnabled.store(settings.selectiveNormalizeEnabled,
                                    std::memory_order_release);
  m_selectiveNormalizeMinSegmentSeconds.store(
      settings.selectiveNormalizeMinSegmentSeconds, std::memory_order_release);
  m_selectiveNormalizePeakDb.store(settings.selectiveNormalizePeakDb,
                                   std::memory_order_release);
  m_selectiveNormalizePasses.store(settings.selectiveNormalizePasses,
                                   std::memory_order_release);
  m_peakReductionEnabled.store(settings.peakReductionEnabled,
                               std::memory_order_release);
  m_peakThresholdDb.store(settings.peakThresholdDb, std::memory_order_release);
  m_limiterEnabled.store(settings.limiterEnabled, std::memory_order_release);
  m_limiterThresholdDb.store(settings.limiterThresholdDb,
                             std::memory_order_release);
  m_compressorEnabled.store(settings.compressorEnabled,
                            std::memory_order_release);
  m_compressorThresholdDb.store(settings.compressorThresholdDb,
                                std::memory_order_release);
  m_compressorRatio.store(settings.compressorRatio, std::memory_order_release);
  m_softClipEnabled.store(settings.softClipEnabled, std::memory_order_release);
  m_stereoToMonoEnabled.store(settings.stereoToMonoEnabled,
                              std::memory_order_release);
}

void AudioEngine::setBackgroundDecodeSuppressed(bool suppressed) {
  {
    std::lock_guard<std::mutex> lock(m_stateMutex);
    if (m_backgroundDecodeSuppressed == suppressed) {
      return;
    }
    m_backgroundDecodeSuppressed = suppressed;
    if (!suppressed) {
      scheduleDecodesLocked(m_timelineClips);
      prioritizeDecodesNearSampleLocked(m_timelineSampleCursor);
    }
  }
  m_decodeCondition.notify_all();
}

void AudioEngine::setBufferFrames(int frames) {
  // RtAudio may adjust the requested size for the active device, but keeping
  // the request power-of-two makes latency choices predictable across hosts.
  constexpr int kMinBufferFrames = 64;
  constexpr int kMaxBufferFrames = 4096;
  const bool valid = frames >= kMinBufferFrames && frames <= kMaxBufferFrames &&
                     (frames & (frames - 1)) == 0;
  if (!valid) {
    frames = 1024;
  }
  std::lock_guard<std::mutex> lock(m_stateMutex);
  if (!m_initialized) {
    m_periodFrames = frames;
  }
}

int AudioEngine::bufferFrames() const {
  std::lock_guard<std::mutex> lock(m_stateMutex);
  return m_periodFrames;
}

bool AudioEngine::initialize() {
  std::lock_guard<std::mutex> lock(m_stateMutex);
  if (m_initialized) {
    return true;
  }
  const qint64 nowMs = QDateTime::currentMSecsSinceEpoch();
  if (nowMs < m_audioInitBackoffUntilMs) {
    return false;
  }

  m_lastKnownDeviceCount = 0;
  m_lastKnownDefaultOutputValid = false;
  m_lastKnownDefaultOutputId = 0;
  m_lastKnownDefaultOutputName.clear();
  m_lastKnownDefaultOutputChannels = 0;
  m_lastDeviceInfoError.clear();
  m_selectedAudioBackend.clear();
  m_audioBackendSelectionReason.clear();
  m_audioBackendCandidates = {};

  const AudioOutputBackendConfig config{
      m_sampleRate,
      m_channelCount,
      m_periodFrames,
      &AudioEngine::rtAudioCallback,
      this,
  };
  AudioOutputBackendSelection selection = selectBestAudioOutputBackend(config);
  for (const AudioOutputBackendProbe& probe : selection.probes) {
    const AudioOutputBackendCapability& candidate = probe.capability;
    QJsonObject candidateJson{
        {QStringLiteral("id"), QString::fromStdString(candidate.id)},
        {QStringLiteral("label"), QString::fromStdString(candidate.label)},
        {QStringLiteral("preference"), candidate.preference},
        {QStringLiteral("compiled"), candidate.compiled},
        {QStringLiteral("operating_system_supported"),
         candidate.operatingSystemSupported},
        {QStringLiteral("reason"), QString::fromStdString(candidate.reason)},
    };
    candidateJson[QStringLiteral("available")] = probe.available;
    if (!probe.error.empty()) {
      candidateJson[QStringLiteral("probe_error")] =
          QString::fromStdString(probe.error);
    }
    if (probe.available) {
      candidateJson[QStringLiteral("selected")] = true;
      candidateJson[QStringLiteral("device_name")] =
          QString::fromStdString(probe.info.deviceName);
    }
    m_audioBackendCandidates.append(candidateJson);
  }
  m_selectedAudioBackend =
      QString::fromStdString(selection.selectedId);
  m_audioBackendSelectionReason =
      QString::fromStdString(selection.selectionReason);
  m_outputBackend = std::move(selection.backend);

  if (!m_outputBackend) {
    m_lastDeviceInfoError =
        QStringLiteral("No detected audio backend could open an output device");
    if (nowMs - m_lastAudioInitWarningMs >= kAudioInitWarningThrottleMs) {
      qWarning() << m_lastDeviceInfoError;
      m_lastAudioInitWarningMs = nowMs;
    }
    m_audioInitBackoffUntilMs = nowMs + kAudioInitBackoffMs;
    return false;
  }

  const AudioOutputBackendInfo& selectedInfo = selection.info;
  m_lastKnownDeviceCount = selectedInfo.deviceCount;
  m_lastKnownDefaultOutputValid = selectedInfo.defaultDeviceValid;
  m_lastKnownDefaultOutputId = selectedInfo.defaultDeviceId;
  m_lastKnownDefaultOutputName =
      QString::fromStdString(selectedInfo.deviceName);
  m_lastKnownDefaultOutputChannels = selectedInfo.outputChannels;
  m_periodFrames = qMax(
      1, selectedInfo.periodFrames > 0
             ? selectedInfo.periodFrames
             : m_periodFrames);
  const int64_t streamLatencyFrames =
      qMax<int64_t>(0, selectedInfo.latencyFrames);
  m_outputStreamLatencyFramesAtOpen.store(
      streamLatencyFrames, std::memory_order_release);
  m_lastObservedBackendConnectionRevision.store(
      m_outputBackend->connectionRevision(), std::memory_order_release);
  m_outputPrimeTargetSamples.store(
      outputPrimeTargetSamples(m_periodFrames, streamLatencyFrames),
      std::memory_order_release);
  m_outputPrimeCapacitySufficient.store(
      outputPrimeCapacitySufficient(m_periodFrames, streamLatencyFrames),
      std::memory_order_release);

  m_running = true;
  m_decodeWorker = std::thread([this]() { decodeLoop(); });
  m_mixWorker = std::thread([this]() { mixLoop(); });
  m_initialized = true;
  return true;
}

void AudioEngine::shutdown() {
  {
    std::lock_guard<std::mutex> lock(m_stateMutex);
    if (!m_initialized) {
      return;
    }
    ++m_mixGeneration;
    m_running = false;
    m_playing = false;
    m_outputStartPending.store(false, std::memory_order_release);
  }
  m_stateCondition.notify_all();
  m_decodeCondition.notify_all();
  m_mixCondition.notify_all();
  if (m_decodeWorker.joinable()) {
    m_decodeWorker.join();
  }
  if (m_mixWorker.joinable()) {
    m_mixWorker.join();
  }
  if (m_outputBackend) {
    m_outputBackend->shutdown();
    m_outputBackend.reset();
  }
  m_lastKnownDeviceCount = 0;
  m_lastKnownDefaultOutputValid = false;
  m_lastKnownDefaultOutputId = 0;
  m_lastKnownDefaultOutputName.clear();
  m_lastKnownDefaultOutputChannels = 0;
  m_lastDeviceInfoError.clear();
  m_ringBuffer.clear();
  std::lock_guard<std::mutex> lock(m_stateMutex);
  m_initialized = false;
}

void AudioEngine::setMuted(bool muted) {
  std::lock_guard<std::mutex> lock(m_stateMutex);
  m_muted = muted;
}

void AudioEngine::setVolume(qreal volume) {
  std::lock_guard<std::mutex> lock(m_stateMutex);
  m_volume = qBound<qreal>(0.0, volume, 1.0);
}

bool AudioEngine::muted() const {
  std::lock_guard<std::mutex> lock(m_stateMutex);
  return m_muted;
}

int AudioEngine::volumePercent() const {
  std::lock_guard<std::mutex> lock(m_stateMutex);
  return qRound(m_volume * 100.0);
}

void AudioEngine::startAtTimelineSample(int64_t startSample) {
  if (!initialize()) {
    return;
  }
  if (m_playing.load(std::memory_order_acquire)) {
    m_redundantStartCount.fetch_add(1, std::memory_order_relaxed);
    if (m_outputBackend && !m_outputBackend->isRunning()) {
      m_outputStartPending.store(true, std::memory_order_release);
    }
    m_stateCondition.notify_all();
    m_mixCondition.notify_all();
    startOutputStreamIfPrimed();
    return;
  }
  m_startCount.fetch_add(1, std::memory_order_relaxed);
  const int64_t boundedStartSample = qMax<int64_t>(0, startSample);
  m_lastStartFrame.store(samplesToTimelineFrame(boundedStartSample),
                         std::memory_order_release);
  {
    std::lock_guard<std::mutex> lock(m_stateMutex);
    ++m_mixGeneration;
    m_timelineSampleCursor = boundedStartSample;
    m_authoritativeTransportSample.store(m_timelineSampleCursor,
                                         std::memory_order_release);
    m_audioClockSample.store(m_timelineSampleCursor, std::memory_order_release);
    m_lastReportedCurrentSample.store(m_timelineSampleCursor,
                                      std::memory_order_release);
    m_ringBufferEndSample.store(m_timelineSampleCursor,
                                std::memory_order_release);
    m_ringBuffer.clear();
    m_playing = true;
    m_outputStartPending.store(true, std::memory_order_release);
    m_outputPrimeCanRebase = true;
    m_outputPrimeStartedMs.store(QDateTime::currentMSecsSinceEpoch(),
                                 std::memory_order_release);
    scheduleDecodesLocked(m_timelineClips);
    prioritizeDecodesNearSampleLocked(m_timelineSampleCursor);
  }
  m_stateCondition.notify_all();
  m_decodeCondition.notify_one();
  m_mixCondition.notify_all();
}

void AudioEngine::stop() {
  {
    std::lock_guard<std::mutex> lock(m_stateMutex);
    ++m_mixGeneration;
    m_playing = false;
    m_outputStartPending.store(false, std::memory_order_release);
    m_outputPrimeCanRebase = false;
    m_outputPrimeStartedMs.store(0, std::memory_order_release);
  }
  if (m_outputBackend && m_outputBackend->isRunning()) {
    // Fade to zero from the last rendered sample to avoid click/pop on stop.
    const int16_t lastL =
        static_cast<int16_t>(m_lastOutputLeft.load(std::memory_order_acquire));
    const int16_t lastR =
        static_cast<int16_t>(m_lastOutputRight.load(std::memory_order_acquire));
    m_ringBuffer.clear();
    QVector<int16_t> fadeSamples;
    fadeSamples.resize(kShutdownFadeFrames * m_channelCount);
    for (int frame = 0; frame < kShutdownFadeFrames; ++frame) {
      const qreal gain = 1.0 - (static_cast<qreal>(frame + 1) /
                                static_cast<qreal>(kShutdownFadeFrames));
      fadeSamples[frame * m_channelCount] =
          static_cast<int16_t>(qRound(static_cast<qreal>(lastL) * gain));
      fadeSamples[frame * m_channelCount + 1] =
          static_cast<int16_t>(qRound(static_cast<qreal>(lastR) * gain));
    }
    m_ringBuffer.write(fadeSamples.constData(),
                       static_cast<size_t>(fadeSamples.size()));
    const int64_t currentEnd =
        m_ringBufferEndSample.load(std::memory_order_acquire);
    m_ringBufferEndSample.store(currentEnd + kShutdownFadeFrames,
                                std::memory_order_release);
    const int fadeMs =
        qMax(1, static_cast<int>(std::ceil(
                    (1000.0 * static_cast<double>(kShutdownFadeFrames)) /
                    static_cast<double>(m_sampleRate))));
    std::this_thread::sleep_for(std::chrono::milliseconds(fadeMs + 2));
    if (!m_outputBackend->stop(true)) {
      m_lastDeviceInfoError =
          QString::fromStdString(m_outputBackend->lastError());
    }
  }
  m_ringBuffer.clear();
  m_mixCondition.notify_all();
}

void AudioEngine::seekToTimelineSample(int64_t sample) {
  const int64_t boundedSample = qMax<int64_t>(0, sample);
  m_seekCount.fetch_add(1, std::memory_order_relaxed);
  m_lastSeekFrame.store(samplesToTimelineFrame(boundedSample),
                        std::memory_order_release);
  {
    std::lock_guard<std::mutex> lock(m_stateMutex);
    ++m_mixGeneration;
    pauseOutputStreamForRefillLocked();
    m_timelineSampleCursor = boundedSample;
    m_authoritativeTransportSample.store(boundedSample,
                                         std::memory_order_release);
    m_audioClockSample.store(boundedSample, std::memory_order_release);
    m_lastReportedCurrentSample.store(boundedSample,
                                      std::memory_order_release);
    m_ringBufferEndSample.store(boundedSample, std::memory_order_release);
    m_ringBuffer.clear();
    scheduleDecodesLocked(m_timelineClips);
    prioritizeDecodesNearSampleLocked(boundedSample);
  }
  m_stateCondition.notify_all();
  m_decodeCondition.notify_one();
  m_mixCondition.notify_all();
}

bool AudioEngine::hasPlayableAudio() const {
  std::lock_guard<std::mutex> lock(m_stateMutex);
  for (const TimelineClip &clip : m_timelineClips) {
    if (clipAudioPlaybackEnabled(clip) &&
        clip.audioSourceStatus != QStringLiteral("missing") &&
        !clipAudioPathForScheduling(clip).isEmpty()) {
      return true;
    }
  }
  return false;
}

QVector<QString> AudioEngine::scheduledAudioSourcePaths() const {
  std::lock_guard<std::mutex> lock(m_stateMutex);
  QVector<QString> paths;
  paths.reserve(m_scheduledDecodePaths.size());
  for (const QString &key : m_scheduledDecodePaths) {
    paths.push_back(editor::audio::pathFromSourceKey(key));
  }
  std::sort(paths.begin(), paths.end());
  paths.erase(std::unique(paths.begin(), paths.end()), paths.end());
  return paths;
}

void AudioEngine::invalidateAudioSourceCaches() {
  std::lock_guard<std::mutex> sourceCommitLock(
      m_sourceGenerationCommitMutex);
  {
    // Source-generation changes and time-stretch job invalidation are one
    // transaction. A worker cannot observe the new generation and publish a
    // job before the old generation's terminal/progress state is cleared.
    std::scoped_lock lock(m_stateMutex, m_timeStretchGenerationMutex);
    ++m_sourceGeneration;
    ++m_mixGeneration;

    m_audioCache.clear();
    m_timeStretchAudioCache.clear();
    m_timeStretchSidecarEntryCache.clear();
    m_failedDecodePaths.clear();
    m_pendingDecodePaths.clear();
    m_pendingDecodeSet.clear();
    m_fullDecodeRequestedWhileActive.clear();
    m_timeStretchPrecomputeRequestedWhileActive.clear();

    m_timeStretchJobs.clear();
    m_timeStretchFailedJobs.clear();
    m_timeStretchJobAttemptCounts.clear();
    m_timeStretchRetrySuppressedMs.clear();
    m_timeStretchGenerationSourceGeneration = 0;
    m_timeStretchGenerationActive.store(false, std::memory_order_release);
    m_timeStretchGenerationPhase.store(TimeStretchGenerationIdle,
                                       std::memory_order_release);
    m_timeStretchGenerationProgressPermille.store(0,
                                                  std::memory_order_release);
    m_timeStretchGenerationLastSucceeded.store(false,
                                               std::memory_order_release);
    m_timeStretchGenerationLastFinishMs.store(
        QDateTime::currentMSecsSinceEpoch(), std::memory_order_release);
    m_timeStretchGenerationLastError.clear();
    m_timeStretchGenerationLastEndReason =
        QStringLiteral("source_generation_invalidated");

    // Resume from the consumer-visible clock. A chunk mixed from the old
    // source generation is rejected by commitMixedChunk(), and resetting the
    // cursor here prevents that rejected chunk from creating an audible gap.
    const int64_t resumeSample =
        m_audioClockSample.load(std::memory_order_acquire);
    pauseOutputStreamForRefillLocked();
    m_timelineSampleCursor = resumeSample;
    m_ringBufferEndSample.store(resumeSample, std::memory_order_release);
    m_ringBuffer.clear();

    m_audioPlaybackBlocked.store(false, std::memory_order_release);
    m_pitchPreservingAudioBlocked.store(false, std::memory_order_release);
    m_timeStretchPrecomputeBlocked.store(false, std::memory_order_release);
    m_timeStretchReadinessState.store(TimeStretchReadinessIdle,
                                      std::memory_order_release);
    scheduleDecodesLocked(m_timelineClips);
    prioritizeDecodesNearSampleLocked(resumeSample);
  }
  m_decodeCondition.notify_all();
  m_stateCondition.notify_all();
  m_mixCondition.notify_all();
}

bool AudioEngine::warmPlaybackAudio(int64_t startFrame, int timeoutMs) {
  if (!initialize()) {
    return false;
  }
  const int64_t timelineSample = timelineFrameToSamples(startFrame);
  const auto deadline = std::chrono::steady_clock::now() +
                        std::chrono::milliseconds(qMax(1, timeoutMs));
  if (ensureTimeStretchAudioReadyForTimelineSample(timelineSample)) {
    m_audioPlaybackBlocked.store(false, std::memory_order_release);
    m_pitchPreservingAudioBlocked.store(false, std::memory_order_release);
    m_timeStretchPrecomputeBlocked.store(false, std::memory_order_release);
    return true;
  }
  if (m_timeStretchReadinessState.load(std::memory_order_acquire) ==
      TimeStretchReadinessMissing) {
    return false;
  }

  std::unique_lock<std::mutex> lock(m_stateMutex);
  while (m_running.load(std::memory_order_acquire)) {
    if (audioReadyForTimelineSampleLocked(timelineSample)) {
      m_audioPlaybackBlocked.store(false, std::memory_order_release);
      m_timeStretchReadinessState.store(TimeStretchReadinessReadyInMemory,
                                        std::memory_order_release);
      return true;
    }
    lock.unlock();
    if (ensureTimeStretchAudioReadyForTimelineSample(timelineSample)) {
      m_audioPlaybackBlocked.store(false, std::memory_order_release);
      m_pitchPreservingAudioBlocked.store(false, std::memory_order_release);
      m_timeStretchPrecomputeBlocked.store(false, std::memory_order_release);
      return true;
    }
    if (m_timeStretchReadinessState.load(std::memory_order_acquire) ==
        TimeStretchReadinessMissing) {
      return false;
    }
    lock.lock();
    if (m_stateCondition.wait_until(lock, deadline) ==
        std::cv_status::timeout) {
      m_audioPlaybackBlocked.store(true, std::memory_order_release);
      return audioReadyForTimelineSampleLocked(timelineSample);
    }
  }
  return false;
}

bool AudioEngine::playbackAudioWarmupPermanentlyFailed(
    int64_t startFrame) const {
  const int64_t timelineSample = timelineFrameToSamples(startFrame);
  QString audioPath;
  qreal playbackRate = 1.0;
  PlaybackAudioWarpMode warpMode = PlaybackAudioWarpMode::Disabled;
  uint64_t sourceGeneration = 0;
  {
    std::lock_guard<std::mutex> stateLock(m_stateMutex);
    if (!clipAndSourceSampleAtTimelineSampleLocked(
            timelineSample, nullptr, &audioPath, nullptr)) {
      return false;
    }
    sourceGeneration = m_sourceGeneration;
    if (m_failedDecodePaths.contains(audioPath)) {
      return true;
    }
    playbackRate =
        qBound<qreal>(0.1, m_playbackRate.load(std::memory_order_acquire), 3.0);
    warpMode = static_cast<PlaybackAudioWarpMode>(
        m_playbackWarpMode.load(std::memory_order_acquire));
  }

  if (!playbackWarpModeUsesTimeStretch(warpMode) ||
      !pitchPreservingTimeStretchActive(playbackRate, warpMode) ||
      m_timeStretchReadinessState.load(std::memory_order_acquire) !=
          TimeStretchReadinessMissing) {
    return false;
  }

  const int speedKey = precomputedTimeStretchSpeedKey(playbackRate, warpMode);
  const QString jobKey = timeStretchJobKey(audioPath, speedKey);
  std::lock_guard<std::mutex> generationLock(m_timeStretchGenerationMutex);
  const auto jobIt = m_timeStretchJobs.constFind(jobKey);
  return jobIt != m_timeStretchJobs.constEnd() &&
         jobIt->sourceGeneration == sourceGeneration &&
         jobIt->state == TimeStretchJobFailed;
}

bool AudioEngine::playbackAudioReadyForFrame(int64_t startFrame) const {
  const int64_t timelineSample = timelineFrameToSamples(startFrame);
  std::lock_guard<std::mutex> lock(m_stateMutex);
  return audioReadyForTimelineSampleLocked(timelineSample);
}

bool AudioEngine::playbackAudioBlocked() const {
  return m_audioPlaybackBlocked.load(std::memory_order_acquire);
}

bool AudioEngine::pitchPreservingAudioBlocked() const {
  return m_pitchPreservingAudioBlocked.load(std::memory_order_acquire);
}

qint64 AudioEngine::timeStretchCacheMissCount() const {
  return m_timeStretchCacheMissCount.load(std::memory_order_acquire);
}

int AudioEngine::underrunCount() const {
  return m_underrunCount.load(std::memory_order_acquire);
}

bool AudioEngine::playbackAudioNeedsRetimingForFrame(int64_t startFrame) const {
  const int64_t timelineSample = timelineFrameToSamples(startFrame);
  QString audioPath;
  qreal playbackRate = 1.0;
  PlaybackAudioWarpMode warpMode = PlaybackAudioWarpMode::Disabled;
  {
    std::lock_guard<std::mutex> lock(m_stateMutex);
    const TimelineClip *clip = nullptr;
    if (!clipAndSourceSampleAtTimelineSampleLocked(timelineSample, &clip,
                                                   &audioPath, nullptr)) {
      return false;
    }
    playbackRate =
        qBound<qreal>(0.1, m_playbackRate.load(std::memory_order_acquire), 3.0);
    warpMode = static_cast<PlaybackAudioWarpMode>(
        m_playbackWarpMode.load(std::memory_order_acquire));
    if (!playbackWarpModeUsesTimeStretch(warpMode) ||
        !pitchPreservingTimeStretchActive(playbackRate, warpMode) ||
        audioReadyForTimelineSampleLocked(timelineSample)) {
      return false;
    }
  }

  const int sidecarSpeedKey =
      precomputedTimeStretchSpeedKey(playbackRate, warpMode);
  if (sidecarSpeedKey <= 1) {
    return true;
  }
  if (timeStretchJobRecentlyFailed(audioPath, sidecarSpeedKey)) {
    return false;
  }

  AudioTimeStretchSidecarMetadata metadata;
  return !readAudioTimeStretchSidecarMetadata(audioPath, sidecarSpeedKey,
                                              &metadata) ||
         !metadata.valid || !metadata.fullyDecoded;
}

bool AudioEngine::audioClockAvailable() const {
  std::lock_guard<std::mutex> lock(m_stateMutex);
  return m_initialized && m_outputBackend && m_outputBackend->isRunning();
}

bool AudioEngine::audioOutputUnavailableForPlayback() const {
  bool initialized = false;
  bool playing = false;
  bool hasPlayable = false;
  {
    std::lock_guard<std::mutex> lock(m_stateMutex);
    initialized = m_initialized;
    playing = m_playing.load(std::memory_order_acquire);
    for (const TimelineClip &clip : m_timelineClips) {
      if (clipAudioPlaybackEnabled(clip)) {
        hasPlayable = true;
        break;
      }
    }
  }
  if (!hasPlayable) {
    return false;
  }
  if (!initialized || !m_outputBackend || !m_outputBackend->isOpen()) {
    return true;
  }
  return playing &&
         !m_outputStartPending.load(std::memory_order_acquire) &&
         !m_outputBackend->isRunning();
}

QString AudioEngine::audioOutputStatusText() const {
  bool initialized = false;
  bool playing = false;
  bool hasPlayable = false;
  qint64 deviceCount = 0;
  bool defaultOutputValid = false;
  QString deviceInfoError;
  {
    std::lock_guard<std::mutex> lock(m_stateMutex);
    initialized = m_initialized;
    playing = m_playing.load(std::memory_order_acquire);
    deviceCount = m_lastKnownDeviceCount;
    defaultOutputValid = m_lastKnownDefaultOutputValid;
    deviceInfoError = m_lastDeviceInfoError;
    for (const TimelineClip &clip : m_timelineClips) {
      if (clipAudioPlaybackEnabled(clip)) {
        hasPlayable = true;
        break;
      }
    }
  }
  if (!hasPlayable) {
    return QString();
  }
  if (!initialized || !m_outputBackend) {
    if (deviceCount == 0) {
      return QStringLiteral("Audio output unavailable: no output device");
    }
    if (!deviceInfoError.isEmpty()) {
      return QStringLiteral("Audio output unavailable: %1")
          .arg(deviceInfoError);
    }
    return QStringLiteral("Audio output unavailable: device initialization failed");
  }
  if (!m_outputBackend->isOpen()) {
    const QString backendError =
        QString::fromStdString(m_outputBackend->lastError());
    if (!backendError.isEmpty()) {
      return QStringLiteral("Audio output reconnecting: %1")
          .arg(backendError);
    }
    return QStringLiteral("Audio output unavailable: stream is not open");
  }
  if (playing &&
      !m_outputStartPending.load(std::memory_order_acquire) &&
      !m_outputBackend->isRunning()) {
    if (!deviceInfoError.isEmpty()) {
      return QStringLiteral("Audio output unavailable: %1")
          .arg(deviceInfoError);
    }
    if (!defaultOutputValid) {
      return QStringLiteral("Audio output unavailable: default output device is invalid");
    }
    return QStringLiteral("Audio output unavailable: stream is stopped");
  }
  return QString();
}

bool AudioEngine::playbackStarted() const {
  return m_playing.load(std::memory_order_acquire);
}
