#include "direct_vulkan_preview_window_internal.h"

namespace {

template <typename Fn>
void runOnPreviewWindowThread(DirectVulkanPreviewWindow* window, Fn&& fn)
{
    if (!window) {
        return;
    }
    if (window->thread() == QThread::currentThread()) {
        std::forward<Fn>(fn)(window);
        return;
    }
    QPointer<DirectVulkanPreviewWindow> guarded(window);
    QMetaObject::invokeMethod(
        window,
        [guarded, fn = std::forward<Fn>(fn)]() mutable {
            if (guarded) {
                fn(guarded);
            }
        },
        Qt::QueuedConnection);
}

} // namespace

DirectVulkanPreviewWindow::~DirectVulkanPreviewWindow()
{
    cleanupDevice();
    delete m_renderer;
    m_renderer = nullptr;
}

void DirectVulkanPreviewWindow::refreshPhysicalDeviceList()
{
    m_availablePhysicalDevices.clear();
    m_availablePhysicalDeviceProperties.clear();
    QVulkanInstance* instance = vulkanInstance();
    if (!instance || instance->vkInstance() == VK_NULL_HANDLE) {
        return;
    }
    uint32_t count = 0;
    if (vkEnumeratePhysicalDevices(instance->vkInstance(), &count, nullptr) != VK_SUCCESS ||
        count == 0) {
        return;
    }
    m_availablePhysicalDevices.resize(static_cast<qsizetype>(count));
    if (vkEnumeratePhysicalDevices(instance->vkInstance(),
                                   &count,
                                   m_availablePhysicalDevices.data()) != VK_SUCCESS) {
        m_availablePhysicalDevices.clear();
        return;
    }
    m_availablePhysicalDeviceProperties.reserve(static_cast<qsizetype>(count));
    for (VkPhysicalDevice device : m_availablePhysicalDevices) {
        VkPhysicalDeviceProperties properties{};
        vkGetPhysicalDeviceProperties(device, &properties);
        m_availablePhysicalDeviceProperties.push_back(properties);
    }
}

QVulkanInfoVector<QVulkanExtension>
DirectVulkanPreviewWindow::supportedDeviceExtensions() const
{
    auto* self = const_cast<DirectVulkanPreviewWindow*>(this);
    if (self->m_availablePhysicalDevices.isEmpty()) {
        self->refreshPhysicalDeviceList();
    }
    const int preferredIndex =
        self->m_preferredPhysicalDeviceIndex >= 0 &&
                self->m_preferredPhysicalDeviceIndex <
                    self->m_availablePhysicalDevices.size()
            ? self->m_preferredPhysicalDeviceIndex
            : editor::gpu::chooseVulkanDevice(self->m_availablePhysicalDeviceProperties);
    if (preferredIndex < 0 ||
        preferredIndex >= self->m_availablePhysicalDevices.size()) {
        return {};
    }
    uint32_t count = 0;
    vkEnumerateDeviceExtensionProperties(
        self->m_availablePhysicalDevices.at(preferredIndex),
        nullptr,
        &count,
        nullptr);
    std::vector<VkExtensionProperties> properties(count);
    if (count > 0 &&
        vkEnumerateDeviceExtensionProperties(
            self->m_availablePhysicalDevices.at(preferredIndex),
            nullptr,
            &count,
            properties.data()) != VK_SUCCESS) {
        return {};
    }
    QVulkanInfoVector<QVulkanExtension> result;
    for (const VkExtensionProperties& property : properties) {
        result.push_back(
            QVulkanExtension{QByteArray(property.extensionName),
                             property.specVersion});
    }
    return result;
}

int DirectVulkanPreviewWindow::selectGraphicsPresentQueueFamily(
    VkPhysicalDevice device)
{
    QVulkanInstance* instance = vulkanInstance();
    if (!instance || device == VK_NULL_HANDLE) {
        return -1;
    }
    uint32_t familyCount = 0;
    vkGetPhysicalDeviceQueueFamilyProperties(device, &familyCount, nullptr);
    if (familyCount == 0) {
        return -1;
    }
    std::vector<VkQueueFamilyProperties> families(familyCount);
    vkGetPhysicalDeviceQueueFamilyProperties(
        device, &familyCount, families.data());
    for (uint32_t i = 0; i < familyCount; ++i) {
        if ((families[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) &&
            instance->supportsPresent(device, i, this)) {
            return static_cast<int>(i);
        }
    }
    return -1;
}

QSize DirectVulkanPreviewWindow::swapchainPixelSizeForWindow() const
{
    const QSize logicalSize = size();
    if (!logicalSize.isValid() || logicalSize.isEmpty()) {
        return QSize();
    }
    const qreal dpr = qMax<qreal>(1.0, devicePixelRatio());
    return QSize(qMax(1, qRound(static_cast<qreal>(logicalSize.width()) * dpr)),
                 qMax(1, qRound(static_cast<qreal>(logicalSize.height()) * dpr)));
}

void DirectVulkanPreviewWindow::cleanupSwapchain()
{
    if (m_rendererInitialized && m_renderer) {
        m_renderer->releaseResources();
        m_rendererInitialized = false;
    }
    if (m_device != VK_NULL_HANDLE) {
        vkDeviceWaitIdle(m_device);
    }
    for (VkFramebuffer framebuffer : m_swapchainFramebuffers) {
        if (framebuffer != VK_NULL_HANDLE) {
            vkDestroyFramebuffer(m_device, framebuffer, nullptr);
        }
    }
    m_swapchainFramebuffers.clear();
    for (VkImageView imageView : m_swapchainImageViews) {
        if (imageView != VK_NULL_HANDLE) {
            vkDestroyImageView(m_device, imageView, nullptr);
        }
    }
    m_swapchainImageViews.clear();
    m_swapchainImages.clear();
    if (m_defaultRenderPass != VK_NULL_HANDLE) {
        vkDestroyRenderPass(m_device, m_defaultRenderPass, nullptr);
        m_defaultRenderPass = VK_NULL_HANDLE;
    }
    if (m_swapchain != VK_NULL_HANDLE) {
        vkDestroySwapchainKHR(m_device, m_swapchain, nullptr);
        m_swapchain = VK_NULL_HANDLE;
    }
    m_swapchainPixelSize = QSize();
    m_colorFormat = VK_FORMAT_UNDEFINED;
    m_depthStencilFormat = VK_FORMAT_UNDEFINED;
    m_currentFramebuffer = VK_NULL_HANDLE;
    m_currentCommandBuffer = VK_NULL_HANDLE;
    m_currentSwapchainImageIndex = -1;
    m_swapchainDirty = true;
}

void DirectVulkanPreviewWindow::cleanupDevice()
{
    cleanupSwapchain();
    if (m_renderer) {
        m_renderer->releaseDeviceResources();
    }
    QVulkanInstance* instance = vulkanInstance();
    if (instance && m_device != VK_NULL_HANDLE) {
        instance->resetDeviceFunctions(m_device);
    }
    for (FrameResources& frame : m_frames) {
        if (frame.imageAcquiredSemaphore != VK_NULL_HANDLE) {
            vkDestroySemaphore(m_device, frame.imageAcquiredSemaphore, nullptr);
            frame.imageAcquiredSemaphore = VK_NULL_HANDLE;
        }
        if (frame.renderCompleteSemaphore != VK_NULL_HANDLE) {
            vkDestroySemaphore(m_device, frame.renderCompleteSemaphore, nullptr);
            frame.renderCompleteSemaphore = VK_NULL_HANDLE;
        }
        if (frame.inFlightFence != VK_NULL_HANDLE) {
            vkDestroyFence(m_device, frame.inFlightFence, nullptr);
            frame.inFlightFence = VK_NULL_HANDLE;
        }
        if (frame.commandPool != VK_NULL_HANDLE) {
            vkDestroyCommandPool(m_device, frame.commandPool, nullptr);
            frame.commandPool = VK_NULL_HANDLE;
            frame.commandBuffer = VK_NULL_HANDLE;
        }
    }
    m_frames.clear();
    if (m_device != VK_NULL_HANDLE) {
        vkDestroyDevice(m_device, nullptr);
        m_device = VK_NULL_HANDLE;
    }
    m_graphicsQueue = VK_NULL_HANDLE;
    m_graphicsQueueFamilyIndex = UINT32_MAX;
    m_physicalDevice = VK_NULL_HANDLE;
    m_hasPhysicalDeviceProperties = false;
    m_frameSubmitted = false;
    m_surface = VK_NULL_HANDLE;
}

bool DirectVulkanPreviewWindow::ensureVulkanReady()
{
    if (m_failureLatched) {
        return false;
    }
    QVulkanInstance* instance = vulkanInstance();
    if (!instance || !instance->isValid()) {
        markFailure(QStringLiteral(
            "Direct Vulkan preview requires a valid QVulkanInstance."));
        return false;
    }
    if (!handle()) {
        create();
    }
    if (m_surface == VK_NULL_HANDLE) {
        m_surface = QVulkanInstance::surfaceForWindow(this);
    }
    if (m_surface == VK_NULL_HANDLE) {
        return false;
    }
    if (m_device != VK_NULL_HANDLE) {
        return true;
    }
    if (m_availablePhysicalDevices.isEmpty()) {
        refreshPhysicalDeviceList();
    }
    if (m_availablePhysicalDevices.isEmpty()) {
        markFailure(QStringLiteral(
            "No Vulkan physical devices are available for direct preview."));
        return false;
    }

    QVector<int> candidateIndices;
    if (m_preferredPhysicalDeviceIndex >= 0 &&
        m_preferredPhysicalDeviceIndex < m_availablePhysicalDevices.size()) {
        candidateIndices.push_back(m_preferredPhysicalDeviceIndex);
    }
    const int automaticIndex =
        editor::gpu::chooseVulkanDevice(m_availablePhysicalDeviceProperties);
    if (automaticIndex >= 0 && !candidateIndices.contains(automaticIndex)) {
        candidateIndices.push_back(automaticIndex);
    }
    for (int i = 0; i < m_availablePhysicalDevices.size(); ++i) {
        if (!candidateIndices.contains(i)) {
            candidateIndices.push_back(i);
        }
    }

    QByteArrayList enabledDeviceExtensions;
    for (int candidateIndex : candidateIndices) {
        if (candidateIndex < 0 ||
            candidateIndex >= m_availablePhysicalDevices.size()) {
            continue;
        }
        const VkPhysicalDevice candidateDevice =
            m_availablePhysicalDevices.at(candidateIndex);
        const int queueFamily =
            selectGraphicsPresentQueueFamily(candidateDevice);
        if (queueFamily < 0) {
            continue;
        }

        uint32_t extensionCount = 0;
        vkEnumerateDeviceExtensionProperties(candidateDevice,
                                             nullptr,
                                             &extensionCount,
                                             nullptr);
        std::vector<VkExtensionProperties> extensionProperties(extensionCount);
        if (extensionCount > 0 &&
            vkEnumerateDeviceExtensionProperties(candidateDevice,
                                                 nullptr,
                                                 &extensionCount,
                                                 extensionProperties.data()) !=
                VK_SUCCESS) {
            continue;
        }
        auto hasExtension = [&extensionProperties](const QByteArray& name) {
            return std::any_of(
                extensionProperties.cbegin(),
                extensionProperties.cend(),
                [&name](const VkExtensionProperties& property) {
                    return QByteArray(property.extensionName) == name;
                });
        };
        if (!hasExtension(
                QByteArrayLiteral(VK_KHR_SWAPCHAIN_EXTENSION_NAME))) {
            continue;
        }
        enabledDeviceExtensions.clear();
        enabledDeviceExtensions.push_back(
            QByteArrayLiteral(VK_KHR_SWAPCHAIN_EXTENSION_NAME));
        for (const QByteArray& extension : m_requestedDeviceExtensions) {
            if (extension != QByteArrayLiteral(VK_KHR_SWAPCHAIN_EXTENSION_NAME) &&
                hasExtension(extension) &&
                !enabledDeviceExtensions.contains(extension)) {
                enabledDeviceExtensions.push_back(extension);
            }
        }

        std::vector<const char*> extensionNames;
        extensionNames.reserve(
            static_cast<size_t>(enabledDeviceExtensions.size()));
        for (const QByteArray& extension : enabledDeviceExtensions) {
            extensionNames.push_back(extension.constData());
        }

        const float queuePriority = 1.0f;
        VkDeviceQueueCreateInfo queueInfo{};
        queueInfo.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
        queueInfo.queueFamilyIndex = static_cast<uint32_t>(queueFamily);
        queueInfo.queueCount = 1;
        queueInfo.pQueuePriorities = &queuePriority;

        VkDeviceCreateInfo createInfo{};
        createInfo.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
        createInfo.queueCreateInfoCount = 1;
        createInfo.pQueueCreateInfos = &queueInfo;
        createInfo.enabledExtensionCount =
            static_cast<uint32_t>(extensionNames.size());
        createInfo.ppEnabledExtensionNames = extensionNames.data();

        VkDevice device = VK_NULL_HANDLE;
        if (vkCreateDevice(candidateDevice, &createInfo, nullptr, &device) !=
            VK_SUCCESS) {
            continue;
        }

        m_physicalDevice = candidateDevice;
        m_physicalDeviceProperties =
            m_availablePhysicalDeviceProperties.at(candidateIndex);
        m_hasPhysicalDeviceProperties = true;
        m_device = device;
        m_graphicsQueueFamilyIndex = static_cast<uint32_t>(queueFamily);
        vkGetDeviceQueue(
            m_device, m_graphicsQueueFamilyIndex, 0, &m_graphicsQueue);
        qInfo() << "[vulkan-preview] selected physical GPU" << candidateIndex
                << m_physicalDeviceProperties.deviceName
                << "preference=" << editor::gpu::preference();
        break;
    }

    if (m_device == VK_NULL_HANDLE) {
        markFailure(QStringLiteral(
            "No Vulkan graphics/present queue supports the direct preview surface."));
        return false;
    }

    constexpr int kFramesInFlight = 3;
    m_frames.resize(kFramesInFlight);
    for (FrameResources& frame : m_frames) {
        VkCommandPoolCreateInfo poolInfo{};
        poolInfo.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
        poolInfo.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
        poolInfo.queueFamilyIndex = m_graphicsQueueFamilyIndex;
        if (vkCreateCommandPool(m_device, &poolInfo, nullptr, &frame.commandPool) !=
            VK_SUCCESS) {
            markFailure(QStringLiteral(
                "Failed to create Vulkan command pool for direct preview."));
            cleanupDevice();
            return false;
        }

        VkCommandBufferAllocateInfo allocInfo{};
        allocInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
        allocInfo.commandPool = frame.commandPool;
        allocInfo.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        allocInfo.commandBufferCount = 1;
        if (vkAllocateCommandBuffers(
                m_device, &allocInfo, &frame.commandBuffer) != VK_SUCCESS) {
            markFailure(QStringLiteral(
                "Failed to allocate Vulkan command buffer for direct preview."));
            cleanupDevice();
            return false;
        }

        VkFenceCreateInfo fenceInfo{};
        fenceInfo.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
        fenceInfo.flags = VK_FENCE_CREATE_SIGNALED_BIT;
        if (vkCreateFence(m_device, &fenceInfo, nullptr, &frame.inFlightFence) !=
            VK_SUCCESS) {
            markFailure(QStringLiteral(
                "Failed to create Vulkan fence for direct preview."));
            cleanupDevice();
            return false;
        }

        VkSemaphoreCreateInfo semaphoreInfo{};
        semaphoreInfo.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;
        if (vkCreateSemaphore(m_device,
                              &semaphoreInfo,
                              nullptr,
                              &frame.imageAcquiredSemaphore) != VK_SUCCESS ||
            vkCreateSemaphore(m_device,
                              &semaphoreInfo,
                              nullptr,
                              &frame.renderCompleteSemaphore) != VK_SUCCESS) {
            markFailure(QStringLiteral(
                "Failed to create Vulkan semaphores for direct preview."));
            cleanupDevice();
            return false;
        }
    }

    if (!m_renderer) {
        m_renderer = new DirectVulkanPreviewRenderer(this, this);
    }
    m_currentFrameSlot = 0;
    return true;
}

bool DirectVulkanPreviewWindow::ensureSwapchain()
{
    if (!ensureVulkanReady()) {
        return false;
    }
    const QSize targetPixelSize = swapchainPixelSizeForWindow();
    if (!targetPixelSize.isValid() || targetPixelSize.isEmpty()) {
        return false;
    }
    if (!m_swapchainDirty && m_swapchain != VK_NULL_HANDLE &&
        m_swapchainPixelSize == targetPixelSize) {
        return true;
    }

    cleanupSwapchain();

    VkSurfaceCapabilitiesKHR capabilities{};
    if (vkGetPhysicalDeviceSurfaceCapabilitiesKHR(
            m_physicalDevice, m_surface, &capabilities) != VK_SUCCESS) {
        markFailure(QStringLiteral(
            "Failed to query Vulkan surface capabilities for direct preview."));
        return false;
    }

    uint32_t formatCount = 0;
    if (vkGetPhysicalDeviceSurfaceFormatsKHR(
            m_physicalDevice, m_surface, &formatCount, nullptr) != VK_SUCCESS ||
        formatCount == 0) {
        markFailure(QStringLiteral(
            "No Vulkan surface formats are available for direct preview."));
        return false;
    }
    std::vector<VkSurfaceFormatKHR> surfaceFormats(formatCount);
    if (vkGetPhysicalDeviceSurfaceFormatsKHR(m_physicalDevice,
                                             m_surface,
                                             &formatCount,
                                             surfaceFormats.data()) !=
        VK_SUCCESS) {
        markFailure(QStringLiteral(
            "Failed to enumerate Vulkan surface formats for direct preview."));
        return false;
    }

    VkSurfaceFormatKHR surfaceFormat = surfaceFormats.front();
    for (const VkSurfaceFormatKHR& candidate : surfaceFormats) {
        if ((candidate.format == VK_FORMAT_B8G8R8A8_UNORM ||
             candidate.format == VK_FORMAT_R8G8B8A8_UNORM) &&
            candidate.colorSpace == VK_COLOR_SPACE_SRGB_NONLINEAR_KHR) {
            surfaceFormat = candidate;
            break;
        }
    }

    uint32_t presentModeCount = 0;
    vkGetPhysicalDeviceSurfacePresentModesKHR(
        m_physicalDevice, m_surface, &presentModeCount, nullptr);
    std::vector<VkPresentModeKHR> presentModes(presentModeCount);
    if (presentModeCount > 0) {
        vkGetPhysicalDeviceSurfacePresentModesKHR(m_physicalDevice,
                                                  m_surface,
                                                  &presentModeCount,
                                                  presentModes.data());
    }
    VkPresentModeKHR presentMode = VK_PRESENT_MODE_FIFO_KHR;
    for (VkPresentModeKHR candidate : presentModes) {
        if (candidate == VK_PRESENT_MODE_MAILBOX_KHR) {
            presentMode = candidate;
            break;
        }
    }

    VkExtent2D extent{};
    if (capabilities.currentExtent.width != UINT32_MAX) {
        extent = capabilities.currentExtent;
    } else {
        extent.width = static_cast<uint32_t>(qBound(
            1,
            targetPixelSize.width(),
            static_cast<int>(capabilities.maxImageExtent.width)));
        extent.height = static_cast<uint32_t>(qBound(
            1,
            targetPixelSize.height(),
            static_cast<int>(capabilities.maxImageExtent.height)));
    }

    uint32_t imageCount =
        std::max<uint32_t>(3u, capabilities.minImageCount + 1u);
    if (capabilities.maxImageCount > 0) {
        imageCount = std::min(imageCount, capabilities.maxImageCount);
    }

    VkSwapchainCreateInfoKHR swapchainInfo{};
    swapchainInfo.sType = VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR;
    swapchainInfo.surface = m_surface;
    swapchainInfo.minImageCount = imageCount;
    swapchainInfo.imageFormat = surfaceFormat.format;
    swapchainInfo.imageColorSpace = surfaceFormat.colorSpace;
    swapchainInfo.imageExtent = extent;
    swapchainInfo.imageArrayLayers = 1;
    swapchainInfo.imageUsage =
        VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
    swapchainInfo.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;
    swapchainInfo.preTransform = capabilities.currentTransform;
    swapchainInfo.compositeAlpha =
        (capabilities.supportedCompositeAlpha &
         VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR)
            ? VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR
            : VK_COMPOSITE_ALPHA_INHERIT_BIT_KHR;
    swapchainInfo.presentMode = presentMode;
    swapchainInfo.clipped = VK_TRUE;
    if (vkCreateSwapchainKHR(m_device, &swapchainInfo, nullptr, &m_swapchain) !=
        VK_SUCCESS) {
        markFailure(QStringLiteral(
            "Failed to create Vulkan swapchain for direct preview."));
        return false;
    }

    uint32_t swapchainImageCount = 0;
    if (vkGetSwapchainImagesKHR(
            m_device, m_swapchain, &swapchainImageCount, nullptr) != VK_SUCCESS ||
        swapchainImageCount == 0) {
        markFailure(QStringLiteral(
            "Failed to query Vulkan swapchain images for direct preview."));
        cleanupSwapchain();
        return false;
    }
    m_swapchainImages.resize(swapchainImageCount);
    if (vkGetSwapchainImagesKHR(m_device,
                                m_swapchain,
                                &swapchainImageCount,
                                m_swapchainImages.data()) != VK_SUCCESS) {
        markFailure(QStringLiteral(
            "Failed to load Vulkan swapchain images for direct preview."));
        cleanupSwapchain();
        return false;
    }

    m_colorFormat = surfaceFormat.format;
    m_swapchainPixelSize = QSize(static_cast<int>(extent.width),
                                 static_cast<int>(extent.height));
    m_swapchainImageViews.resize(m_swapchainImages.size(), VK_NULL_HANDLE);
    for (size_t i = 0; i < m_swapchainImages.size(); ++i) {
        VkImageViewCreateInfo imageViewInfo{};
        imageViewInfo.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
        imageViewInfo.image = m_swapchainImages[i];
        imageViewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
        imageViewInfo.format = m_colorFormat;
        imageViewInfo.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        imageViewInfo.subresourceRange.levelCount = 1;
        imageViewInfo.subresourceRange.layerCount = 1;
        if (vkCreateImageView(m_device,
                              &imageViewInfo,
                              nullptr,
                              &m_swapchainImageViews[i]) != VK_SUCCESS) {
            markFailure(QStringLiteral(
                "Failed to create Vulkan swapchain image view for direct preview."));
            cleanupSwapchain();
            return false;
        }
    }

    VkAttachmentDescription colorAttachment{};
    colorAttachment.format = m_colorFormat;
    colorAttachment.samples = VK_SAMPLE_COUNT_1_BIT;
    colorAttachment.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
    colorAttachment.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    colorAttachment.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    colorAttachment.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    colorAttachment.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    colorAttachment.finalLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;

    VkAttachmentReference colorRef{};
    colorRef.attachment = 0;
    colorRef.layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;

    VkSubpassDescription subpass{};
    subpass.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
    subpass.colorAttachmentCount = 1;
    subpass.pColorAttachments = &colorRef;

    VkSubpassDependency dependency{};
    dependency.srcSubpass = VK_SUBPASS_EXTERNAL;
    dependency.dstSubpass = 0;
    dependency.srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    dependency.dstStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    dependency.dstAccessMask =
        VK_ACCESS_COLOR_ATTACHMENT_READ_BIT |
        VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;

    VkRenderPassCreateInfo renderPassInfo{};
    renderPassInfo.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO;
    renderPassInfo.attachmentCount = 1;
    renderPassInfo.pAttachments = &colorAttachment;
    renderPassInfo.subpassCount = 1;
    renderPassInfo.pSubpasses = &subpass;
    renderPassInfo.dependencyCount = 1;
    renderPassInfo.pDependencies = &dependency;
    if (vkCreateRenderPass(
            m_device, &renderPassInfo, nullptr, &m_defaultRenderPass) !=
        VK_SUCCESS) {
        markFailure(QStringLiteral(
            "Failed to create Vulkan render pass for direct preview."));
        cleanupSwapchain();
        return false;
    }

    m_swapchainFramebuffers.resize(m_swapchainImageViews.size(), VK_NULL_HANDLE);
    for (size_t i = 0; i < m_swapchainImageViews.size(); ++i) {
        VkImageView attachments[] = {m_swapchainImageViews[i]};
        VkFramebufferCreateInfo framebufferInfo{};
        framebufferInfo.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
        framebufferInfo.renderPass = m_defaultRenderPass;
        framebufferInfo.attachmentCount = 1;
        framebufferInfo.pAttachments = attachments;
        framebufferInfo.width = extent.width;
        framebufferInfo.height = extent.height;
        framebufferInfo.layers = 1;
        if (vkCreateFramebuffer(m_device,
                                &framebufferInfo,
                                nullptr,
                                &m_swapchainFramebuffers[i]) != VK_SUCCESS) {
            markFailure(QStringLiteral(
                "Failed to create Vulkan framebuffer for direct preview."));
            cleanupSwapchain();
            return false;
        }
    }

    m_swapchainDirty = false;
    if (m_renderer && !m_rendererInitialized) {
        m_renderer->initResources();
        m_rendererInitialized = !m_failureLatched;
    }
    return isValid();
}

void DirectVulkanPreviewWindow::markSwapchainDirty()
{
    m_swapchainDirty = true;
}

void DirectVulkanPreviewWindow::renderNow()
{
    if (m_frameInProgress || !isExposed()) {
        return;
    }
    if (!ensureSwapchain()) {
        return;
    }
    if (!m_renderer || m_frames.empty()) {
        return;
    }

    FrameResources& frame =
        m_frames[static_cast<size_t>(
            m_currentFrameSlot % static_cast<int>(m_frames.size()))];
    const VkResult frameOwnership =
        vkGetFenceStatus(m_device, frame.inFlightFence);
    if (frameOwnership == VK_NOT_READY) {
        schedulePreviewRetry();
        return;
    }
    if (frameOwnership != VK_SUCCESS) {
        markFailure(QStringLiteral(
            "Failed to query direct preview frame ownership (VkResult %1).")
                        .arg(static_cast<int>(frameOwnership)));
        return;
    }
    VkResult acquireResult = vkAcquireNextImageKHR(
        m_device,
        m_swapchain,
        kPreviewAcquireTimeoutNs,
        frame.imageAcquiredSemaphore,
        VK_NULL_HANDLE,
        reinterpret_cast<uint32_t*>(&m_currentSwapchainImageIndex));
    if (acquireResult == VK_ERROR_OUT_OF_DATE_KHR ||
        acquireResult == VK_SUBOPTIMAL_KHR ||
        acquireResult == VK_ERROR_SURFACE_LOST_KHR) {
        markSwapchainDirty();
        schedulePreviewRetry();
        return;
    }
    if (acquireResult == VK_NOT_READY || acquireResult == VK_TIMEOUT) {
        schedulePreviewRetry();
        return;
    }
    if (acquireResult != VK_SUCCESS) {
        markFailure(QStringLiteral(
            "Failed to acquire a Vulkan swapchain image for direct preview."));
        return;
    }

    vkResetCommandPool(m_device, frame.commandPool, 0);
    VkCommandBufferBeginInfo beginInfo{};
    beginInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    beginInfo.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    if (vkBeginCommandBuffer(frame.commandBuffer, &beginInfo) != VK_SUCCESS) {
        markFailure(QStringLiteral(
            "Failed to begin Vulkan command buffer for direct preview."));
        return;
    }

    m_currentFramebuffer =
        m_swapchainFramebuffers[static_cast<size_t>(m_currentSwapchainImageIndex)];
    m_currentCommandBuffer = frame.commandBuffer;
    m_frameSubmitted = false;
    m_renderer->startNextFrame();
    if (m_frameInProgress) {
        markPreviewUpdateDelivered();
    }
    if (!m_frameSubmitted && !m_failureLatched) {
        markFailure(QStringLiteral(
            "Direct preview renderer returned without presenting the current frame."));
    }
    m_currentCommandBuffer = VK_NULL_HANDLE;
    m_currentFramebuffer = VK_NULL_HANDLE;
    m_currentFrameSlot =
        (m_currentFrameSlot + 1) % std::max(1, static_cast<int>(m_frames.size()));
}

bool DirectVulkanPreviewWindow::frameReady()
{
    if (m_frameSubmitted ||
        m_device == VK_NULL_HANDLE ||
        m_currentCommandBuffer == VK_NULL_HANDLE ||
        m_frames.empty()) {
        return false;
    }
    FrameResources& frame =
        m_frames[static_cast<size_t>(
            m_currentFrameSlot % static_cast<int>(m_frames.size()))];
    if (vkEndCommandBuffer(m_currentCommandBuffer) != VK_SUCCESS) {
        m_frameCompletionSemaphore = VK_NULL_HANDLE;
        markFailure(QStringLiteral(
            "Failed to finalize Vulkan command buffer for direct preview."));
        return false;
    }

    VkPipelineStageFlags waitStage =
        VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    VkSubmitInfo submitInfo{};
    submitInfo.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    submitInfo.waitSemaphoreCount = 1;
    submitInfo.pWaitSemaphores = &frame.imageAcquiredSemaphore;
    submitInfo.pWaitDstStageMask = &waitStage;
    submitInfo.commandBufferCount = 1;
    submitInfo.pCommandBuffers = &m_currentCommandBuffer;
    const std::array<VkSemaphore, 2> signalSemaphores{
        frame.renderCompleteSemaphore,
        m_frameCompletionSemaphore};
    submitInfo.signalSemaphoreCount =
        m_frameCompletionSemaphore == VK_NULL_HANDLE ? 1u : 2u;
    submitInfo.pSignalSemaphores = signalSemaphores.data();
    if (QVulkanInstance* instance = vulkanInstance()) {
        instance->presentAboutToBeQueued(this);
    }
    if (vkResetFences(m_device, 1, &frame.inFlightFence) != VK_SUCCESS) {
        m_frameCompletionSemaphore = VK_NULL_HANDLE;
        markFailure(QStringLiteral(
            "Failed to reset direct preview frame ownership fence."));
        return false;
    }
    if (vkQueueSubmit(
            m_graphicsQueue, 1, &submitInfo, frame.inFlightFence) != VK_SUCCESS) {
        m_frameCompletionSemaphore = VK_NULL_HANDLE;
        markFailure(QStringLiteral(
            "Failed to submit a Vulkan frame for direct preview."));
        return false;
    }
    m_frameCompletionSemaphore = VK_NULL_HANDLE;

    uint32_t imageIndex =
        static_cast<uint32_t>(std::max(0, m_currentSwapchainImageIndex));
    VkPresentInfoKHR presentInfo{};
    presentInfo.sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR;
    presentInfo.waitSemaphoreCount = 1;
    presentInfo.pWaitSemaphores = &frame.renderCompleteSemaphore;
    presentInfo.swapchainCount = 1;
    presentInfo.pSwapchains = &m_swapchain;
    presentInfo.pImageIndices = &imageIndex;
    const VkResult presentResult =
        vkQueuePresentKHR(m_graphicsQueue, &presentInfo);
    if (QVulkanInstance* instance = vulkanInstance()) {
        instance->presentQueued(this);
    }
    if (presentResult == VK_ERROR_OUT_OF_DATE_KHR ||
        presentResult == VK_SUBOPTIMAL_KHR ||
        presentResult == VK_ERROR_SURFACE_LOST_KHR) {
        markSwapchainDirty();
        m_updateDirty = true;
    } else if (presentResult != VK_SUCCESS) {
        markFailure(QStringLiteral(
            "Failed to present a Vulkan frame for direct preview."));
        return true;
    }
    m_frameSubmitted = true;
    return true;
}

QWidget* createDirectVulkanPreviewWindowContainer(DirectVulkanPreviewWindow* window,
                                                  QWidget* parent)
{
    return window ? QWidget::createWindowContainer(window, parent) : nullptr;
}

DirectVulkanPreviewWindow* createDirectVulkanPreviewWindow(
    PreviewInteractionState* state,
    DirectVulkanPresentationTelemetry* presentationTelemetry,
    DirectVulkanPreviewStats* stats,
    bool* active,
    QString* failureReason,
    bool enableAudioPipeline,
    std::function<void(const QString&)> failureCallback)
{
    return new DirectVulkanPreviewWindow(state,
                                         presentationTelemetry,
                                         stats,
                                         active,
                                         failureReason,
                                         enableAudioPipeline,
                                         std::move(failureCallback));
}

void directVulkanPreviewWindowSetVulkanInstance(DirectVulkanPreviewWindow* window,
                                                QVulkanInstance* instance)
{
    if (window) {
        window->setVulkanInstance(instance);
        QVector<VkPhysicalDeviceProperties> devices;
        if (instance && instance->vkInstance() != VK_NULL_HANDLE) {
            uint32_t deviceCount = 0;
            if (vkEnumeratePhysicalDevices(
                    instance->vkInstance(), &deviceCount, nullptr) == VK_SUCCESS &&
                deviceCount > 0) {
                std::vector<VkPhysicalDevice> physicalDevices(deviceCount);
                if (vkEnumeratePhysicalDevices(instance->vkInstance(),
                                               &deviceCount,
                                               physicalDevices.data()) == VK_SUCCESS) {
                    devices.reserve(static_cast<qsizetype>(deviceCount));
                    for (VkPhysicalDevice device : physicalDevices) {
                        VkPhysicalDeviceProperties properties{};
                        vkGetPhysicalDeviceProperties(device, &properties);
                        devices.push_back(properties);
                    }
                }
            }
        }
        const int selectedIndex = editor::gpu::chooseVulkanDevice(devices);
        if (selectedIndex >= 0) {
            window->setPreferredPhysicalDeviceIndex(selectedIndex);
        }
    }
}

QVulkanInfoVector<QVulkanExtension> directVulkanPreviewWindowSupportedDeviceExtensions(
    DirectVulkanPreviewWindow* window)
{
    return window ? window->supportedDeviceExtensions() : QVulkanInfoVector<QVulkanExtension>();
}

void directVulkanPreviewWindowSetDeviceExtensions(DirectVulkanPreviewWindow* window,
                                                  const QByteArrayList& extensions)
{
    if (window) {
        window->setDeviceExtensions(extensions);
    }
}

void directVulkanPreviewWindowResize(DirectVulkanPreviewWindow* window, const QSize& size)
{
    if (window) {
        window->resize(size);
    }
}

void directVulkanPreviewWindowSetInteractionCallbacks(
    DirectVulkanPreviewWindow* window,
    std::function<void(const QString&)> selectionRequested,
    std::function<void(const QString&, qreal, qreal, bool)> moveRequested,
    std::function<void(const QString&, qreal, qreal, qreal, qreal, bool)> transformRequested,
    std::function<void(int64_t)> playbackSampleRequested,
    std::function<void(const QString&, qreal, qreal)> correctionPointRequested,
    std::function<void(const QString&, int64_t, int64_t, qreal, qreal)> maskFuzzyRemovePointRequested,
    std::function<void(const QString&, qreal, qreal)> speakerPointRequested,
    std::function<void(const QString&, qreal, qreal, qreal)> speakerBoxRequested,
    std::function<void(const QString&, int, const QString&, int64_t, qreal, qreal, qreal)> faceStreamBoxRequested,
    std::function<void(const QString&, int, const QString&, int64_t, qreal, qreal, qreal)> faceStreamBoxFocusClearRequested,
    std::function<void(const QString&)> faceStreamBoxClickStatus,
    std::function<void(const QString&)> createKeyframeRequested)
{
    if (!window) {
        return;
    }
    window->setInteractionCallbacks(std::move(selectionRequested),
                                    std::move(moveRequested),
                                    std::move(transformRequested),
                                    std::move(playbackSampleRequested),
                                    std::move(correctionPointRequested),
                                    std::move(maskFuzzyRemovePointRequested),
                                    std::move(speakerPointRequested),
                                    std::move(speakerBoxRequested),
                                    std::move(faceStreamBoxRequested),
                                    std::move(faceStreamBoxFocusClearRequested),
                                    std::move(faceStreamBoxClickStatus),
                                    std::move(createKeyframeRequested));
}

bool directVulkanPreviewWindowUpdatePending(DirectVulkanPreviewWindow* window)
{
    return window && window->updatePending();
}

bool directVulkanPreviewWindowIsValid(DirectVulkanPreviewWindow* window)
{
    return window && window->isValid();
}

bool directVulkanPreviewWindowIsExposed(DirectVulkanPreviewWindow* window)
{
    return window && window->isExposed();
}

void directVulkanPreviewWindowSchedulePreviewUpdate(DirectVulkanPreviewWindow* window)
{
    runOnPreviewWindowThread(
        window,
        [](DirectVulkanPreviewWindow* target) {
            target->schedulePreviewUpdate();
        });
}

void directVulkanPreviewWindowResetProfilingAnchors(
    DirectVulkanPreviewWindow* window)
{
    runOnPreviewWindowThread(
        window,
        [](DirectVulkanPreviewWindow* target) {
            target->resetProfilingAnchors();
        });
}

void directVulkanPreviewWindowRequestPipelineThumbnailReadback(DirectVulkanPreviewWindow* window)
{
    runOnPreviewWindowThread(
        window,
        [](DirectVulkanPreviewWindow* target) {
            target->requestPipelineThumbnailReadback();
        });
}

QImage directVulkanPreviewWindowLatestPipelineThumbnailReadback(DirectVulkanPreviewWindow* window)
{
    return window ? window->latestVulkanReadbackImage() : QImage();
}

bool directVulkanPreviewWindowPipelineThumbnailReadbackPending(DirectVulkanPreviewWindow* window)
{
    return window && window->pipelineThumbnailReadbackPending();
}

void directVulkanPreviewWindowRaise(DirectVulkanPreviewWindow* window)
{
    runOnPreviewWindowThread(
        window,
        [](DirectVulkanPreviewWindow* target) {
            target->raise();
        });
}

void directVulkanPreviewWindowHide(DirectVulkanPreviewWindow* window)
{
    runOnPreviewWindowThread(
        window,
        [](DirectVulkanPreviewWindow* target) {
            target->hide();
        });
}

void directVulkanPreviewWindowSetTitle(DirectVulkanPreviewWindow* window, const QString& title)
{
    runOnPreviewWindowThread(
        window,
        [title](DirectVulkanPreviewWindow* target) {
            target->setTitle(title);
        });
}

void directVulkanPreviewWindowSetGpuExportPreviewFrame(
    DirectVulkanPreviewWindow* window,
    const render_detail::OffscreenVulkanFrame& frame)
{
    runOnPreviewWindowThread(
        window,
        [frame](DirectVulkanPreviewWindow* target) {
            target->setGpuExportPreviewFrame(frame);
        });
}

void directVulkanPreviewWindowClearGpuExportPreview(
    DirectVulkanPreviewWindow* window)
{
    runOnPreviewWindowThread(
        window,
        [](DirectVulkanPreviewWindow* target) {
            target->clearGpuExportPreview();
        });
}

bool directVulkanPreviewWindowIsVisible(DirectVulkanPreviewWindow* window)
{
    return window && window->isVisible();
}

QString directVulkanPreviewWindowCursorShape(DirectVulkanPreviewWindow* window)
{
    if (!window) {
        return QString();
    }
    switch (window->cursor().shape()) {
    case Qt::ArrowCursor:
        return QStringLiteral("arrow");
    case Qt::UpArrowCursor:
        return QStringLiteral("up_arrow");
    case Qt::CrossCursor:
        return QStringLiteral("cross");
    case Qt::WaitCursor:
        return QStringLiteral("wait");
    case Qt::IBeamCursor:
        return QStringLiteral("ibeam");
    case Qt::SizeVerCursor:
        return QStringLiteral("size_ver");
    case Qt::SizeHorCursor:
        return QStringLiteral("size_hor");
    case Qt::SizeBDiagCursor:
        return QStringLiteral("size_bdiag");
    case Qt::SizeFDiagCursor:
        return QStringLiteral("size_fdiag");
    case Qt::SizeAllCursor:
        return QStringLiteral("size_all");
    case Qt::BlankCursor:
        return QStringLiteral("blank");
    case Qt::SplitVCursor:
        return QStringLiteral("split_v");
    case Qt::SplitHCursor:
        return QStringLiteral("split_h");
    case Qt::PointingHandCursor:
        return QStringLiteral("pointing_hand");
    case Qt::ForbiddenCursor:
        return QStringLiteral("forbidden");
    case Qt::OpenHandCursor:
        return QStringLiteral("open_hand");
    case Qt::ClosedHandCursor:
        return QStringLiteral("closed_hand");
    case Qt::WhatsThisCursor:
        return QStringLiteral("whats_this");
    case Qt::BusyCursor:
        return QStringLiteral("busy");
    default:
        return QStringLiteral("other");
    }
}
