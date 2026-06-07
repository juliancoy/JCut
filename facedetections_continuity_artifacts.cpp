#include "facedetections_runtime.h"

#include "facedetections_artifact_utils.h"
#include "json_io_utils.h"
#include "speakers_tab_internal.h"
#include "transcript_engine.h"

#include <QDateTime>
#include <QDataStream>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QMutex>
#include <QMutexLocker>
#include <QSet>

#include <limits>
#include <string>

namespace jcut::facedetections {
namespace {

QJsonArray stringListToJsonArray(const QStringList& values)
{
    QJsonArray array;
    for (const QString& value : values) {
        array.push_back(value);
    }
    return array;
}

bool nearlyEqual(qreal a, qreal b, qreal tolerance)
{
    return qAbs(a - b) <= tolerance;
}
constexpr qint64 kMaxInteractiveContinuityObjectBytes = 128ll * 1024ll * 1024ll;
constexpr quint32 kIndexedTrackArtifactMagic = 0x4A465449; // JFTI
constexpr quint32 kIndexedFrameArtifactMagic = 0x4A464649; // JFFI
constexpr quint32 kIndexedArtifactVersion = 1;

struct TrackArtifactIndexEntry {
    int trackId = -1;
    QString streamId;
    QString source;
    qint64 minFrame = -1;
    qint64 maxFrame = -1;
    int detectionCount = 0;
    qint64 typicalFrameStep = 1;
    qint64 dataOffset = -1;
    quint32 compressedSize = 0;
};

struct IndexedObjectEntry {
    QString type;
    qint64 frame = -1;
    qint64 dataOffset = -1;
    quint32 compressedSize = 0;
};

struct TrackArtifactIndex {
    QString path;
    QString dataPath;
    qint64 size = -1;
    qint64 mtimeMs = -1;
    QString frameDomain = QStringLiteral("source_absolute");
    QString schema;
    QString video;
    QString backend;
    QVector<TrackArtifactIndexEntry> entries;
    QVector<IndexedObjectEntry> frameSummaryEntries;
    QHash<qint64, QJsonObject> loadedTrackRecordsByDataOffset;
    QHash<QString, FacestreamTrack> loadedTrackModelsByCacheKey;
};

struct FrameArtifactIndex {
    QString path;
    QString dataPath;
    qint64 size = -1;
    qint64 mtimeMs = -1;
    QString frameDomain;
    QString schema;
    QString video;
    QString backend;
    QVector<IndexedObjectEntry> entries;
};

QMutex& trackArtifactIndexMutex()
{
    static QMutex mutex;
    return mutex;
}

QHash<QString, TrackArtifactIndex>& trackArtifactIndices()
{
    static QHash<QString, TrackArtifactIndex> indices;
    return indices;
}

QMutex& frameArtifactIndexMutex()
{
    static QMutex mutex;
    return mutex;
}

QHash<QString, FrameArtifactIndex>& frameArtifactIndices()
{
    static QHash<QString, FrameArtifactIndex> indices;
    return indices;
}

quint32 binaryArtifactMagicAtPath(const QString& path)
{
    QFile file(path.trimmed());
    if (!file.open(QIODevice::ReadOnly)) {
        return 0;
    }
    QDataStream stream(&file);
    stream.setVersion(QDataStream::Qt_6_0);
    quint32 magic = 0;
    stream >> magic;
    return stream.status() == QDataStream::Ok ? magic : 0;
}

bool indexedArtifactToRoot(const QString& path, QJsonObject* rootOut);

qint64 minFrameForTrackRecord(const QJsonObject& trackRecord)
{
    qint64 minFrame = std::numeric_limits<qint64>::max();
    for (const QJsonValue& detValue : trackRecord.value(QStringLiteral("detections")).toArray()) {
        const qint64 frame = detValue.toObject().value(QStringLiteral("frame")).toVariant().toLongLong();
        minFrame = qMin(minFrame, frame);
    }
    return minFrame == std::numeric_limits<qint64>::max() ? -1 : minFrame;
}

qint64 maxFrameForTrackRecord(const QJsonObject& trackRecord)
{
    qint64 maxFrame = std::numeric_limits<qint64>::min();
    for (const QJsonValue& detValue : trackRecord.value(QStringLiteral("detections")).toArray()) {
        const qint64 frame = detValue.toObject().value(QStringLiteral("frame")).toVariant().toLongLong();
        maxFrame = qMax(maxFrame, frame);
    }
    return maxFrame == std::numeric_limits<qint64>::min() ? -1 : maxFrame;
}

qint64 typicalFrameStepForTrackRecord(const QJsonObject& trackRecord)
{
    QVector<int64_t> frames;
    const QJsonArray detections = trackRecord.value(QStringLiteral("detections")).toArray();
    frames.reserve(detections.size());
    for (const QJsonValue& detValue : detections) {
        frames.push_back(detValue.toObject().value(QStringLiteral("frame")).toVariant().toLongLong());
    }
    return qMax<int64_t>(1, facedetectionsTypicalFrameStep(frames));
}

QJsonObject trackSummaryObject(const TrackArtifactIndexEntry& entry,
                               const QString& frameDomain)
{
    QJsonObject summary;
    summary[QStringLiteral("track_id")] = entry.trackId;
    summary[QStringLiteral("stream_id")] =
        entry.streamId.isEmpty() ? QStringLiteral("T%1").arg(entry.trackId) : entry.streamId;
    summary[QStringLiteral("frame_domain")] = frameDomain;
    summary[QStringLiteral("keyframe_count")] = entry.detectionCount;
    summary[QStringLiteral("min_frame")] = entry.minFrame;
    summary[QStringLiteral("max_frame")] = entry.maxFrame;
    summary[QStringLiteral("typical_frame_step")] = entry.typicalFrameStep;
    if (!entry.source.isEmpty()) {
        summary[QStringLiteral("source")] = entry.source;
    }
    return summary;
}

FacestreamTrackSummary trackSummaryModelFromIndexEntry(const TrackArtifactIndexEntry& entry,
                                                       const QString& frameDomain,
                                                       const QString& detectorMode)
{
    FacestreamTrackSummary summary;
    summary.trackId = entry.trackId;
    summary.streamId =
        entry.streamId.isEmpty() && entry.trackId >= 0 ? QStringLiteral("T%1").arg(entry.trackId) : entry.streamId;
    summary.source = entry.source.isEmpty() ? detectorMode : entry.source;
    summary.frameDomain = frameDomain.trimmed();
    summary.minFrame = entry.minFrame;
    summary.maxFrame = entry.maxFrame;
    summary.keyframeCount = entry.detectionCount;
    summary.typicalFrameStep = qMax<qint64>(1, entry.typicalFrameStep);
    return summary;
}

FacestreamTrackSummary trackSummaryModelFromObject(const QJsonObject& summary)
{
    FacestreamTrackSummary model;
    model.trackId = summary.value(QStringLiteral("track_id")).toInt(-1);
    model.streamId = summary.value(QStringLiteral("stream_id")).toString().trimmed();
    model.source = summary.value(QStringLiteral("source")).toString().trimmed();
    model.frameDomain = summary.value(QStringLiteral("frame_domain")).toString().trimmed();
    model.minFrame = summary.value(QStringLiteral("min_frame")).toVariant().toLongLong();
    model.maxFrame = summary.value(QStringLiteral("max_frame")).toVariant().toLongLong();
    model.keyframeCount = summary.value(QStringLiteral("keyframe_count")).toInt(0);
    model.typicalFrameStep =
        qMax<qint64>(1, summary.value(QStringLiteral("typical_frame_step")).toVariant().toLongLong());
    return model;
}

bool frameIsInTranscriptDialogue(qint64 frame, const QJsonObject& transcriptRoot)
{
    const QJsonArray segments = transcriptRoot.value(QStringLiteral("segments")).toArray();
    if (segments.isEmpty()) {
        return true;
    }
    const double t = static_cast<double>(frame) / static_cast<double>(kTimelineFps);
    for (const QJsonValue& segValue : segments) {
        const QJsonArray words = segValue.toObject().value(QStringLiteral("words")).toArray();
        for (const QJsonValue& wordValue : words) {
            const QJsonObject wordObj = wordValue.toObject();
            if (wordObj.value(QStringLiteral("skipped")).toBool(false)) {
                continue;
            }
            const double ws = wordObj.value(QStringLiteral("start")).toDouble(-1.0);
            const double we = wordObj.value(QStringLiteral("end")).toDouble(-1.0);
            if (ws >= 0.0 && we >= ws && t >= ws && t <= we) {
                return true;
            }
        }
    }
    return false;
}

FacestreamTrack trackModelFromRawRecordWithSummary(const QJsonObject& trackObj,
                                                   const FacestreamTrackSummary& summary,
                                                   bool onlyDialogue,
                                                   const QJsonObject& transcriptRoot)
{
    FacestreamTrack track;
    track.summary = summary;
    const QJsonArray detections = trackObj.value(QStringLiteral("detections")).toArray();
    track.keyframes.reserve(detections.size());
    for (const QJsonValue& detValue : detections) {
        const QJsonObject det = detValue.toObject();
        const qint64 frame = qMax<qint64>(
            0,
            det.value(QStringLiteral("frame")).toVariant().toLongLong());
            if (onlyDialogue && !frameIsInTranscriptDialogue(frame, transcriptRoot)) {
                continue;
            }
        FacestreamKeyframe keyframe;
        keyframe.frame = frame;
        keyframe.sourceFrame = frame;
        keyframe.x = static_cast<float>(
            qBound(0.0, det.value(QStringLiteral("x")).toDouble(0.5), 1.0));
        keyframe.y = static_cast<float>(
            qBound(0.0, det.value(QStringLiteral("y")).toDouble(0.5), 1.0));
        keyframe.box = static_cast<float>(
            qBound(0.01, det.value(QStringLiteral("box")).toDouble(0.2), 1.0));
        keyframe.confidence = static_cast<float>(
            qBound(0.0, det.value(QStringLiteral("score")).toDouble(0.0), 1.0));
        const qreal frameWidth = det.value(QStringLiteral("frame_width")).toDouble(0.0);
        const qreal frameHeight = det.value(QStringLiteral("frame_height")).toDouble(0.0);
        const QRectF exactTrackBox(det.value(QStringLiteral("track_box_x")).toDouble(-1.0),
                                   det.value(QStringLiteral("track_box_y")).toDouble(-1.0),
                                   det.value(QStringLiteral("track_box_w")).toDouble(0.0),
                                   det.value(QStringLiteral("track_box_h")).toDouble(0.0));
        if (frameWidth > 1.0 &&
            frameHeight > 1.0 &&
            exactTrackBox.isValid() &&
            !exactTrackBox.isEmpty()) {
            keyframe.boxNorm = QRectF(exactTrackBox.x() / frameWidth,
                                      exactTrackBox.y() / frameHeight,
                                      exactTrackBox.width() / frameWidth,
                                      exactTrackBox.height() / frameHeight)
                                   .intersected(QRectF(0.0, 0.0, 1.0, 1.0));
        }
        track.keyframes.push_back(keyframe);
    }
    if (track.keyframes.isEmpty()) {
        return {};
    }
    return track;
}

FacestreamTrack trackModelFromRawRecord(const QJsonObject& trackObj,
                                        const QString& frameDomain,
                                        const QString& detectorMode,
                                        bool onlyDialogue,
                                        const QJsonObject& transcriptRoot)
{
    FacestreamTrackSummary summary;
    summary.trackId = trackObj.value(QStringLiteral("track_id")).toInt(-1);
    summary.streamId =
        trackObj.value(QStringLiteral("stream_id")).toString(
            summary.trackId >= 0 ? QStringLiteral("T%1").arg(summary.trackId) : QString{});
    summary.source = trackObj.value(QStringLiteral("source")).toString();
    summary.minFrame = minFrameForTrackRecord(trackObj);
    summary.maxFrame = maxFrameForTrackRecord(trackObj);
    summary.keyframeCount = trackObj.value(QStringLiteral("detections")).toArray().size();
    summary.typicalFrameStep = typicalFrameStepForTrackRecord(trackObj);
    summary.frameDomain = frameDomain.trimmed();
    if (summary.source.isEmpty()) {
        summary.source = detectorMode;
    }
    if (summary.streamId.isEmpty() && summary.trackId >= 0) {
        summary.streamId = QStringLiteral("T%1").arg(summary.trackId);
    }
    return trackModelFromRawRecordWithSummary(trackObj, summary, onlyDialogue, transcriptRoot);
}

FacestreamFrameDetections frameDetectionModelFromRecord(const QJsonObject& frameObj)
{
    FacestreamFrameDetections frame;
    frame.frame = frameObj.value(QStringLiteral("frame")).toVariant().toLongLong();
    if (frame.frame < 0) {
        return {};
    }
    const QJsonArray rows = frameObj.value(QStringLiteral("detections")).toArray();
    frame.detections.reserve(rows.size());
    for (const QJsonValue& detValue : rows) {
        const QJsonObject detObj = detValue.toObject();
        const qreal frameWidth = qMax<qreal>(
            1.0,
            frameObj.value(QStringLiteral("frame_width")).toDouble(
                detObj.value(QStringLiteral("frame_width")).toDouble(0.0)));
        const qreal frameHeight = qMax<qreal>(
            1.0,
            frameObj.value(QStringLiteral("frame_height")).toDouble(
                detObj.value(QStringLiteral("frame_height")).toDouble(0.0)));
        frame.frameWidth = frameWidth;
        frame.frameHeight = frameHeight;
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
        FacestreamDetection detection;
        detection.frame = frame.frame;
        detection.box = QRectF(x, y, w, h).intersected(QRectF(0.0, 0.0, 1.0, 1.0));
        detection.confidence = static_cast<float>(qBound<qreal>(
            0.0,
            detObj.value(QStringLiteral("confidence")).toDouble(
                detObj.value(QStringLiteral("score")).toDouble(0.0)),
            1.0));
        detection.trackId = detObj.value(QStringLiteral("track_id")).toInt(-1);
        if (detection.box.isValid() && !detection.box.isEmpty()) {
            frame.detections.push_back(detection);
        }
    }
    return frame;
}

QJsonObject streamSummaryObject(const QJsonObject& streamObj)
{
    QJsonObject summary;
    const int trackId = streamObj.value(QStringLiteral("track_id")).toInt(-1);
    summary[QStringLiteral("track_id")] = trackId;
    summary[QStringLiteral("stream_id")] = streamObj.value(QStringLiteral("stream_id"))
                                               .toString(QStringLiteral("T%1").arg(trackId));
    summary[QStringLiteral("frame_domain")] =
        streamObj.value(QStringLiteral("frame_domain")).toString();
    summary[QStringLiteral("source")] = streamObj.value(QStringLiteral("source")).toString();

    qint64 minFrame = std::numeric_limits<qint64>::max();
    qint64 maxFrame = std::numeric_limits<qint64>::min();
    QVector<int64_t> frames;
    const QJsonArray keyframes = streamObj.value(QStringLiteral("keyframes")).toArray();
    frames.reserve(keyframes.size());
    for (const QJsonValue& keyframeValue : keyframes) {
        const qint64 frame =
            keyframeValue.toObject().value(QStringLiteral("frame")).toVariant().toLongLong();
        minFrame = qMin(minFrame, frame);
        maxFrame = qMax(maxFrame, frame);
        frames.push_back(frame);
    }
    summary[QStringLiteral("keyframe_count")] = keyframes.size();
    summary[QStringLiteral("min_frame")] =
        minFrame == std::numeric_limits<qint64>::max() ? -1 : minFrame;
    summary[QStringLiteral("max_frame")] =
        maxFrame == std::numeric_limits<qint64>::min() ? -1 : maxFrame;
    summary[QStringLiteral("typical_frame_step")] = static_cast<qint64>(
        frames.isEmpty() ? 1 : qMax<int64_t>(1, facedetectionsTypicalFrameStep(frames)));
    return summary;
}

QJsonObject rawTrackSummaryObject(const QJsonObject& trackObj,
                                  const QString& frameDomain)
{
    QJsonObject summary;
    const int trackId = trackObj.value(QStringLiteral("track_id")).toInt(-1);
    summary[QStringLiteral("track_id")] = trackId;
    summary[QStringLiteral("stream_id")] =
        trackObj.value(QStringLiteral("stream_id")).toString(QStringLiteral("T%1").arg(trackId));
    summary[QStringLiteral("frame_domain")] = frameDomain;
    summary[QStringLiteral("source")] = trackObj.value(QStringLiteral("source")).toString();
    summary[QStringLiteral("keyframe_count")] =
        trackObj.value(QStringLiteral("detections")).toArray().size();
    summary[QStringLiteral("min_frame")] = minFrameForTrackRecord(trackObj);
    summary[QStringLiteral("max_frame")] = maxFrameForTrackRecord(trackObj);
    summary[QStringLiteral("typical_frame_step")] = typicalFrameStepForTrackRecord(trackObj);
    return summary;
}

bool trackSummaryMatchesFrame(const QJsonObject& summary,
                              qint64 frame,
                              qint64 extraWindowFrames)
{
    const qint64 minFrame = summary.value(QStringLiteral("min_frame")).toVariant().toLongLong();
    const qint64 maxFrame = summary.value(QStringLiteral("max_frame")).toVariant().toLongLong();
    if (minFrame < 0 || maxFrame < 0) {
        return false;
    }
    const qint64 typicalStep = qMax<qint64>(
        1,
        summary.value(QStringLiteral("typical_frame_step")).toVariant().toLongLong());
    const qint64 holdFrames =
        qMax<int64_t>(0, facedetectionsMaxEdgeHoldFrames(typicalStep)) +
        qMax<int64_t>(0, extraWindowFrames);
    return frame >= (minFrame - holdFrames) && frame <= (maxFrame + holdFrames);
}

bool readIndexedObjectAtOffset(const QString& dataPath,
                               qint64 dataOffset,
                               quint32 compressedSize,
                               QJsonObject* recordOut)
{
    if (recordOut) {
        *recordOut = QJsonObject{};
    }
    QFile file(dataPath);
    if (!file.open(QIODevice::ReadOnly) ||
        dataOffset < 0 ||
        compressedSize > static_cast<quint32>(std::numeric_limits<int>::max()) ||
        !file.seek(dataOffset)) {
        return false;
    }

    QByteArray compressed(static_cast<int>(compressedSize), Qt::Uninitialized);
    if (compressedSize > 0 &&
        file.read(compressed.data(), static_cast<int>(compressedSize)) !=
            static_cast<int>(compressedSize)) {
        return false;
    }

    QJsonObject record;
    if (!jcut::jsonio::parseCborObjectBytes(qUncompress(compressed), &record, nullptr)) {
        return false;
    }
    if (recordOut) {
        *recordOut = record;
    }
    return true;
}

QString resolvedIndexedDataPath(const QFileInfo& indexInfo, const QString& storedDataPath)
{
    const QString trimmed = storedDataPath.trimmed();
    if (trimmed.isEmpty()) {
        return {};
    }
    const QFileInfo dataInfo(trimmed);
    return dataInfo.isAbsolute()
        ? dataInfo.absoluteFilePath()
        : indexInfo.dir().absoluteFilePath(trimmed);
}

bool rebuildTrackArtifactIndex(const QString& path, TrackArtifactIndex* indexOut)
{
    if (!indexOut) {
        return false;
    }

    QFileInfo info(path);
    if (!info.exists() || !info.isFile()) {
        return false;
    }

    QFile file(info.absoluteFilePath());
    if (!file.open(QIODevice::ReadOnly)) {
        return false;
    }

    QDataStream stream(&file);
    stream.setVersion(QDataStream::Qt_6_0);

    TrackArtifactIndex index;
    index.path = info.absoluteFilePath();
    index.size = info.size();
    index.mtimeMs = info.lastModified().toMSecsSinceEpoch();
    quint32 magic = 0;
    quint32 version = 0;
    QString dataPath;
    qint64 entryCount = 0;
    stream >> magic
           >> version
           >> index.schema
           >> index.video
           >> index.backend
           >> index.frameDomain
           >> dataPath
           >> entryCount;
    if (stream.status() != QDataStream::Ok ||
        magic != kIndexedTrackArtifactMagic ||
        version != kIndexedArtifactVersion ||
        entryCount < 0) {
        return false;
    }
    index.dataPath = resolvedIndexedDataPath(info, dataPath);
    if (index.dataPath.isEmpty() || !QFileInfo::exists(index.dataPath)) {
        return false;
    }
    for (qint64 i = 0; i < entryCount; ++i) {
        QString type;
        stream >> type;
        if (type == QStringLiteral("track")) {
            TrackArtifactIndexEntry entry;
            stream >> entry.trackId
                   >> entry.streamId
                   >> entry.source
                   >> entry.minFrame
                   >> entry.maxFrame
                   >> entry.detectionCount
                   >> entry.typicalFrameStep
                   >> entry.dataOffset
                   >> entry.compressedSize;
            if (stream.status() != QDataStream::Ok) {
                return false;
            }
            index.entries.push_back(entry);
        } else if (type == QStringLiteral("frame_summary")) {
            IndexedObjectEntry entry;
            entry.type = type;
            stream >> entry.frame >> entry.dataOffset >> entry.compressedSize;
            if (stream.status() != QDataStream::Ok) {
                return false;
            }
            index.frameSummaryEntries.push_back(entry);
        } else {
            return false;
        }
    }

    *indexOut = index;
    return true;
}

bool trackArtifactIndexForPath(const QString& path, TrackArtifactIndex* indexOut)
{
    if (!indexOut) {
        return false;
    }
    const QString normalizedPath = QFileInfo(path).absoluteFilePath();
    if (normalizedPath.isEmpty()) {
        return false;
    }

    QFileInfo info(normalizedPath);
    if (!info.exists() || !info.isFile()) {
        return false;
    }

    QMutexLocker locker(&trackArtifactIndexMutex());
    auto& indices = trackArtifactIndices();
    auto it = indices.find(normalizedPath);
    if (it != indices.end() &&
        it->size == info.size() &&
        it->mtimeMs == info.lastModified().toMSecsSinceEpoch()) {
        *indexOut = it.value();
        return true;
    }

    TrackArtifactIndex rebuilt;
    if (!rebuildTrackArtifactIndex(normalizedPath, &rebuilt)) {
        return false;
    }
    it = indices.insert(normalizedPath, rebuilt);
    *indexOut = it.value();
    return true;
}

bool rebuildFrameArtifactIndex(const QString& path, FrameArtifactIndex* indexOut)
{
    if (!indexOut) {
        return false;
    }
    QFileInfo info(path);
    if (!info.exists() || !info.isFile()) {
        return false;
    }
    QFile file(info.absoluteFilePath());
    if (!file.open(QIODevice::ReadOnly)) {
        return false;
    }

    QDataStream stream(&file);
    stream.setVersion(QDataStream::Qt_6_0);
    FrameArtifactIndex index;
    index.path = info.absoluteFilePath();
    index.size = info.size();
    index.mtimeMs = info.lastModified().toMSecsSinceEpoch();

    quint32 magic = 0;
    quint32 version = 0;
    QString dataPath;
    qint64 entryCount = 0;
    stream >> magic
           >> version
           >> index.schema
           >> index.video
           >> index.backend
           >> index.frameDomain
           >> dataPath
           >> entryCount;
    if (stream.status() != QDataStream::Ok ||
        magic != kIndexedFrameArtifactMagic ||
        version != kIndexedArtifactVersion ||
        entryCount < 0) {
        return false;
    }
    index.dataPath = resolvedIndexedDataPath(info, dataPath);
    if (index.dataPath.isEmpty() || !QFileInfo::exists(index.dataPath)) {
        return false;
    }
    for (qint64 i = 0; i < entryCount; ++i) {
        IndexedObjectEntry entry;
        stream >> entry.type >> entry.frame >> entry.dataOffset >> entry.compressedSize;
        if (stream.status() != QDataStream::Ok || entry.type != QStringLiteral("frame")) {
            return false;
        }
        index.entries.push_back(entry);
    }
    std::sort(index.entries.begin(), index.entries.end(), [](const IndexedObjectEntry& a,
                                                             const IndexedObjectEntry& b) {
        if (a.frame == b.frame) {
            return a.dataOffset < b.dataOffset;
        }
        return a.frame < b.frame;
    });

    *indexOut = index;
    return true;
}

bool frameArtifactIndexForPath(const QString& path, FrameArtifactIndex* indexOut)
{
    if (!indexOut) {
        return false;
    }
    const QString normalizedPath = QFileInfo(path).absoluteFilePath();
    if (normalizedPath.isEmpty()) {
        return false;
    }

    QFileInfo info(normalizedPath);
    if (!info.exists() || !info.isFile()) {
        return false;
    }

    QMutexLocker locker(&frameArtifactIndexMutex());
    auto& indices = frameArtifactIndices();
    auto it = indices.find(normalizedPath);
    if (it != indices.end() &&
        it->size == info.size() &&
        it->mtimeMs == info.lastModified().toMSecsSinceEpoch()) {
        *indexOut = it.value();
        return true;
    }

    FrameArtifactIndex rebuilt;
    if (!rebuildFrameArtifactIndex(normalizedPath, &rebuilt)) {
        return false;
    }
    it = indices.insert(normalizedPath, rebuilt);
    *indexOut = it.value();
    return true;
}

QJsonObject cachedTrackRecordForEntry(const QString& path,
                                      const TrackArtifactIndexEntry& entry)
{
    const QString normalizedPath = QFileInfo(path).absoluteFilePath();
    if (normalizedPath.isEmpty() || entry.dataOffset < 0 || entry.compressedSize == 0) {
        return {};
    }

    {
        QMutexLocker locker(&trackArtifactIndexMutex());
        auto it = trackArtifactIndices().find(normalizedPath);
        if (it != trackArtifactIndices().end()) {
            const auto cached = it->loadedTrackRecordsByDataOffset.constFind(entry.dataOffset);
            if (cached != it->loadedTrackRecordsByDataOffset.cend()) {
                return cached.value();
            }
        }
    }

    TrackArtifactIndex index;
    if (!trackArtifactIndexForPath(normalizedPath, &index)) {
        return {};
    }
    QJsonObject record;
    if (!readIndexedObjectAtOffset(index.dataPath, entry.dataOffset, entry.compressedSize, &record)) {
        return {};
    }

    QMutexLocker locker(&trackArtifactIndexMutex());
    auto it = trackArtifactIndices().find(normalizedPath);
    if (it == trackArtifactIndices().end()) {
        return record;
    }
    if (it->loadedTrackRecordsByDataOffset.size() >= 256) {
        it->loadedTrackRecordsByDataOffset.clear();
    }
    it->loadedTrackRecordsByDataOffset.insert(entry.dataOffset, record);
    return record;
}

QString trackModelCacheKeyForEntry(const TrackArtifactIndexEntry& entry,
                                   const QString& frameDomain,
                                   const QString& detectorMode)
{
    return QStringLiteral("%1:%2:%3:%4")
        .arg(entry.dataOffset)
        .arg(entry.compressedSize)
        .arg(frameDomain.trimmed(), detectorMode.trimmed());
}

FacestreamTrack cachedTrackModelForEntry(const QString& path,
                                         const TrackArtifactIndexEntry& entry,
                                         const QString& frameDomain,
                                         const QString& detectorMode,
                                         bool onlyDialogue,
                                         const QJsonObject& transcriptRoot)
{
    // Dialogue filtering depends on mutable transcript content; keep that path uncached for correctness.
    if (onlyDialogue) {
        QJsonObject record = cachedTrackRecordForEntry(path, entry);
        if (record.isEmpty()) {
            return {};
        }
        record.remove(QStringLiteral("type"));
        return trackModelFromRawRecordWithSummary(
            record,
            trackSummaryModelFromIndexEntry(entry, frameDomain, detectorMode),
            onlyDialogue,
            transcriptRoot);
    }

    const QString normalizedPath = QFileInfo(path).absoluteFilePath();
    const QString cacheKey = trackModelCacheKeyForEntry(entry, frameDomain, detectorMode);
    {
        QMutexLocker locker(&trackArtifactIndexMutex());
        auto it = trackArtifactIndices().find(normalizedPath);
        if (it != trackArtifactIndices().end()) {
            const auto cached = it->loadedTrackModelsByCacheKey.constFind(cacheKey);
            if (cached != it->loadedTrackModelsByCacheKey.cend()) {
                return cached.value();
            }
        }
    }

    QJsonObject record = cachedTrackRecordForEntry(normalizedPath, entry);
    if (record.isEmpty()) {
        return {};
    }
    record.remove(QStringLiteral("type"));
    FacestreamTrack track = trackModelFromRawRecordWithSummary(
        record,
        trackSummaryModelFromIndexEntry(entry, frameDomain, detectorMode),
        false,
        {});
    if (track.keyframes.isEmpty()) {
        return {};
    }

    QMutexLocker locker(&trackArtifactIndexMutex());
    auto it = trackArtifactIndices().find(normalizedPath);
    if (it == trackArtifactIndices().end()) {
        return track;
    }
    if (it->loadedTrackModelsByCacheKey.size() >= 512) {
        it->loadedTrackModelsByCacheKey.clear();
    }
    it->loadedTrackModelsByCacheKey.insert(cacheKey, track);
    return track;
}

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

    TrackArtifactIndex index;
    if (!trackArtifactIndexForPath(trimmedPath, &index)) {
        return false;
    }

    QJsonArray tracks;
    for (const TrackArtifactIndexEntry& entry : std::as_const(index.entries)) {
        if ((entry.trackId >= 0 && trackIds.contains(entry.trackId)) ||
            (!entry.streamId.isEmpty() && streamIds.contains(entry.streamId)) ||
            (entry.trackId >= 0 && streamIds.contains(QStringLiteral("T%1").arg(entry.trackId)))) {
            QJsonObject record = cachedTrackRecordForEntry(trimmedPath, entry);
            if (record.isEmpty()) {
                continue;
            }
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
    if (!indexedArtifactToRoot(trimmedPath, &root)) {
        return {};
    }
    return root;
}

bool indexedArtifactToRoot(const QString& path, QJsonObject* rootOut)
{
    const quint32 magic = binaryArtifactMagicAtPath(path);
    QJsonObject root;
    if (magic == kIndexedTrackArtifactMagic) {
        TrackArtifactIndex index;
        if (!trackArtifactIndexForPath(path, &index)) {
            return false;
        }
        root[QStringLiteral("schema")] = index.schema;
        root[QStringLiteral("video")] = index.video;
        root[QStringLiteral("backend")] = index.backend;
        root[QStringLiteral("frame_domain")] = index.frameDomain;
        QJsonArray tracks;
        for (const TrackArtifactIndexEntry& entry : std::as_const(index.entries)) {
            QJsonObject track = cachedTrackRecordForEntry(path, entry);
            if (track.isEmpty()) {
                return false;
            }
            track.remove(QStringLiteral("type"));
            tracks.push_back(track);
        }
        QJsonArray frameSummaries;
        for (const IndexedObjectEntry& entry : std::as_const(index.frameSummaryEntries)) {
            QJsonObject summary;
            if (!readIndexedObjectAtOffset(index.dataPath, entry.dataOffset, entry.compressedSize, &summary)) {
                return false;
            }
            summary.remove(QStringLiteral("type"));
            frameSummaries.push_back(summary);
        }
        if (!tracks.isEmpty()) {
            root[QStringLiteral("tracks")] = tracks;
        }
        if (!frameSummaries.isEmpty()) {
            root[QStringLiteral("frame_summaries")] = frameSummaries;
        }
    } else if (magic == kIndexedFrameArtifactMagic) {
        FrameArtifactIndex index;
        if (!frameArtifactIndexForPath(path, &index)) {
            return false;
        }
        root[QStringLiteral("schema")] = index.schema;
        root[QStringLiteral("video")] = index.video;
        root[QStringLiteral("backend")] = index.backend;
        root[QStringLiteral("frame_domain")] = index.frameDomain;
        QJsonArray frames;
        for (const IndexedObjectEntry& entry : std::as_const(index.entries)) {
            QJsonObject frameRecord;
            if (!readIndexedObjectAtOffset(index.dataPath, entry.dataOffset, entry.compressedSize, &frameRecord)) {
                return false;
            }
            frameRecord.remove(QStringLiteral("type"));
            frames.push_back(frameRecord);
        }
        if (!frames.isEmpty()) {
            root[QStringLiteral("frames")] = frames;
        }
    } else {
        return false;
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
    return {};
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

QJsonArray rawFramesForContinuityRootImpl(const QJsonObject& continuityRoot)
{
    const QJsonArray inlineFrames = continuityRoot.value(QStringLiteral("raw_frames")).toArray();
    if (!inlineFrames.isEmpty()) {
        return inlineFrames;
    }
    return {};
}

QString rawFramesArtifactPathForContinuityRoot(const QJsonObject& continuityRoot)
{
    QString path = continuityRoot.value(QStringLiteral("raw_frames_artifact_path")).toString().trimmed();
    if (!path.isEmpty()) {
        return path;
    }
    const QString artifactDir =
        continuityRoot.value(QStringLiteral("imported_from_artifact_dir")).toString().trimmed();
    return artifactDir.isEmpty()
        ? QString()
        : QDir(artifactDir).absoluteFilePath(QStringLiteral("detections.idx"));
}

QJsonArray rawFrameRecordsNearPath(const QString& path,
                                   qint64 frame,
                                   qint64 extraWindowFrames,
                                   bool* validRecordFileOut)
{
    if (validRecordFileOut) {
        *validRecordFileOut = false;
    }
    QJsonArray frames;
    if (path.trimmed().isEmpty()) {
        return frames;
    }
    FrameArtifactIndex index;
    if (!frameArtifactIndexForPath(path, &index)) {
        return frames;
    }
    if (validRecordFileOut) {
        *validRecordFileOut = true;
    }
    const qint64 window = qMax<qint64>(0, extraWindowFrames);
    const qint64 lowerFrame = frame - window;
    const qint64 upperFrame = frame + window;
    for (const IndexedObjectEntry& entry : std::as_const(index.entries)) {
        if (entry.frame < 0) {
            continue;
        }
        if (entry.frame > upperFrame) {
            break;
        }
        if (entry.frame >= lowerFrame) {
            QJsonObject record;
            if (!readIndexedObjectAtOffset(index.dataPath, entry.dataOffset, entry.compressedSize, &record)) {
                return {};
            }
            record.remove(QStringLiteral("type"));
            const qint64 recordFrame = record.contains(QStringLiteral("frame"))
                ? record.value(QStringLiteral("frame")).toVariant().toLongLong()
                : -1;
            if (recordFrame >= lowerFrame && recordFrame <= upperFrame) {
                frames.append(record);
            }
        }
    }
    return frames;
}

QJsonArray rawFramesNearFrameForContinuityRootImpl(const QJsonObject& continuityRoot,
                                                   qint64 frame,
                                                   qint64 extraWindowFrames)
{
    const qint64 window = qMax<qint64>(0, extraWindowFrames);
    const qint64 lowerFrame = frame - window;
    const qint64 upperFrame = frame + window;
    const QJsonArray inlineFrames = continuityRoot.value(QStringLiteral("raw_frames")).toArray();
    if (!inlineFrames.isEmpty()) {
        QJsonArray frames;
        for (const QJsonValue& frameValue : inlineFrames) {
            const QJsonObject frameObj = frameValue.toObject();
            const qint64 rawFrame = frameObj.contains(QStringLiteral("frame"))
                ? frameObj.value(QStringLiteral("frame")).toVariant().toLongLong()
                : -1;
            if (rawFrame >= lowerFrame && rawFrame <= upperFrame) {
                frames.append(frameObj);
            }
        }
        return frames;
    }

    bool validRecordFile = false;
    const QJsonArray recordFrames = rawFrameRecordsNearPath(
        rawFramesArtifactPathForContinuityRoot(continuityRoot),
        frame,
        extraWindowFrames,
        &validRecordFile);
    if (validRecordFile) {
        return recordFrames;
    }
    return {};
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
        const QJsonArray rawFrames = rawFramesForContinuityRootImpl(continuityRoot);
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

QJsonArray rawFramesForContinuityRoot(const QJsonObject& continuityRoot)
{
    return rawFramesForContinuityRootImpl(continuityRoot);
}

QJsonArray rawFramesNearFrameForContinuityRoot(const QJsonObject& continuityRoot,
                                               int64_t frame,
                                               int64_t extraWindowFrames)
{
    return rawFramesNearFrameForContinuityRootImpl(continuityRoot, frame, extraWindowFrames);
}

QVector<FacestreamFrameDetections> frameDetectionModelsNearFrameForRoot(
    const QJsonObject& continuityRoot,
    int64_t frame,
    int64_t extraWindowFrames)
{
    QVector<FacestreamFrameDetections> models;
    const qint64 window = qMax<qint64>(0, extraWindowFrames);
    const qint64 lowerFrame = frame - window;
    const qint64 upperFrame = frame + window;
    const QJsonArray inlineFrames = continuityRoot.value(QStringLiteral("raw_frames")).toArray();
    if (!inlineFrames.isEmpty()) {
        models.reserve(inlineFrames.size());
        for (const QJsonValue& frameValue : inlineFrames) {
            const QJsonObject frameObj = frameValue.toObject();
            const qint64 rawFrame = frameObj.contains(QStringLiteral("frame"))
                ? frameObj.value(QStringLiteral("frame")).toVariant().toLongLong()
                : -1;
            if (rawFrame < lowerFrame || rawFrame > upperFrame) {
                continue;
            }
            FacestreamFrameDetections model = frameDetectionModelFromRecord(frameObj);
            if (model.frame >= 0) {
                models.push_back(model);
            }
        }
        return models;
    }

    FrameArtifactIndex index;
    if (!frameArtifactIndexForPath(rawFramesArtifactPathForContinuityRoot(continuityRoot), &index)) {
        return {};
    }
    for (const IndexedObjectEntry& entry : std::as_const(index.entries)) {
        if (entry.frame < 0) {
            continue;
        }
        if (entry.frame > upperFrame) {
            break;
        }
        if (entry.frame < lowerFrame) {
            continue;
        }
        QJsonObject record;
        if (!readIndexedObjectAtOffset(index.dataPath, entry.dataOffset, entry.compressedSize, &record)) {
            return {};
        }
        record.remove(QStringLiteral("type"));
        const qint64 recordFrame = record.contains(QStringLiteral("frame"))
            ? record.value(QStringLiteral("frame")).toVariant().toLongLong()
            : -1;
        if (recordFrame < lowerFrame || recordFrame > upperFrame) {
            continue;
        }
        FacestreamFrameDetections model = frameDetectionModelFromRecord(record);
        if (model.frame >= 0) {
            models.push_back(model);
        }
    }
    return models;
}

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
            if (onlyDialogue && !segments.isEmpty()) {
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
    if (continuityRoot.contains(QStringLiteral("streams"))) {
        return continuityRoot.value(QStringLiteral("streams")).toArray();
    }

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
    if (continuityRoot.contains(QStringLiteral("streams"))) {
        return continuityRoot.value(QStringLiteral("streams")).toArray();
    }

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

QJsonArray continuityTrackSummariesForRoot(const QJsonObject& continuityRoot,
                                           const QJsonObject& transcriptRoot)
{
    if (continuityRoot.contains(QStringLiteral("streams"))) {
        QJsonArray summaries;
        const QJsonArray storedStreams = continuityRoot.value(QStringLiteral("streams")).toArray();
        for (const QJsonValue& value : storedStreams) {
            summaries.push_back(streamSummaryObject(value.toObject()));
        }
        return summaries;
    }

    const QJsonArray storedStreams = continuityRoot.value(QStringLiteral("streams")).toArray();
    if (!storedStreams.isEmpty()) {
        QJsonArray summaries;
        for (const QJsonValue& value : storedStreams) {
            summaries.push_back(streamSummaryObject(value.toObject()));
        }
        return summaries;
    }

    const QJsonArray inlineTracks = continuityRoot.value(QStringLiteral("raw_tracks")).toArray();
    if (!inlineTracks.isEmpty()) {
        QJsonArray summaries;
        const QString frameDomain = resolvedRawTrackFrameDomain(continuityRoot);
        for (const QJsonValue& value : inlineTracks) {
            summaries.push_back(rawTrackSummaryObject(value.toObject(), frameDomain));
        }
        return summaries;
    }

    const QString rawTracksPath =
        continuityRoot.value(QStringLiteral("raw_tracks_artifact_path")).toString().trimmed();
    if (!rawTracksPath.isEmpty()) {
        TrackArtifactIndex index;
        if (trackArtifactIndexForPath(rawTracksPath, &index)) {
            QJsonArray summaries;
            const QString frameDomain = resolvedRawTrackFrameDomain(continuityRoot);
            for (const TrackArtifactIndexEntry& entry : std::as_const(index.entries)) {
                summaries.push_back(trackSummaryObject(entry, frameDomain));
            }
            return summaries;
        }
    }

    const QString processedArtifactPath =
        continuityRoot.value(QStringLiteral("processed_artifact_path")).toString().trimmed();
    const QString clipId = continuityRoot.value(QStringLiteral("clip_id")).toString().trimmed();
    if (!processedArtifactPath.isEmpty() && !clipId.isEmpty()) {
        QJsonObject processedArtifact;
        if (readBinaryJsonObject(processedArtifactPath, &processedArtifact, nullptr)) {
            return continuityTrackSummariesForRoot(
                continuityRootForClip(processedArtifact, clipId),
                transcriptRoot);
        }
    }

    if (!transcriptRoot.isEmpty()) {
        const QJsonArray derivedStreams = continuityStreamsForRoot(continuityRoot, transcriptRoot);
        if (!derivedStreams.isEmpty()) {
            QJsonArray summaries;
            for (const QJsonValue& value : derivedStreams) {
                summaries.push_back(streamSummaryObject(value.toObject()));
            }
            return summaries;
        }
    }
    return {};
}

QVector<FacestreamTrackSummary> continuityTrackSummaryModelsForRoot(
    const QJsonObject& continuityRoot,
    const QJsonObject& transcriptRoot)
{
    const QJsonArray summaries = continuityTrackSummariesForRoot(continuityRoot, transcriptRoot);
    QVector<FacestreamTrackSummary> models;
    models.reserve(summaries.size());
    for (const QJsonValue& value : summaries) {
        models.push_back(trackSummaryModelFromObject(value.toObject()));
    }
    return models;
}

QVector<FacestreamTrack> continuityTrackModelsNearFrameForRoot(
    const QJsonObject& continuityRoot,
    int64_t frame,
    int64_t extraWindowFrames,
    const QJsonObject& transcriptRoot)
{
    if (continuityRoot.contains(QStringLiteral("streams"))) {
        Q_UNUSED(transcriptRoot);
        const QJsonArray streams = continuityRoot.value(QStringLiteral("streams")).toArray();
        QVector<FacestreamTrack> tracks;
        tracks.reserve(streams.size());
        const QString detectorMode =
            continuityRoot.value(QStringLiteral("detector_mode")).toString().trimmed();
        for (const QJsonValue& value : streams) {
            const QJsonObject streamObj = value.toObject();
            if (!trackSummaryMatchesFrame(streamSummaryObject(streamObj), frame, extraWindowFrames)) {
                continue;
            }
            const QJsonArray singleStream{value};
            const QJsonArray rawTracks = QJsonArray{};
            const QJsonObject streamRoot = buildContinuityRoot(
                continuityRoot.value(QStringLiteral("run_id")).toString(),
                false,
                continuityRoot.value(QStringLiteral("scan_start_frame")).toVariant().toLongLong(),
                continuityRoot.value(QStringLiteral("scan_end_frame")).toVariant().toLongLong(),
                singleStream,
                rawTracks,
                QJsonArray{},
                detectorMode);
            const QJsonArray materialized = continuityStreamsForRoot(streamRoot, QJsonObject{});
            for (const QJsonValue& materializedValue : materialized) {
                const QJsonObject materializedStream = materializedValue.toObject();
                FacestreamTrack track;
                track.summary = trackSummaryModelFromObject(streamSummaryObject(materializedStream));
                for (const QJsonValue& keyframeValue : materializedStream.value(QStringLiteral("keyframes")).toArray()) {
                    const QJsonObject keyframeObj = keyframeValue.toObject();
                    FacestreamKeyframe keyframe;
                    keyframe.frame = keyframeObj.value(QStringLiteral("frame")).toVariant().toLongLong();
                    keyframe.sourceFrame = keyframe.frame;
                    keyframe.x = static_cast<float>(qBound(0.0, keyframeObj.value(QStringLiteral("x")).toDouble(0.5), 1.0));
                    keyframe.y = static_cast<float>(qBound(0.0, keyframeObj.value(QStringLiteral("y")).toDouble(0.5), 1.0));
                    keyframe.box = static_cast<float>(qBound(0.01, keyframeObj.value(QStringLiteral("box_size")).toDouble(0.2), 1.0));
                    keyframe.confidence = static_cast<float>(qBound(0.0, keyframeObj.value(QStringLiteral("confidence")).toDouble(0.0), 1.0));
                    track.keyframes.push_back(keyframe);
                }
                if (!track.keyframes.isEmpty()) {
                    tracks.push_back(track);
                }
            }
        }
        return tracks;
    }

    const QString detectorMode =
        continuityRoot.value(QStringLiteral("detector_mode")).toString().trimmed();
    const bool onlyDialogue = continuityRoot.value(QStringLiteral("only_dialogue")).toBool(false);
    const QString frameDomain = resolvedRawTrackFrameDomain(continuityRoot);

    const QJsonArray inlineTracks = continuityRoot.value(QStringLiteral("raw_tracks")).toArray();
    if (!inlineTracks.isEmpty()) {
        QVector<FacestreamTrack> tracks;
        tracks.reserve(inlineTracks.size());
        for (const QJsonValue& value : inlineTracks) {
            const QJsonObject trackObj = value.toObject();
            if (!trackSummaryMatchesFrame(rawTrackSummaryObject(trackObj, frameDomain),
                                          frame,
                                          extraWindowFrames)) {
                continue;
            }
            FacestreamTrack track = trackModelFromRawRecord(
                trackObj,
                frameDomain,
                detectorMode,
                onlyDialogue,
                transcriptRoot);
            if (!track.keyframes.isEmpty()) {
                tracks.push_back(track);
            }
        }
        return tracks;
    }

    const QString rawTracksPath =
        continuityRoot.value(QStringLiteral("raw_tracks_artifact_path")).toString().trimmed();
    if (!rawTracksPath.isEmpty()) {
        TrackArtifactIndex index;
        if (trackArtifactIndexForPath(rawTracksPath, &index)) {
            QVector<FacestreamTrack> tracks;
            for (const TrackArtifactIndexEntry& entry : std::as_const(index.entries)) {
                if (!trackSummaryMatchesFrame(trackSummaryObject(entry, frameDomain),
                                              frame,
                                              extraWindowFrames)) {
                    continue;
                }
                FacestreamTrack track = cachedTrackModelForEntry(
                    rawTracksPath,
                    entry,
                    frameDomain,
                    detectorMode,
                    onlyDialogue,
                    transcriptRoot);
                if (!track.keyframes.isEmpty()) {
                    tracks.push_back(track);
                }
            }
            return tracks;
        }
    }

    const QString processedArtifactPath =
        continuityRoot.value(QStringLiteral("processed_artifact_path")).toString().trimmed();
    const QString clipId = continuityRoot.value(QStringLiteral("clip_id")).toString().trimmed();
    if (!processedArtifactPath.isEmpty() && !clipId.isEmpty()) {
        QJsonObject processedArtifact;
        if (readBinaryJsonObject(processedArtifactPath, &processedArtifact, nullptr)) {
            return continuityTrackModelsNearFrameForRoot(
                continuityRootForClip(processedArtifact, clipId),
                frame,
                extraWindowFrames,
                transcriptRoot);
        }
    }

    return {};
}

QJsonArray continuityStreamsNearFrame(const QJsonObject& continuityRoot,
                                      int64_t frame,
                                      int64_t extraWindowFrames,
                                      const QJsonObject& transcriptRoot)
{
    if (continuityRoot.contains(QStringLiteral("streams"))) {
        Q_UNUSED(transcriptRoot);
        QJsonArray filtered;
        const QJsonArray storedStreams = continuityRoot.value(QStringLiteral("streams")).toArray();
        for (const QJsonValue& value : storedStreams) {
            const QJsonObject streamObj = value.toObject();
            if (trackSummaryMatchesFrame(streamSummaryObject(streamObj), frame, extraWindowFrames)) {
                filtered.push_back(streamObj);
            }
        }
        return filtered;
    }

    const QJsonArray storedStreams = continuityRoot.value(QStringLiteral("streams")).toArray();
    if (!storedStreams.isEmpty()) {
        QJsonArray filtered;
        for (const QJsonValue& value : storedStreams) {
            const QJsonObject streamObj = value.toObject();
            if (trackSummaryMatchesFrame(streamSummaryObject(streamObj), frame, extraWindowFrames)) {
                filtered.push_back(streamObj);
            }
        }
        return filtered;
    }

    const QJsonArray inlineTracks = continuityRoot.value(QStringLiteral("raw_tracks")).toArray();
    if (!inlineTracks.isEmpty()) {
        QJsonArray matchedTracks;
        const QString frameDomain = resolvedRawTrackFrameDomain(continuityRoot);
        for (const QJsonValue& value : inlineTracks) {
            const QJsonObject trackObj = value.toObject();
            if (trackSummaryMatchesFrame(rawTrackSummaryObject(trackObj, frameDomain),
                                         frame,
                                         extraWindowFrames)) {
                matchedTracks.push_back(trackObj);
            }
        }
        return buildContinuityStreams(
            matchedTracks,
            transcriptRoot,
            continuityRoot.value(QStringLiteral("detector_mode")).toString().trimmed(),
            continuityRoot.value(QStringLiteral("only_dialogue")).toBool(false),
            frameDomain);
    }

    const QString rawTracksPath =
        continuityRoot.value(QStringLiteral("raw_tracks_artifact_path")).toString().trimmed();
    if (!rawTracksPath.isEmpty()) {
        TrackArtifactIndex index;
        if (trackArtifactIndexForPath(rawTracksPath, &index)) {
            QJsonArray matchedTracks;
            const QString frameDomain = resolvedRawTrackFrameDomain(continuityRoot);
            for (const TrackArtifactIndexEntry& entry : std::as_const(index.entries)) {
                if (!trackSummaryMatchesFrame(trackSummaryObject(entry, frameDomain),
                                              frame,
                                              extraWindowFrames)) {
                    continue;
                }
                QJsonObject record = cachedTrackRecordForEntry(rawTracksPath, entry);
                if (record.isEmpty()) {
                    continue;
                }
                record.remove(QStringLiteral("type"));
                matchedTracks.push_back(record);
            }
            return buildContinuityStreams(
                matchedTracks,
                transcriptRoot,
                continuityRoot.value(QStringLiteral("detector_mode")).toString().trimmed(),
                continuityRoot.value(QStringLiteral("only_dialogue")).toBool(false),
                frameDomain);
        }
    }

    const QString processedArtifactPath =
        continuityRoot.value(QStringLiteral("processed_artifact_path")).toString().trimmed();
    const QString clipId = continuityRoot.value(QStringLiteral("clip_id")).toString().trimmed();
    if (!processedArtifactPath.isEmpty() && !clipId.isEmpty()) {
        QJsonObject processedArtifact;
        if (readBinaryJsonObject(processedArtifactPath, &processedArtifact, nullptr)) {
            return continuityStreamsNearFrame(
                continuityRootForClip(processedArtifact, clipId),
                frame,
                extraWindowFrames,
                transcriptRoot);
        }
    }

    return {};
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

QVector<FacestreamTrack> continuityTrackModelsForAssignments(
    const QJsonObject& continuityRoot,
    const QSet<int>& trackIds,
    const QSet<QString>& streamIds,
    const QJsonObject& transcriptRoot)
{
    if (trackIds.isEmpty() && streamIds.isEmpty()) {
        return {};
    }
    const QString detectorMode =
        continuityRoot.value(QStringLiteral("detector_mode")).toString().trimmed();
    const bool onlyDialogue = continuityRoot.value(QStringLiteral("only_dialogue")).toBool(false);
    const QString frameDomain = resolvedRawTrackFrameDomain(continuityRoot);

    QVector<FacestreamTrack> tracks;
    const QJsonArray inlineTracks = continuityRoot.value(QStringLiteral("raw_tracks")).toArray();
    if (!inlineTracks.isEmpty()) {
        tracks.reserve(inlineTracks.size());
        for (const QJsonValue& value : inlineTracks) {
            const QJsonObject trackObj = value.toObject();
            const int trackId = trackObj.value(QStringLiteral("track_id")).toInt(-1);
            const QString streamId = trackObj.value(QStringLiteral("stream_id")).toString().trimmed();
            if ((trackId >= 0 && trackIds.contains(trackId)) ||
                (!streamId.isEmpty() && streamIds.contains(streamId)) ||
                (trackId >= 0 && streamIds.contains(QStringLiteral("T%1").arg(trackId)))) {
                FacestreamTrack track =
                    trackModelFromRawRecord(trackObj, frameDomain, detectorMode, onlyDialogue, transcriptRoot);
                if (!track.keyframes.isEmpty()) {
                    tracks.push_back(track);
                }
            }
        }
        return tracks;
    }

    const QString rawTracksPath =
        continuityRoot.value(QStringLiteral("raw_tracks_artifact_path")).toString().trimmed();
    if (!rawTracksPath.isEmpty()) {
        TrackArtifactIndex index;
        if (trackArtifactIndexForPath(rawTracksPath, &index)) {
            for (const TrackArtifactIndexEntry& entry : std::as_const(index.entries)) {
                if (!((entry.trackId >= 0 && trackIds.contains(entry.trackId)) ||
                      (!entry.streamId.isEmpty() && streamIds.contains(entry.streamId)) ||
                      (entry.trackId >= 0 && streamIds.contains(QStringLiteral("T%1").arg(entry.trackId))))) {
                    continue;
                }
                FacestreamTrack track = cachedTrackModelForEntry(
                    rawTracksPath,
                    entry,
                    frameDomain,
                    detectorMode,
                    onlyDialogue,
                    transcriptRoot);
                if (!track.keyframes.isEmpty()) {
                    tracks.push_back(track);
                }
            }
            return tracks;
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
            return continuityTrackModelsForAssignments(
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

QJsonObject artifactCompatibilityMetadataForClip(const TimelineClip& clip)
{
    QJsonObject metadata;
    metadata[QStringLiteral("schema")] = QStringLiteral("jcut_facedetections_media_compat_v1");
    metadata[QStringLiteral("media_path")] = QFileInfo(clip.filePath).absoluteFilePath();
    const QFileInfo mediaInfo(clip.filePath);
    metadata[QStringLiteral("media_file_exists")] = mediaInfo.exists() && mediaInfo.isFile();
    if (mediaInfo.exists() && mediaInfo.isFile()) {
        metadata[QStringLiteral("media_file_size_bytes")] = mediaInfo.size();
        metadata[QStringLiteral("media_file_mtime_ms")] =
            mediaInfo.lastModified().toMSecsSinceEpoch();
    }
    metadata[QStringLiteral("source_fps")] = clip.sourceFps;
    metadata[QStringLiteral("source_duration_frames")] =
        static_cast<qint64>(clip.sourceDurationFrames);
    metadata[QStringLiteral("source_in_frame")] =
        static_cast<qint64>(clip.sourceInFrame);
    metadata[QStringLiteral("source_frame_width")] = clip.sourceFrameSize.width();
    metadata[QStringLiteral("source_frame_height")] = clip.sourceFrameSize.height();
    return metadata;
}

ArtifactCompatibilityResult validateArtifactCompatibilityForClip(
    const QJsonObject& continuityRoot,
    const TimelineClip& clip)
{
    ArtifactCompatibilityResult result;
    const QJsonObject expected =
        continuityRoot.value(QStringLiteral("media_compatibility")).toObject();
    const QJsonObject current = artifactCompatibilityMetadataForClip(clip);
    result.details[QStringLiteral("expected")] = expected;
    result.details[QStringLiteral("current")] = current;

    auto addWarning = [&](const QString& warning) {
        if (!result.warnings.contains(warning)) {
            result.warnings.push_back(warning);
        }
    };
    auto addError = [&](const QString& error) {
        if (!result.errors.contains(error)) {
            result.errors.push_back(error);
        }
        result.compatible = false;
    };

    if (expected.isEmpty()) {
        result.compatibilityClass = QStringLiteral("missing_metadata");
        addWarning(QStringLiteral("FaceDetections artifact has no media compatibility metadata; regenerate to make stale-artifact failures explicit."));
        result.details[QStringLiteral("compatible")] = result.compatible;
        result.details[QStringLiteral("compatibility_class")] = result.compatibilityClass;
        result.details[QStringLiteral("warnings")] = stringListToJsonArray(result.warnings);
        result.details[QStringLiteral("errors")] = stringListToJsonArray(result.errors);
        return result;
    }

    const QString expectedPath =
        QFileInfo(expected.value(QStringLiteral("media_path")).toString().trimmed()).absoluteFilePath();
    const QString currentPath =
        current.value(QStringLiteral("media_path")).toString().trimmed();
    if (!expectedPath.isEmpty() && !currentPath.isEmpty() && expectedPath != currentPath) {
        addWarning(QStringLiteral("Media path differs from the path used when FaceDetections was generated."));
    }

    const qint64 expectedSize =
        expected.value(QStringLiteral("media_file_size_bytes")).toVariant().toLongLong();
    const qint64 currentSize =
        current.value(QStringLiteral("media_file_size_bytes")).toVariant().toLongLong();
    if (expectedSize > 0 && currentSize > 0 && expectedSize != currentSize) {
        addError(QStringLiteral("Media file size differs from the file used when FaceDetections was generated."));
    }

    const int expectedWidth = expected.value(QStringLiteral("source_frame_width")).toInt(0);
    const int currentWidth = current.value(QStringLiteral("source_frame_width")).toInt(0);
    if (expectedWidth > 0 && currentWidth > 0 && expectedWidth != currentWidth) {
        addError(QStringLiteral("Source frame width differs from the file used when FaceDetections was generated."));
    }
    const int expectedHeight = expected.value(QStringLiteral("source_frame_height")).toInt(0);
    const int currentHeight = current.value(QStringLiteral("source_frame_height")).toInt(0);
    if (expectedHeight > 0 && currentHeight > 0 && expectedHeight != currentHeight) {
        addError(QStringLiteral("Source frame height differs from the file used when FaceDetections was generated."));
    }

    const qreal expectedFps = expected.value(QStringLiteral("source_fps")).toDouble(0.0);
    const qreal currentFps = current.value(QStringLiteral("source_fps")).toDouble(0.0);
    if (expectedFps > 0.0 && currentFps > 0.0 && !nearlyEqual(expectedFps, currentFps, 0.001)) {
        addWarning(QStringLiteral("Source FPS metadata changed after FaceDetections generation; source-absolute tracks remain usable, but timeline-to-source mapping may now land on different displayed frames."));
    }

    const qint64 expectedDuration =
        expected.value(QStringLiteral("source_duration_frames")).toVariant().toLongLong();
    const qint64 currentDuration =
        current.value(QStringLiteral("source_duration_frames")).toVariant().toLongLong();
    if (expectedDuration > 0 && currentDuration > 0 && expectedDuration != currentDuration) {
        addWarning(QStringLiteral("Source duration metadata changed after FaceDetections generation."));
    }

    if (!result.compatible) {
        result.compatibilityClass = QStringLiteral("fatal_mismatch");
    } else if (!result.warnings.isEmpty()) {
        result.compatibilityClass = QStringLiteral("warning_mismatch");
    } else {
        result.compatibilityClass = QStringLiteral("ok");
    }
    result.details[QStringLiteral("compatible")] = result.compatible;
    result.details[QStringLiteral("compatibility_class")] = result.compatibilityClass;
    result.details[QStringLiteral("warnings")] = stringListToJsonArray(result.warnings);
    result.details[QStringLiteral("errors")] = stringListToJsonArray(result.errors);
    return result;
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
        QStringLiteral("media_compatibility"),
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

namespace {

bool replaceWithTempFile(const QString& finalPath,
                         const QString& tempPath,
                         QString* errorOut)
{
    QFile::remove(finalPath);
    if (!QFile::rename(tempPath, finalPath)) {
        QFile::remove(tempPath);
        if (errorOut) {
            *errorOut = QStringLiteral("Failed to replace %1").arg(finalPath);
        }
        return false;
    }
    return true;
}

bool writeCompressedIndexedObject(QFile* dataFile,
                                  const QJsonObject& object,
                                  qint64* offsetOut,
                                  quint32* compressedSizeOut,
                                  QString* errorOut)
{
    if (!dataFile || !dataFile->isOpen()) {
        if (errorOut) {
            *errorOut = QStringLiteral("Indexed artifact data file is not open.");
        }
        return false;
    }
    const QByteArray compressed = qCompress(jcut::jsonio::serializeCbor(object), 6);
    if (compressed.size() > std::numeric_limits<quint32>::max()) {
        if (errorOut) {
            *errorOut = QStringLiteral("Indexed artifact record is too large.");
        }
        return false;
    }
    const qint64 offset = dataFile->pos();
    if (dataFile->write(compressed) != compressed.size()) {
        if (errorOut) {
            *errorOut = QStringLiteral("Failed to write indexed artifact data record.");
        }
        return false;
    }
    if (offsetOut) {
        *offsetOut = offset;
    }
    if (compressedSizeOut) {
        *compressedSizeOut = static_cast<quint32>(compressed.size());
    }
    return true;
}

QString indexStoredDataPath(const QString& indexPath, const QString& dataPath)
{
    const QFileInfo indexInfo(indexPath);
    const QFileInfo dataInfo(dataPath);
    return indexInfo.dir().absolutePath() == dataInfo.dir().absolutePath()
        ? dataInfo.fileName()
        : dataInfo.absoluteFilePath();
}

} // namespace

bool writeIndexedTrackArtifact(const QString& indexPath,
                               const QString& dataPath,
                               const QString& videoPath,
                               const QString& backend,
                               const QJsonArray& tracks,
                               const QJsonArray& frameSummaries,
                               QString* errorOut)
{
    const QString tempDataPath = dataPath + QStringLiteral(".tmp");
    const QString tempIndexPath = indexPath + QStringLiteral(".tmp");
    QFile::remove(tempDataPath);
    QFile::remove(tempIndexPath);

    QFile dataFile(tempDataPath);
    if (!dataFile.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        if (errorOut) {
            *errorOut = QStringLiteral("Failed to open %1 for writing.").arg(tempDataPath);
        }
        return false;
    }

    QVector<TrackArtifactIndexEntry> trackEntries;
    QVector<IndexedObjectEntry> summaryEntries;
    for (const QJsonValue& value : tracks) {
        QJsonObject track = value.toObject();
        track[QStringLiteral("type")] = QStringLiteral("track");
        TrackArtifactIndexEntry entry;
        entry.trackId = track.value(QStringLiteral("track_id")).toInt(-1);
        entry.streamId = track.value(QStringLiteral("stream_id")).toString().trimmed();
        entry.source = track.value(QStringLiteral("source")).toString().trimmed();
        entry.minFrame = minFrameForTrackRecord(track);
        entry.maxFrame = maxFrameForTrackRecord(track);
        entry.detectionCount = track.value(QStringLiteral("detections")).toArray().size();
        entry.typicalFrameStep = typicalFrameStepForTrackRecord(track);
        if (!writeCompressedIndexedObject(&dataFile, track, &entry.dataOffset, &entry.compressedSize, errorOut)) {
            dataFile.close();
            QFile::remove(tempDataPath);
            return false;
        }
        trackEntries.push_back(entry);
    }
    for (const QJsonValue& value : frameSummaries) {
        QJsonObject summary = value.toObject();
        summary[QStringLiteral("type")] = QStringLiteral("frame_summary");
        IndexedObjectEntry entry;
        entry.type = QStringLiteral("frame_summary");
        entry.frame = summary.value(QStringLiteral("frame")).toVariant().toLongLong();
        if (!writeCompressedIndexedObject(&dataFile, summary, &entry.dataOffset, &entry.compressedSize, errorOut)) {
            dataFile.close();
            QFile::remove(tempDataPath);
            return false;
        }
        summaryEntries.push_back(entry);
    }
    if (!dataFile.flush()) {
        dataFile.close();
        QFile::remove(tempDataPath);
        if (errorOut) {
            *errorOut = QStringLiteral("Failed to flush %1.").arg(tempDataPath);
        }
        return false;
    }
    dataFile.close();

    QFile indexFile(tempIndexPath);
    if (!indexFile.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        QFile::remove(tempDataPath);
        if (errorOut) {
            *errorOut = QStringLiteral("Failed to open %1 for writing.").arg(tempIndexPath);
        }
        return false;
    }
    QDataStream stream(&indexFile);
    stream.setVersion(QDataStream::Qt_6_0);
    stream << kIndexedTrackArtifactMagic
           << kIndexedArtifactVersion
           << QStringLiteral("jcut_facedetections_offscreen_tracks_v1")
           << videoPath
           << backend
           << QStringLiteral("source_absolute")
           << indexStoredDataPath(indexPath, dataPath)
           << static_cast<qint64>(trackEntries.size() + summaryEntries.size());
    for (const TrackArtifactIndexEntry& entry : std::as_const(trackEntries)) {
        stream << QStringLiteral("track")
               << entry.trackId
               << entry.streamId
               << entry.source
               << entry.minFrame
               << entry.maxFrame
               << entry.detectionCount
               << entry.typicalFrameStep
               << entry.dataOffset
               << entry.compressedSize;
    }
    for (const IndexedObjectEntry& entry : std::as_const(summaryEntries)) {
        stream << entry.type << entry.frame << entry.dataOffset << entry.compressedSize;
    }
    if (stream.status() != QDataStream::Ok || !indexFile.flush()) {
        indexFile.close();
        QFile::remove(tempDataPath);
        QFile::remove(tempIndexPath);
        if (errorOut) {
            *errorOut = QStringLiteral("Failed to write indexed track artifact %1.").arg(tempIndexPath);
        }
        return false;
    }
    indexFile.close();

    if (!replaceWithTempFile(dataPath, tempDataPath, errorOut)) {
        QFile::remove(tempIndexPath);
        return false;
    }
    return replaceWithTempFile(indexPath, tempIndexPath, errorOut);
}

bool writeIndexedFrameArtifact(const QString& indexPath,
                               const QString& dataPath,
                               const QString& videoPath,
                               const QString& backend,
                               const QJsonArray& frames,
                               QString* errorOut)
{
    const QString tempDataPath = dataPath + QStringLiteral(".tmp");
    const QString tempIndexPath = indexPath + QStringLiteral(".tmp");
    QFile::remove(tempDataPath);
    QFile::remove(tempIndexPath);

    QFile dataFile(tempDataPath);
    if (!dataFile.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        if (errorOut) {
            *errorOut = QStringLiteral("Failed to open %1 for writing.").arg(tempDataPath);
        }
        return false;
    }

    QVector<IndexedObjectEntry> entries;
    for (const QJsonValue& value : frames) {
        QJsonObject frame = value.toObject();
        frame[QStringLiteral("type")] = QStringLiteral("frame");
        IndexedObjectEntry entry;
        entry.type = QStringLiteral("frame");
        entry.frame = frame.value(QStringLiteral("frame")).toVariant().toLongLong();
        if (!writeCompressedIndexedObject(&dataFile, frame, &entry.dataOffset, &entry.compressedSize, errorOut)) {
            dataFile.close();
            QFile::remove(tempDataPath);
            return false;
        }
        entries.push_back(entry);
    }
    std::sort(entries.begin(), entries.end(), [](const IndexedObjectEntry& a,
                                                 const IndexedObjectEntry& b) {
        if (a.frame == b.frame) {
            return a.dataOffset < b.dataOffset;
        }
        return a.frame < b.frame;
    });
    if (!dataFile.flush()) {
        dataFile.close();
        QFile::remove(tempDataPath);
        if (errorOut) {
            *errorOut = QStringLiteral("Failed to flush %1.").arg(tempDataPath);
        }
        return false;
    }
    dataFile.close();

    QFile indexFile(tempIndexPath);
    if (!indexFile.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        QFile::remove(tempDataPath);
        if (errorOut) {
            *errorOut = QStringLiteral("Failed to open %1 for writing.").arg(tempIndexPath);
        }
        return false;
    }
    QDataStream stream(&indexFile);
    stream.setVersion(QDataStream::Qt_6_0);
    stream << kIndexedFrameArtifactMagic
           << kIndexedArtifactVersion
           << QStringLiteral("jcut_facedetections_offscreen_detections_v1")
           << videoPath
           << backend
           << QStringLiteral("source_absolute")
           << indexStoredDataPath(indexPath, dataPath)
           << static_cast<qint64>(entries.size());
    for (const IndexedObjectEntry& entry : std::as_const(entries)) {
        stream << entry.type << entry.frame << entry.dataOffset << entry.compressedSize;
    }
    if (stream.status() != QDataStream::Ok || !indexFile.flush()) {
        indexFile.close();
        QFile::remove(tempDataPath);
        QFile::remove(tempIndexPath);
        if (errorOut) {
            *errorOut = QStringLiteral("Failed to write indexed frame artifact %1.").arg(tempIndexPath);
        }
        return false;
    }
    indexFile.close();

    if (!replaceWithTempFile(dataPath, tempDataPath, errorOut)) {
        QFile::remove(tempIndexPath);
        return false;
    }
    return replaceWithTempFile(indexPath, tempIndexPath, errorOut);
}

bool readBinaryJsonObject(const QString& path,
                          QJsonObject* objectOut,
                          QString* errorOut)
{
    if (indexedArtifactToRoot(path, objectOut)) {
        return true;
    }
    if (errorOut) {
        *errorOut = QStringLiteral("Unsupported FaceDetections indexed artifact: %1").arg(path);
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
