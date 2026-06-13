#pragma once

#include "core/geometry.h"

#include <cstdint>
#include <vector>

namespace jcut::core {

enum class PixelFormat {
    Rgba8
};

struct ImageBuffer {
    PixelFormat format = PixelFormat::Rgba8;
    SizeI size;
    int strideBytes = 0;
    std::vector<std::uint8_t> bytes;

    [[nodiscard]] bool empty() const
    {
        return bytes.empty() || !size.valid() || strideBytes <= 0;
    }
};

} // namespace jcut::core
