#include "control_server_media_diag.h"

#include "clip_serialization.h"
#include "decoder_context.h"
#include "editor_shared.h"

#include <QDir>
#include <QElapsedTimer>
#include <QFileInfo>
#include <QHash>
#include <QJsonArray>
#include <QVector>

namespace control_server {

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
    struct BenchmarkTarget {
        TimelineClip clip;
        QString decodePath;
        QString sourcePath;
    };

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

    QJsonArray results;
    int successCount = 0;
    int skippedCount = 0;
    int errorCount = 0;
    double fpsSum = 0.0;

    for (auto it = targetsByDecodePath.cbegin(); it != targetsByDecodePath.cend(); ++it) {
        const BenchmarkTarget& target = it.value();
        QJsonObject result{
            {QStringLiteral("clip_id"), target.clip.id},
            {QStringLiteral("label"), target.clip.label},
            {QStringLiteral("source_path"), QDir::toNativeSeparators(target.sourcePath)},
            {QStringLiteral("decode_path"), QDir::toNativeSeparators(target.decodePath)},
            {QStringLiteral("media_type"), clipMediaTypeLabel(target.clip.mediaType)},
            {QStringLiteral("source_kind"), mediaSourceKindLabel(target.clip.sourceKind)}
        };

        if (!clipHasVisuals(target.clip)) {
            result[QStringLiteral("success")] = false;
            result[QStringLiteral("skipped")] = true;
            result[QStringLiteral("reason")] = QStringLiteral("clip has no visual decode path");
            results.append(result);
            ++skippedCount;
            continue;
        }

        if (target.decodePath.isEmpty()) {
            result[QStringLiteral("success")] = false;
            result[QStringLiteral("skipped")] = true;
            result[QStringLiteral("reason")] = QStringLiteral("no interactive preview media path");
            results.append(result);
            ++skippedCount;
            continue;
        }

        editor::DecoderContext ctx(target.decodePath);
        if (!ctx.initialize()) {
            result[QStringLiteral("success")] = false;
            result[QStringLiteral("error")] = QStringLiteral("failed to initialize decoder context");
            results.append(result);
            ++errorCount;
            continue;
        }

        const editor::VideoStreamInfo info = ctx.info();
        const int64_t durationFrames = qMax<int64_t>(1, info.durationFrames);
        const int framesToBenchmark = static_cast<int>(qMin<int64_t>(90, durationFrames));
        int decodedFrames = 0;
        int nullFrames = 0;

        QElapsedTimer timer;
        timer.start();
        for (int i = 0; i < framesToBenchmark; ++i) {
            editor::FrameHandle frame = ctx.decodeFrame(i);
            if (frame.isNull()) {
                ++nullFrames;
            } else {
                ++decodedFrames;
            }
        }
        const qint64 elapsedMs = qMax<qint64>(1, timer.elapsed());
        const double fps = (1000.0 * decodedFrames) / static_cast<double>(elapsedMs);

        result[QStringLiteral("success")] = true;
        result[QStringLiteral("codec")] = info.codecName;
        result[QStringLiteral("frames_benchmarked")] = framesToBenchmark;
        result[QStringLiteral("frames_decoded")] = decodedFrames;
        result[QStringLiteral("null_frames")] = nullFrames;
        result[QStringLiteral("elapsed_ms")] = elapsedMs;
        result[QStringLiteral("fps")] = fps;
        result[QStringLiteral("hardware_accelerated")] = ctx.isHardwareAccelerated();
        result[QStringLiteral("frame_width")] = info.frameSize.width();
        result[QStringLiteral("frame_height")] = info.frameSize.height();
        results.append(result);
        ++successCount;
        fpsSum += fps;
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
    struct BenchmarkTarget {
        TimelineClip clip;
        QString decodePath;
        QString sourcePath;
    };

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

    QJsonArray results;
    int successCount = 0;
    int skippedCount = 0;
    int errorCount = 0;
    double avgSeekMsSum = 0.0;

    for (auto it = targetsByDecodePath.cbegin(); it != targetsByDecodePath.cend(); ++it) {
        const BenchmarkTarget& target = it.value();
        QJsonObject result{
            {QStringLiteral("clip_id"), target.clip.id},
            {QStringLiteral("label"), target.clip.label},
            {QStringLiteral("source_path"), QDir::toNativeSeparators(target.sourcePath)},
            {QStringLiteral("decode_path"), QDir::toNativeSeparators(target.decodePath)},
            {QStringLiteral("media_type"), clipMediaTypeLabel(target.clip.mediaType)},
            {QStringLiteral("source_kind"), mediaSourceKindLabel(target.clip.sourceKind)}
        };

        if (!clipHasVisuals(target.clip)) {
            result[QStringLiteral("success")] = false;
            result[QStringLiteral("skipped")] = true;
            result[QStringLiteral("reason")] = QStringLiteral("clip has no visual decode path");
            results.append(result);
            ++skippedCount;
            continue;
        }

        if (target.decodePath.isEmpty()) {
            result[QStringLiteral("success")] = false;
            result[QStringLiteral("skipped")] = true;
            result[QStringLiteral("reason")] = QStringLiteral("no interactive preview media path");
            results.append(result);
            ++skippedCount;
            continue;
        }

        editor::DecoderContext ctx(target.decodePath);
        if (!ctx.initialize()) {
            result[QStringLiteral("success")] = false;
            result[QStringLiteral("error")] = QStringLiteral("failed to initialize decoder context");
            results.append(result);
            ++errorCount;
            continue;
        }

        const editor::VideoStreamInfo info = ctx.info();
        const int64_t durationFrames = qMax<int64_t>(1, info.durationFrames);
        const QVector<int64_t> seekTargets = {
            0,
            qMin<int64_t>(durationFrames - 1, durationFrames / 4),
            qMin<int64_t>(durationFrames - 1, durationFrames / 2),
            qMin<int64_t>(durationFrames - 1, (durationFrames * 3) / 4),
            qMax<int64_t>(0, durationFrames - 1)
        };

        QJsonArray samples;
        int successfulSeeks = 0;
        int nullSeeks = 0;
        qint64 totalSeekMs = 0;
        qint64 maxSeekMs = 0;

        for (int64_t targetFrame : seekTargets) {
            QElapsedTimer timer;
            timer.start();
            editor::FrameHandle frame = ctx.decodeFrame(targetFrame);
            const qint64 elapsedMs = qMax<qint64>(1, timer.elapsed());
            totalSeekMs += elapsedMs;
            maxSeekMs = qMax(maxSeekMs, elapsedMs);
            if (frame.isNull()) {
                ++nullSeeks;
            } else {
                ++successfulSeeks;
            }
            samples.append(QJsonObject{
                {QStringLiteral("target_frame"), static_cast<qint64>(targetFrame)},
                {QStringLiteral("elapsed_ms"), elapsedMs},
                {QStringLiteral("success"), !frame.isNull()},
                {QStringLiteral("decoded_frame"), frame.isNull() ? static_cast<qint64>(-1)
                                                                 : static_cast<qint64>(frame.frameNumber())}
            });
        }

        const double avgSeekMs =
            seekTargets.isEmpty() ? 0.0 : static_cast<double>(totalSeekMs) / static_cast<double>(seekTargets.size());

        result[QStringLiteral("success")] = true;
        result[QStringLiteral("codec")] = info.codecName;
        result[QStringLiteral("hardware_accelerated")] = ctx.isHardwareAccelerated();
        result[QStringLiteral("seek_count")] = seekTargets.size();
        result[QStringLiteral("successful_seeks")] = successfulSeeks;
        result[QStringLiteral("null_seeks")] = nullSeeks;
        result[QStringLiteral("avg_seek_ms")] = avgSeekMs;
        result[QStringLiteral("max_seek_ms")] = maxSeekMs;
        result[QStringLiteral("frame_width")] = info.frameSize.width();
        result[QStringLiteral("frame_height")] = info.frameSize.height();
        result[QStringLiteral("samples")] = samples;
        results.append(result);
        ++successCount;
        avgSeekMsSum += avgSeekMs;
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
