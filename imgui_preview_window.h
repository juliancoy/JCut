#pragma once

#include <QRectF>
#include <QSize>
#include <QString>
#include <QVector>

#include <cstdint>
#include <memory>

namespace render_detail {
struct OffscreenVulkanFrame;
}

class ImGuiPreviewWindow final {
public:
    struct Impl;

    ImGuiPreviewWindow();
    ~ImGuiPreviewWindow();

    ImGuiPreviewWindow(const ImGuiPreviewWindow&) = delete;
    ImGuiPreviewWindow& operator=(const ImGuiPreviewWindow&) = delete;

    bool initialize(const QString& title, const QSize& initialSize);
    bool isActive() const;
    bool hasFailed() const;
    bool updatePending() const;
    bool isVisible() const;
    int64_t lastPresentedSourceFrame() const;
    QString failureReason() const;

    void setStatusText(const QString& text);
    void setWindowTitle(const QString& title);
    void pumpEvents();
    bool presentFrame(const render_detail::OffscreenVulkanFrame& frame,
                      int64_t frameNumber,
                      const QVector<QRectF>& boxes,
                      const QRectF& roiRect,
                      int detectionCount);

private:
    void shutdown();
    void markFailure(const QString& reason);

    std::unique_ptr<Impl> m_impl;
};
