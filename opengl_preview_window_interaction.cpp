#include "opengl_preview.h"
#include "titles.h"

#include <QContextMenuEvent>
#include <QApplication>
#include <QCursor>
#include <QEvent>
#include <QKeyEvent>
#include <QMouseEvent>
#include <QMenu>
#include <QOpenGLWidget>
#include <QTimer>
#include <QWheelEvent>

#include <cmath>

void PreviewWindow::showEvent(QShowEvent* event) {
    QOpenGLWidget::showEvent(event);
    m_frameRequestsArmed = true;
    m_pendingFrameRequest = true;
    if (!m_repaintTimer.isActive()) {
        m_repaintTimer.start();
    }
    scheduleFrameRequest();
}

void PreviewWindow::mousePressEvent(QMouseEvent* event) {
    if (event->button() != Qt::LeftButton) {
        QWidget::mousePressEvent(event);
        return;
    }

    if (m_interaction.correctionDrawMode) {
        m_interaction.transient.dragMode = PreviewDragMode::None;
        QString hitClipId;
        if (!m_interaction.selectedClipId.isEmpty()) {
            const PreviewOverlayInfo selectedInfo = m_overlayModel.overlays.value(m_interaction.selectedClipId);
            if (selectedInfo.bounds.contains(event->position())) {
                hitClipId = m_interaction.selectedClipId;
            }
        }
        if (hitClipId.isEmpty()) {
            hitClipId = clipIdAtPosition(event->position());
        }
        if (!hitClipId.isEmpty()) {
            if (m_interaction.selectedClipId != hitClipId) {
                m_interaction.selectedClipId = hitClipId;
                if (selectionRequested) {
                    selectionRequested(hitClipId);
                }
            }
            const PreviewOverlayInfo info = m_overlayModel.overlays.value(hitClipId);
            if (info.bounds.isValid() && info.bounds.width() > 1.0 && info.bounds.height() > 1.0) {
                const QPointF normalized = mapScreenPointToNormalizedClip(info, event->position());
                if (correctionPointRequested) {
                    correctionPointRequested(hitClipId, normalized.x(), normalized.y());
                }
                update();
                event->accept();
                return;
            }
        }
        event->accept();
        return;
    }

    // Shift+Click / Shift+Drag supports speaker anchoring workflows.
    if ((event->modifiers() & Qt::ShiftModifier) && (speakerPointRequested || speakerBoxRequested)) {
        QString hitClipId;
        if (!m_interaction.selectedClipId.isEmpty()) {
            const PreviewOverlayInfo selectedInfo = m_overlayModel.overlays.value(m_interaction.selectedClipId);
            if (selectedInfo.bounds.contains(event->position())) {
                hitClipId = m_interaction.selectedClipId;
            }
        }
        if (hitClipId.isEmpty()) {
            hitClipId = clipIdAtPosition(event->position());
        }
        if (!hitClipId.isEmpty()) {
            const PreviewOverlayInfo info = m_overlayModel.overlays.value(hitClipId);
            if (info.bounds.isValid() && info.bounds.width() > 1.0 && info.bounds.height() > 1.0) {
                m_interaction.transient.speakerPickDragActive = true;
                m_interaction.transient.speakerPickClipId = hitClipId;
                m_interaction.transient.speakerPickStartPos = event->position();
                m_interaction.transient.speakerPickCurrentPos = event->position();
                if (m_interaction.selectedClipId != hitClipId) {
                    m_interaction.selectedClipId = hitClipId;
                    if (selectionRequested) {
                        selectionRequested(hitClipId);
                    }
                }
                scheduleRepaint();
                event->accept();
                return;
            }
        }
    }

    const PreviewOverlayInfo selectedInfo = m_overlayModel.overlays.value(m_interaction.selectedClipId);
    const bool selectedClipIsTitle = clipIdIsTitle(m_interaction.selectedClipId);
    const bool allowSelectedClipInteraction =
        !m_interaction.titleOverlayInteractionOnly || selectedClipIsTitle;
    const bool transcriptOverlayInteractive =
        (selectedInfo.kind != PreviewOverlayKind::TranscriptOverlay || m_interaction.transcriptOverlayInteractionEnabled) &&
        allowSelectedClipInteraction;
    if (!m_interaction.selectedClipId.isEmpty()) {
        if (transcriptOverlayInteractive) {
            if (selectedInfo.cornerHandle.contains(event->position())) m_interaction.transient.dragMode = PreviewDragMode::ResizeBoth;
            else if (selectedInfo.rightHandle.contains(event->position())) m_interaction.transient.dragMode = PreviewDragMode::ResizeX;
            else if (selectedInfo.bottomHandle.contains(event->position())) m_interaction.transient.dragMode = PreviewDragMode::ResizeY;
            else if (selectedInfo.bounds.contains(event->position())) m_interaction.transient.dragMode = PreviewDragMode::Move;
        }
        if (m_interaction.transient.dragMode != PreviewDragMode::None) {
            m_interaction.transient.dragOriginPos = event->position();
            m_interaction.transient.dragOriginTransform = evaluateTransformForSelectedClip();
            m_interaction.transient.dragOriginBounds = selectedInfo.bounds;
            m_interaction.transient.dragOriginTranscriptTranslation = QPointF();
            if (selectedInfo.kind == PreviewOverlayKind::TranscriptOverlay) {
                for (const TimelineClip& clip : m_interaction.clips) {
                    if (clip.id == m_interaction.selectedClipId) {
                        m_interaction.transient.dragOriginTranscriptTranslation = QPointF(
                            clip.transcriptOverlay.translationX,
                            clip.transcriptOverlay.translationY);
                        break;
                    }
                }
            }
            event->accept();
            return;
        }
    }

    QString hitClipId = clipIdAtPosition(event->position());
    if (m_interaction.titleOverlayInteractionOnly && !clipIdIsTitle(hitClipId)) {
        hitClipId.clear();
    }
    if (!hitClipId.isEmpty()) {
        m_interaction.selectedClipId = hitClipId;
        if (selectionRequested) selectionRequested(hitClipId);
        updatePreviewCursor(event->position());
        update();
        event->accept();
        return;
    }

    QWidget::mousePressEvent(event);
}

void PreviewWindow::mouseMoveEvent(QMouseEvent* event) {
    m_interaction.transient.lastMousePos = event->position();

    if (m_interaction.correctionDrawMode) {
        m_interaction.transient.dragMode = PreviewDragMode::None;
        if (event->buttons() & Qt::LeftButton) {
            event->accept();
            return;
        }
        setCursor(Qt::CrossCursor);
        QWidget::mouseMoveEvent(event);
        return;
    }

    if (m_interaction.transient.speakerPickDragActive && (event->buttons() & Qt::LeftButton)) {
        m_interaction.transient.speakerPickCurrentPos = event->position();
        scheduleRepaint();
        event->accept();
        return;
    }

    if (m_interaction.titleOverlayInteractionOnly && !clipIdIsTitle(m_interaction.selectedClipId)) {
        m_interaction.transient.dragMode = PreviewDragMode::None;
    }

    if (m_interaction.transient.dragMode != PreviewDragMode::None && (event->buttons() & Qt::LeftButton) &&
        !m_interaction.selectedClipId.isEmpty() && m_interaction.transient.dragOriginBounds.width() > 1.0 && m_interaction.transient.dragOriginBounds.height() > 1.0) {
        
        const PreviewOverlayInfo selectedInfo = m_overlayModel.overlays.value(m_interaction.selectedClipId);
        
        if (m_interaction.transient.dragMode == PreviewDragMode::Move) {
            if (moveRequested) {
                QSizeF dragClipSize = selectedInfo.clipPixelSize;
                if (dragClipSize.width() <= 1.0 || dragClipSize.height() <= 1.0) {
                    const QRect compositeRect = scaledCanvasRect(previewCanvasBaseRect());
                    dragClipSize = QSizeF(compositeRect.width(), compositeRect.height());
                }
                const QSize safeOutputSize = m_outputSize.isValid() ? m_outputSize : QSize(1080, 1920);
                const qreal previewScaleX = qMax<qreal>(0.0001,
                    dragClipSize.width() / qMax<qreal>(1.0, static_cast<qreal>(safeOutputSize.width())));
                const qreal previewScaleY = qMax<qreal>(0.0001,
                    dragClipSize.height() / qMax<qreal>(1.0, static_cast<qreal>(safeOutputSize.height())));
                const bool invertVertical = (m_effectiveRenderBackend == QStringLiteral("vulkan"));
                const qreal deltaX =
                    (event->position().x() - m_interaction.transient.dragOriginPos.x()) / previewScaleX;
                const qreal deltaY =
                    (event->position().y() - m_interaction.transient.dragOriginPos.y()) / previewScaleY;
                const qreal translationDeltaY = invertVertical ? -deltaY : deltaY;

                if (selectedInfo.kind == PreviewOverlayKind::TranscriptOverlay) {
                    const QSize safeOutputSize = m_interaction.outputSize.isValid() ? m_interaction.outputSize : QSize(1080, 1920);
                    const qreal halfOutputWidth =
                        qMax<qreal>(1.0, static_cast<qreal>(safeOutputSize.width()) * 0.5);
                    const qreal halfOutputHeight =
                        qMax<qreal>(1.0, static_cast<qreal>(safeOutputSize.height()) * 0.5);
                    const qreal deltaXNorm = deltaX / halfOutputWidth;
                    const qreal deltaYNorm = deltaY / halfOutputHeight;
                    moveRequested(m_interaction.selectedClipId,
                                  qBound<qreal>(-1.0, m_interaction.transient.dragOriginTranscriptTranslation.x() + deltaXNorm, 1.0),
                                  qBound<qreal>(-1.0, m_interaction.transient.dragOriginTranscriptTranslation.y() + deltaYNorm, 1.0),
                                  false);
                    event->accept();
                    return;
                }

                moveRequested(m_interaction.selectedClipId,
                              m_interaction.transient.dragOriginTransform.translationX + deltaX,
                              m_interaction.transient.dragOriginTransform.translationY + translationDeltaY,
                              false);
            }
            event->accept();
            return;
        }
        
        qreal scaleX = m_interaction.transient.dragOriginTransform.scaleX;
        qreal scaleY = m_interaction.transient.dragOriginTransform.scaleY;
        
        if (selectedInfo.kind == PreviewOverlayKind::TranscriptOverlay) {
            const QRect compositeRect = scaledCanvasRect(previewCanvasBaseRect());
            const QPointF previewScale = previewCanvasScale(compositeRect);
            const qreal originWidth = m_interaction.transient.dragOriginBounds.width() /
                qMax<qreal>(0.0001, previewScale.x());
            const qreal originHeight = m_interaction.transient.dragOriginBounds.height() /
                qMax<qreal>(0.0001, previewScale.y());
            qreal width = originWidth;
            qreal height = originHeight;
            
            if (m_interaction.transient.dragMode == PreviewDragMode::ResizeX || m_interaction.transient.dragMode == PreviewDragMode::ResizeBoth) {
                width = qMax<qreal>(80.0, width + ((event->position().x() - m_interaction.transient.dragOriginPos.x()) /
                                                   qMax<qreal>(0.0001, previewScale.x())));
            }
            if (m_interaction.transient.dragMode == PreviewDragMode::ResizeY || m_interaction.transient.dragMode == PreviewDragMode::ResizeBoth) {
                height = qMax<qreal>(40.0, height + ((event->position().y() - m_interaction.transient.dragOriginPos.y()) /
                                                    qMax<qreal>(0.0001, previewScale.y())));
            }
            if (resizeRequested) {
                resizeRequested(m_interaction.selectedClipId, width, height, false);
            }
            event->accept();
            return;
        }
        
        if (m_interaction.transient.dragMode == PreviewDragMode::ResizeX || m_interaction.transient.dragMode == PreviewDragMode::ResizeBoth) {
            scaleX = sanitizeScaleValue(
                m_interaction.transient.dragOriginTransform.scaleX *
                ((m_interaction.transient.dragOriginBounds.width() + (event->position().x() - m_interaction.transient.dragOriginPos.x())) /
                 m_interaction.transient.dragOriginBounds.width()));
        }
        if (m_interaction.transient.dragMode == PreviewDragMode::ResizeY || m_interaction.transient.dragMode == PreviewDragMode::ResizeBoth) {
            scaleY = sanitizeScaleValue(
                m_interaction.transient.dragOriginTransform.scaleY *
                ((m_interaction.transient.dragOriginBounds.height() + (event->position().y() - m_interaction.transient.dragOriginPos.y())) /
                 m_interaction.transient.dragOriginBounds.height()));
        }
        if (m_interaction.transient.dragMode == PreviewDragMode::ResizeBoth) {
            const qreal factorX =
                (m_interaction.transient.dragOriginBounds.width() + (event->position().x() - m_interaction.transient.dragOriginPos.x())) /
                m_interaction.transient.dragOriginBounds.width();
            const qreal factorY =
                (m_interaction.transient.dragOriginBounds.height() + (event->position().y() - m_interaction.transient.dragOriginPos.y())) /
                m_interaction.transient.dragOriginBounds.height();
            const qreal uniformFactor = std::abs(factorX) >= std::abs(factorY) ? factorX : factorY;
            scaleX = sanitizeScaleValue(m_interaction.transient.dragOriginTransform.scaleX * uniformFactor);
            scaleY = sanitizeScaleValue(m_interaction.transient.dragOriginTransform.scaleY * uniformFactor);
        }
        if (resizeRequested) {
            resizeRequested(m_interaction.selectedClipId, scaleX, scaleY, false);
        }
        event->accept();
        return;
    }
    
    updatePreviewCursor(event->position());
    if (m_interaction.viewMode == ViewMode::Audio && event->buttons() == Qt::NoButton) {
        scheduleRepaint();
    }
    QWidget::mouseMoveEvent(event);
}

void PreviewWindow::leaveEvent(QEvent* event) {
    m_interaction.transient.lastMousePos = QPointF(-10000.0, -10000.0);
    if (m_interaction.viewMode == ViewMode::Audio) {
        scheduleRepaint();
    }
    QWidget::leaveEvent(event);
}

void PreviewWindow::mouseReleaseEvent(QMouseEvent* event) {
    if (event->button() == Qt::LeftButton && m_interaction.transient.speakerPickDragActive) {
        const QString clipId = m_interaction.transient.speakerPickClipId;
        const PreviewOverlayInfo info = m_overlayModel.overlays.value(clipId);
        const QPointF endPos = event->position();
        m_interaction.transient.speakerPickCurrentPos = endPos;
        if (info.bounds.isValid() && info.bounds.width() > 1.0 && info.bounds.height() > 1.0) {
            const QPointF startNorm = mapScreenPointToNormalizedClip(info, m_interaction.transient.speakerPickStartPos);
            const QPointF endNorm = mapScreenPointToNormalizedClip(info, endPos);
            const qreal dx = endNorm.x() - startNorm.x();
            const qreal dy = endNorm.y() - startNorm.y();
            const qreal dragDistance = std::sqrt((dx * dx) + (dy * dy));
            if (speakerBoxRequested) {
                const qreal startScreenX = m_interaction.transient.speakerPickStartPos.x();
                const qreal startScreenY = m_interaction.transient.speakerPickStartPos.y();
                const qreal endScreenX = endPos.x();
                const qreal endScreenY = endPos.y();
                const qreal sideScreenPx = qMax(qAbs(endScreenX - startScreenX),
                                                qAbs(endScreenY - startScreenY));
                const qreal minScreenSide = qMax<qreal>(
                    1.0, qMin<qreal>(info.bounds.width(), info.bounds.height()));
                const qreal side = qBound<qreal>(
                    0.02,
                    dragDistance >= 0.01 ? (sideScreenPx / minScreenSide) : 0.06,
                    1.0);
                const qreal cx = qBound<qreal>(0.0, (startNorm.x() + endNorm.x()) * 0.5, 1.0);
                const qreal cy = qBound<qreal>(0.0, (startNorm.y() + endNorm.y()) * 0.5, 1.0);
                speakerBoxRequested(clipId, cx, cy, side);
            }
        }
        m_interaction.transient.speakerPickDragActive = false;
        m_interaction.transient.speakerPickClipId.clear();
        m_interaction.transient.speakerPickStartPos = QPointF();
        m_interaction.transient.speakerPickCurrentPos = QPointF();
        scheduleRepaint();
        event->accept();
        return;
    }

    if (m_interaction.correctionDrawMode && event->button() == Qt::LeftButton) {
        m_interaction.transient.dragMode = PreviewDragMode::None;
        m_interaction.transient.dragOriginBounds = QRectF();
        event->accept();
        return;
    }

    if (event->button() == Qt::LeftButton && m_interaction.transient.dragMode != PreviewDragMode::None) {
        const PreviewOverlayInfo selectedInfo = m_overlayModel.overlays.value(m_interaction.selectedClipId);
        const TimelineClip::TransformKeyframe transform = evaluateTransformForSelectedClip();
        
        if (m_interaction.transient.dragMode == PreviewDragMode::Move) {
            if (moveRequested) {
                if (selectedInfo.kind == PreviewOverlayKind::TranscriptOverlay) {
                    for (const TimelineClip& clip : m_interaction.clips) {
                        if (clip.id == m_interaction.selectedClipId) {
                            moveRequested(m_interaction.selectedClipId,
                                          clip.transcriptOverlay.translationX,
                                          clip.transcriptOverlay.translationY,
                                          true);
                            break;
                        }
                    }
                } else {
                    moveRequested(m_interaction.selectedClipId, transform.translationX, transform.translationY, true);
                }
            }
        } else if (resizeRequested) {
            if (selectedInfo.kind == PreviewOverlayKind::TranscriptOverlay) {
                const QSizeF size = transcriptOverlaySizeForSelectedClip();
                resizeRequested(m_interaction.selectedClipId, size.width(), size.height(), true);
            } else {
                resizeRequested(m_interaction.selectedClipId, transform.scaleX, transform.scaleY, true);
            }
        }
        
        m_interaction.transient.dragMode = PreviewDragMode::None;
        m_interaction.transient.dragOriginBounds = QRectF();
        m_interaction.transient.dragOriginTranscriptTranslation = QPointF();
        updatePreviewCursor(event->position());
        event->accept();
        return;
    }
    QWidget::mouseReleaseEvent(event);
}

void PreviewWindow::keyPressEvent(QKeyEvent* event) {
    if ((event->key() == Qt::Key_Shift || (event->modifiers() & Qt::ShiftModifier)) &&
        (speakerPointRequested || speakerBoxRequested)) {
        updatePreviewCursor(mapFromGlobal(QCursor::pos()));
        scheduleRepaint();
    }
    QWidget::keyPressEvent(event);
}

void PreviewWindow::keyReleaseEvent(QKeyEvent* event) {
    if (event->key() == Qt::Key_Shift) {
        updatePreviewCursor(mapFromGlobal(QCursor::pos()));
        scheduleRepaint();
    }
    QWidget::keyReleaseEvent(event);
}

void PreviewWindow::wheelEvent(QWheelEvent* event) {
    if (event->angleDelta().y() == 0) {
        QWidget::wheelEvent(event);
        return;
    }
    if (m_interaction.viewMode == ViewMode::Audio) {
        const qreal factor = event->angleDelta().y() > 0 ? 1.1 : (1.0 / 1.1);
        const QRect safeRect = rect().adjusted(24, 24, -24, -24);
        const QRect panel = safeRect.adjusted(12, 12, -12, -12);
        const QRect waveRect = panel.adjusted(24, 120, -24, -36);

        int64_t clipSamples = 0;
        for (const TimelineClip& clip : m_interaction.clips) {
            const bool includeForAudioView =
                clipAudioPlaybackEnabled(clip) &&
                (clip.id == m_interaction.selectedClipId || isSampleWithinClip(clip, m_interaction.currentSample));
            const bool includeAsFallback =
                clipIsAudioOnly(clip) && isSampleWithinClip(clip, m_interaction.currentSample);
            if (includeForAudioView || includeAsFallback) {
                clipSamples = qMax<int64_t>(1, frameToSamples(qMax<int64_t>(1, clip.durationFrames)));
                break;
            }
        }
        const int rowCount = qBound(2, waveRect.height() / 88, 6);
        const int binsPerRow = qMax(256, waveRect.width());
        const int totalDrawBins = qMax(96, rowCount * binsPerRow);
        const qreal minVisibleBySamples = clipSamples > 0
            ? qBound<qreal>(0.00001,
                            (100.0 * static_cast<qreal>(rowCount)) / static_cast<qreal>(clipSamples),
                            1.0)
            : 0.001;
        const qreal maxAudioZoom =
            qBound<qreal>(20.0, 1.0 / qMax<qreal>(0.00001, minVisibleBySamples), 100000.0);
        const qreal oldZoom = qBound<qreal>(1.0, m_interaction.previewZoom, maxAudioZoom);
        const qreal nextZoom = qBound<qreal>(1.0, oldZoom * factor, maxAudioZoom);
        if (qAbs(nextZoom - oldZoom) < 0.000001) {
            event->accept();
            return;
        }
        const qreal oldVisible = qBound<qreal>(minVisibleBySamples, 1.0 / oldZoom, 1.0);
        const qreal nextVisible = qBound<qreal>(minVisibleBySamples, 1.0 / nextZoom, 1.0);
        const qreal oldStart = qBound<qreal>(0.0, m_interaction.previewPanOffset.x(), qMax<qreal>(0.0, 1.0 - oldVisible));

        const qreal localX = qBound<qreal>(
            0.0,
            event->position().x() - static_cast<qreal>(waveRect.left()),
            qMax<qreal>(0.0, static_cast<qreal>(waveRect.width())));
        const qreal localY = qBound<qreal>(
            0.0,
            event->position().y() - static_cast<qreal>(waveRect.top()),
            qMax<qreal>(0.0, static_cast<qreal>(waveRect.height())));
        const qreal rowHeight = static_cast<qreal>(waveRect.height()) / qMax(1, rowCount);
        const int rowIndex = qBound(
            0,
            static_cast<int>(std::floor(localY / qMax<qreal>(1.0, rowHeight))),
            qMax(0, rowCount - 1));
        const int rowBinCount = qMax(2, binsPerRow);
        const qreal rowXNorm = qBound<qreal>(
            0.0,
            localX / qMax<qreal>(1.0, static_cast<qreal>(waveRect.width())),
            1.0);
        const int binInRow = qBound(
            0,
            static_cast<int>(std::round(rowXNorm * static_cast<qreal>(rowBinCount - 1))),
            rowBinCount - 1);
        const int visibleBinIndex = qBound(
            0,
            rowIndex * binsPerRow + binInRow,
            qMax(0, totalDrawBins - 1));
        const qreal anchorNorm =
            (static_cast<qreal>(visibleBinIndex) + 0.5) / qMax<qreal>(1.0, static_cast<qreal>(totalDrawBins));
        const qreal anchoredTimelineNorm = oldStart + (anchorNorm * oldVisible);
        const qreal nextStart = qBound<qreal>(
            0.0,
            anchoredTimelineNorm - (anchorNorm * nextVisible),
            qMax<qreal>(0.0, 1.0 - nextVisible));

        m_interaction.previewZoom = nextZoom;
        m_interaction.previewPanOffset = QPointF(nextStart, 0.0);
        scheduleRepaint();
        event->accept();
        return;
    }
    const QRect baseRect = previewCanvasBaseRect();
    const QRect oldRect = scaledCanvasRect(baseRect);
    const qreal factor = event->angleDelta().y() > 0 ? 1.1 : (1.0 / 1.1);
    // Extended zoom range: 0.1x to 20.0x
    const qreal oldZoom = qBound<qreal>(0.1, m_interaction.previewZoom, 20.0);
    const qreal nextZoom = qBound<qreal>(0.1, oldZoom * factor, 20.0);
    if (qAbs(nextZoom - oldZoom) < 0.000001) {
        event->accept();
        return;
    }
    const QPointF anchor = QPointF((event->position().x() - oldRect.left()) / qMax(1.0, static_cast<qreal>(oldRect.width())),
                                   (event->position().y() - oldRect.top()) / qMax(1.0, static_cast<qreal>(oldRect.height())));
    m_interaction.previewZoom = nextZoom;
    const QSizeF newSize(baseRect.width() * m_interaction.previewZoom, baseRect.height() * m_interaction.previewZoom);
    const QPointF centeredTopLeft(baseRect.center().x() - (newSize.width() / 2.0),
                                  baseRect.center().y() - (newSize.height() / 2.0));
    const QPointF anchoredTopLeft(event->position().x() - (anchor.x() * newSize.width()),
                                  event->position().y() - (anchor.y() * newSize.height()));
    m_interaction.previewPanOffset = anchoredTopLeft - centeredTopLeft;
    scheduleRepaint();
    event->accept();
}

void PreviewWindow::contextMenuEvent(QContextMenuEvent* event) {
    QString hitClipId = clipIdAtPosition(event->pos());
    if (m_interaction.titleOverlayInteractionOnly && !clipIdIsTitle(hitClipId)) {
        hitClipId.clear();
    }
    if (hitClipId.isEmpty()) {
        QWidget::contextMenuEvent(event);
        return;
    }

    const PreviewOverlayInfo info = m_overlayModel.overlays.value(hitClipId);
    if (info.kind == PreviewOverlayKind::TranscriptOverlay) {
        QWidget::contextMenuEvent(event);
        return;
    }

    if (m_interaction.selectedClipId != hitClipId) {
        m_interaction.selectedClipId = hitClipId;
        if (selectionRequested) selectionRequested(hitClipId);
        update();
    }

    QMenu menu(this);
    QAction* createKeyframeAction = menu.addAction(QStringLiteral("Create Keyframe Here"));
    QAction* chosen = menu.exec(event->globalPos());
    if (chosen == createKeyframeAction && createKeyframeRequested) {
        createKeyframeRequested(hitClipId);
        event->accept();
        return;
    }

    QWidget::contextMenuEvent(event);
}

QString PreviewWindow::clipIdAtPosition(const QPointF& position) const {
    for (int i = m_overlayModel.paintOrder.size() - 1; i >= 0; --i) {
        const QString& clipId = m_overlayModel.paintOrder[i];
        if (m_overlayModel.overlays.value(clipId).bounds.contains(position)) {
            return clipId;
        }
    }
    return QString();
}

TimelineClip::TransformKeyframe PreviewWindow::evaluateTransformForSelectedClip() const {
    for (const TimelineClip& clip : m_interaction.clips) {
        if (clip.id == m_interaction.selectedClipId) {
            // Title clips use their own coordinate system in titleKeyframes
            if (clip.mediaType == ClipMediaType::Title) {
                const int64_t localFrame = qMax<int64_t>(0,
                    static_cast<int64_t>(m_interaction.currentFramePosition) - clip.startFrame);
                const EvaluatedTitle title = evaluateTitleAtLocalFrame(clip, localFrame);
                TimelineClip::TransformKeyframe kf;
                kf.translationX = title.x;
                kf.translationY = title.y;
                return kf;
            }
            return evaluateClipRenderTransformAtPosition(clip, m_interaction.currentFramePosition, m_interaction.outputSize);
        }
    }
    return TimelineClip::TransformKeyframe();
}

void PreviewWindow::updatePreviewCursor(const QPointF& position) {
    m_interaction.transient.speakerPickCurrentPos = position;

    if (m_interaction.correctionDrawMode) {
        if (!m_interaction.transient.speakerPickHintClipId.isEmpty()) {
            m_interaction.transient.speakerPickHintClipId.clear();
            scheduleRepaint();
        }
        setCursor(Qt::CrossCursor);
        return;
    }

    const bool speakerPickModifierActive =
        (QApplication::keyboardModifiers() & Qt::ShiftModifier) &&
        (speakerPointRequested || speakerBoxRequested);
    const QString speakerPickHintClipId =
        speakerPickModifierActive ? clipIdAtPosition(position) : QString();
    if (m_interaction.transient.speakerPickHintClipId != speakerPickHintClipId) {
        m_interaction.transient.speakerPickHintClipId = speakerPickHintClipId;
        scheduleRepaint();
    }
    if (!speakerPickHintClipId.isEmpty()) {
        setCursor(Qt::CrossCursor);
        return;
    }

    const PreviewOverlayInfo info = m_overlayModel.overlays.value(m_interaction.selectedClipId);
    if (m_interaction.titleOverlayInteractionOnly && !clipIdIsTitle(m_interaction.selectedClipId)) {
        unsetCursor();
        return;
    }
    if (!m_interaction.transcriptOverlayInteractionEnabled &&
        info.kind == PreviewOverlayKind::TranscriptOverlay) {
        unsetCursor();
        return;
    }
    if (!m_interaction.selectedClipId.isEmpty()) {
        if (info.cornerHandle.contains(position)) {
            setCursor(Qt::SizeFDiagCursor);
            return;
        }
        if (info.rightHandle.contains(position)) {
            setCursor(Qt::SizeHorCursor);
            return;
        }
        if (info.bottomHandle.contains(position)) {
            setCursor(Qt::SizeVerCursor);
            return;
        }
        if (info.bounds.contains(position)) {
            setCursor(m_interaction.transient.dragMode == PreviewDragMode::Move ? Qt::ClosedHandCursor : Qt::OpenHandCursor);
            return;
        }
    }
    unsetCursor();
}
