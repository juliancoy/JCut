#include "timeline_widget.h"

namespace {

void drawEyeIcon(QPainter& painter, const QRect& rect, bool enabled, bool interactive) {
    painter.save();
    painter.setRenderHint(QPainter::Antialiasing, true);
    const QColor stroke = interactive
                              ? (enabled ? QColor(QStringLiteral("#eef4fa"))
                                         : QColor(QStringLiteral("#7f8a99")))
                              : QColor(QStringLiteral("#556170"));
    painter.setPen(QPen(stroke, 1.7));
    painter.setBrush(Qt::NoBrush);

    QPainterPath path;
    path.moveTo(rect.left() + rect.width() * 0.10, rect.center().y());
    path.quadTo(rect.center().x(), rect.top() + rect.height() * 0.08,
                rect.right() - rect.width() * 0.10, rect.center().y());
    path.quadTo(rect.center().x(), rect.bottom() - rect.height() * 0.08,
                rect.left() + rect.width() * 0.10, rect.center().y());
    painter.drawPath(path);
    painter.setBrush(stroke);
    painter.drawEllipse(QRectF(rect.center().x() - rect.width() * 0.10,
                               rect.center().y() - rect.height() * 0.10,
                               rect.width() * 0.20,
                               rect.height() * 0.20));
    if (!enabled) {
        painter.setPen(QPen(QColor(QStringLiteral("#ff8c82")), 2.0));
        painter.drawLine(rect.left() + 2, rect.bottom() - 2, rect.right() - 2, rect.top() + 2);
    }
    painter.restore();
}

void drawSpeakerIcon(QPainter& painter, const QRect& rect, bool enabled, bool interactive) {
    painter.save();
    painter.setRenderHint(QPainter::Antialiasing, true);
    const QColor stroke = interactive
                              ? (enabled ? QColor(QStringLiteral("#eef4fa"))
                                         : QColor(QStringLiteral("#7f8a99")))
                              : QColor(QStringLiteral("#556170"));
    painter.setPen(QPen(stroke, 1.7));
    painter.setBrush(Qt::NoBrush);

    QPainterPath speaker;
    speaker.moveTo(rect.left() + rect.width() * 0.18, rect.center().y() - rect.height() * 0.18);
    speaker.lineTo(rect.left() + rect.width() * 0.36, rect.center().y() - rect.height() * 0.18);
    speaker.lineTo(rect.left() + rect.width() * 0.55, rect.top() + rect.height() * 0.18);
    speaker.lineTo(rect.left() + rect.width() * 0.55, rect.bottom() - rect.height() * 0.18);
    speaker.lineTo(rect.left() + rect.width() * 0.36, rect.center().y() + rect.height() * 0.18);
    speaker.lineTo(rect.left() + rect.width() * 0.18, rect.center().y() + rect.height() * 0.18);
    speaker.closeSubpath();
    painter.drawPath(speaker);
    if (enabled) {
        painter.drawArc(QRect(rect.left() + rect.width() * 0.45,
                              rect.top() + rect.height() * 0.18,
                              rect.width() * 0.28,
                              rect.height() * 0.64),
                        -40 * 16,
                        80 * 16);
        painter.drawArc(QRect(rect.left() + rect.width() * 0.52,
                              rect.top() + rect.height() * 0.06,
                              rect.width() * 0.34,
                              rect.height() * 0.88),
                        -40 * 16,
                        80 * 16);
    } else {
        painter.setPen(QPen(QColor(QStringLiteral("#ff8c82")), 2.0));
        painter.drawLine(rect.left() + rect.width() * 0.62,
                         rect.top() + rect.height() * 0.24,
                         rect.right() - 2,
                         rect.bottom() - rect.height() * 0.22);
        painter.drawLine(rect.right() - 2,
                         rect.top() + rect.height() * 0.24,
                         rect.left() + rect.width() * 0.62,
                         rect.bottom() - rect.height() * 0.22);
    }
    painter.restore();
}

} // namespace

void TimelineWidget::paintEvent(QPaintEvent*) {
    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing, true);
    painter.fillRect(rect(), QColor(QStringLiteral("#0f1216")));

    const QRect draw = drawRect();
    const QRect topBar = topBarRect();
    const QRect ruler = rulerRect();
    const QRect tracks = trackRect();
    const QRect sidebar = trackSidebarRect();
    const QRect content = timelineContentRect();
    const QRect exportBar = exportRangeRect();

    painter.setPen(Qt::NoPen);
    painter.setBrush(QColor(QStringLiteral("#171c22")));
    painter.drawRoundedRect(topBar, 10, 10);

    const QRect frameInfoRect(draw.left() + 10, topBar.top() + 4, kTimelineLabelWidth + 44, 18);
    painter.setBrush(QColor(QStringLiteral("#202a34")));
    painter.drawRoundedRect(exportBar, 7, 7);

    painter.setBrush(QColor(QStringLiteral("#4ea1ff")));
    for (const ExportRangeSegment& segment : m_exportRanges) {
        painter.drawRoundedRect(exportSegmentRect(segment), 7, 7);
    }
    painter.setBrush(QColor(QStringLiteral("#fff4c2")));
    for (int i = 0; i < m_exportRanges.size(); ++i) {
        painter.drawRoundedRect(exportHandleRect(i, true), 3, 3);
        painter.drawRoundedRect(exportHandleRect(i, false), 3, 3);
    }
    painter.setPen(QColor(QStringLiteral("#0f1216")));
    QString exportLabel;
    for (int i = 0; i < m_exportRanges.size(); ++i) {
        if (i > 0) {
            exportLabel += QStringLiteral(" | ");
        }
        exportLabel += QStringLiteral("%1 -> %2")
                           .arg(timecodeForFrame(m_exportRanges[i].startFrame))
                           .arg(timecodeForFrame(m_exportRanges[i].endFrame));
    }
    painter.drawText(exportBar.adjusted(10, 0, -10, 0),
                     Qt::AlignCenter,
                     QStringLiteral("Export %1").arg(exportLabel));

    painter.setPen(Qt::NoPen);
    painter.setBrush(QColor(QStringLiteral("#202a34")));
    painter.drawRoundedRect(frameInfoRect, 7, 7);
    painter.setPen(QColor(QStringLiteral("#eef4fa")));
    painter.drawText(frameInfoRect.adjusted(8, 0, -8, 0),
                     Qt::AlignLeft | Qt::AlignVCenter,
                     QStringLiteral("Frame %1").arg(m_currentFrame));

    painter.setPen(Qt::NoPen);
    painter.setBrush(QColor(QStringLiteral("#171c22")));
    painter.drawRoundedRect(tracks, 10, 10);
    painter.setBrush(QColor(QStringLiteral("#151b22")));
    painter.drawRoundedRect(sidebar, 10, 10);
    painter.setBrush(QColor(QStringLiteral("#202a34")));
    painter.drawRoundedRect(QRect(sidebar.left(),
                                  sidebar.top(),
                                  sidebar.width(),
                                  qMin(26, sidebar.height())),
                            10,
                            10);
    painter.setPen(QPen(QColor(QStringLiteral("#2b3744")), 1));
    painter.drawLine(sidebar.right() + (kTimelineLabelGap / 2),
                     tracks.top() + 8,
                     sidebar.right() + (kTimelineLabelGap / 2),
                     tracks.bottom() - 8);

    painter.setPen(QColor(QStringLiteral("#6d7887")));
    for (int64_t frame = 0; frame <= totalFrames(); frame += 30) {
        const int x = xFromFrame(frame);
        const bool major = (frame % 150) == 0;
        painter.setPen(major ? QColor(QStringLiteral("#8fa0b5")) : QColor(QStringLiteral("#53606e")));
        painter.drawLine(x, ruler.bottom() - (major ? 18 : 10), x, tracks.bottom() - 8);

        if (major) {
            painter.drawText(QRect(x + 4, ruler.top(), 56, ruler.height()),
                            Qt::AlignLeft | Qt::AlignVCenter,
                            timecodeForFrame(frame));
        }
    }

    painter.setPen(QColor(QStringLiteral("#24303c")));
    for (int track = 0; track < trackCount(); ++track) {
        const QRect labelRect = trackLabelRect(track);
        const QRect nameRect = trackNameRect(track);
        const bool dragged = track == m_draggedTrackIndex;
        const bool target = track == m_trackDropIndex && m_draggedTrackIndex >= 0 && !m_trackDropInGap;
        const bool selected = track == m_selectedTrackIndex && m_selectedClipId.isEmpty();
        const QColor headerFill =
            dragged ? QColor(QStringLiteral("#ff6f61"))
                    : (target ? QColor(QStringLiteral("#32465f"))
                              : (selected ? QColor(QStringLiteral("#24384d"))
                                          : QColor(QStringLiteral("#192028"))));
        painter.setBrush(headerFill);
        painter.drawRoundedRect(labelRect, 8, 8);
        if (selected) {
            painter.setPen(QPen(QColor(QStringLiteral("#7fc4ff")), 1.4));
            painter.setBrush(Qt::NoBrush);
            painter.drawRoundedRect(labelRect.adjusted(0, 0, -1, -1), 8, 8);
        }
        painter.setPen(QColor(QStringLiteral("#eef4fa")));
        QFont nameFont = painter.font();
        nameFont.setBold(true);
        painter.setFont(nameFont);
        const QString trackLabel = QStringLiteral("%1. %2")
                                       .arg(track + 1)
                                       .arg(m_tracks.value(track).name);
        painter.drawText(nameRect,
                         Qt::AlignLeft | Qt::AlignVCenter,
                         painter.fontMetrics().elidedText(trackLabel, Qt::ElideRight, nameRect.width()));
        painter.setFont(QFont());

        const QRect visualRect = trackVisualToggleRect(track);
        const QRect audioRect = trackAudioToggleRect(track);
        const bool hasVisual = trackHasVisualClips(track);
        const bool hasAudio = trackHasAudioClips(track);
        const bool visualEnabled = trackVisualEnabled(track);
        const bool audioEnabled = trackAudioEnabled(track);
        painter.setPen(Qt::NoPen);
        painter.setBrush(QColor(QStringLiteral("#141a21")));
        painter.drawRoundedRect(visualRect.adjusted(-4, -2, 4, 2), 7, 7);
        painter.drawRoundedRect(audioRect.adjusted(-4, -2, 4, 2), 7, 7);
        drawEyeIcon(painter, visualRect, visualEnabled, hasVisual);
        drawSpeakerIcon(painter, audioRect, audioEnabled, hasAudio);

        painter.setPen(QColor(QStringLiteral("#24303c")));
        const int dividerY = trackTop(track) + trackHeight(track);
        painter.drawLine(sidebar.left() + 6, dividerY, sidebar.right() - 6, dividerY);
        painter.drawLine(content.left() - 8, dividerY, tracks.right() - 10, dividerY);
    }

    for (const TimelineClip& clip : m_clips) {
        const QRect clipRect = clipRectFor(clip);
        const bool audioOnly = clipIsAudioOnly(clip);
        const bool hovered = clip.id == m_hoveredClipId;
        const bool showSourceGhost = clip.mediaType != ClipMediaType::Image;
        const bool visualsEnabled = !clipHasVisuals(clip) || clip.videoEnabled;
        const bool audioEnabled = !clip.hasAudio || clip.audioEnabled;
        if (showSourceGhost) {
            const int64_t ghostStartFrame = qMax<int64_t>(0, clip.startFrame - clip.sourceInFrame);
            const int64_t ghostDurationFrames = qMax<int64_t>(clip.durationFrames, clip.sourceDurationFrames);
            const QRect ghostRect(xFromFrame(ghostStartFrame),
                                  clipRect.y(),
                                  qMax(40, widthForFrames(ghostDurationFrames)),
                                  clipRect.height());

            if (ghostRect != clipRect) {
                painter.setPen(QColor(255, 255, 255, 20));
                painter.setBrush(clip.color.lighter(130));
                painter.setOpacity(0.18);
                painter.drawRoundedRect(ghostRect, 7, 7);
                painter.setOpacity(1.0);
            }
        }

        QColor clipFill = clip.color;
        if (!visualsEnabled || !audioEnabled) {
            clipFill = clipFill.darker(160);
            clipFill.setAlpha(160);
        }
        painter.setPen(QColor(255, 255, 255, 32));
        painter.setBrush(clipFill);
        painter.drawRoundedRect(clipRect, 7, 7);

        if (audioOnly) {
            painter.save();
            const int verticalInset = qMax(5, clipRect.height() / 10);
            const QRect envelopeRect = clipRect.adjusted(8, verticalInset, -8, -verticalInset);
            painter.setPen(Qt::NoPen);
            painter.setBrush(QColor(QStringLiteral("#163946")));
            painter.drawRoundedRect(envelopeRect, 5, 5);
            painter.setClipRect(envelopeRect);
            painter.setPen(QPen(QColor(QStringLiteral("#9fe7f4")), 2));
            painter.drawLine(envelopeRect.left(), envelopeRect.center().y(),
                             envelopeRect.right(), envelopeRect.center().y());
            painter.setPen(Qt::NoPen);
            for (int x = envelopeRect.left(); x < envelopeRect.right(); x += 6) {
                const int idx = (x - envelopeRect.left()) / 6;
                const qreal phase = static_cast<qreal>((idx * 17 + clip.startFrame + clip.sourceInFrame) % 100) / 99.0;
                const qreal shaped = 0.2 + std::abs(std::sin(phase * 6.28318)) * 0.8;
                const int barHeight = qMax(8, qRound(shaped * envelopeRect.height()));
                const QRect barRect(x, envelopeRect.center().y() - barHeight / 2, 4, barHeight);
                painter.setBrush(QColor(QStringLiteral("#f2feff")));
                painter.drawRoundedRect(barRect, 2, 2);
            }
            painter.restore();
        }

        painter.setPen(QColor(QStringLiteral("#f4f7fb")));
        if (clip.id == m_selectedClipId) {
            painter.setPen(QPen(QColor(QStringLiteral("#fff4c2")), 2));
            if (audioOnly) {
                painter.setBrush(Qt::NoBrush);
                painter.drawRoundedRect(clipRect.adjusted(1, 1, -1, -1), 7, 7);
            } else {
                painter.setBrush(clip.color.lighter(108));
                painter.drawRoundedRect(clipRect, 7, 7);
            }
            painter.setPen(Qt::NoPen);
            painter.setBrush(QColor(QStringLiteral("#fff4c2")));
            const int handleInset = qMax(5, clipRect.height() / 10);
            const QRect leftHandle(clipRect.left() + 2, clipRect.top() + handleInset, 4, clipRect.height() - (handleInset * 2));
            const QRect rightHandle(clipRect.right() - 5, clipRect.top() + handleInset, 4, clipRect.height() - (handleInset * 2));
            painter.drawRoundedRect(leftHandle, 2, 2);
            painter.drawRoundedRect(rightHandle, 2, 2);
            painter.setPen(QColor(QStringLiteral("#f4f7fb")));
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
        painter.drawText(clipRect.adjusted(10, 0, -10, 0),
                        Qt::AlignLeft | Qt::AlignVCenter,
                        painter.fontMetrics().elidedText(clipTitle, Qt::ElideRight,
                                                         clipRect.width() - 20));

        if (hovered) {
            const bool isSequence = clip.sourceKind == MediaSourceKind::ImageSequence;
            const bool hasProxy = clipHasProxyAvailable(clip);
            QString badgeText;
            if (clip.mediaType == ClipMediaType::Audio) {
                badgeText = QStringLiteral("AUDIO");
            } else if (clip.mediaType == ClipMediaType::Image) {
                badgeText = QStringLiteral("IMAGE");
            } else if (isSequence) {
                badgeText = QStringLiteral("SEQUENCE");
            } else {
                badgeText = hasProxy ? QStringLiteral("PROXY")
                                     : QStringLiteral("NEEDS PROXY");
            }
            const QFontMetrics badgeMetrics = painter.fontMetrics();
            const int badgeWidth = badgeMetrics.horizontalAdvance(badgeText) + 18;
            const int badgeHeight = 18;
            const QRect badgeRect(clipRect.right() - badgeWidth - 8,
                                  clipRect.top() + 7,
                                  badgeWidth,
                                  badgeHeight);
            painter.setPen(Qt::NoPen);
            painter.setBrush(clip.mediaType == ClipMediaType::Audio
                                 ? QColor(QStringLiteral("#1f3e4c"))
                                 : (clip.mediaType == ClipMediaType::Image
                                 ? QColor(QStringLiteral("#3a2c12"))
                                 : (isSequence ? QColor(QStringLiteral("#1f2f5a"))
                                               : (hasProxy ? QColor(QStringLiteral("#113c28"))
                                                           : QColor(QStringLiteral("#4a3113"))))));
            painter.drawRoundedRect(badgeRect, 9, 9);
            painter.setPen(clip.mediaType == ClipMediaType::Audio
                               ? QColor(QStringLiteral("#cfefff"))
                               : (clip.mediaType == ClipMediaType::Image
                               ? QColor(QStringLiteral("#ffe4a8"))
                               : (isSequence ? QColor(QStringLiteral("#c9d9ff"))
                                             : (hasProxy ? QColor(QStringLiteral("#b8f5cf"))
                                                         : QColor(QStringLiteral("#ffe1a8"))))));
            painter.drawText(badgeRect, Qt::AlignCenter, badgeText);

            const int64_t localTimelineFrame =
                qBound<int64_t>(0,
                                m_currentFrame - clip.startFrame,
                                qMax<int64_t>(0, clip.durationFrames - 1));
            const int64_t clipFrame =
                adjustedClipLocalFrameAtTimelineFrame(clip, localTimelineFrame, m_renderSyncMarkers);
            const QString frameBadgeText = QStringLiteral("FRAME %1").arg(clipFrame);
            const int frameBadgeWidth = badgeMetrics.horizontalAdvance(frameBadgeText) + 18;
            const QRect frameBadgeRect(clipRect.right() - frameBadgeWidth - 8,
                                       badgeRect.bottom() + 6,
                                       frameBadgeWidth,
                                       badgeHeight);
            painter.setPen(Qt::NoPen);
            painter.setBrush(QColor(QStringLiteral("#18303e")));
            painter.drawRoundedRect(frameBadgeRect, 9, 9);
            painter.setPen(QColor(QStringLiteral("#d7f2ff")));
            painter.drawText(frameBadgeRect, Qt::AlignCenter, frameBadgeText);

            if (qAbs(clip.playbackRate - 1.0) > 0.001) {
                const QString speedText = QStringLiteral("SPEED %1x").arg(clip.playbackRate, 0, 'g', 3);
                const int speedBadgeWidth = badgeMetrics.horizontalAdvance(speedText) + 18;
                const QRect speedBadgeRect(clipRect.right() - speedBadgeWidth - 8,
                                           frameBadgeRect.bottom() + 6,
                                           speedBadgeWidth,
                                           badgeHeight);
                painter.setPen(Qt::NoPen);
                painter.setBrush(QColor(QStringLiteral("#3a1f4c")));
                painter.drawRoundedRect(speedBadgeRect, 9, 9);
                painter.setPen(QColor(QStringLiteral("#e8cfff")));
                painter.drawText(speedBadgeRect, Qt::AlignCenter, speedText);
            }
        }

    }

    if (m_dropFrame >= 0) {
        const int x = xFromFrame(m_dropFrame);
        painter.setPen(QPen(QColor(QStringLiteral("#f7b955")), 2, Qt::DashLine));
        painter.drawLine(x, tracks.top() + 2, x, tracks.bottom() - 2);
    }

    if (m_snapIndicatorFrame >= 0) {
        const int x = xFromFrame(m_snapIndicatorFrame);
        painter.setPen(QPen(QColor(QStringLiteral("#ffe082")), 2, Qt::DashLine));
        painter.drawLine(x, ruler.top() + 4, x, tracks.bottom() - 2);
    }

    if (m_toolMode == ToolMode::Razor && m_razorHoverFrame >= 0) {
        const int razorX = xFromFrame(m_razorHoverFrame);
        if (razorX >= content.left() && razorX <= content.right()) {
            painter.setPen(QPen(QColor(QStringLiteral("#a0e0ff")), 2, Qt::DashLine));
            painter.drawLine(razorX, ruler.top(), razorX, tracks.bottom());
        }
    }

    if (m_trackDropInGap && m_trackDropIndex >= 0 && (m_draggedClipIndex >= 0 || m_dropFrame >= 0)) {
        int insertionY = trackTop(0) - (kTimelineTrackSpacing / 2);
        if (m_trackDropIndex >= trackCount()) {
            insertionY = trackTop(trackCount() - 1) + trackHeight(trackCount() - 1) + (kTimelineTrackSpacing / 2);
        } else if (m_trackDropIndex > 0) {
            insertionY = trackTop(m_trackDropIndex) - (kTimelineTrackSpacing / 2);
        }
        painter.setPen(QPen(QColor(QStringLiteral("#f7b955")), 2, Qt::DashLine));
        painter.drawLine(content.left() - 8, insertionY, tracks.right() - 10, insertionY);
    }

    const int playheadX = xFromFrame(m_currentFrame);
    painter.setPen(QPen(QColor(QStringLiteral("#ff6f61")), 3));
    painter.drawLine(playheadX, ruler.top(), playheadX, tracks.bottom());

    painter.setBrush(QColor(QStringLiteral("#ff6f61")));
    painter.setPen(Qt::NoPen);
    painter.drawRoundedRect(QRect(playheadX - 8, ruler.top(), 16, 12), 4, 4);
    if (const RenderSyncMarker* marker = renderSyncMarkerAtFrame(m_selectedClipId, m_currentFrame)) {
        const QString label = marker->action == RenderSyncAction::DuplicateFrame
            ? QStringLiteral("DUP %1").arg(marker->count)
            : QStringLiteral("SKIP %1").arg(marker->count);
        const QRect badgeRect(playheadX + 10, ruler.top() - 2, 58, 16);
        painter.setBrush(marker->action == RenderSyncAction::DuplicateFrame
                             ? QColor(QStringLiteral("#ff5b5b"))
                             : QColor(QStringLiteral("#ff9e3d")));
        painter.drawRoundedRect(badgeRect, 6, 6);
        painter.setPen(QColor(QStringLiteral("#ffffff")));
        painter.drawText(badgeRect, Qt::AlignCenter, label);
    }

    for (const TimelineClip& clip : m_clips) {
        for (const RenderSyncMarker& marker : m_renderSyncMarkers) {
            if (marker.clipId != clip.id ||
                marker.frame < clip.startFrame ||
                marker.frame >= clip.startFrame + clip.durationFrames) {
                continue;
            }
            const QRect markerRect = renderSyncMarkerRect(clip, marker);
            const QColor markerColor = marker.action == RenderSyncAction::DuplicateFrame
                ? QColor(QStringLiteral("#ff5b5b"))
                : QColor(QStringLiteral("#ff9e3d"));
            painter.setPen(QPen(markerColor.darker(135), 1));
            painter.setBrush(QColor(markerColor.red(), markerColor.green(), markerColor.blue(), 230));
            painter.drawRoundedRect(markerRect, 4, 4);
        }
    }
}

void TimelineWidget::setExportRange(int64_t startFrame, int64_t endFrame) {
    setExportRanges({ExportRangeSegment{startFrame, endFrame}});
}

void TimelineWidget::setExportRanges(const QVector<ExportRangeSegment>& ranges) {
    m_exportRanges = ranges;
    normalizeExportRanges();
    update();
}
