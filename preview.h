#pragma once

#include <QOpenGLWidget>
#include <QOpenGLFunctions>
#include <QOpenGLBuffer>
#include <QOpenGLShaderProgram>
#include <QTimer>
#include <QHash>
#include <QJsonObject>
#include <QTransform>
#include <QImage>
#include <deque>
#include <memory>
#include <functional>

// Include frame_handle first to avoid conflicts with forward declarations
#include "frame_handle.h"
#include "gl_frame_texture_shared.h"
#include "editor_shared.h"
#include "timeline_widget.h"
#include "async_decoder.h"
#include "timeline_cache.h"
#include "playback_frame_pipeline.h"

using namespace editor;

class PreviewWindow final : public QOpenGLWidget, protected QOpenGLFunctions {
    Q_OBJECT
public:
    explicit PreviewWindow(QWidget* parent = nullptr);
    ~PreviewWindow() override;

    void setPlaybackState(bool playing);
    void setCurrentFrame(int64_t frame);
    void setCurrentPlaybackSample(int64_t samplePosition);
    void setClipCount(int count);
    void setSelectedClipId(const QString& clipId);
    void setTimelineClips(const QVector<TimelineClip>& clips);
    void setTimelineTracks(const QVector<TimelineTrack>& tracks);
    void setRenderSyncMarkers(const QVector<RenderSyncMarker>& markers);
    void setExportRanges(const QVector<ExportRangeSegment>& ranges);
    void invalidateTranscriptOverlayCache(const QString& clipFilePath = QString());
    void beginBulkUpdate();
    void endBulkUpdate();
    QString backendName() const;
    void setAudioMuted(bool muted);
    void setAudioVolume(qreal volume);
    void setOutputSize(const QSize& size);
    void setHideOutsideOutputWindow(bool hide);
    void setBypassGrading(bool bypass);
    void setCorrectionsEnabled(bool enabled);
    void setShowCorrectionOverlays(bool show) { m_showCorrectionOverlays = show; update(); }
    void setSelectedCorrectionPolygon(int polygonIndex) { m_selectedCorrectionPolygon = polygonIndex; update(); }
    void setBackgroundColor(const QColor& color);
    void setPreviewZoom(qreal zoom);
    void setTranscriptOverlayInteractionEnabled(bool enabled);
    void setCorrectionDrawMode(bool enabled) {
        if (m_correctionDrawMode == enabled) {
            return;
        }
        m_correctionDrawMode = enabled;
        if (m_correctionDrawMode) {
            m_dragMode = PreviewDragMode::None;
            m_dragOriginBounds = QRectF();
        }
        update();
    }
    bool correctionDrawMode() const { return m_correctionDrawMode; }
    bool transcriptOverlayInteractionEnabled() const { return m_transcriptOverlayInteractionEnabled; }
    void setCorrectionDraftPoints(const QVector<QPointF>& points) { m_correctionDraftPoints = points; update(); }
    qreal previewZoom() const { return m_previewZoom; }
    void resetPreviewPan() { m_previewPanOffset = QPointF(); }
    QSize outputSize() const { return m_outputSize; }
    bool bypassGrading() const;
    bool correctionsEnabled() const { return m_correctionsEnabled; }
    bool audioMuted() const;
    int audioVolumePercent() const;
    QString activeAudioClipLabel() const;
    bool preparePlaybackAdvance(int64_t targetFrame);
    bool preparePlaybackAdvanceSample(int64_t targetSample);
    QJsonObject profilingSnapshot() const;
    void resetProfilingStats();
    bool selectedOverlayIsTranscript() const {
        return !m_selectedClipId.isEmpty() &&
               m_overlayInfo.value(m_selectedClipId).kind == PreviewOverlayKind::TranscriptOverlay;
    }

    std::function<void(const QString&)> selectionRequested;
    std::function<void(const QString&, qreal, qreal, bool)> resizeRequested;
    std::function<void(const QString&, qreal, qreal, bool)> moveRequested;
    std::function<void(const QString&)> createKeyframeRequested;
    std::function<void(const QString&, qreal, qreal)> correctionPointRequested;

protected:
    void paintEvent(QPaintEvent* event) override;
    void initializeGL() override;
    void resizeGL(int w, int h) override;
    void showEvent(QShowEvent* event) override;
    void paintGL() override;
    void mousePressEvent(QMouseEvent* event) override;
    void mouseMoveEvent(QMouseEvent* event) override;
    void mouseReleaseEvent(QMouseEvent* event) override;
    void wheelEvent(QWheelEvent* event) override;
    void contextMenuEvent(QContextMenuEvent* event) override;

private:
    enum class PreviewOverlayKind {
        VisualClip,
        TranscriptOverlay,
    };

    struct PreviewOverlayInfo {
        PreviewOverlayKind kind = PreviewOverlayKind::VisualClip;
        QRectF bounds;
        QRectF rightHandle;
        QRectF bottomHandle;
        QRectF cornerHandle;
        QTransform clipTransform;
        QSizeF clipPixelSize;
    };

    enum class PreviewDragMode {
        None,
        Move,
        ResizeX,
        ResizeY,
        ResizeBoth,
    };

    QRect previewCanvasBaseRect() const;
    QRect scaledCanvasRect(const QRect& baseRect) const;
    QPointF previewCanvasScale(const QRect& targetRect) const;
    bool clipShowsTranscriptOverlay(const TimelineClip& clip) const;
    const QVector<TranscriptSection>& transcriptSectionsForClip(const TimelineClip& clip) const;
    TranscriptOverlayLayout transcriptOverlayLayoutForClip(const TimelineClip& clip) const;
    QRectF transcriptOverlayRectForTarget(const TimelineClip& clip, const QRect& targetRect) const;
    QSizeF transcriptOverlaySizeForSelectedClip() const;
    void drawTranscriptOverlay(QPainter* painter, const TimelineClip& clip, const QRect& targetRect);
    void drawTranscriptOverlayGL(const TimelineClip& clip, const QRect& targetRect);
    QString transcriptOverlayTextureKey(const TimelineClip& clip,
                                        const QRectF& bounds,
                                        const QRectF& textBounds,
                                        int fontPixelSize,
                                        const QString& shadowHtml,
                                        const QString& textHtml) const;
    QImage renderTranscriptOverlayImage(const TimelineClip& clip,
                                        const QRectF& bounds,
                                        const QRectF& textBounds,
                                        int fontPixelSize,
                                        const QString& shadowHtml,
                                        const QString& textHtml) const;
    GLuint textureForTranscriptOverlay(const QString& key, const QImage& image);
    void trimTranscriptTextureCache();
    bool usingCpuFallback() const;
    void ensurePipeline();
    void releaseGlResources();
    GLuint textureForFrame(const FrameHandle& frame);
    void trimTextureCache();
    bool isSampleWithinClip(const TimelineClip& clip, int64_t samplePosition) const;
    int64_t sourceSampleForPlaybackSample(const TimelineClip& clip, int64_t samplePosition) const;
    int64_t sourceFrameForSample(const TimelineClip& clip, int64_t samplePosition) const;
    bool isFrameTooStaleForPlayback(const TimelineClip& clip,
                                    int64_t localFrame,
                                    const FrameHandle& frame) const;
    PreviewOverlayInfo renderFrameLayerGL(const QRect& targetRect, const TimelineClip& clip, const FrameHandle& frame);
    void renderCompositedPreviewGL(const QRect& compositeRect,
                                   const QList<TimelineClip>& activeClips,
                                   bool& drewAnyFrame,
                                   bool& waitingForFrame);
    void drawCompositedPreviewOverlay(QPainter* painter,
                                      const QRect& safeRect,
                                      const QRect& compositeRect,
                                      const QList<TimelineClip>& activeClips,
                                      bool drewAnyFrame,
                                      bool waitingForFrame);
    void drawBackground(QPainter* painter);
    QList<TimelineClip> getActiveClips() const;
    void requestFramesForCurrentPosition();
    void scheduleFrameRequest();
    void scheduleRepaint();
    void drawCompositedPreview(QPainter* painter, const QRect& safeRect,
                               const QList<TimelineClip>& activeClips);
    void drawEmptyState(QPainter* painter, const QRect& safeRect);
    void drawFrameLayer(QPainter* painter, const QRect& targetRect,
                        const TimelineClip& clip, const FrameHandle& frame);
    void drawFramePlaceholder(QPainter* painter, const QRect& targetRect,
                              const TimelineClip& clip, const QString& message);
    void drawAudioPlaceholder(QPainter* painter, const QRect& safeRect,
                              const QList<TimelineClip>& activeAudioClips);
    void drawAudioBadge(QPainter* painter, const QRect& targetRect,
                        const QList<TimelineClip>& activeAudioClips);
    QRect fitRect(const QSize& source, const QRect& bounds) const;
    QPointF mapNormalizedClipPointToScreen(const PreviewOverlayInfo& info, const QPointF& normalizedPoint) const;
    QPointF mapScreenPointToNormalizedClip(const PreviewOverlayInfo& info, const QPointF& screenPoint) const;
    void drawPreviewChrome(QPainter* painter, const QRect& safeRect, int activeClipCount) const;
    QString clipIdAtPosition(const QPointF& position) const;
    TimelineClip::TransformKeyframe evaluateTransformForSelectedClip() const;
    void updatePreviewCursor(const QPointF& position);

    std::unique_ptr<AsyncDecoder> m_decoder;
    std::unique_ptr<TimelineCache> m_cache;
    std::unique_ptr<PlaybackFramePipeline> m_playbackPipeline;
    std::unique_ptr<QOpenGLShaderProgram> m_shaderProgram;
    std::unique_ptr<QOpenGLShaderProgram> m_correctionMaskShaderProgram;
    std::unique_ptr<QOpenGLShaderProgram> m_overlayShaderProgram;
    QOpenGLBuffer m_quadBuffer;
    QOpenGLBuffer m_polygonBuffer;

    bool m_glInitialized = false;
    bool m_glResourcesReleased = false;
    QColor m_backgroundColor = QColor(Qt::black);
    bool m_playing = false;
    bool m_audioMuted = false;
    qreal m_audioVolume = 0.8;
    bool m_bypassGrading = false;
    bool m_correctionsEnabled = true;
    int64_t m_currentFrame = 0;
    int64_t m_currentSample = 0;
    qreal m_currentFramePosition = 0.0;
    int m_clipCount = 0;
    QVector<TimelineClip> m_clips;
    QVector<TimelineTrack> m_tracks;
    QVector<RenderSyncMarker> m_renderSyncMarkers;
    QSet<QString> m_registeredClips;
    QTimer m_repaintTimer;
    QTimer m_frameRequestTimer;
    qint64 m_lastFrameRequestMs = 0;
    qint64 m_lastFrameReadyMs = 0;
    qint64 m_lastPaintMs = 0;
    qint64 m_waitingForFrameSinceMs = 0;
    qint64 m_lastRepaintScheduleMs = 0;
    qint64 m_lastRenderDurationMs = 0;
    qint64 m_maxRenderDurationMs = 0;
    qint64 m_renderCount = 0;
    qint64 m_totalRenderDurationMs = 0;
    QString m_selectedClipId;
    QSize m_outputSize = QSize(1080, 1920);
    bool m_hideOutsideOutputWindow = false;
    qreal m_previewZoom = 1.0;
    QPointF m_previewPanOffset;
    QHash<QString, PreviewOverlayInfo> m_overlayInfo;
    mutable QHash<QString, QVector<TranscriptSection>> m_transcriptSectionsCache;
    QHash<QString, editor::GlTextureCacheEntry> m_textureCache;
    QHash<QString, editor::GlTextureCacheEntry> m_transcriptTextureCache;
    QHash<QString, FrameHandle> m_lastPresentedFrames;
    mutable QJsonObject m_lastFrameSelectionStats;
    static constexpr int kRenderTimeHistorySize = 60;
    std::deque<qint64> m_renderTimeHistory;
    QVector<QString> m_paintOrder;
    int m_bulkUpdateDepth = 0;
    bool m_pendingFrameRequest = false;
    bool m_frameRequestsArmed = false;
    PreviewDragMode m_dragMode = PreviewDragMode::None;
    QPointF m_dragOriginPos;
    QRectF m_dragOriginBounds;
    TimelineClip::TransformKeyframe m_dragOriginTransform;
    bool m_correctionDrawMode = false;
    bool m_transcriptOverlayInteractionEnabled = false;
    bool m_showCorrectionOverlays = false;
    int m_selectedCorrectionPolygon = -1;
    QVector<QPointF> m_correctionDraftPoints;
};
