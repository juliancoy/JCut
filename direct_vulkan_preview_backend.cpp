#include "direct_vulkan_preview_backend.h"
#include "direct_vulkan_preview_presenter.h"
#include "direct_vulkan_frame_handoff_pipeline.h"
#include "audio_preview_support.h"
#include "direct_vulkan_preview_audio.h"
#include "preview_speaker_profiles.h"
#include "preview_view_transform.h"
#include "preview_overlay_model.h"
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
#include <QCoreApplication>
#include <QCryptographicHash>
#include <QDateTime>
#include <QElapsedTimer>
#include <QHash>
#include <QFile>
#include <QFileInfo>
#include <QFontMetrics>
#include <QImage>
#include <QJsonDocument>
#include <QJsonArray>
#include <QJsonObject>
#include <QFrame>
#include <QHBoxLayout>
#include <QLabel>
#include <QPainter>
#include <QPainterPath>
#include <QPixmap>
#include <QVector>
#include <QSet>
#include <QApplication>
#include <QExposeEvent>
#include <QContextMenuEvent>
#include <QCursor>
#include <QMenu>
#include <QKeyEvent>
#include <QTimer>
#include <QVersionNumber>
#include <QMouseEvent>
#include <QVBoxLayout>
#include <QWheelEvent>
#include <QStackedLayout>
#include <QTransform>
#include <QVulkanFunctions>
#include <QVulkanWindow>
#include <QWidget>

#include <vulkan/vulkan.h>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libswresample/swresample.h>
#include <libavutil/pixdesc.h>
#include <libavutil/pixfmt.h>
}

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

bool envFlagEnabled(const char* name)
{
    const QByteArray value = qgetenv(name).trimmed().toLower();
    return value == "1" || value == "true" || value == "yes" || value == "on";
}

bool envFlagDisabled(const char* name)
{
    const QByteArray value = qgetenv(name).trimmed().toLower();
    return value == "0" || value == "false" || value == "no" || value == "off";
}

bool vulkanPreviewDebugChromeEnabled()
{
    return envFlagEnabled("JCUT_VULKAN_PREVIEW_DEBUG_CHROME");
}

bool vulkanPreviewOptimalPresentEnabled()
{
    if (!qEnvironmentVariableIsEmpty("JCUT_VULKAN_PREVIEW_OPTIMAL_PRESENT")) {
        return !envFlagDisabled("JCUT_VULKAN_PREVIEW_OPTIMAL_PRESENT");
    }
    return true;
}

bool vulkanPreviewReadbackMirrorEnabled()
{
    if (!qEnvironmentVariableIsEmpty("JCUT_VULKAN_PREVIEW_READBACK_MIRROR")) {
        return envFlagEnabled("JCUT_VULKAN_PREVIEW_READBACK_MIRROR");
    }
    return false;
}

bool vulkanPreviewDirectSwapchainVisible()
{
    return vulkanPreviewOptimalPresentEnabled() && !vulkanPreviewReadbackMirrorEnabled();
}

QString vulkanPreviewVisiblePathLabel()
{
    return vulkanPreviewReadbackMirrorEnabled()
        ? QStringLiteral("readback_mirror")
        : QStringLiteral("direct_swapchain");
}

int vulkanPreviewCanvasMarginPx()
{
    return 36;
}

QString pixelFormatName(int format)
{
    const char* name = av_get_pix_fmt_name(static_cast<AVPixelFormat>(format));
    return name ? QString::fromLatin1(name) : QStringLiteral("unknown:%1").arg(format);
}

QString vulkanFormatName(VkFormat format)
{
    switch (format) {
    case VK_FORMAT_R8G8B8A8_UNORM: return QStringLiteral("VK_FORMAT_R8G8B8A8_UNORM");
    case VK_FORMAT_B8G8R8A8_UNORM: return QStringLiteral("VK_FORMAT_B8G8R8A8_UNORM");
    case VK_FORMAT_R8G8B8A8_SRGB: return QStringLiteral("VK_FORMAT_R8G8B8A8_SRGB");
    case VK_FORMAT_B8G8R8A8_SRGB: return QStringLiteral("VK_FORMAT_B8G8R8A8_SRGB");
    default: return QStringLiteral("VkFormat:%1").arg(static_cast<int>(format));
    }
}

void setFontPixelSizeRobust(QFont* font, qreal pixelSize, const QPaintDevice* device)
{
    if (!font) {
        return;
    }
    const qreal dpiY = (device && device->logicalDpiY() > 0) ? device->logicalDpiY() : 96.0;
    font->setPointSizeF((pixelSize * 72.0) / dpiY);
}

QString playbackStatusOverlayTextureKey(const QSize& imageSize, const QString& text, qreal progress)
{
    const QString keyMaterial =
        QString::number(imageSize.width()) + QLatin1Char('|') +
        QString::number(imageSize.height()) + QLatin1Char('|') +
        text.trimmed() + QLatin1Char('|') +
        QString::number(progress < 0.0 ? -1 : qRound(qBound<qreal>(0.0, progress, 1.0) * 1000.0));
    const QByteArray digest = QCryptographicHash::hash(keyMaterial.toUtf8(), QCryptographicHash::Sha1);
    return QString::fromLatin1(digest.toHex());
}

render_detail::OverlayImage renderPlaybackStatusOverlay(const QSize& imageSize, const QString& text, qreal progress)
{
    const QString normalized = text.trimmed();
    if (!imageSize.isValid() || normalized.isEmpty()) {
        return {};
    }

    QImage image(qMax(1, imageSize.width()),
                 qMax(1, imageSize.height()),
                 QImage::Format_RGBA8888_Premultiplied);
    image.fill(Qt::transparent);

    QPainter painter(&image);
    painter.setRenderHint(QPainter::Antialiasing, true);
    painter.setRenderHint(QPainter::TextAntialiasing, true);

    QFont font = painter.font();
    font.setBold(true);
    setFontPixelSizeRobust(&font, qBound<qreal>(18.0, image.height() * 0.026, 34.0), painter.device());
    painter.setFont(font);

    const QFontMetrics metrics(font);
    const int maxWidth = qMax(80, image.width() - 48);
    const QString visibleText = metrics.elidedText(normalized, Qt::ElideRight, qMax(1, maxWidth - 44));
    const bool showProgress = progress >= 0.0;
    const int badgeWidth = qMin(maxWidth, qMax(metrics.horizontalAdvance(visibleText) + 44, showProgress ? 360 : 0));
    const int badgeHeight = qMax(showProgress ? 62 : 42, metrics.height() + (showProgress ? 34 : 18));
    const QRectF badgeRect((image.width() - badgeWidth) * 0.5,
                           qMax<qreal>(16.0, image.height() * 0.03),
                           badgeWidth,
                           badgeHeight);

    painter.setPen(QPen(QColor(255, 209, 102, 240), 2.0));
    painter.setBrush(QColor(12, 16, 22, 226));
    painter.drawRoundedRect(badgeRect, 8.0, 8.0);
    painter.setPen(QColor(255, 244, 204, 255));
    painter.drawText(badgeRect.adjusted(16.0, 0.0, -16.0, 0.0),
                     Qt::AlignCenter,
                     visibleText);
    if (showProgress) {
        const QRectF trackRect = badgeRect.adjusted(20.0, badgeRect.height() - 18.0, -20.0, -9.0);
        const qreal normalizedProgress = qBound<qreal>(0.0, progress, 1.0);
        painter.setPen(Qt::NoPen);
        painter.setBrush(QColor(255, 244, 204, 56));
        painter.drawRoundedRect(trackRect, 3.0, 3.0);
        QRectF fillRect = trackRect;
        fillRect.setWidth(qMax<qreal>(2.0, trackRect.width() * normalizedProgress));
        painter.setBrush(QColor(255, 209, 102, 235));
        painter.drawRoundedRect(fillRect, 3.0, 3.0);
    }
    painter.end();

    render_detail::OverlayImage overlay;
    overlay.width = image.width();
    overlay.height = image.height();
    overlay.rgbaPremultiplied = QByteArray(
        reinterpret_cast<const char*>(image.constBits()),
        static_cast<int>(image.sizeInBytes()));
    return overlay;
}

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
    std::unique_ptr<jcut::VulkanAudioTab> m_audioTab;
    QHash<QString, std::shared_ptr<ClipHandoffResources>> m_clipHandoffResources;
    QVector<RetiredClipHandoffResources> m_retiredClipHandoffResources;
    QString m_playbackStatusOverlayTextureKey;
    bool m_playbackStatusOverlayTextureReady = false;
    std::vector<ReadbackSlot> m_readbackSlots;
    std::vector<ReadbackSlot> m_decoderReadbackSlots;
};

VkClearRect clearRectFromQRect(const QRectF& qrect, const QSize& swapSize)
{
    const int maxW = std::max(1, swapSize.width());
    const int maxH = std::max(1, swapSize.height());
    const QRect bounded = qrect.normalized().toAlignedRect().intersected(QRect(0, 0, maxW, maxH));
    VkClearRect rect{};
    rect.rect.offset = {bounded.x(), bounded.y()};
    rect.rect.extent = {
        static_cast<uint32_t>(std::max(1, bounded.width())),
        static_cast<uint32_t>(std::max(1, bounded.height()))
    };
    rect.baseArrayLayer = 0;
    rect.layerCount = 1;
    return rect;
}

VkRect2D scissorFromQRect(const QRectF& qrect, const QSize& swapSize)
{
    const int maxW = std::max(1, swapSize.width());
    const int maxH = std::max(1, swapSize.height());
    const QRect bounded = qrect.normalized().toAlignedRect().intersected(QRect(0, 0, maxW, maxH));
    VkRect2D scissor{};
    scissor.offset = {bounded.x(), bounded.y()};
    scissor.extent = {
        static_cast<uint32_t>(std::max(1, bounded.width())),
        static_cast<uint32_t>(std::max(1, bounded.height()))
    };
    return scissor;
}

VkClearValue selectionOutlineColor()
{
    VkClearValue clear{};
    clear.color.float32[0] = 1.0f;
    clear.color.float32[1] = 0.9568627f;
    clear.color.float32[2] = 0.7607843f;
    clear.color.float32[3] = 1.0f;
    return clear;
}

bool applyVideoPreviewWheelZoom(PreviewInteractionState* state,
                               const QRectF& surfaceRect,
                               const QPointF& surfacePosition,
                               int deltaY)
{
    if (!state || deltaY == 0 || surfaceRect.isEmpty()) {
        return false;
    }
    const PreviewZoomResult zoom = PreviewViewTransform::zoomForWheel(
        surfaceRect,
        state->outputSize,
        vulkanPreviewCanvasMarginPx(),
        state->previewZoom,
        state->previewPanOffset,
        surfacePosition,
        deltaY);
    if (!zoom.changed) {
        return true;
    }
    state->previewZoom = zoom.zoom;
    state->previewPanOffset = zoom.panOffset;
    return true;
}

bool applyAudioPreviewWheelZoom(PreviewInteractionState* state,
                                const QRectF& surfaceRect,
                                const QPointF& surfacePosition,
                                int deltaY)
{
    if (!state || deltaY == 0 || surfaceRect.isEmpty()) {
        return false;
    }
    const qreal oldZoom = qBound<qreal>(1.0, state->previewZoom, 100000.0);
    const qreal factor = deltaY > 0 ? 1.18 : (1.0 / 1.18);
    const qreal newZoom = qBound<qreal>(1.0, oldZoom * factor, 100000.0);
    if (qFuzzyCompare(oldZoom, newZoom)) {
        return true;
    }
    const qreal oldVisible = qBound<qreal>(0.00001, 1.0 / oldZoom, 1.0);
    const qreal newVisible = qBound<qreal>(0.00001, 1.0 / newZoom, 1.0);
    const qreal focus = qBound<qreal>(
        0.0,
        (surfacePosition.x() - surfaceRect.left()) / qMax<qreal>(1.0, surfaceRect.width()),
        1.0);
    const qreal oldStart = qBound<qreal>(0.0, state->previewPanOffset.x(), qMax<qreal>(0.0, 1.0 - oldVisible));
    const qreal focusNorm = oldStart + (focus * oldVisible);
    const qreal newStart = qBound<qreal>(0.0, focusNorm - (focus * newVisible), qMax<qreal>(0.0, 1.0 - newVisible));
    state->previewZoom = newZoom;
    state->previewPanOffset.setX(newStart);
    return true;
}

bool audioSeekSampleAtSurfacePosition(const PreviewInteractionState& state,
                                      const QRectF& surfaceRect,
                                      const QPointF& surfacePosition,
                                      int64_t* sampleOut)
{
    if (!sampleOut || state.viewMode != PreviewSurface::ViewMode::Audio) {
        return false;
    }
    const QRectF safeRect = surfaceRect.adjusted(18.0, 18.0, -18.0, -18.0);
    const QRectF panel = safeRect.adjusted(12.0, 12.0, -12.0, -12.0);
    const QRectF waveRect = panel.adjusted(24.0, 118.0, -24.0, -36.0);
    const qreal rulerGutterWidth = qBound<qreal>(32.0, waveRect.width() * 0.12, 56.0);
    const QRectF graphRect(waveRect.left() + rulerGutterWidth,
                           waveRect.top(),
                           qMax<qreal>(1.0, waveRect.width() - rulerGutterWidth),
                           waveRect.height());
    if (!graphRect.contains(surfacePosition)) {
        return false;
    }

    const TimelineClip* clip = nullptr;
    for (const TimelineClip& candidate : state.clips) {
        const int64_t clipStartSample = clipTimelineStartSamples(candidate);
        const int64_t clipEndSample = clipStartSample + frameToSamples(candidate.durationFrames);
        const bool withinClip = state.currentSample >= clipStartSample && state.currentSample < clipEndSample;
        const bool includeForAudioView =
            clipAudioPlaybackEnabled(candidate) &&
            (candidate.id == state.selectedClipId || withinClip);
        const bool includeAsFallback = clipIsAudioOnly(candidate) && withinClip;
        if (includeForAudioView || includeAsFallback) {
            clip = &candidate;
            break;
        }
    }
    if (!clip) {
        return false;
    }

    const int rowCount = qBound(2, static_cast<int>(waveRect.height()) / 88, 6);
    const int64_t clipStartSample = clipTimelineStartSamples(*clip);
    const int64_t clipSamples = resolvedAudioPreviewClipSamples(*clip);
    const AudioPreviewViewport viewport = resolveAudioPreviewViewport(
        *clip, rowCount, state.previewZoom, state.previewPanOffset.x(), state.currentSample);
    const qreal localX = qBound<qreal>(0.0,
                                       (surfacePosition.x() - graphRect.left()) / qMax<qreal>(1.0, graphRect.width()),
                                       1.0);
    const qreal localY = qBound<qreal>(0.0,
                                       (surfacePosition.y() - graphRect.top()) / qMax<qreal>(1.0, graphRect.height()),
                                       0.99999);
    const int row = qBound(0, static_cast<int>(std::floor(localY * rowCount)), rowCount - 1);
    const qreal clickedVisibleNorm = qBound<qreal>(
        0.0,
        (static_cast<qreal>(row) + localX) / static_cast<qreal>(qMax(1, rowCount)),
        1.0);
    qreal targetClipNorm = viewport.startNorm + (clickedVisibleNorm * viewport.visibleFraction);
    if (viewport.playheadVisible) {
        const qreal deltaVisibleNorm = clickedVisibleNorm - viewport.playheadVisibleNorm;
        const int64_t deltaSamples = static_cast<int64_t>(
            std::llround(deltaVisibleNorm * viewport.visibleFraction * static_cast<qreal>(clipSamples - 1)));
        *sampleOut = qBound<int64_t>(
            clipStartSample,
            state.currentSample + deltaSamples,
            clipStartSample + clipSamples - 1);
        return true;
    }
    const int64_t targetOffset = static_cast<int64_t>(
        std::llround(targetClipNorm * static_cast<qreal>(clipSamples - 1)));
    *sampleOut = qBound<int64_t>(clipStartSample, clipStartSample + targetOffset, clipStartSample + clipSamples - 1);
    return true;
}

bool clipSupportsTranscriptOverlay(const TimelineClip& clip)
{
    return (clip.mediaType == ClipMediaType::Audio || clip.hasAudio) && clip.transcriptOverlay.enabled;
}

const TimelineClip* clipForId(const PreviewInteractionState* state, const QString& clipId)
{
    if (!state) {
        return nullptr;
    }
    for (const TimelineClip& clip : state->clips) {
        if (clip.id == clipId) {
            return &clip;
        }
    }
    return nullptr;
}

TimelineClip clipWithTransientTranscriptOverride(const PreviewInteractionState* state, const TimelineClip& clip)
{
    if (!state ||
        !state->transient.transcriptOverrideActive ||
        state->transient.transcriptOverrideClipId != clip.id) {
        return clip;
    }

    TimelineClip effective = clip;
    effective.transcriptOverlay.translationX = state->transient.transcriptTranslationOverride.x();
    effective.transcriptOverlay.translationY = state->transient.transcriptTranslationOverride.y();
    effective.transcriptOverlay.useManualPlacement = true;
    if (state->transient.transcriptSizeOverride.width() > 0.0) {
        effective.transcriptOverlay.boxWidth = state->transient.transcriptSizeOverride.width();
    }
    if (state->transient.transcriptSizeOverride.height() > 0.0) {
        effective.transcriptOverlay.boxHeight = state->transient.transcriptSizeOverride.height();
    }
    return effective;
}

TimelineClip::TransformKeyframe transformWithTransientOverride(const PreviewInteractionState* state,
                                                               const QString& clipId,
                                                               const TimelineClip::TransformKeyframe& fallback)
{
    if (state &&
        state->transient.transformOverrideActive &&
        state->transient.transformOverrideClipId == clipId) {
        return state->transient.transformOverride;
    }
    return fallback;
}

void clearVulkanDragOverrides(PreviewInteractionState* state)
{
    if (!state) {
        return;
    }
    state->transient.transformOverrideActive = false;
    state->transient.transformOverrideClipId.clear();
    state->transient.transformOverride = TimelineClip::TransformKeyframe();
    state->transient.transcriptOverrideActive = false;
    state->transient.transcriptOverrideClipId.clear();
    state->transient.transcriptTranslationOverride = QPointF();
    state->transient.transcriptSizeOverride = QSizeF();
}

QRectF transcriptOverlayBoundsForClip(const PreviewInteractionState* state,
                                     const TimelineClip& clip,
                                     const PreviewViewTransform& viewTransform,
                                     bool requireInteraction = true)
{
    const TimelineClip effectiveClip = clipWithTransientTranscriptOverride(state, clip);
    if (!state || !clipSupportsTranscriptOverlay(effectiveClip)) {
        return QRectF();
    }
    if (requireInteraction && !state->transcriptOverlayInteractionEnabled) {
        return QRectF();
    }
    const QSize safeOutputSize = state->outputSize.isValid() ? state->outputSize : QSize(1080, 1920);
    const QString transcriptPath = activeTranscriptPathForClipFile(effectiveClip.filePath);
    const std::shared_ptr<const TranscriptRuntimeDocument> runtimeDocument =
        loadTranscriptRuntimeDocument(transcriptPath);
    const QVector<TranscriptSection>& sections =
        runtimeDocument ? runtimeDocument->sections : QVector<TranscriptSection>{};
    const int64_t sourceFrame =
        transcriptFrameForClipAtTimelineSample(effectiveClip, state->currentSample, state->renderSyncMarkers);
    const TranscriptOverlayLayout layout = transcriptOverlayLayoutAtSourceFrame(effectiveClip, sections, sourceFrame);
    if (layout.lines.isEmpty()) {
        return QRectF();
    }

    const QRectF outputRect = transcriptOverlayRectInOutputSpace(
        effectiveClip, safeOutputSize, transcriptPath, sections, sourceFrame);
    if (outputRect.width() <= 0.0 || outputRect.height() <= 0.0) {
        return QRectF();
    }

    const QPointF center = viewTransform.outputToScreen(outputRect.center());
    const QPointF previewScale = viewTransform.outputScale();
    const QSizeF size(outputRect.width() * qMax<qreal>(0.0001, previewScale.x()),
                      outputRect.height() * qMax<qreal>(0.0001, previewScale.y()));
    if (size.width() <= 0.0 || size.height() <= 0.0) {
        return QRectF();
    }
    return QRectF(center.x() - (size.width() * 0.5),
                  center.y() - (size.height() * 0.5),
                  size.width(),
                  size.height());
}

struct VulkanInteractionOverlayInfo {
    QString clipId;
    QRectF bounds;
    QRectF rightHandle;
    QRectF bottomHandle;
    QRectF cornerHandle;
    PreviewOverlayKind kind = PreviewOverlayKind::VisualClip;
    QTransform clipToScreen;
    QRectF localRect;
    TimelineClip::TransformKeyframe transform;
    QSizeF clipPixelSize;
};

using VulkanInteractionOverlayInfos = QVector<VulkanInteractionOverlayInfo>;

VulkanInteractionOverlayInfos collectVulkanInteractionInfos(const PreviewInteractionState* state,
                                                          const QRectF& surfaceRect)
{
    VulkanInteractionOverlayInfos infos;
    if (!state) {
        return infos;
    }
    if (state->vulkanFrameStatuses.isEmpty()) {
        return infos;
    }
    if (surfaceRect.isEmpty()) {
        return infos;
    }
    const PreviewViewTransform viewTransform(
        surfaceRect,
        state->outputSize,
        vulkanPreviewCanvasMarginPx(),
        state->previewZoom,
        state->previewPanOffset);
    const QPointF previewScale = viewTransform.outputScale();
    const QHash<QString, QSize> sourceSizes = [&state]() {
        QHash<QString, QSize> sizes;
        for (const TimelineClip& clip : state->clips) {
            if (!clip.id.isEmpty()) {
                sizes.insert(clip.id, clip.sourceFrameSize);
            }
        }
        return sizes;
    }();

    for (const VulkanPreviewClipFrameStatus& status : state->vulkanFrameStatuses) {
        if (!status.active || status.drawSuppressed) {
            continue;
        }
        const TimelineClip* clip = clipForId(state, status.clipId);
        const QRectF fitted = viewTransform.fittedClipRect(
            sourceSizes.value(status.clipId),
            status.frameSize);
        const TimelineClip::TransformKeyframe transform =
            transformWithTransientOverride(state, status.clipId, status.transform);
        const PreviewClipGeometry geometry = PreviewViewTransform::clipGeometry(
            fitted,
            previewScale,
            QPointF(transform.translationX, transform.translationY),
            transform.rotation,
            QPointF(transform.scaleX, transform.scaleY));
        const QRectF overlayBounds = [&]() {
            if (!clip) {
                return geometry.bounds;
            }
            if (clipSupportsTranscriptOverlay(*clip) && state->transcriptOverlayInteractionEnabled) {
                const QRectF candidate = transcriptOverlayBoundsForClip(state, *clip, viewTransform);
                if (candidate.isValid() && candidate.width() > 1.0 && candidate.height() > 1.0) {
                    return candidate;
                }
            }
            return geometry.bounds;
        }();
        const PreviewResizeHandles handles = PreviewViewTransform::resizeHandlesForBounds(overlayBounds);
        const bool transcriptBoundsValid = !overlayBounds.isEmpty() &&
            overlayBounds != geometry.bounds &&
            (clip && clipSupportsTranscriptOverlay(*clip) && state->transcriptOverlayInteractionEnabled);
        infos.push_back(VulkanInteractionOverlayInfo{
            status.clipId,
            overlayBounds,
            handles.right,
            handles.bottom,
            handles.corner,
            transcriptBoundsValid
                    ? PreviewOverlayKind::TranscriptOverlay
                    : PreviewOverlayKind::VisualClip,
            geometry.clipToScreen,
            geometry.localRect,
            transform,
            geometry.clipPixelSize});
    }
    return infos;
}

QString clipIdAtPositionForVulkan(const VulkanInteractionOverlayInfos& infos, const QPointF& position)
{
    for (int i = infos.size() - 1; i >= 0; --i) {
        const VulkanInteractionOverlayInfo& info = infos.at(i);
        if (info.bounds.contains(position)) {
            return info.clipId;
        }
    }
    return QString();
}

QPointF mapScreenPointToNormalizedClipForVulkan(const VulkanInteractionOverlayInfo& info,
                                                const QPointF& screenPoint)
{
    if (info.clipPixelSize.width() > 1.0 &&
        info.clipPixelSize.height() > 1.0 &&
        !info.clipToScreen.isIdentity()) {
        bool invertible = false;
        const QTransform inverse = info.clipToScreen.inverted(&invertible);
        if (invertible) {
            const QPointF localPoint = inverse.map(screenPoint);
            const QRectF localRect(-info.clipPixelSize.width() / 2.0,
                                   -info.clipPixelSize.height() / 2.0,
                                   info.clipPixelSize.width(),
                                   info.clipPixelSize.height());
            return QPointF(
                qBound<qreal>(0.0, (localPoint.x() - localRect.left()) / qMax<qreal>(1.0, localRect.width()), 1.0),
                qBound<qreal>(0.0, (localPoint.y() - localRect.top()) / qMax<qreal>(1.0, localRect.height()), 1.0));
        }
    }

    return QPointF(
        qBound<qreal>(0.0, (screenPoint.x() - info.bounds.left()) / qMax<qreal>(1.0, info.bounds.width()), 1.0),
        qBound<qreal>(0.0, (screenPoint.y() - info.bounds.top()) / qMax<qreal>(1.0, info.bounds.height()), 1.0));
}

QPointF mapNormalizedClipPointToScreenForVulkan(const VulkanInteractionOverlayInfo& info,
                                                const QPointF& normalizedPoint)
{
    const qreal x = qBound<qreal>(0.0, normalizedPoint.x(), 1.0);
    const qreal y = qBound<qreal>(0.0, normalizedPoint.y(), 1.0);
    if (info.clipPixelSize.width() > 1.0 &&
        info.clipPixelSize.height() > 1.0 &&
        !info.clipToScreen.isIdentity()) {
        const QRectF localRect(-info.clipPixelSize.width() / 2.0,
                               -info.clipPixelSize.height() / 2.0,
                               info.clipPixelSize.width(),
                               info.clipPixelSize.height());
        const QPointF localPoint =
            PreviewViewTransform::localPointForNormalizedPoint(QPointF(x, y), localRect);
        return info.clipToScreen.map(localPoint);
    }
    return QPointF(info.bounds.left() + (x * info.bounds.width()),
                   info.bounds.top() + (y * info.bounds.height()));
}

bool lookupVulkanInteractionInfo(const VulkanInteractionOverlayInfos& infos,
                                 const QString& clipId,
                                 VulkanInteractionOverlayInfo* out);

bool dispatchFaceDetectionsBoxAtPosition(const PreviewInteractionState* state,
                                     const VulkanInteractionOverlayInfos& infos,
                                     const QPointF& surfacePosition,
                                     const std::function<void(const QString&, int, const QString&, int64_t, qreal, qreal, qreal)>& callback,
                                     const std::function<void(const QString&)>& statusCallback)
{
    QElapsedTimer clickTimer;
    clickTimer.start();
    if (!state || !callback) {
        if (statusCallback) {
            statusCallback(QStringLiteral("Face box click ignored: FaceDetections click callback is not installed."));
        }
        return false;
    }
    if (state->facedetectionsOverlays.isEmpty()) {
        if (statusCallback) {
            statusCallback(QStringLiteral("Face box click ignored: no FaceDetections boxes are available at this frame."));
        }
        return false;
    }
    const VulkanPreviewFacestreamOverlay* nearestOverlay = nullptr;
    qreal nearestDistanceSq = std::numeric_limits<qreal>::max();
    int testedOverlays = 0;
    int mappedOverlays = 0;
    for (int overlayIndex = state->facedetectionsOverlays.size() - 1; overlayIndex >= 0; --overlayIndex) {
        const VulkanPreviewFacestreamOverlay& overlay = state->facedetectionsOverlays.at(overlayIndex);
        ++testedOverlays;
        if (!overlay.boxNorm.isValid() || overlay.boxNorm.isEmpty()) {
            continue;
        }
        VulkanInteractionOverlayInfo info;
        if (!lookupVulkanInteractionInfo(infos, overlay.clipId, &info)) {
            continue;
        }
        ++mappedOverlays;
        const QPointF p1 = mapNormalizedClipPointToScreenForVulkan(info, overlay.boxNorm.topLeft());
        const QPointF p2 = mapNormalizedClipPointToScreenForVulkan(info, overlay.boxNorm.topRight());
        const QPointF p3 = mapNormalizedClipPointToScreenForVulkan(info, overlay.boxNorm.bottomRight());
        const QPointF p4 = mapNormalizedClipPointToScreenForVulkan(info, overlay.boxNorm.bottomLeft());
        QPainterPath boxPath;
        boxPath.moveTo(p1);
        boxPath.lineTo(p2);
        boxPath.lineTo(p3);
        boxPath.lineTo(p4);
        boxPath.closeSubpath();
        QPainterPath hitPath = boxPath;
        const QRectF hitBounds = boxPath.boundingRect();
        constexpr qreal kMinOverlayHitSizePx = 18.0;
        constexpr qreal kNearestOverlayRadiusPx = 28.0;
        if (hitBounds.width() < kMinOverlayHitSizePx || hitBounds.height() < kMinOverlayHitSizePx) {
            const QPointF center = hitBounds.center();
            const QRectF expandedRect(center.x() - (kMinOverlayHitSizePx * 0.5),
                                      center.y() - (kMinOverlayHitSizePx * 0.5),
                                      kMinOverlayHitSizePx,
                                      kMinOverlayHitSizePx);
            hitPath.addRect(expandedRect);
        }
        if (!hitPath.contains(surfacePosition)) {
            const QPointF center = hitBounds.center();
            const qreal dx = surfacePosition.x() - center.x();
            const qreal dy = surfacePosition.y() - center.y();
            const qreal distanceSq = dx * dx + dy * dy;
            if (distanceSq <= (kNearestOverlayRadiusPx * kNearestOverlayRadiusPx) &&
                distanceSq < nearestDistanceSq) {
                nearestDistanceSq = distanceSq;
                nearestOverlay = &overlay;
            }
            continue;
        }
        const QPointF center = overlay.boxNorm.center();
        const qreal boxSideNorm =
            qBound<qreal>(0.01, qMax(overlay.boxNorm.width(), overlay.boxNorm.height()), 1.0);
        qInfo().noquote()
            << QStringLiteral("Face box click dispatch: clip=%1 track=%2 stream=%3 source_frame=%4 hit_test_ms=%5 overlays=%6 mapped=%7")
                   .arg(overlay.clipId)
                   .arg(overlay.trackId)
                   .arg(overlay.streamId.isEmpty() ? QStringLiteral("<empty>") : overlay.streamId)
                   .arg(overlay.sourceFrame)
                   .arg(clickTimer.elapsed())
                   .arg(testedOverlays)
                   .arg(mappedOverlays);
        callback(overlay.clipId,
                 overlay.trackId,
                 overlay.streamId,
                 overlay.sourceFrame,
                 center.x(),
                 center.y(),
                 boxSideNorm);
        return true;
    }
    if (nearestOverlay) {
        const QPointF center = nearestOverlay->boxNorm.center();
        const qreal boxSideNorm =
            qBound<qreal>(0.01, qMax(nearestOverlay->boxNorm.width(), nearestOverlay->boxNorm.height()), 1.0);
        qInfo().noquote()
            << QStringLiteral("Face box click dispatch nearest: clip=%1 track=%2 stream=%3 source_frame=%4 hit_test_ms=%5 overlays=%6 mapped=%7")
                   .arg(nearestOverlay->clipId)
                   .arg(nearestOverlay->trackId)
                   .arg(nearestOverlay->streamId.isEmpty() ? QStringLiteral("<empty>") : nearestOverlay->streamId)
                   .arg(nearestOverlay->sourceFrame)
                   .arg(clickTimer.elapsed())
                   .arg(testedOverlays)
                   .arg(mappedOverlays);
        callback(nearestOverlay->clipId,
                 nearestOverlay->trackId,
                 nearestOverlay->streamId,
                 nearestOverlay->sourceFrame,
                 center.x(),
                 center.y(),
                 boxSideNorm);
        return true;
    }
    if (statusCallback) {
        statusCallback(QStringLiteral("Face box click ignored: no FaceDetections box at the clicked location."));
    }
    return false;
}

bool dispatchFaceDetectionsFocusClearAtPosition(
    const PreviewInteractionState* state,
    const VulkanInteractionOverlayInfos& infos,
    const QPointF& surfacePosition,
    const std::function<void(const QString&, int, const QString&, int64_t, qreal, qreal, qreal)>& callback,
    const std::function<void(const QString&)>& statusCallback)
{
    return dispatchFaceDetectionsBoxAtPosition(
        state,
        infos,
        surfacePosition,
        callback,
        statusCallback);
}

bool lookupVulkanInteractionInfo(const VulkanInteractionOverlayInfos& infos,
                                 const QString& clipId,
                                 VulkanInteractionOverlayInfo* outInfo);

bool updateHoveredFaceDetectionsBox(const PreviewInteractionState* state,
                                const VulkanInteractionOverlayInfos& infos,
                                const QPointF& surfacePosition)
{
    if (!state) {
        return false;
    }
    QString hoveredClipId;
    QString hoveredStreamId;
    int hoveredTrackId = -1;
    QString nearestClipId;
    QString nearestStreamId;
    int nearestTrackId = -1;
    qreal nearestDistanceSq = std::numeric_limits<qreal>::max();
    for (int overlayIndex = state->facedetectionsOverlays.size() - 1; overlayIndex >= 0; --overlayIndex) {
        const VulkanPreviewFacestreamOverlay& overlay = state->facedetectionsOverlays.at(overlayIndex);
        if (!overlay.boxNorm.isValid() || overlay.boxNorm.isEmpty()) {
            continue;
        }
        VulkanInteractionOverlayInfo info;
        if (!lookupVulkanInteractionInfo(infos, overlay.clipId, &info)) {
            continue;
        }
        QPainterPath boxPath;
        boxPath.moveTo(mapNormalizedClipPointToScreenForVulkan(info, overlay.boxNorm.topLeft()));
        boxPath.lineTo(mapNormalizedClipPointToScreenForVulkan(info, overlay.boxNorm.topRight()));
        boxPath.lineTo(mapNormalizedClipPointToScreenForVulkan(info, overlay.boxNorm.bottomRight()));
        boxPath.lineTo(mapNormalizedClipPointToScreenForVulkan(info, overlay.boxNorm.bottomLeft()));
        boxPath.closeSubpath();
        QPainterPath hitPath = boxPath;
        const QRectF hitBounds = boxPath.boundingRect();
        constexpr qreal kMinOverlayHitSizePx = 18.0;
        constexpr qreal kNearestOverlayRadiusPx = 28.0;
        if (hitBounds.width() < kMinOverlayHitSizePx || hitBounds.height() < kMinOverlayHitSizePx) {
            const QPointF center = hitBounds.center();
            const QRectF expandedRect(center.x() - (kMinOverlayHitSizePx * 0.5),
                                      center.y() - (kMinOverlayHitSizePx * 0.5),
                                      kMinOverlayHitSizePx,
                                      kMinOverlayHitSizePx);
            hitPath.addRect(expandedRect);
        }
        if (!hitPath.contains(surfacePosition)) {
            const QPointF center = hitBounds.center();
            const qreal dx = surfacePosition.x() - center.x();
            const qreal dy = surfacePosition.y() - center.y();
            const qreal distanceSq = dx * dx + dy * dy;
            if (distanceSq <= (kNearestOverlayRadiusPx * kNearestOverlayRadiusPx) &&
                distanceSq < nearestDistanceSq) {
                nearestDistanceSq = distanceSq;
                nearestClipId = overlay.clipId;
                nearestStreamId = overlay.streamId;
                nearestTrackId = overlay.trackId;
            }
            continue;
        }
        hoveredClipId = overlay.clipId;
        hoveredStreamId = overlay.streamId;
        hoveredTrackId = overlay.trackId;
        break;
    }
    if (hoveredTrackId < 0 && nearestTrackId >= 0) {
        hoveredClipId = nearestClipId;
        hoveredStreamId = nearestStreamId;
        hoveredTrackId = nearestTrackId;
    }

    PreviewInteractionTransientState& transient =
        const_cast<PreviewInteractionState*>(state)->transient;
    const bool changed =
        transient.hoveredFaceDetectionsTrackId != hoveredTrackId ||
        transient.hoveredFaceDetectionsClipId != hoveredClipId ||
        transient.hoveredFaceDetectionsId != hoveredStreamId;
    if (changed) {
        transient.hoveredFaceDetectionsTrackId = hoveredTrackId;
        transient.hoveredFaceDetectionsClipId = hoveredClipId;
        transient.hoveredFaceDetectionsId = hoveredStreamId;
    }
    return hoveredTrackId >= 0;
}

bool clipIdIsTitleForVulkan(const PreviewInteractionState* state, const QString& clipId)
{
    if (!state || clipId.isEmpty()) {
        return false;
    }
    for (const TimelineClip& clip : state->clips) {
        if (clip.id == clipId) {
            return clip.mediaType == ClipMediaType::Title;
        }
    }
    return false;
}

bool lookupVulkanInteractionInfo(const VulkanInteractionOverlayInfos& infos,
                                const QString& clipId,
                                VulkanInteractionOverlayInfo* outInfo)
{
    if (!outInfo) {
        return false;
    }
    for (const VulkanInteractionOverlayInfo& info : infos) {
        if (info.clipId == clipId) {
            *outInfo = info;
            return true;
        }
    }
    return false;
}

TimelineClip::TransformKeyframe currentTransformForVulkanClip(const PreviewInteractionState* state, const QString& clipId)
{
    if (!state) {
        return TimelineClip::TransformKeyframe();
    }
    for (const TimelineClip& clip : state->clips) {
        if (clip.id == clipId) {
            if (clip.mediaType == ClipMediaType::Title) {
                const int64_t localFrame = qMax<int64_t>(0,
                    static_cast<int64_t>(state->currentFramePosition) - clip.startFrame);
                const EvaluatedTitle evaluated = evaluateTitleAtLocalFrame(clip, localFrame);
                TimelineClip::TransformKeyframe keyframe;
                keyframe.frame = qBound<int64_t>(0, localFrame, qMax<int64_t>(0, clip.durationFrames - 1));
                keyframe.translationX = evaluated.x;
                keyframe.translationY = evaluated.y;
                keyframe.scaleX = 1.0;
                keyframe.scaleY = 1.0;
                return transformWithTransientOverride(state, clipId, keyframe);
            }
            return transformWithTransientOverride(state, clipId, evaluateClipRenderTransformAtPosition(
                clip,
                state->currentFramePosition,
                state->outputSize));
        }
    }
    return TimelineClip::TransformKeyframe();
}

class DirectVulkanPreviewHostWidget final : public QWidget {
public:
    DirectVulkanPreviewHostWidget(PreviewInteractionState* state,
                                  std::function<void()> updateCallback,
                                  QWidget* parent = nullptr)
        : QWidget(parent), m_state(state), m_updateCallback(std::move(updateCallback)) {}

protected:
    void wheelEvent(QWheelEvent* event) override
    {
        if (!event || !m_state || event->angleDelta().y() == 0) {
            QWidget::wheelEvent(event);
            return;
        }
        const QRectF surfaceRect = PreviewViewTransform::rectForWidget(
            this, PreviewSurfaceCoordinateSpace::DeviceSurface);
        const QPointF surfacePosition = PreviewViewTransform::pointForWidgetPoint(
            this, event->position(), PreviewSurfaceCoordinateSpace::DeviceSurface);
        if (m_state->viewMode == PreviewSurface::ViewMode::Audio) {
            if (applyAudioPreviewWheelZoom(m_state, surfaceRect, surfacePosition, event->angleDelta().y())) {
                if (m_updateCallback) {
                    m_updateCallback();
                }
                event->accept();
                return;
            }
            QWidget::wheelEvent(event);
            return;
        }
        if (m_state->viewMode == PreviewSurface::ViewMode::Audio) {
            QWidget::wheelEvent(event);
            return;
        }
        if (applyVideoPreviewWheelZoom(m_state, surfaceRect, surfacePosition, event->angleDelta().y())) {
            if (m_updateCallback) {
                m_updateCallback();
            }
            event->accept();
            return;
        }
        QWidget::wheelEvent(event);
    }

private:
    PreviewInteractionState* m_state = nullptr;
    std::function<void()> m_updateCallback;
};

void mvpForVulkanClipTransform(const QTransform& clipToSwapchain,
                               const QRectF& localRect,
                               const QSize& swapSize,
                               float outMvp[16])
{
    const float fullW = static_cast<float>(std::max(1, swapSize.width()));
    const float fullH = static_cast<float>(std::max(1, swapSize.height()));
    const float halfW = static_cast<float>(std::max<qreal>(1.0, localRect.width())) * 0.5f;
    const float halfH = static_cast<float>(std::max<qreal>(1.0, localRect.height())) * 0.5f;
    const float m11 = static_cast<float>(clipToSwapchain.m11());
    const float m12 = static_cast<float>(clipToSwapchain.m12());
    const float m21 = static_cast<float>(clipToSwapchain.m21());
    const float m22 = static_cast<float>(clipToSwapchain.m22());
    const float dx = static_cast<float>(clipToSwapchain.dx());
    const float dy = static_cast<float>(clipToSwapchain.dy());
    const float m[16] = {
        (2.0f * m11 * halfW) / fullW, (2.0f * m12 * halfW) / fullH, 0.f, 0.f,
        (2.0f * m21 * halfH) / fullW, (2.0f * m22 * halfH) / fullH, 0.f, 0.f,
        0.f,                0.f,                 1.f, 0.f,
        (2.0f * dx / fullW) - 1.0f,
        (2.0f * dy / fullH) - 1.0f,
        0.f,
        1.f
    };
    std::copy(std::begin(m), std::end(m), outMvp);
}

VkClearValue clipColor(const TimelineClip& clip, int ordinal, bool selected)
{
    QColor color = clip.color.isValid() ? clip.color : QColor::fromHsv((ordinal * 47) % 360, 150, 230);
    if (selected) {
        color = color.lighter(145);
    }
    VkClearValue clear{};
    clear.color.float32[0] = static_cast<float>(color.redF());
    clear.color.float32[1] = static_cast<float>(color.greenF());
    clear.color.float32[2] = static_cast<float>(color.blueF());
    clear.color.float32[3] = static_cast<float>(std::clamp(static_cast<double>(clip.opacity), 0.18, 1.0));
    return clear;
}

const VulkanPreviewClipFrameStatus* frameStatusForClip(const PreviewInteractionState* state, const QString& clipId)
{
    if (!state) {
        return nullptr;
    }
    for (const VulkanPreviewClipFrameStatus& status : state->vulkanFrameStatuses) {
        if (status.clipId == clipId) {
            return &status;
        }
    }
    return nullptr;
}

QByteArray curveLutRgbaBytes(const TimelineClip::GradingKeyframe& grade)
{
    const QVector<quint8> lutR = gradingCurveLut8(
        grade.curvePointsR, TimelineClip::kGradingCurveLutSize, grade.curveSmoothingEnabled);
    const QVector<quint8> lutG = gradingCurveLut8(
        grade.curvePointsG, TimelineClip::kGradingCurveLutSize, grade.curveSmoothingEnabled);
    const QVector<quint8> lutB = gradingCurveLut8(
        grade.curvePointsB, TimelineClip::kGradingCurveLutSize, grade.curveSmoothingEnabled);
    const QVector<quint8> lutL = gradingCurveLut8(
        grade.curvePointsLuma, TimelineClip::kGradingCurveLutSize, grade.curveSmoothingEnabled);
    if (lutR.size() != TimelineClip::kGradingCurveLutSize ||
        lutG.size() != TimelineClip::kGradingCurveLutSize ||
        lutB.size() != TimelineClip::kGradingCurveLutSize ||
        lutL.size() != TimelineClip::kGradingCurveLutSize) {
        return QByteArray();
    }
    QByteArray rgba;
    rgba.resize(TimelineClip::kGradingCurveLutSize * 4);
    for (int i = 0; i < TimelineClip::kGradingCurveLutSize; ++i) {
        rgba[i * 4 + 0] = static_cast<char>(lutR[i]);
        rgba[i * 4 + 1] = static_cast<char>(lutG[i]);
        rgba[i * 4 + 2] = static_cast<char>(lutB[i]);
        rgba[i * 4 + 3] = static_cast<char>(lutL[i]);
    }
    return rgba;
}

VkClearValue clipColorForStatus(const TimelineClip& clip,
                                int ordinal,
                                bool selected,
                                const VulkanPreviewClipFrameStatus* status)
{
    VkClearValue clear = clipColor(clip, ordinal, selected);
    if (!status || !status->hasFrame) {
        clear.color.float32[0] = 0.92f;
        clear.color.float32[1] = 0.05f;
        clear.color.float32[2] = 0.02f;
        clear.color.float32[3] = 0.95f;
        return clear;
    }
    if (status->hardwareFrame || status->gpuTexture) {
        clear.color.float32[0] = 0.08f;
        clear.color.float32[1] = status->exact ? 0.72f : 0.52f;
        clear.color.float32[2] = 0.38f;
    } else if (status->cpuImage) {
        clear.color.float32[0] = 0.84f;
        clear.color.float32[1] = status->exact ? 0.58f : 0.42f;
        clear.color.float32[2] = 0.12f;
    }
    clear.color.float32[3] = std::clamp(clear.color.float32[3], 0.35f, 1.0f);
    return clear;
}

VkClearValue facedetectionsOverlayColor(const PreviewInteractionState* state,
                                   const VulkanPreviewFacestreamOverlay& overlay)
{
    if (overlay.source.compare(QStringLiteral("raw_detection"), Qt::CaseInsensitive) == 0) {
        VkClearValue raw{};
        raw.color.float32[0] = 0.659f;
        raw.color.float32[1] = 0.333f;
        raw.color.float32[2] = 0.969f;
        raw.color.float32[3] = 0.90f;
        return raw;
    }
    if (overlay.source.compare(QStringLiteral("roi"), Qt::CaseInsensitive) == 0) {
        VkClearValue roi{};
        roi.color.float32[0] = 1.0f;
        roi.color.float32[1] = 0.667f;
        roi.color.float32[2] = 0.2f;
        roi.color.float32[3] = 0.95f;
        return roi;
    }
    if (state &&
        overlay.trackId >= 0 &&
        state->transient.hoveredFaceDetectionsTrackId == overlay.trackId &&
        state->transient.hoveredFaceDetectionsClipId == overlay.clipId &&
        state->transient.hoveredFaceDetectionsId == overlay.streamId) {
        VkClearValue hovered{};
        hovered.color.float32[0] = 0.96f;
        hovered.color.float32[1] = 0.82f;
        hovered.color.float32[2] = 0.99f;
        hovered.color.float32[3] = 1.0f;
        return hovered;
    }
    if (state &&
        overlay.trackId >= 0 &&
        state->selectedSpeakerAssignedFaceTrackIds.contains(overlay.trackId)) {
        VkClearValue assigned{};
        assigned.color.float32[0] = 0.29f;
        assigned.color.float32[1] = 0.87f;
        assigned.color.float32[2] = 0.50f;
        assigned.color.float32[3] = 0.95f;
        return assigned;
    }
    VkClearValue clear{};
    clear.color.float32[0] = 0.659f;
    clear.color.float32[1] = 0.333f;
    clear.color.float32[2] = 0.969f;
    clear.color.float32[3] = static_cast<float>(std::clamp(0.55 + (overlay.confidence * 0.35), 0.55, 0.95));
    return clear;
}

VkClearRect normalizedBoxToSwapchainRect(const QRectF& normalizedBox,
                                         const QTransform& clipToSwapchain,
                                         const QRectF& localRect,
                                         const QSize& swapSize,
                                         int lineInset = 0)
{
    QRectF localBox = PreviewViewTransform::localRectForNormalizedRect(normalizedBox, localRect);
    QRect mapped = clipToSwapchain.mapRect(localBox).toAlignedRect();
    mapped = mapped.adjusted(lineInset, lineInset, -lineInset, -lineInset);
    mapped = mapped.intersected(QRect(QPoint(0, 0), swapSize));
    VkClearRect rect{};
    rect.rect.offset = {mapped.x(), mapped.y()};
    rect.rect.extent = {
        static_cast<uint32_t>(std::max(1, mapped.width())),
        static_cast<uint32_t>(std::max(1, mapped.height()))
    };
    rect.baseArrayLayer = 0;
    rect.layerCount = 1;
    return rect;
}

void clearRect(QVulkanDeviceFunctions* funcs,
               VkCommandBuffer cb,
               const VkClearValue& value,
               const VkClearRect& rect)
{
    VkClearAttachment attachment{};
    attachment.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    attachment.colorAttachment = 0;
    attachment.clearValue = value;
    funcs->vkCmdClearAttachments(cb, 1, &attachment, 1, &rect);
}

void clearBoxOutline(QVulkanDeviceFunctions* funcs,
                     VkCommandBuffer cb,
                     const VkClearValue& value,
                     const VkClearRect& boxRect,
                     int thickness)
{
    const int x = boxRect.rect.offset.x;
    const int y = boxRect.rect.offset.y;
    const int w = static_cast<int>(boxRect.rect.extent.width);
    const int h = static_cast<int>(boxRect.rect.extent.height);
    const int t = std::max(1, std::min({thickness, std::max(1, w), std::max(1, h)}));
    auto makeRect = [](int rx, int ry, int rw, int rh) {
        VkClearRect rect{};
        rect.rect.offset = {rx, ry};
        rect.rect.extent = {
            static_cast<uint32_t>(std::max(1, rw)),
            static_cast<uint32_t>(std::max(1, rh))
        };
        rect.baseArrayLayer = 0;
        rect.layerCount = 1;
        return rect;
    };
    clearRect(funcs, cb, value, makeRect(x, y, w, t));
    clearRect(funcs, cb, value, makeRect(x, y + h - t, w, t));
    clearRect(funcs, cb, value, makeRect(x, y, t, h));
    clearRect(funcs, cb, value, makeRect(x + w - t, y, t, h));
}

const TimelineClip* selectedClipForTargetBox(const PreviewInteractionState* state)
{
    if (!state) {
        return nullptr;
    }
    const QString selectedId = state->selectedClipId.trimmed();
    if (!selectedId.isEmpty()) {
        for (const TimelineClip& clip : state->clips) {
            const TimelineClip::TransformKeyframe targetState =
                evaluateClipSpeakerFramingTargetAtFrame(clip, state->currentFrame);
            if (clip.id == selectedId &&
                qBound<qreal>(-1.0, targetState.scaleX, 1.0) > 0.0) {
                return &clip;
            }
        }
    }
    for (const TimelineClip& clip : state->clips) {
        const TimelineClip::TransformKeyframe targetState =
            evaluateClipSpeakerFramingTargetAtFrame(clip, state->currentFrame);
        if (qBound<qreal>(-1.0, targetState.scaleX, 1.0) > 0.0) {
            return &clip;
        }
    }
    return nullptr;
}

VkClearValue targetBoxOverlayColor()
{
    VkClearValue clear{};
    clear.color.float32[0] = 1.0f;
    clear.color.float32[1] = 0.886f;
    clear.color.float32[2] = 0.29f;
    clear.color.float32[3] = 0.95f;
    return clear;
}

VkClearRect targetBoxRectForComposite(const TimelineClip& clip,
                                      int64_t timelineFrame,
                                      const QRectF& compositeRect,
                                      const QSize& swapSize)
{
    const TimelineClip::TransformKeyframe targetState =
        evaluateClipSpeakerFramingTargetAtFrame(clip, timelineFrame);
    const qreal targetXNorm = qBound<qreal>(0.0, targetState.translationX, 1.0);
    const qreal targetYNorm = qBound<qreal>(0.0, targetState.translationY, 1.0);
    const qreal targetBoxNorm = qBound<qreal>(-1.0, targetState.scaleX, 1.0);
    const qreal centerX = compositeRect.left() + (targetXNorm * compositeRect.width());
    const qreal centerY = compositeRect.top() + (targetYNorm * compositeRect.height());
    const qreal side = qBound<qreal>(
        2.0,
        targetBoxNorm * qMax<qreal>(1.0, qMin<qreal>(compositeRect.width(), compositeRect.height())),
        4096.0);
    const qreal halfSide = side * 0.5;
    const QRectF targetRect(centerX - halfSide, centerY - halfSide, side, side);
    return clearRectFromQRect(targetRect, swapSize);
}

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
            return;
        }
        if (!isValid()) {
            markFailure(QStringLiteral("QVulkanWindow exposed but invalid; Vulkan surface or swapchain creation failed."));
        } else if (m_active) {
            *m_active = true;
            requestUpdate();
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
                requestUpdate();
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
            requestUpdate();
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
                requestUpdate();
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
            requestUpdate();
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
                requestUpdate();
                event->accept();
                return;
            }
        }

        if (m_faceStreamBoxRequested &&
            dispatchFaceDetectionsBoxAtPosition(
                m_state, infos, surfacePosition, m_faceStreamBoxRequested, m_faceStreamBoxClickStatus)) {
            requestUpdate();
            event->accept();
            return;
        }
        if (m_state->faceStreamAssignmentInteractionEnabled) {
            requestUpdate();
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
            requestUpdate();
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
            requestUpdate();
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
            requestUpdate();
            event->accept();
            return;
        }

        if (m_state->transient.speakerPickDragActive &&
            (event->buttons() & Qt::LeftButton)) {
            m_state->transient.speakerPickCurrentPos = surfacePosition;
            requestUpdate();
            event->accept();
            return;
        }

        if (activeInfo.kind == PreviewOverlayKind::TranscriptOverlay) {
            const qreal originWidth =
                activeInfo.bounds.width() / qMax<qreal>(0.0001, previewScale.x());
            const qreal originHeight =
                activeInfo.bounds.height() / qMax<qreal>(0.0001, previewScale.y());
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
            m_state->transient.transcriptTranslationOverride = transient.dragOriginTranscriptTranslation;
            if (m_state->transient.transcriptSizeOverride.width() <= 0.0 ||
                m_state->transient.transcriptSizeOverride.height() <= 0.0) {
                m_state->transient.transcriptSizeOverride = QSizeF(originWidth, originHeight);
            }
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
        requestUpdate();
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
            requestUpdate();
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
            requestUpdate();
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
            requestUpdate();
        }
        QVulkanWindow::keyPressEvent(event);
    }

    void keyReleaseEvent(QKeyEvent* event) override
    {
        if (event->key() == Qt::Key_Shift || event->key() == Qt::Key_Alt || event->key() == Qt::Key_Control) {
            const QPointF cursorPos = PreviewViewTransform::pointForWindowPoint(
                this, mapFromGlobal(QCursor::pos()), PreviewSurfaceCoordinateSpace::DeviceSurface);
            updatePreviewCursor(cursorPos);
            requestUpdate();
        }
        QVulkanWindow::keyReleaseEvent(event);
    }

    bool event(QEvent* event) override
    {
        if (!event) {
            return QVulkanWindow::event(event);
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
                    requestUpdate();
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
                requestUpdate();
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
                requestUpdate();
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
                requestUpdate();
                setCursor(Qt::PointingHandCursor);
                return;
            }
            if (m_state->transient.hoveredFaceDetectionsTrackId >= 0 ||
                !m_state->transient.hoveredFaceDetectionsClipId.isEmpty() ||
                !m_state->transient.hoveredFaceDetectionsId.isEmpty()) {
                m_state->transient.hoveredFaceDetectionsTrackId = -1;
                m_state->transient.hoveredFaceDetectionsClipId.clear();
                m_state->transient.hoveredFaceDetectionsId.clear();
                requestUpdate();
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
        requestUpdate();
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
        return;
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
    advanceRetiredClipHandoffResources();
    struct DecoderReadbackCandidate {
        VkImage image = VK_NULL_HANDLE;
        VkImageLayout layout = VK_IMAGE_LAYOUT_UNDEFINED;
        QSize size;
        VkFormat format = VK_FORMAT_UNDEFINED;
    } decoderReadbackCandidate;
    QHash<QString, DirectVulkanFrameHandoffPipeline::Result> frameHandoffResults;
    QHash<QString, bool> curveLutUploadResults;
    struct PreparedOverlayTexture {
        VulkanResources* resources = nullptr;
        QRectF bounds;
        bool ready = false;
    };
    struct PreparedTranscriptText {
        TimelineClip clip;
        TranscriptOverlayLayout layout;
        QRectF outputRect;
        QRectF bounds;
        QString speakerTitle;
        bool ready = false;
    };
    QHash<QString, PreparedTranscriptText> preparedTranscriptOverlays;
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
            frameHandoffResults.insert(
                status.clipId,
                handoffResources->pipeline->record(
                    cb,
                    status,
                    handoffResources->resources.get(),
                    m_owner ? m_owner->stats() : nullptr));
            const QByteArray curveLut = curveLutRgbaBytes(status.grading);
            if (!curveLut.isEmpty()) {
                curveLutUploadResults.insert(
                    status.clipId,
                    handoffResources->resources->uploadCurveLut(cb, curveLut));
            }
        }
        updateClipHandoffResourceStats();
    }
    const bool canDrawOverlays = m_pipeline && m_pipeline->isReady();
    if (state && canDrawOverlays) {
        const QRectF fullSwapRect(QPointF(0, 0), QSizeF(swapSize));
        const PreviewViewTransform overlayViewTransform(fullSwapRect,
                                                        state->outputSize,
                                                        vulkanPreviewCanvasMarginPx(),
                                                        state->previewZoom,
                                                        state->previewPanOffset);
        struct TranscriptOverlayCandidate {
            bool valid = false;
            TimelineClip clip;
            TranscriptOverlayLayout layout;
            QRectF outputRect;
            QRectF bounds;
            QString speakerTitle;
        };
        auto buildTranscriptOverlayCandidate =
            [&](const TimelineClip& clip,
                const TimelineClip& effectiveClip,
                int64_t samplePosition,
                const VulkanPreviewClipFrameStatus* status) -> TranscriptOverlayCandidate {
            TranscriptOverlayCandidate candidate;
            const QString transcriptPath = activeTranscriptPathForClipFile(effectiveClip.filePath);
            if (transcriptPath.isEmpty()) {
                return candidate;
            }
            const std::shared_ptr<const TranscriptRuntimeDocument> runtimeDocument =
                loadTranscriptRuntimeDocument(transcriptPath);
            const QVector<TranscriptSection>& sections =
                runtimeDocument ? runtimeDocument->sections : QVector<TranscriptSection>{};
            const int64_t sourceFrame =
                status && status->hasFrame && status->presentedSourceFrame >= 0
                    ? transcriptFrameForClipSourceFrame(effectiveClip, status->presentedSourceFrame)
                    : transcriptFrameForClipAtTimelineSample(effectiveClip,
                                                             samplePosition,
                                                             state->renderSyncMarkers);
            const TranscriptOverlayLayout overlayLayout =
                transcriptOverlayLayoutAtSourceFrame(effectiveClip, sections, sourceFrame);
            if (overlayLayout.lines.isEmpty()) {
                return candidate;
            }
            const QRectF outputRect = transcriptOverlayRectInOutputSpace(
                effectiveClip,
                state->outputSize,
                transcriptPath,
                sections,
                sourceFrame);
            const QPointF center = overlayViewTransform.outputToScreen(outputRect.center());
            const QPointF previewScale = overlayViewTransform.outputScale();
            const QRectF bounds(center.x() - ((outputRect.width() * previewScale.x()) * 0.5),
                                center.y() - ((outputRect.height() * previewScale.y()) * 0.5),
                                outputRect.width() * previewScale.x(),
                                outputRect.height() * previewScale.y());
            const QRectF localBounds(0.0, 0.0, outputRect.width(), outputRect.height());
            const QRectF localTextBounds = localBounds.adjusted(18.0, 14.0, -18.0, -14.0);
            const qreal fontPixelSize = effectiveClip.transcriptOverlay.fontPointSize;
            if (outputRect.width() <= 0.0 ||
                outputRect.height() <= 0.0 ||
                bounds.width() <= 0.0 ||
                bounds.height() <= 0.0 ||
                localTextBounds.width() <= 0.0 ||
                localTextBounds.height() <= 0.0 ||
                fontPixelSize <= 0.0) {
                return candidate;
            }
            QString speakerTitle;
            if (effectiveClip.transcriptOverlay.showSpeakerTitle) {
                speakerTitle = transcriptSpeakerTitleForSourceFrame(transcriptPath, sections, sourceFrame).trimmed();
            }
            candidate.valid = true;
            candidate.clip = effectiveClip;
            candidate.layout = overlayLayout;
            candidate.outputRect = outputRect;
            candidate.bounds = bounds;
            candidate.speakerTitle = speakerTitle;
            return candidate;
        };
        for (const TimelineClip& clip : state->clips) {
            const VulkanPreviewClipFrameStatus* status = frameStatusForClip(state, clip.id);
            if (!status || !status->active || status->drawSuppressed) {
                continue;
            }
            const TimelineClip effectiveClip = clipWithTransientTranscriptOverride(state, clip);
            if (!clipSupportsTranscriptOverlay(effectiveClip)) {
                continue;
            }
            const TranscriptOverlayCandidate candidate =
                buildTranscriptOverlayCandidate(
                    clip,
                    effectiveClip,
                    state->currentSample,
                    status);
            if (!candidate.valid) {
                continue;
            }
            preparedTranscriptOverlays.insert(
                clip.id,
                PreparedTranscriptText{
                    candidate.clip,
                    candidate.layout,
                    candidate.outputRect,
                    candidate.bounds,
                    candidate.speakerTitle,
                    true});
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
    }
    render_detail::SpeakerLabelOverlaySpec preparedSpeakerSpec;
    bool preparedSpeakerLabel = false;
    QSet<QString> preparedTranscriptAtlasClipIds;
    if (m_textRenderer && m_textRenderer->isReady()) {
        for (auto it = preparedTranscriptOverlays.cbegin(); it != preparedTranscriptOverlays.cend(); ++it) {
            const PreparedTranscriptText& transcript = it.value();
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
            }
        }
    }
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
        }
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
    bool audioWaitingForWaveform = false;
    if (renderDirectVulkanAudioFrame(
            DirectVulkanAudioRenderContext{state, m_devFuncs, m_audioTab.get(), cb, swapSize},
            &audioWaitingForWaveform)) {
        drawPreparedOverlay(preparedPlaybackStatusOverlay);
        m_devFuncs->vkCmdEndRenderPass(cb);
        m_owner->markPresented();
        m_window->frameReady();
        m_owner->markPreviewUpdateDelivered();
        if (state->playing || audioWaitingForWaveform) {
            m_owner->schedulePreviewUpdate();
        }
        return;
    }
    int64_t presentedSourceFrame = -1;
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
            if (sampledFrameReady) {
                decoderReadbackCandidate.image = handoffResult.image;
                decoderReadbackCandidate.layout = handoffResult.layout;
                decoderReadbackCandidate.size = handoffResult.size;
                decoderReadbackCandidate.format = handoffResult.format;
            }
            if (canDrawTexture && sampledFrameReady) {
                if (DirectVulkanPreviewStats* stats = m_owner->stats()) {
                    ++stats->textureDraws;
                    ++stats->activeClipDraws;
                }
                VulkanPipeline::Push push{};
                mvpForVulkanClipTransform(effectiveClipGeometry.clipToScreen,
                                          effectiveClipGeometry.localRect,
                                          swapSize,
                                          push.mvp);
                push.opacity = static_cast<float>(std::clamp(static_cast<double>(status->grading.opacity), 0.0, 1.0));
                if (status) {
                    push.brightness = static_cast<float>(status->grading.brightness);
                    push.contrast = static_cast<float>(status->grading.contrast);
                    push.saturation = static_cast<float>(status->grading.saturation);
                    push.shadows[0] = static_cast<float>(status->grading.shadowsR);
                    push.shadows[1] = static_cast<float>(status->grading.shadowsG);
                    push.shadows[2] = static_cast<float>(status->grading.shadowsB);
                    push.midtones[0] = static_cast<float>(status->grading.midtonesR);
                    push.midtones[1] = static_cast<float>(status->grading.midtonesG);
                    push.midtones[2] = static_cast<float>(status->grading.midtonesB);
                    push.highlights[0] = static_cast<float>(status->grading.highlightsR);
                    push.highlights[1] = static_cast<float>(status->grading.highlightsG);
                    push.highlights[2] = static_cast<float>(status->grading.highlightsB);
                    push.shadows[3] = status->curveLutApplied ? 1.0f : 0.0f;
                    push.midtones[3] = static_cast<float>(std::max<qreal>(0.0, status->maskFeather));
                    push.highlights[3] = static_cast<float>(std::max<qreal>(0.01, status->maskFeatherGamma));
                    const auto curveUploadIt = curveLutUploadResults.constFind(status->clipId);
                    if (curveUploadIt != curveLutUploadResults.constEnd() && curveUploadIt.value()) {
                        if (DirectVulkanPreviewStats* stats = m_owner->stats()) {
                            stats->lastCurveLutApplied = status->curveLutApplied;
                        }
                    } else if (status->curveLutApplied) {
                        if (DirectVulkanPreviewStats* stats = m_owner->stats()) {
                            stats->lastUnsupportedEffect = QStringLiteral("curve_lut_upload_failed");
                        }
                    }
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
                m_pipeline->bindAndDraw(cb, viewport, scissor, handoffResult.descriptorSet, push);
            } else {
                if (DirectVulkanPreviewStats* stats = m_owner->stats()) {
                    ++stats->explicitFailureDraws;
                    if (handoffAttempted && !sampledFrameReady) {
                        stats->lastHandoffMode = QStringLiteral("attempted_not_sampled");
                    } else if (!status) {
                        stats->lastHandoffMode = QStringLiteral("decode_status_missing");
                        stats->lastHandoffError = QStringLiteral("No Vulkan decode status exists for the active clip.");
                    } else if (!status->hasFrame) {
                        stats->lastHandoffMode = QStringLiteral("decoded_frame_unavailable");
                        stats->lastHandoffError = status->missingReason.isEmpty()
                            ? QStringLiteral("Active Vulkan clip has no usable decoded frame.")
                            : status->missingReason;
                    } else if (status->frame.hasCpuImage() &&
                               !status->externalVulkanFrame &&
                               !status->frame.hasHardwareFrame()) {
                        stats->lastHandoffMode = QStringLiteral("vulkan_handoff_required");
                        stats->lastHandoffError = QStringLiteral(
                            "Direct Vulkan preview requires a hardware/external frame. CPU upload fallback is disabled.");
                    } else if (!canDrawTexture) {
                        stats->lastHandoffMode = QStringLiteral("texture_pipeline_unavailable");
                        stats->lastHandoffError = QStringLiteral("Vulkan texture pipeline or descriptor set is unavailable.");
                    }
                }
            }
            const auto transcriptOverlayIt = preparedTranscriptOverlays.constFind(clip.id);
            if (transcriptOverlayIt != preparedTranscriptOverlays.constEnd() &&
                transcriptOverlayIt.value().ready &&
                preparedTranscriptAtlasClipIds.contains(clip.id) &&
                m_textRenderer &&
                m_textRenderer->isReady()) {
                const PreparedTranscriptText& transcript = transcriptOverlayIt.value();
                m_textRenderer->drawTranscriptOverlay(cb,
                                                      swapSize,
                                                      state->outputSize,
                                                      compositeRect,
                                                      transcript.clip,
                                                      transcript.layout,
                                                      transcript.outputRect,
                                                      transcript.speakerTitle);
            }
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
        if (preparedSpeakerLabel && m_speakerTextRenderer && m_speakerTextRenderer->isReady()) {
            m_speakerTextRenderer->drawSpeakerLabel(cb,
                                                    swapSize,
                                                    state->outputSize,
                                                    compositeRect,
                                                    preparedSpeakerSpec);
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
            const VkClearRect boxRect = normalizedBoxToSwapchainRect(
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
            const VkClearRect boxRect = normalizedBoxToSwapchainRect(
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

bool directVulkanPreviewDebugChromeEnabled()
{
    return vulkanPreviewDebugChromeEnabled();
}

bool directVulkanPreviewOptimalPresentEnabled()
{
    return vulkanPreviewOptimalPresentEnabled();
}

bool directVulkanPreviewDirectSwapchainVisible()
{
    return vulkanPreviewDirectSwapchainVisible();
}

QString directVulkanPreviewVisiblePathLabel()
{
    return vulkanPreviewVisiblePathLabel();
}

QWidget* createDirectVulkanPreviewHostWidget(PreviewInteractionState* state,
                                             std::function<void()> updateCallback,
                                             QWidget* parent)
{
    return new DirectVulkanPreviewHostWidget(state, std::move(updateCallback), parent);
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
