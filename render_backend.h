#pragma once

#include <QString>

enum class RenderBackend {
    Vulkan,
    Null,
    Auto,
};

RenderBackend parseRenderBackend(const QString& text);
QString renderBackendName(RenderBackend backend);
RenderBackend desiredRenderBackendFromEnvironment();
RenderBackend desiredPreviewBackendFromEnvironment();
