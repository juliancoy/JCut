#include "standalone_export_renderer.h"

#include "export_timing.h"
#include "ffmpeg_compat.h"
#include "audio_time_stretch_core.h"
#include "standalone_audio_mixer.h"
#include "standalone_timeline_renderer.h"
#include "timeline_fps.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <iomanip>
#include <memory>
#include <limits>
#include <sstream>
#include <string>
#include <system_error>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/imgutils.h>
#include <libavutil/opt.h>
#include <libswresample/swresample.h>
#include <libswscale/swscale.h>
}

namespace {

struct AvFormatOutputContextDeleter {
    void operator()(AVFormatContext* value) const
    {
        if (!value) {
            return;
        }
        if (!(value->oformat->flags & AVFMT_NOFILE) && value->pb) {
            avio_closep(&value->pb);
        }
        avformat_free_context(value);
    }
};

struct AvCodecContextDeleter {
    void operator()(AVCodecContext* value) const
    {
        avcodec_free_context(&value);
    }
};

struct AvFrameDeleter {
    void operator()(AVFrame* value) const
    {
        av_frame_free(&value);
    }
};

struct AvPacketDeleter {
    void operator()(AVPacket* value) const
    {
        av_packet_free(&value);
    }
};

struct SwsContextDeleter {
    void operator()(SwsContext* value) const
    {
        sws_freeContext(value);
    }
};

struct SwrContextDeleter {
    void operator()(SwrContext* value) const
    {
        swr_free(&value);
    }
};

std::string avErrorToString(int errorCode)
{
    char buffer[AV_ERROR_MAX_STRING_SIZE] = {};
    av_strerror(errorCode, buffer, sizeof(buffer));
    return buffer;
}

int timelineEndFrame(const jcut::EditorDocumentCore& document)
{
    int endFrame = 0;
    for (const jcut::EditorClip& clip : document.clips) {
        endFrame = std::max(endFrame, clip.startFrame + std::max(1, clip.durationFrames));
    }
    endFrame = std::max(endFrame, static_cast<int>(document.exportRequest.exportEndFrame));
    return endFrame;
}

int exportStartFrame(const jcut::EditorDocumentCore& document)
{
    return std::max(0, static_cast<int>(document.exportRequest.exportStartFrame));
}

int exportEndFrame(const jcut::EditorDocumentCore& document)
{
    return std::max(exportStartFrame(document), timelineEndFrame(document));
}

std::int64_t totalFramesToRender(const jcut::EditorDocumentCore& document)
{
    const int startFrame = exportStartFrame(document);
    const int endFrame = exportEndFrame(document);
    return jcut::export_timing::outputFrameCountForTimelineRange(
        startFrame,
        endFrame,
        document.exportRequest.outputFps,
        document.exportRequest.playbackSpeed);
}

const AVCodec* pickVideoEncoder(const AVOutputFormat* outputFormat,
                                const std::string& requestedFormat)
{
    if (requestedFormat == "mov_mjpeg") {
        return avcodec_find_encoder(AV_CODEC_ID_MJPEG);
    }
    if (requestedFormat == "webm") {
        // Generic codec-ID lookup can select a device-only V4L2/QSV encoder
        // that is registered but unusable on the current host. Prefer the
        // portable encoders required for a dependable WebM export.
        if (const AVCodec* codec = avcodec_find_encoder_by_name("libvpx-vp9")) {
            return codec;
        }
        return avcodec_find_encoder_by_name("libvpx");
    }
    if (outputFormat && outputFormat->video_codec != AV_CODEC_ID_NONE) {
        if (const AVCodec* codec = avcodec_find_encoder(outputFormat->video_codec)) {
            return codec;
        }
    }
    if (const AVCodec* codec = avcodec_find_encoder_by_name("libx264")) {
        return codec;
    }
    if (const AVCodec* codec = avcodec_find_encoder(AV_CODEC_ID_H264)) {
        return codec;
    }
    return avcodec_find_encoder(AV_CODEC_ID_MPEG4);
}

const AVCodec* pickAudioEncoder(const std::string& requestedFormat)
{
    if (requestedFormat == "webm") {
        if (const AVCodec* codec = avcodec_find_encoder_by_name("libopus")) {
            return codec;
        }
        if (const AVCodec* codec = avcodec_find_encoder(AV_CODEC_ID_OPUS)) {
            return codec;
        }
        return avcodec_find_encoder(AV_CODEC_ID_VORBIS);
    }
    return avcodec_find_encoder(AV_CODEC_ID_AAC);
}

struct AudioOutput {
    AVStream* stream = nullptr;
    std::unique_ptr<AVCodecContext, AvCodecContextDeleter> codecContext;
    std::unique_ptr<SwrContext, SwrContextDeleter> resampler;
    std::unique_ptr<AVFrame, AvFrameDeleter> frame;
    std::unique_ptr<AVPacket, AvPacketDeleter> packet;
    std::int64_t nextPts = 0;

    bool initialize(AVFormatContext* formatContext,
                    const std::string& requestedFormat,
                    std::string* errorOut)
    {
        const AVCodec* codec = pickAudioEncoder(requestedFormat);
        if (!codec) {
            if (errorOut) {
                *errorOut = requestedFormat == "webm"
                    ? "WebM export requires an Opus or Vorbis audio encoder"
                    : "export requires an AAC audio encoder";
            }
            return false;
        }
        stream = avformat_new_stream(formatContext, codec);
        codecContext.reset(avcodec_alloc_context3(codec));
        if (!stream || !codecContext) {
            if (errorOut) {
                *errorOut = "failed to allocate audio output stream";
            }
            return false;
        }
        codecContext->codec_id = codec->id;
        codecContext->codec_type = AVMEDIA_TYPE_AUDIO;
        codecContext->sample_rate =
            jcut::standalone_render::audio::kSampleRate;
        codecContext->time_base = AVRational{
            1, jcut::standalone_render::audio::kSampleRate};
        codecContext->bit_rate = 192'000;
        codecContext->sample_fmt = codec->sample_fmts
            ? codec->sample_fmts[0]
            : AV_SAMPLE_FMT_FLTP;
#if LIBAVUTIL_VERSION_MAJOR >= 57
        av_channel_layout_default(
            &codecContext->ch_layout,
            jcut::standalone_render::audio::kChannelCount);
#else
        codecContext->channels = jcut::standalone_render::audio::kChannelCount;
        codecContext->channel_layout = AV_CH_LAYOUT_STEREO;
#endif
        if (formatContext->oformat->flags & AVFMT_GLOBALHEADER) {
            codecContext->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;
        }
        const int openResult = avcodec_open2(codecContext.get(), codec, nullptr);
        if (openResult < 0) {
            if (errorOut) {
                *errorOut = "failed to open audio encoder: " +
                    avErrorToString(openResult);
            }
            return false;
        }
        if (avcodec_parameters_from_context(
                stream->codecpar, codecContext.get()) < 0) {
            if (errorOut) {
                *errorOut = "failed to copy audio codec parameters";
            }
            return false;
        }
        stream->time_base = codecContext->time_base;

        resampler.reset(swr_alloc());
        ffmpeg_compat::ChannelLayoutHandle stereoLayout;
        ffmpeg_compat::defaultChannelLayout(
            &stereoLayout, jcut::standalone_render::audio::kChannelCount);
        const bool configured = resampler &&
            ffmpeg_compat::setSwrOutputLayout(
                resampler.get(), &stereoLayout) >= 0 &&
#if LIBAVUTIL_VERSION_MAJOR >= 57
            av_opt_set_chlayout(
                resampler.get(), "in_chlayout", &stereoLayout, 0) >= 0 &&
#else
            av_opt_set_int(resampler.get(), "in_channel_layout",
                           AV_CH_LAYOUT_STEREO, 0) >= 0 &&
#endif
            av_opt_set_int(resampler.get(), "in_sample_rate",
                           jcut::standalone_render::audio::kSampleRate, 0) >= 0 &&
            av_opt_set_int(resampler.get(), "out_sample_rate",
                           codecContext->sample_rate, 0) >= 0 &&
            av_opt_set_sample_fmt(resampler.get(), "in_sample_fmt",
                                  AV_SAMPLE_FMT_FLT, 0) >= 0 &&
            av_opt_set_sample_fmt(resampler.get(), "out_sample_fmt",
                                  codecContext->sample_fmt, 0) >= 0 &&
            swr_init(resampler.get()) >= 0;
        ffmpeg_compat::uninitChannelLayout(&stereoLayout);
        if (!configured) {
            if (errorOut) {
                *errorOut = "failed to configure audio encoder conversion";
            }
            return false;
        }
        frame.reset(av_frame_alloc());
        packet.reset(av_packet_alloc());
        if (!frame || !packet) {
            if (errorOut) {
                *errorOut = "failed to allocate audio encoder buffers";
            }
            return false;
        }
        return true;
    }

    bool drain(AVFormatContext* formatContext, std::string* errorOut)
    {
        for (;;) {
            const int receiveResult =
                avcodec_receive_packet(codecContext.get(), packet.get());
            if (receiveResult == AVERROR(EAGAIN) || receiveResult == AVERROR_EOF) {
                return true;
            }
            if (receiveResult < 0) {
                if (errorOut) {
                    *errorOut = "failed to receive encoded audio packet: " +
                        avErrorToString(receiveResult);
                }
                return false;
            }
            av_packet_rescale_ts(
                packet.get(), codecContext->time_base, stream->time_base);
            packet->stream_index = stream->index;
            const int writeResult =
                av_interleaved_write_frame(formatContext, packet.get());
            av_packet_unref(packet.get());
            if (writeResult < 0) {
                if (errorOut) {
                    *errorOut = "failed to write encoded audio packet: " +
                        avErrorToString(writeResult);
                }
                return false;
            }
        }
    }

    bool encode(AVFormatContext* formatContext,
                const jcut::EditorDocumentCore& document,
                const jcut::standalone_render::audio::DecodedAudioCache& cache,
                std::int64_t timelineStartSample,
                std::int64_t outputSampleCount,
                double playbackSpeed,
                std::string* errorOut)
    {
        const int preferredFrameSize = codecContext->frame_size > 0
            ? codecContext->frame_size
            : 1024;
        std::vector<float> mixed;
        std::vector<float> stretchedMix;
        if (std::abs(playbackSpeed - 1.0) >= 0.0001) {
            const std::int64_t sourceSampleCount =
                static_cast<std::int64_t>(std::llround(
                    static_cast<long double>(outputSampleCount) *
                    playbackSpeed));
            if (sourceSampleCount <= 0 ||
                sourceSampleCount > std::numeric_limits<int>::max() ||
                sourceSampleCount > static_cast<std::int64_t>(
                    std::numeric_limits<std::size_t>::max() /
                    (sizeof(float) *
                     jcut::standalone_render::audio::kChannelCount))) {
                if (errorOut) {
                    *errorOut = "export audio duration is too large";
                }
                return false;
            }
            std::vector<float> timelineMix(
                static_cast<std::size_t>(sourceSampleCount) *
                jcut::standalone_render::audio::kChannelCount);
            jcut::standalone_render::audio::mixAudioChunk(
                document, cache, timelineMix.data(),
                static_cast<int>(sourceSampleCount),
                timelineStartSample, 1.0);
            stretchedMix = jcut::audio::timeStretchPreservePitch(
                timelineMix,
                jcut::standalone_render::audio::kChannelCount,
                jcut::standalone_render::audio::kSampleRate,
                playbackSpeed);
            if (stretchedMix.empty()) {
                if (errorOut) {
                    *errorOut = "failed to pitch-preserve export playback speed";
                }
                return false;
            }
            stretchedMix.resize(
                static_cast<std::size_t>(outputSampleCount) *
                    jcut::standalone_render::audio::kChannelCount,
                0.0f);
        }
        for (std::int64_t outputOffset = 0;
             outputOffset < outputSampleCount;) {
            const int sampleCount = static_cast<int>(std::min<std::int64_t>(
                preferredFrameSize, outputSampleCount - outputOffset));
            mixed.resize(static_cast<std::size_t>(sampleCount) *
                jcut::standalone_render::audio::kChannelCount);
            if (stretchedMix.empty()) {
                jcut::standalone_render::audio::mixAudioChunk(
                    document, cache, mixed.data(), sampleCount,
                    timelineStartSample + outputOffset, 1.0);
            } else {
                std::copy_n(
                    stretchedMix.data() +
                        static_cast<std::size_t>(outputOffset) *
                            jcut::standalone_render::audio::kChannelCount,
                    static_cast<std::size_t>(sampleCount) *
                        jcut::standalone_render::audio::kChannelCount,
                    mixed.data());
            }

            av_frame_unref(frame.get());
            frame->format = codecContext->sample_fmt;
            frame->sample_rate = codecContext->sample_rate;
            frame->nb_samples = sampleCount;
            if (ffmpeg_compat::copyFrameChannelLayout(
                    frame.get(), codecContext.get()) < 0 ||
                av_frame_get_buffer(frame.get(), 0) < 0) {
                if (errorOut) {
                    *errorOut = "failed to allocate encoded audio frame";
                }
                return false;
            }
            const std::uint8_t* input[] = {
                reinterpret_cast<const std::uint8_t*>(mixed.data())};
            const int converted = swr_convert(
                resampler.get(), frame->data, sampleCount, input, sampleCount);
            if (converted < 0) {
                if (errorOut) {
                    *errorOut = "failed to convert mixed audio: " +
                        avErrorToString(converted);
                }
                return false;
            }
            frame->nb_samples = converted;
            frame->pts = nextPts;
            nextPts += converted;
            const int sendResult =
                avcodec_send_frame(codecContext.get(), frame.get());
            if (sendResult < 0 || !drain(formatContext, errorOut)) {
                if (sendResult < 0 && errorOut) {
                    *errorOut = "failed to submit mixed audio: " +
                        avErrorToString(sendResult);
                }
                return false;
            }
            outputOffset += sampleCount;
        }
        const int flushResult = avcodec_send_frame(codecContext.get(), nullptr);
        if (flushResult < 0) {
            if (errorOut) {
                *errorOut = "failed to flush audio encoder: " +
                    avErrorToString(flushResult);
            }
            return false;
        }
        return drain(formatContext, errorOut);
    }
};

const char* muxerNameForOutputFormat(const std::string& outputFormat)
{
    if (outputFormat == "mp4") {
        return "mp4";
    }
    if (outputFormat == "mov" || outputFormat == "mov_mjpeg") {
        return "mov";
    }
    if (outputFormat == "mkv") {
        return "matroska";
    }
    if (outputFormat == "webm") {
        return "webm";
    }
    return nullptr;
}

AVCodecID imageCodecIdForFormat(const std::string& format)
{
    if (format == "png") {
        return AV_CODEC_ID_PNG;
    }
    if (format == "webp") {
        return AV_CODEC_ID_WEBP;
    }
    return AV_CODEC_ID_MJPEG;
}

AVPixelFormat imagePixelFormatForFormat(const std::string& format)
{
    if (format == "png") {
        return AV_PIX_FMT_RGBA;
    }
    return AV_PIX_FMT_YUV420P;
}

std::string imageExtensionForFormat(const std::string& format)
{
    if (format == "jpg") {
        return "jpeg";
    }
    return format.empty() ? std::string("jpeg") : format;
}

std::filesystem::path sequenceFramePath(const std::filesystem::path& directory,
                                        std::int64_t frameIndex,
                                        const std::string& extension)
{
    std::ostringstream name;
    name << "frame_" << std::setw(8) << std::setfill('0') << frameIndex << "." << extension;
    return directory / name.str();
}

bool flushEncoder(AVCodecContext* codecContext,
                  AVFormatContext* formatContext,
                  AVStream* stream,
                  std::string* errorOut)
{
    const int sendResult = avcodec_send_frame(codecContext, nullptr);
    if (sendResult < 0) {
        if (errorOut) {
            *errorOut = "failed to flush encoder: " + avErrorToString(sendResult);
        }
        return false;
    }

    std::unique_ptr<AVPacket, AvPacketDeleter> packet(av_packet_alloc());
    if (!packet) {
        if (errorOut) {
            *errorOut = "failed to allocate encoder flush packet";
        }
        return false;
    }

    for (;;) {
        const int receiveResult = avcodec_receive_packet(codecContext, packet.get());
        if (receiveResult == AVERROR(EAGAIN) || receiveResult == AVERROR_EOF) {
            return true;
        }
        if (receiveResult < 0) {
            if (errorOut) {
                *errorOut = "failed to receive flush packet: " + avErrorToString(receiveResult);
            }
            return false;
        }
        av_packet_rescale_ts(packet.get(), codecContext->time_base, stream->time_base);
        packet->stream_index = stream->index;
        const int writeResult = av_interleaved_write_frame(formatContext, packet.get());
        av_packet_unref(packet.get());
        if (writeResult < 0) {
            if (errorOut) {
                *errorOut = "failed to write flush packet: " + avErrorToString(writeResult);
            }
            return false;
        }
    }
}

bool encodeImageBufferToFile(const jcut::core::ImageBuffer& image,
                             const std::filesystem::path& outputPath,
                             const std::string& requestedFormat,
                             std::string* errorOut)
{
    const std::string format = imageExtensionForFormat(requestedFormat);
    AVFormatContext* rawFormatContext = nullptr;
    if (avformat_alloc_output_context2(&rawFormatContext, nullptr, nullptr, outputPath.string().c_str()) < 0 ||
        !rawFormatContext) {
        if (errorOut) {
            *errorOut = "failed to create image output context";
        }
        return false;
    }
    std::unique_ptr<AVFormatContext, AvFormatOutputContextDeleter> formatContext(rawFormatContext);

    const AVCodecID codecId = imageCodecIdForFormat(format);
    const AVCodec* codec = avcodec_find_encoder(codecId);
    if (!codec) {
        if (errorOut) {
            *errorOut = "failed to find image encoder";
        }
        return false;
    }

    AVStream* stream = avformat_new_stream(formatContext.get(), codec);
    if (!stream) {
        if (errorOut) {
            *errorOut = "failed to create image stream";
        }
        return false;
    }

    std::unique_ptr<AVCodecContext, AvCodecContextDeleter> codecContext(avcodec_alloc_context3(codec));
    if (!codecContext) {
        if (errorOut) {
            *errorOut = "failed to allocate image encoder context";
        }
        return false;
    }

    codecContext->codec_id = codec->id;
    codecContext->codec_type = AVMEDIA_TYPE_VIDEO;
    codecContext->width = image.size.width;
    codecContext->height = image.size.height;
    codecContext->time_base = AVRational{1, 1};
    codecContext->framerate = AVRational{1, 1};
    codecContext->pix_fmt = imagePixelFormatForFormat(format);
    if (formatContext->oformat->flags & AVFMT_GLOBALHEADER) {
        codecContext->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;
    }
    if (codecContext->pix_fmt == AV_PIX_FMT_YUV420P) {
        codecContext->color_range = AVCOL_RANGE_JPEG;
    }

    const int openResult = avcodec_open2(codecContext.get(), codec, nullptr);
    if (openResult < 0) {
        if (errorOut) {
            *errorOut = "failed to open image encoder: " + avErrorToString(openResult);
        }
        return false;
    }
    if (avcodec_parameters_from_context(stream->codecpar, codecContext.get()) < 0) {
        if (errorOut) {
            *errorOut = "failed to copy image codec parameters";
        }
        return false;
    }
    stream->time_base = codecContext->time_base;

    if (!(formatContext->oformat->flags & AVFMT_NOFILE)) {
        const int ioOpenResult = avio_open(&formatContext->pb, outputPath.string().c_str(), AVIO_FLAG_WRITE);
        if (ioOpenResult < 0) {
            if (errorOut) {
                *errorOut = "failed to open image output file: " + avErrorToString(ioOpenResult);
            }
            return false;
        }
    }
    if (avformat_write_header(formatContext.get(), nullptr) < 0) {
        if (errorOut) {
            *errorOut = "failed to write image header";
        }
        return false;
    }

    std::unique_ptr<AVFrame, AvFrameDeleter> frame(av_frame_alloc());
    if (!frame) {
        if (errorOut) {
            *errorOut = "failed to allocate image frame";
        }
        return false;
    }
    frame->format = codecContext->pix_fmt;
    frame->width = codecContext->width;
    frame->height = codecContext->height;
    if (av_frame_get_buffer(frame.get(), 32) < 0) {
        if (errorOut) {
            *errorOut = "failed to allocate image frame buffer";
        }
        return false;
    }
    if (av_frame_make_writable(frame.get()) < 0) {
        if (errorOut) {
            *errorOut = "failed to make image frame writable";
        }
        return false;
    }

    std::unique_ptr<SwsContext, SwsContextDeleter> scaleContext(
        sws_getContext(image.size.width,
                       image.size.height,
                       AV_PIX_FMT_RGBA,
                       image.size.width,
                       image.size.height,
                       codecContext->pix_fmt,
                       SWS_BILINEAR,
                       nullptr,
                       nullptr,
                       nullptr));
    if (!scaleContext) {
        if (errorOut) {
            *errorOut = "failed to create image conversion context";
        }
        return false;
    }

    std::uint8_t* sourceData[4] = {
        const_cast<std::uint8_t*>(image.bytes.data()),
        nullptr,
        nullptr,
        nullptr};
    int sourceLinesize[4] = {image.strideBytes, 0, 0, 0};
    if (sws_scale(scaleContext.get(),
                  sourceData,
                  sourceLinesize,
                  0,
                  image.size.height,
                  frame->data,
                  frame->linesize) <= 0) {
        if (errorOut) {
            *errorOut = "failed to convert image frame";
        }
        return false;
    }

    frame->pts = 0;
    const int sendResult = avcodec_send_frame(codecContext.get(), frame.get());
    if (sendResult < 0) {
        if (errorOut) {
            *errorOut = "failed to submit image frame: " + avErrorToString(sendResult);
        }
        return false;
    }

    std::unique_ptr<AVPacket, AvPacketDeleter> packet(av_packet_alloc());
    if (!packet) {
        if (errorOut) {
            *errorOut = "failed to allocate image packet";
        }
        return false;
    }
    const int receiveResult = avcodec_receive_packet(codecContext.get(), packet.get());
    if (receiveResult < 0) {
        if (errorOut) {
            *errorOut = "failed to receive image packet: " + avErrorToString(receiveResult);
        }
        return false;
    }
    av_packet_rescale_ts(packet.get(), codecContext->time_base, stream->time_base);
    packet->stream_index = stream->index;
    const int writeResult = av_interleaved_write_frame(formatContext.get(), packet.get());
    av_packet_unref(packet.get());
    if (writeResult < 0) {
        if (errorOut) {
            *errorOut = "failed to write image packet: " + avErrorToString(writeResult);
        }
        return false;
    }
    if (av_write_trailer(formatContext.get()) < 0) {
        if (errorOut) {
            *errorOut = "failed to finalize image file";
        }
        return false;
    }
    return true;
}

} // namespace

namespace jcut::standalone_render {

render::RenderResultCore exportTimelineToFile(const ExportRenderRequest& request,
                                              const ExportProgressCallback& progressCallback)
{
    render::RenderResultCore result;
    result.requestedRenderBackend = "standalone_cpu";
    result.effectiveRenderBackend = "standalone_cpu";

    const render::RenderRequestCore& exportRequest = request.document.exportRequest;
    if (!exportRequest.outputSize.valid()) {
        result.message = "invalid output size";
        return result;
    }
    if (exportRequest.outputPath.empty()) {
        result.message = "no output path selected";
        return result;
    }
    const std::filesystem::path outputPath(exportRequest.outputPath);
    std::error_code filesystemError;
    if (!outputPath.parent_path().empty()) {
        std::filesystem::create_directories(outputPath.parent_path(), filesystemError);
        if (filesystemError) {
            result.message = "failed to create output directory";
            return result;
        }
    }

    std::filesystem::path namedOutputDir;
    const bool imageSequenceRequested =
        exportRequest.outputMode != render::RenderOutputMode::EncodedFile &&
        !exportRequest.imageSequenceFormat.empty();
    if (imageSequenceRequested && !outputPath.stem().empty()) {
        namedOutputDir = outputPath.parent_path() / outputPath.stem();
        std::filesystem::create_directories(namedOutputDir, filesystemError);
        if (filesystemError) {
            result.message = "failed to create named output directory";
            return result;
        }
    }

    const bool writeImageSequence =
        imageSequenceRequested &&
        !namedOutputDir.empty();
    const bool writeEncodedVideo =
        exportRequest.outputMode != render::RenderOutputMode::ImageSequence;

    const double outputFps = jcut::export_timing::normalizedOutputFps(exportRequest.outputFps);
    const AVRational frameRate = av_d2q(outputFps, 1001000);

    std::unique_ptr<AVFormatContext, AvFormatOutputContextDeleter> formatContext;
    const AVCodec* codec = nullptr;
    AVStream* stream = nullptr;
    std::unique_ptr<AVCodecContext, AvCodecContextDeleter> codecContext;
    std::unique_ptr<AVFrame, AvFrameDeleter> frame;
    std::unique_ptr<AVPacket, AvPacketDeleter> packet;
    std::unique_ptr<SwsContext, SwsContextDeleter> scaleContext;
    audio::DecodedAudioCache decodedAudio;
    std::unique_ptr<AudioOutput> audioOutput;

    if (writeEncodedVideo) {
        std::string audioDecodeError;
        if (!audio::decodeDocumentAudio(
                request.document, request.rootDirectory,
                &decodedAudio, &audioDecodeError)) {
            result.message = audioDecodeError.empty()
                ? "failed to decode export audio"
                : audioDecodeError;
            return result;
        }
    }

    if (writeEncodedVideo) {
        AVFormatContext* rawFormatContext = nullptr;
        const char* muxerName = muxerNameForOutputFormat(
            exportRequest.outputFormat);
        if (avformat_alloc_output_context2(&rawFormatContext,
                                           nullptr,
                                           muxerName,
                                           exportRequest.outputPath.c_str()) < 0 ||
            !rawFormatContext) {
            result.message = "failed to create output format context";
            return result;
        }
        formatContext.reset(rawFormatContext);

        codec = pickVideoEncoder(formatContext->oformat, exportRequest.outputFormat);
        if (!codec) {
            result.message = exportRequest.outputFormat == "webm"
                ? "WebM export requires a libvpx VP8/VP9 encoder"
                : "failed to find a video encoder";
            return result;
        }

        stream = avformat_new_stream(formatContext.get(), codec);
        if (!stream) {
            result.message = "failed to create output stream";
            return result;
        }

        codecContext.reset(avcodec_alloc_context3(codec));
        if (!codecContext) {
            result.message = "failed to allocate encoder context";
            return result;
        }

        codecContext->codec_id = codec->id;
        codecContext->codec_type = AVMEDIA_TYPE_VIDEO;
        codecContext->pix_fmt = codec->id == AV_CODEC_ID_MJPEG
            ? AV_PIX_FMT_YUVJ420P
            : AV_PIX_FMT_YUV420P;
        codecContext->width = exportRequest.outputSize.width;
        codecContext->height = exportRequest.outputSize.height;
        codecContext->time_base = av_inv_q(frameRate);
        codecContext->framerate = frameRate;
        codecContext->gop_size = std::max(1, static_cast<int>(std::lround(outputFps)));
        codecContext->max_b_frames = 0;
        codecContext->bit_rate = codec->id == AV_CODEC_ID_MJPEG
            ? 40'000'000
            : 8'000'000;
        if (codec->id == AV_CODEC_ID_MJPEG) {
            codecContext->color_range = AVCOL_RANGE_JPEG;
        }
        if (formatContext->oformat->flags & AVFMT_GLOBALHEADER) {
            codecContext->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;
        }
        if (codec->id == AV_CODEC_ID_H264 || codec->id == AV_CODEC_ID_MPEG4) {
            av_opt_set(codecContext->priv_data, "preset", "veryfast", 0);
        }

        const int openResult = avcodec_open2(codecContext.get(), codec, nullptr);
        if (openResult < 0) {
            result.message = "failed to open encoder: " + avErrorToString(openResult);
            return result;
        }
        if (avcodec_parameters_from_context(stream->codecpar, codecContext.get()) < 0) {
            result.message = "failed to copy codec parameters";
            return result;
        }
        stream->time_base = codecContext->time_base;

        if (!decodedAudio.empty()) {
            audioOutput = std::make_unique<AudioOutput>();
            std::string audioError;
            if (!audioOutput->initialize(
                    formatContext.get(), exportRequest.outputFormat,
                    &audioError)) {
                result.message = audioError;
                return result;
            }
        }

        if (!(formatContext->oformat->flags & AVFMT_NOFILE)) {
            const int ioOpenResult =
                avio_open(&formatContext->pb, exportRequest.outputPath.c_str(), AVIO_FLAG_WRITE);
            if (ioOpenResult < 0) {
                result.message = "failed to open output file: " + avErrorToString(ioOpenResult);
                return result;
            }
        }
        if (avformat_write_header(formatContext.get(), nullptr) < 0) {
            result.message = "failed to write output header";
            return result;
        }

        frame.reset(av_frame_alloc());
        if (!frame) {
            result.message = "failed to allocate video frame";
            return result;
        }
        frame->format = codecContext->pix_fmt;
        frame->width = codecContext->width;
        frame->height = codecContext->height;
        if (av_frame_get_buffer(frame.get(), 32) < 0) {
            result.message = "failed to allocate video frame buffer";
            return result;
        }

        packet.reset(av_packet_alloc());
        if (!packet) {
            result.message = "failed to allocate packet";
            return result;
        }

        scaleContext.reset(
            sws_getContext(exportRequest.outputSize.width,
                           exportRequest.outputSize.height,
                           AV_PIX_FMT_RGBA,
                           exportRequest.outputSize.width,
                           exportRequest.outputSize.height,
                           codecContext->pix_fmt,
                           SWS_BILINEAR,
                           nullptr,
                           nullptr,
                           nullptr));
        if (!scaleContext) {
            result.message = "failed to create color conversion context";
            return result;
        }
    }

    TimelineRenderer renderer;
    const int startFrame = exportStartFrame(request.document);
    const int endFrame = exportEndFrame(request.document);
    const std::int64_t totalFrames = request.outputFrameLimit > 0
        ? std::min(
            totalFramesToRender(request.document),
            request.outputFrameLimit)
        : totalFramesToRender(request.document);
    const double playbackSpeed =
        jcut::export_timing::normalizedPlaybackSpeed(exportRequest.playbackSpeed);
    const auto startTime = std::chrono::steady_clock::now();

    for (std::int64_t frameIndex = 0; frameIndex < totalFrames; ++frameIndex) {
        const jcut::export_timing::ExportFrameTiming exportFrameTiming =
            jcut::export_timing::frameTimingForOutputFrame(
                frameIndex,
                startFrame,
                endFrame,
                outputFps,
                playbackSpeed);
        const double clampedFramePosition = exportFrameTiming.timelineFramePosition;
        const TimelineRenderResult frameResult = renderer.renderFrame({
            request.document,
            exportRequest.outputSize,
            clampedFramePosition,
            request.rootDirectory});
        if (!frameResult.success || frameResult.image.empty()) {
            result.message = frameResult.message.empty() ? "failed to render export frame" : frameResult.message;
            return result;
        }

        if (writeImageSequence) {
            const std::string extension = imageExtensionForFormat(exportRequest.imageSequenceFormat);
            const std::filesystem::path imagePath =
                sequenceFramePath(
                    namedOutputDir,
                    request.imageSequenceFrameNumberOffset + frameIndex,
                    extension);
            std::string imageError;
            if (!encodeImageBufferToFile(frameResult.image, imagePath, exportRequest.imageSequenceFormat, &imageError)) {
                result.message = imageError.empty() ? "failed to write image-sequence frame" : imageError;
                return result;
            }
        }

        if (writeEncodedVideo) {
            if (av_frame_make_writable(frame.get()) < 0) {
                result.message = "failed to make output frame writable";
                return result;
            }

            std::uint8_t* sourceData[4] = {
                const_cast<std::uint8_t*>(frameResult.image.bytes.data()),
                nullptr,
                nullptr,
                nullptr};
            int sourceLinesize[4] = {frameResult.image.strideBytes, 0, 0, 0};
            if (sws_scale(scaleContext.get(),
                          sourceData,
                          sourceLinesize,
                          0,
                          exportRequest.outputSize.height,
                          frame->data,
                          frame->linesize) <= 0) {
                result.message = "failed to convert export frame";
                return result;
            }

            frame->pts = frameIndex;
            const int sendFrameResult = avcodec_send_frame(codecContext.get(), frame.get());
            if (sendFrameResult < 0) {
                result.message = "failed to submit frame to encoder: " + avErrorToString(sendFrameResult);
                return result;
            }

            while (true) {
                const int receivePacketResult = avcodec_receive_packet(codecContext.get(), packet.get());
                if (receivePacketResult == AVERROR(EAGAIN) || receivePacketResult == AVERROR_EOF) {
                    break;
                }
                if (receivePacketResult < 0) {
                    result.message =
                        "failed to receive encoded packet: " + avErrorToString(receivePacketResult);
                    return result;
                }
                av_packet_rescale_ts(packet.get(), codecContext->time_base, stream->time_base);
                packet->stream_index = stream->index;
                const int writeResult = av_interleaved_write_frame(formatContext.get(), packet.get());
                av_packet_unref(packet.get());
                if (writeResult < 0) {
                    result.message = "failed to write encoded packet: " + avErrorToString(writeResult);
                    return result;
                }
            }
        }

        result.framesRendered = frameIndex + 1;
        if (progressCallback) {
            const auto now = std::chrono::steady_clock::now();
            const auto elapsedMs =
                std::chrono::duration_cast<std::chrono::milliseconds>(now - startTime).count();
            render::RenderProgressCore progress;
            progress.framesCompleted = frameIndex + 1;
            progress.totalFrames = totalFrames;
            progress.segmentIndex = 1;
            progress.segmentCount = 1;
            progress.timelineFrame = static_cast<std::int64_t>(std::floor(clampedFramePosition));
            progress.segmentStartFrame = startFrame;
            progress.segmentEndFrame = endFrame;
            progress.usingGpu = false;
            progress.usingHardwareEncode = false;
            progress.encoderLabel = writeEncodedVideo && codec && codec->name ? codec->name : "image_sequence";
            progress.elapsedMs = elapsedMs;
            progress.estimatedRemainingMs =
                progress.framesCompleted > 0
                    ? (elapsedMs * (totalFrames - progress.framesCompleted)) / progress.framesCompleted
                    : -1;
            progress.previewFrame = frameResult.image;
            if (!progressCallback(progress)) {
                result.cancelled = true;
                result.message = "export cancelled";
                return result;
            }
        }
    }

    if (writeEncodedVideo) {
        std::string flushError;
        if (!flushEncoder(codecContext.get(), formatContext.get(), stream, &flushError)) {
            result.message = flushError;
            return result;
        }
        if (audioOutput) {
            const std::int64_t outputSampleCount =
                static_cast<std::int64_t>(std::llround(
                    static_cast<long double>(totalFrames) *
                    audio::kSampleRate / outputFps));
            const std::int64_t timelineStartSample =
                static_cast<std::int64_t>(startFrame) *
                audio::kSamplesPerTimelineFrame;
            std::string audioError;
            if (!audioOutput->encode(
                    formatContext.get(), request.document, decodedAudio,
                    timelineStartSample, outputSampleCount,
                    playbackSpeed, &audioError)) {
                result.message = audioError;
                return result;
            }
        }
        if (av_write_trailer(formatContext.get()) < 0) {
            result.message = "failed to finalize output file";
            return result;
        }
    }

    const auto finishedTime = std::chrono::steady_clock::now();
    result.success = true;
    result.usedGpu = false;
    result.usedHardwareEncode = false;
    result.encoderLabel = writeEncodedVideo && codec && codec->name ? codec->name : "image_sequence";
    result.elapsedMs =
        std::chrono::duration_cast<std::chrono::milliseconds>(finishedTime - startTime).count();
    result.message = writeEncodedVideo && writeImageSequence
        ? "export and image-sequence completed"
        : (writeEncodedVideo ? "export completed" : "image-sequence export completed");
    result.namedOutputDir = namedOutputDir.string();
    return result;
}

} // namespace jcut::standalone_render
