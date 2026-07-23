#include "imgui_vulkan_frame_importer.h"

#include "vulkan_external_frame_import_core.h"

namespace jcut::imgui {

class VulkanFrameImporter::Impl {
public:
    vulkan_import::VulkanExternalFrameImportCore importer;
};

VulkanFrameImporter::VulkanFrameImporter()
    : m_impl(std::make_unique<Impl>())
{
}

VulkanFrameImporter::~VulkanFrameImporter() = default;

bool VulkanFrameImporter::initialize(VkPhysicalDevice physicalDevice,
                                     VkDevice device,
                                     VkQueue queue,
                                     std::uint32_t queueFamilyIndex,
                                     std::string* errorMessage)
{
    return m_impl->importer.initialize(
        vulkan_import::DeviceContext{
            physicalDevice,
            device,
            queue,
            queueFamilyIndex},
        errorMessage);
}

bool VulkanFrameImporter::importFrame(const render_detail::OffscreenVulkanFrame& frame,
                                      std::string* errorMessage)
{
    return m_impl->importer.importFrame(frame, errorMessage);
}

VulkanExternalImage VulkanFrameImporter::externalImage() const
{
    const vulkan_import::ExternalImage imported =
        m_impl->importer.externalImage();
    VulkanExternalImage image;
    image.imageView = imported.imageView;
    image.imageLayout = imported.imageLayout;
    image.size = imported.size;
    image.sourceIsSrgb = imported.sourceIsSrgb;
    image.sourceX = imported.sourceX;
    image.sourceY = imported.sourceY;
    image.sourceWidth = imported.sourceWidth;
    image.sourceHeight = imported.sourceHeight;
    return image;
}

void VulkanFrameImporter::release()
{
    m_impl->importer.release();
}

} // namespace jcut::imgui
