#include "preview_renderer_backend.h"

#include "gpu_compositor.h"

#include <QDebug>
#include <QElapsedTimer>
#include <QOffscreenSurface>
#include <QSurfaceFormat>

#include <QtGui/private/qrhi_p.h>
#include <QtGui/private/qrhigles2_p.h>

using namespace editor;

PreviewRenderer::PreviewRenderer() = default;
PreviewRenderer::~PreviewRenderer() { release(); }

bool PreviewRenderer::initialize() {
    if (m_initialized) return true;

    QElapsedTimer initTimer;
    initTimer.start();

    if (qEnvironmentVariableIntValue("EDITOR_FORCE_NULL_RHI") == 1) {
        qDebug() << "[STARTUP] Forcing QRhi Null backend";
        QRhiInitParams nullParams;
        m_rhi.reset(QRhi::create(QRhi::Null, &nullParams, QRhi::Flags()));
        if (!m_rhi) {
            qWarning() << "Failed to initialize forced Null QRhi backend";
            return false;
        }
        m_backendName = QString::fromLatin1(m_rhi->backendName()) + QStringLiteral(" (forced)");

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

    m_fallbackSurface = std::make_unique<QOffscreenSurface>();
    m_fallbackSurface->setFormat(QSurfaceFormat::defaultFormat());
    m_fallbackSurface->create();
    qDebug() << "[STARTUP] Offscreen surface created in" << initTimer.elapsed() << "ms";

    if (!m_fallbackSurface->isValid()) {
        qWarning() << "Failed to create fallback surface";
        return false;
    }

    qDebug() << "[STARTUP] Creating QRhi OpenGL context...";
    QElapsedTimer rhiTimer;
    rhiTimer.start();
    QRhiGles2InitParams params;
    params.format = QSurfaceFormat::defaultFormat();
    params.fallbackSurface = m_fallbackSurface.get();

    m_rhi.reset(QRhi::create(QRhi::OpenGLES2, &params, QRhi::Flags()));
    qDebug() << "[STARTUP] QRhi::create(OpenGLES2) took" << rhiTimer.elapsed() << "ms";

    if (m_rhi) {
        m_backendName = QString::fromLatin1(m_rhi->backendName());
        qDebug() << "PreviewRenderer: Using backend:" << m_backendName;
    } else {
        QRhiInitParams nullParams;
        m_rhi.reset(QRhi::create(QRhi::Null, &nullParams, QRhi::Flags()));

        if (m_rhi) {
            m_backendName = QString::fromLatin1(m_rhi->backendName()) + QStringLiteral(" (fallback)");
        } else {
            qWarning() << "Failed to initialize any RHI backend";
            return false;
        }
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
    m_fallbackSurface.reset();
    m_initialized = false;
}

QRhi* PreviewRenderer::rhi() const { return m_rhi.get(); }
GPUCompositor* PreviewRenderer::compositor() const { return m_compositor.get(); }
QString PreviewRenderer::backendName() const { return m_backendName; }
bool PreviewRenderer::isInitialized() const { return m_initialized; }
