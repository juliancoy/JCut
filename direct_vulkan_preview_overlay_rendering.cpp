#include "direct_vulkan_preview_overlay_rendering.h"

#include <QCryptographicHash>
#include <QFont>
#include <QFontMetrics>
#include <QImage>
#include <QPaintDevice>
#include <QPainter>

namespace jcut::direct_vulkan_preview {
namespace {

void setFontPixelSizeRobust(QFont* font, qreal pixelSize, const QPaintDevice* device)
{
    if (!font) {
        return;
    }
    const qreal dpiY = (device && device->logicalDpiY() > 0) ? device->logicalDpiY() : 96.0;
    font->setPointSizeF((pixelSize * 72.0) / dpiY);
}

} // namespace

QString playbackStatusOverlayTextureKey(const QSize& imageSize, const QString& text, qreal progress)
{
    const QString keyMaterial =
        QString::number(imageSize.width()) + QLatin1Char('|') +
        QString::number(imageSize.height()) + QLatin1Char('|') +
        text.trimmed() + QLatin1Char('|') +
        QString::number(progress < 0.0 ? -1 : qRound(qBound<qreal>(0.0, progress, 1.0) * 1000.0));
    const QByteArray digest = QCryptographicHash::hash(keyMaterial.toUtf8(), QCryptographicHash::Sha1);
    return QString::fromLatin1(digest.toHex());
}

render_detail::OverlayImage renderPlaybackStatusOverlay(const QSize& imageSize, const QString& text, qreal progress)
{
    const QString normalized = text.trimmed();
    if (!imageSize.isValid() || normalized.isEmpty()) {
        return {};
    }
    const bool rubberBandGenerating =
        normalized.contains(QStringLiteral("Rubber Band"), Qt::CaseInsensitive) ||
        normalized.contains(QStringLiteral("Playback waiting"), Qt::CaseInsensitive);

    QImage image(qMax(1, imageSize.width()),
                 qMax(1, imageSize.height()),
                 QImage::Format_RGBA8888_Premultiplied);
    image.fill(Qt::transparent);

    QPainter painter(&image);
    painter.setRenderHint(QPainter::Antialiasing, true);
    painter.setRenderHint(QPainter::TextAntialiasing, true);

    QFont font = painter.font();
    font.setBold(true);
    setFontPixelSizeRobust(&font,
                           rubberBandGenerating
                               ? qBound<qreal>(24.0, image.height() * 0.034, 44.0)
                               : qBound<qreal>(18.0, image.height() * 0.026, 34.0),
                           painter.device());
    painter.setFont(font);

    const QFontMetrics metrics(font);
    const int maxWidth = qMax(80, image.width() - (rubberBandGenerating ? 28 : 48));
    const int horizontalPadding = rubberBandGenerating ? 68 : 44;
    const QString visibleText =
        metrics.elidedText(normalized, Qt::ElideRight, qMax(1, maxWidth - horizontalPadding));
    const bool showProgress = progress >= 0.0;
    const int badgeWidth = qMin(
        maxWidth,
        qMax(metrics.horizontalAdvance(visibleText) + horizontalPadding,
             showProgress ? (rubberBandGenerating ? 560 : 360) : 0));
    const int badgeHeight = qMax(rubberBandGenerating ? 88 : (showProgress ? 62 : 42),
                                 metrics.height() + (showProgress ? (rubberBandGenerating ? 48 : 34) : 18));
    const QRectF badgeRect((image.width() - badgeWidth) * 0.5,
                           qMax<qreal>(rubberBandGenerating ? 24.0 : 16.0, image.height() * 0.03),
                           badgeWidth,
                           badgeHeight);

    if (rubberBandGenerating) {
        const QRectF scrimRect = badgeRect.adjusted(-12.0, -12.0, 12.0, 12.0);
        painter.setPen(Qt::NoPen);
        painter.setBrush(QColor(0, 0, 0, 92));
        painter.drawRoundedRect(scrimRect, 10.0, 10.0);
    }

    painter.setPen(QPen(rubberBandGenerating ? QColor(255, 116, 64, 255) : QColor(255, 209, 102, 240),
                        rubberBandGenerating ? 3.0 : 2.0));
    painter.setBrush(rubberBandGenerating ? QColor(31, 13, 8, 238) : QColor(12, 16, 22, 226));
    painter.drawRoundedRect(badgeRect, 8.0, 8.0);
    if (rubberBandGenerating) {
        const QRectF accentRect(badgeRect.left(), badgeRect.top(), 10.0, badgeRect.height());
        painter.setPen(Qt::NoPen);
        painter.setBrush(QColor(255, 116, 64, 255));
        painter.drawRoundedRect(accentRect, 4.0, 4.0);
    }
    painter.setPen(rubberBandGenerating ? QColor(255, 250, 235, 255) : QColor(255, 244, 204, 255));
    painter.drawText(badgeRect.adjusted(16.0, 0.0, -16.0, 0.0),
                     Qt::AlignCenter,
                     visibleText);
    if (showProgress) {
        const QRectF trackRect = badgeRect.adjusted(20.0,
                                                    badgeRect.height() - (rubberBandGenerating ? 24.0 : 18.0),
                                                    -20.0,
                                                    rubberBandGenerating ? -10.0 : -9.0);
        const qreal normalizedProgress = qBound<qreal>(0.0, progress, 1.0);
        painter.setPen(Qt::NoPen);
        painter.setBrush(rubberBandGenerating ? QColor(255, 246, 230, 72) : QColor(255, 244, 204, 56));
        painter.drawRoundedRect(trackRect, rubberBandGenerating ? 5.0 : 3.0, rubberBandGenerating ? 5.0 : 3.0);
        QRectF fillRect = trackRect;
        fillRect.setWidth(qMax<qreal>(2.0, trackRect.width() * normalizedProgress));
        painter.setBrush(rubberBandGenerating ? QColor(255, 116, 64, 255) : QColor(255, 209, 102, 235));
        painter.drawRoundedRect(fillRect, rubberBandGenerating ? 5.0 : 3.0, rubberBandGenerating ? 5.0 : 3.0);
    }
    painter.end();

    render_detail::OverlayImage overlay;
    overlay.width = image.width();
    overlay.height = image.height();
    overlay.rgbaPremultiplied = QByteArray(
        reinterpret_cast<const char*>(image.constBits()),
        static_cast<int>(image.sizeInBytes()));
    return overlay;
}

} // namespace jcut::direct_vulkan_preview
