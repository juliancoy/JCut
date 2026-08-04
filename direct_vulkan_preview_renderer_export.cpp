#include "direct_vulkan_preview_window_internal.h"

bool DirectVulkanPreviewRenderer::renderGpuExportPreview(
    VkCommandBuffer commandBuffer)
{
    if (!m_owner || !m_window || !m_pipeline || !m_resources ||
        !m_importSemaphoreFd) {
        return false;
    }
    render_detail::OffscreenVulkanFrame frame;
    if (m_owner->takeGpuExportPreviewFrame(&frame)) {
        bool staleFrame = false;
        if (m_gpuExportPreviewProducerSessionId != frame.producerSessionId) {
            m_gpuExportPreviewProducerSessionId = frame.producerSessionId;
            m_lastAcceptedGpuExportPreviewSequence = 0;
        }
        if (frame.presentationSequence > 0 &&
            frame.presentationSequence <=
                m_lastAcceptedGpuExportPreviewSequence) {
            staleFrame = true;
        } else {
            m_lastAcceptedGpuExportPreviewSequence =
                frame.presentationSequence;
        }
        const int slotIndex = static_cast<int>(frame.bufferIndex);
        if (slotIndex < 0 ||
            slotIndex >= static_cast<int>(m_gpuExportPreviewSlots.size())) {
            return false;
        }
        GpuExportPreviewSlot& slot =
            m_gpuExportPreviewSlots[slotIndex];
        if (slot.initialized &&
            slot.producerSessionId != frame.producerSessionId) {
            vkQueueWaitIdle(m_window->graphicsQueue());
            if (slot.importer) {
                slot.importer->release();
                slot.importer.reset();
            }
            if (slot.ready != VK_NULL_HANDLE) {
                vkDestroySemaphore(
                    m_window->device(), slot.ready, nullptr);
            }
            if (slot.consumed != VK_NULL_HANDLE) {
                vkDestroySemaphore(
                    m_window->device(), slot.consumed, nullptr);
            }
            slot = {};
            if (m_gpuExportPreviewCurrentSlot == slotIndex) {
                m_gpuExportPreviewCurrentSlot = -1;
            }
        }
        if (!slot.initialized) {
            if (frame.readySemaphoreFd < 0 ||
                frame.consumedSemaphoreFd < 0) {
                return false;
            }
            slot.importer =
                std::make_unique<
                    jcut::vulkan_import::VulkanExternalFrameImportCore>();
            const jcut::vulkan_import::DeviceContext context{
                m_window->physicalDevice(),
                m_window->device(),
                m_window->graphicsQueue(),
                m_window->graphicsQueueFamilyIndex()};
            std::string error;
            if (!slot.importer->initialize(context, &error)) {
                close(frame.readySemaphoreFd);
                close(frame.consumedSemaphoreFd);
                return false;
            }
            VkSemaphoreCreateInfo semaphoreInfo{};
            semaphoreInfo.sType =
                VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;
            if (vkCreateSemaphore(m_window->device(),
                                  &semaphoreInfo,
                                  nullptr,
                                  &slot.ready) != VK_SUCCESS ||
                vkCreateSemaphore(m_window->device(),
                                  &semaphoreInfo,
                                  nullptr,
                                  &slot.consumed) != VK_SUCCESS) {
                close(frame.readySemaphoreFd);
                close(frame.consumedSemaphoreFd);
                return false;
            }
            VkImportSemaphoreFdInfoKHR import{};
            import.sType =
                VK_STRUCTURE_TYPE_IMPORT_SEMAPHORE_FD_INFO_KHR;
            import.handleType =
                VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_OPAQUE_FD_BIT;
            import.semaphore = slot.ready;
            import.fd = frame.readySemaphoreFd;
            if (m_importSemaphoreFd(m_window->device(), &import) !=
                VK_SUCCESS) {
                close(frame.readySemaphoreFd);
                close(frame.consumedSemaphoreFd);
                return false;
            }
            import.semaphore = slot.consumed;
            import.fd = frame.consumedSemaphoreFd;
            if (m_importSemaphoreFd(m_window->device(), &import) !=
                VK_SUCCESS) {
                close(frame.consumedSemaphoreFd);
                return false;
            }
            slot.producerSessionId = frame.producerSessionId;
            slot.initialized = true;
        }

        VkPipelineStageFlags waitStage = VK_PIPELINE_STAGE_TRANSFER_BIT;
        VkSubmitInfo waitForReady{};
        waitForReady.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
        waitForReady.waitSemaphoreCount = 1;
        waitForReady.pWaitSemaphores = &slot.ready;
        waitForReady.pWaitDstStageMask = &waitStage;
        if (vkQueueSubmit(m_window->graphicsQueue(),
                          1,
                          &waitForReady,
                          VK_NULL_HANDLE) != VK_SUCCESS) {
            return false;
        }

        const auto signalConsumed = [this, &slot, &frame]() {
            VkSubmitInfo signal{};
            signal.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
            signal.signalSemaphoreCount = 1;
            signal.pSignalSemaphores = &slot.consumed;
            const bool submitted =
                vkQueueSubmit(m_window->graphicsQueue(),
                              1,
                              &signal,
                              VK_NULL_HANDLE) == VK_SUCCESS;
            if (submitted && frame.consumptionState) {
                frame.consumptionState->completedGeneration.store(
                    frame.generation, std::memory_order_release);
            }
            return submitted;
        };
        std::string error;
        if (!slot.importer->importExternalFrame(frame, &error) ||
            !slot.importer->finishPendingCopy(nullptr, &error)) {
            signalConsumed();
            qWarning().noquote()
                << QStringLiteral(
                       "[vulkan-preview] GPU export preview import failed: %1")
                       .arg(QString::fromStdString(error));
            return false;
        }
        if (staleFrame) {
            signalConsumed();
            return m_gpuExportPreviewCurrentSlot >= 0;
        }
        m_resources->beginFrameUploads(
            static_cast<size_t>(qMax(0, m_window->currentFrame())),
            static_cast<size_t>(
                qMax(1, m_window->concurrentFrameCount())));
        if (!m_resources->ensureCheckerTextureUploaded(commandBuffer) ||
            !m_resources->ensureAuxiliaryImagesReadable(commandBuffer)) {
            signalConsumed();
            return false;
        }
        const jcut::vulkan_import::ExternalImage image =
            slot.importer->externalImage();
        if (!m_resources->setSampledImage(
                image.imageView, image.imageLayout)) {
            signalConsumed();
            return false;
        }
        slot.generation = frame.generation;
        m_gpuExportPreviewCurrentSlot = slotIndex;
        m_window->signalSemaphoreWhenFrameCompletes(slot.consumed);
        m_pendingGpuExportPreviewConsumptionState =
            frame.consumptionState;
        m_pendingGpuExportPreviewGeneration = frame.generation;
        m_pendingGpuExportPreviewSlot = slotIndex;
        m_pendingGpuExportPreviewProducerSessionId =
            frame.producerSessionId;
    }
    if (m_gpuExportPreviewCurrentSlot < 0) {
        return false;
    }

    const QSize size = m_window->swapChainImageSize();
    VkClearValue clears[2]{};
    clears[0].color.float32[0] = 0.055f;
    clears[0].color.float32[1] = 0.075f;
    clears[0].color.float32[2] = 0.105f;
    clears[0].color.float32[3] = 1.0f;
    clears[1].depthStencil.depth = 1.0f;
    VkRenderPassBeginInfo renderPass{};
    renderPass.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
    renderPass.renderPass = m_window->defaultRenderPass();
    renderPass.framebuffer = m_window->currentFramebuffer();
    renderPass.renderArea.extent = {
        static_cast<uint32_t>(qMax(1, size.width())),
        static_cast<uint32_t>(qMax(1, size.height()))};
    renderPass.clearValueCount =
        m_window->depthStencilFormat() == VK_FORMAT_UNDEFINED ? 1u : 2u;
    renderPass.pClearValues = clears;
    beginGpuExportPreviewRenderPass(commandBuffer, renderPass);
    VkViewport viewport{};
    viewport.width = static_cast<float>(qMax(1, size.width()));
    viewport.height = static_cast<float>(qMax(1, size.height()));
    viewport.maxDepth = 1.0f;
    VkRect2D scissor{};
    scissor.extent = {
        static_cast<uint32_t>(qMax(1, size.width())),
        static_cast<uint32_t>(qMax(1, size.height()))};
    VulkanPipeline::Push push{};
    m_pipeline->bindAndDraw(commandBuffer,
                            viewport,
                            scissor,
                            m_resources->descriptorSet(),
                            push);
    m_devFuncs->vkCmdEndRenderPass(commandBuffer);
    return true;
}

void DirectVulkanPreviewRenderer::destroyGpuExportPreviewResources()
{
    if (!m_window || m_window->device() == VK_NULL_HANDLE) {
        return;
    }
    vkQueueWaitIdle(m_window->graphicsQueue());
    for (GpuExportPreviewSlot& slot : m_gpuExportPreviewSlots) {
        if (slot.importer) {
            slot.importer->release();
            slot.importer.reset();
        }
        if (slot.ready != VK_NULL_HANDLE) {
            vkDestroySemaphore(
                m_window->device(), slot.ready, nullptr);
        }
        if (slot.consumed != VK_NULL_HANDLE) {
            vkDestroySemaphore(
                m_window->device(), slot.consumed, nullptr);
        }
        slot = {};
    }
    m_gpuExportPreviewCurrentSlot = -1;
    m_pendingGpuExportPreviewConsumptionState.reset();
    m_pendingGpuExportPreviewGeneration = 0;
    m_pendingGpuExportPreviewSlot = -1;
    m_pendingGpuExportPreviewProducerSessionId = 0;
    m_gpuExportPreviewProducerSessionId = 0;
    m_lastAcceptedGpuExportPreviewSequence = 0;
}

void DirectVulkanPreviewRenderer::clearGpuExportPreview()
{
    destroyGpuExportPreviewResources();
}
