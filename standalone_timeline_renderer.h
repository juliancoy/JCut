#pragma once

#include "core/image_buffer.h"
#include "editor_document_core.h"

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
