#include "preview_view_transform.h"

#include <Qt>
#include <QWidget>

QRectF PreviewViewTransform::rectForWidget(const QWidget* widget,
                                           PreviewSurfaceCoordinateSpace coordinateSpace)
{
    if (!widget) {
        return QRectF();
    }

    const qreal scale = coordinateSpace == PreviewSurfaceCoordinateSpace::DeviceSurface
        ? qMax<qreal>(0.0001, widget->devicePixelRatioF())
        : 1.0;
    return QRectF(QPointF(0.0, 0.0),
                  QSizeF(widget->width() * scale, widget->height() * scale));
}

QPointF PreviewViewTransform::pointForWidgetPoint(
    const QWidget* widget,
    const QPointF& widgetPoint,
    PreviewSurfaceCoordinateSpace coordinateSpace)
{
    if (!widget || coordinateSpace == PreviewSurfaceCoordinateSpace::LogicalWidget) {
        return widgetPoint;
    }

    const qreal scale = qMax<qreal>(0.0001, widget->devicePixelRatioF());
    return QPointF(widgetPoint.x() * scale, widgetPoint.y() * scale);
}
