#include "editor_shared.h"
#include "editor_shared_keyframes_cache.h"
#include "editor_shared_timing.h"
#include "facedetections_artifact_utils.h"
#include "facedetections_runtime.h"
#include "facedetections_time_mapping.h"
#include "speaker_track_assignment_service.h"
#include "transcript_engine.h"
#include "transform_skip_aware_timing.h"

#include <QCoreApplication>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QFileInfo>
#include <QMutex>
#include <QMutexLocker>
#include <QSet>
#include <QStringList>
#include <QThread>
#include <QtConcurrent/QtConcurrentRun>
#include <QtGlobal>

#include <algorithm>
#include <cmath>
#include <functional>

qreal sanitizeScaleValue(qreal value);

namespace {
constexpr qreal kGradingEpsilon = 0.000001;
constexpr qreal kDefaultSpeakerFramingTargetXNorm = 0.5;
constexpr qreal kDefaultSpeakerFramingTargetYNorm = 0.35;
constexpr qreal kDefaultSpeakerFramingTargetBoxNorm = -1.0;
constexpr int kSpeakerFramingSmoothingMovingAverage = 0;
constexpr int kSpeakerFramingSmoothingLowPass = 1;
constexpr int kSpeakerFramingSmoothingBoth = 2;
constexpr qint64 kMaxSynchronousContinuityArtifactBytes = 128ll * 1024ll * 1024ll;

struct RobustScalarSample {
    qreal value = 0.0;
    int64_t frame = 0;
    qreal confidence = 1.0;
};

enum class ContinuityAssignmentMode {
    SectionOrIdentity,
    SectionOnly
};

bool assignedContinuityTrackSampleForSpeaker(const TimelineClip& clip,
                                             const QString& speakerId,
                                             qreal timelineFramePosition,
                                             qreal mediaSourceFramePosition,
                                             QPointF* locationOut,
                                             qreal* boxSizeOut,
                                             qreal* rotationDegreesOut = nullptr,
                                             ContinuityAssignmentMode mode = ContinuityAssignmentMode::SectionOrIdentity,
                                             bool* matchedSectionAssignmentOut = nullptr);
bool manualContinuityTrackSampleForClip(const TimelineClip& clip,
                                        qreal timelineFramePosition,
                                        qreal mediaSourceFramePosition,
                                        QPointF* locationOut,
                                        qreal* boxSizeOut);

bool currentThreadIsGuiThread()
{
    QCoreApplication* app = QCoreApplication::instance();
    return app && QThread::currentThread() == app->thread();
}

QJsonObject transformKeyframeDiagnosticObject(const TimelineClip::TransformKeyframe& transform)
{
    return QJsonObject{
        {QStringLiteral("frame"), static_cast<qint64>(transform.frame)},
        {QStringLiteral("translation_x"), transform.translationX},
        {QStringLiteral("translation_y"), transform.translationY},
        {QStringLiteral("rotation"), transform.rotation},
        {QStringLiteral("scale_x"), transform.scaleX},
        {QStringLiteral("scale_y"), transform.scaleY},
        {QStringLiteral("linear_interpolation"), transform.linearInterpolation}
    };
}

QJsonArray intSetDiagnosticArray(const QSet<int>& values)
{
    QList<int> sorted = values.values();
    std::sort(sorted.begin(), sorted.end());
    QJsonArray array;
    for (const int value : sorted) {
        array.push_back(value);
    }
    return array;
}

QJsonArray stringSetDiagnosticArray(const QSet<QString>& values)
{
    QStringList sorted = values.values();
    sorted.sort();
    QJsonArray array;
    for (const QString& value : sorted) {
        array.push_back(value);
    }
    return array;
}

bool& speakerFramingRuntimeBuildAllowed()
{
    static thread_local bool allowed = false;
    return allowed;
}

class ScopedSpeakerFramingRuntimeBuild {
public:
    ScopedSpeakerFramingRuntimeBuild()
        : previous_(speakerFramingRuntimeBuildAllowed())
    {
        speakerFramingRuntimeBuildAllowed() = true;
    }

    ~ScopedSpeakerFramingRuntimeBuild()
    {
        speakerFramingRuntimeBuildAllowed() = previous_;
    }

private:
    bool previous_ = false;
};

QMutex& continuityWarmPendingMutex()
{
    static QMutex mutex;
    return mutex;
}

QSet<QString>& continuityWarmPendingKeys()
{
    static QSet<QString> keys;
    return keys;
}

void enqueueContinuityWarmJob(const QString& cacheKey, std::function<void()> job)
{
    if (cacheKey.trimmed().isEmpty()) {
        return;
    }
    {
        QMutexLocker locker(&continuityWarmPendingMutex());
        QSet<QString>& pending = continuityWarmPendingKeys();
        if (pending.contains(cacheKey)) {
            return;
        }
        pending.insert(cacheKey);
    }
    (void)QtConcurrent::run([cacheKey, job = std::move(job)]() {
        job();
        QMutexLocker locker(&continuityWarmPendingMutex());
        continuityWarmPendingKeys().remove(cacheKey);
    });
}

void warmAssignedContinuityForSpeaker(const TimelineClip& clip, const QString& speakerId)
{
    const QString trimmedSpeakerId = speakerId.trimmed();
    if (clip.id.trimmed().isEmpty() || clip.filePath.trimmed().isEmpty() || trimmedSpeakerId.isEmpty()) {
        return;
    }
    const QString cacheKey = assignedContinuityCacheKey(clip.filePath, clip.id, trimmedSpeakerId);
    AssignedContinuityStreamsPtr cachedStreams;
    if (cachedAssignedContinuityStreamsMemoryOnly(cacheKey, &cachedStreams)) {
        return;
    }
    QPointF ignoredLocation;
    qreal ignoredBoxSize = -1.0;
    const qreal timelineFrame =
        static_cast<qreal>(qMax<int64_t>(0, clip.startFrame));
    const qreal sourceFrame =
        sourceFramePositionForClipAtTimelinePosition(clip, timelineFrame, {});
    (void)assignedContinuityTrackSampleForSpeaker(
        clip, trimmedSpeakerId, timelineFrame, sourceFrame, &ignoredLocation, &ignoredBoxSize);
}

void warmAssignedContinuityForSpeakerAtTranscriptFrame(const TimelineClip& clip,
                                                       const QString& speakerId,
                                                       int64_t transcriptFrame)
{
    const QString trimmedSpeakerId = speakerId.trimmed();
    if (clip.id.trimmed().isEmpty() || clip.filePath.trimmed().isEmpty() || trimmedSpeakerId.isEmpty()) {
        return;
    }
    QPointF ignoredLocation;
    qreal ignoredBoxSize = -1.0;
    const qreal sourceFps = resolvedSourceFps(clip);
    const qreal sourceFrame =
        (static_cast<qreal>(qMax<int64_t>(0, transcriptFrame)) /
         static_cast<qreal>(qMax<int64_t>(1, kTimelineFps))) *
        qMax<qreal>(1.0, sourceFps);
    (void)assignedContinuityTrackSampleForSpeaker(
        clip,
        trimmedSpeakerId,
        static_cast<qreal>(qMax<int64_t>(0, clip.startFrame)),
        sourceFrame,
        &ignoredLocation,
        &ignoredBoxSize);
}

void warmManualContinuityForClip(const TimelineClip& clip)
{
    const int manualTrackId = clip.speakerFramingManualTrackId;
    const QString manualStreamId = clip.speakerFramingManualStreamId.trimmed();
    if (clip.id.trimmed().isEmpty() ||
        clip.filePath.trimmed().isEmpty() ||
        (manualTrackId < 0 && manualStreamId.isEmpty())) {
        return;
    }
    const QString manualKey =
        QStringLiteral("manual:%1:%2").arg(manualTrackId).arg(manualStreamId);
    const QString cacheKey = assignedContinuityCacheKey(clip.filePath, clip.id, manualKey);
    AssignedContinuityStreamsPtr cachedStreams;
    if (cachedAssignedContinuityStreamsMemoryOnly(cacheKey, &cachedStreams)) {
        return;
    }
    QPointF ignoredLocation;
    qreal ignoredBoxSize = -1.0;
    const qreal timelineFrame =
        static_cast<qreal>(qMax<int64_t>(0, clip.startFrame));
    const qreal sourceFrame =
        sourceFramePositionForClipAtTimelinePosition(clip, timelineFrame, {});
    (void)manualContinuityTrackSampleForClip(
        clip, timelineFrame, sourceFrame, &ignoredLocation, &ignoredBoxSize);
}

void appendReferencedArtifactPath(QStringList* paths, const QJsonObject& root, const QString& key)
{
    if (!paths) return;
    const QString path = root.value(key).toString().trimmed();
    if (!path.isEmpty() && !paths->contains(path)) paths->push_back(path);
}

QJsonObject speakerFlowClipPayload(const QJsonObject& transcriptRoot, const QString& clipId)
{
    const QJsonObject speakerFlow = transcriptRoot.value(QStringLiteral("speaker_flow")).toObject();
    const QJsonObject clips = speakerFlow.value(QStringLiteral("clips")).toObject();
    return clips.value(clipId).toObject();
}

QJsonArray sectionTrackMapForClip(const QJsonObject& transcriptRoot, const QString& clipId)
{
    const QJsonObject clipPayload = speakerFlowClipPayload(transcriptRoot, clipId);
    QJsonArray map =
        clipPayload.value(QStringLiteral("resolved_current"))
            .toObject()
            .value(QStringLiteral("section_track_map"))
            .toArray();
    if (map.isEmpty()) {
        map = clipPayload.value(QStringLiteral("section_track_map")).toArray();
    }
    return map;
}

QString sectionAssignmentCacheToken(const QJsonObject& row)
{
    auto canonicalStreamId = [](int trackId, const QString& streamId) {
        const QString trimmed = streamId.trimmed();
        if (trackId >= 0 && (trimmed.isEmpty() || trimmed == QStringLiteral("raw_detection"))) {
            return QStringLiteral("T%1").arg(trackId);
        }
        return trimmed;
    };
    auto trackToken = [&row, &canonicalStreamId]() {
        QStringList ids;
        const QJsonArray entries = row.value(QStringLiteral("tracks")).toArray();
        if (!entries.isEmpty()) {
            for (const QJsonValue& value : entries) {
                const QJsonObject entry = value.toObject();
                const int trackId = entry.value(QStringLiteral("track_id")).toInt(-1);
                const QString streamId = canonicalStreamId(
                    trackId,
                    entry.value(QStringLiteral("stream_id")).toString());
                if (trackId >= 0 || !streamId.isEmpty()) {
                    ids.push_back(QStringLiteral("%1:%2").arg(trackId).arg(streamId));
                }
            }
        } else {
            const int trackId = row.value(QStringLiteral("track_id")).toInt(-1);
            const QString streamId =
                canonicalStreamId(trackId, row.value(QStringLiteral("stream_id")).toString());
            if (trackId >= 0 || !streamId.isEmpty()) {
                ids.push_back(QStringLiteral("%1:%2").arg(trackId).arg(streamId));
            }
        }
        ids.sort();
        return ids.join(QLatin1Char(','));
    };
    const QString sectionKey = row.value(QStringLiteral("section_key")).toString().trimmed();
    if (!sectionKey.isEmpty()) {
        return QStringLiteral("section:%1:%2").arg(sectionKey, trackToken());
    }
    return QStringLiteral("section:%1:%2:%3:%4")
        .arg(row.value(QStringLiteral("speaker_id")).toString().trimmed())
        .arg(row.value(QStringLiteral("start_frame")).toInteger(-1))
        .arg(row.value(QStringLiteral("end_frame")).toInteger(-1))
        .arg(trackToken());
}

QJsonArray sectionTrackEntriesForRuntime(const QJsonObject& row)
{
    auto canonicalStreamId = [](int trackId, const QString& streamId) {
        const QString trimmed = streamId.trimmed();
        if (trackId >= 0 && (trimmed.isEmpty() || trimmed == QStringLiteral("raw_detection"))) {
            return QStringLiteral("T%1").arg(trackId);
        }
        return trimmed;
    };
    QJsonArray entries = row.value(QStringLiteral("tracks")).toArray();
    if (!entries.isEmpty()) {
        QJsonArray normalized;
        for (const QJsonValue& value : entries) {
            QJsonObject entry = value.toObject();
            const int trackId = entry.value(QStringLiteral("track_id")).toInt(-1);
            entry[QStringLiteral("stream_id")] =
                canonicalStreamId(trackId, entry.value(QStringLiteral("stream_id")).toString());
            normalized.push_back(entry);
        }
        return normalized;
    }
    const int trackId = row.value(QStringLiteral("track_id")).toInt(-1);
    if (trackId < 0 && row.value(QStringLiteral("stream_id")).toString().trimmed().isEmpty()) {
        return {};
    }
    QJsonObject entry{
        {QStringLiteral("track_id"), trackId},
        {QStringLiteral("stream_id"),
         canonicalStreamId(trackId, row.value(QStringLiteral("stream_id")).toString())}
    };
    return QJsonArray{entry};
}

int64_t sectionTranscriptFrameForSecondsRuntime(double seconds, bool roundUp)
{
    if (seconds < 0.0) {
        return -1;
    }
    const double frame = seconds * static_cast<double>(kTimelineFps);
    return qMax<int64_t>(
        0,
        static_cast<int64_t>(roundUp ? std::ceil(frame) : std::floor(frame)));
}

int sectionWordCountForRuntime(const QJsonObject& transcriptRoot,
                               const QString& speakerId,
                               int64_t startFrame,
                               int64_t endFrame)
{
    const QString trimmedSpeakerId = speakerId.trimmed();
    if (trimmedSpeakerId.isEmpty() || startFrame < 0 || endFrame < startFrame) {
        return 0;
    }
    int wordCount = 0;
    const QJsonArray segments = transcriptRoot.value(QStringLiteral("segments")).toArray();
    for (const QJsonValue& segValue : segments) {
        const QJsonObject segmentObj = segValue.toObject();
        const QString segmentSpeaker =
            segmentObj.value(QStringLiteral("speaker")).toString().trimmed();
        const QJsonArray words = segmentObj.value(QStringLiteral("words")).toArray();
        if (!words.isEmpty()) {
            for (const QJsonValue& wordValue : words) {
                const QJsonObject wordObj = wordValue.toObject();
                if (wordObj.value(QStringLiteral("skipped")).toBool(false)) {
                    continue;
                }
                QString wordSpeaker = wordObj.value(QStringLiteral("speaker")).toString().trimmed();
                if (wordSpeaker.isEmpty()) {
                    wordSpeaker = segmentSpeaker;
                }
                if (wordSpeaker != trimmedSpeakerId) {
                    continue;
                }
                const int64_t wordFrame =
                    sectionTranscriptFrameForSecondsRuntime(
                        wordObj.value(QStringLiteral("start")).toDouble(-1.0), false);
                if (wordFrame >= startFrame && wordFrame <= endFrame) {
                    ++wordCount;
                }
            }
            continue;
        }
        if (segmentSpeaker != trimmedSpeakerId ||
            segmentObj.value(QStringLiteral("skipped")).toBool(false)) {
            continue;
        }
        const int64_t segmentStart =
            sectionTranscriptFrameForSecondsRuntime(
                segmentObj.value(QStringLiteral("start")).toDouble(-1.0), false);
        const int64_t segmentEnd =
            sectionTranscriptFrameForSecondsRuntime(
                segmentObj.value(QStringLiteral("end")).toDouble(-1.0), true);
        if (segmentStart <= endFrame && segmentEnd >= startFrame) {
            wordCount += segmentObj.value(QStringLiteral("text"))
                             .toString()
                             .split(QLatin1Char(' '), Qt::SkipEmptyParts)
                             .size();
        }
    }
    return wordCount;
}

int sectionAssignmentWordCountForRuntime(const QJsonObject& transcriptRoot, const QJsonObject& row)
{
    const int stored = row.value(QStringLiteral("word_count")).toInt(-1);
    if (stored >= 0) {
        return stored;
    }
    return sectionWordCountForRuntime(
        transcriptRoot,
        row.value(QStringLiteral("speaker_id")).toString().trimmed(),
        row.value(QStringLiteral("start_frame")).toInteger(-1),
        row.value(QStringLiteral("end_frame")).toInteger(-1));
}

int64_t transcriptFrameForMediaSourcePosition(const TimelineClip& clip, qreal mediaSourceFramePosition)
{
    return transcriptFrameForClipSourceFrame(
        clip,
        qMax<int64_t>(0, static_cast<int64_t>(std::floor(mediaSourceFramePosition))));
}

bool collectSectionTrackAssignmentsForSpeaker(const QJsonObject& transcriptRoot,
                                              const TimelineClip& clip,
                                              const QString& speakerId,
                                              qreal mediaSourceFramePosition,
                                              QSet<int>* trackIds,
                                              QSet<QString>* streamIds,
                                              QString* cacheTokenOut,
                                              qreal* rotationDegreesOut = nullptr)
{
    if (!trackIds || !streamIds) {
        return false;
    }
    const QString trimmedSpeakerId = speakerId.trimmed();
    if (trimmedSpeakerId.isEmpty()) {
        return false;
    }
    const int64_t transcriptFrame =
        transcriptFrameForMediaSourcePosition(clip, mediaSourceFramePosition);
    const QJsonArray sectionMap = sectionTrackMapForClip(transcriptRoot, clip.id);
    for (const QJsonValue& value : sectionMap) {
        const QJsonObject row = value.toObject();
        if (row.value(QStringLiteral("speaker_id")).toString().trimmed() != trimmedSpeakerId) {
            continue;
        }
        const int64_t startFrame = row.value(QStringLiteral("start_frame")).toInteger(-1);
        const int64_t endFrame = row.value(QStringLiteral("end_frame")).toInteger(-1);
        if (startFrame >= 0 && endFrame >= startFrame &&
            (transcriptFrame < startFrame || transcriptFrame > endFrame)) {
            continue;
        }
        if (sectionAssignmentWordCountForRuntime(transcriptRoot, row) <
            qBound(0, clip.speakerSectionMinimumWords, 1000)) {
            continue;
        }
        bool anyTrack = false;
        for (const QJsonValue& entryValue : sectionTrackEntriesForRuntime(row)) {
            const QJsonObject entry = entryValue.toObject();
            const int trackId = entry.value(QStringLiteral("track_id")).toInt(-1);
            const QString streamId = entry.value(QStringLiteral("stream_id")).toString().trimmed();
            if (trackId >= 0) {
                trackIds->insert(trackId);
                anyTrack = true;
            }
            if (!streamId.isEmpty()) {
                streamIds->insert(streamId);
                anyTrack = true;
            }
        }
        if (!anyTrack) {
            continue;
        }
        if (cacheTokenOut) {
            *cacheTokenOut = sectionAssignmentCacheToken(row);
        }
        if (rotationDegreesOut) {
            *rotationDegreesOut =
                qBound<qreal>(-180.0, row.value(QStringLiteral("rotation")).toDouble(0.0), 180.0);
        }
        return true;
    }
    return false;
}

QRectF fitRectForSourceInOutput(const QSize& source, const QSize& output) {
    const QSize safeOutput = output.isValid() ? output : QSize(1080, 1920);
    if (!source.isValid()) {
        return QRectF(QPointF(0.0, 0.0), QSizeF(safeOutput));
    }
    return previewFitRectToBoundsF(source, QRectF(QPointF(0.0, 0.0), QSizeF(safeOutput)));
}

QSize sourceSizeForClipCached(const TimelineClip& clip, const QSize& outputSize) {
    if (clip.sourceFrameSize.isValid()) {
        return clip.sourceFrameSize;
    }
    static QMutex mutex;
    static QHash<QString, QSize> cacheByPath;
    const QString mediaPath = interactivePreviewMediaPathForClip(clip);
    const QString path = QFileInfo::exists(mediaPath) ? mediaPath : clip.filePath;
    if (path.trimmed().isEmpty()) {
        return outputSize;
    }
    {
        QMutexLocker locker(&mutex);
        const auto it = cacheByPath.constFind(path);
        if (it != cacheByPath.constEnd() && it.value().isValid()) {
            return it.value();
        }
    }
    const MediaProbeResult probe = probeMediaFile(path, 4.0);
    const QSize resolved = probe.frameSize.isValid() ? probe.frameSize : outputSize;
    {
        QMutexLocker locker(&mutex);
        cacheByPath[path] = resolved;
    }
    return resolved;
}

qreal smoothingAmountForStrength(qreal strength)
{
    const qreal boundedStrength =
        qBound<qreal>(0.0, strength, TimelineClip::kSpeakerFramingSmoothingStrengthMax);
    if (boundedStrength <= 0.0) {
        return 0.0;
    }
    return 1.0 - std::exp(-boundedStrength);
}

qreal medianValue(QVector<qreal> values)
{
    if (values.isEmpty()) {
        return 0.0;
    }
    std::sort(values.begin(), values.end());
    const int middle = values.size() / 2;
    if ((values.size() % 2) == 1) {
        return values.at(middle);
    }
    return (values.at(middle - 1) + values.at(middle)) * 0.5;
}

qreal robustSmoothedScalar(qreal original,
                           const QVector<RobustScalarSample>& samples,
                           int64_t lookupFrame,
                           int windowFrames,
                           int smoothingMode,
                           qreal strength,
                           qreal minRobustScale)
{
    if (samples.size() <= 1 || windowFrames <= 1) {
        return original;
    }
    const qreal amount = smoothingAmountForStrength(strength);
    if (amount <= 0.0) {
        return original;
    }

    QVector<qreal> values;
    values.reserve(samples.size());
    for (const RobustScalarSample& sample : samples) {
        values.push_back(sample.value);
    }
    const qreal median = medianValue(values);

    QVector<qreal> deviations;
    deviations.reserve(samples.size());
    for (const RobustScalarSample& sample : samples) {
        deviations.push_back(std::abs(sample.value - median));
    }
    const qreal mad = medianValue(deviations);
    const qreal modeScale = smoothingMode == kSpeakerFramingSmoothingLowPass
        ? 3.25
        : (smoothingMode == kSpeakerFramingSmoothingBoth ? 2.75 : 2.25);
    const qreal robustScale = qMax(minRobustScale, qMax<qreal>(mad * modeScale, 0.000001));
    const qreal sigmaFrames = qMax<qreal>(1.0, static_cast<qreal>(windowFrames) / 3.0);

    qreal weightedSum = 0.0;
    qreal weightSum = 0.0;
    for (const RobustScalarSample& sample : samples) {
        const qreal normalizedResidual = std::abs(sample.value - median) / robustScale;
        if (normalizedResidual >= 1.0) {
            continue;
        }
        const qreal residualWeight =
            std::pow(1.0 - (normalizedResidual * normalizedResidual), 2.0);
        const qreal frameDistance = std::abs(static_cast<qreal>(sample.frame - lookupFrame));
        const qreal temporalWeight =
            std::exp(-0.5 * std::pow(frameDistance / sigmaFrames, 2.0));
        const qreal confidenceWeight =
            0.25 + (0.75 * qBound<qreal>(0.0, sample.confidence, 1.0));
        const qreal weight = residualWeight * temporalWeight * confidenceWeight;
        weightedSum += sample.value * weight;
        weightSum += weight;
    }

    const qreal robustTarget = weightSum > 0.0 ? weightedSum / weightSum : median;
    return original + ((robustTarget - original) * amount);
}

bool streamSampleAtFrame(const TimelineClip& clip,
                         const jcut::facedetections::FacestreamTrack& stream,
                         qreal timelineFramePosition,
                         qreal mediaSourceFramePosition,
                         int centerSmoothingFrames,
                         int zoomSmoothingFrames,
                         int smoothingMode,
                         qreal centerSmoothingStrength,
                         qreal zoomSmoothingStrength,
                         int gapHoldFrames,
                         QPointF* locationOut,
                         qreal* boxSizeOut)
{
    if (stream.keyframes.isEmpty()) {
        return false;
    }

    FacestreamFrameDomain domain = FacestreamFrameDomain::SourceRelative;
    if (!parseFacestreamFrameDomainString(stream.summary.frameDomain, &domain)) {
        int64_t minFrame = stream.summary.minFrame;
        int64_t maxFrame = stream.summary.maxFrame;
        if (minFrame < 0 || maxFrame < minFrame) {
            minFrame = std::numeric_limits<int64_t>::max();
            maxFrame = std::numeric_limits<int64_t>::min();
            for (const jcut::facedetections::FacestreamKeyframe& keyframe : stream.keyframes) {
                if (keyframe.frame < 0) {
                    continue;
                }
                minFrame = qMin<int64_t>(minFrame, keyframe.frame);
                maxFrame = qMax<int64_t>(maxFrame, keyframe.frame);
            }
        }
        domain = inferFacestreamFrameDomain(clip, minFrame, maxFrame);
    }

    const auto firstValidIt = std::find_if(
        stream.keyframes.constBegin(),
        stream.keyframes.constEnd(),
        [](const jcut::facedetections::FacestreamKeyframe& keyframe) {
            return keyframe.frame >= 0;
        });
    if (firstValidIt == stream.keyframes.constEnd()) {
        return false;
    }
    int64_t lastValidFrame = -1;
    for (int i = stream.keyframes.size() - 1; i >= 0; --i) {
        if (stream.keyframes.at(i).frame >= 0) {
            lastValidFrame = stream.keyframes.at(i).frame;
            break;
        }
    }
    const int64_t typicalStep = qMax<int64_t>(1, stream.summary.typicalFrameStep);
    const int64_t edgeHoldFrames = qMax<int64_t>(
        facedetectionsMaxEdgeHoldFrames(typicalStep),
        qBound(0, gapHoldFrames, TimelineClip::kSpeakerFramingGapHoldMaxFrames));

    const qreal localTimelineFrame =
        qMax<qreal>(0.0, timelineFramePosition - static_cast<qreal>(clip.startFrame));
    const qreal localMediaSourceFrame =
        qMax<qreal>(0.0,
                    mediaSourceFramePosition - static_cast<qreal>(qMax<int64_t>(0, clip.sourceInFrame)));
    auto lookupPositionForDomain = [&](FacestreamFrameDomain lookupDomain) {
        if (lookupDomain == FacestreamFrameDomain::ClipTimeline30Fps) {
            return localTimelineFrame;
        }
        if (lookupDomain == FacestreamFrameDomain::SourceAbsolute) {
            return qMax<qreal>(0.0, mediaSourceFramePosition);
        }
        return localMediaSourceFrame;
    };
    const qreal lookupFramePosition = lookupPositionForDomain(domain);
    const int64_t lookupFrame = qMax<int64_t>(
        0,
        static_cast<int64_t>(std::floor(lookupFramePosition)));

    const auto nextIt = std::lower_bound(
        firstValidIt,
        stream.keyframes.constEnd(),
        lookupFramePosition,
        [](const jcut::facedetections::FacestreamKeyframe& point, qreal frame) {
            return point.frame < frame;
        });
    const jcut::facedetections::FacestreamKeyframe* previous =
        (nextIt != firstValidIt) ? &(*(nextIt - 1)) : nullptr;
    const jcut::facedetections::FacestreamKeyframe* next =
        (nextIt != stream.keyframes.constEnd()) ? &(*nextIt) : nullptr;
    auto resolvedPoint = [](const jcut::facedetections::FacestreamKeyframe& keyframe) {
        FacestreamResolvedKeyframe point;
        point.frame = keyframe.frame;
        point.xNorm = qBound<qreal>(0.0, keyframe.x, 1.0);
        point.yNorm = qBound<qreal>(0.0, keyframe.y, 1.0);
        point.boxSizeNorm = qBound<qreal>(0.01, keyframe.box, 1.0);
        point.confidence = qBound<qreal>(0.0, keyframe.confidence, 1.0);
        point.hasCenterBox = true;
        return point;
    };

    FacestreamResolvedKeyframe sample;
    if (next && qFuzzyCompare(static_cast<qreal>(next->frame) + 1.0, lookupFramePosition + 1.0)) {
        sample = resolvedPoint(*next);
    } else if (previous && qFuzzyCompare(static_cast<qreal>(previous->frame) + 1.0,
                                         lookupFramePosition + 1.0)) {
        sample = resolvedPoint(*previous);
    } else if (previous && next &&
               facedetectionsShouldBridgeGap(previous->frame, next->frame, typicalStep)) {
        const int64_t span = qMax<int64_t>(1, next->frame - previous->frame);
        const qreal t = qBound<qreal>(
            0.0,
            (lookupFramePosition - static_cast<qreal>(previous->frame)) / static_cast<qreal>(span),
            1.0);
        const FacestreamResolvedKeyframe previousPoint = resolvedPoint(*previous);
        const FacestreamResolvedKeyframe nextPoint = resolvedPoint(*next);
        sample.xNorm = previousPoint.xNorm + ((nextPoint.xNorm - previousPoint.xNorm) * t);
        sample.yNorm = previousPoint.yNorm + ((nextPoint.yNorm - previousPoint.yNorm) * t);
        sample.boxSizeNorm =
            previousPoint.boxSizeNorm + ((nextPoint.boxSizeNorm - previousPoint.boxSizeNorm) * t);
        sample.confidence =
            previousPoint.confidence + ((nextPoint.confidence - previousPoint.confidence) * t);
        sample.frame = lookupFrame;
        sample.hasCenterBox = true;
    } else if (previous && qAbs(previous->frame - lookupFrame) <= edgeHoldFrames) {
        sample = resolvedPoint(*previous);
    } else if (next && qAbs(next->frame - lookupFrame) <= edgeHoldFrames) {
        sample = resolvedPoint(*next);
    } else {
        return false;
    }

    const int centerWindowFrames =
        qBound(0, centerSmoothingFrames, TimelineClip::kSpeakerFramingSmoothingMaxFrames);
    const int zoomWindowFrames =
        qBound(0, zoomSmoothingFrames, TimelineClip::kSpeakerFramingSmoothingMaxFrames);
    if ((smoothingAmountForStrength(centerSmoothingStrength) > 0.0 && centerWindowFrames > 1) ||
        (smoothingAmountForStrength(zoomSmoothingStrength) > 0.0 && zoomWindowFrames > 1)) {
        const int centerBefore = centerWindowFrames / 2;
        const int centerAfter = centerWindowFrames - centerBefore - 1;
        const int64_t centerStartFrame = lookupFrame - centerBefore;
        const int64_t centerEndFrame = lookupFrame + centerAfter;
        QVector<RobustScalarSample> centerXSamples;
        QVector<RobustScalarSample> centerYSamples;
        if (centerWindowFrames > 1) {
            centerXSamples.push_back({sample.xNorm, lookupFrame, sample.confidence});
            centerYSamples.push_back({sample.yNorm, lookupFrame, sample.confidence});
        }

        const int zoomBefore = zoomWindowFrames / 2;
        const int zoomAfter = zoomWindowFrames - zoomBefore - 1;
        const int64_t zoomStartFrame = lookupFrame - zoomBefore;
        const int64_t zoomEndFrame = lookupFrame + zoomAfter;
        QVector<RobustScalarSample> zoomSamples;
        if (zoomWindowFrames > 1) {
            zoomSamples.push_back({std::log(sample.boxSizeNorm), lookupFrame, sample.confidence});
        }
        const int64_t sampleStartFrame = qMin(centerStartFrame, zoomStartFrame);
        const int64_t sampleEndFrame = qMax(centerEndFrame, zoomEndFrame);
        const auto sampleStartIt = std::lower_bound(
            firstValidIt,
            stream.keyframes.constEnd(),
            static_cast<qreal>(sampleStartFrame),
            [](const jcut::facedetections::FacestreamKeyframe& point, qreal frame) {
                return point.frame < frame;
            });
        for (auto sampleIt = sampleStartIt;
             sampleIt != stream.keyframes.constEnd() && sampleIt->frame <= sampleEndFrame;
             ++sampleIt) {
            const jcut::facedetections::FacestreamKeyframe& keyframe = *sampleIt;
            if (keyframe.frame < 0 || keyframe.frame == lookupFrame || keyframe.box <= 0.0) {
                continue;
            }
            const FacestreamResolvedKeyframe point = resolvedPoint(keyframe);
            if (centerWindowFrames > 1 &&
                point.frame >= centerStartFrame &&
                point.frame <= centerEndFrame) {
                centerXSamples.push_back({point.xNorm, point.frame, point.confidence});
                centerYSamples.push_back({point.yNorm, point.frame, point.confidence});
            }
            if (zoomWindowFrames > 1 &&
                point.frame >= zoomStartFrame &&
                point.frame <= zoomEndFrame) {
                zoomSamples.push_back({std::log(point.boxSizeNorm), point.frame, point.confidence});
            }
        }
        if (centerWindowFrames > 1) {
            sample.xNorm = qBound<qreal>(
                0.0,
                robustSmoothedScalar(sample.xNorm,
                                     centerXSamples,
                                     lookupFrame,
                                     centerWindowFrames,
                                     smoothingMode,
                                     centerSmoothingStrength,
                                     0.015),
                1.0);
            sample.yNorm = qBound<qreal>(
                0.0,
                robustSmoothedScalar(sample.yNorm,
                                     centerYSamples,
                                     lookupFrame,
                                     centerWindowFrames,
                                     smoothingMode,
                                     centerSmoothingStrength,
                                     0.015),
                1.0);
        }
        if (zoomWindowFrames > 1) {
            const qreal smoothedLogBox = robustSmoothedScalar(std::log(sample.boxSizeNorm),
                                                              zoomSamples,
                                                              lookupFrame,
                                                              zoomWindowFrames,
                                                              smoothingMode,
                                                              zoomSmoothingStrength,
                                                              0.08);
            sample.boxSizeNorm = qBound<qreal>(0.01, std::exp(smoothedLogBox), 1.0);
        }
    }

    if (locationOut) {
        *locationOut = QPointF(sample.xNorm, sample.yNorm);
    }
    if (boxSizeOut) {
        *boxSizeOut = sample.boxSizeNorm;
    }
    return sample.boxSizeNorm > 0.0;
}

bool assignedContinuityTrackSampleForSpeaker(const TimelineClip& clip,
                                             const QString& speakerId,
                                             qreal timelineFramePosition,
                                             qreal mediaSourceFramePosition,
                                             QPointF* locationOut,
                                             qreal* boxSizeOut,
                                             qreal* rotationDegreesOut,
                                             ContinuityAssignmentMode mode,
                                             bool* matchedSectionAssignmentOut)
{
    if (rotationDegreesOut) {
        *rotationDegreesOut = 0.0;
    }
    if (matchedSectionAssignmentOut) {
        *matchedSectionAssignmentOut = false;
    }
    if (clip.id.trimmed().isEmpty() || speakerId.trimmed().isEmpty()) {
        return false;
    }
    const QString trimmedSpeakerId = speakerId.trimmed();
    QString cacheKey = assignedContinuityCacheKey(clip.filePath, clip.id, trimmedSpeakerId);
    const QString transcriptPath = transcriptPathForRuntimeSidecarForClip(clip);
    if (currentThreadIsGuiThread() && !speakerFramingRuntimeBuildAllowed()) {
        AssignedContinuityStreamsPtr cachedStreams;
        QJsonDocument transcriptDoc;
        QSet<int> assignedTrackIds;
        QSet<QString> assignedStreamIds;
        QString assignmentCacheToken;
        qreal sectionRotationDegrees = 0.0;
        if (!transcriptPath.trimmed().isEmpty() &&
            loadTranscriptJsonCached(transcriptPath, &transcriptDoc)) {
            const QJsonObject transcriptRoot = transcriptDoc.object();
            const bool sectionMappingActive =
                !sectionTrackMapForClip(transcriptRoot, clip.id).isEmpty();
            const bool matchedSectionAssignment =
                collectSectionTrackAssignmentsForSpeaker(transcriptRoot,
                                                         clip,
                                                         trimmedSpeakerId,
                                                         mediaSourceFramePosition,
                                                         &assignedTrackIds,
                                                         &assignedStreamIds,
                                                         &assignmentCacheToken,
                                                         &sectionRotationDegrees);
            if (matchedSectionAssignmentOut) {
                *matchedSectionAssignmentOut = matchedSectionAssignment;
            }
            if (sectionMappingActive && !matchedSectionAssignment) {
                return false;
            }
            if (matchedSectionAssignment && !assignmentCacheToken.isEmpty()) {
                if (rotationDegreesOut) {
                    *rotationDegreesOut = sectionRotationDegrees;
                }
                const QString sectionCacheKey =
                    assignedContinuityCacheKey(clip.filePath, clip.id, assignmentCacheToken);
                if (cachedAssignedContinuityStreamsMemoryOnly(sectionCacheKey, &cachedStreams)) {
                    const auto& streams =
                        cachedStreams ? *cachedStreams : QVector<jcut::facedetections::FacestreamTrack>{};
                    for (const jcut::facedetections::FacestreamTrack& stream : streams) {
                        if (streamSampleAtFrame(clip,
                                                stream,
                                                timelineFramePosition,
                                                mediaSourceFramePosition,
                                                clip.speakerFramingCenterSmoothingFrames,
                                                clip.speakerFramingZoomSmoothingFrames,
                                                clip.speakerFramingSmoothingMode,
                                                clip.speakerFramingCenterSmoothingStrength,
                                                clip.speakerFramingZoomSmoothingStrength,
                                                clip.speakerFramingGapHoldFrames,
                                                locationOut,
                                                boxSizeOut)) {
                            return true;
                        }
                    }
                    return false;
                }
            }
        }
        if (mode == ContinuityAssignmentMode::SectionOnly) {
            return false;
        }
        if (cachedAssignedContinuityStreamsMemoryOnly(cacheKey, &cachedStreams)) {
            const auto& streams =
                cachedStreams ? *cachedStreams : QVector<jcut::facedetections::FacestreamTrack>{};
            for (const jcut::facedetections::FacestreamTrack& stream : streams) {
                if (streamSampleAtFrame(clip,
                                        stream,
                                        timelineFramePosition,
                                        mediaSourceFramePosition,
                                        clip.speakerFramingCenterSmoothingFrames,
                                        clip.speakerFramingZoomSmoothingFrames,
                                        clip.speakerFramingSmoothingMode,
                                        clip.speakerFramingCenterSmoothingStrength,
                                        clip.speakerFramingZoomSmoothingStrength,
                                        clip.speakerFramingGapHoldFrames,
                                        locationOut,
                                        boxSizeOut)) {
                    return true;
                }
            }
            return false;
        }
        return false;
    }

    if (transcriptPath.trimmed().isEmpty()) {
        return false;
    }
    editor::TranscriptEngine engine;
    const QString rawArtifactPath = engine.facedetectionsArtifactPath(transcriptPath);
    const QString processedPath = engine.facedetectionsProcessedArtifactPath(transcriptPath);
    QStringList referencedPaths;
    appendReferencedArtifactPath(&referencedPaths, QJsonObject{{QStringLiteral("path"), rawArtifactPath}}, QStringLiteral("path"));

    AssignedContinuityStreamsPtr cachedStreams;
    QJsonDocument transcriptDoc;
    if (!engine.loadTranscriptJson(transcriptPath, &transcriptDoc)) {
        storeAssignedContinuityStreams(
            cacheKey, transcriptPath, processedPath, referencedPaths, {});
        return false;
    }
    const QJsonObject transcriptRoot = transcriptDoc.object();
    QSet<int> assignedTrackIds;
    QSet<QString> assignedStreamIds;
    QString assignmentCacheToken;
    qreal sectionRotationDegrees = 0.0;
    const bool sectionMappingActive =
        !sectionTrackMapForClip(transcriptRoot, clip.id).isEmpty();
    const bool matchedSectionAssignment =
        collectSectionTrackAssignmentsForSpeaker(transcriptRoot,
                                                 clip,
                                                 trimmedSpeakerId,
                                                 mediaSourceFramePosition,
                                                 &assignedTrackIds,
                                                 &assignedStreamIds,
                                                 &assignmentCacheToken,
                                                 &sectionRotationDegrees);
    if (matchedSectionAssignmentOut) {
        *matchedSectionAssignmentOut = matchedSectionAssignment;
    }
    if (rotationDegreesOut) {
        *rotationDegreesOut = matchedSectionAssignment ? sectionRotationDegrees : 0.0;
    }
    if (sectionMappingActive && !matchedSectionAssignment) {
        return false;
    }
    if (!sectionMappingActive &&
        cachedAssignedContinuityStreamsPtr(cacheKey, transcriptPath, processedPath, &cachedStreams)) {
        const auto& streams =
            cachedStreams ? *cachedStreams : QVector<jcut::facedetections::FacestreamTrack>{};
        for (const jcut::facedetections::FacestreamTrack& stream : streams) {
            if (streamSampleAtFrame(clip,
                                    stream,
                                    timelineFramePosition,
                                    mediaSourceFramePosition,
                                    clip.speakerFramingCenterSmoothingFrames,
                                    clip.speakerFramingZoomSmoothingFrames,
                                    clip.speakerFramingSmoothingMode,
                                    clip.speakerFramingCenterSmoothingStrength,
                                    clip.speakerFramingZoomSmoothingStrength,
                                    clip.speakerFramingGapHoldFrames,
                                    locationOut,
                                    boxSizeOut)) {
                return true;
            }
        }
        return false;
    }
    if (mode == ContinuityAssignmentMode::SectionOnly) {
        return false;
    }
    if (!matchedSectionAssignment) {
        const QJsonArray identityMap =
            jcut::speakertrack::assignmentMapForClip(transcriptRoot, clip.id);
        for (const QJsonValue& value : identityMap) {
            const QJsonObject row = value.toObject();
            if (row.value(QStringLiteral("identity_id")).toString().trimmed() != trimmedSpeakerId) {
                continue;
            }
            const int trackId = row.value(QStringLiteral("track_id")).toInt(-1);
            if (trackId >= 0) {
                assignedTrackIds.insert(trackId);
            }
            const QString streamId = row.value(QStringLiteral("stream_id")).toString().trimmed();
            if (!streamId.isEmpty()) {
                assignedStreamIds.insert(streamId);
            }
        }
    }
    if (matchedSectionAssignment && !assignmentCacheToken.isEmpty()) {
        cacheKey = assignedContinuityCacheKey(clip.filePath, clip.id, assignmentCacheToken);
        if (cachedAssignedContinuityStreamsPtr(
                cacheKey, transcriptPath, processedPath, &cachedStreams)) {
            const auto& streams =
                cachedStreams ? *cachedStreams : QVector<jcut::facedetections::FacestreamTrack>{};
            for (const jcut::facedetections::FacestreamTrack& stream : streams) {
                if (streamSampleAtFrame(clip,
                                        stream,
                                        timelineFramePosition,
                                        mediaSourceFramePosition,
                                        clip.speakerFramingCenterSmoothingFrames,
                                        clip.speakerFramingZoomSmoothingFrames,
                                        clip.speakerFramingSmoothingMode,
                                        clip.speakerFramingCenterSmoothingStrength,
                                        clip.speakerFramingZoomSmoothingStrength,
                                        clip.speakerFramingGapHoldFrames,
                                        locationOut,
                                        boxSizeOut)) {
                    return true;
                }
            }
            return false;
        }
    }
    if (assignedTrackIds.isEmpty() && assignedStreamIds.isEmpty()) {
        storeAssignedContinuityStreams(
            cacheKey, transcriptPath, processedPath, referencedPaths, {});
        return false;
    }

    QJsonObject continuityRoot;

    if (QFileInfo(rawArtifactPath).size() <= kMaxSynchronousContinuityArtifactBytes) {
        QJsonObject rawRoot;
        if (engine.loadFacestreamArtifact(transcriptPath, &rawRoot)) {
            continuityRoot = continuityRootForClip(rawRoot, clip.id);
        }
    }

    if (continuityRoot.isEmpty()) {
        if (QFileInfo(processedPath).size() > kMaxSynchronousContinuityArtifactBytes) {
            storeAssignedContinuityStreams(
                cacheKey, transcriptPath, processedPath, referencedPaths, {});
            return false;
        }
        QJsonObject processedRoot;
        if (!engine.loadFacestreamProcessedArtifact(transcriptPath, &processedRoot)) {
            storeAssignedContinuityStreams(
                cacheKey, transcriptPath, processedPath, referencedPaths, {});
            return false;
        }
        continuityRoot = continuityRootForClip(processedRoot, clip.id);
    }
    appendReferencedArtifactPath(&referencedPaths, continuityRoot, QStringLiteral("source_raw_artifact_path"));
    appendReferencedArtifactPath(&referencedPaths, continuityRoot, QStringLiteral("processed_artifact_path"));
    appendReferencedArtifactPath(&referencedPaths, continuityRoot, QStringLiteral("raw_tracks_artifact_path"));
    appendReferencedArtifactPath(&referencedPaths, continuityRoot, QStringLiteral("raw_frames_artifact_path"));
    appendReferencedArtifactPath(&referencedPaths, continuityRoot, QStringLiteral("continuity_artifact_path"));

    const QVector<jcut::facedetections::FacestreamTrack> matchingStreams =
        jcut::facedetections::continuityTrackModelsForAssignments(
        continuityRoot,
        assignedTrackIds,
        assignedStreamIds,
        continuityRoot.value(QStringLiteral("only_dialogue")).toBool(false) ? transcriptRoot : QJsonObject{});
    storeAssignedContinuityStreams(
        cacheKey, transcriptPath, processedPath, referencedPaths, matchingStreams);
    for (const jcut::facedetections::FacestreamTrack& stream : matchingStreams) {
        if (streamSampleAtFrame(clip,
                                stream,
                                timelineFramePosition,
                                mediaSourceFramePosition,
                                clip.speakerFramingCenterSmoothingFrames,
                                clip.speakerFramingZoomSmoothingFrames,
                                clip.speakerFramingSmoothingMode,
                                clip.speakerFramingCenterSmoothingStrength,
                                clip.speakerFramingZoomSmoothingStrength,
                                clip.speakerFramingGapHoldFrames,
                                locationOut,
                                boxSizeOut)) {
            return true;
        }
    }
    return false;
}

bool manualContinuityTrackSampleForClip(const TimelineClip& clip,
                                        qreal timelineFramePosition,
                                        qreal mediaSourceFramePosition,
                                        QPointF* locationOut,
                                        qreal* boxSizeOut)
{
    const int manualTrackId = clip.speakerFramingManualTrackId;
    const QString manualStreamId = clip.speakerFramingManualStreamId.trimmed();
    if (clip.id.trimmed().isEmpty() || (manualTrackId < 0 && manualStreamId.isEmpty())) {
        return false;
    }
    const QString manualKey =
        QStringLiteral("manual:%1:%2").arg(manualTrackId).arg(manualStreamId);
    const QString cacheKey = assignedContinuityCacheKey(clip.filePath, clip.id, manualKey);
    if (currentThreadIsGuiThread() && !speakerFramingRuntimeBuildAllowed()) {
        AssignedContinuityStreamsPtr cachedStreams;
        if (cachedAssignedContinuityStreamsMemoryOnly(cacheKey, &cachedStreams)) {
            const auto& streams =
                cachedStreams ? *cachedStreams : QVector<jcut::facedetections::FacestreamTrack>{};
            for (const jcut::facedetections::FacestreamTrack& stream : streams) {
                if (streamSampleAtFrame(clip,
                                        stream,
                                        timelineFramePosition,
                                        mediaSourceFramePosition,
                                        clip.speakerFramingCenterSmoothingFrames,
                                        clip.speakerFramingZoomSmoothingFrames,
                                        clip.speakerFramingSmoothingMode,
                                        clip.speakerFramingCenterSmoothingStrength,
                                        clip.speakerFramingZoomSmoothingStrength,
                                        clip.speakerFramingGapHoldFrames,
                                        locationOut,
                                        boxSizeOut)) {
                    return true;
                }
            }
            return false;
        }
        return false;
    }

    const QString transcriptPath = transcriptPathForRuntimeSidecarForClip(clip);
    if (transcriptPath.trimmed().isEmpty()) {
        return false;
    }
    editor::TranscriptEngine engine;
    const QString rawArtifactPath = engine.facedetectionsArtifactPath(transcriptPath);
    const QString processedPath = engine.facedetectionsProcessedArtifactPath(transcriptPath);

    AssignedContinuityStreamsPtr cachedStreams;
    if (cachedAssignedContinuityStreamsPtr(
            cacheKey, transcriptPath, processedPath, &cachedStreams)) {
        const auto& streams =
            cachedStreams ? *cachedStreams : QVector<jcut::facedetections::FacestreamTrack>{};
        for (const jcut::facedetections::FacestreamTrack& stream : streams) {
            if (streamSampleAtFrame(clip,
                                    stream,
                                    timelineFramePosition,
                                    mediaSourceFramePosition,
                                    clip.speakerFramingCenterSmoothingFrames,
                                    clip.speakerFramingZoomSmoothingFrames,
                                    clip.speakerFramingSmoothingMode,
                                    clip.speakerFramingCenterSmoothingStrength,
                                    clip.speakerFramingZoomSmoothingStrength,
                                    clip.speakerFramingGapHoldFrames,
                                    locationOut,
                                    boxSizeOut)) {
                return true;
            }
        }
        return false;
    }

    QJsonObject continuityRoot;
    QStringList referencedPaths;
    appendReferencedArtifactPath(&referencedPaths, QJsonObject{{QStringLiteral("path"), rawArtifactPath}}, QStringLiteral("path"));

    if (QFileInfo(rawArtifactPath).size() <= kMaxSynchronousContinuityArtifactBytes) {
        QJsonObject rawRoot;
        if (engine.loadFacestreamArtifact(transcriptPath, &rawRoot)) {
            continuityRoot = continuityRootForClip(rawRoot, clip.id);
        }
    }

    if (continuityRoot.isEmpty()) {
        if (QFileInfo(processedPath).size() > kMaxSynchronousContinuityArtifactBytes) {
            storeAssignedContinuityStreams(
                cacheKey, transcriptPath, processedPath, referencedPaths, {});
            return false;
        }
        QJsonObject processedRoot;
        if (!engine.loadFacestreamProcessedArtifact(transcriptPath, &processedRoot)) {
            storeAssignedContinuityStreams(
                cacheKey, transcriptPath, processedPath, referencedPaths, {});
            return false;
        }
        continuityRoot = continuityRootForClip(processedRoot, clip.id);
    }
    appendReferencedArtifactPath(&referencedPaths, continuityRoot, QStringLiteral("source_raw_artifact_path"));
    appendReferencedArtifactPath(&referencedPaths, continuityRoot, QStringLiteral("processed_artifact_path"));
    appendReferencedArtifactPath(&referencedPaths, continuityRoot, QStringLiteral("raw_tracks_artifact_path"));
    appendReferencedArtifactPath(&referencedPaths, continuityRoot, QStringLiteral("raw_frames_artifact_path"));
    appendReferencedArtifactPath(&referencedPaths, continuityRoot, QStringLiteral("continuity_artifact_path"));

    QSet<int> trackIds;
    if (manualTrackId >= 0) {
        trackIds.insert(manualTrackId);
    }
    QSet<QString> streamIds;
    if (!manualStreamId.isEmpty()) {
        streamIds.insert(manualStreamId);
    }
    const QVector<jcut::facedetections::FacestreamTrack> matchingStreams =
        jcut::facedetections::continuityTrackModelsForAssignments(
            continuityRoot,
            trackIds,
            streamIds,
            QJsonObject{});
    storeAssignedContinuityStreams(
        cacheKey, transcriptPath, processedPath, referencedPaths, matchingStreams);

    for (const jcut::facedetections::FacestreamTrack& stream : matchingStreams) {
        if (streamSampleAtFrame(clip,
                                stream,
                                timelineFramePosition,
                                mediaSourceFramePosition,
                                clip.speakerFramingCenterSmoothingFrames,
                                clip.speakerFramingZoomSmoothingFrames,
                                clip.speakerFramingSmoothingMode,
                                clip.speakerFramingCenterSmoothingStrength,
                                clip.speakerFramingZoomSmoothingStrength,
                                clip.speakerFramingGapHoldFrames,
                                locationOut,
                                boxSizeOut)) {
            return true;
        }
    }
    return false;
}

bool nearlyEqual(qreal a, qreal b) {
    return std::abs(a - b) <= kGradingEpsilon;
}

QVector<QPointF> interpolatedGradingCurvePoints(const QVector<QPointF>& previous,
                                                const QVector<QPointF>& next,
                                                qreal t) {
    const QVector<QPointF> previousPoints = sanitizeGradingCurvePoints(previous);
    const QVector<QPointF> nextPoints = sanitizeGradingCurvePoints(next);
    if (previousPoints.size() != nextPoints.size()) {
        return previousPoints;
    }

    QVector<QPointF> blended;
    blended.reserve(previousPoints.size());
    for (int i = 0; i < previousPoints.size(); ++i) {
        if (!nearlyEqual(previousPoints.at(i).x(), nextPoints.at(i).x())) {
            return previousPoints;
        }
        blended.push_back(QPointF(previousPoints.at(i).x(),
                                  previousPoints.at(i).y() +
                                      ((nextPoints.at(i).y() - previousPoints.at(i).y()) * t)));
    }
    return sanitizeGradingCurvePoints(blended);
}

bool gradingCurvePointsMatch(const QVector<QPointF>& a, const QVector<QPointF>& b) {
    const QVector<QPointF> pointsA = sanitizeGradingCurvePoints(a);
    const QVector<QPointF> pointsB = sanitizeGradingCurvePoints(b);
    if (pointsA.size() != pointsB.size()) {
        return false;
    }
    for (int i = 0; i < pointsA.size(); ++i) {
        if (!nearlyEqual(pointsA.at(i).x(), pointsB.at(i).x()) ||
            !nearlyEqual(pointsA.at(i).y(), pointsB.at(i).y())) {
            return false;
        }
    }
    return true;
}

TimelineClip::GradingKeyframe interpolatedGradingKeyframe(const TimelineClip::GradingKeyframe& previous,
                                                          const TimelineClip::GradingKeyframe& next,
                                                          qreal t) {
    TimelineClip::GradingKeyframe out;
    out.brightness = previous.brightness + ((next.brightness - previous.brightness) * t);
    out.contrast = previous.contrast + ((next.contrast - previous.contrast) * t);
    out.saturation = previous.saturation + ((next.saturation - previous.saturation) * t);
    out.shadowsR = previous.shadowsR + ((next.shadowsR - previous.shadowsR) * t);
    out.shadowsG = previous.shadowsG + ((next.shadowsG - previous.shadowsG) * t);
    out.shadowsB = previous.shadowsB + ((next.shadowsB - previous.shadowsB) * t);
    out.midtonesR = previous.midtonesR + ((next.midtonesR - previous.midtonesR) * t);
    out.midtonesG = previous.midtonesG + ((next.midtonesG - previous.midtonesG) * t);
    out.midtonesB = previous.midtonesB + ((next.midtonesB - previous.midtonesB) * t);
    out.highlightsR = previous.highlightsR + ((next.highlightsR - previous.highlightsR) * t);
    out.highlightsG = previous.highlightsG + ((next.highlightsG - previous.highlightsG) * t);
    out.highlightsB = previous.highlightsB + ((next.highlightsB - previous.highlightsB) * t);
    out.curvePointsR = interpolatedGradingCurvePoints(previous.curvePointsR, next.curvePointsR, t);
    out.curvePointsG = interpolatedGradingCurvePoints(previous.curvePointsG, next.curvePointsG, t);
    out.curvePointsB = interpolatedGradingCurvePoints(previous.curvePointsB, next.curvePointsB, t);
    out.curvePointsLuma = interpolatedGradingCurvePoints(previous.curvePointsLuma, next.curvePointsLuma, t);
    out.curveThreePointLock = previous.curveThreePointLock;
    out.curveSmoothingEnabled = previous.curveSmoothingEnabled;
    return out;
}

bool gradingValuesMatch(const TimelineClip::GradingKeyframe& a,
                        const TimelineClip::GradingKeyframe& b) {
    return nearlyEqual(a.brightness, b.brightness) &&
           nearlyEqual(a.contrast, b.contrast) &&
           nearlyEqual(a.saturation, b.saturation) &&
           nearlyEqual(a.shadowsR, b.shadowsR) &&
           nearlyEqual(a.shadowsG, b.shadowsG) &&
           nearlyEqual(a.shadowsB, b.shadowsB) &&
           nearlyEqual(a.midtonesR, b.midtonesR) &&
           nearlyEqual(a.midtonesG, b.midtonesG) &&
           nearlyEqual(a.midtonesB, b.midtonesB) &&
           nearlyEqual(a.highlightsR, b.highlightsR) &&
           nearlyEqual(a.highlightsG, b.highlightsG) &&
           nearlyEqual(a.highlightsB, b.highlightsB) &&
           gradingCurvePointsMatch(a.curvePointsR, b.curvePointsR) &&
           gradingCurvePointsMatch(a.curvePointsG, b.curvePointsG) &&
           gradingCurvePointsMatch(a.curvePointsB, b.curvePointsB) &&
           gradingCurvePointsMatch(a.curvePointsLuma, b.curvePointsLuma) &&
           a.curveThreePointLock == b.curveThreePointLock &&
           a.curveSmoothingEnabled == b.curveSmoothingEnabled;
}

TimelineClip::TransformKeyframe normalizedSpeakerFramingTargetKeyframe(
    TimelineClip::TransformKeyframe keyframe)
{
    keyframe.translationX = qBound<qreal>(0.0, keyframe.translationX, 1.0);
    keyframe.translationY = qBound<qreal>(0.0, keyframe.translationY, 1.0);
    keyframe.rotation = 0.0;
    keyframe.scaleX = qBound<qreal>(-1.0, keyframe.scaleX, 1.0);
    keyframe.scaleY = keyframe.scaleX;
    return keyframe;
}

TimelineClip::TransformKeyframe clipBaseSpeakerFramingTarget(const TimelineClip& clip)
{
    Q_UNUSED(clip);
    TimelineClip::TransformKeyframe state;
    state.translationX = kDefaultSpeakerFramingTargetXNorm;
    state.translationY = kDefaultSpeakerFramingTargetYNorm;
    state.rotation = 0.0;
    state.scaleX = kDefaultSpeakerFramingTargetBoxNorm;
    state.scaleY = state.scaleX;
    state.linearInterpolation = true;
    return state;
}

TimelineClip::TransformKeyframe retargetSpeakerFramingTransform(
    const TimelineClip::TransformKeyframe& framingState,
    const TimelineClip::TransformKeyframe& targetState,
    const TimelineClip& clip,
    const QSize& outputSize)
{
    const qreal bakedTargetBox = qBound<qreal>(-1.0, clip.speakerFramingBakedTargetBoxNorm, 1.0);
    const qreal targetBoxNorm = qBound<qreal>(-1.0, targetState.scaleX, 1.0);
    if (targetBoxNorm <= 0.0 || bakedTargetBox <= 0.0) {
        return framingState;
    }

    const QSize safeOutput = outputSize.isValid() ? outputSize : QSize(1080, 1920);
    const qreal outputWidth = qMax<qreal>(1.0, static_cast<qreal>(safeOutput.width()));
    const qreal outputHeight = qMax<qreal>(1.0, static_cast<qreal>(safeOutput.height()));
    const qreal bakedTargetXPx =
        qBound<qreal>(0.0, clip.speakerFramingBakedTargetXNorm, 1.0) * outputWidth;
    const qreal bakedTargetYPx =
        qBound<qreal>(0.0, clip.speakerFramingBakedTargetYNorm, 1.0) * outputHeight;
    const qreal targetXPx = qBound<qreal>(0.0, targetState.translationX, 1.0) * outputWidth;
    const qreal targetYPx = qBound<qreal>(0.0, targetState.translationY, 1.0) * outputHeight;
    const QSize sourceSize = sourceSizeForClipCached(clip, safeOutput);
    const QRectF fittedRect = fitRectForSourceInOutput(sourceSize, safeOutput);
    const qreal fittedCenterX = fittedRect.center().x();
    const qreal fittedCenterY = fittedRect.center().y();
    const qreal scaleFactor = qMax<qreal>(0.01, targetBoxNorm / bakedTargetBox);

    TimelineClip::TransformKeyframe retargeted = framingState;
    retargeted.translationX =
        targetXPx - fittedCenterX -
        (scaleFactor * (bakedTargetXPx - fittedCenterX - framingState.translationX));
    retargeted.translationY =
        targetYPx - fittedCenterY -
        (scaleFactor * (bakedTargetYPx - fittedCenterY - framingState.translationY));
    retargeted.scaleX = sanitizeScaleValue(framingState.scaleX * scaleFactor);
    retargeted.scaleY = sanitizeScaleValue(framingState.scaleY * scaleFactor);
    return retargeted;
}

template <typename Keyframe, typename Frame>
int firstKeyframeAfterFrame(const QVector<Keyframe>& keyframes, Frame frame)
{
    const auto it = std::upper_bound(
        keyframes.constBegin(),
        keyframes.constEnd(),
        frame,
        [](Frame needle, const Keyframe& keyframe) {
            return needle < static_cast<Frame>(keyframe.frame);
        });
    return static_cast<int>(std::distance(keyframes.constBegin(), it));
}

TimelineClip::TransformKeyframe interpolatedSpeakerFramingTarget(
    const TimelineClip::TransformKeyframe& previous,
    const TimelineClip::TransformKeyframe& current,
    qreal localFrame)
{
    if (!current.linearInterpolation || current.frame <= previous.frame) {
        return previous;
    }
    const qreal t = qBound<qreal>(
        0.0,
        (localFrame - static_cast<qreal>(previous.frame)) /
            static_cast<qreal>(current.frame - previous.frame),
        1.0);
    TimelineClip::TransformKeyframe state;
    state.frame = qRound64(localFrame);
    state.translationX = previous.translationX + ((current.translationX - previous.translationX) * t);
    state.translationY = previous.translationY + ((current.translationY - previous.translationY) * t);
    state.rotation = 0.0;
    state.scaleX = qBound<qreal>(-1.0, previous.scaleX + ((current.scaleX - previous.scaleX) * t), 1.0);
    state.scaleY = state.scaleX;
    state.linearInterpolation = current.linearInterpolation;
    return state;
}

TimelineClip::TransformKeyframe interpolatedSpeakerFramingKeyframe(
    const TimelineClip::TransformKeyframe& previous,
    const TimelineClip::TransformKeyframe& current,
    qreal localFrame)
{
    if (!current.linearInterpolation || current.frame <= previous.frame) {
        return previous;
    }
    const qreal t = qBound<qreal>(
        0.0,
        (localFrame - static_cast<qreal>(previous.frame)) /
            static_cast<qreal>(current.frame - previous.frame),
        1.0);
    TimelineClip::TransformKeyframe state;
    state.frame = qRound64(localFrame);
    state.translationX = previous.translationX + ((current.translationX - previous.translationX) * t);
    state.translationY = previous.translationY + ((current.translationY - previous.translationY) * t);
    state.rotation = 0.0;
    state.scaleX = sanitizeScaleValue(previous.scaleX + ((current.scaleX - previous.scaleX) * t));
    state.scaleY = sanitizeScaleValue(previous.scaleY + ((current.scaleY - previous.scaleY) * t));
    state.linearInterpolation = current.linearInterpolation;
    return state;
}
} // namespace


qreal sanitizeScaleValue(qreal value) {
    if (std::abs(value) < 0.01) {
        return value < 0.0 ? -0.01 : 0.01;
    }
    return value;
}

void normalizeClipTransformKeyframes(TimelineClip& clip) {
    clip.baseScaleX = sanitizeScaleValue(clip.baseScaleX);
    clip.baseScaleY = sanitizeScaleValue(clip.baseScaleY);
    std::sort(clip.transformKeyframes.begin(), clip.transformKeyframes.end(),
              [](const TimelineClip::TransformKeyframe& a, const TimelineClip::TransformKeyframe& b) {
                  return a.frame < b.frame;
              });

    QVector<TimelineClip::TransformKeyframe> normalized;
    normalized.reserve(clip.transformKeyframes.size());
    const int64_t maxFrame = qMax<int64_t>(0, clip.durationFrames - 1);
    for (TimelineClip::TransformKeyframe keyframe : clip.transformKeyframes) {
        keyframe.frame = qBound<int64_t>(0, keyframe.frame, maxFrame);
        keyframe.scaleX = sanitizeScaleValue(keyframe.scaleX);
        keyframe.scaleY = sanitizeScaleValue(keyframe.scaleY);
        if (!normalized.isEmpty() && normalized.constLast().frame == keyframe.frame) {
            normalized.last() = keyframe;
        } else {
            normalized.push_back(keyframe);
        }
    }
    if (clipHasVisuals(clip)) {
        if (normalized.isEmpty()) {
            TimelineClip::TransformKeyframe keyframe;
            keyframe.frame = 0;
            normalized.push_back(keyframe);
        } else if (normalized.constFirst().frame > 0) {
            TimelineClip::TransformKeyframe firstKeyframe = normalized.constFirst();
            firstKeyframe.frame = 0;
            normalized.push_front(firstKeyframe);
        } else {
            normalized.first().frame = 0;
        }
    }
    clip.transformKeyframes = normalized;

    std::sort(clip.speakerFramingEnabledKeyframes.begin(),
              clip.speakerFramingEnabledKeyframes.end(),
              [](const TimelineClip::BoolKeyframe& a, const TimelineClip::BoolKeyframe& b) {
                  return a.frame < b.frame;
              });
    QVector<TimelineClip::BoolKeyframe> normalizedSpeakerFramingEnabled;
    normalizedSpeakerFramingEnabled.reserve(clip.speakerFramingEnabledKeyframes.size());
    for (TimelineClip::BoolKeyframe keyframe : clip.speakerFramingEnabledKeyframes) {
        keyframe.frame = qBound<int64_t>(0, keyframe.frame, maxFrame);
        if (!normalizedSpeakerFramingEnabled.isEmpty() &&
            normalizedSpeakerFramingEnabled.constLast().frame == keyframe.frame) {
            normalizedSpeakerFramingEnabled.last() = keyframe;
        } else {
            normalizedSpeakerFramingEnabled.push_back(keyframe);
        }
    }
    if (clipHasVisuals(clip) && !normalizedSpeakerFramingEnabled.isEmpty()) {
        if (normalizedSpeakerFramingEnabled.constFirst().frame > 0) {
            TimelineClip::BoolKeyframe firstKeyframe;
            firstKeyframe.frame = 0;
            firstKeyframe.enabled = clip.speakerFramingEnabled;
            normalizedSpeakerFramingEnabled.push_front(firstKeyframe);
        } else {
            normalizedSpeakerFramingEnabled.first().frame = 0;
        }
    }
    clip.speakerFramingEnabledKeyframes = normalizedSpeakerFramingEnabled;

    std::sort(clip.speakerFramingKeyframes.begin(), clip.speakerFramingKeyframes.end(),
              [](const TimelineClip::TransformKeyframe& a, const TimelineClip::TransformKeyframe& b) {
                  return a.frame < b.frame;
              });
    QVector<TimelineClip::TransformKeyframe> normalizedSpeakerFraming;
    normalizedSpeakerFraming.reserve(clip.speakerFramingKeyframes.size());
    for (TimelineClip::TransformKeyframe keyframe : clip.speakerFramingKeyframes) {
        keyframe.frame = qBound<int64_t>(0, keyframe.frame, maxFrame);
        keyframe.scaleX = sanitizeScaleValue(keyframe.scaleX);
        keyframe.scaleY = sanitizeScaleValue(keyframe.scaleY);
        keyframe.rotation = 0.0;
        if (!normalizedSpeakerFraming.isEmpty() &&
            normalizedSpeakerFraming.constLast().frame == keyframe.frame) {
            normalizedSpeakerFraming.last() = keyframe;
        } else {
            normalizedSpeakerFraming.push_back(keyframe);
        }
    }
    clip.speakerFramingKeyframes = normalizedSpeakerFraming;
    std::sort(clip.speakerFramingTargetKeyframes.begin(),
              clip.speakerFramingTargetKeyframes.end(),
              [](const TimelineClip::TransformKeyframe& a, const TimelineClip::TransformKeyframe& b) {
                  return a.frame < b.frame;
              });
    QVector<TimelineClip::TransformKeyframe> normalizedSpeakerFramingTargets;
    normalizedSpeakerFramingTargets.reserve(clip.speakerFramingTargetKeyframes.size());
    for (TimelineClip::TransformKeyframe keyframe : clip.speakerFramingTargetKeyframes) {
        keyframe.frame = qBound<int64_t>(0, keyframe.frame, maxFrame);
        keyframe = normalizedSpeakerFramingTargetKeyframe(keyframe);
        if (!normalizedSpeakerFramingTargets.isEmpty() &&
            normalizedSpeakerFramingTargets.constLast().frame == keyframe.frame) {
            normalizedSpeakerFramingTargets.last() = keyframe;
        } else {
            normalizedSpeakerFramingTargets.push_back(keyframe);
        }
    }
    if (clipHasVisuals(clip) && !normalizedSpeakerFramingTargets.isEmpty()) {
        if (normalizedSpeakerFramingTargets.constFirst().frame > 0) {
            TimelineClip::TransformKeyframe firstKeyframe = clipBaseSpeakerFramingTarget(clip);
            firstKeyframe.frame = 0;
            normalizedSpeakerFramingTargets.push_front(firstKeyframe);
        } else {
            normalizedSpeakerFramingTargets.first().frame = 0;
        }
    }
    clip.speakerFramingTargetKeyframes = normalizedSpeakerFramingTargets;
    clip.speakerFramingBakedTargetXNorm = qBound<qreal>(0.0, clip.speakerFramingBakedTargetXNorm, 1.0);
    clip.speakerFramingBakedTargetYNorm = qBound<qreal>(0.0, clip.speakerFramingBakedTargetYNorm, 1.0);
    clip.speakerFramingBakedTargetBoxNorm = qBound<qreal>(-1.0, clip.speakerFramingBakedTargetBoxNorm, 1.0);
    clip.speakerFramingMinConfidence = qBound<qreal>(0.0, clip.speakerFramingMinConfidence, 1.0);
    clip.speakerFramingCenterSmoothingFrames =
        qBound(0,
               clip.speakerFramingCenterSmoothingFrames,
               TimelineClip::kSpeakerFramingSmoothingMaxFrames);
    clip.speakerFramingZoomSmoothingFrames =
        qBound(0,
               clip.speakerFramingZoomSmoothingFrames,
               TimelineClip::kSpeakerFramingSmoothingMaxFrames);
    clip.speakerFramingSmoothingMode = qBound(0, clip.speakerFramingSmoothingMode, 2);
    clip.speakerFramingCenterSmoothingStrength =
        qBound<qreal>(
            0.0,
            clip.speakerFramingCenterSmoothingStrength,
            TimelineClip::kSpeakerFramingSmoothingStrengthMax);
    clip.speakerFramingZoomSmoothingStrength =
        qBound<qreal>(
            0.0,
            clip.speakerFramingZoomSmoothingStrength,
            TimelineClip::kSpeakerFramingSmoothingStrengthMax);
    clip.speakerFramingGapHoldFrames =
        qBound(0, clip.speakerFramingGapHoldFrames, TimelineClip::kSpeakerFramingGapHoldMaxFrames);
    clip.speakerSectionMinimumWords = qBound(0, clip.speakerSectionMinimumWords, 1000);
}

void normalizeClipGradingKeyframes(TimelineClip& clip) {
    std::sort(clip.gradingKeyframes.begin(), clip.gradingKeyframes.end(),
              [](const TimelineClip::GradingKeyframe& a, const TimelineClip::GradingKeyframe& b) {
                  return a.frame < b.frame;
              });

    QVector<TimelineClip::GradingKeyframe> normalized;
    normalized.reserve(clip.gradingKeyframes.size());
    const int64_t maxFrame = qMax<int64_t>(0, clip.durationFrames - 1);
    for (TimelineClip::GradingKeyframe keyframe : clip.gradingKeyframes) {
        keyframe.frame = qBound<int64_t>(0, keyframe.frame, maxFrame);
        keyframe.curvePointsR = sanitizeGradingCurvePoints(keyframe.curvePointsR);
        keyframe.curvePointsG = sanitizeGradingCurvePoints(keyframe.curvePointsG);
        keyframe.curvePointsB = sanitizeGradingCurvePoints(keyframe.curvePointsB);
        keyframe.curvePointsLuma = sanitizeGradingCurvePoints(keyframe.curvePointsLuma);
        if (!normalized.isEmpty() && normalized.constLast().frame == keyframe.frame) {
            normalized.last() = keyframe;
        } else {
            normalized.push_back(keyframe);
        }
    }

    if (clipHasVisuals(clip)) {
        if (normalized.isEmpty()) {
            TimelineClip::GradingKeyframe keyframe;
            keyframe.frame = 0;
            keyframe.brightness = clip.brightness;
            keyframe.contrast = clip.contrast;
            keyframe.saturation = clip.saturation;
            keyframe.curvePointsR = defaultGradingCurvePoints();
            keyframe.curvePointsG = defaultGradingCurvePoints();
            keyframe.curvePointsB = defaultGradingCurvePoints();
            keyframe.curvePointsLuma = defaultGradingCurvePoints();
            keyframe.curveSmoothingEnabled = true;
            normalized.push_back(keyframe);
        } else if (normalized.constFirst().frame > 0) {
            // FIX: Create a keyframe at frame 0 with clip's base values
            // instead of duplicating the first keyframe
            TimelineClip::GradingKeyframe baseKeyframe;
            baseKeyframe.frame = 0;
            baseKeyframe.brightness = clip.brightness;
            baseKeyframe.contrast = clip.contrast;
            baseKeyframe.saturation = clip.saturation;
            baseKeyframe.curvePointsR = defaultGradingCurvePoints();
            baseKeyframe.curvePointsG = defaultGradingCurvePoints();
            baseKeyframe.curvePointsB = defaultGradingCurvePoints();
            baseKeyframe.curvePointsLuma = defaultGradingCurvePoints();
            baseKeyframe.curveSmoothingEnabled = true;
            // Use default values for shadows/midtones/highlights (0.0)
            // This allows proper interpolation from frame 0 to first keyframe
            normalized.push_front(baseKeyframe);
        } else {
            normalized.first().frame = 0;
        }
    }

    // Legacy cleanup: older builds stored opacity edits as grading keyframes too.
    // Drop grading keys that sit on opacity-key frames and do not change grading
    // relative to linear interpolation between surrounding grading keys.
    if (normalized.size() >= 3 && !clip.opacityKeyframes.isEmpty()) {
        QSet<int64_t> opacityFrames;
        opacityFrames.reserve(clip.opacityKeyframes.size());
        for (const TimelineClip::OpacityKeyframe& opacityKeyframe : clip.opacityKeyframes) {
            opacityFrames.insert(opacityKeyframe.frame);
        }

        for (int i = normalized.size() - 2; i >= 1; --i) {
            const TimelineClip::GradingKeyframe& current = normalized[i];
            if (!opacityFrames.contains(current.frame) || !current.linearInterpolation) {
                continue;
            }

            const TimelineClip::GradingKeyframe& previous = normalized[i - 1];
            const TimelineClip::GradingKeyframe& next = normalized[i + 1];
            const int64_t span = next.frame - previous.frame;
            if (span <= 0 || !next.linearInterpolation) {
                continue;
            }

            const qreal t = static_cast<qreal>(current.frame - previous.frame) /
                            static_cast<qreal>(span);
            const TimelineClip::GradingKeyframe expected = interpolatedGradingKeyframe(previous, next, t);
            if (gradingValuesMatch(current, expected)) {
                normalized.removeAt(i);
            }
        }
    }

    clip.gradingKeyframes = normalized;
    if (!clip.gradingKeyframes.isEmpty()) {
        clip.brightness = clip.gradingKeyframes.constFirst().brightness;
        clip.contrast = clip.gradingKeyframes.constFirst().contrast;
        clip.saturation = clip.gradingKeyframes.constFirst().saturation;
    }
}

void normalizeClipOpacityKeyframes(TimelineClip& clip) {
    std::sort(clip.opacityKeyframes.begin(), clip.opacityKeyframes.end(),
              [](const TimelineClip::OpacityKeyframe& a, const TimelineClip::OpacityKeyframe& b) {
                  return a.frame < b.frame;
              });

    QVector<TimelineClip::OpacityKeyframe> normalized;
    normalized.reserve(clip.opacityKeyframes.size());
    const int64_t maxFrame = qMax<int64_t>(0, clip.durationFrames - 1);
    for (TimelineClip::OpacityKeyframe keyframe : clip.opacityKeyframes) {
        keyframe.frame = qBound<int64_t>(0, keyframe.frame, maxFrame);
        keyframe.opacity = qBound<qreal>(0.0, keyframe.opacity, 1.0);
        if (!normalized.isEmpty() && normalized.constLast().frame == keyframe.frame) {
            normalized.last() = keyframe;
        } else {
            normalized.push_back(keyframe);
        }
    }

    if (clipHasVisuals(clip)) {
        if (normalized.isEmpty()) {
            TimelineClip::OpacityKeyframe keyframe;
            keyframe.frame = 0;
            keyframe.opacity = clip.opacity;
            normalized.push_back(keyframe);
        } else if (normalized.constFirst().frame > 0) {
            TimelineClip::OpacityKeyframe firstKeyframe = normalized.constFirst();
            firstKeyframe.frame = 0;
            normalized.push_front(firstKeyframe);
        } else {
            normalized.first().frame = 0;
        }
    }

    clip.opacityKeyframes = normalized;
    if (!clip.opacityKeyframes.isEmpty()) {
        clip.opacity = clip.opacityKeyframes.constFirst().opacity;
    }
}

void normalizeClipTitleKeyframes(TimelineClip& clip) {
    std::sort(clip.titleKeyframes.begin(), clip.titleKeyframes.end(),
              [](const TimelineClip::TitleKeyframe& a, const TimelineClip::TitleKeyframe& b) {
                  return a.frame < b.frame;
              });

    QVector<TimelineClip::TitleKeyframe> normalized;
    normalized.reserve(clip.titleKeyframes.size());
    const int64_t maxFrame = qMax<int64_t>(0, clip.durationFrames - 1);
    for (TimelineClip::TitleKeyframe keyframe : clip.titleKeyframes) {
        keyframe.frame = qBound<int64_t>(0, keyframe.frame, maxFrame);
        if (!normalized.isEmpty() && normalized.constLast().frame == keyframe.frame) {
            normalized.last() = keyframe;
        } else {
            normalized.push_back(keyframe);
        }
    }
    clip.titleKeyframes = normalized;
}

TimelineClip::TransformKeyframe evaluateClipKeyframeOffsetAtFrame(const TimelineClip& clip, int64_t timelineFrame) {
    TimelineClip::TransformKeyframe state;
    if (clip.transformKeyframes.isEmpty()) {
        return state;
    }

    const int64_t localFrame = qBound<int64_t>(0, timelineFrame - clip.startFrame, qMax<int64_t>(0, clip.durationFrames - 1));
    if (localFrame <= clip.transformKeyframes.constFirst().frame) {
        return clip.transformKeyframes.constFirst();
    }

    for (int i = 1; i < clip.transformKeyframes.size(); ++i) {
        const TimelineClip::TransformKeyframe& previous = clip.transformKeyframes[i - 1];
        const TimelineClip::TransformKeyframe& current = clip.transformKeyframes[i];
        if (localFrame < current.frame) {
            if (!current.linearInterpolation || current.frame <= previous.frame) {
                return previous;
            }
            const qreal t = qBound<qreal>(0.0,
                                          interpolationFactorForTransformFrames(
                                              clip,
                                              static_cast<qreal>(previous.frame),
                                              static_cast<qreal>(current.frame),
                                              static_cast<qreal>(localFrame)),
                                          1.0);
            state.frame = localFrame;
            state.translationX = previous.translationX + ((current.translationX - previous.translationX) * t);
            state.translationY = previous.translationY + ((current.translationY - previous.translationY) * t);
            state.rotation = previous.rotation + ((current.rotation - previous.rotation) * t);
            state.scaleX = previous.scaleX + ((current.scaleX - previous.scaleX) * t);
            state.scaleY = previous.scaleY + ((current.scaleY - previous.scaleY) * t);
            state.linearInterpolation = current.linearInterpolation;
            return state;
        }
        if (localFrame == current.frame) {
            return current;
        }
    }

    return clip.transformKeyframes.constLast();
}

TimelineClip::TransformKeyframe evaluateClipTransformAtFrame(const TimelineClip& clip, int64_t timelineFrame) {
    TimelineClip::TransformKeyframe effective = evaluateClipKeyframeOffsetAtFrame(clip, timelineFrame);
    effective.translationX += clip.baseTranslationX;
    effective.translationY += clip.baseTranslationY;
    effective.rotation += clip.baseRotation;
    effective.scaleX = sanitizeScaleValue(clip.baseScaleX * effective.scaleX);
    effective.scaleY = sanitizeScaleValue(clip.baseScaleY * effective.scaleY);
    return effective;
}

TimelineClip::TransformKeyframe evaluateClipTransformAtPosition(const TimelineClip& clip, qreal timelineFramePosition) {
    TimelineClip::TransformKeyframe state;
    if (clip.transformKeyframes.isEmpty()) {
        state.translationX = clip.baseTranslationX;
        state.translationY = clip.baseTranslationY;
        state.rotation = clip.baseRotation;
        state.scaleX = sanitizeScaleValue(clip.baseScaleX);
        state.scaleY = sanitizeScaleValue(clip.baseScaleY);
        return state;
    }

    const qreal maxFrame = static_cast<qreal>(qMax<int64_t>(0, clip.durationFrames - 1));
    const qreal localFrame = qBound<qreal>(0.0, timelineFramePosition - static_cast<qreal>(clip.startFrame), maxFrame);
    if (localFrame <= static_cast<qreal>(clip.transformKeyframes.constFirst().frame)) {
        state = clip.transformKeyframes.constFirst();
    } else {
        state = clip.transformKeyframes.constLast();
        for (int i = 1; i < clip.transformKeyframes.size(); ++i) {
            const TimelineClip::TransformKeyframe& previous = clip.transformKeyframes[i - 1];
            const TimelineClip::TransformKeyframe& current = clip.transformKeyframes[i];
            if (localFrame < static_cast<qreal>(current.frame)) {
                if (!current.linearInterpolation || current.frame <= previous.frame) {
                    state = previous;
                } else {
                    const qreal t = qBound<qreal>(0.0,
                                                  interpolationFactorForTransformFrames(
                                                      clip,
                                                      static_cast<qreal>(previous.frame),
                                                      static_cast<qreal>(current.frame),
                                                      localFrame),
                                                  1.0);
                    state.frame = qRound64(localFrame);
                    state.translationX = previous.translationX + ((current.translationX - previous.translationX) * t);
                    state.translationY = previous.translationY + ((current.translationY - previous.translationY) * t);
                    state.rotation = previous.rotation + ((current.rotation - previous.rotation) * t);
                    state.scaleX = previous.scaleX + ((current.scaleX - previous.scaleX) * t);
                    state.scaleY = previous.scaleY + ((current.scaleY - previous.scaleY) * t);
                    state.linearInterpolation = current.linearInterpolation;
                }
                break;
            }
            if (qFuzzyCompare(localFrame + 1.0, static_cast<qreal>(current.frame) + 1.0)) {
                state = current;
                break;
            }
        }
    }

    state.translationX += clip.baseTranslationX;
    state.translationY += clip.baseTranslationY;
    state.rotation += clip.baseRotation;
    state.scaleX = sanitizeScaleValue(clip.baseScaleX * state.scaleX);
    state.scaleY = sanitizeScaleValue(clip.baseScaleY * state.scaleY);
    return state;
}

TimelineClip::TransformKeyframe evaluateClipSpeakerFramingTargetAtFrame(const TimelineClip& clip,
                                                                        int64_t timelineFrame) {
    if (clip.speakerFramingTargetKeyframes.isEmpty()) {
        return clipBaseSpeakerFramingTarget(clip);
    }

    const int64_t localFrame = qBound<int64_t>(
        0, timelineFrame - clip.startFrame, qMax<int64_t>(0, clip.durationFrames - 1));
    if (localFrame <= clip.speakerFramingTargetKeyframes.constFirst().frame) {
        return clip.speakerFramingTargetKeyframes.constFirst();
    }

    const int upperIndex = firstKeyframeAfterFrame(clip.speakerFramingTargetKeyframes, localFrame);
    if (upperIndex <= 0) {
        return clip.speakerFramingTargetKeyframes.constFirst();
    }
    if (upperIndex >= clip.speakerFramingTargetKeyframes.size()) {
        return clip.speakerFramingTargetKeyframes.constLast();
    }
    const TimelineClip::TransformKeyframe& previous = clip.speakerFramingTargetKeyframes.at(upperIndex - 1);
    const TimelineClip::TransformKeyframe& current = clip.speakerFramingTargetKeyframes.at(upperIndex);
    return previous.frame == localFrame ? previous : interpolatedSpeakerFramingTarget(previous, current, localFrame);
}

TimelineClip::TransformKeyframe evaluateClipSpeakerFramingTargetAtPosition(const TimelineClip& clip,
                                                                           qreal timelineFramePosition) {
    if (clip.speakerFramingTargetKeyframes.isEmpty()) {
        return clipBaseSpeakerFramingTarget(clip);
    }

    const qreal maxFrame = static_cast<qreal>(qMax<int64_t>(0, clip.durationFrames - 1));
    const qreal localFrame = qBound<qreal>(
        0.0, timelineFramePosition - static_cast<qreal>(clip.startFrame), maxFrame);
    if (localFrame <= static_cast<qreal>(clip.speakerFramingTargetKeyframes.constFirst().frame)) {
        return clip.speakerFramingTargetKeyframes.constFirst();
    }

    const int upperIndex = firstKeyframeAfterFrame(clip.speakerFramingTargetKeyframes, localFrame);
    if (upperIndex <= 0) {
        return clip.speakerFramingTargetKeyframes.constFirst();
    }
    if (upperIndex >= clip.speakerFramingTargetKeyframes.size()) {
        return clip.speakerFramingTargetKeyframes.constLast();
    }
    const TimelineClip::TransformKeyframe& previous = clip.speakerFramingTargetKeyframes.at(upperIndex - 1);
    const TimelineClip::TransformKeyframe& current = clip.speakerFramingTargetKeyframes.at(upperIndex);
    return qFuzzyCompare(localFrame + 1.0, static_cast<qreal>(previous.frame) + 1.0)
        ? previous
        : interpolatedSpeakerFramingTarget(previous, current, localFrame);
}

bool evaluateClipSpeakerFramingEnabledAtFrame(const TimelineClip& clip, int64_t timelineFrame)
{
    if (clip.speakerFramingEnabledKeyframes.isEmpty()) {
        return clip.speakerFramingEnabled;
    }

    const int64_t localFrame = qBound<int64_t>(
        0, timelineFrame - clip.startFrame, qMax<int64_t>(0, clip.durationFrames - 1));
    const int upperIndex = firstKeyframeAfterFrame(clip.speakerFramingEnabledKeyframes, localFrame);
    if (upperIndex <= 0) {
        return clip.speakerFramingEnabledKeyframes.constFirst().enabled;
    }
    return clip.speakerFramingEnabledKeyframes.at(upperIndex - 1).enabled;
}

bool evaluateClipSpeakerFramingEnabledAtPosition(const TimelineClip& clip, qreal timelineFramePosition)
{
    return evaluateClipSpeakerFramingEnabledAtFrame(
        clip, static_cast<int64_t>(std::floor(timelineFramePosition)));
}

TimelineClip::TransformKeyframe evaluateClipSpeakerFramingForFaceBoxAtPosition(
    const TimelineClip& clip,
    qreal timelineFramePosition,
    const QPointF& locationNorm,
    qreal boxSizeNorm,
    qreal rotationDegrees,
    const QSize& outputSize,
    QJsonObject* diagnosticsOut)
{
    TimelineClip::TransformKeyframe state;
    const qreal boundedRotation = qBound<qreal>(-180.0, rotationDegrees, 180.0);
    state.rotation = boundedRotation;
    state.scaleX = 1.0;
    state.scaleY = 1.0;

    const qreal maxFrame = static_cast<qreal>(qMax<int64_t>(0, clip.durationFrames - 1));
    const qreal localFrame = qBound<qreal>(
        0.0, timelineFramePosition - static_cast<qreal>(clip.startFrame), maxFrame);
    const TimelineClip::TransformKeyframe target =
        evaluateClipSpeakerFramingTargetAtPosition(clip, timelineFramePosition);
    const qreal targetXNorm = qBound<qreal>(0.0, target.translationX, 1.0);
    const qreal targetYNorm = qBound<qreal>(0.0, target.translationY, 1.0);
    const qreal targetBoxNorm = qBound<qreal>(-1.0, target.scaleX, 1.0);
    const qreal safeBoxSizeNorm = qBound<qreal>(0.0, boxSizeNorm, 1.0);
    if (targetBoxNorm <= 0.0 || safeBoxSizeNorm <= 0.0) {
        return state;
    }

    const QSize safeOutput = outputSize.isValid() ? outputSize : QSize(1080, 1920);
    const QSize sourceSize = sourceSizeForClipCached(clip, safeOutput);
    const QRectF fittedRect = fitRectForSourceInOutput(sourceSize, safeOutput);
    const qreal fittedCenterX = fittedRect.center().x();
    const qreal fittedCenterY = fittedRect.center().y();
    const qreal fittedWidth = qMax<qreal>(1.0, fittedRect.width());
    const qreal fittedHeight = qMax<qreal>(1.0, fittedRect.height());
    const qreal fittedMinSide = qMax<qreal>(1.0, qMin(fittedWidth, fittedHeight));
    const qreal outputWidth = qMax<qreal>(1.0, static_cast<qreal>(safeOutput.width()));
    const qreal outputHeight = qMax<qreal>(1.0, static_cast<qreal>(safeOutput.height()));
    const qreal outputMinSide = qMax<qreal>(1.0, qMin(outputWidth, outputHeight));
    const qreal targetXPx = targetXNorm * outputWidth;
    const qreal targetYPx = targetYNorm * outputHeight;
    const qreal faceSideOutputPx = qMax<qreal>(1.0, safeBoxSizeNorm * fittedMinSide);
    const qreal targetSideOutputPx = qMax<qreal>(1.0, targetBoxNorm * outputMinSide);
    const qreal scale = sanitizeScaleValue(targetSideOutputPx / faceSideOutputPx);
    const qreal localX = (qBound<qreal>(0.0, locationNorm.x(), 1.0) - 0.5) * fittedWidth;
    const qreal localY = (qBound<qreal>(0.0, locationNorm.y(), 1.0) - 0.5) * fittedHeight;
    constexpr qreal kPi = 3.141592653589793238462643383279502884;
    const qreal radians = boundedRotation * (kPi / 180.0);
    const qreal scaledLocalX = scale * localX;
    const qreal scaledLocalY = scale * localY;
    const qreal rotatedLocalX = (std::cos(radians) * scaledLocalX) -
                                (std::sin(radians) * scaledLocalY);
    const qreal rotatedLocalY = (std::sin(radians) * scaledLocalX) +
                                (std::cos(radians) * scaledLocalY);
    state.frame = static_cast<int64_t>(std::floor(localFrame));
    state.translationX = targetXPx - (fittedCenterX + rotatedLocalX);
    state.translationY = targetYPx - (fittedCenterY + rotatedLocalY);
    state.rotation = boundedRotation;
    state.scaleX = scale;
    state.scaleY = scale;
    state.linearInterpolation = true;
    if (diagnosticsOut) {
        diagnosticsOut->insert(QStringLiteral("target_box_norm"), QJsonObject{
            {QStringLiteral("x"), targetXNorm},
            {QStringLiteral("y"), targetYNorm},
            {QStringLiteral("box"), targetBoxNorm}
        });
        diagnosticsOut->insert(QStringLiteral("sampled_face_box_norm"), QJsonObject{
            {QStringLiteral("x"), qBound<qreal>(0.0, locationNorm.x(), 1.0)},
            {QStringLiteral("y"), qBound<qreal>(0.0, locationNorm.y(), 1.0)},
            {QStringLiteral("box"), safeBoxSizeNorm}
        });
        diagnosticsOut->insert(QStringLiteral("output_size"), QJsonObject{
            {QStringLiteral("width"), safeOutput.width()},
            {QStringLiteral("height"), safeOutput.height()}
        });
        diagnosticsOut->insert(QStringLiteral("source_size"), QJsonObject{
            {QStringLiteral("width"), sourceSize.width()},
            {QStringLiteral("height"), sourceSize.height()}
        });
        diagnosticsOut->insert(QStringLiteral("fitted_rect"), QJsonObject{
            {QStringLiteral("x"), fittedRect.x()},
            {QStringLiteral("y"), fittedRect.y()},
            {QStringLiteral("width"), fittedRect.width()},
            {QStringLiteral("height"), fittedRect.height()}
        });
        diagnosticsOut->insert(QStringLiteral("speaker_framing_transform"),
                               transformKeyframeDiagnosticObject(state));
    }
    return state;
}

void warmClipSpeakerFramingContinuityRuntimeSync(const TimelineClip& clip)
{
    ScopedSpeakerFramingRuntimeBuild allowBuild;
    if (!clipHasVisuals(clip) ||
        !clip.speakerFramingEnabled ||
        !clip.speakerFramingKeyframes.isEmpty() ||
        clip.id.trimmed().isEmpty() ||
        clip.filePath.trimmed().isEmpty()) {
        return;
    }

    const QString transcriptPath = transcriptPathForRuntimeSidecarForClip(clip);
    if (transcriptPath.trimmed().isEmpty()) {
        warmManualContinuityForClip(clip);
        return;
    }
    const std::shared_ptr<const TranscriptRuntimeDocument> runtimeDocument =
        loadTranscriptRuntimeDocument(transcriptPath);
    QSet<QString> speakerIds;
    if (runtimeDocument) {
        for (const TranscriptSection& section : runtimeDocument->sections) {
            for (const TranscriptWord& word : section.words) {
                const QString speakerId = word.speaker.trimmed();
                if (!speakerId.isEmpty()) {
                    speakerIds.insert(speakerId);
                }
            }
        }
    }
    editor::TranscriptEngine engine;
    QJsonDocument transcriptDoc;
    if (!engine.loadTranscriptJson(transcriptPath, &transcriptDoc)) {
        for (const QString& speakerId : std::as_const(speakerIds)) {
            QPointF ignoredLocation;
            qreal ignoredBoxSize = -1.0;
            (void)transcriptSpeakerTrackingSampleForClipFileAtSourceFrame(
                clip.filePath,
                speakerId,
                0,
                clip.speakerFramingMinConfidence,
                &ignoredLocation,
                &ignoredBoxSize);
        }
        warmManualContinuityForClip(clip);
        return;
    }
    const QJsonObject transcriptRoot = transcriptDoc.object();
    const QJsonArray sectionMap = sectionTrackMapForClip(transcriptRoot, clip.id);
    if (sectionMap.isEmpty()) {
        const QJsonArray identityMap =
            jcut::speakertrack::assignmentMapForClip(transcriptRoot, clip.id);
        for (const QJsonValue& value : identityMap) {
            const QString speakerId =
                value.toObject().value(QStringLiteral("identity_id")).toString().trimmed();
            if (!speakerId.isEmpty()) {
                speakerIds.insert(speakerId);
            }
        }
    }
    for (const QJsonValue& value : sectionMap) {
        const QJsonObject row = value.toObject();
        const QString speakerId = row.value(QStringLiteral("speaker_id")).toString().trimmed();
        if (speakerId.isEmpty()) {
            continue;
        }
        speakerIds.insert(speakerId);
        const int64_t startFrame = row.value(QStringLiteral("start_frame")).toInteger(0);
        warmAssignedContinuityForSpeakerAtTranscriptFrame(clip, speakerId, startFrame);
    }
    for (const QString& speakerId : std::as_const(speakerIds)) {
        QPointF ignoredLocation;
        qreal ignoredBoxSize = -1.0;
        (void)transcriptSpeakerTrackingSampleForClipFileAtSourceFrame(
            clip.filePath,
            speakerId,
            0,
            clip.speakerFramingMinConfidence,
            &ignoredLocation,
            &ignoredBoxSize);
    }
    if (sectionMap.isEmpty()) {
        for (const QString& speakerId : std::as_const(speakerIds)) {
            warmAssignedContinuityForSpeaker(clip, speakerId);
        }
    }
    warmManualContinuityForClip(clip);
}

QString speakerFramingWarmKeyForClip(const TimelineClip& clip)
{
    return QStringLiteral("speaker-framing-runtime:%1:%2:%3:%4:%5:%6")
        .arg(clip.filePath.trimmed())
        .arg(clip.id.trimmed())
        .arg(clip.speakerFramingManualTrackId)
        .arg(clip.speakerFramingManualStreamId.trimmed())
        .arg(clip.speakerFramingEnabled)
        .arg(clip.speakerFramingKeyframes.size());
}

void warmClipSpeakerFramingContinuityRuntime(const TimelineClip& clip)
{
    if (!clipHasVisuals(clip) ||
        !clip.speakerFramingEnabled ||
        !clip.speakerFramingKeyframes.isEmpty() ||
        clip.id.trimmed().isEmpty() ||
        clip.filePath.trimmed().isEmpty()) {
        return;
    }
    enqueueContinuityWarmJob(speakerFramingWarmKeyForClip(clip), [clip]() {
        warmClipSpeakerFramingContinuityRuntimeSync(clip);
    });
}

void warmClipsSpeakerFramingContinuityRuntime(const QVector<TimelineClip>& clips)
{
    for (const TimelineClip& clip : clips) {
        warmClipSpeakerFramingContinuityRuntime(clip);
    }
}

void prepareClipSpeakerFramingContinuityRuntimeBlocking(const TimelineClip& clip)
{
    warmClipSpeakerFramingContinuityRuntimeSync(clip);
}

TimelineClip::TransformKeyframe evaluateClipSpeakerFramingAtFrame(const TimelineClip& clip,
                                                                  int64_t timelineFrame,
                                                                  const QSize& outputSize) {
    return evaluateClipSpeakerFramingAtFrame(clip, timelineFrame, {}, outputSize);
}

TimelineClip::TransformKeyframe evaluateClipSpeakerFramingAtFrame(const TimelineClip& clip,
                                                                  int64_t timelineFrame,
                                                                  const QVector<RenderSyncMarker>& markers,
                                                                  const QSize& outputSize) {
    TimelineClip::TransformKeyframe state;
    state.rotation = 0.0;
    state.scaleX = 1.0;
    state.scaleY = 1.0;
    if (!evaluateClipSpeakerFramingEnabledAtFrame(clip, timelineFrame)) {
        return state;
    }

    const int64_t localFrame = qBound<int64_t>(
        0, timelineFrame - clip.startFrame, qMax<int64_t>(0, clip.durationFrames - 1));
    if (clip.speakerFramingKeyframes.isEmpty()) {
        const TimelineClip::TransformKeyframe target =
            evaluateClipSpeakerFramingTargetAtFrame(clip, timelineFrame);
        const qreal targetXNorm = qBound<qreal>(0.0, target.translationX, 1.0);
        const qreal targetYNorm = qBound<qreal>(0.0, target.translationY, 1.0);
        const qreal targetBoxNorm = qBound<qreal>(-1.0, target.scaleX, 1.0);
        if (targetBoxNorm <= 0.0) {
            return state;
        }
        const int64_t mediaSourceFrame =
            sourceFrameForClipAtTimelinePosition(clip, static_cast<qreal>(timelineFrame), markers);
        const qreal sourceFps = resolvedSourceFps(clip);
        const int64_t transcriptFrame = qMax<int64_t>(
            0,
            static_cast<int64_t>(std::floor(
                (static_cast<qreal>(mediaSourceFrame) / qMax<qreal>(1.0, sourceFps)) *
                static_cast<qreal>(kTimelineFps))));
        const int speakerGapHoldFrames =
            qBound(0, clip.speakerFramingGapHoldFrames, TimelineClip::kSpeakerFramingGapHoldMaxFrames);
        auto activeSpeakerAtFrame = [&](int64_t frame) {
            return currentThreadIsGuiThread()
                ? transcriptActiveSpeakerForClipFileAtSourceFrameMemoryOnly(clip.filePath, frame)
                : transcriptActiveSpeakerForClipFileAtSourceFrame(clip.filePath, frame);
        };
        auto speakerTrackingSample = [&](const QString& speakerId,
                                         QPointF* locationOut,
                                         qreal* boxSizeOut) {
            return currentThreadIsGuiThread()
                ? transcriptSpeakerTrackingSampleForClipFileAtSourceFrameMemoryOnly(
                      clip.filePath,
                      speakerId,
                      transcriptFrame,
                      clip.speakerFramingMinConfidence,
                      locationOut,
                      boxSizeOut)
                : transcriptSpeakerTrackingSampleForClipFileAtSourceFrame(
                      clip.filePath,
                      speakerId,
                      transcriptFrame,
                      clip.speakerFramingMinConfidence,
                      locationOut,
                      boxSizeOut);
        };
        QString activeSpeaker = activeSpeakerAtFrame(transcriptFrame);
        for (int offset = 1; activeSpeaker.isEmpty() && offset <= speakerGapHoldFrames; ++offset) {
            activeSpeaker = activeSpeakerAtFrame(qMax<int64_t>(0, transcriptFrame - offset));
            if (activeSpeaker.isEmpty()) {
                activeSpeaker = activeSpeakerAtFrame(transcriptFrame + offset);
            }
        }
        QPointF location;
        qreal boxSize = -1.0;
        qreal sectionRotationDegrees = 0.0;
        bool hasSample = false;
        if (!activeSpeaker.isEmpty()) {
            hasSample = assignedContinuityTrackSampleForSpeaker(
                clip,
                activeSpeaker,
                timelineFrame,
                mediaSourceFrame,
                &location,
                &boxSize,
                &sectionRotationDegrees,
                ContinuityAssignmentMode::SectionOnly);
        }
        if (!hasSample) {
            hasSample =
                manualContinuityTrackSampleForClip(clip, timelineFrame, mediaSourceFrame, &location, &boxSize);
        }
        if (!hasSample && !activeSpeaker.isEmpty()) {
            sectionRotationDegrees = 0.0;
            hasSample = assignedContinuityTrackSampleForSpeaker(
                clip,
                activeSpeaker,
                timelineFrame,
                mediaSourceFrame,
                &location,
                &boxSize,
                &sectionRotationDegrees,
                ContinuityAssignmentMode::SectionOrIdentity);
        }
        if (!hasSample && !activeSpeaker.isEmpty()) {
            sectionRotationDegrees = 0.0;
            hasSample = speakerTrackingSample(activeSpeaker, &location, &boxSize);
        }
        if (!hasSample && activeSpeaker.isEmpty() && !currentThreadIsGuiThread()) {
            QString profileSpeaker;
            hasSample = transcriptActiveSpeakerTrackingSampleForClipFileAtSourceFrame(
                clip.filePath,
                transcriptFrame,
                clip.speakerFramingMinConfidence,
                &location,
                &boxSize,
                &profileSpeaker);
        }
        if (!hasSample || boxSize <= 0.0) {
            return state;
        }
        return evaluateClipSpeakerFramingForFaceBoxAtPosition(
            clip,
            static_cast<qreal>(timelineFrame),
            location,
            boxSize,
            sectionRotationDegrees,
            outputSize);
    }
    TimelineClip::TransformKeyframe framingState;
    if (localFrame <= clip.speakerFramingKeyframes.constFirst().frame) {
        framingState = clip.speakerFramingKeyframes.constFirst();
    } else {
        const int upperIndex = firstKeyframeAfterFrame(clip.speakerFramingKeyframes, localFrame);
        if (upperIndex >= clip.speakerFramingKeyframes.size()) {
            framingState = clip.speakerFramingKeyframes.constLast();
        } else {
            const TimelineClip::TransformKeyframe& previous = clip.speakerFramingKeyframes.at(upperIndex - 1);
            const TimelineClip::TransformKeyframe& current = clip.speakerFramingKeyframes.at(upperIndex);
            framingState = previous.frame == localFrame
                ? previous
                : interpolatedSpeakerFramingKeyframe(previous, current, static_cast<qreal>(localFrame));
        }
    }

    const TimelineClip::TransformKeyframe target =
        evaluateClipSpeakerFramingTargetAtFrame(clip, timelineFrame);
    return retargetSpeakerFramingTransform(framingState, target, clip, outputSize);
}

TimelineClip::TransformKeyframe evaluateClipSpeakerFramingAtPosition(const TimelineClip& clip,
                                                                     qreal timelineFramePosition,
                                                                     const QSize& outputSize) {
    return evaluateClipSpeakerFramingAtPosition(clip, timelineFramePosition, {}, outputSize);
}

TimelineClip::TransformKeyframe evaluateClipSpeakerFramingAtPosition(const TimelineClip& clip,
                                                                     qreal timelineFramePosition,
                                                                     const QVector<RenderSyncMarker>& markers,
                                                                     const QSize& outputSize,
                                                                     QJsonObject* diagnosticsOut) {
    TimelineClip::TransformKeyframe state;
    state.rotation = 0.0;
    state.scaleX = 1.0;
    state.scaleY = 1.0;
    if (diagnosticsOut) {
        diagnosticsOut->insert(QStringLiteral("clip_id"), clip.id);
        diagnosticsOut->insert(QStringLiteral("timeline_frame_position"), timelineFramePosition);
        diagnosticsOut->insert(QStringLiteral("speaker_framing_enabled"),
                               evaluateClipSpeakerFramingEnabledAtPosition(clip, timelineFramePosition));
    }
    if (!evaluateClipSpeakerFramingEnabledAtPosition(clip, timelineFramePosition)) {
        return state;
    }

    const qreal maxFrame = static_cast<qreal>(qMax<int64_t>(0, clip.durationFrames - 1));
    const qreal localFrame = qBound<qreal>(
        0.0, timelineFramePosition - static_cast<qreal>(clip.startFrame), maxFrame);
    if (clip.speakerFramingKeyframes.isEmpty()) {
        const TimelineClip::TransformKeyframe target =
            evaluateClipSpeakerFramingTargetAtPosition(clip, timelineFramePosition);
        const qreal targetXNorm = qBound<qreal>(0.0, target.translationX, 1.0);
        const qreal targetYNorm = qBound<qreal>(0.0, target.translationY, 1.0);
        const qreal targetBoxNorm = qBound<qreal>(-1.0, target.scaleX, 1.0);
        if (targetBoxNorm <= 0.0) {
            return state;
        }
        const qreal mediaSourceFramePosition =
            sourceFramePositionForClipAtTimelinePosition(clip, timelineFramePosition, markers);
        const int64_t mediaSourceFrame = qMax<int64_t>(
            0,
            static_cast<int64_t>(std::floor(mediaSourceFramePosition)));
        const qreal sourceFps = resolvedSourceFps(clip);
        const int64_t transcriptFrame = qMax<int64_t>(
            0,
            static_cast<int64_t>(std::floor(
                (mediaSourceFramePosition / qMax<qreal>(1.0, sourceFps)) *
                static_cast<qreal>(kTimelineFps))));
        if (diagnosticsOut) {
            diagnosticsOut->insert(QStringLiteral("media_source_frame_position"), mediaSourceFramePosition);
            diagnosticsOut->insert(QStringLiteral("media_source_frame"), static_cast<qint64>(mediaSourceFrame));
            diagnosticsOut->insert(QStringLiteral("transcript_frame"), static_cast<qint64>(transcriptFrame));
        }
        const int speakerGapHoldFrames =
            qBound(0, clip.speakerFramingGapHoldFrames, TimelineClip::kSpeakerFramingGapHoldMaxFrames);
        auto activeSpeakerAtFrame = [&](int64_t frame) {
            return currentThreadIsGuiThread()
                ? transcriptActiveSpeakerForClipFileAtSourceFrameMemoryOnly(clip.filePath, frame)
                : transcriptActiveSpeakerForClipFileAtSourceFrame(clip.filePath, frame);
        };
        auto speakerTrackingSample = [&](const QString& speakerId,
                                         QPointF* locationOut,
                                         qreal* boxSizeOut) {
            return currentThreadIsGuiThread()
                ? transcriptSpeakerTrackingSampleForClipFileAtSourceFrameMemoryOnly(
                      clip.filePath,
                      speakerId,
                      transcriptFrame,
                      clip.speakerFramingMinConfidence,
                      locationOut,
                      boxSizeOut)
                : transcriptSpeakerTrackingSampleForClipFileAtSourceFrame(
                      clip.filePath,
                      speakerId,
                      transcriptFrame,
                      clip.speakerFramingMinConfidence,
                      locationOut,
                      boxSizeOut);
        };
        QString activeSpeaker = activeSpeakerAtFrame(transcriptFrame);
        for (int offset = 1; activeSpeaker.isEmpty() && offset <= speakerGapHoldFrames; ++offset) {
            activeSpeaker = activeSpeakerAtFrame(qMax<int64_t>(0, transcriptFrame - offset));
            if (activeSpeaker.isEmpty()) {
                activeSpeaker = activeSpeakerAtFrame(transcriptFrame + offset);
            }
        }
        QPointF location;
        qreal boxSize = -1.0;
        qreal sectionRotationDegrees = 0.0;
        QSet<int> diagnosticAssignedTrackIds;
        QSet<QString> diagnosticAssignedStreamIds;
        QString diagnosticAssignmentCacheToken;
        qreal diagnosticSectionRotationDegrees = 0.0;
        if (diagnosticsOut && !activeSpeaker.isEmpty()) {
            const QString transcriptPath = transcriptPathForRuntimeSidecarForClip(clip);
            QJsonDocument transcriptDoc;
            if (!transcriptPath.trimmed().isEmpty() &&
                loadTranscriptJsonCached(transcriptPath, &transcriptDoc)) {
                const QJsonObject transcriptRoot = transcriptDoc.object();
                const bool matchedAssignment =
                    collectSectionTrackAssignmentsForSpeaker(transcriptRoot,
                                                             clip,
                                                             activeSpeaker,
                                                             mediaSourceFramePosition,
                                                             &diagnosticAssignedTrackIds,
                                                             &diagnosticAssignedStreamIds,
                                                             &diagnosticAssignmentCacheToken,
                                                             &diagnosticSectionRotationDegrees);
                diagnosticsOut->insert(QStringLiteral("section_mapping_active"),
                                       !sectionTrackMapForClip(transcriptRoot, clip.id).isEmpty());
                diagnosticsOut->insert(QStringLiteral("matched_section_assignment"), matchedAssignment);
                diagnosticsOut->insert(QStringLiteral("assigned_track_ids"),
                                       intSetDiagnosticArray(diagnosticAssignedTrackIds));
                diagnosticsOut->insert(QStringLiteral("assigned_stream_ids"),
                                       stringSetDiagnosticArray(diagnosticAssignedStreamIds));
                diagnosticsOut->insert(QStringLiteral("assignment_cache_token"),
                                       diagnosticAssignmentCacheToken);
                diagnosticsOut->insert(QStringLiteral("section_rotation_degrees"),
                                       diagnosticSectionRotationDegrees);
            }
        }
        if (diagnosticsOut) {
            diagnosticsOut->insert(QStringLiteral("active_speaker"), activeSpeaker);
        }
        bool hasSample = false;
        QString sampleSource;
        if (!activeSpeaker.isEmpty()) {
            hasSample = assignedContinuityTrackSampleForSpeaker(
                clip,
                activeSpeaker,
                timelineFramePosition,
                mediaSourceFramePosition,
                &location,
                &boxSize,
                &sectionRotationDegrees,
                ContinuityAssignmentMode::SectionOnly);
            if (hasSample) {
                sampleSource = QStringLiteral("section_assigned_continuity_track");
            }
        }
        if (!hasSample) {
            hasSample = manualContinuityTrackSampleForClip(
                clip, timelineFramePosition, mediaSourceFramePosition, &location, &boxSize);
            if (hasSample) {
                sampleSource = QStringLiteral("manual_continuity_track");
            }
        }
        if (!hasSample && !activeSpeaker.isEmpty()) {
            sectionRotationDegrees = 0.0;
            hasSample = assignedContinuityTrackSampleForSpeaker(
                clip,
                activeSpeaker,
                timelineFramePosition,
                mediaSourceFramePosition,
                &location,
                &boxSize,
                &sectionRotationDegrees,
                ContinuityAssignmentMode::SectionOrIdentity);
            if (hasSample) {
                sampleSource = QStringLiteral("assigned_continuity_track");
            }
        }
        if (!hasSample && !activeSpeaker.isEmpty()) {
            sectionRotationDegrees = 0.0;
            hasSample = speakerTrackingSample(activeSpeaker, &location, &boxSize);
            if (hasSample) {
                sampleSource = QStringLiteral("speaker_profile_tracking");
            }
        }
        if (!hasSample && activeSpeaker.isEmpty() && !currentThreadIsGuiThread()) {
            QString profileSpeaker;
            hasSample = transcriptActiveSpeakerTrackingSampleForClipFileAtSourceFrame(
                clip.filePath,
                transcriptFrame,
                clip.speakerFramingMinConfidence,
                &location,
                &boxSize,
                &profileSpeaker);
            if (hasSample) {
                sampleSource = QStringLiteral("active_speaker_profile_tracking");
                if (diagnosticsOut) {
                    diagnosticsOut->insert(QStringLiteral("profile_speaker"), profileSpeaker);
                }
            }
        }
        if (diagnosticsOut) {
            diagnosticsOut->insert(QStringLiteral("has_face_sample"), hasSample);
            diagnosticsOut->insert(QStringLiteral("sample_source"), sampleSource);
            diagnosticsOut->insert(QStringLiteral("applied_rotation_degrees"), sectionRotationDegrees);
        }
        if (!hasSample || boxSize <= 0.0) {
            return state;
        }
        return evaluateClipSpeakerFramingForFaceBoxAtPosition(
            clip,
            timelineFramePosition,
            location,
            boxSize,
            sectionRotationDegrees,
            outputSize,
            diagnosticsOut);
    }
    if (localFrame <= static_cast<qreal>(clip.speakerFramingKeyframes.constFirst().frame)) {
        state = clip.speakerFramingKeyframes.constFirst();
    } else {
        const int upperIndex = firstKeyframeAfterFrame(clip.speakerFramingKeyframes, localFrame);
        if (upperIndex >= clip.speakerFramingKeyframes.size()) {
            state = clip.speakerFramingKeyframes.constLast();
        } else {
            const TimelineClip::TransformKeyframe& previous = clip.speakerFramingKeyframes.at(upperIndex - 1);
            const TimelineClip::TransformKeyframe& current = clip.speakerFramingKeyframes.at(upperIndex);
            state = qFuzzyCompare(localFrame + 1.0, static_cast<qreal>(previous.frame) + 1.0)
                ? previous
                : interpolatedSpeakerFramingKeyframe(previous, current, localFrame);
        }
    }

    const TimelineClip::TransformKeyframe target =
        evaluateClipSpeakerFramingTargetAtPosition(clip, timelineFramePosition);
    TimelineClip::TransformKeyframe retargeted =
        retargetSpeakerFramingTransform(state, target, clip, outputSize);
    if (diagnosticsOut) {
        diagnosticsOut->insert(QStringLiteral("sample_source"), QStringLiteral("speaker_framing_keyframe"));
        diagnosticsOut->insert(QStringLiteral("speaker_framing_transform"),
                               transformKeyframeDiagnosticObject(retargeted));
    }
    return retargeted;
}

TimelineClip::TransformKeyframe composeClipTransforms(const TimelineClip::TransformKeyframe& base,
                                                      const TimelineClip::TransformKeyframe& overlay) {
    TimelineClip::TransformKeyframe composed = base;
    composed.translationX += overlay.translationX;
    composed.translationY += overlay.translationY;
    composed.rotation += overlay.rotation;
    composed.scaleX = sanitizeScaleValue(base.scaleX * overlay.scaleX);
    composed.scaleY = sanitizeScaleValue(base.scaleY * overlay.scaleY);
    return composed;
}

TimelineClip::TransformKeyframe clipBaseTransformOnly(const TimelineClip& clip) {
    TimelineClip::TransformKeyframe base;
    base.translationX = clip.baseTranslationX;
    base.translationY = clip.baseTranslationY;
    base.rotation = clip.baseRotation;
    base.scaleX = sanitizeScaleValue(clip.baseScaleX);
    base.scaleY = sanitizeScaleValue(clip.baseScaleY);
    return base;
}

TimelineClip::TransformKeyframe evaluateClipRenderTransformAtFrame(const TimelineClip& clip,
                                                                   int64_t timelineFrame,
                                                                   const QSize& outputSize) {
    return evaluateClipRenderTransformAtFrame(clip, timelineFrame, {}, outputSize);
}

TimelineClip::TransformKeyframe evaluateClipRenderTransformAtFrame(const TimelineClip& clip,
                                                                   int64_t timelineFrame,
                                                                   const QVector<RenderSyncMarker>& markers,
                                                                   const QSize& outputSize) {
    if (evaluateClipSpeakerFramingEnabledAtFrame(clip, timelineFrame)) {
        return composeClipTransforms(
            clipBaseTransformOnly(clip),
            evaluateClipSpeakerFramingAtFrame(clip, timelineFrame, markers, outputSize));
    }
    return composeClipTransforms(
        evaluateClipTransformAtFrame(clip, timelineFrame),
        evaluateClipSpeakerFramingAtFrame(clip, timelineFrame, markers, outputSize));
}

TimelineClip::TransformKeyframe evaluateClipRenderTransformAtPosition(const TimelineClip& clip,
                                                                      qreal timelineFramePosition,
                                                                      const QSize& outputSize) {
    return evaluateClipRenderTransformAtPosition(clip, timelineFramePosition, {}, outputSize);
}

TimelineClip::TransformKeyframe evaluateClipRenderTransformAtPosition(const TimelineClip& clip,
                                                                      qreal timelineFramePosition,
                                                                      const QVector<RenderSyncMarker>& markers,
                                                                      const QSize& outputSize,
                                                                      QJsonObject* diagnosticsOut) {
    TimelineClip::TransformKeyframe finalTransform;
    if (evaluateClipSpeakerFramingEnabledAtPosition(clip, timelineFramePosition)) {
        finalTransform = composeClipTransforms(
            clipBaseTransformOnly(clip),
            evaluateClipSpeakerFramingAtPosition(clip, timelineFramePosition, markers, outputSize, diagnosticsOut));
        if (diagnosticsOut) {
            diagnosticsOut->insert(QStringLiteral("base_transform_source"), QStringLiteral("clip_base_only"));
            diagnosticsOut->insert(QStringLiteral("final_render_transform"),
                                   transformKeyframeDiagnosticObject(finalTransform));
        }
        return finalTransform;
    }
    finalTransform = composeClipTransforms(
        evaluateClipTransformAtPosition(clip, timelineFramePosition),
        evaluateClipSpeakerFramingAtPosition(clip, timelineFramePosition, markers, outputSize, diagnosticsOut));
    if (diagnosticsOut) {
        diagnosticsOut->insert(QStringLiteral("base_transform_source"), QStringLiteral("clip_keyframed_transform"));
        diagnosticsOut->insert(QStringLiteral("final_render_transform"),
                               transformKeyframeDiagnosticObject(finalTransform));
    }
    return finalTransform;
}

TimelineClip::GradingKeyframe evaluateClipGradingAtFrame(const TimelineClip& clip, int64_t timelineFrame) {
    TimelineClip::GradingKeyframe state;
    state.brightness = clip.brightness;
    state.contrast = clip.contrast;
    state.saturation = clip.saturation;
    state.curvePointsR = defaultGradingCurvePoints();
    state.curvePointsG = defaultGradingCurvePoints();
    state.curvePointsB = defaultGradingCurvePoints();
    state.curvePointsLuma = defaultGradingCurvePoints();
    state.curveSmoothingEnabled = true;
    state.opacity = evaluateClipOpacityAtFrame(clip, timelineFrame);
    if (clip.gradingKeyframes.isEmpty()) {
        return state;
    }

    const int64_t localFrame = qBound<int64_t>(0, timelineFrame - clip.startFrame, qMax<int64_t>(0, clip.durationFrames - 1));
    if (localFrame <= clip.gradingKeyframes.constFirst().frame) {
        TimelineClip::GradingKeyframe first = clip.gradingKeyframes.constFirst();
        first.opacity = evaluateClipOpacityAtFrame(clip, timelineFrame);
        return first;
    }

    for (int i = 1; i < clip.gradingKeyframes.size(); ++i) {
        const TimelineClip::GradingKeyframe& previous = clip.gradingKeyframes[i - 1];
        const TimelineClip::GradingKeyframe& current = clip.gradingKeyframes[i];
        if (localFrame < current.frame) {
            if (!current.linearInterpolation || current.frame <= previous.frame) {
                TimelineClip::GradingKeyframe resolved = previous;
                resolved.opacity = evaluateClipOpacityAtFrame(clip, timelineFrame);
                return resolved;
            }
            const qreal t = static_cast<qreal>(localFrame - previous.frame) /
                            static_cast<qreal>(current.frame - previous.frame);
            state = interpolatedGradingKeyframe(previous, current, t);
            state.frame = localFrame;
            state.linearInterpolation = current.linearInterpolation;
            state.opacity = evaluateClipOpacityAtFrame(clip, timelineFrame);
            return state;
        }
        if (localFrame == current.frame) {
            TimelineClip::GradingKeyframe resolved = current;
            resolved.opacity = evaluateClipOpacityAtFrame(clip, timelineFrame);
            return resolved;
        }
    }

    TimelineClip::GradingKeyframe last = clip.gradingKeyframes.constLast();
    last.opacity = evaluateClipOpacityAtFrame(clip, timelineFrame);
    return last;
}

TimelineClip::GradingKeyframe evaluateClipGradingAtPosition(const TimelineClip& clip, qreal timelineFramePosition) {
    TimelineClip::GradingKeyframe state;
    state.brightness = clip.brightness;
    state.contrast = clip.contrast;
    state.saturation = clip.saturation;
    state.curvePointsR = defaultGradingCurvePoints();
    state.curvePointsG = defaultGradingCurvePoints();
    state.curvePointsB = defaultGradingCurvePoints();
    state.curvePointsLuma = defaultGradingCurvePoints();
    state.curveSmoothingEnabled = true;
    state.opacity = evaluateClipOpacityAtPosition(clip, timelineFramePosition);
    if (clip.gradingKeyframes.isEmpty()) {
        return state;
    }

    const qreal maxFrame = static_cast<qreal>(qMax<int64_t>(0, clip.durationFrames - 1));
    const qreal localFrame = qBound<qreal>(0.0, timelineFramePosition - static_cast<qreal>(clip.startFrame), maxFrame);
    if (localFrame <= static_cast<qreal>(clip.gradingKeyframes.constFirst().frame)) {
        TimelineClip::GradingKeyframe first = clip.gradingKeyframes.constFirst();
        first.opacity = evaluateClipOpacityAtPosition(clip, timelineFramePosition);
        return first;
    }

    state = clip.gradingKeyframes.constLast();
    for (int i = 1; i < clip.gradingKeyframes.size(); ++i) {
        const TimelineClip::GradingKeyframe& previous = clip.gradingKeyframes[i - 1];
        const TimelineClip::GradingKeyframe& current = clip.gradingKeyframes[i];
        if (localFrame < static_cast<qreal>(current.frame)) {
            if (!current.linearInterpolation || current.frame <= previous.frame) {
                TimelineClip::GradingKeyframe resolved = previous;
                resolved.opacity = evaluateClipOpacityAtPosition(clip, timelineFramePosition);
                return resolved;
            }
            const qreal t = (localFrame - static_cast<qreal>(previous.frame)) /
                            static_cast<qreal>(current.frame - previous.frame);
            state = interpolatedGradingKeyframe(previous, current, t);
            state.frame = qRound64(localFrame);
            state.linearInterpolation = current.linearInterpolation;
            state.opacity = evaluateClipOpacityAtPosition(clip, timelineFramePosition);
            return state;
        }
        if (qFuzzyCompare(localFrame + 1.0, static_cast<qreal>(current.frame) + 1.0)) {
            TimelineClip::GradingKeyframe resolved = current;
            resolved.opacity = evaluateClipOpacityAtPosition(clip, timelineFramePosition);
            return resolved;
        }
    }
    state.opacity = evaluateClipOpacityAtPosition(clip, timelineFramePosition);
    return state;
}

qreal evaluateClipOpacityAtFrame(const TimelineClip& clip, int64_t timelineFrame) {
    if (clip.opacityKeyframes.isEmpty()) {
        return qBound<qreal>(0.0, clip.opacity, 1.0);
    }

    const int64_t localFrame = qBound<int64_t>(0, timelineFrame - clip.startFrame, qMax<int64_t>(0, clip.durationFrames - 1));
    if (localFrame <= clip.opacityKeyframes.constFirst().frame) {
        return qBound<qreal>(0.0, clip.opacityKeyframes.constFirst().opacity, 1.0);
    }

    for (int i = 1; i < clip.opacityKeyframes.size(); ++i) {
        const TimelineClip::OpacityKeyframe& previous = clip.opacityKeyframes[i - 1];
        const TimelineClip::OpacityKeyframe& current = clip.opacityKeyframes[i];
        if (localFrame < current.frame) {
            if (!current.linearInterpolation || current.frame <= previous.frame) {
                return qBound<qreal>(0.0, previous.opacity, 1.0);
            }
            const qreal t = static_cast<qreal>(localFrame - previous.frame) /
                            static_cast<qreal>(current.frame - previous.frame);
            return qBound<qreal>(0.0, previous.opacity + ((current.opacity - previous.opacity) * t), 1.0);
        }
        if (localFrame == current.frame) {
            return qBound<qreal>(0.0, current.opacity, 1.0);
        }
    }
    return qBound<qreal>(0.0, clip.opacityKeyframes.constLast().opacity, 1.0);
}

qreal evaluateClipOpacityAtPosition(const TimelineClip& clip, qreal timelineFramePosition) {
    if (clip.opacityKeyframes.isEmpty()) {
        return qBound<qreal>(0.0, clip.opacity, 1.0);
    }

    const qreal maxFrame = static_cast<qreal>(qMax<int64_t>(0, clip.durationFrames - 1));
    const qreal localFrame = qBound<qreal>(0.0, timelineFramePosition - static_cast<qreal>(clip.startFrame), maxFrame);
    if (localFrame <= static_cast<qreal>(clip.opacityKeyframes.constFirst().frame)) {
        return qBound<qreal>(0.0, clip.opacityKeyframes.constFirst().opacity, 1.0);
    }

    for (int i = 1; i < clip.opacityKeyframes.size(); ++i) {
        const TimelineClip::OpacityKeyframe& previous = clip.opacityKeyframes[i - 1];
        const TimelineClip::OpacityKeyframe& current = clip.opacityKeyframes[i];
        if (localFrame < static_cast<qreal>(current.frame)) {
            if (!current.linearInterpolation || current.frame <= previous.frame) {
                return qBound<qreal>(0.0, previous.opacity, 1.0);
            }
            const qreal t = (localFrame - static_cast<qreal>(previous.frame)) /
                            static_cast<qreal>(current.frame - previous.frame);
            return qBound<qreal>(0.0, previous.opacity + ((current.opacity - previous.opacity) * t), 1.0);
        }
        if (qFuzzyCompare(localFrame + 1.0, static_cast<qreal>(current.frame) + 1.0)) {
            return qBound<qreal>(0.0, current.opacity, 1.0);
        }
    }
    return qBound<qreal>(0.0, clip.opacityKeyframes.constLast().opacity, 1.0);
}
