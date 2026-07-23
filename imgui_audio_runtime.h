#pragma once

#include "editor_document_core.h"

#include <memory>
#include <string>
#include <vector>

namespace jcut {

struct ImGuiAudioStatus {
    bool initialized = false;
    bool timelineConfigured = false;
    bool buffering = false;
    bool playbackActive = false;
    bool playbackStarted = false;
    bool hasPlayableAudio = false;
    bool clockAvailable = false;
    bool outputUnavailable = false;
    unsigned int requestedBufferFrames = 1024;
    unsigned int actualBufferFrames = 0;
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
    void shutdown();

private:
    class Impl;
    std::unique_ptr<Impl> m_impl;
};

} // namespace jcut
