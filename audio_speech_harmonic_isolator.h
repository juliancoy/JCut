#pragma once

#include "audio_time_stretch.h"

#include <QVector>

#include <functional>

namespace editor::audio {

// Offline two-stage Rubber Band treatment used to attenuate non-harmonic
// speech content without changing the requested transport duration.
class SpeechHarmonicIsolator final {
public:
    static constexpr int kAlgorithmVersion = 1;

    struct Request {
        const QVector<float>* samples = nullptr;
        int channelCount = 0;
        int sampleRate = 0;
        double transportSpeed = 1.0;
        AudioTimeStretchRubberBandSettings rubberBand;
    };

    static QVector<float> process(
        const Request& request,
        const std::function<void(double)>& progressCallback = {});
};

} // namespace editor::audio
