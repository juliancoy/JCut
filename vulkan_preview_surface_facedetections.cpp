#include "vulkan_preview_surface.h"

#include "facedetections_artifact_utils.h"
#include "facedetections_runtime.h"
#include "facedetections_time_mapping.h"
#include "transcript_engine.h"

#include <QFileInfo>
#include <QJsonArray>
#include <QJsonObject>

#include <algorithm>
#include <cmath>
#include <limits>

namespace {

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

} // namespace

QVector<VulkanPreviewSurface::FacestreamTrack> VulkanPreviewSurface::parseContinuityTracksForClip(
    const TimelineClip& clip,
    const QJsonObject& artifactRoot) const
{
    QVector<FacestreamTrack> tracks;
    const QJsonObject continuityRoot = continuityRootForClip(artifactRoot, clip.id);
    FacestreamFrameDomain fallbackFrameDomain = FacestreamFrameDomain::SourceRelative;
    const bool hasFallbackFrameDomain = continuityPayloadFrameDomain(
        continuityRoot,
        QStringLiteral("streams_frame_domain"),
        &fallbackFrameDomain);
    const QJsonArray streams = jcut::facedetections::continuityStreamsForRoot(continuityRoot);
    tracks.reserve(streams.size());
    for (const QJsonValue& streamValue : streams) {
        const QJsonObject streamObj = streamValue.toObject();
        FacestreamTrack track;
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
            FacestreamKeyframe keyframe;
            keyframe.frame = keyframeObj.value(QStringLiteral("frame")).toVariant().toLongLong();
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
        if (!parseFacestreamFrameDomainString(
                streamObj.value(QStringLiteral("frame_domain")).toString().trimmed(),
                &track.frameDomain)) {
            if (!hasFallbackFrameDomain) {
                continue;
            }
            track.frameDomain = fallbackFrameDomain;
        }
        std::sort(track.keyframes.begin(), track.keyframes.end(), [](const FacestreamKeyframe& a, const FacestreamKeyframe& b) {
            return a.frame < b.frame;
        });
        QVector<int64_t> sortedFrames;
        sortedFrames.reserve(track.keyframes.size());
        for (const FacestreamKeyframe& keyframe : track.keyframes) {
            sortedFrames.push_back(keyframe.frame);
        }
        track.typicalFrameStep = facedetectionsTypicalFrameStep(sortedFrames);
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
        facedetectionsArtifactRevisionMsForTranscript(transcriptInfo.absoluteFilePath());
    const QString signature = QStringLiteral("%1|%2|%3|%4|%5")
                                  .arg(clip.id)
                                  .arg(transcriptInfo.absoluteFilePath())
                                  .arg(transcriptInfo.exists() ? transcriptInfo.lastModified().toMSecsSinceEpoch() : 0)
                                  .arg(artifactRevisionMs)
                                  .arg(m_facedetectionsOverlaySource);
    FacestreamOverlayCacheEntry& entry = m_facedetectionsOverlayCache[clipPath];
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
        entry.rawDetections += parseRawDetectionsForClip(clip, artifactRoot);
        for (const VulkanPreviewFacestreamOverlay& detection : entry.rawDetections) {
            entry.rawDetectionsBySourceFrame[detection.sourceFrame].push_back(detection);
        }
        entry.rawDetectionSourceFrames.reserve(entry.rawDetectionsBySourceFrame.size());
        for (auto it = entry.rawDetectionsBySourceFrame.constBegin();
             it != entry.rawDetectionsBySourceFrame.constEnd();
             ++it) {
            entry.rawDetectionSourceFrames.push_back(it.key());
        }
        std::sort(entry.rawDetectionSourceFrames.begin(), entry.rawDetectionSourceFrames.end());
        entry.rawDetectionTypicalFrameStep =
            facedetectionsTypicalFrameStep(entry.rawDetectionSourceFrames);
    }
    return entry.tracks;
}

QVector<VulkanPreviewFacestreamOverlay> VulkanPreviewSurface::parseRawDetectionsForClip(
    const TimelineClip& clip,
    const QJsonObject& artifactRoot) const
{
    QVector<VulkanPreviewFacestreamOverlay> detections;
    const QJsonObject continuityRoot = continuityRootForClip(artifactRoot, clip.id);
    const QJsonArray rawFrames = continuityRoot.value(QStringLiteral("raw_frames")).toArray();
    FacestreamFrameDomain frameDomain = FacestreamFrameDomain::SourceRelative;
    if (!continuityPayloadFrameDomain(
            continuityRoot,
            QStringLiteral("raw_frames_frame_domain"),
            &frameDomain)) {
        return detections;
    }
    for (const QJsonValue& frameValue : rawFrames) {
        const QJsonObject frameObj = frameValue.toObject();
        const int64_t frame = frameObj.value(QStringLiteral("frame")).toVariant().toLongLong();
        if (frame < 0) {
            continue;
        }
        const QJsonArray rows = frameObj.value(QStringLiteral("detections")).toArray();
        for (const QJsonValue& detValue : rows) {
            const QJsonObject detObj = detValue.toObject();
            const qreal frameWidth = qMax<qreal>(1.0, frameObj.value(QStringLiteral("frame_width")).toDouble(
                detObj.value(QStringLiteral("frame_width")).toDouble(0.0)));
            const qreal frameHeight = qMax<qreal>(1.0, frameObj.value(QStringLiteral("frame_height")).toDouble(
                detObj.value(QStringLiteral("frame_height")).toDouble(0.0)));
            const qreal x = qBound<qreal>(
                0.0,
                detObj.value(QStringLiteral("x_norm")).toDouble(
                    detObj.value(QStringLiteral("x")).toDouble(0.0) / frameWidth),
                1.0);
            const qreal y = qBound<qreal>(
                0.0,
                detObj.value(QStringLiteral("y_norm")).toDouble(
                    detObj.value(QStringLiteral("y")).toDouble(0.0) / frameHeight),
                1.0);
            const qreal w = qBound<qreal>(
                0.0,
                detObj.value(QStringLiteral("w_norm")).toDouble(
                    detObj.value(QStringLiteral("w")).toDouble(0.0) / frameWidth),
                1.0);
            const qreal h = qBound<qreal>(
                0.0,
                detObj.value(QStringLiteral("h_norm")).toDouble(
                    detObj.value(QStringLiteral("h")).toDouble(0.0) / frameHeight),
                1.0);
            if (w <= 0.0 || h <= 0.0) {
                continue;
            }
            VulkanPreviewFacestreamOverlay overlay;
            overlay.clipId = clip.id;
            overlay.streamId = QStringLiteral("raw_detection");
            overlay.source = QStringLiteral("raw_detection");
            overlay.trackId = -1;
            overlay.sourceFrame = mapFacestreamFrameToSourceFrame(
                clip, frame, frameDomain, m_interaction.renderSyncMarkers);
            overlay.boxNorm = QRectF(x, y, w, h).intersected(QRectF(0.0, 0.0, 1.0, 1.0));
            overlay.confidence = qBound<qreal>(
                0.0,
                detObj.value(QStringLiteral("confidence")).toDouble(detObj.value(QStringLiteral("score")).toDouble(0.0)),
                1.0);
            if (overlay.boxNorm.isValid() && !overlay.boxNorm.isEmpty()) {
                detections.push_back(overlay);
            }
        }
    }
    return detections;
}

QVector<VulkanPreviewFacestreamOverlay> VulkanPreviewSurface::rawDetectionsForClipFrame(
    const TimelineClip& clip,
    int64_t sourceFrame)
{
    loadFacestreamTracksForClip(clip);
    const QString clipPath = QFileInfo(clip.filePath).absoluteFilePath();
    const FacestreamOverlayCacheEntry entry = m_facedetectionsOverlayCache.value(clipPath);
    const auto exact = entry.rawDetectionsBySourceFrame.constFind(sourceFrame);
    if (exact != entry.rawDetectionsBySourceFrame.constEnd()) {
        return exact.value();
    }
    if (entry.rawDetectionSourceFrames.isEmpty()) {
        return {};
    }

    const auto nextIt = std::lower_bound(
        entry.rawDetectionSourceFrames.constBegin(),
        entry.rawDetectionSourceFrames.constEnd(),
        sourceFrame);
    const int64_t typicalStep = qMax<int64_t>(1, entry.rawDetectionTypicalFrameStep);
    const int64_t edgeHoldFrames = facedetectionsMaxEdgeHoldFrames(typicalStep);
    const int64_t* previous =
        (nextIt != entry.rawDetectionSourceFrames.constBegin()) ? &(*(nextIt - 1)) : nullptr;
    const int64_t* next =
        (nextIt != entry.rawDetectionSourceFrames.constEnd()) ? &(*nextIt) : nullptr;

    auto detectionsForStoredFrame = [&](int64_t storedFrame) {
        return entry.rawDetectionsBySourceFrame.value(storedFrame);
    };

    if (previous && next && facedetectionsShouldBridgeGap(*previous, *next, typicalStep)) {
        const int64_t previousDistance = qAbs(sourceFrame - *previous);
        const int64_t nextDistance = qAbs(*next - sourceFrame);
        return previousDistance <= nextDistance
            ? detectionsForStoredFrame(*previous)
            : detectionsForStoredFrame(*next);
    }
    if (previous && qAbs(sourceFrame - *previous) <= edgeHoldFrames) {
        return detectionsForStoredFrame(*previous);
    }
    if (next && qAbs(*next - sourceFrame) <= edgeHoldFrames) {
        return detectionsForStoredFrame(*next);
    }
    return {};
}

void VulkanPreviewSurface::refreshFacestreamOverlays()
{
    QVector<VulkanPreviewFacestreamOverlay> overlays;
    QVector<VulkanPreviewFacestreamOverlay> rawDetections;
    if (!m_showSpeakerTrackBoxes && !m_interaction.faceStreamAssignmentInteractionEnabled && !m_showRawDetections) {
        m_interaction.facedetectionsOverlays = overlays;
        m_interaction.rawDetectionOverlays = rawDetections;
        return;
    }
    auto keyframeBoxNorm = [](const FacestreamKeyframe& keyframe,
                              const QSize& clipFrameSize) -> QRectF {
        if (keyframe.hasCenterBox) {
            if (!clipFrameSize.isValid()) {
                return QRectF();
            }
            return normalizedCenterBoxRect(
                keyframe.xNorm,
                keyframe.yNorm,
                keyframe.boxSizeNorm,
                QSizeF(clipFrameSize.width(), clipFrameSize.height()));
        }
        return keyframe.boxNorm;
    };
    const QString sourceFilter = normalizedFacestreamOverlaySource(m_facedetectionsOverlaySource);
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
        // Keep raw detections visible when explicitly enabled, even while the
        // Speakers tab turns on assignment interaction. The UI exposes these as
        // independent preview overlays, so suppressing raw detections here
        // makes the checkbox appear broken.
        const bool showRawDetectionsForPreview = m_showRawDetections;
        if (showRawDetectionsForPreview) {
            const QVector<VulkanPreviewFacestreamOverlay> clipDetections =
                rawDetectionsForClipFrame(clip, localFrame);
            for (const VulkanPreviewFacestreamOverlay& detection : clipDetections) {
                if (detection.boxNorm.isValid() && !detection.boxNorm.isEmpty()) {
                    rawDetections.push_back(detection);
                }
            }
        }
        for (const FacestreamTrack& track : tracks) {
            if (track.keyframes.isEmpty()) {
                continue;
            }
            if (!facedetectionsOverlaySourceMatches(sourceFilter, track.source, track.streamId)) {
                continue;
            }
            FacestreamResolvedSelection selection;
            if (!resolveFacestreamTrackAtPlayhead(
                    clip,
                    track,
                    m_interaction.renderSyncMarkers,
                    clip.startFrame + localTimelineFrame,
                    localFrame,
                    &selection)) {
                continue;
            }

            VulkanPreviewFacestreamOverlay overlay;
            overlay.clipId = clip.id;
            overlay.streamId = track.streamId;
            overlay.source = selection.keyframe.source.isEmpty() ? track.source : selection.keyframe.source;
            overlay.trackId = track.trackId;
            overlay.sourceFrame = selection.sourceFrame;
            overlay.boxNorm = keyframeBoxNorm(selection.keyframe, clipFrameSize);
            if (!overlay.boxNorm.isValid() || overlay.boxNorm.isEmpty()) {
                continue;
            }
            overlay.confidence = selection.keyframe.confidence;
            overlays.push_back(overlay);
        }
    }
    m_interaction.facedetectionsOverlays = overlays;
    m_interaction.rawDetectionOverlays = rawDetections;
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
