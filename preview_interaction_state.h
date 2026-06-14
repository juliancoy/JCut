#pragma once

#include "background_fill_effect.h"
#include "frame_handle.h"
#include "preview_surface.h"
#include "editor_shared.h"

#include <QColor>
#include <QPointF>
#include <QRect>
#include <QRectF>
#include <QSet>
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
    bool faceDetectionsRightClickHandled = false;
    QPointF lastMousePos = QPointF(-10000.0, -10000.0);
};

struct VulkanPreviewClipFrameStatus {
    QString clipId;
    QString label;
    QString decodePath;
    QString frameSelection;
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
    bool staleFrameRejected = false;
    bool upToDate = false;
    bool currentFrameFailure = false;
    bool drawSuppressed = false;
    bool gradingBypassed = false;
    bool correctionsEnabled = true;
    bool correctionsApplied = false;
    bool correctionsSupported = false;
    bool curveLutApplied = false;
    bool curveLutSupported = false;
    bool gradingShaderActive = false;
    bool speakerFramingEnabled = false;
    bool speakerFramingDynamic = false;
    int speakerFramingKeyframeCount = 0;
    int speakerFramingTargetKeyframeCount = 0;
    int speakerFramingEnabledKeyframeCount = 0;
    int speakerFramingCenterSmoothingFrames = 0;
    int speakerFramingZoomSmoothingFrames = 0;
    int speakerFramingSmoothingMode = 0;
    qreal speakerFramingCenterSmoothingStrength = 0.0;
    qreal speakerFramingZoomSmoothingStrength = 0.0;
    qreal speakerFramingTargetX = 0.0;
    qreal speakerFramingTargetY = 0.0;
    qreal speakerFramingTargetBox = -1.0;
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
    BackgroundFillEffect backgroundFillEffect = kDefaultBackgroundFillEffect;
    int clipCount = 0;
    QVector<TimelineClip> clips;
    QVector<TimelineTrack> tracks;
    QVector<VulkanPreviewClipFrameStatus> vulkanFrameStatuses;
    QVector<VulkanPreviewFacestreamOverlay> facedetectionsOverlays;
    QVector<VulkanPreviewFacestreamOverlay> rawDetectionOverlays;
    QSet<int> selectedSpeakerAssignedFaceTrackIds;
    QVector<RenderSyncMarker> renderSyncMarkers;
    QVector<ExportRangeSegment> exportRanges;
    QString selectedClipId;
    PreviewSurface::ViewMode viewMode = PreviewSurface::ViewMode::Video;
    PreviewSurface::AudioDynamicsSettings audioDynamics;
    PreviewSurface::AudioVisualizationMode audioVisualizationMode = PreviewSurface::AudioVisualizationMode::Waveform;
    PreviewSurface::LoiaconoSpectrumSettings loiaconoSpectrumSettings;
    bool audioSpeakerHoverModalEnabled = true;
    bool audioWaveformVisible = true;
    bool hideOutsideOutputWindow = false;
    qreal previewZoom = 1.0;
    QPointF previewPanOffset;
    bool correctionDrawMode = false;
    bool transcriptOverlayInteractionEnabled = false;
    bool titleOverlayInteractionOnly = false;
    bool faceStreamAssignmentInteractionEnabled = false;
    bool showCurrentSpeakerName = false;
    bool showCurrentSpeakerOrganization = false;
    qreal currentSpeakerNameTextScale = 1.0;
    qreal currentSpeakerOrganizationTextScale = 1.0;
    qreal currentSpeakerNameVerticalPosition = 0.86;
    qreal currentSpeakerOrganizationVerticalPosition = 0.93;
    QColor currentSpeakerNameColor = QColor(QStringLiteral("#f4f8fc"));
    QColor currentSpeakerOrganizationColor = QColor(QStringLiteral("#b9d0e5"));
    QColor currentSpeakerBackgroundColor = QColor(8, 13, 20, 190);
    QColor currentSpeakerBorderColor = QColor(225, 236, 247, 120);
    qreal currentSpeakerBackgroundCornerRadius = 14.0;
    qreal currentSpeakerBorderWidth = 1.0;
    bool currentSpeakerShadowEnabled = true;
    QColor currentSpeakerShadowColor = QColor(0, 0, 0, 190);
    int transcriptPrependMs = 150;
    int transcriptPostpendMs = 70;
    QString playbackStatusOverlayText;
    qreal playbackStatusOverlayProgress = -1.0;
    QString temporalDebugOverlayText;
    PreviewInteractionTransientState transient;
};
