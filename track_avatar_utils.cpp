#include "track_avatar_utils.h"

#include <QPainter>
#include <QPainterPath>
#include <QPen>
#include <QRect>
#include <QRectF>

#include <cmath>

QImage renderTrackAvatarImage(const QImage& image,
                              const QJsonObject& keyframeObj,
                              int avatarSize)
{
    if (image.isNull() || image.width() <= 0 || image.height() <= 0) {
        return {};
    }

    const qreal locX = qBound<qreal>(0.0, keyframeObj.value(QStringLiteral("x")).toDouble(0.5), 1.0);
    const qreal locY = qBound<qreal>(0.0, keyframeObj.value(QStringLiteral("y")).toDouble(0.5), 1.0);
    const qreal boxSizeNorm = qBound<qreal>(
        -1.0,
        keyframeObj.value(QStringLiteral("box_size")).toDouble(
            keyframeObj.value(QStringLiteral("box")).toDouble(-1.0)),
        1.0);
    const qreal boxLeft = keyframeObj.value(QStringLiteral("box_left")).toDouble(-1.0);
    const qreal boxTop = keyframeObj.value(QStringLiteral("box_top")).toDouble(-1.0);
    const qreal boxRight = keyframeObj.value(QStringLiteral("box_right")).toDouble(-1.0);
    const qreal boxBottom = keyframeObj.value(QStringLiteral("box_bottom")).toDouble(-1.0);

    const int width = image.width();
    const int height = image.height();
    const int minSide = qMax(1, qMin(width, height));
    QRect cropRect;
    if (boxLeft >= 0.0 && boxTop >= 0.0 && boxRight > boxLeft && boxBottom > boxTop &&
        boxRight <= 1.0 && boxBottom <= 1.0) {
        const int left = qBound(0, static_cast<int>(std::floor(boxLeft * width)), qMax(0, width - 1));
        const int top = qBound(0, static_cast<int>(std::floor(boxTop * height)), qMax(0, height - 1));
        const int right = qBound(left + 1, static_cast<int>(std::ceil(boxRight * width)), width);
        const int bottom = qBound(top + 1, static_cast<int>(std::ceil(boxBottom * height)), height);
        cropRect = QRect(left, top, right - left, bottom - top);
    }
    if (!cropRect.isValid() || cropRect.isEmpty()) {
        int side = qMax(40, minSide / 3);
        if (boxSizeNorm > 0.0) {
            side = qBound(40, static_cast<int>(std::round(boxSizeNorm * minSide)), minSide);
        }
        const int cx = static_cast<int>(std::round(locX * static_cast<qreal>(width)));
        const int cy = static_cast<int>(std::round(locY * static_cast<qreal>(height)));
        const int left = qBound(0, cx - (side / 2), qMax(0, width - side));
        const int top = qBound(0, cy - (side / 2), qMax(0, height - side));
        cropRect = QRect(left, top, qMin(side, width - left), qMin(side, height - top));
    }

    QImage crop = image.copy(cropRect)
                      .scaled(avatarSize,
                              avatarSize,
                              Qt::KeepAspectRatioByExpanding,
                              Qt::SmoothTransformation)
                      .convertToFormat(QImage::Format_ARGB32_Premultiplied);
    if (crop.isNull()) {
        return {};
    }

    QImage rounded(avatarSize, avatarSize, QImage::Format_ARGB32_Premultiplied);
    rounded.fill(Qt::transparent);
    QPainter painter(&rounded);
    painter.setRenderHint(QPainter::Antialiasing, true);
    QPainterPath path;
    path.addRoundedRect(QRectF(1.0, 1.0, avatarSize - 2.0, avatarSize - 2.0), 8.0, 8.0);
    painter.setClipPath(path);
    painter.drawImage(QRect(0, 0, avatarSize, avatarSize), crop);
    painter.setClipping(false);
    painter.setPen(QPen(QColor(QStringLiteral("#f4d35e")), 1.5));
    painter.setBrush(Qt::NoBrush);
    painter.drawRoundedRect(QRectF(1.0, 1.0, avatarSize - 2.0, avatarSize - 2.0), 8.0, 8.0);
    return rounded;
}
