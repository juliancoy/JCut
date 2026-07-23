#pragma once

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace jcut::masks {

struct MaskSidecarCore {
    std::string id;
    std::string displayName;
    std::filesystem::path directory;
    std::int64_t frameCount = 0;
    std::int64_t firstFrame = -1;
    std::int64_t lastFrame = -1;
    bool decodeOrdinalFrames = false;
    bool ready = false;
    std::string readinessIssue;

    [[nodiscard]] bool valid() const
    {
        return !id.empty() && !directory.empty() && frameCount > 0;
    }
};

[[nodiscard]] std::string stableMaskSidecarIdCore(
    const std::filesystem::path& directory);
[[nodiscard]] MaskSidecarCore inspectMaskSidecarCore(
    const std::filesystem::path& directory,
    const std::filesystem::path& sourceMediaPath = {});
[[nodiscard]] std::vector<MaskSidecarCore> discoverMaskSidecarsCore(
    const std::filesystem::path& sourceMediaPath,
    const std::filesystem::path& preferredDirectory = {});

} // namespace jcut::masks
