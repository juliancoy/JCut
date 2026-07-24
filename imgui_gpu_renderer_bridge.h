#pragma once

#include "editor_document_core.h"
#include "imgui_gpu_renderer_plugin_api.h"

#include <functional>
#include <memory>
#include <string>

namespace jcut::imgui_gpu {

class RendererBridge final {
public:
    RendererBridge();
    ~RendererBridge();

    RendererBridge(const RendererBridge&) = delete;
    RendererBridge& operator=(const RendererBridge&) = delete;

    bool initialize(const std::string& executablePath,
                    std::string* errorOut = nullptr);
    void shutdown();

    bool available() const;
    const std::string& status() const;

    bool renderPreview(
        const EditorDocumentCore& document,
        const std::string& rootDirectory,
        core::SizeI outputSize,
        std::int64_t timelineFrame,
        bool readbackToCpuImage,
        render_detail::OffscreenVulkanFrame* frameOut,
        core::ImageBuffer* imageOut = nullptr,
        std::string* errorOut = nullptr);

    render::RenderResultCore exportTimeline(
        const EditorDocumentCore& document,
        const std::string& rootDirectory,
        const std::function<bool(
            const render::RenderProgressCore&)>& progressCallback = {});

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace jcut::imgui_gpu
