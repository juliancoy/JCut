#include "imgui_gpu_renderer_plugin_api.h"

#include "editor_document_core_json.h"
#include "standalone_export_renderer.h"
#include "standalone_timeline_renderer.h"
#include "vulkan_compositor_core.h"

#include <cstdio>
#include <filesystem>
#include <memory>
#include <string>

namespace {

std::unique_ptr<jcut::standalone_render::TimelineRenderer>
    previewRenderer;
std::unique_ptr<jcut::vulkan::VulkanCompositorCore>
    compositor;

void writeError(
    char* buffer,
    std::size_t bufferSize,
    const std::string& message)
{
    if (!buffer || bufferSize == 0) return;
    std::snprintf(buffer, bufferSize, "%s", message.c_str());
}

std::string resolvePath(
    const std::string& value,
    const std::string& rootDirectory)
{
    if (value.empty()) return {};
    std::filesystem::path path(value);
    if (path.is_relative() && !rootDirectory.empty()) {
        path = std::filesystem::path(rootDirectory) / path;
    }
    return path.lexically_normal().string();
}

jcut::EditorDocumentCore resolvedDocument(
    jcut::EditorDocumentCore document,
    const std::string& rootDirectory)
{
    for (jcut::EditorClip& clip : document.clips) {
        clip.sourcePath =
            resolvePath(clip.sourcePath, rootDirectory);
        clip.proxyPath =
            resolvePath(clip.proxyPath, rootDirectory);
        clip.audioSourcePath =
            resolvePath(clip.audioSourcePath, rootDirectory);
        clip.maskFramesDir =
            resolvePath(clip.maskFramesDir, rootDirectory);
        clip.transcriptActiveCutPath =
            resolvePath(
                clip.transcriptActiveCutPath,
                rootDirectory);
    }
    document.exportRequest.outputPath =
        resolvePath(
            document.exportRequest.outputPath,
            rootDirectory);
    return document;
}

std::optional<jcut::EditorDocumentCore> parseDocument(
    const char* json,
    const char* rootDirectory,
    std::string* errorOut)
{
    const std::string bytes = json ? json : "";
    std::optional<jcut::EditorDocumentCore> document =
        jcut::editorDocumentCoreFromJsonBytes(bytes, errorOut);
    if (!document) return std::nullopt;
    return resolvedDocument(
        std::move(*document),
        rootDirectory ? rootDirectory : "");
}

} // namespace

extern "C" std::uint32_t jcut_imgui_gpu_api_version()
{
    return jcut::imgui_gpu::kPluginApiVersion;
}

extern "C" bool jcut_imgui_gpu_initialize(
    char* errorBuffer,
    std::size_t errorBufferSize)
{
    try {
        previewRenderer = std::make_unique<
            jcut::standalone_render::TimelineRenderer>();
        compositor = std::make_unique<
            jcut::vulkan::VulkanCompositorCore>();
    } catch (const std::exception& exception) {
        writeError(
            errorBuffer,
            errorBufferSize,
            std::string(
                "could not initialize neutral Vulkan renderer: ") +
                exception.what());
        return false;
    }
    return true;
}

extern "C" void jcut_imgui_gpu_shutdown()
{
    compositor.reset();
    previewRenderer.reset();
}

extern "C" bool jcut_imgui_gpu_render_preview(
    const jcut::EditorDocumentCore* documentInput,
    const char* rootDirectory,
    int outputWidth,
    int outputHeight,
    std::int64_t timelineFrame,
    bool readbackToCpuImage,
    render_detail::OffscreenVulkanFrame* frameOut,
    jcut::core::ImageBuffer* imageOut,
    char* errorBuffer,
    std::size_t errorBufferSize)
{
    if (!frameOut || outputWidth <= 0 || outputHeight <= 0) {
        writeError(
            errorBuffer, errorBufferSize,
            "invalid shared GPU preview request");
        return false;
    }
    *frameOut = {};
    if (imageOut) {
        *imageOut = {};
    }
    if (!documentInput) {
        writeError(
            errorBuffer, errorBufferSize,
            "shared GPU preview document is null");
        return false;
    }
    const jcut::EditorDocumentCore document =
        resolvedDocument(
            *documentInput,
            rootDirectory ? rootDirectory : "");

    if (!previewRenderer || !compositor) {
        writeError(
            errorBuffer, errorBufferSize,
            "neutral Vulkan renderer is not initialized");
        return false;
    }
    const jcut::standalone_render::TimelineRenderResult result =
        previewRenderer->renderFrame(
            jcut::standalone_render::TimelineRenderRequest{
                document,
                {outputWidth, outputHeight},
                static_cast<double>(timelineFrame),
                rootDirectory ? rootDirectory : "",
                {},
                false,
                true,
                false,
                true});
    if (!result.success || result.preparedLayers.empty()) {
        writeError(
            errorBuffer,
            errorBufferSize,
            result.message.empty()
                ? "neutral compositor returned no frame"
                : result.message);
        return false;
    }
    std::string uploadError;
    if (!compositor->compose(
            result.preparedLayers, frameOut, &uploadError)) {
        writeError(
            errorBuffer, errorBufferSize, uploadError);
        return false;
    }
    if (readbackToCpuImage && imageOut) {
        if (!compositor->readback(
                imageOut, &uploadError)) {
            writeError(
                errorBuffer,
                errorBufferSize,
                uploadError);
            return false;
        }
    }
    return true;
}

extern "C" bool jcut_imgui_gpu_export_timeline(
    const char* documentJson,
    const char* rootDirectory,
    jcut::render::RenderResultCore* resultOut,
    jcut::imgui_gpu::ExportProgressCallback progressCallback,
    void* progressUserData,
    char* errorBuffer,
    std::size_t errorBufferSize)
{
    if (!resultOut) {
        writeError(
            errorBuffer, errorBufferSize,
            "shared GPU export result is null");
        return false;
    }
    *resultOut = {};
    std::string parseError;
    const auto document = parseDocument(
        documentJson, rootDirectory, &parseError);
    if (!document) {
        resultOut->message = parseError;
        writeError(errorBuffer, errorBufferSize, parseError);
        return false;
    }
    jcut::standalone_render::ExportRenderRequest request;
    request.document = *document;
    request.rootDirectory =
        rootDirectory ? rootDirectory : "";
    *resultOut =
        jcut::standalone_render::exportTimelineToFile(
        request,
        progressCallback
            ? [progressCallback, progressUserData](
                  const jcut::render::RenderProgressCore& progress) {
                  return progressCallback(
                      progressUserData, &progress);
              }
            : jcut::standalone_render::ExportProgressCallback{});
    if (!resultOut->success && !resultOut->cancelled) {
        writeError(
            errorBuffer,
            errorBufferSize,
            resultOut->message.empty()
                ? "shared GPU export failed"
                : resultOut->message);
    }
    return resultOut->success || resultOut->cancelled;
}
