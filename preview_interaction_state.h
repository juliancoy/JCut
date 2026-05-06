#pragma once

#include "preview_surface.h"
#include "editor_shared.h"

#include <QColor>
#include <QPointF>
#include <QRectF>
#include <QSize>
#include <QString>
#include <QVector>

enum class PreviewDragMode {
    None,
    Move,
    ResizeX,
    ResizeY,
    ResizeBoth,
};

struct PreviewInteractionTransientState {
    PreviewDragMode dragMode = PreviewDragMode::None;
    QPointF dragOriginPos;
    QRectF dragOriginBounds;
    TimelineClip::TransformKeyframe dragOriginTransform;
    QPointF dragOriginTranscriptTranslation;
    QVector<QPointF> correctionDraftPoints;
    bool speakerPickDragActive = false;
    QString speakerPickClipId;
    QPointF speakerPickStartPos;
    QPointF speakerPickCurrentPos;
    QString speakerPickHintClipId;
    QPointF lastMousePos = QPointF(-10000.0, -10000.0);
};

struct VulkanPreviewClipFrameStatus {
    QString clipId;
    QString label;
    int64_t requestedSourceFrame = -1;
    int64_t presentedSourceFrame = -1;
    QSize frameSize;
    bool active = false;
    bool exact = false;
    bool hasFrame = false;
    bool hardwareFrame = false;
    bool gpuTexture = false;
    bool cpuImage = false;
};

struct VulkanPreviewBoxstreamOverlay {
    QString clipId;
    QString streamId;
    QString source;
    int trackId = -1;
    int64_t sourceFrame = -1;
    QRectF boxNorm;
    qreal confidence = 0.0;
};

struct PreviewInteractionState {
    bool playing = false;
    bool audioMuted = false;
    qreal audioVolume = 0.8;
    int64_t currentFrame = 0;
    int64_t currentSample = 0;
    qreal currentFramePosition = 0.0;
    QSize outputSize = QSize(1080, 1920);
    QColor backgroundColor = QColor(Qt::black);
    int clipCount = 0;
    QVector<TimelineClip> clips;
    QVector<TimelineTrack> tracks;
    QVector<VulkanPreviewClipFrameStatus> vulkanFrameStatuses;
    QVector<VulkanPreviewBoxstreamOverlay> boxstreamOverlays;
    QVector<RenderSyncMarker> renderSyncMarkers;
    QVector<ExportRangeSegment> exportRanges;
    QString selectedClipId;
    PreviewSurface::ViewMode viewMode = PreviewSurface::ViewMode::Video;
    qreal previewZoom = 1.0;
    QPointF previewPanOffset;
    bool correctionDrawMode = false;
    bool transcriptOverlayInteractionEnabled = false;
    bool titleOverlayInteractionOnly = false;
    PreviewInteractionTransientState transient;
};
