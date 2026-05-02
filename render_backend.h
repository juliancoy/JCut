#pragma once

#include <QString>

enum class RenderBackend {
    OpenGL,
    Vulkan,
    Null,
    Auto,
};

RenderBackend parseRenderBackend(const QString& text);
QString renderBackendName(RenderBackend backend);
RenderBackend desiredRenderBackendFromEnvironment();
RenderBackend desiredPreviewBackendFromEnvironment();
