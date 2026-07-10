#include "audio_speech_harmonic_isolator.h"

#include <algorithm>

namespace editor::audio {

QVector<float> SpeechHarmonicIsolator::process(
    const Request& request,
    const std::function<void(double)>& progressCallback)
{
    if (!request.samples || request.samples->isEmpty() ||
        request.channelCount <= 0 || request.sampleRate <= 0 ||
        request.transportSpeed <= 0.0) {
        return {};
    }
    auto report = [&progressCallback](double progress) {
        if (progressCallback) {
            progressCallback(std::clamp(progress, 0.0, 1.0));
        }
    };
    const QVector<float> isolated = timeStretchPreservePitch(
        *request.samples, request.channelCount, request.sampleRate,
        request.transportSpeed * 2.0, AudioTimeStretchBackend::RubberBand,
        [&report](double progress) { report(progress * 0.5); },
        request.rubberBand);
    if (isolated.isEmpty()) {
        return {};
    }
    return timeStretchPreservePitch(
        isolated, request.channelCount, request.sampleRate, 0.5,
        AudioTimeStretchBackend::RubberBand,
        [&report](double progress) { report(0.5 + progress * 0.5); },
        request.rubberBand);
}

} // namespace editor::audio
