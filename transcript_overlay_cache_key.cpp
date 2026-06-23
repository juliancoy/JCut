#include "transcript_overlay_cache_key.h"

QString transcriptOverlayStyleCacheMaterial(const TimelineClip& clip)
{
    const auto& overlay = clip.transcriptOverlay;
    return QStringLiteral("transcript-style-v1|") +
        QString::number(overlay.showBackground ? 1 : 0) + QLatin1Char('|') +
        QString::number(overlay.backgroundOpacity, 'f', 4) + QLatin1Char('|') +
        QString::number(overlay.backgroundCornerRadius, 'f', 2) + QLatin1Char('|') +
        QString::number(overlay.showShadow ? 1 : 0) + QLatin1Char('|') +
        QString::number(overlay.showSpeakerTitle ? 1 : 0) + QLatin1Char('|') +
        overlay.fontFamily + QLatin1Char('|') +
        QString::number(overlay.fontPointSize) + QLatin1Char('|') +
        QString::number(overlay.bold ? 1 : 0) + QLatin1Char('|') +
        QString::number(overlay.italic ? 1 : 0) + QLatin1Char('|') +
        QString::number(static_cast<quint32>(overlay.textColor.rgba())) + QLatin1Char('|') +
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
