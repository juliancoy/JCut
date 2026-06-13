#pragma once

namespace jcut::core {

struct SizeI {
    int width = 0;
    int height = 0;

    [[nodiscard]] constexpr bool operator==(const SizeI& other) const
    {
        return width == other.width && height == other.height;
    }

    [[nodiscard]] constexpr bool valid() const
    {
        return width > 0 && height > 0;
    }
};

struct RectF {
    double x = 0.0;
    double y = 0.0;
    double width = 0.0;
    double height = 0.0;

    [[nodiscard]] constexpr bool valid() const
    {
        return width >= 0.0 && height >= 0.0;
    }

    [[nodiscard]] constexpr bool operator==(const RectF& other) const
    {
        return x == other.x && y == other.y &&
               width == other.width && height == other.height;
    }
};

} // namespace jcut::core
