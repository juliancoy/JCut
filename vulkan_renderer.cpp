#include "vulkan_renderer.h"

#include <QColor>

namespace {
class VulkanClearRenderer final : public QVulkanWindowRenderer {
public:
    VulkanClearRenderer(QVulkanWindow* window, VulkanRendererState* state)
        : m_window(window), m_state(state) {}

    void initResources() override {}
    void initSwapChainResources() override {}
    void releaseSwapChainResources() override {}
    void releaseResources() override {}

    void startNextFrame() override
    {
        if (m_state) {
            m_state->profiling[QStringLiteral("last_presented_frame")] =
                static_cast<qint64>(m_state->currentFrame);
            m_state->profiling[QStringLiteral("clip_count")] = m_state->clipCount;
            m_state->profiling[QStringLiteral("view_mode")] =
                (m_state->viewMode == PreviewSurface::ViewMode::Audio)
                    ? QStringLiteral("audio")
                    : QStringLiteral("video");
            m_state->profiling[QStringLiteral("path")] = QStringLiteral("vulkan_native_scaffold");
        }

        m_window->frameReady();
        m_window->requestUpdate();
    }

private:
    QVulkanWindow* m_window = nullptr;
    VulkanRendererState* m_state = nullptr;
};
} // namespace

VulkanNativeWindow::VulkanNativeWindow(VulkanRendererState* state)
    : m_state(state)
{
}

QVulkanWindowRenderer* VulkanNativeWindow::createRenderer()
{
    return new VulkanClearRenderer(this, m_state);
}

VulkanNativeWindow* createVulkanNativeWindow(VulkanRendererState* state, QVulkanInstance* instance)
{
    auto* window = new VulkanNativeWindow(state);
    window->setVulkanInstance(instance);
    window->resize(640, 360);
    return window;
}
