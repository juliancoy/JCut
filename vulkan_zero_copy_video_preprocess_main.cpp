#include "decoder_context.h"
#include "debug_controls.h"
#include "editor_shared.h"
#include "frame_handle.h"
#include "vulkan_zero_copy_face_detector.h"

#include <QImage>
#include <QString>

#include <vulkan/vulkan.h>

#include <algorithm>
#include <chrono>
#include <cstring>
#include <filesystem>
#include <iomanip>
#include <iostream>
#include <string>
#include <vector>

namespace {

struct Options {
    std::string videoPath = "nasreen.mp4";
    std::string proxyPath;
    int maxFrames = 120;
    int stride = 12;
    bool requireZeroCopy = false;
};

void usage(const char* argv0)
{
    std::cerr << "Usage: " << argv0 << " [video] [--max-frames N] [--stride N] [--proxy PATH|none] [--require-zero-copy]\n";
}

bool parseArgs(int argc, char** argv, Options* options)
{
    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        auto value = [&](const char* name) -> const char* {
            if (i + 1 >= argc) {
                std::cerr << "Missing value for " << name << "\n";
                return nullptr;
            }
            return argv[++i];
        };
        if (arg == "--help" || arg == "-h") {
            usage(argv[0]);
            return false;
        } else if (arg == "--max-frames") {
            const char* v = value("--max-frames");
            if (!v) return false;
            options->maxFrames = std::max(1, std::atoi(v));
        } else if (arg == "--stride") {
            const char* v = value("--stride");
            if (!v) return false;
            options->stride = std::max(1, std::atoi(v));
        } else if (arg == "--proxy") {
            const char* v = value("--proxy");
            if (!v) return false;
            options->proxyPath = v;
        } else if (arg == "--require-zero-copy") {
            options->requireZeroCopy = true;
        } else if (!arg.empty() && arg[0] == '-') {
            std::cerr << "Unknown argument: " << arg << "\n";
            usage(argv[0]);
            return false;
        } else {
            options->videoPath = arg;
        }
    }
    return true;
}

TimelineClip makeClip(const Options& options)
{
    TimelineClip clip;
    clip.filePath = QString::fromStdString(options.videoPath);
    clip.mediaType = ClipMediaType::Video;
    clip.sourceKind = MediaSourceKind::File;
    clip.videoEnabled = true;
    clip.useProxy = options.proxyPath != "none";
    if (!options.proxyPath.empty() && options.proxyPath != "none") {
        clip.proxyPath = QString::fromStdString(options.proxyPath);
    }
    return clip;
}

void setError(std::string* error, const char* text)
{
    if (error) {
        *error = text;
    }
}

uint32_t findMemoryType(VkPhysicalDevice physicalDevice,
                        uint32_t typeBits,
                        VkMemoryPropertyFlags properties)
{
    VkPhysicalDeviceMemoryProperties memProps{};
    vkGetPhysicalDeviceMemoryProperties(physicalDevice, &memProps);
    for (uint32_t i = 0; i < memProps.memoryTypeCount; ++i) {
        if ((typeBits & (1u << i)) && (memProps.memoryTypes[i].propertyFlags & properties) == properties) {
            return i;
        }
    }
    return UINT32_MAX;
}

class VulkanBenchContext {
public:
    ~VulkanBenchContext() { release(); }

    bool initialize(std::string* error)
    {
        VkApplicationInfo app{};
        app.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
        app.pApplicationName = "jcut-zero-copy-face-preprocess";
        app.apiVersion = VK_API_VERSION_1_1;

        VkInstanceCreateInfo instanceInfo{};
        instanceInfo.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
        instanceInfo.pApplicationInfo = &app;
        if (vkCreateInstance(&instanceInfo, nullptr, &instance) != VK_SUCCESS) {
            setError(error, "failed to create Vulkan instance");
            return false;
        }

        uint32_t deviceCount = 0;
        vkEnumeratePhysicalDevices(instance, &deviceCount, nullptr);
        if (deviceCount == 0) {
            setError(error, "no Vulkan physical devices found");
            return false;
        }
        std::vector<VkPhysicalDevice> devices(deviceCount);
        vkEnumeratePhysicalDevices(instance, &deviceCount, devices.data());
        physicalDevice = devices.front();

        uint32_t queueFamilyCount = 0;
        vkGetPhysicalDeviceQueueFamilyProperties(physicalDevice, &queueFamilyCount, nullptr);
        std::vector<VkQueueFamilyProperties> families(queueFamilyCount);
        vkGetPhysicalDeviceQueueFamilyProperties(physicalDevice, &queueFamilyCount, families.data());
        for (uint32_t i = 0; i < queueFamilyCount; ++i) {
            if (families[i].queueFlags & VK_QUEUE_COMPUTE_BIT) {
                queueFamilyIndex = i;
                break;
            }
        }
        if (queueFamilyIndex == UINT32_MAX) {
            setError(error, "no Vulkan compute queue family found");
            return false;
        }

        const float priority = 1.0f;
        VkDeviceQueueCreateInfo queueInfo{};
        queueInfo.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
        queueInfo.queueFamilyIndex = queueFamilyIndex;
        queueInfo.queueCount = 1;
        queueInfo.pQueuePriorities = &priority;

        VkDeviceCreateInfo deviceInfo{};
        deviceInfo.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
        deviceInfo.queueCreateInfoCount = 1;
        deviceInfo.pQueueCreateInfos = &queueInfo;
        if (vkCreateDevice(physicalDevice, &deviceInfo, nullptr, &device) != VK_SUCCESS) {
            setError(error, "failed to create Vulkan device");
            return false;
        }
        vkGetDeviceQueue(device, queueFamilyIndex, 0, &queue);

        VkCommandPoolCreateInfo poolInfo{};
        poolInfo.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
        poolInfo.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
        poolInfo.queueFamilyIndex = queueFamilyIndex;
        if (vkCreateCommandPool(device, &poolInfo, nullptr, &commandPool) != VK_SUCCESS) {
            setError(error, "failed to create command pool");
            return false;
        }
        VkCommandBufferAllocateInfo allocInfo{};
        allocInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
        allocInfo.commandPool = commandPool;
        allocInfo.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        allocInfo.commandBufferCount = 1;
        if (vkAllocateCommandBuffers(device, &allocInfo, &commandBuffer) != VK_SUCCESS) {
            setError(error, "failed to allocate command buffer");
            return false;
        }
        VkFenceCreateInfo fenceInfo{};
        fenceInfo.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
        if (vkCreateFence(device, &fenceInfo, nullptr, &fence) != VK_SUCCESS) {
            setError(error, "failed to create fence");
            return false;
        }
        return true;
    }

    void release()
    {
        if (device != VK_NULL_HANDLE) {
            vkDeviceWaitIdle(device);
            if (fence) vkDestroyFence(device, fence, nullptr);
            if (commandPool) vkDestroyCommandPool(device, commandPool, nullptr);
            if (imageView) vkDestroyImageView(device, imageView, nullptr);
            if (image) vkDestroyImage(device, image, nullptr);
            if (imageMemory) vkFreeMemory(device, imageMemory, nullptr);
            destroyBuffer(tensorBuffer, tensorMemory);
            destroyBuffer(detectionBuffer, detectionMemory);
            destroyBuffer(stagingBuffer, stagingMemory);
            vkDestroyDevice(device, nullptr);
        }
        if (instance != VK_NULL_HANDLE) {
            vkDestroyInstance(instance, nullptr);
        }
        instance = VK_NULL_HANDLE;
        physicalDevice = VK_NULL_HANDLE;
        device = VK_NULL_HANDLE;
        queue = VK_NULL_HANDLE;
        queueFamilyIndex = UINT32_MAX;
        commandPool = VK_NULL_HANDLE;
        commandBuffer = VK_NULL_HANDLE;
        fence = VK_NULL_HANDLE;
        image = VK_NULL_HANDLE;
        imageMemory = VK_NULL_HANDLE;
        imageView = VK_NULL_HANDLE;
        imageLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        imageSize = QSize();
        stagingBuffer = VK_NULL_HANDLE;
        stagingMemory = VK_NULL_HANDLE;
        stagingSize = 0;
        tensorBuffer = VK_NULL_HANDLE;
        tensorMemory = VK_NULL_HANDLE;
        tensorSize = 0;
        detectionBuffer = VK_NULL_HANDLE;
        detectionMemory = VK_NULL_HANDLE;
        detectionSize = 0;
        tensorBytes = 0;
    }

    bool createBuffer(VkDeviceSize size,
                      VkBufferUsageFlags usage,
                      VkMemoryPropertyFlags properties,
                      VkBuffer* outBuffer,
                      VkDeviceMemory* outMemory,
                      std::string* error)
    {
        VkBufferCreateInfo info{};
        info.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
        info.size = size;
        info.usage = usage;
        info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
        if (vkCreateBuffer(device, &info, nullptr, outBuffer) != VK_SUCCESS) {
            setError(error, "failed to create buffer");
            return false;
        }
        VkMemoryRequirements req{};
        vkGetBufferMemoryRequirements(device, *outBuffer, &req);
        const uint32_t type = findMemoryType(physicalDevice, req.memoryTypeBits, properties);
        if (type == UINT32_MAX) {
            setError(error, "failed to find buffer memory type");
            return false;
        }
        VkMemoryAllocateInfo alloc{};
        alloc.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
        alloc.allocationSize = req.size;
        alloc.memoryTypeIndex = type;
        if (vkAllocateMemory(device, &alloc, nullptr, outMemory) != VK_SUCCESS) {
            setError(error, "failed to allocate buffer memory");
            return false;
        }
        vkBindBufferMemory(device, *outBuffer, *outMemory, 0);
        return true;
    }

    void destroyBuffer(VkBuffer& buffer, VkDeviceMemory& memory)
    {
        if (buffer) vkDestroyBuffer(device, buffer, nullptr);
        if (memory) vkFreeMemory(device, memory, nullptr);
        buffer = VK_NULL_HANDLE;
        memory = VK_NULL_HANDLE;
    }

    bool ensureResources(const QSize& size, VkDeviceSize tensorBytes, std::string* error)
    {
        if (image != VK_NULL_HANDLE && imageSize == size) {
            return true;
        }
        if (imageView) vkDestroyImageView(device, imageView, nullptr);
        if (image) vkDestroyImage(device, image, nullptr);
        if (imageMemory) vkFreeMemory(device, imageMemory, nullptr);
        imageView = VK_NULL_HANDLE;
        image = VK_NULL_HANDLE;
        imageMemory = VK_NULL_HANDLE;
        imageLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        imageSize = size;

        const VkDeviceSize stagingBytes = static_cast<VkDeviceSize>(size.width()) * size.height() * 4;
        if (stagingBytes > stagingSize) {
            destroyBuffer(stagingBuffer, stagingMemory);
            if (!createBuffer(stagingBytes,
                              VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
                              VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                              &stagingBuffer,
                              &stagingMemory,
                              error)) {
                return false;
            }
            stagingSize = stagingBytes;
        }
        if (tensorBytes > tensorSize) {
            destroyBuffer(tensorBuffer, tensorMemory);
            if (!createBuffer(tensorBytes,
                              VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
                              VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT,
                              &tensorBuffer,
                              &tensorMemory,
                              error)) {
                return false;
            }
            tensorSize = tensorBytes;
        }
        const VkDeviceSize detectionBytesNeeded = static_cast<VkDeviceSize>(16 + (maxDetections * 32));
        if (detectionBytesNeeded > detectionSize) {
            destroyBuffer(detectionBuffer, detectionMemory);
            if (!createBuffer(detectionBytesNeeded,
                              VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                              VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                              &detectionBuffer,
                              &detectionMemory,
                              error)) {
                return false;
            }
            detectionSize = detectionBytesNeeded;
        }

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
        if (vkCreateImage(device, &imageInfo, nullptr, &image) != VK_SUCCESS) {
            setError(error, "failed to create source image");
            return false;
        }
        VkMemoryRequirements req{};
        vkGetImageMemoryRequirements(device, image, &req);
        const uint32_t type = findMemoryType(physicalDevice, req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
        if (type == UINT32_MAX) {
            setError(error, "failed to find image memory type");
            return false;
        }
        VkMemoryAllocateInfo alloc{};
        alloc.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
        alloc.allocationSize = req.size;
        alloc.memoryTypeIndex = type;
        if (vkAllocateMemory(device, &alloc, nullptr, &imageMemory) != VK_SUCCESS) {
            setError(error, "failed to allocate image memory");
            return false;
        }
        vkBindImageMemory(device, image, imageMemory, 0);
        VkImageViewCreateInfo viewInfo{};
        viewInfo.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
        viewInfo.image = image;
        viewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
        viewInfo.format = VK_FORMAT_R8G8B8A8_UNORM;
        viewInfo.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        viewInfo.subresourceRange.levelCount = 1;
        viewInfo.subresourceRange.layerCount = 1;
        if (vkCreateImageView(device, &viewInfo, nullptr, &imageView) != VK_SUCCESS) {
            setError(error, "failed to create image view");
            return false;
        }
        return true;
    }

    bool uploadCpuImage(const QImage& frame, double* uploadMs, std::string* error)
    {
        const QImage rgba = frame.convertToFormat(QImage::Format_RGBA8888);
        if (!ensureResources(rgba.size(), tensorBytes, error)) {
            return false;
        }
        const auto start = std::chrono::steady_clock::now();
        void* mapped = nullptr;
        const VkDeviceSize bytes = static_cast<VkDeviceSize>(rgba.width()) * rgba.height() * 4;
        vkMapMemory(device, stagingMemory, 0, bytes, 0, &mapped);
        for (int y = 0; y < rgba.height(); ++y) {
            std::memcpy(static_cast<unsigned char*>(mapped) + y * rgba.width() * 4,
                        rgba.constScanLine(y),
                        static_cast<size_t>(rgba.width() * 4));
        }
        vkUnmapMemory(device, stagingMemory);

        vkResetFences(device, 1, &fence);
        vkResetCommandBuffer(commandBuffer, 0);
        VkCommandBufferBeginInfo begin{};
        begin.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
        begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        vkBeginCommandBuffer(commandBuffer, &begin);
        transition(imageLayout, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
        VkBufferImageCopy copy{};
        copy.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        copy.imageSubresource.layerCount = 1;
        copy.imageExtent = {static_cast<uint32_t>(rgba.width()), static_cast<uint32_t>(rgba.height()), 1};
        vkCmdCopyBufferToImage(commandBuffer, stagingBuffer, image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &copy);
        transition(VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
        vkEndCommandBuffer(commandBuffer);
        VkSubmitInfo submit{};
        submit.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
        submit.commandBufferCount = 1;
        submit.pCommandBuffers = &commandBuffer;
        vkQueueSubmit(queue, 1, &submit, fence);
        vkWaitForFences(device, 1, &fence, VK_TRUE, 5'000'000'000ull);
        imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        const auto end = std::chrono::steady_clock::now();
        if (uploadMs) *uploadMs = std::chrono::duration<double, std::milli>(end - start).count();
        return true;
    }

    void transition(VkImageLayout oldLayout, VkImageLayout newLayout)
    {
        VkImageMemoryBarrier barrier{};
        barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
        barrier.oldLayout = oldLayout;
        barrier.newLayout = newLayout;
        barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        barrier.image = image;
        barrier.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        barrier.subresourceRange.levelCount = 1;
        barrier.subresourceRange.layerCount = 1;
        VkPipelineStageFlags src = VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT;
        VkPipelineStageFlags dst = VK_PIPELINE_STAGE_TRANSFER_BIT;
        if (oldLayout == VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL) {
            barrier.srcAccessMask = VK_ACCESS_SHADER_READ_BIT;
            src = VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT;
        }
        if (newLayout == VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL) {
            barrier.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
            dst = VK_PIPELINE_STAGE_TRANSFER_BIT;
        } else if (newLayout == VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL) {
            barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
            barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
            src = VK_PIPELINE_STAGE_TRANSFER_BIT;
            dst = VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT;
        }
        vkCmdPipelineBarrier(commandBuffer, src, dst, 0, 0, nullptr, 0, nullptr, 1, &barrier);
    }

    jcut::vulkan_detector::VulkanDeviceContext detectorContext() const
    {
        return {physicalDevice, device, queue, queueFamilyIndex};
    }

    VkInstance instance = VK_NULL_HANDLE;
    VkPhysicalDevice physicalDevice = VK_NULL_HANDLE;
    VkDevice device = VK_NULL_HANDLE;
    VkQueue queue = VK_NULL_HANDLE;
    uint32_t queueFamilyIndex = UINT32_MAX;
    VkCommandPool commandPool = VK_NULL_HANDLE;
    VkCommandBuffer commandBuffer = VK_NULL_HANDLE;
    VkFence fence = VK_NULL_HANDLE;
    VkImage image = VK_NULL_HANDLE;
    VkDeviceMemory imageMemory = VK_NULL_HANDLE;
    VkImageView imageView = VK_NULL_HANDLE;
    VkImageLayout imageLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    QSize imageSize;
    VkBuffer stagingBuffer = VK_NULL_HANDLE;
    VkDeviceMemory stagingMemory = VK_NULL_HANDLE;
    VkDeviceSize stagingSize = 0;
    VkBuffer tensorBuffer = VK_NULL_HANDLE;
    VkDeviceMemory tensorMemory = VK_NULL_HANDLE;
    VkDeviceSize tensorSize = 0;
    VkDeviceSize tensorBytes = 0;
    VkBuffer detectionBuffer = VK_NULL_HANDLE;
    VkDeviceMemory detectionMemory = VK_NULL_HANDLE;
    VkDeviceSize detectionSize = 0;
    int maxDetections = 256;
};

} // namespace

int main(int argc, char** argv)
{
    Options options;
    if (!parseArgs(argc, argv, &options)) return 2;
    if (!std::filesystem::exists(options.videoPath)) {
        std::cerr << "Video not found: " << options.videoPath << "\n";
        return 2;
    }
    editor::setDebugDecodePreference(editor::DecodePreference::Software);
    const TimelineClip clip = makeClip(options);
    const QString playbackPath = playbackMediaPathForClip(clip);
    editor::DecoderContext decoder(playbackPath);
    if (!decoder.initialize()) {
        std::cerr << "DecoderContext failed to initialize: " << playbackPath.toStdString() << "\n";
        return 2;
    }

    VulkanBenchContext vk;
    std::string error;
    if (!vk.initialize(&error)) {
        std::cerr << error << "\n";
        return 2;
    }
    jcut::vulkan_detector::VulkanZeroCopyFaceDetector detector;
    if (!detector.initialize(vk.detectorContext(), nullptr)) {
        std::cerr << "failed to initialize JCut zero-copy Vulkan detector preprocessing\n";
        return 2;
    }
    vk.tensorBytes = detector.tensorSpec().byteSize();

    int decoded = 0;
    int processed = 0;
    int cpuFrames = 0;
    int hardwareFrames = 0;
    double uploadTotalMs = 0.0;
    double preprocessTotalMs = 0.0;
    const auto wallStart = std::chrono::steady_clock::now();
    for (int frameNumber = 0; frameNumber < options.maxFrames; ++frameNumber) {
        editor::FrameHandle frame = decoder.decodeFrame(frameNumber);
        if (frame.isNull()) break;
        ++decoded;
        if (frame.hasCpuImage()) ++cpuFrames;
        if (frame.hasHardwareFrame() || frame.hasGpuTexture()) ++hardwareFrames;
        if ((frameNumber % options.stride) != 0) continue;
        if (!frame.hasCpuImage()) {
            std::cout << "frame=" << frameNumber << " skipped_non_cpu=1\n";
            continue;
        }
        double uploadMs = 0.0;
        if (!vk.uploadCpuImage(frame.cpuImage(), &uploadMs, &error)) {
            std::cerr << error << "\n";
            return 2;
        }
        jcut::vulkan_detector::VulkanExternalImage source;
        source.imageView = vk.imageView;
        source.imageLayout = vk.imageLayout;
        source.size = vk.imageSize;
        jcut::vulkan_detector::VulkanTensorBuffer tensor{vk.tensorBuffer, vk.tensorBytes};
        const auto preStart = std::chrono::steady_clock::now();
        QString preprocessError;
        if (!detector.preprocessToTensor(source, tensor, &preprocessError)) {
            std::cerr << preprocessError.toStdString() << "\n";
            return 2;
        }
        jcut::vulkan_detector::VulkanTensorBuffer detectionOutput{vk.detectionBuffer, vk.detectionSize};
        if (!detector.inferFromTensor(tensor, detectionOutput, vk.maxDetections, 0.28f, &preprocessError)) {
            std::cerr << preprocessError.toStdString() << "\n";
            return 2;
        }
        const auto preEnd = std::chrono::steady_clock::now();
        const double preprocessMs = std::chrono::duration<double, std::milli>(preEnd - preStart).count();
        void* mappedDetections = nullptr;
        uint32_t detectionCount = 0;
        if (vkMapMemory(vk.device, vk.detectionMemory, 0, vk.detectionSize, 0, &mappedDetections) == VK_SUCCESS) {
            detectionCount = qMin<uint32_t>(*static_cast<const uint32_t*>(mappedDetections),
                                            static_cast<uint32_t>(vk.maxDetections));
            vkUnmapMemory(vk.device, vk.detectionMemory);
        }
        uploadTotalMs += uploadMs;
        preprocessTotalMs += preprocessMs;
        ++processed;
        std::cout << "frame=" << frameNumber
                  << " upload_ms=" << std::fixed << std::setprecision(3) << uploadMs
                  << " preprocess_ms=" << preprocessMs
                  << " detections=" << detectionCount
                  << " preprocess_zero_copy=1 decode_zero_copy=0\n";
    }
    const auto wallEnd = std::chrono::steady_clock::now();
    const bool decodeZeroCopy = hardwareFrames > 0 && cpuFrames == 0;
    const bool preprocessZeroCopy = processed > 0;
    const bool endToEndZeroCopy = decodeZeroCopy && preprocessZeroCopy;
    std::cout << "summary backend=jcut-vulkan-zero-copy-preprocess"
              << " requested_video=" << options.videoPath
              << " playback_path=" << playbackPath.toStdString()
              << " decoder_path=" << decoder.info().decodePath.toStdString()
              << " interop_path=" << decoder.info().interopPath.toStdString()
              << " proxy_active=" << (playbackPath != QString::fromStdString(options.videoPath) ? 1 : 0)
              << " decoded=" << decoded
              << " processed=" << processed
              << " cpu_frames=" << cpuFrames
              << " hardware_frames=" << hardwareFrames
              << " avg_upload_ms=" << (processed ? uploadTotalMs / processed : 0.0)
              << " avg_preprocess_ms=" << (processed ? preprocessTotalMs / processed : 0.0)
              << " wall_sec=" << std::chrono::duration<double>(wallEnd - wallStart).count()
              << " decode_zero_copy=" << (decodeZeroCopy ? 1 : 0)
              << " preprocess_zero_copy=" << (preprocessZeroCopy ? 1 : 0)
              << " end_to_end_zero_copy=" << (endToEndZeroCopy ? 1 : 0)
              << "\n";
    if (options.requireZeroCopy && !endToEndZeroCopy) {
        std::cerr << "required end-to-end zero-copy was not used; decode still produced CPU frames\n";
        return 3;
    }
    return processed > 0 ? 0 : 1;
}
