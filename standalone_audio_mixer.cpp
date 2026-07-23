#include "standalone_audio_mixer.h"

#include "audio_clip_fade.h"
#include "audio_dynamics_core.h"
#include "audio_time_stretch_core.h"
#include "ffmpeg_compat.h"
#include "transcript_cut_session_core.h"

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <fstream>
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
                     std::int64_t requestedStartSample,
                     std::int64_t requestedEndSampleExclusive,
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
    AVStream* audioStream =
        format->streams[streamIndex];
    const std::int64_t boundedStartSample =
        std::max<std::int64_t>(
            0, requestedStartSample);
    const std::int64_t boundedEndSample =
        std::max<std::int64_t>(
            boundedStartSample + 1,
            requestedEndSampleExclusive);
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
    if (boundedStartSample > 0) {
        const std::int64_t streamStart =
            audioStream->start_time == AV_NOPTS_VALUE
            ? 0
            : audioStream->start_time;
        const std::int64_t seekTimestamp =
            streamStart +
            av_rescale_q(
                boundedStartSample,
                AVRational{
                    1,
                    jcut::standalone_render::audio::
                        kSampleRate},
                audioStream->time_base);
        if (avformat_seek_file(
                format.get(),
                streamIndex,
                std::numeric_limits<std::int64_t>::min(),
                seekTimestamp,
                seekTimestamp,
                AVSEEK_FLAG_BACKWARD) >= 0) {
            avcodec_flush_buffers(
                codecContext.get());
        }
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

    bool requestedRangeComplete = false;
    std::int64_t inferredSourceSample =
        boundedStartSample;
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
            const std::int64_t frameTimestamp =
                frame->best_effort_timestamp !=
                    AV_NOPTS_VALUE
                ? frame->best_effort_timestamp
                : frame->pts;
            std::int64_t frameStartSample =
                inferredSourceSample;
            if (frameTimestamp != AV_NOPTS_VALUE) {
                const std::int64_t streamStart =
                    audioStream->start_time ==
                        AV_NOPTS_VALUE
                    ? 0
                    : audioStream->start_time;
                frameStartSample = std::max<std::int64_t>(
                    0,
                    av_rescale_q(
                        frameTimestamp - streamStart,
                        audioStream->time_base,
                        AVRational{
                            1,
                            jcut::standalone_render::
                                audio::kSampleRate}));
            }
            const int capacity = static_cast<int>(av_rescale_rnd(
                swr_get_delay(resampler.get(), codecContext->sample_rate) + frame->nb_samples,
                jcut::standalone_render::audio::kSampleRate,
                codecContext->sample_rate, AV_ROUND_UP));
            std::vector<float> convertedSamples(
                static_cast<std::size_t>(capacity) *
                    jcut::standalone_render::audio::
                        kChannelCount);
            std::uint8_t* output[] = {
                reinterpret_cast<std::uint8_t*>(
                    convertedSamples.data())};
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
            inferredSourceSample =
                frameStartSample + converted;
            const std::int64_t keepStart =
                std::max<std::int64_t>(
                    0,
                    boundedStartSample -
                        frameStartSample);
            const std::int64_t keepEnd =
                std::min<std::int64_t>(
                    converted,
                    boundedEndSample -
                        frameStartSample);
            if (keepEnd > keepStart) {
                if (decoded->samples.empty()) {
                    decoded->sourceStartSample =
                        frameStartSample +
                        keepStart;
                }
                const auto first =
                    convertedSamples.begin() +
                    static_cast<std::ptrdiff_t>(
                        keepStart *
                        jcut::standalone_render::
                            audio::kChannelCount);
                const auto last =
                    convertedSamples.begin() +
                    static_cast<std::ptrdiff_t>(
                        keepEnd *
                        jcut::standalone_render::
                            audio::kChannelCount);
                decoded->samples.insert(
                    decoded->samples.end(),
                    first,
                    last);
            }
            if (frameStartSample + converted >=
                boundedEndSample) {
                requestedRangeComplete = true;
            }
            if (flushing && converted == 0) {
                return true;
            }
        }
    };

    while (!requestedRangeComplete &&
           (status = av_read_frame(
                format.get(), packet.get())) >= 0) {
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
    status = requestedRangeComplete
        ? 0
        : avcodec_send_packet(codecContext.get(), nullptr);
    if (status < 0 ||
        (!requestedRangeComplete &&
         !receiveFrames(true))) {
        if (status < 0 && errorOut) {
            *errorOut = "failed to flush audio decoder for '" + path + "': " + avError(status);
        }
        return false;
    }
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

void prepareTranscriptNormalization(
    const jcut::EditorDocumentCore& document,
    const jcut::EditorClip& clip,
    const std::string& rootDirectory,
    jcut::standalone_render::audio::DecodedAudioClip* decoded)
{
    if (!decoded || !document.audioDynamics.transcriptNormalizeEnabled ||
        decoded->samples.empty()) {
        return;
    }
    jcut::TranscriptSourceSpec source;
    source.sourcePath = clip.sourcePath;
    source.audioSourcePath = clip.audioSourcePath;
    source.audioSourceMode = clip.audioSourceMode;
    source.audioSourceStatus = clip.audioSourceStatus;
    source.audioStreamIndex = clip.audioStreamIndex;
    source.sourceRootPath = rootDirectory;
    jcut::TranscriptCutSessionOptions options;
    options.requestedActivePath = clip.transcriptActiveCutPath;
    options.ensureEditable = false;
    options.includeOutsideActiveCut = false;
    options.timing.framesPerSecond =
        std::isfinite(clip.sourceFps) && clip.sourceFps > 0.0
        ? clip.sourceFps
        : 30.0;
    options.timing.prependMilliseconds =
        document.exportRequest.transcriptPrependMs;
    options.timing.postpendMilliseconds =
        document.exportRequest.transcriptPostpendMs;
    options.timing.offsetMilliseconds =
        document.exportRequest.transcriptOffsetMs;
    const jcut::TranscriptCutSession session =
        jcut::loadTranscriptCutSession(source, options);
    if (!session.ok()) {
        return;
    }

    constexpr float kTarget = 0.95f;
    constexpr float kMaximumGain = 2.5f;
    const std::int64_t decodedFrames = static_cast<std::int64_t>(
        decoded->samples.size() /
        jcut::standalone_render::audio::kChannelCount);
    const double scale =
        std::isfinite(decoded->sourceSampleScale) &&
            decoded->sourceSampleScale > 0.0
        ? decoded->sourceSampleScale
        : 1.0;
    for (const jcut::TranscriptRow& row : session.rows) {
        if (!row.eligibleForFollow()) {
            continue;
        }
        const std::int64_t startSourceSample =
            sourceFramesToSamples(clip, row.sourceStartFrame);
        const std::int64_t endSourceSampleExclusive =
            sourceFramesToSamples(clip, row.sourceEndFrame + 1);
        const std::int64_t startDecoded = std::clamp<std::int64_t>(
            static_cast<std::int64_t>(std::floor(
                (startSourceSample - decoded->sourceStartSample) * scale)),
            0, decodedFrames);
        const std::int64_t endDecoded = std::clamp<std::int64_t>(
            static_cast<std::int64_t>(std::ceil(
                (endSourceSampleExclusive - decoded->sourceStartSample) *
                scale)),
            startDecoded, decodedFrames);
        float peak = 0.0f;
        for (std::int64_t frame = startDecoded;
             frame < endDecoded;
             ++frame) {
            const std::size_t offset = static_cast<std::size_t>(
                frame * jcut::standalone_render::audio::kChannelCount);
            peak = std::max(peak, std::abs(decoded->samples[offset]));
            peak = std::max(peak, std::abs(decoded->samples[offset + 1]));
        }
        if (peak <= 0.000001f) {
            continue;
        }
        decoded->transcriptNormalizeSegments.push_back(
            {startSourceSample,
             endSourceSampleExclusive,
             std::min(kMaximumGain, kTarget / peak)});
    }
    std::sort(
        decoded->transcriptNormalizeSegments.begin(),
        decoded->transcriptNormalizeSegments.end(),
        [](const auto& left, const auto& right) {
            return left.startSourceSample < right.startSourceSample;
        });
}

float transcriptNormalizeGainAtSourceSample(
    const jcut::standalone_render::audio::DecodedAudioClip& audio,
    std::int64_t sourceSample)
{
    const auto& segments = audio.transcriptNormalizeSegments;
    if (segments.empty()) return 1.0f;
    constexpr std::int64_t kTransitionSamples = 480;
    constexpr std::int64_t kInterWordBridgeSamples = 5760;
    const auto next = std::upper_bound(
        segments.begin(), segments.end(), sourceSample,
        [](std::int64_t sample, const auto& segment) {
            return sample < segment.startSourceSample;
        });
    int index = -1;
    if (next != segments.begin()) {
        const int candidate = static_cast<int>(
            std::distance(segments.begin(), next - 1));
        if (sourceSample <
            segments[static_cast<std::size_t>(candidate)]
                .endSourceSampleExclusive) {
            index = candidate;
        }
    }
    if (index < 0) {
        const int nextIndex = static_cast<int>(
            std::distance(segments.begin(), next));
        const int previousIndex = nextIndex - 1;
        if (previousIndex >= 0 &&
            nextIndex < static_cast<int>(segments.size())) {
            const auto& previous =
                segments[static_cast<std::size_t>(previousIndex)];
            const auto& following =
                segments[static_cast<std::size_t>(nextIndex)];
            const std::int64_t gapStart =
                previous.endSourceSampleExclusive;
            const std::int64_t gapLength =
                following.startSourceSample - gapStart;
            if (sourceSample >= gapStart && gapLength > 0 &&
                sourceSample < following.startSourceSample &&
                gapLength <= kInterWordBridgeSamples) {
                const float t = static_cast<float>(
                    sourceSample - gapStart) /
                    static_cast<float>(gapLength);
                return previous.gain +
                    (following.gain - previous.gain) *
                        std::clamp(t, 0.0f, 1.0f);
            }
        }
        return 1.0f;
    }
    const auto& current = segments[static_cast<std::size_t>(index)];
    const float previousGain = index > 0
        ? segments[static_cast<std::size_t>(index - 1)].gain
        : 1.0f;
    const float nextGain = index + 1 < static_cast<int>(segments.size())
        ? segments[static_cast<std::size_t>(index + 1)].gain
        : 1.0f;
    const std::int64_t length = std::max<std::int64_t>(
        1, current.endSourceSampleExclusive - current.startSourceSample);
    const std::int64_t fadeLength =
        std::min(kTransitionSamples, length);
    float gain = current.gain;
    if (sourceSample < current.startSourceSample + fadeLength) {
        const float t = static_cast<float>(
            sourceSample - current.startSourceSample) /
            static_cast<float>(fadeLength);
        gain = previousGain +
            (current.gain - previousGain) *
                std::clamp(t, 0.0f, 1.0f);
    }
    if (sourceSample >=
        current.endSourceSampleExclusive - fadeLength) {
        const float t = static_cast<float>(
            sourceSample -
            (current.endSourceSampleExclusive - fadeLength)) /
            static_cast<float>(fadeLength);
        const float endGain = current.gain +
            (nextGain - current.gain) *
                std::clamp(t, 0.0f, 1.0f);
        gain = sourceSample <
                current.startSourceSample + fadeLength
            ? 0.5f * (gain + endGain)
            : endGain;
    }
    return gain;
}

} // namespace

namespace jcut::standalone_render::audio {

bool decodeDocumentAudio(const EditorDocumentCore& document,
                         const std::string& rootDirectory,
                         DecodedAudioCache* cacheOut,
                         std::string* errorOut,
                         const std::vector<EditorExportRange>*
                             timelineRanges)
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
        std::int64_t requestedStartSample = 0;
        std::int64_t requestedEndSampleExclusive =
            std::numeric_limits<std::int64_t>::max();
        if (timelineRanges &&
            !timelineRanges->empty()) {
            const std::int64_t clipStart =
                clipTimelineStartSamples(clip);
            const std::int64_t clipEnd =
                clipStart +
                clipTimelineDurationSamples(clip);
            bool intersectsExport = false;
            std::int64_t minimumSourceSample =
                std::numeric_limits<std::int64_t>::max();
            std::int64_t maximumSourceSample = 0;
            for (const EditorExportRange& range :
                 *timelineRanges) {
                const std::int64_t rangeStart =
                    std::max<std::int64_t>(
                        0, range.startFrame) *
                    kSamplesPerTimelineFrame;
                const std::int64_t rangeEnd =
                    (std::max<std::int64_t>(
                         range.startFrame,
                         range.endFrame) +
                     1) *
                    kSamplesPerTimelineFrame;
                const std::int64_t overlapStart =
                    std::max(clipStart, rangeStart);
                const std::int64_t overlapEnd =
                    std::min(clipEnd, rangeEnd);
                if (overlapEnd <= overlapStart) {
                    continue;
                }
                intersectsExport = true;
                const std::int64_t firstSource =
                    sourceSampleForClipAtTimelineSample(
                        document,
                        clip,
                        overlapStart);
                const std::int64_t lastSource =
                    sourceSampleForClipAtTimelineSample(
                        document,
                        clip,
                        overlapEnd - 1);
                minimumSourceSample = std::min(
                    minimumSourceSample,
                    std::min(
                        firstSource, lastSource));
                maximumSourceSample = std::max(
                    maximumSourceSample,
                    std::max(
                        firstSource, lastSource));
            }
            if (!intersectsExport) {
                continue;
            }
            requestedStartSample =
                minimumSourceSample;
            const std::int64_t padding =
                std::max<std::int64_t>(
                    4096,
                    static_cast<std::int64_t>(
                        std::ceil(
                            boundedPlaybackRate(clip) *
                            kSamplesPerTimelineFrame)));
            requestedEndSampleExclusive =
                maximumSourceSample >
                    std::numeric_limits<
                        std::int64_t>::max() -
                        padding - 1
                ? std::numeric_limits<
                      std::int64_t>::max()
                : maximumSourceSample +
                    padding + 1;
        }
        DecodedAudioClip decoded;
        if (!decodeClipAudio(
                clip,
                path,
                requestedStartSample,
                requestedEndSampleExclusive,
                &decoded,
                errorOut)) {
            cacheOut->clear();
            return false;
        }
        const double playbackRate = boundedPlaybackRate(clip);
        const bool harmonicIsolation =
            document.audioTreatment ==
            EditorAudioTreatment::HarmonicSpeechIsolation;
        if (harmonicIsolation ||
            std::abs(playbackRate - 1.0) >= 0.0001) {
            std::vector<float> stretched = harmonicIsolation
                ? jcut::audio::isolateSpeechHarmonics(
                    decoded.samples, kChannelCount, kSampleRate,
                    playbackRate)
                : jcut::audio::timeStretchPreservePitch(
                    decoded.samples, kChannelCount, kSampleRate,
                    playbackRate);
            if (stretched.empty()) {
                if (errorOut) {
                    *errorOut = harmonicIsolation
                        ? "failed to isolate speech harmonics for clip '" +
                            clip.label + "'"
                        : "failed to pitch-preserve audio for clip '" +
                            clip.label + "'";
                }
                cacheOut->clear();
                return false;
            }
            decoded.samples = std::move(stretched);
            decoded.sourceSampleScale = 1.0 / playbackRate;
        }
        prepareTranscriptNormalization(
            document, clip, rootDirectory, &decoded);
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
            const float transcriptGain =
                transcriptNormalizeGainAtSourceSample(audio, sourceSample);
            const float effectiveGain =
                gain * fade * transcriptGain;
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
    jcut::audio::processAudioDynamicsCore(
        output,
        frames,
        kChannelCount,
        kSampleRate,
        document.audioDynamics);
}

} // namespace jcut::standalone_render::audio
