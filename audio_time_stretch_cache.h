#pragma once

#include <QVector>
#include <QString>

#include <cstdint>
#include <functional>

struct AudioTimeStretchCacheEntry {
    QVector<float> samples;
    int sampleRate = 48000;
    int channelCount = 2;
    bool valid = false;
    bool fullyDecoded = false;
};

struct AudioTimeStretchSidecarMetadata {
    int sampleRate = 48000;
    int channelCount = 2;
    bool valid = false;
    bool fullyDecoded = false;
};

QString audioTimeStretchSidecarPathForSource(const QString& sourcePath, int speedKey);

bool readAudioTimeStretchSidecar(const QString& sourcePath,
                                 int speedKey,
                                 AudioTimeStretchCacheEntry* entryOut);

bool readAudioTimeStretchSidecarMetadata(const QString& sourcePath,
                                         int speedKey,
                                         AudioTimeStretchSidecarMetadata* metadataOut);

bool writeAudioTimeStretchSidecar(const QString& sourcePath,
                                  int speedKey,
                                  const AudioTimeStretchCacheEntry& entry,
                                  const std::function<void(double)>& progressCallback = {},
                                  const std::function<bool()>& continuationPredicate = {},
                                  const std::function<bool(
                                      const std::function<bool()>&)>& commitGuard = {});

int64_t audioTimeStretchCacheSampleForSourceSample(int64_t sourceSample, double playbackRate);
int64_t audioTimeStretchCacheEndSampleForSourceEndSample(int64_t sourceEndSample, double playbackRate);
int64_t audioTimeStretchSourceSamplesCoveredByCacheSamples(int64_t cacheSamples, double playbackRate);
bool audioTimeStretchSegmentCoversSourceRange(int64_t segmentCacheStartSample,
                                              int64_t segmentCacheFrameCount,
                                              int64_t sourceStartSample,
                                              int64_t sourceEndSampleExclusive,
                                              double playbackRate);
