#include "render_backend.h"

RenderBackend parseRenderBackend(const QString& text)
{
    const QString normalized = text.trimmed().toLower();
    if (normalized == QStringLiteral("vulkan")) {
        return RenderBackend::Vulkan;
    }
    if (normalized == QStringLiteral("null")) {
        return RenderBackend::Null;
    }
    if (normalized == QStringLiteral("auto")) {
        return RenderBackend::Auto;
    }
    return RenderBackend::Vulkan;
}

QString renderBackendName(RenderBackend backend)
{
    switch (backend) {
    case RenderBackend::Vulkan:
        return QStringLiteral("vulkan");
    case RenderBackend::Null:
        return QStringLiteral("null");
    case RenderBackend::Auto:
        return QStringLiteral("auto");
    default:
        return QStringLiteral("vulkan");
    }
}

RenderBackend desiredRenderBackendFromEnvironment()
{
    const QString configured = qEnvironmentVariable("JCUT_RENDER_BACKEND").trimmed();
    if (configured.isEmpty()) {
        return RenderBackend::Vulkan;
    }
    return parseRenderBackend(configured);
}

RenderBackend desiredPreviewBackendFromEnvironment()
{
    const QString configured = qEnvironmentVariable("JCUT_PREVIEW_BACKEND").trimmed();
    if (configured.isEmpty()) {
        return RenderBackend::Vulkan;
    }
    return parseRenderBackend(configured);
}
