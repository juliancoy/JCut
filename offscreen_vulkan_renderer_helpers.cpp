#include "offscreen_vulkan_renderer_helpers.h"

#include "render_vulkan_shared.h"

#include <QFile>

extern "C" {
#include <libavutil/frame.h>
#include <libswscale/swscale.h>
}

#include <cstring>

namespace render_detail {

QByteArray curveLutBytesForGrade(const TimelineClip::GradingKeyframe& grade)
{
    return vulkanCurveLutRgbaBytes(grade);
}

QByteArray identityCurveLutBytes()
{
    return vulkanIdentityCurveLutRgbaBytes();
}

bool vulkanSubtitleDebugEnabled()
{
    return qEnvironmentVariableIntValue("EDITOR_DEBUG_VULKAN_SUBTITLES") == 1;
}

bool vulkanSubtitleDumpEnabled()
{
    return qEnvironmentVariableIntValue("EDITOR_DUMP_VULKAN_SUBTITLES") == 1;
}

SubtitlePixelCounts countSubtitlePixels(const QImage& image, const QRectF& bounds)
{
    SubtitlePixelCounts counts;
    if (image.isNull() || bounds.isEmpty()) {
        return counts;
    }
    const int left = qMax(0, static_cast<int>(std::floor(bounds.left())));
    const int top = qMax(0, static_cast<int>(std::floor(bounds.top())));
    const int right = qMin(image.width() - 1, static_cast<int>(std::ceil(bounds.right())));
    const int bottom = qMin(image.height() - 1, static_cast<int>(std::ceil(bounds.bottom())));
    for (int y = top; y <= bottom; ++y) {
        for (int x = left; x <= right; ++x) {
            const QColor c = image.pixelColor(x, y);
            if (c.alpha() > 8) {
                ++counts.nonTransparent;
            }
            if (c.alpha() > 80 && c.red() < 70 && c.green() < 70 && c.blue() < 70) {
                ++counts.dark;
            }
            if (c.alpha() > 80 && c.red() > 210 && c.green() > 210 && c.blue() > 210) {
                ++counts.bright;
            }
            if (c.alpha() > 80 && c.red() > 200 && c.green() > 180 && c.blue() > 80 && c.blue() < 220) {
                ++counts.yellow;
            }
        }
    }
    return counts;
}

QRectF alphaBoundsForImage(const QImage& image)
{
    if (image.isNull()) {
        return QRectF();
    }
    int minX = image.width();
    int minY = image.height();
    int maxX = -1;
    int maxY = -1;
    for (int y = 0; y < image.height(); ++y) {
        for (int x = 0; x < image.width(); ++x) {
            if (image.pixelColor(x, y).alpha() <= 8) {
                continue;
            }
            minX = qMin(minX, x);
            minY = qMin(minY, y);
            maxX = qMax(maxX, x);
            maxY = qMax(maxY, y);
        }
    }
    if (maxX < minX || maxY < minY) {
        return QRectF();
    }
    return QRectF(QPointF(minX, minY), QPointF(maxX + 1, maxY + 1));
}

QRectF alphaBoundsForOverlayImage(const OverlayImage& image)
{
    if (image.isNull()) {
        return QRectF();
    }
    int minX = image.width;
    int minY = image.height;
    int maxX = -1;
    int maxY = -1;
    const uchar* bits = reinterpret_cast<const uchar*>(image.rgbaPremultiplied.constData());
    for (int y = 0; y < image.height; ++y) {
        for (int x = 0; x < image.width; ++x) {
            const int offset = ((y * image.width) + x) * 4;
            if (bits[offset + 3] <= 8) {
                continue;
            }
            minX = qMin(minX, x);
            minY = qMin(minY, y);
            maxX = qMax(maxX, x);
            maxY = qMax(maxY, y);
        }
    }
    if (maxX < minX || maxY < minY) {
        return QRectF();
    }
    return QRectF(QPointF(minX, minY), QPointF(maxX + 1, maxY + 1));
}

OverlayImage scaledOverlayImage(const OverlayImage& image, const QSize& targetSize)
{
    if (image.isNull() || !targetSize.isValid()) {
        return {};
    }
    if (image.width == targetSize.width() && image.height == targetSize.height()) {
        return image;
    }

    OverlayImage scaled;
    scaled.width = targetSize.width();
    scaled.height = targetSize.height();
    scaled.rgbaPremultiplied.resize(scaled.width * scaled.height * 4);

    const uchar* src = reinterpret_cast<const uchar*>(image.rgbaPremultiplied.constData());
    uchar* dst = reinterpret_cast<uchar*>(scaled.rgbaPremultiplied.data());
    const qreal scaleX = static_cast<qreal>(image.width) / static_cast<qreal>(scaled.width);
    const qreal scaleY = static_cast<qreal>(image.height) / static_cast<qreal>(scaled.height);

    for (int y = 0; y < scaled.height; ++y) {
        const qreal srcY = (static_cast<qreal>(y) + 0.5) * scaleY - 0.5;
        const int y0 = qBound(0, static_cast<int>(std::floor(srcY)), image.height - 1);
        const int y1 = qMin(y0 + 1, image.height - 1);
        const qreal fy = qBound<qreal>(0.0, srcY - static_cast<qreal>(y0), 1.0);
        for (int x = 0; x < scaled.width; ++x) {
            const qreal srcX = (static_cast<qreal>(x) + 0.5) * scaleX - 0.5;
            const int x0 = qBound(0, static_cast<int>(std::floor(srcX)), image.width - 1);
            const int x1 = qMin(x0 + 1, image.width - 1);
            const qreal fx = qBound<qreal>(0.0, srcX - static_cast<qreal>(x0), 1.0);

            const int idx00 = ((y0 * image.width) + x0) * 4;
            const int idx10 = ((y0 * image.width) + x1) * 4;
            const int idx01 = ((y1 * image.width) + x0) * 4;
            const int idx11 = ((y1 * image.width) + x1) * 4;
            const int dstIdx = ((y * scaled.width) + x) * 4;

            for (int c = 0; c < 4; ++c) {
                const qreal top = (static_cast<qreal>(src[idx00 + c]) * (1.0 - fx)) +
                                  (static_cast<qreal>(src[idx10 + c]) * fx);
                const qreal bottom = (static_cast<qreal>(src[idx01 + c]) * (1.0 - fx)) +
                                     (static_cast<qreal>(src[idx11 + c]) * fx);
                dst[dstIdx + c] = static_cast<uchar>(qBound(0, qRound((top * (1.0 - fy)) + (bottom * fy)), 255));
            }
        }
    }

    return scaled;
}

QImage frameHandleToCpuImage(const editor::FrameHandle& frame)
{
    if (frame.hasCpuImage()) {
        return frame.cpuImage();
    }
    if (!frame.hasHardwareFrame()) {
        return QImage();
    }

    const AVFrame* hwFrame = frame.hardwareFrame();
    if (!hwFrame) {
        return QImage();
    }

    AVFrame* swFrame = av_frame_alloc();
    if (!swFrame) {
        return QImage();
    }

    QImage output;
    if (av_hwframe_transfer_data(swFrame, hwFrame, 0) < 0) {
        av_frame_free(&swFrame);
        return output;
    }

    AVPixelFormat sourceFormat = static_cast<AVPixelFormat>(swFrame->format);
    if (sourceFormat == AV_PIX_FMT_YUVJ420P) {
        sourceFormat = AV_PIX_FMT_YUV420P;
    }

    SwsContext* sws = sws_getContext(swFrame->width,
                                     swFrame->height,
                                     sourceFormat,
                                     swFrame->width,
                                     swFrame->height,
                                     AV_PIX_FMT_BGRA,
                                     SWS_BILINEAR,
                                     nullptr,
                                     nullptr,
                                     nullptr);
    if (!sws) {
        av_frame_free(&swFrame);
        return output;
    }

    output = QImage(swFrame->width, swFrame->height, QImage::Format_ARGB32);
    if (output.isNull()) {
        sws_freeContext(sws);
        av_frame_free(&swFrame);
        return output;
    }

    uint8_t* dstData[4] = {output.bits(), nullptr, nullptr, nullptr};
    int dstLinesize[4] = {static_cast<int>(output.bytesPerLine()), 0, 0, 0};
    sws_scale(sws,
              swFrame->data,
              swFrame->linesize,
              0,
              swFrame->height,
              dstData,
              dstLinesize);

    sws_freeContext(sws);
    av_frame_free(&swFrame);
    return output;
}

void blendPixel(QImage* image, int x, int y, const QColor& color, int alpha)
{
    if (!image || x < 0 || y < 0 || x >= image->width() || y >= image->height() || alpha <= 0) {
        return;
    }
    QColor src(color);
    src.setAlpha(qBound(0, alpha, 255));
    QColor dst = image->pixelColor(x, y);
    const qreal sa = src.alphaF();
    const qreal da = dst.alphaF();
    const qreal outA = sa + (da * (1.0 - sa));
    if (outA <= 0.0) {
        image->setPixelColor(x, y, Qt::transparent);
        return;
    }
    const qreal r = ((src.redF() * sa) + (dst.redF() * da * (1.0 - sa))) / outA;
    const qreal g = ((src.greenF() * sa) + (dst.greenF() * da * (1.0 - sa))) / outA;
    const qreal b = ((src.blueF() * sa) + (dst.blueF() * da * (1.0 - sa))) / outA;
    image->setPixelColor(x, y, QColor::fromRgbF(r, g, b, outA));
}

uint32_t findMemoryType(VkPhysicalDevice physicalDevice,
                        uint32_t typeFilter,
                        VkMemoryPropertyFlags properties)
{
    VkPhysicalDeviceMemoryProperties memProperties{};
    vkGetPhysicalDeviceMemoryProperties(physicalDevice, &memProperties);
    for (uint32_t i = 0; i < memProperties.memoryTypeCount; ++i) {
        if ((typeFilter & (1u << i)) &&
            (memProperties.memoryTypes[i].propertyFlags & properties) == properties) {
            return i;
        }
    }
    return UINT32_MAX;
}

uint32_t findMemoryTypePreferred(VkPhysicalDevice physicalDevice,
                                 uint32_t typeFilter,
                                 VkMemoryPropertyFlags required,
                                 VkMemoryPropertyFlags preferred,
                                 VkMemoryPropertyFlags* selectedFlags)
{
    VkPhysicalDeviceMemoryProperties memProperties{};
    vkGetPhysicalDeviceMemoryProperties(physicalDevice, &memProperties);
    uint32_t fallback = UINT32_MAX;
    VkMemoryPropertyFlags fallbackFlags = 0;
    for (uint32_t i = 0; i < memProperties.memoryTypeCount; ++i) {
        if (!(typeFilter & (1u << i))) {
            continue;
        }
        const VkMemoryPropertyFlags flags = memProperties.memoryTypes[i].propertyFlags;
        if ((flags & required) != required) {
            continue;
        }
        if ((flags & preferred) == preferred) {
            if (selectedFlags) {
                *selectedFlags = flags;
            }
            return i;
        }
        if (fallback == UINT32_MAX) {
            fallback = i;
            fallbackFlags = flags;
        }
    }
    if (selectedFlags && fallback != UINT32_MAX) {
        *selectedFlags = fallbackFlags;
    }
    return fallback;
}

bool physicalDeviceSupportsExtension(VkPhysicalDevice device, const char* extensionName)
{
    uint32_t extensionCount = 0;
    if (vkEnumerateDeviceExtensionProperties(device, nullptr, &extensionCount, nullptr) != VK_SUCCESS) {
        return false;
    }
    QVector<VkExtensionProperties> extensions(static_cast<int>(extensionCount));
    if (vkEnumerateDeviceExtensionProperties(device, nullptr, &extensionCount, extensions.data()) != VK_SUCCESS) {
        return false;
    }
    for (const VkExtensionProperties& extension : extensions) {
        if (qstrcmp(extension.extensionName, extensionName) == 0) {
            return true;
        }
    }
    return false;
}

QByteArray readBinaryFile(const QString& path)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        return {};
    }
    return file.readAll();
}

VkShaderModule createShaderModule(VkDevice device, const QByteArray& bytes)
{
    if (device == VK_NULL_HANDLE || bytes.isEmpty() || (bytes.size() % 4) != 0) {
        return VK_NULL_HANDLE;
    }
    VkShaderModuleCreateInfo info{};
    info.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
    info.codeSize = static_cast<size_t>(bytes.size());
    info.pCode = reinterpret_cast<const uint32_t*>(bytes.constData());
    VkShaderModule module = VK_NULL_HANDLE;
    if (vkCreateShaderModule(device, &info, nullptr, &module) != VK_SUCCESS) {
        return VK_NULL_HANDLE;
    }
    return module;
}

void transitionImageLayout(VkCommandBuffer cmd,
                           VkImage image,
                           VkImageLayout oldLayout,
                           VkImageLayout newLayout)
{
    if (oldLayout == newLayout) {
        return;
    }

    VkImageMemoryBarrier barrier{};
    barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    barrier.oldLayout = oldLayout;
    barrier.newLayout = newLayout;
    barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.image = image;
    barrier.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    barrier.subresourceRange.baseMipLevel = 0;
    barrier.subresourceRange.levelCount = 1;
    barrier.subresourceRange.baseArrayLayer = 0;
    barrier.subresourceRange.layerCount = 1;

    VkPipelineStageFlags srcStage = VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT;
    VkPipelineStageFlags dstStage = VK_PIPELINE_STAGE_TRANSFER_BIT;

    if (oldLayout == VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL &&
        newLayout == VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL) {
        barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
        srcStage = VK_PIPELINE_STAGE_TRANSFER_BIT;
        dstStage = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
    } else if (oldLayout == VK_IMAGE_LAYOUT_UNDEFINED &&
               newLayout == VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL) {
        barrier.srcAccessMask = 0;
        barrier.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        srcStage = VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT;
        dstStage = VK_PIPELINE_STAGE_TRANSFER_BIT;
    } else if (oldLayout == VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL &&
               newLayout == VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL) {
        barrier.srcAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
        barrier.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        srcStage = VK_PIPELINE_STAGE_TRANSFER_BIT;
        dstStage = VK_PIPELINE_STAGE_TRANSFER_BIT;
    } else if (oldLayout == VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL &&
               newLayout == VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL) {
        barrier.srcAccessMask = VK_ACCESS_SHADER_READ_BIT;
        barrier.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        srcStage = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT;
        dstStage = VK_PIPELINE_STAGE_TRANSFER_BIT;
    } else if (oldLayout == VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL &&
               newLayout == VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL) {
        barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        barrier.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
        srcStage = VK_PIPELINE_STAGE_TRANSFER_BIT;
        dstStage = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    } else if (oldLayout == VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL &&
               newLayout == VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL) {
        barrier.srcAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
        barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
        srcStage = VK_PIPELINE_STAGE_TRANSFER_BIT;
        dstStage = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
    } else if (oldLayout == VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL &&
               newLayout == VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL) {
        barrier.srcAccessMask = VK_ACCESS_SHADER_READ_BIT;
        barrier.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
        srcStage = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
        dstStage = VK_PIPELINE_STAGE_TRANSFER_BIT;
    } else if (oldLayout == VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL &&
               newLayout == VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL) {
        barrier.srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
        barrier.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
        srcStage = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
        dstStage = VK_PIPELINE_STAGE_TRANSFER_BIT;
    } else if (oldLayout == VK_IMAGE_LAYOUT_UNDEFINED &&
               newLayout == VK_IMAGE_LAYOUT_GENERAL) {
        barrier.srcAccessMask = 0;
        barrier.dstAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
        srcStage = VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT;
        dstStage = VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT;
    } else if (oldLayout == VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL &&
               newLayout == VK_IMAGE_LAYOUT_GENERAL) {
        barrier.srcAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
        barrier.dstAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
        srcStage = VK_PIPELINE_STAGE_TRANSFER_BIT;
        dstStage = VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT;
    } else if (oldLayout == VK_IMAGE_LAYOUT_GENERAL &&
               newLayout == VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL) {
        barrier.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
        barrier.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
        srcStage = VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT;
        dstStage = VK_PIPELINE_STAGE_TRANSFER_BIT;
    }

    vkCmdPipelineBarrier(cmd,
                         srcStage,
                         dstStage,
                         0,
                         0, nullptr,
                         0, nullptr,
                         1, &barrier);
}

} // namespace render_detail
