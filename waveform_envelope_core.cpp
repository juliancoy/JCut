#include "waveform_envelope_core.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <limits>

namespace jcut::audio {

bool queryWaveformEnvelope(
    const WaveformSampleView& samples,
    std::int64_t sourceStartSample,
    std::int64_t sourceEndSampleExclusive,
    int columns,
    std::vector<float>* minimumOut,
    std::vector<float>* maximumOut)
{
    if (!minimumOut || !maximumOut) {
        return false;
    }
    const int boundedColumns =
        std::clamp(columns, 1, 16384);
    minimumOut->assign(
        static_cast<std::size_t>(boundedColumns), 0.0f);
    maximumOut->assign(
        static_cast<std::size_t>(boundedColumns), 0.0f);
    if (!samples.interleavedSamples ||
        samples.frameCount <= 0 ||
        samples.channelCount <= 0 ||
        !std::isfinite(samples.sourceSampleScale) ||
        samples.sourceSampleScale <= 0.0 ||
        sourceEndSampleExclusive <= sourceStartSample) {
        return false;
    }

    const std::int64_t boundedSourceStart =
        std::max(sourceStartSample, samples.sourceStartSample);
    const long double cachedSourceSpan =
        static_cast<long double>(samples.frameCount) /
        samples.sourceSampleScale;
    const std::int64_t cachedSourceEnd =
        cachedSourceSpan >=
                static_cast<long double>(
                    std::numeric_limits<std::int64_t>::max() -
                    samples.sourceStartSample)
            ? std::numeric_limits<std::int64_t>::max()
            : samples.sourceStartSample +
                static_cast<std::int64_t>(
                    std::ceil(cachedSourceSpan));
    const std::int64_t boundedSourceEnd =
        std::min(sourceEndSampleExclusive, cachedSourceEnd);
    if (boundedSourceEnd <= boundedSourceStart) {
        return false;
    }
    const std::int64_t sourceSpan =
        boundedSourceEnd - boundedSourceStart;
    for (int column = 0;
         column < boundedColumns;
         ++column) {
        const std::int64_t columnSourceStart =
            boundedSourceStart +
            static_cast<std::int64_t>(
                (static_cast<long double>(column) *
                 sourceSpan) /
                boundedColumns);
        const std::int64_t columnSourceEnd =
            boundedSourceStart +
            static_cast<std::int64_t>(
                std::ceil(
                    (static_cast<long double>(column + 1) *
                     sourceSpan) /
                    boundedColumns));
        const std::int64_t firstFrame =
            std::clamp<std::int64_t>(
                static_cast<std::int64_t>(
                    std::floor(
                        (columnSourceStart -
                         samples.sourceStartSample) *
                        samples.sourceSampleScale)),
                0,
                samples.frameCount - 1);
        const std::int64_t lastFrameExclusive =
            std::clamp<std::int64_t>(
                static_cast<std::int64_t>(
                    std::ceil(
                        (columnSourceEnd -
                         samples.sourceStartSample) *
                        samples.sourceSampleScale)),
                firstFrame + 1,
                samples.frameCount);
        float minimum = 1.0f;
        float maximum = -1.0f;
        for (std::int64_t frame = firstFrame;
             frame < lastFrameExclusive;
             ++frame) {
            const std::size_t offset =
                static_cast<std::size_t>(frame) *
                static_cast<std::size_t>(
                    samples.channelCount);
            for (int channel = 0;
                 channel < samples.channelCount;
                 ++channel) {
                const float value = std::clamp(
                    samples.interleavedSamples[
                        offset +
                        static_cast<std::size_t>(channel)],
                    -1.0f,
                    1.0f);
                minimum = std::min(minimum, value);
                maximum = std::max(maximum, value);
            }
        }
        (*minimumOut)[static_cast<std::size_t>(column)] =
            minimum;
        (*maximumOut)[static_cast<std::size_t>(column)] =
            maximum;
    }
    return true;
}

} // namespace jcut::audio
