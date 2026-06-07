#include "direct_vulkan_preview_geometry.h"

#include "editor_shared_effects.h"
#include "editor_shared_keyframes.h"
#include "preview_view_transform.h"

#include <QColor>
#include <QRect>

#include <algorithm>

namespace jcut::direct_vulkan_preview {

VkRect2D scissorFromQRect(const QRectF& qrect, const QSize& swapSize)
{
    const int maxW = std::max(1, swapSize.width());
    const int maxH = std::max(1, swapSize.height());
    const QRect bounded = qrect.normalized().toAlignedRect().intersected(QRect(0, 0, maxW, maxH));
    VkRect2D scissor{};
    scissor.offset = {bounded.x(), bounded.y()};
    scissor.extent = {
        static_cast<uint32_t>(std::max(1, bounded.width())),
        static_cast<uint32_t>(std::max(1, bounded.height()))
    };
    return scissor;
}

VkClearValue selectionOutlineColor()
{
    VkClearValue clear{};
    clear.color.float32[0] = 1.0f;
    clear.color.float32[1] = 0.9568627f;
    clear.color.float32[2] = 0.7607843f;
    clear.color.float32[3] = 1.0f;
    return clear;
}


void mvpForVulkanClipTransform(const QTransform& clipToSwapchain,
                               const QRectF& localRect,
                               const QSize& swapSize,
                               float outMvp[16])
{
    const float fullW = static_cast<float>(std::max(1, swapSize.width()));
    const float fullH = static_cast<float>(std::max(1, swapSize.height()));
    const float halfW = static_cast<float>(std::max<qreal>(1.0, localRect.width())) * 0.5f;
    const float halfH = static_cast<float>(std::max<qreal>(1.0, localRect.height())) * 0.5f;
    const float m11 = static_cast<float>(clipToSwapchain.m11());
    const float m12 = static_cast<float>(clipToSwapchain.m12());
    const float m21 = static_cast<float>(clipToSwapchain.m21());
    const float m22 = static_cast<float>(clipToSwapchain.m22());
    const float dx = static_cast<float>(clipToSwapchain.dx());
    const float dy = static_cast<float>(clipToSwapchain.dy());
    const float m[16] = {
        (2.0f * m11 * halfW) / fullW, (2.0f * m12 * halfW) / fullH, 0.f, 0.f,
        (2.0f * m21 * halfH) / fullW, (2.0f * m22 * halfH) / fullH, 0.f, 0.f,
        0.f,                0.f,                 1.f, 0.f,
        (2.0f * dx / fullW) - 1.0f,
        (2.0f * dy / fullH) - 1.0f,
        0.f,
        1.f
    };
    std::copy(std::begin(m), std::end(m), outMvp);
}

VkClearValue clipColor(const TimelineClip& clip, int ordinal, bool selected)
{
    QColor color = clip.color.isValid() ? clip.color : QColor::fromHsv((ordinal * 47) % 360, 150, 230);
    if (selected) {
        color = color.lighter(145);
    }
    VkClearValue clear{};
    clear.color.float32[0] = static_cast<float>(color.redF());
    clear.color.float32[1] = static_cast<float>(color.greenF());
    clear.color.float32[2] = static_cast<float>(color.blueF());
    clear.color.float32[3] = static_cast<float>(std::clamp(static_cast<double>(clip.opacity), 0.18, 1.0));
    return clear;
}

const VulkanPreviewClipFrameStatus* frameStatusForClip(const PreviewInteractionState* state, const QString& clipId)
{
    if (!state) {
        return nullptr;
    }
    for (const VulkanPreviewClipFrameStatus& status : state->vulkanFrameStatuses) {
        if (status.clipId == clipId) {
            return &status;
        }
    }
    return nullptr;
}

QByteArray curveLutRgbaBytes(const TimelineClip::GradingKeyframe& grade)
{
    const QVector<quint8> lutR = gradingCurveLut8(
        grade.curvePointsR, TimelineClip::kGradingCurveLutSize, grade.curveSmoothingEnabled);
    const QVector<quint8> lutG = gradingCurveLut8(
        grade.curvePointsG, TimelineClip::kGradingCurveLutSize, grade.curveSmoothingEnabled);
    const QVector<quint8> lutB = gradingCurveLut8(
        grade.curvePointsB, TimelineClip::kGradingCurveLutSize, grade.curveSmoothingEnabled);
    const QVector<quint8> lutL = gradingCurveLut8(
        grade.curvePointsLuma, TimelineClip::kGradingCurveLutSize, grade.curveSmoothingEnabled);
    if (lutR.size() != TimelineClip::kGradingCurveLutSize ||
        lutG.size() != TimelineClip::kGradingCurveLutSize ||
        lutB.size() != TimelineClip::kGradingCurveLutSize ||
        lutL.size() != TimelineClip::kGradingCurveLutSize) {
        return QByteArray();
    }
    QByteArray rgba;
    rgba.resize(TimelineClip::kGradingCurveLutSize * 4);
    for (int i = 0; i < TimelineClip::kGradingCurveLutSize; ++i) {
        rgba[i * 4 + 0] = static_cast<char>(lutR[i]);
        rgba[i * 4 + 1] = static_cast<char>(lutG[i]);
        rgba[i * 4 + 2] = static_cast<char>(lutB[i]);
        rgba[i * 4 + 3] = static_cast<char>(lutL[i]);
    }
    return rgba;
}

VkClearValue clipColorForStatus(const TimelineClip& clip,
                                int ordinal,
                                bool selected,
                                const VulkanPreviewClipFrameStatus* status)
{
    VkClearValue clear = clipColor(clip, ordinal, selected);
    if (!status || !status->hasFrame) {
        clear.color.float32[0] = 0.92f;
        clear.color.float32[1] = 0.05f;
        clear.color.float32[2] = 0.02f;
        clear.color.float32[3] = 0.95f;
        return clear;
    }
    if (status->hardwareFrame || status->gpuTexture) {
        clear.color.float32[0] = 0.08f;
        clear.color.float32[1] = status->exact ? 0.72f : 0.52f;
        clear.color.float32[2] = 0.38f;
    } else if (status->cpuImage) {
        clear.color.float32[0] = 0.84f;
        clear.color.float32[1] = status->exact ? 0.58f : 0.42f;
        clear.color.float32[2] = 0.12f;
    }
    clear.color.float32[3] = std::clamp(clear.color.float32[3], 0.35f, 1.0f);
    return clear;
}

VkClearValue facedetectionsOverlayColor(const PreviewInteractionState* state,
                                   const VulkanPreviewFacestreamOverlay& overlay)
{
    if (overlay.source.compare(QStringLiteral("raw_detection"), Qt::CaseInsensitive) == 0) {
        VkClearValue raw{};
        raw.color.float32[0] = 0.659f;
        raw.color.float32[1] = 0.333f;
        raw.color.float32[2] = 0.969f;
        raw.color.float32[3] = 0.90f;
        return raw;
    }
    if (overlay.source.compare(QStringLiteral("roi"), Qt::CaseInsensitive) == 0) {
        VkClearValue roi{};
        roi.color.float32[0] = 1.0f;
        roi.color.float32[1] = 0.667f;
        roi.color.float32[2] = 0.2f;
        roi.color.float32[3] = 0.95f;
        return roi;
    }
    if (state &&
        overlay.trackId >= 0 &&
        state->transient.hoveredFaceDetectionsTrackId == overlay.trackId &&
        state->transient.hoveredFaceDetectionsClipId == overlay.clipId &&
        state->transient.hoveredFaceDetectionsId == overlay.streamId) {
        VkClearValue hovered{};
        hovered.color.float32[0] = 0.96f;
        hovered.color.float32[1] = 0.82f;
        hovered.color.float32[2] = 0.99f;
        hovered.color.float32[3] = 1.0f;
        return hovered;
    }
    if (state &&
        overlay.trackId >= 0 &&
        state->selectedSpeakerAssignedFaceTrackIds.contains(overlay.trackId)) {
        VkClearValue assigned{};
        assigned.color.float32[0] = 0.29f;
        assigned.color.float32[1] = 0.87f;
        assigned.color.float32[2] = 0.50f;
        assigned.color.float32[3] = 0.95f;
        return assigned;
    }
    VkClearValue clear{};
    clear.color.float32[0] = 0.659f;
    clear.color.float32[1] = 0.333f;
    clear.color.float32[2] = 0.969f;
    clear.color.float32[3] = static_cast<float>(std::clamp(0.55 + (overlay.confidence * 0.35), 0.55, 0.95));
    return clear;
}

VkClearRect normalizedBoxToSwapchainRect(const QRectF& normalizedBox,
                                         const QTransform& clipToSwapchain,
                                         const QRectF& localRect,
                                         const QSize& swapSize,
                                         int lineInset)
{
    QRectF localBox = PreviewViewTransform::localRectForNormalizedRect(normalizedBox, localRect);
    QRect mapped = clipToSwapchain.mapRect(localBox).toAlignedRect();
    mapped = mapped.adjusted(lineInset, lineInset, -lineInset, -lineInset);
    mapped = mapped.intersected(QRect(QPoint(0, 0), swapSize));
    VkClearRect rect{};
    rect.rect.offset = {mapped.x(), mapped.y()};
    rect.rect.extent = {
        static_cast<uint32_t>(std::max(1, mapped.width())),
        static_cast<uint32_t>(std::max(1, mapped.height()))
    };
    rect.baseArrayLayer = 0;
    rect.layerCount = 1;
    return rect;
}

VkClearRect faceDetectionBoxToSwapchainRect(const QRectF& normalizedBox,
                                            const QTransform& clipToSwapchain,
                                            const QRectF& localRect,
                                            const QSize& swapSize)
{
    const QRectF localBox = PreviewViewTransform::localRectForNormalizedRect(normalizedBox, localRect);
    const QRectF mapped = clipToSwapchain.mapRect(localBox);
    const QRectF surfaceRect(QPointF(0.0, 0.0), QSizeF(qMax(1, swapSize.width()), qMax(1, swapSize.height())));
    if (mapped.intersects(surfaceRect)) {
        return normalizedBoxToSwapchainRect(normalizedBox, clipToSwapchain, localRect, swapSize);
    }

    constexpr qreal kOffscreenMarkerSizePx = 26.0;
    constexpr qreal kOffscreenMarkerInsetPx = 8.0;
    const QPointF center = clipToSwapchain.map(
        PreviewViewTransform::localPointForNormalizedPoint(normalizedBox.center(), localRect));
    const QRectF marker(
        qBound<qreal>(surfaceRect.left() + kOffscreenMarkerInsetPx,
                      center.x(),
                      surfaceRect.right() - kOffscreenMarkerInsetPx) - (kOffscreenMarkerSizePx * 0.5),
        qBound<qreal>(surfaceRect.top() + kOffscreenMarkerInsetPx,
                      center.y(),
                      surfaceRect.bottom() - kOffscreenMarkerInsetPx) - (kOffscreenMarkerSizePx * 0.5),
        kOffscreenMarkerSizePx,
        kOffscreenMarkerSizePx);
    return clearRectFromQRect(marker, swapSize);
}

const TimelineClip* selectedClipForTargetBox(const PreviewInteractionState* state)
{
    if (!state) {
        return nullptr;
    }
    const QString selectedId = state->selectedClipId.trimmed();
    if (!selectedId.isEmpty()) {
        for (const TimelineClip& clip : state->clips) {
            const TimelineClip::TransformKeyframe targetState =
                evaluateClipSpeakerFramingTargetAtFrame(clip, state->currentFrame);
            if (clip.id == selectedId &&
                qBound<qreal>(-1.0, targetState.scaleX, 1.0) > 0.0) {
                return &clip;
            }
        }
    }
    for (const TimelineClip& clip : state->clips) {
        const TimelineClip::TransformKeyframe targetState =
            evaluateClipSpeakerFramingTargetAtFrame(clip, state->currentFrame);
        if (qBound<qreal>(-1.0, targetState.scaleX, 1.0) > 0.0) {
            return &clip;
        }
    }
    return nullptr;
}

VkClearValue targetBoxOverlayColor()
{
    VkClearValue clear{};
    clear.color.float32[0] = 1.0f;
    clear.color.float32[1] = 0.886f;
    clear.color.float32[2] = 0.29f;
    clear.color.float32[3] = 0.95f;
    return clear;
}

VkClearRect targetBoxRectForComposite(const TimelineClip& clip,
                                      int64_t timelineFrame,
                                      const QRectF& compositeRect,
                                      const QSize& swapSize)
{
    const TimelineClip::TransformKeyframe targetState =
        evaluateClipSpeakerFramingTargetAtFrame(clip, timelineFrame);
    const qreal targetXNorm = qBound<qreal>(0.0, targetState.translationX, 1.0);
    const qreal targetYNorm = qBound<qreal>(0.0, targetState.translationY, 1.0);
    const qreal targetBoxNorm = qBound<qreal>(-1.0, targetState.scaleX, 1.0);
    const qreal centerX = compositeRect.left() + (targetXNorm * compositeRect.width());
    const qreal centerY = compositeRect.top() + (targetYNorm * compositeRect.height());
    const qreal side = qBound<qreal>(
        2.0,
        targetBoxNorm * qMax<qreal>(1.0, qMin<qreal>(compositeRect.width(), compositeRect.height())),
        4096.0);
    const qreal halfSide = side * 0.5;
    const QRectF targetRect(centerX - halfSide, centerY - halfSide, side, side);
    return clearRectFromQRect(targetRect, swapSize);
}

} // namespace jcut::direct_vulkan_preview
