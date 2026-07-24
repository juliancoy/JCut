#pragma once

#include "core/image_buffer.h"

#include <string>

namespace jcut::core {

// Framework-neutral still-image decode used by render and preview workers.
// The returned buffer is always tightly described RGBA8 storage.
ImageBuffer decodeImageFileRgba(const std::string& path,
                                std::string* errorOut = nullptr);

} // namespace jcut::core
