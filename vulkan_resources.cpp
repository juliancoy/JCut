#include "vulkan_resources.h"

#include <QVulkanFunctions>

#include <QFile>

#include <algorithm>
#include <array>
#include <cstring>
#include <limits>

#ifndef JCUT_VULKAN_SHADER_DIR
#define JCUT_VULKAN_SHADER_DIR ""
#endif

namespace {
constexpr uint32_t kTextureWidth = 64;
constexpr uint32_t kTextureHeight = 64;
constexpr VkDeviceSize kTextureBytes = static_cast<VkDeviceSize>(kTextureWidth * kTextureHeight * 4);
constexpr uint32_t kCurveLutWidth = 256;
constexpr uint32_t kCurveLutHeight = 1;
constexpr VkDeviceSize kCurveLutBytes = static_cast<VkDeviceSize>(kCurveLutWidth * kCurveLutHeight * 4);

bool checkedAdd(VkDeviceSize a, VkDeviceSize b, VkDeviceSize* out)
{
    if (!out || a > std::numeric_limits<VkDeviceSize>::max() - b) {
        return false;
    }
    *out = a + b;
    return true;
}

bool checkedMul(VkDeviceSize a, VkDeviceSize b, VkDeviceSize* out)
{
    if (!out || (a != 0 && b > std::numeric_limits<VkDeviceSize>::max() / a)) {
        return false;
    }
    *out = a * b;
    return true;
}

bool alignUp(VkDeviceSize value, VkDeviceSize alignment, VkDeviceSize* out)
{
    if (!out || alignment == 0) {
        return false;
    }
    const VkDeviceSize remainder = value % alignment;
    if (remainder == 0) {
        *out = value;
        return true;
    }
    return checkedAdd(value, alignment - remainder, out);
}

struct MaskPreparePush {
    int outputSize[2];
    int inputSize[2];
    int invert = 0;
    int pad0 = 0;
};

struct MaskMorphBlurPush {
    int outputSize[2];
    int radius = 0;
    int mode = 0;
};
}

VulkanResources::~VulkanResources()
{
    destroy();
}

bool VulkanResources::initialize(VkPhysicalDevice physicalDevice,
                                 VkDevice device,
                                 QVulkanDeviceFunctions* funcs)
{
    destroy();
    if (!physicalDevice || !device) {
        return false;
    }
    m_physicalDevice = physicalDevice;
    m_device = device;
    m_funcs = funcs;

    VkSamplerCreateInfo samplerInfo{};
    samplerInfo.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
    samplerInfo.magFilter = VK_FILTER_LINEAR;
    samplerInfo.minFilter = VK_FILTER_LINEAR;
    samplerInfo.mipmapMode = VK_SAMPLER_MIPMAP_MODE_LINEAR;
    samplerInfo.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    samplerInfo.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    samplerInfo.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    samplerInfo.maxAnisotropy = 1.0f;
    if (vkCreateSampler(m_device, &samplerInfo, nullptr, &m_sampler) != VK_SUCCESS) {
        destroy();
        return false;
    }

    VkDescriptorSetLayoutBinding bindings[5]{};
    bindings[0].binding = 0;
    bindings[0].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    bindings[0].descriptorCount = 1;
    bindings[0].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
    bindings[1].binding = 1;
    bindings[1].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    bindings[1].descriptorCount = 1;
    bindings[1].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
    bindings[2].binding = 2;
    bindings[2].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    bindings[2].descriptorCount = 1;
    bindings[2].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
    bindings[3].binding = 3;
    bindings[3].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    bindings[3].descriptorCount = 1;
    bindings[3].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
    bindings[4].binding = 4;
    bindings[4].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
    bindings[4].descriptorCount = 1;
    bindings[4].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
    VkDescriptorSetLayoutCreateInfo descriptorSetLayoutInfo{};
    descriptorSetLayoutInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    descriptorSetLayoutInfo.bindingCount = 5;
    descriptorSetLayoutInfo.pBindings = bindings;
    if (vkCreateDescriptorSetLayout(m_device, &descriptorSetLayoutInfo, nullptr, &m_descriptorSetLayout) != VK_SUCCESS) {
        destroy();
        return false;
    }

    VkDescriptorPoolSize poolSizes[2]{};
    poolSizes[0].type = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    poolSizes[0].descriptorCount = static_cast<uint32_t>(VulkanResources::kDescriptorSetCount * 4);
    poolSizes[1].type = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
    poolSizes[1].descriptorCount = static_cast<uint32_t>(VulkanResources::kDescriptorSetCount);
    VkDescriptorPoolCreateInfo descriptorPoolInfo{};
    descriptorPoolInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    descriptorPoolInfo.poolSizeCount = 2;
    descriptorPoolInfo.pPoolSizes = poolSizes;
    descriptorPoolInfo.maxSets = static_cast<uint32_t>(VulkanResources::kDescriptorSetCount);
    if (vkCreateDescriptorPool(m_device, &descriptorPoolInfo, nullptr, &m_descriptorPool) != VK_SUCCESS) {
        destroy();
        return false;
    }

    std::array<VkDescriptorSetLayout, VulkanResources::kDescriptorSetCount> layouts{};
    layouts.fill(m_descriptorSetLayout);
    VkDescriptorSetAllocateInfo descriptorSetAllocInfo{};
    descriptorSetAllocInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
    descriptorSetAllocInfo.descriptorPool = m_descriptorPool;
    descriptorSetAllocInfo.descriptorSetCount = static_cast<uint32_t>(m_descriptorSets.size());
    descriptorSetAllocInfo.pSetLayouts = layouts.data();
    if (vkAllocateDescriptorSets(m_device, &descriptorSetAllocInfo, m_descriptorSets.data()) != VK_SUCCESS) {
        destroy();
        return false;
    }

    VkBufferCreateInfo uniformBufferInfo{};
    uniformBufferInfo.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    uniformBufferInfo.size = sizeof(float) * 4;
    uniformBufferInfo.usage = VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT;
    uniformBufferInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    if (vkCreateBuffer(m_device, &uniformBufferInfo, nullptr, &m_frameUniformBuffer) != VK_SUCCESS) {
        destroy();
        return false;
    }
    VkMemoryRequirements uniformRequirements{};
    vkGetBufferMemoryRequirements(m_device, m_frameUniformBuffer, &uniformRequirements);
    VkMemoryAllocateInfo uniformAlloc{};
    uniformAlloc.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    uniformAlloc.allocationSize = uniformRequirements.size;
    uniformAlloc.memoryTypeIndex = findMemoryType(
        uniformRequirements.memoryTypeBits,
        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    if (vkAllocateMemory(m_device, &uniformAlloc, nullptr, &m_frameUniformMemory) != VK_SUCCESS ||
        vkBindBufferMemory(m_device, m_frameUniformBuffer, m_frameUniformMemory, 0) != VK_SUCCESS ||
        vkMapMemory(m_device, m_frameUniformMemory, 0, sizeof(float) * 4, 0, &m_frameUniformMapped) != VK_SUCCESS) {
        destroy();
        return false;
    }
    updateFrameUniform(QSize(1, 1));

    if (!createTextureResources() || !createMaskComputeResources()) {
        destroy();
        return false;
    }
    m_initialized = true;
    return true;
}

void VulkanResources::destroy()
{
    if (!m_device) {
        m_physicalDevice = VK_NULL_HANDLE;
        m_device = VK_NULL_HANDLE;
        m_funcs = nullptr;
        m_initialized = false;
        return;
    }
    vkDeviceWaitIdle(m_device);
    if (m_stagingBuffer != VK_NULL_HANDLE) {
        vkDestroyBuffer(m_device, m_stagingBuffer, nullptr);
        m_stagingBuffer = VK_NULL_HANDLE;
    }
    if (m_stagingMemory != VK_NULL_HANDLE) {
        vkFreeMemory(m_device, m_stagingMemory, nullptr);
        m_stagingMemory = VK_NULL_HANDLE;
    }
    for (RetiredStagingBuffer& retired : m_retiredStagingBuffers) {
        if (retired.buffer != VK_NULL_HANDLE) {
            vkDestroyBuffer(m_device, retired.buffer, nullptr);
            retired.buffer = VK_NULL_HANDLE;
        }
        if (retired.memory != VK_NULL_HANDLE) {
            vkFreeMemory(m_device, retired.memory, nullptr);
            retired.memory = VK_NULL_HANDLE;
        }
    }
    m_retiredStagingBuffers.clear();
    for (RetiredImageResource& retired : m_retiredImageResources) {
        if (retired.view != VK_NULL_HANDLE) {
            vkDestroyImageView(m_device, retired.view, nullptr);
            retired.view = VK_NULL_HANDLE;
        }
        if (retired.image != VK_NULL_HANDLE) {
            vkDestroyImage(m_device, retired.image, nullptr);
            retired.image = VK_NULL_HANDLE;
        }
        if (retired.memory != VK_NULL_HANDLE) {
            vkFreeMemory(m_device, retired.memory, nullptr);
            retired.memory = VK_NULL_HANDLE;
        }
    }
    m_retiredImageResources.clear();
    destroyMaskComputeResources();
    destroyTextureImage();
    if (m_curveLutView != VK_NULL_HANDLE) {
        vkDestroyImageView(m_device, m_curveLutView, nullptr);
        m_curveLutView = VK_NULL_HANDLE;
    }
    if (m_curveLutImage != VK_NULL_HANDLE) {
        vkDestroyImage(m_device, m_curveLutImage, nullptr);
        m_curveLutImage = VK_NULL_HANDLE;
    }
    if (m_curveLutMemory != VK_NULL_HANDLE) {
        vkFreeMemory(m_device, m_curveLutMemory, nullptr);
        m_curveLutMemory = VK_NULL_HANDLE;
    }
    if (m_maskCurveLutView != VK_NULL_HANDLE) {
        vkDestroyImageView(m_device, m_maskCurveLutView, nullptr);
        m_maskCurveLutView = VK_NULL_HANDLE;
    }
    if (m_maskCurveLutImage != VK_NULL_HANDLE) {
        vkDestroyImage(m_device, m_maskCurveLutImage, nullptr);
        m_maskCurveLutImage = VK_NULL_HANDLE;
    }
    if (m_maskCurveLutMemory != VK_NULL_HANDLE) {
        vkFreeMemory(m_device, m_maskCurveLutMemory, nullptr);
        m_maskCurveLutMemory = VK_NULL_HANDLE;
    }
    if (m_maskView != VK_NULL_HANDLE) {
        vkDestroyImageView(m_device, m_maskView, nullptr);
        m_maskView = VK_NULL_HANDLE;
    }
    if (m_maskImage != VK_NULL_HANDLE) {
        vkDestroyImage(m_device, m_maskImage, nullptr);
        m_maskImage = VK_NULL_HANDLE;
    }
    if (m_maskMemory != VK_NULL_HANDLE) {
        vkFreeMemory(m_device, m_maskMemory, nullptr);
        m_maskMemory = VK_NULL_HANDLE;
    }
    destroyMaskImage(m_maskRawImage, m_maskRawMemory, m_maskRawView);
    destroyMaskImage(m_maskWorkImage, m_maskWorkMemory, m_maskWorkView);
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
    m_descriptorSets.fill(VK_NULL_HANDLE);
    m_descriptorSetIndex = 0;
    if (m_descriptorSetLayout != VK_NULL_HANDLE) {
        vkDestroyDescriptorSetLayout(m_device, m_descriptorSetLayout, nullptr);
        m_descriptorSetLayout = VK_NULL_HANDLE;
    }
    if (m_sampler != VK_NULL_HANDLE) {
        vkDestroySampler(m_device, m_sampler, nullptr);
        m_sampler = VK_NULL_HANDLE;
    }
    m_textureLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    m_textureUploaded = false;
    m_textureSize = QSize();
    m_curveLutLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    m_curveLutBytes.clear();
    m_maskCurveLutLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    m_maskCurveLutBytes.clear();
    m_maskLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    m_maskRawLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    m_maskWorkLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    m_maskRawSize = QSize();
    m_uploadedMaskCacheKey = 0;
    m_uploadedMaskOutputSize = QSize();
    m_maskSize = QSize();
    m_stagingRing.reset();
    m_initialized = false;
    m_physicalDevice = VK_NULL_HANDLE;
    m_device = VK_NULL_HANDLE;
    m_funcs = nullptr;
}

bool VulkanResources::createTextureResources()
{
    auto createImageAndView = [this](uint32_t width,
                                     uint32_t height,
                                     VkImage* image,
                                     VkDeviceMemory* memory,
                                     VkImageView* view) -> bool {
    VkImageCreateInfo imageInfo{};
    imageInfo.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    imageInfo.imageType = VK_IMAGE_TYPE_2D;
    imageInfo.extent.width = width;
    imageInfo.extent.height = height;
    imageInfo.extent.depth = 1;
    imageInfo.mipLevels = 1;
    imageInfo.arrayLayers = 1;
    imageInfo.format = VK_FORMAT_R8G8B8A8_UNORM;
    imageInfo.tiling = VK_IMAGE_TILING_OPTIMAL;
    imageInfo.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    imageInfo.usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
    imageInfo.samples = VK_SAMPLE_COUNT_1_BIT;
    imageInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    if (vkCreateImage(m_device, &imageInfo, nullptr, image) != VK_SUCCESS) {
        return false;
    }

    VkMemoryRequirements imageReq{};
    vkGetImageMemoryRequirements(m_device, *image, &imageReq);
    const uint32_t imageMemType = findMemoryType(imageReq.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    if (imageMemType == UINT32_MAX) {
        return false;
    }
    VkMemoryAllocateInfo imageAlloc{};
    imageAlloc.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    imageAlloc.allocationSize = imageReq.size;
    imageAlloc.memoryTypeIndex = imageMemType;
    if (vkAllocateMemory(m_device, &imageAlloc, nullptr, memory) != VK_SUCCESS) {
        return false;
    }
    if (vkBindImageMemory(m_device, *image, *memory, 0) != VK_SUCCESS) {
        return false;
    }

    VkImageViewCreateInfo viewInfo{};
    viewInfo.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    viewInfo.image = *image;
    viewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
    viewInfo.format = VK_FORMAT_R8G8B8A8_UNORM;
    viewInfo.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    viewInfo.subresourceRange.levelCount = 1;
    viewInfo.subresourceRange.layerCount = 1;
    if (vkCreateImageView(m_device, &viewInfo, nullptr, view) != VK_SUCCESS) {
        return false;
    }
    return true;
    };

    if (!createTextureImage(QSize(static_cast<int>(kTextureWidth), static_cast<int>(kTextureHeight))) ||
        !createImageAndView(kCurveLutWidth, kCurveLutHeight, &m_curveLutImage, &m_curveLutMemory, &m_curveLutView) ||
        !createImageAndView(kCurveLutWidth, kCurveLutHeight, &m_maskCurveLutImage, &m_maskCurveLutMemory, &m_maskCurveLutView) ||
        !createMaskImage(QSize(static_cast<int>(kTextureWidth), static_cast<int>(kTextureHeight)),
                         &m_maskRawImage, &m_maskRawMemory, &m_maskRawView) ||
        !createMaskImage(QSize(static_cast<int>(kTextureWidth), static_cast<int>(kTextureHeight)),
                         &m_maskImage, &m_maskMemory, &m_maskView) ||
        !createMaskImage(QSize(static_cast<int>(kTextureWidth), static_cast<int>(kTextureHeight)),
                         &m_maskWorkImage, &m_maskWorkMemory, &m_maskWorkView)) {
        return false;
    }
    m_maskLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    m_maskRawLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    m_maskWorkLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    m_maskSize = QSize(static_cast<int>(kTextureWidth), static_cast<int>(kTextureHeight));
    m_maskRawSize = QSize(static_cast<int>(kTextureWidth), static_cast<int>(kTextureHeight));

    m_stagingRing.frameSlotCount = qMax<size_t>(1, VulkanResources::kDescriptorSetCount);
    m_stagingRing.frameSlotBytes = std::max(kTextureBytes, kCurveLutBytes);
    VkDeviceSize initialStagingBytes = 0;
    if (!checkedMul(m_stagingRing.frameSlotBytes,
                    static_cast<VkDeviceSize>(m_stagingRing.frameSlotCount),
                    &initialStagingBytes) ||
        !ensureStagingCapacity(initialStagingBytes)) {
        return false;
    }

    VkDescriptorImageInfo imageInfoDesc[4]{};
    imageInfoDesc[0].imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    imageInfoDesc[0].imageView = m_textureView;
    imageInfoDesc[0].sampler = m_sampler;
    imageInfoDesc[1].imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    imageInfoDesc[1].imageView = m_curveLutView;
    imageInfoDesc[1].sampler = m_sampler;
    imageInfoDesc[2].imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    imageInfoDesc[2].imageView = m_maskView;
    imageInfoDesc[2].sampler = m_sampler;
    imageInfoDesc[3].imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    imageInfoDesc[3].imageView = m_maskCurveLutView;
    imageInfoDesc[3].sampler = m_sampler;
    VkDescriptorBufferInfo frameUniformInfo{};
    frameUniformInfo.buffer = m_frameUniformBuffer;
    frameUniformInfo.offset = 0;
    frameUniformInfo.range = sizeof(float) * 4;
    std::array<VkWriteDescriptorSet, VulkanResources::kDescriptorSetCount * 5> writes{};
    size_t writeIndex = 0;
    for (VkDescriptorSet descriptorSet : m_descriptorSets) {
        if (descriptorSet == VK_NULL_HANDLE) {
            return false;
        }
        for (uint32_t binding = 0; binding < 4; ++binding) {
            writes[writeIndex].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
            writes[writeIndex].dstSet = descriptorSet;
            writes[writeIndex].dstBinding = binding;
            writes[writeIndex].descriptorCount = 1;
            writes[writeIndex].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
            writes[writeIndex].pImageInfo = &imageInfoDesc[binding];
            ++writeIndex;
        }
        writes[writeIndex].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        writes[writeIndex].dstSet = descriptorSet;
        writes[writeIndex].dstBinding = 4;
        writes[writeIndex].descriptorCount = 1;
        writes[writeIndex].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
        writes[writeIndex].pBufferInfo = &frameUniformInfo;
        ++writeIndex;
    }
    vkUpdateDescriptorSets(m_device, static_cast<uint32_t>(writeIndex), writes.data(), 0, nullptr);
    return true;
}

bool VulkanResources::updateFrameUniform(const QSize& outputSize)
{
    if (!m_frameUniformMapped) {
        return false;
    }
    const float width = static_cast<float>(qMax(1, outputSize.width()));
    const float height = static_cast<float>(qMax(1, outputSize.height()));
    const float values[4] = {width, height, 1.0f / width, 1.0f / height};
    std::memcpy(m_frameUniformMapped, values, sizeof(values));
    return true;
}

bool VulkanResources::createTextureImage(const QSize& size)
{
    if (!size.isValid() || size.width() <= 0 || size.height() <= 0) {
        return false;
    }
    retireTextureImage();

    VkImageCreateInfo imageInfo{};
    imageInfo.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    imageInfo.imageType = VK_IMAGE_TYPE_2D;
    imageInfo.extent.width = static_cast<uint32_t>(size.width());
    imageInfo.extent.height = static_cast<uint32_t>(size.height());
    imageInfo.extent.depth = 1;
    imageInfo.mipLevels = 1;
    imageInfo.arrayLayers = 1;
    imageInfo.format = VK_FORMAT_R8G8B8A8_UNORM;
    imageInfo.tiling = VK_IMAGE_TILING_OPTIMAL;
    imageInfo.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    imageInfo.usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
    imageInfo.samples = VK_SAMPLE_COUNT_1_BIT;
    imageInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    if (vkCreateImage(m_device, &imageInfo, nullptr, &m_textureImage) != VK_SUCCESS) {
        return false;
    }

    VkMemoryRequirements imageReq{};
    vkGetImageMemoryRequirements(m_device, m_textureImage, &imageReq);
    const uint32_t imageMemType = findMemoryType(imageReq.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    if (imageMemType == UINT32_MAX) {
        destroyTextureImage();
        return false;
    }
    VkMemoryAllocateInfo imageAlloc{};
    imageAlloc.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    imageAlloc.allocationSize = imageReq.size;
    imageAlloc.memoryTypeIndex = imageMemType;
    if (vkAllocateMemory(m_device, &imageAlloc, nullptr, &m_textureMemory) != VK_SUCCESS) {
        destroyTextureImage();
        return false;
    }
    if (vkBindImageMemory(m_device, m_textureImage, m_textureMemory, 0) != VK_SUCCESS) {
        destroyTextureImage();
        return false;
    }

    VkImageViewCreateInfo viewInfo{};
    viewInfo.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    viewInfo.image = m_textureImage;
    viewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
    viewInfo.format = VK_FORMAT_R8G8B8A8_UNORM;
    viewInfo.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    viewInfo.subresourceRange.levelCount = 1;
    viewInfo.subresourceRange.layerCount = 1;
    if (vkCreateImageView(m_device, &viewInfo, nullptr, &m_textureView) != VK_SUCCESS) {
        destroyTextureImage();
        return false;
    }

    m_textureLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    m_textureUploaded = false;
    m_textureSize = size;
    setSampledImage(m_textureView, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    return true;
}

bool VulkanResources::createMaskImage(const QSize& size,
                                      VkImage* image,
                                      VkDeviceMemory* memory,
                                      VkImageView* view)
{
    if (!size.isValid() || size.width() <= 0 || size.height() <= 0 ||
        !image || !memory || !view) {
        return false;
    }

    VkImageCreateInfo imageInfo{};
    imageInfo.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    imageInfo.imageType = VK_IMAGE_TYPE_2D;
    imageInfo.extent.width = static_cast<uint32_t>(size.width());
    imageInfo.extent.height = static_cast<uint32_t>(size.height());
    imageInfo.extent.depth = 1;
    imageInfo.mipLevels = 1;
    imageInfo.arrayLayers = 1;
    imageInfo.format = VK_FORMAT_R8G8B8A8_UNORM;
    imageInfo.tiling = VK_IMAGE_TILING_OPTIMAL;
    imageInfo.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    imageInfo.usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT |
                      VK_IMAGE_USAGE_STORAGE_BIT |
                      VK_IMAGE_USAGE_SAMPLED_BIT;
    imageInfo.samples = VK_SAMPLE_COUNT_1_BIT;
    imageInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    if (vkCreateImage(m_device, &imageInfo, nullptr, image) != VK_SUCCESS) {
        return false;
    }

    VkMemoryRequirements imageReq{};
    vkGetImageMemoryRequirements(m_device, *image, &imageReq);
    const uint32_t imageMemType = findMemoryType(imageReq.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    if (imageMemType == UINT32_MAX) {
        destroyMaskImage(*image, *memory, *view);
        return false;
    }
    VkMemoryAllocateInfo imageAlloc{};
    imageAlloc.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    imageAlloc.allocationSize = imageReq.size;
    imageAlloc.memoryTypeIndex = imageMemType;
    if (vkAllocateMemory(m_device, &imageAlloc, nullptr, memory) != VK_SUCCESS ||
        vkBindImageMemory(m_device, *image, *memory, 0) != VK_SUCCESS) {
        destroyMaskImage(*image, *memory, *view);
        return false;
    }

    VkImageViewCreateInfo viewInfo{};
    viewInfo.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    viewInfo.image = *image;
    viewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
    viewInfo.format = VK_FORMAT_R8G8B8A8_UNORM;
    viewInfo.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    viewInfo.subresourceRange.levelCount = 1;
    viewInfo.subresourceRange.layerCount = 1;
    if (vkCreateImageView(m_device, &viewInfo, nullptr, view) != VK_SUCCESS) {
        destroyMaskImage(*image, *memory, *view);
        return false;
    }
    return true;
}

void VulkanResources::destroyMaskImage(VkImage& image, VkDeviceMemory& memory, VkImageView& view)
{
    if (view != VK_NULL_HANDLE) {
        vkDestroyImageView(m_device, view, nullptr);
        view = VK_NULL_HANDLE;
    }
    if (image != VK_NULL_HANDLE) {
        vkDestroyImage(m_device, image, nullptr);
        image = VK_NULL_HANDLE;
    }
    if (memory != VK_NULL_HANDLE) {
        vkFreeMemory(m_device, memory, nullptr);
        memory = VK_NULL_HANDLE;
    }
}

void VulkanResources::retireMaskImage(VkImage& image, VkDeviceMemory& memory, VkImageView& view)
{
    if (image != VK_NULL_HANDLE || memory != VK_NULL_HANDLE || view != VK_NULL_HANDLE) {
        RetiredImageResource retired;
        retired.image = image;
        retired.memory = memory;
        retired.view = view;
        m_retiredImageResources.push_back(retired);
    }
    image = VK_NULL_HANDLE;
    memory = VK_NULL_HANDLE;
    view = VK_NULL_HANDLE;
}

void VulkanResources::destroyTextureImage()
{
    if (m_textureView != VK_NULL_HANDLE) {
        vkDestroyImageView(m_device, m_textureView, nullptr);
        m_textureView = VK_NULL_HANDLE;
    }
    if (m_textureImage != VK_NULL_HANDLE) {
        vkDestroyImage(m_device, m_textureImage, nullptr);
        m_textureImage = VK_NULL_HANDLE;
    }
    if (m_textureMemory != VK_NULL_HANDLE) {
        vkFreeMemory(m_device, m_textureMemory, nullptr);
        m_textureMemory = VK_NULL_HANDLE;
    }
    m_textureLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    m_textureUploaded = false;
    m_textureSize = QSize();
}

void VulkanResources::retireTextureImage()
{
    retireMaskImage(m_textureImage, m_textureMemory, m_textureView);
    m_textureLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    m_textureUploaded = false;
    m_textureSize = QSize();
}

bool VulkanResources::ensureTextureSize(const QSize& size)
{
    if (!size.isValid() || size.width() <= 0 || size.height() <= 0) {
        return false;
    }
    if (m_textureView != VK_NULL_HANDLE && m_textureSize == size) {
        return true;
    }
    return createTextureImage(size);
}

bool VulkanResources::ensureMaskImages(const QSize& size)
{
    if (!size.isValid() || size.width() <= 0 || size.height() <= 0) {
        return false;
    }
    if (m_maskView != VK_NULL_HANDLE && m_maskWorkView != VK_NULL_HANDLE && m_maskSize == size) {
        return true;
    }

    retireMaskImage(m_maskImage, m_maskMemory, m_maskView);
    retireMaskImage(m_maskWorkImage, m_maskWorkMemory, m_maskWorkView);
    m_maskLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    m_maskWorkLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    if (!createMaskImage(size, &m_maskImage, &m_maskMemory, &m_maskView) ||
        !createMaskImage(size, &m_maskWorkImage, &m_maskWorkMemory, &m_maskWorkView)) {
        return false;
    }
    m_maskSize = size;

    VkDescriptorImageInfo maskInfo{};
    maskInfo.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    maskInfo.imageView = m_maskView;
    maskInfo.sampler = m_sampler;
    std::array<VkWriteDescriptorSet, VulkanResources::kDescriptorSetCount> writes{};
    for (size_t i = 0; i < m_descriptorSets.size(); ++i) {
        writes[i].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        writes[i].dstSet = m_descriptorSets[i];
        writes[i].dstBinding = 2;
        writes[i].descriptorCount = 1;
        writes[i].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        writes[i].pImageInfo = &maskInfo;
    }
    vkUpdateDescriptorSets(m_device, static_cast<uint32_t>(writes.size()), writes.data(), 0, nullptr);
    return true;
}

bool VulkanResources::ensureRawMaskImage(const QSize& size)
{
    if (!size.isValid() || size.width() <= 0 || size.height() <= 0) {
        return false;
    }
    if (m_maskRawView != VK_NULL_HANDLE && m_maskRawSize == size) {
        return true;
    }
    retireMaskImage(m_maskRawImage, m_maskRawMemory, m_maskRawView);
    m_maskRawLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    if (!createMaskImage(size, &m_maskRawImage, &m_maskRawMemory, &m_maskRawView)) {
        return false;
    }
    m_maskRawSize = size;
    return true;
}

bool VulkanResources::ensureStagingCapacity(VkDeviceSize bytes)
{
    if (bytes <= 0) {
        return false;
    }
    if (m_stagingBuffer != VK_NULL_HANDLE && m_stagingMemory != VK_NULL_HANDLE && m_stagingRing.capacity >= bytes) {
        return true;
    }
    if (m_stagingBuffer != VK_NULL_HANDLE || m_stagingMemory != VK_NULL_HANDLE) {
        RetiredStagingBuffer retired;
        retired.buffer = m_stagingBuffer;
        retired.memory = m_stagingMemory;
        m_retiredStagingBuffers.push_back(retired);
        m_stagingBuffer = VK_NULL_HANDLE;
        m_stagingMemory = VK_NULL_HANDLE;
        m_stagingRing.resetAllocation();
    }

    VkBufferCreateInfo bufferInfo{};
    bufferInfo.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    bufferInfo.size = bytes;
    bufferInfo.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
    bufferInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    if (vkCreateBuffer(m_device, &bufferInfo, nullptr, &m_stagingBuffer) != VK_SUCCESS) {
        return false;
    }
    VkMemoryRequirements bufferReq{};
    vkGetBufferMemoryRequirements(m_device, m_stagingBuffer, &bufferReq);
    const uint32_t bufferMemType = findMemoryType(
        bufferReq.memoryTypeBits,
        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    if (bufferMemType == UINT32_MAX) {
        if (m_stagingBuffer != VK_NULL_HANDLE) {
            vkDestroyBuffer(m_device, m_stagingBuffer, nullptr);
            m_stagingBuffer = VK_NULL_HANDLE;
        }
        return false;
    }
    VkMemoryAllocateInfo bufferAlloc{};
    bufferAlloc.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    bufferAlloc.allocationSize = bufferReq.size;
    bufferAlloc.memoryTypeIndex = bufferMemType;
    if (vkAllocateMemory(m_device, &bufferAlloc, nullptr, &m_stagingMemory) != VK_SUCCESS) {
        if (m_stagingBuffer != VK_NULL_HANDLE) {
            vkDestroyBuffer(m_device, m_stagingBuffer, nullptr);
            m_stagingBuffer = VK_NULL_HANDLE;
        }
        return false;
    }
    if (vkBindBufferMemory(m_device, m_stagingBuffer, m_stagingMemory, 0) != VK_SUCCESS) {
        if (m_stagingMemory != VK_NULL_HANDLE) {
            vkFreeMemory(m_device, m_stagingMemory, nullptr);
            m_stagingMemory = VK_NULL_HANDLE;
        }
        if (m_stagingBuffer != VK_NULL_HANDLE) {
            vkDestroyBuffer(m_device, m_stagingBuffer, nullptr);
            m_stagingBuffer = VK_NULL_HANDLE;
        }
        return false;
    }
    m_stagingRing.capacity = bytes;
    m_stagingRing.frameSlotBytes = qMax<VkDeviceSize>(
        bytes / qMax<VkDeviceSize>(1, static_cast<VkDeviceSize>(m_stagingRing.frameSlotCount)),
        m_stagingRing.frameSlotBytes);
    m_stagingRing.writeOffset = 0;
    return true;
}

bool VulkanResources::beginFrameUploads(size_t frameSlot, size_t frameSlotCount)
{
    m_stagingRing.frameSlotCount = qMax<size_t>(1, frameSlotCount);
    m_stagingRing.frameSlot = frameSlot % m_stagingRing.frameSlotCount;
    const VkDeviceSize slotBytes = m_stagingRing.frameSlotBytes > 0
                                      ? m_stagingRing.frameSlotBytes
                                      : m_stagingRing.capacity / qMax<VkDeviceSize>(
                                            1, static_cast<VkDeviceSize>(m_stagingRing.frameSlotCount));
    if (!checkedMul(static_cast<VkDeviceSize>(m_stagingRing.frameSlot),
                    slotBytes,
                    &m_stagingRing.writeOffset)) {
        return false;
    }
    return true;
}

bool VulkanResources::reserveStagingUpload(VkDeviceSize bytes, VkDeviceSize alignment, VkDeviceSize* offsetOut)
{
    if (!offsetOut || bytes <= 0) {
        return false;
    }
    const VkDeviceSize safeAlignment = qMax<VkDeviceSize>(alignment, 4);
    const VkDeviceSize slotCount = qMax<VkDeviceSize>(1, static_cast<VkDeviceSize>(m_stagingRing.frameSlotCount));
    VkDeviceSize slotBytes = m_stagingRing.frameSlotBytes > 0
                                 ? m_stagingRing.frameSlotBytes
                                 : qMax<VkDeviceSize>(m_stagingRing.capacity / slotCount, bytes);
    VkDeviceSize slotBase = 0;
    if (!checkedMul(static_cast<VkDeviceSize>(m_stagingRing.frameSlot % m_stagingRing.frameSlotCount),
                    slotBytes,
                    &slotBase)) {
        return false;
    }
    const VkDeviceSize slotCursor = m_stagingRing.writeOffset >= slotBase
                                        ? m_stagingRing.writeOffset - slotBase
                                        : 0;
    VkDeviceSize alignedCursor = 0;
    VkDeviceSize requiredInSlot = 0;
    if (!alignUp(slotCursor, safeAlignment, &alignedCursor) ||
        !checkedAdd(alignedCursor, bytes, &requiredInSlot)) {
        return false;
    }
    if (requiredInSlot > slotBytes) {
        VkDeviceSize doubledSlotBytes = 0;
        if (!checkedMul(slotBytes, 2, &doubledSlotBytes)) {
            doubledSlotBytes = std::numeric_limits<VkDeviceSize>::max();
        }
        slotBytes = qMax(requiredInSlot, qMax<VkDeviceSize>(doubledSlotBytes, bytes));
        m_stagingRing.frameSlotBytes = slotBytes;
        VkDeviceSize requiredCapacity = 0;
        if (!checkedMul(slotBytes, slotCount, &requiredCapacity) ||
            !ensureStagingCapacity(requiredCapacity)) {
            return false;
        }
        m_stagingRing.frameSlotBytes = slotBytes;
        if (!checkedMul(static_cast<VkDeviceSize>(m_stagingRing.frameSlot), slotBytes, &m_stagingRing.writeOffset)) {
            return false;
        }
    } else {
        VkDeviceSize requiredCapacity = 0;
        if (!checkedMul(slotBytes, slotCount, &requiredCapacity) ||
            !ensureStagingCapacity(requiredCapacity)) {
            return false;
        }
    }
    VkDeviceSize offset = 0;
    if (!checkedMul(static_cast<VkDeviceSize>(m_stagingRing.frameSlot), m_stagingRing.frameSlotBytes, &slotBase) ||
        !checkedAdd(slotBase, alignedCursor, &offset)) {
        return false;
    }
    VkDeviceSize uploadEnd = 0;
    VkDeviceSize slotEnd = 0;
    if (!checkedAdd(offset, bytes, &uploadEnd) ||
        !checkedMul(static_cast<VkDeviceSize>(m_stagingRing.frameSlot) + 1,
                    m_stagingRing.frameSlotBytes,
                    &slotEnd) ||
        uploadEnd > m_stagingRing.capacity ||
        uploadEnd > slotEnd) {
        return false;
    }
    m_stagingRing.writeOffset = uploadEnd;
    *offsetOut = offset;
    return true;
}

bool VulkanResources::writeStagingUpload(const void* data, VkDeviceSize bytes, VkDeviceSize* offsetOut)
{
    if (!data || !offsetOut || bytes <= 0) {
        return false;
    }
    VkDeviceSize stagingOffset = 0;
    if (!reserveStagingUpload(bytes, 4, &stagingOffset)) {
        return false;
    }
    void* mapped = nullptr;
    if (vkMapMemory(m_device, m_stagingMemory, stagingOffset, bytes, 0, &mapped) != VK_SUCCESS || !mapped) {
        return false;
    }
    std::memcpy(mapped, data, static_cast<size_t>(bytes));
    vkUnmapMemory(m_device, m_stagingMemory);
    *offsetOut = stagingOffset;
    return true;
}

uint32_t VulkanResources::findMemoryType(uint32_t typeBits, VkMemoryPropertyFlags properties) const
{
    if (!m_physicalDevice) {
        return UINT32_MAX;
    }
    VkPhysicalDeviceMemoryProperties memProps{};
    vkGetPhysicalDeviceMemoryProperties(m_physicalDevice, &memProps);
    for (uint32_t i = 0; i < memProps.memoryTypeCount; ++i) {
        if ((typeBits & (1u << i)) == 0) {
            continue;
        }
        const VkMemoryPropertyFlags flags = memProps.memoryTypes[i].propertyFlags;
        if ((flags & properties) == properties) {
            return i;
        }
    }
    return UINT32_MAX;
}

void VulkanResources::transitionTextureImage(VkCommandBuffer cb,
                                             VkImageLayout oldLayout,
                                             VkImageLayout newLayout,
                                             VkPipelineStageFlags srcStage,
                                             VkPipelineStageFlags dstStage,
                                             VkAccessFlags srcAccess,
                                             VkAccessFlags dstAccess)
{
    VkImageMemoryBarrier barrier{};
    barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    barrier.oldLayout = oldLayout;
    barrier.newLayout = newLayout;
    barrier.srcAccessMask = srcAccess;
    barrier.dstAccessMask = dstAccess;
    barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.image = m_textureImage;
    barrier.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    barrier.subresourceRange.levelCount = 1;
    barrier.subresourceRange.layerCount = 1;
    vkCmdPipelineBarrier(cb, srcStage, dstStage, 0, 0, nullptr, 0, nullptr, 1, &barrier);
}

static void transitionExternalImage(QVulkanDeviceFunctions* funcs,
                                    VkCommandBuffer cb,
                                    VkImage image,
                                    VkImageLayout oldLayout,
                                    VkImageLayout newLayout,
                                    VkPipelineStageFlags srcStage,
                                    VkPipelineStageFlags dstStage,
                                    VkAccessFlags srcAccess,
                                    VkAccessFlags dstAccess)
{
    Q_UNUSED(funcs);
    VkImageMemoryBarrier barrier{};
    barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    barrier.oldLayout = oldLayout;
    barrier.newLayout = newLayout;
    barrier.srcAccessMask = srcAccess;
    barrier.dstAccessMask = dstAccess;
    barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.image = image;
    barrier.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    barrier.subresourceRange.levelCount = 1;
    barrier.subresourceRange.layerCount = 1;
    vkCmdPipelineBarrier(cb, srcStage, dstStage, 0, 0, nullptr, 0, nullptr, 1, &barrier);
}

VkShaderModule VulkanResources::createShaderModule(const QString& path) const
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
    info.codeSize = static_cast<size_t>(bytes.size());
    info.pCode = reinterpret_cast<const uint32_t*>(bytes.constData());
    VkShaderModule module = VK_NULL_HANDLE;
    if (vkCreateShaderModule(m_device, &info, nullptr, &module) != VK_SUCCESS) {
        return VK_NULL_HANDLE;
    }
    return module;
}

bool VulkanResources::createMaskComputeResources()
{
    VkDescriptorSetLayoutBinding bindings[2]{};
    bindings[0].binding = 0;
    bindings[0].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    bindings[0].descriptorCount = 1;
    bindings[0].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
    bindings[1].binding = 1;
    bindings[1].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
    bindings[1].descriptorCount = 1;
    bindings[1].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
    VkDescriptorSetLayoutCreateInfo layoutInfo{};
    layoutInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    layoutInfo.bindingCount = 2;
    layoutInfo.pBindings = bindings;
    if (vkCreateDescriptorSetLayout(m_device, &layoutInfo, nullptr, &m_maskComputeDescriptorSetLayout) != VK_SUCCESS) {
        return false;
    }

    VkDescriptorPoolSize poolSizes[2]{};
    poolSizes[0].type = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    poolSizes[0].descriptorCount = static_cast<uint32_t>(m_maskComputeDescriptorSets.size());
    poolSizes[1].type = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
    poolSizes[1].descriptorCount = static_cast<uint32_t>(m_maskComputeDescriptorSets.size());
    VkDescriptorPoolCreateInfo poolInfo{};
    poolInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    poolInfo.poolSizeCount = 2;
    poolInfo.pPoolSizes = poolSizes;
    poolInfo.maxSets = static_cast<uint32_t>(m_maskComputeDescriptorSets.size());
    if (vkCreateDescriptorPool(m_device, &poolInfo, nullptr, &m_maskComputeDescriptorPool) != VK_SUCCESS) {
        return false;
    }

    std::array<VkDescriptorSetLayout, VulkanResources::kMaskComputeDescriptorSetCount> layouts{};
    layouts.fill(m_maskComputeDescriptorSetLayout);
    VkDescriptorSetAllocateInfo allocInfo{};
    allocInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
    allocInfo.descriptorPool = m_maskComputeDescriptorPool;
    allocInfo.descriptorSetCount = static_cast<uint32_t>(layouts.size());
    allocInfo.pSetLayouts = layouts.data();
    if (vkAllocateDescriptorSets(m_device, &allocInfo, m_maskComputeDescriptorSets.data()) != VK_SUCCESS) {
        return false;
    }

    VkPushConstantRange preparePush{};
    preparePush.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
    preparePush.offset = 0;
    preparePush.size = sizeof(MaskPreparePush);
    VkPipelineLayoutCreateInfo prepareLayoutInfo{};
    prepareLayoutInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    prepareLayoutInfo.setLayoutCount = 1;
    prepareLayoutInfo.pSetLayouts = &m_maskComputeDescriptorSetLayout;
    prepareLayoutInfo.pushConstantRangeCount = 1;
    prepareLayoutInfo.pPushConstantRanges = &preparePush;
    if (vkCreatePipelineLayout(m_device, &prepareLayoutInfo, nullptr, &m_maskPreparePipelineLayout) != VK_SUCCESS) {
        return false;
    }

    VkPushConstantRange morphPush{};
    morphPush.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
    morphPush.offset = 0;
    morphPush.size = sizeof(MaskMorphBlurPush);
    VkPipelineLayoutCreateInfo morphLayoutInfo = prepareLayoutInfo;
    morphLayoutInfo.pPushConstantRanges = &morphPush;
    if (vkCreatePipelineLayout(m_device, &morphLayoutInfo, nullptr, &m_maskMorphBlurPipelineLayout) != VK_SUCCESS) {
        return false;
    }

    const QString shaderDir = QStringLiteral(JCUT_VULKAN_SHADER_DIR);
    m_maskPrepareModule = createShaderModule(shaderDir + QStringLiteral("/mask_prepare.comp.spv"));
    m_maskMorphModule = createShaderModule(shaderDir + QStringLiteral("/mask_morph.comp.spv"));
    m_maskBlurModule = createShaderModule(shaderDir + QStringLiteral("/mask_blur.comp.spv"));
    if (m_maskPrepareModule == VK_NULL_HANDLE ||
        m_maskMorphModule == VK_NULL_HANDLE ||
        m_maskBlurModule == VK_NULL_HANDLE) {
        return false;
    }

    auto createComputePipeline = [this](VkShaderModule module,
                                        VkPipelineLayout layout,
                                        VkPipeline* pipeline) -> bool {
        VkComputePipelineCreateInfo info{};
        info.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO;
        info.stage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        info.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
        info.stage.module = module;
        info.stage.pName = "main";
        info.layout = layout;
        return vkCreateComputePipelines(m_device, VK_NULL_HANDLE, 1, &info, nullptr, pipeline) == VK_SUCCESS;
    };
    return createComputePipeline(m_maskPrepareModule, m_maskPreparePipelineLayout, &m_maskPreparePipeline) &&
           createComputePipeline(m_maskMorphModule, m_maskMorphBlurPipelineLayout, &m_maskMorphPipeline) &&
           createComputePipeline(m_maskBlurModule, m_maskMorphBlurPipelineLayout, &m_maskBlurPipeline);
}

void VulkanResources::destroyMaskComputeResources()
{
    if (m_maskBlurPipeline != VK_NULL_HANDLE) {
        vkDestroyPipeline(m_device, m_maskBlurPipeline, nullptr);
        m_maskBlurPipeline = VK_NULL_HANDLE;
    }
    if (m_maskMorphPipeline != VK_NULL_HANDLE) {
        vkDestroyPipeline(m_device, m_maskMorphPipeline, nullptr);
        m_maskMorphPipeline = VK_NULL_HANDLE;
    }
    if (m_maskPreparePipeline != VK_NULL_HANDLE) {
        vkDestroyPipeline(m_device, m_maskPreparePipeline, nullptr);
        m_maskPreparePipeline = VK_NULL_HANDLE;
    }
    if (m_maskBlurModule != VK_NULL_HANDLE) {
        vkDestroyShaderModule(m_device, m_maskBlurModule, nullptr);
        m_maskBlurModule = VK_NULL_HANDLE;
    }
    if (m_maskMorphModule != VK_NULL_HANDLE) {
        vkDestroyShaderModule(m_device, m_maskMorphModule, nullptr);
        m_maskMorphModule = VK_NULL_HANDLE;
    }
    if (m_maskPrepareModule != VK_NULL_HANDLE) {
        vkDestroyShaderModule(m_device, m_maskPrepareModule, nullptr);
        m_maskPrepareModule = VK_NULL_HANDLE;
    }
    if (m_maskMorphBlurPipelineLayout != VK_NULL_HANDLE) {
        vkDestroyPipelineLayout(m_device, m_maskMorphBlurPipelineLayout, nullptr);
        m_maskMorphBlurPipelineLayout = VK_NULL_HANDLE;
    }
    if (m_maskPreparePipelineLayout != VK_NULL_HANDLE) {
        vkDestroyPipelineLayout(m_device, m_maskPreparePipelineLayout, nullptr);
        m_maskPreparePipelineLayout = VK_NULL_HANDLE;
    }
    if (m_maskComputeDescriptorPool != VK_NULL_HANDLE) {
        vkDestroyDescriptorPool(m_device, m_maskComputeDescriptorPool, nullptr);
        m_maskComputeDescriptorPool = VK_NULL_HANDLE;
    }
    m_maskComputeDescriptorSets.fill(VK_NULL_HANDLE);
    m_maskComputeDescriptorSetIndex = 0;
    if (m_maskComputeDescriptorSetLayout != VK_NULL_HANDLE) {
        vkDestroyDescriptorSetLayout(m_device, m_maskComputeDescriptorSetLayout, nullptr);
        m_maskComputeDescriptorSetLayout = VK_NULL_HANDLE;
    }
}

bool VulkanResources::runMaskComputePass(VkCommandBuffer commandBuffer,
                                         VkPipeline pipeline,
                                         const void* pushData,
                                         uint32_t pushDataSize,
                                         VkImageView inputView,
                                         VkImageView outputView,
                                         VkImage outputImage,
                                         VkImageLayout& outputLayout)
{
    if (pipeline == VK_NULL_HANDLE || inputView == VK_NULL_HANDLE ||
        outputView == VK_NULL_HANDLE || outputImage == VK_NULL_HANDLE ||
        m_maskComputeDescriptorSets[m_maskComputeDescriptorSetIndex] == VK_NULL_HANDLE) {
        return false;
    }
    VkDescriptorSet computeDescriptorSet = m_maskComputeDescriptorSets[m_maskComputeDescriptorSetIndex];
    m_maskComputeDescriptorSetIndex = (m_maskComputeDescriptorSetIndex + 1) % m_maskComputeDescriptorSets.size();

    const VkPipelineStageFlags outputSrcStage =
        outputLayout == VK_IMAGE_LAYOUT_UNDEFINED
            ? VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT
            : (VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT);
    const VkAccessFlags outputSrcAccess =
        outputLayout == VK_IMAGE_LAYOUT_UNDEFINED ? 0 : VK_ACCESS_SHADER_READ_BIT;
    transitionExternalImage(m_funcs,
                            commandBuffer,
                            outputImage,
                            outputLayout,
                            VK_IMAGE_LAYOUT_GENERAL,
                            outputSrcStage,
                            VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                            outputSrcAccess,
                            VK_ACCESS_SHADER_WRITE_BIT);
    outputLayout = VK_IMAGE_LAYOUT_GENERAL;

    VkDescriptorImageInfo inputInfo{};
    inputInfo.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    inputInfo.imageView = inputView;
    inputInfo.sampler = m_sampler;
    VkDescriptorImageInfo outputInfo{};
    outputInfo.imageLayout = VK_IMAGE_LAYOUT_GENERAL;
    outputInfo.imageView = outputView;
    VkWriteDescriptorSet writes[2]{};
    writes[0].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    writes[0].dstSet = computeDescriptorSet;
    writes[0].dstBinding = 0;
    writes[0].descriptorCount = 1;
    writes[0].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    writes[0].pImageInfo = &inputInfo;
    writes[1].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    writes[1].dstSet = computeDescriptorSet;
    writes[1].dstBinding = 1;
    writes[1].descriptorCount = 1;
    writes[1].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
    writes[1].pImageInfo = &outputInfo;
    vkUpdateDescriptorSets(m_device, 2, writes, 0, nullptr);

    const VkPipelineLayout layout =
        pipeline == m_maskPreparePipeline ? m_maskPreparePipelineLayout : m_maskMorphBlurPipelineLayout;
    vkCmdBindPipeline(commandBuffer, VK_PIPELINE_BIND_POINT_COMPUTE, pipeline);
    vkCmdBindDescriptorSets(commandBuffer,
                            VK_PIPELINE_BIND_POINT_COMPUTE,
                            layout,
                            0,
                            1,
                            &computeDescriptorSet,
                            0,
                            nullptr);
    vkCmdPushConstants(commandBuffer, layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, pushDataSize, pushData);
    vkCmdDispatch(commandBuffer,
                  static_cast<uint32_t>((m_maskSize.width() + 15) / 16),
                  static_cast<uint32_t>((m_maskSize.height() + 15) / 16),
                  1);
    transitionExternalImage(m_funcs,
                            commandBuffer,
                            outputImage,
                            outputLayout,
                            VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                            VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                            VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
                            VK_ACCESS_SHADER_WRITE_BIT,
                            VK_ACCESS_SHADER_READ_BIT);
    outputLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    return true;
}

bool VulkanResources::preprocessMaskTexture(VkCommandBuffer commandBuffer,
                                            const VulkanMaskPreprocessOptions& options)
{
    if (!commandBuffer || m_maskSize.isEmpty() || m_maskRawSize.isEmpty()) {
        return false;
    }
    const int erodeRadius = std::max(0, options.erodeRadius);
    const int dilateRadius = std::max(0, options.dilateRadius);
    const int blurRadius = std::max(0, options.blurRadius);

    VkImageView currentView = m_maskRawView;
    bool currentIsMask = false;

    auto dispatchPrepare = [&](VkImageView srcView,
                               VkImageView dstView,
                               VkImage dstImage,
                               VkImageLayout& dstLayout,
                               const QSize& inputSize,
                               bool invert) -> bool {
        MaskPreparePush push{};
        push.outputSize[0] = m_maskSize.width();
        push.outputSize[1] = m_maskSize.height();
        push.inputSize[0] = inputSize.width();
        push.inputSize[1] = inputSize.height();
        push.invert = invert ? 1 : 0;
        return runMaskComputePass(commandBuffer,
                                  m_maskPreparePipeline,
                                  &push,
                                  sizeof(push),
                                  srcView,
                                  dstView,
                                  dstImage,
                                  dstLayout);
    };
    auto dispatchMorphBlur = [&](VkPipeline pipeline, int radius, int mode) -> bool {
        VkImageView dstView = currentIsMask ? m_maskWorkView : m_maskView;
        VkImage dstImage = currentIsMask ? m_maskWorkImage : m_maskImage;
        VkImageLayout& dstLayout = currentIsMask ? m_maskWorkLayout : m_maskLayout;
        MaskMorphBlurPush push{};
        push.outputSize[0] = m_maskSize.width();
        push.outputSize[1] = m_maskSize.height();
        push.radius = radius;
        push.mode = mode;
        if (!runMaskComputePass(commandBuffer,
                                pipeline,
                                &push,
                                sizeof(push),
                                currentView,
                                dstView,
                                dstImage,
                                dstLayout)) {
            return false;
        }
        currentView = dstView;
        currentIsMask = !currentIsMask;
        return true;
    };

    if (!dispatchPrepare(m_maskRawView,
                         m_maskView,
                         m_maskImage,
                         m_maskLayout,
                         m_maskRawSize,
                         options.invert)) {
        return false;
    }
    currentView = m_maskView;
    currentIsMask = true;
    if (erodeRadius > 0 && !dispatchMorphBlur(m_maskMorphPipeline, erodeRadius, 0)) {
        return false;
    }
    if (dilateRadius > 0 && !dispatchMorphBlur(m_maskMorphPipeline, dilateRadius, 1)) {
        return false;
    }
    if (blurRadius > 0) {
        if (!dispatchMorphBlur(m_maskBlurPipeline, blurRadius, 1) ||
            !dispatchMorphBlur(m_maskBlurPipeline, blurRadius, 0)) {
            return false;
        }
    }
    if (!currentIsMask) {
        if (!dispatchPrepare(currentView,
                             m_maskView,
                             m_maskImage,
                             m_maskLayout,
                             m_maskSize,
                             false)) {
            return false;
        }
    }
    return true;
}

bool VulkanResources::ensureCheckerTextureUploaded(VkCommandBuffer commandBuffer)
{
    if (!m_initialized || !commandBuffer || m_textureUploaded) {
        return m_initialized;
    }

    std::array<unsigned char, static_cast<size_t>(kTextureBytes)> pixels{};
    for (uint32_t y = 0; y < kTextureHeight; ++y) {
        for (uint32_t x = 0; x < kTextureWidth; ++x) {
            const bool even = ((x / 8u) + (y / 8u)) % 2u == 0u;
            const size_t idx = static_cast<size_t>((y * kTextureWidth + x) * 4u);
            pixels[idx + 0] = static_cast<unsigned char>(even ? 230 : 56);
            pixels[idx + 1] = static_cast<unsigned char>(even ? 96 : 180);
            pixels[idx + 2] = static_cast<unsigned char>(even ? 48 : 220);
            pixels[idx + 3] = 255;
        }
    }

    VkDeviceSize stagingOffset = 0;
    if (!writeStagingUpload(pixels.data(), kTextureBytes, &stagingOffset)) {
        return false;
    }

    transitionTextureImage(commandBuffer,
                    m_textureLayout,
                    VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                    VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                    VK_PIPELINE_STAGE_TRANSFER_BIT,
                    0,
                    VK_ACCESS_TRANSFER_WRITE_BIT);
    m_textureLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;

    VkBufferImageCopy region{};
    region.bufferOffset = stagingOffset;
    region.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    region.imageSubresource.layerCount = 1;
    region.imageExtent = {static_cast<uint32_t>(m_textureSize.width()),
                          static_cast<uint32_t>(m_textureSize.height()),
                          1};
    vkCmdCopyBufferToImage(commandBuffer,
                                    m_stagingBuffer,
                                    m_textureImage,
                                    VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                                    1,
                                    &region);

    transitionTextureImage(commandBuffer,
                    m_textureLayout,
                    VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                    VK_PIPELINE_STAGE_TRANSFER_BIT,
                    VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
                    VK_ACCESS_TRANSFER_WRITE_BIT,
                    VK_ACCESS_SHADER_READ_BIT);
    m_textureLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    m_textureUploaded = true;
    setSampledImage(m_textureView, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    return true;
}

bool VulkanResources::uploadImageTexture(VkCommandBuffer commandBuffer, const QImage& image)
{
    if (!m_initialized || !commandBuffer || image.isNull()) {
        return false;
    }
    const QImage rgbaImage = image.convertToFormat(QImage::Format_RGBA8888);
    if (rgbaImage.isNull() || rgbaImage.width() <= 0 || rgbaImage.height() <= 0) {
        return false;
    }
    const QSize size = rgbaImage.size();
    const VkDeviceSize bytes = static_cast<VkDeviceSize>(rgbaImage.sizeInBytes());
    if (!ensureTextureSize(size)) {
        return false;
    }

    VkDeviceSize stagingOffset = 0;
    if (!writeStagingUpload(rgbaImage.constBits(), bytes, &stagingOffset)) {
        return false;
    }

    transitionTextureImage(commandBuffer,
                           m_textureLayout,
                           VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                           VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                           VK_PIPELINE_STAGE_TRANSFER_BIT,
                           0,
                           VK_ACCESS_TRANSFER_WRITE_BIT);
    m_textureLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;

    VkBufferImageCopy region{};
    region.bufferOffset = stagingOffset;
    region.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    region.imageSubresource.layerCount = 1;
    region.imageExtent = {static_cast<uint32_t>(size.width()),
                          static_cast<uint32_t>(size.height()),
                          1};
    vkCmdCopyBufferToImage(commandBuffer,
                                    m_stagingBuffer,
                                    m_textureImage,
                                    VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                                    1,
                                    &region);

    transitionTextureImage(commandBuffer,
                           m_textureLayout,
                           VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                           VK_PIPELINE_STAGE_TRANSFER_BIT,
                           VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
                           VK_ACCESS_TRANSFER_WRITE_BIT,
                           VK_ACCESS_SHADER_READ_BIT);
    m_textureLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    m_textureUploaded = true;
    setSampledImage(m_textureView, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    return true;
}

bool VulkanResources::uploadImageTexture(VkCommandBuffer commandBuffer,
                                         const render_detail::OverlayImage& image)
{
    if (!m_initialized || !commandBuffer || image.isNull()) {
        return false;
    }
    const QSize size(image.width, image.height);
    const VkDeviceSize bytes = static_cast<VkDeviceSize>(image.rgbaPremultiplied.size());
    if (!ensureTextureSize(size)) {
        return false;
    }

    VkDeviceSize stagingOffset = 0;
    if (!writeStagingUpload(image.rgbaPremultiplied.constData(), bytes, &stagingOffset)) {
        return false;
    }

    transitionTextureImage(commandBuffer,
                           m_textureLayout,
                           VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                           VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                           VK_PIPELINE_STAGE_TRANSFER_BIT,
                           0,
                           VK_ACCESS_TRANSFER_WRITE_BIT);
    m_textureLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;

    VkBufferImageCopy region{};
    region.bufferOffset = stagingOffset;
    region.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    region.imageSubresource.layerCount = 1;
    region.imageExtent = {static_cast<uint32_t>(size.width()),
                          static_cast<uint32_t>(size.height()),
                          1};
    vkCmdCopyBufferToImage(commandBuffer,
                                    m_stagingBuffer,
                                    m_textureImage,
                                    VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                                    1,
                                    &region);

    transitionTextureImage(commandBuffer,
                           m_textureLayout,
                           VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                           VK_PIPELINE_STAGE_TRANSFER_BIT,
                           VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
                           VK_ACCESS_TRANSFER_WRITE_BIT,
                           VK_ACCESS_SHADER_READ_BIT);
    m_textureLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    m_textureUploaded = true;
    setSampledImage(m_textureView, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    return true;
}

bool VulkanResources::setSampledImage(VkImageView imageView, VkImageLayout imageLayout)
{
    if (!m_initialized || imageView == VK_NULL_HANDLE || m_descriptorSets[m_descriptorSetIndex] == VK_NULL_HANDLE) {
        return false;
    }
    m_descriptorSetIndex = (m_descriptorSetIndex + 1) % m_descriptorSets.size();
    VkDescriptorImageInfo imageInfoDesc{};
    imageInfoDesc.imageLayout = imageLayout;
    imageInfoDesc.imageView = imageView;
    imageInfoDesc.sampler = m_sampler;
    VkWriteDescriptorSet write{};
    write.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    write.dstSet = m_descriptorSets[m_descriptorSetIndex];
    write.dstBinding = 0;
    write.descriptorCount = 1;
    write.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    write.pImageInfo = &imageInfoDesc;
    vkUpdateDescriptorSets(m_device, 1, &write, 0, nullptr);
    return true;
}

bool VulkanResources::uploadMaskTexture(VkCommandBuffer commandBuffer, const QImage& image)
{
    return uploadMaskTexture(commandBuffer, image, VulkanMaskPreprocessOptions{});
}

bool VulkanResources::uploadMaskTexture(VkCommandBuffer commandBuffer,
                                        const QImage& image,
                                        const VulkanMaskPreprocessOptions& options)
{
    if (!m_initialized || !commandBuffer || image.isNull()) {
        return false;
    }
    const QSize requestedOutputSize =
        options.outputSize.isValid() ? options.outputSize : image.size();
    const qint64 sourceCacheKey = image.cacheKey();
    if (sourceCacheKey != 0 && sourceCacheKey == m_uploadedMaskCacheKey &&
        requestedOutputSize == m_uploadedMaskOutputSize &&
        options.invert == m_uploadedMaskInvert &&
        options.erodeRadius == m_uploadedMaskErodeRadius &&
        options.dilateRadius == m_uploadedMaskDilateRadius &&
        options.blurRadius == m_uploadedMaskBlurRadius &&
        m_maskLayout == VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL) {
        return true;
    }
    const QImage gray = image.convertToFormat(QImage::Format_Grayscale8);
    if (gray.isNull() || gray.width() <= 0 || gray.height() <= 0) {
        return false;
    }
    QImage rgba(gray.size(), QImage::Format_RGBA8888);
    for (int y = 0; y < gray.height(); ++y) {
        const uchar* src = gray.constScanLine(y);
        uchar* dst = rgba.scanLine(y);
        for (int x = 0; x < gray.width(); ++x) {
            const uchar v = src[x];
            dst[x * 4 + 0] = v;
            dst[x * 4 + 1] = v;
            dst[x * 4 + 2] = v;
            dst[x * 4 + 3] = 255;
        }
    }

    const QSize rawSize = rgba.size();
    const QSize outputSize = options.outputSize.isValid() && options.outputSize.width() > 0 && options.outputSize.height() > 0
        ? options.outputSize
        : rawSize;
    if (!ensureRawMaskImage(rawSize) || !ensureMaskImages(outputSize)) {
        return false;
    }

    const VkDeviceSize bytes = static_cast<VkDeviceSize>(rgba.sizeInBytes());
    VkDeviceSize stagingOffset = 0;
    if (!writeStagingUpload(rgba.constBits(), bytes, &stagingOffset)) {
        return false;
    }

    const VkPipelineStageFlags uploadSrcStage =
        m_maskRawLayout == VK_IMAGE_LAYOUT_UNDEFINED
            ? VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT
            : (VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT);
    const VkAccessFlags uploadSrcAccess =
        m_maskRawLayout == VK_IMAGE_LAYOUT_UNDEFINED ? 0 : VK_ACCESS_SHADER_READ_BIT;
    transitionExternalImage(m_funcs,
                            commandBuffer,
                            m_maskRawImage,
                            m_maskRawLayout,
                            VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                            uploadSrcStage,
                            VK_PIPELINE_STAGE_TRANSFER_BIT,
                            uploadSrcAccess,
                            VK_ACCESS_TRANSFER_WRITE_BIT);
    m_maskRawLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    VkBufferImageCopy region{};
    region.bufferOffset = stagingOffset;
    region.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    region.imageSubresource.layerCount = 1;
    region.imageExtent = {static_cast<uint32_t>(rawSize.width()),
                          static_cast<uint32_t>(rawSize.height()),
                          1};
    vkCmdCopyBufferToImage(commandBuffer,
                           m_stagingBuffer,
                           m_maskRawImage,
                           VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                           1,
                           &region);
    transitionExternalImage(m_funcs,
                            commandBuffer,
                            m_maskRawImage,
                            m_maskRawLayout,
                            VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                            VK_PIPELINE_STAGE_TRANSFER_BIT,
                            VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                            VK_ACCESS_TRANSFER_WRITE_BIT,
                            VK_ACCESS_SHADER_READ_BIT);
    m_maskRawLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    if (!preprocessMaskTexture(commandBuffer, options)) {
        return false;
    }
    m_uploadedMaskCacheKey = sourceCacheKey;
    m_uploadedMaskOutputSize = requestedOutputSize;
    m_uploadedMaskInvert = options.invert;
    m_uploadedMaskErodeRadius = options.erodeRadius;
    m_uploadedMaskDilateRadius = options.dilateRadius;
    m_uploadedMaskBlurRadius = options.blurRadius;
    return true;
}

bool VulkanResources::ensureAuxiliaryImagesReadable(VkCommandBuffer commandBuffer)
{
    if (!m_initialized || commandBuffer == VK_NULL_HANDLE) {
        return false;
    }
    auto transitionIfUndefined = [&](VkImage image, VkImageLayout& layout) {
        if (image == VK_NULL_HANDLE || layout != VK_IMAGE_LAYOUT_UNDEFINED) {
            return;
        }
        transitionExternalImage(m_funcs,
                                commandBuffer,
                                image,
                                VK_IMAGE_LAYOUT_UNDEFINED,
                                VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                                VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                                VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
                                0,
                                VK_ACCESS_SHADER_READ_BIT);
        layout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    };
    transitionIfUndefined(m_curveLutImage, m_curveLutLayout);
    transitionIfUndefined(m_maskImage, m_maskLayout);
    transitionIfUndefined(m_maskCurveLutImage, m_maskCurveLutLayout);
    return true;
}

bool VulkanResources::uploadCurveLutImage(VkCommandBuffer commandBuffer,
                                          const QByteArray& rgbaLut,
                                          VkImage image,
                                          VkImageLayout& layout,
                                          QByteArray& cachedBytes)
{
    if (!m_initialized || !commandBuffer || image == VK_NULL_HANDLE ||
        rgbaLut.size() != static_cast<int>(kCurveLutBytes)) {
        return false;
    }
    if (rgbaLut == cachedBytes && layout == VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL) {
        return true;
    }

    VkDeviceSize stagingOffset = 0;
    if (!writeStagingUpload(rgbaLut.constData(), kCurveLutBytes, &stagingOffset)) {
        return false;
    }

    transitionExternalImage(m_funcs,
                            commandBuffer,
                            image,
                            layout,
                            VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                            VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                            VK_PIPELINE_STAGE_TRANSFER_BIT,
                            0,
                            VK_ACCESS_TRANSFER_WRITE_BIT);
    layout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;

    VkBufferImageCopy region{};
    region.bufferOffset = stagingOffset;
    region.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    region.imageSubresource.layerCount = 1;
    region.imageExtent = {kCurveLutWidth, kCurveLutHeight, 1};
    vkCmdCopyBufferToImage(commandBuffer,
                                    m_stagingBuffer,
                                    image,
                                    VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                                    1,
                                    &region);

    transitionExternalImage(m_funcs,
                            commandBuffer,
                            image,
                            layout,
                            VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                            VK_PIPELINE_STAGE_TRANSFER_BIT,
                            VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
                            VK_ACCESS_TRANSFER_WRITE_BIT,
                            VK_ACCESS_SHADER_READ_BIT);
    layout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    cachedBytes = rgbaLut;

    return true;
}

bool VulkanResources::uploadCurveLut(VkCommandBuffer commandBuffer, const QByteArray& rgbaLut)
{
    if (m_curveLutView == VK_NULL_HANDLE) {
        return false;
    }
    return uploadCurveLutImage(commandBuffer, rgbaLut, m_curveLutImage, m_curveLutLayout, m_curveLutBytes);
}

bool VulkanResources::uploadMaskCurveLut(VkCommandBuffer commandBuffer, const QByteArray& rgbaLut)
{
    if (m_maskCurveLutView == VK_NULL_HANDLE) {
        return false;
    }
    return uploadCurveLutImage(commandBuffer, rgbaLut, m_maskCurveLutImage, m_maskCurveLutLayout, m_maskCurveLutBytes);
}
