#include "vulkan_preview_surface.h"

#include "async_decoder.h"
#include "debug_controls.h"
#include "direct_vulkan_preview_presenter.h"
#include "frame_handle.h"
#include "media_pipeline_shared.h"
#include "timeline_cache.h"
#include "transcript_engine.h"

#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QMetaObject>
#include <QObject>

#include <algorithm>
#include <cmath>

using editor::FrameHandle;

namespace {
constexpr int kMaxVisibleBacklog = 4;

bool visualClipActiveAtSample(const TimelineClip& clip,
                              const QVector<TimelineTrack>& tracks,
                              int64_t samplePosition,
                              qreal framePosition,
                              bool bypassGrading)
{
    const int64_t clipStartSample = clipTimelineStartSamples(clip);
    const int64_t clipEndSample = clipStartSample + frameToSamples(clip.durationFrames);
    return clipVisualPlaybackEnabled(clip, tracks) &&
           samplePosition >= clipStartSample &&
           samplePosition < clipEndSample &&
           editor::clipIsActiveAtTimelineFrame(clip, tracks, framePosition, bypassGrading);
}
} // namespace

VulkanPreviewSurface::VulkanPreviewSurface(QWidget* parent)
{
    m_pipelineOwner = std::make_unique<QObject>();
    m_previousDecodePreference = editor::debugDecodePreference();
    if (m_previousDecodePreference != editor::DecodePreference::HardwareZeroCopy) {
        editor::setDebugDecodePreference(editor::DecodePreference::HardwareZeroCopy);
        m_forcedZeroCopyDecodePreference = true;
    }
    m_presenter = std::make_unique<DirectVulkanPreviewPresenter>(&m_interaction, parent);
    if (!m_presenter->isActive()) {
        m_failureReason = m_presenter->failureReason();
    }
}

VulkanPreviewSurface::~VulkanPreviewSurface()
{
    if (m_cache) {
        m_cache->stopPrefetching();
    }
    if (m_decoder) {
        m_decoder->shutdown();
    }
    if (m_forcedZeroCopyDecodePreference) {
        editor::setDebugDecodePreference(m_previousDecodePreference);
    }
}

bool VulkanPreviewSurface::isNativeActive() const
{
    return m_presenter && m_presenter->isActive();
}

bool VulkanPreviewSurface::isNativePresentationActive() const
{
    return isNativeActive();
}

QWidget* VulkanPreviewSurface::asWidget()
{
    return m_presenter ? m_presenter->widget() : nullptr;
}

const QWidget* VulkanPreviewSurface::asWidget() const
{
    return m_presenter ? m_presenter->widget() : nullptr;
}

void VulkanPreviewSurface::requestNativeUpdate()
{
    if (!m_bulkUpdating && m_presenter) {
        m_presenter->requestUpdate();
    }
}

void VulkanPreviewSurface::updateNativeTitle()
{
    if (m_presenter) {
        m_presenter->updateTitle();
    }
}

void VulkanPreviewSurface::setPlaybackState(bool playing)
{
    m_interaction.playing = playing;
    if (m_cache) {
        m_cache->setPlaybackState(playing ? editor::TimelineCache::PlaybackState::Playing
                                          : editor::TimelineCache::PlaybackState::Stopped);
    }
    requestFramesForCurrentPosition();
    requestNativeUpdate();
}

void VulkanPreviewSurface::setCurrentFrame(int64_t frame)
{
    m_interaction.currentFrame = std::max<int64_t>(0, frame);
    m_interaction.currentFramePosition = static_cast<qreal>(m_interaction.currentFrame);
    if (m_cache) {
        m_cache->setPlayheadFrame(m_interaction.currentFrame);
    }
    requestFramesForCurrentPosition();
    refreshBoxstreamOverlays();
    updateNativeTitle();
    requestNativeUpdate();
}

void VulkanPreviewSurface::setCurrentPlaybackSample(int64_t samplePosition)
{
    m_interaction.currentSample = std::max<int64_t>(0, samplePosition);
    requestFramesForCurrentPosition();
    refreshBoxstreamOverlays();
    requestNativeUpdate();
}

void VulkanPreviewSurface::setClipCount(int count)
{
    m_interaction.clipCount = std::max(0, count);
    updateNativeTitle();
    requestNativeUpdate();
}

void VulkanPreviewSurface::setSelectedClipId(const QString& clipId)
{
    m_interaction.selectedClipId = clipId;
    requestNativeUpdate();
}

void VulkanPreviewSurface::setTimelineClips(const QVector<TimelineClip>& clips)
{
    m_interaction.clips = clips;
    m_interaction.clipCount = clips.size();
    registerVisibleClips();
    refreshVulkanFrameStatuses();
    refreshBoxstreamOverlays();
    updateNativeTitle();
    requestFramesForCurrentPosition();
    requestNativeUpdate();
}

void VulkanPreviewSurface::setTimelineTracks(const QVector<TimelineTrack>& tracks)
{
    m_interaction.tracks = tracks;
    registerVisibleClips();
    refreshVulkanFrameStatuses();
    refreshBoxstreamOverlays();
    requestFramesForCurrentPosition();
    requestNativeUpdate();
}

void VulkanPreviewSurface::setRenderSyncMarkers(const QVector<RenderSyncMarker>& markers)
{
    m_interaction.renderSyncMarkers = markers;
    if (m_cache) {
        m_cache->setRenderSyncMarkers(markers);
    }
    refreshVulkanFrameStatuses();
    requestFramesForCurrentPosition();
    requestNativeUpdate();
}

void VulkanPreviewSurface::setExportRanges(const QVector<ExportRangeSegment>& ranges)
{
    m_interaction.exportRanges = ranges;
    if (m_cache) {
        m_cache->setExportRanges(ranges);
    }
}

void VulkanPreviewSurface::invalidateTranscriptOverlayCache(const QString& clipFilePath)
{
    if (clipFilePath.trimmed().isEmpty()) {
        m_boxstreamOverlayCache.clear();
    } else {
        m_boxstreamOverlayCache.remove(QFileInfo(clipFilePath).absoluteFilePath());
        m_boxstreamOverlayCache.remove(clipFilePath);
    }
    refreshBoxstreamOverlays();
    requestNativeUpdate();
}

void VulkanPreviewSurface::beginBulkUpdate()
{
    ++m_bulkDepth;
    m_bulkUpdating = true;
}

void VulkanPreviewSurface::endBulkUpdate()
{
    if (m_bulkDepth > 0) {
        --m_bulkDepth;
    }
    m_bulkUpdating = m_bulkDepth > 0;
    if (!m_bulkUpdating) {
        requestFramesForCurrentPosition();
        requestNativeUpdate();
    }
}

QString VulkanPreviewSurface::backendName() const
{
    return m_presenter ? m_presenter->backendName() : QStringLiteral("Vulkan Preview Unavailable");
}

void VulkanPreviewSurface::setRenderBackendPreference(const QString& backendName)
{
    Q_UNUSED(backendName);
}

void VulkanPreviewSurface::setAudioMuted(bool muted)
{
    m_interaction.audioMuted = muted;
}

void VulkanPreviewSurface::setAudioVolume(qreal volume)
{
    m_interaction.audioVolume = std::clamp(volume, 0.0, 1.0);
}

void VulkanPreviewSurface::setOutputSize(const QSize& size)
{
    m_interaction.outputSize = QSize(std::max(16, size.width()), std::max(16, size.height()));
    requestNativeUpdate();
}

void VulkanPreviewSurface::setHideOutsideOutputWindow(bool hide)
{
    m_hideOutsideOutputWindow = hide;
}

void VulkanPreviewSurface::setBypassGrading(bool bypass)
{
    m_bypassGrading = bypass;
    requestNativeUpdate();
}

void VulkanPreviewSurface::setCorrectionsEnabled(bool enabled)
{
    m_correctionsEnabled = enabled;
    requestNativeUpdate();
}

void VulkanPreviewSurface::setShowCorrectionOverlays(bool show)
{
    m_showCorrectionOverlays = show;
    requestNativeUpdate();
}

void VulkanPreviewSurface::setSelectedCorrectionPolygon(int polygonIndex)
{
    m_selectedCorrectionPolygon = polygonIndex;
    requestNativeUpdate();
}

void VulkanPreviewSurface::setBackgroundColor(const QColor& color)
{
    m_interaction.backgroundColor = color;
    requestNativeUpdate();
}

void VulkanPreviewSurface::setPreviewZoom(qreal zoom)
{
    m_interaction.previewZoom = std::clamp(zoom, 0.1, 16.0);
    requestNativeUpdate();
}

void VulkanPreviewSurface::setShowSpeakerTrackPoints(bool show)
{
    m_showSpeakerTrackPoints = show;
    requestNativeUpdate();
}

void VulkanPreviewSurface::setShowSpeakerTrackBoxes(bool show)
{
    m_showSpeakerTrackBoxes = show;
    refreshBoxstreamOverlays();
    requestNativeUpdate();
}

void VulkanPreviewSurface::setBoxstreamOverlaySource(const QString& source)
{
    m_boxstreamOverlaySource = source.trimmed().isEmpty() ? QStringLiteral("all") : source.trimmed().toLower();
    refreshBoxstreamOverlays();
    requestNativeUpdate();
}

void VulkanPreviewSurface::setAudioSpeakerHoverModalEnabled(bool enabled)
{
    m_audioSpeakerHoverModalEnabled = enabled;
}

void VulkanPreviewSurface::setAudioWaveformVisible(bool visible)
{
    m_audioWaveformVisible = visible;
}

bool VulkanPreviewSurface::audioSpeakerHoverModalEnabled() const
{
    return m_audioSpeakerHoverModalEnabled;
}

bool VulkanPreviewSurface::audioWaveformVisible() const
{
    return m_audioWaveformVisible;
}

void VulkanPreviewSurface::setViewMode(ViewMode mode)
{
    m_interaction.viewMode = mode;
    requestNativeUpdate();
}

PreviewSurface::ViewMode VulkanPreviewSurface::viewMode() const
{
    return m_interaction.viewMode;
}

void VulkanPreviewSurface::setAudioDynamicsSettings(const AudioDynamicsSettings& settings)
{
    m_audioDynamics = settings;
}

PreviewSurface::AudioDynamicsSettings VulkanPreviewSurface::audioDynamicsSettings() const
{
    return m_audioDynamics;
}

void VulkanPreviewSurface::setTranscriptOverlayInteractionEnabled(bool enabled)
{
    m_interaction.transcriptOverlayInteractionEnabled = enabled;
}

void VulkanPreviewSurface::setTitleOverlayInteractionOnly(bool enabled)
{
    m_interaction.titleOverlayInteractionOnly = enabled;
}

void VulkanPreviewSurface::setCorrectionDrawMode(bool enabled)
{
    m_interaction.correctionDrawMode = enabled;
}

bool VulkanPreviewSurface::correctionDrawMode() const
{
    return m_interaction.correctionDrawMode;
}

bool VulkanPreviewSurface::transcriptOverlayInteractionEnabled() const
{
    return m_interaction.transcriptOverlayInteractionEnabled;
}

bool VulkanPreviewSurface::titleOverlayInteractionOnly() const
{
    return m_interaction.titleOverlayInteractionOnly;
}

void VulkanPreviewSurface::setCorrectionDraftPoints(const QVector<QPointF>& points)
{
    m_interaction.transient.correctionDraftPoints = points;
    requestNativeUpdate();
}

qreal VulkanPreviewSurface::previewZoom() const
{
    return m_interaction.previewZoom;
}

void VulkanPreviewSurface::resetPreviewPan()
{
    m_interaction.previewPanOffset = QPointF();
    requestNativeUpdate();
}

QSize VulkanPreviewSurface::outputSize() const
{
    return m_interaction.outputSize;
}

bool VulkanPreviewSurface::bypassGrading() const
{
    return m_bypassGrading;
}

bool VulkanPreviewSurface::correctionsEnabled() const
{
    return m_correctionsEnabled;
}

bool VulkanPreviewSurface::audioMuted() const
{
    return m_interaction.audioMuted;
}

int VulkanPreviewSurface::audioVolumePercent() const
{
    return qRound(m_interaction.audioVolume * 100.0);
}

QString VulkanPreviewSurface::activeAudioClipLabel() const
{
    return m_activeAudioClipLabel;
}

bool VulkanPreviewSurface::isSampleWithinClip(const TimelineClip& clip, int64_t samplePosition) const
{
    const int64_t clipStartSample = clipTimelineStartSamples(clip);
    const int64_t clipEndSample = clipStartSample + frameToSamples(clip.durationFrames);
    return samplePosition >= clipStartSample && samplePosition < clipEndSample;
}

int64_t VulkanPreviewSurface::sourceFrameForSample(const TimelineClip& clip, int64_t samplePosition) const
{
    return sourceFrameForClipAtTimelinePosition(
        clip,
        samplesToFramePosition(samplePosition),
        m_interaction.renderSyncMarkers);
}

void VulkanPreviewSurface::ensureFramePipeline()
{
    if (m_cache || !m_pipelineOwner) {
        return;
    }

    m_decoder = std::make_unique<editor::AsyncDecoder>();
    if (!m_decoder->initialize()) {
        return;
    }
    if (editor::MemoryBudget* budget = m_decoder->memoryBudget()) {
        budget->setMaxCpuMemory(768 * 1024 * 1024);
    }

    m_cache = std::make_unique<editor::TimelineCache>(m_decoder.get(), m_decoder->memoryBudget());
    m_cache->setMaxMemory(512 * 1024 * 1024);
    m_cache->setLookaheadFrames(24);
    m_cache->setPlaybackState(m_interaction.playing ? editor::TimelineCache::PlaybackState::Playing
                                                     : editor::TimelineCache::PlaybackState::Stopped);
    m_cache->setPlayheadFrame(m_interaction.currentFrame);
    m_cache->setRenderSyncMarkers(m_interaction.renderSyncMarkers);
    m_cache->setExportRanges(m_interaction.exportRanges);
    QObject::connect(m_cache.get(),
                     &editor::TimelineCache::frameLoaded,
                     m_pipelineOwner.get(),
                     [this](const QString&, int64_t, FrameHandle) {
                         refreshVulkanFrameStatuses();
                         requestNativeUpdate();
                     });
    registerVisibleClips();
    m_cache->startPrefetching();
}

void VulkanPreviewSurface::registerVisibleClips()
{
    if (!m_cache) {
        return;
    }

    QSet<QString> visible;
    for (const TimelineClip& clip : m_interaction.clips) {
        if (!clipVisualPlaybackEnabled(clip, m_interaction.tracks) ||
            clip.mediaType != ClipMediaType::Video ||
            clip.sourceKind == MediaSourceKind::ImageSequence ||
            clip.filePath.isEmpty()) {
            continue;
        }
        visible.insert(clip.id);
        if (!m_registeredClips.contains(clip.id)) {
            m_cache->registerClip(clip);
            m_registeredClips.insert(clip.id);
        }
    }

    for (auto it = m_registeredClips.begin(); it != m_registeredClips.end();) {
        if (!visible.contains(*it)) {
            m_cache->unregisterClip(*it);
            it = m_registeredClips.erase(it);
        } else {
            ++it;
        }
    }
}

void VulkanPreviewSurface::requestFramesForCurrentPosition()
{
    if (m_bulkUpdating) {
        return;
    }

    bool hasActiveDecodableClip = false;
    for (const TimelineClip& clip : m_interaction.clips) {
        if (clip.mediaType != ClipMediaType::Video || clip.sourceKind == MediaSourceKind::ImageSequence || clip.filePath.isEmpty()) {
            continue;
        }
        if (visualClipActiveAtSample(clip,
                                     m_interaction.tracks,
                                     m_interaction.currentSample,
                                     m_interaction.currentFramePosition,
                                     m_bypassGrading)) {
            hasActiveDecodableClip = true;
            break;
        }
    }
    if (!hasActiveDecodableClip) {
        refreshVulkanFrameStatuses();
        return;
    }

    ensureFramePipeline();
    if (!m_cache) {
        refreshVulkanFrameStatuses();
        return;
    }

    registerVisibleClips();
    for (const TimelineClip& clip : m_interaction.clips) {
        if (clip.mediaType != ClipMediaType::Video || clip.sourceKind == MediaSourceKind::ImageSequence || clip.filePath.isEmpty()) {
            continue;
        }
        if (!visualClipActiveAtSample(clip,
                                      m_interaction.tracks,
                                      m_interaction.currentSample,
                                      m_interaction.currentFramePosition,
                                      m_bypassGrading)) {
            continue;
        }

        const int64_t localFrame = sourceFrameForSample(clip, m_interaction.currentSample);
        const bool cached = m_interaction.playing
            ? m_cache->hasDisplayableFrameForPreview(clip.id, localFrame, true, true)
            : m_cache->isFrameCached(clip.id, localFrame);
        const bool pending = m_cache->isVisibleRequestPending(clip.id, localFrame);
        const bool forceRetry = m_cache->shouldForceVisibleRequestRetry(clip.id, localFrame, 250);
        if (!cached && (!pending || forceRetry) &&
            m_cache->pendingVisibleRequestCount() < kMaxVisibleBacklog) {
            m_cache->requestFrame(clip.id, localFrame, [this](FrameHandle) {
                if (!m_pipelineOwner) {
                    return;
                }
                QMetaObject::invokeMethod(
                    m_pipelineOwner.get(),
                    [this]() {
                        refreshVulkanFrameStatuses();
                        requestNativeUpdate();
                    },
                    Qt::QueuedConnection);
            });
        }
    }
    refreshVulkanFrameStatuses();
}

void VulkanPreviewSurface::refreshVulkanFrameStatuses()
{
    QVector<VulkanPreviewClipFrameStatus> statuses;
    int exactCount = 0;
    int approxCount = 0;
    int missingCount = 0;
    int hardwareCount = 0;
    int cpuCount = 0;

    for (const TimelineClip& clip : m_interaction.clips) {
        if (clip.mediaType != ClipMediaType::Video || clip.sourceKind == MediaSourceKind::ImageSequence || clip.filePath.isEmpty()) {
            continue;
        }
        if (!visualClipActiveAtSample(clip,
                                      m_interaction.tracks,
                                      m_interaction.currentSample,
                                      m_interaction.currentFramePosition,
                                      m_bypassGrading)) {
            continue;
        }

        const int64_t localFrame = sourceFrameForSample(clip, m_interaction.currentSample);
        FrameHandle exactFrame;
        FrameHandle selectedFrame;
        if (m_cache) {
            exactFrame = m_cache->getCachedFrame(clip.id, localFrame);
            selectedFrame = exactFrame;
            if (selectedFrame.isNull() && m_interaction.playing) {
                selectedFrame = m_cache->getLatestCachedFrame(clip.id, localFrame);
            }
            if (selectedFrame.isNull()) {
                selectedFrame = m_cache->getBestCachedFrame(clip.id, localFrame);
            }
        }

        VulkanPreviewClipFrameStatus status;
        status.clipId = clip.id;
        status.label = clip.label;
        status.requestedSourceFrame = localFrame;
        status.active = true;
        status.hasFrame = !selectedFrame.isNull() &&
                          (selectedFrame.hasHardwareFrame() || selectedFrame.hasGpuTexture());
        status.exact = status.hasFrame && !exactFrame.isNull() && selectedFrame == exactFrame;
        if (status.hasFrame) {
            status.presentedSourceFrame = selectedFrame.frameNumber();
            status.frameSize = selectedFrame.size();
            status.hardwareFrame = selectedFrame.hasHardwareFrame();
            status.gpuTexture = selectedFrame.hasGpuTexture();
            status.cpuImage = false;
            exactCount += status.exact ? 1 : 0;
            approxCount += status.exact ? 0 : 1;
            hardwareCount += status.hardwareFrame ? 1 : 0;
            cpuCount += 0;
        } else {
            ++missingCount;
        }
        statuses.push_back(status);
    }

    m_interaction.vulkanFrameStatuses = statuses;
    m_frameStatusExactCount = exactCount;
    m_frameStatusApproxCount = approxCount;
    m_frameStatusMissingCount = missingCount;
    m_frameStatusHardwareCount = hardwareCount;
    m_frameStatusCpuCount = cpuCount;
}

QVector<VulkanPreviewSurface::BoxstreamTrack> VulkanPreviewSurface::parseContinuityTracksForClip(
    const TimelineClip& clip,
    const QJsonObject& artifactRoot) const
{
    QVector<BoxstreamTrack> tracks;
    const QJsonObject byClip = artifactRoot.value(QStringLiteral("continuity_boxstreams_by_clip")).toObject();
    const QJsonObject continuityRoot = byClip.value(clip.id.trimmed()).toObject();
    const QJsonArray streams = continuityRoot.value(QStringLiteral("streams")).toArray();
    tracks.reserve(streams.size());
    for (const QJsonValue& streamValue : streams) {
        const QJsonObject streamObj = streamValue.toObject();
        BoxstreamTrack track;
        track.streamId = streamObj.value(QStringLiteral("stream_id")).toString().trimmed();
        track.trackId = streamObj.value(QStringLiteral("track_id")).toInt(-1);
        track.source = streamObj.value(QStringLiteral("source")).toString().trimmed().toLower();
        const QJsonArray keyframes = streamObj.value(QStringLiteral("keyframes")).toArray();
        track.keyframes.reserve(keyframes.size());
        for (const QJsonValue& keyframeValue : keyframes) {
            const QJsonObject keyframeObj = keyframeValue.toObject();
            if (!keyframeObj.contains(QStringLiteral("frame"))) {
                continue;
            }
            const qreal boxSize = qBound<qreal>(
                0.001, keyframeObj.value(QStringLiteral("box_size")).toDouble(-1.0), 1.0);
            const qreal x = qBound<qreal>(0.0, keyframeObj.value(QStringLiteral("x")).toDouble(0.5), 1.0);
            const qreal y = qBound<qreal>(0.0, keyframeObj.value(QStringLiteral("y")).toDouble(0.5), 1.0);
            BoxstreamKeyframe keyframe;
            keyframe.frame = keyframeObj.value(QStringLiteral("frame")).toVariant().toLongLong();
            keyframe.boxNorm = QRectF(qBound<qreal>(0.0, x - (boxSize * 0.5), 1.0),
                                      qBound<qreal>(0.0, y - (boxSize * 0.5), 1.0),
                                      boxSize,
                                      boxSize).intersected(QRectF(0.0, 0.0, 1.0, 1.0));
            keyframe.confidence = qBound<qreal>(
                0.0, keyframeObj.value(QStringLiteral("confidence")).toDouble(1.0), 1.0);
            keyframe.source = keyframeObj.value(QStringLiteral("source")).toString(track.source).trimmed().toLower();
            if (track.source.isEmpty()) {
                track.source = keyframe.source;
            }
            if (keyframe.boxNorm.isValid()) {
                track.keyframes.push_back(keyframe);
            }
        }
        std::sort(track.keyframes.begin(), track.keyframes.end(), [](const BoxstreamKeyframe& a, const BoxstreamKeyframe& b) {
            return a.frame < b.frame;
        });
        if (!track.keyframes.isEmpty()) {
            tracks.push_back(track);
        }
    }
    return tracks;
}

QVector<VulkanPreviewSurface::BoxstreamTrack> VulkanPreviewSurface::loadBoxstreamTracksForClip(
    const TimelineClip& clip)
{
    const QString clipPath = QFileInfo(clip.filePath).absoluteFilePath();
    const QString transcriptPath = activeTranscriptPathForClipFile(clip.filePath);
    const QFileInfo transcriptInfo(transcriptPath);
    const QString signature = QStringLiteral("%1|%2|%3|%4")
                                  .arg(clip.id)
                                  .arg(transcriptInfo.absoluteFilePath())
                                  .arg(transcriptInfo.exists() ? transcriptInfo.lastModified().toMSecsSinceEpoch() : 0)
                                  .arg(m_boxstreamOverlaySource);
    BoxstreamOverlayCacheEntry& entry = m_boxstreamOverlayCache[clipPath];
    if (entry.signature == signature) {
        return entry.tracks;
    }

    entry = BoxstreamOverlayCacheEntry{};
    entry.signature = signature;
    if (!transcriptInfo.exists() || !transcriptInfo.isFile()) {
        return entry.tracks;
    }

    editor::TranscriptEngine engine;
    QJsonObject artifactRoot;
    if (engine.loadBoxstreamArtifact(transcriptPath, &artifactRoot)) {
        entry.tracks += parseContinuityTracksForClip(clip, artifactRoot);
    }
    return entry.tracks;
}

void VulkanPreviewSurface::refreshBoxstreamOverlays()
{
    QVector<VulkanPreviewBoxstreamOverlay> overlays;
    if (!m_showSpeakerTrackBoxes) {
        m_interaction.boxstreamOverlays = overlays;
        return;
    }
    const QString sourceFilter = m_boxstreamOverlaySource.trimmed().isEmpty()
        ? QStringLiteral("all")
        : m_boxstreamOverlaySource.trimmed().toLower();
    for (const TimelineClip& clip : m_interaction.clips) {
        if (clip.mediaType != ClipMediaType::Video || clip.filePath.isEmpty()) {
            continue;
        }
        if (!visualClipActiveAtSample(clip,
                                      m_interaction.tracks,
                                      m_interaction.currentSample,
                                      m_interaction.currentFramePosition,
                                      m_bypassGrading)) {
            continue;
        }
        const int64_t localFrame = sourceFrameForSample(clip, m_interaction.currentSample);
        const QVector<BoxstreamTrack> tracks = loadBoxstreamTracksForClip(clip);
        for (const BoxstreamTrack& track : tracks) {
            if (track.keyframes.isEmpty()) {
                continue;
            }
            const bool sourceAccepted =
                sourceFilter == QStringLiteral("all") ||
                sourceFilter == track.source ||
                sourceFilter == track.streamId.trimmed().toLower();
            if (!sourceAccepted) {
                continue;
            }
            const BoxstreamKeyframe* best = nullptr;
            for (const BoxstreamKeyframe& keyframe : track.keyframes) {
                if (keyframe.frame > localFrame) {
                    break;
                }
                best = &keyframe;
            }
            if (!best) {
                best = &track.keyframes.constFirst();
            }
            if (qAbs(best->frame - localFrame) > 90) {
                continue;
            }
            VulkanPreviewBoxstreamOverlay overlay;
            overlay.clipId = clip.id;
            overlay.streamId = track.streamId;
            overlay.source = best->source.isEmpty() ? track.source : best->source;
            overlay.trackId = track.trackId;
            overlay.sourceFrame = best->frame;
            overlay.boxNorm = best->boxNorm;
            overlay.confidence = best->confidence;
            overlays.push_back(overlay);
        }
    }
    m_interaction.boxstreamOverlays = overlays;
}

bool VulkanPreviewSurface::preparePlaybackAdvance(int64_t targetFrame)
{
    return preparePlaybackAdvanceSample(frameToSamples(targetFrame));
}

bool VulkanPreviewSurface::preparePlaybackAdvanceSample(int64_t targetSample)
{
    ensureFramePipeline();
    if (!m_cache) {
        return false;
    }

    bool ready = true;
    for (const TimelineClip& clip : m_interaction.clips) {
        if (clip.mediaType != ClipMediaType::Video || clip.sourceKind == MediaSourceKind::ImageSequence || clip.filePath.isEmpty()) {
            continue;
        }
        if (!clipVisualPlaybackEnabled(clip, m_interaction.tracks) ||
            !isSampleWithinClip(clip, targetSample)) {
            continue;
        }
        const int64_t localFrame = sourceFrameForSample(clip, targetSample);
        if (m_cache->hasDisplayableFrameForPreview(clip.id, localFrame, m_interaction.playing, true)) {
            continue;
        }
        ready = false;
        if (!m_cache->isVisibleRequestPending(clip.id, localFrame)) {
            m_cache->requestFrame(clip.id, localFrame, [this](FrameHandle) {
                if (!m_pipelineOwner) {
                    return;
                }
                QMetaObject::invokeMethod(
                    m_pipelineOwner.get(),
                    [this]() {
                        refreshVulkanFrameStatuses();
                        requestNativeUpdate();
                    },
                    Qt::QueuedConnection);
            });
        }
    }
    refreshVulkanFrameStatuses();
    return ready;
}

bool VulkanPreviewSurface::warmPlaybackLookahead(int futureFrames, int timeoutMs)
{
    Q_UNUSED(timeoutMs);
    if (futureFrames <= 0) {
        return true;
    }
    ensureFramePipeline();
    if (!m_cache) {
        return false;
    }
    for (int offset = 0; offset <= futureFrames; ++offset) {
        preparePlaybackAdvanceSample(m_interaction.currentSample + frameToSamples(offset));
    }
    return true;
}

QImage VulkanPreviewSurface::latestPresentedFrameImageForClip(const QString& clipId) const
{
    Q_UNUSED(clipId);
    return QImage();
}

QJsonObject VulkanPreviewSurface::profilingSnapshot() const
{
    return m_presenter ? m_presenter->profilingSnapshot() : QJsonObject{};
}

void VulkanPreviewSurface::resetProfilingStats()
{
    if (m_presenter) {
        m_presenter->resetProfilingStats();
    }
}

bool VulkanPreviewSurface::selectedOverlayIsTranscript() const
{
    return m_interaction.transcriptOverlayInteractionEnabled && !m_interaction.selectedClipId.isEmpty();
}
