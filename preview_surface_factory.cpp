#include "preview_surface_factory.h"

#include "null_preview_surface.h"
#include "preview_surface.h"
#include "render_backend.h"
#include "vulkan_preview_surface.h"

#include <QGuiApplication>
#include <QDebug>
#include <QtGlobal>

namespace {
void logDecision(const PreviewBackendDecision& decision)
{
    qDebug().noquote()
        << QStringLiteral("[render-backend-selection] requested=%1 effective=%2 reason=\"%3\"")
               .arg(decision.requested, decision.effective, decision.reason);
}
} // namespace

PreviewSurface* createPreviewSurfaceForConfiguredBackend(QWidget* parent,
                                                         PreviewBackendDecision* decisionOut)
{
    const RenderBackend configured = desiredPreviewBackendFromEnvironment();
    PreviewBackendDecision decision;
    decision.requested = renderBackendName(configured);
    const bool offscreenUiAutomation =
        qEnvironmentVariableIntValue("JCUT_UI_AUTOMATION") > 0 &&
        QGuiApplication::platformName().compare(QStringLiteral("offscreen"),
                                                Qt::CaseInsensitive) == 0;
    if (offscreenUiAutomation) {
        auto* surface = new NullPreviewSurface(parent);
        surface->setRenderBackendPreference(QStringLiteral("offscreen-placeholder"));
        decision.effective = QStringLiteral("offscreen-placeholder");
        decision.reason =
            QStringLiteral("Using null preview surface for offscreen UI automation.");
        logDecision(decision);
        if (decisionOut) {
            *decisionOut = decision;
        }
        return surface;
    }

    auto* surface = new VulkanPreviewSurface(parent);
    if (surface->asWidget() && surface->isNativePresentationActive()) {
        decision.effective = QStringLiteral("vulkan");
        decision.reason = QStringLiteral("Using direct Vulkan swapchain preview presenter.");
        logDecision(decision);
        if (decisionOut) {
            *decisionOut = decision;
        }
        return surface;
    }

    decision.effective = QStringLiteral("vulkan-unavailable");
    const QString nativeReason = surface->nativeFailureReason().trimmed();
    decision.reason = nativeReason.isEmpty()
        ? QStringLiteral("Vulkan direct preview surface unavailable.")
        : nativeReason;
    qCritical().noquote()
        << QStringLiteral("[render-backend-error] requested=%1 effective=%2 reason=\"%3\"")
               .arg(decision.requested, decision.effective, decision.reason);
    if (decisionOut) {
        *decisionOut = decision;
    }
    delete surface;
    return nullptr;
}

PreviewSurface* createPreviewSurfaceForConfiguredBackend(QWidget* parent)
{
    return createPreviewSurfaceForConfiguredBackend(parent, nullptr);
}
