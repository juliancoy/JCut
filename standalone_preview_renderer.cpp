#include "standalone_preview_renderer.h"
#include "editor_document_render_bridge.h"
#include "render_runtime.h"
#include "standalone_timeline_renderer.h"

#include <filesystem>

namespace {

std::string resolvePathForRoot(const std::string& path, const std::string& rootDirectory)
{
    if (path.empty()) {
        return {};
    }

    std::filesystem::path resolved(path);
    if (resolved.is_relative() && !rootDirectory.empty()) {
        resolved = std::filesystem::path(rootDirectory) / resolved;
    }
    return resolved.lexically_normal().string();
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
    }
    return document;
}

} // namespace

namespace jcut::standalone_render {

PreviewRenderResult renderPreviewFrame(const PreviewRenderRequest& request)
{
    const EditorDocumentCore renderDocument =
        documentWithResolvedMediaPaths(request.document, request.rootDirectory);

    render::RenderRequestCore renderRequest = renderDocument.exportRequest;
    renderRequest.outputSize = request.outputSize.valid()
        ? request.outputSize
        : renderDocument.exportRequest.outputSize;

    if (renderRequest.outputSize.valid()) {
        const render::TimelineRenderData timelineData =
            render::buildTimelineRenderData(renderDocument);
        const render::PreviewFrameResultCore qtResult =
            render::renderPreviewFrameCore(
                renderRequest,
                timelineData,
                static_cast<std::int64_t>(request.timelineFrame),
                false,
                !request.vulkanFrameOnly);
        if (qtResult.success && (!qtResult.image.empty() || qtResult.vulkanFrame.valid)) {
            PreviewRenderResult result;
            result.success = true;
            result.message = qtResult.effectiveRenderBackend.empty()
                ? std::string("preview frame rendered")
                : std::string("preview frame rendered with ") + qtResult.effectiveRenderBackend;
            result.image = qtResult.image;
            result.vulkanFrame = qtResult.vulkanFrame;
            return result;
        }

        const TimelineRenderResult fallbackResult = renderTimelineFrame({
            renderDocument,
            renderRequest.outputSize,
            static_cast<double>(request.timelineFrame),
            {}});
        PreviewRenderResult result;
        result.success = fallbackResult.success;
        result.message = qtResult.message.empty()
            ? fallbackResult.message
            : std::string("Qt preview failed: ") + qtResult.message +
                  "; fallback: " + fallbackResult.message;
        result.image = fallbackResult.image;
        result.sourcePath = fallbackResult.sourcePath;
        return result;
    }

    const TimelineRenderResult timelineResult = renderTimelineFrame({
        renderDocument,
        renderRequest.outputSize,
        static_cast<double>(request.timelineFrame),
        {}});

    PreviewRenderResult result;
    result.success = timelineResult.success;
    result.message = timelineResult.message;
    result.image = timelineResult.image;
    result.sourcePath = timelineResult.sourcePath;
    return result;
}

} // namespace jcut::standalone_render
