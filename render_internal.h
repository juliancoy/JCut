#pragma once

#include "render.h"

#include "async_decoder.h"
#include "debug_controls.h"
#include "gl_frame_texture_shared.h"
#include "media_pipeline_shared.h"
#include "render_cpu_fallback.h"

#include <QDir>
#include <QDateTime>
#include <QEventLoop>
#include <QFile>
#include <QFileInfo>
#include <QHash>
#include <QImage>
#include <QJsonArray>
#include <QJsonObject>
#include <QOffscreenSurface>
#include <QOpenGLBuffer>
#include <QOpenGLContext>
#include <QOpenGLFramebufferObject>
#include <QOpenGLFunctions>
#include <QOpenGLShaderProgram>
#include <QPainter>
#include <QTextDocument>
#include <QSurfaceFormat>
#include <QElapsedTimer>
#include <QTimer>

#include <algorithm>
#include <memory>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/audio_fifo.h>
#include <libavutil/avutil.h>
#include <libavutil/channel_layout.h>
#include <libavutil/imgutils.h>
#include <libavutil/opt.h>
#include <libavutil/samplefmt.h>
#include <libavutil/hwcontext.h>
#ifdef EDITOR_HAS_CUDA
#include <libavutil/hwcontext_cuda.h>
#endif
#include <libswresample/swresample.h>
#include <libswscale/swscale.h>
}

#ifdef EDITOR_HAS_CUDA
#include <cuda.h>
#include <cudaGL.h>
#endif

namespace render_detail {

inline constexpr int kRenderAudioSampleRate = 48000;
inline constexpr int kRenderAudioChannels = 2;

struct VideoEncoderChoice {
    QString label;
    AVPixelFormat pixelFormat = AV_PIX_FMT_YUV420P;
};

struct RenderClipStageStats {
    QString id;
    QString label;
    int64_t frames = 0;
    qint64 decodeMs = 0;
    qint64 textureMs = 0;
    qint64 compositeMs = 0;
};

struct RenderFrameStageStats {
    int64_t timelineFrame = -1;
    int segmentIndex = 0;
    qint64 renderMs = 0;
    qint64 decodeMs = 0;
    qint64 textureMs = 0;
    qint64 readbackMs = 0;
    qint64 convertMs = 0;
};

struct RenderAsyncFrameKey {
    QString path;
    int64_t frameNumber = -1;

    bool operator==(const RenderAsyncFrameKey& other) const {
        return path == other.path && frameNumber == other.frameNumber;
    }
};

size_t qHash(const RenderAsyncFrameKey& key, size_t seed = 0);
bool isHardwareEncoderLabel(const QString& codecLabel);
void recordRenderSkip(QJsonArray* skippedClips,
                      QJsonObject* skippedReasonCounts,
                      const TimelineClip& clip,
                      const QString& reason,
                      int64_t timelineFrame,
                      int64_t localFrame = -1);
void accumulateClipStageStats(QHash<QString, RenderClipStageStats>* clipStageStats,
                              const TimelineClip& clip,
                              qint64 decodeMs,
                              qint64 textureMs,
                              qint64 compositeMs);
QJsonObject buildRenderStageTable(const QHash<QString, RenderClipStageStats>& clipStageStats,
                                  qint64 totalRenderStageMs,
                                  int64_t completedFrames);
void recordWorstFrame(QVector<RenderFrameStageStats>* worstFrames,
                      const RenderFrameStageStats& stats,
                      int maxEntries = 8);
QJsonObject buildWorstFrameTable(const QVector<RenderFrameStageStats>& worstFrames);

void enqueueRenderSequenceLookahead(const RenderRequest& request,
                                    int64_t timelineFrame,
                                    const QVector<TimelineClip>& orderedClips,
                                    editor::AsyncDecoder* asyncDecoder,
                                    const QHash<RenderAsyncFrameKey, editor::FrameHandle>& asyncFrameCache);
void prewarmRenderSequenceSegment(const RenderRequest& request,
                                  int64_t segmentStartFrame,
                                  int64_t segmentEndFrame,
                                  const QVector<TimelineClip>& orderedClips,
                                  editor::AsyncDecoder* asyncDecoder,
                                  const QHash<RenderAsyncFrameKey, editor::FrameHandle>& asyncFrameCache);
editor::FrameHandle decodeRenderFrame(const QString& path,
                                      int64_t frameNumber,
                                      QHash<QString, editor::DecoderContext*>& decoders,
                                      editor::AsyncDecoder* asyncDecoder,
                                      QHash<RenderAsyncFrameKey, editor::FrameHandle>* asyncFrameCache);
QString avErrToString(int errnum);
QRect fitRect(const QSize& source, const QSize& bounds);
TranscriptOverlayLayout transcriptOverlayLayoutForFrame(const TimelineClip& clip,
                                                        int64_t timelineFrame,
                                                        const QVector<RenderSyncMarker>& markers,
                                                        QHash<QString, QVector<TranscriptSection>>& transcriptCache);
void renderTranscriptOverlays(QImage* canvas,
                              const RenderRequest& request,
                              int64_t timelineFrame,
                              const QVector<TimelineClip>& orderedClips,
                              QHash<QString, QVector<TranscriptSection>>& transcriptCache);

const AVCodec* codecForRequest(const QString& outputFormat, QString* codecLabel);
QVector<VideoEncoderChoice> videoEncoderChoicesForRequest(const QString& outputFormat);
const AVCodec* audioCodecForRequest(const QString& outputFormat, QString* codecLabel);
AVPixelFormat pixelFormatForCodec(const AVCodec* codec, const QString& outputFormat);
void configureCodecOptions(AVCodecContext* codecCtx, const QString& outputFormat, const QString& codecLabel = QString());
void configureAudioCodecOptions(AVCodecContext* codecCtx, const QString& outputFormat);
AVSampleFormat audioSampleFormatForCodec(const AVCodec* codec);
int audioSampleRateForCodec(const AVCodec* codec);

bool isImageSequenceFormat(const QString& outputFormat);
RenderResult renderTimelineToImageSequence(const RenderRequest& request,
                                          const std::function<bool(const RenderProgress&)>& progressCallback);

struct DecodedAudioClip {
    QVector<float> samples;
    bool valid = false;
};

struct AudioExportState {
    QVector<TimelineClip> clips;
    QVector<RenderSyncMarker> renderSyncMarkers;
    QHash<QString, DecodedAudioClip> cache;
    AVStream* stream = nullptr;
    AVCodecContext* codecCtx = nullptr;
    bool enabled = false;
};

DecodedAudioClip decodeClipAudio(const QString& path);
void mixAudioChunk(const QVector<TimelineClip>& clips,
                   const QVector<RenderSyncMarker>& renderSyncMarkers,
                   const QHash<QString, DecodedAudioClip>& audioCache,
                   float* output,
                   int frames,
                   int64_t chunkStartSample);
bool encodeFrame(AVCodecContext* codecCtx,
                 AVStream* stream,
                 AVFormatContext* formatCtx,
                 AVFrame* frame,
                 QString* errorMessage);
bool initializeExportAudio(const RenderRequest& request,
                           AVFormatContext* formatCtx,
                           AudioExportState* state,
                           QString* errorMessage);
bool encodeExportAudio(const QVector<ExportRangeSegment>& exportRanges,
                       const AudioExportState& state,
                       AVFormatContext* formatCtx,
                       QString* errorMessage);

QVector<TimelineClip> sortedVisualClips(const QVector<TimelineClip>& clips,
                                        const QVector<TimelineTrack>& tracks);

class OffscreenGpuRendererPrivate;

class OffscreenGpuRenderer {
public:
    OffscreenGpuRenderer();
    ~OffscreenGpuRenderer();

    bool initialize(const QSize& outputSize, QString* errorMessage);
    QImage renderFrame(const RenderRequest& request,
                       int64_t timelineFrame,
                       QHash<QString, editor::DecoderContext*>& decoders,
                       editor::AsyncDecoder* asyncDecoder,
                       QHash<RenderAsyncFrameKey, editor::FrameHandle>* asyncFrameCache,
                       const QVector<TimelineClip>& orderedClips,
                       QHash<QString, RenderClipStageStats>* clipStageStats = nullptr,
                       qint64* decodeMs = nullptr,
                       qint64* textureMs = nullptr,
                       qint64* compositeMs = nullptr,
                       qint64* readbackMs = nullptr,
                       QJsonArray* skippedClips = nullptr,
                       QJsonObject* skippedReasonCounts = nullptr);
    bool convertLastFrameToNv12(AVFrame* frame,
                                qint64* nv12ConvertMs = nullptr,
                                qint64* readbackMs = nullptr);

private:
    std::unique_ptr<OffscreenGpuRendererPrivate> d;
};

QImage renderTimelineFrame(const RenderRequest& request,
                           int64_t timelineFrame,
                           QHash<QString, editor::DecoderContext*>& decoders,
                           editor::AsyncDecoder* asyncDecoder,
                           QHash<RenderAsyncFrameKey, editor::FrameHandle>* asyncFrameCache,
                           const QVector<TimelineClip>& orderedClips,
                           QHash<QString, RenderClipStageStats>* clipStageStats = nullptr,
                           QJsonArray* skippedClips = nullptr,
                           QJsonObject* skippedReasonCounts = nullptr);

RenderResult renderTimelineToImageSequenceAndVideo(const RenderRequest& request,
                                                  const std::function<bool(const RenderProgress&)>& progressCallback);

} // namespace render_detail
