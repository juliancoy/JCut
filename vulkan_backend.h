#pragma once

#include <memory>
#include <QString>

class QRhi;

struct VulkanAvailabilityResult {
    bool available = false;
    QString status;
};

struct VulkanBackendResult {
    std::unique_ptr<QRhi> rhi;
    QString status;
};

VulkanAvailabilityResult probeVulkanBackendAvailability();
VulkanBackendResult createVulkanBackendRhi();
