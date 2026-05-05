#include "vulkan_preview_surface.h"

#include "opengl_preview.h"
#include "vulkan_preview_window.h"

namespace {
template <typename Func>
void withDelegate(PreviewWindow* delegate, Func&& func)
{
    if (delegate) {
        func(delegate);
    }
}
} // namespace

VulkanPreviewSurface::VulkanPreviewSurface(QWidget* parent)
{
    qputenv("JCUT_PREVIEW_SUPPRESS_BRIDGE_LOG", "1");
    auto delegate = std::make_unique<VulkanPreviewWindow>(parent);
    delegate->setVisible(false);
    qunsetenv("JCUT_PREVIEW_SUPPRESS_BRIDGE_LOG");
    m_delegate = std::move(delegate);
    if (!m_delegate) {
        m_failureReason = QStringLiteral("Failed to create Vulkan preview delegate.");
    }
}

VulkanPreviewSurface::~VulkanPreviewSurface() = default;

PreviewWindow* VulkanPreviewSurface::activeDelegate() const
{
    return m_delegate.get();
}

QWidget* VulkanPreviewSurface::asWidget()
{
    return m_delegate.get();
}

const QWidget* VulkanPreviewSurface::asWidget() const
{
    return m_delegate.get();
}

void VulkanPreviewSurface::setPlaybackState(bool playing)
{
    withDelegate(activeDelegate(), [playing](PreviewWindow* w) { w->setPlaybackState(playing); });
}

void VulkanPreviewSurface::setCurrentFrame(int64_t frame)
{
    withDelegate(activeDelegate(), [frame](PreviewWindow* w) { w->setCurrentFrame(frame); });
}

void VulkanPreviewSurface::setCurrentPlaybackSample(int64_t samplePosition)
{
    withDelegate(activeDelegate(), [samplePosition](PreviewWindow* w) { w->setCurrentPlaybackSample(samplePosition); });
}

void VulkanPreviewSurface::setClipCount(int count)
{
    withDelegate(activeDelegate(), [count](PreviewWindow* w) { w->setClipCount(count); });
}

void VulkanPreviewSurface::setSelectedClipId(const QString& clipId)
{
    withDelegate(activeDelegate(), [&clipId](PreviewWindow* w) { w->setSelectedClipId(clipId); });
}

void VulkanPreviewSurface::setTimelineClips(const QVector<TimelineClip>& clips)
{
    withDelegate(activeDelegate(), [&clips](PreviewWindow* w) { w->setTimelineClips(clips); });
}

void VulkanPreviewSurface::setTimelineTracks(const QVector<TimelineTrack>& tracks)
{
    withDelegate(activeDelegate(), [&tracks](PreviewWindow* w) { w->setTimelineTracks(tracks); });
}

void VulkanPreviewSurface::setRenderSyncMarkers(const QVector<RenderSyncMarker>& markers)
{
    withDelegate(activeDelegate(), [&markers](PreviewWindow* w) { w->setRenderSyncMarkers(markers); });
}

void VulkanPreviewSurface::setExportRanges(const QVector<ExportRangeSegment>& ranges)
{
    withDelegate(activeDelegate(), [&ranges](PreviewWindow* w) { w->setExportRanges(ranges); });
}

void VulkanPreviewSurface::invalidateTranscriptOverlayCache(const QString& clipFilePath)
{
    withDelegate(activeDelegate(), [&clipFilePath](PreviewWindow* w) { w->invalidateTranscriptOverlayCache(clipFilePath); });
}

void VulkanPreviewSurface::beginBulkUpdate()
{
    withDelegate(activeDelegate(), [](PreviewWindow* w) { w->beginBulkUpdate(); });
}

void VulkanPreviewSurface::endBulkUpdate()
{
    withDelegate(activeDelegate(), [](PreviewWindow* w) { w->endBulkUpdate(); });
}

QString VulkanPreviewSurface::backendName() const
{
    return activeDelegate() ? activeDelegate()->backendName() : QStringLiteral("Vulkan Preview Unavailable");
}

void VulkanPreviewSurface::setRenderBackendPreference(const QString& backendName)
{
    withDelegate(activeDelegate(), [&backendName](PreviewWindow* w) { w->setRenderBackendPreference(backendName); });
}

void VulkanPreviewSurface::setAudioMuted(bool muted)
{
    withDelegate(activeDelegate(), [muted](PreviewWindow* w) { w->setAudioMuted(muted); });
}

void VulkanPreviewSurface::setAudioVolume(qreal volume)
{
    withDelegate(activeDelegate(), [volume](PreviewWindow* w) { w->setAudioVolume(volume); });
}

void VulkanPreviewSurface::setOutputSize(const QSize& size)
{
    withDelegate(activeDelegate(), [&size](PreviewWindow* w) { w->setOutputSize(size); });
}

void VulkanPreviewSurface::setHideOutsideOutputWindow(bool hide)
{
    withDelegate(activeDelegate(), [hide](PreviewWindow* w) { w->setHideOutsideOutputWindow(hide); });
}

void VulkanPreviewSurface::setBypassGrading(bool bypass)
{
    withDelegate(activeDelegate(), [bypass](PreviewWindow* w) { w->setBypassGrading(bypass); });
}

void VulkanPreviewSurface::setCorrectionsEnabled(bool enabled)
{
    withDelegate(activeDelegate(), [enabled](PreviewWindow* w) { w->setCorrectionsEnabled(enabled); });
}

void VulkanPreviewSurface::setShowCorrectionOverlays(bool show)
{
    withDelegate(activeDelegate(), [show](PreviewWindow* w) { w->setShowCorrectionOverlays(show); });
}

void VulkanPreviewSurface::setSelectedCorrectionPolygon(int polygonIndex)
{
    withDelegate(activeDelegate(), [polygonIndex](PreviewWindow* w) { w->setSelectedCorrectionPolygon(polygonIndex); });
}

void VulkanPreviewSurface::setBackgroundColor(const QColor& color)
{
    withDelegate(activeDelegate(), [&color](PreviewWindow* w) { w->setBackgroundColor(color); });
}

void VulkanPreviewSurface::setPreviewZoom(qreal zoom)
{
    withDelegate(activeDelegate(), [zoom](PreviewWindow* w) { w->setPreviewZoom(zoom); });
}

void VulkanPreviewSurface::setShowSpeakerTrackPoints(bool show)
{
    withDelegate(activeDelegate(), [show](PreviewWindow* w) { w->setShowSpeakerTrackPoints(show); });
}

void VulkanPreviewSurface::setShowSpeakerTrackBoxes(bool show)
{
    withDelegate(activeDelegate(), [show](PreviewWindow* w) { w->setShowSpeakerTrackBoxes(show); });
}

void VulkanPreviewSurface::setBoxstreamOverlaySource(const QString& source)
{
    withDelegate(activeDelegate(), [&source](PreviewWindow* w) { w->setBoxstreamOverlaySource(source); });
}

void VulkanPreviewSurface::setAudioSpeakerHoverModalEnabled(bool enabled)
{
    withDelegate(activeDelegate(), [enabled](PreviewWindow* w) { w->setAudioSpeakerHoverModalEnabled(enabled); });
}

void VulkanPreviewSurface::setAudioWaveformVisible(bool visible)
{
    withDelegate(activeDelegate(), [visible](PreviewWindow* w) { w->setAudioWaveformVisible(visible); });
}

bool VulkanPreviewSurface::audioSpeakerHoverModalEnabled() const
{
    return activeDelegate() ? activeDelegate()->audioSpeakerHoverModalEnabled() : true;
}

bool VulkanPreviewSurface::audioWaveformVisible() const
{
    return activeDelegate() ? activeDelegate()->audioWaveformVisible() : true;
}

void VulkanPreviewSurface::setViewMode(ViewMode mode)
{
    withDelegate(activeDelegate(), [mode](PreviewWindow* w) { w->setViewMode(mode); });
}

PreviewSurface::ViewMode VulkanPreviewSurface::viewMode() const
{
    return activeDelegate() ? activeDelegate()->viewMode() : PreviewSurface::ViewMode::Video;
}

void VulkanPreviewSurface::setAudioDynamicsSettings(const AudioDynamicsSettings& settings)
{
    withDelegate(activeDelegate(), [&settings](PreviewWindow* w) { w->setAudioDynamicsSettings(settings); });
}

PreviewSurface::AudioDynamicsSettings VulkanPreviewSurface::audioDynamicsSettings() const
{
    return activeDelegate() ? activeDelegate()->audioDynamicsSettings() : AudioDynamicsSettings{};
}

void VulkanPreviewSurface::setTranscriptOverlayInteractionEnabled(bool enabled)
{
    withDelegate(activeDelegate(), [enabled](PreviewWindow* w) { w->setTranscriptOverlayInteractionEnabled(enabled); });
}

void VulkanPreviewSurface::setTitleOverlayInteractionOnly(bool enabled)
{
    withDelegate(activeDelegate(), [enabled](PreviewWindow* w) { w->setTitleOverlayInteractionOnly(enabled); });
}

void VulkanPreviewSurface::setCorrectionDrawMode(bool enabled)
{
    withDelegate(activeDelegate(), [enabled](PreviewWindow* w) { w->setCorrectionDrawMode(enabled); });
}

bool VulkanPreviewSurface::correctionDrawMode() const
{
    return activeDelegate() ? activeDelegate()->correctionDrawMode() : false;
}

bool VulkanPreviewSurface::transcriptOverlayInteractionEnabled() const
{
    return activeDelegate() ? activeDelegate()->transcriptOverlayInteractionEnabled() : false;
}

bool VulkanPreviewSurface::titleOverlayInteractionOnly() const
{
    return activeDelegate() ? activeDelegate()->titleOverlayInteractionOnly() : false;
}

void VulkanPreviewSurface::setCorrectionDraftPoints(const QVector<QPointF>& points)
{
    withDelegate(activeDelegate(), [&points](PreviewWindow* w) { w->setCorrectionDraftPoints(points); });
}

qreal VulkanPreviewSurface::previewZoom() const
{
    return activeDelegate() ? activeDelegate()->previewZoom() : 1.0;
}

void VulkanPreviewSurface::resetPreviewPan()
{
    withDelegate(activeDelegate(), [](PreviewWindow* w) { w->resetPreviewPan(); });
}

QSize VulkanPreviewSurface::outputSize() const
{
    return activeDelegate() ? activeDelegate()->outputSize() : QSize(1080, 1920);
}

bool VulkanPreviewSurface::bypassGrading() const
{
    return activeDelegate() ? activeDelegate()->bypassGrading() : false;
}

bool VulkanPreviewSurface::correctionsEnabled() const
{
    return activeDelegate() ? activeDelegate()->correctionsEnabled() : false;
}

bool VulkanPreviewSurface::audioMuted() const
{
    return activeDelegate() ? activeDelegate()->audioMuted() : false;
}

int VulkanPreviewSurface::audioVolumePercent() const
{
    return activeDelegate() ? activeDelegate()->audioVolumePercent() : 100;
}

QString VulkanPreviewSurface::activeAudioClipLabel() const
{
    return activeDelegate() ? activeDelegate()->activeAudioClipLabel() : QString();
}

bool VulkanPreviewSurface::preparePlaybackAdvance(int64_t targetFrame)
{
    return activeDelegate() ? activeDelegate()->preparePlaybackAdvance(targetFrame) : true;
}

bool VulkanPreviewSurface::preparePlaybackAdvanceSample(int64_t targetSample)
{
    return activeDelegate() ? activeDelegate()->preparePlaybackAdvanceSample(targetSample) : true;
}

bool VulkanPreviewSurface::warmPlaybackLookahead(int futureFrames, int timeoutMs)
{
    return activeDelegate() ? activeDelegate()->warmPlaybackLookahead(futureFrames, timeoutMs) : true;
}

QImage VulkanPreviewSurface::latestPresentedFrameImageForClip(const QString& clipId) const
{
    return activeDelegate() ? activeDelegate()->latestPresentedFrameImageForClip(clipId) : QImage();
}

QJsonObject VulkanPreviewSurface::profilingSnapshot() const
{
    return activeDelegate() ? activeDelegate()->profilingSnapshot() : QJsonObject{};
}

void VulkanPreviewSurface::resetProfilingStats()
{
    withDelegate(activeDelegate(), [](PreviewWindow* w) { w->resetProfilingStats(); });
}

bool VulkanPreviewSurface::selectedOverlayIsTranscript() const
{
    return activeDelegate() ? activeDelegate()->selectedOverlayIsTranscript() : false;
}
