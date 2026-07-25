#pragma once

#include "frame_payload_core.h"

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace jcut::masks {

struct MaskFrameMapEntryCore {
    std::int64_t sourceFrame = -1;
    std::int64_t sourcePresentationTimestamp =
        jcut::core::kUnknownSourcePresentationTimestamp;
    std::int64_t maskFrame = -1;
};

struct MaskFrameMapCore {
    bool ordinalSidecar = false;
    bool metadataVerified = false;
    bool authenticatedPartialRun = false;
    bool renderReady = false;
    std::int64_t mappedFrameCount = 0;
    std::int64_t firstSourceFrame = -1;
    std::int64_t lastSourceFrame = -1;
    std::int64_t firstSourcePresentationTimestamp =
        jcut::core::kUnknownSourcePresentationTimestamp;
    std::int64_t lastSourcePresentationTimestamp =
        jcut::core::kUnknownSourcePresentationTimestamp;
    std::int64_t lastMaskFrame = -1;
    std::vector<MaskFrameMapEntryCore> entries;
    std::string error;
};

struct MaskFrameMapCoreCacheStats {
    std::uint64_t hitCount = 0;
    std::uint64_t missCount = 0;
    std::uint64_t validationCount = 0;
    std::uint64_t entryCount = 0;
};

// Loads and strictly validates JCut's source-frame to generated-mask-ordinal
// sidecar. Completed ordinal sidecars require authenticated metadata,
// completion, and exact coverage. A suspended BiRefNet run may expose only its
// authenticated contiguous prefix; absent/future samples still fail closed.
// The validated result is cached by filesystem version tokens with bounded
// metadata revalidation; frame lookups stay memory-only between validations
// while newly published sidecars become visible within one second.
[[nodiscard]] MaskFrameMapCore loadMaskFrameMapCore(
    const std::filesystem::path& directory,
    const std::filesystem::path& sourceMediaPath = {});

// Source-frame-only lookup is valid only for ordinary, non-ordinal sidecars.
// Decode-ordinal sidecars require the exact presented sample identity below.
[[nodiscard]] std::optional<std::int64_t> mappedMaskFrameForSourceFrameCore(
    const std::filesystem::path& directory,
    const std::filesystem::path& sourceMediaPath,
    std::int64_t sourceFrame);

// Resolves the mask sample for one actual decoded/presented source sample.
// sourcePresentationTimestamp is raw AVFrame::best_effort_timestamp in the
// source video stream's time_base. Ordinal sidecars fail closed when it is
// unavailable or does not exactly match the authenticated map.
[[nodiscard]] std::optional<std::int64_t> mappedMaskFrameForDecodedSampleCore(
    const std::filesystem::path& directory,
    const std::filesystem::path& sourceMediaPath,
    std::int64_t sourceFrame,
    std::int64_t sourcePresentationTimestamp);

[[nodiscard]] std::optional<std::filesystem::path> maskFramePathForSourceFrameCore(
    const std::filesystem::path& directory,
    const std::filesystem::path& sourceMediaPath,
    std::int64_t sourceFrame);

[[nodiscard]] std::optional<std::filesystem::path> maskFramePathForDecodedSampleCore(
    const std::filesystem::path& directory,
    const std::filesystem::path& sourceMediaPath,
    std::int64_t sourceFrame,
    std::int64_t sourcePresentationTimestamp);

// Prefetch is allowed to load every sample sharing a rounded source key. It
// does not select one for rendering, so duplicate VFR keys remain unambiguous.
[[nodiscard]] std::vector<std::filesystem::path>
maskFramePathsForSourceFramePrefetchCore(
    const std::filesystem::path& directory,
    const std::filesystem::path& sourceMediaPath,
    std::int64_t sourceFrame);

// Diagnostics and deterministic regression-test support for the process-wide
// validated sidecar cache.
[[nodiscard]] MaskFrameMapCoreCacheStats maskFrameMapCoreCacheStats();
void clearMaskFrameMapCoreCache();

} // namespace jcut::masks
