#include "../imgui_vulkan_frame_importer.h"

#include <iostream>
#include <string>

int main()
{
    jcut::imgui::VulkanFrameImporter importer;
    const jcut::imgui::VulkanExternalImage initialImage =
        importer.externalImage();
    if (initialImage.imageView != VK_NULL_HANDLE ||
        initialImage.size.valid()) {
        std::cerr << "uninitialized importer exposed an image\n";
        return 1;
    }

    render_detail::OffscreenVulkanFrame frame;
    std::string error;
    if (importer.importFrame(frame, &error) ||
        error != "Vulkan handoff is not initialized.") {
        std::cerr << "uninitialized import did not fail safely\n";
        return 2;
    }

    error.clear();
    if (importer.initialize(
            VK_NULL_HANDLE,
            VK_NULL_HANDLE,
            VK_NULL_HANDLE,
            UINT32_MAX,
            &error) ||
        error != "Invalid Vulkan device context for frame handoff.") {
        std::cerr << "invalid device context did not fail safely\n";
        return 3;
    }

    importer.release();
    importer.release();
    return 0;
}
