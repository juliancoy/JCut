#include "preview.h"
#include "gl_frame_texture_shared.h"

#include <cmath>
#include <QPainter>
#include <QTextDocument>

namespace {
bool clipSupportsTranscript(const TimelineClip& clip) {
    return clip.mediaType == ClipMediaType::Audio || clip.hasAudio;
}

int effectiveTranscriptLinesForBox(const TimelineClip& clip) {
    const qreal estimatedLineHeight = qMax<qreal>(12.0, clip.transcriptOverlay.fontPointSize * 1.35);
    const qreal usableHeight = qMax<qreal>(estimatedLineHeight, clip.transcriptOverlay.boxHeight - 28.0);
    const int fittedLines = qMax(1, static_cast<int>(std::floor(usableHeight / estimatedLineHeight)));
    return qMax(1, qMin(clip.transcriptOverlay.maxLines, fittedLines));
}

int effectiveTranscriptCharsForBox(const TimelineClip& clip) {
    const qreal estimatedCharWidth = qMax<qreal>(6.0, clip.transcriptOverlay.fontPointSize * 0.62);
    const qreal usableWidth = qMax<qreal>(estimatedCharWidth, clip.transcriptOverlay.boxWidth - 36.0);
    const int fittedChars = qMax(1, static_cast<int>(std::floor(usableWidth / estimatedCharWidth)));
    return qMax(1, qMin(clip.transcriptOverlay.maxCharsPerLine, fittedChars));
}
}

bool PreviewWindow::clipShowsTranscriptOverlay(const TimelineClip& clip) const {
    return clipSupportsTranscript(clip) && clip.transcriptOverlay.enabled;
}

void PreviewWindow::invalidateTranscriptOverlayCache(const QString& clipFilePath) {
    if (clipFilePath.isEmpty()) {
        m_transcriptSectionsCache.clear();
        invalidateTranscriptSpeakerProfileCache();
        for (auto it = m_transcriptTextureCache.begin(); it != m_transcriptTextureCache.end(); ++it) {
            editor::destroyGlTextureEntry(&it.value());
        }
        m_transcriptTextureCache.clear();
    } else {
        m_transcriptSectionsCache.remove(clipFilePath);
        invalidateTranscriptSpeakerProfileCache(activeTranscriptPathForClipFile(clipFilePath));
        // Textures are content/style keyed; clear all to avoid keeping stale overlay textures.
        for (auto it = m_transcriptTextureCache.begin(); it != m_transcriptTextureCache.end(); ++it) {
            editor::destroyGlTextureEntry(&it.value());
        }
        m_transcriptTextureCache.clear();
    }
    scheduleRepaint();
}

const QVector<TranscriptSection>& PreviewWindow::transcriptSectionsForClip(const TimelineClip& clip) const {
    const QString key = clip.filePath;
    auto it = m_transcriptSectionsCache.find(key);
    if (it == m_transcriptSectionsCache.end()) {
        it = m_transcriptSectionsCache.insert(key, loadTranscriptSections(activeTranscriptPathForClipFile(clip.filePath)));
    }
    return it.value();
}

TranscriptOverlayLayout PreviewWindow::transcriptOverlayLayoutForClip(const TimelineClip& clip) const {
    if (!clipShowsTranscriptOverlay(clip)) return {};
    const QVector<TranscriptSection>& sections = transcriptSectionsForClip(clip);
    if (sections.isEmpty()) return {};
    const int64_t sourceFrame =
        transcriptFrameForClipAtTimelineSample(clip, m_currentSample, m_renderSyncMarkers);
    for (const TranscriptSection& section : sections) {
        if (sourceFrame < section.startFrame) return {};
        if (sourceFrame <= section.endFrame) {
            return layoutTranscriptSection(section,
                                           sourceFrame,
                                           effectiveTranscriptCharsForBox(clip),
                                           effectiveTranscriptLinesForBox(clip),
                                           clip.transcriptOverlay.autoScroll);
        }
    }
    return {};
}

QRectF PreviewWindow::transcriptOverlayRectForTarget(const TimelineClip& clip, const QRect& targetRect) const {
    const QPointF previewScale = previewCanvasScale(targetRect);
    qreal translationX = clip.transcriptOverlay.translationX;
    qreal translationY = clip.transcriptOverlay.translationY;
    const QString transcriptPath = activeTranscriptPathForClipFile(clip.filePath);
    if (!transcriptPath.isEmpty()) {
        const QVector<TranscriptSection>& sections = transcriptSectionsForClip(clip);
        if (!sections.isEmpty()) {
            const int64_t sourceFrame =
                transcriptFrameForClipAtTimelineSample(clip, m_currentSample, m_renderSyncMarkers);
            bool speakerLocationResolved = false;
            const QPointF speakerLocation = transcriptSpeakerLocationForSourceFrame(
                transcriptPath, sections, sourceFrame, &speakerLocationResolved);
            if (speakerLocationResolved) {
                const qreal centerX = speakerLocation.x() * static_cast<qreal>(targetRect.width());
                const qreal centerY = speakerLocation.y() * static_cast<qreal>(targetRect.height());
                translationX = centerX - (targetRect.width() / 2.0);
                translationY = centerY - (targetRect.height() / 2.0);
            }
        }
    }
    const QSizeF size(qMax<qreal>(40.0, clip.transcriptOverlay.boxWidth * previewScale.x()),
                      qMax<qreal>(20.0, clip.transcriptOverlay.boxHeight * previewScale.y()));
    const QPointF center(targetRect.center().x() + (translationX * previewScale.x()),
                         targetRect.center().y() + (translationY * previewScale.y()));
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
    if (clip.transcriptOverlay.showBackground) {
        painter->setPen(Qt::NoPen);
        painter->setBrush(QColor(0, 0, 0, 120));
        painter->drawRoundedRect(bounds, 14.0, 14.0);
    }
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
    const qreal widthScale = textDoc.size().width() > textBounds.width()
        ? textBounds.width() / textDoc.size().width()
        : 1.0;
    const qreal heightScale = textDoc.size().height() > textBounds.height()
        ? textBounds.height() / textDoc.size().height()
        : 1.0;
    const qreal docScale = qMin(widthScale, heightScale);
    const qreal scaledDocHeight = textDoc.size().height() * docScale;
    const qreal textY = textBounds.top() + qMax<qreal>(0.0, (textBounds.height() - scaledDocHeight) / 2.0);
    painter->translate(textBounds.left() + 3.0, textY + 3.0);
    if (docScale < 0.999) {
        painter->scale(docScale, docScale);
    }
    shadowDoc.drawContents(painter);
    if (docScale < 0.999) {
        painter->scale(1.0 / docScale, 1.0 / docScale);
    }
    painter->translate(-3.0, -3.0);
    if (docScale < 0.999) {
        painter->scale(docScale, docScale);
    }
    textDoc.drawContents(painter);
    painter->restore();

    if (m_transcriptOverlayInteractionEnabled) {
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
}
