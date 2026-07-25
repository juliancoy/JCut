#pragma once

#include "core/image_buffer.h"

#include <string>

namespace jcut::core {

// Framework-neutral still-image decode used by render and preview workers.
// The returned buffer is always tightly described RGBA8 storage.
ImageBuffer decodeImageFileRgba(const std::string& path,
                                std::string* errorOut = nullptr);

// Masks are single-channel data. Keeping them Gray8 avoids expanding every
// full-resolution mask to four channels before its Vulkan upload.
ImageBuffer decodeImageFileGray(const std::string& path,
                                std::string* errorOut = nullptr);

} // namespace jcut::core
