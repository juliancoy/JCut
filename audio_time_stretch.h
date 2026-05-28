#pragma once

#include <QVector>

QVector<float> timeStretchPreservePitchSola(const QVector<float>& interleavedSamples,
                                            int channelCount,
                                            double speed);

