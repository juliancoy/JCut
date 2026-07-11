#include "transcript_overlay_cache_key.h"

QString transcriptOverlayStyleCacheMaterial(const TimelineClip& clip)
{
    const auto& overlay = clip.transcriptOverlay;
    return QStringLiteral("transcript-style-v4|") +
        QString::number(overlay.showBackground ? 1 : 0) + QLatin1Char('|') +
        QString::number(overlay.backgroundOpacity, 'f', 4) + QLatin1Char('|') +
        QString::number(overlay.backgroundCornerRadius, 'f', 2) + QLatin1Char('|') +
        QString::number(overlay.backgroundPadding, 'f', 2) + QLatin1Char('|') +
        QString::number(overlay.backgroundFrameEnabled ? 1 : 0) + QLatin1Char('|') +
        QString::number(static_cast<quint32>(overlay.backgroundFrameColor.rgba())) + QLatin1Char('|') +
        QString::number(overlay.backgroundFrameOpacity, 'f', 4) + QLatin1Char('|') +
        QString::number(overlay.backgroundFrameWidth, 'f', 2) + QLatin1Char('|') +
        QString::number(overlay.backgroundFrameGap, 'f', 2) + QLatin1Char('|') +
        QString::number(overlay.showShadow ? 1 : 0) + QLatin1Char('|') +
        QString::number(static_cast<quint32>(overlay.shadowColor.rgba())) + QLatin1Char('|') +
        QString::number(overlay.shadowOpacity, 'f', 4) + QLatin1Char('|') +
        QString::number(overlay.shadowOffsetX, 'f', 2) + QLatin1Char('|') +
        QString::number(overlay.shadowOffsetY, 'f', 2) + QLatin1Char('|') +
        QString::number(overlay.textOutlineEnabled ? 1 : 0) + QLatin1Char('|') +
        QString::number(overlay.textOutlineWidth, 'f', 2) + QLatin1Char('|') +
        QString::number(static_cast<quint32>(overlay.textOutlineColor.rgba())) + QLatin1Char('|') +
        QString::number(overlay.textOutlineOpacity, 'f', 4) + QLatin1Char('|') +
        QString::number(static_cast<int>(overlay.textExtrudeMode)) + QLatin1Char('|') +
        QString::number(overlay.textExtrudeDepth, 'f', 3) + QLatin1Char('|') +
        QString::number(overlay.textExtrudeBevelScale, 'f', 3) + QLatin1Char('|') +
        QString::number(overlay.showSpeakerTitle ? 1 : 0) + QLatin1Char('|') +
        QString::number(overlay.highlightCurrentWord ? 1 : 0) + QLatin1Char('|') +
        overlay.fontFamily + QLatin1Char('|') +
        QString::number(overlay.fontPointSize) + QLatin1Char('|') +
        QString::number(overlay.bold ? 1 : 0) + QLatin1Char('|') +
        QString::number(overlay.italic ? 1 : 0) + QLatin1Char('|') +
        QString::number(static_cast<quint32>(overlay.textColor.rgba())) + QLatin1Char('|') +
        QString::number(overlay.textOpacity, 'f', 4) + QLatin1Char('|') +
        QString::number(static_cast<quint32>(overlay.backgroundColor.rgba())) + QLatin1Char('|') +
        QString::number(static_cast<quint32>(overlay.highlightColor.rgba())) + QLatin1Char('|') +
        QString::number(static_cast<quint32>(overlay.highlightTextColor.rgba())) + QLatin1Char('|') +
        QString::number(overlay.useManualPlacement ? 1 : 0) + QLatin1Char('|') +
        QString::number(overlay.autoScroll ? 1 : 0) + QLatin1Char('|') +
        QString::number(overlay.maxLines) + QLatin1Char('|') +
        QString::number(overlay.maxCharsPerLine) + QLatin1Char('|') +
        QString::number(overlay.translationX, 'f', 4) + QLatin1Char('|') +
        QString::number(overlay.translationY, 'f', 4) + QLatin1Char('|') +
        QString::number(overlay.boxWidth, 'f', 2) + QLatin1Char('|') +
        QString::number(overlay.boxHeight, 'f', 2);
}
