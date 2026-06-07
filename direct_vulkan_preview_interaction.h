#pragma once

#include "preview_interaction_state.h"
#include "preview_overlay_model.h"
#include "preview_view_transform.h"

#include <QPainterPath>
#include <QRectF>
#include <QSizeF>
#include <QTransform>
#include <QVector>

#include <functional>

namespace jcut::direct_vulkan_preview {

struct VulkanInteractionOverlayInfo {
    QString clipId;
    QRectF bounds;
    QRectF rightHandle;
    QRectF bottomHandle;
    QRectF cornerHandle;
    PreviewOverlayKind kind = PreviewOverlayKind::VisualClip;
    QTransform clipToScreen;
    QRectF localRect;
    TimelineClip::TransformKeyframe transform;
    QSizeF clipPixelSize;
    QRectF surfaceRect;
};

using VulkanInteractionOverlayInfos = QVector<VulkanInteractionOverlayInfo>;

bool applyVideoPreviewWheelZoom(PreviewInteractionState* state,
                                const QRectF& surfaceRect,
                                const QPointF& surfacePosition,
                                int deltaY);
bool applyAudioPreviewWheelZoom(PreviewInteractionState* state,
                                const QRectF& surfaceRect,
                                const QPointF& surfacePosition,
                                int deltaY);
bool audioSeekSampleAtSurfacePosition(const PreviewInteractionState& state,
                                      const QRectF& surfaceRect,
                                      const QPointF& surfacePosition,
                                      int64_t* sampleOut);
bool clipSupportsTranscriptOverlay(const TimelineClip& clip);
const TimelineClip* clipForId(const PreviewInteractionState* state, const QString& clipId);
TimelineClip clipWithTransientTranscriptOverride(const PreviewInteractionState* state, const TimelineClip& clip);
TimelineClip::TransformKeyframe transformWithTransientOverride(const PreviewInteractionState* state,
                                                               const QString& clipId,
                                                               const TimelineClip::TransformKeyframe& transform);
void clearVulkanDragOverrides(PreviewInteractionState* state);
QRectF transcriptOverlayBoundsForClip(const PreviewInteractionState* state,
                                      const TimelineClip& clip,
                                      const PreviewViewTransform& viewTransform,
                                      bool requireInteraction = true);
VulkanInteractionOverlayInfos collectVulkanInteractionInfos(const PreviewInteractionState* state,
                                                            const QRectF& surfaceRect);
QString clipIdAtPositionForVulkan(const VulkanInteractionOverlayInfos& infos, const QPointF& position);
QPointF mapScreenPointToNormalizedClipForVulkan(const VulkanInteractionOverlayInfo& info,
                                                const QPointF& screenPoint);
QPointF mapNormalizedClipPointToScreenForVulkan(const VulkanInteractionOverlayInfo& info,
                                                const QPointF& normalizedPoint);
QPainterPath faceDetectionScreenPathForVulkan(const VulkanInteractionOverlayInfo& info,
                                              const QRectF& normalizedBox,
                                              const QRectF& surfaceRect);
bool lookupVulkanInteractionInfo(const VulkanInteractionOverlayInfos& infos,
                                 const QString& clipId,
                                 VulkanInteractionOverlayInfo* outInfo);
bool dispatchFaceDetectionsBoxAtPosition(
    const PreviewInteractionState* state,
    const VulkanInteractionOverlayInfos& infos,
    const QPointF& surfacePosition,
    const std::function<void(const QString&, int, const QString&, int64_t, qreal, qreal, qreal)>& callback,
    const std::function<void(const QString&)>& statusCallback);
bool dispatchFaceDetectionsFocusClearAtPosition(
    const PreviewInteractionState* state,
    const VulkanInteractionOverlayInfos& infos,
    const QPointF& surfacePosition,
    const std::function<void(const QString&, int, const QString&, int64_t, qreal, qreal, qreal)>& callback,
    const std::function<void(const QString&)>& statusCallback);
bool updateHoveredFaceDetectionsBox(const PreviewInteractionState* state,
                                    const VulkanInteractionOverlayInfos& infos,
                                    const QPointF& surfacePosition);
bool clipIdIsTitleForVulkan(const PreviewInteractionState* state, const QString& clipId);
TimelineClip::TransformKeyframe currentTransformForVulkanClip(const PreviewInteractionState* state,
                                                              const QString& clipId);

} // namespace jcut::direct_vulkan_preview
