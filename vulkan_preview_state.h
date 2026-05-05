#pragma once

#include "preview_surface.h"
#include "editor_shared.h"

#include <QColor>
#include <QImage>
#include <QJsonObject>
#include <QSize>
#include <QString>

#include <cstdint>

struct VulkanRendererState {
    bool playing = false;
    int64_t currentFrame = 0;
    int64_t currentPlaybackSample = 0;
    int clipCount = 0;
    QVector<TimelineClip> clips;
    QVector<TimelineTrack> tracks;
    QVector<RenderSyncMarker> renderSyncMarkers;
    QString selectedClipId;
    QString backendPreference = QStringLiteral("vulkan");
    bool audioMuted = false;
    qreal audioVolume = 1.0;
    QSize outputSize = QSize(1080, 1920);
    bool hideOutsideOutputWindow = false;
    bool bypassGrading = false;
    bool correctionsEnabled = true;
    bool showCorrectionOverlays = true;
    int selectedCorrectionPolygon = -1;
    QColor backgroundColor = QColor(QStringLiteral("#111111"));
    qreal previewZoom = 1.0;
    bool showSpeakerTrackPoints = false;
    bool showSpeakerTrackBoxes = false;
    QString boxstreamOverlaySource = QStringLiteral("all");
    bool audioSpeakerHoverModalEnabled = true;
    bool audioWaveformVisible = true;
    PreviewSurface::ViewMode viewMode = PreviewSurface::ViewMode::Video;
    PreviewSurface::AudioDynamicsSettings audioDynamics;
    bool transcriptOverlayInteractionEnabled = true;
    bool titleOverlayInteractionOnly = false;
    bool correctionDrawMode = false;
    QJsonObject profiling;
    QImage latestFrame;
    qint64 composeSuccessCount = 0;
    qint64 composeFailureCount = 0;
    qint64 scaffoldFallbackCount = 0;
};
