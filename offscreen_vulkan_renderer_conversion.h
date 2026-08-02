// Frame publication and pixel format conversion methods.
// Included inside OffscreenVulkanRendererPrivate; do not compile separately.
  int lastHardwareSourceImportCount() const {
    return m_lastHardwareSourceImportCount;
  }

  int lastHardwareSourceReuseCount() const {
    return m_lastHardwareSourceReuseCount;
  }

  bool finishLastFrameForExternalSampling(OffscreenVulkanFrame *frame,
                                          QString *errorMessage) const {
    if (!frame) {
      return false;
    }
    frame->valid = false;
    if (!m_initialized || m_device == VK_NULL_HANDLE ||
        m_colorImageView == VK_NULL_HANDLE || m_activeSlotIndex < 0 ||
        m_activeSlotIndex >= m_frameSlots.size()) {
      if (errorMessage) {
        *errorMessage =
            QStringLiteral("No rendered Vulkan frame is available.");
      }
      return false;
    }
    auto *self = const_cast<OffscreenVulkanRendererPrivate *>(this);
    if (m_commandBufferOpenForConversion) {
      transitionImageLayout(self->m_commandBuffer, self->m_colorImage,
                            self->m_colorImageLayout,
                            VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
      self->m_colorImageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
      if (vkEndCommandBuffer(self->m_commandBuffer) != VK_SUCCESS) {
        if (errorMessage) {
          *errorMessage =
              QStringLiteral("Failed to finish Vulkan render command buffer.");
        }
        return false;
      }
      if (!self->submitAndWait()) {
        if (errorMessage) {
          *errorMessage =
              QStringLiteral("Failed to submit Vulkan render command buffer.");
        }
        return false;
      }
      self->m_commandBufferOpenForConversion = false;
    } else if (m_colorImageLayout != VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL &&
               m_colorImageLayout != VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL) {
      if (errorMessage) {
        *errorMessage = QStringLiteral(
            "Rendered Vulkan frame is not in a sampleable state.");
      }
      return false;
    }

    frame->physicalDevice = m_physicalDevice;
    frame->device = m_device;
    frame->queue = m_graphicsQueue;
    frame->queueFamilyIndex = m_graphicsQueueFamily;
    frame->image = m_colorImage;
    frame->imageView = m_colorImageView;
    frame->imageMemory = m_colorImageMemory;
    frame->imageLayout = m_colorImageLayout;
    frame->imageFormat = VK_FORMAT_B8G8R8A8_UNORM;
    frame->readySemaphoreFd = -1;
    frame->size = {m_outputSize.width(), m_outputSize.height()};
    frame->queueSupportsCompute = m_graphicsQueueSupportsCompute;
    frame->valid = true;
    return true;
  }

  bool hasPendingGpuFrame() const {
    return m_initialized && m_commandBufferOpenForConversion &&
        m_activeSlotIndex >= 0 &&
        m_activeSlotIndex < m_frameSlots.size();
  }

  bool convertLastFrameToNv12(AVFrame *frame, qint64 *nv12ConvertMs,
                              qint64 *readbackMs) {
    return beginLastFrameToNv12Readback(nv12ConvertMs, readbackMs) &&
           finishLastFrameToNv12Readback(frame, nv12ConvertMs, readbackMs);
  }

  bool beginLastFrameToNv12Copy(VkBuffer targetBuffer,
                                QVector<int> *pendingSlots, qint64 *convertMs,
                                qint64 *transferMs) {
    if (!m_initialized || m_device == VK_NULL_HANDLE ||
        m_commandBuffer == VK_NULL_HANDLE || targetBuffer == VK_NULL_HANDLE ||
        !pendingSlots) {
      return false;
    }
    const qint64 startMs = QDateTime::currentMSecsSinceEpoch();
    VkCommandBufferBeginInfo beginInfo{};
    beginInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    if (!m_commandBufferOpenForConversion) {
      vkResetCommandBuffer(m_commandBuffer, 0);
      if (vkBeginCommandBuffer(m_commandBuffer, &beginInfo) != VK_SUCCESS) {
        return false;
      }
    }
    transitionImageLayout(m_commandBuffer, m_colorImage,
                          VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                          VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    const VkDeviceSize yPlaneBytes =
        static_cast<VkDeviceSize>(m_outputSize.width()) *
        static_cast<VkDeviceSize>(m_outputSize.height());
    const VkDeviceSize uvPlaneOffset =
        (yPlaneBytes + 255u) & ~VkDeviceSize(255u);
    const VkDeviceSize uvPlaneBytes =
        static_cast<VkDeviceSize>(qMax(1, m_outputSize.width() / 2)) *
        static_cast<VkDeviceSize>(qMax(1, m_outputSize.height() / 2)) * 2;
    FrameSlot &slot = m_frameSlots[m_activeSlotIndex];
    VkDescriptorImageInfo sourceInfo{};
    sourceInfo.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    sourceInfo.imageView = m_colorImageView;
    sourceInfo.sampler = m_sampler;
    VkDescriptorBufferInfo outputInfo{};
    outputInfo.buffer = targetBuffer;
    outputInfo.offset = 0;
    outputInfo.range = uvPlaneOffset + uvPlaneBytes;
    VkWriteDescriptorSet descriptorWrites[2]{};
    descriptorWrites[0].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    descriptorWrites[0].dstSet = slot.nv12ComputeDescriptorSet;
    descriptorWrites[0].dstBinding = 0;
    descriptorWrites[0].descriptorCount = 1;
    descriptorWrites[0].descriptorType =
        VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    descriptorWrites[0].pImageInfo = &sourceInfo;
    descriptorWrites[1].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    descriptorWrites[1].dstSet = slot.nv12ComputeDescriptorSet;
    descriptorWrites[1].dstBinding = 1;
    descriptorWrites[1].descriptorCount = 1;
    descriptorWrites[1].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    descriptorWrites[1].pBufferInfo = &outputInfo;
    vkUpdateDescriptorSets(m_device, 2, descriptorWrites, 0, nullptr);

    struct Nv12PushConstants {
      uint32_t width;
      uint32_t height;
      uint32_t uvOffsetWords;
      uint32_t yWordCount;
      uint32_t uvWordCount;
    } pushConstants{
        static_cast<uint32_t>(m_outputSize.width()),
        static_cast<uint32_t>(m_outputSize.height()),
        static_cast<uint32_t>(uvPlaneOffset / 4),
        static_cast<uint32_t>((yPlaneBytes + 3) / 4),
        static_cast<uint32_t>((uvPlaneBytes + 3) / 4)};
    vkCmdBindPipeline(m_commandBuffer, VK_PIPELINE_BIND_POINT_COMPUTE,
                      m_nv12ComputePipeline);
    vkCmdBindDescriptorSets(m_commandBuffer, VK_PIPELINE_BIND_POINT_COMPUTE,
                            m_nv12ComputePipelineLayout, 0, 1,
                            &slot.nv12ComputeDescriptorSet, 0, nullptr);
    vkCmdPushConstants(m_commandBuffer, m_nv12ComputePipelineLayout,
                       VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(pushConstants),
                       &pushConstants);
    const uint32_t wordCount =
        pushConstants.yWordCount + pushConstants.uvWordCount;
    vkCmdDispatch(m_commandBuffer, (wordCount + 255u) / 256u, 1, 1);

    VkBufferMemoryBarrier outputBarrier{};
    outputBarrier.sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER;
    outputBarrier.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
    outputBarrier.dstAccessMask =
        VK_ACCESS_HOST_READ_BIT | VK_ACCESS_MEMORY_READ_BIT;
    outputBarrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    outputBarrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    outputBarrier.buffer = targetBuffer;
    outputBarrier.offset = 0;
    outputBarrier.size = uvPlaneOffset + uvPlaneBytes;
    vkCmdPipelineBarrier(m_commandBuffer, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                         VK_PIPELINE_STAGE_HOST_BIT |
                             VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
                         0, 0, nullptr, 1, &outputBarrier, 0, nullptr);
    transitionImageLayout(m_commandBuffer, m_colorImage,
                          VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                          VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
    if (vkEndCommandBuffer(m_commandBuffer) != VK_SUCCESS) {
      return false;
    }
    // Queue ordering preserves the shared color/NV12 attachment sequence. Do
    // not wait here: the export loop keeps a bounded pending-frame queue and
    // finishLastFrameToNv12*() waits only when the specific slot's output
    // buffer is needed by the encoder.
    if (!submitActiveSlot()) {
      return false;
    }
    m_commandBufferOpenForConversion = false;
    pendingSlots->push_back(m_activeSlotIndex);
    if (convertMs) {
      *convertMs += QDateTime::currentMSecsSinceEpoch() - startMs;
    }
    Q_UNUSED(transferMs)
    return true;
  }

  bool beginLastFrameToNv12Readback(qint64 *convertMs, qint64 *readbackMs) {
    return beginLastFrameToNv12Copy(m_stagingBuffer, &m_pendingNv12SlotIndices,
                                    convertMs, readbackMs);
  }

  bool beginLastFrameToNv12CudaTransfer(qint64 *convertMs, qint64 *transferMs) {
    if (!supportsCudaExternalMemoryInterop() || m_activeSlotIndex < 0 ||
        m_activeSlotIndex >= m_frameSlots.size()) {
      return false;
    }
    FrameSlot &slot = m_frameSlots[m_activeSlotIndex];
    return beginLastFrameToNv12Copy(slot.cudaExportBuffer,
                                    &m_pendingNv12CudaSlotIndices, convertMs,
                                    transferMs);
  }

  bool finishLastFrameToNv12Readback(AVFrame *frame, qint64 *convertMs,
                                     qint64 *readbackMs) {
    if (!frame || frame->format != AV_PIX_FMT_NV12 || frame->width <= 0 ||
        frame->height <= 0 || m_pendingNv12SlotIndices.isEmpty()) {
      return false;
    }
    const qint64 startMs = QDateTime::currentMSecsSinceEpoch();
    const int slotIndex = m_pendingNv12SlotIndices.takeFirst();
    if (slotIndex < 0 || slotIndex >= m_frameSlots.size()) {
      return false;
    }
    if (!waitSlot(slotIndex)) {
      return false;
    }
    FrameSlot &slot = m_frameSlots[slotIndex];
    if (!slot.stagingMapped || !invalidateSlotForHostRead(slot)) {
      return false;
    }
    const uint8_t *bytes =
        reinterpret_cast<const uint8_t *>(slot.stagingMapped);
    const VkDeviceSize yPlaneBytes =
        static_cast<VkDeviceSize>(m_outputSize.width()) *
        static_cast<VkDeviceSize>(m_outputSize.height());
    const VkDeviceSize uvPlaneOffset =
        (yPlaneBytes + 255u) & ~VkDeviceSize(255u);
    for (int y = 0; y < frame->height; ++y) {
      memcpy(frame->data[0] + y * frame->linesize[0],
             bytes + y * m_outputSize.width(), frame->width);
    }
    const int uvWidthBytes = qMax(1, frame->width / 2) * 2;
    const uint8_t *uvMapped = bytes + uvPlaneOffset;
    for (int y = 0; y < qMax(1, frame->height / 2); ++y) {
      memcpy(frame->data[1] + y * frame->linesize[1],
             uvMapped + y * uvWidthBytes, uvWidthBytes);
    }
    if (convertMs) {
      *convertMs += QDateTime::currentMSecsSinceEpoch() - startMs;
    }
    if (readbackMs) {
      *readbackMs += QDateTime::currentMSecsSinceEpoch() - startMs;
    }
    return true;
  }

#if JCUT_HAS_CUDA_DRIVER
  void retireCudaConsumedSemaphore(FrameSlot &slot) {
    if (!slot.cudaConsumedExternalSemaphore) {
      slot.cudaConsumedContext = nullptr;
      slot.cudaCopyPending = false;
      return;
    }
    CUcontext current = nullptr;
    cuCtxGetCurrent(&current);
    const bool pushed = slot.cudaConsumedContext &&
        current != slot.cudaConsumedContext &&
        cuCtxPushCurrent(slot.cudaConsumedContext) == CUDA_SUCCESS;
    if (current == slot.cudaConsumedContext || pushed) {
      if (slot.cudaCopyPending) {
        cuCtxSynchronize();
      }
      cuDestroyExternalSemaphore(slot.cudaConsumedExternalSemaphore);
    }
    if (pushed) {
      CUcontext previous = nullptr;
      cuCtxPopCurrent(&previous);
    }
    slot.cudaConsumedExternalSemaphore = nullptr;
    slot.cudaConsumedContext = nullptr;
    slot.cudaCopyPending = false;
  }

  bool ensureCudaConsumedSemaphoreForSlot(FrameSlot &slot,
                                          CUcontext cudaContext) {
    if (slot.cudaConsumedExternalSemaphore &&
        slot.cudaConsumedContext == cudaContext) {
      return true;
    }
    if (!m_vkGetSemaphoreFdKHR ||
        slot.cudaConsumedSemaphore == VK_NULL_HANDLE) {
      return false;
    }
    // A changed encoder context owns the previous imported handle. Its retained
    // device reference retires that handle with the old context.
    slot.cudaConsumedExternalSemaphore = nullptr;
    slot.cudaConsumedContext = nullptr;
    VkSemaphoreGetFdInfoKHR fdInfo{};
    fdInfo.sType = VK_STRUCTURE_TYPE_SEMAPHORE_GET_FD_INFO_KHR;
    fdInfo.semaphore = slot.cudaConsumedSemaphore;
    fdInfo.handleType = VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_OPAQUE_FD_BIT;
    int semaphoreFd = -1;
    if (m_vkGetSemaphoreFdKHR(m_device, &fdInfo, &semaphoreFd) != VK_SUCCESS ||
        semaphoreFd < 0) {
      return false;
    }
    CUDA_EXTERNAL_SEMAPHORE_HANDLE_DESC handle{};
    handle.type = CU_EXTERNAL_SEMAPHORE_HANDLE_TYPE_OPAQUE_FD;
    handle.handle.fd = semaphoreFd;
    if (cuImportExternalSemaphore(&slot.cudaConsumedExternalSemaphore,
                                  &handle) != CUDA_SUCCESS) {
      close(semaphoreFd);
      slot.cudaConsumedExternalSemaphore = nullptr;
      return false;
    }
    slot.cudaConsumedContext = cudaContext;
    return true;
  }

  bool ensureCudaExternalMemoryForSlot(FrameSlot &slot, AVFrame *cudaFrame) {
    if (!cudaFrame || cudaFrame->format != AV_PIX_FMT_CUDA ||
        !cudaFrame->hw_frames_ctx || slot.cudaExportMemory == VK_NULL_HANDLE ||
        slot.cudaExportAllocationSize == 0 || !m_vkGetMemoryFdKHR) {
      return false;
    }
    auto *framesCtx =
        reinterpret_cast<AVHWFramesContext *>(cudaFrame->hw_frames_ctx->data);
    if (!framesCtx || !framesCtx->device_ref || !framesCtx->device_ctx ||
        !framesCtx->device_ctx->hwctx) {
      return false;
    }
    auto *cudaDevice =
        reinterpret_cast<AVCUDADeviceContext *>(framesCtx->device_ctx->hwctx);
    CUcontext cudaContext = cudaDevice->cuda_ctx;
    if (!cudaContext) {
      return false;
    }

    AVBufferRef *retiredCudaDeviceRef = nullptr;
    auto releaseRetiredCudaDevice = qScopeGuard(
        [&]() { av_buffer_unref(&retiredCudaDeviceRef); });
    CUcontext previous = nullptr;
    CUresult cuResult = cuInit(0);
    if (cuResult != CUDA_SUCCESS ||
        cuCtxPushCurrent(cudaContext) != CUDA_SUCCESS) {
      return false;
    }

    auto popContext = qScopeGuard([&]() { cuCtxPopCurrent(&previous); });

    if (slot.cudaExternalMemory && slot.cudaImportContext != cudaContext) {
      // The previous encoder's CUDA context owns this opaque import. It can
      // already be in retirement when a new incremental chunk reaches this
      // renderer, so calling the driver destroy entry point from the new
      // context is invalid. Dropping our retained device reference retires the
      // old context and all of its imports together.
      slot.cudaExternalMemory = nullptr;
      slot.cudaExternalDevicePtr = 0;
      slot.cudaImportContext = nullptr;
      slot.cudaConsumedExternalSemaphore = nullptr;
      slot.cudaConsumedContext = nullptr;
      retiredCudaDeviceRef = slot.cudaImportDeviceRef;
      slot.cudaImportDeviceRef = nullptr;
    }
    if (slot.cudaExternalMemory && slot.cudaExternalDevicePtr) {
      return ensureCudaConsumedSemaphoreForSlot(slot, cudaContext);
    }

    AVBufferRef *cudaImportDeviceRef = av_buffer_ref(framesCtx->device_ref);
    if (!cudaImportDeviceRef) {
      return false;
    }

    VkMemoryGetFdInfoKHR fdInfo{};
    fdInfo.sType = VK_STRUCTURE_TYPE_MEMORY_GET_FD_INFO_KHR;
    fdInfo.memory = slot.cudaExportMemory;
    fdInfo.handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_FD_BIT;
    int memoryFd = -1;
    if (m_vkGetMemoryFdKHR(m_device, &fdInfo, &memoryFd) != VK_SUCCESS ||
        memoryFd < 0) {
      av_buffer_unref(&cudaImportDeviceRef);
      return false;
    }

    CUDA_EXTERNAL_MEMORY_HANDLE_DESC handleDesc{};
    handleDesc.type = CU_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_FD;
    handleDesc.handle.fd = memoryFd;
    handleDesc.size = slot.cudaExportAllocationSize;
    cuResult = cuImportExternalMemory(&slot.cudaExternalMemory, &handleDesc);
    if (cuResult != CUDA_SUCCESS) {
      close(memoryFd);
      slot.cudaExternalMemory = nullptr;
      av_buffer_unref(&cudaImportDeviceRef);
      return false;
    }

    CUDA_EXTERNAL_MEMORY_BUFFER_DESC bufferDesc{};
    bufferDesc.offset = 0;
    bufferDesc.size = slot.cudaExportAllocationSize;
    cuResult = cuExternalMemoryGetMappedBuffer(
        &slot.cudaExternalDevicePtr, slot.cudaExternalMemory, &bufferDesc);
    if (cuResult != CUDA_SUCCESS) {
      // Keep the driver-safe lifetime policy consistent with renderer teardown:
      // a failed map retires by dropping our references, not by synchronously
      // destroying the CUDA external-memory import from this worker thread.
      slot.cudaExternalMemory = nullptr;
      slot.cudaExternalDevicePtr = 0;
      av_buffer_unref(&cudaImportDeviceRef);
      return false;
    }
    slot.cudaImportContext = cudaContext;
    slot.cudaImportDeviceRef = cudaImportDeviceRef;
    return ensureCudaConsumedSemaphoreForSlot(slot, cudaContext);
  }
#endif

  bool finishLastFrameToNv12CudaTransfer(AVFrame *cudaFrame, qint64 *convertMs,
                                         qint64 *transferMs) {
#if JCUT_HAS_CUDA_DRIVER
    if (!cudaFrame || cudaFrame->format != AV_PIX_FMT_CUDA ||
        m_pendingNv12CudaSlotIndices.isEmpty()) {
      return false;
    }
    const qint64 startMs = QDateTime::currentMSecsSinceEpoch();
    const int slotIndex = m_pendingNv12CudaSlotIndices.takeFirst();
    if (slotIndex < 0 || slotIndex >= m_frameSlots.size()) {
      return false;
    }
    if (!waitSlot(slotIndex)) {
      return false;
    }
    FrameSlot &slot = m_frameSlots[slotIndex];
    if (!ensureCudaExternalMemoryForSlot(slot, cudaFrame)) {
      return false;
    }

    auto *framesCtx =
        reinterpret_cast<AVHWFramesContext *>(cudaFrame->hw_frames_ctx->data);
    auto *cudaDevice =
        reinterpret_cast<AVCUDADeviceContext *>(framesCtx->device_ctx->hwctx);
    CUcontext cudaContext = cudaDevice->cuda_ctx;
    CUcontext previous = nullptr;
    if (cuCtxPushCurrent(cudaContext) != CUDA_SUCCESS) {
      return false;
    }
    auto popContext = qScopeGuard([&]() { cuCtxPopCurrent(&previous); });

    const int width = qMin(cudaFrame->width, m_outputSize.width());
    const int height = qMin(cudaFrame->height, m_outputSize.height());
    const VkDeviceSize yPlaneBytes =
        static_cast<VkDeviceSize>(m_outputSize.width()) *
        static_cast<VkDeviceSize>(m_outputSize.height());
    const VkDeviceSize uvPlaneOffset =
        (yPlaneBytes + 255u) & ~VkDeviceSize(255u);

    CUDA_MEMCPY2D yCopy{};
    yCopy.srcMemoryType = CU_MEMORYTYPE_DEVICE;
    yCopy.srcDevice = slot.cudaExternalDevicePtr;
    yCopy.srcPitch = static_cast<size_t>(m_outputSize.width());
    yCopy.dstMemoryType = CU_MEMORYTYPE_DEVICE;
    yCopy.dstDevice = reinterpret_cast<CUdeviceptr>(cudaFrame->data[0]);
    yCopy.dstPitch = static_cast<size_t>(cudaFrame->linesize[0]);
    yCopy.WidthInBytes = static_cast<size_t>(width);
    yCopy.Height = static_cast<size_t>(height);
    if (cuMemcpy2DAsync(&yCopy, cudaDevice->stream) != CUDA_SUCCESS) {
      return false;
    }

    CUDA_MEMCPY2D uvCopy{};
    uvCopy.srcMemoryType = CU_MEMORYTYPE_DEVICE;
    uvCopy.srcDevice = slot.cudaExternalDevicePtr + uvPlaneOffset;
    uvCopy.srcPitch = static_cast<size_t>(m_outputSize.width());
    uvCopy.dstMemoryType = CU_MEMORYTYPE_DEVICE;
    uvCopy.dstDevice = reinterpret_cast<CUdeviceptr>(cudaFrame->data[1]);
    uvCopy.dstPitch = static_cast<size_t>(cudaFrame->linesize[1]);
    uvCopy.WidthInBytes = static_cast<size_t>(width);
    uvCopy.Height = static_cast<size_t>(qMax(1, height / 2));
    if (cuMemcpy2DAsync(&uvCopy, cudaDevice->stream) != CUDA_SUCCESS) {
      return false;
    }
    CUDA_EXTERNAL_SEMAPHORE_SIGNAL_PARAMS signalParams{};
    CUexternalSemaphore consumedSemaphores[] = {
        slot.cudaConsumedExternalSemaphore};
    if (cuSignalExternalSemaphoresAsync(consumedSemaphores, &signalParams, 1,
                                        cudaDevice->stream) != CUDA_SUCCESS) {
      return false;
    }
    // FFmpeg configures NVENC input and output on this same CUDA device stream.
    // Copy, semaphore signal, and encoder consumption therefore remain ordered
    // without a host-side stream synchronization.
    slot.cudaCopyPending = true;
    const qint64 elapsed = QDateTime::currentMSecsSinceEpoch() - startMs;
    if (convertMs) {
      *convertMs += elapsed;
    }
    if (transferMs) {
      *transferMs += elapsed;
    }
    return true;
#else
    Q_UNUSED(cudaFrame)
    Q_UNUSED(convertMs)
    Q_UNUSED(transferMs)
    return false;
#endif
  }

  bool beginLastFrameToYuv420pReadback(qint64 *convertMs, qint64 *readbackMs) {
    if (!m_initialized || m_device == VK_NULL_HANDLE ||
        m_commandBuffer == VK_NULL_HANDLE) {
      return false;
    }
    const qint64 startMs = QDateTime::currentMSecsSinceEpoch();
    VkCommandBufferBeginInfo beginInfo{};
    beginInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    if (!m_commandBufferOpenForConversion) {
      vkResetCommandBuffer(m_commandBuffer, 0);
      if (vkBeginCommandBuffer(m_commandBuffer, &beginInfo) != VK_SUCCESS) {
        return false;
      }
    }

    transitionImageLayout(m_commandBuffer, m_colorImage,
                          VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                          VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    const uint32_t yWidth = static_cast<uint32_t>(m_outputSize.width());
    const uint32_t yHeight = static_cast<uint32_t>(m_outputSize.height());
    const uint32_t chromaWidth =
        static_cast<uint32_t>(qMax(1, m_outputSize.width() / 2));
    const uint32_t chromaHeight =
        static_cast<uint32_t>(qMax(1, m_outputSize.height() / 2));
    const VkImageLayout oldYuvLayout =
        m_yuv420pPlanesPrimed ? VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL
                              : VK_IMAGE_LAYOUT_UNDEFINED;
    transitionImageLayout(m_commandBuffer, m_nv12YImage, oldYuvLayout,
                          VK_IMAGE_LAYOUT_GENERAL);
    transitionImageLayout(m_commandBuffer, m_yuv420pUImage, oldYuvLayout,
                          VK_IMAGE_LAYOUT_GENERAL);
    transitionImageLayout(m_commandBuffer, m_yuv420pVImage, oldYuvLayout,
                          VK_IMAGE_LAYOUT_GENERAL);
    vkCmdBindPipeline(m_commandBuffer, VK_PIPELINE_BIND_POINT_COMPUTE,
                      m_yuv420pComputePipeline);
    vkCmdBindDescriptorSets(m_commandBuffer, VK_PIPELINE_BIND_POINT_COMPUTE,
                            m_yuvComputePipelineLayout, 0, 1,
                            &m_yuvComputeDescriptorSet, 0, nullptr);
    vkCmdDispatch(m_commandBuffer, (yWidth + 15u) / 16u, (yHeight + 15u) / 16u,
                  1);
    transitionImageLayout(m_commandBuffer, m_nv12YImage,
                          VK_IMAGE_LAYOUT_GENERAL,
                          VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
    transitionImageLayout(m_commandBuffer, m_yuv420pUImage,
                          VK_IMAGE_LAYOUT_GENERAL,
                          VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
    transitionImageLayout(m_commandBuffer, m_yuv420pVImage,
                          VK_IMAGE_LAYOUT_GENERAL,
                          VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);

    const VkDeviceSize yPlaneBytes =
        static_cast<VkDeviceSize>(m_outputSize.width()) *
        static_cast<VkDeviceSize>(m_outputSize.height());
    const VkDeviceSize uPlaneBytes = static_cast<VkDeviceSize>(chromaWidth) *
                                     static_cast<VkDeviceSize>(chromaHeight);
    const VkDeviceSize uPlaneOffset =
        (yPlaneBytes + 255u) & ~VkDeviceSize(255u);
    const VkDeviceSize vPlaneOffset =
        (uPlaneOffset + uPlaneBytes + 255u) & ~VkDeviceSize(255u);
    VkBufferImageCopy yRegion{};
    yRegion.bufferOffset = 0;
    yRegion.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    yRegion.imageSubresource.mipLevel = 0;
    yRegion.imageSubresource.baseArrayLayer = 0;
    yRegion.imageSubresource.layerCount = 1;
    yRegion.imageExtent = {yWidth, yHeight, 1};
    vkCmdCopyImageToBuffer(m_commandBuffer, m_nv12YImage,
                           VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                           m_stagingBuffer, 1, &yRegion);

    VkBufferImageCopy uRegion{};
    uRegion.bufferOffset = uPlaneOffset;
    uRegion.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    uRegion.imageSubresource.mipLevel = 0;
    uRegion.imageSubresource.baseArrayLayer = 0;
    uRegion.imageSubresource.layerCount = 1;
    uRegion.imageExtent = {chromaWidth, chromaHeight, 1};
    vkCmdCopyImageToBuffer(m_commandBuffer, m_yuv420pUImage,
                           VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                           m_stagingBuffer, 1, &uRegion);

    VkBufferImageCopy vRegion = uRegion;
    vRegion.bufferOffset = vPlaneOffset;
    vkCmdCopyImageToBuffer(m_commandBuffer, m_yuv420pVImage,
                           VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                           m_stagingBuffer, 1, &vRegion);
    transitionImageLayout(m_commandBuffer, m_colorImage,
                          VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                          VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
    if (vkEndCommandBuffer(m_commandBuffer) != VK_SUCCESS) {
      return false;
    }
    if (!submitActiveSlot()) {
      return false;
    }
    m_commandBufferOpenForConversion = false;
    m_yuv420pPlanesPrimed = true;
    m_pendingYuvSlotIndices.push_back(m_activeSlotIndex);
    if (convertMs) {
      *convertMs += QDateTime::currentMSecsSinceEpoch() - startMs;
    }
    Q_UNUSED(readbackMs)
    return true;
  }

  bool finishLastFrameToYuv420pReadback(AVFrame *frame, qint64 *convertMs,
                                        qint64 *readbackMs) {
    if (!frame || frame->format != AV_PIX_FMT_YUV420P || frame->width <= 0 ||
        frame->height <= 0 || m_pendingYuvSlotIndices.isEmpty()) {
      return false;
    }
    const qint64 startMs = QDateTime::currentMSecsSinceEpoch();
    const int slotIndex = m_pendingYuvSlotIndices.takeFirst();
    if (slotIndex < 0 || slotIndex >= m_frameSlots.size()) {
      return false;
    }
    if (!waitSlot(slotIndex)) {
      return false;
    }
    FrameSlot &slot = m_frameSlots[slotIndex];
    if (!slot.stagingMapped) {
      return false;
    }
    if (!invalidateSlotForHostRead(slot)) {
      return false;
    }
    const uint8_t *bytes =
        reinterpret_cast<const uint8_t *>(slot.stagingMapped);
    const int chromaWidth = qMax(1, m_outputSize.width() / 2);
    const int chromaHeight = qMax(1, m_outputSize.height() / 2);
    const VkDeviceSize yPlaneBytes =
        static_cast<VkDeviceSize>(m_outputSize.width()) *
        static_cast<VkDeviceSize>(m_outputSize.height());
    const VkDeviceSize uPlaneBytes = static_cast<VkDeviceSize>(chromaWidth) *
                                     static_cast<VkDeviceSize>(chromaHeight);
    const VkDeviceSize uPlaneOffset =
        (yPlaneBytes + 255u) & ~VkDeviceSize(255u);
    const VkDeviceSize vPlaneOffset =
        (uPlaneOffset + uPlaneBytes + 255u) & ~VkDeviceSize(255u);
    for (int y = 0; y < frame->height; ++y) {
      memcpy(frame->data[0] + y * frame->linesize[0],
             bytes + y * m_outputSize.width(), frame->width);
    }
    const int frameChromaWidth = qMax(1, frame->width / 2);
    const int frameChromaHeight = qMax(1, frame->height / 2);
    for (int y = 0; y < frameChromaHeight; ++y) {
      memcpy(frame->data[1] + y * frame->linesize[1],
             bytes + uPlaneOffset + y * chromaWidth, frameChromaWidth);
      memcpy(frame->data[2] + y * frame->linesize[2],
             bytes + vPlaneOffset + y * chromaWidth, frameChromaWidth);
    }
    if (convertMs) {
      *convertMs += QDateTime::currentMSecsSinceEpoch() - startMs;
    }
    if (readbackMs) {
      *readbackMs += QDateTime::currentMSecsSinceEpoch() - startMs;
    }
    return true;
  }

  bool convertLastFrameToYuv420p(AVFrame *frame, qint64 *convertMs,
                                 qint64 *readbackMs) {
    return beginLastFrameToYuv420pReadback(convertMs, readbackMs) &&
           finishLastFrameToYuv420pReadback(frame, convertMs, readbackMs);
  }

  bool copyLastFrameToBgra(AVFrame *frame, qint64 *readbackMs) {
    if (!frame || frame->width <= 0 || frame->height <= 0 || !m_initialized ||
        m_device == VK_NULL_HANDLE || m_commandBuffer == VK_NULL_HANDLE) {
      return false;
    }
    const qint64 startMs = QDateTime::currentMSecsSinceEpoch();
    VkCommandBufferBeginInfo beginInfo{};
    beginInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    if (!m_commandBufferOpenForConversion) {
      vkResetCommandBuffer(m_commandBuffer, 0);
      if (vkBeginCommandBuffer(m_commandBuffer, &beginInfo) != VK_SUCCESS) {
        return false;
      }
    }
    VkBufferImageCopy region{};
    region.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    region.imageSubresource.mipLevel = 0;
    region.imageSubresource.baseArrayLayer = 0;
    region.imageSubresource.layerCount = 1;
    region.imageExtent = {static_cast<uint32_t>(m_outputSize.width()),
                          static_cast<uint32_t>(m_outputSize.height()), 1};
    vkCmdCopyImageToBuffer(m_commandBuffer, m_colorImage,
                           VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                           m_stagingBuffer, 1, &region);
    if (vkEndCommandBuffer(m_commandBuffer) != VK_SUCCESS) {
      return false;
    }
    if (!submitAndWait()) {
      return false;
    }
    m_commandBufferOpenForConversion = false;

    if (!m_stagingMapped) {
      return false;
    }
    if (m_activeSlotIndex < 0 || m_activeSlotIndex >= m_frameSlots.size() ||
        !invalidateSlotForHostRead(m_frameSlots[m_activeSlotIndex])) {
      return false;
    }
    const int width = qMin(frame->width, m_outputSize.width());
    const int height = qMin(frame->height, m_outputSize.height());
    const int srcStride = m_outputSize.width() * 4;
    for (int y = 0; y < height; ++y) {
      memcpy(frame->data[0] + y * frame->linesize[0],
             reinterpret_cast<uint8_t *>(m_stagingMapped) + y * srcStride,
             static_cast<size_t>(width) * 4);
    }
    if (readbackMs) {
      *readbackMs += QDateTime::currentMSecsSinceEpoch() - startMs;
    }
    return true;
  }

  bool supportsCudaExternalMemoryInterop() const {
    return m_externalMemoryFdSupported &&
           m_vkGetMemoryFdKHR != nullptr && m_cudaExportBuffersReady;
  }

  QString cudaExternalMemoryStatus() const {
    return m_cudaExternalMemoryStatus;
  }
