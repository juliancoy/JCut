#pragma once

#include "editor_document_core.h"

#include <array>
#include <memory>
#include <string>
#include <vector>

namespace jcut {

struct ImGuiAudioOutputDevice {
    unsigned int id = 0;
    std::string name;
    unsigned int outputChannels = 0;
    bool isDefault = false;
};

struct ImGuiAudioStatus {
    static constexpr std::size_t kWaveformPointCount = 128;
    bool initialized = false;
    bool timelineConfigured = false;
    bool buffering = false;
    bool playbackActive = false;
    bool playbackStarted = false;
    bool hasPlayableAudio = false;
    bool clockAvailable = false;
    bool outputUnavailable = false;
    bool muted = false;
    float volume = 0.8f;
    std::array<float, kWaveformPointCount> recentWaveform{};
    unsigned int requestedBufferFrames = 1024;
    unsigned int actualBufferFrames = 0;
    std::string requestedOutputDeviceName;
    std::string activeOutputDeviceName;
    std::vector<ImGuiAudioOutputDevice> outputDevices;
    std::vector<std::string> scheduledSourcePaths;
    std::string message;
};

// Qt-free public boundary for ImGui timeline-audio playback. The implementation
// is hidden so RtAudio, FFmpeg decode state, and callback synchronization do not
// leak into shell code or the stable document-facing API.
class ImGuiAudioRuntime {
public:
    ImGuiAudioRuntime();
    ~ImGuiAudioRuntime();

    ImGuiAudioRuntime(const ImGuiAudioRuntime&) = delete;
    ImGuiAudioRuntime& operator=(const ImGuiAudioRuntime&) = delete;
    ImGuiAudioRuntime(ImGuiAudioRuntime&&) = delete;
    ImGuiAudioRuntime& operator=(ImGuiAudioRuntime&&) = delete;

    void synchronize(const EditorDocumentCore& document,
                     const std::string& mediaRoot);
    ImGuiAudioStatus status() const;
    void setBufferFrames(unsigned int frames);
    void refreshOutputDevices();
    void setOutputDeviceName(const std::string& name);
    [[nodiscard]] bool queryClipWaveform(
        int clipId,
        int columns,
        std::vector<float>* minimumOut,
        std::vector<float>* maximumOut) const;
    void shutdown();

private:
    class Impl;
    std::unique_ptr<Impl> m_impl;
};

} // namespace jcut
