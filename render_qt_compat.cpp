#include "render_qt_compat.h"

#include <QJsonArray>
#include <QJsonObject>
#include <QJsonValue>

#include <cstring>

namespace {

jcut::core::ImageBuffer toImageBuffer(const QImage& image)
{
    jcut::core::ImageBuffer buffer;
    if (image.isNull()) {
        return buffer;
    }

    const QImage rgba = image.convertToFormat(QImage::Format_RGBA8888);
    buffer.size = {rgba.width(), rgba.height()};
    buffer.strideBytes = rgba.bytesPerLine();
    const int byteCount = rgba.sizeInBytes();
    buffer.bytes.resize(byteCount);
    std::memcpy(buffer.bytes.data(), rgba.constBits(), static_cast<std::size_t>(byteCount));
    return buffer;
}

nlohmann::json toJson(const QJsonValue& value)
{
    switch (value.type()) {
    case QJsonValue::Null:
    case QJsonValue::Undefined:
        return nullptr;
    case QJsonValue::Bool:
        return value.toBool();
    case QJsonValue::Double:
        return value.toDouble();
    case QJsonValue::String:
        return value.toString().toStdString();
    case QJsonValue::Array: {
        nlohmann::json result = nlohmann::json::array();
        const QJsonArray array = value.toArray();
        for (const QJsonValue& item : array) {
            result.push_back(toJson(item));
        }
        return result;
    }
    case QJsonValue::Object: {
        nlohmann::json result = nlohmann::json::object();
        const QJsonObject object = value.toObject();
        for (auto it = object.begin(); it != object.end(); ++it) {
            result[it.key().toStdString()] = toJson(it.value());
        }
        return result;
    }
    }

    return nullptr;
}

jcut::render::RenderOutputMode toOutputMode(const ::RenderRequest& request)
{
    if (!request.imageSequenceFormat.isEmpty() && request.createVideoFromImageSequence) {
        return jcut::render::RenderOutputMode::EncodedFileAndImageSequence;
    }
    if (!request.imageSequenceFormat.isEmpty()) {
        return jcut::render::RenderOutputMode::ImageSequence;
    }
    return jcut::render::RenderOutputMode::EncodedFile;
}

} // namespace

namespace jcut::render {

RenderRequestCore toCoreRenderRequest(const ::RenderRequest& request)
{
    RenderRequestCore core;
    core.outputPath = request.outputPath.toStdString();
    core.outputFormat = request.outputFormat.toStdString();
    core.imageSequenceFormat = request.imageSequenceFormat.toStdString();
    core.outputSize = {request.outputSize.width(), request.outputSize.height()};
    core.outputFps = request.outputFps;
    core.useProxyMedia = request.useProxyMedia;
    core.bypassGrading = request.bypassGrading;
    core.correctionsEnabled = request.correctionsEnabled;
    core.createVideoFromImageSequence = request.createVideoFromImageSequence;
    core.disableParallelImageWrite = request.disableParallelImageWrite;
    core.exportStartFrame = request.exportStartFrame;
    core.exportEndFrame = request.exportEndFrame;
    core.clipCount = static_cast<std::size_t>(request.clips.size());
    core.trackCount = static_cast<std::size_t>(request.tracks.size());
    core.renderSyncMarkerCount = static_cast<std::size_t>(request.renderSyncMarkers.size());
    core.exportRangeCount = static_cast<std::size_t>(request.exportRanges.size());
    core.outputMode = toOutputMode(request);
    return core;
}

RenderProgressCore toCoreRenderProgress(const ::RenderProgress& progress)
{
    RenderProgressCore core;
    core.framesCompleted = progress.framesCompleted;
    core.totalFrames = progress.totalFrames;
    core.segmentIndex = progress.segmentIndex;
    core.segmentCount = progress.segmentCount;
    core.timelineFrame = progress.timelineFrame;
    core.segmentStartFrame = progress.segmentStartFrame;
    core.segmentEndFrame = progress.segmentEndFrame;
    core.usingGpu = progress.usingGpu;
    core.usingHardwareEncode = progress.usingHardwareEncode;
    core.encoderLabel = progress.encoderLabel.toStdString();
    core.elapsedMs = progress.elapsedMs;
    core.estimatedRemainingMs = progress.estimatedRemainingMs;
    core.renderStageMs = progress.renderStageMs;
    core.renderDecodeStageMs = progress.renderDecodeStageMs;
    core.renderTextureStageMs = progress.renderTextureStageMs;
    core.renderCompositeStageMs = progress.renderCompositeStageMs;
    core.renderNv12StageMs = progress.renderNv12StageMs;
    core.gpuReadbackMs = progress.gpuReadbackMs;
    core.overlayStageMs = progress.overlayStageMs;
    core.convertStageMs = progress.convertStageMs;
    core.encodeStageMs = progress.encodeStageMs;
    core.audioStageMs = progress.audioStageMs;
    core.maxFrameRenderStageMs = progress.maxFrameRenderStageMs;
    core.maxFrameDecodeStageMs = progress.maxFrameDecodeStageMs;
    core.maxFrameTextureStageMs = progress.maxFrameTextureStageMs;
    core.maxFrameReadbackStageMs = progress.maxFrameReadbackStageMs;
    core.maxFrameConvertStageMs = progress.maxFrameConvertStageMs;
    core.previewFrame = toImageBuffer(progress.previewFrame);
    core.skippedClips = toJson(progress.skippedClips);
    core.skippedClipReasonCounts = toJson(progress.skippedClipReasonCounts);
    core.renderStageTable = toJson(progress.renderStageTable);
    core.worstFrameTable = toJson(progress.worstFrameTable);
    return core;
}

RenderResultCore toCoreRenderResult(const ::RenderResult& result)
{
    RenderResultCore core;
    core.success = result.success;
    core.cancelled = result.cancelled;
    core.usedGpu = result.usedGpu;
    core.usedHardwareEncode = result.usedHardwareEncode;
    core.encoderLabel = result.encoderLabel.toStdString();
    core.namedOutputDir = result.namedOutputDir.toStdString();
    core.framesRendered = result.framesRendered;
    core.elapsedMs = result.elapsedMs;
    core.renderStageMs = result.renderStageMs;
    core.renderDecodeStageMs = result.renderDecodeStageMs;
    core.renderTextureStageMs = result.renderTextureStageMs;
    core.renderCompositeStageMs = result.renderCompositeStageMs;
    core.renderNv12StageMs = result.renderNv12StageMs;
    core.gpuReadbackMs = result.gpuReadbackMs;
    core.overlayStageMs = result.overlayStageMs;
    core.convertStageMs = result.convertStageMs;
    core.encodeStageMs = result.encodeStageMs;
    core.audioStageMs = result.audioStageMs;
    core.maxFrameRenderStageMs = result.maxFrameRenderStageMs;
    core.maxFrameDecodeStageMs = result.maxFrameDecodeStageMs;
    core.maxFrameTextureStageMs = result.maxFrameTextureStageMs;
    core.maxFrameReadbackStageMs = result.maxFrameReadbackStageMs;
    core.maxFrameConvertStageMs = result.maxFrameConvertStageMs;
    core.requestedRenderBackend = result.requestedRenderBackend.toStdString();
    core.effectiveRenderBackend = result.effectiveRenderBackend.toStdString();
    core.message = result.message.toStdString();
    core.skippedClips = toJson(result.skippedClips);
    core.skippedClipReasonCounts = toJson(result.skippedClipReasonCounts);
    core.renderStageTable = toJson(result.renderStageTable);
    core.worstFrameTable = toJson(result.worstFrameTable);
    return core;
}

} // namespace jcut::render
