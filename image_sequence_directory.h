#pragma once

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace jcut {

inline constexpr double kImageSequenceFramesPerSecond = 30.0;

struct ImageSequenceDirectoryInfo {
    std::filesystem::path directory;
    std::vector<std::filesystem::path> framePaths;
    std::string extension;

    bool detected() const noexcept { return framePaths.size() >= 2; }
    std::int64_t frameCount() const noexcept
    {
        return static_cast<std::int64_t>(framePaths.size());
    }
};

// Detects the same numbered-image-directory convention used by the Qt media
// browser and returns every frame of the dominant extension in natural order.
// Results are cached by absolute directory path and directory modification time.
ImageSequenceDirectoryInfo probeImageSequenceDirectory(
    const std::filesystem::path& path);

bool isImageSequenceDirectory(const std::filesystem::path& path);

} // namespace jcut
