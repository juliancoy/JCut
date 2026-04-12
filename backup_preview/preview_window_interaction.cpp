#include "preview.h"
#include "titles.h"

#include <QMouseEvent>
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
    // TODO: paste exact original body if you need byte-for-byte fidelity.
    updatePreviewCursor(event->position());
    QWidget::mouseMoveEvent(event);
}

void PreviewWindow::mouseReleaseEvent(QMouseEvent* event) {
    // TODO: paste exact original body if you need byte-for-byte fidelity.
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
    const qreal nextZoom = qBound<qreal>(0.25, m_previewZoom * factor, 4.0);
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
