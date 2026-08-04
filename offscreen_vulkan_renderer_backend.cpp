#include "offscreen_vulkan_renderer_backend.h"

#include "background_fill_effect.h"
#include "cpu_overlay_render_backend.h"
#include "editor_shared_effects.h"
#include "editor_shared_timing.h"
#include "offscreen_vulkan_renderer_helpers.h"
#include "preview_view_transform.h"
#include "render_internal.h"
#include "render_vulkan_shared.h"
#include "titles.h"
#include "vulkan_detector_frame_handoff.h"
#include "vulkan_mask_preprocessor.h"
#include "vulkan_shader_paths.h"
#include "vulkan_staging_flush_range.h"
#include "vulkan_text_renderer.h"

#include <QDateTime>
#include <QDebug>
#include <QDir>
#include <QFile>
#include <QHash>
#include <QImage>
#include <QScopeGuard>
#include <QSet>

#if JCUT_HAS_CUDA_DRIVER
extern "C" {
#include <libavutil/hwcontext.h>
#include <libavutil/hwcontext_cuda.h>
}
#include <cuda.h>
#endif
#include <cmath>
#include <cstring>
#include <limits>
#include <unistd.h>
#include <vulkan/vulkan.h>

namespace render_detail {
namespace {

constexpr std::uint64_t kExportGpuFenceTimeoutNs = 5'000'000'000ull;
std::atomic<std::uint64_t> g_nextOffscreenProducerSessionId{1};

QJsonObject rectDiagnosticObject(const QRectF &rect) {
  return QJsonObject{{QStringLiteral("x"), rect.x()},
                     {QStringLiteral("y"), rect.y()},
                     {QStringLiteral("width"), rect.width()},
                     {QStringLiteral("height"), rect.height()}};
}

QJsonArray rectDiagnosticArray(const QVector<QRectF> &rects) {
  QJsonArray array;
  for (const QRectF &rect : rects) {
    array.push_back(rectDiagnosticObject(rect));
  }
  return array;
}

QString renderTranscriptPath(const TimelineClip &clip) {
  return activeTranscriptPathForClip(clip);
}

VkRect2D scissorFromRect(const QRectF& rect, const QSize& outputSize) {
  const int outputWidth = qMax(1, outputSize.width());
  const int outputHeight = qMax(1, outputSize.height());
  const int left = qBound(0, static_cast<int>(std::floor(rect.left())), outputWidth);
  const int top = qBound(0, static_cast<int>(std::floor(rect.top())), outputHeight);
  const int right = qBound(left, static_cast<int>(std::ceil(rect.right())), outputWidth);
  const int bottom = qBound(top, static_cast<int>(std::ceil(rect.bottom())), outputHeight);
  VkRect2D scissor{};
  scissor.offset = {left, top};
  scissor.extent = {
      static_cast<uint32_t>(qMax(0, right - left)),
      static_cast<uint32_t>(qMax(0, bottom - top))};
  return scissor;
}

QRectF faceTargetRectFromTransformDiagnostics(const QJsonObject &diagnostics) {
  const QJsonObject target =
      diagnostics.value(QStringLiteral("target_box_norm")).toObject();
  const QJsonObject output =
      diagnostics.value(QStringLiteral("output_size")).toObject();
  const qreal outputWidth = qMax<qreal>(
      1.0, output.value(QStringLiteral("width")).toDouble(0.0));
  const qreal outputHeight = qMax<qreal>(
      1.0, output.value(QStringLiteral("height")).toDouble(0.0));
  const qreal outputMinSide = qMax<qreal>(1.0, qMin(outputWidth, outputHeight));
  const qreal targetSide =
      qMax<qreal>(1.0, target.value(QStringLiteral("box")).toDouble(0.0) *
                            outputMinSide);
  const QPointF center(qBound<qreal>(
                           0.0, target.value(QStringLiteral("x")).toDouble(0.0),
                           1.0) *
                           outputWidth,
                       qBound<qreal>(
                           0.0, target.value(QStringLiteral("y")).toDouble(0.0),
                           1.0) *
                           outputHeight);
  return QRectF(center.x() - (targetSide * 0.5),
                center.y() - (targetSide * 0.5), targetSide, targetSide);
}

struct FrameUniformData {
  float outputSizeAndInverse[4] = {1.0f, 1.0f, 1.0f, 1.0f};
  float backgroundShadows[4] = {0.0f, 0.0f, 0.0f, 0.0f};
  float backgroundMidtones[4] = {0.0f, 0.0f, 0.0f, 0.0f};
  float backgroundHighlights[4] = {0.0f, 0.0f, 0.0f, 0.0f};
  float backgroundGrade[4] = {0.0f, 1.0f, 1.0f, 0.0f};
  float effectParams[4] = {0.0f, 0.0f, 0.0f, 0.0f};
  float effectDomain[4] = {0.0f, 0.0f, 1.0f, 1.0f};
  float effectMaskDomain[4] = {0.0f, 0.0f, 1.0f, 1.0f};
};

static_assert(sizeof(FrameUniformData) == sizeof(float) * 32);

constexpr int kFrameUniformRingCount = 4096;

VkDeviceSize frameUniformStrideForDevice(VkPhysicalDevice physicalDevice)
{
  VkPhysicalDeviceProperties properties{};
  vkGetPhysicalDeviceProperties(physicalDevice, &properties);
  const VkDeviceSize alignment = properties.limits.minUniformBufferOffsetAlignment;
  VkDeviceSize stride = sizeof(FrameUniformData);
  if (alignment > 0) {
    const VkDeviceSize remainder = stride % alignment;
    if (remainder != 0) {
      stride += alignment - remainder;
    }
  }
  return stride;
}

} // namespace

class OffscreenVulkanRendererPrivate {
public:
  static constexpr int kMaxLayerTextures = 12;
  static constexpr int kFrameSlots = 3;
  struct FrameSlot {
    VkCommandBuffer commandBuffer = VK_NULL_HANDLE;
    VkDescriptorSet nv12ComputeDescriptorSet = VK_NULL_HANDLE;
    VkBuffer stagingBuffer = VK_NULL_HANDLE;
    VkDeviceMemory stagingMemory = VK_NULL_HANDLE;
    VkDeviceSize stagingBufferSize = 0;
    VkDeviceSize stagingAllocationSize = 0;
    void *stagingMapped = nullptr;
    VkBuffer cudaExportBuffer = VK_NULL_HANDLE;
    VkDeviceMemory cudaExportMemory = VK_NULL_HANDLE;
    VkDeviceSize cudaExportAllocationSize = 0;
#if JCUT_HAS_CUDA_DRIVER
    CUexternalMemory cudaExternalMemory = nullptr;
    CUdeviceptr cudaExternalDevicePtr = 0;
    CUcontext cudaImportContext = nullptr;
    CUexternalSemaphore cudaConsumedExternalSemaphore = nullptr;
    CUcontext cudaConsumedContext = nullptr;
    AVBufferRef *cudaImportDeviceRef = nullptr;
#endif
    VkSemaphore cudaConsumedSemaphore = VK_NULL_HANDLE;
    VkFence fence = VK_NULL_HANDLE;
    bool stagingHostCoherent = false;
    bool inFlight = false;
    bool cudaCopyPending = false;
  };
  struct PreviewSlot {
    VkImage image = VK_NULL_HANDLE;
    VkDeviceMemory memory = VK_NULL_HANDLE;
    VkImageView view = VK_NULL_HANDLE;
    VkSemaphore readySemaphore = VK_NULL_HANDLE;
    VkSemaphore consumedSemaphore = VK_NULL_HANDLE;
    VkImageLayout layout = VK_IMAGE_LAYOUT_UNDEFINED;
    uint32_t memoryTypeIndex = UINT32_MAX;
    VkDeviceSize memoryAllocationSize = 0;
    std::uint64_t generation = 0;
    std::shared_ptr<OffscreenVulkanFrameConsumptionState> consumptionState =
        std::make_shared<OffscreenVulkanFrameConsumptionState>();
    bool published = false;
    bool handlesExported = false;
  };
  struct LayerTextureSlot {
    VkImage image = VK_NULL_HANDLE;
    VkDeviceMemory memory = VK_NULL_HANDLE;
    VkImageView view = VK_NULL_HANDLE;
    VkImage curveLutImage = VK_NULL_HANDLE;
    VkDeviceMemory curveLutMemory = VK_NULL_HANDLE;
    VkImageView curveLutView = VK_NULL_HANDLE;
    VkImage maskCurveLutImage = VK_NULL_HANDLE;
    VkDeviceMemory maskCurveLutMemory = VK_NULL_HANDLE;
    VkImageView maskCurveLutView = VK_NULL_HANDLE;
    VkImage maskImage = VK_NULL_HANDLE;
    VkDeviceMemory maskMemory = VK_NULL_HANDLE;
    VkImageView maskView = VK_NULL_HANDLE;
    VkImage maskRawImage = VK_NULL_HANDLE;
    VkDeviceMemory maskRawMemory = VK_NULL_HANDLE;
    VkImageView maskRawView = VK_NULL_HANDLE;
    QSize maskRawSize;
    VkFormat maskRawFormat = VK_FORMAT_UNDEFINED;
    VkImage maskWorkImage = VK_NULL_HANDLE;
    VkDeviceMemory maskWorkMemory = VK_NULL_HANDLE;
    VkImageView maskWorkView = VK_NULL_HANDLE;
    VkImageLayout maskLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    VkImageLayout maskRawLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    VkImageLayout maskWorkLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    VkDescriptorSet descriptorSet = VK_NULL_HANDLE;
    bool uploaded = false;
    bool curveUploaded = false;
    bool maskCurveUploaded = false;
    bool maskUploaded = false;
    std::shared_ptr<jcut::vulkan_detector::VulkanDetectorFrameHandoff>
        hardwareFrameHandoff;
    std::shared_ptr<jcut::vulkan_detector::VulkanDetectorFrameHandoff>
        referenceFrameHandoff;
  };
  ~OffscreenVulkanRendererPrivate() { release(); }

#include "offscreen_vulkan_renderer_initialization.h"
#include "offscreen_vulkan_renderer_staging_preview.h"
#include "offscreen_vulkan_renderer_composition.h"
#include "offscreen_vulkan_renderer_conversion.h"
#include "offscreen_vulkan_renderer_text_preparation.h"
private:
  QSize m_outputSize;
  bool m_initialized = false;

  VkInstance m_instance = VK_NULL_HANDLE;
  VkPhysicalDevice m_physicalDevice = VK_NULL_HANDLE;
  VkDeviceSize m_nonCoherentAtomSize = 1;
  VkDeviceSize m_storageBufferOffsetAlignment = 16;
  VkDevice m_device = VK_NULL_HANDLE;
  uint32_t m_graphicsQueueFamily = UINT32_MAX;
  VkQueue m_graphicsQueue = VK_NULL_HANDLE;
  bool m_externalMemoryFdSupported = false;
  bool m_externalSemaphoreFdSupported = false;
  bool m_graphicsQueueSupportsCompute = false;
  PFN_vkGetMemoryFdKHR m_vkGetMemoryFdKHR = nullptr;
  PFN_vkGetSemaphoreFdKHR m_vkGetSemaphoreFdKHR = nullptr;
  QString m_cudaExternalMemoryStatus;

  VkCommandPool m_commandPool = VK_NULL_HANDLE;
  VkCommandBuffer m_commandBuffer = VK_NULL_HANDLE;
  VkFence m_submitFence = VK_NULL_HANDLE;
  QVector<FrameSlot> m_frameSlots;
  int m_activeSlotIndex = -1;
  int m_lastHardwareSourceImportCount = 0;
  int m_lastHardwareSourceReuseCount = 0;
  QVector<int> m_pendingNv12SlotIndices;
  QVector<int> m_pendingNv12CudaSlotIndices;
  QVector<int> m_pendingYuvSlotIndices;
  bool m_cudaExportBuffersReady = false;
  QVector<PreviewSlot> m_previewSlots;
  int m_lastPreviewSlotIndex = -1;
  VkSemaphore m_pendingPreviewWait = VK_NULL_HANDLE;
  VkSemaphore m_pendingPreviewSignal = VK_NULL_HANDLE;

  VkImage m_colorImage = VK_NULL_HANDLE;
  VkDeviceMemory m_colorImageMemory = VK_NULL_HANDLE;
  VkImageView m_colorImageView = VK_NULL_HANDLE;
  VkSampler m_sampler = VK_NULL_HANDLE;

  VkDescriptorSetLayout m_descriptorSetLayout = VK_NULL_HANDLE;
  VkDescriptorPool m_descriptorPool = VK_NULL_HANDLE;
  VkDescriptorSet m_descriptorSet = VK_NULL_HANDLE;
  VkBuffer m_frameUniformBuffer = VK_NULL_HANDLE;
  VkDeviceMemory m_frameUniformMemory = VK_NULL_HANDLE;
  void *m_frameUniformMapped = nullptr;
  VkDeviceSize m_frameUniformStride = 0;
  int m_frameUniformRingIndex = 0;
  VkDescriptorSetLayout m_yuvComputeDescriptorSetLayout = VK_NULL_HANDLE;
  VkDescriptorPool m_yuvComputeDescriptorPool = VK_NULL_HANDLE;
  VkDescriptorSet m_yuvComputeDescriptorSet = VK_NULL_HANDLE;
  VkDescriptorSetLayout m_nv12ComputeDescriptorSetLayout = VK_NULL_HANDLE;
  VkDescriptorPool m_nv12ComputeDescriptorPool = VK_NULL_HANDLE;

  VkRenderPass m_renderPass = VK_NULL_HANDLE;

  VkFramebuffer m_framebuffer = VK_NULL_HANDLE;
  QVector<LayerTextureSlot> m_layerSlots;
  QHash<QString, QImage> m_preparedImageCache;
  OverlayImage m_cachedPlacementGuideOverlay;
  QSize m_cachedPlacementGuideOverlaySize;
  bool m_cachedPlacementGuideInstagramSafeArea = false;
  bool m_cachedPlacementGuideAlignmentGrid = false;
  QHash<QString, QVector<TranscriptSection>> m_transcriptCache;
  std::unique_ptr<VulkanTextRenderer> m_transcriptTextRenderer;
  std::unique_ptr<VulkanTextRenderer> m_speakerTextRenderer;
  VkBuffer m_stagingBuffer = VK_NULL_HANDLE;
  VkDeviceMemory m_stagingMemory = VK_NULL_HANDLE;
  void *m_stagingMapped = nullptr;
  VkPipelineLayout m_effectsPipelineLayout = VK_NULL_HANDLE;
  VkPipelineLayout m_maskPipelineLayout = VK_NULL_HANDLE;
  VkPipelineLayout m_nv12PipelineLayout = VK_NULL_HANDLE;
  VkPipelineLayout m_yuvComputePipelineLayout = VK_NULL_HANDLE;
  VkPipelineLayout m_nv12ComputePipelineLayout = VK_NULL_HANDLE;
  VkShaderModule m_effectsVertModule = VK_NULL_HANDLE;
  VkShaderModule m_effectsFragModule = VK_NULL_HANDLE;
  VkShaderModule m_maskVertModule = VK_NULL_HANDLE;
  VkShaderModule m_maskFragModule = VK_NULL_HANDLE;
  VkShaderModule m_nv12VertModule = VK_NULL_HANDLE;
  VkShaderModule m_nv12ComputeModule = VK_NULL_HANDLE;
  VkShaderModule m_yuv420pUFragModule = VK_NULL_HANDLE;
  VkShaderModule m_yuv420pVFragModule = VK_NULL_HANDLE;
  VkShaderModule m_yuv420pComputeModule = VK_NULL_HANDLE;
  VkPipeline m_effectsPipeline = VK_NULL_HANDLE;
  VkPipeline m_maskPipeline = VK_NULL_HANDLE;
  VkPipeline m_nv12ComputePipeline = VK_NULL_HANDLE;
  VkPipeline m_yuv420pUPipeline = VK_NULL_HANDLE;
  VkPipeline m_yuv420pVPipeline = VK_NULL_HANDLE;
  VkPipeline m_yuv420pComputePipeline = VK_NULL_HANDLE;
  VulkanMaskPreprocessor m_maskPreprocessor;
  VkImage m_nv12YImage = VK_NULL_HANDLE;
  VkDeviceMemory m_nv12YImageMemory = VK_NULL_HANDLE;
  VkImageView m_nv12YImageView = VK_NULL_HANDLE;
  VkImage m_yuv420pUImage = VK_NULL_HANDLE;
  VkDeviceMemory m_yuv420pUImageMemory = VK_NULL_HANDLE;
  VkImageView m_yuv420pUImageView = VK_NULL_HANDLE;
  VkImage m_yuv420pVImage = VK_NULL_HANDLE;
  VkDeviceMemory m_yuv420pVImageMemory = VK_NULL_HANDLE;
  VkImageView m_yuv420pVImageView = VK_NULL_HANDLE;
  VkRenderPass m_nv12YRenderPass = VK_NULL_HANDLE;
  VkFramebuffer m_nv12YFramebuffer = VK_NULL_HANDLE;
  VkFramebuffer m_yuv420pUFramebuffer = VK_NULL_HANDLE;
  VkFramebuffer m_yuv420pVFramebuffer = VK_NULL_HANDLE;
  AVFrame *m_nv12ScratchFrame = nullptr;
  bool m_colorImagePrimed = false;
  VkImageLayout m_colorImageLayout = VK_IMAGE_LAYOUT_UNDEFINED;
  bool m_yuv420pPlanesPrimed = false;
  bool m_commandBufferOpenForConversion = false;
  std::uint64_t m_producerSessionId = 0;
};

OffscreenVulkanRenderer::OffscreenVulkanRenderer()
    : d(std::make_unique<OffscreenVulkanRendererPrivate>()) {}

OffscreenVulkanRenderer::~OffscreenVulkanRenderer() = default;

bool OffscreenVulkanRenderer::initialize(const QSize &outputSize,
                                         QString *errorMessage) {
  return d->initialize(outputSize, errorMessage);
}

QImage OffscreenVulkanRenderer::renderFrame(
    const OffscreenRenderContext &context) {
  const RenderRequest &request = context.request;
  const qreal timelineFrame = context.timelineFrame;
  const qreal generatedEffectClockTimelineFrame =
      context.generatedEffectClockTimelineFrame >= 0.0
          ? context.generatedEffectClockTimelineFrame
          : timelineFrame;
  const qreal transformClockTimelineFrame = timelineFrame;
  QHash<QString, editor::DecoderContext *> &decoders = context.decoders;
  editor::AsyncDecoder *asyncDecoder = context.asyncDecoder;
  RenderPreparedFrameQueue *preparedFrames = context.preparedFrames;
  const QVector<TimelineClip> &orderedClips = context.orderedClips;
  QHash<QString, RenderClipStageStats> *clipStageStats =
      context.clipStageStats;
  qint64 *decodeMs = context.decodeMs;
  qint64 *textureMs = context.textureMs;
  qint64 *compositeMs = context.compositeMs;
  qint64 *readbackMs = context.readbackMs;
  qint64 *layerPlanMs = context.layerPlanMs;
  qint64 *textPrepMs = context.textPrepMs;
  qint64 *guideOverlayMs = context.guideOverlayMs;
  qint64 *gpuCompositeMs = context.gpuCompositeMs;
  QJsonArray *skippedClips = context.skippedClips;
  QJsonObject *skippedReasonCounts = context.skippedReasonCounts;
  QJsonObject *exportFaceTransformDiagnostics =
      context.exportFaceTransformDiagnostics;
  auto recordRenderFrameStageMetric =
      [&](const QString& id, const QString& label, qint64 elapsedMs) {
    if (!clipStageStats || elapsedMs <= 0) {
      return;
    }
    RenderClipStageStats& stats = (*clipStageStats)[id];
    if (stats.id.isEmpty()) {
      stats.id = id;
      stats.label = label;
    }
    stats.frames += 1;
    stats.compositeMs += elapsedMs;
  };
  auto recordRenderFrameCountMetric =
      [&](const QString& id, const QString& label, int count) {
    if (!clipStageStats || count <= 0) {
      return;
    }
    RenderClipStageStats& stats = (*clipStageStats)[id];
    if (stats.id.isEmpty()) {
      stats.id = id;
      stats.label = label;
    }
    stats.frames += count;
  };
  QStringList unresolvedVisualLayers;
  auto recordUnresolvedLayer =
      [&](const TimelineClip& clip, const QString& reason) {
    unresolvedVisualLayers.push_back(
        QStringLiteral("%1: %2")
            .arg(clip.id.isEmpty() ? clip.label : clip.id, reason));
    if (skippedClips) {
      skippedClips->push_back(QJsonObject{
          {QStringLiteral("clip_id"), clip.id},
          {QStringLiteral("clip_label"), clip.label},
          {QStringLiteral("reason"), reason},
      });
    }
    if (skippedReasonCounts) {
      skippedReasonCounts->insert(
          reason,
          skippedReasonCounts->value(reason).toInt() + 1);
    }
  };

  if (decodeMs) {
    *decodeMs = 0;
  }
  if (textureMs) {
    *textureMs = 0;
  }
  if (compositeMs) {
    *compositeMs = 0;
  }
  if (readbackMs) {
    *readbackMs = 0;
  }
  if (layerPlanMs) {
    *layerPlanMs = 0;
  }
  if (textPrepMs) {
    *textPrepMs = 0;
  }
  if (guideOverlayMs) {
    *guideOverlayMs = 0;
  }
  if (gpuCompositeMs) {
    *gpuCompositeMs = 0;
  }
  qint64 renderFrameDecodeWaitMs = 0;
  qint64 renderFrameMaskResolveMs = 0;
  QVector<OffscreenVulkanRendererPrivate::LayerInput> layers;
  layers.reserve((orderedClips.size() * 2) + 1);
  QVector<OffscreenVulkanRendererPrivate::LayerInput> foregroundMaskLayers;
  bool hasTranscriptCandidate = false;
  const QVector<TimelineClip> transcriptOverlayClips =
      sortedTranscriptOverlayClips(request.clips, request.tracks);
  for (const TimelineClip &clip : transcriptOverlayClips) {
    if (timelineFrame >= clip.startFrame &&
        timelineFrame < clip.startFrame + clip.durationFrames &&
        (clip.mediaType == ClipMediaType::Audio || clip.hasAudio) &&
        clip.transcriptOverlay.enabled) {
      hasTranscriptCandidate = true;
      break;
    }
  }
  int visualClipCandidates = 0;
  int visualLayersResolved = 0;
  int decodePathMissingCount = 0;
  int decodeNullCount = 0;
  int decodeConvertFailCount = 0;
  const RenderFrameClock frameClock =
      renderFrameClockForTimelinePosition(timelineFrame);
  QHash<QString, QHash<int64_t, editor::FrameHandle>> decodedFramesByTimingOwner;
  auto decodeFrameForTimingOwner =
      [&](const TimelineClip& timingOwner,
          const QString& decodePath,
          int64_t sourceFrame) -> editor::FrameHandle {
    const QString ownerIdentity = timingOwner.id.trimmed().isEmpty()
        ? QStringLiteral("path:") + decodePath
        : QStringLiteral("id:") + timingOwner.id.trimmed() +
              QStringLiteral("\x1fpath:") + decodePath;
    QHash<int64_t, editor::FrameHandle>& ownerFrames =
        decodedFramesByTimingOwner[ownerIdentity];
    const auto cached = ownerFrames.constFind(sourceFrame);
    if (cached != ownerFrames.cend()) {
      return cached.value();
    }

    const qint64 decodeStart = QDateTime::currentMSecsSinceEpoch();
    const editor::FrameHandle decoded = decodeRenderFrame(
        decodePath,
        sourceFrame,
        decoders,
        asyncDecoder,
        preparedFrames,
        context.forceSoftwareDecode,
        context.preferHardwareFrames);
    const qint64 elapsed = QDateTime::currentMSecsSinceEpoch() - decodeStart;
    renderFrameDecodeWaitMs += elapsed;
    if (decodeMs) {
      *decodeMs += elapsed;
    }
    // Cache failures as well as successes for this render pass. A parent and
    // each virtual child must observe the same decode result for a source
    // frame instead of independently retrying or selecting different frames.
    ownerFrames.insert(sourceFrame, decoded);
    return decoded;
  };
  QRectF transcriptLayerBounds;
  OffscreenVulkanRendererPrivate::VulkanTextInputs textInputs;
  const qint64 layerBuildStartMs = QDateTime::currentMSecsSinceEpoch();
  for (const TimelineClip &clip : orderedClips) {
    const TimelineClip &timingSource =
        resolvedClipTimingSource(clip, request.clips);
    const TimelineClip &mediaOwner = timingSource;
    const TimelineClip &timingOwner = timingSource;
    const TimelineClip &effectsOwner = clip;
    const TimelineClip &matteOwner = clip;
    const bool titleClip =
        clip.mediaType == ClipMediaType::Title &&
        !clip.transcriptOverlay.enabled;
    if (clip.clipRole == ClipRole::TranscriptSubtitle) {
      continue;
    }
    const bool visualLayerClip =
        titleClip ||
        clip.mediaType == ClipMediaType::Video ||
        clip.mediaType == ClipMediaType::Image ||
        clip.sourceKind == MediaSourceKind::ImageSequence;
    if (!visualLayerClip) {
      continue;
    }
    // An orphaned or malformed virtual matte must never fall back to its
    // serialized media-path cache and decode as an independent full layer.
    // Normalization removes these clips, but export remains fail-closed when
    // handed an unnormalized request from an older or external caller.
    if (clip.clipRole == ClipRole::MaskMatte &&
        (timingSource.id.trimmed() != clip.linkedSourceClipId.trimmed() ||
         timingSource.clipRole != ClipRole::Media ||
         timingSource.mediaType != ClipMediaType::Video ||
         timingSource.filePath.trimmed().isEmpty())) {
      continue;
    }
    const TimelineClip &activeRangeClip = titleClip ? clip : timingSource;
    if (timelineFrame < activeRangeClip.startFrame ||
        timelineFrame >= activeRangeClip.startFrame + activeRangeClip.durationFrames) {
      continue;
    }
    const bool clipVisible = clipVisualPlaybackEnabled(clip, request.tracks);
    if (!clipVisible) {
      continue;
    }
    const TimelineClip visualEffectsClip =
        titleClip ? clip : clipWithResolvedTimingOwner(effectsOwner, request.clips);
    EffectiveVisualEffects effects =
        request.bypassGrading
            ? EffectiveVisualEffects{}
            : evaluateEffectiveVisualEffectsAtPosition(
                  visualEffectsClip, request.tracks,
                  static_cast<qreal>(timelineFrame),
                  request.renderSyncMarkers,
                  request.playbackTiming);
    if (!request.correctionsEnabled) {
      effects.correctionPolygons.clear();
    }
    const TimelineClip::GradingKeyframe &grade = effects.grading;
    if (grade.opacity <= 0.001) {
      continue;
    }
    ++visualClipCandidates;
    QString decodePath;
    int64_t localFrame = qBound<int64_t>(
        0,
        timelineFrame - clip.startFrame,
        qMax<int64_t>(0, clip.durationFrames - 1));
    int64_t mappedSourceSample = frameClock.timelineSample;
    qreal mappedSourceFramePosition = static_cast<qreal>(localFrame);
    int64_t mappedTranscriptFrame = localFrame;
    editor::FrameHandle frame;
    if (titleClip) {
      if (clip.titleKeyframes.isEmpty()) {
        recordUnresolvedLayer(clip, QStringLiteral("title_keyframes_empty"));
        continue;
      }
      const EvaluatedTitle title = prepareRenderableTitleForVulkanText(
          clip,
          static_cast<qreal>(timelineFrame),
          request.playbackTiming,
          static_cast<qreal>(grade.opacity),
          request.outputSize);
      if (!title.valid) {
        recordUnresolvedLayer(clip, QStringLiteral("title_evaluated_invisible"));
        continue;
      }
      textInputs.title3D.push_back(title);
      ++visualLayersResolved;
      continue;
    } else {
      decodePath = playbackMediaPathForClip(mediaOwner);
      if (decodePath.isEmpty()) {
        ++decodePathMissingCount;
        recordUnresolvedLayer(clip, QStringLiteral("decode_path_missing"));
        continue;
      }
      const ClipFrameMapping frameMapping =
          clipFrameMappingForClock(
              clip, request.clips, frameClock, request.renderSyncMarkers);
      localFrame = frameMapping.sourceFrame;
      mappedSourceSample = frameMapping.sourceSample;
      mappedSourceFramePosition = frameMapping.sourceFramePosition;
      mappedTranscriptFrame = frameMapping.transcriptFrame;
      frame = decodeFrameForTimingOwner(timingOwner, decodePath, localFrame);
      if (frame.isNull()) {
        ++decodeNullCount;
        recordUnresolvedLayer(clip, QStringLiteral("decode_frame_unavailable"));
        continue;
      }
    }
    OffscreenVulkanRendererPrivate::LayerInput layer;
    layer.clipId = clip.id;
    layer.mediaOwnerClipId = mediaOwner.id;
    layer.timingOwnerClipId = timingOwner.id;
    layer.effectsOwnerClipId = effectsOwner.id;
    layer.matteOwnerClipId = matteOwner.id;
    layer.frame = frame;
    layer.frameSize = frame.size();
    layer.preferHardwareDirect = frame.hasHardwareFrame();
    if (!layer.preferHardwareDirect) {
      const QImage layerImage =
          frame.hasCpuImage() ? frame.cpuImage() : frameHandleToCpuImage(frame);
      if (layerImage.isNull()) {
        ++decodeConvertFailCount;
        recordUnresolvedLayer(clip, QStringLiteral("frame_conversion_failed"));
        continue;
      }
      layer.image = layerImage;
      layer.frameSize = layerImage.size();
    }
    if (clip.mediaType == ClipMediaType::Image && !layer.preferHardwareDirect) {
      layer.cacheKey = clip.id + QStringLiteral(":prepared_rgba");
    }
    const bool generatedMaskMatte = matteOwner.clipRole == ClipRole::MaskMatte;
    if (generatedMaskMatte &&
        (layer.mediaOwnerClipId != layer.timingOwnerClipId ||
         layer.effectsOwnerClipId != layer.clipId ||
         layer.matteOwnerClipId != layer.clipId)) {
      continue;
    }
    const bool gpuMaskEnabled =
        matteOwner.maskEnabled && !matteOwner.maskFramesDir.trimmed().isEmpty() &&
        (generatedMaskMatte || matteOwner.maskShowOnly ||
         matteOwner.maskForegroundLayerEnabled || matteOwner.maskRepeatEnabled);
    if (gpuMaskEnabled) {
      const qint64 maskResolveStartMs = QDateTime::currentMSecsSinceEpoch();
      // The matte follows the frame that was actually decoded/presented for
      // its parent. It must never combine a requested source key with a
      // different bounded decode result (TIME.md).
      QString maskIdentity;
      std::shared_ptr<const jcut::core::ImageBuffer> maskBuffer =
          frame.frameNumber() >= 0
              ? rawClipMaskBuffer(matteOwner, frame, &maskIdentity)
              : std::shared_ptr<const jcut::core::ImageBuffer>{};
      if (!maskBuffer && frame.frameNumber() >= 0) {
        maskBuffer = rawClipMaskBufferBlocking(
            matteOwner, frame, &maskIdentity);
      }
      if (maskBuffer) {
        layer.maskBuffer = maskBuffer;
        layer.maskIdentity = maskIdentity;
        layer.setCorrectionPolygons(
            generatedMaskMatte
                ? effects.correctionPolygons
                : QVector<TimelineClip::CorrectionPolygon>{});
        layer.maskSourceSize =
            QSize(maskBuffer->size.width, maskBuffer->size.height);
        layer.maskTextureEnabled = true;
        layer.maskClipSource = generatedMaskMatte;
        layer.maskShowOnly = matteOwner.maskShowOnly;
        layer.maskGradeEnabled = false;
        layer.maskForegroundLayerEnabled = matteOwner.maskForegroundLayerEnabled;
        layer.maskInvert = matteOwner.maskInvert;
        layer.maskErode = qRound(qMax<qreal>(0.0, matteOwner.maskErode));
        layer.maskDilate = qRound(qMax<qreal>(0.0, matteOwner.maskDilate));
        layer.maskBlur = qRound(qMax<qreal>(matteOwner.maskFeather, matteOwner.maskBlur));
        layer.maskTemporalStabilizeEnabled =
            matteOwner.maskTemporalStabilizeEnabled;
        layer.maskTemporalStabilizeStrength =
            matteOwner.maskTemporalStabilizeStrength;
        layer.maskTemporalStabilizeMotionRadius =
            matteOwner.maskTemporalStabilizeMotionRadius;
        if (matteOwner.maskTemporalStabilizeEnabled &&
            frame.frameNumber() >= 0) {
          QString previousIdentity;
          QString nextIdentity;
          layer.previousMaskBuffer = rawClipMaskBuffer(
              matteOwner,
              qMax<int64_t>(0, frame.frameNumber() - 1),
              &previousIdentity,
              true);
          layer.nextMaskBuffer = rawClipMaskBuffer(
              matteOwner,
              frame.frameNumber() + 1,
              &nextIdentity,
              true);
          layer.temporalMaskIdentity =
              previousIdentity + QLatin1Char('|') + nextIdentity;
        }
        layer.maskFeatherGamma = static_cast<float>(
            qBound<qreal>(0.1, matteOwner.maskFeatherGamma, 5.0));
        layer.maskFeatherFalloff = qBound(0, matteOwner.maskFeatherFalloff, 5);
        layer.maskEdgeGrayAmount =
            qBound<qreal>(0.0, matteOwner.maskEdgeGrayAmount, 1.0);
        layer.maskEdgeGrayWidth =
            qBound<qreal>(0.001, matteOwner.maskEdgeGrayWidth, 2.0);
        layer.maskEdgeGrayGamma =
            qBound<qreal>(0.1, matteOwner.maskEdgeGrayGamma, 8.0);
        layer.maskOpacity = static_cast<float>(qBound<qreal>(0.0, matteOwner.maskOpacity, 1.0));
        layer.maskDropShadowRadius = static_cast<float>(
            qBound<qreal>(0.0, matteOwner.maskDropShadowRadius, 200.0));
        layer.maskDropShadowOffsetX = static_cast<float>(matteOwner.maskDropShadowOffsetX);
        layer.maskDropShadowOffsetY = static_cast<float>(matteOwner.maskDropShadowOffsetY);
        layer.maskDropShadowOpacity = static_cast<float>(
            qBound<qreal>(0.0, matteOwner.maskDropShadowOpacity, 1.0));
        layer.maskDropShadowEnabled = generatedMaskMatte &&
            matteOwner.maskDropShadowEnabled && layer.maskDropShadowOpacity > 0.0f;
        TimelineClip::GradingKeyframe maskGrade;
        maskGrade.brightness = matteOwner.maskGradeBrightness;
        maskGrade.contrast = matteOwner.maskGradeContrast;
        maskGrade.saturation = matteOwner.maskGradeSaturation;
        maskGrade.curvePointsR = matteOwner.maskGradeCurvePointsR;
        maskGrade.curvePointsG = matteOwner.maskGradeCurvePointsG;
        maskGrade.curvePointsB = matteOwner.maskGradeCurvePointsB;
        maskGrade.curvePointsLuma = matteOwner.maskGradeCurvePointsLuma;
        maskGrade.curveSmoothingEnabled = matteOwner.maskGradeCurveSmoothingEnabled;
        layer.setMaskGrade(maskGrade);
      }
      renderFrameMaskResolveMs += QDateTime::currentMSecsSinceEpoch() - maskResolveStartMs;
    }
    // A generated matte is never allowed to fall back to a full-frame copy
    // when its sidecar has no sample for this frame.
    if (generatedMaskMatte && !layer.maskTextureEnabled) {
      --visualClipCandidates;
      continue;
    }
    layer.setGrading(grade);
    VulkanDrawEffectState& layerEffects = layer.gradePayload.effects;
    layerEffects.shadows[3] = layer.gradePayload.curveLutApplied
                           ? kVulkanEffectModeCurve
                           : kVulkanEffectModeNormal;
    if (generatedMaskMatte) {
      layerEffects.opacity *= layer.maskOpacity;
      layer.maskGrade = layer.grading;
      layer.maskGradePayload = layer.gradePayload;
      layerEffects.midtones[3] = layer.gradePayload.curveLutApplied
          ? kVulkanMaskGradeUseSelectedCurveLut : 0.0f;
    }
    QJsonObject transformDiagnostics;
    const TimelineClip::TransformKeyframe transform =
        evaluateClipRenderTransformWithSourceLockAtPosition(
            clip,
            request.clips,
            transformClockTimelineFrame,
            request.renderSyncMarkers,
            request.playbackTiming,
            request.outputSize,
            &transformDiagnostics);
    layer.transform = transform;
    const QSize sourceSize = timingSource.sourceFrameSize.isValid()
        ? timingSource.sourceFrameSize
        : (layer.frameSize.isValid() ? layer.frameSize : layer.image.size());
    const QRectF fitted = fitRectF(sourceSize, request.outputSize);
    QPointF exportVideoTranslation(transform.translationX, transform.translationY);
    PreviewClipGeometry layerGeometry = PreviewViewTransform::clipGeometry(
        fitted,
        QPointF(1.0, 1.0),
        exportVideoTranslation,
        transform.rotation,
        QPointF(transform.scaleX, transform.scaleY));
    const QRectF outputRect(QPointF(0.0, 0.0), QSizeF(request.outputSize));
    layer.targetRect = outputRect;
    layer.fittedRect = fitted;
    const TimelineClip effectClip = clipWithResolvedTimingOwner(
        evaluateClipEffectAnimationAtPosition(
            clipWithRenderableEffectSettings(effectsOwner, request.tracks),
            static_cast<qreal>(timelineFrame),
            request.renderSyncMarkers,
            request.playbackTiming),
        request.clips);
    if (!titleClip && effectClip.effectPreset == ClipEffectPreset::DifferenceMatte) {
      layer.differenceThreshold =
          qBound<qreal>(0.0, effectClip.differenceThreshold, 1.0);
      layer.differenceSoftness =
          qBound<qreal>(0.0, effectClip.differenceSoftness, 1.0);
      const int64_t referenceFrameNumber =
          qMax<int64_t>(0, localFrame - qBound(1, effectClip.differenceReferenceFrames, 300));
      layer.differenceReferenceFrame = decodeFrameForTimingOwner(
          timingSource, decodePath, referenceFrameNumber);
      if (!layer.differenceReferenceFrame.isNull()) {
        layer.differenceMatteEnabled = true;
        layerEffects.shadows[3] = kVulkanEffectModeDifferenceMatte;
        layerEffects.midtones[3] =
            static_cast<float>(layer.differenceThreshold);
        layerEffects.highlights[3] =
            static_cast<float>(layer.differenceSoftness);
        layer.maskTextureEnabled = false;
      } else {
        recordUnresolvedLayer(
            clip, QStringLiteral("difference_reference_unavailable"));
      }
    }
    const bool clipEdgeFillEffect =
        effectClip.edgeFillEffect != BackgroundFillEffect::None &&
        vulkanClipSupportsBackgroundFillSource(clip);
    OffscreenVulkanRendererPrivate::LayerInput bidirectionalEdgeLayer;
    bool bidirectionalEdgeLayerPending = false;
    TimelineClip foregroundEffectClip = effectClip;
    const QRectF effectBounds =
        (foregroundEffectClip.effectPreset == ClipEffectPreset::SourceTile ||
         foregroundEffectClip.maskRepeatEnabled)
            ? layerGeometry.bounds.intersected(outputRect)
            : outputRect;
    QRectF tilingMaskBounds;
    if (foregroundEffectClip.effectPreset == ClipEffectPreset::SourceTile &&
        foregroundEffectClip.tilingUseMaskBounds && layer.maskBuffer) {
      const QRectF normalizedMaskBounds =
          normalizedMaskContentBounds(
              *layer.maskBuffer,
              foregroundEffectClip.tilingMaskIslandSigma,
              matteOwner.maskInvert);
      if (!normalizedMaskBounds.isEmpty()) {
        tilingMaskBounds =
            layerGeometry.clipToScreen
                .mapRect(PreviewViewTransform::localRectForNormalizedRect(
                    normalizedMaskBounds, layerGeometry.localRect))
                .intersected(outputRect);
      }
    }
    VulkanEffectPipelinePlan effectPlan = vulkanEffectPipelinePlan(
        foregroundEffectClip,
        effectBounds,
        sourceSize,
        timelineFrame,
        clipEffectPlaybackFramePosition(foregroundEffectClip, request.clips, generatedEffectClockTimelineFrame,
                                        request.playbackTiming,
                                        request.tracks),
        request.playbackTiming,
        tilingMaskBounds);
    if (generatedMaskMatte && layer.maskBuffer &&
        effectPresetUsesGeneratedMaskDomain(foregroundEffectClip.effectPreset)) {
      const QRectF normalizedMaskDomain =
          normalizedMaskContentBounds(
              *layer.maskBuffer,
              foregroundEffectClip.tilingMaskIslandSigma,
              matteOwner.maskInvert);
      const QRectF maskDomain =
          normalizedMaskDomain.isEmpty()
              ? QRectF{}
              : layerGeometry.clipToScreen
                    .mapRect(PreviewViewTransform::localRectForNormalizedRect(
                        normalizedMaskDomain, layerGeometry.localRect))
                    .intersected(outputRect);
      applyGeneratedEffectMaskDomain(
          effectPlan, maskDomain, outputRect, true, normalizedMaskDomain);
    }
    layer.effectPlan = effectPlan;
    const PlaybackFrameCrossfade frameCrossfade =
        clipShouldApplySpeechFilterFrameCrossfade(clip)
            ? playbackFrameCrossfadeAtTimelineFrame(
                  generatedEffectClockTimelineFrame,
                  request.playbackTiming)
            : PlaybackFrameCrossfade{};
    OffscreenVulkanRendererPrivate::LayerInput frameCrossfadeLayer;
    bool frameCrossfadeLayerPending = false;
    if (frameCrossfade.active) {
      const qreal secondaryTimelineFrame =
          static_cast<qreal>(frameCrossfade.secondaryTimelineFrame);
      const bool secondaryClipVisible =
          secondaryTimelineFrame >= activeRangeClip.startFrame &&
          secondaryTimelineFrame <
              activeRangeClip.startFrame +
                  activeRangeClip.durationFrames;
      if (secondaryClipVisible) {
        const RenderFrameClock secondaryClock =
            renderFrameClockForTimelinePosition(
                secondaryTimelineFrame);
        const ClipFrameMapping secondaryMapping =
            clipFrameMappingForClock(
                clip,
                request.clips,
                secondaryClock,
                request.renderSyncMarkers);
        const editor::FrameHandle secondaryFrame =
            decodeFrameForTimingOwner(
                timingOwner,
                decodePath,
                secondaryMapping.sourceFrame);
        if (!secondaryFrame.isNull()) {
          frameCrossfadeLayer = layer;
          frameCrossfadeLayer.clipId =
              layer.clipId + QStringLiteral("#frameCrossfade");
          frameCrossfadeLayer.frame = secondaryFrame;
          frameCrossfadeLayer.frameSize = secondaryFrame.size();
          frameCrossfadeLayer.preferHardwareDirect =
              secondaryFrame.hasHardwareFrame();
          frameCrossfadeLayer.image = {};
          if (!frameCrossfadeLayer.preferHardwareDirect) {
            frameCrossfadeLayer.image =
                secondaryFrame.hasCpuImage()
                    ? secondaryFrame.cpuImage()
                    : frameHandleToCpuImage(secondaryFrame);
            frameCrossfadeLayer.frameSize =
                frameCrossfadeLayer.image.size();
          }
          frameCrossfadeLayer.cacheKey.clear();
          frameCrossfadeLayer.effectPlan.generatedDraws.clear();
          frameCrossfadeLayer.temporalEchoFrames.clear();
          // The secondary sample has its own timeline position.  Do not reuse
          // the primary sample's temporal neighbors while the crossfade is
          // being prepared; the direct preview follows the same rule.
          frameCrossfadeLayer.maskTemporalStabilizeEnabled = false;
          frameCrossfadeLayer.previousMaskBuffer.reset();
          frameCrossfadeLayer.nextMaskBuffer.reset();
          frameCrossfadeLayer.temporalMaskIdentity.clear();
          frameCrossfadeLayer.differenceMatteEnabled = false;
          frameCrossfadeLayer.differenceReferenceFrame = {};
          frameCrossfadeLayer.maskDropShadowEnabled = false;
          frameCrossfadeLayer.gradePayload.effects.opacity *=
              qBound(
                  0.0f,
                  frameCrossfade.secondaryOpacity,
                  1.0f);
          frameCrossfadeLayer.frameCrossfadeActive = false;
          frameCrossfadeLayer.frameCrossfadeFrame = {};
          frameCrossfadeLayer.frameCrossfadeMaskBuffer.reset();
          frameCrossfadeLayer.frameCrossfadeMaskIdentity.clear();
          frameCrossfadeLayer.frameCrossfadeMaskTextureEnabled =
              false;

          bool secondaryMaskReady = true;
          if (gpuMaskEnabled) {
            QString secondaryMaskIdentity;
            auto secondaryMaskBuffer = rawClipMaskBuffer(
                matteOwner,
                secondaryFrame,
                &secondaryMaskIdentity);
            if (!secondaryMaskBuffer) {
              secondaryMaskBuffer = rawClipMaskBufferBlocking(
                  matteOwner,
                  secondaryFrame,
                  &secondaryMaskIdentity);
            }
            frameCrossfadeLayer.maskBuffer =
                secondaryMaskBuffer;
            frameCrossfadeLayer.maskIdentity =
                secondaryMaskIdentity;
            frameCrossfadeLayer.maskTextureEnabled =
                static_cast<bool>(secondaryMaskBuffer);
            secondaryMaskReady =
                !generatedMaskMatte ||
                frameCrossfadeLayer.maskTextureEnabled;
          }
          frameCrossfadeLayerPending =
              secondaryMaskReady &&
              (frameCrossfadeLayer.preferHardwareDirect ||
               !frameCrossfadeLayer.image.isNull());
          if (qEnvironmentVariableIsSet(
                  "JCUT_TRACE_FRAME_CROSSFADE")) {
            qInfo().noquote()
                << QStringLiteral(
                       "[frame-crossfade] transport=%1 visual=%2 clip=%3 "
                       "secondary_timeline=%4 requested=%5 presented=%6 "
                       "opacity=%7 ready=%8")
                       .arg(generatedEffectClockTimelineFrame)
                       .arg(timelineFrame)
                       .arg(clip.id)
                       .arg(frameCrossfade.secondaryTimelineFrame)
                       .arg(secondaryMapping.sourceFrame)
                       .arg(secondaryFrame.frameNumber())
                       .arg(frameCrossfade.secondaryOpacity)
                       .arg(frameCrossfadeLayerPending);
          }
          if (frameCrossfadeLayerPending) {
            layer.frameCrossfadeActive = true;
            layer.frameCrossfadeTimelineFrame =
                frameCrossfade.secondaryTimelineFrame;
            layer.frameCrossfadeRequestedSourceFrame =
                secondaryMapping.sourceFrame;
            layer.frameCrossfadePresentedSourceFrame =
                secondaryFrame.frameNumber();
            layer.frameCrossfadeOpacity =
                frameCrossfade.secondaryOpacity;
            layer.frameCrossfadeFrame = secondaryFrame;
            layer.frameCrossfadeFrameSize =
                secondaryFrame.size();
            layer.frameCrossfadeMaskBuffer =
                frameCrossfadeLayer.maskBuffer;
            layer.frameCrossfadeMaskIdentity =
                frameCrossfadeLayer.maskIdentity;
            layer.frameCrossfadeMaskTextureEnabled =
                frameCrossfadeLayer.maskTextureEnabled;
          }
        }
      }
    }
    if (foregroundEffectClip.effectPreset == ClipEffectPreset::SourceTile && effectBounds.isValid()) {
      layer.presetScissorEnabled = true;
      layer.presetScissorRect = effectBounds;
    }
    if (exportFaceTransformDiagnostics && clip.speakerFramingEnabled) {
      transformDiagnostics.insert(QStringLiteral("clip_id"), clip.id);
      transformDiagnostics.insert(QStringLiteral("clip_label"), clip.label);
      transformDiagnostics.insert(QStringLiteral("timeline_frame_position"), timelineFrame);
      transformDiagnostics.insert(QStringLiteral("transform_clock_timeline_frame_position"),
                                  transformClockTimelineFrame);
      transformDiagnostics.insert(QStringLiteral("timeline_sample"), static_cast<qint64>(frameClock.timelineSample));
      transformDiagnostics.insert(QStringLiteral("sync_clock_domain"), QStringLiteral("timeline_sample"));
      transformDiagnostics.insert(QStringLiteral("decode_source_frame"), static_cast<qint64>(localFrame));
      transformDiagnostics.insert(QStringLiteral("mapped_source_sample"), static_cast<qint64>(mappedSourceSample));
      transformDiagnostics.insert(QStringLiteral("mapped_source_frame_position"), mappedSourceFramePosition);
      transformDiagnostics.insert(QStringLiteral("mapped_transcript_frame"), static_cast<qint64>(mappedTranscriptFrame));
      transformDiagnostics.insert(QStringLiteral("renderer_texture_origin"), QStringLiteral("top_left"));
      transformDiagnostics.insert(QStringLiteral("renderer_texture_normalized"), true);
      transformDiagnostics.insert(QStringLiteral("export_video_translation"), QJsonObject{
          {QStringLiteral("x"), exportVideoTranslation.x()},
          {QStringLiteral("y"), exportVideoTranslation.y()}
      });
      transformDiagnostics.insert(QStringLiteral("output_path"), request.outputPath);
      const QRectF layerRect = layerGeometry.bounds;
      transformDiagnostics.insert(QStringLiteral("layer_center"), QJsonObject{
          {QStringLiteral("x"), layerRect.center().x()},
          {QStringLiteral("y"), layerRect.center().y()}
      });
      transformDiagnostics.insert(QStringLiteral("layer_size"), QJsonObject{
          {QStringLiteral("width"), layerRect.width()},
          {QStringLiteral("height"), layerRect.height()}
      });
      transformDiagnostics.insert(QStringLiteral("layer_rect"),
                                  rectDiagnosticObject(layerRect));
      transformDiagnostics.insert(QStringLiteral("face_target_rect"),
                                  rectDiagnosticObject(
                                      faceTargetRectFromTransformDiagnostics(
                                          transformDiagnostics)));
      *exportFaceTransformDiagnostics = transformDiagnostics;
    }
    vulkanMvpForExportVideoLayer(
        fitted,
        exportVideoTranslation,
        request.outputSize,
        transform.rotation,
        QPointF(transform.scaleX, transform.scaleY),
        layer.mvp);
    if (frameCrossfadeLayerPending) {
      std::copy(
          std::begin(layer.mvp),
          std::end(layer.mvp),
          std::begin(frameCrossfadeLayer.mvp));
      frameCrossfadeLayer.presetScissorEnabled =
          layer.presetScissorEnabled;
      frameCrossfadeLayer.presetScissorRect =
          layer.presetScissorRect;
    }
    if (clipVisible && clipEdgeFillEffect) {
      OffscreenVulkanRendererPrivate::LayerInput backgroundLayer;
      backgroundLayer.frameSize = request.outputSize;
      const BackgroundFillEffect fillEffect = effectClip.edgeFillEffect;
      const int edgePixels = qBound(1, effectClip.edgeFillPixels, 512);
      const qreal edgePower =
          qBound<qreal>(0.25, effectClip.edgeFillPower, 8.0);
      const VulkanDrawEffectState backgroundEffects =
          vulkanBackgroundFillEffectState(
              fillEffect,
              static_cast<float>(effectClip.edgeFillOpacity),
              static_cast<float>(effectClip.edgeFillBrightness),
              static_cast<float>(effectClip.edgeFillSaturation),
              edgePixels,
              static_cast<float>(edgePower),
              frame.validTextureRectNormalized(),
              vulkanBackgroundFillMapping(
                  layerGeometry.clipToScreen,
                  layerGeometry.localRect,
                  request.outputSize));
      backgroundLayer.gradePayload = layer.gradePayload;
      backgroundLayer.gradePayload.effects = backgroundEffects;
      std::copy(std::begin(layerEffects.shadows),
                std::end(layerEffects.shadows),
                std::begin(backgroundLayer.backgroundShadows));
      std::copy(std::begin(layerEffects.midtones),
                std::end(layerEffects.midtones),
                std::begin(backgroundLayer.backgroundMidtones));
      std::copy(std::begin(layerEffects.highlights),
                std::end(layerEffects.highlights),
                std::begin(backgroundLayer.backgroundHighlights));
      backgroundLayer.backgroundGrade[0] = layerEffects.brightness;
      backgroundLayer.backgroundGrade[1] = layerEffects.contrast;
      backgroundLayer.backgroundGrade[2] = layerEffects.saturation;
      // Background fills are derived views of this source layer, not new
      // color owners. Inherit the complete canonical grade payload so preview
      // and export bind the same curve LUT as well as the same tonal values.
      backgroundLayer.frame = frame;
      backgroundLayer.image = layer.image;
      backgroundLayer.frameSize = sourceSize;
      backgroundLayer.preferHardwareDirect = frame.hasHardwareFrame();
      if (!layer.cacheKey.isEmpty()) {
        backgroundLayer.cacheKey =
            layer.cacheKey + QStringLiteral(":progressive_background");
      }

      const bool fullCanvasFill =
          fillEffect == BackgroundFillEffect::EdgeStretch ||
          fillEffect == BackgroundFillEffect::ProgressiveEdgeStretch ||
          fillEffect == BackgroundFillEffect::ProgressiveBidirectionalEdgeStretch ||
          fillEffect == BackgroundFillEffect::Tile ||
          fillEffect == BackgroundFillEffect::Mirror;
      PreviewClipGeometry backgroundGeometry =
          fullCanvasFill ? PreviewViewTransform::clipGeometry(outputRect,
                                                              QPointF(1.0, 1.0),
                                                              QPointF(),
                                                              0.0,
                                                              QPointF(1.0, 1.0))
                         : layerGeometry;
      if (fillEffect == BackgroundFillEffect::BlurCover) {
        const qreal coverScale = std::max<qreal>(
            1.0,
            std::max(outputRect.width() / qMax<qreal>(1.0, layerGeometry.bounds.width()),
                     outputRect.height() / qMax<qreal>(1.0, layerGeometry.bounds.height())));
        backgroundGeometry.clipToScreen.scale(coverScale * 1.08, coverScale * 1.08);
      }
      vulkanMvpForPreviewTransform(backgroundGeometry.clipToScreen,
                                   backgroundGeometry.localRect,
                                   request.outputSize,
                                   backgroundLayer.mvp);
      if (!backgroundLayer.image.isNull() ||
          !backgroundLayer.frame.isNull()) {
        if (fillEffect ==
            BackgroundFillEffect::ProgressiveBidirectionalEdgeStretch) {
          bidirectionalEdgeLayer = backgroundLayer;
          bidirectionalEdgeLayerPending = true;
        } else {
          layers.push_back(backgroundLayer);
        }
      }
    }
    QVector<int> temporalEchoOrdinals;
    if (clipVisible) {
      if (!titleClip && effectClip.effectPreset == ClipEffectPreset::TemporalEcho) {
        const int echoCount = qBound(1, effectClip.temporalEchoCount, 12);
        const int spacing = qBound(1, effectClip.temporalEchoSpacingFrames, 120);
        const qreal decay = qBound<qreal>(0.0, effectClip.temporalEchoDecay, 1.0);
        layer.temporalEchoDecay = decay;
        for (int echoIndex = 1; echoIndex <= echoCount; ++echoIndex) {
          const int64_t echoFrameNumber = qMax<int64_t>(0, localFrame - echoIndex * spacing);
          const editor::FrameHandle echoFrame = decodeFrameForTimingOwner(
              timingSource, decodePath, echoFrameNumber);
          if (echoFrame.isNull()) {
            recordUnresolvedLayer(
                clip, QStringLiteral("temporal_echo_frame_unavailable"));
            continue;
          }
          layer.temporalEchoFrames.push_back(echoFrame);
          temporalEchoOrdinals.push_back(echoIndex);
        }
      }
      layers.push_back(layer);
      if (effectClip.effectPreset == ClipEffectPreset::TemporalEcho) {
        const qreal decay = layer.temporalEchoDecay;
        for (int echoIndex = 0;
             echoIndex < layer.temporalEchoFrames.size();
             ++echoIndex) {
          const editor::FrameHandle& echoFrame =
              layer.temporalEchoFrames.at(echoIndex);
          OffscreenVulkanRendererPrivate::LayerInput echoLayer = layer;
          echoLayer.frame = echoFrame;
          echoLayer.differenceReferenceFrame = {};
          echoLayer.differenceMatteEnabled = false;
          echoLayer.image = echoFrame.hasCpuImage() ? echoFrame.cpuImage() : QImage();
          echoLayer.frameSize = echoFrame.size();
          echoLayer.preferHardwareDirect = echoFrame.hasHardwareFrame();
          echoLayer.cacheKey.clear();
          echoLayer.effectPlan.generatedDraws.clear();
          echoLayer.maskTextureEnabled = false;
          echoLayer.gradePayload.effects.opacity = static_cast<float>(
              qBound<qreal>(
                  0.0,
                  grade.opacity *
                      std::pow(decay,
                               temporalEchoOrdinals.value(
                                   echoIndex, echoIndex + 1)),
                  1.0));
          echoLayer.gradePayload.effects.shadows[3] =
              echoLayer.gradePayload.curveLutApplied
              ? kVulkanEffectModeCurve : kVulkanEffectModeNormal;
          layers.push_back(echoLayer);
        }
      }
      if (frameCrossfadeLayerPending) {
        layers.push_back(frameCrossfadeLayer);
      }
    }
    if (bidirectionalEdgeLayerPending) {
      layers.push_back(bidirectionalEdgeLayer);
    }
    if (layer.maskTextureEnabled &&
        layer.maskForegroundLayerEnabled) {
      OffscreenVulkanRendererPrivate::LayerInput foregroundLayer = layer;
      const bool applyMaskGradeToForeground = foregroundLayer.maskGradeEnabled;
      foregroundLayer.effectPlan.generatedDraws.clear();
      foregroundLayer.maskShowOnly = false;
      foregroundLayer.maskGradeEnabled = false;
      VulkanDrawEffectState& foregroundEffects =
          foregroundLayer.gradePayload.effects;
      foregroundEffects.opacity = static_cast<float>(layer.maskOpacity);
      foregroundLayer.maskDropShadowEnabled =
          clip.maskDropShadowEnabled && layer.maskDropShadowOpacity > 0.0f;
      foregroundEffects.brightness = applyMaskGradeToForeground
          ? layer.maskGradePayload.effects.brightness : 0.0f;
      foregroundEffects.contrast = applyMaskGradeToForeground
          ? layer.maskGradePayload.effects.contrast : 1.0f;
      foregroundEffects.saturation = applyMaskGradeToForeground
          ? layer.maskGradePayload.effects.saturation : 1.0f;
      foregroundEffects.shadows[0] = 0.0f;
      foregroundEffects.shadows[1] = 0.0f;
      foregroundEffects.shadows[2] = 0.0f;
      foregroundEffects.shadows[3] = kVulkanEffectModeMaskGrade;
      foregroundEffects.midtones[0] = 0.0f;
      foregroundEffects.midtones[1] = 0.0f;
      foregroundEffects.midtones[2] = 0.0f;
      foregroundEffects.midtones[3] =
          applyMaskGradeToForeground &&
                  layer.maskGradePayload.curveLutApplied
              ? kVulkanMaskGradeUseSelectedCurveLut
              : 0.0f;
      foregroundEffects.highlights[0] = 0.0f;
      foregroundEffects.highlights[1] = 0.0f;
      foregroundEffects.highlights[2] = 0.0f;
      foregroundEffects.highlights[3] = static_cast<float>(
          foregroundLayer.maskFeatherFalloff * 10) +
          foregroundLayer.maskFeatherGamma;
      foregroundMaskLayers.push_back(foregroundLayer);
    }
    if (!titleClip && !clip.titleKeyframes.isEmpty()) {
      const EvaluatedTitle title = prepareRenderableTitleForVulkanText(
          clip,
          static_cast<qreal>(timelineFrame),
          request.playbackTiming,
          static_cast<qreal>(grade.opacity),
          request.outputSize);
      if (title.valid) {
        textInputs.title3D.push_back(title);
      }
    }
    ++visualLayersResolved;
  }
  const qint64 layerBuildElapsedMs =
      QDateTime::currentMSecsSinceEpoch() - layerBuildStartMs;
  const qint64 layerPlanElapsedMs =
      qMax<qint64>(0, layerBuildElapsedMs - renderFrameDecodeWaitMs -
                          renderFrameMaskResolveMs);
  if (layerPlanMs) {
    *layerPlanMs += layerPlanElapsedMs;
  }
  layers += foregroundMaskLayers;
  const qint64 textInputStartMs = QDateTime::currentMSecsSinceEpoch();
  QStringList subtitleFailures;
  if (hasTranscriptCandidate) {
    textInputs.transcripts = d->buildTranscriptTextInputs(
        request.outputSize, request,
        frameClock,
        transcriptOverlayClips,
        &subtitleFailures);
    for (const OffscreenVulkanRendererPrivate::TranscriptTextInput &text : textInputs.transcripts) {
      transcriptLayerBounds = transcriptLayerBounds.united(text.outputRect);
    }
  }
  if (!subtitleFailures.isEmpty()) {
    const QString failure = QStringLiteral(
        "Vulkan export refused to drop subtitle overlay(s) at frame %1: %2")
        .arg(timelineFrame)
        .arg(subtitleFailures.join(QStringLiteral("; ")));
    qWarning().noquote() << QStringLiteral(
                                "[vulkan-subtitle] definitive subtitle failure "
                                "frame=%1 failures=%2")
                                .arg(timelineFrame)
                                .arg(subtitleFailures.join(QStringLiteral(" | ")));
    if (context.frameFailureReason) {
      *context.frameFailureReason = failure;
    }
    return QImage();
  }
  textInputs.hasSpeakerLabel = d->buildSpeakerLabelSpec(
      request,
      frameClock,
      orderedClips,
      &textInputs.speakerLabel);
  const qint64 textInputElapsedMs =
      QDateTime::currentMSecsSinceEpoch() - textInputStartMs;
  if (textPrepMs) {
    *textPrepMs += textInputElapsedMs;
  }
  if (exportFaceTransformDiagnostics &&
      textInputs.hasSpeakerLabel &&
      !exportFaceTransformDiagnostics->isEmpty()) {
    const VulkanTextLayoutDebug speakerLabelDebug =
        d->speakerLabelLayoutDebug(request.outputSize, textInputs.speakerLabel);
    if (speakerLabelDebug.valid) {
      exportFaceTransformDiagnostics->insert(
          QStringLiteral("speaker_label_cards"),
          rectDiagnosticArray(speakerLabelDebug.cards));
      exportFaceTransformDiagnostics->insert(
          QStringLiteral("speaker_label_card_count"),
          speakerLabelDebug.cardCount);
      exportFaceTransformDiagnostics->insert(
          QStringLiteral("speaker_label_name"),
          textInputs.speakerLabel.name);
      exportFaceTransformDiagnostics->insert(
          QStringLiteral("speaker_label_organization"),
          textInputs.speakerLabel.organization);
    }
  }
  if (!unresolvedVisualLayers.isEmpty() ||
      visualLayersResolved != visualClipCandidates) {
    const QString failure = QStringLiteral(
        "Vulkan export refused an incomplete composition at frame %1 "
        "(resolved %2/%3): %4")
        .arg(timelineFrame)
        .arg(visualLayersResolved)
        .arg(visualClipCandidates)
        .arg(unresolvedVisualLayers.join(QStringLiteral("; ")));
    qWarning().noquote() << QStringLiteral(
                                "[vulkan-compose] incomplete visual layers at "
                                "frame=%1 candidates=%2 decode_path_missing=%3 "
                                "decode_null=%4 convert_fail=%5")
                                .arg(timelineFrame)
                                .arg(visualClipCandidates)
                                .arg(decodePathMissingCount)
                                .arg(decodeNullCount)
                                .arg(decodeConvertFailCount);
    if (context.frameFailureReason) {
      *context.frameFailureReason = failure;
    }
    return QImage();
  }
  if (layers.isEmpty()) {
    OffscreenVulkanRendererPrivate::LayerInput black;
    black.image = QImage(request.outputSize, QImage::Format_RGBA8888);
    black.image.fill(Qt::black);
    black.frameSize = black.image.size();
    black.gradePayload.effects.opacity = 1.0f;
    layers.push_back(black);
  }
  const qint64 guidePrepareStartMs = QDateTime::currentMSecsSinceEpoch();
  if (request.instagramSafeAreaGuides || request.alignmentGridGuides) {
    OffscreenVulkanRendererPrivate::LayerInput guides;
    guides.clipId = QStringLiteral("output-placement-guides");
    guides.overlayImage =
        d->placementGuideOverlay(request.outputSize,
                                 request.instagramSafeAreaGuides,
                                 request.alignmentGridGuides);
    guides.frameSize = request.outputSize;
    guides.gradePayload.effects.opacity = 1.0f;
    if (!guides.overlayImage.isNull()) {
      render_detail::vulkanMvpForOutputRect(
          QRectF(QPointF(0.0, 0.0), QSizeF(request.outputSize)),
          request.outputSize,
          0.0,
          guides.mvp);
      layers.push_back(guides);
    }
  }
  const qint64 guidePrepareElapsedMs =
      QDateTime::currentMSecsSinceEpoch() - guidePrepareStartMs;
  if (guideOverlayMs) {
    *guideOverlayMs += guidePrepareElapsedMs;
  }
  const qint64 renderStartMs = QDateTime::currentMSecsSinceEpoch();
  const bool shouldReadbackToImage = (readbackMs != nullptr);
  const QImage output = d->renderFrameFromLayers(layers,
                                                 textInputs,
                                                 shouldReadbackToImage,
                                                 context.gpuPreviewFrame,
                                                 context.gpuPreviewError,
                                                 context.frameFailureReason);
  const qint64 compositeElapsedMs =
      QDateTime::currentMSecsSinceEpoch() - renderStartMs;
  if (gpuCompositeMs) {
    *gpuCompositeMs += compositeElapsedMs;
  }
  if (compositeMs) {
    *compositeMs = compositeElapsedMs;
  }
  recordRenderFrameCountMetric(
      QStringLiteral("__render_frame_hardware_source_import__"),
      QStringLiteral("__render_frame_hardware_source_import__"),
      d->lastHardwareSourceImportCount());
  recordRenderFrameCountMetric(
      QStringLiteral("__render_frame_hardware_source_reuse__"),
      QStringLiteral("__render_frame_hardware_source_reuse__"),
      d->lastHardwareSourceReuseCount());
  recordRenderFrameStageMetric(
      QStringLiteral("__render_frame_layer_build__"),
      QStringLiteral("__render_frame_layer_build__"),
      layerPlanElapsedMs);
  recordRenderFrameStageMetric(
      QStringLiteral("__render_frame_decode_wait__"),
      QStringLiteral("__render_frame_decode_wait__"),
      renderFrameDecodeWaitMs);
  recordRenderFrameStageMetric(
      QStringLiteral("__render_frame_mask_resolve__"),
      QStringLiteral("__render_frame_mask_resolve__"),
      renderFrameMaskResolveMs);
  recordRenderFrameStageMetric(
      QStringLiteral("__render_frame_text_inputs__"),
      QStringLiteral("__render_frame_text_inputs__"),
      textInputElapsedMs);
  recordRenderFrameStageMetric(
      QStringLiteral("__render_frame_guide_prepare__"),
      QStringLiteral("__render_frame_guide_prepare__"),
      guidePrepareElapsedMs);
  recordRenderFrameStageMetric(
      QStringLiteral("__render_frame_vulkan_composite_submit__"),
      QStringLiteral("__render_frame_vulkan_composite_submit__"),
      compositeElapsedMs);
  if (!shouldReadbackToImage) {
    return QImage();
  }
  if (hasTranscriptCandidate && !output.isNull() &&
      vulkanSubtitleDebugEnabled()) {
    const QRectF countBounds =
        transcriptLayerBounds.isValid()
            ? transcriptLayerBounds
            : QRectF(QPointF(0, 0), QSizeF(output.size()));
    const SubtitlePixelCounts counts = countSubtitlePixels(output, countBounds);
    qWarning().noquote()
        << QStringLiteral("[vulkan-subtitle-composite] frame=%1 "
                          "bounds=(%2,%3 %4x%5) pixels_dark=%6 "
                          "pixels_bright=%7 pixels_yellow=%8 pixels_alpha=%9")
               .arg(timelineFrame)
               .arg(countBounds.x(), 0, 'f', 1)
               .arg(countBounds.y(), 0, 'f', 1)
               .arg(countBounds.width(), 0, 'f', 1)
               .arg(countBounds.height(), 0, 'f', 1)
               .arg(counts.dark)
               .arg(counts.bright)
               .arg(counts.yellow)
               .arg(counts.nonTransparent);
    if (vulkanSubtitleDumpEnabled()) {
      const QString path = QDir::temp().filePath(
          QStringLiteral("jcut-vulkan-composited-frame-f%1.png")
              .arg(timelineFrame));
      output.save(path);
      qWarning().noquote()
          << QStringLiteral("[vulkan-subtitle-composite] dumped_frame=\"%1\"")
                 .arg(path);
    }
  }
  return output;
}

bool OffscreenVulkanRenderer::renderFrameToOutput(
    const OffscreenRenderContext &context, OffscreenRenderFrame *output,
    bool readbackToCpuImage) {
  if (!output) {
    return false;
  }
  *output = OffscreenRenderFrame{};
  OffscreenRenderContext frameContext = context;
  frameContext.readbackMs = readbackToCpuImage ? context.readbackMs : nullptr;
  frameContext.frameFailureReason = &output->failureReason;
  output->cpuImage = renderFrame(frameContext);
  if (!output->failureReason.isEmpty()) {
    return false;
  }
  if (!frameContext.externalVulkanOutput) {
    const bool rendered = readbackToCpuImage
        ? !output->cpuImage.isNull()
        : d->hasPendingGpuFrame();
    if (!rendered) {
      output->failureReason =
          QStringLiteral("Vulkan compositor produced no frame.");
    }
    return rendered;
  }
  QString error;
  if (!lastRenderedVulkanFrame(&output->vulkanFrame, &error)) {
    output->vulkanFrame.valid = false;
    output->failureReason = error.isEmpty()
        ? QStringLiteral("Vulkan compositor produced no external frame.")
        : error;
    return false;
  }
  const bool rendered = readbackToCpuImage
      ? !output->cpuImage.isNull()
      : output->vulkanFrame.valid;
  if (!rendered) {
    output->failureReason = readbackToCpuImage
        ? QStringLiteral("Vulkan compositor readback produced no CPU image.")
        : QStringLiteral("Vulkan compositor produced an invalid external frame.");
  }
  return rendered;
}

bool OffscreenVulkanRenderer::convertLastFrameToNv12(AVFrame *frame,
                                                     qint64 *nv12ConvertMs,
                                                     qint64 *readbackMs) {
  return d && d->convertLastFrameToNv12(frame, nv12ConvertMs, readbackMs);
}

bool OffscreenVulkanRenderer::beginLastFrameToNv12Readback(qint64 *convertMs,
                                                           qint64 *readbackMs) {
  return d && d->beginLastFrameToNv12Readback(convertMs, readbackMs);
}

bool OffscreenVulkanRenderer::finishLastFrameToNv12Readback(
    AVFrame *frame, qint64 *convertMs, qint64 *readbackMs) {
  return d && d->finishLastFrameToNv12Readback(frame, convertMs, readbackMs);
}

bool OffscreenVulkanRenderer::beginLastFrameToNv12CudaTransfer(
    qint64 *convertMs, qint64 *transferMs) {
  return d && d->beginLastFrameToNv12CudaTransfer(convertMs, transferMs);
}

bool OffscreenVulkanRenderer::finishLastFrameToNv12CudaTransfer(
    AVFrame *cudaFrame, qint64 *convertMs, qint64 *transferMs) {
  return d &&
         d->finishLastFrameToNv12CudaTransfer(cudaFrame, convertMs, transferMs);
}

bool OffscreenVulkanRenderer::convertLastFrameToYuv420p(AVFrame *frame,
                                                        qint64 *convertMs,
                                                        qint64 *readbackMs) {
  return d && d->convertLastFrameToYuv420p(frame, convertMs, readbackMs);
}

bool OffscreenVulkanRenderer::beginLastFrameToYuv420pReadback(
    qint64 *convertMs, qint64 *readbackMs) {
  return d && d->beginLastFrameToYuv420pReadback(convertMs, readbackMs);
}

bool OffscreenVulkanRenderer::finishLastFrameToYuv420pReadback(
    AVFrame *frame, qint64 *convertMs, qint64 *readbackMs) {
  return d && d->finishLastFrameToYuv420pReadback(frame, convertMs, readbackMs);
}

bool OffscreenVulkanRenderer::copyLastFrameToBgra(AVFrame *frame,
                                                  qint64 *readbackMs) {
  return d && d->copyLastFrameToBgra(frame, readbackMs);
}

bool OffscreenVulkanRenderer::publishLastFrameForGpuPreview(
    OffscreenVulkanFrame *frame, QString *errorMessage) {
  return d && d->publishLastFrameForGpuPreview(frame, errorMessage);
}

void OffscreenVulkanRenderer::finishGpuPreviewPublication() {
  if (!d || !d->hasPendingGpuFrame()) {
    return;
  }
  OffscreenVulkanFrame frame;
  QString error;
  d->finishLastFrameForExternalSampling(&frame, &error);
}

bool OffscreenVulkanRenderer::lastRenderedVulkanFrame(
    OffscreenVulkanFrame *frame, QString *errorMessage) const {
  return d && d->finishLastFrameForExternalSampling(frame, errorMessage);
}

bool OffscreenVulkanRenderer::supportsCudaExternalMemoryInterop() const {
  return d && d->supportsCudaExternalMemoryInterop();
}

bool OffscreenVulkanRenderer::supportsNv12CudaTransfer() const {
  return supportsCudaExternalMemoryInterop();
}

QString OffscreenVulkanRenderer::cudaExternalMemoryStatus() const {
  return d ? d->cudaExternalMemoryStatus() : QStringLiteral("renderer unavailable");
}

QString OffscreenVulkanRenderer::backendId() const {
  return QStringLiteral("vulkan");
}

} // namespace render_detail
