#include "standalone_audio_mixer.h"

#include "audio_clip_fade.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <unordered_map>

namespace {

double boundedSourceFps(const jcut::EditorClip& clip)
{
    return std::isfinite(clip.sourceFps) && clip.sourceFps > 0.001
        ? clip.sourceFps
        : 30.0;
}

std::int64_t sourceFramesToSamples(const jcut::EditorClip& clip,
                                   std::int64_t frames)
{
    return std::max<std::int64_t>(
        0,
        static_cast<std::int64_t>(std::llround(
            static_cast<long double>(std::max<std::int64_t>(0, frames)) *
            jcut::standalone_render::audio::kSampleRate /
            boundedSourceFps(clip))));
}

std::int64_t clipSourceInSamples(const jcut::EditorClip& clip)
{
    return sourceFramesToSamples(clip, clip.sourceInFrame) +
        std::max<std::int64_t>(0, clip.sourceInSubframeSamples);
}

int markerDelta(const jcut::EditorRenderSyncMarker& marker)
{
    const int magnitude = std::max(1, marker.count);
    return marker.skipFrame ? magnitude : -magnitude;
}

std::int64_t adjustedLocalFrame(const jcut::EditorDocumentCore& document,
                                const jcut::EditorClip& clip,
                                std::int64_t localFrame)
{
    const std::int64_t boundedLocalFrame = std::max<std::int64_t>(0, localFrame);
    const std::int64_t timelineFrame =
        static_cast<std::int64_t>(clip.startFrame) + boundedLocalFrame;
    int cumulativeDelta = 0;
    for (const jcut::EditorRenderSyncMarker& marker :
         document.renderSyncMarkers) {
        if (marker.clipId == clip.persistentId && marker.frame < timelineFrame) {
            cumulativeDelta += markerDelta(marker);
        }
    }
    return std::max<std::int64_t>(
        0, boundedLocalFrame + static_cast<std::int64_t>(cumulativeDelta));
}

const jcut::EditorTrack* trackForClip(const jcut::EditorDocumentCore& document,
                                     const jcut::EditorClip& clip)
{
    const auto track = std::find_if(
        document.tracks.cbegin(), document.tracks.cend(),
        [&](const jcut::EditorTrack& value) { return value.id == clip.trackId; });
    return track == document.tracks.cend() ? nullptr : &*track;
}

bool anySolo(const jcut::EditorDocumentCore& document)
{
    return std::any_of(
               document.clips.cbegin(), document.clips.cend(),
               [](const jcut::EditorClip& clip) {
                   return clip.hasAudio && clip.audioEnabled && clip.audioSolo;
               }) ||
        std::any_of(
               document.tracks.cbegin(), document.tracks.cend(),
               [](const jcut::EditorTrack& track) { return track.audioSolo; });
}

float mixerGain(const jcut::EditorDocumentCore& document,
                const jcut::EditorClip& clip,
                bool soloActive)
{
    if (!clip.hasAudio || !clip.audioEnabled) {
        return 0.0f;
    }
    double gain = std::clamp(clip.audioGain, 0.0, 4.0);
    bool clipOrTrackSolo = clip.audioSolo;
    if (const jcut::EditorTrack* track = trackForClip(document, clip)) {
        if (!track->audioEnabled || track->audioMuted) {
            return 0.0f;
        }
        gain *= std::clamp(track->audioGain, 0.0, 4.0);
        clipOrTrackSolo = clipOrTrackSolo || track->audioSolo;
    }
    if (soloActive && !clipOrTrackSolo) {
        return 0.0f;
    }
    return static_cast<float>(gain);
}

} // namespace

namespace jcut::standalone_render::audio {

std::int64_t clipTimelineStartSamples(const EditorClip& clip)
{
    return std::max<std::int64_t>(0, clip.startFrame) *
            kSamplesPerTimelineFrame +
        clip.startSubframeSamples;
}

std::int64_t clipTimelineDurationSamples(const EditorClip& clip)
{
    return std::max<std::int64_t>(
        kSamplesPerTimelineFrame,
        std::max<std::int64_t>(0, clip.durationFrames) *
                kSamplesPerTimelineFrame +
            std::max<std::int64_t>(0, clip.durationSubframeSamples));
}

std::int64_t sourceSampleForClipAtTimelineSample(
    const EditorDocumentCore& document,
    const EditorClip& clip,
    std::int64_t timelineSample)
{
    const std::int64_t localTimelineSample = std::max<std::int64_t>(
        0, timelineSample - clipTimelineStartSamples(clip));
    const std::int64_t boundedLocalSample = std::min<std::int64_t>(
        localTimelineSample, clipTimelineDurationSamples(clip) - 1);
    const std::int64_t localFrame =
        boundedLocalSample / kSamplesPerTimelineFrame;
    const std::int64_t subframeSample =
        boundedLocalSample % kSamplesPerTimelineFrame;
    const std::int64_t adjustedSamples =
        adjustedLocalFrame(document, clip, localFrame) *
            kSamplesPerTimelineFrame +
        subframeSample;
    const std::int64_t playbackRateScaled = std::max<std::int64_t>(
        1, static_cast<std::int64_t>(clip.playbackRate * 1000.0));
    std::int64_t sourceSample = clipSourceInSamples(clip) +
        (adjustedSamples * playbackRateScaled) / 1000;
    if (clip.sourceDurationFrames > 0) {
        const std::int64_t sourceEnd = clipSourceInSamples(clip) +
            std::max<std::int64_t>(
                0, sourceFramesToSamples(clip, clip.sourceDurationFrames) - 1);
        sourceSample = std::min(sourceSample, sourceEnd);
    }
    return std::max<std::int64_t>(0, sourceSample);
}

void mixAudioChunk(const EditorDocumentCore& document,
                   const DecodedAudioCache& cache,
                   float* output,
                   int frames,
                   std::int64_t chunkStartSample,
                   double timelineSampleStep)
{
    if (!output || frames <= 0) {
        return;
    }
    std::fill(output, output +
        static_cast<std::ptrdiff_t>(frames * kChannelCount), 0.0f);
    const double sampleStep = std::isfinite(timelineSampleStep) &&
            timelineSampleStep > 0.001
        ? timelineSampleStep
        : 1.0;
    const bool soloActive = anySolo(document);
    for (const EditorClip& clip : document.clips) {
        const float gain = mixerGain(document, clip, soloActive);
        if (gain <= 0.0f) {
            continue;
        }
        const auto decoded = cache.find(clip.id);
        if (decoded == cache.end() || !decoded->second.valid) {
            continue;
        }
        const DecodedAudioClip& audio = decoded->second;
        const std::int64_t decodedFrames = static_cast<std::int64_t>(
            audio.samples.size() / kChannelCount);
        if (decodedFrames <= 0) {
            continue;
        }
        const std::int64_t clipStart = clipTimelineStartSamples(clip);
        const std::int64_t clipEnd =
            clipStart + clipTimelineDurationSamples(clip);
        const int fadeSamples = editor::audio::effectiveClipFadeSamples(
            clip.fadeSamples);
        for (int outputFrame = 0; outputFrame < frames; ++outputFrame) {
            const std::int64_t timelineSample = chunkStartSample +
                static_cast<std::int64_t>(std::floor(
                    static_cast<double>(outputFrame) * sampleStep));
            if (timelineSample < clipStart || timelineSample >= clipEnd) {
                continue;
            }
            const std::int64_t sourceSample =
                sourceSampleForClipAtTimelineSample(
                    document, clip, timelineSample);
            const std::int64_t decodedFrame =
                sourceSample - audio.sourceStartSample;
            if (decodedFrame < 0 || decodedFrame >= decodedFrames) {
                continue;
            }
            const float fade = editor::audio::clipFadeGain(
                timelineSample, clipStart, clipEnd, fadeSamples);
            const float effectiveGain = gain * fade;
            const std::size_t inputOffset = static_cast<std::size_t>(
                decodedFrame * kChannelCount);
            const std::size_t outputOffset = static_cast<std::size_t>(
                outputFrame * kChannelCount);
            for (int channel = 0; channel < kChannelCount; ++channel) {
                output[outputOffset + channel] = std::clamp(
                    output[outputOffset + channel] +
                        audio.samples[inputOffset + channel] * effectiveGain,
                    -1.0f,
                    1.0f);
            }
        }
    }
}

} // namespace jcut::standalone_render::audio
