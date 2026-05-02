#pragma once

#include "preview_surface.h"

#include <QColor>
#include <QImage>
#include <QJsonObject>
#include <QSize>
#include <QString>
#include <QVulkanWindow>

#include <cstdint>

class QVulkanWindowRenderer;
class QVulkanInstance;

struct VulkanRendererState {
    bool playing = false;
    int64_t currentFrame = 0;
    int64_t currentPlaybackSample = 0;
    int clipCount = 0;
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
};

class VulkanNativeWindow final : public QVulkanWindow {
public:
    explicit VulkanNativeWindow(VulkanRendererState* state);
    QVulkanWindowRenderer* createRenderer() override;

private:
    VulkanRendererState* m_state = nullptr;
};

VulkanNativeWindow* createVulkanNativeWindow(VulkanRendererState* state, QVulkanInstance* instance);
