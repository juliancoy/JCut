#include <execinfo.h>
#include "direct_vulkan_preview_backend.h"
#include "direct_vulkan_preview_presenter.h"
#include "direct_vulkan_preview_config.h"
#include "direct_vulkan_preview_geometry.h"
#include "direct_vulkan_preview_interaction.h"
#include "direct_vulkan_preview_overlay_rendering.h"
#include "direct_vulkan_preview_transcript.h"
#include "direct_vulkan_frame_handoff_pipeline.h"
#include "direct_vulkan_preview_audio.h"
#include "preview_speaker_profiles.h"
#include "preview_view_transform.h"
#include "render_vulkan_shared.h"
#include "editor_shared.h"
#include "render_internal.h"
#include "titles.h"
#include "vulkan_audio_tab.h"
#include "vulkan_pipeline.h"
#include "vulkan_resources.h"
#include "vulkan_text_renderer.h"
#include "waveform_service.h"
#include "loiacono/loiacono_rolling.h"

#include <QDebug>
#include <QByteArray>
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
#include <QVulkanWindow>

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

namespace {
constexpr qint64 kPipelineThumbnailReadbackMinIntervalMs = 250;
constexpr bool kAllowCpuRasterTextOverlaysInDirectVulkanPreview = false;

using namespace jcut::direct_vulkan_preview;

class DirectVulkanPreviewRenderer final : public QVulkanWindowRenderer {
public:
    DirectVulkanPreviewRenderer(DirectVulkanPreviewWindow* owner, QVulkanWindow* window)
        : m_owner(owner), m_window(window) {}
    ~DirectVulkanPreviewRenderer() override;

    void initResources() override;
    void releaseResources() override;
    void startNextFrame() override;
    void physicalDeviceLost() override;
    void logicalDeviceLost() override;

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

    DirectVulkanPreviewWindow* m_owner = nullptr;
    QVulkanWindow* m_window = nullptr;
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
};

} // namespace

class DirectVulkanPreviewWindow final : public QVulkanWindow {
public:
    DirectVulkanPreviewWindow(PreviewInteractionState* state,
                              int64_t* presentedFrames,
                              int64_t* lastPresentedSourceFrame,
                              DirectVulkanPreviewStats* stats,
                              bool* active,
                              QString* failureReason,
                              std::function<void(const QString&)> failureCallback = {})
        : m_state(state),
          m_presentedFrames(presentedFrames),
          m_lastPresentedSourceFrame(lastPresentedSourceFrame),
          m_stats(stats),
          m_active(active),
          m_failureReason(failureReason),
          m_failureCallback(std::move(failureCallback))
    {
        setSurfaceType(QSurface::VulkanSurface);
        setTitle(QStringLiteral("JCut Direct Vulkan Preview"));
        setFlags(QVulkanWindow::PersistentResources);
    }

    void setInteractionCallbacks(std::function<void(const QString&)> selectionRequested,
                                 std::function<void(const QString&, qreal, qreal, bool)> resizeRequested,
                                 std::function<void(const QString&, qreal, qreal, bool)> moveRequested,
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
        m_resizeRequested = std::move(resizeRequested);
        m_moveRequested = std::move(moveRequested);
        m_playbackSampleRequested = std::move(playbackSampleRequested);
        m_correctionPointRequested = std::move(correctionPointRequested);
        m_speakerPointRequested = std::move(speakerPointRequested);
        m_speakerBoxRequested = std::move(speakerBoxRequested);
        m_faceStreamBoxRequested = std::move(faceStreamBoxRequested);
        m_faceStreamBoxFocusClearRequested = std::move(faceStreamBoxFocusClearRequested);
        m_faceStreamBoxClickStatus = std::move(faceStreamBoxClickStatus);
        m_createKeyframeRequested = std::move(createKeyframeRequested);
    }

    QVulkanWindowRenderer* createRenderer() override
    {
        return new DirectVulkanPreviewRenderer(this, this);
    }

    void schedulePreviewUpdate()
    {
        if (m_updatePending) {
            return;
        }
        m_updatePending = true;
        m_updateRequestMs = QDateTime::currentMSecsSinceEpoch();
        if (m_stats) {
            ++m_stats->previewUpdateRequests;
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
        requestUpdate();
    }

    bool updatePending() const
    {
        return m_updatePending;
    }

protected:
    void exposeEvent(QExposeEvent* event) override
    {
        QVulkanWindow::exposeEvent(event);
        if (!isExposed()) {
            m_scheduledWhileExposed = false;
            return;
        }
        if (!isValid()) {
            markFailure(QStringLiteral("QVulkanWindow exposed but invalid; Vulkan surface or swapchain creation failed."));
        } else if (m_active) {
            *m_active = true;
            // Schedule a render only on the not-exposed -> exposed
            // transition or when the surface size changed. On macOS every
            // vkQueuePresentKHR re-dirties the CAMetalLayer, AppKit asks the
            // layer to display, and Qt synthesizes another ExposeEvent —
            // unconditionally scheduling here turns each presented frame
            // into the trigger for the next one (idle present storm that
            // starves the UI thread and the control-server bridge).
            if (!m_scheduledWhileExposed || size() != m_lastExposeScheduledSize) {
                m_scheduledWhileExposed = true;
                m_lastExposeScheduledSize = size();
                schedulePreviewUpdate();
            }
        }
    }

    void wheelEvent(QWheelEvent* event) override
    {
        if (!event || !m_state || event->angleDelta().y() == 0) {
            QVulkanWindow::wheelEvent(event);
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
            QVulkanWindow::wheelEvent(event);
            return;
        }
        if (m_state->viewMode == PreviewSurface::ViewMode::Audio) {
            QVulkanWindow::wheelEvent(event);
            return;
        }
        if (applyVideoPreviewWheelZoom(m_state, surfaceRect, surfacePosition, event->angleDelta().y())) {
            schedulePreviewUpdate();
            event->accept();
            return;
        }
        QVulkanWindow::wheelEvent(event);
    }

    void mousePressEvent(QMouseEvent* event) override
    {
        if (!event || !m_state) {
            QVulkanWindow::mousePressEvent(event);
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
            QVulkanWindow::mousePressEvent(event);
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
            QVulkanWindow::mousePressEvent(event);
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
            schedulePreviewUpdate();
            updatePreviewCursor(surfacePosition);
            event->accept();
            return;
        }

        transient.dragMode = PreviewDragMode::None;
        transient.dragOriginBounds = QRectF();
        QVulkanWindow::mousePressEvent(event);
    }

    void mouseMoveEvent(QMouseEvent* event) override
    {
        if (!event || !m_state) {
            QVulkanWindow::mouseMoveEvent(event);
            return;
        }
        const QPointF surfacePosition = PreviewViewTransform::pointForWindowPoint(
            this, event->position(), PreviewSurfaceCoordinateSpace::DeviceSurface);
        m_state->transient.lastMousePos = surfacePosition;
        if (!(event->buttons() & Qt::LeftButton) || m_state->transient.dragMode == PreviewDragMode::None ||
            m_state->selectedClipId.isEmpty()) {
            updatePreviewCursor(surfacePosition);
            schedulePreviewUpdate();
            QVulkanWindow::mouseMoveEvent(event);
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
            QVulkanWindow::mouseMoveEvent(event);
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
                QVulkanWindow::mouseMoveEvent(event);
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
            qreal scaleX = transient.dragOriginTransform.scaleX;
            qreal scaleY = transient.dragOriginTransform.scaleY;
            if (m_state->transient.dragMode == PreviewDragMode::ResizeX ||
                m_state->transient.dragMode == PreviewDragMode::ResizeBoth) {
                const qreal factorX = (transient.dragOriginBounds.width() +
                                       (surfacePosition.x() - transient.dragOriginPos.x())) /
                                      transient.dragOriginBounds.width();
                scaleX = sanitizeScaleValue(transient.dragOriginTransform.scaleX * factorX);
            }
            if (m_state->transient.dragMode == PreviewDragMode::ResizeY ||
                m_state->transient.dragMode == PreviewDragMode::ResizeBoth) {
                const qreal factorY = (transient.dragOriginBounds.height() +
                                       (surfacePosition.y() - transient.dragOriginPos.y())) /
                                      transient.dragOriginBounds.height();
                scaleY = sanitizeScaleValue(transient.dragOriginTransform.scaleY * factorY);
            }
            if (m_state->transient.dragMode == PreviewDragMode::ResizeBoth) {
                const qreal factorX = (transient.dragOriginBounds.width() +
                                       (surfacePosition.x() - transient.dragOriginPos.x())) /
                                      transient.dragOriginBounds.width();
                const qreal factorY = (transient.dragOriginBounds.height() +
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
                (m_state->transient.dragMode == PreviewDragMode::ResizeX
                     ? PreviewResizeAnchor::Left
                     : (m_state->transient.dragMode == PreviewDragMode::ResizeY
                            ? PreviewResizeAnchor::Top
                            : PreviewResizeAnchor::TopLeft)),
                activeInfo.clipPixelSize.isValid() ? previewScale : QPointF(1.0, 1.0));
            TimelineClip::TransformKeyframe overrideTransform = transient.dragOriginTransform;
            overrideTransform.scaleX = scaleX;
            overrideTransform.scaleY = scaleY;
            overrideTransform.translationX = translation.x();
            overrideTransform.translationY = translation.y();
            m_state->transient.transformOverrideActive = true;
            m_state->transient.transformOverrideClipId = clipId;
            m_state->transient.transformOverride = overrideTransform;
        }
        schedulePreviewUpdate();
        event->accept();
    }

    void mouseReleaseEvent(QMouseEvent* event) override
    {
        if (!event || event->button() != Qt::LeftButton || !m_state) {
            QVulkanWindow::mouseReleaseEvent(event);
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
            } else if (m_resizeRequested && !clipId.isEmpty()) {
                if (activeInfoIsTranscript) {
                    const QSizeF size =
                        m_state->transient.transcriptOverrideActive &&
                                m_state->transient.transcriptOverrideClipId == clipId &&
                                m_state->transient.transcriptSizeOverride.width() > 0.0 &&
                                m_state->transient.transcriptSizeOverride.height() > 0.0
                            ? m_state->transient.transcriptSizeOverride
                            : QSizeF(activeInfo.bounds.width() / qMax<qreal>(0.0001, previewScale.x()),
                                     activeInfo.bounds.height() / qMax<qreal>(0.0001, previewScale.y()));
                    if (m_moveRequested) {
                        const QPointF translation =
                            m_state->transient.transcriptOverrideActive &&
                                    m_state->transient.transcriptOverrideClipId == clipId
                                ? m_state->transient.transcriptTranslationOverride
                                : m_state->transient.dragOriginTranscriptTranslation;
                        m_moveRequested(clipId, translation.x(), translation.y(), true);
                    }
                    const qreal width = size.width();
                    const qreal height = size.height();
                    m_resizeRequested(clipId, width, height, true);
                } else {
                    const TimelineClip::TransformKeyframe transform =
                        currentTransformForVulkanClip(m_state, clipId);
                    if (m_moveRequested) {
                        m_moveRequested(clipId, transform.translationX, transform.translationY, false);
                    }
                    m_resizeRequested(clipId, transform.scaleX, transform.scaleY, true);
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

        QVulkanWindow::mouseReleaseEvent(event);
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
        QVulkanWindow::keyPressEvent(event);
    }

    void keyReleaseEvent(QKeyEvent* event) override
    {
        if (event->key() == Qt::Key_Shift || event->key() == Qt::Key_Alt || event->key() == Qt::Key_Control) {
            const QPointF cursorPos = PreviewViewTransform::pointForWindowPoint(
                this, mapFromGlobal(QCursor::pos()), PreviewSurfaceCoordinateSpace::DeviceSurface);
            updatePreviewCursor(cursorPos);
            schedulePreviewUpdate();
        }
        QVulkanWindow::keyReleaseEvent(event);
    }

    bool event(QEvent* event) override
    {
        if (!event) {
            return QVulkanWindow::event(event);
        }

        if (event->type() == QEvent::UpdateRequest) {
            // Render on demand: the macOS platform layer (display link /
            // Metal layer) delivers continuous UpdateRequests for this
            // window even when nothing scheduled one, which saturates the
            // UI thread with idle presents and starves the control-server
            // bridge. Only render when the app latched a request via
            // schedulePreviewUpdate() or during active playback, where the
            // renderer re-arms itself per presented frame.
            const bool playing = m_state && m_state->playing;
            if (!m_updatePending && !playing) {
                return true;
            }
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
            return QVulkanWindow::event(event);
        }

        if (event->type() == QEvent::ContextMenu) {
            auto* contextMenu = static_cast<QContextMenuEvent*>(event);
            if (!contextMenu || !m_state) {
                return QVulkanWindow::event(event);
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
                return QVulkanWindow::event(event);
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
            return QVulkanWindow::event(event);
        }

        return QVulkanWindow::event(event);
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
    void markPresented()
    {
        const qint64 nowMs = QDateTime::currentMSecsSinceEpoch();
        if (m_lastPresentMs > 0 && m_stats) {
            const double intervalMs = static_cast<double>(nowMs - m_lastPresentMs);
            m_stats->lastPresentIntervalMs = intervalMs;
            m_stats->maxPresentIntervalMs = std::max(m_stats->maxPresentIntervalMs, intervalMs);
        }
        m_lastPresentMs = nowMs;
        if (m_presentedFrames) {
            ++(*m_presentedFrames);
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
    void markPresentedSourceFrame(int64_t frame)
    {
        if (m_lastPresentedSourceFrame) {
            *m_lastPresentedSourceFrame = frame;
        }
    }
    void markPreviewUpdateDelivered()
    {
        if (m_updatePending && m_updateRequestMs > 0 && m_stats) {
            const double latencyMs =
                static_cast<double>(QDateTime::currentMSecsSinceEpoch() - m_updateRequestMs);
            ++m_stats->previewUpdatesDelivered;
            m_stats->lastPreviewUpdateLatencyMs = latencyMs;
            m_stats->maxPreviewUpdateLatencyMs =
                std::max(m_stats->maxPreviewUpdateLatencyMs, latencyMs);
        }
        m_updatePending = false;
        m_updateRequestMs = 0;
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
    PreviewInteractionState* m_state = nullptr;
    int64_t* m_presentedFrames = nullptr;
    int64_t* m_lastPresentedSourceFrame = nullptr;
    DirectVulkanPreviewStats* m_stats = nullptr;
    bool* m_active = nullptr;
    QString* m_failureReason = nullptr;
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
    std::function<void(const QString&, qreal, qreal, bool)> m_resizeRequested;
    std::function<void(const QString&, qreal, qreal, bool)> m_moveRequested;
    QImage m_latestVulkanReadbackImage;
    QImage m_latestDecoderDiagnosticImage;
    bool m_pipelineThumbnailReadbackPending = false;
    qint64 m_lastPipelineThumbnailReadbackMs = 0;
    bool m_updatePending = false;
    bool m_scheduledWhileExposed = false;
    QSize m_lastExposeScheduledSize;
    qint64 m_updateRequestMs = 0;
    qint64 m_lastPresentMs = 0;
};

void DirectVulkanPreviewRenderer::initResources()
{
    m_devFuncs = m_window && m_window->vulkanInstance()
        ? m_window->vulkanInstance()->deviceFunctions(m_window->device())
        : nullptr;
    if (m_window && m_window->physicalDeviceProperties()) {
        const VkPhysicalDeviceProperties* props = m_window->physicalDeviceProperties();
        qInfo().noquote()
            << QStringLiteral("[vulkan-preview] direct presenter device=%1 vendor=0x%2 type=%3")
                   .arg(QString::fromLatin1(props->deviceName))
                   .arg(QString::number(props->vendorID, 16))
                   .arg(static_cast<int>(props->deviceType));
    }
    m_resources = std::make_unique<VulkanResources>();
    if (!m_resources->initialize(m_window->physicalDevice(), m_window->device(), m_devFuncs)) {
        if (m_owner) {
            m_owner->markFailure(QStringLiteral("Failed to initialize direct presenter Vulkan resources."));
        }
        return;
    }
    m_playbackStatusOverlayResources = std::make_unique<VulkanResources>();
    if (!m_playbackStatusOverlayResources->initialize(m_window->physicalDevice(), m_window->device(), m_devFuncs)) {
        if (m_owner) {
            m_owner->markFailure(QStringLiteral("Failed to initialize playback status overlay Vulkan resources."));
        }
        return;
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

DirectVulkanPreviewRenderer::~DirectVulkanPreviewRenderer()
{
    destroyReadbackSlots();
}

void DirectVulkanPreviewRenderer::releaseResources()
{
    destroyReadbackSlots();
    for (auto it = m_clipHandoffResources.begin(); it != m_clipHandoffResources.end(); ++it) {
        releaseClipHandoffResources(it.value());
    }
    for (const RetiredClipHandoffResources& retired : m_retiredClipHandoffResources) {
        releaseClipHandoffResources(retired.resources);
    }
    m_clipHandoffResources.clear();
    m_retiredClipHandoffResources.clear();
    m_audioTab.reset();
    m_temporalDebugTextRenderer.reset();
    m_speakerTextRenderer.reset();
    m_textRenderer.reset();
    m_pipeline.reset();
    m_playbackStatusOverlayResources.reset();
    m_playbackStatusOverlayTextureKey.clear();
    m_playbackStatusOverlayTextureReady = false;
    m_resources.reset();
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

void DirectVulkanPreviewRenderer::startNextFrame()
{
    if (!m_owner || !m_window || !m_devFuncs) {
        if (m_owner && m_owner->stats()) {
            editor::accumulatePlaybackStageMetric(&m_owner->stats()->commandRecordingStageMetric,
                                          1,
                                          0,
                                          1,
                                          QStringLiteral("source_unavailable"),
                                          QStringLiteral("renderer_or_device_unavailable"));
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
    rp.renderPass = m_window->defaultRenderPass();
    rp.framebuffer = m_window->currentFramebuffer();
    const QSize swapSize = m_window->swapChainImageSize();
    rp.renderArea.offset = {0, 0};
    rp.renderArea.extent = {static_cast<uint32_t>(std::max(1, swapSize.width())),
                            static_cast<uint32_t>(std::max(1, swapSize.height()))};
    rp.clearValueCount = m_window->depthStencilFormat() == VK_FORMAT_UNDEFINED ? 1u : 2u;
    rp.pClearValues = clearValues;

    VkCommandBuffer cb = m_window->currentCommandBuffer();
    const uint32_t swapchainImageIndex =
        static_cast<uint32_t>(std::max(0, m_window->currentSwapChainImageIndex()));
    advanceRetiredClipHandoffResources();
    struct DecoderReadbackCandidate {
        VkImage image = VK_NULL_HANDLE;
        VkImageLayout layout = VK_IMAGE_LAYOUT_UNDEFINED;
        QSize size;
        VkFormat format = VK_FORMAT_UNDEFINED;
    } decoderReadbackCandidate;
    QHash<QString, DirectVulkanFrameHandoffPipeline::Result> frameHandoffResults;
    QHash<QString, bool> curveLutUploadResults;
    QHash<QString, bool> maskCurveLutUploadResults;
    QHash<QString, bool> maskUploadResults;
    struct PreparedOverlayTexture {
        VulkanResources* resources = nullptr;
        QRectF bounds;
        bool ready = false;
    };
    PreparedTranscriptOverlayMap preparedTranscriptOverlays;
    PreparedOverlayTexture preparedPlaybackStatusOverlay;
    const bool forceChecker = qEnvironmentVariableIntValue("JCUT_VULKAN_PREVIEW_FORCE_CHECKER") == 1;
    const bool canDrawTexture = m_resources && m_pipeline && m_resources->isReady() &&
                                m_pipeline->isReady();
    if (state && !forceChecker && canDrawTexture) {
        QSet<QString> activeHandoffClipIds;
        for (const VulkanPreviewClipFrameStatus& status : state->vulkanFrameStatuses) {
            if (!status.active || status.drawSuppressed) {
                continue;
            }
            activeHandoffClipIds.insert(status.clipId);
        }
        pruneClipHandoffResources(activeHandoffClipIds);
        updateClipHandoffResourceStats();
        for (const VulkanPreviewClipFrameStatus& status : state->vulkanFrameStatuses) {
            if (!status.active || status.drawSuppressed) {
                continue;
            }
            ClipHandoffResources* handoffResources = ensureClipHandoffResources(status.clipId);
            if (!handoffResources || !handoffResources->resources || !handoffResources->pipeline) {
                continue;
            }
            if (!handoffResources->resources->beginFrameUploads(
                    swapchainImageIndex,
                    qMax<size_t>(VulkanResources::kDescriptorSetCount,
                                 static_cast<size_t>(swapchainImageIndex) + 1))) {
                continue;
            }
            DirectVulkanFrameHandoffPipeline::Result handoffResult;
            if (status.frame.hasCpuImage() &&
                !status.frame.hasHardwareFrame() &&
                !status.externalVulkanFrame) {
                handoffResult.attempted = true;
                handoffResult.sampledFrameReady =
                    handoffResources->resources->uploadImageTexture(cb, status.frame.cpuImage());
                handoffResult.descriptorSet =
                    handoffResult.sampledFrameReady
                        ? handoffResources->resources->descriptorSet()
                        : VK_NULL_HANDLE;
                handoffResult.descriptorSetIndex =
                    static_cast<int>(handoffResources->resources->descriptorSetIndex());
                handoffResult.descriptorSetCount =
                    static_cast<int>(handoffResources->resources->descriptorSetCount());
                handoffResult.size = {status.frameSize.width(), status.frameSize.height()};
                handoffResult.format = VK_FORMAT_R8G8B8A8_UNORM;
                if (DirectVulkanPreviewStats* stats = m_owner ? m_owner->stats() : nullptr) {
                    ++stats->handoffAttempts;
                    if (handoffResult.sampledFrameReady) {
                        ++stats->handoffSuccesses;
                        ++stats->sampledImageReady;
                        stats->lastHandoffError.clear();
                        stats->lastHandoffMode = QStringLiteral("cpu_image_upload");
                        stats->lastExternalImageSize = status.frameSize;
                        stats->lastVulkanImageFormat =
                            jcut::direct_vulkan_preview::vulkanFormatName(handoffResult.format);
                        stats->descriptorSetIndex = handoffResult.descriptorSetIndex;
                        stats->descriptorSetCount = handoffResult.descriptorSetCount;
                    } else {
                        ++stats->handoffFailures;
                        stats->lastHandoffMode = QStringLiteral("cpu_image_upload_failed");
                        stats->lastHandoffError =
                            QStringLiteral("Failed to upload CPU image frame to Vulkan texture.");
                    }
                }
            } else {
                handoffResult = handoffResources->pipeline->record(
                    cb,
                    swapchainImageIndex,
                    status,
                    handoffResources->resources.get(),
                    m_owner ? m_owner->stats() : nullptr);
            }
            frameHandoffResults.insert(status.clipId, handoffResult);
            const QByteArray curveLut = curveLutRgbaBytes(status.grading);
            if (!curveLut.isEmpty()) {
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
            if (status.maskTextureEnabled) {
                VulkanMaskPreprocessOptions maskOptions;
                maskOptions.outputSize = status.frameSize;
                maskOptions.invert = status.maskInvert;
                maskOptions.erodeRadius = qRound(qMax<qreal>(0.0, status.maskErode));
                maskOptions.dilateRadius = qRound(qMax<qreal>(0.0, status.maskDilate));
                maskOptions.blurRadius = qRound(qMax<qreal>(status.maskFeather, status.maskBlur));
                maskUploadResults.insert(
                    status.clipId,
                    handoffResources->resources->uploadMaskTexture(cb, status.maskImage, maskOptions));
            }
            handoffResources->resources->ensureAuxiliaryImagesReadable(cb);
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
            ((preparedSpeakerSpec.showName || preparedSpeakerSpec.showOrganization) ? 1 : 0) +
            (!preparedTemporalDebugSpec.organization.trimmed().isEmpty() ? 1 : 0);
        const qint64 textSuccessCount =
            preparedTranscriptAtlasClipIds.size() +
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
                                      QStringLiteral("transcript=%1 speaker=%2 debug=%3 cache_hit=%4")
                                          .arg(preparedTranscriptAtlasClipIds.size())
                                          .arg(preparedSpeakerLabel ? 1 : 0)
                                          .arg(preparedTemporalDebugLabel ? 1 : 0)
                                          .arg(textPrepCacheHit ? 1 : 0));
    }
    m_devFuncs->vkCmdBeginRenderPass(cb, &rp, VK_SUBPASS_CONTENTS_INLINE);
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
            DirectVulkanAudioRenderContext{state, m_devFuncs, m_audioTab.get(), cb, swapSize},
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
        m_owner->markPresented();
        m_window->frameReady();
        m_owner->markPreviewUpdateDelivered();
        if (state->playing || audioWaitingForWaveform) {
            m_owner->schedulePreviewUpdate();
        }
        return;
    }
    int64_t presentedSourceFrame = -1;
    qint64 handoffAttemptCount = 0;
    qint64 handoffSuccessCount = 0;
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
        bool backgroundFilled = false;
        struct PendingMaskForegroundDraw {
            VkDescriptorSet descriptorSet = VK_NULL_HANDLE;
            VulkanPipeline::Push push;
            VkRect2D scissor{};
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
        for (const TimelineClip& clip : state->clips) {
            const VulkanPreviewClipFrameStatus* status = frameStatusForClip(state, clip.id);
            if (!status || !status->active || status->drawSuppressed) {
                continue;
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
            const bool sampledFrameReady =
                handoffResult.sampledFrameReady && handoffResult.descriptorSet != VK_NULL_HANDLE;
            const bool handoffAttempted = handoffResult.attempted;
            handoffAttemptCount += handoffAttempted ? 1 : 0;
            handoffSuccessCount += sampledFrameReady ? 1 : 0;
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
                if (!backgroundFilled &&
                    render_detail::shouldDrawBlurredFillBackground(
                        frameSize.isValid() ? frameSize : clip.sourceFrameSize,
                        state->outputSize)) {
                    const BackgroundFillEffect fillEffect = state->backgroundFillEffect;
                    const QRectF sourceRectNorm(
                        (effectiveClipGeometry.bounds.left() - compositeRect.left()) /
                            qMax<qreal>(1.0, compositeRect.width()),
                        (effectiveClipGeometry.bounds.top() - compositeRect.top()) /
                            qMax<qreal>(1.0, compositeRect.height()),
                        effectiveClipGeometry.bounds.width() / qMax<qreal>(1.0, compositeRect.width()),
                        effectiveClipGeometry.bounds.height() / qMax<qreal>(1.0, compositeRect.height()));
                    const bool fullCanvasFill =
                        fillEffect == BackgroundFillEffect::EdgeStretch ||
                        fillEffect == BackgroundFillEffect::Mirror;
                    PreviewClipGeometry backgroundGeometry =
                        fullCanvasFill ? PreviewViewTransform::clipGeometry(
                                             compositeRect,
                                             QPointF(1.0, 1.0),
                                             QPointF(),
                                             0.0,
                                             QPointF(1.0, 1.0))
                                       : effectiveClipGeometry;
                    if (fillEffect == BackgroundFillEffect::BlurCover) {
                        const qreal coverScale = std::max<qreal>(
                            1.0,
                            std::max(
                                compositeRect.width() / qMax<qreal>(1.0, effectiveClipGeometry.bounds.width()),
                                compositeRect.height() / qMax<qreal>(1.0, effectiveClipGeometry.bounds.height())));
                        backgroundGeometry.clipToScreen.scale(coverScale * 1.08, coverScale * 1.08);
                        backgroundGeometry.bounds =
                            backgroundGeometry.clipToScreen.mapRect(backgroundGeometry.localRect);
                    }
                    VulkanPipeline::Push backgroundPush{};
                    mvpForVulkanClipTransform(backgroundGeometry.clipToScreen,
                                              backgroundGeometry.localRect,
                                              swapSize,
                                              backgroundPush.mvp);
                    const render_detail::VulkanDrawEffectState baseEffects =
                        render_detail::vulkanDrawEffectStateForGrade(status->grading);
                    const render_detail::VulkanDrawEffectState backgroundEffects =
                        render_detail::vulkanBackgroundFillEffectState(
                            fillEffect,
                            baseEffects,
                            static_cast<float>(state->backgroundFillOpacity),
                            static_cast<float>(state->backgroundFillBrightness),
                            static_cast<float>(state->backgroundFillSaturation),
                            state->backgroundFillEdgePixels,
                            state->backgroundFillEdgeProgressive,
                            static_cast<float>(state->backgroundFillEdgePower),
                            sourceRectNorm);
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
                    m_pipeline->bindAndDraw(cb,
                                            viewport,
                                            backgroundScissor,
                                            handoffResult.descriptorSet,
                                            backgroundPush);
                    backgroundFilled = true;
                }
                VulkanPipeline::Push push{};
                mvpForVulkanClipTransform(effectiveClipGeometry.clipToScreen,
                                          effectiveClipGeometry.localRect,
                                          swapSize,
                                          push.mvp);
                if (status) {
                    const render_detail::VulkanDrawEffectState effects =
                        render_detail::vulkanDrawEffectStateForGrade(status->grading);
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
                    push.shadows[3] = status->curveLutApplied
                        ? render_detail::kVulkanEffectModeCurve
                        : render_detail::kVulkanEffectModeNormal;
                    push.midtones[3] = static_cast<float>(std::max<qreal>(0.0, status->maskFeather));
                    push.highlights[3] = static_cast<float>(std::max<qreal>(0.01, status->maskFeatherGamma));
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
                auto drawPush = [&](const VulkanPipeline::Push& drawState) {
                    m_pipeline->bindAndDraw(cb, viewport, scissor, handoffResult.descriptorSet, drawState);
                };
                const bool maskReady =
                    status && status->maskTextureEnabled &&
                    maskUploadResults.value(status->clipId, false);
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
                    if (status && status->curveLutApplied &&
                        !curveLutUploadResults.value(status->clipId, false)) {
                        basePush.shadows[3] = render_detail::kVulkanEffectModeNormal;
                        if (DirectVulkanPreviewStats* stats = m_owner->stats()) {
                            stats->lastUnsupportedEffect = QStringLiteral("curve_lut_upload_failed");
                        }
                    }
                    const render_detail::VulkanEffectPipelinePlan effectPlan =
                        render_detail::vulkanEffectPipelinePlan(
                            clip,
                            compositeRect,
                            frameSize.isValid() ? frameSize : clip.sourceFrameSize,
                            state->currentFramePosition);
                    if (effectPlan.usesGeneratedDraws()) {
                        for (const render_detail::VulkanEffectPipelinePlan::DrawPass& effectDraw :
                             effectPlan.generatedDraws) {
                            VulkanPipeline::Push effectPush = basePush;
                            effectPush.opacity *= effectDraw.opacityMultiplier;
                            effectPush.shadows[3] = effectDraw.shaderMode;
                            render_detail::vulkanMvpForOutputRectMaybeFlippedY(
                                effectDraw.outputRect,
                                swapSize,
                                effectDraw.rotationDegrees,
                                status && status->sampledFrameNeedsYFlip,
                                effectPush.mvp);
                            drawPush(effectPush);
                        }
                    } else {
                        drawPush(basePush);
                    }
                    if (maskReady && status->maskGradeEnabled) {
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
                        foregroundPush.brightness = 0.0f;
                        foregroundPush.contrast = 1.0f;
                        foregroundPush.saturation = 1.0f;
                        foregroundPush.opacity = 1.0f;
                        foregroundPush.shadows[0] = 0.0f;
                        foregroundPush.shadows[1] = 0.0f;
                        foregroundPush.shadows[2] = 0.0f;
                        foregroundPush.shadows[3] = render_detail::kVulkanEffectModeMaskOnly;
                        foregroundPush.midtones[0] = 0.0f;
                        foregroundPush.midtones[1] = 0.0f;
                        foregroundPush.midtones[2] = 0.0f;
                        foregroundPush.midtones[3] = 0.0f;
                        foregroundPush.highlights[0] = 0.0f;
                        foregroundPush.highlights[1] = 0.0f;
                        foregroundPush.highlights[2] = 0.0f;
                        foregroundPush.highlights[3] = 1.0f;
                        pendingMaskForegroundDraws.push_back(
                            PendingMaskForegroundDraw{handoffResult.descriptorSet, foregroundPush, scissor});
                    }
                }
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
            if (status && status->hasFrame && canDrawTexture && sampledFrameReady) {
                presentedSourceFrame = std::max<int64_t>(presentedSourceFrame, status->presentedSourceFrame);
            }
        }
        for (const PendingMaskForegroundDraw& draw : std::as_const(pendingMaskForegroundDraws)) {
            if (draw.descriptorSet != VK_NULL_HANDLE) {
                m_pipeline->bindAndDraw(cb, viewport, draw.scissor, draw.descriptorSet, draw.push);
            }
        }
        qint64 fallbackTranscriptDrawCount = 0;
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
            m_owner->stats()->transcriptDrawnCount = static_cast<int>(transcriptDrawSuccesses);
            editor::accumulatePlaybackStageMetric(&m_owner->stats()->textDrawStageMetric,
                                          transcriptDrawAttempts,
                                          transcriptDrawSuccesses,
                                          qMax<qint64>(0, transcriptDrawAttempts - transcriptDrawSuccesses),
                                          transcriptDrawAttempts > 0
                                              ? QStringLiteral("text_draw_evaluated")
                                              : QStringLiteral("text_draw_not_requested"),
                                          QStringLiteral("transcript_prepared=%1 transcript_drawn=%2 fallback_drawn=%3")
                                              .arg(transcriptDrawAttempts)
                                              .arg(transcriptDrawSuccesses)
                                              .arg(fallbackTranscriptDrawCount));
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

    m_owner->markPresentedSourceFrame(presentedSourceFrame);
    m_owner->markPresented();
    m_window->frameReady();
    m_owner->markPreviewUpdateDelivered();
    if (state && state->playing) {
        m_owner->schedulePreviewUpdate();
    }
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
    int64_t* presentedFrames,
    int64_t* lastPresentedSourceFrame,
    DirectVulkanPreviewStats* stats,
    bool* active,
    QString* failureReason,
    std::function<void(const QString&)> failureCallback)
{
    return new DirectVulkanPreviewWindow(state,
                                         presentedFrames,
                                         lastPresentedSourceFrame,
                                         stats,
                                         active,
                                         failureReason,
                                         std::move(failureCallback));
}

void directVulkanPreviewWindowSetVulkanInstance(DirectVulkanPreviewWindow* window,
                                                QVulkanInstance* instance)
{
    if (window) {
        window->setVulkanInstance(instance);
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
    std::function<void(const QString&, qreal, qreal, bool)> resizeRequested,
    std::function<void(const QString&, qreal, qreal, bool)> moveRequested,
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
                                    std::move(resizeRequested),
                                    std::move(moveRequested),
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
