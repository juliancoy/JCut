#include "imgui_audio_runtime.h"

#include "audio_engine.h"
#include "editor_document_render_bridge.h"
#include "editor_shared_media.h"
#include "editor_shared_render_sync.h"

#include <nlohmann/json.hpp>

#include <QVector>

#include <algorithm>
#include <chrono>
#include <cctype>
#include <cmath>
#include <cstdio>
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
constexpr int kAudioWarmTimeoutMs = 1000;

std::string pathString(const fs::path& path)
{
    return path.lexically_normal().string();
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

jcut::EditorDocumentCore documentWithResolvedMediaPaths(
    jcut::EditorDocumentCore document,
    const std::string& rootDirectory)
{
    for (jcut::EditorMediaItem& mediaItem : document.mediaItems) {
        mediaItem.id = resolvePathForRoot(mediaItem.id, rootDirectory);
    }
    for (jcut::EditorClip& clip : document.clips) {
        clip.sourcePath = resolvePathForRoot(clip.sourcePath, rootDirectory);
        clip.audioSourcePath =
            resolvePathForRoot(clip.audioSourcePath, rootDirectory);
    }
    return document;
}

std::string audioTimelineSignature(const jcut::EditorDocumentCore& document,
                                   const std::string& mediaRoot)
{
    nlohmann::json signature{
        {"mediaRoot", mediaRoot},
        {"tracks", nlohmann::json::array()},
        {"clips", nlohmann::json::array()},
        {"exportRanges", nlohmann::json::array()},
        {"renderSyncMarkers", nlohmann::json::array()}};
    for (const jcut::EditorTrack& track : document.tracks) {
        signature["tracks"].push_back({
            {"id", track.id},
            {"audioEnabled", track.audioEnabled},
            {"audioBusId", track.audioBusId},
            {"audioGain", track.audioGain},
            {"audioMuted", track.audioMuted},
            {"audioSolo", track.audioSolo}});
    }
    for (const jcut::EditorClip& clip : document.clips) {
        const bool mayContainAudio = clip.hasAudio ||
            jcut::mediaKindMayContainAudio(clip.mediaKind, clip.sourcePath);
        if (!mayContainAudio) {
            continue;
        }
        nlohmann::json clipSignature{
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
            {"audioSourceStatus", clip.audioSourceStatus},
            {"audioStreamIndex", clip.audioStreamIndex},
            {"audioBusId", clip.audioBusId},
            {"audioGain", clip.audioGain},
            {"audioPan", clip.audioPan},
            {"audioSolo", clip.audioSolo},
            {"audioLinkedToVideo", clip.audioLinkedToVideo}};
        if (clip.audioEnabled && clip.hasAudio) {
            clipSignature.update({
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
        signature["clips"].push_back(std::move(clipSignature));
    }
    signature["exportStartFrame"] = document.exportRequest.exportStartFrame;
    signature["exportEndFrame"] = document.exportRequest.exportEndFrame;
    for (const jcut::EditorExportRange& range : document.exportRanges) {
        signature["exportRanges"].push_back({
            {"startFrame", range.startFrame},
            {"endFrame", range.endFrame}});
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

std::string lowercaseExtension(const fs::path& path)
{
    std::string extension = path.extension().string();
    std::transform(extension.begin(), extension.end(), extension.begin(),
                   [](unsigned char value) {
                       return static_cast<char>(std::tolower(value));
                   });
    return extension;
}

std::string audioSourceIdentitySignature(
    const jcut::EditorDocumentCore& resolvedDocument)
{
    std::set<std::string> candidates;
    for (const jcut::EditorClip& clip : resolvedDocument.clips) {
        if (!clip.audioEnabled ||
            (!clip.hasAudio &&
             !jcut::mediaKindMayContainAudio(clip.mediaKind, clip.sourcePath))) {
            continue;
        }
        if (!clip.sourcePath.empty()) {
            const fs::path sourcePath(clip.sourcePath);
            candidates.insert(pathString(sourcePath));
            if (lowercaseExtension(sourcePath) != ".wav") {
                fs::path sidecarPath = sourcePath;
                sidecarPath.replace_extension(".wav");
                candidates.insert(pathString(sidecarPath));
            }
        }
        if (!clip.audioSourcePath.empty()) {
            candidates.insert(pathString(fs::path(clip.audioSourcePath)));
        }
    }

    nlohmann::json identity = nlohmann::json::array();
    for (const std::string& candidate : candidates) {
        std::error_code statusError;
        const fs::file_status status = fs::status(candidate, statusError);
        const bool regular = !statusError && fs::is_regular_file(status);
        std::uintmax_t size = 0;
        long long modified = 0;
        if (regular) {
            std::error_code sizeError;
            size = fs::file_size(candidate, sizeError);
            if (sizeError) {
                size = 0;
            }
            std::error_code timeError;
            const fs::file_time_type writeTime =
                fs::last_write_time(candidate, timeError);
            if (!timeError) {
                modified = static_cast<long long>(
                    writeTime.time_since_epoch().count());
            }
        }
        identity.push_back({
            {"path", candidate},
            {"regular", regular},
            {"size", size},
            {"modified", modified}});
    }
    return identity.dump();
}

std::string audioPresenceOverrideKey(const jcut::EditorClip& clip)
{
    return clip.sourcePath + "\n" + clip.audioSourceMode + "\n" +
        clip.audioSourcePath;
}

void applyAudioPresenceOverrides(
    jcut::EditorDocumentCore* document,
    const std::unordered_map<std::string, bool>& overrides)
{
    if (!document) {
        return;
    }
    for (jcut::EditorClip& clip : document->clips) {
        const auto overrideIt = overrides.find(audioPresenceOverrideKey(clip));
        if (overrideIt != overrides.end()) {
            clip.audioPresenceKnown = true;
            clip.hasAudio = overrideIt->second;
        }
    }
}

void refreshAudioPresenceOverrides(
    jcut::EditorDocumentCore* document,
    bool refreshKnown,
    std::unordered_map<std::string, bool>* overrides)
{
    if (!document || !overrides) {
        return;
    }
    for (jcut::EditorClip& clip : document->clips) {
        if (!clip.audioEnabled ||
            (!clip.hasAudio &&
             !jcut::mediaKindMayContainAudio(clip.mediaKind, clip.sourcePath)) ||
            (!refreshKnown && clip.audioPresenceKnown)) {
            continue;
        }

        fs::path probePath(clip.sourcePath);
        const fs::path trackedAudioPath(clip.audioSourcePath);
        std::error_code pathError;
        if (!clip.audioSourcePath.empty() &&
            (clip.audioSourceMode == "explicit_file" ||
             clip.audioSourceMode == "sidecar") &&
            fs::is_regular_file(trackedAudioPath, pathError) && !pathError) {
            probePath = trackedAudioPath;
        } else if (!probePath.empty() && lowercaseExtension(probePath) != ".wav") {
            fs::path sidecarPath = probePath;
            sidecarPath.replace_extension(".wav");
            pathError.clear();
            if (fs::is_regular_file(sidecarPath, pathError) && !pathError) {
                probePath = std::move(sidecarPath);
            }
        }
        if (probePath.empty()) {
            continue;
        }

        const MediaProbeResult probe = probeMediaFile(
            QString::fromStdString(pathString(probePath)),
            std::max(1, clip.durationFrames) / 30.0);
        const bool authoritative = probe.hasVideo || probe.hasAudio ||
            probe.mediaType != ClipMediaType::Unknown;
        if (!authoritative) {
            continue;
        }
        clip.audioPresenceKnown = true;
        clip.hasAudio = probe.hasAudio;
        (*overrides)[audioPresenceOverrideKey(clip)] = probe.hasAudio;
    }
}

template <typename T>
QVector<T> toQVector(const std::vector<T>& values)
{
    QVector<T> result;
    result.reserve(static_cast<qsizetype>(values.size()));
    for (const T& value : values) {
        result.push_back(value);
    }
    return result;
}

} // namespace

namespace jcut {

class ImGuiAudioRuntime::Impl {
public:
    void synchronize(const EditorDocumentCore& document,
                     const std::string& mediaRoot)
    {
        std::lock_guard<std::mutex> operationLock(m_operationMutex);
        try {
            synchronizeLocked(document, mediaRoot);
        } catch (const std::exception& exception) {
            if (m_playbackActive) {
                m_audioEngine.stop();
            }
            m_playbackActive = false;
            m_buffering = false;
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

    void shutdown()
    {
        std::lock_guard<std::mutex> operationLock(m_operationMutex);
        if (m_warmFuture.valid()) {
            try {
                m_warmFuture.wait();
                (void)m_warmFuture.get();
            } catch (const std::exception&) {
                // Shutdown still owns the backend and must continue even if a
                // background warm operation reported an error.
            }
        }
        if (m_playbackActive) {
            m_audioEngine.stop();
        }
        m_audioEngine.shutdown();
        m_initialized = false;
        m_timelineConfigured = false;
        m_playbackActive = false;
        m_outputUnavailable = false;
        m_lastFrame = -1;
        m_lastSpeed = 1.0;
        m_buffering = false;
        m_timelineGeneration = 0;
        m_timelineSignature.clear();
        m_sourceIdentitySignature.clear();
        m_audioPresenceOverrides.clear();
        m_nextSourcePoll = {};
        m_statusMessage.clear();
        publishStatusLocked();
    }

private:
    void publishStatusLocked()
    {
        ImGuiAudioStatus status;
        status.initialized = m_initialized;
        status.timelineConfigured = m_timelineConfigured;
        status.buffering = m_buffering;
        status.playbackActive = m_playbackActive;
        status.playbackStarted =
            m_initialized ? m_audioEngine.playbackStarted() : false;
        status.hasPlayableAudio =
            m_timelineConfigured ? m_audioEngine.hasPlayableAudio() : false;
        status.clockAvailable =
            m_initialized ? m_audioEngine.audioClockAvailable() : false;
        status.outputUnavailable = status.hasPlayableAudio &&
            (m_outputUnavailable ||
             (m_initialized && m_audioEngine.audioOutputUnavailableForPlayback()));
        if (m_timelineConfigured) {
            for (const QString& path : m_audioEngine.scheduledAudioSourcePaths()) {
                status.scheduledSourcePaths.push_back(path.toStdString());
            }
        }
        status.message = m_statusMessage;
        std::lock_guard<std::mutex> statusLock(m_statusMutex);
        m_status = std::move(status);
    }

    bool ensureInitializedLocked()
    {
        if (m_initialized) {
            return true;
        }
        m_initialized = m_audioEngine.initialize();
        m_outputUnavailable = !m_initialized;
        if (!m_initialized) {
            m_statusMessage =
                m_audioEngine.audioOutputStatusText().toStdString();
            if (m_statusMessage.empty()) {
                m_statusMessage = "audio output unavailable";
            }
        }
        return m_initialized;
    }

    bool configureTimelineLocked(const EditorDocumentCore& document,
                                 const std::string& mediaRoot,
                                 int currentFrame)
    {
        EditorDocumentCore renderDocument =
            documentWithResolvedMediaPaths(document, mediaRoot);
        applyAudioPresenceOverrides(&renderDocument, m_audioPresenceOverrides);
        std::string signature =
            audioTimelineSignature(renderDocument, mediaRoot);
        bool documentChanged =
            !m_timelineConfigured || m_timelineSignature != signature;
        const auto now = std::chrono::steady_clock::now();
        if (!documentChanged && now < m_nextSourcePoll) {
            return false;
        }

        const std::string sourceIdentity =
            audioSourceIdentitySignature(renderDocument);
        const bool sourceIdentityChanged = !m_timelineConfigured ||
            sourceIdentity != m_sourceIdentitySignature;
        m_nextSourcePoll = now + kAudioSourcePollInterval;
        if (!documentChanged && !sourceIdentityChanged) {
            return false;
        }

        if (sourceIdentityChanged) {
            // Initial loads only fill legacy unknowns. Once a timeline is
            // installed, an identity change can also mean that the media at
            // a stable path was replaced, so refresh known stream topology.
            refreshAudioPresenceOverrides(
                &renderDocument,
                m_timelineConfigured,
                &m_audioPresenceOverrides);
            applyAudioPresenceOverrides(
                &renderDocument, m_audioPresenceOverrides);
            signature = audioTimelineSignature(renderDocument, mediaRoot);
            documentChanged = !m_timelineConfigured ||
                m_timelineSignature != signature;
        }

        if (m_playbackActive) {
            m_audioEngine.stop();
            m_playbackActive = false;
        }
        if (m_timelineConfigured && sourceIdentityChanged) {
            m_audioEngine.invalidateAudioSourceCaches();
        }

        const render::TimelineRenderData timelineData =
            render::buildTimelineRenderData(renderDocument, false);
        m_audioEngine.setTimelineStateAtFrame(
            toQVector(timelineData.tracks),
            toQVector(timelineData.clips),
            toQVector(timelineData.exportRanges),
            toQVector(timelineData.renderSyncMarkers),
            currentFrame);
        m_timelineConfigured = true;
        m_timelineSignature = signature;
        m_sourceIdentitySignature = sourceIdentity;
        ++m_timelineGeneration;
        return true;
    }

    void synchronizeLocked(const EditorDocumentCore& document,
                           const std::string& mediaRoot)
    {
        const int currentFrame = document.transport.currentFrame;
        configureTimelineLocked(document, mediaRoot, currentFrame);
        const bool hasPlayableAudio = m_audioEngine.hasPlayableAudio();

        if (!document.transport.playbackActive) {
            if (m_playbackActive) {
                m_audioEngine.stop();
            }
            m_playbackActive = false;
            m_buffering = false;
            m_lastFrame = currentFrame;
            if (!hasPlayableAudio) {
                m_statusMessage = "no playable audio on timeline";
            } else if (!m_outputUnavailable) {
                m_statusMessage = m_initialized
                    ? m_audioEngine.audioOutputStatusText().toStdString()
                    : std::string("audio timeline configured");
                if (m_statusMessage.empty()) {
                    m_statusMessage = "audio timeline configured";
                }
            }
            return;
        }

        if (!hasPlayableAudio) {
            if (m_playbackActive) {
                m_audioEngine.stop();
            }
            m_playbackActive = false;
            m_buffering = false;
            m_lastFrame = currentFrame;
            m_statusMessage = "no playable audio on timeline";
            return;
        }

        if (!ensureInitializedLocked()) {
            m_buffering = false;
            return;
        }

        const double speed = document.transport.playbackSpeed == 0.0
            ? 1.0
            : document.transport.playbackSpeed;
        const PlaybackAudioWarpMode requestedWarpMode =
            std::abs(speed - 1.0) < 0.0001
                ? PlaybackAudioWarpMode::Disabled
                : PlaybackAudioWarpMode::Varispeed;
        m_audioEngine.setPlaybackWarpMode(
            normalizedPlaybackAudioWarpMode(speed, requestedWarpMode));
        m_audioEngine.setPlaybackRate(
            effectivePlaybackAudioWarpRate(speed, requestedWarpMode));

        const bool speedChanged =
            std::abs(speed - m_lastSpeed) >= 0.0001;
        const bool frameJumped =
            m_lastFrame >= 0 && std::abs(currentFrame - m_lastFrame) > 8;

        if (!m_playbackActive || !m_audioEngine.playbackStarted()) {
            const bool requestMatchesFuture = m_warmFuture.valid() &&
                m_warmTimelineGeneration == m_timelineGeneration &&
                m_warmFrame == currentFrame &&
                std::abs(m_warmSpeed - speed) < 0.0001;
            if (m_warmFuture.valid()) {
                if (m_warmFuture.wait_for(std::chrono::milliseconds(0)) !=
                    std::future_status::ready) {
                    m_buffering = true;
                    m_lastFrame = currentFrame;
                    m_lastSpeed = speed;
                    m_statusMessage = "buffering audio";
                    return;
                }

                const bool warmed = m_warmFuture.get();
                if (requestMatchesFuture) {
                    if (!warmed &&
                        m_audioEngine.audioOutputUnavailableForPlayback()) {
                        m_outputUnavailable = true;
                        m_buffering = false;
                        m_lastFrame = currentFrame;
                        m_lastSpeed = speed;
                        m_statusMessage =
                            m_audioEngine.audioOutputStatusText().toStdString();
                        if (m_statusMessage.empty()) {
                            m_statusMessage = "audio output unavailable";
                        }
                        return;
                    }
                    const bool terminalSourceFailure = !warmed &&
                        m_audioEngine.playbackAudioWarmupPermanentlyFailed(
                            currentFrame);
                    if (!warmed && !terminalSourceFailure) {
                        std::fprintf(
                            stderr,
                            "[AUDIO] decode still pending after warm interval at frame %d\n",
                            currentFrame);
                    } else {
                        if (terminalSourceFailure) {
                            std::fprintf(
                                stderr,
                                "[AUDIO WARN] source decode failed at frame %d; continuing timeline playback\n",
                                currentFrame);
                        }
                        m_buffering = false;
                        m_outputUnavailable = false;
                        m_audioEngine.start(currentFrame);
                        m_playbackActive = m_audioEngine.playbackStarted();
                    }
                }
            }

            if (!m_playbackActive) {
                m_warmTimelineGeneration = m_timelineGeneration;
                m_warmFrame = currentFrame;
                m_warmSpeed = speed;
                m_warmFuture = std::async(
                    std::launch::async,
                    [this, currentFrame]() {
                        return m_audioEngine.warmPlaybackAudio(
                            currentFrame, kAudioWarmTimeoutMs);
                    });
                m_buffering = true;
                m_lastFrame = currentFrame;
                m_lastSpeed = speed;
                m_statusMessage = "buffering audio";
                return;
            }
        } else if (frameJumped || speedChanged) {
            m_audioEngine.seek(currentFrame);
        }

        m_buffering = false;
        m_lastFrame = currentFrame;
        m_lastSpeed = speed;
        m_statusMessage =
            m_audioEngine.audioOutputStatusText().toStdString();
        if (m_statusMessage.empty()) {
            m_statusMessage = "audio playback active";
        }
    }

    mutable std::mutex m_operationMutex;
    mutable std::mutex m_statusMutex;
    ImGuiAudioStatus m_status;
    AudioEngine m_audioEngine;
    bool m_initialized = false;
    bool m_timelineConfigured = false;
    bool m_playbackActive = false;
    bool m_outputUnavailable = false;
    int m_lastFrame = -1;
    double m_lastSpeed = 1.0;
    bool m_buffering = false;
    std::future<bool> m_warmFuture;
    std::uint64_t m_warmTimelineGeneration = 0;
    int m_warmFrame = -1;
    double m_warmSpeed = 1.0;
    std::uint64_t m_timelineGeneration = 0;
    std::chrono::steady_clock::time_point m_nextSourcePoll{};
    std::unordered_map<std::string, bool> m_audioPresenceOverrides;
    std::string m_timelineSignature;
    std::string m_sourceIdentitySignature;
    std::string m_statusMessage;
};

ImGuiAudioRuntime::ImGuiAudioRuntime()
    : m_impl(std::make_unique<Impl>())
{
}

ImGuiAudioRuntime::~ImGuiAudioRuntime()
{
    shutdown();
}

void ImGuiAudioRuntime::synchronize(const EditorDocumentCore& document,
                                    const std::string& mediaRoot)
{
    m_impl->synchronize(document, mediaRoot);
}

ImGuiAudioStatus ImGuiAudioRuntime::status() const
{
    return m_impl->status();
}

void ImGuiAudioRuntime::shutdown()
{
    m_impl->shutdown();
}

} // namespace jcut
