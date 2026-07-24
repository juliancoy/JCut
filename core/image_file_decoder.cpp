#include "core/image_file_decoder.h"

#include <cmath>
#include <cstddef>
#include <cstring>
#include <limits>

#define STB_IMAGE_IMPLEMENTATION
#define STBI_FAILURE_USERMSG
#include "external/ncnn/src/stb_image.h"

namespace jcut::core {

ImageBuffer decodeImageFileRgba(const std::string& path,
                                std::string* errorOut)
{
    ImageBuffer result;
    if (path.empty()) {
        if (errorOut) {
            *errorOut = "image path is empty";
        }
        return result;
    }

    int width = 0;
    int height = 0;
    int sourceChannels = 0;
    stbi_uc* pixels =
        stbi_load(path.c_str(), &width, &height, &sourceChannels, STBI_rgb_alpha);
    if (!pixels) {
        if (errorOut) {
            const char* reason = stbi_failure_reason();
            *errorOut = reason ? reason : "image decode failed";
        }
        return result;
    }

    constexpr int kMaximumDimension = 16384;
    const bool dimensionsValid =
        width > 0 && height > 0 &&
        width <= kMaximumDimension && height <= kMaximumDimension;
    const std::size_t stride = dimensionsValid
        ? static_cast<std::size_t>(width) * 4u
        : 0u;
    const bool sizeValid =
        dimensionsValid &&
        stride <= static_cast<std::size_t>(std::numeric_limits<int>::max()) &&
        static_cast<std::size_t>(height) <=
            std::numeric_limits<std::size_t>::max() / stride;
    if (!sizeValid) {
        stbi_image_free(pixels);
        if (errorOut) {
            *errorOut = "decoded image dimensions are invalid";
        }
        return result;
    }

    const std::size_t byteCount = stride * static_cast<std::size_t>(height);
    result.size = {width, height};
    result.strideBytes = static_cast<int>(stride);
    result.bytes.resize(byteCount);
    std::memcpy(result.bytes.data(), pixels, byteCount);
    stbi_image_free(pixels);
    return result;
}

} // namespace jcut::core
