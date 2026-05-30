#pragma once

#include <QVector>
#include <QString>

struct AudioTimeStretchCacheEntry {
    QVector<float> samples;
    int sampleRate = 48000;
    int channelCount = 2;
    bool valid = false;
    bool fullyDecoded = false;
};

QString audioTimeStretchSidecarPathForSource(const QString& sourcePath, int speedKey);

bool readAudioTimeStretchSidecar(const QString& sourcePath,
                                 int speedKey,
                                 AudioTimeStretchCacheEntry* entryOut);

bool writeAudioTimeStretchSidecar(const QString& sourcePath,
                                  int speedKey,
                                  const AudioTimeStretchCacheEntry& entry);
