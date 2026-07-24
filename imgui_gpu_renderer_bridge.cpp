#include "imgui_gpu_renderer_bridge.h"

#include "editor_document_core_json.h"

#include <array>
#include <dlfcn.h>
#include <filesystem>
#include <utility>

namespace jcut::imgui_gpu {
namespace {

template <typename Function>
Function loadFunction(void* library, const char* name)
{
    return reinterpret_cast<Function>(dlsym(library, name));
}

struct ProgressContext {
    const std::function<bool(
        const render::RenderProgressCore&)>* callback = nullptr;
};

bool forwardProgress(
    void* userData,
    const render::RenderProgressCore* progress)
{
    const auto* context =
        static_cast<const ProgressContext*>(userData);
    return !context || !context->callback ||
        !*context->callback || !progress ||
        (*context->callback)(*progress);
}

} // namespace

struct RendererBridge::Impl {
    void* library = nullptr;
    ShutdownFunction shutdownFunction = nullptr;
    RenderPreviewFunction renderPreviewFunction = nullptr;
    ExportTimelineFunction exportTimelineFunction = nullptr;
    std::string currentStatus = "shared GPU renderer is not initialized";

    void close()
    {
        if (shutdownFunction) {
            shutdownFunction();
        }
        shutdownFunction = nullptr;
        renderPreviewFunction = nullptr;
        exportTimelineFunction = nullptr;
        if (library) {
            dlclose(library);
            library = nullptr;
        }
    }
};

RendererBridge::RendererBridge()
    : impl_(std::make_unique<Impl>())
{
}

RendererBridge::~RendererBridge()
{
    shutdown();
}

bool RendererBridge::initialize(
    const std::string& executablePath,
    std::string* errorOut)
{
    shutdown();
    const std::filesystem::path executableDirectory =
        std::filesystem::path(executablePath).parent_path();
    const std::array<std::filesystem::path, 2> candidates{{
        executableDirectory /
            "libjcut_imgui_gpu_renderer.so",
        executableDirectory.parent_path() /
            "libjcut_imgui_gpu_renderer.so",
    }};
    std::filesystem::path pluginPath;
    for (const std::filesystem::path& candidate : candidates) {
        if (std::filesystem::is_regular_file(candidate)) {
            pluginPath = candidate;
            break;
        }
    }
    if (pluginPath.empty()) {
        pluginPath = candidates.front();
    }
    impl_->library = dlopen(
        pluginPath.c_str(), RTLD_NOW | RTLD_LOCAL);
    if (!impl_->library) {
        const char* loadError = dlerror();
        impl_->currentStatus =
            "shared GPU renderer unavailable: " +
            std::string(loadError ? loadError : "load failed");
        if (errorOut) *errorOut = impl_->currentStatus;
        return false;
    }

    const ApiVersionFunction apiVersion =
        loadFunction<ApiVersionFunction>(
            impl_->library, "jcut_imgui_gpu_api_version");
    const InitializeFunction initializeFunction =
        loadFunction<InitializeFunction>(
            impl_->library, "jcut_imgui_gpu_initialize");
    impl_->shutdownFunction =
        loadFunction<ShutdownFunction>(
            impl_->library, "jcut_imgui_gpu_shutdown");
    impl_->renderPreviewFunction =
        loadFunction<RenderPreviewFunction>(
            impl_->library, "jcut_imgui_gpu_render_preview");
    impl_->exportTimelineFunction =
        loadFunction<ExportTimelineFunction>(
            impl_->library, "jcut_imgui_gpu_export_timeline");
    if (!apiVersion || !initializeFunction ||
        !impl_->shutdownFunction ||
        !impl_->renderPreviewFunction ||
        !impl_->exportTimelineFunction ||
        apiVersion() != kPluginApiVersion) {
        impl_->currentStatus =
            "shared GPU renderer has an incompatible API";
        impl_->close();
        if (errorOut) *errorOut = impl_->currentStatus;
        return false;
    }

    std::array<char, 2048> error{};
    if (!initializeFunction(error.data(), error.size())) {
        impl_->currentStatus = error[0] != '\0'
            ? error.data()
            : "shared GPU renderer initialization failed";
        impl_->close();
        if (errorOut) *errorOut = impl_->currentStatus;
        return false;
    }
    impl_->currentStatus =
        "Qt-free neutral Vulkan renderer plugin is active";
    return true;
}

void RendererBridge::shutdown()
{
    if (impl_) {
        impl_->close();
        impl_->currentStatus =
            "shared GPU renderer is not initialized";
    }
}

bool RendererBridge::available() const
{
    return impl_ && impl_->library &&
        impl_->renderPreviewFunction &&
        impl_->exportTimelineFunction;
}

const std::string& RendererBridge::status() const
{
    return impl_->currentStatus;
}

bool RendererBridge::renderPreview(
    const EditorDocumentCore& document,
    const std::string& rootDirectory,
    core::SizeI outputSize,
    std::int64_t timelineFrame,
    bool readbackToCpuImage,
    render_detail::OffscreenVulkanFrame* frameOut,
    core::ImageBuffer* imageOut,
    std::string* errorOut)
{
    if (!available() || !frameOut || !outputSize.valid()) {
        if (errorOut) {
            *errorOut = available()
                ? "invalid shared GPU preview request"
                : impl_->currentStatus;
        }
        return false;
    }
    std::array<char, 2048> error{};
    const bool rendered = impl_->renderPreviewFunction(
        &document,
        rootDirectory.c_str(),
        outputSize.width,
        outputSize.height,
        timelineFrame,
        readbackToCpuImage,
        frameOut,
        imageOut,
        error.data(),
        error.size());
    if (!rendered && errorOut) {
        *errorOut = error[0] != '\0'
            ? error.data()
            : "shared GPU preview failed";
    }
    return rendered;
}

render::RenderResultCore RendererBridge::exportTimeline(
    const EditorDocumentCore& document,
    const std::string& rootDirectory,
    const std::function<bool(
        const render::RenderProgressCore&)>& progressCallback)
{
    render::RenderResultCore result;
    if (!available()) {
        result.message = impl_->currentStatus;
        return result;
    }
    const std::string json = toJson(document).dump();
    std::array<char, 2048> error{};
    const ProgressContext progress{&progressCallback};
    if (!impl_->exportTimelineFunction(
            json.c_str(),
            rootDirectory.c_str(),
            &result,
            forwardProgress,
            const_cast<ProgressContext*>(&progress),
            error.data(),
            error.size()) &&
        result.message.empty()) {
        result.message = error[0] != '\0'
            ? error.data()
            : "shared GPU export failed";
    }
    return result;
}

} // namespace jcut::imgui_gpu
