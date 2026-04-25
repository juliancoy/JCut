#pragma once

#include "editor_shared.h"
#include "ffmpeg_compat.h"

#include <QByteArray>
#include <QDateTime>
#include <QDebug>
#include <QFile>
#include <QHash>
#include <QSet>
#include <QVector>

#include <array>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstring>
#include <deque>
#include <limits>
#include <memory>
#include <mutex>
#include <thread>
#include <tuple>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/channel_layout.h>
#include <libavutil/opt.h>
#include <libavutil/samplefmt.h>
#include <libavutil/avutil.h>
#include <libswresample/swresample.h>
}

#include <RtAudio.h>

// Lock-free SPSC ring buffer for int16 audio samples.
// Single producer (mix thread) writes, single consumer (RtAudio callback) reads.
struct AudioRingBuffer {
    static constexpr size_t kCapacity = 32768; // power of 2

    size_t available() const {
        return m_writePos.load(std::memory_order_acquire) -
               m_readPos.load(std::memory_order_relaxed);
    }

    size_t space() const { return kCapacity - available(); }

    size_t write(const int16_t* data, size_t count) {
        const size_t avail = space();
        count = std::min(count, avail);
        const size_t wp = m_writePos.load(std::memory_order_relaxed);
        for (size_t i = 0; i < count; ++i)
            m_buffer[(wp + i) & (kCapacity - 1)] = data[i];
        m_writePos.store(wp + count, std::memory_order_release);
        return count;
    }

    size_t read(int16_t* data, size_t count) {
        const size_t avail = available();
        count = std::min(count, avail);
        const size_t rp = m_readPos.load(std::memory_order_relaxed);
        for (size_t i = 0; i < count; ++i)
            data[i] = m_buffer[(rp + i) & (kCapacity - 1)];
        m_readPos.store(rp + count, std::memory_order_release);
        return count;
    }

    void clear() {
        // Set readPos = writePos so the consumer sees an empty buffer.
        // This is safe even if the consumer is concurrently reading: it will
        // see available() == 0 on the next call. Resetting both to 0 would
        // race because the consumer could see writePos=0 with readPos still
        // at the old value, computing a negative (wrapped) available count.
        m_readPos.store(m_writePos.load(std::memory_order_acquire), std::memory_order_release);
    }

private:
    std::array<int16_t, kCapacity> m_buffer{};
    std::atomic<size_t> m_readPos{0};
    std::atomic<size_t> m_writePos{0};
};

class AudioEngine {
public:
    AudioEngine() = default;
    ~AudioEngine() { shutdown(); }

    void setTimelineClips(const QVector<TimelineClip>& clips) {
        {
            std::lock_guard<std::mutex> lock(m_stateMutex);
            m_timelineClips = clips;
            scheduleDecodesLocked(clips);
            prioritizeDecodesNearSampleLocked(m_timelineSampleCursor);
        }
        m_decodeCondition.notify_one();
    }

    void setExportRanges(const QVector<ExportRangeSegment>& ranges) {
        std::lock_guard<std::mutex> lock(m_exportRangesMutex);
        m_exportRanges = ranges;
    }

    void setRenderSyncMarkers(const QVector<RenderSyncMarker>& markers) {
        std::lock_guard<std::mutex> lock(m_stateMutex);
        m_renderSyncMarkers = markers;
    }

    void setSpeechFilterFadeSamples(int samples) {
        m_speechFilterFadeSamples.store(qMax(0, samples), std::memory_order_release);
    }

    void setSpeechFilterRangeCrossfadeEnabled(bool enabled) {
        m_speechFilterRangeCrossfadeEnabled.store(enabled, std::memory_order_release);
    }

    void setPlaybackWarpMode(PlaybackAudioWarpMode mode) {
        m_playbackWarpMode.store(static_cast<int>(mode), std::memory_order_release);
    }

    void setPlaybackRate(qreal rate) {
        const qreal clampedRate = qBound<qreal>(0.1, rate, 3.0);
        m_playbackRate.store(clampedRate, std::memory_order_release);
    }

    bool initialize() {
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
            m_rtaudio = std::make_unique<rt::audio::RtAudio>(rt::audio::RtAudio::LINUX_ALSA);
            created = true;
        } catch (const std::exception& e) {
            if (nowMs - m_lastAudioInitWarningMs >= kAudioInitWarningThrottleMs) {
                qWarning() << "RtAudio ALSA creation failed, falling back to default API:" << e.what();
                m_lastAudioInitWarningMs = nowMs;
            }
        }
#endif
        if (!created) {
            try {
                m_rtaudio = std::make_unique<rt::audio::RtAudio>();
                created = true;
            } catch (const std::exception& e) {
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
        } catch (const std::exception& e) {
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
        } catch (const std::exception& e) {
            m_lastDeviceInfoError = QString::fromUtf8(e.what());
            if (nowMs - m_lastAudioInitWarningMs >= kAudioInitWarningThrottleMs) {
                qWarning() << "RtAudio default output query failed:" << m_lastDeviceInfoError;
                m_lastAudioInitWarningMs = nowMs;
            }
        }
        params.nChannels = m_channelCount;

        unsigned int bufferFrames = m_periodFrames;
        auto err = m_rtaudio->openStream(
            &params, nullptr,
            rt::audio::RTAUDIO_SINT16, m_sampleRate, &bufferFrames,
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

    void shutdown() {
        {
            std::lock_guard<std::mutex> lock(m_stateMutex);
            if (!m_initialized) {
                return;
            }
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

    void setMuted(bool muted) {
        std::lock_guard<std::mutex> lock(m_stateMutex);
        m_muted = muted;
    }

    void setVolume(qreal volume) {
        std::lock_guard<std::mutex> lock(m_stateMutex);
        m_volume = qBound<qreal>(0.0, volume, 1.0);
    }

    bool muted() const {
        std::lock_guard<std::mutex> lock(m_stateMutex);
        return m_muted;
    }

    int volumePercent() const {
        std::lock_guard<std::mutex> lock(m_stateMutex);
        return qRound(m_volume * 100.0);
    }

    void start(int64_t startFrame) {
        if (!initialize()) {
            return;
        }
        {
            std::lock_guard<std::mutex> lock(m_stateMutex);
            m_timelineSampleCursor = timelineFrameToSamples(startFrame);
            m_audioClockSample.store(m_timelineSampleCursor, std::memory_order_release);
            m_ringBufferEndSample.store(m_timelineSampleCursor, std::memory_order_release);
            m_playing = true;
            scheduleDecodesLocked(m_timelineClips);
            prioritizeDecodesNearSampleLocked(m_timelineSampleCursor);
        }
        m_ringBuffer.clear();
        m_stateCondition.notify_all();
        m_decodeCondition.notify_one();
        m_mixCondition.notify_all();
        if (m_rtaudio && !m_rtaudio->isStreamRunning()) {
            m_rtaudio->startStream();
        }
    }

    void stop() {
        {
            std::lock_guard<std::mutex> lock(m_stateMutex);
            m_playing = false;
        }
        if (m_rtaudio && m_rtaudio->isStreamRunning()) {
            m_rtaudio->stopStream();
        }
        m_ringBuffer.clear();
        m_mixCondition.notify_all();
    }

    void seek(int64_t frame) {
        {
            std::lock_guard<std::mutex> lock(m_stateMutex);
            const int64_t sample = timelineFrameToSamples(frame);
            m_timelineSampleCursor = sample;
            m_audioClockSample.store(sample, std::memory_order_release);
            m_ringBufferEndSample.store(sample, std::memory_order_release);
            scheduleDecodesLocked(m_timelineClips);
            prioritizeDecodesNearSampleLocked(sample);
        }
        m_ringBuffer.clear();
        m_stateCondition.notify_all();
        m_decodeCondition.notify_one();
        m_mixCondition.notify_all();
    }

    bool hasPlayableAudio() const {
        std::lock_guard<std::mutex> lock(m_stateMutex);
        for (const TimelineClip& clip : m_timelineClips) {
            if (clipAudioPlaybackEnabled(clip)) {
                return true;
            }
        }
        return false;
    }

    bool audioClockAvailable() const {
        std::lock_guard<std::mutex> lock(m_stateMutex);
        return m_initialized && m_rtaudio && m_rtaudio->isStreamRunning();
    }

    QJsonObject profilingSnapshot() const {
        QJsonObject snapshot;
        snapshot[QStringLiteral("initialized")] = m_initialized;
        snapshot[QStringLiteral("running")] = m_running.load(std::memory_order_acquire);
        snapshot[QStringLiteral("playing")] = m_playing.load(std::memory_order_acquire);
        snapshot[QStringLiteral("has_playable_audio")] = hasPlayableAudio();
        snapshot[QStringLiteral("audio_clock_available")] = audioClockAvailable();
        snapshot[QStringLiteral("current_sample")] = static_cast<qint64>(currentSample());
        snapshot[QStringLiteral("current_frame")] = static_cast<qint64>(currentFrame());
        snapshot[QStringLiteral("ring_buffer_samples_available")] = static_cast<qint64>(m_ringBuffer.available());
        snapshot[QStringLiteral("ring_buffer_end_sample")] = static_cast<qint64>(m_ringBufferEndSample.load(std::memory_order_acquire));
        snapshot[QStringLiteral("audio_clock_sample")] = static_cast<qint64>(m_audioClockSample.load(std::memory_order_acquire));
        snapshot[QStringLiteral("timeline_sample_cursor")] = static_cast<qint64>(m_timelineSampleCursor);
        snapshot[QStringLiteral("underrun_count")] = m_underrunCount.load(std::memory_order_acquire);
        snapshot[QStringLiteral("sample_rate")] = m_sampleRate;
        snapshot[QStringLiteral("channel_count")] = m_channelCount;
        snapshot[QStringLiteral("period_frames")] = m_periodFrames;
        snapshot[QStringLiteral("playback_rate")] = m_playbackRate.load(std::memory_order_acquire);
        snapshot[QStringLiteral("playback_warp_mode")] =
            playbackAudioWarpModeToString(static_cast<PlaybackAudioWarpMode>(
                m_playbackWarpMode.load(std::memory_order_acquire)));

        std::lock_guard<std::mutex> lock(m_stateMutex);
        if (!m_rtaudio) {
            snapshot[QStringLiteral("api")] = QStringLiteral("none");
            snapshot[QStringLiteral("device_count")] = 0;
            snapshot[QStringLiteral("stream_open")] = false;
            snapshot[QStringLiteral("stream_running")] = false;
            return snapshot;
        }

        snapshot[QStringLiteral("api")] =
            QString::fromStdString(rt::audio::RtAudio::getApiName(m_rtaudio->getCurrentApi()));
        snapshot[QStringLiteral("device_count")] = m_lastKnownDeviceCount;
        snapshot[QStringLiteral("stream_open")] = m_rtaudio->isStreamOpen();
        snapshot[QStringLiteral("stream_running")] = m_rtaudio->isStreamRunning();
        snapshot[QStringLiteral("stream_latency_frames")] = static_cast<qint64>(m_rtaudio->getStreamLatency());
        snapshot[QStringLiteral("default_output_device_id")] =
            static_cast<qint64>(m_lastKnownDefaultOutputId);
        snapshot[QStringLiteral("default_output_device_valid")] = m_lastKnownDefaultOutputValid;
        snapshot[QStringLiteral("default_output_device_name")] = m_lastKnownDefaultOutputName;
        snapshot[QStringLiteral("default_output_channels")] =
            static_cast<qint64>(m_lastKnownDefaultOutputChannels);
        if (!m_lastDeviceInfoError.isEmpty()) {
            snapshot[QStringLiteral("device_info_error")] = m_lastDeviceInfoError;
        }

        return snapshot;
    }

    int64_t currentSample() const {
        const int64_t submitted = m_audioClockSample.load(std::memory_order_acquire);
        long latencyFrames = 0;
        if (m_rtaudio && m_rtaudio->isStreamOpen()) {
            latencyFrames = m_rtaudio->getStreamLatency();
        }
        return qMax<int64_t>(0, submitted - qMax<long>(0, latencyFrames));
    }

    int64_t currentFrame() const {
        return samplesToTimelineFrame(currentSample());
    }

private:
    struct DecodeTask {
        QString path;
        bool fullDecode = false;
    };

    struct AudioClipCacheEntry {
        QVector<float> samples;
        int sampleRate = 48000;
        int channelCount = 2;
        bool valid = false;
        bool fullyDecoded = false;
    };

    struct MixContext {
        QVector<TimelineClip> clips;
        QVector<ExportRangeSegment> exportRanges;
        QVector<RenderSyncMarker> renderSyncMarkers;
        bool muted = false;
        qreal volume = 0.8;
    };

    // --- RtAudio callback (called from OS audio thread) ---

    static int rtAudioCallback(void* outputBuffer, void* /*inputBuffer*/,
                               unsigned int nFrames, double /*streamTime*/,
                               rt::audio::RtAudioStreamStatus /*status*/, void* userData) {
        auto* engine = static_cast<AudioEngine*>(userData);
        auto* out = static_cast<int16_t*>(outputBuffer);
        const size_t samplesNeeded = static_cast<size_t>(nFrames) * engine->m_channelCount;
        const size_t read = engine->m_ringBuffer.read(out, samplesNeeded);

        if (read > 0) {
            engine->m_audioClockSample.store(
                engine->m_ringBufferEndSample.load(std::memory_order_acquire),
                std::memory_order_release);
        }
        // Fill remainder with silence on underrun
        if (read < samplesNeeded) {
            std::memset(out + read, 0, (samplesNeeded - read) * sizeof(int16_t));
            engine->m_underrunCount.fetch_add(1, std::memory_order_relaxed);
        }
        return 0;
    }

    // --- Sample math ---

    int64_t timelineFrameToSamples(int64_t frame) const {
        return frameToSamples(frame);
    }

    int64_t samplesToTimelineFrame(int64_t samples) const {
        return qMax<int64_t>(0, static_cast<int64_t>(
            std::floor((static_cast<double>(samples) * kTimelineFps) / m_sampleRate)));
    }

    int64_t nextPlayableSampleAtOrAfter(int64_t samplePos,
                                        const QVector<ExportRangeSegment>& ranges) const {
        if (ranges.isEmpty()) {
            return qMax<int64_t>(0, samplePos);
        }
        for (const ExportRangeSegment& range : ranges) {
            const int64_t rangeStartSample = timelineFrameToSamples(range.startFrame);
            const int64_t rangeEndSampleExclusive = timelineFrameToSamples(range.endFrame + 1);
            if (samplePos < rangeStartSample) {
                return rangeStartSample;
            }
            if (samplePos >= rangeStartSample && samplePos < rangeEndSampleExclusive) {
                return samplePos;
            }
        }
        return qMax<int64_t>(0, samplePos);
    }

    // --- Decode scheduling ---

    void enqueueDecodePathLocked(const QString& audioPath, bool highPriority, bool fullDecode) {
        if (audioPath.isEmpty()) {
            return;
        }
        auto cacheIt = m_audioCache.constFind(audioPath);
        if (cacheIt != m_audioCache.cend()) {
            if (cacheIt->fullyDecoded || !fullDecode) {
                return;
            }
        }
        if (!m_pendingDecodeSet.contains(audioPath)) {
            DecodeTask task;
            task.path = audioPath;
            task.fullDecode = fullDecode;
            if (highPriority) {
                m_pendingDecodePaths.push_front(task);
            } else {
                m_pendingDecodePaths.push_back(task);
            }
            m_pendingDecodeSet.insert(audioPath);
            return;
        }
        for (auto it = m_pendingDecodePaths.begin(); it != m_pendingDecodePaths.end(); ++it) {
            if (it->path == audioPath) {
                if (fullDecode && !it->fullDecode) {
                    it->fullDecode = true;
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
    }

    void scheduleDecodesLocked(const QVector<TimelineClip>& clips) {
        for (const TimelineClip& clip : clips) {
            if (!clipAudioPlaybackEnabled(clip) || clip.filePath.isEmpty()) {
                continue;
            }
            const QString audioPath = playbackAudioPathForClip(clip);
            if (audioPath.isEmpty()) {
                continue;
            }
            enqueueDecodePathLocked(audioPath, false, false);
        }
    }

    void prioritizeDecodesNearSampleLocked(int64_t focusSample) {
        if (m_timelineClips.isEmpty() || m_pendingDecodePaths.empty()) {
            return;
        }

        struct Candidate {
            int64_t distance = std::numeric_limits<int64_t>::max();
            QString path;
        };
        QVector<Candidate> candidates;
        candidates.reserve(m_timelineClips.size());

        for (const TimelineClip& clip : m_timelineClips) {
            if (!clipAudioPlaybackEnabled(clip) || clip.filePath.isEmpty()) {
                continue;
            }
            const QString audioPath = playbackAudioPathForClip(clip);
            if (audioPath.isEmpty() || m_audioCache.contains(audioPath)) {
                continue;
            }
            const int64_t clipStart = clipTimelineStartSamples(clip);
            const int64_t clipLenSamples =
                qMax<int64_t>(1, frameToSamples(qMax<int64_t>(1, clip.durationFrames)));
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

        std::sort(candidates.begin(), candidates.end(), [](const Candidate& a, const Candidate& b) {
            if (a.distance != b.distance) {
                return a.distance < b.distance;
            }
            return a.path < b.path;
        });

        constexpr int kHighPriorityDecodeCount = 4;
        QSet<QString> promoted;
        int promotedCount = 0;
        for (const Candidate& candidate : std::as_const(candidates)) {
            if (promoted.contains(candidate.path)) {
                continue;
            }
            enqueueDecodePathLocked(candidate.path, true, false);
            promoted.insert(candidate.path);
            if (++promotedCount >= kHighPriorityDecodeCount) {
                break;
            }
        }
    }

    // --- Gain calculations ---

    struct SpeechRangeBlend {
        float primaryGain = 1.0f;
        float secondaryGain = 0.0f;
        int64_t secondaryTimelineSample = -1;
    };

    SpeechRangeBlend calculateSpeechRangeBlend(int64_t samplePos,
                                               const QVector<ExportRangeSegment>& ranges,
                                               int fadeSamples,
                                               bool crossfadeEnabled) const {
        SpeechRangeBlend blend;
        if (ranges.isEmpty()) {
            return blend;
        }

        int currentRangeIndex = -1;
        int64_t currentStart = 0;
        int64_t currentEndExclusive = 0;
        for (int i = 0; i < ranges.size(); ++i) {
            const int64_t rangeStartSample = timelineFrameToSamples(ranges.at(i).startFrame);
            const int64_t rangeEndSampleExclusive =
                timelineFrameToSamples(ranges.at(i).endFrame + 1);
            if (samplePos >= rangeStartSample && samplePos < rangeEndSampleExclusive) {
                currentRangeIndex = i;
                currentStart = rangeStartSample;
                currentEndExclusive = rangeEndSampleExclusive;
                break;
            }
        }

        if (currentRangeIndex < 0) {
            blend.primaryGain = 0.0f;
            return blend;
        }

        if (fadeSamples <= 0) {
            return blend;
        }

        if (!crossfadeEnabled) {
            const int64_t samplesFromStart = samplePos - currentStart;
            const int64_t samplesToEnd = currentEndExclusive - samplePos;

            float gain = 1.0f;
            if (samplesFromStart < fadeSamples) {
                gain = qMin(gain,
                            static_cast<float>(samplesFromStart) / static_cast<float>(fadeSamples));
            }
            if (samplesToEnd < fadeSamples) {
                gain = qMin(gain,
                            static_cast<float>(samplesToEnd) / static_cast<float>(fadeSamples));
            }
            blend.primaryGain = qBound(0.0f, gain, 1.0f);
            return blend;
        }

        static constexpr float kHalfPi = 1.57079632679489661923f;
        const int64_t currentLength = qMax<int64_t>(1, currentEndExclusive - currentStart);

        if (currentRangeIndex > 0) {
            const int64_t prevStart = timelineFrameToSamples(ranges.at(currentRangeIndex - 1).startFrame);
            const int64_t prevEndExclusive =
                timelineFrameToSamples(ranges.at(currentRangeIndex - 1).endFrame + 1);
            const int64_t prevLength = qMax<int64_t>(1, prevEndExclusive - prevStart);
            const int64_t crossWindow =
                qMax<int64_t>(1, qMin<int64_t>(fadeSamples, qMin<int64_t>(prevLength, currentLength)));
            const int64_t offsetFromStart = samplePos - currentStart;
            if (offsetFromStart >= 0 && offsetFromStart < crossWindow) {
                const float t = (static_cast<float>(offsetFromStart) + 0.5f) /
                                static_cast<float>(crossWindow);
                blend.primaryGain = std::sin(t * kHalfPi);
                blend.secondaryGain = std::cos(t * kHalfPi);
                blend.secondaryTimelineSample =
                    (prevEndExclusive - crossWindow) + offsetFromStart;
                return blend;
            }
        }

        if (currentRangeIndex + 1 < ranges.size()) {
            const int64_t nextStart = timelineFrameToSamples(ranges.at(currentRangeIndex + 1).startFrame);
            const int64_t nextEndExclusive =
                timelineFrameToSamples(ranges.at(currentRangeIndex + 1).endFrame + 1);
            const int64_t nextLength = qMax<int64_t>(1, nextEndExclusive - nextStart);
            const int64_t crossWindow =
                qMax<int64_t>(1, qMin<int64_t>(fadeSamples, qMin<int64_t>(nextLength, currentLength)));
            const int64_t offsetFromWindowStart = samplePos - (currentEndExclusive - crossWindow);
            if (offsetFromWindowStart >= 0 && offsetFromWindowStart < crossWindow) {
                const float t = (static_cast<float>(offsetFromWindowStart) + 0.5f) /
                                static_cast<float>(crossWindow);
                blend.primaryGain = std::cos(t * kHalfPi);
                blend.secondaryGain = std::sin(t * kHalfPi);
                blend.secondaryTimelineSample = nextStart + offsetFromWindowStart;
                return blend;
            }
        }

        return blend;
    }

    float calculateClipCrossfadeGain(int64_t samplePos, const TimelineClip& clip,
                                      int64_t clipStartSample, int64_t clipEndSample,
                                      int fadeSamples) const {
        if (fadeSamples <= 0) {
            return 1.0f;
        }

        float gain = 1.0f;
        const int64_t samplesFromStart = samplePos - clipStartSample;
        if (samplesFromStart >= 0 && samplesFromStart < fadeSamples) {
            gain *= static_cast<float>(samplesFromStart) / static_cast<float>(fadeSamples);
        }
        const int64_t samplesToEnd = clipEndSample - samplePos;
        if (samplesToEnd >= 0 && samplesToEnd < fadeSamples) {
            gain *= static_cast<float>(samplesToEnd) / static_cast<float>(fadeSamples);
        }
        return qBound(0.0f, gain, 1.0f);
    }

    // --- FFmpeg full-file decode ---

    AudioClipCacheEntry decodeClipAudio(const QString& path, int64_t maxOutputFrames) {
        AudioClipCacheEntry cache;

        AVFormatContext* formatCtx = nullptr;
        if (avformat_open_input(&formatCtx, QFile::encodeName(path).constData(), nullptr, nullptr) < 0) {
            return cache;
        }

        if (avformat_find_stream_info(formatCtx, nullptr) < 0) {
            avformat_close_input(&formatCtx);
            return cache;
        }

        int audioStreamIndex = -1;
        for (unsigned i = 0; i < formatCtx->nb_streams; ++i) {
            if (formatCtx->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_AUDIO) {
                audioStreamIndex = static_cast<int>(i);
                break;
            }
        }
        if (audioStreamIndex < 0) {
            avformat_close_input(&formatCtx);
            return cache;
        }

        AVStream* stream = formatCtx->streams[audioStreamIndex];
        const AVCodec* decoder = avcodec_find_decoder(stream->codecpar->codec_id);
        if (!decoder) {
            avformat_close_input(&formatCtx);
            return cache;
        }

        AVCodecContext* codecCtx = avcodec_alloc_context3(decoder);
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

        SwrContext* swr = swr_alloc();
        ffmpeg_compat::ChannelLayoutHandle outLayout{};
        ffmpeg_compat::defaultChannelLayout(&outLayout, m_channelCount);
        ffmpeg_compat::setSwrInputLayout(swr, codecCtx);
        ffmpeg_compat::setSwrOutputLayout(swr, &outLayout);
        av_opt_set_int(swr, "in_sample_rate", codecCtx->sample_rate, 0);
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

        AVPacket* packet = av_packet_alloc();
        AVFrame* frame = av_frame_alloc();
        QByteArray converted;
        const bool limitedDecode = maxOutputFrames > 0;
        const int64_t maxOutputSamples =
            limitedDecode ? qMax<int64_t>(1, maxOutputFrames * m_channelCount) : -1;
        bool reachedEof = false;
        bool hitOutputLimit = false;

        auto appendConverted = [&](AVFrame* decoded) {
            const int outSamples = swr_get_out_samples(swr, decoded->nb_samples);
            if (outSamples <= 0) {
                return;
            }
            uint8_t* outData = nullptr;
            int outLineSize = 0;
            if (av_samples_alloc(&outData, &outLineSize, m_channelCount, outSamples, AV_SAMPLE_FMT_FLT, 0) < 0) {
                return;
            }
            const int convertedSamples = swr_convert(swr, &outData, outSamples,
                                                     const_cast<const uint8_t**>(decoded->extended_data),
                                                     decoded->nb_samples);
            if (convertedSamples > 0) {
                const int byteCount = convertedSamples * m_channelCount * static_cast<int>(sizeof(float));
                converted.append(reinterpret_cast<const char*>(outData), byteCount);
                if (maxOutputSamples > 0) {
                    const int64_t currentSamples =
                        static_cast<int64_t>(converted.size() / static_cast<int>(sizeof(float)));
                    if (currentSamples >= maxOutputSamples) {
                        const int truncatedBytes =
                            static_cast<int>(maxOutputSamples * static_cast<int64_t>(sizeof(float)));
                        converted.truncate(truncatedBytes);
                        hitOutputLimit = true;
                    }
                }
            }
            av_freep(&outData);
        };

        while (!hitOutputLimit && av_read_frame(formatCtx, packet) >= 0) {
            if (packet->stream_index != audioStreamIndex) {
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
            uint8_t* outData = nullptr;
            int outLineSize = 0;
            if (av_samples_alloc(&outData, &outLineSize, m_channelCount, outSamples, AV_SAMPLE_FMT_FLT, 0) >= 0) {
                const int flushed = swr_convert(swr, &outData, outSamples, nullptr, 0);
                if (flushed > 0) {
                    converted.append(reinterpret_cast<const char*>(outData),
                                     flushed * m_channelCount * static_cast<int>(sizeof(float)));
                }
                av_freep(&outData);
            }
        }

        const int sampleCount = converted.size() / static_cast<int>(sizeof(float));
        cache.samples.resize(sampleCount);
        std::memcpy(cache.samples.data(), converted.constData(), converted.size());
        cache.valid = !cache.samples.isEmpty();
        cache.fullyDecoded = cache.valid && (!limitedDecode || reachedEof);

        av_frame_free(&frame);
        av_packet_free(&packet);
        ffmpeg_compat::uninitChannelLayout(&outLayout);
        swr_free(&swr);
        avcodec_free_context(&codecCtx);
        avformat_close_input(&formatCtx);
        return cache;
    }

    AudioClipCacheEntry clipCacheForPathCopy(const QString& path) const {
        std::lock_guard<std::mutex> lock(m_stateMutex);
        return m_audioCache.value(path);
    }

    QVector<ExportRangeSegment> exportRangesCopy() const {
        std::lock_guard<std::mutex> lock(m_exportRangesMutex);
        return m_exportRanges;
    }

    // --- Mix engine ---

    void mixChunk(const MixContext& context,
                  float* output,
                  int frames,
                  int64_t chunkStartSample,
                  qreal playbackRate) {
        std::fill(output, output + frames * m_channelCount, 0.0f);
        const qreal clampedRate = qBound<qreal>(0.1, playbackRate, 3.0);

        for (int outFrame = 0; outFrame < frames; ++outFrame) {
            const int64_t timelineOffset =
                static_cast<int64_t>(std::floor(static_cast<qreal>(outFrame) * clampedRate));
            const int64_t timelineSamplePos = chunkStartSample + timelineOffset;
            const int outIndex = outFrame * m_channelCount;

            for (const TimelineClip& clip : context.clips) {
                if (!clipAudioPlaybackEnabled(clip)) {
                    continue;
                }
                const AudioClipCacheEntry audio = clipCacheForPathCopy(playbackAudioPathForClip(clip));
                if (!audio.valid) {
                    continue;
                }

                const int64_t clipStartSample = clipTimelineStartSamples(clip);
                const int64_t sourceInSample = clipSourceInSamples(clip);
                const int64_t clipAvailableSamples =
                    (audio.samples.size() / m_channelCount) - sourceInSample;
                if (clipAvailableSamples <= 0) {
                    continue;
                }
                const int64_t clipEndSample = clipStartSample + qMin<int64_t>(
                    clip.durationFrames * m_sampleRate / kTimelineFps, clipAvailableSamples);
                if (timelineSamplePos < clipStartSample || timelineSamplePos >= clipEndSample) {
                    continue;
                }

                const int64_t inFrame = sourceSampleForClipAtTimelineSample(
                    clip, timelineSamplePos, context.renderSyncMarkers);
                if (inFrame < 0 || inFrame >= (audio.samples.size() / m_channelCount)) {
                    continue;
                }
                const int inIndex = static_cast<int>(inFrame * m_channelCount);

                float primarySpeechGain = 1.0f;
                float secondarySpeechGain = 0.0f;
                int64_t secondaryTimelineSample = -1;
                if (!context.exportRanges.isEmpty()) {
                    const SpeechRangeBlend blend = calculateSpeechRangeBlend(
                        timelineSamplePos,
                        context.exportRanges,
                        m_speechFilterFadeSamples.load(std::memory_order_acquire),
                        m_speechFilterRangeCrossfadeEnabled.load(std::memory_order_acquire));
                    primarySpeechGain = blend.primaryGain;
                    secondarySpeechGain = blend.secondaryGain;
                    secondaryTimelineSample = blend.secondaryTimelineSample;
                }

                const float primaryClipGain = calculateClipCrossfadeGain(
                    timelineSamplePos,
                    clip,
                    clipStartSample,
                    clipEndSample,
                    clip.fadeSamples > 0 ? clip.fadeSamples : m_defaultFadeSamples);
                const float primaryGain = primarySpeechGain * primaryClipGain;
                if (primaryGain > 0.0f) {
                    output[outIndex] += audio.samples[inIndex] * primaryGain;
                    output[outIndex + 1] += audio.samples[inIndex + 1] * primaryGain;
                }

                if (secondarySpeechGain > 0.0f && secondaryTimelineSample >= 0) {
                    const int64_t secondaryInFrame = sourceSampleForClipAtTimelineSample(
                        clip, secondaryTimelineSample, context.renderSyncMarkers);
                    if (secondaryInFrame >= 0 &&
                        secondaryInFrame < (audio.samples.size() / m_channelCount)) {
                        const int secondaryInIndex = static_cast<int>(secondaryInFrame * m_channelCount);
                        const float secondaryClipGain = calculateClipCrossfadeGain(
                            secondaryTimelineSample,
                            clip,
                            clipStartSample,
                            clipEndSample,
                            clip.fadeSamples > 0 ? clip.fadeSamples : m_defaultFadeSamples);
                        const float secondaryGain = secondarySpeechGain * secondaryClipGain;
                        output[outIndex] += audio.samples[secondaryInIndex] * secondaryGain;
                        output[outIndex + 1] += audio.samples[secondaryInIndex + 1] * secondaryGain;
                    }
                }
            }
        }

        const float masterGain = context.muted ? 0.0f : static_cast<float>(context.volume);
        for (int i = 0; i < frames * m_channelCount; ++i) {
            output[i] = qBound(-1.0f, output[i] * masterGain, 1.0f);
        }
    }

    // --- Worker threads ---

    void decodeLoop() {
        while (true) {
            DecodeTask nextTask;
            {
                std::unique_lock<std::mutex> lock(m_stateMutex);
                m_decodeCondition.wait(lock, [this]() {
                    return !m_running || !m_pendingDecodePaths.empty();
                });
                if (!m_running) {
                    break;
                }
                nextTask = m_pendingDecodePaths.front();
                m_pendingDecodePaths.pop_front();
            }

            AudioClipCacheEntry decoded = decodeClipAudio(
                nextTask.path,
                nextTask.fullDecode ? -1 : kInitialDecodeFrames);

            {
                std::lock_guard<std::mutex> lock(m_stateMutex);
                m_pendingDecodeSet.remove(nextTask.path);
                if (nextTask.fullDecode) {
                    if (decoded.valid) {
                        m_audioCache.insert(nextTask.path, decoded);
                    }
                } else {
                    if (decoded.valid) {
                        m_audioCache.insert(nextTask.path, decoded);
                        enqueueDecodePathLocked(nextTask.path, false, true);
                    }
                }
            }
            if (!nextTask.fullDecode && decoded.valid) {
                m_decodeCondition.notify_one();
            }
            m_stateCondition.notify_all();
        }
    }

    void mixLoop() {
        QVector<float> mixBuffer(m_periodFrames * m_channelCount);
        QVector<int16_t> pcmBuffer(m_periodFrames * m_channelCount);

        while (true) {
            // Wait until playing
            {
                std::unique_lock<std::mutex> lock(m_stateMutex);
                m_stateCondition.wait(lock, [this]() {
                    return !m_running || m_playing;
                });
                if (!m_running) {
                    break;
                }
            }

            // Wait until ring buffer needs more data
            {
                std::unique_lock<std::mutex> lock(m_mixMutex);
                m_mixCondition.wait_for(lock, std::chrono::milliseconds(5), [this]() {
                    return !m_running || !m_playing ||
                           m_ringBuffer.available() < static_cast<size_t>(m_mixLowWaterSamples);
                });
                if (!m_running) {
                    break;
                }
                if (!m_playing) {
                    continue;
                }
                if (m_ringBuffer.available() >= static_cast<size_t>(m_mixLowWaterSamples)) {
                    continue;
                }
            }

            MixContext context;
            int64_t chunkStartSample = 0;
            qreal playbackRate = 1.0;
            {
                std::lock_guard<std::mutex> lock(m_stateMutex);
                if (!m_playing) {
                    continue;
                }
                context.clips = m_timelineClips;
                context.exportRanges = exportRangesCopy();
                context.renderSyncMarkers = m_renderSyncMarkers;
                playbackRate = qBound<qreal>(0.1, m_playbackRate.load(std::memory_order_acquire), 3.0);
                const qreal chunkTimelineDuration = playbackRate * static_cast<qreal>(m_periodFrames);
                const int64_t timelineStep = qMax<int64_t>(
                    1, static_cast<int64_t>(std::llround(chunkTimelineDuration)));
                chunkStartSample = nextPlayableSampleAtOrAfter(m_timelineSampleCursor, context.exportRanges);
                m_timelineSampleCursor = chunkStartSample + timelineStep;
                context.muted = m_muted;
                context.volume = m_volume;
            }

            mixChunk(context, mixBuffer.data(), m_periodFrames, chunkStartSample, playbackRate);

            for (int i = 0; i < pcmBuffer.size(); ++i) {
                pcmBuffer[i] = static_cast<int16_t>(mixBuffer[i] * 32767.0f);
            }

            m_ringBuffer.write(pcmBuffer.constData(), static_cast<size_t>(pcmBuffer.size()));
            const qreal chunkTimelineDuration = playbackRate * static_cast<qreal>(m_periodFrames);
            const int64_t timelineStep = qMax<int64_t>(
                1, static_cast<int64_t>(std::llround(chunkTimelineDuration)));
            m_ringBufferEndSample.store(chunkStartSample + timelineStep, std::memory_order_release);
        }
    }

    // --- Member variables ---

    mutable std::mutex m_stateMutex;
    mutable std::mutex m_exportRangesMutex;
    std::mutex m_mixMutex;
    std::condition_variable m_stateCondition;
    std::condition_variable m_decodeCondition;
    std::condition_variable m_mixCondition;

    QVector<TimelineClip> m_timelineClips;
    QVector<RenderSyncMarker> m_renderSyncMarkers;
    QVector<ExportRangeSegment> m_exportRanges;
    QHash<QString, AudioClipCacheEntry> m_audioCache;
    std::deque<DecodeTask> m_pendingDecodePaths;
    QSet<QString> m_pendingDecodeSet;

    std::thread m_decodeWorker;
    std::thread m_mixWorker;

    std::unique_ptr<rt::audio::RtAudio> m_rtaudio;
    AudioRingBuffer m_ringBuffer;
    std::atomic<int64_t> m_ringBufferEndSample{0};

    bool m_initialized = false;
    std::atomic<bool> m_running{false};
    std::atomic<bool> m_playing{false};
    bool m_muted = false;
    qreal m_volume = 0.8;
    int64_t m_timelineSampleCursor = 0;
    std::atomic<int64_t> m_audioClockSample{0};
    std::atomic<int> m_underrunCount{0};
    std::atomic<qreal> m_playbackRate{1.0};
    std::atomic<int> m_playbackWarpMode{static_cast<int>(PlaybackAudioWarpMode::Disabled)};

    static constexpr int m_sampleRate = 48000;
    static constexpr int m_channelCount = 2;
    static constexpr int m_periodFrames = 1024;
    static constexpr int m_initialDecodeSeconds = 2;
    static constexpr int64_t kInitialDecodeFrames =
        static_cast<int64_t>(m_sampleRate) * m_initialDecodeSeconds;
    static constexpr int m_mixLowWaterSamples = 2048 * 2; // samples (frames * channels)
    static constexpr int m_defaultFadeSamples = 250;
    static constexpr qint64 kAudioInitBackoffMs = 10000;
    static constexpr qint64 kAudioInitWarningThrottleMs = 10000;
    std::atomic<int> m_speechFilterFadeSamples{m_defaultFadeSamples};
    std::atomic<bool> m_speechFilterRangeCrossfadeEnabled{false};
    qint64 m_audioInitBackoffUntilMs = 0;
    qint64 m_lastAudioInitWarningMs = 0;
    qint64 m_lastKnownDeviceCount = 0;
    bool m_lastKnownDefaultOutputValid = false;
    unsigned int m_lastKnownDefaultOutputId = 0;
    QString m_lastKnownDefaultOutputName;
    int m_lastKnownDefaultOutputChannels = 0;
    QString m_lastDeviceInfoError;
};
