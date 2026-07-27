#pragma once

#include "core/offscreen_vulkan_frame.h"

#include <QWidget>

#include <memory>

class DirectVulkanPreviewPresenter;
struct PreviewInteractionState;

class ExportVulkanPreviewWidget final : public QWidget {
public:
    explicit ExportVulkanPreviewWidget(QWidget* parent = nullptr);
    ~ExportVulkanPreviewWidget() override;

    bool isReady() const;
    QString failureReason() const;
    void setGpuPreviewFrame(
        const render_detail::OffscreenVulkanFrame& frame);
    void clearPreview();

private:
    std::unique_ptr<PreviewInteractionState> m_state;
    std::unique_ptr<DirectVulkanPreviewPresenter> m_presenter;
};
