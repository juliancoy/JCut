#pragma once

#include <QString>

class QVulkanInstance;

class VulkanContext {
public:
    VulkanContext() = default;
    ~VulkanContext();

    VulkanContext(const VulkanContext&) = delete;
    VulkanContext& operator=(const VulkanContext&) = delete;

    bool initialize();
    QVulkanInstance* instance() const { return m_instance; }
    QString failureReason() const { return m_failureReason; }

private:
    QVulkanInstance* m_instance = nullptr;
    QString m_failureReason;
};
