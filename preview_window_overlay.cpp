#include "preview.h"
#include "preview_debug.h"
#include "debug_controls.h"
#include "titles.h"

#include <QJsonArray>
#include <QJsonObject>
#include <QLinearGradient>
#include <QPainter>
#include <QTextDocument>

#include <cmath>

void PreviewWindow::paintEvent(QPaintEvent* event) {
    if (!usingCpuFallback()) {
        QOpenGLWidget::paintEvent(event);
        return;
    }
    if (!QWidget::paintEngine()) return;

    Q_UNUSED(event)
    m_lastPaintMs = nowMs();

    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing, true);
    drawBackground(&painter);
    const QList<TimelineClip> activeClips = getActiveClips();
    const QRect safeRect = rect().adjusted(24, 24, -24, -24);
    drawCompositedPreview(&painter, safeRect, activeClips);
    drawPreviewChrome(&painter, safeRect, activeClips.size());
}

QRect PreviewWindow::previewCanvasBaseRect() const {
    const QRect available = rect().adjusted(36, 36, -36, -36);
    if (!available.isValid()) return available;
    QSize fitted = (m_outputSize.isValid() ? m_outputSize : QSize(1080, 1920));
    fitted.scale(available.size(), Qt::KeepAspectRatio);
    const QPoint topLeft(available.center().x() - fitted.width() / 2,
                         available.center().y() - fitted.height() / 2);
    return QRect(topLeft, fitted);
}

QRect PreviewWindow::scaledCanvasRect(const QRect& baseRect) const {
    const QSize scaledSize(qMax(1, qRound(baseRect.width() * m_previewZoom)),
                           qMax(1, qRound(baseRect.height() * m_previewZoom)));
    const QPoint center = baseRect.center();
    return QRect(qRound(center.x() - scaledSize.width() / 2.0 + m_previewPanOffset.x()),
                 qRound(center.y() - scaledSize.height() / 2.0 + m_previewPanOffset.y()),
                 scaledSize.width(),
                 scaledSize.height());
}

QPointF PreviewWindow::previewCanvasScale(const QRect& targetRect) const {
    const QSize output = m_outputSize.isValid() ? m_outputSize : QSize(1080, 1920);
    return QPointF(targetRect.width() / qMax<qreal>(1.0, output.width()),
                   targetRect.height() / qMax<qreal>(1.0, output.height()));
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
    painter->save();
    painter->setPen(QPen(QColor(255, 255, 255, 40), 1.5));
    painter->setBrush(QColor(255, 255, 255, 18));
    painter->drawRoundedRect(safeRect, 18, 18);

    if (activeClips.isEmpty()) {
        QList<TimelineClip> activeAudioClips;
        for (const TimelineClip& clip : m_clips) {
            if (clipIsAudioOnly(clip) && isSampleWithinClip(clip, m_currentSample)) {
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

    painter->setPen(QPen(QColor(255, 255, 255, 36), 1.0));
    painter->setBrush(Qt::NoBrush);
    painter->drawRoundedRect(compositeRect.adjusted(0, 0, -1, -1), 12, 12);

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

    for (const TimelineClip& clip : activeClips) {
        if (clipShowsTranscriptOverlay(clip)) {
            drawTranscriptOverlay(painter, clip, compositeRect);
        }
    }

    // Draw title overlays for title clips at the current playhead
    for (const TimelineClip& clip : activeClips) {
        if (clip.mediaType == ClipMediaType::Title && !clip.titleKeyframes.isEmpty()) {
            const int64_t localFrame = qMax<int64_t>(0,
                m_currentFrame - clip.startFrame);
            const EvaluatedTitle title = evaluateTitleAtLocalFrame(clip, localFrame);
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
                const QFontMetricsF fm(font);
                const qreal textWidth = fm.horizontalAdvance(title.text);
                const qreal textHeight = fm.height();
                const qreal cx = compositeRect.center().x() + title.x * sx;
                const qreal cy = compositeRect.center().y() + title.y * sy;
                const QRectF bounds(cx - textWidth / 2.0 - 4, cy - textHeight / 2.0 - 4,
                                    textWidth + 8, textHeight + 8);
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

    for (const TimelineClip& clip : activeClips) {
        const PreviewOverlayInfo info = m_overlayInfo.value(clip.id);
        if (clip.id != m_selectedClipId || !info.bounds.isValid() || clip.correctionPolygons.isEmpty()) {
            continue;
        }
        painter->save();
        painter->setRenderHint(QPainter::Antialiasing, true);
        painter->setPen(QPen(QColor(255, 92, 92, 220), 2.0));
        painter->setBrush(QColor(255, 92, 92, 48));
        for (const TimelineClip::CorrectionPolygon& polygon : clip.correctionPolygons) {
            if (!polygon.enabled || polygon.pointsNormalized.size() < 3) {
                continue;
            }
            QPainterPath path;
            const QPointF first(
                info.bounds.left() + (polygon.pointsNormalized.constFirst().x() * info.bounds.width()),
                info.bounds.top() + (polygon.pointsNormalized.constFirst().y() * info.bounds.height()));
            path.moveTo(first);
            for (int i = 1; i < polygon.pointsNormalized.size(); ++i) {
                const QPointF point(
                    info.bounds.left() + (polygon.pointsNormalized[i].x() * info.bounds.width()),
                    info.bounds.top() + (polygon.pointsNormalized[i].y() * info.bounds.height()));
                path.lineTo(point);
            }
            path.closeSubpath();
            painter->drawPath(path);
        }
        painter->restore();
    }

    if (!m_correctionDraftPoints.isEmpty()) {
        const PreviewOverlayInfo info = m_overlayInfo.value(m_selectedClipId);
        if (info.bounds.isValid()) {
            painter->save();
            painter->setRenderHint(QPainter::Antialiasing, true);
            painter->setPen(QPen(QColor(255, 200, 64, 230), 2.0, Qt::DashLine));
            painter->setBrush(QColor(255, 200, 64, 64));
            QPolygonF polygon;
            polygon.reserve(m_correctionDraftPoints.size());
            for (const QPointF& pointNorm : m_correctionDraftPoints) {
                polygon.push_back(QPointF(
                    info.bounds.left() + (pointNorm.x() * info.bounds.width()),
                    info.bounds.top() + (pointNorm.y() * info.bounds.height())));
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

    QList<TimelineClip> activeAudioClips;
    for (const TimelineClip& clip : m_clips) {
        if (clipIsAudioOnly(clip) && isSampleWithinClip(clip, m_currentSample)) {
            activeAudioClips.push_back(clip);
        }
    }
    if (!activeAudioClips.isEmpty()) {
        drawAudioBadge(painter, compositeRect, activeAudioClips);
    }
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
    
    if (activeClips.isEmpty()) {
        QList<TimelineClip> activeAudioClips;
        for (const TimelineClip& clip : m_clips) {
            if (clipIsAudioOnly(clip) && isSampleWithinClip(clip, m_currentSample)) {
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
    int nullCount = 0;
    QJsonArray clipSelections;

    for (const TimelineClip& clip : activeClips) {
        if (clip.mediaType == ClipMediaType::Title) {
            continue; // Title clips are drawn as text overlays below
        }
        const int64_t localFrame = sourceFrameForSample(clip, m_currentSample);
        const bool usePlaybackPipeline =
            m_playing &&
            clip.sourceKind == MediaSourceKind::ImageSequence &&
            clip.mediaType != ClipMediaType::Image;
        const bool usePlaybackBuffer =
            m_playing &&
            !usePlaybackPipeline &&
            m_cache;
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
            if (frame.isNull() && m_cache) {
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
                frame = m_lastPresentedFrames.value(clip.id);
                if (!frame.isNull()) {
                    ++heldCount;
                    selection = QStringLiteral("held");
                }
            }
        }
        playbackFrameSelectionTrace(QStringLiteral("PreviewWindow::drawCompositedPreview.select"),
                                    clip,
                                    localFrame,
                                    exactFrame,
                                    frame,
                                    m_currentFramePosition);
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
            const EvaluatedTitle title = evaluateTitleAtLocalFrame(clip, localFrame);
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
                const QFontMetricsF fm(font);
                const qreal textWidth = fm.horizontalAdvance(title.text);
                const qreal textHeight = fm.height();
                const qreal cx = compositeRect.center().x() + title.x * sx;
                const qreal cy = compositeRect.center().y() + title.y * sy;
                PreviewOverlayInfo info;
                info.bounds = QRectF(cx - textWidth / 2.0 - 4, cy - textHeight / 2.0 - 4,
                                     textWidth + 8, textHeight + 8);
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
            const EffectiveVisualEffects effects =
                evaluateEffectiveVisualEffectsAtPosition(clip, m_tracks, m_currentFramePosition);
            img = applyEffectiveClipVisualEffectsToImage(img, effects);
        }
        
        const QRect fitted = fitRect(img.size(), targetRect);
        const TimelineClip::TransformKeyframe transform =
            evaluateClipTransformAtPosition(clip, m_currentFramePosition);
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
                      QStringLiteral("Active audio clip: %1\nTransport audio: %2")
                          .arg(activeAudioClips.constFirst().label)
                          .arg(m_playing ? QStringLiteral("live") : QStringLiteral("paused")));

    const QRect waveRect = panel.adjusted(24, 120, -24, -36);
    painter->setPen(Qt::NoPen);
    for (int x = waveRect.left(); x < waveRect.right(); x += 10) {
        const int idx = (x - waveRect.left()) / 10;
        const qreal phase = std::fmod(static_cast<qreal>(idx * 13) + m_currentFramePosition, 100.0) / 99.0;
        const int barHeight = qMax(12, qRound((0.2 + std::sin(phase * 6.28318) * 0.4 + 0.4) * waveRect.height()));
        const QRect barRect(x, waveRect.center().y() - barHeight / 2, 6, barHeight);
        painter->setBrush(QColor(QStringLiteral("#58c4dd")));
        painter->drawRoundedRect(barRect, 3, 3);
    }
    painter->restore();
}

void PreviewWindow::drawAudioBadge(QPainter* painter, const QRect& targetRect,
                    const QList<TimelineClip>& activeAudioClips) {
    painter->save();
    const QRect badgeRect(targetRect.left() + 16, targetRect.bottom() - 46, 240, 30);
    painter->setPen(Qt::NoPen);
    painter->setBrush(QColor(7, 11, 17, 176));
    painter->drawRoundedRect(badgeRect, 10, 10);
    painter->setPen(QColor(QStringLiteral("#dff8ff")));
    painter->drawText(badgeRect.adjusted(12, 0, -12, 0),
                      Qt::AlignLeft | Qt::AlignVCenter,
                      QStringLiteral("Audio  %1").arg(activeAudioClips.constFirst().label));
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
