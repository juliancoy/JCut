// Staging, mask preprocessing, submission, and preview publication methods.
// Included inside OffscreenVulkanRendererPrivate; do not compile separately.
  bool writeOverlayImageToStagingTopLeft(const OverlayImage &overlay,
                                         VkDeviceSize stagingOffset) {
    if (overlay.isNull() || !m_stagingMapped) {
      return false;
    }
    const int rowBytes = overlay.width * 4;
    const size_t bytes = static_cast<size_t>(rowBytes) * static_cast<size_t>(overlay.height);
    if (overlay.rgbaPremultiplied.size() < static_cast<qsizetype>(bytes)) {
      return false;
    }
    if (!activeStagingRangeAvailable(
            stagingOffset, static_cast<VkDeviceSize>(bytes))) {
      return false;
    }
    auto *dst = reinterpret_cast<uint8_t *>(m_stagingMapped) + stagingOffset;
    const auto *src = reinterpret_cast<const uint8_t *>(overlay.rgbaPremultiplied.constData());
    for (int y = 0; y < overlay.height; ++y) {
      std::memcpy(dst + (static_cast<size_t>(y) * rowBytes),
                  src + (static_cast<size_t>(y) * rowBytes),
                  static_cast<size_t>(rowBytes));
    }
    return flushActiveStagingWrite(stagingOffset, static_cast<VkDeviceSize>(bytes));
  }

  OverlayImage placementGuideOverlay(const QSize& outputSize,
                                     bool instagramSafeAreaGuides,
                                     bool alignmentGridGuides) {
    if (!instagramSafeAreaGuides && !alignmentGridGuides) {
      return {};
    }
    if (m_cachedPlacementGuideOverlaySize == outputSize &&
        m_cachedPlacementGuideInstagramSafeArea == instagramSafeAreaGuides &&
        m_cachedPlacementGuideAlignmentGrid == alignmentGridGuides &&
        !m_cachedPlacementGuideOverlay.isNull()) {
      return m_cachedPlacementGuideOverlay;
    }

    OverlayImage overlay;
    overlay.width = qMax(1, outputSize.width());
    overlay.height = qMax(1, outputSize.height());
    overlay.rgbaPremultiplied.resize(
        static_cast<qsizetype>(overlay.width) *
        static_cast<qsizetype>(overlay.height) * 4);
    overlay.rgbaPremultiplied.fill(char(0));

    auto setPixel = [&](int x, int y, int r, int g, int b, int a) {
      if (x < 0 || x >= overlay.width || y < 0 || y >= overlay.height) {
        return;
      }
      const qsizetype offset =
          (static_cast<qsizetype>(y) * overlay.width + x) * 4;
      overlay.rgbaPremultiplied[offset + 0] = static_cast<char>((r * a) / 255);
      overlay.rgbaPremultiplied[offset + 1] = static_cast<char>((g * a) / 255);
      overlay.rgbaPremultiplied[offset + 2] = static_cast<char>((b * a) / 255);
      overlay.rgbaPremultiplied[offset + 3] = static_cast<char>(a);
    };
    auto drawHorizontal = [&](int centerY, int thickness, int r, int g, int b, int a) {
      const int half = qMax(0, thickness / 2);
      for (int y = centerY - half; y <= centerY + half; ++y) {
        for (int x = 0; x < overlay.width; ++x) {
          setPixel(x, y, r, g, b, a);
        }
      }
    };
    auto drawVertical = [&](int centerX, int thickness, int r, int g, int b, int a) {
      const int half = qMax(0, thickness / 2);
      for (int x = centerX - half; x <= centerX + half; ++x) {
        for (int y = 0; y < overlay.height; ++y) {
          setPixel(x, y, r, g, b, a);
        }
      }
    };

    const int thin = qMax(1, qMin(overlay.width, overlay.height) / 720);
    const int thick = qMax(2, thin * 2);
    if (alignmentGridGuides) {
      for (int i = 1; i <= 2; ++i) {
        drawVertical(qRound((static_cast<qreal>(overlay.width) * i) / 3.0),
                     thin, 128, 209, 255, 210);
        drawHorizontal(qRound((static_cast<qreal>(overlay.height) * i) / 3.0),
                       thin, 128, 209, 255, 210);
      }
    }
    if (instagramSafeAreaGuides) {
      const int inset = qMin(250, overlay.height / 2);
      drawHorizontal(inset, thick, 255, 214, 64, 235);
      drawHorizontal(qMax(0, overlay.height - inset), thick, 255, 214, 64, 235);
    }

    m_cachedPlacementGuideOverlay = overlay;
    m_cachedPlacementGuideOverlaySize = outputSize;
    m_cachedPlacementGuideInstagramSafeArea = instagramSafeAreaGuides;
    m_cachedPlacementGuideAlignmentGrid = alignmentGridGuides;
    return m_cachedPlacementGuideOverlay;
  }

  bool writeRgbaImageToStagingTopLeft(const QImage &rgba,
                                      VkDeviceSize stagingOffset) {
    if (rgba.isNull() || !m_stagingMapped ||
        rgba.format() != QImage::Format_RGBA8888) {
      return false;
    }
    const int rowBytes = rgba.width() * 4;
    const size_t bytes = static_cast<size_t>(rowBytes) * static_cast<size_t>(rgba.height());
    if (!activeStagingRangeAvailable(
            stagingOffset, static_cast<VkDeviceSize>(bytes))) {
      return false;
    }
    auto *dst = reinterpret_cast<uint8_t *>(m_stagingMapped) + stagingOffset;
    for (int y = 0; y < rgba.height(); ++y) {
      std::memcpy(dst + (static_cast<size_t>(y) * rowBytes),
                  rgba.constScanLine(y),
                  static_cast<size_t>(rowBytes));
    }
    return flushActiveStagingWrite(stagingOffset, static_cast<VkDeviceSize>(bytes));
  }

  bool writeImageBufferToStagingTopLeft(const jcut::core::ImageBuffer &image,
                                        VkDeviceSize stagingOffset,
                                        int bytesPerPixel) {
    if (image.empty() || !m_stagingMapped || bytesPerPixel <= 0 ||
        image.strideBytes < image.size.width * bytesPerPixel) {
      return false;
    }
    const int rowBytes = image.size.width * bytesPerPixel;
    const size_t bytes =
        static_cast<size_t>(rowBytes) * static_cast<size_t>(image.size.height);
    if (!activeStagingRangeAvailable(
            stagingOffset, static_cast<VkDeviceSize>(bytes))) {
      return false;
    }
    auto *dst = reinterpret_cast<uint8_t *>(m_stagingMapped) + stagingOffset;
    for (int y = 0; y < image.size.height; ++y) {
      std::memcpy(dst + (static_cast<size_t>(y) * rowBytes),
                  image.bytes.data() +
                      (static_cast<size_t>(y) *
                       static_cast<size_t>(image.strideBytes)),
                  static_cast<size_t>(rowBytes));
    }
    return flushActiveStagingWrite(stagingOffset,
                                   static_cast<VkDeviceSize>(bytes));
  }

  bool writePackedTemporalMaskToStagingTopLeft(
      const QByteArray &packed,
      const QSize &size,
      VkDeviceSize stagingOffset) {
    const VkDeviceSize expectedBytes =
        static_cast<VkDeviceSize>(size.width()) *
        static_cast<VkDeviceSize>(size.height()) * 4;
    if (!size.isValid() || packed.size() != expectedBytes ||
        !m_stagingMapped ||
        !activeStagingRangeAvailable(stagingOffset, expectedBytes)) {
      return false;
    }
    std::memcpy(
        reinterpret_cast<std::uint8_t *>(m_stagingMapped) + stagingOffset,
        packed.constData(),
        static_cast<std::size_t>(expectedBytes));
    return flushActiveStagingWrite(stagingOffset, expectedBytes);
  }

  VulkanMaskPreprocessOptions maskPreprocessOptions(
      const LayerInput &layer) const {
    VulkanMaskPreprocessOptions options;
    options.outputSize = m_outputSize;
    options.sourceIdentity = layer.maskIdentity;
    options.correctionStorage = layer.maskCorrectionStorage;
    options.invert = layer.maskInvert;
    options.erodeRadius = qRound(qMax<qreal>(0.0, layer.maskErode));
    options.dilateRadius = qRound(qMax<qreal>(0.0, layer.maskDilate));
    options.blurRadius = qRound(qMax<qreal>(0.0, layer.maskBlur));
    options.temporalStabilizeEnabled =
        layer.maskTemporalStabilizeEnabled;
    options.temporalStabilizeStrength = static_cast<float>(
        qBound<qreal>(0.0, layer.maskTemporalStabilizeStrength, 1.0));
    options.temporalStabilizeMotionRadius = qBound(
        0, layer.maskTemporalStabilizeMotionRadius, 32);
    if (!layer.temporalMaskIdentity.isEmpty()) {
      options.sourceIdentity +=
          QStringLiteral("|temporal=%1").arg(layer.temporalMaskIdentity);
    }
    return options;
  }

  bool preprocessLayerMask(LayerTextureSlot &slot,
                           const LayerInput &layer,
                           VkDeviceSize correctionStorageOffset,
                           VkDeviceSize correctionStorageCapacity) {
    VulkanMaskPreprocessor::Images images;
    images.sampler = m_sampler;
    images.inputSize = layer.maskSourceSize.isValid()
        ? layer.maskSourceSize
        : m_outputSize;
    images.outputSize = m_outputSize;
    images.inputView = slot.maskRawView;
    images.outputImage = slot.maskImage;
    images.outputView = slot.maskView;
    images.outputLayout = &slot.maskLayout;
    images.workImage = slot.maskWorkImage;
    images.workView = slot.maskWorkView;
    images.workLayout = &slot.maskWorkLayout;
    const VulkanMaskPreprocessOptions options =
        maskPreprocessOptions(layer);
    slot.maskUploaded = m_maskPreprocessor.record(
        m_commandBuffer,
        images,
        options,
        [this, correctionStorageOffset, correctionStorageCapacity](
            const QByteArray& storage,
            VkDeviceSize alignment,
            VulkanMaskPreprocessor::StagedCorrectionStorage* staged) {
          if (!staged || alignment == 0 ||
              (correctionStorageOffset % alignment) != 0 ||
              static_cast<VkDeviceSize>(storage.size()) >
                  correctionStorageCapacity ||
              !activeStagingRangeAvailable(
                  correctionStorageOffset,
                  static_cast<VkDeviceSize>(storage.size()))) {
            return false;
          }
          std::memcpy(
              reinterpret_cast<std::uint8_t*>(m_stagingMapped) +
                  correctionStorageOffset,
              storage.constData(),
              static_cast<std::size_t>(storage.size()));
          if (!flushActiveStagingWrite(
                  correctionStorageOffset,
                  static_cast<VkDeviceSize>(storage.size()))) {
            return false;
          }
          staged->buffer = m_stagingBuffer;
          staged->offset = correctionStorageOffset;
          staged->bytes = static_cast<VkDeviceSize>(storage.size());
          return true;
        });
    return slot.maskUploaded;
  }

  bool ensureMaskRawImage(LayerTextureSlot &slot,
                          const QSize &size,
                          VkFormat format = VK_FORMAT_R8G8B8A8_UNORM) {
    if (!size.isValid() || size.isEmpty()) {
      return false;
    }
    if (slot.maskRawImage != VK_NULL_HANDLE &&
        slot.maskRawMemory != VK_NULL_HANDLE &&
        slot.maskRawView != VK_NULL_HANDLE &&
        slot.maskRawSize == size &&
        slot.maskRawFormat == format) {
      return true;
    }
    if (vkDeviceWaitIdle(m_device) != VK_SUCCESS) {
      return false;
    }
    if (slot.maskRawView != VK_NULL_HANDLE) {
      vkDestroyImageView(m_device, slot.maskRawView, nullptr);
    }
    if (slot.maskRawImage != VK_NULL_HANDLE) {
      vkDestroyImage(m_device, slot.maskRawImage, nullptr);
    }
    if (slot.maskRawMemory != VK_NULL_HANDLE) {
      vkFreeMemory(m_device, slot.maskRawMemory, nullptr);
    }
    slot.maskRawView = VK_NULL_HANDLE;
    slot.maskRawImage = VK_NULL_HANDLE;
    slot.maskRawMemory = VK_NULL_HANDLE;
    slot.maskRawLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    slot.maskRawSize = {};
    slot.maskRawFormat = VK_FORMAT_UNDEFINED;

    VkImageCreateInfo imageInfo{};
    imageInfo.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    imageInfo.imageType = VK_IMAGE_TYPE_2D;
    imageInfo.extent = {
        static_cast<uint32_t>(size.width()),
        static_cast<uint32_t>(size.height()), 1};
    imageInfo.mipLevels = 1;
    imageInfo.arrayLayers = 1;
    imageInfo.format = format;
    imageInfo.tiling = VK_IMAGE_TILING_OPTIMAL;
    imageInfo.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    imageInfo.usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT |
                      VK_IMAGE_USAGE_STORAGE_BIT |
                      VK_IMAGE_USAGE_SAMPLED_BIT;
    imageInfo.samples = VK_SAMPLE_COUNT_1_BIT;
    imageInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    if (vkCreateImage(
            m_device, &imageInfo, nullptr,
            &slot.maskRawImage) != VK_SUCCESS) {
      return false;
    }
    VkMemoryRequirements requirements{};
    vkGetImageMemoryRequirements(
        m_device, slot.maskRawImage, &requirements);
    const uint32_t memoryType =
        findMemoryType(
            m_physicalDevice,
            requirements.memoryTypeBits,
            VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    if (memoryType == UINT32_MAX) {
      return false;
    }
    VkMemoryAllocateInfo allocation{};
    allocation.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    allocation.allocationSize = requirements.size;
    allocation.memoryTypeIndex = memoryType;
    if (vkAllocateMemory(
            m_device, &allocation, nullptr,
            &slot.maskRawMemory) != VK_SUCCESS ||
        vkBindImageMemory(
            m_device, slot.maskRawImage,
            slot.maskRawMemory, 0) != VK_SUCCESS) {
      return false;
    }
    VkImageViewCreateInfo view{};
    view.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    view.image = slot.maskRawImage;
    view.viewType = VK_IMAGE_VIEW_TYPE_2D;
    view.format = format;
    view.subresourceRange.aspectMask =
        VK_IMAGE_ASPECT_COLOR_BIT;
    view.subresourceRange.levelCount = 1;
    view.subresourceRange.layerCount = 1;
    if (vkCreateImageView(
            m_device, &view, nullptr,
            &slot.maskRawView) != VK_SUCCESS) {
      return false;
    }
    slot.maskRawSize = size;
    slot.maskRawFormat = format;
    return true;
  }

  bool submitAndWait() {
    if (!submitActiveSlot()) {
      return false;
    }
    return waitSlot(m_activeSlotIndex);
  }

  bool submitActiveSlot() {
    if (m_activeSlotIndex < 0 || m_activeSlotIndex >= m_frameSlots.size() ||
        m_submitFence == VK_NULL_HANDLE) {
      return false;
    }
    FrameSlot &slot = m_frameSlots[m_activeSlotIndex];
    if (slot.commandBuffer == VK_NULL_HANDLE || slot.fence == VK_NULL_HANDLE) {
      return false;
    }
    vkResetFences(m_device, 1, &slot.fence);
    VkSubmitInfo submitInfo{};
    submitInfo.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    std::array<VkSemaphore, 2> waitSemaphores{};
    std::array<VkPipelineStageFlags, 2> waitStages{};
    uint32_t waitCount = 0;
    if (slot.cudaCopyPending) {
      waitSemaphores[waitCount] = slot.cudaConsumedSemaphore;
      waitStages[waitCount] = VK_PIPELINE_STAGE_ALL_COMMANDS_BIT;
      ++waitCount;
    }
    if (m_pendingPreviewWait != VK_NULL_HANDLE) {
      waitSemaphores[waitCount] = m_pendingPreviewWait;
      waitStages[waitCount] = VK_PIPELINE_STAGE_TRANSFER_BIT;
      ++waitCount;
    }
    submitInfo.waitSemaphoreCount = waitCount;
    submitInfo.pWaitSemaphores = waitCount ? waitSemaphores.data() : nullptr;
    submitInfo.pWaitDstStageMask = waitCount ? waitStages.data() : nullptr;
    submitInfo.commandBufferCount = 1;
    submitInfo.pCommandBuffers = &slot.commandBuffer;
    if (m_pendingPreviewSignal != VK_NULL_HANDLE) {
      submitInfo.signalSemaphoreCount = 1;
      submitInfo.pSignalSemaphores = &m_pendingPreviewSignal;
    }
    if (vkQueueSubmit(m_graphicsQueue, 1, &submitInfo, slot.fence) !=
        VK_SUCCESS) {
      return false;
    }
    m_pendingPreviewWait = VK_NULL_HANDLE;
    m_pendingPreviewSignal = VK_NULL_HANDLE;
    slot.cudaCopyPending = false;
    slot.inFlight = true;
    return true;
  }

  bool publishLastFrameForGpuPreview(OffscreenVulkanFrame *frame,
                                     QString *errorMessage) {
    if (frame) {
      *frame = OffscreenVulkanFrame{};
    }
    if (!frame || !m_commandBufferOpenForConversion ||
        m_previewSlots.size() < 3 || m_activeSlotIndex < 0) {
      if (errorMessage) {
        *errorMessage =
            QStringLiteral("GPU preview double buffers are unavailable.");
      }
      return false;
    }
    int previewIndex = -1;
    for (int offset = 1; offset <= m_previewSlots.size(); ++offset) {
      const int candidate =
          (m_lastPreviewSlotIndex + offset) % m_previewSlots.size();
      const PreviewSlot &candidateSlot = m_previewSlots[candidate];
      if (!candidateSlot.published ||
          (candidateSlot.consumptionState &&
           candidateSlot.consumptionState->completedGeneration.load(
               std::memory_order_acquire) >= candidateSlot.generation)) {
        previewIndex = candidate;
        break;
      }
    }
    if (previewIndex < 0) {
      if (errorMessage) {
        *errorMessage = QStringLiteral(
            "GPU export preview dropped: all optional preview slots are busy.");
      }
      return false;
    }
    PreviewSlot &slot = m_previewSlots[previewIndex];
    // Host acknowledgment proves that the prior consumed signal has already
    // been queued. Preserve the semaphore wait for device ownership without
    // ever enqueueing it for an unresponsive optional consumer.
    m_pendingPreviewWait =
        slot.published ? slot.consumedSemaphore : VK_NULL_HANDLE;
    m_pendingPreviewSignal = slot.readySemaphore;

    transitionImageLayout(m_commandBuffer, m_colorImage, m_colorImageLayout,
                          VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
    m_colorImageLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    if (slot.published) {
      VkImageMemoryBarrier acquire{};
      acquire.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
      acquire.srcAccessMask = 0;
      acquire.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
      acquire.oldLayout = slot.layout;
      acquire.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
      acquire.srcQueueFamilyIndex = VK_QUEUE_FAMILY_EXTERNAL;
      acquire.dstQueueFamilyIndex = m_graphicsQueueFamily;
      acquire.image = slot.image;
      acquire.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
      acquire.subresourceRange.levelCount = 1;
      acquire.subresourceRange.layerCount = 1;
      vkCmdPipelineBarrier(m_commandBuffer,
                           VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                           VK_PIPELINE_STAGE_TRANSFER_BIT,
                           0,
                           0,
                           nullptr,
                           0,
                           nullptr,
                           1,
                           &acquire);
    } else {
      transitionImageLayout(m_commandBuffer, slot.image, slot.layout,
                            VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
    }
    slot.layout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    VkImageCopy copy{};
    copy.srcSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    copy.srcSubresource.layerCount = 1;
    copy.dstSubresource = copy.srcSubresource;
    copy.extent = {
        static_cast<uint32_t>(m_outputSize.width()),
        static_cast<uint32_t>(m_outputSize.height()),
        1};
    vkCmdCopyImage(m_commandBuffer,
                   m_colorImage,
                   VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                   slot.image,
                   VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                   1,
                   &copy);
    VkImageMemoryBarrier release{};
    release.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    release.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    release.dstAccessMask = 0;
    release.oldLayout = slot.layout;
    release.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    release.srcQueueFamilyIndex = m_graphicsQueueFamily;
    release.dstQueueFamilyIndex = VK_QUEUE_FAMILY_EXTERNAL;
    release.image = slot.image;
    release.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    release.subresourceRange.levelCount = 1;
    release.subresourceRange.layerCount = 1;
    vkCmdPipelineBarrier(m_commandBuffer,
                         VK_PIPELINE_STAGE_TRANSFER_BIT,
                         VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT,
                         0,
                         0,
                         nullptr,
                         0,
                         nullptr,
                         1,
                         &release);
    slot.layout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    transitionImageLayout(m_commandBuffer, m_colorImage, m_colorImageLayout,
                          VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
    m_colorImageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;

    int readyFd = -1;
    int consumedFd = -1;
    if (!slot.handlesExported) {
      VkSemaphoreGetFdInfoKHR fdInfo{};
      fdInfo.sType = VK_STRUCTURE_TYPE_SEMAPHORE_GET_FD_INFO_KHR;
      fdInfo.handleType =
          VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_OPAQUE_FD_BIT;
      fdInfo.semaphore = slot.readySemaphore;
      if (m_vkGetSemaphoreFdKHR(m_device, &fdInfo, &readyFd) != VK_SUCCESS) {
        readyFd = -1;
      }
      fdInfo.semaphore = slot.consumedSemaphore;
      if (m_vkGetSemaphoreFdKHR(m_device, &fdInfo, &consumedFd) !=
          VK_SUCCESS) {
        consumedFd = -1;
      }
      if (readyFd < 0 || consumedFd < 0) {
        if (readyFd >= 0) {
          close(readyFd);
        }
        if (consumedFd >= 0) {
          close(consumedFd);
        }
        m_pendingPreviewWait = VK_NULL_HANDLE;
        m_pendingPreviewSignal = VK_NULL_HANDLE;
        if (errorMessage) {
          *errorMessage =
              QStringLiteral("Failed to export GPU preview semaphores.");
        }
        return false;
      }
      slot.handlesExported = true;
    }

    ++slot.generation;
    slot.published = true;
    m_lastPreviewSlotIndex = previewIndex;
    frame->physicalDevice = m_physicalDevice;
    frame->device = m_device;
    frame->queue = m_graphicsQueue;
    frame->queueFamilyIndex = m_graphicsQueueFamily;
    frame->image = slot.image;
    frame->imageView = slot.view;
    frame->imageMemory = slot.memory;
    frame->imageLayout = slot.layout;
    frame->imageFormat = VK_FORMAT_B8G8R8A8_UNORM;
    frame->readySemaphoreFd = readyFd;
    frame->consumedSemaphoreFd = consumedFd;
    frame->bufferIndex = static_cast<std::uint32_t>(previewIndex);
    frame->memoryTypeIndex = slot.memoryTypeIndex;
    frame->memoryAllocationSize = slot.memoryAllocationSize;
    frame->producerSessionId = m_producerSessionId;
    frame->generation = slot.generation;
    frame->consumptionState = slot.consumptionState;
    frame->size = {m_outputSize.width(), m_outputSize.height()};
    frame->queueSupportsCompute = m_graphicsQueueSupportsCompute;
    frame->valid = true;
    if (slot.generation == 1) {
      qInfo().noquote()
          << QStringLiteral(
                 "[render-export-preview] published GPU slot=%1 "
                 "producer_session=%2")
                 .arg(previewIndex)
                 .arg(m_producerSessionId);
    }
    return true;
  }

  void destroyPreviewSlots() {
    for (PreviewSlot &slot : m_previewSlots) {
      if (slot.readySemaphore != VK_NULL_HANDLE) {
        vkDestroySemaphore(m_device, slot.readySemaphore, nullptr);
      }
      if (slot.consumedSemaphore != VK_NULL_HANDLE) {
        vkDestroySemaphore(m_device, slot.consumedSemaphore, nullptr);
      }
      if (slot.view != VK_NULL_HANDLE) {
        vkDestroyImageView(m_device, slot.view, nullptr);
      }
      if (slot.image != VK_NULL_HANDLE) {
        vkDestroyImage(m_device, slot.image, nullptr);
      }
      if (slot.memory != VK_NULL_HANDLE) {
        vkFreeMemory(m_device, slot.memory, nullptr);
      }
    }
    m_previewSlots.clear();
    m_lastPreviewSlotIndex = -1;
    m_pendingPreviewWait = VK_NULL_HANDLE;
    m_pendingPreviewSignal = VK_NULL_HANDLE;
  }

  bool waitSlot(int slotIndex) {
    if (slotIndex < 0 || slotIndex >= m_frameSlots.size()) {
      return false;
    }
    FrameSlot &slot = m_frameSlots[slotIndex];
    if (slot.inFlight) {
      const VkResult waitResult = vkWaitForFences(
          m_device,
          1,
          &slot.fence,
          VK_TRUE,
          kExportGpuFenceTimeoutNs);
      if (waitResult != VK_SUCCESS) {
        qWarning().noquote()
            << QStringLiteral(
                   "[vulkan-sync-timeout] stage=export_frame_slot "
                   "slot=%1 timeout_ms=5000 result=%2 "
                   "preview_wait_pending=%3 preview_signal_pending=%4")
                   .arg(slotIndex)
                   .arg(static_cast<int>(waitResult))
                   .arg(m_pendingPreviewWait != VK_NULL_HANDLE)
                   .arg(m_pendingPreviewSignal != VK_NULL_HANDLE);
        return false;
      }
      slot.inFlight = false;
    }
    return true;
  }

  bool activeStagingRangeAvailable(VkDeviceSize offset,
                                   VkDeviceSize size) const {
    if (m_activeSlotIndex < 0 ||
        m_activeSlotIndex >= m_frameSlots.size()) {
      return false;
    }
    const FrameSlot &slot = m_frameSlots[m_activeSlotIndex];
    return slot.stagingBuffer != VK_NULL_HANDLE &&
        slot.stagingMemory != VK_NULL_HANDLE &&
        slot.stagingMapped != nullptr &&
        offset <= slot.stagingBufferSize &&
        size <= slot.stagingBufferSize - offset;
  }

  bool ensureActiveStagingCapacity(VkDeviceSize requiredSize) {
    if (requiredSize == 0 || m_activeSlotIndex < 0 ||
        m_activeSlotIndex >= m_frameSlots.size()) {
      return false;
    }
    FrameSlot &slot = m_frameSlots[m_activeSlotIndex];
    if (slot.inFlight) {
      return false;
    }
    if (slot.stagingBuffer != VK_NULL_HANDLE &&
        slot.stagingMemory != VK_NULL_HANDLE &&
        slot.stagingMapped != nullptr &&
        slot.stagingBufferSize >= requiredSize) {
      return true;
    }

    VkBufferCreateInfo bufferInfo{};
    bufferInfo.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    bufferInfo.size = requiredSize;
    bufferInfo.usage =
        VK_BUFFER_USAGE_TRANSFER_SRC_BIT |
        VK_BUFFER_USAGE_TRANSFER_DST_BIT |
        VK_BUFFER_USAGE_STORAGE_BUFFER_BIT;
    bufferInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    VkBuffer newBuffer = VK_NULL_HANDLE;
    if (vkCreateBuffer(
            m_device, &bufferInfo, nullptr, &newBuffer) != VK_SUCCESS) {
      return false;
    }

    VkMemoryRequirements requirements{};
    vkGetBufferMemoryRequirements(m_device, newBuffer, &requirements);
    VkMemoryPropertyFlags memoryFlags = 0;
    const uint32_t memoryType = findMemoryTypePreferred(
        m_physicalDevice, requirements.memoryTypeBits,
        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT,
        VK_MEMORY_PROPERTY_HOST_CACHED_BIT, &memoryFlags);
    if (memoryType == UINT32_MAX) {
      vkDestroyBuffer(m_device, newBuffer, nullptr);
      return false;
    }

    VkMemoryAllocateInfo allocation{};
    allocation.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    allocation.allocationSize = requirements.size;
    allocation.memoryTypeIndex = memoryType;
    VkDeviceMemory newMemory = VK_NULL_HANDLE;
    if (vkAllocateMemory(
            m_device, &allocation, nullptr, &newMemory) != VK_SUCCESS) {
      vkDestroyBuffer(m_device, newBuffer, nullptr);
      return false;
    }
    if (vkBindBufferMemory(
            m_device, newBuffer, newMemory, 0) != VK_SUCCESS) {
      vkFreeMemory(m_device, newMemory, nullptr);
      vkDestroyBuffer(m_device, newBuffer, nullptr);
      return false;
    }
    void *newMapped = nullptr;
    if (vkMapMemory(
            m_device, newMemory, 0, VK_WHOLE_SIZE, 0,
            &newMapped) != VK_SUCCESS) {
      vkDestroyBuffer(m_device, newBuffer, nullptr);
      vkFreeMemory(m_device, newMemory, nullptr);
      return false;
    }

    if (slot.stagingMapped && slot.stagingMemory != VK_NULL_HANDLE) {
      vkUnmapMemory(m_device, slot.stagingMemory);
    }
    if (slot.stagingBuffer != VK_NULL_HANDLE) {
      vkDestroyBuffer(m_device, slot.stagingBuffer, nullptr);
    }
    if (slot.stagingMemory != VK_NULL_HANDLE) {
      vkFreeMemory(m_device, slot.stagingMemory, nullptr);
    }
    slot.stagingBuffer = newBuffer;
    slot.stagingMemory = newMemory;
    slot.stagingBufferSize = requiredSize;
    slot.stagingAllocationSize = requirements.size;
    slot.stagingMapped = newMapped;
    slot.stagingHostCoherent =
        (memoryFlags & VK_MEMORY_PROPERTY_HOST_COHERENT_BIT) != 0;
    m_stagingBuffer = slot.stagingBuffer;
    m_stagingMemory = slot.stagingMemory;
    m_stagingMapped = slot.stagingMapped;
    return true;
  }

  bool invalidateSlotForHostRead(FrameSlot &slot) {
    if (slot.stagingHostCoherent || slot.stagingMemory == VK_NULL_HANDLE) {
      return true;
    }
    VkMappedMemoryRange range{};
    range.sType = VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE;
    range.memory = slot.stagingMemory;
    range.offset = 0;
    range.size = slot.stagingAllocationSize;
    return vkInvalidateMappedMemoryRanges(m_device, 1, &range) == VK_SUCCESS;
  }

  bool flushActiveStagingWrite(VkDeviceSize offset, VkDeviceSize size) {
    if (m_activeSlotIndex < 0 || m_activeSlotIndex >= m_frameSlots.size()) {
      return false;
    }
    FrameSlot &slot = m_frameSlots[m_activeSlotIndex];
    if (!activeStagingRangeAvailable(offset, size)) {
      return false;
    }
    if (slot.stagingHostCoherent || slot.stagingMemory == VK_NULL_HANDLE ||
        size == 0) {
      return true;
    }
    const auto flushRange = alignedVulkanStagingFlushRange(
        static_cast<std::uint64_t>(offset),
        static_cast<std::uint64_t>(size),
        static_cast<std::uint64_t>(slot.stagingAllocationSize),
        static_cast<std::uint64_t>(m_nonCoherentAtomSize));
    if (!flushRange.has_value()) {
      return false;
    }
    VkMappedMemoryRange range{};
    range.sType = VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE;
    range.memory = slot.stagingMemory;
    range.offset = static_cast<VkDeviceSize>(flushRange->offset);
    range.size = static_cast<VkDeviceSize>(flushRange->size);
    return vkFlushMappedMemoryRanges(m_device, 1, &range) == VK_SUCCESS;
  }

  void useSlot(int slotIndex) {
    FrameSlot &slot = m_frameSlots[slotIndex];
    m_activeSlotIndex = slotIndex;
    m_commandBuffer = slot.commandBuffer;
    m_stagingBuffer = slot.stagingBuffer;
    m_stagingMemory = slot.stagingMemory;
    m_stagingMapped = slot.stagingMapped;
    m_submitFence = slot.fence;
  }

  bool selectNextSlot() {
    if (m_frameSlots.isEmpty()) {
      return false;
    }
    const int next =
        (m_activeSlotIndex + 1 + m_frameSlots.size()) % m_frameSlots.size();
    // Staging memory/fences are per-slot. Render targets are still
    // queue-ordered shared images.
    if (!waitSlot(next)) {
      return false;
    }
    useSlot(next);
    return true;
  }

