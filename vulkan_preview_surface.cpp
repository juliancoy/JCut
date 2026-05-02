#include "vulkan_preview_surface.h"

#include "opengl_preview.h"
#include "vulkan_preview_window.h"

#include <QDebug>
#include <QWidget>

namespace {
QString normalizedOverlaySource(const QString& source)
{
    const QString normalized = source.trimmed().toLower();
    return normalized.isEmpty() ? QStringLiteral("all") : normalized;
}
} // namespace

VulkanPreviewSurface::VulkanPreviewSurface(QWidget* parent)
{
    const QByteArray nativeSurfaceEnv = qgetenv("JCUT_VULKAN_NATIVE_SURFACE");
    const bool nativeRequested = nativeSurfaceEnv.isEmpty()
        ? true
        : (qEnvironmentVariableIntValue("JCUT_VULKAN_NATIVE_SURFACE") == 1);
    if (nativeRequested) {
        m_nativeActive = initializeNativeSurface(parent);
    }

    const QByteArray parityBridgeEnv = qgetenv("JCUT_VULKAN_PARITY_BRIDGE");
    m_nativeParityBridgeActive = m_nativeActive &&
        !parityBridgeEnv.isEmpty() &&
        qEnvironmentVariableIntValue("JCUT_VULKAN_PARITY_BRIDGE") != 0;

    if (!m_nativeActive || m_nativeParityBridgeActive) {
        qputenv("JCUT_PREVIEW_SUPPRESS_BRIDGE_LOG", "1");
        auto delegate = std::make_unique<VulkanPreviewWindow>(parent);
        delegate->setVisible(false);
        qunsetenv("JCUT_PREVIEW_SUPPRESS_BRIDGE_LOG");
        m_delegate = std::move(delegate);
    }
}

VulkanPreviewSurface::~VulkanPreviewSurface()
{
    if (m_nativeContainer) {
        m_nativeContainer->deleteLater();
        m_nativeContainer = nullptr;
    }
    if (m_vulkanWindow) {
        m_vulkanWindow->destroy();
        delete m_vulkanWindow;
        m_vulkanWindow = nullptr;
    }
}

bool VulkanPreviewSurface::initializeNativeSurface(QWidget* parent)
{
    if (!m_vulkanContext.initialize()) {
        m_nativeFailureReason = m_vulkanContext.failureReason();
        return false;
    }

    auto* window = createVulkanNativeWindow(&m_nativeState, m_vulkanContext.instance());
    QWidget* container = QWidget::createWindowContainer(window, parent);
    if (!container) {
        delete window;
        m_nativeFailureReason = QStringLiteral("Failed to create Vulkan window container.");
        return false;
    }

    container->setContentsMargins(0, 0, 0, 0);
    container->setMinimumSize(160, 120);
    m_vulkanWindow = window;
    m_nativeContainer = container;

    qDebug().noquote()
        << "[vulkan] native preview surface initialized (QVulkanWindow)";
    return true;
}

PreviewWindow* VulkanPreviewSurface::activeDelegate() const
{
    return m_delegate.get();
}

bool VulkanPreviewSurface::usingParityBridge() const
{
    return m_nativeParityBridgeActive && activeDelegate();
}

void VulkanPreviewSurface::requestNativeUpdate()
{
    if (m_nativeActive && m_vulkanWindow) {
        m_vulkanWindow->requestUpdate();
    }
}

QWidget* VulkanPreviewSurface::asWidget()
{
    if (usingParityBridge()) {
        return m_delegate.get();
    }
    return m_nativeActive ? m_nativeContainer : m_delegate.get();
}

const QWidget* VulkanPreviewSurface::asWidget() const
{
    if (usingParityBridge()) {
        return m_delegate.get();
    }
    return m_nativeActive ? m_nativeContainer : m_delegate.get();
}

void VulkanPreviewSurface::setPlaybackState(bool playing)
{
    if (m_nativeActive && !usingParityBridge()) {
        m_nativeState.playing = playing;
        requestNativeUpdate();
        return;
    }
    if (activeDelegate()) activeDelegate()->setPlaybackState(playing);
}

void VulkanPreviewSurface::setCurrentFrame(int64_t frame)
{
    if (m_nativeActive && !usingParityBridge()) {
        m_nativeState.currentFrame = qMax<int64_t>(0, frame);
        requestNativeUpdate();
        return;
    }
    if (activeDelegate()) activeDelegate()->setCurrentFrame(frame);
}

void VulkanPreviewSurface::setCurrentPlaybackSample(int64_t samplePosition)
{
    if (m_nativeActive && !usingParityBridge()) {
        m_nativeState.currentPlaybackSample = qMax<int64_t>(0, samplePosition);
        return;
    }
    if (activeDelegate()) activeDelegate()->setCurrentPlaybackSample(samplePosition);
}

void VulkanPreviewSurface::setClipCount(int count)
{
    if (m_nativeActive && !usingParityBridge()) {
        m_nativeState.clipCount = qMax(0, count);
        return;
    }
    if (activeDelegate()) activeDelegate()->setClipCount(count);
}

void VulkanPreviewSurface::setSelectedClipId(const QString& clipId)
{
    if (m_nativeActive && !usingParityBridge()) {
        m_nativeState.selectedClipId = clipId;
        return;
    }
    if (activeDelegate()) activeDelegate()->setSelectedClipId(clipId);
}

void VulkanPreviewSurface::setTimelineClips(const QVector<TimelineClip>& clips)
{
    if (m_nativeActive && !usingParityBridge()) {
        m_nativeState.clipCount = qMax(0, clips.size());
        requestNativeUpdate();
        return;
    }
    if (activeDelegate()) activeDelegate()->setTimelineClips(clips);
}

void VulkanPreviewSurface::setTimelineTracks(const QVector<TimelineTrack>& tracks)
{
    if (m_nativeActive && !usingParityBridge()) {
        Q_UNUSED(tracks);
        return;
    }
    if (activeDelegate()) activeDelegate()->setTimelineTracks(tracks);
}

void VulkanPreviewSurface::setRenderSyncMarkers(const QVector<RenderSyncMarker>& markers)
{
    if (m_nativeActive && !usingParityBridge()) {
        Q_UNUSED(markers);
        return;
    }
    if (activeDelegate()) activeDelegate()->setRenderSyncMarkers(markers);
}

void VulkanPreviewSurface::setExportRanges(const QVector<ExportRangeSegment>& ranges)
{
    if (m_nativeActive && !usingParityBridge()) {
        Q_UNUSED(ranges);
        return;
    }
    if (activeDelegate()) activeDelegate()->setExportRanges(ranges);
}

void VulkanPreviewSurface::invalidateTranscriptOverlayCache(const QString& clipFilePath)
{
    if (m_nativeActive && !usingParityBridge()) {
        Q_UNUSED(clipFilePath);
        return;
    }
    if (activeDelegate()) activeDelegate()->invalidateTranscriptOverlayCache(clipFilePath);
}

void VulkanPreviewSurface::beginBulkUpdate() { if (activeDelegate()) activeDelegate()->beginBulkUpdate(); }
void VulkanPreviewSurface::endBulkUpdate() { if (activeDelegate()) activeDelegate()->endBulkUpdate(); }
QString VulkanPreviewSurface::backendName() const {
    if (usingParityBridge()) {
        return QStringLiteral("Vulkan Native (Parity Bridge)");
    }
    return m_nativeActive ? QStringLiteral("Vulkan Native Surface (Scaffold)") : m_delegate->backendName();
}

void VulkanPreviewSurface::setRenderBackendPreference(const QString& backendName)
{
    if (m_nativeActive && !usingParityBridge()) {
        m_nativeState.backendPreference = backendName.trimmed().toLower();
        return;
    }
    if (activeDelegate()) activeDelegate()->setRenderBackendPreference(backendName);
}

void VulkanPreviewSurface::setAudioMuted(bool muted)
{
    if (m_nativeActive && !usingParityBridge()) {
        m_nativeState.audioMuted = muted;
        return;
    }
    if (activeDelegate()) activeDelegate()->setAudioMuted(muted);
}

void VulkanPreviewSurface::setAudioVolume(qreal volume)
{
    if (m_nativeActive && !usingParityBridge()) {
        m_nativeState.audioVolume = qBound<qreal>(0.0, volume, 1.0);
        return;
    }
    if (activeDelegate()) activeDelegate()->setAudioVolume(volume);
}

void VulkanPreviewSurface::setOutputSize(const QSize& size)
{
    if (m_nativeActive && !usingParityBridge()) {
        m_nativeState.outputSize = QSize(qMax(16, size.width()), qMax(16, size.height()));
        requestNativeUpdate();
        return;
    }
    if (activeDelegate()) activeDelegate()->setOutputSize(size);
}

void VulkanPreviewSurface::setHideOutsideOutputWindow(bool hide)
{
    if (m_nativeActive && !usingParityBridge()) {
        m_nativeState.hideOutsideOutputWindow = hide;
        requestNativeUpdate();
        return;
    }
    if (activeDelegate()) activeDelegate()->setHideOutsideOutputWindow(hide);
}

void VulkanPreviewSurface::setBypassGrading(bool bypass)
{
    if (m_nativeActive && !usingParityBridge()) {
        m_nativeState.bypassGrading = bypass;
        return;
    }
    if (activeDelegate()) activeDelegate()->setBypassGrading(bypass);
}

void VulkanPreviewSurface::setCorrectionsEnabled(bool enabled)
{
    if (m_nativeActive && !usingParityBridge()) {
        m_nativeState.correctionsEnabled = enabled;
        return;
    }
    if (activeDelegate()) activeDelegate()->setCorrectionsEnabled(enabled);
}

void VulkanPreviewSurface::setShowCorrectionOverlays(bool show)
{
    if (m_nativeActive && !usingParityBridge()) {
        m_nativeState.showCorrectionOverlays = show;
        requestNativeUpdate();
        return;
    }
    if (activeDelegate()) activeDelegate()->setShowCorrectionOverlays(show);
}

void VulkanPreviewSurface::setSelectedCorrectionPolygon(int polygonIndex)
{
    if (m_nativeActive && !usingParityBridge()) {
        m_nativeState.selectedCorrectionPolygon = polygonIndex;
        requestNativeUpdate();
        return;
    }
    if (activeDelegate()) activeDelegate()->setSelectedCorrectionPolygon(polygonIndex);
}

void VulkanPreviewSurface::setBackgroundColor(const QColor& color)
{
    if (m_nativeActive && !usingParityBridge()) {
        m_nativeState.backgroundColor = color;
        requestNativeUpdate();
        return;
    }
    if (activeDelegate()) activeDelegate()->setBackgroundColor(color);
}

void VulkanPreviewSurface::setPreviewZoom(qreal zoom)
{
    if (m_nativeActive && !usingParityBridge()) {
        m_nativeState.previewZoom = qBound<qreal>(0.1, zoom, 100000.0);
        return;
    }
    if (activeDelegate()) activeDelegate()->setPreviewZoom(zoom);
}

void VulkanPreviewSurface::setShowSpeakerTrackPoints(bool show)
{
    if (m_nativeActive && !usingParityBridge()) {
        m_nativeState.showSpeakerTrackPoints = show;
        requestNativeUpdate();
        return;
    }
    if (activeDelegate()) activeDelegate()->setShowSpeakerTrackPoints(show);
}

void VulkanPreviewSurface::setShowSpeakerTrackBoxes(bool show)
{
    if (m_nativeActive && !usingParityBridge()) {
        m_nativeState.showSpeakerTrackBoxes = show;
        requestNativeUpdate();
        return;
    }
    if (activeDelegate()) activeDelegate()->setShowSpeakerTrackBoxes(show);
}

void VulkanPreviewSurface::setBoxstreamOverlaySource(const QString& source)
{
    if (m_nativeActive && !usingParityBridge()) {
        m_nativeState.boxstreamOverlaySource = normalizedOverlaySource(source);
        requestNativeUpdate();
        return;
    }
    if (activeDelegate()) activeDelegate()->setBoxstreamOverlaySource(source);
}

void VulkanPreviewSurface::setAudioSpeakerHoverModalEnabled(bool enabled)
{
    if (m_nativeActive && !usingParityBridge()) {
        m_nativeState.audioSpeakerHoverModalEnabled = enabled;
        return;
    }
    if (activeDelegate()) activeDelegate()->setAudioSpeakerHoverModalEnabled(enabled);
}

void VulkanPreviewSurface::setAudioWaveformVisible(bool visible)
{
    if (m_nativeActive && !usingParityBridge()) {
        m_nativeState.audioWaveformVisible = visible;
        requestNativeUpdate();
        return;
    }
    if (activeDelegate()) activeDelegate()->setAudioWaveformVisible(visible);
}

bool VulkanPreviewSurface::audioSpeakerHoverModalEnabled() const
{
    return (m_nativeActive && !usingParityBridge()) ? m_nativeState.audioSpeakerHoverModalEnabled
                                                     : (activeDelegate() ? activeDelegate()->audioSpeakerHoverModalEnabled() : true);
}

bool VulkanPreviewSurface::audioWaveformVisible() const
{
    return (m_nativeActive && !usingParityBridge()) ? m_nativeState.audioWaveformVisible
                                                     : (activeDelegate() ? activeDelegate()->audioWaveformVisible() : true);
}

void VulkanPreviewSurface::setViewMode(ViewMode mode)
{
    if (m_nativeActive && !usingParityBridge()) {
        m_nativeState.viewMode = mode;
        requestNativeUpdate();
        return;
    }
    if (activeDelegate()) activeDelegate()->setViewMode(mode);
}

PreviewSurface::ViewMode VulkanPreviewSurface::viewMode() const
{
    return (m_nativeActive && !usingParityBridge()) ? m_nativeState.viewMode
                                                     : (activeDelegate() ? activeDelegate()->viewMode() : PreviewSurface::ViewMode::Video);
}

void VulkanPreviewSurface::setAudioDynamicsSettings(const AudioDynamicsSettings& settings)
{
    if (m_nativeActive && !usingParityBridge()) {
        m_nativeState.audioDynamics = settings;
        return;
    }
    if (activeDelegate()) activeDelegate()->setAudioDynamicsSettings(settings);
}

PreviewSurface::AudioDynamicsSettings VulkanPreviewSurface::audioDynamicsSettings() const
{
    return (m_nativeActive && !usingParityBridge()) ? m_nativeState.audioDynamics
                                                     : (activeDelegate() ? activeDelegate()->audioDynamicsSettings() : AudioDynamicsSettings{});
}

void VulkanPreviewSurface::setTranscriptOverlayInteractionEnabled(bool enabled)
{
    if (m_nativeActive && !usingParityBridge()) {
        m_nativeState.transcriptOverlayInteractionEnabled = enabled;
        return;
    }
    if (activeDelegate()) activeDelegate()->setTranscriptOverlayInteractionEnabled(enabled);
}

void VulkanPreviewSurface::setTitleOverlayInteractionOnly(bool enabled)
{
    if (m_nativeActive && !usingParityBridge()) {
        m_nativeState.titleOverlayInteractionOnly = enabled;
        return;
    }
    if (activeDelegate()) activeDelegate()->setTitleOverlayInteractionOnly(enabled);
}

void VulkanPreviewSurface::setCorrectionDrawMode(bool enabled)
{
    if (m_nativeActive && !usingParityBridge()) {
        m_nativeState.correctionDrawMode = enabled;
        return;
    }
    if (activeDelegate()) activeDelegate()->setCorrectionDrawMode(enabled);
}

bool VulkanPreviewSurface::correctionDrawMode() const
{
    return (m_nativeActive && !usingParityBridge()) ? m_nativeState.correctionDrawMode
                                                     : (activeDelegate() ? activeDelegate()->correctionDrawMode() : false);
}

bool VulkanPreviewSurface::transcriptOverlayInteractionEnabled() const
{
    return (m_nativeActive && !usingParityBridge()) ? m_nativeState.transcriptOverlayInteractionEnabled
                                                     : (activeDelegate() ? activeDelegate()->transcriptOverlayInteractionEnabled() : false);
}

bool VulkanPreviewSurface::titleOverlayInteractionOnly() const
{
    return (m_nativeActive && !usingParityBridge()) ? m_nativeState.titleOverlayInteractionOnly
                                                     : (activeDelegate() ? activeDelegate()->titleOverlayInteractionOnly() : false);
}

void VulkanPreviewSurface::setCorrectionDraftPoints(const QVector<QPointF>& points)
{
    if (m_nativeActive && !usingParityBridge()) {
        Q_UNUSED(points);
        return;
    }
    if (activeDelegate()) activeDelegate()->setCorrectionDraftPoints(points);
}

qreal VulkanPreviewSurface::previewZoom() const
{
    return (m_nativeActive && !usingParityBridge()) ? m_nativeState.previewZoom
                                                     : (activeDelegate() ? activeDelegate()->previewZoom() : 1.0);
}

void VulkanPreviewSurface::resetPreviewPan()
{
    if (activeDelegate()) {
        activeDelegate()->resetPreviewPan();
    }
}

QSize VulkanPreviewSurface::outputSize() const
{
    return (m_nativeActive && !usingParityBridge()) ? m_nativeState.outputSize
                                                     : (activeDelegate() ? activeDelegate()->outputSize() : QSize(1080, 1920));
}

bool VulkanPreviewSurface::bypassGrading() const
{
    return (m_nativeActive && !usingParityBridge()) ? m_nativeState.bypassGrading
                                                     : (activeDelegate() ? activeDelegate()->bypassGrading() : false);
}

bool VulkanPreviewSurface::correctionsEnabled() const
{
    return (m_nativeActive && !usingParityBridge()) ? m_nativeState.correctionsEnabled
                                                     : (activeDelegate() ? activeDelegate()->correctionsEnabled() : false);
}

bool VulkanPreviewSurface::audioMuted() const
{
    return (m_nativeActive && !usingParityBridge()) ? m_nativeState.audioMuted
                                                     : (activeDelegate() ? activeDelegate()->audioMuted() : false);
}

int VulkanPreviewSurface::audioVolumePercent() const
{
    return (m_nativeActive && !usingParityBridge()) ? qRound(m_nativeState.audioVolume * 100.0)
                                                     : (activeDelegate() ? activeDelegate()->audioVolumePercent() : 100);
}

QString VulkanPreviewSurface::activeAudioClipLabel() const
{
    return activeDelegate() ? activeDelegate()->activeAudioClipLabel() : QString();
}

bool VulkanPreviewSurface::preparePlaybackAdvance(int64_t targetFrame)
{
    if (m_nativeActive && !usingParityBridge()) {
        m_nativeState.currentFrame = qMax<int64_t>(0, targetFrame);
        return true;
    }
    return activeDelegate() ? activeDelegate()->preparePlaybackAdvance(targetFrame) : true;
}

bool VulkanPreviewSurface::preparePlaybackAdvanceSample(int64_t targetSample)
{
    if (m_nativeActive && !usingParityBridge()) {
        m_nativeState.currentPlaybackSample = qMax<int64_t>(0, targetSample);
        return true;
    }
    return activeDelegate() ? activeDelegate()->preparePlaybackAdvanceSample(targetSample) : true;
}

bool VulkanPreviewSurface::warmPlaybackLookahead(int futureFrames, int timeoutMs)
{
    if (m_nativeActive && !usingParityBridge()) {
        Q_UNUSED(futureFrames);
        Q_UNUSED(timeoutMs);
        return true;
    }
    return activeDelegate() ? activeDelegate()->warmPlaybackLookahead(futureFrames, timeoutMs) : true;
}

QImage VulkanPreviewSurface::latestPresentedFrameImageForClip(const QString& clipId) const
{
    if (m_nativeActive && !usingParityBridge()) {
        Q_UNUSED(clipId);
        return m_nativeState.latestFrame;
    }
    return activeDelegate() ? activeDelegate()->latestPresentedFrameImageForClip(clipId) : QImage();
}

QJsonObject VulkanPreviewSurface::profilingSnapshot() const
{
    return (m_nativeActive && !usingParityBridge()) ? m_nativeState.profiling
                                                     : (activeDelegate() ? activeDelegate()->profilingSnapshot() : QJsonObject{});
}

void VulkanPreviewSurface::resetProfilingStats()
{
    if (m_nativeActive && !usingParityBridge()) {
        m_nativeState.profiling = QJsonObject{};
        return;
    }
    if (activeDelegate()) activeDelegate()->resetProfilingStats();
}

bool VulkanPreviewSurface::selectedOverlayIsTranscript() const
{
    return activeDelegate() ? activeDelegate()->selectedOverlayIsTranscript() : false;
}
