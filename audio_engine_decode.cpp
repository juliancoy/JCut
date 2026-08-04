#include "audio_engine.h"
#include "audio_engine_internal.h"
#include "audio_speech_harmonic_isolator.h"
#include "audio_clip_fade.h"
#include "audio_source_key.h"
#include "decoder_ffmpeg_utils.h"
#include "ffmpeg_compat.h"

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

using namespace jcut::audio_internal;

AudioEngine::SpeechRangeBlend AudioEngine::calculateSpeechRangeBlend(
    int64_t samplePos, const QVector<SpeechSampleRange> &ranges,
    int fadeSamples, SpeechFilterFadeMode fadeMode, qreal curveStrength) const {
  return editor::speech::rangeBlendAtSample(
      samplePos,
      ranges,
      fadeSamples,
      fadeMode,
      curveStrength);
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
