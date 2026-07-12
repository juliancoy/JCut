#pragma once

#include <QString>

enum class BackgroundFillEffect {
    EdgeStretch,
    ProgressiveEdgeStretch,
    Mirror,
    BlurCover,
};

inline constexpr BackgroundFillEffect kDefaultBackgroundFillEffect = BackgroundFillEffect::EdgeStretch;

inline QString backgroundFillEffectToString(BackgroundFillEffect effect)
{
    switch (effect) {
    case BackgroundFillEffect::EdgeStretch:
        return QStringLiteral("edge_stretch");
    case BackgroundFillEffect::ProgressiveEdgeStretch:
        return QStringLiteral("progressive_edge_stretch");
    case BackgroundFillEffect::Mirror:
        return QStringLiteral("mirror");
    case BackgroundFillEffect::BlurCover:
        return QStringLiteral("blur_cover");
    }
    return QStringLiteral("edge_stretch");
}

inline BackgroundFillEffect backgroundFillEffectFromString(const QString& value)
{
    const QString normalized = value.trimmed().toLower().replace(QLatin1Char('-'), QLatin1Char('_'));
    if (normalized == QStringLiteral("blur") ||
        normalized == QStringLiteral("blur_cover") ||
        normalized == QStringLiteral("blurred_cover")) {
        return BackgroundFillEffect::BlurCover;
    }
    if (normalized == QStringLiteral("progressive_edge_stretch") ||
        normalized == QStringLiteral("progressive_stretch") ||
        normalized == QStringLiteral("edge_stretch_progressive")) {
        return BackgroundFillEffect::EdgeStretch;
    }
    if (normalized == QStringLiteral("mirror") ||
        normalized == QStringLiteral("mirror_cover") ||
        normalized == QStringLiteral("mirrored_cover")) {
        return BackgroundFillEffect::Mirror;
    }
    return BackgroundFillEffect::EdgeStretch;
}
