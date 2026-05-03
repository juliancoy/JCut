#include "opengl_preview.h"
#include "waveform_service.h"

#include <QApplication>
#include <QPainter>

QString PreviewWindow::audioDynamicsCacheKey() const {
    return QStringLiteral("%1|%2|%3|%4|%5|%6|%7|%8|%9|%10")
        .arg(m_audioDynamics.amplifyEnabled ? 1 : 0)
        .arg(m_audioDynamics.amplifyDb, 0, 'f', 2)
        .arg(m_audioDynamics.normalizeEnabled ? 1 : 0)
        .arg(m_audioDynamics.normalizeTargetDb, 0, 'f', 2)
        .arg(m_audioDynamics.peakReductionEnabled ? 1 : 0)
        .arg(m_audioDynamics.peakThresholdDb, 0, 'f', 2)
        .arg(m_audioDynamics.limiterEnabled ? 1 : 0)
        .arg(m_audioDynamics.limiterThresholdDb, 0, 'f', 2)
        .arg(m_audioDynamics.compressorEnabled ? 1 : 0)
        .arg(QStringLiteral("%1|%2")
                 .arg(m_audioDynamics.compressorThresholdDb, 0, 'f', 2)
                 .arg(m_audioDynamics.compressorRatio, 0, 'f', 2));
}

bool PreviewWindow::audioWaveformEnvelopeForClip(const TimelineClip& clip,
                                                 int binCount,
                                                 qreal rangeStartNorm,
                                                 qreal rangeEndNorm,
                                                 QVector<qreal>* minOut,
                                                 QVector<qreal>* maxOut) const {
    if (!minOut || !maxOut) {
        return false;
    }
    const int safeBins = qBound(64, binCount, 8192);
    minOut->fill(0.0, safeBins);
    maxOut->fill(0.0, safeBins);

    // Waveform must follow playback audio source, not visual proxy media.
    QString mediaPath = playbackAudioPathForClip(clip);
    if (mediaPath.isEmpty()) {
        mediaPath = interactivePreviewMediaPathForClip(clip);
    }
    if (mediaPath.isEmpty()) {
        return false;
    }

    const int64_t sourceStartSample = qMax<int64_t>(0, frameToSamples(qMax<int64_t>(0, clip.sourceInFrame)));
    const int64_t sourceDurationSamples = qMax<int64_t>(1, frameToSamples(qMax<int64_t>(1, clip.durationFrames)));
    const qreal startNorm = qBound<qreal>(0.0, rangeStartNorm, 1.0);
    const qreal endNorm = qBound<qreal>(startNorm, rangeEndNorm, 1.0);
    const int64_t visibleStartOffset = static_cast<int64_t>(
        std::floor(startNorm * static_cast<qreal>(sourceDurationSamples)));
    const int64_t visibleEndOffset = static_cast<int64_t>(
        std::ceil(endNorm * static_cast<qreal>(sourceDurationSamples)));
    const int64_t visibleStartSample = sourceStartSample + qBound<int64_t>(0, visibleStartOffset, sourceDurationSamples);
    const int64_t visibleEndSample = sourceStartSample + qBound<int64_t>(
        visibleStartOffset + 1,
        visibleEndOffset,
        sourceDurationSamples);
    const QString variantKey = audioDynamicsCacheKey();
    const editor::WaveformService::WaveformProcessSettings processSettings{
        m_audioDynamics.amplifyEnabled,
        static_cast<float>(m_audioDynamics.amplifyDb),
        m_audioDynamics.normalizeEnabled,
        static_cast<float>(m_audioDynamics.normalizeTargetDb),
        m_audioDynamics.peakReductionEnabled,
        static_cast<float>(m_audioDynamics.peakThresholdDb),
        m_audioDynamics.limiterEnabled,
        static_cast<float>(m_audioDynamics.limiterThresholdDb),
        m_audioDynamics.compressorEnabled,
        static_cast<float>(m_audioDynamics.compressorThresholdDb),
        static_cast<float>(m_audioDynamics.compressorRatio)};

    QVector<float> minValues;
    QVector<float> maxValues;
    if (!editor::WaveformService::instance().queryEnvelope(mediaPath,
                                                           visibleStartSample,
                                                           visibleEndSample,
                                                           safeBins,
                                                           &minValues,
                                                           &maxValues,
                                                           variantKey,
                                                           &processSettings) ||
        minValues.size() != safeBins ||
        maxValues.size() != safeBins) {
        return false;
    }

    for (int i = 0; i < safeBins; ++i) {
        const qreal minV = qBound<qreal>(-1.0, static_cast<qreal>(minValues[i]), 1.0);
        const qreal maxV = qBound<qreal>(-1.0, static_cast<qreal>(maxValues[i]), 1.0);
        if (minV <= maxV) {
            (*minOut)[i] = minV;
            (*maxOut)[i] = maxV;
        } else {
            (*minOut)[i] = maxV;
            (*maxOut)[i] = minV;
        }
    }
    return true;
}


void PreviewWindow::drawAudioBadge(QPainter* painter, const QRect& targetRect,
                    const QList<TimelineClip>& activeAudioClips) {
    Q_UNUSED(painter)
    Q_UNUSED(targetRect)
    Q_UNUSED(activeAudioClips)
}

void PreviewWindow::drawSpeakerPickOverlay(QPainter* painter) const {
    const bool speakerPickReady =
        !m_speakerPickDragActive &&
        (QApplication::keyboardModifiers() & Qt::ShiftModifier) &&
        (speakerPointRequested || speakerBoxRequested) &&
        !m_speakerPickHintClipId.isEmpty();
    const QString clipId = m_speakerPickDragActive ? m_speakerPickClipId : m_speakerPickHintClipId;
    if (clipId.isEmpty()) {
        return;
    }
    const PreviewOverlayInfo info = m_overlayInfo.value(clipId);
    if (!info.bounds.isValid() || info.bounds.width() <= 1.0 || info.bounds.height() <= 1.0) {
        return;
    }

    const QPointF start = m_speakerPickDragActive ? m_speakerPickStartPos : m_speakerPickCurrentPos;
    const QPointF current = m_speakerPickDragActive
        ? (m_speakerPickCurrentPos.isNull() ? start : m_speakerPickCurrentPos)
        : m_speakerPickCurrentPos;
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
    if (m_speakerPickDragActive && dragDistance >= 0.01) {
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
    } else if (m_speakerPickDragActive) {
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

QRect PreviewWindow::fitRect(const QSize& source, const QRect& bounds) const {
    if (source.isEmpty() || bounds.isEmpty()) {
        return bounds;
    }
    
    QSize scaled = source;
    scaled.scale(bounds.size(), Qt::KeepAspectRatio);
    const QPoint topLeft(bounds.center().x() - scaled.width() / 2,
                         bounds.center().y() - scaled.height() / 2);
    return QRect(topLeft, scaled);
}
void PreviewWindow::drawPreviewChrome(QPainter* painter, const QRect& safeRect, int activeClipCount) const {
    Q_UNUSED(painter)
    Q_UNUSED(safeRect)
    Q_UNUSED(activeClipCount)
}
