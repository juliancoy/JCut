#include "preview_surface_factory.h"

#include "null_preview_surface.h"
#include "preview_surface.h"
#include "render_backend.h"
#include "vulkan_preview_surface.h"

#include <QDebug>
#include <QtGlobal>

namespace {
void logDecision(const PreviewBackendDecision& decision)
{
    if (decision.fallbackApplied) {
        qWarning().noquote()
            << QStringLiteral("[render-backend-fallback] requested=%1 effective=%2 reason=\"%3\"")
                   .arg(decision.requested, decision.effective, decision.reason);
        return;
    }
    qDebug().noquote()
        << QStringLiteral("[render-backend-selection] requested=%1 effective=%2 reason=\"%3\"")
               .arg(decision.requested, decision.effective, decision.reason);
}

PreviewSurface* createPlaceholderPreview(QWidget* parent,
                                         const PreviewBackendDecision& decision,
                                         PreviewBackendDecision* decisionOut)
{
    logDecision(decision);
    if (decisionOut) {
        *decisionOut = decision;
    }
    auto* placeholder = new NullPreviewSurface(parent);
    placeholder->setRenderBackendPreference(decision.effective);
    return placeholder;
}
} // namespace

PreviewSurface* createPreviewSurfaceForConfiguredBackend(QWidget* parent,
                                                         PreviewBackendDecision* decisionOut)
{
    const RenderBackend configured = desiredPreviewBackendFromEnvironment();
    PreviewBackendDecision decision;
    decision.requested = renderBackendName(configured);

    const bool offscreenPlatform =
        qEnvironmentVariable("QT_QPA_PLATFORM").contains(QStringLiteral("offscreen"),
                                                         Qt::CaseInsensitive);
    if (offscreenPlatform) {
        decision.effective = QStringLiteral("offscreen-placeholder");
        decision.fallbackApplied = true;
        decision.reason = QStringLiteral(
            "Offscreen Qt platform does not safely support the interactive preview widget stack; "
            "using a non-GL placeholder preview surface.");
        return createPlaceholderPreview(parent, decision, decisionOut);
    }

    if (configured == RenderBackend::Null) {
        decision.effective = QStringLiteral("null");
        decision.reason = QStringLiteral("Null preview backend selected.");
        return createPlaceholderPreview(parent, decision, decisionOut);
    }

    if (configured == RenderBackend::Vulkan || configured == RenderBackend::Auto) {
        const QString presenterMode =
            qEnvironmentVariable("JCUT_VULKAN_PREVIEW_PRESENTER", QStringLiteral("direct"))
                .trimmed()
                .toLower();
        const bool directVulkanPreviewEnabled =
            (presenterMode == QStringLiteral("direct") ||
             presenterMode == QStringLiteral("native") ||
             presenterMode == QStringLiteral("swapchain") ||
             qEnvironmentVariableIntValue("JCUT_DIRECT_VULKAN_PREVIEW") == 1);
        if (directVulkanPreviewEnabled) {
            auto* surface = new VulkanPreviewSurface(parent);
            if (surface->asWidget() && surface->isNativePresentationActive()) {
                decision.effective = QStringLiteral("vulkan");
                decision.fallbackApplied = false;
                decision.reason = QStringLiteral("Using direct Vulkan swapchain preview presenter (explicit mode).");
                logDecision(decision);
                if (decisionOut) {
                    *decisionOut = decision;
                }
                return surface;
            }
            decision.effective = QStringLiteral("vulkan-unavailable");
            decision.fallbackApplied = true;
            const QString nativeReason = surface->nativeFailureReason().trimmed();
            decision.reason = nativeReason.isEmpty()
                ? QStringLiteral("Vulkan direct preview surface unavailable in explicit direct mode.")
                : nativeReason;
            delete surface;
            return createPlaceholderPreview(parent, decision, decisionOut);
        }

        decision.effective = QStringLiteral("vulkan-unavailable");
        decision.fallbackApplied = true;
        decision.reason = QStringLiteral("Direct Vulkan presenter is not enabled.");
        return createPlaceholderPreview(parent, decision, decisionOut);
    }

    decision.effective = QStringLiteral("vulkan-unavailable");
    decision.fallbackApplied = true;
    decision.reason = QStringLiteral("Unknown backend request; only Vulkan and placeholder preview paths are available.");
    return createPlaceholderPreview(parent, decision, decisionOut);
}

PreviewSurface* createPreviewSurfaceForConfiguredBackend(QWidget* parent)
{
    return createPreviewSurfaceForConfiguredBackend(parent, nullptr);
}
