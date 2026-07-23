#pragma once

#include "core/image_buffer.h"
#include "editor_document_core.h"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>

namespace jcut::standalone_render {

struct TimelineRenderRequest {
    EditorDocumentCore document;
    core::SizeI outputSize;
    double timelineFrame = 0.0;
    std::string rootDirectory;
};

struct TimelineRenderResult {
    bool success = false;
    std::string message;
    core::ImageBuffer image;
    std::string sourcePath;
};

struct StandaloneMediaInfo {
    bool probed = false;
    bool hasVideo = false;
    bool hasAudio = false;
    int audioStreamIndex = -1;
    double videoFps = 0.0;
    std::int64_t durationFrames = 0;
    core::SizeI frameSize;
    std::string mediaKind = "unknown";
    std::string message;
};

// Qt-free stream metadata probe used by the ImGui import and document-load
// paths. It shares the standalone renderer's FFmpeg boundary so
// command/document code can remain framework neutral.
StandaloneMediaInfo probeStandaloneMedia(const std::string& path);

// Resolves legacy/unknown stream-presence metadata once when a document is
// loaded. Missing or unprobeable sources remain unknown and can be retried on
// a later load. Returns the number of clips whose metadata was resolved.
std::size_t probeUnknownAudioPresence(EditorDocumentCore* document,
                                      const std::string& rootDirectory);

class TimelineRenderer {
public:
    TimelineRenderer();
    ~TimelineRenderer();

    TimelineRenderer(const TimelineRenderer&) = delete;
    TimelineRenderer& operator=(const TimelineRenderer&) = delete;

    TimelineRenderResult renderFrame(const TimelineRenderRequest& request);

private:
    class Impl;
    std::unique_ptr<Impl> m_impl;
};

TimelineRenderResult renderTimelineFrame(const TimelineRenderRequest& request);

} // namespace jcut::standalone_render
