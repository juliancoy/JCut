#pragma once

#include "core/geometry.h"
#include "core/image_buffer.h"

#include <nlohmann/json.hpp>

#include <cstdint>
#include <string>

namespace jcut::render {

enum class RenderOutputMode {
    EncodedFile,
    ImageSequence,
    EncodedFileAndImageSequence
};

struct RenderRequestCore {
    std::string outputPath;
    std::string outputFormat;
    std::string imageSequenceFormat;
    core::SizeI outputSize;
    double outputFps = 0.0;
    double playbackSpeed = 1.0;
    bool useProxyMedia = false;
    bool bypassGrading = false;
    bool correctionsEnabled = true;
    bool createVideoFromImageSequence = false;
    bool disableParallelImageWrite = false;
    std::int64_t exportStartFrame = 0;
    std::int64_t exportEndFrame = 0;
    std::size_t clipCount = 0;
    std::size_t trackCount = 0;
    std::size_t renderSyncMarkerCount = 0;
    std::size_t exportRangeCount = 0;
    RenderOutputMode outputMode = RenderOutputMode::EncodedFile;
};

struct RenderProgressCore {
    std::int64_t framesCompleted = 0;
    std::int64_t totalFrames = 0;
    int segmentIndex = 0;
    int segmentCount = 0;
    std::int64_t timelineFrame = 0;
    std::int64_t segmentStartFrame = 0;
    std::int64_t segmentEndFrame = 0;
    bool usingGpu = false;
    bool usingHardwareEncode = false;
    std::string encoderLabel;
    std::int64_t elapsedMs = 0;
    std::int64_t estimatedRemainingMs = -1;
    std::int64_t renderStageMs = 0;
    std::int64_t renderDecodeStageMs = 0;
    std::int64_t renderTextureStageMs = 0;
    std::int64_t renderCompositeStageMs = 0;
    std::int64_t renderNv12StageMs = 0;
    std::int64_t gpuReadbackMs = 0;
    std::int64_t overlayStageMs = 0;
    std::int64_t convertStageMs = 0;
    std::int64_t encodeStageMs = 0;
    std::int64_t audioStageMs = 0;
    std::int64_t maxFrameRenderStageMs = 0;
    std::int64_t maxFrameDecodeStageMs = 0;
    std::int64_t maxFrameTextureStageMs = 0;
    std::int64_t maxFrameReadbackStageMs = 0;
    std::int64_t maxFrameConvertStageMs = 0;
    core::ImageBuffer previewFrame;
    nlohmann::json skippedClips;
    nlohmann::json skippedClipReasonCounts;
    nlohmann::json renderStageTable;
    nlohmann::json worstFrameTable;
};

struct RenderResultCore {
    bool success = false;
    bool cancelled = false;
    bool usedGpu = false;
    bool usedHardwareEncode = false;
    std::string encoderLabel;
    std::string namedOutputDir;
    std::int64_t framesRendered = 0;
    std::int64_t elapsedMs = 0;
    std::int64_t renderStageMs = 0;
    std::int64_t renderDecodeStageMs = 0;
    std::int64_t renderTextureStageMs = 0;
    std::int64_t renderCompositeStageMs = 0;
    std::int64_t renderNv12StageMs = 0;
    std::int64_t gpuReadbackMs = 0;
    std::int64_t overlayStageMs = 0;
    std::int64_t convertStageMs = 0;
    std::int64_t encodeStageMs = 0;
    std::int64_t audioStageMs = 0;
    std::int64_t maxFrameRenderStageMs = 0;
    std::int64_t maxFrameDecodeStageMs = 0;
    std::int64_t maxFrameTextureStageMs = 0;
    std::int64_t maxFrameReadbackStageMs = 0;
    std::int64_t maxFrameConvertStageMs = 0;
    std::string requestedRenderBackend;
    std::string effectiveRenderBackend;
    std::string message;
    nlohmann::json skippedClips;
    nlohmann::json skippedClipReasonCounts;
    nlohmann::json renderStageTable;
    nlohmann::json worstFrameTable;
};

} // namespace jcut::render
