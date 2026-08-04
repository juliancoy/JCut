#pragma once

#include "geometry.h"

#include <atomic>
#include <cstdint>
#include <memory>

#include <vulkan/vulkan.h>

namespace render_detail {

struct OffscreenVulkanFrameConsumptionState {
    std::atomic<std::uint64_t> completedGeneration{0};
};

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
    VkDeviceSize memoryAllocationSize = 0;
    // Logical producer lifetime. Vulkan handles may be numerically reused
    // after a chunk renderer is destroyed, so handles alone cannot identify
    // an imported allocation or its semaphore pair.
    std::uint64_t producerSessionId = 0;
    // Monotonic publication order within producerSessionId. Unlike generation,
    // which is local to one reusable slot, this orders frames across all slots.
    std::uint64_t presentationSequence = 0;
    std::uint64_t generation = 0;
    // Same-process, host-visible mailbox acknowledgment. A producer may reuse
    // a preview slot only after the consumer has queued its consumed semaphore
    // signal and published this generation. Busy slots are dropped instead of
    // ever placing an unresolved preview wait onto the export queue.
    std::shared_ptr<OffscreenVulkanFrameConsumptionState> consumptionState;
    jcut::core::SizeI size;
    bool queueSupportsCompute = false;
    bool valid = false;
};

} // namespace render_detail
