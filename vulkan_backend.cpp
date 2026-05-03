#include "vulkan_backend.h"

#include <QVersionNumber>
#include <QVulkanInstance>
#include <QtGui/private/qrhi_p.h>
#include <QtGui/private/qrhivulkan_p.h>

VulkanAvailabilityResult probeVulkanBackendAvailability()
{
    VulkanAvailabilityResult result;
    QVulkanInstance instance;
    instance.setApiVersion(QVersionNumber(1, 1));
    if (instance.create()) {
        result.available = true;
        result.status = QStringLiteral("ok");
    } else {
        result.status = QStringLiteral("QVulkanInstance::create() failed");
    }
    return result;
}

VulkanBackendResult createVulkanBackendRhi()
{
    VulkanBackendResult result;
    QRhiVulkanInitParams params;
    result.rhi.reset(QRhi::create(QRhi::Vulkan, &params, QRhi::Flags()));
    if (result.rhi) {
        result.status = QStringLiteral("ok");
    } else {
        result.status = QStringLiteral("QRhi::create(Vulkan) failed");
    }
    return result;
}
