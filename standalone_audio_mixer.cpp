#include "standalone_audio_mixer.h"

#include "audio_clip_fade.h"
#include "audio_time_stretch_core.h"
#include "ffmpeg_compat.h"

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <limits>
#include <memory>
#include <unordered_map>

extern "C" {
#include <libavformat/avformat.h>
#include <libswresample/swresample.h>
}

namespace {

struct FormatContextDeleter {
    void operator()(AVFormatContext* value) const
    {
        if (value) {
            avformat_close_input(&value);
        }
    }
};

struct CodecContextDeleter {
    void operator()(AVCodecContext* value) const { avcodec_free_context(&value); }
};

struct FrameDeleter {
    void operator()(AVFrame* value) const { av_frame_free(&value); }
};

struct PacketDeleter {
    void operator()(AVPacket* value) const { av_packet_free(&value); }
};

struct SwrContextDeleter {
    void operator()(SwrContext* value) const { swr_free(&value); }
};

std::string avError(int code)
{
    char buffer[AV_ERROR_MAX_STRING_SIZE] = {};
    av_strerror(code, buffer, sizeof(buffer));
    return buffer;
}

std::string clipAudioPath(const jcut::EditorClip& clip,
                          const std::string& rootDirectory)
{
    const bool separateAudio = clip.audioSourceMode == "external" &&
        !clip.audioSourcePath.empty();
    std::filesystem::path path(
        separateAudio ? clip.audioSourcePath : clip.sourcePath);
    if (path.is_relative() && !rootDirectory.empty()) {
        path = std::filesystem::path(rootDirectory) / path;
    }
    return path.lexically_normal().string();
}

bool decodeClipAudio(const jcut::EditorClip& clip,
                     const std::string& path,
                     jcut::standalone_render::audio::DecodedAudioClip* decoded,
                     std::string* errorOut)
{
    AVFormatContext* rawFormat = nullptr;
    int status = avformat_open_input(&rawFormat, path.c_str(), nullptr, nullptr);
    if (status < 0) {
        if (errorOut) {
            *errorOut = "failed to open audio source '" + path + "': " + avError(status);
        }
        return false;
    }
    std::unique_ptr<AVFormatContext, FormatContextDeleter> format(rawFormat);
    status = avformat_find_stream_info(format.get(), nullptr);
    if (status < 0) {
        if (errorOut) {
            *errorOut = "failed to inspect audio source '" + path + "': " + avError(status);
        }
        return false;
    }

    int streamIndex = clip.audioStreamIndex;
    if (streamIndex < 0 || streamIndex >= static_cast<int>(format->nb_streams) ||
        format->streams[streamIndex]->codecpar->codec_type != AVMEDIA_TYPE_AUDIO) {
        streamIndex = av_find_best_stream(
            format.get(), AVMEDIA_TYPE_AUDIO, -1, -1, nullptr, 0);
    }
    if (streamIndex < 0) {
        if (errorOut) {
            *errorOut = "audio source has no decodable audio stream: '" + path + "'";
        }
        return false;
    }

    AVCodecParameters* parameters = format->streams[streamIndex]->codecpar;
    const AVCodec* codec = avcodec_find_decoder(parameters->codec_id);
    if (!codec) {
        if (errorOut) {
            *errorOut = "no decoder is available for audio source '" + path + "'";
        }
        return false;
    }
    std::unique_ptr<AVCodecContext, CodecContextDeleter> codecContext(
        avcodec_alloc_context3(codec));
    if (!codecContext ||
        avcodec_parameters_to_context(codecContext.get(), parameters) < 0) {
        if (errorOut) {
            *errorOut = "failed to configure audio decoder for '" + path + "'";
        }
        return false;
    }
    status = avcodec_open2(codecContext.get(), codec, nullptr);
    if (status < 0) {
        if (errorOut) {
            *errorOut = "failed to open audio decoder for '" + path + "': " + avError(status);
        }
        return false;
    }

    std::unique_ptr<SwrContext, SwrContextDeleter> resampler(swr_alloc());
    ffmpeg_compat::ChannelLayoutHandle stereoLayout;
    ffmpeg_compat::defaultChannelLayout(
        &stereoLayout, jcut::standalone_render::audio::kChannelCount);
    const bool configured = resampler &&
        ffmpeg_compat::setSwrInputLayout(resampler.get(), codecContext.get()) >= 0 &&
        ffmpeg_compat::setSwrOutputLayout(resampler.get(), &stereoLayout) >= 0 &&
        av_opt_set_int(resampler.get(), "in_sample_rate", codecContext->sample_rate, 0) >= 0 &&
        av_opt_set_int(resampler.get(), "out_sample_rate",
                       jcut::standalone_render::audio::kSampleRate, 0) >= 0 &&
        av_opt_set_sample_fmt(resampler.get(), "in_sample_fmt",
                              codecContext->sample_fmt, 0) >= 0 &&
        av_opt_set_sample_fmt(resampler.get(), "out_sample_fmt",
                              AV_SAMPLE_FMT_FLT, 0) >= 0 &&
        swr_init(resampler.get()) >= 0;
    ffmpeg_compat::uninitChannelLayout(&stereoLayout);
    if (!configured) {
        if (errorOut) {
            *errorOut = "failed to configure audio conversion for '" + path + "'";
        }
        return false;
    }

    std::unique_ptr<AVPacket, PacketDeleter> packet(av_packet_alloc());
    std::unique_ptr<AVFrame, FrameDeleter> frame(av_frame_alloc());
    if (!packet || !frame) {
        if (errorOut) {
            *errorOut = "failed to allocate audio decoder buffers";
        }
        return false;
    }

    auto receiveFrames = [&](bool flushing) -> bool {
        while (true) {
            const int receive = avcodec_receive_frame(codecContext.get(), frame.get());
            if (receive == AVERROR(EAGAIN) || receive == AVERROR_EOF) {
                return true;
            }
            if (receive < 0) {
                if (errorOut) {
                    *errorOut = "failed to decode audio from '" + path + "': " + avError(receive);
                }
                return false;
            }
            const int capacity = static_cast<int>(av_rescale_rnd(
                swr_get_delay(resampler.get(), codecContext->sample_rate) + frame->nb_samples,
                jcut::standalone_render::audio::kSampleRate,
                codecContext->sample_rate, AV_ROUND_UP));
            const std::size_t oldSize = decoded->samples.size();
            decoded->samples.resize(oldSize +
                static_cast<std::size_t>(capacity) *
                    jcut::standalone_render::audio::kChannelCount);
            std::uint8_t* output[] = {
                reinterpret_cast<std::uint8_t*>(decoded->samples.data() + oldSize)};
            const int converted = swr_convert(
                resampler.get(), output, capacity,
                const_cast<const std::uint8_t**>(frame->extended_data), frame->nb_samples);
            av_frame_unref(frame.get());
            if (converted < 0) {
                if (errorOut) {
                    *errorOut = "failed to convert audio from '" + path + "': " + avError(converted);
                }
                return false;
            }
            decoded->samples.resize(oldSize +
                static_cast<std::size_t>(converted) *
                    jcut::standalone_render::audio::kChannelCount);
            if (flushing && converted == 0) {
                return true;
            }
        }
    };

    while ((status = av_read_frame(format.get(), packet.get())) >= 0) {
        if (packet->stream_index == streamIndex) {
            status = avcodec_send_packet(codecContext.get(), packet.get());
            av_packet_unref(packet.get());
            if (status < 0 || !receiveFrames(false)) {
                if (status < 0 && errorOut) {
                    *errorOut = "failed to submit audio packet from '" + path + "': " + avError(status);
                }
                return false;
            }
        } else {
            av_packet_unref(packet.get());
        }
    }
    status = avcodec_send_packet(codecContext.get(), nullptr);
    if (status < 0 || !receiveFrames(true)) {
        if (status < 0 && errorOut) {
            *errorOut = "failed to flush audio decoder for '" + path + "': " + avError(status);
        }
        return false;
    }
    decoded->sourceStartSample = 0;
    decoded->valid = !decoded->samples.empty();
    if (!decoded->valid && errorOut) {
        *errorOut = "audio decoder produced no samples for '" + path + "'";
    }
    return decoded->valid;
}

double boundedSourceFps(const jcut::EditorClip& clip)
{
    return std::isfinite(clip.sourceFps) && clip.sourceFps > 0.001
        ? clip.sourceFps
        : 30.0;
}

double boundedPlaybackRate(const jcut::EditorClip& clip)
{
    return std::isfinite(clip.playbackRate) && clip.playbackRate > 0.001
        ? std::min(clip.playbackRate, 64.0)
        : 1.0;
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

bool decodeDocumentAudio(const EditorDocumentCore& document,
                         const std::string& rootDirectory,
                         DecodedAudioCache* cacheOut,
                         std::string* errorOut)
{
    if (!cacheOut) {
        if (errorOut) {
            *errorOut = "audio decode cache is unavailable";
        }
        return false;
    }
    cacheOut->clear();
    for (const EditorClip& clip : document.clips) {
        if (!clip.hasAudio || !clip.audioEnabled) {
            continue;
        }
        const std::string path = clipAudioPath(clip, rootDirectory);
        if (path.empty()) {
            if (errorOut) {
                *errorOut = "audio clip '" + clip.label + "' has no source path";
            }
            cacheOut->clear();
            return false;
        }
        DecodedAudioClip decoded;
        if (!decodeClipAudio(clip, path, &decoded, errorOut)) {
            cacheOut->clear();
            return false;
        }
        const double playbackRate = boundedPlaybackRate(clip);
        if (std::abs(playbackRate - 1.0) >= 0.0001) {
            std::vector<float> stretched =
                jcut::audio::timeStretchPreservePitch(
                    decoded.samples, kChannelCount, kSampleRate,
                    playbackRate);
            if (stretched.empty()) {
                if (errorOut) {
                    *errorOut = "failed to pitch-preserve audio for clip '" +
                        clip.label + "'";
                }
                cacheOut->clear();
                return false;
            }
            decoded.samples = std::move(stretched);
            decoded.sourceSampleScale = 1.0 / playbackRate;
        }
        cacheOut->emplace(clip.id, std::move(decoded));
    }
    return true;
}

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
        1, static_cast<std::int64_t>(boundedPlaybackRate(clip) * 1000.0));
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
        const double pan = std::clamp(
            std::isfinite(clip.audioPan) ? clip.audioPan : 0.0,
            -1.0, 1.0);
        const float channelGain[kChannelCount] = {
            static_cast<float>(1.0 - std::max(0.0, pan)),
            static_cast<float>(1.0 + std::min(0.0, pan))};
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
            const double sourceScale =
                std::isfinite(audio.sourceSampleScale) &&
                        audio.sourceSampleScale > 0.0
                    ? audio.sourceSampleScale
                    : 1.0;
            const std::int64_t decodedFrame = static_cast<std::int64_t>(
                std::llround(static_cast<double>(
                    sourceSample - audio.sourceStartSample) * sourceScale));
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
                        audio.samples[inputOffset + channel] * effectiveGain *
                            channelGain[channel],
                    -1.0f,
                    1.0f);
            }
        }
    }
}

} // namespace jcut::standalone_render::audio
