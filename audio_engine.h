#pragma once

#include "editor_shared.h"
#include "audio_time_stretch.h"
#include "audio_time_stretch_cache.h"
#include "debug_controls.h"
#include "ffmpeg_compat.h"
#include "decoder_ffmpeg_utils.h"
#include "preview_surface.h"

#include <QByteArray>
#include <QDateTime>
#include <QDebug>
#include <QFile>
#include <QFileInfo>
#include <QHash>
#include <QSet>
#include <QVector>

#include <array>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
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
        bool queueChanged = false;
        {
            std::lock_guard<std::mutex> lock(m_stateMutex);
            const QSet<QString> nextScheduledPaths = scheduledAudioPathsFromClips(clips);
            if (nextScheduledPaths != m_scheduledDecodePaths) {
                const QSet<QString> addedPaths = nextScheduledPaths - m_scheduledDecodePaths;
                const QSet<QString> removedPaths = m_scheduledDecodePaths - nextScheduledPaths;
                for (const QString& path : removedPaths) {
                    removePendingDecodePathLocked(path);
                }
                for (const QString& path : addedPaths) {
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
        const int newMode = static_cast<int>(mode);
        const int oldMode = m_playbackWarpMode.exchange(newMode, std::memory_order_acq_rel);
        if (oldMode == newMode) {
            return;
        }
        if (mode == PlaybackAudioWarpMode::TimeStretch &&
            pitchPreservingTimeStretchActive(m_playbackRate.load(std::memory_order_acquire))) {
            enqueueTimeStretchPrecomputeForScheduledPaths();
        }
    }

    void setPlaybackRate(qreal rate) {
        const qreal clampedRate = qBound<qreal>(0.1, rate, 3.0);
        const int oldRateKey = timeStretchRateKey(m_playbackRate.load(std::memory_order_acquire));
        const int newRateKey = timeStretchRateKey(clampedRate);
        m_playbackRate.store(clampedRate, std::memory_order_release);
        if (oldRateKey == newRateKey) {
            return;
        }
        if (pitchPreservingTimeStretchActive(clampedRate) &&
            static_cast<PlaybackAudioWarpMode>(m_playbackWarpMode.load(std::memory_order_acquire)) ==
                PlaybackAudioWarpMode::TimeStretch) {
            enqueueTimeStretchPrecomputeForScheduledPaths();
        }
    }

    void setTranscriptNormalizeEnabled(bool enabled) {
        m_transcriptNormalizeEnabled.store(enabled, std::memory_order_release);
    }

    void setTranscriptNormalizeRanges(const QVector<ExportRangeSegment>& ranges) {
        std::lock_guard<std::mutex> lock(m_transcriptNormalizeRangesMutex);
        m_transcriptNormalizeRanges = ranges;
    }

    void setAudioDynamicsSettings(const PreviewSurface::AudioDynamicsSettings& settings) {
        m_amplifyEnabled.store(settings.amplifyEnabled, std::memory_order_release);
        m_amplifyDb.store(settings.amplifyDb, std::memory_order_release);
        m_normalizeEnabled.store(settings.normalizeEnabled, std::memory_order_release);
        m_normalizeTargetDb.store(settings.normalizeTargetDb, std::memory_order_release);
        m_selectiveNormalizeEnabled.store(settings.selectiveNormalizeEnabled, std::memory_order_release);
        m_selectiveNormalizeMinSegmentSeconds.store(
            settings.selectiveNormalizeMinSegmentSeconds, std::memory_order_release);
        m_selectiveNormalizePeakDb.store(settings.selectiveNormalizePeakDb, std::memory_order_release);
        m_selectiveNormalizePasses.store(settings.selectiveNormalizePasses, std::memory_order_release);
        m_peakReductionEnabled.store(settings.peakReductionEnabled, std::memory_order_release);
        m_peakThresholdDb.store(settings.peakThresholdDb, std::memory_order_release);
        m_limiterEnabled.store(settings.limiterEnabled, std::memory_order_release);
        m_limiterThresholdDb.store(settings.limiterThresholdDb, std::memory_order_release);
        m_compressorEnabled.store(settings.compressorEnabled, std::memory_order_release);
        m_compressorThresholdDb.store(settings.compressorThresholdDb, std::memory_order_release);
        m_compressorRatio.store(settings.compressorRatio, std::memory_order_release);
    }

    void setBackgroundDecodeSuppressed(bool suppressed) {
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
            m_timelineSampleCursor = timelineFrameToSamples(startFrame);
            m_audioClockSample.store(m_timelineSampleCursor, std::memory_order_release);
            m_lastReportedCurrentSample.store(m_timelineSampleCursor, std::memory_order_release);
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
            // Fade to zero from the last rendered sample to avoid click/pop on stop.
            const int16_t lastL = static_cast<int16_t>(m_lastOutputLeft.load(std::memory_order_acquire));
            const int16_t lastR = static_cast<int16_t>(m_lastOutputRight.load(std::memory_order_acquire));
            m_ringBuffer.clear();
            QVector<int16_t> fadeSamples;
            fadeSamples.resize(kShutdownFadeFrames * m_channelCount);
            for (int frame = 0; frame < kShutdownFadeFrames; ++frame) {
                const qreal gain = 1.0 - (static_cast<qreal>(frame + 1) / static_cast<qreal>(kShutdownFadeFrames));
                fadeSamples[frame * m_channelCount] =
                    static_cast<int16_t>(qRound(static_cast<qreal>(lastL) * gain));
                fadeSamples[frame * m_channelCount + 1] =
                    static_cast<int16_t>(qRound(static_cast<qreal>(lastR) * gain));
            }
            m_ringBuffer.write(fadeSamples.constData(), static_cast<size_t>(fadeSamples.size()));
            const int64_t currentEnd = m_ringBufferEndSample.load(std::memory_order_acquire);
            m_ringBufferEndSample.store(currentEnd + kShutdownFadeFrames, std::memory_order_release);
            const int fadeMs = qMax(1, static_cast<int>(std::ceil(
                (1000.0 * static_cast<double>(kShutdownFadeFrames)) / static_cast<double>(m_sampleRate))));
            std::this_thread::sleep_for(std::chrono::milliseconds(fadeMs + 2));
            m_rtaudio->stopStream();
        }
        m_ringBuffer.clear();
        m_mixCondition.notify_all();
    }

    void seek(int64_t frame) {
        m_seekCount.fetch_add(1, std::memory_order_relaxed);
        m_lastSeekFrame.store(frame, std::memory_order_release);
        {
            std::lock_guard<std::mutex> lock(m_stateMutex);
            const int64_t sample = timelineFrameToSamples(frame);
            m_timelineSampleCursor = sample;
            m_audioClockSample.store(sample, std::memory_order_release);
            m_lastReportedCurrentSample.store(sample, std::memory_order_release);
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

    bool warmPlaybackAudio(int64_t startFrame, int timeoutMs) {
        if (!initialize()) {
            return false;
        }
        const int64_t timelineSample = timelineFrameToSamples(startFrame);
        const auto deadline =
            std::chrono::steady_clock::now() +
            std::chrono::milliseconds(qMax(1, timeoutMs));
        if (ensureTimeStretchAudioReadyForTimelineSample(timelineSample)) {
            m_audioPlaybackBlocked.store(false, std::memory_order_release);
            m_pitchPreservingAudioBlocked.store(false, std::memory_order_release);
            m_timeStretchPrecomputeBlocked.store(false, std::memory_order_release);
            return true;
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
            lock.lock();
            if (m_stateCondition.wait_until(lock, deadline) == std::cv_status::timeout) {
                m_audioPlaybackBlocked.store(true, std::memory_order_release);
                return audioReadyForTimelineSampleLocked(timelineSample);
            }
        }
        return false;
    }

    bool playbackAudioReadyForFrame(int64_t startFrame) const {
        const int64_t timelineSample = timelineFrameToSamples(startFrame);
        std::lock_guard<std::mutex> lock(m_stateMutex);
        return audioReadyForTimelineSampleLocked(timelineSample);
    }

    bool playbackAudioBlocked() const {
        return m_audioPlaybackBlocked.load(std::memory_order_acquire);
    }

    bool playbackAudioNeedsRetimingForFrame(int64_t startFrame) const {
        const int64_t timelineSample = timelineFrameToSamples(startFrame);
        QString audioPath;
        qreal playbackRate = 1.0;
        PlaybackAudioWarpMode warpMode = PlaybackAudioWarpMode::Disabled;
        {
            std::lock_guard<std::mutex> lock(m_stateMutex);
            const TimelineClip* clip = nullptr;
            if (!clipAndSourceSampleAtTimelineSampleLocked(
                    timelineSample, &clip, &audioPath, nullptr)) {
                return false;
            }
            playbackRate =
                qBound<qreal>(0.1, m_playbackRate.load(std::memory_order_acquire), 3.0);
            warpMode = static_cast<PlaybackAudioWarpMode>(
                m_playbackWarpMode.load(std::memory_order_acquire));
            if (warpMode != PlaybackAudioWarpMode::TimeStretch ||
                !pitchPreservingTimeStretchActive(playbackRate) ||
                audioReadyForTimelineSampleLocked(timelineSample)) {
                return false;
            }
        }

        const int sidecarSpeedKey = precomputedTimeStretchSpeedKey(playbackRate);
        if (sidecarSpeedKey <= 1) {
            return true;
        }

        AudioTimeStretchSidecarMetadata metadata;
        return !readAudioTimeStretchSidecarMetadata(audioPath, sidecarSpeedKey, &metadata) ||
               !metadata.valid ||
               !metadata.fullyDecoded;
    }

    bool audioClockAvailable() const {
        std::lock_guard<std::mutex> lock(m_stateMutex);
        return m_initialized && m_rtaudio && m_rtaudio->isStreamRunning();
    }

    bool playbackStarted() const {
        return m_playing.load(std::memory_order_acquire);
    }

    QJsonObject profilingSnapshot() const {
        QJsonObject snapshot;
        const auto silentReasonToString = [](int reason) -> QString {
            switch (reason) {
            case 1: return QStringLiteral("muted_or_volume_zero");
            case 2: return QStringLiteral("no_prepared_clips");
            case 3: return QStringLiteral("waiting_for_playable_audio");
            case 4: return QStringLiteral("no_active_clip_in_chunk");
            case 5: return QStringLiteral("input_out_of_range");
            case 6: return QStringLiteral("speech_gain_zero");
            case 7: return QStringLiteral("clip_gain_zero");
            case 8: return QStringLiteral("source_samples_zero");
            case 9: return QStringLiteral("output_below_threshold");
            default: return QStringLiteral("none");
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
        const qreal playbackRate = qBound<qreal>(
            0.1, m_playbackRate.load(std::memory_order_acquire), 3.0);
        snapshot[QStringLiteral("initialized")] = m_initialized;
        snapshot[QStringLiteral("running")] = m_running.load(std::memory_order_acquire);
        snapshot[QStringLiteral("playing")] = m_playing.load(std::memory_order_acquire);
        snapshot[QStringLiteral("has_playable_audio")] = hasPlayableAudio();
        snapshot[QStringLiteral("audio_clock_available")] = audioClockAvailable();
        snapshot[QStringLiteral("current_sample")] = static_cast<qint64>(currentSampleValue);
        snapshot[QStringLiteral("current_frame")] = static_cast<qint64>(currentFrame());
        snapshot[QStringLiteral("ring_buffer_samples_available")] = static_cast<qint64>(ringBufferSamplesAvailable);
        snapshot[QStringLiteral("ring_buffer_frames_available")] = static_cast<qint64>(ringBufferFramesAvailable);
        snapshot[QStringLiteral("ring_buffer_ms_available")] =
            static_cast<qint64>((ringBufferFramesAvailable * 1000) / qMax(1, m_sampleRate));
        snapshot[QStringLiteral("ring_buffer_end_sample")] = static_cast<qint64>(ringBufferEndSample);
        snapshot[QStringLiteral("ring_buffer_end_frame")] =
            static_cast<qint64>(samplesToTimelineFrame(ringBufferEndSample));
        snapshot[QStringLiteral("buffered_timeline_samples")] =
            static_cast<qint64>(qMax<int64_t>(0, ringBufferEndSample - currentSampleValue));
        snapshot[QStringLiteral("buffered_timeline_frames")] =
            static_cast<qint64>(qMax<int64_t>(
                0,
                samplesToTimelineFrame(ringBufferEndSample) -
                    samplesToTimelineFrame(currentSampleValue)));
        snapshot[QStringLiteral("audio_clock_sample")] = static_cast<qint64>(audioClockSample);
        snapshot[QStringLiteral("audio_clock_frame")] =
            static_cast<qint64>(samplesToTimelineFrame(audioClockSample));
        snapshot[QStringLiteral("timeline_sample_cursor")] = static_cast<qint64>(m_timelineSampleCursor);
        snapshot[QStringLiteral("timeline_cursor_frame")] =
            static_cast<qint64>(samplesToTimelineFrame(m_timelineSampleCursor));
        snapshot[QStringLiteral("underrun_count")] = m_underrunCount.load(std::memory_order_acquire);
        snapshot[QStringLiteral("last_callback_requested_samples")] =
            static_cast<qint64>(m_lastCallbackRequestedSamples.load(std::memory_order_acquire));
        snapshot[QStringLiteral("last_callback_read_samples")] =
            static_cast<qint64>(m_lastCallbackReadSamples.load(std::memory_order_acquire));
        snapshot[QStringLiteral("last_callback_underrun_samples")] =
            static_cast<qint64>(m_lastCallbackUnderrunSamples.load(std::memory_order_acquire));
        snapshot[QStringLiteral("last_output_left")] = m_lastOutputLeft.load(std::memory_order_acquire);
        snapshot[QStringLiteral("last_output_right")] = m_lastOutputRight.load(std::memory_order_acquire);
        snapshot[QStringLiteral("sample_rate")] = m_sampleRate;
        snapshot[QStringLiteral("channel_count")] = m_channelCount;
        snapshot[QStringLiteral("period_frames")] = m_periodFrames;
        snapshot[QStringLiteral("playback_rate")] = playbackRate;
        snapshot[QStringLiteral("playback_warp_mode")] =
            playbackAudioWarpModeToString(static_cast<PlaybackAudioWarpMode>(
                m_playbackWarpMode.load(std::memory_order_acquire)));
        snapshot[QStringLiteral("time_stretch_cache_miss_count")] =
            static_cast<qint64>(m_timeStretchCacheMissCount.load(std::memory_order_acquire));
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
        snapshot[QStringLiteral("last_mix_chunk_start_sample")] =
            static_cast<qint64>(m_lastMixChunkStartSample.load(std::memory_order_acquire));
        snapshot[QStringLiteral("last_mix_chunk_end_sample")] =
            static_cast<qint64>(m_lastMixChunkEndSample.load(std::memory_order_acquire));
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
            static_cast<qint64>(m_lastMixOutOfRangeTimelineSample.load(std::memory_order_acquire));
        snapshot[QStringLiteral("last_mix_out_of_range_source_sample")] =
            static_cast<qint64>(m_lastMixOutOfRangeSourceSample.load(std::memory_order_acquire));
        snapshot[QStringLiteral("last_mix_out_of_range_normalized_sample")] =
            static_cast<qint64>(m_lastMixOutOfRangeNormalizedSample.load(std::memory_order_acquire));
        snapshot[QStringLiteral("last_mix_out_of_range_audio_start_sample")] =
            static_cast<qint64>(m_lastMixOutOfRangeAudioStartSample.load(std::memory_order_acquire));
        snapshot[QStringLiteral("last_mix_out_of_range_audio_end_sample")] =
            static_cast<qint64>(m_lastMixOutOfRangeAudioEndSample.load(std::memory_order_acquire));
        snapshot[QStringLiteral("last_mix_time_stretch_speed_per_mille")] =
            m_lastMixTimeStretchSpeedPermille.load(std::memory_order_acquire);
        snapshot[QStringLiteral("last_mix_first_clip_start_sample")] =
            static_cast<qint64>(m_lastMixFirstClipStartSample.load(std::memory_order_acquire));
        snapshot[QStringLiteral("last_mix_first_clip_end_sample")] =
            static_cast<qint64>(m_lastMixFirstClipEndSample.load(std::memory_order_acquire));
        snapshot[QStringLiteral("last_mix_first_audio_start_sample")] =
            static_cast<qint64>(m_lastMixFirstAudioStartSample.load(std::memory_order_acquire));
        snapshot[QStringLiteral("last_mix_first_audio_frame_count")] =
            static_cast<qint64>(m_lastMixFirstAudioFrameCount.load(std::memory_order_acquire));
        snapshot[QStringLiteral("last_mix_first_local_sample_at_chunk_start")] =
            static_cast<qint64>(m_lastMixFirstLocalSampleAtChunkStart.load(std::memory_order_acquire));
        snapshot[QStringLiteral("last_mix_silence_reason")] =
            silentReasonToString(m_lastMixSilentReason.load(std::memory_order_acquire));

        std::lock_guard<std::mutex> lock(m_stateMutex);
        snapshot[QStringLiteral("decoded_audio_path_count")] =
            static_cast<qint64>(m_audioCache.size());
        snapshot[QStringLiteral("time_stretch_cache_path_count")] =
            static_cast<qint64>(m_timeStretchAudioCache.size());
        qint64 timeStretchEntryCount = 0;
        for (auto it = m_timeStretchAudioCache.cbegin(); it != m_timeStretchAudioCache.cend(); ++it) {
            for (auto speedIt = it.value().cbegin(); speedIt != it.value().cend(); ++speedIt) {
                timeStretchEntryCount += speedIt.value().size();
            }
        }
        snapshot[QStringLiteral("time_stretch_cache_entry_count")] = timeStretchEntryCount;
        snapshot[QStringLiteral("background_decode_suppressed")] = m_backgroundDecodeSuppressed;
        snapshot[QStringLiteral("scheduled_decode_path_count")] =
            static_cast<qint64>(m_scheduledDecodePaths.size());
        snapshot[QStringLiteral("pending_decode_path_count")] =
            static_cast<qint64>(m_pendingDecodePaths.size());
        snapshot[QStringLiteral("active_decode_path_count")] =
            static_cast<qint64>(m_activeDecodeFullDecode.size());
        snapshot[QStringLiteral("time_stretch_precompute_request_count")] =
            m_timeStretchPrecomputeRequestCount;
        snapshot[QStringLiteral("last_time_stretch_cache_miss_path")] =
            m_lastTimeStretchCacheMissPath;
        const int lastTimeStretchMissSpeed =
            m_lastTimeStretchCacheMissSpeed.load(std::memory_order_acquire);
        snapshot[QStringLiteral("last_time_stretch_cache_miss_speed")] =
            lastTimeStretchMissSpeed;
        snapshot[QStringLiteral("last_time_stretch_expected_sidecar_path")] =
            (!m_lastTimeStretchCacheMissPath.isEmpty() && lastTimeStretchMissSpeed > 1000)
                ? audioTimeStretchSidecarPathForSource(m_lastTimeStretchCacheMissPath,
                                                       lastTimeStretchMissSpeed)
                : QString();
        snapshot[QStringLiteral("time_stretch_precompute_blocked")] =
            m_timeStretchPrecomputeBlocked.load(std::memory_order_acquire);
        snapshot[QStringLiteral("time_stretch_readiness_state")] =
            timeStretchReadinessStateString(
                m_timeStretchReadinessState.load(std::memory_order_acquire));
        {
            std::lock_guard<std::mutex> generationLock(m_timeStretchGenerationMutex);
            const qint64 startedMs =
                m_timeStretchGenerationStartedMs.load(std::memory_order_acquire);
            const qint64 finishedMs =
                m_timeStretchGenerationLastFinishMs.load(std::memory_order_acquire);
            const bool active =
                m_timeStretchGenerationActive.load(std::memory_order_acquire);
            snapshot[QStringLiteral("time_stretch_generation_active")] = active;
            snapshot[QStringLiteral("time_stretch_generation_phase")] =
                timeStretchGenerationPhaseString(
                    m_timeStretchGenerationPhase.load(std::memory_order_acquire));
            snapshot[QStringLiteral("time_stretch_generation_started_ms")] = startedMs;
            snapshot[QStringLiteral("time_stretch_generation_elapsed_ms")] =
                active ? qMax<qint64>(0, QDateTime::currentMSecsSinceEpoch() - startedMs) : 0;
            snapshot[QStringLiteral("time_stretch_generation_last_finish_ms")] = finishedMs;
            snapshot[QStringLiteral("time_stretch_generation_speed_key")] =
                m_timeStretchGenerationSpeedKey.load(std::memory_order_acquire);
            snapshot[QStringLiteral("time_stretch_generation_source_frames")] =
                static_cast<qint64>(
                    m_timeStretchGenerationSourceFrames.load(std::memory_order_acquire));
        snapshot[QStringLiteral("time_stretch_generation_output_frames")] =
            static_cast<qint64>(
                m_timeStretchGenerationOutputFrames.load(std::memory_order_acquire));
            snapshot[QStringLiteral("time_stretch_generation_progress")] =
                static_cast<double>(
                    m_timeStretchGenerationProgressPermille.load(std::memory_order_acquire)) /
                1000.0;
            snapshot[QStringLiteral("time_stretch_generation_last_succeeded")] =
                m_timeStretchGenerationLastSucceeded.load(std::memory_order_acquire);
            snapshot[QStringLiteral("time_stretch_generation_path")] =
                m_timeStretchGenerationPath;
            snapshot[QStringLiteral("time_stretch_generation_sidecar_path")] =
                m_timeStretchGenerationSidecarPath;
            snapshot[QStringLiteral("time_stretch_generation_last_error")] =
                m_timeStretchGenerationLastError;
        }
        snapshot[QStringLiteral("time_stretch_sidecar_only")] = true;
        snapshot[QStringLiteral("pitch_preserving_audio_blocked")] =
            m_pitchPreservingAudioBlocked.load(std::memory_order_acquire);
        snapshot[QStringLiteral("audio_playback_blocked")] =
            m_audioPlaybackBlocked.load(std::memory_order_acquire);
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
        const long streamLatencyFrames = m_rtaudio->getStreamLatency();
        snapshot[QStringLiteral("stream_latency_frames")] = static_cast<qint64>(streamLatencyFrames);
        snapshot[QStringLiteral("stream_latency_ms")] =
            static_cast<qint64>((qMax<long>(0, streamLatencyFrames) * 1000) / qMax(1, m_sampleRate));
        snapshot[QStringLiteral("start_count")] =
            static_cast<qint64>(m_startCount.load(std::memory_order_acquire));
        snapshot[QStringLiteral("redundant_start_count")] =
            static_cast<qint64>(m_redundantStartCount.load(std::memory_order_acquire));
        snapshot[QStringLiteral("seek_count")] =
            static_cast<qint64>(m_seekCount.load(std::memory_order_acquire));
        snapshot[QStringLiteral("last_start_frame")] =
            static_cast<qint64>(m_lastStartFrame.load(std::memory_order_acquire));
        snapshot[QStringLiteral("last_seek_frame")] =
            static_cast<qint64>(m_lastSeekFrame.load(std::memory_order_acquire));
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
        const int64_t audibleSample = playbackClockSample();
        int64_t previous = m_lastReportedCurrentSample.load(std::memory_order_acquire);
        while (audibleSample > previous &&
               !m_lastReportedCurrentSample.compare_exchange_weak(
                   previous, audibleSample, std::memory_order_release, std::memory_order_acquire)) {
        }
        return qMax(previous, audibleSample);
    }

    int64_t playbackClockSample() const {
        const int64_t submittedSample = m_audioClockSample.load(std::memory_order_acquire);
        const qreal playbackRate = qBound<qreal>(
            0.1, m_playbackRate.load(std::memory_order_acquire), 3.0);
        long latencyFrames = 0;
        if (m_rtaudio && m_rtaudio->isStreamOpen()) {
            latencyFrames = m_rtaudio->getStreamLatency();
        }
        const int64_t latencyTimelineSamples = qMax<int64_t>(
            0,
            static_cast<int64_t>(std::llround(
                static_cast<long double>(qMax<long>(0, latencyFrames)) *
                static_cast<long double>(playbackRate))));
        return qMax<int64_t>(0, submittedSample - latencyTimelineSamples);
    }

    int64_t currentFrame() const {
        return samplesToTimelineFrame(currentSample());
    }

    qreal timeStretchGenerationProgress() const {
        return qBound<qreal>(
            0.0,
            static_cast<qreal>(
                m_timeStretchGenerationProgressPermille.load(std::memory_order_acquire)) /
                1000.0,
            1.0);
    }

    bool timeStretchGenerationActive() const {
        return m_timeStretchGenerationActive.load(std::memory_order_acquire);
    }

private:
    struct DecodeTask {
        QString path;
        bool fullDecode = false;
        bool precomputeTimeStretch = false;
        int64_t sourceStartSample = 0;
    };

    struct AudioClipCacheEntry {
        QVector<float> samples;
        int64_t sourceStartSample = 0;
        int sampleRate = 48000;
        int channelCount = 2;
        bool valid = false;
        bool fullyDecoded = false;
    };

    static qreal normalizedTimeStretchSpeed(qreal playbackRate) {
        return qBound<qreal>(0.1, playbackRate, 3.0);
    }

    static int precomputedTimeStretchSpeedKey(qreal playbackRate) {
        const qreal speed = normalizedTimeStretchSpeed(playbackRate);
        if (!pitchPreservingTimeStretchActive(speed)) {
            return 0;
        }
        const int engine = editor::rubberBandEnginePreference() == editor::RubberBandEnginePreference::Faster ? 0 : 1;
        int threading = 0;
        switch (editor::rubberBandThreadingPreference()) {
        case editor::RubberBandThreadingPreference::Never: threading = 1; break;
        case editor::RubberBandThreadingPreference::Always: threading = 2; break;
        case editor::RubberBandThreadingPreference::Auto: threading = 0; break;
        }
        int window = 0;
        switch (editor::rubberBandWindowPreference()) {
        case editor::RubberBandWindowPreference::Short: window = 1; break;
        case editor::RubberBandWindowPreference::Long: window = 2; break;
        case editor::RubberBandWindowPreference::Standard: window = 0; break;
        }
        int pitch = 0;
        switch (editor::rubberBandPitchPreference()) {
        case editor::RubberBandPitchPreference::HighQuality: pitch = 1; break;
        case editor::RubberBandPitchPreference::HighConsistency: pitch = 2; break;
        case editor::RubberBandPitchPreference::HighSpeed: pitch = 0; break;
        }
        const int channels = editor::rubberBandChannelsTogether() ? 1 : 0;
        const int settingsOrdinal =
            engine + (threading * 2) + (window * 6) + (pitch * 18) + (channels * 54);
        return qMax(1, qRound(speed * 1000.0) * 1000 + settingsOrdinal);
    }

    static bool pitchPreservingTimeStretchActive(qreal playbackRate) {
        return qAbs(normalizedTimeStretchSpeed(playbackRate) - 1.0) >= 0.0001;
    }

    static int timeStretchRateKey(qreal playbackRate) {
        return qRound(normalizedTimeStretchSpeed(playbackRate) * 1000.0);
    }

    enum TimeStretchGenerationPhase {
        TimeStretchGenerationIdle = 0,
        TimeStretchGenerationReadingSidecar = 1,
        TimeStretchGenerationRubberBand = 2,
        TimeStretchGenerationWritingSidecar = 3,
        TimeStretchGenerationFinished = 4,
        TimeStretchGenerationFailed = 5,
    };

    static QString timeStretchGenerationPhaseString(int phase) {
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

    enum TimeStretchReadinessState {
        TimeStretchReadinessIdle = 0,
        TimeStretchReadinessNotNeeded = 1,
        TimeStretchReadinessReadyInMemory = 2,
        TimeStretchReadinessReadingSidecar = 3,
        TimeStretchReadinessReadyFromSidecar = 4,
        TimeStretchReadinessQueuedPrecompute = 5,
        TimeStretchReadinessMissing = 6,
    };

    static QString timeStretchReadinessStateString(int state) {
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

    static int64_t timeStretchCacheSampleForSourceSample(int64_t sourceSample, qreal playbackRate) {
        return audioTimeStretchCacheSampleForSourceSample(sourceSample, playbackRate);
    }

    static int64_t timeStretchCacheEndSampleForSourceEndSample(int64_t sourceEndSample, qreal playbackRate) {
        return audioTimeStretchCacheEndSampleForSourceEndSample(sourceEndSample, playbackRate);
    }

    static int64_t sourceSamplesCoveredByTimeStretchCacheSamples(int64_t cacheSamples, qreal playbackRate) {
        return audioTimeStretchSourceSamplesCoveredByCacheSamples(cacheSamples, playbackRate);
    }

    static AudioTimeStretchRubberBandSettings rubberBandSettingsFromRuntimeControls() {
        AudioTimeStretchRubberBandSettings settings;
        settings.engine =
            editor::rubberBandEnginePreference() == editor::RubberBandEnginePreference::Faster
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

    static bool audioEntryCoversSourceSample(const AudioClipCacheEntry& entry,
                                             int64_t sourceSample,
                                             int64_t minFrames = 48000 * 2) {
        if (!entry.valid || entry.samples.isEmpty()) {
            return false;
        }
        const int64_t frameCount =
            static_cast<int64_t>(entry.samples.size() / qMax(1, entry.channelCount));
        const int64_t localSample = sourceSample - entry.sourceStartSample;
        return localSample >= 0 && localSample + qMax<int64_t>(1, minFrames) <= frameCount;
    }

    bool clipAndSourceSampleAtTimelineSampleLocked(int64_t timelineSample,
                                                   const TimelineClip** clipOut,
                                                   QString* audioPathOut,
                                                   int64_t* sourceSampleOut) const {
        for (const TimelineClip& clip : m_timelineClips) {
            if (!clipAudioPlaybackEnabled(clip)) {
                continue;
            }
            const int64_t clipStart = clipTimelineStartSamples(clip);
            const int64_t clipEnd = clipStart + frameToSamples(qMax<int64_t>(1, clip.durationFrames));
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
                *sourceSampleOut =
                    sourceSampleForClipAtTimelineSample(clip, timelineSample, m_renderSyncMarkers);
            }
            return true;
        }
        return false;
    }

    bool audioReadyForTimelineSampleLocked(int64_t timelineSample) const {
        const TimelineClip* clip = nullptr;
        QString audioPath;
        int64_t sourceSample = 0;
        if (!clipAndSourceSampleAtTimelineSampleLocked(
                timelineSample, &clip, &audioPath, &sourceSample)) {
            return true;
        }

        const qreal playbackRate =
            qBound<qreal>(0.1, m_playbackRate.load(std::memory_order_acquire), 3.0);
        const PlaybackAudioWarpMode warpMode = static_cast<PlaybackAudioWarpMode>(
            m_playbackWarpMode.load(std::memory_order_acquire));
        if (warpMode == PlaybackAudioWarpMode::TimeStretch &&
            pitchPreservingTimeStretchActive(playbackRate)) {
            const auto pathIt = m_timeStretchAudioCache.constFind(audioPath);
            if (pathIt == m_timeStretchAudioCache.cend()) {
                return false;
            }
            const int64_t stretchedSourceSample =
                timeStretchCacheSampleForSourceSample(sourceSample, playbackRate);
            const auto speedIt = pathIt.value().constFind(timeStretchRateKey(playbackRate));
            if (speedIt == pathIt.value().cend()) {
                return false;
            }
            for (const AudioClipCacheEntry& entry : speedIt.value()) {
                if (audioEntryCoversSourceSample(entry, stretchedSourceSample, kPlaybackWarmupFrames)) {
                    return true;
                }
            }
            return false;
        }

        return audioEntryCoversSourceSample(m_audioCache.value(audioPath), sourceSample, 1);
    }

    bool ensureTimeStretchAudioReadyForTimelineSample(int64_t timelineSample) {
        QString audioPath;
        qreal playbackRate = 1.0;
        int64_t sourceSample = 0;
        {
            std::lock_guard<std::mutex> lock(m_stateMutex);
            if (audioReadyForTimelineSampleLocked(timelineSample)) {
                m_timeStretchReadinessState.store(TimeStretchReadinessReadyInMemory,
                                                  std::memory_order_release);
                return true;
            }
            if (!clipAndSourceSampleAtTimelineSampleLocked(
                    timelineSample, nullptr, &audioPath, &sourceSample)) {
                m_timeStretchReadinessState.store(TimeStretchReadinessNotNeeded,
                                                  std::memory_order_release);
                return true;
            }
            playbackRate =
                qBound<qreal>(0.1, m_playbackRate.load(std::memory_order_acquire), 3.0);
            const PlaybackAudioWarpMode warpMode = static_cast<PlaybackAudioWarpMode>(
                m_playbackWarpMode.load(std::memory_order_acquire));
            if (warpMode != PlaybackAudioWarpMode::TimeStretch ||
                !pitchPreservingTimeStretchActive(playbackRate)) {
                m_timeStretchReadinessState.store(TimeStretchReadinessNotNeeded,
                                                  std::memory_order_release);
                return false;
            }
        }

        m_timeStretchReadinessState.store(TimeStretchReadinessReadingSidecar,
                                          std::memory_order_release);
        const AudioClipCacheEntry entry = timeStretchCacheForPathCopy(
            audioPath,
            playbackRate,
            sourceSample,
            sourceSample + qMax<int64_t>(1, kPlaybackWarmupFrames));
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

    void requestAudioForTimelineSampleLocked(int64_t timelineSample) {
        const TimelineClip* clip = nullptr;
        QString audioPath;
        int64_t sourceSample = 0;
        if (!clipAndSourceSampleAtTimelineSampleLocked(
                timelineSample, &clip, &audioPath, &sourceSample)) {
            return;
        }
        const qreal playbackRate =
            qBound<qreal>(0.1, m_playbackRate.load(std::memory_order_acquire), 3.0);
        const PlaybackAudioWarpMode warpMode = static_cast<PlaybackAudioWarpMode>(
            m_playbackWarpMode.load(std::memory_order_acquire));
        if (warpMode == PlaybackAudioWarpMode::TimeStretch &&
            pitchPreservingTimeStretchActive(playbackRate)) {
            enqueueTimeStretchPrecomputeForPathLocked(audioPath, 0, true);
            return;
        }
        enqueueDecodePathLocked(
            audioPath,
            true,
            false,
            false,
            qMax<int64_t>(0, sourceSample - kPreviewDecodePrerollFrames),
            true);
    }

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
        engine->m_lastCallbackRequestedSamples.store(
            static_cast<qint64>(samplesNeeded), std::memory_order_release);
        engine->m_lastCallbackReadSamples.store(
            static_cast<qint64>(read), std::memory_order_release);
        engine->m_lastCallbackUnderrunSamples.store(
            static_cast<qint64>(samplesNeeded > read ? samplesNeeded - read : 0),
            std::memory_order_release);

        if (read > 0) {
            const int64_t readFrames =
                static_cast<int64_t>(read / static_cast<size_t>(engine->m_channelCount));
            const qreal playbackRate = qBound<qreal>(
                0.1,
                engine->m_playbackRate.load(std::memory_order_acquire),
                3.0);
            const int64_t timelineAdvance = qMax<int64_t>(
                1,
                static_cast<int64_t>(std::llround(
                    static_cast<long double>(readFrames) *
                    static_cast<long double>(playbackRate))));
            engine->m_audioClockSample.fetch_add(timelineAdvance, std::memory_order_release);
        }
        // Fill remainder with silence on underrun
        if (read < samplesNeeded) {
            std::memset(out + read, 0, (samplesNeeded - read) * sizeof(int16_t));
            engine->m_underrunCount.fetch_add(1, std::memory_order_relaxed);
        }
        if (samplesNeeded >= static_cast<size_t>(engine->m_channelCount)) {
            const size_t lastIndex = samplesNeeded - static_cast<size_t>(engine->m_channelCount);
            engine->m_lastOutputLeft.store(out[lastIndex], std::memory_order_release);
            engine->m_lastOutputRight.store(out[lastIndex + 1], std::memory_order_release);
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

    void enqueueDecodePathLocked(const QString& audioPath,
                                 bool highPriority,
                                 bool fullDecode,
                                 bool precomputeTimeStretch = false,
                                 int64_t sourceStartSample = 0,
                                 bool force = false) {
        if (audioPath.isEmpty()) {
            return;
        }
        const int64_t boundedSourceStartSample = fullDecode ? 0 : qMax<int64_t>(0, sourceStartSample);
        const auto activeIt = m_activeDecodeFullDecode.constFind(audioPath);
        if (activeIt != m_activeDecodeFullDecode.cend()) {
            if (fullDecode && !activeIt.value()) {
                m_fullDecodeRequestedWhileActive.insert(audioPath);
            }
            if (precomputeTimeStretch) {
                m_timeStretchPrecomputeRequestedWhileActive.insert(audioPath, boundedSourceStartSample);
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
            for (auto it = m_pendingDecodePaths.begin(); it != m_pendingDecodePaths.end(); ++it) {
                if (it->path == audioPath) {
                    if (fullDecode && !it->fullDecode) {
                        it->fullDecode = true;
                    }
                    it->precomputeTimeStretch = it->precomputeTimeStretch || precomputeTimeStretch;
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
        task.path = audioPath;
        task.fullDecode = fullDecode;
        task.precomputeTimeStretch = precomputeTimeStretch;
        task.sourceStartSample = boundedSourceStartSample;
        if (highPriority) {
            m_pendingDecodePaths.push_front(task);
        } else {
            m_pendingDecodePaths.push_back(task);
        }
        m_pendingDecodeSet.insert(audioPath);
    }

    void enqueueTimeStretchPrecomputeForScheduledPaths() {
        const qreal speed = qBound<qreal>(
            0.1, m_playbackRate.load(std::memory_order_acquire), 3.0);
        if (!pitchPreservingTimeStretchActive(speed) ||
            static_cast<PlaybackAudioWarpMode>(m_playbackWarpMode.load(std::memory_order_acquire)) !=
                PlaybackAudioWarpMode::TimeStretch) {
            return;
        }

        std::lock_guard<std::mutex> lock(m_stateMutex);
        bool queued = false;
        for (const QString& audioPath : m_scheduledDecodePaths) {
            if (timeStretchCacheHasFullyDecodedPathLocked(audioPath, speed)) {
                continue;
            }
            enqueueDecodePathLocked(audioPath, false, true, true, 0, true);
            ++m_timeStretchPrecomputeRequestCount;
            queued = true;
        }
        if (!queued) {
            m_timeStretchPrecomputeBlocked.store(false, std::memory_order_release);
            return;
        }
        m_timeStretchPrecomputeBlocked.store(true, std::memory_order_release);
        m_decodeCondition.notify_one();
    }

    void enqueueTimeStretchPrecomputeForPath(const QString& audioPath, int64_t sourceStartSample = 0) {
        if (!audioPath.isEmpty()) {
            std::lock_guard<std::mutex> lock(m_stateMutex);
            m_lastTimeStretchCacheMissPath = audioPath;
            enqueueTimeStretchPrecomputeForPathLocked(audioPath, sourceStartSample, true);
        }
        m_timeStretchPrecomputeBlocked.store(true, std::memory_order_release);
        m_decodeCondition.notify_one();
    }

    void enqueueTimeStretchPrecomputeForPathLocked(const QString& audioPath,
                                                   int64_t sourceStartSample,
                                                   bool highPriority) {
        if (audioPath.isEmpty()) {
            return;
        }
        m_lastTimeStretchCacheMissPath = audioPath;
        ++m_timeStretchPrecomputeRequestCount;
        enqueueDecodePathLocked(audioPath, highPriority, true, true, sourceStartSample, true);
    }

    void enqueuePreviewDecodeForPath(const QString& audioPath, int64_t sourceStartSample) {
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

    void removePendingDecodePathLocked(const QString& audioPath) {
        if (audioPath.isEmpty() || !m_pendingDecodeSet.contains(audioPath)) {
            return;
        }
        m_pendingDecodeSet.remove(audioPath);
        for (auto it = m_pendingDecodePaths.begin(); it != m_pendingDecodePaths.end();) {
            if (it->path == audioPath) {
                it = m_pendingDecodePaths.erase(it);
            } else {
                ++it;
            }
        }
    }

    QString clipAudioPathForScheduling(const TimelineClip& clip) const {
        if (!clipAudioPlaybackEnabled(clip) || clip.filePath.isEmpty()) {
            return QString();
        }
        if (clip.audioSourceStatus == QStringLiteral("ok") &&
            !clip.audioSourcePath.trimmed().isEmpty()) {
            return QFileInfo(clip.audioSourcePath).absoluteFilePath();
        }
        if (clip.audioSourceMode == QStringLiteral("embedded")) {
            return QFileInfo(clip.filePath).absoluteFilePath();
        }
        return playbackAudioPathForClip(clip);
    }

    QSet<QString> scheduledAudioPathsFromClips(const QVector<TimelineClip>& clips) const {
        QSet<QString> paths;
        for (const TimelineClip& clip : clips) {
            const QString audioPath = clipAudioPathForScheduling(clip);
            if (!audioPath.isEmpty()) {
                paths.insert(audioPath);
            }
        }
        return paths;
    }

    void scheduleDecodesLocked(const QVector<TimelineClip>& clips) {
        for (const TimelineClip& clip : clips) {
            const QString audioPath = clipAudioPathForScheduling(clip);
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
            const QString audioPath = clipAudioPathForScheduling(clip);
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
            int64_t sourceStartSample = 0;
            for (const TimelineClip& clip : std::as_const(m_timelineClips)) {
                if (clipAudioPathForScheduling(clip) != candidate.path) {
                    continue;
                }
                const int64_t clipStart = clipTimelineStartSamples(clip);
                const int64_t clipEnd = clipStart + frameToSamples(qMax<int64_t>(1, clip.durationFrames));
                const int64_t timelineSample = qBound<int64_t>(clipStart, focusSample, clipEnd - 1);
                sourceStartSample =
                    sourceSampleForClipAtTimelineSample(clip, timelineSample, m_renderSyncMarkers);
                break;
            }
            enqueueDecodePathLocked(candidate.path, true, false, false, sourceStartSample, true);
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

    AudioClipCacheEntry decodeClipAudio(const QString& path,
                                        int64_t maxOutputFrames,
                                        int64_t sourceStartSample = 0) {
        AudioClipCacheEntry cache;
        const int64_t requestedSourceStartSample = qMax<int64_t>(0, sourceStartSample);

        AVFormatContext* formatCtx = nullptr;
        if (avformat_open_input(&formatCtx, QFile::encodeName(path).constData(), nullptr, nullptr) < 0) {
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

        if (requestedSourceStartSample > 0) {
            const AVRational outputSampleTimeBase{1, m_sampleRate};
            const int64_t seekTimestamp = av_rescale_q(
                requestedSourceStartSample,
                outputSampleTimeBase,
                stream->time_base);
            if (av_seek_frame(formatCtx, audioStreamIndex, seekTimestamp, AVSEEK_FLAG_BACKWARD) >= 0) {
                avcodec_flush_buffers(codecCtx);
            }
        }

        AVPacket* packet = av_packet_alloc();
        AVFrame* frame = av_frame_alloc();
        QByteArray converted;
        const bool limitedDecode = maxOutputFrames > 0;
        const int64_t maxOutputSamples =
            limitedDecode ? qMax<int64_t>(1, maxOutputFrames * m_channelCount) : -1;
        bool reachedEof = false;
        bool hitOutputLimit = false;
        int64_t firstOutputSourceSample = -1;
        int64_t nextUnknownOutputSourceSample = requestedSourceStartSample;

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
                int64_t frameStartSourceSample = nextUnknownOutputSourceSample;
                const int64_t bestTimestamp = decoded->best_effort_timestamp;
                if (bestTimestamp != AV_NOPTS_VALUE) {
                    frameStartSourceSample =
                        av_rescale_q(bestTimestamp, stream->time_base, AVRational{1, m_sampleRate});
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
                            qMin<int64_t>(convertedSamples, requestedSourceStartSample - frameStartSourceSample));
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
                const int byteOffset = skipFrames * m_channelCount * static_cast<int>(sizeof(float));
                const int byteCount = retainedFrames * m_channelCount * static_cast<int>(sizeof(float));
                converted.append(reinterpret_cast<const char*>(outData) + byteOffset, byteCount);
                nextUnknownOutputSourceSample =
                    frameStartSourceSample + static_cast<int64_t>(convertedSamples);
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
        cache.sourceStartSample =
            firstOutputSourceSample >= 0 ? firstOutputSourceSample : requestedSourceStartSample;
        cache.valid = !cache.samples.isEmpty();
        cache.fullyDecoded =
            cache.valid && requestedSourceStartSample == 0 && (!limitedDecode || reachedEof);

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

    AudioClipCacheEntry timeStretchCacheForPathCopy(const QString& path,
                                                    qreal speed,
                                                    int64_t sourceSample,
                                                    int64_t sourceEndSampleExclusive) {
        const int rateKey = timeStretchRateKey(speed);
        const int sidecarSpeedKey = precomputedTimeStretchSpeedKey(speed);
        if (!pitchPreservingTimeStretchActive(speed)) {
            return {};
        }
        {
            std::lock_guard<std::mutex> lock(m_stateMutex);
            const auto pathIt = m_timeStretchAudioCache.constFind(path);
            if (pathIt != m_timeStretchAudioCache.cend()) {
                const QVector<AudioClipCacheEntry> segments = pathIt.value().value(rateKey);
                for (const AudioClipCacheEntry& segment : segments) {
                    const int64_t segmentFrames =
                        static_cast<int64_t>(segment.samples.size() / qMax(1, segment.channelCount));
                    if (segment.valid &&
                        audioTimeStretchSegmentCoversSourceRange(segment.sourceStartSample,
                                                                 segmentFrames,
                                                                 sourceSample,
                                                                 sourceEndSampleExclusive,
                                                                 speed)) {
                        return segment;
                    }
                }
            }
        }

        if (sidecarSpeedKey <= 1) {
            return {};
        }
        const AudioClipCacheEntry sidecarEntry =
            audioCacheEntryFromTimeStretchEntry(
                readTimeStretchSidecarEntrySingleFlight(path, sidecarSpeedKey));
        if (!sidecarEntry.valid) {
            return {};
        }
        const int64_t sidecarFrames =
            static_cast<int64_t>(sidecarEntry.samples.size() / qMax(1, sidecarEntry.channelCount));
        if (!audioTimeStretchSegmentCoversSourceRange(sidecarEntry.sourceStartSample,
                                                      sidecarFrames,
                                                      sourceSample,
                                                      sourceEndSampleExclusive,
                                                      speed)) {
            return {};
        }
        {
            std::lock_guard<std::mutex> lock(m_stateMutex);
            m_timeStretchAudioCache[path].insert(rateKey, QVector<AudioClipCacheEntry>{sidecarEntry});
        }
        return sidecarEntry;
    }

    void insertTimeStretchSegmentsLocked(const QString& path,
                                         QHash<int, AudioClipCacheEntry> warpedBySpeed) {
        if (path.isEmpty() || warpedBySpeed.isEmpty()) {
            return;
        }
        auto& pathCache = m_timeStretchAudioCache[path];
        for (auto it = warpedBySpeed.begin(); it != warpedBySpeed.end(); ++it) {
            AudioClipCacheEntry segment = it.value();
            if (!segment.valid) {
                continue;
            }
            QVector<AudioClipCacheEntry>& segments = pathCache[it.key()];
            bool replaced = false;
            for (AudioClipCacheEntry& existing : segments) {
                if (existing.sourceStartSample == segment.sourceStartSample) {
                    existing = std::move(segment);
                    replaced = true;
                    break;
                }
            }
            if (!replaced) {
                segments.push_back(std::move(segment));
            }
            std::sort(segments.begin(), segments.end(), [](const AudioClipCacheEntry& a,
                                                           const AudioClipCacheEntry& b) {
                return a.sourceStartSample < b.sourceStartSample;
            });
            constexpr int kMaxTimeStretchSegmentsPerRate = 6;
            while (segments.size() > kMaxTimeStretchSegmentsPerRate) {
                segments.removeFirst();
            }
        }
    }

    bool timeStretchCacheHasFullyDecodedPathLocked(const QString& path, qreal speed) const {
        const auto pathIt = m_timeStretchAudioCache.constFind(path);
        if (pathIt == m_timeStretchAudioCache.cend()) {
            return false;
        }
        const auto speedIt = pathIt.value().constFind(timeStretchRateKey(speed));
        if (speedIt == pathIt.value().cend()) {
            return false;
        }
        for (const AudioClipCacheEntry& segment : speedIt.value()) {
            if (segment.valid && segment.sourceStartSample == 0 && segment.fullyDecoded) {
                return true;
            }
        }
        return false;
    }

    static AudioClipCacheEntry audioCacheEntryFromTimeStretchEntry(
        const AudioTimeStretchCacheEntry& source)
    {
        AudioClipCacheEntry entry;
        entry.samples = source.samples;
        entry.sourceStartSample = 0;
        entry.sampleRate = source.sampleRate;
        entry.channelCount = source.channelCount;
        entry.valid = source.valid;
        entry.fullyDecoded = source.fullyDecoded;
        return entry;
    }

    static AudioTimeStretchCacheEntry timeStretchEntryFromAudioCacheEntry(
        const AudioClipCacheEntry& source)
    {
        AudioTimeStretchCacheEntry entry;
        entry.samples = source.samples;
        entry.sampleRate = source.sampleRate;
        entry.channelCount = source.channelCount;
        entry.valid = source.valid;
        entry.fullyDecoded = source.fullyDecoded;
        return entry;
    }

    static AudioTimeStretchCacheEntry readTimeStretchSidecarEntry(const QString& path, int speedKey)
    {
        AudioTimeStretchCacheEntry entry;
        readAudioTimeStretchSidecar(path, speedKey, &entry);
        return entry;
    }

    static QString timeStretchSidecarLoadKey(const QString& path, int speedKey)
    {
        return QStringLiteral("%1|%2").arg(path, QString::number(speedKey));
    }

    AudioTimeStretchCacheEntry readTimeStretchSidecarEntrySingleFlight(const QString& path, int speedKey)
    {
        const QString key = timeStretchSidecarLoadKey(path, speedKey);
        {
            std::unique_lock<std::mutex> lock(m_stateMutex);
            while (m_timeStretchSidecarLoadsInFlight.contains(key)) {
                m_stateCondition.wait(lock);
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

        AudioTimeStretchCacheEntry entry = readTimeStretchSidecarEntry(path, speedKey);
        {
            std::lock_guard<std::mutex> lock(m_stateMutex);
            if (entry.valid && entry.fullyDecoded) {
                m_timeStretchSidecarEntryCache.insert(key, entry);
            }
            m_timeStretchSidecarLoadsInFlight.remove(key);
        }
        m_stateCondition.notify_all();
        return entry;
    }

    AudioClipCacheEntry buildTimeStretchCacheEntry(const QString& path,
                                                   const AudioClipCacheEntry& decoded,
                                                   qreal speed) {
        AudioClipCacheEntry warped;
        const int sidecarSpeedKey = precomputedTimeStretchSpeedKey(speed);
        if (!decoded.valid || decoded.samples.isEmpty() || !pitchPreservingTimeStretchActive(speed)) {
            return warped;
        }
        if (sidecarSpeedKey > 1 && decoded.sourceStartSample == 0 && decoded.fullyDecoded) {
            const int64_t sourceFrames =
                static_cast<int64_t>(decoded.samples.size() / qMax(1, decoded.channelCount));
            {
                std::lock_guard<std::mutex> generationLock(m_timeStretchGenerationMutex);
                m_timeStretchGenerationPath = path;
                m_timeStretchGenerationSidecarPath =
                    audioTimeStretchSidecarPathForSource(path, sidecarSpeedKey);
                m_timeStretchGenerationLastError.clear();
                m_timeStretchGenerationStartedMs.store(
                    QDateTime::currentMSecsSinceEpoch(), std::memory_order_release);
                m_timeStretchGenerationLastFinishMs.store(0, std::memory_order_release);
                m_timeStretchGenerationSpeedKey.store(sidecarSpeedKey, std::memory_order_release);
                m_timeStretchGenerationSourceFrames.store(sourceFrames, std::memory_order_release);
                m_timeStretchGenerationOutputFrames.store(0, std::memory_order_release);
                m_timeStretchGenerationProgressPermille.store(0, std::memory_order_release);
                m_timeStretchGenerationLastSucceeded.store(false, std::memory_order_release);
                m_timeStretchGenerationPhase.store(TimeStretchGenerationReadingSidecar,
                                                   std::memory_order_release);
                m_timeStretchGenerationActive.store(true, std::memory_order_release);
            }
            warped = audioCacheEntryFromTimeStretchEntry(
                readTimeStretchSidecarEntrySingleFlight(path, sidecarSpeedKey));
            const int64_t warpedFrames =
                static_cast<int64_t>(warped.samples.size() / qMax(1, warped.channelCount));
            const int64_t minExpectedFrames =
                qMax<int64_t>(1, static_cast<int64_t>(
                    std::floor(static_cast<long double>(sourceFrames) /
                               static_cast<long double>(qMax<qreal>(0.1, speed)) * 0.90L)));
            if (warped.valid && warpedFrames >= minExpectedFrames) {
                m_timeStretchGenerationOutputFrames.store(warpedFrames, std::memory_order_release);
                m_timeStretchGenerationProgressPermille.store(1000, std::memory_order_release);
                m_timeStretchGenerationLastSucceeded.store(true, std::memory_order_release);
                m_timeStretchGenerationPhase.store(TimeStretchGenerationFinished,
                                                   std::memory_order_release);
                m_timeStretchGenerationLastFinishMs.store(
                    QDateTime::currentMSecsSinceEpoch(), std::memory_order_release);
                m_timeStretchGenerationActive.store(false, std::memory_order_release);
                return warped;
            }
        }
        if (sidecarSpeedKey > 1 && decoded.sourceStartSample == 0 && decoded.fullyDecoded) {
            m_timeStretchGenerationPhase.store(TimeStretchGenerationRubberBand,
                                               std::memory_order_release);
            const QVector<float> stretched =
                timeStretchPreservePitch(decoded.samples,
                                         decoded.channelCount,
                                         decoded.sampleRate,
                                         speed,
                                         AudioTimeStretchBackend::RubberBand,
                                         [this](double progress) {
                                             m_timeStretchGenerationProgressPermille.store(
                                                 qBound(0, qRound(progress * 950.0), 950),
                                                 std::memory_order_release);
                                         },
                                         rubberBandSettingsFromRuntimeControls());
            if (!stretched.isEmpty()) {
                const int64_t sourceFrames =
                    static_cast<int64_t>(decoded.samples.size() / qMax(1, decoded.channelCount));
                const int64_t stretchedFrames =
                    static_cast<int64_t>(stretched.size() / qMax(1, decoded.channelCount));
                m_timeStretchGenerationOutputFrames.store(stretchedFrames,
                                                          std::memory_order_release);
                const int64_t minExpectedFrames =
                    qMax<int64_t>(1, static_cast<int64_t>(
                        std::floor(static_cast<long double>(sourceFrames) /
                                   static_cast<long double>(qMax<qreal>(0.1, speed)) * 0.90L)));
                if (stretchedFrames < minExpectedFrames) {
                    qWarning().noquote()
                        << QStringLiteral("Audio time-stretch sidecar generation rejected short output: speed=%1x frames=%2 expected_min=%3 path=\"%4\"")
                               .arg(QString::number(speed, 'f', 3))
                               .arg(stretchedFrames)
                               .arg(minExpectedFrames)
                               .arg(path);
                    {
                        std::lock_guard<std::mutex> generationLock(m_timeStretchGenerationMutex);
                        m_timeStretchGenerationLastError =
                            QStringLiteral("short_output:%1:%2")
                                .arg(stretchedFrames)
                                .arg(minExpectedFrames);
                    }
                    m_timeStretchGenerationPhase.store(TimeStretchGenerationFailed,
                                                       std::memory_order_release);
                    m_timeStretchGenerationLastFinishMs.store(
                        QDateTime::currentMSecsSinceEpoch(), std::memory_order_release);
                    m_timeStretchGenerationActive.store(false, std::memory_order_release);
                    return {};
                }
                AudioTimeStretchCacheEntry sidecar;
                sidecar.samples = stretched;
                sidecar.sampleRate = decoded.sampleRate;
                sidecar.channelCount = decoded.channelCount;
                sidecar.valid = true;
                sidecar.fullyDecoded = true;
                m_timeStretchGenerationPhase.store(TimeStretchGenerationWritingSidecar,
                                                   std::memory_order_release);
                m_timeStretchGenerationProgressPermille.store(980, std::memory_order_release);
                if (writeAudioTimeStretchSidecar(path, sidecarSpeedKey, sidecar)) {
                    warped = audioCacheEntryFromTimeStretchEntry(
                        readTimeStretchSidecarEntrySingleFlight(path, sidecarSpeedKey));
                }
            }
        }
        if (sidecarSpeedKey > 1 && decoded.sourceStartSample == 0 && decoded.fullyDecoded) {
            const bool succeeded = warped.valid;
            if (succeeded) {
                const int64_t warpedFrames =
                    static_cast<int64_t>(warped.samples.size() / qMax(1, warped.channelCount));
                m_timeStretchGenerationOutputFrames.store(warpedFrames,
                                                          std::memory_order_release);
            } else {
                std::lock_guard<std::mutex> generationLock(m_timeStretchGenerationMutex);
                if (m_timeStretchGenerationLastError.isEmpty()) {
                    m_timeStretchGenerationLastError =
                        QStringLiteral("generation_failed_or_empty_output");
                }
            }
            m_timeStretchGenerationLastSucceeded.store(succeeded, std::memory_order_release);
            m_timeStretchGenerationProgressPermille.store(succeeded ? 1000 : 0,
                                                          std::memory_order_release);
            m_timeStretchGenerationPhase.store(succeeded
                                                   ? TimeStretchGenerationFinished
                                                   : TimeStretchGenerationFailed,
                                               std::memory_order_release);
            m_timeStretchGenerationLastFinishMs.store(
                QDateTime::currentMSecsSinceEpoch(), std::memory_order_release);
            m_timeStretchGenerationActive.store(false, std::memory_order_release);
        }
        return warped;
    }

    QHash<int, AudioClipCacheEntry> buildPrecomputedTimeStretchEntries(
        const QString& path,
        const AudioClipCacheEntry& decoded,
        bool precomputeRequested) {
        QHash<int, AudioClipCacheEntry> warpedBySpeed;
        if (!precomputeRequested &&
            static_cast<PlaybackAudioWarpMode>(m_playbackWarpMode.load(std::memory_order_acquire)) !=
            PlaybackAudioWarpMode::TimeStretch) {
            return warpedBySpeed;
        }
        const qreal speed = qBound<qreal>(
            0.1, m_playbackRate.load(std::memory_order_acquire), 3.0);
        if (!pitchPreservingTimeStretchActive(speed)) {
            return warpedBySpeed;
        }
        AudioClipCacheEntry warped = buildTimeStretchCacheEntry(path, decoded, speed);
            if (warped.valid) {
            warpedBySpeed.insert(timeStretchRateKey(speed), std::move(warped));
            }
        return warpedBySpeed;
    }

    QVector<ExportRangeSegment> exportRangesCopy() const {
        std::lock_guard<std::mutex> lock(m_exportRangesMutex);
        return m_exportRanges;
    }

    QVector<ExportRangeSegment> transcriptNormalizeRangesCopy() const {
        std::lock_guard<std::mutex> lock(m_transcriptNormalizeRangesMutex);
        return m_transcriptNormalizeRanges;
    }

    // --- Mix engine ---

    bool mixChunk(const MixContext& context,
                  float* output,
                  int frames,
                  int64_t chunkStartSample,
                  qreal playbackRate) {
        std::fill(output, output + frames * m_channelCount, 0.0f);
        const qreal clampedRate = qBound<qreal>(0.1, playbackRate, 3.0);
        struct PreparedClipAudio {
            struct TranscriptNormalizeSegment {
                int64_t startSample = 0;
                int64_t endSampleExclusive = 0;
                float gain = 1.0f;
            };
            const TimelineClip* clip = nullptr;
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
        const qreal timeStretchSpeed =
            warpMode == PlaybackAudioWarpMode::TimeStretch &&
                    pitchPreservingTimeStretchActive(clampedRate)
                ? clampedRate
                : 1.0;
        auto storeBlockedMixDebug = [&](int reason) {
            m_lastMixPreparedClipCount.store(preparedClips.size(), std::memory_order_release);
            m_lastMixCacheHitCount.store(cacheHitCount, std::memory_order_release);
            m_lastMixCacheMissCount.store(cacheMissCount, std::memory_order_release);
            m_lastMixInvalidAudioCount.store(invalidAudioCount, std::memory_order_release);
            m_lastMixPeakPermille.store(0, std::memory_order_release);
            m_lastMixRmsPermille.store(0, std::memory_order_release);
            m_lastMixNonzeroSampleCount.store(0, std::memory_order_release);
            m_lastMixChunkStartSample.store(chunkStartSample, std::memory_order_release);
            m_lastMixChunkEndSample.store(
                chunkStartSample + static_cast<int64_t>(std::ceil(frames * clampedRate)),
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
            m_lastMixTimeStretchSpeedPermille.store(
                qRound(timeStretchSpeed * 1000.0), std::memory_order_release);
            m_lastMixSilentReason.store(reason, std::memory_order_release);
            m_pitchPreservingAudioBlocked.store(
                qAbs(timeStretchSpeed - 1.0) >= 0.0001, std::memory_order_release);
            m_audioPlaybackBlocked.store(true, std::memory_order_release);
        };
        for (const TimelineClip& clip : context.clips) {
            if (!clipAudioPlaybackEnabled(clip)) {
                continue;
            }
            const QString audioPath = clipAudioPathForScheduling(clip);
            AudioClipCacheEntry audio;
            bool usingPrecomputedTimeStretch = false;
            const int64_t clipStartSampleForLookup = clipTimelineStartSamples(clip);
            const int64_t clipEndSampleForLookup =
                clipStartSampleForLookup + frameToSamples(qMax<int64_t>(1, clip.durationFrames));
            const int64_t lookupTimelineSample =
                qBound<int64_t>(clipStartSampleForLookup, chunkStartSample, clipEndSampleForLookup - 1);
            const int64_t chunkTimelineStep = qMax<int64_t>(
                1,
                static_cast<int64_t>(std::llround(
                    clampedRate * static_cast<qreal>(frames))));
            const int64_t lookupEndTimelineSample =
                qBound<int64_t>(lookupTimelineSample + 1,
                                lookupTimelineSample + chunkTimelineStep,
                                clipEndSampleForLookup);
            const int64_t lookupSourceSample =
                sourceSampleForClipAtTimelineSample(clip, lookupTimelineSample, context.renderSyncMarkers);
            const int64_t lookupSourceEndSampleExclusive =
                sourceSampleForClipAtTimelineSample(
                    clip, qMax<int64_t>(lookupTimelineSample, lookupEndTimelineSample - 1),
                    context.renderSyncMarkers) + 1;
            if (qAbs(timeStretchSpeed - 1.0) >= 0.0001) {
                audio = timeStretchCacheForPathCopy(audioPath,
                                                    timeStretchSpeed,
                                                    lookupSourceSample,
                                                    lookupSourceEndSampleExclusive);
                usingPrecomputedTimeStretch = audio.valid;
                if (!usingPrecomputedTimeStretch) {
                    ++cacheMissCount;
                    m_timeStretchCacheMissCount.fetch_add(1, std::memory_order_relaxed);
                    m_lastTimeStretchCacheMissSpeed.store(
                        timeStretchRateKey(timeStretchSpeed), std::memory_order_release);
                    const int64_t clipStartSample = clipTimelineStartSamples(clip);
                    const int64_t clipEndSample =
                        clipStartSample + frameToSamples(qMax<int64_t>(1, clip.durationFrames));
                    const int64_t timelineSample =
                        qBound<int64_t>(clipStartSample, chunkStartSample, clipEndSample - 1);
                    const int64_t sourceSample =
                        sourceSampleForClipAtTimelineSample(
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
                        m_lastTimeStretchCacheMissWarningMs.store(now, std::memory_order_release);
                        qWarning().noquote()
                            << QStringLiteral("Audio time-stretch cache miss: speed=%1x path=\"%2\"; "
                                              "holding playback audio until pitch-preserving audio is ready.")
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
                    const int64_t clipEndSample =
                        clipStartSample + frameToSamples(qMax<int64_t>(1, clip.durationFrames));
                    const int64_t timelineSample =
                        qBound<int64_t>(clipStartSample, chunkStartSample, clipEndSample - 1);
                    enqueuePreviewDecodeForPath(
                        audioPath,
                        sourceSampleForClipAtTimelineSample(
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
            const int64_t clipAvailableTimelineSamples = usingPrecomputedTimeStretch
                ? sourceSamplesCoveredByTimeStretchCacheSamples(clipAvailableSamples, timeStretchSpeed)
                : clipAvailableSamples;
            const int64_t timelineClipSamples =
                clip.durationFrames * m_sampleRate / kTimelineFps;
            const int64_t clipEndSample = clipStartSample + timelineClipSamples;
            if (clipEndSample <= clipStartSample) {
                continue;
            }
            const int64_t segmentEndSample = clipStartSample + clipAvailableTimelineSamples;
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
                    0,
                    sourceFramesToSamples(clip, static_cast<qreal>(qMax<int64_t>(0, clip.sourceDurationFrames))) - 1);
            prepared.playbackRateScaled =
                qMax<int64_t>(1, static_cast<int64_t>(clip.playbackRate * 1000.0));
            prepared.precomputedTimeStretchSpeed = usingPrecomputedTimeStretch ? timeStretchSpeed : 1.0;
            prepared.linearSourceMapping = context.renderSyncMarkers.isEmpty();
            preparedClips.push_back(prepared);

            if (usingPrecomputedTimeStretch) {
                const int64_t segmentFrames =
                    static_cast<int64_t>(audio.samples.size() / qMax(1, audio.channelCount));
                const int64_t normalizedLookupSample =
                    timeStretchCacheSampleForSourceSample(lookupSourceSample, timeStretchSpeed);
                const int64_t remainingSegmentSamples =
                    (audio.sourceStartSample + segmentFrames) - normalizedLookupSample;
                constexpr int64_t kTimeStretchPrefetchLeadSamples = m_sampleRate * 5;
                if (remainingSegmentSamples > 0 &&
                    remainingSegmentSamples < kTimeStretchPrefetchLeadSamples) {
                    const int64_t nextSourceSample =
                        sourceSamplesCoveredByTimeStretchCacheSamples(
                            audio.sourceStartSample + segmentFrames,
                            timeStretchSpeed);
                    enqueueTimeStretchPrecomputeForPath(audioPath, nextSourceSample);
                }
            }
        }

        const bool transcriptNormalizeEnabled =
            m_transcriptNormalizeEnabled.load(std::memory_order_acquire);
        const bool blockedWaitingForPlayableAudio =
            preparedClips.isEmpty() && (cacheMissCount > 0 || invalidAudioCount > 0);
        if (blockedWaitingForPlayableAudio) {
            storeBlockedMixDebug(SilentReasonWaitingForPlayableAudio);
            return false;
        }
        m_pitchPreservingAudioBlocked.store(false, std::memory_order_release);
        m_audioPlaybackBlocked.store(false, std::memory_order_release);
        m_timeStretchPrecomputeBlocked.store(false, std::memory_order_release);

        const QVector<ExportRangeSegment> transcriptNormalizeRanges = transcriptNormalizeRangesCopy();
        if (transcriptNormalizeEnabled && !transcriptNormalizeRanges.isEmpty()) {
            constexpr float kTranscriptNormalizeTargetLinear = 0.95f;
            constexpr float kMaxTranscriptNormalizeGain = 2.5f;
            for (PreparedClipAudio& prepared : preparedClips) {
                for (const ExportRangeSegment& range : transcriptNormalizeRanges) {
                    const int64_t rangeStartSample = timelineFrameToSamples(range.startFrame);
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
                    const int64_t sourceEndSampleExclusive = sourceSampleForClipAtTimelineSample(
                        *prepared.clip, overlapEndExclusive - 1, context.renderSyncMarkers) + 1;
                    const int64_t clipSampleCount =
                        static_cast<int64_t>(prepared.audio.samples.size() / m_channelCount);
                    const int64_t normalizedSourceStart =
                        timeStretchCacheSampleForSourceSample(
                            sourceStartSample,
                            prepared.precomputedTimeStretchSpeed);
                    const int64_t normalizedSourceEndExclusive =
                        timeStretchCacheEndSampleForSourceEndSample(
                            sourceEndSampleExclusive,
                            prepared.precomputedTimeStretchSpeed);
                    const int64_t localSourceStart =
                        normalizedSourceStart - prepared.audio.sourceStartSample;
                    const int64_t localSourceEndExclusive =
                        normalizedSourceEndExclusive - prepared.audio.sourceStartSample;
                    const int64_t clampedStart = qBound<int64_t>(0, localSourceStart, clipSampleCount);
                    const int64_t clampedEndExclusive =
                        qBound<int64_t>(clampedStart, localSourceEndExclusive, clipSampleCount);
                    float transcriptPeak = 0.0f;
                    for (int64_t sample = clampedStart; sample < clampedEndExclusive; ++sample) {
                        const int index = static_cast<int>(sample * m_channelCount);
                        transcriptPeak = qMax(transcriptPeak, std::abs(prepared.audio.samples[index]));
                        transcriptPeak = qMax(transcriptPeak, std::abs(prepared.audio.samples[index + 1]));
                    }
                    if (transcriptPeak <= 0.000001f) {
                        continue;
                    }

                    PreparedClipAudio::TranscriptNormalizeSegment segment;
                    segment.startSample = overlapStart;
                    segment.endSampleExclusive = overlapEndExclusive;
                    segment.gain = qMin(
                        kMaxTranscriptNormalizeGain,
                        kTranscriptNormalizeTargetLinear / transcriptPeak);
                    prepared.transcriptNormalizeSegments.push_back(segment);
                }
                if (!prepared.transcriptNormalizeSegments.isEmpty()) {
                    std::sort(prepared.transcriptNormalizeSegments.begin(),
                              prepared.transcriptNormalizeSegments.end(),
                              [](const PreparedClipAudio::TranscriptNormalizeSegment& a,
                                 const PreparedClipAudio::TranscriptNormalizeSegment& b) {
                                  return a.startSample < b.startSample;
                              });
                }
            }
        }

        auto transcriptNormalizeGainAtSample = [](const PreparedClipAudio& prepared,
                                                  int64_t timelineSamplePos) -> float {
            if (prepared.transcriptNormalizeSegments.isEmpty()) {
                return 1.0f;
            }
            constexpr int64_t kTransitionSamples = 480; // 10 ms at 48 kHz
            constexpr int64_t kInterWordBridgeSamples = 5760; // 120 ms at 48 kHz
            const auto& segments = prepared.transcriptNormalizeSegments;
            const auto it = std::upper_bound(
                segments.begin(),
                segments.end(),
                timelineSamplePos,
                [](int64_t sample, const PreparedClipAudio::TranscriptNormalizeSegment& segment) {
                    return sample < segment.startSample;
                });

            int index = -1;
            if (it != segments.begin()) {
                const int candidateIndex = static_cast<int>(std::distance(segments.begin(), it - 1));
                const auto& candidate = segments[static_cast<qsizetype>(candidateIndex)];
                if (timelineSamplePos < candidate.endSampleExclusive) {
                    index = candidateIndex;
                }
            }
            if (index < 0) {
                const int nextIndex = static_cast<int>(std::distance(segments.begin(), it));
                const int prevIndex = nextIndex - 1;
                if (prevIndex >= 0 && nextIndex < segments.size()) {
                    const auto& prev = segments[static_cast<qsizetype>(prevIndex)];
                    const auto& next = segments[static_cast<qsizetype>(nextIndex)];
                    const int64_t gapStart = prev.endSampleExclusive;
                    const int64_t gapEnd = next.startSample;
                    const int64_t gapLen = gapEnd - gapStart;
                    if (timelineSamplePos >= gapStart &&
                        timelineSamplePos < gapEnd &&
                        gapLen > 0 &&
                        gapLen <= kInterWordBridgeSamples) {
                        const float t = static_cast<float>(timelineSamplePos - gapStart) /
                                        static_cast<float>(gapLen);
                        return prev.gain + ((next.gain - prev.gain) * qBound(0.0f, t, 1.0f));
                    }
                }
                return 1.0f;
            }

            const auto& current = segments[static_cast<qsizetype>(index)];
            const float currentGain = current.gain;

            float gain = currentGain;
            const float previousGain =
                index > 0 ? segments[static_cast<qsizetype>(index - 1)].gain : 1.0f;
            const float nextGain =
                (index + 1) < segments.size() ? segments[static_cast<qsizetype>(index + 1)].gain : 1.0f;

            const int64_t startFadeLen = qMin<int64_t>(
                kTransitionSamples, qMax<int64_t>(1, current.endSampleExclusive - current.startSample));
            if (timelineSamplePos < current.startSample + startFadeLen) {
                const float t = static_cast<float>(timelineSamplePos - current.startSample) /
                                static_cast<float>(startFadeLen);
                gain = previousGain + ((currentGain - previousGain) * qBound(0.0f, t, 1.0f));
            }

            const int64_t endFadeLen = qMin<int64_t>(
                kTransitionSamples, qMax<int64_t>(1, current.endSampleExclusive - current.startSample));
            if (timelineSamplePos >= current.endSampleExclusive - endFadeLen) {
                const float t = static_cast<float>(timelineSamplePos - (current.endSampleExclusive - endFadeLen)) /
                                static_cast<float>(endFadeLen);
                const float endGain = currentGain + ((nextGain - currentGain) * qBound(0.0f, t, 1.0f));
                gain = (timelineSamplePos < current.startSample + startFadeLen)
                    ? (0.5f * (gain + endGain))
                    : endGain;
            }

            return gain;
        };
        auto sourceSampleAtTimelineSample = [&context](const PreparedClipAudio& prepared,
                                                       int64_t timelineSamplePos) -> int64_t {
            if (prepared.linearSourceMapping) {
                const int64_t localTimelineSample =
                    qMax<int64_t>(0, timelineSamplePos - prepared.clipStartSample);
                const int64_t sourceOffset =
                    (localTimelineSample * prepared.playbackRateScaled) / 1000;
                return qMax<int64_t>(
                    0,
                    qMin<int64_t>(prepared.sourceInSample + sourceOffset,
                                  prepared.maxSourceSample));
            }
            return sourceSampleForClipAtTimelineSample(
                *prepared.clip,
                timelineSamplePos,
                context.renderSyncMarkers);
        };

        int framesWithActiveClip = 0;
        int framesInputOutOfRange = 0;
        int framesSpeechGainZero = 0;
        int framesClipGainZero = 0;
        int framesSourceNonzero = 0;
        float sourcePeak = 0.0f;
        float primaryGainPeak = 0.0f;

        for (int outFrame = 0; outFrame < frames; ++outFrame) {
            const int64_t timelineOffset =
                static_cast<int64_t>(std::floor(static_cast<qreal>(outFrame) * clampedRate));
            const int64_t timelineSamplePos = chunkStartSample + timelineOffset;
            const int outIndex = outFrame * m_channelCount;
            bool frameHadActiveClip = false;
            bool frameInputOutOfRange = false;
            bool frameSpeechGainZero = false;
            bool frameClipGainZero = false;
            bool frameSourceNonzero = false;

            for (const PreparedClipAudio& prepared : preparedClips) {
                const TimelineClip& clip = *prepared.clip;
                const AudioClipCacheEntry& audio = prepared.audio;
                if (timelineSamplePos < prepared.clipStartSample ||
                    timelineSamplePos >= prepared.clipEndSample) {
                    continue;
                }
                frameHadActiveClip = true;

                const int64_t sourceFrame = sourceSampleAtTimelineSample(prepared, timelineSamplePos);
                int64_t inFrame = sourceFrame;
                inFrame = timeStretchCacheSampleForSourceSample(
                    inFrame,
                    prepared.precomputedTimeStretchSpeed);
                const int64_t localInFrame = inFrame - audio.sourceStartSample;
                if (localInFrame < 0 || localInFrame >= (audio.samples.size() / m_channelCount)) {
                    frameInputOutOfRange = true;
                    m_lastMixOutOfRangeTimelineSample.store(timelineSamplePos, std::memory_order_release);
                    m_lastMixOutOfRangeSourceSample.store(sourceFrame, std::memory_order_release);
                    m_lastMixOutOfRangeNormalizedSample.store(inFrame, std::memory_order_release);
                    m_lastMixOutOfRangeAudioStartSample.store(audio.sourceStartSample, std::memory_order_release);
                    m_lastMixOutOfRangeAudioEndSample.store(
                        audio.sourceStartSample +
                            static_cast<int64_t>(audio.samples.size() / qMax(1, audio.channelCount)),
                        std::memory_order_release);
                    if (pitchPreservingTimeStretchActive(prepared.precomputedTimeStretchSpeed)) {
                        enqueueTimeStretchPrecomputeForPath(
                            clipAudioPathForScheduling(clip),
                            sourceFrame);
                    } else {
                        enqueuePreviewDecodeForPath(clipAudioPathForScheduling(clip), inFrame);
                    }
                    storeBlockedMixDebug(SilentReasonInputOutOfRange);
                    return false;
                }
                const int inIndex = static_cast<int>(localInFrame * m_channelCount);

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
                    prepared.clipStartSample,
                    prepared.clipEndSample,
                    clip.fadeSamples > 0 ? clip.fadeSamples : m_defaultFadeSamples);
                const float transcriptNormalizeGain =
                    transcriptNormalizeGainAtSample(prepared, timelineSamplePos);
                const float primaryGain =
                    primarySpeechGain * primaryClipGain * transcriptNormalizeGain;
                primaryGainPeak = qMax(primaryGainPeak, std::abs(primaryGain));
                if (primarySpeechGain <= 0.0f && secondarySpeechGain <= 0.0f) {
                    frameSpeechGainZero = true;
                }
                if (primaryClipGain <= 0.0f) {
                    frameClipGainZero = true;
                }
                const float sourceFramePeak =
                    qMax(std::abs(audio.samples[inIndex]), std::abs(audio.samples[inIndex + 1]));
                sourcePeak = qMax(sourcePeak, sourceFramePeak);
                if (sourceFramePeak > 0.000001f) {
                    frameSourceNonzero = true;
                }
                if (primaryGain > 0.0f) {
                    output[outIndex] += audio.samples[inIndex] * primaryGain;
                    output[outIndex + 1] += audio.samples[inIndex + 1] * primaryGain;
                }

                if (secondarySpeechGain > 0.0f && secondaryTimelineSample >= 0) {
                    int64_t secondaryInFrame =
                        sourceSampleAtTimelineSample(prepared, secondaryTimelineSample);
                    secondaryInFrame = timeStretchCacheSampleForSourceSample(
                        secondaryInFrame,
                        prepared.precomputedTimeStretchSpeed);
                    if (secondaryInFrame >= 0 &&
                        secondaryInFrame - audio.sourceStartSample < (audio.samples.size() / m_channelCount)) {
                        const int64_t localSecondaryInFrame =
                            secondaryInFrame - audio.sourceStartSample;
                        if (localSecondaryInFrame < 0) {
                            continue;
                        }
                        const int secondaryInIndex =
                            static_cast<int>(localSecondaryInFrame * m_channelCount);
                        const float secondaryClipGain = calculateClipCrossfadeGain(
                            secondaryTimelineSample,
                            clip,
                            prepared.clipStartSample,
                            prepared.clipEndSample,
                            clip.fadeSamples > 0 ? clip.fadeSamples : m_defaultFadeSamples);
                        const float secondaryGain =
                            secondarySpeechGain * secondaryClipGain * transcriptNormalizeGain;
                        output[outIndex] += audio.samples[secondaryInIndex] * secondaryGain;
                        output[outIndex + 1] += audio.samples[secondaryInIndex + 1] * secondaryGain;
                    }
                }
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
        const bool normalizeEnabled = m_normalizeEnabled.load(std::memory_order_acquire);
        const qreal normalizeTargetDb = m_normalizeTargetDb.load(std::memory_order_acquire);
        const bool selectiveNormalizeEnabled =
            m_selectiveNormalizeEnabled.load(std::memory_order_acquire);
        const qreal selectiveNormalizeMinSegmentSeconds =
            m_selectiveNormalizeMinSegmentSeconds.load(std::memory_order_acquire);
        const qreal selectiveNormalizePeakDb =
            m_selectiveNormalizePeakDb.load(std::memory_order_acquire);
        const int selectiveNormalizePasses =
            m_selectiveNormalizePasses.load(std::memory_order_acquire);
        const bool peakReductionEnabled = m_peakReductionEnabled.load(std::memory_order_acquire);
        const qreal peakThresholdDb = m_peakThresholdDb.load(std::memory_order_acquire);
        const bool limiterEnabled = m_limiterEnabled.load(std::memory_order_acquire);
        const qreal limiterThresholdDb = m_limiterThresholdDb.load(std::memory_order_acquire);
        const bool compressorEnabled = m_compressorEnabled.load(std::memory_order_acquire);
        const qreal compressorThresholdDb = m_compressorThresholdDb.load(std::memory_order_acquire);
        const qreal compressorRatio = m_compressorRatio.load(std::memory_order_acquire);

        auto dbToAmpLocal = [](float db) -> float {
            return std::pow(10.0f, db / 20.0f);
        };

        const float amplifyGain = amplifyEnabled ? dbToAmpLocal(static_cast<float>(amplifyDb)) : 1.0f;
        const float normalizeTargetLinear = dbToAmpLocal(
            std::clamp(static_cast<float>(normalizeTargetDb), -24.0f, 0.0f));
        const float selectiveThresholdLinear = dbToAmpLocal(
            std::clamp(static_cast<float>(selectiveNormalizePeakDb), -36.0f, 0.0f));
        const float peakLinear = dbToAmpLocal(std::clamp(static_cast<float>(peakThresholdDb), -24.0f, 0.0f));
        const float limiterLinear = dbToAmpLocal(std::clamp(static_cast<float>(limiterThresholdDb), -12.0f, 0.0f));
        const float compLinear = dbToAmpLocal(std::clamp(static_cast<float>(compressorThresholdDb), -30.0f, -1.0f));
        const float compRatio = std::clamp(static_cast<float>(compressorRatio), 1.0f, 20.0f);
        const int safeSelectivePasses = qBound(1, selectiveNormalizePasses, 8);
        const float safeMinSegmentSeconds =
            std::clamp(static_cast<float>(selectiveNormalizeMinSegmentSeconds), 0.1f, 30.0f);
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
            const int binCount = qMax(1, static_cast<int>(std::ceil(
                                        static_cast<float>(frames) / static_cast<float>(kAnalysisWindowFrames))));
            QVector<float> binPeaks(binCount, 0.0f);
            auto rebuildBinPeaks = [&]() {
                std::fill(binPeaks.begin(), binPeaks.end(), 0.0f);
                for (int f = 0; f < frames; ++f) {
                    const int idx = f * m_channelCount;
                    const float peak = qMax(std::abs(output[idx]), std::abs(output[idx + 1]));
                    const int bin = qMin(binCount - 1, f / kAnalysisWindowFrames);
                    binPeaks[bin] = qMax(binPeaks[bin], peak);
                }
            };
            rebuildBinPeaks();

            const int minBins = qMax(
                1, static_cast<int>(std::ceil(
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
                    const int endFrameExclusive = qMin(frames, (endBinInclusive + 1) * kAnalysisWindowFrames);
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

        const float masterGain = context.muted ? 0.0f : static_cast<float>(context.volume);
        double sumSquares = 0.0;
        float peak = 0.0f;
        int nonzeroSamples = 0;
        int outputNonzeroFrames = 0;
        for (int i = 0; i < frames * m_channelCount; ++i) {
            output[i] = qBound(-1.0f, output[i] * masterGain, 1.0f);
            const float absSample = std::abs(output[i]);
            peak = qMax(peak, absSample);
            sumSquares += static_cast<double>(output[i]) * static_cast<double>(output[i]);
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
        const float rms = static_cast<float>(std::sqrt(sumSquares / static_cast<double>(sampleCount)));
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
        int64_t firstClipStart = 0;
        int64_t firstClipEnd = 0;
        int64_t firstAudioStart = 0;
        int64_t firstAudioFrameCount = 0;
        int64_t firstLocalSampleAtChunkStart = 0;
        if (!preparedClips.isEmpty()) {
            const PreparedClipAudio& first = preparedClips.first();
            firstClipStart = first.clipStartSample;
            firstClipEnd = first.clipEndSample;
            firstAudioStart = first.audio.sourceStartSample;
            firstAudioFrameCount = first.audio.samples.size() / m_channelCount;
            int64_t firstInFrame = sourceSampleForClipAtTimelineSample(
                *first.clip,
                qBound<int64_t>(first.clipStartSample, chunkStartSample, first.clipEndSample - 1),
                context.renderSyncMarkers);
            firstInFrame = timeStretchCacheSampleForSourceSample(
                firstInFrame,
                first.precomputedTimeStretchSpeed);
            firstLocalSampleAtChunkStart = firstInFrame - first.audio.sourceStartSample;
        }
        m_lastMixPreparedClipCount.store(preparedClips.size(), std::memory_order_release);
        m_lastMixCacheHitCount.store(cacheHitCount, std::memory_order_release);
        m_lastMixCacheMissCount.store(cacheMissCount, std::memory_order_release);
        m_lastMixInvalidAudioCount.store(invalidAudioCount, std::memory_order_release);
        m_lastMixPeakPermille.store(qRound(peak * 1000.0f), std::memory_order_release);
        m_lastMixRmsPermille.store(qRound(rms * 1000.0f), std::memory_order_release);
        m_lastMixNonzeroSampleCount.store(nonzeroSamples, std::memory_order_release);
        m_lastMixChunkStartSample.store(chunkStartSample, std::memory_order_release);
        m_lastMixChunkEndSample.store(
            chunkStartSample + static_cast<int64_t>(std::ceil(frames * clampedRate)),
            std::memory_order_release);
        m_lastMixFramesWithActiveClip.store(framesWithActiveClip, std::memory_order_release);
        m_lastMixFramesInputOutOfRange.store(framesInputOutOfRange, std::memory_order_release);
        m_lastMixFramesSpeechGainZero.store(framesSpeechGainZero, std::memory_order_release);
        m_lastMixFramesClipGainZero.store(framesClipGainZero, std::memory_order_release);
        m_lastMixFramesSourceNonzero.store(framesSourceNonzero, std::memory_order_release);
        m_lastMixFramesOutputNonzero.store(outputNonzeroFrames, std::memory_order_release);
        m_lastMixSourcePeakPermille.store(qRound(sourcePeak * 1000.0f), std::memory_order_release);
        m_lastMixPrimaryGainPeakPermille.store(qRound(primaryGainPeak * 1000.0f), std::memory_order_release);
        m_lastMixTimeStretchSpeedPermille.store(
            qRound(timeStretchSpeed * 1000.0), std::memory_order_release);
        m_lastMixFirstClipStartSample.store(firstClipStart, std::memory_order_release);
        m_lastMixFirstClipEndSample.store(firstClipEnd, std::memory_order_release);
        m_lastMixFirstAudioStartSample.store(firstAudioStart, std::memory_order_release);
        m_lastMixFirstAudioFrameCount.store(firstAudioFrameCount, std::memory_order_release);
        m_lastMixFirstLocalSampleAtChunkStart.store(firstLocalSampleAtChunkStart, std::memory_order_release);
        m_lastMixSilentReason.store(silentReason, std::memory_order_release);
        return true;
    }

    // --- Worker threads ---

    void decodeLoop() {
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
                m_pendingDecodeSet.remove(nextTask.path);
                m_activeDecodeFullDecode.insert(nextTask.path, nextTask.fullDecode);
            }

            AudioClipCacheEntry decoded = decodeClipAudio(
                nextTask.path,
                nextTask.fullDecode
                    ? -1
                    : (nextTask.precomputeTimeStretch
                           ? kTimeStretchPreviewDecodeFrames
                           : kPreviewDecodeFrames),
                nextTask.fullDecode ? 0 : nextTask.sourceStartSample);
            QHash<int, AudioClipCacheEntry> warpedBySpeed =
                buildPrecomputedTimeStretchEntries(
                    nextTask.path,
                    decoded,
                    nextTask.precomputeTimeStretch);

            {
                std::lock_guard<std::mutex> lock(m_stateMutex);
                const bool fullDecodeRequestedWhileActive =
                    m_fullDecodeRequestedWhileActive.remove(nextTask.path) > 0;
                const bool timeStretchRequestedWhileActive =
                    m_timeStretchPrecomputeRequestedWhileActive.contains(nextTask.path);
                const int64_t activeTimeStretchSourceStart =
                    m_timeStretchPrecomputeRequestedWhileActive.take(nextTask.path);
                m_activeDecodeFullDecode.remove(nextTask.path);
                if (nextTask.fullDecode) {
                    if (decoded.valid) {
                        m_audioCache.insert(nextTask.path, decoded);
                        if (!warpedBySpeed.isEmpty()) {
                            insertTimeStretchSegmentsLocked(nextTask.path, std::move(warpedBySpeed));
                            m_timeStretchPrecomputeBlocked.store(false, std::memory_order_release);
                        }
                    }
                } else {
                    if (decoded.valid) {
                        m_audioCache.insert(nextTask.path, decoded);
                        if (!warpedBySpeed.isEmpty()) {
                            insertTimeStretchSegmentsLocked(nextTask.path, std::move(warpedBySpeed));
                            m_timeStretchPrecomputeBlocked.store(false, std::memory_order_release);
                        }
                    }
                }
                if (fullDecodeRequestedWhileActive && !nextTask.fullDecode) {
                    enqueueDecodePathLocked(nextTask.path, true, true, true, 0, true);
                } else if (timeStretchRequestedWhileActive && !nextTask.precomputeTimeStretch) {
                    enqueueDecodePathLocked(
                        nextTask.path,
                        true,
                        nextTask.fullDecode,
                        true,
                        nextTask.fullDecode ? 0 : activeTimeStretchSourceStart,
                        true);
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

            if (!mixChunk(context, mixBuffer.data(), m_periodFrames, chunkStartSample, playbackRate)) {
                std::lock_guard<std::mutex> lock(m_stateMutex);
                m_timelineSampleCursor = chunkStartSample;
                m_ringBufferEndSample.store(chunkStartSample, std::memory_order_release);
                continue;
            }

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
    mutable std::mutex m_transcriptNormalizeRangesMutex;
    std::mutex m_mixMutex;
    std::condition_variable m_stateCondition;
    std::condition_variable m_decodeCondition;
    std::condition_variable m_mixCondition;

    QVector<TimelineClip> m_timelineClips;
    QVector<RenderSyncMarker> m_renderSyncMarkers;
    QVector<ExportRangeSegment> m_exportRanges;
    QVector<ExportRangeSegment> m_transcriptNormalizeRanges;
    QHash<QString, AudioClipCacheEntry> m_audioCache;
    QHash<QString, QHash<int, QVector<AudioClipCacheEntry>>> m_timeStretchAudioCache;
    QHash<QString, AudioTimeStretchCacheEntry> m_timeStretchSidecarEntryCache;
    QSet<QString> m_timeStretchSidecarLoadsInFlight;
    std::deque<DecodeTask> m_pendingDecodePaths;
    QSet<QString> m_pendingDecodeSet;
    QHash<QString, bool> m_activeDecodeFullDecode;
    QSet<QString> m_fullDecodeRequestedWhileActive;
    QHash<QString, int64_t> m_timeStretchPrecomputeRequestedWhileActive;
    QSet<QString> m_scheduledDecodePaths;

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
    std::atomic<qint64> m_lastTimeStretchCacheMissWarningMs{0};
    std::atomic<qint64> m_timeStretchCacheMissCount{0};
    std::atomic<int> m_lastTimeStretchCacheMissSpeed{0};
    std::atomic<bool> m_pitchPreservingAudioBlocked{false};
    std::atomic<bool> m_audioPlaybackBlocked{false};
    std::atomic<bool> m_timeStretchPrecomputeBlocked{false};
    std::atomic<int> m_timeStretchReadinessState{TimeStretchReadinessIdle};
    mutable std::mutex m_timeStretchGenerationMutex;
    mutable QString m_timeStretchGenerationPath;
    mutable QString m_timeStretchGenerationSidecarPath;
    mutable QString m_timeStretchGenerationLastError;
    mutable std::atomic<bool> m_timeStretchGenerationActive{false};
    mutable std::atomic<int> m_timeStretchGenerationPhase{TimeStretchGenerationIdle};
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
    std::atomic<int> m_playbackWarpMode{static_cast<int>(PlaybackAudioWarpMode::Disabled)};

    static constexpr int m_sampleRate = 48000;
    static constexpr int m_channelCount = 2;
    static constexpr int m_periodFrames = 1024;
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
    qint64 m_audioInitBackoffUntilMs = 0;
    qint64 m_lastAudioInitWarningMs = 0;
    qint64 m_lastKnownDeviceCount = 0;
    bool m_lastKnownDefaultOutputValid = false;
    unsigned int m_lastKnownDefaultOutputId = 0;
    QString m_lastKnownDefaultOutputName;
    int m_lastKnownDefaultOutputChannels = 0;
    QString m_lastDeviceInfoError;
};
