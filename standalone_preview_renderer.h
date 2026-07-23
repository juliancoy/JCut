#pragma once

#include "core/image_buffer.h"
#include "core/offscreen_vulkan_frame.h"
#include "editor_document_core.h"

#include <string>

namespace jcut::standalone_render {

struct PreviewRenderRequest {
    EditorDocumentCore document;
    core::SizeI outputSize;
    int timelineFrame = 0;
    std::string rootDirectory;
    bool preferVulkanFrame = true;
    bool allowCpuFallback = true;
};

struct PreviewRenderResult {
    bool success = false;
    std::string message;
    core::ImageBuffer image;
    render_detail::OffscreenVulkanFrame vulkanFrame;
    std::string sourcePath;
};

PreviewRenderResult renderPreviewFrame(const PreviewRenderRequest& request);

} // namespace jcut::standalone_render
