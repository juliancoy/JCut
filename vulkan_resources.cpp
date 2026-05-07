#include "vulkan_resources.h"

#include <QVulkanFunctions>

#include <array>
#include <cstring>

namespace {
constexpr uint32_t kTextureWidth = 64;
constexpr uint32_t kTextureHeight = 64;
constexpr VkDeviceSize kTextureBytes = static_cast<VkDeviceSize>(kTextureWidth * kTextureHeight * 4);
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
    if (!physicalDevice || !device || !funcs) {
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
    if (m_funcs->vkCreateSampler(m_device, &samplerInfo, nullptr, &m_sampler) != VK_SUCCESS) {
        destroy();
        return false;
    }

    VkDescriptorSetLayoutBinding textureBinding{};
    textureBinding.binding = 0;
    textureBinding.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    textureBinding.descriptorCount = 1;
    textureBinding.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
    VkDescriptorSetLayoutCreateInfo descriptorSetLayoutInfo{};
    descriptorSetLayoutInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    descriptorSetLayoutInfo.bindingCount = 1;
    descriptorSetLayoutInfo.pBindings = &textureBinding;
    if (m_funcs->vkCreateDescriptorSetLayout(m_device, &descriptorSetLayoutInfo, nullptr, &m_descriptorSetLayout) != VK_SUCCESS) {
        destroy();
        return false;
    }

    VkDescriptorPoolSize poolSize{};
    poolSize.type = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    poolSize.descriptorCount = 1;
    VkDescriptorPoolCreateInfo descriptorPoolInfo{};
    descriptorPoolInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    descriptorPoolInfo.poolSizeCount = 1;
    descriptorPoolInfo.pPoolSizes = &poolSize;
    descriptorPoolInfo.maxSets = 1;
    if (m_funcs->vkCreateDescriptorPool(m_device, &descriptorPoolInfo, nullptr, &m_descriptorPool) != VK_SUCCESS) {
        destroy();
        return false;
    }

    VkDescriptorSetAllocateInfo descriptorSetAllocInfo{};
    descriptorSetAllocInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
    descriptorSetAllocInfo.descriptorPool = m_descriptorPool;
    descriptorSetAllocInfo.descriptorSetCount = 1;
    descriptorSetAllocInfo.pSetLayouts = &m_descriptorSetLayout;
    if (m_funcs->vkAllocateDescriptorSets(m_device, &descriptorSetAllocInfo, &m_descriptorSet) != VK_SUCCESS) {
        destroy();
        return false;
    }

    if (!createTextureResources()) {
        destroy();
        return false;
    }
    m_initialized = true;
    return true;
}

void VulkanResources::destroy()
{
    if (!m_device || !m_funcs) {
        m_physicalDevice = VK_NULL_HANDLE;
        m_device = VK_NULL_HANDLE;
        m_funcs = nullptr;
        m_initialized = false;
        return;
    }
    if (m_stagingBuffer != VK_NULL_HANDLE) {
        m_funcs->vkDestroyBuffer(m_device, m_stagingBuffer, nullptr);
        m_stagingBuffer = VK_NULL_HANDLE;
    }
    if (m_stagingMemory != VK_NULL_HANDLE) {
        m_funcs->vkFreeMemory(m_device, m_stagingMemory, nullptr);
        m_stagingMemory = VK_NULL_HANDLE;
    }
    if (m_textureView != VK_NULL_HANDLE) {
        m_funcs->vkDestroyImageView(m_device, m_textureView, nullptr);
        m_textureView = VK_NULL_HANDLE;
    }
    if (m_textureImage != VK_NULL_HANDLE) {
        m_funcs->vkDestroyImage(m_device, m_textureImage, nullptr);
        m_textureImage = VK_NULL_HANDLE;
    }
    if (m_textureMemory != VK_NULL_HANDLE) {
        m_funcs->vkFreeMemory(m_device, m_textureMemory, nullptr);
        m_textureMemory = VK_NULL_HANDLE;
    }
    if (m_descriptorPool != VK_NULL_HANDLE) {
        m_funcs->vkDestroyDescriptorPool(m_device, m_descriptorPool, nullptr);
        m_descriptorPool = VK_NULL_HANDLE;
    }
    if (m_descriptorSetLayout != VK_NULL_HANDLE) {
        m_funcs->vkDestroyDescriptorSetLayout(m_device, m_descriptorSetLayout, nullptr);
        m_descriptorSetLayout = VK_NULL_HANDLE;
    }
    if (m_sampler != VK_NULL_HANDLE) {
        m_funcs->vkDestroySampler(m_device, m_sampler, nullptr);
        m_sampler = VK_NULL_HANDLE;
    }
    m_textureLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    m_textureUploaded = false;
    m_initialized = false;
    m_physicalDevice = VK_NULL_HANDLE;
    m_device = VK_NULL_HANDLE;
    m_funcs = nullptr;
}

bool VulkanResources::createTextureResources()
{
    VkImageCreateInfo imageInfo{};
    imageInfo.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    imageInfo.imageType = VK_IMAGE_TYPE_2D;
    imageInfo.extent.width = kTextureWidth;
    imageInfo.extent.height = kTextureHeight;
    imageInfo.extent.depth = 1;
    imageInfo.mipLevels = 1;
    imageInfo.arrayLayers = 1;
    imageInfo.format = VK_FORMAT_R8G8B8A8_UNORM;
    imageInfo.tiling = VK_IMAGE_TILING_OPTIMAL;
    imageInfo.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    imageInfo.usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
    imageInfo.samples = VK_SAMPLE_COUNT_1_BIT;
    imageInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    if (m_funcs->vkCreateImage(m_device, &imageInfo, nullptr, &m_textureImage) != VK_SUCCESS) {
        return false;
    }

    VkMemoryRequirements imageReq{};
    m_funcs->vkGetImageMemoryRequirements(m_device, m_textureImage, &imageReq);
    const uint32_t imageMemType = findMemoryType(imageReq.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    if (imageMemType == UINT32_MAX) {
        return false;
    }
    VkMemoryAllocateInfo imageAlloc{};
    imageAlloc.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    imageAlloc.allocationSize = imageReq.size;
    imageAlloc.memoryTypeIndex = imageMemType;
    if (m_funcs->vkAllocateMemory(m_device, &imageAlloc, nullptr, &m_textureMemory) != VK_SUCCESS) {
        return false;
    }
    if (m_funcs->vkBindImageMemory(m_device, m_textureImage, m_textureMemory, 0) != VK_SUCCESS) {
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
    if (m_funcs->vkCreateImageView(m_device, &viewInfo, nullptr, &m_textureView) != VK_SUCCESS) {
        return false;
    }

    VkBufferCreateInfo bufferInfo{};
    bufferInfo.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    bufferInfo.size = kTextureBytes;
    bufferInfo.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
    bufferInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    if (m_funcs->vkCreateBuffer(m_device, &bufferInfo, nullptr, &m_stagingBuffer) != VK_SUCCESS) {
        return false;
    }
    VkMemoryRequirements bufferReq{};
    m_funcs->vkGetBufferMemoryRequirements(m_device, m_stagingBuffer, &bufferReq);
    const uint32_t bufferMemType = findMemoryType(
        bufferReq.memoryTypeBits,
        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    if (bufferMemType == UINT32_MAX) {
        return false;
    }
    VkMemoryAllocateInfo bufferAlloc{};
    bufferAlloc.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    bufferAlloc.allocationSize = bufferReq.size;
    bufferAlloc.memoryTypeIndex = bufferMemType;
    if (m_funcs->vkAllocateMemory(m_device, &bufferAlloc, nullptr, &m_stagingMemory) != VK_SUCCESS) {
        return false;
    }
    if (m_funcs->vkBindBufferMemory(m_device, m_stagingBuffer, m_stagingMemory, 0) != VK_SUCCESS) {
        return false;
    }

    VkDescriptorImageInfo imageInfoDesc{};
    imageInfoDesc.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    imageInfoDesc.imageView = m_textureView;
    imageInfoDesc.sampler = m_sampler;
    VkWriteDescriptorSet write{};
    write.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    write.dstSet = m_descriptorSet;
    write.dstBinding = 0;
    write.descriptorCount = 1;
    write.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    write.pImageInfo = &imageInfoDesc;
    m_funcs->vkUpdateDescriptorSets(m_device, 1, &write, 0, nullptr);
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

void VulkanResources::transitionImage(VkCommandBuffer cb,
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
    m_funcs->vkCmdPipelineBarrier(cb, srcStage, dstStage, 0, 0, nullptr, 0, nullptr, 1, &barrier);
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

    void* mapped = nullptr;
    if (m_funcs->vkMapMemory(m_device, m_stagingMemory, 0, kTextureBytes, 0, &mapped) != VK_SUCCESS || !mapped) {
        return false;
    }
    std::memcpy(mapped, pixels.data(), pixels.size());
    m_funcs->vkUnmapMemory(m_device, m_stagingMemory);

    transitionImage(commandBuffer,
                    m_textureLayout,
                    VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                    VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                    VK_PIPELINE_STAGE_TRANSFER_BIT,
                    0,
                    VK_ACCESS_TRANSFER_WRITE_BIT);
    m_textureLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;

    VkBufferImageCopy region{};
    region.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    region.imageSubresource.layerCount = 1;
    region.imageExtent = {kTextureWidth, kTextureHeight, 1};
    m_funcs->vkCmdCopyBufferToImage(commandBuffer,
                                    m_stagingBuffer,
                                    m_textureImage,
                                    VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                                    1,
                                    &region);

    transitionImage(commandBuffer,
                    m_textureLayout,
                    VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                    VK_PIPELINE_STAGE_TRANSFER_BIT,
                    VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
                    VK_ACCESS_TRANSFER_WRITE_BIT,
                    VK_ACCESS_SHADER_READ_BIT);
    m_textureLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    m_textureUploaded = true;
    return true;
}
