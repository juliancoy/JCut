#include "vulkan_preview_surface.h"
#include "facedetections_artifact_utils.h"
#include "facedetections_runtime.h"
#include "facedetections_time_mapping.h"

#include "async_decoder.h"
#include "audio_preview_support.h"
#include "debug_controls.h"
#include "direct_vulkan_preview_presenter.h"
#include "frame_handle.h"
#include "media_pipeline_shared.h"
#include "editor_shared.h"
#include "preview_frame_selection.h"
#include "preview_view_transform.h"
#include "timeline_cache.h"
#include "transcript_engine.h"

#include <QFileInfo>
#include <QCoreApplication>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QMetaObject>
#include <QObject>
#include <QStringList>
#include <QThread>

#include <algorithm>
#include <cmath>
#include <numeric>
#include <utility>
#include <QDateTime>

using editor::FrameHandle;

namespace {
constexpr int kDefaultMaxVisibleBacklog = 1;
constexpr size_t kVulkanPreviewCpuCacheBytes = 192ull * 1024ull * 1024ull;
constexpr size_t kVulkanPreviewGpuCacheBytes = 512ull * 1024ull * 1024ull;
constexpr int kDefaultVulkanPreviewSourceLookaheadFrames = 2;
constexpr int kDefaultVulkanPreviewProxyLookaheadFrames = 8;

QString cacheRegistrationKeyForClip(const TimelineClip& clip)
{
    const QString decodePath = interactivePreviewMediaPathForClip(clip);
    return QStringLiteral("%1|%2|%3|%4|%5|%6|%7|%8|%9")
        .arg(decodePath)
        .arg(clip.filePath)
        .arg(static_cast<int>(clip.sourceKind))
        .arg(clip.startFrame)
        .arg(clip.durationFrames)
        .arg(clip.sourceInFrame)
        .arg(clip.sourceDurationFrames)
        .arg(QString::number(clip.playbackRate, 'f', 6))
        .arg(QString::number(clip.sourceFps, 'f', 6));
}

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

bool gradingCurveDiffersFromIdentity(const QVector<QPointF>& points, bool smoothingEnabled)
{
    const QVector<QPointF> identity = defaultGradingCurvePoints();
    const QVector<quint8> lut = gradingCurveLut8(points, TimelineClip::kGradingCurveLutSize, smoothingEnabled);
    const QVector<quint8> identityLut = gradingCurveLut8(identity, TimelineClip::kGradingCurveLutSize, smoothingEnabled);
    return !lut.isEmpty() && !identityLut.isEmpty() && lut != identityLut;
}

bool gradingUsesCurveLut(const TimelineClip::GradingKeyframe& grade)
{
    return gradingCurveDiffersFromIdentity(grade.curvePointsR, grade.curveSmoothingEnabled) ||
           gradingCurveDiffersFromIdentity(grade.curvePointsG, grade.curveSmoothingEnabled) ||
           gradingCurveDiffersFromIdentity(grade.curvePointsB, grade.curveSmoothingEnabled) ||
           gradingCurveDiffersFromIdentity(grade.curvePointsLuma, grade.curveSmoothingEnabled);
}

bool pointInNormalizedPolygon(const QPointF& p, const QVector<QPointF>& polygon)
{
    bool inside = false;
    for (int i = 0, j = polygon.size() - 1; i < polygon.size(); j = i++) {
        const QPointF& a = polygon.at(i);
        const QPointF& b = polygon.at(j);
        const bool crosses = ((a.y() > p.y()) != (b.y() > p.y())) &&
            (p.x() < (b.x() - a.x()) * (p.y() - a.y()) / ((b.y() - a.y()) + 1e-12) + a.x());
        if (crosses) {
            inside = !inside;
        }
    }
    return inside;
}

QImage applyCorrectionMasksToCpuImage(const QImage& source,
                                      const QVector<TimelineClip::CorrectionPolygon>& polygons)
{
    if (source.isNull() || polygons.isEmpty()) {
        return source;
    }
    QImage masked = source.convertToFormat(QImage::Format_ARGB32);
    const qreal invW = 1.0 / qMax(1, masked.width());
    const qreal invH = 1.0 / qMax(1, masked.height());
    for (int y = 0; y < masked.height(); ++y) {
        QRgb* row = reinterpret_cast<QRgb*>(masked.scanLine(y));
        for (int x = 0; x < masked.width(); ++x) {
            const QPointF p((x + 0.5) * invW, (y + 0.5) * invH);
            bool erased = false;
            for (const TimelineClip::CorrectionPolygon& polygon : polygons) {
                if (polygon.enabled && pointInNormalizedPolygon(p, polygon.pointsNormalized)) {
                    erased = true;
                    break;
                }
            }
            if (erased) {
                row[x] = qRgba(qRed(row[x]), qGreen(row[x]), qBlue(row[x]), 0);
            }
        }
    }
    return masked.convertToFormat(QImage::Format_ARGB32_Premultiplied);
}

} // namespace

VulkanPreviewSurface::VulkanPreviewSurface(QWidget* parent)
{
    m_pipelineOwner = std::make_unique<QObject>();
    m_playbackTuning.visibleBacklogLimit = kDefaultMaxVisibleBacklog;
    m_playbackTuning.sourceLookaheadFrames = kDefaultVulkanPreviewSourceLookaheadFrames;
    m_playbackTuning.proxyLookaheadFrames = kDefaultVulkanPreviewProxyLookaheadFrames;
    m_configuredPlaybackTuning = m_playbackTuning;
    m_previousDecodePreference = editor::debugDecodePreference();
    if (m_previousDecodePreference != editor::DecodePreference::Auto) {
        editor::setDebugDecodePreference(editor::DecodePreference::Auto);
        m_forcedPreviewDecodePreference = true;
    }
    m_presenter = std::make_unique<DirectVulkanPreviewPresenter>(&m_interaction, parent);
    if (!m_presenter->isActive()) {
        m_failureReason = m_presenter->failureReason();
    }
    if (m_presenter) {
        m_presenter->setInteractionCallbacks(
            [this](const QString& clipId) {
                if (selectionRequested) {
                    selectionRequested(clipId);
                }
            },
            [this](const QString& clipId, qreal scaleX, qreal scaleY, bool finalize) {
                if (resizeRequested) {
                    resizeRequested(clipId, scaleX, scaleY, finalize);
                }
            },
            [this](const QString& clipId, qreal x, qreal y, bool finalize) {
                if (moveRequested) {
                    moveRequested(clipId, x, y, finalize);
                }
            },
            [this](int64_t samplePosition) {
                if (playbackSampleRequested) {
                    playbackSampleRequested(samplePosition);
                }
            },
            [this](const QString& clipId, qreal xNorm, qreal yNorm) {
                if (correctionPointRequested) {
                    correctionPointRequested(clipId, xNorm, yNorm);
                }
            },
            [this](const QString& clipId, qreal xNorm, qreal yNorm) {
                if (speakerPointRequested) {
                    speakerPointRequested(clipId, xNorm, yNorm);
                }
            },
            [this](const QString& clipId, qreal xNorm, qreal yNorm, qreal side) {
                if (speakerBoxRequested) {
                    speakerBoxRequested(clipId, xNorm, yNorm, side);
                }
            },
            [this](const QString& clipId,
                   int trackId,
                   const QString& streamId,
                   int64_t sourceFrame,
                   qreal xNorm,
                   qreal yNorm,
                   qreal side) {
                if (faceStreamBoxRequested) {
                    faceStreamBoxRequested(clipId, trackId, streamId, sourceFrame, xNorm, yNorm, side);
                }
            },
            [this](const QString& message) {
                if (faceStreamBoxClickStatus) {
                    faceStreamBoxClickStatus(message);
                }
            },
            [this](const QString& clipId) {
                if (createKeyframeRequested) {
                    createKeyframeRequested(clipId);
                }
            });
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
    if (m_forcedPreviewDecodePreference) {
        editor::setDebugDecodePreference(m_previousDecodePreference);
    }
}

bool VulkanPreviewSurface::isNativeActive() const
{
    return m_presenter && m_presenter->isActive();
}

bool VulkanPreviewSurface::isNativePresentationActive() const
{
    return m_presenter && m_presenter->widget() != nullptr;
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
    if (!playing) {
        m_playbackSmoothnessSamples.clear();
        m_adaptivePlaybackBoostLevel = 0;
        m_lastAdaptivePlaybackTuningAdjustMs = 0;
        applyAdaptivePlaybackTuning();
    }
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
    refreshFacestreamOverlays();
    updateNativeTitle();
    requestNativeUpdate();
}

void VulkanPreviewSurface::setCurrentPlaybackSample(int64_t samplePosition)
{
    m_interaction.currentSample = std::max<int64_t>(0, samplePosition);
    m_interaction.currentFramePosition = samplesToFramePosition(m_interaction.currentSample);
    m_interaction.currentFrame = qMax<int64_t>(
        0, static_cast<int64_t>(std::floor(m_interaction.currentFramePosition)));
    syncAudioPreviewPanToPlayhead(&m_interaction);
    if (m_cache) {
        m_cache->setPlayheadFrame(m_interaction.currentFrame);
    }
    requestFramesForCurrentPosition();
    refreshVulkanFrameStatuses();
    refreshFacestreamOverlays();
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
    refreshFacestreamOverlays();
    updateNativeTitle();
    requestFramesForCurrentPosition();
    requestNativeUpdate();
}

void VulkanPreviewSurface::setTimelineTracks(const QVector<TimelineTrack>& tracks)
{
    m_interaction.tracks = tracks;
    registerVisibleClips();
    refreshVulkanFrameStatuses();
    refreshFacestreamOverlays();
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

void VulkanPreviewSurface::setUseProxyMedia(bool useProxyMedia)
{
    if (m_useProxyMedia == useProxyMedia) {
        return;
    }
    m_useProxyMedia = useProxyMedia;
    if (m_cache) {
        m_cache->setLookaheadFrames(effectivePlaybackLookaheadFrames());
        for (const QString& clipId : std::as_const(m_registeredClips)) {
            m_cache->unregisterClip(clipId);
        }
        m_registeredClips.clear();
        m_registeredClipRegistrationKeys.clear();
    }
    registerVisibleClips();
    requestFramesForCurrentPosition();
    requestNativeUpdate();
}

void VulkanPreviewSurface::setPlaybackTuning(const PlaybackTuning& tuning)
{
    PlaybackTuning normalized;
    normalized.visibleBacklogLimit = qBound(1, tuning.visibleBacklogLimit, 16);
    normalized.sourceLookaheadFrames = qBound(1, tuning.sourceLookaheadFrames, 32);
    normalized.proxyLookaheadFrames = qBound(1, tuning.proxyLookaheadFrames, 64);
    if (m_configuredPlaybackTuning.visibleBacklogLimit == normalized.visibleBacklogLimit &&
        m_configuredPlaybackTuning.sourceLookaheadFrames == normalized.sourceLookaheadFrames &&
        m_configuredPlaybackTuning.proxyLookaheadFrames == normalized.proxyLookaheadFrames) {
        return;
    }
    m_configuredPlaybackTuning = normalized;
    applyAdaptivePlaybackTuning();
}

PreviewSurface::PlaybackTuning VulkanPreviewSurface::playbackTuning() const
{
    return m_playbackTuning;
}

int VulkanPreviewSurface::effectivePlaybackLookaheadFrames() const
{
    return m_useProxyMedia ? m_playbackTuning.proxyLookaheadFrames
                           : m_playbackTuning.sourceLookaheadFrames;
}

void VulkanPreviewSurface::invalidateTranscriptOverlayCache(const QString& clipFilePath)
{
    if (clipFilePath.trimmed().isEmpty()) {
        m_facedetectionsOverlayCache.clear();
    } else {
        m_facedetectionsOverlayCache.remove(QFileInfo(clipFilePath).absoluteFilePath());
        m_facedetectionsOverlayCache.remove(clipFilePath);
    }
    refreshFacestreamOverlays();
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
    m_interaction.hideOutsideOutputWindow = hide;
    requestNativeUpdate();
}

void VulkanPreviewSurface::setBypassGrading(bool bypass)
{
    m_bypassGrading = bypass;
    refreshVulkanFrameStatuses();
    requestNativeUpdate();
}

void VulkanPreviewSurface::setCorrectionsEnabled(bool enabled)
{
    m_correctionsEnabled = enabled;
    refreshVulkanFrameStatuses();
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
    const qreal oldZoom = m_interaction.previewZoom;
    m_interaction.previewZoom = std::clamp(zoom, 0.1, 20.0);
    if (!qFuzzyCompare(oldZoom, m_interaction.previewZoom) && m_presenter && m_presenter->widget()) {
        const QRectF surfaceRect = PreviewViewTransform::rectForWidget(
            m_presenter->widget(),
            PreviewSurfaceCoordinateSpace::DeviceSurface);
        const QRectF baseRect = PreviewViewTransform::baseRectForWidget(
            surfaceRect,
            m_interaction.outputSize,
            36.0);
        m_interaction.previewPanOffset =
            PreviewViewTransform::clampedPanOffset(baseRect, m_interaction.previewZoom, m_interaction.previewPanOffset);
    }
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
    refreshFacestreamOverlays();
    requestNativeUpdate();
}

void VulkanPreviewSurface::setShowRawDetections(bool show)
{
    if (m_showRawDetections == show) {
        return;
    }
    m_showRawDetections = show;
    refreshFacestreamOverlays();
    requestNativeUpdate();
}

void VulkanPreviewSurface::setSelectedSpeakerAssignedFaceTrackIds(const QSet<int>& trackIds)
{
    if (m_interaction.selectedSpeakerAssignedFaceTrackIds == trackIds) {
        return;
    }
    m_interaction.selectedSpeakerAssignedFaceTrackIds = trackIds;
    requestNativeUpdate();
}

void VulkanPreviewSurface::setFacestreamOverlaySource(const QString& source)
{
    m_facedetectionsOverlaySource = normalizedFacestreamOverlaySource(source);
    refreshFacestreamOverlays();
    requestNativeUpdate();
}

void VulkanPreviewSurface::setAudioSpeakerHoverModalEnabled(bool enabled)
{
    m_audioSpeakerHoverModalEnabled = enabled;
    m_interaction.audioSpeakerHoverModalEnabled = enabled;
    requestNativeUpdate();
}

void VulkanPreviewSurface::setAudioWaveformVisible(bool visible)
{
    m_audioWaveformVisible = visible;
    m_interaction.audioWaveformVisible = visible;
    requestNativeUpdate();
}

void VulkanPreviewSurface::setAudioVisualizationMode(AudioVisualizationMode mode)
{
    m_audioVisualizationMode = mode;
    m_interaction.audioVisualizationMode = mode;
    requestNativeUpdate();
}

void VulkanPreviewSurface::setLoiaconoSpectrumSettings(const LoiaconoSpectrumSettings& settings)
{
    m_loiaconoSpectrumSettings = settings;
    m_interaction.loiaconoSpectrumSettings = settings;
    requestNativeUpdate();
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
    if (m_interaction.viewMode == mode) {
        return;
    }
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
    m_interaction.audioDynamics = settings;
    requestNativeUpdate();
}

PreviewSurface::AudioDynamicsSettings VulkanPreviewSurface::audioDynamicsSettings() const
{
    return m_audioDynamics;
}

void VulkanPreviewSurface::setTranscriptOverlayInteractionEnabled(bool enabled)
{
    if (m_interaction.transcriptOverlayInteractionEnabled == enabled) {
        return;
    }
    m_interaction.transcriptOverlayInteractionEnabled = enabled;
    if (!enabled && m_interaction.transient.dragMode != PreviewDragMode::None) {
        m_interaction.transient.dragMode = PreviewDragMode::None;
        m_interaction.transient.dragOriginBounds = QRectF();
    }
    requestNativeUpdate();
}

void VulkanPreviewSurface::setTitleOverlayInteractionOnly(bool enabled)
{
    if (m_interaction.titleOverlayInteractionOnly == enabled) {
        return;
    }
    m_interaction.titleOverlayInteractionOnly = enabled;
    if (m_interaction.titleOverlayInteractionOnly) {
        bool selectedClipIsTitle = false;
        for (const TimelineClip& clip : m_interaction.clips) {
            if (clip.id == m_interaction.selectedClipId) {
                selectedClipIsTitle = clip.mediaType == ClipMediaType::Title;
                break;
            }
        }
        if (!selectedClipIsTitle) {
            m_interaction.transient.dragMode = PreviewDragMode::None;
            m_interaction.transient.dragOriginBounds = QRectF();
        }
    }
    requestNativeUpdate();
}

void VulkanPreviewSurface::setFaceDetectionsAssignmentInteractionEnabled(bool enabled)
{
    if (m_interaction.faceStreamAssignmentInteractionEnabled == enabled) {
        return;
    }
    m_interaction.faceStreamAssignmentInteractionEnabled = enabled;
    m_interaction.transient.hoveredFaceDetectionsTrackId = -1;
    m_interaction.transient.hoveredFaceDetectionsClipId.clear();
    m_interaction.transient.hoveredFaceDetectionsId.clear();
    if (enabled) {
        m_interaction.transient.dragMode = PreviewDragMode::None;
        m_interaction.transient.dragOriginBounds = QRectF();
    }
    refreshFacestreamOverlays();
    requestNativeUpdate();
}

void VulkanPreviewSurface::setCorrectionDrawMode(bool enabled)
{
    if (m_interaction.correctionDrawMode == enabled) {
        return;
    }
    m_interaction.correctionDrawMode = enabled;
    requestNativeUpdate();
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

bool VulkanPreviewSurface::faceStreamAssignmentInteractionEnabled() const
{
    return m_interaction.faceStreamAssignmentInteractionEnabled;
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

    if (editor::debugDecodePreference() != editor::DecodePreference::Auto) {
        editor::setDebugDecodePreference(editor::DecodePreference::Auto);
        m_forcedPreviewDecodePreference = true;
    }

    m_decoder = std::make_unique<editor::AsyncDecoder>();
    if (!m_decoder->initialize()) {
        return;
    }
    if (editor::MemoryBudget* budget = m_decoder->memoryBudget()) {
        budget->setMaxCpuMemory(kVulkanPreviewCpuCacheBytes);
        budget->setMaxGpuMemory(kVulkanPreviewGpuCacheBytes);
    }

    m_cache = std::make_unique<editor::TimelineCache>(m_decoder.get(), m_decoder->memoryBudget());
    m_cache->setMaxMemory(kVulkanPreviewCpuCacheBytes);
    if (editor::MemoryBudget* budget = m_decoder->memoryBudget()) {
        budget->setMaxCpuMemory(kVulkanPreviewCpuCacheBytes);
        budget->setMaxGpuMemory(kVulkanPreviewGpuCacheBytes);
    }
    m_cache->setLookaheadFrames(effectivePlaybackLookaheadFrames());
    m_cache->setPlaybackState(m_interaction.playing ? editor::TimelineCache::PlaybackState::Playing
                                                     : editor::TimelineCache::PlaybackState::Stopped);
    m_cache->setPlayheadFrame(m_interaction.currentFrame);
    m_cache->setRenderSyncMarkers(m_interaction.renderSyncMarkers);
    m_cache->setExportRanges(m_interaction.exportRanges);
    QObject::connect(m_cache.get(),
                     &editor::TimelineCache::frameLoaded,
                     m_pipelineOwner.get(),
                     [this](const QString& clipId, int64_t frame, FrameHandle) {
                         if (!loadedFrameAffectsCurrentView(clipId, frame)) {
                             return;
                         }
                         queueFrameStatusRefresh(false);
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
    QHash<QString, QString> nextRegisteredClipRegistrationKeys;
    for (const TimelineClip& clip : m_interaction.clips) {
        if (!clipVisualPlaybackEnabled(clip, m_interaction.tracks) ||
            clip.mediaType != ClipMediaType::Video ||
            clip.sourceKind == MediaSourceKind::ImageSequence ||
            clip.filePath.isEmpty()) {
            continue;
        }
        visible.insert(clip.id);
        const QString registrationKey = cacheRegistrationKeyForClip(clip);
        nextRegisteredClipRegistrationKeys.insert(clip.id, registrationKey);
        const bool alreadyRegistered = m_registeredClips.contains(clip.id);
        const bool registrationChanged =
            m_registeredClipRegistrationKeys.value(clip.id) != registrationKey;
        if (!alreadyRegistered || registrationChanged) {
            if (alreadyRegistered) {
                m_cache->unregisterClip(clip.id);
            }
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
    m_registeredClipRegistrationKeys = nextRegisteredClipRegistrationKeys;
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
        const int backlog = m_cache->pendingVisibleRequestCount();
        m_lastVisibleRequestClipId = clip.id;
        m_lastVisibleRequestFrame = localFrame;
        m_lastVisibleRequestCached = cached;
        m_lastVisibleRequestPending = pending;
        m_lastVisibleRequestForceRetry = forceRetry;
        m_lastVisibleRequestBacklog = backlog;
        ++m_visibleRequestAttempts;
        if (cached) {
            m_lastVisibleRequestDecision = QStringLiteral("skipped");
            m_lastVisibleRequestBlockReason = QStringLiteral("frame_already_cached_or_displayable");
            ++m_visibleRequestBlocked;
            continue;
        }
        if (pending && !forceRetry) {
            m_lastVisibleRequestDecision = QStringLiteral("skipped");
            m_lastVisibleRequestBlockReason = QStringLiteral("visible_request_already_pending");
            ++m_visibleRequestBlocked;
            continue;
        }
        if (backlog >= m_playbackTuning.visibleBacklogLimit) {
            m_lastVisibleRequestDecision = QStringLiteral("skipped");
            m_lastVisibleRequestBlockReason = QStringLiteral("visible_request_backlog_full");
            ++m_visibleRequestBlocked;
            continue;
        }
        m_lastVisibleRequestDecision = QStringLiteral("dispatch");
        m_lastVisibleRequestBlockReason.clear();
        ++m_visibleRequestDispatched;
        const QString requestedClipId = clip.id;
        const int64_t requestedFrame = localFrame;
        m_cache->requestFrame(clip.id, localFrame, [this, requestedClipId, requestedFrame](FrameHandle frame) {
                ++m_visibleRequestCallbacks;
                const bool nullFrame = frame.isNull();
                if (nullFrame) {
                    ++m_visibleRequestNullCallbacks;
                    m_lastVisibleRequestCallbackPayload = QStringLiteral("null");
                } else if (frame.hasHardwareFrame()) {
                    m_lastVisibleRequestCallbackPayload = QStringLiteral("hardware frame=%1").arg(frame.frameNumber());
                } else if (frame.hasGpuTexture()) {
                    m_lastVisibleRequestCallbackPayload = QStringLiteral("gpu_texture frame=%1").arg(frame.frameNumber());
                } else if (frame.hasCpuImage()) {
                    m_lastVisibleRequestCallbackPayload = QStringLiteral("cpu frame=%1").arg(frame.frameNumber());
                } else {
                    m_lastVisibleRequestCallbackPayload = QStringLiteral("unsupported_payload frame=%1").arg(frame.frameNumber());
                }
                if (!m_pipelineOwner) {
                    return;
                }
                QMetaObject::invokeMethod(
                    m_pipelineOwner.get(),
                    [this, requestedClipId, requestedFrame, nullFrame]() {
                        const bool affectsCurrent = loadedFrameAffectsCurrentView(requestedClipId, requestedFrame);
                        if (m_interaction.playing && !affectsCurrent) {
                            return;
                        }
                        queueFrameStatusRefresh(nullFrame && affectsCurrent);
                    },
                    Qt::QueuedConnection);
            });
    }
    refreshVulkanFrameStatuses();
}

bool VulkanPreviewSurface::loadedFrameAffectsCurrentView(const QString& clipId, int64_t frame) const
{
    if (clipId.trimmed().isEmpty()) {
        return false;
    }

    const int lookaheadFrames = qMax(0, effectivePlaybackLookaheadFrames());
    for (const TimelineClip& clip : m_interaction.clips) {
        if (clip.id != clipId ||
            clip.mediaType != ClipMediaType::Video ||
            clip.sourceKind == MediaSourceKind::ImageSequence ||
            clip.filePath.isEmpty()) {
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
        if (frame == localFrame) {
            return true;
        }
        if (m_interaction.playing &&
            frame > localFrame &&
            frame <= (localFrame + lookaheadFrames)) {
            return true;
        }
        return false;
    }
    return false;
}

void VulkanPreviewSurface::queueFrameStatusRefresh(bool requestVisibleFrames)
{
    if (!m_pipelineOwner) {
        return;
    }
    m_frameStatusRefreshNeedsVisibleRequest =
        m_frameStatusRefreshNeedsVisibleRequest || requestVisibleFrames;
    if (m_frameStatusRefreshQueued) {
        return;
    }
    m_frameStatusRefreshQueued = true;
    QMetaObject::invokeMethod(
        m_pipelineOwner.get(),
        [this]() {
            m_frameStatusRefreshQueued = false;
            const bool requestVisibleFrames = m_frameStatusRefreshNeedsVisibleRequest;
            m_frameStatusRefreshNeedsVisibleRequest = false;
            refreshVulkanFrameStatuses();
            if (requestVisibleFrames) {
                if (m_cache) {
                    const qint64 now = QDateTime::currentMSecsSinceEpoch();
                    if (now - m_lastFrameStatusTrimMs >= 500) {
                        m_cache->trimCache();
                        m_lastFrameStatusTrimMs = now;
                    }
                }
                requestFramesForCurrentPosition();
            }
            requestNativeUpdate();
        },
        Qt::QueuedConnection);
}

void VulkanPreviewSurface::refreshVulkanFrameStatuses()
{
    QVector<VulkanPreviewClipFrameStatus> statuses;
    int exactCount = 0;
    int approxCount = 0;
    int missingCount = 0;
    int hardwareCount = 0;
    int cpuCount = 0;
    int64_t maxFrameLag = 0;

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
        const editor::PreviewFrameSelectionResult frameSelection = editor::selectPreviewFrame(
            editor::PreviewFrameSelectionRequest{
                clip.id,
                localFrame,
                m_interaction.playing,
                false,
                false,
                true,
                true,
                false,
                -1,
            },
            m_cache.get(),
            nullptr);
        const FrameHandle exactFrame = frameSelection.exactFrame;
        FrameHandle selectedFrame = frameSelection.frame;

        VulkanPreviewClipFrameStatus status;
        status.clipId = clip.id;
        status.label = clip.label;
        status.decodePath = QStringLiteral("missing");
        status.requestedSourceFrame = localFrame;
        status.active = true;
        status.effectsPath = QStringLiteral("evaluateEffectiveVisualEffectsAtPosition");
        status.gradingBypassed = m_bypassGrading;
        status.correctionsEnabled = m_correctionsEnabled;
        status.transform = evaluateClipRenderTransformAtPosition(
            clip, m_interaction.currentFramePosition, m_interaction.outputSize);
        EffectiveVisualEffects effects = evaluateEffectiveVisualEffectsAtPosition(
            clip, m_interaction.tracks, m_interaction.currentFramePosition, m_interaction.renderSyncMarkers);
        if (m_bypassGrading) {
            effects.grading = TimelineClip::GradingKeyframe{};
        }
        if (!m_correctionsEnabled) {
            effects.correctionPolygons.clear();
        }
        status.grading = effects.grading;
        status.maskFeather = effects.maskFeather;
        status.maskFeatherGamma = effects.maskFeatherGamma;
        status.correctionPolygonCount = effects.correctionPolygons.size();
        status.correctionsApplied = false;
        status.correctionsSupported = false;
        status.curveLutApplied = gradingUsesCurveLut(effects.grading);
        status.curveLutSupported = true;
        status.gradingShaderActive = true;
        if (effects.grading.opacity <= 0.0001) {
            status.drawSuppressed = true;
            status.missingReason = QStringLiteral("skipped_zero_opacity");
        }
        if (status.correctionPolygonCount > 0 && !selectedFrame.isNull() && selectedFrame.hasCpuImage()) {
            const QImage masked = applyCorrectionMasksToCpuImage(selectedFrame.cpuImage(), effects.correctionPolygons);
            if (!masked.isNull()) {
                selectedFrame = FrameHandle::createCpuFrame(
                    masked,
                    selectedFrame.frameNumber(),
                    selectedFrame.sourcePath());
                status.correctionsApplied = true;
                status.correctionsSupported = true;
            }
        } else if (status.correctionPolygonCount > 0) {
            status.missingReason = QStringLiteral("vulkan_correction_masks_require_cpu_upload_frame");
        }
        const bool selectedHasHardwareFrame = !selectedFrame.isNull() && selectedFrame.hasHardwareFrame();
        const bool selectedHasGpuTexture = !selectedFrame.isNull() && selectedFrame.hasGpuTexture();
        const bool selectedHasCpuFrame = !selectedFrame.isNull() && selectedFrame.hasCpuImage();
        status.exactFrameAvailable = !exactFrame.isNull();
        status.selectedFrameAvailable = !selectedFrame.isNull();
        status.hasFrame = !selectedFrame.isNull() &&
                          (selectedHasHardwareFrame || selectedHasGpuTexture || selectedHasCpuFrame);
        status.exact = status.hasFrame && !exactFrame.isNull() && selectedFrame == exactFrame;
        if (status.hasFrame) {
            status.frame = selectedFrame;
            status.presentedSourceFrame = selectedFrame.frameNumber();
            status.frameSize = selectedFrame.size();
            status.hardwareFrame = selectedHasHardwareFrame;
            status.gpuTexture = selectedHasGpuTexture;
            status.cpuImage = selectedHasCpuFrame && !selectedHasHardwareFrame && !selectedHasGpuTexture;
            if (status.gpuTexture) {
                status.decodePath = QStringLiteral("gpu_texture");
            } else if (status.hardwareFrame) {
                status.decodePath = QStringLiteral("hardware_frame");
            } else if (status.cpuImage) {
                status.decodePath = QStringLiteral("cpu_upload");
            } else {
                status.decodePath = QStringLiteral("unsupported_payload");
            }
            maxFrameLag = qMax(maxFrameLag, qAbs(status.requestedSourceFrame - status.presentedSourceFrame));
            exactCount += status.exact ? 1 : 0;
            approxCount += status.exact ? 0 : 1;
            hardwareCount += status.hardwareFrame ? 1 : 0;
            cpuCount += status.cpuImage ? 1 : 0;
        } else {
            if (!m_cache) {
                status.missingReason = QStringLiteral("cache_unavailable");
            } else if (selectedFrame.isNull()) {
                status.missingReason = QStringLiteral("no_decoded_frame_for_active_clip");
            } else if (!selectedHasHardwareFrame && !selectedHasGpuTexture && !selectedHasCpuFrame) {
                status.missingReason = QStringLiteral("decoded_frame_has_no_supported_payload");
            } else {
                status.missingReason = QStringLiteral("decoded_frame_rejected");
            }
            status.decodePath = QStringLiteral("missing");
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
    recordPlaybackSmoothnessSample(exactCount, approxCount, missingCount, maxFrameLag);
    refreshFacestreamOverlays();
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
        if (!m_cache->isVisibleRequestPending(clip.id, localFrame) &&
            m_cache->pendingVisibleRequestCount() < m_playbackTuning.visibleBacklogLimit) {
            m_cache->requestFrame(clip.id, localFrame, [this](FrameHandle) {
                queueFrameStatusRefresh(false);
            });
        }
    }
    refreshVulkanFrameStatuses();
    return ready;
}

bool VulkanPreviewSurface::hasPlaybackLookaheadBuffered(int futureFrames) const
{
    if (futureFrames < 0 || !m_cache) {
        return true;
    }

    for (int offset = 0; offset <= futureFrames; ++offset) {
        const int64_t samplePosition = m_interaction.currentSample + frameToSamples(offset);
        const qreal framePosition = samplesToFramePosition(samplePosition);
        for (const TimelineClip& clip : m_interaction.clips) {
            if (clip.mediaType != ClipMediaType::Video ||
                clip.sourceKind == MediaSourceKind::ImageSequence ||
                clip.filePath.isEmpty()) {
                continue;
            }
            if (!visualClipActiveAtSample(clip,
                                          m_interaction.tracks,
                                          samplePosition,
                                          framePosition,
                                          m_bypassGrading)) {
                continue;
            }
            const int64_t localFrame = sourceFrameForSample(clip, samplePosition);
            if (!m_cache->hasDisplayableFrameForPreview(clip.id, localFrame, true, true)) {
                return false;
            }
        }
    }

    return true;
}

bool VulkanPreviewSurface::warmPlaybackLookahead(int futureFrames, int timeoutMs)
{
    if (futureFrames <= 0) {
        return true;
    }
    ensureFramePipeline();
    if (!m_cache) {
        return false;
    }
    const int cappedFutureFrames = std::min(futureFrames, effectivePlaybackLookaheadFrames());
    QElapsedTimer timer;
    timer.start();
    while (timer.elapsed() < timeoutMs) {
        if (hasPlaybackLookaheadBuffered(cappedFutureFrames)) {
            return true;
        }
        for (int offset = 0; offset <= cappedFutureFrames; ++offset) {
            preparePlaybackAdvanceSample(m_interaction.currentSample + frameToSamples(offset));
        }
        QCoreApplication::processEvents(QEventLoop::AllEvents, 8);
        QThread::msleep(8);
    }
    return hasPlaybackLookaheadBuffered(cappedFutureFrames);
}

QImage VulkanPreviewSurface::latestPresentedFrameImageForClip(const QString& clipId) const
{
    Q_UNUSED(clipId);
    return QImage();
}

QVector<PreviewSurface::PipelineStageSnapshot> VulkanPreviewSurface::livePipelineSnapshots() const
{
    QVector<PipelineStageSnapshot> snapshots;
    const QImage previewImage;
    const QImage decoderDiagnosticImage;
    auto decoderStageImage = [](const VulkanPreviewClipFrameStatus& status) {
        return QImage();
    };
    auto addStage = [&snapshots](const QString& label,
                                 const QString& detail,
                                 const QImage& image,
                                 const QString& kind,
                                 bool exact,
                                 bool active,
                                 const QString& state = QString(),
                                 const QJsonObject& facts = QJsonObject{}) {
        snapshots.push_back(PipelineStageSnapshot{label, detail, image, kind, exact, active, state, facts});
    };

    addStage(QStringLiteral("00 Preview State"),
             QStringLiteral("%1 | timeline frame %2 | sample %3 | clips %4")
                 .arg(backendName())
                 .arg(m_interaction.currentFrame)
                 .arg(m_interaction.currentSample)
                 .arg(m_interaction.clipCount),
             QImage(),
             QStringLiteral("surface"),
             true,
             true);

    const QJsonObject presenterSnapshot = m_presenter ? m_presenter->profilingSnapshot() : QJsonObject{};
    const int cachePendingVisible = m_cache ? m_cache->pendingVisibleRequestCount() : 0;
    const qint64 textureDraws =
        static_cast<qint64>(presenterSnapshot.value(QStringLiteral("texture_draw_count")).toDouble());
    const qint64 fallbackDraws =
        static_cast<qint64>(presenterSnapshot.value(QStringLiteral("clear_fallback_draw_count")).toDouble());
    const qint64 explicitFailureDraws =
        static_cast<qint64>(presenterSnapshot.value(QStringLiteral("explicit_failure_draw_count")).toDouble());
    const qint64 handoffAttempts =
        static_cast<qint64>(presenterSnapshot.value(QStringLiteral("handoff_attempts")).toDouble());
    const qint64 handoffSuccesses =
        static_cast<qint64>(presenterSnapshot.value(QStringLiteral("handoff_successes")).toDouble());
    const qint64 sampledImages =
        static_cast<qint64>(presenterSnapshot.value(QStringLiteral("sampled_image_ready_count")).toDouble());

    for (const VulkanPreviewClipFrameStatus& status : m_interaction.vulkanFrameStatuses) {
        const TimelineClip* clip = nullptr;
        for (const TimelineClip& candidate : m_interaction.clips) {
            if (candidate.id == status.clipId) {
                clip = &candidate;
                break;
            }
        }
        const QString clipLabel = status.label.isEmpty() ? status.clipId : status.label;
        const qint64 frameLag = status.hasFrame
                                    ? qMax<qint64>(0, status.requestedSourceFrame - status.presentedSourceFrame)
                                    : -1;
        const QString exactState = status.exact
                                       ? QStringLiteral("ready")
                                       : (status.hasFrame ? QStringLiteral("approximate")
                                                          : QStringLiteral("missing"));
        addStage(QStringLiteral("01 Timeline Input"),
                 clip ? QStringLiteral("%1 | %2 | %3 | timeline %4-%5")
                            .arg(clipLabel,
                                 clipMediaTypeLabel(clip->mediaType),
                                 mediaSourceKindLabel(clip->sourceKind))
                            .arg(clip->startFrame)
                            .arg(clip->startFrame + qMax<int64_t>(0, clip->durationFrames - 1))
                      : QStringLiteral("%1 | active status").arg(clipLabel),
                 QImage(),
                 QStringLiteral("timeline"),
                 true,
                 status.active,
                 status.active ? QStringLiteral("ready") : QStringLiteral("waiting"),
                 QJsonObject{
                     {QStringLiteral("clip_id"), status.clipId},
                     {QStringLiteral("active"), status.active}
                 });
        addStage(QStringLiteral("02 Source Mapping"),
                 clip ? QStringLiteral("sample %1 -> source frame %2 | source in %3 | sync markers %4")
                            .arg(m_interaction.currentSample)
                            .arg(status.requestedSourceFrame)
                            .arg(clip->sourceInFrame)
                            .arg(m_interaction.renderSyncMarkers.size())
                      : QStringLiteral("source frame %1").arg(status.requestedSourceFrame),
                 QImage(),
                 QStringLiteral("mapping"),
                 true,
                 status.active,
                 QStringLiteral("ready"),
                 QJsonObject{
                     {QStringLiteral("requested_source_frame"), static_cast<qint64>(status.requestedSourceFrame)},
                     {QStringLiteral("current_sample"), static_cast<qint64>(m_interaction.currentSample)},
                     {QStringLiteral("sync_marker_count"), m_interaction.renderSyncMarkers.size()}
                 });
        addStage(QStringLiteral("03 Cache Lookup"),
                 QStringLiteral("exact frame %1 | selected frame %2 | pending visible %3")
                     .arg(status.exactFrameAvailable ? QStringLiteral("yes") : QStringLiteral("no"))
                     .arg(status.selectedFrameAvailable ? QStringLiteral("yes") : QStringLiteral("no"))
                     .arg(cachePendingVisible),
                 QImage(),
                 QStringLiteral("selection"),
                 status.exact,
                 status.active,
                 status.exactFrameAvailable ? QStringLiteral("ready")
                                            : (status.selectedFrameAvailable ? QStringLiteral("approximate")
                                                                             : QStringLiteral("blocked")),
                 QJsonObject{
                     {QStringLiteral("exact_frame_available"), status.exactFrameAvailable},
                     {QStringLiteral("selected_frame_available"), status.selectedFrameAvailable},
                     {QStringLiteral("pending_visible_requests"), cachePendingVisible},
                     {QStringLiteral("frame_lag"), frameLag}
                 });
        addStage(QStringLiteral("04 Decoder Output"),
                 QStringLiteral("presented frame %1 | %2 | size %3x%4 | exact frame %5")
                     .arg(status.presentedSourceFrame)
                     .arg(status.decodePath)
                     .arg(status.frameSize.width())
                     .arg(status.frameSize.height())
                     .arg(status.exactFrameAvailable ? QStringLiteral("yes") : QStringLiteral("no")),
                 decoderStageImage(status),
                 QStringLiteral("decoder"),
                 status.exact,
                 status.hasFrame,
                 exactState,
                 QJsonObject{
                     {QStringLiteral("decode_path"), status.decodePath},
                     {QStringLiteral("requested_source_frame"), static_cast<qint64>(status.requestedSourceFrame)},
                     {QStringLiteral("presented_source_frame"), static_cast<qint64>(status.presentedSourceFrame)},
                     {QStringLiteral("frame_lag"), frameLag},
                     {QStringLiteral("hardware_frame"), status.hardwareFrame},
                     {QStringLiteral("gpu_texture"), status.gpuTexture},
                     {QStringLiteral("cpu_image"), status.cpuImage},
                     {QStringLiteral("thumbnail_source"), status.frame.hasCpuImage()
                          ? QStringLiteral("cpu_frame")
                          : (decoderDiagnosticImage.isNull()
                                 ? QStringLiteral("pending_decoder_gpu_readback")
                                 : QStringLiteral("decoder_gpu_readback"))}
                 });
        addStage(QStringLiteral("05 Hardware Handoff"),
                 QStringLiteral("mode %1 | upload %2 ms | probe %3 | error %4")
                     .arg(presenterSnapshot.value(QStringLiteral("last_handoff_mode")).toString(status.decodePath))
                     .arg(presenterSnapshot.value(QStringLiteral("last_handoff_upload_ms")).toDouble())
                     .arg(presenterSnapshot.value(QStringLiteral("last_handoff_probe_path")).toString())
                     .arg(presenterSnapshot.value(QStringLiteral("last_handoff_error")).toString(QStringLiteral("none"))),
                 decoderStageImage(status),
                 QStringLiteral("decoder"),
                 status.exact,
                 status.hasFrame,
                 presenterSnapshot.value(QStringLiteral("last_handoff_error")).toString().isEmpty()
                     ? (handoffSuccesses > 0 ? QStringLiteral("ready") : QStringLiteral("waiting"))
                     : QStringLiteral("error"),
                 QJsonObject{
                     {QStringLiteral("handoff_attempted"), handoffAttempts > 0},
                     {QStringLiteral("handoff_attempts"), handoffAttempts},
                     {QStringLiteral("handoff_successes"), handoffSuccesses},
                     {QStringLiteral("handoff_failures"), static_cast<qint64>(presenterSnapshot.value(QStringLiteral("handoff_failures")).toDouble())},
                     {QStringLiteral("last_handoff_mode"), presenterSnapshot.value(QStringLiteral("last_handoff_mode")).toString()},
                     {QStringLiteral("last_handoff_error"), presenterSnapshot.value(QStringLiteral("last_handoff_error")).toString()},
                     {QStringLiteral("last_handoff_probe_path"), presenterSnapshot.value(QStringLiteral("last_handoff_probe_path")).toString()},
                     {QStringLiteral("sampled_frame_ready"), sampledImages > 0},
                     {QStringLiteral("thumbnail_source"), status.frame.hasCpuImage()
                          ? QStringLiteral("cpu_frame")
                          : (decoderDiagnosticImage.isNull()
                                 ? QStringLiteral("pending_decoder_gpu_readback")
                                 : QStringLiteral("decoder_gpu_readback"))}
                 });
        addStage(QStringLiteral("06 Frame Selection"),
                 QStringLiteral("selected %1 | exact %2 | missing %3")
                     .arg(status.selectedFrameAvailable ? QStringLiteral("yes") : QStringLiteral("no"))
                     .arg(status.exact ? QStringLiteral("yes") : QStringLiteral("no"))
                     .arg(status.missingReason.isEmpty() ? QStringLiteral("no") : status.missingReason),
                 decoderStageImage(status),
                 QStringLiteral("selection"),
                 status.exact,
                 status.hasFrame,
                 exactState,
                 QJsonObject{
                     {QStringLiteral("selected"), status.selectedFrameAvailable},
                     {QStringLiteral("exact"), status.exact},
                     {QStringLiteral("missing_reason"), status.missingReason},
                     {QStringLiteral("draw_suppressed"), status.drawSuppressed},
                     {QStringLiteral("thumbnail_source"), status.frame.hasCpuImage()
                          ? QStringLiteral("cpu_frame")
                          : (decoderDiagnosticImage.isNull()
                                 ? QStringLiteral("pending_decoder_gpu_readback")
                                 : QStringLiteral("decoder_gpu_readback"))}
                 });
        addStage(QStringLiteral("07 Corrections / Mask"),
                 QStringLiteral("polygons %1 | applied %2 | supported %3 | feather %4 gamma %5")
                     .arg(status.correctionPolygonCount)
                     .arg(status.correctionsApplied ? QStringLiteral("yes") : QStringLiteral("no"))
                     .arg(status.correctionsSupported ? QStringLiteral("yes") : QStringLiteral("no"))
                     .arg(status.maskFeather)
                     .arg(status.maskFeatherGamma),
                 status.correctionsApplied ? decoderStageImage(status) : QImage(),
                 QStringLiteral("mask"),
                 status.exact,
                 status.hasFrame,
                 status.correctionPolygonCount == 0 || status.correctionsSupported ? QStringLiteral("ready")
                                                                                   : QStringLiteral("blocked"),
                 QJsonObject{
                     {QStringLiteral("correction_polygon_count"), status.correctionPolygonCount},
                     {QStringLiteral("corrections_applied"), status.correctionsApplied},
                     {QStringLiteral("corrections_supported"), status.correctionsSupported},
                     {QStringLiteral("thumbnail_source"), status.correctionsApplied
                          ? (status.frame.hasCpuImage()
                                 ? QStringLiteral("cpu_frame")
                                 : (decoderDiagnosticImage.isNull()
                                        ? QStringLiteral("pending_decoder_gpu_readback")
                                        : QStringLiteral("decoder_gpu_readback")))
                          : QStringLiteral("not_applicable")}
                 });
        addStage(QStringLiteral("08 Effects Evaluation"),
                 QStringLiteral("%1 | opacity %2 | bypass grading %3 | curve LUT %4")
                     .arg(status.effectsPath)
                     .arg(status.grading.opacity)
                     .arg(status.gradingBypassed ? QStringLiteral("yes") : QStringLiteral("no"))
                     .arg(status.curveLutApplied ? QStringLiteral("yes") : QStringLiteral("no")),
                 previewImage,
                 QStringLiteral("effects"),
                 true,
                 status.active,
                 QStringLiteral("ready"),
                 QJsonObject{
                     {QStringLiteral("effects_path"), status.effectsPath},
                     {QStringLiteral("opacity"), status.grading.opacity},
                     {QStringLiteral("grading_bypassed"), status.gradingBypassed},
                     {QStringLiteral("curve_lut_applied"), status.curveLutApplied},
                     {QStringLiteral("thumbnail_source"), previewImage.isNull()
                          ? QStringLiteral("pending_gpu_diagnostic_readback")
                          : QStringLiteral("gpu_diagnostic_readback")}
                 });
        addStage(QStringLiteral("09 Grading Shader Output"),
                 QStringLiteral("active %1 | brightness %2 | contrast %3 | saturation %4 | unsupported %5")
                     .arg(status.gradingShaderActive ? QStringLiteral("yes") : QStringLiteral("no"))
                     .arg(status.grading.brightness)
                     .arg(status.grading.contrast)
                     .arg(status.grading.saturation)
                     .arg(presenterSnapshot.value(QStringLiteral("last_unsupported_effect")).toString(QStringLiteral("none"))),
                 previewImage,
                 QStringLiteral("shader"),
                 status.exact,
                 status.hasFrame,
                 presenterSnapshot.value(QStringLiteral("last_unsupported_effect")).toString().isEmpty()
                     ? QStringLiteral("ready")
                     : QStringLiteral("blocked"),
                 QJsonObject{
                     {QStringLiteral("grading_shader_active"), status.gradingShaderActive},
                     {QStringLiteral("last_unsupported_effect"), presenterSnapshot.value(QStringLiteral("last_unsupported_effect")).toString()},
                     {QStringLiteral("thumbnail_source"), previewImage.isNull()
                          ? QStringLiteral("pending_gpu_diagnostic_readback")
                          : QStringLiteral("gpu_diagnostic_readback")}
                 });
        addStage(QStringLiteral("10 Transform"),
                 QStringLiteral("translate %1,%2 | scale %3,%4 | rotate %5 | target %6")
                     .arg(status.transform.translationX)
                     .arg(status.transform.translationY)
                     .arg(status.transform.scaleX)
                     .arg(status.transform.scaleY)
                     .arg(status.transform.rotation)
                     .arg(presenterSnapshot.value(QStringLiteral("last_target_rect")).toString()),
                 previewImage,
                 QStringLiteral("transform"),
                 true,
                 status.active,
                 QStringLiteral("ready"),
                 QJsonObject{
                     {QStringLiteral("target_rect"), presenterSnapshot.value(QStringLiteral("last_target_rect")).toString()},
                     {QStringLiteral("fitted_rect"), presenterSnapshot.value(QStringLiteral("last_fitted_rect")).toString()},
                     {QStringLiteral("translation_x"), status.transform.translationX},
                     {QStringLiteral("translation_y"), status.transform.translationY},
                     {QStringLiteral("scale_x"), status.transform.scaleX},
                     {QStringLiteral("scale_y"), status.transform.scaleY},
                     {QStringLiteral("rotation"), status.transform.rotation},
                     {QStringLiteral("thumbnail_source"), previewImage.isNull()
                          ? QStringLiteral("pending_gpu_diagnostic_readback")
                          : QStringLiteral("gpu_diagnostic_readback")}
                 });
        addStage(QStringLiteral("11 Vulkan Composite"),
                 QStringLiteral("handoff %1 | upload %2 ms | texture draws %3 | fallback draws %4")
                     .arg(presenterSnapshot.value(QStringLiteral("last_handoff_mode")).toString(status.decodePath))
                     .arg(presenterSnapshot.value(QStringLiteral("last_handoff_upload_ms")).toDouble())
                     .arg(textureDraws)
                     .arg(fallbackDraws),
                 previewImage,
                 QStringLiteral("composite"),
                 status.exact,
                 status.hasFrame,
                 explicitFailureDraws > 0 ? QStringLiteral("fallback")
                                          : (textureDraws > 0 ? QStringLiteral("ready") : QStringLiteral("blocked")),
                 QJsonObject{
                     {QStringLiteral("sampled_frame_ready"), sampledImages > 0},
                     {QStringLiteral("texture_draw_count"), textureDraws},
                     {QStringLiteral("active_clip_draw_count"), static_cast<qint64>(presenterSnapshot.value(QStringLiteral("active_clip_draw_count")).toDouble())},
                     {QStringLiteral("fallback_draw_count"), fallbackDraws},
                     {QStringLiteral("explicit_failure_draw_count"), explicitFailureDraws},
                     {QStringLiteral("timeline_texture_draw_pipeline"), presenterSnapshot.value(QStringLiteral("timeline_texture_draw_pipeline")).toBool()},
                     {QStringLiteral("frame_lag"), frameLag},
                     {QStringLiteral("thumbnail_source"), previewImage.isNull()
                          ? QStringLiteral("pending_gpu_diagnostic_readback")
                          : QStringLiteral("gpu_diagnostic_readback")}
                 });
        addStage(QStringLiteral("12 FaceDetections Overlay"),
                 QStringLiteral("active overlays %1 | source frame %2")
                     .arg(m_interaction.facedetectionsOverlays.size())
                     .arg(status.presentedSourceFrame),
                 previewImage,
                 QStringLiteral("composite"),
                 true,
                 !m_interaction.facedetectionsOverlays.isEmpty());
    }

    if (snapshots.size() == 1 && !m_interaction.selectedClipId.isEmpty()) {
        const QImage selectedImage;
        snapshots.push_back(PipelineStageSnapshot{
            QStringLiteral("Selected Clip"),
            QStringLiteral("Vulkan path does not materialize CPU preview images"),
            selectedImage,
            QStringLiteral("decoder"),
            false,
            false});
    }

    addStage(QStringLiteral("13 Swapchain Readback"),
             QStringLiteral("direct VkImage copy | image %1x%2 | sampled images %3 | failures %4")
                 .arg(previewImage.width())
                 .arg(previewImage.height())
                 .arg(static_cast<qint64>(presenterSnapshot.value(QStringLiteral("sampled_image_ready_count")).toDouble()))
                 .arg(static_cast<qint64>(presenterSnapshot.value(QStringLiteral("handoff_failures")).toDouble())),
             previewImage,
             QStringLiteral("surface"),
             true,
             !previewImage.isNull());

    addStage(QStringLiteral("14 Presented Surface"),
             QStringLiteral("swapchain presenter | final visible frame"),
             previewImage,
             QStringLiteral("surface"),
             true,
             !previewImage.isNull());

    return snapshots;
}
