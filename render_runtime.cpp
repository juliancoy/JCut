#include "render_runtime.h"

#include "render.h"
#include "render_backend.h"
#include "render_internal.h"
#include "render_qt_compat.h"

#include <QHash>
#include <QImage>
#include <QSize>
#include <QVector>

namespace {

template <typename T>
QVector<T> toQVector(const std::vector<T>& items)
{
    QVector<T> result;
    result.reserve(static_cast<qsizetype>(items.size()));
    for (const T& item : items) {
        result.push_back(item);
    }
    return result;
}

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
    buffer.bytes.resize(static_cast<std::size_t>(byteCount));
    std::memcpy(buffer.bytes.data(), rgba.constBits(), static_cast<std::size_t>(byteCount));
    return buffer;
}

} // namespace

namespace jcut::render {

RenderRequest toQtRenderRequest(const RenderRequestCore& request,
                                const TimelineRenderData& timelineData)
{
    RenderRequest qtRequest;
    qtRequest.outputPath = QString::fromStdString(request.outputPath);
    qtRequest.outputFormat = QString::fromStdString(request.outputFormat);
    qtRequest.imageSequenceFormat = QString::fromStdString(request.imageSequenceFormat);
    qtRequest.outputSize = QSize(request.outputSize.width, request.outputSize.height);
    qtRequest.outputFps = request.outputFps;
    qtRequest.playbackSpeed = request.playbackSpeed;
    qtRequest.useProxyMedia = request.useProxyMedia;
    qtRequest.bypassGrading = request.bypassGrading;
    qtRequest.correctionsEnabled = request.correctionsEnabled;
    qtRequest.createVideoFromImageSequence = request.createVideoFromImageSequence;
    qtRequest.disableParallelImageWrite = request.disableParallelImageWrite;
    qtRequest.backgroundFillEffect =
        backgroundFillEffectFromString(QString::fromStdString(request.backgroundFillEffect));
    qtRequest.transcriptPrependMs = request.transcriptPrependMs;
    qtRequest.transcriptPostpendMs = request.transcriptPostpendMs;
    qtRequest.exportStartFrame = request.exportStartFrame;
    qtRequest.exportEndFrame = request.exportEndFrame;
    qtRequest.clips = toQVector(timelineData.clips);
    qtRequest.tracks = toQVector(timelineData.tracks);
    qtRequest.renderSyncMarkers = toQVector(timelineData.renderSyncMarkers);
    qtRequest.exportRanges = toQVector(timelineData.exportRanges);
    return qtRequest;
}

RenderResultCore renderTimelineToFileCore(const RenderRequestCore& request,
                                          const TimelineRenderData& timelineData,
                                          const RenderProgressCoreCallback& progressCallback)
{
    const RenderRequest qtRequest = toQtRenderRequest(request, timelineData);
    const RenderResult qtResult = renderTimelineToFile(
        qtRequest,
        progressCallback
            ? [&progressCallback](const RenderProgress& progress) {
                  return progressCallback(toCoreRenderProgress(progress));
              }
            : std::function<bool(const RenderProgress&)>{});
    return toCoreRenderResult(qtResult);
}

PreviewFrameResultCore renderPreviewFrameCore(const RenderRequestCore& request,
                                              const TimelineRenderData& timelineData,
                                              std::int64_t timelineFrame)
{
    PreviewFrameResultCore result;
    const RenderBackend requestedBackend = desiredPreviewBackendFromEnvironment();
    result.requestedRenderBackend = renderBackendName(requestedBackend).toStdString();
    result.effectiveRenderBackend = "none";

    if (!request.outputSize.valid()) {
        result.message = "invalid preview output size";
        return result;
    }
    if (timelineData.tracks.empty() || timelineData.clips.empty()) {
        result.message = "preview requires at least one track and clip";
        return result;
    }

    const RenderRequest qtRequest = toQtRenderRequest(request, timelineData);
    QHash<QString, editor::DecoderContext*> decoders;
    editor::AsyncDecoder asyncDecoder;
    QHash<render_detail::RenderAsyncFrameKey, editor::FrameHandle> asyncFrameCache;
    QString gpuInitializationError;
    std::unique_ptr<render_detail::OffscreenRenderer> activeRenderer;

    if (requestedBackend == RenderBackend::Vulkan) {
        activeRenderer = std::make_unique<render_detail::OffscreenVulkanRenderer>();
        result.effectiveRenderBackend = "vulkan";
    } else {
        result.message = "unsupported preview backend";
        return result;
    }

    const bool gpuInitialized = activeRenderer &&
        activeRenderer->initialize(qtRequest.outputSize, &gpuInitializationError);
    const bool useGpuRenderer = gpuInitialized;
    if (!useGpuRenderer) {
        result.message = gpuInitializationError.isEmpty()
            ? "vulkan preview renderer initialization failed"
            : gpuInitializationError.toStdString();
        return result;
    }
    result.effectiveRenderBackend = activeRenderer->backendId().toStdString();

    render_detail::OffscreenRenderFrame previewFrame;
    const bool renderedOk =
        activeRenderer->renderFrameToOutput(qtRequest,
                                            static_cast<qreal>(timelineFrame),
                                            decoders,
                                            &asyncDecoder,
                                            &asyncFrameCache,
                                            toQVector(timelineData.clips),
                                            &previewFrame,
                                            true);
    if (!renderedOk || previewFrame.cpuImage.isNull()) {
        result.message = "failed to render preview frame with Vulkan renderer";
        return result;
    }

    result.success = true;
    result.usedGpu = true;
    result.image = toImageBuffer(previewFrame.cpuImage);
    return result;
}

} // namespace jcut::render
