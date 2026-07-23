#pragma once

#include "core/image_buffer.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace jcut {

struct FaceAvatarCropRectCore {
    int left = 0;
    int top = 0;
    int width = 0;
    int height = 0;

    bool valid() const noexcept
    {
        return width > 0 && height > 0;
    }
};

struct FaceAvatarStripUvCore {
    double left = 0.0;
    double right = 0.0;

    bool valid() const noexcept
    {
        return left >= 0.0 && right > left && right <= 1.0;
    }
};

inline FaceAvatarStripUvCore faceAvatarStripUvCore(
    std::size_t avatarIndex,
    int textureWidth,
    int tileWidth,
    int gapPixels)
{
    if (textureWidth <= 0 || tileWidth <= 0) return {};
    const int gap = std::max(0, gapPixels);
    const std::uint64_t leftPixels =
        static_cast<std::uint64_t>(avatarIndex) *
        static_cast<std::uint64_t>(tileWidth + gap);
    const std::uint64_t rightPixels =
        leftPixels + static_cast<std::uint64_t>(tileWidth);
    if (rightPixels >
        static_cast<std::uint64_t>(textureWidth)) {
        return {};
    }
    return {
        static_cast<double>(leftPixels) /
            static_cast<double>(textureWidth),
        static_cast<double>(rightPixels) /
            static_cast<double>(textureWidth)};
}

inline FaceAvatarCropRectCore faceAvatarCropRectCore(
    int imageWidth,
    int imageHeight,
    double centerX,
    double centerY,
    double boxSize,
    double boxLeft = -1.0,
    double boxTop = -1.0,
    double boxRight = -1.0,
    double boxBottom = -1.0)
{
    const int width = std::max(1, imageWidth);
    const int height = std::max(1, imageHeight);
    if (boxLeft >= 0.0 && boxTop >= 0.0 &&
        boxRight > boxLeft && boxBottom > boxTop &&
        boxRight <= 1.0 && boxBottom <= 1.0) {
        const int left = std::clamp(
            static_cast<int>(std::floor(boxLeft * width)),
            0,
            std::max(0, width - 1));
        const int top = std::clamp(
            static_cast<int>(std::floor(boxTop * height)),
            0,
            std::max(0, height - 1));
        const int right = std::clamp(
            static_cast<int>(std::ceil(boxRight * width)),
            left + 1,
            width);
        const int bottom = std::clamp(
            static_cast<int>(std::ceil(boxBottom * height)),
            top + 1,
            height);
        return {left, top, right - left, bottom - top};
    }

    const int minimumSide = std::max(1, std::min(width, height));
    int side = std::max(40, minimumSide / 3);
    if (boxSize > 0.0) {
        side = std::clamp(
            static_cast<int>(std::round(
                std::clamp(boxSize, 0.0, 1.0) * minimumSide)),
            std::min(40, minimumSide),
            minimumSide);
    } else {
        side = std::min(side, minimumSide);
    }
    const int centerPixelX = static_cast<int>(std::round(
        std::clamp(centerX, 0.0, 1.0) * width));
    const int centerPixelY = static_cast<int>(std::round(
        std::clamp(centerY, 0.0, 1.0) * height));
    const int left = std::clamp(
        centerPixelX - side / 2, 0, std::max(0, width - side));
    const int top = std::clamp(
        centerPixelY - side / 2, 0, std::max(0, height - side));
    return {
        left,
        top,
        std::min(side, width - left),
        std::min(side, height - top)};
}

inline core::ImageBuffer cropFaceAvatarImageCore(
    const core::ImageBuffer& source,
    double centerX,
    double centerY,
    double boxSize,
    int outputSize)
{
    if (source.empty() || outputSize <= 0) return {};
    const FaceAvatarCropRectCore crop = faceAvatarCropRectCore(
        source.size.width,
        source.size.height,
        centerX,
        centerY,
        boxSize);
    if (!crop.valid()) return {};
    core::ImageBuffer output;
    output.format = core::PixelFormat::Rgba8;
    output.size = {outputSize, outputSize};
    output.strideBytes = outputSize * 4;
    output.bytes.resize(
        static_cast<std::size_t>(output.strideBytes * outputSize));
    for (int y = 0; y < outputSize; ++y) {
        const int sourceY = std::clamp(
            crop.top +
                static_cast<int>(
                    (static_cast<std::int64_t>(y) * crop.height) /
                    outputSize),
            crop.top,
            crop.top + crop.height - 1);
        for (int x = 0; x < outputSize; ++x) {
            const int sourceX = std::clamp(
                crop.left +
                    static_cast<int>(
                        (static_cast<std::int64_t>(x) * crop.width) /
                        outputSize),
                crop.left,
                crop.left + crop.width - 1);
            const std::size_t sourceOffset =
                static_cast<std::size_t>(
                    sourceY * source.strideBytes + sourceX * 4);
            const std::size_t outputOffset =
                static_cast<std::size_t>(
                    y * output.strideBytes + x * 4);
            std::copy_n(
                source.bytes.begin() +
                    static_cast<std::ptrdiff_t>(sourceOffset),
                4,
                output.bytes.begin() +
                    static_cast<std::ptrdiff_t>(outputOffset));
        }
    }
    return output;
}

inline core::ImageBuffer faceAvatarStripImageCore(
    const std::vector<core::ImageBuffer>& avatars,
    int gapPixels = 4)
{
    if (avatars.empty()) return {};
    const int tileWidth = avatars.front().size.width;
    const int tileHeight = avatars.front().size.height;
    if (tileWidth <= 0 || tileHeight <= 0) return {};
    for (const core::ImageBuffer& avatar : avatars) {
        if (avatar.empty() ||
            avatar.format != core::PixelFormat::Rgba8 ||
            avatar.size.width != tileWidth ||
            avatar.size.height != tileHeight ||
            avatar.strideBytes < tileWidth * 4) {
            return {};
        }
    }
    const int gap = std::max(0, gapPixels);
    core::ImageBuffer strip;
    strip.format = core::PixelFormat::Rgba8;
    strip.size = {
        static_cast<int>(avatars.size()) * tileWidth +
            (static_cast<int>(avatars.size()) - 1) * gap,
        tileHeight};
    strip.strideBytes = strip.size.width * 4;
    strip.bytes.assign(
        static_cast<std::size_t>(
            strip.strideBytes * strip.size.height),
        std::uint8_t{0});
    for (std::size_t index = 0; index < avatars.size(); ++index) {
        const core::ImageBuffer& avatar = avatars[index];
        const int destinationX =
            static_cast<int>(index) * (tileWidth + gap);
        for (int y = 0; y < tileHeight; ++y) {
            const std::size_t sourceOffset =
                static_cast<std::size_t>(y * avatar.strideBytes);
            const std::size_t destinationOffset =
                static_cast<std::size_t>(
                    y * strip.strideBytes + destinationX * 4);
            std::copy_n(
                avatar.bytes.begin() +
                    static_cast<std::ptrdiff_t>(sourceOffset),
                tileWidth * 4,
                strip.bytes.begin() +
                    static_cast<std::ptrdiff_t>(destinationOffset));
        }
    }
    return strip;
}

} // namespace jcut
