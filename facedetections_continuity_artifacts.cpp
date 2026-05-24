#include "facedetections_runtime.h"

#include "facedetections_artifact_utils.h"
#include "json_io_utils.h"
#include "speakers_tab_internal.h"
#include "transcript_engine.h"

#include <QDateTime>
#include <QFileInfo>

namespace jcut::facedetections {
namespace {

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
        const QJsonArray rawTracks = continuityRoot.value(QStringLiteral("raw_tracks")).toArray();
        const QJsonArray rawFrames = continuityRoot.value(QStringLiteral("raw_frames")).toArray();
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
    const QJsonArray rawTracks = continuityRoot.value(QStringLiteral("raw_tracks")).toArray();
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
        }
    }

    return deriveContinuityStreamsFromRawRoot(continuityRoot, transcriptRoot);
}

bool continuityRootHasTracks(const QJsonObject& continuityRoot,
                             const QJsonObject& transcriptRoot)
{
    Q_UNUSED(transcriptRoot);
    if (!continuityRoot.value(QStringLiteral("streams")).toArray().isEmpty()) {
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
    const QJsonArray streams = deriveContinuityStreamsFromRawRoot(rawContinuityRoot, transcriptRoot);

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
    processedRoot[QStringLiteral("streams")] = streams;
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
    byClip[clipId] = processedRoot;
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
    return jcut::jsonio::readBinaryJsonObject(path, objectOut, 0x4A435554, 1, errorOut);
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
    continuityByClip[clipId] = continuityRoot;
    artifactRoot[QStringLiteral("schema")] = QStringLiteral("jcut_facedetections_v1");
    setContinuityFacestreamsByClipObject(&artifactRoot, continuityByClip);
    if (artifactRootOut) {
        *artifactRootOut = artifactRoot;
    }
    return engine.saveFacestreamArtifact(transcriptPath, artifactRoot);
}

} // namespace jcut::facedetections
