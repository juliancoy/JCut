#pragma once

#include "decoder_context.h"

#include <QString>
#include <QVector>

#include <cstdint>

namespace editor {

struct DecodeBenchmarkResult {
    bool success = false;
    QString error;
    VideoStreamInfo info;
    bool hardwareAccelerated = false;
    int framesBenchmarked = 0;
    int decodedFrames = 0;
    int nullFrames = 0;
    qint64 elapsedMs = 0;
    double fps = 0.0;
};

struct SeekBenchmarkSample {
    int64_t targetFrame = 0;
    qint64 elapsedMs = 0;
    bool success = false;
    int64_t decodedFrame = -1;
};

struct SeekBenchmarkResult {
    bool success = false;
    QString error;
    VideoStreamInfo info;
    bool hardwareAccelerated = false;
    QVector<SeekBenchmarkSample> samples;
    int successfulSeeks = 0;
    int nullSeeks = 0;
    qint64 maxSeekMs = 0;
    double avgSeekMs = 0.0;
};

DecodeBenchmarkResult benchmarkDecodeFrames(const QString& mediaPath, int maxFrames = 90);
SeekBenchmarkResult benchmarkDecodeSeeks(const QString& mediaPath,
                                         const QVector<int64_t>& targetFrames = {});

} // namespace editor
