#include "editor.h"
#include "auto_sync_plan.h"
#include "decoder_context.h"
#include "decoder_ffmpeg_utils.h"
#include "processing_job_manifest.h"
#include <QApplication>
#include <QDialog>
#include <QDir>
#include <QFile>
#include <QFileDialog>
#include <QFileInfo>
#include <QComboBox>
#include <QDoubleSpinBox>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QInputDialog>
#include <QMessageBox>
#include <QPlainTextEdit>
#include <QPointer>
#include <QProcess>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QPushButton>
#include <QProgressDialog>
#include <QProgressBar>
#include <QProcessEnvironment>
#include <QRegularExpression>
#include <QSettings>
#include <QSpinBox>
#include <QStandardPaths>
#include <QTemporaryFile>
#include <QTextCursor>
#include <QTextStream>
#include <QVBoxLayout>

#include <algorithm>
#include <atomic>
#include <filesystem>
#include <functional>
#include <limits>
#include <memory>
#include <system_error>
#include <thread>
#include <vector>

extern "C" {
#include <libavutil/opt.h>
}

using namespace editor;

namespace {

QString shellQuote(const QString& value) {
    QString escaped = value;
    escaped.replace(QLatin1Char('\''), QStringLiteral("'\\''"));
    return QStringLiteral("'%1'").arg(escaped);
}

QString expandUserPath(const QString& path) {
    QString trimmed = path.trimmed();
    if (trimmed == QStringLiteral("~")) {
        return QDir::homePath();
    }
    if (trimmed.startsWith(QStringLiteral("~/"))) {
        return QDir::home().absoluteFilePath(trimmed.mid(2));
    }
    return trimmed;
}

QString defaultSam3ModelCachePath() {
    const QString appCache =
        QStandardPaths::writableLocation(QStandardPaths::CacheLocation);
    if (!appCache.isEmpty()) {
        return QDir(appCache).absoluteFilePath(QStringLiteral("sam3/hf"));
    }
    return QDir(QDir::currentPath()).absoluteFilePath(QStringLiteral(".cache/hf"));
}

QString defaultSam3RuntimeCachePath() {
    const QString appCache =
        QStandardPaths::writableLocation(QStandardPaths::CacheLocation);
    if (!appCache.isEmpty()) {
        return QDir(appCache).absoluteFilePath(QStringLiteral("sam3/runtime"));
    }
    return QDir(QDir::currentPath()).absoluteFilePath(QStringLiteral(".cache"));
}

QString defaultSam3RuntimeCachePath(const QString& modelCachePath) {
    const QString expandedModelCache = expandUserPath(modelCachePath);
    if (!expandedModelCache.trimmed().isEmpty()) {
        const QFileInfo modelInfo(expandedModelCache);
        if (modelInfo.fileName().compare(QStringLiteral("hf"), Qt::CaseInsensitive) == 0) {
            return modelInfo.dir().absoluteFilePath(QStringLiteral("runtime"));
        }
        return modelInfo.dir().absoluteFilePath(QStringLiteral("sam3_runtime"));
    }
    return defaultSam3RuntimeCachePath();
}

std::filesystem::path fsPathForQtPath(const QString& path) {
    return std::filesystem::path(path.toStdString()).lexically_normal();
}

bool pathsEqual(const std::filesystem::path& lhs, const std::filesystem::path& rhs) {
    std::error_code ec;
    if (std::filesystem::equivalent(lhs, rhs, ec)) {
        return true;
    }
    return lhs.lexically_normal() == rhs.lexically_normal();
}

bool pathContains(const std::filesystem::path& parent, const std::filesystem::path& child) {
    const auto normalizedParent = parent.lexically_normal();
    const auto normalizedChild = child.lexically_normal();
    auto parentIt = normalizedParent.begin();
    auto childIt = normalizedChild.begin();
    for (; parentIt != normalizedParent.end() && childIt != normalizedChild.end();
         ++parentIt, ++childIt) {
        if (*parentIt != *childIt) {
            return false;
        }
    }
    return parentIt == normalizedParent.end();
}

bool directoryHasEntries(const QString& path) {
    std::error_code ec;
    const std::filesystem::path fsPath = fsPathForQtPath(path);
    if (!std::filesystem::exists(fsPath, ec) ||
        !std::filesystem::is_directory(fsPath, ec)) {
        return false;
    }
    return std::filesystem::directory_iterator(fsPath, ec) !=
           std::filesystem::directory_iterator();
}

struct ProxyGenerationResult {
    bool success = false;
    bool canceled = false;
    QString message;
    int framesWritten = 0;
};

struct AvFormatContextDeleter {
    void operator()(AVFormatContext* value) const
    {
        if (!value) {
            return;
        }
        if (value->pb && value->oformat && !(value->oformat->flags & AVFMT_NOFILE)) {
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

QImage proxyScaledImage(const QImage& source)
{
    if (source.isNull()) {
        return QImage();
    }
    QImage image = source;
    if (image.width() > 1280) {
        const int scaledHeight = qMax(1, qRound(static_cast<qreal>(image.height()) *
                                                (1280.0 / static_cast<qreal>(image.width()))));
        image = image.scaled(1280,
                             scaledHeight,
                             Qt::KeepAspectRatio,
                             Qt::SmoothTransformation);
    }
    if (image.hasAlphaChannel()) {
        return image.convertToFormat(QImage::Format_RGBA8888);
    }
    return image.convertToFormat(QImage::Format_RGB888);
}

QSize evenProxyVideoSize(const QSize& size)
{
    return QSize(qMax(2, size.width() & ~1),
                 qMax(2, size.height() & ~1));
}

QString proxyFramePath(const QString& outputDir, int frameNumber, bool hasAlpha)
{
    return QDir(outputDir).absoluteFilePath(
        QStringLiteral("frame_%1.%2")
            .arg(frameNumber, 6, 10, QLatin1Char('0'))
            .arg(hasAlpha ? QStringLiteral("png") : QStringLiteral("jpg")));
}

bool flushProxyEncoder(AVCodecContext* codecContext,
                       AVFormatContext* formatContext,
                       AVStream* stream,
                       QString* errorOut)
{
    const int sendResult = avcodec_send_frame(codecContext, nullptr);
    if (sendResult < 0) {
        if (errorOut) {
            *errorOut = QStringLiteral("Failed to flush proxy encoder: %1").arg(avErrToString(sendResult));
        }
        return false;
    }

    std::unique_ptr<AVPacket, AvPacketDeleter> packet(av_packet_alloc());
    if (!packet) {
        if (errorOut) {
            *errorOut = QStringLiteral("Failed to allocate proxy flush packet.");
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
                *errorOut = QStringLiteral("Failed to receive proxy flush packet: %1")
                                .arg(avErrToString(receiveResult));
            }
            return false;
        }
        av_packet_rescale_ts(packet.get(), codecContext->time_base, stream->time_base);
        packet->stream_index = stream->index;
        const int writeResult = av_interleaved_write_frame(formatContext, packet.get());
        av_packet_unref(packet.get());
        if (writeResult < 0) {
            if (errorOut) {
                *errorOut = QStringLiteral("Failed to write proxy flush packet: %1")
                                .arg(avErrToString(writeResult));
            }
            return false;
        }
    }
}

ProxyGenerationResult generateImageSequenceProxyWithLibraries(
    const QString& sourcePath,
    const QStringList& sequenceFrames,
    bool imageSequenceSource,
    const QString& outputPath,
    const MediaProbeResult& sourceProbe,
    int resumeNextFrameNumber,
    const std::shared_ptr<std::atomic_bool>& cancelFlag,
    const std::function<void(const QString&)>& log,
    const std::function<void(int)>& progress)
{
    ProxyGenerationResult result;
    const bool hasAlpha = sourceProbe.hasAlpha;
    if (resumeNextFrameNumber <= 1) {
        const QFileInfo outputInfo(outputPath);
        if (outputInfo.exists()) {
            bool removed = false;
            if (outputInfo.isDir()) {
                QDir staleDir(outputPath);
                removed = staleDir.removeRecursively();
            } else {
                removed = QFile::remove(outputPath);
            }
            if (!removed) {
                result.message = QStringLiteral("Failed to remove existing proxy output: %1")
                                     .arg(QDir::toNativeSeparators(outputPath));
                return result;
            }
        }
    }
    QDir outputDir(outputPath);
    if (!outputDir.exists() && !outputDir.mkpath(QStringLiteral("."))) {
        result.message = QStringLiteral("Failed to create proxy directory: %1").arg(outputPath);
        return result;
    }

    const int startFrameNumber = qMax(1, resumeNextFrameNumber);
    const int startIndex = startFrameNumber - 1;
    int totalFrames = 0;
    if (imageSequenceSource) {
        totalFrames = sequenceFrames.size();
    } else if (sourceProbe.durationFrames > 0) {
        totalFrames = static_cast<int>(qMin<int64_t>(sourceProbe.durationFrames, std::numeric_limits<int>::max()));
    }
    if (totalFrames <= 0) {
        totalFrames = 1;
    }
    if (startIndex >= totalFrames) {
        result.success = true;
        result.message = QStringLiteral("Proxy already contains all expected frames.");
        progress(100);
        return result;
    }

    log(QStringLiteral("Internal proxy generation\n"));
    log(QStringLiteral("Source: %1\n").arg(QDir::toNativeSeparators(sourcePath)));
    log(QStringLiteral("Output: %1\n").arg(QDir::toNativeSeparators(outputPath)));
    log(QStringLiteral("Decode: explicit software path for video sources\n"));
    log(QStringLiteral("Format: %1 image sequence\n").arg(hasAlpha ? QStringLiteral("PNG") : QStringLiteral("JPEG")));

    std::unique_ptr<editor::DecoderContext> decoder;
    if (!imageSequenceSource) {
        decoder = std::make_unique<editor::DecoderContext>(sourcePath, nullptr, true);
        if (!decoder->initialize()) {
            result.message = QStringLiteral("Failed to initialize software decoder for proxy source.");
            return result;
        }
        if (decoder->info().durationFrames > 0) {
            totalFrames = static_cast<int>(qMin<int64_t>(decoder->info().durationFrames,
                                                        std::numeric_limits<int>::max()));
        }
        log(QStringLiteral("Decoder: %1 (%2)\n")
                .arg(decoder->info().codecName, decoder->info().decodePath));
    }

    for (int frameIndex = startIndex; frameIndex < totalFrames; ++frameIndex) {
        if (cancelFlag && cancelFlag->load()) {
            result.canceled = true;
            result.message = QStringLiteral("Proxy generation canceled.");
            return result;
        }

        QImage image;
        if (imageSequenceSource) {
            if (frameIndex >= sequenceFrames.size()) {
                break;
            }
            image = QImage(sequenceFrames[frameIndex]);
        } else {
            const editor::FrameHandle frame = decoder->decodeFrame(frameIndex);
            if (frame.isNull() || !frame.hasCpuImage()) {
                result.message = QStringLiteral("Software decoder returned no frame at source frame %1.")
                                     .arg(frameIndex);
                return result;
            }
            image = frame.cpuImage();
        }

        image = proxyScaledImage(image);
        if (image.isNull()) {
            result.message = QStringLiteral("Failed to prepare proxy frame %1.").arg(frameIndex + 1);
            return result;
        }

        const QString framePath = proxyFramePath(outputPath, frameIndex + 1, hasAlpha);
        const bool saved = hasAlpha
                               ? image.save(framePath, "PNG")
                               : image.save(framePath, "JPG", 90);
        if (!saved) {
            result.message = QStringLiteral("Failed to write proxy frame: %1")
                                 .arg(QDir::toNativeSeparators(framePath));
            return result;
        }
        ++result.framesWritten;

        if (result.framesWritten == 1 || result.framesWritten % 15 == 0) {
            log(QStringLiteral("Wrote frame %1/%2\n").arg(frameIndex + 1).arg(totalFrames));
        }
        const int pct = qBound(0,
                               qRound((static_cast<double>(frameIndex + 1) /
                                       static_cast<double>(qMax(1, totalFrames))) * 100.0),
                               100);
        progress(pct);
    }

    result.success = true;
    result.message = QStringLiteral("Proxy generation finished. Frames written: %1.")
                         .arg(result.framesWritten);
    progress(100);
    return result;
}

const AVCodec* proxyVideoEncoderForFormat(ProxyFormat format, QString* labelOut)
{
    if (format == ProxyFormat::H264) {
        if (const AVCodec* codec = avcodec_find_encoder_by_name("libx264")) {
            if (labelOut) {
                *labelOut = QStringLiteral("libx264");
            }
            return codec;
        }
        if (const AVCodec* codec = avcodec_find_encoder(AV_CODEC_ID_H264)) {
            if (labelOut) {
                *labelOut = QStringLiteral("h264");
            }
            return codec;
        }
        if (const AVCodec* codec = avcodec_find_encoder_by_name("libopenh264")) {
            if (labelOut) {
                *labelOut = QStringLiteral("libopenh264");
            }
            return codec;
        }
        if (const AVCodec* codec = avcodec_find_encoder(AV_CODEC_ID_MPEG4)) {
            if (labelOut) {
                *labelOut = QStringLiteral("mpeg4");
            }
            return codec;
        }
        return nullptr;
    }

    if (format == ProxyFormat::MJPEG) {
        if (labelOut) {
            *labelOut = QStringLiteral("mjpeg");
        }
        return avcodec_find_encoder(AV_CODEC_ID_MJPEG);
    }

    return nullptr;
}

ProxyGenerationResult generateVideoContainerProxyWithLibraries(
    const QString& sourcePath,
    const QString& outputPath,
    ProxyFormat format,
    const MediaProbeResult& sourceProbe,
    const std::shared_ptr<std::atomic_bool>& cancelFlag,
    const std::function<void(const QString&)>& log,
    const std::function<void(int)>& progress)
{
    ProxyGenerationResult result;
    if (format != ProxyFormat::H264 && format != ProxyFormat::MJPEG) {
        result.message = QStringLiteral("Unsupported container proxy format.");
        return result;
    }
    if (sourceProbe.hasAlpha) {
        result.message = QStringLiteral("Container proxies for alpha sources are not supported; use PNG image sequence proxy.");
        return result;
    }

    const QFileInfo outputInfo(outputPath);
    if (!outputInfo.dir().exists() && !outputInfo.dir().mkpath(QStringLiteral("."))) {
        result.message = QStringLiteral("Failed to create proxy output directory: %1")
                             .arg(QDir::toNativeSeparators(outputInfo.dir().absolutePath()));
        return result;
    }
    if (outputInfo.exists()) {
        bool removed = outputInfo.isDir() ? QDir(outputPath).removeRecursively()
                                          : QFile::remove(outputPath);
        if (!removed) {
            result.message = QStringLiteral("Failed to remove existing proxy output: %1")
                                 .arg(QDir::toNativeSeparators(outputPath));
            return result;
        }
    }

    std::unique_ptr<editor::DecoderContext> decoder =
        std::make_unique<editor::DecoderContext>(sourcePath, nullptr, true);
    if (!decoder->initialize()) {
        result.message = QStringLiteral("Failed to initialize software decoder for proxy source.");
        return result;
    }

    int totalFrames = sourceProbe.durationFrames > 0
                          ? static_cast<int>(qMin<int64_t>(sourceProbe.durationFrames,
                                                           std::numeric_limits<int>::max()))
                          : 0;
    if (decoder->info().durationFrames > 0) {
        totalFrames = static_cast<int>(qMin<int64_t>(decoder->info().durationFrames,
                                                    std::numeric_limits<int>::max()));
    }
    if (totalFrames <= 0) {
        totalFrames = 1;
    }

    const editor::FrameHandle firstFrame = decoder->decodeFrame(0);
    if (firstFrame.isNull() || !firstFrame.hasCpuImage()) {
        result.message = QStringLiteral("Software decoder returned no first frame for proxy source.");
        return result;
    }
    QImage firstImage = proxyScaledImage(firstFrame.cpuImage());
    if (firstImage.isNull()) {
        result.message = QStringLiteral("Failed to prepare first proxy frame.");
        return result;
    }
    const QSize outputSize = evenProxyVideoSize(firstImage.size());
    if (outputSize != firstImage.size()) {
        firstImage = firstImage.scaled(outputSize, Qt::IgnoreAspectRatio, Qt::SmoothTransformation);
    }

    AVFormatContext* rawFormatContext = nullptr;
    const QByteArray outputPathBytes = QFile::encodeName(outputPath);
    const int allocResult = avformat_alloc_output_context2(&rawFormatContext,
                                                           nullptr,
                                                           nullptr,
                                                           outputPathBytes.constData());
    if (allocResult < 0 || !rawFormatContext) {
        result.message = QStringLiteral("Failed to create proxy output context: %1").arg(avErrToString(allocResult));
        return result;
    }
    std::unique_ptr<AVFormatContext, AvFormatContextDeleter> formatContext(rawFormatContext);

    QString encoderLabel;
    const AVCodec* codec = proxyVideoEncoderForFormat(format, &encoderLabel);
    if (!codec) {
        result.message = format == ProxyFormat::H264
                             ? QStringLiteral("No H.264-compatible software encoder is available in bundled FFmpeg.")
                             : QStringLiteral("No MJPEG encoder is available in bundled FFmpeg.");
        return result;
    }

    AVStream* stream = avformat_new_stream(formatContext.get(), codec);
    if (!stream) {
        result.message = QStringLiteral("Failed to create proxy video stream.");
        return result;
    }

    std::unique_ptr<AVCodecContext, AvCodecContextDeleter> codecContext(avcodec_alloc_context3(codec));
    if (!codecContext) {
        result.message = QStringLiteral("Failed to allocate proxy encoder context.");
        return result;
    }

    const int proxyFps =
        qMax(1, qRound(sourceProbe.fps > 0.001 ? sourceProbe.fps : static_cast<double>(kTimelineFps)));
    codecContext->codec_id = codec->id;
    codecContext->codec_type = AVMEDIA_TYPE_VIDEO;
    codecContext->width = outputSize.width();
    codecContext->height = outputSize.height();
    codecContext->time_base = AVRational{1, proxyFps};
    codecContext->framerate = AVRational{proxyFps, 1};
    codecContext->gop_size = proxyFps;
    codecContext->max_b_frames = 0;
    codecContext->pix_fmt = format == ProxyFormat::MJPEG ? AV_PIX_FMT_YUVJ420P : AV_PIX_FMT_YUV420P;
    codecContext->bit_rate = format == ProxyFormat::MJPEG ? 40'000'000 : 8'000'000;
    if (formatContext->oformat->flags & AVFMT_GLOBALHEADER) {
        codecContext->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;
    }

    if (format == ProxyFormat::H264) {
        av_opt_set(codecContext->priv_data, "preset", "veryfast", 0);
        av_opt_set(codecContext->priv_data, "crf", "18", 0);
    } else {
        codecContext->color_range = AVCOL_RANGE_JPEG;
    }

    const int openResult = avcodec_open2(codecContext.get(), codec, nullptr);
    if (openResult < 0) {
        result.message = QStringLiteral("Failed to open proxy encoder %1: %2")
                             .arg(encoderLabel, avErrToString(openResult));
        return result;
    }
    if (avcodec_parameters_from_context(stream->codecpar, codecContext.get()) < 0) {
        result.message = QStringLiteral("Failed to copy proxy encoder parameters.");
        return result;
    }
    stream->time_base = codecContext->time_base;

    if (!(formatContext->oformat->flags & AVFMT_NOFILE)) {
        const int ioResult = avio_open(&formatContext->pb, outputPathBytes.constData(), AVIO_FLAG_WRITE);
        if (ioResult < 0) {
            result.message = QStringLiteral("Failed to open proxy output file: %1").arg(avErrToString(ioResult));
            return result;
        }
    }
    const int headerResult = avformat_write_header(formatContext.get(), nullptr);
    if (headerResult < 0) {
        result.message = QStringLiteral("Failed to write proxy header: %1").arg(avErrToString(headerResult));
        return result;
    }

    std::unique_ptr<AVFrame, AvFrameDeleter> frame(av_frame_alloc());
    std::unique_ptr<AVPacket, AvPacketDeleter> packet(av_packet_alloc());
    if (!frame || !packet) {
        result.message = QStringLiteral("Failed to allocate proxy frame/packet.");
        return result;
    }
    frame->format = codecContext->pix_fmt;
    frame->width = codecContext->width;
    frame->height = codecContext->height;
    if (av_frame_get_buffer(frame.get(), 32) < 0) {
        result.message = QStringLiteral("Failed to allocate proxy frame buffer.");
        return result;
    }

    std::unique_ptr<SwsContext, SwsContextDeleter> scaleContext(
        sws_getContext(outputSize.width(),
                       outputSize.height(),
                       AV_PIX_FMT_RGB24,
                       outputSize.width(),
                       outputSize.height(),
                       codecContext->pix_fmt,
                       SWS_BILINEAR,
                       nullptr,
                       nullptr,
                       nullptr));
    if (!scaleContext) {
        result.message = QStringLiteral("Failed to create proxy color converter.");
        return result;
    }

    log(QStringLiteral("Internal container proxy generation\n"));
    log(QStringLiteral("Source: %1\n").arg(QDir::toNativeSeparators(sourcePath)));
    log(QStringLiteral("Output: %1\n").arg(QDir::toNativeSeparators(outputPath)));
    log(QStringLiteral("Decode: explicit software path\n"));
    log(QStringLiteral("Encode: %1 software encoder, %2x%3 @ %4 fps\n")
            .arg(encoderLabel)
            .arg(outputSize.width())
            .arg(outputSize.height())
            .arg(proxyFps));
    log(QStringLiteral("Audio: original media remains the audio source during proxy playback\n"));

    for (int frameIndex = 0; frameIndex < totalFrames; ++frameIndex) {
        if (cancelFlag && cancelFlag->load()) {
            result.canceled = true;
            result.message = QStringLiteral("Proxy generation canceled.");
            return result;
        }

        QImage image;
        if (frameIndex == 0) {
            image = firstImage;
        } else {
            const editor::FrameHandle decoded = decoder->decodeFrame(frameIndex);
            if (decoded.isNull() || !decoded.hasCpuImage()) {
                result.message = QStringLiteral("Software decoder returned no frame at source frame %1.")
                                     .arg(frameIndex);
                return result;
            }
            image = proxyScaledImage(decoded.cpuImage());
            if (image.size() != outputSize) {
                image = image.scaled(outputSize, Qt::IgnoreAspectRatio, Qt::SmoothTransformation);
            }
        }
        if (image.format() != QImage::Format_RGB888) {
            image = image.convertToFormat(QImage::Format_RGB888);
        }
        if (image.isNull()) {
            result.message = QStringLiteral("Failed to prepare proxy frame %1.").arg(frameIndex + 1);
            return result;
        }

        if (av_frame_make_writable(frame.get()) < 0) {
            result.message = QStringLiteral("Failed to make proxy frame writable.");
            return result;
        }
        const uint8_t* sourceData[4] = {image.constBits(), nullptr, nullptr, nullptr};
        int sourceLinesize[4] = {static_cast<int>(image.bytesPerLine()), 0, 0, 0};
        if (sws_scale(scaleContext.get(),
                      sourceData,
                      sourceLinesize,
                      0,
                      outputSize.height(),
                      frame->data,
                      frame->linesize) <= 0) {
            result.message = QStringLiteral("Failed to convert proxy frame %1.").arg(frameIndex + 1);
            return result;
        }

        frame->pts = frameIndex;
        const int sendResult = avcodec_send_frame(codecContext.get(), frame.get());
        if (sendResult < 0) {
            result.message = QStringLiteral("Failed to submit proxy frame %1: %2")
                                 .arg(frameIndex + 1)
                                 .arg(avErrToString(sendResult));
            return result;
        }
        for (;;) {
            const int receiveResult = avcodec_receive_packet(codecContext.get(), packet.get());
            if (receiveResult == AVERROR(EAGAIN) || receiveResult == AVERROR_EOF) {
                break;
            }
            if (receiveResult < 0) {
                result.message = QStringLiteral("Failed to receive proxy packet: %1")
                                     .arg(avErrToString(receiveResult));
                return result;
            }
            av_packet_rescale_ts(packet.get(), codecContext->time_base, stream->time_base);
            packet->stream_index = stream->index;
            const int writeResult = av_interleaved_write_frame(formatContext.get(), packet.get());
            av_packet_unref(packet.get());
            if (writeResult < 0) {
                result.message = QStringLiteral("Failed to write proxy packet: %1")
                                     .arg(avErrToString(writeResult));
                return result;
            }
        }

        ++result.framesWritten;
        if (result.framesWritten == 1 || result.framesWritten % 15 == 0) {
            log(QStringLiteral("Encoded frame %1/%2\n").arg(frameIndex + 1).arg(totalFrames));
        }
        progress(qBound(0,
                        qRound((static_cast<double>(frameIndex + 1) /
                                static_cast<double>(qMax(1, totalFrames))) * 100.0),
                        100));
    }

    QString flushError;
    if (!flushProxyEncoder(codecContext.get(), formatContext.get(), stream, &flushError)) {
        result.message = flushError;
        return result;
    }
    const int trailerResult = av_write_trailer(formatContext.get());
    if (trailerResult < 0) {
        result.message = QStringLiteral("Failed to finalize proxy output: %1").arg(avErrToString(trailerResult));
        return result;
    }
    result.success = true;
    result.message = QStringLiteral("Proxy generation finished. Frames encoded: %1.")
                         .arg(result.framesWritten);
    progress(100);
    return result;
}

bool moveDirectoryEntry(const std::filesystem::path& source,
                        const std::filesystem::path& destination,
                        QString* errorOut) {
    std::error_code ec;
    if (std::filesystem::exists(destination, ec)) {
        if (errorOut) {
            *errorOut = QStringLiteral("Destination already contains: %1")
                            .arg(QString::fromStdString(destination.string()));
        }
        return false;
    }

    std::filesystem::rename(source, destination, ec);
    if (!ec) {
        return true;
    }

    ec.clear();
    const auto copyOptions = std::filesystem::copy_options::recursive |
                             std::filesystem::copy_options::copy_symlinks;
    std::filesystem::copy(source, destination, copyOptions, ec);
    if (ec) {
        if (errorOut) {
            *errorOut = QString::fromStdString(ec.message());
        }
        return false;
    }

    ec.clear();
    std::filesystem::remove_all(source, ec);
    if (ec) {
        if (errorOut) {
            *errorOut = QStringLiteral("Copied but could not remove old cache item: %1")
                            .arg(QString::fromStdString(ec.message()));
        }
        return false;
    }
    return true;
}

bool moveDirectoryContents(const QString& sourcePath,
                           const QString& destinationPath,
                           QString* errorOut) {
    const std::filesystem::path source = fsPathForQtPath(sourcePath);
    const std::filesystem::path destination = fsPathForQtPath(destinationPath);
    std::error_code ec;
    std::filesystem::create_directories(destination, ec);
    if (ec) {
        if (errorOut) {
            *errorOut = QString::fromStdString(ec.message());
        }
        return false;
    }

    std::vector<std::filesystem::path> entries;
    for (std::filesystem::directory_iterator it(source, ec), end; it != end && !ec; ++it) {
        entries.push_back(it->path());
    }
    if (ec) {
        if (errorOut) {
            *errorOut = QString::fromStdString(ec.message());
        }
        return false;
    }

    for (const std::filesystem::path& entry : entries) {
        if (!moveDirectoryEntry(entry, destination / entry.filename(), errorOut)) {
            return false;
        }
    }
    return true;
}

QString extractJsonObject(const QString& text) {
    const int start = text.indexOf(QLatin1Char('{'));
    const int end = text.lastIndexOf(QLatin1Char('}'));
    if (start < 0 || end < start) {
        return QString();
    }
    return text.mid(start, (end - start) + 1);
}

bool parseSyncAction(const QString& value, RenderSyncAction* actionOut) {
    if (!actionOut) {
        return false;
    }
    const QString normalized = value.trimmed().toLower();
    if (normalized == QStringLiteral("duplicate") || normalized == QStringLiteral("dup")) {
        *actionOut = RenderSyncAction::DuplicateFrame;
        return true;
    }
    if (normalized == QStringLiteral("skip")) {
        *actionOut = RenderSyncAction::SkipFrame;
        return true;
    }
    return false;
}

QString syncAudioPathForClip(const TimelineClip& clip) {
    if (!clip.audioSourcePath.trimmed().isEmpty()) {
        const QFileInfo audioInfo(clip.audioSourcePath);
        if (audioInfo.exists() && audioInfo.isFile()) {
            return audioInfo.absoluteFilePath();
        }
    }
    const QString mediaPath = playbackMediaPathForClip(clip);
    const QFileInfo mediaInfo(mediaPath);
    return mediaInfo.exists() && mediaInfo.isFile() ? mediaInfo.absoluteFilePath() : QString();
}

double syncDetectionConfidence(const QJsonObject& root) {
    const QJsonObject metadata = root.value(QStringLiteral("metadata")).toObject();
    return metadata.value(QStringLiteral("confidence")).toDouble(0.0);
}

struct ShellRunResult {
    int exitCode = -1;
    QProcess::ExitStatus exitStatus = QProcess::CrashExit;
    QString output;
    QString errorString;
    bool canceled = false;
};

ShellRunResult runShellCommandStreaming(const QString& command,
                                        std::atomic_bool* cancelFlag,
                                        const std::function<void(const QString&)>& logFn,
                                        int timeoutMs = -1) {
    ShellRunResult result;
    QProcess process;
    process.setProcessChannelMode(QProcess::MergedChannels);
    if (logFn) {
        logFn(QStringLiteral("$ %1\n").arg(command));
    }
    process.start(QStringLiteral("/bin/bash"), {QStringLiteral("-lc"), command});
    if (!process.waitForStarted(5000)) {
        result.errorString = QStringLiteral("failed to start shell command");
        return result;
    }

    auto flushOutput = [&]() {
        const QByteArray chunk = process.readAllStandardOutput();
        if (!chunk.isEmpty()) {
            const QString text = QString::fromLocal8Bit(chunk);
            result.output += text;
            if (logFn) {
                logFn(text);
            }
        }
    };

    if (timeoutMs < 0) {
        while (!process.waitForFinished(120)) {
            flushOutput();
            if (cancelFlag && cancelFlag->load()) {
                process.kill();
                process.waitForFinished(2000);
                result.canceled = true;
                break;
            }
        }
        flushOutput();
    } else {
        process.waitForFinished(timeoutMs);
        flushOutput();
    }

    result.exitCode = process.exitCode();
    result.exitStatus = process.exitStatus();
    result.errorString = process.errorString();
    return result;
}

} // namespace

void EditorWindow::openTranscriptionWindow(const QString &filePath, const QString &label)
{
    const QFileInfo inputInfo(filePath);
    if (!inputInfo.exists() || !inputInfo.isFile())
    {
        QMessageBox::warning(this,
                             QStringLiteral("Transcribe Failed"),
                             QStringLiteral("The selected file does not exist:\n%1")
                                 .arg(QDir::toNativeSeparators(filePath)));
        return;
    }

    const QString scriptPath = QDir(QDir::currentPath()).absoluteFilePath(QStringLiteral("whisperx.sh"));
    if (!QFileInfo::exists(scriptPath))
    {
        QMessageBox::warning(this,
                             QStringLiteral("Transcribe Failed"),
                             QStringLiteral("whisperx.sh was not found at:\n%1")
                                 .arg(QDir::toNativeSeparators(scriptPath)));
        return;
    }

    auto *dialog = new QDialog(this);
    dialog->setAttribute(Qt::WA_DeleteOnClose);
    dialog->setWindowTitle(QStringLiteral("Transcribe  %1").arg(label));
    dialog->resize(920, 560);

    auto *layout = new QVBoxLayout(dialog);
    layout->setContentsMargins(12, 12, 12, 12);
    layout->setSpacing(8);

    auto *title = new QLabel(QStringLiteral("whisperx.sh %1").arg(QDir::toNativeSeparators(filePath)), dialog);
    title->setWordWrap(true);
    layout->addWidget(title);

    auto *output = new QPlainTextEdit(dialog);
    output->setReadOnly(true);
    output->setLineWrapMode(QPlainTextEdit::NoWrap);
    output->setStyleSheet(QStringLiteral(
        "QPlainTextEdit { background: #05080c; color: #d6e2ee; border: 1px solid #24303c; "
        "border-radius: 8px; font-family: monospace; font-size: 12px; }"));
    layout->addWidget(output, 1);

    auto *autoScrollBox = new QCheckBox(QStringLiteral("Auto-scroll"), dialog);
    autoScrollBox->setChecked(true);
    layout->addWidget(autoScrollBox);

    auto *inputRow = new QHBoxLayout;
    inputRow->setContentsMargins(0, 0, 0, 0);
    inputRow->setSpacing(8);
    auto *inputLabel = new QLabel(QStringLiteral("stdin"), dialog);
    auto *inputLine = new QLineEdit(dialog);
    inputLine->setPlaceholderText(QStringLiteral("Type input for whisperx.sh prompts, then press Send"));
    auto *sendButton = new QPushButton(QStringLiteral("Send"), dialog);
    auto *closeButton = new QPushButton(QStringLiteral("Close"), dialog);
    inputRow->addWidget(inputLabel);
    inputRow->addWidget(inputLine, 1);
    inputRow->addWidget(sendButton);
    inputRow->addWidget(closeButton);
    layout->addLayout(inputRow);

    auto *process = new QProcess(dialog);
    process->setProcessChannelMode(QProcess::MergedChannels);
    process->setWorkingDirectory(QDir::currentPath());

    const auto appendOutput = [output, autoScrollBox](const QString &text)
    {
        if (text.isEmpty()) return;
        if (autoScrollBox->isChecked()) output->moveCursor(QTextCursor::End);
        output->insertPlainText(text);
        if (autoScrollBox->isChecked()) output->moveCursor(QTextCursor::End);
    };

    connect(process, &QProcess::readyReadStandardOutput, dialog, [process, appendOutput]()
            { appendOutput(QString::fromLocal8Bit(process->readAllStandardOutput())); });
    connect(process, &QProcess::started, dialog, [appendOutput, filePath]()
            { appendOutput(QStringLiteral("$ ./whisperx.sh \"%1\"\n").arg(QDir::toNativeSeparators(filePath))); });
    connect(process, &QProcess::errorOccurred, dialog, [appendOutput](QProcess::ProcessError error)
            { appendOutput(QStringLiteral("\n[process error] %1\n").arg(static_cast<int>(error))); });
    connect(process, qOverload<int, QProcess::ExitStatus>(&QProcess::finished), dialog,
            [appendOutput](int exitCode, QProcess::ExitStatus exitStatus)
            {
                appendOutput(QStringLiteral("\n[process finished] exitCode=%1 status=%2\n")
                                 .arg(exitCode)
                                 .arg(exitStatus == QProcess::NormalExit ? QStringLiteral("normal") : QStringLiteral("crashed")));
            });
    connect(sendButton, &QPushButton::clicked, dialog, [process, inputLine, appendOutput]()
            {
                const QString text = inputLine->text();
                if (text.isEmpty()) return;
                process->write(text.toUtf8());
                process->write("\n");
                appendOutput(QStringLiteral("> %1\n").arg(text));
                inputLine->clear();
            });
    connect(inputLine, &QLineEdit::returnPressed, sendButton, &QPushButton::click);
    connect(closeButton, &QPushButton::clicked, dialog, &QDialog::close);
    connect(dialog, &QDialog::finished, dialog, [process](int)
            {
                if (process->state() != QProcess::NotRunning) {
                    process->kill();
                    process->waitForFinished(1000);
                }
            });

    process->start(QStringLiteral("/bin/bash"), {scriptPath, QFileInfo(filePath).absoluteFilePath()});
    dialog->show();
}

void EditorWindow::openSamDetectorWindow(const QString& clipId)
{
    if (!m_timeline) {
        return;
    }

    const TimelineClip* clip = nullptr;
    for (const TimelineClip& candidate : m_timeline->clips()) {
        if (candidate.id == clipId) {
            clip = &candidate;
            break;
        }
    }
    if (!clip) {
        QMessageBox::warning(this,
                             QStringLiteral("Detect Failed"),
                             QStringLiteral("The selected clip no longer exists."));
        return;
    }
    if (clip->mediaType != ClipMediaType::Video) {
        QMessageBox::warning(this,
                             QStringLiteral("Detect Failed"),
                             QStringLiteral("Detect is only available for video clips."));
        return;
    }

    const QFileInfo inputInfo(clip->filePath);
    if (!inputInfo.exists() || !inputInfo.isFile()) {
        QMessageBox::warning(this,
                             QStringLiteral("Detect Failed"),
                             QStringLiteral("The selected video file does not exist:\n%1")
                                 .arg(QDir::toNativeSeparators(clip->filePath)));
        return;
    }

    const QString scriptPath = QDir(QDir::currentPath()).absoluteFilePath(QStringLiteral("sam3.sh"));
    if (!QFileInfo::exists(scriptPath)) {
        QMessageBox::warning(this,
                             QStringLiteral("Detect Failed"),
                             QStringLiteral("sam3.sh was not found at:\n%1")
                                 .arg(QDir::toNativeSeparators(scriptPath)));
        return;
    }

    QSettings settings(QStringLiteral("PanelTalkEditor"), QStringLiteral("JCut"));
    const QString savedModelCachePath =
        settings.value(QStringLiteral("sam3/modelCachePath"), defaultSam3ModelCachePath())
            .toString();
    const QString savedRuntimeCachePath =
        settings.value(QStringLiteral("sam3/runtimeCachePath"), defaultSam3RuntimeCachePath(savedModelCachePath))
            .toString();

    QDialog preflight(this);
    preflight.setWindowTitle(QStringLiteral("Detect Preflight"));
    auto* preflightLayout = new QVBoxLayout(&preflight);
    preflightLayout->setContentsMargins(12, 12, 12, 12);
    preflightLayout->setSpacing(8);

    auto* inputLabel = new QLabel(
        QStringLiteral("Input: %1").arg(QDir::toNativeSeparators(inputInfo.absoluteFilePath())),
        &preflight);
    inputLabel->setWordWrap(true);
    preflightLayout->addWidget(inputLabel);

    auto* promptLabel = new QLabel(QStringLiteral("Prompt"), &preflight);
    preflightLayout->addWidget(promptLabel);

    auto* promptEdit = new QLineEdit(
        QStringLiteral("a microphone mounted on a microphone stand"), &preflight);
    promptEdit->setClearButtonEnabled(true);
    promptEdit->selectAll();
    preflightLayout->addWidget(promptEdit);

    auto* cacheLabel = new QLabel(QStringLiteral("Model cache"), &preflight);
    preflightLayout->addWidget(cacheLabel);

    auto* cacheRow = new QHBoxLayout;
    cacheRow->setContentsMargins(0, 0, 0, 0);
    cacheRow->setSpacing(8);
    auto* cacheEdit = new QLineEdit(savedModelCachePath, &preflight);
    cacheEdit->setClearButtonEnabled(true);
    auto* browseCacheButton = new QPushButton(QStringLiteral("Browse"), &preflight);
    cacheRow->addWidget(cacheEdit, 1);
    cacheRow->addWidget(browseCacheButton);
    preflightLayout->addLayout(cacheRow);

    auto* runtimeCacheLabel = new QLabel(QStringLiteral("Runtime cache"), &preflight);
    preflightLayout->addWidget(runtimeCacheLabel);

    auto* runtimeCacheRow = new QHBoxLayout;
    runtimeCacheRow->setContentsMargins(0, 0, 0, 0);
    runtimeCacheRow->setSpacing(8);
    auto* runtimeCacheEdit = new QLineEdit(savedRuntimeCachePath, &preflight);
    runtimeCacheEdit->setClearButtonEnabled(true);
    runtimeCacheEdit->setToolTip(
        QStringLiteral("Docker runtime cache root for pip, container home, torch kernels, and related temporary model runtime data."));
    auto* browseRuntimeCacheButton = new QPushButton(QStringLiteral("Browse"), &preflight);
    runtimeCacheRow->addWidget(runtimeCacheEdit, 1);
    runtimeCacheRow->addWidget(browseRuntimeCacheButton);
    preflightLayout->addLayout(runtimeCacheRow);

    auto* performanceForm = new QFormLayout;
    performanceForm->setContentsMargins(0, 0, 0, 0);
    performanceForm->setSpacing(8);

    auto* scaleWidthSpin = new QSpinBox(&preflight);
    scaleWidthSpin->setRange(0, 8192);
    scaleWidthSpin->setSingleStep(64);
    scaleWidthSpin->setSpecialValueText(QStringLiteral("Original"));
    scaleWidthSpin->setValue(settings.value(QStringLiteral("sam3/scaleWidth"), 0).toInt());
    scaleWidthSpin->setToolTip(
        QStringLiteral("Resize frames to this width during SAM processing. Lower values are faster but less detailed."));
    performanceForm->addRow(QStringLiteral("Scale width"), scaleWidthSpin);

    auto* prescaleWidthSpin = new QSpinBox(&preflight);
    prescaleWidthSpin->setRange(0, 8192);
    prescaleWidthSpin->setSingleStep(64);
    prescaleWidthSpin->setSpecialValueText(QStringLiteral("Disabled"));
    prescaleWidthSpin->setValue(settings.value(QStringLiteral("sam3/prescaleWidth"), 0).toInt());
    prescaleWidthSpin->setToolTip(
        QStringLiteral("Pre-encode a resized temporary video before SAM. Useful for long jobs when using a lower working resolution."));
    performanceForm->addRow(QStringLiteral("Prescale width"), prescaleWidthSpin);

    auto* extractFpsSpin = new QDoubleSpinBox(&preflight);
    extractFpsSpin->setRange(0.0, 240.0);
    extractFpsSpin->setDecimals(3);
    extractFpsSpin->setSingleStep(1.0);
    extractFpsSpin->setSpecialValueText(QStringLiteral("Source"));
    extractFpsSpin->setValue(settings.value(QStringLiteral("sam3/extractFps"), 0.0).toDouble());
    extractFpsSpin->setToolTip(
        QStringLiteral("Process only this many frames per second for diagnostic frame output. Timeline mask sidecars always use every source frame for exact synchronization."));
    performanceForm->addRow(QStringLiteral("Extract FPS"), extractFpsSpin);

    auto* frameFormatCombo = new QComboBox(&preflight);
    frameFormatCombo->addItem(QStringLiteral("JPEG"), QStringLiteral("jpg"));
    frameFormatCombo->addItem(QStringLiteral("PNG"), QStringLiteral("png"));
    const QString savedFrameFormat =
        settings.value(QStringLiteral("sam3/intermediateFramesFormat"), QStringLiteral("jpg"))
            .toString();
    const int savedFrameFormatIndex = frameFormatCombo->findData(savedFrameFormat);
    frameFormatCombo->setCurrentIndex(savedFrameFormatIndex >= 0 ? savedFrameFormatIndex : 0);
    frameFormatCombo->setToolTip(
        QStringLiteral("Intermediate extracted frame format. JPEG is smaller and usually faster; PNG is lossless but heavier."));
    performanceForm->addRow(QStringLiteral("Frame format"), frameFormatCombo);

    auto* compileModelCheckBox = new QCheckBox(QStringLiteral("Enable torch.compile"), &preflight);
    compileModelCheckBox->setChecked(
        settings.value(QStringLiteral("sam3/compileModel"), false).toBool());
    compileModelCheckBox->setToolTip(
        QStringLiteral("Compile supported SAM3 modules. This can improve long runs after warmup but may add startup time and memory use."));
    performanceForm->addRow(QString(), compileModelCheckBox);

    preflightLayout->addLayout(performanceForm);

    auto* binaryMasksCheckBox = new QCheckBox(QStringLiteral("Write binary mask frames"), &preflight);
    binaryMasksCheckBox->setChecked(true);
    binaryMasksCheckBox->setToolTip(
        QStringLiteral("Write per-frame PNG mattes for video processing. White pixels are detected regions; black pixels are background."));
    preflightLayout->addWidget(binaryMasksCheckBox);

    QString currentMaskDir;
    const TimelineClip* selectedMaskChild = m_timeline->selectedClip();
    if (selectedMaskChild &&
        selectedMaskChild->clipRole == ClipRole::MaskMatte &&
        selectedMaskChild->linkedSourceClipId.trimmed() == clipId.trimmed()) {
        currentMaskDir = selectedMaskChild->maskFramesDir.trimmed();
    }
    if (currentMaskDir.isEmpty()) {
        // Legacy projects may still carry the association on the source. New
        // materialization writes it only to the Mask Matte child.
        currentMaskDir = clip->maskFramesDir.trimmed();
    }
    const bool canUnionWithCurrentMask = !currentMaskDir.isEmpty() &&
        !QDir(currentMaskDir).entryList(
            QStringList{QStringLiteral("frame_*.png")}, QDir::Files, QDir::Name).isEmpty();
    auto* unionCurrentMaskCheckBox = new QCheckBox(
        QStringLiteral("Union with current mask"), &preflight);
    unionCurrentMaskCheckBox->setChecked(canUnionWithCurrentMask);
    unionCurrentMaskCheckBox->setEnabled(canUnionWithCurrentMask);
    unionCurrentMaskCheckBox->setToolTip(canUnionWithCurrentMask
        ? QStringLiteral("Keep the new prompt as a separate sidecar and also create a third sidecar containing current OR new.")
        : QStringLiteral("The selected clip does not have an existing binary-mask sidecar to combine."));
    preflightLayout->addWidget(unionCurrentMaskCheckBox);

    auto* videoModeCheckBox =
        new QCheckBox(QStringLiteral("Run SAM video mode"), &preflight);
    videoModeCheckBox->setChecked(false);
    videoModeCheckBox->setToolTip(
        QStringLiteral("Run SAM on the video stream instead of extracting and processing individual frames. Frame-only exports are disabled in this mode."));
    preflightLayout->addWidget(videoModeCheckBox);

    auto* maskPreviewFramesCheckBox =
        new QCheckBox(QStringLiteral("Write masked preview frames"), &preflight);
    maskPreviewFramesCheckBox->setChecked(false);
    maskPreviewFramesCheckBox->setToolTip(
        QStringLiteral("Write diagnostic frames showing the original frame with the detection mask overlay applied."));
    preflightLayout->addWidget(maskPreviewFramesCheckBox);

    auto* centersJsonCheckBox =
        new QCheckBox(QStringLiteral("Export centers JSONL"), &preflight);
    centersJsonCheckBox->setChecked(false);
    centersJsonCheckBox->setToolTip(
        QStringLiteral("Write a standalone JSONL file with per-frame detection center records."));
    preflightLayout->addWidget(centersJsonCheckBox);

    auto* rootModeCheckBox =
        new QCheckBox(QStringLiteral("Run Docker container as root"), &preflight);
    rootModeCheckBox->setChecked(false);
    rootModeCheckBox->setToolTip(
        QStringLiteral("Run the SAM Docker container as root. Use this only to work around ownership or permission problems."));
    preflightLayout->addWidget(rootModeCheckBox);

    auto* preflightButtonRow = new QHBoxLayout;
    auto* cancelPreflightButton = new QPushButton(QStringLiteral("Cancel"), &preflight);
    auto* runPreflightButton = new QPushButton(QStringLiteral("Run"), &preflight);
    runPreflightButton->setDefault(true);
    preflightButtonRow->addStretch(1);
    preflightButtonRow->addWidget(cancelPreflightButton);
    preflightButtonRow->addWidget(runPreflightButton);
    preflightLayout->addLayout(preflightButtonRow);

    connect(browseCacheButton, &QPushButton::clicked, &preflight, [cacheEdit, &preflight]() {
        const QString selectedPath = QFileDialog::getExistingDirectory(
            &preflight,
            QStringLiteral("Choose SAM3 Model Cache"),
            expandUserPath(cacheEdit->text().trimmed().isEmpty()
                               ? defaultSam3ModelCachePath()
                               : cacheEdit->text()),
            QFileDialog::ShowDirsOnly | QFileDialog::DontResolveSymlinks);
        if (!selectedPath.isEmpty()) {
            cacheEdit->setText(selectedPath);
        }
    });
    connect(browseRuntimeCacheButton, &QPushButton::clicked, &preflight, [runtimeCacheEdit, cacheEdit, &preflight]() {
        const QString selectedPath = QFileDialog::getExistingDirectory(
            &preflight,
            QStringLiteral("Choose SAM3 Runtime Cache"),
            expandUserPath(runtimeCacheEdit->text().trimmed().isEmpty()
                               ? defaultSam3RuntimeCachePath(cacheEdit->text())
                               : runtimeCacheEdit->text()),
            QFileDialog::ShowDirsOnly | QFileDialog::DontResolveSymlinks);
        if (!selectedPath.isEmpty()) {
            runtimeCacheEdit->setText(selectedPath);
        }
    });
    connect(cancelPreflightButton, &QPushButton::clicked, &preflight, &QDialog::reject);
    connect(videoModeCheckBox, &QCheckBox::toggled, &preflight,
            [binaryMasksCheckBox, unionCurrentMaskCheckBox, canUnionWithCurrentMask,
             maskPreviewFramesCheckBox, extractFpsSpin, frameFormatCombo](bool checked) {
        binaryMasksCheckBox->setEnabled(!checked);
        unionCurrentMaskCheckBox->setEnabled(!checked && canUnionWithCurrentMask);
        maskPreviewFramesCheckBox->setEnabled(!checked);
        extractFpsSpin->setEnabled(!checked);
        frameFormatCombo->setEnabled(!checked);
        if (checked) {
            binaryMasksCheckBox->setChecked(false);
            unionCurrentMaskCheckBox->setChecked(false);
            maskPreviewFramesCheckBox->setChecked(false);
        } else {
            binaryMasksCheckBox->setChecked(true);
            unionCurrentMaskCheckBox->setChecked(canUnionWithCurrentMask);
        }
    });
    auto refreshExtractFpsAvailability = [videoModeCheckBox,
                                          binaryMasksCheckBox,
                                          extractFpsSpin]() {
        extractFpsSpin->setEnabled(
            !videoModeCheckBox->isChecked() && !binaryMasksCheckBox->isChecked());
    };
    connect(binaryMasksCheckBox, &QCheckBox::toggled, &preflight,
            [&refreshExtractFpsAvailability](bool) {
        refreshExtractFpsAvailability();
    });
    connect(videoModeCheckBox, &QCheckBox::toggled, &preflight,
            [&refreshExtractFpsAvailability](bool) {
        refreshExtractFpsAvailability();
    });
    refreshExtractFpsAvailability();
    connect(runPreflightButton, &QPushButton::clicked, &preflight, [&preflight, promptEdit, cacheEdit, runtimeCacheEdit]() {
        if (promptEdit->text().trimmed().isEmpty()) {
            promptEdit->setFocus();
            return;
        }
        if (cacheEdit->text().trimmed().isEmpty()) {
            cacheEdit->setFocus();
            return;
        }
        if (runtimeCacheEdit->text().trimmed().isEmpty()) {
            runtimeCacheEdit->setFocus();
            return;
        }
        preflight.accept();
    });
    if (preflight.exec() != QDialog::Accepted) {
        return;
    }
    const QString prompt = promptEdit->text().trimmed();
    const bool useVideoMode = videoModeCheckBox->isChecked();
    const bool writeBinaryMasks = !useVideoMode && binaryMasksCheckBox->isChecked();
    const bool unionWithCurrentMask =
        writeBinaryMasks && canUnionWithCurrentMask && unionCurrentMaskCheckBox->isChecked();
    const bool writeMaskPreviewFrames = !useVideoMode && maskPreviewFramesCheckBox->isChecked();
    const bool exportCentersJson = centersJsonCheckBox->isChecked();
    const bool runDockerAsRoot = rootModeCheckBox->isChecked();
    const int scaleWidth = scaleWidthSpin->value();
    const int prescaleWidth = prescaleWidthSpin->value();
    const double extractFps = extractFpsSpin->value();
    const QString intermediateFramesFormat = frameFormatCombo->currentData().toString();
    const bool compileModel = compileModelCheckBox->isChecked();
    const QString previousModelCachePath =
        QFileInfo(expandUserPath(savedModelCachePath)).absoluteFilePath();
    const QString modelCachePath =
        QFileInfo(expandUserPath(cacheEdit->text())).absoluteFilePath();
    const QString runtimeCachePath =
        QFileInfo(expandUserPath(runtimeCacheEdit->text())).absoluteFilePath();
    QDir modelCacheDir(modelCachePath);
    if (!modelCacheDir.exists() && !modelCacheDir.mkpath(QStringLiteral("."))) {
        QMessageBox::warning(this,
                             QStringLiteral("Detect Failed"),
                             QStringLiteral("Could not create the SAM3 model cache:\n%1")
                                 .arg(QDir::toNativeSeparators(modelCachePath)));
        return;
    }
    QDir runtimeCacheDir(runtimeCachePath);
    if (!runtimeCacheDir.exists() && !runtimeCacheDir.mkpath(QStringLiteral("."))) {
        QMessageBox::warning(this,
                             QStringLiteral("Detect Failed"),
                             QStringLiteral("Could not create the SAM3 runtime cache:\n%1")
                                 .arg(QDir::toNativeSeparators(runtimeCachePath)));
        return;
    }
    const std::filesystem::path previousModelCacheFsPath =
        fsPathForQtPath(previousModelCachePath);
    const std::filesystem::path modelCacheFsPath = fsPathForQtPath(modelCachePath);
    if (!pathsEqual(previousModelCacheFsPath, modelCacheFsPath) &&
        directoryHasEntries(previousModelCachePath)) {
        if (pathContains(previousModelCacheFsPath, modelCacheFsPath) ||
            pathContains(modelCacheFsPath, previousModelCacheFsPath)) {
            QMessageBox::warning(
                this,
                QStringLiteral("Detect Failed"),
                QStringLiteral("Choose a model cache folder outside the current cache tree.\n\nCurrent: %1\nSelected: %2")
                    .arg(QDir::toNativeSeparators(previousModelCachePath),
                         QDir::toNativeSeparators(modelCachePath)));
            return;
        }

        QMessageBox movePrompt(this);
        movePrompt.setIcon(QMessageBox::Question);
        movePrompt.setWindowTitle(QStringLiteral("Move SAM3 Models"));
        movePrompt.setText(QStringLiteral("Move the existing SAM3 model cache to the selected folder?"));
        movePrompt.setInformativeText(
            QStringLiteral("Old cache:\n%1\n\nNew cache:\n%2")
                .arg(QDir::toNativeSeparators(previousModelCachePath),
                     QDir::toNativeSeparators(modelCachePath)));
        QPushButton* moveButton =
            movePrompt.addButton(QStringLiteral("Move Models"), QMessageBox::AcceptRole);
        QPushButton* useSelectedButton =
            movePrompt.addButton(QStringLiteral("Use Selected Folder"), QMessageBox::DestructiveRole);
        movePrompt.addButton(QMessageBox::Cancel);
        movePrompt.setDefaultButton(moveButton);
        movePrompt.exec();
        if (movePrompt.clickedButton() == nullptr ||
            movePrompt.standardButton(movePrompt.clickedButton()) == QMessageBox::Cancel) {
            return;
        }
        if (movePrompt.clickedButton() == moveButton) {
            QString moveError;
            if (!moveDirectoryContents(previousModelCachePath, modelCachePath, &moveError)) {
                QMessageBox::warning(
                    this,
                    QStringLiteral("Detect Failed"),
                    QStringLiteral("Could not move the SAM3 model cache:\n%1")
                        .arg(moveError));
                return;
            }
        } else if (movePrompt.clickedButton() != useSelectedButton) {
            return;
        }
    }
    settings.setValue(QStringLiteral("sam3/modelCachePath"), modelCachePath);
    settings.setValue(QStringLiteral("sam3/runtimeCachePath"), runtimeCachePath);
    settings.setValue(QStringLiteral("sam3/scaleWidth"), scaleWidth);
    settings.setValue(QStringLiteral("sam3/prescaleWidth"), prescaleWidth);
    settings.setValue(QStringLiteral("sam3/extractFps"), extractFps);
    settings.setValue(QStringLiteral("sam3/intermediateFramesFormat"), intermediateFramesFormat);
    settings.setValue(QStringLiteral("sam3/compileModel"), compileModel);

    QStringList samOptimizationArgs;
    if (scaleWidth > 0) {
        samOptimizationArgs << QStringLiteral("--scale-width") << QString::number(scaleWidth);
    }
    if (prescaleWidth > 0) {
        samOptimizationArgs << QStringLiteral("--prescale-width") << QString::number(prescaleWidth);
    }
    if (!useVideoMode && !writeBinaryMasks && extractFps > 0.0) {
        samOptimizationArgs << QStringLiteral("--extract-fps")
                            << QString::number(extractFps, 'f', 3);
    }
    if (!useVideoMode && !intermediateFramesFormat.isEmpty() &&
        intermediateFramesFormat != QStringLiteral("jpg")) {
        samOptimizationArgs << QStringLiteral("--intermediate-frames-format")
                            << intermediateFramesFormat;
    }
    if (compileModel) {
        samOptimizationArgs << QStringLiteral("--compile-model");
    }
    QString samOptimizationArgsPreview;
    for (const QString& arg : samOptimizationArgs) {
        samOptimizationArgsPreview += QStringLiteral(" ") + shellQuote(arg);
    }

    const QString samJobRoot =
        jcut::jobs::defaultJobRootForInput(inputInfo.absoluteFilePath(),
                                           QStringLiteral("sam3"),
                                           prompt);
    const QString samManifestPath = jcut::jobs::manifestPathForJobRoot(samJobRoot);
    const QString samPromptOutputStem =
        QStringLiteral("%1_sam3_%2")
            .arg(inputInfo.completeBaseName(),
                 jcut::jobs::sanitizedJobComponent(prompt));
    const QString centersPath =
        inputInfo.dir().absoluteFilePath(QStringLiteral("%1.jsonl")
                                             .arg(samPromptOutputStem));
    const QString binaryMasksPath =
        inputInfo.dir().absoluteFilePath(QStringLiteral("%1_binary_masks")
                                             .arg(samPromptOutputStem));
    QString currentMaskComponent = QFileInfo(currentMaskDir).fileName();
    currentMaskComponent.remove(QStringLiteral("%1_sam3_").arg(inputInfo.completeBaseName()));
    currentMaskComponent.remove(QStringLiteral("_binary_masks"));
    const QString combinedMasksPath = unionWithCurrentMask
        ? inputInfo.dir().absoluteFilePath(
              QStringLiteral("%1_sam3_%2_or_%3_binary_masks")
                  .arg(inputInfo.completeBaseName(),
                       jcut::jobs::sanitizedJobComponent(currentMaskComponent),
                       jcut::jobs::sanitizedJobComponent(prompt)))
        : QString();
    const QString defaultOutputPath =
        inputInfo.dir().absoluteFilePath(QStringLiteral("%1.mp4")
                                             .arg(samPromptOutputStem));
    QJsonObject artifacts{
        {QStringLiteral("job_root"), samJobRoot},
        {QStringLiteral("manifest"), samManifestPath},
        {QStringLiteral("centers_json"), exportCentersJson ? centersPath : QString()},
        {QStringLiteral("binary_masks_dir"), writeBinaryMasks ? binaryMasksPath : QString()},
        {QStringLiteral("combined_binary_masks_dir"), combinedMasksPath},
        {QStringLiteral("default_output_video"), defaultOutputPath},
        {QStringLiteral("model_cache"), modelCachePath},
        {QStringLiteral("runtime_cache"), runtimeCachePath},
    };
    QJsonObject parameters{
        {QStringLiteral("prompt"), prompt},
        {QStringLiteral("video_mode"), useVideoMode},
        {QStringLiteral("extract_frames"), !useVideoMode},
        {QStringLiteral("stream_extract"), !useVideoMode},
        {QStringLiteral("binary_masks"), writeBinaryMasks},
        {QStringLiteral("union_with_current_mask"), unionWithCurrentMask},
        {QStringLiteral("union_mask_dir"), unionWithCurrentMask ? currentMaskDir : QString()},
        {QStringLiteral("mask_preview_frames"), writeMaskPreviewFrames},
        {QStringLiteral("centers_json"), exportCentersJson},
        {QStringLiteral("docker_root_mode"), runDockerAsRoot},
        {QStringLiteral("scale_width"), scaleWidth},
        {QStringLiteral("prescale_width"), prescaleWidth},
        {QStringLiteral("extract_fps"), extractFps},
        {QStringLiteral("intermediate_frames_format"), intermediateFramesFormat},
        {QStringLiteral("compile_model"), compileModel},
    };
    if (QFileInfo::exists(samManifestPath)) {
        QJsonObject existingManifest;
        jcut::jobs::readManifest(samManifestPath, &existingManifest, nullptr);
        const QString existingStatus =
            existingManifest.value(QStringLiteral("status")).toString();
        if (existingStatus != QStringLiteral("completed")) {
            QMessageBox resumePrompt(this);
            resumePrompt.setIcon(QMessageBox::Question);
            resumePrompt.setWindowTitle(QStringLiteral("Resume SAM3 Job"));
            resumePrompt.setText(QStringLiteral("An unfinished SAM3 job exists for this input and prompt."));
            resumePrompt.setInformativeText(
                QStringLiteral("Job manifest:\n%1\n\nResume will continue from existing extracted frames and enabled outputs. Restart removes enabled outputs and the manifest first.")
                    .arg(QDir::toNativeSeparators(samManifestPath)));
            QPushButton* resumeButton =
                resumePrompt.addButton(QStringLiteral("Resume"), QMessageBox::AcceptRole);
            QPushButton* restartButton =
                resumePrompt.addButton(QStringLiteral("Restart"), QMessageBox::DestructiveRole);
            resumePrompt.addButton(QMessageBox::Cancel);
            resumePrompt.setDefaultButton(resumeButton);
            resumePrompt.exec();
            if (resumePrompt.clickedButton() == nullptr ||
                resumePrompt.standardButton(resumePrompt.clickedButton()) == QMessageBox::Cancel) {
                return;
            }
            if (resumePrompt.clickedButton() == restartButton) {
                if (exportCentersJson) {
                    QFile::remove(centersPath);
                }
                QFile::remove(samManifestPath);
                if (writeBinaryMasks) {
                    QDir(binaryMasksPath).removeRecursively();
                }
            }
        }
    }

    auto* dialog = new QDialog(this);
    dialog->setAttribute(Qt::WA_DeleteOnClose);
    dialog->setWindowTitle(QStringLiteral("Detect  %1").arg(clip->label));
    dialog->resize(920, 560);

    auto* layout = new QVBoxLayout(dialog);
    layout->setContentsMargins(12, 12, 12, 12);
    layout->setSpacing(8);

    auto* title = new QLabel(
        QStringLiteral("sam3.sh %1 --prompt %2\nModel cache: %3")
            .arg(QDir::toNativeSeparators(inputInfo.absoluteFilePath()),
                 prompt,
                 QDir::toNativeSeparators(modelCachePath)),
        dialog);
    title->setWordWrap(true);
    layout->addWidget(title);

    auto* output = new QPlainTextEdit(dialog);
    output->setReadOnly(true);
    output->setLineWrapMode(QPlainTextEdit::NoWrap);
    output->setStyleSheet(QStringLiteral(
        "QPlainTextEdit { background: #05080c; color: #d6e2ee; border: 1px solid #24303c; "
        "border-radius: 8px; font-family: monospace; font-size: 12px; }"));
    layout->addWidget(output, 1);

    auto* autoScrollBox = new QCheckBox(QStringLiteral("Auto-scroll"), dialog);
    autoScrollBox->setChecked(true);
    layout->addWidget(autoScrollBox);

    auto* backgroundButton = new QPushButton(QStringLiteral("Send to Background"), dialog);
    backgroundButton->setToolTip(
        QStringLiteral("Close this output window and keep the SAM job running. Reopen it from the Jobs tab."));
    auto* closeButton = new QPushButton(QStringLiteral("Close"), dialog);
    auto* buttonRow = new QHBoxLayout;
    buttonRow->addStretch(1);
    buttonRow->addWidget(backgroundButton);
    buttonRow->addWidget(closeButton);
    layout->addLayout(buttonRow);

    auto* process = new QProcess(dialog);
    auto* keepRunningOnClose = new bool(false);
    const QString selectedMaskPath = writeBinaryMasks
        ? (unionWithCurrentMask ? combinedMasksPath : binaryMasksPath)
        : QString();
    process->setProcessChannelMode(QProcess::MergedChannels);
    process->setWorkingDirectory(QDir::currentPath());
    QProcessEnvironment processEnv = QProcessEnvironment::systemEnvironment();
    processEnv.insert(QStringLiteral("SAM3_MODEL_CACHE"), modelCachePath);
    processEnv.insert(QStringLiteral("SAM3_RUNTIME_CACHE"), runtimeCachePath);
    processEnv.insert(QStringLiteral("SAM3_JOB_DIR"), samJobRoot);
    if (runDockerAsRoot) {
        processEnv.insert(QStringLiteral("SAM3_DOCKER_RUN_AS_ROOT"), QStringLiteral("1"));
    }
    process->setProcessEnvironment(processEnv);

    const auto appendOutput = [output, autoScrollBox](const QString& text) {
        if (text.isEmpty()) {
            return;
        }
        if (autoScrollBox->isChecked()) {
            output->moveCursor(QTextCursor::End);
        }
        output->insertPlainText(text);
        if (autoScrollBox->isChecked()) {
            output->moveCursor(QTextCursor::End);
        }
    };

    connect(process, &QProcess::readyReadStandardOutput, dialog, [process, appendOutput]() {
        appendOutput(QString::fromLocal8Bit(process->readAllStandardOutput()));
    });
    connect(process, &QProcess::started, dialog, [appendOutput,
                                                  inputPath = inputInfo.absoluteFilePath(),
                                                  prompt,
                                                  modelCachePath,
                                                  runtimeCachePath,
                                                  samJobRoot,
                                                  samManifestPath,
                                                  writeBinaryMasks,
                                                  binaryMasksPath,
                                                  writeMaskPreviewFrames,
                                                  exportCentersJson,
                                                  centersPath,
                                                  useVideoMode,
                                                  runDockerAsRoot,
                                                  samOptimizationArgsPreview]() {
        const QString videoModeArg = useVideoMode
            ? QStringLiteral(" --video-mode")
            : QString();
        const QString maskArg = writeBinaryMasks
            ? QStringLiteral(" --binary-mask-dir %1").arg(shellQuote(binaryMasksPath))
            : QString();
        const QString previewArg = writeMaskPreviewFrames
            ? QStringLiteral(" --write-mask-preview-frames")
            : QString();
        const QString centersArg = exportCentersJson
            ? QStringLiteral(" --centers-json %1").arg(shellQuote(centersPath))
            : QStringLiteral(" --no-centers-json");
        appendOutput(QStringLiteral("SAM3_MODEL_CACHE=%1\nSAM3_RUNTIME_CACHE=%2\nSAM3_JOB_DIR=%3\nSAM3_DOCKER_RUN_AS_ROOT=%4\njob_manifest=%5\nmode=%6\nbinary_masks=%7\nmask_preview_frames=%8\ncenters_json=%9\n$ ./sam3.sh \"%10\" --prompt %11%12%13%14%15%16\n")
                         .arg(QDir::toNativeSeparators(modelCachePath),
                              QDir::toNativeSeparators(runtimeCachePath),
                              QDir::toNativeSeparators(samJobRoot),
                              runDockerAsRoot ? QStringLiteral("1") : QStringLiteral("0"),
                              QDir::toNativeSeparators(samManifestPath),
                              useVideoMode ? QStringLiteral("video") : QStringLiteral("frames"),
                              writeBinaryMasks ? QDir::toNativeSeparators(binaryMasksPath) : QStringLiteral("disabled"),
                              writeMaskPreviewFrames ? QStringLiteral("enabled") : QStringLiteral("disabled"),
                              exportCentersJson ? QDir::toNativeSeparators(centersPath) : QStringLiteral("disabled"),
                              QDir::toNativeSeparators(inputPath),
                              shellQuote(prompt),
                              videoModeArg,
                              maskArg,
                              previewArg,
                              centersArg,
                              samOptimizationArgsPreview));
    });
    connect(process, &QProcess::errorOccurred, dialog, [appendOutput](QProcess::ProcessError error) {
        appendOutput(QStringLiteral("\n[process error] %1\n").arg(static_cast<int>(error)));
    });
    connect(process,
            qOverload<int, QProcess::ExitStatus>(&QProcess::finished),
            dialog,
            [appendOutput, samManifestPath](int exitCode, QProcess::ExitStatus exitStatus) {
                QString manifestError;
                QJsonObject patch{
                    {QStringLiteral("exit_code"), exitCode},
                    {QStringLiteral("exit_status"),
                     exitStatus == QProcess::NormalExit ? QStringLiteral("normal")
                                                        : QStringLiteral("crashed")},
                };
                jcut::jobs::updateManifestStatus(
                    samManifestPath,
                    (exitStatus == QProcess::NormalExit && exitCode == 0)
                        ? QStringLiteral("completed")
                        : QStringLiteral("failed"),
                    patch,
                    &manifestError);
                if (!manifestError.isEmpty()) {
                    appendOutput(QStringLiteral("\n[job manifest warning] %1\n").arg(manifestError));
                }
                appendOutput(QStringLiteral("\n[process finished] exitCode=%1 status=%2\n")
                                 .arg(exitCode)
                                 .arg(exitStatus == QProcess::NormalExit
                                          ? QStringLiteral("normal")
                                          : QStringLiteral("crashed")));
            });
    connect(process,
            qOverload<int, QProcess::ExitStatus>(&QProcess::finished),
            this,
            [this, clipId, selectedMaskPath](int exitCode,
                                             QProcess::ExitStatus exitStatus) {
                if (exitStatus != QProcess::NormalExit || exitCode != 0 || !m_timeline) return;
                // Materialize directly from the completed artifact. The source
                // parent never serves as temporary storage for child-owned
                // sidecar state. Reconciliation inside this model operation
                // also discovers any additional sibling sidecars.
                if (!selectedMaskPath.isEmpty()) {
                    m_timeline->createOrReplaceMaskMatteForSidecar(
                        clipId, selectedMaskPath, false);
                }
                if (m_inspectorPane) m_inspectorPane->refreshTab(QStringLiteral("Masks"));
            });
    connect(backgroundButton, &QPushButton::clicked, dialog, [dialog,
                                                              process,
                                                              keepRunningOnClose,
                                                              samManifestPath]() {
        *keepRunningOnClose = true;
        process->setParent(nullptr);
        QObject::connect(process,
                         qOverload<int, QProcess::ExitStatus>(&QProcess::finished),
                         process,
                         &QObject::deleteLater);
        jcut::jobs::updateManifestStatus(
            samManifestPath,
            QStringLiteral("running"),
            QJsonObject{{QStringLiteral("ui_state"), QStringLiteral("background")}},
            nullptr);
        dialog->close();
    });
    connect(closeButton, &QPushButton::clicked, dialog, &QDialog::close);
    connect(dialog, &QDialog::finished, dialog, [process,
                                                 samManifestPath,
                                                 keepRunningOnClose](int) {
        if (!*keepRunningOnClose && process->state() != QProcess::NotRunning) {
            jcut::jobs::updateManifestStatus(samManifestPath,
                                             QStringLiteral("paused"),
                                             QJsonObject{{QStringLiteral("pause_reason"),
                                                          QStringLiteral("ui_closed")}},
                                             nullptr);
            process->kill();
            process->waitForFinished(1000);
        }
        delete keepRunningOnClose;
    });

    QString manifestError;
    QStringList launchCommand{scriptPath,
                              inputInfo.absoluteFilePath(),
                              QStringLiteral("--prompt"),
                              prompt};
    if (useVideoMode) {
        launchCommand << QStringLiteral("--video-mode");
    }
    if (writeBinaryMasks) {
        launchCommand << QStringLiteral("--binary-mask-dir") << binaryMasksPath;
    }
    if (unionWithCurrentMask) {
        launchCommand << QStringLiteral("--union-mask-dir") << currentMaskDir
                      << QStringLiteral("--combined-binary-mask-dir") << combinedMasksPath;
    }
    if (writeMaskPreviewFrames) {
        launchCommand << QStringLiteral("--write-mask-preview-frames");
    }
    if (exportCentersJson) {
        launchCommand << QStringLiteral("--centers-json") << centersPath;
    } else {
        launchCommand << QStringLiteral("--no-centers-json");
    }
    launchCommand << samOptimizationArgs;
    QJsonObject manifest =
        jcut::jobs::makeManifest(QStringLiteral("sam3"),
                                 samJobRoot,
                                 inputInfo.absoluteFilePath(),
                                 parameters,
                                 artifacts,
                                 launchCommand);
    manifest.insert(QStringLiteral("status"), QStringLiteral("running"));
    if (!jcut::jobs::writeManifest(samManifestPath, manifest, &manifestError)) {
        QMessageBox::warning(this,
                             QStringLiteral("Detect Failed"),
                             QStringLiteral("Could not write SAM3 job manifest:\n%1")
                                 .arg(manifestError));
        return;
    }
    process->start(QStringLiteral("/bin/bash"), launchCommand);
    dialog->show();
}

QString EditorWindow::defaultProxyOutputPath(const TimelineClip &clip, const MediaProbeResult *knownProbe,
                                              ProxyFormat format) const
{
    const QFileInfo sourceInfo(clip.filePath);
    if (format == ProxyFormat::ImageSequence) {
        // Image sequence proxy: a directory named <basename>.proxy/ containing JPEG frames
        return sourceInfo.dir().absoluteFilePath(
            QStringLiteral("%1.proxy").arg(sourceInfo.completeBaseName()));
    }
    const QString suffix = (format == ProxyFormat::H264) ? QStringLiteral("mp4")
                                                         : QStringLiteral("mov");
    return sourceInfo.dir().absoluteFilePath(
        QStringLiteral("%1.proxy.%2").arg(sourceInfo.completeBaseName(), suffix));
}

QString EditorWindow::clipFileInfoSummary(const QString &filePath, const MediaProbeResult *knownProbe) const
{
    if (filePath.isEmpty()) return QStringLiteral("Path: None");

    const QFileInfo info(filePath);
    QStringList lines;
    lines << QStringLiteral("Path: %1").arg(QDir::toNativeSeparators(filePath));
    lines << QStringLiteral("Exists: %1").arg(info.exists() ? QStringLiteral("Yes") : QStringLiteral("No"));
    if (!info.exists()) return lines.join(QLatin1Char('\n'));

    lines << QStringLiteral("Size: %1 MB").arg(
        QString::number(static_cast<double>(info.size()) / (1024.0 * 1024.0), 'f', 1));
    lines << QStringLiteral("Modified: %1").arg(info.lastModified().toString(Qt::ISODate));
    MediaProbeResult fallbackProbe;
    const MediaProbeResult &probe = knownProbe ? *knownProbe : fallbackProbe;
    lines << QStringLiteral("Media Type: %1").arg(clipMediaTypeLabel(probe.mediaType));
    lines << QStringLiteral("Duration: %1 frames").arg(probe.durationFrames);
    lines << QStringLiteral("Audio: %1").arg(probe.hasAudio ? QStringLiteral("Yes") : QStringLiteral("No"));
    lines << QStringLiteral("Video: %1").arg(probe.hasVideo ? QStringLiteral("Yes") : QStringLiteral("No"));
    return lines.join(QLatin1Char('\n'));
}
void EditorWindow::continueProxyForClip(const QString &clipId)
{
    createProxyForClip(clipId, true);
}

void EditorWindow::createProxyForClip(const QString &clipId, bool continueGeneration)
{
    if (!m_timeline) return;

    const TimelineClip *clip = nullptr;
    for (const TimelineClip &candidate : m_timeline->clips())
    {
        if (candidate.id == clipId)
        {
            clip = &candidate;
            break;
        }
    }
    if (!clip) return;
    if (clip->mediaType != ClipMediaType::Video)
    {
        QMessageBox::information(this, QStringLiteral("Create Proxy"),
                                 QStringLiteral("Proxy creation is currently available for video clips."));
        return;
    }

    const bool imageSequenceProxy = isImageSequencePath(clip->filePath);
    const QStringList sequenceFrames = imageSequenceProxy ? imageSequenceFramePaths(clip->filePath) : QStringList{};
    if (imageSequenceProxy && sequenceFrames.isEmpty())
    {
        QMessageBox::warning(this, QStringLiteral("Create Proxy Failed"),
                             QStringLiteral("No sequence frames were found for this clip."));
        return;
    }

    const QString probePath = imageSequenceProxy ? sequenceFrames.constFirst() : clip->filePath;
    const MediaProbeResult sourceProbe = probeMediaFile(probePath, clip->durationFrames / kTimelineFps);
    const QString existingProxyPath = playbackProxyPathForClip(*clip);
    const QString outputPath = defaultProxyOutputPath(*clip, &sourceProbe, ProxyFormat::ImageSequence);
    const QString overwriteTarget = !existingProxyPath.isEmpty() ? existingProxyPath : outputPath;
    
    const bool sourceIsVfr = !imageSequenceProxy && isVariableFrameRate(clip->filePath);
    if (sourceIsVfr) {
        const auto response = QMessageBox::question(
            this,
            QStringLiteral("Variable Frame Rate Detected"),
            QStringLiteral("Source video appears variable frame rate (~%1 fps avg).\n\n"
                           "Continue with CFR proxy encoding (recommended)?")
                .arg(sourceProbe.fps, 0, 'f', 2),
            QMessageBox::Yes | QMessageBox::No,
            QMessageBox::Yes);
        if (response != QMessageBox::Yes) return;
    }
    
    int resumeNextFrameNumber = 1;
    if (continueGeneration) {
        const QFileInfo proxyInfo(overwriteTarget);
        if (!proxyInfo.exists() || !proxyInfo.isDir()) {
            QMessageBox::information(
                this,
                QStringLiteral("Continue Proxy Gen"),
                QStringLiteral("A generated image-sequence proxy directory was not found for this clip."));
            return;
        }
        const QStringList generatedFrames = QDir(overwriteTarget).entryList(
            {QStringLiteral("frame_*.jpg"), QStringLiteral("frame_*.png")},
            QDir::Files,
            QDir::Name);
        if (generatedFrames.isEmpty()) {
            QMessageBox::information(
                this,
                QStringLiteral("Continue Proxy Gen"),
                QStringLiteral("No generated proxy frames were found. Use Create Proxy instead."));
            return;
        }
        static const QRegularExpression kFrameNumberPattern(QStringLiteral("^frame_(\\d+)\\.(?:jpg|png)$"),
                                                            QRegularExpression::CaseInsensitiveOption);
        int maxFrameNumber = 0;
        for (const QString& fileName : generatedFrames) {
            const QRegularExpressionMatch match = kFrameNumberPattern.match(fileName);
            if (!match.hasMatch()) {
                continue;
            }
            const int parsed = match.captured(1).toInt();
            maxFrameNumber = qMax(maxFrameNumber, parsed);
        }
        resumeNextFrameNumber = qMax(1, maxFrameNumber + 1);
    } else if (QFileInfo::exists(overwriteTarget))
    {
        const auto response = QMessageBox::question(
            this,
            QStringLiteral("Overwrite Proxy"),
            QStringLiteral("A proxy already exists:\n%1\n\nOverwrite it?")
                .arg(QDir::toNativeSeparators(overwriteTarget)),
            QMessageBox::Yes | QMessageBox::No,
            QMessageBox::No);
        if (response != QMessageBox::Yes) return;
    }

    auto *dialog = new QDialog(this);
    dialog->setAttribute(Qt::WA_DeleteOnClose);
    dialog->setWindowTitle(QStringLiteral("%1  %2")
                               .arg(continueGeneration ? QStringLiteral("Continue Proxy Gen")
                                                       : QStringLiteral("Create Proxy"))
                               .arg(clip->label));
    dialog->resize(920, 560);

    auto *layout = new QVBoxLayout(dialog);
    layout->setContentsMargins(12, 12, 12, 12);
    layout->setSpacing(8);

    auto *title = new QLabel(QStringLiteral("Internal proxy for %1").arg(QDir::toNativeSeparators(clip->filePath)), dialog);
    title->setWordWrap(true);
    layout->addWidget(title);

    auto *formatRow = new QHBoxLayout;
    formatRow->setContentsMargins(0, 0, 0, 0);
    formatRow->addWidget(new QLabel(QStringLiteral("Format:"), dialog));
    auto *formatCombo = new QComboBox(dialog);
    formatCombo->addItem(QStringLiteral("Image Sequence (JPEG)"), static_cast<int>(ProxyFormat::ImageSequence));
    if (!continueGeneration && !sourceProbe.hasAlpha) {
        formatCombo->addItem(QStringLiteral("H.264 (MP4)"), static_cast<int>(ProxyFormat::H264));
#ifndef __APPLE__
        formatCombo->addItem(QStringLiteral("Motion JPEG (MOV)"), static_cast<int>(ProxyFormat::MJPEG));
#endif
    }
    formatCombo->setCurrentIndex(0);
    formatCombo->setEnabled(!continueGeneration && formatCombo->count() > 1);
    formatRow->addWidget(formatCombo);
    formatRow->addStretch(1);
    layout->addLayout(formatRow);

    auto *progressBar = new QProgressBar(dialog);
    progressBar->setRange(0, 100);
    progressBar->setValue(0);
    layout->addWidget(progressBar);

    auto *output = new QPlainTextEdit(dialog);
    output->setReadOnly(true);
    output->setLineWrapMode(QPlainTextEdit::NoWrap);
    output->setStyleSheet(QStringLiteral(
        "QPlainTextEdit { background: #05080c; color: #d6e2ee; border: 1px solid #24303c; "
        "border-radius: 8px; font-family: monospace; font-size: 12px; }"));
    layout->addWidget(output, 1);

    auto *autoScrollBox = new QCheckBox(QStringLiteral("Auto-scroll"), dialog);
    autoScrollBox->setChecked(true);
    layout->addWidget(autoScrollBox);

    auto *buttonRow = new QHBoxLayout;
    buttonRow->setContentsMargins(0, 0, 0, 0);
    buttonRow->setSpacing(8);
    auto *cancelButton = new QPushButton(QStringLiteral("Cancel"), dialog);
    auto *closeButton = new QPushButton(QStringLiteral("Close"), dialog);
    closeButton->setEnabled(false);
    buttonRow->addWidget(cancelButton);
    buttonRow->addStretch(1);
    buttonRow->addWidget(closeButton);
    layout->addLayout(buttonRow);

    const auto proxyFormat = static_cast<ProxyFormat>(formatCombo->currentData().toInt());
    const QString finalOutputPath = defaultProxyOutputPath(*clip, &sourceProbe, proxyFormat);
    const int proxyFps =
        qMax(1, qRound(sourceProbe.fps > 0.001 ? sourceProbe.fps : static_cast<double>(kTimelineFps)));
    if (!continueGeneration && finalOutputPath != overwriteTarget && QFileInfo::exists(finalOutputPath))
    {
        const auto response = QMessageBox::question(
            this,
            QStringLiteral("Overwrite Proxy"),
            QStringLiteral("A proxy already exists:\n%1\n\nOverwrite it?")
                .arg(QDir::toNativeSeparators(finalOutputPath)),
            QMessageBox::Yes | QMessageBox::No,
            QMessageBox::No);
        if (response != QMessageBox::Yes) return;
    }

    {
        QMessageBox confirm(this);
        confirm.setIcon(QMessageBox::Information);
        confirm.setWindowTitle(QStringLiteral("Confirm Proxy Generation"));
        const QString formatLabel =
            proxyFormat == ProxyFormat::H264
                ? QStringLiteral("H.264 MP4")
                : (proxyFormat == ProxyFormat::MJPEG
                       ? QStringLiteral("Motion JPEG MOV")
                       : QStringLiteral("image-sequence"));
        confirm.setText(QStringLiteral("Create an internal %1 proxy?").arg(formatLabel));
        confirm.setInformativeText(
            QStringLiteral("No external ffmpeg executable is required.\n"
                           "Video sources are decoded through the explicit software proxy path.\n"
                           "Source VFR: %1\nProxy FPS: %2\nMode: %3")
                .arg(sourceIsVfr ? QStringLiteral("Yes") : QStringLiteral("No"))
                .arg(proxyFps)
                .arg(continueGeneration ? QStringLiteral("Continue from frame %1").arg(resumeNextFrameNumber)
                                        : QStringLiteral("Full regeneration")));
        confirm.setStandardButtons(QMessageBox::Yes | QMessageBox::Cancel);
        confirm.setDefaultButton(QMessageBox::Yes);
        if (confirm.exec() != QMessageBox::Yes) {
            return;
        }
    }

    const auto appendOutput = [output, autoScrollBox](const QString &text)
    {
        if (text.isEmpty()) return;
        if (autoScrollBox->isChecked()) output->moveCursor(QTextCursor::End);
        output->insertPlainText(text);
        if (autoScrollBox->isChecked()) output->moveCursor(QTextCursor::End);
    };

    auto cancelFlag = std::make_shared<std::atomic_bool>(false);
    connect(cancelButton, &QPushButton::clicked, dialog, [cancelFlag, appendOutput, cancelButton]() {
        cancelFlag->store(true);
        cancelButton->setEnabled(false);
        appendOutput(QStringLiteral("\n[cancel requested]\n"));
    });
    connect(closeButton, &QPushButton::clicked, dialog, &QDialog::close);
    connect(dialog, &QDialog::finished, dialog, [cancelFlag](int) {
        cancelFlag->store(true);
    });

    dialog->show();

    QPointer<QDialog> dialogPtr(dialog);
    QPointer<QProgressBar> progressPtr(progressBar);
    QPointer<QPushButton> closeButtonPtr(closeButton);
    QPointer<QPushButton> cancelButtonPtr(cancelButton);
    std::thread([this,
                 dialogPtr,
                 cancelFlag,
                 appendOutput,
                 progressPtr,
                 closeButtonPtr,
                 cancelButtonPtr,
                 clipId,
                 sourcePath = QFileInfo(clip->filePath).absoluteFilePath(),
                 sequenceFrames,
                 imageSequenceProxy,
                 outputPath = finalOutputPath,
                 proxyFormat,
                 existingProxyPath,
                 sourceProbe,
                 resumeNextFrameNumber]() {
        auto uiLog = [dialogPtr, appendOutput](const QString& text) {
            if (!dialogPtr) {
                return;
            }
            QMetaObject::invokeMethod(dialogPtr,
                                      [appendOutput, text]() { appendOutput(text); },
                                      Qt::QueuedConnection);
        };
        auto uiProgress = [dialogPtr, progressPtr](int value) {
            if (!dialogPtr || !progressPtr) {
                return;
            }
            QMetaObject::invokeMethod(dialogPtr,
                                      [progressPtr, value]() {
                                          if (progressPtr) {
                                              progressPtr->setValue(value);
                                          }
                                      },
                                      Qt::QueuedConnection);
        };

        const ProxyGenerationResult generation =
            proxyFormat == ProxyFormat::ImageSequence
                ? generateImageSequenceProxyWithLibraries(sourcePath,
                                                          sequenceFrames,
                                                          imageSequenceProxy,
                                                          outputPath,
                                                          sourceProbe,
                                                          resumeNextFrameNumber,
                                                          cancelFlag,
                                                          uiLog,
                                                          uiProgress)
                : generateVideoContainerProxyWithLibraries(sourcePath,
                                                           outputPath,
                                                           proxyFormat,
                                                           sourceProbe,
                                                           cancelFlag,
                                                           uiLog,
                                                           uiProgress);

        if (!dialogPtr) {
            return;
        }
        QMetaObject::invokeMethod(
            dialogPtr,
            [this,
             clipId,
             outputPath,
             existingProxyPath,
             sourceFps = sourceProbe.fps,
             sourceDurationFrames = sourceProbe.durationFrames,
             appendOutput,
             closeButtonPtr,
             cancelButtonPtr,
             generation]() {
                if (cancelButtonPtr) {
                    cancelButtonPtr->setEnabled(false);
                }
                if (closeButtonPtr) {
                    closeButtonPtr->setEnabled(true);
                }
                appendOutput(QStringLiteral("\n[%1] %2\n")
                                 .arg(generation.success ? QStringLiteral("finished")
                                                         : (generation.canceled ? QStringLiteral("canceled")
                                                                                : QStringLiteral("failed")),
                                      generation.message));
                if (!generation.success || !QFileInfo::exists(outputPath))
                {
                    return;
                }
                if (!existingProxyPath.isEmpty() &&
                    existingProxyPath != outputPath &&
                    QFileInfo::exists(existingProxyPath))
                {
                    const QFileInfo existingInfo(existingProxyPath);
                    if (existingInfo.isDir()) {
                        QDir(existingProxyPath).removeRecursively();
                    } else {
                        QFile::remove(existingProxyPath);
                    }
                }
                if (!m_timeline->updateClipById(
                        clipId,
                        [outputPath, sourceFps, sourceDurationFrames](TimelineClip &updatedClip)
                        {
                            updatedClip.proxyPath = outputPath;
                            updatedClip.useProxy = true;
                            if (sourceFps > 0.001) {
                                const bool hadLegacyTimelineFps =
                                    qAbs(updatedClip.sourceFps - static_cast<qreal>(kTimelineFps)) <= 0.001;
                                updatedClip.sourceFps = sourceFps;
                                if (hadLegacyTimelineFps &&
                                    updatedClip.sourceDurationFrames > 0 &&
                                    qAbs(updatedClip.durationFrames - updatedClip.sourceDurationFrames) <= 1) {
                                    updatedClip.durationFrames = qMax<int64_t>(
                                        1,
                                        qRound64((static_cast<qreal>(updatedClip.sourceDurationFrames) /
                                                  updatedClip.sourceFps) *
                                                 static_cast<qreal>(kTimelineFps)));
                                }
                            }
                            if (sourceDurationFrames > 0) {
                                updatedClip.sourceDurationFrames = sourceDurationFrames;
                            }
                        }))
                {
                    return;
                }
                if (m_timeline->clipsChanged)
                {
                    m_timeline->clipsChanged();
                }
                if (m_renderUseProxiesCheckBox && !m_renderUseProxiesCheckBox->isChecked()) {
                    m_renderUseProxiesCheckBox->setChecked(true);
                    appendOutput(QStringLiteral("Enabled proxy playback for preview.\n"));
                }
                scheduleSaveState();
            },
            Qt::QueuedConnection);
    }).detach();
}

void EditorWindow::requestAutoSyncForSelection(const QSet<QString>& selectedClipIds)
{
    if (!m_timeline || selectedClipIds.isEmpty()) {
        return;
    }

    const QString backend = qEnvironmentVariable("SYNC_DETECTOR_BACKEND", QStringLiteral("auto")).trimmed();
    const QString extraArgs = qEnvironmentVariable("SYNC_DETECTOR_EXTRA_ARGS").trimmed();
    const QString runtime = qEnvironmentVariable("SYNC_DETECTOR_RUNTIME", QString()).trimmed().toLower();
    bool useDockerRuntime =
        (runtime == QStringLiteral("docker")) ||
        (backend.compare(QStringLiteral("syncnet"), Qt::CaseInsensitive) == 0);
    const QString scriptPath = QDir(QDir::currentPath()).absoluteFilePath(QStringLiteral("sync_detector.py"));
    const bool scriptExists = QFileInfo::exists(scriptPath);
    QString pythonPath = QStandardPaths::findExecutable(QStringLiteral("python3"));
    if (!scriptExists) {
        QMessageBox::warning(this,
                             QStringLiteral("Sync Failed"),
                             QStringLiteral("sync_detector.py was not found at:\n%1")
                                 .arg(QDir::toNativeSeparators(scriptPath)));
        return;
    }
    {
        QMessageBox runtimeChoice(this);
        runtimeChoice.setIcon(QMessageBox::Information);
        runtimeChoice.setWindowTitle(QStringLiteral("Sync Runtime"));
        runtimeChoice.setText(QStringLiteral("Choose runtime for this sync run."));
        runtimeChoice.setInformativeText(
            QStringLiteral("Docker can use containerized dependencies. Local Python can be faster to start."));
        QPushButton* dockerButton =
            runtimeChoice.addButton(QStringLiteral("Use Docker"), QMessageBox::AcceptRole);
        QPushButton* localButton = nullptr;
        if (!pythonPath.isEmpty()) {
            localButton =
                runtimeChoice.addButton(QStringLiteral("Use Local Python"), QMessageBox::ActionRole);
        }
        runtimeChoice.addButton(QMessageBox::Cancel);
        runtimeChoice.setDefaultButton(useDockerRuntime ? dockerButton : localButton ? localButton : dockerButton);
        runtimeChoice.exec();
        if (runtimeChoice.clickedButton() == dockerButton) {
            useDockerRuntime = true;
        } else if (runtimeChoice.clickedButton() == localButton) {
            useDockerRuntime = false;
        } else {
            return;
        }
    }
    if (!useDockerRuntime && pythonPath.isEmpty()) {
        QMessageBox::warning(this,
                             QStringLiteral("Sync Failed"),
                             QStringLiteral("python3 was not found in PATH."));
        return;
    }

    if (m_renderInProgress) {
        QMessageBox::information(this, QStringLiteral("Sync"), QStringLiteral("A render is currently in progress."));
        return;
    }

    QVector<TimelineClip> clips = m_timeline->clips();
    if (selectedClipIds.isEmpty()) {
        return;
    }

    const AutoSyncSelectionPlan syncPlan = buildAutoSyncSelectionPlan(
        clips,
        selectedClipIds,
        [this](const TimelineClip& clip) { return clipHasVisuals(clip); },
        [](const TimelineClip& clip) { return syncAudioPathForClip(clip); },
        [](const TimelineClip& clip) { return playbackMediaPathForClip(clip); });

    if (!syncPlan.ok &&
        syncPlan.message == QStringLiteral("Select at least one clip with audio to use as the sync anchor.")) {
        QMessageBox::information(this,
                                 QStringLiteral("Sync"),
                                 syncPlan.message);
        return;
    }
    if (!syncPlan.ok &&
        syncPlan.message == QStringLiteral("Audio source is unavailable for sync detection.")) {
        QMessageBox::warning(this,
                             QStringLiteral("Sync Failed"),
                             syncPlan.message);
        return;
    }
    if (!syncPlan.ok) {
        QMessageBox::information(this,
                                 QStringLiteral("Sync"),
                                 syncPlan.message);
        return;
    }

    const QVector<AutoSyncTargetPlan> syncTargets = syncPlan.targets;
    const QSet<QString> syncMarkerClipIds = syncPlan.syncMarkerClipIds;

    QString dockerImage = qEnvironmentVariable("SYNCNET_DOCKER_IMAGE", QStringLiteral("syncnet-detector:latest")).trimmed();
    if (dockerImage.isEmpty()) {
        dockerImage = QStringLiteral("syncnet-detector:latest");
    }
    const QString effectiveBackend = backend.isEmpty() ? QStringLiteral("auto") : backend;
    const QString modelPath = qEnvironmentVariable("SYNCNET_MODEL_PATH", QString()).trimmed();
    const QString modelDevice = qEnvironmentVariable("SYNCNET_DEVICE", QStringLiteral("auto")).trimmed();
    bool intervalOk = false;
    const double intervalSeconds =
        qEnvironmentVariable("SYNC_DETECTOR_INTERVAL_SECONDS", QStringLiteral("5.0"))
            .trimmed()
            .toDouble(&intervalOk);
    const double effectiveIntervalSeconds =
        intervalOk ? qMax(0.1, intervalSeconds) : 5.0;
    bool windowOk = false;
    const double windowSeconds =
        qEnvironmentVariable("SYNC_DETECTOR_WINDOW_SECONDS", QStringLiteral("10.0"))
            .trimmed()
            .toDouble(&windowOk);
    const double effectiveWindowSeconds =
        windowOk ? qMax(0.1, windowSeconds) : 10.0;
    const QString intervalArg = QString::number(effectiveIntervalSeconds, 'g', 8);
    const QString windowArg = QString::number(effectiveWindowSeconds, 'g', 8);

    QStringList commandPreview;
    if (useDockerRuntime) {
        commandPreview.push_back(QStringLiteral("docker image inspect %1").arg(dockerImage));
        commandPreview.push_back(QStringLiteral("docker build -f %1 -t %2 %3")
                                     .arg(QStringLiteral("syncnet.dockerfile"),
                                          dockerImage,
                                          QDir::currentPath()));
        commandPreview.push_back(QStringLiteral(
                                     "docker run --rm [mounts incl: local sync_detector.py + source/dest workspace] "
                                     "--entrypoint python %1 /workspace/sync_detector.py "
                                     "--video ... --audio ... --mode audio|av --fps %2 --interval-seconds %3 "
                                     "--window-seconds %4 --backend %5 --progress")
                                     .arg(dockerImage)
                                     .arg(kTimelineFps)
                                     .arg(intervalArg)
                                     .arg(windowArg)
                                     .arg(effectiveBackend));
    } else {
        commandPreview.push_back(QStringLiteral("%1 %2 --video ... --audio ... --mode audio|av --fps %3 --interval-seconds %4 --window-seconds %5 --backend %6 --progress")
                                     .arg(QFileInfo(pythonPath).fileName(),
                                          QFileInfo(scriptPath).fileName())
                                     .arg(kTimelineFps)
                                     .arg(intervalArg)
                                     .arg(windowArg)
                                     .arg(effectiveBackend));
    }
    if (!extraArgs.isEmpty()) {
        commandPreview.push_back(QStringLiteral("extra args: %1").arg(extraArgs));
    }

    QMessageBox confirm(this);
    confirm.setIcon(QMessageBox::Warning);
    confirm.setWindowTitle(QStringLiteral("Sync: Run External Commands"));
    confirm.setText(QStringLiteral("Sync will run external shell commands to analyze selected clips. Continue?"));
    confirm.setInformativeText(
        useDockerRuntime
            ? QStringLiteral("Runtime: Docker. This may build and run Docker containers and can take a while. No timeline changes are applied until you review and accept recommendations.")
            : QStringLiteral("Runtime: Local Python. This will run sync_detector.py in a shell process. No timeline changes are applied until you review and accept recommendations."));
    confirm.setDetailedText(commandPreview.join(QLatin1Char('\n')));
    confirm.setStandardButtons(QMessageBox::Yes | QMessageBox::Cancel);
    confirm.setDefaultButton(QMessageBox::Cancel);
    if (confirm.exec() != QMessageBox::Yes) {
        return;
    }

    auto* terminalDialog = new QDialog(this);
    terminalDialog->setAttribute(Qt::WA_DeleteOnClose);
    terminalDialog->setWindowTitle(QStringLiteral("Sync Terminal"));
    terminalDialog->resize(980, 560);
    auto* terminalLayout = new QVBoxLayout(terminalDialog);
    terminalLayout->setContentsMargins(12, 12, 12, 12);
    terminalLayout->setSpacing(8);
    auto* statusLabel = new QLabel(QStringLiteral("Running sync..."), terminalDialog);
    terminalLayout->addWidget(statusLabel);
    auto* progressBar = new QProgressBar(terminalDialog);
    progressBar->setRange(0, syncTargets.size());
    progressBar->setValue(0);
    terminalLayout->addWidget(progressBar);
    auto* output = new QPlainTextEdit(terminalDialog);
    output->setReadOnly(true);
    output->setLineWrapMode(QPlainTextEdit::NoWrap);
    output->setStyleSheet(QStringLiteral(
        "QPlainTextEdit { background: #05080c; color: #d6e2ee; border: 1px solid #24303c; "
        "border-radius: 8px; font-family: monospace; font-size: 12px; }"));
    terminalLayout->addWidget(output, 1);
    auto* buttonRow = new QHBoxLayout;
    buttonRow->setContentsMargins(0, 0, 0, 0);
    auto* cancelButton = new QPushButton(QStringLiteral("Cancel"), terminalDialog);
    auto* closeButton = new QPushButton(QStringLiteral("Close"), terminalDialog);
    closeButton->setEnabled(false);
    buttonRow->addWidget(cancelButton);
    buttonRow->addStretch(1);
    buttonRow->addWidget(closeButton);
    terminalLayout->addLayout(buttonRow);

    auto appendOutput = [output](const QString& text) {
        if (text.isEmpty()) {
            return;
        }
        output->moveCursor(QTextCursor::End);
        output->insertPlainText(text);
        output->moveCursor(QTextCursor::End);
    };
    terminalDialog->show();

    auto cancelFlag = std::make_shared<std::atomic_bool>(false);
    connect(cancelButton, &QPushButton::clicked, terminalDialog, [cancelFlag]() {
        cancelFlag->store(true);
    });
    connect(closeButton, &QPushButton::clicked, terminalDialog, &QDialog::close);

    const QString previousSelected = m_timeline->selectedClipId();
    const QVector<TimelineClip> initialClips = m_timeline->clips();
    const QVector<RenderSyncMarker> initialMarkers = m_timeline->renderSyncMarkers();

    std::thread([this,
                 cancelFlag,
                 appendOutput,
                 statusLabel,
                 progressBar,
                 closeButton,
                 cancelButton,
                 terminalDialog,
                 initialClips,
                 initialMarkers,
                 syncTargets,
                 syncMarkerClipIds,
                 useDockerRuntime,
                 scriptPath,
                 pythonPath,
                 dockerImage,
                 effectiveBackend,
                 modelPath,
                 modelDevice,
                 intervalArg,
                 windowArg,
                 extraArgs,
                 previousSelected]() {
        struct ClipRecommendation {
            QString clipId;
            QString label;
            QString mode;
            int videoOffsetFrames = 0;
            QVector<RenderSyncMarker> markers;
        };
        auto mergeMarkersWithMinSpacing = [](const QVector<RenderSyncMarker>& inputMarkers,
                                             const QString& clipId,
                                             int minSpacingFrames) {
            if (inputMarkers.isEmpty() || minSpacingFrames <= 0) {
                return inputMarkers;
            }

            QVector<RenderSyncMarker> markers = inputMarkers;
            std::sort(markers.begin(), markers.end(),
                      [](const RenderSyncMarker& a, const RenderSyncMarker& b) {
                          if (a.frame != b.frame) {
                              return a.frame < b.frame;
                          }
                          return a.count < b.count;
                      });

            auto markerDelta = [](const RenderSyncMarker& marker) {
                const int sign = marker.action == RenderSyncAction::DuplicateFrame ? 1 : -1;
                return sign * qMax(1, marker.count);
            };

            QVector<RenderSyncMarker> output;
            bool haveBucket = false;
            int64_t bucketFrame = 0;
            int bucketDelta = 0;
            auto flushBucket = [&]() {
                if (!haveBucket || bucketDelta == 0) {
                    haveBucket = false;
                    bucketDelta = 0;
                    return;
                }
                RenderSyncMarker merged;
                merged.clipId = clipId;
                merged.frame = bucketFrame;
                merged.action = bucketDelta > 0 ? RenderSyncAction::DuplicateFrame
                                                : RenderSyncAction::SkipFrame;
                merged.count = qMax(1, qAbs(bucketDelta));
                output.push_back(merged);
                haveBucket = false;
                bucketDelta = 0;
            };

            for (const RenderSyncMarker& marker : markers) {
                const int delta = markerDelta(marker);
                if (!haveBucket) {
                    haveBucket = true;
                    bucketFrame = marker.frame;
                    bucketDelta = delta;
                    continue;
                }
                if ((marker.frame - bucketFrame) < minSpacingFrames) {
                    bucketDelta += delta;
                    continue;
                }
                flushBucket();
                haveBucket = true;
                bucketFrame = marker.frame;
                bucketDelta = delta;
            }
            flushBucket();
            return output;
        };

        QVector<TimelineClip> clips = initialClips;
        QVector<RenderSyncMarker> nextMarkers = initialMarkers;
        nextMarkers.erase(std::remove_if(nextMarkers.begin(), nextMarkers.end(),
                                         [&](const RenderSyncMarker& marker) {
                                             return syncMarkerClipIds.contains(marker.clipId);
                                         }),
                          nextMarkers.end());
        QVector<ClipRecommendation> recommendations;
        int syncedCount = 0;
        QStringList failures;

        auto uiLog = [terminalDialog, appendOutput](const QString& line) {
            QMetaObject::invokeMethod(terminalDialog, [appendOutput, line]() { appendOutput(line); }, Qt::QueuedConnection);
        };
        auto uiProgress = [terminalDialog, progressBar](int value) {
            QMetaObject::invokeMethod(terminalDialog, [progressBar, value]() { progressBar->setValue(value); }, Qt::QueuedConnection);
        };
        auto uiStatus = [terminalDialog, statusLabel](const QString& text) {
            QMetaObject::invokeMethod(terminalDialog, [statusLabel, text]() { statusLabel->setText(text); }, Qt::QueuedConnection);
        };

        if (useDockerRuntime) {
            const QString dockerPath = QStandardPaths::findExecutable(QStringLiteral("docker"));
            if (dockerPath.isEmpty()) {
                failures.push_back(QStringLiteral("Docker runtime requested, but docker was not found in PATH."));
                cancelFlag->store(true);
            } else if (!QFileInfo::exists(scriptPath)) {
                failures.push_back(QStringLiteral("sync_detector.py not found at %1").arg(scriptPath));
                cancelFlag->store(true);
            } else {
                uiStatus(QStringLiteral("Preparing Docker runtime..."));
                const QString inspectCommand = QStringLiteral("docker image inspect %1 >/dev/null 2>&1")
                                                   .arg(shellQuote(dockerImage));
                const ShellRunResult inspectResult = runShellCommandStreaming(inspectCommand, cancelFlag.get(), uiLog, 60000);
                if (!inspectResult.canceled &&
                    (inspectResult.exitStatus != QProcess::NormalExit || inspectResult.exitCode != 0)) {
                    const QString dockerfilePath =
                        QDir(QDir::currentPath()).absoluteFilePath(QStringLiteral("syncnet.dockerfile"));
                    if (!QFileInfo::exists(dockerfilePath)) {
                        failures.push_back(QStringLiteral("Docker image missing and syncnet.dockerfile not found."));
                        cancelFlag->store(true);
                    } else {
                        const QString buildCommand = QStringLiteral("docker build -f %1 -t %2 %3")
                                                         .arg(shellQuote(dockerfilePath))
                                                         .arg(shellQuote(dockerImage))
                                                         .arg(shellQuote(QDir::currentPath()));
                        uiStatus(QStringLiteral("Building Docker image..."));
                        const ShellRunResult buildResult = runShellCommandStreaming(buildCommand, cancelFlag.get(), uiLog, -1);
                        if (buildResult.canceled) {
                            cancelFlag->store(true);
                        } else if (buildResult.exitStatus != QProcess::NormalExit || buildResult.exitCode != 0) {
                            failures.push_back(QStringLiteral("Failed to build Docker image %1.").arg(dockerImage));
                            cancelFlag->store(true);
                        }
                    }
                } else if (inspectResult.canceled) {
                    cancelFlag->store(true);
                }
            }
        }

        for (int i = 0; i < syncTargets.size(); ++i) {
            uiProgress(i);
            if (cancelFlag->load()) {
                break;
            }

            const AutoSyncTargetPlan target = syncTargets[i];
            const TimelineClip targetClip = target.clip;
            const QString targetPath = target.mediaPath;
            if (targetPath.isEmpty() || !QFileInfo::exists(targetPath)) {
                failures.push_back(QStringLiteral("%1: missing sync source").arg(targetClip.label));
                continue;
            }
            if (target.anchors.isEmpty()) {
                failures.push_back(QStringLiteral("%1: no fixed audio anchor").arg(targetClip.label));
                continue;
            }

            const QString targetBackend =
                target.mode == QStringLiteral("audio") ? QStringLiteral("avcorr") : effectiveBackend;

            QJsonObject bestRoot;
            AutoSyncAnchorPlan bestAnchor;
            double bestConfidence = -1.0;
            QStringList anchorFailures;
            for (const AutoSyncAnchorPlan& anchor : target.anchors) {
                if (anchor.audioPath.isEmpty() || !QFileInfo::exists(anchor.audioPath)) {
                    anchorFailures.push_back(QStringLiteral("%1: missing anchor audio")
                                                 .arg(anchor.clip.label));
                    continue;
                }

                QString command;
                if (useDockerRuntime) {
                    QString modelArg;
                    QVector<QPair<QString, QString>> mounts;
                    auto mountPath = [&mounts](const QString& hostFile, const QString& prefix) -> QString {
                        const QFileInfo info(hostFile);
                        const QString hostDir = info.absolutePath();
                        for (const auto& existing : std::as_const(mounts)) {
                            if (existing.first == hostDir) {
                                return existing.second + QStringLiteral("/") + info.fileName();
                            }
                        }
                        const QString target = QStringLiteral("/%1%2").arg(prefix).arg(mounts.size());
                        mounts.push_back(qMakePair(hostDir, target));
                        return target + QStringLiteral("/") + info.fileName();
                    };
                    const QString containerVideoPath = mountPath(targetPath, QStringLiteral("input"));
                    const QString containerAudioPath = mountPath(anchor.audioPath, QStringLiteral("input"));
                    if (!modelPath.isEmpty()) {
                        const QString containerModelPath = mountPath(modelPath, QStringLiteral("model"));
                        modelArg = QStringLiteral(" --syncnet-model %1 --syncnet-device %2")
                                       .arg(shellQuote(containerModelPath),
                                            shellQuote(modelDevice.isEmpty() ? QStringLiteral("auto") : modelDevice));
                    }
                    QString gpuArg;
                    if (qEnvironmentVariable("SYNCNET_DOCKER_GPU", QStringLiteral("1")).trimmed() != QStringLiteral("0")) {
                        gpuArg = QStringLiteral(" --gpus all");
                    }
                    QString mountArgs;
                    for (const auto& mount : std::as_const(mounts)) {
                        mountArgs += QStringLiteral(" -v %1:%2:ro").arg(shellQuote(mount.first), shellQuote(mount.second));
                    }
                    const QString workspaceHostDir = QDir::currentPath();
                    const QString scriptContainerPath = QStringLiteral("/workspace/sync_detector.py");
                    const QString workspaceContainerDir = QStringLiteral("/workspace-host");
                    mountArgs += QStringLiteral(" -v %1:%2:ro")
                                     .arg(shellQuote(scriptPath),
                                          shellQuote(scriptContainerPath));
                    mountArgs += QStringLiteral(" -v %1:%2:rw")
                                     .arg(shellQuote(workspaceHostDir),
                                          shellQuote(workspaceContainerDir));
                    command = QStringLiteral(
                                  "docker run --rm%1%2 -w %3 --entrypoint python %4 %5 "
                                  "--video %6 --audio %7 --mode %8 --fps %9 --interval-seconds %10 --window-seconds %11 --backend %12 --progress%13")
                                  .arg(gpuArg)
                                  .arg(mountArgs)
                                  .arg(shellQuote(workspaceContainerDir))
                                  .arg(shellQuote(dockerImage))
                                  .arg(shellQuote(scriptContainerPath))
                                  .arg(shellQuote(containerVideoPath))
                                  .arg(shellQuote(containerAudioPath))
                                  .arg(shellQuote(target.mode))
                                  .arg(kTimelineFps)
                                  .arg(shellQuote(intervalArg))
                                  .arg(shellQuote(windowArg))
                                  .arg(shellQuote(targetBackend))
                                  .arg(modelArg);
                } else {
                    command = QStringLiteral("%1 %2 --video %3 --audio %4 --mode %5 --fps %6 --interval-seconds %7 --window-seconds %8 --backend %9")
                                  .arg(shellQuote(pythonPath))
                                  .arg(shellQuote(scriptPath))
                                  .arg(shellQuote(targetPath))
                                  .arg(shellQuote(anchor.audioPath))
                                  .arg(shellQuote(target.mode))
                                  .arg(kTimelineFps)
                                  .arg(shellQuote(intervalArg))
                                  .arg(shellQuote(windowArg))
                                  .arg(shellQuote(targetBackend));
                    command += QStringLiteral(" --progress");
                }
                if (!extraArgs.isEmpty()) {
                    command += QStringLiteral(" ");
                    command += extraArgs;
                }

                uiLog(QStringLiteral("Sync %1 against fixed anchor %2\n")
                          .arg(targetClip.label, anchor.clip.label));
                const ShellRunResult detectorRun = runShellCommandStreaming(command, cancelFlag.get(), uiLog, -1);
                if (detectorRun.canceled || cancelFlag->load()) {
                    cancelFlag->store(true);
                    break;
                }
                const QString detectorOutput = detectorRun.output;
                if (detectorRun.exitStatus != QProcess::NormalExit || detectorRun.exitCode != 0) {
                    anchorFailures.push_back(QStringLiteral("%1: detector failed (%2)")
                                                 .arg(anchor.clip.label,
                                                      detectorOutput.trimmed().isEmpty() ? detectorRun.errorString : detectorOutput.trimmed()));
                    continue;
                }

                const QString jsonPayload = extractJsonObject(detectorOutput);
                QJsonParseError parseError;
                const QJsonDocument json = QJsonDocument::fromJson(jsonPayload.toUtf8(), &parseError);
                if (json.isNull() || !json.isObject()) {
                    anchorFailures.push_back(QStringLiteral("%1: invalid detector output").arg(anchor.clip.label));
                    continue;
                }

                const QJsonObject root = json.object();
                const double confidence = syncDetectionConfidence(root);
                if (confidence > bestConfidence) {
                    bestConfidence = confidence;
                    bestRoot = root;
                    bestAnchor = anchor;
                }
            }
            if (cancelFlag->load()) {
                break;
            }
            if (bestRoot.isEmpty()) {
                failures.push_back(QStringLiteral("%1: detector failed for all fixed anchors%2")
                                       .arg(targetClip.label,
                                            anchorFailures.isEmpty()
                                                ? QString()
                                                : QStringLiteral(" (%1)").arg(anchorFailures.join(QStringLiteral("; ")))));
                continue;
            }

            const QJsonObject root = bestRoot;
            const int videoOffsetFrames = root.value(QStringLiteral("videoOffsetFrames")).toInt(0);
            int64_t appliedClipStartFrame = targetClip.startFrame;
            for (TimelineClip& mutableClip : clips) {
                if (mutableClip.id != targetClip.id) {
                    continue;
                }
                if (videoOffsetFrames != 0) {
                    mutableClip.startFrame = qMax<int64_t>(0, mutableClip.startFrame + videoOffsetFrames);
                    normalizeClipTiming(mutableClip);
                }
                appliedClipStartFrame = mutableClip.startFrame;
                break;
            }

            ClipRecommendation recommendation;
            recommendation.clipId = targetClip.id;
            recommendation.label = targetClip.label;
            recommendation.mode = target.mode;
            recommendation.videoOffsetFrames = videoOffsetFrames;

            if (target.replaceRenderSyncMarkers) {
                const QJsonArray markerArray = root.value(QStringLiteral("markers")).toArray();
                for (const QJsonValue& markerValue : markerArray) {
                    if (!markerValue.isObject()) {
                        continue;
                    }
                    const QJsonObject markerObj = markerValue.toObject();
                    RenderSyncMarker marker;
                    marker.clipId = targetClip.id;
                    marker.count = qMax(1, markerObj.value(QStringLiteral("count")).toInt(1));
                    if (!parseSyncAction(markerObj.value(QStringLiteral("action")).toString(), &marker.action)) {
                        continue;
                    }

                    const QJsonValue timelineFrameValue = markerObj.value(QStringLiteral("timelineFrame"));
                    if (timelineFrameValue.isDouble()) {
                        marker.frame = qMax<int64_t>(0, timelineFrameValue.toVariant().toLongLong());
                    } else {
                        const int64_t localFrame = qMax<int64_t>(0, markerObj.value(QStringLiteral("frame")).toVariant().toLongLong());
                        marker.frame = qMax<int64_t>(0, appliedClipStartFrame + localFrame);
                    }
                    recommendation.markers.push_back(marker);
                    nextMarkers.push_back(marker);
                }
            }
            recommendations.push_back(recommendation);
            uiLog(QStringLiteral("Recommendation for %1 [%2] anchored to %3: shift %4 frame(s), confidence %5, new sync points: %6\n")
                      .arg(targetClip.label,
                           target.mode,
                           bestAnchor.clip.label,
                           QString::number(videoOffsetFrames),
                           QString::number(bestConfidence, 'f', 3),
                           QString::number(recommendation.markers.size())));
            ++syncedCount;
        }
        uiProgress(syncTargets.size());

        QMetaObject::invokeMethod(terminalDialog, [=, this]() {
            cancelButton->setEnabled(false);
            closeButton->setEnabled(true);
            if (cancelFlag->load() && syncedCount == 0) {
                statusLabel->setText(QStringLiteral("Sync canceled"));
                return;
            }

            QStringList recommendationLines;
            int clipsWithAdjustments = 0;
            int totalSuggestedMarkers = 0;
            for (const ClipRecommendation& recommendation : recommendations) {
                const bool hasAdjustment =
                    (recommendation.videoOffsetFrames != 0) || !recommendation.markers.isEmpty();
                if (!hasAdjustment) {
                    continue;
                }
                ++clipsWithAdjustments;
                totalSuggestedMarkers += recommendation.markers.size();
                QString line = QStringLiteral("%1 [%2]: shift %3 frame(s), sync points %4")
                                   .arg(recommendation.label,
                                        recommendation.mode,
                                        QString::number(recommendation.videoOffsetFrames),
                                        QString::number(recommendation.markers.size()));
                if (!recommendation.markers.isEmpty()) {
                    QStringList markerFrames;
                    const int markerPreviewCount = qMin(8, recommendation.markers.size());
                    for (int markerIndex = 0; markerIndex < markerPreviewCount; ++markerIndex) {
                        const RenderSyncMarker& marker = recommendation.markers[markerIndex];
                        const QString action =
                            marker.action == RenderSyncAction::DuplicateFrame
                                ? QStringLiteral("dup")
                                : QStringLiteral("skip");
                        markerFrames.push_back(
                            QStringLiteral("%1:%2x%3")
                                .arg(QString::number(marker.frame),
                                     action,
                                     QString::number(marker.count)));
                    }
                    line += QStringLiteral(" [") + markerFrames.join(QStringLiteral(", ")) + QStringLiteral("]");
                }
                recommendationLines.push_back(line);
            }

            QString summary = QStringLiteral("Analyzed %1 clip%2.")
                                  .arg(syncedCount)
                                  .arg(syncedCount == 1 ? QString() : QStringLiteral("s"));
            if (cancelFlag->load()) {
                summary += QStringLiteral(" (canceled)");
            }
            if (!recommendationLines.isEmpty()) {
                summary += QStringLiteral("\n\nRecommended adjustments:\n- %1")
                               .arg(recommendationLines.join(QStringLiteral("\n- ")));
            } else {
                summary += QStringLiteral("\n\nNo non-zero sync adjustments were recommended.");
            }
            if (!failures.isEmpty()) {
                summary += QStringLiteral("\n\nFailures:\n- %1").arg(failures.join(QStringLiteral("\n- ")));
            }
            appendOutput(QStringLiteral("\n%1\n").arg(summary));

            bool applied = false;
            if (syncedCount > 0 && clipsWithAdjustments > 0) {
                bool spacingAccepted = false;
                bool hasDefaultSpacing = false;
                int defaultSpacing = qEnvironmentVariableIntValue("SYNC_MIN_MARKER_SPACING_FRAMES",
                                                                  &hasDefaultSpacing);
                if (!hasDefaultSpacing) {
                    defaultSpacing = 6;
                }
                const int minSpacingFrames = QInputDialog::getInt(
                    terminalDialog,
                    QStringLiteral("Sync Options"),
                    QStringLiteral("Minimum spacing between sync points (frames).\n"
                                   "Points closer than this are merged and counts recalculated.\n"
                                   "Use 0 to keep all points."),
                    qMax(0, defaultSpacing),
                    0,
                    10000,
                    1,
                    &spacingAccepted);
                if (!spacingAccepted) {
                    appendOutput(QStringLiteral("Recommendations were not applied (options canceled).\n"));
                    summary += QStringLiteral("\n\nNot applied.");
                    statusLabel->setText(summary);
                    return;
                }

                QMessageBox applyConfirm(terminalDialog);
                applyConfirm.setIcon(QMessageBox::Question);
                applyConfirm.setWindowTitle(QStringLiteral("Apply Sync Recommendations"));
                applyConfirm.setText(
                    QStringLiteral("Apply recommended shift(s) and sync point(s)?"));
                applyConfirm.setInformativeText(
                    QStringLiteral("Clips with adjustments: %1\nSuggested sync points: %2\nMin spacing: %3 frame(s)")
                        .arg(clipsWithAdjustments)
                        .arg(totalSuggestedMarkers)
                        .arg(minSpacingFrames));
                applyConfirm.setDetailedText(recommendationLines.join(QLatin1Char('\n')));
                applyConfirm.setStandardButtons(QMessageBox::Yes | QMessageBox::No);
                applyConfirm.setDefaultButton(QMessageBox::Yes);
                if (applyConfirm.exec() == QMessageBox::Yes) {
                    QVector<RenderSyncMarker> filteredMarkers;
                    filteredMarkers.reserve(nextMarkers.size());
                    const int unaffectedCount = nextMarkers.size() - totalSuggestedMarkers;
                    if (unaffectedCount > 0) {
                        for (const RenderSyncMarker& marker : nextMarkers) {
                            if (!syncMarkerClipIds.contains(marker.clipId)) {
                                filteredMarkers.push_back(marker);
                            }
                        }
                    }
                    int recalculatedMarkers = 0;
                    for (const ClipRecommendation& recommendation : recommendations) {
                        const QVector<RenderSyncMarker> mergedMarkers =
                            mergeMarkersWithMinSpacing(recommendation.markers,
                                                       recommendation.clipId,
                                                       minSpacingFrames);
                        recalculatedMarkers += mergedMarkers.size();
                        for (const RenderSyncMarker& marker : mergedMarkers) {
                            filteredMarkers.push_back(marker);
                        }
                    }

                    const bool previousSuppress = m_suppressHistorySnapshots;
                    m_suppressHistorySnapshots = true;
                    m_timeline->setClips(clips);
                    m_timeline->setRenderSyncMarkers(filteredMarkers);
                    if (!previousSelected.isEmpty()) {
                        m_timeline->setSelectedClipId(previousSelected);
                    }
                    m_suppressHistorySnapshots = previousSuppress;
                    scheduleSaveState();
                    pushHistorySnapshot();
                    applied = true;
                    appendOutput(QStringLiteral("Sync points recalculated: %1 -> %2 (min spacing %3 frame(s)).\n")
                                     .arg(totalSuggestedMarkers)
                                     .arg(recalculatedMarkers)
                                     .arg(minSpacingFrames));
                    appendOutput(QStringLiteral("Applied recommended sync updates.\n"));
                } else {
                    appendOutput(QStringLiteral("Recommendations were not applied.\n"));
                }
            }

            if (applied) {
                summary += QStringLiteral("\n\nApplied.");
            } else if (clipsWithAdjustments > 0) {
                summary += QStringLiteral("\n\nNot applied.");
            }
            statusLabel->setText(summary);
        }, Qt::QueuedConnection);
    }).detach();
}

void EditorWindow::deleteProxyForClip(const QString &clipId)
{
    if (!m_timeline) return;

    const TimelineClip *clip = nullptr;
    for (const TimelineClip &candidate : m_timeline->clips())
    {
        if (candidate.id == clipId)
        {
            clip = &candidate;
            break;
        }
    }
    if (!clip) return;

    const QString proxyPath = playbackProxyPathForClip(*clip);
    if (proxyPath.isEmpty())
    {
        QMessageBox::information(this, QStringLiteral("Delete Proxy"),
                                 QStringLiteral("No proxy exists for this clip."));
        return;
    }

    const auto response = QMessageBox::question(
        this,
        QStringLiteral("Delete Proxy"),
        QStringLiteral("Delete this proxy?\n%1").arg(QDir::toNativeSeparators(proxyPath)),
        QMessageBox::Yes | QMessageBox::No,
        QMessageBox::No);
    if (response != QMessageBox::Yes) return;

    const QFileInfo proxyInfo(proxyPath);
    if (proxyInfo.isDir()) {
        QDir dir(proxyPath);
        dir.removeRecursively();
    } else {
        QFile::remove(proxyPath);
    }
    if (!m_timeline->updateClipById(clipId, [](TimelineClip &updatedClip)
                                   { updatedClip.proxyPath.clear(); }))
    {
        return;
    }
    if (m_timeline->clipsChanged)
    {
        m_timeline->clipsChanged();
    }
}
