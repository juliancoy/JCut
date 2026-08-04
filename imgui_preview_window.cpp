#include "imgui_preview_window.h"

#include "external/imgui/imgui.h"
#include "external/imgui/backends/imgui_impl_glfw.h"
#include "external/imgui/backends/imgui_impl_vulkan.h"
#include "render_internal.h"
#include "timeline_fps.h"
#include "vulkan_external_frame_import_core.h"
#include "vulkan_detector_frame_handoff.h"
#include "vulkan_zero_copy_face_detector.h"

#define GLFW_INCLUDE_NONE
#define GLFW_INCLUDE_VULKAN
#include <GLFW/glfw3.h>

#include <algorithm>
#include <atomic>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <cstdio>
#include <sstream>
#include <utility>
#include <vector>

namespace {

constexpr uint32_t kMinImageCount = 2;
constexpr std::uint64_t kMonitorSwapchainWaitTimeoutNs = 16'000'000ull;

template <typename Fn>
class ScopeExit {
public:
    explicit ScopeExit(Fn fn)
        : m_fn(std::move(fn))
    {
    }

    ScopeExit(const ScopeExit&) = delete;
    ScopeExit& operator=(const ScopeExit&) = delete;

    ~ScopeExit()
    {
        if (m_active) {
            m_fn();
        }
    }

    void release() { m_active = false; }

private:
    Fn m_fn;
    bool m_active = true;
};

bool validNonEmptyRect(const jcut::core::RectF& rect)
{
    return rect.valid() && rect.width > 0.0 && rect.height > 0.0;
}

double rectRight(const jcut::core::RectF& rect)
{
    return rect.x + rect.width;
}

double rectBottom(const jcut::core::RectF& rect)
{
    return rect.y + rect.height;
}

ImVec2 fitImageIntoRegion(jcut::core::SizeI imageSize, const ImVec2& avail)
{
    if (!imageSize.valid() || avail.x <= 1.0f || avail.y <= 1.0f) {
        return ImVec2(1.0f, 1.0f);
    }

    const float imageW = static_cast<float>(imageSize.width);
    const float imageH = static_cast<float>(imageSize.height);
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

int clampFrameToRange(int frame, int minFrame, int maxFrame)
{
    if (maxFrame < minFrame) {
        return minFrame;
    }
    return std::clamp(frame, minFrame, maxFrame);
}

bool hasExtension(const std::vector<VkExtensionProperties>& properties, const char* name)
{
    return std::any_of(properties.begin(), properties.end(), [&](const VkExtensionProperties& ext) {
        return std::strcmp(ext.extensionName, name) == 0;
    });
}

void drawAnimatedProgressBar(const char* id,
                             float fraction,
                             const std::string& title,
                             const std::string& detail,
                             float markerFraction = -1.0f)
{
    const ImVec2 cursor = ImGui::GetCursorScreenPos();
    const float width = ImGui::GetContentRegionAvail().x;
    const float height = 44.0f;
    const ImVec2 size(width, height);
    const ImVec2 rectMin = cursor;
    const ImVec2 rectMax(cursor.x + width, cursor.y + height);
    ImGui::InvisibleButton(id, size);

    ImDrawList* drawList = ImGui::GetWindowDrawList();
    const float rounding = 10.0f;
    const float clampedFraction = std::clamp(fraction, 0.0f, 1.0f);
    const float rectWidth = width;
    const float fillWidth = rectWidth * clampedFraction;

    drawList->AddRectFilled(rectMin, rectMax, IM_COL32(16, 22, 30, 235), rounding);
    drawList->AddRect(rectMin, rectMax, IM_COL32(68, 90, 116, 255), rounding, 0, 1.0f);

    if (fillWidth > 0.0f) {
        const ImVec2 fillMax(rectMin.x + fillWidth, rectMax.y);
        drawList->AddRectFilledMultiColor(rectMin,
                                          fillMax,
                                          IM_COL32(244, 181, 63, 255),
                                          IM_COL32(255, 141, 48, 255),
                                          IM_COL32(255, 98, 72, 255),
                                          IM_COL32(245, 165, 70, 255));

        const float t = static_cast<float>(ImGui::GetTime());
        const float sweepWidth = std::max(18.0f, rectWidth * 0.14f);
        const float sweepTravel = std::max(0.0f, fillWidth + sweepWidth);
        const float sweepRight = rectMin.x + std::fmod(t * 140.0f, std::max(1.0f, sweepTravel));
        const float sweepLeft = std::max(rectMin.x, sweepRight - sweepWidth);
        const float sweepClampedRight = std::min(rectMin.x + fillWidth, sweepRight);
        if (sweepClampedRight > sweepLeft) {
            drawList->AddRectFilledMultiColor(ImVec2(sweepLeft, rectMin.y),
                                              ImVec2(sweepClampedRight, rectMax.y),
                                              IM_COL32(255, 255, 255, 0),
                                              IM_COL32(255, 255, 255, 92),
                                              IM_COL32(255, 255, 255, 38),
                                              IM_COL32(255, 255, 255, 0));
        }
    }

    if (markerFraction >= 0.0f) {
        const float clampedMarker = std::clamp(markerFraction, 0.0f, 1.0f);
        const float markerX = rectMin.x + rectWidth * clampedMarker;
        drawList->AddLine(ImVec2(markerX, rectMin.y + 3.0f),
                          ImVec2(markerX, rectMax.y - 3.0f),
                          IM_COL32(186, 230, 255, 220),
                          2.0f);
    }

    const ImVec2 titlePos(rectMin.x + 12.0f, rectMin.y + 7.0f);
    const ImVec2 detailPos(rectMin.x + 12.0f, rectMin.y + 24.0f);
    drawList->AddText(titlePos, IM_COL32(250, 252, 255, 255), title.c_str());
    drawList->AddText(detailPos, IM_COL32(188, 201, 219, 255), detail.c_str());
}

template <typename... Args>
std::string formatString(const char* fmt, Args... args)
{
    const int size = std::snprintf(nullptr, 0, fmt, args...);
    if (size <= 0) {
        return std::string();
    }
    std::string output(static_cast<std::size_t>(size), '\0');
    std::snprintf(output.data(), static_cast<std::size_t>(size) + 1, fmt, args...);
    return output;
}

std::string trimCopy(const std::string& value)
{
    const auto begin = value.find_first_not_of(" \t\r\n");
    if (begin == std::string::npos) {
        return std::string();
    }
    const auto end = value.find_last_not_of(" \t\r\n");
    return value.substr(begin, end - begin + 1);
}

std::string formatDurationMs(int64_t value)
{
    if (value < 0) {
        return "--";
    }
    const int64_t totalSeconds = value / 1000;
    const int64_t seconds = totalSeconds % 60;
    const int64_t minutes = (totalSeconds / 60) % 60;
    const int64_t hours = totalSeconds / 3600;
    if (hours > 0) {
        return formatString("%lld:%02lld:%02lld",
                            static_cast<long long>(hours),
                            static_cast<long long>(minutes),
                            static_cast<long long>(seconds));
    }
    return formatString("%lld:%02lld",
                        static_cast<long long>(minutes),
                        static_cast<long long>(seconds));
}

} // namespace

struct ImGuiPreviewWindow::Impl {
    GLFWwindow* window = nullptr;
    std::string failureReason;
    std::string statusText;
    std::string windowTitle;
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
    PFN_vkImportSemaphoreFdKHR importSemaphoreFd = nullptr;

    jcut::vulkan_detector::VulkanDetectorFrameHandoff frameHandoff;
    struct RenderMonitorSlot {
        std::unique_ptr<jcut::vulkan_import::VulkanExternalFrameImportCore> importer;
        VkSemaphore ready = VK_NULL_HANDLE;
        VkSemaphore consumed = VK_NULL_HANDLE;
        std::uint64_t producerSessionId = 0;
        bool initialized = false;
    };
    std::array<RenderMonitorSlot, 3> renderMonitorSlots;
    std::uint64_t renderMonitorProducerSessionId = 0;
    std::uint64_t lastAcceptedRenderMonitorSequence = 0;
    VkImageView boundImageView = VK_NULL_HANDLE;
    VkImageLayout boundImageLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    jcut::core::SizeI boundImageSize;
    VkDescriptorSet textureSet = VK_NULL_HANDLE;

    bool showDetections = true;
    bool showTracks = true;
    bool showRoi = true;
    bool showTrackLabels = true;
    bool showConfirmedTracks = true;
    bool showTentativeTracks = true;
    bool showLostTracks = true;
    float detectionLineThickness = 1.5f;
    float trackLineThickness = 2.5f;
    float overlayOpacity = 1.0f;

    bool processingPausedRequested = false;
    bool processingPaused = false;
    bool followLatest = true;
    bool historyPlaying = false;
    float historyPlaybackSpeed = 1.0f;
    int minTimelineFrame = 0;
    int maxTimelineFrame = 0;
    int latestProcessedFrame = 0;
    int requestedPreviewFrame = 0;
    double lastUiTickSec = 0.0;
    double previewFrameAccumulator = 0.0;
    bool redrawRequested = true;
    bool renderMonitorCancelRequested = false;
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

void checkVkResult(VkResult err, std::string* errorOut)
{
    if (err == VK_SUCCESS) {
        return;
    }
    if (errorOut && errorOut->empty()) {
        *errorOut = formatString("Vulkan call failed with VkResult=%d.", static_cast<int>(err));
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

bool createInstance(ImGuiPreviewWindow::Impl* impl, std::string* errorOut)
{
    if (!impl) {
        return false;
    }
    uint32_t glfwExtensionCount = 0;
    const char** glfwExtensions = glfwGetRequiredInstanceExtensions(&glfwExtensionCount);
    if (!glfwExtensions || glfwExtensionCount == 0) {
        if (errorOut) {
            *errorOut = "GLFW did not expose required Vulkan instance extensions.";
        }
        return false;
    }

    uint32_t propertyCount = 0;
    if (vkEnumerateInstanceExtensionProperties(nullptr, &propertyCount, nullptr) != VK_SUCCESS) {
        if (errorOut) {
            *errorOut = "Failed to enumerate Vulkan instance extensions.";
        }
        return false;
    }
    std::vector<VkExtensionProperties> properties(propertyCount);
    if (propertyCount > 0 &&
        vkEnumerateInstanceExtensionProperties(nullptr, &propertyCount, properties.data()) != VK_SUCCESS) {
        if (errorOut) {
            *errorOut = "Failed to load Vulkan instance extension list.";
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
                          std::string* errorOut)
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
            *errorOut = "No Vulkan physical devices found for preview window.";
        }
        return false;
    }
    std::vector<VkPhysicalDevice> devices(deviceCount);
    if (vkEnumeratePhysicalDevices(impl->instance, &deviceCount, devices.data()) != VK_SUCCESS) {
        if (errorOut) {
            *errorOut = "Failed to enumerate Vulkan physical devices for preview window.";
        }
        return false;
    }
    for (VkPhysicalDevice device : devices) {
        if (selectIfSupported(device)) {
            return true;
        }
    }
    if (errorOut) {
        *errorOut = "No Vulkan graphics/present queue supports the Dear ImGui preview surface.";
    }
    return false;
}

bool createDevice(ImGuiPreviewWindow::Impl* impl, std::string* errorOut)
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
            *errorOut = formatString("Required Vulkan device extension is unavailable: %s", name);
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
#ifdef __linux__
    if (tryEnable(VK_KHR_EXTERNAL_MEMORY_FD_EXTENSION_NAME, false)) {
        extensions.push_back(VK_KHR_EXTERNAL_MEMORY_FD_EXTENSION_NAME);
    }
    if (tryEnable(VK_KHR_EXTERNAL_SEMAPHORE_EXTENSION_NAME, false)) {
        extensions.push_back(VK_KHR_EXTERNAL_SEMAPHORE_EXTENSION_NAME);
    }
    if (tryEnable(VK_KHR_EXTERNAL_SEMAPHORE_FD_EXTENSION_NAME, false)) {
        extensions.push_back(VK_KHR_EXTERNAL_SEMAPHORE_FD_EXTENSION_NAME);
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
    impl->importSemaphoreFd =
        reinterpret_cast<PFN_vkImportSemaphoreFdKHR>(
            vkGetDeviceProcAddr(impl->device, "vkImportSemaphoreFdKHR"));
    return true;
}

bool createDescriptorPool(ImGuiPreviewWindow::Impl* impl, std::string* errorOut)
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

bool createSampler(ImGuiPreviewWindow::Impl* impl, std::string* errorOut)
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

bool setupWindowData(ImGuiPreviewWindow::Impl* impl, int width, int height, std::string* errorOut)
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
            *errorOut = "Failed to initialize Vulkan swapchain/render pass for Dear ImGui preview.";
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
    for (ImGuiPreviewWindow::Impl::RenderMonitorSlot& slot :
         impl->renderMonitorSlots) {
        if (slot.importer) {
            slot.importer->release();
            slot.importer.reset();
        }
        if (slot.ready != VK_NULL_HANDLE) {
            vkDestroySemaphore(impl->device, slot.ready, nullptr);
        }
        if (slot.consumed != VK_NULL_HANDLE) {
            vkDestroySemaphore(impl->device, slot.consumed, nullptr);
        }
        slot = {};
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
    impl->importSemaphoreFd = nullptr;
    impl->boundImageView = VK_NULL_HANDLE;
    impl->boundImageLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    impl->boundImageSize = {};
    impl->renderMonitorProducerSessionId = 0;
    impl->lastAcceptedRenderMonitorSequence = 0;
}

bool releaseRenderMonitorExportFrame(
    ImGuiPreviewWindow::Impl* impl,
    const render_detail::OffscreenVulkanFrame& frame,
    std::string* errorOut)
{
    if (!impl || frame.bufferIndex >= impl->renderMonitorSlots.size()) {
        if (errorOut) {
            *errorOut = "Dear ImGui render monitor cannot release an invalid export frame slot.";
        }
        return false;
    }
    ImGuiPreviewWindow::Impl::RenderMonitorSlot& slot =
        impl->renderMonitorSlots[frame.bufferIndex];
    if (!slot.initialized || slot.consumed == VK_NULL_HANDLE) {
        if (errorOut) {
            *errorOut = "Dear ImGui render monitor cannot release an uninitialized export frame slot.";
        }
        return false;
    }

    VkSubmitInfo signal{};
    signal.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    signal.signalSemaphoreCount = 1;
    signal.pSignalSemaphores = &slot.consumed;
    if (vkQueueSubmit(impl->queue, 1, &signal, VK_NULL_HANDLE) != VK_SUCCESS) {
        if (errorOut) {
            *errorOut = "Dear ImGui render monitor failed to release the exported frame.";
        }
        return false;
    }
    if (frame.consumptionState) {
        frame.consumptionState->completedGeneration.store(
            frame.generation, std::memory_order_release);
    }
    return true;
}

bool acquireRenderMonitorExportFrame(
    ImGuiPreviewWindow::Impl* impl,
    const render_detail::OffscreenVulkanFrame& frame,
    jcut::vulkan_import::ExternalImage* imageOut,
    std::string* errorOut)
{
    if (imageOut) {
        *imageOut = {};
    }
    if (!impl || impl->device == VK_NULL_HANDLE ||
        impl->queue == VK_NULL_HANDLE ||
        impl->queueFamily == UINT32_MAX) {
        if (errorOut) {
            *errorOut = "Dear ImGui render monitor Vulkan device is unavailable.";
        }
        return false;
    }
    if (!frame.valid || frame.bufferIndex >= impl->renderMonitorSlots.size()) {
        if (errorOut) {
            *errorOut = "Dear ImGui render monitor received an invalid export frame slot.";
        }
        return false;
    }
    if (!impl->importSemaphoreFd) {
        if (errorOut) {
            *errorOut =
                "Dear ImGui render monitor Vulkan device cannot import external semaphore FDs.";
        }
        return false;
    }

    ImGuiPreviewWindow::Impl::RenderMonitorSlot& slot =
        impl->renderMonitorSlots[frame.bufferIndex];
    const auto resetSlot = [&]() {
        if (slot.importer) {
            slot.importer->release();
            slot.importer.reset();
        }
        if (slot.ready != VK_NULL_HANDLE) {
            vkDestroySemaphore(impl->device, slot.ready, nullptr);
        }
        if (slot.consumed != VK_NULL_HANDLE) {
            vkDestroySemaphore(impl->device, slot.consumed, nullptr);
        }
        slot = {};
    };
    if (slot.initialized &&
        slot.producerSessionId != frame.producerSessionId) {
        vkQueueWaitIdle(impl->queue);
        resetSlot();
    }

    if (!slot.initialized) {
        if (frame.readySemaphoreFd < 0 || frame.consumedSemaphoreFd < 0) {
            if (errorOut) {
                *errorOut =
                    "Dear ImGui render monitor received a new export slot without semaphore FDs.";
            }
            return false;
        }
        slot.importer =
            std::make_unique<jcut::vulkan_import::VulkanExternalFrameImportCore>();
        const jcut::vulkan_import::DeviceContext context{
            impl->physicalDevice,
            impl->device,
            impl->queue,
            impl->queueFamily};
        if (!slot.importer->initialize(context, errorOut)) {
            close(frame.readySemaphoreFd);
            close(frame.consumedSemaphoreFd);
            resetSlot();
            return false;
        }
        VkSemaphoreCreateInfo semaphoreInfo{};
        semaphoreInfo.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;
        if (vkCreateSemaphore(impl->device, &semaphoreInfo, nullptr,
                              &slot.ready) != VK_SUCCESS ||
            vkCreateSemaphore(impl->device, &semaphoreInfo, nullptr,
                              &slot.consumed) != VK_SUCCESS) {
            close(frame.readySemaphoreFd);
            close(frame.consumedSemaphoreFd);
            resetSlot();
            if (errorOut) {
                *errorOut =
                    "Dear ImGui render monitor failed to create imported export semaphores.";
            }
            return false;
        }

        VkImportSemaphoreFdInfoKHR import{};
        import.sType = VK_STRUCTURE_TYPE_IMPORT_SEMAPHORE_FD_INFO_KHR;
        import.handleType = VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_OPAQUE_FD_BIT;
        import.semaphore = slot.ready;
        import.fd = frame.readySemaphoreFd;
        if (impl->importSemaphoreFd(impl->device, &import) != VK_SUCCESS) {
            close(frame.readySemaphoreFd);
            close(frame.consumedSemaphoreFd);
            resetSlot();
            if (errorOut) {
                *errorOut =
                    "Dear ImGui render monitor failed to import ready semaphore FD.";
            }
            return false;
        }
        import.semaphore = slot.consumed;
        import.fd = frame.consumedSemaphoreFd;
        if (impl->importSemaphoreFd(impl->device, &import) != VK_SUCCESS) {
            close(frame.consumedSemaphoreFd);
            resetSlot();
            if (errorOut) {
                *errorOut =
                    "Dear ImGui render monitor failed to import consumed semaphore FD.";
            }
            return false;
        }
        slot.producerSessionId = frame.producerSessionId;
        slot.initialized = true;
    }

    VkPipelineStageFlags waitStage = VK_PIPELINE_STAGE_TRANSFER_BIT;
    VkSubmitInfo waitForReady{};
    waitForReady.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    waitForReady.waitSemaphoreCount = 1;
    waitForReady.pWaitSemaphores = &slot.ready;
    waitForReady.pWaitDstStageMask = &waitStage;
    if (vkQueueSubmit(impl->queue, 1, &waitForReady, VK_NULL_HANDLE) !=
        VK_SUCCESS) {
        if (errorOut) {
            *errorOut =
                "Dear ImGui render monitor failed to wait for the exported frame.";
        }
        return false;
    }

    if (!slot.importer ||
        !slot.importer->importExternalFrame(frame, errorOut) ||
        !slot.importer->finishPendingCopy(nullptr, errorOut)) {
        releaseRenderMonitorExportFrame(impl, frame, nullptr);
        return false;
    }
    if (imageOut) {
        *imageOut = slot.importer->externalImage();
    }
    return imageOut && imageOut->imageView != VK_NULL_HANDLE &&
        imageOut->size.valid();
}

bool ensureVulkanReady(ImGuiPreviewWindow::Impl* impl,
                       VkPhysicalDevice preferredPhysicalDevice,
                       std::string* errorOut)
{
    if (!impl) {
        return false;
    }
    if (impl->imguiBackendsInitialized) {
        return true;
    }
    if (impl->window == nullptr) {
        if (errorOut) {
            *errorOut = "Dear ImGui preview GLFW window is unavailable.";
        }
        return false;
    }
    if (!glfwVulkanSupported()) {
        if (errorOut) {
            *errorOut = "GLFW reports that Vulkan is not supported on this system.";
        }
        return false;
    }
    if (!createInstance(impl, errorOut)) {
        return false;
    }
    if (glfwCreateWindowSurface(impl->instance, impl->window, nullptr, &impl->surface) != VK_SUCCESS) {
        if (errorOut) {
            *errorOut = "Failed to create Vulkan surface for Dear ImGui preview.";
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
            std::fprintf(stderr,
                         "Dear ImGui Vulkan preview backend error: %d\n",
                         static_cast<int>(err));
        }
    };
    if (!ImGui_ImplVulkan_Init(&initInfo)) {
        if (errorOut) {
            *errorOut = "Failed to initialize Dear ImGui Vulkan backend.";
        }
        cleanupVulkan(impl);
        return false;
    }
    impl->imguiBackendsInitialized = true;
    std::string handoffError;
    if (!impl->frameHandoff.initialize({impl->physicalDevice, impl->device, impl->queue, impl->queueFamily},
                                       &handoffError)) {
        if (errorOut) {
            *errorOut = handoffError;
        }
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

bool frameRender(ImGuiPreviewWindow::Impl* impl, ImDrawData* drawData)
{
    ImGui_ImplVulkanH_Window* wd = &impl->windowData;
    VkSemaphore imageAcquiredSemaphore = wd->FrameSemaphores[wd->SemaphoreIndex].ImageAcquiredSemaphore;
    VkSemaphore renderCompleteSemaphore = wd->FrameSemaphores[wd->SemaphoreIndex].RenderCompleteSemaphore;
    VkResult err = vkAcquireNextImageKHR(impl->device,
                                         wd->Swapchain,
                                         kMonitorSwapchainWaitTimeoutNs,
                                         imageAcquiredSemaphore,
                                         VK_NULL_HANDLE,
                                         &wd->FrameIndex);
    if (err == VK_ERROR_OUT_OF_DATE_KHR || err == VK_SUBOPTIMAL_KHR) {
        impl->swapchainRebuild = true;
    }
    if (err == VK_ERROR_OUT_OF_DATE_KHR) {
        return false;
    }
    if (err == VK_TIMEOUT || err == VK_NOT_READY) {
        return false;
    }
    if (err != VK_SUCCESS && err != VK_SUBOPTIMAL_KHR) {
        return false;
    }

    ImGui_ImplVulkanH_Frame* fd = &wd->Frames[wd->FrameIndex];
    const VkResult fenceWait =
        vkWaitForFences(impl->device,
                        1,
                        &fd->Fence,
                        VK_TRUE,
                        kMonitorSwapchainWaitTimeoutNs);
    if (fenceWait == VK_TIMEOUT || fenceWait == VK_NOT_READY) {
        return false;
    }
    if (fenceWait != VK_SUCCESS) {
        impl->swapchainRebuild = true;
        return false;
    }
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
    if (vkQueueSubmit(impl->queue, 1, &submitInfo, fd->Fence) != VK_SUCCESS) {
        impl->swapchainRebuild = true;
        return false;
    }
    return true;
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

bool ImGuiPreviewWindow::initialize(const std::string& title, jcut::core::SizeI initialSize)
{
    shutdown();

    if (!glfwInit()) {
        markFailure("glfwInit() failed for Dear ImGui preview.");
        return false;
    }
    m_impl->glfwInitialized = true;

    glfwWindowHint(GLFW_CLIENT_API, GLFW_NO_API);
    m_impl->window = glfwCreateWindow(std::max(320, initialSize.width),
                                      std::max(240, initialSize.height),
                                      title.c_str(),
                                      nullptr,
                                      nullptr);
    if (!m_impl->window) {
        markFailure("glfwCreateWindow() failed for Dear ImGui preview.");
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
        markFailure("ImGui GLFW Vulkan backend initialization failed.");
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
           trimCopy(m_impl->failureReason).empty() &&
           !glfwWindowShouldClose(m_impl->window);
}

bool ImGuiPreviewWindow::hasFailed() const
{
    return !trimCopy(m_impl->failureReason).empty();
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

std::string ImGuiPreviewWindow::failureReason() const
{
    return m_impl ? m_impl->failureReason : std::string();
}

void ImGuiPreviewWindow::setStatusText(const std::string& text)
{
    m_impl->statusText = text;
}

void ImGuiPreviewWindow::setWindowTitle(const std::string& title)
{
    m_impl->windowTitle = title;
    if (!m_impl->window) {
        return;
    }
    glfwSetWindowTitle(m_impl->window, m_impl->windowTitle.c_str());
}

void ImGuiPreviewWindow::setTimelineRange(int minFrame, int maxFrame, int latestProcessedFrame)
{
    m_impl->minTimelineFrame = minFrame;
    m_impl->maxTimelineFrame = std::max(minFrame, maxFrame);
    m_impl->latestProcessedFrame = clampFrameToRange(latestProcessedFrame,
                                                     m_impl->minTimelineFrame,
                                                     m_impl->maxTimelineFrame);
    if (m_impl->followLatest) {
        m_impl->requestedPreviewFrame = m_impl->latestProcessedFrame;
        m_impl->redrawRequested = true;
    } else {
        m_impl->requestedPreviewFrame =
            clampFrameToRange(m_impl->requestedPreviewFrame,
                              m_impl->minTimelineFrame,
                              m_impl->latestProcessedFrame);
    }
}

void ImGuiPreviewWindow::setProcessingPaused(bool paused)
{
    m_impl->processingPaused = paused;
    m_impl->processingPausedRequested = paused;
    if (!paused) {
        m_impl->historyPlaying = false;
        m_impl->followLatest = true;
        m_impl->requestedPreviewFrame = m_impl->latestProcessedFrame;
    }
    m_impl->redrawRequested = true;
}

void ImGuiPreviewWindow::setFollowLatest(bool followLatest)
{
    m_impl->followLatest = followLatest;
    if (followLatest) {
        m_impl->historyPlaying = false;
        m_impl->requestedPreviewFrame = m_impl->latestProcessedFrame;
    }
    m_impl->redrawRequested = true;
}

void ImGuiPreviewWindow::setRequestedPreviewFrame(int frameNumber)
{
    m_impl->followLatest = false;
    m_impl->historyPlaying = false;
    m_impl->requestedPreviewFrame =
        clampFrameToRange(frameNumber,
                          m_impl->minTimelineFrame,
                          std::max(m_impl->minTimelineFrame, m_impl->latestProcessedFrame));
    m_impl->redrawRequested = true;
}

void ImGuiPreviewWindow::setPreviewPlaybackActive(bool active)
{
    m_impl->historyPlaying = active;
    if (active) {
        m_impl->followLatest = false;
        m_impl->requestedPreviewFrame =
            clampFrameToRange(m_impl->requestedPreviewFrame,
                              m_impl->minTimelineFrame,
                              std::max(m_impl->minTimelineFrame, m_impl->latestProcessedFrame));
    }
    m_impl->redrawRequested = true;
}

void ImGuiPreviewWindow::setPreviewPlaybackSpeed(float speed)
{
    m_impl->historyPlaybackSpeed = std::clamp(speed, 0.25f, 4.0f);
    m_impl->redrawRequested = true;
}

void ImGuiPreviewWindow::setShowDetections(bool show)
{
    m_impl->showDetections = show;
    m_impl->redrawRequested = true;
}

void ImGuiPreviewWindow::setShowTracks(bool show)
{
    m_impl->showTracks = show;
    m_impl->redrawRequested = true;
}

void ImGuiPreviewWindow::setShowLabels(bool show)
{
    m_impl->showTrackLabels = show;
    m_impl->redrawRequested = true;
}

void ImGuiPreviewWindow::setShowConfirmedTracks(bool show)
{
    m_impl->showConfirmedTracks = show;
    m_impl->redrawRequested = true;
}

void ImGuiPreviewWindow::setShowTentativeTracks(bool show)
{
    m_impl->showTentativeTracks = show;
    m_impl->redrawRequested = true;
}

void ImGuiPreviewWindow::setShowLostTracks(bool show)
{
    m_impl->showLostTracks = show;
    m_impl->redrawRequested = true;
}

void ImGuiPreviewWindow::setDetectionLineThickness(float value)
{
    m_impl->detectionLineThickness = std::clamp(value, 1.0f, 4.0f);
    m_impl->redrawRequested = true;
}

void ImGuiPreviewWindow::setTrackLineThickness(float value)
{
    m_impl->trackLineThickness = std::clamp(value, 1.0f, 5.0f);
    m_impl->redrawRequested = true;
}

void ImGuiPreviewWindow::setOverlayOpacity(float value)
{
    m_impl->overlayOpacity = std::clamp(value, 0.2f, 1.0f);
    m_impl->redrawRequested = true;
}

bool ImGuiPreviewWindow::processingPausedRequested() const
{
    return m_impl->processingPausedRequested;
}

bool ImGuiPreviewWindow::followLatest() const
{
    return m_impl->followLatest;
}

bool ImGuiPreviewWindow::previewPlaybackActive() const
{
    return m_impl->historyPlaying;
}

float ImGuiPreviewWindow::previewPlaybackSpeed() const
{
    return m_impl->historyPlaybackSpeed;
}

bool ImGuiPreviewWindow::showDetections() const
{
    return m_impl->showDetections;
}

bool ImGuiPreviewWindow::showTracks() const
{
    return m_impl->showTracks;
}

bool ImGuiPreviewWindow::showLabels() const
{
    return m_impl->showTrackLabels;
}

bool ImGuiPreviewWindow::showConfirmedTracks() const
{
    return m_impl->showConfirmedTracks;
}

bool ImGuiPreviewWindow::showTentativeTracks() const
{
    return m_impl->showTentativeTracks;
}

bool ImGuiPreviewWindow::showLostTracks() const
{
    return m_impl->showLostTracks;
}

float ImGuiPreviewWindow::detectionLineThickness() const
{
    return m_impl->detectionLineThickness;
}

float ImGuiPreviewWindow::trackLineThickness() const
{
    return m_impl->trackLineThickness;
}

float ImGuiPreviewWindow::overlayOpacity() const
{
    return m_impl->overlayOpacity;
}

int ImGuiPreviewWindow::requestedPreviewFrame() const
{
    return m_impl->requestedPreviewFrame;
}

bool ImGuiPreviewWindow::previewRefreshRequested() const
{
    return m_impl->redrawRequested;
}

void ImGuiPreviewWindow::pumpEvents()
{
    if (!m_impl->window) {
        return;
    }
    const double nowSec = glfwGetTime();
    if (m_impl->lastUiTickSec <= 0.0) {
        m_impl->lastUiTickSec = nowSec;
    }
    const double deltaSec = std::max(0.0, nowSec - m_impl->lastUiTickSec);
    m_impl->lastUiTickSec = nowSec;
    if (m_impl->followLatest) {
        m_impl->requestedPreviewFrame = m_impl->latestProcessedFrame;
    } else if (m_impl->historyPlaying && m_impl->latestProcessedFrame > m_impl->minTimelineFrame) {
        m_impl->previewFrameAccumulator +=
            deltaSec * (static_cast<double>(kTimelineFps) * m_impl->historyPlaybackSpeed);
        while (m_impl->previewFrameAccumulator >= 1.0) {
            m_impl->previewFrameAccumulator -= 1.0;
            if (m_impl->requestedPreviewFrame < m_impl->latestProcessedFrame) {
                ++m_impl->requestedPreviewFrame;
                m_impl->redrawRequested = true;
            } else {
                m_impl->historyPlaying = false;
                break;
            }
        }
    }
    m_impl->requestedPreviewFrame =
        clampFrameToRange(m_impl->requestedPreviewFrame,
                          m_impl->minTimelineFrame,
                          std::max(m_impl->minTimelineFrame, m_impl->latestProcessedFrame));
    glfwPollEvents();
    if (glfwWindowShouldClose(m_impl->window) && trimCopy(m_impl->failureReason).empty()) {
        markFailure("Dear ImGui preview window was closed.");
    }
}

bool ImGuiPreviewWindow::presentFrame(const render_detail::OffscreenVulkanFrame& frame,
                                      int64_t frameNumber,
                                      std::span<const jcut::imgui_preview::TrackOverlay> tracks,
                                      std::span<const jcut::imgui_preview::DetectionOverlay> detections,
                                      const jcut::core::RectF& roiRect,
                                      int detectionCount)
{
    m_impl->updatePending = true;
    if (!isActive()) {
        m_impl->updatePending = false;
        return false;
    }
    if (!frame.valid || frame.imageView == VK_NULL_HANDLE || !frame.size.valid()) {
        m_impl->updatePending = false;
        return false;
    }

    std::string error;
    if (!ensureVulkanReady(m_impl.get(), VK_NULL_HANDLE, &error)) {
        markFailure(error);
        m_impl->updatePending = false;
        return false;
    }
    rebuildSwapchainIfNeeded(m_impl.get());

    std::string handoffError;
    if (!m_impl->frameHandoff.importOffscreenFrame(frame, &handoffError)) {
        markFailure(handoffError);
        m_impl->updatePending = false;
        return false;
    }

    const jcut::vulkan_detector::VulkanExternalImage external = m_impl->frameHandoff.externalImage();
    if (external.imageView == VK_NULL_HANDLE || !external.size.valid()) {
        markFailure("Dear ImGui preview received an invalid imported Vulkan image.");
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
            markFailure("Failed to bind imported Vulkan image into Dear ImGui.");
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
    ImGui::Begin("JCut FaceDetections Preview", nullptr, flags);
    ImGui::TextUnformatted(m_impl->windowTitle.c_str());
    if (!trimCopy(m_impl->statusText).empty()) {
        ImGui::Separator();
        ImGui::TextWrapped("%s", m_impl->statusText.c_str());
    }
    ImGui::Separator();

    const int timelineMax = std::max(m_impl->minTimelineFrame, m_impl->maxTimelineFrame);
    const int processedFrame =
        std::clamp(m_impl->latestProcessedFrame, m_impl->minTimelineFrame, timelineMax);
    const int requestedFrame = clampFrameToRange(m_impl->requestedPreviewFrame,
                                                 m_impl->minTimelineFrame,
                                                 std::max(m_impl->minTimelineFrame, processedFrame));
    const int totalSpan = std::max(1, timelineMax - m_impl->minTimelineFrame);
    const float processedFraction =
        static_cast<float>(processedFrame - m_impl->minTimelineFrame) / static_cast<float>(totalSpan);
    const float requestedFraction =
        static_cast<float>(requestedFrame - m_impl->minTimelineFrame) / static_cast<float>(totalSpan);
    const std::string progressMode = m_impl->followLatest
        ? "follow latest"
        : (m_impl->historyPlaying ? "history playback" : "manual inspect");
    const std::string progressTitle = formatString(
        "%s  %.1f%%",
        m_impl->processingPausedRequested ? "Processing Paused" : "Processing",
        processedFraction * 100.0f);
    const std::string progressDetail = formatString(
        "processed %d / %d frames   preview %d   %s",
        processedFrame,
        timelineMax,
        requestedFrame,
        progressMode.c_str());
    drawAnimatedProgressBar("processing_progress_bar",
                            processedFraction,
                            progressTitle,
                            progressDetail,
                            requestedFraction);

    if (ImGui::Button(m_impl->processingPausedRequested ? "Resume Processing" : "Pause Processing")) {
        m_impl->processingPausedRequested = !m_impl->processingPausedRequested;
        m_impl->redrawRequested = true;
        if (!m_impl->processingPausedRequested) {
            m_impl->followLatest = true;
            m_impl->historyPlaying = false;
            m_impl->requestedPreviewFrame = m_impl->latestProcessedFrame;
        }
    }
    ImGui::SameLine();
    if (ImGui::Button(m_impl->historyPlaying ? "Pause Preview" : "Play Preview")) {
        m_impl->historyPlaying = !m_impl->historyPlaying;
        m_impl->redrawRequested = true;
        if (m_impl->historyPlaying) {
            m_impl->followLatest = false;
            m_impl->requestedPreviewFrame =
                clampFrameToRange(m_impl->requestedPreviewFrame,
                                  m_impl->minTimelineFrame,
                                  m_impl->latestProcessedFrame);
        }
    }
    ImGui::SameLine();
    if (ImGui::Checkbox("Follow Latest", &m_impl->followLatest)) {
        m_impl->redrawRequested = true;
    }
    if (m_impl->followLatest) {
        m_impl->historyPlaying = false;
        m_impl->requestedPreviewFrame = m_impl->latestProcessedFrame;
    }
    const int seekTimelineMax = std::max(m_impl->minTimelineFrame, m_impl->latestProcessedFrame);
    int requestedFrameSlider = clampFrameToRange(m_impl->requestedPreviewFrame,
                                                 m_impl->minTimelineFrame,
                                                 seekTimelineMax);
    ImGui::SetNextItemWidth(240.0f);
    if (ImGui::SliderInt("Seek", &requestedFrameSlider, m_impl->minTimelineFrame, seekTimelineMax)) {
        m_impl->followLatest = false;
        m_impl->historyPlaying = false;
        m_impl->requestedPreviewFrame = requestedFrameSlider;
        m_impl->redrawRequested = true;
    }
    ImGui::SameLine();
    ImGui::SetNextItemWidth(120.0f);
    if (ImGui::SliderFloat("Speed", &m_impl->historyPlaybackSpeed, 0.25f, 4.0f, "%.2fx")) {
        m_impl->redrawRequested = true;
    }
    m_impl->historyPlaybackSpeed = std::clamp(m_impl->historyPlaybackSpeed, 0.25f, 4.0f);

    if (ImGui::Checkbox("Detections", &m_impl->showDetections)) {
        m_impl->redrawRequested = true;
    }
    ImGui::SameLine();
    if (ImGui::Checkbox("Tracks", &m_impl->showTracks)) {
        m_impl->redrawRequested = true;
    }
    ImGui::SameLine();
    if (ImGui::Checkbox("ROI", &m_impl->showRoi)) {
        m_impl->redrawRequested = true;
    }
    ImGui::SameLine();
    if (ImGui::Checkbox("Labels", &m_impl->showTrackLabels)) {
        m_impl->redrawRequested = true;
    }
    if (ImGui::Checkbox("Confirmed", &m_impl->showConfirmedTracks)) {
        m_impl->redrawRequested = true;
    }
    ImGui::SameLine();
    if (ImGui::Checkbox("Tentative", &m_impl->showTentativeTracks)) {
        m_impl->redrawRequested = true;
    }
    ImGui::SameLine();
    if (ImGui::Checkbox("Lost", &m_impl->showLostTracks)) {
        m_impl->redrawRequested = true;
    }
    ImGui::SetNextItemWidth(140.0f);
    if (ImGui::SliderFloat("Det Line", &m_impl->detectionLineThickness, 1.0f, 4.0f, "%.1f")) {
        m_impl->redrawRequested = true;
    }
    ImGui::SameLine();
    ImGui::SetNextItemWidth(140.0f);
    if (ImGui::SliderFloat("Track Line", &m_impl->trackLineThickness, 1.0f, 5.0f, "%.1f")) {
        m_impl->redrawRequested = true;
    }
    ImGui::SameLine();
    ImGui::SetNextItemWidth(140.0f);
    if (ImGui::SliderFloat("Opacity", &m_impl->overlayOpacity, 0.2f, 1.0f, "%.2f")) {
        m_impl->redrawRequested = true;
    }
    m_impl->detectionLineThickness = std::clamp(m_impl->detectionLineThickness, 1.0f, 4.0f);
    m_impl->trackLineThickness = std::clamp(m_impl->trackLineThickness, 1.0f, 5.0f);
    m_impl->overlayOpacity = std::clamp(m_impl->overlayOpacity, 0.2f, 1.0f);
    ImGui::Separator();

    const ImVec2 panelAvail = ImGui::GetContentRegionAvail();
    constexpr float kTrackPanelWidth = 320.0f;
    const bool showTrackPanel = panelAvail.x > 720.0f;
    const ImVec2 imageAvail(
        std::max(1.0f, panelAvail.x - (showTrackPanel ? (kTrackPanelWidth + 12.0f) : 0.0f)),
        panelAvail.y);
    const ImVec2 fitted = fitImageIntoRegion(external.size, imageAvail);
    const float offsetX = std::max(0.0f, (imageAvail.x - fitted.x) * 0.5f);
    if (offsetX > 0.0f) {
        ImGui::SetCursorPosX(ImGui::GetCursorPosX() + offsetX);
    }
    const ImVec2 imagePos = ImGui::GetCursorScreenPos();
    ImGui::Image(m_impl->textureSet, fitted, ImVec2(0.0f, 1.0f), ImVec2(1.0f, 0.0f));

    ImDrawList* drawList = ImGui::GetWindowDrawList();
    const float scaleX = fitted.x / static_cast<float>(std::max(1, external.size.width));
    const float scaleY = fitted.y / static_cast<float>(std::max(1, external.size.height));
    const int alpha = static_cast<int>(std::round(255.0f * m_impl->overlayOpacity));
    const ImU32 roiColor = IM_COL32(255, 170, 51, alpha);
    const ImU32 detColor = IM_COL32(168, 85, 247, alpha);
    auto trackColor = [alpha](jcut::imgui_preview::OverlayTrackState state) {
        if (state == jcut::imgui_preview::OverlayTrackState::Removed) {
            return IM_COL32(160, 160, 160, std::min(alpha, 220));
        }
        return IM_COL32(168, 85, 247, alpha);
    };
    auto trackStateLabel = [](jcut::imgui_preview::OverlayTrackState state) -> const char* {
        switch (state) {
        case jcut::imgui_preview::OverlayTrackState::Confirmed:
            return "Confirmed";
        case jcut::imgui_preview::OverlayTrackState::Tentative:
            return "Tentative";
        case jcut::imgui_preview::OverlayTrackState::Lost:
            return "Lost";
        case jcut::imgui_preview::OverlayTrackState::Removed:
        default:
            return "Removed";
        }
    };
    if (m_impl->showRoi && validNonEmptyRect(roiRect)) {
        const ImVec2 roiMin(imagePos.x + static_cast<float>(roiRect.x) * scaleX,
                            imagePos.y + static_cast<float>(roiRect.y) * scaleY);
        const ImVec2 roiMax(imagePos.x + static_cast<float>(rectRight(roiRect)) * scaleX,
                            imagePos.y + static_cast<float>(rectBottom(roiRect)) * scaleY);
        drawList->AddRect(roiMin, roiMax, roiColor, 0.0f, 0, 2.0f);
    }
    if (m_impl->showDetections) {
        for (const jcut::imgui_preview::DetectionOverlay& detection : detections) {
            const jcut::core::RectF& box = detection.box;
            if (!validNonEmptyRect(box)) {
                continue;
            }
            const ImVec2 boxMin(imagePos.x + static_cast<float>(box.x) * scaleX,
                                imagePos.y + static_cast<float>(box.y) * scaleY);
            const ImVec2 boxMax(imagePos.x + static_cast<float>(rectRight(box)) * scaleX,
                                imagePos.y + static_cast<float>(rectBottom(box)) * scaleY);
            drawList->AddRect(boxMin, boxMax, detColor, 0.0f, 0, m_impl->detectionLineThickness);
        }
    }

    int confirmedCount = 0;
    int tentativeCount = 0;
    int lostCount = 0;
    for (const jcut::imgui_preview::TrackOverlay& track : tracks) {
        if (track.state == jcut::imgui_preview::OverlayTrackState::Removed ||
            !validNonEmptyRect(track.box)) {
            continue;
        }
        if ((track.state == jcut::imgui_preview::OverlayTrackState::Confirmed && !m_impl->showConfirmedTracks) ||
            (track.state == jcut::imgui_preview::OverlayTrackState::Tentative && !m_impl->showTentativeTracks) ||
            (track.state == jcut::imgui_preview::OverlayTrackState::Lost && !m_impl->showLostTracks)) {
            continue;
        }
        switch (track.state) {
        case jcut::imgui_preview::OverlayTrackState::Confirmed:
            ++confirmedCount;
            break;
        case jcut::imgui_preview::OverlayTrackState::Tentative:
            ++tentativeCount;
            break;
        case jcut::imgui_preview::OverlayTrackState::Lost:
            ++lostCount;
            break;
        case jcut::imgui_preview::OverlayTrackState::Removed:
            break;
        }
        const ImU32 color = trackColor(track.state);
        if (m_impl->showTracks) {
            const ImVec2 boxMin(imagePos.x + static_cast<float>(track.box.x) * scaleX,
                                imagePos.y + static_cast<float>(track.box.y) * scaleY);
            const ImVec2 boxMax(imagePos.x + static_cast<float>(rectRight(track.box)) * scaleX,
                                imagePos.y + static_cast<float>(rectBottom(track.box)) * scaleY);
            drawList->AddRect(boxMin, boxMax, color, 0.0f, 0, m_impl->trackLineThickness);
            if (m_impl->showTrackLabels) {
                const std::string labelText =
                    formatString("T%d  %s", track.id, trackStateLabel(track.state));
                const ImVec2 labelSize = ImGui::CalcTextSize(labelText.c_str());
                const ImVec2 labelMin(boxMin.x, std::max(imagePos.y, boxMin.y - labelSize.y - 6.0f));
                const ImVec2 labelMax(labelMin.x + labelSize.x + 10.0f, labelMin.y + labelSize.y + 4.0f);
                drawList->AddRectFilled(labelMin, labelMax, IM_COL32(0, 0, 0, std::min(alpha, 180)), 5.0f);
                drawList->AddText(ImVec2(labelMin.x + 5.0f, labelMin.y + 2.0f), color, labelText.c_str());
            }
        }
    }

    const ImVec2 panelMin(imagePos.x + 8.0f, imagePos.y + 8.0f);
    const ImVec2 panelMax(panelMin.x + 270.0f, panelMin.y + 58.0f);
    drawList->AddRectFilled(panelMin, panelMax, IM_COL32(0, 0, 0, 160), 6.0f);
    drawList->AddText(ImVec2(panelMin.x + 10.0f, panelMin.y + 9.0f),
                      IM_COL32(255, 255, 255, 255),
                      formatString("Detections: %d", detectionCount).c_str());
    drawList->AddText(ImVec2(panelMin.x + 10.0f, panelMin.y + 30.0f),
                      IM_COL32(255, 255, 255, 255),
                      formatString("Tracks: %zu  C:%d  T:%d  L:%d",
                                   tracks.size(),
                                   confirmedCount,
                                   tentativeCount,
                                   lostCount)
                          .c_str());

    if (showTrackPanel) {
        ImGui::SameLine();
        ImGui::BeginChild("Tracking Inspector",
                          ImVec2(kTrackPanelWidth, 0.0f),
                          true,
                          ImGuiWindowFlags_NoScrollWithMouse);
        ImGui::TextUnformatted("Tracking");
        ImGui::Separator();
        ImGui::Text("Frame: %lld", static_cast<long long>(frameNumber));
        ImGui::Text("Detections: %d", detectionCount);
        ImGui::Text("Tracks: %d", static_cast<int>(tracks.size()));
        ImGui::Text("Confirmed: %d", confirmedCount);
        ImGui::Text("Tentative: %d", tentativeCount);
        ImGui::Text("Lost: %d", lostCount);
        ImGui::Separator();
        ImGui::TextUnformatted("Active Tracks");
        ImGui::BeginChild("Track Rows", ImVec2(0.0f, 0.0f), false);
        for (const jcut::imgui_preview::TrackOverlay& track : tracks) {
            if (track.state == jcut::imgui_preview::OverlayTrackState::Removed) {
                continue;
            }
            const ImU32 color = trackColor(track.state);
            ImGui::PushStyleColor(ImGuiCol_Text, ImGui::ColorConvertU32ToFloat4(color));
            ImGui::Text("T%d  %s", track.id, trackStateLabel(track.state));
            ImGui::PopStyleColor();
            ImGui::TextDisabled("frames %d-%d | hits %d | misses %d",
                                track.firstFrame,
                                track.lastFrame,
                                track.hits,
                                track.misses);
            ImGui::TextDisabled("box %.0f, %.0f  %.0fx%.0f",
                                std::round(track.box.x),
                                std::round(track.box.y),
                                std::round(track.box.width),
                                std::round(track.box.height));
            ImGui::Separator();
        }
        ImGui::EndChild();
        ImGui::EndChild();
    }

    ImGui::End();
    ImGui::PopStyleVar(2);

    ImGui::Render();
    ImDrawData* drawData = ImGui::GetDrawData();
    const bool minimized = drawData->DisplaySize.x <= 0.0f || drawData->DisplaySize.y <= 0.0f;
    if (!minimized) {
        if (frameRender(m_impl.get(), drawData)) {
            framePresent(m_impl.get());
        }
    }

    m_impl->lastPresentedSourceFrame = frameNumber;
    m_impl->redrawRequested = false;
    m_impl->updatePending = false;
    return true;
}

bool ImGuiPreviewWindow::presentRenderMonitorFrame(
    const render_detail::OffscreenVulkanFrame& frame,
    const RenderMonitorStatus& status)
{
    m_impl->updatePending = true;
    if (!isActive()) {
        m_impl->updatePending = false;
        return false;
    }
    if (!frame.valid || frame.imageView == VK_NULL_HANDLE || !frame.size.valid()) {
        m_impl->updatePending = false;
        return false;
    }

    std::string error;
    if (!ensureVulkanReady(m_impl.get(), VK_NULL_HANDLE, &error)) {
        markFailure(error);
        m_impl->updatePending = false;
        return false;
    }
    rebuildSwapchainIfNeeded(m_impl.get());

    if (m_impl->renderMonitorProducerSessionId == frame.producerSessionId &&
        frame.presentationSequence > 0 &&
        frame.presentationSequence <=
            m_impl->lastAcceptedRenderMonitorSequence) {
        const bool discarded = discardRenderMonitorFrame(frame);
        m_impl->updatePending = false;
        return discarded;
    }
    if (m_impl->renderMonitorProducerSessionId != frame.producerSessionId) {
        m_impl->renderMonitorProducerSessionId = frame.producerSessionId;
        m_impl->lastAcceptedRenderMonitorSequence = 0;
    }

    jcut::vulkan_import::ExternalImage external;
    std::string handoffError;
    if (!acquireRenderMonitorExportFrame(
            m_impl.get(), frame, &external, &handoffError)) {
        markFailure(handoffError);
        m_impl->updatePending = false;
        return false;
    }
    ScopeExit releaseFrameOnError([&]() {
        releaseRenderMonitorExportFrame(
            m_impl.get(), frame, nullptr);
    });
    m_impl->lastAcceptedRenderMonitorSequence = frame.presentationSequence;
    if (external.imageView == VK_NULL_HANDLE || !external.size.valid()) {
        markFailure("Dear ImGui render monitor received an invalid imported Vulkan image.");
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
        m_impl->textureSet =
            ImGui_ImplVulkan_AddTexture(m_impl->sampler,
                                        external.imageView,
                                        external.imageLayout);
        if (m_impl->textureSet == VK_NULL_HANDLE) {
            markFailure("Failed to bind exported Vulkan frame into Dear ImGui.");
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
    ImGui::Begin("JCut Render Monitor", nullptr, flags);
    ImGui::TextUnformatted(m_impl->windowTitle.empty()
                               ? "JCut Render Monitor"
                               : m_impl->windowTitle.c_str());
    if (!trimCopy(m_impl->statusText).empty()) {
        ImGui::Separator();
        ImGui::TextWrapped("%s", m_impl->statusText.c_str());
    }
    ImGui::Separator();

    const int64_t totalFrames = std::max<int64_t>(1, status.totalFrames);
    const int64_t completedFrames =
        std::clamp<int64_t>(status.framesCompleted, 0, totalFrames);
    const float fraction =
        static_cast<float>(completedFrames) / static_cast<float>(totalFrames);
    const std::string progressTitle =
        formatString("Export %.1f%%", fraction * 100.0f);
    const std::string progressDetail =
        formatString("%lld / %lld frames   timeline %lld   ETA %s",
                     static_cast<long long>(completedFrames),
                     static_cast<long long>(totalFrames),
                     static_cast<long long>(status.timelineFrame),
                     formatDurationMs(status.estimatedRemainingMs).c_str());
    drawAnimatedProgressBar("render_monitor_progress",
                            fraction,
                            progressTitle,
                            progressDetail);

    if (!status.activity.empty()) {
        ImGui::Text("Activity: %s", status.activity.c_str());
    }
    if (ImGui::Button("Cancel Render")) {
        m_impl->renderMonitorCancelRequested = true;
    }
    ImGui::SameLine();
    ImGui::Text("Elapsed %s", formatDurationMs(status.elapsedMs).c_str());
    ImGui::SameLine();
    ImGui::Text("Segment %d / %d",
                status.segmentIndex,
                std::max(1, status.segmentCount));
    if (status.incrementalChunksTotal > 0) {
        ImGui::Text("Incremental chunks: %d / %d   reused frames: %lld",
                    status.incrementalChunksCompleted,
                    status.incrementalChunksTotal,
                    static_cast<long long>(status.incrementalFramesReused));
    }
    if (!status.cachePath.empty()) {
        ImGui::TextWrapped("Cache: %s", status.cachePath.c_str());
    }
    ImGui::Separator();
    ImGui::Columns(2, "render_monitor_stats", false);
    ImGui::TextUnformatted("Pipeline");
    ImGui::Text("Renderer: %s", status.usingGpu ? "Vulkan" : "CPU");
    ImGui::Text("Encoder: %s",
                status.encoderLabel.empty() ? "unknown"
                                            : status.encoderLabel.c_str());
    ImGui::Text("Hardware encode: %s",
                status.usingHardwareEncode ? "yes" : "no");
    ImGui::Text("Transfer: %s",
                status.gpuTransferLabel.empty()
                    ? "unknown"
                    : status.gpuTransferLabel.c_str());
    ImGui::Text("Image sequence: %s",
                status.createVideoFromImageSequence ? "yes" : "no");
    ImGui::NextColumn();
    ImGui::TextUnformatted("Cumulative stage time");
    ImGui::Text("Render: %.2fs", status.renderStageMs / 1000.0);
    ImGui::Text("Decode: %.2fs", status.decodeStageMs / 1000.0);
    ImGui::Text("Texture: %.2fs", status.textureStageMs / 1000.0);
    ImGui::Text("Composite: %.2fs", status.compositeStageMs / 1000.0);
    ImGui::Text("Readback: %.2fs", status.readbackStageMs / 1000.0);
    ImGui::Text("Encode: %.2fs", status.encodeStageMs / 1000.0);
    ImGui::Columns(1);
    ImGui::Separator();

    const ImVec2 imageAvail = ImGui::GetContentRegionAvail();
    const ImVec2 fitted = fitImageIntoRegion(external.size, imageAvail);
    const float offsetX = std::max(0.0f, (imageAvail.x - fitted.x) * 0.5f);
    if (offsetX > 0.0f) {
        ImGui::SetCursorPosX(ImGui::GetCursorPosX() + offsetX);
    }
    ImGui::Image(m_impl->textureSet,
                 fitted,
                 ImVec2(0.0f, 0.0f),
                 ImVec2(1.0f, 1.0f));

    ImGui::End();
    ImGui::PopStyleVar(2);

    ImGui::Render();
    ImDrawData* drawData = ImGui::GetDrawData();
    const bool minimized =
        drawData->DisplaySize.x <= 0.0f || drawData->DisplaySize.y <= 0.0f;
    const bool samplingSubmitted = !minimized &&
        frameRender(m_impl.get(), drawData);
    if (!releaseRenderMonitorExportFrame(
            m_impl.get(), frame, &handoffError)) {
        releaseFrameOnError.release();
        markFailure(handoffError);
        m_impl->updatePending = false;
        return false;
    }
    releaseFrameOnError.release();
    if (samplingSubmitted) {
        framePresent(m_impl.get());
    }

    m_impl->lastPresentedSourceFrame = status.timelineFrame;
    m_impl->redrawRequested = false;
    m_impl->updatePending = false;
    return true;
}

bool ImGuiPreviewWindow::discardRenderMonitorFrame(
    const render_detail::OffscreenVulkanFrame& frame)
{
    if (!isActive() || !frame.valid) {
        return false;
    }
    if (frame.consumptionState &&
        frame.consumptionState->completedGeneration.load(
            std::memory_order_acquire) >= frame.generation) {
        if (frame.readySemaphoreFd >= 0) {
            close(frame.readySemaphoreFd);
        }
        if (frame.consumedSemaphoreFd >= 0) {
            close(frame.consumedSemaphoreFd);
        }
        return true;
    }
    std::string error;
    if (!ensureVulkanReady(m_impl.get(), VK_NULL_HANDLE, &error)) {
        markFailure(error);
        return false;
    }
    jcut::vulkan_import::ExternalImage ignored;
    if (!acquireRenderMonitorExportFrame(
            m_impl.get(), frame, &ignored, &error) ||
        !releaseRenderMonitorExportFrame(m_impl.get(), frame, &error)) {
        markFailure(error);
        return false;
    }
    return true;
}

bool ImGuiPreviewWindow::renderMonitorCancelRequested() const
{
    return m_impl && m_impl->renderMonitorCancelRequested;
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

void ImGuiPreviewWindow::markFailure(const std::string& reason)
{
    const std::string trimmedReason = trimCopy(reason);
    m_impl->failureReason = trimmedReason.empty()
        ? "Unknown Dear ImGui preview failure."
        : trimmedReason;
}
