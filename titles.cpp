#include "titles.h"

#include <QUuid>
#include <cmath>

EvaluatedTitle evaluateTitleAtLocalFrame(const TimelineClip& clip, int64_t localFrame)
{
    EvaluatedTitle result;
    if (clip.titleKeyframes.isEmpty()) {
        return result;
    }

    // Find the keyframe at or before localFrame (step interpolation for text,
    // linear interpolation for numeric properties when enabled).
    int beforeIdx = 0;
    int afterIdx = -1;
    for (int i = 0; i < clip.titleKeyframes.size(); ++i) {
        if (clip.titleKeyframes[i].frame <= localFrame) {
            beforeIdx = i;
        } else if (afterIdx < 0) {
            afterIdx = i;
        }
    }

    const auto& kf = clip.titleKeyframes[beforeIdx];
    result.text = kf.text;
    // Title clips use their own coordinate system in titleKeyframes directly.
    // moveRequested writes to titleKeyframes, not baseTranslation.
    result.x = kf.translationX;
    result.y = kf.translationY;
    result.fontSize = kf.fontSize;
    result.opacity = kf.opacity;
    result.fontFamily = kf.fontFamily;
    result.bold = kf.bold;
    result.italic = kf.italic;
    result.color = kf.color;
    result.dropShadowEnabled = kf.dropShadowEnabled;
    result.dropShadowColor = kf.dropShadowColor;
    result.dropShadowOpacity = kf.dropShadowOpacity;
    result.dropShadowOffsetX = kf.dropShadowOffsetX;
    result.dropShadowOffsetY = kf.dropShadowOffsetY;
    result.windowEnabled = kf.windowEnabled;
    result.windowColor = kf.windowColor;
    result.windowOpacity = kf.windowOpacity;
    result.windowPadding = kf.windowPadding;
    result.windowFrameEnabled = kf.windowFrameEnabled;
    result.windowFrameColor = kf.windowFrameColor;
    result.windowFrameOpacity = kf.windowFrameOpacity;
    result.windowFrameWidth = kf.windowFrameWidth;
    result.windowFrameGap = kf.windowFrameGap;
    result.valid = true;

    // Linear interpolation of numeric properties between keyframes.
    // Match the other keyframe evaluators: the segment interpolation mode
    // is owned by the destination (next) keyframe.
    if (afterIdx >= 0) {
        const auto& next = clip.titleKeyframes[afterIdx];
        if (!next.linearInterpolation) {
            return result;
        }
        const int64_t span = next.frame - kf.frame;
        if (span > 0) {
            const qreal t = static_cast<qreal>(localFrame - kf.frame) / static_cast<qreal>(span);
            result.x = kf.translationX + (next.translationX - kf.translationX) * t;
            result.y = kf.translationY + (next.translationY - kf.translationY) * t;
            result.fontSize = kf.fontSize + (next.fontSize - kf.fontSize) * t;
            result.opacity = kf.opacity + (next.opacity - kf.opacity) * t;
            // Text, font, bold, italic, color are NOT interpolated — they step at the keyframe
        }
    }

    return result;
}

EvaluatedTitle composeTitleWithOpacity(const EvaluatedTitle& title, qreal opacityMultiplier)
{
    EvaluatedTitle composed = title;
    if (!composed.valid) {
        return composed;
    }
    composed.opacity = qBound<qreal>(0.0, composed.opacity * opacityMultiplier, 1.0);
    return composed;
}

TitleLayoutMetrics measureTitleLayout(const QFont& font, const QString& text)
{
    TitleLayoutMetrics metrics;
    const QFontMetricsF fm(font);
    const QStringList lines = text.split(QLatin1Char('\n'), Qt::KeepEmptyParts);
    metrics.lineCount = qMax(1, lines.size());
    metrics.lineHeight = fm.lineSpacing();
    metrics.height = metrics.lineHeight * metrics.lineCount;

    qreal maxWidth = 0.0;
    for (const QString& line : lines) {
        maxWidth = qMax(maxWidth, fm.horizontalAdvance(line));
    }
    metrics.width = maxWidth;
    return metrics;
}

TimelineClip createDefaultTitleClip(int64_t startFrame, int trackIndex, int64_t durationFrames)
{
    TimelineClip clip;
    clip.id = QUuid::createUuid().toString(QUuid::WithoutBraces);
    clip.mediaType = ClipMediaType::Title;
    clip.label = QStringLiteral("Title");
    clip.startFrame = startFrame;
    clip.trackIndex = trackIndex;
    clip.durationFrames = durationFrames;
    clip.sourceDurationFrames = durationFrames;
    clip.videoEnabled = true;
    clip.audioEnabled = false;
    clip.hasAudio = false;
    clip.color = QColor(QStringLiteral("#4a2d6b"));

    TimelineClip::TitleKeyframe defaultKeyframe;
    defaultKeyframe.frame = 0;
    defaultKeyframe.text = QStringLiteral("Title");
    defaultKeyframe.translationX = 0.0;
    defaultKeyframe.translationY = 0.0;
    defaultKeyframe.fontSize = 48.0;
    defaultKeyframe.opacity = 1.0;
    clip.titleKeyframes.push_back(defaultKeyframe);

    return clip;
}

void drawTitleOverlay(QPainter* painter, const QRect& canvasRect,
                      const EvaluatedTitle& title, const QSize& outputSize)
{
    if (!painter || !title.valid || title.text.isEmpty()) {
        return;
    }
    if (title.opacity <= 0.001) {
        return;
    }

    painter->save();

    // Scale from output-canvas coordinates to widget-pixel coordinates
    const qreal scaleX = outputSize.width() > 0
        ? static_cast<qreal>(canvasRect.width()) / outputSize.width() : 1.0;
    const qreal scaleY = outputSize.height() > 0
        ? static_cast<qreal>(canvasRect.height()) / outputSize.height() : 1.0;

    QFont font(title.fontFamily);
    font.setPointSizeF(title.fontSize * qMin(scaleX, scaleY));
    font.setBold(title.bold);
    font.setItalic(title.italic);
    painter->setFont(font);

    QColor textColor = title.color;
    textColor.setAlphaF(title.opacity);
    painter->setPen(textColor);

    // Position is relative to the canvas center, stored in output-canvas units.
    // Scale to widget pixels for rendering.
    const qreal centerX = canvasRect.center().x() + title.x * scaleX;
    const qreal centerY = canvasRect.center().y() + title.y * scaleY;

    const TitleLayoutMetrics metrics = measureTitleLayout(font, title.text);
    const QStringList lines = title.text.split(QLatin1Char('\n'), Qt::KeepEmptyParts);
    const QFontMetricsF fm(font);
    const qreal topY = centerY - (metrics.height / 2.0);
    const qreal windowPaddingPx = qMax<qreal>(0.0, title.windowPadding * qMin(scaleX, scaleY));

    const QRectF windowRect(centerX - (metrics.width / 2.0) - windowPaddingPx,
                            topY - windowPaddingPx,
                            metrics.width + (windowPaddingPx * 2.0),
                            metrics.height + (windowPaddingPx * 2.0));

    if (title.windowEnabled) {
        QColor windowColor = title.windowColor;
        windowColor.setAlphaF(qBound<qreal>(0.0, title.opacity * title.windowOpacity, 1.0));
        painter->setPen(Qt::NoPen);
        painter->setBrush(windowColor);
        painter->drawRect(windowRect);
    }

    if (title.windowFrameEnabled) {
        const qreal frameGapPx = qMax<qreal>(0.0, title.windowFrameGap * qMin(scaleX, scaleY));
        const qreal frameWidthPx = qMax<qreal>(0.0, title.windowFrameWidth * qMin(scaleX, scaleY));
        QColor frameColor = title.windowFrameColor;
        frameColor.setAlphaF(qBound<qreal>(0.0, title.opacity * title.windowFrameOpacity, 1.0));
        QPen framePen(frameColor);
        framePen.setWidthF(frameWidthPx);
        painter->setPen(framePen);
        painter->setBrush(Qt::NoBrush);
        painter->drawRect(windowRect.adjusted(-frameGapPx, -frameGapPx, frameGapPx, frameGapPx));
    }

    if (title.dropShadowEnabled) {
        QColor shadowColor = title.dropShadowColor;
        shadowColor.setAlphaF(qBound<qreal>(0.0, title.opacity * title.dropShadowOpacity, 1.0));
        painter->setPen(shadowColor);
        for (int i = 0; i < lines.size(); ++i) {
            const QString& line = lines[i];
            const qreal lineWidth = fm.horizontalAdvance(line);
            const qreal baselineY = topY + (i * metrics.lineHeight) + fm.ascent();
            painter->drawText(QPointF(centerX - (lineWidth / 2.0) + title.dropShadowOffsetX,
                                      baselineY + title.dropShadowOffsetY),
                              line);
        }
    }

    painter->setPen(textColor);
    for (int i = 0; i < lines.size(); ++i) {
        const QString& line = lines[i];
        const qreal lineWidth = fm.horizontalAdvance(line);
        const qreal baselineY = topY + (i * metrics.lineHeight) + fm.ascent();
        painter->drawText(QPointF(centerX - (lineWidth / 2.0), baselineY), line);
    }

    painter->restore();
}
