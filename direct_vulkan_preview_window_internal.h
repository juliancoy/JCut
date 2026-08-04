#pragma once

#include <execinfo.h>
#include "background_fill_effect.h"
#include "direct_vulkan_preview_backend.h"
#include "gpu_selection.h"
#include "direct_vulkan_preview_presenter.h"
#include "direct_vulkan_preview_config.h"
#include "direct_vulkan_preview_geometry.h"
#include "direct_vulkan_preview_interaction.h"
#include "direct_vulkan_media_handoff_plan.h"
#include "direct_vulkan_preview_overlay_rendering.h"
#include "direct_vulkan_preview_transcript.h"
#include "direct_vulkan_frame_handoff_pipeline.h"
#include "direct_vulkan_preview_audio.h"
#include "cpu_overlay_render_backend.h"
#include "preview_speaker_profiles.h"
#include "presentation_miss_tracker.h"
#include "preview_view_transform.h"
#include "render_vulkan_shared.h"
#include "editor_shared.h"
#include "render_internal.h"
#include "titles.h"
#include "vulkan_audio_tab.h"
#include "vulkan_clear_helpers.h"
#include "vulkan_pipeline.h"
#include "vulkan_resources.h"
#include "vulkan_external_frame_import_core.h"
#include "vulkan_text_renderer.h"
#include "waveform_service.h"
#include "loiacono/loiacono_rolling.h"

#include <QDebug>
#include <QByteArray>
#include <QCoreApplication>
#include <QDateTime>
#include <QElapsedTimer>
#include <QHash>
#include <QImage>
#include <QVector>
#include <QSet>
#include <QApplication>
#include <QExposeEvent>
#include <QContextMenuEvent>
#include <QCryptographicHash>
#include <QCursor>
#include <QMenu>
#include <QKeyEvent>
#include <QMouseEvent>
#include <QWheelEvent>
#include <QTransform>
#include <QVulkanFunctions>
#include <QWindow>

namespace {

QSize toQSize(const jcut::core::SizeI& size)
{
    return QSize(size.width, size.height);
}

} // namespace
#include <QWidget>

#include <vulkan/vulkan.h>

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <functional>
#include <memory>
#include <cmath>
#include <utility>
#include <vector>
#include <unistd.h>

namespace {
constexpr qint64 kPipelineThumbnailReadbackMinIntervalMs = 250;
constexpr bool kAllowCpuRasterTextOverlaysInDirectVulkanPreview = false;
constexpr std::uint64_t kPreviewGpuWaitTimeoutNs = 1'000'000'000ull;

using namespace jcut::direct_vulkan_preview;

VkClearValue guideClearValue(float r, float g, float b, float a = 1.0f)
{
    VkClearValue value{};
    value.color.float32[0] = r;
    value.color.float32[1] = g;
    value.color.float32[2] = b;
    value.color.float32[3] = a;
    return value;
}

void drawOutputPlacementGuides(QVulkanDeviceFunctions* funcs,
                               VkCommandBuffer commandBuffer,
                               const QSize& swapSize,
                               const QSize& outputSize,
                               const QRectF& compositeRect,
                               bool instagramSafeAreaGuides,
                               bool alignmentGridGuides)
{
    if (!funcs || commandBuffer == VK_NULL_HANDLE || compositeRect.isEmpty() ||
        (!instagramSafeAreaGuides && !alignmentGridGuides)) {
        return;
    }

    const qreal linePx = qMax<qreal>(
        1.0, qMin(compositeRect.width(), compositeRect.height()) / 540.0);
    auto drawLineRect = [&](const QRectF& rect, const VkClearValue& value) {
        const QRectF bounded = rect.intersected(compositeRect);
        if (bounded.width() <= 0.0 || bounded.height() <= 0.0) {
            return;
        }
        clearRect(funcs, commandBuffer, value, clearRectFromQRect(bounded, swapSize));
    };

    if (alignmentGridGuides) {
        const VkClearValue grid = guideClearValue(0.50f, 0.82f, 1.00f);
        for (int i = 1; i <= 2; ++i) {
            const qreal fraction = static_cast<qreal>(i) / 3.0;
            const qreal x = compositeRect.left() + compositeRect.width() * fraction;
            const qreal y = compositeRect.top() + compositeRect.height() * fraction;
            drawLineRect(QRectF(x - linePx * 0.5, compositeRect.top(), linePx, compositeRect.height()), grid);
            drawLineRect(QRectF(compositeRect.left(), y - linePx * 0.5, compositeRect.width(), linePx), grid);
        }
    }

    if (instagramSafeAreaGuides) {
        const int safeHeight = qMax(1, outputSize.height());
        const qreal safeInset = qMin<qreal>(250.0, static_cast<qreal>(safeHeight) * 0.5);
        const qreal topY = compositeRect.top() + compositeRect.height() * (safeInset / safeHeight);
        const qreal bottomY = compositeRect.bottom() - compositeRect.height() * (safeInset / safeHeight);
        const VkClearValue safe = guideClearValue(1.00f, 0.84f, 0.25f);
        drawLineRect(QRectF(compositeRect.left(), topY - linePx, compositeRect.width(), linePx * 2.0), safe);
        drawLineRect(QRectF(compositeRect.left(), bottomY - linePx, compositeRect.width(), linePx * 2.0), safe);
    }
}

bool computeVulkanVisualResizeTransform(const PreviewInteractionTransientState& transient,
                                        PreviewDragMode dragMode,
                                        const QPointF& surfacePosition,
                                        const QPointF& previewScale,
                                        bool clipPixelSizeIsKnown,
                                        TimelineClip::TransformKeyframe* transformOut)
{
    if (!transformOut ||
        (dragMode != PreviewDragMode::ResizeX &&
         dragMode != PreviewDragMode::ResizeY &&
         dragMode != PreviewDragMode::ResizeBoth) ||
        transient.dragOriginBounds.width() <= 1.0 ||
        transient.dragOriginBounds.height() <= 1.0) {
        return false;
    }

    qreal scaleX = transient.dragOriginTransform.scaleX;
    qreal scaleY = transient.dragOriginTransform.scaleY;
    if (dragMode == PreviewDragMode::ResizeX ||
        dragMode == PreviewDragMode::ResizeBoth) {
        const qreal factorX =
            (transient.dragOriginBounds.width() +
             (surfacePosition.x() - transient.dragOriginPos.x())) /
            transient.dragOriginBounds.width();
        scaleX = sanitizeScaleValue(transient.dragOriginTransform.scaleX * factorX);
    }
    if (dragMode == PreviewDragMode::ResizeY ||
        dragMode == PreviewDragMode::ResizeBoth) {
        const qreal factorY =
            (transient.dragOriginBounds.height() +
             (surfacePosition.y() - transient.dragOriginPos.y())) /
            transient.dragOriginBounds.height();
        scaleY = sanitizeScaleValue(transient.dragOriginTransform.scaleY * factorY);
    }
    if (dragMode == PreviewDragMode::ResizeBoth) {
        const qreal factorX =
            (transient.dragOriginBounds.width() +
             (surfacePosition.x() - transient.dragOriginPos.x())) /
            transient.dragOriginBounds.width();
        const qreal factorY =
            (transient.dragOriginBounds.height() +
             (surfacePosition.y() - transient.dragOriginPos.y())) /
            transient.dragOriginBounds.height();
        const qreal uniformFactor =
            std::abs(factorX) >= std::abs(factorY) ? factorX : factorY;
        scaleX = sanitizeScaleValue(transient.dragOriginTransform.scaleX * uniformFactor);
        scaleY = sanitizeScaleValue(transient.dragOriginTransform.scaleY * uniformFactor);
    }

    const QPointF translation = PreviewViewTransform::translationForAnchoredResize(
        QPointF(transient.dragOriginTransform.translationX, transient.dragOriginTransform.translationY),
        QPointF(transient.dragOriginTransform.scaleX, transient.dragOriginTransform.scaleY),
        QPointF(scaleX, scaleY),
        transient.dragOriginBounds,
        (dragMode == PreviewDragMode::ResizeX
             ? PreviewResizeAnchor::Left
             : (dragMode == PreviewDragMode::ResizeY
                    ? PreviewResizeAnchor::Top
                    : PreviewResizeAnchor::TopLeft)),
        clipPixelSizeIsKnown ? previewScale : QPointF(1.0, 1.0));

    TimelineClip::TransformKeyframe transform = transient.dragOriginTransform;
    transform.scaleX = scaleX;
    transform.scaleY = scaleY;
    transform.translationX = translation.x();
    transform.translationY = translation.y();
    *transformOut = transform;
    return true;
}


} // namespace

inline constexpr std::size_t kGpuExportPreviewSlotCount = 3;

class DirectVulkanPreviewRenderer final {
public:
    DirectVulkanPreviewRenderer(DirectVulkanPreviewWindow* owner,
                                DirectVulkanPreviewWindow* window)
        : m_owner(owner), m_window(window) {}
    ~DirectVulkanPreviewRenderer();

    void initResources();
    void releaseResources();
    void releaseDeviceResources();
    void startNextFrame();
    void physicalDeviceLost();
    void logicalDeviceLost();
    void clearGpuExportPreview();

private:
    struct ReadbackSlot {
        VkBuffer buffer = VK_NULL_HANDLE;
        VkDeviceMemory memory = VK_NULL_HANDLE;
        VkDeviceSize size = 0;
        QSize imageSize;
        VkFormat format = VK_FORMAT_UNDEFINED;
        bool pending = false;
    };

    void destroyReadbackSlots();
    bool ensureReadbackSlot(ReadbackSlot* slot, const QSize& size, VkFormat format);
    void consumeReadbackSlot(ReadbackSlot* slot);
    void consumeDecoderReadbackSlot(ReadbackSlot* slot);
    void recordSwapchainReadback(VkCommandBuffer cb, ReadbackSlot* slot, const QSize& swapSize);
    void recordImageReadback(VkCommandBuffer cb,
                             ReadbackSlot* slot,
                             VkImage image,
                             VkImageLayout layout,
                             const QSize& size,
                             VkFormat format);
    uint32_t findMemoryType(uint32_t typeBits, VkMemoryPropertyFlags properties) const;
    QImage imageFromReadback(const uchar* bytes, const QSize& size, VkFormat format) const;
    bool ensureCompositeTarget(const QSize& size, VkFormat colorFormat, VkFormat depthFormat);
    void destroyCompositeTarget();
    struct ClipHandoffResources {
        std::unique_ptr<VulkanResources> resources;
        std::unique_ptr<DirectVulkanFrameHandoffPipeline> pipeline;
    };
    struct RetiredClipHandoffResources {
        QString clipId;
        std::shared_ptr<ClipHandoffResources> resources;
        int framesRemaining = 0;
    };
    ClipHandoffResources* ensureClipHandoffResources(const QString& clipId);
    void pruneClipHandoffResources(const QSet<QString>& activeClipIds);
    void advanceRetiredClipHandoffResources();
    void releaseClipHandoffResources(const std::shared_ptr<ClipHandoffResources>& resources);
    void updateClipHandoffResourceStats();
    bool renderGpuExportPreview(VkCommandBuffer commandBuffer);
    void beginGpuExportPreviewRenderPass(
        VkCommandBuffer commandBuffer,
        const VkRenderPassBeginInfo& renderPass);
    void destroyGpuExportPreviewResources();
    struct GpuExportPreviewSlot {
        std::unique_ptr<jcut::vulkan_import::VulkanExternalFrameImportCore> importer;
        VkSemaphore ready = VK_NULL_HANDLE;
        VkSemaphore consumed = VK_NULL_HANDLE;
        std::uint64_t producerSessionId = 0;
        quint64 generation = 0;
        bool initialized = false;
    };

    DirectVulkanPreviewWindow* m_owner = nullptr;
    DirectVulkanPreviewWindow* m_window = nullptr;
    QVulkanDeviceFunctions* m_devFuncs = nullptr;
    std::unique_ptr<VulkanResources> m_resources;
    std::unique_ptr<VulkanResources> m_playbackStatusOverlayResources;
    std::unique_ptr<VulkanPipeline> m_pipeline;
    std::unique_ptr<VulkanTextRenderer> m_textRenderer;
    std::unique_ptr<VulkanTextRenderer> m_speakerTextRenderer;
    std::unique_ptr<VulkanTextRenderer> m_temporalDebugTextRenderer;
    std::unique_ptr<jcut::VulkanAudioTab> m_audioTab;
    QHash<QString, std::shared_ptr<ClipHandoffResources>> m_clipHandoffResources;
    QVector<RetiredClipHandoffResources> m_retiredClipHandoffResources;
    QString m_playbackStatusOverlayTextureKey;
    QString m_lastPreparedTextKey;
    bool m_lastPreparedTextReady = false;
    bool m_playbackStatusOverlayTextureReady = false;
    std::vector<ReadbackSlot> m_readbackSlots;
    std::vector<ReadbackSlot> m_decoderReadbackSlots;
    VkRenderPass m_compositeRenderPass = VK_NULL_HANDLE;
    VkFramebuffer m_compositeFramebuffer = VK_NULL_HANDLE;
    VkImage m_compositeImage = VK_NULL_HANDLE;
    VkDeviceMemory m_compositeMemory = VK_NULL_HANDLE;
    VkImageView m_compositeView = VK_NULL_HANDLE;
    VkImage m_compositeDepthImage = VK_NULL_HANDLE;
    VkDeviceMemory m_compositeDepthMemory = VK_NULL_HANDLE;
    VkImageView m_compositeDepthView = VK_NULL_HANDLE;
    QSize m_compositeSize;
    VkFormat m_compositeColorFormat = VK_FORMAT_UNDEFINED;
    VkFormat m_compositeDepthFormat = VK_FORMAT_UNDEFINED;
    std::array<GpuExportPreviewSlot, kGpuExportPreviewSlotCount>
        m_gpuExportPreviewSlots;
    int m_gpuExportPreviewCurrentSlot = -1;
    std::shared_ptr<render_detail::OffscreenVulkanFrameConsumptionState>
        m_pendingGpuExportPreviewConsumptionState;
    quint64 m_pendingGpuExportPreviewGeneration = 0;
    int m_pendingGpuExportPreviewSlot = -1;
    std::uint64_t m_pendingGpuExportPreviewProducerSessionId = 0;
    std::uint64_t m_gpuExportPreviewProducerSessionId = 0;
    std::uint64_t m_lastAcceptedGpuExportPreviewSequence = 0;
    PFN_vkImportSemaphoreFdKHR m_importSemaphoreFd = nullptr;
};

class DirectVulkanPreviewWindow final : public QWindow {
public:
    DirectVulkanPreviewWindow(PreviewInteractionState* state,
                              DirectVulkanPresentationTelemetry* presentationTelemetry,
                              DirectVulkanPreviewStats* stats,
                              bool* active,
                              QString* failureReason,
                              bool enableAudioPipeline,
                              std::function<void(const QString&)> failureCallback = {})
        : m_state(state),
          m_presentationTelemetry(presentationTelemetry),
          m_stats(stats),
          m_active(active),
          m_failureReason(failureReason),
          m_enableAudioPipeline(enableAudioPipeline),
          m_failureCallback(std::move(failureCallback))
    {
        setSurfaceType(QSurface::VulkanSurface);
        setTitle(QStringLiteral("JCut Direct Vulkan Preview"));
    }

    ~DirectVulkanPreviewWindow() override;

    void setPreferredPhysicalDeviceIndex(int index)
    {
        m_preferredPhysicalDeviceIndex = index;
    }

    QVulkanInfoVector<QVulkanExtension> supportedDeviceExtensions() const;

    void setDeviceExtensions(const QByteArrayList& extensions)
    {
        m_requestedDeviceExtensions = extensions;
    }

    bool isValid() const
    {
        return !m_failureLatched &&
               m_device != VK_NULL_HANDLE &&
               m_swapchain != VK_NULL_HANDLE;
    }

    VkPhysicalDevice physicalDevice() const { return m_physicalDevice; }
    VkDevice device() const { return m_device; }
    const VkPhysicalDeviceProperties* physicalDeviceProperties() const
    {
        return m_hasPhysicalDeviceProperties ? &m_physicalDeviceProperties : nullptr;
    }
    VkRenderPass defaultRenderPass() const { return m_defaultRenderPass; }
    VkQueue graphicsQueue() const { return m_graphicsQueue; }
    uint32_t graphicsQueueFamilyIndex() const { return m_graphicsQueueFamilyIndex; }
    int concurrentFrameCount() const
    {
        return static_cast<int>(m_frames.size());
    }
    int currentFrame() const { return m_currentFrameSlot; }
    int currentSwapChainImageIndex() const { return m_currentSwapchainImageIndex; }
    VkImage swapChainImage(int index) const
    {
        return index >= 0 &&
                       index < static_cast<int>(m_swapchainImages.size())
                   ? m_swapchainImages[static_cast<size_t>(index)]
                   : VK_NULL_HANDLE;
    }
    QSize swapChainImageSize() const { return m_swapchainPixelSize; }
    VkFramebuffer currentFramebuffer() const { return m_currentFramebuffer; }
    VkCommandBuffer currentCommandBuffer() const { return m_currentCommandBuffer; }
    VkFormat depthStencilFormat() const { return m_depthStencilFormat; }
    VkFormat colorFormat() const { return m_colorFormat; }
    bool audioPipelineEnabled() const { return m_enableAudioPipeline; }
    bool frameReady();
    void signalSemaphoreWhenFrameCompletes(VkSemaphore semaphore)
    {
        m_frameCompletionSemaphore = semaphore;
    }

    void setInteractionCallbacks(std::function<void(const QString&)> selectionRequested,
                                 std::function<void(const QString&, qreal, qreal, bool)> moveRequested,
                                 std::function<void(const QString&, qreal, qreal, qreal, qreal, bool)> transformRequested = {},
                                 std::function<void(int64_t)> playbackSampleRequested = {},
                                 std::function<void(const QString&, qreal, qreal)> correctionPointRequested = {},
                                 std::function<void(const QString&, int64_t, int64_t, qreal, qreal)> maskFuzzyRemovePointRequested = {},
                                 std::function<void(const QString&, qreal, qreal)> speakerPointRequested = {},
                                 std::function<void(const QString&, qreal, qreal, qreal)> speakerBoxRequested = {},
                                 std::function<void(const QString&, int, const QString&, int64_t, qreal, qreal, qreal)> faceStreamBoxRequested = {},
                                 std::function<void(const QString&, int, const QString&, int64_t, qreal, qreal, qreal)> faceStreamBoxFocusClearRequested = {},
                                 std::function<void(const QString&)> faceStreamBoxClickStatus = {},
                                 std::function<void(const QString&)> createKeyframeRequested = {})
    {
        m_selectionRequested = std::move(selectionRequested);
        m_moveRequested = std::move(moveRequested);
        m_transformRequested = std::move(transformRequested);
        m_playbackSampleRequested = std::move(playbackSampleRequested);
        m_correctionPointRequested = std::move(correctionPointRequested);
        m_maskFuzzyRemovePointRequested = std::move(maskFuzzyRemovePointRequested);
        m_speakerPointRequested = std::move(speakerPointRequested);
        m_speakerBoxRequested = std::move(speakerBoxRequested);
        m_faceStreamBoxRequested = std::move(faceStreamBoxRequested);
        m_faceStreamBoxFocusClearRequested = std::move(faceStreamBoxFocusClearRequested);
        m_faceStreamBoxClickStatus = std::move(faceStreamBoxClickStatus);
        m_createKeyframeRequested = std::move(createKeyframeRequested);
    }

    void setGpuExportPreviewFrame(
        const render_detail::OffscreenVulkanFrame& frame)
    {
        if (!frame.valid ||
            frame.bufferIndex >= kGpuExportPreviewSlotCount) {
            if (frame.readySemaphoreFd >= 0) {
                ::close(frame.readySemaphoreFd);
            }
            if (frame.consumedSemaphoreFd >= 0) {
                ::close(frame.consumedSemaphoreFd);
            }
            return;
        }
        m_gpuExportPreviewFrames.push_back(frame);
        schedulePreviewUpdate();
    }

    bool takeGpuExportPreviewFrame(
        render_detail::OffscreenVulkanFrame* frame)
    {
        if (!frame || m_gpuExportPreviewFrames.isEmpty()) {
            return false;
        }
        *frame = m_gpuExportPreviewFrames.takeFirst();
        return true;
    }

    bool hasGpuExportPreviewFrames() const
    {
        return !m_gpuExportPreviewFrames.isEmpty();
    }

    void clearGpuExportPreview()
    {
        if (m_renderer) {
            m_renderer->clearGpuExportPreview();
        }
        for (const render_detail::OffscreenVulkanFrame& frame :
             std::as_const(m_gpuExportPreviewFrames)) {
            if (frame.readySemaphoreFd >= 0) {
                ::close(frame.readySemaphoreFd);
            }
            if (frame.consumedSemaphoreFd >= 0) {
                ::close(frame.consumedSemaphoreFd);
            }
        }
        m_gpuExportPreviewFrames.clear();
        schedulePreviewUpdate();
    }

    void schedulePreviewUpdate()
    {
        m_updateDirty = true;
        if (!isExposed()) {
            if (!m_updateDeferredWhileNotExposed && m_stats) {
                ++m_stats->previewUpdatesDeferredWhileNotExposed;
            }
            m_updateDeferredWhileNotExposed = true;
            return;
        }
        m_updateDeferredWhileNotExposed = false;
        if (m_frameInProgress) {
            return;
        }
        if (m_updateRequestPosted) {
            return;
        }
        m_updateRequestPosted = true;
        m_updateRequestMs = QDateTime::currentMSecsSinceEpoch();
        if (m_presentationTelemetry) {
            m_presentationTelemetry->previewUpdateRequests.fetch_add(
                1, std::memory_order_relaxed);
        }
        // Diagnostic: JCUT_DEBUG_UPDATE_STORM=1 dumps who keeps re-arming
        // preview updates (first 40 requests). Off by default.
        if (qEnvironmentVariableIsSet("JCUT_DEBUG_UPDATE_STORM")) {
            static int dumped = 0;
            if (dumped < 40) {
                ++dumped;
                void* frames[16];
                const int n = ::backtrace(frames, 16);
                char** syms = ::backtrace_symbols(frames, n);
                fprintf(stderr, "[update-storm] request #%d\n", dumped);
                if (syms) {
                    for (int i = 2; i < n && i < 12; ++i) {
                        fprintf(stderr, "    %s\n", syms[i]);
                    }
                    free(syms);
                }
            }
        }
        QCoreApplication::postEvent(this, new QEvent(QEvent::UpdateRequest));
    }

    bool updatePending() const
    {
        return m_updateRequestMs >= 0 ||
               m_frameInProgress ||
               m_updateDirty ||
               m_swapchainDirty;
    }

    bool ensureVulkanReady();
    bool ensureSwapchain();
    void cleanupSwapchain();
    void cleanupDevice();
    void renderNow();
    void markSwapchainDirty();
    QSize swapchainPixelSizeForWindow() const;
    int selectGraphicsPresentQueueFamily(VkPhysicalDevice device);
    void refreshPhysicalDeviceList();

protected:
    void exposeEvent(QExposeEvent* event) override
    {
        QWindow::exposeEvent(event);
        if (!isExposed()) {
            m_scheduledWhileExposed = false;
            const bool hadOutstandingUpdate =
                m_updateRequestMs >= 0 ||
                m_frameInProgress ||
                m_updateDirty;
            if (hadOutstandingUpdate && m_stats) {
                ++m_stats->previewUpdatesDiscardedWhileNotExposed;
            }
            m_updateDirty = hadOutstandingUpdate;
            m_updateRequestMs = -1;
            m_updateDeferredWhileNotExposed = hadOutstandingUpdate;
            // Do not destroy the swapchain from Qt's hide/unexpose path.
            // QWindowContainer can hide the embedded native Vulkan window while
            // its platform surface is already mid-teardown; destroying the
            // swapchain from that callback has been observed to crash inside
            // NVIDIA's driver. Mark dirty and rebuild on the next expose, or
            // retire resources from cleanupDevice() during actual destruction.
            m_swapchainDirty = true;
            return;
        }
        if (m_active) {
            *m_active = true;
            if (!m_scheduledWhileExposed || size() != m_lastExposeScheduledSize) {
                m_scheduledWhileExposed = true;
                m_lastExposeScheduledSize = size();
                markSwapchainDirty();
                schedulePreviewUpdate();
            }
        }
    }

    void resizeEvent(QResizeEvent* event) override
    {
        QWindow::resizeEvent(event);
        markSwapchainDirty();
        schedulePreviewUpdate();
    }

    void showEvent(QShowEvent* event) override
    {
        QWindow::showEvent(event);
        markSwapchainDirty();
        schedulePreviewUpdate();
    }

    void hideEvent(QHideEvent* event) override
    {
        QWindow::hideEvent(event);
        m_scheduledWhileExposed = false;
        m_updateRequestMs = -1;
        m_updateDirty = false;
        m_updateDeferredWhileNotExposed = false;
        // See exposeEvent(!isExposed): hide can be delivered during
        // QWindowContainer/native-surface teardown, so defer Vulkan swapchain
        // destruction until the next exposed rebuild or device cleanup.
        m_swapchainDirty = true;
    }

    void wheelEvent(QWheelEvent* event) override
    {
        if (!event || !m_state || event->angleDelta().y() == 0) {
            QWindow::wheelEvent(event);
            return;
        }
        const QRectF surfaceRect = PreviewViewTransform::rectForWindow(
            this, PreviewSurfaceCoordinateSpace::DeviceSurface);
        const QPointF surfacePosition = PreviewViewTransform::pointForWindowPoint(
            this, event->position(), PreviewSurfaceCoordinateSpace::DeviceSurface);
        if (m_state->viewMode == PreviewSurface::ViewMode::Audio) {
            if (applyAudioPreviewWheelZoom(m_state, surfaceRect, surfacePosition, event->angleDelta().y())) {
                schedulePreviewUpdate();
                event->accept();
                return;
            }
            QWindow::wheelEvent(event);
            return;
        }
        if (m_state->viewMode == PreviewSurface::ViewMode::Audio) {
            QWindow::wheelEvent(event);
            return;
        }
        if (applyVideoPreviewWheelZoom(m_state, surfaceRect, surfacePosition, event->angleDelta().y())) {
            schedulePreviewUpdate();
            event->accept();
            return;
        }
        QWindow::wheelEvent(event);
    }

    void mousePressEvent(QMouseEvent* event) override
    {
        if (!event || !m_state) {
            QWindow::mousePressEvent(event);
            return;
        }
        const QRectF surfaceRect = PreviewViewTransform::rectForWindow(
            this, PreviewSurfaceCoordinateSpace::DeviceSurface);
        const QPointF surfacePosition = PreviewViewTransform::pointForWindowPoint(
            this, event->position(), PreviewSurfaceCoordinateSpace::DeviceSurface);
        m_state->transient.lastMousePos = surfacePosition;
        if (event->button() == Qt::RightButton && m_faceStreamBoxFocusClearRequested) {
            const VulkanInteractionOverlayInfos infos = collectVulkanInteractionInfos(m_state, surfaceRect);
            if (dispatchFaceDetectionsFocusClearAtPosition(
                    m_state,
                    infos,
                    surfacePosition,
                    m_faceStreamBoxFocusClearRequested,
                    m_faceStreamBoxClickStatus)) {
                m_state->transient.faceDetectionsRightClickHandled = true;
                schedulePreviewUpdate();
                event->accept();
                return;
            }
        }
        if (event->button() != Qt::LeftButton) {
            QWindow::mousePressEvent(event);
            return;
        }
        if (m_state->viewMode == PreviewSurface::ViewMode::Audio) {
            if (m_state->viewMode == PreviewSurface::ViewMode::Audio && m_playbackSampleRequested) {
                int64_t targetSample = 0;
                if (audioSeekSampleAtSurfacePosition(*m_state, surfaceRect, surfacePosition, &targetSample)) {
                    m_playbackSampleRequested(targetSample);
                    event->accept();
                    return;
                }
            }
            QWindow::mousePressEvent(event);
            return;
        }
        const VulkanInteractionOverlayInfos infos = collectVulkanInteractionInfos(m_state, surfaceRect);

        PreviewInteractionTransientState& transient = m_state->transient;
        VulkanInteractionOverlayInfo selectedInfo;
        transient.dragMode = PreviewDragMode::None;

        if (m_state->correctionDrawMode) {
            QString hitClipId = m_state->selectedClipId;
            if (!hitClipId.isEmpty()) {
                if (lookupVulkanInteractionInfo(infos, hitClipId, &selectedInfo) &&
                    !selectedInfo.bounds.contains(surfacePosition)) {
                    hitClipId.clear();
                }
            }
            if (hitClipId.isEmpty()) {
                hitClipId = clipIdAtPositionForVulkan(infos, surfacePosition);
            }
            if (!hitClipId.isEmpty() &&
                lookupVulkanInteractionInfo(infos, hitClipId, &selectedInfo) &&
                selectedInfo.bounds.isValid() &&
                selectedInfo.bounds.width() > 1.0 &&
                selectedInfo.bounds.height() > 1.0) {
                if (m_state->selectedClipId != hitClipId) {
                    m_state->selectedClipId = hitClipId;
                    if (m_selectionRequested) {
                        m_selectionRequested(hitClipId);
                    }
                }
                const QPointF normalized = mapScreenPointToNormalizedClipForVulkan(
                    selectedInfo, surfacePosition);
                if (m_correctionPointRequested) {
                    m_correctionPointRequested(hitClipId, normalized.x(), normalized.y());
                }
            }
            schedulePreviewUpdate();
            event->accept();
            return;
        }

        if (m_state->maskFuzzyRemoveMode) {
            QString hitClipId = m_state->selectedClipId;
            if (!lookupVulkanInteractionInfo(infos, hitClipId, &selectedInfo) ||
                !selectedInfo.bounds.contains(surfacePosition)) {
                hitClipId.clear();
            }
            if (!hitClipId.isEmpty() && m_maskFuzzyRemovePointRequested) {
                const QPointF normalized = mapScreenPointToNormalizedClipForVulkan(
                    selectedInfo, surfacePosition);
                const VulkanPreviewClipFrameStatus* status =
                    frameStatusForClip(m_state, hitClipId);
                if (status && status->frame) {
                    m_maskFuzzyRemovePointRequested(
                        hitClipId,
                        status->presentedSourceFrame,
                        status->frame.sourcePresentationTimestamp(),
                        normalized.x(),
                        normalized.y());
                }
            }
            schedulePreviewUpdate();
            event->accept();
            return;
        }

        if ((event->modifiers() & Qt::ShiftModifier) &&
            (m_speakerPointRequested || m_speakerBoxRequested)) {
            QString hitClipId;
        if (!m_state->selectedClipId.isEmpty()) {
            const QString selectedInfoClipId = m_state->selectedClipId;
            if (selectedInfoClipId == clipIdAtPositionForVulkan(infos, surfacePosition) &&
                lookupVulkanInteractionInfo(infos, selectedInfoClipId, &selectedInfo) &&
                selectedInfo.bounds.isValid() &&
                selectedInfo.bounds.width() > 1.0 &&
                selectedInfo.bounds.height() > 1.0) {
                hitClipId = selectedInfoClipId;
            }
        }
            if (hitClipId.isEmpty()) {
                hitClipId = clipIdAtPositionForVulkan(infos, surfacePosition);
            }
            if (!hitClipId.isEmpty() &&
                lookupVulkanInteractionInfo(infos, hitClipId, &selectedInfo) &&
                selectedInfo.bounds.isValid() &&
                selectedInfo.bounds.width() > 1.0 &&
                selectedInfo.bounds.height() > 1.0) {
                transient.speakerPickDragActive = true;
                transient.speakerPickClipId = hitClipId;
                transient.speakerPickStartPos = surfacePosition;
                transient.speakerPickCurrentPos = surfacePosition;
                if (m_state->selectedClipId != hitClipId) {
                    m_state->selectedClipId = hitClipId;
                    if (m_selectionRequested) {
                        m_selectionRequested(hitClipId);
                    }
                }
                schedulePreviewUpdate();
                event->accept();
                return;
            }
        }

        if (m_faceStreamBoxRequested &&
            dispatchFaceDetectionsBoxAtPosition(
                m_state, infos, surfacePosition, m_faceStreamBoxRequested, m_faceStreamBoxClickStatus)) {
            schedulePreviewUpdate();
            event->accept();
            return;
        }
        if (m_state->faceStreamAssignmentInteractionEnabled) {
            schedulePreviewUpdate();
            event->accept();
            return;
        }

        const bool selectedClipAllowedForInteraction = !m_state->titleOverlayInteractionOnly ||
                                                     clipIdIsTitleForVulkan(m_state, m_state->selectedClipId);
        const bool allowSelectedClipDrag =
            !m_state->selectedClipId.isEmpty() && selectedClipAllowedForInteraction;
        if (!m_state->selectedClipId.isEmpty()) {
            if (lookupVulkanInteractionInfo(infos, m_state->selectedClipId, &selectedInfo)) {
                const bool selectedInfoInteractive =
                    selectedInfo.kind != PreviewOverlayKind::TranscriptOverlay ||
                    m_state->transcriptOverlayInteractionEnabled;
                if (!selectedInfoInteractive) {
                    m_state->transient.dragMode = PreviewDragMode::None;
                } else if (selectedInfo.cornerHandle.contains(surfacePosition)) {
                    transient.dragMode = PreviewDragMode::ResizeBoth;
                } else if (selectedInfo.rightHandle.contains(surfacePosition)) {
                    transient.dragMode = PreviewDragMode::ResizeX;
                } else if (selectedInfo.bottomHandle.contains(surfacePosition)) {
                    transient.dragMode = PreviewDragMode::ResizeY;
                } else if (selectedInfo.bounds.contains(surfacePosition)) {
                    transient.dragMode = PreviewDragMode::Move;
                }
                if (allowSelectedClipDrag && transient.dragMode != PreviewDragMode::None) {
                    const auto originTransform = currentTransformForVulkanClip(m_state, m_state->selectedClipId);
                    transient.dragOriginPos = surfacePosition;
                    transient.dragOriginTransform = originTransform;
                    transient.dragOriginBounds = selectedInfo.bounds;
                    transient.dragOriginTranscriptTranslation = QPointF();
                    transient.transformOverrideActive = false;
                    transient.transformOverrideClipId.clear();
                    transient.transcriptOverrideActive = false;
                    transient.transcriptOverrideClipId.clear();
                    if (selectedInfo.kind == PreviewOverlayKind::TranscriptOverlay) {
                        const TimelineClip* selectedClip = clipForId(m_state, m_state->selectedClipId);
                        if (selectedClip) {
                            transient.dragOriginTranscriptTranslation =
                                QPointF(selectedClip->transcriptOverlay.placement.translationX,
                                        selectedClip->transcriptOverlay.placement.translationY);
                            transient.transcriptSizeOverride =
                                QSizeF(selectedClip->transcriptOverlay.boxWidth,
                                       selectedClip->transcriptOverlay.boxHeight);
                        }
                    }
                    updatePreviewCursor(surfacePosition);
                    event->accept();
                    return;
                }
            }
        }

        const QString hitClipId =
            m_state->titleOverlayInteractionOnly && !clipIdIsTitleForVulkan(m_state, clipIdAtPositionForVulkan(infos, surfacePosition))
                ? QString()
                : clipIdAtPositionForVulkan(infos, surfacePosition);
        if (!hitClipId.isEmpty()) {
            if (m_state->selectedClipId != hitClipId) {
                m_state->selectedClipId = hitClipId;
                if (m_selectionRequested) {
                    m_selectionRequested(hitClipId);
                }
            }
            // A title should be draggable with the same gesture that selects it.
            // Previously this branch returned immediately after selection, so an
            // unselected title required a click followed by a second click-drag.
            VulkanInteractionOverlayInfo hitInfo;
            if (clipIdIsTitleForVulkan(m_state, hitClipId) &&
                lookupVulkanInteractionInfo(infos, hitClipId, &hitInfo) &&
                hitInfo.bounds.contains(surfacePosition)) {
                transient.dragMode = PreviewDragMode::Move;
                transient.dragOriginPos = surfacePosition;
                transient.dragOriginTransform =
                    currentTransformForVulkanClip(m_state, hitClipId);
                transient.dragOriginBounds = hitInfo.bounds;
                transient.dragOriginTranscriptTranslation = QPointF();
                transient.transformOverrideActive = false;
                transient.transformOverrideClipId.clear();
                transient.transcriptOverrideActive = false;
                transient.transcriptOverrideClipId.clear();
            }
            schedulePreviewUpdate();
            updatePreviewCursor(surfacePosition);
            event->accept();
            return;
        }

        transient.dragMode = PreviewDragMode::None;
        transient.dragOriginBounds = QRectF();
        QWindow::mousePressEvent(event);
    }

    void mouseMoveEvent(QMouseEvent* event) override
    {
        if (!event || !m_state) {
            QWindow::mouseMoveEvent(event);
            return;
        }
        const QPointF surfacePosition = PreviewViewTransform::pointForWindowPoint(
            this, event->position(), PreviewSurfaceCoordinateSpace::DeviceSurface);
        m_state->transient.lastMousePos = surfacePosition;
        if (!(event->buttons() & Qt::LeftButton) || m_state->transient.dragMode == PreviewDragMode::None ||
            m_state->selectedClipId.isEmpty()) {
            updatePreviewCursor(surfacePosition);
            schedulePreviewUpdate();
            QWindow::mouseMoveEvent(event);
            return;
        }

        const QString& clipId = m_state->selectedClipId;
        const QRectF surfaceRect = PreviewViewTransform::rectForWindow(
            this, PreviewSurfaceCoordinateSpace::DeviceSurface);
        const PreviewViewTransform viewTransform(
            surfaceRect,
            m_state->outputSize,
            vulkanPreviewCanvasMarginPx(),
            m_state->previewZoom,
            m_state->previewPanOffset);
        const VulkanInteractionOverlayInfos infos = collectVulkanInteractionInfos(m_state, surfaceRect);
        VulkanInteractionOverlayInfo activeInfo;
        if (clipId.isEmpty() || !lookupVulkanInteractionInfo(infos, clipId, &activeInfo) ||
            activeInfo.bounds.width() <= 1.0 ||
            activeInfo.bounds.height() <= 1.0) {
            QWindow::mouseMoveEvent(event);
            return;
        }

        const PreviewInteractionTransientState& transient = m_state->transient;
        const QPointF previewScale = viewTransform.outputScale();
        const QPointF safeScale(
            qMax<qreal>(0.0001, previewScale.x()),
            qMax<qreal>(0.0001, previewScale.y()));

        if (m_state->transient.dragMode == PreviewDragMode::Move) {
            if (m_state->titleOverlayInteractionOnly && !clipIdIsTitleForVulkan(m_state, clipId)) {
                m_state->transient.dragMode = PreviewDragMode::None;
                QWindow::mouseMoveEvent(event);
                return;
            }
            if (activeInfo.kind == PreviewOverlayKind::TranscriptOverlay) {
                const QSizeF safeOutputSize = m_state->outputSize.isValid()
                                                ? m_state->outputSize
                                                : QSize(1080, 1920);
                const qreal halfOutputWidth = qMax<qreal>(1.0, safeOutputSize.width() * 0.5);
                const qreal halfOutputHeight = qMax<qreal>(1.0, safeOutputSize.height() * 0.5);
                const qreal deltaX = (surfacePosition.x() - transient.dragOriginPos.x()) / safeScale.x();
                const qreal deltaY = (surfacePosition.y() - transient.dragOriginPos.y()) / safeScale.y();
                const qreal nextTranslationX =
                    qBound<qreal>(-1.0,
                                  transient.dragOriginTranscriptTranslation.x() + (deltaX / halfOutputWidth),
                                  1.0);
                const qreal nextTranslationY =
                    qBound<qreal>(-1.0,
                                  transient.dragOriginTranscriptTranslation.y() + (deltaY / halfOutputHeight),
                                  1.0);
                m_state->transient.transcriptOverrideActive = true;
                m_state->transient.transcriptOverrideClipId = clipId;
                m_state->transient.transcriptTranslationOverride = QPointF(nextTranslationX, nextTranslationY);
            } else {
                const qreal deltaX = (surfacePosition.x() - transient.dragOriginPos.x()) / safeScale.x();
                const qreal deltaY = (surfacePosition.y() - transient.dragOriginPos.y()) / safeScale.y();
                TimelineClip::TransformKeyframe overrideTransform = transient.dragOriginTransform;
                overrideTransform.translationX = transient.dragOriginTransform.translationX + deltaX;
                overrideTransform.translationY = transient.dragOriginTransform.translationY + deltaY;
                m_state->transient.transformOverrideActive = true;
                m_state->transient.transformOverrideClipId = clipId;
                m_state->transient.transformOverride = overrideTransform;
            }
            schedulePreviewUpdate();
            event->accept();
            return;
        }

        if (m_state->transient.speakerPickDragActive &&
            (event->buttons() & Qt::LeftButton)) {
            m_state->transient.speakerPickCurrentPos = surfacePosition;
            schedulePreviewUpdate();
            event->accept();
            return;
        }

        if (activeInfo.kind == PreviewOverlayKind::TranscriptOverlay) {
            const QSizeF safeOutputSize = m_state->outputSize.isValid()
                                            ? m_state->outputSize
                                            : QSize(1080, 1920);
            const qreal halfOutputWidth = qMax<qreal>(1.0, safeOutputSize.width() * 0.5);
            const qreal halfOutputHeight = qMax<qreal>(1.0, safeOutputSize.height() * 0.5);
            const QRectF originBounds = transient.dragOriginBounds.isValid()
                                            ? transient.dragOriginBounds
                                            : activeInfo.bounds;
            const qreal originWidth =
                originBounds.width() / qMax<qreal>(0.0001, previewScale.x());
            const qreal originHeight =
                originBounds.height() / qMax<qreal>(0.0001, previewScale.y());
            qreal width = originWidth;
            qreal height = originHeight;
            if (m_state->transient.dragMode == PreviewDragMode::ResizeX ||
                m_state->transient.dragMode == PreviewDragMode::ResizeBoth) {
                width = qMax<qreal>(80.0,
                                    originWidth +
                                        ((surfacePosition.x() - transient.dragOriginPos.x()) /
                                         qMax<qreal>(0.0001, safeScale.x())));
            }
            if (m_state->transient.dragMode == PreviewDragMode::ResizeY ||
                m_state->transient.dragMode == PreviewDragMode::ResizeBoth) {
                height = qMax<qreal>(40.0,
                                     originHeight +
                                         ((surfacePosition.y() - transient.dragOriginPos.y()) /
                                          qMax<qreal>(0.0001, safeScale.y())));
            }
            m_state->transient.transcriptOverrideActive = true;
            m_state->transient.transcriptOverrideClipId = clipId;
            QPointF translation = transient.dragOriginTranscriptTranslation;
            if (m_state->transient.dragMode == PreviewDragMode::ResizeX ||
                m_state->transient.dragMode == PreviewDragMode::ResizeBoth) {
                translation.setX(qBound<qreal>(
                    -1.0,
                    translation.x() + (((width - originWidth) * 0.5) / halfOutputWidth),
                    1.0));
            }
            if (m_state->transient.dragMode == PreviewDragMode::ResizeY ||
                m_state->transient.dragMode == PreviewDragMode::ResizeBoth) {
                translation.setY(qBound<qreal>(
                    -1.0,
                    translation.y() + (((height - originHeight) * 0.5) / halfOutputHeight),
                    1.0));
            }
            m_state->transient.transcriptTranslationOverride = translation;
            m_state->transient.transcriptSizeOverride = QSizeF(width, height);
        } else {
            TimelineClip::TransformKeyframe overrideTransform;
            if (computeVulkanVisualResizeTransform(transient,
                                                   m_state->transient.dragMode,
                                                   surfacePosition,
                                                   previewScale,
                                                   activeInfo.clipPixelSize.isValid(),
                                                   &overrideTransform)) {
                m_state->transient.transformOverrideActive = true;
                m_state->transient.transformOverrideClipId = clipId;
                m_state->transient.transformOverride = overrideTransform;
            }
        }
        schedulePreviewUpdate();
        event->accept();
    }

    void mouseReleaseEvent(QMouseEvent* event) override
    {
        if (!event || event->button() != Qt::LeftButton || !m_state) {
            QWindow::mouseReleaseEvent(event);
            return;
        }

        if (m_state->transient.dragMode != PreviewDragMode::None) {
            const QString& clipId = m_state->selectedClipId;
            const QRectF surfaceRect = PreviewViewTransform::rectForWindow(
                this, PreviewSurfaceCoordinateSpace::DeviceSurface);
            const PreviewViewTransform viewTransform(
                surfaceRect,
                m_state->outputSize,
                vulkanPreviewCanvasMarginPx(),
                m_state->previewZoom,
                m_state->previewPanOffset);
            const QPointF previewScale = viewTransform.outputScale();
            const VulkanInteractionOverlayInfos infos = collectVulkanInteractionInfos(m_state, surfaceRect);
            VulkanInteractionOverlayInfo activeInfo;
            const bool activeInfoIsTranscript =
                lookupVulkanInteractionInfo(infos, clipId, &activeInfo) &&
                activeInfo.kind == PreviewOverlayKind::TranscriptOverlay;
            if (m_state->transient.dragMode == PreviewDragMode::Move) {
                if (m_moveRequested) {
                    if (activeInfoIsTranscript) {
                        const QPointF translation =
                            m_state->transient.transcriptOverrideActive &&
                                    m_state->transient.transcriptOverrideClipId == clipId
                                ? m_state->transient.transcriptTranslationOverride
                                : m_state->transient.dragOriginTranscriptTranslation;
                        m_moveRequested(clipId, translation.x(), translation.y(), true);
                    } else {
                        const TimelineClip::TransformKeyframe transform =
                            currentTransformForVulkanClip(m_state, clipId);
                        m_moveRequested(clipId, transform.translationX, transform.translationY, true);
                    }
                }
            } else if (m_transformRequested && !clipId.isEmpty()) {
                if (activeInfoIsTranscript) {
                    const QSizeF size =
                        m_state->transient.transcriptOverrideActive &&
                                m_state->transient.transcriptOverrideClipId == clipId &&
                                m_state->transient.transcriptSizeOverride.width() > 0.0 &&
                                m_state->transient.transcriptSizeOverride.height() > 0.0
                            ? m_state->transient.transcriptSizeOverride
                            : QSizeF(activeInfo.bounds.width() / qMax<qreal>(0.0001, previewScale.x()),
                                     activeInfo.bounds.height() / qMax<qreal>(0.0001, previewScale.y()));
                    const QPointF translation =
                        m_state->transient.transcriptOverrideActive &&
                                m_state->transient.transcriptOverrideClipId == clipId
                            ? m_state->transient.transcriptTranslationOverride
                            : m_state->transient.dragOriginTranscriptTranslation;
                    m_transformRequested(clipId,
                                         translation.x(),
                                         translation.y(),
                                         size.width(),
                                         size.height(),
                                         true);
                } else {
                    bool hasResizeOverride =
                        m_state->transient.transformOverrideActive &&
                        m_state->transient.transformOverrideClipId == clipId;
                    TimelineClip::TransformKeyframe transform =
                        hasResizeOverride
                            ? m_state->transient.transformOverride
                            : TimelineClip::TransformKeyframe();
                    if (!hasResizeOverride) {
                        hasResizeOverride = computeVulkanVisualResizeTransform(
                            m_state->transient,
                            m_state->transient.dragMode,
                            PreviewViewTransform::pointForWindowPoint(
                                this,
                                event->position(),
                                PreviewSurfaceCoordinateSpace::DeviceSurface),
                            previewScale,
                            activeInfo.clipPixelSize.isValid(),
                            &transform);
                    }
                    if (!hasResizeOverride) {
                        qWarning().noquote()
                            << QStringLiteral("[preview-transform-release] ok=false reason=missing_resize_override clip=%1 drag_mode=%2")
                                   .arg(clipId)
                                   .arg(static_cast<int>(m_state->transient.dragMode));
                        m_state->transient.dragMode = PreviewDragMode::None;
                        m_state->transient.dragOriginBounds = QRectF();
                        m_state->transient.dragOriginTranscriptTranslation = QPointF();
                        clearVulkanDragOverrides(m_state);
                        schedulePreviewUpdate();
                        event->accept();
                        return;
                    }
                    qInfo().noquote()
                        << QStringLiteral("[preview-transform-release] ok=true clip=%1 drag_mode=%2 tx=%3 ty=%4 sx=%5 sy=%6")
                               .arg(clipId)
                               .arg(static_cast<int>(m_state->transient.dragMode))
                               .arg(transform.translationX, 0, 'f', 3)
                               .arg(transform.translationY, 0, 'f', 3)
                               .arg(transform.scaleX, 0, 'f', 4)
                               .arg(transform.scaleY, 0, 'f', 4);
                    m_transformRequested(clipId,
                                         transform.translationX,
                                         transform.translationY,
                                         transform.scaleX,
                                         transform.scaleY,
                                         true);
                }
            }
            m_state->transient.dragMode = PreviewDragMode::None;
            m_state->transient.dragOriginBounds = QRectF();
            m_state->transient.dragOriginTranscriptTranslation = QPointF();
            clearVulkanDragOverrides(m_state);
            schedulePreviewUpdate();
            event->accept();
            return;
        }
        if (m_state->transient.speakerPickDragActive) {
            const QString clipId = m_state->transient.speakerPickClipId;
            const QRectF surfaceRect = PreviewViewTransform::rectForWindow(
                this, PreviewSurfaceCoordinateSpace::DeviceSurface);
            const VulkanInteractionOverlayInfos infos = collectVulkanInteractionInfos(m_state, surfaceRect);
            VulkanInteractionOverlayInfo info;
            const bool haveActiveInfo = lookupVulkanInteractionInfo(infos, clipId, &info);
            const QPointF endPos = PreviewViewTransform::pointForWindowPoint(
                this, event->position(), PreviewSurfaceCoordinateSpace::DeviceSurface);
            m_state->transient.speakerPickCurrentPos = endPos;
            if (haveActiveInfo && info.bounds.isValid() &&
                info.bounds.width() > 1.0 && info.bounds.height() > 1.0) {
                const QPointF startNorm = mapScreenPointToNormalizedClipForVulkan(info, m_state->transient.speakerPickStartPos);
                const QPointF endNorm = mapScreenPointToNormalizedClipForVulkan(info, endPos);
                const qreal dx = endNorm.x() - startNorm.x();
                const qreal dy = endNorm.y() - startNorm.y();
                const qreal dragDistance = std::sqrt((dx * dx) + (dy * dy));
                if (dragDistance < 0.01 && m_speakerPointRequested) {
                    m_speakerPointRequested(clipId, startNorm.x(), startNorm.y());
                } else if (m_speakerBoxRequested) {
                    const qreal startScreenX = m_state->transient.speakerPickStartPos.x();
                    const qreal startScreenY = m_state->transient.speakerPickStartPos.y();
                    const qreal endScreenX = endPos.x();
                    const qreal endScreenY = endPos.y();
                    const qreal sideScreenPx = qMax(qAbs(endScreenX - startScreenX),
                                                    qAbs(endScreenY - startScreenY));
                    const qreal minScreenSide = qMax<qreal>(
                        1.0, qMin<qreal>(info.bounds.width(), info.bounds.height()));
                    const qreal side = qBound<qreal>(
                        0.02,
                        dragDistance >= 0.01 ? (sideScreenPx / minScreenSide) : 0.06,
                        1.0);
                    const qreal cx = qBound<qreal>(0.0, (startNorm.x() + endNorm.x()) * 0.5, 1.0);
                    const qreal cy = qBound<qreal>(0.0, (startNorm.y() + endNorm.y()) * 0.5, 1.0);
                    m_speakerBoxRequested(clipId, cx, cy, side);
                }
            }
            m_state->transient.speakerPickDragActive = false;
            m_state->transient.speakerPickClipId.clear();
            m_state->transient.speakerPickStartPos = QPointF();
            m_state->transient.speakerPickCurrentPos = QPointF();
            schedulePreviewUpdate();
            event->accept();
            return;
        }

        QWindow::mouseReleaseEvent(event);
    }

    void keyPressEvent(QKeyEvent* event) override
    {
        if ((event->key() == Qt::Key_Shift || (event->modifiers() & Qt::ShiftModifier)) &&
            (m_speakerPointRequested || m_speakerBoxRequested)) {
            const QPointF cursorPos = PreviewViewTransform::pointForWindowPoint(
                this, mapFromGlobal(QCursor::pos()), PreviewSurfaceCoordinateSpace::DeviceSurface);
            updatePreviewCursor(cursorPos);
            schedulePreviewUpdate();
        }
        QWindow::keyPressEvent(event);
    }

    void keyReleaseEvent(QKeyEvent* event) override
    {
        if (event->key() == Qt::Key_Shift || event->key() == Qt::Key_Alt || event->key() == Qt::Key_Control) {
            const QPointF cursorPos = PreviewViewTransform::pointForWindowPoint(
                this, mapFromGlobal(QCursor::pos()), PreviewSurfaceCoordinateSpace::DeviceSurface);
            updatePreviewCursor(cursorPos);
            schedulePreviewUpdate();
        }
        QWindow::keyReleaseEvent(event);
    }

    bool event(QEvent* event) override
    {
        if (!event) {
            return QWindow::event(event);
        }

        if (event->type() == QEvent::UpdateRequest) {
            m_updateRequestPosted = false;
            if (m_presentationTelemetry) {
                m_presentationTelemetry->previewUpdateEventsDelivered.fetch_add(
                    1, std::memory_order_relaxed);
            }
            if (!m_frameInProgress && isExposed()) {
                m_updateDirty = false;
                renderNow();
                if (!m_frameInProgress) {
                    m_updateRequestMs = -1;
                }
            }
            return true;
        }

        if (event->type() == QEvent::Leave) {
            if (m_state) {
                m_state->transient.lastMousePos = QPointF(-10000.0, -10000.0);
                m_state->transient.speakerPickCurrentPos = QPointF(-10000.0, -10000.0);
                m_state->transient.hoveredFaceDetectionsTrackId = -1;
                m_state->transient.hoveredFaceDetectionsClipId.clear();
                m_state->transient.hoveredFaceDetectionsId.clear();
                if (!m_state->transient.speakerPickDragActive) {
                    unsetCursor();
                    schedulePreviewUpdate();
                }
            }
            return QWindow::event(event);
        }

        if (event->type() == QEvent::ContextMenu) {
            auto* contextMenu = static_cast<QContextMenuEvent*>(event);
            if (!contextMenu || !m_state) {
                return QWindow::event(event);
            }
            const QRectF surfaceRect = PreviewViewTransform::rectForWindow(
                this, PreviewSurfaceCoordinateSpace::DeviceSurface);
            const QPointF surfacePosition = PreviewViewTransform::pointForWindowPoint(
                this, contextMenu->pos(), PreviewSurfaceCoordinateSpace::DeviceSurface);
            const VulkanInteractionOverlayInfos infos = collectVulkanInteractionInfos(m_state, surfaceRect);
            if (m_state->transient.faceDetectionsRightClickHandled) {
                m_state->transient.faceDetectionsRightClickHandled = false;
                contextMenu->accept();
                return true;
            }
            if (m_faceStreamBoxFocusClearRequested &&
                dispatchFaceDetectionsFocusClearAtPosition(
                    m_state,
                    infos,
                    surfacePosition,
                    m_faceStreamBoxFocusClearRequested,
                    m_faceStreamBoxClickStatus)) {
                schedulePreviewUpdate();
                contextMenu->accept();
                return true;
            }
            QString hitClipId = clipIdAtPositionForVulkan(infos, surfacePosition);
            if (m_state->titleOverlayInteractionOnly && !clipIdIsTitleForVulkan(m_state, hitClipId)) {
                hitClipId.clear();
            }
            if (hitClipId.isEmpty()) {
                return QWindow::event(event);
            }
            if (m_state->selectedClipId != hitClipId) {
                m_state->selectedClipId = hitClipId;
                if (m_selectionRequested) {
                    m_selectionRequested(hitClipId);
                }
                schedulePreviewUpdate();
            }
            QMenu menu;
            QAction* createKeyframeAction = menu.addAction(QStringLiteral("Create Keyframe Here"));
            QAction* chosen = menu.exec(contextMenu->globalPos());
            if (chosen == createKeyframeAction && m_createKeyframeRequested) {
                m_createKeyframeRequested(hitClipId);
                return true;
            }
            return QWindow::event(event);
        }

        return QWindow::event(event);
    }

private:
    void updatePreviewCursor(const QPointF& position)
    {
        if (!m_state) {
            return;
        }
        m_state->transient.speakerPickCurrentPos = position;
        const QRectF surfaceRect = PreviewViewTransform::rectForWindow(
            this, PreviewSurfaceCoordinateSpace::DeviceSurface);
        const VulkanInteractionOverlayInfos infos = collectVulkanInteractionInfos(m_state, surfaceRect);
        const QString currentClipId = m_state->selectedClipId;

        if (m_state->correctionDrawMode) {
            if (!m_state->transient.speakerPickHintClipId.isEmpty()) {
                m_state->transient.speakerPickHintClipId.clear();
            }
            setCursor(Qt::CrossCursor);
            return;
        }

        const bool speakerPickModifierActive =
            (QApplication::keyboardModifiers() & Qt::ShiftModifier) &&
            (m_speakerPointRequested || m_speakerBoxRequested);
        const QString speakerPickHintClipId =
            speakerPickModifierActive ? clipIdAtPositionForVulkan(infos, position) : QString();
        if (m_state->transient.speakerPickHintClipId != speakerPickHintClipId) {
            m_state->transient.speakerPickHintClipId = speakerPickHintClipId;
        }
        if (!speakerPickHintClipId.isEmpty()) {
            setCursor(Qt::CrossCursor);
            return;
        }

        if (!m_state->facedetectionsOverlays.isEmpty()) {
            if (updateHoveredFaceDetectionsBox(m_state, infos, position)) {
                schedulePreviewUpdate();
                setCursor(Qt::PointingHandCursor);
                return;
            }
            if (m_state->transient.hoveredFaceDetectionsTrackId >= 0 ||
                !m_state->transient.hoveredFaceDetectionsClipId.isEmpty() ||
                !m_state->transient.hoveredFaceDetectionsId.isEmpty()) {
                m_state->transient.hoveredFaceDetectionsTrackId = -1;
                m_state->transient.hoveredFaceDetectionsClipId.clear();
                m_state->transient.hoveredFaceDetectionsId.clear();
                schedulePreviewUpdate();
            }
        }

        if (m_state->viewMode == PreviewSurface::ViewMode::Audio) {
            setCursor(Qt::ArrowCursor);
            return;
        }

        if (m_state->titleOverlayInteractionOnly && !clipIdIsTitleForVulkan(m_state, currentClipId)) {
            unsetCursor();
            return;
        }

        const bool titleInteractionOnly = m_state->titleOverlayInteractionOnly;
        const bool selectedClipIsTitle = clipIdIsTitleForVulkan(m_state, currentClipId);
        const bool allowSelectedClipInteraction = !titleInteractionOnly || selectedClipIsTitle;
        if (!currentClipId.isEmpty() && allowSelectedClipInteraction) {
            VulkanInteractionOverlayInfo selectedInfo;
            if (lookupVulkanInteractionInfo(infos, currentClipId, &selectedInfo)) {
                if (selectedInfo.cornerHandle.contains(position)) {
                    setCursor(Qt::SizeFDiagCursor);
                    return;
                }
                if (selectedInfo.rightHandle.contains(position)) {
                    setCursor(Qt::SizeHorCursor);
                    return;
                }
                if (selectedInfo.bottomHandle.contains(position)) {
                    setCursor(Qt::SizeVerCursor);
                    return;
                }
                if (selectedInfo.bounds.contains(position)) {
                    setCursor(m_state->transient.dragMode == PreviewDragMode::Move ? Qt::ClosedHandCursor
                                                                                  : Qt::OpenHandCursor);
                    return;
                }
            }
        }
        unsetCursor();
    }

public:
    PreviewInteractionState* state() const { return m_state; }
    DirectVulkanPreviewStats* stats() const { return m_stats; }
    void beginPreviewFrame()
    {
        const qint64 nowMs = QDateTime::currentMSecsSinceEpoch();
        m_frameInProgress = true;
        m_updateDirty = false;
        m_frameRequestMs =
            m_updateRequestMs >= 0 ? m_updateRequestMs : nowMs;
        m_updateRequestMs = -1;
    }
    void markPresented(
        const PreviewInteractionState* presentedState = nullptr,
        const QSet<QString>* submittedClipIds = nullptr,
        const QSet<QString>* submittedCrossfadeClipIds = nullptr)
    {
        if (presentedState &&
            (!presentedState->playing ||
             presentedState->viewMode != PreviewSurface::ViewMode::Video)) {
            m_presentationMissTracker.endPresentationRun();
        } else if (presentedState && submittedClipIds) {
            QVector<editor::PresentationFrameSample> samples;
            samples.reserve(presentedState->vulkanFrameStatuses.size() * 2);
            for (const VulkanPreviewClipFrameStatus& status :
                 presentedState->vulkanFrameStatuses) {
                if (!editor::presentationStatusRequiresDraw(
                        status.active,
                        status.drawSuppressed,
                        status.missingReason)) {
                    continue;
                }
                const QString streamId = editor::presentationStreamId(
                    status.clipId,
                    status.mediaOwnerClipId,
                    status.maskClipSource);
                samples.push_back(editor::PresentationFrameSample{
                    streamId,
                    status.requestedSourceFrame,
                    editor::presentedFrameForDrawOutcome(
                        submittedClipIds->contains(status.clipId),
                        status.presentedSourceFrame),
                });
                if (status.frameCrossfadeActive) {
                    samples.push_back(editor::PresentationFrameSample{
                        streamId + QStringLiteral("#frame_crossfade"),
                        status.frameCrossfadeRequestedSourceFrame,
                        editor::presentedFrameForDrawOutcome(
                            submittedCrossfadeClipIds &&
                                submittedCrossfadeClipIds->contains(status.clipId),
                            status.frameCrossfadePresentedSourceFrame),
                    });
                }
            }
            const int64_t presentationMisses =
                m_presentationMissTracker.recordPresentedFrame(samples);
            if (m_presentationTelemetry && presentationMisses > 0) {
                m_presentationTelemetry->uniquePresentationMisses.fetch_add(
                    presentationMisses, std::memory_order_relaxed);
            }
        }

        const qint64 nowMs = QDateTime::currentMSecsSinceEpoch();
        if (m_lastPresentMs > 0 && m_stats) {
            const double intervalMs = static_cast<double>(nowMs - m_lastPresentMs);
            m_stats->lastPresentIntervalMs = intervalMs;
            m_stats->maxPresentIntervalMs = std::max(m_stats->maxPresentIntervalMs, intervalMs);
        }
        m_lastPresentMs = nowMs;
        if (m_presentationTelemetry) {
            m_presentationTelemetry->presentedFrames.fetch_add(
                1, std::memory_order_relaxed);
        }
        if (m_stats) {
            editor::accumulatePlaybackStageMetric(&m_stats->presentationStageMetric,
                                          0,
                                          1,
                                          0,
                                          QStringLiteral("presented"),
                                          QStringLiteral("frame_ready"));
        }
    }
    void markPresentedSourceFrames(int64_t requestedFrame, int64_t presentedFrame)
    {
        if (m_presentationTelemetry) {
            m_presentationTelemetry->activeRequestedSourceFrame.store(
                requestedFrame, std::memory_order_relaxed);
            m_presentationTelemetry->activePresentedSourceFrame.store(
                presentedFrame, std::memory_order_relaxed);
        }
    }
    void markPreviewUpdateDelivered()
    {
        if (m_frameRequestMs >= 0 && m_stats) {
            const double latencyMs =
                static_cast<double>(QDateTime::currentMSecsSinceEpoch() - m_frameRequestMs);
            m_stats->lastPreviewUpdateLatencyMs = latencyMs;
            m_stats->maxPreviewUpdateLatencyMs =
                std::max(m_stats->maxPreviewUpdateLatencyMs, latencyMs);
        }
        if (m_presentationTelemetry) {
            m_presentationTelemetry->previewUpdatesDelivered.fetch_add(
                1, std::memory_order_relaxed);
        }
        m_frameInProgress = false;
        m_frameRequestMs = -1;
        if (m_updateDirty && isExposed()) {
            schedulePreviewUpdate();
        }
    }
    void resetProfilingAnchors()
    {
        m_presentationMissTracker.reset();
        m_lastPresentMs = 0;
        const qint64 nowMs = QDateTime::currentMSecsSinceEpoch();
        if (m_updateRequestMs >= 0) {
            m_updateRequestMs = nowMs;
        }
        if (m_frameRequestMs >= 0) {
            m_frameRequestMs = nowMs;
        }
    }
    void setLatestVulkanReadbackImage(const QImage& image)
    {
        m_latestVulkanReadbackImage = image;
        if (m_mirrorCallback) {
            m_mirrorCallback(image);
        }
    }
    void setLatestDecoderDiagnosticImage(const QImage& image)
    {
        m_latestDecoderDiagnosticImage = image;
    }
    void requestPipelineThumbnailReadback()
    {
        const qint64 now = QDateTime::currentMSecsSinceEpoch();
        if (m_pipelineThumbnailReadbackPending) {
            return;
        }
        if (!m_latestVulkanReadbackImage.isNull() &&
            m_lastPipelineThumbnailReadbackMs > 0 &&
            now - m_lastPipelineThumbnailReadbackMs < kPipelineThumbnailReadbackMinIntervalMs) {
            return;
        }
        m_pipelineThumbnailReadbackPending = true;
        if (m_stats) {
            ++m_stats->diagnosticReadbackRequests;
        }
        schedulePreviewUpdate();
    }
    bool pipelineThumbnailReadbackPending() const
    {
        return m_pipelineThumbnailReadbackPending;
    }
    void markPipelineThumbnailReadbackRecorded(const QSize& size)
    {
        m_pipelineThumbnailReadbackPending = false;
        m_lastPipelineThumbnailReadbackMs = QDateTime::currentMSecsSinceEpoch();
        if (m_stats) {
            ++m_stats->diagnosticReadbackCopies;
            m_stats->lastDiagnosticReadbackSize = size;
            m_stats->lastDiagnosticReadbackFormat = vulkanFormatName(colorFormat());
        }
    }
    QImage latestVulkanReadbackImage() const
    {
        return m_latestVulkanReadbackImage;
    }
    QImage latestDecoderDiagnosticImage() const
    {
        return m_latestDecoderDiagnosticImage;
    }
    void markFailure(const QString& reason)
    {
        m_failureLatched = true;
        if (m_active) {
            *m_active = false;
        }
        if (m_failureReason) {
            *m_failureReason = reason;
        }
        if (m_failureCallback) {
            m_failureCallback(reason);
        }
        qWarning().noquote() << QStringLiteral("[vulkan-preview] %1").arg(reason);
    }
    void setMirrorCallback(std::function<void(const QImage&)> callback)
    {
        m_mirrorCallback = std::move(callback);
    }

private:
    struct FrameResources {
        VkCommandPool commandPool = VK_NULL_HANDLE;
        VkCommandBuffer commandBuffer = VK_NULL_HANDLE;
        VkFence inFlightFence = VK_NULL_HANDLE;
        VkSemaphore imageAcquiredSemaphore = VK_NULL_HANDLE;
        VkSemaphore renderCompleteSemaphore = VK_NULL_HANDLE;
    };

    PreviewInteractionState* m_state = nullptr;
    DirectVulkanPresentationTelemetry* m_presentationTelemetry = nullptr;
    DirectVulkanPreviewStats* m_stats = nullptr;
    bool* m_active = nullptr;
    QString* m_failureReason = nullptr;
    bool m_enableAudioPipeline = true;
    std::function<void(const QString&)> m_failureCallback;
    std::function<void(const QImage&)> m_mirrorCallback;
    std::function<void(const QString&)> m_selectionRequested;
    std::function<void(int64_t)> m_playbackSampleRequested;
    std::function<void(const QString&, qreal, qreal)> m_correctionPointRequested;
    std::function<void(const QString&, int64_t, int64_t, qreal, qreal)> m_maskFuzzyRemovePointRequested;
    std::function<void(const QString&, qreal, qreal)> m_speakerPointRequested;
    std::function<void(const QString&, qreal, qreal, qreal)> m_speakerBoxRequested;
    std::function<void(const QString&, int, const QString&, int64_t, qreal, qreal, qreal)> m_faceStreamBoxRequested;
    std::function<void(const QString&, int, const QString&, int64_t, qreal, qreal, qreal)> m_faceStreamBoxFocusClearRequested;
    std::function<void(const QString&)> m_faceStreamBoxClickStatus;
    std::function<void(const QString&)> m_createKeyframeRequested;
    std::function<void(const QString&, qreal, qreal, bool)> m_moveRequested;
    std::function<void(const QString&, qreal, qreal, qreal, qreal, bool)> m_transformRequested;
    QImage m_latestVulkanReadbackImage;
    QImage m_latestDecoderDiagnosticImage;
    bool m_pipelineThumbnailReadbackPending = false;
    qint64 m_lastPipelineThumbnailReadbackMs = 0;
    bool m_updateDirty = false;
    bool m_frameInProgress = false;
    bool m_updateRequestPosted = false;
    bool m_updateDeferredWhileNotExposed = false;
    bool m_scheduledWhileExposed = false;
    QSize m_lastExposeScheduledSize;
    qint64 m_updateRequestMs = -1;
    qint64 m_frameRequestMs = -1;
    qint64 m_lastPresentMs = 0;
    bool m_failureLatched = false;
    bool m_rendererInitialized = false;
    bool m_swapchainDirty = true;
    bool m_frameSubmitted = false;
    int m_preferredPhysicalDeviceIndex = -1;
    int m_currentFrameSlot = 0;
    int m_currentSwapchainImageIndex = -1;
    bool m_hasPhysicalDeviceProperties = false;
    QByteArrayList m_requestedDeviceExtensions;
    QVector<VkPhysicalDevice> m_availablePhysicalDevices;
    QVector<VkPhysicalDeviceProperties> m_availablePhysicalDeviceProperties;
    VkPhysicalDevice m_physicalDevice = VK_NULL_HANDLE;
    VkPhysicalDeviceProperties m_physicalDeviceProperties{};
    VkDevice m_device = VK_NULL_HANDLE;
    VkQueue m_graphicsQueue = VK_NULL_HANDLE;
    uint32_t m_graphicsQueueFamilyIndex = UINT32_MAX;
    VkSurfaceKHR m_surface = VK_NULL_HANDLE;
    VkSwapchainKHR m_swapchain = VK_NULL_HANDLE;
    QSize m_swapchainPixelSize;
    VkFormat m_colorFormat = VK_FORMAT_UNDEFINED;
    VkFormat m_depthStencilFormat = VK_FORMAT_UNDEFINED;
    VkRenderPass m_defaultRenderPass = VK_NULL_HANDLE;
    std::vector<VkImage> m_swapchainImages;
    std::vector<VkImageView> m_swapchainImageViews;
    std::vector<VkFramebuffer> m_swapchainFramebuffers;
    std::vector<FrameResources> m_frames;
    VkFramebuffer m_currentFramebuffer = VK_NULL_HANDLE;
    VkCommandBuffer m_currentCommandBuffer = VK_NULL_HANDLE;
    VkSemaphore m_frameCompletionSemaphore = VK_NULL_HANDLE;
    editor::PresentationMissTracker m_presentationMissTracker;
    DirectVulkanPreviewRenderer* m_renderer = nullptr;
    QVector<render_detail::OffscreenVulkanFrame> m_gpuExportPreviewFrames;
};
