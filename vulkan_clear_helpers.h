#pragma once

#include <QRectF>
#include <QSize>
#include <QVulkanFunctions>

#include <vulkan/vulkan.h>

namespace jcut::vulkan {

VkClearRect clearRectFromQRect(const QRectF& qrect, const QSize& swapSize);
void clearRect(QVulkanDeviceFunctions* funcs,
               VkCommandBuffer cb,
               const VkClearValue& value,
               const VkClearRect& rect);
void clearBoxOutline(QVulkanDeviceFunctions* funcs,
                     VkCommandBuffer cb,
                     const VkClearValue& value,
                     const VkClearRect& boxRect,
                     int thickness);

} // namespace jcut::vulkan
