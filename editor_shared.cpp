#include "editor_shared.h"

#include <Qt>

#include <algorithm>

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
    const QRect available = widgetRect.adjusted(marginPx, marginPx, -marginPx, -marginPx);
    if (!available.isValid()) {
        return available;
    }
    QSize fitted = outputSize.isValid() ? outputSize : QSize(1080, 1920);
    fitted.scale(available.size(), Qt::KeepAspectRatio);
    const QPoint topLeft(available.center().x() - (fitted.width() / 2),
                         available.center().y() - (fitted.height() / 2));
    return QRect(topLeft, fitted);
}

QRect scaledPreviewCanvasRect(const QRect& baseRect,
                              qreal previewZoom,
                              const QPointF& previewPanOffset) {
    const QSize scaledSize(std::max(1, qRound(baseRect.width() * previewZoom)),
                           std::max(1, qRound(baseRect.height() * previewZoom)));
    const QPoint center = baseRect.center();
    return QRect(qRound(center.x() - (scaledSize.width() / 2.0) + previewPanOffset.x()),
                 qRound(center.y() - (scaledSize.height() / 2.0) + previewPanOffset.y()),
                 scaledSize.width(),
                 scaledSize.height());
}

QPointF previewCanvasScaleForTargetRect(const QRect& targetRect,
                                        const QSize& outputSize) {
    const QSize output = outputSize.isValid() ? outputSize : QSize(1080, 1920);
    return QPointF(targetRect.width() / qMax<qreal>(1.0, output.width()),
                   targetRect.height() / qMax<qreal>(1.0, output.height()));
}
