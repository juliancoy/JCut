#include "preview.h"
#include "preview_debug.h"
#include "debug_controls.h"
#include "titles.h"
#include "decoder_image_io.h"
#include "waveform_service.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLinearGradient>
#include <QApplication>
#include <QMetaObject>
#include <QPainter>
#include <QTextDocument>
#include <QFile>
#include <QFileInfo>
#include <QDir>
#include <QRegularExpression>

#include <algorithm>
#include <cmath>

namespace {
constexpr int64_t kMaxHeldPresentationFrameDelta = 8;

struct HoverSpeakerProfile {
    QString speakerId;
    QString name;
    QString organization;
    QString description;
    QString imagePath;
};

struct HoverSpeakerProfileCacheEntry {
    qint64 mtimeMs = -1;
    QHash<QString, HoverSpeakerProfile> profilesBySpeaker;
};

QHash<QString, HoverSpeakerProfileCacheEntry>& hoverSpeakerProfileCache() {
    static QHash<QString, HoverSpeakerProfileCacheEntry> cache;
    return cache;
}

QHash<QString, QPixmap>& hoverSpeakerImageCache() {
    static QHash<QString, QPixmap> cache;
    return cache;
}

QString clippedSummaryFromWords(const QStringList& words) {
    if (words.isEmpty()) {
        return QStringLiteral("No transcript summary available.");
    }
    QStringList clipped;
    clipped.reserve(qMin(34, words.size()));
    for (int i = 0; i < words.size() && i < 34; ++i) {
        QString token = words.at(i).trimmed();
        token.remove(QRegularExpression(QStringLiteral("^[\\s\\p{Punct}]+|[\\s\\p{Punct}]+$")));
        if (token.isEmpty()) {
            continue;
        }
        clipped.push_back(token);
    }
    if (clipped.isEmpty()) {
        return QStringLiteral("No transcript summary available.");
    }
    QString summary = clipped.join(QLatin1Char(' '));
    if (words.size() > clipped.size()) {
        summary += QStringLiteral("...");
    }
    return summary;
}

QHash<QString, QStringList> wordsBySpeakerFromTranscriptRoot(const QJsonObject& root) {
    QHash<QString, QStringList> wordsBySpeaker;
    const QJsonArray segments = root.value(QStringLiteral("segments")).toArray();
    for (const QJsonValue& segValue : segments) {
        const QJsonObject segObj = segValue.toObject();
        const QString segmentSpeaker = segObj.value(QStringLiteral("speaker")).toString().trimmed();
        const QJsonArray words = segObj.value(QStringLiteral("words")).toArray();
        for (const QJsonValue& wordValue : words) {
            const QJsonObject wordObj = wordValue.toObject();
            if (wordObj.value(QStringLiteral("skipped")).toBool(false)) {
                continue;
            }
            QString speakerId = wordObj.value(QStringLiteral("speaker")).toString().trimmed();
            if (speakerId.isEmpty()) {
                speakerId = segmentSpeaker;
            }
            const QString token = wordObj.value(QStringLiteral("word")).toString().trimmed();
            if (!speakerId.isEmpty() && !token.isEmpty()) {
                wordsBySpeaker[speakerId].push_back(token);
            }
        }
    }
    return wordsBySpeaker;
}

const HoverSpeakerProfile* hoverSpeakerProfileFor(const QString& transcriptPath, const QString& speakerId) {
    if (transcriptPath.isEmpty() || speakerId.trimmed().isEmpty()) {
        return nullptr;
    }
    const QFileInfo info(transcriptPath);
    if (!info.exists() || !info.isFile()) {
        return nullptr;
    }
    const qint64 mtimeMs = info.lastModified().toMSecsSinceEpoch();
    HoverSpeakerProfileCacheEntry& entry = hoverSpeakerProfileCache()[transcriptPath];
    if (entry.mtimeMs != mtimeMs || entry.profilesBySpeaker.isEmpty()) {
        entry = HoverSpeakerProfileCacheEntry{};
        entry.mtimeMs = mtimeMs;
        QFile transcriptFile(transcriptPath);
        if (transcriptFile.open(QIODevice::ReadOnly)) {
            QJsonParseError parseError;
            const QJsonDocument doc = QJsonDocument::fromJson(transcriptFile.readAll(), &parseError);
            if (parseError.error == QJsonParseError::NoError && doc.isObject()) {
                const QJsonObject root = doc.object();
                const QHash<QString, QStringList> wordsBySpeaker =
                    wordsBySpeakerFromTranscriptRoot(root);
                const QJsonObject profiles = root.value(QStringLiteral("speaker_profiles")).toObject();
                QSet<QString> speakerIds;
                for (auto it = wordsBySpeaker.constBegin(); it != wordsBySpeaker.constEnd(); ++it) {
                    speakerIds.insert(it.key());
                }
                for (auto it = profiles.begin(); it != profiles.end(); ++it) {
                    speakerIds.insert(it.key());
                }
                for (const QString& id : speakerIds) {
                    const QJsonObject profileObj = profiles.value(id).toObject();
                    HoverSpeakerProfile profile;
                    profile.speakerId = id;
                    profile.name = profileObj.value(QStringLiteral("name")).toString(id).trimmed();
                    profile.organization = profileObj.value(QStringLiteral("organization")).toString().trimmed();
                    QString description =
                        profileObj.value(QStringLiteral("brief_description")).toString().trimmed();
                    if (description.isEmpty()) {
                        description = profileObj.value(QStringLiteral("description")).toString().trimmed();
                    }
                    if (description.isEmpty()) {
                        description = profileObj.value(QStringLiteral("bio")).toString().trimmed();
                    }
                    if (description.isEmpty()) {
                        description = clippedSummaryFromWords(wordsBySpeaker.value(id));
                    }
                    profile.description = description;
                    QString imagePath =
                        profileObj.value(QStringLiteral("image_path")).toString().trimmed();
                    if (imagePath.isEmpty()) {
                        imagePath = profileObj.value(QStringLiteral("avatar_path")).toString().trimmed();
                    }
                    if (imagePath.isEmpty()) {
                        imagePath = profileObj.value(QStringLiteral("photo_path")).toString().trimmed();
                    }
                    if (imagePath.isEmpty()) {
                        imagePath = profileObj.value(QStringLiteral("image")).toString().trimmed();
                    }
                    if (!imagePath.isEmpty() && QDir::isRelativePath(imagePath)) {
                        imagePath = QFileInfo(info.absolutePath(), imagePath).absoluteFilePath();
                    }
                    profile.imagePath = imagePath;
                    entry.profilesBySpeaker.insert(id, profile);
                }
            }
        }
    }
    auto it = entry.profilesBySpeaker.constFind(speakerId);
    if (it == entry.profilesBySpeaker.constEnd()) {
        return nullptr;
    }
    return &(*it);
}

QPixmap hoverSpeakerImage(const HoverSpeakerProfile& profile, int edgePx) {
    const int safeSize = qBound(28, edgePx, 192);
    if (profile.imagePath.trimmed().isEmpty()) {
        return QPixmap();
    }
    const QString cacheKey = QStringLiteral("%1|%2").arg(profile.imagePath).arg(safeSize);
    auto& cache = hoverSpeakerImageCache();
    const auto cached = cache.constFind(cacheKey);
    if (cached != cache.constEnd()) {
        return cached.value();
    }
    QPixmap pix(profile.imagePath);
    if (!pix.isNull() && (pix.width() != safeSize || pix.height() != safeSize)) {
        pix = pix.scaled(safeSize, safeSize, Qt::KeepAspectRatioByExpanding, Qt::SmoothTransformation);
    }
    cache.insert(cacheKey, pix);
    return pix;
}

QString speakerAtSourceFrame(const QVector<TranscriptSection>& sections, int64_t sourceFrame) {
    for (const TranscriptSection& section : sections) {
        if (sourceFrame < section.startFrame) {
            return QString();
        }
        if (sourceFrame > section.endFrame) {
            continue;
        }
        int bestIndex = -1;
        for (int i = 0; i < section.words.size(); ++i) {
            const TranscriptWord& word = section.words.at(i);
            if (sourceFrame >= word.startFrame && sourceFrame <= word.endFrame) {
                bestIndex = i;
                break;
            }
            if (sourceFrame > word.endFrame) {
                bestIndex = i;
            }
        }
        if (bestIndex < 0 && !section.words.isEmpty()) {
            bestIndex = 0;
        }
        if (bestIndex >= 0 && bestIndex < section.words.size()) {
            return section.words.at(bestIndex).speaker.trimmed();
        }
        return QString();
    }
    return QString();
}

QColor speakerColor(const QString& speakerId, int alpha) {
    const uint hueHash = qHash(speakerId);
    QColor color = QColor::fromHsv(static_cast<int>(hueHash % 360), 170, 185);
    color.setAlpha(qBound(0, alpha, 255));
    return color;
}

void fillShortUnknownSpeakerGaps(QVector<int>* speakerIndexByBin, int maxGapBins) {
    if (!speakerIndexByBin || speakerIndexByBin->isEmpty() || maxGapBins <= 0) {
        return;
    }
    QVector<int>& bins = *speakerIndexByBin;
    const int count = bins.size();
    int i = 0;
    while (i < count) {
        if (bins[i] >= 0) {
            ++i;
            continue;
        }
        const int start = i;
        while (i < count && bins[i] < 0) {
            ++i;
        }
        const int end = i - 1;
        const int len = end - start + 1;
        if (len > maxGapBins) {
            continue;
        }
        const int left = (start > 0) ? bins[start - 1] : -1;
        const int right = (i < count) ? bins[i] : -1;
        if (left < 0 && right < 0) {
            continue;
        }
        int fill = left;
        if (fill < 0) {
            fill = right;
        } else if (right >= 0 && right != left) {
            fill = (len <= 2) ? right : left;
        }
        for (int j = start; j <= end; ++j) {
            bins[j] = fill;
        }
    }
}
}

void PreviewWindow::paintEvent(QPaintEvent* event) {
    if (!usingCpuFallback()) {
        QOpenGLWidget::paintEvent(event);
        return;
    }

    Q_UNUSED(event)
    m_lastPaintMs = nowMs();

    QPainter painter(this);
    if (!painter.isActive()) {
        static qint64 s_lastPaintWarningMs = 0;
        const qint64 now = nowMs();
        if (now - s_lastPaintWarningMs >= 2000) {
            s_lastPaintWarningMs = now;
            qWarning() << "[PreviewWindow] CPU fallback painter is inactive; skipping paint";
        }
        return;
    }
    painter.setRenderHint(QPainter::Antialiasing, true);
    drawBackground(&painter);
    const QList<TimelineClip> activeClips = getActiveClips();
    const QRect safeRect = rect().adjusted(24, 24, -24, -24);
    drawCompositedPreview(&painter, safeRect, activeClips);
    drawPreviewChrome(&painter, safeRect, activeClips.size());
}

QRect PreviewWindow::previewCanvasBaseRect() const {
    return previewCanvasBaseRectForWidget(rect(), m_outputSize, 36);
}

QRect PreviewWindow::scaledCanvasRect(const QRect& baseRect) const {
    return scaledPreviewCanvasRect(baseRect, m_previewZoom, m_previewPanOffset);
}

QPointF PreviewWindow::previewCanvasScale(const QRect& targetRect) const {
    return previewCanvasScaleForTargetRect(targetRect, m_outputSize);
}

QPointF PreviewWindow::mapNormalizedClipPointToScreen(const PreviewOverlayInfo& info,
                                                      const QPointF& normalizedPoint) const {
    const qreal x = qBound<qreal>(0.0, normalizedPoint.x(), 1.0);
    const qreal y = qBound<qreal>(0.0, normalizedPoint.y(), 1.0);
    if (info.clipPixelSize.width() > 1.0 && info.clipPixelSize.height() > 1.0) {
        const QPointF localPoint((x - 0.5) * info.clipPixelSize.width(),
                                 (y - 0.5) * info.clipPixelSize.height());
        return info.clipTransform.map(localPoint);
    }
    return QPointF(info.bounds.left() + (x * info.bounds.width()),
                   info.bounds.top() + (y * info.bounds.height()));
}

QPointF PreviewWindow::mapScreenPointToNormalizedClip(const PreviewOverlayInfo& info,
                                                      const QPointF& screenPoint) const {
    if (info.clipPixelSize.width() > 1.0 && info.clipPixelSize.height() > 1.0 && !info.clipTransform.isIdentity()) {
        bool invertible = false;
        const QTransform inverse = info.clipTransform.inverted(&invertible);
        if (invertible) {
            const QPointF localPoint = inverse.map(screenPoint);
            return QPointF(
                qBound<qreal>(0.0, (localPoint.x() / info.clipPixelSize.width()) + 0.5, 1.0),
                qBound<qreal>(0.0, (localPoint.y() / info.clipPixelSize.height()) + 0.5, 1.0));
        }
    }
    return QPointF(
        qBound<qreal>(0.0, (screenPoint.x() - info.bounds.left()) / qMax<qreal>(1.0, info.bounds.width()), 1.0),
        qBound<qreal>(0.0, (screenPoint.y() - info.bounds.top()) / qMax<qreal>(1.0, info.bounds.height()), 1.0));
}

void PreviewWindow::drawBackground(QPainter* painter) {
    const float phase = std::fmod(static_cast<float>(m_currentFramePosition), 180.0f) / 179.0f;
    const float clipFactor = qBound(0.0f, static_cast<float>(m_clipCount) / 8.0f, 1.0f);
    const float motion = m_playing ? phase : 0.25f;

    QLinearGradient gradient(rect().topLeft(), rect().bottomRight());
    gradient.setColorAt(0.0, QColor::fromRgbF(0.08f + 0.22f * motion,
                                              0.10f + 0.18f * clipFactor,
                                              0.13f + 0.35f * (1.0f - motion),
                                              1.0f));
    gradient.setColorAt(1.0, QColor::fromRgbF(0.14f + 0.10f * clipFactor,
                                              0.07f + 0.08f * motion,
                                              0.09f + 0.25f * clipFactor,
                                              1.0f));
    painter->fillRect(rect(), gradient);
}

void PreviewWindow::drawCompositedPreviewOverlay(QPainter* painter,
                                                 const QRect& safeRect,
                                                 const QRect& compositeRect,
                                                 const QList<TimelineClip>& activeClips,
                                                 bool drewAnyFrame,
                                                 bool waitingForFrame) {
    const qint64 now = nowMs();
    if (waitingForFrame) {
        if (m_waitingForFrameSinceMs <= 0) {
            m_waitingForFrameSinceMs = now;
        }
    } else {
        m_waitingForFrameSinceMs = 0;
    }
    const bool showLoadingBadge =
        waitingForFrame && (now - m_waitingForFrameSinceMs) >= 150;

    painter->save();
    painter->setPen(QPen(QColor(255, 255, 255, 40), 1.5));
    painter->setBrush(QColor(255, 255, 255, 18));
    painter->drawRoundedRect(safeRect, 18, 18);

    QList<TimelineClip> activeAudioClips;
    for (const TimelineClip& clip : m_clips) {
        const bool includeForAudioView =
            clipAudioPlaybackEnabled(clip) &&
            (clip.id == m_selectedClipId || isSampleWithinClip(clip, m_currentSample));
        const bool includeAsFallback =
            clipIsAudioOnly(clip) && isSampleWithinClip(clip, m_currentSample);
        if (includeForAudioView || includeAsFallback) {
            activeAudioClips.push_back(clip);
        }
    }
    if (m_viewMode == ViewMode::Audio || activeClips.isEmpty()) {
        if (!activeAudioClips.isEmpty()) {
            drawAudioPlaceholder(painter, safeRect, activeAudioClips);
        } else {
            drawEmptyState(painter, safeRect);
        }
        painter->restore();
        return;
    }

    painter->setPen(QPen(QColor(255, 255, 255, 36), 1.0));
    painter->setBrush(Qt::NoBrush);
    painter->drawRoundedRect(compositeRect.adjusted(0, 0, -1, -1), 12, 12);

    if (!drewAnyFrame) {
        const TimelineClip& primaryClip = activeClips.constFirst();
        drawFramePlaceholder(painter, compositeRect, primaryClip,
                             waitingForFrame
                                 ? QStringLiteral("Frame loading...")
                                 : QStringLiteral("No composited frame available"));
    } else if (showLoadingBadge) {
        painter->save();
        painter->setPen(Qt::NoPen);
        painter->setBrush(QColor(9, 12, 16, 170));
        const QRect badgeRect(compositeRect.left() + 16, compositeRect.top() + 16, 150, 28);
        painter->drawRoundedRect(badgeRect, 10, 10);
        painter->setPen(QColor(QStringLiteral("#edf3f8")));
        painter->drawText(badgeRect.adjusted(12, 0, -12, 0),
                          Qt::AlignLeft | Qt::AlignVCenter,
                          QStringLiteral("Overlay loading..."));
        painter->restore();
    }

    if (usingCpuFallback() || !m_overlayShaderProgram) {
        for (const TimelineClip& clip : activeClips) {
            if (clipShowsTranscriptOverlay(clip)) {
                drawTranscriptOverlay(painter, clip, compositeRect);
            }
        }
    }
    if (m_showSpeakerTrackPoints || m_showSpeakerTrackBoxes) {
        drawSpeakerTrackPointsOverlay(painter, activeClips);
    }
    drawSpeakerFramingTargetOverlay(painter, activeClips, compositeRect);

    // Draw title overlays for title clips at the current playhead
    for (const TimelineClip& clip : activeClips) {
        if (clip.mediaType == ClipMediaType::Title && !clip.titleKeyframes.isEmpty()) {
            const int64_t localFrame = qMax<int64_t>(0,
                m_currentFrame - clip.startFrame);
            const EvaluatedTitle evaluatedTitle = evaluateTitleAtLocalFrame(clip, localFrame);
            const EffectiveVisualEffects effects = evaluateEffectiveVisualEffectsAtPosition(
                clip, m_tracks, m_currentFramePosition, m_renderSyncMarkers);
            const EvaluatedTitle title =
                composeTitleWithOpacity(evaluatedTitle, static_cast<qreal>(effects.grading.opacity));
            drawTitleOverlay(painter, compositeRect, title, m_outputSize);

            // Register overlay bounds so the title is draggable in the preview
            if (title.valid && !title.text.isEmpty()) {
                const qreal sx = m_outputSize.width() > 0
                    ? static_cast<qreal>(compositeRect.width()) / m_outputSize.width() : 1.0;
                const qreal sy = m_outputSize.height() > 0
                    ? static_cast<qreal>(compositeRect.height()) / m_outputSize.height() : 1.0;
                QFont font(title.fontFamily);
                font.setPointSizeF(title.fontSize * qMin(sx, sy));
                font.setBold(title.bold);
                font.setItalic(title.italic);
                const TitleLayoutMetrics metrics = measureTitleLayout(font, title.text);
                const qreal cx = compositeRect.center().x() + title.x * sx;
                const qreal cy = compositeRect.center().y() + title.y * sy;
                const qreal windowPaddingPx = title.windowEnabled
                    ? qMax<qreal>(0.0, title.windowPadding * qMin(sx, sy))
                    : 0.0;
                const qreal frameExtraPx = title.windowFrameEnabled
                    ? (qMax<qreal>(0.0, title.windowFrameGap * qMin(sx, sy)) +
                       (qMax<qreal>(0.0, title.windowFrameWidth * qMin(sx, sy)) * 0.5))
                    : 0.0;
                const QRectF bounds(cx - metrics.width / 2.0 - 4 - windowPaddingPx - frameExtraPx,
                                    cy - metrics.height / 2.0 - 4 - windowPaddingPx - frameExtraPx,
                                    metrics.width + 8 + (windowPaddingPx * 2.0) + (frameExtraPx * 2.0),
                                    metrics.height + 8 + (windowPaddingPx * 2.0) + (frameExtraPx * 2.0));
                PreviewOverlayInfo info;
                info.bounds = bounds;
                m_overlayInfo.insert(clip.id, info);
                m_paintOrder.push_back(clip.id);
            }
        }
    }

    for (const TimelineClip& clip : activeClips) {
        const PreviewOverlayInfo info = m_overlayInfo.value(clip.id);
        if (clip.id == m_selectedClipId && info.bounds.isValid()) {
            painter->setPen(QPen(QColor(QStringLiteral("#fff4c2")), 2.0));
            painter->setBrush(Qt::NoBrush);
            painter->drawRect(info.bounds);
            if (info.rightHandle.isValid()) {
                painter->setBrush(QColor(QStringLiteral("#fff4c2")));
                painter->drawRect(info.rightHandle);
                painter->drawRect(info.bottomHandle);
                painter->drawRect(info.cornerHandle);
            }
        }
    }

    if (m_showCorrectionOverlays) {
        for (const TimelineClip& clip : activeClips) {
            const PreviewOverlayInfo info = m_overlayInfo.value(clip.id);
            if (clip.id != m_selectedClipId || !info.bounds.isValid() || clip.correctionPolygons.isEmpty()) {
                continue;
            }
            QVector<TimelineClip::CorrectionPolygon> visiblePolygons;
            if (m_selectedCorrectionPolygon >= 0 &&
                m_selectedCorrectionPolygon < clip.correctionPolygons.size()) {
                const auto& selected = clip.correctionPolygons[m_selectedCorrectionPolygon];
                visiblePolygons.push_back(selected);
            }
            if (visiblePolygons.isEmpty()) {
                for (const TimelineClip::CorrectionPolygon& polygon : clip.correctionPolygons) {
                    if (polygon.pointsNormalized.size() >= 3) {
                        visiblePolygons.push_back(polygon);
                    }
                }
            }
            if (visiblePolygons.isEmpty()) {
                continue;
            }
            painter->save();
            painter->setRenderHint(QPainter::Antialiasing, true);
            painter->setPen(QPen(QColor(255, 92, 92, 220), 2.0));
            painter->setBrush(QColor(255, 92, 92, 48));
            for (const TimelineClip::CorrectionPolygon& polygon : visiblePolygons) {
                QPainterPath path;
                const QPointF first = mapNormalizedClipPointToScreen(info, polygon.pointsNormalized.constFirst());
                path.moveTo(first);
                for (int i = 1; i < polygon.pointsNormalized.size(); ++i) {
                    const QPointF point = mapNormalizedClipPointToScreen(info, polygon.pointsNormalized[i]);
                    path.lineTo(point);
                }
                path.closeSubpath();
                painter->drawPath(path);
            }
            painter->restore();
        }
    }

    if (!m_correctionDraftPoints.isEmpty() && (m_showCorrectionOverlays || m_correctionDrawMode)) {
        const PreviewOverlayInfo info = m_overlayInfo.value(m_selectedClipId);
        if (info.bounds.isValid()) {
            painter->save();
            painter->setRenderHint(QPainter::Antialiasing, true);
            painter->setPen(QPen(QColor(255, 200, 64, 230), 2.0, Qt::DashLine));
            painter->setBrush(QColor(255, 200, 64, 64));
            QPolygonF polygon;
            polygon.reserve(m_correctionDraftPoints.size());
            for (const QPointF& pointNorm : m_correctionDraftPoints) {
                polygon.push_back(mapNormalizedClipPointToScreen(info, pointNorm));
            }
            if (!polygon.isEmpty()) {
                if (polygon.size() >= 3) {
                    painter->drawPolygon(polygon);
                } else {
                    painter->drawPolyline(polygon);
                }
                painter->setPen(Qt::NoPen);
                painter->setBrush(QColor(255, 200, 64, 255));
                for (const QPointF& p : polygon) {
                    painter->drawEllipse(p, 3.0, 3.0);
                }
            }
            painter->restore();
        }
    }

    if (!activeAudioClips.isEmpty()) {
        drawAudioBadge(painter, compositeRect, activeAudioClips);
    }
    drawSpeakerPickOverlay(painter);
    painter->restore();
}

void PreviewWindow::drawCompositedPreview(QPainter* painter, const QRect& safeRect,
                           const QList<TimelineClip>& activeClips) {
    painter->save();
    m_overlayInfo.clear();
    m_paintOrder.clear();
    
    // Draw background panel
    painter->setPen(QPen(QColor(255, 255, 255, 40), 1.5));
    painter->setBrush(QColor(255, 255, 255, 18));
    painter->drawRoundedRect(safeRect, 18, 18);
    
    if (m_viewMode == ViewMode::Audio || activeClips.isEmpty()) {
        QList<TimelineClip> activeAudioClips;
        for (const TimelineClip& clip : m_clips) {
            const bool includeForAudioView =
                clipAudioPlaybackEnabled(clip) &&
                (clip.id == m_selectedClipId || isSampleWithinClip(clip, m_currentSample));
            const bool includeAsFallback =
                clipIsAudioOnly(clip) && isSampleWithinClip(clip, m_currentSample);
            if (includeForAudioView || includeAsFallback) {
                activeAudioClips.push_back(clip);
            }
        }
        if (!activeAudioClips.isEmpty()) {
            drawAudioPlaceholder(painter, safeRect, activeAudioClips);
        } else {
            drawEmptyState(painter, safeRect);
        }
        painter->restore();
        return;
    }

    const QRect compositeRect = scaledCanvasRect(previewCanvasBaseRect());
    painter->fillRect(compositeRect, Qt::black);
    if (m_hideOutsideOutputWindow) {
        painter->setClipRect(compositeRect);
    }
    bool drewAnyFrame = false;
    bool waitingForFrame = false;
    int usedPlaybackPipelineCount = 0;
    int presentationCount = 0;
    int exactCount = 0;
    int bestCount = 0;
    int heldCount = 0;
    int staleRejectedCount = 0;
    int nullCount = 0;
    QJsonArray clipSelections;

    for (const TimelineClip& clip : activeClips) {
        if (clip.mediaType == ClipMediaType::Title) {
            continue; // Title clips are drawn as text overlays below
        }
        const int64_t localFrame = sourceFrameForSample(clip, m_currentSample);
        const bool isImageSequence = clip.sourceKind == MediaSourceKind::ImageSequence &&
                                     clip.mediaType != ClipMediaType::Image;
        const bool usePlaybackPipeline = m_playing && isImageSequence;
        const bool usePlaybackBuffer = !isImageSequence && m_playing && m_cache;
        const bool allowApproximateFrame =
            m_playing &&
            (usePlaybackPipeline || !m_cache ||
             m_cache->shouldAllowApproximatePreviewFrame(clip.id, localFrame, nowMs()));
        QString selection = QStringLiteral("none");
        const FrameHandle exactFrame = usePlaybackPipeline
                                           ? m_playbackPipeline->getFrame(clip.id, localFrame)
                                           : (usePlaybackBuffer
                                                  ? m_cache->getPlaybackFrame(clip.id, localFrame)
                                                  : (m_cache ? m_cache->getCachedFrame(clip.id, localFrame) : FrameHandle()));
        FrameHandle frame;
        if (usePlaybackPipeline) {
            ++usedPlaybackPipelineCount;
            frame = m_playbackPipeline->getPresentationFrame(clip.id, localFrame);
            if (!frame.isNull()) {
                ++presentationCount;
                selection = QStringLiteral("presentation");
            }
        } else {
            frame = exactFrame;
            if (frame.isNull() && m_cache && allowApproximateFrame) {
                if (usePlaybackBuffer) {
                    frame = m_cache->getLatestPlaybackFrame(clip.id, localFrame);
                    if (frame.isNull() && editor::debugPlaybackCacheFallbackEnabled()) {
                        const FrameHandle cacheExact = m_cache->getCachedFrame(clip.id, localFrame);
                        frame = !cacheExact.isNull()
                                    ? cacheExact
                                    : m_cache->getLatestCachedFrame(clip.id, localFrame);
                    }
                } else {
                    frame = m_playing
                                ? m_cache->getLatestCachedFrame(clip.id, localFrame)
                                : m_cache->getBestCachedFrame(clip.id, localFrame);
                }
            }
            if (!frame.isNull()) {
                if (!exactFrame.isNull() && frame == exactFrame) {
                    ++exactCount;
                    selection = QStringLiteral("exact");
                } else {
                    ++bestCount;
                    selection = m_playing ? QStringLiteral("latest") : QStringLiteral("best");
                }
            }
        }
        if (usePlaybackPipeline && frame.isNull()) {
            frame = !exactFrame.isNull() ? exactFrame
                                         : m_playbackPipeline->getBestFrame(clip.id, localFrame);
            if (!frame.isNull()) {
                if (!exactFrame.isNull() && frame == exactFrame) {
                    ++exactCount;
                    selection = QStringLiteral("exact");
                } else {
                    ++bestCount;
                    selection = QStringLiteral("best");
                }
            }
        }
        if (usePlaybackPipeline && frame.isNull() && m_cache) {
            const FrameHandle cacheExact = m_cache->getCachedFrame(clip.id, localFrame);
            frame = !cacheExact.isNull()
                        ? cacheExact
                        : (m_playing
                               ? m_cache->getLatestCachedFrame(clip.id, localFrame)
                               : m_cache->getBestCachedFrame(clip.id, localFrame));
            if (!frame.isNull()) {
                if (!cacheExact.isNull() && frame == cacheExact) {
                    ++exactCount;
                    selection = QStringLiteral("exact");
                } else {
                    ++bestCount;
                    selection = m_playing ? QStringLiteral("latest") : QStringLiteral("best");
                }
            }
        }
        if (usePlaybackPipeline) {
            if (!frame.isNull()) {
                m_lastPresentedFrames.insert(clip.id, frame);
            } else {
                const FrameHandle heldFrame = m_lastPresentedFrames.value(clip.id);
                if (!heldFrame.isNull() &&
                    qAbs(heldFrame.frameNumber() - localFrame) <= kMaxHeldPresentationFrameDelta) {
                    frame = heldFrame;
                    ++heldCount;
                    selection = QStringLiteral("held");
                } else {
                    m_lastPresentedFrames.remove(clip.id);
                }
            }
        }
        if (isFrameTooStaleForPlayback(clip, localFrame, frame)) {
            frame = FrameHandle();
            selection = QStringLiteral("stale");
            ++staleRejectedCount;
            m_lastPresentedFrames.remove(clip.id);
        }
        playbackFrameSelectionTrace(QStringLiteral("PreviewWindow::drawCompositedPreview.select"),
                                    clip,
                                    localFrame,
                                    exactFrame,
                                    frame,
                                    m_currentFramePosition);
        if (frame.isNull()) {
            if (usePlaybackPipeline && m_playbackPipeline && m_playing) {
                static constexpr int kMaxVisibleBacklog = 4;
                if (m_playbackPipeline->pendingVisibleRequestCount() < kMaxVisibleBacklog) {
                    m_lastFrameRequestMs = nowMs();
                    m_playbackPipeline->requestFramesForSample(
                        m_currentSample,
                        [this]() {
                            QMetaObject::invokeMethod(this, [this]() {
                                scheduleRepaint();
                            }, Qt::QueuedConnection);
                        });
                }
            } else if (m_cache &&
                       (!m_cache->isVisibleRequestPending(clip.id, localFrame) ||
                        m_cache->shouldForceVisibleRequestRetry(clip.id, localFrame, 250))) {
                m_lastFrameRequestMs = nowMs();
                m_cache->requestFrame(
                    clip.id,
                    localFrame,
                    [this](FrameHandle delivered) {
                        Q_UNUSED(delivered)
                        QMetaObject::invokeMethod(this, [this]() {
                            scheduleRepaint();
                        }, Qt::QueuedConnection);
                    });
            }

            // For static images, try to load them synchronously as a last resort
            if (clip.mediaType == ClipMediaType::Image && !clip.filePath.isEmpty()) {
                // Try to load the image synchronously
                QImage image = editor::loadSingleImageFile(clip.filePath);
                if (!image.isNull()) {
                    // Create a frame handle from the loaded image
                    frame = FrameHandle::createCpuFrame(image, localFrame, clip.filePath);
                    // Store it in cache for future use
                    if (m_cache) {
                        m_cache->requestFrame(clip.id, localFrame, [](FrameHandle){});
                    }
                    selection = QStringLiteral("sync-loaded");
                }
            }
            
            if (frame.isNull()) {
                ++nullCount;
                selection = QStringLiteral("null");
                waitingForFrame = true;
                clipSelections.append(QJsonObject{
                    {QStringLiteral("id"), clip.id},
                    {QStringLiteral("label"), clip.label},
                    {QStringLiteral("media_type"), clipMediaTypeLabel(clip.mediaType)},
                    {QStringLiteral("source_kind"), mediaSourceKindLabel(clip.sourceKind)},
                    {QStringLiteral("playback_pipeline"), usePlaybackPipeline},
                    {QStringLiteral("local_frame"), static_cast<qint64>(localFrame)},
                    {QStringLiteral("selection"), selection}
                });
                continue;
            }
        }
        clipSelections.append(QJsonObject{
            {QStringLiteral("id"), clip.id},
            {QStringLiteral("label"), clip.label},
            {QStringLiteral("media_type"), clipMediaTypeLabel(clip.mediaType)},
            {QStringLiteral("source_kind"), mediaSourceKindLabel(clip.sourceKind)},
            {QStringLiteral("playback_pipeline"), usePlaybackPipeline},
            {QStringLiteral("local_frame"), static_cast<qint64>(localFrame)},
            {QStringLiteral("frame_number"), static_cast<qint64>(frame.frameNumber())},
            {QStringLiteral("selection"), selection}
        });
        drawFrameLayer(painter, compositeRect, clip, frame);
        drewAnyFrame = true;
    }

    m_lastFrameSelectionStats = QJsonObject{
        {QStringLiteral("path"), QStringLiteral("cpu")},
        {QStringLiteral("active_visual_clips"), activeClips.size()},
        {QStringLiteral("use_playback_pipeline_clips"), usedPlaybackPipelineCount},
        {QStringLiteral("presentation"), presentationCount},
        {QStringLiteral("exact"), exactCount},
        {QStringLiteral("best"), bestCount},
        {QStringLiteral("held"), heldCount},
        {QStringLiteral("stale_rejected"), staleRejectedCount},
        {QStringLiteral("null"), nullCount},
        {QStringLiteral("clips"), clipSelections}
    };

    if (!drewAnyFrame) {
        const TimelineClip& primaryClip = activeClips.constFirst();
        drawFramePlaceholder(painter, compositeRect, primaryClip,
                             waitingForFrame
                                 ? QStringLiteral("Frame loading...")
                                 : QStringLiteral("No composited frame available"));
    } else if (waitingForFrame) {
        painter->save();
        painter->setPen(Qt::NoPen);
        painter->setBrush(QColor(9, 12, 16, 170));
        const QRect badgeRect(compositeRect.left() + 16, compositeRect.top() + 16, 150, 28);
        painter->drawRoundedRect(badgeRect, 10, 10);
        painter->setPen(QColor(QStringLiteral("#edf3f8")));
        painter->drawText(badgeRect.adjusted(12, 0, -12, 0),
                          Qt::AlignLeft | Qt::AlignVCenter,
                          QStringLiteral("Overlay loading..."));
        painter->restore();
    }

    QList<TimelineClip> activeAudioClips;
    for (const TimelineClip& clip : m_clips) {
        if (clipIsAudioOnly(clip) && isSampleWithinClip(clip, m_currentSample)) {
            activeAudioClips.push_back(clip);
        }
    }
    if (!activeAudioClips.isEmpty()) {
        drawAudioBadge(painter, compositeRect, activeAudioClips);
    }
    for (const TimelineClip& clip : activeClips) {
        if (clipShowsTranscriptOverlay(clip)) {
            drawTranscriptOverlay(painter, clip, compositeRect);
        }
    }
    // Draw title overlays and register their bounds for drag interaction
    for (const TimelineClip& clip : activeClips) {
        if (clip.mediaType == ClipMediaType::Title && !clip.titleKeyframes.isEmpty()) {
            const int64_t localFrame = qMax<int64_t>(0, m_currentFrame - clip.startFrame);
            const EvaluatedTitle evaluatedTitle = evaluateTitleAtLocalFrame(clip, localFrame);
            const EffectiveVisualEffects effects = evaluateEffectiveVisualEffectsAtPosition(
                clip, m_tracks, m_currentFramePosition, m_renderSyncMarkers);
            const EvaluatedTitle title =
                composeTitleWithOpacity(evaluatedTitle, static_cast<qreal>(effects.grading.opacity));
            drawTitleOverlay(painter, compositeRect, title, m_outputSize);
            if (title.valid && !title.text.isEmpty()) {
                const qreal sx = m_outputSize.width() > 0
                    ? static_cast<qreal>(compositeRect.width()) / m_outputSize.width() : 1.0;
                const qreal sy = m_outputSize.height() > 0
                    ? static_cast<qreal>(compositeRect.height()) / m_outputSize.height() : 1.0;
                QFont font(title.fontFamily);
                font.setPointSizeF(title.fontSize * qMin(sx, sy));
                font.setBold(title.bold);
                font.setItalic(title.italic);
                const TitleLayoutMetrics metrics = measureTitleLayout(font, title.text);
                const qreal cx = compositeRect.center().x() + title.x * sx;
                const qreal cy = compositeRect.center().y() + title.y * sy;
                const qreal windowPaddingPx = title.windowEnabled
                    ? qMax<qreal>(0.0, title.windowPadding * qMin(sx, sy))
                    : 0.0;
                const qreal frameExtraPx = title.windowFrameEnabled
                    ? (qMax<qreal>(0.0, title.windowFrameGap * qMin(sx, sy)) +
                       (qMax<qreal>(0.0, title.windowFrameWidth * qMin(sx, sy)) * 0.5))
                    : 0.0;
                PreviewOverlayInfo info;
                info.bounds = QRectF(cx - metrics.width / 2.0 - 4 - windowPaddingPx - frameExtraPx,
                                     cy - metrics.height / 2.0 - 4 - windowPaddingPx - frameExtraPx,
                                     metrics.width + 8 + (windowPaddingPx * 2.0) + (frameExtraPx * 2.0),
                                     metrics.height + 8 + (windowPaddingPx * 2.0) + (frameExtraPx * 2.0));
                m_overlayInfo.insert(clip.id, info);
                m_paintOrder.push_back(clip.id);
            }
        }
    }
    // Draw selection handles for all clips with overlay info
    for (const TimelineClip& clip : activeClips) {
        const PreviewOverlayInfo info = m_overlayInfo.value(clip.id);
        if (clip.id == m_selectedClipId && info.bounds.isValid()) {
            painter->setPen(QPen(QColor(QStringLiteral("#fff4c2")), 2.0));
            painter->setBrush(Qt::NoBrush);
            painter->drawRect(info.bounds);
            if (info.rightHandle.isValid()) {
                painter->setBrush(QColor(QStringLiteral("#fff4c2")));
                painter->drawRect(info.rightHandle);
                painter->drawRect(info.bottomHandle);
                painter->drawRect(info.cornerHandle);
            }
        }
    }
    if (m_hideOutsideOutputWindow) {
        painter->setClipping(false);
    }
    drawSpeakerPickOverlay(painter);

    painter->restore();
}

void PreviewWindow::drawEmptyState(QPainter* painter, const QRect& safeRect) {
    painter->setPen(QColor(QStringLiteral("#f5f8fb")));
    QFont titleFont = painter->font();
    titleFont.setPointSize(titleFont.pointSize() + 4);
    titleFont.setBold(true);
    painter->setFont(titleFont);
    painter->drawText(safeRect.adjusted(20, 18, -20, -20),
                      Qt::AlignTop | Qt::AlignLeft,
                      QStringLiteral("Preview"));
    
    QFont bodyFont = painter->font();
    bodyFont.setBold(false);
    bodyFont.setPointSize(qMax(10, bodyFont.pointSize() - 2));
    painter->setFont(bodyFont);
    painter->setPen(QColor(QStringLiteral("#d2dbe4")));
    painter->drawText(safeRect.adjusted(20, 58, -20, -20),
                      Qt::AlignTop | Qt::AlignLeft,
                      QStringLiteral("No active clips at this frame.\nFrame %1\nQRhi backend: %2\nGrading: %3")
                          .arg(m_currentFramePosition, 0, 'f', 3)
                          .arg(backendName())
                          .arg(m_bypassGrading ? QStringLiteral("bypassed") : QStringLiteral("on")));
}

void PreviewWindow::drawFrameLayer(QPainter* painter, const QRect& targetRect,
                    const TimelineClip& clip, const FrameHandle& frame) {
    painter->save();
    painter->setClipRect(targetRect);

    if (!frame.isNull() && frame.hasCpuImage()) {
        QImage img = frame.cpuImage();
        if (!m_bypassGrading) {
            EffectiveVisualEffects effects = evaluateEffectiveVisualEffectsAtPosition(
                clip, m_tracks, m_currentFramePosition, m_renderSyncMarkers);
            if (!m_correctionsEnabled) {
                effects.correctionPolygons.clear();
            }
            img = applyEffectiveClipVisualEffectsToImage(img, effects);
        }
        
        const QRect fitted = fitRect(img.size(), targetRect);
        const TimelineClip::TransformKeyframe transform =
            evaluateClipRenderTransformAtPosition(clip, m_currentFramePosition, m_outputSize);
        const QPointF previewScale = previewCanvasScale(targetRect);
        painter->translate(fitted.center().x() + (transform.translationX * previewScale.x()),
                           fitted.center().y() + (transform.translationY * previewScale.y()));
        painter->rotate(transform.rotation);
        painter->scale(transform.scaleX, transform.scaleY);
        const QRectF drawRect(-fitted.width() / 2.0,
                              -fitted.height() / 2.0,
                              fitted.width(),
                              fitted.height());
        painter->drawImage(drawRect, img);

        QTransform overlayTransform;
        overlayTransform.translate(fitted.center().x() + (transform.translationX * previewScale.x()),
                                   fitted.center().y() + (transform.translationY * previewScale.y()));
        overlayTransform.rotate(transform.rotation);
        overlayTransform.scale(transform.scaleX, transform.scaleY);
        const QRectF bounds = overlayTransform.mapRect(drawRect);
        PreviewOverlayInfo info;
        info.kind = PreviewOverlayKind::VisualClip;
        info.bounds = bounds;
        info.clipTransform = overlayTransform;
        info.clipPixelSize = QSizeF(fitted.width(), fitted.height());
        constexpr qreal kHandleSize = 12.0;
        info.rightHandle = QRectF(bounds.right() - kHandleSize,
                                  bounds.center().y() - kHandleSize,
                                  kHandleSize,
                                  kHandleSize * 2.0);
        info.bottomHandle = QRectF(bounds.center().x() - kHandleSize,
                                   bounds.bottom() - kHandleSize,
                                   kHandleSize * 2.0,
                                   kHandleSize);
        info.cornerHandle = QRectF(bounds.right() - kHandleSize * 1.5,
                                   bounds.bottom() - kHandleSize * 1.5,
                                   kHandleSize * 1.5,
                                   kHandleSize * 1.5);
        m_overlayInfo.insert(clip.id, info);
        m_paintOrder.push_back(clip.id);

        painter->resetTransform();
        painter->setClipping(false);
        if (clip.id == m_selectedClipId) {
            painter->setPen(QPen(QColor(QStringLiteral("#fff4c2")), 2.0));
            painter->setBrush(Qt::NoBrush);
            painter->drawRect(bounds);
            painter->setBrush(QColor(QStringLiteral("#fff4c2")));
            painter->drawRect(info.rightHandle);
            painter->drawRect(info.bottomHandle);
            painter->drawRect(info.cornerHandle);
        }
    }

    painter->setPen(QPen(QColor(255, 255, 255, 36), 1.0));
    painter->setBrush(Qt::NoBrush);
    painter->drawRoundedRect(targetRect.adjusted(0, 0, -1, -1), 12, 12);

    painter->restore();
}

void PreviewWindow::drawFramePlaceholder(QPainter* painter, const QRect& targetRect,
                          const TimelineClip& clip, const QString& message) {
    painter->save();
    painter->fillRect(targetRect, clip.color.darker(160));
    painter->setPen(QColor(255, 255, 255, 48));
    painter->drawRect(targetRect.adjusted(0, 0, -1, -1));
    painter->setPen(QColor(QStringLiteral("#f2f6fa")));
    painter->drawText(targetRect.adjusted(16, 16, -16, -16),
                      Qt::AlignCenter | Qt::TextWordWrap,
                      QStringLiteral("Track %1\n%2\n%3")
                          .arg(clip.trackIndex + 1)
                          .arg(clip.label)
                          .arg(message));
    painter->restore();
}

void PreviewWindow::drawAudioPlaceholder(QPainter* painter, const QRect& safeRect,
                          const QList<TimelineClip>& activeAudioClips) {
    if (activeAudioClips.isEmpty()) {
        return;
    }
    const TimelineClip& clip = activeAudioClips.constFirst();

    painter->save();
    painter->setPen(QPen(QColor(255, 255, 255, 40), 1.5));
    painter->setBrush(QColor(255, 255, 255, 18));
    painter->drawRoundedRect(safeRect, 18, 18);

    const QRect panel = safeRect.adjusted(12, 12, -12, -12);
    QLinearGradient gradient(panel.topLeft(), panel.bottomRight());
    gradient.setColorAt(0.0, QColor(QStringLiteral("#13222d")));
    gradient.setColorAt(1.0, QColor(QStringLiteral("#0a1218")));
    painter->fillRect(panel, gradient);

    QFont titleFont = painter->font();
    titleFont.setBold(true);
    titleFont.setPointSize(titleFont.pointSize() + 5);
    painter->setFont(titleFont);
    painter->setPen(QColor(QStringLiteral("#eef5fb")));
    painter->drawText(panel.adjusted(20, 22, -20, -20),
                      Qt::AlignTop | Qt::AlignLeft,
                      QStringLiteral("Audio Monitor"));

    QFont bodyFont = painter->font();
    bodyFont.setBold(false);
    bodyFont.setPointSize(qMax(10, bodyFont.pointSize() - 2));
    painter->setFont(bodyFont);
    painter->setPen(QColor(QStringLiteral("#c8d5e0")));
    painter->drawText(panel.adjusted(20, 60, -20, -20),
                      Qt::AlignTop | Qt::AlignLeft,
                      QStringLiteral("Selected audio clip: %1\nTransport audio: %2")
                          .arg(clip.label)
                          .arg(m_playing ? QStringLiteral("live") : QStringLiteral("paused")));

    const QRect waveRect = panel.adjusted(24, 120, -24, -36);
    const qreal rulerGutterWidth = qBound<qreal>(32.0, waveRect.width() * 0.12, 56.0);
    const int rowCount = qBound(2, waveRect.height() / 88, 6);
    const int binsPerRow = qMax(256, waveRect.width());
    const int totalDrawBins = qMax(96, rowCount * binsPerRow);

    QStringList speakerIds;
    QHash<QString, int> speakerToIndex;
    QVector<int> speakerIndexByBin(totalDrawBins, -1);
    const QVector<TranscriptSection>& sections = transcriptSectionsForClip(clip);
    const int64_t sourceStart = qMax<int64_t>(0, clip.sourceInFrame);
    const int64_t sourceSpan = qMax<int64_t>(1, clip.durationFrames);

    const int64_t clipSamples = qMax<int64_t>(1, frameToSamples(qMax<int64_t>(1, clip.durationFrames)));
    const qreal minVisibleBySamples = qBound<qreal>(
        0.00001,
        (100.0 * static_cast<qreal>(rowCount)) / static_cast<qreal>(clipSamples),
        1.0);
    const qreal maxAudioZoom =
        qBound<qreal>(20.0, 1.0 / qMax<qreal>(0.00001, minVisibleBySamples), 100000.0);
    const qreal zoom = qBound<qreal>(1.0, m_previewZoom, maxAudioZoom);
    const qreal visibleFraction = qBound<qreal>(minVisibleBySamples, 1.0 / zoom, 1.0);
    const qreal maxStart = qMax<qreal>(0.0, 1.0 - visibleFraction);
    const qreal startNorm = qBound<qreal>(0.0, m_previewPanOffset.x(), maxStart);
    const qreal endNorm = qBound<qreal>(startNorm, startNorm + visibleFraction, 1.0);

    QVector<qreal> waveformMin(totalDrawBins, 0.0);
    QVector<qreal> waveformMax(totalDrawBins, 0.0);
    (void)audioWaveformEnvelopeForClip(
        clip, totalDrawBins, startNorm, endNorm, &waveformMin, &waveformMax);

    // Absolute amplitude scale: +/-1.0 is always full row height (digital full-scale).
    const qreal unityScale = 1.0;

    const int64_t visibleSourceStart = sourceStart + static_cast<int64_t>(
        std::floor(startNorm * static_cast<qreal>(sourceSpan)));
    const int64_t visibleSourceSpan = qMax<int64_t>(
        1,
        static_cast<int64_t>(std::ceil(visibleFraction * static_cast<qreal>(sourceSpan))));
    if (!sections.isEmpty()) {
        for (int i = 0; i < totalDrawBins; ++i) {
            const qreal t = (static_cast<qreal>(i) + 0.5) / static_cast<qreal>(totalDrawBins);
            const int64_t sourceFrame = visibleSourceStart + static_cast<int64_t>(
                std::floor(t * static_cast<qreal>(visibleSourceSpan)));
            const QString speakerId = speakerAtSourceFrame(sections, sourceFrame);
            if (speakerId.isEmpty()) {
                continue;
            }
            int idx = speakerToIndex.value(speakerId, -1);
            if (idx < 0) {
                idx = speakerIds.size();
                speakerIds.push_back(speakerId);
                speakerToIndex.insert(speakerId, idx);
            }
            speakerIndexByBin[i] = idx;
        }
        const int maxGapBins = qMax(1, binsPerRow / 72);
        fillShortUnknownSpeakerGaps(&speakerIndexByBin, maxGapBins);
    }

    bool hoverInfoValid = false;
    QString hoverSpeakerId;
    int64_t hoverSourceFrame = 0;
    qreal hoverAmplitude = 0.0;
    qreal hoverTimelinePercent = 0.0;
    const QString transcriptPath = activeTranscriptPathForClipFile(clip.filePath);

    painter->setBrush(Qt::NoBrush);
    for (int row = 0; row < rowCount; ++row) {
        const qreal rowTop = static_cast<qreal>(waveRect.top()) +
                             (static_cast<qreal>(row) * static_cast<qreal>(waveRect.height()) / rowCount);
        const qreal rowBottom = static_cast<qreal>(waveRect.top()) +
                                (static_cast<qreal>(row + 1) * static_cast<qreal>(waveRect.height()) / rowCount);
        const QRectF rowRect(static_cast<qreal>(waveRect.left()) + rulerGutterWidth, rowTop,
                             qMax<qreal>(1.0, static_cast<qreal>(waveRect.width()) - rulerGutterWidth),
                             rowBottom - rowTop);
        const qreal centerY = rowRect.center().y();

        painter->setPen(QPen(QColor(255, 255, 255, 14), 1.0));
        painter->drawRect(rowRect.adjusted(0.5, 0.5, -0.5, -0.5));
        painter->setPen(QPen(QColor(255, 255, 255, 22), 1.0));
        painter->drawLine(QPointF(rowRect.left(), centerY), QPointF(rowRect.right(), centerY));

        struct DbMark {
            qreal db = 0.0;
            QString dbLabel;
        };
        const DbMark dbMarks[] = {
            {0.0, QStringLiteral("0 dBFS")},
            {-6.0, QStringLiteral("-6 dBFS")},
            {-12.0, QStringLiteral("-12 dBFS")},
            {-18.0, QStringLiteral("-18 dBFS")}
        };
        QFont rulerFont = painter->font();
        rulerFont.setPointSize(qMax(8, rulerFont.pointSize() - 1));
        painter->setFont(rulerFont);
        for (const DbMark& mark : dbMarks) {
            const qreal amp = std::pow(10.0, mark.db / 20.0);
            const qreal yTopMark = centerY - (amp * rowRect.height() * 0.5);
            const qreal yBottomMark = centerY + (amp * rowRect.height() * 0.5);
            const int gridAlpha = (mark.db == 0.0) ? 52 : 24;
            painter->setPen(QPen(QColor(157, 239, 255, gridAlpha), 1.0));
            painter->drawLine(QPointF(rowRect.left(), yTopMark), QPointF(rowRect.right(), yTopMark));
            painter->drawLine(QPointF(rowRect.left(), yBottomMark), QPointF(rowRect.right(), yBottomMark));

            if (row == 0) {
                painter->setPen(QColor(166, 211, 230, 175));
                const QString label = QStringLiteral("%1 (%2)")
                                          .arg(mark.dbLabel)
                                          .arg(QString::number(amp, 'f', 1));
                const QRectF labelRect(static_cast<qreal>(waveRect.left()) + 3.0,
                                       yTopMark - 8.0,
                                       rulerGutterWidth - 8.0,
                                       16.0);
                painter->drawText(labelRect, Qt::AlignRight | Qt::AlignVCenter, label);
            }
        }

        int rowStartBin = row * binsPerRow;
        int rowEndBin = qMin(totalDrawBins, rowStartBin + binsPerRow);
        if (rowStartBin >= rowEndBin) {
            continue;
        }

        int runStart = rowStartBin;
        int runSpeaker = speakerIndexByBin[qBound(0, runStart, totalDrawBins - 1)];
        for (int i = rowStartBin + 1; i <= rowEndBin; ++i) {
            const int idx = (i < rowEndBin)
                ? speakerIndexByBin[qBound(0, i, totalDrawBins - 1)]
                : std::numeric_limits<int>::min();
            if (idx == runSpeaker) {
                continue;
            }
            if (runSpeaker >= 0 && runSpeaker < speakerIds.size()) {
                const qreal x0 = rowRect.left() +
                    (static_cast<qreal>(runStart - rowStartBin) / qMax<qreal>(1.0, rowEndBin - rowStartBin)) * rowRect.width();
                const qreal x1 = rowRect.left() +
                    (static_cast<qreal>(i - rowStartBin) / qMax<qreal>(1.0, rowEndBin - rowStartBin)) * rowRect.width();
                painter->fillRect(QRectF(x0, rowRect.top(), qMax<qreal>(1.0, x1 - x0), rowRect.height()),
                                  speakerColor(speakerIds.at(runSpeaker), 30));
            }
            runStart = i;
            runSpeaker = idx;
        }

        const int rowBinCount = qMax(2, rowEndBin - rowStartBin);
        QVector<qreal> rowMinValues(rowBinCount, 0.0);
        QVector<qreal> rowMaxValues(rowBinCount, 0.0);
        for (int i = 0; i < rowBinCount; ++i) {
            const int sourceBin = qBound(0, rowStartBin + i, totalDrawBins - 1);
            qreal minV = qBound<qreal>(-1.0, waveformMin[sourceBin] * unityScale, 1.0);
            qreal maxV = qBound<qreal>(-1.0, waveformMax[sourceBin] * unityScale, 1.0);
            if (minV > maxV) {
                std::swap(minV, maxV);
            }
            rowMinValues[i] = minV;
            rowMaxValues[i] = maxV;
        }

        QVector<qreal> displayMin = rowMinValues;
        QVector<qreal> displayMax = rowMaxValues;
        if (zoom <= 4.0 && rowBinCount >= 3) {
            // Low-zoom smoothing reduces staircase artifacts from min/max decimation.
            for (int i = 1; i < rowBinCount - 1; ++i) {
                const qreal smoothMin =
                    (rowMinValues[i - 1] + (rowMinValues[i] * 2.0) + rowMinValues[i + 1]) * 0.25;
                const qreal smoothMax =
                    (rowMaxValues[i - 1] + (rowMaxValues[i] * 2.0) + rowMaxValues[i + 1]) * 0.25;
                if (smoothMin <= smoothMax) {
                    displayMin[i] = smoothMin;
                    displayMax[i] = smoothMax;
                } else {
                    displayMin[i] = smoothMax;
                    displayMax[i] = smoothMin;
                }
            }
        }

        QPainterPath areaPath;
        QPainterPath topPath;
        QPainterPath bottomPath;
        areaPath.moveTo(rowRect.left(), centerY);
        topPath.moveTo(rowRect.left(), centerY);
        bottomPath.moveTo(rowRect.left(), centerY);

        for (int i = 0; i < rowBinCount; ++i) {
            const qreal x = rowRect.left() +
                (static_cast<qreal>(i) / qMax<qreal>(1.0, rowBinCount - 1)) * rowRect.width();
            const qreal halfRange = rowRect.height() * 0.5;
            const qreal yTop = centerY - (displayMax[i] * halfRange);
            const qreal yBottom = centerY - (displayMin[i] * halfRange);
            topPath.lineTo(x, yTop);
            bottomPath.lineTo(x, yBottom);
            areaPath.lineTo(x, yTop);
        }
        for (int i = rowBinCount - 1; i >= 0; --i) {
            const qreal x = rowRect.left() +
                (static_cast<qreal>(i) / qMax<qreal>(1.0, rowBinCount - 1)) * rowRect.width();
            const qreal halfRange = rowRect.height() * 0.5;
            const qreal yBottom = centerY - (displayMin[i] * halfRange);
            areaPath.lineTo(x, yBottom);
        }
        areaPath.closeSubpath();

        auto withAlphaScale = [](QColor c, qreal weight) -> QColor {
            const int a = qBound(0, static_cast<int>(std::round(c.alphaF() * weight * 255.0)), 255);
            c.setAlpha(a);
            return c;
        };
        auto smoothStep = [](qreal edge0, qreal edge1, qreal x) -> qreal {
            if (edge1 <= edge0) {
                return x >= edge1 ? 1.0 : 0.0;
            }
            const qreal t = qBound<qreal>(0.0, (x - edge0) / (edge1 - edge0), 1.0);
            return t * t * (3.0 - (2.0 * t));
        };

        const qreal lowToStem = smoothStep(6.0, 10.0, zoom);
        const qreal stemToTrace = smoothStep(48.0, 88.0, zoom);
        const qreal lowFillWeight = 1.0 - lowToStem;
        const qreal stemWeight = lowToStem * (1.0 - stemToTrace);
        const qreal traceWeight = stemToTrace;
        if (m_audioWaveformVisible) {
            if (traceWeight > 0.001) {
                // At very high zoom, blend in a continuous peak-following trace.
                // Use dominant signed excursion (not midpoint), otherwise amplitude appears undersized.
                QPainterPath centerPath;
                centerPath.moveTo(rowRect.left(), centerY);
                for (int i = 0; i < rowBinCount; ++i) {
                    const qreal x = rowRect.left() +
                        (static_cast<qreal>(i) / qMax<qreal>(1.0, rowBinCount - 1)) * rowRect.width();
                    const qreal halfRange = rowRect.height() * 0.5;
                    const qreal posAmp = qBound<qreal>(0.0, displayMax[i], 1.0);
                    const qreal negAmp = qBound<qreal>(-1.0, displayMin[i], 0.0);
                    const qreal signedPeakAmp =
                        (qAbs(posAmp) >= qAbs(negAmp)) ? posAmp : negAmp;
                    const qreal y = centerY - (signedPeakAmp * halfRange);
                    centerPath.lineTo(x, y);
                }
                painter->setPen(QPen(withAlphaScale(QColor(127, 216, 237, 210), traceWeight),
                                     1.65, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
                painter->setBrush(Qt::NoBrush);
                painter->drawPath(centerPath);
            }
            if (stemWeight > 0.001) {
                painter->setPen(Qt::NoPen);
                painter->setBrush(withAlphaScale(QColor(127, 216, 237, 78), stemWeight));
                painter->drawPath(areaPath);
                painter->save();
                painter->setPen(QPen(withAlphaScale(QColor(127, 216, 237, 155), stemWeight),
                                     0.9, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
                for (int i = 0; i < rowBinCount; ++i) {
                    const qreal x = rowRect.left() +
                        (static_cast<qreal>(i) / qMax<qreal>(1.0, rowBinCount - 1)) * rowRect.width();
                    const qreal halfRange = rowRect.height() * 0.5;
                    const qreal yTop = centerY - (displayMax[i] * halfRange);
                    const qreal yBottom = centerY - (displayMin[i] * halfRange);
                    painter->drawLine(QPointF(x, yTop), QPointF(x, yBottom));
                }
                painter->restore();
            }
            if (lowFillWeight > 0.001) {
                painter->setPen(Qt::NoPen);
                painter->setBrush(withAlphaScale(QColor(127, 216, 237, 102), lowFillWeight));
                painter->drawPath(areaPath);
            }

            const qreal outlineWeight = qBound<qreal>(0.2, 1.0 - (traceWeight * 0.9), 1.0);
            painter->setBrush(Qt::NoBrush);
            painter->setPen(QPen(withAlphaScale(QColor(QStringLiteral("#9defff")), outlineWeight),
                                 1.2, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
            painter->drawPath(topPath);
            painter->drawPath(bottomPath);
        }

        if (m_audioSpeakerHoverModalEnabled &&
            m_lastMousePos.x() >= rowRect.left() && m_lastMousePos.x() <= rowRect.right() &&
            m_lastMousePos.y() >= rowRect.top() && m_lastMousePos.y() <= rowRect.bottom()) {
            const qreal rowXNorm = qBound<qreal>(
                0.0,
                (m_lastMousePos.x() - rowRect.left()) / qMax<qreal>(1.0, rowRect.width()),
                1.0);
            const int hoverBinInRow = qBound(
                0,
                static_cast<int>(std::round(rowXNorm * static_cast<qreal>(rowBinCount - 1))),
                rowBinCount - 1);
            const int hoverBin = qBound(0, rowStartBin + hoverBinInRow, totalDrawBins - 1);
            const int speakerIdx = speakerIndexByBin.value(hoverBin, -1);
            if (speakerIdx >= 0 && speakerIdx < speakerIds.size()) {
                hoverInfoValid = true;
                hoverSpeakerId = speakerIds.at(speakerIdx);
                hoverSourceFrame = visibleSourceStart + static_cast<int64_t>(
                    std::floor(((static_cast<qreal>(hoverBin) + 0.5) / static_cast<qreal>(totalDrawBins)) *
                               static_cast<qreal>(visibleSourceSpan)));
                hoverAmplitude = qMax(qAbs(displayMin[hoverBinInRow]), qAbs(displayMax[hoverBinInRow]));
                hoverTimelinePercent = qBound<qreal>(
                    0.0,
                    ((static_cast<qreal>(hoverBin) + 0.5) / static_cast<qreal>(totalDrawBins)) * 100.0,
                    100.0);
            }
        }
    }

    if (hoverInfoValid) {
        const HoverSpeakerProfile* profile = hoverSpeakerProfileFor(transcriptPath, hoverSpeakerId);
        const QString hoverName = profile && !profile->name.trimmed().isEmpty()
            ? profile->name.trimmed() : hoverSpeakerId;
        const QString hoverOrg = profile ? profile->organization.trimmed() : QString();
        const QString hoverDesc = profile && !profile->description.trimmed().isEmpty()
            ? profile->description.trimmed()
            : QStringLiteral("No speaker description available.");
        const QPixmap profileImage = profile ? hoverSpeakerImage(*profile, 76) : QPixmap();
        const QString hoverText = QStringLiteral("Name: %1\nSpeaker ID: %2\nOrganization: %3\nDescription: %4\nFrame: %5\nAmp: %6\nView Pos: %7%")
                                      .arg(hoverName,
                                           hoverSpeakerId,
                                           hoverOrg.isEmpty() ? QStringLiteral("None") : hoverOrg,
                                           hoverDesc)
                                      .arg(hoverSourceFrame)
                                      .arg(QString::number(hoverAmplitude, 'f', 3))
                                      .arg(QString::number(hoverTimelinePercent, 'f', 1));
        const QFontMetrics fm(painter->font());
        const int imageWidth = profileImage.isNull() ? 0 : 76;
        const int textWidth = profileImage.isNull() ? 320 : 244;
        const QRect textRect = fm.boundingRect(QRect(0, 0, textWidth, 220),
                                               Qt::AlignLeft | Qt::AlignTop | Qt::TextWordWrap,
                                               hoverText);
        const QRect modalRect(panel.right() - (textRect.width() + imageWidth + 30) - 34,
                              panel.top() + 22,
                              textRect.width() + imageWidth + 30,
                              qMax(textRect.height() + 14, profileImage.isNull() ? 0 : (imageWidth + 18)));
        painter->setPen(Qt::NoPen);
        painter->setBrush(QColor(8, 13, 20, 225));
        painter->drawRoundedRect(modalRect, 8, 8);
        if (!profileImage.isNull()) {
            const QRect imageRect(modalRect.left() + 8, modalRect.top() + 8, imageWidth, imageWidth);
            painter->save();
            QPainterPath imageClip;
            imageClip.addRoundedRect(imageRect, 6, 6);
            painter->setClipPath(imageClip);
            painter->drawPixmap(imageRect, profileImage);
            painter->restore();
        } else {
            painter->setPen(QColor(170, 184, 198, 170));
            painter->drawText(QRect(modalRect.left() + 8, modalRect.top() + 8, 76, 20),
                              Qt::AlignLeft | Qt::AlignVCenter,
                              QStringLiteral("Image: none"));
        }
        painter->setPen(QColor(QStringLiteral("#dff5ff")));
        const int textLeft = modalRect.left() + (profileImage.isNull() ? 8 : (imageWidth + 14));
        painter->drawText(QRect(textLeft, modalRect.top() + 7,
                                modalRect.right() - textLeft - 8, modalRect.height() - 14),
                          Qt::AlignLeft | Qt::AlignTop | Qt::TextWordWrap,
                          hoverText);
    }

    painter->setPen(QColor(QStringLiteral("#9fb3c8")));
    painter->drawText(panel.adjusted(20, panel.height() - 36, -20, 6),
                      Qt::AlignLeft | Qt::AlignVCenter,
                      QStringLiteral("Waveform source: decoded clip audio (mono envelope), absolute full-scale display (1.0 = 0 dBFS), wrapped rows with speaker timeline tint | Zoom %1%")
                          .arg(QString::number(zoom * 100.0, 'f', 0)));
    painter->restore();
}

