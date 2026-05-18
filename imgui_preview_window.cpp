#include "imgui_preview_window.h"

#include "external/imgui/imgui.h"
#include "external/imgui/backends/imgui_impl_glfw.h"
#include "external/imgui/backends/imgui_impl_vulkan.h"
#include "render_internal.h"
#include "vulkan_detector_frame_handoff.h"
#include "vulkan_zero_copy_face_detector.h"

#define GLFW_INCLUDE_NONE
#define GLFW_INCLUDE_VULKAN
#include <GLFW/glfw3.h>

#include <QByteArray>

#include <algorithm>
#include <array>
#include <cstring>
#include <vector>

namespace {

constexpr uint32_t kMinImageCount = 2;

ImVec2 fitImageIntoRegion(const QSize& imageSize, const ImVec2& avail)
{
    if (!imageSize.isValid() || avail.x <= 1.0f || avail.y <= 1.0f) {
        return ImVec2(1.0f, 1.0f);
    }

    const float imageW = static_cast<float>(imageSize.width());
    const float imageH = static_cast<float>(imageSize.height());
    const float scale = std::min(avail.x / imageW, avail.y / imageH);
    return ImVec2(std::max(1.0f, imageW * scale), std::max(1.0f, imageH * scale));
}

VkClearValue makeClearValue()
{
    VkClearValue clearValue{};
    clearValue.color.float32[0] = 0.03f;
    clearValue.color.float32[1] = 0.04f;
    clearValue.color.float32[2] = 0.06f;
    clearValue.color.float32[3] = 1.0f;
    return clearValue;
}

bool hasExtension(const std::vector<VkExtensionProperties>& properties, const char* name)
{
    return std::any_of(properties.begin(), properties.end(), [&](const VkExtensionProperties& ext) {
        return std::strcmp(ext.extensionName, name) == 0;
    });
}

} // namespace

struct ImGuiPreviewWindow::Impl {
    GLFWwindow* window = nullptr;
    QString failureReason;
    QString statusText;
    QString windowTitle;
    int64_t lastPresentedSourceFrame = -1;
    bool glfwInitialized = false;
    bool imguiContextInitialized = false;
    bool imguiBackendsInitialized = false;
    bool updatePending = false;
    bool swapchainRebuild = false;
    uint32_t minImageCount = kMinImageCount;

    VkInstance instance = VK_NULL_HANDLE;
    VkSurfaceKHR surface = VK_NULL_HANDLE;
    VkPhysicalDevice physicalDevice = VK_NULL_HANDLE;
    VkDevice device = VK_NULL_HANDLE;
    uint32_t queueFamily = UINT32_MAX;
    VkQueue queue = VK_NULL_HANDLE;
    VkDescriptorPool descriptorPool = VK_NULL_HANDLE;
    VkPipelineCache pipelineCache = VK_NULL_HANDLE;
    VkSampler sampler = VK_NULL_HANDLE;
    ImGui_ImplVulkanH_Window windowData{};

    jcut::vulkan_detector::VulkanDetectorFrameHandoff frameHandoff;
    VkImageView boundImageView = VK_NULL_HANDLE;
    VkImageLayout boundImageLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    QSize boundImageSize;
    VkDescriptorSet textureSet = VK_NULL_HANDLE;
};

ImGuiPreviewWindow::ImGuiPreviewWindow()
    : m_impl(std::make_unique<Impl>())
{
}

ImGuiPreviewWindow::~ImGuiPreviewWindow()
{
    shutdown();
}

namespace {

void checkVkResult(VkResult err, QString* errorOut)
{
    if (err == VK_SUCCESS) {
        return;
    }
    if (errorOut && errorOut->isEmpty()) {
        *errorOut = QStringLiteral("Vulkan call failed with VkResult=%1.").arg(static_cast<int>(err));
    }
}

bool queueFamilySupportsPresent(VkPhysicalDevice physicalDevice,
                                uint32_t queueFamilyIndex,
                                VkSurfaceKHR surface)
{
    VkBool32 supported = VK_FALSE;
    if (vkGetPhysicalDeviceSurfaceSupportKHR(physicalDevice, queueFamilyIndex, surface, &supported) != VK_SUCCESS) {
        return false;
    }
    return supported == VK_TRUE;
}

bool selectQueueFamilyForPresent(VkPhysicalDevice physicalDevice,
                                 VkSurfaceKHR surface,
                                 uint32_t* queueFamilyOut)
{
    if (!queueFamilyOut) {
        return false;
    }
    *queueFamilyOut = UINT32_MAX;
    uint32_t familyCount = 0;
    vkGetPhysicalDeviceQueueFamilyProperties(physicalDevice, &familyCount, nullptr);
    if (familyCount == 0) {
        return false;
    }
    std::vector<VkQueueFamilyProperties> families(familyCount);
    vkGetPhysicalDeviceQueueFamilyProperties(physicalDevice, &familyCount, families.data());
    for (uint32_t i = 0; i < familyCount; ++i) {
        if ((families[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) &&
            queueFamilySupportsPresent(physicalDevice, i, surface)) {
            *queueFamilyOut = i;
            return true;
        }
    }
    return false;
}

bool createInstance(ImGuiPreviewWindow::Impl* impl, QString* errorOut)
{
    if (!impl) {
        return false;
    }
    uint32_t glfwExtensionCount = 0;
    const char** glfwExtensions = glfwGetRequiredInstanceExtensions(&glfwExtensionCount);
    if (!glfwExtensions || glfwExtensionCount == 0) {
        if (errorOut) {
            *errorOut = QStringLiteral("GLFW did not expose required Vulkan instance extensions.");
        }
        return false;
    }

    uint32_t propertyCount = 0;
    if (vkEnumerateInstanceExtensionProperties(nullptr, &propertyCount, nullptr) != VK_SUCCESS) {
        if (errorOut) {
            *errorOut = QStringLiteral("Failed to enumerate Vulkan instance extensions.");
        }
        return false;
    }
    std::vector<VkExtensionProperties> properties(propertyCount);
    if (propertyCount > 0 &&
        vkEnumerateInstanceExtensionProperties(nullptr, &propertyCount, properties.data()) != VK_SUCCESS) {
        if (errorOut) {
            *errorOut = QStringLiteral("Failed to load Vulkan instance extension list.");
        }
        return false;
    }

    std::vector<const char*> extensions(glfwExtensions, glfwExtensions + glfwExtensionCount);
    if (hasExtension(properties, VK_KHR_GET_PHYSICAL_DEVICE_PROPERTIES_2_EXTENSION_NAME)) {
        extensions.push_back(VK_KHR_GET_PHYSICAL_DEVICE_PROPERTIES_2_EXTENSION_NAME);
    }
#ifdef VK_KHR_PORTABILITY_ENUMERATION_EXTENSION_NAME
    VkInstanceCreateFlags flags = 0;
    if (hasExtension(properties, VK_KHR_PORTABILITY_ENUMERATION_EXTENSION_NAME)) {
        extensions.push_back(VK_KHR_PORTABILITY_ENUMERATION_EXTENSION_NAME);
        flags |= VK_INSTANCE_CREATE_ENUMERATE_PORTABILITY_BIT_KHR;
    }
#else
    VkInstanceCreateFlags flags = 0;
#endif

    VkApplicationInfo appInfo{};
    appInfo.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
    appInfo.pApplicationName = "jcut-imgui-preview";
    appInfo.apiVersion = VK_API_VERSION_1_1;

    VkInstanceCreateInfo createInfo{};
    createInfo.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
    createInfo.flags = flags;
    createInfo.pApplicationInfo = &appInfo;
    createInfo.enabledExtensionCount = static_cast<uint32_t>(extensions.size());
    createInfo.ppEnabledExtensionNames = extensions.data();
    const VkResult err = vkCreateInstance(&createInfo, nullptr, &impl->instance);
    if (err != VK_SUCCESS) {
        checkVkResult(err, errorOut);
        return false;
    }
    return true;
}

bool selectPhysicalDevice(ImGuiPreviewWindow::Impl* impl,
                          VkPhysicalDevice preferred,
                          QString* errorOut)
{
    if (!impl || impl->instance == VK_NULL_HANDLE || impl->surface == VK_NULL_HANDLE) {
        return false;
    }

    auto selectIfSupported = [&](VkPhysicalDevice device) -> bool {
        uint32_t queueFamily = UINT32_MAX;
        if (!selectQueueFamilyForPresent(device, impl->surface, &queueFamily)) {
            return false;
        }
        impl->physicalDevice = device;
        impl->queueFamily = queueFamily;
        return true;
    };

    if (preferred != VK_NULL_HANDLE && selectIfSupported(preferred)) {
        return true;
    }

    uint32_t deviceCount = 0;
    if (vkEnumeratePhysicalDevices(impl->instance, &deviceCount, nullptr) != VK_SUCCESS || deviceCount == 0) {
        if (errorOut) {
            *errorOut = QStringLiteral("No Vulkan physical devices found for preview window.");
        }
        return false;
    }
    std::vector<VkPhysicalDevice> devices(deviceCount);
    if (vkEnumeratePhysicalDevices(impl->instance, &deviceCount, devices.data()) != VK_SUCCESS) {
        if (errorOut) {
            *errorOut = QStringLiteral("Failed to enumerate Vulkan physical devices for preview window.");
        }
        return false;
    }
    for (VkPhysicalDevice device : devices) {
        if (selectIfSupported(device)) {
            return true;
        }
    }
    if (errorOut) {
        *errorOut = QStringLiteral("No Vulkan graphics/present queue supports the Dear ImGui preview surface.");
    }
    return false;
}

bool createDevice(ImGuiPreviewWindow::Impl* impl, QString* errorOut)
{
    if (!impl || impl->physicalDevice == VK_NULL_HANDLE || impl->queueFamily == UINT32_MAX) {
        return false;
    }

    uint32_t extensionCount = 0;
    vkEnumerateDeviceExtensionProperties(impl->physicalDevice, nullptr, &extensionCount, nullptr);
    std::vector<VkExtensionProperties> properties(extensionCount);
    if (extensionCount > 0) {
        vkEnumerateDeviceExtensionProperties(impl->physicalDevice,
                                             nullptr,
                                             &extensionCount,
                                             properties.data());
    }

    auto tryEnable = [&](const char* name, bool required) -> bool {
        if (hasExtension(properties, name)) {
            return true;
        }
        if (required && errorOut) {
            *errorOut = QStringLiteral("Required Vulkan device extension is unavailable: %1").arg(name);
        }
        return false;
    };

    std::vector<const char*> extensions;
    if (!tryEnable(VK_KHR_SWAPCHAIN_EXTENSION_NAME, true)) {
        return false;
    }
    extensions.push_back(VK_KHR_SWAPCHAIN_EXTENSION_NAME);
    if (tryEnable(VK_KHR_EXTERNAL_MEMORY_EXTENSION_NAME, false)) {
        extensions.push_back(VK_KHR_EXTERNAL_MEMORY_EXTENSION_NAME);
    }
#ifdef Q_OS_LINUX
    if (tryEnable(VK_KHR_EXTERNAL_MEMORY_FD_EXTENSION_NAME, false)) {
        extensions.push_back(VK_KHR_EXTERNAL_MEMORY_FD_EXTENSION_NAME);
    }
#endif
    if (tryEnable(VK_KHR_BIND_MEMORY_2_EXTENSION_NAME, false)) {
        extensions.push_back(VK_KHR_BIND_MEMORY_2_EXTENSION_NAME);
    }
    if (tryEnable(VK_KHR_GET_MEMORY_REQUIREMENTS_2_EXTENSION_NAME, false)) {
        extensions.push_back(VK_KHR_GET_MEMORY_REQUIREMENTS_2_EXTENSION_NAME);
    }
    if (tryEnable(VK_KHR_MAINTENANCE1_EXTENSION_NAME, false)) {
        extensions.push_back(VK_KHR_MAINTENANCE1_EXTENSION_NAME);
    }
    if (tryEnable(VK_KHR_MAINTENANCE3_EXTENSION_NAME, false)) {
        extensions.push_back(VK_KHR_MAINTENANCE3_EXTENSION_NAME);
    }
#ifdef VK_KHR_PORTABILITY_SUBSET_EXTENSION_NAME
    if (tryEnable(VK_KHR_PORTABILITY_SUBSET_EXTENSION_NAME, false)) {
        extensions.push_back(VK_KHR_PORTABILITY_SUBSET_EXTENSION_NAME);
    }
#endif

    const float queuePriority = 1.0f;
    VkDeviceQueueCreateInfo queueInfo{};
    queueInfo.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
    queueInfo.queueFamilyIndex = impl->queueFamily;
    queueInfo.queueCount = 1;
    queueInfo.pQueuePriorities = &queuePriority;

    VkDeviceCreateInfo createInfo{};
    createInfo.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
    createInfo.queueCreateInfoCount = 1;
    createInfo.pQueueCreateInfos = &queueInfo;
    createInfo.enabledExtensionCount = static_cast<uint32_t>(extensions.size());
    createInfo.ppEnabledExtensionNames = extensions.data();
    const VkResult err = vkCreateDevice(impl->physicalDevice, &createInfo, nullptr, &impl->device);
    if (err != VK_SUCCESS) {
        checkVkResult(err, errorOut);
        return false;
    }
    vkGetDeviceQueue(impl->device, impl->queueFamily, 0, &impl->queue);
    return true;
}

bool createDescriptorPool(ImGuiPreviewWindow::Impl* impl, QString* errorOut)
{
    const std::array<VkDescriptorPoolSize, 2> poolSizes{{
        {VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, IMGUI_IMPL_VULKAN_MINIMUM_SAMPLED_IMAGE_POOL_SIZE + 16},
        {VK_DESCRIPTOR_TYPE_SAMPLER, IMGUI_IMPL_VULKAN_MINIMUM_SAMPLER_POOL_SIZE + 4},
    }};

    VkDescriptorPoolCreateInfo poolInfo{};
    poolInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    poolInfo.flags = VK_DESCRIPTOR_POOL_CREATE_FREE_DESCRIPTOR_SET_BIT;
    poolInfo.poolSizeCount = static_cast<uint32_t>(poolSizes.size());
    poolInfo.pPoolSizes = poolSizes.data();
    for (const auto& poolSize : poolSizes) {
        poolInfo.maxSets += poolSize.descriptorCount;
    }
    const VkResult err = vkCreateDescriptorPool(impl->device, &poolInfo, nullptr, &impl->descriptorPool);
    if (err != VK_SUCCESS) {
        checkVkResult(err, errorOut);
        return false;
    }
    return true;
}

bool createSampler(ImGuiPreviewWindow::Impl* impl, QString* errorOut)
{
    VkSamplerCreateInfo samplerInfo{};
    samplerInfo.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
    samplerInfo.magFilter = VK_FILTER_LINEAR;
    samplerInfo.minFilter = VK_FILTER_LINEAR;
    samplerInfo.mipmapMode = VK_SAMPLER_MIPMAP_MODE_LINEAR;
    samplerInfo.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    samplerInfo.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    samplerInfo.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    samplerInfo.maxLod = 1.0f;
    const VkResult err = vkCreateSampler(impl->device, &samplerInfo, nullptr, &impl->sampler);
    if (err != VK_SUCCESS) {
        checkVkResult(err, errorOut);
        return false;
    }
    return true;
}

bool setupWindowData(ImGuiPreviewWindow::Impl* impl, int width, int height, QString* errorOut)
{
    impl->windowData.Surface = impl->surface;
    const VkFormat requestSurfaceImageFormat[] = {
        VK_FORMAT_B8G8R8A8_UNORM,
        VK_FORMAT_R8G8B8A8_UNORM,
        VK_FORMAT_B8G8R8_UNORM,
        VK_FORMAT_R8G8B8_UNORM,
    };
    impl->windowData.SurfaceFormat =
        ImGui_ImplVulkanH_SelectSurfaceFormat(impl->physicalDevice,
                                              impl->surface,
                                              requestSurfaceImageFormat,
                                              IM_ARRAYSIZE(requestSurfaceImageFormat),
                                              VK_COLORSPACE_SRGB_NONLINEAR_KHR);
    const VkPresentModeKHR presentModes[] = {VK_PRESENT_MODE_FIFO_KHR};
    impl->windowData.PresentMode =
        ImGui_ImplVulkanH_SelectPresentMode(impl->physicalDevice,
                                            impl->surface,
                                            presentModes,
                                            IM_ARRAYSIZE(presentModes));
    ImGui_ImplVulkanH_CreateOrResizeWindow(impl->instance,
                                           impl->physicalDevice,
                                           impl->device,
                                           &impl->windowData,
                                           impl->queueFamily,
                                           nullptr,
                                           width,
                                           height,
                                           impl->minImageCount,
                                           0);
    impl->windowData.ClearValue = makeClearValue();
    if (impl->windowData.RenderPass == VK_NULL_HANDLE) {
        if (errorOut) {
            *errorOut = QStringLiteral("Failed to initialize Vulkan swapchain/render pass for Dear ImGui preview.");
        }
        return false;
    }
    return true;
}

void cleanupVulkan(ImGuiPreviewWindow::Impl* impl)
{
    if (!impl) {
        return;
    }
    if (impl->device != VK_NULL_HANDLE) {
        vkDeviceWaitIdle(impl->device);
    }
    if (impl->textureSet != VK_NULL_HANDLE) {
        ImGui_ImplVulkan_RemoveTexture(impl->textureSet);
        impl->textureSet = VK_NULL_HANDLE;
    }
    impl->frameHandoff.release();
    if (impl->imguiBackendsInitialized) {
        ImGui_ImplVulkan_Shutdown();
        impl->imguiBackendsInitialized = false;
    }
    if (impl->device != VK_NULL_HANDLE && impl->windowData.Surface != VK_NULL_HANDLE) {
        ImGui_ImplVulkanH_DestroyWindow(impl->instance, impl->device, &impl->windowData, nullptr);
    }
    impl->windowData = ImGui_ImplVulkanH_Window{};
    if (impl->sampler != VK_NULL_HANDLE) {
        vkDestroySampler(impl->device, impl->sampler, nullptr);
        impl->sampler = VK_NULL_HANDLE;
    }
    if (impl->descriptorPool != VK_NULL_HANDLE) {
        vkDestroyDescriptorPool(impl->device, impl->descriptorPool, nullptr);
        impl->descriptorPool = VK_NULL_HANDLE;
    }
    if (impl->pipelineCache != VK_NULL_HANDLE) {
        vkDestroyPipelineCache(impl->device, impl->pipelineCache, nullptr);
        impl->pipelineCache = VK_NULL_HANDLE;
    }
    if (impl->device != VK_NULL_HANDLE) {
        vkDestroyDevice(impl->device, nullptr);
        impl->device = VK_NULL_HANDLE;
    }
    if (impl->surface != VK_NULL_HANDLE) {
        vkDestroySurfaceKHR(impl->instance, impl->surface, nullptr);
        impl->surface = VK_NULL_HANDLE;
    }
    if (impl->instance != VK_NULL_HANDLE) {
        vkDestroyInstance(impl->instance, nullptr);
        impl->instance = VK_NULL_HANDLE;
    }
    impl->physicalDevice = VK_NULL_HANDLE;
    impl->queueFamily = UINT32_MAX;
    impl->queue = VK_NULL_HANDLE;
    impl->boundImageView = VK_NULL_HANDLE;
    impl->boundImageLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    impl->boundImageSize = QSize();
}

bool ensureVulkanReady(ImGuiPreviewWindow::Impl* impl,
                       VkPhysicalDevice preferredPhysicalDevice,
                       QString* errorOut)
{
    if (!impl) {
        return false;
    }
    if (impl->imguiBackendsInitialized) {
        return true;
    }
    if (impl->window == nullptr) {
        if (errorOut) {
            *errorOut = QStringLiteral("Dear ImGui preview GLFW window is unavailable.");
        }
        return false;
    }
    if (!glfwVulkanSupported()) {
        if (errorOut) {
            *errorOut = QStringLiteral("GLFW reports that Vulkan is not supported on this system.");
        }
        return false;
    }
    if (!createInstance(impl, errorOut)) {
        return false;
    }
    if (glfwCreateWindowSurface(impl->instance, impl->window, nullptr, &impl->surface) != VK_SUCCESS) {
        if (errorOut) {
            *errorOut = QStringLiteral("Failed to create Vulkan surface for Dear ImGui preview.");
        }
        cleanupVulkan(impl);
        return false;
    }
    if (!selectPhysicalDevice(impl, preferredPhysicalDevice, errorOut) ||
        !createDevice(impl, errorOut) ||
        !createDescriptorPool(impl, errorOut) ||
        !createSampler(impl, errorOut)) {
        cleanupVulkan(impl);
        return false;
    }

    int fbWidth = 0;
    int fbHeight = 0;
    glfwGetFramebufferSize(impl->window, &fbWidth, &fbHeight);
    if (!setupWindowData(impl, std::max(1, fbWidth), std::max(1, fbHeight), errorOut)) {
        cleanupVulkan(impl);
        return false;
    }

    ImGui_ImplVulkan_InitInfo initInfo{};
    initInfo.Instance = impl->instance;
    initInfo.PhysicalDevice = impl->physicalDevice;
    initInfo.Device = impl->device;
    initInfo.QueueFamily = impl->queueFamily;
    initInfo.Queue = impl->queue;
    initInfo.PipelineCache = impl->pipelineCache;
    initInfo.DescriptorPool = impl->descriptorPool;
    initInfo.MinImageCount = impl->minImageCount;
    initInfo.ImageCount = impl->windowData.ImageCount;
    initInfo.Allocator = nullptr;
    initInfo.PipelineInfoMain.RenderPass = impl->windowData.RenderPass;
    initInfo.PipelineInfoMain.Subpass = 0;
    initInfo.PipelineInfoMain.MSAASamples = VK_SAMPLE_COUNT_1_BIT;
    initInfo.CheckVkResultFn = [](VkResult err) {
        if (err != VK_SUCCESS && err != VK_SUBOPTIMAL_KHR) {
            qWarning("Dear ImGui Vulkan preview backend error: %d", static_cast<int>(err));
        }
    };
    if (!ImGui_ImplVulkan_Init(&initInfo)) {
        if (errorOut) {
            *errorOut = QStringLiteral("Failed to initialize Dear ImGui Vulkan backend.");
        }
        cleanupVulkan(impl);
        return false;
    }
    impl->imguiBackendsInitialized = true;
    if (!impl->frameHandoff.initialize({impl->physicalDevice, impl->device, impl->queue, impl->queueFamily},
                                       errorOut)) {
        cleanupVulkan(impl);
        return false;
    }
    return true;
}

void rebuildSwapchainIfNeeded(ImGuiPreviewWindow::Impl* impl)
{
    if (!impl || !impl->imguiBackendsInitialized) {
        return;
    }
    int fbWidth = 0;
    int fbHeight = 0;
    glfwGetFramebufferSize(impl->window, &fbWidth, &fbHeight);
    if (fbWidth <= 0 || fbHeight <= 0) {
        return;
    }
    if (impl->swapchainRebuild ||
        impl->windowData.Width != static_cast<uint32_t>(fbWidth) ||
        impl->windowData.Height != static_cast<uint32_t>(fbHeight)) {
        ImGui_ImplVulkan_SetMinImageCount(impl->minImageCount);
        ImGui_ImplVulkanH_CreateOrResizeWindow(impl->instance,
                                               impl->physicalDevice,
                                               impl->device,
                                               &impl->windowData,
                                               impl->queueFamily,
                                               nullptr,
                                               fbWidth,
                                               fbHeight,
                                               impl->minImageCount,
                                               0);
        impl->windowData.ClearValue = makeClearValue();
        impl->windowData.FrameIndex = 0;
        impl->swapchainRebuild = false;
    }
}

void frameRender(ImGuiPreviewWindow::Impl* impl, ImDrawData* drawData)
{
    ImGui_ImplVulkanH_Window* wd = &impl->windowData;
    VkSemaphore imageAcquiredSemaphore = wd->FrameSemaphores[wd->SemaphoreIndex].ImageAcquiredSemaphore;
    VkSemaphore renderCompleteSemaphore = wd->FrameSemaphores[wd->SemaphoreIndex].RenderCompleteSemaphore;
    VkResult err = vkAcquireNextImageKHR(impl->device,
                                         wd->Swapchain,
                                         UINT64_MAX,
                                         imageAcquiredSemaphore,
                                         VK_NULL_HANDLE,
                                         &wd->FrameIndex);
    if (err == VK_ERROR_OUT_OF_DATE_KHR || err == VK_SUBOPTIMAL_KHR) {
        impl->swapchainRebuild = true;
    }
    if (err == VK_ERROR_OUT_OF_DATE_KHR) {
        return;
    }

    ImGui_ImplVulkanH_Frame* fd = &wd->Frames[wd->FrameIndex];
    vkWaitForFences(impl->device, 1, &fd->Fence, VK_TRUE, UINT64_MAX);
    vkResetFences(impl->device, 1, &fd->Fence);
    vkResetCommandPool(impl->device, fd->CommandPool, 0);

    VkCommandBufferBeginInfo beginInfo{};
    beginInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    beginInfo.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    vkBeginCommandBuffer(fd->CommandBuffer, &beginInfo);

    VkRenderPassBeginInfo renderPassInfo{};
    renderPassInfo.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
    renderPassInfo.renderPass = wd->RenderPass;
    renderPassInfo.framebuffer = fd->Framebuffer;
    renderPassInfo.renderArea.extent.width = wd->Width;
    renderPassInfo.renderArea.extent.height = wd->Height;
    renderPassInfo.clearValueCount = 1;
    renderPassInfo.pClearValues = &wd->ClearValue;
    vkCmdBeginRenderPass(fd->CommandBuffer, &renderPassInfo, VK_SUBPASS_CONTENTS_INLINE);
    ImGui_ImplVulkan_RenderDrawData(drawData, fd->CommandBuffer);
    vkCmdEndRenderPass(fd->CommandBuffer);
    vkEndCommandBuffer(fd->CommandBuffer);

    const VkPipelineStageFlags waitStage = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    VkSubmitInfo submitInfo{};
    submitInfo.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    submitInfo.waitSemaphoreCount = 1;
    submitInfo.pWaitSemaphores = &imageAcquiredSemaphore;
    submitInfo.pWaitDstStageMask = &waitStage;
    submitInfo.commandBufferCount = 1;
    submitInfo.pCommandBuffers = &fd->CommandBuffer;
    submitInfo.signalSemaphoreCount = 1;
    submitInfo.pSignalSemaphores = &renderCompleteSemaphore;
    vkQueueSubmit(impl->queue, 1, &submitInfo, fd->Fence);
}

void framePresent(ImGuiPreviewWindow::Impl* impl)
{
    if (!impl || impl->swapchainRebuild) {
        return;
    }
    ImGui_ImplVulkanH_Window* wd = &impl->windowData;
    VkSemaphore renderCompleteSemaphore = wd->FrameSemaphores[wd->SemaphoreIndex].RenderCompleteSemaphore;
    VkPresentInfoKHR presentInfo{};
    presentInfo.sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR;
    presentInfo.waitSemaphoreCount = 1;
    presentInfo.pWaitSemaphores = &renderCompleteSemaphore;
    presentInfo.swapchainCount = 1;
    presentInfo.pSwapchains = &wd->Swapchain;
    presentInfo.pImageIndices = &wd->FrameIndex;
    VkResult err = vkQueuePresentKHR(impl->queue, &presentInfo);
    if (err == VK_ERROR_OUT_OF_DATE_KHR || err == VK_SUBOPTIMAL_KHR) {
        impl->swapchainRebuild = true;
    }
    wd->SemaphoreIndex = (wd->SemaphoreIndex + 1) % wd->SemaphoreCount;
}

} // namespace

bool ImGuiPreviewWindow::initialize(const QString& title, const QSize& initialSize)
{
    shutdown();

    if (!glfwInit()) {
        markFailure(QStringLiteral("glfwInit() failed for Dear ImGui preview."));
        return false;
    }
    m_impl->glfwInitialized = true;

    glfwWindowHint(GLFW_CLIENT_API, GLFW_NO_API);
    const QByteArray titleUtf8 = title.toUtf8();
    m_impl->window = glfwCreateWindow(std::max(320, initialSize.width()),
                                      std::max(240, initialSize.height()),
                                      titleUtf8.constData(),
                                      nullptr,
                                      nullptr);
    if (!m_impl->window) {
        markFailure(QStringLiteral("glfwCreateWindow() failed for Dear ImGui preview."));
        return false;
    }

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGui::StyleColorsDark();
    ImGuiIO& io = ImGui::GetIO();
    io.IniFilename = nullptr;
    io.LogFilename = nullptr;

    ImGuiStyle& style = ImGui::GetStyle();
    style.WindowRounding = 0.0f;
    style.FrameRounding = 4.0f;
    style.Colors[ImGuiCol_WindowBg] = ImVec4(0.04f, 0.05f, 0.07f, 1.0f);

    if (!ImGui_ImplGlfw_InitForVulkan(m_impl->window, true)) {
        markFailure(QStringLiteral("ImGui GLFW Vulkan backend initialization failed."));
        return false;
    }

    m_impl->imguiContextInitialized = true;
    m_impl->windowTitle = title;
    m_impl->failureReason.clear();
    return true;
}

bool ImGuiPreviewWindow::isActive() const
{
    return m_impl->window &&
           m_impl->failureReason.trimmed().isEmpty() &&
           !glfwWindowShouldClose(m_impl->window);
}

bool ImGuiPreviewWindow::hasFailed() const
{
    return !m_impl->failureReason.trimmed().isEmpty();
}

bool ImGuiPreviewWindow::updatePending() const
{
    return m_impl->updatePending;
}

bool ImGuiPreviewWindow::isVisible() const
{
    return m_impl->window && glfwGetWindowAttrib(m_impl->window, GLFW_VISIBLE) != 0;
}

int64_t ImGuiPreviewWindow::lastPresentedSourceFrame() const
{
    return m_impl->lastPresentedSourceFrame;
}

QString ImGuiPreviewWindow::failureReason() const
{
    return m_impl->failureReason;
}

void ImGuiPreviewWindow::setStatusText(const QString& text)
{
    m_impl->statusText = text;
}

void ImGuiPreviewWindow::setWindowTitle(const QString& title)
{
    m_impl->windowTitle = title;
    if (!m_impl->window) {
        return;
    }
    const QByteArray titleUtf8 = title.toUtf8();
    glfwSetWindowTitle(m_impl->window, titleUtf8.constData());
}

void ImGuiPreviewWindow::pumpEvents()
{
    if (!m_impl->window) {
        return;
    }
    glfwPollEvents();
    if (glfwWindowShouldClose(m_impl->window) && m_impl->failureReason.trimmed().isEmpty()) {
        markFailure(QStringLiteral("Dear ImGui preview window was closed."));
    }
}

bool ImGuiPreviewWindow::presentFrame(const render_detail::OffscreenVulkanFrame& frame,
                                      int64_t frameNumber,
                                      const QVector<QRectF>& boxes,
                                      const QRectF& roiRect,
                                      int detectionCount)
{
    m_impl->updatePending = true;
    pumpEvents();
    if (!isActive()) {
        m_impl->updatePending = false;
        return false;
    }
    if (!frame.valid || frame.imageView == VK_NULL_HANDLE || !frame.size.isValid()) {
        m_impl->updatePending = false;
        return false;
    }

    QString error;
    if (!ensureVulkanReady(m_impl.get(), frame.physicalDevice, &error)) {
        markFailure(error);
        m_impl->updatePending = false;
        return false;
    }
    rebuildSwapchainIfNeeded(m_impl.get());

    if (!m_impl->frameHandoff.importOffscreenFrame(frame, &error)) {
        markFailure(error);
        m_impl->updatePending = false;
        return false;
    }

    const jcut::vulkan_detector::VulkanExternalImage external = m_impl->frameHandoff.externalImage();
    if (external.imageView == VK_NULL_HANDLE || !external.size.isValid()) {
        markFailure(QStringLiteral("Dear ImGui preview received an invalid imported Vulkan image."));
        m_impl->updatePending = false;
        return false;
    }

    if (m_impl->textureSet == VK_NULL_HANDLE ||
        m_impl->boundImageView != external.imageView ||
        m_impl->boundImageLayout != external.imageLayout) {
        if (m_impl->textureSet != VK_NULL_HANDLE) {
            ImGui_ImplVulkan_RemoveTexture(m_impl->textureSet);
            m_impl->textureSet = VK_NULL_HANDLE;
        }
        m_impl->textureSet = ImGui_ImplVulkan_AddTexture(m_impl->sampler,
                                                         external.imageView,
                                                         external.imageLayout);
        if (m_impl->textureSet == VK_NULL_HANDLE) {
            markFailure(QStringLiteral("Failed to bind imported Vulkan image into Dear ImGui."));
            m_impl->updatePending = false;
            return false;
        }
        m_impl->boundImageView = external.imageView;
        m_impl->boundImageLayout = external.imageLayout;
    }
    m_impl->boundImageSize = external.size;

    ImGui_ImplVulkan_NewFrame();
    ImGui_ImplGlfw_NewFrame();
    ImGui::NewFrame();

    const ImGuiViewport* viewport = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(viewport->Pos);
    ImGui::SetNextWindowSize(viewport->Size);
    const ImGuiWindowFlags flags =
        ImGuiWindowFlags_NoDecoration |
        ImGuiWindowFlags_NoMove |
        ImGuiWindowFlags_NoSavedSettings |
        ImGuiWindowFlags_NoBringToFrontOnFocus;

    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(12.0f, 12.0f));
    ImGui::Begin("JCut FaceStream Preview", nullptr, flags);
    ImGui::TextUnformatted(m_impl->windowTitle.toUtf8().constData());
    if (!m_impl->statusText.trimmed().isEmpty()) {
        ImGui::Separator();
        ImGui::TextWrapped("%s", m_impl->statusText.toUtf8().constData());
    }
    ImGui::Separator();

    const ImVec2 avail = ImGui::GetContentRegionAvail();
    const ImVec2 fitted = fitImageIntoRegion(external.size, avail);
    const float offsetX = std::max(0.0f, (avail.x - fitted.x) * 0.5f);
    if (offsetX > 0.0f) {
        ImGui::SetCursorPosX(ImGui::GetCursorPosX() + offsetX);
    }
    const ImVec2 imagePos = ImGui::GetCursorScreenPos();
    ImGui::Image(m_impl->textureSet, fitted, ImVec2(0.0f, 1.0f), ImVec2(1.0f, 0.0f));

    ImDrawList* drawList = ImGui::GetWindowDrawList();
    const float scaleX = fitted.x / static_cast<float>(std::max(1, external.size.width()));
    const float scaleY = fitted.y / static_cast<float>(std::max(1, external.size.height()));
    const ImU32 roiColor = IM_COL32(255, 170, 51, 255);
    const ImU32 detColor = IM_COL32(102, 255, 102, 255);
    if (roiRect.isValid() && !roiRect.isEmpty()) {
        const ImVec2 roiMin(imagePos.x + static_cast<float>(roiRect.left()) * scaleX,
                            imagePos.y + static_cast<float>(roiRect.top()) * scaleY);
        const ImVec2 roiMax(imagePos.x + static_cast<float>(roiRect.right()) * scaleX,
                            imagePos.y + static_cast<float>(roiRect.bottom()) * scaleY);
        drawList->AddRect(roiMin, roiMax, roiColor, 0.0f, 0, 2.0f);
    }
    for (const QRectF& box : boxes) {
        if (!box.isValid() || box.isEmpty()) {
            continue;
        }
        const ImVec2 boxMin(imagePos.x + static_cast<float>(box.left()) * scaleX,
                            imagePos.y + static_cast<float>(box.top()) * scaleY);
        const ImVec2 boxMax(imagePos.x + static_cast<float>(box.right()) * scaleX,
                            imagePos.y + static_cast<float>(box.bottom()) * scaleY);
        drawList->AddRect(boxMin, boxMax, detColor, 0.0f, 0, 2.0f);
    }

    const ImVec2 panelMin(imagePos.x + 8.0f, imagePos.y + 8.0f);
    const ImVec2 panelMax(panelMin.x + 220.0f, panelMin.y + 34.0f);
    drawList->AddRectFilled(panelMin, panelMax, IM_COL32(0, 0, 0, 160), 6.0f);
    drawList->AddText(ImVec2(panelMin.x + 10.0f, panelMin.y + 9.0f),
                      IM_COL32(255, 255, 255, 255),
                      QStringLiteral("Detections: %1").arg(detectionCount).toUtf8().constData());

    ImGui::End();
    ImGui::PopStyleVar(2);

    ImGui::Render();
    ImDrawData* drawData = ImGui::GetDrawData();
    const bool minimized = drawData->DisplaySize.x <= 0.0f || drawData->DisplaySize.y <= 0.0f;
    if (!minimized) {
        frameRender(m_impl.get(), drawData);
        framePresent(m_impl.get());
    }

    m_impl->lastPresentedSourceFrame = frameNumber;
    m_impl->updatePending = false;
    return true;
}

void ImGuiPreviewWindow::shutdown()
{
    if (!m_impl) {
        return;
    }

    cleanupVulkan(m_impl.get());

    if (m_impl->imguiContextInitialized) {
        ImGui_ImplGlfw_Shutdown();
        ImGui::DestroyContext();
        m_impl->imguiContextInitialized = false;
    }
    if (m_impl->window) {
        glfwDestroyWindow(m_impl->window);
        m_impl->window = nullptr;
    }
    if (m_impl->glfwInitialized) {
        glfwTerminate();
        m_impl->glfwInitialized = false;
    }
    m_impl->updatePending = false;
    m_impl->lastPresentedSourceFrame = -1;
}

void ImGuiPreviewWindow::markFailure(const QString& reason)
{
    m_impl->failureReason = reason.trimmed().isEmpty()
        ? QStringLiteral("Unknown Dear ImGui preview failure.")
        : reason.trimmed();
}
