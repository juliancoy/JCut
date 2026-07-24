#pragma once

#include <QString>

enum class BackgroundFillEffect {
    None,
    EdgeStretch,
    ProgressiveEdgeStretch,
    ProgressiveBidirectionalEdgeStretch,
    Tile,
    Mirror,
    BlurCover,
};

inline constexpr BackgroundFillEffect kDefaultBackgroundFillEffect = BackgroundFillEffect::None;

inline QString backgroundFillEffectToString(BackgroundFillEffect effect)
{
    switch (effect) {
    case BackgroundFillEffect::None:
        return QStringLiteral("none");
    case BackgroundFillEffect::EdgeStretch:
        return QStringLiteral("edge_stretch");
    case BackgroundFillEffect::ProgressiveEdgeStretch:
        return QStringLiteral("progressive_edge_stretch");
    case BackgroundFillEffect::ProgressiveBidirectionalEdgeStretch:
        return QStringLiteral("progressive_bidirectional_edge_stretch");
    case BackgroundFillEffect::Tile:
        return QStringLiteral("tile");
    case BackgroundFillEffect::Mirror:
        return QStringLiteral("mirror");
    case BackgroundFillEffect::BlurCover:
        return QStringLiteral("blur_cover");
    }
    return QStringLiteral("none");
}

inline BackgroundFillEffect backgroundFillEffectFromString(const QString& value)
{
    const QString normalized = value.trimmed().toLower().replace(QLatin1Char('-'), QLatin1Char('_'));
    if (normalized == QStringLiteral("none") ||
        normalized == QStringLiteral("off") ||
        normalized == QStringLiteral("disabled")) {
        return BackgroundFillEffect::None;
    }
    if (normalized == QStringLiteral("blur") ||
        normalized == QStringLiteral("blur_cover") ||
        normalized == QStringLiteral("blurred_cover")) {
        return BackgroundFillEffect::BlurCover;
    }
    if (normalized == QStringLiteral("edge_stretch") ||
        normalized == QStringLiteral("stretch")) {
        return BackgroundFillEffect::EdgeStretch;
    }
    if (normalized == QStringLiteral("progressive_edge_stretch") ||
        normalized == QStringLiteral("progressive_stretch") ||
        normalized == QStringLiteral("edge_stretch_progressive")) {
        return BackgroundFillEffect::ProgressiveEdgeStretch;
    }
    if (normalized == QStringLiteral("progressive_bidirectional_edge_stretch") ||
        normalized == QStringLiteral("progressive_bidirectional_stretch") ||
        normalized == QStringLiteral("bidirectional_edge_stretch")) {
        return BackgroundFillEffect::ProgressiveBidirectionalEdgeStretch;
    }
    if (normalized == QStringLiteral("tile") ||
        normalized == QStringLiteral("tiled") ||
        normalized == QStringLiteral("repeat")) {
        return BackgroundFillEffect::Tile;
    }
    if (normalized == QStringLiteral("mirror") ||
        normalized == QStringLiteral("mirror_cover") ||
        normalized == QStringLiteral("mirrored_cover")) {
        return BackgroundFillEffect::Mirror;
    }
    return BackgroundFillEffect::None;
}
