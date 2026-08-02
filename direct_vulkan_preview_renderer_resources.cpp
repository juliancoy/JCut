#include "direct_vulkan_preview_window_internal.h"

void DirectVulkanPreviewRenderer::initResources()
{
    m_devFuncs = m_window && m_window->vulkanInstance()
        ? m_window->vulkanInstance()->deviceFunctions(m_window->device())
        : nullptr;
    if (!m_resources) {
        if (m_window && m_window->physicalDeviceProperties()) {
            const VkPhysicalDeviceProperties* props =
                m_window->physicalDeviceProperties();
            qInfo().noquote()
                << QStringLiteral(
                       "[vulkan-preview] direct presenter device=%1 vendor=0x%2 type=%3")
                       .arg(QString::fromLatin1(props->deviceName))
                       .arg(QString::number(props->vendorID, 16))
                       .arg(static_cast<int>(props->deviceType));
        }
        m_resources = std::make_unique<VulkanResources>();
        if (!m_resources->initialize(
                m_window->physicalDevice(),
                m_window->device(),
                m_devFuncs)) {
            if (m_owner) {
                m_owner->markFailure(QStringLiteral(
                    "Failed to initialize direct presenter Vulkan resources."));
            }
            return;
        }
        m_importSemaphoreFd =
            reinterpret_cast<PFN_vkImportSemaphoreFdKHR>(
                vkGetDeviceProcAddr(m_window->device(),
                                    "vkImportSemaphoreFdKHR"));
    }
    if (!m_playbackStatusOverlayResources) {
        m_playbackStatusOverlayResources =
            std::make_unique<VulkanResources>();
        if (!m_playbackStatusOverlayResources->initialize(
                m_window->physicalDevice(),
                m_window->device(),
                m_devFuncs)) {
            if (m_owner) {
                m_owner->markFailure(QStringLiteral(
                    "Failed to initialize playback status overlay Vulkan resources."));
            }
            return;
        }
    }
    m_pipeline = std::make_unique<VulkanPipeline>();
    QString error;
    if (!m_pipeline->initialize(m_window->device(),
                                m_devFuncs,
                                m_window->defaultRenderPass(),
                                m_resources->descriptorSetLayout(),
                                &error)) {
        if (m_owner) {
            m_owner->markFailure(error.isEmpty()
                                     ? QStringLiteral("Failed to initialize direct presenter Vulkan pipeline.")
                                     : error);
        }
        return;
    }
    m_textRenderer = std::make_unique<VulkanTextRenderer>();
    if (!m_textRenderer->initialize(m_window->physicalDevice(),
                                    m_window->device(),
                                    m_devFuncs,
                                    m_window->defaultRenderPass(),
                                    &error)) {
        if (m_owner) {
            m_owner->markFailure(error.isEmpty()
                                     ? QStringLiteral("Failed to initialize Vulkan text renderer.")
                                     : error);
        }
        return;
    }
    m_speakerTextRenderer = std::make_unique<VulkanTextRenderer>();
    if (!m_speakerTextRenderer->initialize(m_window->physicalDevice(),
                                           m_window->device(),
                                           m_devFuncs,
                                           m_window->defaultRenderPass(),
                                           &error)) {
        if (m_owner) {
            m_owner->markFailure(error.isEmpty()
                                     ? QStringLiteral("Failed to initialize Vulkan speaker text renderer.")
                                     : error);
        }
        return;
    }
    m_temporalDebugTextRenderer = std::make_unique<VulkanTextRenderer>();
    if (!m_temporalDebugTextRenderer->initialize(m_window->physicalDevice(),
                                                m_window->device(),
                                                m_devFuncs,
                                                m_window->defaultRenderPass(),
                                                &error)) {
        if (m_owner) {
            m_owner->markFailure(error.isEmpty()
                                     ? QStringLiteral("Failed to initialize Vulkan temporal debug text renderer.")
                                     : error);
        }
        return;
    }
    if (m_window->audioPipelineEnabled()) {
        m_audioTab = std::make_unique<jcut::VulkanAudioTab>();
        if (!m_audioTab->initialize(m_window->physicalDevice(),
                                    m_window->device(),
                                    m_devFuncs,
                                    m_window->defaultRenderPass(),
                                    &error)) {
            if (m_owner) {
                m_owner->markFailure(error.isEmpty()
                                         ? QStringLiteral("Failed to initialize Vulkan audio waveform pipeline.")
                                         : error);
            }
            return;
        }
    }
}

DirectVulkanPreviewRenderer::~DirectVulkanPreviewRenderer()
{
    destroyCompositeTarget();
    destroyReadbackSlots();
}

void DirectVulkanPreviewRenderer::releaseResources()
{
    // Swapchain lifetime: only objects tied to its extent or render pass are
    // rebuilt. Device-level descriptors, imported images/semaphores, and
    // per-clip handoff allocations remain valid across ordinary resizes.
    destroyCompositeTarget();
    destroyReadbackSlots();
    m_audioTab.reset();
    m_temporalDebugTextRenderer.reset();
    m_speakerTextRenderer.reset();
    m_textRenderer.reset();
    m_pipeline.reset();
    m_lastPreparedTextKey.clear();
    m_lastPreparedTextReady = false;
}

void DirectVulkanPreviewRenderer::releaseDeviceResources()
{
    // The queue is idle when cleanupDevice reaches this method, so every
    // device-owned import and cached allocation can be retired exactly once.
    destroyGpuExportPreviewResources();
    for (auto it = m_clipHandoffResources.begin(); it != m_clipHandoffResources.end(); ++it) {
        releaseClipHandoffResources(it.value());
    }
    for (const RetiredClipHandoffResources& retired : m_retiredClipHandoffResources) {
        releaseClipHandoffResources(retired.resources);
    }
    m_clipHandoffResources.clear();
    m_retiredClipHandoffResources.clear();
    m_playbackStatusOverlayResources.reset();
    m_playbackStatusOverlayTextureKey.clear();
    m_playbackStatusOverlayTextureReady = false;
    m_resources.reset();
    m_importSemaphoreFd = nullptr;
    m_devFuncs = nullptr;
}

DirectVulkanPreviewRenderer::ClipHandoffResources*
DirectVulkanPreviewRenderer::ensureClipHandoffResources(const QString& clipId)
{
    if (clipId.trimmed().isEmpty() || !m_window || !m_devFuncs) {
        return nullptr;
    }
    auto existing = m_clipHandoffResources.find(clipId);
    if (existing != m_clipHandoffResources.end()) {
        return existing.value().get();
    }

    for (auto it = m_retiredClipHandoffResources.begin(); it != m_retiredClipHandoffResources.end(); ++it) {
        if (it->clipId != clipId || !it->resources) {
            continue;
        }
        std::shared_ptr<ClipHandoffResources> resources = it->resources;
        m_retiredClipHandoffResources.erase(it);
        m_clipHandoffResources.insert(clipId, resources);
        updateClipHandoffResourceStats();
        return resources.get();
    }

    auto resources = std::make_shared<ClipHandoffResources>();
    resources->resources = std::make_unique<VulkanResources>();
    if (!resources->resources->initialize(m_window->physicalDevice(), m_window->device(), m_devFuncs)) {
        if (m_owner) {
            m_owner->markFailure(QStringLiteral("Failed to initialize per-clip Vulkan handoff resources for %1.")
                                     .arg(clipId));
        }
        return nullptr;
    }
    resources->pipeline = std::make_unique<DirectVulkanFrameHandoffPipeline>();
    const jcut::vulkan_detector::VulkanDeviceContext handoffContext{
        m_window->physicalDevice(),
        m_window->device(),
        m_window->graphicsQueue(),
        m_window->graphicsQueueFamilyIndex()
    };
    QString handoffError;
    if (!resources->pipeline->initialize(handoffContext, &handoffError)) {
        qWarning().noquote()
            << QStringLiteral("[vulkan-preview] hardware frame handoff unavailable for clip %1: %2")
                   .arg(clipId, handoffError);
    }

    ClipHandoffResources* raw = resources.get();
    m_clipHandoffResources.insert(clipId, resources);
    updateClipHandoffResourceStats();
    return raw;
}

void DirectVulkanPreviewRenderer::pruneClipHandoffResources(const QSet<QString>& activeClipIds)
{
    for (auto it = m_clipHandoffResources.begin(); it != m_clipHandoffResources.end();) {
        if (activeClipIds.contains(it.key())) {
            ++it;
            continue;
        }
        if (it.value()) {
            m_retiredClipHandoffResources.push_back(RetiredClipHandoffResources{
                it.key(),
                it.value(),
                static_cast<int>(VulkanResources::kDescriptorSetCount) + 1});
        }
        it = m_clipHandoffResources.erase(it);
    }
    updateClipHandoffResourceStats();
}

void DirectVulkanPreviewRenderer::advanceRetiredClipHandoffResources()
{
    for (auto it = m_retiredClipHandoffResources.begin(); it != m_retiredClipHandoffResources.end();) {
        --it->framesRemaining;
        if (it->framesRemaining > 0) {
            ++it;
            continue;
        }
        releaseClipHandoffResources(it->resources);
        it = m_retiredClipHandoffResources.erase(it);
    }
    updateClipHandoffResourceStats();
}

void DirectVulkanPreviewRenderer::releaseClipHandoffResources(
    const std::shared_ptr<ClipHandoffResources>& resources)
{
    if (resources && resources->pipeline) {
        resources->pipeline->release();
    }
}

void DirectVulkanPreviewRenderer::updateClipHandoffResourceStats()
{
    if (DirectVulkanPreviewStats* stats = m_owner ? m_owner->stats() : nullptr) {
        stats->activeClipHandoffResourceCount = static_cast<int>(m_clipHandoffResources.size());
        stats->retiredClipHandoffResourceCount = static_cast<int>(m_retiredClipHandoffResources.size());
    }
}

uint32_t DirectVulkanPreviewRenderer::findMemoryType(uint32_t typeBits, VkMemoryPropertyFlags properties) const
{
    if (!m_window) {
        return UINT32_MAX;
    }
    auto getMemoryProperties = reinterpret_cast<PFN_vkGetPhysicalDeviceMemoryProperties>(
        m_window->vulkanInstance()
            ? m_window->vulkanInstance()->getInstanceProcAddr("vkGetPhysicalDeviceMemoryProperties")
            : nullptr);
    if (!getMemoryProperties) {
        return UINT32_MAX;
    }
    VkPhysicalDeviceMemoryProperties memoryProperties{};
    getMemoryProperties(m_window->physicalDevice(), &memoryProperties);
    for (uint32_t i = 0; i < memoryProperties.memoryTypeCount; ++i) {
        if ((typeBits & (1u << i)) &&
            (memoryProperties.memoryTypes[i].propertyFlags & properties) == properties) {
            return i;
        }
    }
    return UINT32_MAX;
}

void DirectVulkanPreviewRenderer::destroyCompositeTarget()
{
    if (!m_devFuncs || !m_window || !m_window->device()) {
        m_compositeSize = QSize();
        return;
    }
    const VkDevice device = m_window->device();
    if (m_compositeFramebuffer != VK_NULL_HANDLE) {
        m_devFuncs->vkDestroyFramebuffer(device, m_compositeFramebuffer, nullptr);
        m_compositeFramebuffer = VK_NULL_HANDLE;
    }
    if (m_compositeRenderPass != VK_NULL_HANDLE) {
        m_devFuncs->vkDestroyRenderPass(device, m_compositeRenderPass, nullptr);
        m_compositeRenderPass = VK_NULL_HANDLE;
    }
    if (m_compositeDepthView != VK_NULL_HANDLE) {
        m_devFuncs->vkDestroyImageView(device, m_compositeDepthView, nullptr);
        m_compositeDepthView = VK_NULL_HANDLE;
    }
    if (m_compositeDepthImage != VK_NULL_HANDLE) {
        m_devFuncs->vkDestroyImage(device, m_compositeDepthImage, nullptr);
        m_compositeDepthImage = VK_NULL_HANDLE;
    }
    if (m_compositeDepthMemory != VK_NULL_HANDLE) {
        m_devFuncs->vkFreeMemory(device, m_compositeDepthMemory, nullptr);
        m_compositeDepthMemory = VK_NULL_HANDLE;
    }
    if (m_compositeView != VK_NULL_HANDLE) {
        m_devFuncs->vkDestroyImageView(device, m_compositeView, nullptr);
        m_compositeView = VK_NULL_HANDLE;
    }
    if (m_compositeImage != VK_NULL_HANDLE) {
        m_devFuncs->vkDestroyImage(device, m_compositeImage, nullptr);
        m_compositeImage = VK_NULL_HANDLE;
    }
    if (m_compositeMemory != VK_NULL_HANDLE) {
        m_devFuncs->vkFreeMemory(device, m_compositeMemory, nullptr);
        m_compositeMemory = VK_NULL_HANDLE;
    }
    m_compositeSize = QSize();
    m_compositeColorFormat = VK_FORMAT_UNDEFINED;
    m_compositeDepthFormat = VK_FORMAT_UNDEFINED;
}

bool DirectVulkanPreviewRenderer::ensureCompositeTarget(const QSize& size,
                                                        VkFormat colorFormat,
                                                        VkFormat depthFormat)
{
    if (!m_window || !m_devFuncs || size.isEmpty() || colorFormat == VK_FORMAT_UNDEFINED) {
        return false;
    }
    if (m_compositeFramebuffer != VK_NULL_HANDLE &&
        m_compositeSize == size &&
        m_compositeColorFormat == colorFormat &&
        m_compositeDepthFormat == depthFormat) {
        return true;
    }
    destroyCompositeTarget();
    const VkDevice device = m_window->device();
    auto createImage = [&](VkFormat format,
                           VkImageUsageFlags usage,
                           VkImageAspectFlags aspect,
                           VkImage* image,
                           VkDeviceMemory* memory,
                           VkImageView* view) -> bool {
        VkImageCreateInfo imageInfo{};
        imageInfo.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
        imageInfo.imageType = VK_IMAGE_TYPE_2D;
        imageInfo.extent = {static_cast<uint32_t>(std::max(1, size.width())),
                            static_cast<uint32_t>(std::max(1, size.height())),
                            1};
        imageInfo.mipLevels = 1;
        imageInfo.arrayLayers = 1;
        imageInfo.format = format;
        imageInfo.tiling = VK_IMAGE_TILING_OPTIMAL;
        imageInfo.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        imageInfo.usage = usage;
        imageInfo.samples = VK_SAMPLE_COUNT_1_BIT;
        imageInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
        if (m_devFuncs->vkCreateImage(device, &imageInfo, nullptr, image) != VK_SUCCESS) {
            return false;
        }
        VkMemoryRequirements requirements{};
        m_devFuncs->vkGetImageMemoryRequirements(device, *image, &requirements);
        const uint32_t memoryType = findMemoryType(requirements.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
        if (memoryType == UINT32_MAX) {
            return false;
        }
        VkMemoryAllocateInfo alloc{};
        alloc.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
        alloc.allocationSize = requirements.size;
        alloc.memoryTypeIndex = memoryType;
        if (m_devFuncs->vkAllocateMemory(device, &alloc, nullptr, memory) != VK_SUCCESS ||
            m_devFuncs->vkBindImageMemory(device, *image, *memory, 0) != VK_SUCCESS) {
            return false;
        }
        VkImageViewCreateInfo viewInfo{};
        viewInfo.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
        viewInfo.image = *image;
        viewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
        viewInfo.format = format;
        viewInfo.subresourceRange.aspectMask = aspect;
        viewInfo.subresourceRange.baseMipLevel = 0;
        viewInfo.subresourceRange.levelCount = 1;
        viewInfo.subresourceRange.baseArrayLayer = 0;
        viewInfo.subresourceRange.layerCount = 1;
        return m_devFuncs->vkCreateImageView(device, &viewInfo, nullptr, view) == VK_SUCCESS;
    };
    if (!createImage(colorFormat,
                     VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
                     VK_IMAGE_ASPECT_COLOR_BIT,
                     &m_compositeImage,
                     &m_compositeMemory,
                     &m_compositeView)) {
        destroyCompositeTarget();
        return false;
    }
    const bool hasDepth = depthFormat != VK_FORMAT_UNDEFINED;
    if (hasDepth &&
        !createImage(depthFormat,
                     VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT,
                     VK_IMAGE_ASPECT_DEPTH_BIT,
                     &m_compositeDepthImage,
                     &m_compositeDepthMemory,
                     &m_compositeDepthView)) {
        destroyCompositeTarget();
        return false;
    }

    VkAttachmentDescription attachments[2]{};
    attachments[0].format = colorFormat;
    attachments[0].samples = VK_SAMPLE_COUNT_1_BIT;
    attachments[0].loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
    attachments[0].storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    attachments[0].stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    attachments[0].stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    attachments[0].initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    attachments[0].finalLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    VkAttachmentReference colorRef{};
    colorRef.attachment = 0;
    colorRef.layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    VkAttachmentReference depthRef{};
    if (hasDepth) {
        attachments[1].format = depthFormat;
        attachments[1].samples = VK_SAMPLE_COUNT_1_BIT;
        attachments[1].loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
        attachments[1].storeOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
        attachments[1].stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
        attachments[1].stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
        attachments[1].initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        attachments[1].finalLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
        depthRef.attachment = 1;
        depthRef.layout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
    }
    VkSubpassDescription subpass{};
    subpass.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
    subpass.colorAttachmentCount = 1;
    subpass.pColorAttachments = &colorRef;
    subpass.pDepthStencilAttachment = hasDepth ? &depthRef : nullptr;
    VkRenderPassCreateInfo renderPassInfo{};
    renderPassInfo.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO;
    renderPassInfo.attachmentCount = hasDepth ? 2u : 1u;
    renderPassInfo.pAttachments = attachments;
    renderPassInfo.subpassCount = 1;
    renderPassInfo.pSubpasses = &subpass;
    if (m_devFuncs->vkCreateRenderPass(device, &renderPassInfo, nullptr, &m_compositeRenderPass) != VK_SUCCESS) {
        destroyCompositeTarget();
        return false;
    }
    VkImageView views[2] = {m_compositeView, m_compositeDepthView};
    VkFramebufferCreateInfo framebufferInfo{};
    framebufferInfo.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
    framebufferInfo.renderPass = m_compositeRenderPass;
    framebufferInfo.attachmentCount = hasDepth ? 2u : 1u;
    framebufferInfo.pAttachments = views;
    framebufferInfo.width = static_cast<uint32_t>(std::max(1, size.width()));
    framebufferInfo.height = static_cast<uint32_t>(std::max(1, size.height()));
    framebufferInfo.layers = 1;
    if (m_devFuncs->vkCreateFramebuffer(device, &framebufferInfo, nullptr, &m_compositeFramebuffer) != VK_SUCCESS) {
        destroyCompositeTarget();
        return false;
    }
    m_compositeSize = size;
    m_compositeColorFormat = colorFormat;
    m_compositeDepthFormat = depthFormat;
    return true;
}

void DirectVulkanPreviewRenderer::destroyReadbackSlots()
{
    if (!m_window || !m_devFuncs) {
        m_readbackSlots.clear();
        return;
    }
    const VkDevice device = m_window->device();
    for (ReadbackSlot& slot : m_readbackSlots) {
        if (slot.buffer != VK_NULL_HANDLE) {
            m_devFuncs->vkDestroyBuffer(device, slot.buffer, nullptr);
        }
        if (slot.memory != VK_NULL_HANDLE) {
            m_devFuncs->vkFreeMemory(device, slot.memory, nullptr);
        }
    }
    m_readbackSlots.clear();
    for (ReadbackSlot& slot : m_decoderReadbackSlots) {
        if (slot.buffer != VK_NULL_HANDLE) {
            m_devFuncs->vkDestroyBuffer(device, slot.buffer, nullptr);
        }
        if (slot.memory != VK_NULL_HANDLE) {
            m_devFuncs->vkFreeMemory(device, slot.memory, nullptr);
        }
    }
    m_decoderReadbackSlots.clear();
}

bool DirectVulkanPreviewRenderer::ensureReadbackSlot(ReadbackSlot* slot, const QSize& size, VkFormat format)
{
    if (!slot || !m_window || !m_devFuncs || size.isEmpty()) {
        return false;
    }
    const VkDeviceSize bytes = static_cast<VkDeviceSize>(std::max(1, size.width())) *
                               static_cast<VkDeviceSize>(std::max(1, size.height())) * 4u;
    if (slot->buffer != VK_NULL_HANDLE && slot->size >= bytes && slot->imageSize == size && slot->format == format) {
        return true;
    }
    if (slot->buffer != VK_NULL_HANDLE) {
        m_devFuncs->vkDestroyBuffer(m_window->device(), slot->buffer, nullptr);
        slot->buffer = VK_NULL_HANDLE;
    }
    if (slot->memory != VK_NULL_HANDLE) {
        m_devFuncs->vkFreeMemory(m_window->device(), slot->memory, nullptr);
        slot->memory = VK_NULL_HANDLE;
    }

    VkBufferCreateInfo bufferInfo{};
    bufferInfo.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    bufferInfo.size = bytes;
    bufferInfo.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT;
    bufferInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    if (m_devFuncs->vkCreateBuffer(m_window->device(), &bufferInfo, nullptr, &slot->buffer) != VK_SUCCESS) {
        return false;
    }

    VkMemoryRequirements req{};
    m_devFuncs->vkGetBufferMemoryRequirements(m_window->device(), slot->buffer, &req);
    const uint32_t memoryType = findMemoryType(
        req.memoryTypeBits,
        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    if (memoryType == UINT32_MAX) {
        m_devFuncs->vkDestroyBuffer(m_window->device(), slot->buffer, nullptr);
        slot->buffer = VK_NULL_HANDLE;
        return false;
    }

    VkMemoryAllocateInfo alloc{};
    alloc.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    alloc.allocationSize = req.size;
    alloc.memoryTypeIndex = memoryType;
    if (m_devFuncs->vkAllocateMemory(m_window->device(), &alloc, nullptr, &slot->memory) != VK_SUCCESS ||
        m_devFuncs->vkBindBufferMemory(m_window->device(), slot->buffer, slot->memory, 0) != VK_SUCCESS) {
        if (slot->memory != VK_NULL_HANDLE) {
            m_devFuncs->vkFreeMemory(m_window->device(), slot->memory, nullptr);
            slot->memory = VK_NULL_HANDLE;
        }
        if (slot->buffer != VK_NULL_HANDLE) {
            m_devFuncs->vkDestroyBuffer(m_window->device(), slot->buffer, nullptr);
            slot->buffer = VK_NULL_HANDLE;
        }
        return false;
    }

    slot->size = bytes;
    slot->imageSize = size;
    slot->format = format;
    slot->pending = false;
    return true;
}

QImage DirectVulkanPreviewRenderer::imageFromReadback(const uchar* bytes, const QSize& size, VkFormat format) const
{
    if (!bytes || size.isEmpty()) {
        return QImage();
    }
    QImage image(size, QImage::Format_RGBA8888);
    if (image.isNull()) {
        return QImage();
    }
    const int pixelCount = size.width() * size.height();
    uchar* out = image.bits();
    if (format == VK_FORMAT_B8G8R8A8_UNORM || format == VK_FORMAT_B8G8R8A8_SRGB) {
        for (int i = 0; i < pixelCount; ++i) {
            out[i * 4 + 0] = bytes[i * 4 + 2];
            out[i * 4 + 1] = bytes[i * 4 + 1];
            out[i * 4 + 2] = bytes[i * 4 + 0];
            out[i * 4 + 3] = bytes[i * 4 + 3];
        }
    } else if (format == VK_FORMAT_R8G8B8A8_UNORM || format == VK_FORMAT_R8G8B8A8_SRGB) {
        std::memcpy(out, bytes, static_cast<size_t>(pixelCount) * 4u);
    } else if (format == VK_FORMAT_A2B10G10R10_UNORM_PACK32 ||
               format == VK_FORMAT_A2R10G10B10_UNORM_PACK32) {
        const auto* words = reinterpret_cast<const uint32_t*>(bytes);
        for (int i = 0; i < pixelCount; ++i) {
            const uint32_t v = words[i];
            const uint32_t c0 = (v >> 0) & 0x3ffu;
            const uint32_t c1 = (v >> 10) & 0x3ffu;
            const uint32_t c2 = (v >> 20) & 0x3ffu;
            const uint32_t a = (v >> 30) & 0x3u;
            if (format == VK_FORMAT_A2B10G10R10_UNORM_PACK32) {
                out[i * 4 + 0] = static_cast<uchar>((c2 * 255u) / 1023u);
                out[i * 4 + 1] = static_cast<uchar>((c1 * 255u) / 1023u);
                out[i * 4 + 2] = static_cast<uchar>((c0 * 255u) / 1023u);
            } else {
                out[i * 4 + 0] = static_cast<uchar>((c0 * 255u) / 1023u);
                out[i * 4 + 1] = static_cast<uchar>((c1 * 255u) / 1023u);
                out[i * 4 + 2] = static_cast<uchar>((c2 * 255u) / 1023u);
            }
            out[i * 4 + 3] = static_cast<uchar>((a * 255u) / 3u);
        }
    } else {
        std::memcpy(out, bytes, static_cast<size_t>(pixelCount) * 4u);
    }
    return image;
}

void DirectVulkanPreviewRenderer::consumeReadbackSlot(ReadbackSlot* slot)
{
    if (!slot || !slot->pending || slot->memory == VK_NULL_HANDLE || !m_window || !m_devFuncs || !m_owner) {
        return;
    }
    void* mapped = nullptr;
    if (m_devFuncs->vkMapMemory(m_window->device(), slot->memory, 0, slot->size, 0, &mapped) != VK_SUCCESS || !mapped) {
        return;
    }
    const QImage image = imageFromReadback(static_cast<const uchar*>(mapped), slot->imageSize, slot->format);
    m_devFuncs->vkUnmapMemory(m_window->device(), slot->memory);
    if (!image.isNull()) {
        m_owner->setLatestVulkanReadbackImage(image);
    }
    slot->pending = false;
}

void DirectVulkanPreviewRenderer::consumeDecoderReadbackSlot(ReadbackSlot* slot)
{
    if (!slot || !slot->pending || slot->memory == VK_NULL_HANDLE || !m_window || !m_devFuncs || !m_owner) {
        return;
    }
    void* mapped = nullptr;
    if (m_devFuncs->vkMapMemory(m_window->device(), slot->memory, 0, slot->size, 0, &mapped) != VK_SUCCESS || !mapped) {
        return;
    }
    const QImage image = imageFromReadback(static_cast<const uchar*>(mapped), slot->imageSize, slot->format);
    m_devFuncs->vkUnmapMemory(m_window->device(), slot->memory);
    if (!image.isNull()) {
        m_owner->setLatestDecoderDiagnosticImage(image);
        if (DirectVulkanPreviewStats* stats = m_owner->stats()) {
            ++stats->decoderDiagnosticReadbackCopies;
            stats->lastDecoderDiagnosticReadbackSize = image.size();
            stats->lastDecoderDiagnosticReadbackFormat = vulkanFormatName(slot->format);
        }
    }
    slot->pending = false;
}

void DirectVulkanPreviewRenderer::recordSwapchainReadback(VkCommandBuffer cb, ReadbackSlot* slot, const QSize& swapSize)
{
    if (!slot || slot->buffer == VK_NULL_HANDLE || !m_window || !m_devFuncs || swapSize.isEmpty()) {
        return;
    }
    const int imageIndex = m_window->currentSwapChainImageIndex();
    if (imageIndex < 0) {
        return;
    }
    const VkImage image = m_window->swapChainImage(imageIndex);
    if (image == VK_NULL_HANDLE) {
        return;
    }

    VkImageMemoryBarrier toTransfer{};
    toTransfer.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    toTransfer.srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
    toTransfer.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
    toTransfer.oldLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
    toTransfer.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    toTransfer.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    toTransfer.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    toTransfer.image = image;
    toTransfer.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    toTransfer.subresourceRange.baseMipLevel = 0;
    toTransfer.subresourceRange.levelCount = 1;
    toTransfer.subresourceRange.baseArrayLayer = 0;
    toTransfer.subresourceRange.layerCount = 1;
    m_devFuncs->vkCmdPipelineBarrier(cb,
                                     VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
                                     VK_PIPELINE_STAGE_TRANSFER_BIT,
                                     0,
                                     0,
                                     nullptr,
                                     0,
                                     nullptr,
                                     1,
                                     &toTransfer);

    VkBufferImageCopy region{};
    region.bufferOffset = 0;
    region.bufferRowLength = 0;
    region.bufferImageHeight = 0;
    region.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    region.imageSubresource.mipLevel = 0;
    region.imageSubresource.baseArrayLayer = 0;
    region.imageSubresource.layerCount = 1;
    region.imageOffset = {0, 0, 0};
    region.imageExtent = {
        static_cast<uint32_t>(std::max(1, swapSize.width())),
        static_cast<uint32_t>(std::max(1, swapSize.height())),
        1u
    };
    m_devFuncs->vkCmdCopyImageToBuffer(cb,
                                       image,
                                       VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                                       slot->buffer,
                                       1,
                                       &region);

    VkImageMemoryBarrier toPresent = toTransfer;
    toPresent.srcAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
    toPresent.dstAccessMask = VK_ACCESS_MEMORY_READ_BIT;
    toPresent.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    toPresent.newLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
    m_devFuncs->vkCmdPipelineBarrier(cb,
                                     VK_PIPELINE_STAGE_TRANSFER_BIT,
                                     VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT,
                                     0,
                                     0,
                                     nullptr,
                                     0,
                                     nullptr,
                                     1,
                                     &toPresent);
    slot->pending = true;
}

void DirectVulkanPreviewRenderer::recordImageReadback(VkCommandBuffer cb,
                                                      ReadbackSlot* slot,
                                                      VkImage image,
                                                      VkImageLayout layout,
                                                      const QSize& size,
                                                      VkFormat format)
{
    if (!slot || slot->buffer == VK_NULL_HANDLE || image == VK_NULL_HANDLE ||
        !m_window || !m_devFuncs || size.isEmpty() || layout == VK_IMAGE_LAYOUT_UNDEFINED) {
        return;
    }

    VkImageMemoryBarrier toTransfer{};
    toTransfer.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    if (layout == VK_IMAGE_LAYOUT_GENERAL) {
        toTransfer.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
    } else if (layout == VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL) {
        toTransfer.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    } else {
        toTransfer.srcAccessMask = VK_ACCESS_SHADER_READ_BIT;
    }
    toTransfer.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
    toTransfer.oldLayout = layout;
    toTransfer.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    toTransfer.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    toTransfer.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    toTransfer.image = image;
    toTransfer.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    toTransfer.subresourceRange.baseMipLevel = 0;
    toTransfer.subresourceRange.levelCount = 1;
    toTransfer.subresourceRange.baseArrayLayer = 0;
    toTransfer.subresourceRange.layerCount = 1;
    m_devFuncs->vkCmdPipelineBarrier(cb,
                                     VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
                                     VK_PIPELINE_STAGE_TRANSFER_BIT,
                                     0,
                                     0,
                                     nullptr,
                                     0,
                                     nullptr,
                                     1,
                                     &toTransfer);

    VkBufferImageCopy region{};
    region.bufferOffset = 0;
    region.bufferRowLength = 0;
    region.bufferImageHeight = 0;
    region.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    region.imageSubresource.mipLevel = 0;
    region.imageSubresource.baseArrayLayer = 0;
    region.imageSubresource.layerCount = 1;
    region.imageOffset = {0, 0, 0};
    region.imageExtent = {
        static_cast<uint32_t>(std::max(1, size.width())),
        static_cast<uint32_t>(std::max(1, size.height())),
        1u
    };
    m_devFuncs->vkCmdCopyImageToBuffer(cb,
                                       image,
                                       VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                                       slot->buffer,
                                       1,
                                       &region);

    VkImageMemoryBarrier toOriginal = toTransfer;
    toOriginal.srcAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
    toOriginal.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
    toOriginal.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    toOriginal.newLayout = layout;
    m_devFuncs->vkCmdPipelineBarrier(cb,
                                     VK_PIPELINE_STAGE_TRANSFER_BIT,
                                     VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
                                     0,
                                     0,
                                     nullptr,
                                     0,
                                     nullptr,
                                     1,
                                     &toOriginal);
    slot->format = format;
    slot->pending = true;
}

void DirectVulkanPreviewRenderer::physicalDeviceLost()
{
    if (m_owner) {
        m_owner->markFailure(QStringLiteral("Physical Vulkan device lost during direct preview presentation."));
    }
}

void DirectVulkanPreviewRenderer::logicalDeviceLost()
{
    if (m_owner) {
        m_owner->markFailure(QStringLiteral("Logical Vulkan device lost during direct preview presentation."));
    }
}
