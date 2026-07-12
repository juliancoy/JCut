#include "direct_vulkan_preview_presenter.h"
#include "direct_vulkan_preview_backend.h"
#include "direct_vulkan_preview_config.h"
#include "direct_vulkan_preview_geometry.h"
#include "direct_vulkan_preview_audio.h"
#include "preview_speaker_profiles.h"
#include "preview_view_transform.h"

#include <QByteArray>
#include <QDebug>
#include <QFrame>
#include <QHBoxLayout>
#include <QLabel>
#include <QJsonArray>
#include <QPaintEvent>
#include <QPainter>
#include <QPen>
#include <QStackedLayout>
#include <QStringList>
#include <QTransform>
#include <QVBoxLayout>
#include <QWidget>
#include <QVersionNumber>

#include <vulkan/vulkan.h>

#include <algorithm>
#include <cmath>

namespace {

constexpr bool kAllowQtPainterOverlayInDirectVulkanPreview = false;

struct OverlayGeometry {
    QTransform clipToScreen;
    QRectF localRect;
};

QSize sourceSizeForClipId(const PreviewInteractionState* state, const QString& clipId)
{
    if (!state) {
        return QSize();
    }
    for (const TimelineClip& clip : state->clips) {
        if (clip.id == clipId) {
            return clip.sourceFrameSize;
        }
    }
    return QSize();
}

TimelineClip::TransformKeyframe transientAwareTransform(
    const PreviewInteractionState* state,
    const QString& clipId,
    const TimelineClip::TransformKeyframe& fallback)
{
    if (state &&
        state->transient.transformOverrideActive &&
        state->transient.transformOverrideClipId == clipId) {
        return state->transient.transformOverride;
    }
    return fallback;
}

QHash<QString, OverlayGeometry> activeClipGeometryById(const PreviewInteractionState* state,
                                                       QWidget* widget)
{
    QHash<QString, OverlayGeometry> geometries;
    if (!state || !widget) {
        return geometries;
    }

    const QRectF logicalSurfaceRect =
        PreviewViewTransform::rectForWidget(widget, PreviewSurfaceCoordinateSpace::LogicalWidget);
    if (logicalSurfaceRect.isEmpty()) {
        return geometries;
    }

    const PreviewViewTransform viewTransform(
        logicalSurfaceRect,
        state->outputSize,
        jcut::direct_vulkan_preview::vulkanPreviewCanvasMarginPx(),
        state->previewZoom,
        state->previewPanOffset);
    const QPointF previewScale = viewTransform.outputScale();
    for (const VulkanPreviewClipFrameStatus& status : state->vulkanFrameStatuses) {
        if (!status.active || status.drawSuppressed) {
            continue;
        }
        const QRectF fitted =
            viewTransform.fittedClipRect(sourceSizeForClipId(state, status.clipId), status.frameSize);
        const TimelineClip::TransformKeyframe transform =
            transientAwareTransform(state, status.clipId, status.transform);
        PreviewClipGeometry geometry = PreviewViewTransform::clipGeometry(
            fitted,
            previewScale,
            QPointF(transform.translationX, transform.translationY),
            transform.rotation,
            QPointF(transform.scaleX, transform.scaleY));
        if (status.sampledFrameNeedsYFlip) {
            geometry.clipToScreen.scale(1.0, -1.0);
        }
        geometries.insert(status.clipId, OverlayGeometry{geometry.clipToScreen, geometry.localRect});
    }
    return geometries;
}

class DirectVulkanPreviewOverlayWidget final : public QWidget {
public:
    explicit DirectVulkanPreviewOverlayWidget(PreviewInteractionState* state, QWidget* parent = nullptr)
        : QWidget(parent)
        , m_state(state)
    {
        setAttribute(Qt::WA_TransparentForMouseEvents, true);
        setAttribute(Qt::WA_NoSystemBackground, true);
        setAttribute(Qt::WA_TranslucentBackground, true);
        setAutoFillBackground(false);
    }

protected:
    void paintEvent(QPaintEvent* event) override
    {
        Q_UNUSED(event)
        if (!kAllowQtPainterOverlayInDirectVulkanPreview) {
            return;
        }
        if (!m_state || m_state->viewMode == PreviewSurface::ViewMode::Audio) {
            return;
        }

        const QHash<QString, OverlayGeometry> geometries = activeClipGeometryById(m_state, this);
        if (geometries.isEmpty()) {
            return;
        }

        QPainter painter(this);
        painter.setRenderHint(QPainter::Antialiasing, true);
        drawContinuityOverlays(&painter, geometries);
        drawRawDetections(&painter, geometries);
        drawTargetBoxOverlay(&painter);
    }

private:
    static QRectF screenBoxForOverlay(const VulkanPreviewFacestreamOverlay& overlay,
                                      const OverlayGeometry& geometry,
                                      const QRect& viewportRect)
    {
        if (!overlay.boxNorm.isValid() || overlay.boxNorm.isEmpty()) {
            return QRectF();
        }
        const QRectF localBox =
            PreviewViewTransform::localRectForNormalizedRect(overlay.boxNorm, geometry.localRect);
        return geometry.clipToScreen.mapRect(localBox).normalized().intersected(QRectF(viewportRect));
    }

    void drawContinuityOverlays(QPainter* painter,
                                const QHash<QString, OverlayGeometry>& geometries) const
    {
        if (!painter || !m_state || m_state->facedetectionsOverlays.isEmpty()) {
            return;
        }

        for (const VulkanPreviewFacestreamOverlay& overlay : m_state->facedetectionsOverlays) {
            const auto geometryIt = geometries.constFind(overlay.clipId);
            if (geometryIt == geometries.constEnd()) {
                continue;
            }
            const QRectF screenBox = screenBoxForOverlay(overlay, geometryIt.value(), rect());
            if (screenBox.width() < 2.0 || screenBox.height() < 2.0) {
                continue;
            }

            QColor stroke(QStringLiteral("#a855f7"));
            QColor fill(168, 85, 247, 54);
            qreal width = 2.5;
            const bool assignedToSelectedSpeaker =
                overlay.trackId >= 0 &&
                m_state->selectedSpeakerAssignedFaceTrackIds.contains(overlay.trackId);
            const bool hovered =
                overlay.trackId >= 0 &&
                m_state->transient.hoveredFaceDetectionsTrackId == overlay.trackId &&
                m_state->transient.hoveredFaceDetectionsClipId == overlay.clipId &&
                m_state->transient.hoveredFaceDetectionsId == overlay.streamId;
            if (assignedToSelectedSpeaker) {
                stroke = QColor(QStringLiteral("#4ade80"));
                fill = QColor(74, 222, 128, 70);
                width = 3.0;
            }
            QRectF drawBox = screenBox;
            if (hovered) {
                drawBox = screenBox.adjusted(-7.0, -7.0, 7.0, 7.0);
                stroke = QColor(QStringLiteral("#f5d0fe"));
                fill = QColor(216, 180, 254, 92);
                width = 4.5;
            }

            painter->setPen(QPen(stroke, width, Qt::SolidLine, Qt::SquareCap, Qt::MiterJoin));
            painter->setBrush(fill);
            painter->drawRect(drawBox.adjusted(0.5, 0.5, -0.5, -0.5));
        }
    }

    void drawRawDetections(QPainter* painter,
                           const QHash<QString, OverlayGeometry>& geometries) const
    {
        if (!painter || !m_state || m_state->rawDetectionOverlays.isEmpty()) {
            return;
        }

        for (const VulkanPreviewFacestreamOverlay& overlay : m_state->rawDetectionOverlays) {
            const auto geometryIt = geometries.constFind(overlay.clipId);
            if (geometryIt == geometries.constEnd()) {
                continue;
            }
            const QRectF screenBox = screenBoxForOverlay(overlay, geometryIt.value(), rect());
            if (screenBox.width() < 2.0 || screenBox.height() < 2.0) {
                continue;
            }

            painter->setPen(QPen(QColor(QStringLiteral("#a855f7")), 2.0, Qt::SolidLine, Qt::SquareCap, Qt::MiterJoin));
            painter->setBrush(QColor(168, 85, 247, 44));
            painter->drawRect(screenBox.adjusted(0.5, 0.5, -0.5, -0.5));
        }
    }

    void drawTargetBoxOverlay(QPainter* painter) const
    {
        if (!painter || !m_state) {
            return;
        }
        const TimelineClip* selectedClip = jcut::direct_vulkan_preview::selectedClipForTargetBox(m_state);
        if (!selectedClip) {
            return;
        }

        const QRectF logicalSurfaceRect =
            PreviewViewTransform::rectForWidget(this, PreviewSurfaceCoordinateSpace::LogicalWidget);
        if (logicalSurfaceRect.isEmpty()) {
            return;
        }
        const PreviewViewTransform viewTransform(
            logicalSurfaceRect,
            m_state->outputSize,
            jcut::direct_vulkan_preview::vulkanPreviewCanvasMarginPx(),
            m_state->previewZoom,
            m_state->previewPanOffset);
        const QRectF compositeRect = viewTransform.targetRect();
        if (compositeRect.isEmpty()) {
            return;
        }

        const TimelineClip::TransformKeyframe targetState =
            evaluateClipSpeakerFramingTargetAtFrame(*selectedClip, m_state->currentFrame);
        const qreal targetXNorm = qBound<qreal>(0.0, targetState.translationX, 1.0);
        const qreal targetYNorm = qBound<qreal>(0.0, targetState.translationY, 1.0);
        const qreal targetBoxNorm = qBound<qreal>(-1.0, targetState.scaleX, 1.0);
        if (targetBoxNorm <= 0.0) {
            return;
        }

        const qreal centerX = compositeRect.left() + (targetXNorm * compositeRect.width());
        const qreal centerY = compositeRect.top() + (targetYNorm * compositeRect.height());
        const qreal targetSideScreenPx = qBound<qreal>(
            2.0,
            targetBoxNorm * qMax<qreal>(1.0, qMin<qreal>(compositeRect.width(), compositeRect.height())),
            4096.0);
        const qreal halfSide = targetSideScreenPx * 0.5;

        const QColor accentColor(255, 226, 74, 235);
        painter->setPen(QPen(accentColor, 2.0, Qt::DashLine));
        painter->setBrush(QColor(255, 226, 74, 28));
        painter->drawRect(QRectF(centerX - halfSide, centerY - halfSide, targetSideScreenPx, targetSideScreenPx));
        painter->setPen(QPen(accentColor, 1.8));
        painter->drawLine(QPointF(centerX - 9.0, centerY), QPointF(centerX + 9.0, centerY));
        painter->drawLine(QPointF(centerX, centerY - 9.0), QPointF(centerX, centerY + 9.0));
    }

    void drawCurrentSpeakerLabelOverlay(QPainter* painter) const
    {
        if (!painter || !m_state ||
            (!m_state->showCurrentSpeakerName && !m_state->showCurrentSpeakerOrganization)) {
            return;
        }
        const CurrentSpeakerLabel label = currentSpeakerLabelForState(m_state);
        struct SpeakerOverlayLine {
            QString text;
            bool name = false;
        };
        QVector<SpeakerOverlayLine> lines;
        if (m_state->showCurrentSpeakerName && !label.name.trimmed().isEmpty()) {
            lines.push_back(SpeakerOverlayLine{label.name.trimmed(), true});
        }
        if (m_state->showCurrentSpeakerOrganization && !label.organization.trimmed().isEmpty()) {
            lines.push_back(SpeakerOverlayLine{label.organization.trimmed(), false});
        }
        if (lines.isEmpty()) {
            return;
        }

        const QRect surfaceRect = rect();
        if (surfaceRect.isEmpty()) {
            return;
        }

        painter->save();
        QFont nameFont = painter->font();
        nameFont.setBold(true);
        const qreal nameScale = qBound<qreal>(0.25, m_state->currentSpeakerNameTextScale, 3.0);
        const qreal organizationScale = qBound<qreal>(0.25, m_state->currentSpeakerOrganizationTextScale, 3.0);
        nameFont.setPointSizeF(qBound<qreal>(6.0, (nameFont.pointSizeF() + 3.0) * nameScale, 72.0));
        QFont orgFont = painter->font();
        orgFont.setPointSizeF(qBound<qreal>(6.0, (orgFont.pointSizeF() + 1.0) * organizationScale, 64.0));

        const int maxTextWidth = qMax(180, static_cast<int>(surfaceRect.width() * 0.72));
        const int paddingX = 18;
        const int paddingY = 10;
        const auto drawBlock = [&](const QVector<SpeakerOverlayLine>& blockLines, qreal verticalPosition) {
            if (blockLines.isEmpty()) {
                return;
            }
            int contentHeight = 0;
            int contentWidth = 0;
            QVector<QRect> lineRects;
            lineRects.reserve(blockLines.size());
            for (int i = 0; i < blockLines.size(); ++i) {
                const SpeakerOverlayLine& line = blockLines.at(i);
                const QFontMetrics fm(line.name ? nameFont : orgFont);
                const QRect bounds = fm.boundingRect(QRect(0, 0, maxTextWidth, 120),
                                                     Qt::AlignCenter | Qt::TextWordWrap,
                                                     line.text);
                lineRects.push_back(bounds);
                contentHeight += bounds.height();
                if (i > 0) {
                    contentHeight += 4;
                }
                contentWidth = qMax(contentWidth, bounds.width());
            }

            const int cardWidth = qMin(maxTextWidth + (paddingX * 2), qMax(contentWidth + (paddingX * 2), 220));
            const int cardHeight = contentHeight + (paddingY * 2);
            const int centerY = static_cast<int>(std::round(qBound<qreal>(
                surfaceRect.top() + (cardHeight * 0.5),
                surfaceRect.top() + (surfaceRect.height() * qBound<qreal>(0.0, verticalPosition, 1.0)),
                surfaceRect.bottom() - (cardHeight * 0.5))));
            const QRect cardRect(surfaceRect.center().x() - cardWidth / 2,
                                 centerY - cardHeight / 2,
                                 cardWidth,
                                 cardHeight);
            const qreal radius = qBound<qreal>(0.0, m_state->currentSpeakerBackgroundCornerRadius, 128.0);
            const qreal borderWidth = qBound<qreal>(0.0, m_state->currentSpeakerBorderWidth, 16.0);
            painter->setPen(borderWidth > 0.0
                                ? QPen(m_state->currentSpeakerBorderColor, borderWidth)
                                : Qt::NoPen);
            painter->setBrush(m_state->currentSpeakerBackgroundColor);
            painter->drawRoundedRect(cardRect, radius, radius);

            int y = cardRect.top() + paddingY;
            for (int i = 0; i < blockLines.size(); ++i) {
                const SpeakerOverlayLine& line = blockLines.at(i);
                painter->setFont(line.name ? nameFont : orgFont);
                const QRect textRect(cardRect.left() + paddingX,
                                     y,
                                     cardRect.width() - (paddingX * 2),
                                     lineRects.at(i).height());
                if (m_state->currentSpeakerShadowEnabled &&
                    m_state->currentSpeakerShadowColor.alpha() > 0) {
                    painter->setPen(m_state->currentSpeakerShadowColor);
                    painter->drawText(textRect.translated(2, 2),
                                      Qt::AlignCenter | Qt::TextWordWrap,
                                      line.text);
                }
                painter->setPen(line.name
                                    ? m_state->currentSpeakerNameColor
                                    : m_state->currentSpeakerOrganizationColor);
                painter->drawText(textRect, Qt::AlignCenter | Qt::TextWordWrap, line.text);
                y += lineRects.at(i).height() + 4;
            }
        };
        QVector<SpeakerOverlayLine> nameLines;
        QVector<SpeakerOverlayLine> organizationLines;
        for (const SpeakerOverlayLine& line : lines) {
            (line.name ? nameLines : organizationLines).push_back(line);
        }
        drawBlock(nameLines, m_state->currentSpeakerNameVerticalPosition);
        drawBlock(organizationLines, m_state->currentSpeakerOrganizationVerticalPosition);
        painter->restore();
    }

    PreviewInteractionState* m_state = nullptr;
};

} // namespace

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
                                      .arg(jcut::direct_vulkan_preview::vulkanPreviewVisiblePathLabel()));

    if (kAllowQtPainterOverlayInDirectVulkanPreview) {
        m_overlayWidget = new DirectVulkanPreviewOverlayWidget(m_state, m_placeholder.get());
        m_overlayWidget->setGeometry(m_placeholder->rect());
        m_overlayWidget->show();
    }

    m_statusLabel = new QLabel(m_placeholder.get());
    m_statusLabel->setAlignment(Qt::AlignLeft | Qt::AlignVCenter);
    m_statusLabel->setWordWrap(false);
    m_statusLabel->setAttribute(Qt::WA_TransparentForMouseEvents, true);
    m_statusLabel->setStyleSheet(QStringLiteral(
        "background:#1a2430; color:#dbe7f1; border:0; padding:2px 8px;"
        "font:600 11px 'DejaVu Sans Mono';"));
    m_statusLabel->setText(QStringLiteral("Vulkan preview initializing"));
    m_statusLabel->setVisible(jcut::direct_vulkan_preview::vulkanPreviewDebugChromeEnabled());
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
    std::function<void(const QString&, int, const QString&, int64_t, qreal, qreal, qreal)> faceStreamBoxFocusClearRequested,
    std::function<void(const QString&)> faceStreamBoxClickStatus,
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
        std::move(faceStreamBoxFocusClearRequested),
        std::move(faceStreamBoxClickStatus),
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
    if (m_windowContainer) {
        m_windowContainer->raise();
    }
    if (m_window) {
        directVulkanPreviewWindowSchedulePreviewUpdate(m_window);
    }
    if (m_overlayWidget && kAllowQtPainterOverlayInDirectVulkanPreview) {
        m_overlayWidget->setGeometry(m_placeholder->rect());
        m_overlayWidget->raise();
        m_overlayWidget->update();
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

void DirectVulkanPreviewPresenter::requestPipelineTapReadback()
{
    if (m_window) {
        directVulkanPreviewWindowRequestPipelineThumbnailReadback(m_window);
    }
}

QImage DirectVulkanPreviewPresenter::latestPipelineTapImage() const
{
    return directVulkanPreviewWindowLatestPipelineThumbnailReadback(m_window);
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
    if (!jcut::direct_vulkan_preview::vulkanPreviewDebugChromeEnabled() && !showOverlayLabel) {
        const QString style = QStringLiteral("background:#05080d; border:0;");
        if (m_lastDiagnosticChromeStyle != style) {
            m_placeholder->setStyleSheet(style);
            m_lastDiagnosticChromeStyle = style;
        }
        if (m_statusLabel) {
            if (m_lastDiagnosticChromeLabelVisible) {
                m_statusLabel->hide();
                m_lastDiagnosticChromeLabelVisible = false;
            }
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

    const QString style =
        QStringLiteral("background:#111821; border:3px solid %1;").arg(color);
    if (m_lastDiagnosticChromeStyle != style) {
        m_placeholder->setStyleSheet(style);
        m_lastDiagnosticChromeStyle = style;
    }

    if (!m_statusLabel) {
        return;
    }
    if (!showOverlayLabel) {
        if (m_lastDiagnosticChromeLabelVisible) {
            m_statusLabel->hide();
            m_lastDiagnosticChromeLabelVisible = false;
        }
        return;
    }
    if (!m_lastDiagnosticChromeLabelVisible) {
        m_statusLabel->show();
        m_lastDiagnosticChromeLabelVisible = true;
    }
    const int labelWidth = std::max(240, m_placeholder->width() - 24);
    const QRect labelGeometry(12, 12, labelWidth, 28);
    if (m_lastDiagnosticChromeGeometry != labelGeometry) {
        m_statusLabel->setGeometry(labelGeometry);
        m_lastDiagnosticChromeGeometry = labelGeometry;
    }
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
    const QString labelText = waitingForDecode
        ? QStringLiteral("Loading media...")
        : (error.isEmpty()
               ? QStringLiteral("Vulkan preview failure: %1").arg(state)
               : QStringLiteral("Vulkan preview failure: %1 | %2").arg(state, error));
    if (m_lastDiagnosticChromeText != labelText) {
        m_statusLabel->setText(labelText);
        m_lastDiagnosticChromeText = labelText;
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
            jcut::direct_vulkan_preview::vulkanPreviewCanvasMarginPx(),
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
    QJsonArray facedetectionsOverlays;
    QJsonArray rawDetectionOverlays;
    QJsonArray selectedSpeakerAssignedFaceTrackIds;
    QJsonArray visibleFaceTrackIds;
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
                {QStringLiteral("media_owner_clip_id"), status.mediaOwnerClipId},
                {QStringLiteral("effects_owner_clip_id"), status.effectsOwnerClipId},
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
                {QStringLiteral("mask_texture_enabled"), status.maskTextureEnabled},
                {QStringLiteral("mask_foreground_layer_enabled"), status.maskForegroundLayerEnabled},
                {QStringLiteral("mask_clip_source"), status.maskClipSource},
                {QStringLiteral("mask_grade_enabled"), status.maskGradeEnabled},
                {QStringLiteral("mask_grade_contrast"), status.maskGradeContrast},
                {QStringLiteral("mask_grade_saturation"), status.maskGradeSaturation},
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

        for (const VulkanPreviewFacestreamOverlay& overlay : m_state->facedetectionsOverlays) {
            facedetectionsOverlays.append(overlayToJson(overlay));
            if (overlay.trackId >= 0 && !visibleFaceTrackIds.contains(overlay.trackId)) {
                visibleFaceTrackIds.append(overlay.trackId);
            }
        }
        for (const VulkanPreviewFacestreamOverlay& overlay : m_state->rawDetectionOverlays) {
            rawDetectionOverlays.append(overlayToJson(overlay));
        }
        QList<int> selectedTrackIds = m_state->selectedSpeakerAssignedFaceTrackIds.values();
        std::sort(selectedTrackIds.begin(), selectedTrackIds.end());
        for (int trackId : selectedTrackIds) {
            selectedSpeakerAssignedFaceTrackIds.append(trackId);
        }
    }
    const bool cpuUploadPath = false;
    const QJsonObject playbackStageMetrics{
        {QStringLiteral("text_prep"),
         editor::playbackStageMetricToJson(m_stats.textPrepStageMetric, QStringLiteral("presenter"))},
        {QStringLiteral("text_draw"),
         editor::playbackStageMetricToJson(m_stats.textDrawStageMetric, QStringLiteral("presenter"))},
        {QStringLiteral("gpu_handoff"),
         editor::playbackStageMetricToJson(m_stats.gpuHandoffStageMetric, QStringLiteral("presenter"))},
        {QStringLiteral("command_recording"),
         editor::playbackStageMetricToJson(m_stats.commandRecordingStageMetric, QStringLiteral("presenter"))},
        {QStringLiteral("presentation"),
         editor::playbackStageMetricToJson(m_stats.presentationStageMetric, QStringLiteral("presenter"))},
    };
    return QJsonObject{
        {QStringLiteral("backend"), QStringLiteral("vulkan")},
        {QStringLiteral("presenter"), QStringLiteral("qvulkanwindow_direct_swapchain")},
        {QStringLiteral("composition_path"), QStringLiteral("direct_swapchain_frame_status_composition")},
        {QStringLiteral("visible_path"), jcut::direct_vulkan_preview::vulkanPreviewVisiblePathLabel()},
        {QStringLiteral("preview_cursor"), directVulkanPreviewWindowCursorShape(m_window)},
        {QStringLiteral("optimal_present_requested"), jcut::direct_vulkan_preview::vulkanPreviewOptimalPresentEnabled()},
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
        {QStringLiteral("hovered_facedetections_track_id"),
         m_state ? m_state->transient.hoveredFaceDetectionsTrackId : -1},
        {QStringLiteral("direct_preview_frame_image"), false},
        {QStringLiteral("direct_preview_frame_size"), QString()},
        {QStringLiteral("pipeline_thumbnail_readback_pending"),
         directVulkanPreviewWindowPipelineThumbnailReadbackPending(m_window)},
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
        {QStringLiteral("facedetections_overlay_boxes"), m_state ? m_state->facedetectionsOverlays.size() : 0},
        {QStringLiteral("facedetections_overlays"), facedetectionsOverlays},
        {QStringLiteral("preview_assigned_face_track_ids"), selectedSpeakerAssignedFaceTrackIds},
        {QStringLiteral("selected_speaker_assigned_face_track_ids"), selectedSpeakerAssignedFaceTrackIds},
        {QStringLiteral("visible_face_track_ids"), visibleFaceTrackIds},
        {QStringLiteral("raw_detection_overlay_boxes"), m_state ? m_state->rawDetectionOverlays.size() : 0},
        {QStringLiteral("raw_detection_overlays"), rawDetectionOverlays},
        {QStringLiteral("timeline_texture_composition"), hasTimelineFrameGeometry && readyStatuses > 0},
        {QStringLiteral("timeline_texture_draw_pipeline"), texturePipelineReady},
        {QStringLiteral("vulkan_grading_scalars_supported"), true},
        {QStringLiteral("vulkan_transform_geometry_supported"), true},
        {QStringLiteral("vulkan_curve_lut_supported"), true},
        {QStringLiteral("vulkan_curve_lut_applied"), curveLutApplied},
        {QStringLiteral("vulkan_correction_masks_supported"), true},
        {QStringLiteral("vulkan_correction_masks_cpu_upload_supported"), false},
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
        {QStringLiteral("preview_update_requests"), static_cast<double>(m_stats.previewUpdateRequests)},
        {QStringLiteral("preview_updates_delivered"), static_cast<double>(m_stats.previewUpdatesDelivered)},
        {QStringLiteral("last_preview_update_latency_ms"), m_stats.lastPreviewUpdateLatencyMs},
        {QStringLiteral("max_preview_update_latency_ms"), m_stats.maxPreviewUpdateLatencyMs},
        {QStringLiteral("last_present_interval_ms"), m_stats.lastPresentIntervalMs},
        {QStringLiteral("max_present_interval_ms"), m_stats.maxPresentIntervalMs},
        {QStringLiteral("handoff_attempts"), static_cast<double>(m_stats.handoffAttempts)},
        {QStringLiteral("handoff_successes"), static_cast<double>(m_stats.handoffSuccesses)},
        {QStringLiteral("handoff_failures"), static_cast<double>(m_stats.handoffFailures)},
        {QStringLiteral("sampled_image_ready_count"), static_cast<double>(m_stats.sampledImageReady)},
        {QStringLiteral("sampled_image_descriptor_index"), m_stats.descriptorSetIndex},
        {QStringLiteral("sampled_image_descriptor_count"), m_stats.descriptorSetCount},
        {QStringLiteral("active_clip_handoff_resource_count"), m_stats.activeClipHandoffResourceCount},
        {QStringLiteral("retired_clip_handoff_resource_count"), m_stats.retiredClipHandoffResourceCount},
        {QStringLiteral("final_composite_stretch_prepared"), m_stats.finalCompositeStretchPrepared},
        {QStringLiteral("final_composite_stretch_drawn"), m_stats.finalCompositeStretchDrawn},
        {QStringLiteral("final_composite_stretch_source_clip_id"), m_stats.finalCompositeStretchSourceClipId},
        {QStringLiteral("final_composite_stretch_source_label"), m_stats.finalCompositeStretchSourceLabel},
        {QStringLiteral("final_composite_stretch_reason"), m_stats.finalCompositeStretchReason},
        {QStringLiteral("transcript_candidate_count"), m_stats.transcriptCandidateCount},
        {QStringLiteral("transcript_prepared_count"), m_stats.transcriptPreparedCount},
        {QStringLiteral("transcript_drawn_count"), m_stats.transcriptDrawnCount},
        {QStringLiteral("title_candidate_count"), m_stats.titleCandidateCount},
        {QStringLiteral("title_prepared_count"), m_stats.titlePreparedCount},
        {QStringLiteral("title_drawn_count"), m_stats.titleDrawnCount},
        {QStringLiteral("last_title_skip_reason"), m_stats.lastTitleSkipReason},
        {QStringLiteral("last_title_clip_id"), m_stats.lastTitleClipId},
        {QStringLiteral("last_transcript_skip_reason"), m_stats.lastTranscriptSkipReason},
        {QStringLiteral("last_transcript_clip_id"), m_stats.lastTranscriptClipId},
        {QStringLiteral("last_transcript_path"), m_stats.lastTranscriptPath},
        {QStringLiteral("last_transcript_timing_source"), m_stats.lastTranscriptTimingSource},
        {QStringLiteral("last_transcript_timeline_sample"), static_cast<qint64>(m_stats.lastTranscriptTimelineSample)},
        {QStringLiteral("last_transcript_frame"), static_cast<qint64>(m_stats.lastTranscriptFrame)},
        {QStringLiteral("last_transcript_presented_media_source_frame"),
         static_cast<qint64>(m_stats.lastTranscriptPresentedMediaSourceFrame)},
        {QStringLiteral("last_text_prep_failure_reason"), m_stats.lastTextPrepFailureReason},
        {QStringLiteral("last_text_draw_failure_reason"), m_stats.lastTextDrawFailureReason},
        {QStringLiteral("texture_draw_count"), static_cast<double>(m_stats.textureDraws)},
        {QStringLiteral("checker_draw_count"), static_cast<double>(m_stats.checkerDraws)},
        {QStringLiteral("clear_fallback_draw_count"), static_cast<double>(m_stats.clearFallbackDraws)},
        {QStringLiteral("fallback_draw_count"), static_cast<double>(m_stats.clearFallbackDraws)},
        {QStringLiteral("last_clear_fallback_reason"), m_stats.lastClearFallbackReason},
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
        {QStringLiteral("last_yuv_rgb_matrix"), m_stats.lastYuvRgbMatrix},
        {QStringLiteral("last_external_image_size"), m_stats.lastExternalImageSize.isValid()
             ? QStringLiteral("%1x%2").arg(m_stats.lastExternalImageSize.width()).arg(m_stats.lastExternalImageSize.height())
             : QString()},
        {QStringLiteral("failure_reason"), m_failureReason},
        {QStringLiteral("playback_pipeline_stages"), playbackStageMetrics}
    };
}

QJsonObject DirectVulkanPreviewPresenter::pipelineHealthSnapshot() const
{
    int activeStatuses = 0;
    int readyStatuses = 0;
    int exactStatuses = 0;
    int hardwareStatuses = 0;
    int cpuStatuses = 0;
    int64_t requestedSourceFrame = -1;
    int64_t presentedSourceFrame = -1;
    int64_t maxFrameLag = 0;
    QString decodePath;
    QString missingReason;
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
            requestedSourceFrame = status.requestedSourceFrame;
            presentedSourceFrame = status.presentedSourceFrame;
            decodePath = status.decodePath;
            missingReason = status.missingReason;
            if (status.hasFrame && status.requestedSourceFrame >= 0 && status.presentedSourceFrame >= 0) {
                maxFrameLag = qMax<int64_t>(
                    maxFrameLag,
                    qAbs(status.requestedSourceFrame - status.presentedSourceFrame));
            }
        }
    }
    const bool cpuUploadPath = false;
    const QJsonObject playbackStageMetrics{
        {QStringLiteral("text_prep"),
         editor::playbackStageMetricToJson(m_stats.textPrepStageMetric, QStringLiteral("presenter"))},
        {QStringLiteral("text_draw"),
         editor::playbackStageMetricToJson(m_stats.textDrawStageMetric, QStringLiteral("presenter"))},
        {QStringLiteral("gpu_handoff"),
         editor::playbackStageMetricToJson(m_stats.gpuHandoffStageMetric, QStringLiteral("presenter"))},
        {QStringLiteral("command_recording"),
         editor::playbackStageMetricToJson(m_stats.commandRecordingStageMetric, QStringLiteral("presenter"))},
        {QStringLiteral("presentation"),
         editor::playbackStageMetricToJson(m_stats.presentationStageMetric, QStringLiteral("presenter"))},
    };
    return QJsonObject{
        {QStringLiteral("backend"), QStringLiteral("vulkan")},
        {QStringLiteral("presenter"), QStringLiteral("qvulkanwindow_direct_swapchain")},
        {QStringLiteral("composition_path"), QStringLiteral("direct_swapchain_frame_status_composition")},
        {QStringLiteral("visible_path"), jcut::direct_vulkan_preview::vulkanPreviewVisiblePathLabel()},
        {QStringLiteral("swapchain_present"), m_active && m_window != nullptr},
        {QStringLiteral("qvulkanwindow_valid"), directVulkanPreviewWindowIsValid(m_window)},
        {QStringLiteral("native_window_visible"), directVulkanPreviewWindowIsVisible(m_window)},
        {QStringLiteral("native_active"), m_active},
        {QStringLiteral("qimage_bridge"), false},
        {QStringLiteral("qimage_materialized"), cpuUploadPath},
        {QStringLiteral("vulkan_path_uses_qimage"), cpuUploadPath},
        {QStringLiteral("vulkan_cpu_upload_path"), cpuUploadPath},
        {QStringLiteral("current_frame"), m_state ? static_cast<qint64>(m_state->currentFrame) : 0},
        {QStringLiteral("clip_count"), m_state ? m_state->clipCount : 0},
        {QStringLiteral("active_decode_status_clips"), activeStatuses},
        {QStringLiteral("ready_decode_status_clips"), readyStatuses},
        {QStringLiteral("exact_decode_status_clips"), exactStatuses},
        {QStringLiteral("hardware_decode_status_clips"), hardwareStatuses},
        {QStringLiteral("cpu_decode_status_clips"), cpuStatuses},
        {QStringLiteral("requested_source_frame"), static_cast<qint64>(requestedSourceFrame)},
        {QStringLiteral("presented_source_frame"), static_cast<qint64>(presentedSourceFrame)},
        {QStringLiteral("frame_lag"), static_cast<qint64>(maxFrameLag)},
        {QStringLiteral("decode_path"), decodePath},
        {QStringLiteral("missing_reason"), missingReason},
        {QStringLiteral("timeline_texture_draw_pipeline"), m_active && m_window != nullptr},
        {QStringLiteral("vulkan_curve_lut_applied"), m_stats.lastCurveLutApplied},
        {QStringLiteral("presented_frames"), static_cast<double>(m_presentedFrames)},
        {QStringLiteral("preview_update_requests"), static_cast<double>(m_stats.previewUpdateRequests)},
        {QStringLiteral("preview_updates_delivered"), static_cast<double>(m_stats.previewUpdatesDelivered)},
        {QStringLiteral("last_preview_update_latency_ms"), m_stats.lastPreviewUpdateLatencyMs},
        {QStringLiteral("max_preview_update_latency_ms"), m_stats.maxPreviewUpdateLatencyMs},
        {QStringLiteral("last_present_interval_ms"), m_stats.lastPresentIntervalMs},
        {QStringLiteral("max_present_interval_ms"), m_stats.maxPresentIntervalMs},
        {QStringLiteral("handoff_attempts"), static_cast<double>(m_stats.handoffAttempts)},
        {QStringLiteral("handoff_successes"), static_cast<double>(m_stats.handoffSuccesses)},
        {QStringLiteral("handoff_failures"), static_cast<double>(m_stats.handoffFailures)},
        {QStringLiteral("handoff_success_rate"),
         m_stats.handoffAttempts > 0
             ? static_cast<double>(m_stats.handoffSuccesses) / static_cast<double>(m_stats.handoffAttempts)
             : 1.0},
        {QStringLiteral("active_clip_handoff_resource_count"), m_stats.activeClipHandoffResourceCount},
        {QStringLiteral("retired_clip_handoff_resource_count"), m_stats.retiredClipHandoffResourceCount},
        {QStringLiteral("final_composite_stretch_prepared"), m_stats.finalCompositeStretchPrepared},
        {QStringLiteral("final_composite_stretch_drawn"), m_stats.finalCompositeStretchDrawn},
        {QStringLiteral("final_composite_stretch_source_clip_id"), m_stats.finalCompositeStretchSourceClipId},
        {QStringLiteral("final_composite_stretch_source_label"), m_stats.finalCompositeStretchSourceLabel},
        {QStringLiteral("final_composite_stretch_reason"), m_stats.finalCompositeStretchReason},
        {QStringLiteral("transcript_candidate_count"), m_stats.transcriptCandidateCount},
        {QStringLiteral("transcript_prepared_count"), m_stats.transcriptPreparedCount},
        {QStringLiteral("transcript_drawn_count"), m_stats.transcriptDrawnCount},
        {QStringLiteral("title_candidate_count"), m_stats.titleCandidateCount},
        {QStringLiteral("title_prepared_count"), m_stats.titlePreparedCount},
        {QStringLiteral("title_drawn_count"), m_stats.titleDrawnCount},
        {QStringLiteral("last_title_skip_reason"), m_stats.lastTitleSkipReason},
        {QStringLiteral("last_title_clip_id"), m_stats.lastTitleClipId},
        {QStringLiteral("last_transcript_skip_reason"), m_stats.lastTranscriptSkipReason},
        {QStringLiteral("last_transcript_clip_id"), m_stats.lastTranscriptClipId},
        {QStringLiteral("last_transcript_path"), m_stats.lastTranscriptPath},
        {QStringLiteral("last_transcript_timing_source"), m_stats.lastTranscriptTimingSource},
        {QStringLiteral("last_transcript_timeline_sample"), static_cast<qint64>(m_stats.lastTranscriptTimelineSample)},
        {QStringLiteral("last_transcript_frame"), static_cast<qint64>(m_stats.lastTranscriptFrame)},
        {QStringLiteral("last_transcript_presented_media_source_frame"),
         static_cast<qint64>(m_stats.lastTranscriptPresentedMediaSourceFrame)},
        {QStringLiteral("last_text_prep_failure_reason"), m_stats.lastTextPrepFailureReason},
        {QStringLiteral("last_text_draw_failure_reason"), m_stats.lastTextDrawFailureReason},
        {QStringLiteral("texture_draw_count"), static_cast<double>(m_stats.textureDraws)},
        {QStringLiteral("clear_fallback_draw_count"), static_cast<double>(m_stats.clearFallbackDraws)},
        {QStringLiteral("fallback_draw_count"), static_cast<double>(m_stats.clearFallbackDraws)},
        {QStringLiteral("last_clear_fallback_reason"), m_stats.lastClearFallbackReason},
        {QStringLiteral("explicit_failure_draw_count"), static_cast<double>(m_stats.explicitFailureDraws)},
        {QStringLiteral("implicit_fallback_permitted"), false},
        {QStringLiteral("last_handoff_upload_ms"), m_stats.lastUploadMs},
        {QStringLiteral("last_handoff_mode"), m_stats.lastHandoffMode},
        {QStringLiteral("last_handoff_error"), m_stats.lastHandoffError},
        {QStringLiteral("last_handoff_probe_path"), m_stats.lastProbePath},
        {QStringLiteral("last_hardware_sw_format"), m_stats.lastHardwareSwFormat},
        {QStringLiteral("last_vulkan_image_format"), m_stats.lastVulkanImageFormat},
        {QStringLiteral("last_yuv_rgb_matrix"), m_stats.lastYuvRgbMatrix},
        {QStringLiteral("failure_reason"), m_failureReason},
        {QStringLiteral("playback_pipeline_stages"), playbackStageMetrics}
    };
}

void DirectVulkanPreviewPresenter::resetProfilingStats()
{
    m_presentedFrames = 0;
    m_stats = DirectVulkanPreviewStats{};
}
