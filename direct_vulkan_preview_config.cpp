#include "direct_vulkan_preview_config.h"

#include <QByteArray>

extern "C" {
#include <libavutil/pixdesc.h>
#include <libavutil/pixfmt.h>
}

namespace jcut::direct_vulkan_preview {
namespace {

bool envFlagEnabled(const char* name)
{
    const QByteArray value = qgetenv(name).trimmed().toLower();
    return value == "1" || value == "true" || value == "yes" || value == "on";
}

bool envFlagDisabled(const char* name)
{
    const QByteArray value = qgetenv(name).trimmed().toLower();
    return value == "0" || value == "false" || value == "no" || value == "off";
}

} // namespace

bool vulkanPreviewDebugChromeEnabled()
{
    return envFlagEnabled("JCUT_VULKAN_PREVIEW_DEBUG_CHROME");
}

bool vulkanPreviewOptimalPresentEnabled()
{
    if (!qEnvironmentVariableIsEmpty("JCUT_VULKAN_PREVIEW_OPTIMAL_PRESENT")) {
        return !envFlagDisabled("JCUT_VULKAN_PREVIEW_OPTIMAL_PRESENT");
    }
    return true;
}

bool vulkanPreviewReadbackMirrorEnabled()
{
    if (!qEnvironmentVariableIsEmpty("JCUT_VULKAN_PREVIEW_READBACK_MIRROR")) {
        return envFlagEnabled("JCUT_VULKAN_PREVIEW_READBACK_MIRROR");
    }
    return false;
}

bool vulkanPreviewDirectSwapchainVisible()
{
    return vulkanPreviewOptimalPresentEnabled() && !vulkanPreviewReadbackMirrorEnabled();
}

QString vulkanPreviewVisiblePathLabel()
{
    return vulkanPreviewReadbackMirrorEnabled()
        ? QStringLiteral("readback_mirror")
        : QStringLiteral("direct_swapchain");
}

int vulkanPreviewCanvasMarginPx()
{
    return 36;
}

QString pixelFormatName(int format)
{
    const char* name = av_get_pix_fmt_name(static_cast<AVPixelFormat>(format));
    return name ? QString::fromLatin1(name) : QStringLiteral("unknown:%1").arg(format);
}

QString vulkanFormatName(VkFormat format)
{
    switch (format) {
    case VK_FORMAT_R8G8B8A8_UNORM: return QStringLiteral("VK_FORMAT_R8G8B8A8_UNORM");
    case VK_FORMAT_B8G8R8A8_UNORM: return QStringLiteral("VK_FORMAT_B8G8R8A8_UNORM");
    case VK_FORMAT_R8G8B8A8_SRGB: return QStringLiteral("VK_FORMAT_R8G8B8A8_SRGB");
    case VK_FORMAT_B8G8R8A8_SRGB: return QStringLiteral("VK_FORMAT_B8G8R8A8_SRGB");
    default: return QStringLiteral("VkFormat:%1").arg(static_cast<int>(format));
    }
}

} // namespace jcut::direct_vulkan_preview
