#include "control_server_media_diag.h"

#include "clip_serialization.h"
#include "decoder_benchmark_utils.h"
#include "editor_shared.h"

#include <QDir>
#include <QFileInfo>
#include <QHash>
#include <QJsonArray>
#include <QVector>

namespace control_server {

namespace {

struct BenchmarkTarget {
    TimelineClip clip;
    QString decodePath;
    QString sourcePath;
};

QHash<QString, BenchmarkTarget> collectBenchmarkTargets(const QJsonObject& state) {
    QHash<QString, BenchmarkTarget> targetsByDecodePath;
    const QJsonArray timeline = state.value(QStringLiteral("timeline")).toArray();
    for (const QJsonValue& value : timeline) {
        if (!value.isObject()) {
            continue;
        }
        const TimelineClip clip = editor::clipFromJson(value.toObject());
        if (clip.filePath.isEmpty()) {
            continue;
        }
        const QString decodePath = interactivePreviewMediaPathForClip(clip);
        const QString dedupeKey = decodePath.isEmpty() ? clip.filePath : decodePath;
        if (dedupeKey.isEmpty() || targetsByDecodePath.contains(dedupeKey)) {
            continue;
        }
        targetsByDecodePath.insert(dedupeKey, BenchmarkTarget{clip, decodePath, clip.filePath});
    }
    return targetsByDecodePath;
}

QJsonObject makeBenchmarkResultBase(const BenchmarkTarget& target) {
    return QJsonObject{
        {QStringLiteral("clip_id"), target.clip.id},
        {QStringLiteral("label"), target.clip.label},
        {QStringLiteral("source_path"), QDir::toNativeSeparators(target.sourcePath)},
        {QStringLiteral("decode_path"), QDir::toNativeSeparators(target.decodePath)},
        {QStringLiteral("media_type"), clipMediaTypeLabel(target.clip.mediaType)},
        {QStringLiteral("source_kind"), mediaSourceKindLabel(target.clip.sourceKind)}
    };
}

QString benchmarkSkipReason(const BenchmarkTarget& target) {
    if (!clipHasVisuals(target.clip)) {
        return QStringLiteral("clip has no visual decode path");
    }
    if (target.decodePath.isEmpty()) {
        return QStringLiteral("no interactive preview media path");
    }
    return QString();
}

void populateVideoStreamFields(QJsonObject* result, const editor::VideoStreamInfo& info) {
    if (!result) {
        return;
    }
    (*result)[QStringLiteral("codec")] = info.codecName;
    (*result)[QStringLiteral("frame_width")] = info.frameSize.width();
    (*result)[QStringLiteral("frame_height")] = info.frameSize.height();
}

} // namespace

QJsonObject enrichClipForApi(const QJsonObject& clipObject) {
    QJsonObject enriched = clipObject;
    const TimelineClip clip = editor::clipFromJson(clipObject);
    const QString playbackPath = playbackMediaPathForClip(clip);
    const QString audioPath = playbackAudioPathForClip(clip);
    const QString detectedProxyPath = playbackProxyPathForClip(clip);
    const QString transcriptPath = transcriptWorkingPathForClipFile(clip.filePath);
    const qint64 startFrame = clip.startFrame;
    const qint64 durationFrames = clip.durationFrames;

    enriched[QStringLiteral("endFrame")] = startFrame + durationFrames;
    enriched[QStringLiteral("playbackMediaPath")] = QDir::toNativeSeparators(playbackPath);
    enriched[QStringLiteral("playbackAudioPath")] = QDir::toNativeSeparators(audioPath);
    enriched[QStringLiteral("detectedProxyPath")] = QDir::toNativeSeparators(detectedProxyPath);
    enriched[QStringLiteral("proxyVideoAvailable")] = !detectedProxyPath.isEmpty();
    enriched[QStringLiteral("proxyVideoActive")] = !playbackPath.isEmpty() &&
        QFileInfo(playbackPath).absoluteFilePath() != QFileInfo(clip.filePath).absoluteFilePath();
    enriched[QStringLiteral("proxyAudioActive")] = playbackUsesAlternateAudioSource(clip);
    enriched[QStringLiteral("transcriptPath")] = QDir::toNativeSeparators(transcriptPath);
    enriched[QStringLiteral("transcriptAvailable")] =
        !transcriptPath.isEmpty() && QFileInfo::exists(transcriptPath);
    enriched[QStringLiteral("audioSourceModeResolved")] = clip.audioSourceMode;
    enriched[QStringLiteral("audioSourceStatusResolved")] = clip.audioSourceStatus;
    return enriched;
}

QJsonObject benchmarkDecodeRatesForState(const QJsonObject& state) {
    const QHash<QString, BenchmarkTarget> targetsByDecodePath = collectBenchmarkTargets(state);

    QJsonArray results;
    int successCount = 0;
    int skippedCount = 0;
    int errorCount = 0;
    double fpsSum = 0.0;

    for (auto it = targetsByDecodePath.cbegin(); it != targetsByDecodePath.cend(); ++it) {
        const BenchmarkTarget& target = it.value();
        QJsonObject result = makeBenchmarkResultBase(target);
        const QString skipReason = benchmarkSkipReason(target);
        if (!skipReason.isEmpty()) {
            result[QStringLiteral("success")] = false;
            result[QStringLiteral("skipped")] = true;
            result[QStringLiteral("reason")] = skipReason;
            results.append(result);
            ++skippedCount;
            continue;
        }

        const editor::DecodeBenchmarkResult bench =
            editor::benchmarkDecodeFrames(target.decodePath);
        if (!bench.success) {
            result[QStringLiteral("success")] = false;
            result[QStringLiteral("error")] = bench.error;
            results.append(result);
            ++errorCount;
            continue;
        }

        result[QStringLiteral("success")] = true;
        populateVideoStreamFields(&result, bench.info);
        result[QStringLiteral("frames_benchmarked")] = bench.framesBenchmarked;
        result[QStringLiteral("frames_decoded")] = bench.decodedFrames;
        result[QStringLiteral("null_frames")] = bench.nullFrames;
        result[QStringLiteral("elapsed_ms")] = bench.elapsedMs;
        result[QStringLiteral("fps")] = bench.fps;
        result[QStringLiteral("hardware_accelerated")] = bench.hardwareAccelerated;
        results.append(result);
        ++successCount;
        fpsSum += bench.fps;
    }

    return QJsonObject{
        {QStringLiteral("ok"), true},
        {QStringLiteral("file_count"), results.size()},
        {QStringLiteral("success_count"), successCount},
        {QStringLiteral("skipped_count"), skippedCount},
        {QStringLiteral("error_count"), errorCount},
        {QStringLiteral("avg_fps"), successCount > 0 ? fpsSum / static_cast<double>(successCount) : 0.0},
        {QStringLiteral("results"), results}
    };
}

QJsonObject benchmarkSeekRatesForState(const QJsonObject& state) {
    const QHash<QString, BenchmarkTarget> targetsByDecodePath = collectBenchmarkTargets(state);

    QJsonArray results;
    int successCount = 0;
    int skippedCount = 0;
    int errorCount = 0;
    double avgSeekMsSum = 0.0;

    for (auto it = targetsByDecodePath.cbegin(); it != targetsByDecodePath.cend(); ++it) {
        const BenchmarkTarget& target = it.value();
        QJsonObject result = makeBenchmarkResultBase(target);
        const QString skipReason = benchmarkSkipReason(target);
        if (!skipReason.isEmpty()) {
            result[QStringLiteral("success")] = false;
            result[QStringLiteral("skipped")] = true;
            result[QStringLiteral("reason")] = skipReason;
            results.append(result);
            ++skippedCount;
            continue;
        }

        const editor::SeekBenchmarkResult bench =
            editor::benchmarkDecodeSeeks(target.decodePath);
        if (!bench.success) {
            result[QStringLiteral("success")] = false;
            result[QStringLiteral("error")] = bench.error;
            results.append(result);
            ++errorCount;
            continue;
        }

        QJsonArray samples;
        for (const editor::SeekBenchmarkSample& sample : bench.samples) {
            samples.append(QJsonObject{
                {QStringLiteral("target_frame"), static_cast<qint64>(sample.targetFrame)},
                {QStringLiteral("elapsed_ms"), sample.elapsedMs},
                {QStringLiteral("success"), sample.success},
                {QStringLiteral("decoded_frame"), static_cast<qint64>(sample.decodedFrame)}
            });
        }

        result[QStringLiteral("success")] = true;
        populateVideoStreamFields(&result, bench.info);
        result[QStringLiteral("hardware_accelerated")] = bench.hardwareAccelerated;
        result[QStringLiteral("seek_count")] = bench.samples.size();
        result[QStringLiteral("successful_seeks")] = bench.successfulSeeks;
        result[QStringLiteral("null_seeks")] = bench.nullSeeks;
        result[QStringLiteral("avg_seek_ms")] = bench.avgSeekMs;
        result[QStringLiteral("max_seek_ms")] = bench.maxSeekMs;
        result[QStringLiteral("samples")] = samples;
        results.append(result);
        ++successCount;
        avgSeekMsSum += bench.avgSeekMs;
    }

    return QJsonObject{
        {QStringLiteral("ok"), true},
        {QStringLiteral("file_count"), results.size()},
        {QStringLiteral("success_count"), successCount},
        {QStringLiteral("skipped_count"), skippedCount},
        {QStringLiteral("error_count"), errorCount},
        {QStringLiteral("avg_seek_ms"), successCount > 0 ? avgSeekMsSum / static_cast<double>(successCount) : 0.0},
        {QStringLiteral("results"), results}
    };
}

} // namespace control_server
