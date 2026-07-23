#include "timeline_renderer.h"
#include "timeline_widget.h"
#include "timeline_layout.h"
#include "timeline_fps.h"
#include "debug_controls.h"
#include "editor_shared_media.h"
#include "editor_shared_timing.h"
#include "timeline_clip_title.h"
#include "waveform_service.h"

#include <QFileInfo>
#include <algorithm>
#include <cmath>

namespace {

void drawClipAudioWaveform(QPainter* painter,
                           const TimelineClip& clip,
                           const QRect& clipRect,
                           const QRect& visibleClipRect,
                           bool audioOnly)
{
    if (!painter || visibleClipRect.isEmpty()) {
        return;
    }

    painter->save();
    const int verticalInset = audioOnly
        ? qMax(5, visibleClipRect.height() / 10)
        : qBound(6, visibleClipRect.height() / 5, 10);
    const QRect envelopeRect = visibleClipRect.adjusted(8, verticalInset, -8, -verticalInset);
    if (envelopeRect.isEmpty()) {
        painter->restore();
        return;
    }

    painter->setPen(Qt::NoPen);
    QColor background = audioOnly
        ? QColor(QStringLiteral("#163946"))
        : QColor(12, 26, 32, 150);
    painter->setBrush(background);
    painter->drawRoundedRect(envelopeRect, 5, 5);
    painter->setClipRect(envelopeRect);

    const QColor baselineColor = audioOnly
        ? QColor(QStringLiteral("#9fe7f4"))
        : QColor(159, 231, 244, 145);
    painter->setPen(QPen(baselineColor, audioOnly ? 2.0 : 1.0));
    painter->drawLine(envelopeRect.left(), envelopeRect.center().y(),
                      envelopeRect.right(), envelopeRect.center().y());

    const QString audioPath = playbackAudioPathForClip(clip);
    const int64_t clipSampleSpan = clipTimelineDurationSamples(clip);
    const qreal visibleStartRatio = qBound<qreal>(
        0.0,
        static_cast<qreal>(visibleClipRect.left() - clipRect.left()) /
            qMax<qreal>(1.0, static_cast<qreal>(clipRect.width())),
        1.0);
    const qreal visibleEndRatio = qBound<qreal>(
        visibleStartRatio,
        static_cast<qreal>(visibleClipRect.right() - clipRect.left() + 1) /
            qMax<qreal>(1.0, static_cast<qreal>(clipRect.width())),
        1.0);
    const int64_t sourceStartSample = qMax<int64_t>(0, clipSourceInSamples(clip));
    const int64_t visibleSampleStart = sourceStartSample + static_cast<int64_t>(
        std::floor(static_cast<qreal>(clipSampleSpan) * visibleStartRatio));
    const int64_t visibleSampleEnd = sourceStartSample + static_cast<int64_t>(
        std::ceil(static_cast<qreal>(clipSampleSpan) * visibleEndRatio));

    QVector<float> minVals;
    QVector<float> maxVals;
    const int columns = qMax(1, envelopeRect.width());
    const bool hasWaveform = !audioPath.isEmpty() &&
        editor::WaveformService::instance().queryEnvelope(
            audioPath,
            visibleSampleStart,
            qMax<int64_t>(visibleSampleStart + 1, visibleSampleEnd),
            columns,
            &minVals,
            &maxVals);

    if (hasWaveform && minVals.size() == columns && maxVals.size() == columns) {
        const QColor waveformColor = audioOnly
            ? QColor(QStringLiteral("#f2feff"))
            : QColor(242, 254, 255, 210);
        painter->setPen(QPen(waveformColor, 1.0));
        for (int i = 0; i < columns; ++i) {
            const int x = envelopeRect.left() + i;
            const qreal minAmp = qBound<qreal>(-1.0, static_cast<qreal>(minVals[i]), 1.0);
            const qreal maxAmp = qBound<qreal>(-1.0, static_cast<qreal>(maxVals[i]), 1.0);
            const int yTop = qRound(envelopeRect.center().y() - (maxAmp * envelopeRect.height() * 0.5));
            const int yBottom = qRound(envelopeRect.center().y() - (minAmp * envelopeRect.height() * 0.5));
            painter->drawLine(x, yTop, x, yBottom);
        }
    } else {
        const QColor pendingColor = audioOnly
            ? QColor(QStringLiteral("#8fc8d3"))
            : QColor(143, 200, 211, 160);
        painter->setPen(QPen(pendingColor, 1.0, Qt::DashLine));
        painter->drawLine(envelopeRect.left(),
                          envelopeRect.center().y(),
                          envelopeRect.right(),
                          envelopeRect.center().y());
    }

    painter->restore();
}

qreal linearColorChannel(int channel)
{
    const qreal normalized = static_cast<qreal>(channel) / 255.0;
    return normalized <= 0.04045
        ? normalized / 12.92
        : std::pow((normalized + 0.055) / 1.055, 2.4);
}

qreal relativeLuminance(const QColor& color)
{
    return 0.2126 * linearColorChannel(color.red()) +
           0.7152 * linearColorChannel(color.green()) +
           0.0722 * linearColorChannel(color.blue());
}

qreal contrastRatio(const QColor& first, const QColor& second)
{
    const qreal lighter = qMax(relativeLuminance(first), relativeLuminance(second));
    const qreal darker = qMin(relativeLuminance(first), relativeLuminance(second));
    return (lighter + 0.05) / (darker + 0.05);
}

QColor opaqueColorOver(const QColor& foreground, const QColor& background)
{
    const qreal alpha = foreground.alphaF();
    return QColor::fromRgbF(
        foreground.redF() * alpha + background.redF() * (1.0 - alpha),
        foreground.greenF() * alpha + background.greenF() * (1.0 - alpha),
        foreground.blueF() * alpha + background.blueF() * (1.0 - alpha));
}

QColor readableTitleColor(const QColor& clipFill)
{
    const QColor light(Qt::white);
    const QColor dark(Qt::black);
    return contrastRatio(light, clipFill) >= contrastRatio(dark, clipFill)
        ? light
        : dark;
}

} // namespace

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

    const int64_t visibleStartFrame = qMax<int64_t>(0, m_widget->frameFromX(content.left()));
    const int64_t visibleEndFrame =
        qMax<int64_t>(visibleStartFrame, m_widget->frameFromX(content.right() + 1));
    const int64_t rulerStartFrame =
        qMax<int64_t>(0, (visibleStartFrame / kTimelineFps) * kTimelineFps);
    const int64_t rulerEndFrame =
        qMin<int64_t>(m_widget->totalFrames(), visibleEndFrame + kTimelineFps);

    int64_t rulerStepFrames = kTimelineFps;
    if (m_widget->m_pixelsPerFrame > 0.0) {
        // Keep ruler painting bounded even when zoom is extremely far out.
        const qreal targetSpacingPx = 10.0;
        const int64_t minFramesForSpacing =
            static_cast<int64_t>(std::ceil(targetSpacingPx / m_widget->m_pixelsPerFrame));
        if (minFramesForSpacing > kTimelineFps) {
            const int64_t snapped =
                ((minFramesForSpacing + (kTimelineFps - 1)) / kTimelineFps) * kTimelineFps;
            rulerStepFrames = qMax<int64_t>(kTimelineFps, snapped);
        }
    }
    const int kMaxRulerTicks = 4000;
    const int64_t maxRulerSpan = static_cast<int64_t>(kMaxRulerTicks) * rulerStepFrames;
    const int64_t boundedRulerEnd = qMin<int64_t>(rulerEndFrame, rulerStartFrame + maxRulerSpan);

    painter->setPen(QColor(QStringLiteral("#6d7887")));
    for (int64_t frame = rulerStartFrame; frame <= boundedRulerEnd; frame += rulerStepFrames) {
        const int x = m_widget->xFromFrame(frame);
        const bool major = (frame % qMax<int64_t>(150, rulerStepFrames * 5)) == 0;
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
    const TimelineClipTitleModel clipTitleModel(m_widget->m_clips, m_widget->m_tracks);
    for (const TimelineClip& clip : m_widget->m_clips) {
        const QRect clipRect = layout.clipRectFor(clip);
        if (clipRect.right() < content.left() || clipRect.left() > content.right()) {
            continue;
        }
        const QRect visibleClipRect = clipRect.intersected(content.adjusted(-2, 0, 2, 0));
        if (visibleClipRect.isEmpty()) {
            continue;
        }
        const bool audioOnly = clipIsAudioOnly(clip);
        const bool hovered = clip.id == m_widget->m_hoveredClipId;
        const bool selected = m_widget->isClipSelected(clip.id);
        const bool primarySelected = selected && (clip.id == m_widget->selectedClipId());
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

            if (ghostRect != clipRect &&
                !(ghostRect.right() < content.left() || ghostRect.left() > content.right())) {
                painter->setPen(QColor(255, 255, 255, 20));
                painter->setBrush(clip.color.lighter(130));
                painter->setOpacity(0.18);
                painter->drawRoundedRect(ghostRect.intersected(content.adjusted(-2, 0, 2, 0)), 7, 7);
                painter->setOpacity(1.0);
            }
        }

        QColor clipFill = clip.color;
        if (clip.clipRole == ClipRole::MaskMatte) {
            clipFill = QColor(QStringLiteral("#2f4056"));
        }
        if (!visualsEnabled || !audioEnabled) {
            clipFill = clipFill.darker(160);
            clipFill.setAlpha(160);
        }
        QColor titleFill = opaqueColorOver(
            clipFill, QColor(QStringLiteral("#171c22")));
        painter->setPen(QColor(255, 255, 255, 32));
        painter->setBrush(clipFill);
        painter->drawRoundedRect(visibleClipRect, 7, 7);
        if (hovered && !selected) {
            painter->setPen(QPen(QColor(QStringLiteral("#7ad7ff")), 2));
            painter->setBrush(Qt::NoBrush);
            painter->drawRoundedRect(visibleClipRect.adjusted(1, 1, -1, -1), 7, 7);
        }

        painter->setPen(QColor(QStringLiteral("#f4f7fb")));
        if (selected) {
            const QColor outerBorder = primarySelected
                ? QColor(QStringLiteral("#ffd23f"))
                : QColor(QStringLiteral("#ffe89a"));
            const QColor innerBorder = primarySelected
                ? QColor(QStringLiteral("#fff8d9"))
                : QColor(QStringLiteral("#fff1c9"));
            painter->setPen(QPen(outerBorder, primarySelected ? 4 : 3));
            painter->setBrush(Qt::NoBrush);
            painter->drawRoundedRect(visibleClipRect.adjusted(-1, -1, 1, 1), 8, 8);
            painter->setPen(QPen(innerBorder, 2));
            painter->drawRoundedRect(visibleClipRect.adjusted(1, 1, -1, -1), 7, 7);
            if (audioOnly) {
                painter->setPen(QPen(innerBorder, 2));
                painter->drawRoundedRect(visibleClipRect.adjusted(2, 2, -2, -2), 6, 6);
            } else {
                const QColor selectedFill = clip.color.lighter(108);
                titleFill = opaqueColorOver(selectedFill, titleFill);
                painter->setBrush(selectedFill);
                painter->drawRoundedRect(visibleClipRect, 7, 7);
            }
            painter->setPen(Qt::NoPen);
            painter->setBrush(outerBorder);
            const int handleInset = qMax(5, clipRect.height() / 10);
            const QRect leftHandle(clipRect.left() + 2, clipRect.top() + handleInset, 4, clipRect.height() - (handleInset * 2));
            const QRect rightHandle(clipRect.right() - 5, clipRect.top() + handleInset, 4, clipRect.height() - (handleInset * 2));
            painter->drawRoundedRect(leftHandle, 2, 2);
            painter->drawRoundedRect(rightHandle, 2, 2);
            if (primarySelected) {
                const QRect anchorRect(visibleClipRect.left() + 6, visibleClipRect.top() + 4, 10, 4);
                painter->drawRoundedRect(anchorRect, 2, 2);
            }
            painter->setPen(QColor(QStringLiteral("#f4f7fb")));
        }

        bool showAudioTabTrackWaveform = false;
        if (m_widget->m_audioTabWaveformsVisible && clip.hasAudio &&
            clip.trackIndex >= 0 && clip.trackIndex < m_widget->m_tracks.size()) {
            showAudioTabTrackWaveform = m_widget->m_tracks[clip.trackIndex].audioWaveformVisible;
        }
        const bool showWaveform =
            showAudioTabTrackWaveform || (!m_widget->m_audioTabWaveformsVisible && audioOnly);
        if (showWaveform) {
            drawClipAudioWaveform(painter, clip, clipRect, visibleClipRect, audioOnly);
        }

        // Status decorations are an isolated paint layer. In particular, none
        // of their NoPen/brush state may leak into the title layer below.
        painter->save();
        const int barHeight = qBound(3, visibleClipRect.height() / 10, 6);
        int barBottom = visibleClipRect.bottom() - 1;

        const bool alternateAudioInUse = playbackUsesAlternateAudioSource(clip);
        if (alternateAudioInUse) {
            const QRect alternateAudioBarRect(visibleClipRect.left() + 2,
                                              barBottom - barHeight + 1,
                                              qMax(1, visibleClipRect.width() - 4),
                                              barHeight);
            painter->setPen(Qt::NoPen);
            painter->setBrush(QColor(QStringLiteral("#36c96a")));
            painter->drawRoundedRect(alternateAudioBarRect, 2, 2);
            barBottom -= (barHeight + 1);
        }

        if (clip.clipRole == ClipRole::MaskMatte) {
            const QRect zBarRect(visibleClipRect.left() + 2,
                                 visibleClipRect.top() + 2,
                                 qMax(1, visibleClipRect.width() - 4),
                                 qBound(3, visibleClipRect.height() / 8, 6));
            painter->setPen(Qt::NoPen);
            painter->setBrush(QColor(QStringLiteral("#62d2ff")));
            painter->drawRoundedRect(zBarRect, 2, 2);
        }

        const QString transcriptPath = transcriptWorkingPathForClip(clip);
        const bool transcriptExists =
            !transcriptPath.isEmpty() && QFileInfo::exists(transcriptPath);
        if (transcriptExists) {
            const QRect transcriptBarRect(visibleClipRect.left() + 2,
                                          barBottom - barHeight + 1,
                                          qMax(1, visibleClipRect.width() - 4),
                                          barHeight);
            painter->setPen(Qt::NoPen);
            painter->setBrush(QColor(QStringLiteral("#3d8bff")));
            painter->drawRoundedRect(transcriptBarRect, 2, 2);
            barBottom -= (barHeight + 1);
        }

        const bool facedetectionsSidecarExists = facedetectionsSidecarExistsForClip(clip);
        if (facedetectionsSidecarExists) {
            const QRect facedetectionsBarRect(visibleClipRect.left() + 2,
                                          barBottom - barHeight + 1,
                                          qMax(1, visibleClipRect.width() - 4),
                                          barHeight);
            painter->setPen(Qt::NoPen);
            painter->setBrush(QColor(QStringLiteral("#c35cff")));
            painter->drawRoundedRect(facedetectionsBarRect, 2, 2);
            barBottom -= (barHeight + 1);
        }

        const bool proxyInUse = !clip.proxyPath.trimmed().isEmpty() && QFileInfo::exists(clip.proxyPath);
        if (proxyInUse) {
            const QRect proxyBarRect(visibleClipRect.left() + 2,
                                     barBottom - barHeight + 1,
                                     qMax(1, visibleClipRect.width() - 4),
                                     barHeight);
            painter->setPen(Qt::NoPen);
            painter->setBrush(QColor(QStringLiteral("#ff9e3d")));
            painter->drawRoundedRect(proxyBarRect, 2, 2);
        }
        painter->restore();

        const TimelineClipTitlePresentation title = clipTitleModel.describe(clip);
        const QRect titleRect = visibleClipRect.adjusted(10, 2, -10, -2);
        if (!titleRect.isEmpty()) {
            painter->save();
            painter->setClipRect(titleRect, Qt::IntersectClip);

            QFont labelFont = painter->font();
            labelFont.setWeight(QFont::DemiBold);
            const QFontMetrics labelMetrics(labelFont);
            QFont badgeFont = labelFont;
            badgeFont.setPointSizeF(qMax<qreal>(7.0, labelFont.pointSizeF() - 1.0));
            badgeFont.setWeight(QFont::Bold);
            const QFontMetrics badgeMetrics(badgeFont);
            const int badgeWidth = badgeMetrics.horizontalAdvance(title.badge) + 12;
            const int badgeHeight = qMin(titleRect.height(), badgeMetrics.height() + 4);
            const int badgeGap = 7;
            const int minimumPrimaryWidth = qMax(
                52, labelMetrics.horizontalAdvance(title.primary.left(5)));
            const bool badgeFits =
                titleRect.width() >= badgeWidth + badgeGap + minimumPrimaryWidth;
            // Audio waveforms can vary from nearly black to white beneath the
            // title. A compact, near-opaque backplate keeps the text contrast
            // deterministic without hiding the rest of the envelope.
            const QColor waveformTitleBackplate(10, 15, 20, 220);
            const QColor titleColor = showWaveform
                ? QColor(QStringLiteral("#ffffff"))
                : readableTitleColor(titleFill);
            const auto drawWaveformTitleBackplate =
                [&](const QRect& textRect, const QString& text) {
                    if (!showWaveform || textRect.isEmpty() || text.isEmpty()) {
                        return;
                    }
                    const int backplateWidth = qMin(
                        textRect.width(), labelMetrics.horizontalAdvance(text) + 8);
                    const int backplateHeight = qMin(
                        textRect.height(), labelMetrics.height() + 4);
                    const QRect backplateRect(
                        textRect.left() - 4,
                        textRect.center().y() - backplateHeight / 2,
                        backplateWidth + 4,
                        backplateHeight);
                    painter->setPen(Qt::NoPen);
                    painter->setBrush(waveformTitleBackplate);
                    painter->drawRoundedRect(backplateRect, 4, 4);
                };

            if (badgeFits) {
                const QRect badgeRect(titleRect.left(),
                                      titleRect.center().y() - badgeHeight / 2,
                                      badgeWidth,
                                      badgeHeight);
                QColor badgeColor(QStringLiteral("#315b7d"));
                if (title.badge == QStringLiteral("MASK")) {
                    badgeColor = QColor(QStringLiteral("#14738f"));
                } else if (title.badge == QStringLiteral("SOURCE")) {
                    badgeColor = QColor(QStringLiteral("#6b3e78"));
                } else if (title.badge == QStringLiteral("AUDIO")) {
                    badgeColor = QColor(QStringLiteral("#26735d"));
                } else if (title.badge == QStringLiteral("TITLE")) {
                    badgeColor = QColor(QStringLiteral("#8a5b25"));
                } else if (title.badge == QStringLiteral("FX")) {
                    badgeColor = QColor(QStringLiteral("#704b91"));
                }
                const QRect labelRect = titleRect.adjusted(badgeWidth + badgeGap, 0, 0, 0);
                const QString labelText = labelMetrics.elidedText(
                    title.inlineText(), Qt::ElideRight, qMax(1, labelRect.width()));
                drawWaveformTitleBackplate(labelRect, labelText);

                painter->setPen(Qt::NoPen);
                painter->setBrush(badgeColor);
                painter->drawRoundedRect(badgeRect, 5, 5);
                painter->setFont(badgeFont);
                painter->setPen(QColor(QStringLiteral("#ffffff")));
                painter->drawText(badgeRect, Qt::AlignCenter, title.badge);

                painter->setFont(labelFont);
                painter->setPen(titleColor);
                painter->drawText(
                    labelRect,
                    Qt::AlignLeft | Qt::AlignVCenter,
                    labelText);
            } else {
                painter->setFont(labelFont);
                const QString compactTitle =
                    QStringLiteral("%1 %2").arg(title.badge, title.primary);
                const QString compactText = labelMetrics.elidedText(
                    compactTitle, Qt::ElideRight, qMax(1, titleRect.width()));
                drawWaveformTitleBackplate(titleRect, compactText);
                painter->setPen(titleColor);
                painter->drawText(
                    titleRect,
                    Qt::AlignLeft | Qt::AlignVCenter,
                    compactText);
            }
            painter->restore();
        }
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
    if (const RenderSyncMarker* marker = m_widget->renderSyncMarkerAtFrame(m_widget->selectedClipId(), m_widget->m_currentFrame)) {
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
        const QVector<RenderSyncMarker>* clipMarkers = m_widget->renderSyncMarkersForClipId(clip.id);
        if (!clipMarkers) {
            continue;
        }
        const int64_t clipVisibleStart = qMax<int64_t>(clip.startFrame, visibleStartFrame);
        const int64_t clipVisibleEnd =
            qMin<int64_t>(clip.startFrame + clip.durationFrames, visibleEndFrame + 1);
        if (clipVisibleEnd <= clipVisibleStart) {
            continue;
        }
        auto markerIt = std::lower_bound(clipMarkers->begin(),
                                         clipMarkers->end(),
                                         clipVisibleStart,
                                         [](const RenderSyncMarker& marker, int64_t frame) {
                                             return marker.frame < frame;
                                         });
        for (; markerIt != clipMarkers->end(); ++markerIt) {
            const RenderSyncMarker& marker = *markerIt;
            if (marker.frame >= clipVisibleEnd) {
                break;
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
