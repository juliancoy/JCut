#pragma once

#include "preview_interaction_state.h"
#include "vulkan_clear_helpers.h"

#include <QByteArray>
#include <QRectF>
#include <QSize>
#include <QTransform>
#include <QVulkanFunctions>

#include <vulkan/vulkan.h>

namespace jcut::direct_vulkan_preview {

using jcut::vulkan::clearBoxOutline;
using jcut::vulkan::clearRect;
using jcut::vulkan::clearRectFromQRect;

VkRect2D scissorFromQRect(const QRectF& qrect, const QSize& swapSize);
VkClearValue selectionOutlineColor();
void mvpForVulkanClipTransform(const QTransform& clipToSwapchain,
                               const QRectF& localRect,
                               const QSize& swapSize,
                               float outMvp[16]);
VkClearValue clipColor(const TimelineClip& clip, int ordinal, bool selected);
const VulkanPreviewClipFrameStatus* frameStatusForClip(const PreviewInteractionState* state,
                                                       const QString& clipId);
QByteArray curveLutRgbaBytes(const TimelineClip::GradingKeyframe& grade);
VkClearValue clipColorForStatus(const TimelineClip& clip,
                                int ordinal,
                                bool selected,
                                const VulkanPreviewClipFrameStatus* status);
VkClearValue facedetectionsOverlayColor(const PreviewInteractionState* state,
                                        const VulkanPreviewFacestreamOverlay& overlay);
VkClearRect normalizedBoxToSwapchainRect(const QRectF& normalizedBox,
                                         const QTransform& clipToSwapchain,
                                         const QRectF& localRect,
                                         const QSize& swapSize,
                                         int lineInset = 0);
VkClearRect faceDetectionBoxToSwapchainRect(const QRectF& normalizedBox,
                                            const QTransform& clipToSwapchain,
                                            const QRectF& localRect,
                                            const QSize& swapSize);
const TimelineClip* selectedClipForTargetBox(const PreviewInteractionState* state);
VkClearValue targetBoxOverlayColor();
VkClearRect targetBoxRectForComposite(const TimelineClip& clip,
                                      int64_t timelineFrame,
                                      const QRectF& compositeRect,
                                      const QSize& swapSize);

} // namespace jcut::direct_vulkan_preview
