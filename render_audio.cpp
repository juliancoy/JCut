#include "render_internal.h"

#include <QDebug>
#include <cstdlib>

namespace render_detail {

DecodedAudioClip decodeClipAudio(const QString& path) {
    DecodedAudioClip cache;

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

    if (avformat_find_stream_info(formatCtx, nullptr) < 0) {
        avformat_close_input(&formatCtx);
        return cache;
    }

    int audioStreamIndex = -1;
    for (unsigned i = 0; i < formatCtx->nb_streams; ++i) {
        if (formatCtx->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_AUDIO) {
            audioStreamIndex = static_cast<int>(i);
            break;
        }
    }
    if (audioStreamIndex < 0) {
        avformat_close_input(&formatCtx);
        return cache;
    }

    AVStream* stream = formatCtx->streams[audioStreamIndex];
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
    av_opt_set_int(swr, "in_sample_rate", codecCtx->sample_rate, 0);
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

    AVPacket* packet = av_packet_alloc();
    AVFrame* frame = av_frame_alloc();
    QByteArray converted;

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
            const int byteCount = convertedSamples * kRenderAudioChannels * static_cast<int>(sizeof(float));
            converted.append(reinterpret_cast<const char*>(outData), byteCount);
        }
        av_freep(&outData);
    };

    while (av_read_frame(formatCtx, packet) >= 0) {
        if (packet->stream_index != audioStreamIndex) {
            av_packet_unref(packet);
            continue;
        }
        if (avcodec_send_packet(codecCtx, packet) >= 0) {
            while (avcodec_receive_frame(codecCtx, frame) >= 0) {
                appendConverted(frame);
                av_frame_unref(frame);
            }
        }
        av_packet_unref(packet);
    }

    avcodec_send_packet(codecCtx, nullptr);
    while (avcodec_receive_frame(codecCtx, frame) >= 0) {
        appendConverted(frame);
        av_frame_unref(frame);
    }

    const int outSamples = swr_get_out_samples(swr, 0);
    if (outSamples > 0) {
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
                   const QVector<RenderSyncMarker>& renderSyncMarkers,
                   const QHash<QString, DecodedAudioClip>& audioCache,
                   float* output,
                   int frames,
                   int64_t chunkStartSample) {
    std::fill(output, output + frames * kRenderAudioChannels, 0.0f);

    for (const TimelineClip& clip : clips) {
        if (!clipAudioPlaybackEnabled(clip)) {
            continue;
        }
        const auto audioIt = audioCache.constFind(playbackAudioPathForClip(clip));
        if (audioIt == audioCache.constEnd() || !audioIt->valid) {
            continue;
        }
        const DecodedAudioClip& audio = audioIt.value();

        const int64_t clipStartSample = clipTimelineStartSamples(clip);
        const int64_t sourceInSample = clipSourceInSamples(clip);
        const int64_t clipAvailableSamples = (audio.samples.size() / kRenderAudioChannels) - sourceInSample;
        if (clipAvailableSamples <= 0) {
            continue;
        }
        const int64_t clipEndSample =
            clipStartSample + qMin<int64_t>(frameToSamples(clip.durationFrames), clipAvailableSamples);
        const int64_t chunkEndSample = chunkStartSample + frames;
        if (chunkEndSample <= clipStartSample || chunkStartSample >= clipEndSample) {
            continue;
        }

        const int64_t mixStart = qMax<int64_t>(chunkStartSample, clipStartSample);
        const int64_t mixEnd = qMin<int64_t>(chunkEndSample, clipEndSample);
        for (int64_t samplePos = mixStart; samplePos < mixEnd; ++samplePos) {
            const int outFrame = static_cast<int>(samplePos - chunkStartSample);
            const int64_t inFrame =
                sourceSampleForClipAtTimelineSample(clip, samplePos, renderSyncMarkers);
            if (inFrame < 0 || inFrame >= (audio.samples.size() / kRenderAudioChannels)) {
                continue;
            }
            const int outIndex = outFrame * kRenderAudioChannels;
            const int inIndex = static_cast<int>(inFrame * kRenderAudioChannels);
            output[outIndex] = qBound(-1.0f, output[outIndex] + audio.samples[inIndex], 1.0f);
            output[outIndex + 1] = qBound(-1.0f, output[outIndex + 1] + audio.samples[inIndex + 1], 1.0f);
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

    QHash<QString, DecodedAudioClip> audioCache;
    int decodedCount = 0;
    int failedCount = 0;
    for (const TimelineClip& clip : audioClips) {
        const QString audioPath = playbackAudioPathForClip(clip);
        if (audioPath.isEmpty() || audioCache.contains(audioPath)) {
            continue;
        }
        const DecodedAudioClip decoded = decodeClipAudio(audioPath);
        if (decoded.valid) {
            audioCache.insert(audioPath, decoded);
            decodedCount++;
            qDebug() << "Audio export: Successfully decoded audio clip:" << audioPath
                     << "samples:" << decoded.samples.size();
        } else {
            // Log failure but continue - other audio clips might work
            failedCount++;
            qWarning() << "Audio export: Failed to decode audio clip:" << audioPath;
        }
    }

    bool hasDecodedAudio = false;
    for (const TimelineClip& clip : audioClips) {
        const auto audioIt = audioCache.constFind(playbackAudioPathForClip(clip));
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
    state->cache = audioCache;
    state->renderSyncMarkers = request.renderSyncMarkers;
    state->enabled = true;
    return true;
}

bool encodeExportAudio(const QVector<ExportRangeSegment>& exportRanges,
                       const AudioExportState& state,
                       AVFormatContext* formatCtx,
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
    av_opt_set_int(swr, "in_sample_rate", kRenderAudioSampleRate, 0);
    av_opt_set_int(swr, "out_sample_rate", state.codecCtx->sample_rate, 0);
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

    for (const ExportRangeSegment& range : exportRanges) {
        const int64_t exportStart = qMax<int64_t>(0, range.startFrame);
        const int64_t exportEnd = qMax(exportStart, range.endFrame);
        const int64_t segmentTotalSamples = frameToSamples(exportEnd - exportStart + 1);
        int64_t producedSamples = 0;
        while (producedSamples < segmentTotalSamples) {
            const int framesThisChunk =
                static_cast<int>(qMin<int64_t>(mixChunkFrames, segmentTotalSamples - producedSamples));
            const int64_t chunkStartSample = frameToSamples(exportStart) + producedSamples;
            mixAudioChunk(state.clips,
                          state.renderSyncMarkers,
                          state.cache,
                          mixBuffer.data(),
                          framesThisChunk,
                          chunkStartSample);

            const int estimatedOutSamples = swr_get_out_samples(swr, framesThisChunk);
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
                av_frame_free(&audioFrame);
                av_audio_fifo_free(fifo);
                ffmpeg_compat::uninitChannelLayout(&inputLayout);
                swr_free(&swr);
                return false;
            }

            const uint8_t* inputData[1] = {
                reinterpret_cast<const uint8_t*>(mixBuffer.constData())
            };
            const int convertedSamples =
                swr_convert(swr, convertedData, estimatedOutSamples, inputData, framesThisChunk);
            if (convertedSamples < 0) {
                if (errorMessage) {
                    *errorMessage = QStringLiteral("Failed to convert mixed audio for encoding.");
                }
                av_freep(&convertedData[0]);
                av_freep(&convertedData);
                av_frame_free(&audioFrame);
                av_audio_fifo_free(fifo);
                ffmpeg_compat::uninitChannelLayout(&inputLayout);
                swr_free(&swr);
                return false;
            }

            if (av_audio_fifo_realloc(fifo, av_audio_fifo_size(fifo) + convertedSamples) < 0 ||
                av_audio_fifo_write(fifo, reinterpret_cast<void**>(convertedData), convertedSamples) < convertedSamples) {
                if (errorMessage) {
                    *errorMessage = QStringLiteral("Failed to queue mixed audio for encoding.");
                }
                av_freep(&convertedData[0]);
                av_freep(&convertedData);
                av_frame_free(&audioFrame);
                av_audio_fifo_free(fifo);
                ffmpeg_compat::uninitChannelLayout(&inputLayout);
                swr_free(&swr);
                return false;
            }

            av_freep(&convertedData[0]);
            av_freep(&convertedData);

            if (!writeAvailableAudioFrames(false)) {
                av_frame_free(&audioFrame);
                av_audio_fifo_free(fifo);
                ffmpeg_compat::uninitChannelLayout(&inputLayout);
                swr_free(&swr);
                return false;
            }

            producedSamples += framesThisChunk;
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
