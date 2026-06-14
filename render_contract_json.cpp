#include "render_contract_json.h"

namespace jcut::render {
namespace {

nlohmann::json sizeJson(const core::SizeI& size)
{
    return nlohmann::json{{"width", size.width}, {"height", size.height}};
}

nlohmann::json imageJson(const core::ImageBuffer& image)
{
    return nlohmann::json{
        {"valid", !image.empty()},
        {"width", image.size.width},
        {"height", image.size.height},
        {"strideBytes", image.strideBytes},
        {"byteCount", image.bytes.size()}
    };
}

} // namespace

nlohmann::json toJson(RenderOutputMode mode)
{
    switch (mode) {
    case RenderOutputMode::EncodedFile:
        return "encoded_file";
    case RenderOutputMode::ImageSequence:
        return "image_sequence";
    case RenderOutputMode::EncodedFileAndImageSequence:
        return "encoded_file_and_image_sequence";
    }
    return "encoded_file";
}

nlohmann::json toJson(const RenderRequestCore& request)
{
    return nlohmann::json{
        {"outputPath", request.outputPath},
        {"outputFormat", request.outputFormat},
        {"imageSequenceFormat", request.imageSequenceFormat},
        {"outputSize", sizeJson(request.outputSize)},
        {"outputFps", request.outputFps},
        {"playbackSpeed", request.playbackSpeed},
        {"useProxyMedia", request.useProxyMedia},
        {"bypassGrading", request.bypassGrading},
        {"correctionsEnabled", request.correctionsEnabled},
        {"createVideoFromImageSequence", request.createVideoFromImageSequence},
        {"disableParallelImageWrite", request.disableParallelImageWrite},
        {"transcriptPrependMs", request.transcriptPrependMs},
        {"transcriptPostpendMs", request.transcriptPostpendMs},
        {"exportStartFrame", request.exportStartFrame},
        {"exportEndFrame", request.exportEndFrame},
        {"clipCount", request.clipCount},
        {"trackCount", request.trackCount},
        {"renderSyncMarkerCount", request.renderSyncMarkerCount},
        {"exportRangeCount", request.exportRangeCount},
        {"outputMode", toJson(request.outputMode)}
    };
}

nlohmann::json toJson(const RenderProgressCore& progress)
{
    return nlohmann::json{
        {"framesCompleted", progress.framesCompleted},
        {"totalFrames", progress.totalFrames},
        {"segmentIndex", progress.segmentIndex},
        {"segmentCount", progress.segmentCount},
        {"timelineFrame", progress.timelineFrame},
        {"segmentStartFrame", progress.segmentStartFrame},
        {"segmentEndFrame", progress.segmentEndFrame},
        {"usingGpu", progress.usingGpu},
        {"usingHardwareEncode", progress.usingHardwareEncode},
        {"encoderLabel", progress.encoderLabel},
        {"exportPipeline", progress.exportPipeline},
        {"gpuTransferLabel", progress.gpuTransferLabel},
        {"encoderPixelFormat", progress.encoderPixelFormat},
        {"encoderSoftwarePixelFormat", progress.encoderSoftwarePixelFormat},
        {"cudaExternalTransfer", progress.cudaExternalTransfer},
        {"encoderHardwareFrames", progress.encoderHardwareFrames},
        {"elapsedMs", progress.elapsedMs},
        {"estimatedRemainingMs", progress.estimatedRemainingMs},
        {"renderStageMs", progress.renderStageMs},
        {"renderDecodeStageMs", progress.renderDecodeStageMs},
        {"renderTextureStageMs", progress.renderTextureStageMs},
        {"renderCompositeStageMs", progress.renderCompositeStageMs},
        {"renderNv12StageMs", progress.renderNv12StageMs},
        {"gpuReadbackMs", progress.gpuReadbackMs},
        {"overlayStageMs", progress.overlayStageMs},
        {"convertStageMs", progress.convertStageMs},
        {"encodeStageMs", progress.encodeStageMs},
        {"audioStageMs", progress.audioStageMs},
        {"maxFrameRenderStageMs", progress.maxFrameRenderStageMs},
        {"maxFrameDecodeStageMs", progress.maxFrameDecodeStageMs},
        {"maxFrameTextureStageMs", progress.maxFrameTextureStageMs},
        {"maxFrameReadbackStageMs", progress.maxFrameReadbackStageMs},
        {"maxFrameConvertStageMs", progress.maxFrameConvertStageMs},
        {"previewFrame", imageJson(progress.previewFrame)},
        {"skippedClips", progress.skippedClips},
        {"skippedClipReasonCounts", progress.skippedClipReasonCounts},
        {"renderStageTable", progress.renderStageTable},
        {"worstFrameTable", progress.worstFrameTable}
    };
}

nlohmann::json toJson(const RenderResultCore& result)
{
    return nlohmann::json{
        {"success", result.success},
        {"cancelled", result.cancelled},
        {"usedGpu", result.usedGpu},
        {"usedHardwareEncode", result.usedHardwareEncode},
        {"encoderLabel", result.encoderLabel},
        {"exportPipeline", result.exportPipeline},
        {"gpuTransferLabel", result.gpuTransferLabel},
        {"encoderPixelFormat", result.encoderPixelFormat},
        {"encoderSoftwarePixelFormat", result.encoderSoftwarePixelFormat},
        {"cudaExternalTransfer", result.cudaExternalTransfer},
        {"encoderHardwareFrames", result.encoderHardwareFrames},
        {"namedOutputDir", result.namedOutputDir},
        {"framesRendered", result.framesRendered},
        {"elapsedMs", result.elapsedMs},
        {"renderStageMs", result.renderStageMs},
        {"renderDecodeStageMs", result.renderDecodeStageMs},
        {"renderTextureStageMs", result.renderTextureStageMs},
        {"renderCompositeStageMs", result.renderCompositeStageMs},
        {"renderNv12StageMs", result.renderNv12StageMs},
        {"gpuReadbackMs", result.gpuReadbackMs},
        {"overlayStageMs", result.overlayStageMs},
        {"convertStageMs", result.convertStageMs},
        {"encodeStageMs", result.encodeStageMs},
        {"audioStageMs", result.audioStageMs},
        {"maxFrameRenderStageMs", result.maxFrameRenderStageMs},
        {"maxFrameDecodeStageMs", result.maxFrameDecodeStageMs},
        {"maxFrameTextureStageMs", result.maxFrameTextureStageMs},
        {"maxFrameReadbackStageMs", result.maxFrameReadbackStageMs},
        {"maxFrameConvertStageMs", result.maxFrameConvertStageMs},
        {"requestedRenderBackend", result.requestedRenderBackend},
        {"effectiveRenderBackend", result.effectiveRenderBackend},
        {"message", result.message},
        {"skippedClips", result.skippedClips},
        {"skippedClipReasonCounts", result.skippedClipReasonCounts},
        {"renderStageTable", result.renderStageTable},
        {"worstFrameTable", result.worstFrameTable}
    };
}

} // namespace jcut::render
