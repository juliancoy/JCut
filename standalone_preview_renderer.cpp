#include "standalone_preview_renderer.h"
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

    const core::SizeI outputSize = request.outputSize.valid()
        ? request.outputSize
        : renderDocument.exportRequest.outputSize;
    const TimelineRenderResult timelineResult = renderTimelineFrame({
        renderDocument,
        outputSize,
        static_cast<double>(request.timelineFrame),
        {},
        request.decoderPolicy,
        request.preferVulkanFrame,
        request.allowCpuFallback});

    PreviewRenderResult result;
    result.success = timelineResult.success;
    result.message =
        request.preferVulkanFrame &&
            timelineResult.success &&
            !timelineResult.hardwareFrame
        ? "Qt-free CPU preview fallback: " + timelineResult.message
        : timelineResult.message;
    result.image = timelineResult.image;
    result.hardwareFrame = timelineResult.hardwareFrame;
    result.hardwareDirectEligible =
        timelineResult.hardwareDirectEligible;
    result.hardwareDirectFallbackReason =
        timelineResult.hardwareDirectFallbackReason;
    result.hardwarePresentationTransformValid =
        timelineResult.hardwarePresentationTransformValid;
    result.hardwarePresentationTransform =
        timelineResult.hardwarePresentationTransform;
    result.hardwarePresentationOpacity =
        timelineResult.hardwarePresentationOpacity;
    result.hardwarePresentationGrade =
        timelineResult.hardwarePresentationGrade;
    result.hardwareOverlayImage =
        timelineResult.hardwareOverlayImage;
    result.hardwareOverlayX =
        timelineResult.hardwareOverlayX;
    result.hardwareOverlayY =
        timelineResult.hardwareOverlayY;
    result.sourcePath = timelineResult.sourcePath;
    result.requestedDecodePreference =
        timelineResult.requestedDecodePreference;
    result.effectiveDecodePreference =
        timelineResult.effectiveDecodePreference;
    result.hardwareAccelerated =
        timelineResult.hardwareAccelerated;
    result.hardwareDeviceLabel =
        timelineResult.hardwareDeviceLabel;
    result.hardwareFallbackReason =
        timelineResult.hardwareFallbackReason;
    return result;
}

} // namespace jcut::standalone_render
