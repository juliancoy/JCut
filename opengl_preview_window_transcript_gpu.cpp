// OpenGL Render Path File
// This file is OpenGL-specific and renders transcript overlays via GL.
#include "opengl_preview.h"

#include "gl_frame_texture_shared.h"
#include "opengl_preview_debug.h"

#include <QCryptographicHash>
#include <QOpenGLShaderProgram>
#include <QPainter>
#include <QTextDocument>

namespace {
constexpr int kMaxTranscriptTextureCacheEntries = 128;

void setFontPixelSizeRobust(QFont* font, qreal pixelSize, const QPaintDevice* device) {
    if (!font) {
        return;
    }
    const qreal dpiY = (device && device->logicalDpiY() > 0) ? device->logicalDpiY() : 96.0;
    font->setPointSizeF((pixelSize * 72.0) / dpiY);
}

QString transcriptSpeakerTitleHtml(const QString& title, const QColor& color) {
    const QString safeTitle = title.trimmed().toHtmlEscaped();
    if (safeTitle.isEmpty()) {
        return QString();
    }
    return QStringLiteral(
               "<div style=\"text-align:center;"
               " font-weight:700;"
               " letter-spacing:0.02em;"
               " font-size:0.62em;"
               " margin:0 0 0.30em 0;"
               " color:%1;\">%2</div>")
        .arg(color.name(QColor::HexRgb), safeTitle);
}
}

QString PreviewWindow::transcriptOverlayTextureKey(const TimelineClip& clip,
                                                   const QRectF& bounds,
                                                   const QRectF& textBounds,
                                                   qreal fontPixelSize,
                                                   const QString& shadowHtml,
                                                   const QString& textHtml) const {
    const QString keyMaterial =
        clip.id + QLatin1Char('|') +
        QString::number(qRound(bounds.width())) + QLatin1Char('|') +
        QString::number(qRound(bounds.height())) + QLatin1Char('|') +
        QString::number(qRound(textBounds.width())) + QLatin1Char('|') +
        QString::number(qRound(textBounds.height())) + QLatin1Char('|') +
        clip.transcriptOverlay.fontFamily + QLatin1Char('|') +
        QString::number(fontPixelSize, 'f', 3) + QLatin1Char('|') +
        (clip.transcriptOverlay.bold ? QStringLiteral("1") : QStringLiteral("0")) + QLatin1Char('|') +
        (clip.transcriptOverlay.italic ? QStringLiteral("1") : QStringLiteral("0")) + QLatin1Char('|') +
        (clip.transcriptOverlay.showBackground ? QStringLiteral("1") : QStringLiteral("0")) + QLatin1Char('|') +
        shadowHtml + QLatin1Char('|') + textHtml;
    const QByteArray digest = QCryptographicHash::hash(keyMaterial.toUtf8(), QCryptographicHash::Sha1);
    return QString::fromLatin1(digest.toHex());
}

QImage PreviewWindow::renderTranscriptOverlayImage(const TimelineClip& clip,
                                                   const QRectF& bounds,
                                                   const QRectF& textBounds,
                                                   qreal fontPixelSize,
                                                   const QString& shadowHtml,
                                                   const QString& textHtml) const {
    const int imageWidth = qMax(1, qRound(bounds.width()));
    const int imageHeight = qMax(1, qRound(bounds.height()));
    QImage image(imageWidth, imageHeight, QImage::Format_RGBA8888_Premultiplied);
    image.fill(Qt::transparent);

    QPainter painter(&image);
    painter.setRenderHint(QPainter::Antialiasing, true);
    painter.setRenderHint(QPainter::TextAntialiasing, true);

    if (clip.transcriptOverlay.showBackground) {
        painter.setPen(Qt::NoPen);
        painter.setBrush(QColor(0, 0, 0, 120));
        painter.drawRoundedRect(QRectF(0.0, 0.0, imageWidth, imageHeight), 14.0, 14.0);
    }

    QFont font(clip.transcriptOverlay.fontFamily);
    if (fontPixelSize <= 0.0) {
        return image;
    }
    setFontPixelSizeRobust(&font, fontPixelSize, painter.device());
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

    painter.translate(textBounds.left() + 3.0, textY + 3.0);
    if (docScale < 0.999) {
        painter.scale(docScale, docScale);
    }
    shadowDoc.drawContents(&painter);
    if (docScale < 0.999) {
        painter.scale(1.0 / docScale, 1.0 / docScale);
    }
    painter.translate(-3.0, -3.0);
    if (docScale < 0.999) {
        painter.scale(docScale, docScale);
    }
    textDoc.drawContents(&painter);
    painter.end();

    return image;
}

GLuint PreviewWindow::textureForTranscriptOverlay(const QString& key, const QImage& image) {
    if (key.isEmpty() || image.isNull()) {
        return 0;
    }
    editor::GlTextureCacheEntry entry = m_transcriptTextureCache.value(key);
    if (entry.textureId != 0 && entry.size == image.size()) {
        entry.lastUsedMs = nowMs();
        m_transcriptTextureCache.insert(key, entry);
        return entry.textureId;
    }

    editor::destroyGlTextureEntry(&entry);
    glGenTextures(1, &entry.textureId);
    glBindTexture(GL_TEXTURE_2D, entry.textureId);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    glTexImage2D(GL_TEXTURE_2D,
                 0,
                 GL_RGBA,
                 image.width(),
                 image.height(),
                 0,
                 GL_RGBA,
                 GL_UNSIGNED_BYTE,
                 image.constBits());
    glBindTexture(GL_TEXTURE_2D, 0);

    entry.size = image.size();
    entry.lastUsedMs = nowMs();
    m_transcriptTextureCache.insert(key, entry);
    trimTranscriptTextureCache();
    return entry.textureId;
}

void PreviewWindow::trimTranscriptTextureCache() {
    editor::trimGlTextureCache(&m_transcriptTextureCache, kMaxTranscriptTextureCacheEntries);
}

void PreviewWindow::drawTranscriptOverlayGL(const TimelineClip& clip, const QRect& targetRect) {
    if (!m_overlayShaderProgram || !m_quadBuffer.isCreated()) {
        return;
    }
    const TranscriptOverlayLayout overlayLayout = transcriptOverlayLayoutForClip(clip);
    if (overlayLayout.lines.isEmpty()) {
        return;
    }

    const QSize outputSize = m_interaction.outputSize.isValid() ? m_interaction.outputSize : QSize(1080, 1920);
    const QString transcriptPath = activeTranscriptPathForClipFile(clip.filePath);
    const QVector<TranscriptSection>& sections = transcriptSectionsForClip(clip);
    const int64_t sourceFrame =
        transcriptFrameForClipAtTimelineSample(clip, m_interaction.currentSample, m_interaction.renderSyncMarkers);
    const QRectF outputRect = transcriptOverlayRectInOutputSpace(
        clip, outputSize, transcriptPath, sections, sourceFrame);
    if (outputRect.width() <= 0.0 || outputRect.height() <= 0.0) {
        return;
    }
    const QRectF bounds = transcriptOverlayRectForTarget(clip, targetRect);
    if (bounds.width() <= 0.0 || bounds.height() <= 0.0) {
        return;
    }
    const QRectF localBounds(0.0, 0.0, outputRect.width(), outputRect.height());
    const QRectF localTextBounds = localBounds.adjusted(18.0, 14.0, -18.0, -14.0);
    if (localTextBounds.width() <= 0.0 || localTextBounds.height() <= 0.0) {
        return;
    }
    const qreal fontPixelSize = clip.transcriptOverlay.fontPointSize;
    if (fontPixelSize <= 0.0) {
        return;
    }
    const QColor highlightFillColor(QStringLiteral("#fff2a8"));
    const QColor highlightTextColor(QStringLiteral("#181818"));
    QString titleShadowHtml;
    QString titleTextHtml;
    if (clip.transcriptOverlay.showSpeakerTitle) {
        const QString titleText = transcriptSpeakerTitleForSourceFrame(transcriptPath, sections, sourceFrame);
        titleShadowHtml = transcriptSpeakerTitleHtml(titleText, QColor(0, 0, 0, 200));
        titleTextHtml = transcriptSpeakerTitleHtml(titleText, clip.transcriptOverlay.textColor);
    }
    const QString shadowHtml = titleShadowHtml + transcriptOverlayHtml(
        overlayLayout, QColor(0, 0, 0, 200), QColor(0, 0, 0, 200), QColor(0, 0, 0, 0));
    const QString textHtml = titleTextHtml + transcriptOverlayHtml(
        overlayLayout, clip.transcriptOverlay.textColor, highlightTextColor, highlightFillColor);
    if (textHtml.isEmpty()) {
        return;
    }

    const QString textureKey =
        transcriptOverlayTextureKey(clip, bounds, localTextBounds, fontPixelSize, shadowHtml, textHtml);
    const QImage image = renderTranscriptOverlayImage(
        clip,
        localBounds,
        localTextBounds,
        fontPixelSize,
        shadowHtml,
        textHtml);
    const GLuint textureId = textureForTranscriptOverlay(textureKey, image);
    if (textureId == 0) {
        return;
    }

    QMatrix4x4 projection;
    projection.ortho(0.0f, static_cast<float>(width()),
                     static_cast<float>(height()), 0.0f,
                     -1.0f, 1.0f);
    QMatrix4x4 model;
    model.translate(bounds.center().x(), bounds.center().y());
    model.scale(bounds.width(), bounds.height(), 1.0f);

    m_overlayShaderProgram->bind();
    m_overlayShaderProgram->setUniformValue("u_mvp", projection * model);
    m_overlayShaderProgram->setUniformValue("u_texture", 0);

    m_quadBuffer.bind();
    const int positionLoc = m_overlayShaderProgram->attributeLocation("a_position");
    const int texCoordLoc = m_overlayShaderProgram->attributeLocation("a_texCoord");
    m_overlayShaderProgram->enableAttributeArray(positionLoc);
    m_overlayShaderProgram->setAttributeBuffer(positionLoc, GL_FLOAT, 0, 2, 4 * sizeof(GLfloat));
    m_overlayShaderProgram->enableAttributeArray(texCoordLoc);
    m_overlayShaderProgram->setAttributeBuffer(texCoordLoc, GL_FLOAT, 2 * sizeof(GLfloat), 2, 4 * sizeof(GLfloat));

    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, textureId);
    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
    glBindTexture(GL_TEXTURE_2D, 0);
    m_quadBuffer.release();
    m_overlayShaderProgram->disableAttributeArray(texCoordLoc);
    m_overlayShaderProgram->disableAttributeArray(positionLoc);
    m_overlayShaderProgram->release();

    if (m_interaction.transcriptOverlayInteractionEnabled) {
        PreviewOverlayInfo info;
        info.kind = PreviewOverlayKind::TranscriptOverlay;
        info.bounds = bounds;
        constexpr qreal kHandleSize = 12.0;
        info.rightHandle = QRectF(bounds.right() - kHandleSize, bounds.center().y() - kHandleSize, kHandleSize, kHandleSize * 2.0);
        info.bottomHandle = QRectF(bounds.center().x() - kHandleSize, bounds.bottom() - kHandleSize, kHandleSize * 2.0, kHandleSize);
        info.cornerHandle = QRectF(bounds.right() - kHandleSize * 1.5, bounds.bottom() - kHandleSize * 1.5, kHandleSize * 1.5, kHandleSize * 1.5);
        m_overlayModel.overlays.insert(clip.id, info);
        m_overlayModel.paintOrder.push_back(clip.id);
    }
}
