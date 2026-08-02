#include "audio_engine.h"
#include "audio_engine_internal.h"
#include "debug_controls.h"

#include <QDateTime>
#include <QFileInfo>
#include <QJsonObject>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <limits>

using namespace jcut::audio_internal;

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
  snapshot[QStringLiteral("output_start_pending")] =
      m_outputStartPending.load(std::memory_order_acquire);
  snapshot[QStringLiteral("output_prime_target_frames")] =
      static_cast<qint64>(
          m_outputPrimeTargetSamples.load(std::memory_order_acquire) /
          qMax(1, m_channelCount));
  snapshot[QStringLiteral("output_prime_capacity_sufficient")] =
      m_outputPrimeCapacitySufficient.load(std::memory_order_acquire);
  snapshot[QStringLiteral("output_stream_latency_frames_at_open")] =
      static_cast<qint64>(
          m_outputStreamLatencyFramesAtOpen.load(std::memory_order_acquire));
  snapshot[QStringLiteral("authoritative_transport_sample")] =
      static_cast<qint64>(
          m_authoritativeTransportSample.load(std::memory_order_acquire));
  snapshot[QStringLiteral("output_start_revision")] =
      static_cast<qint64>(
          m_outputStartRevision.load(std::memory_order_acquire));
  snapshot[QStringLiteral("last_output_start_timeline_sample")] =
      static_cast<qint64>(
          m_lastOutputStartTimelineSample.load(std::memory_order_acquire));
  snapshot[QStringLiteral("last_output_start_feedback_sample")] =
      static_cast<qint64>(
          m_lastOutputStartFeedbackSample.load(std::memory_order_acquire));
  const qint64 outputPrimeStartedMs =
      m_outputPrimeStartedMs.load(std::memory_order_acquire);
  snapshot[QStringLiteral("output_prime_elapsed_ms")] =
      m_outputStartPending.load(std::memory_order_acquire) &&
              outputPrimeStartedMs > 0
          ? qMax<qint64>(
                0,
                QDateTime::currentMSecsSinceEpoch() -
                    outputPrimeStartedMs)
          : 0;
  snapshot[QStringLiteral("last_output_prime_duration_ms")] =
      m_lastOutputPrimeDurationMs.load(std::memory_order_acquire);
  snapshot[QStringLiteral("output_prime_rebase_count")] =
      m_outputPrimeRebaseCount.load(std::memory_order_acquire);
  snapshot[QStringLiteral("last_output_prime_rebase_lag_samples")] =
      static_cast<qint64>(
          m_lastOutputPrimeRebaseLagSamples.load(std::memory_order_acquire));
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
      timeStretchGenerationSpeedKey > 1000 &&
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
  snapshot[QStringLiteral("backend_candidates")] = m_audioBackendCandidates;
  snapshot[QStringLiteral("selected_backend")] = m_selectedAudioBackend;
  snapshot[QStringLiteral("backend_selection_reason")] =
      m_audioBackendSelectionReason;
  if (!m_outputBackend) {
    snapshot[QStringLiteral("api")] = QStringLiteral("none");
    snapshot[QStringLiteral("device_count")] = 0;
    snapshot[QStringLiteral("stream_open")] = false;
    snapshot[QStringLiteral("stream_running")] = false;
    return snapshot;
  }

  snapshot[QStringLiteral("api")] =
      QString::fromStdString(m_outputBackend->info().apiName);
  snapshot[QStringLiteral("device_count")] = m_lastKnownDeviceCount;
  snapshot[QStringLiteral("stream_open")] = m_outputBackend->isOpen();
  snapshot[QStringLiteral("stream_running")] = m_outputBackend->isRunning();
  snapshot[QStringLiteral("connection_revision")] =
      static_cast<qint64>(m_outputBackend->connectionRevision());
  snapshot[QStringLiteral("backend_error")] =
      QString::fromStdString(m_outputBackend->lastError());
  const int64_t streamLatencyFrames = m_outputBackend->latencyFrames();
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
  if (m_outputBackend && m_outputBackend->isOpen()) {
    latencyFrames = m_outputBackend->latencyFrames();
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

void AudioEngine::setAuthoritativeTransportSample(int64_t sample) {
  const int64_t boundedSample = qMax<int64_t>(0, sample);
  m_authoritativeTransportSample.store(boundedSample,
                                       std::memory_order_release);
  if (!m_outputBackend ||
      !m_outputBackend->supportsSeamlessReprime()) {
    return;
  }
  const uint64_t backendRevision =
      m_outputBackend->connectionRevision();
  if (backendRevision == 0) {
    return;
  }
  const uint64_t observedRevision =
      m_lastObservedBackendConnectionRevision.exchange(
          backendRevision, std::memory_order_acq_rel);
  if (backendRevision == observedRevision) {
    return;
  }

  {
    std::lock_guard<std::mutex> lock(m_stateMutex);
    ++m_mixGeneration;
    m_timelineSampleCursor = boundedSample;
    m_audioClockSample.store(boundedSample, std::memory_order_release);
    m_lastReportedCurrentSample.store(boundedSample,
                                      std::memory_order_release);
    m_ringBufferEndSample.store(boundedSample,
                                std::memory_order_release);
    m_ringBuffer.clear();
    scheduleDecodesLocked(m_timelineClips);
    prioritizeDecodesNearSampleLocked(boundedSample);
  }
  m_stateCondition.notify_all();
  m_decodeCondition.notify_one();
  m_mixCondition.notify_all();
}

AudioEngine::AudioFollowerSnapshot AudioEngine::audioFollowerSnapshot() const {
  std::lock_guard<std::mutex> lock(m_stateMutex);
  AudioFollowerSnapshot snapshot;
  snapshot.available =
      m_initialized && m_outputBackend && m_outputBackend->isRunning();
  const int64_t submittedSample =
      m_audioClockSample.load(std::memory_order_acquire);
  const qreal playbackRate =
      qBound<qreal>(0.1, m_playbackRate.load(std::memory_order_acquire), 3.0);
  long latencyFrames = 0;
  if (snapshot.available && m_outputBackend->isOpen()) {
    latencyFrames = m_outputBackend->latencyFrames();
  }
  const int64_t latencyTimelineSamples = qMax<int64_t>(
      0, static_cast<int64_t>(std::llround(
             static_cast<long double>(qMax<long>(0, latencyFrames)) *
             static_cast<long double>(playbackRate))));
  snapshot.feedbackSample =
      qMax<int64_t>(0, submittedSample - latencyTimelineSamples);
  snapshot.outputStartRevision =
      m_outputStartRevision.load(std::memory_order_acquire);
  snapshot.outputStartTimelineSample =
      m_lastOutputStartTimelineSample.load(std::memory_order_acquire);
  snapshot.outputStartFeedbackSample =
      m_lastOutputStartFeedbackSample.load(std::memory_order_acquire);
  return snapshot;
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
