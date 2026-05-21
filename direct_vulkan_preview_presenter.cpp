#include "direct_vulkan_preview_presenter.h"
#include "direct_vulkan_preview_backend.h"
#include "direct_vulkan_preview_audio.h"
#include "preview_view_transform.h"

#include <QByteArray>
#include <QDebug>
#include <QFrame>
#include <QHBoxLayout>
#include <QLabel>
#include <QJsonArray>
#include <QStackedLayout>
#include <QTransform>
#include <QVBoxLayout>
#include <QWidget>
#include <QVersionNumber>

#include <vulkan/vulkan.h>

#include <algorithm>

DirectVulkanPreviewPresenter::DirectVulkanPreviewPresenter(PreviewInteractionState* state, QWidget* parent)
    : m_state(state)
{
    m_instance = std::make_unique<QVulkanInstance>();
    m_instance->setApiVersion(QVersionNumber(1, 1));
    // Use Qt's platform-provided WSI extension list. On X11 this must include
    // VK_KHR_surface plus the matching xcb/xlib surface extension before
    // QVulkanWindow can create its embedded native surface.
    const QVulkanInfoVector<QVulkanExtension> supportedExtensions = m_instance->supportedExtensions();
    auto extensionSupported = [&supportedExtensions](const QByteArray& name) {
        for (const QVulkanExtension& extension : supportedExtensions) {
            if (extension.name == name) {
                return true;
            }
        }
        return false;
    };
    QByteArrayList requestedExtensions;
    auto addExtension = [&requestedExtensions, &extensionSupported](const QByteArray& name) {
        if (extensionSupported(name) && !requestedExtensions.contains(name)) {
            requestedExtensions.append(name);
        }
    };
    addExtension(QByteArrayLiteral("VK_KHR_surface"));
    addExtension(QByteArrayLiteral("VK_KHR_xcb_surface"));
    addExtension(QByteArrayLiteral("VK_KHR_xlib_surface"));
    addExtension(QByteArrayLiteral("VK_KHR_wayland_surface"));
    m_instance->setExtensions(requestedExtensions);
    qDebug().noquote() << QStringLiteral("[vulkan-preview] requested instance extensions=%1")
                              .arg(QString::fromLatin1(requestedExtensions.join(',')));
    if (!m_instance->create()) {
        m_failureReason = QStringLiteral("QVulkanInstance::create() failed for direct Vulkan preview presenter: VkResult %1.")
                              .arg(static_cast<int>(m_instance->errorCode()));
        return;
    }

    m_placeholder.reset(createDirectVulkanPreviewHostWidget(
        m_state,
        [this]() { requestUpdate(); },
        parent));
    m_placeholder->setFocusPolicy(Qt::StrongFocus);
    m_placeholder->setMinimumSize(160, 120);
    m_placeholder->setStyleSheet(QStringLiteral("background:#05080d; border:0;"));
    m_placeholder->setAttribute(Qt::WA_NativeWindow, true);
    m_placeholder->winId();
    m_stack = new QStackedLayout(m_placeholder.get());
    m_stack->setContentsMargins(0, 0, 0, 0);

    m_window = createDirectVulkanPreviewWindow(
        m_state,
        &m_presentedFrames,
        &m_lastPresentedSourceFrame,
        &m_stats,
        &m_active,
        &m_failureReason,
        [this](const QString& reason) { showFailure(reason); });
    directVulkanPreviewWindowSetVulkanInstance(m_window, m_instance.get());
    const QVulkanInfoVector<QVulkanExtension> supportedDeviceExtensions =
        directVulkanPreviewWindowSupportedDeviceExtensions(m_window);
    QByteArrayList requestedDeviceExtensions;
    auto addDeviceExtension = [&requestedDeviceExtensions, &supportedDeviceExtensions](const QByteArray& name) {
        for (const QVulkanExtension& extension : supportedDeviceExtensions) {
            if (extension.name == name && !requestedDeviceExtensions.contains(name)) {
                requestedDeviceExtensions.append(name);
                return;
            }
        }
    };
    addDeviceExtension(QByteArrayLiteral(VK_KHR_EXTERNAL_MEMORY_EXTENSION_NAME));
    addDeviceExtension(QByteArrayLiteral(VK_KHR_EXTERNAL_MEMORY_FD_EXTENSION_NAME));
    addDeviceExtension(QByteArrayLiteral(VK_KHR_EXTERNAL_SEMAPHORE_EXTENSION_NAME));
    addDeviceExtension(QByteArrayLiteral(VK_KHR_EXTERNAL_SEMAPHORE_FD_EXTENSION_NAME));
    addDeviceExtension(QByteArrayLiteral(VK_KHR_BIND_MEMORY_2_EXTENSION_NAME));
    if (!requestedDeviceExtensions.isEmpty()) {
        directVulkanPreviewWindowSetDeviceExtensions(m_window, requestedDeviceExtensions);
        qDebug().noquote() << QStringLiteral("[vulkan-preview] requested device extensions=%1")
                                  .arg(QString::fromLatin1(requestedDeviceExtensions.join(',')));
    }
    directVulkanPreviewWindowResize(m_window, QSize(960, 540));

    m_windowContainer = createDirectVulkanPreviewWindowContainer(m_window, m_placeholder.get());
    if (!m_windowContainer) {
        m_failureReason = QStringLiteral("Failed to embed direct Vulkan preview window container.");
        showFailure(m_failureReason);
        return;
    }
    m_windowContainer->setFocusPolicy(Qt::StrongFocus);
    m_windowContainer->setMouseTracking(true);
    m_windowContainer->setMinimumSize(160, 120);
    m_windowContainer->setToolTip(QStringLiteral("Direct Vulkan preview presenter (%1).")
                                      .arg(directVulkanPreviewVisiblePathLabel()));

    m_statusLabel = new QLabel(m_placeholder.get());
    m_statusLabel->setAlignment(Qt::AlignLeft | Qt::AlignVCenter);
    m_statusLabel->setWordWrap(false);
    m_statusLabel->setAttribute(Qt::WA_TransparentForMouseEvents, true);
    m_statusLabel->setStyleSheet(QStringLiteral(
        "background:#1a2430; color:#dbe7f1; border:0; padding:2px 8px;"
        "font:600 11px 'DejaVu Sans Mono';"));
    m_statusLabel->setText(QStringLiteral("Vulkan preview initializing"));
    m_statusLabel->setVisible(directVulkanPreviewDebugChromeEnabled());
    m_statusLabel->raise();

    m_audioInfoPanel = new QWidget(m_placeholder.get());
    m_audioInfoPanel->setAttribute(Qt::WA_TransparentForMouseEvents, true);
    m_audioInfoPanel->setStyleSheet(QStringLiteral(
        "background:rgba(9,18,24,170); border:1px solid rgba(120,145,168,120); border-radius:10px;"));
    auto* audioInfoLayout = new QVBoxLayout(m_audioInfoPanel);
    audioInfoLayout->setContentsMargins(16, 14, 16, 14);
    audioInfoLayout->setSpacing(6);
    m_audioTitleLabel = new QLabel(m_audioInfoPanel);
    m_audioTitleLabel->setStyleSheet(QStringLiteral("color:#eef5fb; font-weight:700; font-size:17px;"));
    m_audioSummaryLabel = new QLabel(m_audioInfoPanel);
    m_audioSummaryLabel->setWordWrap(true);
    m_audioSummaryLabel->setStyleSheet(QStringLiteral("color:#c8d5e0; font-size:12px;"));
    m_audioFooterLabel = new QLabel(m_audioInfoPanel);
    m_audioFooterLabel->setWordWrap(true);
    m_audioFooterLabel->setStyleSheet(QStringLiteral("color:#9fb3c8; font-size:11px;"));
    audioInfoLayout->addWidget(m_audioTitleLabel);
    audioInfoLayout->addWidget(m_audioSummaryLabel);
    audioInfoLayout->addWidget(m_audioFooterLabel);
    m_audioInfoPanel->hide();

    m_audioHoverCard = new QFrame(m_placeholder.get());
    m_audioHoverCard->setAttribute(Qt::WA_TransparentForMouseEvents, true);
    m_audioHoverCard->setStyleSheet(QStringLiteral(
        "QFrame { background:qlineargradient(x1:0,y1:0,x2:0,y2:1, stop:0 rgba(12,19,29,238), stop:1 rgba(7,12,19,235));"
        "border:1px solid rgba(94,127,160,170); border-radius:10px; }"));
    auto* hoverRoot = new QHBoxLayout(m_audioHoverCard);
    hoverRoot->setContentsMargins(10, 10, 10, 10);
    hoverRoot->setSpacing(12);
    m_audioHoverAvatarLabel = new QLabel(m_audioHoverCard);
    m_audioHoverAvatarLabel->setFixedSize(72, 72);
    m_audioHoverAvatarLabel->setScaledContents(true);
    auto* hoverTextLayout = new QVBoxLayout;
    hoverTextLayout->setContentsMargins(0, 0, 0, 0);
    hoverTextLayout->setSpacing(4);
    m_audioHoverNameLabel = new QLabel(m_audioHoverCard);
    m_audioHoverNameLabel->setWordWrap(true);
    m_audioHoverNameLabel->setStyleSheet(QStringLiteral("color:#e8f6ff; font-weight:700; font-size:14px;"));
    m_audioHoverOrgLabel = new QLabel(m_audioHoverCard);
    m_audioHoverOrgLabel->setWordWrap(true);
    m_audioHoverOrgLabel->setStyleSheet(QStringLiteral("color:rgb(151,192,224); font-size:12px;"));
    m_audioHoverMetaLabel = new QLabel(m_audioHoverCard);
    m_audioHoverMetaLabel->setWordWrap(true);
    m_audioHoverMetaLabel->setStyleSheet(QStringLiteral("color:rgb(132,164,191); font-size:11px;"));
    m_audioHoverDescLabel = new QLabel(m_audioHoverCard);
    m_audioHoverDescLabel->setWordWrap(true);
    m_audioHoverDescLabel->setStyleSheet(QStringLiteral("color:#dff5ff; font-size:12px;"));
    hoverTextLayout->addWidget(m_audioHoverNameLabel);
    hoverTextLayout->addWidget(m_audioHoverOrgLabel);
    hoverTextLayout->addWidget(m_audioHoverMetaLabel);
    hoverTextLayout->addWidget(m_audioHoverDescLabel);
    hoverTextLayout->addStretch(1);
    hoverRoot->addWidget(m_audioHoverAvatarLabel, 0, Qt::AlignTop);
    hoverRoot->addLayout(hoverTextLayout, 1);
    m_audioHoverCard->hide();

    m_errorLabel = new QLabel(m_placeholder.get());
    m_errorLabel->setAlignment(Qt::AlignCenter);
    m_errorLabel->setWordWrap(true);
    m_errorLabel->setStyleSheet(QStringLiteral(
        "background:#070b10; color:#f3d5d5; border:1px solid #6f2b2b;"
        "font-weight:600; padding:18px;"));
    m_errorLabel->setText(QStringLiteral("Vulkan preview is initializing..."));

    m_stack->addWidget(m_windowContainer);
    m_stack->addWidget(m_errorLabel);
    m_stack->setCurrentWidget(m_windowContainer);
    m_active = true;
    updateTitle();
    updateDiagnosticChrome();
}

void DirectVulkanPreviewPresenter::setInteractionCallbacks(
    std::function<void(const QString&)> selectionRequested,
    std::function<void(const QString&, qreal, qreal, bool)> resizeRequested,
    std::function<void(const QString&, qreal, qreal, bool)> moveRequested,
    std::function<void(int64_t)> playbackSampleRequested,
    std::function<void(const QString&, qreal, qreal)> correctionPointRequested,
    std::function<void(const QString&, qreal, qreal)> speakerPointRequested,
    std::function<void(const QString&, qreal, qreal, qreal)> speakerBoxRequested,
    std::function<void(const QString&, int, const QString&, int64_t, qreal, qreal, qreal)> faceStreamBoxRequested,
    std::function<void(const QString&)> createKeyframeRequested)
{
    if (!m_window) {
        return;
    }
    directVulkanPreviewWindowSetInteractionCallbacks(
        m_window,
        std::move(selectionRequested),
        std::move(resizeRequested),
        std::move(moveRequested),
        std::move(playbackSampleRequested),
        std::move(correctionPointRequested),
        std::move(speakerPointRequested),
        std::move(speakerBoxRequested),
        std::move(faceStreamBoxRequested),
        std::move(createKeyframeRequested));
}

DirectVulkanPreviewPresenter::~DirectVulkanPreviewPresenter()
{
    if (m_windowContainer) {
        m_windowContainer->hide();
    }
    if (m_window) {
        directVulkanPreviewWindowHide(m_window);
    }
    m_window = nullptr;
}

QWidget* DirectVulkanPreviewPresenter::widget() const
{
    return m_placeholder.get();
}

bool DirectVulkanPreviewPresenter::isActive() const
{
    return m_active && directVulkanPreviewWindowIsValid(m_window);
}

bool DirectVulkanPreviewPresenter::hasFailed() const
{
    return !m_failureReason.trimmed().isEmpty();
}

bool DirectVulkanPreviewPresenter::updatePending() const
{
    return directVulkanPreviewWindowUpdatePending(m_window);
}

int64_t DirectVulkanPreviewPresenter::presentedFrames() const
{
    return m_presentedFrames;
}

int64_t DirectVulkanPreviewPresenter::lastPresentedSourceFrame() const
{
    return m_lastPresentedSourceFrame;
}

QString DirectVulkanPreviewPresenter::failureReason() const
{
    return m_failureReason;
}

QString DirectVulkanPreviewPresenter::backendName() const
{
    return m_active
        ? QStringLiteral("Vulkan Preview (Direct Swapchain Presenter)")
        : QStringLiteral("Vulkan Preview Unavailable");
}

void DirectVulkanPreviewPresenter::showFailure(const QString& reason)
{
    m_active = false;
    m_failureReason = reason.trimmed().isEmpty()
        ? QStringLiteral("Vulkan preview surface or swapchain initialization failed.")
        : reason.trimmed();
    if (m_errorLabel) {
        m_errorLabel->setText(QStringLiteral("Vulkan preview unavailable\n\n%1").arg(m_failureReason));
    }
    if (m_stack && m_errorLabel) {
        m_stack->setCurrentWidget(m_errorLabel);
    }
    updateDiagnosticChrome();
}

void DirectVulkanPreviewPresenter::requestUpdate()
{
    updateDiagnosticChrome();
    if (m_stack) {
        if (!m_failureReason.trimmed().isEmpty() && m_errorLabel) {
            m_stack->setCurrentWidget(m_errorLabel);
        } else if (m_windowContainer) {
            m_stack->setCurrentWidget(m_windowContainer);
        }
    }
    if (directVulkanPreviewDirectSwapchainVisible() && m_windowContainer) {
        m_windowContainer->raise();
    }
    if (m_window) {
        if (directVulkanPreviewDirectSwapchainVisible()) {
            directVulkanPreviewWindowRaise(m_window);
        }
        directVulkanPreviewWindowSchedulePreviewUpdate(m_window);
    }
    updateAudioOverlay();
    if (m_audioInfoPanel && m_audioInfoPanel->isVisible()) {
        m_audioInfoPanel->raise();
    }
    if (m_audioHoverCard && m_audioHoverCard->isVisible()) {
        m_audioHoverCard->raise();
    }
    if (m_statusLabel && m_statusLabel->isVisible()) {
        m_statusLabel->raise();
    }
}

void DirectVulkanPreviewPresenter::updateTitle()
{
    if (!m_window || !m_state) {
        return;
    }
    const QString modeLabel =
        m_state->viewMode == PreviewSurface::ViewMode::Audio
            ? QStringLiteral("audio")
            : QStringLiteral("video");
    directVulkanPreviewWindowSetTitle(
        m_window,
        QStringLiteral("JCut Direct Vulkan Preview - %1 - frame %2 clips %3")
            .arg(modeLabel)
            .arg(m_state->currentFrame)
            .arg(m_state->clipCount));
    updateDiagnosticChrome();
    updateAudioOverlay();
}

void DirectVulkanPreviewPresenter::updateDiagnosticChrome()
{
    if (!m_placeholder) {
        return;
    }

    const bool waitingForDecode = m_active &&
        m_failureReason.trimmed().isEmpty() &&
        m_stats.textureDraws <= 0 &&
        m_stats.explicitFailureDraws <= 0 &&
        m_state &&
        !m_state->vulkanFrameStatuses.isEmpty();
    const bool showOverlayLabel = !m_failureReason.trimmed().isEmpty() || waitingForDecode;
    if (!directVulkanPreviewDebugChromeEnabled() && !showOverlayLabel) {
        m_placeholder->setStyleSheet(QStringLiteral("background:#05080d; border:0;"));
        if (m_statusLabel) {
            m_statusLabel->hide();
        }
        return;
    }

    QString color = QStringLiteral("#7f8b96");
    QString state = QStringLiteral("initializing");
    if (m_state && m_state->viewMode == PreviewSurface::ViewMode::Audio) {
        color = QStringLiteral("#4a90e2");
        state = QStringLiteral("audio-vulkan");
    } else if (!m_active || !m_failureReason.trimmed().isEmpty()) {
        color = QStringLiteral("#d9483b");
        state = QStringLiteral("failed");
    } else if (m_stats.textureDraws > 0) {
        color = QStringLiteral("#33b86b");
        state = m_stats.lastHandoffMode.isEmpty() ? QStringLiteral("texture") : m_stats.lastHandoffMode;
    } else if (m_stats.explicitFailureDraws > 0) {
        color = QStringLiteral("#d9483b");
        state = QStringLiteral("no-frame");
    } else if (m_state && !m_state->vulkanFrameStatuses.isEmpty()) {
        color = QStringLiteral("#d8a52b");
        state = QStringLiteral("waiting-decode");
    }

    m_placeholder->setStyleSheet(QStringLiteral("background:#111821; border:3px solid %1;").arg(color));

    if (!m_statusLabel) {
        return;
    }
    if (!showOverlayLabel) {
        m_statusLabel->hide();
        return;
    }
    m_statusLabel->show();
    const int labelWidth = std::max(240, m_placeholder->width() - 24);
    m_statusLabel->setGeometry(12, 12, labelWidth, 28);
    m_statusLabel->raise();

    int ready = 0;
    int exact = 0;
    int cpu = 0;
    int hardwareFrame = 0;
    int gpuTexture = 0;
    int missing = 0;
    int64_t requestedFrame = -1;
    int64_t presentedFrame = -1;
    if (m_state) {
        for (const VulkanPreviewClipFrameStatus& status : m_state->vulkanFrameStatuses) {
            if (!status.active) {
                continue;
            }
            ready += status.hasFrame ? 1 : 0;
            exact += status.exact ? 1 : 0;
            cpu += status.cpuImage ? 1 : 0;
            hardwareFrame += status.hardwareFrame ? 1 : 0;
            gpuTexture += status.gpuTexture ? 1 : 0;
            missing += status.hasFrame ? 0 : 1;
            if (requestedFrame < 0) {
                requestedFrame = status.requestedSourceFrame;
                presentedFrame = status.presentedSourceFrame;
            }
        }
    }

    const QString error = m_stats.lastHandoffError.trimmed();
    if (waitingForDecode) {
        m_statusLabel->setText(QStringLiteral("⌛ Loading media..."));
    } else {
        m_statusLabel->setText(
            error.isEmpty()
                ? QStringLiteral("Vulkan preview failure: %1").arg(state)
                : QStringLiteral("Vulkan preview failure: %1 | %2").arg(state, error));
    }
}

void DirectVulkanPreviewPresenter::updateAudioOverlay()
{
    if (!m_placeholder || !m_state || !m_audioInfoPanel || !m_audioHoverCard) {
        return;
    }
    const bool audioMode = m_state->viewMode == PreviewSurface::ViewMode::Audio;
    updateDirectVulkanAudioOverlay(
        m_state,
        DirectVulkanAudioOverlayWidgets{
            m_placeholder.get(),
            m_audioInfoPanel,
            m_audioTitleLabel,
            m_audioSummaryLabel,
            m_audioFooterLabel,
            m_audioHoverCard,
            m_audioHoverAvatarLabel,
            m_audioHoverNameLabel,
            m_audioHoverOrgLabel,
            m_audioHoverMetaLabel,
            m_audioHoverDescLabel});
}

QJsonObject DirectVulkanPreviewPresenter::profilingSnapshot() const
{
    auto rectToJson = [](const QRectF& rect) {
        return QJsonObject{
            {QStringLiteral("x"), rect.x()},
            {QStringLiteral("y"), rect.y()},
            {QStringLiteral("width"), rect.width()},
            {QStringLiteral("height"), rect.height()}
        };
    };
    auto findSourceSize = [this](const QString& clipId) {
        if (!m_state) {
            return QSize();
        }
        for (const TimelineClip& clip : m_state->clips) {
            if (clip.id == clipId) {
                return clip.sourceFrameSize;
            }
        }
        return QSize();
    };
    auto transformWithTransientOverride = [this](const QString& clipId,
                                                 const TimelineClip::TransformKeyframe& fallback) {
        if (m_state &&
            m_state->transient.transformOverrideActive &&
            m_state->transient.transformOverrideClipId == clipId) {
            return m_state->transient.transformOverride;
        }
        return fallback;
    };

    struct OverlayGeometry {
        QTransform clipToScreen;
        QRectF localRect;
    };

    const qreal devicePixelRatio = m_windowContainer
        ? qMax<qreal>(0.0001, m_windowContainer->devicePixelRatioF())
        : 1.0;
    const QRectF deviceSurfaceRect = m_windowContainer
        ? PreviewViewTransform::rectForWidget(m_windowContainer, PreviewSurfaceCoordinateSpace::DeviceSurface)
        : QRectF();
    const QRectF logicalSurfaceRect = m_windowContainer
        ? PreviewViewTransform::rectForWidget(m_windowContainer, PreviewSurfaceCoordinateSpace::LogicalWidget)
        : QRectF();
    QHash<QString, OverlayGeometry> activeClipGeometry;
    if (m_state && !deviceSurfaceRect.isEmpty()) {
        const PreviewViewTransform viewTransform(
            deviceSurfaceRect,
            m_state->outputSize,
            36.0,
            m_state->previewZoom,
            m_state->previewPanOffset);
        const QPointF previewScale = viewTransform.outputScale();
        for (const VulkanPreviewClipFrameStatus& status : m_state->vulkanFrameStatuses) {
            if (!status.active || status.drawSuppressed) {
                continue;
            }
            const QRectF fitted =
                viewTransform.fittedClipRect(findSourceSize(status.clipId), status.frameSize);
            const TimelineClip::TransformKeyframe transform =
                transformWithTransientOverride(status.clipId, status.transform);
            PreviewClipGeometry geometry = PreviewViewTransform::clipGeometry(
                fitted,
                previewScale,
                QPointF(transform.translationX, transform.translationY),
                transform.rotation,
                QPointF(transform.scaleX, transform.scaleY));
            if (status.sampledFrameNeedsYFlip) {
                geometry.clipToScreen.scale(1.0, -1.0);
                geometry.bounds = geometry.clipToScreen.mapRect(geometry.localRect);
            }
            activeClipGeometry.insert(status.clipId, OverlayGeometry{geometry.clipToScreen, geometry.localRect});
        }
    }

    auto overlayToJson = [&](const VulkanPreviewFacestreamOverlay& overlay) {
        QJsonObject object{
            {QStringLiteral("clip_id"), overlay.clipId},
            {QStringLiteral("stream_id"), overlay.streamId},
            {QStringLiteral("source"), overlay.source},
            {QStringLiteral("track_id"), overlay.trackId},
            {QStringLiteral("source_frame"), static_cast<double>(overlay.sourceFrame)},
            {QStringLiteral("confidence"), overlay.confidence},
            {QStringLiteral("box_norm"), rectToJson(overlay.boxNorm)}
        };
        const auto geometryIt = activeClipGeometry.constFind(overlay.clipId);
        if (geometryIt != activeClipGeometry.constEnd() && overlay.boxNorm.isValid()) {
            const QRectF localBox =
                PreviewViewTransform::localRectForNormalizedRect(overlay.boxNorm, geometryIt->localRect);
            const QRectF deviceBox = geometryIt->clipToScreen.mapRect(localBox);
            const QRectF logicalBox(deviceBox.x() / devicePixelRatio,
                                    deviceBox.y() / devicePixelRatio,
                                    deviceBox.width() / devicePixelRatio,
                                    deviceBox.height() / devicePixelRatio);
            object.insert(QStringLiteral("box_device"), rectToJson(deviceBox));
            object.insert(QStringLiteral("box_logical"), rectToJson(logicalBox));
        }
        return object;
    };

    int activeStatuses = 0;
    int readyStatuses = 0;
    int exactStatuses = 0;
    int hardwareStatuses = 0;
    int cpuStatuses = 0;
    bool hasTimelineFrameGeometry = false;
    bool correctionsSupported = false;
    bool correctionsApplied = false;
    bool curveLutApplied = false;
    QJsonArray statusDetails;
    QJsonArray facestreamOverlays;
    QJsonArray rawDetectionOverlays;
    const bool texturePipelineReady = m_active && m_window != nullptr;
    const bool windowValid = directVulkanPreviewWindowIsValid(m_window);
    if (m_state) {
        for (const VulkanPreviewClipFrameStatus& status : m_state->vulkanFrameStatuses) {
            if (!status.active) {
                continue;
            }
            ++activeStatuses;
            readyStatuses += status.hasFrame ? 1 : 0;
            exactStatuses += status.exact ? 1 : 0;
            hardwareStatuses += (status.hardwareFrame || status.gpuTexture) ? 1 : 0;
            cpuStatuses += status.cpuImage ? 1 : 0;
            hasTimelineFrameGeometry = hasTimelineFrameGeometry || status.frameSize.isValid();
            correctionsSupported = correctionsSupported || status.correctionsSupported;
            correctionsApplied = correctionsApplied || status.correctionsApplied;
            curveLutApplied = curveLutApplied || status.curveLutApplied;
            statusDetails.append(QJsonObject{
                {QStringLiteral("clip_id"), status.clipId},
                {QStringLiteral("label"), status.label},
                {QStringLiteral("decode_path"), status.decodePath},
                {QStringLiteral("requested_source_frame"), static_cast<double>(status.requestedSourceFrame)},
                {QStringLiteral("presented_source_frame"), static_cast<double>(status.presentedSourceFrame)},
                {QStringLiteral("active"), status.active},
                {QStringLiteral("has_frame"), status.hasFrame},
                {QStringLiteral("exact"), status.exact},
                {QStringLiteral("exact_frame_available"), status.exactFrameAvailable},
                {QStringLiteral("selected_frame_available"), status.selectedFrameAvailable},
                {QStringLiteral("hardware_frame"), status.hardwareFrame},
                {QStringLiteral("gpu_texture"), status.gpuTexture},
                {QStringLiteral("cpu_image"), status.cpuImage},
                {QStringLiteral("draw_suppressed"), status.drawSuppressed},
                {QStringLiteral("effects_path"), status.effectsPath},
                {QStringLiteral("grading_shader_active"), status.gradingShaderActive},
                {QStringLiteral("grading_bypassed"), status.gradingBypassed},
                {QStringLiteral("grading_brightness"), status.grading.brightness},
                {QStringLiteral("grading_contrast"), status.grading.contrast},
                {QStringLiteral("grading_saturation"), status.grading.saturation},
                {QStringLiteral("grading_opacity"), status.grading.opacity},
                {QStringLiteral("grading_shadows"), QStringLiteral("%1,%2,%3")
                     .arg(status.grading.shadowsR).arg(status.grading.shadowsG).arg(status.grading.shadowsB)},
                {QStringLiteral("grading_midtones"), QStringLiteral("%1,%2,%3")
                     .arg(status.grading.midtonesR).arg(status.grading.midtonesG).arg(status.grading.midtonesB)},
                {QStringLiteral("grading_highlights"), QStringLiteral("%1,%2,%3")
                     .arg(status.grading.highlightsR).arg(status.grading.highlightsG).arg(status.grading.highlightsB)},
                {QStringLiteral("transform_translation"), QStringLiteral("%1,%2")
                     .arg(status.transform.translationX).arg(status.transform.translationY)},
                {QStringLiteral("transform_rotation"), status.transform.rotation},
                {QStringLiteral("transform_scale"), QStringLiteral("%1,%2")
                     .arg(status.transform.scaleX).arg(status.transform.scaleY)},
                {QStringLiteral("correction_polygon_count"), status.correctionPolygonCount},
                {QStringLiteral("vulkan_correction_masks_supported"), status.correctionsSupported},
                {QStringLiteral("vulkan_correction_masks_applied"), status.correctionsApplied},
                {QStringLiteral("vulkan_curve_lut_supported"), status.curveLutSupported},
                {QStringLiteral("vulkan_curve_lut_applied"), status.curveLutApplied},
                {QStringLiteral("frame_size"), status.frameSize.isValid()
                     ? QStringLiteral("%1x%2").arg(status.frameSize.width()).arg(status.frameSize.height())
                     : QString()},
                {QStringLiteral("missing_reason"), status.missingReason}
            });
        }

        for (const VulkanPreviewFacestreamOverlay& overlay : m_state->facestreamOverlays) {
            facestreamOverlays.append(overlayToJson(overlay));
        }
        for (const VulkanPreviewFacestreamOverlay& overlay : m_state->rawDetectionOverlays) {
            rawDetectionOverlays.append(overlayToJson(overlay));
        }
    }
    const bool cpuUploadPath = false;
    return QJsonObject{
        {QStringLiteral("backend"), QStringLiteral("vulkan")},
        {QStringLiteral("presenter"), QStringLiteral("qvulkanwindow_direct_swapchain")},
        {QStringLiteral("composition_path"), QStringLiteral("direct_swapchain_frame_status_composition")},
        {QStringLiteral("visible_path"), directVulkanPreviewVisiblePathLabel()},
        {QStringLiteral("preview_cursor"), directVulkanPreviewWindowCursorShape(m_window)},
        {QStringLiteral("optimal_present_requested"), directVulkanPreviewOptimalPresentEnabled()},
        {QStringLiteral("readback_mirror_enabled"), false},
        {QStringLiteral("swapchain_readback_enabled"), false},
        {QStringLiteral("swapchain_present"), m_active && m_window != nullptr},
        {QStringLiteral("qvulkanwindow_valid"), windowValid},
        {QStringLiteral("native_window_visible"), directVulkanPreviewWindowIsVisible(m_window)},
        {QStringLiteral("native_active"), m_active},
        {QStringLiteral("qimage_bridge"), false},
        {QStringLiteral("qimage_materialized"), cpuUploadPath},
        {QStringLiteral("vulkan_path_uses_qimage"), cpuUploadPath},
        {QStringLiteral("vulkan_cpu_upload_path"), cpuUploadPath},
        {QStringLiteral("materialized_frame_path"), cpuUploadPath},
        {QStringLiteral("current_frame"), m_state ? static_cast<double>(m_state->currentFrame) : 0.0},
        {QStringLiteral("clip_count"), m_state ? m_state->clipCount : 0},
        {QStringLiteral("device_pixel_ratio"), devicePixelRatio},
        {QStringLiteral("preview_surface_rect_device"), rectToJson(deviceSurfaceRect)},
        {QStringLiteral("preview_surface_rect_logical"), rectToJson(logicalSurfaceRect)},
        {QStringLiteral("preview_zoom"), m_state ? m_state->previewZoom : 1.0},
        {QStringLiteral("preview_pan_x"), m_state ? m_state->previewPanOffset.x() : 0.0},
        {QStringLiteral("preview_pan_y"), m_state ? m_state->previewPanOffset.y() : 0.0},
        {QStringLiteral("face_stream_assignment_interaction_enabled"),
         m_state ? m_state->faceStreamAssignmentInteractionEnabled : false},
        {QStringLiteral("hovered_facestream_track_id"),
         m_state ? m_state->transient.hoveredFaceStreamTrackId : -1},
        {QStringLiteral("direct_preview_frame_image"), false},
        {QStringLiteral("direct_preview_frame_size"), QString()},
        {QStringLiteral("pipeline_thumbnail_readback_pending"), false},
        {QStringLiteral("pipeline_thumbnail_readback_requests"), static_cast<double>(m_stats.diagnosticReadbackRequests)},
        {QStringLiteral("pipeline_thumbnail_readback_copies"), static_cast<double>(m_stats.diagnosticReadbackCopies)},
        {QStringLiteral("pipeline_thumbnail_readback_size"), m_stats.lastDiagnosticReadbackSize.isValid()
             ? QStringLiteral("%1x%2").arg(m_stats.lastDiagnosticReadbackSize.width()).arg(m_stats.lastDiagnosticReadbackSize.height())
             : QString()},
        {QStringLiteral("pipeline_thumbnail_readback_format"), m_stats.lastDiagnosticReadbackFormat},
        {QStringLiteral("decoder_diagnostic_readback_image"), false},
        {QStringLiteral("decoder_diagnostic_readback_copies"), static_cast<double>(m_stats.decoderDiagnosticReadbackCopies)},
        {QStringLiteral("decoder_diagnostic_readback_size"), m_stats.lastDecoderDiagnosticReadbackSize.isValid()
             ? QStringLiteral("%1x%2")
                   .arg(m_stats.lastDecoderDiagnosticReadbackSize.width())
                   .arg(m_stats.lastDecoderDiagnosticReadbackSize.height())
             : QString()},
        {QStringLiteral("decoder_diagnostic_readback_format"), m_stats.lastDecoderDiagnosticReadbackFormat},
        {QStringLiteral("active_decode_status_clips"), activeStatuses},
        {QStringLiteral("ready_decode_status_clips"), readyStatuses},
        {QStringLiteral("exact_decode_status_clips"), exactStatuses},
        {QStringLiteral("hardware_decode_status_clips"), hardwareStatuses},
        {QStringLiteral("cpu_decode_status_clips"), cpuStatuses},
        {QStringLiteral("decode_status_details"), statusDetails},
        {QStringLiteral("facestream_overlay_boxes"), m_state ? m_state->facestreamOverlays.size() : 0},
        {QStringLiteral("facestream_overlays"), facestreamOverlays},
        {QStringLiteral("raw_detection_overlay_boxes"), m_state ? m_state->rawDetectionOverlays.size() : 0},
        {QStringLiteral("raw_detection_overlays"), rawDetectionOverlays},
        {QStringLiteral("timeline_texture_composition"), hasTimelineFrameGeometry && readyStatuses > 0},
        {QStringLiteral("timeline_texture_draw_pipeline"), texturePipelineReady},
        {QStringLiteral("vulkan_matches_opengl_grading_scalars"), true},
        {QStringLiteral("vulkan_matches_opengl_transform_geometry"), true},
        {QStringLiteral("vulkan_curve_lut_supported"), true},
        {QStringLiteral("vulkan_curve_lut_applied"), curveLutApplied},
        {QStringLiteral("vulkan_correction_masks_supported"), true},
        {QStringLiteral("vulkan_correction_masks_cpu_upload_supported"), true},
        {QStringLiteral("vulkan_correction_masks_applied"), correctionsApplied},
        {QStringLiteral("last_curve_lut_applied"), m_stats.lastCurveLutApplied},
        {QStringLiteral("last_effects_path"), m_stats.lastEffectsPath},
        {QStringLiteral("last_unsupported_effect"), m_stats.lastUnsupportedEffect},
        {QStringLiteral("last_applied_brightness"), m_stats.lastAppliedBrightness},
        {QStringLiteral("last_applied_contrast"), m_stats.lastAppliedContrast},
        {QStringLiteral("last_applied_saturation"), m_stats.lastAppliedSaturation},
        {QStringLiteral("last_applied_opacity"), m_stats.lastAppliedOpacity},
        {QStringLiteral("last_applied_rotation"), m_stats.lastAppliedRotation},
        {QStringLiteral("last_applied_scale_x"), m_stats.lastAppliedScaleX},
        {QStringLiteral("last_applied_scale_y"), m_stats.lastAppliedScaleY},
        {QStringLiteral("last_target_rect"), m_stats.lastTargetRect.isValid()
             ? QStringLiteral("%1,%2 %3x%4")
                   .arg(m_stats.lastTargetRect.x())
                   .arg(m_stats.lastTargetRect.y())
                   .arg(m_stats.lastTargetRect.width())
                   .arg(m_stats.lastTargetRect.height())
             : QString()},
        {QStringLiteral("last_fitted_rect"), m_stats.lastFittedRect.isValid()
             ? QStringLiteral("%1,%2 %3x%4")
                   .arg(m_stats.lastFittedRect.x())
                   .arg(m_stats.lastFittedRect.y())
                   .arg(m_stats.lastFittedRect.width())
                   .arg(m_stats.lastFittedRect.height())
             : QString()},
        {QStringLiteral("presented_frames"), static_cast<double>(m_presentedFrames)},
        {QStringLiteral("handoff_attempts"), static_cast<double>(m_stats.handoffAttempts)},
        {QStringLiteral("handoff_successes"), static_cast<double>(m_stats.handoffSuccesses)},
        {QStringLiteral("handoff_failures"), static_cast<double>(m_stats.handoffFailures)},
        {QStringLiteral("sampled_image_ready_count"), static_cast<double>(m_stats.sampledImageReady)},
        {QStringLiteral("texture_draw_count"), static_cast<double>(m_stats.textureDraws)},
        {QStringLiteral("checker_draw_count"), static_cast<double>(m_stats.checkerDraws)},
        {QStringLiteral("clear_fallback_draw_count"), static_cast<double>(m_stats.clearFallbackDraws)},
        {QStringLiteral("explicit_failure_draw_count"), static_cast<double>(m_stats.explicitFailureDraws)},
        {QStringLiteral("implicit_fallback_permitted"), false},
        {QStringLiteral("active_clip_draw_count"), static_cast<double>(m_stats.activeClipDraws)},
        {QStringLiteral("last_handoff_upload_ms"), m_stats.lastUploadMs},
        {QStringLiteral("last_handoff_mode"), m_stats.lastHandoffMode},
        {QStringLiteral("last_handoff_error"), m_stats.lastHandoffError},
        {QStringLiteral("last_handoff_probe_path"), m_stats.lastProbePath},
        {QStringLiteral("last_handoff_probe_reason"), m_stats.lastProbeReason},
        {QStringLiteral("last_hardware_sw_format"), m_stats.lastHardwareSwFormat},
        {QStringLiteral("last_vulkan_image_format"), m_stats.lastVulkanImageFormat},
        {QStringLiteral("last_yuv_rgb_matrix"), m_stats.lastHardwareSwFormat == QStringLiteral("nv12")
             ? QStringLiteral("bt709_limited")
             : QString()},
        {QStringLiteral("last_external_image_size"), m_stats.lastExternalImageSize.isValid()
             ? QStringLiteral("%1x%2").arg(m_stats.lastExternalImageSize.width()).arg(m_stats.lastExternalImageSize.height())
             : QString()},
        {QStringLiteral("failure_reason"), m_failureReason}
    };
}

void DirectVulkanPreviewPresenter::resetProfilingStats()
{
    m_presentedFrames = 0;
    m_stats = DirectVulkanPreviewStats{};
}
