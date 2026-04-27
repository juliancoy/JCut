#pragma once

#include <QHash>
#include <QMutex>
#include <QString>
#include <QVector>

#include <cstdint>
#include <functional>

namespace editor {

class WaveformService final {
public:
    struct WaveformProcessSettings {
        bool amplifyEnabled = false;
        float amplifyDb = 0.0f;
        bool normalizeEnabled = false;
        float normalizeTargetDb = -1.0f;
        bool peakReductionEnabled = false;
        float peakThresholdDb = -6.0f;
        bool limiterEnabled = false;
        float limiterThresholdDb = -1.0f;
        bool compressorEnabled = false;
        float compressorThresholdDb = -18.0f;
        float compressorRatio = 3.0f;
    };

    struct WaveformLevel {
        int windowSamples = 0;
        QVector<float> minValues;
        QVector<float> maxValues;
    };

    static WaveformService& instance();

    // Returns true when cached data is ready and outputs are filled.
    // Returns false while decode is pending or unavailable.
    bool queryEnvelope(const QString& mediaPath,
                       int64_t sampleStart,
                       int64_t sampleEnd,
                       int columns,
                       QVector<float>* minOut,
                       QVector<float>* maxOut,
                       const QString& variantKey = QString(),
                       const WaveformProcessSettings* processSettings = nullptr);

    void setReadyCallback(std::function<void()> callback);
    void trimCache(int maxEntries = 32);

private:
    WaveformService() = default;
    WaveformService(const WaveformService&) = delete;
    WaveformService& operator=(const WaveformService&) = delete;

    struct Entry {
        struct ProcessedVariant {
            qint64 lastAccessMs = 0;
            QVector<WaveformLevel> levels;
        };

        qint64 fileMtimeMs = -1;
        qint64 fileSize = -1;
        int baseWindowSamples = 0;
        int sampleRate = 0;
        int64_t totalSamples = 0;
        bool decoding = false;
        bool ready = false;
        bool failed = false;
        qint64 lastAccessMs = 0;
        QVector<WaveformLevel> levels;
        QHash<QString, ProcessedVariant> processedVariants;
    };

    QString canonicalPath(const QString& mediaPath) const;
    void ensureDecodeScheduledLocked(const QString& path, Entry* entry);
    void finishDecode(const QString& path,
                      qint64 fileMtimeMs,
                      qint64 fileSize,
                      int sampleRate,
                      int64_t totalSamples,
                      QVector<WaveformLevel>&& levels,
                      bool ok);
    void trimCacheLocked(int maxEntries);
    QVector<WaveformLevel> buildProcessedLevels(const QVector<WaveformLevel>& sourceLevels,
                                                const WaveformProcessSettings& settings) const;

    static bool decodePyramidForPath(const QString& mediaPath,
                                     int baseWindowSamples,
                                     int* sampleRateOut,
                                     int64_t* totalSamplesOut,
                                     QVector<WaveformLevel>* levelsOut);

    mutable QMutex m_mutex;
    QHash<QString, Entry> m_entries;
    std::function<void()> m_readyCallback;
};

} // namespace editor
