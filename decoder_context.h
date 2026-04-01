#pragma once

#include "editor_shared.h"
#include "frame_handle.h"

// VideoStreamInfo is defined in async_decoder.h
#include "async_decoder.h"

#include <QDateTime>
#include <QHash>
#include <QImage>
#include <QSize>
#include <QString>
#include <QStringList>
#include <QVector>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/hwcontext.h>
#include <libavutil/pixdesc.h>
#include <libswscale/swscale.h>
}

namespace editor {

class DecoderContext {
public:
    // sharedHwDevices: optional map from AVHWDeviceType (as int) to a
    // process-wide AVBufferRef*. When provided, initHardwareAccel() borrows a
    // reference instead of calling av_hwdevice_ctx_create().
    explicit DecoderContext(const QString& path,
                            const QHash<int, AVBufferRef*>* sharedHwDevices = nullptr);
    ~DecoderContext();

    bool initialize();
    void shutdown();

    FrameHandle decodeFrame(int64_t frameNumber);
    QVector<FrameHandle> decodeThroughFrame(int64_t targetFrame);

    const VideoStreamInfo& info() const { return m_info; }
    qint64 lastAccessTime() const { return m_lastAccessTime; }

    bool supportsSequenceBatchDecode() const {
        return m_isImageSequence || m_isStillImage;
    }

    bool isHardwareAccelerated() const { return m_hwDeviceCtx != nullptr; }

private:
    bool openInput();
    bool initCodec();
    bool initHardwareAccel(const AVCodec* decoder);

    bool loadStillImage();
    bool loadImageSequence();
    QImage loadCachedSequenceFrameImage(int64_t frameNumber);
    void cacheSequenceFrameImage(int64_t frameNumber, const QImage& image);
    void trimSequenceFrameCache();

    FrameHandle seekAndDecode(int64_t frameNumber);
    QVector<FrameHandle> decodeForwardUntil(int64_t targetFrame, bool forceSeek);
    bool seekToKeyframe(int64_t targetFrame);
    FrameHandle convertToFrame(AVFrame* avFrame, int64_t frameNumber);
    QImage convertAVFrameToImage(AVFrame* frame);
    void updateAccessTime();

private:
    QString m_path;
    VideoStreamInfo m_info;

    AVFormatContext* m_formatCtx = nullptr;
    AVCodecContext* m_codecCtx = nullptr;
    AVBufferRef* m_hwDeviceCtx = nullptr;
    bool m_ownsHwDeviceCtx = true; // false when borrowed from AsyncDecoder's shared pool
    AVPixelFormat m_hwPixFmt = AV_PIX_FMT_NONE;
    SwsContext* m_swsCtx = nullptr;
    AVFrame* m_convertFrame = nullptr;

    int m_videoStreamIndex = -1;
    int64_t m_lastDecodedFrame = -1;
    bool m_eof = false;

    bool m_isStillImage = false;
    bool m_isImageSequence = false;
    QImage m_stillImage;

    QStringList m_sequenceFramePaths;
    bool m_sequenceUsesWebp = false;
    QHash<int64_t, QImage> m_sequenceFrameCache;
    QHash<int64_t, quint64> m_sequenceFrameCacheUseOrder;
    size_t m_sequenceFrameCacheBytes = 0;
    quint64 m_sequenceFrameUseCounter = 0;

    bool m_streamHasAlphaTag = false;
    bool m_loggedSourceFormat = false;
    bool m_reportedAlphaMismatch = false;
    bool m_loggedAlphaProbe = false;

    // Optional pointer into AsyncDecoder's shared device map (not owned).
    const QHash<int, AVBufferRef*>* m_sharedHwDevices = nullptr;

    AVPixelFormat m_swsSourceFormat = AV_PIX_FMT_NONE;
    QSize m_swsSourceSize;
    QSize m_convertFrameSize;

    qint64 m_lastAccessTime = 0;
};

} // namespace editor
