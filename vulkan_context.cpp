#include "vulkan_context.h"

#include <QByteArrayList>
#include <QVersionNumber>
#include <QVulkanInstance>

VulkanContext::~VulkanContext()
{
    delete m_instance;
    m_instance = nullptr;
}

bool VulkanContext::initialize()
{
    if (m_instance) {
        return true;
    }

    auto* instance = new QVulkanInstance();
    instance->setApiVersion(QVersionNumber(1, 1));
    instance->setLayers(QByteArrayList() << "VK_LAYER_KHRONOS_validation");
    if (!instance->create()) {
        m_failureReason = QStringLiteral("Native Vulkan instance creation failed.");
        delete instance;
        return false;
    }

    m_instance = instance;
    m_failureReason.clear();
    return true;
}
