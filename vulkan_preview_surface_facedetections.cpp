#include "vulkan_preview_surface.h"

#include "facedetections_artifact_utils.h"
#include "facedetections_debug.h"
#include "facedetections_runtime.h"
#include "facedetections_time_mapping.h"
#include "editor_shared_timing.h"
#include "transcript_engine.h"

#include <QFileInfo>
#include <QJsonArray>
#include <QJsonObject>
#include <QElapsedTimer>
#include <QDateTime>
#include <QMetaObject>
#include <QPointer>
#include <QSet>
#include <QTimer>
#include <QtConcurrent/QtConcurrentRun>

#include <algorithm>
#include <cmath>
#include <limits>

namespace {
constexpr qint64 kFacestreamOverlaySlowQueryWarnMs = 100;
constexpr int64_t kFacestreamOverlayInteractiveWindowFrames = 240;
constexpr int64_t kFacestreamOverlayPlaybackWarmAheadFrames =
    kFacestreamOverlayInteractiveWindowFrames * 2;
constexpr int64_t kMaxPreservedPlaybackOverlayDriftFrames =
    kFacestreamOverlayInteractiveWindowFrames * 2;

struct FacestreamOverlayCacheIdentity {
    QString clipPath;
    QString transcriptPath;
    qint64 transcriptModifiedMs = 0;
    qint64 artifactRevisionMs = -1;
    quint64 renderSyncSignature = 0;
    int64_t bucket = 0;
    int64_t querySourceFrame = 0;
    QString signature;
    QString cacheKey;
    bool transcriptExists = false;
};

quint64 renderSyncMarkerOverlaySignature(const QVector<RenderSyncMarker>& markers);

FacestreamOverlayCacheIdentity facestreamOverlayCacheIdentity(
    const TimelineClip& clip,
    int64_t sourceFrame,
    const QString& sourceFilter,
    const QVector<RenderSyncMarker>& renderSyncMarkers)
{
    FacestreamOverlayCacheIdentity identity;
    identity.bucket = qMax<int64_t>(0, sourceFrame) / kFacestreamOverlayInteractiveWindowFrames;
    identity.querySourceFrame = identity.bucket * kFacestreamOverlayInteractiveWindowFrames;
    identity.clipPath = QFileInfo(clip.filePath).absoluteFilePath();
    identity.transcriptPath = activeTranscriptPathForClip(clip);
    const QFileInfo transcriptInfo(identity.transcriptPath);
    identity.transcriptExists = transcriptInfo.exists() && transcriptInfo.isFile();
    identity.transcriptPath = transcriptInfo.absoluteFilePath();
    identity.transcriptModifiedMs =
        identity.transcriptExists ? transcriptInfo.lastModified().toMSecsSinceEpoch() : 0;
    identity.artifactRevisionMs =
        facedetectionsArtifactRevisionMsForTranscript(identity.transcriptPath);
    identity.renderSyncSignature = renderSyncMarkerOverlaySignature(renderSyncMarkers);
    identity.signature = QStringLiteral("%1|%2|%3|%4|%5|bucket:%6|sync:%7")
                             .arg(clip.id)
                             .arg(identity.transcriptPath)
                             .arg(identity.transcriptModifiedMs)
                             .arg(identity.artifactRevisionMs)
                             .arg(sourceFilter)
                             .arg(identity.bucket)
                             .arg(identity.renderSyncSignature);
    identity.cacheKey = identity.clipPath + QLatin1Char('|') + identity.signature;
    return identity;
}

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
    const int64_t clipEndSample = clipTimelineEndSamples(clip);
    return clipVisualPlaybackEnabled(clip, tracks) &&
           samplePosition >= clipStartSample &&
           samplePosition < clipEndSample &&
           editor::clipIsActiveAtTimelineFrame(clip, tracks, framePosition, bypassGrading);
}

bool clipSupportsDrawableTranscriptOverlayForSelection(const TimelineClip& clip,
                                                       int64_t currentSample,
                                                       const QVector<RenderSyncMarker>& renderSyncMarkers,
                                                       const TranscriptOverlayTiming& timing)
{
    if (!((clip.mediaType == ClipMediaType::Audio) || clip.hasAudio) || !clip.transcriptOverlay.enabled) {
        return false;
    }
    const QString transcriptPath = activeTranscriptPathForClip(clip);
    const std::shared_ptr<const TranscriptRuntimeDocument> runtimeDocument =
        loadTranscriptRuntimeDocument(transcriptPath);
    const QVector<TranscriptSection>& sections =
        runtimeDocument ? runtimeDocument->sections : QVector<TranscriptSection>{};
    const int64_t sourceFrame = transcriptFrameForClipAtTimelineSample(clip, currentSample, renderSyncMarkers);
    const TranscriptOverlayLayout layout = transcriptOverlayLayoutAtSourceFrame(clip, sections, sourceFrame, timing);
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
        std::sort(track.keyframes.begin(), track.keyframes.end(), [](const FacestreamKeyframe& a, const FacestreamKeyframe& b) {
            return a.frame < b.frame;
        });
        QVector<int64_t> sortedFrames;
        sortedFrames.reserve(track.keyframes.size());
        for (const FacestreamKeyframe& keyframe : track.keyframes) {
            sortedFrames.push_back(keyframe.frame);
        }
        if (!parseFacestreamFrameDomainString(
                streamObj.value(QStringLiteral("frame_domain")).toString().trimmed(),
                &track.frameDomain)) {
            track.frameDomain =
                hasFallbackFrameDomain
                    ? fallbackFrameDomain
                    : inferFacestreamFrameDomain(
                          clip,
                          sortedFrames.isEmpty() ? static_cast<int64_t>(-1) : sortedFrames.constFirst(),
                          sortedFrames.isEmpty() ? static_cast<int64_t>(-1) : sortedFrames.constLast());
        }
        if (!isSourceMediaFacestreamFrameDomain(track.frameDomain)) {
            continue;
        }
        track.typicalFrameStep = facedetectionsTypicalFrameStep(sortedFrames);
        if (!track.keyframes.isEmpty()) {
            tracks.push_back(track);
        }
    }
    return tracks;
}

QVector<VulkanPreviewSurface::FacestreamTrack> VulkanPreviewSurface::convertContinuityTrackModelsForClip(
    const TimelineClip& clip,
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
        track.typicalFrameStep = qMax<int64_t>(1, model.summary.typicalFrameStep);
        track.keyframes.reserve(model.keyframes.size());
        QVector<int64_t> sortedFrames;
        sortedFrames.reserve(model.keyframes.size());
        for (const jcut::facedetections::FacestreamKeyframe& modelKeyframe : model.keyframes) {
            FacestreamKeyframe keyframe;
            keyframe.frame = modelKeyframe.frame;
            keyframe.xNorm = qBound<qreal>(0.0, modelKeyframe.x, 1.0);
            keyframe.yNorm = qBound<qreal>(0.0, modelKeyframe.y, 1.0);
            keyframe.boxSizeNorm = qBound<qreal>(0.001, modelKeyframe.box, 1.0);
            keyframe.boxNorm = modelKeyframe.boxNorm;
            keyframe.hasCenterBox = !keyframe.boxNorm.isValid() || keyframe.boxNorm.isEmpty();
            keyframe.confidence = qBound<qreal>(0.0, modelKeyframe.confidence, 1.0);
            keyframe.source = track.source;
            if (keyframe.hasCenterBox || (keyframe.boxNorm.isValid() && !keyframe.boxNorm.isEmpty())) {
                track.keyframes.push_back(keyframe);
            }
            sortedFrames.push_back(keyframe.frame);
        }
        std::sort(sortedFrames.begin(), sortedFrames.end());
        track.frameDomain = hasFrameDomain
            ? parsedFrameDomain
            : inferFacestreamFrameDomain(
                  clip,
                  sortedFrames.isEmpty() ? static_cast<int64_t>(-1) : sortedFrames.constFirst(),
                  sortedFrames.isEmpty() ? static_cast<int64_t>(-1) : sortedFrames.constLast());
        if (!isSourceMediaFacestreamFrameDomain(track.frameDomain)) {
            continue;
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
    const QString sourceFilter = normalizedFacestreamOverlaySource(m_facedetectionsOverlaySource);
    const FacestreamOverlayCacheIdentity cacheIdentity =
        facestreamOverlayCacheIdentity(clip, sourceFrame, sourceFilter, m_interaction.renderSyncMarkers);
    FacestreamOverlayCacheEntry& entry = m_facedetectionsOverlayCache[cacheIdentity.cacheKey];
    if (entry.signature == cacheIdentity.signature) {
        if (clip.id == m_interaction.selectedClipId) {
            jcut::facedetections::debugLogJson(
                QStringLiteral("FACESTREAM OVERLAY LOAD"),
                QJsonObject{
                    {QStringLiteral("status"), QStringLiteral("cache_hit")},
                    {QStringLiteral("clip_id"), clip.id},
                    {QStringLiteral("requested_source_frame"), static_cast<qint64>(sourceFrame)},
                    {QStringLiteral("source_filter"), sourceFilter},
                    {QStringLiteral("cache_bucket"), static_cast<qint64>(cacheIdentity.bucket)},
                    {QStringLiteral("cache_query_source_frame"),
                     static_cast<qint64>(cacheIdentity.querySourceFrame)},
                    {QStringLiteral("cached_track_count"), entry.tracks.size()},
                    {QStringLiteral("cached_raw_detection_count"), entry.rawDetections.size()},
                    {QStringLiteral("track_index_source_frame_count"),
                     entry.trackIndexSourceFrames.size()},
                    {QStringLiteral("track_index_typical_frame_step"),
                     static_cast<qint64>(entry.trackIndexTypicalFrameStep)},
                    {QStringLiteral("signature"), cacheIdentity.signature}
                });
        }
        return entry.tracks;
    }

    entry = FacestreamOverlayCacheEntry{};
    entry.signature = cacheIdentity.signature;
    if (!cacheIdentity.transcriptExists) {
        if (clip.id == m_interaction.selectedClipId) {
            m_lastFacedetectionsQueryDebug = QJsonObject{
                {QStringLiteral("status"), QStringLiteral("missing_transcript")},
                {QStringLiteral("clip_id"), clip.id},
                {QStringLiteral("transcript_path"), cacheIdentity.transcriptPath},
                {QStringLiteral("requested_source_frame"), static_cast<qint64>(sourceFrame)}
            };
            jcut::facedetections::debugLogJson(
                QStringLiteral("FACESTREAM OVERLAY LOAD"), m_lastFacedetectionsQueryDebug);
        }
        return entry.tracks;
    }

    editor::TranscriptEngine engine;
    QJsonObject queryDebug{
        {QStringLiteral("status"), QStringLiteral("ok")},
        {QStringLiteral("clip_id"), clip.id},
        {QStringLiteral("transcript_path"), cacheIdentity.transcriptPath},
        {QStringLiteral("requested_source_frame"), static_cast<qint64>(sourceFrame)},
        {QStringLiteral("cache_bucket"), static_cast<qint64>(cacheIdentity.bucket)},
        {QStringLiteral("cache_query_source_frame"), static_cast<qint64>(cacheIdentity.querySourceFrame)},
        {QStringLiteral("cache_window_frames"), static_cast<qint64>(kFacestreamOverlayInteractiveWindowFrames)},
        {QStringLiteral("artifact_revision_ms"), static_cast<qint64>(cacheIdentity.artifactRevisionMs)},
        {QStringLiteral("signature"), cacheIdentity.signature},
        {QStringLiteral("cache_key"), cacheIdentity.cacheKey}
    };
    const QString artifactRootCacheKey = QStringLiteral("%1|%2|%3")
                                             .arg(cacheIdentity.transcriptPath)
                                             .arg(cacheIdentity.transcriptModifiedMs)
                                             .arg(cacheIdentity.artifactRevisionMs);
    QJsonObject processedArtifactRoot =
        m_facedetectionsProcessedArtifactRootCache.value(artifactRootCacheKey);
    if (processedArtifactRoot.isEmpty() &&
        engine.loadFacestreamProcessedArtifact(cacheIdentity.transcriptPath, &processedArtifactRoot)) {
        m_facedetectionsProcessedArtifactRootCache.insert(artifactRootCacheKey, processedArtifactRoot);
    }
    if (!processedArtifactRoot.isEmpty()) {
        const QJsonObject continuityRoot = continuityRootForClip(processedArtifactRoot, clip.id);
        const jcut::facedetections::ArtifactCompatibilityResult compatibility =
            jcut::facedetections::validateArtifactCompatibilityForClip(continuityRoot, clip);
        queryDebug[QStringLiteral("processed_artifact_compatibility")] = compatibility.details;
        if (!compatibility.compatible) {
            queryDebug[QStringLiteral("status")] = QStringLiteral("artifact_incompatible");
            queryDebug[QStringLiteral("processed_artifact_incompatible")] = true;
        } else {
        QJsonArray streams = jcut::facedetections::storedContinuityStreamsForRoot(continuityRoot);
        const bool hasAuthoritativeStreams = continuityRoot.contains(QStringLiteral("streams"));
        queryDebug[QStringLiteral("processed_continuity_root_found")] = !continuityRoot.isEmpty();
        queryDebug[QStringLiteral("processed_stored_stream_count")] = streams.size();
        queryDebug[QStringLiteral("processed_streams_authoritative")] = hasAuthoritativeStreams;
        queryDebug[QStringLiteral("processed_raw_tracks_artifact_path")] =
            continuityRoot.value(QStringLiteral("raw_tracks_artifact_path")).toString().trimmed();
        queryDebug[QStringLiteral("processed_raw_frames_artifact_path")] =
            continuityRoot.value(QStringLiteral("raw_frames_artifact_path")).toString().trimmed();
        if (streams.isEmpty() && !hasAuthoritativeStreams) {
            const QVector<jcut::facedetections::FacestreamTrack> trackModels =
                jcut::facedetections::continuityTrackModelsNearFrameForRoot(
                continuityRoot,
                cacheIdentity.querySourceFrame,
                kFacestreamOverlayInteractiveWindowFrames,
                    QJsonObject{});
            entry.tracks += convertContinuityTrackModelsForClip(
                clip,
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
    }
    QJsonObject artifactRoot = m_facedetectionsArtifactRootCache.value(artifactRootCacheKey);
    if (artifactRoot.isEmpty() && engine.loadFacestreamArtifact(cacheIdentity.transcriptPath, &artifactRoot)) {
        m_facedetectionsArtifactRootCache.insert(artifactRootCacheKey, artifactRoot);
    }
    if (!artifactRoot.isEmpty()) {
        if (entry.tracks.isEmpty()) {
            const QJsonObject continuityRoot = continuityRootForClip(artifactRoot, clip.id);
            const jcut::facedetections::ArtifactCompatibilityResult compatibility =
                jcut::facedetections::validateArtifactCompatibilityForClip(continuityRoot, clip);
            queryDebug[QStringLiteral("raw_artifact_compatibility")] = compatibility.details;
            if (!compatibility.compatible) {
                queryDebug[QStringLiteral("status")] = QStringLiteral("artifact_incompatible");
                queryDebug[QStringLiteral("raw_artifact_incompatible")] = true;
            } else {
            QJsonArray streams = jcut::facedetections::storedContinuityStreamsForRoot(continuityRoot);
            const bool hasAuthoritativeStreams = continuityRoot.contains(QStringLiteral("streams"));
            queryDebug[QStringLiteral("raw_continuity_root_found")] = !continuityRoot.isEmpty();
            queryDebug[QStringLiteral("raw_stored_stream_count")] = streams.size();
            queryDebug[QStringLiteral("raw_streams_authoritative")] = hasAuthoritativeStreams;
            queryDebug[QStringLiteral("raw_raw_tracks_artifact_path")] =
                continuityRoot.value(QStringLiteral("raw_tracks_artifact_path")).toString().trimmed();
            queryDebug[QStringLiteral("raw_raw_frames_artifact_path")] =
                continuityRoot.value(QStringLiteral("raw_frames_artifact_path")).toString().trimmed();
            queryDebug[QStringLiteral("raw_tracks_inline_count")] =
                continuityRoot.value(QStringLiteral("raw_tracks")).toArray().size();
            queryDebug[QStringLiteral("raw_frames_inline_count")] =
                continuityRoot.value(QStringLiteral("raw_frames")).toArray().size();
            if (streams.isEmpty() && !hasAuthoritativeStreams) {
                const QVector<jcut::facedetections::FacestreamTrack> trackModels =
                    jcut::facedetections::continuityTrackModelsNearFrameForRoot(
                    continuityRoot,
                    cacheIdentity.querySourceFrame,
                    kFacestreamOverlayInteractiveWindowFrames,
                        QJsonObject{});
                entry.tracks += convertContinuityTrackModelsForClip(
                    clip,
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
        }
        const QJsonObject rawDetectionContinuityRoot = continuityRootForClip(artifactRoot, clip.id);
        const jcut::facedetections::ArtifactCompatibilityResult rawDetectionCompatibility =
            jcut::facedetections::validateArtifactCompatibilityForClip(rawDetectionContinuityRoot, clip);
        queryDebug[QStringLiteral("raw_detection_artifact_compatibility")] =
            rawDetectionCompatibility.details;
        if (rawDetectionCompatibility.compatible) {
        FacestreamFrameDomain rawFrameDomain = FacestreamFrameDomain::SourceRelative;
        if (continuityPayloadFrameDomain(
                rawDetectionContinuityRoot,
                QStringLiteral("raw_frames_frame_domain"),
                &rawFrameDomain) &&
            isSourceMediaFacestreamFrameDomain(rawFrameDomain)) {
            const QVector<jcut::facedetections::FacestreamFrameDetections> rawFrameModels =
                jcut::facedetections::frameDetectionModelsNearFrameForRoot(
                    rawDetectionContinuityRoot,
                    cacheIdentity.querySourceFrame,
                    kFacestreamOverlayInteractiveWindowFrames);
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
        } else {
            queryDebug[QStringLiteral("raw_detection_artifact_incompatible")] = true;
        }
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
        jcut::facedetections::debugLogJson(
            QStringLiteral("FACESTREAM OVERLAY LOAD"), queryDebug);
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
    const QString sourceFilter = normalizedFacestreamOverlaySource(m_facedetectionsOverlaySource);
    const FacestreamOverlayCacheIdentity cacheIdentity =
        facestreamOverlayCacheIdentity(clip, sourceFrame, sourceFilter, m_interaction.renderSyncMarkers);
    const auto entryIt = m_facedetectionsOverlayCache.constFind(cacheIdentity.cacheKey);
    if (entryIt == m_facedetectionsOverlayCache.constEnd()) {
        return {};
    }
    const FacestreamOverlayCacheEntry& entry = entryIt.value();
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
    const bool overlayRequested =
        m_showSpeakerTrackBoxes || m_interaction.faceStreamAssignmentInteractionEnabled || m_showRawDetections;
    QVector<VulkanPreviewFacestreamOverlay> overlays;
    QVector<VulkanPreviewFacestreamOverlay> rawDetections;
    if (!m_showSpeakerTrackBoxes && !m_interaction.faceStreamAssignmentInteractionEnabled && !m_showRawDetections) {
        editor::accumulatePlaybackStageMetric(&m_overlayPrepStageMetric,
                                      1,
                                      1,
                                      0,
                                      QStringLiteral("overlay_disabled"),
                                      QStringLiteral("no_overlay_modes_enabled"));
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
        int64_t playbackSuppressedColdLookupSourceFrame = -1;
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
            const FacestreamOverlayCacheIdentity cacheIdentity =
                facestreamOverlayCacheIdentity(
                    clip, localFrame, sourceFilter, m_interaction.renderSyncMarkers);
            const auto cachedEntryIt = m_facedetectionsOverlayCache.constFind(cacheIdentity.cacheKey);
            const FacestreamOverlayCacheEntry* overlayCacheEntry =
                cachedEntryIt == m_facedetectionsOverlayCache.constEnd() ? nullptr : &cachedEntryIt.value();
            for (int64_t warmOffset = kFacestreamOverlayInteractiveWindowFrames;
                 warmOffset <= kFacestreamOverlayPlaybackWarmAheadFrames;
                 warmOffset += kFacestreamOverlayInteractiveWindowFrames) {
                const int64_t futureSample =
                    m_interaction.currentSample + frameToSamples(warmOffset);
                const qreal futureFramePosition = samplesToFramePosition(futureSample);
                if (!visualClipActiveAtSample(clip,
                                              m_interaction.tracks,
                                              futureSample,
                                              futureFramePosition,
                                              m_bypassGrading)) {
                    continue;
                }
                const int64_t futureLocalFrame = sourceFrameForSample(clip, futureSample);
                const FacestreamOverlayCacheIdentity futureIdentity =
                    facestreamOverlayCacheIdentity(
                        clip, futureLocalFrame, sourceFilter, m_interaction.renderSyncMarkers);
                if (!m_facedetectionsOverlayCache.contains(futureIdentity.cacheKey)) {
                    queueFacestreamOverlayCacheWarmup(clip, futureLocalFrame, futureIdentity.cacheKey);
                }
            }
            if (!overlayCacheEntry) {
                playbackSuppressedColdLookup = true;
                playbackSuppressedColdLookupSourceFrame = localFrame;
                queueFacestreamOverlayCacheWarmup(clip, localFrame, cacheIdentity.cacheKey);
                if (clip.id == m_interaction.selectedClipId) {
                    QJsonObject drift = overlayDriftDebug(previousOverlays, previousRawDetections, localFrame);
                    m_lastFacedetectionsQueryDebug = QJsonObject{
                        {QStringLiteral("status"), QStringLiteral("playback_cold_overlay_cache_missing_single_warmup")},
                        {QStringLiteral("clip_id"), clip.id},
                        {QStringLiteral("playback_active"), true},
                        {QStringLiteral("previous_overlay_count"), previousOverlays.size()},
                        {QStringLiteral("previous_raw_detection_count"), previousRawDetections.size()},
                        {QStringLiteral("requested_source_frame"), static_cast<qint64>(localFrame)},
                        {QStringLiteral("cache_bucket"), static_cast<qint64>(cacheIdentity.bucket)},
                        {QStringLiteral("cache_query_source_frame"),
                         static_cast<qint64>(cacheIdentity.querySourceFrame)},
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
            requestClip.cacheEntry = *overlayCacheEntry;
            requestClips.push_back(requestClip);
            keyParts << QStringLiteral("%1:%2:%3").arg(clip.id).arg(localFrame).arg(cacheIdentity.cacheKey);
            if (clip.id == m_interaction.selectedClipId) {
                QJsonObject playbackDebug{
                    {QStringLiteral("status"), QStringLiteral("playback_overlay_request_clip")},
                    {QStringLiteral("clip_id"), clip.id},
                    {QStringLiteral("requested_source_frame"), static_cast<qint64>(localFrame)},
                    {QStringLiteral("exact_playback_cache_entry"), true},
                    {QStringLiteral("local_source_frame"), static_cast<qint64>(requestClip.localSourceFrame)},
                    {QStringLiteral("local_timeline_frame"), static_cast<qint64>(requestClip.localTimelineFrame)},
                    {QStringLiteral("cached_track_count"), overlayCacheEntry->tracks.size()},
                    {QStringLiteral("cached_raw_detection_count"), overlayCacheEntry->rawDetections.size()},
                    {QStringLiteral("track_index_source_frame_count"),
                     overlayCacheEntry->trackIndexSourceFrames.size()},
                    {QStringLiteral("track_index_typical_frame_step"),
                     static_cast<qint64>(overlayCacheEntry->trackIndexTypicalFrameStep)},
                    {QStringLiteral("show_speaker_track_boxes"), m_showSpeakerTrackBoxes},
                    {QStringLiteral("show_raw_detections"), m_showRawDetections},
                    {QStringLiteral("assignment_interaction_enabled"),
                     m_interaction.faceStreamAssignmentInteractionEnabled}
                };
                m_lastFacedetectionsQueryDebug = playbackDebug;
                jcut::facedetections::debugLogJson(
                    QStringLiteral("FACESTREAM OVERLAY SELECT"), playbackDebug);
            }
        }

        const QString requestKey = keyParts.join(QLatin1Char('|'));
        if (requestKey == m_appliedFacestreamOverlaySnapshotKey) {
            editor::accumulatePlaybackStageMetric(&m_overlayPrepStageMetric,
                                          1,
                                          1,
                                          0,
                                          QStringLiteral("overlay_cached"),
                                          QStringLiteral("playback_snapshot_reused"));
            return;
        }
        if (requestClips.isEmpty()) {
            if (playbackSuppressedColdLookup) {
                const bool preservePrevious = previousPlaybackOverlayIsCloseEnough(
                    previousOverlays,
                    previousRawDetections,
                    playbackSuppressedColdLookupSourceFrame);
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
                    jcut::facedetections::debugLogJson(
                        QStringLiteral("FACESTREAM OVERLAY SELECT"),
                        m_lastFacedetectionsQueryDebug);
                }
            } else {
                m_interaction.facedetectionsOverlays = overlays;
                m_interaction.rawDetectionOverlays = rawDetections;
            }
            editor::accumulatePlaybackStageMetric(&m_overlayPrepStageMetric,
                                          1,
                                          0,
                                          1,
                                          QStringLiteral("source_unavailable"),
                                          playbackSuppressedColdLookup
                                              ? QStringLiteral("playback_cold_lookup_suppressed")
                                              : QStringLiteral("no_overlay_request_clips"));
            return;
        }
        requestFacestreamOverlaySnapshotAsync(requestKey,
                                             requestClips,
                                             m_interaction.selectedClipId,
                                             sourceFilter,
                                             m_showSpeakerTrackBoxes,
                                             m_showRawDetections,
                                             m_interaction.faceStreamAssignmentInteractionEnabled);
        editor::accumulatePlaybackStageMetric(&m_overlayPrepStageMetric,
                                      1,
                                      1,
                                      0,
                                      QStringLiteral("overlay_worker_requested"),
                                      QStringLiteral("playback_async_prepare"));
        return;
    }

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
        const FacestreamOverlayCacheIdentity cacheIdentity =
            facestreamOverlayCacheIdentity(clip, localFrame, sourceFilter, m_interaction.renderSyncMarkers);
        const auto cachedEntryIt = m_facedetectionsOverlayCache.constFind(cacheIdentity.cacheKey);
        QVector<FacestreamTrack> tracks = loadFacestreamTracksForClip(clip, localFrame);
        const FacestreamOverlayCacheEntry* overlayCacheEntry = nullptr;
        const auto loadedEntryIt = m_facedetectionsOverlayCache.constFind(cacheIdentity.cacheKey);
        if (loadedEntryIt != m_facedetectionsOverlayCache.constEnd()) {
            overlayCacheEntry = &loadedEntryIt.value();
        } else if (cachedEntryIt != m_facedetectionsOverlayCache.constEnd()) {
            overlayCacheEntry = &cachedEntryIt.value();
        }
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
            if (clip.id == m_interaction.selectedClipId) {
                m_lastFacedetectionsQueryDebug[QStringLiteral("selected_clip_raw_detection_matches")] =
                    clipDetections.size();
            }
        }
        int selectedClipOverlayCount = 0;
        const bool needsTrackOverlays =
            m_showSpeakerTrackBoxes || m_interaction.faceStreamAssignmentInteractionEnabled;
        QVector<int> trackCandidateIndices;
        QJsonArray selectedClipCandidateTrackIds;
        QJsonArray selectedClipOverlayTrackIds;
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
                if (clip.id == m_interaction.selectedClipId) {
                    selectedClipCandidateTrackIds.push_back(track.trackId);
                }
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
                if (clip.id == m_interaction.selectedClipId) {
                    selectedClipOverlayTrackIds.push_back(track.trackId);
                }
            }
        }
        if (clip.id == m_interaction.selectedClipId) {
            m_lastFacedetectionsQueryDebug[QStringLiteral("selected_clip_source_frame")] =
                static_cast<qint64>(localFrame);
            m_lastFacedetectionsQueryDebug[QStringLiteral("selected_clip_local_source_frame")] =
                static_cast<qint64>(localSourceFrame);
            m_lastFacedetectionsQueryDebug[QStringLiteral("selected_clip_local_timeline_frame")] =
                static_cast<qint64>(localTimelineFrame);
            m_lastFacedetectionsQueryDebug[QStringLiteral("selected_clip_track_candidates")] =
                trackCandidateIndices.size();
            m_lastFacedetectionsQueryDebug[QStringLiteral("selected_clip_candidate_track_ids")] =
                selectedClipCandidateTrackIds;
            m_lastFacedetectionsQueryDebug[QStringLiteral("selected_clip_indexed_track_candidates")] =
                trackCandidateIndices.size();
            m_lastFacedetectionsQueryDebug[QStringLiteral("selected_clip_overlay_matches")] =
                selectedClipOverlayCount;
            m_lastFacedetectionsQueryDebug[QStringLiteral("selected_clip_overlay_track_ids")] =
                selectedClipOverlayTrackIds;
            m_lastFacedetectionsQueryDebug[QStringLiteral("show_speaker_track_boxes")] =
                m_showSpeakerTrackBoxes;
            m_lastFacedetectionsQueryDebug[QStringLiteral("show_raw_detections")] =
                m_showRawDetections;
            m_lastFacedetectionsQueryDebug[QStringLiteral("overlay_source_filter")] = sourceFilter;
            m_lastFacedetectionsQueryDebug[QStringLiteral("assignment_interaction_enabled")] =
                m_interaction.faceStreamAssignmentInteractionEnabled;
            jcut::facedetections::debugLogJson(
                QStringLiteral("FACESTREAM OVERLAY SELECT"),
                m_lastFacedetectionsQueryDebug);
        }
    }
    m_interaction.facedetectionsOverlays = overlays;
    m_interaction.rawDetectionOverlays = rawDetections;
    editor::accumulatePlaybackStageMetric(&m_overlayPrepStageMetric,
                                  1,
                                  (!overlays.isEmpty() || !rawDetections.isEmpty()) ? 1 : 0,
                                  overlayRequested && overlays.isEmpty() && rawDetections.isEmpty() ? 1 : 0,
                                  (!overlays.isEmpty() || !rawDetections.isEmpty())
                                      ? QStringLiteral("overlay_ready")
                                      : QStringLiteral("source_unavailable"),
                                  QStringLiteral("track=%1 raw=%2")
                                      .arg(overlays.size())
                                      .arg(rawDetections.size()));
}

void VulkanPreviewSurface::queueFacestreamOverlayCacheWarmup(const TimelineClip& clip,
                                                             int64_t sourceFrame,
                                                             const QString& cacheKey)
{
    if (cacheKey.isEmpty() || m_pendingFacestreamOverlayCacheWarmups.contains(cacheKey)) {
        return;
    }
    if (!m_pipelineOwner) {
        return;
    }
    m_pendingFacestreamOverlayCacheWarmups.insert(cacheKey);
    QPointer<QObject> receiver(m_pipelineOwner.get());
    QTimer::singleShot(250, receiver, [receiver, this, clip, sourceFrame, cacheKey]() {
        if (!receiver) {
            return;
        }
        if (m_interaction.playing) {
            const qint64 nowMs = QDateTime::currentMSecsSinceEpoch();
            if (nowMs - m_lastFacestreamOverlayPlaybackWarmupMs < 1000) {
                m_pendingFacestreamOverlayCacheWarmups.remove(cacheKey);
                return;
            }
            m_lastFacestreamOverlayPlaybackWarmupMs = nowMs;
            loadFacestreamTracksForClip(clip, sourceFrame);
            m_pendingFacestreamOverlayCacheWarmups.remove(cacheKey);
            refreshFacestreamOverlays();
            requestNativeUpdate();
            return;
        }
        loadFacestreamTracksForClip(clip, sourceFrame);
        m_pendingFacestreamOverlayCacheWarmups.remove(cacheKey);
        refreshFacestreamOverlays();
        requestNativeUpdate();
    });
}

bool VulkanPreviewSurface::warmFacestreamOverlayLookahead(int futureFrames, int timeoutMs)
{
    const bool overlayRequested =
        m_showSpeakerTrackBoxes || m_interaction.faceStreamAssignmentInteractionEnabled || m_showRawDetections;
    if (!overlayRequested || m_interaction.clips.isEmpty()) {
        return true;
    }

    QElapsedTimer timer;
    timer.start();
    const QString sourceFilter = normalizedFacestreamOverlaySource(m_facedetectionsOverlaySource);
    const int64_t maxWarmOffset = qMax<int64_t>(
        qMax<int>(0, futureFrames),
        kFacestreamOverlayPlaybackWarmAheadFrames);
    QVector<int64_t> frameOffsets;
    frameOffsets.reserve(static_cast<int>(maxWarmOffset / kFacestreamOverlayInteractiveWindowFrames) + 2);
    frameOffsets.push_back(0);
    for (int64_t offset = kFacestreamOverlayInteractiveWindowFrames;
         offset <= maxWarmOffset;
         offset += kFacestreamOverlayInteractiveWindowFrames) {
        frameOffsets.push_back(offset);
    }
    if (!frameOffsets.contains(qMax<int>(0, futureFrames))) {
        frameOffsets.push_back(qMax<int>(0, futureFrames));
    }
    std::sort(frameOffsets.begin(), frameOffsets.end());

    QSet<QString> visitedCacheKeys;
    int warmedEntries = 0;
    int cachedEntries = 0;
    int timedOutBeforeLoad = 0;
    for (int64_t frameOffset : frameOffsets) {
        const int64_t sample = m_interaction.currentSample + frameToSamples(frameOffset);
        const qreal framePosition = samplesToFramePosition(sample);
        for (const TimelineClip& clip : m_interaction.clips) {
            if (clip.mediaType != ClipMediaType::Video || clip.filePath.isEmpty()) {
                continue;
            }
            if (!visualClipActiveAtSample(clip,
                                          m_interaction.tracks,
                                          sample,
                                          framePosition,
                                          m_bypassGrading)) {
                continue;
            }
            const int64_t localFrame = sourceFrameForSample(clip, sample);
            const FacestreamOverlayCacheIdentity cacheIdentity =
                facestreamOverlayCacheIdentity(
                    clip, localFrame, sourceFilter, m_interaction.renderSyncMarkers);
            if (visitedCacheKeys.contains(cacheIdentity.cacheKey)) {
                continue;
            }
            visitedCacheKeys.insert(cacheIdentity.cacheKey);
            if (m_facedetectionsOverlayCache.contains(cacheIdentity.cacheKey)) {
                ++cachedEntries;
                continue;
            }
            if (m_interaction.playing || (timeoutMs >= 0 && timer.elapsed() >= timeoutMs)) {
                ++timedOutBeforeLoad;
                queueFacestreamOverlayCacheWarmup(clip, localFrame, cacheIdentity.cacheKey);
                continue;
            }
            loadFacestreamTracksForClip(clip, localFrame);
            ++warmedEntries;
        }
    }

    m_lastFacedetectionsQueryDebug[QStringLiteral("playback_overlay_warmup_cache_keys")] =
        visitedCacheKeys.size();
    m_lastFacedetectionsQueryDebug[QStringLiteral("playback_overlay_warmup_loaded")] = warmedEntries;
    m_lastFacedetectionsQueryDebug[QStringLiteral("playback_overlay_warmup_cached")] = cachedEntries;
    m_lastFacedetectionsQueryDebug[QStringLiteral("playback_overlay_warmup_deferred")] =
        timedOutBeforeLoad;
    m_lastFacedetectionsQueryDebug[QStringLiteral("playback_overlay_warmup_elapsed_ms")] =
        timer.elapsed();
    return timedOutBeforeLoad == 0;
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
        QJsonObject dropDebug = snapshot.debug;
        dropDebug[QStringLiteral("status")] = QStringLiteral("overlay_snapshot_dropped");
        dropDebug[QStringLiteral("expected_key")] = expectedKey;
        dropDebug[QStringLiteral("snapshot_key")] = snapshot.requestKey;
        dropDebug[QStringLiteral("request_id")] = static_cast<qint64>(snapshot.requestId);
        dropDebug[QStringLiteral("latest_applied_request_id")] =
            static_cast<qint64>(m_latestAppliedFacestreamOverlayRequestId);
        jcut::facedetections::debugLogJson(
            QStringLiteral("FACESTREAM OVERLAY SNAPSHOT"), dropDebug);
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
    jcut::facedetections::debugLogJson(
        QStringLiteral("FACESTREAM OVERLAY SNAPSHOT"), m_lastFacedetectionsQueryDebug);
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
                                                              m_interaction.renderSyncMarkers,
                                                              TranscriptOverlayTiming{
                                                                  m_interaction.transcriptPrependMs,
                                                                  m_interaction.transcriptPostpendMs,
                                                                  m_interaction.transcriptOffsetMs})) {
            return false;
        }
        return true;
    }
    return false;
}
