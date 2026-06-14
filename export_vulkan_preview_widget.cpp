#include "export_vulkan_preview_widget.h"

#include "vulkan_pipeline.h"
#include "vulkan_resources.h"

#include <QMutex>
#include <QMutexLocker>
#include <QVBoxLayout>
#include <QVulkanDeviceFunctions>
#include <QVulkanInstance>
#include <QVulkanWindow>
#include <QVersionNumber>

#include <algorithm>
#include <memory>

namespace {

class ExportVulkanPreviewWindow;

class ExportVulkanPreviewRenderer final : public QVulkanWindowRenderer {
public:
    explicit ExportVulkanPreviewRenderer(ExportVulkanPreviewWindow* window)
        : m_window(window) {}

    void initResources() override;
    void releaseResources() override;
    void startNextFrame() override;
    void logicalDeviceLost() override {}
    void physicalDeviceLost() override {}

private:
    ExportVulkanPreviewWindow* m_window = nullptr;
    QVulkanWindow* m_qwindow = nullptr;
    QVulkanDeviceFunctions* m_funcs = nullptr;
    std::unique_ptr<VulkanResources> m_resources;
    std::unique_ptr<VulkanPipeline> m_pipeline;
};

class ExportVulkanPreviewWindow final : public QVulkanWindow {
public:
    ExportVulkanPreviewWindow()
    {
        setSurfaceType(QSurface::VulkanSurface);
        setTitle(QStringLiteral("JCut Export Vulkan Preview"));
        setFlags(QVulkanWindow::PersistentResources);
    }

    QVulkanWindowRenderer* createRenderer() override
    {
        return new ExportVulkanPreviewRenderer(this);
    }

    void setPreviewFrame(const QImage& image)
    {
        QMutexLocker locker(&m_mutex);
        m_pendingFrame = image.convertToFormat(QImage::Format_RGBA8888);
        m_hasPendingFrame = !m_pendingFrame.isNull();
        requestUpdate();
    }

    void clearPreview()
    {
        QMutexLocker locker(&m_mutex);
        m_pendingFrame = QImage();
        m_latestFrame = QImage();
        m_hasPendingFrame = false;
        m_hasFrame = false;
        requestUpdate();
    }

    QImage takePendingFrame()
    {
        QMutexLocker locker(&m_mutex);
        if (!m_hasPendingFrame) {
            return QImage();
        }
        QImage frame = m_pendingFrame;
        m_latestFrame = frame;
        m_pendingFrame = QImage();
        m_hasPendingFrame = false;
        m_hasFrame = !m_latestFrame.isNull();
        return frame;
    }

    bool hasFrame() const
    {
        QMutexLocker locker(&m_mutex);
        return m_hasFrame || m_hasPendingFrame;
    }

private:
    mutable QMutex m_mutex;
    QImage m_pendingFrame;
    QImage m_latestFrame;
    bool m_hasPendingFrame = false;
    bool m_hasFrame = false;
};

VkClearValue clearColor(float r, float g, float b, float a = 1.0f)
{
    VkClearValue clear{};
    clear.color.float32[0] = r;
    clear.color.float32[1] = g;
    clear.color.float32[2] = b;
    clear.color.float32[3] = a;
    return clear;
}

} // namespace

void ExportVulkanPreviewRenderer::initResources()
{
    m_qwindow = m_window;
    m_funcs = m_qwindow && m_qwindow->vulkanInstance()
        ? m_qwindow->vulkanInstance()->deviceFunctions(m_qwindow->device())
        : nullptr;
    if (!m_qwindow || !m_funcs) {
        return;
    }

    m_resources = std::make_unique<VulkanResources>();
    if (!m_resources->initialize(m_qwindow->physicalDevice(), m_qwindow->device(), m_funcs)) {
        m_resources.reset();
        return;
    }
    m_pipeline = std::make_unique<VulkanPipeline>();
    QString error;
    if (!m_pipeline->initialize(m_qwindow->device(),
                                m_funcs,
                                m_qwindow->defaultRenderPass(),
                                m_resources->descriptorSetLayout(),
                                &error)) {
        m_pipeline.reset();
    }
}

void ExportVulkanPreviewRenderer::releaseResources()
{
    m_pipeline.reset();
    m_resources.reset();
    m_funcs = nullptr;
    m_qwindow = nullptr;
}

void ExportVulkanPreviewRenderer::startNextFrame()
{
    if (!m_qwindow || !m_funcs) {
        return;
    }
    const QSize size = m_qwindow->swapChainImageSize();
    VkCommandBuffer cb = m_qwindow->currentCommandBuffer();
    const QImage pending = m_window ? m_window->takePendingFrame() : QImage();
    if (!pending.isNull() && m_resources) {
        m_resources->uploadImageTexture(cb, pending);
    }

    VkClearValue clears[2]{};
    clears[0] = clearColor(0.055f, 0.075f, 0.105f);
    clears[1].depthStencil.depth = 1.0f;
    clears[1].depthStencil.stencil = 0;

    VkRenderPassBeginInfo rp{};
    rp.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
    rp.renderPass = m_qwindow->defaultRenderPass();
    rp.framebuffer = m_qwindow->currentFramebuffer();
    rp.renderArea.offset = {0, 0};
    rp.renderArea.extent = {
        static_cast<uint32_t>(std::max(1, size.width())),
        static_cast<uint32_t>(std::max(1, size.height()))
    };
    rp.clearValueCount = m_qwindow->depthStencilFormat() == VK_FORMAT_UNDEFINED ? 1u : 2u;
    rp.pClearValues = clears;

    m_funcs->vkCmdBeginRenderPass(cb, &rp, VK_SUBPASS_CONTENTS_INLINE);
    if (m_pipeline && m_pipeline->isReady() && m_resources && m_window && m_window->hasFrame()) {
        VkViewport viewport{};
        viewport.x = 0.0f;
        viewport.y = 0.0f;
        viewport.width = static_cast<float>(std::max(1, size.width()));
        viewport.height = static_cast<float>(std::max(1, size.height()));
        viewport.minDepth = 0.0f;
        viewport.maxDepth = 1.0f;
        VkRect2D scissor{};
        scissor.offset = {0, 0};
        scissor.extent = {
            static_cast<uint32_t>(std::max(1, size.width())),
            static_cast<uint32_t>(std::max(1, size.height()))
        };
        VulkanPipeline::Push push{};
        m_pipeline->bindAndDraw(cb, viewport, scissor, m_resources->descriptorSet(), push);
    }
    m_funcs->vkCmdEndRenderPass(cb);
    m_qwindow->frameReady();
}

ExportVulkanPreviewWidget::ExportVulkanPreviewWidget(QWidget* parent)
    : QWidget(parent)
{
    setMinimumSize(360, 202);
    setStyleSheet(QStringLiteral("background:#0e131b; border:1px solid #c9c2b8; border-radius:6px;"));

    m_instance = std::make_unique<QVulkanInstance>();
    m_instance->setApiVersion(QVersionNumber(1, 1));
    if (!m_instance->create()) {
        m_instance.reset();
        return;
    }

    auto* window = new ExportVulkanPreviewWindow;
    window->setVulkanInstance(m_instance.get());
    m_window = window;
    m_container = QWidget::createWindowContainer(window, this);
    if (!m_container) {
        delete window;
        m_window = nullptr;
        m_instance.reset();
        return;
    }
    m_container->setMinimumSize(360, 202);
    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(0);
    layout->addWidget(m_container);
}

ExportVulkanPreviewWidget::~ExportVulkanPreviewWidget()
{
    delete m_container;
    m_container = nullptr;
    m_window = nullptr;
    m_instance.reset();
}

bool ExportVulkanPreviewWidget::isReady() const
{
    return m_container != nullptr;
}

void ExportVulkanPreviewWidget::setPreviewFrame(const QImage& image)
{
    if (!m_container || image.isNull()) {
        return;
    }
    if (auto* window = dynamic_cast<ExportVulkanPreviewWindow*>(m_window)) {
        window->setPreviewFrame(image);
    }
}

void ExportVulkanPreviewWidget::clearPreview()
{
    if (!m_container) {
        return;
    }
    if (auto* window = dynamic_cast<ExportVulkanPreviewWindow*>(m_window)) {
        window->clearPreview();
    }
}
