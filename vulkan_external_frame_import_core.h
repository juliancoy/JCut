#pragma once

#include "core/geometry.h"
#include "core/offscreen_vulkan_frame.h"

#include <cstdint>
#include <memory>
#include <string>

#include <vulkan/vulkan.h>

namespace jcut::vulkan_import {

struct DeviceContext {
    VkPhysicalDevice physicalDevice = VK_NULL_HANDLE;
    VkDevice device = VK_NULL_HANDLE;
    VkQueue queue = VK_NULL_HANDLE;
    std::uint32_t queueFamilyIndex = UINT32_MAX;
};

struct ExternalImage {
    VkImage image = VK_NULL_HANDLE;
    VkImageView imageView = VK_NULL_HANDLE;
    VkImageLayout imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    VkFormat imageFormat = VK_FORMAT_UNDEFINED;
    core::SizeI size;
    bool sourceIsSrgb = false;
    float sourceX = 0.0f;
    float sourceY = 0.0f;
    float sourceWidth = 1.0f;
    float sourceHeight = 1.0f;
};

struct ResourceStats {
    std::uint64_t imageMemoryAllocations = 0;
    std::uint64_t imageMemoryFrees = 0;
    std::uint64_t importedMemoryAllocations = 0;
    std::uint64_t importedMemoryFrees = 0;
};

// Qt-free external-memory importer shared by the Dear ImGui facade and the
// legacy Vulkan handoff. The producer owns OffscreenVulkanFrame resources;
// this class imports their opaque FD into a local image and copies into an
// independently owned sampled image before exposing it to the caller.
class VulkanExternalFrameImportCore final {
public:
    VulkanExternalFrameImportCore();
    ~VulkanExternalFrameImportCore();

    VulkanExternalFrameImportCore(const VulkanExternalFrameImportCore&) = delete;
    VulkanExternalFrameImportCore& operator=(const VulkanExternalFrameImportCore&) = delete;

    bool initialize(const DeviceContext& context,
                    std::string* errorMessage = nullptr);
    void release();

    [[nodiscard]] bool isInitialized() const;
    bool importFrame(const render_detail::OffscreenVulkanFrame& frame,
                     std::string* errorMessage = nullptr);
    bool recordFrameCopy(VkCommandBuffer commandBuffer,
                         const render_detail::OffscreenVulkanFrame& frame,
                         std::string* errorMessage = nullptr);
    bool finishPendingCopy(double* copyMs = nullptr,
                           std::string* errorMessage = nullptr);

    [[nodiscard]] ExternalImage externalImage() const;
    [[nodiscard]] ResourceStats resourceStats() const;
    void resetResourceStats();

private:
    class Impl;
    std::unique_ptr<Impl> m_impl;
};

} // namespace jcut::vulkan_import
