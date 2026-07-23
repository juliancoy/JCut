#include "editor_shared_effects.h"
#include "mask_frame_map_core.h"
#include "editor_grading_core.h"
#include "editor_shared_keyframes.h"
#include "editor_shared_media.h"
#include "editor_shared_render_sync.h"
#include "editor_shared_transcript.h"
#include "mask_sidecar.h"
#include "transform_skip_aware_timing.h"

#include <QCache>
#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QHash>
#include <QJsonDocument>
#include <QJsonObject>
#include <QMutex>
#include <QMutexLocker>
#include <QPainter>
#include <QPainterPath>
#include <QRegularExpression>
#include <QTextStream>
#include <QtGlobal>

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <optional>
#include <string>
#include <sys/stat.h>

bool trackHasEffectPreset(const TimelineTrack& track)
{
    return track.effectPreset != ClipEffectPreset::None;
}

TimelineClip clipWithTrackEffectSettings(const TimelineClip& clip, const QVector<TimelineTrack>& tracks)
{
    Q_UNUSED(tracks);
    // Effects are clip-owned. A track/canvas may have its own effect state,
    // but that state must never be inherited by clips placed beneath it.
    return clip;
}

bool effectPresetSupportedForClipRole(ClipEffectPreset preset, ClipRole role)
{
    return role != ClipRole::MaskMatte ||
           (preset != ClipEffectPreset::DifferenceMatte &&
            preset != ClipEffectPreset::TemporalEcho);
}

TimelineClip clipWithRenderableEffectSettings(const TimelineClip& clip,
                                              const QVector<TimelineTrack>& tracks)
{
    TimelineClip renderable = clipWithTrackEffectSettings(clip, tracks);
    if (!effectPresetSupportedForClipRole(renderable.effectPreset, renderable.clipRole)) {
        renderable.effectPreset = ClipEffectPreset::None;
    }
    return renderable;
}

namespace {
std::vector<jcut::EditorPoint> editorCurveFromQt(
    const QVector<QPointF>& points)
{
    std::vector<jcut::EditorPoint> result;
    result.reserve(static_cast<std::size_t>(points.size()));
    for (const QPointF& point : points) {
        result.push_back({point.x(), point.y()});
    }
    return result;
}

QVector<QPointF> qtCurveFromEditor(
    const std::vector<jcut::EditorPoint>& points)
{
    QVector<QPointF> result;
    result.reserve(static_cast<qsizetype>(points.size()));
    for (const jcut::EditorPoint& point : points) {
        result.push_back(QPointF(point.x, point.y));
    }
    return result;
}

QVector<quint8> qtCurveLutFromEditor(
    const std::vector<std::uint8_t>& values)
{
    QVector<quint8> result;
    result.reserve(static_cast<qsizetype>(values.size()));
    for (const std::uint8_t value : values) {
        result.push_back(static_cast<quint8>(value));
    }
    return result;
}

int clampChannel(int value) {
    return qBound(0, value, 255);
}

TimelineClip::GradingKeyframe maskGradeForClip(const TimelineClip& clip)
{
    TimelineClip::GradingKeyframe grade;
    grade.brightness = clip.maskGradeBrightness;
    grade.contrast = clip.maskGradeContrast;
    grade.saturation = clip.maskGradeSaturation;
    grade.curvePointsR = clip.maskGradeCurvePointsR;
    grade.curvePointsG = clip.maskGradeCurvePointsG;
    grade.curvePointsB = clip.maskGradeCurvePointsB;
    grade.curvePointsLuma = clip.maskGradeCurvePointsLuma;
    grade.curveSmoothingEnabled = clip.maskGradeCurveSmoothingEnabled;
    return grade;
}

QCache<QString, QImage>& preparedMaskCache()
{
    static QCache<QString, QImage> cache(256 * 1024);
    return cache;
}

QMutex& preparedMaskCacheMutex()
{
    static QMutex mutex;
    return mutex;
}

struct FileVersionSignature {
    bool valid = false;
    qint64 size = -1;
    qint64 modifiedNanoseconds = -1;
    qint64 changedNanoseconds = -1;
    quint64 device = 0;
    quint64 inode = 0;

    bool operator==(const FileVersionSignature& other) const
    {
        return valid == other.valid && size == other.size &&
            modifiedNanoseconds == other.modifiedNanoseconds &&
            changedNanoseconds == other.changedNanoseconds &&
            device == other.device && inode == other.inode;
    }

    bool operator!=(const FileVersionSignature& other) const
    {
        return !(*this == other);
    }
};

FileVersionSignature fileVersionSignature(const QString& path)
{
    FileVersionSignature signature;
#if defined(Q_OS_WIN)
    struct _stat64 fileStat {};
    const std::wstring nativePath = QFileInfo(path).absoluteFilePath().toStdWString();
    if (::_wstat64(nativePath.c_str(), &fileStat) != 0 ||
        (fileStat.st_mode & _S_IFREG) == 0 || fileStat.st_size < 0) {
        return signature;
    }
    signature.modifiedNanoseconds =
        static_cast<qint64>(fileStat.st_mtime) * 1000000000;
    signature.changedNanoseconds =
        static_cast<qint64>(fileStat.st_ctime) * 1000000000;
#else
    struct stat fileStat {};
    const QByteArray nativePath = QFile::encodeName(QFileInfo(path).absoluteFilePath());
    if (::stat(nativePath.constData(), &fileStat) != 0 ||
        !S_ISREG(fileStat.st_mode) || fileStat.st_size < 0) {
        return signature;
    }
#if defined(Q_OS_DARWIN)
    signature.modifiedNanoseconds =
        static_cast<qint64>(fileStat.st_mtimespec.tv_sec) * 1000000000 +
        fileStat.st_mtimespec.tv_nsec;
    signature.changedNanoseconds =
        static_cast<qint64>(fileStat.st_ctimespec.tv_sec) * 1000000000 +
        fileStat.st_ctimespec.tv_nsec;
#else
    signature.modifiedNanoseconds =
        static_cast<qint64>(fileStat.st_mtim.tv_sec) * 1000000000 +
        fileStat.st_mtim.tv_nsec;
    signature.changedNanoseconds =
        static_cast<qint64>(fileStat.st_ctim.tv_sec) * 1000000000 +
        fileStat.st_ctim.tv_nsec;
#endif
#endif
    signature.valid = true;
    signature.size = static_cast<qint64>(fileStat.st_size);
    signature.device = static_cast<quint64>(fileStat.st_dev);
    signature.inode = static_cast<quint64>(fileStat.st_ino);
    return signature;
}

struct MaskFrameMap {
    FileVersionSignature mapVersion;
    FileVersionSignature metadataVersion;
    FileVersionSignature completionVersion;
    FileVersionSignature alphaCompletionVersion;
    FileVersionSignature sourceVersion;
    quint64 lastAccess = 0;
    int64_t mappedFrameCount = 0;
    int64_t firstSourceFrame = -1;
    int64_t lastSourceFrame = -1;
    int64_t lastMaskFrame = -1;
    QString mapSha256;
    bool metadataVerified = false;
    bool renderReady = false;
    bool ordinalSidecar = false;
    QVector<QPair<int64_t, int64_t>> sorted;
};

constexpr qsizetype kMaxCachedMaskFrameMaps = 8;

QHash<QString, MaskFrameMap>& maskFrameMapCache()
{
    static QHash<QString, MaskFrameMap> cache;
    return cache;
}

QMutex& maskFrameMapCacheMutex()
{
    static QMutex mutex;
    return mutex;
}

quint64& maskFrameMapAccessCounter()
{
    static quint64 counter = 0;
    return counter;
}

MaskFrameMap loadMaskFrameMap(const QString& path, const QString& sourceMediaPath)
{
    MaskFrameMap map;
    map.ordinalSidecar = editor::masks::maskSidecarUsesDecodeOrdinalFrames(
        QFileInfo(path).dir().absolutePath());
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        return map;
    }
    map.mapSha256 = QString::fromLatin1(
        QCryptographicHash::hash(file.readAll(), QCryptographicHash::Sha256).toHex());
    if (!file.seek(0)) {
        return map;
    }

    QTextStream stream(&file);
    const QRegularExpression whitespace(QStringLiteral("\\s+"));
    int64_t previousSourceFrame = -1;
    int64_t previousMaskFrame = -1;
    bool valid = true;
    while (!stream.atEnd()) {
        const QString line = stream.readLine().trimmed();
        if (line.isEmpty() || line.startsWith(QLatin1Char('#'))) {
            continue;
        }
        const QStringList parts = line.split(whitespace, Qt::SkipEmptyParts);
        if (parts.size() < 2) {
            valid = false;
            break;
        }
        bool okSource = false;
        bool okMask = false;
        const int64_t sourceFrame = parts.at(0).toLongLong(&okSource);
        const int64_t maskFrame = parts.at(1).toLongLong(&okMask);
        if (!okSource || !okMask || sourceFrame < 0 || maskFrame < 0 ||
            maskFrame != map.mappedFrameCount ||
            (map.mappedFrameCount > 0 &&
             (sourceFrame < previousSourceFrame || maskFrame < previousMaskFrame))) {
            valid = false;
            break;
        }
        ++map.mappedFrameCount;
        map.firstSourceFrame = map.firstSourceFrame < 0
            ? sourceFrame
            : qMin(map.firstSourceFrame, sourceFrame);
        map.lastSourceFrame = qMax(map.lastSourceFrame, sourceFrame);
        map.lastMaskFrame = qMax(map.lastMaskFrame, maskFrame);
        map.sorted.push_back(qMakePair(sourceFrame, maskFrame));
        previousSourceFrame = sourceFrame;
        previousMaskFrame = maskFrame;
    }
    if (!valid) {
        map.sorted.clear();
        map.metadataVerified = false;
        map.renderReady = false;
        return map;
    }

    QFile metadataFile(QFileInfo(path).dir().filePath(
        QStringLiteral("jcut_frame_map.json")));
    if (metadataFile.open(QIODevice::ReadOnly)) {
        const QJsonObject metadata =
            QJsonDocument::fromJson(metadataFile.readAll()).object();
        map.metadataVerified =
            metadata.value(QStringLiteral("schema")).toString() ==
                QStringLiteral("jcut_frame_index_map_v2") &&
            metadata.value(QStringLiteral("status")).toString() ==
                QStringLiteral("ready") &&
            metadata.value(QStringLiteral("frame_domain")).toString() ==
                QStringLiteral("source_timestamp_to_generated_ordinal") &&
            metadata.value(QStringLiteral("map_file")).toString() ==
                QStringLiteral("jcut_frame_map.tsv") &&
            metadata.contains(QStringLiteral("output_fps")) &&
            metadata.value(QStringLiteral("output_fps")).isNull() &&
            metadata.value(QStringLiteral("mapped_frame_count")).toInteger(-1) ==
                map.mappedFrameCount &&
            metadata.value(QStringLiteral("min_source_frame")).toInteger(-1) ==
                map.firstSourceFrame &&
            metadata.value(QStringLiteral("max_source_frame")).toInteger(-1) ==
                map.lastSourceFrame &&
            metadata.value(QStringLiteral("max_mask_frame")).toInteger(-1) ==
                map.lastMaskFrame &&
            metadata.value(QStringLiteral("expected_output_frame_count")).toInteger(-1) ==
                map.lastMaskFrame + 1 &&
            metadata.value(QStringLiteral("map_sha256")).toString().compare(
                map.mapSha256, Qt::CaseInsensitive) == 0;
    }
    if (map.metadataVerified) {
        map.renderReady =
            editor::masks::maskSidecarCompletionConfirmedForRender(
                QFileInfo(path).dir().absolutePath(),
                map.lastMaskFrame,
                sourceMediaPath);
    }
    return map;
}

std::optional<int64_t> mappedMaskFrameForSourceFrame(const TimelineClip& clip,
                                                     int64_t sourceFrame)
{
    return jcut::masks::mappedMaskFrameForSourceFrameCore(
        std::filesystem::path(clip.maskFramesDir.toStdString()),
        std::filesystem::path(clip.filePath.toStdString()),
        sourceFrame);
#if 0
    const QString maskDir = clip.maskFramesDir;
    const QString mapPath = QDir(maskDir).absoluteFilePath(QStringLiteral("jcut_frame_map.tsv"));
    const QFileInfo info(mapPath);
    if (!info.exists() || !info.isFile()) {
        if (editor::masks::maskSidecarUsesDecodeOrdinalFrames(maskDir)) {
            return std::nullopt;
        }
        return sourceFrame;
    }

    MaskFrameMap map;
    {
        QMutexLocker lock(&maskFrameMapCacheMutex());
        const QFileInfo metadataInfo(
            QDir(maskDir).absoluteFilePath(QStringLiteral("jcut_frame_map.json")));
        const QFileInfo completionInfo(
            QDir(maskDir).absoluteFilePath(QStringLiteral("jcut_mask.json")));
        const QFileInfo alphaCompletionInfo(
            QDir(maskDir).absoluteFilePath(QStringLiteral("jcut_alpha.json")));
        const QFileInfo sourceInfo(clip.filePath);
        const QString cacheKey = info.absoluteFilePath() + QChar(0x1f) +
            sourceInfo.absoluteFilePath();
        const FileVersionSignature mapVersion =
            fileVersionSignature(info.absoluteFilePath());
        const FileVersionSignature metadataVersion =
            fileVersionSignature(metadataInfo.absoluteFilePath());
        const FileVersionSignature completionVersion =
            fileVersionSignature(completionInfo.absoluteFilePath());
        const FileVersionSignature alphaCompletionVersion =
            fileVersionSignature(alphaCompletionInfo.absoluteFilePath());
        const FileVersionSignature sourceVersion =
            fileVersionSignature(sourceInfo.absoluteFilePath());
        MaskFrameMap& cached = maskFrameMapCache()[cacheKey];
        if (cached.mapVersion != mapVersion ||
            cached.metadataVersion != metadataVersion ||
            cached.completionVersion != completionVersion ||
            cached.alphaCompletionVersion != alphaCompletionVersion ||
            cached.sourceVersion != sourceVersion) {
            cached = loadMaskFrameMap(info.absoluteFilePath(), clip.filePath);
            cached.mapVersion = mapVersion;
            cached.metadataVersion = metadataVersion;
            cached.completionVersion = completionVersion;
            cached.alphaCompletionVersion = alphaCompletionVersion;
            cached.sourceVersion = sourceVersion;
        }
        cached.lastAccess = ++maskFrameMapAccessCounter();
        map = cached;
        while (maskFrameMapCache().size() > kMaxCachedMaskFrameMaps) {
            auto oldest = maskFrameMapCache().end();
            for (auto candidate = maskFrameMapCache().begin();
                 candidate != maskFrameMapCache().end(); ++candidate) {
                if (candidate.key() == cacheKey) {
                    continue;
                }
                if (oldest == maskFrameMapCache().end() ||
                    candidate.value().lastAccess < oldest.value().lastAccess) {
                    oldest = candidate;
                }
            }
            if (oldest == maskFrameMapCache().end()) {
                break;
            }
            maskFrameMapCache().erase(oldest);
        }
    }

    const bool ordinalSidecar = map.ordinalSidecar;
    if (map.sorted.isEmpty()) {
        if (ordinalSidecar) {
            return std::nullopt;
        }
        return sourceFrame;
    }
    if (ordinalSidecar &&
        (!map.metadataVerified || !map.renderReady)) {
        return std::nullopt;
    }
    if (ordinalSidecar &&
        (sourceFrame < map.firstSourceFrame || sourceFrame > map.lastSourceFrame)) {
        return std::nullopt;
    }

    const auto lower = std::lower_bound(
        map.sorted.constBegin(),
        map.sorted.constEnd(),
        sourceFrame,
        [](const QPair<int64_t, int64_t>& entry, int64_t value) {
            return entry.first < value;
        });
    if (lower != map.sorted.constEnd() && lower->first == sourceFrame) {
        return lower->second;
    }
    if (ordinalSidecar) {
        return std::nullopt;
    }
    if (lower == map.sorted.constBegin()) {
        return lower->second;
    }
    if (lower == map.sorted.constEnd()) {
        return (map.sorted.constEnd() - 1)->second;
    }
    const auto previous = lower - 1;
    return qAbs(previous->first - sourceFrame) <= qAbs(lower->first - sourceFrame)
               ? previous->second
               : lower->second;
#endif
}

qreal effectTimelinePositionForClip(const TimelineClip& clip,
                                    qreal timelineFramePosition,
                                    const QVector<RenderSyncMarker>& markers,
                                    const PlaybackTimingContext& timing) {
    qreal adjustedTimelineFramePosition = timelineFramePosition;

    if (clip.effectSkipAwareTiming && !markers.isEmpty()) {
        const qreal maxLocalFrame = static_cast<qreal>(qMax<int64_t>(0, clip.durationFrames - 1));
        const qreal localTimelineFrame =
            qBound<qreal>(0.0, timelineFramePosition - static_cast<qreal>(clip.startFrame), maxLocalFrame);
        const int64_t steppedLocalTimelineFrame =
            qMax<int64_t>(0, static_cast<int64_t>(std::floor(localTimelineFrame)));
        const qreal fractional = localTimelineFrame - static_cast<qreal>(steppedLocalTimelineFrame);
        const int64_t adjustedLocalFrame =
            adjustedClipLocalFrameAtTimelineFrame(clip, steppedLocalTimelineFrame, markers);
        const qreal adjustedLocalFramePosition =
            qBound<qreal>(0.0, static_cast<qreal>(adjustedLocalFrame) + fractional, maxLocalFrame);
        adjustedTimelineFramePosition = static_cast<qreal>(clip.startFrame) + adjustedLocalFramePosition;
    }

    return static_cast<qreal>(clip.startFrame) +
           clipPlaybackFramePositionForTimelineFrame(clip, adjustedTimelineFramePosition, timing);
}
}  // namespace

QVector<QPointF> defaultGradingCurvePoints() {
    return {QPointF(0.0, 0.0), QPointF(1.0, 1.0)};
}

QVector<QPointF> sanitizeGradingCurvePoints(const QVector<QPointF>& points) {
    return qtCurveFromEditor(
        jcut::sanitizeEditorGradingCurve(editorCurveFromQt(points)));
}

qreal sampleGradingCurveAt(const QVector<QPointF>& points, qreal xNorm, bool smoothingEnabled) {
    return jcut::sampleEditorGradingCurveAt(
        editorCurveFromQt(points), xNorm, smoothingEnabled);
}

QVector<quint8> gradingCurveLut8(const QVector<QPointF>& points, int samples, bool smoothingEnabled) {
    return qtCurveLutFromEditor(jcut::editorGradingCurveLut8(
        editorCurveFromQt(points), samples, smoothingEnabled));
}

bool gradingCurveDiffersFromIdentity(const QVector<QPointF>& points, bool smoothingEnabled)
{
    const QVector<quint8> lut =
        gradingCurveLut8(points, TimelineClip::kGradingCurveLutSize, smoothingEnabled);
    const QVector<quint8> identityLut =
        gradingCurveLut8(defaultGradingCurvePoints(), TimelineClip::kGradingCurveLutSize, smoothingEnabled);
    return !lut.isEmpty() && !identityLut.isEmpty() && lut != identityLut;
}

bool gradingUsesCurveLut(const TimelineClip::GradingKeyframe& grade)
{
    return gradingCurveDiffersFromIdentity(grade.curvePointsR, grade.curveSmoothingEnabled) ||
           gradingCurveDiffersFromIdentity(grade.curvePointsG, grade.curveSmoothingEnabled) ||
           gradingCurveDiffersFromIdentity(grade.curvePointsB, grade.curveSmoothingEnabled) ||
           gradingCurveDiffersFromIdentity(grade.curvePointsLuma, grade.curveSmoothingEnabled);
}

qreal evaluateEffectiveClipOpacityAtFrame(const TimelineClip& clip,
                                          const QVector<TimelineTrack>& tracks,
                                          int64_t timelineFrame) {
    if (trackVisualModeForClip(clip, tracks) == TrackVisualMode::ForceOpaque) {
        return 1.0;
    }
    return evaluateClipOpacityAtFrame(clip, timelineFrame);
}

qreal evaluateEffectiveClipOpacityAtPosition(const TimelineClip& clip,
                                             const QVector<TimelineTrack>& tracks,
                                             qreal timelineFramePosition) {
    if (trackVisualModeForClip(clip, tracks) == TrackVisualMode::ForceOpaque) {
        return 1.0;
    }
    return evaluateClipOpacityAtPosition(clip, timelineFramePosition);
}

TimelineClip::GradingKeyframe evaluateEffectiveClipGradingAtFrame(const TimelineClip& clip,
                                                                  const QVector<TimelineTrack>& tracks,
                                                                  int64_t timelineFrame) {
    TimelineClip::GradingKeyframe grade = evaluateClipGradingAtFrame(clip, timelineFrame);
    TimelineClip::GradingKeyframe speakerGrade;
    const int64_t sourceFrame = sourceFrameForClipAtTimelinePosition(clip, timelineFrame, {});
    if (clip.clipRole == ClipRole::Media &&
        transcriptSpeakerGradingForClipFileAtSourceFrame(clip.filePath, sourceFrame, &speakerGrade)) {
        grade = gradingWithSpeakerOverride(grade, speakerGrade);
    }
    grade.opacity = evaluateEffectiveClipOpacityAtFrame(clip, tracks, timelineFrame);
    return grade;
}

TimelineClip::GradingKeyframe evaluateEffectiveClipGradingAtPosition(const TimelineClip& clip,
                                                                     const QVector<TimelineTrack>& tracks,
                                                                     qreal timelineFramePosition) {
    TimelineClip::GradingKeyframe grade = evaluateClipGradingAtPosition(clip, timelineFramePosition);
    TimelineClip::GradingKeyframe speakerGrade;
    const int64_t sourceFrame = sourceFrameForClipAtTimelinePosition(clip, timelineFramePosition, {});
    if (clip.clipRole == ClipRole::Media &&
        transcriptSpeakerGradingForClipFileAtSourceFrame(clip.filePath, sourceFrame, &speakerGrade)) {
        grade = gradingWithSpeakerOverride(grade, speakerGrade);
    }
    grade.opacity = evaluateEffectiveClipOpacityAtPosition(clip, tracks, timelineFramePosition);
    return grade;
}

TimelineClip::GradingKeyframe gradingWithSpeakerOverride(
    const TimelineClip::GradingKeyframe& clipGrade,
    const TimelineClip::GradingKeyframe& speakerGrade)
{
    TimelineClip::GradingKeyframe result = speakerGrade;
    // Per-person grading is an override layer, not an adjustment stacked on
    // the master or virtual-child grade. Preserve only temporal/compositing
    // metadata that is not part of the color transform.
    result.frame = clipGrade.frame;
    result.linearInterpolation = clipGrade.linearInterpolation;
    result.opacity = clipGrade.opacity;
    return result;
}

TimelineClip::GradingKeyframe evaluateEffectiveClipGradingAtFrame(const TimelineClip& clip, int64_t timelineFrame) {
    return evaluateEffectiveClipGradingAtFrame(clip, {}, timelineFrame);
}

TimelineClip::GradingKeyframe evaluateEffectiveClipGradingAtPosition(const TimelineClip& clip, qreal timelineFramePosition) {
    return evaluateEffectiveClipGradingAtPosition(clip, {}, timelineFramePosition);
}

EffectiveVisualEffects evaluateEffectiveVisualEffectsAtFrame(const TimelineClip& clip,
                                                             const QVector<TimelineTrack>& tracks,
                                                             int64_t timelineFrame) {
    EffectiveVisualEffects effects;
    effects.grading = evaluateEffectiveClipGradingAtFrame(clip, tracks, timelineFrame);
    effects.maskFeather = clip.maskFeather;
    effects.maskFeatherGamma = clip.maskFeatherGamma;
    effects.maskFeatherFalloff = clip.maskFeatherFalloff;
    for (const TimelineClip::CorrectionPolygon& polygon : clip.correctionPolygons) {
        if (correctionPolygonActiveAtTimelineFrame(clip, polygon, timelineFrame)) {
            effects.correctionPolygons.push_back(polygon);
        }
    }
    return effects;
}

EffectiveVisualEffects evaluateEffectiveVisualEffectsAtPosition(const TimelineClip& clip,
                                                                const QVector<TimelineTrack>& tracks,
                                                                qreal timelineFramePosition) {
    EffectiveVisualEffects effects;
    effects.grading = evaluateEffectiveClipGradingAtPosition(clip, tracks, timelineFramePosition);
    effects.maskFeather = clip.maskFeather;
    effects.maskFeatherGamma = clip.maskFeatherGamma;
    effects.maskFeatherFalloff = clip.maskFeatherFalloff;
    for (const TimelineClip::CorrectionPolygon& polygon : clip.correctionPolygons) {
        if (correctionPolygonActiveAtTimelinePosition(clip, polygon, timelineFramePosition)) {
            effects.correctionPolygons.push_back(polygon);
        }
    }
    return effects;
}

EffectiveVisualEffects evaluateEffectiveVisualEffectsAtFrame(const TimelineClip& clip,
                                                             const QVector<TimelineTrack>& tracks,
                                                             int64_t timelineFrame,
                                                             const QVector<RenderSyncMarker>& markers) {
    return evaluateEffectiveVisualEffectsAtPosition(
        clip, tracks, static_cast<qreal>(timelineFrame), markers);
}

EffectiveVisualEffects evaluateEffectiveVisualEffectsAtPosition(const TimelineClip& clip,
                                                                const QVector<TimelineTrack>& tracks,
                                                                qreal timelineFramePosition,
                                                                const QVector<RenderSyncMarker>& markers) {
    return evaluateEffectiveVisualEffectsAtPosition(
        clip, tracks, timelineFramePosition, markers, activePlaybackTimingContext());
}

EffectiveVisualEffects evaluateEffectiveVisualEffectsAtPosition(const TimelineClip& clip,
                                                                const QVector<TimelineTrack>& tracks,
                                                                qreal timelineFramePosition,
                                                                const QVector<RenderSyncMarker>& markers,
                                                                const PlaybackTimingContext& timing) {
    const qreal adjustedTimelinePosition =
        effectTimelinePositionForClip(clip, timelineFramePosition, markers, timing);
    return evaluateEffectiveVisualEffectsAtPosition(clip, tracks, adjustedTimelinePosition);
}
QImage applyClipGrade(const QImage& source, const TimelineClip::GradingKeyframe& grade) {
    const bool needsBasicGrade =
        !qFuzzyIsNull(grade.brightness) ||
        !qFuzzyCompare(grade.contrast, 1.0) ||
        !qFuzzyCompare(grade.saturation, 1.0) ||
        !qFuzzyCompare(grade.opacity, 1.0);
    const bool needsToneGrade =
        !qFuzzyIsNull(grade.shadowsR) || !qFuzzyIsNull(grade.shadowsG) || !qFuzzyIsNull(grade.shadowsB) ||
        !qFuzzyIsNull(grade.midtonesR) || !qFuzzyIsNull(grade.midtonesG) || !qFuzzyIsNull(grade.midtonesB) ||
        !qFuzzyIsNull(grade.highlightsR) || !qFuzzyIsNull(grade.highlightsG) || !qFuzzyIsNull(grade.highlightsB);

    const bool needsCurveGrade = gradingUsesCurveLut(grade);

    if (source.isNull() || (!needsBasicGrade && !needsToneGrade && !needsCurveGrade)) {
        return source;
    }

    QVector<quint8> curveLutR;
    QVector<quint8> curveLutG;
    QVector<quint8> curveLutB;
    QVector<quint8> curveLutL;
    if (needsCurveGrade) {
        curveLutR =
            gradingCurveLut8(grade.curvePointsR, TimelineClip::kGradingCurveLutSize, grade.curveSmoothingEnabled);
        curveLutG =
            gradingCurveLut8(grade.curvePointsG, TimelineClip::kGradingCurveLutSize, grade.curveSmoothingEnabled);
        curveLutB =
            gradingCurveLut8(grade.curvePointsB, TimelineClip::kGradingCurveLutSize, grade.curveSmoothingEnabled);
        curveLutL =
            gradingCurveLut8(grade.curvePointsLuma, TimelineClip::kGradingCurveLutSize, grade.curveSmoothingEnabled);
    }

    auto smoothShadows = [](float luma) { return std::pow(1.0f - luma, 2.0f); };
    auto smoothMidtones = [](float luma) { return 1.0f - std::abs(luma - 0.5f) * 2.0f; };
    auto smoothHighlights = [](float luma) { return std::pow(luma, 2.0f); };

    QImage graded = source.convertToFormat(QImage::Format_ARGB32);
    for (int y = 0; y < graded.height(); ++y) {
        QRgb* row = reinterpret_cast<QRgb*>(graded.scanLine(y));
        for (int x = 0; x < graded.width(); ++x) {
            QColor color = QColor::fromRgba(row[x]);
            float h = 0.0f, s = 0.0f, l = 0.0f, a = 0.0f;
            color.getHslF(&h, &s, &l, &a);

            float rf = color.redF();
            float gf = color.greenF();
            float bf = color.blueF();

            // Calculate luminance for tone-based grading
            float luminance = rf * 0.2126f + gf * 0.7152f + bf * 0.0722f;

            // Apply Shadows (Lift)
            if (needsToneGrade) {
                float shadowWeight = smoothShadows(luminance);
                rf *= (1.0f + grade.shadowsR * shadowWeight);
                gf *= (1.0f + grade.shadowsG * shadowWeight);
                bf *= (1.0f + grade.shadowsB * shadowWeight);

                // Apply Midtones (Gamma)
                float midtoneWeight = smoothMidtones(luminance);
                rf = std::pow(rf, 1.0f / (1.0f + grade.midtonesR * midtoneWeight));
                gf = std::pow(gf, 1.0f / (1.0f + grade.midtonesG * midtoneWeight));
                bf = std::pow(bf, 1.0f / (1.0f + grade.midtonesB * midtoneWeight));

                // Apply Highlights (Gain)
                float highlightWeight = smoothHighlights(luminance);
                rf += grade.highlightsR * highlightWeight;
                gf += grade.highlightsG * highlightWeight;
                bf += grade.highlightsB * highlightWeight;
            }

            if (!curveLutR.isEmpty() && !curveLutG.isEmpty() && !curveLutB.isEmpty()) {
                const int ri = qBound(0, qRound(rf * 255.0f), 255);
                const int gi = qBound(0, qRound(gf * 255.0f), 255);
                const int bi = qBound(0, qRound(bf * 255.0f), 255);
                rf = static_cast<float>(curveLutR[ri]) / 255.0f;
                gf = static_cast<float>(curveLutG[gi]) / 255.0f;
                bf = static_cast<float>(curveLutB[bi]) / 255.0f;
                if (!curveLutL.isEmpty()) {
                    const float curveLuma = (rf * 0.2126f) + (gf * 0.7152f) + (bf * 0.0722f);
                    const int lumaIndex = qBound(0, qRound(curveLuma * 255.0f), 255);
                    const float remappedLuma = static_cast<float>(curveLutL[lumaIndex]) / 255.0f;
                    if (curveLuma > 0.0001f) {
                        const float lumaScale = remappedLuma / curveLuma;
                        rf *= lumaScale;
                        gf *= lumaScale;
                        bf *= lumaScale;
                    } else {
                        rf = remappedLuma;
                        gf = remappedLuma;
                        bf = remappedLuma;
                    }
                }
            }

            // Basic grading
            rf = qBound(0.0f, rf, 1.0f);
            gf = qBound(0.0f, gf, 1.0f);
            bf = qBound(0.0f, bf, 1.0f);

            int r = clampChannel(qRound(((rf * 255.0 - 127.5) * grade.contrast) + 127.5 + grade.brightness * 255.0));
            int g = clampChannel(qRound(((gf * 255.0 - 127.5) * grade.contrast) + 127.5 + grade.brightness * 255.0));
            int b = clampChannel(qRound(((bf * 255.0 - 127.5) * grade.contrast) + 127.5 + grade.brightness * 255.0));

            QColor adjusted(r, g, b, color.alpha());
            adjusted.getHslF(&h, &s, &l, &a);
            s = qBound(0.0f, static_cast<float>(s * grade.saturation), 1.0f);
            a = qBound(0.0f, static_cast<float>(a * grade.opacity), 1.0f);
            adjusted.setHslF(h, s, l, a);
            row[x] = adjusted.rgba();
        }
    }
    return graded.convertToFormat(QImage::Format_ARGB32_Premultiplied);
}

QImage applyClipGrade(const QImage& source, const TimelineClip& clip) {
    return applyClipGrade(source, evaluateEffectiveClipGradingAtFrame(clip, clip.startFrame));
}

namespace {
QString maskFramePathForSourceFrame(const TimelineClip& clip, int64_t sourceFrame)
{
    if (clip.maskFramesDir.trimmed().isEmpty()) {
        return QString();
    }
    const std::optional<int64_t> maskFrame = mappedMaskFrameForSourceFrame(
        clip, qMax<int64_t>(0, sourceFrame));
    if (!maskFrame.has_value()) {
        return QString();
    }
    const QString fileName =
        QStringLiteral("frame_%1.png")
            .arg(*maskFrame + 1, 6, 10, QLatin1Char('0'));
    return QDir(clip.maskFramesDir).absoluteFilePath(fileName);
}

QImage morphMask(const QImage& source, int radius, bool dilate)
{
    if (source.isNull() || radius <= 0) {
        return source;
    }
    const QImage input = source.convertToFormat(QImage::Format_Grayscale8);
    QImage output(input.size(), QImage::Format_Grayscale8);
    const int width = input.width();
    const int height = input.height();
    for (int y = 0; y < height; ++y) {
        uchar* dst = output.scanLine(y);
        for (int x = 0; x < width; ++x) {
            int value = dilate ? 0 : 255;
            for (int yy = qMax(0, y - radius); yy <= qMin(height - 1, y + radius); ++yy) {
                const uchar* src = input.constScanLine(yy);
                for (int xx = qMax(0, x - radius); xx <= qMin(width - 1, x + radius); ++xx) {
                    value = dilate ? qMax(value, static_cast<int>(src[xx]))
                                   : qMin(value, static_cast<int>(src[xx]));
                }
            }
            dst[x] = static_cast<uchar>(value);
        }
    }
    return output;
}

QImage blurMask(const QImage& source, int radius)
{
    if (source.isNull() || radius <= 0) {
        return source;
    }
    const QImage input = source.convertToFormat(QImage::Format_Grayscale8);
    const int width = input.width();
    const int height = input.height();
    if (width <= 0 || height <= 0) {
        return input;
    }

    QVector<int> horizontal(width * height, 0);
    for (int y = 0; y < height; ++y) {
        const uchar* src = input.constScanLine(y);
        int sum = 0;
        int count = 0;
        for (int x = 0; x <= qMin(width - 1, radius); ++x) {
            sum += src[x];
            ++count;
        }
        for (int x = 0; x < width; ++x) {
            horizontal[(y * width) + x] = count > 0 ? sum / count : 0;
            const int removeX = x - radius;
            if (removeX >= 0) {
                sum -= src[removeX];
                --count;
            }
            const int addX = x + radius + 1;
            if (addX < width) {
                sum += src[addX];
                ++count;
            }
        }
    }

    QImage output(input.size(), QImage::Format_Grayscale8);
    QVector<int> columnSums(width, 0);
    QVector<int> columnCounts(width, 0);
    for (int y = 0; y <= qMin(height - 1, radius); ++y) {
        const int* row = horizontal.constData() + (y * width);
        for (int x = 0; x < width; ++x) {
            columnSums[x] += row[x];
            ++columnCounts[x];
        }
    }
    for (int y = 0; y < height; ++y) {
        uchar* dst = output.scanLine(y);
        for (int x = 0; x < width; ++x) {
            dst[x] = static_cast<uchar>(columnCounts[x] > 0
                                            ? qBound(0, columnSums[x] / columnCounts[x], 255)
                                            : 0);
        }
        const int removeY = y - radius;
        if (removeY >= 0) {
            const int* row = horizontal.constData() + (removeY * width);
            for (int x = 0; x < width; ++x) {
                columnSums[x] -= row[x];
                --columnCounts[x];
            }
        }
        const int addY = y + radius + 1;
        if (addY < height) {
            const int* row = horizontal.constData() + (addY * width);
            for (int x = 0; x < width; ++x) {
                columnSums[x] += row[x];
                ++columnCounts[x];
            }
        }
    }
    return output;
}

QImage preparedClipMask(const TimelineClip& clip, int64_t sourceFrame, const QSize& size)
{
    const QString path = maskFramePathForSourceFrame(clip, sourceFrame);
    const QFileInfo info(path);
    if (path.isEmpty() || !info.exists()) {
        return QImage();
    }
    const QString cacheKey =
        QStringLiteral("%1|%2x%3|%4|%5|inv=%6|er=%7|di=%8|fe=%9|fg=%10|bl=%11")
            .arg(info.absoluteFilePath())
            .arg(size.width())
            .arg(size.height())
            .arg(info.size())
            .arg(info.lastModified().toMSecsSinceEpoch())
            .arg(clip.maskInvert ? 1 : 0)
            .arg(clip.maskErode, 0, 'g', 8)
            .arg(clip.maskDilate, 0, 'g', 8)
            .arg(clip.maskFeather, 0, 'g', 8)
            .arg(clip.maskFeatherGamma, 0, 'g', 8)
            .arg(clip.maskBlur, 0, 'g', 8);
    {
        QMutexLocker lock(&preparedMaskCacheMutex());
        if (QImage* cached = preparedMaskCache().object(cacheKey)) {
            return *cached;
        }
    }
    QImage mask(path);
    if (mask.isNull()) {
        return QImage();
    }
    if (mask.size() != size) {
        mask = mask.scaled(size, Qt::IgnoreAspectRatio, Qt::SmoothTransformation);
    }
    mask = mask.convertToFormat(QImage::Format_Grayscale8);
    if (clip.maskInvert) {
        mask.invertPixels();
    }
    if (clip.maskErode > 0.0) {
        mask = morphMask(mask, qRound(clip.maskErode), false);
    }
    if (clip.maskDilate > 0.0) {
        mask = morphMask(mask, qRound(clip.maskDilate), true);
    }
    const int blurRadius = qRound(qMax(clip.maskFeather, clip.maskBlur));
    if (blurRadius > 0) {
        mask = blurMask(mask, blurRadius);
    }
    {
        QMutexLocker lock(&preparedMaskCacheMutex());
        const int costKb = qMax(1, static_cast<int>((mask.sizeInBytes() + 1023) / 1024));
        preparedMaskCache().insert(cacheKey, new QImage(mask), costKb);
    }
    return mask;
}
}

QImage preparedClipMaskImage(const TimelineClip& clip, int64_t sourceFrame, const QSize& size)
{
    return preparedClipMask(clip, sourceFrame, size);
}

QImage rawClipMaskImage(const TimelineClip& clip, int64_t sourceFrame)
{
    const QString path = maskFramePathForSourceFrame(clip, sourceFrame);
    const QFileInfo info(path);
    if (path.isEmpty() || !info.exists()) {
        return QImage();
    }
    return QImage(path);
}

QImage applyCorrectionPolygonsToMaskImage(
    const QImage& source,
    const QVector<TimelineClip::CorrectionPolygon>& activePolygons)
{
    if (source.isNull() || activePolygons.isEmpty()) {
        return source;
    }

    QImage corrected = source.convertToFormat(QImage::Format_ARGB32_Premultiplied);
    QPainter painter(&corrected);
    painter.setRenderHint(QPainter::Antialiasing, true);
    painter.setCompositionMode(QPainter::CompositionMode_Source);
    painter.setBrush(Qt::black);
    painter.setPen(Qt::NoPen);
    const qreal width = qMax<qreal>(1.0, corrected.width());
    const qreal height = qMax<qreal>(1.0, corrected.height());
    for (const TimelineClip::CorrectionPolygon& polygon : activePolygons) {
        if (!polygon.enabled || polygon.pointsNormalized.size() < 3) {
            continue;
        }
        QPainterPath path;
        const auto imagePoint = [width, height](const QPointF& point) {
            return QPointF(qBound<qreal>(0.0, point.x(), 1.0) * width,
                           qBound<qreal>(0.0, point.y(), 1.0) * height);
        };
        path.moveTo(imagePoint(polygon.pointsNormalized.constFirst()));
        for (int i = 1; i < polygon.pointsNormalized.size(); ++i) {
            path.lineTo(imagePoint(polygon.pointsNormalized.at(i)));
        }
        path.closeSubpath();
        painter.drawPath(path);
    }
    painter.end();
    return corrected.convertToFormat(QImage::Format_Grayscale8);
}

QImage applyClipMaskEffectsToImage(const QImage& source,
                                   const TimelineClip& clip,
                                   int64_t sourceFrame)
{
    TimelineClip::GradingKeyframe neutralGrade;
    return applyClipMaskEffectsToImage(source, clip, sourceFrame, neutralGrade);
}

QImage applyClipMaskEffectsToImage(const QImage& source,
                                   const TimelineClip& clip,
                                   int64_t sourceFrame,
                                   const TimelineClip::GradingKeyframe& clipGrade)
{
    if (source.isNull()) {
        return source;
    }
    auto failClosedMaskMatte = [&source, &clip]() {
        if (clip.clipRole != ClipRole::MaskMatte) {
            return source;
        }
        QImage transparent(source.size(), QImage::Format_ARGB32_Premultiplied);
        transparent.fill(Qt::transparent);
        return transparent;
    };
    if (!clip.maskEnabled || clip.maskFramesDir.trimmed().isEmpty()) {
        return failClosedMaskMatte();
    }
    const QImage mask = preparedClipMask(clip, sourceFrame, source.size());
    if (mask.isNull()) {
        return failClosedMaskMatte();
    }

    if (clip.maskShowOnly) {
        QImage output(mask.size(), QImage::Format_ARGB32_Premultiplied);
        for (int y = 0; y < output.height(); ++y) {
            const uchar* maskRow = mask.constScanLine(y);
            QRgb* dstRow = reinterpret_cast<QRgb*>(output.scanLine(y));
            for (int x = 0; x < output.width(); ++x) {
                const int v = maskRow[x];
                dstRow[x] = qRgba(v, v, v, v);
            }
        }
        return output;
    }

    QImage original = source.convertToFormat(QImage::Format_ARGB32_Premultiplied);
    const bool generatedMaskMatte = clip.clipRole == ClipRole::MaskMatte;
    QImage base = applyClipGrade(original, clipGrade)
                      .convertToFormat(QImage::Format_ARGB32_Premultiplied);
    QImage output(base.size(), QImage::Format_ARGB32_Premultiplied);
    output.fill(Qt::transparent);

    if (clip.maskDropShadowEnabled && clip.maskDropShadowOpacity > 0.0) {
        QImage shadowMask = clip.maskDropShadowRadius > 0.0
                                ? blurMask(mask, qRound(clip.maskDropShadowRadius))
                                : mask;
        QImage shadow(base.size(), QImage::Format_ARGB32_Premultiplied);
        shadow.fill(Qt::transparent);
        for (int y = 0; y < shadow.height(); ++y) {
            const uchar* maskRow = shadowMask.constScanLine(y);
            QRgb* dst = reinterpret_cast<QRgb*>(shadow.scanLine(y));
            for (int x = 0; x < shadow.width(); ++x) {
                const int alpha =
                    qBound(0, qRound(maskRow[x] * clip.maskDropShadowOpacity), 255);
                dst[x] = qRgba(0, 0, 0, alpha);
            }
        }
        QPainter painter(&output);
        painter.drawImage(QPointF(clip.maskDropShadowOffsetX, clip.maskDropShadowOffsetY), shadow);
        painter.end();
    }

    if (generatedMaskMatte) {
        QImage foreground = base.convertToFormat(QImage::Format_ARGB32);
        const qreal opacity = qBound<qreal>(0.0, clip.maskOpacity, 1.0);
        for (int y = 0; y < foreground.height(); ++y) {
            const uchar* maskRow = mask.constScanLine(y);
            QRgb* dstRow = reinterpret_cast<QRgb*>(foreground.scanLine(y));
            for (int x = 0; x < foreground.width(); ++x) {
                dstRow[x] = qRgba(qRed(dstRow[x]),
                                  qGreen(dstRow[x]),
                                  qBlue(dstRow[x]),
                                  qBound(0, qRound(qAlpha(dstRow[x]) *
                                                  (maskRow[x] / 255.0) * opacity), 255));
            }
        }
        QPainter foregroundPainter(&output);
        foregroundPainter.drawImage(0, 0, foreground);
        foregroundPainter.end();
    } else {
        QPainter basePainter(&output);
        basePainter.drawImage(0, 0, base);
        basePainter.end();
    }
    if (!generatedMaskMatte && clip.maskGradeEnabled) {
        const QImage maskedGrade = applyClipGrade(original, maskGradeForClip(clip))
                                       .convertToFormat(QImage::Format_ARGB32_Premultiplied);
        const qreal opacity = qBound<qreal>(0.0, clip.maskOpacity, 1.0);
        for (int y = 0; y < output.height(); ++y) {
            const uchar* maskRow = mask.constScanLine(y);
            const QRgb* gradeRow = reinterpret_cast<const QRgb*>(maskedGrade.constScanLine(y));
            QRgb* dstRow = reinterpret_cast<QRgb*>(output.scanLine(y));
            for (int x = 0; x < output.width(); ++x) {
                const qreal amount = (maskRow[x] / 255.0) * opacity;
                if (amount <= 0.0) continue;
                const QRgb basePixel = dstRow[x];
                const QRgb gradePixel = gradeRow[x];
                auto mixChannel = [amount](int baseValue, int gradeValue) {
                    return qBound(0, qRound(baseValue + (gradeValue - baseValue) * amount), 255);
                };
                dstRow[x] = qRgba(mixChannel(qRed(basePixel), qRed(gradePixel)),
                                  mixChannel(qGreen(basePixel), qGreen(gradePixel)),
                                  mixChannel(qBlue(basePixel), qBlue(gradePixel)),
                                  mixChannel(qAlpha(basePixel), qAlpha(gradePixel)));
            }
        }
    }
    return output.convertToFormat(QImage::Format_ARGB32_Premultiplied);
}

QImage applyEffectiveClipVisualEffectsToImage(const QImage& source, const EffectiveVisualEffects& effects) {
    QImage output = applyClipGrade(source, effects.grading);
    if (!effects.correctionPolygons.isEmpty() && !output.isNull()) {
        output = output.convertToFormat(QImage::Format_ARGB32_Premultiplied);
        QPainter painter(&output);
        painter.setRenderHint(QPainter::Antialiasing, true);
        painter.setCompositionMode(QPainter::CompositionMode_DestinationOut);
        painter.setBrush(Qt::black);
        painter.setPen(Qt::NoPen);
        const qreal width = qMax<qreal>(1.0, output.width());
        const qreal height = qMax<qreal>(1.0, output.height());
        for (const TimelineClip::CorrectionPolygon& polygon : effects.correctionPolygons) {
            if (!polygon.enabled || polygon.pointsNormalized.size() < 3) {
                continue;
            }
            QPainterPath path;
            const QPointF first(
                qBound<qreal>(0.0, polygon.pointsNormalized.constFirst().x(), 1.0) * width,
                qBound<qreal>(0.0, polygon.pointsNormalized.constFirst().y(), 1.0) * height);
            path.moveTo(first);
            for (int i = 1; i < polygon.pointsNormalized.size(); ++i) {
                const QPointF point(
                    qBound<qreal>(0.0, polygon.pointsNormalized[i].x(), 1.0) * width,
                    qBound<qreal>(0.0, polygon.pointsNormalized[i].y(), 1.0) * height);
                path.lineTo(point);
            }
            path.closeSubpath();
            painter.drawPath(path);
        }
        painter.end();
    }
    if (effects.maskFeather > 0.0) {
        output = applyMaskFeather(output, effects.maskFeather, effects.maskFeatherGamma,
                                  effects.maskFeatherFalloff);
    }
    return output;
}

QImage applyMaskFeather(const QImage& source, qreal featherRadius, qreal featherGamma,
                        int featherFalloff) {
    if (source.isNull() || featherRadius <= 0.0) {
        return source;
    }

    QImage feathered = source.convertToFormat(QImage::Format_ARGB32);
    const int radius = qRound(featherRadius);
    if (radius <= 0) {
        return source;
    }

    // Create a copy for reading
    const QImage sourceCopy = feathered.copy();
    const int width = feathered.width();
    const int height = feathered.height();

    // Box blur on the alpha channel with gamma curve
    const qreal gamma = qMax(0.01, featherGamma);
    for (int y = 0; y < height; ++y) {
        QRgb* destRow = reinterpret_cast<QRgb*>(feathered.scanLine(y));
        for (int x = 0; x < width; ++x) {
            int alphaSum = 0;
            int pixelCount = 0;

            // Sample the box
            for (int dy = -radius; dy <= radius; ++dy) {
                const int sampleY = qBound(0, y + dy, height - 1);
                const QRgb* srcRow = reinterpret_cast<const QRgb*>(sourceCopy.scanLine(sampleY));
                for (int dx = -radius; dx <= radius; ++dx) {
                    const int sampleX = qBound(0, x + dx, width - 1);
                    alphaSum += qAlpha(srcRow[sampleX]);
                    pixelCount++;
                }
            }

            // Box blur average
            const qreal blurredAlpha = static_cast<qreal>(alphaSum) / pixelCount / 255.0;
            const qreal t = qBound<qreal>(0.0, blurredAlpha, 1.0);
            qreal shapedAlpha = t;
            switch (qBound(0, featherFalloff, 5)) {
            case 1: // Linear
                break;
            case 2: // Smoothstep: C1-continuous, common compositing falloff.
                shapedAlpha = t * t * (3.0 - 2.0 * t);
                break;
            case 3: // Smootherstep: C2-continuous for especially clean motion.
                shapedAlpha = t * t * t * (t * (t * 6.0 - 15.0) + 10.0);
                break;
            case 4: // Raised cosine.
                shapedAlpha = 0.5 - 0.5 * std::cos(t * 3.14159265358979323846);
                break;
            case 5: { // Gaussian-style S curve, normalized to [0, 1].
                constexpr qreal k = 4.0;
                const qreal lo = std::exp(-k);
                shapedAlpha = (std::exp(-k * (1.0 - t) * (1.0 - t)) - lo) /
                              (1.0 - lo);
                break;
            }
            case 0:
            default:
                shapedAlpha = std::pow(t, 1.0 / gamma);
                break;
            }
            const int newAlpha = qBound(0, qRound(shapedAlpha * 255.0), 255);
            const QRgb original = destRow[x];
            destRow[x] = qRgba(qRed(original), qGreen(original), qBlue(original), newAlpha);
        }
    }

    return feathered.convertToFormat(QImage::Format_ARGB32_Premultiplied);
}
