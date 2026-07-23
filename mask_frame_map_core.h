#pragma once

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace jcut::masks {

struct MaskFrameMapCore {
    bool ordinalSidecar = false;
    bool metadataVerified = false;
    bool renderReady = false;
    std::int64_t mappedFrameCount = 0;
    std::int64_t firstSourceFrame = -1;
    std::int64_t lastSourceFrame = -1;
    std::int64_t lastMaskFrame = -1;
    std::vector<std::pair<std::int64_t, std::int64_t>> sorted;
    std::string error;
};

struct MaskFrameMapCoreCacheStats {
    std::uint64_t hitCount = 0;
    std::uint64_t missCount = 0;
    std::uint64_t validationCount = 0;
    std::uint64_t entryCount = 0;
};

// Loads and strictly validates JCut's source-frame to generated-mask-ordinal
// sidecar. Generated ordinal sidecars fail closed unless their map, metadata,
// source identity, completion manifest, and exact frame coverage agree. The
// validated result is cached by filesystem version tokens; callers receive a
// value snapshot while frame lookups reuse the cached map without copying it.
[[nodiscard]] MaskFrameMapCore loadMaskFrameMapCore(
    const std::filesystem::path& directory,
    const std::filesystem::path& sourceMediaPath = {});

// Matches the legacy Qt lookup policy: exact-only for generated ordinal maps;
// nearest mapped frame for non-ordinal maps; identity mapping for ordinary
// sidecars without a map.
[[nodiscard]] std::optional<std::int64_t> mappedMaskFrameForSourceFrameCore(
    const std::filesystem::path& directory,
    const std::filesystem::path& sourceMediaPath,
    std::int64_t sourceFrame);

[[nodiscard]] std::optional<std::filesystem::path> maskFramePathForSourceFrameCore(
    const std::filesystem::path& directory,
    const std::filesystem::path& sourceMediaPath,
    std::int64_t sourceFrame);

// Diagnostics and deterministic regression-test support for the process-wide
// validated sidecar cache.
[[nodiscard]] MaskFrameMapCoreCacheStats maskFrameMapCoreCacheStats();
void clearMaskFrameMapCoreCache();

} // namespace jcut::masks
