#include "preview_view_transform.h"

#include <Qt>
#include <QWindow>

#include <algorithm>

namespace {
QSize safeOutputSize(const QSize& outputSize)
{
    return outputSize.isValid() ? outputSize : QSize(1080, 1920);
}
}

PreviewViewTransform::PreviewViewTransform(const QRectF& widgetRect,
                                           const QSize& outputSize,
                                           qreal marginPx,
                                           qreal zoom,
                                           const QPointF& panOffset)
    : m_outputSize(safeOutputSize(outputSize))
    , m_baseRect(baseRectForWidget(widgetRect, m_outputSize, marginPx))
    , m_clampedPan(clampedPanOffset(m_baseRect, zoom, panOffset))
    , m_targetRect(scaledRect(m_baseRect, zoom, m_clampedPan))
{
}

QRectF PreviewViewTransform::baseRectForWidget(const QRectF& widgetRect,
                                               const QSize& outputSize,
                                               qreal marginPx)
{
    const QRectF available = widgetRect.adjusted(marginPx, marginPx, -marginPx, -marginPx);
    if (!available.isValid()) {
        return available;
    }
    const QSize output = safeOutputSize(outputSize);
    const qreal scale = std::min(available.width() / qMax<qreal>(1.0, output.width()),
                                 available.height() / qMax<qreal>(1.0, output.height()));
    const QSizeF fitted(qMax<qreal>(1.0, output.width() * scale),
                        qMax<qreal>(1.0, output.height() * scale));
    return QRectF(available.x() + ((available.width() - fitted.width()) / 2.0),
                  available.y() + ((available.height() - fitted.height()) / 2.0),
                  fitted.width(),
                  fitted.height());
}

QRectF PreviewViewTransform::rectForWindow(const QWindow* window,
                                           PreviewSurfaceCoordinateSpace coordinateSpace)
{
    if (!window) {
        return QRectF();
    }

    const qreal scale = coordinateSpace == PreviewSurfaceCoordinateSpace::DeviceSurface
        ? qMax<qreal>(0.0001, window->devicePixelRatio())
        : 1.0;
    return QRectF(QPointF(0.0, 0.0),
                  QSizeF(window->width() * scale, window->height() * scale));
}

QPointF PreviewViewTransform::pointForWindowPoint(const QWindow* window,
                                                  const QPointF& windowPoint,
                                                  PreviewSurfaceCoordinateSpace coordinateSpace)
{
    if (!window || coordinateSpace == PreviewSurfaceCoordinateSpace::LogicalWidget) {
        return windowPoint;
    }

    const qreal scale = qMax<qreal>(0.0001, window->devicePixelRatio());
    return QPointF(windowPoint.x() * scale, windowPoint.y() * scale);
}

QPointF PreviewViewTransform::clampedPanOffset(const QRectF& baseRect,
                                               qreal zoom,
                                               const QPointF& panOffset)
{
    if (baseRect.isEmpty()) {
        return QPointF();
    }
    const qreal scaledW = qMax<qreal>(1.0, baseRect.width() * zoom);
    const qreal scaledH = qMax<qreal>(1.0, baseRect.height() * zoom);
    const qreal maxX = std::abs(scaledW - baseRect.width()) / 2.0;
    const qreal maxY = std::abs(scaledH - baseRect.height()) / 2.0;
    return QPointF(qBound<qreal>(-maxX, panOffset.x(), maxX),
                   qBound<qreal>(-maxY, panOffset.y(), maxY));
}

QPointF PreviewViewTransform::panForAnchoredZoom(const QRectF& baseRect,
                                                 const QRectF& oldTargetRect,
                                                 const QPointF& anchorScreenPoint,
                                                 qreal nextZoom)
{
    if (baseRect.isEmpty() || oldTargetRect.isEmpty()) {
        return QPointF();
    }
    const QPointF anchorNorm(
        (anchorScreenPoint.x() - oldTargetRect.left()) / qMax<qreal>(1.0, oldTargetRect.width()),
        (anchorScreenPoint.y() - oldTargetRect.top()) / qMax<qreal>(1.0, oldTargetRect.height()));
    const QSizeF newSize(qMax<qreal>(1.0, baseRect.width() * nextZoom),
                         qMax<qreal>(1.0, baseRect.height() * nextZoom));
    const QPointF centeredTopLeft(baseRect.x() + ((baseRect.width() - newSize.width()) / 2.0),
                                  baseRect.y() + ((baseRect.height() - newSize.height()) / 2.0));
    const QPointF anchoredTopLeft(anchorScreenPoint.x() - (anchorNorm.x() * newSize.width()),
                                  anchorScreenPoint.y() - (anchorNorm.y() * newSize.height()));
    return clampedPanOffset(baseRect, nextZoom, anchoredTopLeft - centeredTopLeft);
}

PreviewZoomResult PreviewViewTransform::zoomForWheel(const QRectF& surfaceRect,
                                                     const QSize& outputSize,
                                                     qreal marginPx,
                                                     qreal currentZoom,
                                                     const QPointF& currentPanOffset,
                                                     const QPointF& anchorScreenPoint,
                                                     int deltaY,
                                                     qreal minZoom,
                                                     qreal maxZoom,
                                                     qreal wheelFactor)
{
    PreviewZoomResult result;
    result.zoom = qBound<qreal>(minZoom, currentZoom, maxZoom);
    result.panOffset = currentPanOffset;
    if (deltaY == 0 || surfaceRect.isEmpty()) {
        return result;
    }

    const QRectF baseRect = baseRectForWidget(surfaceRect, outputSize, marginPx);
    const QRectF oldRect = scaledRect(baseRect, result.zoom, currentPanOffset);
    const qreal factor = deltaY > 0 ? wheelFactor : (1.0 / qMax<qreal>(0.0001, wheelFactor));
    const qreal nextZoom = qBound<qreal>(minZoom, result.zoom * factor, maxZoom);
    if (qAbs(nextZoom - result.zoom) < 0.000001) {
        return result;
    }

    result.changed = true;
    result.zoom = nextZoom;
    result.panOffset = panForAnchoredZoom(baseRect, oldRect, anchorScreenPoint, nextZoom);
    return result;
}

QRectF PreviewViewTransform::scaledRect(const QRectF& baseRect,
                                        qreal zoom,
                                        const QPointF& panOffset)
{
    const QPointF clamped = clampedPanOffset(baseRect, zoom, panOffset);
    const QSizeF scaledSize(qMax<qreal>(1.0, baseRect.width() * zoom),
                            qMax<qreal>(1.0, baseRect.height() * zoom));
    return QRectF(baseRect.x() + ((baseRect.width() - scaledSize.width()) / 2.0) + clamped.x(),
                  baseRect.y() + ((baseRect.height() - scaledSize.height()) / 2.0) + clamped.y(),
                  scaledSize.width(),
                  scaledSize.height());
}

QPointF PreviewViewTransform::scaleForTargetRect(const QRectF& targetRect,
                                                 const QSize& outputSize)
{
    const QSize output = safeOutputSize(outputSize);
    return QPointF(targetRect.width() / qMax<qreal>(1.0, output.width()),
                   targetRect.height() / qMax<qreal>(1.0, output.height()));
}

QRectF PreviewViewTransform::fitRectToBounds(const QSize& source,
                                             const QRectF& bounds)
{
    if (source.isEmpty() || bounds.isEmpty()) {
        return bounds;
    }
    const qreal scale = std::min(bounds.width() / qMax<qreal>(1.0, source.width()),
                                 bounds.height() / qMax<qreal>(1.0, source.height()));
    const QSizeF scaled(qMax<qreal>(1.0, source.width() * scale),
                        qMax<qreal>(1.0, source.height() * scale));
    return QRectF(bounds.x() + ((bounds.width() - scaled.width()) / 2.0),
                  bounds.y() + ((bounds.height() - scaled.height()) / 2.0),
                  scaled.width(),
                  scaled.height());
}

QSize PreviewViewTransform::clipLayoutSize(const QSize& sourceSize,
                                           const QSize& payloadSize,
                                           const QSize& outputSize)
{
    if (!sourceSize.isEmpty()) {
        return sourceSize;
    }
    if (!payloadSize.isEmpty()) {
        return payloadSize;
    }
    return safeOutputSize(outputSize);
}

QRectF PreviewViewTransform::fittedClipRect(const QSize& sourceSize,
                                            const QSize& payloadSize,
                                            const QRectF& targetRect,
                                            const QSize& outputSize)
{
    return fitRectToBounds(clipLayoutSize(sourceSize, payloadSize, outputSize), targetRect);
}

PreviewClipGeometry PreviewViewTransform::clipGeometry(const QRectF& fitted,
                                                       const QPointF& previewScale,
                                                       const QPointF& translation,
                                                       qreal rotation,
                                                       const QPointF& scale)
{
    PreviewClipGeometry geometry;
    geometry.localRect = QRectF(-fitted.width() / 2.0,
                                -fitted.height() / 2.0,
                                fitted.width(),
                                fitted.height());
    geometry.clipPixelSize = QSizeF(fitted.width(), fitted.height());
    geometry.clipToScreen.translate(fitted.center().x() + (translation.x() * previewScale.x()),
                                    fitted.center().y() + (translation.y() * previewScale.y()));
    geometry.clipToScreen.rotate(rotation);
    geometry.clipToScreen.scale(scale.x(), scale.y());
    geometry.bounds = geometry.clipToScreen.mapRect(geometry.localRect);
    return geometry;
}

QRectF PreviewViewTransform::localRectForNormalizedRect(const QRectF& normalizedRect,
                                                        const QRectF& localRect)
{
    const qreal localW = qMax<qreal>(1.0, localRect.width());
    const qreal localH = qMax<qreal>(1.0, localRect.height());
    return QRectF(localRect.left() + (qBound<qreal>(0.0, normalizedRect.left(), 1.0) * localW),
                  localRect.top() + (qBound<qreal>(0.0, normalizedRect.top(), 1.0) * localH),
                  qBound<qreal>(0.0, normalizedRect.width(), 1.0) * localW,
                  qBound<qreal>(0.0, normalizedRect.height(), 1.0) * localH);
}

QPointF PreviewViewTransform::localPointForNormalizedPoint(const QPointF& normalizedPoint,
                                                           const QRectF& localRect)
{
    const qreal localW = qMax<qreal>(1.0, localRect.width());
    const qreal localH = qMax<qreal>(1.0, localRect.height());
    return QPointF(localRect.left() + (qBound<qreal>(0.0, normalizedPoint.x(), 1.0) * localW),
                   localRect.top() + (qBound<qreal>(0.0, normalizedPoint.y(), 1.0) * localH));
}

PreviewResizeHandles PreviewViewTransform::resizeHandlesForBounds(const QRectF& bounds,
                                                                   qreal handleSize)
{
    PreviewResizeHandles handles;
    handles.right = QRectF(bounds.right() - handleSize,
                           bounds.center().y() - handleSize,
                           handleSize,
                           handleSize * 2.0);
    handles.bottom = QRectF(bounds.center().x() - handleSize,
                            bounds.bottom() - handleSize,
                            handleSize * 2.0,
                            handleSize);
    handles.corner = QRectF(bounds.right() - handleSize * 1.5,
                            bounds.bottom() - handleSize * 1.5,
                            handleSize * 1.5,
                            handleSize * 1.5);
    return handles;
}

QPointF PreviewViewTransform::screenShiftForAnchoredResize(const QRectF& originBounds,
                                                           PreviewResizeAnchor anchor,
                                                           qreal scaleXFactor,
                                                           qreal scaleYFactor)
{
    if (originBounds.width() <= 1.0 || originBounds.height() <= 1.0) {
        return QPointF();
    }

    QPointF anchorPoint = originBounds.center();
    QPointF resizedAnchorPoint = originBounds.center();
    const qreal nextHalfWidth = originBounds.width() * scaleXFactor * 0.5;
    const qreal nextHalfHeight = originBounds.height() * scaleYFactor * 0.5;

    if (anchor == PreviewResizeAnchor::Left || anchor == PreviewResizeAnchor::TopLeft) {
        anchorPoint.setX(originBounds.left());
        resizedAnchorPoint.setX(originBounds.center().x() - nextHalfWidth);
    }
    if (anchor == PreviewResizeAnchor::Top || anchor == PreviewResizeAnchor::TopLeft) {
        anchorPoint.setY(originBounds.top());
        resizedAnchorPoint.setY(originBounds.center().y() - nextHalfHeight);
    }

    return anchorPoint - resizedAnchorPoint;
}

QPointF PreviewViewTransform::translationForAnchoredResize(const QPointF& originTranslation,
                                                           const QPointF& originScale,
                                                           const QPointF& nextScale,
                                                           const QRectF& originBounds,
                                                           PreviewResizeAnchor anchor,
                                                           const QPointF& previewScale)
{
    const auto safeScale = [](qreal value) {
        if (std::abs(value) < 0.0001) {
            return value < 0.0 ? -0.0001 : 0.0001;
        }
        return value;
    };
    const qreal scaleXFactor = nextScale.x() / safeScale(originScale.x());
    const qreal scaleYFactor = nextScale.y() / safeScale(originScale.y());
    const QPointF screenShift =
        screenShiftForAnchoredResize(originBounds, anchor, scaleXFactor, scaleYFactor);
    return QPointF(originTranslation.x() + (screenShift.x() / qMax<qreal>(0.0001, previewScale.x())),
                   originTranslation.y() + (screenShift.y() / qMax<qreal>(0.0001, previewScale.y())));
}

QPointF PreviewViewTransform::outputScale() const
{
    return scaleForTargetRect(m_targetRect, m_outputSize);
}

QPointF PreviewViewTransform::outputToScreen(const QPointF& outputPoint) const
{
    const QPointF scale = outputScale();
    const QPointF outputCenter(m_outputSize.width() / 2.0, m_outputSize.height() / 2.0);
    return QPointF(m_targetRect.center().x() + ((outputPoint.x() - outputCenter.x()) * scale.x()),
                   m_targetRect.center().y() + ((outputPoint.y() - outputCenter.y()) * scale.y()));
}

QPointF PreviewViewTransform::screenToOutput(const QPointF& screenPoint) const
{
    const QPointF scale = outputScale();
    const QPointF outputCenter(m_outputSize.width() / 2.0, m_outputSize.height() / 2.0);
    return QPointF(outputCenter.x() + ((screenPoint.x() - m_targetRect.center().x()) / qMax<qreal>(0.0001, scale.x())),
                   outputCenter.y() + ((screenPoint.y() - m_targetRect.center().y()) / qMax<qreal>(0.0001, scale.y())));
}

QRectF PreviewViewTransform::outputRectToScreen(const QRectF& outputRect) const
{
    const QPointF topLeft = outputToScreen(outputRect.topLeft());
    const QPointF bottomRight = outputToScreen(outputRect.bottomRight());
    return QRectF(topLeft, bottomRight).normalized();
}

QRectF PreviewViewTransform::fittedClipRect(const QSize& payloadSize) const
{
    return fittedClipRect(QSize(), payloadSize, m_targetRect, m_outputSize);
}

QRectF PreviewViewTransform::fittedClipRect(const QSize& sourceSize,
                                            const QSize& payloadSize) const
{
    return fittedClipRect(sourceSize, payloadSize, m_targetRect, m_outputSize);
}
