// Layer composition command recording methods.
// Included inside OffscreenVulkanRendererPrivate; do not compile separately.
  QImage renderFrameFromLayers(const QVector<LayerInput> &layers,
                               const VulkanTextInputs &textInputs,
                               bool readbackToImage,
                               OffscreenVulkanFrame *gpuPreviewFrame = nullptr,
                               QString *gpuPreviewError = nullptr,
                               QString *failureReason = nullptr) {
    if (!m_initialized || m_device == VK_NULL_HANDLE ||
        m_commandBuffer == VK_NULL_HANDLE) {
      return QImage();
    }
    if (!selectNextSlot()) {
      return QImage();
    }
    m_lastHardwareSourceImportCount = 0;
    m_lastHardwareSourceReuseCount = 0;
    const size_t activeTextFrameSlot =
        static_cast<size_t>(qMax(0, m_activeSlotIndex));
    const size_t textFrameSlotCount =
        static_cast<size_t>(qMax(1, m_frameSlots.size()));
    if (m_transcriptTextRenderer &&
        m_transcriptTextRenderer->isReady() &&
        !m_transcriptTextRenderer->beginFrameUploads(activeTextFrameSlot,
                                                     textFrameSlotCount)) {
      if (failureReason) {
        *failureReason = QStringLiteral(
            "Vulkan export text renderer failed to begin transcript/title "
            "atlas uploads for frame slot %1/%2.")
            .arg(activeTextFrameSlot)
            .arg(textFrameSlotCount);
      }
      return QImage();
    }
    if (m_speakerTextRenderer &&
        m_speakerTextRenderer->isReady() &&
        !m_speakerTextRenderer->beginFrameUploads(activeTextFrameSlot,
                                                  textFrameSlotCount)) {
      if (failureReason) {
        *failureReason = QStringLiteral(
            "Vulkan export text renderer failed to begin speaker-label atlas "
            "uploads for frame slot %1/%2.")
            .arg(activeTextFrameSlot)
            .arg(textFrameSlotCount);
      }
      return QImage();
    }

    const auto rgbaBytesForSize = [](const QSize &size) -> VkDeviceSize {
      if (!size.isValid() || size.isEmpty()) {
        return 0;
      }
      return static_cast<VkDeviceSize>(size.width()) *
          static_cast<VkDeviceSize>(size.height()) * 4;
    };
    const VkDeviceSize layerImageBytes = rgbaBytesForSize(m_outputSize);
    VkDeviceSize maxAuxiliaryImageBytes = layerImageBytes;
    VkDeviceSize maxCorrectionStorageBytes = sizeof(float) * 4;
    for (const LayerInput &layer : layers) {
      if (layer.maskTextureEnabled && layer.maskBuffer) {
        maxAuxiliaryImageBytes = qMax(
            maxAuxiliaryImageBytes,
            static_cast<VkDeviceSize>(layer.maskBuffer->size.width) *
                static_cast<VkDeviceSize>(layer.maskBuffer->size.height) * 4);
      }
      if (layer.differenceMatteEnabled &&
          !layer.differenceReferenceFrame.hasHardwareFrame()) {
        maxAuxiliaryImageBytes = qMax(
            maxAuxiliaryImageBytes,
            rgbaBytesForSize(layer.differenceReferenceFrame.size()));
      }
      maxCorrectionStorageBytes = qMax(
          maxCorrectionStorageBytes,
          static_cast<VkDeviceSize>(layer.maskCorrectionStorage.size()));
    }
    constexpr VkDeviceSize maxDeviceSize =
        std::numeric_limits<VkDeviceSize>::max();
    const VkDeviceSize curveBytes = kCurveLutBytes * 2;
    if (layerImageBytes > maxDeviceSize - curveBytes ||
        maxAuxiliaryImageBytes >
            maxDeviceSize - layerImageBytes - curveBytes) {
      return QImage();
    }
    const VkDeviceSize alignment = qMax<VkDeviceSize>(
        16, m_storageBufferOffsetAlignment);
    const VkDeviceSize unalignedCorrectionStorageOffset =
        layerImageBytes + curveBytes + maxAuxiliaryImageBytes;
    if (unalignedCorrectionStorageOffset >
        maxDeviceSize - (alignment - 1)) {
      return QImage();
    }
    const VkDeviceSize correctionStorageOffsetWithinLayer =
        ((unalignedCorrectionStorageOffset + alignment - 1) / alignment) *
        alignment;
    if (maxCorrectionStorageBytes >
        maxDeviceSize - correctionStorageOffsetWithinLayer) {
      return QImage();
    }
    const VkDeviceSize unalignedLayerStagingSize =
        correctionStorageOffsetWithinLayer + maxCorrectionStorageBytes;
    if (unalignedLayerStagingSize > maxDeviceSize - (alignment - 1)) {
      return QImage();
    }
    const VkDeviceSize layerStagingSize =
        ((unalignedLayerStagingSize + alignment - 1) / alignment) *
        alignment;
    const VkDeviceSize stagingLayerCount = static_cast<VkDeviceSize>(
        qMin(kMaxLayerTextures,
             qMax(1, static_cast<int>(layers.size()))));
    if (layerStagingSize > maxDeviceSize / stagingLayerCount) {
      return QImage();
    }
    const VkDeviceSize requiredStagingBytes =
        layerStagingSize * stagingLayerCount;
    if (!ensureActiveStagingCapacity(requiredStagingBytes)) {
      qWarning().noquote()
          << QStringLiteral(
                 "[vulkan-compose] unable to provide %1 bytes of per-frame "
                 "staging for raw GPU auxiliary preprocessing")
                 .arg(static_cast<qulonglong>(requiredStagingBytes));
      return QImage();
    }

    vkResetCommandBuffer(m_commandBuffer, 0);

    VkCommandBufferBeginInfo beginInfo{};
    beginInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    if (vkBeginCommandBuffer(m_commandBuffer, &beginInfo) != VK_SUCCESS) {
      return QImage();
    }

    struct Push {
      float mvp[16];
      float brightness;
      float contrast;
      float saturation;
      float opacity;
      float shadows[4];
      float midtones[4];
      float highlights[4];
    } push{};
    auto updateFrameUniformForDraw =
        [&](const LayerInput* layer = nullptr,
            const float* effectParams = nullptr,
            const float* effectDomain = nullptr,
            const float* effectMaskDomain = nullptr) -> uint32_t {
      if (!m_frameUniformMapped || m_frameUniformStride == 0) {
        return 0u;
      }
      FrameUniformData values;
      values.outputSizeAndInverse[0] = static_cast<float>(qMax(1, m_outputSize.width()));
      values.outputSizeAndInverse[1] = static_cast<float>(qMax(1, m_outputSize.height()));
      values.outputSizeAndInverse[2] = 1.0f / values.outputSizeAndInverse[0];
      values.outputSizeAndInverse[3] = 1.0f / values.outputSizeAndInverse[1];
      if (layer) {
        std::memcpy(values.backgroundShadows,
                    layer->backgroundShadows,
                    sizeof(values.backgroundShadows));
        std::memcpy(values.backgroundMidtones,
                    layer->backgroundMidtones,
                    sizeof(values.backgroundMidtones));
        std::memcpy(values.backgroundHighlights,
                    layer->backgroundHighlights,
                    sizeof(values.backgroundHighlights));
        std::memcpy(values.backgroundGrade,
                    layer->backgroundGrade,
                    sizeof(values.backgroundGrade));
      }
      if (effectParams) {
        std::memcpy(values.effectParams, effectParams, sizeof(values.effectParams));
      }
      if (effectDomain) {
        std::memcpy(values.effectDomain, effectDomain, sizeof(values.effectDomain));
      }
      if (effectMaskDomain) {
        std::memcpy(values.effectMaskDomain,
                    effectMaskDomain,
                    sizeof(values.effectMaskDomain));
      }
      const VkDeviceSize offset =
          m_frameUniformStride * static_cast<VkDeviceSize>(m_frameUniformRingIndex);
      std::memcpy(static_cast<char*>(m_frameUniformMapped) + offset,
                  &values,
                  sizeof(values));
      m_frameUniformRingIndex = (m_frameUniformRingIndex + 1) % kFrameUniformRingCount;
      return static_cast<uint32_t>(offset);
    };
    transitionImageLayout(m_commandBuffer, m_colorImage, m_colorImageLayout,
                          VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
    m_colorImageLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    VkClearColorValue clearColor{};
    clearColor.float32[0] = 0.0f;
    clearColor.float32[1] = 0.0f;
    clearColor.float32[2] = 0.0f;
    clearColor.float32[3] = 1.0f;
    VkImageSubresourceRange clearRange{};
    clearRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    clearRange.baseMipLevel = 0;
    clearRange.levelCount = 1;
    clearRange.baseArrayLayer = 0;
    clearRange.layerCount = 1;
    vkCmdClearColorImage(m_commandBuffer, m_colorImage,
                         VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &clearColor, 1,
                         &clearRange);
    transitionImageLayout(m_commandBuffer, m_colorImage,
                          VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                          VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
    m_colorImageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;

    VkClearValue clearValue{};
    clearValue.color = {{0.0f, 0.0f, 0.0f, 1.0f}};
    VkRenderPassBeginInfo renderPassBeginInfo{};
    renderPassBeginInfo.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
    renderPassBeginInfo.renderPass = m_renderPass;
    renderPassBeginInfo.framebuffer = m_framebuffer;
    renderPassBeginInfo.renderArea.offset = {0, 0};
    renderPassBeginInfo.renderArea.extent = {
        static_cast<uint32_t>(m_outputSize.width()),
        static_cast<uint32_t>(m_outputSize.height())};
    renderPassBeginInfo.clearValueCount = 1;
    renderPassBeginInfo.pClearValues = &clearValue;
    VkViewport fullViewport{};
    fullViewport.x = 0.0f;
    fullViewport.y = 0.0f;
    fullViewport.width = static_cast<float>(m_outputSize.width());
    fullViewport.height = static_cast<float>(m_outputSize.height());
    fullViewport.minDepth = 0.0f;
    fullViewport.maxDepth = 1.0f;
    VkRect2D fullScissor{};
    fullScissor.offset = {0, 0};
    fullScissor.extent = {static_cast<uint32_t>(m_outputSize.width()),
                          static_cast<uint32_t>(m_outputSize.height())};
    auto layerHasRenderableSource = [](const LayerInput &layer) {
      return !layer.overlayImage.isNull() || !layer.image.isNull() ||
             !layer.frame.isNull();
    };
    auto updateLayerDescriptorSet = [&](LayerTextureSlot &slot,
                                        VkImageView sourceView,
                                        VkImageLayout sourceLayout,
                                        VkImageView auxiliaryView = VK_NULL_HANDLE,
                                        VkImageLayout auxiliaryLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL) {
      VkDescriptorImageInfo di[4]{};
      di[0].imageLayout = sourceLayout;
      di[0].imageView = sourceView;
      di[0].sampler = m_sampler;
      di[1].imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
      di[1].imageView = slot.curveLutView;
      di[1].sampler = m_sampler;
      di[2].imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
      di[2].imageView = auxiliaryView != VK_NULL_HANDLE ? auxiliaryView : slot.maskView;
      di[2].imageLayout = auxiliaryView != VK_NULL_HANDLE ? auxiliaryLayout : VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
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
    };
    struct PreparedGpuSource {
      VkImageView view = VK_NULL_HANDLE;
      VkImageLayout layout = VK_IMAGE_LAYOUT_UNDEFINED;
      int ownerSlotIndex = -1;
    };
    QHash<QString, PreparedGpuSource> preparedGpuSources;
    auto prepareLayerSource =
        [&](int slotIndex, LayerTextureSlot &slot, const LayerInput &layer,
            VkDeviceSize stagingOffset, VkImageView *sourceViewOut,
            VkImageLayout *sourceLayoutOut) -> bool {
      if (sourceViewOut) {
        *sourceViewOut = VK_NULL_HANDLE;
      }
      if (sourceLayoutOut) {
        *sourceLayoutOut = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
      }
      if (layer.preferHardwareDirect && !layer.frame.isNull() &&
          layer.frame.hasHardwareFrame()) {
        const QString sourceKey = vulkanSourceFrameCacheKey(
            layer.mediaOwnerClipId, layer.frame);
        const auto preparedSource =
            preparedGpuSources.constFind(sourceKey);
        if (!sourceKey.isEmpty() &&
            preparedSource != preparedGpuSources.cend()) {
          ++m_lastHardwareSourceReuseCount;
          if (sourceViewOut) {
            *sourceViewOut = preparedSource->view;
          }
          if (sourceLayoutOut) {
            *sourceLayoutOut = preparedSource->layout;
          }
          return preparedSource->view != VK_NULL_HANDLE;
        }
        for (auto cached = preparedGpuSources.begin();
             cached != preparedGpuSources.end();) {
          if (cached->ownerSlotIndex == slotIndex) {
            cached = preparedGpuSources.erase(cached);
          } else {
            ++cached;
          }
        }
        if (!slot.hardwareFrameHandoff) {
          auto handoff = std::make_shared<
              jcut::vulkan_detector::VulkanDetectorFrameHandoff>();
          jcut::vulkan_detector::VulkanDeviceContext context;
          context.physicalDevice = m_physicalDevice;
          context.device = m_device;
          context.queue = m_graphicsQueue;
          context.queueFamilyIndex = m_graphicsQueueFamily;
          std::string handoffError;
          if (!handoff->initialize(context, &handoffError)) {
            qWarning().noquote()
                << QStringLiteral("[vulkan-compose] hardware frame handoff "
                                  "initialization failed: %1")
                       .arg(QString::fromStdString(handoffError));
          } else {
            slot.hardwareFrameHandoff = handoff;
          }
        }
        if (slot.hardwareFrameHandoff) {
          std::string uploadError;
          if (slot.hardwareFrameHandoff->uploadFrame(layer.frame, false,
                                                     nullptr, &uploadError)) {
            ++m_lastHardwareSourceImportCount;
            const auto external = slot.hardwareFrameHandoff->externalImage();
            if (!sourceKey.isEmpty() &&
                external.imageView != VK_NULL_HANDLE) {
              preparedGpuSources.insert(
                  sourceKey,
                  PreparedGpuSource{
                      external.imageView,
                      external.imageLayout,
                      slotIndex});
            }
            if (sourceViewOut) {
              *sourceViewOut = external.imageView;
            }
            if (sourceLayoutOut) {
              *sourceLayoutOut = external.imageLayout;
            }
            return external.imageView != VK_NULL_HANDLE;
          }
          qWarning().noquote()
              << QStringLiteral(
                     "[vulkan-compose] hardware frame handoff failed and CPU "
                     "image fallback is disabled: %1")
                     .arg(QString::fromStdString(uploadError));
          return false;
        }
      }
      QImage rgba;
      if (!layer.cacheKey.isEmpty()) {
        rgba = m_preparedImageCache.value(layer.cacheKey);
      }
      if (rgba.isNull()) {
        if (!layer.overlayImage.isNull()) {
          if (layer.overlayImage.width == m_outputSize.width() &&
              layer.overlayImage.height == m_outputSize.height() &&
              m_stagingMapped) {
            if (!writeOverlayImageToStagingTopLeft(layer.overlayImage, stagingOffset)) {
              return false;
            }
            transitionImageLayout(m_commandBuffer, slot.image,
                                  slot.uploaded
                                      ? VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL
                                      : VK_IMAGE_LAYOUT_UNDEFINED,
                                  VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
            VkBufferImageCopy uploadRegion{};
            uploadRegion.bufferOffset = stagingOffset;
            uploadRegion.bufferRowLength = 0;
            uploadRegion.bufferImageHeight = 0;
            uploadRegion.imageSubresource.aspectMask =
                VK_IMAGE_ASPECT_COLOR_BIT;
            uploadRegion.imageSubresource.mipLevel = 0;
            uploadRegion.imageSubresource.baseArrayLayer = 0;
            uploadRegion.imageSubresource.layerCount = 1;
            uploadRegion.imageExtent = {
                static_cast<uint32_t>(m_outputSize.width()),
                static_cast<uint32_t>(m_outputSize.height()), 1};
            vkCmdCopyBufferToImage(m_commandBuffer, m_stagingBuffer, slot.image,
                                   VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1,
                                   &uploadRegion);
            transitionImageLayout(m_commandBuffer, slot.image,
                                  VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                                  VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
            slot.uploaded = true;
            if (sourceViewOut) {
              *sourceViewOut = slot.view;
            }
            if (sourceLayoutOut) {
              *sourceLayoutOut = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
            }
            return true;
          }
          const OverlayImage scaledOverlay =
              scaledOverlayImage(layer.overlayImage, m_outputSize);
          if (!scaledOverlay.isNull() && m_stagingMapped) {
            if (!writeOverlayImageToStagingTopLeft(scaledOverlay, stagingOffset)) {
              return false;
            }
            transitionImageLayout(m_commandBuffer, slot.image,
                                  slot.uploaded
                                      ? VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL
                                      : VK_IMAGE_LAYOUT_UNDEFINED,
                                  VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
            VkBufferImageCopy uploadRegion{};
            uploadRegion.bufferOffset = stagingOffset;
            uploadRegion.bufferRowLength = 0;
            uploadRegion.bufferImageHeight = 0;
            uploadRegion.imageSubresource.aspectMask =
                VK_IMAGE_ASPECT_COLOR_BIT;
            uploadRegion.imageSubresource.mipLevel = 0;
            uploadRegion.imageSubresource.baseArrayLayer = 0;
            uploadRegion.imageSubresource.layerCount = 1;
            uploadRegion.imageExtent = {
                static_cast<uint32_t>(m_outputSize.width()),
                static_cast<uint32_t>(m_outputSize.height()), 1};
            vkCmdCopyBufferToImage(m_commandBuffer, m_stagingBuffer, slot.image,
                                   VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1,
                                   &uploadRegion);
            transitionImageLayout(m_commandBuffer, slot.image,
                                  VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                                  VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
            slot.uploaded = true;
            if (sourceViewOut) {
              *sourceViewOut = slot.view;
            }
            if (sourceLayoutOut) {
              *sourceLayoutOut = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
            }
            return true;
          }
        }
      }
      if (rgba.isNull()) {
        if (!layer.image.isNull()) {
          rgba = layer.image;
        } else if (!layer.frame.isNull() &&
                   (layer.frame.hasHardwareFrame() || layer.frame.hasGpuTexture())) {
          return false;
        } else {
          rgba = frameHandleToCpuImage(layer.frame);
        }
        if (rgba.format() != QImage::Format_RGBA8888) {
          rgba = rgba.convertToFormat(QImage::Format_RGBA8888);
        }
        if (rgba.size() != m_outputSize) {
          rgba = rgba.scaled(m_outputSize, Qt::IgnoreAspectRatio,
                             Qt::SmoothTransformation);
        }
        if (!layer.cacheKey.isEmpty()) {
          m_preparedImageCache.insert(layer.cacheKey, rgba);
        }
      }
      if (rgba.isNull() || !m_stagingMapped) {
        return false;
      }
      if (!writeRgbaImageToStagingTopLeft(rgba, stagingOffset)) {
        return false;
      }
      transitionImageLayout(m_commandBuffer, slot.image,
                            slot.uploaded
                                ? VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL
                                : VK_IMAGE_LAYOUT_UNDEFINED,
                            VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
      VkBufferImageCopy uploadRegion{};
      uploadRegion.bufferOffset = stagingOffset;
      uploadRegion.bufferRowLength = 0;
      uploadRegion.bufferImageHeight = 0;
      uploadRegion.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
      uploadRegion.imageSubresource.mipLevel = 0;
      uploadRegion.imageSubresource.baseArrayLayer = 0;
      uploadRegion.imageSubresource.layerCount = 1;
      uploadRegion.imageExtent = {static_cast<uint32_t>(m_outputSize.width()),
                                  static_cast<uint32_t>(m_outputSize.height()),
                                  1};
      vkCmdCopyBufferToImage(m_commandBuffer, m_stagingBuffer, slot.image,
                             VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1,
                             &uploadRegion);
      transitionImageLayout(m_commandBuffer, slot.image,
                            VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                            VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
      slot.uploaded = true;
      if (sourceViewOut) {
        *sourceViewOut = slot.view;
      }
      if (sourceLayoutOut) {
        *sourceLayoutOut = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
      }
      return true;
    };
    int layerIndex = 0;
    struct PreparedGpuMask {
      VkImageView view = VK_NULL_HANDLE;
      VkImageLayout layout = VK_IMAGE_LAYOUT_UNDEFINED;
    };
    QHash<QString, PreparedGpuMask> preparedGpuMasks;
    while (layerIndex < layers.size()) {
      const int batchCount =
          qMin(kMaxLayerTextures, layers.size() - layerIndex);
      struct PreparedBatchLayer {
        VkImageView view = VK_NULL_HANDLE;
        VkImageLayout layout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        VkImageView auxiliaryView = VK_NULL_HANDLE;
        VkImageLayout auxiliaryLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
      };
      QVector<PreparedBatchLayer> preparedLayers(batchCount);
      for (int i = 0; i < batchCount; ++i) {
        const LayerInput &layer = layers.at(layerIndex + i);
        if (!layerHasRenderableSource(layer)) {
          continue;
        }
        LayerTextureSlot &slot = m_layerSlots[i];
        const VkDeviceSize stagingOffset = layerStagingSize * i;
        if (!prepareLayerSource(i, slot, layer, stagingOffset,
                                &preparedLayers[i].view,
                                &preparedLayers[i].layout)) {
          vkEndCommandBuffer(m_commandBuffer);
          return QImage();
        }
        if (layer.differenceMatteEnabled && layer.differenceReferenceFrame.hasHardwareFrame()) {
          if (!slot.referenceFrameHandoff) {
            auto handoff = std::make_shared<jcut::vulkan_detector::VulkanDetectorFrameHandoff>();
            jcut::vulkan_detector::VulkanDeviceContext context;
            context.physicalDevice = m_physicalDevice;
            context.device = m_device;
            context.queue = m_graphicsQueue;
            context.queueFamilyIndex = m_graphicsQueueFamily;
            std::string error;
            if (handoff->initialize(context, &error)) {
              slot.referenceFrameHandoff = handoff;
            } else {
              qWarning().noquote() << QStringLiteral("[vulkan-compose] difference reference handoff initialization failed: %1")
                                         .arg(QString::fromStdString(error));
            }
          }
          if (!slot.referenceFrameHandoff) {
            vkEndCommandBuffer(m_commandBuffer);
            return QImage();
          }
          std::string error;
          if (!slot.referenceFrameHandoff->uploadFrame(layer.differenceReferenceFrame, false, nullptr, &error)) {
            qWarning().noquote() << QStringLiteral("[vulkan-compose] difference reference handoff failed: %1")
                                       .arg(QString::fromStdString(error));
            vkEndCommandBuffer(m_commandBuffer);
            return QImage();
          }
          const auto external = slot.referenceFrameHandoff->externalImage();
          preparedLayers[i].auxiliaryView = external.imageView;
          preparedLayers[i].auxiliaryLayout = external.imageLayout;
        }

        const QByteArray curveBytes =
            layer.gradePayload.curveLutRgba.size() ==
                    static_cast<int>(kCurveLutBytes)
                ? layer.gradePayload.curveLutRgba
                : identityCurveLutBytes();
        if (!m_stagingMapped) {
          vkEndCommandBuffer(m_commandBuffer);
          return QImage();
        }
        const VkDeviceSize curveStagingOffset = stagingOffset + layerImageBytes;
        if (!activeStagingRangeAvailable(
                curveStagingOffset, kCurveLutBytes)) {
          vkEndCommandBuffer(m_commandBuffer);
          return QImage();
        }
        std::memcpy(
            reinterpret_cast<uint8_t *>(m_stagingMapped) + curveStagingOffset,
            curveBytes.constData(), static_cast<size_t>(kCurveLutBytes));
        if (!flushActiveStagingWrite(curveStagingOffset, kCurveLutBytes)) {
          vkEndCommandBuffer(m_commandBuffer);
          return QImage();
        }
        transitionImageLayout(m_commandBuffer, slot.curveLutImage,
                              slot.curveUploaded
                                  ? VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL
                                  : VK_IMAGE_LAYOUT_UNDEFINED,
                              VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
        VkBufferImageCopy curveUploadRegion{};
        curveUploadRegion.bufferOffset = curveStagingOffset;
        curveUploadRegion.bufferRowLength = 0;
        curveUploadRegion.bufferImageHeight = 0;
        curveUploadRegion.imageSubresource.aspectMask =
            VK_IMAGE_ASPECT_COLOR_BIT;
        curveUploadRegion.imageSubresource.mipLevel = 0;
        curveUploadRegion.imageSubresource.baseArrayLayer = 0;
        curveUploadRegion.imageSubresource.layerCount = 1;
        curveUploadRegion.imageExtent = {static_cast<uint32_t>(kCurveLutWidth),
                                         static_cast<uint32_t>(kCurveLutHeight),
                                         1};
        vkCmdCopyBufferToImage(
            m_commandBuffer, m_stagingBuffer, slot.curveLutImage,
            VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &curveUploadRegion);
        transitionImageLayout(m_commandBuffer, slot.curveLutImage,
                              VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                              VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
        slot.curveUploaded = true;

        const QByteArray maskCurveBytes =
            layer.maskGradePayload.curveLutRgba.size() ==
                    static_cast<int>(kCurveLutBytes)
                ? layer.maskGradePayload.curveLutRgba
                : identityCurveLutBytes();
        const VkDeviceSize maskCurveStagingOffset =
            stagingOffset + layerImageBytes + kCurveLutBytes;
        if (!activeStagingRangeAvailable(
                maskCurveStagingOffset, kCurveLutBytes)) {
          vkEndCommandBuffer(m_commandBuffer);
          return QImage();
        }
        std::memcpy(
            reinterpret_cast<uint8_t *>(m_stagingMapped) + maskCurveStagingOffset,
            maskCurveBytes.constData(), static_cast<size_t>(kCurveLutBytes));
        if (!flushActiveStagingWrite(maskCurveStagingOffset, kCurveLutBytes)) {
          vkEndCommandBuffer(m_commandBuffer);
          return QImage();
        }
        transitionImageLayout(m_commandBuffer, slot.maskCurveLutImage,
                              slot.maskCurveUploaded
                                  ? VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL
                                  : VK_IMAGE_LAYOUT_UNDEFINED,
                              VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
        VkBufferImageCopy maskCurveUploadRegion{};
        maskCurveUploadRegion.bufferOffset = maskCurveStagingOffset;
        maskCurveUploadRegion.bufferRowLength = 0;
        maskCurveUploadRegion.bufferImageHeight = 0;
        maskCurveUploadRegion.imageSubresource.aspectMask =
            VK_IMAGE_ASPECT_COLOR_BIT;
        maskCurveUploadRegion.imageSubresource.mipLevel = 0;
        maskCurveUploadRegion.imageSubresource.baseArrayLayer = 0;
        maskCurveUploadRegion.imageSubresource.layerCount = 1;
        maskCurveUploadRegion.imageExtent = {static_cast<uint32_t>(kCurveLutWidth),
                                             static_cast<uint32_t>(kCurveLutHeight),
                                             1};
        vkCmdCopyBufferToImage(
            m_commandBuffer, m_stagingBuffer, slot.maskCurveLutImage,
            VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &maskCurveUploadRegion);
        transitionImageLayout(m_commandBuffer, slot.maskCurveLutImage,
                              VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                              VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
        slot.maskCurveUploaded = true;
        if (layer.differenceMatteEnabled && !layer.differenceReferenceFrame.hasHardwareFrame()) {
          QImage reference = layer.differenceReferenceFrame.hasCpuImage()
              ? layer.differenceReferenceFrame.cpuImage()
              : frameHandleToCpuImage(layer.differenceReferenceFrame);
          if (reference.isNull()) {
            vkEndCommandBuffer(m_commandBuffer);
            return QImage();
          }
          reference = reference.convertToFormat(QImage::Format_RGBA8888);
          if (!ensureMaskRawImage(slot, reference.size())) {
            vkEndCommandBuffer(m_commandBuffer);
            return QImage();
          }
          const VkDeviceSize referenceOffset = stagingOffset + layerImageBytes + (kCurveLutBytes * 2);
          if (!writeRgbaImageToStagingTopLeft(reference, referenceOffset)) {
            vkEndCommandBuffer(m_commandBuffer);
            return QImage();
          }
          transitionImageLayout(m_commandBuffer, slot.maskRawImage,
                                slot.maskRawLayout,
                                VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
          slot.maskRawLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
          VkBufferImageCopy region{};
          region.bufferOffset = referenceOffset;
          region.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
          region.imageSubresource.layerCount = 1;
          region.imageExtent = {static_cast<uint32_t>(reference.width()),
                                static_cast<uint32_t>(reference.height()), 1};
          vkCmdCopyBufferToImage(m_commandBuffer, m_stagingBuffer,
                                 slot.maskRawImage,
                                 VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);
          transitionImageLayout(m_commandBuffer, slot.maskRawImage,
                                VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                                VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
          slot.maskRawLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
          preparedLayers[i].auxiliaryView = slot.maskRawView;
        } else if (layer.maskTextureEnabled && layer.maskBuffer) {
          const jcut::core::ImageBuffer &maskUpload = *layer.maskBuffer;
          if (maskUpload.empty() ||
              maskUpload.format != jcut::core::PixelFormat::Gray8) {
            vkEndCommandBuffer(m_commandBuffer);
            return QImage();
          }
          const VulkanMaskPreprocessOptions maskOptions =
              maskPreprocessOptions(layer);
          const QString maskKey =
              vulkanMaskTextureCacheKey(maskOptions, m_outputSize);
          const auto preparedMask = preparedGpuMasks.constFind(maskKey);
          if (!maskKey.isEmpty() &&
              preparedMask != preparedGpuMasks.cend()) {
            preparedLayers[i].auxiliaryView = preparedMask->view;
            preparedLayers[i].auxiliaryLayout = preparedMask->layout;
          } else {
            for (auto cached = preparedGpuMasks.begin();
                 cached != preparedGpuMasks.end();) {
              if (cached->view == slot.maskView) {
                cached = preparedGpuMasks.erase(cached);
              } else {
                ++cached;
              }
            }
            const QSize maskSize(
                maskUpload.size.width, maskUpload.size.height);
            if (!ensureMaskRawImage(
                    slot, maskSize, VK_FORMAT_R8G8B8A8_UNORM)) {
              vkEndCommandBuffer(m_commandBuffer);
              return QImage();
            }
            const QByteArray packedTemporalMask =
                packVulkanTemporalMaskChannels(
                    maskUpload,
                    layer.previousMaskBuffer.get(),
                    layer.nextMaskBuffer.get());
            const VkDeviceSize maskStagingOffset =
                stagingOffset + layerImageBytes + (kCurveLutBytes * 2);
            if (!writePackedTemporalMaskToStagingTopLeft(
                    packedTemporalMask, maskSize, maskStagingOffset)) {
              vkEndCommandBuffer(m_commandBuffer);
              return QImage();
            }
            transitionImageLayout(
                m_commandBuffer,
                slot.maskRawImage,
                slot.maskRawLayout,
                VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
            slot.maskRawLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
            VkBufferImageCopy maskUploadRegion{};
            maskUploadRegion.bufferOffset = maskStagingOffset;
            maskUploadRegion.bufferRowLength = 0;
            maskUploadRegion.bufferImageHeight = 0;
            maskUploadRegion.imageSubresource.aspectMask =
                VK_IMAGE_ASPECT_COLOR_BIT;
            maskUploadRegion.imageSubresource.mipLevel = 0;
            maskUploadRegion.imageSubresource.baseArrayLayer = 0;
            maskUploadRegion.imageSubresource.layerCount = 1;
            maskUploadRegion.imageExtent = {
                static_cast<uint32_t>(maskSize.width()),
                static_cast<uint32_t>(maskSize.height()), 1};
            vkCmdCopyBufferToImage(
                m_commandBuffer,
                m_stagingBuffer,
                slot.maskRawImage,
                VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                1,
                &maskUploadRegion);
            transitionImageLayout(
                m_commandBuffer,
                slot.maskRawImage,
                VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
            slot.maskRawLayout =
                VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
            LayerInput maskLayer = layer;
            maskLayer.maskSourceSize = maskSize;
            const VkDeviceSize correctionStorageOffset =
                stagingOffset + correctionStorageOffsetWithinLayer;
            if (!preprocessLayerMask(
                    slot,
                    maskLayer,
                    correctionStorageOffset,
                    maxCorrectionStorageBytes)) {
              vkEndCommandBuffer(m_commandBuffer);
              return QImage();
            }
            if (!maskKey.isEmpty() &&
                slot.maskView != VK_NULL_HANDLE &&
                slot.maskLayout ==
                    VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL) {
              preparedGpuMasks.insert(
                  maskKey,
                  PreparedGpuMask{slot.maskView, slot.maskLayout});
            }
          }
        } else if (!slot.maskUploaded) {
          transitionImageLayout(m_commandBuffer, slot.maskImage,
                                slot.maskLayout,
                                VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
          slot.maskLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
          VkClearColorValue whiteMaskClear{};
          whiteMaskClear.float32[0] = 1.0f;
          whiteMaskClear.float32[1] = 1.0f;
          whiteMaskClear.float32[2] = 1.0f;
          whiteMaskClear.float32[3] = 1.0f;
          VkImageSubresourceRange whiteMaskRange{};
          whiteMaskRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
          whiteMaskRange.baseMipLevel = 0;
          whiteMaskRange.levelCount = 1;
          whiteMaskRange.baseArrayLayer = 0;
          whiteMaskRange.layerCount = 1;
          vkCmdClearColorImage(
              m_commandBuffer, slot.maskImage,
              VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &whiteMaskClear, 1,
              &whiteMaskRange);
          transitionImageLayout(m_commandBuffer, slot.maskImage,
                                VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                                VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
          slot.maskLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
          slot.maskUploaded = true;
        }
        updateLayerDescriptorSet(slot, preparedLayers[i].view,
                                 preparedLayers[i].layout,
                                 preparedLayers[i].auxiliaryView,
                                 preparedLayers[i].auxiliaryLayout);
      }

      vkCmdBeginRenderPass(m_commandBuffer, &renderPassBeginInfo,
                           VK_SUBPASS_CONTENTS_INLINE);
      vkCmdBindPipeline(m_commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS,
                        m_effectsPipeline);
      vkCmdSetViewport(m_commandBuffer, 0, 1, &fullViewport);
      vkCmdSetScissor(m_commandBuffer, 0, 1, &fullScissor);
      for (int i = 0; i < batchCount; ++i) {
        const LayerInput &layer = layers.at(layerIndex + i);
        if (!layerHasRenderableSource(layer)) {
          continue;
        }
        LayerTextureSlot &slot = m_layerSlots[i];
        auto drawLayerWithMvp = [&](const float drawMvp[16],
                                    float brightness,
                                    float contrast,
                                    float saturation,
                                    float opacity,
                                    const float shadows[4],
                                    const float midtones[4],
                                    const float highlights[4],
                                    float mode,
                                    const float* effectParams = nullptr,
                                    const float* effectDomain = nullptr,
                                    const float* effectMaskDomain = nullptr) {
          const uint32_t frameUniformOffset =
              updateFrameUniformForDraw(&layer,
                                        effectParams,
                                        effectDomain,
                                        effectMaskDomain);
          vkCmdBindDescriptorSets(
              m_commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS,
              m_effectsPipelineLayout, 0, 1, &slot.descriptorSet, 1, &frameUniformOffset);
          std::memcpy(push.mvp, drawMvp, sizeof(push.mvp));
          push.brightness = brightness;
          push.contrast = contrast;
          push.saturation = saturation;
          push.opacity = qBound(0.0f, opacity, 1.0f);
          push.shadows[0] = shadows[0];
          push.shadows[1] = shadows[1];
          push.shadows[2] = shadows[2];
          push.shadows[3] = mode;
          push.midtones[0] = midtones[0];
          push.midtones[1] = midtones[1];
          push.midtones[2] = midtones[2];
          push.midtones[3] = midtones[3];
          push.highlights[0] = highlights[0];
          push.highlights[1] = highlights[1];
          push.highlights[2] = highlights[2];
          push.highlights[3] = highlights[3];
          vkCmdPushConstants(m_commandBuffer, m_effectsPipelineLayout,
                             VK_SHADER_STAGE_VERTEX_BIT |
                                 VK_SHADER_STAGE_FRAGMENT_BIT,
                             0, sizeof(Push), &push);
          vkCmdDraw(m_commandBuffer, 4, 1, 0, 0);
        };
        auto drawLayer = [&](float brightness,
                             float contrast,
                             float saturation,
                             float opacity,
                             const float shadows[4],
                             const float midtones[4],
                             const float highlights[4],
                             float mode,
                             const float* effectParams = nullptr) {
          drawLayerWithMvp(layer.mvp,
                           brightness,
                           contrast,
                           saturation,
                           opacity,
                           shadows,
                           midtones,
                           highlights,
                           mode,
                           effectParams);
        };
        const float packedMaskFalloff = static_cast<float>(
            layer.maskFeatherFalloff * 10) + layer.maskFeatherGamma;
        const float maskEdgeParams[4] = {
            static_cast<float>(qBound<qreal>(0.0, layer.maskEdgeGrayAmount, 1.0)),
            static_cast<float>(qBound<qreal>(0.001, layer.maskEdgeGrayWidth, 2.0)),
            static_cast<float>(qBound<qreal>(0.1, layer.maskEdgeGrayGamma, 8.0)),
            0.0f};
        const VulkanDrawEffectState& layerEffects =
            layer.gradePayload.effects;
        const VulkanDrawEffectState& maskEffects =
            layer.maskGradePayload.effects;
        float maskHighlights[4];
        std::copy_n(layerEffects.highlights, 4, maskHighlights);
        maskHighlights[3] = packedMaskFalloff;
        if (layer.maskTextureEnabled && layer.maskDropShadowEnabled &&
            layer.maskDropShadowOpacity > 0.0f) {
          float shadowMvp[16];
          std::copy_n(layer.mvp, 16, shadowMvp);
          shadowMvp[12] += 2.0f * layer.maskDropShadowOffsetX /
                           static_cast<float>(std::max(1, m_outputSize.width()));
          shadowMvp[13] += 2.0f * layer.maskDropShadowOffsetY /
                           static_cast<float>(std::max(1, m_outputSize.height()));
          float neutral[4] = {0.0f, 0.0f, 0.0f, 0.0f};
          float shadowMidtones[4] = {0.0f, 0.0f, 0.0f,
                                     static_cast<float>(
                                         layer.maskDropShadowRadius)};
          drawLayerWithMvp(shadowMvp,
                           0.0f,
                           1.0f,
                           1.0f,
                           layerEffects.opacity * layer.maskDropShadowOpacity,
                           neutral,
                           shadowMidtones,
                           maskHighlights,
                           kVulkanEffectModeMaskShadow);
        }
        if (layer.maskTextureEnabled && layer.maskShowOnly) {
          drawLayer(0.0f,
                    1.0f,
                    1.0f,
                    layer.maskOpacity,
                    layerEffects.shadows,
                    layerEffects.midtones,
                    maskHighlights,
                    kVulkanEffectModeMaskOnly,
                    maskEdgeParams);
          continue;
        }
        if (!layer.effectPlan.generatedDraws.isEmpty()) {
          if (layer.presetScissorEnabled) {
            const VkRect2D presetScissor = scissorFromRect(layer.presetScissorRect, m_outputSize);
            vkCmdSetScissor(m_commandBuffer, 0, 1, &presetScissor);
          }
          for (const VulkanEffectPipelinePlan::DrawPass& drawPass : layer.effectPlan.generatedDraws) {
            float presetMvp[16];
            vulkanMvpForOutputRect(
                drawPass.outputRect,
                m_outputSize,
                drawPass.rotationDegrees,
                presetMvp);
            const bool generatedMaskDomainDraw =
                layer.maskClipSource && drawPass.effectDomain[3] < 0.0f;
            const float drawMode =
                layer.maskClipSource && !generatedMaskDomainDraw
                    ? kVulkanEffectModeMaskGrade
                    : drawPass.shaderMode;
            float drawShadows[4];
            float drawMidtones[4];
            float drawHighlights[4];
            std::copy_n(layerEffects.shadows, 4, drawShadows);
            std::copy_n(layerEffects.midtones, 4, drawMidtones);
            std::copy_n(layerEffects.highlights, 4, drawHighlights);
            if (drawMode == kVulkanEffectModeMaskGrade) {
              drawHighlights[3] = packedMaskFalloff;
            }
            if (drawMode >= kVulkanEffectModeSpeakerMaskDilation &&
                drawMode <= kVulkanEffectModeSpeakerMaskDilationRings) {
              std::copy_n(drawPass.palette, 3, drawShadows);
              std::copy_n(drawPass.palette + 3, 3, drawMidtones);
              std::copy_n(drawPass.palette + 6, 3, drawHighlights);
            }
            drawLayerWithMvp(presetMvp,
                             layerEffects.brightness,
                             layerEffects.contrast,
                             layerEffects.saturation,
                             layerEffects.opacity * drawPass.opacityMultiplier *
                                 (drawMode == kVulkanEffectModeMaskGrade && !layer.maskClipSource
                                      ? layer.maskOpacity
                                      : 1.0f),
                             drawShadows,
                             drawMidtones,
                             drawHighlights,
                             drawMode,
                             drawPass.effectParams,
                             drawPass.effectDomain,
                             drawPass.effectMaskDomain);
          }
          if (layer.presetScissorEnabled) {
            vkCmdSetScissor(m_commandBuffer, 0, 1, &fullScissor);
          }
        } else {
          const float drawMode = layer.maskClipSource
                                     ? kVulkanEffectModeMaskGrade
                                     : layerEffects.shadows[3];
          drawLayer(layerEffects.brightness,
                    layerEffects.contrast,
                    layerEffects.saturation,
                    layerEffects.opacity,
                    layerEffects.shadows,
                    layerEffects.midtones,
                    drawMode == kVulkanEffectModeMaskGrade
                        ? maskHighlights
                        : layerEffects.highlights,
                    drawMode,
                    drawMode == kVulkanEffectModeMaskGrade ? maskEdgeParams : nullptr);
        }
        if (layer.maskTextureEnabled && layer.maskGradeEnabled && !layer.maskForegroundLayerEnabled) {
          float neutral[4] = {0.0f, 0.0f, 0.0f, 0.0f};
          float maskMidtones[4] = {0.0f, 0.0f, 0.0f,
                                   layer.maskGradePayload.curveLutApplied
                                       ? kVulkanMaskGradeUseSelectedCurveLut
                                       : 0.0f};
          float maskGradeHighlights[4] = {0.0f, 0.0f, 0.0f,
                                          packedMaskFalloff};
          drawLayer(maskEffects.brightness,
                    maskEffects.contrast,
                    maskEffects.saturation,
                    layer.maskOpacity,
                    neutral,
                    maskMidtones,
                    maskGradeHighlights,
                    kVulkanEffectModeMaskGrade,
                    maskEdgeParams);
        }
      }
      vkCmdEndRenderPass(m_commandBuffer);
      layerIndex += batchCount;
      if (layerIndex < layers.size()) {
        if (vkEndCommandBuffer(m_commandBuffer) != VK_SUCCESS ||
            !submitAndWait()) {
          return QImage();
        }
        vkResetCommandBuffer(m_commandBuffer, 0);
        VkCommandBufferBeginInfo nextBatchBegin{};
        nextBatchBegin.sType =
            VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
        if (vkBeginCommandBuffer(
                m_commandBuffer,
                &nextBatchBegin) != VK_SUCCESS) {
          return QImage();
        }
      }
    }

    const QRectF outputTargetRect(
        QPointF(0.0, 0.0), QSizeF(m_outputSize));
    if ((!textInputs.transcripts.isEmpty() || !textInputs.title3D.isEmpty()) &&
        (!m_transcriptTextRenderer ||
         !m_transcriptTextRenderer->isReady())) {
      if (failureReason) {
        *failureReason = QStringLiteral(
            "Vulkan export refused to drop %1 subtitle overlay(s) and %2 "
            "title overlay(s): "
            "transcript text renderer is unavailable.")
            .arg(textInputs.transcripts.size())
            .arg(textInputs.title3D.size());
      }
      return QImage();
    }
    if (textInputs.hasSpeakerLabel &&
        (!m_speakerTextRenderer ||
         !m_speakerTextRenderer->isReady())) {
      if (failureReason) {
        *failureReason = QStringLiteral(
            "Vulkan export refused to drop speaker label overlay: "
            "speaker text renderer is unavailable.");
      }
      return QImage();
    }
    if (m_transcriptTextRenderer &&
        m_transcriptTextRenderer->isReady()) {
      for (const TranscriptTextInput& text :
           std::as_const(textInputs.transcripts)) {
        if (!m_transcriptTextRenderer->prepareTranscriptOverlayAtlas(
                m_commandBuffer, m_outputSize, text.clip, text.layout,
                text.outputRect, text.speakerTitle)) {
          if (failureReason) {
            *failureReason = QStringLiteral(
                "Vulkan export refused to drop subtitle overlay for clip %1: "
                "%2")
                .arg(text.clip.id,
                     m_transcriptTextRenderer->lastFailureReason().isEmpty()
                         ? QStringLiteral("transcript_prepare_failed")
                         : m_transcriptTextRenderer->lastFailureReason());
          }
          vkEndCommandBuffer(m_commandBuffer);
          return QImage();
        }
      }
      for (const EvaluatedTitle& title :
           std::as_const(textInputs.title3D)) {
        if (!m_transcriptTextRenderer->prepareTitleOverlayAtlas(
                m_commandBuffer, m_outputSize, title)) {
          if (failureReason) {
            *failureReason = QStringLiteral(
                "Vulkan export refused to drop title overlay \"%1\": %2")
                .arg(title.text,
                     m_transcriptTextRenderer->lastFailureReason().isEmpty()
                         ? QStringLiteral("title_prepare_failed")
                         : m_transcriptTextRenderer->lastFailureReason());
          }
          vkEndCommandBuffer(m_commandBuffer);
          return QImage();
        }
      }
    }
    if (textInputs.hasSpeakerLabel) {
      if (!m_speakerTextRenderer->prepareSpeakerLabelAtlas(
              m_commandBuffer, m_outputSize,
              textInputs.speakerLabel)) {
        if (failureReason) {
          *failureReason = QStringLiteral(
              "Vulkan export refused to drop speaker label overlay: %1")
              .arg(m_speakerTextRenderer->lastFailureReason().isEmpty()
                       ? QStringLiteral("speaker_prepare_failed")
                       : m_speakerTextRenderer->lastFailureReason());
        }
        vkEndCommandBuffer(m_commandBuffer);
        return QImage();
      }
    }
    if (m_transcriptTextRenderer &&
        m_transcriptTextRenderer->isReady()) {
      for (const TranscriptTextInput& text :
           std::as_const(textInputs.transcripts)) {
        vkCmdBeginRenderPass(
            m_commandBuffer, &renderPassBeginInfo,
            VK_SUBPASS_CONTENTS_INLINE);
        const bool transcriptDrawn =
            m_transcriptTextRenderer->drawTranscriptOverlay(
                m_commandBuffer, m_outputSize, m_outputSize,
                outputTargetRect, text.clip, text.layout,
                text.outputRect, text.speakerTitle,
                text.opacityMultiplier);
        vkCmdEndRenderPass(m_commandBuffer);
        if (!transcriptDrawn) {
          if (failureReason) {
            *failureReason = QStringLiteral(
                "Vulkan export refused to drop subtitle overlay for clip %1: "
                "%2")
                .arg(text.clip.id,
                     m_transcriptTextRenderer->lastFailureReason().isEmpty()
                         ? QStringLiteral("transcript_draw_failed")
                         : m_transcriptTextRenderer->lastFailureReason());
          }
          vkEndCommandBuffer(m_commandBuffer);
          return QImage();
        }
      }
      for (const EvaluatedTitle& title :
           std::as_const(textInputs.title3D)) {
        vkCmdBeginRenderPass(
            m_commandBuffer, &renderPassBeginInfo,
            VK_SUBPASS_CONTENTS_INLINE);
        const bool titleDrawn =
            m_transcriptTextRenderer->drawTitleOverlay3D(
                m_commandBuffer, m_outputSize, m_outputSize,
                outputTargetRect, title);
        vkCmdEndRenderPass(m_commandBuffer);
        if (!titleDrawn) {
          if (failureReason) {
            *failureReason = QStringLiteral(
                "Vulkan export refused to drop title overlay \"%1\": %2")
                .arg(title.text,
                     m_transcriptTextRenderer->lastFailureReason().isEmpty()
                         ? QStringLiteral("title_draw_failed")
                         : m_transcriptTextRenderer->lastFailureReason());
          }
          vkEndCommandBuffer(m_commandBuffer);
          return QImage();
        }
      }
    }
    if (textInputs.hasSpeakerLabel) {
      vkCmdBeginRenderPass(
          m_commandBuffer, &renderPassBeginInfo,
          VK_SUBPASS_CONTENTS_INLINE);
      const bool speakerDrawn =
          m_speakerTextRenderer->drawSpeakerLabel(
              m_commandBuffer, m_outputSize, m_outputSize,
              outputTargetRect, textInputs.speakerLabel);
      vkCmdEndRenderPass(m_commandBuffer);
      if (!speakerDrawn) {
        if (failureReason) {
          *failureReason = QStringLiteral(
              "Vulkan export refused to drop speaker label overlay: %1")
              .arg(m_speakerTextRenderer->lastFailureReason().isEmpty()
                       ? QStringLiteral("speaker_draw_failed")
                       : m_speakerTextRenderer->lastFailureReason());
        }
        vkEndCommandBuffer(m_commandBuffer);
        return QImage();
      }
    }

    transitionImageLayout(m_commandBuffer, m_colorImage,
                          VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                          VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
    m_colorImageLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    if (gpuPreviewFrame) {
      m_commandBufferOpenForConversion = true;
      const bool previewPublished =
          publishLastFrameForGpuPreview(gpuPreviewFrame, gpuPreviewError);
      m_commandBufferOpenForConversion = false;
      if (previewPublished) {
        transitionImageLayout(m_commandBuffer, m_colorImage,
                              m_colorImageLayout,
                              VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
        m_colorImageLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
      }
    }
    if (readbackToImage) {
      VkBufferImageCopy readbackRegion{};
      readbackRegion.bufferOffset = 0;
      readbackRegion.bufferRowLength = 0;
      readbackRegion.bufferImageHeight = 0;
      readbackRegion.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
      readbackRegion.imageSubresource.mipLevel = 0;
      readbackRegion.imageSubresource.baseArrayLayer = 0;
      readbackRegion.imageSubresource.layerCount = 1;
      readbackRegion.imageExtent = {
          static_cast<uint32_t>(m_outputSize.width()),
          static_cast<uint32_t>(m_outputSize.height()), 1};
      vkCmdCopyImageToBuffer(m_commandBuffer, m_colorImage,
                             VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                             m_stagingBuffer, 1, &readbackRegion);
    }

    if (!readbackToImage) {
      m_commandBufferOpenForConversion = true;
      m_colorImagePrimed = true;
      return QImage();
    }

    if (vkEndCommandBuffer(m_commandBuffer) != VK_SUCCESS) {
      return QImage();
    }

    if (!submitAndWait()) {
      return QImage();
    }

    QImage out;
    if (readbackToImage) {
      if (!m_stagingMapped) {
        return QImage();
      }
      if (m_activeSlotIndex < 0 || m_activeSlotIndex >= m_frameSlots.size() ||
          !invalidateSlotForHostRead(m_frameSlots[m_activeSlotIndex])) {
        return QImage();
      }
      QImage readbackRgba(reinterpret_cast<const uchar *>(m_stagingMapped),
                          m_outputSize.width(), m_outputSize.height(),
                          m_outputSize.width() * 4, QImage::Format_ARGB32);
      out = readbackRgba.copy()
                .convertToFormat(QImage::Format_ARGB32_Premultiplied);
    }
    m_colorImagePrimed = true;
    return out;
  }
