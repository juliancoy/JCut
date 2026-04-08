#include "timeline_renderer.h"
#include "timeline_widget.h"
#include "timeline_layout.h"

TimelineRenderer::TimelineRenderer(TimelineWidget* widget)
    : m_widget(widget) {
}

void TimelineRenderer::paint(QPainter* painter) {
    const TimelineLayout& layout = *m_widget->m_layout;
    painter->setRenderHint(QPainter::Antialiasing, true);
    painter->fillRect(m_widget->rect(), QColor(QStringLiteral("#0f1216")));

    const QRect draw = layout.drawRect();
    const QRect topBar = layout.topBarRect();
    const QRect ruler = layout.rulerRect();
    const QRect tracks = layout.trackRect();
    const QRect content = layout.timelineContentRect();
    const QRect exportBar = layout.exportRangeRect();

    painter->setPen(Qt::NoPen);
    painter->setBrush(QColor(QStringLiteral("#171c22")));
    painter->drawRoundedRect(topBar, 10, 10);

    const QRect frameInfoRect(draw.left() + 10, topBar.top() + 4, TimelineWidget::kTimelineLabelWidth + 44, 18);
    painter->setBrush(QColor(QStringLiteral("#202a34")));
    painter->drawRoundedRect(exportBar, 7, 7);

    painter->setBrush(QColor(QStringLiteral("#4ea1ff")));
    for (const ExportRangeSegment& segment : m_widget->m_exportRanges) {
        painter->drawRoundedRect(layout.exportSegmentRect(segment), 7, 7);
    }
    painter->setBrush(QColor(QStringLiteral("#fff4c2")));
    for (int i = 0; i < m_widget->m_exportRanges.size(); ++i) {
        painter->drawRoundedRect(layout.exportHandleRect(i, true), 3, 3);
        painter->drawRoundedRect(layout.exportHandleRect(i, false), 3, 3);
    }
    painter->setPen(QColor(QStringLiteral("#0f1216")));
    QString exportLabel;
    for (int i = 0; i < m_widget->m_exportRanges.size(); ++i) {
        if (i > 0) {
            exportLabel += QStringLiteral(" | ");
        }
        exportLabel += QStringLiteral("%1 -> %2")
                           .arg(m_widget->timecodeForFrame(m_widget->m_exportRanges[i].startFrame))
                           .arg(m_widget->timecodeForFrame(m_widget->m_exportRanges[i].endFrame));
    }
    painter->drawText(exportBar.adjusted(10, 0, -10, 0),
                     Qt::AlignCenter,
                     QStringLiteral("Export %1").arg(exportLabel));

    painter->setPen(Qt::NoPen);
    painter->setBrush(QColor(QStringLiteral("#202a34")));
    painter->drawRoundedRect(frameInfoRect, 7, 7);
    painter->setPen(QColor(QStringLiteral("#eef4fa")));
    painter->drawText(frameInfoRect.adjusted(8, 0, -8, 0),
                     Qt::AlignLeft | Qt::AlignVCenter,
                     QStringLiteral("Frame %1").arg(m_widget->m_currentFrame));

    painter->setPen(Qt::NoPen);
    painter->setBrush(QColor(QStringLiteral("#171c22")));
    painter->drawRoundedRect(tracks, 10, 10);

    painter->setPen(QColor(QStringLiteral("#6d7887")));
    for (int64_t frame = 0; frame <= m_widget->totalFrames(); frame += 30) {
        const int x = m_widget->xFromFrame(frame);
        const bool major = (frame % 150) == 0;
        painter->setPen(major ? QColor(QStringLiteral("#8fa0b5")) : QColor(QStringLiteral("#53606e")));
        painter->drawLine(x, ruler.bottom() - (major ? 18 : 10), x, tracks.bottom() - 8);

        if (major) {
            painter->drawText(QRect(x + 4, ruler.top(), 56, ruler.height()),
                              Qt::AlignLeft | Qt::AlignVCenter,
                              m_widget->timecodeForFrame(frame));
        }
    }

    painter->setPen(QColor(QStringLiteral("#24303c")));
    for (int track = 0; track < m_widget->trackCount(); ++track) {
        const int dividerY = layout.trackTop(track) + layout.trackHeight(track);
        painter->drawLine(content.left() - 8, dividerY, tracks.right() - 10, dividerY);
    }

    painter->save();
    painter->setClipRect(content);
    for (const TimelineClip& clip : m_widget->m_clips) {
        const QRect clipRect = layout.clipRectFor(clip);
        const bool audioOnly = clipIsAudioOnly(clip);
        const bool hovered = clip.id == m_widget->m_hoveredClipId;
        const bool showSourceGhost = clip.mediaType != ClipMediaType::Image;
        const bool visualsEnabled = !clipHasVisuals(clip) || clip.videoEnabled;
        const bool audioEnabled = !clip.hasAudio || clip.audioEnabled;
        if (showSourceGhost) {
            const int64_t ghostStartFrame = qMax<int64_t>(0, clip.startFrame - clip.sourceInFrame);
            const int64_t ghostDurationFrames = qMax<int64_t>(clip.durationFrames, clip.sourceDurationFrames);
            const QRect ghostRect(m_widget->xFromFrame(ghostStartFrame),
                                  clipRect.y(),
                                  qMax(40, m_widget->widthForFrames(ghostDurationFrames)),
                                  clipRect.height());

            if (ghostRect != clipRect) {
                painter->setPen(QColor(255, 255, 255, 20));
                painter->setBrush(clip.color.lighter(130));
                painter->setOpacity(0.18);
                painter->drawRoundedRect(ghostRect, 7, 7);
                painter->setOpacity(1.0);
            }
        }

        QColor clipFill = clip.color;
        if (!visualsEnabled || !audioEnabled) {
            clipFill = clipFill.darker(160);
            clipFill.setAlpha(160);
        }
        painter->setPen(QColor(255, 255, 255, 32));
        painter->setBrush(clipFill);
        painter->drawRoundedRect(clipRect, 7, 7);

        if (audioOnly) {
            painter->save();
            const int verticalInset = qMax(5, clipRect.height() / 10);
            const QRect envelopeRect = clipRect.adjusted(8, verticalInset, -8, -verticalInset);
            painter->setPen(Qt::NoPen);
            painter->setBrush(QColor(QStringLiteral("#163946")));
            painter->drawRoundedRect(envelopeRect, 5, 5);
            painter->setClipRect(envelopeRect);
            painter->setPen(QPen(QColor(QStringLiteral("#9fe7f4")), 2));
            painter->drawLine(envelopeRect.left(), envelopeRect.center().y(),
                             envelopeRect.right(), envelopeRect.center().y());
            painter->setPen(Qt::NoPen);
            for (int x = envelopeRect.left(); x < envelopeRect.right(); x += 6) {
                const int idx = (x - envelopeRect.left()) / 6;
                const qreal phase = static_cast<qreal>((idx * 17 + clip.startFrame + clip.sourceInFrame) % 100) / 99.0;
                const qreal shaped = 0.2 + std::abs(std::sin(phase * 6.28318)) * 0.8;
                const int barHeight = qMax(8, qRound(shaped * envelopeRect.height()));
                const QRect barRect(x, envelopeRect.center().y() - barHeight / 2, 4, barHeight);
                painter->setBrush(QColor(QStringLiteral("#f2feff")));
                painter->drawRoundedRect(barRect, 2, 2);
            }
            painter->restore();
        }

        painter->setPen(QColor(QStringLiteral("#f4f7fb")));
        if (clip.id == m_widget->m_selectedClipId) {
            painter->setPen(QPen(QColor(QStringLiteral("#fff4c2")), 2));
            if (audioOnly) {
                painter->setBrush(Qt::NoBrush);
                painter->drawRoundedRect(clipRect.adjusted(1, 1, -1, -1), 7, 7);
            } else {
                painter->setBrush(clip.color.lighter(108));
                painter->drawRoundedRect(clipRect, 7, 7);
            }
            painter->setPen(Qt::NoPen);
            painter->setBrush(QColor(QStringLiteral("#fff4c2")));
            const int handleInset = qMax(5, clipRect.height() / 10);
            const QRect leftHandle(clipRect.left() + 2, clipRect.top() + handleInset, 4, clipRect.height() - (handleInset * 2));
            const QRect rightHandle(clipRect.right() - 5, clipRect.top() + handleInset, 4, clipRect.height() - (handleInset * 2));
            painter->drawRoundedRect(leftHandle, 2, 2);
            painter->drawRoundedRect(rightHandle, 2, 2);
            painter->setPen(QColor(QStringLiteral("#f4f7fb")));
        }
        QString clipTitle;
        if (clip.mediaType == ClipMediaType::Title) {
            const QString titleText = clip.titleKeyframes.isEmpty()
                ? QStringLiteral("Title") : clip.titleKeyframes.constFirst().text;
            clipTitle = QStringLiteral("T  %1").arg(titleText);
        } else if (audioOnly) {
            clipTitle = QStringLiteral("AUDIO  %1").arg(clip.label);
        } else {
            clipTitle = clip.label;
        }
        if (clip.locked) {
            clipTitle = QStringLiteral("🔒 %1").arg(clipTitle);
        }
        if (!visualsEnabled && clipHasVisuals(clip)) {
            clipTitle = QStringLiteral("Hidden  %1").arg(clipTitle);
        }
        if (!audioEnabled && clip.hasAudio) {
            clipTitle = QStringLiteral("Muted  %1").arg(clipTitle);
        }
        if (qAbs(clip.playbackRate - 1.0) > 0.001) {
            clipTitle = QStringLiteral("%1  %2x").arg(clipTitle).arg(clip.playbackRate, 0, 'g', 3);
        }
        painter->drawText(clipRect.adjusted(10, 0, -10, 0),
                        Qt::AlignLeft | Qt::AlignVCenter,
                        clipTitle);
    }
    painter->restore();

    if (m_widget->m_dropFrame >= 0) {
        const int x = m_widget->xFromFrame(m_widget->m_dropFrame);
        painter->setPen(QPen(QColor(QStringLiteral("#f7b955")), 2, Qt::DashLine));
        painter->drawLine(x, tracks.top() + 2, x, tracks.bottom() - 2);
    }

    if (m_widget->m_snapIndicatorFrame >= 0) {
        const int x = m_widget->xFromFrame(m_widget->m_snapIndicatorFrame);
        painter->setPen(QPen(QColor(QStringLiteral("#ffe082")), 2, Qt::DashLine));
        painter->drawLine(x, ruler.top() + 4, x, tracks.bottom() - 2);
    }

    if (m_widget->m_toolMode == TimelineWidget::ToolMode::Razor && m_widget->m_razorHoverFrame >= 0) {
        const int razorX = m_widget->xFromFrame(m_widget->m_razorHoverFrame);
        if (razorX >= content.left() && razorX <= content.right()) {
            painter->setPen(QPen(QColor(QStringLiteral("#a0e0ff")), 2, Qt::DashLine));
            painter->drawLine(razorX, ruler.top(), razorX, tracks.bottom());
        }
    }

    if (m_widget->m_trackDropInGap && m_widget->m_trackDropIndex >= 0 &&
        (m_widget->m_draggedClipIndex >= 0 || m_widget->m_dropFrame >= 0)) {
        int insertionY = layout.trackTop(0) - (TimelineWidget::kTimelineTrackSpacing / 2);
        if (m_widget->m_trackDropIndex >= m_widget->trackCount()) {
            insertionY = layout.trackTop(m_widget->trackCount() - 1) +
                         layout.trackHeight(m_widget->trackCount() - 1) +
                         (TimelineWidget::kTimelineTrackSpacing / 2);
        } else if (m_widget->m_trackDropIndex > 0) {
            insertionY = layout.trackTop(m_widget->m_trackDropIndex) -
                         (TimelineWidget::kTimelineTrackSpacing / 2);
        }
        painter->setPen(QPen(QColor(QStringLiteral("#f7b955")), 2, Qt::DashLine));
        painter->drawLine(content.left() - 8, insertionY, tracks.right() - 10, insertionY);
    }

    const int playheadX = m_widget->xFromFrame(m_widget->m_currentFrame);
    painter->setPen(QPen(QColor(QStringLiteral("#ff6f61")), 3));
    painter->drawLine(playheadX, ruler.top(), playheadX, tracks.bottom());

    painter->setBrush(QColor(QStringLiteral("#ff6f61")));
    painter->setPen(Qt::NoPen);
    painter->drawRoundedRect(QRect(playheadX - 8, ruler.top(), 16, 12), 4, 4);
    if (const RenderSyncMarker* marker = m_widget->renderSyncMarkerAtFrame(m_widget->m_selectedClipId, m_widget->m_currentFrame)) {
        const QString label = marker->action == RenderSyncAction::DuplicateFrame
            ? QStringLiteral("DUP %1").arg(marker->count)
            : QStringLiteral("SKIP %1").arg(marker->count);
        const QRect badgeRect(playheadX + 10, ruler.top() - 2, 58, 16);
        painter->setBrush(marker->action == RenderSyncAction::DuplicateFrame
                             ? QColor(QStringLiteral("#ff5b5b"))
                             : QColor(QStringLiteral("#ff9e3d")));
        painter->drawRoundedRect(badgeRect, 6, 6);
        painter->setPen(QColor(QStringLiteral("#ffffff")));
        painter->drawText(badgeRect, Qt::AlignCenter, label);
    }

    for (const TimelineClip& clip : m_widget->m_clips) {
        for (const RenderSyncMarker& marker : m_widget->m_renderSyncMarkers) {
            if (marker.clipId != clip.id ||
                marker.frame < clip.startFrame ||
                marker.frame >= clip.startFrame + clip.durationFrames) {
                continue;
            }
            const QRect markerRect = m_widget->renderSyncMarkerRect(clip, marker);
            const QColor markerColor = marker.action == RenderSyncAction::DuplicateFrame
                ? QColor(QStringLiteral("#ff5b5b"))
                : QColor(QStringLiteral("#ff9e3d"));
            painter->setPen(QPen(markerColor.darker(135), 1));
            painter->setBrush(QColor(markerColor.red(), markerColor.green(), markerColor.blue(), 230));
            painter->drawRoundedRect(markerRect, 4, 4);
        }
    }
}
