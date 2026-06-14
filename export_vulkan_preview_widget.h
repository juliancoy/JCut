#pragma once

#include <QImage>
#include <QWidget>

#include <memory>

class QVulkanInstance;
class QVulkanWindow;

class ExportVulkanPreviewWidget final : public QWidget {
public:
    explicit ExportVulkanPreviewWidget(QWidget* parent = nullptr);
    ~ExportVulkanPreviewWidget() override;

    bool isReady() const;
    void setPreviewFrame(const QImage& image);
    void clearPreview();

private:
    std::unique_ptr<QVulkanInstance> m_instance;
    QVulkanWindow* m_window = nullptr;
    QWidget* m_container = nullptr;
};
