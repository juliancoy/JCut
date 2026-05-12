#include "facestream_runtime.h"

#include "decoder_context.h"
#include "frame_handle.h"
#include "render_internal.h"
#include "speakers_tab_internal.h"
#include "transcript_engine.h"

#include <QApplication>
#include <QDataStream>
#include <QDateTime>
#include <QFile>
#include <QJsonDocument>
#include <QPainter>
#include <QPixmap>

namespace jcut::facestream {

VulkanFrameProvider::~VulkanFrameProvider()
{
    qDeleteAll(decoders);
    decoders.clear();
}

bool VulkanFrameProvider::ensureInitialized(const QSize& size)
{
    const QSize normalized(qMax(16, size.width()), qMax(16, size.height()));
    if (initialized && outputSize == normalized) {
        return true;
    }
    renderer = std::make_unique<render_detail::OffscreenVulkanRenderer>();
    qDeleteAll(decoders);
    decoders.clear();
    asyncFrameCache.clear();
    outputSize = normalized;
    QString error;
    if (!renderer->initialize(outputSize, &error)) {
        renderer.reset();
        initialized = false;
        failed = true;
        failureReason = error.isEmpty()
            ? QStringLiteral("Vulkan FaceStream renderer initialization failed.")
            : error;
        return false;
    }
    initialized = true;
    failed = false;
    failureReason.clear();
    return true;
}

TimelineClip buildFacestreamRenderClip(const TimelineClip& sourceClip,
                                      const QString& mediaPath,
                                      int64_t timelineFrame,
                                      int64_t sourceFrame)
{
    TimelineClip clip = sourceClip;
    clip.id = sourceClip.id.trimmed().isEmpty()
        ? QStringLiteral("facestream-vulkan-source")
        : sourceClip.id;
    clip.filePath = mediaPath;
    clip.proxyPath.clear();
    clip.useProxy = false;
    clip.mediaType = ClipMediaType::Video;
    clip.videoEnabled = true;
    clip.audioEnabled = false;
    clip.hasAudio = false;
    clip.startFrame = timelineFrame;
    clip.startSubframeSamples = 0;
    clip.sourceInFrame = qMax<int64_t>(0, sourceFrame);
    clip.sourceInSubframeSamples = 0;
    clip.durationFrames = 1;
    clip.sourceDurationFrames = qMax<int64_t>(clip.sourceInFrame + 1, sourceClip.sourceDurationFrames);
    clip.playbackRate = 1.0;
    clip.trackIndex = 0;
    clip.brightness = 0.0;
    clip.contrast = 1.0;
    clip.saturation = 1.0;
    clip.opacity = 1.0;
    clip.baseTranslationX = 0.0;
    clip.baseTranslationY = 0.0;
    clip.baseRotation = 0.0;
    clip.baseScaleX = 1.0;
    clip.baseScaleY = 1.0;
    clip.speakerFramingEnabled = false;
    clip.transformKeyframes.clear();
    clip.speakerFramingKeyframes.clear();
    clip.gradingKeyframes.clear();
    clip.opacityKeyframes.clear();
    clip.titleKeyframes.clear();
    clip.transcriptOverlay.enabled = false;
    clip.correctionPolygons.clear();
    return clip;
}

RenderRequest buildFacestreamRenderRequest(const TimelineClip& clip,
                                          int64_t timelineFrame,
                                          const QSize& outputSize)
{
    RenderRequest request;
    request.outputPath = QStringLiteral("facestream://vulkan");
    request.outputFormat = QStringLiteral("facestream-preview");
    request.outputSize = outputSize;
    request.bypassGrading = true;
    request.correctionsEnabled = false;
    request.clips = QVector<TimelineClip>{clip};
    request.tracks = QVector<TimelineTrack>{TimelineTrack{}};
    request.exportStartFrame = timelineFrame;
    request.exportEndFrame = timelineFrame;
    return request;
}

QImage renderFrameWithVulkan(VulkanFrameProvider* provider,
                             const TimelineClip& sourceClip,
                             const QString& mediaPath,
                             int64_t timelineFrame,
                             int64_t sourceFrame,
                             const QSize& outputSize,
                             VulkanFrameStats* stats)
{
    if (!provider || !provider->ensureInitialized(outputSize)) {
        return {};
    }

    const TimelineClip clip = buildFacestreamRenderClip(sourceClip, mediaPath, timelineFrame, sourceFrame);
    const RenderRequest request = buildFacestreamRenderRequest(clip, timelineFrame, outputSize);

    qint64 decodeMs = 0;
    qint64 textureMs = 0;
    qint64 compositeMs = 0;
    qint64 readbackMs = 0;
    QImage frame = provider->renderer->renderFrame(request,
                                                   timelineFrame,
                                                   provider->decoders,
                                                   nullptr,
                                                   &provider->asyncFrameCache,
                                                   QVector<TimelineClip>{clip},
                                                   nullptr,
                                                   &decodeMs,
                                                   &textureMs,
                                                   &compositeMs,
                                                   &readbackMs,
                                                   nullptr,
                                                   nullptr);
    if (stats) {
        stats->decodeMs = decodeMs;
        stats->textureMs = textureMs;
        stats->compositeMs = compositeMs;
        stats->readbackMs = readbackMs;
    }
    if (frame.isNull()) {
        provider->failed = true;
        provider->failureReason = QStringLiteral("Vulkan FaceStream frame render returned null.");
    }
    return frame;
}

bool renderFrameToVulkan(VulkanFrameProvider* provider,
                         const TimelineClip& sourceClip,
                         const QString& mediaPath,
                         int64_t timelineFrame,
                         int64_t sourceFrame,
                         const QSize& outputSize,
                         render_detail::OffscreenVulkanFrame* frame,
                         VulkanFrameStats* stats,
                         QString* errorMessage)
{
    if (!frame) {
        if (errorMessage) {
            *errorMessage = QStringLiteral("Missing Vulkan frame output.");
        }
        return false;
    }
    frame->valid = false;
    if (!provider || !provider->ensureInitialized(outputSize)) {
        if (errorMessage) {
            *errorMessage = provider ? provider->failureReason
                                     : QStringLiteral("Missing Vulkan frame provider.");
        }
        return false;
    }

    const TimelineClip clip = buildFacestreamRenderClip(sourceClip, mediaPath, timelineFrame, sourceFrame);
    const RenderRequest request = buildFacestreamRenderRequest(clip, timelineFrame, outputSize);
    qint64 decodeMs = 0;
    qint64 textureMs = 0;
    qint64 compositeMs = 0;
    provider->renderer->renderFrame(request,
                                    timelineFrame,
                                    provider->decoders,
                                    nullptr,
                                    &provider->asyncFrameCache,
                                    QVector<TimelineClip>{clip},
                                    nullptr,
                                    &decodeMs,
                                    &textureMs,
                                    &compositeMs,
                                    nullptr,
                                    nullptr,
                                    nullptr);
    if (stats) {
        stats->decodeMs = decodeMs;
        stats->textureMs = textureMs;
        stats->compositeMs = compositeMs;
        stats->readbackMs = 0;
    }
    QString error;
    if (!provider->renderer->lastRenderedVulkanFrame(frame, &error)) {
        provider->failed = true;
        provider->failureReason = error.isEmpty()
            ? QStringLiteral("Vulkan FaceStream frame render returned no GPU image.")
            : error;
        if (errorMessage) {
            *errorMessage = provider->failureReason;
        }
        return false;
    }
    provider->failed = false;
    provider->failureReason.clear();
    return true;
}

QImage readLastRenderedVulkanFrameImage(VulkanFrameProvider* provider,
                                        VulkanFrameStats* stats,
                                        QString* errorMessage)
{
    if (!provider || !provider->renderer) {
        if (errorMessage) {
            *errorMessage = QStringLiteral("Missing Vulkan frame provider/renderer.");
        }
        return {};
    }

    AVFrame* bgra = av_frame_alloc();
    if (!bgra) {
        if (errorMessage) {
            *errorMessage = QStringLiteral("Failed to allocate BGRA preview frame.");
        }
        return {};
    }
    bgra->format = AV_PIX_FMT_BGRA;
    bgra->width = qMax(1, provider->outputSize.width());
    bgra->height = qMax(1, provider->outputSize.height());
    if (av_frame_get_buffer(bgra, 32) < 0) {
        av_frame_free(&bgra);
        if (errorMessage) {
            *errorMessage = QStringLiteral("Failed to allocate BGRA preview buffer.");
        }
        return {};
    }

    qint64 readbackMs = 0;
    if (!provider->renderer->copyLastFrameToBgra(bgra, &readbackMs)) {
        av_frame_free(&bgra);
        provider->failed = true;
        provider->failureReason = QStringLiteral("Failed to read back last rendered Vulkan frame.");
        if (errorMessage) {
            *errorMessage = provider->failureReason;
        }
        return {};
    }

    QImage wrapped(reinterpret_cast<const uchar*>(bgra->data[0]),
                   bgra->width,
                   bgra->height,
                   bgra->linesize[0],
                   QImage::Format_ARGB32);
    QImage out = wrapped.copy().mirrored().convertToFormat(QImage::Format_ARGB32_Premultiplied);
    av_frame_free(&bgra);

    if (stats) {
        stats->readbackMs += readbackMs;
    }
    provider->failed = false;
    provider->failureReason.clear();
    return out;
}

QImage buildScanPreview(const QImage& source, const QVector<QRect>& detections, int activeTracks)
{
    if (source.isNull()) {
        return QImage();
    }
    QImage preview = source.convertToFormat(QImage::Format_ARGB32_Premultiplied);
    QPainter painter(&preview);
    painter.setRenderHint(QPainter::Antialiasing, true);
    painter.setPen(QPen(QColor(QStringLiteral("#66ff66")), 2.0));
    for (const QRect& det : detections) {
        painter.drawRect(det);
    }
    painter.setPen(Qt::NoPen);
    painter.setBrush(QColor(0, 0, 0, 160));
    const QRect panel(8, 8, 220, 34);
    painter.drawRoundedRect(panel, 6.0, 6.0);
    painter.setPen(Qt::white);
    painter.drawText(panel.adjusted(10, 0, -10, 0), Qt::AlignVCenter | Qt::AlignLeft,
                     QStringLiteral("Tracks: %1").arg(activeTracks));
    return preview;
}

QJsonArray buildContinuityStreams(const QJsonArray& tracks,
                                  const QJsonObject& transcriptRoot,
                                  const QString& detectorMode,
                                  bool onlyDialogue)
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
        stream[QStringLiteral("keyframes")] = keyframes;
        streams.push_back(stream);
    }
    return streams;
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
    return buildContinuityStreams(rawTracks, transcriptRoot, detectorMode, onlyDialogue);
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
            const QJsonObject byClip =
                processedArtifact.value(QStringLiteral("continuity_facestreams_by_clip")).toObject();
            const QJsonObject processedRoot = byClip.value(clipId).toObject();
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
                const QJsonObject byClip =
                    processedArtifact.value(QStringLiteral("continuity_facestreams_by_clip")).toObject();
                const QJsonObject processedRoot = byClip.value(clipId).toObject();
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
        QStringLiteral("facestream_part"),
        QStringLiteral("facestream_bin"),
        QStringLiteral("facestream_ndjson"),
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

    const QString rawArtifactPath = engine.facestreamArtifactPath(transcriptPath);
    const QJsonObject processedRoot =
        buildProcessedContinuityRoot(clipId, rawContinuityRoot, transcriptRoot, rawArtifactPath);

    QJsonObject byClip = artifactRoot.value(QStringLiteral("continuity_facestreams_by_clip")).toObject();
    byClip[clipId] = processedRoot;
    artifactRoot[QStringLiteral("schema")] = QStringLiteral("jcut_facestream_processed_v1");
    artifactRoot[QStringLiteral("updated_at_utc")] = QDateTime::currentDateTimeUtc().toString(Qt::ISODate);
    artifactRoot[QStringLiteral("continuity_facestreams_by_clip")] = byClip;
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
    }
    if (!rawTracks.isEmpty()) {
        root[QStringLiteral("raw_tracks")] = rawTracks;
    }
    if (!rawFrames.isEmpty()) {
        root[QStringLiteral("raw_frames")] = rawFrames;
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
    if (objectOut) {
        *objectOut = QJsonObject{};
    }
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        if (errorOut) {
            *errorOut = QStringLiteral("Failed to open %1").arg(path);
        }
        return false;
    }
    QDataStream stream(&file);
    stream.setVersion(QDataStream::Qt_6_0);
    quint32 magic = 0;
    quint32 version = 0;
    QByteArray compressed;
    stream >> magic;
    stream >> version;
    stream >> compressed;
    if (stream.status() != QDataStream::Ok || magic != 0x4A435554 || version != 1) {
        if (errorOut) {
            *errorOut = QStringLiteral("Invalid binary artifact header in %1").arg(path);
        }
        return false;
    }

    const QByteArray payload = qUncompress(compressed);
    QJsonParseError parseError;
    const QJsonDocument doc = QJsonDocument::fromJson(payload, &parseError);
    if (parseError.error != QJsonParseError::NoError || !doc.isObject()) {
        if (errorOut) {
            *errorOut = QStringLiteral("Failed to parse binary artifact %1: %2")
                            .arg(path, parseError.errorString());
        }
        return false;
    }
    if (objectOut) {
        *objectOut = doc.object();
    }
    return true;
}

bool saveContinuityArtifact(const QString& transcriptPath,
                            const QString& clipId,
                            const QJsonObject& continuityRoot,
                            QJsonObject* artifactRootOut)
{
    editor::TranscriptEngine engine;
    QJsonObject artifactRoot;
    engine.loadFacestreamArtifact(transcriptPath, &artifactRoot);
    QJsonObject continuityByClip = artifactRoot.value(QStringLiteral("continuity_facestreams_by_clip")).toObject();
    continuityByClip[clipId] = continuityRoot;
    artifactRoot[QStringLiteral("schema")] = QStringLiteral("jcut_facestream_v1");
    artifactRoot[QStringLiteral("continuity_facestreams_by_clip")] = continuityByClip;
    if (artifactRootOut) {
        *artifactRootOut = artifactRoot;
    }
    return engine.saveFacestreamArtifact(transcriptPath, artifactRoot);
}

} // namespace jcut::facestream
