#include "vulkan_preview_surface.h"

#include "facedetections_artifact_utils.h"
#include "facedetections_runtime.h"
#include "facedetections_time_mapping.h"
#include "transcript_engine.h"

#include <QFileInfo>
#include <QJsonArray>
#include <QJsonObject>
#include <QElapsedTimer>
#include <QDateTime>
#include <QMetaObject>
#include <QPointer>
#include <QtConcurrent/QtConcurrentRun>

#include <algorithm>
#include <cmath>
#include <limits>

namespace {
constexpr qint64 kFacestreamOverlaySlowQueryWarnMs = 100;
constexpr int64_t kMaxPreservedPlaybackOverlayDriftFrames = 60;

QJsonObject overlayDriftDebug(const QVector<VulkanPreviewFacestreamOverlay>& overlays,
                              const QVector<VulkanPreviewFacestreamOverlay>& rawDetections,
                              int64_t requestedSourceFrame)
{
    int64_t minFrame = std::numeric_limits<int64_t>::max();
    int64_t maxFrame = std::numeric_limits<int64_t>::min();
    auto scan = [&](const QVector<VulkanPreviewFacestreamOverlay>& values) {
        for (const VulkanPreviewFacestreamOverlay& overlay : values) {
            if (overlay.sourceFrame < 0) {
                continue;
            }
            minFrame = qMin(minFrame, overlay.sourceFrame);
            maxFrame = qMax(maxFrame, overlay.sourceFrame);
        }
    };
    scan(overlays);
    scan(rawDetections);
    if (minFrame == std::numeric_limits<int64_t>::max()) {
        return QJsonObject{
            {QStringLiteral("previous_overlay_has_source_frames"), false},
            {QStringLiteral("previous_overlay_max_abs_drift_frames"), static_cast<qint64>(-1)}
        };
    }
    const int64_t drift = qMax(qAbs(requestedSourceFrame - minFrame),
                               qAbs(requestedSourceFrame - maxFrame));
    return QJsonObject{
        {QStringLiteral("previous_overlay_has_source_frames"), true},
        {QStringLiteral("previous_overlay_min_source_frame"), static_cast<qint64>(minFrame)},
        {QStringLiteral("previous_overlay_max_source_frame"), static_cast<qint64>(maxFrame)},
        {QStringLiteral("previous_overlay_max_abs_drift_frames"), static_cast<qint64>(drift)}
    };
}

bool previousPlaybackOverlayIsCloseEnough(const QVector<VulkanPreviewFacestreamOverlay>& overlays,
                                          const QVector<VulkanPreviewFacestreamOverlay>& rawDetections,
                                          int64_t requestedSourceFrame)
{
    const QJsonObject drift = overlayDriftDebug(overlays, rawDetections, requestedSourceFrame);
    if (!drift.value(QStringLiteral("previous_overlay_has_source_frames")).toBool(false)) {
        return false;
    }
    return drift.value(QStringLiteral("previous_overlay_max_abs_drift_frames")).toInteger(
               std::numeric_limits<qint64>::max()) <= kMaxPreservedPlaybackOverlayDriftFrames;
}

quint64 renderSyncMarkerOverlaySignature(const QVector<RenderSyncMarker>& markers)
{
    quint64 signature = static_cast<quint64>(markers.size()) * 1469598103934665603ULL;
    for (const RenderSyncMarker& marker : markers) {
        signature ^= static_cast<quint64>(qHash(marker.clipId));
        signature *= 1099511628211ULL;
        signature ^= static_cast<quint64>(marker.frame);
        signature *= 1099511628211ULL;
        signature ^= static_cast<quint64>(qMax(1, marker.count));
        signature *= 1099511628211ULL;
        signature ^= static_cast<quint64>(static_cast<int>(marker.action));
    }
    return signature;
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

} // namespace

QRectF VulkanPreviewSurface::facestreamKeyframeBoxNorm(const FacestreamKeyframe& keyframe,
                                                       const QSize& clipFrameSize)
{
    return jcut::preview_overlay::facestreamKeyframeBoxNorm(keyframe, clipFrameSize);
}

QVector<VulkanPreviewFacestreamOverlay> VulkanPreviewSurface::rawDetectionsFromCacheEntry(
    const FacestreamOverlayCacheEntry& entry,
    int64_t sourceFrame)
{
    return jcut::preview_overlay::rawDetectionsFromCacheEntry(entry, sourceFrame);
}

QVector<VulkanPreviewSurface::FacestreamTrack> VulkanPreviewSurface::parseContinuityTracksForClip(
    const TimelineClip& clip,
    const QJsonArray& streams,
    const QJsonObject& continuityRoot) const
{
    QVector<FacestreamTrack> tracks;
    FacestreamFrameDomain fallbackFrameDomain = FacestreamFrameDomain::SourceRelative;
    const bool hasFallbackFrameDomain = continuityPayloadFrameDomain(
        continuityRoot,
        QStringLiteral("streams_frame_domain"),
        &fallbackFrameDomain);
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

QVector<VulkanPreviewSurface::FacestreamTrack> VulkanPreviewSurface::convertContinuityTrackModelsForClip(
    const QVector<jcut::facedetections::FacestreamTrack>& models,
    const QString& frameDomain,
    const QString& detectorMode) const
{
    QVector<FacestreamTrack> tracks;
    FacestreamFrameDomain parsedFrameDomain = FacestreamFrameDomain::SourceRelative;
    const bool hasFrameDomain =
        parseFacestreamFrameDomainString(frameDomain.trimmed(), &parsedFrameDomain);
    tracks.reserve(models.size());
    for (const jcut::facedetections::FacestreamTrack& model : models) {
        FacestreamTrack track;
        track.streamId = model.summary.streamId.trimmed();
        if (track.streamId.isEmpty() && model.summary.trackId >= 0) {
            track.streamId = QStringLiteral("T%1").arg(model.summary.trackId);
        }
        track.trackId = model.summary.trackId;
        track.source = model.summary.source.trimmed().toLower();
        if (track.source.isEmpty()) {
            track.source = detectorMode.trimmed().toLower();
        }
        track.frameDomain = hasFrameDomain ? parsedFrameDomain : FacestreamFrameDomain::SourceAbsolute;
        track.typicalFrameStep = qMax<int64_t>(1, model.summary.typicalFrameStep);
        track.keyframes.reserve(model.keyframes.size());
        for (const jcut::facedetections::FacestreamKeyframe& modelKeyframe : model.keyframes) {
            FacestreamKeyframe keyframe;
            keyframe.frame = modelKeyframe.frame;
            keyframe.xNorm = qBound<qreal>(0.0, modelKeyframe.x, 1.0);
            keyframe.yNorm = qBound<qreal>(0.0, modelKeyframe.y, 1.0);
            keyframe.boxSizeNorm = qBound<qreal>(0.001, modelKeyframe.box, 1.0);
            keyframe.hasCenterBox = true;
            keyframe.confidence = qBound<qreal>(0.0, modelKeyframe.confidence, 1.0);
            keyframe.source = track.source;
            track.keyframes.push_back(keyframe);
        }
        if (!track.keyframes.isEmpty()) {
            tracks.push_back(track);
        }
    }
    return tracks;
}

QVector<VulkanPreviewSurface::FacestreamTrack> VulkanPreviewSurface::loadFacestreamTracksForClip(
    const TimelineClip& clip,
    int64_t sourceFrame)
{
    QElapsedTimer queryTimer;
    queryTimer.start();
    const QString clipPath = QFileInfo(clip.filePath).absoluteFilePath();
    const QString transcriptPath = activeTranscriptPathForClipFile(clip.filePath);
    const QFileInfo transcriptInfo(transcriptPath);
    const qint64 artifactRevisionMs =
        facedetectionsArtifactRevisionMsForTranscript(transcriptInfo.absoluteFilePath());
    const quint64 renderSyncSignature =
        renderSyncMarkerOverlaySignature(m_interaction.renderSyncMarkers);
    const QString signature = QStringLiteral("%1|%2|%3|%4|%5|sync:%6")
                                  .arg(clip.id)
                                  .arg(transcriptInfo.absoluteFilePath())
                                  .arg(transcriptInfo.exists() ? transcriptInfo.lastModified().toMSecsSinceEpoch() : 0)
                                  .arg(artifactRevisionMs)
                                  .arg(QStringLiteral("%1|%2").arg(m_facedetectionsOverlaySource).arg(sourceFrame))
                                  .arg(renderSyncSignature);
    FacestreamOverlayCacheEntry& entry = m_facedetectionsOverlayCache[clipPath];
    if (entry.signature == signature) {
        return entry.tracks;
    }

    entry = FacestreamOverlayCacheEntry{};
    entry.signature = signature;
    if (!transcriptInfo.exists() || !transcriptInfo.isFile()) {
        if (clip.id == m_interaction.selectedClipId) {
            m_lastFacedetectionsQueryDebug = QJsonObject{
                {QStringLiteral("status"), QStringLiteral("missing_transcript")},
                {QStringLiteral("clip_id"), clip.id},
                {QStringLiteral("transcript_path"), transcriptPath},
                {QStringLiteral("requested_source_frame"), static_cast<qint64>(sourceFrame)}
            };
        }
        return entry.tracks;
    }

    editor::TranscriptEngine engine;
    QJsonDocument transcriptDoc;
    bool transcriptLoaded = false;
    auto transcriptRootForDemand = [&]() -> QJsonObject {
        if (!transcriptLoaded) {
            engine.loadTranscriptJson(transcriptPath, &transcriptDoc);
            transcriptLoaded = true;
        }
        return transcriptDoc.object();
    };
    QJsonObject queryDebug{
        {QStringLiteral("status"), QStringLiteral("ok")},
        {QStringLiteral("clip_id"), clip.id},
        {QStringLiteral("transcript_path"), transcriptPath},
        {QStringLiteral("requested_source_frame"), static_cast<qint64>(sourceFrame)},
        {QStringLiteral("artifact_revision_ms"), static_cast<qint64>(artifactRevisionMs)},
        {QStringLiteral("signature"), signature}
    };
    QJsonObject processedArtifactRoot;
    if (engine.loadFacestreamProcessedArtifact(transcriptPath, &processedArtifactRoot)) {
        const QJsonObject continuityRoot = continuityRootForClip(processedArtifactRoot, clip.id);
        QJsonArray streams = jcut::facedetections::storedContinuityStreamsForRoot(continuityRoot);
        queryDebug[QStringLiteral("processed_continuity_root_found")] = !continuityRoot.isEmpty();
        queryDebug[QStringLiteral("processed_stored_stream_count")] = streams.size();
        queryDebug[QStringLiteral("processed_raw_tracks_artifact_path")] =
            continuityRoot.value(QStringLiteral("raw_tracks_artifact_path")).toString().trimmed();
        queryDebug[QStringLiteral("processed_raw_frames_artifact_path")] =
            continuityRoot.value(QStringLiteral("raw_frames_artifact_path")).toString().trimmed();
        if (streams.isEmpty()) {
            const QJsonObject transcriptRoot = continuityRoot.value(QStringLiteral("only_dialogue")).toBool(false)
                ? transcriptRootForDemand()
                : QJsonObject{};
            const QVector<jcut::facedetections::FacestreamTrack> trackModels =
                jcut::facedetections::continuityTrackModelsNearFrameForRoot(
                continuityRoot,
                sourceFrame,
                0,
                    transcriptRoot);
            entry.tracks += convertContinuityTrackModelsForClip(
                trackModels,
                continuityRoot.value(QStringLiteral("raw_tracks_frame_domain")).toString(
                    continuityRoot.value(QStringLiteral("streams_frame_domain")).toString()),
                continuityRoot.value(QStringLiteral("detector_mode")).toString());
            queryDebug[QStringLiteral("processed_near_frame_track_model_count")] = trackModels.size();
        } else {
            entry.tracks += parseContinuityTracksForClip(clip, streams, continuityRoot);
        }
        queryDebug[QStringLiteral("processed_parsed_track_count")] = entry.tracks.size();
    }
    QJsonObject artifactRoot;
    if (engine.loadFacestreamArtifact(transcriptPath, &artifactRoot)) {
        if (entry.tracks.isEmpty()) {
            const QJsonObject continuityRoot = continuityRootForClip(artifactRoot, clip.id);
            QJsonArray streams = jcut::facedetections::storedContinuityStreamsForRoot(continuityRoot);
            queryDebug[QStringLiteral("raw_continuity_root_found")] = !continuityRoot.isEmpty();
            queryDebug[QStringLiteral("raw_stored_stream_count")] = streams.size();
            queryDebug[QStringLiteral("raw_raw_tracks_artifact_path")] =
                continuityRoot.value(QStringLiteral("raw_tracks_artifact_path")).toString().trimmed();
            queryDebug[QStringLiteral("raw_raw_frames_artifact_path")] =
                continuityRoot.value(QStringLiteral("raw_frames_artifact_path")).toString().trimmed();
            queryDebug[QStringLiteral("raw_tracks_inline_count")] =
                continuityRoot.value(QStringLiteral("raw_tracks")).toArray().size();
            queryDebug[QStringLiteral("raw_frames_inline_count")] =
                continuityRoot.value(QStringLiteral("raw_frames")).toArray().size();
            if (streams.isEmpty()) {
                const QJsonObject transcriptRoot = continuityRoot.value(QStringLiteral("only_dialogue")).toBool(false)
                    ? transcriptRootForDemand()
                    : QJsonObject{};
                const QVector<jcut::facedetections::FacestreamTrack> trackModels =
                    jcut::facedetections::continuityTrackModelsNearFrameForRoot(
                    continuityRoot,
                    sourceFrame,
                    0,
                        transcriptRoot);
                entry.tracks += convertContinuityTrackModelsForClip(
                    trackModels,
                    continuityRoot.value(QStringLiteral("raw_tracks_frame_domain")).toString(
                        continuityRoot.value(QStringLiteral("streams_frame_domain")).toString()),
                    continuityRoot.value(QStringLiteral("detector_mode")).toString());
                queryDebug[QStringLiteral("raw_near_frame_track_model_count")] = trackModels.size();
            } else {
                entry.tracks += parseContinuityTracksForClip(clip, streams, continuityRoot);
            }
            queryDebug[QStringLiteral("raw_parsed_track_count")] = entry.tracks.size();
        }
        const QJsonObject rawDetectionContinuityRoot = continuityRootForClip(artifactRoot, clip.id);
        FacestreamFrameDomain rawFrameDomain = FacestreamFrameDomain::SourceRelative;
        if (continuityPayloadFrameDomain(
                rawDetectionContinuityRoot,
                QStringLiteral("raw_frames_frame_domain"),
                &rawFrameDomain)) {
            constexpr int64_t kRawDetectionInteractiveWindowFrames = 45;
            const QVector<jcut::facedetections::FacestreamFrameDetections> rawFrameModels =
                jcut::facedetections::frameDetectionModelsNearFrameForRoot(
                    rawDetectionContinuityRoot,
                    sourceFrame,
                    kRawDetectionInteractiveWindowFrames);
            entry.rawDetections += convertRawDetectionModelsForClip(clip, rawFrameModels, rawFrameDomain);
        }
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
        queryDebug[QStringLiteral("raw_detection_frame_count")] = entry.rawDetectionsBySourceFrame.size();
        queryDebug[QStringLiteral("raw_detection_source_frame_count")] = entry.rawDetectionSourceFrames.size();
        queryDebug[QStringLiteral("raw_detection_typical_frame_step")] =
            static_cast<qint64>(entry.rawDetectionTypicalFrameStep);
    }
    jcut::preview_overlay::buildFacestreamTrackCandidateIndex(
        entry, clip, m_interaction.renderSyncMarkers);
    queryDebug[QStringLiteral("final_track_count")] = entry.tracks.size();
    queryDebug[QStringLiteral("track_index_source_frame_count")] = entry.trackIndexSourceFrames.size();
    queryDebug[QStringLiteral("track_index_typical_frame_step")] =
        static_cast<qint64>(entry.trackIndexTypicalFrameStep);
    queryDebug[QStringLiteral("final_raw_detection_count")] = entry.rawDetections.size();
    queryDebug[QStringLiteral("elapsed_ms")] = queryTimer.elapsed();
    if (queryTimer.elapsed() >= kFacestreamOverlaySlowQueryWarnMs) {
        queryDebug[QStringLiteral("slow_query")] = true;
        qWarning().noquote()
            << QStringLiteral("[PREVIEW WARN] FaceDetections overlay query slow: elapsed_ms=%1 clip_id=%2 source_frame=%3")
                   .arg(queryTimer.elapsed())
                   .arg(clip.id)
                   .arg(sourceFrame);
    }
    if (clip.id == m_interaction.selectedClipId) {
        m_lastFacedetectionsQueryDebug = queryDebug;
    }
    return entry.tracks;
}

QVector<VulkanPreviewFacestreamOverlay> VulkanPreviewSurface::convertRawDetectionModelsForClip(
    const TimelineClip& clip,
    const QVector<jcut::facedetections::FacestreamFrameDetections>& frames,
    FacestreamFrameDomain frameDomain) const
{
    QVector<VulkanPreviewFacestreamOverlay> detections;
    for (const jcut::facedetections::FacestreamFrameDetections& frame : frames) {
        if (frame.frame < 0) {
            continue;
        }
        const int64_t sourceFrame = mapFacestreamFrameToSourceFrame(
            clip, frame.frame, frameDomain, m_interaction.renderSyncMarkers);
        for (const jcut::facedetections::FacestreamDetection& detection : frame.detections) {
            if (!detection.box.isValid() || detection.box.isEmpty()) {
                continue;
            }
            VulkanPreviewFacestreamOverlay overlay;
            overlay.clipId = clip.id;
            overlay.streamId = QStringLiteral("raw_detection");
            overlay.source = QStringLiteral("raw_detection");
            overlay.trackId = detection.trackId;
            overlay.sourceFrame = sourceFrame;
            overlay.boxNorm = detection.box;
            overlay.confidence = qBound<qreal>(0.0, detection.confidence, 1.0);
            detections.push_back(overlay);
        }
    }
    return detections;
}

QVector<VulkanPreviewFacestreamOverlay> VulkanPreviewSurface::rawDetectionsForClipFrame(
    const TimelineClip& clip,
    int64_t sourceFrame)
{
    loadFacestreamTracksForClip(clip, sourceFrame);
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
    const QVector<VulkanPreviewFacestreamOverlay> previousOverlays =
        m_interaction.facedetectionsOverlays;
    const QVector<VulkanPreviewFacestreamOverlay> previousRawDetections =
        m_interaction.rawDetectionOverlays;
    QVector<VulkanPreviewFacestreamOverlay> overlays;
    QVector<VulkanPreviewFacestreamOverlay> rawDetections;
    if (!m_showSpeakerTrackBoxes && !m_interaction.faceStreamAssignmentInteractionEnabled && !m_showRawDetections) {
        m_interaction.facedetectionsOverlays = overlays;
        m_interaction.rawDetectionOverlays = rawDetections;
        m_appliedFacestreamOverlaySnapshotKey.clear();
        return;
    }
    const QString sourceFilter = normalizedFacestreamOverlaySource(m_facedetectionsOverlaySource);

    if (m_interaction.playing) {
        QVector<FacestreamOverlayRequestClip> requestClips;
        QStringList keyParts;
        keyParts.reserve(m_interaction.clips.size() + 4);
        keyParts << QString::number(m_interaction.currentSample)
                 << sourceFilter
                 << QString::number(m_showSpeakerTrackBoxes)
                 << QString::number(m_showRawDetections)
                 << QString::number(m_interaction.faceStreamAssignmentInteractionEnabled);
        bool playbackSuppressedColdLookup = false;
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
            const QString clipPath = QFileInfo(clip.filePath).absoluteFilePath();
            const auto cachedEntryIt = m_facedetectionsOverlayCache.constFind(clipPath);
            if (cachedEntryIt == m_facedetectionsOverlayCache.constEnd()) {
                playbackSuppressedColdLookup = true;
                if (clip.id == m_interaction.selectedClipId) {
                    QJsonObject drift = overlayDriftDebug(previousOverlays, previousRawDetections, localFrame);
                    m_lastFacedetectionsQueryDebug = QJsonObject{
                        {QStringLiteral("status"), QStringLiteral("playback_cold_overlay_lookup_suppressed_preserving_previous")},
                        {QStringLiteral("clip_id"), clip.id},
                        {QStringLiteral("playback_active"), true},
                        {QStringLiteral("previous_overlay_count"), previousOverlays.size()},
                        {QStringLiteral("previous_raw_detection_count"), previousRawDetections.size()},
                        {QStringLiteral("requested_source_frame"), static_cast<qint64>(localFrame)},
                        {QStringLiteral("preserve_previous_overlay_drift_limit_frames"),
                         static_cast<qint64>(kMaxPreservedPlaybackOverlayDriftFrames)},
                        {QStringLiteral("show_speaker_track_boxes"), m_showSpeakerTrackBoxes},
                        {QStringLiteral("show_raw_detections"), m_showRawDetections},
                        {QStringLiteral("assignment_interaction_enabled"),
                         m_interaction.faceStreamAssignmentInteractionEnabled}
                    };
                    for (auto it = drift.constBegin(); it != drift.constEnd(); ++it) {
                        m_lastFacedetectionsQueryDebug.insert(it.key(), it.value());
                    }
                }
                continue;
            }

            QSize clipFrameSize;
            for (const VulkanPreviewClipFrameStatus& status : m_interaction.vulkanFrameStatuses) {
                if (status.clipId == clip.id && status.frameSize.isValid()) {
                    clipFrameSize = status.frameSize;
                    break;
                }
            }

            FacestreamOverlayRequestClip requestClip;
            requestClip.clip = clip;
            requestClip.localFrame = localFrame;
            requestClip.localSourceFrame = qMax<int64_t>(0, localFrame - qMax<int64_t>(0, clip.sourceInFrame));
            requestClip.localTimelineFrame = qMax<int64_t>(
                0,
                static_cast<int64_t>(std::floor(m_interaction.currentFramePosition)) - clip.startFrame);
            requestClip.clipFrameSize = clipFrameSize;
            requestClip.renderSyncMarkers = m_interaction.renderSyncMarkers;
            requestClip.cacheEntry = cachedEntryIt.value();
            requestClips.push_back(requestClip);
            keyParts << QStringLiteral("%1:%2:%3").arg(clip.id).arg(localFrame).arg(cachedEntryIt.value().signature);
        }

        const QString requestKey = keyParts.join(QLatin1Char('|'));
        if (requestKey == m_appliedFacestreamOverlaySnapshotKey) {
            return;
        }
        if (requestClips.isEmpty()) {
            if (playbackSuppressedColdLookup) {
                const bool preservePrevious = previousPlaybackOverlayIsCloseEnough(
                    previousOverlays,
                    previousRawDetections,
                    m_interaction.vulkanFrameStatuses.isEmpty()
                        ? -1
                        : m_interaction.vulkanFrameStatuses.constFirst().requestedSourceFrame);
                if (preservePrevious) {
                    m_interaction.facedetectionsOverlays = previousOverlays;
                    m_interaction.rawDetectionOverlays = previousRawDetections;
                } else {
                    m_interaction.facedetectionsOverlays = overlays;
                    m_interaction.rawDetectionOverlays = rawDetections;
                    m_appliedFacestreamOverlaySnapshotKey.clear();
                    m_lastFacedetectionsQueryDebug.insert(
                        QStringLiteral("status"),
                        QStringLiteral("playback_cold_overlay_cache_missing_previous_too_stale_cleared"));
                }
            } else {
                m_interaction.facedetectionsOverlays = overlays;
                m_interaction.rawDetectionOverlays = rawDetections;
            }
            return;
        }
        requestFacestreamOverlaySnapshotAsync(requestKey,
                                             requestClips,
                                             m_interaction.selectedClipId,
                                             sourceFilter,
                                             m_showSpeakerTrackBoxes,
                                             m_showRawDetections,
                                             m_interaction.faceStreamAssignmentInteractionEnabled);
        return;
    }

    bool playbackSuppressedColdLookup = false;
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
        const QString clipPath = QFileInfo(clip.filePath).absoluteFilePath();
        const auto cachedEntryIt = m_facedetectionsOverlayCache.constFind(clipPath);
        const bool useCachedPlaybackOverlay =
            m_interaction.playing && cachedEntryIt != m_facedetectionsOverlayCache.constEnd();
        QVector<FacestreamTrack> tracks;
        const FacestreamOverlayCacheEntry* overlayCacheEntry = nullptr;
        if (m_interaction.playing) {
            if (useCachedPlaybackOverlay) {
                overlayCacheEntry = &cachedEntryIt.value();
                tracks = overlayCacheEntry->tracks;
                if (clip.id == m_interaction.selectedClipId) {
                    m_lastFacedetectionsQueryDebug = QJsonObject{
                        {QStringLiteral("status"), QStringLiteral("playback_cached_overlay")},
                        {QStringLiteral("clip_id"), clip.id},
                        {QStringLiteral("playback_active"), true},
                        {QStringLiteral("cached_track_count"), tracks.size()},
                        {QStringLiteral("cached_track_index_source_frame_count"),
                         overlayCacheEntry->trackIndexSourceFrames.size()},
                        {QStringLiteral("cached_raw_detection_frame_count"),
                         overlayCacheEntry->rawDetectionSourceFrames.size()},
                        {QStringLiteral("requested_source_frame"), static_cast<qint64>(localFrame)},
                        {QStringLiteral("show_speaker_track_boxes"), m_showSpeakerTrackBoxes},
                        {QStringLiteral("show_raw_detections"), m_showRawDetections},
                        {QStringLiteral("assignment_interaction_enabled"),
                         m_interaction.faceStreamAssignmentInteractionEnabled}
                    };
                }
            } else {
                playbackSuppressedColdLookup = true;
                if (clip.id == m_interaction.selectedClipId) {
                    m_lastFacedetectionsQueryDebug = QJsonObject{
                        {QStringLiteral("status"), QStringLiteral("playback_cold_overlay_lookup_suppressed_preserving_previous")},
                        {QStringLiteral("clip_id"), clip.id},
                        {QStringLiteral("playback_active"), true},
                        {QStringLiteral("previous_overlay_count"), previousOverlays.size()},
                        {QStringLiteral("previous_raw_detection_count"), previousRawDetections.size()},
                        {QStringLiteral("requested_source_frame"), static_cast<qint64>(localFrame)},
                        {QStringLiteral("show_speaker_track_boxes"), m_showSpeakerTrackBoxes},
                        {QStringLiteral("show_raw_detections"), m_showRawDetections},
                        {QStringLiteral("assignment_interaction_enabled"),
                         m_interaction.faceStreamAssignmentInteractionEnabled}
                    };
                }
                continue;
            }
        } else {
            tracks = loadFacestreamTracksForClip(clip, localFrame);
            const auto loadedEntryIt = m_facedetectionsOverlayCache.constFind(clipPath);
            if (loadedEntryIt != m_facedetectionsOverlayCache.constEnd()) {
                overlayCacheEntry = &loadedEntryIt.value();
            }
        }
        // Keep raw detections visible when explicitly enabled, even while the
        // Speakers tab turns on assignment interaction. The UI exposes these as
        // independent preview overlays, so suppressing raw detections here
        // makes the checkbox appear broken.
        const bool showRawDetectionsForPreview = m_showRawDetections;
        if (showRawDetectionsForPreview) {
            const QVector<VulkanPreviewFacestreamOverlay> clipDetections = m_interaction.playing
                ? rawDetectionsFromCacheEntry(cachedEntryIt.value(), localFrame)
                : rawDetectionsForClipFrame(clip, localFrame);
            for (const VulkanPreviewFacestreamOverlay& detection : clipDetections) {
                if (detection.boxNorm.isValid() && !detection.boxNorm.isEmpty()) {
                    rawDetections.push_back(detection);
                }
            }
            if (clip.id == m_interaction.selectedClipId) {
                m_lastFacedetectionsQueryDebug[QStringLiteral("selected_clip_raw_detection_matches")] =
                    clipDetections.size();
            }
        }
        int selectedClipOverlayCount = 0;
        const bool needsTrackOverlays =
            m_showSpeakerTrackBoxes || m_interaction.faceStreamAssignmentInteractionEnabled;
        QVector<int> trackCandidateIndices;
        if (needsTrackOverlays) {
            trackCandidateIndices = overlayCacheEntry
                ? jcut::preview_overlay::facestreamTrackCandidateIndicesFromCacheEntry(*overlayCacheEntry, localFrame)
                : [&tracks]() {
                      QVector<int> indices;
                      indices.reserve(tracks.size());
                      for (int i = 0; i < tracks.size(); ++i) {
                          indices.push_back(i);
                      }
                      return indices;
                  }();
            for (int trackIndex : trackCandidateIndices) {
                if (trackIndex < 0 || trackIndex >= tracks.size()) {
                    continue;
                }
                const FacestreamTrack& track = tracks.at(trackIndex);
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
                overlay.boxNorm = facestreamKeyframeBoxNorm(selection.keyframe, clipFrameSize);
                if (!overlay.boxNorm.isValid() || overlay.boxNorm.isEmpty()) {
                    continue;
                }
                overlay.confidence = selection.keyframe.confidence;
                overlays.push_back(overlay);
                ++selectedClipOverlayCount;
            }
        }
        if (clip.id == m_interaction.selectedClipId) {
            m_lastFacedetectionsQueryDebug[QStringLiteral("selected_clip_source_frame")] =
                static_cast<qint64>(localFrame);
            m_lastFacedetectionsQueryDebug[QStringLiteral("selected_clip_local_source_frame")] =
                static_cast<qint64>(localSourceFrame);
            m_lastFacedetectionsQueryDebug[QStringLiteral("selected_clip_local_timeline_frame")] =
                static_cast<qint64>(localTimelineFrame);
            m_lastFacedetectionsQueryDebug[QStringLiteral("selected_clip_track_candidates")] = tracks.size();
            m_lastFacedetectionsQueryDebug[QStringLiteral("selected_clip_indexed_track_candidates")] =
                trackCandidateIndices.size();
            m_lastFacedetectionsQueryDebug[QStringLiteral("selected_clip_overlay_matches")] =
                selectedClipOverlayCount;
            m_lastFacedetectionsQueryDebug[QStringLiteral("show_speaker_track_boxes")] =
                m_showSpeakerTrackBoxes;
            m_lastFacedetectionsQueryDebug[QStringLiteral("show_raw_detections")] =
                m_showRawDetections;
            m_lastFacedetectionsQueryDebug[QStringLiteral("overlay_source_filter")] = sourceFilter;
            m_lastFacedetectionsQueryDebug[QStringLiteral("assignment_interaction_enabled")] =
                m_interaction.faceStreamAssignmentInteractionEnabled;
        }
    }
    if (m_interaction.playing && playbackSuppressedColdLookup && overlays.isEmpty() && rawDetections.isEmpty()) {
        m_interaction.facedetectionsOverlays = previousOverlays;
        m_interaction.rawDetectionOverlays = previousRawDetections;
        return;
    }
    m_interaction.facedetectionsOverlays = overlays;
    m_interaction.rawDetectionOverlays = rawDetections;
}

void VulkanPreviewSurface::requestFacestreamOverlaySnapshotAsync(
    const QString& requestKey,
    const QVector<FacestreamOverlayRequestClip>& requestClips,
    const QString& selectedClipId,
    const QString& sourceFilter,
    bool showSpeakerTrackBoxes,
    bool showRawDetections,
    bool assignmentInteractionEnabled)
{
    if (!m_pipelineOwner) {
        return;
    }
    if (m_facedetectionsOverlayWorkerPending) {
        if (requestKey != m_pendingFacestreamOverlaySnapshotKey) {
            ++m_facedetectionsOverlayWorkerCoalesced;
            m_queuedFacestreamOverlaySnapshotKey = requestKey;
            m_queuedFacestreamOverlayRequestClips = requestClips;
            m_queuedFacestreamOverlaySelectedClipId = selectedClipId;
            m_queuedFacestreamOverlaySourceFilter = sourceFilter;
            m_queuedFacestreamOverlayShowSpeakerTrackBoxes = showSpeakerTrackBoxes;
            m_queuedFacestreamOverlayShowRawDetections = showRawDetections;
            m_queuedFacestreamOverlayAssignmentInteractionEnabled = assignmentInteractionEnabled;
        } else {
            m_queuedFacestreamOverlaySnapshotKey.clear();
            m_queuedFacestreamOverlayRequestClips.clear();
            m_queuedFacestreamOverlaySelectedClipId.clear();
            m_queuedFacestreamOverlaySourceFilter.clear();
            m_queuedFacestreamOverlayShowSpeakerTrackBoxes = false;
            m_queuedFacestreamOverlayShowRawDetections = false;
            m_queuedFacestreamOverlayAssignmentInteractionEnabled = false;
        }
        return;
    }

    startFacestreamOverlaySnapshotWorker(requestKey,
                                        requestClips,
                                        selectedClipId,
                                        sourceFilter,
                                        showSpeakerTrackBoxes,
                                        showRawDetections,
                                        assignmentInteractionEnabled);
}

void VulkanPreviewSurface::startFacestreamOverlaySnapshotWorker(
    const QString& requestKey,
    const QVector<FacestreamOverlayRequestClip>& requestClips,
    const QString& selectedClipId,
    const QString& sourceFilter,
    bool showSpeakerTrackBoxes,
    bool showRawDetections,
    bool assignmentInteractionEnabled)
{
    if (!m_pipelineOwner || requestClips.isEmpty()) {
        return;
    }
    m_facedetectionsOverlayWorkerPending = true;
    m_pendingFacestreamOverlaySnapshotKey = requestKey;
    m_lastFacedetectionsOverlayQueuedAtMs = QDateTime::currentMSecsSinceEpoch();
    m_lastFacedetectionsOverlayRequestClipCount = requestClips.size();
    ++m_facedetectionsOverlayWorkerStarted;
    const uint64_t requestId = ++m_nextFacestreamOverlayRequestId;
    const int64_t currentSample = m_interaction.currentSample;
    const int64_t currentFrame = m_interaction.currentFrame;
    const qint64 queuedAtMs = m_lastFacedetectionsOverlayQueuedAtMs;
    const QPointer<QObject> receiver(m_pipelineOwner.get());

    (void)QtConcurrent::run([receiver,
                             this,
                             requestId,
                             requestKey,
                             requestClips,
                             selectedClipId,
                             sourceFilter,
                             showSpeakerTrackBoxes,
                             showRawDetections,
                             assignmentInteractionEnabled,
                             currentSample,
                             currentFrame,
                             queuedAtMs]() {
        const FacestreamOverlaySnapshot snapshot =
            jcut::preview_overlay::buildFacestreamOverlaySnapshot(
                jcut::preview_overlay::FacestreamOverlaySnapshotRequest{
                    requestId,
                    requestKey,
                    currentSample,
                    currentFrame,
                    requestClips,
                    selectedClipId,
                    sourceFilter,
                    showSpeakerTrackBoxes,
                    showRawDetections,
                    assignmentInteractionEnabled});

        if (!receiver) {
            return;
        }
        QMetaObject::invokeMethod(
            receiver,
            [this, snapshot, queuedAtMs]() {
                m_lastFacedetectionsOverlayApplyLatencyMs =
                    qMax<qint64>(0, QDateTime::currentMSecsSinceEpoch() - queuedAtMs);
                applyFacestreamOverlaySnapshot(snapshot);
            },
            Qt::QueuedConnection);
    });
}

void VulkanPreviewSurface::applyFacestreamOverlaySnapshot(const FacestreamOverlaySnapshot& snapshot)
{
    const QString expectedKey = m_pendingFacestreamOverlaySnapshotKey;
    m_facedetectionsOverlayWorkerPending = false;
    m_pendingFacestreamOverlaySnapshotKey.clear();
    m_lastFacedetectionsOverlayPrepMs = snapshot.prepMs;
    m_lastFacedetectionsOverlayRequestClipCount = snapshot.requestClipCount;
    m_lastFacedetectionsOverlayTrackCandidateCount = snapshot.trackCandidateCount;
    m_lastFacedetectionsOverlayMatchCount = snapshot.overlayMatchCount;
    m_lastFacedetectionsRawDetectionMatchCount = snapshot.rawDetectionMatchCount;

    auto launchQueuedRequest = [this]() {
        if (!m_interaction.playing ||
            m_queuedFacestreamOverlaySnapshotKey.isEmpty() ||
            m_queuedFacestreamOverlayRequestClips.isEmpty()) {
            if (!m_interaction.playing) {
                m_queuedFacestreamOverlaySnapshotKey.clear();
                m_queuedFacestreamOverlayRequestClips.clear();
                m_queuedFacestreamOverlaySelectedClipId.clear();
                m_queuedFacestreamOverlaySourceFilter.clear();
                m_queuedFacestreamOverlayShowSpeakerTrackBoxes = false;
                m_queuedFacestreamOverlayShowRawDetections = false;
                m_queuedFacestreamOverlayAssignmentInteractionEnabled = false;
            }
            return;
        }
        const QString queuedKey = m_queuedFacestreamOverlaySnapshotKey;
        const QVector<FacestreamOverlayRequestClip> queuedClips = m_queuedFacestreamOverlayRequestClips;
        const QString queuedSelectedClipId = m_queuedFacestreamOverlaySelectedClipId;
        const QString queuedSourceFilter = m_queuedFacestreamOverlaySourceFilter;
        const bool queuedShowSpeakerTrackBoxes = m_queuedFacestreamOverlayShowSpeakerTrackBoxes;
        const bool queuedShowRawDetections = m_queuedFacestreamOverlayShowRawDetections;
        const bool queuedAssignmentInteractionEnabled =
            m_queuedFacestreamOverlayAssignmentInteractionEnabled;
        m_queuedFacestreamOverlaySnapshotKey.clear();
        m_queuedFacestreamOverlayRequestClips.clear();
        m_queuedFacestreamOverlaySelectedClipId.clear();
        m_queuedFacestreamOverlaySourceFilter.clear();
        m_queuedFacestreamOverlayShowSpeakerTrackBoxes = false;
        m_queuedFacestreamOverlayShowRawDetections = false;
        m_queuedFacestreamOverlayAssignmentInteractionEnabled = false;
        startFacestreamOverlaySnapshotWorker(queuedKey,
                                            queuedClips,
                                            queuedSelectedClipId,
                                            queuedSourceFilter,
                                            queuedShowSpeakerTrackBoxes,
                                            queuedShowRawDetections,
                                            queuedAssignmentInteractionEnabled);
    };

    const jcut::preview_overlay::FacestreamOverlaySnapshotApplyDecision applyDecision =
        jcut::preview_overlay::facestreamOverlaySnapshotApplyDecision(
            m_interaction.playing,
            snapshot.requestKey,
            expectedKey,
            snapshot.requestId,
            m_latestAppliedFacestreamOverlayRequestId);
    if (applyDecision != jcut::preview_overlay::FacestreamOverlaySnapshotApplyDecision::Apply) {
        ++m_facedetectionsOverlayWorkerDropped;
        launchQueuedRequest();
        return;
    }
    m_latestAppliedFacestreamOverlayRequestId = snapshot.requestId;
    m_appliedFacestreamOverlaySnapshotKey = snapshot.requestKey;
    m_interaction.facedetectionsOverlays = snapshot.overlays;
    m_interaction.rawDetectionOverlays = snapshot.rawDetections;
    m_lastFacedetectionsQueryDebug = snapshot.debug;
    m_lastFacedetectionsQueryDebug.insert(QStringLiteral("overlay_preparation_thread"),
                                          QStringLiteral("worker"));
    m_lastFacedetectionsQueryDebug.insert(QStringLiteral("overlay_apply_latency_ms"),
                                          m_lastFacedetectionsOverlayApplyLatencyMs);
    m_lastFacedetectionsOverlayAppliedAtMs = QDateTime::currentMSecsSinceEpoch();
    ++m_facedetectionsOverlayWorkerApplied;
    requestNativeUpdate();
    if (!m_queuedFacestreamOverlaySnapshotKey.isEmpty()) {
        launchQueuedRequest();
    } else if (m_interaction.playing && snapshot.currentSample != m_interaction.currentSample) {
        refreshFacestreamOverlays();
    }
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
