#include "speakers_tab.h"
#include "speakers_tab_internal.h"

#include "decoder_context.h"
#include "facedetections_artifact_utils.h"
#include "facedetections_runtime.h"
#include "facedetections_time_mapping.h"
#include "track_avatar_utils.h"
#include "transcript_engine.h"

#include <QBuffer>
#include <QDateTime>
#include <QCoreApplication>
#include <QDebug>
#include <QDialog>
#include <QDir>
#include <QElapsedTimer>
#include <QFileInfo>
#include <QHBoxLayout>
#include <QImage>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLabel>
#include <QListView>
#include <QListWidget>
#include <QMenu>
#include <QMessageBox>
#include <QPainter>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QSet>
#include <QSignalBlocker>
#include <QTableWidget>
#include <QThread>
#include <QTimer>
#include <QToolTip>
#include <QVBoxLayout>

#include <algorithm>
#include <cmath>
#include <limits>
#include <memory>

namespace {
constexpr int kPersistentTrackAvatarSize = 160;

struct CachedFacestreamKeyframe {
    int64_t sourceFrame = -1;
    qreal x = 0.5;
    qreal y = 0.5;
    qreal box = 0.2;
};

struct CachedFacestreamTrack {
    int trackId = -1;
    QString streamId;
    QVector<CachedFacestreamKeyframe> keyframes;
};

QString trackIdentityResolutionSignature(const QString& transcriptPath,
                                         const TimelineClip& clip,
                                         const QJsonArray& resolvedMap,
                                         const QJsonArray& streams);
QVector<CachedFacestreamTrack> buildCachedFacestreamTracks(
    const TimelineClip& clip,
    const QJsonArray& streams,
    const QVector<RenderSyncMarker>& renderSyncMarkers);
int resolveTrackIdFromCachedTracks(const QVector<CachedFacestreamTrack>& cachedTracks,
                                   const QJsonObject& row);

bool shouldAvoidTransientUiThreadDecoder()
{
    QCoreApplication* const app = QCoreApplication::instance();
    return app && QThread::currentThread() == app->thread();
}

QString persistentTrackAvatarFilePath(const QString& transcriptPath,
                                      const QString& clipId,
                                      int trackId)
{
    const QFileInfo info(transcriptPath);
    const QString rootDir = info.dir().filePath(info.completeBaseName() + QStringLiteral("_track_memory"));
    return QDir(rootDir)
        .filePath(QStringLiteral("%1/track_%2_avatar.png").arg(clipId.trimmed()).arg(trackId));
}

bool resolvePlayheadTrackSelection(const TimelineClip& clip,
                                   const QJsonObject& streamObj,
                                   const QVector<RenderSyncMarker>& renderSyncMarkers,
                                   int64_t playheadTimelineFrame,
                                   int64_t playheadSourceFrame,
                                   FacestreamResolvedSelection* selectionOut)
{
    if (!selectionOut) {
        return false;
    }

    const int trackId = streamObj.value(QStringLiteral("track_id")).toInt(-1);
    const QString streamId =
        streamObj.value(QStringLiteral("stream_id")).toString(QStringLiteral("T%1").arg(trackId));
    const QString trackSource =
        streamObj.value(QStringLiteral("source")).toString().trimmed().toLower();
    if (trackId < 0) {
        return false;
    }

    const QJsonArray keyframes = streamObj.value(QStringLiteral("keyframes")).toArray();
    if (keyframes.isEmpty()) {
        return false;
    }

    FacestreamResolvedTrack track;
    track.trackId = trackId;
    track.streamId = streamId;
    track.source = trackSource;
    track.keyframes.reserve(keyframes.size());
    int64_t minStreamFrame = std::numeric_limits<int64_t>::max();
    int64_t maxStreamFrame = std::numeric_limits<int64_t>::min();
    for (const QJsonValue& keyframeValue : keyframes) {
        const QJsonObject keyframeObj = keyframeValue.toObject();
        if (!keyframeObj.contains(QStringLiteral("frame"))) {
            continue;
        }
        FacestreamResolvedKeyframe keyframe;
        keyframe.frame =
            keyframeObj.value(QStringLiteral("frame")).toVariant().toLongLong();
        keyframe.xNorm = qBound<qreal>(
            0.0, keyframeObj.value(QString(kTranscriptSpeakerLocationXKey)).toDouble(0.5), 1.0);
        keyframe.yNorm = qBound<qreal>(
            0.0, keyframeObj.value(QString(kTranscriptSpeakerLocationYKey)).toDouble(0.5), 1.0);
        keyframe.boxSizeNorm = qBound<qreal>(
            0.01,
            keyframeObj.value(QString(kTranscriptSpeakerTrackingBoxSizeKey)).toDouble(
                keyframeObj.value(QStringLiteral("box")).toDouble(0.2)),
            1.0);
        keyframe.confidence = qBound<qreal>(
            0.0, keyframeObj.value(QStringLiteral("confidence")).toDouble(1.0), 1.0);
        keyframe.source =
            keyframeObj.value(QStringLiteral("source")).toString(trackSource).trimmed().toLower();
        keyframe.hasCenterBox = true;
        track.keyframes.push_back(keyframe);
        minStreamFrame = qMin(minStreamFrame, keyframe.frame);
        maxStreamFrame = qMax(maxStreamFrame, keyframe.frame);
    }
    if (track.keyframes.isEmpty()) {
        return false;
    }

    std::sort(track.keyframes.begin(),
              track.keyframes.end(),
              [](const FacestreamResolvedKeyframe& a, const FacestreamResolvedKeyframe& b) {
                  return a.frame < b.frame;
              });

    QVector<int64_t> sortedFrames;
    sortedFrames.reserve(track.keyframes.size());
    for (const FacestreamResolvedKeyframe& keyframe : track.keyframes) {
        sortedFrames.push_back(keyframe.frame);
    }
    track.typicalFrameStep = qMax<int64_t>(1, facedetectionsTypicalFrameStep(sortedFrames));

    if (!parseFacestreamFrameDomainString(
            streamObj.value(QStringLiteral("frame_domain")).toString().trimmed(),
            &track.frameDomain)) {
        track.frameDomain = inferFacestreamFrameDomain(clip, minStreamFrame, maxStreamFrame);
    }
    return resolveFacestreamTrackAtPlayhead(
        clip,
        track,
        renderSyncMarkers,
        playheadTimelineFrame,
        playheadSourceFrame,
        selectionOut);
}
}

QPixmap SpeakersTab::faceStreamPreviewAvatar(const TimelineClip& clip,
                                             const QString& speakerId,
                                             const QJsonObject& keyframeObj,
                                             int size) const
{
    return faceStreamPreviewAvatarWithDecoder(
        clip, speakerId, keyframeObj, size, nullptr, nullptr);
}

QPixmap SpeakersTab::faceStreamPreviewAvatarWithDecoder(const TimelineClip& clip,
                                                        const QString& speakerId,
                                                        const QJsonObject& keyframeObj,
                                                        int size,
                                                        editor::DecoderContext* decoderCtx,
                                                        QHash<int64_t, QImage>* frameImageCache) const
{
    if (keyframeObj.isEmpty()) {
        return placeholderSpeakerAvatar(speakerId);
    }

    const int avatarSize = qMax(24, size);
    const int64_t sourceFrame = qMax<int64_t>(
        0, keyframeObj.value(QStringLiteral("frame")).toVariant().toLongLong());
    const QString cacheKey = QStringLiteral("facedetections|%1|%2|%3|%4|%5|%6|%7|%8|%9")
        .arg(m_transcriptSession.transcriptPath())
        .arg(clip.id)
        .arg(speakerId)
        .arg(sourceFrame)
        .arg(static_cast<int>(std::round(
            qBound<qreal>(0.0, keyframeObj.value(QStringLiteral("x")).toDouble(0.5), 1.0) * 1000.0)))
        .arg(static_cast<int>(std::round(
            qBound<qreal>(0.0, keyframeObj.value(QStringLiteral("y")).toDouble(0.5), 1.0) * 1000.0)))
        .arg(static_cast<int>(std::round(qMax<qreal>(
            0.0,
            qBound<qreal>(
                -1.0,
                keyframeObj.value(QStringLiteral("box_size")).toDouble(
                    keyframeObj.value(QStringLiteral("box")).toDouble(-1.0)),
                1.0)) * 1000.0)))
        .arg(static_cast<int>(std::round(qMax<qreal>(
            0.0,
            keyframeObj.value(QStringLiteral("box_left")).toDouble(-1.0)) * 1000.0)))
        .arg(avatarSize);
    const auto cached = m_avatarCache.constFind(cacheKey);
    if (cached != m_avatarCache.cend()) {
        return cached.value();
    }

    const int64_t decodeFrame = sourceFrame;
    QPixmap avatar = placeholderSpeakerAvatar(speakerId);
    QImage image;
    if (frameImageCache) {
        const auto cachedFrame = frameImageCache->constFind(decodeFrame);
        if (cachedFrame != frameImageCache->cend()) {
            image = cachedFrame.value();
        }
    }
    if (image.isNull()) {
        std::unique_ptr<editor::DecoderContext> localDecoder;
        editor::DecoderContext* activeDecoder = decoderCtx;
        if (!activeDecoder) {
            if (shouldAvoidTransientUiThreadDecoder()) {
                return avatar;
            }
            const QString mediaPath = interactivePreviewMediaPathForClip(clip);
            localDecoder = std::make_unique<editor::DecoderContext>(mediaPath);
            if (!localDecoder->initialize()) {
                activeDecoder = nullptr;
            } else {
                activeDecoder = localDecoder.get();
            }
        }
        if (activeDecoder) {
            const editor::FrameHandle frame = activeDecoder->decodeFrame(decodeFrame);
            image = frame.hasCpuImage() ? frame.cpuImage() : QImage();
            if (frameImageCache && !image.isNull()) {
                frameImageCache->insert(decodeFrame, image);
            }
        }
    }
    if (!image.isNull() && image.width() > 0 && image.height() > 0) {
        const QImage rounded = renderTrackAvatarImage(image, keyframeObj, avatarSize);
        if (!rounded.isNull()) {
            avatar = QPixmap::fromImage(rounded);
        }
    }

    m_avatarCache.insert(cacheKey, avatar);
    return avatar;
}

QJsonObject SpeakersTab::representativeKeyframeForTrack(const TimelineClip& clip,
                                                        const QJsonObject& streamObj) const
{
    const QJsonArray keyframes = streamObj.value(QStringLiteral("keyframes")).toArray();
    if (keyframes.isEmpty()) {
        return {};
    }

    QJsonObject representative = keyframes.at(keyframes.size() / 2).toObject();
    if (representative.isEmpty()) {
        representative = keyframes.first().toObject();
    }
    if (representative.isEmpty()) {
        return {};
    }

    FacestreamFrameDomain frameDomain = FacestreamFrameDomain::SourceRelative;
    if (!parseFacestreamFrameDomainString(
            streamObj.value(QStringLiteral("frame_domain")).toString().trimmed(),
            &frameDomain)) {
        frameDomain = FacestreamFrameDomain::SourceRelative;
    }
    const QVector<RenderSyncMarker> renderSyncMarkers =
        m_speakerDeps.getRenderSyncMarkers ? m_speakerDeps.getRenderSyncMarkers() : QVector<RenderSyncMarker>{};
    const int64_t storedFrame =
        representative.value(QStringLiteral("frame")).toVariant().toLongLong();
    representative[QStringLiteral("frame")] = static_cast<qint64>(
        qMax<int64_t>(0, mapFacestreamFrameToSourceFrame(clip, storedFrame, frameDomain, renderSyncMarkers)));
    return representative;
}

QPixmap SpeakersTab::continuityTrackAvatar(const TimelineClip& clip,
                                           const QString& speakerId,
                                           const QJsonObject& streamObj,
                                           int size,
                                           editor::DecoderContext* decoderCtx,
                                           QHash<int64_t, QImage>* frameImageCache) const
{
    const int trackId = streamObj.value(QStringLiteral("track_id")).toInt(-1);
    if (trackId >= 0) {
        const QJsonObject memoryEntry = trackMemoryEntryForClip(clip.id, trackId);
        const QString avatarPath = memoryEntry.value(QStringLiteral("avatar_path")).toString().trimmed();
        if (!avatarPath.isEmpty()) {
            QPixmap persisted;
            if (persisted.load(avatarPath) && !persisted.isNull()) {
                const int avatarSize = qMax(24, size);
                if (persisted.width() != avatarSize || persisted.height() != avatarSize) {
                    persisted = persisted.scaled(
                        avatarSize,
                        avatarSize,
                        Qt::KeepAspectRatio,
                        Qt::SmoothTransformation);
                }
                if (!persisted.isNull()) {
                    return persisted;
                }
            }
        }
    }
    return faceStreamPreviewAvatarWithDecoder(
        clip,
        speakerId,
        representativeKeyframeForTrack(clip, streamObj),
        size,
        decoderCtx,
        frameImageCache);
}

QVector<QPixmap> SpeakersTab::assignedFaceDetectionsPreviewPixmaps(const TimelineClip& clip,
                                                               const QString& speakerId) const
{
    QVector<QPixmap> pixmaps;
    if (!m_transcriptSession.hasObjectDocument() || speakerId.isEmpty()) {
        return pixmaps;
    }

    std::unique_ptr<editor::DecoderContext> decoder;
    const QString mediaPath = interactivePreviewMediaPathForClip(clip);
    if (!mediaPath.isEmpty()) {
        decoder = std::make_unique<editor::DecoderContext>(mediaPath);
        if (!decoder->initialize()) {
            decoder.reset();
        }
    }
    QHash<int64_t, QImage> frameImageCache;
    const QJsonArray streams = continuityStreamsForClip(clip);
    const QVector<int> assignedTrackIdList =
        resolvedAssignedTrackIdsForSpeaker(clip, streams, speakerId);
    QSet<int> assignedTrackIds;
    for (int trackId : assignedTrackIdList) {
        assignedTrackIds.insert(trackId);
    }
    if (assignedTrackIds.isEmpty()) {
        return pixmaps;
    }
    for (const QJsonValue& streamValue : streams) {
        const QJsonObject streamObj = streamValue.toObject();
        const int trackId = streamObj.value(QStringLiteral("track_id")).toInt(-1);
        if (!assignedTrackIds.contains(trackId)) {
            continue;
        }
        const QPixmap avatar = continuityTrackAvatar(
            clip,
            speakerId,
            streamObj,
            72,
            decoder.get(),
            &frameImageCache);
        if (!avatar.isNull()) {
            pixmaps.push_back(avatar);
        }
    }
    return pixmaps;
}

QJsonArray SpeakersTab::continuityStreamsForClip(const TimelineClip& clip) const
{
    const QString cacheKey = m_transcriptSession.transcriptPath() + QLatin1Char('\n') + clip.id.trimmed();
    const auto cached = m_continuityStreamsCache.constFind(cacheKey);
    if (cached != m_continuityStreamsCache.cend()) {
        return cached.value();
    }

    QJsonArray streams;
    editor::TranscriptEngine transcriptEngine;
    QJsonObject artifactRoot;
    if (transcriptEngine.loadFacestreamProcessedArtifact(m_transcriptSession.transcriptPath(), &artifactRoot) ||
        transcriptEngine.loadFacestreamArtifact(m_transcriptSession.transcriptPath(), &artifactRoot)) {
        const QJsonObject continuityRoot = continuityRootForClip(artifactRoot, clip.id);
        streams = jcut::facedetections::storedContinuityStreamsForRoot(continuityRoot);
        if (streams.isEmpty() && continuityRoot.value(QStringLiteral("raw_tracks_artifact_path")).toString().trimmed().isEmpty()) {
            streams = jcut::facedetections::continuityStreamsForRoot(
                continuityRoot,
                m_transcriptSession.rootObject());
        }
        if (!streams.isEmpty()) {
            m_continuityStreamsCache.insert(cacheKey, streams);
            return streams;
        }
    }
    m_continuityStreamsCache.insert(cacheKey, streams);
    return streams;
}

QJsonObject SpeakersTab::faceDetectionsDebugSnapshot() const
{
    return m_lastFaceDetectionsDebugSnapshot;
}

QJsonObject SpeakersTab::trackMemoryEntryForClip(const QString& clipId, int trackId) const
{
    if (clipId.trimmed().isEmpty() || trackId < 0 || !m_transcriptSession.hasObjectDocument()) {
        return {};
    }
    const QJsonObject speakerFlow =
        m_transcriptSession.rootObject().value(QStringLiteral("speaker_flow")).toObject();
    const QJsonObject clipsRoot = speakerFlow.value(QStringLiteral("clips")).toObject();
    const QJsonObject clipRoot = clipsRoot.value(clipId.trimmed()).toObject();
    const QJsonObject trackMemory = clipRoot.value(QStringLiteral("track_memory")).toObject();
    const QJsonObject tracksRoot = trackMemory.value(QStringLiteral("tracks")).toObject();
    return tracksRoot.value(QString::number(trackId)).toObject();
}

void SpeakersTab::ensurePersistentTrackAvatarMemory(const TimelineClip& clip,
                                                    const QJsonArray& streams,
                                                    bool forceRefresh,
                                                    editor::DecoderContext* decoderCtx,
                                                    QHash<int64_t, QImage>* frameImageCache)
{
    if (!m_transcriptSession.hasObjectDocument() || streams.isEmpty()) {
        return;
    }

    struct PendingEntry {
        int trackId = -1;
        QString streamId;
        QString avatarPath;
        QJsonObject representativeKeyframe;
    };
    QVector<PendingEntry> pending;
    pending.reserve(streams.size());
    for (const QJsonValue& streamValue : streams) {
        const QJsonObject streamObj = streamValue.toObject();
        const int trackId = streamObj.value(QStringLiteral("track_id")).toInt(-1);
        if (trackId < 0) {
            continue;
        }
        const QJsonObject existing = trackMemoryEntryForClip(clip.id, trackId);
        const QString existingPath = existing.value(QStringLiteral("avatar_path")).toString().trimmed();
        if (!forceRefresh && !existingPath.isEmpty() && QFileInfo::exists(existingPath)) {
            continue;
        }
        const QJsonObject representative = representativeKeyframeForTrack(clip, streamObj);
        if (representative.isEmpty()) {
            continue;
        }
        PendingEntry entry;
        entry.trackId = trackId;
        entry.streamId =
            streamObj.value(QStringLiteral("stream_id")).toString(QStringLiteral("T%1").arg(trackId));
        entry.avatarPath = persistentTrackAvatarFilePath(m_transcriptSession.transcriptPath(), clip.id, trackId);
        entry.representativeKeyframe = representative;
        pending.push_back(entry);
    }
    if (pending.isEmpty()) {
        return;
    }

    std::unique_ptr<editor::DecoderContext> localDecoder;
    editor::DecoderContext* activeDecoder = decoderCtx;
    if (!activeDecoder) {
        if (shouldAvoidTransientUiThreadDecoder()) {
            return;
        }
        const QString mediaPath = interactivePreviewMediaPathForClip(clip);
        if (!mediaPath.isEmpty()) {
            localDecoder = std::make_unique<editor::DecoderContext>(mediaPath);
            if (localDecoder->initialize()) {
                activeDecoder = localDecoder.get();
            }
        }
    }
    if (!activeDecoder) {
        return;
    }

    QHash<int64_t, QImage> localFrameCache;
    QHash<int64_t, QImage>* activeFrameCache = frameImageCache ? frameImageCache : &localFrameCache;

    QJsonObject transcriptRoot = m_transcriptSession.rootObject();
    QJsonObject speakerFlow = transcriptRoot.value(QStringLiteral("speaker_flow")).toObject();
    speakerFlow[QStringLiteral("schema_version")] = QStringLiteral("1.0");
    QJsonObject clipsRoot = speakerFlow.value(QStringLiteral("clips")).toObject();
    QJsonObject clipRoot = clipsRoot.value(clip.id).toObject();
    clipRoot[QStringLiteral("clip_id")] = clip.id;
    QJsonObject trackMemory = clipRoot.value(QStringLiteral("track_memory")).toObject();
    QJsonObject tracksRoot = trackMemory.value(QStringLiteral("tracks")).toObject();

    editor::TranscriptEngine engine;
    QJsonObject identityRoot;
    engine.loadIdentityArtifact(m_transcriptSession.transcriptPath(), &identityRoot);
    QJsonObject memoryByClip = identityRoot.value(QStringLiteral("track_memory_by_clip")).toObject();
    QJsonObject identityClipMemory = memoryByClip.value(clip.id).toObject();
    QJsonObject identityTracksRoot = identityClipMemory.value(QStringLiteral("tracks")).toObject();

    bool changed = false;
    const QString timestamp = QDateTime::currentDateTimeUtc().toString(Qt::ISODate);
    for (const PendingEntry& entry : std::as_const(pending)) {
        const int64_t sourceFrame = qMax<int64_t>(
            0,
            entry.representativeKeyframe.value(QStringLiteral("frame")).toVariant().toLongLong());
        const int64_t decodeFrame = sourceFrame;
        QImage image = activeFrameCache->value(decodeFrame);
        if (image.isNull()) {
            const editor::FrameHandle frame = activeDecoder->decodeFrame(decodeFrame);
            image = frame.hasCpuImage() ? frame.cpuImage() : QImage();
            if (!image.isNull()) {
                activeFrameCache->insert(decodeFrame, image);
            }
        }
        const QImage avatarImage =
            renderTrackAvatarImage(image, entry.representativeKeyframe, kPersistentTrackAvatarSize);
        if (avatarImage.isNull()) {
            continue;
        }
        const QFileInfo avatarInfo(entry.avatarPath);
        QDir().mkpath(avatarInfo.dir().absolutePath());
        if (!avatarImage.save(entry.avatarPath)) {
            continue;
        }

        QJsonObject memoryEntry;
        memoryEntry[QStringLiteral("track_id")] = entry.trackId;
        memoryEntry[QStringLiteral("stream_id")] = entry.streamId;
        memoryEntry[QStringLiteral("avatar_path")] = entry.avatarPath;
        memoryEntry[QStringLiteral("source_frame")] =
            static_cast<qint64>(qMax<int64_t>(
                0,
                entry.representativeKeyframe.value(QStringLiteral("frame")).toVariant().toLongLong()));
        memoryEntry[QStringLiteral("x")] =
            qBound<qreal>(0.0, entry.representativeKeyframe.value(QStringLiteral("x")).toDouble(0.5), 1.0);
        memoryEntry[QStringLiteral("y")] =
            qBound<qreal>(0.0, entry.representativeKeyframe.value(QStringLiteral("y")).toDouble(0.5), 1.0);
        memoryEntry[QStringLiteral("box")] = qBound<qreal>(
            0.01,
            entry.representativeKeyframe.value(QStringLiteral("box_size")).toDouble(0.2),
            1.0);
        memoryEntry[QStringLiteral("updated_at_utc")] = timestamp;
        tracksRoot[QString::number(entry.trackId)] = memoryEntry;
        identityTracksRoot[QString::number(entry.trackId)] = memoryEntry;
        changed = true;
    }

    if (!changed) {
        return;
    }

    trackMemory[QStringLiteral("tracks")] = tracksRoot;
    trackMemory[QStringLiteral("updated_at_utc")] = timestamp;
    clipRoot[QStringLiteral("track_memory")] = trackMemory;
    clipRoot[QStringLiteral("updated_at_utc")] = timestamp;
    clipsRoot[clip.id] = clipRoot;
    speakerFlow[QStringLiteral("clips")] = clipsRoot;
    transcriptRoot[QStringLiteral("speaker_flow")] = speakerFlow;
    if (!updateLoadedTranscriptDocument([&](QJsonObject& root) {
            root = transcriptRoot;
            return true;
        }) || !saveLoadedTranscriptDocument()) {
        return;
    }

    identityClipMemory[QStringLiteral("tracks")] = identityTracksRoot;
    identityClipMemory[QStringLiteral("updated_at_utc")] = timestamp;
    memoryByClip[clip.id] = identityClipMemory;
    identityRoot[QStringLiteral("schema")] = QStringLiteral("jcut_identity_v1");
    identityRoot[QStringLiteral("updated_at_utc")] = timestamp;
    identityRoot[QStringLiteral("track_memory_by_clip")] = memoryByClip;
    engine.saveIdentityArtifact(m_transcriptSession.transcriptPath(), identityRoot);
}

QJsonObject SpeakersTab::resolveFaceDetectionsAssignmentRow(const TimelineClip& clip,
                                                        const QJsonArray& streams,
                                                        const QJsonObject& row) const
{
    const QString identityId = row.value(QStringLiteral("identity_id")).toString().trimmed();
    if (identityId.isEmpty() || streams.isEmpty()) {
        return {};
    }

    const int storedTrackId = row.value(QStringLiteral("track_id")).toInt(-1);
    const QString storedStreamId = row.value(QStringLiteral("stream_id")).toString().trimmed();
    const bool hasAnchor =
        row.contains(QString(kSpeakerFlowAnchorSourceFrameKey)) &&
        row.contains(QString(kSpeakerFlowAnchorXKey)) &&
        row.contains(QString(kSpeakerFlowAnchorYKey)) &&
        row.contains(QString(kSpeakerFlowAnchorBoxSizeKey));
    const int64_t anchorSourceFrame =
        row.value(QString(kSpeakerFlowAnchorSourceFrameKey)).toVariant().toLongLong();
    const qreal anchorX = qBound<qreal>(0.0, row.value(QString(kSpeakerFlowAnchorXKey)).toDouble(0.5), 1.0);
    const qreal anchorY = qBound<qreal>(0.0, row.value(QString(kSpeakerFlowAnchorYKey)).toDouble(0.5), 1.0);
    const qreal anchorBox = qBound<qreal>(0.01, row.value(QString(kSpeakerFlowAnchorBoxSizeKey)).toDouble(0.2), 1.0);
    const QVector<RenderSyncMarker> renderSyncMarkers =
        m_speakerDeps.getRenderSyncMarkers ? m_speakerDeps.getRenderSyncMarkers() : QVector<RenderSyncMarker>{};

    QJsonObject bestResolved;
    double bestScore = std::numeric_limits<double>::max();
    for (const QJsonValue& streamValue : streams) {
        const QJsonObject streamObj = streamValue.toObject();
        const int trackId = streamObj.value(QStringLiteral("track_id")).toInt(-1);
        const QString streamId = streamObj.value(QStringLiteral("stream_id")).toString().trimmed();
        const QJsonArray keyframes = streamObj.value(QStringLiteral("keyframes")).toArray();
        if (trackId < 0 || keyframes.isEmpty()) {
            continue;
        }

        if (!hasAnchor) {
            if ((storedTrackId >= 0 && trackId == storedTrackId) ||
                (!storedStreamId.isEmpty() && streamId == storedStreamId)) {
                QJsonObject resolved = row;
                resolved[QStringLiteral("track_id")] = trackId;
                resolved[QStringLiteral("stream_id")] = streamId;
                return resolved;
            }
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
            const qreal x =
                qBound<qreal>(0.0, keyframeObj.value(QString(kTranscriptSpeakerLocationXKey)).toDouble(0.5), 1.0);
            const qreal y =
                qBound<qreal>(0.0, keyframeObj.value(QString(kTranscriptSpeakerLocationYKey)).toDouble(0.5), 1.0);
            const qreal box =
                qBound<qreal>(0.01, keyframeObj.value(QString(kTranscriptSpeakerTrackingBoxSizeKey)).toDouble(0.2), 1.0);
            const qreal posDist = std::hypot(x - anchorX, y - anchorY);
            const qreal boxDist = std::abs(box - anchorBox);
            const qreal frameDist =
                static_cast<qreal>(std::llabs(sourceFrame - anchorSourceFrame));
            const double score = (frameDist / 12.0) + (posDist * 6.0) + (boxDist * 3.0);
            if (score < bestScore) {
                bestScore = score;
                bestResolved = row;
                bestResolved[QStringLiteral("track_id")] = trackId;
                bestResolved[QStringLiteral("stream_id")] = streamId;
            }
        }
    }

    if (!bestResolved.isEmpty()) {
        return bestResolved;
    }

    return {};
}

QHash<int, QString> SpeakersTab::resolvedIdentityByTrackId(const TimelineClip& clip,
                                                           const QJsonArray& streams) const
{
    return trackIdentityResolutionCacheForClip(clip, streams).identityByTrackId;
}

const SpeakersTab::TrackIdentityResolutionCache& SpeakersTab::trackIdentityResolutionCacheForClip(
    const TimelineClip& clip,
    const QJsonArray& streams) const
{
    const QJsonObject speakerFlow = m_transcriptSession.rootObject().value(QStringLiteral("speaker_flow")).toObject();
    const QJsonObject clipsRoot = speakerFlow.value(QStringLiteral("clips")).toObject();
    const QJsonObject clipFlow = clipsRoot.value(clip.id.trimmed()).toObject();
    const QJsonObject resolvedCurrent = clipFlow.value(QStringLiteral("resolved_current")).toObject();
    const QJsonArray resolvedMap = resolvedCurrent.value(QStringLiteral("track_identity_map")).toArray();
    const QString signature =
        trackIdentityResolutionSignature(m_transcriptSession.transcriptPath(), clip, resolvedMap, streams);
    const QString cacheKey = clip.id.trimmed();
    const auto cached = m_trackIdentityResolutionCache.constFind(cacheKey);
    if (cached != m_trackIdentityResolutionCache.cend() && cached.value().signature == signature) {
        return cached.value();
    }

    TrackIdentityResolutionCache nextCache;
    nextCache.signature = signature;
    const QVector<RenderSyncMarker> renderSyncMarkers =
        m_speakerDeps.getRenderSyncMarkers ? m_speakerDeps.getRenderSyncMarkers() : QVector<RenderSyncMarker>{};
    const QVector<CachedFacestreamTrack> cachedTracks =
        buildCachedFacestreamTracks(clip, streams, renderSyncMarkers);
    for (const QJsonValue& value : resolvedMap) {
        const QJsonObject row = value.toObject();
        const int trackId = resolveTrackIdFromCachedTracks(cachedTracks, row);
        const QString identityId = row.value(QStringLiteral("identity_id")).toString().trimmed();
        if (trackId >= 0 && !identityId.isEmpty()) {
            nextCache.identityByTrackId.insert(trackId, identityId);
            QVector<int>& trackIds = nextCache.trackIdsByIdentity[identityId];
            if (!trackIds.contains(trackId)) {
                trackIds.push_back(trackId);
            }
        }
    }
    auto inserted = m_trackIdentityResolutionCache.insert(cacheKey, nextCache);
    return inserted.value();
}

QVector<int> SpeakersTab::resolvedAssignedTrackIdsForSpeaker(const TimelineClip& clip,
                                                             const QJsonArray& streams,
                                                             const QString& speakerId) const
{
    return trackIdentityResolutionCacheForClip(clip, streams).trackIdsByIdentity.value(speakerId.trimmed());
}

QString SpeakersTab::assignedFaceDetectionsPreviewTooltipHtml(const TimelineClip& clip,
                                                          const QString& speakerId) const
{
    const QFileInfo transcriptInfo(m_transcriptSession.transcriptPath());
    const qint64 artifactRevisionMs = facedetectionsArtifactRevisionMsForTranscript(m_transcriptSession.transcriptPath());
    const QString cacheKey = QStringLiteral("%1|%2|%3|%4")
        .arg(clip.id)
        .arg(speakerId)
        .arg(transcriptInfo.exists() ? transcriptInfo.lastModified().toMSecsSinceEpoch() : 0)
        .arg(artifactRevisionMs);
    const auto cached = m_avatarHoverTooltipHtmlCache.constFind(cacheKey);
    if (cached != m_avatarHoverTooltipHtmlCache.cend()) {
        return cached.value();
    }

    const QVector<QPixmap> previews = assignedFaceDetectionsPreviewPixmaps(clip, speakerId);
    if (previews.isEmpty()) {
        return QString();
    }

    QString html = QStringLiteral("<div style='white-space:nowrap;'>");
    int count = 0;
    for (const QPixmap& pixmap : previews) {
        if (pixmap.isNull()) {
            continue;
        }
        QByteArray bytes;
        QBuffer buffer(&bytes);
        buffer.open(QIODevice::WriteOnly);
        pixmap.save(&buffer, "PNG");
        html += QStringLiteral("<img width='72' height='72' style='margin:2px;border:1px solid #f4d35e;border-radius:6px;' src='data:image/png;base64,%1' />")
                    .arg(QString::fromLatin1(bytes.toBase64()));
        ++count;
        if (count >= 12) {
            break;
        }
    }
    html += QStringLiteral("</div>");
    if (count == 0) {
        return QString();
    }
    m_avatarHoverTooltipHtmlCache.insert(cacheKey, html);
    return html;
}

void SpeakersTab::showSpeakerAvatarHoverPreview(const QString& speakerId, const QPoint& globalPos)
{
    const TimelineClip* clip = m_deps.getSelectedClip ? m_deps.getSelectedClip() : nullptr;
    if (!clip || speakerId.isEmpty()) {
        QToolTip::hideText();
        return;
    }
    const QString html = assignedFaceDetectionsPreviewTooltipHtml(*clip, speakerId);
    if (html.isEmpty()) {
        QToolTip::hideText();
        return;
    }
    QToolTip::showText(globalPos, html, m_widgets.speakersTable);
}

void SpeakersTab::hideSpeakerAvatarHoverPreview()
{
    QToolTip::hideText();
}

void SpeakersTab::requestRefreshFaceDetectionsPathsPanel()
{
    const bool playbackActive = m_speakerDeps.isPlaybackActive && m_speakerDeps.isPlaybackActive();
    if (playbackActive) {
        m_faceStreamPanelRefreshQueued = true;
        m_faceStreamPanelRefreshDeferredForPlayback = true;
        return;
    }
    if (!m_faceStreamPanelRefreshTimer) {
        refreshFaceDetectionsPathsPanel();
        return;
    }
    m_faceStreamPanelRefreshQueued = true;
    m_faceStreamPanelRefreshTimer->start();
}

void SpeakersTab::flushDeferredPlaybackRefreshes()
{
    if (!m_faceStreamPanelRefreshDeferredForPlayback) {
        return;
    }
    m_faceStreamPanelRefreshDeferredForPlayback = false;
    m_faceStreamPanelRefreshQueued = false;
    if (m_faceStreamPanelRefreshTimer) {
        m_faceStreamPanelRefreshTimer->stop();
    }
    m_faceStreamPanelRefreshSignature.clear();
    refreshFaceDetectionsPathsPanel();
}

void SpeakersTab::refreshFaceDetectionsPathsPanel()
{
    QElapsedTimer refreshTimer;
    refreshTimer.start();
    const bool playbackActive = m_speakerDeps.isPlaybackActive && m_speakerDeps.isPlaybackActive();
    if (playbackActive) {
        m_lastFaceDetectionsDebugSnapshot = QJsonObject{
            {QStringLiteral("status"), QStringLiteral("deferred_for_playback")},
            {QStringLiteral("transcript_path"), m_transcriptSession.transcriptPath()},
            {QStringLiteral("refresh_signature"), m_faceStreamPanelRefreshSignature}
        };
        m_faceStreamPanelRefreshQueued = true;
        m_faceStreamPanelRefreshDeferredForPlayback = true;
        if (m_widgets.speakerFaceDetectionsDetailsEdit) {
            m_widgets.speakerFaceDetectionsDetailsEdit->setPlainText(
                QStringLiteral("FaceDetections path refresh deferred until playback stops."));
        }
        m_lastFaceDetectionsPanelRefreshDurationMs = refreshTimer.elapsed();
        m_maxFaceDetectionsPanelRefreshDurationMs =
            qMax(m_maxFaceDetectionsPanelRefreshDurationMs, m_lastFaceDetectionsPanelRefreshDurationMs);
        return;
    }
    m_faceStreamPanelRefreshDeferredForPlayback = false;
    if (!m_widgets.speakerFaceDetectionsTable || m_refreshingFaceDetectionsPathsPanel) {
        m_lastFaceDetectionsPanelRefreshDurationMs = refreshTimer.elapsed();
        m_maxFaceDetectionsPanelRefreshDurationMs =
            qMax(m_maxFaceDetectionsPanelRefreshDurationMs, m_lastFaceDetectionsPanelRefreshDurationMs);
        return;
    }
    m_refreshingFaceDetectionsPathsPanel = true;
    struct RefreshGuard {
        bool& flag;
        qint64* lastDurationMs = nullptr;
        qint64* maxDurationMs = nullptr;
        QElapsedTimer* timer = nullptr;
        ~RefreshGuard() {
            flag = false;
            if (lastDurationMs && maxDurationMs && timer) {
                *lastDurationMs = timer->elapsed();
                *maxDurationMs = qMax(*maxDurationMs, *lastDurationMs);
            }
        }
    } guard{m_refreshingFaceDetectionsPathsPanel,
            &m_lastFaceDetectionsPanelRefreshDurationMs,
            &m_maxFaceDetectionsPanelRefreshDurationMs,
            &refreshTimer};

    if (!m_transcriptSession.hasObjectDocument()) {
        m_lastFaceDetectionsDebugSnapshot = QJsonObject{
            {QStringLiteral("status"), QStringLiteral("missing_transcript_document")},
            {QStringLiteral("transcript_path"), m_transcriptSession.transcriptPath()}
        };
        return;
    }
    const TimelineClip* clip = m_deps.getSelectedClip ? m_deps.getSelectedClip() : nullptr;
    if (!clip) {
        m_lastFaceDetectionsDebugSnapshot = QJsonObject{
            {QStringLiteral("status"), QStringLiteral("missing_selected_clip")},
            {QStringLiteral("transcript_path"), m_transcriptSession.transcriptPath()}
        };
        return;
    }
    const QFileInfo transcriptInfo(m_transcriptSession.transcriptPath());
    const qint64 artifactRevisionMs = facedetectionsArtifactRevisionMsForTranscript(m_transcriptSession.transcriptPath());
    const QString refreshSignature =
        clip->id + QLatin1Char('|') +
        m_transcriptSession.transcriptPath() + QLatin1Char('|') +
        QString::number(transcriptInfo.exists() ? transcriptInfo.lastModified().toMSecsSinceEpoch() : 0) +
        QLatin1Char('|') +
        QString::number(artifactRevisionMs);
    if (refreshSignature == m_faceStreamPanelRefreshSignature) {
        m_lastFaceDetectionsDebugSnapshot[QStringLiteral("status")] = QStringLiteral("refresh_signature_unchanged");
        m_lastFaceDetectionsDebugSnapshot[QStringLiteral("refresh_signature")] = refreshSignature;
        return;
    }

    QSignalBlocker tableBlocker(m_widgets.speakerFaceDetectionsTable);
    QSignalBlocker selectionBlocker(
        m_widgets.speakerFaceDetectionsTable->selectionModel());
    m_widgets.speakerFaceDetectionsTable->clearContents();
    m_widgets.speakerFaceDetectionsTable->setRowCount(0);
    if (m_widgets.speakerFaceDetectionsTable->columnCount() >= 5) {
        m_widgets.speakerFaceDetectionsTable->setHorizontalHeaderLabels(
            QStringList{
                QStringLiteral("Stream"),
                QStringLiteral("Track"),
                QStringLiteral("Assignment"),
                QStringLiteral("Range"),
                QStringLiteral("Source")
            });
    }
    m_faceStreamPanelRows = QJsonArray();
    if (m_widgets.speakerFaceDetectionsDetailsEdit) {
        m_widgets.speakerFaceDetectionsDetailsEdit->setPlainText(
            QStringLiteral("Select a FaceDetections path row to inspect full JSON."));
    }
    if (m_widgets.speakerRawDetectionDetailsEdit) {
        m_widgets.speakerRawDetectionDetailsEdit->setPlainText(
            QStringLiteral("Select a detection row to inspect full JSON."));
    }
    if (m_widgets.speakerRawDetectionTable) {
        QSignalBlocker rawTableBlocker(m_widgets.speakerRawDetectionTable);
        m_widgets.speakerRawDetectionTable->clearContents();
        m_widgets.speakerRawDetectionTable->setRowCount(0);
    }

    editor::TranscriptEngine transcriptEngine;
    QJsonObject artifactRoot;
    QJsonObject continuityRoot;
    const bool loadedArtifact =
        transcriptEngine.loadFacestreamArtifact(m_transcriptSession.transcriptPath(), &artifactRoot);
    if (loadedArtifact) {
        continuityRoot = continuityRootForClip(artifactRoot, clip->id);
    }
    const auto resolveRawContinuityRoot = [&](const QJsonObject& root) -> QJsonObject {
        if (!root.value(QStringLiteral("raw_tracks")).toArray().isEmpty()) {
            return root;
        }
        const QString processedArtifactPath =
            root.value(QStringLiteral("processed_artifact_path")).toString().trimmed();
        const QString clipId = root.value(QStringLiteral("clip_id")).toString().trimmed();
        if (processedArtifactPath.isEmpty() || clipId.isEmpty()) {
            return root;
        }
        QJsonObject processedArtifact;
        if (!jcut::facedetections::readBinaryJsonObject(processedArtifactPath, &processedArtifact, nullptr)) {
            return root;
        }
        const QJsonObject processedRoot = continuityRootForClip(processedArtifact, clipId);
        const QString rawArtifactPath =
            processedRoot.value(QStringLiteral("source_raw_artifact_path")).toString().trimmed();
        if (rawArtifactPath.isEmpty()) {
            return root;
        }
        QJsonObject rawArtifact;
        if (!jcut::facedetections::readBinaryJsonObject(rawArtifactPath, &rawArtifact, nullptr)) {
            return root;
        }
        const QJsonObject rawRoot = continuityRootForClip(rawArtifact, clipId);
        return rawRoot.isEmpty() ? root : rawRoot;
    };
    continuityRoot = resolveRawContinuityRoot(continuityRoot);
    const QString externalRawTracksPath =
        continuityRoot.value(QStringLiteral("raw_tracks_artifact_path")).toString().trimmed();
    const QString externalRawFramesPath =
        continuityRoot.value(QStringLiteral("raw_frames_artifact_path")).toString().trimmed();
    const bool hasExternalRawTracks = !externalRawTracksPath.isEmpty();
    const bool hasExternalRawFrames = !externalRawFramesPath.isEmpty();
    if (m_widgets.speakerDetectionsAvailableCheckBox) {
        QSignalBlocker block(m_widgets.speakerDetectionsAvailableCheckBox);
        m_widgets.speakerDetectionsAvailableCheckBox->setChecked(
            hasExternalRawFrames || !continuityRoot.value(QStringLiteral("raw_frames")).toArray().isEmpty());
    }
    if (m_widgets.speakerTracksAvailableCheckBox) {
        QSignalBlocker block(m_widgets.speakerTracksAvailableCheckBox);
        m_widgets.speakerTracksAvailableCheckBox->setChecked(
            hasExternalRawTracks || !continuityRoot.value(QStringLiteral("raw_tracks")).toArray().isEmpty());
    }

    const QJsonObject transcriptRoot = m_transcriptSession.rootObject();
    QJsonArray streams;
    QJsonArray streamSummaries;
    const QJsonArray rawTracks = continuityRoot.value(QStringLiteral("raw_tracks")).toArray();
    streams = jcut::facedetections::storedContinuityStreamsForRoot(continuityRoot);
    if (!streams.isEmpty()) {
        for (const QJsonValue& value : streams) {
            const QJsonObject streamObj = value.toObject();
            QJsonObject summary;
            summary[QStringLiteral("track_id")] = streamObj.value(QStringLiteral("track_id")).toInt(-1);
            summary[QStringLiteral("stream_id")] = streamObj.value(QStringLiteral("stream_id")).toString();
            summary[QStringLiteral("frame_domain")] = streamObj.value(QStringLiteral("frame_domain")).toString();
            summary[QStringLiteral("source")] = streamObj.value(QStringLiteral("source")).toString();
            const QJsonArray keyframes = streamObj.value(QStringLiteral("keyframes")).toArray();
            qint64 minFrame = std::numeric_limits<qint64>::max();
            qint64 maxFrame = std::numeric_limits<qint64>::min();
            QVector<int64_t> frames;
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
            streamSummaries.push_back(summary);
        }
    } else {
        streamSummaries = jcut::facedetections::continuityTrackSummariesForRoot(
            continuityRoot,
            transcriptRoot);
        if (streamSummaries.isEmpty()) {
            streams = continuityStreamsForClip(*clip);
            for (const QJsonValue& value : streams) {
                const QJsonObject streamObj = value.toObject();
                QJsonObject summary;
                summary[QStringLiteral("track_id")] = streamObj.value(QStringLiteral("track_id")).toInt(-1);
                summary[QStringLiteral("stream_id")] = streamObj.value(QStringLiteral("stream_id")).toString();
                summary[QStringLiteral("frame_domain")] = streamObj.value(QStringLiteral("frame_domain")).toString();
                summary[QStringLiteral("source")] = streamObj.value(QStringLiteral("source")).toString();
                const QJsonArray keyframes = streamObj.value(QStringLiteral("keyframes")).toArray();
                qint64 minFrame = std::numeric_limits<qint64>::max();
                qint64 maxFrame = std::numeric_limits<qint64>::min();
                QVector<int64_t> frames;
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
                streamSummaries.push_back(summary);
            }
        }
    }
    m_faceStreamPanelRefreshSignature = refreshSignature;

    QString panelMode = QStringLiteral("track_summaries");
    if (!streams.isEmpty()) {
        panelMode = QStringLiteral("stored_streams");
    } else if (!rawTracks.isEmpty()) {
        panelMode = QStringLiteral("inline_track_summaries");
    } else if (hasExternalRawTracks) {
        panelMode = QStringLiteral("external_track_summaries");
    }

    QHash<int, QString> identityByTrackId = streams.isEmpty()
        ? QHash<int, QString>{}
        : resolvedIdentityByTrackId(*clip, streams);
    if (identityByTrackId.isEmpty()) {
        const QJsonObject speakerFlow = transcriptRoot.value(QStringLiteral("speaker_flow")).toObject();
        const QJsonObject clipsRoot = speakerFlow.value(QStringLiteral("clips")).toObject();
        const QJsonObject clipRoot = clipsRoot.value(clip->id.trimmed()).toObject();
        const QJsonObject resolvedCurrent = clipRoot.value(QStringLiteral("resolved_current")).toObject();
        const QJsonArray resolvedMap = resolvedCurrent.value(QStringLiteral("track_identity_map")).toArray();
        for (const QJsonValue& value : resolvedMap) {
            const QJsonObject row = value.toObject();
            const int trackId = row.value(QStringLiteral("track_id")).toInt(-1);
            const QString identityId = row.value(QStringLiteral("identity_id")).toString().trimmed();
            if (trackId >= 0 && !identityId.isEmpty()) {
                identityByTrackId.insert(trackId, identityId);
            }
        }
    }

    struct StreamRow {
        QJsonObject streamObj;
        int trackId = -1;
        QString identityId;
        int64_t minFrame = -1;
        int64_t maxFrame = -1;
        QString sourceTag;
        int keyframeCount = 0;
    };
    QVector<StreamRow> panelRows;
    panelRows.reserve(streamSummaries.size());
    for (int row = 0; row < streamSummaries.size(); ++row) {
        const QJsonObject streamSummary = streamSummaries.at(row).toObject();
        const int trackId = streamSummary.value(QStringLiteral("track_id")).toInt(-1);
        StreamRow panelRow;
        panelRow.streamObj = streams.size() == streamSummaries.size()
            ? streams.at(row).toObject()
            : streamSummary;
        panelRow.trackId = trackId;
        panelRow.identityId = identityByTrackId.value(trackId).trimmed();
        panelRow.minFrame = streamSummary.value(QStringLiteral("min_frame")).toVariant().toLongLong();
        panelRow.maxFrame = streamSummary.value(QStringLiteral("max_frame")).toVariant().toLongLong();
        panelRow.sourceTag = streamSummary.value(QStringLiteral("source")).toString().trimmed();
        panelRow.keyframeCount = streamSummary.value(QStringLiteral("keyframe_count")).toInt(0);
        panelRows.push_back(panelRow);
    }
    std::sort(panelRows.begin(), panelRows.end(), [](const StreamRow& a, const StreamRow& b) {
        const bool aAssigned = !a.identityId.isEmpty();
        const bool bAssigned = !b.identityId.isEmpty();
        if (aAssigned != bAssigned) {
            return aAssigned && !bAssigned;
        }
        if (aAssigned && bAssigned && a.identityId != b.identityId) {
            return a.identityId.localeAwareCompare(b.identityId) < 0;
        }
        return a.trackId < b.trackId;
    });

    int assignedCount = 0;
    int unassignedCount = 0;
    for (const StreamRow& row : std::as_const(panelRows)) {
        if (row.identityId.isEmpty()) {
            ++unassignedCount;
        } else {
            ++assignedCount;
        }
        m_faceStreamPanelRows.push_back(row.streamObj);
    }

    m_widgets.speakerFaceDetectionsTable->setRowCount(panelRows.size());
    for (int row = 0; row < panelRows.size(); ++row) {
        const StreamRow& panelRow = panelRows.at(row);
        const QJsonObject streamObj = panelRow.streamObj;
        const QString streamId = streamObj.value(QStringLiteral("stream_id")).toString().trimmed();
        const QString rangeText = panelRow.keyframeCount <= 0
            ? QStringLiteral("-")
            : QStringLiteral("%1..%2").arg(panelRow.minFrame).arg(panelRow.maxFrame);
        auto* streamItem = new QTableWidgetItem(streamId.isEmpty() ? QStringLiteral("—") : streamId);
        streamItem->setData(Qt::UserRole + 1, row);
        const qlonglong seekFrame = panelRow.keyframeCount <= 0
            ? static_cast<qlonglong>(-1)
            : static_cast<qlonglong>(panelRow.minFrame);
        streamItem->setData(Qt::UserRole + 2, QVariant(seekFrame));
        auto* trackItem = new QTableWidgetItem(panelRow.trackId >= 0 ? QString::number(panelRow.trackId) : QStringLiteral("—"));
        const bool assigned = !panelRow.identityId.isEmpty();
        auto* countItem = new QTableWidgetItem(
            assigned ? panelRow.identityId : QStringLiteral("Unassigned"));
        auto* rangeItem = new QTableWidgetItem(rangeText);
        auto* sourceItem = new QTableWidgetItem(
            panelRow.sourceTag.isEmpty() ? QStringLiteral("continuity_track_v1") : panelRow.sourceTag);
        countItem->setToolTip(
            assigned
                ? QStringLiteral("Assigned to speaker identity: %1").arg(panelRow.identityId)
                : QStringLiteral("No speaker identity assignment yet."));
        rangeItem->setToolTip(QStringLiteral("Keyframes: %1").arg(panelRow.keyframeCount));
        if (!assigned) {
            const QColor bg(QStringLiteral("#3a2a2a"));
            streamItem->setBackground(bg);
            trackItem->setBackground(bg);
            countItem->setBackground(bg);
            rangeItem->setBackground(bg);
            sourceItem->setBackground(bg);
        }
        m_widgets.speakerFaceDetectionsTable->setItem(row, 0, streamItem);
        m_widgets.speakerFaceDetectionsTable->setItem(row, 1, trackItem);
        m_widgets.speakerFaceDetectionsTable->setItem(row, 2, countItem);
        m_widgets.speakerFaceDetectionsTable->setItem(row, 3, rangeItem);
        m_widgets.speakerFaceDetectionsTable->setItem(row, 4, sourceItem);
    }
    if (panelRows.isEmpty() && m_widgets.speakerFaceDetectionsDetailsEdit) {
        m_widgets.speakerFaceDetectionsDetailsEdit->setPlainText(
            QStringLiteral("No FaceDetections paths found for this clip. Run JCut DNN FaceDetections Generator first."));
    } else if (!panelRows.isEmpty()) {
        if (m_widgets.speakerFaceDetectionsDetailsEdit) {
            QString detail = QStringLiteral("Track assignment summary\n\nAssigned: %1\nUnassigned: %2\nTotal: %3\n\nSelect a row to inspect full JSON.")
                .arg(assignedCount)
                .arg(unassignedCount)
                .arg(panelRows.size());
            if (streams.isEmpty() && (hasExternalRawTracks || !rawTracks.isEmpty())) {
                detail += QStringLiteral("\n\nRows are summarized first and full per-track JSON is loaded on demand.");
            }
            m_widgets.speakerFaceDetectionsDetailsEdit->setPlainText(detail);
        }
        m_widgets.speakerFaceDetectionsTable->setCurrentCell(0, 0);
    }
    m_lastFaceDetectionsDebugSnapshot = QJsonObject{
        {QStringLiteral("status"), QStringLiteral("ok")},
        {QStringLiteral("clip_id"), clip->id},
        {QStringLiteral("transcript_path"), m_transcriptSession.transcriptPath()},
        {QStringLiteral("artifact_revision_ms"), artifactRevisionMs},
        {QStringLiteral("refresh_signature"), refreshSignature},
        {QStringLiteral("artifact_loaded"), loadedArtifact},
        {QStringLiteral("continuity_root_found"), !continuityRoot.isEmpty()},
        {QStringLiteral("continuity_root_has_tracks"),
         jcut::facedetections::continuityRootHasTracks(continuityRoot, transcriptRoot)},
        {QStringLiteral("continuity_root_has_stored_payload"),
         jcut::facedetections::continuityRootHasStoredPayload(continuityRoot)},
        {QStringLiteral("panel_mode"), panelMode},
        {QStringLiteral("stored_stream_count"), streams.size()},
        {QStringLiteral("summary_count"), streamSummaries.size()},
        {QStringLiteral("panel_row_count"), panelRows.size()},
        {QStringLiteral("raw_tracks_inline_count"), rawTracks.size()},
        {QStringLiteral("raw_frames_inline_count"),
         continuityRoot.value(QStringLiteral("raw_frames")).toArray().size()},
        {QStringLiteral("has_external_raw_tracks"), hasExternalRawTracks},
        {QStringLiteral("has_external_raw_frames"), hasExternalRawFrames},
        {QStringLiteral("raw_tracks_artifact_path"), externalRawTracksPath},
        {QStringLiteral("raw_frames_artifact_path"), externalRawFramesPath},
        {QStringLiteral("processed_artifact_path"),
         continuityRoot.value(QStringLiteral("processed_artifact_path")).toString().trimmed()},
        {QStringLiteral("source_raw_artifact_path"),
         continuityRoot.value(QStringLiteral("source_raw_artifact_path")).toString().trimmed()},
        {QStringLiteral("imported_from_artifact_dir"),
         continuityRoot.value(QStringLiteral("imported_from_artifact_dir")).toString().trimmed()}
    };
    updateSpeakerTrackingStatusLabel();

    refreshRawDetectionsPanel(continuityRoot);
}

void SpeakersTab::refreshRawDetectionsPanel(const QJsonObject& continuityRoot)
{
    QElapsedTimer refreshTimer;
    refreshTimer.start();
    auto finalizeRefreshTiming = [this, &refreshTimer]() {
        m_lastRawDetectionsPanelRefreshDurationMs = refreshTimer.elapsed();
        m_maxRawDetectionsPanelRefreshDurationMs =
            qMax(m_maxRawDetectionsPanelRefreshDurationMs, m_lastRawDetectionsPanelRefreshDurationMs);
    };
    if (!m_widgets.speakerRawDetectionTable) {
        finalizeRefreshTiming();
        return;
    }

    QSignalBlocker tableBlocker(m_widgets.speakerRawDetectionTable);
    m_widgets.speakerRawDetectionTable->clearContents();
    m_widgets.speakerRawDetectionTable->setRowCount(0);
    if (m_widgets.speakerRawDetectionDetailsEdit) {
        m_widgets.speakerRawDetectionDetailsEdit->setPlainText(
            QStringLiteral("Select a detection row to inspect full JSON."));
    }

    const TimelineClip* clip = m_deps.getSelectedClip ? m_deps.getSelectedClip() : nullptr;
    if (!clip) {
        finalizeRefreshTiming();
        return;
    }

    const QJsonArray rawFrames = continuityRoot.value(QStringLiteral("raw_frames")).toArray();
    if (rawFrames.isEmpty()) {
        if (m_widgets.speakerRawDetectionDetailsEdit) {
            m_widgets.speakerRawDetectionDetailsEdit->setPlainText(
                QStringLiteral("No raw detections were imported for this clip."));
        }
        finalizeRefreshTiming();
        return;
    }

    FacestreamFrameDomain frameDomain = FacestreamFrameDomain::SourceRelative;
    if (!continuityPayloadFrameDomain(
            continuityRoot,
            QStringLiteral("raw_frames_frame_domain"),
            &frameDomain)) {
        if (m_widgets.speakerRawDetectionDetailsEdit) {
            m_widgets.speakerRawDetectionDetailsEdit->setPlainText(
                QStringLiteral("Raw detections are present, but frame-domain metadata is missing. "
                               "This artifact does not satisfy the current contract."));
        }
        finalizeRefreshTiming();
        return;
    }
    const int64_t timelineFrame =
        m_deps.getCurrentTimelineFrame ? m_deps.getCurrentTimelineFrame() : clip->startFrame;
    const QVector<RenderSyncMarker> markers =
        m_speakerDeps.getRenderSyncMarkers
            ? m_speakerDeps.getRenderSyncMarkers()
            : QVector<RenderSyncMarker>{};
    const int64_t absoluteSourceFrame =
        sourceFrameForClipAtTimelinePosition(*clip, static_cast<qreal>(timelineFrame), markers);
    const int64_t localTimelineFrame = qMax<int64_t>(0, timelineFrame - clip->startFrame);
    const int64_t localSourceFrame =
        qMax<int64_t>(0, absoluteSourceFrame - qMax<int64_t>(0, clip->sourceInFrame));
    const int64_t lookupFrame = facedetectionsLookupFrameForDomain(
        frameDomain, localTimelineFrame, localSourceFrame, absoluteSourceFrame);
    const QString frameLabel =
        frameDomain == FacestreamFrameDomain::ClipTimeline30Fps
            ? QStringLiteral("clip frame")
            : (frameDomain == FacestreamFrameDomain::SourceRelative
                   ? QStringLiteral("local source frame")
                   : QStringLiteral("source frame"));

    QJsonArray detectionsForFrame;
    for (const QJsonValue& frameValue : continuityRoot.value(QStringLiteral("raw_frames")).toArray()) {
        const QJsonObject frameObj = frameValue.toObject();
        const int64_t frameNumber =
            frameObj.value(QStringLiteral("frame")).toVariant().toLongLong();
        if (frameNumber != lookupFrame) {
            continue;
        }
        detectionsForFrame = frameObj.value(QStringLiteral("detections")).toArray();
        break;
    }

    if (detectionsForFrame.isEmpty()) {
        if (m_widgets.speakerRawDetectionDetailsEdit) {
            m_widgets.speakerRawDetectionDetailsEdit->setPlainText(
                QStringLiteral("No raw detections at %1 %2.")
                    .arg(frameLabel)
                    .arg(lookupFrame));
        }
        finalizeRefreshTiming();
        return;
    }

    m_widgets.speakerRawDetectionTable->setRowCount(detectionsForFrame.size());
    for (int row = 0; row < detectionsForFrame.size(); ++row) {
        const QJsonObject detectionObj = detectionsForFrame.at(row).toObject();
        auto* indexItem = new QTableWidgetItem(QString::number(row + 1));
        indexItem->setData(
            Qt::UserRole + 1,
            QString::fromUtf8(QJsonDocument(detectionObj).toJson(QJsonDocument::Indented)));
        auto* confItem = new QTableWidgetItem(
            QString::number(detectionObj.value(QStringLiteral("confidence")).toDouble(
                                detectionObj.value(QStringLiteral("score")).toDouble(0.0)),
                            'f',
                            3));
        auto* xItem = new QTableWidgetItem(
            QString::number(detectionObj.value(QStringLiteral("x")).toDouble(0.0), 'f', 3));
        auto* yItem = new QTableWidgetItem(
            QString::number(detectionObj.value(QStringLiteral("y")).toDouble(0.0), 'f', 3));
        auto* wItem = new QTableWidgetItem(
            QString::number(detectionObj.value(QStringLiteral("w")).toDouble(0.0), 'f', 3));
        auto* hItem = new QTableWidgetItem(
            QString::number(detectionObj.value(QStringLiteral("h")).toDouble(0.0), 'f', 3));
        m_widgets.speakerRawDetectionTable->setItem(row, 0, indexItem);
        m_widgets.speakerRawDetectionTable->setItem(row, 1, confItem);
        m_widgets.speakerRawDetectionTable->setItem(row, 2, xItem);
        m_widgets.speakerRawDetectionTable->setItem(row, 3, yItem);
        m_widgets.speakerRawDetectionTable->setItem(row, 4, wItem);
        m_widgets.speakerRawDetectionTable->setItem(row, 5, hItem);
    }
    m_widgets.speakerRawDetectionTable->setCurrentCell(0, 0);
    if (m_widgets.speakerRawDetectionDetailsEdit) {
        m_widgets.speakerRawDetectionDetailsEdit->setPlainText(
            QStringLiteral("Raw detections at %1 %2: %3\n\nSelect a row to inspect full JSON.")
                .arg(frameLabel)
                .arg(lookupFrame)
                .arg(detectionsForFrame.size()));
    }
    finalizeRefreshTiming();
}

void SpeakersTab::clearFaceDetectionsDerivedCaches()
{
    m_avatarCache.clear();
    m_avatarHoverTooltipHtmlCache.clear();
    m_continuityStreamsCache.clear();
    m_trackIdentityResolutionCache.clear();
    m_speakersTableRefreshSignature.clear();
}

QJsonObject SpeakersTab::makeTrackAssignmentAnchor(int trackId,
                                                   const QString& streamId,
                                                   int64_t sourceFrame,
                                                   qreal xNorm,
                                                   qreal yNorm,
                                                   qreal boxSizeNorm) const
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

bool SpeakersTab::persistTrackAssignments(
    const QString& clipId,
    const QString& speakerId,
    const QJsonArray& trackAnchors,
    const QString& resolutionSource,
    const QString& auditAction,
    const QString& runIdPrefix,
    const std::function<bool(const QJsonObject&, const QJsonObject&)>& shouldReplaceResolvedRow,
    const std::function<bool(const QJsonObject&, const QJsonObject&)>& faceRefMatches)
{
    const QString trimmedSpeakerId = speakerId.trimmed();
    const QString trimmedClipId = clipId.trimmed();
    if (!activeCutMutable() || trimmedSpeakerId.isEmpty() || trimmedClipId.isEmpty() ||
        trackAnchors.isEmpty() || !m_transcriptSession.hasObjectDocument()) {
        return false;
    }

    QHash<int, QJsonObject> anchorByTrackId;
    for (const QJsonValue& value : trackAnchors) {
        const QJsonObject anchor = value.toObject();
        const int trackId = anchor.value(QStringLiteral("track_id")).toInt(-1);
        if (trackId < 0) {
            continue;
        }
        anchorByTrackId.insert(trackId, anchor);
    }
    if (anchorByTrackId.isEmpty()) {
        return false;
    }

    const QString effectiveResolutionSource = resolutionSource.trimmed().isEmpty()
        ? QStringLiteral("speaker_track_picker")
        : resolutionSource.trimmed();
    const QString effectiveAuditAction = auditAction.trimmed().isEmpty()
        ? QStringLiteral("speaker_track_picker_identity_set")
        : auditAction.trimmed();
    const QString effectiveRunPrefix = runIdPrefix.trimmed().isEmpty()
        ? QStringLiteral("track_batch")
        : runIdPrefix.trimmed();
    const QString timestamp = QDateTime::currentDateTimeUtc().toString(Qt::ISODate);

    editor::TranscriptEngine engine;
    QJsonObject transcriptRoot = m_transcriptSession.rootObject();
    QJsonObject speakerFlow = transcriptRoot.value(QStringLiteral("speaker_flow")).toObject();
    speakerFlow[QStringLiteral("schema_version")] = QStringLiteral("1.0");
    QJsonObject clipsRoot = speakerFlow.value(QStringLiteral("clips")).toObject();
    QJsonObject clipRoot = clipsRoot.value(trimmedClipId).toObject();
    clipRoot[QStringLiteral("clip_id")] = trimmedClipId;
    clipRoot[QStringLiteral("updated_at_utc")] = timestamp;

    QJsonObject resolvedPayload = clipRoot.value(QStringLiteral("resolved_current")).toObject();
    resolvedPayload[QStringLiteral("updated_at_utc")] = timestamp;
    const QJsonArray resolvedMap = resolvedPayload.value(QStringLiteral("track_identity_map")).toArray();
    QJsonArray nextResolvedMap;
    QSet<int> appliedTrackIds;
    for (const QJsonValue& value : resolvedMap) {
        QJsonObject row = value.toObject();
        bool replaced = false;
        for (auto it = anchorByTrackId.constBegin(); it != anchorByTrackId.constEnd(); ++it) {
            const QJsonObject anchor = it.value();
            if (!shouldReplaceResolvedRow(row, anchor)) {
                continue;
            }
            row[QStringLiteral("track_id")] = it.key();
            row[QStringLiteral("identity_id")] = trimmedSpeakerId;
            row[QStringLiteral("stream_id")] =
                anchor.value(QStringLiteral("stream_id")).toString().trimmed();
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
            row[QStringLiteral("resolution_source")] = effectiveResolutionSource;
            row[QStringLiteral("updated_at_utc")] = timestamp;
            appliedTrackIds.insert(it.key());
            replaced = true;
            break;
        }
        nextResolvedMap.push_back(row);
        if (replaced) {
            continue;
        }
    }
    for (auto it = anchorByTrackId.constBegin(); it != anchorByTrackId.constEnd(); ++it) {
        if (appliedTrackIds.contains(it.key())) {
            continue;
        }
        const QJsonObject anchor = it.value();
        QJsonObject row;
        row[QStringLiteral("track_id")] = it.key();
        row[QStringLiteral("identity_id")] = trimmedSpeakerId;
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
        row[QStringLiteral("resolution_source")] = effectiveResolutionSource;
        row[QStringLiteral("updated_at_utc")] = timestamp;
        nextResolvedMap.push_back(row);
    }
    resolvedPayload[QStringLiteral("track_identity_map")] = nextResolvedMap;
    clipRoot[QStringLiteral("resolved_current")] = resolvedPayload;

    QJsonObject humanRuns = clipRoot.value(QStringLiteral("human_runs")).toObject();
    const QString runId = QStringLiteral("%1_%2").arg(
        effectiveRunPrefix,
        QDateTime::currentDateTimeUtc().toString(QStringLiteral("yyyyMMddTHHmmsszzz")));
    QJsonObject humanPayload;
    humanPayload[QStringLiteral("run_id")] = runId;
    humanPayload[QStringLiteral("updated_at_utc")] = timestamp;
    QJsonArray auditLog;
    for (const QJsonValue& value : trackAnchors) {
        const QJsonObject anchor = value.toObject();
        const int trackId = anchor.value(QStringLiteral("track_id")).toInt(-1);
        if (trackId < 0) {
            continue;
        }
        QJsonObject auditRow;
        auditRow[QStringLiteral("timestamp_utc")] = timestamp;
        auditRow[QStringLiteral("action")] = effectiveAuditAction;
        auditRow[QStringLiteral("track_id")] = trackId;
        auditRow[QStringLiteral("stream_id")] = anchor.value(QStringLiteral("stream_id")).toString().trimmed();
        auditRow[QStringLiteral("identity_id")] = trimmedSpeakerId;
        auditRow[QString(kSpeakerFlowAnchorSourceFrameKey)] =
            static_cast<qint64>(qMax<int64_t>(
                0,
                anchor.value(QStringLiteral("source_frame")).toVariant().toLongLong()));
        auditRow[QString(kSpeakerFlowAnchorXKey)] =
            qBound<qreal>(0.0, anchor.value(QStringLiteral("x")).toDouble(0.5), 1.0);
        auditRow[QString(kSpeakerFlowAnchorYKey)] =
            qBound<qreal>(0.0, anchor.value(QStringLiteral("y")).toDouble(0.5), 1.0);
        auditRow[QString(kSpeakerFlowAnchorBoxSizeKey)] =
            qBound<qreal>(0.01, anchor.value(QStringLiteral("box")).toDouble(0.2), 1.0);
        auditLog.push_back(auditRow);
    }
    humanPayload[QStringLiteral("audit_log")] = auditLog;
    humanRuns[runId] = humanPayload;
    clipRoot[QStringLiteral("human_runs")] = humanRuns;
    clipRoot[QStringLiteral("latest_human_run_id")] = runId;

    QJsonObject profiles = transcriptRoot.value(QString(kTranscriptSpeakerProfilesKey)).toObject();
    for (auto profileIt = profiles.begin(); profileIt != profiles.end(); ++profileIt) {
        const QString profileSpeakerId = profileIt.key().trimmed();
        if (profileSpeakerId == trimmedSpeakerId) {
            continue;
        }
        QJsonObject otherProfile = profileIt.value().toObject();
        const QJsonArray oldRefs = speakerFaceRefs(otherProfile);
        QJsonArray nextRefs;
        bool removedRef = false;
        for (const QJsonValue& refValue : oldRefs) {
            const QJsonObject ref = refValue.toObject();
            const int refTrackId = ref.value(QStringLiteral("track_id")).toInt(-1);
            if (anchorByTrackId.contains(refTrackId)) {
                removedRef = true;
                continue;
            }
            nextRefs.push_back(ref);
        }
        if (removedRef) {
            otherProfile[QString(kTranscriptSpeakerFaceRefsKey)] = nextRefs;
            profileIt.value() = otherProfile;
        }
    }
    QJsonObject profile = profiles.value(trimmedSpeakerId).toObject();
    QJsonArray faceRefs = speakerFaceRefs(profile);
    for (const QJsonValue& value : trackAnchors) {
        const QJsonObject anchor = value.toObject();
        const int trackId = anchor.value(QStringLiteral("track_id")).toInt(-1);
        if (trackId < 0) {
            continue;
        }
        bool exists = false;
        for (const QJsonValue& faceRefValue : faceRefs) {
            if (faceRefMatches(faceRefValue.toObject(), anchor)) {
                exists = true;
                break;
            }
        }
        if (exists) {
            continue;
        }
        QJsonObject faceRef;
        faceRef[QStringLiteral("track_id")] = trackId;
        faceRef[QStringLiteral("stream_id")] = anchor.value(QStringLiteral("stream_id")).toString().trimmed();
        faceRef[QString(kSpeakerFlowAnchorSourceFrameKey)] =
            static_cast<qint64>(qMax<int64_t>(
                0,
                anchor.value(QStringLiteral("source_frame")).toVariant().toLongLong()));
        faceRef[QString(kSpeakerFlowAnchorXKey)] =
            qBound<qreal>(0.0, anchor.value(QStringLiteral("x")).toDouble(0.5), 1.0);
        faceRef[QString(kSpeakerFlowAnchorYKey)] =
            qBound<qreal>(0.0, anchor.value(QStringLiteral("y")).toDouble(0.5), 1.0);
        faceRef[QString(kSpeakerFlowAnchorBoxSizeKey)] =
            qBound<qreal>(0.01, anchor.value(QStringLiteral("box")).toDouble(0.2), 1.0);
        faceRef[QStringLiteral("source")] = effectiveResolutionSource;
        faceRefs.push_back(faceRef);
    }
    profile[QString(kTranscriptSpeakerFaceRefsKey)] = faceRefs;
    profiles[trimmedSpeakerId] = profile;
    transcriptRoot[QString(kTranscriptSpeakerProfilesKey)] = profiles;

    clipsRoot[trimmedClipId] = clipRoot;
    speakerFlow[QStringLiteral("clips")] = clipsRoot;
    transcriptRoot[QStringLiteral("speaker_flow")] = speakerFlow;
    if (!updateLoadedTranscriptDocument([&](QJsonObject& root) {
            root = transcriptRoot;
            return true;
        }, false) || !saveLoadedTranscriptDocument()) {
        refresh();
        return false;
    }
    m_avatarHoverTooltipHtmlCache.clear();

    QJsonObject identityRoot;
    engine.loadIdentityArtifact(m_transcriptSession.transcriptPath(), &identityRoot);
    QJsonObject assignmentsByClip =
        identityRoot.value(QStringLiteral("identity_assignments_by_clip")).toObject();
    QJsonObject assignmentRoot = assignmentsByClip.value(trimmedClipId).toObject();
    assignmentRoot[QStringLiteral("updated_at_utc")] = timestamp;
    assignmentRoot[QStringLiteral("track_identity_map")] = nextResolvedMap;
    assignmentsByClip[trimmedClipId] = assignmentRoot;
    identityRoot[QStringLiteral("schema")] = QStringLiteral("jcut_identity_v1");
    identityRoot[QStringLiteral("updated_at_utc")] = timestamp;
    identityRoot[QStringLiteral("identity_assignments_by_clip")] = assignmentsByClip;
    engine.saveIdentityArtifact(m_transcriptSession.transcriptPath(), identityRoot);

    m_trackIdentityResolutionCache.clear();
    m_speakersTableRefreshSignature.clear();
    m_faceStreamPanelRefreshSignature.clear();
    emit transcriptDocumentChanged();
    if (m_deps.scheduleSaveState) {
        m_deps.scheduleSaveState();
    }
    if (m_deps.pushHistorySnapshot && !m_assignmentHistorySnapshotQueued) {
        m_assignmentHistorySnapshotQueued = true;
        QTimer::singleShot(350, this, [this]() {
            m_assignmentHistorySnapshotQueued = false;
            if (m_deps.pushHistorySnapshot) {
                m_deps.pushHistorySnapshot();
            }
        });
    }
    if (m_widgets.speakerFaceDetectionsTable) {
        const QString assignmentLabel = speakerDisplayLabel(trimmedSpeakerId);
        for (int row = 0; row < m_widgets.speakerFaceDetectionsTable->rowCount(); ++row) {
            QTableWidgetItem* trackItem = m_widgets.speakerFaceDetectionsTable->item(row, 1);
            QTableWidgetItem* assignmentItem = m_widgets.speakerFaceDetectionsTable->item(row, 2);
            bool trackIdOk = false;
            const int rowTrackId = trackItem ? trackItem->text().trimmed().toInt(&trackIdOk) : -1;
            if (!trackIdOk || !assignmentItem || !anchorByTrackId.contains(rowTrackId)) {
                continue;
            }
            assignmentItem->setText(assignmentLabel);
            assignmentItem->setToolTip(
                QStringLiteral("Assigned to speaker identity: %1").arg(trimmedSpeakerId));
        }
    }
    updateSpeakerTrackingStatusLabelFast();
    scheduleSelectedSpeakerPanelRefresh();
    return true;
}

namespace {

QString trackIdentityResolutionSignature(const QString& transcriptPath,
                                         const TimelineClip& clip,
                                         const QJsonArray& resolvedMap,
                                         const QJsonArray& streams)
{
    const QFileInfo transcriptInfo(transcriptPath);
    QString streamSummary;
    streamSummary.reserve(streams.size() * 16);
    for (const QJsonValue& streamValue : streams) {
        const QJsonObject streamObj = streamValue.toObject();
        streamSummary += QString::number(streamObj.value(QStringLiteral("track_id")).toInt(-1));
        streamSummary += QLatin1Char(':');
        streamSummary += QString::number(streamObj.value(QStringLiteral("keyframes")).toArray().size());
        streamSummary += QLatin1Char(';');
    }
    return clip.id + QLatin1Char('|') +
           transcriptPath + QLatin1Char('|') +
           QString::number(transcriptInfo.exists() ? transcriptInfo.lastModified().toMSecsSinceEpoch() : 0) +
           QLatin1Char('|') +
           QString::number(facedetectionsArtifactRevisionMsForTranscript(transcriptPath)) +
           QLatin1Char('|') +
           QString::fromUtf8(QJsonDocument(resolvedMap).toJson(QJsonDocument::Compact)) +
           QLatin1Char('|') +
           streamSummary;
}

QVector<CachedFacestreamTrack> buildCachedFacestreamTracks(const TimelineClip& clip,
                                                           const QJsonArray& streams,
                                                           const QVector<RenderSyncMarker>& renderSyncMarkers)
{
    QVector<CachedFacestreamTrack> tracks;
    tracks.reserve(streams.size());
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

        CachedFacestreamTrack track;
        track.trackId = trackId;
        track.streamId = streamObj.value(QStringLiteral("stream_id")).toString().trimmed();
        track.keyframes.reserve(keyframes.size());
        for (const QJsonValue& keyframeValue : keyframes) {
            const QJsonObject keyframeObj = keyframeValue.toObject();
            CachedFacestreamKeyframe keyframe;
            const int64_t frame =
                keyframeObj.value(QString(kTranscriptSpeakerTrackingFrameKey)).toVariant().toLongLong();
            keyframe.sourceFrame =
                mapFacestreamFrameToSourceFrame(clip, frame, frameDomain, renderSyncMarkers);
            keyframe.x = qBound<qreal>(
                0.0,
                keyframeObj.value(QString(kTranscriptSpeakerLocationXKey)).toDouble(0.5),
                1.0);
            keyframe.y = qBound<qreal>(
                0.0,
                keyframeObj.value(QString(kTranscriptSpeakerLocationYKey)).toDouble(0.5),
                1.0);
            keyframe.box = qBound<qreal>(
                0.01,
                keyframeObj.value(QString(kTranscriptSpeakerTrackingBoxSizeKey)).toDouble(0.2),
                1.0);
            track.keyframes.push_back(keyframe);
        }
        if (!track.keyframes.isEmpty()) {
            tracks.push_back(track);
        }
    }
    return tracks;
}

int resolveTrackIdFromCachedTracks(const QVector<CachedFacestreamTrack>& cachedTracks,
                                   const QJsonObject& row)
{
    const QString identityId = row.value(QStringLiteral("identity_id")).toString().trimmed();
    if (identityId.isEmpty() || cachedTracks.isEmpty()) {
        return -1;
    }

    const int storedTrackId = row.value(QStringLiteral("track_id")).toInt(-1);
    const QString storedStreamId = row.value(QStringLiteral("stream_id")).toString().trimmed();
    const bool hasAnchor =
        row.contains(QString(kSpeakerFlowAnchorSourceFrameKey)) &&
        row.contains(QString(kSpeakerFlowAnchorXKey)) &&
        row.contains(QString(kSpeakerFlowAnchorYKey)) &&
        row.contains(QString(kSpeakerFlowAnchorBoxSizeKey));
    if (!hasAnchor) {
        for (const CachedFacestreamTrack& track : cachedTracks) {
            if ((storedTrackId >= 0 && track.trackId == storedTrackId) ||
                (!storedStreamId.isEmpty() && track.streamId == storedStreamId)) {
                return track.trackId;
            }
        }
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
    for (const CachedFacestreamTrack& track : cachedTracks) {
        for (const CachedFacestreamKeyframe& keyframe : track.keyframes) {
            const qreal posDist = std::hypot(keyframe.x - anchorX, keyframe.y - anchorY);
            const qreal boxDist = std::abs(keyframe.box - anchorBox);
            const qreal frameDist =
                static_cast<qreal>(std::llabs(keyframe.sourceFrame - anchorSourceFrame));
            const double score = (frameDist / 12.0) + (posDist * 6.0) + (boxDist * 3.0);
            if (score < bestScore) {
                bestScore = score;
                bestTrackId = track.trackId;
            }
        }
    }
    if (bestTrackId >= 0) {
        return bestTrackId;
    }

    return -1;
}

} // namespace

bool SpeakersTab::assignTrackToSpeaker(const QString& speakerId,
                                       int trackId,
                                       const QString& streamId,
                                       int64_t sourceFrame,
                                       qreal xNorm,
                                       qreal yNorm,
                                       qreal boxSizeNorm,
                                       const QString& resolutionSource)
{
    if (!activeCutMutable() || trackId < 0 || speakerId.trimmed().isEmpty() ||
        !m_transcriptSession.hasObjectDocument()) {
        return false;
    }
    const TimelineClip* clip = m_deps.getSelectedClip ? m_deps.getSelectedClip() : nullptr;
    if (!clip) {
        return false;
    }

    const QJsonObject anchor = makeTrackAssignmentAnchor(
        trackId,
        streamId,
        sourceFrame,
        xNorm,
        yNorm,
        boxSizeNorm);
    return persistTrackAssignments(
        clip->id,
        speakerId,
        QJsonArray{anchor},
        resolutionSource,
        QStringLiteral("speaker_track_picker_identity_set"),
        QStringLiteral("track_picker"),
        [trackId](const QJsonObject& row, const QJsonObject&) {
            return row.value(QStringLiteral("track_id")).toInt(-1) == trackId;
        },
        [trackId](const QJsonObject& faceRef, const QJsonObject&) {
            return faceRef.value(QStringLiteral("track_id")).toInt(-1) == trackId;
        });
}

bool SpeakersTab::deassignTrackFromSpeaker(const QString& speakerId, int trackId)
{
    const QString trimmedSpeakerId = speakerId.trimmed();
    if (!activeCutMutable() || trimmedSpeakerId.isEmpty() || trackId < 0 ||
        !m_transcriptSession.hasObjectDocument()) {
        return false;
    }
    const TimelineClip* clip = m_deps.getSelectedClip ? m_deps.getSelectedClip() : nullptr;
    if (!clip) {
        return false;
    }

    editor::TranscriptEngine engine;
    QJsonObject transcriptRoot = m_transcriptSession.rootObject();
    QJsonObject speakerFlow = transcriptRoot.value(QStringLiteral("speaker_flow")).toObject();
    speakerFlow[QStringLiteral("schema_version")] = QStringLiteral("1.0");
    QJsonObject clipsRoot = speakerFlow.value(QStringLiteral("clips")).toObject();
    QJsonObject clipRoot = clipsRoot.value(clip->id).toObject();
    clipRoot[QStringLiteral("clip_id")] = clip->id;
    clipRoot[QStringLiteral("updated_at_utc")] = QDateTime::currentDateTimeUtc().toString(Qt::ISODate);

    QJsonObject resolvedPayload = clipRoot.value(QStringLiteral("resolved_current")).toObject();
    resolvedPayload[QStringLiteral("updated_at_utc")] = QDateTime::currentDateTimeUtc().toString(Qt::ISODate);
    const QJsonArray resolvedMap = resolvedPayload.value(QStringLiteral("track_identity_map")).toArray();
    const QJsonArray streams = continuityStreamsForClip(*clip);
    QJsonArray nextResolvedMap;
    QString removedStreamId;
    qint64 removedSourceFrame = -1;
    qreal removedX = 0.5;
    qreal removedY = 0.5;
    qreal removedBoxSize = 0.2;
    bool removed = false;
    for (const QJsonValue& value : resolvedMap) {
        const QJsonObject row = value.toObject();
        const QJsonObject resolved = resolveFaceDetectionsAssignmentRow(*clip, streams, row);
        const auto resolvedOrRowValue = [&](const QString& key) {
            return resolved.contains(key) ? resolved.value(key) : row.value(key);
        };
        const QString resolvedIdentity = resolved.value(QStringLiteral("identity_id")).toString().trimmed();
        const bool resolvedTrackMatches =
            resolved.value(QStringLiteral("track_id")).toInt(-1) == trackId;
        const bool shouldRemove = resolvedIdentity == trimmedSpeakerId && resolvedTrackMatches;
        if (shouldRemove) {
            removed = true;
            removedStreamId = resolved.value(QStringLiteral("stream_id")).toString(
                row.value(QStringLiteral("stream_id")).toString()).trimmed();
            removedSourceFrame =
                resolvedOrRowValue(QString(kSpeakerFlowAnchorSourceFrameKey)).toVariant().toLongLong();
            removedX = qBound<qreal>(
                0.0,
                resolvedOrRowValue(QString(kSpeakerFlowAnchorXKey)).toDouble(0.5),
                1.0);
            removedY = qBound<qreal>(
                0.0,
                resolvedOrRowValue(QString(kSpeakerFlowAnchorYKey)).toDouble(0.5),
                1.0);
            removedBoxSize =
                qBound<qreal>(
                    0.01,
                    resolvedOrRowValue(QString(kSpeakerFlowAnchorBoxSizeKey)).toDouble(0.2),
                    1.0);
            continue;
        }
        nextResolvedMap.push_back(row);
    }
    if (!removed) {
        return false;
    }
    resolvedPayload[QStringLiteral("track_identity_map")] = nextResolvedMap;
    clipRoot[QStringLiteral("resolved_current")] = resolvedPayload;

    QJsonObject humanRuns = clipRoot.value(QStringLiteral("human_runs")).toObject();
    const QString runId = QStringLiteral("track_deassign_%1").arg(
        QDateTime::currentDateTimeUtc().toString(QStringLiteral("yyyyMMddTHHmmsszzz")));
    QJsonObject humanPayload;
    humanPayload[QStringLiteral("run_id")] = runId;
    humanPayload[QStringLiteral("updated_at_utc")] = QDateTime::currentDateTimeUtc().toString(Qt::ISODate);
    QJsonArray auditLog;
    QJsonObject auditRow;
    auditRow[QStringLiteral("timestamp_utc")] = QDateTime::currentDateTimeUtc().toString(Qt::ISODate);
    auditRow[QStringLiteral("action")] = QStringLiteral("speaker_track_picker_identity_cleared");
    auditRow[QStringLiteral("track_id")] = trackId;
    auditRow[QStringLiteral("stream_id")] = removedStreamId;
    auditRow[QStringLiteral("identity_id")] = trimmedSpeakerId;
    auditRow[QString(kSpeakerFlowAnchorSourceFrameKey)] = removedSourceFrame;
    auditRow[QString(kSpeakerFlowAnchorXKey)] = removedX;
    auditRow[QString(kSpeakerFlowAnchorYKey)] = removedY;
    auditRow[QString(kSpeakerFlowAnchorBoxSizeKey)] = removedBoxSize;
    auditLog.push_back(auditRow);
    humanPayload[QStringLiteral("audit_log")] = auditLog;
    humanRuns[runId] = humanPayload;
    clipRoot[QStringLiteral("human_runs")] = humanRuns;
    clipRoot[QStringLiteral("latest_human_run_id")] = runId;

    QJsonObject profiles = transcriptRoot.value(QString(kTranscriptSpeakerProfilesKey)).toObject();
    for (auto it = profiles.begin(); it != profiles.end(); ++it) {
        QJsonObject profile = it.value().toObject();
        const QJsonArray faceRefs = speakerFaceRefs(profile);
        QJsonArray nextFaceRefs;
        bool changed = false;
        for (const QJsonValue& faceRefValue : faceRefs) {
            const QJsonObject faceRef = faceRefValue.toObject();
            if (faceRef.value(QStringLiteral("track_id")).toInt(-1) == trackId) {
                changed = true;
                continue;
            }
            nextFaceRefs.push_back(faceRef);
        }
        if (changed) {
            profile[QString(kTranscriptSpeakerFaceRefsKey)] = nextFaceRefs;
            it.value() = profile;
        }
    }
    transcriptRoot[QString(kTranscriptSpeakerProfilesKey)] = profiles;

    clipsRoot[clip->id] = clipRoot;
    speakerFlow[QStringLiteral("clips")] = clipsRoot;
    transcriptRoot[QStringLiteral("speaker_flow")] = speakerFlow;
    if (!updateLoadedTranscriptDocument([&](QJsonObject& root) {
            root = transcriptRoot;
            return true;
        }) || !saveLoadedTranscriptDocument()) {
        refresh();
        return false;
    }
    m_avatarHoverTooltipHtmlCache.clear();

    QJsonObject identityRoot;
    engine.loadIdentityArtifact(m_transcriptSession.transcriptPath(), &identityRoot);
    QJsonObject assignmentsByClip = identityRoot.value(QStringLiteral("identity_assignments_by_clip")).toObject();
    QJsonObject assignmentRoot = assignmentsByClip.value(clip->id).toObject();
    assignmentRoot[QStringLiteral("updated_at_utc")] = QDateTime::currentDateTimeUtc().toString(Qt::ISODate);
    assignmentRoot[QStringLiteral("track_identity_map")] = nextResolvedMap;
    assignmentsByClip[clip->id] = assignmentRoot;
    identityRoot[QStringLiteral("schema")] = QStringLiteral("jcut_identity_v1");
    identityRoot[QStringLiteral("updated_at_utc")] = QDateTime::currentDateTimeUtc().toString(Qt::ISODate);
    identityRoot[QStringLiteral("identity_assignments_by_clip")] = assignmentsByClip;
    engine.saveIdentityArtifact(m_transcriptSession.transcriptPath(), identityRoot);

    m_trackIdentityResolutionCache.clear();
    m_speakersTableRefreshSignature.clear();
    m_faceStreamPanelRefreshSignature.clear();
    emit transcriptDocumentChanged();
    if (m_deps.scheduleSaveState) {
        m_deps.scheduleSaveState();
    }
    if (m_deps.pushHistorySnapshot) {
        m_deps.pushHistorySnapshot();
    }
    refresh();
    return true;
}

bool SpeakersTab::deassignSelectedSpeakerAssignedTracks()
{
    if (!m_widgets.selectedSpeakerFaceDetectionsList) {
        return false;
    }
    const QString speakerId = selectedSpeakerId().trimmed();
    if (speakerId.isEmpty()) {
        return false;
    }
    const QList<QListWidgetItem*> selectedItems = m_widgets.selectedSpeakerFaceDetectionsList->selectedItems();
    if (selectedItems.isEmpty()) {
        return false;
    }

    QVector<int> selectedTrackIds;
    selectedTrackIds.reserve(selectedItems.size());
    for (QListWidgetItem* item : selectedItems) {
        if (!item) {
            continue;
        }
        bool ok = false;
        const int trackId = item->data(Qt::UserRole).toInt(&ok);
        if (ok && trackId >= 0 && !selectedTrackIds.contains(trackId)) {
            selectedTrackIds.push_back(trackId);
        }
    }

    bool changed = false;
    for (int trackId : selectedTrackIds) {
        changed = deassignTrackFromSpeaker(speakerId, trackId) || changed;
    }
    return changed;
}

void SpeakersTab::showSelectedSpeakerAssignedTracksContextMenu(const QPoint& pos)
{
    if (!m_widgets.selectedSpeakerFaceDetectionsList) {
        return;
    }
    if (QListWidgetItem* item = m_widgets.selectedSpeakerFaceDetectionsList->itemAt(pos)) {
        if (!item->isSelected()) {
            m_widgets.selectedSpeakerFaceDetectionsList->setCurrentItem(item);
            item->setSelected(true);
        }
    }
    QMenu menu(m_widgets.selectedSpeakerFaceDetectionsList);
    QAction* deassignAction = menu.addAction(QStringLiteral("Deassign Track"));
    QAction* findMatchesAction = menu.addAction(QStringLiteral("Find Matching Tracks"));
    const bool enabled =
        activeCutMutable() &&
        !selectedSpeakerId().trimmed().isEmpty() &&
        !m_widgets.selectedSpeakerFaceDetectionsList->selectedItems().isEmpty();
    deassignAction->setEnabled(enabled);
    const QList<QListWidgetItem*> selectedItems =
        m_widgets.selectedSpeakerFaceDetectionsList->selectedItems();
    findMatchesAction->setEnabled(activeCutMutable() &&
                                  !selectedSpeakerId().trimmed().isEmpty() &&
                                  selectedItems.size() == 1 &&
                                  selectedItems.first() &&
                                  selectedItems.first()->data(Qt::UserRole).toInt() >= 0);
    QAction* chosen = menu.exec(
        m_widgets.selectedSpeakerFaceDetectionsList->viewport()->mapToGlobal(pos));
    if (chosen == deassignAction) {
        deassignSelectedSpeakerAssignedTracks();
    } else if (chosen == findMatchesAction) {
        const int seedTrackId = selectedItems.isEmpty() || !selectedItems.first()
            ? -1
            : selectedItems.first()->data(Qt::UserRole).toInt();
        findMatchingTracksFromSeedTrack(seedTrackId);
    }
}

bool SpeakersTab::assignTrackAnchorsToSpeakerBatch(const QString& speakerId,
                                                   const QJsonArray& trackAnchors,
                                                   const QString& resolutionSource,
                                                   const QString& auditAction)
{
    const TimelineClip* clip = m_deps.getSelectedClip ? m_deps.getSelectedClip() : nullptr;
    if (!clip) {
        return false;
    }
    return persistTrackAssignments(
        clip->id,
        speakerId,
        trackAnchors,
        resolutionSource,
        auditAction,
        QStringLiteral("track_batch"),
        [](const QJsonObject& row, const QJsonObject& anchor) {
            return row.value(QStringLiteral("track_id")).toInt(-1) ==
                   anchor.value(QStringLiteral("track_id")).toInt(-1);
        },
        [](const QJsonObject& faceRef, const QJsonObject& anchor) {
            return faceRef.value(QStringLiteral("track_id")).toInt(-1) ==
                   anchor.value(QStringLiteral("track_id")).toInt(-1);
        });
}

bool SpeakersTab::handlePreviewFaceDetectionsBox(const QString& clipId,
                                                 int trackId,
                                                 const QString& streamId,
                                                 int64_t sourceFrame,
                                                 qreal xNorm,
                                                 qreal yNorm,
                                                 qreal boxSizeNorm)
{
    auto report = [this](const QString& message) {
        qInfo().noquote() << message;
        showPreviewFaceDetectionsClickStatus(message);
    };

    report(QStringLiteral("Face box click: clip=%1 track=%2 stream=%3 source_frame=%4 x=%5 y=%6 box=%7")
               .arg(clipId)
               .arg(trackId)
               .arg(streamId.isEmpty() ? QStringLiteral("<empty>") : streamId)
               .arg(sourceFrame)
               .arg(xNorm, 0, 'f', 4)
               .arg(yNorm, 0, 'f', 4)
               .arg(boxSizeNorm, 0, 'f', 4));

    if (!activeCutMutable()) {
        report(QStringLiteral("Face box click ignored: the active cut is not editable."));
        return false;
    }
    if (trackId < 0) {
        report(QStringLiteral("Face box click ignored: invalid track id %1.").arg(trackId));
        return false;
    }
    const TimelineClip* clip = m_deps.getSelectedClip ? m_deps.getSelectedClip() : nullptr;
    if (!clip || clip->id != clipId) {
        report(QStringLiteral("Face box click ignored: clicked clip %1 is not the selected clip %2.")
                   .arg(clipId, clip ? clip->id : QStringLiteral("<none>")));
        return false;
    }
    const int64_t mediaSourceFrame = sourceFrame >= 0
        ? sourceFrame
        : sourceFrameForClipAtTimelinePosition(
              *clip,
              static_cast<qreal>(m_deps.getCurrentTimelineFrame ? m_deps.getCurrentTimelineFrame() : clip->startFrame),
              m_speakerDeps.getRenderSyncMarkers
                  ? m_speakerDeps.getRenderSyncMarkers()
                  : QVector<RenderSyncMarker>{});
    const qreal sourceFps = resolvedSourceFps(*clip);
    const int64_t transcriptFrame = sourceFrame >= 0
        ? qMax<int64_t>(
              0,
              static_cast<int64_t>(std::floor(
                  (static_cast<qreal>(mediaSourceFrame) / qMax<qreal>(1.0, sourceFps)) *
                  static_cast<qreal>(kTimelineFps))))
        : currentSourceFrameForClip(*clip);
    QString speakerResolutionDetail =
        QStringLiteral("transcript exact frame %1 from media source frame %2 at %3 fps")
            .arg(transcriptFrame)
            .arg(mediaSourceFrame)
            .arg(sourceFps, 0, 'f', 3);
    QString speakerId = activeSpeakerIdAtSourceFrame(transcriptFrame);
    int64_t speakerSourceFrame = transcriptFrame;
    if (speakerId.isEmpty()) {
        const int gapHoldFrames = qBound(0, clip->speakerFramingGapHoldFrames, 240);
        int64_t resolvedGapFrame = transcriptFrame;
        speakerId = activeSpeakerIdNearSourceFrame(transcriptFrame, gapHoldFrames, &resolvedGapFrame);
        if (!speakerId.isEmpty()) {
            speakerSourceFrame = resolvedGapFrame;
            speakerResolutionDetail =
                QStringLiteral("transcript gap hold: media source frame %1 -> transcript frame %2 -> speaker frame %3 within %4 transcript frame(s)")
                    .arg(mediaSourceFrame)
                    .arg(transcriptFrame)
                    .arg(speakerSourceFrame)
                    .arg(gapHoldFrames);
        }
    }
    if (speakerId.isEmpty() || !m_transcriptSession.hasObjectDocument()) {
        const int gapHoldFrames = qBound(0, clip->speakerFramingGapHoldFrames, 240);
        report(QStringLiteral("Face box click ignored: no transcript-active speaker at media source frame %1 (transcript frame %2 at %3 fps); gap hold=%4 transcript frame(s).")
                   .arg(mediaSourceFrame)
                   .arg(transcriptFrame)
                   .arg(sourceFps, 0, 'f', 3)
                   .arg(gapHoldFrames));
        return false;
    }

    selectSpeakerRowById(speakerId);
    selectSpeakerSectionRowAtFrame(speakerId, speakerSourceFrame);
    m_lastSelectedSpeakerIdHint = speakerId;

    const bool assigned = assignTrackToSpeaker(
        speakerId,
        trackId,
        streamId,
        mediaSourceFrame,
        xNorm,
        yNorm,
        boxSizeNorm,
        QStringLiteral("preview_click"));
    if (assigned && m_speakerDeps.refreshPreview) {
        m_speakerDeps.refreshPreview();
    }
    if (assigned) {
        report(QStringLiteral("Face box click assigned: track %1 added to %2 at media source frame %3 (%4).")
                   .arg(trackId)
                   .arg(speakerDisplayLabel(speakerId))
                   .arg(mediaSourceFrame)
                   .arg(speakerResolutionDetail));
    } else {
        report(QStringLiteral("Face box click failed: track %1 was not assigned to %2.")
                   .arg(trackId)
                   .arg(speakerDisplayLabel(speakerId)));
    }
    return assigned;
}

void SpeakersTab::showPreviewFaceDetectionsClickStatus(const QString& message)
{
    if (m_widgets.speakerTrackingStatusLabel) {
        m_widgets.speakerTrackingStatusLabel->setText(message);
        m_widgets.speakerTrackingStatusLabel->setToolTip(message);
    }
}

void SpeakersTab::refreshPlayheadTrackCandidatesList(const TimelineClip& clip, const QString& speakerId)
{
    QElapsedTimer refreshTimer;
    refreshTimer.start();
    const bool playbackActive = m_speakerDeps.isPlaybackActive && m_speakerDeps.isPlaybackActive();
    auto finalizeRefreshTiming = [this, &refreshTimer]() {
        m_lastPlayheadTrackCandidatesRefreshDurationMs = refreshTimer.elapsed();
        m_maxPlayheadTrackCandidatesRefreshDurationMs =
            qMax(m_maxPlayheadTrackCandidatesRefreshDurationMs,
                 m_lastPlayheadTrackCandidatesRefreshDurationMs);
    };
    if (!m_widgets.speakerPlayheadFaceDetectionsList) {
        finalizeRefreshTiming();
        return;
    }

    m_widgets.speakerPlayheadFaceDetectionsList->clear();
    m_lastPlayheadTrackCandidateCount = 0;
    QJsonArray streams = continuityStreamsForClip(clip);
    if (streams.isEmpty()) {
        editor::TranscriptEngine transcriptEngine;
        QJsonObject artifactRoot;
        if (transcriptEngine.loadFacestreamArtifact(m_transcriptSession.transcriptPath(), &artifactRoot) ||
            transcriptEngine.loadFacestreamProcessedArtifact(m_transcriptSession.transcriptPath(), &artifactRoot)) {
            const QJsonObject continuityRoot = continuityRootForClip(artifactRoot, clip.id);
            const QVector<RenderSyncMarker> renderSyncMarkers =
                m_speakerDeps.getRenderSyncMarkers ? m_speakerDeps.getRenderSyncMarkers() : QVector<RenderSyncMarker>{};
            const int64_t playheadTimelineFrame =
                m_deps.getCurrentTimelineFrame ? m_deps.getCurrentTimelineFrame() : clip.startFrame;
            const int64_t playheadSourceFrame =
                sourceFrameForClipAtTimelinePosition(clip, static_cast<qreal>(playheadTimelineFrame), renderSyncMarkers);
            streams = jcut::facedetections::continuityStreamsNearFrame(
                continuityRoot,
                playheadSourceFrame,
                0,
                m_transcriptSession.rootObject());
        }
    }
    if (streams.isEmpty()) {
        auto* item = new QListWidgetItem(QStringLiteral("No Continuity Tracks"));
        item->setFlags(item->flags() & ~Qt::ItemIsSelectable);
        item->setSizeHint(QSize(140, 40));
        m_widgets.speakerPlayheadFaceDetectionsList->addItem(item);
        finalizeRefreshTiming();
        return;
    }

    const QVector<RenderSyncMarker> renderSyncMarkers =
        m_speakerDeps.getRenderSyncMarkers ? m_speakerDeps.getRenderSyncMarkers() : QVector<RenderSyncMarker>{};
    const int64_t playheadTimelineFrame =
        m_deps.getCurrentTimelineFrame ? m_deps.getCurrentTimelineFrame() : clip.startFrame;
    const int64_t playheadSourceFrame =
        sourceFrameForClipAtTimelinePosition(clip, static_cast<qreal>(playheadTimelineFrame), renderSyncMarkers);
    const QHash<int, QString> assignedIdentityByTrackId = resolvedIdentityByTrackId(clip, streams);
    std::unique_ptr<editor::DecoderContext> decoder;
    if (!playbackActive) {
        const QString mediaPath = interactivePreviewMediaPathForClip(clip);
        if (!mediaPath.isEmpty()) {
            decoder = std::make_unique<editor::DecoderContext>(mediaPath);
            if (!decoder->initialize()) {
                decoder.reset();
            }
        }
    }
    QHash<int64_t, QImage> frameImageCache;
    for (const QJsonValue& value : streams) {
        const QJsonObject streamObj = value.toObject();
        const int trackId = streamObj.value(QStringLiteral("track_id")).toInt(-1);
        if (trackId < 0) {
            continue;
        }
        FacestreamResolvedSelection selection;
        if (!resolvePlayheadTrackSelection(clip,
                                           streamObj,
                                           renderSyncMarkers,
                                           playheadTimelineFrame,
                                           playheadSourceFrame,
                                           &selection)) {
            continue;
        }

        const QString streamId = streamObj.value(QStringLiteral("stream_id")).toString(QStringLiteral("T%1").arg(trackId));
        const QString assignedSpeakerId = assignedIdentityByTrackId.value(trackId).trimmed();
        const QString assignedLabel =
            assignedSpeakerId.isEmpty() ? QStringLiteral("Unassigned") : speakerDisplayLabel(assignedSpeakerId);
        const QJsonObject previewKeyframe{
            {QString(kTranscriptSpeakerTrackingFrameKey),
             static_cast<qint64>(qMax<int64_t>(0, selection.sourceFrame))},
            {QString(kTranscriptSpeakerLocationXKey), selection.keyframe.xNorm},
            {QString(kTranscriptSpeakerLocationYKey), selection.keyframe.yNorm},
            {QString(kTranscriptSpeakerTrackingBoxSizeKey), selection.keyframe.boxSizeNorm},
            {QStringLiteral("confidence"), selection.keyframe.confidence},
            {QStringLiteral("source"), selection.keyframe.source},
        };
        QListWidgetItem* item = playbackActive
            ? new QListWidgetItem(QStringLiteral("%1\n%2").arg(streamId, assignedLabel))
            : new QListWidgetItem(
                  QIcon(faceStreamPreviewAvatarWithDecoder(
                      clip,
                      speakerId,
                      previewKeyframe,
                      72,
                      decoder.get(),
                      &frameImageCache)),
                  QStringLiteral("%1\n%2").arg(streamId, assignedLabel));
        item->setData(Qt::UserRole, trackId);
        item->setData(Qt::UserRole + 1, streamId);
        item->setData(Qt::UserRole + 2, QVariant::fromValue<qlonglong>(selection.sourceFrame));
        item->setData(
            Qt::UserRole + 3,
            qBound<qreal>(
                0.0,
                selection.keyframe.xNorm,
                1.0));
        item->setData(
            Qt::UserRole + 4,
            qBound<qreal>(
                0.0,
                selection.keyframe.yNorm,
                1.0));
        item->setData(
            Qt::UserRole + 5,
            qBound<qreal>(
                0.01,
                selection.keyframe.boxSizeNorm,
                1.0));
        item->setToolTip(
            QStringLiteral("Track %1 | Frame %2 | Source %3 | Current assignment: %4")
                .arg(trackId)
                .arg(selection.sourceFrame)
                .arg(selection.keyframe.source.isEmpty() ? QStringLiteral("-") : selection.keyframe.source)
                .arg(assignedLabel));
        item->setSizeHint(QSize(100, 96));
        if (assignedSpeakerId == speakerId) {
            item->setSelected(true);
        }
        m_widgets.speakerPlayheadFaceDetectionsList->addItem(item);
        ++m_lastPlayheadTrackCandidateCount;
    }

    if (m_widgets.speakerPlayheadFaceDetectionsList->count() == 0) {
        auto* item = new QListWidgetItem(QStringLiteral("No Tracks At Playhead"));
        item->setFlags(item->flags() & ~Qt::ItemIsSelectable);
        item->setSizeHint(QSize(150, 40));
        m_widgets.speakerPlayheadFaceDetectionsList->addItem(item);
    }
    finalizeRefreshTiming();
}

void SpeakersTab::openTrackPickerForSpeaker(const QString& speakerId)
{
    const QString trimmedSpeakerId = speakerId.trimmed();
    const TimelineClip* clip = m_deps.getSelectedClip ? m_deps.getSelectedClip() : nullptr;
    if (trimmedSpeakerId.isEmpty() || !clip || !m_transcriptSession.hasObjectDocument()) {
        return;
    }
    const QJsonArray streams = continuityStreamsForClip(*clip);
    if (streams.isEmpty()) {
        QMessageBox::information(nullptr,
                                 QStringLiteral("Add Tracks"),
                                 QStringLiteral("No continuity tracks are available for this clip."));
        return;
    }

    QDialog dialog;
    dialog.setWindowTitle(QStringLiteral("Add Tracks to %1").arg(speakerDisplayLabel(trimmedSpeakerId)));
    dialog.resize(960, 640);
    auto* layout = new QVBoxLayout(&dialog);
    auto* help = new QLabel(
        QStringLiteral("Select one or more continuity tracks. Each thumbnail uses a representative frame halfway through that track."),
        &dialog);
    help->setWordWrap(true);
    auto* list = new QListWidget(&dialog);
    list->setViewMode(QListView::IconMode);
    list->setFlow(QListView::LeftToRight);
    list->setWrapping(true);
    list->setResizeMode(QListView::Adjust);
    list->setMovement(QListView::Static);
    list->setSelectionMode(QAbstractItemView::ExtendedSelection);
    list->setIconSize(QSize(96, 96));
    list->setSpacing(10);

    const QString mediaPath = interactivePreviewMediaPathForClip(*clip);
    std::unique_ptr<editor::DecoderContext> decoder;
    if (!mediaPath.isEmpty()) {
        decoder = std::make_unique<editor::DecoderContext>(mediaPath);
        if (!decoder->initialize()) {
            decoder.reset();
        }
    }
    QHash<int64_t, QImage> frameImageCache;
    const QHash<int, QString> identityByTrackId = resolvedIdentityByTrackId(*clip, streams);
    for (const QJsonValue& streamValue : streams) {
        const QJsonObject streamObj = streamValue.toObject();
        const int trackId = streamObj.value(QStringLiteral("track_id")).toInt(-1);
        const QString streamId = streamObj.value(QStringLiteral("stream_id")).toString(
            QStringLiteral("T%1").arg(trackId));
        if (trackId < 0) {
            continue;
        }
        const QJsonObject representativeKeyframe = representativeKeyframeForTrack(*clip, streamObj);
        if (representativeKeyframe.isEmpty()) {
            continue;
        }
        const QString assignedSpeakerId = identityByTrackId.value(trackId).trimmed();
        const QString assignedLabel =
            assignedSpeakerId.isEmpty() ? QStringLiteral("Unassigned") : speakerDisplayLabel(assignedSpeakerId);
        auto* item = new QListWidgetItem(
            QIcon(continuityTrackAvatar(
                *clip,
                trimmedSpeakerId,
                streamObj,
                96,
                decoder.get(),
                &frameImageCache)),
            QStringLiteral("%1\n%2").arg(streamId, assignedLabel));
        item->setData(Qt::UserRole, trackId);
        item->setData(Qt::UserRole + 1, streamId);
        item->setData(
            Qt::UserRole + 2,
            QString::fromUtf8(QJsonDocument(representativeKeyframe).toJson(QJsonDocument::Compact)));
        item->setToolTip(
            QStringLiteral("Track %1\nCurrent assignment: %2").arg(trackId).arg(assignedLabel));
        item->setSizeHint(QSize(120, 130));
        if (assignedSpeakerId == trimmedSpeakerId) {
            item->setSelected(true);
        }
        list->addItem(item);
    }

    auto* buttonsRow = new QHBoxLayout;
    auto* addButton = new QPushButton(QStringLiteral("Assign Selected"), &dialog);
    auto* cancelButton = new QPushButton(QStringLiteral("Cancel"), &dialog);
    buttonsRow->addStretch(1);
    buttonsRow->addWidget(addButton);
    buttonsRow->addWidget(cancelButton);
    layout->addWidget(help);
    layout->addWidget(list, 1);
    layout->addLayout(buttonsRow);
    connect(cancelButton, &QPushButton::clicked, &dialog, &QDialog::reject);
    connect(addButton, &QPushButton::clicked, &dialog, [&dialog]() { dialog.accept(); });

    if (dialog.exec() != QDialog::Accepted) {
        return;
    }

    bool changed = false;
    for (QListWidgetItem* item : list->selectedItems()) {
        const int trackId = item->data(Qt::UserRole).toInt();
        const QString streamId = item->data(Qt::UserRole + 1).toString().trimmed();
        const QJsonObject keyframe =
            QJsonDocument::fromJson(item->data(Qt::UserRole + 2).toString().toUtf8()).object();
        if (trackId < 0 || keyframe.isEmpty()) {
            continue;
        }
        const int64_t sourceFrame =
            qMax<int64_t>(0, keyframe.value(QString(kTranscriptSpeakerTrackingFrameKey)).toVariant().toLongLong());
        const qreal xNorm =
            qBound<qreal>(0.0, keyframe.value(QString(kTranscriptSpeakerLocationXKey)).toDouble(0.5), 1.0);
        const qreal yNorm =
            qBound<qreal>(0.0, keyframe.value(QString(kTranscriptSpeakerLocationYKey)).toDouble(0.5), 1.0);
        const qreal boxNorm =
            qBound<qreal>(0.01, keyframe.value(QString(kTranscriptSpeakerTrackingBoxSizeKey)).toDouble(0.2), 1.0);
        changed = assignTrackToSpeaker(
                      trimmedSpeakerId,
                      trackId,
                      streamId.isEmpty() ? QStringLiteral("T%1").arg(trackId) : streamId,
                      sourceFrame,
                      xNorm,
                      yNorm,
                      boxNorm,
                      QStringLiteral("speaker_track_picker")) || changed;
    }
    if (!changed) {
        updateSpeakerTrackingStatusLabel();
        updateSelectedSpeakerPanel();
    }
}

bool SpeakersTab::openPlayheadTrackPickerForSpeaker(const QString& speakerId)
{
    const QString trimmedSpeakerId = speakerId.trimmed();
    if (trimmedSpeakerId.isEmpty() || !m_widgets.speakerPlayheadFaceDetectionsList) {
        return false;
    }

    QVector<QListWidgetItem*> sourceItems;
    sourceItems.reserve(m_widgets.speakerPlayheadFaceDetectionsList->count());
    for (int i = 0; i < m_widgets.speakerPlayheadFaceDetectionsList->count(); ++i) {
        QListWidgetItem* item = m_widgets.speakerPlayheadFaceDetectionsList->item(i);
        bool ok = false;
        const int trackId = item ? item->data(Qt::UserRole).toInt(&ok) : -1;
        if (!item || !ok || trackId < 0) {
            continue;
        }
        sourceItems.push_back(item);
    }
    if (sourceItems.isEmpty()) {
        return false;
    }

    QDialog dialog;
    dialog.setWindowTitle(QStringLiteral("Add Playhead Tracks to %1").arg(speakerDisplayLabel(trimmedSpeakerId)));
    dialog.resize(720, 420);
    auto* layout = new QVBoxLayout(&dialog);
    auto* help = new QLabel(
        QStringLiteral("These are the continuity tracks active at the current playhead. This path assigns existing playhead anchors directly and avoids the slower all-track thumbnail scan."),
        &dialog);
    help->setWordWrap(true);
    layout->addWidget(help);

    auto* list = new QListWidget(&dialog);
    list->setViewMode(QListView::IconMode);
    list->setFlow(QListView::LeftToRight);
    list->setWrapping(true);
    list->setResizeMode(QListView::Adjust);
    list->setMovement(QListView::Static);
    list->setSelectionMode(QAbstractItemView::ExtendedSelection);
    list->setIconSize(QSize(96, 96));
    list->setSpacing(10);
    for (QListWidgetItem* sourceItem : std::as_const(sourceItems)) {
        auto* copy = new QListWidgetItem(sourceItem->icon(), sourceItem->text());
        for (int role = Qt::UserRole; role <= Qt::UserRole + 5; ++role) {
            copy->setData(role, sourceItem->data(role));
        }
        copy->setToolTip(sourceItem->toolTip());
        copy->setSizeHint(QSize(120, 120));
        copy->setSelected(sourceItem->isSelected());
        list->addItem(copy);
    }
    layout->addWidget(list, 1);

    auto* buttonsRow = new QHBoxLayout;
    auto* showAllButton = new QPushButton(QStringLiteral("Show All Tracks..."), &dialog);
    auto* assignButton = new QPushButton(QStringLiteral("Assign Selected"), &dialog);
    auto* cancelButton = new QPushButton(QStringLiteral("Cancel"), &dialog);
    buttonsRow->addWidget(showAllButton);
    buttonsRow->addStretch(1);
    buttonsRow->addWidget(assignButton);
    buttonsRow->addWidget(cancelButton);
    layout->addLayout(buttonsRow);
    bool showAllTracks = false;
    connect(showAllButton, &QPushButton::clicked, &dialog, [&]() {
        showAllTracks = true;
        dialog.reject();
    });
    connect(cancelButton, &QPushButton::clicked, &dialog, &QDialog::reject);
    connect(assignButton, &QPushButton::clicked, &dialog, &QDialog::accept);

    if (dialog.exec() != QDialog::Accepted) {
        if (showAllTracks) {
            openTrackPickerForSpeaker(trimmedSpeakerId);
            return true;
        }
        return true;
    }

    QJsonArray anchors;
    for (QListWidgetItem* item : list->selectedItems()) {
        if (!item) {
            continue;
        }
        bool ok = false;
        const int trackId = item->data(Qt::UserRole).toInt(&ok);
        if (!ok || trackId < 0) {
            continue;
        }
        anchors.push_back(makeTrackAssignmentAnchor(
            trackId,
            item->data(Qt::UserRole + 1).toString().trimmed(),
            item->data(Qt::UserRole + 2).toLongLong(),
            item->data(Qt::UserRole + 3).toDouble(),
            item->data(Qt::UserRole + 4).toDouble(),
            item->data(Qt::UserRole + 5).toDouble()));
    }
    if (anchors.isEmpty()) {
        QMessageBox::information(
            nullptr,
            QStringLiteral("Add Tracks"),
            QStringLiteral("Select one or more playhead tracks first."));
        return true;
    }
    assignTrackAnchorsToSpeakerBatch(
        trimmedSpeakerId,
        anchors,
        QStringLiteral("speaker_playhead_picker"),
        QStringLiteral("speaker_playhead_picker_identity_set"));
    return true;
}

void SpeakersTab::onSpeakerRefreshTrackAvatarsClicked()
{
    const TimelineClip* clip = m_deps.getSelectedClip ? m_deps.getSelectedClip() : nullptr;
    if (!clip || !m_transcriptSession.hasObjectDocument()) {
        QMessageBox::information(
            nullptr,
            QStringLiteral("Refresh Track Avatars"),
            QStringLiteral("Select a clip with continuity tracks first."));
        return;
    }

    const QJsonArray streams = continuityStreamsForClip(*clip);
    if (streams.isEmpty()) {
        QMessageBox::information(
            nullptr,
            QStringLiteral("Refresh Track Avatars"),
            QStringLiteral("No continuity tracks are available for the selected clip."));
        return;
    }

    std::unique_ptr<editor::DecoderContext> decoder;
    const QString mediaPath = interactivePreviewMediaPathForClip(*clip);
    if (!mediaPath.isEmpty()) {
        decoder = std::make_unique<editor::DecoderContext>(mediaPath);
        if (!decoder->initialize()) {
            decoder.reset();
        }
    }
    if (!decoder) {
        QMessageBox::warning(
            nullptr,
            QStringLiteral("Refresh Track Avatars"),
            QStringLiteral("Unable to decode media for the selected clip, so track avatars could not be regenerated."));
        return;
    }

    QHash<int64_t, QImage> frameImageCache;
    ensurePersistentTrackAvatarMemory(
        *clip,
        streams,
        true,
        decoder.get(),
        &frameImageCache);
    m_avatarCache.clear();
    m_avatarHoverTooltipHtmlCache.clear();
    refreshSpeakersTable(m_transcriptSession.rootObject(), selectedSpeakerId());
    updateSelectedSpeakerPanel();
    updateSpeakerTrackingStatusLabel();
    QMessageBox::information(
        nullptr,
        QStringLiteral("Refresh Track Avatars"),
        QStringLiteral("Track avatars were regenerated for the selected clip."));
}
