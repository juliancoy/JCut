#include "audio_engine.h"
#include "audio_dynamics_core.h"
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

#include <RtAudio.h>

namespace {

constexpr qint64 kTimeStretchFailedJobRetryDelayMs = 10 * 60 * 1000;

bool anyAudioSolo(const QVector<TimelineClip> &clips,
                  const QVector<TimelineTrack> &tracks) {
  for (const TimelineClip &clip : clips) {
    if (clipAudioPlaybackEnabled(clip) && clip.audioSolo) {
      return true;
    }
  }
  for (const TimelineTrack &track : tracks) {
    if (track.audioSolo) {
      return true;
    }
  }
  return false;
}

float mixerGainForClip(const TimelineClip &clip,
                       const QVector<TimelineTrack> &tracks,
                       bool soloActive) {
  if (!clipAudioPlaybackEnabled(clip)) {
    return 0.0f;
  }
  float gain = static_cast<float>(qBound<qreal>(0.0, clip.audioGain, 4.0));
  bool clipOrTrackSolo = clip.audioSolo;
  if (clip.trackIndex >= 0 && clip.trackIndex < tracks.size()) {
    const TimelineTrack &track = tracks.at(clip.trackIndex);
    if (!track.audioEnabled || track.audioMuted) {
      return 0.0f;
    }
    gain *= static_cast<float>(qBound<qreal>(0.0, track.audioGain, 4.0));
    clipOrTrackSolo = clipOrTrackSolo || track.audioSolo;
  }
  if (soloActive && !clipOrTrackSolo) {
    return 0.0f;
  }
  return gain;
}

} // namespace

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
    const int64_t sample = timelineFrameToSamples(frame);
    m_timelineSampleCursor = sample;
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

  bool created = false;
#if defined(Q_OS_LINUX)
  try {
    m_rtaudio =
        std::make_unique<rt::audio::RtAudio>(rt::audio::RtAudio::LINUX_ALSA);
    created = true;
  } catch (const std::exception &e) {
    if (nowMs - m_lastAudioInitWarningMs >= kAudioInitWarningThrottleMs) {
      qWarning() << "RtAudio ALSA creation failed, falling back to default API:"
                 << e.what();
      m_lastAudioInitWarningMs = nowMs;
    }
  }
#endif
  if (!created) {
    try {
      m_rtaudio = std::make_unique<rt::audio::RtAudio>();
      created = true;
    } catch (const std::exception &e) {
      if (nowMs - m_lastAudioInitWarningMs >= kAudioInitWarningThrottleMs) {
        qWarning() << "RtAudio creation failed:" << e.what();
        m_lastAudioInitWarningMs = nowMs;
      }
      m_audioInitBackoffUntilMs = nowMs + kAudioInitBackoffMs;
      return false;
    }
  }
  m_rtaudio->showWarnings(false);

  m_lastKnownDeviceCount = 0;
  m_lastKnownDefaultOutputValid = false;
  m_lastKnownDefaultOutputId = 0;
  m_lastKnownDefaultOutputName.clear();
  m_lastKnownDefaultOutputChannels = 0;
  m_lastDeviceInfoError.clear();

  unsigned int deviceCount = 0;
  try {
    deviceCount = m_rtaudio->getDeviceCount();
    m_lastKnownDeviceCount = static_cast<qint64>(deviceCount);
  } catch (const std::exception &e) {
    m_lastDeviceInfoError = QString::fromUtf8(e.what());
    if (nowMs - m_lastAudioInitWarningMs >= kAudioInitWarningThrottleMs) {
      qWarning() << "RtAudio getDeviceCount failed:" << m_lastDeviceInfoError;
      m_lastAudioInitWarningMs = nowMs;
    }
    m_rtaudio.reset();
    m_audioInitBackoffUntilMs = nowMs + kAudioInitBackoffMs;
    return false;
  }

  if (deviceCount == 0) {
    if (nowMs - m_lastAudioInitWarningMs >= kAudioInitWarningThrottleMs) {
      qWarning() << "No audio output devices found";
      m_lastAudioInitWarningMs = nowMs;
    }
    m_rtaudio.reset();
    m_audioInitBackoffUntilMs = nowMs + kAudioInitBackoffMs;
    return false;
  }

  rt::audio::RtAudio::StreamParameters params;
  try {
    params.deviceId = m_rtaudio->getDefaultOutputDevice();
    m_lastKnownDefaultOutputId = params.deviceId;
    if (params.deviceId < deviceCount) {
      const auto info = m_rtaudio->getDeviceInfo(params.deviceId);
      m_lastKnownDefaultOutputValid = true;
      m_lastKnownDefaultOutputName = QString::fromStdString(info.name);
      m_lastKnownDefaultOutputChannels = info.outputChannels;
    }
  } catch (const std::exception &e) {
    m_lastDeviceInfoError = QString::fromUtf8(e.what());
    if (nowMs - m_lastAudioInitWarningMs >= kAudioInitWarningThrottleMs) {
      qWarning() << "RtAudio default output query failed:"
                 << m_lastDeviceInfoError;
      m_lastAudioInitWarningMs = nowMs;
    }
  }
  params.nChannels = m_channelCount;

  unsigned int bufferFrames = m_periodFrames;
  auto err = m_rtaudio->openStream(&params, nullptr, rt::audio::RTAUDIO_SINT16,
                                   m_sampleRate, &bufferFrames,
                                   &AudioEngine::rtAudioCallback, this);
  if (err != rt::audio::RTAUDIO_NO_ERROR) {
    if (nowMs - m_lastAudioInitWarningMs >= kAudioInitWarningThrottleMs) {
      qWarning() << "RtAudio openStream failed:"
                 << QString::fromStdString(m_rtaudio->getErrorText());
      m_lastAudioInitWarningMs = nowMs;
    }
    m_rtaudio.reset();
    m_audioInitBackoffUntilMs = nowMs + kAudioInitBackoffMs;
    return false;
  }

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
  if (m_rtaudio) {
    if (m_rtaudio->isStreamRunning()) {
      m_rtaudio->stopStream();
    }
    if (m_rtaudio->isStreamOpen()) {
      m_rtaudio->closeStream();
    }
    m_rtaudio.reset();
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

void AudioEngine::start(int64_t startFrame) {
  if (!initialize()) {
    return;
  }
  if (m_playing.load(std::memory_order_acquire)) {
    m_redundantStartCount.fetch_add(1, std::memory_order_relaxed);
    if (m_rtaudio && !m_rtaudio->isStreamRunning()) {
      m_rtaudio->startStream();
    }
    m_stateCondition.notify_all();
    m_mixCondition.notify_all();
    return;
  }
  m_startCount.fetch_add(1, std::memory_order_relaxed);
  m_lastStartFrame.store(startFrame, std::memory_order_release);
  {
    std::lock_guard<std::mutex> lock(m_stateMutex);
    ++m_mixGeneration;
    m_timelineSampleCursor = timelineFrameToSamples(startFrame);
    m_audioClockSample.store(m_timelineSampleCursor, std::memory_order_release);
    m_lastReportedCurrentSample.store(m_timelineSampleCursor,
                                      std::memory_order_release);
    m_ringBufferEndSample.store(m_timelineSampleCursor,
                                std::memory_order_release);
    m_ringBuffer.clear();
    m_playing = true;
    scheduleDecodesLocked(m_timelineClips);
    prioritizeDecodesNearSampleLocked(m_timelineSampleCursor);
  }
  m_stateCondition.notify_all();
  m_decodeCondition.notify_one();
  m_mixCondition.notify_all();
  if (m_rtaudio && !m_rtaudio->isStreamRunning()) {
    m_rtaudio->startStream();
  }
}

void AudioEngine::stop() {
  {
    std::lock_guard<std::mutex> lock(m_stateMutex);
    ++m_mixGeneration;
    m_playing = false;
  }
  if (m_rtaudio && m_rtaudio->isStreamRunning()) {
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
    m_rtaudio->stopStream();
  }
  m_ringBuffer.clear();
  m_mixCondition.notify_all();
}

void AudioEngine::seek(int64_t frame) {
  m_seekCount.fetch_add(1, std::memory_order_relaxed);
  m_lastSeekFrame.store(frame, std::memory_order_release);
  {
    std::lock_guard<std::mutex> lock(m_stateMutex);
    ++m_mixGeneration;
    const int64_t sample = timelineFrameToSamples(frame);
    m_timelineSampleCursor = sample;
    m_audioClockSample.store(sample, std::memory_order_release);
    m_lastReportedCurrentSample.store(sample, std::memory_order_release);
    m_ringBufferEndSample.store(sample, std::memory_order_release);
    m_ringBuffer.clear();
    scheduleDecodesLocked(m_timelineClips);
    prioritizeDecodesNearSampleLocked(sample);
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
  return m_initialized && m_rtaudio && m_rtaudio->isStreamRunning();
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
  if (!initialized || !m_rtaudio || !m_rtaudio->isStreamOpen()) {
    return true;
  }
  return playing && !m_rtaudio->isStreamRunning();
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
  if (!initialized || !m_rtaudio) {
    if (deviceCount == 0) {
      return QStringLiteral("Audio output unavailable: no output device");
    }
    if (!deviceInfoError.isEmpty()) {
      return QStringLiteral("Audio output unavailable: %1")
          .arg(deviceInfoError);
    }
    return QStringLiteral("Audio output unavailable: device initialization failed");
  }
  if (!m_rtaudio->isStreamOpen()) {
    return QStringLiteral("Audio output unavailable: stream is not open");
  }
  if (playing && !m_rtaudio->isStreamRunning()) {
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

QJsonObject AudioEngine::profilingSnapshot() const {
  QJsonObject snapshot;
  const auto silentReasonToString = [](int reason) -> QString {
    switch (reason) {
    case 1:
      return QStringLiteral("muted_or_volume_zero");
    case 2:
      return QStringLiteral("no_prepared_clips");
    case 3:
      return QStringLiteral("waiting_for_playable_audio");
    case 4:
      return QStringLiteral("no_active_clip_in_chunk");
    case 5:
      return QStringLiteral("input_out_of_range");
    case 6:
      return QStringLiteral("speech_gain_zero");
    case 7:
      return QStringLiteral("clip_gain_zero");
    case 8:
      return QStringLiteral("source_samples_zero");
    case 9:
      return QStringLiteral("output_below_threshold");
    default:
      return QStringLiteral("none");
    }
  };
  const int64_t ringBufferSamplesAvailable =
      static_cast<int64_t>(m_ringBuffer.available());
  const int64_t ringBufferFramesAvailable =
      ringBufferSamplesAvailable / qMax(1, m_channelCount);
  const int64_t ringBufferEndSample =
      m_ringBufferEndSample.load(std::memory_order_acquire);
  const int64_t audioClockSample =
      m_audioClockSample.load(std::memory_order_acquire);
  const int64_t currentSampleValue = currentSample();
  const qreal playbackRate =
      qBound<qreal>(0.1, m_playbackRate.load(std::memory_order_acquire), 3.0);
  const qreal driftRetimeRate =
      qBound<qreal>(0.92,
                    m_playbackDriftRetimeRate.load(std::memory_order_acquire),
                    1.08);
  snapshot[QStringLiteral("initialized")] = m_initialized;
  snapshot[QStringLiteral("running")] =
      m_running.load(std::memory_order_acquire);
  snapshot[QStringLiteral("playing")] =
      m_playing.load(std::memory_order_acquire);
  snapshot[QStringLiteral("has_playable_audio")] = hasPlayableAudio();
  snapshot[QStringLiteral("audio_clock_available")] = audioClockAvailable();
  snapshot[QStringLiteral("current_sample")] =
      static_cast<qint64>(currentSampleValue);
  snapshot[QStringLiteral("current_frame")] =
      static_cast<qint64>(currentFrame());
  snapshot[QStringLiteral("ring_buffer_samples_available")] =
      static_cast<qint64>(ringBufferSamplesAvailable);
  snapshot[QStringLiteral("ring_buffer_frames_available")] =
      static_cast<qint64>(ringBufferFramesAvailable);
  snapshot[QStringLiteral("ring_buffer_ms_available")] = static_cast<qint64>(
      (ringBufferFramesAvailable * 1000) / qMax(1, m_sampleRate));
  snapshot[QStringLiteral("ring_buffer_end_sample")] =
      static_cast<qint64>(ringBufferEndSample);
  snapshot[QStringLiteral("ring_buffer_end_frame")] =
      static_cast<qint64>(samplesToTimelineFrame(ringBufferEndSample));
  snapshot[QStringLiteral("buffered_timeline_samples")] = static_cast<qint64>(
      qMax<int64_t>(0, ringBufferEndSample - currentSampleValue));
  snapshot[QStringLiteral("buffered_timeline_frames")] = static_cast<qint64>(
      qMax<int64_t>(0, samplesToTimelineFrame(ringBufferEndSample) -
                           samplesToTimelineFrame(currentSampleValue)));
  snapshot[QStringLiteral("audio_clock_sample")] =
      static_cast<qint64>(audioClockSample);
  snapshot[QStringLiteral("audio_clock_frame")] =
      static_cast<qint64>(samplesToTimelineFrame(audioClockSample));
  snapshot[QStringLiteral("timeline_sample_cursor")] =
      static_cast<qint64>(m_timelineSampleCursor);
  snapshot[QStringLiteral("timeline_cursor_frame")] =
      static_cast<qint64>(samplesToTimelineFrame(m_timelineSampleCursor));
  snapshot[QStringLiteral("underrun_count")] =
      m_underrunCount.load(std::memory_order_acquire);
  snapshot[QStringLiteral("last_callback_requested_samples")] =
      static_cast<qint64>(
          m_lastCallbackRequestedSamples.load(std::memory_order_acquire));
  snapshot[QStringLiteral("last_callback_read_samples")] = static_cast<qint64>(
      m_lastCallbackReadSamples.load(std::memory_order_acquire));
  snapshot[QStringLiteral("last_callback_underrun_samples")] =
      static_cast<qint64>(
          m_lastCallbackUnderrunSamples.load(std::memory_order_acquire));
  snapshot[QStringLiteral("last_output_left")] =
      m_lastOutputLeft.load(std::memory_order_acquire);
  snapshot[QStringLiteral("last_output_right")] =
      m_lastOutputRight.load(std::memory_order_acquire);
  snapshot[QStringLiteral("sample_rate")] = m_sampleRate;
  snapshot[QStringLiteral("channel_count")] = m_channelCount;
  snapshot[QStringLiteral("period_frames")] = m_periodFrames;
  snapshot[QStringLiteral("playback_rate")] = playbackRate;
  snapshot[QStringLiteral("playback_drift_retime_rate")] = driftRetimeRate;
  snapshot[QStringLiteral("effective_playback_timeline_rate")] =
      playbackRate * driftRetimeRate;
  snapshot[QStringLiteral("playback_warp_mode")] =
      playbackAudioWarpModeToString(static_cast<PlaybackAudioWarpMode>(
          m_playbackWarpMode.load(std::memory_order_acquire)));
  snapshot[QStringLiteral("time_stretch_cache_miss_count")] =
      static_cast<qint64>(
          m_timeStretchCacheMissCount.load(std::memory_order_acquire));
  snapshot[QStringLiteral("muted")] = muted();
  snapshot[QStringLiteral("volume_percent")] = volumePercent();
  snapshot[QStringLiteral("last_mix_prepared_clip_count")] =
      m_lastMixPreparedClipCount.load(std::memory_order_acquire);
  snapshot[QStringLiteral("last_mix_cache_hit_count")] =
      m_lastMixCacheHitCount.load(std::memory_order_acquire);
  snapshot[QStringLiteral("last_mix_cache_miss_count")] =
      m_lastMixCacheMissCount.load(std::memory_order_acquire);
  snapshot[QStringLiteral("last_mix_invalid_audio_count")] =
      m_lastMixInvalidAudioCount.load(std::memory_order_acquire);
  snapshot[QStringLiteral("last_mix_peak_per_mille")] =
      m_lastMixPeakPermille.load(std::memory_order_acquire);
  snapshot[QStringLiteral("last_mix_rms_per_mille")] =
      m_lastMixRmsPermille.load(std::memory_order_acquire);
  snapshot[QStringLiteral("last_mix_nonzero_sample_count")] =
      m_lastMixNonzeroSampleCount.load(std::memory_order_acquire);
  snapshot[QStringLiteral("last_mix_chunk_start_sample")] = static_cast<qint64>(
      m_lastMixChunkStartSample.load(std::memory_order_acquire));
  snapshot[QStringLiteral("last_mix_chunk_end_sample")] = static_cast<qint64>(
      m_lastMixChunkEndSample.load(std::memory_order_acquire));
  snapshot[QStringLiteral("last_mix_frames_with_active_clip")] =
      m_lastMixFramesWithActiveClip.load(std::memory_order_acquire);
  snapshot[QStringLiteral("last_mix_frames_input_out_of_range")] =
      m_lastMixFramesInputOutOfRange.load(std::memory_order_acquire);
  snapshot[QStringLiteral("last_mix_frames_speech_gain_zero")] =
      m_lastMixFramesSpeechGainZero.load(std::memory_order_acquire);
  snapshot[QStringLiteral("last_mix_frames_clip_gain_zero")] =
      m_lastMixFramesClipGainZero.load(std::memory_order_acquire);
  snapshot[QStringLiteral("last_mix_frames_source_nonzero")] =
      m_lastMixFramesSourceNonzero.load(std::memory_order_acquire);
  snapshot[QStringLiteral("last_mix_frames_output_nonzero")] =
      m_lastMixFramesOutputNonzero.load(std::memory_order_acquire);
  snapshot[QStringLiteral("last_mix_source_peak_per_mille")] =
      m_lastMixSourcePeakPermille.load(std::memory_order_acquire);
  snapshot[QStringLiteral("last_mix_primary_gain_peak_per_mille")] =
      m_lastMixPrimaryGainPeakPermille.load(std::memory_order_acquire);
  snapshot[QStringLiteral("last_mix_out_of_range_timeline_sample")] =
      static_cast<qint64>(
          m_lastMixOutOfRangeTimelineSample.load(std::memory_order_acquire));
  snapshot[QStringLiteral("last_mix_out_of_range_source_sample")] =
      static_cast<qint64>(
          m_lastMixOutOfRangeSourceSample.load(std::memory_order_acquire));
  snapshot[QStringLiteral("last_mix_out_of_range_normalized_sample")] =
      static_cast<qint64>(
          m_lastMixOutOfRangeNormalizedSample.load(std::memory_order_acquire));
  snapshot[QStringLiteral("last_mix_out_of_range_audio_start_sample")] =
      static_cast<qint64>(
          m_lastMixOutOfRangeAudioStartSample.load(std::memory_order_acquire));
  snapshot[QStringLiteral("last_mix_out_of_range_audio_end_sample")] =
      static_cast<qint64>(
          m_lastMixOutOfRangeAudioEndSample.load(std::memory_order_acquire));
  snapshot[QStringLiteral("last_mix_time_stretch_speed_per_mille")] =
      m_lastMixTimeStretchSpeedPermille.load(std::memory_order_acquire);
  snapshot[QStringLiteral("last_mix_first_clip_start_sample")] =
      static_cast<qint64>(
          m_lastMixFirstClipStartSample.load(std::memory_order_acquire));
  snapshot[QStringLiteral("last_mix_first_clip_end_sample")] =
      static_cast<qint64>(
          m_lastMixFirstClipEndSample.load(std::memory_order_acquire));
  snapshot[QStringLiteral("last_mix_first_audio_start_sample")] =
      static_cast<qint64>(
          m_lastMixFirstAudioStartSample.load(std::memory_order_acquire));
  snapshot[QStringLiteral("last_mix_first_audio_frame_count")] =
      static_cast<qint64>(
          m_lastMixFirstAudioFrameCount.load(std::memory_order_acquire));
  snapshot[QStringLiteral("last_mix_first_local_sample_at_chunk_start")] =
      static_cast<qint64>(m_lastMixFirstLocalSampleAtChunkStart.load(
          std::memory_order_acquire));
  snapshot[QStringLiteral("last_mix_silence_reason")] = silentReasonToString(
      m_lastMixSilentReason.load(std::memory_order_acquire));
  snapshot[QStringLiteral("last_mix_starved_clip_count")] =
      m_lastMixStarvedClipCount.load(std::memory_order_acquire);
  snapshot[QStringLiteral("mix_degraded_chunk_count")] = static_cast<qint64>(
      m_mixDegradedChunkCount.load(std::memory_order_acquire));
  const TimeStretchProgressSnapshot timeStretchProgress =
      timeStretchProgressSnapshot();
  snapshot[QStringLiteral("time_stretch_progress_visible")] =
      timeStretchProgress.visible;
  snapshot[QStringLiteral("time_stretch_progress_current_path")] =
      timeStretchProgress.currentPath;
  snapshot[QStringLiteral("time_stretch_progress_phase")] =
      timeStretchProgress.phase;
  snapshot[QStringLiteral("time_stretch_progress_total_clips")] =
      timeStretchProgress.totalClips;
  snapshot[QStringLiteral("time_stretch_progress_completed_clips")] =
      timeStretchProgress.completedClips;
  snapshot[QStringLiteral("time_stretch_progress_remaining_clips")] =
      timeStretchProgress.remainingClips;
  snapshot[QStringLiteral("time_stretch_progress_current")] =
      timeStretchProgress.currentProgress;
  snapshot[QStringLiteral("time_stretch_progress_overall")] =
      timeStretchProgress.overallProgress;
  qint64 timeStretchGenerationStartedMs = 0;
  qint64 timeStretchGenerationLastFinishMs = 0;
  bool timeStretchGenerationActive = false;
  QString timeStretchGenerationPhase;
  int timeStretchGenerationSpeedKey = 0;
  int64_t timeStretchGenerationSourceFrames = 0;
  int64_t timeStretchGenerationOutputFrames = 0;
  double timeStretchGenerationProgress = 0.0;
  bool timeStretchGenerationLastSucceeded = false;
  int timeStretchGenerationAttempt = 0;
  qint64 timeStretchGenerationRetrySuppressedMs = 0;
  QString timeStretchGenerationPath;
  QString timeStretchGenerationSidecarPath;
  QString timeStretchGenerationLastError;
  QString timeStretchGenerationLastEndReason;
  qint64 timeStretchJobFailedMs = 0;
  qint64 timeStretchJobRetrySuppressedMs = 0;
  {
    std::lock_guard<std::mutex> generationLock(m_timeStretchGenerationMutex);
    timeStretchGenerationStartedMs =
        m_timeStretchGenerationStartedMs.load(std::memory_order_acquire);
    timeStretchGenerationLastFinishMs =
        m_timeStretchGenerationLastFinishMs.load(std::memory_order_acquire);
    timeStretchGenerationActive =
        m_timeStretchGenerationActive.load(std::memory_order_acquire);
    timeStretchGenerationPhase = timeStretchGenerationPhaseString(
        m_timeStretchGenerationPhase.load(std::memory_order_acquire));
    timeStretchGenerationSpeedKey =
        m_timeStretchGenerationSpeedKey.load(std::memory_order_acquire);
    timeStretchGenerationSourceFrames =
        m_timeStretchGenerationSourceFrames.load(std::memory_order_acquire);
    timeStretchGenerationOutputFrames =
        m_timeStretchGenerationOutputFrames.load(std::memory_order_acquire);
    timeStretchGenerationProgress =
        static_cast<double>(m_timeStretchGenerationProgressPermille.load(
            std::memory_order_acquire)) /
        1000.0;
    timeStretchGenerationLastSucceeded =
        m_timeStretchGenerationLastSucceeded.load(std::memory_order_acquire);
    timeStretchGenerationAttempt =
        m_timeStretchGenerationAttempt.load(std::memory_order_acquire);
    timeStretchGenerationRetrySuppressedMs =
        m_timeStretchGenerationRetrySuppressedMs.load(
            std::memory_order_acquire);
    timeStretchGenerationPath = m_timeStretchGenerationPath;
    timeStretchGenerationSidecarPath = m_timeStretchGenerationSidecarPath;
    timeStretchGenerationLastError = m_timeStretchGenerationLastError;
    timeStretchGenerationLastEndReason = m_timeStretchGenerationLastEndReason;
    const QString generationKey = timeStretchJobKey(
        timeStretchGenerationPath, timeStretchGenerationSpeedKey);
    timeStretchJobFailedMs = m_timeStretchFailedJobs.value(generationKey, 0);
    timeStretchJobRetrySuppressedMs =
        m_timeStretchRetrySuppressedMs.value(generationKey, 0);
  }
  QFileInfo timeStretchSidecarInfo(timeStretchGenerationSidecarPath);
  AudioTimeStretchSidecarMetadata timeStretchSidecarMetadata;
  const bool timeStretchSidecarMetadataReadable =
      !timeStretchGenerationPath.isEmpty() &&
      timeStretchGenerationSpeedKey > 1 &&
      readAudioTimeStretchSidecarMetadata(timeStretchGenerationPath,
                                          timeStretchGenerationSpeedKey,
                                          &timeStretchSidecarMetadata);
  const QString audioOutputStatus = audioOutputStatusText();

  std::lock_guard<std::mutex> lock(m_stateMutex);
  snapshot[QStringLiteral("decoded_audio_path_count")] =
      static_cast<qint64>(m_audioCache.size());
  snapshot[QStringLiteral("time_stretch_cache_path_count")] =
      static_cast<qint64>(m_timeStretchAudioCache.size());
  qint64 timeStretchEntryCount = 0;
  for (auto it = m_timeStretchAudioCache.cbegin();
       it != m_timeStretchAudioCache.cend(); ++it) {
    for (auto speedIt = it.value().cbegin(); speedIt != it.value().cend();
         ++speedIt) {
      timeStretchEntryCount += speedIt.value().size();
    }
  }
  snapshot[QStringLiteral("time_stretch_cache_entry_count")] =
      timeStretchEntryCount;
  snapshot[QStringLiteral("background_decode_suppressed")] =
      m_backgroundDecodeSuppressed;
  snapshot[QStringLiteral("scheduled_decode_path_count")] =
      static_cast<qint64>(m_scheduledDecodePaths.size());
  snapshot[QStringLiteral("pending_decode_path_count")] =
      static_cast<qint64>(m_pendingDecodePaths.size());
  snapshot[QStringLiteral("active_decode_path_count")] =
      static_cast<qint64>(m_activeDecodeFullDecode.size());
  snapshot[QStringLiteral("time_stretch_precompute_request_count")] =
      m_timeStretchPrecomputeRequestCount;
  snapshot[QStringLiteral("last_mix_starved_clip_path")] =
      m_lastMixStarvedClipPath;
  snapshot[QStringLiteral("last_time_stretch_cache_miss_path")] =
      m_lastTimeStretchCacheMissPath;
  const int lastTimeStretchMissSpeed =
      m_lastTimeStretchCacheMissSpeed.load(std::memory_order_acquire);
  snapshot[QStringLiteral("last_time_stretch_cache_miss_speed")] =
      lastTimeStretchMissSpeed;
  snapshot[QStringLiteral("last_time_stretch_expected_sidecar_path")] =
      (!m_lastTimeStretchCacheMissPath.isEmpty() &&
       lastTimeStretchMissSpeed > 1000)
          ? audioTimeStretchSidecarPathForSource(m_lastTimeStretchCacheMissPath,
                                                 lastTimeStretchMissSpeed)
          : QString();
  snapshot[QStringLiteral("time_stretch_precompute_blocked")] =
      m_timeStretchPrecomputeBlocked.load(std::memory_order_acquire);
  snapshot[QStringLiteral("time_stretch_readiness_state")] =
      timeStretchReadinessStateString(
          m_timeStretchReadinessState.load(std::memory_order_acquire));
  snapshot[QStringLiteral("time_stretch_generation_active")] =
      timeStretchGenerationActive;
  snapshot[QStringLiteral("time_stretch_generation_phase")] =
      timeStretchGenerationPhase;
  snapshot[QStringLiteral("time_stretch_generation_started_ms")] =
      timeStretchGenerationStartedMs;
  snapshot[QStringLiteral("time_stretch_generation_elapsed_ms")] =
      timeStretchGenerationActive
          ? qMax<qint64>(0, QDateTime::currentMSecsSinceEpoch() -
                                timeStretchGenerationStartedMs)
          : 0;
  snapshot[QStringLiteral("time_stretch_generation_last_finish_ms")] =
      timeStretchGenerationLastFinishMs;
  snapshot[QStringLiteral("time_stretch_generation_speed_key")] =
      timeStretchGenerationSpeedKey;
  snapshot[QStringLiteral("time_stretch_generation_source_frames")] =
      static_cast<qint64>(timeStretchGenerationSourceFrames);
  snapshot[QStringLiteral("time_stretch_generation_output_frames")] =
      static_cast<qint64>(timeStretchGenerationOutputFrames);
  snapshot[QStringLiteral("time_stretch_generation_progress")] =
      timeStretchGenerationProgress;
  snapshot[QStringLiteral("time_stretch_generation_last_succeeded")] =
      timeStretchGenerationLastSucceeded;
  snapshot[QStringLiteral("time_stretch_generation_attempt")] =
      timeStretchGenerationAttempt;
  snapshot[QStringLiteral("time_stretch_generation_last_end_reason")] =
      timeStretchGenerationLastEndReason;
  snapshot[QStringLiteral("time_stretch_generation_retry_suppressed_ms")] =
      timeStretchGenerationRetrySuppressedMs;
  snapshot[QStringLiteral("time_stretch_generation_job_failed_ms")] =
      timeStretchJobFailedMs;
  snapshot[QStringLiteral("time_stretch_generation_job_retry_suppressed_ms")] =
      timeStretchJobRetrySuppressedMs;
  snapshot[QStringLiteral("time_stretch_generation_path")] =
      timeStretchGenerationPath;
  snapshot[QStringLiteral("time_stretch_generation_sidecar_path")] =
      timeStretchGenerationSidecarPath;
  snapshot[QStringLiteral("time_stretch_generation_sidecar_exists")] =
      timeStretchSidecarInfo.exists();
  snapshot[QStringLiteral("time_stretch_generation_sidecar_bytes")] =
      timeStretchSidecarInfo.exists() ? timeStretchSidecarInfo.size() : 0;
  snapshot[QStringLiteral("time_stretch_generation_sidecar_metadata_readable")] =
      timeStretchSidecarMetadataReadable;
  snapshot[QStringLiteral("time_stretch_generation_sidecar_metadata_valid")] =
      timeStretchSidecarMetadata.valid;
  snapshot[QStringLiteral("time_stretch_generation_sidecar_metadata_fully_decoded")] =
      timeStretchSidecarMetadata.fullyDecoded;
  snapshot[QStringLiteral("time_stretch_generation_sidecar_metadata_sample_rate")] =
      timeStretchSidecarMetadata.sampleRate;
  snapshot[QStringLiteral("time_stretch_generation_sidecar_metadata_channel_count")] =
      timeStretchSidecarMetadata.channelCount;
  snapshot[QStringLiteral("time_stretch_generation_last_error")] =
      timeStretchGenerationLastError;
  snapshot[QStringLiteral("time_stretch_sidecar_only")] = true;
  snapshot[QStringLiteral("pitch_preserving_audio_blocked")] =
      m_pitchPreservingAudioBlocked.load(std::memory_order_acquire);
  snapshot[QStringLiteral("audio_playback_blocked")] =
      m_audioPlaybackBlocked.load(std::memory_order_acquire);
  snapshot[QStringLiteral("audio_output_unavailable")] =
      !audioOutputStatus.isEmpty();
  snapshot[QStringLiteral("audio_output_status")] = audioOutputStatus;
  if (!m_rtaudio) {
    snapshot[QStringLiteral("api")] = QStringLiteral("none");
    snapshot[QStringLiteral("device_count")] = 0;
    snapshot[QStringLiteral("stream_open")] = false;
    snapshot[QStringLiteral("stream_running")] = false;
    return snapshot;
  }

  snapshot[QStringLiteral("api")] = QString::fromStdString(
      rt::audio::RtAudio::getApiName(m_rtaudio->getCurrentApi()));
  snapshot[QStringLiteral("device_count")] = m_lastKnownDeviceCount;
  snapshot[QStringLiteral("stream_open")] = m_rtaudio->isStreamOpen();
  snapshot[QStringLiteral("stream_running")] = m_rtaudio->isStreamRunning();
  const long streamLatencyFrames = m_rtaudio->getStreamLatency();
  snapshot[QStringLiteral("stream_latency_frames")] =
      static_cast<qint64>(streamLatencyFrames);
  snapshot[QStringLiteral("stream_latency_ms")] = static_cast<qint64>(
      (qMax<long>(0, streamLatencyFrames) * 1000) / qMax(1, m_sampleRate));
  snapshot[QStringLiteral("start_count")] =
      static_cast<qint64>(m_startCount.load(std::memory_order_acquire));
  snapshot[QStringLiteral("redundant_start_count")] = static_cast<qint64>(
      m_redundantStartCount.load(std::memory_order_acquire));
  snapshot[QStringLiteral("seek_count")] =
      static_cast<qint64>(m_seekCount.load(std::memory_order_acquire));
  snapshot[QStringLiteral("last_start_frame")] =
      static_cast<qint64>(m_lastStartFrame.load(std::memory_order_acquire));
  snapshot[QStringLiteral("last_seek_frame")] =
      static_cast<qint64>(m_lastSeekFrame.load(std::memory_order_acquire));
  snapshot[QStringLiteral("default_output_device_id")] =
      static_cast<qint64>(m_lastKnownDefaultOutputId);
  snapshot[QStringLiteral("default_output_device_valid")] =
      m_lastKnownDefaultOutputValid;
  snapshot[QStringLiteral("default_output_device_name")] =
      m_lastKnownDefaultOutputName;
  snapshot[QStringLiteral("default_output_channels")] =
      static_cast<qint64>(m_lastKnownDefaultOutputChannels);
  if (!m_lastDeviceInfoError.isEmpty()) {
    snapshot[QStringLiteral("device_info_error")] = m_lastDeviceInfoError;
  }

  return snapshot;
}

int64_t AudioEngine::currentSample() const {
  const int64_t audibleSample = playbackClockSample();
  int64_t previous =
      m_lastReportedCurrentSample.load(std::memory_order_acquire);
  while (audibleSample > previous &&
         !m_lastReportedCurrentSample.compare_exchange_weak(
             previous, audibleSample, std::memory_order_release,
             std::memory_order_acquire)) {
  }
  return qMax(previous, audibleSample);
}

int64_t AudioEngine::playbackClockSample() const {
  const int64_t submittedSample =
      m_audioClockSample.load(std::memory_order_acquire);
  const qreal playbackRate =
      qBound<qreal>(0.1, m_playbackRate.load(std::memory_order_acquire), 3.0);
  long latencyFrames = 0;
  if (m_rtaudio && m_rtaudio->isStreamOpen()) {
    latencyFrames = m_rtaudio->getStreamLatency();
  }
  const int64_t latencyTimelineSamples = qMax<int64_t>(
      0, static_cast<int64_t>(std::llround(
             static_cast<long double>(qMax<long>(0, latencyFrames)) *
             static_cast<long double>(playbackRate))));
  return qMax<int64_t>(0, submittedSample - latencyTimelineSamples);
}

int64_t AudioEngine::currentFrame() const {
  return samplesToTimelineFrame(currentSample());
}

qreal AudioEngine::timeStretchGenerationProgress() const {
  return qBound<qreal>(
      0.0,
      static_cast<qreal>(m_timeStretchGenerationProgressPermille.load(
          std::memory_order_acquire)) /
          1000.0,
      1.0);
}

bool AudioEngine::timeStretchGenerationActive() const {
  return m_timeStretchGenerationActive.load(std::memory_order_acquire);
}

QString AudioEngine::timeStretchJobStateString(int state) {
  switch (state) {
  case TimeStretchJobDecoding:
    return QStringLiteral("decoding_source");
  case TimeStretchJobReadingSidecar:
    return QStringLiteral("reading_sidecar");
  case TimeStretchJobGenerating:
    return QStringLiteral("generating");
  case TimeStretchJobWritingSidecar:
    return QStringLiteral("writing_sidecar");
  case TimeStretchJobComplete:
    return QStringLiteral("complete");
  case TimeStretchJobFailed:
    return QStringLiteral("failed");
  case TimeStretchJobQueued:
  default:
    return QStringLiteral("queued");
  }
}

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
                                 rt::audio::RtAudioStreamStatus status,
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

AudioEngine::SpeechRangeBlend AudioEngine::calculateSpeechRangeBlend(
    int64_t samplePos, const QVector<SpeechSampleRange> &ranges,
    int fadeSamples, SpeechFilterFadeMode fadeMode, qreal curveStrength,
    bool crossfadeEnabled) const {
  SpeechRangeBlend blend;
  if (ranges.isEmpty()) {
    return blend;
  }

  int currentRangeIndex = -1;
  int64_t currentStart = 0;
  int64_t currentEndExclusive = 0;
  const auto currentIt = std::upper_bound(
      ranges.cbegin(), ranges.cend(), samplePos,
      [](int64_t sample, const SpeechSampleRange &range) {
        return sample < range.endSampleExclusive;
      });
  if (currentIt != ranges.cend() && samplePos >= currentIt->startSample) {
    currentRangeIndex = static_cast<int>(std::distance(ranges.cbegin(), currentIt));
    currentStart = currentIt->startSample;
    currentEndExclusive = currentIt->endSampleExclusive;
  }

  if (currentRangeIndex < 0) {
    blend.primaryGain = 0.0f;
    return blend;
  }

  if (fadeMode == SpeechFilterFadeMode::JumpCut || fadeSamples <= 0) {
    return blend;
  }

  const qreal boundedCurveStrength = qBound<qreal>(0.25, curveStrength, 4.0);
  const auto shaped = [fadeMode, boundedCurveStrength](float t) {
    t = qBound(0.0f, t, 1.0f);
    float value = t;
    switch (fadeMode) {
    case SpeechFilterFadeMode::SmoothStep:
      value = t * t * (3.0f - 2.0f * t);
      break;
    case SpeechFilterFadeMode::SmootherStep:
      value = t * t * t * (t * (t * 6.0f - 15.0f) + 10.0f);
      break;
    case SpeechFilterFadeMode::JumpCut:
    case SpeechFilterFadeMode::Fade:
    case SpeechFilterFadeMode::Crossfade:
      value = t;
      break;
    }
    if (fadeMode == SpeechFilterFadeMode::SmoothStep ||
        fadeMode == SpeechFilterFadeMode::SmootherStep) {
      value = std::pow(qBound(0.0f, value, 1.0f),
                       static_cast<float>(boundedCurveStrength));
    }
    return qBound(0.0f, value, 1.0f);
  };

  const bool useBoundaryCrossfade =
      crossfadeEnabled || fadeMode == SpeechFilterFadeMode::Crossfade;
  if (!useBoundaryCrossfade) {
    const int64_t samplesFromStart = samplePos - currentStart;
    const int64_t samplesToEnd = currentEndExclusive - samplePos;

    float gain = 1.0f;
    if (samplesFromStart < fadeSamples) {
      gain = qMin(gain, shaped(static_cast<float>(samplesFromStart) /
                               static_cast<float>(fadeSamples)));
    }
    if (samplesToEnd < fadeSamples) {
      gain = qMin(gain, shaped(static_cast<float>(samplesToEnd) /
                               static_cast<float>(fadeSamples)));
    }
    blend.primaryGain = qBound(0.0f, gain, 1.0f);
    return blend;
  }

  static constexpr float kHalfPi = 1.57079632679489661923f;
  const int64_t currentLength =
      qMax<int64_t>(1, currentEndExclusive - currentStart);

  if (currentRangeIndex > 0) {
    const int64_t prevStart = ranges.at(currentRangeIndex - 1).startSample;
    const int64_t prevEndExclusive =
        ranges.at(currentRangeIndex - 1).endSampleExclusive;
    const int64_t prevLength = qMax<int64_t>(1, prevEndExclusive - prevStart);
    const int64_t crossWindow = qMax<int64_t>(
        1,
        qMin<int64_t>(fadeSamples, qMin<int64_t>(prevLength, currentLength)));
    const int64_t offsetFromStart = samplePos - currentStart;
    if (offsetFromStart >= 0 && offsetFromStart < crossWindow) {
      const float t = (static_cast<float>(offsetFromStart) + 0.5f) /
                      static_cast<float>(crossWindow);
      const float s = (fadeMode == SpeechFilterFadeMode::Fade ||
                       fadeMode == SpeechFilterFadeMode::Crossfade)
                          ? t
                          : shaped(t);
      blend.primaryGain = std::sin(s * kHalfPi);
      blend.secondaryGain = std::cos(s * kHalfPi);
      blend.secondaryTimelineSample =
          (prevEndExclusive - crossWindow) + offsetFromStart;
      return blend;
    }
  }

  if (currentRangeIndex + 1 < ranges.size()) {
    const int64_t nextStart = ranges.at(currentRangeIndex + 1).startSample;
    const int64_t nextEndExclusive =
        ranges.at(currentRangeIndex + 1).endSampleExclusive;
    const int64_t nextLength = qMax<int64_t>(1, nextEndExclusive - nextStart);
    const int64_t crossWindow = qMax<int64_t>(
        1,
        qMin<int64_t>(fadeSamples, qMin<int64_t>(nextLength, currentLength)));
    const int64_t offsetFromWindowStart =
        samplePos - (currentEndExclusive - crossWindow);
    if (offsetFromWindowStart >= 0 && offsetFromWindowStart < crossWindow) {
      const float t = (static_cast<float>(offsetFromWindowStart) + 0.5f) /
                      static_cast<float>(crossWindow);
      const float s = (fadeMode == SpeechFilterFadeMode::Fade ||
                       fadeMode == SpeechFilterFadeMode::Crossfade)
                          ? t
                          : shaped(t);
      blend.primaryGain = std::cos(s * kHalfPi);
      blend.secondaryGain = std::sin(s * kHalfPi);
      blend.secondaryTimelineSample = nextStart + offsetFromWindowStart;
      return blend;
    }
  }

  return blend;
}

float AudioEngine::calculateClipCrossfadeGain(int64_t samplePos,
                                              const TimelineClip &clip,
                                              int64_t clipStartSample,
                                              int64_t clipEndSample,
                                              int fadeSamples) const {
  (void)clip;
  return editor::audio::clipFadeGain(samplePos, clipStartSample,
                                     clipEndSample, fadeSamples);
}

AudioEngine::AudioClipCacheEntry
AudioEngine::decodeClipAudio(const QString &path, int64_t maxOutputFrames,
                             int64_t sourceStartSample,
                             int audioStreamIndex) {
  AudioClipCacheEntry cache;
  const int64_t requestedSourceStartSample =
      qMax<int64_t>(0, sourceStartSample);

  AVFormatContext *formatCtx = nullptr;
  if (avformat_open_input(&formatCtx, QFile::encodeName(path).constData(),
                          nullptr, nullptr) < 0) {
    return cache;
  }

  int streamInfoRet = 0;
  {
    std::unique_lock<std::mutex> decodeLock(editor::ffmpegDecodeMutex());
    streamInfoRet = avformat_find_stream_info(formatCtx, nullptr);
  }
  if (streamInfoRet < 0) {
    avformat_close_input(&formatCtx);
    return cache;
  }

  int resolvedAudioStreamIndex = -1;
  if (audioStreamIndex >= 0 &&
      audioStreamIndex < static_cast<int>(formatCtx->nb_streams) &&
      formatCtx->streams[audioStreamIndex]->codecpar->codec_type ==
          AVMEDIA_TYPE_AUDIO) {
    resolvedAudioStreamIndex = audioStreamIndex;
  } else {
    for (unsigned i = 0; i < formatCtx->nb_streams; ++i) {
      if (formatCtx->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_AUDIO) {
        resolvedAudioStreamIndex = static_cast<int>(i);
        break;
      }
    }
  }
  if (resolvedAudioStreamIndex < 0) {
    avformat_close_input(&formatCtx);
    return cache;
  }

  AVStream *stream = formatCtx->streams[resolvedAudioStreamIndex];
  if (!stream || !stream->codecpar) {
    avformat_close_input(&formatCtx);
    return cache;
  }

  const AVCodec *decoder = avcodec_find_decoder(stream->codecpar->codec_id);
  if (!decoder) {
    avformat_close_input(&formatCtx);
    return cache;
  }

  AVCodecContext *codecCtx = avcodec_alloc_context3(decoder);
  if (!codecCtx) {
    avformat_close_input(&formatCtx);
    return cache;
  }

  if (avcodec_parameters_to_context(codecCtx, stream->codecpar) < 0 ||
      avcodec_open2(codecCtx, decoder, nullptr) < 0) {
    avcodec_free_context(&codecCtx);
    avformat_close_input(&formatCtx);
    return cache;
  }

  // Validate sample_rate: on some platforms (macOS/VideoToolbox),
  // avcodec_parameters_to_context may leave sample_rate at 0 even
  // when stream->codecpar->sample_rate is valid. Fall back to the
  // stream parameter if the codec context has 0.
  const int inSampleRate = codecCtx->sample_rate > 0
      ? codecCtx->sample_rate
      : (stream->codecpar->sample_rate > 0 ? stream->codecpar->sample_rate : 48000);
  SwrContext *swr = swr_alloc();
  ffmpeg_compat::ChannelLayoutHandle outLayout{};
  ffmpeg_compat::defaultChannelLayout(&outLayout, m_channelCount);
  ffmpeg_compat::setSwrInputLayout(swr, codecCtx);
  ffmpeg_compat::setSwrOutputLayout(swr, &outLayout);
  av_opt_set_int(swr, "in_sample_rate", inSampleRate, 0);
  av_opt_set_int(swr, "out_sample_rate", m_sampleRate, 0);
  av_opt_set_sample_fmt(swr, "in_sample_fmt", codecCtx->sample_fmt, 0);
  av_opt_set_sample_fmt(swr, "out_sample_fmt", AV_SAMPLE_FMT_FLT, 0);
  if (!swr || swr_init(swr) < 0) {
    ffmpeg_compat::uninitChannelLayout(&outLayout);
    swr_free(&swr);
    avcodec_free_context(&codecCtx);
    avformat_close_input(&formatCtx);
    return cache;
  }

  if (requestedSourceStartSample > 0) {
    const AVRational outputSampleTimeBase{1, m_sampleRate};
    const int64_t seekTimestamp = av_rescale_q(
        requestedSourceStartSample, outputSampleTimeBase, stream->time_base);
    if (av_seek_frame(formatCtx, resolvedAudioStreamIndex, seekTimestamp,
                      AVSEEK_FLAG_BACKWARD) >= 0) {
      avcodec_flush_buffers(codecCtx);
    }
  }

  AVPacket *packet = av_packet_alloc();
  AVFrame *frame = av_frame_alloc();
  QByteArray converted;
  const bool limitedDecode = maxOutputFrames > 0;
  const int64_t maxOutputSamples =
      limitedDecode ? qMax<int64_t>(1, maxOutputFrames * m_channelCount) : -1;
  bool reachedEof = false;
  bool hitOutputLimit = false;
  int64_t firstOutputSourceSample = -1;
  int64_t nextUnknownOutputSourceSample = requestedSourceStartSample;

  auto appendConverted = [&](AVFrame *decoded) {
    const int outSamples = swr_get_out_samples(swr, decoded->nb_samples);
    if (outSamples <= 0) {
      return;
    }
    uint8_t *outData = nullptr;
    int outLineSize = 0;
    if (av_samples_alloc(&outData, &outLineSize, m_channelCount, outSamples,
                         AV_SAMPLE_FMT_FLT, 0) < 0) {
      return;
    }
    const int convertedSamples =
        swr_convert(swr, &outData, outSamples,
                    const_cast<const uint8_t **>(decoded->extended_data),
                    decoded->nb_samples);
    if (convertedSamples > 0) {
      int64_t frameStartSourceSample = nextUnknownOutputSourceSample;
      const int64_t bestTimestamp = decoded->best_effort_timestamp;
      if (bestTimestamp != AV_NOPTS_VALUE) {
        frameStartSourceSample = av_rescale_q(bestTimestamp, stream->time_base,
                                              AVRational{1, m_sampleRate});
      }
      int skipFrames = 0;
      if (requestedSourceStartSample > 0) {
        const int64_t frameEndSourceSample =
            frameStartSourceSample + static_cast<int64_t>(convertedSamples);
        if (frameEndSourceSample <= requestedSourceStartSample) {
          nextUnknownOutputSourceSample = frameEndSourceSample;
          av_freep(&outData);
          return;
        }
        if (frameStartSourceSample < requestedSourceStartSample) {
          skipFrames = static_cast<int>(
              qMin<int64_t>(convertedSamples, requestedSourceStartSample -
                                                  frameStartSourceSample));
        }
      }
      const int retainedFrames = convertedSamples - skipFrames;
      if (retainedFrames <= 0) {
        nextUnknownOutputSourceSample =
            frameStartSourceSample + static_cast<int64_t>(convertedSamples);
        av_freep(&outData);
        return;
      }
      if (firstOutputSourceSample < 0) {
        firstOutputSourceSample = frameStartSourceSample + skipFrames;
      }
      const int byteOffset =
          skipFrames * m_channelCount * static_cast<int>(sizeof(float));
      const int byteCount =
          retainedFrames * m_channelCount * static_cast<int>(sizeof(float));
      converted.append(reinterpret_cast<const char *>(outData) + byteOffset,
                       byteCount);
      nextUnknownOutputSourceSample =
          frameStartSourceSample + static_cast<int64_t>(convertedSamples);
      if (maxOutputSamples > 0) {
        const int64_t currentSamples = static_cast<int64_t>(
            converted.size() / static_cast<int>(sizeof(float)));
        if (currentSamples >= maxOutputSamples) {
          const int truncatedBytes = static_cast<int>(
              maxOutputSamples * static_cast<int64_t>(sizeof(float)));
          converted.truncate(truncatedBytes);
          hitOutputLimit = true;
        }
      }
    }
    av_freep(&outData);
  };

  while (!hitOutputLimit && av_read_frame(formatCtx, packet) >= 0) {
    if (packet->stream_index != resolvedAudioStreamIndex) {
      av_packet_unref(packet);
      continue;
    }
    if (avcodec_send_packet(codecCtx, packet) >= 0) {
      while (avcodec_receive_frame(codecCtx, frame) >= 0) {
        appendConverted(frame);
        av_frame_unref(frame);
        if (hitOutputLimit) {
          break;
        }
      }
    }
    av_packet_unref(packet);
  }
  if (!hitOutputLimit) {
    reachedEof = true;
    avcodec_send_packet(codecCtx, nullptr);
    while (avcodec_receive_frame(codecCtx, frame) >= 0) {
      appendConverted(frame);
      av_frame_unref(frame);
    }
  }

  const int outSamples = (!hitOutputLimit) ? swr_get_out_samples(swr, 0) : 0;
  if (outSamples > 0 && !hitOutputLimit) {
    uint8_t *outData = nullptr;
    int outLineSize = 0;
    if (av_samples_alloc(&outData, &outLineSize, m_channelCount, outSamples,
                         AV_SAMPLE_FMT_FLT, 0) >= 0) {
      const int flushed = swr_convert(swr, &outData, outSamples, nullptr, 0);
      if (flushed > 0) {
        converted.append(reinterpret_cast<const char *>(outData),
                         flushed * m_channelCount *
                             static_cast<int>(sizeof(float)));
      }
      av_freep(&outData);
    }
  }

  const int sampleCount = converted.size() / static_cast<int>(sizeof(float));
  cache.samples.resize(sampleCount);
  std::memcpy(cache.samples.data(), converted.constData(), converted.size());
  cache.sourceStartSample = firstOutputSourceSample >= 0
                                ? firstOutputSourceSample
                                : requestedSourceStartSample;
  cache.valid = !cache.samples.isEmpty();
  cache.fullyDecoded = cache.valid && requestedSourceStartSample == 0 &&
                       (!limitedDecode || reachedEof);

  av_frame_free(&frame);
  av_packet_free(&packet);
  ffmpeg_compat::uninitChannelLayout(&outLayout);
  swr_free(&swr);
  avcodec_free_context(&codecCtx);
  avformat_close_input(&formatCtx);
  return cache;
}

AudioEngine::AudioClipCacheEntry
AudioEngine::clipCacheForPathCopy(const QString &path) const {
  std::lock_guard<std::mutex> lock(m_stateMutex);
  return m_audioCache.value(path);
}

AudioEngine::AudioClipCacheEntry
AudioEngine::timeStretchCacheForPathCopy(const QString &path, qreal speed,
                                         int64_t sourceSample,
                                         int64_t sourceEndSampleExclusive,
                                         PlaybackAudioWarpMode mode) {
  const int rateKey = timeStretchRateKey(speed);
  const int sidecarSpeedKey = precomputedTimeStretchSpeedKey(speed, mode);
  if (!pitchPreservingTimeStretchActive(speed, mode)) {
    return {};
  }
  uint64_t sourceGeneration = 0;
  {
    std::lock_guard<std::mutex> lock(m_stateMutex);
    sourceGeneration = m_sourceGeneration;
    const auto pathIt = m_timeStretchAudioCache.constFind(path);
    if (pathIt != m_timeStretchAudioCache.cend()) {
      const QVector<AudioClipCacheEntry> segments =
          pathIt.value().value(rateKey);
      for (const AudioClipCacheEntry &segment : segments) {
        const int64_t segmentFrames = static_cast<int64_t>(
            segment.samples.size() / qMax(1, segment.channelCount));
        if (segment.valid &&
            audioTimeStretchSegmentCoversSourceRange(
                segment.sourceStartSample, segmentFrames, sourceSample,
                sourceEndSampleExclusive, speed)) {
          return segment;
        }
      }
    }
  }

  if (sidecarSpeedKey <= 1) {
    return {};
  }
  const AudioClipCacheEntry sidecarEntry = audioCacheEntryFromTimeStretchEntry(
      readTimeStretchSidecarEntrySingleFlight(path, sidecarSpeedKey));
  if (!sidecarEntry.valid) {
    return {};
  }
  const int64_t sidecarFrames = static_cast<int64_t>(
      sidecarEntry.samples.size() / qMax(1, sidecarEntry.channelCount));
  if (!audioTimeStretchSegmentCoversSourceRange(
          sidecarEntry.sourceStartSample, sidecarFrames, sourceSample,
          sourceEndSampleExclusive, speed)) {
    return {};
  }
  {
    std::lock_guard<std::mutex> lock(m_stateMutex);
    if (sourceGeneration != m_sourceGeneration) {
      return {};
    }
    m_timeStretchAudioCache[path].insert(
        rateKey, QVector<AudioClipCacheEntry>{sidecarEntry});
  }
  return sidecarEntry;
}

void AudioEngine::insertTimeStretchSegmentsLocked(
    const QString &path, QHash<int, AudioClipCacheEntry> warpedBySpeed) {
  if (path.isEmpty() || warpedBySpeed.isEmpty()) {
    return;
  }
  auto &pathCache = m_timeStretchAudioCache[path];
  for (auto it = warpedBySpeed.begin(); it != warpedBySpeed.end(); ++it) {
    AudioClipCacheEntry segment = it.value();
    if (!segment.valid) {
      continue;
    }
    QVector<AudioClipCacheEntry> &segments = pathCache[it.key()];
    bool replaced = false;
    for (AudioClipCacheEntry &existing : segments) {
      if (existing.sourceStartSample == segment.sourceStartSample) {
        existing = std::move(segment);
        replaced = true;
        break;
      }
    }
    if (!replaced) {
      segments.push_back(std::move(segment));
    }
    std::sort(segments.begin(), segments.end(),
              [](const AudioClipCacheEntry &a, const AudioClipCacheEntry &b) {
                return a.sourceStartSample < b.sourceStartSample;
              });
    constexpr int kMaxTimeStretchSegmentsPerRate = 6;
    while (segments.size() > kMaxTimeStretchSegmentsPerRate) {
      segments.removeFirst();
    }
  }
}

bool AudioEngine::timeStretchCacheHasFullyDecodedPathLocked(const QString &path,
                                                            qreal speed) const {
  const auto pathIt = m_timeStretchAudioCache.constFind(path);
  if (pathIt == m_timeStretchAudioCache.cend()) {
    return false;
  }
  const auto speedIt = pathIt.value().constFind(timeStretchRateKey(speed));
  if (speedIt == pathIt.value().cend()) {
    return false;
  }
  for (const AudioClipCacheEntry &segment : speedIt.value()) {
    if (segment.valid && segment.sourceStartSample == 0 &&
        segment.fullyDecoded) {
      return true;
    }
  }
  return false;
}

AudioEngine::AudioClipCacheEntry
AudioEngine::audioCacheEntryFromTimeStretchEntry(
    const AudioTimeStretchCacheEntry &source) {
  AudioClipCacheEntry entry;
  entry.samples = source.samples;
  entry.sourceStartSample = 0;
  entry.sampleRate = source.sampleRate;
  entry.channelCount = source.channelCount;
  entry.valid = source.valid;
  entry.fullyDecoded = source.fullyDecoded;
  return entry;
}

AudioTimeStretchCacheEntry AudioEngine::timeStretchEntryFromAudioCacheEntry(
    const AudioClipCacheEntry &source) {
  AudioTimeStretchCacheEntry entry;
  entry.samples = source.samples;
  entry.sampleRate = source.sampleRate;
  entry.channelCount = source.channelCount;
  entry.valid = source.valid;
  entry.fullyDecoded = source.fullyDecoded;
  return entry;
}

AudioTimeStretchCacheEntry
AudioEngine::readTimeStretchSidecarEntry(const QString &path, int speedKey) {
  AudioTimeStretchCacheEntry entry;
  readAudioTimeStretchSidecar(path, speedKey, &entry);
  return entry;
}

QString AudioEngine::timeStretchSidecarLoadKey(const QString &path,
                                               int speedKey) {
  return QStringLiteral("%1|%2").arg(path, QString::number(speedKey));
}

AudioTimeStretchCacheEntry
AudioEngine::readTimeStretchSidecarEntrySingleFlight(const QString &path,
                                                     int speedKey) {
  const QString key = timeStretchSidecarLoadKey(path, speedKey);
  uint64_t sourceGeneration = 0;
  {
    std::unique_lock<std::mutex> lock(m_stateMutex);
    sourceGeneration = m_sourceGeneration;
    while (m_timeStretchSidecarLoadsInFlight.contains(key)) {
      m_stateCondition.wait(lock);
      if (sourceGeneration != m_sourceGeneration) {
        return {};
      }
      const auto loadedIt = m_timeStretchSidecarEntryCache.constFind(key);
      if (loadedIt != m_timeStretchSidecarEntryCache.constEnd()) {
        return loadedIt.value();
      }
    }
    const auto loadedIt = m_timeStretchSidecarEntryCache.constFind(key);
    if (loadedIt != m_timeStretchSidecarEntryCache.constEnd()) {
      return loadedIt.value();
    }
    m_timeStretchSidecarLoadsInFlight.insert(key);
  }

  AudioTimeStretchCacheEntry entry =
      readTimeStretchSidecarEntry(path, speedKey);
  {
    std::lock_guard<std::mutex> lock(m_stateMutex);
    if (sourceGeneration == m_sourceGeneration && entry.valid &&
        entry.fullyDecoded) {
      m_timeStretchSidecarEntryCache.insert(key, entry);
    }
    m_timeStretchSidecarLoadsInFlight.remove(key);
  }
  m_stateCondition.notify_all();
  return sourceGenerationCurrent(sourceGeneration) ? entry
                                                   : AudioTimeStretchCacheEntry{};
}

bool AudioEngine::sourceGenerationCurrent(
    uint64_t expectedGeneration) const {
  std::lock_guard<std::mutex> lock(m_stateMutex);
  return expectedGeneration == m_sourceGeneration;
}

bool AudioEngine::recordDecodeResultLocked(
    const QString &path, uint64_t expectedSourceGeneration, bool valid) {
  if (path.isEmpty() || expectedSourceGeneration != m_sourceGeneration) {
    return false;
  }
  if (valid) {
    m_failedDecodePaths.remove(path);
  } else {
    m_failedDecodePaths.insert(path);
  }
  return true;
}

AudioEngine::AudioClipCacheEntry AudioEngine::buildTimeStretchCacheEntry(
    const QString &path, const AudioClipCacheEntry &decoded, qreal speed,
    PlaybackAudioWarpMode mode, uint64_t expectedSourceGeneration) {
  AudioClipCacheEntry warped;
  if (!sourceGenerationCurrent(expectedSourceGeneration)) {
    return warped;
  }
  const int sidecarSpeedKey = precomputedTimeStretchSpeedKey(speed, mode);
  if (!decoded.valid || decoded.samples.isEmpty() ||
      !pitchPreservingTimeStretchActive(speed, mode)) {
    return warped;
  }
  if (sidecarSpeedKey > 1 && decoded.sourceStartSample == 0 &&
      decoded.fullyDecoded) {
    const int64_t sourceFrames = static_cast<int64_t>(
        decoded.samples.size() / qMax(1, decoded.channelCount));
    if (beginTimeStretchJobAttempt(path, sidecarSpeedKey,
                                   expectedSourceGeneration) <= 0 ||
        !beginTimeStretchGenerationForSourceGeneration(
            path, sidecarSpeedKey, sourceFrames,
            expectedSourceGeneration)) {
      return {};
    }
    warped = audioCacheEntryFromTimeStretchEntry(
        readTimeStretchSidecarEntrySingleFlight(path, sidecarSpeedKey));
    if (!sourceGenerationCurrent(expectedSourceGeneration)) {
      abandonTimeStretchGenerationForSourceGeneration(
          path, sidecarSpeedKey, expectedSourceGeneration);
      return {};
    }
    const int64_t warpedFrames = static_cast<int64_t>(
        warped.samples.size() / qMax(1, warped.channelCount));
    const int64_t minExpectedFrames = qMax<int64_t>(
        1, static_cast<int64_t>(std::floor(
               static_cast<long double>(sourceFrames) /
               static_cast<long double>(qMax<qreal>(0.1, speed)) * 0.90L)));
    if (warped.valid && warpedFrames >= minExpectedFrames) {
      if (!finishTimeStretchGenerationForSourceGeneration(
              path, sidecarSpeedKey, true, warpedFrames,
              QStringLiteral("sidecar_cache_hit"), QString(),
              expectedSourceGeneration)) {
        abandonTimeStretchGenerationForSourceGeneration(
            path, sidecarSpeedKey, expectedSourceGeneration);
        return {};
      }
      return warped;
    }
  }
  if (sidecarSpeedKey > 1 && decoded.sourceStartSample == 0 &&
      decoded.fullyDecoded) {
    if (!updateTimeStretchGenerationForSourceGeneration(
            path, sidecarSpeedKey, TimeStretchJobGenerating, 0.0,
            TimeStretchGenerationRubberBand, expectedSourceGeneration)) {
      abandonTimeStretchGenerationForSourceGeneration(
          path, sidecarSpeedKey, expectedSourceGeneration);
      return {};
    }
    auto reportRubberBandProgress =
        [this, path, sidecarSpeedKey,
         expectedSourceGeneration](double progress) {
          (void)updateTimeStretchGenerationForSourceGeneration(
              path, sidecarSpeedKey, TimeStretchJobGenerating,
              qBound<qreal>(0.0, progress * 0.95, 0.95),
              TimeStretchGenerationRubberBand, expectedSourceGeneration);
        };
    QVector<float> stretched;
    if (mode == PlaybackAudioWarpMode::RubberBandPassThroughFrequency) {
      stretched = editor::audio::SpeechHarmonicIsolator::process(
          {&decoded.samples, decoded.channelCount, decoded.sampleRate, speed,
           rubberBandSettingsFromRuntimeControls()},
          reportRubberBandProgress);
    } else {
      stretched = timeStretchPreservePitch(
          decoded.samples, decoded.channelCount, decoded.sampleRate, speed,
          AudioTimeStretchBackend::RubberBand, reportRubberBandProgress,
          rubberBandSettingsFromRuntimeControls());
    }
    QString terminalError;
    QString terminalReason;
    if (!stretched.isEmpty()) {
      const int64_t sourceFrames = static_cast<int64_t>(
          decoded.samples.size() / qMax(1, decoded.channelCount));
      const int64_t stretchedFrames = static_cast<int64_t>(
          stretched.size() / qMax(1, decoded.channelCount));
      const int64_t minExpectedFrames = qMax<int64_t>(
          1, static_cast<int64_t>(std::floor(
                 static_cast<long double>(sourceFrames) /
                 static_cast<long double>(qMax<qreal>(0.1, speed)) * 0.90L)));
      if (stretchedFrames < minExpectedFrames) {
        qWarning().noquote()
            << QStringLiteral(
                   "Audio time-stretch sidecar generation rejected short "
                   "output: speed=%1x frames=%2 expected_min=%3 path=\"%4\"")
                   .arg(QString::number(speed, 'f', 3))
                   .arg(stretchedFrames)
                   .arg(minExpectedFrames)
                   .arg(path);
        const QString error = QStringLiteral("short_output:%1:%2")
                                  .arg(stretchedFrames)
                                  .arg(minExpectedFrames);
        if (!finishTimeStretchGenerationForSourceGeneration(
                path, sidecarSpeedKey, false, stretchedFrames,
                QStringLiteral("rubberband_short_output"), error,
                expectedSourceGeneration)) {
          abandonTimeStretchGenerationForSourceGeneration(
              path, sidecarSpeedKey, expectedSourceGeneration);
        }
        return {};
      }
      AudioTimeStretchCacheEntry sidecar;
      sidecar.samples = stretched;
      sidecar.sampleRate = decoded.sampleRate;
      sidecar.channelCount = decoded.channelCount;
      sidecar.valid = true;
      sidecar.fullyDecoded = true;
      if (!updateTimeStretchGenerationForSourceGeneration(
              path, sidecarSpeedKey, TimeStretchJobWritingSidecar, 0.98,
              TimeStretchGenerationWritingSidecar,
              expectedSourceGeneration)) {
        abandonTimeStretchGenerationForSourceGeneration(
            path, sidecarSpeedKey, expectedSourceGeneration);
        return {};
      }
      const bool sidecarWritten = writeAudioTimeStretchSidecar(
          path, sidecarSpeedKey, sidecar,
          [this, path, sidecarSpeedKey,
           expectedSourceGeneration](double writeProgress) {
            const qreal boundedProgress =
                qBound<qreal>(0.0, writeProgress, 1.0);
            const qreal totalProgress = 0.98 + (boundedProgress * 0.02);
            (void)updateTimeStretchGenerationForSourceGeneration(
                path, sidecarSpeedKey, TimeStretchJobWritingSidecar,
                qBound<qreal>(0.98, totalProgress, 1.0),
                TimeStretchGenerationWritingSidecar,
                expectedSourceGeneration);
          },
          [this, expectedSourceGeneration]() {
            return sourceGenerationCurrent(expectedSourceGeneration);
          },
          [this, expectedSourceGeneration](
              const std::function<bool()> &commit) {
            std::lock_guard<std::mutex> commitLock(
                m_sourceGenerationCommitMutex);
            {
              std::lock_guard<std::mutex> stateLock(m_stateMutex);
              if (expectedSourceGeneration != m_sourceGeneration) {
                return false;
              }
            }
            return commit();
          });
      if (!sourceGenerationCurrent(expectedSourceGeneration)) {
        abandonTimeStretchGenerationForSourceGeneration(
            path, sidecarSpeedKey, expectedSourceGeneration);
        return {};
      }
      if (sidecarWritten) {
        warped = audioCacheEntryFromTimeStretchEntry(sidecar);
      } else {
        terminalError =
            QStringLiteral("sidecar_write_failed:%1")
                .arg(audioTimeStretchSidecarPathForSource(path,
                                                          sidecarSpeedKey));
        terminalReason = QStringLiteral("sidecar_write_failed");
      }
    } else {
      terminalError = QStringLiteral("rubberband_empty_output");
      terminalReason = QStringLiteral("rubberband_empty_output");
    }
    const bool succeeded = warped.valid;
    if (!succeeded && terminalError.isEmpty()) {
      terminalError = QStringLiteral("generation_failed_or_empty_output");
    }
    if (terminalReason.isEmpty()) {
      terminalReason =
          succeeded ? QStringLiteral("generated_and_sidecar_committed")
                    : QStringLiteral("generation_failed_or_empty_output");
    }
    const int64_t warpedFrames =
        succeeded ? static_cast<int64_t>(
                        warped.samples.size() / qMax(1, warped.channelCount))
                  : 0;
    if (!finishTimeStretchGenerationForSourceGeneration(
            path, sidecarSpeedKey, succeeded, warpedFrames, terminalReason,
            terminalError, expectedSourceGeneration)) {
      abandonTimeStretchGenerationForSourceGeneration(
          path, sidecarSpeedKey, expectedSourceGeneration);
      return {};
    }
  }
  return warped;
}

QHash<int, AudioEngine::AudioClipCacheEntry>
AudioEngine::buildPrecomputedTimeStretchEntries(
    const QString &path, const AudioClipCacheEntry &decoded,
    bool precomputeRequested, uint64_t expectedSourceGeneration) {
  QHash<int, AudioClipCacheEntry> warpedBySpeed;
  if (!sourceGenerationCurrent(expectedSourceGeneration)) {
    return warpedBySpeed;
  }
  const PlaybackAudioWarpMode warpMode = static_cast<PlaybackAudioWarpMode>(
      m_playbackWarpMode.load(std::memory_order_acquire));
  if (!precomputeRequested &&
      !playbackWarpModeUsesTimeStretch(warpMode)) {
    return warpedBySpeed;
  }
  const qreal speed =
      qBound<qreal>(0.1, m_playbackRate.load(std::memory_order_acquire), 3.0);
  if (!pitchPreservingTimeStretchActive(speed, warpMode)) {
    return warpedBySpeed;
  }
  AudioClipCacheEntry warped =
      buildTimeStretchCacheEntry(path, decoded, speed, warpMode,
                                 expectedSourceGeneration);
  if (warped.valid) {
    warpedBySpeed.insert(timeStretchRateKey(speed), std::move(warped));
  }
  return warpedBySpeed;
}

QVector<ExportRangeSegment> AudioEngine::exportRangesCopy() const {
  std::lock_guard<std::mutex> lock(m_exportRangesMutex);
  return m_exportRanges;
}

QVector<ExportRangeSegment> AudioEngine::transcriptNormalizeRangesCopy() const {
  std::lock_guard<std::mutex> lock(m_transcriptNormalizeRangesMutex);
  return m_transcriptNormalizeRanges;
}

bool AudioEngine::mixChunk(const MixContext &context, float *output, int frames,
                           int64_t chunkStartSample, qreal playbackRate,
                           qreal timelineRate) {
  std::fill(output, output + frames * m_channelCount, 0.0f);
  const qreal clampedRate = qBound<qreal>(0.1, playbackRate, 3.0);
  const qreal clampedTimelineRate = qBound<qreal>(0.05, timelineRate, 3.05);
  QVector<SpeechSampleRange> derivedSpeechSampleRanges;
  const QVector<SpeechSampleRange> *speechSampleRanges = &context.speechSampleRanges;
  if (speechSampleRanges->isEmpty() && !context.exportRanges.isEmpty()) {
    derivedSpeechSampleRanges.reserve(context.exportRanges.size());
    for (const ExportRangeSegment &range : context.exportRanges) {
      const int64_t startSample = timelineFrameToSamples(range.startFrame);
      const int64_t endSampleExclusive = timelineFrameToSamples(range.endFrame + 1);
      if (endSampleExclusive > startSample) {
        derivedSpeechSampleRanges.push_back(
            SpeechSampleRange{startSample, endSampleExclusive});
      }
    }
    speechSampleRanges = &derivedSpeechSampleRanges;
  }
  struct PreparedClipAudio {
    struct TranscriptNormalizeSegment {
      int64_t startSample = 0;
      int64_t endSampleExclusive = 0;
      float gain = 1.0f;
    };
    const TimelineClip *clip = nullptr;
    AudioClipCacheEntry audio;
    int64_t clipStartSample = 0;
    int64_t clipEndSample = 0;
    int64_t sourceInSample = 0;
    int64_t maxSourceSample = 0;
    int64_t playbackRateScaled = 1000;
    float transcriptNormalizeGain = 1.0f;
    QVector<TranscriptNormalizeSegment> transcriptNormalizeSegments;
    qreal precomputedTimeStretchSpeed = 1.0;
    bool linearSourceMapping = false;
    bool usingPrecomputedTimeStretch = false;
    bool starvedThisChunk = false;
    bool starvationEnqueued = false;
  };
  QVector<PreparedClipAudio> preparedClips;
  preparedClips.reserve(context.clips.size());
  int cacheHitCount = 0;
  int cacheMissCount = 0;
  int invalidAudioCount = 0;
  enum LastMixSilentReason {
    SilentReasonNone = 0,
    SilentReasonMuted = 1,
    SilentReasonNoPreparedClips = 2,
    SilentReasonWaitingForPlayableAudio = 3,
    SilentReasonNoActiveClipInChunk = 4,
    SilentReasonInputOutOfRange = 5,
    SilentReasonSpeechGainZero = 6,
    SilentReasonClipGainZero = 7,
    SilentReasonSourceSamplesZero = 8,
    SilentReasonOutputBelowThreshold = 9
  };
  const PlaybackAudioWarpMode warpMode = static_cast<PlaybackAudioWarpMode>(
      m_playbackWarpMode.load(std::memory_order_acquire));
  const bool timeStretchActive =
      playbackWarpModeUsesTimeStretch(warpMode) &&
      pitchPreservingTimeStretchActive(clampedRate, warpMode);
  const qreal timeStretchSpeed =
      timeStretchActive ? clampedRate : 1.0;
  auto storeBlockedMixDebug = [&](int reason) {
    m_lastMixPreparedClipCount.store(preparedClips.size(),
                                     std::memory_order_release);
    m_lastMixCacheHitCount.store(cacheHitCount, std::memory_order_release);
    m_lastMixCacheMissCount.store(cacheMissCount, std::memory_order_release);
    m_lastMixInvalidAudioCount.store(invalidAudioCount,
                                     std::memory_order_release);
    m_lastMixPeakPermille.store(0, std::memory_order_release);
    m_lastMixRmsPermille.store(0, std::memory_order_release);
    m_lastMixNonzeroSampleCount.store(0, std::memory_order_release);
    m_lastMixChunkStartSample.store(chunkStartSample,
                                    std::memory_order_release);
    m_lastMixChunkEndSample.store(
        chunkStartSample +
            static_cast<int64_t>(std::ceil(frames * clampedTimelineRate)),
        std::memory_order_release);
    m_lastMixFramesWithActiveClip.store(0, std::memory_order_release);
    m_lastMixFramesInputOutOfRange.store(0, std::memory_order_release);
    m_lastMixFramesSpeechGainZero.store(0, std::memory_order_release);
    m_lastMixFramesClipGainZero.store(0, std::memory_order_release);
    m_lastMixFramesSourceNonzero.store(0, std::memory_order_release);
    m_lastMixFramesOutputNonzero.store(0, std::memory_order_release);
    m_lastMixSourcePeakPermille.store(0, std::memory_order_release);
    m_lastMixPrimaryGainPeakPermille.store(0, std::memory_order_release);
    m_lastMixOutOfRangeTimelineSample.store(-1, std::memory_order_release);
    m_lastMixOutOfRangeSourceSample.store(-1, std::memory_order_release);
    m_lastMixOutOfRangeNormalizedSample.store(-1, std::memory_order_release);
    m_lastMixOutOfRangeAudioStartSample.store(-1, std::memory_order_release);
    m_lastMixOutOfRangeAudioEndSample.store(-1, std::memory_order_release);
    m_lastMixTimeStretchSpeedPermille.store(qRound(timeStretchSpeed * 1000.0),
                                            std::memory_order_release);
    m_lastMixSilentReason.store(reason, std::memory_order_release);
    m_lastMixStarvedClipCount.store(0, std::memory_order_release);
    m_pitchPreservingAudioBlocked.store(timeStretchActive,
                                        std::memory_order_release);
    m_audioPlaybackBlocked.store(true, std::memory_order_release);
  };
  const bool soloActive = anyAudioSolo(context.clips, context.tracks);
  for (const TimelineClip &clip : context.clips) {
    const float mixerGain = mixerGainForClip(clip, context.tracks, soloActive);
    if (mixerGain <= 0.0f) {
      continue;
    }
    const QString audioPath = clipAudioPathForScheduling(clip);
    AudioClipCacheEntry audio;
    bool usingPrecomputedTimeStretch = false;
    const int64_t clipStartSampleForLookup = clipTimelineStartSamples(clip);
    const int64_t clipEndSampleForLookup = clipTimelineEndSamples(clip);
    const int64_t lookupTimelineSample = qBound<int64_t>(
        clipStartSampleForLookup, chunkStartSample, clipEndSampleForLookup - 1);
    const int64_t chunkTimelineStep =
        qMax<int64_t>(1, static_cast<int64_t>(std::llround(
                             clampedTimelineRate * static_cast<qreal>(frames))));
    const int64_t lookupEndTimelineSample = qBound<int64_t>(
        lookupTimelineSample + 1, lookupTimelineSample + chunkTimelineStep,
        clipEndSampleForLookup);
    const int64_t lookupSourceSample = sourceSampleForClipAtTimelineSample(
        clip, lookupTimelineSample, context.renderSyncMarkers);
    const int64_t lookupSourceEndSampleExclusive =
        sourceSampleForClipAtTimelineSample(
            clip,
            qMax<int64_t>(lookupTimelineSample, lookupEndTimelineSample - 1),
            context.renderSyncMarkers) +
        1;
    if (timeStretchActive) {
      audio = timeStretchCacheForPathCopy(audioPath, timeStretchSpeed,
                                          lookupSourceSample,
                                          lookupSourceEndSampleExclusive,
                                          warpMode);
      usingPrecomputedTimeStretch = audio.valid;
      if (!usingPrecomputedTimeStretch) {
        ++cacheMissCount;
        m_timeStretchCacheMissCount.fetch_add(1, std::memory_order_relaxed);
        m_lastTimeStretchCacheMissSpeed.store(
            timeStretchRateKey(timeStretchSpeed), std::memory_order_release);
        const int64_t clipStartSample = clipTimelineStartSamples(clip);
        const int64_t clipEndSample = clipTimelineEndSamples(clip);
        const int64_t timelineSample = qBound<int64_t>(
            clipStartSample, chunkStartSample, clipEndSample - 1);
        const int64_t sourceSample = sourceSampleForClipAtTimelineSample(
            clip, timelineSample, context.renderSyncMarkers);
        {
          std::lock_guard<std::mutex> lock(m_stateMutex);
          m_lastTimeStretchCacheMissPath = audioPath;
        }
        enqueueTimeStretchPrecomputeForPath(audioPath, sourceSample);
        const qint64 now = QDateTime::currentMSecsSinceEpoch();
        const qint64 previousWarning =
            m_lastTimeStretchCacheMissWarningMs.load(std::memory_order_acquire);
        if (now - previousWarning >= kAudioInitWarningThrottleMs) {
          m_lastTimeStretchCacheMissWarningMs.store(now,
                                                    std::memory_order_release);
          qWarning().noquote()
              << QStringLiteral(
                     "Audio time-stretch cache miss: speed=%1x path=\"%2\"; "
                     "holding playback audio until pitch-preserving audio is "
                     "ready.")
                     .arg(QString::number(timeStretchSpeed, 'f', 3))
                     .arg(audioPath);
        }
        continue;
      }
    } else {
      audio = clipCacheForPathCopy(audioPath);
    }
    if (!audio.valid) {
      if (audioPath.isEmpty()) {
        ++cacheMissCount;
      } else {
        ++invalidAudioCount;
        const int64_t clipStartSample = clipTimelineStartSamples(clip);
        const int64_t clipEndSample = clipTimelineEndSamples(clip);
        const int64_t timelineSample = qBound<int64_t>(
            clipStartSample, chunkStartSample, clipEndSample - 1);
        enqueuePreviewDecodeForPath(
            audioPath, sourceSampleForClipAtTimelineSample(
                           clip, timelineSample, context.renderSyncMarkers));
      }
      continue;
    }
    ++cacheHitCount;
    const int64_t clipStartSample = clipTimelineStartSamples(clip);
    const int64_t sourceInSample = clipSourceInSamples(clip);
    const int64_t normalizedSourceInSample =
        timeStretchCacheSampleForSourceSample(sourceInSample, timeStretchSpeed);
    const int64_t clipAvailableSamples =
        audio.sourceStartSample + (audio.samples.size() / m_channelCount) -
        normalizedSourceInSample;
    if (clipAvailableSamples <= 0) {
      if (usingPrecomputedTimeStretch) {
        enqueueTimeStretchPrecomputeForPath(audioPath, sourceInSample);
      } else {
        enqueuePreviewDecodeForPath(audioPath, normalizedSourceInSample);
      }
      continue;
    }
    const int64_t clipAvailableTimelineSamples =
        usingPrecomputedTimeStretch
            ? sourceSamplesCoveredByTimeStretchCacheSamples(
                  clipAvailableSamples, timeStretchSpeed)
            : clipAvailableSamples;
    const int64_t timelineClipSamples = clipTimelineDurationSamples(clip);
    const int64_t clipEndSample = clipStartSample + timelineClipSamples;
    if (clipEndSample <= clipStartSample) {
      continue;
    }
    const int64_t segmentEndSample =
        clipStartSample + clipAvailableTimelineSamples;
    if (chunkStartSample >= segmentEndSample) {
      if (usingPrecomputedTimeStretch) {
        enqueueTimeStretchPrecomputeForPath(audioPath, lookupSourceSample);
      } else {
        enqueuePreviewDecodeForPath(audioPath, lookupSourceSample);
      }
    }
    PreparedClipAudio prepared;
    prepared.clip = &clip;
    prepared.audio = audio;
    prepared.clipStartSample = clipStartSample;
    prepared.clipEndSample = clipEndSample;
    prepared.sourceInSample = sourceInSample;
    prepared.maxSourceSample =
        sourceInSample +
        qMax<int64_t>(
            0, sourceFramesToSamples(clip, static_cast<qreal>(qMax<int64_t>(
                                               0, clip.sourceDurationFrames))) -
                   1);
    prepared.playbackRateScaled =
        qMax<int64_t>(1, static_cast<int64_t>(clip.playbackRate * 1000.0));
    prepared.precomputedTimeStretchSpeed =
        usingPrecomputedTimeStretch ? timeStretchSpeed : 1.0;
    prepared.usingPrecomputedTimeStretch = usingPrecomputedTimeStretch;
    prepared.linearSourceMapping = context.renderSyncMarkers.isEmpty();
    preparedClips.push_back(prepared);

    if (usingPrecomputedTimeStretch) {
      const int64_t segmentFrames = static_cast<int64_t>(
          audio.samples.size() / qMax(1, audio.channelCount));
      const int64_t normalizedLookupSample =
          timeStretchCacheSampleForSourceSample(lookupSourceSample,
                                                timeStretchSpeed);
      const int64_t remainingSegmentSamples =
          (audio.sourceStartSample + segmentFrames) - normalizedLookupSample;
      constexpr int64_t kTimeStretchPrefetchLeadSamples = m_sampleRate * 5;
      if (remainingSegmentSamples > 0 &&
          remainingSegmentSamples < kTimeStretchPrefetchLeadSamples) {
        const int64_t nextSourceSample =
            sourceSamplesCoveredByTimeStretchCacheSamples(
                audio.sourceStartSample + segmentFrames, timeStretchSpeed);
        enqueueTimeStretchPrecomputeForPath(audioPath, nextSourceSample);
      }
    }
  }

  const bool transcriptNormalizeEnabled =
      m_transcriptNormalizeEnabled.load(std::memory_order_acquire);
  const bool blockedWaitingForPlayableAudio = mixPrepareMustBlock(
      preparedClips.size(), cacheMissCount, invalidAudioCount);
  if (blockedWaitingForPlayableAudio) {
    storeBlockedMixDebug(SilentReasonWaitingForPlayableAudio);
    return false;
  }
  m_pitchPreservingAudioBlocked.store(false, std::memory_order_release);
  m_audioPlaybackBlocked.store(false, std::memory_order_release);
  m_timeStretchPrecomputeBlocked.store(false, std::memory_order_release);

  const QVector<ExportRangeSegment> transcriptNormalizeRanges =
      transcriptNormalizeRangesCopy();
  if (transcriptNormalizeEnabled && !transcriptNormalizeRanges.isEmpty()) {
    constexpr float kTranscriptNormalizeTargetLinear = 0.95f;
    constexpr float kMaxTranscriptNormalizeGain = 2.5f;
    for (PreparedClipAudio &prepared : preparedClips) {
      for (const ExportRangeSegment &range : transcriptNormalizeRanges) {
        const int64_t rangeStartSample =
            timelineFrameToSamples(range.startFrame);
        const int64_t rangeEndSampleExclusive =
            timelineFrameToSamples(range.endFrame + 1);
        const int64_t overlapStart =
            qMax<int64_t>(prepared.clipStartSample, rangeStartSample);
        const int64_t overlapEndExclusive =
            qMin<int64_t>(prepared.clipEndSample, rangeEndSampleExclusive);
        if (overlapEndExclusive <= overlapStart) {
          continue;
        }

        const int64_t sourceStartSample = sourceSampleForClipAtTimelineSample(
            *prepared.clip, overlapStart, context.renderSyncMarkers);
        const int64_t sourceEndSampleExclusive =
            sourceSampleForClipAtTimelineSample(*prepared.clip,
                                                overlapEndExclusive - 1,
                                                context.renderSyncMarkers) +
            1;
        const int64_t clipSampleCount = static_cast<int64_t>(
            prepared.audio.samples.size() / m_channelCount);
        const int64_t normalizedSourceStart =
            timeStretchCacheSampleForSourceSample(
                sourceStartSample, prepared.precomputedTimeStretchSpeed);
        const int64_t normalizedSourceEndExclusive =
            timeStretchCacheEndSampleForSourceEndSample(
                sourceEndSampleExclusive, prepared.precomputedTimeStretchSpeed);
        const int64_t localSourceStart =
            normalizedSourceStart - prepared.audio.sourceStartSample;
        const int64_t localSourceEndExclusive =
            normalizedSourceEndExclusive - prepared.audio.sourceStartSample;
        const int64_t clampedStart =
            qBound<int64_t>(0, localSourceStart, clipSampleCount);
        const int64_t clampedEndExclusive = qBound<int64_t>(
            clampedStart, localSourceEndExclusive, clipSampleCount);
        float transcriptPeak = 0.0f;
        for (int64_t sample = clampedStart; sample < clampedEndExclusive;
             ++sample) {
          const int index = static_cast<int>(sample * m_channelCount);
          transcriptPeak =
              qMax(transcriptPeak, std::abs(prepared.audio.samples[index]));
          transcriptPeak =
              qMax(transcriptPeak, std::abs(prepared.audio.samples[index + 1]));
        }
        if (transcriptPeak <= 0.000001f) {
          continue;
        }

        PreparedClipAudio::TranscriptNormalizeSegment segment;
        segment.startSample = overlapStart;
        segment.endSampleExclusive = overlapEndExclusive;
        segment.gain = qMin(kMaxTranscriptNormalizeGain,
                            kTranscriptNormalizeTargetLinear / transcriptPeak);
        prepared.transcriptNormalizeSegments.push_back(segment);
      }
      if (!prepared.transcriptNormalizeSegments.isEmpty()) {
        std::sort(prepared.transcriptNormalizeSegments.begin(),
                  prepared.transcriptNormalizeSegments.end(),
                  [](const PreparedClipAudio::TranscriptNormalizeSegment &a,
                     const PreparedClipAudio::TranscriptNormalizeSegment &b) {
                    return a.startSample < b.startSample;
                  });
      }
    }
  }

  auto transcriptNormalizeGainAtSample =
      [](const PreparedClipAudio &prepared,
         int64_t timelineSamplePos) -> float {
    if (prepared.transcriptNormalizeSegments.isEmpty()) {
      return 1.0f;
    }
    constexpr int64_t kTransitionSamples = 480;       // 10 ms at 48 kHz
    constexpr int64_t kInterWordBridgeSamples = 5760; // 120 ms at 48 kHz
    const auto &segments = prepared.transcriptNormalizeSegments;
    const auto it = std::upper_bound(
        segments.begin(), segments.end(), timelineSamplePos,
        [](int64_t sample,
           const PreparedClipAudio::TranscriptNormalizeSegment &segment) {
          return sample < segment.startSample;
        });

    int index = -1;
    if (it != segments.begin()) {
      const int candidateIndex =
          static_cast<int>(std::distance(segments.begin(), it - 1));
      const auto &candidate = segments[static_cast<qsizetype>(candidateIndex)];
      if (timelineSamplePos < candidate.endSampleExclusive) {
        index = candidateIndex;
      }
    }
    if (index < 0) {
      const int nextIndex =
          static_cast<int>(std::distance(segments.begin(), it));
      const int prevIndex = nextIndex - 1;
      if (prevIndex >= 0 && nextIndex < segments.size()) {
        const auto &prev = segments[static_cast<qsizetype>(prevIndex)];
        const auto &next = segments[static_cast<qsizetype>(nextIndex)];
        const int64_t gapStart = prev.endSampleExclusive;
        const int64_t gapEnd = next.startSample;
        const int64_t gapLen = gapEnd - gapStart;
        if (timelineSamplePos >= gapStart && timelineSamplePos < gapEnd &&
            gapLen > 0 && gapLen <= kInterWordBridgeSamples) {
          const float t = static_cast<float>(timelineSamplePos - gapStart) /
                          static_cast<float>(gapLen);
          return prev.gain + ((next.gain - prev.gain) * qBound(0.0f, t, 1.0f));
        }
      }
      return 1.0f;
    }

    const auto &current = segments[static_cast<qsizetype>(index)];
    const float currentGain = current.gain;

    float gain = currentGain;
    const float previousGain =
        index > 0 ? segments[static_cast<qsizetype>(index - 1)].gain : 1.0f;
    const float nextGain =
        (index + 1) < segments.size()
            ? segments[static_cast<qsizetype>(index + 1)].gain
            : 1.0f;

    const int64_t startFadeLen = qMin<int64_t>(
        kTransitionSamples,
        qMax<int64_t>(1, current.endSampleExclusive - current.startSample));
    if (timelineSamplePos < current.startSample + startFadeLen) {
      const float t =
          static_cast<float>(timelineSamplePos - current.startSample) /
          static_cast<float>(startFadeLen);
      gain =
          previousGain + ((currentGain - previousGain) * qBound(0.0f, t, 1.0f));
    }

    const int64_t endFadeLen = qMin<int64_t>(
        kTransitionSamples,
        qMax<int64_t>(1, current.endSampleExclusive - current.startSample));
    if (timelineSamplePos >= current.endSampleExclusive - endFadeLen) {
      const float t =
          static_cast<float>(timelineSamplePos -
                             (current.endSampleExclusive - endFadeLen)) /
          static_cast<float>(endFadeLen);
      const float endGain =
          currentGain + ((nextGain - currentGain) * qBound(0.0f, t, 1.0f));
      gain = (timelineSamplePos < current.startSample + startFadeLen)
                 ? (0.5f * (gain + endGain))
                 : endGain;
    }

    return gain;
  };
  auto sourceSampleAtTimelineSample =
      [&context](const PreparedClipAudio &prepared,
                 int64_t timelineSamplePos) -> int64_t {
    if (prepared.linearSourceMapping) {
      const int64_t localTimelineSample =
          qMax<int64_t>(0, timelineSamplePos - prepared.clipStartSample);
      const int64_t sourceOffset =
          (localTimelineSample * prepared.playbackRateScaled) / 1000;
      return qMax<int64_t>(0,
                           qMin<int64_t>(prepared.sourceInSample + sourceOffset,
                                         prepared.maxSourceSample));
    }
    return sourceSampleForClipAtTimelineSample(
        *prepared.clip, timelineSamplePos, context.renderSyncMarkers);
  };

  int framesWithActiveClip = 0;
  int framesInputOutOfRange = 0;
  int framesSpeechGainZero = 0;
  int framesClipGainZero = 0;
  int framesSourceNonzero = 0;
  float sourcePeak = 0.0f;
  float primaryGainPeak = 0.0f;

  for (int outFrame = 0; outFrame < frames; ++outFrame) {
    const int64_t timelineOffset = static_cast<int64_t>(
        std::floor(static_cast<qreal>(outFrame) * clampedTimelineRate));
    const int64_t timelineSamplePos = chunkStartSample + timelineOffset;
    const int outIndex = outFrame * m_channelCount;
    bool frameHadActiveClip = false;
    bool frameHadReadyContribution = false;
    bool frameInputOutOfRange = false;
    bool frameSpeechGainZero = false;
    bool frameClipGainZero = false;
    bool frameSourceNonzero = false;

    for (PreparedClipAudio &prepared : preparedClips) {
      const TimelineClip &clip = *prepared.clip;
      const AudioClipCacheEntry &audio = prepared.audio;
      if (timelineSamplePos < prepared.clipStartSample ||
          timelineSamplePos >= prepared.clipEndSample) {
        continue;
      }
      frameHadActiveClip = true;

      const int64_t sourceFrame =
          sourceSampleAtTimelineSample(prepared, timelineSamplePos);
      int64_t inFrame = sourceFrame;
      inFrame = timeStretchCacheSampleForSourceSample(
          inFrame, prepared.precomputedTimeStretchSpeed);
      const int64_t localInFrame = inFrame - audio.sourceStartSample;
      if (localInFrame < 0 ||
          localInFrame >= (audio.samples.size() / m_channelCount)) {
        // Starved clip: drop it for this chunk and decode in the background.
        // Ready clips keep playing; the chunk blocks only if no active clip
        // can contribute at this frame (checked after the clip loop).
        frameInputOutOfRange = true;
        prepared.starvedThisChunk = true;
        m_lastMixOutOfRangeTimelineSample.store(timelineSamplePos,
                                                std::memory_order_release);
        m_lastMixOutOfRangeSourceSample.store(sourceFrame,
                                              std::memory_order_release);
        m_lastMixOutOfRangeNormalizedSample.store(inFrame,
                                                  std::memory_order_release);
        m_lastMixOutOfRangeAudioStartSample.store(audio.sourceStartSample,
                                                  std::memory_order_release);
        m_lastMixOutOfRangeAudioEndSample.store(
            audio.sourceStartSample +
                static_cast<int64_t>(audio.samples.size() /
                                     qMax(1, audio.channelCount)),
            std::memory_order_release);
        if (!prepared.starvationEnqueued) {
          prepared.starvationEnqueued = true;
          if (prepared.usingPrecomputedTimeStretch) {
            enqueueTimeStretchPrecomputeForPath(
                clipAudioPathForScheduling(clip), sourceFrame);
          } else {
            enqueuePreviewDecodeForPath(clipAudioPathForScheduling(clip),
                                        inFrame);
          }
        }
        continue;
      }
      frameHadReadyContribution = true;
      const int inIndex = static_cast<int>(localInFrame * m_channelCount);

      float primarySpeechGain = 1.0f;
      float secondarySpeechGain = 0.0f;
      int64_t secondaryTimelineSample = -1;
      if (!speechSampleRanges->isEmpty()) {
        const SpeechRangeBlend blend = calculateSpeechRangeBlend(
            timelineSamplePos, *speechSampleRanges,
            m_speechFilterFadeSamples.load(std::memory_order_acquire),
            static_cast<SpeechFilterFadeMode>(
                m_speechFilterFadeMode.load(std::memory_order_acquire)),
            m_speechFilterCurveStrength.load(std::memory_order_acquire),
            m_speechFilterRangeCrossfadeEnabled.load(
                std::memory_order_acquire));
        primarySpeechGain = blend.primaryGain;
        secondarySpeechGain = blend.secondaryGain;
        secondaryTimelineSample = blend.secondaryTimelineSample;
      }

      const float primaryClipGain = calculateClipCrossfadeGain(
          timelineSamplePos, clip, prepared.clipStartSample,
          prepared.clipEndSample,
          clip.fadeSamples > 0 ? clip.fadeSamples : m_defaultFadeSamples);
      const float transcriptNormalizeGain =
          transcriptNormalizeGainAtSample(prepared, timelineSamplePos);
      const float mixerGain = mixerGainForClip(clip, context.tracks, soloActive);
      const float primaryGain =
          primarySpeechGain * primaryClipGain * transcriptNormalizeGain * mixerGain;
      primaryGainPeak = qMax(primaryGainPeak, std::abs(primaryGain));
      if (primarySpeechGain <= 0.0f && secondarySpeechGain <= 0.0f) {
        frameSpeechGainZero = true;
      }
      if (primaryClipGain <= 0.0f) {
        frameClipGainZero = true;
      }
      const float sourceFramePeak = qMax(std::abs(audio.samples[inIndex]),
                                         std::abs(audio.samples[inIndex + 1]));
      sourcePeak = qMax(sourcePeak, sourceFramePeak);
      if (sourceFramePeak > 0.000001f) {
        frameSourceNonzero = true;
      }
      if (primaryGain > 0.0f) {
        output[outIndex] += audio.samples[inIndex] * primaryGain;
        output[outIndex + 1] += audio.samples[inIndex + 1] * primaryGain;
      }

      if (secondarySpeechGain > 0.0f && secondaryTimelineSample >= 0 &&
          spliceSecondaryTapWithinClip(secondaryTimelineSample,
                                       prepared.clipStartSample,
                                       prepared.clipEndSample)) {
        int64_t secondaryInFrame =
            sourceSampleAtTimelineSample(prepared, secondaryTimelineSample);
        secondaryInFrame = timeStretchCacheSampleForSourceSample(
            secondaryInFrame, prepared.precomputedTimeStretchSpeed);
        if (secondaryInFrame >= 0 &&
            secondaryInFrame - audio.sourceStartSample <
                (audio.samples.size() / m_channelCount)) {
          const int64_t localSecondaryInFrame =
              secondaryInFrame - audio.sourceStartSample;
          if (localSecondaryInFrame < 0) {
            continue;
          }
          const int secondaryInIndex =
              static_cast<int>(localSecondaryInFrame * m_channelCount);
          const float secondaryClipGain = calculateClipCrossfadeGain(
              secondaryTimelineSample, clip, prepared.clipStartSample,
              prepared.clipEndSample,
              clip.fadeSamples > 0 ? clip.fadeSamples : m_defaultFadeSamples);
          const float secondaryGain =
              secondarySpeechGain * secondaryClipGain * transcriptNormalizeGain * mixerGain;
          output[outIndex] += audio.samples[secondaryInIndex] * secondaryGain;
          output[outIndex + 1] +=
              audio.samples[secondaryInIndex + 1] * secondaryGain;
        }
      }
    }
    if (mixFrameMustBlock(frameHadActiveClip, frameHadReadyContribution,
                          frameInputOutOfRange)) {
      // Every active clip at this frame is starved: emitting silence here
      // would skip content instead of waiting for it, so block the chunk.
      storeBlockedMixDebug(SilentReasonInputOutOfRange);
      return false;
    }
    if (frameHadActiveClip) {
      ++framesWithActiveClip;
    }
    if (frameInputOutOfRange) {
      ++framesInputOutOfRange;
    }
    if (frameSpeechGainZero) {
      ++framesSpeechGainZero;
    }
    if (frameClipGainZero) {
      ++framesClipGainZero;
    }
    if (frameSourceNonzero) {
      ++framesSourceNonzero;
    }
  }

  const bool amplifyEnabled = m_amplifyEnabled.load(std::memory_order_acquire);
  const qreal amplifyDb = m_amplifyDb.load(std::memory_order_acquire);
  const bool normalizeEnabled =
      m_normalizeEnabled.load(std::memory_order_acquire);
  const qreal normalizeTargetDb =
      m_normalizeTargetDb.load(std::memory_order_acquire);
  const bool selectiveNormalizeEnabled =
      m_selectiveNormalizeEnabled.load(std::memory_order_acquire);
  const qreal selectiveNormalizeMinSegmentSeconds =
      m_selectiveNormalizeMinSegmentSeconds.load(std::memory_order_acquire);
  const qreal selectiveNormalizePeakDb =
      m_selectiveNormalizePeakDb.load(std::memory_order_acquire);
  const int selectiveNormalizePasses =
      m_selectiveNormalizePasses.load(std::memory_order_acquire);
  const bool peakReductionEnabled =
      m_peakReductionEnabled.load(std::memory_order_acquire);
  const qreal peakThresholdDb =
      m_peakThresholdDb.load(std::memory_order_acquire);
  const bool limiterEnabled = m_limiterEnabled.load(std::memory_order_acquire);
  const qreal limiterThresholdDb =
      m_limiterThresholdDb.load(std::memory_order_acquire);
  const bool compressorEnabled =
      m_compressorEnabled.load(std::memory_order_acquire);
  const qreal compressorThresholdDb =
      m_compressorThresholdDb.load(std::memory_order_acquire);
  const qreal compressorRatio =
      m_compressorRatio.load(std::memory_order_acquire);
  const bool softClipEnabled =
      m_softClipEnabled.load(std::memory_order_acquire);
  const bool stereoToMonoEnabled =
      m_stereoToMonoEnabled.load(std::memory_order_acquire);

  jcut::audio::DynamicsSettingsCore sharedDynamics;
  sharedDynamics.amplifyEnabled = amplifyEnabled;
  sharedDynamics.amplifyDb = amplifyDb;
  sharedDynamics.normalizeEnabled = normalizeEnabled;
  sharedDynamics.normalizeTargetDb = normalizeTargetDb;
  sharedDynamics.selectiveNormalizeEnabled =
      selectiveNormalizeEnabled;
  sharedDynamics.selectiveNormalizeMinSegmentSeconds =
      selectiveNormalizeMinSegmentSeconds;
  sharedDynamics.selectiveNormalizePeakDb =
      selectiveNormalizePeakDb;
  sharedDynamics.selectiveNormalizePasses =
      selectiveNormalizePasses;
  sharedDynamics.peakReductionEnabled = peakReductionEnabled;
  sharedDynamics.peakThresholdDb = peakThresholdDb;
  sharedDynamics.limiterEnabled = limiterEnabled;
  sharedDynamics.limiterThresholdDb = limiterThresholdDb;
  sharedDynamics.compressorEnabled = compressorEnabled;
  sharedDynamics.compressorThresholdDb = compressorThresholdDb;
  sharedDynamics.compressorRatio = compressorRatio;
  sharedDynamics.softClipEnabled = softClipEnabled;
  sharedDynamics.stereoToMonoEnabled = stereoToMonoEnabled;
  jcut::audio::processAudioDynamicsCore(
      output,
      frames,
      m_channelCount,
      m_sampleRate,
      sharedDynamics);

#if 0
  auto dbToAmpLocal = [](float db) -> float {
    return std::pow(10.0f, db / 20.0f);
  };

  const float amplifyGain =
      amplifyEnabled ? dbToAmpLocal(static_cast<float>(amplifyDb)) : 1.0f;
  const float normalizeTargetLinear = dbToAmpLocal(
      std::clamp(static_cast<float>(normalizeTargetDb), -24.0f, 0.0f));
  const float selectiveThresholdLinear = dbToAmpLocal(
      std::clamp(static_cast<float>(selectiveNormalizePeakDb), -36.0f, 0.0f));
  const float peakLinear = dbToAmpLocal(
      std::clamp(static_cast<float>(peakThresholdDb), -24.0f, 0.0f));
  const float limiterLinear = dbToAmpLocal(
      std::clamp(static_cast<float>(limiterThresholdDb), -12.0f, 0.0f));
  const float compLinear = dbToAmpLocal(
      std::clamp(static_cast<float>(compressorThresholdDb), -30.0f, -1.0f));
  const float compRatio =
      std::clamp(static_cast<float>(compressorRatio), 1.0f, 20.0f);
  const int safeSelectivePasses = qBound(1, selectiveNormalizePasses, 8);
  const float safeMinSegmentSeconds = std::clamp(
      static_cast<float>(selectiveNormalizeMinSegmentSeconds), 0.1f, 30.0f);
  constexpr float kSelectiveTargetLinear = 0.95f;

  auto processSignedSample = [&](float sample) -> float {
    const float sign = sample < 0.0f ? -1.0f : 1.0f;
    float out = std::abs(sample) * amplifyGain;
    if (compressorEnabled && out > compLinear) {
      const float over = out - compLinear;
      out = compLinear + (over / compRatio);
    }
    if (peakReductionEnabled && out > peakLinear) {
      out = peakLinear + (out - peakLinear) * 0.35f;
    }
    if (softClipEnabled) {
      constexpr float kSoftClipDrive = 1.75f;
      constexpr float kSoftClipNorm = 1.0f / 0.94137555f; // tanh(1.75)
      out = std::tanh(out * kSoftClipDrive) * kSoftClipNorm;
    }
    if (limiterEnabled) {
      out = std::min(out, limiterLinear);
    }
    return std::clamp(sign * out, -1.0f, 1.0f);
  };

  for (int i = 0; i < frames * m_channelCount; ++i) {
    output[i] = processSignedSample(output[i]);
  }

  if (selectiveNormalizeEnabled && frames > 0) {
    constexpr int kAnalysisWindowFrames = 256;
    const int binCount = qMax(
        1,
        static_cast<int>(std::ceil(static_cast<float>(frames) /
                                   static_cast<float>(kAnalysisWindowFrames))));
    QVector<float> binPeaks(binCount, 0.0f);
    auto rebuildBinPeaks = [&]() {
      std::fill(binPeaks.begin(), binPeaks.end(), 0.0f);
      for (int f = 0; f < frames; ++f) {
        const int idx = f * m_channelCount;
        const float peak =
            qMax(std::abs(output[idx]), std::abs(output[idx + 1]));
        const int bin = qMin(binCount - 1, f / kAnalysisWindowFrames);
        binPeaks[bin] = qMax(binPeaks[bin], peak);
      }
    };
    rebuildBinPeaks();

    const int minBins =
        qMax(1, static_cast<int>(std::ceil(
                    (safeMinSegmentSeconds * static_cast<float>(m_sampleRate)) /
                    static_cast<float>(kAnalysisWindowFrames))));

    for (int pass = 0; pass < safeSelectivePasses; ++pass) {
      QVector<int> peaks;
      peaks.reserve(binCount / 2);
      for (int i = 0; i < binCount; ++i) {
        const float v = binPeaks[i];
        if (v < selectiveThresholdLinear) {
          continue;
        }
        const float left = (i > 0) ? binPeaks[i - 1] : v;
        const float right = (i + 1 < binCount) ? binPeaks[i + 1] : v;
        if (v >= left && v >= right) {
          peaks.push_back(i);
        }
      }
      if (binCount >= 2) {
        if (peaks.isEmpty() || peaks.first() != 0) {
          peaks.prepend(0);
        }
        if (peaks.last() != (binCount - 1)) {
          peaks.push_back(binCount - 1);
        }
      }
      if (peaks.size() < 2) {
        break;
      }

      for (int p = 0; p + 1 < peaks.size(); ++p) {
        const int startBin = peaks[p];
        const int endBinInclusive = peaks[p + 1];
        const int lenBins = (endBinInclusive - startBin + 1);
        if (lenBins < minBins) {
          continue;
        }
        float segmentPeak = 0.0f;
        bool hasAboveThreshold = false;
        for (int b = startBin; b <= endBinInclusive; ++b) {
          segmentPeak = qMax(segmentPeak, binPeaks[b]);
          if (binPeaks[b] >= selectiveThresholdLinear) {
            hasAboveThreshold = true;
          }
        }
        if (!hasAboveThreshold || segmentPeak <= 0.000001f) {
          continue;
        }
        const float gain = kSelectiveTargetLinear / segmentPeak;
        const int startFrame = startBin * kAnalysisWindowFrames;
        const int endFrameExclusive =
            qMin(frames, (endBinInclusive + 1) * kAnalysisWindowFrames);
        for (int f = startFrame; f < endFrameExclusive; ++f) {
          const int idx = f * m_channelCount;
          output[idx] = std::clamp(output[idx] * gain, -1.0f, 1.0f);
          output[idx + 1] = std::clamp(output[idx + 1] * gain, -1.0f, 1.0f);
        }
      }
      rebuildBinPeaks();
    }
  }

  if (normalizeEnabled) {
    float postPeak = 0.0f;
    for (int i = 0; i < frames * m_channelCount; ++i) {
      postPeak = qMax(postPeak, std::abs(output[i]));
    }
    if (postPeak > 0.000001f) {
      const float normalizeGain = normalizeTargetLinear / postPeak;
      for (int i = 0; i < frames * m_channelCount; ++i) {
        output[i] = std::clamp(output[i] * normalizeGain, -1.0f, 1.0f);
      }
    }
  }

  if (stereoToMonoEnabled && m_channelCount > 1) {
    for (int frame = 0; frame < frames; ++frame) {
      const int frameOffset = frame * m_channelCount;
      float mono = 0.0f;
      for (int channel = 0; channel < m_channelCount; ++channel) {
        mono += output[frameOffset + channel];
      }
      mono /= static_cast<float>(m_channelCount);
      for (int channel = 0; channel < m_channelCount; ++channel) {
        output[frameOffset + channel] = mono;
      }
    }
  }
#endif

  const float masterGain =
      context.muted ? 0.0f : static_cast<float>(context.volume);
  double sumSquares = 0.0;
  float peak = 0.0f;
  int nonzeroSamples = 0;
  int outputNonzeroFrames = 0;
  for (int i = 0; i < frames * m_channelCount; ++i) {
    output[i] = qBound(-1.0f, output[i] * masterGain, 1.0f);
    const float absSample = std::abs(output[i]);
    peak = qMax(peak, absSample);
    sumSquares +=
        static_cast<double>(output[i]) * static_cast<double>(output[i]);
    if (absSample > 0.000001f) {
      ++nonzeroSamples;
    }
  }
  for (int outFrame = 0; outFrame < frames; ++outFrame) {
    const int outIndex = outFrame * m_channelCount;
    if (std::abs(output[outIndex]) > 0.000001f ||
        std::abs(output[outIndex + 1]) > 0.000001f) {
      ++outputNonzeroFrames;
    }
  }
  const int sampleCount = qMax(1, frames * m_channelCount);
  const float rms = static_cast<float>(
      std::sqrt(sumSquares / static_cast<double>(sampleCount)));
  int silentReason = SilentReasonNone;
  if (nonzeroSamples == 0) {
    if (masterGain <= 0.0f) {
      silentReason = SilentReasonMuted;
    } else if (preparedClips.isEmpty()) {
      silentReason = SilentReasonNoPreparedClips;
    } else if (framesWithActiveClip == 0) {
      silentReason = SilentReasonNoActiveClipInChunk;
    } else if (framesInputOutOfRange >= framesWithActiveClip) {
      silentReason = SilentReasonInputOutOfRange;
    } else if (framesSpeechGainZero >= framesWithActiveClip) {
      silentReason = SilentReasonSpeechGainZero;
    } else if (framesClipGainZero >= framesWithActiveClip) {
      silentReason = SilentReasonClipGainZero;
    } else if (framesSourceNonzero == 0) {
      silentReason = SilentReasonSourceSamplesZero;
    } else {
      silentReason = SilentReasonOutputBelowThreshold;
    }
  }
  int starvedClipCount = 0;
  QString starvedClipPath;
  for (const PreparedClipAudio &prepared : preparedClips) {
    if (prepared.starvedThisChunk) {
      ++starvedClipCount;
      if (starvedClipPath.isEmpty()) {
        starvedClipPath = clipAudioPathForScheduling(*prepared.clip);
      }
    }
  }
  m_lastMixStarvedClipCount.store(starvedClipCount,
                                  std::memory_order_release);
  if (starvedClipCount > 0) {
    m_mixDegradedChunkCount.fetch_add(1, std::memory_order_relaxed);
    std::lock_guard<std::mutex> lock(m_stateMutex);
    m_lastMixStarvedClipPath = starvedClipPath;
  }
  int64_t firstClipStart = 0;
  int64_t firstClipEnd = 0;
  int64_t firstAudioStart = 0;
  int64_t firstAudioFrameCount = 0;
  int64_t firstLocalSampleAtChunkStart = 0;
  if (!preparedClips.isEmpty()) {
    const PreparedClipAudio &first = preparedClips.first();
    firstClipStart = first.clipStartSample;
    firstClipEnd = first.clipEndSample;
    firstAudioStart = first.audio.sourceStartSample;
    firstAudioFrameCount = first.audio.samples.size() / m_channelCount;
    int64_t firstInFrame = sourceSampleForClipAtTimelineSample(
        *first.clip,
        qBound<int64_t>(first.clipStartSample, chunkStartSample,
                        first.clipEndSample - 1),
        context.renderSyncMarkers);
    firstInFrame = timeStretchCacheSampleForSourceSample(
        firstInFrame, first.precomputedTimeStretchSpeed);
    firstLocalSampleAtChunkStart = firstInFrame - first.audio.sourceStartSample;
  }
  m_lastMixPreparedClipCount.store(preparedClips.size(),
                                   std::memory_order_release);
  m_lastMixCacheHitCount.store(cacheHitCount, std::memory_order_release);
  m_lastMixCacheMissCount.store(cacheMissCount, std::memory_order_release);
  m_lastMixInvalidAudioCount.store(invalidAudioCount,
                                   std::memory_order_release);
  m_lastMixPeakPermille.store(qRound(peak * 1000.0f),
                              std::memory_order_release);
  m_lastMixRmsPermille.store(qRound(rms * 1000.0f), std::memory_order_release);
  m_lastMixNonzeroSampleCount.store(nonzeroSamples, std::memory_order_release);
  m_lastMixChunkStartSample.store(chunkStartSample, std::memory_order_release);
  m_lastMixChunkEndSample.store(
      chunkStartSample +
          static_cast<int64_t>(std::ceil(frames * clampedTimelineRate)),
      std::memory_order_release);
  m_lastMixFramesWithActiveClip.store(framesWithActiveClip,
                                      std::memory_order_release);
  m_lastMixFramesInputOutOfRange.store(framesInputOutOfRange,
                                       std::memory_order_release);
  m_lastMixFramesSpeechGainZero.store(framesSpeechGainZero,
                                      std::memory_order_release);
  m_lastMixFramesClipGainZero.store(framesClipGainZero,
                                    std::memory_order_release);
  m_lastMixFramesSourceNonzero.store(framesSourceNonzero,
                                     std::memory_order_release);
  m_lastMixFramesOutputNonzero.store(outputNonzeroFrames,
                                     std::memory_order_release);
  m_lastMixSourcePeakPermille.store(qRound(sourcePeak * 1000.0f),
                                    std::memory_order_release);
  m_lastMixPrimaryGainPeakPermille.store(qRound(primaryGainPeak * 1000.0f),
                                         std::memory_order_release);
  m_lastMixTimeStretchSpeedPermille.store(qRound(timeStretchSpeed * 1000.0),
                                          std::memory_order_release);
  m_lastMixFirstClipStartSample.store(firstClipStart,
                                      std::memory_order_release);
  m_lastMixFirstClipEndSample.store(firstClipEnd, std::memory_order_release);
  m_lastMixFirstAudioStartSample.store(firstAudioStart,
                                       std::memory_order_release);
  m_lastMixFirstAudioFrameCount.store(firstAudioFrameCount,
                                      std::memory_order_release);
  m_lastMixFirstLocalSampleAtChunkStart.store(firstLocalSampleAtChunkStart,
                                              std::memory_order_release);
  m_lastMixSilentReason.store(silentReason, std::memory_order_release);
  return true;
}

void AudioEngine::decodeLoop() {
  while (true) {
    DecodeTask nextTask;
    {
      std::unique_lock<std::mutex> lock(m_stateMutex);
      m_decodeCondition.wait(lock, [this]() {
        return !m_running ||
               (!m_backgroundDecodeSuppressed && !m_pendingDecodePaths.empty());
      });
      if (!m_running) {
        break;
      }
      nextTask = m_pendingDecodePaths.front();
      m_pendingDecodePaths.pop_front();
      m_pendingDecodeSet.remove(nextTask.key);
      m_activeDecodeFullDecode.insert(nextTask.key, nextTask.fullDecode);
    }

    const QString decodePath = editor::audio::pathFromSourceKey(nextTask.key);
    const PlaybackAudioWarpMode activeWarpMode =
        static_cast<PlaybackAudioWarpMode>(
            m_playbackWarpMode.load(std::memory_order_acquire));
    const int activeTimeStretchSpeedKey =
        nextTask.precomputeTimeStretch
            ? precomputedTimeStretchSpeedKey(
                  m_playbackRate.load(std::memory_order_acquire),
                  activeWarpMode)
            : 0;
    if (nextTask.precomputeTimeStretch) {
      (void)markTimeStretchJobForSourceGeneration(
          nextTask.key, activeTimeStretchSpeedKey, TimeStretchJobDecoding, 0.0,
          nextTask.sourceGeneration);
    }
    const int decodeStreamIndex =
        editor::audio::streamIndexFromSourceKey(nextTask.key);
    AudioClipCacheEntry decoded = decodeClipAudio(
        decodePath,
        nextTask.fullDecode
            ? -1
            : (nextTask.precomputeTimeStretch ? kTimeStretchPreviewDecodeFrames
                                              : kPreviewDecodeFrames),
        nextTask.fullDecode ? 0 : nextTask.sourceStartSample,
        decodeStreamIndex);
    QHash<int, AudioClipCacheEntry> warpedBySpeed;
    if (sourceGenerationCurrent(nextTask.sourceGeneration)) {
      warpedBySpeed = buildPrecomputedTimeStretchEntries(
          nextTask.key, decoded, nextTask.precomputeTimeStretch,
          nextTask.sourceGeneration);
    }

    {
      std::lock_guard<std::mutex> lock(m_stateMutex);
      const bool staleSourceGeneration =
          nextTask.sourceGeneration != m_sourceGeneration;
      const bool fullDecodeRequestedWhileActive =
          m_fullDecodeRequestedWhileActive.remove(nextTask.key) > 0;
      const bool timeStretchRequestedWhileActive =
          m_timeStretchPrecomputeRequestedWhileActive.contains(nextTask.key);
      const int64_t activeTimeStretchSourceStart =
          m_timeStretchPrecomputeRequestedWhileActive.take(nextTask.key);
      m_activeDecodeFullDecode.remove(nextTask.key);
      (void)recordDecodeResultLocked(nextTask.key, nextTask.sourceGeneration,
                                     decoded.valid);
      if (!staleSourceGeneration && decoded.valid) {
        m_audioCache.insert(nextTask.key, decoded);
        if (!warpedBySpeed.isEmpty()) {
          insertTimeStretchSegmentsLocked(nextTask.key,
                                          std::move(warpedBySpeed));
          m_timeStretchPrecomputeBlocked.store(false,
                                               std::memory_order_release);
        }
      }
      if (staleSourceGeneration) {
        // invalidateAudioSourceCaches() cannot queue a duplicate while this
        // path is active. Re-evaluate the current timeline once the stale
        // worker has relinquished the path.
        scheduleDecodesLocked(m_timelineClips);
        prioritizeDecodesNearSampleLocked(m_timelineSampleCursor);
      } else if (fullDecodeRequestedWhileActive && !nextTask.fullDecode) {
        enqueueDecodePathLocked(nextTask.key, true, true, true, 0, true);
      } else if (timeStretchRequestedWhileActive &&
                 !nextTask.precomputeTimeStretch) {
        enqueueDecodePathLocked(
            nextTask.key, true, nextTask.fullDecode, true,
            nextTask.fullDecode ? 0 : activeTimeStretchSourceStart, true);
      }
    }
    if (!nextTask.fullDecode && decoded.valid) {
      m_decodeCondition.notify_one();
    }
    m_stateCondition.notify_all();
  }
}

bool AudioEngine::commitMixedChunk(uint64_t generation,
                                   const int16_t *samples,
                                   size_t sampleCount,
                                   int64_t chunkEndSample) {
  std::lock_guard<std::mutex> lock(m_stateMutex);
  if (!m_playing || generation != m_mixGeneration) {
    return false;
  }
  m_ringBuffer.write(samples, sampleCount);
  m_ringBufferEndSample.store(chunkEndSample, std::memory_order_release);
  return true;
}

void AudioEngine::mixLoop() {
  QVector<float> mixBuffer(m_periodFrames * m_channelCount);
  QVector<int16_t> pcmBuffer(m_periodFrames * m_channelCount);

  while (true) {
    // Wait until playing
    {
      std::unique_lock<std::mutex> lock(m_stateMutex);
      m_stateCondition.wait(lock, [this]() { return !m_running || m_playing; });
      if (!m_running) {
        break;
      }
    }

    // Wait until ring buffer needs more data
    {
      std::unique_lock<std::mutex> lock(m_mixMutex);
      m_mixCondition.wait_for(lock, std::chrono::milliseconds(5), [this]() {
        return !m_running || !m_playing ||
               m_ringBuffer.available() <
                   static_cast<size_t>(m_mixLowWaterSamples);
      });
      if (!m_running) {
        break;
      }
      if (!m_playing) {
        continue;
      }
      if (m_ringBuffer.available() >=
          static_cast<size_t>(m_mixLowWaterSamples)) {
        continue;
      }
    }

    MixContext context;
    int64_t chunkStartSample = 0;
    qreal playbackRate = 1.0;
    qreal driftRetimeRate = 1.0;
    uint64_t generation = 0;
    {
      std::lock_guard<std::mutex> lock(m_stateMutex);
      if (!m_playing) {
        continue;
      }
      context.clips = m_timelineClips;
      context.tracks = m_timelineTracks;
      context.exportRanges = exportRangesCopy();
      context.speechSampleRanges.reserve(context.exportRanges.size());
      for (const ExportRangeSegment &range : std::as_const(context.exportRanges)) {
        const int64_t startSample = timelineFrameToSamples(range.startFrame);
        const int64_t endSampleExclusive = timelineFrameToSamples(range.endFrame + 1);
        if (endSampleExclusive > startSample) {
          context.speechSampleRanges.push_back(
              SpeechSampleRange{startSample, endSampleExclusive});
        }
      }
      context.renderSyncMarkers = m_renderSyncMarkers;
      generation = m_mixGeneration;
      playbackRate = qBound<qreal>(
          0.1, m_playbackRate.load(std::memory_order_acquire), 3.0);
      driftRetimeRate = qBound<qreal>(
          0.92, m_playbackDriftRetimeRate.load(std::memory_order_acquire),
          1.08);
      const qreal chunkTimelineDuration =
          playbackRate * driftRetimeRate * static_cast<qreal>(m_periodFrames);
      const int64_t timelineStep = qMax<int64_t>(
          1, static_cast<int64_t>(std::llround(chunkTimelineDuration)));
      chunkStartSample = nextPlayableSampleAtOrAfter(m_timelineSampleCursor,
                                                     context.exportRanges);
      m_timelineSampleCursor = chunkStartSample + timelineStep;
      context.muted = m_muted;
      context.volume = m_volume;
    }

    const qreal timelineRate = playbackRate * driftRetimeRate;
    if (!mixChunk(context, mixBuffer.data(), m_periodFrames, chunkStartSample,
                  playbackRate, timelineRate)) {
      std::lock_guard<std::mutex> lock(m_stateMutex);
      if (m_playing && generation == m_mixGeneration) {
        m_timelineSampleCursor = chunkStartSample;
        m_ringBufferEndSample.store(chunkStartSample,
                                    std::memory_order_release);
      }
      continue;
    }

    for (int i = 0; i < pcmBuffer.size(); ++i) {
      pcmBuffer[i] = static_cast<int16_t>(mixBuffer[i] * 32767.0f);
    }

    const qreal chunkTimelineDuration =
        timelineRate * static_cast<qreal>(m_periodFrames);
    const int64_t timelineStep = qMax<int64_t>(
        1, static_cast<int64_t>(std::llround(chunkTimelineDuration)));
    commitMixedChunk(generation, pcmBuffer.constData(),
                     static_cast<size_t>(pcmBuffer.size()),
                     chunkStartSample + timelineStep);
  }
}
