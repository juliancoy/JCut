#include "render_internal.h"
#include "vulkan_backend.h"

#include <QtGlobal>

namespace render_detail {

class OffscreenVulkanRendererPrivate {};

OffscreenVulkanRenderer::OffscreenVulkanRenderer()
    : d(std::make_unique<OffscreenVulkanRendererPrivate>())
{
}

OffscreenVulkanRenderer::~OffscreenVulkanRenderer() = default;

bool OffscreenVulkanRenderer::initialize(const QSize&, QString* errorMessage)
{
    if (errorMessage) {
        *errorMessage = QStringLiteral("Vulkan export backend is unavailable in jcut_imgui.");
    }
    return false;
}

QImage OffscreenVulkanRenderer::renderFrame(const OffscreenRenderContext&)
{
    return {};
}

bool OffscreenVulkanRenderer::renderFrameToOutput(const OffscreenRenderContext&,
                                                  OffscreenRenderFrame*,
                                                  bool)
{
    return false;
}

bool OffscreenVulkanRenderer::convertLastFrameToNv12(AVFrame*, qint64*, qint64*)
{
    return false;
}

bool OffscreenVulkanRenderer::beginLastFrameToNv12Readback(qint64*, qint64*)
{
    return false;
}

bool OffscreenVulkanRenderer::finishLastFrameToNv12Readback(AVFrame*, qint64*, qint64*)
{
    return false;
}

bool OffscreenVulkanRenderer::beginLastFrameToNv12CudaTransfer(qint64*, qint64*)
{
    return false;
}

bool OffscreenVulkanRenderer::finishLastFrameToNv12CudaTransfer(AVFrame*, qint64*, qint64*)
{
    return false;
}

bool OffscreenVulkanRenderer::convertLastFrameToYuv420p(AVFrame*, qint64*, qint64*)
{
    return false;
}

bool OffscreenVulkanRenderer::beginLastFrameToYuv420pReadback(qint64*, qint64*)
{
    return false;
}

bool OffscreenVulkanRenderer::finishLastFrameToYuv420pReadback(AVFrame*, qint64*, qint64*)
{
    return false;
}

bool OffscreenVulkanRenderer::copyLastFrameToBgra(AVFrame*, qint64*)
{
    return false;
}

bool OffscreenVulkanRenderer::lastRenderedVulkanFrame(OffscreenVulkanFrame*, QString* errorMessage) const
{
    if (errorMessage) {
        *errorMessage = QStringLiteral("No Vulkan frame available.");
    }
    return false;
}

bool OffscreenVulkanRenderer::supportsCudaExternalMemoryInterop() const
{
    return false;
}

QString OffscreenVulkanRenderer::backendId() const
{
    return QStringLiteral("vulkan_stub");
}

} // namespace render_detail

VulkanAvailabilityResult probeVulkanBackendAvailability()
{
    return {false, QStringLiteral("Vulkan export backend is unavailable in jcut_imgui.")};
}
