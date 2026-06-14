#pragma once

#include <QString>

enum class BackgroundFillEffect {
    EdgeStretch,
    BlurCover,
};

inline constexpr BackgroundFillEffect kDefaultBackgroundFillEffect = BackgroundFillEffect::EdgeStretch;

inline QString backgroundFillEffectToString(BackgroundFillEffect effect)
{
    switch (effect) {
    case BackgroundFillEffect::EdgeStretch:
        return QStringLiteral("edge_stretch");
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
    return BackgroundFillEffect::EdgeStretch;
}
