#include "editor_shared.h"
#include "debug_controls.h"

#include <QDateTime>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QHash>
#include <QImage>
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
QStringList collectSequenceFrames(const QString& path);

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

struct ProxyResolutionInput {
    bool useProxy = false;
    bool hasVisuals = false;
    QString filePath;
    QString proxyPath;
    QString sourceAbsPath;
    qint64 sourceMtimeMs = -1;
    qint64 sourceDirMtimeMs = -1;
};

ProxyResolutionInput proxyResolutionInputForClip(const TimelineClip& clip) {
    ProxyResolutionInput input;
    input.useProxy = clip.useProxy;
    input.hasVisuals = clipHasVisuals(clip);
    input.filePath = clip.filePath;
    input.proxyPath = clip.proxyPath;

    const QFileInfo sourceInfo(clip.filePath);
    input.sourceAbsPath = sourceInfo.absoluteFilePath();
    const QFileInfo sourceDirInfo(sourceInfo.absolutePath());
    if (sourceDirInfo.exists()) {
        input.sourceDirMtimeMs = sourceDirInfo.lastModified().toMSecsSinceEpoch();
    }
    if (sourceInfo.exists()) {
        input.sourceMtimeMs = sourceInfo.lastModified().toMSecsSinceEpoch();
    }
    return input;
}

QString proxyResolutionKey(const ProxyResolutionInput& input) {
    return QStringLiteral("use=%1|vis=%2|src=%3|srcm=%4|dirm=%5|stored=%6")
        .arg(input.useProxy ? 1 : 0)
        .arg(input.hasVisuals ? 1 : 0)
        .arg(input.sourceAbsPath)
        .arg(input.sourceMtimeMs)
        .arg(input.sourceDirMtimeMs)
        .arg(input.proxyPath);
}

bool cachedIsImageSequencePathImpl(const QString& path) {
    static QMutex cacheMutex;
    static QHash<QString, bool> cachedResultByKey;

    const QFileInfo info(path);
    const QFileInfo parentInfo(info.absolutePath());
    const qint64 pathMtime =
        (info.exists() && info.isDir()) ? info.lastModified().toMSecsSinceEpoch() : -1;
    const qint64 parentMtime =
        parentInfo.exists() ? parentInfo.lastModified().toMSecsSinceEpoch() : -1;
    const QString key = info.absoluteFilePath() + QLatin1Char('|') +
                        QString::number(pathMtime) + QLatin1Char('|') +
                        QString::number(parentMtime);

    {
        QMutexLocker locker(&cacheMutex);
        const auto it = cachedResultByKey.constFind(key);
        if (it != cachedResultByKey.cend()) {
            return it.value();
        }
    }

    const bool isSequence = !collectSequenceFrames(path).isEmpty();
    {
        QMutexLocker locker(&cacheMutex);
        cachedResultByKey.insert(key, isSequence);
    }
    return isSequence;
}

bool naturalFileNameLessCaseInsensitive(const QString& a, const QString& b) {
    int ia = 0;
    int ib = 0;
    const int na = a.size();
    const int nb = b.size();

    while (ia < na && ib < nb) {
        const QChar ca = a.at(ia);
        const QChar cb = b.at(ib);
        const bool aDigit = ca.isDigit();
        const bool bDigit = cb.isDigit();

        if (aDigit && bDigit) {
            int sa = ia;
            int sb = ib;
            while (sa < na && a.at(sa) == QLatin1Char('0')) ++sa;
            while (sb < nb && b.at(sb) == QLatin1Char('0')) ++sb;

            int ea = sa;
            int eb = sb;
            while (ea < na && a.at(ea).isDigit()) ++ea;
            while (eb < nb && b.at(eb).isDigit()) ++eb;

            const int lenA = ea - sa;
            const int lenB = eb - sb;
            if (lenA != lenB) {
                return lenA < lenB;
            }

            for (int i = 0; i < lenA; ++i) {
                const QChar da = a.at(sa + i);
                const QChar db = b.at(sb + i);
                if (da != db) {
                    return da < db;
                }
            }

            const int runA = ea - ia;
            const int runB = eb - ib;
            if (runA != runB) {
                return runA < runB;
            }

            ia = ea;
            ib = eb;
            continue;
        }

        const QChar fa = ca.toCaseFolded();
        const QChar fb = cb.toCaseFolded();
        if (fa != fb) {
            return fa < fb;
        }

        ++ia;
        ++ib;
    }

    return na < nb;
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
    QFileInfoList entries = dir.entryInfoList(QDir::Files | QDir::NoDotAndDotDot, QDir::NoSort);
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

    std::sort(bestGroup.begin(), bestGroup.end(), [](const QFileInfo& a, const QFileInfo& b) {
        return naturalFileNameLessCaseInsensitive(a.fileName(), b.fileName());
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

MediaProbeResult probeMediaFile(const QString& filePath, qreal fallbackSeconds) {
    QElapsedTimer probeTimer;
    probeTimer.start();
    MediaProbeResult result;
    // result.durationFrames will be set later based on fallbackSeconds and detected fps

    if (filePath.trimmed().isEmpty()) {
        // For empty path, set default
        result.durationFrames = qRound64(fallbackSeconds * 30.0);
        return result;
    }

    const QFileInfo info(filePath);
    if (!info.exists()) {
        result.durationFrames = qRound64(fallbackSeconds * 30.0);
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
        } else {
            result.durationFrames = qRound64(fallbackSeconds * 30.0);
        }
        return result;
    }
    const QString suffix = info.suffix().toLower();
    if (isImageSuffix(suffix)) {
        result.mediaType = ClipMediaType::Image;
        result.durationFrames = qRound64(fallbackSeconds * 30.0); // For images, assume 30 fps
        return result;
    }

    AVFormatContext* formatCtx = nullptr;
    const QByteArray pathBytes = QFile::encodeName(filePath);
    if (avformat_open_input(&formatCtx, pathBytes.constData(), nullptr, nullptr) < 0) {
        result.durationFrames = qRound64(fallbackSeconds * 30.0);
        return result;
    }

    double sourceFps = 30.0;
    bool durationFound = false;

    if (avformat_find_stream_info(formatCtx, nullptr) >= 0) {
        double durationSeconds = 0.0;
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
            durationFound = true;
        }
    }

    avformat_close_input(&formatCtx);

    if (!durationFound) {
        result.durationFrames = qRound64(fallbackSeconds * sourceFps);
        result.fps = sourceFps;
    }

    if (result.hasVideo) {
        result.mediaType = ClipMediaType::Video;
    } else if (result.hasAudio) {
        result.mediaType = ClipMediaType::Audio;
    }

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


qreal effectiveFpsForClip(const TimelineClip& clip) {
    // If clip has explicit source FPS, use that (considering proxy if available)
    if (clip.sourceFps > 0) {
        return clip.sourceFps;
    }
    // Otherwise probe the media file
    const QString mediaPath = playbackMediaPathForClip(clip);
    if (!mediaPath.isEmpty()) {
        const MediaProbeResult probe = probeMediaFile(mediaPath, clip.durationFrames / kTimelineFps);
        return probe.fps;
    }
    return kTimelineFps;
}

QString playbackProxyPathForClip(const TimelineClip& clip) {
    static QMutex cacheMutex;
    static QHash<QString, QString> cachedProxyPathByKey;

    const ProxyResolutionInput input = proxyResolutionInputForClip(clip);
    const QString cacheKey = proxyResolutionKey(input);
    {
        QMutexLocker locker(&cacheMutex);
        const auto it = cachedProxyPathByKey.constFind(cacheKey);
        if (it != cachedProxyPathByKey.cend()) {
            return it.value();
        }
    }

    QString resolvedProxyPath;
    if (input.useProxy &&
        input.hasVisuals &&
        !input.filePath.isEmpty()) {
        // Validate stored proxy path - must be a valid media file
        if (!input.proxyPath.isEmpty() && isValidMediaFile(input.proxyPath)) {
            resolvedProxyPath = input.proxyPath;
        } else {
            const QFileInfo sourceInfo(input.filePath);
            if (sourceInfo.exists()) {
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
                    if (cachedIsImageSequencePathImpl(dirPath)) {
                        resolvedProxyPath = dirPath;
                        break;
                    }
                }

                if (resolvedProxyPath.isEmpty()) {
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
                            resolvedProxyPath = candidatePath;
                            break;
                        }
                    }
                }

                if (resolvedProxyPath.isEmpty()) {
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
                                resolvedProxyPath = candidatePath;
                                break;
                            }
                        }
                    }
                }
            }
        }
    }

    {
        QMutexLocker locker(&cacheMutex);
        cachedProxyPathByKey.insert(cacheKey, resolvedProxyPath);
    }
    return resolvedProxyPath;
}

QString playbackMediaPathForClip(const TimelineClip& clip) {
    const QString proxyPath = playbackProxyPathForClip(clip);
    return proxyPath.isEmpty() ? clip.filePath : proxyPath;
}

void refreshClipAudioSource(TimelineClip& clip) {
    const qint64 nowMs = QDateTime::currentMSecsSinceEpoch();
    clip.audioSourceLastVerifiedMs = nowMs;
    clip.audioSourceOriginalPath = QFileInfo(clip.filePath).absoluteFilePath();

    if (!clip.hasAudio || clip.filePath.isEmpty()) {
        clip.audioSourceMode = QStringLiteral("embedded");
        clip.audioSourcePath = clip.filePath;
        clip.audioSourceStatus = QStringLiteral("disabled");
        return;
    }

    // Explicit external audio file chosen by user takes precedence when valid.
    if (clip.audioSourceMode == QStringLiteral("explicit_file") &&
        !clip.audioSourcePath.trimmed().isEmpty()) {
        const QFileInfo explicitInfo(clip.audioSourcePath);
        if (explicitInfo.exists() && explicitInfo.isFile()) {
            clip.audioSourcePath = explicitInfo.absoluteFilePath();
            clip.audioSourceStatus = QStringLiteral("ok");
            return;
        }
    }

    const QFileInfo sourceInfo(clip.filePath);
    if (!sourceInfo.exists() || !sourceInfo.isFile()) {
        clip.audioSourceMode = QStringLiteral("embedded");
        clip.audioSourcePath = sourceInfo.absoluteFilePath();
        clip.audioSourceStatus = QStringLiteral("missing");
        return;
    }

    const QString sourceAbsolutePath = sourceInfo.absoluteFilePath();
    const bool sourceIsWav =
        sourceInfo.suffix().compare(QStringLiteral("wav"), Qt::CaseInsensitive) == 0;
    if (sourceIsWav) {
        clip.audioSourceMode = QStringLiteral("explicit_file");
        clip.audioSourcePath = sourceAbsolutePath;
        clip.audioSourceStatus = QStringLiteral("ok");
        return;
    }

    const QString sidecarPath =
        sourceInfo.dir().filePath(sourceInfo.completeBaseName() + QStringLiteral(".wav"));
    const QFileInfo sidecarInfo(sidecarPath);
    if (sidecarInfo.exists() && sidecarInfo.isFile()) {
        clip.audioSourceMode = QStringLiteral("sidecar");
        clip.audioSourcePath = sidecarInfo.absoluteFilePath();
        clip.audioSourceStatus = QStringLiteral("ok");
        return;
    }

    clip.audioSourceMode = QStringLiteral("embedded");
    clip.audioSourcePath = sourceAbsolutePath;
    clip.audioSourceStatus = QStringLiteral("ok");
}

QString playbackAudioPathForClip(const TimelineClip& clip) {
    if (!clipAudioPlaybackEnabled(clip) || clip.filePath.isEmpty()) {
        return clip.filePath;
    }

    if (!clip.audioSourcePath.trimmed().isEmpty() &&
        clip.audioSourceStatus == QStringLiteral("ok")) {
        const QFileInfo trackedInfo(clip.audioSourcePath);
        if (trackedInfo.exists() && trackedInfo.isFile()) {
            return trackedInfo.absoluteFilePath();
        }
    }

    const QFileInfo sourceInfo(clip.filePath);
    if (!sourceInfo.exists() || !sourceInfo.isFile()) {
        return sourceInfo.absoluteFilePath();
    }
    if (sourceInfo.suffix().compare(QStringLiteral("wav"), Qt::CaseInsensitive) == 0) {
        return sourceInfo.absoluteFilePath();
    }

    const QString alternatePath =
        sourceInfo.dir().filePath(sourceInfo.completeBaseName() + QStringLiteral(".wav"));
    const QFileInfo alternateInfo(alternatePath);
    if (alternateInfo.exists() && alternateInfo.isFile()) {
        return alternateInfo.absoluteFilePath();
    }
    return clip.filePath;
}

bool playbackUsesAlternateAudioSource(const TimelineClip& clip) {
    if (!clipAudioPlaybackEnabled(clip) || clip.filePath.isEmpty()) {
        return false;
    }
    const QString resolvedAudioPath = playbackAudioPathForClip(clip);
    if (resolvedAudioPath.isEmpty()) {
        return false;
    }
    return QFileInfo(resolvedAudioPath).absoluteFilePath() !=
           QFileInfo(clip.filePath).absoluteFilePath();
}

QString interactivePreviewMediaPathForClip(const TimelineClip& clip) {
    const auto interactivePathAllowed = [durationFrames = clip.durationFrames](const QString& path) {
        if (path.isEmpty()) {
            return false;
        }
        const MediaProbeResult probe = probeMediaFile(path, durationFrames / kTimelineFps);
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
    return cachedIsImageSequencePathImpl(path);
}

QStringList imageSequenceFramePaths(const QString& path) {
    return collectSequenceFrames(path);
}

QString imageSequenceDisplayLabel(const QString& path) {
    return QFileInfo(path).fileName();
}

