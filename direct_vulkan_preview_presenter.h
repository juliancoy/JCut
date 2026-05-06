#pragma once

#include "preview_interaction_state.h"

#include <QJsonObject>
#include <QPointer>
#include <QVulkanInstance>

#include <memory>

class QWidget;
class QVulkanWindow;
class DirectVulkanPreviewWindow;

class DirectVulkanPreviewPresenter final {
public:
    explicit DirectVulkanPreviewPresenter(PreviewInteractionState* state, QWidget* parent = nullptr);
    ~DirectVulkanPreviewPresenter();

    DirectVulkanPreviewPresenter(const DirectVulkanPreviewPresenter&) = delete;
    DirectVulkanPreviewPresenter& operator=(const DirectVulkanPreviewPresenter&) = delete;

    QWidget* widget() const;
    bool isActive() const;
    QString failureReason() const;
    QString backendName() const;

    void requestUpdate();
    void updateTitle();
    QJsonObject profilingSnapshot() const;
    void resetProfilingStats();

private:
    std::unique_ptr<QVulkanInstance> m_instance;
    std::unique_ptr<QWidget> m_placeholder;
    DirectVulkanPreviewWindow* m_window = nullptr;
    PreviewInteractionState* m_state = nullptr;
    QString m_failureReason;
    bool m_active = false;
    int64_t m_presentedFrames = 0;
};
