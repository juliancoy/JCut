#include "speaker_track_assignment_service.h"

#include "facedetections_artifact_utils.h"
#include "facedetections_time_mapping.h"
#include "speakers_tab_internal.h"

#include <QJsonValue>
#include <QtGlobal>

#include <cmath>
#include <limits>

namespace jcut::speakertrack {
namespace {

QJsonObject rowForAnchor(const QString& identityId,
                         const QJsonObject& anchor,
                         const QString& resolutionSource,
                         const QString& timestampUtc)
{
    QJsonObject row;
    row[QStringLiteral("track_id")] = anchor.value(QStringLiteral("track_id")).toInt(-1);
    row[QStringLiteral("identity_id")] = identityId.trimmed();
    row[QStringLiteral("stream_id")] = anchor.value(QStringLiteral("stream_id")).toString().trimmed();
    row[QString(kSpeakerFlowAnchorSourceFrameKey)] =
        static_cast<qint64>(qMax<int64_t>(
            0,
            anchor.value(QStringLiteral("source_frame")).toVariant().toLongLong()));
    row[QString(kSpeakerFlowAnchorXKey)] =
        qBound<qreal>(0.0, anchor.value(QStringLiteral("x")).toDouble(0.5), 1.0);
    row[QString(kSpeakerFlowAnchorYKey)] =
        qBound<qreal>(0.0, anchor.value(QStringLiteral("y")).toDouble(0.5), 1.0);
    row[QString(kSpeakerFlowAnchorBoxSizeKey)] =
        qBound<qreal>(0.01, anchor.value(QStringLiteral("box")).toDouble(0.2), 1.0);
    row[QStringLiteral("resolution_source")] = resolutionSource.trimmed();
    row[QStringLiteral("updated_at_utc")] = timestampUtc;
    return row;
}

int resolveTrackIdFromStreams(const TimelineClip& clip,
                              const QJsonArray& streams,
                              const QVector<RenderSyncMarker>& renderSyncMarkers,
                              const QJsonObject& row)
{
    const int storedTrackId = row.value(QStringLiteral("track_id")).toInt(-1);
    const QString storedStreamId = row.value(QStringLiteral("stream_id")).toString().trimmed();
    const bool hasAnchor =
        row.contains(QString(kSpeakerFlowAnchorSourceFrameKey)) &&
        row.contains(QString(kSpeakerFlowAnchorXKey)) &&
        row.contains(QString(kSpeakerFlowAnchorYKey)) &&
        row.contains(QString(kSpeakerFlowAnchorBoxSizeKey));
    if (streams.isEmpty()) {
        return storedTrackId;
    }

    for (const QJsonValue& value : streams) {
        const QJsonObject streamObj = value.toObject();
        const int trackId = streamObj.value(QStringLiteral("track_id")).toInt(-1);
        const QString streamId = streamObj.value(QStringLiteral("stream_id")).toString().trimmed();
        if (storedTrackId >= 0 && trackId == storedTrackId &&
            (storedStreamId.isEmpty() || streamId == storedStreamId)) {
            return trackId;
        }
        if (storedTrackId < 0 && !storedStreamId.isEmpty() && streamId == storedStreamId) {
            return trackId;
        }
    }

    if (!hasAnchor) {
        return -1;
    }

    const int64_t anchorSourceFrame =
        row.value(QString(kSpeakerFlowAnchorSourceFrameKey)).toVariant().toLongLong();
    const qreal anchorX = qBound<qreal>(
        0.0,
        row.value(QString(kSpeakerFlowAnchorXKey)).toDouble(0.5),
        1.0);
    const qreal anchorY = qBound<qreal>(
        0.0,
        row.value(QString(kSpeakerFlowAnchorYKey)).toDouble(0.5),
        1.0);
    const qreal anchorBox = qBound<qreal>(
        0.01,
        row.value(QString(kSpeakerFlowAnchorBoxSizeKey)).toDouble(0.2),
        1.0);

    int bestTrackId = -1;
    double bestScore = std::numeric_limits<double>::max();
    for (const QJsonValue& streamValue : streams) {
        const QJsonObject streamObj = streamValue.toObject();
        const int trackId = streamObj.value(QStringLiteral("track_id")).toInt(-1);
        const QJsonArray keyframes = streamObj.value(QStringLiteral("keyframes")).toArray();
        if (trackId < 0 || keyframes.isEmpty()) {
            continue;
        }
        FacestreamFrameDomain frameDomain = FacestreamFrameDomain::SourceRelative;
        if (!parseFacestreamFrameDomainString(
                streamObj.value(QStringLiteral("frame_domain")).toString(),
                &frameDomain)) {
            continue;
        }
        for (const QJsonValue& keyframeValue : keyframes) {
            const QJsonObject keyframeObj = keyframeValue.toObject();
            const int64_t frame =
                keyframeObj.value(QString(kTranscriptSpeakerTrackingFrameKey)).toVariant().toLongLong();
            const int64_t sourceFrame =
                mapFacestreamFrameToSourceFrame(clip, frame, frameDomain, renderSyncMarkers);
            const qreal x = qBound<qreal>(
                0.0,
                keyframeObj.value(QString(kTranscriptSpeakerLocationXKey)).toDouble(0.5),
                1.0);
            const qreal y = qBound<qreal>(
                0.0,
                keyframeObj.value(QString(kTranscriptSpeakerLocationYKey)).toDouble(0.5),
                1.0);
            const qreal box = qBound<qreal>(
                0.01,
                keyframeObj.value(QString(kTranscriptSpeakerTrackingBoxSizeKey)).toDouble(0.2),
                1.0);
            const qreal posDist = std::hypot(x - anchorX, y - anchorY);
            const qreal boxDist = std::abs(box - anchorBox);
            const qreal frameDist =
                static_cast<qreal>(std::llabs(sourceFrame - anchorSourceFrame));
            const double score = (frameDist / 12.0) + (posDist * 6.0) + (boxDist * 3.0);
            if (score < bestScore) {
                bestScore = score;
                bestTrackId = trackId;
            }
        }
    }
    return bestTrackId;
}

} // namespace

QJsonObject makeTrackAnchor(int trackId,
                            const QString& streamId,
                            int64_t sourceFrame,
                            qreal xNorm,
                            qreal yNorm,
                            qreal boxSizeNorm)
{
    QJsonObject anchor;
    anchor[QStringLiteral("track_id")] = trackId;
    anchor[QStringLiteral("stream_id")] = streamId.trimmed();
    anchor[QStringLiteral("source_frame")] = static_cast<qint64>(qMax<int64_t>(0, sourceFrame));
    anchor[QStringLiteral("x")] = qBound<qreal>(0.0, xNorm, 1.0);
    anchor[QStringLiteral("y")] = qBound<qreal>(0.0, yNorm, 1.0);
    anchor[QStringLiteral("box")] = qBound<qreal>(0.01, boxSizeNorm, 1.0);
    return anchor;
}

QJsonArray assignmentMapForClip(const QJsonObject& transcriptRoot, const QString& clipId)
{
    const QJsonObject speakerFlow = transcriptRoot.value(QStringLiteral("speaker_flow")).toObject();
    const QJsonObject clipsRoot = speakerFlow.value(QStringLiteral("clips")).toObject();
    const QJsonObject clipRoot = clipsRoot.value(clipId.trimmed()).toObject();
    const QJsonObject resolvedCurrent = clipRoot.value(QStringLiteral("resolved_current")).toObject();
    return resolvedCurrent.value(QStringLiteral("track_identity_map")).toArray();
}

QJsonArray upsertAssignmentRows(const QJsonArray& currentMap,
                                const QString& identityId,
                                const QJsonArray& trackAnchors,
                                const QString& resolutionSource,
                                const QString& timestampUtc,
                                bool evictExistingForIdentity)
{
    const QString trimmedIdentity = identityId.trimmed();
    const QString effectiveSource = resolutionSource.trimmed().isEmpty()
        ? QStringLiteral("speaker_track_picker")
        : resolutionSource.trimmed();
    QHash<int, QJsonObject> anchorByTrackId;
    for (const QJsonValue& value : trackAnchors) {
        const QJsonObject anchor = value.toObject();
        const int trackId = anchor.value(QStringLiteral("track_id")).toInt(-1);
        if (trackId >= 0) {
            anchorByTrackId.insert(trackId, anchor);
        }
    }

    QJsonArray nextMap;
    QSet<int> appliedTrackIds;
    for (const QJsonValue& value : currentMap) {
        const QJsonObject row = value.toObject();
        const int rowTrackId = row.value(QStringLiteral("track_id")).toInt(-1);
        const auto anchorIt = anchorByTrackId.constFind(rowTrackId);
        if (anchorIt != anchorByTrackId.constEnd()) {
            nextMap.push_back(rowForAnchor(trimmedIdentity, anchorIt.value(), effectiveSource, timestampUtc));
            appliedTrackIds.insert(rowTrackId);
            continue;
        }
        if (evictExistingForIdentity &&
            row.value(QStringLiteral("identity_id")).toString().trimmed() == trimmedIdentity) {
            continue;
        }
        nextMap.push_back(row);
    }
    for (auto it = anchorByTrackId.constBegin(); it != anchorByTrackId.constEnd(); ++it) {
        if (!appliedTrackIds.contains(it.key())) {
            nextMap.push_back(rowForAnchor(trimmedIdentity, it.value(), effectiveSource, timestampUtc));
        }
    }
    return nextMap;
}

void setAssignmentMapForClip(QJsonObject* transcriptRoot,
                             const QString& clipId,
                             const QJsonArray& assignmentMap,
                             const QString& timestampUtc)
{
    if (!transcriptRoot) {
        return;
    }
    QJsonObject speakerFlow = transcriptRoot->value(QStringLiteral("speaker_flow")).toObject();
    speakerFlow[QStringLiteral("schema_version")] = QStringLiteral("1.0");
    QJsonObject clipsRoot = speakerFlow.value(QStringLiteral("clips")).toObject();
    QJsonObject clipRoot = clipsRoot.value(clipId.trimmed()).toObject();
    clipRoot[QStringLiteral("clip_id")] = clipId.trimmed();
    clipRoot[QStringLiteral("updated_at_utc")] = timestampUtc;
    QJsonObject resolvedPayload = clipRoot.value(QStringLiteral("resolved_current")).toObject();
    resolvedPayload[QStringLiteral("updated_at_utc")] = timestampUtc;
    resolvedPayload[QStringLiteral("track_identity_map")] = assignmentMap;
    clipRoot[QStringLiteral("resolved_current")] = resolvedPayload;
    clipsRoot[clipId.trimmed()] = clipRoot;
    speakerFlow[QStringLiteral("clips")] = clipsRoot;
    (*transcriptRoot)[QStringLiteral("speaker_flow")] = speakerFlow;
}

ResolvedAssignments resolveAssignments(const QJsonObject& transcriptRoot,
                                       const TimelineClip& clip,
                                       const QJsonArray& streams,
                                       const QVector<RenderSyncMarker>& renderSyncMarkers)
{
    ResolvedAssignments result;
    const QJsonArray rows = assignmentMapForClip(transcriptRoot, clip.id);
    for (const QJsonValue& value : rows) {
        const QJsonObject row = value.toObject();
        const QString identityId = row.value(QStringLiteral("identity_id")).toString().trimmed();
        if (identityId.isEmpty()) {
            continue;
        }
        const int trackId = resolveTrackIdFromStreams(clip, streams, renderSyncMarkers, row);
        if (trackId < 0) {
            continue;
        }
        result.identityByTrackId.insert(trackId, identityId);
        QVector<int>& trackIds = result.trackIdsByIdentity[identityId];
        if (!trackIds.contains(trackId)) {
            trackIds.push_back(trackId);
        }
    }
    return result;
}

QSet<int> trackIdsForIdentity(const QJsonObject& transcriptRoot,
                              const TimelineClip& clip,
                              const QJsonArray& streams,
                              const QVector<RenderSyncMarker>& renderSyncMarkers,
                              const QString& identityId)
{
    QSet<int> ids;
    const QVector<int> resolvedIds =
        resolveAssignments(transcriptRoot, clip, streams, renderSyncMarkers)
            .trackIdsByIdentity.value(identityId.trimmed());
    for (int id : resolvedIds) {
        ids.insert(id);
    }
    return ids;
}

} // namespace jcut::speakertrack
