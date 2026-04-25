#include "preview.h"
#include "gl_frame_texture_shared.h"

#include <cmath>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonParseError>
#include <QJsonArray>
#include <QJsonObject>
#include <QPainter>
#include <QTextDocument>

#include <algorithm>
#include <cstdlib>
#include <limits>

namespace {
bool clipSupportsTranscript(const TimelineClip& clip) {
    return clip.mediaType == ClipMediaType::Audio || clip.hasAudio;
}
}

bool PreviewWindow::clipShowsTranscriptOverlay(const TimelineClip& clip) const {
    return clipSupportsTranscript(clip) && clip.transcriptOverlay.enabled;
}

void PreviewWindow::invalidateTranscriptOverlayCache(const QString& clipFilePath) {
    if (clipFilePath.isEmpty()) {
        m_transcriptSectionsCache.clear();
        m_speakerTrackPointsCache.clear();
        invalidateTranscriptSpeakerProfileCache();
        for (auto it = m_transcriptTextureCache.begin(); it != m_transcriptTextureCache.end(); ++it) {
            editor::destroyGlTextureEntry(&it.value());
        }
        m_transcriptTextureCache.clear();
    } else {
        m_transcriptSectionsCache.remove(clipFilePath);
        m_speakerTrackPointsCache.remove(activeTranscriptPathForClipFile(clipFilePath));
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

const QVector<PreviewWindow::SpeakerTrackPoint>& PreviewWindow::speakerTrackPointsForClip(const TimelineClip& clip) const {
    static const QVector<SpeakerTrackPoint> kEmpty;
    if (!clipSupportsTranscript(clip) || clip.filePath.isEmpty()) {
        return kEmpty;
    }
    const QString transcriptPath = activeTranscriptPathForClipFile(clip.filePath);
    if (transcriptPath.isEmpty()) {
        return kEmpty;
    }

    const QFileInfo info(transcriptPath);
    if (!info.exists() || !info.isFile()) {
        return kEmpty;
    }
    const qint64 mtimeMs = info.lastModified().toMSecsSinceEpoch();
    auto it = m_speakerTrackPointsCache.find(transcriptPath);
    if (it != m_speakerTrackPointsCache.end() && it->mtimeMs == mtimeMs) {
        return it->points;
    }

    SpeakerTrackPointCacheEntry entry;
    entry.mtimeMs = mtimeMs;
    QFile file(transcriptPath);
    if (!file.open(QIODevice::ReadOnly)) {
        it = m_speakerTrackPointsCache.insert(transcriptPath, entry);
        return it->points;
    }
    QJsonParseError parseError;
    const QJsonDocument doc = QJsonDocument::fromJson(file.readAll(), &parseError);
    if (parseError.error != QJsonParseError::NoError || !doc.isObject()) {
        it = m_speakerTrackPointsCache.insert(transcriptPath, entry);
        return it->points;
    }

    const QJsonObject profiles = doc.object().value(QStringLiteral("speaker_profiles")).toObject();
    for (auto profileIt = profiles.constBegin(); profileIt != profiles.constEnd(); ++profileIt) {
        const QString speakerId = profileIt.key().trimmed();
        if (speakerId.isEmpty()) {
            continue;
        }
        const QJsonObject profile = profileIt.value().toObject();
        QJsonObject tracking = profile.value(QStringLiteral("framing")).toObject();
        if (tracking.isEmpty()) {
            tracking = profile.value(QStringLiteral("tracking")).toObject();
        }
        const QJsonArray keyframes = tracking.value(QStringLiteral("keyframes")).toArray();
        for (const QJsonValue& value : keyframes) {
            const QJsonObject obj = value.toObject();
            if (obj.isEmpty()) {
                continue;
            }
            const int64_t frame = obj.value(QStringLiteral("frame")).toVariant().toLongLong();
            const qreal rawX = obj.value(QStringLiteral("x")).toDouble(-1.0);
            const qreal rawY = obj.value(QStringLiteral("y")).toDouble(-1.0);
            if (frame < 0 || rawX < 0.0 || rawX > 1.0 || rawY < 0.0 || rawY > 1.0) {
                continue;
            }
            SpeakerTrackPoint p;
            p.frame = frame;
            p.x = rawX;
            p.y = rawY;
            p.speakerId = speakerId;
            entry.points.push_back(p);
        }
    }

    std::sort(entry.points.begin(), entry.points.end(), [](const SpeakerTrackPoint& a, const SpeakerTrackPoint& b) {
        if (a.frame != b.frame) {
            return a.frame < b.frame;
        }
        return a.speakerId < b.speakerId;
    });
    it = m_speakerTrackPointsCache.insert(transcriptPath, entry);
    return it->points;
}

void PreviewWindow::drawSpeakerTrackPointsOverlay(QPainter* painter, const QList<TimelineClip>& activeClips) {
    if (!painter || activeClips.isEmpty()) {
        return;
    }
    painter->save();
    painter->setRenderHint(QPainter::Antialiasing, true);

    for (const TimelineClip& clip : activeClips) {
        if (!clipSupportsTranscript(clip)) {
            continue;
        }
        const PreviewOverlayInfo info = m_overlayInfo.value(clip.id);
        if (!info.bounds.isValid()) {
            continue;
        }
        const QVector<SpeakerTrackPoint>& points = speakerTrackPointsForClip(clip);
        if (points.isEmpty()) {
            continue;
        }
        const int64_t sourceStart = qMax<int64_t>(0, clip.sourceInFrame);
        const int64_t sourceEnd = sourceStart + qMax<int64_t>(0, clip.durationFrames - 1);
        const int64_t currentSourceFrame =
            transcriptFrameForClipAtTimelineSample(clip, m_currentSample, m_renderSyncMarkers);
        int64_t nearestDistance = std::numeric_limits<int64_t>::max();
        QPointF nearestPoint;
        QColor nearestColor;
        bool nearestValid = false;

        for (const SpeakerTrackPoint& p : points) {
            if (p.frame < sourceStart || p.frame > sourceEnd) {
                continue;
            }
            const uint hueHash = qHash(p.speakerId);
            const QColor color = QColor::fromHsv(static_cast<int>(hueHash % 360), 180, 245, 190);
            const QPointF screenPoint = mapNormalizedClipPointToScreen(info, QPointF(p.x, p.y));
            painter->setPen(Qt::NoPen);
            painter->setBrush(color);
            painter->drawEllipse(screenPoint, 2.7, 2.7);

            const int64_t distance = std::llabs(p.frame - currentSourceFrame);
            if (distance < nearestDistance) {
                nearestDistance = distance;
                nearestPoint = screenPoint;
                nearestColor = color;
                nearestValid = true;
            }
        }

        if (nearestValid) {
            painter->setPen(QPen(QColor(255, 255, 255, 235), 1.2));
            painter->setBrush(nearestColor);
            painter->drawEllipse(nearestPoint, 4.4, 4.4);
        }
    }

    painter->restore();
}

TranscriptOverlayLayout PreviewWindow::transcriptOverlayLayoutForClip(const TimelineClip& clip) const {
    if (!clipShowsTranscriptOverlay(clip)) return {};
    const QVector<TranscriptSection>& sections = transcriptSectionsForClip(clip);
    if (sections.isEmpty()) return {};
    const int64_t sourceFrame =
        transcriptFrameForClipAtTimelineSample(clip, m_currentSample, m_renderSyncMarkers);
    return transcriptOverlayLayoutAtSourceFrame(clip, sections, sourceFrame);
}

QRectF PreviewWindow::transcriptOverlayRectForTarget(const TimelineClip& clip, const QRect& targetRect) const {
    const QPointF previewScale = previewCanvasScale(targetRect);
    const QSize outputSize = m_outputSize.isValid() ? m_outputSize : QSize(1080, 1920);
    const qreal outputWidth = qMax<qreal>(1.0, static_cast<qreal>(outputSize.width()));
    const qreal outputHeight = qMax<qreal>(1.0, static_cast<qreal>(outputSize.height()));
    const QString transcriptPath = activeTranscriptPathForClipFile(clip.filePath);
    const QVector<TranscriptSection>& sections = transcriptSectionsForClip(clip);
    const int64_t sourceFrame =
        transcriptFrameForClipAtTimelineSample(clip, m_currentSample, m_renderSyncMarkers);
    const QRectF outputRect = transcriptOverlayRectInOutputSpace(
        clip, outputSize, transcriptPath, sections, sourceFrame);
    const QSizeF size(qMax<qreal>(40.0, outputRect.width() * previewScale.x()),
                      qMax<qreal>(20.0, outputRect.height() * previewScale.y()));
    const QPointF outputTranslation(outputRect.center().x() - (outputWidth / 2.0),
                                    outputRect.center().y() - (outputHeight / 2.0));
    const QPointF center(targetRect.center().x() + (outputTranslation.x() * previewScale.x()),
                         targetRect.center().y() + (outputTranslation.y() * previewScale.y()));
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
