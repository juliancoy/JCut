#include "decoder_benchmark_utils.h"

#include <QElapsedTimer>

namespace editor {

DecodeBenchmarkResult benchmarkDecodeFrames(const QString& mediaPath, int maxFrames) {
    DecodeBenchmarkResult result;
    DecoderContext ctx(mediaPath);
    if (!ctx.initialize()) {
        result.error = QStringLiteral("failed to initialize decoder context");
        return result;
    }

    result.success = true;
    result.info = ctx.info();
    result.hardwareAccelerated = ctx.isHardwareAccelerated();

    const int64_t durationFrames = qMax<int64_t>(1, result.info.durationFrames);
    result.framesBenchmarked = static_cast<int>(qMin<int64_t>(qMax(1, maxFrames), durationFrames));

    QElapsedTimer timer;
    timer.start();
    for (int i = 0; i < result.framesBenchmarked; ++i) {
        FrameHandle frame = ctx.decodeFrame(i);
        if (frame.isNull()) {
            ++result.nullFrames;
        } else {
            ++result.decodedFrames;
        }
    }

    result.elapsedMs = qMax<qint64>(1, timer.elapsed());
    result.fps = (1000.0 * result.decodedFrames) / static_cast<double>(result.elapsedMs);
    return result;
}

SeekBenchmarkResult benchmarkDecodeSeeks(const QString& mediaPath,
                                         const QVector<int64_t>& targetFrames) {
    SeekBenchmarkResult result;
    DecoderContext ctx(mediaPath);
    if (!ctx.initialize()) {
        result.error = QStringLiteral("failed to initialize decoder context");
        return result;
    }

    result.success = true;
    result.info = ctx.info();
    result.hardwareAccelerated = ctx.isHardwareAccelerated();
    QVector<int64_t> safeTargets = targetFrames;
    if (safeTargets.isEmpty()) {
        const int64_t durationFrames = qMax<int64_t>(1, result.info.durationFrames);
        safeTargets = {
            0,
            qMin<int64_t>(durationFrames - 1, durationFrames / 4),
            qMin<int64_t>(durationFrames - 1, durationFrames / 2),
            qMin<int64_t>(durationFrames - 1, (durationFrames * 3) / 4),
            qMax<int64_t>(0, durationFrames - 1)
        };
    }
    result.samples.reserve(safeTargets.size());

    qint64 totalSeekMs = 0;
    for (int64_t targetFrame : safeTargets) {
        QElapsedTimer timer;
        timer.start();
        FrameHandle frame = ctx.decodeFrame(targetFrame);
        const qint64 elapsedMs = qMax<qint64>(1, timer.elapsed());
        totalSeekMs += elapsedMs;
        result.maxSeekMs = qMax(result.maxSeekMs, elapsedMs);

        SeekBenchmarkSample sample;
        sample.targetFrame = targetFrame;
        sample.elapsedMs = elapsedMs;
        sample.success = !frame.isNull();
        sample.decodedFrame = frame.isNull() ? static_cast<int64_t>(-1)
                                             : static_cast<int64_t>(frame.frameNumber());
        result.samples.push_back(sample);
        if (sample.success) {
            ++result.successfulSeeks;
        } else {
            ++result.nullSeeks;
        }
    }

    if (!result.samples.isEmpty()) {
        result.avgSeekMs = static_cast<double>(totalSeekMs) /
                           static_cast<double>(result.samples.size());
    }
    return result;
}

} // namespace editor
