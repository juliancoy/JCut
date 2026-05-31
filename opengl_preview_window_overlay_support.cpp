#include "opengl_preview.h"
#include "audio_preview_support.h"
#include "waveform_service.h"

#include <QApplication>
#include <QFontMetrics>
#include <QPainter>

void PreviewWindow::drawAudioBadge(QPainter* painter, const QRect& targetRect,
                    const QList<TimelineClip>& activeAudioClips) {
    Q_UNUSED(painter)
    Q_UNUSED(targetRect)
    Q_UNUSED(activeAudioClips)
}

void PreviewWindow::drawSpeakerPickOverlay(QPainter* painter) const {
    const bool speakerPickReady =
        !m_interaction.transient.speakerPickDragActive &&
        (QApplication::keyboardModifiers() & Qt::ShiftModifier) &&
        (speakerPointRequested || speakerBoxRequested) &&
        !m_interaction.transient.speakerPickHintClipId.isEmpty();
    const QString clipId = m_interaction.transient.speakerPickDragActive ? m_interaction.transient.speakerPickClipId : m_interaction.transient.speakerPickHintClipId;
    if (clipId.isEmpty()) {
        return;
    }
    const PreviewOverlayInfo info = m_overlayModel.overlays.value(clipId);
    if (!info.bounds.isValid() || info.bounds.width() <= 1.0 || info.bounds.height() <= 1.0) {
        return;
    }

    const QPointF start = m_interaction.transient.speakerPickDragActive ? m_interaction.transient.speakerPickStartPos : m_interaction.transient.speakerPickCurrentPos;
    const QPointF current = m_interaction.transient.speakerPickDragActive
        ? (m_interaction.transient.speakerPickCurrentPos.isNull() ? start : m_interaction.transient.speakerPickCurrentPos)
        : m_interaction.transient.speakerPickCurrentPos;
    const QPointF startNorm = mapScreenPointToNormalizedClip(info, start);
    const QPointF currentNorm = mapScreenPointToNormalizedClip(info, current);
    const qreal dx = currentNorm.x() - startNorm.x();
    const qreal dy = currentNorm.y() - startNorm.y();
    const qreal dragDistance = std::sqrt((dx * dx) + (dy * dy));

    painter->save();
    painter->setRenderHint(QPainter::Antialiasing, true);

    const QRectF clipBounds = info.bounds.adjusted(0.5, 0.5, -0.5, -0.5);
    painter->setPen(QPen(QColor(72, 190, 255, 220), 1.5, Qt::DashLine));
    painter->setBrush(Qt::NoBrush);
    painter->drawRect(clipBounds);

    const QPointF centerPx = mapNormalizedClipPointToScreen(info, currentNorm);
    painter->setPen(QPen(QColor(72, 190, 255, 220), 1.5));
    painter->drawLine(QPointF(centerPx.x() - 8.0, centerPx.y()),
                      QPointF(centerPx.x() + 8.0, centerPx.y()));
    painter->drawLine(QPointF(centerPx.x(), centerPx.y() - 8.0),
                      QPointF(centerPx.x(), centerPx.y() + 8.0));

    QString hintText = QStringLiteral("Speaker pick ready: Shift+Drag square");
    if (m_interaction.transient.speakerPickDragActive && dragDistance >= 0.01) {
        const qreal side = qBound<qreal>(0.02, qMax(std::abs(dx), std::abs(dy)), 1.0);
        const qreal cx = qBound<qreal>(0.0, (startNorm.x() + currentNorm.x()) * 0.5, 1.0);
        const qreal cy = qBound<qreal>(0.0, (startNorm.y() + currentNorm.y()) * 0.5, 1.0);
        const QPointF center = mapNormalizedClipPointToScreen(info, QPointF(cx, cy));
        const qreal sidePx = side * qMin(info.bounds.width(), info.bounds.height());
        QRectF square(center.x() - (sidePx * 0.5),
                      center.y() - (sidePx * 0.5),
                      sidePx,
                      sidePx);
        square = square.intersected(info.bounds);

        painter->setPen(QPen(QColor(72, 190, 255, 240), 2.0));
        painter->setBrush(QColor(72, 190, 255, 52));
        painter->drawRect(square);
        hintText = QStringLiteral("Release: set head box");
    } else if (m_interaction.transient.speakerPickDragActive) {
        painter->setPen(QPen(QColor(72, 190, 255, 230), 2.0));
        painter->setBrush(QColor(72, 190, 255, 80));
        painter->drawEllipse(centerPx, 5.0, 5.0);
        hintText = QStringLiteral("Release: set square box");
    } else if (speakerPickReady) {
        painter->setPen(QPen(QColor(72, 190, 255, 210), 2.0, Qt::DashLine));
        painter->setBrush(Qt::NoBrush);
        painter->drawEllipse(centerPx, 8.0, 8.0);
    }

    const QRectF badgeRect(info.bounds.left() + 10.0, info.bounds.top() + 10.0, 250.0, 26.0);
    painter->setPen(Qt::NoPen);
    painter->setBrush(QColor(10, 15, 21, 196));
    painter->drawRoundedRect(badgeRect, 8.0, 8.0);
    painter->setPen(QColor(224, 244, 255, 245));
    painter->drawText(badgeRect.adjusted(10.0, 0.0, -8.0, 0.0),
                      Qt::AlignLeft | Qt::AlignVCenter,
                      hintText);

    painter->restore();
}

void PreviewWindow::drawPlaybackStatusOverlay(QPainter* painter, const QRect& bounds) const {
    const QString text = m_interaction.playbackStatusOverlayText.trimmed();
    if (!painter || text.isEmpty() || bounds.isEmpty()) {
        return;
    }

    painter->save();
    painter->setRenderHint(QPainter::Antialiasing, true);
    painter->setRenderHint(QPainter::TextAntialiasing, true);

    QFont font = painter->font();
    font.setBold(true);
    font.setPointSize(qMax(11, font.pointSize() + 2));
    painter->setFont(font);

    const QFontMetrics metrics(font);
    const int maxWidth = qMax(64, bounds.width() - 40);
    const QString visibleText = metrics.elidedText(text, Qt::ElideRight, qMax(1, maxWidth - 36));
    const bool showProgress = m_interaction.playbackStatusOverlayProgress >= 0.0;
    const int badgeWidth = qMin(maxWidth, qMax(metrics.horizontalAdvance(visibleText) + 36, showProgress ? 320 : 0));
    const int badgeHeight = qMax(showProgress ? 54 : 36, metrics.height() + (showProgress ? 30 : 18));
    const QRectF badgeRect(bounds.center().x() - badgeWidth / 2.0,
                           bounds.top() + 18.0,
                           badgeWidth,
                           badgeHeight);

    painter->setPen(QPen(QColor(255, 209, 102, 240), 2.0));
    painter->setBrush(QColor(12, 16, 22, 226));
    painter->drawRoundedRect(badgeRect, 8.0, 8.0);
    painter->setPen(QColor(255, 244, 204, 255));
    painter->drawText(badgeRect.adjusted(14.0, 0.0, -14.0, 0.0),
                      Qt::AlignCenter,
                      visibleText);
    if (showProgress) {
        const QRectF trackRect = badgeRect.adjusted(18.0, badgeRect.height() - 16.0, -18.0, -8.0);
        const qreal progress = qBound<qreal>(0.0, m_interaction.playbackStatusOverlayProgress, 1.0);
        painter->setPen(Qt::NoPen);
        painter->setBrush(QColor(255, 244, 204, 56));
        painter->drawRoundedRect(trackRect, 3.0, 3.0);
        QRectF fillRect = trackRect;
        fillRect.setWidth(qMax<qreal>(2.0, trackRect.width() * progress));
        painter->setBrush(QColor(255, 209, 102, 235));
        painter->drawRoundedRect(fillRect, 3.0, 3.0);
    }
    painter->restore();
}

QRect PreviewWindow::fitRect(const QSize& source, const QRect& bounds) const {
    return previewFitRectToBounds(source, bounds);
}
void PreviewWindow::drawPreviewChrome(QPainter* painter, const QRect& safeRect, int activeClipCount) const {
    Q_UNUSED(painter)
    Q_UNUSED(safeRect)
    Q_UNUSED(activeClipCount)
}
