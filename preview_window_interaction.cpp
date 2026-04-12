#include "preview.h"
#include "titles.h"

#include <QContextMenuEvent>
#include <QMouseEvent>
#include <QMenu>
#include <QOpenGLWidget>
#include <QTimer>
#include <QWheelEvent>

void PreviewWindow::showEvent(QShowEvent* event) {
    QOpenGLWidget::showEvent(event);
    m_frameRequestsArmed = true;
    m_pendingFrameRequest = true;
    scheduleFrameRequest();
}

void PreviewWindow::mousePressEvent(QMouseEvent* event) {
    if (event->button() != Qt::LeftButton) {
        QWidget::mousePressEvent(event);
        return;
    }

    if (m_correctionDrawMode) {
        m_dragMode = PreviewDragMode::None;
        QString hitClipId;
        if (!m_selectedClipId.isEmpty()) {
            const PreviewOverlayInfo selectedInfo = m_overlayInfo.value(m_selectedClipId);
            if (selectedInfo.bounds.contains(event->position())) {
                hitClipId = m_selectedClipId;
            }
        }
        if (hitClipId.isEmpty()) {
            hitClipId = clipIdAtPosition(event->position());
        }
        if (!hitClipId.isEmpty()) {
            if (m_selectedClipId != hitClipId) {
                m_selectedClipId = hitClipId;
                if (selectionRequested) {
                    selectionRequested(hitClipId);
                }
            }
            const PreviewOverlayInfo info = m_overlayInfo.value(hitClipId);
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

    const PreviewOverlayInfo selectedInfo = m_overlayInfo.value(m_selectedClipId);
    if (!m_selectedClipId.isEmpty()) {
        if (selectedInfo.cornerHandle.contains(event->position())) m_dragMode = PreviewDragMode::ResizeBoth;
        else if (selectedInfo.rightHandle.contains(event->position())) m_dragMode = PreviewDragMode::ResizeX;
        else if (selectedInfo.bottomHandle.contains(event->position())) m_dragMode = PreviewDragMode::ResizeY;
        else if (selectedInfo.bounds.contains(event->position())) m_dragMode = PreviewDragMode::Move;
        if (m_dragMode != PreviewDragMode::None) {
            m_dragOriginPos = event->position();
            m_dragOriginTransform = evaluateTransformForSelectedClip();
            m_dragOriginBounds = selectedInfo.bounds;
            event->accept();
            return;
        }
    }

    const QString hitClipId = clipIdAtPosition(event->position());
    if (!hitClipId.isEmpty()) {
        m_selectedClipId = hitClipId;
        if (selectionRequested) selectionRequested(hitClipId);
        updatePreviewCursor(event->position());
        update();
        event->accept();
        return;
    }

    QWidget::mousePressEvent(event);
}

void PreviewWindow::mouseMoveEvent(QMouseEvent* event) {
    if (m_correctionDrawMode) {
        m_dragMode = PreviewDragMode::None;
        if (event->buttons() & Qt::LeftButton) {
            event->accept();
            return;
        }
        setCursor(Qt::CrossCursor);
        QWidget::mouseMoveEvent(event);
        return;
    }

    if (m_dragMode != PreviewDragMode::None && (event->buttons() & Qt::LeftButton) &&
        !m_selectedClipId.isEmpty() && m_dragOriginBounds.width() > 1.0 && m_dragOriginBounds.height() > 1.0) {
        
        const PreviewOverlayInfo selectedInfo = m_overlayInfo.value(m_selectedClipId);
        
        if (m_dragMode == PreviewDragMode::Move) {
            if (moveRequested) {
                const QRect compositeRect = scaledCanvasRect(previewCanvasBaseRect());
                const QPointF previewScale = previewCanvasScale(compositeRect);
                moveRequested(m_selectedClipId,
                              m_dragOriginTransform.translationX +
                                  ((event->position().x() - m_dragOriginPos.x()) /
                                   qMax<qreal>(0.0001, previewScale.x())),
                              m_dragOriginTransform.translationY +
                                  ((event->position().y() - m_dragOriginPos.y()) /
                                   qMax<qreal>(0.0001, previewScale.y())),
                              false);
            }
            event->accept();
            return;
        }
        
        qreal scaleX = m_dragOriginTransform.scaleX;
        qreal scaleY = m_dragOriginTransform.scaleY;
        
        if (selectedInfo.kind == PreviewOverlayKind::TranscriptOverlay) {
            const QRect compositeRect = scaledCanvasRect(previewCanvasBaseRect());
            const QPointF previewScale = previewCanvasScale(compositeRect);
            qreal width = transcriptOverlaySizeForSelectedClip().width();
            qreal height = transcriptOverlaySizeForSelectedClip().height();
            
            if (m_dragMode == PreviewDragMode::ResizeX || m_dragMode == PreviewDragMode::ResizeBoth) {
                width = qMax<qreal>(80.0, width + ((event->position().x() - m_dragOriginPos.x()) /
                                                   qMax<qreal>(0.0001, previewScale.x())));
            }
            if (m_dragMode == PreviewDragMode::ResizeY || m_dragMode == PreviewDragMode::ResizeBoth) {
                height = qMax<qreal>(40.0, height + ((event->position().y() - m_dragOriginPos.y()) /
                                                    qMax<qreal>(0.0001, previewScale.y())));
            }
            if (resizeRequested) {
                resizeRequested(m_selectedClipId, width, height, false);
            }
            event->accept();
            return;
        }
        
        if (m_dragMode == PreviewDragMode::ResizeX || m_dragMode == PreviewDragMode::ResizeBoth) {
            scaleX = sanitizeScaleValue(
                m_dragOriginTransform.scaleX *
                ((m_dragOriginBounds.width() + (event->position().x() - m_dragOriginPos.x())) /
                 m_dragOriginBounds.width()));
        }
        if (m_dragMode == PreviewDragMode::ResizeY || m_dragMode == PreviewDragMode::ResizeBoth) {
            scaleY = sanitizeScaleValue(
                m_dragOriginTransform.scaleY *
                ((m_dragOriginBounds.height() + (event->position().y() - m_dragOriginPos.y())) /
                 m_dragOriginBounds.height()));
        }
        if (m_dragMode == PreviewDragMode::ResizeBoth) {
            const qreal factorX =
                (m_dragOriginBounds.width() + (event->position().x() - m_dragOriginPos.x())) /
                m_dragOriginBounds.width();
            const qreal factorY =
                (m_dragOriginBounds.height() + (event->position().y() - m_dragOriginPos.y())) /
                m_dragOriginBounds.height();
            const qreal uniformFactor = std::abs(factorX) >= std::abs(factorY) ? factorX : factorY;
            scaleX = sanitizeScaleValue(m_dragOriginTransform.scaleX * uniformFactor);
            scaleY = sanitizeScaleValue(m_dragOriginTransform.scaleY * uniformFactor);
        }
        if (resizeRequested) {
            resizeRequested(m_selectedClipId, scaleX, scaleY, false);
        }
        event->accept();
        return;
    }
    
    updatePreviewCursor(event->position());
    QWidget::mouseMoveEvent(event);
}

void PreviewWindow::mouseReleaseEvent(QMouseEvent* event) {
    if (m_correctionDrawMode && event->button() == Qt::LeftButton) {
        m_dragMode = PreviewDragMode::None;
        m_dragOriginBounds = QRectF();
        event->accept();
        return;
    }

    if (event->button() == Qt::LeftButton && m_dragMode != PreviewDragMode::None) {
        const PreviewOverlayInfo selectedInfo = m_overlayInfo.value(m_selectedClipId);
        const TimelineClip::TransformKeyframe transform = evaluateTransformForSelectedClip();
        
        if (m_dragMode == PreviewDragMode::Move) {
            if (moveRequested) {
                if (selectedInfo.kind == PreviewOverlayKind::TranscriptOverlay) {
                    for (const TimelineClip& clip : m_clips) {
                        if (clip.id == m_selectedClipId) {
                            moveRequested(m_selectedClipId,
                                          clip.transcriptOverlay.translationX,
                                          clip.transcriptOverlay.translationY,
                                          true);
                            break;
                        }
                    }
                } else {
                    moveRequested(m_selectedClipId, transform.translationX, transform.translationY, true);
                }
            }
        } else if (resizeRequested) {
            if (selectedInfo.kind == PreviewOverlayKind::TranscriptOverlay) {
                const QSizeF size = transcriptOverlaySizeForSelectedClip();
                resizeRequested(m_selectedClipId, size.width(), size.height(), true);
            } else {
                resizeRequested(m_selectedClipId, transform.scaleX, transform.scaleY, true);
            }
        }
        
        m_dragMode = PreviewDragMode::None;
        m_dragOriginBounds = QRectF();
        updatePreviewCursor(event->position());
        event->accept();
        return;
    }
    QWidget::mouseReleaseEvent(event);
}

void PreviewWindow::wheelEvent(QWheelEvent* event) {
    if (event->angleDelta().y() == 0) {
        QWidget::wheelEvent(event);
        return;
    }
    const QRect baseRect = previewCanvasBaseRect();
    const QRect oldRect = scaledCanvasRect(baseRect);
    const qreal factor = event->angleDelta().y() > 0 ? 1.1 : (1.0 / 1.1);
    // Extended zoom range: 0.1x to 5.0x (allows further zoom out)
    const qreal nextZoom = qBound<qreal>(0.1, m_previewZoom * factor, 5.0);
    const QPointF anchor = QPointF((event->position().x() - oldRect.left()) / qMax(1.0, static_cast<qreal>(oldRect.width())),
                                   (event->position().y() - oldRect.top()) / qMax(1.0, static_cast<qreal>(oldRect.height())));
    m_previewZoom = nextZoom;
    const QSizeF newSize(baseRect.width() * m_previewZoom, baseRect.height() * m_previewZoom);
    const QPointF centeredTopLeft(baseRect.center().x() - (newSize.width() / 2.0),
                                  baseRect.center().y() - (newSize.height() / 2.0));
    const QPointF anchoredTopLeft(event->position().x() - (anchor.x() * newSize.width()),
                                  event->position().y() - (anchor.y() * newSize.height()));
    m_previewPanOffset = anchoredTopLeft - centeredTopLeft;
    scheduleRepaint();
    event->accept();
}

void PreviewWindow::contextMenuEvent(QContextMenuEvent* event) {
    const QString hitClipId = clipIdAtPosition(event->pos());
    if (hitClipId.isEmpty()) {
        QWidget::contextMenuEvent(event);
        return;
    }

    const PreviewOverlayInfo info = m_overlayInfo.value(hitClipId);
    if (info.kind == PreviewOverlayKind::TranscriptOverlay) {
        QWidget::contextMenuEvent(event);
        return;
    }

    if (m_selectedClipId != hitClipId) {
        m_selectedClipId = hitClipId;
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
    for (int i = m_paintOrder.size() - 1; i >= 0; --i) {
        const QString& clipId = m_paintOrder[i];
        if (m_overlayInfo.value(clipId).bounds.contains(position)) {
            return clipId;
        }
    }
    return QString();
}

TimelineClip::TransformKeyframe PreviewWindow::evaluateTransformForSelectedClip() const {
    for (const TimelineClip& clip : m_clips) {
        if (clip.id == m_selectedClipId) {
            // Title clips use their own coordinate system in titleKeyframes
            if (clip.mediaType == ClipMediaType::Title) {
                const int64_t localFrame = qMax<int64_t>(0,
                    static_cast<int64_t>(m_currentFramePosition) - clip.startFrame);
                const EvaluatedTitle title = evaluateTitleAtLocalFrame(clip, localFrame);
                TimelineClip::TransformKeyframe kf;
                kf.translationX = title.x;
                kf.translationY = title.y;
                return kf;
            }
            return evaluateClipTransformAtPosition(clip, m_currentFramePosition);
        }
    }
    return TimelineClip::TransformKeyframe();
}

void PreviewWindow::updatePreviewCursor(const QPointF& position) {
    if (m_correctionDrawMode) {
        setCursor(Qt::CrossCursor);
        return;
    }

    const PreviewOverlayInfo info = m_overlayInfo.value(m_selectedClipId);
    if (!m_selectedClipId.isEmpty()) {
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
            setCursor(m_dragMode == PreviewDragMode::Move ? Qt::ClosedHandCursor : Qt::OpenHandCursor);
            return;
        }
    }
    unsetCursor();
}
