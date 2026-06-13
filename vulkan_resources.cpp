#include "vulkan_resources.h"

#include <QVulkanFunctions>

#include <array>
#include <cstring>

namespace {
constexpr uint32_t kTextureWidth = 64;
constexpr uint32_t kTextureHeight = 64;
constexpr VkDeviceSize kTextureBytes = static_cast<VkDeviceSize>(kTextureWidth * kTextureHeight * 4);
constexpr uint32_t kCurveLutWidth = 256;
constexpr uint32_t kCurveLutHeight = 1;
constexpr VkDeviceSize kCurveLutBytes = static_cast<VkDeviceSize>(kCurveLutWidth * kCurveLutHeight * 4);
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
    Q_UNUSED(funcs);
    if (!physicalDevice || !device) {
        return false;
    }
    m_physicalDevice = physicalDevice;
    m_device = device;

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

    VkDescriptorSetLayoutBinding bindings[2]{};
    bindings[0].binding = 0;
    bindings[0].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    bindings[0].descriptorCount = 1;
    bindings[0].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
    bindings[1].binding = 1;
    bindings[1].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    bindings[1].descriptorCount = 1;
    bindings[1].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
    VkDescriptorSetLayoutCreateInfo descriptorSetLayoutInfo{};
    descriptorSetLayoutInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    descriptorSetLayoutInfo.bindingCount = 2;
    descriptorSetLayoutInfo.pBindings = bindings;
    if (vkCreateDescriptorSetLayout(m_device, &descriptorSetLayoutInfo, nullptr, &m_descriptorSetLayout) != VK_SUCCESS) {
        destroy();
        return false;
    }

    VkDescriptorPoolSize poolSize{};
    poolSize.type = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    poolSize.descriptorCount = static_cast<uint32_t>(VulkanResources::kDescriptorSetCount * 2);
    VkDescriptorPoolCreateInfo descriptorPoolInfo{};
    descriptorPoolInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    descriptorPoolInfo.poolSizeCount = 1;
    descriptorPoolInfo.pPoolSizes = &poolSize;
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

    if (!createTextureResources()) {
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
    if (m_stagingBuffer != VK_NULL_HANDLE) {
        vkDestroyBuffer(m_device, m_stagingBuffer, nullptr);
        m_stagingBuffer = VK_NULL_HANDLE;
    }
    if (m_stagingMemory != VK_NULL_HANDLE) {
        vkFreeMemory(m_device, m_stagingMemory, nullptr);
        m_stagingMemory = VK_NULL_HANDLE;
    }
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
    m_stagingCapacity = 0;
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
        !createImageAndView(kCurveLutWidth, kCurveLutHeight, &m_curveLutImage, &m_curveLutMemory, &m_curveLutView)) {
        return false;
    }

    if (!ensureStagingCapacity(std::max(kTextureBytes, kCurveLutBytes))) {
        return false;
    }

    VkDescriptorImageInfo imageInfoDesc[2]{};
    imageInfoDesc[0].imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    imageInfoDesc[0].imageView = m_textureView;
    imageInfoDesc[0].sampler = m_sampler;
    imageInfoDesc[1].imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    imageInfoDesc[1].imageView = m_curveLutView;
    imageInfoDesc[1].sampler = m_sampler;
    std::array<VkWriteDescriptorSet, VulkanResources::kDescriptorSetCount * 2> writes{};
    size_t writeIndex = 0;
    for (VkDescriptorSet descriptorSet : m_descriptorSets) {
        if (descriptorSet == VK_NULL_HANDLE) {
            return false;
        }
        for (uint32_t binding = 0; binding < 2; ++binding) {
            writes[writeIndex].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
            writes[writeIndex].dstSet = descriptorSet;
            writes[writeIndex].dstBinding = binding;
            writes[writeIndex].descriptorCount = 1;
            writes[writeIndex].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
            writes[writeIndex].pImageInfo = &imageInfoDesc[binding];
            ++writeIndex;
        }
    }
    vkUpdateDescriptorSets(m_device, static_cast<uint32_t>(writeIndex), writes.data(), 0, nullptr);
    return true;
}

bool VulkanResources::createTextureImage(const QSize& size)
{
    if (!size.isValid() || size.width() <= 0 || size.height() <= 0) {
        return false;
    }
    destroyTextureImage();

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

bool VulkanResources::ensureStagingCapacity(VkDeviceSize bytes)
{
    if (bytes <= 0) {
        return false;
    }
    if (m_stagingBuffer != VK_NULL_HANDLE && m_stagingMemory != VK_NULL_HANDLE && m_stagingCapacity >= bytes) {
        return true;
    }
    if (m_stagingBuffer != VK_NULL_HANDLE) {
        vkDestroyBuffer(m_device, m_stagingBuffer, nullptr);
        m_stagingBuffer = VK_NULL_HANDLE;
    }
    if (m_stagingMemory != VK_NULL_HANDLE) {
        vkFreeMemory(m_device, m_stagingMemory, nullptr);
        m_stagingMemory = VK_NULL_HANDLE;
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
        return false;
    }
    VkMemoryAllocateInfo bufferAlloc{};
    bufferAlloc.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    bufferAlloc.allocationSize = bufferReq.size;
    bufferAlloc.memoryTypeIndex = bufferMemType;
    if (vkAllocateMemory(m_device, &bufferAlloc, nullptr, &m_stagingMemory) != VK_SUCCESS) {
        return false;
    }
    if (vkBindBufferMemory(m_device, m_stagingBuffer, m_stagingMemory, 0) != VK_SUCCESS) {
        return false;
    }
    m_stagingCapacity = bytes;
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
    if (vkMapMemory(m_device, m_stagingMemory, 0, kTextureBytes, 0, &mapped) != VK_SUCCESS || !mapped) {
        return false;
    }
    std::memcpy(mapped, pixels.data(), pixels.size());
    vkUnmapMemory(m_device, m_stagingMemory);

    transitionTextureImage(commandBuffer,
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
    const QImage rgbaImage = image.convertToFormat(QImage::Format_RGBA8888_Premultiplied);
    if (rgbaImage.isNull() || rgbaImage.width() <= 0 || rgbaImage.height() <= 0) {
        return false;
    }
    const QSize size = rgbaImage.size();
    const VkDeviceSize bytes = static_cast<VkDeviceSize>(rgbaImage.sizeInBytes());
    if (!ensureTextureSize(size) || !ensureStagingCapacity(bytes)) {
        return false;
    }

    void* mapped = nullptr;
    if (vkMapMemory(m_device, m_stagingMemory, 0, bytes, 0, &mapped) != VK_SUCCESS || !mapped) {
        return false;
    }
    std::memcpy(mapped, rgbaImage.constBits(), static_cast<size_t>(bytes));
    vkUnmapMemory(m_device, m_stagingMemory);

    transitionTextureImage(commandBuffer,
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
    if (!ensureTextureSize(size) || !ensureStagingCapacity(bytes)) {
        return false;
    }

    void* mapped = nullptr;
    if (vkMapMemory(m_device, m_stagingMemory, 0, bytes, 0, &mapped) != VK_SUCCESS || !mapped) {
        return false;
    }
    std::memcpy(mapped, image.rgbaPremultiplied.constData(), static_cast<size_t>(bytes));
    vkUnmapMemory(m_device, m_stagingMemory);

    transitionTextureImage(commandBuffer,
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

bool VulkanResources::uploadCurveLut(VkCommandBuffer commandBuffer, const QByteArray& rgbaLut)
{
    if (!m_initialized || !commandBuffer || m_curveLutImage == VK_NULL_HANDLE ||
        m_curveLutView == VK_NULL_HANDLE || rgbaLut.size() != static_cast<int>(kCurveLutBytes)) {
        return false;
    }
    if (rgbaLut == m_curveLutBytes && m_curveLutLayout == VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL) {
        return true;
    }

    void* mapped = nullptr;
    if (vkMapMemory(m_device, m_stagingMemory, 0, kCurveLutBytes, 0, &mapped) != VK_SUCCESS || !mapped) {
        return false;
    }
    std::memcpy(mapped, rgbaLut.constData(), static_cast<size_t>(kCurveLutBytes));
    vkUnmapMemory(m_device, m_stagingMemory);

    transitionExternalImage(m_funcs,
                            commandBuffer,
                            m_curveLutImage,
                            m_curveLutLayout,
                            VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                            VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                            VK_PIPELINE_STAGE_TRANSFER_BIT,
                            0,
                            VK_ACCESS_TRANSFER_WRITE_BIT);
    m_curveLutLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;

    VkBufferImageCopy region{};
    region.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    region.imageSubresource.layerCount = 1;
    region.imageExtent = {kCurveLutWidth, kCurveLutHeight, 1};
    vkCmdCopyBufferToImage(commandBuffer,
                                    m_stagingBuffer,
                                    m_curveLutImage,
                                    VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                                    1,
                                    &region);

    transitionExternalImage(m_funcs,
                            commandBuffer,
                            m_curveLutImage,
                            m_curveLutLayout,
                            VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                            VK_PIPELINE_STAGE_TRANSFER_BIT,
                            VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
                            VK_ACCESS_TRANSFER_WRITE_BIT,
                            VK_ACCESS_SHADER_READ_BIT);
    m_curveLutLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    m_curveLutBytes = rgbaLut;

    return true;
}
