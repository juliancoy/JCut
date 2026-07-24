#pragma once

#include "core/image_buffer.h"
#include "core/offscreen_vulkan_frame.h"

#include <memory>
#include <string>
#include <vector>

namespace jcut::vulkan {

// Qt-free ordered Vulkan compositor. Prepared RGBA layers are uploaded into a
// reusable device image, alpha-composited by a compute pipeline, and exposed
// through the neutral borrowed-frame contract for ImGui or export consumers.
class VulkanCompositorCore final {
public:
    VulkanCompositorCore();
    ~VulkanCompositorCore();

    VulkanCompositorCore(
        const VulkanCompositorCore&) = delete;
    VulkanCompositorCore& operator=(
        const VulkanCompositorCore&) = delete;

    bool upload(const core::ImageBuffer& image,
                render_detail::OffscreenVulkanFrame* frameOut,
                std::string* errorOut = nullptr);
    bool compose(
        const std::vector<core::ImageBuffer>& layers,
        render_detail::OffscreenVulkanFrame* frameOut,
        std::string* errorOut = nullptr);
    bool readback(
        core::ImageBuffer* imageOut,
        std::string* errorOut = nullptr);
    void release();

private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace jcut::vulkan
