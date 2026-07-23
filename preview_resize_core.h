#pragma once

#include <algorithm>
#include <cmath>

namespace jcut::preview {

struct PointD {
    double x = 0.0;
    double y = 0.0;
};

struct RectD {
    double left = 0.0;
    double top = 0.0;
    double width = 0.0;
    double height = 0.0;
};

enum class ResizeAnchor {
    Center,
    Left,
    Top,
    TopLeft,
};

inline PointD screenShiftForAnchoredResize(
    const RectD& originBounds,
    ResizeAnchor anchor,
    double scaleXFactor,
    double scaleYFactor)
{
    if (originBounds.width <= 1.0 || originBounds.height <= 1.0) {
        return {};
    }
    const double centerX = originBounds.left + originBounds.width * 0.5;
    const double centerY = originBounds.top + originBounds.height * 0.5;
    double anchorX = centerX;
    double anchorY = centerY;
    double resizedAnchorX = centerX;
    double resizedAnchorY = centerY;
    if (anchor == ResizeAnchor::Left || anchor == ResizeAnchor::TopLeft) {
        anchorX = originBounds.left;
        resizedAnchorX = centerX - originBounds.width * scaleXFactor * 0.5;
    }
    if (anchor == ResizeAnchor::Top || anchor == ResizeAnchor::TopLeft) {
        anchorY = originBounds.top;
        resizedAnchorY = centerY - originBounds.height * scaleYFactor * 0.5;
    }
    return {anchorX - resizedAnchorX, anchorY - resizedAnchorY};
}

inline PointD translationForAnchoredResize(
    const PointD& originTranslation,
    const PointD& originScale,
    const PointD& nextScale,
    const RectD& originBounds,
    ResizeAnchor anchor,
    const PointD& previewScale)
{
    const auto safeSignedScale = [](double value) {
        if (std::abs(value) < 0.0001) {
            return value < 0.0 ? -0.0001 : 0.0001;
        }
        return value;
    };
    const PointD shift = screenShiftForAnchoredResize(
        originBounds,
        anchor,
        nextScale.x / safeSignedScale(originScale.x),
        nextScale.y / safeSignedScale(originScale.y));
    return {
        originTranslation.x + shift.x / std::max(0.0001, previewScale.x),
        originTranslation.y + shift.y / std::max(0.0001, previewScale.y)};
}

} // namespace jcut::preview
