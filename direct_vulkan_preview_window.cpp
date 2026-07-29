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
    std::array<GpuExportPreviewSlot, 2> m_gpuExportPreviewSlots;
    int m_gpuExportPreviewCurrentSlot = -1;
    std::shared_ptr<render_detail::OffscreenVulkanFrameConsumptionState>
        m_pendingGpuExportPreviewConsumptionState;
    quint64 m_pendingGpuExportPreviewGeneration = 0;
    int m_pendingGpuExportPreviewSlot = -1;
    std::uint64_t m_pendingGpuExportPreviewProducerSessionId = 0;
    PFN_vkImportSemaphoreFdKHR m_importSemaphoreFd = nullptr;
};

} // namespace

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
        if (!frame.valid || frame.bufferIndex >= 2) {
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
                                QPointF(selectedClip->transcriptOverlay.translationX,
                                        selectedClip->transcriptOverlay.translationY);
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

DirectVulkanPreviewWindow::~DirectVulkanPreviewWindow()
{
    cleanupDevice();
    delete m_renderer;
    m_renderer = nullptr;
}

void DirectVulkanPreviewWindow::refreshPhysicalDeviceList()
{
    m_availablePhysicalDevices.clear();
    m_availablePhysicalDeviceProperties.clear();
    QVulkanInstance* instance = vulkanInstance();
    if (!instance || instance->vkInstance() == VK_NULL_HANDLE) {
        return;
    }
    uint32_t count = 0;
    if (vkEnumeratePhysicalDevices(instance->vkInstance(), &count, nullptr) != VK_SUCCESS ||
        count == 0) {
        return;
    }
    m_availablePhysicalDevices.resize(static_cast<qsizetype>(count));
    if (vkEnumeratePhysicalDevices(instance->vkInstance(),
                                   &count,
                                   m_availablePhysicalDevices.data()) != VK_SUCCESS) {
        m_availablePhysicalDevices.clear();
        return;
    }
    m_availablePhysicalDeviceProperties.reserve(static_cast<qsizetype>(count));
    for (VkPhysicalDevice device : m_availablePhysicalDevices) {
        VkPhysicalDeviceProperties properties{};
        vkGetPhysicalDeviceProperties(device, &properties);
        m_availablePhysicalDeviceProperties.push_back(properties);
    }
}

QVulkanInfoVector<QVulkanExtension>
DirectVulkanPreviewWindow::supportedDeviceExtensions() const
{
    auto* self = const_cast<DirectVulkanPreviewWindow*>(this);
    if (self->m_availablePhysicalDevices.isEmpty()) {
        self->refreshPhysicalDeviceList();
    }
    const int preferredIndex =
        self->m_preferredPhysicalDeviceIndex >= 0 &&
                self->m_preferredPhysicalDeviceIndex <
                    self->m_availablePhysicalDevices.size()
            ? self->m_preferredPhysicalDeviceIndex
            : editor::gpu::chooseVulkanDevice(self->m_availablePhysicalDeviceProperties);
    if (preferredIndex < 0 ||
        preferredIndex >= self->m_availablePhysicalDevices.size()) {
        return {};
    }
    uint32_t count = 0;
    vkEnumerateDeviceExtensionProperties(
        self->m_availablePhysicalDevices.at(preferredIndex),
        nullptr,
        &count,
        nullptr);
    std::vector<VkExtensionProperties> properties(count);
    if (count > 0 &&
        vkEnumerateDeviceExtensionProperties(
            self->m_availablePhysicalDevices.at(preferredIndex),
            nullptr,
            &count,
            properties.data()) != VK_SUCCESS) {
        return {};
    }
    QVulkanInfoVector<QVulkanExtension> result;
    for (const VkExtensionProperties& property : properties) {
        result.push_back(
            QVulkanExtension{QByteArray(property.extensionName),
                             property.specVersion});
    }
    return result;
}

int DirectVulkanPreviewWindow::selectGraphicsPresentQueueFamily(
    VkPhysicalDevice device)
{
    QVulkanInstance* instance = vulkanInstance();
    if (!instance || device == VK_NULL_HANDLE) {
        return -1;
    }
    uint32_t familyCount = 0;
    vkGetPhysicalDeviceQueueFamilyProperties(device, &familyCount, nullptr);
    if (familyCount == 0) {
        return -1;
    }
    std::vector<VkQueueFamilyProperties> families(familyCount);
    vkGetPhysicalDeviceQueueFamilyProperties(
        device, &familyCount, families.data());
    for (uint32_t i = 0; i < familyCount; ++i) {
        if ((families[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) &&
            instance->supportsPresent(device, i, this)) {
            return static_cast<int>(i);
        }
    }
    return -1;
}

QSize DirectVulkanPreviewWindow::swapchainPixelSizeForWindow() const
{
    const QSize logicalSize = size();
    if (!logicalSize.isValid() || logicalSize.isEmpty()) {
        return QSize();
    }
    const qreal dpr = qMax<qreal>(1.0, devicePixelRatio());
    return QSize(qMax(1, qRound(static_cast<qreal>(logicalSize.width()) * dpr)),
                 qMax(1, qRound(static_cast<qreal>(logicalSize.height()) * dpr)));
}

void DirectVulkanPreviewWindow::cleanupSwapchain()
{
    if (m_rendererInitialized && m_renderer) {
        m_renderer->releaseResources();
        m_rendererInitialized = false;
    }
    if (m_device != VK_NULL_HANDLE) {
        vkDeviceWaitIdle(m_device);
    }
    for (VkFramebuffer framebuffer : m_swapchainFramebuffers) {
        if (framebuffer != VK_NULL_HANDLE) {
            vkDestroyFramebuffer(m_device, framebuffer, nullptr);
        }
    }
    m_swapchainFramebuffers.clear();
    for (VkImageView imageView : m_swapchainImageViews) {
        if (imageView != VK_NULL_HANDLE) {
            vkDestroyImageView(m_device, imageView, nullptr);
        }
    }
    m_swapchainImageViews.clear();
    m_swapchainImages.clear();
    if (m_defaultRenderPass != VK_NULL_HANDLE) {
        vkDestroyRenderPass(m_device, m_defaultRenderPass, nullptr);
        m_defaultRenderPass = VK_NULL_HANDLE;
    }
    if (m_swapchain != VK_NULL_HANDLE) {
        vkDestroySwapchainKHR(m_device, m_swapchain, nullptr);
        m_swapchain = VK_NULL_HANDLE;
    }
    m_swapchainPixelSize = QSize();
    m_colorFormat = VK_FORMAT_UNDEFINED;
    m_depthStencilFormat = VK_FORMAT_UNDEFINED;
    m_currentFramebuffer = VK_NULL_HANDLE;
    m_currentCommandBuffer = VK_NULL_HANDLE;
    m_currentSwapchainImageIndex = -1;
    m_swapchainDirty = true;
}

void DirectVulkanPreviewWindow::cleanupDevice()
{
    cleanupSwapchain();
    if (m_renderer) {
        m_renderer->releaseDeviceResources();
    }
    QVulkanInstance* instance = vulkanInstance();
    if (instance && m_device != VK_NULL_HANDLE) {
        instance->resetDeviceFunctions(m_device);
    }
    for (FrameResources& frame : m_frames) {
        if (frame.imageAcquiredSemaphore != VK_NULL_HANDLE) {
            vkDestroySemaphore(m_device, frame.imageAcquiredSemaphore, nullptr);
            frame.imageAcquiredSemaphore = VK_NULL_HANDLE;
        }
        if (frame.renderCompleteSemaphore != VK_NULL_HANDLE) {
            vkDestroySemaphore(m_device, frame.renderCompleteSemaphore, nullptr);
            frame.renderCompleteSemaphore = VK_NULL_HANDLE;
        }
        if (frame.inFlightFence != VK_NULL_HANDLE) {
            vkDestroyFence(m_device, frame.inFlightFence, nullptr);
            frame.inFlightFence = VK_NULL_HANDLE;
        }
        if (frame.commandPool != VK_NULL_HANDLE) {
            vkDestroyCommandPool(m_device, frame.commandPool, nullptr);
            frame.commandPool = VK_NULL_HANDLE;
            frame.commandBuffer = VK_NULL_HANDLE;
        }
    }
    m_frames.clear();
    if (m_device != VK_NULL_HANDLE) {
        vkDestroyDevice(m_device, nullptr);
        m_device = VK_NULL_HANDLE;
    }
    m_graphicsQueue = VK_NULL_HANDLE;
    m_graphicsQueueFamilyIndex = UINT32_MAX;
    m_physicalDevice = VK_NULL_HANDLE;
    m_hasPhysicalDeviceProperties = false;
    m_frameSubmitted = false;
    m_surface = VK_NULL_HANDLE;
}

bool DirectVulkanPreviewWindow::ensureVulkanReady()
{
    if (m_failureLatched) {
        return false;
    }
    QVulkanInstance* instance = vulkanInstance();
    if (!instance || !instance->isValid()) {
        markFailure(QStringLiteral(
            "Direct Vulkan preview requires a valid QVulkanInstance."));
        return false;
    }
    if (!handle()) {
        create();
    }
    if (m_surface == VK_NULL_HANDLE) {
        m_surface = QVulkanInstance::surfaceForWindow(this);
    }
    if (m_surface == VK_NULL_HANDLE) {
        return false;
    }
    if (m_device != VK_NULL_HANDLE) {
        return true;
    }
    if (m_availablePhysicalDevices.isEmpty()) {
        refreshPhysicalDeviceList();
    }
    if (m_availablePhysicalDevices.isEmpty()) {
        markFailure(QStringLiteral(
            "No Vulkan physical devices are available for direct preview."));
        return false;
    }

    QVector<int> candidateIndices;
    if (m_preferredPhysicalDeviceIndex >= 0 &&
        m_preferredPhysicalDeviceIndex < m_availablePhysicalDevices.size()) {
        candidateIndices.push_back(m_preferredPhysicalDeviceIndex);
    }
    const int automaticIndex =
        editor::gpu::chooseVulkanDevice(m_availablePhysicalDeviceProperties);
    if (automaticIndex >= 0 && !candidateIndices.contains(automaticIndex)) {
        candidateIndices.push_back(automaticIndex);
    }
    for (int i = 0; i < m_availablePhysicalDevices.size(); ++i) {
        if (!candidateIndices.contains(i)) {
            candidateIndices.push_back(i);
        }
    }

    QByteArrayList enabledDeviceExtensions;
    for (int candidateIndex : candidateIndices) {
        if (candidateIndex < 0 ||
            candidateIndex >= m_availablePhysicalDevices.size()) {
            continue;
        }
        const VkPhysicalDevice candidateDevice =
            m_availablePhysicalDevices.at(candidateIndex);
        const int queueFamily =
            selectGraphicsPresentQueueFamily(candidateDevice);
        if (queueFamily < 0) {
            continue;
        }

        uint32_t extensionCount = 0;
        vkEnumerateDeviceExtensionProperties(candidateDevice,
                                             nullptr,
                                             &extensionCount,
                                             nullptr);
        std::vector<VkExtensionProperties> extensionProperties(extensionCount);
        if (extensionCount > 0 &&
            vkEnumerateDeviceExtensionProperties(candidateDevice,
                                                 nullptr,
                                                 &extensionCount,
                                                 extensionProperties.data()) !=
                VK_SUCCESS) {
            continue;
        }
        auto hasExtension = [&extensionProperties](const QByteArray& name) {
            return std::any_of(
                extensionProperties.cbegin(),
                extensionProperties.cend(),
                [&name](const VkExtensionProperties& property) {
                    return QByteArray(property.extensionName) == name;
                });
        };
        if (!hasExtension(
                QByteArrayLiteral(VK_KHR_SWAPCHAIN_EXTENSION_NAME))) {
            continue;
        }
        enabledDeviceExtensions.clear();
        enabledDeviceExtensions.push_back(
            QByteArrayLiteral(VK_KHR_SWAPCHAIN_EXTENSION_NAME));
        for (const QByteArray& extension : m_requestedDeviceExtensions) {
            if (extension != QByteArrayLiteral(VK_KHR_SWAPCHAIN_EXTENSION_NAME) &&
                hasExtension(extension) &&
                !enabledDeviceExtensions.contains(extension)) {
                enabledDeviceExtensions.push_back(extension);
            }
        }

        std::vector<const char*> extensionNames;
        extensionNames.reserve(
            static_cast<size_t>(enabledDeviceExtensions.size()));
        for (const QByteArray& extension : enabledDeviceExtensions) {
            extensionNames.push_back(extension.constData());
        }

        const float queuePriority = 1.0f;
        VkDeviceQueueCreateInfo queueInfo{};
        queueInfo.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
        queueInfo.queueFamilyIndex = static_cast<uint32_t>(queueFamily);
        queueInfo.queueCount = 1;
        queueInfo.pQueuePriorities = &queuePriority;

        VkDeviceCreateInfo createInfo{};
        createInfo.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
        createInfo.queueCreateInfoCount = 1;
        createInfo.pQueueCreateInfos = &queueInfo;
        createInfo.enabledExtensionCount =
            static_cast<uint32_t>(extensionNames.size());
        createInfo.ppEnabledExtensionNames = extensionNames.data();

        VkDevice device = VK_NULL_HANDLE;
        if (vkCreateDevice(candidateDevice, &createInfo, nullptr, &device) !=
            VK_SUCCESS) {
            continue;
        }

        m_physicalDevice = candidateDevice;
        m_physicalDeviceProperties =
            m_availablePhysicalDeviceProperties.at(candidateIndex);
        m_hasPhysicalDeviceProperties = true;
        m_device = device;
        m_graphicsQueueFamilyIndex = static_cast<uint32_t>(queueFamily);
        vkGetDeviceQueue(
            m_device, m_graphicsQueueFamilyIndex, 0, &m_graphicsQueue);
        qInfo() << "[vulkan-preview] selected physical GPU" << candidateIndex
                << m_physicalDeviceProperties.deviceName
                << "preference=" << editor::gpu::preference();
        break;
    }

    if (m_device == VK_NULL_HANDLE) {
        markFailure(QStringLiteral(
            "No Vulkan graphics/present queue supports the direct preview surface."));
        return false;
    }

    constexpr int kFramesInFlight = 2;
    m_frames.resize(kFramesInFlight);
    for (FrameResources& frame : m_frames) {
        VkCommandPoolCreateInfo poolInfo{};
        poolInfo.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
        poolInfo.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
        poolInfo.queueFamilyIndex = m_graphicsQueueFamilyIndex;
        if (vkCreateCommandPool(m_device, &poolInfo, nullptr, &frame.commandPool) !=
            VK_SUCCESS) {
            markFailure(QStringLiteral(
                "Failed to create Vulkan command pool for direct preview."));
            cleanupDevice();
            return false;
        }

        VkCommandBufferAllocateInfo allocInfo{};
        allocInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
        allocInfo.commandPool = frame.commandPool;
        allocInfo.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        allocInfo.commandBufferCount = 1;
        if (vkAllocateCommandBuffers(
                m_device, &allocInfo, &frame.commandBuffer) != VK_SUCCESS) {
            markFailure(QStringLiteral(
                "Failed to allocate Vulkan command buffer for direct preview."));
            cleanupDevice();
            return false;
        }

        VkFenceCreateInfo fenceInfo{};
        fenceInfo.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
        fenceInfo.flags = VK_FENCE_CREATE_SIGNALED_BIT;
        if (vkCreateFence(m_device, &fenceInfo, nullptr, &frame.inFlightFence) !=
            VK_SUCCESS) {
            markFailure(QStringLiteral(
                "Failed to create Vulkan fence for direct preview."));
            cleanupDevice();
            return false;
        }

        VkSemaphoreCreateInfo semaphoreInfo{};
        semaphoreInfo.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;
        if (vkCreateSemaphore(m_device,
                              &semaphoreInfo,
                              nullptr,
                              &frame.imageAcquiredSemaphore) != VK_SUCCESS ||
            vkCreateSemaphore(m_device,
                              &semaphoreInfo,
                              nullptr,
                              &frame.renderCompleteSemaphore) != VK_SUCCESS) {
            markFailure(QStringLiteral(
                "Failed to create Vulkan semaphores for direct preview."));
            cleanupDevice();
            return false;
        }
    }

    if (!m_renderer) {
        m_renderer = new DirectVulkanPreviewRenderer(this, this);
    }
    m_currentFrameSlot = 0;
    return true;
}

bool DirectVulkanPreviewWindow::ensureSwapchain()
{
    if (!ensureVulkanReady()) {
        return false;
    }
    const QSize targetPixelSize = swapchainPixelSizeForWindow();
    if (!targetPixelSize.isValid() || targetPixelSize.isEmpty()) {
        return false;
    }
    if (!m_swapchainDirty && m_swapchain != VK_NULL_HANDLE &&
        m_swapchainPixelSize == targetPixelSize) {
        return true;
    }

    cleanupSwapchain();

    VkSurfaceCapabilitiesKHR capabilities{};
    if (vkGetPhysicalDeviceSurfaceCapabilitiesKHR(
            m_physicalDevice, m_surface, &capabilities) != VK_SUCCESS) {
        markFailure(QStringLiteral(
            "Failed to query Vulkan surface capabilities for direct preview."));
        return false;
    }

    uint32_t formatCount = 0;
    if (vkGetPhysicalDeviceSurfaceFormatsKHR(
            m_physicalDevice, m_surface, &formatCount, nullptr) != VK_SUCCESS ||
        formatCount == 0) {
        markFailure(QStringLiteral(
            "No Vulkan surface formats are available for direct preview."));
        return false;
    }
    std::vector<VkSurfaceFormatKHR> surfaceFormats(formatCount);
    if (vkGetPhysicalDeviceSurfaceFormatsKHR(m_physicalDevice,
                                             m_surface,
                                             &formatCount,
                                             surfaceFormats.data()) !=
        VK_SUCCESS) {
        markFailure(QStringLiteral(
            "Failed to enumerate Vulkan surface formats for direct preview."));
        return false;
    }

    VkSurfaceFormatKHR surfaceFormat = surfaceFormats.front();
    for (const VkSurfaceFormatKHR& candidate : surfaceFormats) {
        if ((candidate.format == VK_FORMAT_B8G8R8A8_UNORM ||
             candidate.format == VK_FORMAT_R8G8B8A8_UNORM) &&
            candidate.colorSpace == VK_COLOR_SPACE_SRGB_NONLINEAR_KHR) {
            surfaceFormat = candidate;
            break;
        }
    }

    uint32_t presentModeCount = 0;
    vkGetPhysicalDeviceSurfacePresentModesKHR(
        m_physicalDevice, m_surface, &presentModeCount, nullptr);
    std::vector<VkPresentModeKHR> presentModes(presentModeCount);
    if (presentModeCount > 0) {
        vkGetPhysicalDeviceSurfacePresentModesKHR(m_physicalDevice,
                                                  m_surface,
                                                  &presentModeCount,
                                                  presentModes.data());
    }
    VkPresentModeKHR presentMode = VK_PRESENT_MODE_FIFO_KHR;
    for (VkPresentModeKHR candidate : presentModes) {
        if (candidate == VK_PRESENT_MODE_MAILBOX_KHR) {
            presentMode = candidate;
            break;
        }
    }

    VkExtent2D extent{};
    if (capabilities.currentExtent.width != UINT32_MAX) {
        extent = capabilities.currentExtent;
    } else {
        extent.width = static_cast<uint32_t>(qBound(
            1,
            targetPixelSize.width(),
            static_cast<int>(capabilities.maxImageExtent.width)));
        extent.height = static_cast<uint32_t>(qBound(
            1,
            targetPixelSize.height(),
            static_cast<int>(capabilities.maxImageExtent.height)));
    }

    uint32_t imageCount = std::max<uint32_t>(2u, capabilities.minImageCount);
    if (capabilities.maxImageCount > 0) {
        imageCount = std::min(imageCount, capabilities.maxImageCount);
    }

    VkSwapchainCreateInfoKHR swapchainInfo{};
    swapchainInfo.sType = VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR;
    swapchainInfo.surface = m_surface;
    swapchainInfo.minImageCount = imageCount;
    swapchainInfo.imageFormat = surfaceFormat.format;
    swapchainInfo.imageColorSpace = surfaceFormat.colorSpace;
    swapchainInfo.imageExtent = extent;
    swapchainInfo.imageArrayLayers = 1;
    swapchainInfo.imageUsage =
        VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
    swapchainInfo.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;
    swapchainInfo.preTransform = capabilities.currentTransform;
    swapchainInfo.compositeAlpha =
        (capabilities.supportedCompositeAlpha &
         VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR)
            ? VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR
            : VK_COMPOSITE_ALPHA_INHERIT_BIT_KHR;
    swapchainInfo.presentMode = presentMode;
    swapchainInfo.clipped = VK_TRUE;
    if (vkCreateSwapchainKHR(m_device, &swapchainInfo, nullptr, &m_swapchain) !=
        VK_SUCCESS) {
        markFailure(QStringLiteral(
            "Failed to create Vulkan swapchain for direct preview."));
        return false;
    }

    uint32_t swapchainImageCount = 0;
    if (vkGetSwapchainImagesKHR(
            m_device, m_swapchain, &swapchainImageCount, nullptr) != VK_SUCCESS ||
        swapchainImageCount == 0) {
        markFailure(QStringLiteral(
            "Failed to query Vulkan swapchain images for direct preview."));
        cleanupSwapchain();
        return false;
    }
    m_swapchainImages.resize(swapchainImageCount);
    if (vkGetSwapchainImagesKHR(m_device,
                                m_swapchain,
                                &swapchainImageCount,
                                m_swapchainImages.data()) != VK_SUCCESS) {
        markFailure(QStringLiteral(
            "Failed to load Vulkan swapchain images for direct preview."));
        cleanupSwapchain();
        return false;
    }

    m_colorFormat = surfaceFormat.format;
    m_swapchainPixelSize = QSize(static_cast<int>(extent.width),
                                 static_cast<int>(extent.height));
    m_swapchainImageViews.resize(m_swapchainImages.size(), VK_NULL_HANDLE);
    for (size_t i = 0; i < m_swapchainImages.size(); ++i) {
        VkImageViewCreateInfo imageViewInfo{};
        imageViewInfo.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
        imageViewInfo.image = m_swapchainImages[i];
        imageViewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
        imageViewInfo.format = m_colorFormat;
        imageViewInfo.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        imageViewInfo.subresourceRange.levelCount = 1;
        imageViewInfo.subresourceRange.layerCount = 1;
        if (vkCreateImageView(m_device,
                              &imageViewInfo,
                              nullptr,
                              &m_swapchainImageViews[i]) != VK_SUCCESS) {
            markFailure(QStringLiteral(
                "Failed to create Vulkan swapchain image view for direct preview."));
            cleanupSwapchain();
            return false;
        }
    }

    VkAttachmentDescription colorAttachment{};
    colorAttachment.format = m_colorFormat;
    colorAttachment.samples = VK_SAMPLE_COUNT_1_BIT;
    colorAttachment.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
    colorAttachment.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    colorAttachment.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    colorAttachment.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    colorAttachment.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    colorAttachment.finalLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;

    VkAttachmentReference colorRef{};
    colorRef.attachment = 0;
    colorRef.layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;

    VkSubpassDescription subpass{};
    subpass.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
    subpass.colorAttachmentCount = 1;
    subpass.pColorAttachments = &colorRef;

    VkSubpassDependency dependency{};
    dependency.srcSubpass = VK_SUBPASS_EXTERNAL;
    dependency.dstSubpass = 0;
    dependency.srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    dependency.dstStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    dependency.dstAccessMask =
        VK_ACCESS_COLOR_ATTACHMENT_READ_BIT |
        VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;

    VkRenderPassCreateInfo renderPassInfo{};
    renderPassInfo.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO;
    renderPassInfo.attachmentCount = 1;
    renderPassInfo.pAttachments = &colorAttachment;
    renderPassInfo.subpassCount = 1;
    renderPassInfo.pSubpasses = &subpass;
    renderPassInfo.dependencyCount = 1;
    renderPassInfo.pDependencies = &dependency;
    if (vkCreateRenderPass(
            m_device, &renderPassInfo, nullptr, &m_defaultRenderPass) !=
        VK_SUCCESS) {
        markFailure(QStringLiteral(
            "Failed to create Vulkan render pass for direct preview."));
        cleanupSwapchain();
        return false;
    }

    m_swapchainFramebuffers.resize(m_swapchainImageViews.size(), VK_NULL_HANDLE);
    for (size_t i = 0; i < m_swapchainImageViews.size(); ++i) {
        VkImageView attachments[] = {m_swapchainImageViews[i]};
        VkFramebufferCreateInfo framebufferInfo{};
        framebufferInfo.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
        framebufferInfo.renderPass = m_defaultRenderPass;
        framebufferInfo.attachmentCount = 1;
        framebufferInfo.pAttachments = attachments;
        framebufferInfo.width = extent.width;
        framebufferInfo.height = extent.height;
        framebufferInfo.layers = 1;
        if (vkCreateFramebuffer(m_device,
                                &framebufferInfo,
                                nullptr,
                                &m_swapchainFramebuffers[i]) != VK_SUCCESS) {
            markFailure(QStringLiteral(
                "Failed to create Vulkan framebuffer for direct preview."));
            cleanupSwapchain();
            return false;
        }
    }

    m_swapchainDirty = false;
    if (m_renderer && !m_rendererInitialized) {
        m_renderer->initResources();
        m_rendererInitialized = !m_failureLatched;
    }
    return isValid();
}

void DirectVulkanPreviewWindow::markSwapchainDirty()
{
    m_swapchainDirty = true;
}

void DirectVulkanPreviewWindow::renderNow()
{
    if (m_frameInProgress || !isExposed()) {
        return;
    }
    if (!ensureSwapchain()) {
        return;
    }
    if (!m_renderer || m_frames.empty()) {
        return;
    }

    FrameResources& frame =
        m_frames[static_cast<size_t>(
            m_currentFrameSlot % static_cast<int>(m_frames.size()))];
    const VkResult frameWaitResult = vkWaitForFences(
        m_device,
        1,
        &frame.inFlightFence,
        VK_TRUE,
        kPreviewGpuWaitTimeoutNs);
    if (frameWaitResult != VK_SUCCESS) {
        markFailure(QStringLiteral(
            "Timed out after 1000 ms waiting for direct preview frame "
            "ownership (VkResult %1).")
                        .arg(static_cast<int>(frameWaitResult)));
        return;
    }
    vkResetFences(m_device, 1, &frame.inFlightFence);

    VkResult acquireResult = vkAcquireNextImageKHR(
        m_device,
        m_swapchain,
        kPreviewGpuWaitTimeoutNs,
        frame.imageAcquiredSemaphore,
        VK_NULL_HANDLE,
        reinterpret_cast<uint32_t*>(&m_currentSwapchainImageIndex));
    if (acquireResult == VK_ERROR_OUT_OF_DATE_KHR ||
        acquireResult == VK_SUBOPTIMAL_KHR ||
        acquireResult == VK_ERROR_SURFACE_LOST_KHR) {
        markSwapchainDirty();
        m_updateDirty = true;
        return;
    }
    if (acquireResult == VK_TIMEOUT) {
        m_updateDirty = true;
        qWarning().noquote()
            << QStringLiteral(
                   "[vulkan-sync-timeout] stage=preview_swapchain_acquire "
                   "timeout_ms=1000");
        return;
    }
    if (acquireResult != VK_SUCCESS) {
        markFailure(QStringLiteral(
            "Failed to acquire a Vulkan swapchain image for direct preview."));
        return;
    }

    vkResetCommandPool(m_device, frame.commandPool, 0);
    VkCommandBufferBeginInfo beginInfo{};
    beginInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    beginInfo.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    if (vkBeginCommandBuffer(frame.commandBuffer, &beginInfo) != VK_SUCCESS) {
        markFailure(QStringLiteral(
            "Failed to begin Vulkan command buffer for direct preview."));
        return;
    }

    m_currentFramebuffer =
        m_swapchainFramebuffers[static_cast<size_t>(m_currentSwapchainImageIndex)];
    m_currentCommandBuffer = frame.commandBuffer;
    m_frameSubmitted = false;
    m_renderer->startNextFrame();
    if (!m_frameSubmitted && !m_failureLatched) {
        markFailure(QStringLiteral(
            "Direct preview renderer returned without presenting the current frame."));
    }
    m_currentCommandBuffer = VK_NULL_HANDLE;
    m_currentFramebuffer = VK_NULL_HANDLE;
    m_currentFrameSlot =
        (m_currentFrameSlot + 1) % std::max(1, static_cast<int>(m_frames.size()));
}

bool DirectVulkanPreviewWindow::frameReady()
{
    if (m_frameSubmitted ||
        m_device == VK_NULL_HANDLE ||
        m_currentCommandBuffer == VK_NULL_HANDLE ||
        m_frames.empty()) {
        return false;
    }
    FrameResources& frame =
        m_frames[static_cast<size_t>(
            m_currentFrameSlot % static_cast<int>(m_frames.size()))];
    if (vkEndCommandBuffer(m_currentCommandBuffer) != VK_SUCCESS) {
        m_frameCompletionSemaphore = VK_NULL_HANDLE;
        markFailure(QStringLiteral(
            "Failed to finalize Vulkan command buffer for direct preview."));
        return false;
    }

    VkPipelineStageFlags waitStage =
        VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    VkSubmitInfo submitInfo{};
    submitInfo.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    submitInfo.waitSemaphoreCount = 1;
    submitInfo.pWaitSemaphores = &frame.imageAcquiredSemaphore;
    submitInfo.pWaitDstStageMask = &waitStage;
    submitInfo.commandBufferCount = 1;
    submitInfo.pCommandBuffers = &m_currentCommandBuffer;
    const std::array<VkSemaphore, 2> signalSemaphores{
        frame.renderCompleteSemaphore,
        m_frameCompletionSemaphore};
    submitInfo.signalSemaphoreCount =
        m_frameCompletionSemaphore == VK_NULL_HANDLE ? 1u : 2u;
    submitInfo.pSignalSemaphores = signalSemaphores.data();
    if (QVulkanInstance* instance = vulkanInstance()) {
        instance->presentAboutToBeQueued(this);
    }
    if (vkQueueSubmit(
            m_graphicsQueue, 1, &submitInfo, frame.inFlightFence) != VK_SUCCESS) {
        m_frameCompletionSemaphore = VK_NULL_HANDLE;
        markFailure(QStringLiteral(
            "Failed to submit a Vulkan frame for direct preview."));
        return false;
    }
    m_frameCompletionSemaphore = VK_NULL_HANDLE;

    uint32_t imageIndex =
        static_cast<uint32_t>(std::max(0, m_currentSwapchainImageIndex));
    VkPresentInfoKHR presentInfo{};
    presentInfo.sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR;
    presentInfo.waitSemaphoreCount = 1;
    presentInfo.pWaitSemaphores = &frame.renderCompleteSemaphore;
    presentInfo.swapchainCount = 1;
    presentInfo.pSwapchains = &m_swapchain;
    presentInfo.pImageIndices = &imageIndex;
    const VkResult presentResult =
        vkQueuePresentKHR(m_graphicsQueue, &presentInfo);
    if (QVulkanInstance* instance = vulkanInstance()) {
        instance->presentQueued(this);
    }
    if (presentResult == VK_ERROR_OUT_OF_DATE_KHR ||
        presentResult == VK_SUBOPTIMAL_KHR ||
        presentResult == VK_ERROR_SURFACE_LOST_KHR) {
        markSwapchainDirty();
        m_updateDirty = true;
    } else if (presentResult != VK_SUCCESS) {
        markFailure(QStringLiteral(
            "Failed to present a Vulkan frame for direct preview."));
        return true;
    }
    m_frameSubmitted = true;
    return true;
}

void DirectVulkanPreviewRenderer::initResources()
{
    m_devFuncs = m_window && m_window->vulkanInstance()
        ? m_window->vulkanInstance()->deviceFunctions(m_window->device())
        : nullptr;
    if (!m_resources) {
        if (m_window && m_window->physicalDeviceProperties()) {
            const VkPhysicalDeviceProperties* props =
                m_window->physicalDeviceProperties();
            qInfo().noquote()
                << QStringLiteral(
                       "[vulkan-preview] direct presenter device=%1 vendor=0x%2 type=%3")
                       .arg(QString::fromLatin1(props->deviceName))
                       .arg(QString::number(props->vendorID, 16))
                       .arg(static_cast<int>(props->deviceType));
        }
        m_resources = std::make_unique<VulkanResources>();
        if (!m_resources->initialize(
                m_window->physicalDevice(),
                m_window->device(),
                m_devFuncs)) {
            if (m_owner) {
                m_owner->markFailure(QStringLiteral(
                    "Failed to initialize direct presenter Vulkan resources."));
            }
            return;
        }
        m_importSemaphoreFd =
            reinterpret_cast<PFN_vkImportSemaphoreFdKHR>(
                vkGetDeviceProcAddr(m_window->device(),
                                    "vkImportSemaphoreFdKHR"));
    }
    if (!m_playbackStatusOverlayResources) {
        m_playbackStatusOverlayResources =
            std::make_unique<VulkanResources>();
        if (!m_playbackStatusOverlayResources->initialize(
                m_window->physicalDevice(),
                m_window->device(),
                m_devFuncs)) {
            if (m_owner) {
                m_owner->markFailure(QStringLiteral(
                    "Failed to initialize playback status overlay Vulkan resources."));
            }
            return;
        }
    }
    m_pipeline = std::make_unique<VulkanPipeline>();
    QString error;
    if (!m_pipeline->initialize(m_window->device(),
                                m_devFuncs,
                                m_window->defaultRenderPass(),
                                m_resources->descriptorSetLayout(),
                                &error)) {
        if (m_owner) {
            m_owner->markFailure(error.isEmpty()
                                     ? QStringLiteral("Failed to initialize direct presenter Vulkan pipeline.")
                                     : error);
        }
        return;
    }
    m_textRenderer = std::make_unique<VulkanTextRenderer>();
    if (!m_textRenderer->initialize(m_window->physicalDevice(),
                                    m_window->device(),
                                    m_devFuncs,
                                    m_window->defaultRenderPass(),
                                    &error)) {
        if (m_owner) {
            m_owner->markFailure(error.isEmpty()
                                     ? QStringLiteral("Failed to initialize Vulkan text renderer.")
                                     : error);
        }
        return;
    }
    m_speakerTextRenderer = std::make_unique<VulkanTextRenderer>();
    if (!m_speakerTextRenderer->initialize(m_window->physicalDevice(),
                                           m_window->device(),
                                           m_devFuncs,
                                           m_window->defaultRenderPass(),
                                           &error)) {
        if (m_owner) {
            m_owner->markFailure(error.isEmpty()
                                     ? QStringLiteral("Failed to initialize Vulkan speaker text renderer.")
                                     : error);
        }
        return;
    }
    m_temporalDebugTextRenderer = std::make_unique<VulkanTextRenderer>();
    if (!m_temporalDebugTextRenderer->initialize(m_window->physicalDevice(),
                                                m_window->device(),
                                                m_devFuncs,
                                                m_window->defaultRenderPass(),
                                                &error)) {
        if (m_owner) {
            m_owner->markFailure(error.isEmpty()
                                     ? QStringLiteral("Failed to initialize Vulkan temporal debug text renderer.")
                                     : error);
        }
        return;
    }
    if (m_window->audioPipelineEnabled()) {
        m_audioTab = std::make_unique<jcut::VulkanAudioTab>();
        if (!m_audioTab->initialize(m_window->physicalDevice(),
                                    m_window->device(),
                                    m_devFuncs,
                                    m_window->defaultRenderPass(),
                                    &error)) {
            if (m_owner) {
                m_owner->markFailure(error.isEmpty()
                                         ? QStringLiteral("Failed to initialize Vulkan audio waveform pipeline.")
                                         : error);
            }
            return;
        }
    }
}

DirectVulkanPreviewRenderer::~DirectVulkanPreviewRenderer()
{
    destroyCompositeTarget();
    destroyReadbackSlots();
}

void DirectVulkanPreviewRenderer::releaseResources()
{
    // Swapchain lifetime: only objects tied to its extent or render pass are
    // rebuilt. Device-level descriptors, imported images/semaphores, and
    // per-clip handoff allocations remain valid across ordinary resizes.
    destroyCompositeTarget();
    destroyReadbackSlots();
    m_audioTab.reset();
    m_temporalDebugTextRenderer.reset();
    m_speakerTextRenderer.reset();
    m_textRenderer.reset();
    m_pipeline.reset();
    m_lastPreparedTextKey.clear();
    m_lastPreparedTextReady = false;
}

void DirectVulkanPreviewRenderer::releaseDeviceResources()
{
    // The queue is idle when cleanupDevice reaches this method, so every
    // device-owned import and cached allocation can be retired exactly once.
    destroyGpuExportPreviewResources();
    for (auto it = m_clipHandoffResources.begin(); it != m_clipHandoffResources.end(); ++it) {
        releaseClipHandoffResources(it.value());
    }
    for (const RetiredClipHandoffResources& retired : m_retiredClipHandoffResources) {
        releaseClipHandoffResources(retired.resources);
    }
    m_clipHandoffResources.clear();
    m_retiredClipHandoffResources.clear();
    m_playbackStatusOverlayResources.reset();
    m_playbackStatusOverlayTextureKey.clear();
    m_playbackStatusOverlayTextureReady = false;
    m_resources.reset();
    m_importSemaphoreFd = nullptr;
    m_devFuncs = nullptr;
}

DirectVulkanPreviewRenderer::ClipHandoffResources*
DirectVulkanPreviewRenderer::ensureClipHandoffResources(const QString& clipId)
{
    if (clipId.trimmed().isEmpty() || !m_window || !m_devFuncs) {
        return nullptr;
    }
    auto existing = m_clipHandoffResources.find(clipId);
    if (existing != m_clipHandoffResources.end()) {
        return existing.value().get();
    }

    for (auto it = m_retiredClipHandoffResources.begin(); it != m_retiredClipHandoffResources.end(); ++it) {
        if (it->clipId != clipId || !it->resources) {
            continue;
        }
        std::shared_ptr<ClipHandoffResources> resources = it->resources;
        m_retiredClipHandoffResources.erase(it);
        m_clipHandoffResources.insert(clipId, resources);
        updateClipHandoffResourceStats();
        return resources.get();
    }

    auto resources = std::make_shared<ClipHandoffResources>();
    resources->resources = std::make_unique<VulkanResources>();
    if (!resources->resources->initialize(m_window->physicalDevice(), m_window->device(), m_devFuncs)) {
        if (m_owner) {
            m_owner->markFailure(QStringLiteral("Failed to initialize per-clip Vulkan handoff resources for %1.")
                                     .arg(clipId));
        }
        return nullptr;
    }
    resources->pipeline = std::make_unique<DirectVulkanFrameHandoffPipeline>();
    const jcut::vulkan_detector::VulkanDeviceContext handoffContext{
        m_window->physicalDevice(),
        m_window->device(),
        m_window->graphicsQueue(),
        m_window->graphicsQueueFamilyIndex()
    };
    QString handoffError;
    if (!resources->pipeline->initialize(handoffContext, &handoffError)) {
        qWarning().noquote()
            << QStringLiteral("[vulkan-preview] hardware frame handoff unavailable for clip %1: %2")
                   .arg(clipId, handoffError);
    }

    ClipHandoffResources* raw = resources.get();
    m_clipHandoffResources.insert(clipId, resources);
    updateClipHandoffResourceStats();
    return raw;
}

void DirectVulkanPreviewRenderer::pruneClipHandoffResources(const QSet<QString>& activeClipIds)
{
    for (auto it = m_clipHandoffResources.begin(); it != m_clipHandoffResources.end();) {
        if (activeClipIds.contains(it.key())) {
            ++it;
            continue;
        }
        if (it.value()) {
            m_retiredClipHandoffResources.push_back(RetiredClipHandoffResources{
                it.key(),
                it.value(),
                static_cast<int>(VulkanResources::kDescriptorSetCount) + 1});
        }
        it = m_clipHandoffResources.erase(it);
    }
    updateClipHandoffResourceStats();
}

void DirectVulkanPreviewRenderer::advanceRetiredClipHandoffResources()
{
    for (auto it = m_retiredClipHandoffResources.begin(); it != m_retiredClipHandoffResources.end();) {
        --it->framesRemaining;
        if (it->framesRemaining > 0) {
            ++it;
            continue;
        }
        releaseClipHandoffResources(it->resources);
        it = m_retiredClipHandoffResources.erase(it);
    }
    updateClipHandoffResourceStats();
}

void DirectVulkanPreviewRenderer::releaseClipHandoffResources(
    const std::shared_ptr<ClipHandoffResources>& resources)
{
    if (resources && resources->pipeline) {
        resources->pipeline->release();
    }
}

void DirectVulkanPreviewRenderer::updateClipHandoffResourceStats()
{
    if (DirectVulkanPreviewStats* stats = m_owner ? m_owner->stats() : nullptr) {
        stats->activeClipHandoffResourceCount = static_cast<int>(m_clipHandoffResources.size());
        stats->retiredClipHandoffResourceCount = static_cast<int>(m_retiredClipHandoffResources.size());
    }
}

uint32_t DirectVulkanPreviewRenderer::findMemoryType(uint32_t typeBits, VkMemoryPropertyFlags properties) const
{
    if (!m_window) {
        return UINT32_MAX;
    }
    auto getMemoryProperties = reinterpret_cast<PFN_vkGetPhysicalDeviceMemoryProperties>(
        m_window->vulkanInstance()
            ? m_window->vulkanInstance()->getInstanceProcAddr("vkGetPhysicalDeviceMemoryProperties")
            : nullptr);
    if (!getMemoryProperties) {
        return UINT32_MAX;
    }
    VkPhysicalDeviceMemoryProperties memoryProperties{};
    getMemoryProperties(m_window->physicalDevice(), &memoryProperties);
    for (uint32_t i = 0; i < memoryProperties.memoryTypeCount; ++i) {
        if ((typeBits & (1u << i)) &&
            (memoryProperties.memoryTypes[i].propertyFlags & properties) == properties) {
            return i;
        }
    }
    return UINT32_MAX;
}

void DirectVulkanPreviewRenderer::destroyCompositeTarget()
{
    if (!m_devFuncs || !m_window || !m_window->device()) {
        m_compositeSize = QSize();
        return;
    }
    const VkDevice device = m_window->device();
    if (m_compositeFramebuffer != VK_NULL_HANDLE) {
        m_devFuncs->vkDestroyFramebuffer(device, m_compositeFramebuffer, nullptr);
        m_compositeFramebuffer = VK_NULL_HANDLE;
    }
    if (m_compositeRenderPass != VK_NULL_HANDLE) {
        m_devFuncs->vkDestroyRenderPass(device, m_compositeRenderPass, nullptr);
        m_compositeRenderPass = VK_NULL_HANDLE;
    }
    if (m_compositeDepthView != VK_NULL_HANDLE) {
        m_devFuncs->vkDestroyImageView(device, m_compositeDepthView, nullptr);
        m_compositeDepthView = VK_NULL_HANDLE;
    }
    if (m_compositeDepthImage != VK_NULL_HANDLE) {
        m_devFuncs->vkDestroyImage(device, m_compositeDepthImage, nullptr);
        m_compositeDepthImage = VK_NULL_HANDLE;
    }
    if (m_compositeDepthMemory != VK_NULL_HANDLE) {
        m_devFuncs->vkFreeMemory(device, m_compositeDepthMemory, nullptr);
        m_compositeDepthMemory = VK_NULL_HANDLE;
    }
    if (m_compositeView != VK_NULL_HANDLE) {
        m_devFuncs->vkDestroyImageView(device, m_compositeView, nullptr);
        m_compositeView = VK_NULL_HANDLE;
    }
    if (m_compositeImage != VK_NULL_HANDLE) {
        m_devFuncs->vkDestroyImage(device, m_compositeImage, nullptr);
        m_compositeImage = VK_NULL_HANDLE;
    }
    if (m_compositeMemory != VK_NULL_HANDLE) {
        m_devFuncs->vkFreeMemory(device, m_compositeMemory, nullptr);
        m_compositeMemory = VK_NULL_HANDLE;
    }
    m_compositeSize = QSize();
    m_compositeColorFormat = VK_FORMAT_UNDEFINED;
    m_compositeDepthFormat = VK_FORMAT_UNDEFINED;
}

bool DirectVulkanPreviewRenderer::ensureCompositeTarget(const QSize& size,
                                                        VkFormat colorFormat,
                                                        VkFormat depthFormat)
{
    if (!m_window || !m_devFuncs || size.isEmpty() || colorFormat == VK_FORMAT_UNDEFINED) {
        return false;
    }
    if (m_compositeFramebuffer != VK_NULL_HANDLE &&
        m_compositeSize == size &&
        m_compositeColorFormat == colorFormat &&
        m_compositeDepthFormat == depthFormat) {
        return true;
    }
    destroyCompositeTarget();
    const VkDevice device = m_window->device();
    auto createImage = [&](VkFormat format,
                           VkImageUsageFlags usage,
                           VkImageAspectFlags aspect,
                           VkImage* image,
                           VkDeviceMemory* memory,
                           VkImageView* view) -> bool {
        VkImageCreateInfo imageInfo{};
        imageInfo.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
        imageInfo.imageType = VK_IMAGE_TYPE_2D;
        imageInfo.extent = {static_cast<uint32_t>(std::max(1, size.width())),
                            static_cast<uint32_t>(std::max(1, size.height())),
                            1};
        imageInfo.mipLevels = 1;
        imageInfo.arrayLayers = 1;
        imageInfo.format = format;
        imageInfo.tiling = VK_IMAGE_TILING_OPTIMAL;
        imageInfo.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        imageInfo.usage = usage;
        imageInfo.samples = VK_SAMPLE_COUNT_1_BIT;
        imageInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
        if (m_devFuncs->vkCreateImage(device, &imageInfo, nullptr, image) != VK_SUCCESS) {
            return false;
        }
        VkMemoryRequirements requirements{};
        m_devFuncs->vkGetImageMemoryRequirements(device, *image, &requirements);
        const uint32_t memoryType = findMemoryType(requirements.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
        if (memoryType == UINT32_MAX) {
            return false;
        }
        VkMemoryAllocateInfo alloc{};
        alloc.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
        alloc.allocationSize = requirements.size;
        alloc.memoryTypeIndex = memoryType;
        if (m_devFuncs->vkAllocateMemory(device, &alloc, nullptr, memory) != VK_SUCCESS ||
            m_devFuncs->vkBindImageMemory(device, *image, *memory, 0) != VK_SUCCESS) {
            return false;
        }
        VkImageViewCreateInfo viewInfo{};
        viewInfo.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
        viewInfo.image = *image;
        viewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
        viewInfo.format = format;
        viewInfo.subresourceRange.aspectMask = aspect;
        viewInfo.subresourceRange.baseMipLevel = 0;
        viewInfo.subresourceRange.levelCount = 1;
        viewInfo.subresourceRange.baseArrayLayer = 0;
        viewInfo.subresourceRange.layerCount = 1;
        return m_devFuncs->vkCreateImageView(device, &viewInfo, nullptr, view) == VK_SUCCESS;
    };
    if (!createImage(colorFormat,
                     VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
                     VK_IMAGE_ASPECT_COLOR_BIT,
                     &m_compositeImage,
                     &m_compositeMemory,
                     &m_compositeView)) {
        destroyCompositeTarget();
        return false;
    }
    const bool hasDepth = depthFormat != VK_FORMAT_UNDEFINED;
    if (hasDepth &&
        !createImage(depthFormat,
                     VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT,
                     VK_IMAGE_ASPECT_DEPTH_BIT,
                     &m_compositeDepthImage,
                     &m_compositeDepthMemory,
                     &m_compositeDepthView)) {
        destroyCompositeTarget();
        return false;
    }

    VkAttachmentDescription attachments[2]{};
    attachments[0].format = colorFormat;
    attachments[0].samples = VK_SAMPLE_COUNT_1_BIT;
    attachments[0].loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
    attachments[0].storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    attachments[0].stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    attachments[0].stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    attachments[0].initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    attachments[0].finalLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    VkAttachmentReference colorRef{};
    colorRef.attachment = 0;
    colorRef.layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    VkAttachmentReference depthRef{};
    if (hasDepth) {
        attachments[1].format = depthFormat;
        attachments[1].samples = VK_SAMPLE_COUNT_1_BIT;
        attachments[1].loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
        attachments[1].storeOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
        attachments[1].stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
        attachments[1].stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
        attachments[1].initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        attachments[1].finalLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
        depthRef.attachment = 1;
        depthRef.layout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
    }
    VkSubpassDescription subpass{};
    subpass.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
    subpass.colorAttachmentCount = 1;
    subpass.pColorAttachments = &colorRef;
    subpass.pDepthStencilAttachment = hasDepth ? &depthRef : nullptr;
    VkRenderPassCreateInfo renderPassInfo{};
    renderPassInfo.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO;
    renderPassInfo.attachmentCount = hasDepth ? 2u : 1u;
    renderPassInfo.pAttachments = attachments;
    renderPassInfo.subpassCount = 1;
    renderPassInfo.pSubpasses = &subpass;
    if (m_devFuncs->vkCreateRenderPass(device, &renderPassInfo, nullptr, &m_compositeRenderPass) != VK_SUCCESS) {
        destroyCompositeTarget();
        return false;
    }
    VkImageView views[2] = {m_compositeView, m_compositeDepthView};
    VkFramebufferCreateInfo framebufferInfo{};
    framebufferInfo.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
    framebufferInfo.renderPass = m_compositeRenderPass;
    framebufferInfo.attachmentCount = hasDepth ? 2u : 1u;
    framebufferInfo.pAttachments = views;
    framebufferInfo.width = static_cast<uint32_t>(std::max(1, size.width()));
    framebufferInfo.height = static_cast<uint32_t>(std::max(1, size.height()));
    framebufferInfo.layers = 1;
    if (m_devFuncs->vkCreateFramebuffer(device, &framebufferInfo, nullptr, &m_compositeFramebuffer) != VK_SUCCESS) {
        destroyCompositeTarget();
        return false;
    }
    m_compositeSize = size;
    m_compositeColorFormat = colorFormat;
    m_compositeDepthFormat = depthFormat;
    return true;
}

void DirectVulkanPreviewRenderer::destroyReadbackSlots()
{
    if (!m_window || !m_devFuncs) {
        m_readbackSlots.clear();
        return;
    }
    const VkDevice device = m_window->device();
    for (ReadbackSlot& slot : m_readbackSlots) {
        if (slot.buffer != VK_NULL_HANDLE) {
            m_devFuncs->vkDestroyBuffer(device, slot.buffer, nullptr);
        }
        if (slot.memory != VK_NULL_HANDLE) {
            m_devFuncs->vkFreeMemory(device, slot.memory, nullptr);
        }
    }
    m_readbackSlots.clear();
    for (ReadbackSlot& slot : m_decoderReadbackSlots) {
        if (slot.buffer != VK_NULL_HANDLE) {
            m_devFuncs->vkDestroyBuffer(device, slot.buffer, nullptr);
        }
        if (slot.memory != VK_NULL_HANDLE) {
            m_devFuncs->vkFreeMemory(device, slot.memory, nullptr);
        }
    }
    m_decoderReadbackSlots.clear();
}

bool DirectVulkanPreviewRenderer::ensureReadbackSlot(ReadbackSlot* slot, const QSize& size, VkFormat format)
{
    if (!slot || !m_window || !m_devFuncs || size.isEmpty()) {
        return false;
    }
    const VkDeviceSize bytes = static_cast<VkDeviceSize>(std::max(1, size.width())) *
                               static_cast<VkDeviceSize>(std::max(1, size.height())) * 4u;
    if (slot->buffer != VK_NULL_HANDLE && slot->size >= bytes && slot->imageSize == size && slot->format == format) {
        return true;
    }
    if (slot->buffer != VK_NULL_HANDLE) {
        m_devFuncs->vkDestroyBuffer(m_window->device(), slot->buffer, nullptr);
        slot->buffer = VK_NULL_HANDLE;
    }
    if (slot->memory != VK_NULL_HANDLE) {
        m_devFuncs->vkFreeMemory(m_window->device(), slot->memory, nullptr);
        slot->memory = VK_NULL_HANDLE;
    }

    VkBufferCreateInfo bufferInfo{};
    bufferInfo.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    bufferInfo.size = bytes;
    bufferInfo.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT;
    bufferInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    if (m_devFuncs->vkCreateBuffer(m_window->device(), &bufferInfo, nullptr, &slot->buffer) != VK_SUCCESS) {
        return false;
    }

    VkMemoryRequirements req{};
    m_devFuncs->vkGetBufferMemoryRequirements(m_window->device(), slot->buffer, &req);
    const uint32_t memoryType = findMemoryType(
        req.memoryTypeBits,
        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    if (memoryType == UINT32_MAX) {
        m_devFuncs->vkDestroyBuffer(m_window->device(), slot->buffer, nullptr);
        slot->buffer = VK_NULL_HANDLE;
        return false;
    }

    VkMemoryAllocateInfo alloc{};
    alloc.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    alloc.allocationSize = req.size;
    alloc.memoryTypeIndex = memoryType;
    if (m_devFuncs->vkAllocateMemory(m_window->device(), &alloc, nullptr, &slot->memory) != VK_SUCCESS ||
        m_devFuncs->vkBindBufferMemory(m_window->device(), slot->buffer, slot->memory, 0) != VK_SUCCESS) {
        if (slot->memory != VK_NULL_HANDLE) {
            m_devFuncs->vkFreeMemory(m_window->device(), slot->memory, nullptr);
            slot->memory = VK_NULL_HANDLE;
        }
        if (slot->buffer != VK_NULL_HANDLE) {
            m_devFuncs->vkDestroyBuffer(m_window->device(), slot->buffer, nullptr);
            slot->buffer = VK_NULL_HANDLE;
        }
        return false;
    }

    slot->size = bytes;
    slot->imageSize = size;
    slot->format = format;
    slot->pending = false;
    return true;
}

QImage DirectVulkanPreviewRenderer::imageFromReadback(const uchar* bytes, const QSize& size, VkFormat format) const
{
    if (!bytes || size.isEmpty()) {
        return QImage();
    }
    QImage image(size, QImage::Format_RGBA8888);
    if (image.isNull()) {
        return QImage();
    }
    const int pixelCount = size.width() * size.height();
    uchar* out = image.bits();
    if (format == VK_FORMAT_B8G8R8A8_UNORM || format == VK_FORMAT_B8G8R8A8_SRGB) {
        for (int i = 0; i < pixelCount; ++i) {
            out[i * 4 + 0] = bytes[i * 4 + 2];
            out[i * 4 + 1] = bytes[i * 4 + 1];
            out[i * 4 + 2] = bytes[i * 4 + 0];
            out[i * 4 + 3] = bytes[i * 4 + 3];
        }
    } else if (format == VK_FORMAT_R8G8B8A8_UNORM || format == VK_FORMAT_R8G8B8A8_SRGB) {
        std::memcpy(out, bytes, static_cast<size_t>(pixelCount) * 4u);
    } else if (format == VK_FORMAT_A2B10G10R10_UNORM_PACK32 ||
               format == VK_FORMAT_A2R10G10B10_UNORM_PACK32) {
        const auto* words = reinterpret_cast<const uint32_t*>(bytes);
        for (int i = 0; i < pixelCount; ++i) {
            const uint32_t v = words[i];
            const uint32_t c0 = (v >> 0) & 0x3ffu;
            const uint32_t c1 = (v >> 10) & 0x3ffu;
            const uint32_t c2 = (v >> 20) & 0x3ffu;
            const uint32_t a = (v >> 30) & 0x3u;
            if (format == VK_FORMAT_A2B10G10R10_UNORM_PACK32) {
                out[i * 4 + 0] = static_cast<uchar>((c2 * 255u) / 1023u);
                out[i * 4 + 1] = static_cast<uchar>((c1 * 255u) / 1023u);
                out[i * 4 + 2] = static_cast<uchar>((c0 * 255u) / 1023u);
            } else {
                out[i * 4 + 0] = static_cast<uchar>((c0 * 255u) / 1023u);
                out[i * 4 + 1] = static_cast<uchar>((c1 * 255u) / 1023u);
                out[i * 4 + 2] = static_cast<uchar>((c2 * 255u) / 1023u);
            }
            out[i * 4 + 3] = static_cast<uchar>((a * 255u) / 3u);
        }
    } else {
        std::memcpy(out, bytes, static_cast<size_t>(pixelCount) * 4u);
    }
    return image;
}

void DirectVulkanPreviewRenderer::consumeReadbackSlot(ReadbackSlot* slot)
{
    if (!slot || !slot->pending || slot->memory == VK_NULL_HANDLE || !m_window || !m_devFuncs || !m_owner) {
        return;
    }
    void* mapped = nullptr;
    if (m_devFuncs->vkMapMemory(m_window->device(), slot->memory, 0, slot->size, 0, &mapped) != VK_SUCCESS || !mapped) {
        return;
    }
    const QImage image = imageFromReadback(static_cast<const uchar*>(mapped), slot->imageSize, slot->format);
    m_devFuncs->vkUnmapMemory(m_window->device(), slot->memory);
    if (!image.isNull()) {
        m_owner->setLatestVulkanReadbackImage(image);
    }
    slot->pending = false;
}

void DirectVulkanPreviewRenderer::consumeDecoderReadbackSlot(ReadbackSlot* slot)
{
    if (!slot || !slot->pending || slot->memory == VK_NULL_HANDLE || !m_window || !m_devFuncs || !m_owner) {
        return;
    }
    void* mapped = nullptr;
    if (m_devFuncs->vkMapMemory(m_window->device(), slot->memory, 0, slot->size, 0, &mapped) != VK_SUCCESS || !mapped) {
        return;
    }
    const QImage image = imageFromReadback(static_cast<const uchar*>(mapped), slot->imageSize, slot->format);
    m_devFuncs->vkUnmapMemory(m_window->device(), slot->memory);
    if (!image.isNull()) {
        m_owner->setLatestDecoderDiagnosticImage(image);
        if (DirectVulkanPreviewStats* stats = m_owner->stats()) {
            ++stats->decoderDiagnosticReadbackCopies;
            stats->lastDecoderDiagnosticReadbackSize = image.size();
            stats->lastDecoderDiagnosticReadbackFormat = vulkanFormatName(slot->format);
        }
    }
    slot->pending = false;
}

void DirectVulkanPreviewRenderer::recordSwapchainReadback(VkCommandBuffer cb, ReadbackSlot* slot, const QSize& swapSize)
{
    if (!slot || slot->buffer == VK_NULL_HANDLE || !m_window || !m_devFuncs || swapSize.isEmpty()) {
        return;
    }
    const int imageIndex = m_window->currentSwapChainImageIndex();
    if (imageIndex < 0) {
        return;
    }
    const VkImage image = m_window->swapChainImage(imageIndex);
    if (image == VK_NULL_HANDLE) {
        return;
    }

    VkImageMemoryBarrier toTransfer{};
    toTransfer.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    toTransfer.srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
    toTransfer.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
    toTransfer.oldLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
    toTransfer.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    toTransfer.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    toTransfer.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    toTransfer.image = image;
    toTransfer.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    toTransfer.subresourceRange.baseMipLevel = 0;
    toTransfer.subresourceRange.levelCount = 1;
    toTransfer.subresourceRange.baseArrayLayer = 0;
    toTransfer.subresourceRange.layerCount = 1;
    m_devFuncs->vkCmdPipelineBarrier(cb,
                                     VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
                                     VK_PIPELINE_STAGE_TRANSFER_BIT,
                                     0,
                                     0,
                                     nullptr,
                                     0,
                                     nullptr,
                                     1,
                                     &toTransfer);

    VkBufferImageCopy region{};
    region.bufferOffset = 0;
    region.bufferRowLength = 0;
    region.bufferImageHeight = 0;
    region.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    region.imageSubresource.mipLevel = 0;
    region.imageSubresource.baseArrayLayer = 0;
    region.imageSubresource.layerCount = 1;
    region.imageOffset = {0, 0, 0};
    region.imageExtent = {
        static_cast<uint32_t>(std::max(1, swapSize.width())),
        static_cast<uint32_t>(std::max(1, swapSize.height())),
        1u
    };
    m_devFuncs->vkCmdCopyImageToBuffer(cb,
                                       image,
                                       VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                                       slot->buffer,
                                       1,
                                       &region);

    VkImageMemoryBarrier toPresent = toTransfer;
    toPresent.srcAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
    toPresent.dstAccessMask = VK_ACCESS_MEMORY_READ_BIT;
    toPresent.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    toPresent.newLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
    m_devFuncs->vkCmdPipelineBarrier(cb,
                                     VK_PIPELINE_STAGE_TRANSFER_BIT,
                                     VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT,
                                     0,
                                     0,
                                     nullptr,
                                     0,
                                     nullptr,
                                     1,
                                     &toPresent);
    slot->pending = true;
}

void DirectVulkanPreviewRenderer::recordImageReadback(VkCommandBuffer cb,
                                                      ReadbackSlot* slot,
                                                      VkImage image,
                                                      VkImageLayout layout,
                                                      const QSize& size,
                                                      VkFormat format)
{
    if (!slot || slot->buffer == VK_NULL_HANDLE || image == VK_NULL_HANDLE ||
        !m_window || !m_devFuncs || size.isEmpty() || layout == VK_IMAGE_LAYOUT_UNDEFINED) {
        return;
    }

    VkImageMemoryBarrier toTransfer{};
    toTransfer.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    if (layout == VK_IMAGE_LAYOUT_GENERAL) {
        toTransfer.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
    } else if (layout == VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL) {
        toTransfer.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    } else {
        toTransfer.srcAccessMask = VK_ACCESS_SHADER_READ_BIT;
    }
    toTransfer.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
    toTransfer.oldLayout = layout;
    toTransfer.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    toTransfer.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    toTransfer.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    toTransfer.image = image;
    toTransfer.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    toTransfer.subresourceRange.baseMipLevel = 0;
    toTransfer.subresourceRange.levelCount = 1;
    toTransfer.subresourceRange.baseArrayLayer = 0;
    toTransfer.subresourceRange.layerCount = 1;
    m_devFuncs->vkCmdPipelineBarrier(cb,
                                     VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
                                     VK_PIPELINE_STAGE_TRANSFER_BIT,
                                     0,
                                     0,
                                     nullptr,
                                     0,
                                     nullptr,
                                     1,
                                     &toTransfer);

    VkBufferImageCopy region{};
    region.bufferOffset = 0;
    region.bufferRowLength = 0;
    region.bufferImageHeight = 0;
    region.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    region.imageSubresource.mipLevel = 0;
    region.imageSubresource.baseArrayLayer = 0;
    region.imageSubresource.layerCount = 1;
    region.imageOffset = {0, 0, 0};
    region.imageExtent = {
        static_cast<uint32_t>(std::max(1, size.width())),
        static_cast<uint32_t>(std::max(1, size.height())),
        1u
    };
    m_devFuncs->vkCmdCopyImageToBuffer(cb,
                                       image,
                                       VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                                       slot->buffer,
                                       1,
                                       &region);

    VkImageMemoryBarrier toOriginal = toTransfer;
    toOriginal.srcAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
    toOriginal.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
    toOriginal.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    toOriginal.newLayout = layout;
    m_devFuncs->vkCmdPipelineBarrier(cb,
                                     VK_PIPELINE_STAGE_TRANSFER_BIT,
                                     VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
                                     0,
                                     0,
                                     nullptr,
                                     0,
                                     nullptr,
                                     1,
                                     &toOriginal);
    slot->format = format;
    slot->pending = true;
}

bool DirectVulkanPreviewRenderer::renderGpuExportPreview(
    VkCommandBuffer commandBuffer)
{
    if (!m_owner || !m_window || !m_pipeline || !m_resources ||
        !m_importSemaphoreFd) {
        return false;
    }
    render_detail::OffscreenVulkanFrame frame;
    if (m_owner->takeGpuExportPreviewFrame(&frame)) {
        const int slotIndex = static_cast<int>(frame.bufferIndex);
        if (slotIndex < 0 ||
            slotIndex >= static_cast<int>(m_gpuExportPreviewSlots.size())) {
            return false;
        }
        GpuExportPreviewSlot& slot =
            m_gpuExportPreviewSlots[slotIndex];
        if (slot.initialized &&
            slot.producerSessionId != frame.producerSessionId) {
            vkQueueWaitIdle(m_window->graphicsQueue());
            if (slot.importer) {
                slot.importer->release();
                slot.importer.reset();
            }
            if (slot.ready != VK_NULL_HANDLE) {
                vkDestroySemaphore(
                    m_window->device(), slot.ready, nullptr);
            }
            if (slot.consumed != VK_NULL_HANDLE) {
                vkDestroySemaphore(
                    m_window->device(), slot.consumed, nullptr);
            }
            slot = {};
            if (m_gpuExportPreviewCurrentSlot == slotIndex) {
                m_gpuExportPreviewCurrentSlot = -1;
            }
        }
        if (!slot.initialized) {
            if (frame.readySemaphoreFd < 0 ||
                frame.consumedSemaphoreFd < 0) {
                return false;
            }
            slot.importer =
                std::make_unique<
                    jcut::vulkan_import::VulkanExternalFrameImportCore>();
            const jcut::vulkan_import::DeviceContext context{
                m_window->physicalDevice(),
                m_window->device(),
                m_window->graphicsQueue(),
                m_window->graphicsQueueFamilyIndex()};
            std::string error;
            if (!slot.importer->initialize(context, &error)) {
                close(frame.readySemaphoreFd);
                close(frame.consumedSemaphoreFd);
                return false;
            }
            VkSemaphoreCreateInfo semaphoreInfo{};
            semaphoreInfo.sType =
                VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;
            if (vkCreateSemaphore(m_window->device(),
                                  &semaphoreInfo,
                                  nullptr,
                                  &slot.ready) != VK_SUCCESS ||
                vkCreateSemaphore(m_window->device(),
                                  &semaphoreInfo,
                                  nullptr,
                                  &slot.consumed) != VK_SUCCESS) {
                close(frame.readySemaphoreFd);
                close(frame.consumedSemaphoreFd);
                return false;
            }
            VkImportSemaphoreFdInfoKHR import{};
            import.sType =
                VK_STRUCTURE_TYPE_IMPORT_SEMAPHORE_FD_INFO_KHR;
            import.handleType =
                VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_OPAQUE_FD_BIT;
            import.semaphore = slot.ready;
            import.fd = frame.readySemaphoreFd;
            if (m_importSemaphoreFd(m_window->device(), &import) !=
                VK_SUCCESS) {
                close(frame.readySemaphoreFd);
                close(frame.consumedSemaphoreFd);
                return false;
            }
            import.semaphore = slot.consumed;
            import.fd = frame.consumedSemaphoreFd;
            if (m_importSemaphoreFd(m_window->device(), &import) !=
                VK_SUCCESS) {
                close(frame.consumedSemaphoreFd);
                return false;
            }
            slot.producerSessionId = frame.producerSessionId;
            slot.initialized = true;
        }

        VkPipelineStageFlags waitStage = VK_PIPELINE_STAGE_TRANSFER_BIT;
        VkSubmitInfo waitForReady{};
        waitForReady.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
        waitForReady.waitSemaphoreCount = 1;
        waitForReady.pWaitSemaphores = &slot.ready;
        waitForReady.pWaitDstStageMask = &waitStage;
        if (vkQueueSubmit(m_window->graphicsQueue(),
                          1,
                          &waitForReady,
                          VK_NULL_HANDLE) != VK_SUCCESS) {
            return false;
        }

        const auto signalConsumed = [this, &slot, &frame]() {
            VkSubmitInfo signal{};
            signal.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
            signal.signalSemaphoreCount = 1;
            signal.pSignalSemaphores = &slot.consumed;
            const bool submitted =
                vkQueueSubmit(m_window->graphicsQueue(),
                              1,
                              &signal,
                              VK_NULL_HANDLE) == VK_SUCCESS;
            if (submitted && frame.consumptionState) {
                frame.consumptionState->completedGeneration.store(
                    frame.generation, std::memory_order_release);
            }
            return submitted;
        };
        std::string error;
        if (!slot.importer->importExternalFrame(frame, &error) ||
            !slot.importer->finishPendingCopy(nullptr, &error)) {
            signalConsumed();
            qWarning().noquote()
                << QStringLiteral(
                       "[vulkan-preview] GPU export preview import failed: %1")
                       .arg(QString::fromStdString(error));
            return false;
        }
        m_resources->beginFrameUploads(
            static_cast<size_t>(qMax(0, m_window->currentFrame())),
            static_cast<size_t>(
                qMax(1, m_window->concurrentFrameCount())));
        if (!m_resources->ensureCheckerTextureUploaded(commandBuffer) ||
            !m_resources->ensureAuxiliaryImagesReadable(commandBuffer)) {
            signalConsumed();
            return false;
        }
        const jcut::vulkan_import::ExternalImage image =
            slot.importer->externalImage();
        if (!m_resources->setSampledImage(
                image.imageView, image.imageLayout)) {
            signalConsumed();
            return false;
        }
        slot.generation = frame.generation;
        m_gpuExportPreviewCurrentSlot = slotIndex;
        m_window->signalSemaphoreWhenFrameCompletes(slot.consumed);
        m_pendingGpuExportPreviewConsumptionState =
            frame.consumptionState;
        m_pendingGpuExportPreviewGeneration = frame.generation;
        m_pendingGpuExportPreviewSlot = slotIndex;
        m_pendingGpuExportPreviewProducerSessionId =
            frame.producerSessionId;
    }
    if (m_gpuExportPreviewCurrentSlot < 0) {
        return false;
    }

    const QSize size = m_window->swapChainImageSize();
    VkClearValue clears[2]{};
    clears[0].color.float32[0] = 0.055f;
    clears[0].color.float32[1] = 0.075f;
    clears[0].color.float32[2] = 0.105f;
    clears[0].color.float32[3] = 1.0f;
    clears[1].depthStencil.depth = 1.0f;
    VkRenderPassBeginInfo renderPass{};
    renderPass.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
    renderPass.renderPass = m_window->defaultRenderPass();
    renderPass.framebuffer = m_window->currentFramebuffer();
    renderPass.renderArea.extent = {
        static_cast<uint32_t>(qMax(1, size.width())),
        static_cast<uint32_t>(qMax(1, size.height()))};
    renderPass.clearValueCount =
        m_window->depthStencilFormat() == VK_FORMAT_UNDEFINED ? 1u : 2u;
    renderPass.pClearValues = clears;
    beginGpuExportPreviewRenderPass(commandBuffer, renderPass);
    VkViewport viewport{};
    viewport.width = static_cast<float>(qMax(1, size.width()));
    viewport.height = static_cast<float>(qMax(1, size.height()));
    viewport.maxDepth = 1.0f;
    VkRect2D scissor{};
    scissor.extent = {
        static_cast<uint32_t>(qMax(1, size.width())),
        static_cast<uint32_t>(qMax(1, size.height()))};
    VulkanPipeline::Push push{};
    m_pipeline->bindAndDraw(commandBuffer,
                            viewport,
                            scissor,
                            m_resources->descriptorSet(),
                            push);
    m_devFuncs->vkCmdEndRenderPass(commandBuffer);
    return true;
}

void DirectVulkanPreviewRenderer::destroyGpuExportPreviewResources()
{
    if (!m_window || m_window->device() == VK_NULL_HANDLE) {
        return;
    }
    vkQueueWaitIdle(m_window->graphicsQueue());
    for (GpuExportPreviewSlot& slot : m_gpuExportPreviewSlots) {
        if (slot.importer) {
            slot.importer->release();
            slot.importer.reset();
        }
        if (slot.ready != VK_NULL_HANDLE) {
            vkDestroySemaphore(
                m_window->device(), slot.ready, nullptr);
        }
        if (slot.consumed != VK_NULL_HANDLE) {
            vkDestroySemaphore(
                m_window->device(), slot.consumed, nullptr);
        }
        slot = {};
    }
    m_gpuExportPreviewCurrentSlot = -1;
    m_pendingGpuExportPreviewConsumptionState.reset();
    m_pendingGpuExportPreviewGeneration = 0;
    m_pendingGpuExportPreviewSlot = -1;
    m_pendingGpuExportPreviewProducerSessionId = 0;
}

void DirectVulkanPreviewRenderer::clearGpuExportPreview()
{
    destroyGpuExportPreviewResources();
}

void DirectVulkanPreviewRenderer::startNextFrame()
{
    if (m_owner) {
        m_owner->beginPreviewFrame();
    }
    if (!m_owner || !m_window || !m_devFuncs) {
        if (m_owner && m_owner->stats()) {
            editor::accumulatePlaybackStageMetric(&m_owner->stats()->commandRecordingStageMetric,
                                          1,
                                          0,
                                          1,
                                          QStringLiteral("source_unavailable"),
                                          QStringLiteral("renderer_or_device_unavailable"));
        }
        // The native presenter requires exactly one frameReady() for every
        // startNextFrame() invocation, including an unavailable renderer path.
        // Failing to complete this callback permanently stalls preview
        // presentation.
        if (m_window) {
            m_window->frameReady();
        }
        if (m_owner) {
            m_owner->markPreviewUpdateDelivered();
        }
        return;
    }
    VkCommandBuffer cb = m_window->currentCommandBuffer();
    if (renderGpuExportPreview(cb)) {
        m_owner->markPresented();
        const bool submitted = m_window->frameReady();
        if (submitted && m_pendingGpuExportPreviewConsumptionState) {
            m_pendingGpuExportPreviewConsumptionState->completedGeneration.store(
                m_pendingGpuExportPreviewGeneration,
                std::memory_order_release);
            if (m_pendingGpuExportPreviewGeneration == 1) {
                qInfo().noquote()
                    << QStringLiteral(
                           "[render-export-preview] consumed GPU slot=%1 "
                           "producer_session=%2")
                           .arg(m_pendingGpuExportPreviewSlot)
                           .arg(m_pendingGpuExportPreviewProducerSessionId);
            }
        }
        m_pendingGpuExportPreviewConsumptionState.reset();
        m_pendingGpuExportPreviewGeneration = 0;
        m_pendingGpuExportPreviewSlot = -1;
        m_pendingGpuExportPreviewProducerSessionId = 0;
        m_owner->markPreviewUpdateDelivered();
        if (m_owner->hasGpuExportPreviewFrames()) {
            m_owner->schedulePreviewUpdate();
        }
        return;
    }
    if (m_owner->stats()) {
        editor::accumulatePlaybackStageMetric(&m_owner->stats()->commandRecordingStageMetric,
                                      1,
                                      0,
                                      0,
                                      QStringLiteral("recording_started"),
                                      QStringLiteral("start_next_frame"));
        editor::accumulatePlaybackStageMetric(&m_owner->stats()->presentationStageMetric,
                                      1,
                                      0,
                                      0,
                                      QStringLiteral("present_pending"),
                                      QStringLiteral("start_next_frame"));
    }

    const PreviewInteractionState* liveState = m_owner->state();
    PreviewInteractionState renderSnapshot;
    if (liveState) {
        // Latch a per-frame render snapshot so UI/overlay/status updates cannot mutate command recording inputs.
        renderSnapshot = *liveState;
    }
    const PreviewInteractionState* state = liveState ? &renderSnapshot : nullptr;
    QColor base = state ? state->backgroundColor : QColor(Qt::black);
    if (!base.isValid()) {
        base = QColor(Qt::black);
    }

    const float phase = state
        ? std::fmod(static_cast<float>(state->currentFramePosition), 180.0f) / 179.0f
        : 0.25f;
    const float clipFactor = state
        ? qBound(0.0f, static_cast<float>(state->clipCount) / 8.0f, 1.0f)
        : 0.0f;
    const float motion = (state && state->playing) ? phase : 0.25f;

    VkClearValue clearValues[2]{};
    clearValues[0].color.float32[0] = 0.08f + 0.22f * motion;
    clearValues[0].color.float32[1] = 0.10f + 0.18f * clipFactor;
    clearValues[0].color.float32[2] = 0.13f + 0.35f * (1.0f - motion);
    clearValues[0].color.float32[3] = 1.0f;
    clearValues[1].depthStencil.depth = 1.0f;
    clearValues[1].depthStencil.stencil = 0;

    VkRenderPassBeginInfo rp{};
    rp.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
    const QSize swapSize = m_window->swapChainImageSize();
    rp.renderPass = m_window->defaultRenderPass();
    rp.framebuffer = m_window->currentFramebuffer();
    rp.renderArea.offset = {0, 0};
    rp.renderArea.extent = {static_cast<uint32_t>(std::max(1, swapSize.width())),
                            static_cast<uint32_t>(std::max(1, swapSize.height()))};
    rp.clearValueCount = m_window->depthStencilFormat() == VK_FORMAT_UNDEFINED ? 1u : 2u;
    rp.pClearValues = clearValues;

    const uint32_t swapchainImageIndex =
        static_cast<uint32_t>(std::max(0, m_window->currentSwapChainImageIndex()));
    for (ReadbackSlot& slot : m_readbackSlots) {
        consumeReadbackSlot(&slot);
    }
    advanceRetiredClipHandoffResources();
    struct DecoderReadbackCandidate {
        VkImage image = VK_NULL_HANDLE;
        VkImageLayout layout = VK_IMAGE_LAYOUT_UNDEFINED;
        QSize size;
        VkFormat format = VK_FORMAT_UNDEFINED;
    } decoderReadbackCandidate;
    QHash<QString, DirectVulkanFrameHandoffPipeline::Result> frameHandoffResults;
    QHash<QString, render_detail::VulkanGradePayload> gradePayloads;
    QHash<QString, bool> curveLutUploadResults;
    QHash<QString, bool> maskCurveLutUploadResults;
    QHash<QString, bool> maskUploadResults;
    QHash<QString, bool> frameCrossfadeMaskUploadResults;
    QHash<QString, bool> frameCrossfadeCurveLutUploadResults;
    QHash<QString, bool> frameCrossfadeMaskCurveLutUploadResults;
    struct PreparedOverlayTexture {
        VulkanResources* resources = nullptr;
        QRectF bounds;
        bool ready = false;
    };
    PreparedTranscriptOverlayMap preparedTranscriptOverlays;
    QHash<QString, EvaluatedTitle> prepared3DTitleOverlays;
    PreparedOverlayTexture preparedPlaybackStatusOverlay;
    qint64 mediaOwnerHandoffAttemptCount = 0;
    qint64 mediaOwnerHandoffSuccessCount = 0;
    const bool forceChecker = qEnvironmentVariableIntValue("JCUT_VULKAN_PREVIEW_FORCE_CHECKER") == 1;
    const bool canDrawTexture = m_resources && m_pipeline && m_resources->isReady() &&
                                m_pipeline->isReady();
    if (state && !forceChecker && canDrawTexture) {
        const QVector<MediaOwnerHandoffPlanEntry> handoffPlan =
            mediaOwnerHandoffPlan(state->vulkanFrameStatuses);
        QSet<QString> activeHandoffClipIds;
        for (const MediaOwnerHandoffPlanEntry& entry : handoffPlan) {
            activeHandoffClipIds.insert(entry.mediaOwnerClipId);
            if (entry.providerStatusIndex >= 0 &&
                entry.providerStatusIndex < state->vulkanFrameStatuses.size()) {
                const VulkanPreviewClipFrameStatus& providerStatus =
                    state->vulkanFrameStatuses.at(entry.providerStatusIndex);
                if (providerStatus.frameCrossfadeActive &&
                    !providerStatus.frameCrossfadeFrame.isNull()) {
                    activeHandoffClipIds.insert(
                        entry.mediaOwnerClipId + QStringLiteral("#frameCrossfade"));
                }
            }
            for (const int statusIndex : entry.consumerStatusIndices) {
                const VulkanPreviewClipFrameStatus& status =
                    state->vulkanFrameStatuses.at(statusIndex);
                activeHandoffClipIds.insert(status.clipId);
                if (status.frameCrossfadeActive) {
                    activeHandoffClipIds.insert(status.clipId + QStringLiteral("#frameCrossfade"));
                }
                if (status.differenceMatteEnabled) {
                    activeHandoffClipIds.insert(status.clipId + QStringLiteral("#differenceReference"));
                }
                for (int i = 0; i < status.temporalEchoFrames.size(); ++i) {
                    activeHandoffClipIds.insert(
                        status.clipId + QStringLiteral("#temporalEcho%1").arg(i));
                }
            }
        }
        pruneClipHandoffResources(activeHandoffClipIds);
        updateClipHandoffResourceStats();

        const auto prepareBaseMediaOwner = [&](
            const VulkanPreviewClipFrameStatus& providerStatus,
            ClipHandoffResources* mediaOwnerResources) {
            DirectVulkanFrameHandoffPipeline::Result ownerResult;
            if (!mediaOwnerResources || !mediaOwnerResources->resources ||
                !mediaOwnerResources->pipeline) {
                return ownerResult;
            }
            if (providerStatus.frame.hasCpuImage() &&
                !providerStatus.frame.hasHardwareFrame() &&
                !providerStatus.externalVulkanFrame) {
                ownerResult.attempted = true;
                const auto cpuBuffer =
                    providerStatus.frame.cpuImageBuffer();
                ownerResult.sampledFrameReady =
                    cpuBuffer &&
                    mediaOwnerResources->resources->uploadImageTexture(
                        cb, *cpuBuffer);
                ownerResult.descriptorSet = ownerResult.sampledFrameReady
                    ? mediaOwnerResources->resources->descriptorSet()
                    : VK_NULL_HANDLE;
                ownerResult.descriptorSetIndex = static_cast<int>(
                    mediaOwnerResources->resources->descriptorSetIndex());
                ownerResult.descriptorSetCount = static_cast<int>(
                    mediaOwnerResources->resources->descriptorSetCount());
                const QSize providerFrameSize = providerStatus.frameSize.isValid()
                    ? providerStatus.frameSize
                    : providerStatus.frame.size();
                ownerResult.size = {providerFrameSize.width(), providerFrameSize.height()};
                ownerResult.format = VK_FORMAT_R8G8B8A8_UNORM;
                if (ownerResult.sampledFrameReady) {
                    ownerResult.image = mediaOwnerResources->resources->sampledImage();
                    ownerResult.imageView = mediaOwnerResources->resources->sampledImageView();
                    ownerResult.layout = mediaOwnerResources->resources->sampledImageLayout();
                }
                if (DirectVulkanPreviewStats* stats = m_owner ? m_owner->stats() : nullptr) {
                    ++stats->handoffAttempts;
                    if (ownerResult.sampledFrameReady) {
                        ++stats->handoffSuccesses;
                        ++stats->sampledImageReady;
                        stats->lastHandoffError.clear();
                        stats->lastHandoffMode = QStringLiteral("cpu_image_upload");
                        stats->lastExternalImageSize = providerFrameSize;
                        stats->lastVulkanImageFormat =
                            jcut::direct_vulkan_preview::vulkanFormatName(ownerResult.format);
                        stats->descriptorSetIndex = ownerResult.descriptorSetIndex;
                        stats->descriptorSetCount = ownerResult.descriptorSetCount;
                    } else {
                        ++stats->handoffFailures;
                        stats->lastHandoffMode = QStringLiteral("cpu_image_upload_failed");
                        stats->lastHandoffError = QStringLiteral(
                            "Failed to upload CPU media-owner frame to Vulkan texture.");
                    }
                }
            } else {
                ownerResult = mediaOwnerResources->pipeline->record(
                    cb,
                    swapchainImageIndex,
                    providerStatus,
                    mediaOwnerResources->resources.get(),
                    m_owner ? m_owner->stats() : nullptr);
            }
            return ownerResult;
        };
        const auto frameCrossfadeHandoffStatus = [](
            const VulkanPreviewClipFrameStatus& status,
            const QString& handoffKey) {
            VulkanPreviewClipFrameStatus secondaryStatus = status;
            secondaryStatus.clipId = handoffKey;
            secondaryStatus.frame = status.frameCrossfadeFrame;
            secondaryStatus.frameSize = status.frameCrossfadeFrameSize;
            secondaryStatus.requestedSourceFrame =
                status.frameCrossfadeRequestedSourceFrame;
            secondaryStatus.presentedSourceFrame =
                status.frameCrossfadePresentedSourceFrame;
            secondaryStatus.hasFrame = !secondaryStatus.frame.isNull();
            secondaryStatus.externalVulkanFrame = false;
            secondaryStatus.externalImage = VK_NULL_HANDLE;
            secondaryStatus.externalImageView = VK_NULL_HANDLE;
            secondaryStatus.externalImageMemory = VK_NULL_HANDLE;
            secondaryStatus.externalReadySemaphoreFd = -1;
            return secondaryStatus;
        };

        // Upload/import each decoded media payload exactly once. A hidden
        // parent can be the provider even though it is never composited; when
        // that status was omitted, the first child carries its cloned payload.
        QHash<QString, DirectVulkanFrameHandoffPipeline::Result> mediaOwnerHandoffResults;
        for (const MediaOwnerHandoffPlanEntry& entry : handoffPlan) {
            if (entry.providerStatusIndex < 0 ||
                entry.providerStatusIndex >= state->vulkanFrameStatuses.size()) {
                continue;
            }
            const VulkanPreviewClipFrameStatus& providerStatus =
                state->vulkanFrameStatuses.at(entry.providerStatusIndex);
            ClipHandoffResources* mediaOwnerResources =
                ensureClipHandoffResources(entry.mediaOwnerClipId);
            if (!mediaOwnerResources || !mediaOwnerResources->resources ||
                !mediaOwnerResources->pipeline ||
                !mediaOwnerResources->resources->beginFrameUploads(
                    swapchainImageIndex,
                    qMax<size_t>(VulkanResources::kDescriptorSetCount,
                                 static_cast<size_t>(swapchainImageIndex) + 1))) {
                continue;
            }
            DirectVulkanFrameHandoffPipeline::Result ownerResult =
                prepareBaseMediaOwner(providerStatus, mediaOwnerResources);
            mediaOwnerHandoffAttemptCount += ownerResult.attempted ? 1 : 0;
            mediaOwnerHandoffSuccessCount += ownerResult.sampledFrameReady ? 1 : 0;
            mediaOwnerHandoffResults.insert(entry.mediaOwnerClipId, ownerResult);
            frameHandoffResults.insert(entry.mediaOwnerClipId, ownerResult);
            mediaOwnerResources->resources->ensureAuxiliaryImagesReadable(cb);

            if (providerStatus.frameCrossfadeActive &&
                !providerStatus.frameCrossfadeFrame.isNull()) {
                const QString ownerSecondaryKey =
                    entry.mediaOwnerClipId + QStringLiteral("#frameCrossfade");
                ClipHandoffResources* ownerSecondaryResources =
                    ensureClipHandoffResources(ownerSecondaryKey);
                if (ownerSecondaryResources && ownerSecondaryResources->resources &&
                    ownerSecondaryResources->pipeline &&
                    ownerSecondaryResources->resources->beginFrameUploads(
                        swapchainImageIndex,
                        qMax<size_t>(VulkanResources::kDescriptorSetCount,
                                     static_cast<size_t>(swapchainImageIndex) + 1))) {
                    const VulkanPreviewClipFrameStatus ownerSecondaryStatus =
                        frameCrossfadeHandoffStatus(providerStatus, ownerSecondaryKey);
                    DirectVulkanFrameHandoffPipeline::Result secondaryOwnerResult =
                        prepareBaseMediaOwner(ownerSecondaryStatus, ownerSecondaryResources);
                    mediaOwnerHandoffAttemptCount += secondaryOwnerResult.attempted ? 1 : 0;
                    mediaOwnerHandoffSuccessCount +=
                        secondaryOwnerResult.sampledFrameReady ? 1 : 0;
                    mediaOwnerHandoffResults.insert(ownerSecondaryKey, secondaryOwnerResult);
                    frameHandoffResults.insert(ownerSecondaryKey, secondaryOwnerResult);
                    ownerSecondaryResources->resources->ensureAuxiliaryImagesReadable(cb);
                }
            }
        }

        for (const MediaOwnerHandoffPlanEntry& entry : handoffPlan) {
            if (entry.providerStatusIndex < 0 ||
                entry.providerStatusIndex >= state->vulkanFrameStatuses.size()) {
                continue;
            }
            const VulkanPreviewClipFrameStatus& providerStatus =
                state->vulkanFrameStatuses.at(entry.providerStatusIndex);
            const DirectVulkanFrameHandoffPipeline::Result ownerResult =
                mediaOwnerHandoffResults.value(entry.mediaOwnerClipId);
            for (const int statusIndex : entry.consumerStatusIndices) {
                const VulkanPreviewClipFrameStatus& status =
                    state->vulkanFrameStatuses.at(statusIndex);
                const render_detail::VulkanGradePayload gradePayload =
                    render_detail::vulkanGradePayloadForGrade(status.grading);
                gradePayloads.insert(status.clipId, gradePayload);
                const QString mediaOwnerId = entry.mediaOwnerClipId;
            // A virtual mask child may reuse the parent's decoded image, but
            // it must own a descriptor set so its mask and curve bindings
            // cannot alter the parent's draw.
            const QString handoffResourceId = status.clipId;
            ClipHandoffResources* handoffResources = ensureClipHandoffResources(handoffResourceId);
            if (!handoffResources || !handoffResources->resources || !handoffResources->pipeline) {
                continue;
            }
            DirectVulkanFrameHandoffPipeline::Result handoffResult;
            if (handoffResourceId == mediaOwnerId) {
                handoffResult = ownerResult;
            } else {
                if (!handoffResources->resources->beginFrameUploads(
                        swapchainImageIndex,
                        qMax<size_t>(VulkanResources::kDescriptorSetCount,
                                     static_cast<size_t>(swapchainImageIndex) + 1))) {
                    continue;
                }
                const bool reusesParentMedia =
                    mediaOwnerPayloadMatches(providerStatus, status) &&
                    ownerResult.sampledFrameReady &&
                    ownerResult.imageView != VK_NULL_HANDLE;
                handoffResult = ownerResult;
                handoffResult.attempted = false;
                handoffResult.sampledFrameReady = reusesParentMedia &&
                    handoffResources->resources->setSampledImage(ownerResult.imageView,
                                                                  ownerResult.layout);
                handoffResult.descriptorSet = handoffResult.sampledFrameReady
                    ? handoffResources->resources->descriptorSet()
                    : VK_NULL_HANDLE;
                handoffResult.descriptorSetIndex =
                    static_cast<int>(handoffResources->resources->descriptorSetIndex());
                handoffResult.descriptorSetCount =
                    static_cast<int>(handoffResources->resources->descriptorSetCount());
            }
            frameHandoffResults.insert(status.clipId, handoffResult);
            const auto prepareAuxiliaryFrame = [&](const QString& key, const editor::FrameHandle& frame) {
                if (frame.isNull()) return;
                ClipHandoffResources* auxiliaryResources = ensureClipHandoffResources(key);
                if (!auxiliaryResources || !auxiliaryResources->resources || !auxiliaryResources->pipeline ||
                    !auxiliaryResources->resources->beginFrameUploads(
                        swapchainImageIndex,
                        qMax<size_t>(VulkanResources::kDescriptorSetCount,
                                     static_cast<size_t>(swapchainImageIndex) + 1))) return;
                VulkanPreviewClipFrameStatus auxiliaryStatus = status;
                auxiliaryStatus.clipId = key;
                auxiliaryStatus.frame = frame;
                auxiliaryStatus.frameSize = frame.size();
                auxiliaryStatus.hasFrame = true;
                auxiliaryStatus.externalVulkanFrame = false;
                DirectVulkanFrameHandoffPipeline::Result result;
                if (frame.hasCpuImage() && !frame.hasHardwareFrame()) {
                    result.attempted = true;
                    const auto cpuBuffer = frame.cpuImageBuffer();
                    result.sampledFrameReady =
                        cpuBuffer &&
                        auxiliaryResources->resources->uploadImageTexture(
                            cb, *cpuBuffer);
                    result.descriptorSet = result.sampledFrameReady
                        ? auxiliaryResources->resources->descriptorSet() : VK_NULL_HANDLE;
                    result.imageView = auxiliaryResources->resources->sampledImageView();
                    result.layout = auxiliaryResources->resources->sampledImageLayout();
                    result.size = {frame.size().width(), frame.size().height()};
                    result.format = VK_FORMAT_R8G8B8A8_UNORM;
                } else {
                    result = auxiliaryResources->pipeline->record(
                        cb, swapchainImageIndex, auxiliaryStatus,
                        auxiliaryResources->resources.get(), m_owner ? m_owner->stats() : nullptr);
                }
                frameHandoffResults.insert(key, result);
                auxiliaryResources->resources->ensureAuxiliaryImagesReadable(cb);
            };
            if (status.differenceMatteEnabled) {
                prepareAuxiliaryFrame(status.clipId + QStringLiteral("#differenceReference"),
                                      status.differenceReferenceFrame);
            }
            for (int i = 0; i < status.temporalEchoFrames.size(); ++i) {
                prepareAuxiliaryFrame(status.clipId + QStringLiteral("#temporalEcho%1").arg(i),
                                      status.temporalEchoFrames.at(i));
            }
            if (status.frameCrossfadeActive &&
                !status.frameCrossfadeFrame.isNull()) {
                const QString secondaryHandoffKey =
                    status.clipId + QStringLiteral("#frameCrossfade");
                const QString ownerSecondaryKey =
                    mediaOwnerId + QStringLiteral("#frameCrossfade");
                ClipHandoffResources* secondaryHandoffResources =
                    ensureClipHandoffResources(secondaryHandoffKey);
                if (secondaryHandoffResources &&
                    secondaryHandoffResources->resources &&
                    secondaryHandoffResources->pipeline) {
                    const VulkanPreviewClipFrameStatus secondaryStatus =
                        frameCrossfadeHandoffStatus(status, secondaryHandoffKey);
                    const VulkanPreviewClipFrameStatus providerSecondaryStatus =
                        frameCrossfadeHandoffStatus(providerStatus, ownerSecondaryKey);
                    const DirectVulkanFrameHandoffPipeline::Result secondaryOwnerResult =
                        mediaOwnerHandoffResults.value(ownerSecondaryKey);
                    DirectVulkanFrameHandoffPipeline::Result secondaryResult;
                    if (secondaryHandoffKey == ownerSecondaryKey) {
                        secondaryResult = secondaryOwnerResult;
                    } else if (secondaryHandoffResources->resources->beginFrameUploads(
                                   swapchainImageIndex,
                                   qMax<size_t>(VulkanResources::kDescriptorSetCount,
                                                static_cast<size_t>(swapchainImageIndex) + 1))) {
                        const bool reusesOwnerSecondary =
                            mediaOwnerPayloadMatches(providerSecondaryStatus, secondaryStatus) &&
                            secondaryOwnerResult.sampledFrameReady &&
                            secondaryOwnerResult.imageView != VK_NULL_HANDLE;
                        secondaryResult = secondaryOwnerResult;
                        secondaryResult.attempted = false;
                        secondaryResult.sampledFrameReady = reusesOwnerSecondary &&
                            secondaryHandoffResources->resources->setSampledImage(
                                secondaryOwnerResult.imageView,
                                secondaryOwnerResult.layout);
                        secondaryResult.descriptorSet = secondaryResult.sampledFrameReady
                            ? secondaryHandoffResources->resources->descriptorSet()
                            : VK_NULL_HANDLE;
                        secondaryResult.descriptorSetIndex = static_cast<int>(
                            secondaryHandoffResources->resources->descriptorSetIndex());
                        secondaryResult.descriptorSetCount = static_cast<int>(
                            secondaryHandoffResources->resources->descriptorSetCount());
                    } else {
                        secondaryResult = {};
                    }
                    frameHandoffResults.insert(secondaryHandoffKey, secondaryResult);
                    if (!status.maskClipSource &&
                        gradePayload.curveLutApplied) {
                        const QByteArray& secondaryCurveLut =
                            gradePayload.curveLutRgba;
                        if (!secondaryCurveLut.isEmpty()) {
                            frameCrossfadeCurveLutUploadResults.insert(
                                status.clipId,
                                secondaryHandoffResources->resources->uploadCurveLut(
                                    cb, secondaryCurveLut));
                        }
                    }
                    if (status.maskGradeEnabled && status.maskCurveLutApplied) {
                        const QByteArray secondaryMaskCurveLut =
                            curveLutRgbaBytes(status.maskGrade);
                        if (!secondaryMaskCurveLut.isEmpty()) {
                            frameCrossfadeMaskCurveLutUploadResults.insert(
                                status.clipId,
                                secondaryHandoffResources->resources->uploadMaskCurveLut(
                                    cb, secondaryMaskCurveLut));
                        }
                    }
                    if (status.frameCrossfadeMaskTextureEnabled &&
                        status.frameCrossfadeMaskBuffer) {
                        VulkanMaskPreprocessOptions secondaryMaskOptions;
                        secondaryMaskOptions.correctionStorage =
                            render_detail::vulkanMaskCorrectionStorageData(
                                status.correctionPolygons);
                        secondaryMaskOptions.outputSize =
                            status.frameCrossfadeFrameSize.isValid()
                                ? status.frameCrossfadeFrameSize
                                : status.frameSize;
                        secondaryMaskOptions.invert = status.maskInvert;
                        secondaryMaskOptions.erodeRadius =
                            qRound(qMax<qreal>(0.0, status.maskErode));
                        secondaryMaskOptions.dilateRadius =
                            qRound(qMax<qreal>(0.0, status.maskDilate));
                        secondaryMaskOptions.blurRadius = qRound(
                            qMax<qreal>(status.maskFeather, status.maskBlur));
                        frameCrossfadeMaskUploadResults.insert(
                            status.clipId,
                            secondaryHandoffResources->resources->uploadMaskTexture(
                                cb,
                                *status.frameCrossfadeMaskBuffer,
                                secondaryMaskOptions));
                    }
                    secondaryHandoffResources->resources->ensureAuxiliaryImagesReadable(cb);
                }
            }
            const QByteArray& curveLut = gradePayload.curveLutRgba;
            if (!status.maskClipSource && !curveLut.isEmpty()) {
                const bool uploaded =
                    handoffResources->resources->uploadCurveLut(cb, curveLut);
                curveLutUploadResults.insert(status.clipId, uploaded);
            }
            if (status.maskGradeEnabled && status.maskCurveLutApplied) {
                const QByteArray maskCurveLut = curveLutRgbaBytes(status.maskGrade);
                if (!maskCurveLut.isEmpty()) {
                    const bool uploaded =
                        handoffResources->resources->uploadMaskCurveLut(cb, maskCurveLut);
                    maskCurveLutUploadResults.insert(status.clipId, uploaded);
                }
            }
            if (status.maskTextureEnabled && status.maskBuffer) {
                VulkanMaskPreprocessOptions maskOptions;
                maskOptions.correctionStorage =
                    render_detail::vulkanMaskCorrectionStorageData(
                        status.correctionPolygons);
                maskOptions.outputSize = status.frameSize;
                maskOptions.invert = status.maskInvert;
                maskOptions.erodeRadius = qRound(qMax<qreal>(0.0, status.maskErode));
                maskOptions.dilateRadius = qRound(qMax<qreal>(0.0, status.maskDilate));
                maskOptions.blurRadius = qRound(qMax<qreal>(status.maskFeather, status.maskBlur));
                maskUploadResults.insert(
                    status.clipId,
                    handoffResources->resources->uploadMaskTexture(
                        cb, *status.maskBuffer, maskOptions));
            }
            handoffResources->resources->ensureAuxiliaryImagesReadable(cb);
            if (handoffResult.sampledFrameReady) {
                handoffResult.descriptorSet = handoffResources->resources->descriptorSet();
                handoffResult.descriptorSetIndex =
                    static_cast<int>(handoffResources->resources->descriptorSetIndex());
                frameHandoffResults.insert(status.clipId, handoffResult);
            }
            }
        }
        updateClipHandoffResourceStats();
    }
    const bool canDrawOverlays = m_pipeline && m_pipeline->isReady();
    if (state && canDrawOverlays) {
        TranscriptOverlayCollectionStats transcriptCollectionStats;
        preparedTranscriptOverlays =
            collectPreparedTranscriptOverlays(state, swapSize, &transcriptCollectionStats);
        if (DirectVulkanPreviewStats* stats = m_owner ? m_owner->stats() : nullptr) {
            stats->transcriptCandidateCount = transcriptCollectionStats.candidateCount;
            stats->transcriptPreparedCount = transcriptCollectionStats.preparedCount;
            stats->lastTranscriptSkipReason = transcriptCollectionStats.lastSkipReason;
            stats->lastTranscriptClipId = transcriptCollectionStats.lastPreparedClipId;
            stats->lastTranscriptPath = transcriptCollectionStats.lastPreparedTranscriptPath;
            stats->lastTranscriptTimingSource = transcriptCollectionStats.lastPreparedTimingSource;
            stats->lastTranscriptTimelineSample = transcriptCollectionStats.lastPreparedTimelineSample;
            stats->lastTranscriptFrame = transcriptCollectionStats.lastPreparedTranscriptFrame;
            stats->lastTranscriptPresentedMediaSourceFrame =
                transcriptCollectionStats.lastPreparedPresentedMediaSourceFrame;
        }
        QSet<QString> activeTitleClipIds;
        int titleCandidateCount = 0;
        int titlePreparedCount = 0;
        QString lastTitleSkipReason;
        QString lastTitleClipId;
        for (const TimelineClip& clip : state->clips) {
            if (clip.titleKeyframes.isEmpty()) {
                continue;
            }
            const bool inClipRange =
                state->currentFramePosition >= static_cast<qreal>(clip.startFrame) &&
                state->currentFramePosition < static_cast<qreal>(clip.startFrame + clip.durationFrames);
            const bool standaloneTitle = clip.mediaType == ClipMediaType::Title;
            if (!inClipRange ||
                (standaloneTitle && !clipVisualPlaybackEnabled(clip, state->tracks))) {
                continue;
            }
            ++titleCandidateCount;
            activeTitleClipIds.insert(clip.id);
            lastTitleClipId = clip.id;
            if (clip.titleKeyframes.isEmpty()) {
                lastTitleSkipReason = QStringLiteral("title_keyframes_empty");
                continue;
            }
            const EffectiveVisualEffects effects =
                evaluateEffectiveVisualEffectsAtPosition(
                    clip,
                    state->tracks,
                    state->currentFramePosition,
                    state->renderSyncMarkers,
                    state->playbackTiming);
            if (effects.grading.opacity <= 0.001) {
                lastTitleSkipReason = QStringLiteral("title_zero_opacity");
                continue;
            }
            const EvaluatedTitle title = prepareRenderableTitleForVulkanText(
                clip,
                state->currentFramePosition,
                state->playbackTiming,
                static_cast<qreal>(effects.grading.opacity),
                state->outputSize);
            if (!title.valid) {
                lastTitleSkipReason = QStringLiteral("title_evaluated_invisible");
                continue;
            }
            prepared3DTitleOverlays.insert(clip.id, title);
            ++titlePreparedCount;
            lastTitleSkipReason.clear();
            continue;
        }
        if (DirectVulkanPreviewStats* stats = m_owner ? m_owner->stats() : nullptr) {
            stats->titleCandidateCount = titleCandidateCount;
            stats->titlePreparedCount = titlePreparedCount;
            stats->lastTitleSkipReason = lastTitleSkipReason;
            stats->lastTitleClipId = lastTitleClipId;
        }
        if (m_playbackStatusOverlayResources &&
            m_playbackStatusOverlayResources->isReady() &&
            m_playbackStatusOverlayResources->descriptorSet() != VK_NULL_HANDLE) {
            const QString statusText = state->playbackStatusOverlayText.trimmed();
            if (!statusText.isEmpty()) {
                const qreal statusProgress = state->playbackStatusOverlayProgress;
                const QString textureKey = playbackStatusOverlayTextureKey(swapSize, statusText, statusProgress);
                bool textureReady =
                    m_playbackStatusOverlayTextureReady &&
                    textureKey == m_playbackStatusOverlayTextureKey;
                if (!kAllowCpuRasterTextOverlaysInDirectVulkanPreview) {
                    textureReady = false;
                    m_playbackStatusOverlayTextureKey.clear();
                    m_playbackStatusOverlayTextureReady = false;
                }
                if (!textureReady) {
                    if (kAllowCpuRasterTextOverlaysInDirectVulkanPreview) {
                        const render_detail::OverlayImage overlayImage =
                            renderPlaybackStatusOverlay(swapSize, statusText, statusProgress);
                        textureReady = !overlayImage.isNull() &&
                            m_playbackStatusOverlayResources->uploadImageTexture(cb, overlayImage);
                        if (textureReady) {
                            m_playbackStatusOverlayTextureKey = textureKey;
                            m_playbackStatusOverlayTextureReady = true;
                        }
                    }
                }
                if (textureReady) {
                    preparedPlaybackStatusOverlay = PreparedOverlayTexture{
                        m_playbackStatusOverlayResources.get(),
                        QRectF(QPointF(0.0, 0.0), QSizeF(swapSize)),
                        true};
                }
            } else {
                m_playbackStatusOverlayTextureKey.clear();
                m_playbackStatusOverlayTextureReady = false;
            }
        }
    } else if (DirectVulkanPreviewStats* stats = m_owner ? m_owner->stats() : nullptr) {
        stats->transcriptCandidateCount = 0;
        stats->transcriptPreparedCount = 0;
        stats->transcriptDrawnCount = 0;
        stats->titleCandidateCount = 0;
        stats->titlePreparedCount = 0;
        stats->titleDrawnCount = 0;
        stats->lastTitleSkipReason = QStringLiteral("overlays_unavailable");
        stats->lastTranscriptSkipReason = QStringLiteral("overlays_unavailable");
    }
    render_detail::SpeakerLabelOverlaySpec preparedSpeakerSpec;
    bool preparedSpeakerLabel = false;
    render_detail::SpeakerLabelOverlaySpec preparedTemporalDebugSpec;
    bool preparedTemporalDebugLabel = false;
    QSet<QString> preparedTranscriptAtlasClipIds;
    QString textPrepFailureReason;
    const size_t textFrameSlotCount =
        qMax<size_t>(VulkanResources::kDescriptorSetCount,
                     static_cast<size_t>(swapchainImageIndex) + 1);
    auto beginTextFrameUploads = [&](VulkanTextRenderer* renderer, const QString& failureReason) {
        if (renderer &&
            renderer->isReady() &&
            !renderer->beginFrameUploads(swapchainImageIndex, textFrameSlotCount) &&
            textPrepFailureReason.isEmpty()) {
            textPrepFailureReason = failureReason;
        }
    };
    beginTextFrameUploads(m_textRenderer.get(),
                          QStringLiteral("transcript:text_upload_frame_slot_unavailable"));
    if (m_textRenderer && m_textRenderer->isReady()) {
        for (auto it = prepared3DTitleOverlays.cbegin(); it != prepared3DTitleOverlays.cend(); ++it) {
            if (!m_textRenderer->prepareTitleOverlayAtlas(cb, state->outputSize, it.value())) {
                textPrepFailureReason = QStringLiteral("title:%1").arg(m_textRenderer->lastFailureReason());
            }
        }
    }
    beginTextFrameUploads(m_speakerTextRenderer.get(),
                          QStringLiteral("speaker:text_upload_frame_slot_unavailable"));
    beginTextFrameUploads(m_temporalDebugTextRenderer.get(),
                          QStringLiteral("debug:text_upload_frame_slot_unavailable"));
    if (m_speakerTextRenderer &&
        m_speakerTextRenderer->isReady() &&
        (state->showCurrentSpeakerName || state->showCurrentSpeakerOrganization)) {
        preparedSpeakerSpec = currentSpeakerLabelOverlaySpecForState(state);
        const bool hasVisibleLabel =
            (preparedSpeakerSpec.showName && !preparedSpeakerSpec.name.trimmed().isEmpty()) ||
            (preparedSpeakerSpec.showOrganization && !preparedSpeakerSpec.organization.trimmed().isEmpty());
        if (hasVisibleLabel) {
            preparedSpeakerLabel =
                m_speakerTextRenderer->prepareSpeakerLabelAtlas(cb, state->outputSize, preparedSpeakerSpec);
            if (!preparedSpeakerLabel) {
                textPrepFailureReason = QStringLiteral("speaker:%1")
                    .arg(m_speakerTextRenderer->lastFailureReason());
            }
        }
    }
    if (m_temporalDebugTextRenderer &&
        m_temporalDebugTextRenderer->isReady() &&
        !state->temporalDebugOverlayText.trimmed().isEmpty()) {
        preparedTemporalDebugSpec.name = QStringLiteral("TEMPORAL DEBUG");
        preparedTemporalDebugSpec.organization = state->temporalDebugOverlayText.trimmed();
        preparedTemporalDebugSpec.showName = true;
        preparedTemporalDebugSpec.showOrganization = true;
        preparedTemporalDebugSpec.nameTextScale = 0.42;
        preparedTemporalDebugSpec.organizationTextScale = 0.36;
        preparedTemporalDebugSpec.nameVerticalPosition = 0.07;
        preparedTemporalDebugSpec.organizationVerticalPosition = 0.18;
        preparedTemporalDebugSpec.nameColor = QColor(QStringLiteral("#fff4cc"));
        preparedTemporalDebugSpec.organizationColor = QColor(QStringLiteral("#d6e7f7"));
        preparedTemporalDebugSpec.backgroundColor = QColor(4, 8, 14, 218);
        preparedTemporalDebugSpec.borderColor = QColor(255, 209, 102, 170);
        preparedTemporalDebugLabel =
            m_temporalDebugTextRenderer->prepareSpeakerLabelAtlas(cb, state->outputSize, preparedTemporalDebugSpec);
        if (!preparedTemporalDebugLabel) {
            textPrepFailureReason = QStringLiteral("debug:%1")
                .arg(m_temporalDebugTextRenderer->lastFailureReason());
        }
    }
    QString textPrepMaterial = transcriptOverlayTextPrepMaterial(preparedTranscriptOverlays, state->outputSize);
    textPrepMaterial += QStringLiteral("s:%1:%2:%3:%4:%5|")
                            .arg(preparedSpeakerSpec.name)
                            .arg(preparedSpeakerSpec.organization)
                            .arg(preparedSpeakerSpec.showName ? 1 : 0)
                            .arg(preparedSpeakerSpec.showOrganization ? 1 : 0)
                            .arg(preparedSpeakerSpec.fontFamily);
    textPrepMaterial += QStringLiteral("d:%1|").arg(preparedTemporalDebugSpec.organization);
    const QString textPrepKey = QString::fromLatin1(
        QCryptographicHash::hash(textPrepMaterial.toUtf8(), QCryptographicHash::Sha1).toHex());
    const bool textPrepCacheHit =
        m_lastPreparedTextReady &&
        !textPrepKey.isEmpty() &&
        textPrepKey == m_lastPreparedTextKey;
    if (textPrepCacheHit) {
        for (auto it = preparedTranscriptOverlays.cbegin(); it != preparedTranscriptOverlays.cend(); ++it) {
            if (it.value().ready) {
                preparedTranscriptAtlasClipIds.insert(it.key());
            }
        }
        preparedSpeakerLabel =
            (preparedSpeakerSpec.showName && !preparedSpeakerSpec.name.trimmed().isEmpty()) ||
            (preparedSpeakerSpec.showOrganization && !preparedSpeakerSpec.organization.trimmed().isEmpty());
        preparedTemporalDebugLabel = !preparedTemporalDebugSpec.organization.trimmed().isEmpty();
    } else {
        if (m_textRenderer && m_textRenderer->isReady()) {
            for (auto it = preparedTranscriptOverlays.cbegin(); it != preparedTranscriptOverlays.cend(); ++it) {
                const PreparedTranscriptOverlay& transcript = it.value();
                if (!transcript.ready) {
                    continue;
                }
                if (m_textRenderer->prepareTranscriptOverlayAtlas(cb,
                                                                  state->outputSize,
                                                                  transcript.clip,
                                                                  transcript.layout,
                                                                  transcript.outputRect,
                                                                  transcript.speakerTitle)) {
                    preparedTranscriptAtlasClipIds.insert(it.key());
                } else {
                    textPrepFailureReason = QStringLiteral("transcript:%1")
                        .arg(m_textRenderer->lastFailureReason());
                }
            }
        } else if (!preparedTranscriptOverlays.isEmpty()) {
            textPrepFailureReason = QStringLiteral("transcript:text_renderer_not_ready");
        }
        m_lastPreparedTextKey = textPrepKey;
        m_lastPreparedTextReady =
            !preparedTranscriptAtlasClipIds.isEmpty() ||
            preparedSpeakerLabel ||
            preparedTemporalDebugLabel;
    }
    if (DirectVulkanPreviewStats* stats = m_owner ? m_owner->stats() : nullptr) {
        stats->lastTextPrepFailureReason = textPrepFailureReason;
        stats->lastTextDrawFailureReason.clear();
    }
    if (m_owner->stats()) {
        const qint64 textAttemptCount =
            preparedTranscriptOverlays.size() +
            prepared3DTitleOverlays.size() +
            ((preparedSpeakerSpec.showName || preparedSpeakerSpec.showOrganization) ? 1 : 0) +
            (!preparedTemporalDebugSpec.organization.trimmed().isEmpty() ? 1 : 0);
        const qint64 textSuccessCount =
            preparedTranscriptAtlasClipIds.size() +
            prepared3DTitleOverlays.size() +
            (preparedSpeakerLabel ? 1 : 0) +
            (preparedTemporalDebugLabel ? 1 : 0);
        editor::accumulatePlaybackStageMetric(&m_owner->stats()->textPrepStageMetric,
                                      qMax<qint64>(1, textAttemptCount),
                                      textSuccessCount,
                                      qMax<qint64>(0, textAttemptCount - textSuccessCount),
                                      textAttemptCount > 0
                                          ? (textPrepCacheHit
                                                 ? QStringLiteral("text_prepare_cache_hit")
                                                 : QStringLiteral("text_prepared"))
                                          : QStringLiteral("text_not_requested"),
                                      QStringLiteral("transcript=%1 title=%2 speaker=%3 debug=%4 cache_hit=%5")
                                          .arg(preparedTranscriptAtlasClipIds.size())
                                          .arg(prepared3DTitleOverlays.size())
                                          .arg(preparedSpeakerLabel ? 1 : 0)
                                          .arg(preparedTemporalDebugLabel ? 1 : 0)
                                          .arg(textPrepCacheHit ? 1 : 0));
    }
    bool renderPassBegun = false;
    const auto beginRenderPass = [&]() {
        if (!renderPassBegun) {
            m_devFuncs->vkCmdBeginRenderPass(cb, &rp, VK_SUBPASS_CONTENTS_INLINE);
            renderPassBegun = true;
        }
    };
    const bool directAudioMode = state && state->viewMode == PreviewSurface::ViewMode::Audio;
    if (!directAudioMode) beginRenderPass();
    auto drawPreparedOverlay = [&](const PreparedOverlayTexture& overlay) {
        if (!overlay.ready ||
            !overlay.resources ||
            overlay.resources->descriptorSet() == VK_NULL_HANDLE ||
            !m_pipeline ||
            !m_pipeline->isReady()) {
            return;
        }
        VkViewport viewport{};
        viewport.x = 0.0f;
        viewport.y = 0.0f;
        viewport.width = static_cast<float>(std::max(1, swapSize.width()));
        viewport.height = static_cast<float>(std::max(1, swapSize.height()));
        viewport.minDepth = 0.0f;
        viewport.maxDepth = 1.0f;
        const QRectF& bounds = overlay.bounds;
        PreviewClipGeometry overlayGeometry;
        overlayGeometry.localRect = QRectF(-bounds.width() / 2.0,
                                           -bounds.height() / 2.0,
                                           bounds.width(),
                                           bounds.height());
        overlayGeometry.clipToScreen.translate(bounds.center().x(), bounds.center().y());
        overlayGeometry.bounds = bounds;
        VulkanPipeline::Push overlayPush{};
        mvpForVulkanClipTransform(overlayGeometry.clipToScreen,
                                  overlayGeometry.localRect,
                                  swapSize,
                                  overlayPush.mvp);
        VkRect2D overlayScissor{};
        overlayScissor.offset = {0, 0};
        overlayScissor.extent = {static_cast<uint32_t>(std::max(1, swapSize.width())),
                                 static_cast<uint32_t>(std::max(1, swapSize.height()))};
        m_pipeline->bindAndDraw(cb,
                                viewport,
                                overlayScissor,
                                overlay.resources->descriptorSet(),
                                overlayPush);
    };
    auto drawPreparedTitleOverlayForClip = [&](const QString& clipId, const QRectF& compositeRect) -> bool {
        const auto title3DIt = prepared3DTitleOverlays.constFind(clipId);
        if (title3DIt != prepared3DTitleOverlays.constEnd()) {
            if (!m_textRenderer || !m_textRenderer->isReady()) return false;
            const bool drawn = m_textRenderer->drawTitleOverlay3D(
                cb, swapSize, state->outputSize, compositeRect, title3DIt.value());
            if (drawn && m_owner && m_owner->stats()) ++m_owner->stats()->titleDrawnCount;
            return drawn;
        }
        return false;
    };
    QSet<QString> drawnTranscriptOverlayClipIds;
    auto drawPreparedTranscriptOverlayForClip = [&](const QString& clipId, const QRectF& compositeRect) -> bool {
        const auto transcriptOverlayIt = preparedTranscriptOverlays.constFind(clipId);
        if (transcriptOverlayIt != preparedTranscriptOverlays.constEnd() &&
            transcriptOverlayIt.value().ready &&
            preparedTranscriptAtlasClipIds.contains(clipId) &&
            m_textRenderer &&
            m_textRenderer->isReady()) {
            const PreparedTranscriptOverlay& transcript = transcriptOverlayIt.value();
            const bool drawn = m_textRenderer->drawTranscriptOverlay(cb,
                                                                     swapSize,
                                                                     state->outputSize,
                                                                     compositeRect,
                                                                     transcript.clip,
                                                                     transcript.layout,
                                                                     transcript.outputRect,
                                                                     transcript.speakerTitle);
            if (drawn) {
                drawnTranscriptOverlayClipIds.insert(clipId);
                return true;
            }
            if (DirectVulkanPreviewStats* stats = m_owner ? m_owner->stats() : nullptr) {
                stats->lastTextDrawFailureReason = QStringLiteral("transcript:%1")
                    .arg(m_textRenderer->lastFailureReason());
            }
        }
        return false;
    };
    bool audioWaitingForWaveform = false;
    if (renderDirectVulkanAudioFrame(
            DirectVulkanAudioRenderContext{
                state, m_devFuncs, m_audioTab.get(), cb, swapSize, beginRenderPass},
            &audioWaitingForWaveform)) {
        drawPreparedOverlay(preparedPlaybackStatusOverlay);
        m_devFuncs->vkCmdEndRenderPass(cb);
        if (m_owner->stats()) {
            editor::accumulatePlaybackStageMetric(&m_owner->stats()->commandRecordingStageMetric,
                                          0,
                                          1,
                                          0,
                                          QStringLiteral("recorded"),
                                          QStringLiteral("audio_view_frame"));
        }
        m_owner->markPresentedSourceFrames(-1, -1);
        m_owner->markPresented(state);
        m_window->frameReady();
        m_owner->markPreviewUpdateDelivered();
        if (audioWaitingForWaveform && !state->playing) {
            m_owner->schedulePreviewUpdate();
        }
        return;
    }
    beginRenderPass();
    int64_t requestedSourceFrame = -1;
    int64_t presentedSourceFrame = -1;
    qint64 handoffAttemptCount = mediaOwnerHandoffAttemptCount;
    qint64 handoffSuccessCount = mediaOwnerHandoffSuccessCount;
    QSet<QString> submittedClipIds;
    QSet<QString> submittedCrossfadeClipIds;
    if (state) {
        VkViewport viewport{};
        viewport.x = 0.0f;
        viewport.y = 0.0f;
        viewport.width = static_cast<float>(std::max(1, swapSize.width()));
        viewport.height = static_cast<float>(std::max(1, swapSize.height()));
        viewport.minDepth = 0.0f;
        viewport.maxDepth = 1.0f;
        QHash<QString, PreviewClipGeometry> activeClipGeometry;
        const QRectF fullSwapRect(QPointF(0, 0), QSizeF(swapSize));
        const PreviewViewTransform viewTransform(fullSwapRect,
                                                 state->outputSize,
                                                 vulkanPreviewCanvasMarginPx(),
                                                 state->previewZoom,
                                                 state->previewPanOffset);
        const QRectF compositeRect = viewTransform.targetRect();
        const QPointF previewScale = viewTransform.outputScale();
        struct PendingMaskForegroundDraw {
            VkDescriptorSet descriptorSet = VK_NULL_HANDLE;
            VulkanPipeline::Push push;
            VkRect2D scissor{};
            uint32_t frameUniformDynamicOffset = 0;
        };
        QVector<PendingMaskForegroundDraw> pendingMaskForegroundDraws;
        VkClearValue canvasClear{};
        canvasClear.color.float32[0] = static_cast<float>(std::clamp<double>(base.redF(), 0.0, 1.0));
        canvasClear.color.float32[1] = static_cast<float>(std::clamp<double>(base.greenF(), 0.0, 1.0));
        canvasClear.color.float32[2] = static_cast<float>(std::clamp<double>(base.blueF(), 0.0, 1.0));
        canvasClear.color.float32[3] = 1.0f;
        clearRect(m_devFuncs, cb, canvasClear, clearRectFromQRect(compositeRect, swapSize));
        VkClearValue canvasBorder{};
        canvasBorder.color.float32[0] = 0.22f;
        canvasBorder.color.float32[1] = 0.28f;
        canvasBorder.color.float32[2] = 0.35f;
        canvasBorder.color.float32[3] = 1.0f;
        clearBoxOutline(m_devFuncs,
                        cb,
                        canvasBorder,
                        clearRectFromQRect(compositeRect.adjusted(-1, -1, 1, 1), swapSize),
                        std::max(1, std::min(swapSize.width(), swapSize.height()) / 360));
        QSet<QString> drawnTitleOverlayClipIds;
        for (const TimelineClip& clip : state->clips) {
            if (clip.mediaType == ClipMediaType::Title) {
                if (drawPreparedTitleOverlayForClip(clip.id, compositeRect)) {
                    drawnTitleOverlayClipIds.insert(clip.id);
                }
                if (clip.id == state->selectedClipId && state->titleOverlayInteractionOnly) {
                    const int selectionThickness = std::max(2, std::min(swapSize.width(), swapSize.height()) / 360);
                    clearBoxOutline(m_devFuncs,
                                    cb,
                                    selectionOutlineColor(),
                                    clearRectFromQRect(compositeRect, swapSize),
                                    selectionThickness);
                }
                continue;
            }
            const VulkanPreviewClipFrameStatus* status = frameStatusForClip(state, clip.id);
            if (!status || !status->active || status->drawSuppressed) {
                continue;
            }
            if (requestedSourceFrame < 0) {
                requestedSourceFrame = status->requestedSourceFrame;
            }
            if (status->maskClipSource) {
                const bool ownerMappingValid =
                    !status->mediaOwnerClipId.trimmed().isEmpty() &&
                    status->timingOwnerClipId == status->mediaOwnerClipId &&
                    status->effectsOwnerClipId == clip.id &&
                    status->matteOwnerClipId == clip.id;
                if (!ownerMappingValid) {
                    if (DirectVulkanPreviewStats* stats = m_owner->stats()) {
                        stats->lastUnsupportedEffect =
                            QStringLiteral("invalid_mask_owner_mapping");
                    }
                    continue;
                }
            }
            const bool selected = !state->selectedClipId.isEmpty() && clip.id == state->selectedClipId;
            VkClearAttachment attachment{};
            attachment.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
            attachment.colorAttachment = 0;
            attachment.clearValue = clipColorForStatus(clip, activeClipGeometry.size(), selected, status);
            const QSize frameSize = (status && status->frameSize.isValid()) ? status->frameSize : QSize();
            const QRectF fitted = viewTransform.fittedClipRect(clip.sourceFrameSize, frameSize);
            const TimelineClip::TransformKeyframe transform =
                transformWithTransientOverride(state, clip.id, status->transform);
            const PreviewClipGeometry clipGeometry =
                PreviewViewTransform::clipGeometry(
                    fitted,
                    previewScale,
                    QPointF(transform.translationX, transform.translationY),
                    transform.rotation,
                    QPointF(transform.scaleX, transform.scaleY));
            PreviewClipGeometry effectiveClipGeometry = clipGeometry;
            if (status->sampledFrameNeedsYFlip) {
                effectiveClipGeometry.clipToScreen.scale(1.0, -1.0);
                effectiveClipGeometry.bounds =
                    effectiveClipGeometry.clipToScreen.mapRect(effectiveClipGeometry.localRect);
            }
            const QRectF transformedBounds = effectiveClipGeometry.bounds;
            const VkClearRect rect = clearRectFromQRect(transformedBounds, swapSize);
            const DirectVulkanFrameHandoffPipeline::Result handoffResult =
                status ? frameHandoffResults.value(status->clipId) : DirectVulkanFrameHandoffPipeline::Result{};
            const DirectVulkanFrameHandoffPipeline::Result secondaryHandoffResult =
                status && status->frameCrossfadeActive
                    ? frameHandoffResults.value(status->clipId + QStringLiteral("#frameCrossfade"))
                    : DirectVulkanFrameHandoffPipeline::Result{};
            VulkanResources* sampledResources = nullptr;
            if (status) {
                const QString resourceId = status->clipId;
                auto resourcesIt = m_clipHandoffResources.constFind(resourceId);
                if (resourcesIt != m_clipHandoffResources.cend() && resourcesIt.value()) {
                    sampledResources = resourcesIt.value()->resources.get();
                }
            }
            const bool sampledFrameReady =
                handoffResult.sampledFrameReady && handoffResult.descriptorSet != VK_NULL_HANDLE;
            const QString mediaOwnerClipId = status
                ? status->mediaOwnerClipId.trimmed()
                : QString();
            const bool handoffAttempted = handoffResult.attempted ||
                (!mediaOwnerClipId.isEmpty() &&
                 frameHandoffResults.value(mediaOwnerClipId).attempted);
            if (sampledFrameReady) {
                decoderReadbackCandidate.image = handoffResult.image;
                decoderReadbackCandidate.layout = handoffResult.layout;
                decoderReadbackCandidate.size = toQSize(handoffResult.size);
                decoderReadbackCandidate.format = handoffResult.format;
            }
            const bool statusHasDrawableFrame = status && status->hasFrame;
            if (canDrawTexture && sampledFrameReady && statusHasDrawableFrame) {
                if (DirectVulkanPreviewStats* stats = m_owner->stats()) {
                    ++stats->textureDraws;
                    ++stats->activeClipDraws;
                }
                const TimelineClip* effectsOwner =
                    clipForId(state, status->effectsOwnerClipId);
                if (!effectsOwner) {
                    if (DirectVulkanPreviewStats* stats = m_owner->stats()) {
                        stats->lastUnsupportedEffect =
                            QStringLiteral("effects_owner_missing");
                    }
                    continue;
                }
                const TimelineClip effectClip = clipWithResolvedTimingOwner(
                    evaluateClipEffectAnimationAtPosition(
                        clipWithRenderableEffectSettings(
                            *effectsOwner, state->tracks),
                        state->currentFramePosition,
                        state->renderSyncMarkers,
                        state->playbackTiming),
                    state->clips);
                const bool clipEdgeFillEffect =
                    effectClip.edgeFillEffect != BackgroundFillEffect::None &&
                    render_detail::vulkanClipSupportsBackgroundFillSource(clip) &&
                    !(status && status->maskClipSource);
                const bool progressiveStretchOwnsClipBackground =
                    clipEdgeFillEffect &&
                    (effectClip.edgeFillEffect == BackgroundFillEffect::ProgressiveEdgeStretch ||
                     effectClip.edgeFillEffect ==
                         BackgroundFillEffect::ProgressiveBidirectionalEdgeStretch);
                PendingMaskForegroundDraw bidirectionalEdgeDraw;
                bool bidirectionalEdgeDrawPending = false;
                if (clipEdgeFillEffect) {
                    const BackgroundFillEffect effectiveFillEffect =
                        effectClip.edgeFillEffect;
                    const bool fullCanvasFill =
                        effectiveFillEffect == BackgroundFillEffect::EdgeStretch ||
                        effectiveFillEffect == BackgroundFillEffect::ProgressiveEdgeStretch ||
                        effectiveFillEffect ==
                            BackgroundFillEffect::ProgressiveBidirectionalEdgeStretch ||
                        effectiveFillEffect == BackgroundFillEffect::Tile ||
                        effectiveFillEffect == BackgroundFillEffect::Mirror;
                    PreviewClipGeometry backgroundGeometry =
                        fullCanvasFill ? PreviewViewTransform::clipGeometry(
                                             compositeRect,
                                             QPointF(1.0, 1.0),
                                             QPointF(),
                                             0.0,
                                             QPointF(1.0, 1.0))
                                       : effectiveClipGeometry;
                    if (effectiveFillEffect == BackgroundFillEffect::BlurCover) {
                        const qreal coverScale = std::max<qreal>(
                            1.0,
                            std::max(
                                compositeRect.width() / qMax<qreal>(1.0, effectiveClipGeometry.bounds.width()),
                                compositeRect.height() / qMax<qreal>(1.0, effectiveClipGeometry.bounds.height())));
                        backgroundGeometry.clipToScreen.scale(coverScale * 1.08, coverScale * 1.08);
                        backgroundGeometry.bounds =
                            backgroundGeometry.clipToScreen.mapRect(backgroundGeometry.localRect);
                    }
                    const bool progressiveRenderSpaceFill =
                        effectiveFillEffect == BackgroundFillEffect::ProgressiveEdgeStretch ||
                        effectiveFillEffect ==
                            BackgroundFillEffect::ProgressiveBidirectionalEdgeStretch;
                    const QSize renderOutputSize =
                        state->outputSize.isValid() ? state->outputSize : compositeRect.size().toSize();
                    const QSize renderSourceSize =
                        clip.sourceFrameSize.isValid()
                            ? clip.sourceFrameSize
                            : (frameSize.isValid() ? frameSize : renderOutputSize);
                    const QRectF renderOutputRect(QPointF(0.0, 0.0), QSizeF(renderOutputSize));
                    const QRectF renderFitted = render_detail::fitRectF(renderSourceSize, renderOutputSize);
                    PreviewClipGeometry renderClipGeometry =
                        PreviewViewTransform::clipGeometry(
                            renderFitted,
                            QPointF(1.0, 1.0),
                            QPointF(transform.translationX, transform.translationY),
                            transform.rotation,
                            QPointF(transform.scaleX, transform.scaleY));
                    if (status->sampledFrameNeedsYFlip) {
                        renderClipGeometry.clipToScreen.scale(1.0, -1.0);
                        renderClipGeometry.bounds =
                            renderClipGeometry.clipToScreen.mapRect(renderClipGeometry.localRect);
                    }
                    const render_detail::VulkanGradePayload gradePayload =
                        gradePayloads.value(status->clipId);
                    const render_detail::VulkanDrawEffectState& baseEffects =
                        gradePayload.effects;
                    VulkanPipeline::Push backgroundPush{};
                    // Progressive edge stretch is a clip effect, but its scan
                    // and edge band are defined in render/output pixels.  The
                    // preview transform only presents the already-defined
                    // output area; it must not change shader sampling.
                    uint32_t backgroundFrameUniformOffset = 0;
                    const float backgroundGrade[4] = {
                        baseEffects.brightness,
                        baseEffects.contrast,
                        baseEffects.saturation,
                        0.0f};
                    if (sampledResources &&
                        sampledResources->updateFrameUniform(progressiveRenderSpaceFill
                                                                 ? renderOutputSize
                                                                 : compositeRect.size().toSize(),
                                                             baseEffects.shadows,
                                                             baseEffects.midtones,
                                                             baseEffects.highlights,
                                                             backgroundGrade)) {
                        backgroundFrameUniformOffset = sampledResources->frameUniformDynamicOffset();
                    }
                    mvpForVulkanClipTransform(backgroundGeometry.clipToScreen,
                                              backgroundGeometry.localRect,
                                              swapSize,
                                              backgroundPush.mvp);
                    const int edgePixels =
                        qBound(1, effectClip.edgeFillPixels, 512);
                    const qreal edgePower =
                        qBound<qreal>(0.25, effectClip.edgeFillPower, 8.0);
                    const render_detail::VulkanDrawEffectState backgroundEffects =
                        render_detail::vulkanBackgroundFillEffectState(
                            effectiveFillEffect,
                            static_cast<float>(effectClip.edgeFillOpacity),
                            static_cast<float>(effectClip.edgeFillBrightness),
                            static_cast<float>(effectClip.edgeFillSaturation),
                            edgePixels,
                            static_cast<float>(edgePower),
                            status->frame.validTextureRectNormalized(),
                            progressiveRenderSpaceFill
                                ? render_detail::vulkanBackgroundFillMapping(
                                      renderClipGeometry.clipToScreen,
                                      renderClipGeometry.localRect,
                                      renderOutputRect)
                                : render_detail::vulkanBackgroundFillMapping(
                                      effectiveClipGeometry.clipToScreen,
                                      effectiveClipGeometry.localRect,
                                      compositeRect));
                    backgroundPush.opacity = backgroundEffects.opacity;
                    backgroundPush.brightness = backgroundEffects.brightness;
                    backgroundPush.contrast = backgroundEffects.contrast;
                    backgroundPush.saturation = backgroundEffects.saturation;
                    backgroundPush.shadows[0] = backgroundEffects.shadows[0];
                    backgroundPush.shadows[1] = backgroundEffects.shadows[1];
                    backgroundPush.shadows[2] = backgroundEffects.shadows[2];
                    backgroundPush.shadows[3] = backgroundEffects.shadows[3];
                    backgroundPush.midtones[0] = backgroundEffects.midtones[0];
                    backgroundPush.midtones[1] = backgroundEffects.midtones[1];
                    backgroundPush.midtones[2] = backgroundEffects.midtones[2];
                    backgroundPush.midtones[3] = backgroundEffects.midtones[3];
                    backgroundPush.highlights[0] = backgroundEffects.highlights[0];
                    backgroundPush.highlights[1] = backgroundEffects.highlights[1];
                    backgroundPush.highlights[2] = backgroundEffects.highlights[2];
                    backgroundPush.highlights[3] = backgroundEffects.highlights[3];
                    VkRect2D backgroundScissor{};
                    if (state->hideOutsideOutputWindow) {
                        backgroundScissor = scissorFromQRect(compositeRect, swapSize);
                    } else {
                        backgroundScissor.offset = {0, 0};
                        backgroundScissor.extent = {static_cast<uint32_t>(std::max(1, swapSize.width())),
                                                    static_cast<uint32_t>(std::max(1, swapSize.height()))};
                    }
                    if (effectiveFillEffect ==
                        BackgroundFillEffect::ProgressiveBidirectionalEdgeStretch) {
                        bidirectionalEdgeDraw = PendingMaskForegroundDraw{
                            handoffResult.descriptorSet,
                            backgroundPush,
                            backgroundScissor,
                            backgroundFrameUniformOffset};
                        bidirectionalEdgeDrawPending = true;
                    } else {
                        m_pipeline->bindAndDraw(cb,
                                                viewport,
                                                backgroundScissor,
                                                handoffResult.descriptorSet,
                                                backgroundPush,
                                                backgroundFrameUniformOffset);
                    }
                }
                VulkanPipeline::Push push{};
                mvpForVulkanClipTransform(effectiveClipGeometry.clipToScreen,
                                          effectiveClipGeometry.localRect,
                                          swapSize,
                                          push.mvp);
                if (status) {
                    const render_detail::VulkanGradePayload gradePayload =
                        gradePayloads.value(status->clipId);
                    const render_detail::VulkanDrawEffectState& effects =
                        gradePayload.effects;
                    push.brightness = effects.brightness;
                    push.contrast = effects.contrast;
                    push.saturation = effects.saturation;
                    push.opacity = effects.opacity;
                    push.shadows[0] = effects.shadows[0];
                    push.shadows[1] = effects.shadows[1];
                    push.shadows[2] = effects.shadows[2];
                    push.midtones[0] = effects.midtones[0];
                    push.midtones[1] = effects.midtones[1];
                    push.midtones[2] = effects.midtones[2];
                    push.highlights[0] = effects.highlights[0];
                    push.highlights[1] = effects.highlights[1];
                    push.highlights[2] = effects.highlights[2];
                    push.shadows[3] = gradePayload.curveLutApplied
                        ? render_detail::kVulkanEffectModeCurve
                        : render_detail::kVulkanEffectModeNormal;
                    push.midtones[3] = static_cast<float>(std::max<qreal>(0.0, status->maskFeather));
                    // Pack falloff profile and power into the otherwise unused
                    // positive mask parameter: profile * 10 + exponent.
                    push.highlights[3] = static_cast<float>(
                        qBound(0, status->maskFeatherFalloff, 5) * 10.0 +
                        std::clamp<qreal>(status->maskFeatherGamma, 0.1, 5.0));
                    if (DirectVulkanPreviewStats* stats = m_owner->stats()) {
                        stats->lastEffectsPath = status->effectsPath;
                        stats->lastTargetRect = compositeRect;
                        stats->lastFittedRect = fitted;
                        stats->lastAppliedBrightness = status->grading.brightness;
                        stats->lastAppliedContrast = status->grading.contrast;
                        stats->lastAppliedSaturation = status->grading.saturation;
                        stats->lastAppliedOpacity = status->grading.opacity;
                        stats->lastAppliedRotation = transform.rotation;
                        stats->lastAppliedScaleX = transform.scaleX;
                        stats->lastAppliedScaleY = transform.scaleY;
                        stats->lastCurveLutApplied = status->curveLutApplied;
                        if (status->correctionPolygonCount > 0 && !status->correctionsSupported) {
                            stats->lastUnsupportedEffect = QStringLiteral("correction_masks");
                        } else if (stats->lastUnsupportedEffect != QStringLiteral("curve_lut_upload_failed")) {
                            stats->lastUnsupportedEffect.clear();
                        }
                    }
                }
                VkRect2D scissor{};
                if (state->hideOutsideOutputWindow) {
                    scissor = scissorFromQRect(compositeRect, swapSize);
                } else {
                    scissor.offset = {0, 0};
                    scissor.extent = {static_cast<uint32_t>(std::max(1, swapSize.width())),
                                      static_cast<uint32_t>(std::max(1, swapSize.height()))};
                }
                if (sampledResources) {
                    sampledResources->updateFrameUniform(compositeRect.size().toSize());
                }
                auto drawPush = [&](const VulkanPipeline::Push& drawState) {
                    m_pipeline->bindAndDraw(cb,
                                            viewport,
                                            scissor,
                                            handoffResult.descriptorSet,
                                            drawState,
                                            sampledResources ? sampledResources->frameUniformDynamicOffset() : 0);
                };
                const bool maskReady =
                    status && status->maskTextureEnabled &&
                    maskUploadResults.value(status->clipId, false);
                const auto drawMaskShadow = [&](const VulkanPipeline::Push& maskedPush) {
                    if (!maskReady || !status || !status->maskDropShadowEnabled ||
                        status->maskDropShadowOpacity <= 0.0) {
                        return;
                    }
                    VulkanPipeline::Push shadowPush = maskedPush;
                    shadowPush.mvp[12] += static_cast<float>(
                        2.0 * status->maskDropShadowOffsetX /
                        std::max(1, swapSize.width()));
                    shadowPush.mvp[13] += static_cast<float>(
                        2.0 * status->maskDropShadowOffsetY /
                        std::max(1, swapSize.height()));
                    shadowPush.brightness = 0.0f;
                    shadowPush.contrast = 1.0f;
                    shadowPush.saturation = 1.0f;
                    shadowPush.opacity *= static_cast<float>(
                        std::clamp(status->maskDropShadowOpacity, 0.0, 1.0));
                    shadowPush.shadows[0] = 0.0f;
                    shadowPush.shadows[1] = 0.0f;
                    shadowPush.shadows[2] = 0.0f;
                    shadowPush.shadows[3] = render_detail::kVulkanEffectModeMaskShadow;
                    shadowPush.midtones[0] = 0.0f;
                    shadowPush.midtones[1] = 0.0f;
                    shadowPush.midtones[2] = 0.0f;
                    shadowPush.midtones[3] = static_cast<float>(
                        std::clamp(status->maskDropShadowRadius, 0.0, 200.0));
                    drawPush(shadowPush);
                };
                // A virtual mask clip is an explicitly masked overlay, never a
                // second ordinary media layer. If its matte is unavailable for
                // this frame, fail closed instead of grading the entire source
                // rectangle. The parent remains independently drawable.
                if (status->maskClipSource && !maskReady) {
                    if (DirectVulkanPreviewStats* stats = m_owner->stats()) {
                        stats->lastUnsupportedEffect = QStringLiteral("mask_texture_unavailable");
                    }
                    continue;
                }
                if (maskReady && status->maskShowOnly) {
                    VulkanPipeline::Push maskPush = push;
                    maskPush.brightness = 0.0f;
                    maskPush.contrast = 1.0f;
                    maskPush.saturation = 1.0f;
                    maskPush.opacity = static_cast<float>(std::clamp(status->maskOpacity, 0.0, 1.0));
                    maskPush.shadows[3] = render_detail::kVulkanEffectModeMaskOnly;
                    drawPush(maskPush);
                } else {
                    VulkanPipeline::Push basePush = push;
                    if (status && !status->maskClipSource && status->curveLutApplied &&
                        !curveLutUploadResults.value(status->clipId, false)) {
                        basePush.shadows[3] = render_detail::kVulkanEffectModeNormal;
                        if (DirectVulkanPreviewStats* stats = m_owner->stats()) {
                            stats->lastUnsupportedEffect = QStringLiteral("curve_lut_upload_failed");
                        }
                    }
                    if (maskReady && status->maskClipSource) {
                        basePush.shadows[3] = render_detail::kVulkanEffectModeMaskGrade;
                        basePush.opacity *= static_cast<float>(
                            std::clamp(status->maskOpacity, 0.0, 1.0));
                        basePush.midtones[3] = 0.0f;
                        if (status->maskGradeEnabled) {
                            basePush.brightness = static_cast<float>(status->maskGradeBrightness);
                            basePush.contrast = static_cast<float>(status->maskGradeContrast);
                            basePush.saturation = static_cast<float>(status->maskGradeSaturation);
                            if (maskCurveLutUploadResults.value(status->clipId, false)) {
                                basePush.midtones[3] = render_detail::kVulkanMaskGradeUseSelectedCurveLut;
                            } else if (status->maskCurveLutApplied) {
                                if (DirectVulkanPreviewStats* stats = m_owner->stats()) {
                                    stats->lastUnsupportedEffect = QStringLiteral("mask_curve_lut_upload_failed");
                                }
                            }
                        }
                        drawMaskShadow(basePush);
                    }
                    if (status && status->differenceMatteEnabled && sampledResources) {
                        const auto referenceResult = frameHandoffResults.value(
                            status->clipId + QStringLiteral("#differenceReference"));
                        if (referenceResult.sampledFrameReady && referenceResult.imageView != VK_NULL_HANDLE &&
                            sampledResources->bindAuxiliaryImage(referenceResult.imageView, referenceResult.layout)) {
                            basePush.shadows[3] = render_detail::kVulkanEffectModeDifferenceMatte;
                            basePush.midtones[3] = static_cast<float>(qBound<qreal>(0.0, status->differenceThreshold, 1.0));
                            basePush.highlights[3] = static_cast<float>(qBound<qreal>(0.0, status->differenceSoftness, 1.0));
                        }
                    }
                    TimelineClip foregroundEffectClip = effectClip;
                    if (progressiveStretchOwnsClipBackground) {
                        foregroundEffectClip.effectPreset = ClipEffectPreset::None;
                        foregroundEffectClip.maskRepeatEnabled = false;
                    }
                    const QRectF effectBounds =
                        (foregroundEffectClip.effectPreset == ClipEffectPreset::SourceTile ||
                         foregroundEffectClip.maskRepeatEnabled)
                            ? transformedBounds.intersected(compositeRect)
                            : compositeRect;
                    const render_detail::VulkanEffectPipelinePlan effectPlan =
                        render_detail::vulkanEffectPipelinePlan(
                            foregroundEffectClip,
                            effectBounds,
                            frameSize.isValid() ? frameSize : clip.sourceFrameSize,
                            status ? status->visualTimelineFramePosition : state->currentFramePosition,
                            render_detail::clipEffectPlaybackFramePosition(
                                foregroundEffectClip,
                                state->clips,
                                state->currentFramePosition,
                                state->playbackTiming,
                                state->tracks),
                            state->playbackTiming);
                    if (effectPlan.usesGeneratedDraws()) {
                        const VkRect2D generatedScissor =
                            foregroundEffectClip.effectPreset == ClipEffectPreset::SourceTile
                                ? scissorFromQRect(effectBounds, swapSize)
                                : scissor;
                        for (const render_detail::VulkanEffectPipelinePlan::DrawPass& effectDraw :
                             effectPlan.generatedDraws) {
                            VulkanPipeline::Push effectPush = basePush;
                            effectPush.opacity *= effectDraw.opacityMultiplier;
                            if (effectDraw.shaderMode == render_detail::kVulkanEffectModeMaskGrade &&
                                status && !status->maskClipSource) {
                                effectPush.opacity *= static_cast<float>(
                                    std::clamp(status->maskOpacity, 0.0, 1.0));
                            }
                            if (!status || !status->maskClipSource) {
                                effectPush.shadows[3] = effectDraw.shaderMode;
                            }
                            if (effectDraw.shaderMode >= render_detail::kVulkanEffectModeSpeakerMaskDilation &&
                                effectDraw.shaderMode <= render_detail::kVulkanEffectModeSpeakerMaskDilationRings) {
                                std::copy_n(effectDraw.palette, 3, effectPush.shadows);
                                std::copy_n(effectDraw.palette + 3, 3, effectPush.midtones);
                                std::copy_n(effectDraw.palette + 6, 3, effectPush.highlights);
                                effectPush.shadows[3] = effectDraw.shaderMode;
                            }
                            render_detail::vulkanMvpForOutputRectMaybeFlippedY(
                                effectDraw.outputRect,
                                swapSize,
                                effectDraw.rotationDegrees,
                                status && status->sampledFrameNeedsYFlip,
                                effectPush.mvp);
                            uint32_t effectUniformOffset =
                                sampledResources ? sampledResources->frameUniformDynamicOffset() : 0;
                            if (sampledResources && sampledResources->updateFrameUniform(
                                    swapSize, nullptr, nullptr, nullptr, effectDraw.effectParams)) {
                                effectUniformOffset = sampledResources->frameUniformDynamicOffset();
                            }
                            m_pipeline->bindAndDraw(cb,
                                                     viewport,
                                                     generatedScissor,
                                                     handoffResult.descriptorSet,
                                                     effectPush,
                                                     effectUniformOffset);
                        }
                    } else {
                        drawPush(basePush);
                    }
                    if (status && !status->temporalEchoFrames.isEmpty()) {
                        for (int echoIndex = 0; echoIndex < status->temporalEchoFrames.size(); ++echoIndex) {
                            const QString echoKey = status->clipId + QStringLiteral("#temporalEcho%1").arg(echoIndex);
                            const auto echoResult = frameHandoffResults.value(echoKey);
                            if (!echoResult.sampledFrameReady || echoResult.descriptorSet == VK_NULL_HANDLE) continue;
                            VulkanPipeline::Push echoPush = push;
                            echoPush.opacity *= static_cast<float>(std::pow(
                                qBound<qreal>(0.0, status->temporalEchoDecay, 1.0), echoIndex + 1));
                            auto echoResourcesIt = m_clipHandoffResources.constFind(echoKey);
                            uint32_t echoUniformOffset = 0;
                            if (echoResourcesIt != m_clipHandoffResources.cend() && echoResourcesIt.value() &&
                                echoResourcesIt.value()->resources &&
                                echoResourcesIt.value()->resources->updateFrameUniform(compositeRect.size().toSize())) {
                                echoUniformOffset = echoResourcesIt.value()->resources->frameUniformDynamicOffset();
                            }
                            m_pipeline->bindAndDraw(cb, viewport, scissor, echoResult.descriptorSet,
                                                    echoPush, echoUniformOffset);
                        }
                    }
                    const bool frameCrossfadeMaskReady =
                        !status || !status->maskTextureEnabled ||
                        (status->frameCrossfadeMaskTextureEnabled &&
                         frameCrossfadeMaskUploadResults.value(
                             status->clipId, false));
                    const bool frameCrossfadeCurveReady =
                        !status || status->maskClipSource ||
                        !status->curveLutApplied ||
                        frameCrossfadeCurveLutUploadResults.value(
                            status->clipId, false);
                    const bool frameCrossfadeMaskCurveReady =
                        !status || !status->maskCurveLutApplied ||
                        frameCrossfadeMaskCurveLutUploadResults.value(
                            status->clipId, false);
                    if (status && status->frameCrossfadeActive &&
                        secondaryHandoffResult.sampledFrameReady &&
                        secondaryHandoffResult.descriptorSet != VK_NULL_HANDLE &&
                        frameCrossfadeMaskReady &&
                        frameCrossfadeCurveReady &&
                        frameCrossfadeMaskCurveReady) {
                        VulkanPipeline::Push crossfadePush = basePush;
                        crossfadePush.opacity *= qBound(
                            0.0f, status->frameCrossfadeOpacity, 1.0f);
                        uint32_t secondaryFrameUniformOffset = 0;
                        if (status) {
                            const QString secondaryKey = status->clipId + QStringLiteral("#frameCrossfade");
                            auto secondaryResourcesIt = m_clipHandoffResources.constFind(secondaryKey);
                            if (secondaryResourcesIt != m_clipHandoffResources.cend() &&
                                secondaryResourcesIt.value() &&
                                secondaryResourcesIt.value()->resources &&
                                secondaryResourcesIt.value()->resources->updateFrameUniform(compositeRect.size().toSize())) {
                                secondaryFrameUniformOffset =
                                    secondaryResourcesIt.value()->resources->frameUniformDynamicOffset();
                            }
                        }
                        m_pipeline->bindAndDraw(cb,
                                                 viewport,
                                                 scissor,
                                                 secondaryHandoffResult.descriptorSet,
                                                 crossfadePush,
                                                 secondaryFrameUniformOffset);
                        submittedCrossfadeClipIds.insert(status->clipId);
                    }
                    if (maskReady && status->maskGradeEnabled &&
                        !status->maskForegroundLayerEnabled &&
                        !status->maskClipSource) {
                        VulkanPipeline::Push maskPush = push;
                        maskPush.brightness = static_cast<float>(status->maskGradeBrightness);
                        maskPush.contrast = static_cast<float>(status->maskGradeContrast);
                        maskPush.saturation = static_cast<float>(status->maskGradeSaturation);
                        maskPush.opacity = static_cast<float>(std::clamp(status->maskOpacity, 0.0, 1.0));
                        maskPush.shadows[0] = 0.0f;
                        maskPush.shadows[1] = 0.0f;
                        maskPush.shadows[2] = 0.0f;
                        maskPush.shadows[3] = render_detail::kVulkanEffectModeMaskGrade;
                        maskPush.midtones[0] = 0.0f;
                        maskPush.midtones[1] = 0.0f;
                        maskPush.midtones[2] = 0.0f;
                        maskPush.midtones[3] = 0.0f;
                        maskPush.highlights[0] = 0.0f;
                        maskPush.highlights[1] = 0.0f;
                        maskPush.highlights[2] = 0.0f;
                        maskPush.highlights[3] = 1.0f;
                        if (maskCurveLutUploadResults.value(status->clipId, false)) {
                            maskPush.midtones[3] = render_detail::kVulkanMaskGradeUseSelectedCurveLut;
                        } else if (status->maskCurveLutApplied) {
                            if (DirectVulkanPreviewStats* stats = m_owner->stats()) {
                                stats->lastUnsupportedEffect = QStringLiteral("mask_curve_lut_upload_failed");
                            }
                        }
                        drawPush(maskPush);
                    }
                    if (maskReady && status->maskForegroundLayerEnabled) {
                        VulkanPipeline::Push foregroundPush = push;
                        const bool applyMaskGradeToForeground = status->maskGradeEnabled;
                        foregroundPush.brightness = applyMaskGradeToForeground
                            ? static_cast<float>(status->maskGradeBrightness)
                            : 0.0f;
                        foregroundPush.contrast = applyMaskGradeToForeground
                            ? static_cast<float>(status->maskGradeContrast)
                            : 1.0f;
                        foregroundPush.saturation = applyMaskGradeToForeground
                            ? static_cast<float>(status->maskGradeSaturation)
                            : 1.0f;
                        foregroundPush.opacity = static_cast<float>(
                            std::clamp(status->maskOpacity, 0.0, 1.0));
                        foregroundPush.shadows[0] = 0.0f;
                        foregroundPush.shadows[1] = 0.0f;
                        foregroundPush.shadows[2] = 0.0f;
                        foregroundPush.shadows[3] = render_detail::kVulkanEffectModeMaskGrade;
                        foregroundPush.midtones[0] = 0.0f;
                        foregroundPush.midtones[1] = 0.0f;
                        foregroundPush.midtones[2] = 0.0f;
                        foregroundPush.midtones[3] = 0.0f;
                        if (applyMaskGradeToForeground) {
                            if (maskCurveLutUploadResults.value(status->clipId, false)) {
                                foregroundPush.midtones[3] = render_detail::kVulkanMaskGradeUseSelectedCurveLut;
                            } else if (status->maskCurveLutApplied) {
                                if (DirectVulkanPreviewStats* stats = m_owner->stats()) {
                                    stats->lastUnsupportedEffect = QStringLiteral("mask_curve_lut_upload_failed");
                                }
                            }
                        }
                        foregroundPush.highlights[0] = 0.0f;
                        foregroundPush.highlights[1] = 0.0f;
                        foregroundPush.highlights[2] = 0.0f;
                        foregroundPush.highlights[3] = push.highlights[3];
                        drawMaskShadow(foregroundPush);
                        pendingMaskForegroundDraws.push_back(
                            PendingMaskForegroundDraw{
                                handoffResult.descriptorSet,
                                foregroundPush,
                                scissor,
                                sampledResources ? sampledResources->frameUniformDynamicOffset() : 0});
                    }
                }
                if (bidirectionalEdgeDrawPending &&
                    bidirectionalEdgeDraw.descriptorSet != VK_NULL_HANDLE) {
                    m_pipeline->bindAndDraw(
                        cb,
                        viewport,
                        bidirectionalEdgeDraw.scissor,
                        bidirectionalEdgeDraw.descriptorSet,
                        bidirectionalEdgeDraw.push,
                        bidirectionalEdgeDraw.frameUniformDynamicOffset);
                }
                submittedClipIds.insert(status->clipId);
            } else {
                if (DirectVulkanPreviewStats* stats = m_owner->stats()) {
                    ++stats->explicitFailureDraws;
                    ++stats->clearFallbackDraws;
                    if (handoffAttempted && !sampledFrameReady) {
                        stats->lastHandoffMode = QStringLiteral("attempted_not_sampled");
                        stats->lastClearFallbackReason = QStringLiteral("handoff_attempted_not_sampled");
                    } else if (sampledFrameReady && !statusHasDrawableFrame) {
                        stats->lastHandoffMode = QStringLiteral("stale_sampled_resource_rejected");
                        stats->lastClearFallbackReason = QStringLiteral("stale_sampled_resource_rejected");
                        stats->lastHandoffError = status && !status->missingReason.isEmpty()
                            ? status->missingReason
                            : QStringLiteral("Retained Vulkan sampled image ignored because the active frame has no drawable payload.");
                    } else if (!status) {
                        stats->lastHandoffMode = QStringLiteral("decode_status_missing");
                        stats->lastClearFallbackReason = QStringLiteral("decode_status_missing");
                        stats->lastHandoffError = QStringLiteral("No Vulkan decode status exists for the active clip.");
                    } else if (!status->hasFrame) {
                        stats->lastHandoffMode = QStringLiteral("decoded_frame_unavailable");
                        stats->lastClearFallbackReason = QStringLiteral("decoded_frame_unavailable");
                        stats->lastHandoffError = status->missingReason.isEmpty()
                            ? QStringLiteral("Active Vulkan clip has no usable decoded frame.")
                            : status->missingReason;
                    } else if (status->frame.hasCpuImage() &&
                               !status->externalVulkanFrame &&
                               !status->frame.hasHardwareFrame()) {
                        stats->lastHandoffMode = QStringLiteral("vulkan_handoff_required");
                        stats->lastClearFallbackReason = QStringLiteral("vulkan_handoff_required");
                        stats->lastHandoffError = QStringLiteral(
                            "Direct Vulkan preview did not receive a drawable hardware or external Vulkan frame; CPU image upload is disabled.");
                    } else if (!canDrawTexture) {
                        stats->lastHandoffMode = QStringLiteral("texture_pipeline_unavailable");
                        stats->lastClearFallbackReason = QStringLiteral("texture_pipeline_unavailable");
                        stats->lastHandoffError = QStringLiteral("Vulkan texture pipeline or descriptor set is unavailable.");
                    }
                }
            }
            drawPreparedTranscriptOverlayForClip(clip.id, compositeRect);
            QRectF selectionBounds = transformedBounds;
            if (selected) {
                const QRectF transcriptBounds = transcriptOverlayBoundsForClip(state, clip, viewTransform);
                if (transcriptBounds.width() > 1.0 && transcriptBounds.height() > 1.0) {
                    selectionBounds = transcriptBounds;
                }
            }
            if (selected) {
                const int selectionThickness = std::max(2, std::min(swapSize.width(), swapSize.height()) / 360);
                clearBoxOutline(m_devFuncs,
                               cb,
                               selectionOutlineColor(),
                               clearRectFromQRect(selectionBounds, swapSize),
                               selectionThickness);
            }
            activeClipGeometry.insert(clip.id, effectiveClipGeometry);
            if (status && status->hasFrame && canDrawTexture && sampledFrameReady &&
                status->presentedSourceFrame >= presentedSourceFrame) {
                requestedSourceFrame = status->requestedSourceFrame;
                presentedSourceFrame = status->presentedSourceFrame;
            }
        }
        for (const PendingMaskForegroundDraw& draw : std::as_const(pendingMaskForegroundDraws)) {
            if (draw.descriptorSet != VK_NULL_HANDLE) {
                m_pipeline->bindAndDraw(cb,
                                        viewport,
                                        draw.scissor,
                                        draw.descriptorSet,
                                        draw.push,
                                        draw.frameUniformDynamicOffset);
            }
        }
        qint64 fallbackTranscriptDrawCount = 0;
        qint64 fallbackTitleDrawCount = 0;
        for (auto it = prepared3DTitleOverlays.cbegin(); it != prepared3DTitleOverlays.cend(); ++it) {
            if (!drawnTitleOverlayClipIds.contains(it.key()) &&
                m_textRenderer && m_textRenderer->isReady() &&
                m_textRenderer->drawTitleOverlay3D(cb, swapSize, state->outputSize,
                                                   compositeRect, it.value())) {
                drawnTitleOverlayClipIds.insert(it.key());
                ++fallbackTitleDrawCount;
            }
        }
        for (auto it = preparedTranscriptOverlays.cbegin(); it != preparedTranscriptOverlays.cend(); ++it) {
            if (!drawnTranscriptOverlayClipIds.contains(it.key())) {
                if (drawPreparedTranscriptOverlayForClip(it.key(), compositeRect)) {
                    ++fallbackTranscriptDrawCount;
                }
            }
        }
        if (m_owner->stats()) {
            const qint64 transcriptDrawAttempts = preparedTranscriptAtlasClipIds.size();
            const qint64 transcriptDrawSuccesses = drawnTranscriptOverlayClipIds.size();
            const qint64 titleDrawAttempts = prepared3DTitleOverlays.size();
            const qint64 titleDrawSuccesses = drawnTitleOverlayClipIds.size();
            m_owner->stats()->transcriptDrawnCount = static_cast<int>(transcriptDrawSuccesses);
            m_owner->stats()->titleDrawnCount = static_cast<int>(titleDrawSuccesses);
            editor::accumulatePlaybackStageMetric(&m_owner->stats()->textDrawStageMetric,
                                          transcriptDrawAttempts + titleDrawAttempts,
                                          transcriptDrawSuccesses + titleDrawSuccesses,
                                          qMax<qint64>(0, transcriptDrawAttempts + titleDrawAttempts -
                                                             transcriptDrawSuccesses - titleDrawSuccesses),
                                          (transcriptDrawAttempts + titleDrawAttempts) > 0
                                              ? QStringLiteral("text_draw_evaluated")
                                              : QStringLiteral("text_draw_not_requested"),
                                          QStringLiteral("transcript_prepared=%1 transcript_drawn=%2 fallback_drawn=%3 title_prepared=%4 title_drawn=%5 title_fallback_drawn=%6")
                                              .arg(transcriptDrawAttempts)
                                              .arg(transcriptDrawSuccesses)
                                              .arg(fallbackTranscriptDrawCount)
                                              .arg(titleDrawAttempts)
                                              .arg(titleDrawSuccesses)
                                              .arg(fallbackTitleDrawCount));
        }
        if (preparedSpeakerLabel && m_speakerTextRenderer && m_speakerTextRenderer->isReady()) {
            if (!m_speakerTextRenderer->drawSpeakerLabel(cb,
                                                         swapSize,
                                                         state->outputSize,
                                                         compositeRect,
                                                         preparedSpeakerSpec)) {
                if (DirectVulkanPreviewStats* stats = m_owner ? m_owner->stats() : nullptr) {
                    stats->lastTextDrawFailureReason = QStringLiteral("speaker:%1")
                        .arg(m_speakerTextRenderer->lastFailureReason());
                }
            }
        }
        if (preparedTemporalDebugLabel &&
            m_temporalDebugTextRenderer &&
            m_temporalDebugTextRenderer->isReady()) {
            m_temporalDebugTextRenderer->drawSpeakerLabel(cb,
                                                         swapSize,
                                                         state->outputSize,
                                                         compositeRect,
                                                         preparedTemporalDebugSpec);
        }
        drawPreparedOverlay(preparedPlaybackStatusOverlay);
        drawOutputPlacementGuides(m_devFuncs,
                                  cb,
                                  swapSize,
                                  state->outputSize,
                                  compositeRect,
                                  state->instagramSafeAreaGuides,
                                  state->alignmentGridGuides);
        const int thickness = std::max(2, std::min(swapSize.width(), swapSize.height()) / 180);
        for (const VulkanPreviewFacestreamOverlay& overlay : state->facedetectionsOverlays) {
            const auto it = activeClipGeometry.constFind(overlay.clipId);
            if (it == activeClipGeometry.constEnd() || !overlay.boxNorm.isValid()) {
                continue;
            }
            const PreviewClipGeometry& geometry = it.value();
            QRectF boxNorm = overlay.boxNorm;
            const bool hovered =
                overlay.trackId >= 0 &&
                state->transient.hoveredFaceDetectionsTrackId == overlay.trackId &&
                state->transient.hoveredFaceDetectionsClipId == overlay.clipId &&
                state->transient.hoveredFaceDetectionsId == overlay.streamId;
            if (hovered) {
                boxNorm = boxNorm.adjusted(-0.01, -0.01, 0.01, 0.01).intersected(QRectF(0.0, 0.0, 1.0, 1.0));
            }
            const VkClearRect boxRect = faceDetectionBoxToSwapchainRect(
                boxNorm,
                geometry.clipToScreen,
                geometry.localRect,
                swapSize);
            clearBoxOutline(
                m_devFuncs,
                cb,
                facedetectionsOverlayColor(state, overlay),
                boxRect,
                hovered ? qMax(thickness + 3, thickness * 2) : thickness);
        }
        for (const VulkanPreviewFacestreamOverlay& overlay : state->rawDetectionOverlays) {
            const auto it = activeClipGeometry.constFind(overlay.clipId);
            if (it == activeClipGeometry.constEnd() || !overlay.boxNorm.isValid()) {
                continue;
            }
            const PreviewClipGeometry& geometry = it.value();
            const VkClearRect boxRect = faceDetectionBoxToSwapchainRect(
                overlay.boxNorm,
                geometry.clipToScreen,
                geometry.localRect,
                swapSize);
            clearBoxOutline(m_devFuncs, cb, facedetectionsOverlayColor(state, overlay), boxRect, qMax(1, thickness - 1));
        }
        if (const TimelineClip* selectedClip = selectedClipForTargetBox(state)) {
            const TimelineClip::TransformKeyframe targetState =
                evaluateClipSpeakerFramingTargetAtFrame(*selectedClip, state->currentFrame);
            const qreal targetBoxNorm = qBound<qreal>(-1.0, targetState.scaleX, 1.0);
            if (targetBoxNorm > 0.0) {
                const int targetThickness = std::max(2, std::min(swapSize.width(), swapSize.height()) / 220);
                clearBoxOutline(
                    m_devFuncs,
                    cb,
                    targetBoxOverlayColor(),
                    targetBoxRectForComposite(*selectedClip, state->currentFrame, compositeRect, swapSize),
                    targetThickness);
            }
        }
    }
    m_devFuncs->vkCmdEndRenderPass(cb);
    if (m_owner->pipelineThumbnailReadbackPending()) {
        const int imageIndex = m_window->currentSwapChainImageIndex();
        if (imageIndex >= 0) {
            if (static_cast<int>(m_readbackSlots.size()) <= imageIndex) {
                m_readbackSlots.resize(static_cast<size_t>(imageIndex + 1));
            }
            ReadbackSlot& slot =
                m_readbackSlots[static_cast<size_t>(imageIndex)];
            if (ensureReadbackSlot(
                    &slot, swapSize, m_window->colorFormat())) {
                recordSwapchainReadback(cb, &slot, swapSize);
                m_owner->markPipelineThumbnailReadbackRecorded(swapSize);
            }
        }
    }
    if (m_owner->stats()) {
        editor::accumulatePlaybackStageMetric(&m_owner->stats()->gpuHandoffStageMetric,
                                      qMax<qint64>(1, handoffAttemptCount),
                                      handoffSuccessCount,
                                      qMax<qint64>(0, handoffAttemptCount - handoffSuccessCount),
                                      handoffAttemptCount > 0
                                          ? QStringLiteral("handoff_evaluated")
                                          : QStringLiteral("source_unavailable"),
                                      handoffAttemptCount > 0
                                          ? QStringLiteral("ready=%1").arg(handoffSuccessCount)
                                          : QStringLiteral("no_active_handoff_attempts"));
        editor::accumulatePlaybackStageMetric(&m_owner->stats()->commandRecordingStageMetric,
                                      0,
                                      1,
                                      0,
                                      QStringLiteral("recorded"),
                                      QStringLiteral("video_frame"));
    }

    m_owner->markPresentedSourceFrames(requestedSourceFrame, presentedSourceFrame);
    m_owner->markPresented(
        state, &submittedClipIds, &submittedCrossfadeClipIds);
    m_window->frameReady();
    m_owner->markPreviewUpdateDelivered();
}

void DirectVulkanPreviewRenderer::beginGpuExportPreviewRenderPass(
    VkCommandBuffer commandBuffer,
    const VkRenderPassBeginInfo& renderPass)
{
    m_devFuncs->vkCmdBeginRenderPass(
        commandBuffer, &renderPass, VK_SUBPASS_CONTENTS_INLINE);
}

void DirectVulkanPreviewRenderer::physicalDeviceLost()
{
    if (m_owner) {
        m_owner->markFailure(QStringLiteral("Physical Vulkan device lost during direct preview presentation."));
    }
}

void DirectVulkanPreviewRenderer::logicalDeviceLost()
{
    if (m_owner) {
        m_owner->markFailure(QStringLiteral("Logical Vulkan device lost during direct preview presentation."));
    }
}

QWidget* createDirectVulkanPreviewWindowContainer(DirectVulkanPreviewWindow* window,
                                                  QWidget* parent)
{
    return window ? QWidget::createWindowContainer(window, parent) : nullptr;
}

DirectVulkanPreviewWindow* createDirectVulkanPreviewWindow(
    PreviewInteractionState* state,
    DirectVulkanPresentationTelemetry* presentationTelemetry,
    DirectVulkanPreviewStats* stats,
    bool* active,
    QString* failureReason,
    bool enableAudioPipeline,
    std::function<void(const QString&)> failureCallback)
{
    return new DirectVulkanPreviewWindow(state,
                                         presentationTelemetry,
                                         stats,
                                         active,
                                         failureReason,
                                         enableAudioPipeline,
                                         std::move(failureCallback));
}

void directVulkanPreviewWindowSetVulkanInstance(DirectVulkanPreviewWindow* window,
                                                QVulkanInstance* instance)
{
    if (window) {
        window->setVulkanInstance(instance);
        QVector<VkPhysicalDeviceProperties> devices;
        if (instance && instance->vkInstance() != VK_NULL_HANDLE) {
            uint32_t deviceCount = 0;
            if (vkEnumeratePhysicalDevices(
                    instance->vkInstance(), &deviceCount, nullptr) == VK_SUCCESS &&
                deviceCount > 0) {
                std::vector<VkPhysicalDevice> physicalDevices(deviceCount);
                if (vkEnumeratePhysicalDevices(instance->vkInstance(),
                                               &deviceCount,
                                               physicalDevices.data()) == VK_SUCCESS) {
                    devices.reserve(static_cast<qsizetype>(deviceCount));
                    for (VkPhysicalDevice device : physicalDevices) {
                        VkPhysicalDeviceProperties properties{};
                        vkGetPhysicalDeviceProperties(device, &properties);
                        devices.push_back(properties);
                    }
                }
            }
        }
        const int selectedIndex = editor::gpu::chooseVulkanDevice(devices);
        if (selectedIndex >= 0) {
            window->setPreferredPhysicalDeviceIndex(selectedIndex);
        }
    }
}

QVulkanInfoVector<QVulkanExtension> directVulkanPreviewWindowSupportedDeviceExtensions(
    DirectVulkanPreviewWindow* window)
{
    return window ? window->supportedDeviceExtensions() : QVulkanInfoVector<QVulkanExtension>();
}

void directVulkanPreviewWindowSetDeviceExtensions(DirectVulkanPreviewWindow* window,
                                                  const QByteArrayList& extensions)
{
    if (window) {
        window->setDeviceExtensions(extensions);
    }
}

void directVulkanPreviewWindowResize(DirectVulkanPreviewWindow* window, const QSize& size)
{
    if (window) {
        window->resize(size);
    }
}

void directVulkanPreviewWindowSetInteractionCallbacks(
    DirectVulkanPreviewWindow* window,
    std::function<void(const QString&)> selectionRequested,
    std::function<void(const QString&, qreal, qreal, bool)> moveRequested,
    std::function<void(const QString&, qreal, qreal, qreal, qreal, bool)> transformRequested,
    std::function<void(int64_t)> playbackSampleRequested,
    std::function<void(const QString&, qreal, qreal)> correctionPointRequested,
    std::function<void(const QString&, qreal, qreal)> speakerPointRequested,
    std::function<void(const QString&, qreal, qreal, qreal)> speakerBoxRequested,
    std::function<void(const QString&, int, const QString&, int64_t, qreal, qreal, qreal)> faceStreamBoxRequested,
    std::function<void(const QString&, int, const QString&, int64_t, qreal, qreal, qreal)> faceStreamBoxFocusClearRequested,
    std::function<void(const QString&)> faceStreamBoxClickStatus,
    std::function<void(const QString&)> createKeyframeRequested)
{
    if (!window) {
        return;
    }
    window->setInteractionCallbacks(std::move(selectionRequested),
                                    std::move(moveRequested),
                                    std::move(transformRequested),
                                    std::move(playbackSampleRequested),
                                    std::move(correctionPointRequested),
                                    std::move(speakerPointRequested),
                                    std::move(speakerBoxRequested),
                                    std::move(faceStreamBoxRequested),
                                    std::move(faceStreamBoxFocusClearRequested),
                                    std::move(faceStreamBoxClickStatus),
                                    std::move(createKeyframeRequested));
}

bool directVulkanPreviewWindowUpdatePending(DirectVulkanPreviewWindow* window)
{
    return window && window->updatePending();
}

bool directVulkanPreviewWindowIsValid(DirectVulkanPreviewWindow* window)
{
    return window && window->isValid();
}

void directVulkanPreviewWindowSchedulePreviewUpdate(DirectVulkanPreviewWindow* window)
{
    if (window) {
        window->schedulePreviewUpdate();
    }
}

void directVulkanPreviewWindowResetProfilingAnchors(
    DirectVulkanPreviewWindow* window)
{
    if (window) {
        window->resetProfilingAnchors();
    }
}

void directVulkanPreviewWindowRequestPipelineThumbnailReadback(DirectVulkanPreviewWindow* window)
{
    if (window) {
        window->requestPipelineThumbnailReadback();
    }
}

QImage directVulkanPreviewWindowLatestPipelineThumbnailReadback(DirectVulkanPreviewWindow* window)
{
    return window ? window->latestVulkanReadbackImage() : QImage();
}

bool directVulkanPreviewWindowPipelineThumbnailReadbackPending(DirectVulkanPreviewWindow* window)
{
    return window && window->pipelineThumbnailReadbackPending();
}

void directVulkanPreviewWindowRaise(DirectVulkanPreviewWindow* window)
{
    if (window) {
        window->raise();
    }
}

void directVulkanPreviewWindowHide(DirectVulkanPreviewWindow* window)
{
    if (window) {
        window->hide();
    }
}

void directVulkanPreviewWindowSetTitle(DirectVulkanPreviewWindow* window, const QString& title)
{
    if (window) {
        window->setTitle(title);
    }
}

void directVulkanPreviewWindowSetGpuExportPreviewFrame(
    DirectVulkanPreviewWindow* window,
    const render_detail::OffscreenVulkanFrame& frame)
{
    if (window) {
        window->setGpuExportPreviewFrame(frame);
    }
}

void directVulkanPreviewWindowClearGpuExportPreview(
    DirectVulkanPreviewWindow* window)
{
    if (window) {
        window->clearGpuExportPreview();
    }
}

bool directVulkanPreviewWindowIsVisible(DirectVulkanPreviewWindow* window)
{
    return window && window->isVisible();
}

QString directVulkanPreviewWindowCursorShape(DirectVulkanPreviewWindow* window)
{
    if (!window) {
        return QString();
    }
    switch (window->cursor().shape()) {
    case Qt::ArrowCursor:
        return QStringLiteral("arrow");
    case Qt::UpArrowCursor:
        return QStringLiteral("up_arrow");
    case Qt::CrossCursor:
        return QStringLiteral("cross");
    case Qt::WaitCursor:
        return QStringLiteral("wait");
    case Qt::IBeamCursor:
        return QStringLiteral("ibeam");
    case Qt::SizeVerCursor:
        return QStringLiteral("size_ver");
    case Qt::SizeHorCursor:
        return QStringLiteral("size_hor");
    case Qt::SizeBDiagCursor:
        return QStringLiteral("size_bdiag");
    case Qt::SizeFDiagCursor:
        return QStringLiteral("size_fdiag");
    case Qt::SizeAllCursor:
        return QStringLiteral("size_all");
    case Qt::BlankCursor:
        return QStringLiteral("blank");
    case Qt::SplitVCursor:
        return QStringLiteral("split_v");
    case Qt::SplitHCursor:
        return QStringLiteral("split_h");
    case Qt::PointingHandCursor:
        return QStringLiteral("pointing_hand");
    case Qt::ForbiddenCursor:
        return QStringLiteral("forbidden");
    case Qt::OpenHandCursor:
        return QStringLiteral("open_hand");
    case Qt::ClosedHandCursor:
        return QStringLiteral("closed_hand");
    case Qt::WhatsThisCursor:
        return QStringLiteral("whats_this");
    case Qt::BusyCursor:
        return QStringLiteral("busy");
    default:
        return QStringLiteral("other");
    }
}
