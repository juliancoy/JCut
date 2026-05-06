#include "transport_icons.h"

#include <QFile>
#include <QPainter>
#include <QPainterPath>
#include <QSet>
#include <QDebug>

namespace editor {
namespace {

QString iconResourcePath(TransportIconGlyph glyph)
{
    switch (glyph) {
    case TransportIconGlyph::Play: return QStringLiteral(":/icons/transport/assets/icons/transport/play.svg");
    case TransportIconGlyph::Pause: return QStringLiteral(":/icons/transport/assets/icons/transport/pause.svg");
    case TransportIconGlyph::StepBack: return QStringLiteral(":/icons/transport/assets/icons/transport/step_back.svg");
    case TransportIconGlyph::StepForward: return QStringLiteral(":/icons/transport/assets/icons/transport/step_forward.svg");
    case TransportIconGlyph::ToStart: return QStringLiteral(":/icons/transport/assets/icons/transport/to_start.svg");
    case TransportIconGlyph::ToEnd: return QStringLiteral(":/icons/transport/assets/icons/transport/to_end.svg");
    case TransportIconGlyph::Volume: return QStringLiteral(":/icons/transport/assets/icons/transport/volume.svg");
    case TransportIconGlyph::VolumeMuted: return QStringLiteral(":/icons/transport/assets/icons/transport/volume_muted.svg");
    }
    return QString();
}

QPixmap paintFallbackIcon(TransportIconGlyph glyph, const QColor& color, const QSize& size)
{
    QPixmap px(size);
    px.fill(Qt::transparent);
    QPainter p(&px);
    p.setRenderHint(QPainter::Antialiasing, true);
    const qreal w = static_cast<qreal>(size.width());
    const qreal h = static_cast<qreal>(size.height());
    const qreal stroke = qMax<qreal>(1.6, qMin(w, h) * 0.11);
    p.setPen(QPen(color, stroke, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
    p.setBrush(color);

    auto tri = [](qreal x, qreal y, qreal tw, qreal th, bool right) {
        QPainterPath path;
        if (right) {
            path.moveTo(x, y);
            path.lineTo(x + tw, y + th * 0.5);
            path.lineTo(x, y + th);
        } else {
            path.moveTo(x + tw, y);
            path.lineTo(x, y + th * 0.5);
            path.lineTo(x + tw, y + th);
        }
        path.closeSubpath();
        return path;
    };

    switch (glyph) {
    case TransportIconGlyph::Play:
        p.drawPath(tri(w * 0.32, h * 0.18, w * 0.42, h * 0.64, true));
        break;
    case TransportIconGlyph::Pause:
        p.drawRoundedRect(QRectF(w * 0.29, h * 0.18, w * 0.17, h * 0.64), 1.2, 1.2);
        p.drawRoundedRect(QRectF(w * 0.54, h * 0.18, w * 0.17, h * 0.64), 1.2, 1.2);
        break;
    case TransportIconGlyph::StepBack:
        p.drawPath(tri(w * 0.24, h * 0.20, w * 0.30, h * 0.60, false));
        p.drawLine(QPointF(w * 0.72, h * 0.18), QPointF(w * 0.72, h * 0.82));
        break;
    case TransportIconGlyph::StepForward:
        p.drawPath(tri(w * 0.46, h * 0.20, w * 0.30, h * 0.60, true));
        p.drawLine(QPointF(w * 0.28, h * 0.18), QPointF(w * 0.28, h * 0.82));
        break;
    case TransportIconGlyph::ToStart:
        p.drawPath(tri(w * 0.36, h * 0.18, w * 0.24, h * 0.64, false));
        p.drawPath(tri(w * 0.60, h * 0.18, w * 0.24, h * 0.64, false));
        p.drawLine(QPointF(w * 0.20, h * 0.16), QPointF(w * 0.20, h * 0.84));
        break;
    case TransportIconGlyph::ToEnd:
        p.drawPath(tri(w * 0.16, h * 0.18, w * 0.24, h * 0.64, true));
        p.drawPath(tri(w * 0.40, h * 0.18, w * 0.24, h * 0.64, true));
        p.drawLine(QPointF(w * 0.80, h * 0.16), QPointF(w * 0.80, h * 0.84));
        break;
    case TransportIconGlyph::Volume:
    case TransportIconGlyph::VolumeMuted: {
        p.setPen(Qt::NoPen);
        p.drawRect(QRectF(w * 0.14, h * 0.34, w * 0.16, h * 0.32));
        QPainterPath horn;
        horn.moveTo(w * 0.30, h * 0.34);
        horn.lineTo(w * 0.56, h * 0.18);
        horn.lineTo(w * 0.56, h * 0.82);
        horn.lineTo(w * 0.30, h * 0.66);
        horn.closeSubpath();
        p.drawPath(horn);
        p.setPen(QPen(color, stroke, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
        if (glyph == TransportIconGlyph::VolumeMuted) {
            p.drawLine(QPointF(w * 0.68, h * 0.30), QPointF(w * 0.90, h * 0.70));
            p.drawLine(QPointF(w * 0.90, h * 0.30), QPointF(w * 0.68, h * 0.70));
        } else {
            p.drawArc(QRectF(w * 0.58, h * 0.30, w * 0.22, h * 0.40), -40 * 16, 80 * 16);
            p.drawArc(QRectF(w * 0.64, h * 0.20, w * 0.28, h * 0.60), -40 * 16, 80 * 16);
        }
        break;
    }
    }

    return px;
}

QIcon themedStateIcon(TransportIconGlyph glyph, const QSize& size)
{
    QIcon icon;
    icon.addPixmap(paintFallbackIcon(glyph, QColor(QStringLiteral("#d8e4ef")), size), QIcon::Normal, QIcon::Off);
    icon.addPixmap(paintFallbackIcon(glyph, QColor(QStringLiteral("#f5fbff")), size), QIcon::Active, QIcon::Off);
    icon.addPixmap(paintFallbackIcon(glyph, QColor(QStringLiteral("#9ee5ff")), size), QIcon::Selected, QIcon::Off);
    icon.addPixmap(paintFallbackIcon(glyph, QColor(QStringLiteral("#6f8296")), size), QIcon::Disabled, QIcon::Off);
    icon.addPixmap(paintFallbackIcon(glyph, QColor(QStringLiteral("#9ee5ff")), size), QIcon::Normal, QIcon::On);
    icon.addPixmap(paintFallbackIcon(glyph, QColor(QStringLiteral("#c8f3ff")), size), QIcon::Active, QIcon::On);
    icon.addPixmap(paintFallbackIcon(glyph, QColor(QStringLiteral("#6f8296")), size), QIcon::Disabled, QIcon::On);
    return icon;
}

} // namespace

QIcon transportIcon(TransportIconGlyph glyph, const QSize& size)
{
    return themedStateIcon(glyph, size);
}

QIcon playPauseTransportIcon(bool playing, const QSize& size)
{
    return themedStateIcon(playing ? TransportIconGlyph::Pause : TransportIconGlyph::Play, size);
}

QIcon volumeTransportIcon(bool muted, const QSize& size)
{
    return themedStateIcon(muted ? TransportIconGlyph::VolumeMuted : TransportIconGlyph::Volume, size);
}

void validateTransportIconResources()
{
    static bool checked = false;
    if (checked) {
        return;
    }
    checked = true;

    const QList<TransportIconGlyph> glyphs = {
        TransportIconGlyph::Play,
        TransportIconGlyph::Pause,
        TransportIconGlyph::StepBack,
        TransportIconGlyph::StepForward,
        TransportIconGlyph::ToStart,
        TransportIconGlyph::ToEnd,
        TransportIconGlyph::Volume,
        TransportIconGlyph::VolumeMuted,
    };

    QSet<QString> missing;
    for (TransportIconGlyph glyph : glyphs) {
        const QString path = iconResourcePath(glyph);
        if (!path.isEmpty() && !QFile::exists(path)) {
            missing.insert(path);
        }
    }
    if (!missing.isEmpty()) {
        qWarning().noquote() << QStringLiteral("[icons] transport resource(s) missing (%1); using built-in fallback glyphs")
                                    .arg(missing.size());
    }
}

} // namespace editor
