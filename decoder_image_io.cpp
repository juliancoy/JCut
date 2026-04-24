#include "decoder_image_io.h"
#include "decoder_ffmpeg_utils.h"

#include <QDebug>
#include <QFile>
#include <QFileInfo>
#include <QImageReader>
#include <QSet>
#include <QtConcurrent/QtConcurrent>

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

    // Always try the Qt reader first (which might have a fast WebP plugin).
    // If it fails, we fall through to the FFmpeg path.
    if (true) {
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

        // If Qt fails, we try the fast path below before falling back to the full FFmpeg pipe.
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

    QImage decodedImage;

    auto tryConvertFrame = [&](AVFrame* decodedFrame) -> bool {
        if (!decodedFrame || decodedFrame->width <= 0 || decodedFrame->height <= 0 ||
            decodedFrame->width > 16384 || decodedFrame->height > 16384) {
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

    static thread_local AVCodecContext* tl_webpCtx = nullptr;
    static thread_local const AVCodec* tl_webpDecoder = nullptr;

    // Fast path for WebP: decode directly from file bytes using avcodec, bypassing avformat probing
    if (suffix == QStringLiteral("webp")) {
        QFile file(framePath);
        if (file.open(QIODevice::ReadOnly)) {
            const QByteArray fileData = file.readAll();
            if (!fileData.isEmpty()) {
                if (!tl_webpDecoder) {
                    tl_webpDecoder = avcodec_find_decoder(AV_CODEC_ID_WEBP);
                }
                if (tl_webpDecoder) {
                    if (!tl_webpCtx) {
                        tl_webpCtx = avcodec_alloc_context3(tl_webpDecoder);
                        if (tl_webpCtx) {
                            avcodec_open2(tl_webpCtx, tl_webpDecoder, nullptr);
                        }
                    }

                    if (tl_webpCtx) {
                        AVPacket* wPacket = av_packet_alloc();
                        AVFrame* wFrame = av_frame_alloc();
                        wPacket->data = (uint8_t*)fileData.constData();
                        wPacket->size = fileData.size();

                        if (avcodec_send_packet(tl_webpCtx, wPacket) == 0 &&
                            avcodec_receive_frame(tl_webpCtx, wFrame) == 0) {
                            if (tryConvertFrame(wFrame)) {
                                av_frame_free(&wFrame);
                                av_packet_free(&wPacket);
                                return decodedImage;
                            }
                        }
                        av_frame_free(&wFrame);
                        av_packet_free(&wPacket);
                        // If decode fails, flush to reset state for next attempt
                        avcodec_flush_buffers(tl_webpCtx);
                    }
                }
            }
        }
    }

    // Only log if we fall through all optimized paths for a WebP file
    if (suffix == QStringLiteral("webp")) {
        qDebug().noquote() << "[DECODE][WARN] WebP fast path failed for:" << framePath << "falling through to slow avformat path";
    }

    AVFormatContext* formatCtx = nullptr;
    const QByteArray pathBytes = QFile::encodeName(framePath);

    const AVInputFormat* inputFormat = nullptr;
    if (suffix == QStringLiteral("webp")) {
        inputFormat = av_find_input_format("webp_pipe");
    } else {
        inputFormat = av_find_input_format("image2");
    }

    // libavformat's avformat_open_input() uses AVInputFormat* in some ABI variants
    // even though av_find_input_format() returns const AVInputFormat*.
    AVInputFormat* openInputFormat = const_cast<AVInputFormat*>(inputFormat);
    if (avformat_open_input(&formatCtx, pathBytes.constData(), openInputFormat, nullptr) < 0) {
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
        (applyVideoDecoderThreadingPolicy(codecCtx,
                                          decoder,
                                          stream->codecpar->codec_id,
                                          false,
                                          false),
         avcodec_open2(codecCtx, decoder, nullptr) < 0)) {
        avcodec_free_context(&codecCtx);
        avformat_close_input(&formatCtx);
        return QImage();
    }

    AVPacket* packet = av_packet_alloc();
    AVFrame* vFrame = av_frame_alloc();
    if (!packet || !vFrame) {
        if (packet) {
            av_packet_free(&packet);
        }
        if (vFrame) {
            av_frame_free(&vFrame);
        }
        avcodec_free_context(&codecCtx);
        avformat_close_input(&formatCtx);
        return QImage();
    }

    while (av_read_frame(formatCtx, packet) >= 0 && decodedImage.isNull()) {
        if (packet->stream_index == videoStreamIndex && avcodec_send_packet(codecCtx, packet) >= 0) {
            while (avcodec_receive_frame(codecCtx, vFrame) >= 0) {
                if (tryConvertFrame(vFrame)) {
                    av_frame_unref(vFrame);
                    break;
                }
                av_frame_unref(vFrame);
            }
        }
        av_packet_unref(packet);
    }

    if (decodedImage.isNull()) {
        avcodec_send_packet(codecCtx, nullptr);
        while (avcodec_receive_frame(codecCtx, vFrame) >= 0) {
            if (tryConvertFrame(vFrame)) {
                av_frame_unref(vFrame);
                break;
            }
            av_frame_unref(vFrame);
        }
    }

    av_frame_free(&vFrame);
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

QVector<QImage> loadImageSequenceBatch(const QStringList& framePaths, const QVector<int64_t>& frameNumbers) {
    QVector<QImage> results;
    if (framePaths.isEmpty() || frameNumbers.isEmpty()) {
        return results;
    }
    
    results.reserve(frameNumbers.size());
    
    // Simple parallel loading using QtConcurrent
    // This is more efficient than sequential loading for many small files
    QVector<QImage> loadedImages = QtConcurrent::blockingMapped<QVector<QImage>>(
        frameNumbers,
        [&framePaths](int64_t frameNumber) -> QImage {
            const int index = qBound(0, static_cast<int>(frameNumber), framePaths.size() - 1);
            return loadSingleImageFile(framePaths.at(index));
        }
    );
    
    // Transfer results
    results = std::move(loadedImages);
    return results;
}

QImage loadSequenceFrameImageBatched(const QStringList& framePaths, int64_t frameNumber, 
                                     QVector<int64_t>& additionalFrames, QVector<QImage>& batchCache) {
    if (framePaths.isEmpty()) {
        return QImage();
    }
    
    // Check if we have this frame in cache
    for (int i = 0; i < additionalFrames.size(); ++i) {
        if (additionalFrames[i] == frameNumber && i < batchCache.size()) {
            return batchCache[i];
        }
    }
    
    // If not in cache, load a batch
    QVector<int64_t> framesToLoad;
    framesToLoad.reserve(additionalFrames.size() + 1);
    framesToLoad.append(frameNumber);
    
    for (int64_t additionalFrame : additionalFrames) {
        if (additionalFrame >= 0 && additionalFrame < framePaths.size()) {
            framesToLoad.append(additionalFrame);
        }
    }
    
    // Remove duplicates
    std::sort(framesToLoad.begin(), framesToLoad.end());
    framesToLoad.erase(std::unique(framesToLoad.begin(), framesToLoad.end()), framesToLoad.end());
    
    // Load batch
    QVector<QImage> batch = loadImageSequenceBatch(framePaths, framesToLoad);
    
    // Update cache
    additionalFrames = framesToLoad;
    batchCache = batch;
    
    // Return requested frame
    for (int i = 0; i < framesToLoad.size(); ++i) {
        if (framesToLoad[i] == frameNumber && i < batch.size()) {
            return batch[i];
        }
    }
    
    // Fallback to single load
    return loadSequenceFrameImage(framePaths, frameNumber);
}

} // namespace editor
