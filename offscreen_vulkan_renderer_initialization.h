// Initialization and resource lifecycle methods.
// Included inside OffscreenVulkanRendererPrivate; do not compile separately.
  bool initialize(const QSize &outputSize, QString *errorMessage) {
    release();
    m_producerSessionId =
        g_nextOffscreenProducerSessionId.fetch_add(
            1, std::memory_order_relaxed);

    m_outputSize =
        QSize(qMax(16, outputSize.width()), qMax(16, outputSize.height()));
    m_cudaExternalMemoryStatus = QStringLiteral("initializing");

    VkApplicationInfo appInfo{};
    appInfo.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
    appInfo.pApplicationName = "JCut Offscreen Vulkan Renderer";
    appInfo.applicationVersion = VK_MAKE_VERSION(1, 0, 0);
    appInfo.pEngineName = "JCut";
    appInfo.engineVersion = VK_MAKE_VERSION(1, 0, 0);
    appInfo.apiVersion = VK_API_VERSION_1_1;

    VkInstanceCreateInfo instanceInfo{};
    instanceInfo.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
    instanceInfo.pApplicationInfo = &appInfo;

    if (vkCreateInstance(&instanceInfo, nullptr, &m_instance) != VK_SUCCESS) {
      if (errorMessage) {
        *errorMessage = QStringLiteral("Failed to create Vulkan instance.");
      }
      return false;
    }

    uint32_t physicalDeviceCount = 0;
    vkEnumeratePhysicalDevices(m_instance, &physicalDeviceCount, nullptr);
    if (physicalDeviceCount == 0) {
      if (errorMessage) {
        *errorMessage = QStringLiteral("No Vulkan physical devices found.");
      }
      return false;
    }

    QVector<VkPhysicalDevice> devices(static_cast<int>(physicalDeviceCount));
    vkEnumeratePhysicalDevices(m_instance, &physicalDeviceCount,
                               devices.data());

    int bestScore = -1;
    VkPhysicalDevice bestDevice = VK_NULL_HANDLE;
    uint32_t bestQueueFamily = UINT32_MAX;
    VkPhysicalDeviceProperties bestProperties{};
    for (VkPhysicalDevice candidate : devices) {
      VkPhysicalDeviceProperties properties{};
      vkGetPhysicalDeviceProperties(candidate, &properties);
      uint32_t queueFamilyCount = 0;
      vkGetPhysicalDeviceQueueFamilyProperties(candidate, &queueFamilyCount,
                                               nullptr);
      QVector<VkQueueFamilyProperties> queueFamilies(
          static_cast<int>(queueFamilyCount));
      vkGetPhysicalDeviceQueueFamilyProperties(candidate, &queueFamilyCount,
                                               queueFamilies.data());
      for (uint32_t i = 0; i < queueFamilyCount; ++i) {
        if (queueFamilies[static_cast<int>(i)].queueFlags &
            VK_QUEUE_GRAPHICS_BIT) {
          int score = 0;
          if (properties.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU) {
            score += 1000;
          } else if (properties.deviceType ==
                     VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU) {
            score += 500;
          } else if (properties.deviceType == VK_PHYSICAL_DEVICE_TYPE_CPU) {
            score -= 1000;
          }
          if (properties.vendorID == 0x10de) {
            score += 100;
          }
          if (score > bestScore) {
            bestScore = score;
            bestDevice = candidate;
            bestQueueFamily = i;
            bestProperties = properties;
          }
        }
      }
    }
    m_physicalDevice = bestDevice;
    m_graphicsQueueFamily = bestQueueFamily;
    m_nonCoherentAtomSize =
        qMax<VkDeviceSize>(1, bestProperties.limits.nonCoherentAtomSize);
    m_storageBufferOffsetAlignment = qMax<VkDeviceSize>(
        16, bestProperties.limits.minStorageBufferOffsetAlignment);

    if (m_physicalDevice == VK_NULL_HANDLE) {
      if (errorMessage) {
        *errorMessage =
            QStringLiteral("No Vulkan graphics queue family found.");
      }
      return false;
    }
    qInfo().noquote() << QStringLiteral(
                             "Offscreen Vulkan device: %1 vendor=0x%2 type=%3")
                             .arg(QString::fromUtf8(bestProperties.deviceName))
                             .arg(bestProperties.vendorID, 0, 16)
                             .arg(static_cast<int>(bestProperties.deviceType));
    m_externalMemoryFdSupported =
        physicalDeviceSupportsExtension(
            m_physicalDevice, VK_KHR_EXTERNAL_MEMORY_EXTENSION_NAME) &&
        physicalDeviceSupportsExtension(
            m_physicalDevice, VK_KHR_EXTERNAL_MEMORY_FD_EXTENSION_NAME);
    m_externalSemaphoreFdSupported =
        physicalDeviceSupportsExtension(
            m_physicalDevice, VK_KHR_EXTERNAL_SEMAPHORE_EXTENSION_NAME) &&
        physicalDeviceSupportsExtension(
            m_physicalDevice, VK_KHR_EXTERNAL_SEMAPHORE_FD_EXTENSION_NAME);

    const float queuePriority = 1.0f;
    VkDeviceQueueCreateInfo queueInfo{};
    queueInfo.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
    queueInfo.queueFamilyIndex = m_graphicsQueueFamily;
    queueInfo.queueCount = 1;
    queueInfo.pQueuePriorities = &queuePriority;

    VkDeviceCreateInfo deviceInfo{};
    deviceInfo.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
    deviceInfo.queueCreateInfoCount = 1;
    deviceInfo.pQueueCreateInfos = &queueInfo;
    QVector<const char *> enabledDeviceExtensions;
    if (m_externalMemoryFdSupported) {
      enabledDeviceExtensions.push_back(VK_KHR_EXTERNAL_MEMORY_EXTENSION_NAME);
      enabledDeviceExtensions.push_back(
          VK_KHR_EXTERNAL_MEMORY_FD_EXTENSION_NAME);
    }
    if (m_externalSemaphoreFdSupported) {
      enabledDeviceExtensions.push_back(
          VK_KHR_EXTERNAL_SEMAPHORE_EXTENSION_NAME);
      enabledDeviceExtensions.push_back(
          VK_KHR_EXTERNAL_SEMAPHORE_FD_EXTENSION_NAME);
    }
    deviceInfo.enabledExtensionCount =
        static_cast<uint32_t>(enabledDeviceExtensions.size());
    deviceInfo.ppEnabledExtensionNames =
        enabledDeviceExtensions.isEmpty() ? nullptr
                                          : enabledDeviceExtensions.constData();

    if (vkCreateDevice(m_physicalDevice, &deviceInfo, nullptr, &m_device) !=
        VK_SUCCESS) {
      if (errorMessage) {
        *errorMessage =
            QStringLiteral("Failed to create Vulkan logical device.");
      }
      return false;
    }

    vkGetDeviceQueue(m_device, m_graphicsQueueFamily, 0, &m_graphicsQueue);
    uint32_t selectedQueueFamilyCount = 0;
    vkGetPhysicalDeviceQueueFamilyProperties(
        m_physicalDevice, &selectedQueueFamilyCount, nullptr);
    QVector<VkQueueFamilyProperties> selectedFamilies(
        static_cast<int>(selectedQueueFamilyCount));
    vkGetPhysicalDeviceQueueFamilyProperties(
        m_physicalDevice, &selectedQueueFamilyCount, selectedFamilies.data());
    m_graphicsQueueSupportsCompute =
        m_graphicsQueueFamily <
            static_cast<uint32_t>(selectedFamilies.size()) &&
        (selectedFamilies[static_cast<int>(m_graphicsQueueFamily)].queueFlags &
         VK_QUEUE_COMPUTE_BIT);
    m_vkGetMemoryFdKHR = reinterpret_cast<PFN_vkGetMemoryFdKHR>(
        vkGetDeviceProcAddr(m_device, "vkGetMemoryFdKHR"));
    m_vkGetSemaphoreFdKHR = reinterpret_cast<PFN_vkGetSemaphoreFdKHR>(
        vkGetDeviceProcAddr(m_device, "vkGetSemaphoreFdKHR"));
    qInfo().noquote()
        << QStringLiteral(
               "Vulkan/CUDA interop capability: external_memory_fd=%1 "
               "external_semaphore_fd=%2 get_memory_fd=%3 get_semaphore_fd=%4")
               .arg(m_externalMemoryFdSupported ? QStringLiteral("yes")
                                                : QStringLiteral("no"),
                    m_externalSemaphoreFdSupported ? QStringLiteral("yes")
                                                   : QStringLiteral("no"),
                    m_vkGetMemoryFdKHR ? QStringLiteral("yes")
                                       : QStringLiteral("no"),
                    m_vkGetSemaphoreFdKHR ? QStringLiteral("yes")
                                          : QStringLiteral("no"));

    VkCommandPoolCreateInfo poolInfo{};
    poolInfo.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
    poolInfo.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    poolInfo.queueFamilyIndex = m_graphicsQueueFamily;

    if (vkCreateCommandPool(m_device, &poolInfo, nullptr, &m_commandPool) !=
        VK_SUCCESS) {
      if (errorMessage) {
        *errorMessage = QStringLiteral("Failed to create Vulkan command pool.");
      }
      return false;
    }

    m_frameSlots.resize(kFrameSlots);
    QVector<VkCommandBuffer> commandBuffers(kFrameSlots);
    VkCommandBufferAllocateInfo allocInfo{};
    allocInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    allocInfo.commandPool = m_commandPool;
    allocInfo.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    allocInfo.commandBufferCount = kFrameSlots;

    if (vkAllocateCommandBuffers(m_device, &allocInfo, commandBuffers.data()) !=
        VK_SUCCESS) {
      if (errorMessage) {
        *errorMessage =
            QStringLiteral("Failed to allocate Vulkan command buffers.");
      }
      return false;
    }
    for (int i = 0; i < kFrameSlots; ++i) {
      m_frameSlots[i].commandBuffer = commandBuffers[i];
    }

    VkImageCreateInfo imageInfo{};
    imageInfo.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    imageInfo.imageType = VK_IMAGE_TYPE_2D;
    imageInfo.extent.width = static_cast<uint32_t>(m_outputSize.width());
    imageInfo.extent.height = static_cast<uint32_t>(m_outputSize.height());
    imageInfo.extent.depth = 1;
    imageInfo.mipLevels = 1;
    imageInfo.arrayLayers = 1;
    imageInfo.format = VK_FORMAT_B8G8R8A8_UNORM;
    imageInfo.tiling = VK_IMAGE_TILING_OPTIMAL;
    imageInfo.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    imageInfo.usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT |
                      VK_IMAGE_USAGE_TRANSFER_DST_BIT |
                      VK_IMAGE_USAGE_TRANSFER_SRC_BIT |
                      VK_IMAGE_USAGE_SAMPLED_BIT;
    imageInfo.samples = VK_SAMPLE_COUNT_1_BIT;
    imageInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    VkExternalMemoryImageCreateInfo externalImageInfo{};
    if (m_externalMemoryFdSupported && m_vkGetMemoryFdKHR) {
      externalImageInfo.sType =
          VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_IMAGE_CREATE_INFO;
      externalImageInfo.handleTypes =
          VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_FD_BIT;
      imageInfo.pNext = &externalImageInfo;
    }

    if (vkCreateImage(m_device, &imageInfo, nullptr, &m_colorImage) !=
        VK_SUCCESS) {
      if (errorMessage) {
        *errorMessage =
            QStringLiteral("Failed to create Vulkan offscreen image.");
      }
      return false;
    }

    VkMemoryRequirements memRequirements{};
    vkGetImageMemoryRequirements(m_device, m_colorImage, &memRequirements);

    const uint32_t memoryTypeIndex =
        findMemoryType(m_physicalDevice, memRequirements.memoryTypeBits,
                       VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    if (memoryTypeIndex == UINT32_MAX) {
      if (errorMessage) {
        *errorMessage = QStringLiteral(
            "No suitable Vulkan memory type for offscreen image.");
      }
      return false;
    }

    VkMemoryAllocateInfo allocMemoryInfo{};
    allocMemoryInfo.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    allocMemoryInfo.allocationSize = memRequirements.size;
    allocMemoryInfo.memoryTypeIndex = memoryTypeIndex;
    VkExportMemoryAllocateInfo exportAllocInfo{};
    if (m_externalMemoryFdSupported && m_vkGetMemoryFdKHR) {
      exportAllocInfo.sType = VK_STRUCTURE_TYPE_EXPORT_MEMORY_ALLOCATE_INFO;
      exportAllocInfo.handleTypes =
          VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_FD_BIT;
      allocMemoryInfo.pNext = &exportAllocInfo;
    }

    if (vkAllocateMemory(m_device, &allocMemoryInfo, nullptr,
                         &m_colorImageMemory) != VK_SUCCESS) {
      if (errorMessage) {
        *errorMessage =
            QStringLiteral("Failed to allocate Vulkan image memory.");
      }
      return false;
    }

    if (vkBindImageMemory(m_device, m_colorImage, m_colorImageMemory, 0) !=
        VK_SUCCESS) {
      if (errorMessage) {
        *errorMessage = QStringLiteral("Failed to bind Vulkan image memory.");
      }
      return false;
    }

    VkImageViewCreateInfo viewInfo{};
    viewInfo.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    viewInfo.image = m_colorImage;
    viewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
    viewInfo.format = VK_FORMAT_B8G8R8A8_UNORM;
    viewInfo.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    viewInfo.subresourceRange.baseMipLevel = 0;
    viewInfo.subresourceRange.levelCount = 1;
    viewInfo.subresourceRange.baseArrayLayer = 0;
    viewInfo.subresourceRange.layerCount = 1;

    if (vkCreateImageView(m_device, &viewInfo, nullptr, &m_colorImageView) !=
        VK_SUCCESS) {
      if (errorMessage) {
        *errorMessage = QStringLiteral("Failed to create Vulkan image view.");
      }
      return false;
    }

    if (m_externalMemoryFdSupported &&
        m_externalSemaphoreFdSupported &&
        m_vkGetMemoryFdKHR &&
        m_vkGetSemaphoreFdKHR) {
      m_previewSlots.resize(3);
      bool previewSlotsReady = true;
      for (PreviewSlot &slot : m_previewSlots) {
        VkImageCreateInfo previewImageInfo = imageInfo;
        previewImageInfo.pNext = &externalImageInfo;
        if (vkCreateImage(m_device, &previewImageInfo, nullptr, &slot.image) !=
            VK_SUCCESS) {
          previewSlotsReady = false;
          break;
        }
        VkMemoryRequirements previewRequirements{};
        vkGetImageMemoryRequirements(m_device, slot.image,
                                     &previewRequirements);
        slot.memoryAllocationSize = previewRequirements.size;
        const uint32_t previewMemoryType =
            findMemoryType(m_physicalDevice,
                           previewRequirements.memoryTypeBits,
                           VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
        slot.memoryTypeIndex = previewMemoryType;
        VkExportMemoryAllocateInfo previewExport{};
        previewExport.sType = VK_STRUCTURE_TYPE_EXPORT_MEMORY_ALLOCATE_INFO;
        previewExport.handleTypes =
            VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_FD_BIT;
        VkMemoryDedicatedAllocateInfo previewDedicated{};
        previewDedicated.sType =
            VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO;
        previewDedicated.image = slot.image;
        previewExport.pNext = &previewDedicated;
        VkMemoryAllocateInfo previewAllocation{};
        previewAllocation.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
        previewAllocation.pNext = &previewExport;
        previewAllocation.allocationSize = previewRequirements.size;
        previewAllocation.memoryTypeIndex = previewMemoryType;
        if (previewMemoryType == UINT32_MAX ||
            vkAllocateMemory(m_device, &previewAllocation, nullptr,
                             &slot.memory) != VK_SUCCESS ||
            vkBindImageMemory(m_device, slot.image, slot.memory, 0) !=
                VK_SUCCESS) {
          previewSlotsReady = false;
          break;
        }
        VkImageViewCreateInfo previewViewInfo = viewInfo;
        previewViewInfo.image = slot.image;
        if (vkCreateImageView(m_device, &previewViewInfo, nullptr,
                              &slot.view) != VK_SUCCESS) {
          previewSlotsReady = false;
          break;
        }
        VkExportSemaphoreCreateInfo semaphoreExport{};
        semaphoreExport.sType =
            VK_STRUCTURE_TYPE_EXPORT_SEMAPHORE_CREATE_INFO;
        semaphoreExport.handleTypes =
            VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_OPAQUE_FD_BIT;
        VkSemaphoreCreateInfo semaphoreInfo{};
        semaphoreInfo.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;
        semaphoreInfo.pNext = &semaphoreExport;
        if (vkCreateSemaphore(m_device, &semaphoreInfo, nullptr,
                              &slot.readySemaphore) != VK_SUCCESS ||
            vkCreateSemaphore(m_device, &semaphoreInfo, nullptr,
                              &slot.consumedSemaphore) != VK_SUCCESS) {
          previewSlotsReady = false;
          break;
        }
      }
      if (!previewSlotsReady) {
        qWarning() << "GPU export preview double buffers are unavailable.";
        destroyPreviewSlots();
      }
    }

    VkSamplerCreateInfo samplerInfo{};
    samplerInfo.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
    samplerInfo.magFilter = VK_FILTER_LINEAR;
    samplerInfo.minFilter = VK_FILTER_LINEAR;
    samplerInfo.mipmapMode = VK_SAMPLER_MIPMAP_MODE_LINEAR;
    samplerInfo.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    samplerInfo.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    samplerInfo.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    samplerInfo.maxAnisotropy = 1.0f;

    if (vkCreateSampler(m_device, &samplerInfo, nullptr, &m_sampler) !=
        VK_SUCCESS) {
      if (errorMessage) {
        *errorMessage = QStringLiteral("Failed to create Vulkan sampler.");
      }
      return false;
    }

    VkBufferCreateInfo frameUniformBufferInfo{};
    frameUniformBufferInfo.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    m_frameUniformStride = frameUniformStrideForDevice(m_physicalDevice);
    frameUniformBufferInfo.size = m_frameUniformStride * kFrameUniformRingCount;
    frameUniformBufferInfo.usage = VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT;
    frameUniformBufferInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    if (vkCreateBuffer(m_device, &frameUniformBufferInfo, nullptr, &m_frameUniformBuffer) != VK_SUCCESS) {
      return false;
    }
    VkMemoryRequirements frameUniformRequirements{};
    vkGetBufferMemoryRequirements(m_device, m_frameUniformBuffer, &frameUniformRequirements);
    VkMemoryAllocateInfo frameUniformAlloc{};
    frameUniformAlloc.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    frameUniformAlloc.allocationSize = frameUniformRequirements.size;
    frameUniformAlloc.memoryTypeIndex = findMemoryType(
        m_physicalDevice,
        frameUniformRequirements.memoryTypeBits,
        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    if (vkAllocateMemory(m_device, &frameUniformAlloc, nullptr, &m_frameUniformMemory) != VK_SUCCESS ||
        vkBindBufferMemory(m_device, m_frameUniformBuffer, m_frameUniformMemory, 0) != VK_SUCCESS ||
        vkMapMemory(m_device, m_frameUniformMemory, 0, frameUniformBufferInfo.size, 0, &m_frameUniformMapped) != VK_SUCCESS) {
      return false;
    }
    FrameUniformData frameUniformValues;
    frameUniformValues.outputSizeAndInverse[0] = static_cast<float>(qMax(1, m_outputSize.width()));
    frameUniformValues.outputSizeAndInverse[1] = static_cast<float>(qMax(1, m_outputSize.height()));
    frameUniformValues.outputSizeAndInverse[2] = 1.0f / frameUniformValues.outputSizeAndInverse[0];
    frameUniformValues.outputSizeAndInverse[3] = 1.0f / frameUniformValues.outputSizeAndInverse[1];
    std::memcpy(m_frameUniformMapped, &frameUniformValues, sizeof(frameUniformValues));

    VkDescriptorSetLayoutBinding descriptorBindings[5]{};
    descriptorBindings[0].binding = 0;
    descriptorBindings[0].descriptorType =
        VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    descriptorBindings[0].descriptorCount = 1;
    descriptorBindings[0].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
    descriptorBindings[1].binding = 1;
    descriptorBindings[1].descriptorType =
        VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    descriptorBindings[1].descriptorCount = 1;
    descriptorBindings[1].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
    descriptorBindings[2].binding = 2;
    descriptorBindings[2].descriptorType =
        VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    descriptorBindings[2].descriptorCount = 1;
    descriptorBindings[2].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
    descriptorBindings[3].binding = 3;
    descriptorBindings[3].descriptorType =
        VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    descriptorBindings[3].descriptorCount = 1;
    descriptorBindings[3].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
    descriptorBindings[4].binding = 4;
    descriptorBindings[4].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC;
    descriptorBindings[4].descriptorCount = 1;
    descriptorBindings[4].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;

    VkDescriptorSetLayoutCreateInfo descriptorSetLayoutInfo{};
    descriptorSetLayoutInfo.sType =
        VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    descriptorSetLayoutInfo.bindingCount = 5;
    descriptorSetLayoutInfo.pBindings = descriptorBindings;

    if (vkCreateDescriptorSetLayout(m_device, &descriptorSetLayoutInfo, nullptr,
                                    &m_descriptorSetLayout) != VK_SUCCESS) {
      if (errorMessage) {
        *errorMessage =
            QStringLiteral("Failed to create Vulkan descriptor set layout.");
      }
      return false;
    }

    VkDescriptorPoolSize poolSizes[2]{};
    poolSizes[0].type = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    poolSizes[0].descriptorCount = (1 + kMaxLayerTextures) * 4;
    poolSizes[1].type = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC;
    poolSizes[1].descriptorCount = 1 + kMaxLayerTextures;

    VkDescriptorPoolCreateInfo descriptorPoolInfo{};
    descriptorPoolInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    descriptorPoolInfo.poolSizeCount = 2;
    descriptorPoolInfo.pPoolSizes = poolSizes;
    descriptorPoolInfo.maxSets = 1 + kMaxLayerTextures;

    if (vkCreateDescriptorPool(m_device, &descriptorPoolInfo, nullptr,
                               &m_descriptorPool) != VK_SUCCESS) {
      if (errorMessage) {
        *errorMessage =
            QStringLiteral("Failed to create Vulkan descriptor pool.");
      }
      return false;
    }

    VkDescriptorSetAllocateInfo descriptorSetAllocInfo{};
    descriptorSetAllocInfo.sType =
        VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
    descriptorSetAllocInfo.descriptorPool = m_descriptorPool;
    descriptorSetAllocInfo.descriptorSetCount = 1;
    descriptorSetAllocInfo.pSetLayouts = &m_descriptorSetLayout;

    if (vkAllocateDescriptorSets(m_device, &descriptorSetAllocInfo,
                                 &m_descriptorSet) != VK_SUCCESS) {
      if (errorMessage) {
        *errorMessage =
            QStringLiteral("Failed to allocate Vulkan descriptor set.");
      }
      return false;
    }

    auto createDeviceImage =
        [&](uint32_t width, uint32_t height, VkFormat format,
            VkImageUsageFlags usage, VkImage *image, VkDeviceMemory *memory,
            VkImageView *view, const QString &failurePrefix,
            QString *err) -> bool {
      VkImageCreateInfo imageInfo{};
      imageInfo.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
      imageInfo.imageType = VK_IMAGE_TYPE_2D;
      imageInfo.extent.width = width;
      imageInfo.extent.height = height;
      imageInfo.extent.depth = 1;
      imageInfo.mipLevels = 1;
      imageInfo.arrayLayers = 1;
      imageInfo.format = format;
      imageInfo.tiling = VK_IMAGE_TILING_OPTIMAL;
      imageInfo.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
      imageInfo.usage = usage;
      imageInfo.samples = VK_SAMPLE_COUNT_1_BIT;
      imageInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
      if (vkCreateImage(m_device, &imageInfo, nullptr, image) != VK_SUCCESS) {
        if (err)
          *err = failurePrefix + QStringLiteral(": create image failed.");
        return false;
      }
      VkMemoryRequirements req{};
      vkGetImageMemoryRequirements(m_device, *image, &req);
      const uint32_t memType =
          findMemoryType(m_physicalDevice, req.memoryTypeBits,
                         VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
      if (memType == UINT32_MAX) {
        if (err)
          *err = failurePrefix + QStringLiteral(": no image memory type.");
        return false;
      }
      VkMemoryAllocateInfo ai{};
      ai.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
      ai.allocationSize = req.size;
      ai.memoryTypeIndex = memType;
      if (vkAllocateMemory(m_device, &ai, nullptr, memory) != VK_SUCCESS ||
          vkBindImageMemory(m_device, *image, *memory, 0) != VK_SUCCESS) {
        if (err)
          *err = failurePrefix +
                 QStringLiteral(": allocate/bind image memory failed.");
        return false;
      }
      VkImageViewCreateInfo vi{};
      vi.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
      vi.image = *image;
      vi.viewType = VK_IMAGE_VIEW_TYPE_2D;
      vi.format = format;
      vi.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
      vi.subresourceRange.levelCount = 1;
      vi.subresourceRange.layerCount = 1;
      if (vkCreateImageView(m_device, &vi, nullptr, view) != VK_SUCCESS) {
        if (err)
          *err = failurePrefix + QStringLiteral(": create image view failed.");
        return false;
      }
      return true;
    };

    auto createLayerSlot = [&](QString *err) -> bool {
      LayerTextureSlot slot;
      if (!createDeviceImage(static_cast<uint32_t>(m_outputSize.width()),
                             static_cast<uint32_t>(m_outputSize.height()),
                             VK_FORMAT_R8G8B8A8_UNORM,
                             VK_IMAGE_USAGE_TRANSFER_DST_BIT |
                                 VK_IMAGE_USAGE_SAMPLED_BIT,
                             &slot.image, &slot.memory, &slot.view,
                             QStringLiteral("Vulkan layer image"), err)) {
        return false;
      }
      if (!createDeviceImage(
              kCurveLutWidth, kCurveLutHeight, VK_FORMAT_R8G8B8A8_UNORM,
              VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
              &slot.curveLutImage, &slot.curveLutMemory, &slot.curveLutView,
              QStringLiteral("Vulkan layer curve LUT"), err)) {
        return false;
      }
      if (!createDeviceImage(
              kCurveLutWidth, kCurveLutHeight, VK_FORMAT_R8G8B8A8_UNORM,
              VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
              &slot.maskCurveLutImage, &slot.maskCurveLutMemory, &slot.maskCurveLutView,
              QStringLiteral("Vulkan layer mask curve LUT"), err)) {
        return false;
      }
      if (!createDeviceImage(static_cast<uint32_t>(m_outputSize.width()),
                             static_cast<uint32_t>(m_outputSize.height()),
                             VK_FORMAT_R8G8B8A8_UNORM,
                             VK_IMAGE_USAGE_TRANSFER_DST_BIT |
                                 VK_IMAGE_USAGE_STORAGE_BIT |
                                 VK_IMAGE_USAGE_SAMPLED_BIT,
                             &slot.maskImage, &slot.maskMemory, &slot.maskView,
                             QStringLiteral("Vulkan layer mask image"), err)) {
        return false;
      }
      if (!createDeviceImage(static_cast<uint32_t>(m_outputSize.width()),
                             static_cast<uint32_t>(m_outputSize.height()),
                             VK_FORMAT_R8G8B8A8_UNORM,
                             VK_IMAGE_USAGE_TRANSFER_DST_BIT |
                                 VK_IMAGE_USAGE_STORAGE_BIT |
                                 VK_IMAGE_USAGE_SAMPLED_BIT,
                             &slot.maskRawImage, &slot.maskRawMemory, &slot.maskRawView,
                             QStringLiteral("Vulkan layer raw mask image"), err)) {
        return false;
      }
      if (!createDeviceImage(static_cast<uint32_t>(m_outputSize.width()),
                             static_cast<uint32_t>(m_outputSize.height()),
                             VK_FORMAT_R8G8B8A8_UNORM,
                             VK_IMAGE_USAGE_TRANSFER_DST_BIT |
                                 VK_IMAGE_USAGE_STORAGE_BIT |
                                 VK_IMAGE_USAGE_SAMPLED_BIT,
                             &slot.maskWorkImage, &slot.maskWorkMemory, &slot.maskWorkView,
                             QStringLiteral("Vulkan layer work mask image"), err)) {
        return false;
      }
      VkDescriptorSetAllocateInfo dsi{};
      dsi.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
      dsi.descriptorPool = m_descriptorPool;
      dsi.descriptorSetCount = 1;
      dsi.pSetLayouts = &m_descriptorSetLayout;
      if (vkAllocateDescriptorSets(m_device, &dsi, &slot.descriptorSet) !=
          VK_SUCCESS) {
        if (err)
          *err =
              QStringLiteral("Failed to allocate Vulkan layer descriptor set.");
        return false;
      }
      VkDescriptorImageInfo di[4]{};
      di[0].imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
      di[0].imageView = slot.view;
      di[0].sampler = m_sampler;
      di[1].imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
      di[1].imageView = slot.curveLutView;
      di[1].sampler = m_sampler;
      di[2].imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
      di[2].imageView = slot.maskView;
      di[2].sampler = m_sampler;
      di[3].imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
      di[3].imageView = slot.maskCurveLutView;
      di[3].sampler = m_sampler;
      VkDescriptorBufferInfo frameUniformInfo{};
      frameUniformInfo.buffer = m_frameUniformBuffer;
      frameUniformInfo.range = sizeof(FrameUniformData);
      VkWriteDescriptorSet writes[5]{};
      for (uint32_t binding = 0; binding < 4; ++binding) {
        writes[binding].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        writes[binding].dstSet = slot.descriptorSet;
        writes[binding].dstBinding = binding;
        writes[binding].descriptorCount = 1;
        writes[binding].descriptorType =
            VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        writes[binding].pImageInfo = &di[binding];
      }
      writes[4].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
      writes[4].dstSet = slot.descriptorSet;
      writes[4].dstBinding = 4;
      writes[4].descriptorCount = 1;
      writes[4].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC;
      writes[4].pBufferInfo = &frameUniformInfo;
      vkUpdateDescriptorSets(m_device, 5, writes, 0, nullptr);
      m_layerSlots.push_back(slot);
      return true;
    };
    for (int i = 0; i < kMaxLayerTextures; ++i) {
      if (!createLayerSlot(errorMessage)) {
        return false;
      }
    }

    VkAttachmentDescription colorAttachment{};
    colorAttachment.format = VK_FORMAT_B8G8R8A8_UNORM;
    colorAttachment.samples = VK_SAMPLE_COUNT_1_BIT;
    colorAttachment.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
    colorAttachment.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    colorAttachment.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    colorAttachment.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    colorAttachment.initialLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    colorAttachment.finalLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;

    VkAttachmentReference colorAttachmentRef{};
    colorAttachmentRef.attachment = 0;
    colorAttachmentRef.layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;

    VkSubpassDescription subpass{};
    subpass.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
    subpass.colorAttachmentCount = 1;
    subpass.pColorAttachments = &colorAttachmentRef;

    VkRenderPassCreateInfo renderPassInfo{};
    renderPassInfo.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO;
    renderPassInfo.attachmentCount = 1;
    renderPassInfo.pAttachments = &colorAttachment;
    renderPassInfo.subpassCount = 1;
    renderPassInfo.pSubpasses = &subpass;

    if (vkCreateRenderPass(m_device, &renderPassInfo, nullptr, &m_renderPass) !=
        VK_SUCCESS) {
      if (errorMessage) {
        *errorMessage = QStringLiteral("Failed to create Vulkan render pass.");
      }
      return false;
    }

    VkImageView attachments[] = {m_colorImageView};
    VkFramebufferCreateInfo framebufferInfo{};
    framebufferInfo.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
    framebufferInfo.renderPass = m_renderPass;
    framebufferInfo.attachmentCount = 1;
    framebufferInfo.pAttachments = attachments;
    framebufferInfo.width = static_cast<uint32_t>(m_outputSize.width());
    framebufferInfo.height = static_cast<uint32_t>(m_outputSize.height());
    framebufferInfo.layers = 1;

    if (vkCreateFramebuffer(m_device, &framebufferInfo, nullptr,
                            &m_framebuffer) != VK_SUCCESS) {
      if (errorMessage) {
        *errorMessage = QStringLiteral("Failed to create Vulkan framebuffer.");
      }
      return false;
    }

    const VkDeviceSize layerStagingSize =
        static_cast<VkDeviceSize>(m_outputSize.width()) *
            static_cast<VkDeviceSize>(m_outputSize.height()) * 4 +
        kCurveLutBytes +
        kCurveLutBytes +
        static_cast<VkDeviceSize>(m_outputSize.width()) *
            static_cast<VkDeviceSize>(m_outputSize.height()) * 4;
    const VkDeviceSize stagingSize = layerStagingSize * kMaxLayerTextures;
    VkBufferCreateInfo stagingBufInfo{};
    stagingBufInfo.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    stagingBufInfo.size = stagingSize;
    stagingBufInfo.usage =
        VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT;
    stagingBufInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    VkFenceCreateInfo fenceInfo{};
    fenceInfo.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
    for (int i = 0; i < m_frameSlots.size(); ++i) {
      FrameSlot &slot = m_frameSlots[i];
      if (vkCreateBuffer(m_device, &stagingBufInfo, nullptr,
                         &slot.stagingBuffer) != VK_SUCCESS) {
        if (errorMessage) {
          *errorMessage =
              QStringLiteral("Failed to create Vulkan staging buffer.");
        }
        return false;
      }
      VkMemoryRequirements stagingReq{};
      vkGetBufferMemoryRequirements(m_device, slot.stagingBuffer, &stagingReq);
      VkMemoryPropertyFlags stagingFlags = 0;
      uint32_t stagingType = findMemoryTypePreferred(
          m_physicalDevice, stagingReq.memoryTypeBits,
          VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT,
          VK_MEMORY_PROPERTY_HOST_CACHED_BIT, &stagingFlags);
      if (stagingType == UINT32_MAX) {
        if (errorMessage) {
          *errorMessage =
              QStringLiteral("No suitable Vulkan staging memory type.");
        }
        return false;
      }
      VkMemoryAllocateInfo stagingAlloc{};
      stagingAlloc.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
      stagingAlloc.allocationSize = stagingReq.size;
      stagingAlloc.memoryTypeIndex = stagingType;
      slot.stagingAllocationSize = stagingReq.size;
      slot.stagingHostCoherent =
          (stagingFlags & VK_MEMORY_PROPERTY_HOST_COHERENT_BIT) != 0;
      if (vkAllocateMemory(m_device, &stagingAlloc, nullptr,
                           &slot.stagingMemory) != VK_SUCCESS ||
          vkBindBufferMemory(m_device, slot.stagingBuffer, slot.stagingMemory,
                             0) != VK_SUCCESS) {
        if (errorMessage) {
          *errorMessage =
              QStringLiteral("Failed to allocate/bind Vulkan staging memory.");
        }
        return false;
      }
      if (vkMapMemory(m_device, slot.stagingMemory, 0, VK_WHOLE_SIZE, 0,
                      &slot.stagingMapped) != VK_SUCCESS) {
        if (errorMessage) {
          *errorMessage = QStringLiteral(
              "Failed to persistently map Vulkan staging memory.");
        }
        return false;
      }
      slot.stagingBufferSize = stagingSize;
      if (m_externalMemoryFdSupported && m_vkGetMemoryFdKHR) {
        const VkDeviceSize yPlaneBytes =
            static_cast<VkDeviceSize>(m_outputSize.width()) *
            static_cast<VkDeviceSize>(m_outputSize.height());
        const VkDeviceSize uvPlaneOffset =
            (yPlaneBytes + 255u) & ~VkDeviceSize(255u);
        const VkDeviceSize uvPlaneBytes =
            static_cast<VkDeviceSize>(qMax(1, m_outputSize.width() / 2)) *
            static_cast<VkDeviceSize>(qMax(1, m_outputSize.height() / 2)) * 2;
        const VkDeviceSize nv12ExportBufferSize = uvPlaneOffset + uvPlaneBytes;
        VkExternalMemoryBufferCreateInfo externalBufferInfo{};
        externalBufferInfo.sType =
            VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_BUFFER_CREATE_INFO;
        externalBufferInfo.handleTypes =
            VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_FD_BIT;
        VkBufferCreateInfo exportBufferInfo = stagingBufInfo;
        exportBufferInfo.pNext = &externalBufferInfo;
        exportBufferInfo.size = nv12ExportBufferSize;
        exportBufferInfo.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT;
        const VkResult createExportBufferResult =
            vkCreateBuffer(m_device, &exportBufferInfo, nullptr,
                           &slot.cudaExportBuffer);
        if (createExportBufferResult == VK_SUCCESS) {
          VkMemoryRequirements exportReq{};
          vkGetBufferMemoryRequirements(m_device, slot.cudaExportBuffer,
                                        &exportReq);
          const uint32_t exportType =
              findMemoryType(m_physicalDevice, exportReq.memoryTypeBits,
                             VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
          VkExportMemoryAllocateInfo exportAllocInfo{};
          exportAllocInfo.sType = VK_STRUCTURE_TYPE_EXPORT_MEMORY_ALLOCATE_INFO;
          exportAllocInfo.handleTypes =
              VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_FD_BIT;
          VkMemoryAllocateInfo exportAlloc{};
          exportAlloc.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
          exportAlloc.pNext = &exportAllocInfo;
          exportAlloc.allocationSize = exportReq.size;
          exportAlloc.memoryTypeIndex = exportType;
          VkResult exportAllocResult = VK_ERROR_FEATURE_NOT_PRESENT;
          VkResult exportBindResult = VK_ERROR_FEATURE_NOT_PRESENT;
          if (exportType != UINT32_MAX) {
            exportAllocResult =
                vkAllocateMemory(m_device, &exportAlloc, nullptr,
                                 &slot.cudaExportMemory);
            if (exportAllocResult == VK_SUCCESS) {
              exportBindResult =
                  vkBindBufferMemory(m_device, slot.cudaExportBuffer,
                                     slot.cudaExportMemory, 0);
            }
          }
          if (exportType != UINT32_MAX &&
              exportAllocResult == VK_SUCCESS &&
              exportBindResult == VK_SUCCESS) {
            slot.cudaExportAllocationSize = exportReq.size;
            if (m_externalSemaphoreFdSupported && m_vkGetSemaphoreFdKHR) {
              VkExportSemaphoreCreateInfo semaphoreExport{};
              semaphoreExport.sType =
                  VK_STRUCTURE_TYPE_EXPORT_SEMAPHORE_CREATE_INFO;
              semaphoreExport.handleTypes =
                  VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_OPAQUE_FD_BIT;
              VkSemaphoreCreateInfo semaphoreInfo{};
              semaphoreInfo.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;
              semaphoreInfo.pNext = &semaphoreExport;
              if (vkCreateSemaphore(m_device, &semaphoreInfo, nullptr,
                                    &slot.cudaConsumedSemaphore) !=
                  VK_SUCCESS) {
                m_cudaExternalMemoryStatus = QStringLiteral(
                    "CUDA/Vulkan slot semaphore creation failed");
              }
            }
            m_cudaExternalMemoryStatus =
                QStringLiteral("ready size=%1 memory_type=%2")
                    .arg(static_cast<qulonglong>(exportReq.size))
                    .arg(exportType);
          } else {
            m_cudaExternalMemoryStatus =
                QStringLiteral("export buffer memory failed: type=%1 alloc=%2 bind=%3 size=%4 bits=0x%5")
                    .arg(exportType == UINT32_MAX ? QStringLiteral("none") : QString::number(exportType),
                         QString::number(static_cast<int>(exportAllocResult)),
                         QString::number(static_cast<int>(exportBindResult)))
                    .arg(static_cast<qulonglong>(exportReq.size))
                    .arg(exportReq.memoryTypeBits, 0, 16);
            if (slot.cudaExportMemory != VK_NULL_HANDLE) {
              vkFreeMemory(m_device, slot.cudaExportMemory, nullptr);
              slot.cudaExportMemory = VK_NULL_HANDLE;
            }
            vkDestroyBuffer(m_device, slot.cudaExportBuffer, nullptr);
            slot.cudaExportBuffer = VK_NULL_HANDLE;
          }
        } else {
          m_cudaExternalMemoryStatus =
              QStringLiteral("export buffer create failed: result=%1 size=%2")
                  .arg(static_cast<int>(createExportBufferResult))
                  .arg(static_cast<qulonglong>(nv12ExportBufferSize));
        }
      } else if (!m_externalMemoryFdSupported) {
        m_cudaExternalMemoryStatus = QStringLiteral("VK_KHR_external_memory_fd unsupported");
      } else if (!m_vkGetMemoryFdKHR) {
        m_cudaExternalMemoryStatus = QStringLiteral("vkGetMemoryFdKHR unavailable");
      }
      if (vkCreateFence(m_device, &fenceInfo, nullptr, &slot.fence) !=
          VK_SUCCESS) {
        if (errorMessage) {
          *errorMessage =
              QStringLiteral("Failed to create Vulkan submit fence.");
        }
        return false;
      }
    }
    m_cudaExportBuffersReady = !m_frameSlots.isEmpty() &&
        std::all_of(m_frameSlots.cbegin(), m_frameSlots.cend(),
                    [](const FrameSlot &slot) {
                      return slot.cudaExportBuffer != VK_NULL_HANDLE &&
                          slot.cudaExportMemory != VK_NULL_HANDLE &&
                          slot.cudaConsumedSemaphore != VK_NULL_HANDLE;
                    });
    if (!m_cudaExportBuffersReady &&
        m_externalMemoryFdSupported && m_vkGetMemoryFdKHR) {
      m_cudaExternalMemoryStatus = QStringLiteral(
          "CUDA/Vulkan export buffers require external semaphore synchronization");
    }
    if (m_externalMemoryFdSupported && m_vkGetMemoryFdKHR && !m_cudaExportBuffersReady) {
      qWarning().noquote() << QStringLiteral(
          "Vulkan/CUDA interop capability present but exportable Vulkan "
          "buffers could not be allocated: %1")
          .arg(m_cudaExternalMemoryStatus);
    }
    if (m_cudaExportBuffersReady) {
      qInfo().noquote() << QStringLiteral("Vulkan/CUDA export buffer ready: %1")
                               .arg(m_cudaExternalMemoryStatus);
    }
    useSlot(0);

    auto createAttachment =
        [&](VkFormat format, uint32_t width, uint32_t height, VkImage *image,
            VkDeviceMemory *memory, VkImageView *view, QString *err) -> bool {
      VkImageCreateInfo ci{};
      ci.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
      ci.imageType = VK_IMAGE_TYPE_2D;
      ci.extent = {width, height, 1};
      ci.mipLevels = 1;
      ci.arrayLayers = 1;
      ci.format = format;
      ci.tiling = VK_IMAGE_TILING_OPTIMAL;
      ci.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
      ci.usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT |
                 VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_SAMPLED_BIT |
                 VK_IMAGE_USAGE_STORAGE_BIT;
      ci.samples = VK_SAMPLE_COUNT_1_BIT;
      ci.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
      if (vkCreateImage(m_device, &ci, nullptr, image) != VK_SUCCESS) {
        if (err)
          *err =
              QStringLiteral("Failed to create Vulkan NV12 attachment image.");
        return false;
      }
      VkMemoryRequirements req{};
      vkGetImageMemoryRequirements(m_device, *image, &req);
      const uint32_t memType =
          findMemoryType(m_physicalDevice, req.memoryTypeBits,
                         VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
      if (memType == UINT32_MAX) {
        if (err)
          *err = QStringLiteral("No Vulkan memory type for NV12 attachment.");
        return false;
      }
      VkMemoryAllocateInfo ai{};
      ai.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
      ai.allocationSize = req.size;
      ai.memoryTypeIndex = memType;
      if (vkAllocateMemory(m_device, &ai, nullptr, memory) != VK_SUCCESS ||
          vkBindImageMemory(m_device, *image, *memory, 0) != VK_SUCCESS) {
        if (err)
          *err = QStringLiteral(
              "Failed to allocate/bind Vulkan NV12 attachment memory.");
        return false;
      }
      VkImageViewCreateInfo vi{};
      vi.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
      vi.image = *image;
      vi.viewType = VK_IMAGE_VIEW_TYPE_2D;
      vi.format = format;
      vi.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
      vi.subresourceRange.levelCount = 1;
      vi.subresourceRange.layerCount = 1;
      if (vkCreateImageView(m_device, &vi, nullptr, view) != VK_SUCCESS) {
        if (err)
          *err =
              QStringLiteral("Failed to create Vulkan NV12 attachment view.");
        return false;
      }
      return true;
    };
    if (!createAttachment(
            VK_FORMAT_R8_UNORM, static_cast<uint32_t>(m_outputSize.width()),
            static_cast<uint32_t>(m_outputSize.height()), &m_nv12YImage,
            &m_nv12YImageMemory, &m_nv12YImageView, errorMessage)) {
      return false;
    }
    if (!createAttachment(
            VK_FORMAT_R8G8_UNORM,
            static_cast<uint32_t>(qMax(1, m_outputSize.width() / 2)),
            static_cast<uint32_t>(qMax(1, m_outputSize.height() / 2)),
            &m_nv12UvImage, &m_nv12UvImageMemory, &m_nv12UvImageView,
            errorMessage)) {
      return false;
    }
    if (!createAttachment(
            VK_FORMAT_R8_UNORM,
            static_cast<uint32_t>(qMax(1, m_outputSize.width() / 2)),
            static_cast<uint32_t>(qMax(1, m_outputSize.height() / 2)),
            &m_yuv420pUImage, &m_yuv420pUImageMemory, &m_yuv420pUImageView,
            errorMessage)) {
      return false;
    }
    if (!createAttachment(
            VK_FORMAT_R8_UNORM,
            static_cast<uint32_t>(qMax(1, m_outputSize.width() / 2)),
            static_cast<uint32_t>(qMax(1, m_outputSize.height() / 2)),
            &m_yuv420pVImage, &m_yuv420pVImageMemory, &m_yuv420pVImageView,
            errorMessage)) {
      return false;
    }

    auto createColorRenderPass = [&](VkFormat format, VkRenderPass *pass,
                                     QString *err) -> bool {
      VkAttachmentDescription a{};
      a.format = format;
      a.samples = VK_SAMPLE_COUNT_1_BIT;
      a.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
      a.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
      a.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
      a.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
      a.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
      a.finalLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
      VkAttachmentReference ref{};
      ref.attachment = 0;
      ref.layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
      VkSubpassDescription sub{};
      sub.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
      sub.colorAttachmentCount = 1;
      sub.pColorAttachments = &ref;
      VkRenderPassCreateInfo rp{};
      rp.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO;
      rp.attachmentCount = 1;
      rp.pAttachments = &a;
      rp.subpassCount = 1;
      rp.pSubpasses = &sub;
      if (vkCreateRenderPass(m_device, &rp, nullptr, pass) != VK_SUCCESS) {
        if (err)
          *err = QStringLiteral("Failed to create Vulkan NV12 render pass.");
        return false;
      }
      return true;
    };
    if (!createColorRenderPass(VK_FORMAT_R8_UNORM, &m_nv12YRenderPass,
                               errorMessage) ||
        !createColorRenderPass(VK_FORMAT_R8G8_UNORM, &m_nv12UvRenderPass,
                               errorMessage)) {
      return false;
    }
    VkImageView yAtt[] = {m_nv12YImageView};
    VkFramebufferCreateInfo yFb{};
    yFb.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
    yFb.renderPass = m_nv12YRenderPass;
    yFb.attachmentCount = 1;
    yFb.pAttachments = yAtt;
    yFb.width = static_cast<uint32_t>(m_outputSize.width());
    yFb.height = static_cast<uint32_t>(m_outputSize.height());
    yFb.layers = 1;
    if (vkCreateFramebuffer(m_device, &yFb, nullptr, &m_nv12YFramebuffer) !=
        VK_SUCCESS) {
      if (errorMessage)
        *errorMessage =
            QStringLiteral("Failed to create Vulkan NV12 Y framebuffer.");
      return false;
    }
    VkImageView uvAtt[] = {m_nv12UvImageView};
    VkFramebufferCreateInfo uvFb{};
    uvFb.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
    uvFb.renderPass = m_nv12UvRenderPass;
    uvFb.attachmentCount = 1;
    uvFb.pAttachments = uvAtt;
    uvFb.width = static_cast<uint32_t>(qMax(1, m_outputSize.width() / 2));
    uvFb.height = static_cast<uint32_t>(qMax(1, m_outputSize.height() / 2));
    uvFb.layers = 1;
    if (vkCreateFramebuffer(m_device, &uvFb, nullptr, &m_nv12UvFramebuffer) !=
        VK_SUCCESS) {
      if (errorMessage)
        *errorMessage =
            QStringLiteral("Failed to create Vulkan NV12 UV framebuffer.");
      return false;
    }
    VkImageView uAtt[] = {m_yuv420pUImageView};
    VkFramebufferCreateInfo uFb = uvFb;
    uFb.renderPass = m_nv12YRenderPass;
    uFb.pAttachments = uAtt;
    if (vkCreateFramebuffer(m_device, &uFb, nullptr, &m_yuv420pUFramebuffer) !=
        VK_SUCCESS) {
      if (errorMessage)
        *errorMessage =
            QStringLiteral("Failed to create Vulkan YUV420P U framebuffer.");
      return false;
    }
    VkImageView vAtt[] = {m_yuv420pVImageView};
    VkFramebufferCreateInfo vFb = uvFb;
    vFb.renderPass = m_nv12YRenderPass;
    vFb.pAttachments = vAtt;
    if (vkCreateFramebuffer(m_device, &vFb, nullptr, &m_yuv420pVFramebuffer) !=
        VK_SUCCESS) {
      if (errorMessage)
        *errorMessage =
            QStringLiteral("Failed to create Vulkan YUV420P V framebuffer.");
      return false;
    }

    VkDescriptorSetLayoutBinding yuvBindings[4]{};
    yuvBindings[0].binding = 0;
    yuvBindings[0].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    yuvBindings[0].descriptorCount = 1;
    yuvBindings[0].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
    for (uint32_t i = 1; i < 4; ++i) {
      yuvBindings[i].binding = i;
      yuvBindings[i].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
      yuvBindings[i].descriptorCount = 1;
      yuvBindings[i].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
    }
    VkDescriptorSetLayoutCreateInfo yuvLayoutInfo{};
    yuvLayoutInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    yuvLayoutInfo.bindingCount = 4;
    yuvLayoutInfo.pBindings = yuvBindings;
    if (vkCreateDescriptorSetLayout(m_device, &yuvLayoutInfo, nullptr,
                                    &m_yuvComputeDescriptorSetLayout) !=
        VK_SUCCESS) {
      if (errorMessage)
        *errorMessage = QStringLiteral(
            "Failed to create Vulkan YUV compute descriptor layout.");
      return false;
    }
    VkDescriptorPoolSize yuvPoolSizes[2]{};
    yuvPoolSizes[0].type = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    yuvPoolSizes[0].descriptorCount = 1;
    yuvPoolSizes[1].type = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
    yuvPoolSizes[1].descriptorCount = 3;
    VkDescriptorPoolCreateInfo yuvPoolInfo{};
    yuvPoolInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    yuvPoolInfo.poolSizeCount = 2;
    yuvPoolInfo.pPoolSizes = yuvPoolSizes;
    yuvPoolInfo.maxSets = 1;
    if (vkCreateDescriptorPool(m_device, &yuvPoolInfo, nullptr,
                               &m_yuvComputeDescriptorPool) != VK_SUCCESS) {
      if (errorMessage)
        *errorMessage = QStringLiteral(
            "Failed to create Vulkan YUV compute descriptor pool.");
      return false;
    }
    VkDescriptorSetAllocateInfo yuvSetInfo{};
    yuvSetInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
    yuvSetInfo.descriptorPool = m_yuvComputeDescriptorPool;
    yuvSetInfo.descriptorSetCount = 1;
    yuvSetInfo.pSetLayouts = &m_yuvComputeDescriptorSetLayout;
    if (vkAllocateDescriptorSets(m_device, &yuvSetInfo,
                                 &m_yuvComputeDescriptorSet) != VK_SUCCESS) {
      if (errorMessage)
        *errorMessage = QStringLiteral(
            "Failed to allocate Vulkan YUV compute descriptor set.");
      return false;
    }
    VkDescriptorImageInfo yuvSrc{};
    yuvSrc.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    yuvSrc.imageView = m_colorImageView;
    yuvSrc.sampler = m_sampler;
    VkDescriptorImageInfo yuvImages[3]{};
    yuvImages[0].imageLayout = VK_IMAGE_LAYOUT_GENERAL;
    yuvImages[0].imageView = m_nv12YImageView;
    yuvImages[1].imageLayout = VK_IMAGE_LAYOUT_GENERAL;
    yuvImages[1].imageView = m_yuv420pUImageView;
    yuvImages[2].imageLayout = VK_IMAGE_LAYOUT_GENERAL;
    yuvImages[2].imageView = m_yuv420pVImageView;
    VkWriteDescriptorSet yuvWrites[4]{};
    yuvWrites[0].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    yuvWrites[0].dstSet = m_yuvComputeDescriptorSet;
    yuvWrites[0].dstBinding = 0;
    yuvWrites[0].descriptorCount = 1;
    yuvWrites[0].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    yuvWrites[0].pImageInfo = &yuvSrc;
    for (uint32_t i = 1; i < 4; ++i) {
      yuvWrites[i].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
      yuvWrites[i].dstSet = m_yuvComputeDescriptorSet;
      yuvWrites[i].dstBinding = i;
      yuvWrites[i].descriptorCount = 1;
      yuvWrites[i].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
      yuvWrites[i].pImageInfo = &yuvImages[i - 1];
    }
    vkUpdateDescriptorSets(m_device, 4, yuvWrites, 0, nullptr);

     VkPushConstantRange effectsPushConstantRange{};
    effectsPushConstantRange.stageFlags =
        VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;
    effectsPushConstantRange.offset = 0;
    effectsPushConstantRange.size = 128;

    VkPipelineLayoutCreateInfo effectsLayoutInfo{};
    effectsLayoutInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    effectsLayoutInfo.setLayoutCount = 1;
    effectsLayoutInfo.pSetLayouts = &m_descriptorSetLayout;
    effectsLayoutInfo.pushConstantRangeCount = 1;
    effectsLayoutInfo.pPushConstantRanges = &effectsPushConstantRange;
    if (vkCreatePipelineLayout(m_device, &effectsLayoutInfo, nullptr,
                               &m_effectsPipelineLayout) != VK_SUCCESS) {
      if (errorMessage) {
        *errorMessage =
            QStringLiteral("Failed to create Vulkan effects pipeline layout.");
      }
      return false;
    }

    VkPushConstantRange maskPushConstantRange{};
    maskPushConstantRange.stageFlags = VK_SHADER_STAGE_VERTEX_BIT;
    maskPushConstantRange.offset = 0;
    maskPushConstantRange.size = 64;
    VkPipelineLayoutCreateInfo maskLayoutInfo{};
    maskLayoutInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    maskLayoutInfo.pushConstantRangeCount = 1;
    maskLayoutInfo.pPushConstantRanges = &maskPushConstantRange;
    if (vkCreatePipelineLayout(m_device, &maskLayoutInfo, nullptr,
                               &m_maskPipelineLayout) != VK_SUCCESS) {
      if (errorMessage) {
        *errorMessage =
            QStringLiteral("Failed to create Vulkan mask pipeline layout.");
      }
      return false;
    }

    VkPipelineLayoutCreateInfo nv12LayoutInfo{};
    nv12LayoutInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    nv12LayoutInfo.setLayoutCount = 1;
    nv12LayoutInfo.pSetLayouts = &m_descriptorSetLayout;
    if (vkCreatePipelineLayout(m_device, &nv12LayoutInfo, nullptr,
                               &m_nv12PipelineLayout) != VK_SUCCESS) {
      if (errorMessage) {
        *errorMessage =
            QStringLiteral("Failed to create Vulkan NV12 pipeline layout.");
      }
      return false;
    }
    VkPipelineLayoutCreateInfo yuvComputeLayoutInfo{};
    yuvComputeLayoutInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    yuvComputeLayoutInfo.setLayoutCount = 1;
    yuvComputeLayoutInfo.pSetLayouts = &m_yuvComputeDescriptorSetLayout;
    if (vkCreatePipelineLayout(m_device, &yuvComputeLayoutInfo, nullptr,
                               &m_yuvComputePipelineLayout) != VK_SUCCESS) {
      if (errorMessage) {
        *errorMessage = QStringLiteral(
            "Failed to create Vulkan YUV compute pipeline layout.");
      }
      return false;
    }
     const QString shaderDir = jcutVulkanShaderDirectory();
    m_effectsVertModule = createShaderModule(
        m_device,
        readBinaryFile(shaderDir + QStringLiteral("/effects.vert.spv")));
    m_effectsFragModule = createShaderModule(
        m_device,
        readBinaryFile(shaderDir + QStringLiteral("/effects.frag.spv")));
    m_maskVertModule = createShaderModule(
        m_device, readBinaryFile(shaderDir + QStringLiteral("/mask.vert.spv")));
    m_maskFragModule = createShaderModule(
        m_device, readBinaryFile(shaderDir + QStringLiteral("/mask.frag.spv")));
    m_nv12VertModule = createShaderModule(
        m_device, readBinaryFile(shaderDir + QStringLiteral("/nv12.vert.spv")));
    m_nv12YFragModule = createShaderModule(
        m_device,
        readBinaryFile(shaderDir + QStringLiteral("/nv12_y.frag.spv")));
    m_nv12UvFragModule = createShaderModule(
        m_device,
        readBinaryFile(shaderDir + QStringLiteral("/nv12_uv.frag.spv")));
    m_yuv420pUFragModule = createShaderModule(
        m_device,
        readBinaryFile(shaderDir + QStringLiteral("/yuv420p_u.frag.spv")));
    m_yuv420pVFragModule = createShaderModule(
        m_device,
        readBinaryFile(shaderDir + QStringLiteral("/yuv420p_v.frag.spv")));
    m_yuv420pComputeModule = createShaderModule(
        m_device,
        readBinaryFile(shaderDir + QStringLiteral("/yuv420p.comp.spv")));
    if (m_effectsVertModule == VK_NULL_HANDLE ||
        m_effectsFragModule == VK_NULL_HANDLE ||
        m_maskVertModule == VK_NULL_HANDLE ||
        m_maskFragModule == VK_NULL_HANDLE ||
        m_nv12VertModule == VK_NULL_HANDLE ||
        m_nv12YFragModule == VK_NULL_HANDLE ||
        m_nv12UvFragModule == VK_NULL_HANDLE ||
        m_yuv420pUFragModule == VK_NULL_HANDLE ||
        m_yuv420pVFragModule == VK_NULL_HANDLE ||
        m_yuv420pComputeModule == VK_NULL_HANDLE) {
      if (errorMessage) {
        *errorMessage = QStringLiteral(
            "Failed to load Vulkan SPIR-V shader modules. Ensure "
            "shader build step succeeded.");
      }
      return false;
    }

    VkDescriptorImageInfo imageInfoDesc[4]{};
    imageInfoDesc[0].imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    imageInfoDesc[0].imageView = m_colorImageView;
    imageInfoDesc[0].sampler = m_sampler;
    imageInfoDesc[1].imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    imageInfoDesc[1].imageView = !m_layerSlots.isEmpty()
                                     ? m_layerSlots.first().curveLutView
                                     : VK_NULL_HANDLE;
    imageInfoDesc[1].sampler = m_sampler;
    imageInfoDesc[2].imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    imageInfoDesc[2].imageView = !m_layerSlots.isEmpty()
                                     ? m_layerSlots.first().maskView
                                     : VK_NULL_HANDLE;
    imageInfoDesc[2].sampler = m_sampler;
    imageInfoDesc[3].imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    imageInfoDesc[3].imageView = !m_layerSlots.isEmpty()
                                     ? m_layerSlots.first().maskCurveLutView
                                     : VK_NULL_HANDLE;
    imageInfoDesc[3].sampler = m_sampler;
    VkDescriptorBufferInfo frameUniformInfo{};
    frameUniformInfo.buffer = m_frameUniformBuffer;
    frameUniformInfo.range = sizeof(FrameUniformData);
    VkWriteDescriptorSet writes[5]{};
    for (uint32_t binding = 0; binding < 4; ++binding) {
      writes[binding].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
      writes[binding].dstSet = m_descriptorSet;
      writes[binding].dstBinding = binding;
      writes[binding].descriptorCount = 1;
      writes[binding].descriptorType =
          VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
      writes[binding].pImageInfo = &imageInfoDesc[binding];
    }
    writes[4].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    writes[4].dstSet = m_descriptorSet;
    writes[4].dstBinding = 4;
    writes[4].descriptorCount = 1;
    writes[4].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC;
    writes[4].pBufferInfo = &frameUniformInfo;
    vkUpdateDescriptorSets(m_device, 5, writes, 0, nullptr);

    auto createPipeline = [&](VkShaderModule vert, VkShaderModule frag,
                              VkPipelineLayout layout, VkRenderPass renderPass,
                              VkPipeline *outPipeline) -> bool {
      VkPipelineShaderStageCreateInfo shaderStages[2]{};
      shaderStages[0].sType =
          VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
      shaderStages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
      shaderStages[0].module = vert;
      shaderStages[0].pName = "main";
      shaderStages[1].sType =
          VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
      shaderStages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
      shaderStages[1].module = frag;
      shaderStages[1].pName = "main";

      VkPipelineVertexInputStateCreateInfo vertexInput{};
      vertexInput.sType =
          VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;
      vertexInput.vertexBindingDescriptionCount = 0;
      vertexInput.vertexAttributeDescriptionCount = 0;

      VkPipelineInputAssemblyStateCreateInfo inputAssembly{};
      inputAssembly.sType =
          VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
      inputAssembly.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP;

      VkPipelineViewportStateCreateInfo viewportState{};
      viewportState.sType =
          VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
      viewportState.viewportCount = 1;
      viewportState.scissorCount = 1;

      VkPipelineRasterizationStateCreateInfo rasterizer{};
      rasterizer.sType =
          VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
      rasterizer.polygonMode = VK_POLYGON_MODE_FILL;
      rasterizer.lineWidth = 1.0f;
      rasterizer.cullMode = VK_CULL_MODE_NONE;
      rasterizer.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;

      VkPipelineMultisampleStateCreateInfo multisampling{};
      multisampling.sType =
          VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
      multisampling.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
      VkPipelineDepthStencilStateCreateInfo depthStencil{};
      depthStencil.sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO;
      depthStencil.depthTestEnable = VK_FALSE;
      depthStencil.depthWriteEnable = VK_FALSE;
      depthStencil.depthCompareOp = VK_COMPARE_OP_ALWAYS;
      depthStencil.depthBoundsTestEnable = VK_FALSE;
      depthStencil.stencilTestEnable = VK_FALSE;

      VkPipelineColorBlendAttachmentState colorBlendAttachment{};
      colorBlendAttachment.colorWriteMask =
          VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
          VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
      colorBlendAttachment.blendEnable = VK_TRUE;
      colorBlendAttachment.srcColorBlendFactor = VK_BLEND_FACTOR_ONE;
      colorBlendAttachment.dstColorBlendFactor =
          VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
      colorBlendAttachment.colorBlendOp = VK_BLEND_OP_ADD;
      colorBlendAttachment.srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
      colorBlendAttachment.dstAlphaBlendFactor =
          VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
      colorBlendAttachment.alphaBlendOp = VK_BLEND_OP_ADD;
      VkPipelineColorBlendStateCreateInfo colorBlending{};
      colorBlending.sType =
          VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
      colorBlending.attachmentCount = 1;
      colorBlending.pAttachments = &colorBlendAttachment;
      VkDynamicState dynamicStates[] = {VK_DYNAMIC_STATE_VIEWPORT,
                                        VK_DYNAMIC_STATE_SCISSOR};
      VkPipelineDynamicStateCreateInfo dynamicState{};
      dynamicState.sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO;
      dynamicState.dynamicStateCount = 2;
      dynamicState.pDynamicStates = dynamicStates;

      VkGraphicsPipelineCreateInfo pipelineInfo{};
      pipelineInfo.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
      pipelineInfo.stageCount = 2;
      pipelineInfo.pStages = shaderStages;
      pipelineInfo.pVertexInputState = &vertexInput;
      pipelineInfo.pInputAssemblyState = &inputAssembly;
      pipelineInfo.pViewportState = &viewportState;
      pipelineInfo.pRasterizationState = &rasterizer;
      pipelineInfo.pMultisampleState = &multisampling;
      pipelineInfo.pDepthStencilState = &depthStencil;
      pipelineInfo.pColorBlendState = &colorBlending;
      pipelineInfo.pDynamicState = &dynamicState;
      pipelineInfo.layout = layout;
      pipelineInfo.renderPass = renderPass;
      pipelineInfo.subpass = 0;

      return vkCreateGraphicsPipelines(m_device, VK_NULL_HANDLE, 1,
                                       &pipelineInfo, nullptr,
                                       outPipeline) == VK_SUCCESS;
    };

    VkComputePipelineCreateInfo computeInfo{};
    computeInfo.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO;
    computeInfo.stage.sType =
        VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    computeInfo.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
    computeInfo.stage.module = m_yuv420pComputeModule;
    computeInfo.stage.pName = "main";
    computeInfo.layout = m_yuvComputePipelineLayout;
    if (vkCreateComputePipelines(m_device, VK_NULL_HANDLE, 1, &computeInfo,
                                 nullptr,
                                 &m_yuv420pComputePipeline) != VK_SUCCESS) {
      if (errorMessage) {
        *errorMessage =
            QStringLiteral("Failed to create Vulkan YUV420P compute pipeline.");
      }
      return false;
    }

    if (!m_maskPreprocessor.initialize(
            m_physicalDevice, m_device, errorMessage)) {
      return false;
    }

	    if (!createPipeline(m_effectsVertModule, m_effectsFragModule,
	                        m_effectsPipelineLayout, m_renderPass,
	                        &m_effectsPipeline) ||
        !createPipeline(m_nv12VertModule, m_nv12YFragModule,
                        m_nv12PipelineLayout, m_nv12YRenderPass,
                        &m_nv12YPipeline) ||
        !createPipeline(m_nv12VertModule, m_nv12UvFragModule,
                        m_nv12PipelineLayout, m_nv12UvRenderPass,
                        &m_nv12UvPipeline) ||
        !createPipeline(m_nv12VertModule, m_yuv420pUFragModule,
                        m_nv12PipelineLayout, m_nv12YRenderPass,
                        &m_yuv420pUPipeline) ||
        !createPipeline(m_nv12VertModule, m_yuv420pVFragModule,
                        m_nv12PipelineLayout, m_nv12YRenderPass,
                        &m_yuv420pVPipeline)) {
      if (errorMessage) {
        *errorMessage =
            QStringLiteral("Failed to create Vulkan graphics pipelines.");
	      }
	      return false;
	    }

    m_transcriptTextRenderer = std::make_unique<VulkanTextRenderer>();
    if (!m_transcriptTextRenderer->initialize(m_physicalDevice, m_device, nullptr, m_renderPass, errorMessage)) {
      return false;
    }
    m_speakerTextRenderer = std::make_unique<VulkanTextRenderer>();
    if (!m_speakerTextRenderer->initialize(m_physicalDevice, m_device, nullptr, m_renderPass, errorMessage)) {
      return false;
    }

	    m_initialized = true;
	    return true;
	  }

	  void release() {
	    if (m_nv12ScratchFrame) {
	      av_frame_free(&m_nv12ScratchFrame);
	    }
    if (m_device != VK_NULL_HANDLE) {
      vkDeviceWaitIdle(m_device);
    }
    m_maskPreprocessor.destroy();
    destroyPreviewSlots();
    m_speakerTextRenderer.reset();
    m_transcriptTextRenderer.reset();
    for (FrameSlot &slot : m_frameSlots) {
#if JCUT_HAS_CUDA_DRIVER
      retireCudaConsumedSemaphore(slot);
      if (slot.cudaExternalMemory) {
        // Do not explicitly destroy the imported opaque-FD allocation from the
        // render worker. The import is tied to the CUDA device/context retained
        // in cudaImportDeviceRef; forcing the driver teardown entry point here
        // has proven unsafe across incremental export chunk boundaries.
        // Retiring the retained device reference lets the CUDA context own the
        // import lifetime without putting driver teardown on the hot path.
        slot.cudaExternalMemory = nullptr;
        slot.cudaExternalDevicePtr = 0;
        slot.cudaImportContext = nullptr;
      }
      if (slot.cudaImportDeviceRef) {
        av_buffer_unref(&slot.cudaImportDeviceRef);
      }
#endif
      if (slot.stagingMapped && slot.stagingMemory != VK_NULL_HANDLE) {
        vkUnmapMemory(m_device, slot.stagingMemory);
        slot.stagingMapped = nullptr;
      }
      if (slot.fence != VK_NULL_HANDLE) {
        vkDestroyFence(m_device, slot.fence, nullptr);
        slot.fence = VK_NULL_HANDLE;
      }
      if (slot.cudaConsumedSemaphore != VK_NULL_HANDLE) {
        vkDestroySemaphore(m_device, slot.cudaConsumedSemaphore, nullptr);
        slot.cudaConsumedSemaphore = VK_NULL_HANDLE;
      }
      if (slot.stagingBuffer != VK_NULL_HANDLE) {
        vkDestroyBuffer(m_device, slot.stagingBuffer, nullptr);
        slot.stagingBuffer = VK_NULL_HANDLE;
      }
      if (slot.stagingMemory != VK_NULL_HANDLE) {
        vkFreeMemory(m_device, slot.stagingMemory, nullptr);
        slot.stagingMemory = VK_NULL_HANDLE;
      }
      slot.stagingBufferSize = 0;
      slot.stagingAllocationSize = 0;
      if (slot.cudaExportBuffer != VK_NULL_HANDLE) {
        vkDestroyBuffer(m_device, slot.cudaExportBuffer, nullptr);
        slot.cudaExportBuffer = VK_NULL_HANDLE;
      }
      if (slot.cudaExportMemory != VK_NULL_HANDLE) {
        vkFreeMemory(m_device, slot.cudaExportMemory, nullptr);
        slot.cudaExportMemory = VK_NULL_HANDLE;
      }
      slot.commandBuffer = VK_NULL_HANDLE;
      slot.inFlight = false;
    }
    m_frameSlots.clear();
    m_activeSlotIndex = -1;
    m_pendingNv12SlotIndices.clear();
    m_pendingNv12CudaSlotIndices.clear();
    m_pendingYuvSlotIndices.clear();
    m_commandBuffer = VK_NULL_HANDLE;
    m_stagingBuffer = VK_NULL_HANDLE;
    m_stagingMemory = VK_NULL_HANDLE;
    m_stagingMapped = nullptr;
    m_submitFence = VK_NULL_HANDLE;
    m_cudaExportBuffersReady = false;
    for (LayerTextureSlot &slot : m_layerSlots) {
      if (slot.maskView != VK_NULL_HANDLE) {
        vkDestroyImageView(m_device, slot.maskView, nullptr);
        slot.maskView = VK_NULL_HANDLE;
      }
      if (slot.maskImage != VK_NULL_HANDLE) {
        vkDestroyImage(m_device, slot.maskImage, nullptr);
        slot.maskImage = VK_NULL_HANDLE;
      }
      if (slot.maskMemory != VK_NULL_HANDLE) {
        vkFreeMemory(m_device, slot.maskMemory, nullptr);
        slot.maskMemory = VK_NULL_HANDLE;
      }
      if (slot.maskRawView != VK_NULL_HANDLE) {
        vkDestroyImageView(m_device, slot.maskRawView, nullptr);
        slot.maskRawView = VK_NULL_HANDLE;
      }
      if (slot.maskRawImage != VK_NULL_HANDLE) {
        vkDestroyImage(m_device, slot.maskRawImage, nullptr);
        slot.maskRawImage = VK_NULL_HANDLE;
      }
      if (slot.maskRawMemory != VK_NULL_HANDLE) {
        vkFreeMemory(m_device, slot.maskRawMemory, nullptr);
        slot.maskRawMemory = VK_NULL_HANDLE;
      }
      if (slot.maskWorkView != VK_NULL_HANDLE) {
        vkDestroyImageView(m_device, slot.maskWorkView, nullptr);
        slot.maskWorkView = VK_NULL_HANDLE;
      }
      if (slot.maskWorkImage != VK_NULL_HANDLE) {
        vkDestroyImage(m_device, slot.maskWorkImage, nullptr);
        slot.maskWorkImage = VK_NULL_HANDLE;
      }
      if (slot.maskWorkMemory != VK_NULL_HANDLE) {
        vkFreeMemory(m_device, slot.maskWorkMemory, nullptr);
        slot.maskWorkMemory = VK_NULL_HANDLE;
      }
      if (slot.curveLutView != VK_NULL_HANDLE) {
        vkDestroyImageView(m_device, slot.curveLutView, nullptr);
        slot.curveLutView = VK_NULL_HANDLE;
      }
      if (slot.curveLutImage != VK_NULL_HANDLE) {
        vkDestroyImage(m_device, slot.curveLutImage, nullptr);
        slot.curveLutImage = VK_NULL_HANDLE;
      }
      if (slot.curveLutMemory != VK_NULL_HANDLE) {
        vkFreeMemory(m_device, slot.curveLutMemory, nullptr);
        slot.curveLutMemory = VK_NULL_HANDLE;
      }
      if (slot.maskCurveLutView != VK_NULL_HANDLE) {
        vkDestroyImageView(m_device, slot.maskCurveLutView, nullptr);
        slot.maskCurveLutView = VK_NULL_HANDLE;
      }
      if (slot.maskCurveLutImage != VK_NULL_HANDLE) {
        vkDestroyImage(m_device, slot.maskCurveLutImage, nullptr);
        slot.maskCurveLutImage = VK_NULL_HANDLE;
      }
      if (slot.maskCurveLutMemory != VK_NULL_HANDLE) {
        vkFreeMemory(m_device, slot.maskCurveLutMemory, nullptr);
        slot.maskCurveLutMemory = VK_NULL_HANDLE;
      }
      if (slot.view != VK_NULL_HANDLE) {
        vkDestroyImageView(m_device, slot.view, nullptr);
        slot.view = VK_NULL_HANDLE;
      }
      if (slot.image != VK_NULL_HANDLE) {
        vkDestroyImage(m_device, slot.image, nullptr);
        slot.image = VK_NULL_HANDLE;
      }
      if (slot.memory != VK_NULL_HANDLE) {
        vkFreeMemory(m_device, slot.memory, nullptr);
        slot.memory = VK_NULL_HANDLE;
      }
    }
    m_layerSlots.clear();
    if (m_effectsPipeline != VK_NULL_HANDLE) {
      vkDestroyPipeline(m_device, m_effectsPipeline, nullptr);
      m_effectsPipeline = VK_NULL_HANDLE;
    }
    if (m_maskPipeline != VK_NULL_HANDLE) {
      vkDestroyPipeline(m_device, m_maskPipeline, nullptr);
      m_maskPipeline = VK_NULL_HANDLE;
    }
    if (m_nv12YPipeline != VK_NULL_HANDLE) {
      vkDestroyPipeline(m_device, m_nv12YPipeline, nullptr);
      m_nv12YPipeline = VK_NULL_HANDLE;
    }
    if (m_nv12UvPipeline != VK_NULL_HANDLE) {
      vkDestroyPipeline(m_device, m_nv12UvPipeline, nullptr);
      m_nv12UvPipeline = VK_NULL_HANDLE;
    }
    if (m_yuv420pUPipeline != VK_NULL_HANDLE) {
      vkDestroyPipeline(m_device, m_yuv420pUPipeline, nullptr);
      m_yuv420pUPipeline = VK_NULL_HANDLE;
    }
    if (m_yuv420pVPipeline != VK_NULL_HANDLE) {
      vkDestroyPipeline(m_device, m_yuv420pVPipeline, nullptr);
      m_yuv420pVPipeline = VK_NULL_HANDLE;
    }
    if (m_yuv420pComputePipeline != VK_NULL_HANDLE) {
      vkDestroyPipeline(m_device, m_yuv420pComputePipeline, nullptr);
      m_yuv420pComputePipeline = VK_NULL_HANDLE;
    }
    if (m_nv12YFramebuffer != VK_NULL_HANDLE) {
      vkDestroyFramebuffer(m_device, m_nv12YFramebuffer, nullptr);
      m_nv12YFramebuffer = VK_NULL_HANDLE;
    }
    if (m_nv12UvFramebuffer != VK_NULL_HANDLE) {
      vkDestroyFramebuffer(m_device, m_nv12UvFramebuffer, nullptr);
      m_nv12UvFramebuffer = VK_NULL_HANDLE;
    }
    if (m_yuv420pUFramebuffer != VK_NULL_HANDLE) {
      vkDestroyFramebuffer(m_device, m_yuv420pUFramebuffer, nullptr);
      m_yuv420pUFramebuffer = VK_NULL_HANDLE;
    }
    if (m_yuv420pVFramebuffer != VK_NULL_HANDLE) {
      vkDestroyFramebuffer(m_device, m_yuv420pVFramebuffer, nullptr);
      m_yuv420pVFramebuffer = VK_NULL_HANDLE;
    }
    if (m_nv12YRenderPass != VK_NULL_HANDLE) {
      vkDestroyRenderPass(m_device, m_nv12YRenderPass, nullptr);
      m_nv12YRenderPass = VK_NULL_HANDLE;
    }
    if (m_nv12UvRenderPass != VK_NULL_HANDLE) {
      vkDestroyRenderPass(m_device, m_nv12UvRenderPass, nullptr);
      m_nv12UvRenderPass = VK_NULL_HANDLE;
    }
    if (m_nv12YImageView != VK_NULL_HANDLE) {
      vkDestroyImageView(m_device, m_nv12YImageView, nullptr);
      m_nv12YImageView = VK_NULL_HANDLE;
    }
    if (m_nv12UvImageView != VK_NULL_HANDLE) {
      vkDestroyImageView(m_device, m_nv12UvImageView, nullptr);
      m_nv12UvImageView = VK_NULL_HANDLE;
    }
    if (m_yuv420pUImageView != VK_NULL_HANDLE) {
      vkDestroyImageView(m_device, m_yuv420pUImageView, nullptr);
      m_yuv420pUImageView = VK_NULL_HANDLE;
    }
    if (m_yuv420pVImageView != VK_NULL_HANDLE) {
      vkDestroyImageView(m_device, m_yuv420pVImageView, nullptr);
      m_yuv420pVImageView = VK_NULL_HANDLE;
    }
    if (m_nv12YImage != VK_NULL_HANDLE) {
      vkDestroyImage(m_device, m_nv12YImage, nullptr);
      m_nv12YImage = VK_NULL_HANDLE;
    }
    if (m_nv12UvImage != VK_NULL_HANDLE) {
      vkDestroyImage(m_device, m_nv12UvImage, nullptr);
      m_nv12UvImage = VK_NULL_HANDLE;
    }
    if (m_yuv420pUImage != VK_NULL_HANDLE) {
      vkDestroyImage(m_device, m_yuv420pUImage, nullptr);
      m_yuv420pUImage = VK_NULL_HANDLE;
    }
    if (m_yuv420pVImage != VK_NULL_HANDLE) {
      vkDestroyImage(m_device, m_yuv420pVImage, nullptr);
      m_yuv420pVImage = VK_NULL_HANDLE;
    }
    if (m_nv12YImageMemory != VK_NULL_HANDLE) {
      vkFreeMemory(m_device, m_nv12YImageMemory, nullptr);
      m_nv12YImageMemory = VK_NULL_HANDLE;
    }
    if (m_nv12UvImageMemory != VK_NULL_HANDLE) {
      vkFreeMemory(m_device, m_nv12UvImageMemory, nullptr);
      m_nv12UvImageMemory = VK_NULL_HANDLE;
    }
    if (m_yuv420pUImageMemory != VK_NULL_HANDLE) {
      vkFreeMemory(m_device, m_yuv420pUImageMemory, nullptr);
      m_yuv420pUImageMemory = VK_NULL_HANDLE;
    }
    if (m_yuv420pVImageMemory != VK_NULL_HANDLE) {
      vkFreeMemory(m_device, m_yuv420pVImageMemory, nullptr);
      m_yuv420pVImageMemory = VK_NULL_HANDLE;
    }
    if (m_effectsPipelineLayout != VK_NULL_HANDLE) {
      vkDestroyPipelineLayout(m_device, m_effectsPipelineLayout, nullptr);
      m_effectsPipelineLayout = VK_NULL_HANDLE;
    }
    if (m_maskPipelineLayout != VK_NULL_HANDLE) {
      vkDestroyPipelineLayout(m_device, m_maskPipelineLayout, nullptr);
      m_maskPipelineLayout = VK_NULL_HANDLE;
    }
    if (m_nv12PipelineLayout != VK_NULL_HANDLE) {
      vkDestroyPipelineLayout(m_device, m_nv12PipelineLayout, nullptr);
      m_nv12PipelineLayout = VK_NULL_HANDLE;
    }
    if (m_yuvComputePipelineLayout != VK_NULL_HANDLE) {
      vkDestroyPipelineLayout(m_device, m_yuvComputePipelineLayout, nullptr);
      m_yuvComputePipelineLayout = VK_NULL_HANDLE;
    }
    if (m_effectsVertModule != VK_NULL_HANDLE) {
      vkDestroyShaderModule(m_device, m_effectsVertModule, nullptr);
      m_effectsVertModule = VK_NULL_HANDLE;
    }
    if (m_effectsFragModule != VK_NULL_HANDLE) {
      vkDestroyShaderModule(m_device, m_effectsFragModule, nullptr);
      m_effectsFragModule = VK_NULL_HANDLE;
    }
    if (m_maskVertModule != VK_NULL_HANDLE) {
      vkDestroyShaderModule(m_device, m_maskVertModule, nullptr);
      m_maskVertModule = VK_NULL_HANDLE;
    }
    if (m_maskFragModule != VK_NULL_HANDLE) {
      vkDestroyShaderModule(m_device, m_maskFragModule, nullptr);
      m_maskFragModule = VK_NULL_HANDLE;
    }
    if (m_nv12VertModule != VK_NULL_HANDLE) {
      vkDestroyShaderModule(m_device, m_nv12VertModule, nullptr);
      m_nv12VertModule = VK_NULL_HANDLE;
    }
    if (m_nv12YFragModule != VK_NULL_HANDLE) {
      vkDestroyShaderModule(m_device, m_nv12YFragModule, nullptr);
      m_nv12YFragModule = VK_NULL_HANDLE;
    }
    if (m_nv12UvFragModule != VK_NULL_HANDLE) {
      vkDestroyShaderModule(m_device, m_nv12UvFragModule, nullptr);
      m_nv12UvFragModule = VK_NULL_HANDLE;
    }
    if (m_yuv420pUFragModule != VK_NULL_HANDLE) {
      vkDestroyShaderModule(m_device, m_yuv420pUFragModule, nullptr);
      m_yuv420pUFragModule = VK_NULL_HANDLE;
    }
    if (m_yuv420pVFragModule != VK_NULL_HANDLE) {
      vkDestroyShaderModule(m_device, m_yuv420pVFragModule, nullptr);
      m_yuv420pVFragModule = VK_NULL_HANDLE;
    }
    if (m_yuv420pComputeModule != VK_NULL_HANDLE) {
      vkDestroyShaderModule(m_device, m_yuv420pComputeModule, nullptr);
      m_yuv420pComputeModule = VK_NULL_HANDLE;
    }
    if (m_framebuffer != VK_NULL_HANDLE) {
      vkDestroyFramebuffer(m_device, m_framebuffer, nullptr);
      m_framebuffer = VK_NULL_HANDLE;
    }
    if (m_renderPass != VK_NULL_HANDLE) {
      vkDestroyRenderPass(m_device, m_renderPass, nullptr);
      m_renderPass = VK_NULL_HANDLE;
    }
    if (m_frameUniformMapped && m_frameUniformMemory != VK_NULL_HANDLE) {
      vkUnmapMemory(m_device, m_frameUniformMemory);
      m_frameUniformMapped = nullptr;
    }
    if (m_frameUniformBuffer != VK_NULL_HANDLE) {
      vkDestroyBuffer(m_device, m_frameUniformBuffer, nullptr);
      m_frameUniformBuffer = VK_NULL_HANDLE;
    }
    if (m_frameUniformMemory != VK_NULL_HANDLE) {
      vkFreeMemory(m_device, m_frameUniformMemory, nullptr);
      m_frameUniformMemory = VK_NULL_HANDLE;
    }
    if (m_descriptorPool != VK_NULL_HANDLE) {
      vkDestroyDescriptorPool(m_device, m_descriptorPool, nullptr);
      m_descriptorPool = VK_NULL_HANDLE;
    }
    if (m_yuvComputeDescriptorPool != VK_NULL_HANDLE) {
      vkDestroyDescriptorPool(m_device, m_yuvComputeDescriptorPool, nullptr);
      m_yuvComputeDescriptorPool = VK_NULL_HANDLE;
    }
    if (m_descriptorSetLayout != VK_NULL_HANDLE) {
      vkDestroyDescriptorSetLayout(m_device, m_descriptorSetLayout, nullptr);
      m_descriptorSetLayout = VK_NULL_HANDLE;
    }
    if (m_yuvComputeDescriptorSetLayout != VK_NULL_HANDLE) {
      vkDestroyDescriptorSetLayout(m_device, m_yuvComputeDescriptorSetLayout,
                                   nullptr);
      m_yuvComputeDescriptorSetLayout = VK_NULL_HANDLE;
    }
    if (m_sampler != VK_NULL_HANDLE) {
      vkDestroySampler(m_device, m_sampler, nullptr);
      m_sampler = VK_NULL_HANDLE;
    }
    if (m_colorImageView != VK_NULL_HANDLE) {
      vkDestroyImageView(m_device, m_colorImageView, nullptr);
      m_colorImageView = VK_NULL_HANDLE;
    }
    if (m_colorImage != VK_NULL_HANDLE) {
      vkDestroyImage(m_device, m_colorImage, nullptr);
      m_colorImage = VK_NULL_HANDLE;
    }
    if (m_colorImageMemory != VK_NULL_HANDLE) {
      vkFreeMemory(m_device, m_colorImageMemory, nullptr);
      m_colorImageMemory = VK_NULL_HANDLE;
    }
    if (m_commandPool != VK_NULL_HANDLE) {
      vkDestroyCommandPool(m_device, m_commandPool, nullptr);
      m_commandPool = VK_NULL_HANDLE;
    }
    if (m_device != VK_NULL_HANDLE) {
      vkDestroyDevice(m_device, nullptr);
      m_device = VK_NULL_HANDLE;
    }
    if (m_instance != VK_NULL_HANDLE) {
      vkDestroyInstance(m_instance, nullptr);
      m_instance = VK_NULL_HANDLE;
    }

    m_physicalDevice = VK_NULL_HANDLE;
    m_nonCoherentAtomSize = 1;
    m_graphicsQueue = VK_NULL_HANDLE;
    m_commandBuffer = VK_NULL_HANDLE;
    m_descriptorSet = VK_NULL_HANDLE;
    m_yuvComputeDescriptorSet = VK_NULL_HANDLE;
    m_graphicsQueueFamily = UINT32_MAX;
    m_initialized = false;
    m_yuv420pPlanesPrimed = false;
    m_colorImageLayout = VK_IMAGE_LAYOUT_UNDEFINED;
  }

  struct LayerInput : public VulkanRenderLayerPacket {
    QImage image;
    OverlayImage overlayImage;
    bool preferHardwareDirect = false;
    QString cacheKey;
    float backgroundShadows[4] = {0.0f, 0.0f, 0.0f, 0.0f};
    float backgroundMidtones[4] = {0.0f, 0.0f, 0.0f, 0.0f};
    float backgroundHighlights[4] = {0.0f, 0.0f, 0.0f, 0.0f};
    float backgroundGrade[4] = {0.0f, 1.0f, 1.0f, 0.0f};
    float mvp[16] = {1.f, 0.f, 0.f, 0.f, 0.f, 1.f, 0.f, 0.f,
                     0.f, 0.f, 1.f, 0.f, 0.f, 0.f, 0.f, 1.f};
    bool presetScissorEnabled = false;
    QRectF presetScissorRect;
  };

  using TranscriptTextInput = VulkanRenderTranscriptInput;
  using VulkanTextInputs = VulkanRenderTextInputs;

