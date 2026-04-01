#include "decoder_context.h"

#include "async_decoder.h"
#include "debug_controls.h"
#include "decode_trace.h"
#include "decoder_ffmpeg_utils.h"
#include "decoder_image_io.h"

#include <QDateTime>
#include <QDebug>
#include <QFile>
#include <QFileInfo>
#include <QThread>

#include <limits>

namespace editor {

namespace {

constexpr int64_t kMaxSequentialDecodeGap = 90;
constexpr size_t kMaxSequenceFrameCacheBytes = 384 * 1024 * 1024;  // 384MB for 4K WebP
constexpr int kMaxSequenceFrameCacheEntries = 48;  // 48 frames = 1.6s at 30fps
constexpr int kWebpSequenceBatchAhead = 12;  // 12 frames = 400ms lookahead

#if defined(__SANITIZE_ADDRESS__)
constexpr bool kAsanBuild = true;
#else
constexpr bool kAsanBuild = false;
#endif

} // namespace

DecoderContext::DecoderContext(const QString& path,
                               const QHash<int, AVBufferRef*>* sharedHwDevices)
    : m_path(path)
    , m_sharedHwDevices(sharedHwDevices) {}

DecoderContext::~DecoderContext() {
    shutdown();
}

void DecoderContext::shutdown() {
    if (m_codecCtx) {
        avcodec_free_context(&m_codecCtx);
    }
    if (m_formatCtx) {
        avformat_close_input(&m_formatCtx);
    }
    if (m_hwDeviceCtx) {
        // Only release the ref if we created it ourselves.
        // Borrowed refs from the shared pool are managed by AsyncDecoder.
        if (m_ownsHwDeviceCtx) {
            av_buffer_unref(&m_hwDeviceCtx);
        } else {
            m_hwDeviceCtx = nullptr;
        }
    }
    if (m_swsCtx) {
        sws_freeContext(m_swsCtx);
        m_swsCtx = nullptr;
    }
    if (m_convertFrame) {
        av_frame_free(&m_convertFrame);
    }

    m_hwDeviceCtx = nullptr;
    m_hwPixFmt = AV_PIX_FMT_NONE;
    m_swsSourceFormat = AV_PIX_FMT_NONE;
    m_swsSourceSize = QSize();
    m_convertFrameSize = QSize();
    m_sequenceFramePaths.clear();
    m_sequenceFrameCache.clear();
    m_sequenceFrameCacheUseOrder.clear();
    m_sequenceFrameCacheBytes = 0;
    m_sequenceFrameUseCounter = 0;
}

bool DecoderContext::initialize() {
    if (isImageSequencePath(m_path)) {
        if (!loadImageSequence()) {
            return false;
        }
        updateAccessTime();
        return true;
    }

    if (isStillImagePath(m_path)) {
        if (!loadStillImage()) {
            return false;
        }
        updateAccessTime();
        return true;
    }

    if (!openInput()) {
        return false;
    }
    if (!initCodec()) {
        return false;
    }

    updateAccessTime();
    return true;
}

bool DecoderContext::openInput() {
    const QByteArray utf8Path = m_path.toUtf8();
    int ret = avformat_open_input(&m_formatCtx, utf8Path.constData(), nullptr, nullptr);
    if (ret < 0) {
        qWarning() << "Failed to open input:" << m_path << avErrToString(ret);
        return false;
    }

    ret = avformat_find_stream_info(m_formatCtx, nullptr);
    if (ret < 0) {
        qWarning() << "Failed to find stream info:" << avErrToString(ret);
        return false;
    }

    for (unsigned i = 0; i < m_formatCtx->nb_streams; ++i) {
        AVStream* stream = m_formatCtx->streams[i];
        if (!stream || !stream->codecpar) {
            continue;
        }
        if (stream->codecpar->codec_type == AVMEDIA_TYPE_VIDEO) {
            m_videoStreamIndex = static_cast<int>(i);

            m_info.path = m_path;
            m_info.frameSize = QSize(stream->codecpar->width, stream->codecpar->height);

            const AVRational framerate = av_guess_frame_rate(m_formatCtx, stream, nullptr);
            m_info.fps = (framerate.num > 0 && framerate.den > 0) ? av_q2d(framerate) : 30.0;

            if (stream->duration != AV_NOPTS_VALUE) {
                const double secs = stream->duration * av_q2d(stream->time_base);
                m_info.durationFrames = static_cast<int64_t>(secs * m_info.fps);
            } else if (m_formatCtx->duration > 0) {
                const double secs = m_formatCtx->duration / static_cast<double>(AV_TIME_BASE);
                m_info.durationFrames = static_cast<int64_t>(secs * m_info.fps);
            }

            m_info.bitrate = stream->codecpar->bit_rate;
            if (AVDictionaryEntry* alphaMode = av_dict_get(stream->metadata, "alpha_mode", nullptr, 0)) {
                m_streamHasAlphaTag = QByteArray(alphaMode->value) == "1";
                m_info.hasAlpha = m_streamHasAlphaTag;
            }
            m_info.isValid = true;
            break;
        }
    }

    if (m_videoStreamIndex < 0) {
        qWarning() << "No video stream found in:" << m_path;
        return false;
    }

    return true;
}

bool DecoderContext::loadStillImage() {
    QImage image = loadSingleImageFile(m_path);
    if (image.isNull()) {
        qWarning() << "Failed to load image:" << m_path;
        return false;
    }

    m_isStillImage = true;
    m_stillImage = std::move(image);
    m_info.path = m_path;
    m_info.durationFrames = 1;
    m_info.fps = 30.0;
    m_info.frameSize = m_stillImage.size();
    m_info.codecName = QStringLiteral("still-image");
    m_info.hasAlpha = m_stillImage.hasAlphaChannel();
    m_info.isValid = true;
    m_lastDecodedFrame = 0;
    m_eof = false;
    return true;
}

bool DecoderContext::loadImageSequence() {
    const QStringList framePaths = imageSequenceFramePaths(m_path);
    if (framePaths.isEmpty()) {
        return false;
    }

    QImage image = loadSequenceFrameImage(framePaths, 0);
    if (image.isNull()) {
        qWarning() << "Failed to load first image sequence frame:" << m_path;
        return false;
    }

    m_isImageSequence = true;
    m_sequenceFramePaths = framePaths;
    m_sequenceUsesWebp =
        QFileInfo(framePaths.constFirst()).suffix().compare(QStringLiteral("webp"), Qt::CaseInsensitive) == 0;
    m_stillImage = image.copy();

    m_info.path = m_path;
    m_info.durationFrames = framePaths.size();
    m_info.fps = 30.0;
    m_info.frameSize = image.size();
    m_info.codecName = QStringLiteral("image_sequence");
    m_info.hasAlpha = image.hasAlphaChannel();
    m_info.isValid = true;
    m_lastDecodedFrame = 0;
    m_eof = false;
    return true;
}

QImage DecoderContext::loadCachedSequenceFrameImage(int64_t frameNumber) {
    if (m_sequenceFramePaths.isEmpty()) {
        return QImage();
    }

    const int64_t boundedFrame =
        qBound<int64_t>(0, frameNumber, static_cast<int64_t>(m_sequenceFramePaths.size() - 1));

    auto cached = m_sequenceFrameCache.find(boundedFrame);
    if (cached != m_sequenceFrameCache.end()) {
        m_sequenceFrameCacheUseOrder[boundedFrame] = ++m_sequenceFrameUseCounter;
        return cached.value();
    }

    QImage image = loadSequenceFrameImage(m_sequenceFramePaths, boundedFrame);
    if (image.isNull()) {
        return QImage();
    }

    if (m_sequenceUsesWebp) {
        cacheSequenceFrameImage(boundedFrame, image);
    }
    return image;
}

void DecoderContext::cacheSequenceFrameImage(int64_t frameNumber, const QImage& image) {
    if (!m_sequenceUsesWebp || image.isNull()) {
        return;
    }

    const size_t imageBytes = image.sizeInBytes();
    if (imageBytes == 0 || imageBytes > kMaxSequenceFrameCacheBytes) {
        return;
    }

    auto existing = m_sequenceFrameCache.find(frameNumber);
    if (existing != m_sequenceFrameCache.end()) {
        m_sequenceFrameCacheBytes -= existing.value().sizeInBytes();
    }

    m_sequenceFrameCache.insert(frameNumber, image);
    m_sequenceFrameCacheUseOrder.insert(frameNumber, ++m_sequenceFrameUseCounter);
    m_sequenceFrameCacheBytes += imageBytes;
    trimSequenceFrameCache();
}

void DecoderContext::trimSequenceFrameCache() {
    while (m_sequenceFrameCache.size() > kMaxSequenceFrameCacheEntries ||
           m_sequenceFrameCacheBytes > kMaxSequenceFrameCacheBytes) {
        int64_t oldestFrame = -1;
        quint64 oldestUse = std::numeric_limits<quint64>::max();

        for (auto it = m_sequenceFrameCacheUseOrder.cbegin(); it != m_sequenceFrameCacheUseOrder.cend(); ++it) {
            if (it.value() < oldestUse) {
                oldestUse = it.value();
                oldestFrame = it.key();
            }
        }

        if (oldestFrame < 0) {
            break;
        }

        auto cacheIt = m_sequenceFrameCache.find(oldestFrame);
        if (cacheIt != m_sequenceFrameCache.end()) {
            m_sequenceFrameCacheBytes -= cacheIt.value().sizeInBytes();
            m_sequenceFrameCache.erase(cacheIt);
        }
        m_sequenceFrameCacheUseOrder.remove(oldestFrame);
    }
}

bool DecoderContext::initCodec() {
    AVStream* stream = m_formatCtx->streams[m_videoStreamIndex];
    const AVCodec* decoder = nullptr;

    const bool requiresSoftwareAlphaPath =
        stream->codecpar->codec_id == AV_CODEC_ID_VP9 && m_streamHasAlphaTag;
    if (requiresSoftwareAlphaPath) {
        decoder = avcodec_find_decoder_by_name("libvpx-vp9");
        if (decoder) {
            qDebug() << "Using libvpx-vp9 for alpha-tagged VP9 stream:" << m_path;
        }
    }
    if (!decoder) {
        decoder = avcodec_find_decoder(stream->codecpar->codec_id);
    }

    if (!decoder) {
        qWarning() << "Decoder not found for codec_id:" << stream->codecpar->codec_id;
        return false;
    }

    m_info.codecName = QString::fromUtf8(decoder->name);

    m_codecCtx = avcodec_alloc_context3(decoder);
    if (!m_codecCtx) {
        qWarning() << "Failed to allocate codec context";
        return false;
    }

    int ret = avcodec_parameters_to_context(m_codecCtx, stream->codecpar);
    if (ret < 0) {
        qWarning() << "Failed to copy codec params:" << avErrToString(ret);
        return false;
    }

    const bool headlessOffscreen =
        qEnvironmentVariable("QT_QPA_PLATFORM") == QStringLiteral("offscreen");
    const DecodePreference decodePreference = debugDecodePreference();
    const bool zeroCopyPreferred =
        decodePreference == DecodePreference::Auto ||
        decodePreference == DecodePreference::HardwareZeroCopy;
    const bool zeroCopySupported =
        zeroCopyPreferred &&
        !headlessOffscreen &&
        !m_streamHasAlphaTag &&
        zeroCopyInteropSupportedForCurrentBuild();
    const bool allowHardware =
        decodePreference != DecodePreference::Software &&
        !headlessOffscreen &&
        !m_streamHasAlphaTag;
    const bool hardwareEnabled = allowHardware && initHardwareAccel(decoder);
    const bool softwareProResWorkaround =
        !hardwareEnabled &&
        stream->codecpar->codec_id == AV_CODEC_ID_PRORES;

    if (hardwareEnabled) {
        m_codecCtx->thread_count = 0;
        m_codecCtx->thread_type = FF_THREAD_FRAME;
    } else if (softwareProResWorkaround) {
        m_codecCtx->thread_count = 1;
        m_codecCtx->thread_type = 0;
    } else {
        m_codecCtx->thread_count = qBound(2, QThread::idealThreadCount(), 8);
        m_codecCtx->thread_type = FF_THREAD_FRAME | FF_THREAD_SLICE;
        m_codecCtx->flags2 |= AV_CODEC_FLAG2_FAST;
    }

    if (m_streamHasAlphaTag) {
        m_codecCtx->get_format = get_alpha_compatible_format;
    }

    ret = avcodec_open2(m_codecCtx, decoder, nullptr);
    if (ret < 0) {
        qWarning() << "Failed to open codec:" << avErrToString(ret);
        return false;
    }

    m_info.requestedDecodeMode = decodePreferenceToString(decodePreference);
    m_info.hardwareAccelerated = hardwareEnabled;
    m_info.decodePath = hardwareEnabled ? QStringLiteral("hardware")
                                        : QStringLiteral("software");
    m_info.interopPath = hardwareEnabled
                             ? (zeroCopySupported ? QStringLiteral("cuda_gl_nv12_copy_candidate")
                                                  : QStringLiteral("hardware_cpu_upload"))
                             : QStringLiteral("software");

    qDebug() << "Decoder path for" << m_path << ":" << (hardwareEnabled ? "hardware" : "software");
    return true;
}

bool DecoderContext::initHardwareAccel(const AVCodec* decoder) {
    static const AVHWDeviceType kPreferredDevices[] = {
#ifdef __APPLE__
        AV_HWDEVICE_TYPE_VIDEOTOOLBOX,
#elif defined(_WIN32)
        AV_HWDEVICE_TYPE_D3D11VA,
        AV_HWDEVICE_TYPE_DXVA2,
#endif
        AV_HWDEVICE_TYPE_CUDA,
        AV_HWDEVICE_TYPE_VAAPI,
    };

    for (AVHWDeviceType type : kPreferredDevices) {
        const AVCodecHWConfig* selectedConfig = nullptr;
        for (int i = 0;; ++i) {
            const AVCodecHWConfig* config = avcodec_get_hw_config(decoder, i);
            if (!config) {
                break;
            }
            if (config->device_type == type &&
                (config->methods & AV_CODEC_HW_CONFIG_METHOD_HW_DEVICE_CTX)) {
                selectedConfig = config;
                break;
            }
        }

        if (!selectedConfig) {
            continue;
        }

        const char* deviceName = nullptr;
#if defined(Q_OS_LINUX)
        QByteArray devicePath;
        if (type == AV_HWDEVICE_TYPE_VAAPI) {
            static const char* kRenderNodes[] = {
                "/dev/dri/renderD128",
                "/dev/dri/renderD129",
                "/dev/dri/renderD130",
            };
            for (const char* candidate : kRenderNodes) {
                if (QFile::exists(QString::fromLatin1(candidate))) {
                    devicePath = QByteArray(candidate);
                    deviceName = devicePath.constData();
                    break;
                }
            }
            if (!deviceName) {
                continue;
            }
        }
#endif

        // --- Shared device path ---
        // If AsyncDecoder pre-created a device context for this type, borrow a ref
        // instead of allocating a brand-new CUDA/VAAPI context. This is the fix
        // for CUDA_ERROR_OUT_OF_MEMORY caused by N decoder threads each creating
        // their own cuCtxCreate().
        if (m_sharedHwDevices) {
            auto it = m_sharedHwDevices->find(static_cast<int>(type));
            if (it != m_sharedHwDevices->end() && it.value()) {
                AVBufferRef* borrowed = av_buffer_ref(it.value());
                if (borrowed) {
                    m_codecCtx->hw_device_ctx = av_buffer_ref(borrowed);
                    m_hwDeviceCtx = borrowed;
                    m_ownsHwDeviceCtx = false;
                    m_hwPixFmt = selectedConfig->pix_fmt;
                    m_codecCtx->get_format = get_hw_format;
                    m_codecCtx->opaque = reinterpret_cast<void*>(static_cast<intptr_t>(m_hwPixFmt));
                    qDebug() << "Using shared hardware acceleration:" << type << "for" << m_path;
                    return true;
                }
            }
        }

        // --- Fallback: create a new device context ---
        // This happens during getVideoInfo() probing (no shared pool available)
        // or when the shared pool doesn't have this device type.
        AVBufferRef* hwCtx = nullptr;
        const int ret = av_hwdevice_ctx_create(&hwCtx, type, deviceName, nullptr, 0);
        if (ret >= 0) {
            m_codecCtx->hw_device_ctx = av_buffer_ref(hwCtx);
            m_hwDeviceCtx = hwCtx;
            m_ownsHwDeviceCtx = true;
            m_hwPixFmt = selectedConfig->pix_fmt;
            m_codecCtx->get_format = get_hw_format;
            m_codecCtx->opaque = reinterpret_cast<void*>(static_cast<intptr_t>(m_hwPixFmt));

            qDebug() << "Using hardware acceleration:" << type << "for" << m_path;
            return true;
        }
    }

    m_codecCtx->get_format = nullptr;
    m_codecCtx->opaque = nullptr;
    m_hwPixFmt = AV_PIX_FMT_NONE;
    return false;
}

FrameHandle DecoderContext::decodeFrame(int64_t frameNumber) {
    if (m_isImageSequence) {
        updateAccessTime();
        QImage image = loadCachedSequenceFrameImage(frameNumber);
        if (image.isNull()) {
            return FrameHandle();
        }
        m_lastDecodedFrame = qBound<int64_t>(0, frameNumber, static_cast<int64_t>(m_sequenceFramePaths.size() - 1));
        return FrameHandle::createCpuFrame(image, m_lastDecodedFrame, m_path);
    }

    if (m_isStillImage) {
        updateAccessTime();
        return FrameHandle::createCpuFrame(m_stillImage, 0, m_path);
    }

    decodeTrace(QStringLiteral("DecoderContext::decodeFrame.begin"),
                QStringLiteral("file=%1 target=%2 last=%3")
                    .arg(shortPath(m_path))
                    .arg(frameNumber)
                    .arg(m_lastDecodedFrame));

    FrameHandle result = seekAndDecode(frameNumber);

    decodeTrace(QStringLiteral("DecoderContext::decodeFrame.end"),
                QStringLiteral("file=%1 target=%2 null=%3 decoded=%4")
                    .arg(shortPath(m_path))
                    .arg(frameNumber)
                    .arg(result.isNull())
                    .arg(result.frameNumber()));
    return result;
}

QVector<FrameHandle> DecoderContext::decodeThroughFrame(int64_t targetFrame) {
    if (m_isImageSequence) {
        updateAccessTime();

        QVector<FrameHandle> frames;
        const int64_t maxFrame = static_cast<int64_t>(m_sequenceFramePaths.size() - 1);
        const int64_t boundedTarget = qBound<int64_t>(0, targetFrame, maxFrame);
        const int64_t endFrame = m_sequenceUsesWebp
                                     ? qMin<int64_t>(maxFrame, boundedTarget + kWebpSequenceBatchAhead)
                                     : boundedTarget;

        frames.reserve(static_cast<int>(endFrame - boundedTarget + 1));
        for (int64_t frameNumber = boundedTarget; frameNumber <= endFrame; ++frameNumber) {
            QImage image = loadCachedSequenceFrameImage(frameNumber);
            if (image.isNull()) {
                continue;
            }
            frames.push_back(FrameHandle::createCpuFrame(image, frameNumber, m_path));
        }

        m_lastDecodedFrame = boundedTarget;
        return frames;
    }

    if (m_isStillImage) {
        updateAccessTime();
        return {FrameHandle::createCpuFrame(m_stillImage, 0, m_path)};
    }

    if (m_eof) {
        m_eof = false;
    }

    updateAccessTime();

    const int64_t frameDelta = targetFrame - m_lastDecodedFrame;
    const bool forceSeek = m_lastDecodedFrame < 0 || frameDelta < 0 || frameDelta > kMaxSequentialDecodeGap;
    if (forceSeek) {
        decodeTrace(QStringLiteral("DecoderContext::decodeFrame.seek"),
                    QStringLiteral("file=%1 target=%2 delta=%3")
                        .arg(shortPath(m_path))
                        .arg(targetFrame)
                        .arg(frameDelta));
    }

    return decodeForwardUntil(targetFrame, forceSeek);
}

FrameHandle DecoderContext::seekAndDecode(int64_t frameNumber) {
    if (m_isImageSequence) {
        return decodeFrame(frameNumber);
    }

    if (m_isStillImage) {
        updateAccessTime();
        return FrameHandle::createCpuFrame(m_stillImage, 0, m_path);
    }

    const qint64 startedAt = decodeTraceMs();
    decodeTrace(QStringLiteral("DecoderContext::seekAndDecode.begin"),
                QStringLiteral("file=%1 target=%2")
                    .arg(shortPath(m_path))
                    .arg(frameNumber));

    const QVector<FrameHandle> frames = decodeForwardUntil(frameNumber, true);
    FrameHandle result;
    for (const FrameHandle& frame : frames) {
        if (!frame.isNull() && frame.frameNumber() >= frameNumber) {
            result = frame;
            break;
        }
    }

    decodeTrace(QStringLiteral("DecoderContext::seekAndDecode.end"),
                QStringLiteral("file=%1 target=%2 null=%3 waitMs=%4 decoded=%5")
                    .arg(shortPath(m_path))
                    .arg(frameNumber)
                    .arg(result.isNull())
                    .arg(decodeTraceMs() - startedAt)
                    .arg(result.frameNumber()));
    return result;
}

QVector<FrameHandle> DecoderContext::decodeForwardUntil(int64_t targetFrame, bool forceSeek) {
    QVector<FrameHandle> decodedFrames;

    if (m_isStillImage) {
        decodedFrames.push_back(FrameHandle::createCpuFrame(m_stillImage, 0, m_path));
        return decodedFrames;
    }

    if (forceSeek && !seekToKeyframe(targetFrame)) {
        decodeTrace(QStringLiteral("DecoderContext::seekAndDecode.seek-failed"),
                    QStringLiteral("file=%1 target=%2")
                        .arg(shortPath(m_path))
                        .arg(targetFrame));
        return decodedFrames;
    }

    AVFrame* frame = av_frame_alloc();
    AVPacket* packet = av_packet_alloc();
    if (!frame || !packet) {
        if (frame) {
            av_frame_free(&frame);
        }
        if (packet) {
            av_packet_free(&packet);
        }
        return decodedFrames;
    }

    AVStream* stream = m_formatCtx->streams[m_videoStreamIndex];

    auto receiveAvailableFrames = [&, this](bool& reachedTarget) {
        while (true) {
            const int ret = avcodec_receive_frame(m_codecCtx, frame);
            if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF) {
                break;
            }
            if (ret < 0) {
                break;
            }

            int64_t pts = frame->best_effort_timestamp != AV_NOPTS_VALUE ? frame->best_effort_timestamp : frame->pts;
            int64_t currentFrame = ptsToFrameNumber(pts, stream->time_base, m_info.fps);
            if (currentFrame < 0) {
                currentFrame = targetFrame;
            }

            m_lastDecodedFrame = currentFrame;
            decodedFrames.push_back(convertToFrame(frame, currentFrame));
            av_frame_unref(frame);

            if (currentFrame >= targetFrame) {
                reachedTarget = true;
                break;
            }
        }
    };

    bool reachedTarget = false;

    while (!reachedTarget && av_read_frame(m_formatCtx, packet) >= 0) {
        if (packet->stream_index != m_videoStreamIndex) {
            av_packet_unref(packet);
            continue;
        }

        const int sendRet = avcodec_send_packet(m_codecCtx, packet);
        av_packet_unref(packet);
        if (sendRet < 0) {
            continue;
        }

        receiveAvailableFrames(reachedTarget);
    }

    if (!reachedTarget) {
        avcodec_send_packet(m_codecCtx, nullptr);
        receiveAvailableFrames(reachedTarget);
        m_eof = true;
    }

    av_frame_free(&frame);
    av_packet_free(&packet);
    return decodedFrames;
}

bool DecoderContext::seekToKeyframe(int64_t targetFrame) {
    AVStream* stream = m_formatCtx->streams[m_videoStreamIndex];

    const double targetSeconds = targetFrame / qMax(1.0, m_info.fps);
    const int64_t targetUsec = qMax<int64_t>(0, qRound64(targetSeconds * AV_TIME_BASE));
    const int64_t targetTs = av_rescale_q(targetUsec, AVRational{1, AV_TIME_BASE}, stream->time_base);

    const int ret = avformat_seek_file(m_formatCtx,
                                       m_videoStreamIndex,
                                       std::numeric_limits<int64_t>::min(),
                                       targetTs,
                                       targetTs,
                                       AVSEEK_FLAG_BACKWARD);
    if (ret < 0) {
        qWarning() << "Seek failed:" << avErrToString(ret);
        return false;
    }

    avcodec_flush_buffers(m_codecCtx);
    m_lastDecodedFrame = -1;
    m_eof = false;
    return true;
}

FrameHandle DecoderContext::convertToFrame(AVFrame* avFrame, int64_t frameNumber) {
#ifdef EDITOR_HAS_CUDA
    if (avFrame->format == m_hwPixFmt &&
        m_hwPixFmt != AV_PIX_FMT_NONE &&
        (debugDecodePreference() == DecodePreference::HardwareZeroCopy ||
         debugDecodePreference() == DecodePreference::Auto) &&
        avFrame->hw_frames_ctx) {
        auto* framesContext = reinterpret_cast<AVHWFramesContext*>(avFrame->hw_frames_ctx->data);
        const int swPixelFormat = framesContext ? framesContext->sw_format : AV_PIX_FMT_NONE;
        if (swPixelFormat == AV_PIX_FMT_NV12) {
            FrameHandle hardwareHandle =
                FrameHandle::createHardwareFrame(avFrame, frameNumber, m_path, swPixelFormat);
            if (!hardwareHandle.isNull()) {
                return hardwareHandle;
            }
        }
    }
#endif

    QImage image = convertAVFrameToImage(avFrame);
    if (image.isNull()) {
        return FrameHandle();
    }

    if (image.format() != QImage::Format_ARGB32_Premultiplied) {
        image = image.convertToFormat(QImage::Format_ARGB32_Premultiplied);
    }

    return FrameHandle::createCpuFrame(image, frameNumber, m_path);
}

QImage DecoderContext::convertAVFrameToImage(AVFrame* frame) {
    AVFrame* swFrame = frame;
    AVFrame* tempFrame = nullptr;

    if (frame->format == m_hwPixFmt && m_hwPixFmt != AV_PIX_FMT_NONE) {
        tempFrame = av_frame_alloc();
        if (!tempFrame) {
            return QImage();
        }
        if (av_hwframe_transfer_data(tempFrame, frame, 0) < 0) {
            av_frame_free(&tempFrame);
            return QImage();
        }
        swFrame = tempFrame;
    }

    AVPixelFormat sourceFormat = static_cast<AVPixelFormat>(swFrame->format);
    if (sourceFormat == AV_PIX_FMT_YUVJ420P) {
        sourceFormat = AV_PIX_FMT_YUV420P;
    }

    const AVPixFmtDescriptor* sourceDesc = av_pix_fmt_desc_get(sourceFormat);
    const bool sourceHasAlpha = sourceDesc && (sourceDesc->flags & AV_PIX_FMT_FLAG_ALPHA);

    if (!m_loggedSourceFormat) {
        m_loggedSourceFormat = true;
        decodeTrace(QStringLiteral("DecoderContext::convertAVFrameToImage.format"),
                    QStringLiteral("file=%1 fmt=%2 alphaTag=%3 alphaFmt=%4 linesize=[%5,%6,%7,%8]")
                        .arg(shortPath(m_path))
                        .arg(sourceDesc ? QString::fromUtf8(sourceDesc->name) : QStringLiteral("unknown"))
                        .arg(m_streamHasAlphaTag)
                        .arg(sourceHasAlpha)
                        .arg(swFrame->linesize[0])
                        .arg(swFrame->linesize[1])
                        .arg(swFrame->linesize[2])
                        .arg(swFrame->linesize[3]));
    }

    if (m_streamHasAlphaTag && !sourceHasAlpha && !m_reportedAlphaMismatch) {
        m_reportedAlphaMismatch = true;
        qWarning().noquote() << QStringLiteral(
            "Alpha-tagged stream is decoding without alpha-capable pixel format; attempting conversion anyway for %1 (fmt=%2)")
            .arg(shortPath(m_path))
            .arg(sourceDesc ? QString::fromUtf8(sourceDesc->name) : QStringLiteral("unknown"));
    }

    if (!m_swsCtx ||
        m_swsSourceFormat != sourceFormat ||
        m_swsSourceSize.width() != swFrame->width ||
        m_swsSourceSize.height() != swFrame->height) {
        m_swsCtx = sws_getCachedContext(m_swsCtx,
                                        swFrame->width,
                                        swFrame->height,
                                        sourceFormat,
                                        swFrame->width,
                                        swFrame->height,
                                        AV_PIX_FMT_RGBA,
                                        SWS_BILINEAR,
                                        nullptr,
                                        nullptr,
                                        nullptr);
        m_swsSourceFormat = sourceFormat;
        m_swsSourceSize = QSize(swFrame->width, swFrame->height);
    }

    if (!m_swsCtx) {
        if (tempFrame) {
            av_frame_free(&tempFrame);
        }
        return QImage();
    }

    if (!m_convertFrame ||
        m_convertFrameSize.width() != swFrame->width ||
        m_convertFrameSize.height() != swFrame->height ||
        m_convertFrame->format != AV_PIX_FMT_RGBA) {
        if (m_convertFrame) {
            av_frame_free(&m_convertFrame);
        }

        m_convertFrame = av_frame_alloc();
        if (!m_convertFrame) {
            if (tempFrame) {
                av_frame_free(&tempFrame);
            }
            return QImage();
        }

        m_convertFrame->format = AV_PIX_FMT_RGBA;
        m_convertFrame->width = swFrame->width;
        m_convertFrame->height = swFrame->height;
        if (av_frame_get_buffer(m_convertFrame, 32) < 0) {
            av_frame_free(&m_convertFrame);
            if (tempFrame) {
                av_frame_free(&tempFrame);
            }
            return QImage();
        }
        m_convertFrameSize = QSize(swFrame->width, swFrame->height);
    }

    if (av_frame_make_writable(m_convertFrame) < 0) {
        if (tempFrame) {
            av_frame_free(&tempFrame);
        }
        return QImage();
    }

    if (sws_scale(m_swsCtx,
                  swFrame->data,
                  swFrame->linesize,
                  0,
                  swFrame->height,
                  m_convertFrame->data,
                  m_convertFrame->linesize) <= 0) {
        if (tempFrame) {
            av_frame_free(&tempFrame);
        }
        return QImage();
    }

    QImage image(swFrame->width, swFrame->height, QImage::Format_RGBA8888);
    if (image.isNull()) {
        if (tempFrame) {
            av_frame_free(&tempFrame);
        }
        return QImage();
    }

    const int copyBytesPerRow = qMin(image.bytesPerLine(), m_convertFrame->linesize[0]);
    for (int y = 0; y < swFrame->height; ++y) {
        memcpy(image.scanLine(y),
               m_convertFrame->data[0] + (y * m_convertFrame->linesize[0]),
               static_cast<size_t>(copyBytesPerRow));
    }

    if (m_streamHasAlphaTag && !m_loggedAlphaProbe && !image.isNull()) {
        m_loggedAlphaProbe = true;
        const auto samplePixel = [&image](int x, int y) {
            const int sx = qBound(0, x, image.width() - 1);
            const int sy = qBound(0, y, image.height() - 1);
            const uchar* px = image.constScanLine(sy) + (sx * 4);
            return QStringLiteral("(%1,%2)=[r=%3 g=%4 b=%5 a=%6]")
                .arg(sx)
                .arg(sy)
                .arg(px[0])
                .arg(px[1])
                .arg(px[2])
                .arg(px[3]);
        };

        qDebug().noquote() << QStringLiteral(
            "[ALPHA] file=%1 size=%2x%3 samples: %4 %5 %6 %7 %8")
            .arg(shortPath(m_path))
            .arg(image.width())
            .arg(image.height())
            .arg(samplePixel(0, 0))
            .arg(samplePixel(image.width() / 2, image.height() / 2))
            .arg(samplePixel(image.width() - 1, 0))
            .arg(samplePixel(0, image.height() - 1))
            .arg(samplePixel(image.width() - 1, image.height() - 1));
    }

    if (tempFrame) {
        av_frame_free(&tempFrame);
    }

    return image;
}

void DecoderContext::updateAccessTime() {
    m_lastAccessTime = QDateTime::currentMSecsSinceEpoch();
}

} // namespace editor
