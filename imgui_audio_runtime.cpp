#include "imgui_audio_runtime.h"

#include "RtAudio.h"
#include "standalone_audio_mixer.h"
#include "standalone_timeline_renderer.h"
#include "waveform_envelope_core.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <filesystem>
#include <future>
#include <mutex>
#include <set>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace {

namespace fs = std::filesystem;

constexpr auto kAudioSourcePollInterval = std::chrono::milliseconds(250);
constexpr unsigned int kDefaultAudioBufferFrames = 1024;

std::string pathString(const fs::path& path)
{
    return path.lexically_normal().string();
}

std::string lowerAscii(std::string value)
{
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });
    return value;
}

std::string resolvePathForRoot(const std::string& path,
                               const std::string& rootDirectory)
{
    if (path.empty()) {
        return {};
    }
    fs::path resolved(path);
    if (resolved.is_relative() && !rootDirectory.empty()) {
        resolved = fs::path(rootDirectory) / resolved;
    }
    return pathString(resolved);
}

bool regularFile(const fs::path& path)
{
    std::error_code error;
    return fs::is_regular_file(path, error) && !error;
}

std::string fileIdentity(const fs::path& path)
{
    nlohmann::json identity{{"path", pathString(path)}, {"regular", false}};
    std::error_code error;
    if (!fs::is_regular_file(path, error) || error) {
        return identity.dump();
    }
    identity["regular"] = true;
    const std::uintmax_t size = fs::file_size(path, error);
    identity["size"] = error ? 0 : size;
    error.clear();
    const fs::file_time_type modified = fs::last_write_time(path, error);
    identity["modified"] = error
        ? 0
        : static_cast<long long>(modified.time_since_epoch().count());
    return identity.dump();
}

std::string audioTimelineSignature(const jcut::EditorDocumentCore& document,
                                   const std::string& mediaRoot)
{
    nlohmann::json signature{
        {"mediaRoot", mediaRoot},
        {"audioTreatment",
         std::string(editorAudioTreatmentId(document.audioTreatment))},
        {"audioDynamics", {
            {"amplifyEnabled", document.audioDynamics.amplifyEnabled},
            {"amplifyDb", document.audioDynamics.amplifyDb},
            {"normalizeEnabled", document.audioDynamics.normalizeEnabled},
            {"normalizeTargetDb", document.audioDynamics.normalizeTargetDb},
            {"selectiveNormalizeEnabled",
             document.audioDynamics.selectiveNormalizeEnabled},
            {"selectiveNormalizeMinSegmentSeconds",
             document.audioDynamics.selectiveNormalizeMinSegmentSeconds},
            {"selectiveNormalizePeakDb",
             document.audioDynamics.selectiveNormalizePeakDb},
            {"selectiveNormalizePasses",
             document.audioDynamics.selectiveNormalizePasses},
            {"transcriptNormalizeEnabled",
             document.audioDynamics.transcriptNormalizeEnabled},
            {"peakReductionEnabled",
             document.audioDynamics.peakReductionEnabled},
            {"peakThresholdDb",
             document.audioDynamics.peakThresholdDb},
            {"limiterEnabled", document.audioDynamics.limiterEnabled},
            {"limiterThresholdDb",
             document.audioDynamics.limiterThresholdDb},
            {"compressorEnabled",
             document.audioDynamics.compressorEnabled},
            {"compressorThresholdDb",
             document.audioDynamics.compressorThresholdDb},
            {"compressorRatio",
             document.audioDynamics.compressorRatio},
            {"softClipEnabled",
             document.audioDynamics.softClipEnabled},
            {"stereoToMonoEnabled",
             document.audioDynamics.stereoToMonoEnabled}
        }},
        {"tracks", nlohmann::json::array()},
        {"clips", nlohmann::json::array()},
        {"renderSyncMarkers", nlohmann::json::array()}};
    for (const jcut::EditorTrack& track : document.tracks) {
        signature["tracks"].push_back({
            {"id", track.id},
            {"audioEnabled", track.audioEnabled},
            {"audioGain", track.audioGain},
            {"audioMuted", track.audioMuted},
            {"audioSolo", track.audioSolo}});
    }
    for (const jcut::EditorClip& clip : document.clips) {
        signature["clips"].push_back({
            {"id", clip.id},
            {"persistentId", clip.persistentId},
            {"trackId", clip.trackId},
            {"sourcePath", clip.sourcePath},
            {"mediaKind", clip.mediaKind},
            {"audioEnabled", clip.audioEnabled},
            {"audioPresenceKnown", clip.audioPresenceKnown},
            {"hasAudio", clip.hasAudio},
            {"audioSourceMode", clip.audioSourceMode},
            {"audioSourcePath", clip.audioSourcePath},
            {"audioStreamIndex", clip.audioStreamIndex},
            {"audioGain", clip.audioGain},
            {"audioPan", clip.audioPan},
            {"audioSolo", clip.audioSolo},
            {"startFrame", clip.startFrame},
            {"startSubframeSamples", clip.startSubframeSamples},
            {"durationFrames", clip.durationFrames},
            {"durationSubframeSamples", clip.durationSubframeSamples},
            {"sourceDurationFrames", clip.sourceDurationFrames},
            {"sourceInFrame", clip.sourceInFrame},
            {"sourceInSubframeSamples", clip.sourceInSubframeSamples},
            {"sourceFps", clip.sourceFps},
            {"playbackRate", clip.playbackRate},
            {"fadeSamples", clip.fadeSamples}});
    }
    for (const jcut::EditorRenderSyncMarker& marker : document.renderSyncMarkers) {
        signature["renderSyncMarkers"].push_back({
            {"clipId", marker.clipId},
            {"frame", marker.frame},
            {"skipFrame", marker.skipFrame},
            {"count", marker.count}});
    }
    return signature.dump();
}

struct ResolvedAudioTimeline {
    jcut::EditorDocumentCore document;
    std::vector<std::string> scheduledPaths;
    std::string sourceIdentity;
};

struct ProbeCacheEntry {
    std::string identity;
    bool hasAudio = false;
};

bool fallbackAudioPresence(const jcut::EditorClip& clip)
{
    return clip.hasAudio || lowerAscii(clip.mediaKind) == "audio";
}

fs::path selectedAudioPath(const jcut::EditorClip& clip,
                           const std::string& mediaRoot)
{
    const fs::path source(resolvePathForRoot(clip.sourcePath, mediaRoot));
    const fs::path explicitAudio(
        resolvePathForRoot(clip.audioSourcePath, mediaRoot));
    const std::string sourceMode = lowerAscii(clip.audioSourceMode);
    if ((sourceMode == "external" || sourceMode == "explicit_file" ||
         sourceMode == "sidecar") &&
        !explicitAudio.empty() && regularFile(explicitAudio)) {
        return explicitAudio;
    }
    if (!source.empty() && lowerAscii(source.extension().string()) != ".wav") {
        fs::path sidecar = source;
        sidecar.replace_extension(".wav");
        if (regularFile(sidecar)) {
            return sidecar;
        }
    }
    return source;
}

ResolvedAudioTimeline resolveAudioTimeline(
    const jcut::EditorDocumentCore& sourceDocument,
    const std::string& mediaRoot,
    std::unordered_map<std::string, ProbeCacheEntry>* probeCache)
{
    ResolvedAudioTimeline resolved;
    resolved.document = sourceDocument;
    nlohmann::json identities = nlohmann::json::array();
    std::set<std::string> uniquePaths;

    for (jcut::EditorClip& clip : resolved.document.clips) {
        if (!clip.audioEnabled) {
            clip.hasAudio = false;
            continue;
        }
        const fs::path audioPath = selectedAudioPath(clip, mediaRoot);
        const std::string identity = fileIdentity(audioPath);
        identities.push_back(nlohmann::json::parse(identity));
        if (audioPath.empty() || !regularFile(audioPath)) {
            clip.hasAudio = false;
            continue;
        }

        const std::string normalizedPath = pathString(audioPath);
        bool hasAudio = fallbackAudioPresence(clip);
        ProbeCacheEntry& cached = (*probeCache)[normalizedPath];
        if (cached.identity != identity) {
            const jcut::standalone_render::StandaloneMediaInfo probe =
                jcut::standalone_render::probeStandaloneMedia(normalizedPath);
            cached.identity = identity;
            cached.hasAudio = probe.probed ? probe.hasAudio : hasAudio;
        }
        hasAudio = cached.hasAudio;
        clip.audioPresenceKnown = true;
        clip.hasAudio = hasAudio;
        if (!hasAudio) {
            continue;
        }

        // The standalone mixer consumes one canonical path per clip. Resolve
        // explicit and derived sidecars here so decode and status agree.
        clip.sourcePath = normalizedPath;
        clip.audioSourceMode = "embedded";
        clip.audioSourcePath.clear();
        if (uniquePaths.insert(normalizedPath).second) {
            resolved.scheduledPaths.push_back(normalizedPath);
        }
    }
    resolved.sourceIdentity = identities.dump();
    return resolved;
}

double normalizedPlaybackSpeed(double speed)
{
    if (!std::isfinite(speed) || std::abs(speed) < 0.001) {
        return 1.0;
    }
    return std::clamp(std::abs(speed), 0.05, 8.0);
}

} // namespace

namespace jcut {

class ImGuiAudioRuntime::Impl {
public:
    ~Impl()
    {
        shutdown();
    }

    void synchronize(const EditorDocumentCore& document,
                     const std::string& mediaRoot)
    {
        std::lock_guard<std::mutex> operationLock(m_operationMutex);
        try {
            synchronizeLocked(document, mediaRoot);
        } catch (const std::exception& exception) {
            stopOutputLocked();
            m_buffering = false;
            m_outputUnavailable = true;
            m_statusMessage =
                "audio synchronization failed: " + std::string(exception.what());
        }
        publishStatusLocked();
    }

    ImGuiAudioStatus status() const
    {
        std::lock_guard<std::mutex> statusLock(m_statusMutex);
        return m_status;
    }

    bool queryClipWaveform(
        int clipId,
        int columns,
        std::vector<float>* minimumOut,
        std::vector<float>* maximumOut) const
    {
        std::lock_guard<std::mutex> operationLock(
            m_operationMutex);
        if (!minimumOut || !maximumOut ||
            !m_cacheReady) {
            return false;
        }
        const auto clip = std::find_if(
            m_resolvedDocument.clips.begin(),
            m_resolvedDocument.clips.end(),
            [clipId](const EditorClip& candidate) {
                return candidate.id == clipId;
            });
        const auto decoded = m_decodedCache.find(clipId);
        if (clip == m_resolvedDocument.clips.end() ||
            decoded == m_decodedCache.end() ||
            !decoded->second.valid ||
            decoded->second.samples.empty()) {
            return false;
        }
        const std::int64_t timelineStart =
            standalone_render::audio::
                clipTimelineStartSamples(*clip);
        const std::int64_t timelineEnd =
            timelineStart +
            standalone_render::audio::
                clipTimelineDurationSamples(*clip);
        const std::int64_t firstSource =
            standalone_render::audio::
                sourceSampleForClipAtTimelineSample(
                    m_resolvedDocument,
                    *clip,
                    timelineStart);
        const std::int64_t lastSource =
            standalone_render::audio::
                sourceSampleForClipAtTimelineSample(
                    m_resolvedDocument,
                    *clip,
                    timelineEnd - 1);
        const std::int64_t sourceStart =
            std::min(firstSource, lastSource);
        const std::int64_t sourceEnd =
            std::max(firstSource, lastSource) + 1;
        const auto& audio = decoded->second;
        return audio::queryWaveformEnvelope(
            audio::WaveformSampleView{
                audio.samples.data(),
                static_cast<std::int64_t>(
                    audio.samples.size() /
                    standalone_render::audio::
                        kChannelCount),
                standalone_render::audio::kChannelCount,
                audio.sourceStartSample,
                audio.sourceSampleScale},
            sourceStart,
            sourceEnd,
            columns,
            minimumOut,
            maximumOut);
    }

    void setBufferFrames(unsigned int frames)
    {
        std::lock_guard<std::mutex> operationLock(m_operationMutex);
        const unsigned int normalized = std::clamp(frames, 64U, 8192U);
        if (m_requestedBufferFrames == normalized) return;
        stopOutputLocked();
        if (m_audio && m_audio->isStreamOpen()) {
            m_audio->closeStream();
        }
        m_requestedBufferFrames = normalized;
        m_actualBufferFrames = 0;
        m_initialized = false;
        m_statusMessage =
            "audio buffer changed; output will restart on playback";
        publishStatusLocked();
    }

    void refreshOutputDevices()
    {
        std::lock_guard<std::mutex> operationLock(m_operationMutex);
        refreshOutputDevicesLocked();
        publishStatusLocked();
    }

    void setOutputDeviceName(const std::string& name)
    {
        std::lock_guard<std::mutex> operationLock(m_operationMutex);
        if (m_requestedOutputDeviceName == name) return;
        stopOutputLocked();
        if (m_audio && m_audio->isStreamOpen()) {
            m_audio->closeStream();
        }
        m_requestedOutputDeviceName = name;
        m_activeOutputDeviceName.clear();
        m_actualBufferFrames = 0;
        m_initialized = false;
        m_outputUnavailable = false;
        refreshOutputDevicesLocked();
        m_statusMessage = name.empty()
            ? "default audio output selected; stream will restart on playback"
            : "audio output selected; stream will restart on playback";
        publishStatusLocked();
    }

    void shutdown()
    {
        std::lock_guard<std::mutex> operationLock(m_operationMutex);
        stopOutputLocked();
        if (m_decodeFuture.valid()) {
            try {
                m_decodeFuture.wait();
                (void)m_decodeFuture.get();
            } catch (...) {
                // Teardown must continue after a background decode failure.
            }
        }
        if (m_audio && m_audio->isStreamOpen()) {
            m_audio->closeStream();
        }
        m_audio.reset();
        m_playbackData.reset();
        m_initialized = false;
        m_actualBufferFrames = 0;
        m_timelineConfigured = false;
        m_outputUnavailable = false;
        m_buffering = false;
        m_cacheReady = false;
        m_decodeFailed = false;
        m_lastFrame = -1;
        m_lastSpeed = 1.0;
        m_timelineGeneration = 0;
        m_timelineSignature.clear();
        m_sourceIdentity.clear();
        m_scheduledPaths.clear();
        m_probeCache.clear();
        m_nextSourcePoll = {};
        m_statusMessage.clear();
        publishStatusLocked();
    }

private:
    struct PlaybackData {
        EditorDocumentCore document;
        standalone_render::audio::DecodedAudioCache cache;
        std::atomic<std::int64_t> timelineSample{0};
        std::atomic<double> timelineSampleStep{1.0};
        std::atomic<float> volume{0.8f};
        std::atomic<bool> muted{false};
        std::array<
            std::atomic<float>,
            ImGuiAudioStatus::kWaveformPointCount> waveform{};
    };

    struct DecodeResult {
        std::uint64_t generation = 0;
        bool success = false;
        std::string error;
        EditorDocumentCore document;
        standalone_render::audio::DecodedAudioCache cache;
    };

    static int audioCallback(void* outputBuffer,
                             void*,
                             unsigned int frameCount,
                             double,
                             RtAudioStreamStatus,
                             void* userData)
    {
        auto* self = static_cast<Impl*>(userData);
        if (!self || !outputBuffer) {
            return 0;
        }
        self->renderAudio(static_cast<float*>(outputBuffer), frameCount);
        return 0;
    }

    void renderAudio(float* output, unsigned int frameCount)
    {
        PlaybackData* data = m_playbackData.get();
        if (!data) {
            std::fill(output,
                      output + static_cast<std::ptrdiff_t>(
                          frameCount * standalone_render::audio::kChannelCount),
                      0.0f);
            return;
        }
        const std::int64_t start =
            data->timelineSample.load(std::memory_order_relaxed);
        const double step =
            data->timelineSampleStep.load(std::memory_order_relaxed);
        standalone_render::audio::mixAudioChunk(
            data->document,
            data->cache,
            output,
            static_cast<int>(frameCount),
            start,
            step);
        const bool muted = data->muted.load(
            std::memory_order_relaxed);
        const float volume = data->volume.load(
            std::memory_order_relaxed);
        const std::ptrdiff_t sampleCount =
            static_cast<std::ptrdiff_t>(
                frameCount *
                standalone_render::audio::kChannelCount);
        if (muted || volume <= 0.0f) {
            std::fill(output, output + sampleCount, 0.0f);
        } else if (volume < 0.9999f) {
            for (std::ptrdiff_t index = 0;
                 index < sampleCount;
                 ++index) {
                output[index] *= volume;
            }
        }
        for (std::size_t point = 0;
             point < ImGuiAudioStatus::kWaveformPointCount;
             ++point) {
            const unsigned int beginFrame =
                static_cast<unsigned int>(
                    (static_cast<std::uint64_t>(point) * frameCount) /
                    ImGuiAudioStatus::kWaveformPointCount);
            const unsigned int endFrame = std::max(
                beginFrame + 1,
                static_cast<unsigned int>(
                    (static_cast<std::uint64_t>(point + 1) *
                     frameCount) /
                    ImGuiAudioStatus::kWaveformPointCount));
            float peak = 0.0f;
            for (unsigned int frame = beginFrame;
                 frame < std::min(frameCount, endFrame);
                 ++frame) {
                const std::ptrdiff_t offset =
                    static_cast<std::ptrdiff_t>(
                        frame *
                        standalone_render::audio::kChannelCount);
                peak = std::max(
                    peak,
                    std::max(
                        std::abs(output[offset]),
                        std::abs(output[offset + 1])));
            }
            data->waveform[point].store(
                std::clamp(peak, 0.0f, 1.0f),
                std::memory_order_relaxed);
        }
        const std::int64_t consumed = std::max<std::int64_t>(
            1,
            static_cast<std::int64_t>(std::llround(frameCount * step)));
        data->timelineSample.fetch_add(consumed, std::memory_order_relaxed);
    }

    void publishStatusLocked()
    {
        ImGuiAudioStatus status;
        status.initialized = m_initialized;
        status.timelineConfigured = m_timelineConfigured;
        status.buffering = m_buffering;
        status.playbackActive = m_playbackActive;
        status.playbackStarted = m_audio && m_audio->isStreamRunning();
        status.hasPlayableAudio = !m_scheduledPaths.empty();
        status.clockAvailable = status.playbackStarted;
        status.outputUnavailable = status.hasPlayableAudio && m_outputUnavailable;
        status.muted = m_transportMuted;
        status.volume = m_transportVolume;
        if (m_playbackData) {
            for (std::size_t point = 0;
                 point < status.recentWaveform.size();
                 ++point) {
                status.recentWaveform[point] =
                    m_playbackData->waveform[point].load(
                        std::memory_order_relaxed);
            }
        }
        status.requestedBufferFrames = m_requestedBufferFrames;
        status.actualBufferFrames = m_actualBufferFrames;
        status.requestedOutputDeviceName = m_requestedOutputDeviceName;
        status.activeOutputDeviceName = m_activeOutputDeviceName;
        status.outputDevices = m_outputDevices;
        status.scheduledSourcePaths = m_scheduledPaths;
        status.message = m_statusMessage;
        std::lock_guard<std::mutex> statusLock(m_statusMutex);
        m_status = std::move(status);
    }

    void stopOutputLocked()
    {
        if (m_audio && m_audio->isStreamRunning()) {
            (void)m_audio->stopStream();
        }
        m_playbackActive = false;
    }

    void refreshOutputDevicesLocked()
    {
        if (!m_audio) {
            m_audio = std::make_unique<RtAudio>();
            m_audio->showWarnings(false);
        }
        m_outputDevices.clear();
        for (const unsigned int id : m_audio->getDeviceIds()) {
            const RtAudio::DeviceInfo info = m_audio->getDeviceInfo(id);
            if (info.outputChannels <
                standalone_render::audio::kChannelCount) {
                continue;
            }
            m_outputDevices.push_back(ImGuiAudioOutputDevice{
                id,
                info.name,
                info.outputChannels,
                info.isDefaultOutput});
        }
    }

    bool openOutputLocked()
    {
        refreshOutputDevicesLocked();
        if (m_audio->isStreamOpen()) {
            m_initialized = true;
            return true;
        }
        if (m_outputDevices.empty()) {
            m_outputUnavailable = true;
            m_statusMessage =
                "audio output unavailable: no stereo output device";
            return false;
        }
        const ImGuiAudioOutputDevice* selected = nullptr;
        if (!m_requestedOutputDeviceName.empty()) {
            const auto found = std::find_if(
                m_outputDevices.begin(),
                m_outputDevices.end(),
                [this](const ImGuiAudioOutputDevice& device) {
                    return device.name == m_requestedOutputDeviceName;
                });
            if (found != m_outputDevices.end()) {
                selected = &*found;
            }
        }
        if (!selected) {
            const auto defaultDevice = std::find_if(
                m_outputDevices.begin(),
                m_outputDevices.end(),
                [](const ImGuiAudioOutputDevice& device) {
                    return device.isDefault;
                });
            selected = defaultDevice != m_outputDevices.end()
                ? &*defaultDevice
                : &m_outputDevices.front();
        }
        RtAudio::StreamParameters output;
        output.deviceId = selected->id;
        output.nChannels = standalone_render::audio::kChannelCount;
        output.firstChannel = 0;
        unsigned int bufferFrames = m_requestedBufferFrames;
        RtAudio::StreamOptions options;
        options.streamName = "JCut ImGui Preview";
        options.flags = RTAUDIO_MINIMIZE_LATENCY;
        if (m_audio->openStream(
                &output,
                nullptr,
                RTAUDIO_FLOAT32,
                standalone_render::audio::kSampleRate,
                &bufferFrames,
                &Impl::audioCallback,
                this,
                &options) != RTAUDIO_NO_ERROR) {
            m_outputUnavailable = true;
            m_statusMessage = m_audio->getErrorText();
            if (m_statusMessage.empty()) {
                m_statusMessage = "audio output unavailable";
            }
            return false;
        }
        m_initialized = true;
        m_actualBufferFrames = bufferFrames;
        m_activeOutputDeviceName = selected->name;
        m_outputUnavailable = false;
        return true;
    }

    bool startOutputLocked(int currentFrame,
                           double speed,
                           bool muted,
                           float volume)
    {
        if (!m_cacheReady || !openOutputLocked()) {
            return false;
        }
        if (!m_playbackData) {
            m_playbackData = std::make_unique<PlaybackData>();
        }
        m_playbackData->document = m_resolvedDocument;
        m_playbackData->cache = m_decodedCache;
        m_playbackData->timelineSample.store(
            static_cast<std::int64_t>(std::max(0, currentFrame)) *
                standalone_render::audio::kSamplesPerTimelineFrame,
            std::memory_order_relaxed);
        m_playbackData->timelineSampleStep.store(
            normalizedPlaybackSpeed(speed), std::memory_order_relaxed);
        m_playbackData->muted.store(
            muted,
            std::memory_order_relaxed);
        m_playbackData->volume.store(
            std::clamp(
                volume,
                0.0f,
                1.0f),
            std::memory_order_relaxed);
        if (m_audio->startStream() != RTAUDIO_NO_ERROR) {
            m_outputUnavailable = true;
            m_statusMessage = m_audio->getErrorText();
            if (m_statusMessage.empty()) {
                m_statusMessage = "audio output unavailable";
            }
            return false;
        }
        m_playbackActive = m_audio->isStreamRunning();
        m_outputUnavailable = !m_playbackActive;
        return m_playbackActive;
    }

    bool refreshTimelineLocked(const EditorDocumentCore& document,
                               const std::string& mediaRoot)
    {
        const std::string documentSignature =
            audioTimelineSignature(document, mediaRoot);
        const auto now = std::chrono::steady_clock::now();
        if (m_timelineConfigured && documentSignature == m_timelineSignature &&
            now < m_nextSourcePoll) {
            return false;
        }

        ResolvedAudioTimeline resolved = resolveAudioTimeline(
            document, mediaRoot, &m_probeCache);
        m_nextSourcePoll = now + kAudioSourcePollInterval;
        const bool changed = !m_timelineConfigured ||
            documentSignature != m_timelineSignature ||
            resolved.sourceIdentity != m_sourceIdentity ||
            resolved.scheduledPaths != m_scheduledPaths;
        if (!changed) {
            return false;
        }

        stopOutputLocked();
        m_resolvedDocument = std::move(resolved.document);
        m_scheduledPaths = std::move(resolved.scheduledPaths);
        m_timelineSignature = documentSignature;
        m_sourceIdentity = std::move(resolved.sourceIdentity);
        m_timelineConfigured = true;
        m_cacheReady = false;
        m_decodeFailed = false;
        m_decodedCache.clear();
        ++m_timelineGeneration;
        return true;
    }

    void collectDecodeResultLocked()
    {
        if (!m_decodeFuture.valid() ||
            m_decodeFuture.wait_for(std::chrono::milliseconds(0)) !=
                std::future_status::ready) {
            return;
        }
        DecodeResult decoded = m_decodeFuture.get();
        if (decoded.generation != m_timelineGeneration) {
            return;
        }
        m_buffering = false;
        if (!decoded.success) {
            m_cacheReady = false;
            m_decodeFailed = true;
            m_outputUnavailable = true;
            m_statusMessage = decoded.error.empty()
                ? "audio source decode failed"
                : "audio source decode failed: " + decoded.error;
            return;
        }
        m_resolvedDocument = std::move(decoded.document);
        m_decodedCache = std::move(decoded.cache);
        m_cacheReady = true;
        m_decodeFailed = false;
        m_outputUnavailable = false;
    }

    void launchDecodeLocked()
    {
        if (m_decodeFuture.valid() || m_decodeFailed ||
            m_scheduledPaths.empty()) {
            return;
        }
        const std::uint64_t generation = m_timelineGeneration;
        const EditorDocumentCore document = m_resolvedDocument;
        m_decodeFuture = std::async(
            std::launch::async,
            [generation, document]() mutable {
                DecodeResult result;
                result.generation = generation;
                result.document = std::move(document);
                result.success = standalone_render::audio::decodeDocumentAudio(
                    result.document, {}, &result.cache, &result.error);
                return result;
            });
        m_buffering = true;
        m_statusMessage = "buffering audio";
    }

    void synchronizeLocked(const EditorDocumentCore& document,
                           const std::string& mediaRoot)
    {
        m_transportMuted = document.transport.audioMuted;
        m_transportVolume = std::clamp(
            document.transport.audioVolume, 0.0f, 1.0f);
        refreshTimelineLocked(document, mediaRoot);
        collectDecodeResultLocked();
        const int currentFrame = document.transport.currentFrame;
        const double speed = normalizedPlaybackSpeed(
            document.transport.playbackSpeed);

        if (!document.transport.playbackActive) {
            stopOutputLocked();
            if (document.panels.showWaveform &&
                !m_cacheReady &&
                !m_scheduledPaths.empty()) {
                launchDecodeLocked();
            }
            m_buffering =
                m_decodeFuture.valid() &&
                !m_cacheReady;
            m_outputUnavailable = false;
            m_lastFrame = currentFrame;
            m_lastSpeed = speed;
            m_statusMessage = m_scheduledPaths.empty()
                ? "no playable audio on timeline"
                : "audio timeline configured";
            return;
        }

        if (m_scheduledPaths.empty()) {
            stopOutputLocked();
            m_buffering = false;
            m_lastFrame = currentFrame;
            m_statusMessage = "no playable audio on timeline";
            return;
        }

        if (!m_cacheReady) {
            launchDecodeLocked();
            if (m_decodeFuture.valid()) {
                m_buffering = true;
                m_statusMessage = "buffering audio";
            } else if (m_decodeFailed) {
                // Keep the transport fail-open after a terminal decode error,
                // but do not spin up a full-file decode on every UI frame.
                m_buffering = false;
                m_outputUnavailable = true;
            }
            m_lastFrame = currentFrame;
            m_lastSpeed = speed;
            return;
        }

        if (!m_playbackActive) {
            if (!startOutputLocked(
                    currentFrame,
                    speed,
                    document.transport.audioMuted,
                    document.transport.audioVolume)) {
                m_buffering = false;
                m_lastFrame = currentFrame;
                m_lastSpeed = speed;
                return;
            }
        } else if (m_playbackData) {
            m_playbackData->muted.store(
                document.transport.audioMuted,
                std::memory_order_relaxed);
            m_playbackData->volume.store(
                std::clamp(
                    document.transport.audioVolume,
                    0.0f,
                    1.0f),
                std::memory_order_relaxed);
            const bool speedChanged = std::abs(speed - m_lastSpeed) >= 0.0001;
            const bool frameJumped = m_lastFrame >= 0 &&
                std::abs(currentFrame - m_lastFrame) > 8;
            if (speedChanged) {
                m_playbackData->timelineSampleStep.store(
                    speed, std::memory_order_relaxed);
            }
            if (frameJumped) {
                m_playbackData->timelineSample.store(
                    static_cast<std::int64_t>(std::max(0, currentFrame)) *
                        standalone_render::audio::kSamplesPerTimelineFrame,
                    std::memory_order_relaxed);
            }
        }

        m_buffering = false;
        m_lastFrame = currentFrame;
        m_lastSpeed = speed;
        m_statusMessage = "audio playback active";
    }

    mutable std::mutex m_operationMutex;
    mutable std::mutex m_statusMutex;
    ImGuiAudioStatus m_status;
    std::unique_ptr<RtAudio> m_audio;
    std::unique_ptr<PlaybackData> m_playbackData;
    std::future<DecodeResult> m_decodeFuture;
    EditorDocumentCore m_resolvedDocument;
    standalone_render::audio::DecodedAudioCache m_decodedCache;
    std::unordered_map<std::string, ProbeCacheEntry> m_probeCache;
    std::vector<std::string> m_scheduledPaths;
    std::vector<ImGuiAudioOutputDevice> m_outputDevices;
    bool m_initialized = false;
    bool m_timelineConfigured = false;
    bool m_playbackActive = false;
    bool m_outputUnavailable = false;
    bool m_buffering = false;
    bool m_cacheReady = false;
    bool m_decodeFailed = false;
    bool m_transportMuted = false;
    float m_transportVolume = 0.8f;
    unsigned int m_requestedBufferFrames = kDefaultAudioBufferFrames;
    unsigned int m_actualBufferFrames = 0;
    std::string m_requestedOutputDeviceName;
    std::string m_activeOutputDeviceName;
    int m_lastFrame = -1;
    double m_lastSpeed = 1.0;
    std::uint64_t m_timelineGeneration = 0;
    std::chrono::steady_clock::time_point m_nextSourcePoll{};
    std::string m_timelineSignature;
    std::string m_sourceIdentity;
    std::string m_statusMessage;
};

ImGuiAudioRuntime::ImGuiAudioRuntime()
    : m_impl(std::make_unique<Impl>())
{
}

ImGuiAudioRuntime::~ImGuiAudioRuntime() = default;

void ImGuiAudioRuntime::synchronize(const EditorDocumentCore& document,
                                    const std::string& mediaRoot)
{
    m_impl->synchronize(document, mediaRoot);
}

ImGuiAudioStatus ImGuiAudioRuntime::status() const
{
    return m_impl->status();
}

bool ImGuiAudioRuntime::queryClipWaveform(
    int clipId,
    int columns,
    std::vector<float>* minimumOut,
    std::vector<float>* maximumOut) const
{
    return m_impl->queryClipWaveform(
        clipId, columns, minimumOut, maximumOut);
}

void ImGuiAudioRuntime::setBufferFrames(unsigned int frames)
{
    m_impl->setBufferFrames(frames);
}

void ImGuiAudioRuntime::refreshOutputDevices()
{
    m_impl->refreshOutputDevices();
}

void ImGuiAudioRuntime::setOutputDeviceName(const std::string& name)
{
    m_impl->setOutputDeviceName(name);
}

void ImGuiAudioRuntime::shutdown()
{
    m_impl->shutdown();
}

} // namespace jcut
