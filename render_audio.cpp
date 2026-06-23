#include "render_internal.h"

#include "audio_source_key.h"
#include "audio_time_stretch.h"
#include "decoder_ffmpeg_utils.h"

#include <QDebug>
#include <cmath>
#include <cstdlib>
#include <limits>
#include <mutex>

namespace render_detail {

AudioTimeStretchRubberBandSettings exportRubberBandSettings()
{
    AudioTimeStretchRubberBandSettings settings;
    settings.engine = editor::rubberBandEnginePreference() == editor::RubberBandEnginePreference::Finer
        ? RubberBandEngineMode::Finer
        : RubberBandEngineMode::Faster;
    switch (editor::rubberBandThreadingPreference()) {
    case editor::RubberBandThreadingPreference::Never:
        settings.threading = RubberBandThreadingMode::Never;
        break;
    case editor::RubberBandThreadingPreference::Always:
        settings.threading = RubberBandThreadingMode::Always;
        break;
    case editor::RubberBandThreadingPreference::Auto:
        settings.threading = RubberBandThreadingMode::Auto;
        break;
    }
    switch (editor::rubberBandWindowPreference()) {
    case editor::RubberBandWindowPreference::Short:
        settings.window = RubberBandWindowMode::Short;
        break;
    case editor::RubberBandWindowPreference::Long:
        settings.window = RubberBandWindowMode::Long;
        break;
    case editor::RubberBandWindowPreference::Standard:
        settings.window = RubberBandWindowMode::Standard;
        break;
    }
    switch (editor::rubberBandPitchPreference()) {
    case editor::RubberBandPitchPreference::HighQuality:
        settings.pitch = RubberBandPitchMode::HighQuality;
        break;
    case editor::RubberBandPitchPreference::HighConsistency:
        settings.pitch = RubberBandPitchMode::HighConsistency;
        break;
    case editor::RubberBandPitchPreference::HighSpeed:
        settings.pitch = RubberBandPitchMode::HighSpeed;
        break;
    }
    settings.channelsTogether = editor::rubberBandChannelsTogether();
    return settings;
}

namespace {

inline constexpr int64_t kExportAudioDecodeMarginSamples = kRenderAudioSampleRate;

QString audioCacheKeyForClip(const TimelineClip& clip)
{
    return editor::audio::makeSourceKey(playbackAudioPathForClip(clip), clip.audioStreamIndex);
}

bool anyAudioSolo(const QVector<TimelineClip>& clips, const QVector<TimelineTrack>& tracks)
{
    for (const TimelineClip& clip : clips) {
        if (clipAudioPlaybackEnabled(clip) && clip.audioSolo) {
            return true;
        }
    }
    for (const TimelineTrack& track : tracks) {
        if (track.audioSolo) {
            return true;
        }
    }
    return false;
}

float mixerGainForClip(const TimelineClip& clip,
                       const QVector<TimelineTrack>& tracks,
                       bool soloActive)
{
    if (!clipAudioPlaybackEnabled(clip)) {
        return 0.0f;
    }
    float gain = static_cast<float>(qBound<qreal>(0.0, clip.audioGain, 4.0));
    bool clipOrTrackSolo = clip.audioSolo;
    if (clip.trackIndex >= 0 && clip.trackIndex < tracks.size()) {
        const TimelineTrack& track = tracks.at(clip.trackIndex);
        if (!track.audioEnabled || track.audioMuted) {
            return 0.0f;
        }
        gain *= static_cast<float>(qBound<qreal>(0.0, track.audioGain, 4.0));
        clipOrTrackSolo = clipOrTrackSolo || track.audioSolo;
    }
    if (soloActive && !clipOrTrackSolo) {
        return 0.0f;
    }
    return gain;
}

struct ExportAudioDecodeRange {
    int64_t startSample = 0;
    int64_t endSampleExclusive = 0;
    bool bounded = false;

    int64_t frameCount() const {
        return bounded ? qMax<int64_t>(1, endSampleExclusive - startSample) : -1;
    }
};

QVector<ExportRangeSegment> normalizedAudioExportRanges(const RenderRequest& request)
{
    QVector<ExportRangeSegment> ranges = request.exportRanges;
    if (ranges.isEmpty()) {
        const int64_t start = qMax<int64_t>(0, request.exportStartFrame);
        ranges.push_back(ExportRangeSegment{start, qMax<int64_t>(start, request.exportEndFrame)});
    }
    std::sort(ranges.begin(), ranges.end(), [](const ExportRangeSegment& a, const ExportRangeSegment& b) {
        if (a.startFrame == b.startFrame) {
            return a.endFrame < b.endFrame;
        }
        return a.startFrame < b.startFrame;
    });
    return ranges;
}

ExportAudioDecodeRange audioDecodeRangeForPath(const QString& path,
                                               const QVector<TimelineClip>& clips,
                                               const QVector<ExportRangeSegment>& exportRanges,
                                               const QVector<RenderSyncMarker>& markers)
{
    ExportAudioDecodeRange result;
    int64_t minSourceSample = std::numeric_limits<int64_t>::max();
    int64_t maxSourceSample = 0;

    for (const TimelineClip& clip : clips) {
        if (playbackAudioPathForClip(clip) != path) {
            continue;
        }
        const int64_t clipStartSample = clipTimelineStartSamples(clip);
        const int64_t clipEndSampleExclusive = clipTimelineEndSamples(clip);
        for (const ExportRangeSegment& range : exportRanges) {
            const int64_t rangeStartSample = frameToSamples(qMax<int64_t>(0, range.startFrame));
            const int64_t rangeEndSampleExclusive =
                frameToSamples(qMax<int64_t>(range.startFrame, range.endFrame) + 1);
            const int64_t overlapStart = qMax<int64_t>(clipStartSample, rangeStartSample);
            const int64_t overlapEndExclusive = qMin<int64_t>(clipEndSampleExclusive, rangeEndSampleExclusive);
            if (overlapEndExclusive <= overlapStart) {
                continue;
            }

            const int64_t sourceAtStart =
                sourceSampleForClipAtTimelineSample(clip, overlapStart, markers);
            const int64_t sourceAtEnd =
                sourceSampleForClipAtTimelineSample(clip, qMax<int64_t>(overlapStart, overlapEndExclusive - 1), markers);
            minSourceSample = qMin<int64_t>(minSourceSample, qMin(sourceAtStart, sourceAtEnd));
            maxSourceSample = qMax<int64_t>(maxSourceSample, qMax(sourceAtStart, sourceAtEnd) + 1);
        }
    }

    if (minSourceSample == std::numeric_limits<int64_t>::max() || maxSourceSample <= minSourceSample) {
        return result;
    }

    result.startSample = qMax<int64_t>(0, minSourceSample - kExportAudioDecodeMarginSamples);
    result.endSampleExclusive = qMax<int64_t>(result.startSample + 1,
                                              maxSourceSample + kExportAudioDecodeMarginSamples);
    result.bounded = true;
    return result;
}

} // namespace

DecodedAudioClip decodeClipAudio(const QString& path,
                                 int64_t sourceStartSample,
                                 int64_t maxOutputFrames,
                                 int audioStreamIndex) {
    DecodedAudioClip cache;
    const int64_t requestedSourceStartSample = qMax<int64_t>(0, sourceStartSample);

    // Suppress ALSA/PulseAudio errors by setting environment variables
    // These errors occur when FFmpeg tries to access audio devices during decode
    static bool alsaSuppressed = false;
    if (!alsaSuppressed) {
        // Set environment variables to suppress ALSA device errors
        qputenv("ALSA_CONFIG_PATH", "/dev/null");
        qputenv("PULSE_SERVER", "none");
        qputenv("SDL_AUDIODRIVER", "dummy");
        alsaSuppressed = true;
    }

    AVFormatContext* formatCtx = nullptr;
    if (avformat_open_input(&formatCtx, QFile::encodeName(path).constData(), nullptr, nullptr) < 0) {
        return cache;
    }

    int streamInfoRet = 0;
    {
        std::unique_lock<std::mutex> decodeLock(editor::ffmpegDecodeMutex());
        streamInfoRet = avformat_find_stream_info(formatCtx, nullptr);
    }
    if (streamInfoRet < 0) {
        avformat_close_input(&formatCtx);
        return cache;
    }

    int resolvedAudioStreamIndex = -1;
    if (audioStreamIndex >= 0 &&
        audioStreamIndex < static_cast<int>(formatCtx->nb_streams) &&
        formatCtx->streams[audioStreamIndex]->codecpar->codec_type == AVMEDIA_TYPE_AUDIO) {
        resolvedAudioStreamIndex = audioStreamIndex;
    } else {
        for (unsigned i = 0; i < formatCtx->nb_streams; ++i) {
            if (formatCtx->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_AUDIO) {
                resolvedAudioStreamIndex = static_cast<int>(i);
                break;
            }
        }
    }
    if (resolvedAudioStreamIndex < 0) {
        avformat_close_input(&formatCtx);
        return cache;
    }

    AVStream* stream = formatCtx->streams[resolvedAudioStreamIndex];
    if (!stream || !stream->codecpar) {
        avformat_close_input(&formatCtx);
        return cache;
    }
    
    const AVCodec* decoder = avcodec_find_decoder(stream->codecpar->codec_id);
    if (!decoder) {
        avformat_close_input(&formatCtx);
        return cache;
    }

    AVCodecContext* codecCtx = avcodec_alloc_context3(decoder);
    if (!codecCtx) {
        avformat_close_input(&formatCtx);
        return cache;
    }

    if (avcodec_parameters_to_context(codecCtx, stream->codecpar) < 0 ||
        avcodec_open2(codecCtx, decoder, nullptr) < 0) {
        avcodec_free_context(&codecCtx);
        avformat_close_input(&formatCtx);
        return cache;
    }

    // Validate sample_rate: on some platforms (macOS/VideoToolbox),
    // avcodec_parameters_to_context may leave sample_rate at 0 even
    // when stream->codecpar->sample_rate is valid.
    const int inSampleRate = codecCtx->sample_rate > 0
        ? codecCtx->sample_rate
        : (stream->codecpar->sample_rate > 0 ? stream->codecpar->sample_rate : 48000);
    SwrContext* swr = swr_alloc();
    if (!swr) {
        avcodec_free_context(&codecCtx);
        avformat_close_input(&formatCtx);
        return cache;
    }
    ffmpeg_compat::ChannelLayoutHandle outLayout{};
    ffmpeg_compat::defaultChannelLayout(&outLayout, kRenderAudioChannels);
    ffmpeg_compat::setSwrInputLayout(swr, codecCtx);
    ffmpeg_compat::setSwrOutputLayout(swr, &outLayout);
    av_opt_set_int(swr, "in_sample_rate", inSampleRate, 0);
    av_opt_set_int(swr, "out_sample_rate", kRenderAudioSampleRate, 0);
    av_opt_set_sample_fmt(swr, "in_sample_fmt", codecCtx->sample_fmt, 0);
    av_opt_set_sample_fmt(swr, "out_sample_fmt", AV_SAMPLE_FMT_FLT, 0);
    if (swr_init(swr) < 0) {
        ffmpeg_compat::uninitChannelLayout(&outLayout);
        swr_free(&swr);
        avcodec_free_context(&codecCtx);
        avformat_close_input(&formatCtx);
        return cache;
    }

    if (requestedSourceStartSample > 0) {
        const AVRational outputSampleTimeBase{1, kRenderAudioSampleRate};
        const int64_t seekTimestamp = av_rescale_q(
            requestedSourceStartSample, outputSampleTimeBase, stream->time_base);
        if (av_seek_frame(formatCtx, resolvedAudioStreamIndex, seekTimestamp, AVSEEK_FLAG_BACKWARD) >= 0) {
            avcodec_flush_buffers(codecCtx);
        }
    }

    AVPacket* packet = av_packet_alloc();
    AVFrame* frame = av_frame_alloc();
    QByteArray converted;
    const bool limitedDecode = maxOutputFrames > 0;
    const int64_t maxOutputSamples =
        limitedDecode ? qMax<int64_t>(1, maxOutputFrames * kRenderAudioChannels) : -1;
    bool reachedEof = false;
    bool hitOutputLimit = false;
    int64_t firstOutputSourceSample = -1;
    int64_t nextUnknownOutputSourceSample = requestedSourceStartSample;

    auto appendConverted = [&](AVFrame* decoded) {
        const int outSamples = swr_get_out_samples(swr, decoded->nb_samples);
        if (outSamples <= 0) {
            return;
        }
        uint8_t* outData = nullptr;
        int outLineSize = 0;
        if (av_samples_alloc(&outData, &outLineSize, kRenderAudioChannels, outSamples, AV_SAMPLE_FMT_FLT, 0) < 0) {
            return;
        }
        const int convertedSamples = swr_convert(swr, &outData, outSamples,
                                                 const_cast<const uint8_t**>(decoded->extended_data),
                                                 decoded->nb_samples);
        if (convertedSamples > 0) {
            int64_t frameStartSourceSample = nextUnknownOutputSourceSample;
            const int64_t bestTimestamp = decoded->best_effort_timestamp;
            if (bestTimestamp != AV_NOPTS_VALUE) {
                frameStartSourceSample = av_rescale_q(bestTimestamp,
                                                      stream->time_base,
                                                      AVRational{1, kRenderAudioSampleRate});
            }
            int skipFrames = 0;
            if (requestedSourceStartSample > 0) {
                const int64_t frameEndSourceSample =
                    frameStartSourceSample + static_cast<int64_t>(convertedSamples);
                if (frameEndSourceSample <= requestedSourceStartSample) {
                    nextUnknownOutputSourceSample = frameEndSourceSample;
                    av_freep(&outData);
                    return;
                }
                if (frameStartSourceSample < requestedSourceStartSample) {
                    skipFrames = static_cast<int>(
                        qMin<int64_t>(convertedSamples, requestedSourceStartSample - frameStartSourceSample));
                }
            }
            const int retainedFrames = convertedSamples - skipFrames;
            if (retainedFrames <= 0) {
                nextUnknownOutputSourceSample =
                    frameStartSourceSample + static_cast<int64_t>(convertedSamples);
                av_freep(&outData);
                return;
            }
            if (firstOutputSourceSample < 0) {
                firstOutputSourceSample = frameStartSourceSample + skipFrames;
            }
            const int byteOffset = skipFrames * kRenderAudioChannels * static_cast<int>(sizeof(float));
            const int byteCount = retainedFrames * kRenderAudioChannels * static_cast<int>(sizeof(float));
            converted.append(reinterpret_cast<const char*>(outData) + byteOffset, byteCount);
            nextUnknownOutputSourceSample =
                frameStartSourceSample + static_cast<int64_t>(convertedSamples);
            if (maxOutputSamples > 0) {
                const int64_t currentSamples =
                    static_cast<int64_t>(converted.size() / static_cast<int>(sizeof(float)));
                if (currentSamples >= maxOutputSamples) {
                    converted.truncate(static_cast<int>(maxOutputSamples * static_cast<int64_t>(sizeof(float))));
                    hitOutputLimit = true;
                }
            }
        }
        av_freep(&outData);
    };

    while (!hitOutputLimit && av_read_frame(formatCtx, packet) >= 0) {
        if (packet->stream_index != resolvedAudioStreamIndex) {
            av_packet_unref(packet);
            continue;
        }
        if (avcodec_send_packet(codecCtx, packet) >= 0) {
            while (avcodec_receive_frame(codecCtx, frame) >= 0) {
                appendConverted(frame);
                av_frame_unref(frame);
                if (hitOutputLimit) {
                    break;
                }
            }
        }
        av_packet_unref(packet);
    }

    if (!hitOutputLimit) {
        reachedEof = true;
        avcodec_send_packet(codecCtx, nullptr);
        while (avcodec_receive_frame(codecCtx, frame) >= 0) {
            appendConverted(frame);
            av_frame_unref(frame);
        }
    }

    const int outSamples = hitOutputLimit ? 0 : swr_get_out_samples(swr, 0);
    if (outSamples > 0 && !hitOutputLimit) {
        uint8_t* outData = nullptr;
        int outLineSize = 0;
        if (av_samples_alloc(&outData, &outLineSize, kRenderAudioChannels, outSamples, AV_SAMPLE_FMT_FLT, 0) >= 0) {
            const int flushed = swr_convert(swr, &outData, outSamples, nullptr, 0);
            if (flushed > 0) {
                converted.append(reinterpret_cast<const char*>(outData),
                                 flushed * kRenderAudioChannels * static_cast<int>(sizeof(float)));
            }
            av_freep(&outData);
        }
    }

    const int sampleCount = converted.size() / static_cast<int>(sizeof(float));
    cache.samples.resize(sampleCount);
    if (sampleCount > 0) {
        std::memcpy(cache.samples.data(), converted.constData(), converted.size());
        cache.sourceStartSample = firstOutputSourceSample >= 0
            ? firstOutputSourceSample
            : requestedSourceStartSample;
        cache.fullyDecoded = requestedSourceStartSample == 0 && (!limitedDecode || reachedEof);
        cache.valid = true;
    }

    av_frame_free(&frame);
    av_packet_free(&packet);
    ffmpeg_compat::uninitChannelLayout(&outLayout);
    swr_free(&swr);
    avcodec_free_context(&codecCtx);
    avformat_close_input(&formatCtx);
    return cache;
}

void mixAudioChunk(const QVector<TimelineClip>& clips,
                   const QVector<TimelineTrack>& tracks,
                   const QVector<RenderSyncMarker>& renderSyncMarkers,
                   const QHash<QString, DecodedAudioClip>& audioCache,
                   float* output,
                   int frames,
                   int64_t chunkStartSample,
                   qreal timelineSampleStep) {
    std::fill(output, output + frames * kRenderAudioChannels, 0.0f);
    const qreal sampleStep = std::isfinite(timelineSampleStep) && timelineSampleStep > 0.001
        ? timelineSampleStep
        : 1.0;

    const bool soloActive = anyAudioSolo(clips, tracks);
    for (const TimelineClip& clip : clips) {
        const float mixerGain = mixerGainForClip(clip, tracks, soloActive);
        if (mixerGain <= 0.0f) {
            continue;
        }
        const auto audioIt = audioCache.constFind(audioCacheKeyForClip(clip));
        if (audioIt == audioCache.constEnd() || !audioIt->valid) {
            continue;
        }
        const DecodedAudioClip& audio = audioIt.value();

        const int64_t clipStartSample = clipTimelineStartSamples(clip);
        const int64_t decodedFrameCount = audio.samples.size() / kRenderAudioChannels;
        if (decodedFrameCount <= 0) {
            continue;
        }
        const int64_t clipEndSample = clipTimelineEndSamples(clip);
        const int64_t chunkEndSample =
            chunkStartSample + static_cast<int64_t>(std::ceil(static_cast<qreal>(frames) * sampleStep));
        if (chunkEndSample <= clipStartSample || chunkStartSample >= clipEndSample) {
            continue;
        }

        for (int outFrame = 0; outFrame < frames; ++outFrame) {
            const int64_t samplePos =
                chunkStartSample + static_cast<int64_t>(std::floor(static_cast<qreal>(outFrame) * sampleStep));
            if (samplePos < clipStartSample || samplePos >= clipEndSample) {
                continue;
            }
            const int64_t inFrame =
                sourceSampleForClipAtTimelineSample(clip, samplePos, renderSyncMarkers);
            const int64_t localInFrame = inFrame - audio.sourceStartSample;
            if (localInFrame < 0 || localInFrame >= decodedFrameCount) {
                continue;
            }
            const int outIndex = outFrame * kRenderAudioChannels;
            const int inIndex = static_cast<int>(localInFrame * kRenderAudioChannels);
            output[outIndex] = qBound(-1.0f, output[outIndex] + audio.samples[inIndex] * mixerGain, 1.0f);
            output[outIndex + 1] = qBound(-1.0f, output[outIndex + 1] + audio.samples[inIndex + 1] * mixerGain, 1.0f);
        }
    }
}

bool initializeExportAudio(const RenderRequest& request,
                           AVFormatContext* formatCtx,
                           AudioExportState* state,
                           QString* errorMessage) {
    if (!state) {
        return true;
    }

    QVector<TimelineClip> audioClips;
    for (const TimelineClip& clip : request.clips) {
        if (clipAudioPlaybackEnabled(clip)) {
            audioClips.push_back(clip);
        }
    }
    if (audioClips.isEmpty()) {
        // No audio clips to export - this is normal for video-only projects
        qDebug() << "Audio export: No audio clips found (video-only export)";
        return true;
    }

    qDebug() << "Audio export: Found" << audioClips.size() << "audio clip(s)";

    const QVector<ExportRangeSegment> exportRanges = normalizedAudioExportRanges(request);
    QHash<QString, DecodedAudioClip> audioCache;
    int decodedCount = 0;
    int failedCount = 0;
    for (const TimelineClip& clip : audioClips) {
        const QString audioPath = playbackAudioPathForClip(clip);
        const QString audioKey = audioCacheKeyForClip(clip);
        if (audioPath.isEmpty() || audioKey.isEmpty() || audioCache.contains(audioKey)) {
            continue;
        }
        const ExportAudioDecodeRange range =
            audioDecodeRangeForPath(audioPath, audioClips, exportRanges, request.renderSyncMarkers);
        const DecodedAudioClip decoded = range.bounded
            ? decodeClipAudio(audioPath, range.startSample, range.frameCount(), editor::audio::streamIndexFromSourceKey(audioKey))
            : decodeClipAudio(audioPath, 0, -1, editor::audio::streamIndexFromSourceKey(audioKey));
        if (decoded.valid) {
            audioCache.insert(audioKey, decoded);
            decodedCount++;
            qDebug() << "Audio export: Successfully decoded audio clip:" << audioKey
                     << "sourceStartSample:" << decoded.sourceStartSample
                     << "frames:" << (decoded.samples.size() / kRenderAudioChannels)
                     << "bounded:" << range.bounded;
        } else {
            // Log failure but continue - other audio clips might work
            failedCount++;
            qWarning() << "Audio export: Failed to decode audio clip:" << audioPath;
        }
    }

    bool hasDecodedAudio = false;
    for (const TimelineClip& clip : audioClips) {
        const auto audioIt = audioCache.constFind(audioCacheKeyForClip(clip));
        if (audioIt != audioCache.constEnd() && audioIt->valid) {
            hasDecodedAudio = true;
            break;
        }
    }
    if (!hasDecodedAudio) {
        // All audio clips failed to decode - export will proceed without audio
        // This matches the audio engine behavior which also silently ignores
        // clips that fail to decode
        qWarning() << "Audio export: All" << audioClips.size() << "audio clip(s) failed to decode";
        return true;
    }
    
    // At least one audio clip decoded successfully
    qDebug() << "Audio export:" << decodedCount << "clip(s) decoded successfully," << failedCount << "failed";

    QString codecLabel;
    const AVCodec* audioCodec = audioCodecForRequest(request.outputFormat, &codecLabel);
    if (!audioCodec) {
        if (errorMessage) {
            *errorMessage = QStringLiteral("No audio encoder available for format %1.").arg(request.outputFormat);
        }
        return false;
    }

    state->stream = avformat_new_stream(formatCtx, nullptr);
    if (!state->stream) {
        if (errorMessage) {
            *errorMessage = QStringLiteral("Failed to create output audio stream.");
        }
        return false;
    }

    state->codecCtx = avcodec_alloc_context3(audioCodec);
    if (!state->codecCtx) {
        if (errorMessage) {
            *errorMessage = QStringLiteral("Failed to allocate audio encoder context.");
        }
        return false;
    }

    const int sampleRate = audioSampleRateForCodec(audioCodec);
    const AVSampleFormat sampleFormat = audioSampleFormatForCodec(audioCodec);
#if LIBAVUTIL_VERSION_MAJOR >= 57
    av_channel_layout_default(&state->codecCtx->ch_layout, kRenderAudioChannels);
#else
    state->codecCtx->channel_layout = static_cast<uint64_t>(av_get_default_channel_layout(kRenderAudioChannels));
    state->codecCtx->channels = kRenderAudioChannels;
#endif
    state->codecCtx->codec_id = audioCodec->id;
    state->codecCtx->codec_type = AVMEDIA_TYPE_AUDIO;
    state->codecCtx->sample_rate = sampleRate;
    state->codecCtx->sample_fmt = sampleFormat;
    state->codecCtx->time_base = AVRational{1, sampleRate};
    state->codecCtx->bit_rate = 192000;
    if (formatCtx->oformat->flags & AVFMT_GLOBALHEADER) {
        state->codecCtx->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;
    }
    configureAudioCodecOptions(state->codecCtx, request.outputFormat);

    if (avcodec_open2(state->codecCtx, audioCodec, nullptr) < 0) {
        if (errorMessage) {
            *errorMessage = QStringLiteral("Failed to open audio encoder %1.").arg(codecLabel);
        }
        avcodec_free_context(&state->codecCtx);
        return false;
    }

    if (avcodec_parameters_from_context(state->stream->codecpar, state->codecCtx) < 0) {
        if (errorMessage) {
            *errorMessage = QStringLiteral("Failed to copy audio encoder parameters.");
        }
        avcodec_free_context(&state->codecCtx);
        return false;
    }
    state->stream->time_base = state->codecCtx->time_base;
    state->clips = audioClips;
    state->tracks = request.tracks;
    state->cache = audioCache;
    state->renderSyncMarkers = request.renderSyncMarkers;
    state->enabled = true;
    return true;
}

bool encodeExportAudio(const QVector<ExportRangeSegment>& exportRanges,
                       const AudioExportState& state,
                       AVFormatContext* formatCtx,
                       qreal playbackSpeed,
                       QString* errorMessage) {
    if (!state.enabled || !state.codecCtx || !state.stream) {
        return true;
    }

    SwrContext* swr = swr_alloc();
    if (!swr) {
        if (errorMessage) {
            *errorMessage = QStringLiteral("Failed to allocate audio resampler for export.");
        }
        return false;
    }
    ffmpeg_compat::ChannelLayoutHandle inputLayout{};
    ffmpeg_compat::defaultChannelLayout(&inputLayout, kRenderAudioChannels);
#if LIBAVUTIL_VERSION_MAJOR >= 57
    av_opt_set_chlayout(swr, "in_chlayout", &inputLayout, 0);
    av_opt_set_chlayout(swr, "out_chlayout", &state.codecCtx->ch_layout, 0);
#else
    av_opt_set_int(swr, "in_channel_layout", static_cast<int64_t>(inputLayout), 0);
    av_opt_set_int(swr, "out_channel_layout",
                   static_cast<int64_t>(ffmpeg_compat::channelLayoutForCodecContext(state.codecCtx)), 0);
#endif
    // Validate sample_rate: the encoder context should have a valid rate
    // set during initialization, but guard against 0 just in case.
    const int outSampleRate = state.codecCtx->sample_rate > 0
        ? state.codecCtx->sample_rate
        : kRenderAudioSampleRate;
    av_opt_set_int(swr, "in_sample_rate", kRenderAudioSampleRate, 0);
    av_opt_set_int(swr, "out_sample_rate", outSampleRate, 0);
    av_opt_set_sample_fmt(swr, "in_sample_fmt", AV_SAMPLE_FMT_FLT, 0);
    av_opt_set_sample_fmt(swr, "out_sample_fmt", state.codecCtx->sample_fmt, 0);
    if (swr_init(swr) < 0) {
        if (errorMessage) {
            *errorMessage = QStringLiteral("Failed to create audio resampler for export.");
        }
        ffmpeg_compat::uninitChannelLayout(&inputLayout);
        swr_free(&swr);
        return false;
    }

    AVAudioFifo* fifo = av_audio_fifo_alloc(state.codecCtx->sample_fmt,
                                            ffmpeg_compat::codecContextChannelCount(state.codecCtx),
                                            qMax(1, state.codecCtx->frame_size * 2));
    if (!fifo) {
        if (errorMessage) {
            *errorMessage = QStringLiteral("Failed to allocate audio FIFO for export.");
        }
        ffmpeg_compat::uninitChannelLayout(&inputLayout);
        swr_free(&swr);
        return false;
    }

    AVFrame* audioFrame = av_frame_alloc();
    if (!audioFrame) {
        if (errorMessage) {
            *errorMessage = QStringLiteral("Failed to allocate audio frame.");
        }
        av_audio_fifo_free(fifo);
        ffmpeg_compat::uninitChannelLayout(&inputLayout);
        swr_free(&swr);
        return false;
    }

    const int mixChunkFrames = 1024;
    QVector<float> mixBuffer(mixChunkFrames * kRenderAudioChannels);
    int64_t audioPts = 0;
    const qreal speed = std::isfinite(playbackSpeed) && playbackSpeed > 0.001
        ? playbackSpeed
        : 1.0;

    auto writeAvailableAudioFrames = [&](bool flushTail) -> bool {
        const int encoderFrameSamples = state.codecCtx->frame_size > 0 ? state.codecCtx->frame_size : 1024;
        while (av_audio_fifo_size(fifo) >= encoderFrameSamples ||
               (flushTail && av_audio_fifo_size(fifo) > 0)) {
            const int frameSamples = flushTail
                ? qMin(av_audio_fifo_size(fifo), encoderFrameSamples)
                : encoderFrameSamples;
            audioFrame->nb_samples = frameSamples;
            audioFrame->format = state.codecCtx->sample_fmt;
            audioFrame->sample_rate = state.codecCtx->sample_rate;
            ffmpeg_compat::copyFrameChannelLayout(audioFrame, state.codecCtx);
            if (av_frame_get_buffer(audioFrame, 0) < 0) {
                if (errorMessage) {
                    *errorMessage = QStringLiteral("Failed to allocate encoded audio frame buffer.");
                }
                return false;
            }
            if (av_frame_make_writable(audioFrame) < 0) {
                if (errorMessage) {
                    *errorMessage = QStringLiteral("Failed to make encoded audio frame writable.");
                }
                return false;
            }
            if (av_audio_fifo_read(fifo, reinterpret_cast<void**>(audioFrame->data), frameSamples) < frameSamples) {
                if (errorMessage) {
                    *errorMessage = QStringLiteral("Failed to read mixed audio from FIFO.");
                }
                return false;
            }
            audioFrame->pts = audioPts;
            audioPts += frameSamples;
            if (!encodeFrame(state.codecCtx, state.stream, formatCtx, audioFrame, errorMessage)) {
                return false;
            }
            av_frame_unref(audioFrame);
#if LIBAVUTIL_VERSION_MAJOR >= 57
            av_channel_layout_uninit(&audioFrame->ch_layout);
#endif
        }
        return true;
    };

    auto queueFloatAudioForEncode = [&](const float* samples, int frameCount) -> bool {
        if (!samples || frameCount <= 0) {
            return true;
        }
        const int estimatedOutSamples = swr_get_out_samples(swr, frameCount);
        uint8_t** convertedData = nullptr;
        int convertedLineSize = 0;
        if (av_samples_alloc_array_and_samples(&convertedData,
                                               &convertedLineSize,
                                               ffmpeg_compat::codecContextChannelCount(state.codecCtx),
                                               estimatedOutSamples,
                                               state.codecCtx->sample_fmt,
                                               0) < 0) {
            if (errorMessage) {
                *errorMessage = QStringLiteral("Failed to allocate converted audio buffer.");
            }
            return false;
        }

        const uint8_t* inputData[1] = {
            reinterpret_cast<const uint8_t*>(samples)
        };
        const int convertedSamples =
            swr_convert(swr, convertedData, estimatedOutSamples, inputData, frameCount);
        if (convertedSamples < 0) {
            if (errorMessage) {
                *errorMessage = QStringLiteral("Failed to convert mixed audio for encoding.");
            }
            av_freep(&convertedData[0]);
            av_freep(&convertedData);
            return false;
        }

        if (av_audio_fifo_realloc(fifo, av_audio_fifo_size(fifo) + convertedSamples) < 0 ||
            av_audio_fifo_write(fifo, reinterpret_cast<void**>(convertedData), convertedSamples) < convertedSamples) {
            if (errorMessage) {
                *errorMessage = QStringLiteral("Failed to queue mixed audio for encoding.");
            }
            av_freep(&convertedData[0]);
            av_freep(&convertedData);
            return false;
        }

        av_freep(&convertedData[0]);
        av_freep(&convertedData);
        return writeAvailableAudioFrames(false);
    };

    for (const ExportRangeSegment& range : exportRanges) {
        const int64_t exportStart = qMax<int64_t>(0, range.startFrame);
        const int64_t exportEnd = qMax(exportStart, range.endFrame);
        const int64_t sourceSegmentSamples = frameToSamples(exportEnd - exportStart + 1);
        if (sourceSegmentSamples <= 0 ||
            sourceSegmentSamples > std::numeric_limits<int>::max() / kRenderAudioChannels) {
            if (errorMessage) {
                *errorMessage = QStringLiteral("Export audio segment is too large to pitch-preserve in memory.");
            }
            av_frame_free(&audioFrame);
            av_audio_fifo_free(fifo);
            ffmpeg_compat::uninitChannelLayout(&inputLayout);
            swr_free(&swr);
            return false;
        }

        QVector<float> sourceMix(static_cast<int>(sourceSegmentSamples) * kRenderAudioChannels);
        int64_t mixedSamples = 0;
        while (mixedSamples < sourceSegmentSamples) {
            const int framesThisChunk =
                static_cast<int>(qMin<int64_t>(mixChunkFrames, sourceSegmentSamples - mixedSamples));
            const int64_t chunkStartSample = frameToSamples(exportStart) + mixedSamples;
            mixAudioChunk(state.clips,
                          state.tracks,
                          state.renderSyncMarkers,
                          state.cache,
                          mixBuffer.data(),
                          framesThisChunk,
                          chunkStartSample,
                          1.0);
            std::memcpy(sourceMix.data() + (mixedSamples * kRenderAudioChannels),
                        mixBuffer.constData(),
                        static_cast<size_t>(framesThisChunk * kRenderAudioChannels) * sizeof(float));
            mixedSamples += framesThisChunk;
        }

        QVector<float> exportMix = sourceMix;
        if (qAbs(speed - 1.0) >= 0.0001) {
            exportMix = timeStretchPreservePitch(sourceMix,
                                                 kRenderAudioChannels,
                                                 kRenderAudioSampleRate,
                                                 speed,
                                                 AudioTimeStretchBackend::RubberBand,
                                                 nullptr,
                                                 exportRubberBandSettings());
            if (exportMix.isEmpty()) {
                if (errorMessage) {
                    *errorMessage = QStringLiteral(
                        "Rubber Band pitch-preserving audio stretch failed for export speed %1.")
                        .arg(speed, 0, 'f', 3);
                }
                av_frame_free(&audioFrame);
                av_audio_fifo_free(fifo);
                ffmpeg_compat::uninitChannelLayout(&inputLayout);
                swr_free(&swr);
                return false;
            }
        }

        int64_t queuedFrames = 0;
        const int64_t exportFrames = exportMix.size() / kRenderAudioChannels;
        while (queuedFrames < exportFrames) {
            const int framesThisChunk =
                static_cast<int>(qMin<int64_t>(mixChunkFrames, exportFrames - queuedFrames));
            if (!queueFloatAudioForEncode(exportMix.constData() + (queuedFrames * kRenderAudioChannels),
                                          framesThisChunk)) {
                av_frame_free(&audioFrame);
                av_audio_fifo_free(fifo);
                ffmpeg_compat::uninitChannelLayout(&inputLayout);
                swr_free(&swr);
                return false;
            }
            queuedFrames += framesThisChunk;
        }
    }

    while (swr_get_delay(swr, kRenderAudioSampleRate) > 0) {
        const int delayedInputSamples = static_cast<int>(swr_get_delay(swr, kRenderAudioSampleRate));
        const int estimatedOutSamples = swr_get_out_samples(swr, delayedInputSamples);
        if (estimatedOutSamples <= 0) {
            break;
        }

        uint8_t** convertedData = nullptr;
        int convertedLineSize = 0;
        if (av_samples_alloc_array_and_samples(&convertedData,
                                               &convertedLineSize,
                                               ffmpeg_compat::codecContextChannelCount(state.codecCtx),
                                               estimatedOutSamples,
                                               state.codecCtx->sample_fmt,
                                               0) < 0) {
            if (errorMessage) {
                *errorMessage = QStringLiteral("Failed to allocate resampler flush buffer.");
            }
            av_frame_free(&audioFrame);
            av_audio_fifo_free(fifo);
            ffmpeg_compat::uninitChannelLayout(&inputLayout);
            swr_free(&swr);
            return false;
        }

        const int convertedSamples = swr_convert(swr, convertedData, estimatedOutSamples, nullptr, 0);
        if (convertedSamples < 0) {
            if (errorMessage) {
                *errorMessage = QStringLiteral("Failed to flush audio resampler.");
            }
            av_freep(&convertedData[0]);
            av_freep(&convertedData);
            av_frame_free(&audioFrame);
            av_audio_fifo_free(fifo);
            ffmpeg_compat::uninitChannelLayout(&inputLayout);
            swr_free(&swr);
            return false;
        }
        if (convertedSamples > 0) {
            if (av_audio_fifo_realloc(fifo, av_audio_fifo_size(fifo) + convertedSamples) < 0 ||
                av_audio_fifo_write(fifo, reinterpret_cast<void**>(convertedData), convertedSamples) < convertedSamples) {
                if (errorMessage) {
                    *errorMessage = QStringLiteral("Failed to queue resampler flush audio for encoding.");
                }
                av_freep(&convertedData[0]);
                av_freep(&convertedData);
                av_frame_free(&audioFrame);
                av_audio_fifo_free(fifo);
                ffmpeg_compat::uninitChannelLayout(&inputLayout);
                swr_free(&swr);
                return false;
            }
        }
        av_freep(&convertedData[0]);
        av_freep(&convertedData);

        if (convertedSamples == 0) {
            break;
        }
    }

    if (!writeAvailableAudioFrames(true) ||
        !encodeFrame(state.codecCtx, state.stream, formatCtx, nullptr, errorMessage)) {
        av_frame_free(&audioFrame);
        av_audio_fifo_free(fifo);
        ffmpeg_compat::uninitChannelLayout(&inputLayout);
        swr_free(&swr);
        return false;
    }

    av_frame_free(&audioFrame);
    av_audio_fifo_free(fifo);
    ffmpeg_compat::uninitChannelLayout(&inputLayout);
    swr_free(&swr);
    return true;
}

} // namespace render_detail
