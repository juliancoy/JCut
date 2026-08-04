#include "audio_engine.h"
#include "audio_engine_internal.h"
#include "audio_clip_fade.h"
#include "audio_mix_readiness.h"
#include "audio_source_key.h"
#include "debug_controls.h"

#include <QDateTime>
#include <QDebug>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>
#include <limits>

using namespace jcut::audio_internal;

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
    jcut::audio::DynamicsSettingsCore dynamics;
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
    prepared.dynamics = audioDynamicsForClip(clip);
    if (audioDynamicsProcessingEnabled(prepared.dynamics) &&
        !prepared.audio.samples.isEmpty()) {
      jcut::audio::processAudioDynamicsCore(
          prepared.audio.samples.data(),
          static_cast<int>(prepared.audio.samples.size() /
                           qMax(1, prepared.audio.channelCount)),
          prepared.audio.channelCount,
          prepared.audio.sampleRate > 0 ? prepared.audio.sampleRate
                                        : m_sampleRate,
          prepared.dynamics);
    }
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
  if (!transcriptNormalizeRanges.isEmpty()) {
    constexpr float kTranscriptNormalizeTargetLinear = 0.95f;
    constexpr float kMaxTranscriptNormalizeGain = 2.5f;
    for (PreparedClipAudio &prepared : preparedClips) {
      if (!prepared.dynamics.transcriptNormalizeEnabled) {
        continue;
      }
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
            m_speechFilterCurveStrength.load(std::memory_order_acquire));
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

bool AudioEngine::outputStreamCanStart(bool playing,
                                       bool outputStartPending,
                                       size_t availableSamples,
                                       size_t primeTargetSamples) {
  return playing &&
         outputStartPending &&
         primeTargetSamples > 0 &&
         availableSamples >= primeTargetSamples;
}

size_t AudioEngine::outputPrimeTargetSamples(
    int periodFrames,
    int64_t streamLatencyFrames) {
  const size_t safePeriodFrames =
      static_cast<size_t>(qMax(1, periodFrames));
  const size_t safeLatencyFrames =
      static_cast<size_t>(qMax<int64_t>(0, streamLatencyFrames));
  const size_t steadyLowWaterFrames =
      static_cast<size_t>(m_mixLowWaterSamples / qMax(1, m_channelCount));
  const size_t requestedFrames = qMax(
      steadyLowWaterFrames,
      safeLatencyFrames + (2 * safePeriodFrames));
  const size_t capacityFrames =
      AudioRingBuffer::kCapacity / static_cast<size_t>(m_channelCount);
  const size_t maximumPrimeFrames =
      capacityFrames > safePeriodFrames
          ? capacityFrames - safePeriodFrames
          : safePeriodFrames;
  const size_t targetFrames = qBound(
      safePeriodFrames,
      requestedFrames,
      maximumPrimeFrames);
  return targetFrames * static_cast<size_t>(m_channelCount);
}

bool AudioEngine::outputPrimeCapacitySufficient(
    int periodFrames,
    int64_t streamLatencyFrames) {
  const size_t safePeriodFrames =
      static_cast<size_t>(qMax(1, periodFrames));
  const size_t safeLatencyFrames =
      static_cast<size_t>(qMax<int64_t>(0, streamLatencyFrames));
  const size_t steadyLowWaterFrames =
      static_cast<size_t>(m_mixLowWaterSamples / qMax(1, m_channelCount));
  const size_t requestedFrames = qMax(
      steadyLowWaterFrames,
      safeLatencyFrames + (2 * safePeriodFrames));
  const size_t capacityFrames =
      AudioRingBuffer::kCapacity / static_cast<size_t>(m_channelCount);
  return requestedFrames + safePeriodFrames <= capacityFrames;
}

bool AudioEngine::outputPrimeNeedsRebase(
    int64_t queuedTimelineSample,
    int64_t authoritativeTimelineSample,
    int64_t deadbandSamples) {
  const int64_t safeDeadband = qMax<int64_t>(0, deadbandSamples);
  const int64_t delta =
      authoritativeTimelineSample - queuedTimelineSample;
  return delta > safeDeadband || delta < -safeDeadband;
}

bool AudioEngine::rebasePendingOutputToAuthoritativeLocked() {
  const int64_t queuedTimelineSample =
      m_audioClockSample.load(std::memory_order_acquire);
  const int64_t authoritativeTimelineSample =
      m_authoritativeTransportSample.load(std::memory_order_acquire);
  if (!m_outputPrimeCanRebase ||
      !outputPrimeNeedsRebase(
          queuedTimelineSample,
          authoritativeTimelineSample,
          kOutputPrimeRebaseDeadbandSamples)) {
    return false;
  }

  // Priming can wait on source decode while the system-clock transport keeps
  // advancing. Discard that stale prime exactly once; the decoded source is
  // now warm, so the replacement prime is bounded and cannot livelock.
  m_outputPrimeCanRebase = false;
  ++m_mixGeneration;
  m_timelineSampleCursor = authoritativeTimelineSample;
  m_audioClockSample.store(authoritativeTimelineSample,
                           std::memory_order_release);
  m_lastReportedCurrentSample.store(authoritativeTimelineSample,
                                    std::memory_order_release);
  m_ringBufferEndSample.store(authoritativeTimelineSample,
                              std::memory_order_release);
  m_ringBuffer.clear();
  scheduleDecodesLocked(m_timelineClips);
  prioritizeDecodesNearSampleLocked(authoritativeTimelineSample);
  m_outputPrimeRebaseCount.fetch_add(1, std::memory_order_relaxed);
  m_lastOutputPrimeRebaseLagSamples.store(
      authoritativeTimelineSample - queuedTimelineSample,
      std::memory_order_release);
  return true;
}

void AudioEngine::startOutputStreamIfPrimed() {
  const size_t primeTargetSamples =
      m_outputPrimeTargetSamples.load(std::memory_order_acquire);
  if (!outputStreamCanStart(
          m_playing.load(std::memory_order_acquire),
          m_outputStartPending.load(std::memory_order_acquire),
          m_ringBuffer.available(),
          primeTargetSamples)) {
    return;
  }

  std::lock_guard<std::mutex> lock(m_stateMutex);
  if (!outputStreamCanStart(
          m_playing.load(std::memory_order_acquire),
          m_outputStartPending.load(std::memory_order_acquire),
          m_ringBuffer.available(),
          primeTargetSamples) ||
      !m_outputBackend ||
      !m_outputBackend->isOpen()) {
    return;
  }
  if (m_outputBackend->isRunning()) {
    m_outputStartPending.store(false, std::memory_order_release);
    m_outputPrimeCanRebase = false;
    return;
  }

  const int64_t outputStartSample =
      m_audioClockSample.load(std::memory_order_acquire);
  if (rebasePendingOutputToAuthoritativeLocked()) {
    m_stateCondition.notify_all();
    m_decodeCondition.notify_one();
    m_mixCondition.notify_all();
    return;
  }

  const bool started = m_outputBackend->start();
  m_outputStartPending.store(false, std::memory_order_release);
  m_outputPrimeCanRebase = false;
  const qint64 primeStartedMs =
      m_outputPrimeStartedMs.exchange(0, std::memory_order_acq_rel);
  if (primeStartedMs > 0) {
    m_lastOutputPrimeDurationMs.store(
        qMax<qint64>(
            0, QDateTime::currentMSecsSinceEpoch() - primeStartedMs),
        std::memory_order_release);
  }
  if (!started) {
    m_lastDeviceInfoError =
        QString::fromStdString(m_outputBackend->lastError());
    return;
  }
  m_lastOutputStartTimelineSample.store(outputStartSample,
                                        std::memory_order_release);
  m_lastOutputStartFeedbackSample.store(outputStartSample,
                                        std::memory_order_release);
  m_outputStartRevision.fetch_add(1, std::memory_order_release);
}

void AudioEngine::pauseOutputStreamForRefillLocked() {
  m_outputStartPending.store(false, std::memory_order_release);
  if (m_outputBackend &&
      m_outputBackend->isRunning() &&
      !m_outputBackend->supportsSeamlessReprime()) {
    if (!m_outputBackend->stop(false)) {
      m_lastDeviceInfoError =
          QString::fromStdString(m_outputBackend->lastError());
    }
  }
  m_outputStartPending.store(
      m_playing.load(std::memory_order_acquire),
      std::memory_order_release);
  m_outputPrimeCanRebase =
      m_playing.load(std::memory_order_acquire);
  m_outputPrimeStartedMs.store(
      m_outputPrimeCanRebase
          ? QDateTime::currentMSecsSinceEpoch()
          : 0,
      std::memory_order_release);
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
        const size_t fillTargetSamples =
            m_outputStartPending.load(std::memory_order_acquire)
                ? m_outputPrimeTargetSamples.load(std::memory_order_acquire)
                : static_cast<size_t>(m_mixLowWaterSamples);
        return !m_running || !m_playing ||
               m_ringBuffer.available() < fillTargetSamples;
      });
      if (!m_running) {
        break;
      }
      if (!m_playing) {
        continue;
      }
      const size_t fillTargetSamples =
          m_outputStartPending.load(std::memory_order_acquire)
              ? m_outputPrimeTargetSamples.load(std::memory_order_acquire)
              : static_cast<size_t>(m_mixLowWaterSamples);
      if (m_ringBuffer.available() >= fillTargetSamples) {
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
    if (commitMixedChunk(generation, pcmBuffer.constData(),
                         static_cast<size_t>(pcmBuffer.size()),
                         chunkStartSample + timelineStep)) {
      startOutputStreamIfPrimed();
    }
  }
}
