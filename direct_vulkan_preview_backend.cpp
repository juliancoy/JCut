#include "direct_vulkan_preview_backend.h"
#include "direct_vulkan_preview_presenter.h"
#include "audio_preview_support.h"
#include "direct_vulkan_preview_audio.h"
#include "preview_speaker_profiles.h"
#include "preview_view_transform.h"
#include "preview_overlay_model.h"
#include "editor_shared.h"
#include "render_internal.h"
#include "titles.h"
#include "vulkan_detector_frame_handoff.h"
#include "vulkan_audio_tab.h"
#include "vulkan_pipeline.h"
#include "vulkan_resources.h"
#include "waveform_service.h"
#include "loiacono/loiacono_rolling.h"

#include <QDebug>
#include <QByteArray>
#include <QCoreApplication>
#include <QCryptographicHash>
#include <QDateTime>
#include <QHash>
#include <QFile>
#include <QFileInfo>
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
#include <QTextDocument>
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
#include <vector>

namespace {
constexpr qint64 kPipelineThumbnailReadbackMinIntervalMs = 250;

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

QString transcriptSpeakerTitleHtml(const QString& title, const QColor& color)
{
    const QString safeTitle = title.trimmed().toHtmlEscaped();
    if (safeTitle.isEmpty()) {
        return QString();
    }
    return QStringLiteral(
               "<div style=\"text-align:center;"
               " font-weight:700;"
               " letter-spacing:0.02em;"
               " font-size:0.62em;"
               " margin:0 0 0.30em 0;"
               " color:%1;\">%2</div>")
        .arg(color.name(QColor::HexRgb), safeTitle);
}

QString transcriptOverlayTextureKey(const TimelineClip& clip,
                                    const QRectF& bounds,
                                    const QRectF& textBounds,
                                    qreal fontPixelSize,
                                    const QString& shadowHtml,
                                    const QString& textHtml)
{
    const QString keyMaterial =
        clip.id + QLatin1Char('|') +
        QString::number(qRound(bounds.width())) + QLatin1Char('|') +
        QString::number(qRound(bounds.height())) + QLatin1Char('|') +
        QString::number(qRound(textBounds.width())) + QLatin1Char('|') +
        QString::number(qRound(textBounds.height())) + QLatin1Char('|') +
        clip.transcriptOverlay.fontFamily + QLatin1Char('|') +
        QString::number(fontPixelSize, 'f', 3) + QLatin1Char('|') +
        (clip.transcriptOverlay.bold ? QStringLiteral("1") : QStringLiteral("0")) + QLatin1Char('|') +
        (clip.transcriptOverlay.italic ? QStringLiteral("1") : QStringLiteral("0")) + QLatin1Char('|') +
        (clip.transcriptOverlay.showBackground ? QStringLiteral("1") : QStringLiteral("0")) + QLatin1Char('|') +
        shadowHtml + QLatin1Char('|') + textHtml;
    const QByteArray digest = QCryptographicHash::hash(keyMaterial.toUtf8(), QCryptographicHash::Sha1);
    return QString::fromLatin1(digest.toHex());
}

render_detail::OverlayImage renderTranscriptOverlay(const TimelineClip& clip,
                                                    const QRectF& bounds,
                                                    const QRectF& textBounds,
                                                    qreal fontPixelSize,
                                                    const QString& shadowHtml,
                                                    const QString& textHtml)
{
    const int imageWidth = qMax(1, qRound(bounds.width()));
    const int imageHeight = qMax(1, qRound(bounds.height()));
    QImage image(imageWidth, imageHeight, QImage::Format_RGBA8888_Premultiplied);
    image.fill(Qt::transparent);

    QPainter painter(&image);
    painter.setRenderHint(QPainter::Antialiasing, true);
    painter.setRenderHint(QPainter::TextAntialiasing, true);

    if (clip.transcriptOverlay.showBackground) {
        painter.setPen(Qt::NoPen);
        painter.setBrush(QColor(0, 0, 0, 120));
        painter.drawRoundedRect(QRectF(0.0, 0.0, imageWidth, imageHeight), 14.0, 14.0);
    }

    QFont font(clip.transcriptOverlay.fontFamily);
    if (fontPixelSize <= 0.0) {
        render_detail::OverlayImage overlay;
        overlay.width = image.width();
        overlay.height = image.height();
        overlay.rgbaPremultiplied = QByteArray(
            reinterpret_cast<const char*>(image.constBits()),
            static_cast<int>(image.sizeInBytes()));
        return overlay;
    }
    setFontPixelSizeRobust(&font, fontPixelSize, painter.device());
    font.setBold(clip.transcriptOverlay.bold);
    font.setItalic(clip.transcriptOverlay.italic);

    QTextDocument shadowDoc;
    shadowDoc.setDefaultFont(font);
    shadowDoc.setDocumentMargin(0.0);
    shadowDoc.setTextWidth(textBounds.width());
    shadowDoc.setHtml(shadowHtml);

    QTextDocument textDoc;
    textDoc.setDefaultFont(font);
    textDoc.setDocumentMargin(0.0);
    textDoc.setTextWidth(textBounds.width());
    textDoc.setHtml(textHtml);

    const qreal widthScale = textDoc.size().width() > textBounds.width()
                                 ? textBounds.width() / textDoc.size().width()
                                 : 1.0;
    const qreal heightScale = textDoc.size().height() > textBounds.height()
                                  ? textBounds.height() / textDoc.size().height()
                                  : 1.0;
    const qreal docScale = qMin(widthScale, heightScale);
    const qreal scaledDocHeight = textDoc.size().height() * docScale;
    const qreal textY = textBounds.top() + qMax<qreal>(0.0, (textBounds.height() - scaledDocHeight) / 2.0);

    painter.translate(textBounds.left() + 3.0, textY + 3.0);
    if (docScale < 0.999) {
        painter.scale(docScale, docScale);
    }
    shadowDoc.drawContents(&painter);
    if (docScale < 0.999) {
        painter.scale(1.0 / docScale, 1.0 / docScale);
    }
    painter.translate(-3.0, -3.0);
    if (docScale < 0.999) {
        painter.scale(docScale, docScale);
    }
    textDoc.drawContents(&painter);
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

    DirectVulkanPreviewWindow* m_owner = nullptr;
    QVulkanWindow* m_window = nullptr;
    QVulkanDeviceFunctions* m_devFuncs = nullptr;
    std::unique_ptr<VulkanResources> m_resources;
    std::unique_ptr<VulkanResources> m_overlayResources;
    std::unique_ptr<VulkanPipeline> m_pipeline;
    std::unique_ptr<jcut::VulkanAudioTab> m_audioTab;
    std::unique_ptr<jcut::vulkan_detector::VulkanDetectorFrameHandoff> m_frameHandoff;
    QString m_lastHandoffError;
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
                                     const PreviewViewTransform& viewTransform)
{
    const TimelineClip effectiveClip = clipWithTransientTranscriptOverride(state, clip);
    if (!state || !clipSupportsTranscriptOverlay(effectiveClip) || !state->transcriptOverlayInteractionEnabled) {
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

bool dispatchFaceStreamBoxAtPosition(const PreviewInteractionState* state,
                                     const VulkanInteractionOverlayInfos& infos,
                                     const QPointF& surfacePosition,
                                     const std::function<void(const QString&, int, const QString&, int64_t, qreal, qreal, qreal)>& callback)
{
    if (!state || !callback) {
        return false;
    }
    for (int overlayIndex = state->facestreamOverlays.size() - 1; overlayIndex >= 0; --overlayIndex) {
        const VulkanPreviewFacestreamOverlay& overlay = state->facestreamOverlays.at(overlayIndex);
        if (!overlay.boxNorm.isValid() || overlay.boxNorm.isEmpty()) {
            continue;
        }
        VulkanInteractionOverlayInfo info;
        if (!lookupVulkanInteractionInfo(infos, overlay.clipId, &info)) {
            continue;
        }
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
        if (!boxPath.contains(surfacePosition)) {
            continue;
        }
        const QPointF center = overlay.boxNorm.center();
        const qreal boxSideNorm =
            qBound<qreal>(0.01, qMax(overlay.boxNorm.width(), overlay.boxNorm.height()), 1.0);
        callback(overlay.clipId,
                 overlay.trackId,
                 overlay.streamId,
                 overlay.sourceFrame,
                 center.x(),
                 center.y(),
                 boxSideNorm);
        return true;
    }
    return false;
}

bool lookupVulkanInteractionInfo(const VulkanInteractionOverlayInfos& infos,
                                 const QString& clipId,
                                 VulkanInteractionOverlayInfo* outInfo);

bool updateHoveredFaceStreamBox(const PreviewInteractionState* state,
                                const VulkanInteractionOverlayInfos& infos,
                                const QPointF& surfacePosition)
{
    if (!state) {
        return false;
    }
    QString hoveredClipId;
    QString hoveredStreamId;
    int hoveredTrackId = -1;
    for (int overlayIndex = state->facestreamOverlays.size() - 1; overlayIndex >= 0; --overlayIndex) {
        const VulkanPreviewFacestreamOverlay& overlay = state->facestreamOverlays.at(overlayIndex);
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
        if (!boxPath.contains(surfacePosition)) {
            continue;
        }
        hoveredClipId = overlay.clipId;
        hoveredStreamId = overlay.streamId;
        hoveredTrackId = overlay.trackId;
        break;
    }

    PreviewInteractionTransientState& transient =
        const_cast<PreviewInteractionState*>(state)->transient;
    const bool changed =
        transient.hoveredFaceStreamTrackId != hoveredTrackId ||
        transient.hoveredFaceStreamClipId != hoveredClipId ||
        transient.hoveredFaceStreamId != hoveredStreamId;
    if (changed) {
        transient.hoveredFaceStreamTrackId = hoveredTrackId;
        transient.hoveredFaceStreamClipId = hoveredClipId;
        transient.hoveredFaceStreamId = hoveredStreamId;
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

VkClearValue facestreamOverlayColor(const PreviewInteractionState* state,
                                   const VulkanPreviewFacestreamOverlay& overlay)
{
    if (overlay.source.compare(QStringLiteral("raw_detection"), Qt::CaseInsensitive) == 0) {
        VkClearValue raw{};
        raw.color.float32[0] = 0.29f;
        raw.color.float32[1] = 0.87f;
        raw.color.float32[2] = 0.50f;
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
        overlay.trackId == state->transient.hoveredFaceStreamTrackId &&
        overlay.clipId == state->transient.hoveredFaceStreamClipId &&
        overlay.streamId == state->transient.hoveredFaceStreamId) {
        VkClearValue hovered{};
        hovered.color.float32[0] = 1.0f;
        hovered.color.float32[1] = 0.835f;
        hovered.color.float32[2] = 0.29f;
        hovered.color.float32[3] = 0.95f;
        return hovered;
    }
    const uint hueHash = qHash(overlay.streamId.isEmpty()
                                   ? QString::number(overlay.trackId)
                                   : overlay.streamId);
    QColor color = QColor::fromHsv(static_cast<int>(hueHash % 360), 210, 255);
    VkClearValue clear{};
    clear.color.float32[0] = static_cast<float>(color.redF());
    clear.color.float32[1] = static_cast<float>(color.greenF());
    clear.color.float32[2] = static_cast<float>(color.blueF());
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
        if (!event || event->button() != Qt::LeftButton || !m_state) {
            QVulkanWindow::mousePressEvent(event);
            return;
        }
        const QRectF surfaceRect = PreviewViewTransform::rectForWindow(
            this, PreviewSurfaceCoordinateSpace::DeviceSurface);
        const QPointF surfacePosition = PreviewViewTransform::pointForWindowPoint(
            this, event->position(), PreviewSurfaceCoordinateSpace::DeviceSurface);
        m_state->transient.lastMousePos = surfacePosition;
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
            dispatchFaceStreamBoxAtPosition(m_state, infos, surfacePosition, m_faceStreamBoxRequested)) {
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
                m_state->transient.hoveredFaceStreamTrackId = -1;
                m_state->transient.hoveredFaceStreamClipId.clear();
                m_state->transient.hoveredFaceStreamId.clear();
                if (!m_state->transient.speakerPickDragActive) {
                    unsetCursor();
                    requestUpdate();
                }
            }
            return QVulkanWindow::event(event);
        }

        if (event->type() == QEvent::ContextMenu) {
            const auto* contextMenu = static_cast<QContextMenuEvent*>(event);
            if (!contextMenu || !m_state) {
                return QVulkanWindow::event(event);
            }
            const QRectF surfaceRect = PreviewViewTransform::rectForWindow(
                this, PreviewSurfaceCoordinateSpace::DeviceSurface);
            const QPointF surfacePosition = PreviewViewTransform::pointForWindowPoint(
                this, contextMenu->pos(), PreviewSurfaceCoordinateSpace::DeviceSurface);
            const VulkanInteractionOverlayInfos infos = collectVulkanInteractionInfos(m_state, surfaceRect);
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

        if (m_state->faceStreamAssignmentInteractionEnabled) {
            if (updateHoveredFaceStreamBox(m_state, infos, position)) {
                requestUpdate();
                setCursor(Qt::PointingHandCursor);
            } else {
                if (m_state->transient.hoveredFaceStreamTrackId >= 0 ||
                    !m_state->transient.hoveredFaceStreamClipId.isEmpty() ||
                    !m_state->transient.hoveredFaceStreamId.isEmpty()) {
                    m_state->transient.hoveredFaceStreamTrackId = -1;
                    m_state->transient.hoveredFaceStreamClipId.clear();
                    m_state->transient.hoveredFaceStreamId.clear();
                    requestUpdate();
                }
                setCursor(Qt::ArrowCursor);
            }
            return;
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
        m_updatePending = false;
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
    std::function<void(const QString&)> m_createKeyframeRequested;
    std::function<void(const QString&, qreal, qreal, bool)> m_resizeRequested;
    std::function<void(const QString&, qreal, qreal, bool)> m_moveRequested;
    QImage m_latestVulkanReadbackImage;
    QImage m_latestDecoderDiagnosticImage;
    bool m_pipelineThumbnailReadbackPending = false;
    qint64 m_lastPipelineThumbnailReadbackMs = 0;
    bool m_updatePending = false;
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
    m_overlayResources = std::make_unique<VulkanResources>();
    if (!m_overlayResources->initialize(m_window->physicalDevice(), m_window->device(), m_devFuncs)) {
        if (m_owner) {
            m_owner->markFailure(QStringLiteral("Failed to initialize transcript overlay Vulkan resources."));
        }
        return;
    }
    m_frameHandoff = std::make_unique<jcut::vulkan_detector::VulkanDetectorFrameHandoff>();
    const jcut::vulkan_detector::VulkanDeviceContext handoffContext{
        m_window->physicalDevice(),
        m_window->device(),
        m_window->graphicsQueue(),
        m_window->graphicsQueueFamilyIndex()
    };
    QString handoffError;
    if (!m_frameHandoff->initialize(handoffContext, &handoffError)) {
        m_lastHandoffError = handoffError;
        qWarning().noquote()
            << QStringLiteral("[vulkan-preview] hardware frame handoff unavailable: %1").arg(handoffError);
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
    if (m_frameHandoff) {
        m_frameHandoff->release();
        m_frameHandoff.reset();
    }
    m_audioTab.reset();
    m_pipeline.reset();
    m_overlayResources.reset();
    m_resources.reset();
    m_devFuncs = nullptr;
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

    const PreviewInteractionState* state = m_owner->state();
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
    struct DecoderReadbackCandidate {
        VkImage image = VK_NULL_HANDLE;
        VkImageLayout layout = VK_IMAGE_LAYOUT_UNDEFINED;
        QSize size;
        VkFormat format = VK_FORMAT_UNDEFINED;
    } decoderReadbackCandidate;
    m_devFuncs->vkCmdBeginRenderPass(cb, &rp, VK_SUBPASS_CONTENTS_INLINE);
    bool audioWaitingForWaveform = false;
    if (renderDirectVulkanAudioFrame(
            DirectVulkanAudioRenderContext{state, m_devFuncs, m_audioTab.get(), cb, swapSize},
            &audioWaitingForWaveform)) {
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
            const bool canDrawTexture = m_resources && m_pipeline && m_resources->isReady() &&
                                        m_pipeline->isReady() && m_resources->descriptorSet() != VK_NULL_HANDLE;
            const bool forceChecker = qEnvironmentVariableIntValue("JCUT_VULKAN_PREVIEW_FORCE_CHECKER") == 1;
            bool sampledFrameReady = false;
            bool handoffAttempted = false;
            if (!forceChecker && canDrawTexture && status && status->hasFrame && m_frameHandoff && m_frameHandoff->isInitialized()) {
                QString handoffError;
                double uploadMs = 0.0;
                const bool allowCpuUploadFallback = status->cpuImage;
                handoffAttempted = true;
                if (DirectVulkanPreviewStats* stats = m_owner->stats()) {
                    ++stats->handoffAttempts;
                }
                const bool handoffOk = [&]() -> bool {
                    if (status->externalVulkanFrame) {
                        render_detail::OffscreenVulkanFrame offscreenFrame;
                        offscreenFrame.physicalDevice = status->externalPhysicalDevice;
                        offscreenFrame.device = status->externalDevice;
                        offscreenFrame.queue = status->externalQueue;
                        offscreenFrame.queueFamilyIndex = status->externalQueueFamilyIndex;
                        offscreenFrame.image = status->externalImage;
                        offscreenFrame.imageView = status->externalImageView;
                        offscreenFrame.imageMemory = status->externalImageMemory;
                        offscreenFrame.imageLayout = status->externalImageLayout;
                        offscreenFrame.imageFormat = status->externalImageFormat;
                        offscreenFrame.readySemaphoreFd = status->externalReadySemaphoreFd;
                        offscreenFrame.size = status->frameSize;
                        offscreenFrame.valid = status->hasFrame;
                        return m_frameHandoff->importOffscreenFrame(offscreenFrame, &handoffError);
                    }
                    return m_frameHandoff->uploadFrame(status->frame,
                                                       allowCpuUploadFallback,
                                                       &uploadMs,
                                                       &handoffError);
                }();
                if (handoffOk) {
                    const jcut::vulkan_detector::VulkanExternalImage external = m_frameHandoff->externalImage();
                    decoderReadbackCandidate.image = m_frameHandoff->image();
                    decoderReadbackCandidate.layout = m_frameHandoff->imageLayout();
                    decoderReadbackCandidate.size = external.size;
                    decoderReadbackCandidate.format = m_frameHandoff->imageFormat();
                    sampledFrameReady = m_resources->setSampledImage(external.imageView, external.imageLayout);
                    if (DirectVulkanPreviewStats* stats = m_owner->stats()) {
                        ++stats->handoffSuccesses;
                        stats->lastUploadMs = uploadMs;
                        stats->lastExternalImageSize = external.size;
                        stats->lastHandoffError.clear();
                        stats->lastHandoffMode =
                            m_frameHandoff->lastMode() == jcut::vulkan_detector::FrameHandoffMode::HardwareDirect
                                ? QStringLiteral("hardware_direct")
                                : (m_frameHandoff->lastMode() == jcut::vulkan_detector::FrameHandoffMode::ExternalMemoryImport
                                       ? QStringLiteral("external_memory_import")
                                       : (m_frameHandoff->lastMode() == jcut::vulkan_detector::FrameHandoffMode::CpuUpload
                                              ? QStringLiteral("cpu_upload")
                                              : QStringLiteral("invalid")));
                        const auto& probe = m_frameHandoff->lastProbe();
                        stats->lastProbePath = probe.path;
                        stats->lastProbeReason = probe.reason;
                        stats->lastHardwareSwFormat = pixelFormatName(status->frame.hardwareSwPixelFormat());
                        stats->lastVulkanImageFormat = vulkanFormatName(m_frameHandoff->imageFormat());
                        if (sampledFrameReady) {
                            ++stats->sampledImageReady;
                        }
                    }
                } else if (handoffError != m_lastHandoffError) {
                    m_lastHandoffError = handoffError;
                    if (DirectVulkanPreviewStats* stats = m_owner->stats()) {
                        ++stats->handoffFailures;
                        stats->lastHandoffError = handoffError;
                        stats->lastHandoffMode = QStringLiteral("invalid");
                        const auto& probe = m_frameHandoff->lastProbe();
                        stats->lastProbePath = probe.path;
                        stats->lastProbeReason = probe.reason;
                    }
                    qWarning().noquote()
                        << QStringLiteral("[vulkan-preview] hardware frame handoff failed: %1").arg(handoffError);
                } else if (DirectVulkanPreviewStats* stats = m_owner->stats()) {
                    ++stats->handoffFailures;
                    stats->lastHandoffError = handoffError;
                }
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
                    const QByteArray curveLut = curveLutRgbaBytes(status->grading);
                    if (!curveLut.isEmpty() && m_resources->uploadCurveLut(cb, curveLut)) {
                        if (DirectVulkanPreviewStats* stats = m_owner->stats()) {
                            stats->lastCurveLutApplied = status->curveLutApplied;
                        }
                    } else if (DirectVulkanPreviewStats* stats = m_owner->stats()) {
                        stats->lastUnsupportedEffect = QStringLiteral("curve_lut_upload_failed");
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
                scissor.offset = {0, 0};
                scissor.extent = {static_cast<uint32_t>(std::max(1, swapSize.width())),
                                  static_cast<uint32_t>(std::max(1, swapSize.height()))};
                m_pipeline->bindAndDraw(cb, viewport, scissor, m_resources->descriptorSet(), push);
            } else {
                if (DirectVulkanPreviewStats* stats = m_owner->stats()) {
                    ++stats->clearFallbackDraws;
                    ++stats->explicitFailureDraws;
                    ++stats->activeClipDraws;
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
                    } else if (!canDrawTexture) {
                        stats->lastHandoffMode = QStringLiteral("texture_pipeline_unavailable");
                        stats->lastHandoffError = QStringLiteral("Vulkan texture pipeline or descriptor set is unavailable.");
                    }
                }
                m_devFuncs->vkCmdClearAttachments(cb, 1, &attachment, 1, &rect);
            }
            const TimelineClip effectiveClip = clipWithTransientTranscriptOverride(state, clip);
            if (m_overlayResources && m_pipeline && m_overlayResources->isReady() &&
                m_pipeline->isReady() && m_overlayResources->descriptorSet() != VK_NULL_HANDLE &&
                clipSupportsTranscriptOverlay(effectiveClip)) {
                const QString transcriptPath = activeTranscriptPathForClipFile(effectiveClip.filePath);
                if (!transcriptPath.isEmpty()) {
                    const std::shared_ptr<const TranscriptRuntimeDocument> runtimeDocument =
                        loadTranscriptRuntimeDocument(transcriptPath);
                    const QVector<TranscriptSection>& sections =
                        runtimeDocument ? runtimeDocument->sections : QVector<TranscriptSection>{};
                    const int64_t sourceFrame =
                        transcriptFrameForClipAtTimelineSample(effectiveClip,
                                                               state->currentSample,
                                                               state->renderSyncMarkers);
                    const TranscriptOverlayLayout overlayLayout =
                        transcriptOverlayLayoutAtSourceFrame(effectiveClip, sections, sourceFrame);
                    if (!overlayLayout.lines.isEmpty()) {
                        const QRectF outputRect = transcriptOverlayRectInOutputSpace(
                            effectiveClip,
                            state->outputSize,
                            transcriptPath,
                            sections,
                            sourceFrame);
                        const QRectF bounds = transcriptOverlayBoundsForClip(state, clip, viewTransform);
                        const QRectF localBounds(0.0, 0.0, outputRect.width(), outputRect.height());
                        const QRectF localTextBounds = localBounds.adjusted(18.0, 14.0, -18.0, -14.0);
                        const qreal fontPixelSize = effectiveClip.transcriptOverlay.fontPointSize;
                        if (outputRect.width() > 0.0 &&
                            outputRect.height() > 0.0 &&
                            bounds.width() > 0.0 &&
                            bounds.height() > 0.0 &&
                            localTextBounds.width() > 0.0 &&
                            localTextBounds.height() > 0.0 &&
                            fontPixelSize > 0.0) {
                            const QColor highlightFillColor(QStringLiteral("#fff2a8"));
                            const QColor highlightTextColor(QStringLiteral("#181818"));
                            QString titleShadowHtml;
                            QString titleTextHtml;
                            if (effectiveClip.transcriptOverlay.showSpeakerTitle) {
                                const QString titleText =
                                    transcriptSpeakerTitleForSourceFrame(transcriptPath, sections, sourceFrame);
                                if (effectiveClip.transcriptOverlay.showShadow) {
                                    titleShadowHtml = transcriptSpeakerTitleHtml(titleText, QColor(0, 0, 0, 200));
                                }
                                titleTextHtml = transcriptSpeakerTitleHtml(titleText, effectiveClip.transcriptOverlay.textColor);
                            }
                            const QString shadowHtml = effectiveClip.transcriptOverlay.showShadow
                                ? (titleShadowHtml + transcriptOverlayHtml(
                                    overlayLayout,
                                    QColor(0, 0, 0, 200),
                                    QColor(0, 0, 0, 200),
                                    QColor(0, 0, 0, 0)))
                                : QString();
                            const QString textHtml = titleTextHtml + transcriptOverlayHtml(
                                overlayLayout,
                                effectiveClip.transcriptOverlay.textColor,
                                highlightTextColor,
                                highlightFillColor);
                            if (!textHtml.isEmpty()) {
                                const QString overlayKey = transcriptOverlayTextureKey(
                                    effectiveClip,
                                    bounds,
                                    localTextBounds,
                                    fontPixelSize,
                                    shadowHtml,
                                    textHtml);
                                Q_UNUSED(overlayKey);
                                const render_detail::OverlayImage overlayImage = renderTranscriptOverlay(
                                    effectiveClip,
                                    localBounds,
                                    localTextBounds,
                                    fontPixelSize,
                                    shadowHtml,
                                    textHtml);
                                if (m_overlayResources->uploadImageTexture(cb, overlayImage)) {
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
                                                            m_overlayResources->descriptorSet(),
                                                            overlayPush);
                                }
                            }
                        }
                    }
                }
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
        const int thickness = std::max(2, std::min(swapSize.width(), swapSize.height()) / 180);
        for (const VulkanPreviewFacestreamOverlay& overlay : state->facestreamOverlays) {
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
            clearBoxOutline(m_devFuncs, cb, facestreamOverlayColor(state, overlay), boxRect, thickness);
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
            clearBoxOutline(m_devFuncs, cb, facestreamOverlayColor(state, overlay), boxRect, qMax(1, thickness - 1));
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
