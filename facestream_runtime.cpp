#include "facestream_runtime.h"

#include "decoder_context.h"
#include "frame_handle.h"
#include "json_io_utils.h"
#include "render_internal.h"
#include "speakers_tab_internal.h"
#include "transcript_engine.h"

#include <QApplication>
#include <QDateTime>
#include <QFile>
#include <QPainter>
#include <QPixmap>

namespace jcut::facestream {

VulkanFrameProvider::~VulkanFrameProvider()
{
    qDeleteAll(decoders);
    decoders.clear();
}

bool VulkanFrameProvider::ensureInitialized(const QSize& size)
{
    const QSize normalized(qMax(16, size.width()), qMax(16, size.height()));
    if (initialized && outputSize == normalized) {
        return true;
    }
    renderer = std::make_unique<render_detail::OffscreenVulkanRenderer>();
    qDeleteAll(decoders);
    decoders.clear();
    asyncFrameCache.clear();
    outputSize = normalized;
    QString error;
    if (!renderer->initialize(outputSize, &error)) {
        renderer.reset();
        initialized = false;
        failed = true;
        failureReason = error.isEmpty()
            ? QStringLiteral("Vulkan FaceStream renderer initialization failed.")
            : error;
        return false;
    }
    initialized = true;
    failed = false;
    failureReason.clear();
    return true;
}

TimelineClip buildFacestreamRenderClip(const TimelineClip& sourceClip,
                                      const QString& mediaPath,
                                      int64_t timelineFrame,
                                      int64_t sourceFrame)
{
    TimelineClip clip = sourceClip;
    clip.id = sourceClip.id.trimmed().isEmpty()
        ? QStringLiteral("facestream-vulkan-source")
        : sourceClip.id;
    clip.filePath = mediaPath;
    clip.proxyPath.clear();
    clip.useProxy = false;
    clip.mediaType = ClipMediaType::Video;
    clip.videoEnabled = true;
    clip.audioEnabled = false;
    clip.hasAudio = false;
    clip.startFrame = timelineFrame;
    clip.startSubframeSamples = 0;
    clip.sourceInFrame = qMax<int64_t>(0, sourceFrame);
    clip.sourceInSubframeSamples = 0;
    clip.durationFrames = 1;
    clip.sourceDurationFrames = qMax<int64_t>(clip.sourceInFrame + 1, sourceClip.sourceDurationFrames);
    clip.playbackRate = 1.0;
    clip.trackIndex = 0;
    clip.baseTranslationX = 0.0;
    clip.baseTranslationY = 0.0;
    clip.baseRotation = 0.0;
    clip.baseScaleX = 1.0;
    clip.baseScaleY = 1.0;
    clip.speakerFramingEnabled = false;
    clip.transformKeyframes.clear();
    clip.speakerFramingKeyframes.clear();
    clip.titleKeyframes.clear();
    clip.transcriptOverlay.enabled = false;
    return clip;
}

RenderRequest buildFacestreamRenderRequest(const TimelineClip& clip,
                                          int64_t timelineFrame,
                                          const QSize& outputSize)
{
    RenderRequest request;
    request.outputPath = QStringLiteral("facestream://vulkan");
    request.outputFormat = QStringLiteral("facestream-preview");
    request.outputSize = outputSize;
    request.bypassGrading = false;
    request.correctionsEnabled = false;
    request.clips = QVector<TimelineClip>{clip};
    request.tracks = QVector<TimelineTrack>{TimelineTrack{}};
    request.exportStartFrame = timelineFrame;
    request.exportEndFrame = timelineFrame;
    return request;
}

QImage renderFrameWithVulkan(VulkanFrameProvider* provider,
                             const TimelineClip& sourceClip,
                             const QString& mediaPath,
                             int64_t timelineFrame,
                             int64_t sourceFrame,
                             const QSize& outputSize,
                             VulkanFrameStats* stats)
{
    VulkanRenderResult result;
    if (!renderFrameWithVulkanResult(provider,
                                     sourceClip,
                                     mediaPath,
                                     timelineFrame,
                                     sourceFrame,
                                     outputSize,
                                     &result,
                                     true,
                                     stats,
                                     nullptr)) {
        return {};
    }
    return result.frame.cpuImage;
}

bool renderFrameWithVulkanResult(VulkanFrameProvider* provider,
                                 const TimelineClip& sourceClip,
                                 const QString& mediaPath,
                                 int64_t timelineFrame,
                                 int64_t sourceFrame,
                                 const QSize& outputSize,
                                 VulkanRenderResult* result,
                                 bool readbackToCpuImage,
                                 VulkanFrameStats* stats,
                                 QString* errorMessage)
{
    if (!provider || !provider->ensureInitialized(outputSize)) {
        if (errorMessage) {
            *errorMessage = provider ? provider->failureReason
                                     : QStringLiteral("Missing Vulkan frame provider.");
        }
        return false;
    }
    if (!result) {
        if (errorMessage) {
            *errorMessage = QStringLiteral("Missing Vulkan render result output.");
        }
        return false;
    }
    *result = VulkanRenderResult{};

    const TimelineClip clip = buildFacestreamRenderClip(sourceClip, mediaPath, timelineFrame, sourceFrame);
    const RenderRequest request = buildFacestreamRenderRequest(clip, timelineFrame, outputSize);

    qint64 decodeMs = 0;
    qint64 textureMs = 0;
    qint64 compositeMs = 0;
    qint64 readbackMs = 0;
    if (!provider->renderer->renderFrameToOutput(request,
                                                 timelineFrame,
                                                 provider->decoders,
                                                 nullptr,
                                                 &provider->asyncFrameCache,
                                                 QVector<TimelineClip>{clip},
                                                 &result->frame,
                                                 readbackToCpuImage,
                                                 nullptr,
                                                 &decodeMs,
                                                 &textureMs,
                                                 &compositeMs,
                                                 &readbackMs,
                                                 nullptr,
                                                 nullptr)) {
        provider->failed = true;
        provider->failureReason = errorMessage && !errorMessage->isEmpty()
            ? *errorMessage
            : QStringLiteral("Vulkan FaceStream render output request failed.");
        if (errorMessage && errorMessage->isEmpty()) {
            *errorMessage = provider->failureReason;
        }
        return false;
    }
    if (stats) {
        stats->decodeMs = decodeMs;
        stats->textureMs = textureMs;
        stats->compositeMs = compositeMs;
        stats->readbackMs = readbackMs;
    }
    if (readbackToCpuImage && result->frame.cpuImage.isNull()) {
        provider->failed = true;
        provider->failureReason = QStringLiteral("Vulkan FaceStream frame render returned null.");
        if (errorMessage) {
            *errorMessage = provider->failureReason;
        }
        return false;
    }
    if (!readbackToCpuImage && !result->frame.vulkanFrame.valid) {
        provider->failed = true;
        provider->failureReason = QStringLiteral("Vulkan FaceStream frame render returned no GPU image.");
        if (errorMessage) {
            *errorMessage = provider->failureReason;
        }
        return false;
    }
    provider->failed = false;
    provider->failureReason.clear();
    return true;
}

bool renderFrameToVulkan(VulkanFrameProvider* provider,
                         const TimelineClip& sourceClip,
                         const QString& mediaPath,
                         int64_t timelineFrame,
                         int64_t sourceFrame,
                         const QSize& outputSize,
                         render_detail::OffscreenVulkanFrame* frame,
                         VulkanFrameStats* stats,
                         QString* errorMessage)
{
    if (!frame) {
        if (errorMessage) {
            *errorMessage = QStringLiteral("Missing Vulkan frame output.");
        }
        return false;
    }
    VulkanRenderResult result;
    if (!renderFrameWithVulkanResult(provider,
                                     sourceClip,
                                     mediaPath,
                                     timelineFrame,
                                     sourceFrame,
                                     outputSize,
                                     &result,
                                     false,
                                     stats,
                                     errorMessage)) {
        frame->valid = false;
        return false;
    }
    *frame = result.frame.vulkanFrame;
    return frame->valid;
}

bool renderFrameToVulkanWithPreviewImage(VulkanFrameProvider* provider,
                                         const TimelineClip& sourceClip,
                                         const QString& mediaPath,
                                         int64_t timelineFrame,
                                         int64_t sourceFrame,
                                         const QSize& outputSize,
                                         render_detail::OffscreenVulkanFrame* frame,
                                         QImage* previewImageOut,
                                         VulkanFrameStats* stats,
                                         QString* errorMessage)
{
    if (previewImageOut) {
        *previewImageOut = QImage();
    }
    if (!renderFrameToVulkan(provider,
                             sourceClip,
                             mediaPath,
                             timelineFrame,
                             sourceFrame,
                             outputSize,
                             frame,
                             stats,
                             errorMessage)) {
        return false;
    }

    QImage previewImage = readLastRenderedVulkanFrameImage(provider, stats, errorMessage);
    if (previewImage.isNull()) {
        provider->failed = true;
        if (provider->failureReason.isEmpty()) {
            provider->failureReason = errorMessage && !errorMessage->isEmpty()
                ? *errorMessage
                : QStringLiteral("Failed to read back Vulkan FaceStream preview frame.");
        }
        return false;
    }

    provider->failed = false;
    provider->failureReason.clear();
    if (previewImageOut) {
        *previewImageOut = previewImage;
    }
    return true;
}

QImage readLastRenderedVulkanFrameImage(VulkanFrameProvider* provider,
                                        VulkanFrameStats* stats,
                                        QString* errorMessage)
{
    if (!provider || !provider->renderer) {
        if (errorMessage) {
            *errorMessage = QStringLiteral("Missing Vulkan frame provider/renderer.");
        }
        return {};
    }

    AVFrame* bgra = av_frame_alloc();
    if (!bgra) {
        if (errorMessage) {
            *errorMessage = QStringLiteral("Failed to allocate BGRA preview frame.");
        }
        return {};
    }
    bgra->format = AV_PIX_FMT_BGRA;
    bgra->width = qMax(1, provider->outputSize.width());
    bgra->height = qMax(1, provider->outputSize.height());
    if (av_frame_get_buffer(bgra, 32) < 0) {
        av_frame_free(&bgra);
        if (errorMessage) {
            *errorMessage = QStringLiteral("Failed to allocate BGRA preview buffer.");
        }
        return {};
    }

    qint64 readbackMs = 0;
    if (!provider->renderer->copyLastFrameToBgra(bgra, &readbackMs)) {
        av_frame_free(&bgra);
        provider->failed = true;
        provider->failureReason = QStringLiteral("Failed to read back last rendered Vulkan frame.");
        if (errorMessage) {
            *errorMessage = provider->failureReason;
        }
        return {};
    }

    QImage wrapped(reinterpret_cast<const uchar*>(bgra->data[0]),
                   bgra->width,
                   bgra->height,
                   bgra->linesize[0],
                   QImage::Format_ARGB32);
    QImage out = wrapped.copy().mirrored().convertToFormat(QImage::Format_ARGB32_Premultiplied);
    av_frame_free(&bgra);

    if (stats) {
        stats->readbackMs += readbackMs;
    }
    provider->failed = false;
    provider->failureReason.clear();
    return out;
}

VulkanPreviewClipFrameStatus buildPreviewClipFrameStatus(const QString& clipId,
                                                         const editor::FrameHandle& frameHandle,
                                                         int64_t requestedFrame,
                                                         const QSize& fallbackFrameSize)
{
    VulkanPreviewClipFrameStatus status;
    status.clipId = clipId;
    status.label = clipId;
    status.active = true;
    status.requestedSourceFrame = requestedFrame;
    status.presentedSourceFrame = frameHandle.isNull() ? -1 : frameHandle.frameNumber();
    status.frameSize = frameHandle.size().isValid() ? frameHandle.size() : fallbackFrameSize;
    status.hasFrame = !frameHandle.isNull();
    status.exact = status.hasFrame && status.presentedSourceFrame == requestedFrame;
    status.exactFrameAvailable = status.exact;
    status.selectedFrameAvailable = status.hasFrame;
    status.hardwareFrame = frameHandle.hasHardwareFrame();
    status.gpuTexture = frameHandle.hasGpuTexture();
    status.cpuImage = frameHandle.hasCpuImage() && !status.hardwareFrame && !status.gpuTexture;
    status.frame = frameHandle;
    status.transform = TimelineClip::TransformKeyframe{};
    status.grading = TimelineClip::GradingKeyframe{};
    if (status.gpuTexture) {
        status.decodePath = QStringLiteral("gpu_texture");
    } else if (status.hardwareFrame) {
        status.decodePath = QStringLiteral("hardware_frame");
    } else if (status.cpuImage) {
        status.decodePath = QStringLiteral("cpu_image");
    } else {
        status.decodePath = QStringLiteral("missing");
        status.missingReason = QStringLiteral("No presentable decoded frame payload.");
    }
    return status;
}

VulkanPreviewClipFrameStatus buildPreviewClipFrameStatus(const QString& clipId,
                                                         const render_detail::OffscreenVulkanFrame& frame,
                                                         int64_t requestedFrame)
{
    VulkanPreviewClipFrameStatus status;
    status.clipId = clipId;
    status.label = clipId;
    status.active = true;
    status.requestedSourceFrame = requestedFrame;
    status.presentedSourceFrame = requestedFrame;
    status.frameSize = frame.size;
    status.hasFrame = frame.valid && frame.imageView != VK_NULL_HANDLE;
    status.exact = status.hasFrame;
    status.exactFrameAvailable = status.hasFrame;
    status.selectedFrameAvailable = status.hasFrame;
    status.decodePath = status.hasFrame ? QStringLiteral("offscreen_vulkan") : QStringLiteral("missing");
    status.missingReason = status.hasFrame ? QString() : QStringLiteral("No presentable offscreen Vulkan frame payload.");
    status.externalVulkanFrame = status.hasFrame;
    status.sampledFramePregraded = status.hasFrame;
    status.sampledFrameNeedsYFlip = status.hasFrame;
    status.externalPhysicalDevice = frame.physicalDevice;
    status.externalDevice = frame.device;
    status.externalQueue = frame.queue;
    status.externalQueueFamilyIndex = frame.queueFamilyIndex;
    status.externalImage = frame.image;
    status.externalImageView = frame.imageView;
    status.externalImageMemory = frame.imageMemory;
    status.externalImageLayout = frame.imageLayout;
    status.externalImageFormat = frame.imageFormat;
    status.externalReadySemaphoreFd = frame.readySemaphoreFd;
    return status;
}

QVector<VulkanPreviewFacestreamOverlay> buildDetectionPreviewOverlays(
    const QString& clipId,
    int64_t sourceFrame,
    const QSize& frameSize,
    const QVector<QRectF>& detectionBoxes,
    const QVector<float>& confidences,
    const QRectF& roiRect,
    const QString& source)
{
    QVector<VulkanPreviewFacestreamOverlay> overlays;
    overlays.reserve(detectionBoxes.size() + 1);
    const qreal denomW = qMax<qreal>(1.0, frameSize.width());
    const qreal denomH = qMax<qreal>(1.0, frameSize.height());
    if (!roiRect.isNull() && roiRect.isValid() && !roiRect.isEmpty()) {
        VulkanPreviewFacestreamOverlay roiOverlay;
        roiOverlay.clipId = clipId;
        roiOverlay.streamId = QStringLiteral("roi");
        roiOverlay.source = QStringLiteral("roi");
        roiOverlay.trackId = -1;
        roiOverlay.sourceFrame = sourceFrame;
        roiOverlay.confidence = 1.0;
        roiOverlay.boxNorm = QRectF(roiRect.x() / denomW,
                                    roiRect.y() / denomH,
                                    roiRect.width() / denomW,
                                    roiRect.height() / denomH);
        if (roiOverlay.boxNorm.isValid() && !roiOverlay.boxNorm.isEmpty()) {
            overlays.push_back(roiOverlay);
        }
    }
    for (int i = 0; i < detectionBoxes.size(); ++i) {
        const QRectF& box = detectionBoxes.at(i);
        VulkanPreviewFacestreamOverlay overlay;
        overlay.clipId = clipId;
        overlay.streamId = QStringLiteral("det-%1").arg(i);
        overlay.source = source;
        overlay.trackId = i;
        overlay.sourceFrame = sourceFrame;
        overlay.confidence = i < confidences.size() ? confidences.at(i) : 0.0f;
        overlay.boxNorm = QRectF(box.x() / denomW,
                                 box.y() / denomH,
                                 box.width() / denomW,
                                 box.height() / denomH);
        if (overlay.boxNorm.isValid() && !overlay.boxNorm.isEmpty()) {
            overlays.push_back(overlay);
        }
    }
    return overlays;
}

void updateSingleClipPreviewInteractionState(PreviewInteractionState* state,
                                             const TimelineClip& sourceClip,
                                             int64_t frameNumber,
                                             const VulkanPreviewClipFrameStatus& status,
                                             const QVector<VulkanPreviewFacestreamOverlay>& overlays)
{
    if (!state) {
        return;
    }
    state->currentFrame = frameNumber;
    state->currentSample = frameToSamples(frameNumber);
    state->currentFramePosition = static_cast<qreal>(frameNumber);
    state->playing = true;
    state->selectedClipId = sourceClip.id;
    state->clipCount = 1;
    state->clips = QVector<TimelineClip>{sourceClip};
    state->tracks = QVector<TimelineTrack>{TimelineTrack{}};
    state->vulkanFrameStatuses = QVector<VulkanPreviewClipFrameStatus>{status};
    state->facestreamOverlays = overlays;
}

QImage buildScanPreview(const QImage& source,
                        const QVector<QRect>& detections,
                        int detectionCount,
                        const QRectF& roiRect)
{
    if (source.isNull()) {
        return QImage();
    }
    QImage preview = source.convertToFormat(QImage::Format_ARGB32_Premultiplied);
    QPainter painter(&preview);
    painter.setRenderHint(QPainter::Antialiasing, true);
    if (!roiRect.isNull() && roiRect.width() > 0.0 && roiRect.height() > 0.0) {
        painter.setPen(QPen(QColor(QStringLiteral("#ffaa33")), 2.0, Qt::DashLine));
        painter.drawRect(roiRect);
    }
    painter.setPen(QPen(QColor(QStringLiteral("#66ff66")), 2.0));
    for (const QRect& det : detections) {
        painter.drawRect(det);
    }
    painter.setPen(Qt::NoPen);
    painter.setBrush(QColor(0, 0, 0, 160));
    const QRect panel(8, 8, 220, 34);
    painter.drawRoundedRect(panel, 6.0, 6.0);
    painter.setPen(Qt::white);
    painter.drawText(panel.adjusted(10, 0, -10, 0), Qt::AlignVCenter | Qt::AlignLeft,
                     QStringLiteral("Detections: %1").arg(detectionCount));
    return preview;
}

} // namespace jcut::facestream
