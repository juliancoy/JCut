#pragma once

#include "frame_handle.h"
#include "preview_surface.h"
#include "editor_shared.h"

#include <QColor>
#include <QPointF>
#include <QRect>
#include <QRectF>
#include <QSize>
#include <QSizeF>
#include <QString>
#include <QVector>
#include <vulkan/vulkan.h>

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
    bool transformOverrideActive = false;
    QString transformOverrideClipId;
    TimelineClip::TransformKeyframe transformOverride;
    bool transcriptOverrideActive = false;
    QString transcriptOverrideClipId;
    QPointF transcriptTranslationOverride;
    QSizeF transcriptSizeOverride;
    QVector<QPointF> correctionDraftPoints;
    bool speakerPickDragActive = false;
    QString speakerPickClipId;
    QPointF speakerPickStartPos;
    QPointF speakerPickCurrentPos;
    QString speakerPickHintClipId;
    QString hoveredFaceDetectionsClipId;
    QString hoveredFaceDetectionsId;
    int hoveredFaceDetectionsTrackId = -1;
    QPointF lastMousePos = QPointF(-10000.0, -10000.0);
};

struct VulkanPreviewClipFrameStatus {
    QString clipId;
    QString label;
    QString decodePath;
    int64_t requestedSourceFrame = -1;
    int64_t presentedSourceFrame = -1;
    QSize frameSize;
    bool active = false;
    bool exact = false;
    bool hasFrame = false;
    bool hardwareFrame = false;
    bool gpuTexture = false;
    bool cpuImage = false;
    bool exactFrameAvailable = false;
    bool selectedFrameAvailable = false;
    bool drawSuppressed = false;
    bool gradingBypassed = false;
    bool correctionsEnabled = true;
    bool correctionsApplied = false;
    bool correctionsSupported = false;
    bool curveLutApplied = false;
    bool curveLutSupported = false;
    bool gradingShaderActive = false;
    int correctionPolygonCount = 0;
    qreal maskFeather = 0.0;
    qreal maskFeatherGamma = 1.0;
    QRect targetRect;
    QRect fittedRect;
    TimelineClip::TransformKeyframe transform;
    TimelineClip::GradingKeyframe grading;
    QString missingReason;
    QString effectsPath;
    editor::FrameHandle frame;
    bool externalVulkanFrame = false;
    bool sampledFramePregraded = false;
    bool sampledFrameNeedsYFlip = false;
    VkPhysicalDevice externalPhysicalDevice = VK_NULL_HANDLE;
    VkDevice externalDevice = VK_NULL_HANDLE;
    VkQueue externalQueue = VK_NULL_HANDLE;
    uint32_t externalQueueFamilyIndex = UINT32_MAX;
    VkImage externalImage = VK_NULL_HANDLE;
    VkImageView externalImageView = VK_NULL_HANDLE;
    VkDeviceMemory externalImageMemory = VK_NULL_HANDLE;
    VkImageLayout externalImageLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    VkFormat externalImageFormat = VK_FORMAT_UNDEFINED;
    int externalReadySemaphoreFd = -1;
};

struct VulkanPreviewFacestreamOverlay {
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
    QVector<VulkanPreviewFacestreamOverlay> facedetectionsOverlays;
    QVector<VulkanPreviewFacestreamOverlay> rawDetectionOverlays;
    QVector<RenderSyncMarker> renderSyncMarkers;
    QVector<ExportRangeSegment> exportRanges;
    QString selectedClipId;
    PreviewSurface::ViewMode viewMode = PreviewSurface::ViewMode::Video;
    PreviewSurface::AudioDynamicsSettings audioDynamics;
    PreviewSurface::AudioVisualizationMode audioVisualizationMode = PreviewSurface::AudioVisualizationMode::Waveform;
    PreviewSurface::LoiaconoSpectrumSettings loiaconoSpectrumSettings;
    bool audioSpeakerHoverModalEnabled = true;
    bool audioWaveformVisible = true;
    qreal previewZoom = 1.0;
    QPointF previewPanOffset;
    bool correctionDrawMode = false;
    bool transcriptOverlayInteractionEnabled = false;
    bool titleOverlayInteractionOnly = false;
    bool faceStreamAssignmentInteractionEnabled = false;
    PreviewInteractionTransientState transient;
};
