#include "imgui_gpu_renderer_plugin_api.h"

#include "editor_document_core_json.h"
#include "editor_document_render_bridge.h"
#include "render_runtime.h"

#include <QCoreApplication>
#include <QGuiApplication>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <memory>
#include <string>
#include <thread>

namespace {

std::unique_ptr<QGuiApplication> application;
int applicationArgc = 1;
std::array<char, 32> applicationName{"jcut-gpu-renderer"};
std::array<char*, 2> applicationArgv{
    applicationName.data(), nullptr};

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
    if (QCoreApplication::instance()) {
        return true;
    }
    if (qEnvironmentVariableIsEmpty("QT_QPA_PLATFORM")) {
        qputenv("QT_QPA_PLATFORM", "offscreen");
    }
    try {
        application = std::make_unique<QGuiApplication>(
            applicationArgc, applicationArgv.data());
    } catch (const std::exception& exception) {
        writeError(
            errorBuffer,
            errorBufferSize,
            std::string(
                "could not initialize shared GPU renderer: ") +
                exception.what());
        return false;
    }
    return true;
}

extern "C" void jcut_imgui_gpu_shutdown()
{
    application.reset();
}

extern "C" bool jcut_imgui_gpu_render_preview(
    const char* documentJson,
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
    std::string parseError;
    const auto document = parseDocument(
        documentJson, rootDirectory, &parseError);
    if (!document) {
        writeError(errorBuffer, errorBufferSize, parseError);
        return false;
    }

    jcut::render::RenderRequestCore request =
        document->exportRequest;
    request.outputPath = "preview://imgui-shared-gpu";
    request.outputFormat = "preview";
    request.outputSize = {outputWidth, outputHeight};
    request.exportStartFrame = timelineFrame;
    request.exportEndFrame = timelineFrame;
    const jcut::render::TimelineRenderData timeline =
        jcut::render::buildTimelineRenderData(
            *document, false);

    jcut::render::PreviewFrameResultCore result;
    for (int attempt = 0; attempt < 40; ++attempt) {
        result = jcut::render::renderPreviewFrameCore(
            request,
            timeline,
            timelineFrame,
            false,
            readbackToCpuImage);
        if (result.success && result.vulkanFrame.valid) {
            *frameOut = result.vulkanFrame;
            if (imageOut) {
                *imageOut = std::move(result.image);
            }
            return true;
        }
        std::this_thread::sleep_for(
            std::chrono::milliseconds(10));
    }
    writeError(
        errorBuffer,
        errorBufferSize,
        result.message.empty()
            ? "shared Vulkan preview returned no frame"
            : result.message);
    return false;
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
    const jcut::render::TimelineRenderData timeline =
        jcut::render::buildTimelineRenderData(
            *document, true);
    *resultOut = jcut::render::renderTimelineToFileCore(
        document->exportRequest,
        timeline,
        progressCallback
            ? [progressCallback, progressUserData](
                  const jcut::render::RenderProgressCore& progress) {
                  return progressCallback(
                      progressUserData, &progress);
              }
            : jcut::render::RenderProgressCoreCallback{});
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
