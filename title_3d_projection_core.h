#pragma once

#include <algorithm>
#include <cmath>
#include <numbers>

namespace jcut {

struct Title3DProjectionCore {
    bool enabled = false;
    double yawDegrees = 0.0;
    double pitchDegrees = 0.0;
    double rollDegrees = 0.0;
    double depth = 0.0;
    double scale = 1.0;
};

struct TitleProjectedPointCore {
    double x = 0.0;
    double y = 0.0;
    bool valid = false;
};

// Qt-free point form of VulkanTextRenderer::mvpForTitle3DRect. The element
// center stays in the title layout while scale applies to the element quad,
// matching Qt's per-background/per-glyph MVP rather than scaling the whole
// layout around the title center.
inline TitleProjectedPointCore projectTitle3DPointCore(
    double sourceX,
    double sourceY,
    double elementCenterX,
    double elementCenterY,
    double titleCenterX,
    double titleCenterY,
    int outputWidth,
    int outputHeight,
    const Title3DProjectionCore& projection)
{
    if (outputWidth <= 0 || outputHeight <= 0 ||
        !std::isfinite(sourceX) || !std::isfinite(sourceY) ||
        !std::isfinite(elementCenterX) ||
        !std::isfinite(elementCenterY) ||
        !std::isfinite(titleCenterX) ||
        !std::isfinite(titleCenterY)) {
        return {};
    }
    if (!projection.enabled) {
        return {sourceX, sourceY, true};
    }

    constexpr double cameraDistance = 5.2;
    constexpr double fovDegrees = 43.0;
    const double width = static_cast<double>(outputWidth);
    const double height = static_cast<double>(outputHeight);
    const double aspect = width / height;
    const double halfViewHeight =
        std::tan(fovDegrees * std::numbers::pi / 360.0) *
        cameraDistance;
    const double halfViewWidth = halfViewHeight * aspect;
    const double titleWorldX =
        ((2.0 * titleCenterX / width) - 1.0) *
        halfViewWidth;
    const double titleWorldY =
        ((2.0 * titleCenterY / height) - 1.0) *
        halfViewHeight;
    const double elementOffsetX =
        ((elementCenterX - titleCenterX) / width) *
        2.0 * halfViewWidth;
    const double elementOffsetY =
        ((elementCenterY - titleCenterY) / height) *
        2.0 * halfViewHeight;
    const double scale = std::clamp(
        std::isfinite(projection.scale) ? projection.scale : 1.0,
        0.05,
        4.0);
    double x =
        elementOffsetX +
        ((sourceX - elementCenterX) / width) *
            2.0 * halfViewWidth * scale;
    double y =
        elementOffsetY +
        ((sourceY - elementCenterY) / height) *
            2.0 * halfViewHeight * scale;
    double z = std::clamp(
        std::isfinite(projection.depth) ? projection.depth : 0.0,
        -3.0,
        3.0);

    const double radians = std::numbers::pi / 180.0;
    const double roll =
        (std::isfinite(projection.rollDegrees)
             ? projection.rollDegrees
             : 0.0) *
        radians;
    const double rollCos = std::cos(roll);
    const double rollSin = std::sin(roll);
    const double rolledX = x * rollCos - y * rollSin;
    const double rolledY = x * rollSin + y * rollCos;

    const double pitch =
        (std::isfinite(projection.pitchDegrees)
             ? projection.pitchDegrees
             : 0.0) *
        radians;
    const double pitchCos = std::cos(pitch);
    const double pitchSin = std::sin(pitch);
    const double pitchedY = rolledY * pitchCos - z * pitchSin;
    const double pitchedZ = rolledY * pitchSin + z * pitchCos;

    const double yaw =
        (std::isfinite(projection.yawDegrees)
             ? projection.yawDegrees
             : 0.0) *
        radians;
    const double yawCos = std::cos(yaw);
    const double yawSin = std::sin(yaw);
    const double yawedX = rolledX * yawCos + pitchedZ * yawSin;
    const double yawedZ = -rolledX * yawSin + pitchedZ * yawCos;

    const double worldX = titleWorldX + yawedX;
    const double worldY = titleWorldY + pitchedY;
    const double cameraDepth = cameraDistance - yawedZ;
    if (!std::isfinite(cameraDepth) || cameraDepth <= 0.001) {
        return {};
    }
    const double tangent =
        std::tan(fovDegrees * std::numbers::pi / 360.0);
    const double ndcX =
        worldX / (cameraDepth * tangent * aspect);
    const double ndcY =
        worldY / (cameraDepth * tangent);
    const double projectedX = (ndcX + 1.0) * width * 0.5;
    const double projectedY = (ndcY + 1.0) * height * 0.5;
    if (!std::isfinite(projectedX) || !std::isfinite(projectedY)) {
        return {};
    }
    return {projectedX, projectedY, true};
}

} // namespace jcut
