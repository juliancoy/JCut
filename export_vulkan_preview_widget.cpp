#include "export_vulkan_preview_widget.h"

#include "direct_vulkan_preview_presenter.h"
#include "preview_interaction_state.h"

#include <QVBoxLayout>

ExportVulkanPreviewWidget::ExportVulkanPreviewWidget(QWidget* parent)
    : QWidget(parent),
      m_state(std::make_unique<PreviewInteractionState>())
{
    setMinimumSize(360, 202);
    setStyleSheet(QStringLiteral(
        "ExportVulkanPreviewWidget {"
        " background:#0e131b;"
        " border:1px solid #c9c2b8;"
        " border-radius:6px;"
        "}"));

    m_state->outputSize = QSize(1920, 1080);
    m_state->previewZoom = 1.0;
    m_presenter =
        std::make_unique<DirectVulkanPreviewPresenter>(
            m_state.get(), this, false);

    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(0);
    if (QWidget* preview = m_presenter->widget()) {
        preview->setMinimumSize(360, 202);
        layout->addWidget(preview);
    }
}

ExportVulkanPreviewWidget::~ExportVulkanPreviewWidget()
{
    clearPreview();
}

bool ExportVulkanPreviewWidget::isReady() const
{
    return m_presenter && m_presenter->isActive() &&
        !m_presenter->hasFailed() &&
        m_presenter->presentationTelemetrySnapshot()
                .presentedFrames > 0;
}

QString ExportVulkanPreviewWidget::failureReason() const
{
    return m_presenter ? m_presenter->failureReason()
                       : QStringLiteral(
                             "Vulkan export preview presenter is unavailable.");
}

void ExportVulkanPreviewWidget::setGpuPreviewFrame(
    const render_detail::OffscreenVulkanFrame& frame)
{
    if (!m_presenter) {
        return;
    }
    if (frame.size.valid()) {
        m_state->outputSize =
            QSize(frame.size.width, frame.size.height);
    }
    m_presenter->setGpuExportPreviewFrame(frame);
}

void ExportVulkanPreviewWidget::clearPreview()
{
    if (m_presenter) {
        m_presenter->clearGpuExportPreview();
    }
}
