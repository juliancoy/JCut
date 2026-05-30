#include "control_server_media_diag.h"

#include "clip_serialization.h"
#include "decoder_benchmark_utils.h"
#include "editor_shared.h"
#include "transcript_engine.h"

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

QJsonObject transcriptVariantSidecars(const QString& variantName, const QString& transcriptPath) {
    const QFileInfo transcriptInfo(transcriptPath);
    editor::TranscriptEngine engine;
    const QString rawPath = engine.facedetectionsArtifactPath(transcriptPath);
    const QString processedPath = engine.facedetectionsProcessedArtifactPath(transcriptPath);
    const QString identityPath = engine.identityArtifactPath(transcriptPath);
    const bool transcriptExists =
        !transcriptPath.trimmed().isEmpty() && transcriptInfo.exists() && transcriptInfo.isFile();
    const bool rawExists = !rawPath.trimmed().isEmpty() && QFileInfo::exists(rawPath);
    const bool processedExists = !processedPath.trimmed().isEmpty() && QFileInfo::exists(processedPath);
    const bool identityExists = !identityPath.trimmed().isEmpty() && QFileInfo::exists(identityPath);

    return QJsonObject{
        {QStringLiteral("name"), variantName},
        {QStringLiteral("transcript_path"), QDir::toNativeSeparators(transcriptInfo.absoluteFilePath())},
        {QStringLiteral("transcript_exists"), transcriptExists},
        {QStringLiteral("facedetections_raw_path"), QDir::toNativeSeparators(rawPath)},
        {QStringLiteral("facedetections_raw_exists"), rawExists},
        {QStringLiteral("facedetections_processed_path"), QDir::toNativeSeparators(processedPath)},
        {QStringLiteral("facedetections_processed_exists"), processedExists},
        {QStringLiteral("identity_path"), QDir::toNativeSeparators(identityPath)},
        {QStringLiteral("identity_exists"), identityExists},
        {QStringLiteral("any_runtime_sidecar_exists"), rawExists || processedExists || identityExists}
    };
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
    const QString originalTranscriptPath = transcriptPathForClipFile(clip.filePath);
    const QFileInfo clipInfo(clip.filePath);
    const QString editableTranscriptPath =
        clipInfo.dir().filePath(clipInfo.completeBaseName() + QStringLiteral("_editable.json"));
    const QString workingTranscriptPath = transcriptWorkingPathForClipFile(clip.filePath);
    const QString activeTranscriptPath = activeTranscriptPathForClipFile(clip.filePath);
    const QString runtimeTranscriptPath =
        transcriptPathForRuntimeSidecarForClipFile(clip.filePath, activeTranscriptPath);
    const QString transcriptPath = transcriptWorkingPathForClipFile(clip.filePath);
    const qint64 startFrame = clip.startFrame;
    const qint64 durationFrames = clip.durationFrames;
    const QJsonObject originalVariant =
        transcriptVariantSidecars(QStringLiteral("original"), originalTranscriptPath);
    const QJsonObject editableVariant =
        transcriptVariantSidecars(QStringLiteral("editable"), editableTranscriptPath);
    const QJsonObject workingVariant =
        transcriptVariantSidecars(QStringLiteral("working"), workingTranscriptPath);
    const QJsonObject activeVariant =
        transcriptVariantSidecars(QStringLiteral("active"), activeTranscriptPath);
    const QJsonObject runtimeVariant =
        transcriptVariantSidecars(QStringLiteral("runtime"), runtimeTranscriptPath);
    QJsonArray transcriptVariants;
    transcriptVariants.push_back(originalVariant);
    if (QFileInfo(editableTranscriptPath).absoluteFilePath() != QFileInfo(originalTranscriptPath).absoluteFilePath()) {
        transcriptVariants.push_back(editableVariant);
    }
    if (QFileInfo(workingTranscriptPath).absoluteFilePath() != QFileInfo(originalTranscriptPath).absoluteFilePath() &&
        QFileInfo(workingTranscriptPath).absoluteFilePath() != QFileInfo(editableTranscriptPath).absoluteFilePath()) {
        transcriptVariants.push_back(workingVariant);
    }
    if (QFileInfo(activeTranscriptPath).absoluteFilePath() != QFileInfo(originalTranscriptPath).absoluteFilePath() &&
        QFileInfo(activeTranscriptPath).absoluteFilePath() != QFileInfo(editableTranscriptPath).absoluteFilePath() &&
        QFileInfo(activeTranscriptPath).absoluteFilePath() != QFileInfo(workingTranscriptPath).absoluteFilePath()) {
        transcriptVariants.push_back(activeVariant);
    }
    if (QFileInfo(runtimeTranscriptPath).absoluteFilePath() != QFileInfo(originalTranscriptPath).absoluteFilePath() &&
        QFileInfo(runtimeTranscriptPath).absoluteFilePath() != QFileInfo(editableTranscriptPath).absoluteFilePath() &&
        QFileInfo(runtimeTranscriptPath).absoluteFilePath() != QFileInfo(workingTranscriptPath).absoluteFilePath() &&
        QFileInfo(runtimeTranscriptPath).absoluteFilePath() != QFileInfo(activeTranscriptPath).absoluteFilePath()) {
        transcriptVariants.push_back(runtimeVariant);
    }
    const bool anyRuntimeSidecarExists =
        runtimeVariant.value(QStringLiteral("any_runtime_sidecar_exists")).toBool(false);
    const bool anyTranscriptExists =
        originalVariant.value(QStringLiteral("transcript_exists")).toBool(false) ||
        editableVariant.value(QStringLiteral("transcript_exists")).toBool(false) ||
        workingVariant.value(QStringLiteral("transcript_exists")).toBool(false) ||
        activeVariant.value(QStringLiteral("transcript_exists")).toBool(false);
    const QJsonObject sidecars{
        {QStringLiteral("transcript_variants"), transcriptVariants},
        {QStringLiteral("original_transcript_path"), QDir::toNativeSeparators(QFileInfo(originalTranscriptPath).absoluteFilePath())},
        {QStringLiteral("editable_transcript_path"), QDir::toNativeSeparators(QFileInfo(editableTranscriptPath).absoluteFilePath())},
        {QStringLiteral("working_transcript_path"), QDir::toNativeSeparators(QFileInfo(workingTranscriptPath).absoluteFilePath())},
        {QStringLiteral("active_transcript_path"), QDir::toNativeSeparators(QFileInfo(activeTranscriptPath).absoluteFilePath())},
        {QStringLiteral("runtime_transcript_path"), QDir::toNativeSeparators(QFileInfo(runtimeTranscriptPath).absoluteFilePath())},
        {QStringLiteral("any_transcript_exists"), anyTranscriptExists},
        {QStringLiteral("runtime"), runtimeVariant}
    };

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
    enriched[QStringLiteral("runtimeTranscriptPath")] =
        QDir::toNativeSeparators(QFileInfo(runtimeTranscriptPath).absoluteFilePath());
    enriched[QStringLiteral("faceDetectionsSidecarAvailable")] = anyRuntimeSidecarExists;
    enriched[QStringLiteral("audioSourceModeResolved")] = clip.audioSourceMode;
    enriched[QStringLiteral("audioSourceStatusResolved")] = clip.audioSourceStatus;
    enriched[QStringLiteral("sidecars")] = sidecars;
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
