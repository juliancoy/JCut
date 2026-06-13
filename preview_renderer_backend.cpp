#include "preview_renderer_backend.h"

#include "gpu_compositor.h"
#include "render_backend.h"
#include "vulkan_backend.h"

#include <QDebug>
#include <QElapsedTimer>

#include <QtGui/private/qrhi_p.h>

using namespace editor;

PreviewRenderer::PreviewRenderer() = default;
PreviewRenderer::~PreviewRenderer() { release(); }

bool PreviewRenderer::initialize() {
    if (m_initialized) return true;

    QElapsedTimer initTimer;
    initTimer.start();

    const RenderBackend desiredBackend = desiredRenderBackendFromEnvironment();
    qDebug().noquote() << "[render-backend] requested=" << renderBackendName(desiredBackend);

    qDebug() << "[vulkan] attempting QRhi Vulkan initialization";
    VulkanBackendResult vk = createVulkanBackendRhi();
    if (vk.rhi) {
        m_rhi = std::move(vk.rhi);
        m_backendName = QString::fromLatin1(m_rhi->backendName());
        qDebug() << "[vulkan] initialized backend:" << m_backendName;
    } else {
        qCritical().noquote()
            << QStringLiteral("[render-backend-error] vulkan_init_failed reason=\"%1\"")
                   .arg(vk.status);
        return false;
    }
    if (!m_rhi) {
        qCritical() << "Failed to initialize Vulkan RHI backend";
        return false;
    } else {
        m_backendName = QString::fromLatin1(m_rhi->backendName());
        qDebug() << "PreviewRenderer: Using backend:" << m_backendName;
    }

    qDebug() << "[STARTUP] Initializing GPUCompositor...";
    QElapsedTimer compositorTimer;
    compositorTimer.start();
    m_compositor = std::make_unique<GPUCompositor>(m_rhi.get());
    if (!m_compositor->initialize()) {
        qWarning() << "Failed to initialize GPU compositor";
    }
    qDebug() << "[STARTUP] GPUCompositor initialized in" << compositorTimer.elapsed() << "ms";

    m_initialized = true;
    qDebug() << "[STARTUP] PreviewRenderer::initialize() total:" << initTimer.elapsed() << "ms";
    return true;
}

void PreviewRenderer::release() {
    m_compositor.reset();
    m_rhi.reset();
    m_initialized = false;
}

QRhi* PreviewRenderer::rhi() const { return m_rhi.get(); }
GPUCompositor* PreviewRenderer::compositor() const { return m_compositor.get(); }
QString PreviewRenderer::backendName() const { return m_backendName; }
bool PreviewRenderer::isInitialized() const { return m_initialized; }
