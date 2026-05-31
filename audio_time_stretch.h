#pragma once

#include <QVector>

enum class AudioTimeStretchBackend {
    Default,
    RubberBand,
    Sola,
};

QVector<float> timeStretchPreservePitch(const QVector<float>& interleavedSamples,
                                        int channelCount,
                                        int sampleRate,
                                        double speed,
                                        AudioTimeStretchBackend backend = AudioTimeStretchBackend::Default);

QVector<float> timeStretchPreservePitchRubberBand(const QVector<float>& interleavedSamples,
                                                  int channelCount,
                                                  int sampleRate,
                                                  double speed);

QVector<float> timeStretchPreservePitchSola(const QVector<float>& interleavedSamples,
                                            int channelCount,
                                            double speed);
