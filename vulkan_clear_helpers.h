#pragma once

#include <QRectF>
#include <QSize>

#include <vulkan/vulkan.h>

class QVulkanDeviceFunctions;

namespace jcut::vulkan {

VkClearRect clearRectFromQRect(const QRectF& qrect, const QSize& swapSize);
void clearRect(QVulkanDeviceFunctions* funcs,
               VkCommandBuffer cb,
               const VkClearValue& value,
               const VkClearRect& rect);
void clearRoundedRect(QVulkanDeviceFunctions* funcs,
                      VkCommandBuffer cb,
                      const VkClearValue& value,
                      const QRectF& rect,
                      const QSize& swapSize,
                      int radius);
void clearBoxOutline(QVulkanDeviceFunctions* funcs,
                     VkCommandBuffer cb,
                     const VkClearValue& value,
                     const VkClearRect& boxRect,
                     int thickness);

} // namespace jcut::vulkan
