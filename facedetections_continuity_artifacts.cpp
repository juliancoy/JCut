#include "facedetections_runtime.h"

#include "facedetections_artifact_utils.h"
#include "json_io_utils.h"
#include "speakers_tab_internal.h"
#include "transcript_engine.h"

#include <QDateTime>
#include <QDir>
#include <QFileInfo>
#include <QSet>

namespace jcut::facedetections {
namespace {
constexpr qint64 kMaxInteractiveContinuityObjectBytes = 128ll * 1024ll * 1024ll;

bool streamMatchesAssignments(const QJsonObject& streamObj,
                              const QSet<int>& trackIds,
                              const QSet<QString>& streamIds)
{
    const int trackId = streamObj.value(QStringLiteral("track_id")).toInt(-1);
    const QString streamId = streamObj.value(QStringLiteral("stream_id")).toString().trimmed();
    return (trackId >= 0 && trackIds.contains(trackId)) ||
           (!streamId.isEmpty() && streamIds.contains(streamId));
}

QJsonArray filterStreamsForAssignments(const QJsonArray& streams,
                                       const QSet<int>& trackIds,
                                       const QSet<QString>& streamIds)
{
    if (trackIds.isEmpty() && streamIds.isEmpty()) {
        return {};
    }
    QJsonArray filtered;
    for (const QJsonValue& value : streams) {
        const QJsonObject streamObj = value.toObject();
        if (streamMatchesAssignments(streamObj, trackIds, streamIds)) {
            filtered.push_back(streamObj);
        }
    }
    return filtered;
}

bool recordArtifactTracksForAssignments(const QString& path,
                                        const QSet<int>& trackIds,
                                        const QSet<QString>& streamIds,
                                        QJsonArray* tracksOut)
{
    if (tracksOut) {
        *tracksOut = QJsonArray{};
    }
    const QString trimmedPath = path.trimmed();
    if (trimmedPath.isEmpty() || (trackIds.isEmpty() && streamIds.isEmpty())) {
        return false;
    }
    QFile file(trimmedPath);
    if (!file.open(QIODevice::ReadOnly)) {
        return false;
    }

    QDataStream stream(&file);
    stream.setVersion(QDataStream::Qt_6_0);
    QJsonArray tracks;
    while (!stream.atEnd()) {
        quint32 magic = 0;
        quint32 version = 0;
        quint32 compressedSize = 0;
        stream >> magic;
        stream >> version;
        stream >> compressedSize;
        if (stream.status() != QDataStream::Ok || magic != 0x4A465352 || version != 1 ||
            compressedSize > static_cast<quint32>(std::numeric_limits<int>::max())) {
            return false;
        }
        QByteArray compressed(static_cast<int>(compressedSize), Qt::Uninitialized);
        if (compressedSize > 0 &&
            stream.readRawData(compressed.data(), static_cast<int>(compressedSize)) !=
                static_cast<int>(compressedSize)) {
            return false;
        }
        QJsonObject record;
        if (!jcut::jsonio::parseRecordPayload(qUncompress(compressed), &record, nullptr)) {
            return false;
        }
        if (record.value(QStringLiteral("type")).toString() != QStringLiteral("track")) {
            continue;
        }
        const int trackId = record.value(QStringLiteral("track_id")).toInt(-1);
        const QString streamId = record.value(QStringLiteral("stream_id")).toString().trimmed();
        if ((trackId >= 0 && trackIds.contains(trackId)) ||
            (!streamId.isEmpty() && streamIds.contains(streamId)) ||
            (trackId >= 0 && streamIds.contains(QStringLiteral("T%1").arg(trackId)))) {
            record.remove(QStringLiteral("type"));
            tracks.push_back(record);
        }
    }
    if (tracksOut) {
        *tracksOut = tracks;
    }
    return true;
}

QJsonObject readArtifactRootAtPath(const QString& path)
{
    const QString trimmedPath = path.trimmed();
    if (trimmedPath.isEmpty()) {
        return {};
    }
    QJsonObject root;
    if (!readBinaryJsonObject(trimmedPath, &root, nullptr)) {
        return {};
    }
    return root;
}

bool recordArtifactToRoot(const QString& path, QJsonObject* rootOut)
{
    QVector<QJsonObject> records;
    if (!jcut::jsonio::readBinaryJsonRecords(path, &records, 0x4A465352, 1, nullptr) ||
        records.isEmpty()) {
        return false;
    }

    QJsonObject root;
    QJsonArray frames;
    QJsonArray tracks;
    QJsonArray frameSummaries;
    QString schema;
    QString video;
    QString backend;
    QString frameDomain = QStringLiteral("source_absolute");
    for (const QJsonObject& record : records) {
        const QString type = record.value(QStringLiteral("type")).toString();
        if (type == QStringLiteral("meta")) {
            schema = record.value(QStringLiteral("schema")).toString(schema);
            video = record.value(QStringLiteral("video")).toString(video);
            backend = record.value(QStringLiteral("backend")).toString(backend);
            frameDomain = record.value(QStringLiteral("frame_domain")).toString(frameDomain);
            continue;
        }
        if (type == QStringLiteral("frame")) {
            QJsonObject frame = record;
            frame.remove(QStringLiteral("type"));
            frames.push_back(frame);
            continue;
        }
        if (type == QStringLiteral("track")) {
            QJsonObject track = record;
            track.remove(QStringLiteral("type"));
            tracks.push_back(track);
            continue;
        }
        if (type == QStringLiteral("frame_summary")) {
            QJsonObject summary = record;
            summary.remove(QStringLiteral("type"));
            frameSummaries.push_back(summary);
        }
    }

    if (!schema.isEmpty()) {
        root[QStringLiteral("schema")] = schema;
    }
    if (!video.isEmpty()) {
        root[QStringLiteral("video")] = video;
    }
    if (!backend.isEmpty()) {
        root[QStringLiteral("backend")] = backend;
    }
    root[QStringLiteral("frame_domain")] = frameDomain;
    if (!frames.isEmpty()) {
        root[QStringLiteral("frames")] = frames;
    }
    if (!tracks.isEmpty()) {
        root[QStringLiteral("tracks")] = tracks;
    }
    if (!frameSummaries.isEmpty()) {
        root[QStringLiteral("frame_summaries")] = frameSummaries;
    }
    if (rootOut) {
        *rootOut = root;
    }
    return !root.isEmpty();
}

QJsonArray rawTracksForContinuityRoot(const QJsonObject& continuityRoot)
{
    const QJsonArray inlineTracks = continuityRoot.value(QStringLiteral("raw_tracks")).toArray();
    if (!inlineTracks.isEmpty()) {
        return inlineTracks;
    }

    QJsonObject tracksArtifact =
        readArtifactRootAtPath(continuityRoot.value(QStringLiteral("raw_tracks_artifact_path")).toString());
    if (tracksArtifact.isEmpty()) {
        const QString artifactDir =
            continuityRoot.value(QStringLiteral("imported_from_artifact_dir")).toString().trimmed();
        if (!artifactDir.isEmpty()) {
            tracksArtifact = readArtifactRootAtPath(
                QDir(artifactDir).absoluteFilePath(QStringLiteral("tracks.bin")));
        }
    }
    return tracksArtifact.value(QStringLiteral("tracks")).toArray();
}

void copyIfPresent(const QJsonObject& source, QJsonObject* dest, const QString& key)
{
    if (!dest || !source.contains(key)) {
        return;
    }
    (*dest)[key] = source.value(key);
}

QJsonObject compactContinuityRootForSidecar(const QJsonObject& root)
{
    QJsonObject compact = root;
    if (!compact.value(QStringLiteral("raw_tracks_artifact_path")).toString().trimmed().isEmpty()) {
        compact.remove(QStringLiteral("raw_tracks"));
    }
    if (!compact.value(QStringLiteral("raw_frames_artifact_path")).toString().trimmed().isEmpty()) {
        compact.remove(QStringLiteral("raw_frames"));
    }
    return compact;
}

QJsonArray rawFramesForContinuityRoot(const QJsonObject& continuityRoot)
{
    const QJsonArray inlineFrames = continuityRoot.value(QStringLiteral("raw_frames")).toArray();
    if (!inlineFrames.isEmpty()) {
        return inlineFrames;
    }

    QJsonObject detectionsArtifact =
        readArtifactRootAtPath(continuityRoot.value(QStringLiteral("raw_frames_artifact_path")).toString());
    if (detectionsArtifact.isEmpty()) {
        const QString artifactDir =
            continuityRoot.value(QStringLiteral("imported_from_artifact_dir")).toString().trimmed();
        if (!artifactDir.isEmpty()) {
            detectionsArtifact = readArtifactRootAtPath(
                QDir(artifactDir).absoluteFilePath(QStringLiteral("detections.bin")));
        }
    }
    QJsonArray frames = detectionsArtifact.value(QStringLiteral("frames")).toArray();
    if (!frames.isEmpty()) {
        return frames;
    }

    const QJsonObject tracksArtifact =
        readArtifactRootAtPath(continuityRoot.value(QStringLiteral("raw_tracks_artifact_path")).toString());
    return tracksArtifact.value(QStringLiteral("frames")).toArray();
}

QString resolvedRawTrackFrameDomain(const QJsonObject& continuityRoot)
{
    const QString rawTracksFrameDomain =
        continuityRoot.value(QStringLiteral("raw_tracks_frame_domain")).toString().trimmed();
    const QString rawFramesFrameDomain =
        continuityRoot.value(QStringLiteral("raw_frames_frame_domain")).toString().trimmed();
    if (rawTracksFrameDomain.isEmpty()) {
        return rawFramesFrameDomain;
    }
    if (rawTracksFrameDomain == facedetectionsFrameDomainString(FacestreamFrameDomain::SourceAbsolute) &&
        !rawFramesFrameDomain.isEmpty() &&
        rawFramesFrameDomain != rawTracksFrameDomain) {
        const QJsonArray rawTracks = rawTracksForContinuityRoot(continuityRoot);
        const QJsonArray rawFrames = rawFramesForContinuityRoot(continuityRoot);
        auto frameRangeForTrackDetections = [](const QJsonArray& tracks) {
            qint64 minFrame = std::numeric_limits<qint64>::max();
            qint64 maxFrame = std::numeric_limits<qint64>::min();
            for (const QJsonValue& trackValue : tracks) {
                for (const QJsonValue& detValue : trackValue.toObject().value(QStringLiteral("detections")).toArray()) {
                    const qint64 frame = detValue.toObject().value(QStringLiteral("frame")).toVariant().toLongLong();
                    minFrame = qMin(minFrame, frame);
                    maxFrame = qMax(maxFrame, frame);
                }
            }
            return qMakePair(minFrame, maxFrame);
        };
        auto frameRangeForRawFrames = [](const QJsonArray& frames) {
            qint64 minFrame = std::numeric_limits<qint64>::max();
            qint64 maxFrame = std::numeric_limits<qint64>::min();
            for (const QJsonValue& frameValue : frames) {
                const qint64 frame = frameValue.toObject().value(QStringLiteral("frame")).toVariant().toLongLong();
                minFrame = qMin(minFrame, frame);
                maxFrame = qMax(maxFrame, frame);
            }
            return qMakePair(minFrame, maxFrame);
        };
        const auto trackRange = frameRangeForTrackDetections(rawTracks);
        const auto rawFrameRange = frameRangeForRawFrames(rawFrames);
        if (trackRange.first != std::numeric_limits<qint64>::max() &&
            rawFrameRange.first != std::numeric_limits<qint64>::max()) {
            const qint64 minDelta = std::llabs(trackRange.first - rawFrameRange.first);
            const qint64 maxDelta = std::llabs(trackRange.second - rawFrameRange.second);
            if (minDelta <= 2 && maxDelta <= 2) {
                return rawFramesFrameDomain;
            }
        }
    }
    return rawTracksFrameDomain;
}

QJsonArray deriveContinuityStreamsFromRawRoot(const QJsonObject& continuityRoot,
                                              const QJsonObject& transcriptRoot)
{
    const QJsonArray rawTracks = rawTracksForContinuityRoot(continuityRoot);
    if (rawTracks.isEmpty()) {
        return continuityRoot.value(QStringLiteral("streams")).toArray();
    }

    const QString detectorMode =
        continuityRoot.value(QStringLiteral("detector_mode")).toString().trimmed();
    const bool onlyDialogue = continuityRoot.value(QStringLiteral("only_dialogue")).toBool(false);
    return buildContinuityStreams(
        rawTracks,
        transcriptRoot,
        detectorMode,
        onlyDialogue,
        resolvedRawTrackFrameDomain(continuityRoot));
}

} // namespace

QJsonArray buildContinuityStreams(const QJsonArray& tracks,
                                  const QJsonObject& transcriptRoot,
                                  const QString& detectorMode,
                                  bool onlyDialogue,
                                  const QString& frameDomain)
{
    QJsonArray streams;
    const QJsonArray segments = transcriptRoot.value(QStringLiteral("segments")).toArray();
    for (const QJsonValue& trackValue : tracks) {
        const QJsonObject trackObj = trackValue.toObject();
        const int trackId = trackObj.value(QStringLiteral("track_id")).toInt(-1);
        const QJsonArray detections = trackObj.value(QStringLiteral("detections")).toArray();
        if (trackId < 0 || detections.isEmpty()) {
            continue;
        }
        QJsonArray keyframes;
        for (const QJsonValue& detValue : detections) {
            const QJsonObject det = detValue.toObject();
            const int64_t frame = qMax<int64_t>(0, det.value(QStringLiteral("frame")).toVariant().toLongLong());
            if (onlyDialogue) {
                bool spoken = false;
                const double t = static_cast<double>(frame) / static_cast<double>(kTimelineFps);
                for (const QJsonValue& segValue : segments) {
                    const QJsonObject segObj = segValue.toObject();
                    const QJsonArray words = segObj.value(QStringLiteral("words")).toArray();
                    for (const QJsonValue& wordValue : words) {
                        const QJsonObject wordObj = wordValue.toObject();
                        if (wordObj.value(QStringLiteral("skipped")).toBool(false)) {
                            continue;
                        }
                        const double ws = wordObj.value(QStringLiteral("start")).toDouble(-1.0);
                        const double we = wordObj.value(QStringLiteral("end")).toDouble(-1.0);
                        if (ws >= 0.0 && we >= ws && t >= ws && t <= we) {
                            spoken = true;
                            break;
                        }
                    }
                    if (spoken) {
                        break;
                    }
                }
                if (!spoken) {
                    continue;
                }
            }
            QJsonObject p;
            p[QString(kTranscriptSpeakerTrackingFrameKey)] = static_cast<qint64>(frame);
            p[QString(kTranscriptSpeakerLocationXKey)] =
                qBound(0.0, det.value(QStringLiteral("x")).toDouble(0.5), 1.0);
            p[QString(kTranscriptSpeakerLocationYKey)] =
                qBound(0.0, det.value(QStringLiteral("y")).toDouble(0.5), 1.0);
            p[QString(kTranscriptSpeakerTrackingBoxSizeKey)] =
                qBound(0.01, det.value(QStringLiteral("box")).toDouble(0.2), 1.0);
            p[QString(kTranscriptSpeakerTrackingConfidenceKey)] =
                qBound(0.0, det.value(QStringLiteral("score")).toDouble(0.0), 1.0);
            p[QString(kTranscriptSpeakerTrackingSourceKey)] = detectorMode;
            keyframes.push_back(p);
        }
        if (keyframes.isEmpty()) {
            continue;
        }
        QJsonObject stream;
        stream[QStringLiteral("stream_id")] = QStringLiteral("T%1").arg(trackId);
        stream[QStringLiteral("track_id")] = trackId;
        if (!frameDomain.trimmed().isEmpty()) {
            stream[QStringLiteral("frame_domain")] = frameDomain.trimmed();
        }
        stream[QStringLiteral("keyframes")] = keyframes;
        streams.push_back(stream);
    }
    return streams;
}

QJsonArray continuityStreamsForRoot(const QJsonObject& continuityRoot,
                                    const QJsonObject& transcriptRoot)
{
    const QJsonArray storedStreams = continuityRoot.value(QStringLiteral("streams")).toArray();
    if (!storedStreams.isEmpty()) {
        return storedStreams;
    }

    const QString processedArtifactPath =
        continuityRoot.value(QStringLiteral("processed_artifact_path")).toString().trimmed();
    const QString clipId = continuityRoot.value(QStringLiteral("clip_id")).toString().trimmed();
    if (!processedArtifactPath.isEmpty() && !clipId.isEmpty()) {
        QJsonObject processedArtifact;
        if (readBinaryJsonObject(processedArtifactPath, &processedArtifact, nullptr)) {
            const QJsonObject processedRoot = continuityRootForClip(processedArtifact, clipId);
            const QJsonArray processedStreams =
                processedRoot.value(QStringLiteral("streams")).toArray();
            if (!processedStreams.isEmpty()) {
                return processedStreams;
            }
            const QJsonArray derivedStreams =
                deriveContinuityStreamsFromRawRoot(processedRoot, transcriptRoot);
            if (!derivedStreams.isEmpty()) {
                return derivedStreams;
            }
        }
    }

    return deriveContinuityStreamsFromRawRoot(continuityRoot, transcriptRoot);
}

QJsonArray storedContinuityStreamsForRoot(const QJsonObject& continuityRoot,
                                          qint64 maxReferencedArtifactBytes)
{
    const QJsonArray storedStreams = continuityRoot.value(QStringLiteral("streams")).toArray();
    if (!storedStreams.isEmpty()) {
        return storedStreams;
    }

    const QString processedArtifactPath =
        continuityRoot.value(QStringLiteral("processed_artifact_path")).toString().trimmed();
    const QString clipId = continuityRoot.value(QStringLiteral("clip_id")).toString().trimmed();
    if (processedArtifactPath.isEmpty() || clipId.isEmpty()) {
        return {};
    }

    const QFileInfo processedInfo(processedArtifactPath);
    if (!processedInfo.exists() ||
        !processedInfo.isFile() ||
        processedInfo.size() > qMax<qint64>(0, maxReferencedArtifactBytes)) {
        return {};
    }

    QJsonObject processedArtifact;
    if (!readBinaryJsonObject(processedInfo.absoluteFilePath(), &processedArtifact, nullptr)) {
        return {};
    }
    return continuityRootForClip(processedArtifact, clipId).value(QStringLiteral("streams")).toArray();
}

QJsonArray continuityStreamsForAssignments(const QJsonObject& continuityRoot,
                                           const QSet<int>& trackIds,
                                           const QSet<QString>& streamIds,
                                           const QJsonObject& transcriptRoot)
{
    const QJsonArray storedStreams = filterStreamsForAssignments(
        continuityRoot.value(QStringLiteral("streams")).toArray(), trackIds, streamIds);
    if (!storedStreams.isEmpty()) {
        return storedStreams;
    }

    const QJsonArray inlineTracks = continuityRoot.value(QStringLiteral("raw_tracks")).toArray();
    if (!inlineTracks.isEmpty()) {
        QJsonArray matchedTracks;
        for (const QJsonValue& value : inlineTracks) {
            const QJsonObject track = value.toObject();
            const int trackId = track.value(QStringLiteral("track_id")).toInt(-1);
            const QString streamId = track.value(QStringLiteral("stream_id")).toString().trimmed();
            if ((trackId >= 0 && trackIds.contains(trackId)) ||
                (!streamId.isEmpty() && streamIds.contains(streamId)) ||
                (trackId >= 0 && streamIds.contains(QStringLiteral("T%1").arg(trackId)))) {
                matchedTracks.push_back(track);
            }
        }
        return buildContinuityStreams(
            matchedTracks,
            transcriptRoot,
            continuityRoot.value(QStringLiteral("detector_mode")).toString().trimmed(),
            continuityRoot.value(QStringLiteral("only_dialogue")).toBool(false),
            resolvedRawTrackFrameDomain(continuityRoot));
    }

    QJsonArray matchedTracks;
    const QString rawTracksPath =
        continuityRoot.value(QStringLiteral("raw_tracks_artifact_path")).toString().trimmed();
    if (!rawTracksPath.isEmpty() &&
        recordArtifactTracksForAssignments(rawTracksPath, trackIds, streamIds, &matchedTracks) &&
        !matchedTracks.isEmpty()) {
        return buildContinuityStreams(
            matchedTracks,
            transcriptRoot,
            continuityRoot.value(QStringLiteral("detector_mode")).toString().trimmed(),
            continuityRoot.value(QStringLiteral("only_dialogue")).toBool(false),
            resolvedRawTrackFrameDomain(continuityRoot));
    }

    if (!rawTracksPath.isEmpty() &&
        QFileInfo(rawTracksPath).size() <= kMaxInteractiveContinuityObjectBytes) {
        const QJsonObject tracksArtifact = readArtifactRootAtPath(rawTracksPath);
        const QJsonArray tracks = tracksArtifact.value(QStringLiteral("tracks")).toArray();
        for (const QJsonValue& value : tracks) {
            const QJsonObject track = value.toObject();
            const int trackId = track.value(QStringLiteral("track_id")).toInt(-1);
            const QString streamId = track.value(QStringLiteral("stream_id")).toString().trimmed();
            if ((trackId >= 0 && trackIds.contains(trackId)) ||
                (!streamId.isEmpty() && streamIds.contains(streamId)) ||
                (trackId >= 0 && streamIds.contains(QStringLiteral("T%1").arg(trackId)))) {
                matchedTracks.push_back(track);
            }
        }
        if (!matchedTracks.isEmpty()) {
            return buildContinuityStreams(
                matchedTracks,
                transcriptRoot,
                continuityRoot.value(QStringLiteral("detector_mode")).toString().trimmed(),
                continuityRoot.value(QStringLiteral("only_dialogue")).toBool(false),
                resolvedRawTrackFrameDomain(continuityRoot));
        }
    }

    const QString processedArtifactPath =
        continuityRoot.value(QStringLiteral("processed_artifact_path")).toString().trimmed();
    const QString clipId = continuityRoot.value(QStringLiteral("clip_id")).toString().trimmed();
    if (!processedArtifactPath.isEmpty() &&
        !clipId.isEmpty() &&
        QFileInfo(processedArtifactPath).size() <= kMaxInteractiveContinuityObjectBytes) {
        QJsonObject processedArtifact;
        if (readBinaryJsonObject(processedArtifactPath, &processedArtifact, nullptr)) {
            return continuityStreamsForAssignments(
                continuityRootForClip(processedArtifact, clipId),
                trackIds,
                streamIds,
                transcriptRoot);
        }
    }

    return {};
}

bool continuityRootHasTracks(const QJsonObject& continuityRoot,
                             const QJsonObject& transcriptRoot)
{
    Q_UNUSED(transcriptRoot);
    if (!continuityRoot.value(QStringLiteral("streams")).toArray().isEmpty()) {
        return true;
    }
    if (continuityRoot.value(QStringLiteral("raw_tracks_count")).toInt(0) > 0 ||
        !continuityRoot.value(QStringLiteral("raw_tracks_artifact_path")).toString().trimmed().isEmpty()) {
        return true;
    }
    const QString processedArtifactPath =
        continuityRoot.value(QStringLiteral("processed_artifact_path")).toString().trimmed();
    if (!processedArtifactPath.isEmpty()) {
        const QString clipId = continuityRoot.value(QStringLiteral("clip_id")).toString().trimmed();
        if (!clipId.isEmpty()) {
            QJsonObject processedArtifact;
            if (readBinaryJsonObject(processedArtifactPath, &processedArtifact, nullptr)) {
                const QJsonObject processedRoot = continuityRootForClip(processedArtifact, clipId);
                if (!processedRoot.value(QStringLiteral("streams")).toArray().isEmpty()) {
                    return true;
                }
                if (processedRoot.value(QStringLiteral("raw_tracks_count")).toInt(0) > 0 ||
                    !processedRoot.value(QStringLiteral("raw_tracks_artifact_path")).toString().trimmed().isEmpty()) {
                    return true;
                }
            }
        }
    }
    return !continuityRoot.value(QStringLiteral("raw_tracks")).toArray().isEmpty();
}

bool continuityRootHasStoredPayload(const QJsonObject& continuityRoot)
{
    if (continuityRoot.isEmpty()) {
        return false;
    }
    if (!continuityRoot.value(QStringLiteral("streams")).toArray().isEmpty() ||
        !continuityRoot.value(QStringLiteral("raw_tracks")).toArray().isEmpty() ||
        !continuityRoot.value(QStringLiteral("raw_frames")).toArray().isEmpty()) {
        return true;
    }

    static const QStringList kPathLikeKeys = {
        QStringLiteral("facedetections_part"),
        QStringLiteral("facedetections_bin"),
        QStringLiteral("facedetections_ndjson"),
        QStringLiteral("raw_tracks_artifact_path"),
        QStringLiteral("raw_frames_artifact_path"),
        QStringLiteral("continuity_artifact_path"),
        QStringLiteral("summary_json"),
        QStringLiteral("processed_artifact_path"),
        QStringLiteral("imported_from_artifact_dir")
    };
    for (const QString& key : kPathLikeKeys) {
        if (!continuityRoot.value(key).toString().trimmed().isEmpty()) {
            return true;
        }
    }

    return !continuityRoot.value(QStringLiteral("clip_id")).toString().trimmed().isEmpty() ||
           !continuityRoot.value(QStringLiteral("run_id")).toString().trimmed().isEmpty();
}

QJsonObject buildProcessedContinuityRoot(const QString& clipId,
                                         const QJsonObject& rawContinuityRoot,
                                         const QJsonObject& transcriptRoot,
                                         const QString& rawArtifactPath)
{
    Q_UNUSED(transcriptRoot);

    QJsonObject processedRoot;
    processedRoot[QStringLiteral("clip_id")] = clipId.trimmed();
    processedRoot[QStringLiteral("run_id")] = rawContinuityRoot.value(QStringLiteral("run_id")).toString();
    processedRoot[QStringLiteral("updated_at_utc")] = QDateTime::currentDateTimeUtc().toString(Qt::ISODate);
    processedRoot[QStringLiteral("only_dialogue")] = rawContinuityRoot.value(QStringLiteral("only_dialogue")).toBool(false);
    processedRoot[QStringLiteral("scan_start_frame")] =
        rawContinuityRoot.value(QStringLiteral("scan_start_frame")).toVariant().toLongLong();
    processedRoot[QStringLiteral("scan_end_frame")] =
        rawContinuityRoot.value(QStringLiteral("scan_end_frame")).toVariant().toLongLong();
    processedRoot[QStringLiteral("detector_mode")] =
        rawContinuityRoot.value(QStringLiteral("detector_mode")).toString().trimmed();
    processedRoot[QStringLiteral("source_raw_artifact_path")] = rawArtifactPath;
    const QFileInfo rawInfo(rawArtifactPath);
    if (rawInfo.exists() && rawInfo.isFile()) {
        processedRoot[QStringLiteral("source_raw_artifact_mtime_ms")] =
            rawInfo.lastModified().toMSecsSinceEpoch();
    }
    static const QStringList kManifestKeys = {
        QStringLiteral("raw_tracks_artifact_path"),
        QStringLiteral("raw_frames_artifact_path"),
        QStringLiteral("continuity_artifact_path"),
        QStringLiteral("raw_tracks_count"),
        QStringLiteral("raw_frames_count"),
        QStringLiteral("raw_tracks_schema"),
        QStringLiteral("raw_frames_schema"),
        QStringLiteral("raw_tracks_frame_domain"),
        QStringLiteral("raw_frames_frame_domain"),
        QStringLiteral("streams_frame_domain"),
        QStringLiteral("imported_from_artifact_dir"),
        QStringLiteral("facedetections_part"),
        QStringLiteral("summary_json")
    };
    for (const QString& key : kManifestKeys) {
        copyIfPresent(rawContinuityRoot, &processedRoot, key);
    }
    if (processedRoot.value(QStringLiteral("raw_tracks_artifact_path")).toString().trimmed().isEmpty()) {
        copyIfPresent(rawContinuityRoot, &processedRoot, QStringLiteral("raw_tracks"));
    }
    if (processedRoot.value(QStringLiteral("raw_frames_artifact_path")).toString().trimmed().isEmpty()) {
        copyIfPresent(rawContinuityRoot, &processedRoot, QStringLiteral("raw_frames"));
    }
    const QString streamsFrameDomain =
        rawContinuityRoot.value(QStringLiteral("streams_frame_domain")).toString().trimmed();
    if (!streamsFrameDomain.isEmpty()) {
        processedRoot[QStringLiteral("streams_frame_domain")] = streamsFrameDomain;
    } else {
        const QString rawTracksFrameDomain = resolvedRawTrackFrameDomain(rawContinuityRoot);
        if (!rawTracksFrameDomain.isEmpty()) {
            processedRoot[QStringLiteral("streams_frame_domain")] = rawTracksFrameDomain;
        }
    }
    return processedRoot;
}

bool saveProcessedContinuityArtifact(const QString& transcriptPath,
                                     const QString& clipId,
                                     const QJsonObject& rawContinuityRoot,
                                     const QJsonObject& transcriptRoot,
                                     QJsonObject* artifactRootOut)
{
    editor::TranscriptEngine engine;
    QJsonObject artifactRoot;
    engine.loadFacestreamProcessedArtifact(transcriptPath, &artifactRoot);

    const QString rawArtifactPath = engine.facedetectionsArtifactPath(transcriptPath);
    const QJsonObject processedRoot =
        buildProcessedContinuityRoot(clipId, rawContinuityRoot, transcriptRoot, rawArtifactPath);

    QJsonObject byClip = continuityFacestreamsByClipObject(artifactRoot);
    byClip[clipId] = compactContinuityRootForSidecar(processedRoot);
    artifactRoot[QStringLiteral("schema")] = QStringLiteral("jcut_facedetections_processed_v1");
    artifactRoot[QStringLiteral("updated_at_utc")] = QDateTime::currentDateTimeUtc().toString(Qt::ISODate);
    setContinuityFacestreamsByClipObject(&artifactRoot, byClip);
    if (artifactRootOut) {
        *artifactRootOut = artifactRoot;
    }
    return engine.saveFacestreamProcessedArtifact(transcriptPath, artifactRoot);
}

QJsonObject buildContinuityRoot(const QString& runId,
                                bool onlyDialogue,
                                int64_t scanStart,
                                int64_t scanEnd,
                                const QJsonArray& streams,
                                const QJsonArray& rawTracks,
                                const QJsonArray& rawFrames,
                                const QString& detectorMode)
{
    QJsonObject root;
    root[QStringLiteral("run_id")] = runId;
    root[QStringLiteral("updated_at_utc")] = QDateTime::currentDateTimeUtc().toString(Qt::ISODate);
    root[QStringLiteral("only_dialogue")] = onlyDialogue;
    root[QStringLiteral("scan_start_frame")] = static_cast<qint64>(scanStart);
    root[QStringLiteral("scan_end_frame")] = static_cast<qint64>(scanEnd);
    if (!streams.isEmpty()) {
        root[QStringLiteral("streams")] = streams;
        root[QStringLiteral("streams_frame_domain")] =
            facedetectionsFrameDomainString(FacestreamFrameDomain::SourceAbsolute);
    }
    if (!rawTracks.isEmpty()) {
        root[QStringLiteral("raw_tracks")] = rawTracks;
        root[QStringLiteral("raw_tracks_frame_domain")] =
            facedetectionsFrameDomainString(FacestreamFrameDomain::SourceAbsolute);
    }
    if (!rawFrames.isEmpty()) {
        root[QStringLiteral("raw_frames")] = rawFrames;
        root[QStringLiteral("raw_frames_frame_domain")] =
            facedetectionsFrameDomainString(FacestreamFrameDomain::SourceAbsolute);
    }
    if (!detectorMode.trimmed().isEmpty()) {
        root[QStringLiteral("detector_mode")] = detectorMode.trimmed();
    }
    return root;
}

bool readBinaryJsonObject(const QString& path,
                          QJsonObject* objectOut,
                          QString* errorOut)
{
    if (jcut::jsonio::readBinaryJsonObject(path, objectOut, 0x4A435554, 1, errorOut)) {
        return true;
    }
    if (recordArtifactToRoot(path, objectOut)) {
        return true;
    }
    return false;
}

bool saveContinuityArtifact(const QString& transcriptPath,
                            const QString& clipId,
                            const QJsonObject& continuityRoot,
                            QJsonObject* artifactRootOut)
{
    editor::TranscriptEngine engine;
    QJsonObject artifactRoot;
    engine.loadFacestreamArtifact(transcriptPath, &artifactRoot);
    QJsonObject continuityByClip = continuityFacestreamsByClipObject(artifactRoot);
    continuityByClip[clipId] = compactContinuityRootForSidecar(continuityRoot);
    artifactRoot[QStringLiteral("schema")] = QStringLiteral("jcut_facedetections_v1");
    setContinuityFacestreamsByClipObject(&artifactRoot, continuityByClip);
    if (artifactRootOut) {
        *artifactRootOut = artifactRoot;
    }
    return engine.saveFacestreamArtifact(transcriptPath, artifactRoot);
}

} // namespace jcut::facedetections
