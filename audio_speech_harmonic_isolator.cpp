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
    const std::vector<float> input(
        request.samples->cbegin(), request.samples->cend());
    const std::vector<float> output = jcut::audio::isolateSpeechHarmonics(
        input, request.channelCount, request.sampleRate,
        request.transportSpeed, progressCallback,
        toCoreStretchSettings(request.rubberBand));
    return QVector<float>(output.cbegin(), output.cend());
}

} // namespace editor::audio
