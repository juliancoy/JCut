#include "editor_shared.h"
#include "transform_skip_aware_timing.h"
#include "debug_controls.h"

#include <QDir>
#include <QCollator>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QHash>
#include <QMutex>
#include <QMutexLocker>
#include <QPainter>
#include <QPainterPath>
#include <QRegularExpression>
#include <QSet>

#include <algorithm>
#include <cmath>

extern "C" {
#include <libavformat/avformat.h>
#include <libavutil/avutil.h>
#include <libavutil/pixdesc.h>
}

namespace {
bool isImageSuffix(const QString& suffix) {
    static const QSet<QString> kImageSuffixes = {
        QStringLiteral("png"),
        QStringLiteral("jpg"),
        QStringLiteral("jpeg"),
        QStringLiteral("webp"),
        QStringLiteral("tga"),
        QStringLiteral("tif"),
        QStringLiteral("tiff"),
        QStringLiteral("exr"),
        QStringLiteral("bmp"),
        QStringLiteral("gif"),
    };
    return kImageSuffixes.contains(suffix);
}

bool isVideoSuffix(const QString& suffix) {
    static const QSet<QString> kVideoSuffixes = {
        QStringLiteral("mp4"),
        QStringLiteral("mov"),
        QStringLiteral("mkv"),
        QStringLiteral("webm"),
        QStringLiteral("avi"),
        QStringLiteral("m4v"),
        QStringLiteral("mpg"),
        QStringLiteral("mpeg"),
        QStringLiteral("mxf"),
    };
    return kVideoSuffixes.contains(suffix.toLower());
}

bool isValidMediaFile(const QString& path) {
    const QFileInfo info(path);
    if (!info.exists() || !info.isFile()) {
        return false;
    }
    const QString suffix = info.suffix().toLower();
    return isImageSuffix(suffix) || isVideoSuffix(suffix);
}

int clampChannel(int value) {
    return qBound(0, value, 255);
}

bool detectVariableFrameRate(const QString& path) {
    AVFormatContext* formatCtx = nullptr;
    const QByteArray pathBytes = QFile::encodeName(path);
    if (avformat_open_input(&formatCtx, pathBytes.constData(), nullptr, nullptr) < 0) {
        return false;
    }

    bool isVfr = false;
    if (avformat_find_stream_info(formatCtx, nullptr) >= 0) {
        for (unsigned i = 0; i < formatCtx->nb_streams; ++i) {
            AVStream* stream = formatCtx->streams[i];
            if (!stream || !stream->codecpar) continue;
            if (stream->codecpar->codec_type == AVMEDIA_TYPE_VIDEO) {
                const AVRational& avgFrameRate = stream->avg_frame_rate;
                const AVRational& rFrameRate = stream->r_frame_rate;
                // VFR if avg and real frame rates differ significantly
                if (avgFrameRate.num > 0 && avgFrameRate.den > 0 &&
                    rFrameRate.num > 0 && rFrameRate.den > 0) {
                    const double avgFps = av_q2d(avgFrameRate);
                    const double realFps = av_q2d(rFrameRate);
                    // If they differ by more than 1%, consider it VFR
                    if (qAbs(avgFps - realFps) > 0.01 * qMax(avgFps, realFps)) {
                        isVfr = true;
                    }
                }
                break;
            }
        }
    }
    avformat_close_input(&formatCtx);
    return isVfr;
}

QStringList collectSequenceFrames(const QString& path) {
    static QMutex cacheMutex;
    static QHash<QString, QStringList> cachedFramesByKey;

    const QFileInfo dirInfo(path);
    if (!dirInfo.exists() || !dirInfo.isDir()) {
        return {};
    }

    const QString cacheKey = dirInfo.absoluteFilePath() + QLatin1Char('|') +
                             QString::number(dirInfo.lastModified().toMSecsSinceEpoch());
    {
        QMutexLocker locker(&cacheMutex);
        const auto it = cachedFramesByKey.constFind(cacheKey);
        if (it != cachedFramesByKey.cend()) {
            return it.value();
        }
    }

    const QDir dir(dirInfo.absoluteFilePath());
    QFileInfoList entries = dir.entryInfoList(QDir::Files | QDir::NoDotAndDotDot, QDir::Name | QDir::IgnoreCase);
    if (entries.size() < 2) {
        return {};
    }

    QHash<QString, QFileInfoList> bySuffix;
    for (const QFileInfo& entry : entries) {
        const QString suffix = entry.suffix().toLower();
        if (!isImageSuffix(suffix)) {
            continue;
        }
        bySuffix[suffix].push_back(entry);
    }

    QFileInfoList bestGroup;
    for (auto it = bySuffix.begin(); it != bySuffix.end(); ++it) {
        if (it.value().size() > bestGroup.size()) {
            bestGroup = it.value();
        }
    }
    if (bestGroup.size() < 2) {
        return {};
    }

    static const QRegularExpression kDigitsPattern(QStringLiteral("(\\d+)"));
    int numberedCount = 0;
    for (const QFileInfo& entry : bestGroup) {
        if (kDigitsPattern.match(entry.completeBaseName()).hasMatch()) {
            ++numberedCount;
        }
    }
    if (numberedCount < 2 || (numberedCount * 2) < bestGroup.size()) {
        return {};
    }

    QCollator collator;
    collator.setNumericMode(true);
    collator.setCaseSensitivity(Qt::CaseInsensitive);
    std::sort(bestGroup.begin(), bestGroup.end(), [&collator](const QFileInfo& a, const QFileInfo& b) {
        return collator.compare(a.fileName(), b.fileName()) < 0;
    });

    // Safety limit: don't treat directories with too many files as image sequences
    // This prevents memory exhaustion and potential heap corruption with large directories
    constexpr int kMaxSequenceFrames = 500000;
    if (bestGroup.size() > kMaxSequenceFrames) {
        qWarning() << "Directory has too many image files to be a sequence:"
                   << bestGroup.size() << "(max:" << kMaxSequenceFrames << ")";
        return {};
    }

    QStringList frames;
    frames.reserve(bestGroup.size());
    for (const QFileInfo& entry : bestGroup) {
        frames.push_back(entry.absoluteFilePath());
    }
    {
        QMutexLocker locker(&cacheMutex);
        cachedFramesByKey.insert(cacheKey, frames);
    }
    return frames;
}

struct RenderSyncClipLookup {
    QVector<int64_t> timelineFrames;
    QVector<int> cumulativeDelta;
};

struct RenderSyncLookupCache {
    const QVector<RenderSyncMarker>* source = nullptr;
    int size = -1;
    quint64 signature = 0;
    QHash<QString, RenderSyncClipLookup> byClip;
};

quint64 markerQuickSignature(const QVector<RenderSyncMarker>& markers) {
    quint64 sig = static_cast<quint64>(markers.size()) * 1469598103934665603ULL;
    if (markers.isEmpty()) {
        return sig;
    }
    auto mixMarker = [&sig](const RenderSyncMarker& marker) {
        sig ^= static_cast<quint64>(qHash(marker.clipId));
        sig = (sig * 1099511628211ULL) ^ static_cast<quint64>(marker.frame);
        sig = (sig * 1099511628211ULL) ^ static_cast<quint64>(marker.count);
        sig = (sig * 1099511628211ULL) ^ static_cast<quint64>(static_cast<int>(marker.action));
    };
    mixMarker(markers.constFirst());
    if (markers.size() > 1) {
        mixMarker(markers[markers.size() / 2]);
        mixMarker(markers.constLast());
    }
    return sig;
}

int markerDelta(const RenderSyncMarker& marker) {
    const int magnitude = qMax(1, marker.count);
    return marker.action == RenderSyncAction::DuplicateFrame ? -magnitude : magnitude;
}

void rebuildRenderSyncLookupCache(RenderSyncLookupCache& cache,
                                  const QVector<RenderSyncMarker>& markers) {
    cache.byClip.clear();
    if (markers.isEmpty()) {
        return;
    }

    QHash<QString, QVector<RenderSyncMarker>> grouped;
    grouped.reserve(markers.size());
    for (const RenderSyncMarker& marker : markers) {
        grouped[marker.clipId].push_back(marker);
    }

    for (auto it = grouped.begin(); it != grouped.end(); ++it) {
        QVector<RenderSyncMarker>& clipMarkers = it.value();
        std::sort(clipMarkers.begin(), clipMarkers.end(),
                  [](const RenderSyncMarker& a, const RenderSyncMarker& b) {
                      if (a.frame == b.frame) {
                          return static_cast<int>(a.action) < static_cast<int>(b.action);
                      }
                      return a.frame < b.frame;
                  });
        RenderSyncClipLookup lookup;
        lookup.timelineFrames.reserve(clipMarkers.size());
        lookup.cumulativeDelta.reserve(clipMarkers.size());
        int runningDelta = 0;
        for (const RenderSyncMarker& marker : clipMarkers) {
            runningDelta += markerDelta(marker);
            lookup.timelineFrames.push_back(marker.frame);
            lookup.cumulativeDelta.push_back(runningDelta);
        }
        cache.byClip.insert(it.key(), std::move(lookup));
    }
}

const RenderSyncClipLookup* lookupForClip(const QVector<RenderSyncMarker>& markers,
                                          const QString& clipId) {
    thread_local RenderSyncLookupCache cache;
    const quint64 signature = markerQuickSignature(markers);
    if (cache.source != &markers || cache.size != markers.size() || cache.signature != signature) {
        cache.source = &markers;
        cache.size = markers.size();
        cache.signature = signature;
        rebuildRenderSyncLookupCache(cache, markers);
    }
    auto it = cache.byClip.constFind(clipId);
    if (it == cache.byClip.constEnd()) {
        return nullptr;
    }
    return &it.value();
}
}

bool isVariableFrameRate(const QString& path) {
    return detectVariableFrameRate(path);
}

QString clipMediaTypeToString(ClipMediaType type) {
    switch (type) {
    case ClipMediaType::Image:
        return QStringLiteral("image");
    case ClipMediaType::Video:
        return QStringLiteral("video");
    case ClipMediaType::Audio:
        return QStringLiteral("audio");
    case ClipMediaType::Title:
        return QStringLiteral("title");
    case ClipMediaType::Unknown:
    default:
        return QStringLiteral("unknown");
    }
}

ClipMediaType clipMediaTypeFromString(const QString& value) {
    if (value == QStringLiteral("image")) return ClipMediaType::Image;
    if (value == QStringLiteral("video")) return ClipMediaType::Video;
    if (value == QStringLiteral("audio")) return ClipMediaType::Audio;
    if (value == QStringLiteral("title")) return ClipMediaType::Title;
    return ClipMediaType::Unknown;
}

QString clipMediaTypeLabel(ClipMediaType type) {
    switch (type) {
    case ClipMediaType::Image:
        return QStringLiteral("Image");
    case ClipMediaType::Video:
        return QStringLiteral("Video");
    case ClipMediaType::Audio:
        return QStringLiteral("Audio");
    case ClipMediaType::Title:
        return QStringLiteral("Title");
    case ClipMediaType::Unknown:
    default:
        return QStringLiteral("Unknown");
    }
}

QString mediaSourceKindToString(MediaSourceKind kind) {
    switch (kind) {
    case MediaSourceKind::ImageSequence:
        return QStringLiteral("image_sequence");
    case MediaSourceKind::File:
    default:
        return QStringLiteral("file");
    }
}

MediaSourceKind mediaSourceKindFromString(const QString& value) {
    if (value == QStringLiteral("image_sequence")) return MediaSourceKind::ImageSequence;
    return MediaSourceKind::File;
}

QString mediaSourceKindLabel(MediaSourceKind kind) {
    switch (kind) {
    case MediaSourceKind::ImageSequence:
        return QStringLiteral("Image Sequence");
    case MediaSourceKind::File:
    default:
        return QStringLiteral("File");
    }
}

QString trackVisualModeToString(TrackVisualMode mode) {
    switch (mode) {
    case TrackVisualMode::Enabled:
        return QStringLiteral("enabled");
    case TrackVisualMode::ForceOpaque:
        return QStringLiteral("force_opaque");
    case TrackVisualMode::Hidden:
        return QStringLiteral("hidden");
    default:
        return QStringLiteral("enabled");
    }
}

TrackVisualMode trackVisualModeFromString(const QString& value) {
    const QString normalized = value.trimmed().toLower();
    if (normalized == QStringLiteral("force_opaque")) {
        return TrackVisualMode::ForceOpaque;
    }
    if (normalized == QStringLiteral("hidden")) {
        return TrackVisualMode::Hidden;
    }
    return TrackVisualMode::Enabled;
}

QString trackVisualModeLabel(TrackVisualMode mode) {
    switch (mode) {
    case TrackVisualMode::Enabled:
        return QStringLiteral("Visible");
    case TrackVisualMode::ForceOpaque:
        return QStringLiteral("Force Opaque");
    case TrackVisualMode::Hidden:
        return QStringLiteral("Hidden");
    default:
        return QStringLiteral("Visible");
    }
}

QString renderSyncActionToString(RenderSyncAction action) {
    switch (action) {
    case RenderSyncAction::DuplicateFrame:
        return QStringLiteral("duplicate");
    case RenderSyncAction::SkipFrame:
        return QStringLiteral("skip");
    default:
        return QStringLiteral("duplicate");
    }
}

RenderSyncAction renderSyncActionFromString(const QString& value) {
    if (value == QStringLiteral("skip")) {
        return RenderSyncAction::SkipFrame;
    }
    return RenderSyncAction::DuplicateFrame;
}

QString renderSyncActionLabel(RenderSyncAction action) {
    switch (action) {
    case RenderSyncAction::DuplicateFrame:
        return QStringLiteral("Duplicate");
    case RenderSyncAction::SkipFrame:
        return QStringLiteral("Skip");
    default:
        return QStringLiteral("Duplicate");
    }
}

bool clipHasVisuals(const TimelineClip& clip) {
    return clip.mediaType == ClipMediaType::Image ||
           clip.mediaType == ClipMediaType::Video ||
           clip.mediaType == ClipMediaType::Title ||
           clip.sourceKind == MediaSourceKind::ImageSequence;
}

bool clipHasAlpha(const TimelineClip& clip) {
    if (!clipHasVisuals(clip)) {
        return false;
    }

    // Title clips always support alpha
    if (clip.mediaType == ClipMediaType::Title) {
        return true;
    }

    // For image sequences, check the first frame's extension
    if (clip.sourceKind == MediaSourceKind::ImageSequence) {
        const QStringList frames = imageSequenceFramePaths(clip.filePath);
        if (!frames.isEmpty()) {
            const QString frameSuffix = QFileInfo(frames.constFirst()).suffix().toLower();
            const QStringList alphaImageFormats = {
                QStringLiteral("png"), QStringLiteral("tga"), QStringLiteral("tiff"),
                QStringLiteral("tif"), QStringLiteral("exr"), QStringLiteral("webp")
            };
            if (alphaImageFormats.contains(frameSuffix)) {
                return true;
            }
        }
        // Fall through to probe
    }

    const QString suffix = QFileInfo(clip.filePath).suffix().toLower();
    const QStringList alphaFormats = {
        QStringLiteral("png"),
        QStringLiteral("tga"),
        QStringLiteral("tiff"),
        QStringLiteral("tif"),
        QStringLiteral("exr"),
        QStringLiteral("webp"),
        QStringLiteral("mov"),  // QuickTime can have alpha
        QStringLiteral("prores"),
    };

    if (alphaFormats.contains(suffix)) {
        return true;
    }

    // Fast-path formats that are effectively non-alpha in this editor context.
    static const QSet<QString> kNoAlphaProbeFormats = {
        QStringLiteral("mp4"),
        QStringLiteral("m4v"),
        QStringLiteral("avi"),
        QStringLiteral("mpg"),
        QStringLiteral("mpeg"),
        QStringLiteral("mts"),
        QStringLiteral("m2ts"),
        QStringLiteral("ts"),
    };
    if (kNoAlphaProbeFormats.contains(suffix)) {
        return false;
    }

    // Cache probe results to avoid repeated expensive FFmpeg opens on playhead/UI refresh.
    struct AlphaProbeCacheEntry {
        qint64 size = -1;
        qint64 modifiedMs = -1;
        bool hasAlpha = false;
    };
    static QMutex cacheMutex;
    static QHash<QString, AlphaProbeCacheEntry> cacheByPath;

    const QFileInfo info(clip.filePath);
    const QString cacheKey = info.absoluteFilePath();
    const qint64 size = info.size();
    const qint64 modifiedMs = info.lastModified().toMSecsSinceEpoch();

    {
        QMutexLocker locker(&cacheMutex);
        const auto it = cacheByPath.constFind(cacheKey);
        if (it != cacheByPath.cend() &&
            it->size == size &&
            it->modifiedMs == modifiedMs) {
            return it->hasAlpha;
        }
    }

    const MediaProbeResult probe = probeMediaFile(clip.filePath);
    const bool hasAlpha = probe.hasAlpha;

    {
        QMutexLocker locker(&cacheMutex);
        cacheByPath.insert(cacheKey, AlphaProbeCacheEntry{size, modifiedMs, hasAlpha});
    }
    return hasAlpha;
}

bool clipIsAudioOnly(const TimelineClip& clip) {
    return clip.mediaType == ClipMediaType::Audio;
}

bool clipHasCorrections(const TimelineClip& clip) {
    for (const TimelineClip::CorrectionPolygon& polygon : clip.correctionPolygons) {
        if (polygon.enabled && polygon.pointsNormalized.size() >= 3) {
            return true;
        }
    }
    return false;
}

bool correctionPolygonActiveAtTimelineFrame(const TimelineClip& clip,
                                            const TimelineClip::CorrectionPolygon& polygon,
                                            int64_t timelineFrame) {
    if (!polygon.enabled || polygon.pointsNormalized.size() < 3) {
        return false;
    }
    const int64_t localFrame = qMax<int64_t>(0, timelineFrame - clip.startFrame);
    const int64_t start = qMax<int64_t>(0, polygon.startFrame);
    if (localFrame < start) {
        return false;
    }
    return polygon.endFrame < 0 || localFrame <= polygon.endFrame;
}

bool correctionPolygonActiveAtTimelinePosition(const TimelineClip& clip,
                                               const TimelineClip::CorrectionPolygon& polygon,
                                               qreal timelineFramePosition) {
    return correctionPolygonActiveAtTimelineFrame(
        clip,
        polygon,
        static_cast<int64_t>(std::floor(timelineFramePosition)));
}

bool clipVisualPlaybackEnabled(const TimelineClip& clip) {
    return clipHasVisuals(clip) && clip.videoEnabled;
}

TrackVisualMode trackVisualModeForClip(const TimelineClip& clip, const QVector<TimelineTrack>& tracks) {
    if (clip.trackIndex >= 0 && clip.trackIndex < tracks.size()) {
        return tracks[clip.trackIndex].visualMode;
    }
    return TrackVisualMode::Enabled;
}

bool clipVisualPlaybackEnabled(const TimelineClip& clip, const QVector<TimelineTrack>& tracks) {
    if (!clipVisualPlaybackEnabled(clip)) {
        return false;
    }
    return trackVisualModeForClip(clip, tracks) != TrackVisualMode::Hidden;
}

bool clipAudioPlaybackEnabled(const TimelineClip& clip) {
    return clip.hasAudio && clip.audioEnabled;
}

int64_t frameToSamples(int64_t frame) {
    return qMax<int64_t>(0, frame * kSamplesPerFrame);
}

qreal samplesToFramePosition(int64_t samples) {
    return static_cast<qreal>(samples) / static_cast<qreal>(kSamplesPerFrame);
}

qreal resolvedSourceFps(const TimelineClip& clip) {
    const qreal fps = clip.sourceFps;
    if (!std::isfinite(fps) || fps <= 0.001) {
        return static_cast<qreal>(kTimelineFps);
    }
    return fps;
}

int64_t sourceFramesToSamples(const TimelineClip& clip, qreal sourceFrames) {
    const qreal clampedSourceFrames = qMax<qreal>(0.0, sourceFrames);
    const qreal durationSeconds = clampedSourceFrames / resolvedSourceFps(clip);
    return qMax<int64_t>(0, qRound64(durationSeconds * static_cast<qreal>(kAudioSampleRate)));
}

int64_t clipTimelineStartSamples(const TimelineClip& clip) {
    return frameToSamples(clip.startFrame) + clip.startSubframeSamples;
}

int64_t clipSourceInSamples(const TimelineClip& clip) {
    return sourceFramesToSamples(clip, static_cast<qreal>(clip.sourceInFrame)) + clip.sourceInSubframeSamples;
}

void normalizeSubframeTiming(int64_t& frame, int64_t& subframeSamples) {
    if (subframeSamples >= kSamplesPerFrame) {
        frame += subframeSamples / kSamplesPerFrame;
        subframeSamples %= kSamplesPerFrame;
    }
    while (subframeSamples < 0 && frame > 0) {
        --frame;
        subframeSamples += kSamplesPerFrame;
    }
    if (frame <= 0) {
        frame = 0;
        subframeSamples = qMax<int64_t>(0, subframeSamples);
    }
}

void normalizeClipTiming(TimelineClip& clip) {
    normalizeSubframeTiming(clip.startFrame, clip.startSubframeSamples);
    normalizeSubframeTiming(clip.sourceInFrame, clip.sourceInSubframeSamples);
    clip.playbackRate = qBound<qreal>(0.001, clip.playbackRate, 1000.0);
}

QString transformInterpolationLabel(bool linearInterpolation) {
    return linearInterpolation ? QStringLiteral("Linear") : QStringLiteral("Step");
}

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
            normalized.push_back(keyframe);
        } else if (normalized.constFirst().frame > 0) {
            // FIX: Create a keyframe at frame 0 with clip's base values
            // instead of duplicating the first keyframe
            TimelineClip::GradingKeyframe baseKeyframe;
            baseKeyframe.frame = 0;
            baseKeyframe.brightness = clip.brightness;
            baseKeyframe.contrast = clip.contrast;
            baseKeyframe.saturation = clip.saturation;
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

        auto nearlyEqual = [](qreal a, qreal b) {
            return std::abs(a - b) <= 0.000001;
        };

        auto gradingValuesMatch = [&nearlyEqual](const TimelineClip::GradingKeyframe& a,
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
                   nearlyEqual(a.highlightsB, b.highlightsB);
        };

        auto blended = [](const TimelineClip::GradingKeyframe& previous,
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
            return out;
        };

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
            const TimelineClip::GradingKeyframe expected = blended(previous, next, t);
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

TimelineClip::GradingKeyframe evaluateClipGradingAtFrame(const TimelineClip& clip, int64_t timelineFrame) {
    TimelineClip::GradingKeyframe state;
    state.brightness = clip.brightness;
    state.contrast = clip.contrast;
    state.saturation = clip.saturation;
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
                return previous;
            }
            const qreal t = static_cast<qreal>(localFrame - previous.frame) /
                            static_cast<qreal>(current.frame - previous.frame);
            state.frame = localFrame;
            state.brightness = previous.brightness + ((current.brightness - previous.brightness) * t);
            state.contrast = previous.contrast + ((current.contrast - previous.contrast) * t);
            state.saturation = previous.saturation + ((current.saturation - previous.saturation) * t);
            // Shadows/Midtones/Highlights interpolation
            state.shadowsR = previous.shadowsR + ((current.shadowsR - previous.shadowsR) * t);
            state.shadowsG = previous.shadowsG + ((current.shadowsG - previous.shadowsG) * t);
            state.shadowsB = previous.shadowsB + ((current.shadowsB - previous.shadowsB) * t);
            state.midtonesR = previous.midtonesR + ((current.midtonesR - previous.midtonesR) * t);
            state.midtonesG = previous.midtonesG + ((current.midtonesG - previous.midtonesG) * t);
            state.midtonesB = previous.midtonesB + ((current.midtonesB - previous.midtonesB) * t);
            state.highlightsR = previous.highlightsR + ((current.highlightsR - previous.highlightsR) * t);
            state.highlightsG = previous.highlightsG + ((current.highlightsG - previous.highlightsG) * t);
            state.highlightsB = previous.highlightsB + ((current.highlightsB - previous.highlightsB) * t);
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
                return previous;
            }
            const qreal t = (localFrame - static_cast<qreal>(previous.frame)) /
                            static_cast<qreal>(current.frame - previous.frame);
            state.frame = qRound64(localFrame);
            state.brightness = previous.brightness + ((current.brightness - previous.brightness) * t);
            state.contrast = previous.contrast + ((current.contrast - previous.contrast) * t);
            state.saturation = previous.saturation + ((current.saturation - previous.saturation) * t);
            // Shadows/Midtones/Highlights interpolation
            state.shadowsR = previous.shadowsR + ((current.shadowsR - previous.shadowsR) * t);
            state.shadowsG = previous.shadowsG + ((current.shadowsG - previous.shadowsG) * t);
            state.shadowsB = previous.shadowsB + ((current.shadowsB - previous.shadowsB) * t);
            state.midtonesR = previous.midtonesR + ((current.midtonesR - previous.midtonesR) * t);
            state.midtonesG = previous.midtonesG + ((current.midtonesG - previous.midtonesG) * t);
            state.midtonesB = previous.midtonesB + ((current.midtonesB - previous.midtonesB) * t);
            state.highlightsR = previous.highlightsR + ((current.highlightsR - previous.highlightsR) * t);
            state.highlightsG = previous.highlightsG + ((current.highlightsG - previous.highlightsG) * t);
            state.highlightsB = previous.highlightsB + ((current.highlightsB - previous.highlightsB) * t);
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
    grade.opacity = evaluateEffectiveClipOpacityAtFrame(clip, tracks, timelineFrame);
    return grade;
}

TimelineClip::GradingKeyframe evaluateEffectiveClipGradingAtPosition(const TimelineClip& clip,
                                                                     const QVector<TimelineTrack>& tracks,
                                                                     qreal timelineFramePosition) {
    TimelineClip::GradingKeyframe grade = evaluateClipGradingAtPosition(clip, timelineFramePosition);
    grade.opacity = evaluateEffectiveClipOpacityAtPosition(clip, tracks, timelineFramePosition);
    return grade;
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
    for (const TimelineClip::CorrectionPolygon& polygon : clip.correctionPolygons) {
        if (correctionPolygonActiveAtTimelineFrame(clip, polygon, timelineFrame)) {
            effects.correctionPolygons.push_back(polygon);
        }
    }
    return effects;
}

namespace {
qreal effectTimelinePositionForClip(const TimelineClip& clip,
                                    qreal timelineFramePosition,
                                    const QVector<RenderSyncMarker>& markers) {
    if (!clip.transformSkipAwareTiming || markers.isEmpty()) {
        return timelineFramePosition;
    }

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
    return static_cast<qreal>(clip.startFrame) + adjustedLocalFramePosition;
}
}  // namespace

EffectiveVisualEffects evaluateEffectiveVisualEffectsAtPosition(const TimelineClip& clip,
                                                                const QVector<TimelineTrack>& tracks,
                                                                qreal timelineFramePosition) {
    EffectiveVisualEffects effects;
    effects.grading = evaluateEffectiveClipGradingAtPosition(clip, tracks, timelineFramePosition);
    effects.maskFeather = clip.maskFeather;
    effects.maskFeatherGamma = clip.maskFeatherGamma;
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
    const qreal adjustedTimelinePosition =
        effectTimelinePositionForClip(clip, timelineFramePosition, markers);
    return evaluateEffectiveVisualEffectsAtPosition(clip, tracks, adjustedTimelinePosition);
}

int64_t adjustedClipLocalFrameAtTimelineFrame(const TimelineClip& clip,
                                              int64_t localTimelineFrame,
                                              const QVector<RenderSyncMarker>& markers) {
    const int64_t boundedLocalFrame = qMax<int64_t>(0, localTimelineFrame);
    const RenderSyncClipLookup* lookup = lookupForClip(markers, clip.id);
    if (!lookup || lookup->timelineFrames.isEmpty()) {
        return boundedLocalFrame;
    }
    const int64_t timelineFrame = clip.startFrame + boundedLocalFrame;
    const auto endIt =
        std::lower_bound(lookup->timelineFrames.begin(), lookup->timelineFrames.end(), timelineFrame);
    if (endIt == lookup->timelineFrames.begin()) {
        return boundedLocalFrame;
    }
    const int index = static_cast<int>(std::distance(lookup->timelineFrames.begin(), endIt) - 1);
    const int delta = lookup->cumulativeDelta[index];
    return qMax<int64_t>(0, boundedLocalFrame + static_cast<int64_t>(delta));
}

int64_t sourceFrameForClipAtTimelinePosition(const TimelineClip& clip,
                                             qreal timelineFramePosition,
                                             const QVector<RenderSyncMarker>& markers) {
    const qreal maxFrame = static_cast<qreal>(qMax<int64_t>(0, clip.durationFrames - 1));
    const qreal localTimelineFramePosition =
        qBound<qreal>(0.0, timelineFramePosition - static_cast<qreal>(clip.startFrame), maxFrame);
    const int64_t steppedLocalTimelineFrame =
        qMax<int64_t>(0, static_cast<int64_t>(std::floor(localTimelineFramePosition)));
    const int64_t adjustedLocalFrame =
        adjustedClipLocalFrameAtTimelineFrame(clip, steppedLocalTimelineFrame, markers);
    
    // Use fixed-point arithmetic for FPS scaling and playback rate
    const qreal sourceFps = resolvedSourceFps(clip);
    const int64_t sourceFpsScaled = qMax<int64_t>(1, static_cast<int64_t>(sourceFps * 1000.0));
    const int64_t timelineFpsScaled = static_cast<int64_t>(kTimelineFps * 1000.0);
    const int64_t playbackRateScaled = qMax<int64_t>(1, static_cast<int64_t>(clip.playbackRate * 1000.0));
    
    // Calculate source frame offset using 64-bit integer arithmetic
    // (adjustedLocalFrame * playbackRate * sourceFps) / timelineFps
    const int64_t numerator = adjustedLocalFrame * playbackRateScaled * sourceFpsScaled;
    const int64_t denominator = timelineFpsScaled * 1000LL; // Extra 1000 for playbackRate scaling
    const int64_t sourceFrameOffset = numerator / denominator;
    
    return qMax<int64_t>(0,
                         qMin<int64_t>(qMax<int64_t>(0, clip.sourceDurationFrames - 1),
                                       clip.sourceInFrame + sourceFrameOffset));
}

int64_t sourceSampleForClipAtTimelineSample(const TimelineClip& clip,
                                            int64_t timelineSample,
                                            const QVector<RenderSyncMarker>& markers) {
    const int64_t clipStartSample = clipTimelineStartSamples(clip);
    const int64_t localTimelineSample = qMax<int64_t>(0, timelineSample - clipStartSample);
    const int64_t maxLocalTimelineSample =
        qMax<int64_t>(0, frameToSamples(qMax<int64_t>(0, clip.durationFrames)) - 1);
    const int64_t boundedLocalTimelineSample = qMin<int64_t>(localTimelineSample, maxLocalTimelineSample);
    
    // Use integer division for frame position to avoid floating-point errors
    const int64_t steppedLocalTimelineFrame = boundedLocalTimelineSample / kSamplesPerFrame;
    const int64_t sampleOffsetWithinFrame = boundedLocalTimelineSample % kSamplesPerFrame;
    
    const int64_t adjustedLocalFrame =
        adjustedClipLocalFrameAtTimelineFrame(clip, steppedLocalTimelineFrame, markers);
    
    // Calculate source sample offset using integer arithmetic
    // Convert adjusted frame to samples, add sample offset, then apply playback rate
    const int64_t adjustedLocalSamples = frameToSamples(adjustedLocalFrame) + sampleOffsetWithinFrame;
    
    // Apply playback rate with fixed-point arithmetic (scaled by 1000 for precision)
    const int64_t playbackRateScaled = qMax<int64_t>(1, static_cast<int64_t>(clip.playbackRate * 1000.0));
    const int64_t sourceSampleOffset = (adjustedLocalSamples * playbackRateScaled) / 1000;
    
    const int64_t sourceSample = clipSourceInSamples(clip) + sourceSampleOffset;
    const int64_t maxSourceSample =
        clipSourceInSamples(clip) +
        qMax<int64_t>(0,
                      sourceFramesToSamples(clip, static_cast<qreal>(qMax<int64_t>(0, clip.sourceDurationFrames))) - 1);
    return qMax<int64_t>(0, qMin<int64_t>(sourceSample, maxSourceSample));
}

MediaProbeResult probeMediaFile(const QString& filePath, int64_t fallbackFrames) {
    QElapsedTimer probeTimer;
    probeTimer.start();
    MediaProbeResult result;
    result.durationFrames = fallbackFrames;

    if (filePath.trimmed().isEmpty()) {
        return result;
    }

    const QFileInfo info(filePath);
    if (!info.exists()) {
        return result;
    }
    if (info.exists() && info.isDir()) {
        const QStringList sequenceFrames = collectSequenceFrames(filePath);
        if (!sequenceFrames.isEmpty()) {
            result.mediaType = ClipMediaType::Video;
            result.sourceKind = MediaSourceKind::ImageSequence;
            result.hasVideo = true;
            result.durationFrames = sequenceFrames.size();
            result.codecName = QStringLiteral("image_sequence");
            QImage firstImage(sequenceFrames.constFirst());
            result.hasAlpha = !firstImage.isNull() && firstImage.hasAlphaChannel();
            if (!firstImage.isNull()) {
                result.frameSize = firstImage.size();
            }
        }
        return result;
    }
    const QString suffix = info.suffix().toLower();
    if (isImageSuffix(suffix)) {
        result.mediaType = ClipMediaType::Image;
        return result;
    }

    AVFormatContext* formatCtx = nullptr;
    const QByteArray pathBytes = QFile::encodeName(filePath);
    if (avformat_open_input(&formatCtx, pathBytes.constData(), nullptr, nullptr) < 0) {
        return result;
    }

    if (avformat_find_stream_info(formatCtx, nullptr) >= 0) {
        double durationSeconds = 0.0;
        double sourceFps = 30.0;
        if (formatCtx->duration > 0) {
            durationSeconds = formatCtx->duration / static_cast<double>(AV_TIME_BASE);
        }

        for (unsigned i = 0; i < formatCtx->nb_streams; ++i) {
            const AVStream* stream = formatCtx->streams[i];
            if (!stream || !stream->codecpar) {
                continue;
            }
            if (stream->codecpar->codec_type == AVMEDIA_TYPE_VIDEO) {
                result.hasVideo = true;
                if (result.frameSize.isEmpty() &&
                    stream->codecpar->width > 0 && stream->codecpar->height > 0) {
                    result.frameSize = QSize(stream->codecpar->width, stream->codecpar->height);
                }
                if (result.codecName.isEmpty()) {
                    result.codecName = QString::fromUtf8(avcodec_get_name(stream->codecpar->codec_id));
                }
                const AVPixFmtDescriptor* descriptor =
                    av_pix_fmt_desc_get(static_cast<AVPixelFormat>(stream->codecpar->format));
                if (descriptor && (descriptor->flags & AV_PIX_FMT_FLAG_ALPHA)) {
                    result.hasAlpha = true;
                }
                // Calculate source FPS from the video stream
                const AVRational framerate = av_guess_frame_rate(formatCtx, const_cast<AVStream*>(stream), nullptr);
                if (framerate.num > 0 && framerate.den > 0) {
                    sourceFps = av_q2d(framerate);
                }
            } else if (stream->codecpar->codec_type == AVMEDIA_TYPE_AUDIO) {
                result.hasAudio = true;
            }
            if (durationSeconds <= 0.0 && stream->duration != AV_NOPTS_VALUE) {
                const double streamSeconds = stream->duration * av_q2d(stream->time_base);
                if (streamSeconds > durationSeconds) {
                    durationSeconds = streamSeconds;
                }
            }
        }

        if (durationSeconds > 0.0) {
            result.durationFrames = qMax<int64_t>(1, qRound64(durationSeconds * sourceFps));
            result.fps = sourceFps;
        }
    }

    avformat_close_input(&formatCtx);

    if (result.hasVideo) {
        result.mediaType = ClipMediaType::Video;
    } else if (result.hasAudio) {
        result.mediaType = ClipMediaType::Audio;
    }

    const qint64 elapsedMs = probeTimer.elapsed();
    if (editor::debugDecodeWarnEnabled() && elapsedMs >= 80) {
        qWarning().noquote()
            << QStringLiteral("[DECODE WARN] slow probeMediaFile: %1 ms | path=%2 hasVideo=%3 hasAudio=%4 codec=%5")
                   .arg(elapsedMs)
                   .arg(filePath)
                   .arg(result.hasVideo)
                   .arg(result.hasAudio)
                   .arg(result.codecName.isEmpty() ? QStringLiteral("unknown") : result.codecName);
    } else if (editor::debugDecodeLevel() >= editor::DebugLogLevel::Info &&
               (editor::debugDecodeVerboseEnabled() || elapsedMs >= 20)) {
        qDebug().noquote()
            << QStringLiteral("[DECODE] probeMediaFile: %1 ms | path=%2 hasVideo=%3 hasAudio=%4 codec=%5 fps=%6")
                   .arg(elapsedMs)
                   .arg(filePath)
                   .arg(result.hasVideo)
                   .arg(result.hasAudio)
                   .arg(result.codecName.isEmpty() ? QStringLiteral("unknown") : result.codecName)
                   .arg(result.fps, 0, 'f', 3);
    }

    return result;
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

    if (source.isNull() || (!needsBasicGrade && !needsToneGrade)) {
        return source;
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
        output = applyMaskFeather(output, effects.maskFeather, effects.maskFeatherGamma);
    }
    return output;
}

QImage applyMaskFeather(const QImage& source, qreal featherRadius, qreal featherGamma) {
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
            // Apply gamma curve (1.0 = linear, <1.0 = sharper, >1.0 = softer)
            const qreal curvedAlpha = std::pow(blurredAlpha, 1.0 / gamma);
            const int newAlpha = qBound(0, qRound(curvedAlpha * 255.0), 255);
            const QRgb original = destRow[x];
            destRow[x] = qRgba(qRed(original), qGreen(original), qBlue(original), newAlpha);
        }
    }

    return feathered.convertToFormat(QImage::Format_ARGB32_Premultiplied);
}

qreal effectiveFpsForClip(const TimelineClip& clip) {
    // If clip has explicit source FPS, use that (considering proxy if available)
    if (clip.sourceFps > 0) {
        return clip.sourceFps;
    }
    // Otherwise probe the media file
    const QString mediaPath = playbackMediaPathForClip(clip);
    if (!mediaPath.isEmpty()) {
        const MediaProbeResult probe = probeMediaFile(mediaPath, clip.durationFrames);
        return probe.fps;
    }
    return kTimelineFps;
}

QString playbackProxyPathForClip(const TimelineClip& clip) {
    // Validate stored proxy path - must be a valid media file
    if (!clip.proxyPath.isEmpty() && isValidMediaFile(clip.proxyPath)) {
        return clip.proxyPath;
    }
    if (clip.filePath.isEmpty() || !clipHasVisuals(clip)) {
        return QString();
    }

    const QFileInfo sourceInfo(clip.filePath);
    if (!sourceInfo.exists()) {
        return QString();
    }

    const QString baseName = sourceInfo.completeBaseName();
    const QDir sourceDir = sourceInfo.dir();

    // Check for image sequence proxy directory first (preferred format)
    const QStringList seqDirCandidates = {
        baseName + QStringLiteral(".proxy"),
        baseName + QStringLiteral("_proxy"),
        baseName + QStringLiteral("-proxy"),
    };
    for (const QString& dirName : seqDirCandidates) {
        const QString dirPath = sourceDir.filePath(dirName);
        if (isImageSequencePath(dirPath)) {
            return dirPath;
        }
    }

    const QStringList candidateNames = {
        baseName + QStringLiteral(".proxy.mov"),
        baseName + QStringLiteral(".proxy.mp4"),
        baseName + QStringLiteral(".proxy.mkv"),
        baseName + QStringLiteral(".proxy.webm"),
        baseName + QStringLiteral("_proxy.mov"),
        baseName + QStringLiteral("_proxy.mp4"),
        baseName + QStringLiteral("_proxy.mkv"),
        baseName + QStringLiteral("_proxy.webm"),
        baseName + QStringLiteral("-proxy.mov"),
        baseName + QStringLiteral("-proxy.mp4"),
        baseName + QStringLiteral("-proxy.mkv"),
        baseName + QStringLiteral("-proxy.webm"),
    };

    for (const QString& candidateName : candidateNames) {
        const QString candidatePath = sourceDir.filePath(candidateName);
        const QFileInfo candidateInfo(candidatePath);
        if (candidateInfo.exists() && isValidMediaFile(candidatePath)) {
            return candidatePath;
        }
    }

    const QDir proxiesDir(sourceDir.filePath(QStringLiteral("proxies")));
    if (proxiesDir.exists()) {
        const QStringList proxySuffixes = {
            QStringLiteral(".mov"),
            QStringLiteral(".mp4"),
            QStringLiteral(".mkv"),
            QStringLiteral(".webm")
        };
        for (const QString& suffix : proxySuffixes) {
            const QString candidatePath = proxiesDir.filePath(baseName + suffix);
            const QFileInfo candidateInfo(candidatePath);
            if (candidateInfo.exists() && isValidMediaFile(candidatePath)) {
                return candidatePath;
            }
        }
    }

    return QString();
}

QString playbackMediaPathForClip(const TimelineClip& clip) {
    const QString proxyPath = playbackProxyPathForClip(clip);
    return proxyPath.isEmpty() ? clip.filePath : proxyPath;
}

QString interactivePreviewMediaPathForClip(const TimelineClip& clip) {
    const auto interactivePathAllowed = [durationFrames = clip.durationFrames](const QString& path) {
        if (path.isEmpty()) {
            return false;
        }
        const MediaProbeResult probe = probeMediaFile(path, durationFrames);
        const QString suffix = QFileInfo(path).suffix().toLower();
        const bool disallowAlphaProresMov =
            probe.mediaType == ClipMediaType::Video &&
            probe.hasAlpha &&
            probe.codecName == QStringLiteral("prores") &&
            suffix == QStringLiteral("mov");
        return !disallowAlphaProresMov;
    };

    const QString proxyPath = playbackProxyPathForClip(clip);
    if (!proxyPath.isEmpty()) {
        return interactivePathAllowed(proxyPath) ? proxyPath : QString();
    }
    if (!clipHasVisuals(clip) || clip.filePath.isEmpty()) {
        return QString();
    }

    return interactivePathAllowed(clip.filePath) ? clip.filePath : QString();
}

bool isImageSequencePath(const QString& path) {
    return !collectSequenceFrames(path).isEmpty();
}

QStringList imageSequenceFramePaths(const QString& path) {
    return collectSequenceFrames(path);
}

QString imageSequenceDisplayLabel(const QString& path) {
    return QFileInfo(path).fileName();
}

QString transcriptPathForClipFile(const QString& filePath) {
    const QFileInfo info(filePath);
    return info.dir().filePath(info.completeBaseName() + QStringLiteral(".json"));
}

QString transcriptEditablePathForClipFile(const QString& filePath) {
    const QFileInfo info(filePath);
    return info.dir().filePath(info.completeBaseName() + QStringLiteral("_editable.json"));
}

QString transcriptWorkingPathForClipFile(const QString& filePath) {
    const QString editablePath = transcriptEditablePathForClipFile(filePath);
    if (QFileInfo::exists(editablePath)) {
        return editablePath;
    }
    return transcriptPathForClipFile(filePath);
}

bool ensureEditableTranscriptForClipFile(const QString& filePath, QString* editablePathOut) {
    const QString editablePath = transcriptEditablePathForClipFile(filePath);
    if (editablePathOut) {
        *editablePathOut = editablePath;
    }
    if (QFileInfo::exists(editablePath)) {
        return true;
    }

    const QString originalPath = transcriptPathForClipFile(filePath);
    if (!QFileInfo::exists(originalPath)) {
        return false;
    }
    QFile::remove(editablePath);
    return QFile::copy(originalPath, editablePath);
}

QVector<TranscriptSection> loadTranscriptSections(const QString& transcriptPath) {
    QFile transcriptFile(transcriptPath);
    if (!transcriptFile.open(QIODevice::ReadOnly)) {
        return {};
    }

    QJsonParseError parseError;
    const QJsonDocument transcriptDoc = QJsonDocument::fromJson(transcriptFile.readAll(), &parseError);
    if (parseError.error != QJsonParseError::NoError || !transcriptDoc.isObject()) {
        return {};
    }

    QVector<TranscriptWord> words;
    const QJsonArray segments = transcriptDoc.object().value(QStringLiteral("segments")).toArray();
    for (const QJsonValue& segmentValue : segments) {
        const QJsonArray segmentWords = segmentValue.toObject().value(QStringLiteral("words")).toArray();
        for (const QJsonValue& wordValue : segmentWords) {
            const QJsonObject wordObj = wordValue.toObject();
            const QString text = wordObj.value(QStringLiteral("word")).toString().trimmed();
            if (text.isEmpty()) {
                continue;
            }
            const bool skipped = wordObj.value(QStringLiteral("skipped")).toBool(false);
            if (skipped) {
                continue;
            }
            const double startSeconds = wordObj.value(QStringLiteral("start")).toDouble(-1.0);
            const double endSeconds = wordObj.value(QStringLiteral("end")).toDouble(-1.0);
            if (startSeconds < 0.0 || endSeconds < startSeconds) {
                continue;
            }
            const int64_t startFrame =
                qMax<int64_t>(0, static_cast<int64_t>(std::floor(startSeconds * kTimelineFps)));
            const int64_t endFrame =
                qMax<int64_t>(startFrame, static_cast<int64_t>(std::ceil(endSeconds * kTimelineFps)) - 1);
            words.push_back({startFrame, endFrame, text, false});
        }
    }
    std::sort(words.begin(), words.end(), [](const TranscriptWord& a, const TranscriptWord& b) {
        if (a.startFrame == b.startFrame) {
            return a.endFrame < b.endFrame;
        }
        return a.startFrame < b.startFrame;
    });

    QVector<TranscriptSection> sections;
    TranscriptSection current;
    const QRegularExpression punctuationPattern(QStringLiteral("[\\.!\\?;:]$"));
    for (const TranscriptWord& word : std::as_const(words)) {
        if (current.text.isEmpty()) {
            current.startFrame = word.startFrame;
            current.endFrame = word.endFrame;
            current.text = word.text;
            current.words.push_back(word);
        } else {
            current.endFrame = word.endFrame;
            current.text += QStringLiteral(" ") + word.text;
            current.words.push_back(word);
        }
        if (punctuationPattern.match(word.text).hasMatch()) {
            sections.push_back(current);
            current = TranscriptSection();
        }
    }

    if (!current.text.isEmpty()) {
        sections.push_back(current);
    }
    return sections;
}

QString wrappedTranscriptSectionText(const QString& text, int maxCharsPerLine, int maxLines) {
    const int charsPerLine = qMax(1, maxCharsPerLine);
    const int linesAllowed = qMax(1, maxLines);
    const QStringList words = text.simplified().split(QLatin1Char(' '), Qt::SkipEmptyParts);
    if (words.isEmpty()) {
        return QString();
    }

    QStringList lines;
    QString currentLine;
    int consumedWords = 0;
    for (const QString& word : words) {
        const QString candidate = currentLine.isEmpty() ? word : currentLine + QStringLiteral(" ") + word;
        if (candidate.size() <= charsPerLine || currentLine.isEmpty()) {
            currentLine = candidate;
            ++consumedWords;
            continue;
        }
        lines.push_back(currentLine);
        if (lines.size() >= linesAllowed) {
            break;
        }
        currentLine = word;
        ++consumedWords;
    }
    if (lines.size() < linesAllowed && !currentLine.isEmpty()) {
        lines.push_back(currentLine);
    }
    if (lines.isEmpty()) {
        return QString();
    }
    return lines.join(QLatin1Char('\n'));
}

TranscriptOverlayLayout layoutTranscriptSection(const TranscriptSection& section,
                                                int64_t sourceFrame,
                                                int maxCharsPerLine,
                                                int maxLines,
                                                bool autoScroll) {
    TranscriptOverlayLayout layout;
    if (section.words.isEmpty()) {
        return layout;
    }

    const int charsPerLine = qMax(1, maxCharsPerLine);
    const int linesAllowed = qMax(1, maxLines);
    int activeWordIndex = -1;
    for (int i = 0; i < section.words.size(); ++i) {
        const TranscriptWord& word = section.words.at(i);
        if (sourceFrame >= word.startFrame && sourceFrame <= word.endFrame) {
            activeWordIndex = i;
            break;
        }
        if (sourceFrame > word.endFrame) {
            activeWordIndex = i;
        } else if (activeWordIndex < 0 && sourceFrame < word.startFrame) {
            activeWordIndex = i;
            break;
        }
    }

    QVector<TranscriptOverlayLine> allLines;
    TranscriptOverlayLine currentLine;
    int currentLength = 0;
    for (int i = 0; i < section.words.size(); ++i) {
        const QString wordText = section.words.at(i).text.simplified();
        if (wordText.isEmpty()) {
            continue;
        }

        const int candidateLength = currentLine.words.isEmpty()
                                        ? wordText.size()
                                        : currentLength + 1 + wordText.size();
        if (!currentLine.words.isEmpty() && candidateLength > charsPerLine) {
            allLines.push_back(currentLine);
            currentLine = TranscriptOverlayLine();
            currentLength = 0;
        }

        currentLine.words.push_back(wordText);
        if (i == activeWordIndex) {
            currentLine.activeWord = currentLine.words.size() - 1;
        }
        currentLength = currentLine.words.join(QStringLiteral(" ")).size();
    }
    if (!currentLine.words.isEmpty()) {
        allLines.push_back(currentLine);
    }
    if (allLines.isEmpty()) {
        return layout;
    }

    int activeLineIndex = -1;
    for (int i = 0; i < allLines.size(); ++i) {
        if (allLines.at(i).activeWord >= 0) {
            activeLineIndex = i;
            break;
        }
    }

    int startLine = 0;
    if (activeLineIndex >= 0 && allLines.size() > linesAllowed) {
        if (autoScroll) {
            startLine = qBound(0, activeLineIndex - (linesAllowed - 1), allLines.size() - linesAllowed);
        } else {
            startLine = qBound(0,
                               (activeLineIndex / linesAllowed) * linesAllowed,
                               allLines.size() - linesAllowed);
        }
    }

    const int endLine = qMin(allLines.size(), startLine + linesAllowed);
    for (int i = startLine; i < endLine; ++i) {
        layout.lines.push_back(allLines.at(i));
    }
    layout.truncatedTop = startLine > 0;
    layout.truncatedBottom = endLine < allLines.size();
    return layout;
}

QString transcriptOverlayHtml(const TranscriptOverlayLayout& layout,
                              const QColor& textColor,
                              const QColor& highlightTextColor,
                              const QColor& highlightFillColor) {
    if (layout.lines.isEmpty()) {
        return QString();
    }

    QStringList htmlLines;
    htmlLines.reserve(layout.lines.size());
    for (int lineIndex = 0; lineIndex < layout.lines.size(); ++lineIndex) {
        const TranscriptOverlayLine& line = layout.lines.at(lineIndex);
        QStringList htmlWords;
        htmlWords.reserve(line.words.size());
        for (int wordIndex = 0; wordIndex < line.words.size(); ++wordIndex) {
            QString wordHtml = line.words.at(wordIndex).toHtmlEscaped();
            if (wordIndex == line.activeWord) {
                wordHtml = QStringLiteral(
                               "<span style=\"background:%1;color:%2;border-radius:0.28em;padding:0.02em 0.18em;\">%3</span>")
                               .arg(highlightFillColor.name(QColor::HexArgb),
                                    highlightTextColor.name(QColor::HexArgb),
                                    wordHtml);
            }
            htmlWords.push_back(wordHtml);
        }

        QString lineHtml = htmlWords.join(QStringLiteral(" "));
        htmlLines.push_back(lineHtml);
    }

    return QStringLiteral("<div style=\"color:%1;text-align:center;\">%2</div>")
        .arg(textColor.name(QColor::HexArgb), htmlLines.join(QStringLiteral("<br/>")));
}
