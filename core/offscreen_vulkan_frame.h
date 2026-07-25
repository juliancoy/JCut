#pragma once

#include "geometry.h"

#include <cstdint>

#include <vulkan/vulkan.h>

namespace render_detail {

// Neutral borrowed-frame contract shared by render backends and UI adapters.
// Resource ownership remains with the producing renderer; consumers must finish
// importing/copying the frame before the producer reuses its backing resources.
struct OffscreenVulkanFrame {
    VkPhysicalDevice physicalDevice = VK_NULL_HANDLE;
    VkDevice device = VK_NULL_HANDLE;
    VkQueue queue = VK_NULL_HANDLE;
    std::uint32_t queueFamilyIndex = UINT32_MAX;
    VkImage image = VK_NULL_HANDLE;
    VkImageView imageView = VK_NULL_HANDLE;
    VkDeviceMemory imageMemory = VK_NULL_HANDLE;
    VkImageLayout imageLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    VkFormat imageFormat = VK_FORMAT_UNDEFINED;
    // Opaque-FD binary semaphores for a GPU-only producer/consumer handoff.
    // The consumer owns each non-negative FD and must import or close it.
    int readySemaphoreFd = -1;
    int consumedSemaphoreFd = -1;
    std::uint32_t bufferIndex = 0;
    std::uint32_t memoryTypeIndex = UINT32_MAX;
    std::uint64_t generation = 0;
    jcut::core::SizeI size;
    bool queueSupportsCompute = false;
    bool valid = false;
};

} // namespace render_detail
