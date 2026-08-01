#include "vulkan_mask_preprocessor.h"

#include "vulkan_shader_paths.h"

#include <QCryptographicHash>
#include <QFile>

#include <algorithm>

namespace {

struct MaskPreparePush {
    int outputSize[2];
    int inputSize[2];
    int invert = 0;
    int applyCorrections = 0;
};

struct MaskMorphBlurPush {
    int outputSize[2];
    int radius = 0;
    int mode = 0;
};

QByteArray canonicalCorrectionStorage(const QByteArray& storage)
{
    if (!storage.isEmpty()) {
        return storage;
    }
    return QByteArray(static_cast<qsizetype>(sizeof(float) * 4), '\0');
}

} // namespace

QString vulkanMaskTextureCacheKey(
    const VulkanMaskPreprocessOptions& options,
    const QSize& outputSize)
{
    if (options.sourceIdentity.isEmpty() || !outputSize.isValid()) {
        return {};
    }
    QByteArray treatment;
    treatment.reserve(options.correctionStorage.size() + 96);
    treatment.append(options.invert ? "invert=1" : "invert=0");
    treatment.append("|erode=");
    treatment.append(QByteArray::number(options.erodeRadius));
    treatment.append("|dilate=");
    treatment.append(QByteArray::number(options.dilateRadius));
    treatment.append("|blur=");
    treatment.append(QByteArray::number(options.blurRadius));
    treatment.append("|corrections=");
    treatment.append(options.correctionStorage);
    const QByteArray treatmentHash =
        QCryptographicHash::hash(
            treatment, QCryptographicHash::Sha256).toHex();
    return QStringLiteral("%1|output=%2x%3|treatment=%4")
        .arg(options.sourceIdentity)
        .arg(outputSize.width())
        .arg(outputSize.height())
        .arg(QString::fromLatin1(treatmentHash));
}

VulkanMaskPreprocessor::~VulkanMaskPreprocessor()
{
    destroy();
}

bool VulkanMaskPreprocessor::initialize(VkPhysicalDevice physicalDevice,
                                        VkDevice device,
                                        QString* errorMessage)
{
    destroy();
    if (physicalDevice == VK_NULL_HANDLE || device == VK_NULL_HANDLE) {
        if (errorMessage) {
            *errorMessage = QStringLiteral(
                "Mask preprocessor requires a valid Vulkan device.");
        }
        return false;
    }
    m_physicalDevice = physicalDevice;
    m_device = device;
    VkPhysicalDeviceProperties properties{};
    vkGetPhysicalDeviceProperties(m_physicalDevice, &properties);
    m_correctionStorageAlignment = std::max<VkDeviceSize>(
        16, properties.limits.minStorageBufferOffsetAlignment);

    VkDescriptorSetLayoutBinding bindings[3]{};
    bindings[0].binding = 0;
    bindings[0].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    bindings[0].descriptorCount = 1;
    bindings[0].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
    bindings[1].binding = 1;
    bindings[1].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
    bindings[1].descriptorCount = 1;
    bindings[1].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
    bindings[2].binding = 2;
    bindings[2].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    bindings[2].descriptorCount = 1;
    bindings[2].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
    VkDescriptorSetLayoutCreateInfo layoutInfo{};
    layoutInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    layoutInfo.bindingCount = 3;
    layoutInfo.pBindings = bindings;
    if (vkCreateDescriptorSetLayout(
            m_device, &layoutInfo, nullptr, &m_descriptorSetLayout) !=
        VK_SUCCESS) {
        if (errorMessage) {
            *errorMessage = QStringLiteral(
                "Failed to create Vulkan mask compute descriptor layout.");
        }
        destroy();
        return false;
    }

    VkDescriptorPoolSize poolSizes[3]{};
    poolSizes[0] = {VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
                    static_cast<std::uint32_t>(kDescriptorSetCount)};
    poolSizes[1] = {VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,
                    static_cast<std::uint32_t>(kDescriptorSetCount)};
    poolSizes[2] = {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
                    static_cast<std::uint32_t>(kDescriptorSetCount)};
    VkDescriptorPoolCreateInfo poolInfo{};
    poolInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    poolInfo.poolSizeCount = 3;
    poolInfo.pPoolSizes = poolSizes;
    poolInfo.maxSets = static_cast<std::uint32_t>(kDescriptorSetCount);
    if (vkCreateDescriptorPool(
            m_device, &poolInfo, nullptr, &m_descriptorPool) != VK_SUCCESS) {
        if (errorMessage) {
            *errorMessage = QStringLiteral(
                "Failed to create Vulkan mask compute descriptor pool.");
        }
        destroy();
        return false;
    }

    m_descriptorSets.assign(kDescriptorSetCount, VK_NULL_HANDLE);
    std::vector<VkDescriptorSetLayout> layouts(
        kDescriptorSetCount, m_descriptorSetLayout);
    VkDescriptorSetAllocateInfo allocation{};
    allocation.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
    allocation.descriptorPool = m_descriptorPool;
    allocation.descriptorSetCount =
        static_cast<std::uint32_t>(m_descriptorSets.size());
    allocation.pSetLayouts = layouts.data();
    if (vkAllocateDescriptorSets(
            m_device, &allocation, m_descriptorSets.data()) != VK_SUCCESS) {
        if (errorMessage) {
            *errorMessage = QStringLiteral(
                "Failed to allocate Vulkan mask compute descriptor sets.");
        }
        destroy();
        return false;
    }

    VkPushConstantRange preparePush{};
    preparePush.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
    preparePush.size = sizeof(MaskPreparePush);
    VkPipelineLayoutCreateInfo prepareLayout{};
    prepareLayout.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    prepareLayout.setLayoutCount = 1;
    prepareLayout.pSetLayouts = &m_descriptorSetLayout;
    prepareLayout.pushConstantRangeCount = 1;
    prepareLayout.pPushConstantRanges = &preparePush;
    if (vkCreatePipelineLayout(
            m_device,
            &prepareLayout,
            nullptr,
            &m_preparePipelineLayout) != VK_SUCCESS) {
        if (errorMessage) {
            *errorMessage = QStringLiteral(
                "Failed to create Vulkan mask prepare pipeline layout.");
        }
        destroy();
        return false;
    }

    VkPushConstantRange morphPush{};
    morphPush.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
    morphPush.size = sizeof(MaskMorphBlurPush);
    VkPipelineLayoutCreateInfo morphLayout = prepareLayout;
    morphLayout.pPushConstantRanges = &morphPush;
    if (vkCreatePipelineLayout(
            m_device,
            &morphLayout,
            nullptr,
            &m_morphBlurPipelineLayout) != VK_SUCCESS) {
        if (errorMessage) {
            *errorMessage = QStringLiteral(
                "Failed to create Vulkan mask morph/blur pipeline layout.");
        }
        destroy();
        return false;
    }

    const QString shaderDir = jcutVulkanShaderDirectory();
    m_prepareModule = createShaderModule(
        shaderDir + QStringLiteral("/mask_prepare.comp.spv"));
    m_morphModule = createShaderModule(
        shaderDir + QStringLiteral("/mask_morph.comp.spv"));
    m_blurModule = createShaderModule(
        shaderDir + QStringLiteral("/mask_blur.comp.spv"));
    if (m_prepareModule == VK_NULL_HANDLE ||
        m_morphModule == VK_NULL_HANDLE ||
        m_blurModule == VK_NULL_HANDLE) {
        if (errorMessage) {
            *errorMessage = QStringLiteral(
                "Failed to load Vulkan mask compute shaders.");
        }
        destroy();
        return false;
    }

    const auto createPipeline = [this](
        VkShaderModule module,
        VkPipelineLayout layout,
        VkPipeline* pipeline) {
        VkComputePipelineCreateInfo info{};
        info.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO;
        info.stage.sType =
            VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        info.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
        info.stage.module = module;
        info.stage.pName = "main";
        info.layout = layout;
        return vkCreateComputePipelines(
                   m_device,
                   VK_NULL_HANDLE,
                   1,
                   &info,
                   nullptr,
                   pipeline) == VK_SUCCESS;
    };
    if (!createPipeline(
            m_prepareModule, m_preparePipelineLayout, &m_preparePipeline) ||
        !createPipeline(
            m_morphModule, m_morphBlurPipelineLayout, &m_morphPipeline) ||
        !createPipeline(
            m_blurModule, m_morphBlurPipelineLayout, &m_blurPipeline)) {
        if (errorMessage) {
            *errorMessage = QStringLiteral(
                "Failed to create Vulkan mask compute pipelines.");
        }
        destroy();
        return false;
    }
    return true;
}

void VulkanMaskPreprocessor::destroy()
{
    if (m_device != VK_NULL_HANDLE) {
        if (m_blurPipeline != VK_NULL_HANDLE) {
            vkDestroyPipeline(m_device, m_blurPipeline, nullptr);
        }
        if (m_morphPipeline != VK_NULL_HANDLE) {
            vkDestroyPipeline(m_device, m_morphPipeline, nullptr);
        }
        if (m_preparePipeline != VK_NULL_HANDLE) {
            vkDestroyPipeline(m_device, m_preparePipeline, nullptr);
        }
        if (m_blurModule != VK_NULL_HANDLE) {
            vkDestroyShaderModule(m_device, m_blurModule, nullptr);
        }
        if (m_morphModule != VK_NULL_HANDLE) {
            vkDestroyShaderModule(m_device, m_morphModule, nullptr);
        }
        if (m_prepareModule != VK_NULL_HANDLE) {
            vkDestroyShaderModule(m_device, m_prepareModule, nullptr);
        }
        if (m_morphBlurPipelineLayout != VK_NULL_HANDLE) {
            vkDestroyPipelineLayout(
                m_device, m_morphBlurPipelineLayout, nullptr);
        }
        if (m_preparePipelineLayout != VK_NULL_HANDLE) {
            vkDestroyPipelineLayout(
                m_device, m_preparePipelineLayout, nullptr);
        }
        if (m_descriptorPool != VK_NULL_HANDLE) {
            vkDestroyDescriptorPool(m_device, m_descriptorPool, nullptr);
        }
        if (m_descriptorSetLayout != VK_NULL_HANDLE) {
            vkDestroyDescriptorSetLayout(
                m_device, m_descriptorSetLayout, nullptr);
        }
    }
    m_blurPipeline = VK_NULL_HANDLE;
    m_morphPipeline = VK_NULL_HANDLE;
    m_preparePipeline = VK_NULL_HANDLE;
    m_blurModule = VK_NULL_HANDLE;
    m_morphModule = VK_NULL_HANDLE;
    m_prepareModule = VK_NULL_HANDLE;
    m_morphBlurPipelineLayout = VK_NULL_HANDLE;
    m_preparePipelineLayout = VK_NULL_HANDLE;
    m_descriptorSets.clear();
    m_descriptorSetIndex = 0;
    m_descriptorPool = VK_NULL_HANDLE;
    m_descriptorSetLayout = VK_NULL_HANDLE;
    m_correctionStorageAlignment = 16;
    m_physicalDevice = VK_NULL_HANDLE;
    m_device = VK_NULL_HANDLE;
}

bool VulkanMaskPreprocessor::record(
    VkCommandBuffer commandBuffer,
    const Images& images,
    const VulkanMaskPreprocessOptions& options,
    const StageCorrectionStorage& stageCorrectionStorage)
{
    if (!isReady() || commandBuffer == VK_NULL_HANDLE ||
        images.sampler == VK_NULL_HANDLE || !images.inputSize.isValid() ||
        !images.outputSize.isValid() || images.inputView == VK_NULL_HANDLE ||
        images.outputImage == VK_NULL_HANDLE ||
        images.outputView == VK_NULL_HANDLE || !images.outputLayout ||
        images.workImage == VK_NULL_HANDLE ||
        images.workView == VK_NULL_HANDLE || !images.workLayout ||
        !stageCorrectionStorage) {
        return false;
    }
    const QByteArray correctionStorage =
        canonicalCorrectionStorage(options.correctionStorage);
    if ((correctionStorage.size() %
         static_cast<qsizetype>(sizeof(float) * 4)) != 0) {
        return false;
    }
    StagedCorrectionStorage staged;
    if (!stageCorrectionStorage(
            correctionStorage, m_correctionStorageAlignment, &staged) ||
        staged.buffer == VK_NULL_HANDLE ||
        staged.bytes < static_cast<VkDeviceSize>(sizeof(float) * 4)) {
        return false;
    }

    MaskPreparePush prepare{};
    prepare.outputSize[0] = images.outputSize.width();
    prepare.outputSize[1] = images.outputSize.height();
    prepare.inputSize[0] = images.inputSize.width();
    prepare.inputSize[1] = images.inputSize.height();
    prepare.invert = options.invert ? 1 : 0;
    prepare.applyCorrections = 1;
    if (!recordPass(
            commandBuffer,
            PipelineKind::Prepare,
            &prepare,
            sizeof(prepare),
            images,
            images.inputView,
            images.outputImage,
            images.outputView,
            images.outputLayout,
            staged)) {
        return false;
    }

    VkImageView currentView = images.outputView;
    bool currentIsOutput = true;
    const auto dispatchMorphBlur = [&](
        PipelineKind kind, int radius, int mode) {
        VkImage destinationImage =
            currentIsOutput ? images.workImage : images.outputImage;
        VkImageView destinationView =
            currentIsOutput ? images.workView : images.outputView;
        VkImageLayout* destinationLayout =
            currentIsOutput ? images.workLayout : images.outputLayout;
        MaskMorphBlurPush push{};
        push.outputSize[0] = images.outputSize.width();
        push.outputSize[1] = images.outputSize.height();
        push.radius = radius;
        push.mode = mode;
        if (!recordPass(
                commandBuffer,
                kind,
                &push,
                sizeof(push),
                images,
                currentView,
                destinationImage,
                destinationView,
                destinationLayout,
                staged)) {
            return false;
        }
        currentView = destinationView;
        currentIsOutput = !currentIsOutput;
        return true;
    };

    const int erodeRadius = std::max(0, options.erodeRadius);
    const int dilateRadius = std::max(0, options.dilateRadius);
    const int blurRadius = std::max(0, options.blurRadius);
    if (erodeRadius > 0 &&
        !dispatchMorphBlur(PipelineKind::Morph, erodeRadius, 0)) {
        return false;
    }
    if (dilateRadius > 0 &&
        !dispatchMorphBlur(PipelineKind::Morph, dilateRadius, 1)) {
        return false;
    }
    if (blurRadius > 0 &&
        (!dispatchMorphBlur(PipelineKind::Blur, blurRadius, 1) ||
         !dispatchMorphBlur(PipelineKind::Blur, blurRadius, 0))) {
        return false;
    }
    if (!currentIsOutput) {
        MaskPreparePush copy{};
        copy.outputSize[0] = images.outputSize.width();
        copy.outputSize[1] = images.outputSize.height();
        copy.inputSize[0] = images.outputSize.width();
        copy.inputSize[1] = images.outputSize.height();
        if (!recordPass(
                commandBuffer,
                PipelineKind::Prepare,
                &copy,
                sizeof(copy),
                images,
                currentView,
                images.outputImage,
                images.outputView,
                images.outputLayout,
                staged)) {
            return false;
        }
    }
    return true;
}

bool VulkanMaskPreprocessor::isReady() const
{
    return m_device != VK_NULL_HANDLE &&
           m_preparePipeline != VK_NULL_HANDLE &&
           m_morphPipeline != VK_NULL_HANDLE &&
           m_blurPipeline != VK_NULL_HANDLE &&
           !m_descriptorSets.empty();
}

bool VulkanMaskPreprocessor::recordPass(
    VkCommandBuffer commandBuffer,
    PipelineKind kind,
    const void* pushData,
    std::uint32_t pushDataSize,
    const Images& images,
    VkImageView inputView,
    VkImage outputImage,
    VkImageView outputView,
    VkImageLayout* outputLayout,
    const StagedCorrectionStorage& correctionStorage)
{
    if (!outputLayout || m_descriptorSets.empty()) {
        return false;
    }
    VkPipeline pipeline = VK_NULL_HANDLE;
    VkPipelineLayout pipelineLayout = VK_NULL_HANDLE;
    switch (kind) {
    case PipelineKind::Prepare:
        pipeline = m_preparePipeline;
        pipelineLayout = m_preparePipelineLayout;
        break;
    case PipelineKind::Morph:
        pipeline = m_morphPipeline;
        pipelineLayout = m_morphBlurPipelineLayout;
        break;
    case PipelineKind::Blur:
        pipeline = m_blurPipeline;
        pipelineLayout = m_morphBlurPipelineLayout;
        break;
    }
    VkDescriptorSet descriptorSet =
        m_descriptorSets.at(m_descriptorSetIndex);
    m_descriptorSetIndex =
        (m_descriptorSetIndex + 1) % m_descriptorSets.size();

    transitionImage(
        commandBuffer, outputImage, *outputLayout, VK_IMAGE_LAYOUT_GENERAL);
    *outputLayout = VK_IMAGE_LAYOUT_GENERAL;

    VkDescriptorImageInfo inputInfo{};
    inputInfo.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    inputInfo.imageView = inputView;
    inputInfo.sampler = images.sampler;
    VkDescriptorImageInfo outputInfo{};
    outputInfo.imageLayout = VK_IMAGE_LAYOUT_GENERAL;
    outputInfo.imageView = outputView;
    VkDescriptorBufferInfo correctionInfo{};
    correctionInfo.buffer = correctionStorage.buffer;
    correctionInfo.offset = correctionStorage.offset;
    correctionInfo.range = correctionStorage.bytes;
    VkWriteDescriptorSet writes[3]{};
    writes[0].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    writes[0].dstSet = descriptorSet;
    writes[0].dstBinding = 0;
    writes[0].descriptorCount = 1;
    writes[0].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    writes[0].pImageInfo = &inputInfo;
    writes[1].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    writes[1].dstSet = descriptorSet;
    writes[1].dstBinding = 1;
    writes[1].descriptorCount = 1;
    writes[1].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
    writes[1].pImageInfo = &outputInfo;
    writes[2].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    writes[2].dstSet = descriptorSet;
    writes[2].dstBinding = 2;
    writes[2].descriptorCount = 1;
    writes[2].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    writes[2].pBufferInfo = &correctionInfo;
    vkUpdateDescriptorSets(m_device, 3, writes, 0, nullptr);

    vkCmdBindPipeline(
        commandBuffer, VK_PIPELINE_BIND_POINT_COMPUTE, pipeline);
    vkCmdBindDescriptorSets(
        commandBuffer,
        VK_PIPELINE_BIND_POINT_COMPUTE,
        pipelineLayout,
        0,
        1,
        &descriptorSet,
        0,
        nullptr);
    vkCmdPushConstants(
        commandBuffer,
        pipelineLayout,
        VK_SHADER_STAGE_COMPUTE_BIT,
        0,
        pushDataSize,
        pushData);
    vkCmdDispatch(
        commandBuffer,
        static_cast<std::uint32_t>((images.outputSize.width() + 15) / 16),
        static_cast<std::uint32_t>((images.outputSize.height() + 15) / 16),
        1);

    transitionImage(
        commandBuffer,
        outputImage,
        VK_IMAGE_LAYOUT_GENERAL,
        VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    *outputLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    return true;
}

VkShaderModule VulkanMaskPreprocessor::createShaderModule(
    const QString& path) const
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        return VK_NULL_HANDLE;
    }
    const QByteArray bytes = file.readAll();
    if (bytes.isEmpty() || (bytes.size() % 4) != 0) {
        return VK_NULL_HANDLE;
    }
    VkShaderModuleCreateInfo info{};
    info.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
    info.codeSize = static_cast<std::size_t>(bytes.size());
    info.pCode =
        reinterpret_cast<const std::uint32_t*>(bytes.constData());
    VkShaderModule module = VK_NULL_HANDLE;
    return vkCreateShaderModule(
               m_device, &info, nullptr, &module) == VK_SUCCESS
        ? module
        : VK_NULL_HANDLE;
}

void VulkanMaskPreprocessor::transitionImage(
    VkCommandBuffer commandBuffer,
    VkImage image,
    VkImageLayout oldLayout,
    VkImageLayout newLayout) const
{
    VkImageMemoryBarrier barrier{};
    barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    barrier.oldLayout = oldLayout;
    barrier.newLayout = newLayout;
    barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.image = image;
    barrier.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    barrier.subresourceRange.levelCount = 1;
    barrier.subresourceRange.layerCount = 1;

    VkPipelineStageFlags sourceStage = VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT;
    VkPipelineStageFlags destinationStage =
        VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT;
    if (oldLayout == VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL) {
        sourceStage = VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT |
                      VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
        barrier.srcAccessMask = VK_ACCESS_SHADER_READ_BIT;
    } else if (oldLayout == VK_IMAGE_LAYOUT_GENERAL) {
        sourceStage = VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT;
        barrier.srcAccessMask =
            VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT;
    } else if (oldLayout == VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL) {
        sourceStage = VK_PIPELINE_STAGE_TRANSFER_BIT;
        barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    }
    if (newLayout == VK_IMAGE_LAYOUT_GENERAL) {
        barrier.dstAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
    } else {
        destinationStage = VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT |
                           VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
        barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
    }
    vkCmdPipelineBarrier(
        commandBuffer,
        sourceStage,
        destinationStage,
        0,
        0,
        nullptr,
        0,
        nullptr,
        1,
        &barrier);
}
