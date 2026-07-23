#pragma once

#include "core/geometry.h"

#include <algorithm>
#include <cmath>
#include <optional>

namespace jcut {

// Clip transforms are applied after the source has been aspect-fitted into the
// output canvas. This returns the uniform multiplier that turns that fitted
// rectangle into an aspect-fill rectangle.
inline std::optional<double> scaleToFillFactor(core::SizeI sourceSize,
                                               core::SizeI outputSize)
{
    if (!sourceSize.valid() || !outputSize.valid()) {
        return std::nullopt;
    }

    const double fitScaleX = static_cast<double>(outputSize.width) /
        static_cast<double>(sourceSize.width);
    const double fitScaleY = static_cast<double>(outputSize.height) /
        static_cast<double>(sourceSize.height);
    const double minimumFit = std::min(fitScaleX, fitScaleY);
    const double maximumFit = std::max(fitScaleX, fitScaleY);
    if (!std::isfinite(minimumFit) || !std::isfinite(maximumFit) ||
        minimumFit <= 0.0) {
        return std::nullopt;
    }

    const double factor = maximumFit / minimumFit;
    return std::isfinite(factor) && factor > 0.0
        ? std::optional<double>(factor)
        : std::nullopt;
}

} // namespace jcut
