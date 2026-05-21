#pragma once

#include "render_internal.h"

#include <QByteArray>
#include <QImage>
#include <QRectF>

#include <vulkan/vulkan.h>

namespace render_detail {

constexpr int kCurveLutWidth = TimelineClip::kGradingCurveLutSize;
constexpr int kCurveLutHeight = 1;
constexpr VkDeviceSize kCurveLutBytes = kCurveLutWidth * kCurveLutHeight * 4;

struct SubtitlePixelCounts {
    int dark = 0;
    int bright = 0;
    int yellow = 0;
    int nonTransparent = 0;
};

QByteArray curveLutBytesForGrade(const TimelineClip::GradingKeyframe& grade);
QByteArray identityCurveLutBytes();
bool vulkanSubtitleDebugEnabled();
bool vulkanSubtitleDumpEnabled();
SubtitlePixelCounts countSubtitlePixels(const QImage& image, const QRectF& bounds);
QRectF alphaBoundsForImage(const QImage& image);
QRectF alphaBoundsForOverlayImage(const OverlayImage& image);
OverlayImage scaledOverlayImage(const OverlayImage& image, const QSize& targetSize);
QImage frameHandleToCpuImage(const editor::FrameHandle& frame);
void blendPixel(QImage* image, int x, int y, const QColor& color, int alpha);
uint32_t findMemoryType(VkPhysicalDevice physicalDevice,
                        uint32_t typeFilter,
                        VkMemoryPropertyFlags properties);
uint32_t findMemoryTypePreferred(VkPhysicalDevice physicalDevice,
                                 uint32_t typeFilter,
                                 VkMemoryPropertyFlags required,
                                 VkMemoryPropertyFlags preferred,
                                 VkMemoryPropertyFlags* selectedFlags = nullptr);
bool physicalDeviceSupportsExtension(VkPhysicalDevice device, const char* extensionName);
QByteArray readBinaryFile(const QString& path);
VkShaderModule createShaderModule(VkDevice device, const QByteArray& bytes);
void transitionImageLayout(VkCommandBuffer cmd,
                           VkImage image,
                           VkImageLayout oldLayout,
                           VkImageLayout newLayout);

} // namespace render_detail
