#pragma once

#include "audio_time_stretch_core.h"

#include <functional>

#include <QVector>

enum class AudioTimeStretchBackend {
    Default,
    RubberBand,
    Sola,
};

enum class RubberBandEngineMode {
    Faster,
    Finer,
};

enum class RubberBandThreadingMode {
    Auto,
    Never,
    Always,
};

enum class RubberBandWindowMode {
    Standard,
    Short,
    Long,
};

enum class RubberBandPitchMode {
    HighSpeed,
    HighQuality,
    HighConsistency,
};

struct AudioTimeStretchRubberBandSettings {
    RubberBandEngineMode engine = RubberBandEngineMode::Faster;
    RubberBandThreadingMode threading = RubberBandThreadingMode::Always;
    RubberBandWindowMode window = RubberBandWindowMode::Standard;
    RubberBandPitchMode pitch = RubberBandPitchMode::HighSpeed;
    bool channelsTogether = true;
};

jcut::audio::StretchSettings toCoreStretchSettings(
    const AudioTimeStretchRubberBandSettings& settings);

QVector<float> timeStretchPreservePitch(const QVector<float>& interleavedSamples,
                                        int channelCount,
                                        int sampleRate,
                                        double speed,
                                        AudioTimeStretchBackend backend = AudioTimeStretchBackend::Default,
                                        const std::function<void(double)>& progressCallback = {},
                                        const AudioTimeStretchRubberBandSettings& rubberBandSettings = {});

QVector<float> timeStretchPreservePitchRubberBand(const QVector<float>& interleavedSamples,
                                                  int channelCount,
                                                  int sampleRate,
                                                  double speed,
                                                  const std::function<void(double)>& progressCallback = {},
                                                  const AudioTimeStretchRubberBandSettings& settings = {});

QVector<float> timeStretchPreservePitchSola(const QVector<float>& interleavedSamples,
                                            int channelCount,
                                            double speed);
