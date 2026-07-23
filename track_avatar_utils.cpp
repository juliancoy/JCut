#include "track_avatar_utils.h"
#include "face_avatar_crop_core.h"

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

    const jcut::FaceAvatarCropRectCore cropGeometry =
        jcut::faceAvatarCropRectCore(
            image.width(),
            image.height(),
            locX,
            locY,
            boxSizeNorm,
            boxLeft,
            boxTop,
            boxRight,
            boxBottom);
    const QRect cropRect(
        cropGeometry.left,
        cropGeometry.top,
        cropGeometry.width,
        cropGeometry.height);

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
