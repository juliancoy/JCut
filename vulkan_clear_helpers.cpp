#include "vulkan_clear_helpers.h"

#include <QPoint>
#include <QRect>

#include <algorithm>
#include <cmath>

namespace jcut::vulkan {

VkClearRect clearRectFromQRect(const QRectF& qrect, const QSize& swapSize)
{
    const int maxW = std::max(1, swapSize.width());
    const int maxH = std::max(1, swapSize.height());
    const QRect bounded = qrect.normalized().toAlignedRect().intersected(QRect(0, 0, maxW, maxH));
    VkClearRect rect{};
    rect.rect.offset = {bounded.x(), bounded.y()};
    rect.rect.extent = {
        static_cast<uint32_t>(std::max(1, bounded.width())),
        static_cast<uint32_t>(std::max(1, bounded.height()))
    };
    rect.baseArrayLayer = 0;
    rect.layerCount = 1;
    return rect;
}

void clearRect(QVulkanDeviceFunctions* funcs,
               VkCommandBuffer cb,
               const VkClearValue& value,
               const VkClearRect& rect)
{
    Q_UNUSED(funcs);
    VkClearAttachment attachment{};
    attachment.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    attachment.colorAttachment = 0;
    attachment.clearValue = value;
    vkCmdClearAttachments(cb, 1, &attachment, 1, &rect);
}

void clearRoundedRect(QVulkanDeviceFunctions* funcs,
                      VkCommandBuffer cb,
                      const VkClearValue& value,
                      const QRectF& qrect,
                      const QSize& swapSize,
                      int radius)
{
    const QRect bounded = qrect.normalized().toAlignedRect().intersected(
        QRect(0, 0, std::max(1, swapSize.width()), std::max(1, swapSize.height())));
    if (bounded.isEmpty()) {
        return;
    }
    const int r = std::max(0, std::min({radius, bounded.width() / 2, bounded.height() / 2}));
    if (r <= 0) {
        clearRect(funcs, cb, value, clearRectFromQRect(bounded, swapSize));
        return;
    }
    const int x = bounded.x();
    const int y = bounded.y();
    const int w = bounded.width();
    const int h = bounded.height();
    auto makeRect = [](int rx, int ry, int rw, int rh) {
        VkClearRect rect{};
        rect.rect.offset = {rx, ry};
        rect.rect.extent = {
            static_cast<uint32_t>(std::max(1, rw)),
            static_cast<uint32_t>(std::max(1, rh))
        };
        rect.baseArrayLayer = 0;
        rect.layerCount = 1;
        return rect;
    };
    clearRect(funcs, cb, value, makeRect(x + r, y, w - (2 * r), h));
    clearRect(funcs, cb, value, makeRect(x, y + r, r, h - (2 * r)));
    clearRect(funcs, cb, value, makeRect(x + w - r, y + r, r, h - (2 * r)));
    for (int dy = 0; dy < r; ++dy) {
        const double yCenter = static_cast<double>(r) - static_cast<double>(dy) - 0.5;
        const int dx = static_cast<int>(std::floor(std::sqrt(std::max(0.0, static_cast<double>(r * r) - (yCenter * yCenter)))));
        const int inset = std::max(0, r - dx);
        clearRect(funcs, cb, value, makeRect(x + inset, y + dy, w - (2 * inset), 1));
        clearRect(funcs, cb, value, makeRect(x + inset, y + h - dy - 1, w - (2 * inset), 1));
    }
}

void clearBoxOutline(QVulkanDeviceFunctions* funcs,
                     VkCommandBuffer cb,
                     const VkClearValue& value,
                     const VkClearRect& boxRect,
                     int thickness)
{
    const int x = boxRect.rect.offset.x;
    const int y = boxRect.rect.offset.y;
    const int w = static_cast<int>(boxRect.rect.extent.width);
    const int h = static_cast<int>(boxRect.rect.extent.height);
    const int t = std::max(1, std::min({thickness, std::max(1, w), std::max(1, h)}));
    auto makeRect = [](int rx, int ry, int rw, int rh) {
        VkClearRect rect{};
        rect.rect.offset = {rx, ry};
        rect.rect.extent = {
            static_cast<uint32_t>(std::max(1, rw)),
            static_cast<uint32_t>(std::max(1, rh))
        };
        rect.baseArrayLayer = 0;
        rect.layerCount = 1;
        return rect;
    };
    clearRect(funcs, cb, value, makeRect(x, y, w, t));
    clearRect(funcs, cb, value, makeRect(x, y + h - t, w, t));
    clearRect(funcs, cb, value, makeRect(x, y, t, h));
    clearRect(funcs, cb, value, makeRect(x + w - t, y, t, h));
}

} // namespace jcut::vulkan
