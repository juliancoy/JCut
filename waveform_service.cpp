#include "waveform_service.h"

#include "debug_controls.h"

#include <QDateTime>
#include <QDir>
#include <QFileInfo>
#include <QtConcurrent/QtConcurrent>

#include <algorithm>
#include <cmath>
#include <limits>
#include <vector>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/channel_layout.h>
#include <libavutil/samplefmt.h>
#include <libswresample/swresample.h>
}

namespace editor {

namespace {

constexpr int kMinBaseWindowSamples = 64;
constexpr int kMaxBaseWindowSamples = 8192;
constexpr int kTargetCoarsestBins = 512;
constexpr int kMaxWaveformLevels = 32;

inline float dbToAmp(float db) {
    return std::pow(10.0f, db / 20.0f);
}

} // namespace

WaveformService& WaveformService::instance() {
    static WaveformService service;
    return service;
}

QString WaveformService::canonicalPath(const QString& mediaPath) const {
    if (mediaPath.trimmed().isEmpty()) {
        return QString();
    }
    return QFileInfo(mediaPath).absoluteFilePath();
}

bool WaveformService::queryEnvelope(const QString& mediaPath,
                                    int64_t sampleStart,
                                    int64_t sampleEnd,
                                    int columns,
                                    QVector<float>* minOut,
                                    QVector<float>* maxOut,
                                    const QString& variantKey,
                                    const WaveformProcessSettings* processSettings) {
    if (!minOut || !maxOut) {
        return false;
    }
    const int safeColumns = qBound(1, columns, 16384);
    minOut->fill(0.0f, safeColumns);
    maxOut->fill(0.0f, safeColumns);

    const QString path = canonicalPath(mediaPath);
    if (path.isEmpty()) {
        return false;
    }

    const QFileInfo info(path);
    if (!info.exists() || !info.isFile()) {
        return false;
    }
    const qint64 mtimeMs = info.lastModified().toMSecsSinceEpoch();
    const qint64 fileSize = info.size();

    QMutexLocker locker(&m_mutex);
    Entry& entry = m_entries[path];
    const bool fingerprintChanged =
        entry.fileMtimeMs != mtimeMs || entry.fileSize != fileSize;
    if (fingerprintChanged) {
        entry = Entry{};
        entry.fileMtimeMs = mtimeMs;
        entry.fileSize = fileSize;
    }
    entry.lastAccessMs = QDateTime::currentMSecsSinceEpoch();

    const int desiredBaseWindow =
        qBound(kMinBaseWindowSamples,
               debugTimelineAudioEnvelopeGranularity(),
               kMaxBaseWindowSamples);
    const bool needsRebuildForGranularity =
        entry.baseWindowSamples > 0 && entry.baseWindowSamples != desiredBaseWindow;
    if (needsRebuildForGranularity) {
        entry.ready = false;
        entry.failed = false;
        entry.levels.clear();
        entry.processedVariants.clear();
    }

    if (!entry.ready) {
        ensureDecodeScheduledLocked(path, &entry);
        return false;
    }
    if (entry.levels.isEmpty() || entry.totalSamples <= 0) {
        return false;
    }

    const int64_t boundedStart = qBound<int64_t>(0, sampleStart, entry.totalSamples);
    const int64_t boundedEnd = qBound<int64_t>(boundedStart, sampleEnd, entry.totalSamples);
    const int64_t spanSamples = qMax<int64_t>(1, boundedEnd - boundedStart);
    const double samplesPerColumn =
        static_cast<double>(spanSamples) / static_cast<double>(safeColumns);

    const QVector<WaveformLevel>* levelsPtr = &entry.levels;
    if (processSettings && !variantKey.trimmed().isEmpty()) {
        auto processedIt = entry.processedVariants.find(variantKey);
        if (processedIt == entry.processedVariants.end()) {
            Entry::ProcessedVariant processed;
            processed.levels = buildProcessedLevels(entry.levels, entry.sampleRate, *processSettings);
            if (processed.levels.isEmpty()) {
                return false;
            }
            processed.lastAccessMs = entry.lastAccessMs;
            processedIt = entry.processedVariants.insert(variantKey, std::move(processed));
        } else {
            processedIt->lastAccessMs = entry.lastAccessMs;
        }
        levelsPtr = &processedIt->levels;

        constexpr int kMaxProcessedVariantsPerEntry = 8;
        if (entry.processedVariants.size() > kMaxProcessedVariantsPerEntry) {
            auto oldest = entry.processedVariants.begin();
            for (auto it = entry.processedVariants.begin(); it != entry.processedVariants.end(); ++it) {
                if (it.key() == variantKey) {
                    continue;
                }
                if (oldest.key() == variantKey || it->lastAccessMs < oldest->lastAccessMs) {
                    oldest = it;
                }
            }
            if (oldest != entry.processedVariants.end() && oldest.key() != variantKey) {
                entry.processedVariants.erase(oldest);
            }
        }
    }

    if (!levelsPtr || levelsPtr->isEmpty()) {
        return false;
    }

    int levelIndex = 0;
    const double targetSamplesPerColumn = qMax(1.0, samplesPerColumn);
    double bestLevelScore = std::numeric_limits<double>::infinity();
    for (int i = 0; i < levelsPtr->size(); ++i) {
        const int windowSamples = (*levelsPtr)[i].windowSamples;
        if (windowSamples <= 0) {
            continue;
        }
        const double ratio = static_cast<double>(windowSamples) / targetSamplesPerColumn;
        double score = std::abs(std::log2(ratio));
        if (ratio > 1.0) {
            // Slightly prefer finer levels over coarser ones to avoid chunky low-zoom plateaus.
            score += 0.35;
        }
        if (score < bestLevelScore) {
            bestLevelScore = score;
            levelIndex = i;
        }
    }
    const WaveformLevel& level = (*levelsPtr)[levelIndex];
    if (level.windowSamples <= 0 || level.minValues.isEmpty() ||
        level.minValues.size() != level.maxValues.size()) {
        return false;
    }

    for (int x = 0; x < safeColumns; ++x) {
        const int64_t colStart = boundedStart + static_cast<int64_t>(
            std::floor((static_cast<double>(x) * spanSamples) / safeColumns));
        const int64_t colEnd = boundedStart + static_cast<int64_t>(
            std::ceil((static_cast<double>(x + 1) * spanSamples) / safeColumns));
        const int64_t boundedColEnd = qBound<int64_t>(colStart + 1, colEnd, boundedEnd);
        const int windowSamples = qMax(1, level.windowSamples);
        const int startIdx = qBound<int>(
            0, static_cast<int>(colStart / qMax(1, level.windowSamples)),
            level.minValues.size() - 1);
        const int endIdx = qBound<int>(
            startIdx, static_cast<int>((boundedColEnd - 1) / qMax(1, level.windowSamples)),
            level.minValues.size() - 1);

        float minV = 0.0f;
        float maxV = 0.0f;
        bool initialized = false;
        const bool preferInterpolatedDetail =
            level.minValues.size() > 1 &&
            samplesPerColumn < (static_cast<double>(windowSamples) * 0.75);
        if (preferInterpolatedDetail) {
            const double colCenter =
                static_cast<double>(colStart) +
                (static_cast<double>(boundedColEnd - colStart) * 0.5);
            const double idxFloat = colCenter / static_cast<double>(windowSamples);
            const int i0 = qBound(0, static_cast<int>(std::floor(idxFloat)), level.minValues.size() - 1);
            const int i1 = qBound(0, i0 + 1, level.minValues.size() - 1);
            const float t = qBound(0.0f, static_cast<float>(idxFloat - static_cast<double>(i0)), 1.0f);
            minV = ((1.0f - t) * level.minValues[i0]) + (t * level.minValues[i1]);
            maxV = ((1.0f - t) * level.maxValues[i0]) + (t * level.maxValues[i1]);
            initialized = true;
        } else {
            for (int i = startIdx; i <= endIdx; ++i) {
                if (!initialized) {
                    minV = level.minValues[i];
                    maxV = level.maxValues[i];
                    initialized = true;
                } else {
                    minV = std::min(minV, level.minValues[i]);
                    maxV = std::max(maxV, level.maxValues[i]);
                }
            }
        }
        if (minV > maxV) {
            std::swap(minV, maxV);
        }
        (*minOut)[x] = qBound(-1.0f, minV, 1.0f);
        (*maxOut)[x] = qBound(-1.0f, maxV, 1.0f);
    }

    return true;
}

void WaveformService::setReadyCallback(std::function<void()> callback) {
    QMutexLocker locker(&m_mutex);
    if (!m_readyCallback) {
        m_readyCallback = std::move(callback);
        return;
    }
    const std::function<void()> existing = m_readyCallback;
    m_readyCallback = [existing, callback = std::move(callback)]() {
        if (existing) {
            existing();
        }
        if (callback) {
            callback();
        }
    };
}

void WaveformService::trimCache(int maxEntries) {
    QMutexLocker locker(&m_mutex);
    trimCacheLocked(maxEntries);
}

void WaveformService::ensureDecodeScheduledLocked(const QString& path, Entry* entry) {
    if (!entry || entry->decoding) {
        return;
    }
    entry->decoding = true;
    entry->failed = false;
    const qint64 expectedMtime = entry->fileMtimeMs;
    const qint64 expectedSize = entry->fileSize;
    const int baseWindowSamples =
        qBound(kMinBaseWindowSamples,
               debugTimelineAudioEnvelopeGranularity(),
               kMaxBaseWindowSamples);
    entry->baseWindowSamples = baseWindowSamples;

    (void)QtConcurrent::run([this, path, expectedMtime, expectedSize, baseWindowSamples]() {
        int sampleRate = 0;
        int64_t totalSamples = 0;
        QVector<WaveformLevel> levels;
        const bool ok = decodePyramidForPath(path,
                                             baseWindowSamples,
                                             &sampleRate,
                                             &totalSamples,
                                             &levels);
        finishDecode(path,
                     expectedMtime,
                     expectedSize,
                     sampleRate,
                     totalSamples,
                     std::move(levels),
                     ok);
    });
}

void WaveformService::finishDecode(const QString& path,
                                   qint64 fileMtimeMs,
                                   qint64 fileSize,
                                   int sampleRate,
                                   int64_t totalSamples,
                                   QVector<WaveformLevel>&& levels,
                                   bool ok) {
    std::function<void()> callback;
    {
        QMutexLocker locker(&m_mutex);
        auto it = m_entries.find(path);
        if (it == m_entries.end()) {
            return;
        }
        Entry& entry = it.value();
        if (entry.fileMtimeMs != fileMtimeMs || entry.fileSize != fileSize) {
            return;
        }

        entry.decoding = false;
        entry.ready = ok && !levels.isEmpty();
        entry.failed = !entry.ready;
        entry.sampleRate = sampleRate;
        entry.totalSamples = totalSamples;
        entry.levels = entry.ready ? std::move(levels) : QVector<WaveformLevel>{};
        entry.processedVariants.clear();
        entry.lastAccessMs = QDateTime::currentMSecsSinceEpoch();
        trimCacheLocked(32);
        callback = m_readyCallback;
    }
    if (callback) {
        callback();
    }
}

void WaveformService::trimCacheLocked(int maxEntries) {
    const int safeMax = qBound(4, maxEntries, 256);
    while (m_entries.size() > safeMax) {
        auto oldestIt = m_entries.begin();
        for (auto it = m_entries.begin(); it != m_entries.end(); ++it) {
            if (it->decoding) {
                continue;
            }
            if (oldestIt->decoding || it->lastAccessMs < oldestIt->lastAccessMs) {
                oldestIt = it;
            }
        }
        if (oldestIt == m_entries.end() || oldestIt->decoding) {
            break;
        }
        m_entries.erase(oldestIt);
    }
}

QVector<WaveformService::WaveformLevel> WaveformService::buildProcessedLevels(
    const QVector<WaveformLevel>& sourceLevels,
    int sampleRate,
    const WaveformProcessSettings& settings) const {
    if (sourceLevels.isEmpty()) {
        return {};
    }

    const float amplifyGain = settings.amplifyEnabled ? dbToAmp(settings.amplifyDb) : 1.0f;
    const float normalizeTarget = dbToAmp(std::clamp(settings.normalizeTargetDb, -24.0f, 0.0f));
    // Selective normalization targets a consistent near-full speaking level.
    // Keep a small headroom to avoid clipping artifacts in preview dynamics.
    constexpr float kSelectiveTargetLinear = 0.95f;
    const int selectivePasses = qBound(1, settings.selectiveNormalizePasses, 8);
    const float minSegmentSeconds = std::clamp(settings.selectiveNormalizeMinSegmentSeconds, 0.1f, 30.0f);
    const int safeSampleRate = qMax(1, sampleRate);
    const float peakLinear = dbToAmp(std::clamp(settings.peakThresholdDb, -24.0f, 0.0f));
    const float limiterLinear = dbToAmp(std::clamp(settings.limiterThresholdDb, -12.0f, 0.0f));
    const float compLinear = dbToAmp(std::clamp(settings.compressorThresholdDb, -30.0f, -1.0f));
    const float compRatio = std::clamp(settings.compressorRatio, 1.0f, 20.0f);

    auto processSignedSample = [&](float sample) -> float {
        const float sign = sample < 0.0f ? -1.0f : 1.0f;
        float out = std::abs(sample) * amplifyGain;
        if (settings.compressorEnabled && out > compLinear) {
            const float over = out - compLinear;
            out = compLinear + (over / compRatio);
        }
        if (settings.peakReductionEnabled && out > peakLinear) {
            out = peakLinear + (out - peakLinear) * 0.35f;
        }
        if (settings.limiterEnabled) {
            out = std::min(out, limiterLinear);
        }
        return std::clamp(sign * out, -1.0f, 1.0f);
    };

    auto applySelectiveNormalize = [&](WaveformLevel* level) {
        if (!level || !settings.selectiveNormalizeEnabled || level->minValues.isEmpty()) {
            return;
        }
        const float binSeconds =
            static_cast<float>(qMax(1, level->windowSamples)) / static_cast<float>(safeSampleRate);
        const int minBins = qMax(1, static_cast<int>(std::ceil(minSegmentSeconds / qMax(0.0001f, binSeconds))));
        const float selectivePeakLinear =
            dbToAmp(std::clamp(settings.selectiveNormalizePeakDb, -36.0f, 0.0f));

        for (int pass = 0; pass < selectivePasses; ++pass) {
            const int valueCount = level->minValues.size();
            QVector<float> localPeaks(valueCount, 0.0f);
            QVector<int> peakIndices;
            peakIndices.reserve(valueCount / 4);
            for (int i = 0; i < valueCount; ++i) {
                localPeaks[i] = std::max(std::abs(level->minValues[i]), std::abs(level->maxValues[i]));
            }
            for (int i = 0; i < valueCount; ++i) {
                const float v = localPeaks[i];
                if (v < selectivePeakLinear) {
                    continue;
                }
                const float left = (i > 0) ? localPeaks[i - 1] : v;
                const float right = (i + 1 < valueCount) ? localPeaks[i + 1] : v;
                if (v >= left && v >= right) {
                    peakIndices.push_back(i);
                }
            }
            // Treat audio bounds as synthetic full peaks so segmenting spans the full clip.
            if (valueCount >= 2) {
                if (peakIndices.isEmpty() || peakIndices.first() != 0) {
                    peakIndices.prepend(0);
                }
                if (peakIndices.last() != (valueCount - 1)) {
                    peakIndices.push_back(valueCount - 1);
                }
            }
            if (peakIndices.size() < 2) {
                continue;
            }

            for (int p = 0; p + 1 < peakIndices.size(); ++p) {
                const int start = peakIndices[p];
                const int endExclusive = peakIndices[p + 1] + 1;
                if (endExclusive <= start) {
                    continue;
                }
                const int len = endExclusive - start;
                float segmentPeak = 0.0f;
                bool hasAboveThresholdSample = false;
                for (int i = start; i < endExclusive; ++i) {
                    segmentPeak = std::max(segmentPeak, localPeaks[i]);
                    if (localPeaks[i] >= selectivePeakLinear) {
                        hasAboveThresholdSample = true;
                    }
                }
                if (len < minBins || !hasAboveThresholdSample || segmentPeak <= 0.000001f) {
                    continue;
                }
                const float gain = kSelectiveTargetLinear / segmentPeak;
                for (int i = start; i < endExclusive; ++i) {
                    level->minValues[i] = std::clamp(level->minValues[i] * gain, -1.0f, 1.0f);
                    level->maxValues[i] = std::clamp(level->maxValues[i] * gain, -1.0f, 1.0f);
                    if (level->minValues[i] > level->maxValues[i]) {
                        std::swap(level->minValues[i], level->maxValues[i]);
                    }
                }
            }
        }
    };

    QVector<WaveformLevel> processed;
    processed.reserve(sourceLevels.size());
    for (const WaveformLevel& src : sourceLevels) {
        if (src.minValues.size() != src.maxValues.size()) {
            continue;
        }
        WaveformLevel dst;
        dst.windowSamples = src.windowSamples;
        dst.minValues.resize(src.minValues.size());
        dst.maxValues.resize(src.maxValues.size());
        for (int i = 0; i < src.minValues.size(); ++i) {
            float minP = processSignedSample(src.minValues[i]);
            float maxP = processSignedSample(src.maxValues[i]);
            if (minP > maxP) {
                std::swap(minP, maxP);
            }
            dst.minValues[i] = minP;
            dst.maxValues[i] = maxP;
        }
        applySelectiveNormalize(&dst);
        processed.push_back(std::move(dst));
    }

    if (settings.normalizeEnabled && !processed.isEmpty()) {
        float postProcessPeak = 0.0f;
        const WaveformLevel& baseProcessedLevel = processed.constFirst();
        for (int i = 0; i < baseProcessedLevel.minValues.size(); ++i) {
            postProcessPeak = std::max(postProcessPeak, std::abs(baseProcessedLevel.minValues[i]));
            postProcessPeak = std::max(postProcessPeak, std::abs(baseProcessedLevel.maxValues[i]));
        }
        if (postProcessPeak > 0.000001f) {
            const float normalizeGain = normalizeTarget / postProcessPeak;
            for (WaveformLevel& level : processed) {
                for (int i = 0; i < level.minValues.size(); ++i) {
                    level.minValues[i] = std::clamp(level.minValues[i] * normalizeGain, -1.0f, 1.0f);
                    level.maxValues[i] = std::clamp(level.maxValues[i] * normalizeGain, -1.0f, 1.0f);
                    if (level.minValues[i] > level.maxValues[i]) {
                        std::swap(level.minValues[i], level.maxValues[i]);
                    }
                }
            }
        }
    }

    return processed;
}

bool WaveformService::decodePyramidForPath(const QString& mediaPath,
                                           int baseWindowSamples,
                                           int* sampleRateOut,
                                           int64_t* totalSamplesOut,
                                           QVector<WaveformLevel>* levelsOut) {
    if (!sampleRateOut || !totalSamplesOut || !levelsOut) {
        return false;
    }
    *sampleRateOut = 0;
    *totalSamplesOut = 0;
    levelsOut->clear();

    AVFormatContext* formatCtx = nullptr;
    if (avformat_open_input(&formatCtx, mediaPath.toUtf8().constData(), nullptr, nullptr) < 0 || !formatCtx) {
        return false;
    }
    auto closeFormat = [&]() { avformat_close_input(&formatCtx); };

    if (avformat_find_stream_info(formatCtx, nullptr) < 0) {
        closeFormat();
        return false;
    }
    const int streamIndex = av_find_best_stream(formatCtx, AVMEDIA_TYPE_AUDIO, -1, -1, nullptr, 0);
    if (streamIndex < 0 || streamIndex >= static_cast<int>(formatCtx->nb_streams)) {
        closeFormat();
        return false;
    }
    AVStream* stream = formatCtx->streams[streamIndex];
    const AVCodec* codec = avcodec_find_decoder(stream->codecpar->codec_id);
    if (!codec) {
        closeFormat();
        return false;
    }
    AVCodecContext* codecCtx = avcodec_alloc_context3(codec);
    if (!codecCtx) {
        closeFormat();
        return false;
    }
    auto freeCodec = [&]() { avcodec_free_context(&codecCtx); };

    if (avcodec_parameters_to_context(codecCtx, stream->codecpar) < 0 ||
        avcodec_open2(codecCtx, codec, nullptr) < 0) {
        freeCodec();
        closeFormat();
        return false;
    }

    const AVChannelLayout outLayout = AV_CHANNEL_LAYOUT_MONO;
    SwrContext* swr = nullptr;
    if (swr_alloc_set_opts2(&swr,
                            &outLayout,
                            AV_SAMPLE_FMT_FLT,
                            codecCtx->sample_rate,
                            &codecCtx->ch_layout,
                            codecCtx->sample_fmt,
                            codecCtx->sample_rate,
                            0,
                            nullptr) < 0 ||
        !swr ||
        swr_init(swr) < 0) {
        if (swr) {
            swr_free(&swr);
        }
        freeCodec();
        closeFormat();
        return false;
    }
    auto freeSwr = [&]() { swr_free(&swr); };

    AVPacket* packet = av_packet_alloc();
    AVFrame* frame = av_frame_alloc();
    if (!packet || !frame) {
        if (packet) {
            av_packet_free(&packet);
        }
        if (frame) {
            av_frame_free(&frame);
        }
        freeSwr();
        freeCodec();
        closeFormat();
        return false;
    }
    auto freeFramePacket = [&]() {
        av_frame_free(&frame);
        av_packet_free(&packet);
    };

    const int safeWindow = qBound(kMinBaseWindowSamples, baseWindowSamples, kMaxBaseWindowSamples);
    QVector<float> minL0;
    QVector<float> maxL0;
    minL0.reserve(16384);
    maxL0.reserve(16384);

    int windowCount = 0;
    float windowMin = 0.0f;
    float windowMax = 0.0f;
    int64_t totalSamples = 0;

    auto appendSample = [&](float sample) {
        const float bounded = qBound(-1.0f, sample, 1.0f);
        if (windowCount == 0) {
            windowMin = bounded;
            windowMax = bounded;
        } else {
            windowMin = std::min(windowMin, bounded);
            windowMax = std::max(windowMax, bounded);
        }
        ++windowCount;
        ++totalSamples;
        if (windowCount >= safeWindow) {
            minL0.push_back(windowMin);
            maxL0.push_back(windowMax);
            windowCount = 0;
        }
    };

    while (av_read_frame(formatCtx, packet) >= 0) {
        if (packet->stream_index != streamIndex) {
            av_packet_unref(packet);
            continue;
        }
        if (avcodec_send_packet(codecCtx, packet) < 0) {
            av_packet_unref(packet);
            continue;
        }
        av_packet_unref(packet);
        while (avcodec_receive_frame(codecCtx, frame) == 0) {
            const int outSamples = swr_get_out_samples(swr, frame->nb_samples);
            if (outSamples <= 0) {
                av_frame_unref(frame);
                continue;
            }
            std::vector<float> mono(static_cast<size_t>(outSamples), 0.0f);
            uint8_t* outData[1] = {reinterpret_cast<uint8_t*>(mono.data())};
            const int converted = swr_convert(swr,
                                              outData,
                                              outSamples,
                                              const_cast<const uint8_t**>(frame->extended_data),
                                              frame->nb_samples);
            if (converted > 0) {
                for (int i = 0; i < converted; ++i) {
                    appendSample(mono[static_cast<size_t>(i)]);
                }
            }
            av_frame_unref(frame);
        }
    }

    avcodec_send_packet(codecCtx, nullptr);
    while (avcodec_receive_frame(codecCtx, frame) == 0) {
        const int outSamples = swr_get_out_samples(swr, frame->nb_samples);
        if (outSamples > 0) {
            std::vector<float> mono(static_cast<size_t>(outSamples), 0.0f);
            uint8_t* outData[1] = {reinterpret_cast<uint8_t*>(mono.data())};
            const int converted = swr_convert(swr,
                                              outData,
                                              outSamples,
                                              const_cast<const uint8_t**>(frame->extended_data),
                                              frame->nb_samples);
            if (converted > 0) {
                for (int i = 0; i < converted; ++i) {
                    appendSample(mono[static_cast<size_t>(i)]);
                }
            }
        }
        av_frame_unref(frame);
    }

    if (windowCount > 0) {
        minL0.push_back(windowMin);
        maxL0.push_back(windowMax);
    }

    if (minL0.isEmpty() || minL0.size() != maxL0.size()) {
        freeFramePacket();
        freeSwr();
        freeCodec();
        closeFormat();
        return false;
    }

    QVector<WaveformLevel> levels;
    WaveformLevel base;
    base.windowSamples = safeWindow;
    base.minValues = std::move(minL0);
    base.maxValues = std::move(maxL0);
    levels.push_back(std::move(base));

    while (levels.constLast().minValues.size() > kTargetCoarsestBins &&
           levels.size() < kMaxWaveformLevels &&
           levels.constLast().windowSamples <= (std::numeric_limits<int>::max() / 2)) {
        const WaveformLevel& prev = levels.constLast();
        WaveformLevel next;
        next.windowSamples = prev.windowSamples * 2;
        next.minValues.reserve((prev.minValues.size() + 1) / 2);
        next.maxValues.reserve((prev.maxValues.size() + 1) / 2);
        for (int i = 0; i < prev.minValues.size(); i += 2) {
            const int j = qMin(i + 1, prev.minValues.size() - 1);
            next.minValues.push_back(std::min(prev.minValues[i], prev.minValues[j]));
            next.maxValues.push_back(std::max(prev.maxValues[i], prev.maxValues[j]));
        }
        levels.push_back(std::move(next));
    }

    const int decodedSampleRate = qMax(1, codecCtx->sample_rate);
    *totalSamplesOut = qMax<int64_t>(1, totalSamples);
    *levelsOut = std::move(levels);
    *sampleRateOut = decodedSampleRate;

    freeFramePacket();
    freeSwr();
    freeCodec();
    closeFormat();
    return true;
}

} // namespace editor
