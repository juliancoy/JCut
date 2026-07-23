#include "audio_time_stretch.h"

#include "audio_time_stretch_core.h"

#include <utility>
#include <vector>

jcut::audio::StretchSettings toCoreStretchSettings(
    const AudioTimeStretchRubberBandSettings& settings)
{
    jcut::audio::StretchSettings result;
    result.engine = settings.engine == RubberBandEngineMode::Faster
        ? jcut::audio::StretchEngine::Faster
        : jcut::audio::StretchEngine::Finer;
    switch (settings.threading) {
    case RubberBandThreadingMode::Never:
        result.threading = jcut::audio::StretchThreading::Never;
        break;
    case RubberBandThreadingMode::Always:
        result.threading = jcut::audio::StretchThreading::Always;
        break;
    case RubberBandThreadingMode::Auto:
        result.threading = jcut::audio::StretchThreading::Auto;
        break;
    }
    switch (settings.window) {
    case RubberBandWindowMode::Short:
        result.window = jcut::audio::StretchWindow::Short;
        break;
    case RubberBandWindowMode::Long:
        result.window = jcut::audio::StretchWindow::Long;
        break;
    case RubberBandWindowMode::Standard:
        result.window = jcut::audio::StretchWindow::Standard;
        break;
    }
    switch (settings.pitch) {
    case RubberBandPitchMode::HighSpeed:
        result.pitch = jcut::audio::StretchPitch::HighSpeed;
        break;
    case RubberBandPitchMode::HighQuality:
        result.pitch = jcut::audio::StretchPitch::HighQuality;
        break;
    case RubberBandPitchMode::HighConsistency:
        result.pitch = jcut::audio::StretchPitch::HighConsistency;
        break;
    }
    result.channelsTogether = settings.channelsTogether;
    return result;
}

namespace {

QVector<float> toQVector(std::vector<float> values)
{
    return QVector<float>(values.cbegin(), values.cend());
}

} // namespace

QVector<float> timeStretchPreservePitch(
    const QVector<float>& interleavedSamples,
    int channelCount,
    int sampleRate,
    double speed,
    AudioTimeStretchBackend backend,
    const std::function<void(double)>& progressCallback,
    const AudioTimeStretchRubberBandSettings& rubberBandSettings)
{
    if (backend == AudioTimeStretchBackend::Sola) {
        return {};
    }
    return timeStretchPreservePitchRubberBand(
        interleavedSamples, channelCount, sampleRate, speed,
        progressCallback, rubberBandSettings);
}

QVector<float> timeStretchPreservePitchRubberBand(
    const QVector<float>& interleavedSamples,
    int channelCount,
    int sampleRate,
    double speed,
    const std::function<void(double)>& progressCallback,
    const AudioTimeStretchRubberBandSettings& settings)
{
    const std::vector<float> input(
        interleavedSamples.cbegin(), interleavedSamples.cend());
    return toQVector(jcut::audio::timeStretchPreservePitch(
        input, channelCount, sampleRate, speed, progressCallback,
        toCoreStretchSettings(settings)));
}

QVector<float> timeStretchPreservePitchSola(
    const QVector<float>& interleavedSamples,
    int channelCount,
    double speed)
{
    (void)interleavedSamples;
    (void)channelCount;
    (void)speed;
    return {};
}
