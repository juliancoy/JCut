#pragma once

#include <memory>
#include <QString>

class QOffscreenSurface;
class QRhi;

namespace editor {
class GPUCompositor;
}

class PreviewRenderer {
public:
    PreviewRenderer();
    ~PreviewRenderer();

    bool initialize();
    void release();

    QRhi* rhi() const;
    editor::GPUCompositor* compositor() const;
    QString backendName() const;
    bool isInitialized() const;

private:
    std::unique_ptr<QOffscreenSurface> m_fallbackSurface;
    std::unique_ptr<QRhi> m_rhi;
    std::unique_ptr<editor::GPUCompositor> m_compositor;
    QString m_backendName = QStringLiteral("not initialized");
    bool m_initialized = false;
};
