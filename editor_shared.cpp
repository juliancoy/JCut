#include "editor_shared.h"
#include "preview_view_transform.h"

#include <Qt>

// Definitions formerly in this file were split into:
// - editor_shared_media.cpp
// - editor_shared_timing.cpp
// - editor_shared_keyframes.cpp
// - editor_shared_effects.cpp
// - editor_shared_render_sync.cpp
// - editor_shared_transcript.cpp
//
// Keep this translation unit intentionally minimal to avoid duplicate
// definitions once the new .cpp files are added to the build.

QRect previewCanvasBaseRectForWidget(const QRect& widgetRect,
                                     const QSize& outputSize,
                                     int marginPx) {
    return previewCanvasBaseRectForWidgetF(QRectF(widgetRect), outputSize, marginPx).toAlignedRect();
}

QRectF previewCanvasBaseRectForWidgetF(const QRectF& widgetRect,
                                       const QSize& outputSize,
                                       qreal marginPx) {
    return PreviewViewTransform::baseRectForWidget(widgetRect, outputSize, marginPx);
}

QRect scaledPreviewCanvasRect(const QRect& baseRect,
                              qreal previewZoom,
                              const QPointF& previewPanOffset) {
    const QRectF rect = scaledPreviewCanvasRectF(QRectF(baseRect), previewZoom, previewPanOffset);
    return QRect(qRound(rect.x()), qRound(rect.y()), qRound(rect.width()), qRound(rect.height()));
}

QRectF scaledPreviewCanvasRectF(const QRectF& baseRect,
                                qreal previewZoom,
                                const QPointF& previewPanOffset) {
    return PreviewViewTransform::scaledRect(baseRect, previewZoom, previewPanOffset);
}

QPointF clampedPreviewPanOffset(const QRectF& baseRect,
                                qreal previewZoom,
                                const QPointF& previewPanOffset) {
    return PreviewViewTransform::clampedPanOffset(baseRect, previewZoom, previewPanOffset);
}

QPointF previewCanvasScaleForTargetRect(const QRect& targetRect,
                                        const QSize& outputSize) {
    return previewCanvasScaleForTargetRectF(QRectF(targetRect), outputSize);
}

QPointF previewCanvasScaleForTargetRectF(const QRectF& targetRect,
                                         const QSize& outputSize) {
    return PreviewViewTransform::scaleForTargetRect(targetRect, outputSize);
}

QRect previewFitRectToBounds(const QSize& source, const QRect& bounds) {
    const QRectF rect = previewFitRectToBoundsF(source, QRectF(bounds));
    return QRect(qRound(rect.x()), qRound(rect.y()), qRound(rect.width()), qRound(rect.height()));
}

QRectF previewFitRectToBoundsF(const QSize& source, const QRectF& bounds) {
    return PreviewViewTransform::fitRectToBounds(source, bounds);
}
