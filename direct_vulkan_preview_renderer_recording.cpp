#include "direct_vulkan_preview_window_internal.h"

void DirectVulkanPreviewRenderer::startNextFrame()
{
    if (m_owner) {
        m_owner->beginPreviewFrame();
    }
    if (!m_owner || !m_window || !m_devFuncs) {
        if (m_owner && m_owner->stats()) {
            editor::accumulatePlaybackStageMetric(&m_owner->stats()->commandRecordingStageMetric,
                                          1,
                                          0,
                                          1,
                                          QStringLiteral("source_unavailable"),
                                          QStringLiteral("renderer_or_device_unavailable"));
        }
        // The native presenter requires exactly one frameReady() for every
        // startNextFrame() invocation, including an unavailable renderer path.
        // Failing to complete this callback permanently stalls preview
        // presentation.
        if (m_window) {
            m_window->frameReady();
        }
        if (m_owner) {
            m_owner->markPreviewUpdateDelivered();
        }
        return;
    }
    VkCommandBuffer cb = m_window->currentCommandBuffer();
    if (renderGpuExportPreview(cb)) {
        m_owner->markPresented();
        const bool submitted = m_window->frameReady();
        if (submitted && m_pendingGpuExportPreviewConsumptionState) {
            m_pendingGpuExportPreviewConsumptionState->completedGeneration.store(
                m_pendingGpuExportPreviewGeneration,
                std::memory_order_release);
            if (m_pendingGpuExportPreviewGeneration == 1) {
                qInfo().noquote()
                    << QStringLiteral(
                           "[render-export-preview] consumed GPU slot=%1 "
                           "producer_session=%2")
                           .arg(m_pendingGpuExportPreviewSlot)
                           .arg(m_pendingGpuExportPreviewProducerSessionId);
            }
        }
        m_pendingGpuExportPreviewConsumptionState.reset();
        m_pendingGpuExportPreviewGeneration = 0;
        m_pendingGpuExportPreviewSlot = -1;
        m_pendingGpuExportPreviewProducerSessionId = 0;
        m_owner->markPreviewUpdateDelivered();
        if (m_owner->hasGpuExportPreviewFrames()) {
            m_owner->schedulePreviewUpdate();
        }
        return;
    }
    if (m_owner->stats()) {
        editor::accumulatePlaybackStageMetric(&m_owner->stats()->commandRecordingStageMetric,
                                      1,
                                      0,
                                      0,
                                      QStringLiteral("recording_started"),
                                      QStringLiteral("start_next_frame"));
        editor::accumulatePlaybackStageMetric(&m_owner->stats()->presentationStageMetric,
                                      1,
                                      0,
                                      0,
                                      QStringLiteral("present_pending"),
                                      QStringLiteral("start_next_frame"));
    }

    const PreviewInteractionState* liveState = m_owner->state();
    PreviewInteractionState renderSnapshot;
    if (liveState) {
        // Latch a per-frame render snapshot so UI/overlay/status updates cannot mutate command recording inputs.
        renderSnapshot = *liveState;
    }
    const PreviewInteractionState* state = liveState ? &renderSnapshot : nullptr;
    QColor base = state ? state->backgroundColor : QColor(Qt::black);
    if (!base.isValid()) {
        base = QColor(Qt::black);
    }

    const float phase = state
        ? std::fmod(static_cast<float>(state->currentFramePosition), 180.0f) / 179.0f
        : 0.25f;
    const float clipFactor = state
        ? qBound(0.0f, static_cast<float>(state->clipCount) / 8.0f, 1.0f)
        : 0.0f;
    const float motion = (state && state->playing) ? phase : 0.25f;

    VkClearValue clearValues[2]{};
    clearValues[0].color.float32[0] = 0.08f + 0.22f * motion;
    clearValues[0].color.float32[1] = 0.10f + 0.18f * clipFactor;
    clearValues[0].color.float32[2] = 0.13f + 0.35f * (1.0f - motion);
    clearValues[0].color.float32[3] = 1.0f;
    clearValues[1].depthStencil.depth = 1.0f;
    clearValues[1].depthStencil.stencil = 0;

    VkRenderPassBeginInfo rp{};
    rp.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
    const QSize swapSize = m_window->swapChainImageSize();
    rp.renderPass = m_window->defaultRenderPass();
    rp.framebuffer = m_window->currentFramebuffer();
    rp.renderArea.offset = {0, 0};
    rp.renderArea.extent = {static_cast<uint32_t>(std::max(1, swapSize.width())),
                            static_cast<uint32_t>(std::max(1, swapSize.height()))};
    rp.clearValueCount = m_window->depthStencilFormat() == VK_FORMAT_UNDEFINED ? 1u : 2u;
    rp.pClearValues = clearValues;

    const uint32_t swapchainImageIndex =
        static_cast<uint32_t>(std::max(0, m_window->currentSwapChainImageIndex()));
    for (ReadbackSlot& slot : m_readbackSlots) {
        consumeReadbackSlot(&slot);
    }
    advanceRetiredClipHandoffResources();
    struct DecoderReadbackCandidate {
        VkImage image = VK_NULL_HANDLE;
        VkImageLayout layout = VK_IMAGE_LAYOUT_UNDEFINED;
        QSize size;
        VkFormat format = VK_FORMAT_UNDEFINED;
    } decoderReadbackCandidate;
    QHash<QString, DirectVulkanFrameHandoffPipeline::Result> frameHandoffResults;
    QHash<QString, render_detail::VulkanGradePayload> gradePayloads;
    QHash<QString, bool> curveLutUploadResults;
    QHash<QString, bool> maskCurveLutUploadResults;
    QHash<QString, bool> maskUploadResults;
    QHash<QString, bool> frameCrossfadeMaskUploadResults;
    QHash<QString, bool> frameCrossfadeCurveLutUploadResults;
    QHash<QString, bool> frameCrossfadeMaskCurveLutUploadResults;
    struct PreparedGpuMask {
        VkImageView view = VK_NULL_HANDLE;
        VkImageLayout layout = VK_IMAGE_LAYOUT_UNDEFINED;
    };
    QHash<QString, PreparedGpuMask> preparedGpuMasks;
    const auto bindOrPrepareGpuMask = [&](
        VulkanResources* resources,
        const jcut::core::ImageBuffer& mask,
        const jcut::core::ImageBuffer* previousMask,
        const jcut::core::ImageBuffer* nextMask,
        const VulkanMaskPreprocessOptions& options) {
        if (!resources) {
            return false;
        }
        const QSize outputSize =
            options.outputSize.isValid()
            ? options.outputSize
            : QSize(mask.size.width, mask.size.height);
        const QString key =
            vulkanMaskTextureCacheKey(options, outputSize);
        const auto prepared = preparedGpuMasks.constFind(key);
        if (!key.isEmpty() && prepared != preparedGpuMasks.cend()) {
            return resources->bindAuxiliaryImage(
                prepared->view, prepared->layout);
        }
        if (!resources->uploadMaskTexture(
                cb, mask, previousMask, nextMask, options)) {
            return false;
        }
        const PreparedGpuMask result{
            resources->preparedMaskImageView(),
            resources->preparedMaskImageLayout()};
        if (!key.isEmpty() && result.view != VK_NULL_HANDLE &&
            result.layout == VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL) {
            preparedGpuMasks.insert(key, result);
        }
        return true;
    };
    struct PreparedOverlayTexture {
        VulkanResources* resources = nullptr;
        QRectF bounds;
        bool ready = false;
    };
    PreparedTranscriptOverlayMap preparedTranscriptOverlays;
    QHash<QString, EvaluatedTitle> prepared3DTitleOverlays;
    PreparedOverlayTexture preparedPlaybackStatusOverlay;
    qint64 mediaOwnerHandoffAttemptCount = 0;
    qint64 mediaOwnerHandoffSuccessCount = 0;
    const bool forceChecker = qEnvironmentVariableIntValue("JCUT_VULKAN_PREVIEW_FORCE_CHECKER") == 1;
    const bool canDrawTexture = m_resources && m_pipeline && m_resources->isReady() &&
                                m_pipeline->isReady();
    if (state && !forceChecker && canDrawTexture) {
        const QVector<MediaOwnerHandoffPlanEntry> handoffPlan =
            mediaOwnerHandoffPlan(state->vulkanFrameStatuses);
        QSet<QString> activeHandoffClipIds;
        for (const MediaOwnerHandoffPlanEntry& entry : handoffPlan) {
            activeHandoffClipIds.insert(entry.mediaOwnerClipId);
            if (entry.providerStatusIndex >= 0 &&
                entry.providerStatusIndex < state->vulkanFrameStatuses.size()) {
                const VulkanPreviewClipFrameStatus& providerStatus =
                    state->vulkanFrameStatuses.at(entry.providerStatusIndex);
                if (providerStatus.frameCrossfadeActive &&
                    !providerStatus.frameCrossfadeFrame.isNull()) {
                    activeHandoffClipIds.insert(
                        entry.mediaOwnerClipId + QStringLiteral("#frameCrossfade"));
                }
            }
            for (const int statusIndex : entry.consumerStatusIndices) {
                const VulkanPreviewClipFrameStatus& status =
                    state->vulkanFrameStatuses.at(statusIndex);
                activeHandoffClipIds.insert(status.clipId);
                if (status.frameCrossfadeActive) {
                    activeHandoffClipIds.insert(status.clipId + QStringLiteral("#frameCrossfade"));
                }
                if (status.differenceMatteEnabled) {
                    activeHandoffClipIds.insert(status.clipId + QStringLiteral("#differenceReference"));
                }
                for (int i = 0; i < status.temporalEchoFrames.size(); ++i) {
                    activeHandoffClipIds.insert(
                        status.clipId + QStringLiteral("#temporalEcho%1").arg(i));
                }
            }
        }
        pruneClipHandoffResources(activeHandoffClipIds);
        updateClipHandoffResourceStats();

        const auto prepareBaseMediaOwner = [&](
            const VulkanPreviewClipFrameStatus& providerStatus,
            ClipHandoffResources* mediaOwnerResources) {
            DirectVulkanFrameHandoffPipeline::Result ownerResult;
            if (!mediaOwnerResources || !mediaOwnerResources->resources ||
                !mediaOwnerResources->pipeline) {
                return ownerResult;
            }
            if (providerStatus.frame.hasCpuImage() &&
                !providerStatus.frame.hasHardwareFrame() &&
                !providerStatus.externalVulkanFrame) {
                ownerResult.attempted = true;
                const auto cpuBuffer =
                    providerStatus.frame.cpuImageBuffer();
                ownerResult.sampledFrameReady =
                    cpuBuffer &&
                    mediaOwnerResources->resources->uploadImageTexture(
                        cb, *cpuBuffer);
                ownerResult.descriptorSet = ownerResult.sampledFrameReady
                    ? mediaOwnerResources->resources->descriptorSet()
                    : VK_NULL_HANDLE;
                ownerResult.descriptorSetIndex = static_cast<int>(
                    mediaOwnerResources->resources->descriptorSetIndex());
                ownerResult.descriptorSetCount = static_cast<int>(
                    mediaOwnerResources->resources->descriptorSetCount());
                const QSize providerFrameSize = providerStatus.frameSize.isValid()
                    ? providerStatus.frameSize
                    : providerStatus.frame.size();
                ownerResult.size = {providerFrameSize.width(), providerFrameSize.height()};
                ownerResult.format = VK_FORMAT_R8G8B8A8_UNORM;
                if (ownerResult.sampledFrameReady) {
                    ownerResult.image = mediaOwnerResources->resources->sampledImage();
                    ownerResult.imageView = mediaOwnerResources->resources->sampledImageView();
                    ownerResult.layout = mediaOwnerResources->resources->sampledImageLayout();
                }
                if (DirectVulkanPreviewStats* stats = m_owner ? m_owner->stats() : nullptr) {
                    ++stats->handoffAttempts;
                    if (ownerResult.sampledFrameReady) {
                        ++stats->handoffSuccesses;
                        ++stats->sampledImageReady;
                        stats->lastHandoffError.clear();
                        stats->lastHandoffMode = QStringLiteral("cpu_image_upload");
                        stats->lastExternalImageSize = providerFrameSize;
                        stats->lastVulkanImageFormat =
                            jcut::direct_vulkan_preview::vulkanFormatName(ownerResult.format);
                        stats->descriptorSetIndex = ownerResult.descriptorSetIndex;
                        stats->descriptorSetCount = ownerResult.descriptorSetCount;
                    } else {
                        ++stats->handoffFailures;
                        stats->lastHandoffMode = QStringLiteral("cpu_image_upload_failed");
                        stats->lastHandoffError = QStringLiteral(
                            "Failed to upload CPU media-owner frame to Vulkan texture.");
                    }
                }
            } else {
                ownerResult = mediaOwnerResources->pipeline->record(
                    cb,
                    swapchainImageIndex,
                    providerStatus,
                    mediaOwnerResources->resources.get(),
                    m_owner ? m_owner->stats() : nullptr);
            }
            return ownerResult;
        };
        const auto frameCrossfadeHandoffStatus = [](
            const VulkanPreviewClipFrameStatus& status,
            const QString& handoffKey) {
            VulkanPreviewClipFrameStatus secondaryStatus = status;
            secondaryStatus.clipId = handoffKey;
            secondaryStatus.frame = status.frameCrossfadeFrame;
            secondaryStatus.frameSize = status.frameCrossfadeFrameSize;
            secondaryStatus.requestedSourceFrame =
                status.frameCrossfadeRequestedSourceFrame;
            secondaryStatus.presentedSourceFrame =
                status.frameCrossfadePresentedSourceFrame;
            secondaryStatus.hasFrame = !secondaryStatus.frame.isNull();
            secondaryStatus.externalVulkanFrame = false;
            secondaryStatus.externalImage = VK_NULL_HANDLE;
            secondaryStatus.externalImageView = VK_NULL_HANDLE;
            secondaryStatus.externalImageMemory = VK_NULL_HANDLE;
            secondaryStatus.externalReadySemaphoreFd = -1;
            return secondaryStatus;
        };

        // Upload/import each decoded media payload exactly once. A hidden
        // parent can be the provider even though it is never composited; when
        // that status was omitted, the first child carries its cloned payload.
        QHash<QString, DirectVulkanFrameHandoffPipeline::Result> mediaOwnerHandoffResults;
        for (const MediaOwnerHandoffPlanEntry& entry : handoffPlan) {
            if (entry.providerStatusIndex < 0 ||
                entry.providerStatusIndex >= state->vulkanFrameStatuses.size()) {
                continue;
            }
            const VulkanPreviewClipFrameStatus& providerStatus =
                state->vulkanFrameStatuses.at(entry.providerStatusIndex);
            ClipHandoffResources* mediaOwnerResources =
                ensureClipHandoffResources(entry.mediaOwnerClipId);
            if (!mediaOwnerResources || !mediaOwnerResources->resources ||
                !mediaOwnerResources->pipeline ||
                !mediaOwnerResources->resources->beginFrameUploads(
                    swapchainImageIndex,
                    qMax<size_t>(VulkanResources::kDescriptorSetCount,
                                 static_cast<size_t>(swapchainImageIndex) + 1))) {
                continue;
            }
            DirectVulkanFrameHandoffPipeline::Result ownerResult =
                prepareBaseMediaOwner(providerStatus, mediaOwnerResources);
            mediaOwnerHandoffAttemptCount += ownerResult.attempted ? 1 : 0;
            mediaOwnerHandoffSuccessCount += ownerResult.sampledFrameReady ? 1 : 0;
            mediaOwnerHandoffResults.insert(entry.mediaOwnerClipId, ownerResult);
            frameHandoffResults.insert(entry.mediaOwnerClipId, ownerResult);
            mediaOwnerResources->resources->ensureAuxiliaryImagesReadable(cb);

            if (providerStatus.frameCrossfadeActive &&
                !providerStatus.frameCrossfadeFrame.isNull()) {
                const QString ownerSecondaryKey =
                    entry.mediaOwnerClipId + QStringLiteral("#frameCrossfade");
                ClipHandoffResources* ownerSecondaryResources =
                    ensureClipHandoffResources(ownerSecondaryKey);
                if (ownerSecondaryResources && ownerSecondaryResources->resources &&
                    ownerSecondaryResources->pipeline &&
                    ownerSecondaryResources->resources->beginFrameUploads(
                        swapchainImageIndex,
                        qMax<size_t>(VulkanResources::kDescriptorSetCount,
                                     static_cast<size_t>(swapchainImageIndex) + 1))) {
                    const VulkanPreviewClipFrameStatus ownerSecondaryStatus =
                        frameCrossfadeHandoffStatus(providerStatus, ownerSecondaryKey);
                    DirectVulkanFrameHandoffPipeline::Result secondaryOwnerResult =
                        prepareBaseMediaOwner(ownerSecondaryStatus, ownerSecondaryResources);
                    mediaOwnerHandoffAttemptCount += secondaryOwnerResult.attempted ? 1 : 0;
                    mediaOwnerHandoffSuccessCount +=
                        secondaryOwnerResult.sampledFrameReady ? 1 : 0;
                    mediaOwnerHandoffResults.insert(ownerSecondaryKey, secondaryOwnerResult);
                    frameHandoffResults.insert(ownerSecondaryKey, secondaryOwnerResult);
                    ownerSecondaryResources->resources->ensureAuxiliaryImagesReadable(cb);
                }
            }
        }

        for (const MediaOwnerHandoffPlanEntry& entry : handoffPlan) {
            if (entry.providerStatusIndex < 0 ||
                entry.providerStatusIndex >= state->vulkanFrameStatuses.size()) {
                continue;
            }
            const VulkanPreviewClipFrameStatus& providerStatus =
                state->vulkanFrameStatuses.at(entry.providerStatusIndex);
            const DirectVulkanFrameHandoffPipeline::Result ownerResult =
                mediaOwnerHandoffResults.value(entry.mediaOwnerClipId);
            for (const int statusIndex : entry.consumerStatusIndices) {
                const VulkanPreviewClipFrameStatus& status =
                    state->vulkanFrameStatuses.at(statusIndex);
                const render_detail::VulkanGradePayload gradePayload =
                    status.gradePayload;
                gradePayloads.insert(status.clipId, gradePayload);
                const QString mediaOwnerId = entry.mediaOwnerClipId;
            // A virtual mask child may reuse the parent's decoded image, but
            // it must own a descriptor set so its mask and curve bindings
            // cannot alter the parent's draw.
            const QString handoffResourceId = status.clipId;
            ClipHandoffResources* handoffResources = ensureClipHandoffResources(handoffResourceId);
            if (!handoffResources || !handoffResources->resources || !handoffResources->pipeline) {
                continue;
            }
            DirectVulkanFrameHandoffPipeline::Result handoffResult;
            if (handoffResourceId == mediaOwnerId) {
                handoffResult = ownerResult;
            } else {
                if (!handoffResources->resources->beginFrameUploads(
                        swapchainImageIndex,
                        qMax<size_t>(VulkanResources::kDescriptorSetCount,
                                     static_cast<size_t>(swapchainImageIndex) + 1))) {
                    continue;
                }
                const bool reusesParentMedia =
                    mediaOwnerPayloadMatches(providerStatus, status) &&
                    ownerResult.sampledFrameReady &&
                    ownerResult.imageView != VK_NULL_HANDLE;
                handoffResult = ownerResult;
                handoffResult.attempted = false;
                handoffResult.sampledFrameReady = reusesParentMedia &&
                    handoffResources->resources->setSampledImage(ownerResult.imageView,
                                                                  ownerResult.layout);
                handoffResult.descriptorSet = handoffResult.sampledFrameReady
                    ? handoffResources->resources->descriptorSet()
                    : VK_NULL_HANDLE;
                handoffResult.descriptorSetIndex =
                    static_cast<int>(handoffResources->resources->descriptorSetIndex());
                handoffResult.descriptorSetCount =
                    static_cast<int>(handoffResources->resources->descriptorSetCount());
            }
            frameHandoffResults.insert(status.clipId, handoffResult);
            const auto prepareAuxiliaryFrame = [&](const QString& key, const editor::FrameHandle& frame) {
                if (frame.isNull()) return;
                ClipHandoffResources* auxiliaryResources = ensureClipHandoffResources(key);
                if (!auxiliaryResources || !auxiliaryResources->resources || !auxiliaryResources->pipeline ||
                    !auxiliaryResources->resources->beginFrameUploads(
                        swapchainImageIndex,
                        qMax<size_t>(VulkanResources::kDescriptorSetCount,
                                     static_cast<size_t>(swapchainImageIndex) + 1))) return;
                VulkanPreviewClipFrameStatus auxiliaryStatus = status;
                auxiliaryStatus.clipId = key;
                auxiliaryStatus.frame = frame;
                auxiliaryStatus.frameSize = frame.size();
                auxiliaryStatus.hasFrame = true;
                auxiliaryStatus.externalVulkanFrame = false;
                DirectVulkanFrameHandoffPipeline::Result result;
                if (frame.hasCpuImage() && !frame.hasHardwareFrame()) {
                    result.attempted = true;
                    const auto cpuBuffer = frame.cpuImageBuffer();
                    result.sampledFrameReady =
                        cpuBuffer &&
                        auxiliaryResources->resources->uploadImageTexture(
                            cb, *cpuBuffer);
                    result.descriptorSet = result.sampledFrameReady
                        ? auxiliaryResources->resources->descriptorSet() : VK_NULL_HANDLE;
                    result.imageView = auxiliaryResources->resources->sampledImageView();
                    result.layout = auxiliaryResources->resources->sampledImageLayout();
                    result.size = {frame.size().width(), frame.size().height()};
                    result.format = VK_FORMAT_R8G8B8A8_UNORM;
                } else {
                    result = auxiliaryResources->pipeline->record(
                        cb, swapchainImageIndex, auxiliaryStatus,
                        auxiliaryResources->resources.get(), m_owner ? m_owner->stats() : nullptr);
                }
                frameHandoffResults.insert(key, result);
                auxiliaryResources->resources->ensureAuxiliaryImagesReadable(cb);
            };
            if (status.differenceMatteEnabled) {
                prepareAuxiliaryFrame(status.clipId + QStringLiteral("#differenceReference"),
                                      status.differenceReferenceFrame);
            }
            for (int i = 0; i < status.temporalEchoFrames.size(); ++i) {
                prepareAuxiliaryFrame(status.clipId + QStringLiteral("#temporalEcho%1").arg(i),
                                      status.temporalEchoFrames.at(i));
            }
            if (status.frameCrossfadeActive &&
                !status.frameCrossfadeFrame.isNull()) {
                const QString secondaryHandoffKey =
                    status.clipId + QStringLiteral("#frameCrossfade");
                const QString ownerSecondaryKey =
                    mediaOwnerId + QStringLiteral("#frameCrossfade");
                ClipHandoffResources* secondaryHandoffResources =
                    ensureClipHandoffResources(secondaryHandoffKey);
                if (secondaryHandoffResources &&
                    secondaryHandoffResources->resources &&
                    secondaryHandoffResources->pipeline) {
                    const VulkanPreviewClipFrameStatus secondaryStatus =
                        frameCrossfadeHandoffStatus(status, secondaryHandoffKey);
                    const VulkanPreviewClipFrameStatus providerSecondaryStatus =
                        frameCrossfadeHandoffStatus(providerStatus, ownerSecondaryKey);
                    const DirectVulkanFrameHandoffPipeline::Result secondaryOwnerResult =
                        mediaOwnerHandoffResults.value(ownerSecondaryKey);
                    DirectVulkanFrameHandoffPipeline::Result secondaryResult;
                    if (secondaryHandoffKey == ownerSecondaryKey) {
                        secondaryResult = secondaryOwnerResult;
                    } else if (secondaryHandoffResources->resources->beginFrameUploads(
                                   swapchainImageIndex,
                                   qMax<size_t>(VulkanResources::kDescriptorSetCount,
                                                static_cast<size_t>(swapchainImageIndex) + 1))) {
                        const bool reusesOwnerSecondary =
                            mediaOwnerPayloadMatches(providerSecondaryStatus, secondaryStatus) &&
                            secondaryOwnerResult.sampledFrameReady &&
                            secondaryOwnerResult.imageView != VK_NULL_HANDLE;
                        secondaryResult = secondaryOwnerResult;
                        secondaryResult.attempted = false;
                        secondaryResult.sampledFrameReady = reusesOwnerSecondary &&
                            secondaryHandoffResources->resources->setSampledImage(
                                secondaryOwnerResult.imageView,
                                secondaryOwnerResult.layout);
                        secondaryResult.descriptorSet = secondaryResult.sampledFrameReady
                            ? secondaryHandoffResources->resources->descriptorSet()
                            : VK_NULL_HANDLE;
                        secondaryResult.descriptorSetIndex = static_cast<int>(
                            secondaryHandoffResources->resources->descriptorSetIndex());
                        secondaryResult.descriptorSetCount = static_cast<int>(
                            secondaryHandoffResources->resources->descriptorSetCount());
                    } else {
                        secondaryResult = {};
                    }
                    frameHandoffResults.insert(secondaryHandoffKey, secondaryResult);
                    if (!status.maskClipSource &&
                        gradePayload.curveLutApplied) {
                        const QByteArray& secondaryCurveLut =
                            gradePayload.curveLutRgba;
                        if (!secondaryCurveLut.isEmpty()) {
                            frameCrossfadeCurveLutUploadResults.insert(
                                status.clipId,
                                secondaryHandoffResources->resources->uploadCurveLut(
                                    cb, secondaryCurveLut));
                        }
                    }
                    if (status.maskGradeEnabled &&
                        status.maskGradePayload.curveLutApplied) {
                        const QByteArray secondaryMaskCurveLut =
                            status.maskGradePayload.curveLutRgba;
                        if (!secondaryMaskCurveLut.isEmpty()) {
                            frameCrossfadeMaskCurveLutUploadResults.insert(
                                status.clipId,
                                secondaryHandoffResources->resources->uploadMaskCurveLut(
                                    cb, secondaryMaskCurveLut));
                        }
                    }
                    secondaryHandoffResources->resources
                        ->ensureAuxiliaryImagesReadable(cb);
                    if (status.frameCrossfadeMaskTextureEnabled &&
                        status.frameCrossfadeMaskBuffer) {
                        VulkanMaskPreprocessOptions secondaryMaskOptions;
                        secondaryMaskOptions.sourceIdentity =
                            status.frameCrossfadeMaskIdentity;
                        secondaryMaskOptions.correctionStorage =
                            status.maskCorrectionStorage;
                        secondaryMaskOptions.outputSize =
                            status.frameCrossfadeFrameSize.isValid()
                                ? status.frameCrossfadeFrameSize
                                : status.frameSize;
                        secondaryMaskOptions.invert = status.maskInvert;
                        secondaryMaskOptions.erodeRadius =
                            qRound(qMax<qreal>(0.0, status.maskErode));
                        secondaryMaskOptions.dilateRadius =
                            qRound(qMax<qreal>(0.0, status.maskDilate));
                        secondaryMaskOptions.blurRadius = qRound(
                            qMax<qreal>(status.maskFeather, status.maskBlur));
                        secondaryMaskOptions.temporalStabilizeEnabled = false;
                        frameCrossfadeMaskUploadResults.insert(
                            status.clipId,
                            bindOrPrepareGpuMask(
                                secondaryHandoffResources->resources.get(),
                                *status.frameCrossfadeMaskBuffer,
                                nullptr,
                                nullptr,
                                secondaryMaskOptions));
                    }
                }
            }
            const QByteArray& curveLut = gradePayload.curveLutRgba;
            if (!status.maskClipSource && !curveLut.isEmpty()) {
                const bool uploaded =
                    handoffResources->resources->uploadCurveLut(cb, curveLut);
                curveLutUploadResults.insert(status.clipId, uploaded);
            }
            if (status.maskGradeEnabled &&
                status.maskGradePayload.curveLutApplied) {
                const QByteArray maskCurveLut =
                    status.maskGradePayload.curveLutRgba;
                if (!maskCurveLut.isEmpty()) {
                    const bool uploaded =
                        handoffResources->resources->uploadMaskCurveLut(cb, maskCurveLut);
                    maskCurveLutUploadResults.insert(status.clipId, uploaded);
                }
            }
            handoffResources->resources->ensureAuxiliaryImagesReadable(cb);
            if (status.maskTextureEnabled && status.maskBuffer) {
                VulkanMaskPreprocessOptions maskOptions;
                maskOptions.sourceIdentity = status.maskIdentity;
                maskOptions.correctionStorage =
                    status.maskCorrectionStorage;
                maskOptions.outputSize = status.frameSize;
                maskOptions.invert = status.maskInvert;
                maskOptions.erodeRadius = qRound(qMax<qreal>(0.0, status.maskErode));
                maskOptions.dilateRadius = qRound(qMax<qreal>(0.0, status.maskDilate));
                maskOptions.blurRadius = qRound(qMax<qreal>(status.maskFeather, status.maskBlur));
                maskOptions.temporalStabilizeEnabled =
                    status.maskTemporalStabilizeEnabled;
                maskOptions.temporalStabilizeStrength = static_cast<float>(
                    qBound<qreal>(
                        0.0, status.maskTemporalStabilizeStrength, 1.0));
                maskOptions.temporalStabilizeMotionRadius = qBound(
                    0, status.maskTemporalStabilizeMotionRadius, 32);
                if (!status.temporalMaskIdentity.isEmpty()) {
                    maskOptions.sourceIdentity += QStringLiteral("|temporal=%1")
                        .arg(status.temporalMaskIdentity);
                }
                maskUploadResults.insert(
                    status.clipId,
                    bindOrPrepareGpuMask(
                        handoffResources->resources.get(),
                        *status.maskBuffer,
                        status.previousMaskBuffer.get(),
                        status.nextMaskBuffer.get(),
                        maskOptions));
            }
            if (handoffResult.sampledFrameReady) {
                handoffResult.descriptorSet = handoffResources->resources->descriptorSet();
                handoffResult.descriptorSetIndex =
                    static_cast<int>(handoffResources->resources->descriptorSetIndex());
                frameHandoffResults.insert(status.clipId, handoffResult);
            }
            }
        }
        updateClipHandoffResourceStats();
    }
    const bool canDrawOverlays = m_pipeline && m_pipeline->isReady();
    if (state && canDrawOverlays) {
        TranscriptOverlayCollectionStats transcriptCollectionStats;
        preparedTranscriptOverlays =
            collectPreparedTranscriptOverlays(state, swapSize, &transcriptCollectionStats);
        if (DirectVulkanPreviewStats* stats = m_owner ? m_owner->stats() : nullptr) {
            stats->transcriptCandidateCount = transcriptCollectionStats.candidateCount;
            stats->transcriptPreparedCount = transcriptCollectionStats.preparedCount;
            stats->lastTranscriptSkipReason = transcriptCollectionStats.lastSkipReason;
            stats->lastTranscriptClipId = transcriptCollectionStats.lastPreparedClipId;
            stats->lastTranscriptPath = transcriptCollectionStats.lastPreparedTranscriptPath;
            stats->lastTranscriptTimingSource = transcriptCollectionStats.lastPreparedTimingSource;
            stats->lastTranscriptTimelineSample = transcriptCollectionStats.lastPreparedTimelineSample;
            stats->lastTranscriptFrame = transcriptCollectionStats.lastPreparedTranscriptFrame;
            stats->lastTranscriptPresentedMediaSourceFrame =
                transcriptCollectionStats.lastPreparedPresentedMediaSourceFrame;
        }
        QSet<QString> activeTitleClipIds;
        int titleCandidateCount = 0;
        int titlePreparedCount = 0;
        QString lastTitleSkipReason;
        QString lastTitleClipId;
        for (const TimelineClip& clip : state->clips) {
            if (clip.titleKeyframes.isEmpty()) {
                continue;
            }
            const bool inClipRange =
                state->currentFramePosition >= static_cast<qreal>(clip.startFrame) &&
                state->currentFramePosition < static_cast<qreal>(clip.startFrame + clip.durationFrames);
            const bool standaloneTitle = clip.mediaType == ClipMediaType::Title;
            if (!inClipRange ||
                (standaloneTitle && !clipVisualPlaybackEnabled(clip, state->tracks))) {
                continue;
            }
            ++titleCandidateCount;
            activeTitleClipIds.insert(clip.id);
            lastTitleClipId = clip.id;
            if (clip.titleKeyframes.isEmpty()) {
                lastTitleSkipReason = QStringLiteral("title_keyframes_empty");
                continue;
            }
            const EffectiveVisualEffects effects =
                evaluateEffectiveVisualEffectsAtPosition(
                    clip,
                    state->tracks,
                    state->currentFramePosition,
                    state->renderSyncMarkers,
                    state->playbackTiming);
            if (effects.grading.opacity <= 0.001) {
                lastTitleSkipReason = QStringLiteral("title_zero_opacity");
                continue;
            }
            const EvaluatedTitle title = prepareRenderableTitleForVulkanText(
                clip,
                state->currentFramePosition,
                state->playbackTiming,
                static_cast<qreal>(effects.grading.opacity),
                state->outputSize);
            if (!title.valid) {
                lastTitleSkipReason = QStringLiteral("title_evaluated_invisible");
                continue;
            }
            prepared3DTitleOverlays.insert(clip.id, title);
            ++titlePreparedCount;
            lastTitleSkipReason.clear();
            continue;
        }
        if (DirectVulkanPreviewStats* stats = m_owner ? m_owner->stats() : nullptr) {
            stats->titleCandidateCount = titleCandidateCount;
            stats->titlePreparedCount = titlePreparedCount;
            stats->lastTitleSkipReason = lastTitleSkipReason;
            stats->lastTitleClipId = lastTitleClipId;
        }
        if (m_playbackStatusOverlayResources &&
            m_playbackStatusOverlayResources->isReady() &&
            m_playbackStatusOverlayResources->descriptorSet() != VK_NULL_HANDLE) {
            const QString statusText = state->playbackStatusOverlayText.trimmed();
            if (!statusText.isEmpty()) {
                const qreal statusProgress = state->playbackStatusOverlayProgress;
                const QString textureKey = playbackStatusOverlayTextureKey(swapSize, statusText, statusProgress);
                bool textureReady =
                    m_playbackStatusOverlayTextureReady &&
                    textureKey == m_playbackStatusOverlayTextureKey;
                if (!kAllowCpuRasterTextOverlaysInDirectVulkanPreview) {
                    textureReady = false;
                    m_playbackStatusOverlayTextureKey.clear();
                    m_playbackStatusOverlayTextureReady = false;
                }
                if (!textureReady) {
                    if (kAllowCpuRasterTextOverlaysInDirectVulkanPreview) {
                        const render_detail::OverlayImage overlayImage =
                            renderPlaybackStatusOverlay(swapSize, statusText, statusProgress);
                        textureReady = !overlayImage.isNull() &&
                            m_playbackStatusOverlayResources->uploadImageTexture(cb, overlayImage);
                        if (textureReady) {
                            m_playbackStatusOverlayTextureKey = textureKey;
                            m_playbackStatusOverlayTextureReady = true;
                        }
                    }
                }
                if (textureReady) {
                    preparedPlaybackStatusOverlay = PreparedOverlayTexture{
                        m_playbackStatusOverlayResources.get(),
                        QRectF(QPointF(0.0, 0.0), QSizeF(swapSize)),
                        true};
                }
            } else {
                m_playbackStatusOverlayTextureKey.clear();
                m_playbackStatusOverlayTextureReady = false;
            }
        }
    } else if (DirectVulkanPreviewStats* stats = m_owner ? m_owner->stats() : nullptr) {
        stats->transcriptCandidateCount = 0;
        stats->transcriptPreparedCount = 0;
        stats->transcriptDrawnCount = 0;
        stats->titleCandidateCount = 0;
        stats->titlePreparedCount = 0;
        stats->titleDrawnCount = 0;
        stats->lastTitleSkipReason = QStringLiteral("overlays_unavailable");
        stats->lastTranscriptSkipReason = QStringLiteral("overlays_unavailable");
    }
    render_detail::SpeakerLabelOverlaySpec preparedSpeakerSpec;
    bool preparedSpeakerLabel = false;
    render_detail::SpeakerLabelOverlaySpec preparedTemporalDebugSpec;
    bool preparedTemporalDebugLabel = false;
    QSet<QString> preparedTranscriptAtlasClipIds;
    QString textPrepFailureReason;
    const size_t textFrameSlotCount =
        qMax<size_t>(VulkanResources::kDescriptorSetCount,
                     static_cast<size_t>(swapchainImageIndex) + 1);
    auto beginTextFrameUploads = [&](VulkanTextRenderer* renderer, const QString& failureReason) {
        if (renderer &&
            renderer->isReady() &&
            !renderer->beginFrameUploads(swapchainImageIndex, textFrameSlotCount) &&
            textPrepFailureReason.isEmpty()) {
            textPrepFailureReason = failureReason;
        }
    };
    beginTextFrameUploads(m_textRenderer.get(),
                          QStringLiteral("transcript:text_upload_frame_slot_unavailable"));
    if (m_textRenderer && m_textRenderer->isReady()) {
        for (auto it = prepared3DTitleOverlays.cbegin(); it != prepared3DTitleOverlays.cend(); ++it) {
            if (!m_textRenderer->prepareTitleOverlayAtlas(cb, state->outputSize, it.value())) {
                textPrepFailureReason = QStringLiteral("title:%1").arg(m_textRenderer->lastFailureReason());
            }
        }
    }
    beginTextFrameUploads(m_speakerTextRenderer.get(),
                          QStringLiteral("speaker:text_upload_frame_slot_unavailable"));
    beginTextFrameUploads(m_temporalDebugTextRenderer.get(),
                          QStringLiteral("debug:text_upload_frame_slot_unavailable"));
    if (m_speakerTextRenderer &&
        m_speakerTextRenderer->isReady() &&
        (state->showCurrentSpeakerName || state->showCurrentSpeakerOrganization)) {
        preparedSpeakerSpec = currentSpeakerLabelOverlaySpecForState(state);
        const bool hasVisibleLabel =
            (preparedSpeakerSpec.showName && !preparedSpeakerSpec.name.trimmed().isEmpty()) ||
            (preparedSpeakerSpec.showOrganization && !preparedSpeakerSpec.organization.trimmed().isEmpty());
        if (hasVisibleLabel) {
            preparedSpeakerLabel =
                m_speakerTextRenderer->prepareSpeakerLabelAtlas(cb, state->outputSize, preparedSpeakerSpec);
            if (!preparedSpeakerLabel) {
                textPrepFailureReason = QStringLiteral("speaker:%1")
                    .arg(m_speakerTextRenderer->lastFailureReason());
            }
        }
    }
    if (m_temporalDebugTextRenderer &&
        m_temporalDebugTextRenderer->isReady() &&
        !state->temporalDebugOverlayText.trimmed().isEmpty()) {
        preparedTemporalDebugSpec.name = QStringLiteral("TEMPORAL DEBUG");
        preparedTemporalDebugSpec.organization = state->temporalDebugOverlayText.trimmed();
        preparedTemporalDebugSpec.showName = true;
        preparedTemporalDebugSpec.showOrganization = true;
        preparedTemporalDebugSpec.nameTextScale = 0.42;
        preparedTemporalDebugSpec.organizationTextScale = 0.36;
        preparedTemporalDebugSpec.nameVerticalPosition = 0.07;
        preparedTemporalDebugSpec.organizationVerticalPosition = 0.18;
        preparedTemporalDebugSpec.nameColor = QColor(QStringLiteral("#fff4cc"));
        preparedTemporalDebugSpec.organizationColor = QColor(QStringLiteral("#d6e7f7"));
        preparedTemporalDebugSpec.backgroundColor = QColor(4, 8, 14, 218);
        preparedTemporalDebugSpec.borderColor = QColor(255, 209, 102, 170);
        preparedTemporalDebugLabel =
            m_temporalDebugTextRenderer->prepareSpeakerLabelAtlas(cb, state->outputSize, preparedTemporalDebugSpec);
        if (!preparedTemporalDebugLabel) {
            textPrepFailureReason = QStringLiteral("debug:%1")
                .arg(m_temporalDebugTextRenderer->lastFailureReason());
        }
    }
    QString textPrepMaterial = transcriptOverlayTextPrepMaterial(preparedTranscriptOverlays, state->outputSize);
    textPrepMaterial += QStringLiteral("s:%1:%2:%3:%4:%5|")
                            .arg(preparedSpeakerSpec.name)
                            .arg(preparedSpeakerSpec.organization)
                            .arg(preparedSpeakerSpec.showName ? 1 : 0)
                            .arg(preparedSpeakerSpec.showOrganization ? 1 : 0)
                            .arg(preparedSpeakerSpec.fontFamily);
    textPrepMaterial += QStringLiteral("d:%1|").arg(preparedTemporalDebugSpec.organization);
    const QString textPrepKey = QString::fromLatin1(
        QCryptographicHash::hash(textPrepMaterial.toUtf8(), QCryptographicHash::Sha1).toHex());
    const bool textPrepCacheHit =
        m_lastPreparedTextReady &&
        !textPrepKey.isEmpty() &&
        textPrepKey == m_lastPreparedTextKey;
    if (textPrepCacheHit) {
        for (auto it = preparedTranscriptOverlays.cbegin(); it != preparedTranscriptOverlays.cend(); ++it) {
            if (it.value().ready) {
                preparedTranscriptAtlasClipIds.insert(it.key());
            }
        }
        preparedSpeakerLabel =
            (preparedSpeakerSpec.showName && !preparedSpeakerSpec.name.trimmed().isEmpty()) ||
            (preparedSpeakerSpec.showOrganization && !preparedSpeakerSpec.organization.trimmed().isEmpty());
        preparedTemporalDebugLabel = !preparedTemporalDebugSpec.organization.trimmed().isEmpty();
    } else {
        if (m_textRenderer && m_textRenderer->isReady()) {
            for (auto it = preparedTranscriptOverlays.cbegin(); it != preparedTranscriptOverlays.cend(); ++it) {
                const PreparedTranscriptOverlay& transcript = it.value();
                if (!transcript.ready) {
                    continue;
                }
                if (m_textRenderer->prepareTranscriptOverlayAtlas(cb,
                                                                  state->outputSize,
                                                                  transcript.clip,
                                                                  transcript.layout,
                                                                  transcript.outputRect,
                                                                  transcript.speakerTitle)) {
                    preparedTranscriptAtlasClipIds.insert(it.key());
                } else {
                    textPrepFailureReason = QStringLiteral("transcript:%1")
                        .arg(m_textRenderer->lastFailureReason());
                }
            }
        } else if (!preparedTranscriptOverlays.isEmpty()) {
            textPrepFailureReason = QStringLiteral("transcript:text_renderer_not_ready");
        }
        m_lastPreparedTextKey = textPrepKey;
        m_lastPreparedTextReady =
            !preparedTranscriptAtlasClipIds.isEmpty() ||
            preparedSpeakerLabel ||
            preparedTemporalDebugLabel;
    }
    if (DirectVulkanPreviewStats* stats = m_owner ? m_owner->stats() : nullptr) {
        stats->lastTextPrepFailureReason = textPrepFailureReason;
        stats->lastTextDrawFailureReason.clear();
    }
    if (m_owner->stats()) {
        const qint64 textAttemptCount =
            preparedTranscriptOverlays.size() +
            prepared3DTitleOverlays.size() +
            ((preparedSpeakerSpec.showName || preparedSpeakerSpec.showOrganization) ? 1 : 0) +
            (!preparedTemporalDebugSpec.organization.trimmed().isEmpty() ? 1 : 0);
        const qint64 textSuccessCount =
            preparedTranscriptAtlasClipIds.size() +
            prepared3DTitleOverlays.size() +
            (preparedSpeakerLabel ? 1 : 0) +
            (preparedTemporalDebugLabel ? 1 : 0);
        editor::accumulatePlaybackStageMetric(&m_owner->stats()->textPrepStageMetric,
                                      qMax<qint64>(1, textAttemptCount),
                                      textSuccessCount,
                                      qMax<qint64>(0, textAttemptCount - textSuccessCount),
                                      textAttemptCount > 0
                                          ? (textPrepCacheHit
                                                 ? QStringLiteral("text_prepare_cache_hit")
                                                 : QStringLiteral("text_prepared"))
                                          : QStringLiteral("text_not_requested"),
                                      QStringLiteral("transcript=%1 title=%2 speaker=%3 debug=%4 cache_hit=%5")
                                          .arg(preparedTranscriptAtlasClipIds.size())
                                          .arg(prepared3DTitleOverlays.size())
                                          .arg(preparedSpeakerLabel ? 1 : 0)
                                          .arg(preparedTemporalDebugLabel ? 1 : 0)
                                          .arg(textPrepCacheHit ? 1 : 0));
    }
    bool renderPassBegun = false;
    const auto beginRenderPass = [&]() {
        if (!renderPassBegun) {
            m_devFuncs->vkCmdBeginRenderPass(cb, &rp, VK_SUBPASS_CONTENTS_INLINE);
            renderPassBegun = true;
        }
    };
    const bool directAudioMode = state && state->viewMode == PreviewSurface::ViewMode::Audio;
    if (!directAudioMode) beginRenderPass();
    auto drawPreparedOverlay = [&](const PreparedOverlayTexture& overlay) {
        if (!overlay.ready ||
            !overlay.resources ||
            overlay.resources->descriptorSet() == VK_NULL_HANDLE ||
            !m_pipeline ||
            !m_pipeline->isReady()) {
            return;
        }
        VkViewport viewport{};
        viewport.x = 0.0f;
        viewport.y = 0.0f;
        viewport.width = static_cast<float>(std::max(1, swapSize.width()));
        viewport.height = static_cast<float>(std::max(1, swapSize.height()));
        viewport.minDepth = 0.0f;
        viewport.maxDepth = 1.0f;
        const QRectF& bounds = overlay.bounds;
        PreviewClipGeometry overlayGeometry;
        overlayGeometry.localRect = QRectF(-bounds.width() / 2.0,
                                           -bounds.height() / 2.0,
                                           bounds.width(),
                                           bounds.height());
        overlayGeometry.clipToScreen.translate(bounds.center().x(), bounds.center().y());
        overlayGeometry.bounds = bounds;
        VulkanPipeline::Push overlayPush{};
        mvpForVulkanClipTransform(overlayGeometry.clipToScreen,
                                  overlayGeometry.localRect,
                                  swapSize,
                                  overlayPush.mvp);
        VkRect2D overlayScissor{};
        overlayScissor.offset = {0, 0};
        overlayScissor.extent = {static_cast<uint32_t>(std::max(1, swapSize.width())),
                                 static_cast<uint32_t>(std::max(1, swapSize.height()))};
        m_pipeline->bindAndDraw(cb,
                                viewport,
                                overlayScissor,
                                overlay.resources->descriptorSet(),
                                overlayPush);
    };
    auto drawPreparedTitleOverlayForClip = [&](const QString& clipId, const QRectF& compositeRect) -> bool {
        const auto title3DIt = prepared3DTitleOverlays.constFind(clipId);
        if (title3DIt != prepared3DTitleOverlays.constEnd()) {
            if (!m_textRenderer || !m_textRenderer->isReady()) return false;
            const bool drawn = m_textRenderer->drawTitleOverlay3D(
                cb, swapSize, state->outputSize, compositeRect, title3DIt.value());
            if (drawn && m_owner && m_owner->stats()) ++m_owner->stats()->titleDrawnCount;
            return drawn;
        }
        return false;
    };
    QSet<QString> drawnTranscriptOverlayClipIds;
    auto drawPreparedTranscriptOverlayForClip = [&](const QString& clipId, const QRectF& compositeRect) -> bool {
        const auto transcriptOverlayIt = preparedTranscriptOverlays.constFind(clipId);
        if (transcriptOverlayIt != preparedTranscriptOverlays.constEnd() &&
            transcriptOverlayIt.value().ready &&
            preparedTranscriptAtlasClipIds.contains(clipId) &&
            m_textRenderer &&
            m_textRenderer->isReady()) {
            const PreparedTranscriptOverlay& transcript = transcriptOverlayIt.value();
            const bool drawn = m_textRenderer->drawTranscriptOverlay(cb,
                                                                     swapSize,
                                                                     state->outputSize,
                                                                     compositeRect,
                                                                     transcript.clip,
                                                                     transcript.layout,
                                                                     transcript.outputRect,
                                                                     transcript.speakerTitle,
                                                                     transcript.opacityMultiplier);
            if (drawn) {
                drawnTranscriptOverlayClipIds.insert(clipId);
                return true;
            }
            if (DirectVulkanPreviewStats* stats = m_owner ? m_owner->stats() : nullptr) {
                stats->lastTextDrawFailureReason = QStringLiteral("transcript:%1")
                    .arg(m_textRenderer->lastFailureReason());
            }
        }
        return false;
    };
    bool audioWaitingForWaveform = false;
    if (renderDirectVulkanAudioFrame(
            DirectVulkanAudioRenderContext{
                state, m_devFuncs, m_audioTab.get(), cb, swapSize, beginRenderPass},
            &audioWaitingForWaveform)) {
        drawPreparedOverlay(preparedPlaybackStatusOverlay);
        m_devFuncs->vkCmdEndRenderPass(cb);
        if (m_owner->stats()) {
            editor::accumulatePlaybackStageMetric(&m_owner->stats()->commandRecordingStageMetric,
                                          0,
                                          1,
                                          0,
                                          QStringLiteral("recorded"),
                                          QStringLiteral("audio_view_frame"));
        }
        m_owner->markPresentedSourceFrames(-1, -1);
        m_owner->markPresented(state);
        m_window->frameReady();
        m_owner->markPreviewUpdateDelivered();
        if (audioWaitingForWaveform && !state->playing) {
            m_owner->schedulePreviewUpdate();
        }
        return;
    }
    beginRenderPass();
    int64_t requestedSourceFrame = -1;
    int64_t presentedSourceFrame = -1;
    qint64 handoffAttemptCount = mediaOwnerHandoffAttemptCount;
    qint64 handoffSuccessCount = mediaOwnerHandoffSuccessCount;
    QSet<QString> submittedClipIds;
    QSet<QString> submittedCrossfadeClipIds;
    if (state) {
        VkViewport viewport{};
        viewport.x = 0.0f;
        viewport.y = 0.0f;
        viewport.width = static_cast<float>(std::max(1, swapSize.width()));
        viewport.height = static_cast<float>(std::max(1, swapSize.height()));
        viewport.minDepth = 0.0f;
        viewport.maxDepth = 1.0f;
        QHash<QString, PreviewClipGeometry> activeClipGeometry;
        const QRectF fullSwapRect(QPointF(0, 0), QSizeF(swapSize));
        const PreviewViewTransform viewTransform(fullSwapRect,
                                                 state->outputSize,
                                                 vulkanPreviewCanvasMarginPx(),
                                                 state->previewZoom,
                                                 state->previewPanOffset);
        const QRectF compositeRect = viewTransform.targetRect();
        const QPointF previewScale = viewTransform.outputScale();
        struct PendingMaskForegroundDraw {
            VkDescriptorSet descriptorSet = VK_NULL_HANDLE;
            VulkanPipeline::Push push;
            VkRect2D scissor{};
            uint32_t frameUniformDynamicOffset = 0;
        };
        QVector<PendingMaskForegroundDraw> pendingMaskForegroundDraws;
        VkClearValue canvasClear{};
        canvasClear.color.float32[0] = static_cast<float>(std::clamp<double>(base.redF(), 0.0, 1.0));
        canvasClear.color.float32[1] = static_cast<float>(std::clamp<double>(base.greenF(), 0.0, 1.0));
        canvasClear.color.float32[2] = static_cast<float>(std::clamp<double>(base.blueF(), 0.0, 1.0));
        canvasClear.color.float32[3] = 1.0f;
        clearRect(m_devFuncs, cb, canvasClear, clearRectFromQRect(compositeRect, swapSize));
        VkClearValue canvasBorder{};
        canvasBorder.color.float32[0] = 0.22f;
        canvasBorder.color.float32[1] = 0.28f;
        canvasBorder.color.float32[2] = 0.35f;
        canvasBorder.color.float32[3] = 1.0f;
        clearBoxOutline(m_devFuncs,
                        cb,
                        canvasBorder,
                        clearRectFromQRect(compositeRect.adjusted(-1, -1, 1, 1), swapSize),
                        std::max(1, std::min(swapSize.width(), swapSize.height()) / 360));
        QSet<QString> drawnTitleOverlayClipIds;
        for (const TimelineClip& clip : state->clips) {
            if (clip.mediaType == ClipMediaType::Title) {
                if (drawPreparedTitleOverlayForClip(clip.id, compositeRect)) {
                    drawnTitleOverlayClipIds.insert(clip.id);
                }
                if (clip.id == state->selectedClipId && state->titleOverlayInteractionOnly) {
                    const int selectionThickness = std::max(2, std::min(swapSize.width(), swapSize.height()) / 360);
                    clearBoxOutline(m_devFuncs,
                                    cb,
                                    selectionOutlineColor(),
                                    clearRectFromQRect(compositeRect, swapSize),
                                    selectionThickness);
                }
                continue;
            }
            const VulkanPreviewClipFrameStatus* status = frameStatusForClip(state, clip.id);
            if (!status || !status->active || status->drawSuppressed) {
                continue;
            }
            if (requestedSourceFrame < 0) {
                requestedSourceFrame = status->requestedSourceFrame;
            }
            if (status->maskClipSource) {
                const bool ownerMappingValid =
                    !status->mediaOwnerClipId.trimmed().isEmpty() &&
                    status->timingOwnerClipId == status->mediaOwnerClipId &&
                    status->effectsOwnerClipId == clip.id &&
                    status->matteOwnerClipId == clip.id;
                if (!ownerMappingValid) {
                    if (DirectVulkanPreviewStats* stats = m_owner->stats()) {
                        stats->lastUnsupportedEffect =
                            QStringLiteral("invalid_mask_owner_mapping");
                    }
                    continue;
                }
            }
            const bool selected = !state->selectedClipId.isEmpty() && clip.id == state->selectedClipId;
            VkClearAttachment attachment{};
            attachment.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
            attachment.colorAttachment = 0;
            attachment.clearValue = clipColorForStatus(clip, activeClipGeometry.size(), selected, status);
            const QSize frameSize = (status && status->frameSize.isValid()) ? status->frameSize : QSize();
            const QRectF fitted = viewTransform.fittedClipRect(clip.sourceFrameSize, frameSize);
            const TimelineClip::TransformKeyframe transform =
                transformWithTransientOverride(state, clip.id, status->transform);
            const PreviewClipGeometry clipGeometry =
                PreviewViewTransform::clipGeometry(
                    fitted,
                    previewScale,
                    QPointF(transform.translationX, transform.translationY),
                    transform.rotation,
                    QPointF(transform.scaleX, transform.scaleY));
            PreviewClipGeometry effectiveClipGeometry = clipGeometry;
            if (status->sampledFrameNeedsYFlip) {
                effectiveClipGeometry.clipToScreen.scale(1.0, -1.0);
                effectiveClipGeometry.bounds =
                    effectiveClipGeometry.clipToScreen.mapRect(effectiveClipGeometry.localRect);
            }
            const QRectF transformedBounds = effectiveClipGeometry.bounds;
            const VkClearRect rect = clearRectFromQRect(transformedBounds, swapSize);
            const DirectVulkanFrameHandoffPipeline::Result handoffResult =
                status ? frameHandoffResults.value(status->clipId) : DirectVulkanFrameHandoffPipeline::Result{};
            const DirectVulkanFrameHandoffPipeline::Result secondaryHandoffResult =
                status && status->frameCrossfadeActive
                    ? frameHandoffResults.value(status->clipId + QStringLiteral("#frameCrossfade"))
                    : DirectVulkanFrameHandoffPipeline::Result{};
            VulkanResources* sampledResources = nullptr;
            if (status) {
                const QString resourceId = status->clipId;
                auto resourcesIt = m_clipHandoffResources.constFind(resourceId);
                if (resourcesIt != m_clipHandoffResources.cend() && resourcesIt.value()) {
                    sampledResources = resourcesIt.value()->resources.get();
                }
            }
            const bool sampledFrameReady =
                handoffResult.sampledFrameReady && handoffResult.descriptorSet != VK_NULL_HANDLE;
            const QString mediaOwnerClipId = status
                ? status->mediaOwnerClipId.trimmed()
                : QString();
            const bool handoffAttempted = handoffResult.attempted ||
                (!mediaOwnerClipId.isEmpty() &&
                 frameHandoffResults.value(mediaOwnerClipId).attempted);
            if (sampledFrameReady) {
                decoderReadbackCandidate.image = handoffResult.image;
                decoderReadbackCandidate.layout = handoffResult.layout;
                decoderReadbackCandidate.size = toQSize(handoffResult.size);
                decoderReadbackCandidate.format = handoffResult.format;
            }
            const bool statusHasDrawableFrame = status && status->hasFrame;
            if (canDrawTexture && sampledFrameReady && statusHasDrawableFrame) {
                if (DirectVulkanPreviewStats* stats = m_owner->stats()) {
                    ++stats->textureDraws;
                    ++stats->activeClipDraws;
                }
                const TimelineClip* effectsOwner =
                    clipForId(state, status->effectsOwnerClipId);
                if (!effectsOwner) {
                    if (DirectVulkanPreviewStats* stats = m_owner->stats()) {
                        stats->lastUnsupportedEffect =
                            QStringLiteral("effects_owner_missing");
                    }
                    continue;
                }
                const TimelineClip effectClip = clipWithResolvedTimingOwner(
                    evaluateClipEffectAnimationAtPosition(
                        clipWithRenderableEffectSettings(
                            *effectsOwner, state->tracks),
                        state->currentFramePosition,
                        state->renderSyncMarkers,
                        state->playbackTiming),
                    state->clips);
                const bool clipEdgeFillEffect =
                    effectClip.edgeFillEffect != BackgroundFillEffect::None &&
                    render_detail::vulkanClipSupportsBackgroundFillSource(clip) &&
                    !(status && status->maskClipSource);
                const bool progressiveStretchOwnsClipBackground =
                    clipEdgeFillEffect &&
                    (effectClip.edgeFillEffect == BackgroundFillEffect::ProgressiveEdgeStretch ||
                     effectClip.edgeFillEffect ==
                         BackgroundFillEffect::ProgressiveBidirectionalEdgeStretch);
                PendingMaskForegroundDraw bidirectionalEdgeDraw;
                bool bidirectionalEdgeDrawPending = false;
                if (clipEdgeFillEffect) {
                    const BackgroundFillEffect effectiveFillEffect =
                        effectClip.edgeFillEffect;
                    const bool fullCanvasFill =
                        effectiveFillEffect == BackgroundFillEffect::EdgeStretch ||
                        effectiveFillEffect == BackgroundFillEffect::ProgressiveEdgeStretch ||
                        effectiveFillEffect ==
                            BackgroundFillEffect::ProgressiveBidirectionalEdgeStretch ||
                        effectiveFillEffect == BackgroundFillEffect::Tile ||
                        effectiveFillEffect == BackgroundFillEffect::Mirror;
                    PreviewClipGeometry backgroundGeometry =
                        fullCanvasFill ? PreviewViewTransform::clipGeometry(
                                             compositeRect,
                                             QPointF(1.0, 1.0),
                                             QPointF(),
                                             0.0,
                                             QPointF(1.0, 1.0))
                                       : effectiveClipGeometry;
                    if (effectiveFillEffect == BackgroundFillEffect::BlurCover) {
                        const qreal coverScale = std::max<qreal>(
                            1.0,
                            std::max(
                                compositeRect.width() / qMax<qreal>(1.0, effectiveClipGeometry.bounds.width()),
                                compositeRect.height() / qMax<qreal>(1.0, effectiveClipGeometry.bounds.height())));
                        backgroundGeometry.clipToScreen.scale(coverScale * 1.08, coverScale * 1.08);
                        backgroundGeometry.bounds =
                            backgroundGeometry.clipToScreen.mapRect(backgroundGeometry.localRect);
                    }
                    const bool progressiveRenderSpaceFill =
                        effectiveFillEffect == BackgroundFillEffect::ProgressiveEdgeStretch ||
                        effectiveFillEffect ==
                            BackgroundFillEffect::ProgressiveBidirectionalEdgeStretch;
                    const QSize renderOutputSize =
                        state->outputSize.isValid() ? state->outputSize : compositeRect.size().toSize();
                    const QSize renderSourceSize =
                        clip.sourceFrameSize.isValid()
                            ? clip.sourceFrameSize
                            : (frameSize.isValid() ? frameSize : renderOutputSize);
                    const QRectF renderOutputRect(QPointF(0.0, 0.0), QSizeF(renderOutputSize));
                    const QRectF renderFitted = render_detail::fitRectF(renderSourceSize, renderOutputSize);
                    PreviewClipGeometry renderClipGeometry =
                        PreviewViewTransform::clipGeometry(
                            renderFitted,
                            QPointF(1.0, 1.0),
                            QPointF(transform.translationX, transform.translationY),
                            transform.rotation,
                            QPointF(transform.scaleX, transform.scaleY));
                    if (status->sampledFrameNeedsYFlip) {
                        renderClipGeometry.clipToScreen.scale(1.0, -1.0);
                        renderClipGeometry.bounds =
                            renderClipGeometry.clipToScreen.mapRect(renderClipGeometry.localRect);
                    }
                    const render_detail::VulkanGradePayload gradePayload =
                        gradePayloads.value(status->clipId);
                    const render_detail::VulkanDrawEffectState& baseEffects =
                        gradePayload.effects;
                    VulkanPipeline::Push backgroundPush{};
                    // Progressive edge stretch is a clip effect, but its scan
                    // and edge band are defined in render/output pixels.  The
                    // preview transform only presents the already-defined
                    // output area; it must not change shader sampling.
                    uint32_t backgroundFrameUniformOffset = 0;
                    const float backgroundGrade[4] = {
                        baseEffects.brightness,
                        baseEffects.contrast,
                        baseEffects.saturation,
                        0.0f};
                    if (sampledResources &&
                        sampledResources->updateFrameUniform(progressiveRenderSpaceFill
                                                                 ? renderOutputSize
                                                                 : compositeRect.size().toSize(),
                                                             baseEffects.shadows,
                                                             baseEffects.midtones,
                                                             baseEffects.highlights,
                                                             backgroundGrade)) {
                        backgroundFrameUniformOffset = sampledResources->frameUniformDynamicOffset();
                    }
                    mvpForVulkanClipTransform(backgroundGeometry.clipToScreen,
                                              backgroundGeometry.localRect,
                                              swapSize,
                                              backgroundPush.mvp);
                    const int edgePixels =
                        qBound(1, effectClip.edgeFillPixels, 512);
                    const qreal edgePower =
                        qBound<qreal>(0.25, effectClip.edgeFillPower, 8.0);
                    const render_detail::VulkanDrawEffectState backgroundEffects =
                        render_detail::vulkanBackgroundFillEffectState(
                            effectiveFillEffect,
                            static_cast<float>(effectClip.edgeFillOpacity),
                            static_cast<float>(effectClip.edgeFillBrightness),
                            static_cast<float>(effectClip.edgeFillSaturation),
                            edgePixels,
                            static_cast<float>(edgePower),
                            status->frame.validTextureRectNormalized(),
                            progressiveRenderSpaceFill
                                ? render_detail::vulkanBackgroundFillMapping(
                                      renderClipGeometry.clipToScreen,
                                      renderClipGeometry.localRect,
                                      renderOutputRect)
                                : render_detail::vulkanBackgroundFillMapping(
                                      effectiveClipGeometry.clipToScreen,
                                      effectiveClipGeometry.localRect,
                                      compositeRect));
                    backgroundPush.opacity = backgroundEffects.opacity;
                    backgroundPush.brightness = backgroundEffects.brightness;
                    backgroundPush.contrast = backgroundEffects.contrast;
                    backgroundPush.saturation = backgroundEffects.saturation;
                    backgroundPush.shadows[0] = backgroundEffects.shadows[0];
                    backgroundPush.shadows[1] = backgroundEffects.shadows[1];
                    backgroundPush.shadows[2] = backgroundEffects.shadows[2];
                    backgroundPush.shadows[3] = backgroundEffects.shadows[3];
                    backgroundPush.midtones[0] = backgroundEffects.midtones[0];
                    backgroundPush.midtones[1] = backgroundEffects.midtones[1];
                    backgroundPush.midtones[2] = backgroundEffects.midtones[2];
                    backgroundPush.midtones[3] = backgroundEffects.midtones[3];
                    backgroundPush.highlights[0] = backgroundEffects.highlights[0];
                    backgroundPush.highlights[1] = backgroundEffects.highlights[1];
                    backgroundPush.highlights[2] = backgroundEffects.highlights[2];
                    backgroundPush.highlights[3] = backgroundEffects.highlights[3];
                    VkRect2D backgroundScissor{};
                    if (state->hideOutsideOutputWindow) {
                        backgroundScissor = scissorFromQRect(compositeRect, swapSize);
                    } else {
                        backgroundScissor.offset = {0, 0};
                        backgroundScissor.extent = {static_cast<uint32_t>(std::max(1, swapSize.width())),
                                                    static_cast<uint32_t>(std::max(1, swapSize.height()))};
                    }
                    if (effectiveFillEffect ==
                        BackgroundFillEffect::ProgressiveBidirectionalEdgeStretch) {
                        bidirectionalEdgeDraw = PendingMaskForegroundDraw{
                            handoffResult.descriptorSet,
                            backgroundPush,
                            backgroundScissor,
                            backgroundFrameUniformOffset};
                        bidirectionalEdgeDrawPending = true;
                    } else {
                        m_pipeline->bindAndDraw(cb,
                                                viewport,
                                                backgroundScissor,
                                                handoffResult.descriptorSet,
                                                backgroundPush,
                                                backgroundFrameUniformOffset);
                    }
                }
                VulkanPipeline::Push push{};
                mvpForVulkanClipTransform(effectiveClipGeometry.clipToScreen,
                                          effectiveClipGeometry.localRect,
                                          swapSize,
                                          push.mvp);
                if (status) {
                    const render_detail::VulkanGradePayload gradePayload =
                        gradePayloads.value(status->clipId);
                    const render_detail::VulkanDrawEffectState& effects =
                        gradePayload.effects;
                    push.brightness = effects.brightness;
                    push.contrast = effects.contrast;
                    push.saturation = effects.saturation;
                    push.opacity = effects.opacity;
                    push.shadows[0] = effects.shadows[0];
                    push.shadows[1] = effects.shadows[1];
                    push.shadows[2] = effects.shadows[2];
                    push.midtones[0] = effects.midtones[0];
                    push.midtones[1] = effects.midtones[1];
                    push.midtones[2] = effects.midtones[2];
                    push.highlights[0] = effects.highlights[0];
                    push.highlights[1] = effects.highlights[1];
                    push.highlights[2] = effects.highlights[2];
                    push.shadows[3] = gradePayload.curveLutApplied
                        ? render_detail::kVulkanEffectModeCurve
                        : render_detail::kVulkanEffectModeNormal;
                    push.midtones[3] = static_cast<float>(std::max<qreal>(0.0, status->maskFeather));
                    // Pack falloff profile and power into the otherwise unused
                    // positive mask parameter: profile * 10 + exponent.
                    push.highlights[3] = static_cast<float>(
                        qBound(0, status->maskFeatherFalloff, 5) * 10.0 +
                        std::clamp<qreal>(status->maskFeatherGamma, 0.1, 5.0));
                    if (DirectVulkanPreviewStats* stats = m_owner->stats()) {
                        stats->lastEffectsPath = status->effectsPath;
                        stats->lastTargetRect = compositeRect;
                        stats->lastFittedRect = fitted;
                        stats->lastAppliedBrightness = status->grading.brightness;
                        stats->lastAppliedContrast = status->grading.contrast;
                        stats->lastAppliedSaturation = status->grading.saturation;
                        stats->lastAppliedOpacity = status->grading.opacity;
                        stats->lastAppliedRotation = transform.rotation;
                        stats->lastAppliedScaleX = transform.scaleX;
                        stats->lastAppliedScaleY = transform.scaleY;
                        stats->lastCurveLutApplied =
                            status->gradePayload.curveLutApplied;
                        if (status->correctionPolygonCount > 0 && !status->correctionsSupported) {
                            stats->lastUnsupportedEffect = QStringLiteral("correction_masks");
                        } else if (stats->lastUnsupportedEffect != QStringLiteral("curve_lut_upload_failed")) {
                            stats->lastUnsupportedEffect.clear();
                        }
                    }
                }
                VkRect2D scissor{};
                if (state->hideOutsideOutputWindow) {
                    scissor = scissorFromQRect(compositeRect, swapSize);
                } else {
                    scissor.offset = {0, 0};
                    scissor.extent = {static_cast<uint32_t>(std::max(1, swapSize.width())),
                                      static_cast<uint32_t>(std::max(1, swapSize.height()))};
                }
                const float maskEdgeParams[4] = {
                    status ? static_cast<float>(std::clamp(status->maskEdgeGrayAmount, 0.0, 1.0)) : 0.0f,
                    status ? static_cast<float>(std::clamp(status->maskEdgeGrayWidth, 0.001, 0.5)) : 0.25f,
                    status ? static_cast<float>(std::clamp(status->maskEdgeGrayGamma, 0.1, 8.0)) : 1.0f,
                    0.0f};
                if (sampledResources) {
                    sampledResources->updateFrameUniform(compositeRect.size().toSize());
                }
                auto uniformOffsetForDraw = [&](const float* effectParams = nullptr) {
                    if (sampledResources && effectParams &&
                        sampledResources->updateFrameUniform(
                            compositeRect.size().toSize(),
                            nullptr,
                            nullptr,
                            nullptr,
                            nullptr,
                            effectParams)) {
                        return sampledResources->frameUniformDynamicOffset();
                    }
                    return sampledResources ? sampledResources->frameUniformDynamicOffset() : 0u;
                };
                auto drawPush = [&](const VulkanPipeline::Push& drawState,
                                    const float* effectParams = nullptr) {
                    m_pipeline->bindAndDraw(cb,
                                            viewport,
                                            scissor,
                                            handoffResult.descriptorSet,
                                            drawState,
                                            uniformOffsetForDraw(effectParams));
                };
                const bool maskReady =
                    status && status->maskTextureEnabled &&
                    maskUploadResults.value(status->clipId, false);
                const auto drawMaskShadow = [&](const VulkanPipeline::Push& maskedPush) {
                    if (!maskReady || !status || !status->maskDropShadowEnabled ||
                        status->maskDropShadowOpacity <= 0.0) {
                        return;
                    }
                    VulkanPipeline::Push shadowPush = maskedPush;
                    shadowPush.mvp[12] += static_cast<float>(
                        2.0 * status->maskDropShadowOffsetX /
                        std::max(1, swapSize.width()));
                    shadowPush.mvp[13] += static_cast<float>(
                        2.0 * status->maskDropShadowOffsetY /
                        std::max(1, swapSize.height()));
                    shadowPush.brightness = 0.0f;
                    shadowPush.contrast = 1.0f;
                    shadowPush.saturation = 1.0f;
                    shadowPush.opacity *= static_cast<float>(
                        std::clamp(status->maskDropShadowOpacity, 0.0, 1.0));
                    shadowPush.shadows[0] = 0.0f;
                    shadowPush.shadows[1] = 0.0f;
                    shadowPush.shadows[2] = 0.0f;
                    shadowPush.shadows[3] = render_detail::kVulkanEffectModeMaskShadow;
                    shadowPush.midtones[0] = 0.0f;
                    shadowPush.midtones[1] = 0.0f;
                    shadowPush.midtones[2] = 0.0f;
                    shadowPush.midtones[3] = static_cast<float>(
                        std::clamp(status->maskDropShadowRadius, 0.0, 200.0));
                    drawPush(shadowPush);
                };
                // A virtual mask clip is an explicitly masked overlay, never a
                // second ordinary media layer. If its matte is unavailable for
                // this frame, fail closed instead of grading the entire source
                // rectangle. The parent remains independently drawable.
                if (status->maskClipSource && !maskReady) {
                    if (DirectVulkanPreviewStats* stats = m_owner->stats()) {
                        stats->lastUnsupportedEffect = QStringLiteral("mask_texture_unavailable");
                    }
                    continue;
                }
                if (maskReady && status->maskShowOnly) {
                    VulkanPipeline::Push maskPush = push;
                    maskPush.brightness = 0.0f;
                    maskPush.contrast = 1.0f;
                    maskPush.saturation = 1.0f;
                    maskPush.opacity = static_cast<float>(std::clamp(status->maskOpacity, 0.0, 1.0));
                    maskPush.shadows[3] = render_detail::kVulkanEffectModeMaskOnly;
                    drawPush(maskPush, maskEdgeParams);
                } else {
                    VulkanPipeline::Push basePush = push;
                    if (status && !status->maskClipSource &&
                        status->gradePayload.curveLutApplied &&
                        !curveLutUploadResults.value(status->clipId, false)) {
                        basePush.shadows[3] = render_detail::kVulkanEffectModeNormal;
                        if (DirectVulkanPreviewStats* stats = m_owner->stats()) {
                            stats->lastUnsupportedEffect = QStringLiteral("curve_lut_upload_failed");
                        }
                    }
                    if (maskReady && status->maskClipSource) {
                        basePush.shadows[3] = render_detail::kVulkanEffectModeMaskGrade;
                        basePush.opacity *= static_cast<float>(
                            std::clamp(status->maskOpacity, 0.0, 1.0));
                        basePush.midtones[3] = 0.0f;
                        if (status->maskGradeEnabled) {
                            basePush.brightness =
                                status->maskGradePayload.effects.brightness;
                            basePush.contrast =
                                status->maskGradePayload.effects.contrast;
                            basePush.saturation =
                                status->maskGradePayload.effects.saturation;
                            if (maskCurveLutUploadResults.value(status->clipId, false)) {
                                basePush.midtones[3] = render_detail::kVulkanMaskGradeUseSelectedCurveLut;
                            } else if (
                                status->maskGradePayload.curveLutApplied) {
                                if (DirectVulkanPreviewStats* stats = m_owner->stats()) {
                                    stats->lastUnsupportedEffect = QStringLiteral("mask_curve_lut_upload_failed");
                                }
                            }
                        }
                        drawMaskShadow(basePush);
                    }
                    if (status && status->differenceMatteEnabled && sampledResources) {
                        const auto referenceResult = frameHandoffResults.value(
                            status->clipId + QStringLiteral("#differenceReference"));
                        if (referenceResult.sampledFrameReady && referenceResult.imageView != VK_NULL_HANDLE &&
                            sampledResources->bindAuxiliaryImage(referenceResult.imageView, referenceResult.layout)) {
                            basePush.shadows[3] = render_detail::kVulkanEffectModeDifferenceMatte;
                            basePush.midtones[3] = static_cast<float>(qBound<qreal>(0.0, status->differenceThreshold, 1.0));
                            basePush.highlights[3] = static_cast<float>(qBound<qreal>(0.0, status->differenceSoftness, 1.0));
                        }
                    }
                    TimelineClip foregroundEffectClip = effectClip;
                    if (progressiveStretchOwnsClipBackground) {
                        foregroundEffectClip.effectPreset =
                            ClipEffectPreset::None;
                        foregroundEffectClip.maskRepeatEnabled = false;
                    }
                    const QRectF effectBounds =
                        (foregroundEffectClip.effectPreset == ClipEffectPreset::SourceTile ||
                         foregroundEffectClip.maskRepeatEnabled)
                            ? transformedBounds.intersected(compositeRect)
                            : compositeRect;
                    const render_detail::VulkanEffectPipelinePlan effectPlan =
                        status
                            ? status->effectPlan
                            : render_detail::VulkanEffectPipelinePlan{};
                    if (effectPlan.usesGeneratedDraws()) {
                        const VkRect2D generatedScissor =
                            foregroundEffectClip.effectPreset == ClipEffectPreset::SourceTile
                                ? scissorFromQRect(effectBounds, swapSize)
                                : scissor;
                        for (const render_detail::VulkanEffectPipelinePlan::DrawPass& effectDraw :
                             effectPlan.generatedDraws) {
                            VulkanPipeline::Push effectPush = basePush;
                            effectPush.opacity *= effectDraw.opacityMultiplier;
                            if (effectDraw.shaderMode == render_detail::kVulkanEffectModeMaskGrade &&
                                status && !status->maskClipSource) {
                                effectPush.opacity *= static_cast<float>(
                                    std::clamp(status->maskOpacity, 0.0, 1.0));
                            }
                            if (!status || !status->maskClipSource) {
                                effectPush.shadows[3] = effectDraw.shaderMode;
                            }
                            if (effectDraw.shaderMode >= render_detail::kVulkanEffectModeSpeakerMaskDilation &&
                                effectDraw.shaderMode <= render_detail::kVulkanEffectModeSpeakerMaskDilationRings) {
                                std::copy_n(effectDraw.palette, 3, effectPush.shadows);
                                std::copy_n(effectDraw.palette + 3, 3, effectPush.midtones);
                                std::copy_n(effectDraw.palette + 6, 3, effectPush.highlights);
                                effectPush.shadows[3] = effectDraw.shaderMode;
                            }
                            QRectF previewDrawRect = effectDraw.outputRect;
                            if (status && status->targetRect.isValid() &&
                                status->targetRect.width() > 0 &&
                                status->targetRect.height() > 0) {
                                const qreal scaleX =
                                    compositeRect.width() /
                                    status->targetRect.width();
                                const qreal scaleY =
                                    compositeRect.height() /
                                    status->targetRect.height();
                                previewDrawRect = QRectF(
                                    compositeRect.x() +
                                        (effectDraw.outputRect.x() -
                                         status->targetRect.x()) *
                                            scaleX,
                                    compositeRect.y() +
                                        (effectDraw.outputRect.y() -
                                         status->targetRect.y()) *
                                            scaleY,
                                    effectDraw.outputRect.width() * scaleX,
                                    effectDraw.outputRect.height() * scaleY);
                            }
                            render_detail::vulkanMvpForOutputRectMaybeFlippedY(
                                previewDrawRect,
                                swapSize,
                                effectDraw.rotationDegrees,
                                status && status->sampledFrameNeedsYFlip,
                                effectPush.mvp);
                            uint32_t effectUniformOffset =
                                sampledResources ? sampledResources->frameUniformDynamicOffset() : 0;
                            if (sampledResources && sampledResources->updateFrameUniform(
                                    swapSize, nullptr, nullptr, nullptr, effectDraw.effectParams)) {
                                effectUniformOffset = sampledResources->frameUniformDynamicOffset();
                            }
                            m_pipeline->bindAndDraw(cb,
                                                     viewport,
                                                     generatedScissor,
                                                     handoffResult.descriptorSet,
                                                     effectPush,
                                                     effectUniformOffset);
                        }
                    } else {
                        drawPush(
                            basePush,
                            basePush.shadows[3] == render_detail::kVulkanEffectModeMaskGrade
                                ? maskEdgeParams
                                : nullptr);
                    }
                    if (status && !status->temporalEchoFrames.isEmpty()) {
                        for (int echoIndex = 0; echoIndex < status->temporalEchoFrames.size(); ++echoIndex) {
                            const QString echoKey = status->clipId + QStringLiteral("#temporalEcho%1").arg(echoIndex);
                            const auto echoResult = frameHandoffResults.value(echoKey);
                            if (!echoResult.sampledFrameReady || echoResult.descriptorSet == VK_NULL_HANDLE) continue;
                            VulkanPipeline::Push echoPush = push;
                            echoPush.opacity *= static_cast<float>(std::pow(
                                qBound<qreal>(0.0, status->temporalEchoDecay, 1.0), echoIndex + 1));
                            auto echoResourcesIt = m_clipHandoffResources.constFind(echoKey);
                            uint32_t echoUniformOffset = 0;
                            if (echoResourcesIt != m_clipHandoffResources.cend() && echoResourcesIt.value() &&
                                echoResourcesIt.value()->resources &&
                                echoResourcesIt.value()->resources->updateFrameUniform(compositeRect.size().toSize())) {
                                echoUniformOffset = echoResourcesIt.value()->resources->frameUniformDynamicOffset();
                            }
                            m_pipeline->bindAndDraw(cb, viewport, scissor, echoResult.descriptorSet,
                                                    echoPush, echoUniformOffset);
                        }
                    }
                    const bool frameCrossfadeMaskReady =
                        !status || !status->maskTextureEnabled ||
                        (status->frameCrossfadeMaskTextureEnabled &&
                         frameCrossfadeMaskUploadResults.value(
                             status->clipId, false));
                    const bool frameCrossfadeCurveReady =
                        !status || status->maskClipSource ||
                        !status->gradePayload.curveLutApplied ||
                        frameCrossfadeCurveLutUploadResults.value(
                            status->clipId, false);
                    const bool frameCrossfadeMaskCurveReady =
                        !status ||
                        !status->maskGradePayload.curveLutApplied ||
                        frameCrossfadeMaskCurveLutUploadResults.value(
                            status->clipId, false);
                    if (status && status->frameCrossfadeActive &&
                        secondaryHandoffResult.sampledFrameReady &&
                        secondaryHandoffResult.descriptorSet != VK_NULL_HANDLE &&
                        frameCrossfadeMaskReady &&
                        frameCrossfadeCurveReady &&
                        frameCrossfadeMaskCurveReady) {
                        VulkanPipeline::Push crossfadePush = basePush;
                        crossfadePush.opacity *= qBound(
                            0.0f, status->frameCrossfadeOpacity, 1.0f);
                        uint32_t secondaryFrameUniformOffset = 0;
                        if (status) {
                            const QString secondaryKey = status->clipId + QStringLiteral("#frameCrossfade");
                            auto secondaryResourcesIt = m_clipHandoffResources.constFind(secondaryKey);
                            if (secondaryResourcesIt != m_clipHandoffResources.cend() &&
                                secondaryResourcesIt.value() &&
                                secondaryResourcesIt.value()->resources &&
                                secondaryResourcesIt.value()->resources->updateFrameUniform(compositeRect.size().toSize())) {
                                secondaryFrameUniformOffset =
                                    secondaryResourcesIt.value()->resources->frameUniformDynamicOffset();
                            }
                        }
                        m_pipeline->bindAndDraw(cb,
                                                 viewport,
                                                 scissor,
                                                 secondaryHandoffResult.descriptorSet,
                                                 crossfadePush,
                                                 secondaryFrameUniformOffset);
                        submittedCrossfadeClipIds.insert(status->clipId);
                    }
                    if (maskReady && status->maskGradeEnabled &&
                        !status->maskForegroundLayerEnabled &&
                        !status->maskClipSource) {
                        VulkanPipeline::Push maskPush = push;
                        maskPush.brightness =
                            status->maskGradePayload.effects.brightness;
                        maskPush.contrast =
                            status->maskGradePayload.effects.contrast;
                        maskPush.saturation =
                            status->maskGradePayload.effects.saturation;
                        maskPush.opacity = static_cast<float>(std::clamp(status->maskOpacity, 0.0, 1.0));
                        maskPush.shadows[0] = 0.0f;
                        maskPush.shadows[1] = 0.0f;
                        maskPush.shadows[2] = 0.0f;
                        maskPush.shadows[3] = render_detail::kVulkanEffectModeMaskGrade;
                        maskPush.midtones[0] = 0.0f;
                        maskPush.midtones[1] = 0.0f;
                        maskPush.midtones[2] = 0.0f;
                        maskPush.midtones[3] = 0.0f;
                        maskPush.highlights[0] = 0.0f;
                        maskPush.highlights[1] = 0.0f;
                        maskPush.highlights[2] = 0.0f;
                        maskPush.highlights[3] = 1.0f;
                        if (maskCurveLutUploadResults.value(status->clipId, false)) {
                            maskPush.midtones[3] = render_detail::kVulkanMaskGradeUseSelectedCurveLut;
                        } else if (
                            status->maskGradePayload.curveLutApplied) {
                            if (DirectVulkanPreviewStats* stats = m_owner->stats()) {
                                stats->lastUnsupportedEffect = QStringLiteral("mask_curve_lut_upload_failed");
                            }
                        }
                        drawPush(maskPush, maskEdgeParams);
                    }
                    if (maskReady && status->maskForegroundLayerEnabled) {
                        VulkanPipeline::Push foregroundPush = push;
                        const bool applyMaskGradeToForeground = status->maskGradeEnabled;
                        foregroundPush.brightness = applyMaskGradeToForeground
                            ? status->maskGradePayload.effects.brightness
                            : 0.0f;
                        foregroundPush.contrast = applyMaskGradeToForeground
                            ? status->maskGradePayload.effects.contrast
                            : 1.0f;
                        foregroundPush.saturation = applyMaskGradeToForeground
                            ? status->maskGradePayload.effects.saturation
                            : 1.0f;
                        foregroundPush.opacity = static_cast<float>(
                            std::clamp(status->maskOpacity, 0.0, 1.0));
                        foregroundPush.shadows[0] = 0.0f;
                        foregroundPush.shadows[1] = 0.0f;
                        foregroundPush.shadows[2] = 0.0f;
                        foregroundPush.shadows[3] = render_detail::kVulkanEffectModeMaskGrade;
                        foregroundPush.midtones[0] = 0.0f;
                        foregroundPush.midtones[1] = 0.0f;
                        foregroundPush.midtones[2] = 0.0f;
                        foregroundPush.midtones[3] = 0.0f;
                        if (applyMaskGradeToForeground) {
                            if (maskCurveLutUploadResults.value(status->clipId, false)) {
                                foregroundPush.midtones[3] = render_detail::kVulkanMaskGradeUseSelectedCurveLut;
                            } else if (
                                status->maskGradePayload.curveLutApplied) {
                                if (DirectVulkanPreviewStats* stats = m_owner->stats()) {
                                    stats->lastUnsupportedEffect = QStringLiteral("mask_curve_lut_upload_failed");
                                }
                            }
                        }
                        foregroundPush.highlights[0] = 0.0f;
                        foregroundPush.highlights[1] = 0.0f;
                        foregroundPush.highlights[2] = 0.0f;
                        foregroundPush.highlights[3] = push.highlights[3];
                        drawMaskShadow(foregroundPush);
                        const uint32_t foregroundUniformOffset =
                            uniformOffsetForDraw(maskEdgeParams);
                        pendingMaskForegroundDraws.push_back(
                            PendingMaskForegroundDraw{
                                handoffResult.descriptorSet,
                                foregroundPush,
                                scissor,
                                foregroundUniformOffset});
                    }
                }
                if (bidirectionalEdgeDrawPending &&
                    bidirectionalEdgeDraw.descriptorSet != VK_NULL_HANDLE) {
                    m_pipeline->bindAndDraw(
                        cb,
                        viewport,
                        bidirectionalEdgeDraw.scissor,
                        bidirectionalEdgeDraw.descriptorSet,
                        bidirectionalEdgeDraw.push,
                        bidirectionalEdgeDraw.frameUniformDynamicOffset);
                }
                submittedClipIds.insert(status->clipId);
            } else {
                if (DirectVulkanPreviewStats* stats = m_owner->stats()) {
                    ++stats->explicitFailureDraws;
                    ++stats->clearFallbackDraws;
                    if (handoffAttempted && !sampledFrameReady) {
                        stats->lastHandoffMode = QStringLiteral("attempted_not_sampled");
                        stats->lastClearFallbackReason = QStringLiteral("handoff_attempted_not_sampled");
                    } else if (sampledFrameReady && !statusHasDrawableFrame) {
                        stats->lastHandoffMode = QStringLiteral("stale_sampled_resource_rejected");
                        stats->lastClearFallbackReason = QStringLiteral("stale_sampled_resource_rejected");
                        stats->lastHandoffError = status && !status->missingReason.isEmpty()
                            ? status->missingReason
                            : QStringLiteral("Retained Vulkan sampled image ignored because the active frame has no drawable payload.");
                    } else if (!status) {
                        stats->lastHandoffMode = QStringLiteral("decode_status_missing");
                        stats->lastClearFallbackReason = QStringLiteral("decode_status_missing");
                        stats->lastHandoffError = QStringLiteral("No Vulkan decode status exists for the active clip.");
                    } else if (!status->hasFrame) {
                        stats->lastHandoffMode = QStringLiteral("decoded_frame_unavailable");
                        stats->lastClearFallbackReason = QStringLiteral("decoded_frame_unavailable");
                        stats->lastHandoffError = status->missingReason.isEmpty()
                            ? QStringLiteral("Active Vulkan clip has no usable decoded frame.")
                            : status->missingReason;
                    } else if (status->frame.hasCpuImage() &&
                               !status->externalVulkanFrame &&
                               !status->frame.hasHardwareFrame()) {
                        stats->lastHandoffMode = QStringLiteral("vulkan_handoff_required");
                        stats->lastClearFallbackReason = QStringLiteral("vulkan_handoff_required");
                        stats->lastHandoffError = QStringLiteral(
                            "Direct Vulkan preview did not receive a drawable hardware or external Vulkan frame; CPU image upload is disabled.");
                    } else if (!canDrawTexture) {
                        stats->lastHandoffMode = QStringLiteral("texture_pipeline_unavailable");
                        stats->lastClearFallbackReason = QStringLiteral("texture_pipeline_unavailable");
                        stats->lastHandoffError = QStringLiteral("Vulkan texture pipeline or descriptor set is unavailable.");
                    }
                }
            }
            drawPreparedTranscriptOverlayForClip(clip.id, compositeRect);
            QRectF selectionBounds = transformedBounds;
            if (selected) {
                const QRectF transcriptBounds = transcriptOverlayBoundsForClip(state, clip, viewTransform);
                if (transcriptBounds.width() > 1.0 && transcriptBounds.height() > 1.0) {
                    selectionBounds = transcriptBounds;
                }
            }
            if (selected) {
                const int selectionThickness = std::max(2, std::min(swapSize.width(), swapSize.height()) / 360);
                clearBoxOutline(m_devFuncs,
                               cb,
                               selectionOutlineColor(),
                               clearRectFromQRect(selectionBounds, swapSize),
                               selectionThickness);
            }
            activeClipGeometry.insert(clip.id, effectiveClipGeometry);
            if (status && status->hasFrame && canDrawTexture && sampledFrameReady &&
                status->presentedSourceFrame >= presentedSourceFrame) {
                requestedSourceFrame = status->requestedSourceFrame;
                presentedSourceFrame = status->presentedSourceFrame;
            }
        }
        for (const PendingMaskForegroundDraw& draw : std::as_const(pendingMaskForegroundDraws)) {
            if (draw.descriptorSet != VK_NULL_HANDLE) {
                m_pipeline->bindAndDraw(cb,
                                        viewport,
                                        draw.scissor,
                                        draw.descriptorSet,
                                        draw.push,
                                        draw.frameUniformDynamicOffset);
            }
        }
        qint64 fallbackTranscriptDrawCount = 0;
        qint64 fallbackTitleDrawCount = 0;
        for (auto it = prepared3DTitleOverlays.cbegin(); it != prepared3DTitleOverlays.cend(); ++it) {
            if (!drawnTitleOverlayClipIds.contains(it.key()) &&
                m_textRenderer && m_textRenderer->isReady() &&
                m_textRenderer->drawTitleOverlay3D(cb, swapSize, state->outputSize,
                                                   compositeRect, it.value())) {
                drawnTitleOverlayClipIds.insert(it.key());
                ++fallbackTitleDrawCount;
            }
        }
        for (auto it = preparedTranscriptOverlays.cbegin(); it != preparedTranscriptOverlays.cend(); ++it) {
            if (!drawnTranscriptOverlayClipIds.contains(it.key())) {
                if (drawPreparedTranscriptOverlayForClip(it.key(), compositeRect)) {
                    ++fallbackTranscriptDrawCount;
                }
            }
        }
        if (m_owner->stats()) {
            const qint64 transcriptDrawAttempts = preparedTranscriptAtlasClipIds.size();
            const qint64 transcriptDrawSuccesses = drawnTranscriptOverlayClipIds.size();
            const qint64 titleDrawAttempts = prepared3DTitleOverlays.size();
            const qint64 titleDrawSuccesses = drawnTitleOverlayClipIds.size();
            m_owner->stats()->transcriptDrawnCount = static_cast<int>(transcriptDrawSuccesses);
            m_owner->stats()->titleDrawnCount = static_cast<int>(titleDrawSuccesses);
            editor::accumulatePlaybackStageMetric(&m_owner->stats()->textDrawStageMetric,
                                          transcriptDrawAttempts + titleDrawAttempts,
                                          transcriptDrawSuccesses + titleDrawSuccesses,
                                          qMax<qint64>(0, transcriptDrawAttempts + titleDrawAttempts -
                                                             transcriptDrawSuccesses - titleDrawSuccesses),
                                          (transcriptDrawAttempts + titleDrawAttempts) > 0
                                              ? QStringLiteral("text_draw_evaluated")
                                              : QStringLiteral("text_draw_not_requested"),
                                          QStringLiteral("transcript_prepared=%1 transcript_drawn=%2 fallback_drawn=%3 title_prepared=%4 title_drawn=%5 title_fallback_drawn=%6")
                                              .arg(transcriptDrawAttempts)
                                              .arg(transcriptDrawSuccesses)
                                              .arg(fallbackTranscriptDrawCount)
                                              .arg(titleDrawAttempts)
                                              .arg(titleDrawSuccesses)
                                              .arg(fallbackTitleDrawCount));
        }
        if (preparedSpeakerLabel && m_speakerTextRenderer && m_speakerTextRenderer->isReady()) {
            if (!m_speakerTextRenderer->drawSpeakerLabel(cb,
                                                         swapSize,
                                                         state->outputSize,
                                                         compositeRect,
                                                         preparedSpeakerSpec)) {
                if (DirectVulkanPreviewStats* stats = m_owner ? m_owner->stats() : nullptr) {
                    stats->lastTextDrawFailureReason = QStringLiteral("speaker:%1")
                        .arg(m_speakerTextRenderer->lastFailureReason());
                }
            }
        }
        if (preparedTemporalDebugLabel &&
            m_temporalDebugTextRenderer &&
            m_temporalDebugTextRenderer->isReady()) {
            m_temporalDebugTextRenderer->drawSpeakerLabel(cb,
                                                         swapSize,
                                                         state->outputSize,
                                                         compositeRect,
                                                         preparedTemporalDebugSpec);
        }
        drawPreparedOverlay(preparedPlaybackStatusOverlay);
        drawOutputPlacementGuides(m_devFuncs,
                                  cb,
                                  swapSize,
                                  state->outputSize,
                                  compositeRect,
                                  state->instagramSafeAreaGuides,
                                  state->alignmentGridGuides);
        const int thickness = std::max(2, std::min(swapSize.width(), swapSize.height()) / 180);
        for (const VulkanPreviewFacestreamOverlay& overlay : state->facedetectionsOverlays) {
            const auto it = activeClipGeometry.constFind(overlay.clipId);
            if (it == activeClipGeometry.constEnd() || !overlay.boxNorm.isValid()) {
                continue;
            }
            const PreviewClipGeometry& geometry = it.value();
            QRectF boxNorm = overlay.boxNorm;
            const bool hovered =
                overlay.trackId >= 0 &&
                state->transient.hoveredFaceDetectionsTrackId == overlay.trackId &&
                state->transient.hoveredFaceDetectionsClipId == overlay.clipId &&
                state->transient.hoveredFaceDetectionsId == overlay.streamId;
            if (hovered) {
                boxNorm = boxNorm.adjusted(-0.01, -0.01, 0.01, 0.01).intersected(QRectF(0.0, 0.0, 1.0, 1.0));
            }
            const VkClearRect boxRect = faceDetectionBoxToSwapchainRect(
                boxNorm,
                geometry.clipToScreen,
                geometry.localRect,
                swapSize);
            clearBoxOutline(
                m_devFuncs,
                cb,
                facedetectionsOverlayColor(state, overlay),
                boxRect,
                hovered ? qMax(thickness + 3, thickness * 2) : thickness);
        }
        for (const VulkanPreviewFacestreamOverlay& overlay : state->rawDetectionOverlays) {
            const auto it = activeClipGeometry.constFind(overlay.clipId);
            if (it == activeClipGeometry.constEnd() || !overlay.boxNorm.isValid()) {
                continue;
            }
            const PreviewClipGeometry& geometry = it.value();
            const VkClearRect boxRect = faceDetectionBoxToSwapchainRect(
                overlay.boxNorm,
                geometry.clipToScreen,
                geometry.localRect,
                swapSize);
            clearBoxOutline(m_devFuncs, cb, facedetectionsOverlayColor(state, overlay), boxRect, qMax(1, thickness - 1));
        }
        if (const TimelineClip* selectedClip = selectedClipForTargetBox(state)) {
            const TimelineClip::TransformKeyframe targetState =
                evaluateClipSpeakerFramingTargetAtFrame(*selectedClip, state->currentFrame);
            const qreal targetBoxNorm = qBound<qreal>(-1.0, targetState.scaleX, 1.0);
            if (targetBoxNorm > 0.0) {
                const int targetThickness = std::max(2, std::min(swapSize.width(), swapSize.height()) / 220);
                clearBoxOutline(
                    m_devFuncs,
                    cb,
                    targetBoxOverlayColor(),
                    targetBoxRectForComposite(*selectedClip, state->currentFrame, compositeRect, swapSize),
                    targetThickness);
            }
        }
    }
    m_devFuncs->vkCmdEndRenderPass(cb);
    if (m_owner->pipelineThumbnailReadbackPending()) {
        const int imageIndex = m_window->currentSwapChainImageIndex();
        if (imageIndex >= 0) {
            if (static_cast<int>(m_readbackSlots.size()) <= imageIndex) {
                m_readbackSlots.resize(static_cast<size_t>(imageIndex + 1));
            }
            ReadbackSlot& slot =
                m_readbackSlots[static_cast<size_t>(imageIndex)];
            if (ensureReadbackSlot(
                    &slot, swapSize, m_window->colorFormat())) {
                recordSwapchainReadback(cb, &slot, swapSize);
                m_owner->markPipelineThumbnailReadbackRecorded(swapSize);
            }
        }
    }
    if (m_owner->stats()) {
        editor::accumulatePlaybackStageMetric(&m_owner->stats()->gpuHandoffStageMetric,
                                      qMax<qint64>(1, handoffAttemptCount),
                                      handoffSuccessCount,
                                      qMax<qint64>(0, handoffAttemptCount - handoffSuccessCount),
                                      handoffAttemptCount > 0
                                          ? QStringLiteral("handoff_evaluated")
                                          : QStringLiteral("source_unavailable"),
                                      handoffAttemptCount > 0
                                          ? QStringLiteral("ready=%1").arg(handoffSuccessCount)
                                          : QStringLiteral("no_active_handoff_attempts"));
        editor::accumulatePlaybackStageMetric(&m_owner->stats()->commandRecordingStageMetric,
                                      0,
                                      1,
                                      0,
                                      QStringLiteral("recorded"),
                                      QStringLiteral("video_frame"));
    }

    m_owner->markPresentedSourceFrames(requestedSourceFrame, presentedSourceFrame);
    m_owner->markPresented(
        state, &submittedClipIds, &submittedCrossfadeClipIds);
    m_window->frameReady();
    m_owner->markPreviewUpdateDelivered();
}

void DirectVulkanPreviewRenderer::beginGpuExportPreviewRenderPass(
    VkCommandBuffer commandBuffer,
    const VkRenderPassBeginInfo& renderPass)
{
    m_devFuncs->vkCmdBeginRenderPass(
        commandBuffer, &renderPass, VK_SUBPASS_CONTENTS_INLINE);
}
