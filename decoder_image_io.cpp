#include "decoder_image_io.h"

#include <QDebug>
#include <QFile>
#include <QFileInfo>
#include <QImageReader>
#include <QSet>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/imgutils.h>
#include <libswscale/swscale.h>
}

namespace editor {

bool isStillImagePath(const QString& path) {
    static const QSet<QString> suffixes = {
        QStringLiteral("png"),
        QStringLiteral("jpg"),
        QStringLiteral("jpeg"),
        QStringLiteral("bmp"),
        QStringLiteral("gif"),
        QStringLiteral("webp"),
        QStringLiteral("tif"),
        QStringLiteral("tiff")
    };
    return suffixes.contains(QFileInfo(path).suffix().toLower());
}

QImage loadSequenceFrameImage(const QStringList& framePaths, int64_t frameNumber) {
    if (framePaths.isEmpty()) {
        return QImage();
    }
    const int index = qBound(0, static_cast<int>(frameNumber), framePaths.size() - 1);
    return loadSingleImageFile(framePaths.at(index));
}

QImage loadSingleImageFile(const QString& framePath) {
    if (framePath.isEmpty()) {
        return QImage();
    }

    const QString suffix = QFileInfo(framePath).suffix().toLower();
    const bool useQtReader = (suffix != QStringLiteral("webp"));

    if (useQtReader) {
        QImageReader reader(framePath);

        const QSize imageSize = reader.size();
        if (imageSize.isValid()) {
            if (imageSize.width() <= 0 || imageSize.height() <= 0 ||
                imageSize.width() > 16384 || imageSize.height() > 16384) {
                qWarning() << "Invalid image dimensions:" << imageSize << "for file:" << framePath;
                return QImage();
            }
            constexpr qint64 kMaxImageBytes = 1024LL * 1024LL * 1024LL;
            if (static_cast<qint64>(imageSize.width()) * imageSize.height() * 4 > kMaxImageBytes) {
                qWarning() << "Image too large:" << imageSize << "for file:" << framePath;
                return QImage();
            }
        }

        QImage image = reader.read();
        if (!image.isNull()) {
            if (image.width() <= 0 || image.height() <= 0 ||
                image.width() > 16384 || image.height() > 16384) {
                qWarning() << "Invalid image dimensions after load:" << image.size() << "for file:" << framePath;
                return QImage();
            }
            if (!image.bits()) {
                qWarning() << "Image has no pixel data:" << framePath;
                return QImage();
            }
            if (image.bytesPerLine() <= 0 || image.bytesPerLine() > image.width() * 8) {
                qWarning() << "Invalid bytesPerLine:" << image.bytesPerLine() << "for image:" << framePath;
                return QImage();
            }
            if (image.format() == QImage::Format_Invalid) {
                qWarning() << "Invalid image format for file:" << framePath;
                return QImage();
            }

            if (image.format() != QImage::Format_ARGB32_Premultiplied) {
                QImage safeImage = image;
                if (image.hasAlphaChannel() &&
                    image.format() != QImage::Format_RGBA8888 &&
                    image.format() != QImage::Format_ARGB32) {
                    safeImage = image.convertToFormat(QImage::Format_RGBA8888);
                    if (safeImage.isNull()) {
                        qWarning() << "Failed to convert to intermediate RGBA format for file:" << framePath;
                        return QImage();
                    }
                }

                QImage converted = safeImage.convertToFormat(QImage::Format_ARGB32_Premultiplied);
                if (converted.isNull()) {
                    qWarning() << "Failed to convert image format for file:" << framePath;
                    return QImage();
                }
                image = std::move(converted);
            }
            return image;
        }

        qDebug() << "Qt reader failed for:" << framePath << "falling through to FFmpeg";
    }

    const QFileInfo fileInfo(framePath);
    if (!fileInfo.exists() || fileInfo.size() == 0) {
        qWarning() << "File does not exist or is empty:" << framePath;
        return QImage();
    }
    if (fileInfo.size() > 100 * 1024 * 1024) {
        qWarning() << "File too large:" << fileInfo.size() << "bytes:" << framePath;
        return QImage();
    }

    AVFormatContext* formatCtx = nullptr;
    const QByteArray pathBytes = QFile::encodeName(framePath);

    const AVInputFormat* inputFormat = nullptr;
    if (suffix == QStringLiteral("webp")) {
        inputFormat = av_find_input_format("webp_pipe");
    } else {
        inputFormat = av_find_input_format("image2");
    }

    if (avformat_open_input(&formatCtx, pathBytes.constData(), inputFormat, nullptr) < 0) {
        return QImage();
    }

    if (avformat_find_stream_info(formatCtx, nullptr) < 0) {
        avformat_close_input(&formatCtx);
        return QImage();
    }

    int videoStreamIndex = -1;
    for (unsigned i = 0; i < formatCtx->nb_streams; ++i) {
        if (formatCtx->streams[i] &&
            formatCtx->streams[i]->codecpar &&
            formatCtx->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_VIDEO) {
            videoStreamIndex = static_cast<int>(i);
            break;
        }
    }
    if (videoStreamIndex < 0) {
        avformat_close_input(&formatCtx);
        return QImage();
    }

    AVStream* stream = formatCtx->streams[videoStreamIndex];
    const AVCodec* decoder = avcodec_find_decoder(stream->codecpar->codec_id);
    if (!decoder) {
        avformat_close_input(&formatCtx);
        return QImage();
    }

    AVCodecContext* codecCtx = avcodec_alloc_context3(decoder);
    if (!codecCtx) {
        avformat_close_input(&formatCtx);
        return QImage();
    }

    if (avcodec_parameters_to_context(codecCtx, stream->codecpar) < 0 ||
        avcodec_open2(codecCtx, decoder, nullptr) < 0) {
        avcodec_free_context(&codecCtx);
        avformat_close_input(&formatCtx);
        return QImage();
    }

    AVPacket* packet = av_packet_alloc();
    AVFrame* frame = av_frame_alloc();
    if (!packet || !frame) {
        if (packet) {
            av_packet_free(&packet);
        }
        if (frame) {
            av_frame_free(&frame);
        }
        avcodec_free_context(&codecCtx);
        avformat_close_input(&formatCtx);
        return QImage();
    }

    QImage decodedImage;

    auto tryConvertFrame = [&](AVFrame* decodedFrame) -> bool {
        if (decodedFrame->width <= 0 || decodedFrame->height <= 0 ||
            decodedFrame->width > 16384 || decodedFrame->height > 16384) {
            qWarning() << "Invalid frame dimensions from FFmpeg:"
                       << decodedFrame->width << "x" << decodedFrame->height;
            return false;
        }

        if (decodedFrame->format == AV_PIX_FMT_RGBA) {
            decodedImage = QImage(decodedFrame->width, decodedFrame->height, QImage::Format_RGBA8888);
            if (!decodedImage.isNull() && decodedImage.bits()) {
                for (int y = 0; y < decodedFrame->height; ++y) {
                    memcpy(decodedImage.scanLine(y),
                           decodedFrame->data[0] + y * decodedFrame->linesize[0],
                           static_cast<size_t>(decodedFrame->width) * 4);
                }
                return true;
            }
            return false;
        }

        const int destW = decodedFrame->width;
        const int destH = decodedFrame->height;
        int destLinesize[4] = {0};
        uint8_t* destData[4] = {nullptr};
        const int bufferSize = av_image_alloc(destData, destLinesize, destW, destH, AV_PIX_FMT_RGBA, 32);
        if (bufferSize <= 0 || !destData[0]) {
            return false;
        }

        bool success = false;
        SwsContext* swsCtx = sws_getContext(decodedFrame->width,
                                            decodedFrame->height,
                                            static_cast<AVPixelFormat>(decodedFrame->format),
                                            destW,
                                            destH,
                                            AV_PIX_FMT_RGBA,
                                            SWS_BILINEAR,
                                            nullptr,
                                            nullptr,
                                            nullptr);
        if (swsCtx) {
            const int swsResult = sws_scale(swsCtx,
                                            decodedFrame->data,
                                            decodedFrame->linesize,
                                            0,
                                            decodedFrame->height,
                                            destData,
                                            destLinesize);
            sws_freeContext(swsCtx);

            if (swsResult > 0) {
                decodedImage = QImage(destW, destH, QImage::Format_RGBA8888);
                if (!decodedImage.isNull() && decodedImage.bits()) {
                    for (int y = 0; y < destH; ++y) {
                        memcpy(decodedImage.scanLine(y),
                               destData[0] + y * destLinesize[0],
                               static_cast<size_t>(destW) * 4);
                    }
                    success = true;
                }
            }
        }

        av_freep(&destData[0]);
        return success;
    };

    while (av_read_frame(formatCtx, packet) >= 0 && decodedImage.isNull()) {
        if (packet->stream_index == videoStreamIndex && avcodec_send_packet(codecCtx, packet) >= 0) {
            while (avcodec_receive_frame(codecCtx, frame) >= 0) {
                if (tryConvertFrame(frame)) {
                    av_frame_unref(frame);
                    break;
                }
                av_frame_unref(frame);
            }
        }
        av_packet_unref(packet);
    }

    if (decodedImage.isNull()) {
        avcodec_send_packet(codecCtx, nullptr);
        while (avcodec_receive_frame(codecCtx, frame) >= 0) {
            if (tryConvertFrame(frame)) {
                av_frame_unref(frame);
                break;
            }
            av_frame_unref(frame);
        }
    }

    av_frame_free(&frame);
    av_packet_free(&packet);
    avcodec_free_context(&codecCtx);
    avformat_close_input(&formatCtx);

    if (!decodedImage.isNull()) {
        if (decodedImage.width() <= 0 || decodedImage.height() <= 0 ||
            decodedImage.width() > 16384 || decodedImage.height() > 16384) {
            qWarning() << "Invalid decoded image dimensions:" << decodedImage.size() << "for file:" << framePath;
            return QImage();
        }

        constexpr qint64 kMaxDecodedBytes = 1024LL * 1024LL * 1024LL;
        if (static_cast<qint64>(decodedImage.width()) * decodedImage.height() * 4 > kMaxDecodedBytes) {
            qWarning() << "Decoded image too large:" << decodedImage.size() << "for file:" << framePath;
            return QImage();
        }

        if (!decodedImage.bits() || decodedImage.bytesPerLine() <= 0) {
            qWarning() << "Invalid image data before conversion for file:" << framePath;
            return QImage();
        }
    }

    return decodedImage;
}

} // namespace editor
