#pragma once

#include "background_fill_effect.h"

#include <QSize>

#include <cstdint>
#include <limits>
#include <cstddef>

namespace render_detail {

inline constexpr uint32_t kProgressiveCompositeNoEdge =
    std::numeric_limits<uint32_t>::max();
inline constexpr uint32_t kProgressiveCompositeTileSize = 16;

inline constexpr uint32_t progressiveCompositeTileCount(uint32_t dimension)
{
    return (dimension + kProgressiveCompositeTileSize - 1u) /
        kProgressiveCompositeTileSize;
}

struct ProgressiveCompositeBufferSizes {
    size_t rowTileBytes = 0;
    size_t columnTileBytes = 0;
    size_t rowEdgeBytes = 0;
    size_t columnEdgeBytes = 0;
};

struct ProgressiveCompositeBoundaryTilePush {
    uint32_t outputWidth = 0;
    uint32_t outputHeight = 0;
    uint32_t rowTileCount = 0;
    uint32_t columnTileCount = 0;
    float alphaThreshold = 1.0f / 255.0f;
};

struct ProgressiveCompositeBoundaryMergePush {
    uint32_t outputWidth = 0;
    uint32_t outputHeight = 0;
    uint32_t rowTileCount = 0;
    uint32_t columnTileCount = 0;
};

static_assert(sizeof(ProgressiveCompositeBoundaryTilePush) == 20);
static_assert(sizeof(ProgressiveCompositeBoundaryMergePush) == 16);

struct alignas(8) ProgressiveCompositeRowEdges {
    uint32_t left = kProgressiveCompositeNoEdge;
    uint32_t right = kProgressiveCompositeNoEdge;
};

struct alignas(8) ProgressiveCompositeColumnEdges {
    uint32_t top = kProgressiveCompositeNoEdge;
    uint32_t bottom = kProgressiveCompositeNoEdge;
};

inline ProgressiveCompositeBufferSizes progressiveCompositeBufferSizes(
    uint32_t width,
    uint32_t height)
{
    const size_t rowTiles = progressiveCompositeTileCount(width);
    const size_t columnTiles = progressiveCompositeTileCount(height);
    return {
        static_cast<size_t>(height) * rowTiles * sizeof(ProgressiveCompositeRowEdges),
        static_cast<size_t>(width) * columnTiles * sizeof(ProgressiveCompositeColumnEdges),
        static_cast<size_t>(height) * sizeof(ProgressiveCompositeRowEdges),
        static_cast<size_t>(width) * sizeof(ProgressiveCompositeColumnEdges),
    };
}

struct ProgressiveCompositeStretchSettings {
    BackgroundFillEffect effect = BackgroundFillEffect::BlurCover;
    QSize outputSize;
    float alphaThreshold = 1.0f / 255.0f;
    float opacity = 1.0f;
    float brightness = 0.0f;
    float saturation = 1.0f;
    float power = 2.0f;
    uint32_t edgePixels = 1;

    bool enabled() const
    {
        return effect == BackgroundFillEffect::ProgressiveEdgeStretch &&
            outputSize.isValid() && opacity > 0.0f;
    }
};

struct alignas(8) ProgressiveCompositeStretchPush {
    uint32_t outputWidth = 0;
    uint32_t outputHeight = 0;
    uint32_t edgePixels = 1;
    float alphaThreshold = 1.0f / 255.0f;
    float opacity = 1.0f;
    float brightness = 0.0f;
    float saturation = 1.0f;
    float power = 2.0f;
};

static_assert(sizeof(ProgressiveCompositeStretchPush) == 32);

enum class ProgressiveCompositeDirection : uint32_t {
    None = 0,
    Left,
    Right,
    Top,
    Bottom,
};

inline ProgressiveCompositeDirection progressiveCompositeDirection(
    uint32_t x,
    uint32_t y,
    uint32_t width,
    uint32_t height,
    const ProgressiveCompositeRowEdges& row,
    const ProgressiveCompositeColumnEdges& column)
{
    float best = std::numeric_limits<float>::infinity();
    ProgressiveCompositeDirection direction = ProgressiveCompositeDirection::None;
    const auto consider = [&](bool valid,
                              float normalizedDistance,
                              ProgressiveCompositeDirection candidate) {
        if (valid && normalizedDistance < best) {
            best = normalizedDistance;
            direction = candidate;
        }
    };
    consider(row.left != kProgressiveCompositeNoEdge && x < row.left,
             static_cast<float>(row.left - x) / qMax(1u, row.left),
             ProgressiveCompositeDirection::Left);
    consider(row.right != kProgressiveCompositeNoEdge && x > row.right,
             static_cast<float>(x - row.right) /
                 qMax(1u, (width - 1u) - row.right),
             ProgressiveCompositeDirection::Right);
    consider(column.top != kProgressiveCompositeNoEdge && y < column.top,
             static_cast<float>(column.top - y) / qMax(1u, column.top),
             ProgressiveCompositeDirection::Top);
    consider(column.bottom != kProgressiveCompositeNoEdge && y > column.bottom,
             static_cast<float>(y - column.bottom) /
                 qMax(1u, (height - 1u) - column.bottom),
             ProgressiveCompositeDirection::Bottom);
    return direction;
}

} // namespace render_detail
