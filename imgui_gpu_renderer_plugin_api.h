#pragma once

#include "core/image_buffer.h"
#include "core/offscreen_vulkan_frame.h"
#include "editor_document_core.h"
#include "render_contract_types.h"

#include <cstddef>
#include <cstdint>

namespace jcut::imgui_gpu {

inline constexpr std::uint32_t kPluginApiVersion = 3;

using ExportProgressCallback = bool (*)(
    void* userData,
    const render::RenderProgressCore* progress);

using ApiVersionFunction = std::uint32_t (*)();
using InitializeFunction = bool (*)(
    char* errorBuffer,
    std::size_t errorBufferSize);
using ShutdownFunction = void (*)();
using RenderPreviewFunction = bool (*)(
    const EditorDocumentCore* document,
    const char* rootDirectory,
    int outputWidth,
    int outputHeight,
    std::int64_t timelineFrame,
    bool readbackToCpuImage,
    render_detail::OffscreenVulkanFrame* frameOut,
    core::ImageBuffer* imageOut,
    char* errorBuffer,
    std::size_t errorBufferSize);
using ExportTimelineFunction = bool (*)(
    const char* documentJson,
    const char* rootDirectory,
    render::RenderResultCore* resultOut,
    ExportProgressCallback progressCallback,
    void* progressUserData,
    char* errorBuffer,
    std::size_t errorBufferSize);

} // namespace jcut::imgui_gpu
