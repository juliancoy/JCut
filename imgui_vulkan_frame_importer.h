#pragma once

#include "core/geometry.h"
#include "core/offscreen_vulkan_frame.h"

#include <cstdint>
#include <memory>
#include <string>

#include <vulkan/vulkan.h>

namespace jcut::imgui {

struct VulkanExternalImage {
    VkImageView imageView = VK_NULL_HANDLE;
    VkImageLayout imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    core::SizeI size;
    bool sourceIsSrgb = false;
    float sourceX = 0.0f;
    float sourceY = 0.0f;
    float sourceWidth = 1.0f;
    float sourceHeight = 1.0f;
};

class VulkanFrameImporter final {
public:
    VulkanFrameImporter();
    ~VulkanFrameImporter();

    VulkanFrameImporter(const VulkanFrameImporter&) = delete;
    VulkanFrameImporter& operator=(const VulkanFrameImporter&) = delete;

    bool initialize(VkPhysicalDevice physicalDevice,
                    VkDevice device,
                    VkQueue queue,
                    std::uint32_t queueFamilyIndex,
                    std::string* errorMessage = nullptr);
    bool importFrame(const render_detail::OffscreenVulkanFrame& frame,
                     std::string* errorMessage = nullptr);
    [[nodiscard]] VulkanExternalImage externalImage() const;
    void release();

private:
    class Impl;
    std::unique_ptr<Impl> m_impl;
};

} // namespace jcut::imgui
