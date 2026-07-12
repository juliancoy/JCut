#include "render_runtime.h"

#include "background_fill_effect.h"
#include "decoder_context.h"
#include "render.h"
#include "render_backend.h"
#include "render_internal.h"
#include "render_qt_compat.h"

#include <QHash>
#include <QImage>
#include <QSize>
#include <QVector>

#include <memory>

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

struct ThreadLocalPreviewRenderer {
    ~ThreadLocalPreviewRenderer()
    {
        for (editor::DecoderContext* decoder : decoders) {
            delete decoder;
        }
        asyncDecoder.shutdown();
    }

    bool ensureInitialized(RenderBackend requestedBackend,
                           const QSize& outputSize,
                           QString* errorOut)
    {
        if (renderer &&
            backend == requestedBackend &&
            initializedSize == outputSize) {
            return true;
        }

        renderer.reset();
        asyncFrameCache.clear();
        for (editor::DecoderContext* decoder : decoders) {
            delete decoder;
        }
        decoders.clear();
        initializedSize = QSize();
        backend = requestedBackend;

        if (requestedBackend != RenderBackend::Vulkan) {
            if (errorOut) {
                *errorOut = QStringLiteral("unsupported preview backend");
            }
            return false;
        }

        if (!asyncDecoder.initialize()) {
            if (errorOut) {
                *errorOut = QStringLiteral("preview decoder initialization failed");
            }
            return false;
        }

        renderer = std::make_unique<render_detail::OffscreenVulkanRenderer>();
        if (!renderer->initialize(outputSize, errorOut)) {
            renderer.reset();
            return false;
        }

        initializedSize = outputSize;
        return true;
    }

    RenderBackend backend = RenderBackend::Vulkan;
    QSize initializedSize;
    std::unique_ptr<render_detail::OffscreenRenderer> renderer;
    QHash<QString, editor::DecoderContext*> decoders;
    editor::AsyncDecoder asyncDecoder;
    QHash<render_detail::RenderAsyncFrameKey, editor::FrameHandle> asyncFrameCache;
};

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
    qtRequest.backgroundFillOpacity = qBound(0.0, request.backgroundFillOpacity, 1.0);
    qtRequest.backgroundFillBrightness = qBound(-1.0, request.backgroundFillBrightness, 1.0);
    qtRequest.backgroundFillSaturation = qBound(0.0, request.backgroundFillSaturation, 3.0);
    qtRequest.backgroundFillEdgePixels = qBound(1, request.backgroundFillEdgePixels, 512);
    qtRequest.backgroundFillEdgeProgressive = request.backgroundFillEdgeProgressive;
    qtRequest.backgroundFillEdgePower = qBound(0.25, request.backgroundFillEdgePower, 8.0);
    qtRequest.backgroundFillStretchSourceClipId =
        QString::fromStdString(request.backgroundFillStretchSourceClipId).trimmed();
    qtRequest.transcriptPrependMs = request.transcriptPrependMs;
    qtRequest.transcriptPostpendMs = request.transcriptPostpendMs;
    qtRequest.transcriptOffsetMs = request.transcriptOffsetMs;
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
                                              std::int64_t timelineFrame,
                                              bool forceSoftwareDecode,
                                              bool readbackToCpuImage)
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
    QString gpuInitializationError;
    thread_local ThreadLocalPreviewRenderer previewRenderer;
    if (!previewRenderer.ensureInitialized(requestedBackend,
                                           qtRequest.outputSize,
                                           &gpuInitializationError)) {
        result.message = gpuInitializationError.isEmpty()
            ? "vulkan preview renderer initialization failed"
            : gpuInitializationError.toStdString();
        return result;
    }
    result.effectiveRenderBackend = previewRenderer.renderer->backendId().toStdString();

    render_detail::OffscreenRenderFrame previewFrame;
    qint64 decodeMs = 0;
    qint64 textureMs = 0;
    qint64 compositeMs = 0;
    qint64 readbackMs = 0;
    qint64* readbackMsPtr = readbackToCpuImage ? &readbackMs : nullptr;
    const bool renderedOk =
        previewRenderer.renderer->renderFrameToOutput(qtRequest,
                                                      static_cast<qreal>(timelineFrame),
                                                      previewRenderer.decoders,
                                                      &previewRenderer.asyncDecoder,
                                                      &previewRenderer.asyncFrameCache,
                                                      toQVector(timelineData.clips),
                                                      &previewFrame,
                                                      readbackToCpuImage,
                                                      nullptr,
                                                      &decodeMs,
                                                      &textureMs,
                                                      &compositeMs,
                                                      readbackMsPtr,
                                                      nullptr,
                                                      nullptr,
                                                      nullptr,
                                                      -1.0,
                                                      forceSoftwareDecode,
                                                      !readbackToCpuImage);
    const bool hasRequestedOutput = readbackToCpuImage
        ? !previewFrame.cpuImage.isNull()
        : previewFrame.vulkanFrame.valid;
    if (!renderedOk || !hasRequestedOutput) {
        result.message = "failed to render preview frame with Vulkan renderer";
        return result;
    }

    result.success = true;
    result.usedGpu = true;
    result.image = toImageBuffer(previewFrame.cpuImage);
    result.vulkanFrame = previewFrame.vulkanFrame;
    return result;
}

} // namespace jcut::render
