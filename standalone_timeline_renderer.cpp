#include "standalone_timeline_renderer.h"
#include "image_sequence_directory.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <limits>
#include <memory>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/imgutils.h>
#include <libavutil/rational.h>
#include <libswscale/swscale.h>
}

namespace {

using jcut::EditorClip;
using jcut::EditorDocumentCore;
using jcut::ImageSequenceDirectoryInfo;
using jcut::core::ImageBuffer;
using jcut::core::PixelFormat;
using jcut::core::SizeI;
using jcut::probeImageSequenceDirectory;

struct AvFormatContextDeleter {
    void operator()(AVFormatContext* value) const
    {
        if (value) {
            avformat_close_input(&value);
        }
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

ImageBuffer makeSolidImage(SizeI size, std::uint8_t red, std::uint8_t green, std::uint8_t blue)
{
    ImageBuffer image;
    image.format = PixelFormat::Rgba8;
    image.size = size;
    image.strideBytes = size.width * 4;
    image.bytes.resize(static_cast<std::size_t>(image.strideBytes * size.height));
    for (int y = 0; y < size.height; ++y) {
        for (int x = 0; x < size.width; ++x) {
            const std::size_t offset = static_cast<std::size_t>(y * image.strideBytes + x * 4);
            image.bytes[offset + 0] = red;
            image.bytes[offset + 1] = green;
            image.bytes[offset + 2] = blue;
            image.bytes[offset + 3] = 255;
        }
    }
    return image;
}

void blitImage(const ImageBuffer& source, ImageBuffer* destination, int offsetX, int offsetY)
{
    if (!destination || source.empty() || destination->empty()) {
        return;
    }
    for (int y = 0; y < source.size.height; ++y) {
        const int destY = y + offsetY;
        if (destY < 0 || destY >= destination->size.height) {
            continue;
        }
        for (int x = 0; x < source.size.width; ++x) {
            const int destX = x + offsetX;
            if (destX < 0 || destX >= destination->size.width) {
                continue;
            }
            const std::size_t sourceOffset = static_cast<std::size_t>(y * source.strideBytes + x * 4);
            const std::size_t destOffset =
                static_cast<std::size_t>(destY * destination->strideBytes + destX * 4);
            destination->bytes[destOffset + 0] = source.bytes[sourceOffset + 0];
            destination->bytes[destOffset + 1] = source.bytes[sourceOffset + 1];
            destination->bytes[destOffset + 2] = source.bytes[sourceOffset + 2];
            destination->bytes[destOffset + 3] = source.bytes[sourceOffset + 3];
        }
    }
}

double streamFps(const AVStream* stream)
{
    if (!stream) {
        return 30.0;
    }
    if (stream->avg_frame_rate.num > 0 && stream->avg_frame_rate.den > 0) {
        return av_q2d(stream->avg_frame_rate);
    }
    if (stream->r_frame_rate.num > 0 && stream->r_frame_rate.den > 0) {
        return av_q2d(stream->r_frame_rate);
    }
    return 30.0;
}

std::int64_t ptsForFrameIndex(int frameIndex, const AVStream* stream)
{
    const double fps = std::max(0.001, streamFps(stream));
    const double seconds = static_cast<double>(std::max(0, frameIndex)) / fps;
    const double ticks = seconds / av_q2d(stream->time_base);
    return static_cast<std::int64_t>(std::llround(ticks));
}

const EditorClip* selectVisualClip(const EditorDocumentCore& document, double timelineFrame)
{
    std::unordered_map<int, int> trackOrder;
    for (std::size_t i = 0; i < document.tracks.size(); ++i) {
        trackOrder.emplace(document.tracks[i].id, static_cast<int>(i));
    }

    const EditorClip* best = nullptr;
    int bestTrackOrder = std::numeric_limits<int>::min();
    for (const EditorClip& clip : document.clips) {
        const double clipStart = static_cast<double>(clip.startFrame);
        const double clipEnd = static_cast<double>(clip.startFrame + std::max(1, clip.durationFrames));
        if (timelineFrame < clipStart || timelineFrame >= clipEnd || clip.sourcePath.empty()) {
            continue;
        }
        const auto it = trackOrder.find(clip.trackId);
        const int order = it == trackOrder.end() ? 0 : it->second;
        if (!best || order >= bestTrackOrder) {
            best = &clip;
            bestTrackOrder = order;
        }
    }
    return best;
}

std::string resolveClipPath(const EditorClip& clip, const std::string& rootDirectory)
{
    std::filesystem::path resolvedPath(clip.sourcePath);
    if (resolvedPath.is_relative() && !rootDirectory.empty()) {
        resolvedPath = std::filesystem::path(rootDirectory) / resolvedPath;
    }
    return resolvedPath.lexically_normal().string();
}

class MediaSource {
public:
    explicit MediaSource(std::string path)
        : m_path(std::move(path))
    {
        ImageSequenceDirectoryInfo sequence =
            probeImageSequenceDirectory(std::filesystem::path(m_path));
        m_sequenceFramePaths = std::move(sequence.framePaths);
    }

    bool open(std::string* errorOut)
    {
        if (m_opened) {
            return true;
        }

        AVFormatContext* rawFormatContext = nullptr;
        if (avformat_open_input(&rawFormatContext, m_path.c_str(), nullptr, nullptr) < 0) {
            if (errorOut) {
                *errorOut = "failed to open media source";
            }
            return false;
        }
        m_formatContext.reset(rawFormatContext);
        if (avformat_find_stream_info(m_formatContext.get(), nullptr) < 0) {
            if (errorOut) {
                *errorOut = "failed to read media stream info";
            }
            return false;
        }

        m_videoStreamIndex =
            av_find_best_stream(m_formatContext.get(), AVMEDIA_TYPE_VIDEO, -1, -1, nullptr, 0);
        if (m_videoStreamIndex < 0) {
            if (errorOut) {
                *errorOut = "no video stream available";
            }
            return false;
        }

        m_stream = m_formatContext->streams[m_videoStreamIndex];
        const AVCodec* codec = avcodec_find_decoder(m_stream->codecpar->codec_id);
        if (!codec) {
            if (errorOut) {
                *errorOut = "no decoder available for source stream";
            }
            return false;
        }

        m_codecContext.reset(avcodec_alloc_context3(codec));
        if (!m_codecContext) {
            if (errorOut) {
                *errorOut = "failed to allocate decoder context";
            }
            return false;
        }
        if (avcodec_parameters_to_context(m_codecContext.get(), m_stream->codecpar) < 0 ||
            avcodec_open2(m_codecContext.get(), codec, nullptr) < 0) {
            if (errorOut) {
                *errorOut = "failed to initialize decoder context";
            }
            return false;
        }

        m_packet.reset(av_packet_alloc());
        m_frame.reset(av_frame_alloc());
        m_bestFrame.reset(av_frame_alloc());
        if (!m_packet || !m_frame || !m_bestFrame) {
            if (errorOut) {
                *errorOut = "failed to allocate ffmpeg frame buffers";
            }
            return false;
        }

        m_opened = true;
        return true;
    }

    bool decodeScaledFrame(int frameIndex,
                           SizeI outputSize,
                           ImageBuffer* imageOut,
                           std::string* errorOut)
    {
        if (!imageOut) {
            return false;
        }
        if (!m_sequenceFramePaths.empty()) {
            const int sequenceFrameIndex = std::clamp(
                frameIndex,
                0,
                static_cast<int>(m_sequenceFramePaths.size()) - 1);
            if (!m_sequenceFrameSource ||
                sequenceFrameIndex != m_sequenceFrameSourceIndex) {
                m_sequenceFrameSource = std::make_unique<MediaSource>(
                    m_sequenceFramePaths[static_cast<std::size_t>(
                        sequenceFrameIndex)].string());
                m_sequenceFrameSourceIndex = sequenceFrameIndex;
            }
            return m_sequenceFrameSource->decodeScaledFrame(
                0, outputSize, imageOut, errorOut);
        }
        if (!open(errorOut)) {
            return false;
        }

        if (m_lastDecodedFrameIndex < 0 || frameIndex < m_lastDecodedFrameIndex) {
            const std::int64_t targetPts = ptsForFrameIndex(frameIndex, m_stream);
            av_seek_frame(m_formatContext.get(), m_videoStreamIndex, targetPts, AVSEEK_FLAG_BACKWARD);
            avcodec_flush_buffers(m_codecContext.get());
            m_lastDecodedFrameIndex = -1;
            av_frame_unref(m_bestFrame.get());
            m_haveBestFrame = false;
        }

        while (!m_haveBestFrame || m_lastDecodedFrameIndex < frameIndex) {
            if (!readNextFrame(errorOut)) {
                break;
            }
        }

        if (!m_haveBestFrame) {
            if (errorOut) {
                *errorOut = "failed to decode timeline frame";
            }
            return false;
        }

        return scaleFrame(m_bestFrame.get(), outputSize, imageOut, errorOut);
    }

private:
    bool readNextFrame(std::string* errorOut)
    {
        while (av_read_frame(m_formatContext.get(), m_packet.get()) >= 0) {
            if (m_packet->stream_index != m_videoStreamIndex) {
                av_packet_unref(m_packet.get());
                continue;
            }
            const int sendResult = avcodec_send_packet(m_codecContext.get(), m_packet.get());
            av_packet_unref(m_packet.get());
            if (sendResult < 0) {
                continue;
            }
            while (avcodec_receive_frame(m_codecContext.get(), m_frame.get()) >= 0) {
                av_frame_unref(m_bestFrame.get());
                av_frame_ref(m_bestFrame.get(), m_frame.get());
                m_haveBestFrame = true;
                ++m_lastDecodedFrameIndex;
                av_frame_unref(m_frame.get());
                return true;
            }
        }
        if (errorOut && errorOut->empty()) {
            *errorOut = "failed to decode timeline frame";
        }
        return false;
    }

    bool scaleFrame(const AVFrame* sourceFrame,
                    SizeI outputSize,
                    ImageBuffer* imageOut,
                    std::string* errorOut) const
    {
        const double scale = std::min(
            static_cast<double>(outputSize.width) / std::max(1, sourceFrame->width),
            static_cast<double>(outputSize.height) / std::max(1, sourceFrame->height));
        const int targetWidth = std::max(1, static_cast<int>(std::lround(sourceFrame->width * scale)));
        const int targetHeight = std::max(1, static_cast<int>(std::lround(sourceFrame->height * scale)));

        std::unique_ptr<SwsContext, SwsContextDeleter> scaleContext(
            sws_getContext(sourceFrame->width,
                           sourceFrame->height,
                           static_cast<AVPixelFormat>(sourceFrame->format),
                           targetWidth,
                           targetHeight,
                           AV_PIX_FMT_RGBA,
                           SWS_BILINEAR,
                           nullptr,
                           nullptr,
                           nullptr));
        if (!scaleContext) {
            if (errorOut) {
                *errorOut = "failed to create scale context";
            }
            return false;
        }

        ImageBuffer scaled;
        scaled.format = PixelFormat::Rgba8;
        scaled.size = {targetWidth, targetHeight};
        scaled.strideBytes = targetWidth * 4;
        scaled.bytes.resize(static_cast<std::size_t>(scaled.strideBytes * targetHeight));

        std::uint8_t* destinationData[4] = {scaled.bytes.data(), nullptr, nullptr, nullptr};
        int destinationLinesize[4] = {scaled.strideBytes, 0, 0, 0};
        if (sws_scale(scaleContext.get(),
                      sourceFrame->data,
                      sourceFrame->linesize,
                      0,
                      sourceFrame->height,
                      destinationData,
                      destinationLinesize) <= 0) {
            if (errorOut) {
                *errorOut = "failed to scale decoded frame";
            }
            return false;
        }

        *imageOut = std::move(scaled);
        return true;
    }

    std::string m_path;
    std::vector<std::filesystem::path> m_sequenceFramePaths;
    int m_sequenceFrameSourceIndex = -1;
    std::unique_ptr<MediaSource> m_sequenceFrameSource;
    bool m_opened = false;
    int m_videoStreamIndex = -1;
    int m_lastDecodedFrameIndex = -1;
    bool m_haveBestFrame = false;
    AVStream* m_stream = nullptr;
    std::unique_ptr<AVFormatContext, AvFormatContextDeleter> m_formatContext;
    std::unique_ptr<AVCodecContext, AvCodecContextDeleter> m_codecContext;
    std::unique_ptr<AVPacket, AvPacketDeleter> m_packet;
    std::unique_ptr<AVFrame, AvFrameDeleter> m_frame;
    std::unique_ptr<AVFrame, AvFrameDeleter> m_bestFrame;
};

} // namespace

namespace jcut::standalone_render {

StandaloneMediaInfo probeStandaloneMedia(const std::string& path)
{
    StandaloneMediaInfo result;
    if (path.empty()) {
        result.message = "media path is empty";
        return result;
    }

    const ImageSequenceDirectoryInfo sequence =
        probeImageSequenceDirectory(std::filesystem::path(path));
    if (sequence.detected()) {
        result.probed = true;
        result.hasVideo = true;
        result.hasAudio = false;
        result.videoFps = kImageSequenceFramesPerSecond;
        result.durationFrames = sequence.frameCount();
        if (!sequence.framePaths.empty()) {
            const StandaloneMediaInfo firstFrame = probeStandaloneMedia(
                sequence.framePaths.front().string());
            result.frameSize = firstFrame.frameSize;
        }
        result.mediaKind = "video";
        result.message = "image sequence directory probed";
        return result;
    }

    AVFormatContext* rawFormatContext = nullptr;
    if (avformat_open_input(&rawFormatContext, path.c_str(), nullptr, nullptr) < 0) {
        result.message = "failed to open media source";
        return result;
    }
    std::unique_ptr<AVFormatContext, AvFormatContextDeleter> formatContext(
        rawFormatContext);
    if (avformat_find_stream_info(formatContext.get(), nullptr) < 0) {
        result.message = "failed to read media stream info";
        return result;
    }

    AVStream* firstVideoStream = nullptr;
    for (unsigned int index = 0; index < formatContext->nb_streams; ++index) {
        AVStream* stream = formatContext->streams[index];
        if (!stream || !stream->codecpar) {
            continue;
        }
        if (stream->codecpar->codec_type == AVMEDIA_TYPE_VIDEO) {
            result.hasVideo = true;
            if (!firstVideoStream) {
                firstVideoStream = stream;
            }
        } else if (stream->codecpar->codec_type == AVMEDIA_TYPE_AUDIO) {
            result.hasAudio = true;
            if (result.audioStreamIndex < 0) {
                result.audioStreamIndex = static_cast<int>(index);
            }
        }
    }

    result.probed = result.hasVideo || result.hasAudio;
    result.mediaKind = result.hasVideo
        ? "video"
        : (result.hasAudio ? "audio" : "unknown");
    if (firstVideoStream) {
        result.videoFps = streamFps(firstVideoStream);
        if (firstVideoStream->codecpar &&
            firstVideoStream->codecpar->width > 0 &&
            firstVideoStream->codecpar->height > 0) {
            result.frameSize = {
                firstVideoStream->codecpar->width,
                firstVideoStream->codecpar->height};
        }
    }
    if (formatContext->duration > 0) {
        const double seconds = static_cast<double>(formatContext->duration) /
            static_cast<double>(AV_TIME_BASE);
        result.durationFrames = static_cast<std::int64_t>(
            std::llround(seconds * 30.0));
    }
    result.message = result.probed
        ? "media streams probed"
        : "no audio or video streams found";
    return result;
}

std::size_t probeUnknownAudioPresence(EditorDocumentCore* document,
                                      const std::string& rootDirectory)
{
    if (!document) {
        return 0;
    }

    std::unordered_map<std::string, StandaloneMediaInfo> probeByPath;
    std::size_t resolvedClipCount = 0;
    for (EditorClip& clip : document->clips) {
        if (clip.audioPresenceKnown ||
            !mediaKindMayContainAudio(clip.mediaKind, clip.sourcePath)) {
            continue;
        }

        std::filesystem::path resolvedPath(clip.sourcePath);
        if (resolvedPath.is_relative() && !rootDirectory.empty()) {
            resolvedPath = std::filesystem::path(rootDirectory) / resolvedPath;
        }
        const std::string probePath = resolvedPath.lexically_normal().string();
        if (probePath.empty()) {
            continue;
        }

        auto [probeIt, inserted] = probeByPath.try_emplace(probePath);
        if (inserted) {
            probeIt->second = probeStandaloneMedia(probePath);
        }
        const StandaloneMediaInfo& info = probeIt->second;
        if (!info.probed) {
            continue;
        }

        clip.audioPresenceKnown = true;
        clip.hasAudio = info.hasAudio;
        ++resolvedClipCount;
        for (EditorMediaItem& mediaItem : document->mediaItems) {
            if (mediaItem.id == clip.sourcePath) {
                mediaItem.audioPresenceKnown = true;
                mediaItem.hasAudio = info.hasAudio;
            }
        }
    }
    return resolvedClipCount;
}

class TimelineRenderer::Impl {
public:
    TimelineRenderResult renderFrame(const TimelineRenderRequest& request)
    {
        TimelineRenderResult result;
        if (!request.outputSize.valid()) {
            result.message = "invalid preview output size";
            return result;
        }

        ImageBuffer canvas = makeSolidImage(request.outputSize, 12, 14, 18);
        const EditorClip* clip = selectVisualClip(request.document, request.timelineFrame);
        if (!clip) {
            result.success = true;
            result.message = "no active visual clip";
            result.image = std::move(canvas);
            return result;
        }

        result.sourcePath = resolveClipPath(*clip, request.rootDirectory);
        const int localFrame = std::max(
            0,
            static_cast<int>(std::floor(request.timelineFrame)) - clip->startFrame);
        MediaSource& mediaSource = sourceForPath(result.sourcePath);
        ImageBuffer decoded;
        std::string decodeError;
        if (!mediaSource.decodeScaledFrame(localFrame, request.outputSize, &decoded, &decodeError)) {
            result.success = false;
            result.message = decodeError.empty() ? "timeline render failed" : decodeError;
            result.image = std::move(canvas);
            return result;
        }

        const int offsetX = (request.outputSize.width - decoded.size.width) / 2;
        const int offsetY = (request.outputSize.height - decoded.size.height) / 2;
        blitImage(decoded, &canvas, offsetX, offsetY);

        result.success = true;
        result.message = "timeline frame rendered";
        result.image = std::move(canvas);
        return result;
    }

private:
    MediaSource& sourceForPath(const std::string& path)
    {
        auto it = m_sources.find(path);
        if (it == m_sources.end()) {
            it = m_sources.emplace(path, std::make_unique<MediaSource>(path)).first;
        }
        return *it->second;
    }

    std::unordered_map<std::string, std::unique_ptr<MediaSource>> m_sources;
};

TimelineRenderer::TimelineRenderer()
    : m_impl(std::make_unique<Impl>())
{
}

TimelineRenderer::~TimelineRenderer() = default;

TimelineRenderResult TimelineRenderer::renderFrame(const TimelineRenderRequest& request)
{
    return m_impl->renderFrame(request);
}

TimelineRenderResult renderTimelineFrame(const TimelineRenderRequest& request)
{
    TimelineRenderer renderer;
    return renderer.renderFrame(request);
}

} // namespace jcut::standalone_render
