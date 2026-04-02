#include "preview.h"

#include <QPainter>
#include <QTextDocument>

bool PreviewWindow::clipShowsTranscriptOverlay(const TimelineClip& clip) const {
    return clip.mediaType == ClipMediaType::Audio && clip.transcriptOverlay.enabled;
}

void PreviewWindow::invalidateTranscriptOverlayCache(const QString& clipFilePath) {
    if (clipFilePath.isEmpty()) {
        m_transcriptSectionsCache.clear();
    } else {
        m_transcriptSectionsCache.remove(clipFilePath);
    }
    scheduleRepaint();
}

const QVector<TranscriptSection>& PreviewWindow::transcriptSectionsForClip(const TimelineClip& clip) const {
    const QString key = clip.filePath;
    auto it = m_transcriptSectionsCache.find(key);
    if (it == m_transcriptSectionsCache.end()) {
        it = m_transcriptSectionsCache.insert(key, loadTranscriptSections(transcriptWorkingPathForClipFile(clip.filePath)));
    }
    return it.value();
}

TranscriptOverlayLayout PreviewWindow::transcriptOverlayLayoutForClip(const TimelineClip& clip) const {
    if (!clipShowsTranscriptOverlay(clip)) return {};
    const QVector<TranscriptSection>& sections = transcriptSectionsForClip(clip);
    if (sections.isEmpty()) return {};
    const int64_t sourceFrame = sourceFrameForSample(clip, m_currentSample);
    for (const TranscriptSection& section : sections) {
        if (sourceFrame < section.startFrame) return {};
        if (sourceFrame <= section.endFrame) {
            return layoutTranscriptSection(section,
                                           sourceFrame,
                                           clip.transcriptOverlay.maxCharsPerLine,
                                           clip.transcriptOverlay.maxLines,
                                           clip.transcriptOverlay.autoScroll);
        }
    }
    return {};
}

QRectF PreviewWindow::transcriptOverlayRectForTarget(const TimelineClip& clip, const QRect& targetRect) const {
    const QPointF previewScale = previewCanvasScale(targetRect);
    const QSizeF size(qMax<qreal>(40.0, clip.transcriptOverlay.boxWidth * previewScale.x()),
                      qMax<qreal>(20.0, clip.transcriptOverlay.boxHeight * previewScale.y()));
    const QPointF center(targetRect.center().x() + (clip.transcriptOverlay.translationX * previewScale.x()),
                         targetRect.center().y() + (clip.transcriptOverlay.translationY * previewScale.y()));
    return QRectF(center.x() - (size.width() / 2.0),
                  center.y() - (size.height() / 2.0),
                  size.width(),
                  size.height());
}

QSizeF PreviewWindow::transcriptOverlaySizeForSelectedClip() const {
    const PreviewOverlayInfo info = m_overlayInfo.value(m_selectedClipId);
    const QRect compositeRect = scaledCanvasRect(previewCanvasBaseRect());
    const QPointF previewScale = previewCanvasScale(compositeRect);
    return QSizeF(info.bounds.width() / qMax<qreal>(0.0001, previewScale.x()),
                  info.bounds.height() / qMax<qreal>(0.0001, previewScale.y()));
}

void PreviewWindow::drawTranscriptOverlay(QPainter* painter, const TimelineClip& clip, const QRect& targetRect) {
    const TranscriptOverlayLayout overlayLayout = transcriptOverlayLayoutForClip(clip);
    if (overlayLayout.lines.isEmpty()) return;
    const QRectF bounds = transcriptOverlayRectForTarget(clip, targetRect);
    const QRectF textBounds = bounds.adjusted(18.0, 14.0, -18.0, -14.0);
    const QColor highlightFillColor(QStringLiteral("#fff2a8"));
    const QColor highlightTextColor(QStringLiteral("#181818"));
    const QString shadowHtml = transcriptOverlayHtml(overlayLayout, QColor(0, 0, 0, 200), QColor(0, 0, 0, 200), QColor(0, 0, 0, 0));
    const QString textHtml = transcriptOverlayHtml(overlayLayout, clip.transcriptOverlay.textColor, highlightTextColor, highlightFillColor);
    if (textHtml.isEmpty()) return;

    painter->save();
    painter->setPen(Qt::NoPen);
    painter->setBrush(QColor(0, 0, 0, 120));
    painter->drawRoundedRect(bounds, 14.0, 14.0);
    QFont font(clip.transcriptOverlay.fontFamily);
    font.setPixelSize(qMax(8, qRound(clip.transcriptOverlay.fontPointSize * previewCanvasScale(targetRect).y())));
    font.setBold(clip.transcriptOverlay.bold);
    font.setItalic(clip.transcriptOverlay.italic);
    QTextDocument shadowDoc;
    shadowDoc.setDefaultFont(font);
    shadowDoc.setDocumentMargin(0.0);
    shadowDoc.setTextWidth(textBounds.width());
    shadowDoc.setHtml(shadowHtml);
    QTextDocument textDoc;
    textDoc.setDefaultFont(font);
    textDoc.setDocumentMargin(0.0);
    textDoc.setTextWidth(textBounds.width());
    textDoc.setHtml(textHtml);
    const qreal textY = textBounds.top() + qMax<qreal>(0.0, (textBounds.height() - textDoc.size().height()) / 2.0);
    painter->translate(textBounds.left() + 3.0, textY + 3.0);
    shadowDoc.drawContents(painter);
    painter->translate(-3.0, -3.0);
    textDoc.drawContents(painter);
    painter->restore();

    PreviewOverlayInfo info;
    info.kind = PreviewOverlayKind::TranscriptOverlay;
    info.bounds = bounds;
    constexpr qreal kHandleSize = 12.0;
    info.rightHandle = QRectF(bounds.right() - kHandleSize, bounds.center().y() - kHandleSize, kHandleSize, kHandleSize * 2.0);
    info.bottomHandle = QRectF(bounds.center().x() - kHandleSize, bounds.bottom() - kHandleSize, kHandleSize * 2.0, kHandleSize);
    info.cornerHandle = QRectF(bounds.right() - kHandleSize * 1.5, bounds.bottom() - kHandleSize * 1.5, kHandleSize * 1.5, kHandleSize * 1.5);
    m_overlayInfo.insert(clip.id, info);
    m_paintOrder.push_back(clip.id);
}
