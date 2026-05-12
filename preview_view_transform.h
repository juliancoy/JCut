#pragma once

#include <QPointF>
#include <QRectF>
#include <QSize>
#include <QTransform>

class QWidget;
class QWindow;

enum class PreviewSurfaceCoordinateSpace {
    LogicalWidget,
    DeviceSurface,
};

enum class PreviewResizeAnchor {
    Center,
    Left,
    Top,
    TopLeft,
};

struct PreviewResizeHandles {
    QRectF right;
    QRectF bottom;
    QRectF corner;
};

struct PreviewClipGeometry {
    QTransform clipToScreen;
    QRectF localRect;
    QRectF bounds;
    QSizeF clipPixelSize;
};

struct PreviewZoomResult {
    bool changed = false;
    qreal zoom = 1.0;
    QPointF panOffset;
};

class PreviewViewTransform {
public:
    PreviewViewTransform(const QRectF& widgetRect,
                         const QSize& outputSize,
                         qreal marginPx,
                         qreal zoom,
                         const QPointF& panOffset);

    static QRectF baseRectForWidget(const QRectF& widgetRect,
                                    const QSize& outputSize,
                                    qreal marginPx);
    static QRectF rectForWidget(const QWidget* widget,
                                PreviewSurfaceCoordinateSpace coordinateSpace);
    static QPointF pointForWidgetPoint(const QWidget* widget,
                                       const QPointF& widgetPoint,
                                       PreviewSurfaceCoordinateSpace coordinateSpace);
    static QRectF rectForWindow(const QWindow* window,
                                PreviewSurfaceCoordinateSpace coordinateSpace);
    static QPointF pointForWindowPoint(const QWindow* window,
                                       const QPointF& windowPoint,
                                       PreviewSurfaceCoordinateSpace coordinateSpace);
    static QPointF clampedPanOffset(const QRectF& baseRect,
                                    qreal zoom,
                                    const QPointF& panOffset);
    static QPointF panForAnchoredZoom(const QRectF& baseRect,
                                      const QRectF& oldTargetRect,
                                      const QPointF& anchorScreenPoint,
                                      qreal nextZoom);
    static PreviewZoomResult zoomForWheel(const QRectF& surfaceRect,
                                          const QSize& outputSize,
                                          qreal marginPx,
                                          qreal currentZoom,
                                          const QPointF& currentPanOffset,
                                          const QPointF& anchorScreenPoint,
                                          int deltaY,
                                          qreal minZoom = 0.1,
                                          qreal maxZoom = 20.0,
                                          qreal wheelFactor = 1.1);
    static QRectF scaledRect(const QRectF& baseRect,
                             qreal zoom,
                             const QPointF& panOffset);
    static QPointF scaleForTargetRect(const QRectF& targetRect,
                                      const QSize& outputSize);
    static QRectF fitRectToBounds(const QSize& source,
                                  const QRectF& bounds);
    static QSize clipLayoutSize(const QSize& sourceSize,
                                const QSize& payloadSize,
                                const QSize& outputSize);
    static QRectF fittedClipRect(const QSize& sourceSize,
                                 const QSize& payloadSize,
                                 const QRectF& targetRect,
                                 const QSize& outputSize);
    static PreviewClipGeometry clipGeometry(const QRectF& fitted,
                                            const QPointF& previewScale,
                                            const QPointF& translation,
                                            qreal rotation,
                                            const QPointF& scale);
    static QRectF localRectForNormalizedRect(const QRectF& normalizedRect,
                                             const QRectF& localRect);
    static QPointF localPointForNormalizedPoint(const QPointF& normalizedPoint,
                                                const QRectF& localRect);
    static PreviewResizeHandles resizeHandlesForBounds(const QRectF& bounds,
                                                       qreal handleSize = 12.0);
    static QPointF screenShiftForAnchoredResize(const QRectF& originBounds,
                                                PreviewResizeAnchor anchor,
                                                qreal scaleXFactor,
                                                qreal scaleYFactor);
    static QPointF translationForAnchoredResize(const QPointF& originTranslation,
                                                const QPointF& originScale,
                                                const QPointF& nextScale,
                                                const QRectF& originBounds,
                                                PreviewResizeAnchor anchor,
                                                const QPointF& previewScale);

    QRectF baseRect() const { return m_baseRect; }
    QRectF targetRect() const { return m_targetRect; }
    QPointF clampedPan() const { return m_clampedPan; }
    QPointF outputScale() const;

    QPointF outputToScreen(const QPointF& outputPoint) const;
    QPointF screenToOutput(const QPointF& screenPoint) const;
    QRectF outputRectToScreen(const QRectF& outputRect) const;
    QRectF fittedClipRect(const QSize& payloadSize) const;
    QRectF fittedClipRect(const QSize& sourceSize,
                          const QSize& payloadSize) const;

private:
    QSize m_outputSize;
    QRectF m_baseRect;
    QPointF m_clampedPan;
    QRectF m_targetRect;
};
