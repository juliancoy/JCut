#include "preview_surface_factory.h"

#include "null_preview_surface.h"
#include "opengl_preview.h"
#include "preview_surface.h"
#include "render_backend.h"
#include "vulkan_preview_surface.h"

#include <QDebug>
#include <QCoreApplication>
#include <QMessageBox>
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

bool userApprovesOpenGlFallback(QWidget* parent, const QString& reason)
{
    if (qEnvironmentVariable("QT_QPA_PLATFORM").contains("offscreen", Qt::CaseInsensitive)) {
        return true;
    }
    const QString prompt = QStringLiteral(
        "Vulkan preview is unavailable:\n\n%1\n\nAllow fallback to OpenGL preview?")
                               .arg(reason.trimmed().isEmpty()
                                        ? QStringLiteral("Unknown Vulkan initialization failure.")
                                        : reason.trimmed());
    return QMessageBox::question(parent,
                                 QStringLiteral("Confirm OpenGL Fallback"),
                                 prompt,
                                 QMessageBox::Yes | QMessageBox::No,
                                 QMessageBox::No) == QMessageBox::Yes;
}

PreviewSurface* exitAfterFallbackRefusal(const PreviewBackendDecision& decision,
                                         PreviewBackendDecision* decisionOut)
{
    logDecision(decision);
    if (decisionOut) {
        *decisionOut = decision;
    }
    QCoreApplication::exit(2);
    return nullptr;
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
        logDecision(decision);
        if (decisionOut) {
            *decisionOut = decision;
        }
        auto* placeholder = new NullPreviewSurface(parent);
        placeholder->setRenderBackendPreference(decision.effective);
        return placeholder;
    }

    const bool preferVulkan = (configured == RenderBackend::Vulkan || configured == RenderBackend::Auto);
    if (preferVulkan) {
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
            decision.effective = QStringLiteral("opengl");
            decision.fallbackApplied = true;
            const QString nativeReason = surface->nativeFailureReason().trimmed();
            decision.reason = nativeReason.isEmpty()
                ? QStringLiteral("Vulkan direct preview surface unavailable in explicit direct mode.")
                : nativeReason;
            const bool allowFallback = userApprovesOpenGlFallback(parent, decision.reason);
            delete surface;
            if (!allowFallback) {
                decision.effective = QStringLiteral("vulkan");
                decision.fallbackApplied = false;
                decision.reason = QStringLiteral("User declined OpenGL fallback after Vulkan preview failure.");
                return exitAfterFallbackRefusal(decision, decisionOut);
            }
            logDecision(decision);
            if (decisionOut) {
                *decisionOut = decision;
            }
            auto* fallback = new PreviewWindow(parent);
            fallback->setRenderBackendPreference(QStringLiteral("opengl"));
            return fallback;
        }

        const QString noDirectReason =
            QStringLiteral("Direct Vulkan presenter is not enabled; embedded mode uses OpenGL.");
        if (!userApprovesOpenGlFallback(parent, noDirectReason)) {
            decision.effective = QStringLiteral("vulkan");
            decision.fallbackApplied = false;
            decision.reason = QStringLiteral("User declined OpenGL fallback in embedded preview mode.");
            return exitAfterFallbackRefusal(decision, decisionOut);
        }
        auto* embedded = new PreviewWindow(parent);
        embedded->setRenderBackendPreference(QStringLiteral("opengl"));
        decision.effective = QStringLiteral("opengl");
        decision.fallbackApplied = true;
        decision.reason = noDirectReason;
        logDecision(decision);
        if (decisionOut) {
            *decisionOut = decision;
        }
        return embedded;
    }

    if (configured == RenderBackend::OpenGL || configured == RenderBackend::Null) {
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
