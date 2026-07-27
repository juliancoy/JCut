#pragma once

#include "direct_vulkan_preview_presenter.h"

#include <functional>
#include <QByteArrayList>
#include <QImage>
#include <QWidget>

class DirectVulkanPreviewWindow;
class QVulkanInstance;
struct QVulkanExtension;
template <typename T> class QVulkanInfoVector;

QWidget* createDirectVulkanPreviewHostWidget(PreviewInteractionState* state,
                                             std::function<void()> updateCallback,
                                             QWidget* parent);
QWidget* createDirectVulkanPreviewWindowContainer(DirectVulkanPreviewWindow* window,
                                                  QWidget* parent);
DirectVulkanPreviewWindow* createDirectVulkanPreviewWindow(
    PreviewInteractionState* state,
    DirectVulkanPresentationTelemetry* presentationTelemetry,
    DirectVulkanPreviewStats* stats,
    bool* active,
    QString* failureReason,
    bool enableAudioPipeline = true,
    std::function<void(const QString&)> failureCallback = {});
void directVulkanPreviewWindowSetVulkanInstance(DirectVulkanPreviewWindow* window,
                                                QVulkanInstance* instance);
QVulkanInfoVector<QVulkanExtension> directVulkanPreviewWindowSupportedDeviceExtensions(
    DirectVulkanPreviewWindow* window);
void directVulkanPreviewWindowSetDeviceExtensions(DirectVulkanPreviewWindow* window,
                                                  const QByteArrayList& extensions);
void directVulkanPreviewWindowResize(DirectVulkanPreviewWindow* window, const QSize& size);
void directVulkanPreviewWindowSetInteractionCallbacks(
    DirectVulkanPreviewWindow* window,
    std::function<void(const QString&)> selectionRequested,
    std::function<void(const QString&, qreal, qreal, bool)> moveRequested,
    std::function<void(const QString&, qreal, qreal, qreal, qreal, bool)> transformRequested = {},
    std::function<void(int64_t)> playbackSampleRequested = {},
    std::function<void(const QString&, qreal, qreal)> correctionPointRequested = {},
    std::function<void(const QString&, qreal, qreal)> speakerPointRequested = {},
    std::function<void(const QString&, qreal, qreal, qreal)> speakerBoxRequested = {},
    std::function<void(const QString&, int, const QString&, int64_t, qreal, qreal, qreal)> faceStreamBoxRequested = {},
    std::function<void(const QString&, int, const QString&, int64_t, qreal, qreal, qreal)> faceStreamBoxFocusClearRequested = {},
    std::function<void(const QString&)> faceStreamBoxClickStatus = {},
    std::function<void(const QString&)> createKeyframeRequested = {});
bool directVulkanPreviewWindowUpdatePending(DirectVulkanPreviewWindow* window);
bool directVulkanPreviewWindowIsValid(DirectVulkanPreviewWindow* window);
void directVulkanPreviewWindowSchedulePreviewUpdate(DirectVulkanPreviewWindow* window);
void directVulkanPreviewWindowResetProfilingAnchors(
    DirectVulkanPreviewWindow* window);
void directVulkanPreviewWindowRequestPipelineThumbnailReadback(DirectVulkanPreviewWindow* window);
QImage directVulkanPreviewWindowLatestPipelineThumbnailReadback(DirectVulkanPreviewWindow* window);
bool directVulkanPreviewWindowPipelineThumbnailReadbackPending(DirectVulkanPreviewWindow* window);
void directVulkanPreviewWindowRaise(DirectVulkanPreviewWindow* window);
void directVulkanPreviewWindowHide(DirectVulkanPreviewWindow* window);
void directVulkanPreviewWindowSetTitle(DirectVulkanPreviewWindow* window, const QString& title);
void directVulkanPreviewWindowSetGpuExportPreviewFrame(
    DirectVulkanPreviewWindow* window,
    const render_detail::OffscreenVulkanFrame& frame);
void directVulkanPreviewWindowClearGpuExportPreview(
    DirectVulkanPreviewWindow* window);
bool directVulkanPreviewWindowIsVisible(DirectVulkanPreviewWindow* window);
QString directVulkanPreviewWindowCursorShape(DirectVulkanPreviewWindow* window);
