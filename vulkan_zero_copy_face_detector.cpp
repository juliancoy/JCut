#include "vulkan_zero_copy_face_detector.h"

#include <QFile>
#include <QtGlobal>

#include <array>
#include <cstring>

#ifndef JCUT_VULKAN_SHADER_DIR
#define JCUT_VULKAN_SHADER_DIR ""
#endif

namespace jcut::vulkan_detector {
namespace {

struct FacePreprocessPushConstants {
    int dstWidth = 300;
    int dstHeight = 300;
    int sourceIsSrgb = 0;
    int reserved = 0;
};

struct FaceInferencePushConstants {
    int tensorWidth = 300;
    int tensorHeight = 300;
    int gridWidth = 18;
    int gridHeight = 18;
    float threshold = 0.42f;
    int scaleIndex = 0;
};

QByteArray readShader(const QString& name)
{
    QFile file(QStringLiteral(JCUT_VULKAN_SHADER_DIR) + QLatin1Char('/') + name);
    if (!file.open(QIODevice::ReadOnly)) {
        return {};
    }
    return file.readAll();
}

void setError(QString* out, const QString& value)
{
    if (out) {
        *out = value;
    }
}

bool validContext(const VulkanDeviceContext& context)
{
    return context.physicalDevice != VK_NULL_HANDLE &&
           context.device != VK_NULL_HANDLE &&
           context.queue != VK_NULL_HANDLE;
}

} // namespace

VkDeviceSize FaceDetectorTensorSpec::byteSize() const
{
    return static_cast<VkDeviceSize>(qMax(0, width)) *
           static_cast<VkDeviceSize>(qMax(0, height)) *
           static_cast<VkDeviceSize>(qMax(0, channels)) *
           static_cast<VkDeviceSize>(sizeof(float));
}

VulkanZeroCopyFaceDetector::VulkanZeroCopyFaceDetector() = default;

VulkanZeroCopyFaceDetector::~VulkanZeroCopyFaceDetector()
{
    release();
}

bool VulkanZeroCopyFaceDetector::initialize(const VulkanDeviceContext& context, QString* errorMessage)
{
    if (m_initialized) {
        return true;
    }
    if (!validContext(context)) {
        setError(errorMessage, QStringLiteral("invalid Vulkan device context for zero-copy face detector"));
        return false;
    }

    m_context = context;
    if (!createDescriptorResources(errorMessage) ||
        !createPipeline(errorMessage) ||
        !createInferenceDescriptorResources(errorMessage) ||
        !createInferencePipeline(errorMessage) ||
        !createCommandResources(errorMessage) ||
        !createSampler(errorMessage)) {
        release();
        return false;
    }

    m_initialized = true;
    return true;
}

void VulkanZeroCopyFaceDetector::release()
{
    const VkDevice device = m_context.device;
    if (device != VK_NULL_HANDLE) {
        if (m_fence != VK_NULL_HANDLE) {
            vkDestroyFence(device, m_fence, nullptr);
        }
        if (m_commandPool != VK_NULL_HANDLE) {
            vkDestroyCommandPool(device, m_commandPool, nullptr);
        }
        if (m_sampler != VK_NULL_HANDLE) {
            vkDestroySampler(device, m_sampler, nullptr);
        }
        if (m_pipeline != VK_NULL_HANDLE) {
            vkDestroyPipeline(device, m_pipeline, nullptr);
        }
        if (m_inferencePipeline != VK_NULL_HANDLE) {
            vkDestroyPipeline(device, m_inferencePipeline, nullptr);
        }
        if (m_pipelineLayout != VK_NULL_HANDLE) {
            vkDestroyPipelineLayout(device, m_pipelineLayout, nullptr);
        }
        if (m_inferencePipelineLayout != VK_NULL_HANDLE) {
            vkDestroyPipelineLayout(device, m_inferencePipelineLayout, nullptr);
        }
        if (m_descriptorPool != VK_NULL_HANDLE) {
            vkDestroyDescriptorPool(device, m_descriptorPool, nullptr);
        }
        if (m_inferenceDescriptorPool != VK_NULL_HANDLE) {
            vkDestroyDescriptorPool(device, m_inferenceDescriptorPool, nullptr);
        }
        if (m_descriptorSetLayout != VK_NULL_HANDLE) {
            vkDestroyDescriptorSetLayout(device, m_descriptorSetLayout, nullptr);
        }
        if (m_inferenceDescriptorSetLayout != VK_NULL_HANDLE) {
            vkDestroyDescriptorSetLayout(device, m_inferenceDescriptorSetLayout, nullptr);
        }
    }

    m_descriptorSetLayout = VK_NULL_HANDLE;
    m_descriptorPool = VK_NULL_HANDLE;
    m_pipelineLayout = VK_NULL_HANDLE;
    m_pipeline = VK_NULL_HANDLE;
    m_inferenceDescriptorSetLayout = VK_NULL_HANDLE;
    m_inferenceDescriptorPool = VK_NULL_HANDLE;
    m_inferencePipelineLayout = VK_NULL_HANDLE;
    m_inferencePipeline = VK_NULL_HANDLE;
    m_commandPool = VK_NULL_HANDLE;
    m_commandBuffer = VK_NULL_HANDLE;
    m_fence = VK_NULL_HANDLE;
    m_sampler = VK_NULL_HANDLE;
    m_context = {};
    m_initialized = false;
}

bool VulkanZeroCopyFaceDetector::isInitialized() const
{
    return m_initialized;
}

FaceDetectorTensorSpec VulkanZeroCopyFaceDetector::tensorSpec() const
{
    return m_tensorSpec;
}

QString VulkanZeroCopyFaceDetector::backendId() const
{
    return QStringLiteral("jcut_vulkan_zero_copy_face_detector_v1");
}

bool VulkanZeroCopyFaceDetector::preprocessToTensor(const VulkanExternalImage& source,
                                                    const VulkanTensorBuffer& outputTensor,
                                                    QString* errorMessage)
{
    if (!m_initialized) {
        setError(errorMessage, QStringLiteral("zero-copy Vulkan face detector is not initialized"));
        return false;
    }
    if (source.imageView == VK_NULL_HANDLE || !source.size.isValid()) {
        setError(errorMessage, QStringLiteral("invalid source image for zero-copy face preprocessing"));
        return false;
    }
    if (outputTensor.buffer == VK_NULL_HANDLE || outputTensor.byteSize < m_tensorSpec.byteSize()) {
        setError(errorMessage, QStringLiteral("output tensor buffer is missing or too small"));
        return false;
    }

    VkDescriptorSet descriptorSet = VK_NULL_HANDLE;
    VkDescriptorSetAllocateInfo allocInfo{};
    allocInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
    allocInfo.descriptorPool = m_descriptorPool;
    allocInfo.descriptorSetCount = 1;
    allocInfo.pSetLayouts = &m_descriptorSetLayout;
    if (vkAllocateDescriptorSets(m_context.device, &allocInfo, &descriptorSet) != VK_SUCCESS) {
        setError(errorMessage, QStringLiteral("failed to allocate zero-copy detector descriptor set"));
        return false;
    }

    VkDescriptorImageInfo imageInfo{};
    imageInfo.sampler = m_sampler;
    imageInfo.imageView = source.imageView;
    imageInfo.imageLayout = source.imageLayout;

    VkDescriptorBufferInfo bufferInfo{};
    bufferInfo.buffer = outputTensor.buffer;
    bufferInfo.offset = 0;
    bufferInfo.range = m_tensorSpec.byteSize();

    std::array<VkWriteDescriptorSet, 2> writes{};
    writes[0].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    writes[0].dstSet = descriptorSet;
    writes[0].dstBinding = 0;
    writes[0].descriptorCount = 1;
    writes[0].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    writes[0].pImageInfo = &imageInfo;
    writes[1].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    writes[1].dstSet = descriptorSet;
    writes[1].dstBinding = 1;
    writes[1].descriptorCount = 1;
    writes[1].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    writes[1].pBufferInfo = &bufferInfo;
    vkUpdateDescriptorSets(m_context.device, static_cast<uint32_t>(writes.size()), writes.data(), 0, nullptr);

    const bool ok = submitPreprocess(descriptorSet, source, errorMessage);
    vkFreeDescriptorSets(m_context.device, m_descriptorPool, 1, &descriptorSet);
    return ok;
}

bool VulkanZeroCopyFaceDetector::inferFromTensor(const VulkanTensorBuffer& inputTensor,
                                                 const VulkanTensorBuffer& outputDetections,
                                                 int maxDetections,
                                                 float threshold,
                                                 QString* errorMessage)
{
    if (!m_initialized) {
        setError(errorMessage, QStringLiteral("zero-copy Vulkan face detector is not initialized"));
        return false;
    }
    if (inputTensor.buffer == VK_NULL_HANDLE || inputTensor.byteSize < m_tensorSpec.byteSize()) {
        setError(errorMessage, QStringLiteral("input tensor buffer is missing or too small"));
        return false;
    }
    if (outputDetections.buffer == VK_NULL_HANDLE || maxDetections <= 0 ||
        outputDetections.byteSize < static_cast<VkDeviceSize>(16 + (maxDetections * 32))) {
        setError(errorMessage, QStringLiteral("output detection buffer is missing or too small"));
        return false;
    }

    VkDescriptorSet descriptorSet = VK_NULL_HANDLE;
    VkDescriptorSetAllocateInfo allocInfo{};
    allocInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
    allocInfo.descriptorPool = m_inferenceDescriptorPool;
    allocInfo.descriptorSetCount = 1;
    allocInfo.pSetLayouts = &m_inferenceDescriptorSetLayout;
    if (vkAllocateDescriptorSets(m_context.device, &allocInfo, &descriptorSet) != VK_SUCCESS) {
        setError(errorMessage, QStringLiteral("failed to allocate Vulkan face inference descriptor set"));
        return false;
    }

    VkDescriptorBufferInfo tensorInfo{};
    tensorInfo.buffer = inputTensor.buffer;
    tensorInfo.range = m_tensorSpec.byteSize();
    VkDescriptorBufferInfo detectionInfo{};
    detectionInfo.buffer = outputDetections.buffer;
    detectionInfo.range = outputDetections.byteSize;

    std::array<VkWriteDescriptorSet, 2> writes{};
    writes[0].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    writes[0].dstSet = descriptorSet;
    writes[0].dstBinding = 0;
    writes[0].descriptorCount = 1;
    writes[0].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    writes[0].pBufferInfo = &tensorInfo;
    writes[1].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    writes[1].dstSet = descriptorSet;
    writes[1].dstBinding = 1;
    writes[1].descriptorCount = 1;
    writes[1].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    writes[1].pBufferInfo = &detectionInfo;
    vkUpdateDescriptorSets(m_context.device, static_cast<uint32_t>(writes.size()), writes.data(), 0, nullptr);

    const bool ok = submitInference(descriptorSet, outputDetections, maxDetections, threshold, errorMessage);
    vkFreeDescriptorSets(m_context.device, m_inferenceDescriptorPool, 1, &descriptorSet);
    return ok;
}

bool VulkanZeroCopyFaceDetector::createDescriptorResources(QString* errorMessage)
{
    std::array<VkDescriptorSetLayoutBinding, 2> bindings{};
    bindings[0].binding = 0;
    bindings[0].descriptorCount = 1;
    bindings[0].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    bindings[0].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
    bindings[1].binding = 1;
    bindings[1].descriptorCount = 1;
    bindings[1].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    bindings[1].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;

    VkDescriptorSetLayoutCreateInfo layoutInfo{};
    layoutInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    layoutInfo.bindingCount = static_cast<uint32_t>(bindings.size());
    layoutInfo.pBindings = bindings.data();
    if (vkCreateDescriptorSetLayout(m_context.device, &layoutInfo, nullptr, &m_descriptorSetLayout) != VK_SUCCESS) {
        setError(errorMessage, QStringLiteral("failed to create zero-copy detector descriptor set layout"));
        return false;
    }

    std::array<VkDescriptorPoolSize, 2> poolSizes{};
    poolSizes[0].type = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    poolSizes[0].descriptorCount = 8;
    poolSizes[1].type = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    poolSizes[1].descriptorCount = 8;
    VkDescriptorPoolCreateInfo poolInfo{};
    poolInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    poolInfo.flags = VK_DESCRIPTOR_POOL_CREATE_FREE_DESCRIPTOR_SET_BIT;
    poolInfo.maxSets = 8;
    poolInfo.poolSizeCount = static_cast<uint32_t>(poolSizes.size());
    poolInfo.pPoolSizes = poolSizes.data();
    if (vkCreateDescriptorPool(m_context.device, &poolInfo, nullptr, &m_descriptorPool) != VK_SUCCESS) {
        setError(errorMessage, QStringLiteral("failed to create zero-copy detector descriptor pool"));
        return false;
    }
    return true;
}

bool VulkanZeroCopyFaceDetector::createPipeline(QString* errorMessage)
{
    const QByteArray spirv = readShader(QStringLiteral("face_preprocess.comp.spv"));
    VkShaderModule shader = createShaderModule(spirv, QStringLiteral("face_preprocess.comp.spv"), errorMessage);
    if (shader == VK_NULL_HANDLE) {
        return false;
    }

    VkPushConstantRange pushRange{};
    pushRange.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
    pushRange.offset = 0;
    pushRange.size = sizeof(FacePreprocessPushConstants);

    VkPipelineLayoutCreateInfo layoutInfo{};
    layoutInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    layoutInfo.setLayoutCount = 1;
    layoutInfo.pSetLayouts = &m_descriptorSetLayout;
    layoutInfo.pushConstantRangeCount = 1;
    layoutInfo.pPushConstantRanges = &pushRange;
    if (vkCreatePipelineLayout(m_context.device, &layoutInfo, nullptr, &m_pipelineLayout) != VK_SUCCESS) {
        vkDestroyShaderModule(m_context.device, shader, nullptr);
        setError(errorMessage, QStringLiteral("failed to create zero-copy detector pipeline layout"));
        return false;
    }

    VkPipelineShaderStageCreateInfo stage{};
    stage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
    stage.module = shader;
    stage.pName = "main";

    VkComputePipelineCreateInfo pipelineInfo{};
    pipelineInfo.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO;
    pipelineInfo.stage = stage;
    pipelineInfo.layout = m_pipelineLayout;
    const VkResult result = vkCreateComputePipelines(m_context.device, VK_NULL_HANDLE, 1, &pipelineInfo, nullptr, &m_pipeline);
    vkDestroyShaderModule(m_context.device, shader, nullptr);
    if (result != VK_SUCCESS) {
        setError(errorMessage, QStringLiteral("failed to create zero-copy detector preprocessing pipeline"));
        return false;
    }
    return true;
}

bool VulkanZeroCopyFaceDetector::createInferenceDescriptorResources(QString* errorMessage)
{
    std::array<VkDescriptorSetLayoutBinding, 2> bindings{};
    bindings[0].binding = 0;
    bindings[0].descriptorCount = 1;
    bindings[0].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    bindings[0].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
    bindings[1].binding = 1;
    bindings[1].descriptorCount = 1;
    bindings[1].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    bindings[1].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;

    VkDescriptorSetLayoutCreateInfo layoutInfo{};
    layoutInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    layoutInfo.bindingCount = static_cast<uint32_t>(bindings.size());
    layoutInfo.pBindings = bindings.data();
    if (vkCreateDescriptorSetLayout(m_context.device, &layoutInfo, nullptr, &m_inferenceDescriptorSetLayout) != VK_SUCCESS) {
        setError(errorMessage, QStringLiteral("failed to create Vulkan face inference descriptor set layout"));
        return false;
    }

    std::array<VkDescriptorPoolSize, 1> poolSizes{};
    poolSizes[0].type = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    poolSizes[0].descriptorCount = 16;
    VkDescriptorPoolCreateInfo poolInfo{};
    poolInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    poolInfo.flags = VK_DESCRIPTOR_POOL_CREATE_FREE_DESCRIPTOR_SET_BIT;
    poolInfo.maxSets = 8;
    poolInfo.poolSizeCount = static_cast<uint32_t>(poolSizes.size());
    poolInfo.pPoolSizes = poolSizes.data();
    if (vkCreateDescriptorPool(m_context.device, &poolInfo, nullptr, &m_inferenceDescriptorPool) != VK_SUCCESS) {
        setError(errorMessage, QStringLiteral("failed to create Vulkan face inference descriptor pool"));
        return false;
    }
    return true;
}

bool VulkanZeroCopyFaceDetector::createInferencePipeline(QString* errorMessage)
{
    const QByteArray spirv = readShader(QStringLiteral("face_infer_heuristic.comp.spv"));
    VkShaderModule shader = createShaderModule(spirv, QStringLiteral("face_infer_heuristic.comp.spv"), errorMessage);
    if (shader == VK_NULL_HANDLE) {
        return false;
    }

    VkPushConstantRange pushRange{};
    pushRange.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
    pushRange.offset = 0;
    pushRange.size = sizeof(FaceInferencePushConstants);

    VkPipelineLayoutCreateInfo layoutInfo{};
    layoutInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    layoutInfo.setLayoutCount = 1;
    layoutInfo.pSetLayouts = &m_inferenceDescriptorSetLayout;
    layoutInfo.pushConstantRangeCount = 1;
    layoutInfo.pPushConstantRanges = &pushRange;
    if (vkCreatePipelineLayout(m_context.device, &layoutInfo, nullptr, &m_inferencePipelineLayout) != VK_SUCCESS) {
        vkDestroyShaderModule(m_context.device, shader, nullptr);
        setError(errorMessage, QStringLiteral("failed to create Vulkan face inference pipeline layout"));
        return false;
    }

    VkPipelineShaderStageCreateInfo stage{};
    stage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
    stage.module = shader;
    stage.pName = "main";

    VkComputePipelineCreateInfo pipelineInfo{};
    pipelineInfo.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO;
    pipelineInfo.stage = stage;
    pipelineInfo.layout = m_inferencePipelineLayout;
    const VkResult result = vkCreateComputePipelines(m_context.device, VK_NULL_HANDLE, 1, &pipelineInfo, nullptr, &m_inferencePipeline);
    vkDestroyShaderModule(m_context.device, shader, nullptr);
    if (result != VK_SUCCESS) {
        setError(errorMessage, QStringLiteral("failed to create Vulkan face inference pipeline"));
        return false;
    }
    return true;
}

bool VulkanZeroCopyFaceDetector::createCommandResources(QString* errorMessage)
{
    VkCommandPoolCreateInfo poolInfo{};
    poolInfo.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
    poolInfo.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    poolInfo.queueFamilyIndex = m_context.queueFamilyIndex;
    if (vkCreateCommandPool(m_context.device, &poolInfo, nullptr, &m_commandPool) != VK_SUCCESS) {
        setError(errorMessage, QStringLiteral("failed to create zero-copy detector command pool"));
        return false;
    }

    VkCommandBufferAllocateInfo allocInfo{};
    allocInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    allocInfo.commandPool = m_commandPool;
    allocInfo.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    allocInfo.commandBufferCount = 1;
    if (vkAllocateCommandBuffers(m_context.device, &allocInfo, &m_commandBuffer) != VK_SUCCESS) {
        setError(errorMessage, QStringLiteral("failed to allocate zero-copy detector command buffer"));
        return false;
    }

    VkFenceCreateInfo fenceInfo{};
    fenceInfo.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
    if (vkCreateFence(m_context.device, &fenceInfo, nullptr, &m_fence) != VK_SUCCESS) {
        setError(errorMessage, QStringLiteral("failed to create zero-copy detector fence"));
        return false;
    }
    return true;
}

bool VulkanZeroCopyFaceDetector::createSampler(QString* errorMessage)
{
    VkSamplerCreateInfo samplerInfo{};
    samplerInfo.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
    samplerInfo.magFilter = VK_FILTER_LINEAR;
    samplerInfo.minFilter = VK_FILTER_LINEAR;
    samplerInfo.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
    samplerInfo.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    samplerInfo.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    samplerInfo.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    samplerInfo.maxLod = 1.0f;
    if (vkCreateSampler(m_context.device, &samplerInfo, nullptr, &m_sampler) != VK_SUCCESS) {
        setError(errorMessage, QStringLiteral("failed to create zero-copy detector sampler"));
        return false;
    }
    return true;
}

VkShaderModule VulkanZeroCopyFaceDetector::createShaderModule(const QByteArray& spirv, const QString& name, QString* errorMessage) const
{
    if (spirv.isEmpty() || (spirv.size() % 4) != 0) {
        setError(errorMessage, QStringLiteral("missing or invalid %1").arg(name));
        return VK_NULL_HANDLE;
    }
    VkShaderModuleCreateInfo info{};
    info.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
    info.codeSize = static_cast<size_t>(spirv.size());
    info.pCode = reinterpret_cast<const uint32_t*>(spirv.constData());
    VkShaderModule module = VK_NULL_HANDLE;
    if (vkCreateShaderModule(m_context.device, &info, nullptr, &module) != VK_SUCCESS) {
        setError(errorMessage, QStringLiteral("failed to create zero-copy detector shader module"));
        return VK_NULL_HANDLE;
    }
    return module;
}

bool VulkanZeroCopyFaceDetector::submitInference(VkDescriptorSet descriptorSet,
                                                 const VulkanTensorBuffer& outputDetections,
                                                 int maxDetections,
                                                 float threshold,
                                                 QString* errorMessage)
{
    vkResetFences(m_context.device, 1, &m_fence);
    vkResetCommandBuffer(m_commandBuffer, 0);

    VkCommandBufferBeginInfo beginInfo{};
    beginInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    beginInfo.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    if (vkBeginCommandBuffer(m_commandBuffer, &beginInfo) != VK_SUCCESS) {
        setError(errorMessage, QStringLiteral("failed to begin Vulkan face inference command buffer"));
        return false;
    }

    const uint32_t zero = 0;
    vkCmdFillBuffer(m_commandBuffer, outputDetections.buffer, 0, sizeof(uint32_t), zero);

    VkMemoryBarrier clearBarrier{};
    clearBarrier.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER;
    clearBarrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    clearBarrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT;
    vkCmdPipelineBarrier(m_commandBuffer,
                         VK_PIPELINE_STAGE_TRANSFER_BIT,
                         VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                         0,
                         1,
                         &clearBarrier,
                         0,
                         nullptr,
                         0,
                         nullptr);

    vkCmdBindPipeline(m_commandBuffer, VK_PIPELINE_BIND_POINT_COMPUTE, m_inferencePipeline);
    vkCmdBindDescriptorSets(m_commandBuffer,
                            VK_PIPELINE_BIND_POINT_COMPUTE,
                            m_inferencePipelineLayout,
                            0,
                            1,
                            &descriptorSet,
                            0,
                            nullptr);

    for (int scale = 0; scale < 3; ++scale) {
        FaceInferencePushConstants push{};
        push.tensorWidth = m_tensorSpec.width;
        push.tensorHeight = m_tensorSpec.height;
        push.gridWidth = 18;
        push.gridHeight = 18;
        push.threshold = threshold;
        push.scaleIndex = scale;
        vkCmdPushConstants(m_commandBuffer,
                           m_inferencePipelineLayout,
                           VK_SHADER_STAGE_COMPUTE_BIT,
                           0,
                           sizeof(push),
                           &push);
        vkCmdDispatch(m_commandBuffer,
                      static_cast<uint32_t>((push.gridWidth + 15) / 16),
                      static_cast<uint32_t>((push.gridHeight + 15) / 16),
                      1);
    }

    VkMemoryBarrier barrier{};
    barrier.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER;
    barrier.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
    barrier.dstAccessMask = VK_ACCESS_HOST_READ_BIT | VK_ACCESS_TRANSFER_READ_BIT;
    vkCmdPipelineBarrier(m_commandBuffer,
                         VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                         VK_PIPELINE_STAGE_HOST_BIT | VK_PIPELINE_STAGE_TRANSFER_BIT,
                         0,
                         1,
                         &barrier,
                         0,
                         nullptr,
                         0,
                         nullptr);

    if (vkEndCommandBuffer(m_commandBuffer) != VK_SUCCESS) {
        setError(errorMessage, QStringLiteral("failed to end Vulkan face inference command buffer"));
        return false;
    }

    VkSubmitInfo submitInfo{};
    submitInfo.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    submitInfo.commandBufferCount = 1;
    submitInfo.pCommandBuffers = &m_commandBuffer;
    if (vkQueueSubmit(m_context.queue, 1, &submitInfo, m_fence) != VK_SUCCESS) {
        setError(errorMessage, QStringLiteral("failed to submit Vulkan face inference work"));
        return false;
    }
    if (vkWaitForFences(m_context.device, 1, &m_fence, VK_TRUE, 5'000'000'000ull) != VK_SUCCESS) {
        setError(errorMessage, QStringLiteral("timed out waiting for Vulkan face inference"));
        return false;
    }
    Q_UNUSED(maxDetections);
    return true;
}

bool VulkanZeroCopyFaceDetector::submitPreprocess(VkDescriptorSet descriptorSet,
                                                  const VulkanExternalImage& source,
                                                  QString* errorMessage)
{
    Q_UNUSED(source.size);

    vkResetFences(m_context.device, 1, &m_fence);
    vkResetCommandBuffer(m_commandBuffer, 0);

    VkCommandBufferBeginInfo beginInfo{};
    beginInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    beginInfo.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    if (vkBeginCommandBuffer(m_commandBuffer, &beginInfo) != VK_SUCCESS) {
        setError(errorMessage, QStringLiteral("failed to begin zero-copy detector command buffer"));
        return false;
    }

    vkCmdBindPipeline(m_commandBuffer, VK_PIPELINE_BIND_POINT_COMPUTE, m_pipeline);
    vkCmdBindDescriptorSets(m_commandBuffer,
                            VK_PIPELINE_BIND_POINT_COMPUTE,
                            m_pipelineLayout,
                            0,
                            1,
                            &descriptorSet,
                            0,
                            nullptr);

    FacePreprocessPushConstants push{};
    push.dstWidth = m_tensorSpec.width;
    push.dstHeight = m_tensorSpec.height;
    push.sourceIsSrgb = source.sourceIsSrgb ? 1 : 0;
    vkCmdPushConstants(m_commandBuffer,
                       m_pipelineLayout,
                       VK_SHADER_STAGE_COMPUTE_BIT,
                       0,
                       sizeof(push),
                       &push);

    const uint32_t groupsX = static_cast<uint32_t>((m_tensorSpec.width + 15) / 16);
    const uint32_t groupsY = static_cast<uint32_t>((m_tensorSpec.height + 15) / 16);
    vkCmdDispatch(m_commandBuffer, groupsX, groupsY, 1);

    VkMemoryBarrier barrier{};
    barrier.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER;
    barrier.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
    barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_TRANSFER_READ_BIT;
    vkCmdPipelineBarrier(m_commandBuffer,
                         VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                         VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_TRANSFER_BIT,
                         0,
                         1,
                         &barrier,
                         0,
                         nullptr,
                         0,
                         nullptr);

    if (vkEndCommandBuffer(m_commandBuffer) != VK_SUCCESS) {
        setError(errorMessage, QStringLiteral("failed to end zero-copy detector command buffer"));
        return false;
    }

    VkSubmitInfo submitInfo{};
    submitInfo.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    submitInfo.commandBufferCount = 1;
    submitInfo.pCommandBuffers = &m_commandBuffer;
    if (vkQueueSubmit(m_context.queue, 1, &submitInfo, m_fence) != VK_SUCCESS) {
        setError(errorMessage, QStringLiteral("failed to submit zero-copy detector preprocessing work"));
        return false;
    }
    if (vkWaitForFences(m_context.device, 1, &m_fence, VK_TRUE, 5'000'000'000ull) != VK_SUCCESS) {
        setError(errorMessage, QStringLiteral("timed out waiting for zero-copy detector preprocessing"));
        return false;
    }
    return true;
}

} // namespace jcut::vulkan_detector
