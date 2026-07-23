#pragma once

#include <algorithm>
#include <array>
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

inline double rotationDeltaDegrees(
    const PointD& center,
    const PointD& fromPointer,
    const PointD& toPointer)
{
    const double fromLengthSquared =
        std::pow(fromPointer.x - center.x, 2.0) +
        std::pow(fromPointer.y - center.y, 2.0);
    const double toLengthSquared =
        std::pow(toPointer.x - center.x, 2.0) +
        std::pow(toPointer.y - center.y, 2.0);
    if (fromLengthSquared < 0.0001 || toLengthSquared < 0.0001) {
        return 0.0;
    }
    constexpr double kRadiansToDegrees =
        180.0 / 3.14159265358979323846;
    double delta = (
        std::atan2(toPointer.y - center.y, toPointer.x - center.x) -
        std::atan2(
            fromPointer.y - center.y,
            fromPointer.x - center.x)) *
        kRadiansToDegrees;
    while (delta > 180.0) delta -= 360.0;
    while (delta < -180.0) delta += 360.0;
    return delta;
}

inline double rotationForPointerDrag(
    double originRotation,
    const PointD& center,
    const PointD& fromPointer,
    const PointD& toPointer,
    double snapDegrees = 0.0)
{
    double rotation = std::clamp(
        originRotation +
            rotationDeltaDegrees(center, fromPointer, toPointer),
        -360.0,
        360.0);
    if (std::isfinite(snapDegrees) && snapDegrees > 0.0) {
        rotation =
            std::round(rotation / snapDegrees) * snapDegrees;
    }
    return std::clamp(rotation, -360.0, 360.0);
}

inline std::array<PointD, 4> transformedPresentationQuad(
    const RectD& imageRect,
    const RectD& outputRect,
    const PointD& outputSize,
    const PointD& translation,
    const PointD& scale,
    double rotationDegrees)
{
    const double safeOutputWidth =
        std::max(1.0, outputSize.x);
    const double safeOutputHeight =
        std::max(1.0, outputSize.y);
    const PointD center{
        imageRect.left + imageRect.width * 0.5 +
            translation.x * outputRect.width /
                safeOutputWidth,
        imageRect.top + imageRect.height * 0.5 +
            translation.y * outputRect.height /
                safeOutputHeight};
    const double halfWidth =
        imageRect.width * 0.5 * scale.x;
    const double halfHeight =
        imageRect.height * 0.5 * scale.y;
    const double radians =
        rotationDegrees *
        3.14159265358979323846 / 180.0;
    const double cosine = std::cos(radians);
    const double sine = std::sin(radians);
    const auto rotate = [&](double x, double y) {
        return PointD{
            center.x + cosine * x - sine * y,
            center.y + sine * x + cosine * y};
    };
    return {
        rotate(-halfWidth, -halfHeight),
        rotate(halfWidth, -halfHeight),
        rotate(halfWidth, halfHeight),
        rotate(-halfWidth, halfHeight)};
}

inline std::array<PointD, 4> transformedPresentationQuad(
    const RectD& outputRect,
    const PointD& outputSize,
    const PointD& translation,
    const PointD& scale,
    double rotationDegrees)
{
    return transformedPresentationQuad(
        outputRect,
        outputRect,
        outputSize,
        translation,
        scale,
        rotationDegrees);
}

inline RectD fittedPresentationRect(
    const RectD& outputRect,
    const PointD& outputSize,
    const PointD& sourceSize)
{
    const double safeOutputWidth = std::max(1.0, outputSize.x);
    const double safeOutputHeight = std::max(1.0, outputSize.y);
    const double safeSourceWidth = std::max(1.0, sourceSize.x);
    const double safeSourceHeight = std::max(1.0, sourceSize.y);
    const double fitScale = std::min(
        safeOutputWidth / safeSourceWidth,
        safeOutputHeight / safeSourceHeight);
    const double fittedWidth = std::max(
        1.0, std::round(safeSourceWidth * fitScale));
    const double fittedHeight = std::max(
        1.0, std::round(safeSourceHeight * fitScale));
    const double screenWidth =
        outputRect.width * fittedWidth / safeOutputWidth;
    const double screenHeight =
        outputRect.height * fittedHeight / safeOutputHeight;
    return {
        outputRect.left + (outputRect.width - screenWidth) * 0.5,
        outputRect.top + (outputRect.height - screenHeight) * 0.5,
        screenWidth,
        screenHeight};
}

} // namespace jcut::preview
