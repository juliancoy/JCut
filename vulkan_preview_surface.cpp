#include "vulkan_preview_surface.h"
#include "facestream_artifact_utils.h"
#include "facestream_runtime.h"
#include "facestream_time_mapping.h"

#include "async_decoder.h"
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

bool syncAudioPreviewPanToPlayhead(PreviewInteractionState* state)
{
    if (!state ||
        !state->playing ||
        state->viewMode != PreviewSurface::ViewMode::Audio) {
        return false;
    }

    const TimelineClip* audioClip = nullptr;
    for (const TimelineClip& clip : state->clips) {
        const int64_t clipStartSample = clipTimelineStartSamples(clip);
        const int64_t clipEndSample = clipStartSample + frameToSamples(clip.durationFrames);
        const bool withinClip = state->currentSample >= clipStartSample && state->currentSample < clipEndSample;
        const bool includeForAudioView =
            clipAudioPlaybackEnabled(clip) &&
            (clip.id == state->selectedClipId || withinClip);
        const bool includeAsFallback = clipIsAudioOnly(clip) && withinClip;
        if (includeForAudioView || includeAsFallback) {
            audioClip = &clip;
            break;
        }
    }
    if (!audioClip) {
        return false;
    }

    const int64_t clipStartSample = clipTimelineStartSamples(*audioClip);
    const int64_t clipSamples = qMax<int64_t>(1, frameToSamples(qMax<int64_t>(1, audioClip->durationFrames)));
    const qreal zoom = qBound<qreal>(1.0, state->previewZoom, 100000.0);
    const qreal visibleFraction = qBound<qreal>(0.00001, 1.0 / zoom, 1.0);
    const qreal maxStart = qMax<qreal>(0.0, 1.0 - visibleFraction);
    const qreal startNorm = qBound<qreal>(0.0, state->previewPanOffset.x(), maxStart);
    const qreal playheadNorm = qBound<qreal>(
        0.0,
        static_cast<qreal>(state->currentSample - clipStartSample) / static_cast<qreal>(clipSamples),
        1.0);

    const qreal leftGuard = startNorm + visibleFraction * 0.15;
    const qreal rightGuard = startNorm + visibleFraction * 0.85;
    qreal newStart = startNorm;
    if (playheadNorm < leftGuard) {
        newStart = playheadNorm - visibleFraction * 0.15;
    } else if (playheadNorm > rightGuard) {
        newStart = playheadNorm - visibleFraction * 0.85;
    } else {
        return false;
    }

    newStart = qBound<qreal>(0.0, newStart, maxStart);
    if (qAbs(newStart - startNorm) < 0.000001) {
        return false;
    }
    state->previewPanOffset.setX(newStart);
    return true;
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

bool clipSupportsDrawableTranscriptOverlayForSelection(const TimelineClip& clip,
                                                     int64_t currentSample,
                                                     const QVector<RenderSyncMarker>& renderSyncMarkers)
{
    if (!((clip.mediaType == ClipMediaType::Audio) || clip.hasAudio) || !clip.transcriptOverlay.enabled) {
        return false;
    }
    const QString transcriptPath = activeTranscriptPathForClipFile(clip.filePath);
    const std::shared_ptr<const TranscriptRuntimeDocument> runtimeDocument =
        loadTranscriptRuntimeDocument(transcriptPath);
    const QVector<TranscriptSection>& sections =
        runtimeDocument ? runtimeDocument->sections : QVector<TranscriptSection>{};
    const int64_t sourceFrame = transcriptFrameForClipAtTimelineSample(clip, currentSample, renderSyncMarkers);
    const TranscriptOverlayLayout layout = transcriptOverlayLayoutAtSourceFrame(clip, sections, sourceFrame);
    return !layout.lines.isEmpty();
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
    if (m_playbackTuning.visibleBacklogLimit == normalized.visibleBacklogLimit &&
        m_playbackTuning.sourceLookaheadFrames == normalized.sourceLookaheadFrames &&
        m_playbackTuning.proxyLookaheadFrames == normalized.proxyLookaheadFrames) {
        return;
    }
    m_playbackTuning = normalized;
    if (m_cache) {
        m_cache->setLookaheadFrames(effectivePlaybackLookaheadFrames());
    }
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
        m_facestreamOverlayCache.clear();
    } else {
        m_facestreamOverlayCache.remove(QFileInfo(clipFilePath).absoluteFilePath());
        m_facestreamOverlayCache.remove(clipFilePath);
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

void VulkanPreviewSurface::setFacestreamOverlaySource(const QString& source)
{
    m_facestreamOverlaySource = source.trimmed().isEmpty() ? QStringLiteral("all") : source.trimmed().toLower();
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

void VulkanPreviewSurface::setFaceStreamAssignmentInteractionEnabled(bool enabled)
{
    if (m_interaction.faceStreamAssignmentInteractionEnabled == enabled) {
        return;
    }
    m_interaction.faceStreamAssignmentInteractionEnabled = enabled;
    m_interaction.transient.hoveredFaceStreamTrackId = -1;
    m_interaction.transient.hoveredFaceStreamClipId.clear();
    m_interaction.transient.hoveredFaceStreamId.clear();
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
        m_cache->requestFrame(clip.id, localFrame, [this](FrameHandle frame) {
                ++m_visibleRequestCallbacks;
                if (frame.isNull()) {
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
                    [this]() {
                        refreshVulkanFrameStatuses();
                        if (m_cache) {
                            m_cache->trimCache();
                        }
                        requestFramesForCurrentPosition();
                        requestNativeUpdate();
                    },
                    Qt::QueuedConnection);
            });
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

QVector<VulkanPreviewSurface::FacestreamTrack> VulkanPreviewSurface::parseContinuityTracksForClip(
    const TimelineClip& clip,
    const QJsonObject& artifactRoot) const
{
    QVector<FacestreamTrack> tracks;
    const QJsonObject byClip = artifactRoot.value(QStringLiteral("continuity_facestreams_by_clip")).toObject();
    const QJsonObject continuityRoot = byClip.value(clip.id.trimmed()).toObject();
    const QJsonArray streams = jcut::facestream::continuityStreamsForRoot(continuityRoot);
    tracks.reserve(streams.size());
    for (const QJsonValue& streamValue : streams) {
        const QJsonObject streamObj = streamValue.toObject();
        FacestreamTrack track;
        track.streamId = streamObj.value(QStringLiteral("stream_id")).toString().trimmed();
        track.trackId = streamObj.value(QStringLiteral("track_id")).toInt(-1);
        track.source = streamObj.value(QStringLiteral("source")).toString().trimmed().toLower();
        const QJsonArray keyframes = streamObj.value(QStringLiteral("keyframes")).toArray();
        track.keyframes.reserve(keyframes.size());
        int64_t streamFrameMin = std::numeric_limits<int64_t>::max();
        int64_t streamFrameMax = -1;
        for (const QJsonValue& keyframeValue : keyframes) {
            const QJsonObject keyframeObj = keyframeValue.toObject();
            if (!keyframeObj.contains(QStringLiteral("frame"))) {
                continue;
            }
            const qreal boxSize = qBound<qreal>(
                0.001, keyframeObj.value(QStringLiteral("box_size")).toDouble(-1.0), 1.0);
            const qreal x = qBound<qreal>(0.0, keyframeObj.value(QStringLiteral("x")).toDouble(0.5), 1.0);
            const qreal y = qBound<qreal>(0.0, keyframeObj.value(QStringLiteral("y")).toDouble(0.5), 1.0);
            FacestreamKeyframe keyframe;
            keyframe.frame = keyframeObj.value(QStringLiteral("frame")).toVariant().toLongLong();
            if (keyframe.frame >= 0) {
                streamFrameMin = qMin<int64_t>(streamFrameMin, keyframe.frame);
                streamFrameMax = qMax<int64_t>(streamFrameMax, keyframe.frame);
            }
            keyframe.xNorm = x;
            keyframe.yNorm = y;
            keyframe.boxSizeNorm = boxSize;
            keyframe.hasCenterBox = keyframeObj.contains(QStringLiteral("x")) &&
                                    keyframeObj.contains(QStringLiteral("y")) &&
                                    keyframeObj.contains(QStringLiteral("box_size"));
            if (!keyframe.hasCenterBox) {
                keyframe.boxNorm = QRectF(qBound<qreal>(0.0, x - (boxSize * 0.5), 1.0),
                                          qBound<qreal>(0.0, y - (boxSize * 0.5), 1.0),
                                          boxSize,
                                          boxSize).intersected(QRectF(0.0, 0.0, 1.0, 1.0));
            }
            keyframe.confidence = qBound<qreal>(
                0.0, keyframeObj.value(QStringLiteral("confidence")).toDouble(1.0), 1.0);
            keyframe.source = keyframeObj.value(QStringLiteral("source")).toString(track.source).trimmed().toLower();
            if (track.source.isEmpty()) {
                track.source = keyframe.source;
            }
            if (keyframe.hasCenterBox || keyframe.boxNorm.isValid()) {
                track.keyframes.push_back(keyframe);
            }
        }
        track.frameDomain = inferFacestreamFrameDomain(clip, streamFrameMin, streamFrameMax);
        std::sort(track.keyframes.begin(), track.keyframes.end(), [](const FacestreamKeyframe& a, const FacestreamKeyframe& b) {
            return a.frame < b.frame;
        });
        if (!track.keyframes.isEmpty()) {
            tracks.push_back(track);
        }
    }
    return tracks;
}

QVector<VulkanPreviewSurface::FacestreamTrack> VulkanPreviewSurface::loadFacestreamTracksForClip(
    const TimelineClip& clip)
{
    const QString clipPath = QFileInfo(clip.filePath).absoluteFilePath();
    const QString transcriptPath = activeTranscriptPathForClipFile(clip.filePath);
    const QFileInfo transcriptInfo(transcriptPath);
    const qint64 artifactRevisionMs =
        facestreamArtifactRevisionMsForTranscript(transcriptInfo.absoluteFilePath());
    const QString signature = QStringLiteral("%1|%2|%3|%4|%5")
                                  .arg(clip.id)
                                  .arg(transcriptInfo.absoluteFilePath())
                                  .arg(transcriptInfo.exists() ? transcriptInfo.lastModified().toMSecsSinceEpoch() : 0)
                                  .arg(artifactRevisionMs)
                                  .arg(m_facestreamOverlaySource);
    FacestreamOverlayCacheEntry& entry = m_facestreamOverlayCache[clipPath];
    if (entry.signature == signature) {
        return entry.tracks;
    }

    entry = FacestreamOverlayCacheEntry{};
    entry.signature = signature;
    if (!transcriptInfo.exists() || !transcriptInfo.isFile()) {
        return entry.tracks;
    }

    editor::TranscriptEngine engine;
    QJsonObject artifactRoot;
    if (engine.loadFacestreamArtifact(transcriptPath, &artifactRoot)) {
        entry.tracks += parseContinuityTracksForClip(clip, artifactRoot);
    }
    return entry.tracks;
}

void VulkanPreviewSurface::refreshFacestreamOverlays()
{
    QVector<VulkanPreviewFacestreamOverlay> overlays;
    if (!m_showSpeakerTrackBoxes && !m_interaction.faceStreamAssignmentInteractionEnabled) {
        m_interaction.facestreamOverlays = overlays;
        return;
    }
    const QString sourceFilter = m_facestreamOverlaySource.trimmed().isEmpty()
        ? QStringLiteral("all")
        : m_facestreamOverlaySource.trimmed().toLower();
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
        const int64_t localSourceFrame =
            qMax<int64_t>(0, localFrame - qMax<int64_t>(0, clip.sourceInFrame));
        const int64_t localTimelineFrame = qMax<int64_t>(
            0,
            static_cast<int64_t>(std::floor(m_interaction.currentFramePosition)) - clip.startFrame);
        QSize clipFrameSize;
        for (const VulkanPreviewClipFrameStatus& status : m_interaction.vulkanFrameStatuses) {
            if (status.clipId == clip.id && status.frameSize.isValid()) {
                clipFrameSize = status.frameSize;
                break;
            }
        }
        const QVector<FacestreamTrack> tracks = loadFacestreamTracksForClip(clip);
        for (const FacestreamTrack& track : tracks) {
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
            const int64_t lookupFrame = facestreamLookupFrameForDomain(
                track.frameDomain, localTimelineFrame, localSourceFrame, localFrame);
            const FacestreamKeyframe* best = &track.keyframes.constFirst();
            auto nextIt = std::lower_bound(
                track.keyframes.constBegin(),
                track.keyframes.constEnd(),
                lookupFrame,
                [](const FacestreamKeyframe& keyframe, int64_t frame) {
                    return keyframe.frame < frame;
                });
            if (nextIt == track.keyframes.constBegin()) {
                best = &(*nextIt);
            } else if (nextIt == track.keyframes.constEnd()) {
                best = &track.keyframes.constLast();
            } else {
                const FacestreamKeyframe& nextKeyframe = *nextIt;
                const FacestreamKeyframe& prevKeyframe = *(nextIt - 1);
                const int64_t nextDistance = qAbs(nextKeyframe.frame - lookupFrame);
                const int64_t prevDistance = qAbs(prevKeyframe.frame - lookupFrame);
                best = (nextDistance < prevDistance) ? &nextKeyframe : &prevKeyframe;
            }
            if (qAbs(best->frame - lookupFrame) > 90) {
                continue;
            }
            VulkanPreviewFacestreamOverlay overlay;
            overlay.clipId = clip.id;
            overlay.streamId = track.streamId;
            overlay.source = best->source.isEmpty() ? track.source : best->source;
            overlay.trackId = track.trackId;
            overlay.sourceFrame = mapFacestreamFrameToSourceFrame(
                clip, best->frame, track.frameDomain, m_interaction.renderSyncMarkers);
            if (best->hasCenterBox && clipFrameSize.isValid()) {
                const QRectF centerBoxNorm = normalizedCenterBoxRect(
                    best->xNorm,
                    best->yNorm,
                    best->boxSizeNorm,
                    QSizeF(clipFrameSize.width(), clipFrameSize.height()));
                if (centerBoxNorm.isValid() && !centerBoxNorm.isEmpty()) {
                    overlay.boxNorm = centerBoxNorm;
                }
            } else {
                overlay.boxNorm = best->boxNorm;
            }
            if (!overlay.boxNorm.isValid() || overlay.boxNorm.isEmpty()) {
                continue;
            }
            overlay.confidence = best->confidence;
            overlays.push_back(overlay);
        }
    }
    m_interaction.facestreamOverlays = overlays;
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
        addStage(QStringLiteral("12 FaceStream Overlay"),
                 QStringLiteral("active overlays %1 | source frame %2")
                     .arg(m_interaction.facestreamOverlays.size())
                     .arg(status.presentedSourceFrame),
                 previewImage,
                 QStringLiteral("composite"),
                 true,
                 !m_interaction.facestreamOverlays.isEmpty());
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

QJsonObject VulkanPreviewSurface::profilingSnapshot() const
{
    QJsonObject snapshot = m_presenter ? m_presenter->profilingSnapshot() : QJsonObject{};
    snapshot.insert(QStringLiteral("render_use_proxy_media"), m_useProxyMedia);
    snapshot.insert(QStringLiteral("vulkan_decode_preference"), editor::decodePreferenceToString(editor::debugDecodePreference()));
    snapshot.insert(QStringLiteral("vulkan_cpu_upload_permitted"), false);
    snapshot.insert(QStringLiteral("vulkan_visible_backlog_limit"), m_playbackTuning.visibleBacklogLimit);
    snapshot.insert(QStringLiteral("vulkan_effective_lookahead_frames"), effectivePlaybackLookaheadFrames());
    snapshot.insert(QStringLiteral("vulkan_source_lookahead_frames"), m_playbackTuning.sourceLookaheadFrames);
    snapshot.insert(QStringLiteral("vulkan_proxy_lookahead_frames"), m_playbackTuning.proxyLookaheadFrames);
    if (m_decoder) {
        snapshot.insert(QStringLiteral("decoder_worker_count"), m_decoder->workerCount());
        snapshot.insert(QStringLiteral("decoder_pending_requests"), m_decoder->pendingRequestCount());
    }
    if (m_cache) {
        snapshot.insert(QStringLiteral("cache_pending_visible_requests"), m_cache->pendingVisibleRequestCount());
        snapshot.insert(QStringLiteral("pending_visible_requests"),
                        m_cache->pendingVisibleDebugSnapshot(QDateTime::currentMSecsSinceEpoch()));
        QJsonObject cacheSnapshot{
            {QStringLiteral("hit_rate"), m_cache->cacheHitRate()},
            {QStringLiteral("total_memory_usage"), static_cast<qint64>(m_cache->totalMemoryUsage())},
            {QStringLiteral("total_cached_frames"), m_cache->totalCachedFrames()},
            {QStringLiteral("pending_visible_requests"), m_cache->pendingVisibleRequestCount()}
        };
        const QJsonObject residency = m_cache->cacheResidencySnapshot();
        for (auto it = residency.begin(); it != residency.end(); ++it) {
            cacheSnapshot.insert(it.key(), it.value());
        }
        snapshot.insert(QStringLiteral("cache"), cacheSnapshot);
    }
    snapshot.insert(QStringLiteral("visible_request_attempts"), static_cast<double>(m_visibleRequestAttempts));
    snapshot.insert(QStringLiteral("visible_request_dispatched"), static_cast<double>(m_visibleRequestDispatched));
    snapshot.insert(QStringLiteral("visible_request_blocked"), static_cast<double>(m_visibleRequestBlocked));
    snapshot.insert(QStringLiteral("visible_request_callbacks"), static_cast<double>(m_visibleRequestCallbacks));
    snapshot.insert(QStringLiteral("visible_request_null_callbacks"), static_cast<double>(m_visibleRequestNullCallbacks));
    snapshot.insert(QStringLiteral("last_visible_request_clip_id"), m_lastVisibleRequestClipId);
    snapshot.insert(QStringLiteral("last_visible_request_frame"), static_cast<double>(m_lastVisibleRequestFrame));
    snapshot.insert(QStringLiteral("last_visible_request_decision"), m_lastVisibleRequestDecision);
    snapshot.insert(QStringLiteral("last_visible_request_block_reason"), m_lastVisibleRequestBlockReason);
    snapshot.insert(QStringLiteral("last_visible_request_cached"), m_lastVisibleRequestCached);
    snapshot.insert(QStringLiteral("last_visible_request_pending"), m_lastVisibleRequestPending);
    snapshot.insert(QStringLiteral("last_visible_request_force_retry"), m_lastVisibleRequestForceRetry);
    snapshot.insert(QStringLiteral("last_visible_request_backlog"), m_lastVisibleRequestBacklog);
    snapshot.insert(QStringLiteral("last_visible_request_callback_payload"), m_lastVisibleRequestCallbackPayload);
    snapshot.insert(QStringLiteral("playback_smoothness"), playbackSmoothnessSnapshot(snapshot));
    if (m_decoder && m_decoder->memoryBudget()) {
        const editor::MemoryBudget* budget = m_decoder->memoryBudget();
        snapshot.insert(QStringLiteral("memory_budget"), QJsonObject{
            {QStringLiteral("cpu_usage"), static_cast<qint64>(budget->currentCpuUsage())},
            {QStringLiteral("gpu_usage"), static_cast<qint64>(budget->currentGpuUsage())},
            {QStringLiteral("cpu_pressure"), budget->cpuPressure()},
            {QStringLiteral("gpu_pressure"), budget->gpuPressure()},
            {QStringLiteral("cpu_max"), static_cast<qint64>(budget->maxCpuMemory())},
            {QStringLiteral("gpu_max"), static_cast<qint64>(budget->maxGpuMemory())},
            {QStringLiteral("cpu_peak"), static_cast<qint64>(budget->peakCpuUsage())},
            {QStringLiteral("gpu_peak"), static_cast<qint64>(budget->peakGpuUsage())}
        });
        snapshot.insert(QStringLiteral("memory_budget_cpu_max_bytes"), static_cast<qint64>(budget->maxCpuMemory()));
        snapshot.insert(QStringLiteral("memory_budget_gpu_max_bytes"), static_cast<qint64>(budget->maxGpuMemory()));
        snapshot.insert(QStringLiteral("memory_budget_cpu_used_bytes"), static_cast<qint64>(budget->currentCpuUsage()));
        snapshot.insert(QStringLiteral("memory_budget_gpu_used_bytes"), static_cast<qint64>(budget->currentGpuUsage()));
        snapshot.insert(QStringLiteral("memory_budget_cpu_peak_bytes"), static_cast<qint64>(budget->peakCpuUsage()));
        snapshot.insert(QStringLiteral("memory_budget_gpu_peak_bytes"), static_cast<qint64>(budget->peakGpuUsage()));
        snapshot.insert(QStringLiteral("memory_budget_cpu_pressure"), budget->cpuPressure());
        snapshot.insert(QStringLiteral("memory_budget_gpu_pressure"), budget->gpuPressure());
    }
    return snapshot;
}

void VulkanPreviewSurface::resetProfilingStats()
{
    if (m_presenter) {
        m_presenter->resetProfilingStats();
    }
    m_playbackSmoothnessSamples.clear();
}

void VulkanPreviewSurface::recordPlaybackSmoothnessSample(int exactCount,
                                                          int approxCount,
                                                          int missingCount,
                                                          int64_t maxFrameLag)
{
    const qint64 nowMs = QDateTime::currentMSecsSinceEpoch();
    PlaybackSmoothnessSample sample;
    sample.timestampMs = nowMs;
    sample.exactCount = exactCount;
    sample.approxCount = approxCount;
    sample.missingCount = missingCount;
    sample.maxFrameLag = maxFrameLag;
    sample.playing = m_interaction.playing;
    sample.visibleRequestAttempts = m_visibleRequestAttempts;
    sample.visibleRequestDispatched = m_visibleRequestDispatched;
    sample.visibleRequestBlocked = m_visibleRequestBlocked;
    if (m_presenter) {
        const QJsonObject presenterSnapshot = m_presenter->profilingSnapshot();
        sample.lastUploadMs = presenterSnapshot.value(QStringLiteral("last_handoff_upload_ms")).toDouble();
        sample.handoffAttempts =
            static_cast<qint64>(presenterSnapshot.value(QStringLiteral("handoff_attempts")).toDouble());
        sample.handoffSuccesses =
            static_cast<qint64>(presenterSnapshot.value(QStringLiteral("handoff_successes")).toDouble());
        sample.handoffFailures =
            static_cast<qint64>(presenterSnapshot.value(QStringLiteral("handoff_failures")).toDouble());
        sample.presentedFrames =
            static_cast<qint64>(presenterSnapshot.value(QStringLiteral("presented_frames")).toDouble());
    }
    m_playbackSmoothnessSamples.push_back(sample);

    constexpr qint64 kWindowMs = 5000;
    constexpr int kMaxSamples = 240;
    while (!m_playbackSmoothnessSamples.isEmpty() &&
           (m_playbackSmoothnessSamples.constFirst().timestampMs < nowMs - kWindowMs ||
            m_playbackSmoothnessSamples.size() > kMaxSamples)) {
        m_playbackSmoothnessSamples.removeFirst();
    }
}

QJsonObject VulkanPreviewSurface::playbackSmoothnessSnapshot(const QJsonObject& presenterSnapshot) const
{
    constexpr qint64 kWindowMs = 5000;
    const qint64 nowMs = QDateTime::currentMSecsSinceEpoch();
    QVector<PlaybackSmoothnessSample> window;
    window.reserve(m_playbackSmoothnessSamples.size());
    for (const PlaybackSmoothnessSample& sample : m_playbackSmoothnessSamples) {
        if (sample.timestampMs >= nowMs - kWindowMs) {
            window.push_back(sample);
        }
    }

    QJsonObject smoothness{
        {QStringLiteral("window_ms"), kWindowMs},
        {QStringLiteral("sample_count"), window.size()},
        {QStringLiteral("playing"), m_interaction.playing},
        {QStringLiteral("playing_sample_count"), 0},
        {QStringLiteral("exact_hit_rate"), 0.0},
        {QStringLiteral("approximate_hit_rate"), 0.0},
        {QStringLiteral("missing_frame_rate"), 0.0},
        {QStringLiteral("late_sample_rate"), 0.0},
        {QStringLiteral("avg_frame_lag"), 0.0},
        {QStringLiteral("max_frame_lag"), 0},
        {QStringLiteral("avg_handoff_upload_ms"), 0.0},
        {QStringLiteral("p95_handoff_upload_ms"), 0.0},
        {QStringLiteral("max_handoff_upload_ms"), 0.0},
        {QStringLiteral("visible_request_attempt_rate"), 0.0},
        {QStringLiteral("visible_request_dispatch_rate"), 0.0},
        {QStringLiteral("visible_request_block_rate"), 0.0},
        {QStringLiteral("visible_request_blocked_fraction"), 0.0},
        {QStringLiteral("handoff_success_rate"), 0.0},
        {QStringLiteral("presented_fps_estimate"), 0.0},
        {QStringLiteral("current_decoder_pending_requests"),
         m_decoder ? m_decoder->pendingRequestCount() : 0},
        {QStringLiteral("current_decoder_worker_count"),
         m_decoder ? m_decoder->workerCount() : 0},
        {QStringLiteral("current_last_handoff_upload_ms"),
         presenterSnapshot.value(QStringLiteral("last_handoff_upload_ms")).toDouble()},
        {QStringLiteral("current_visible_request_backlog"), m_lastVisibleRequestBacklog},
        {QStringLiteral("current_last_visible_request_block_reason"), m_lastVisibleRequestBlockReason}
    };

    if (window.isEmpty()) {
        return smoothness;
    }

    int playingSampleCount = 0;
    qint64 exactTotal = 0;
    qint64 approxTotal = 0;
    qint64 missingTotal = 0;
    qint64 lateSamples = 0;
    qint64 frameLagTotal = 0;
    qint64 maxFrameLag = 0;
    QVector<double> uploadSamples;
    uploadSamples.reserve(window.size());

    for (const PlaybackSmoothnessSample& sample : window) {
        if (sample.playing) {
            ++playingSampleCount;
        }
        exactTotal += sample.exactCount;
        approxTotal += sample.approxCount;
        missingTotal += sample.missingCount;
        frameLagTotal += sample.maxFrameLag;
        maxFrameLag = qMax(maxFrameLag, sample.maxFrameLag);
        if (sample.maxFrameLag > 0) {
            ++lateSamples;
        }
        if (sample.lastUploadMs > 0.0) {
            uploadSamples.push_back(sample.lastUploadMs);
        }
    }

    const qint64 frameSamples = exactTotal + approxTotal + missingTotal;
    const qint64 availableSamples = exactTotal + approxTotal;
    smoothness[QStringLiteral("playing_sample_count")] = playingSampleCount;
    if (frameSamples > 0) {
        smoothness[QStringLiteral("exact_hit_rate")] =
            static_cast<double>(exactTotal) / static_cast<double>(frameSamples);
        smoothness[QStringLiteral("approximate_hit_rate")] =
            static_cast<double>(approxTotal) / static_cast<double>(frameSamples);
        smoothness[QStringLiteral("missing_frame_rate")] =
            static_cast<double>(missingTotal) / static_cast<double>(frameSamples);
    }
    if (availableSamples > 0) {
        smoothness[QStringLiteral("avg_frame_lag")] =
            static_cast<double>(frameLagTotal) / static_cast<double>(availableSamples);
    }
    smoothness[QStringLiteral("max_frame_lag")] = maxFrameLag;
    smoothness[QStringLiteral("late_sample_rate")] =
        static_cast<double>(lateSamples) / static_cast<double>(window.size());

    if (!uploadSamples.isEmpty()) {
        std::sort(uploadSamples.begin(), uploadSamples.end());
        const double uploadSum = std::accumulate(uploadSamples.begin(), uploadSamples.end(), 0.0);
        const int p95Index = qBound(0,
                                    static_cast<int>(std::ceil(uploadSamples.size() * 0.95)) - 1,
                                    uploadSamples.size() - 1);
        smoothness[QStringLiteral("avg_handoff_upload_ms")] =
            uploadSum / static_cast<double>(uploadSamples.size());
        smoothness[QStringLiteral("p95_handoff_upload_ms")] = uploadSamples.at(p95Index);
        smoothness[QStringLiteral("max_handoff_upload_ms")] = uploadSamples.constLast();
    }

    const PlaybackSmoothnessSample& first = window.constFirst();
    const PlaybackSmoothnessSample& last = window.constLast();
    const qint64 elapsedMs = qMax<qint64>(1, last.timestampMs - first.timestampMs);
    const double elapsedSeconds = static_cast<double>(elapsedMs) / 1000.0;
    const qint64 visibleAttemptsDelta =
        qMax<qint64>(0, last.visibleRequestAttempts - first.visibleRequestAttempts);
    const qint64 visibleDispatchedDelta =
        qMax<qint64>(0, last.visibleRequestDispatched - first.visibleRequestDispatched);
    const qint64 visibleBlockedDelta =
        qMax<qint64>(0, last.visibleRequestBlocked - first.visibleRequestBlocked);
    const qint64 handoffAttemptsDelta =
        qMax<qint64>(0, last.handoffAttempts - first.handoffAttempts);
    const qint64 handoffSuccessesDelta =
        qMax<qint64>(0, last.handoffSuccesses - first.handoffSuccesses);
    const qint64 presentedFramesDelta =
        qMax<qint64>(0, last.presentedFrames - first.presentedFrames);

    smoothness[QStringLiteral("visible_request_attempt_rate")] =
        static_cast<double>(visibleAttemptsDelta) / elapsedSeconds;
    smoothness[QStringLiteral("visible_request_dispatch_rate")] =
        static_cast<double>(visibleDispatchedDelta) / elapsedSeconds;
    smoothness[QStringLiteral("visible_request_block_rate")] =
        static_cast<double>(visibleBlockedDelta) / elapsedSeconds;
    if (visibleAttemptsDelta > 0) {
        smoothness[QStringLiteral("visible_request_blocked_fraction")] =
            static_cast<double>(visibleBlockedDelta) / static_cast<double>(visibleAttemptsDelta);
    }
    if (handoffAttemptsDelta > 0) {
        smoothness[QStringLiteral("handoff_success_rate")] =
            static_cast<double>(handoffSuccessesDelta) / static_cast<double>(handoffAttemptsDelta);
    }
    smoothness[QStringLiteral("presented_fps_estimate")] =
        static_cast<double>(presentedFramesDelta) / elapsedSeconds;

    return smoothness;
}

bool VulkanPreviewSurface::selectedOverlayIsTranscript() const
{
    if (!m_interaction.transcriptOverlayInteractionEnabled || m_interaction.selectedClipId.isEmpty()) {
        return false;
    }
    for (const TimelineClip& clip : m_interaction.clips) {
        if (clip.id != m_interaction.selectedClipId) {
            continue;
        }
        if (!clipSupportsDrawableTranscriptOverlayForSelection(clip,
                                                              m_interaction.currentSample,
                                                              m_interaction.renderSyncMarkers)) {
            return false;
        }
        return true;
    }
    return false;
}
