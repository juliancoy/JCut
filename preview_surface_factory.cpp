#include "preview_surface_factory.h"

#include "opengl_preview.h"
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
} // namespace

PreviewSurface* createPreviewSurfaceForConfiguredBackend(QWidget* parent,
                                                         PreviewBackendDecision* decisionOut)
{
    const RenderBackend configured = desiredPreviewBackendFromEnvironment();
    PreviewBackendDecision decision;
    decision.requested = renderBackendName(configured);

    if (configured == RenderBackend::Vulkan) {
        auto* surface = new VulkanPreviewSurface(parent);
        if (surface->isNativeActive()) {
            decision.effective = QStringLiteral("vulkan");
            decision.reason = QStringLiteral("Native Vulkan preview surface initialized.");
            logDecision(decision);
            if (decisionOut) {
                *decisionOut = decision;
            }
            return surface;
        } else {
            decision.effective = QStringLiteral("vulkan");
            decision.fallbackApplied = true;
            const QString nativeReason = surface->nativeFailureReason().trimmed();
            decision.reason = nativeReason.isEmpty()
                ? QStringLiteral("Native Vulkan preview surface unavailable.")
                : nativeReason;
            logDecision(decision);
            if (decisionOut) {
                *decisionOut = decision;
            }
            delete surface;
            return new PreviewWindow(parent);
        }
    }

    if (configured == RenderBackend::OpenGL || configured == RenderBackend::Auto || configured == RenderBackend::Null) {
        decision.effective = QStringLiteral("opengl");
        decision.reason = QStringLiteral("Selected OpenGL preview path.");
        if (configured == RenderBackend::Null) {
            decision.fallbackApplied = true;
            decision.reason = QStringLiteral("Null preview backend is unsupported; falling back to OpenGL preview.");
        }
        logDecision(decision);
        if (decisionOut) {
            *decisionOut = decision;
        }
        return new PreviewWindow(parent);
    }

    decision.effective = QStringLiteral("opengl");
    decision.fallbackApplied = true;
    decision.reason = QStringLiteral("Unknown backend request; falling back to OpenGL preview.");
    logDecision(decision);
    if (decisionOut) {
        *decisionOut = decision;
    }
    return new PreviewWindow(parent);
}

PreviewSurface* createPreviewSurfaceForConfiguredBackend(QWidget* parent)
{
    return createPreviewSurfaceForConfiguredBackend(parent, nullptr);
}
