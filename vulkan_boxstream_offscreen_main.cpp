#include "boxstream_generation.h"
#include "boxstream_runtime.h"
#include "decoder_context.h"
#include "debug_controls.h"
#include "editor_shared.h"
#include "frame_handle.h"

#include <QDir>
#include <QDataStream>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QApplication>
#include <QImage>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLabel>
#include <QLocalSocket>
#include <QPainter>
#include <QPixmap>
#include <QString>
#include <QtGlobal>

#include <vulkan/vulkan.h>

#include <algorithm>
#include <chrono>
#include <cstring>
#include <cstdlib>
#include <iomanip>
#include <iostream>
#include <string>
#include <vector>

#if JCUT_HAVE_OPENCV
#include <opencv2/imgproc.hpp>
#include <opencv2/objdetect.hpp>
#endif

namespace {

struct Options {
    QString videoPath = QStringLiteral("/mnt/Cancer/PanelVid2TikTok/Politics/YTDown.com_YouTube_Meet-the-Candidates-for-Baltimore-County_Media_Hho5MORgIj8_001_1080p.mp4");
    QString outputDir = QStringLiteral("testbench_assets/vulkan_boxstream_offscreen");
    int maxFrames = 240;
    int stride = 12;
    int maxDetections = 256;
    int previewFrames = 24;
    float threshold = 0.28f;
    QString previewSocket;
    bool livePreview = true;
    bool writePreviewFiles = false;
    editor::DecodePreference decodePreference = editor::DecodePreference::Software;
};

struct Detection {
    QRectF box;
    float confidence = 0.0f;
};

struct Track {
    int id = -1;
    QRectF box;
    int lastFrame = -1;
    QJsonArray detections;
};

void usage(const char* argv0)
{
    std::cerr << "Usage: " << argv0
              << " [video] [--out-dir DIR] [--max-frames N] [--stride N]"
              << " [--threshold F] [--preview-frames N]"
              << " [--no-preview-window] [--preview-files]"
              << " [--decode software|hardware|auto|hardware_zero_copy]\n";
}

bool parseArgs(int argc, char** argv, Options* options)
{
    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        auto next = [&](const char* name) -> const char* {
            if (i + 1 >= argc) {
                std::cerr << "Missing value for " << name << "\n";
                return nullptr;
            }
            return argv[++i];
        };
        if (arg == "--help" || arg == "-h") {
            usage(argv[0]);
            return false;
        }
        if (arg == "--out-dir") {
            const char* v = next("--out-dir");
            if (!v) return false;
            options->outputDir = QString::fromLocal8Bit(v);
        } else if (arg == "--max-frames") {
            const char* v = next("--max-frames");
            if (!v) return false;
            options->maxFrames = std::max(1, std::atoi(v));
        } else if (arg == "--stride") {
            const char* v = next("--stride");
            if (!v) return false;
            options->stride = std::max(1, std::atoi(v));
        } else if (arg == "--threshold") {
            const char* v = next("--threshold");
            if (!v) return false;
            options->threshold = std::clamp(static_cast<float>(std::atof(v)), 0.0f, 1.0f);
        } else if (arg == "--preview-frames") {
            const char* v = next("--preview-frames");
            if (!v) return false;
            options->previewFrames = std::max(0, std::atoi(v));
        } else if (arg == "--preview-socket") {
            const char* v = next("--preview-socket");
            if (!v) return false;
            options->previewSocket = QString::fromLocal8Bit(v);
        } else if (arg == "--no-preview-window") {
            options->livePreview = false;
        } else if (arg == "--preview-files") {
            options->writePreviewFiles = true;
        } else if (arg == "--no-preview-files") {
            options->writePreviewFiles = false;
        } else if (arg == "--decode") {
            const char* v = next("--decode");
            if (!v) return false;
            editor::DecodePreference preference = editor::DecodePreference::Software;
            if (!editor::parseDecodePreference(QString::fromLocal8Bit(v), &preference)) {
                std::cerr << "Invalid --decode value: " << v << "\n";
                return false;
            }
            options->decodePreference = preference;
        } else if (!arg.empty() && arg[0] == '-') {
            std::cerr << "Unknown argument: " << arg << "\n";
            return false;
        } else {
            options->videoPath = QString::fromLocal8Bit(arg.c_str());
        }
    }
    return true;
}

uint32_t findMemoryType(VkPhysicalDevice physicalDevice,
                        uint32_t typeBits,
                        VkMemoryPropertyFlags properties)
{
    VkPhysicalDeviceMemoryProperties memProps{};
    vkGetPhysicalDeviceMemoryProperties(physicalDevice, &memProps);
    for (uint32_t i = 0; i < memProps.memoryTypeCount; ++i) {
        if ((typeBits & (1u << i)) &&
            (memProps.memoryTypes[i].propertyFlags & properties) == properties) {
            return i;
        }
    }
    return UINT32_MAX;
}

class VulkanHarnessContext {
public:
    ~VulkanHarnessContext() { release(); }

    bool initialize(QString* error)
    {
        VkApplicationInfo app{};
        app.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
        app.pApplicationName = "jcut-vulkan-boxstream-offscreen";
        app.apiVersion = VK_API_VERSION_1_1;

        VkInstanceCreateInfo instanceInfo{};
        instanceInfo.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
        instanceInfo.pApplicationInfo = &app;
        if (vkCreateInstance(&instanceInfo, nullptr, &instance) != VK_SUCCESS) {
            if (error) *error = QStringLiteral("Failed to create Vulkan instance.");
            return false;
        }

        uint32_t deviceCount = 0;
        vkEnumeratePhysicalDevices(instance, &deviceCount, nullptr);
        if (deviceCount == 0) {
            if (error) *error = QStringLiteral("No Vulkan physical devices found.");
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
            if (error) *error = QStringLiteral("No Vulkan compute queue found.");
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
            if (error) *error = QStringLiteral("Failed to create Vulkan device.");
            return false;
        }
        vkGetDeviceQueue(device, queueFamilyIndex, 0, &queue);

        VkCommandPoolCreateInfo poolInfo{};
        poolInfo.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
        poolInfo.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
        poolInfo.queueFamilyIndex = queueFamilyIndex;
        if (vkCreateCommandPool(device, &poolInfo, nullptr, &commandPool) != VK_SUCCESS) {
            if (error) *error = QStringLiteral("Failed to create command pool.");
            return false;
        }

        VkCommandBufferAllocateInfo allocInfo{};
        allocInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
        allocInfo.commandPool = commandPool;
        allocInfo.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        allocInfo.commandBufferCount = 1;
        if (vkAllocateCommandBuffers(device, &allocInfo, &commandBuffer) != VK_SUCCESS) {
            if (error) *error = QStringLiteral("Failed to allocate command buffer.");
            return false;
        }

        VkFenceCreateInfo fenceInfo{};
        fenceInfo.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
        if (vkCreateFence(device, &fenceInfo, nullptr, &fence) != VK_SUCCESS) {
            if (error) *error = QStringLiteral("Failed to create fence.");
            return false;
        }
        return true;
    }

    bool attachExternalDevice(const jcut::vulkan_detector::VulkanDeviceContext& context, QString* error)
    {
        release();
        if (context.physicalDevice == VK_NULL_HANDLE ||
            context.device == VK_NULL_HANDLE ||
            context.queue == VK_NULL_HANDLE ||
            context.queueFamilyIndex == UINT32_MAX) {
            if (error) *error = QStringLiteral("Invalid external Vulkan detector context.");
            return false;
        }
        ownsInstanceAndDevice = false;
        physicalDevice = context.physicalDevice;
        device = context.device;
        queue = context.queue;
        queueFamilyIndex = context.queueFamilyIndex;
        return true;
    }

    void release()
    {
        if (device) {
            vkDeviceWaitIdle(device);
            if (fence) vkDestroyFence(device, fence, nullptr);
            if (commandPool) vkDestroyCommandPool(device, commandPool, nullptr);
            if (imageView) vkDestroyImageView(device, imageView, nullptr);
            if (image) vkDestroyImage(device, image, nullptr);
            if (imageMemory) vkFreeMemory(device, imageMemory, nullptr);
            destroyBuffer(stagingBuffer, stagingMemory);
            destroyBuffer(tensorBuffer, tensorMemory);
            destroyBuffer(detectionBuffer, detectionMemory);
            if (ownsInstanceAndDevice) {
                vkDestroyDevice(device, nullptr);
            }
        }
        if (ownsInstanceAndDevice && instance) {
            vkDestroyInstance(instance, nullptr);
        }
        instance = VK_NULL_HANDLE;
        physicalDevice = VK_NULL_HANDLE;
        device = VK_NULL_HANDLE;
        queue = VK_NULL_HANDLE;
        ownsInstanceAndDevice = true;
    }

    bool ensureDetectorBuffers(VkDeviceSize tensorBytesIn, int maxDetectionsIn, QString* error)
    {
        tensorBytes = tensorBytesIn;
        maxDetections = maxDetectionsIn;
        if (tensorBytes > tensorSize &&
            !createBuffer(tensorBytes,
                          VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
                          VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT,
                          &tensorBuffer,
                          &tensorMemory,
                          &tensorSize,
                          error)) {
            return false;
        }
        const VkDeviceSize detectionBytes = static_cast<VkDeviceSize>(16 + maxDetections * 32);
        if (detectionBytes > detectionSize &&
            !createBuffer(detectionBytes,
                          VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                          VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                          &detectionBuffer,
                          &detectionMemory,
                          &detectionSize,
                          error)) {
            return false;
        }
        return true;
    }

    bool ensureResources(const QSize& size, VkDeviceSize tensorBytes, int maxDetections, QString* error)
    {
        const VkDeviceSize stagingBytes = static_cast<VkDeviceSize>(size.width()) * size.height() * 4;
        if (stagingBytes > stagingSize &&
            !createBuffer(stagingBytes,
                          VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
                          VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                          &stagingBuffer,
                          &stagingMemory,
                          &stagingSize,
                          error)) {
            return false;
        }
        if (tensorBytes > tensorSize &&
            !createBuffer(tensorBytes,
                          VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
                          VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT,
                          &tensorBuffer,
                          &tensorMemory,
                          &tensorSize,
                          error)) {
            return false;
        }
        const VkDeviceSize detectionBytes = static_cast<VkDeviceSize>(16 + maxDetections * 32);
        if (detectionBytes > detectionSize &&
            !createBuffer(detectionBytes,
                          VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                          VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                          &detectionBuffer,
                          &detectionMemory,
                          &detectionSize,
                          error)) {
            return false;
        }
        if (image && imageSize == size) {
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

        VkImageCreateInfo imageInfo{};
        imageInfo.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
        imageInfo.imageType = VK_IMAGE_TYPE_2D;
        imageInfo.extent = {static_cast<uint32_t>(size.width()), static_cast<uint32_t>(size.height()), 1};
        imageInfo.mipLevels = 1;
        imageInfo.arrayLayers = 1;
        imageInfo.format = VK_FORMAT_R8G8B8A8_UNORM;
        imageInfo.tiling = VK_IMAGE_TILING_OPTIMAL;
        imageInfo.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        imageInfo.usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
        imageInfo.samples = VK_SAMPLE_COUNT_1_BIT;
        imageInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
        if (vkCreateImage(device, &imageInfo, nullptr, &image) != VK_SUCCESS) {
            if (error) *error = QStringLiteral("Failed to create Vulkan image.");
            return false;
        }
        VkMemoryRequirements req{};
        vkGetImageMemoryRequirements(device, image, &req);
        const uint32_t type = findMemoryType(physicalDevice, req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
        if (type == UINT32_MAX) {
            if (error) *error = QStringLiteral("No Vulkan image memory type.");
            return false;
        }
        VkMemoryAllocateInfo alloc{};
        alloc.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
        alloc.allocationSize = req.size;
        alloc.memoryTypeIndex = type;
        if (vkAllocateMemory(device, &alloc, nullptr, &imageMemory) != VK_SUCCESS) {
            if (error) *error = QStringLiteral("Failed to allocate Vulkan image memory.");
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
            if (error) *error = QStringLiteral("Failed to create Vulkan image view.");
            return false;
        }
        return true;
    }

    bool uploadCpuImage(const QImage& imageIn, double* uploadMs, QString* error)
    {
        const QImage rgba = imageIn.convertToFormat(QImage::Format_RGBA8888);
        if (!ensureResources(rgba.size(), tensorBytes, maxDetections, error)) {
            return false;
        }
        QElapsedTimer timer;
        timer.start();
        void* mapped = nullptr;
        const VkDeviceSize bytes = static_cast<VkDeviceSize>(rgba.width()) * rgba.height() * 4;
        if (vkMapMemory(device, stagingMemory, 0, bytes, 0, &mapped) != VK_SUCCESS) {
            if (error) *error = QStringLiteral("Failed to map staging memory.");
            return false;
        }
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
        if (uploadMs) *uploadMs = static_cast<double>(timer.nsecsElapsed()) / 1'000'000.0;
        return true;
    }

    jcut::vulkan_detector::VulkanDeviceContext detectorContext() const
    {
        return {physicalDevice, device, queue, queueFamilyIndex};
    }

    bool createBuffer(VkDeviceSize bytes,
                      VkBufferUsageFlags usage,
                      VkMemoryPropertyFlags properties,
                      VkBuffer* buffer,
                      VkDeviceMemory* memory,
                      VkDeviceSize* storedSize,
                      QString* error)
    {
        destroyBuffer(*buffer, *memory);
        VkBufferCreateInfo info{};
        info.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
        info.size = bytes;
        info.usage = usage;
        info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
        if (vkCreateBuffer(device, &info, nullptr, buffer) != VK_SUCCESS) {
            if (error) *error = QStringLiteral("Failed to create Vulkan buffer.");
            return false;
        }
        VkMemoryRequirements req{};
        vkGetBufferMemoryRequirements(device, *buffer, &req);
        const uint32_t type = findMemoryType(physicalDevice, req.memoryTypeBits, properties);
        if (type == UINT32_MAX) {
            if (error) *error = QStringLiteral("No Vulkan buffer memory type.");
            return false;
        }
        VkMemoryAllocateInfo alloc{};
        alloc.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
        alloc.allocationSize = req.size;
        alloc.memoryTypeIndex = type;
        if (vkAllocateMemory(device, &alloc, nullptr, memory) != VK_SUCCESS) {
            if (error) *error = QStringLiteral("Failed to allocate Vulkan buffer memory.");
            return false;
        }
        vkBindBufferMemory(device, *buffer, *memory, 0);
        *storedSize = bytes;
        return true;
    }

    void destroyBuffer(VkBuffer& buffer, VkDeviceMemory& memory)
    {
        if (buffer) vkDestroyBuffer(device, buffer, nullptr);
        if (memory) vkFreeMemory(device, memory, nullptr);
        buffer = VK_NULL_HANDLE;
        memory = VK_NULL_HANDLE;
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
        } else if (newLayout == VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL) {
            barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
            barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
            src = VK_PIPELINE_STAGE_TRANSFER_BIT;
            dst = VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT;
        }
        vkCmdPipelineBarrier(commandBuffer, src, dst, 0, 0, nullptr, 0, nullptr, 1, &barrier);
    }

    VkInstance instance = VK_NULL_HANDLE;
    bool ownsInstanceAndDevice = true;
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

double iou(const QRectF& a, const QRectF& b)
{
    const QRectF ix = a.intersected(b);
    const double inter = ix.width() * ix.height();
    const double uni = a.width() * a.height() + b.width() * b.height() - inter;
    return uni > 0.0 ? inter / uni : 0.0;
}

QVector<Detection> readDetections(VulkanHarnessContext& vk, int imageWidth, int imageHeight)
{
    QVector<Detection> out;
    void* mapped = nullptr;
    if (vkMapMemory(vk.device, vk.detectionMemory, 0, vk.detectionSize, 0, &mapped) != VK_SUCCESS) {
        return out;
    }
    const auto* bytes = static_cast<const unsigned char*>(mapped);
    const uint32_t count = qMin<uint32_t>(*reinterpret_cast<const uint32_t*>(bytes),
                                          static_cast<uint32_t>(vk.maxDetections));
    const float* det = reinterpret_cast<const float*>(bytes + 16);
    out.reserve(static_cast<int>(count));
    for (uint32_t i = 0; i < count; ++i) {
        const float x = det[i * 8 + 0];
        const float y = det[i * 8 + 1];
        const float w = det[i * 8 + 2];
        const float h = det[i * 8 + 3];
        const float c = det[i * 8 + 4];
        QRectF box(x * imageWidth, y * imageHeight, w * imageWidth, h * imageHeight);
        box = box.intersected(QRectF(0, 0, imageWidth, imageHeight));
        if (box.width() >= 8.0 && box.height() >= 8.0) {
            out.push_back({box, c});
        }
    }
    vkUnmapMemory(vk.device, vk.detectionMemory);
    std::sort(out.begin(), out.end(), [](const Detection& a, const Detection& b) {
        return a.confidence > b.confidence;
    });
    return out;
}

void updateTracks(QVector<Track>* tracks,
                  const QVector<Detection>& detections,
                  int frameNumber,
                  const QSize& frameSize)
{
    QVector<bool> used(tracks->size(), false);
    for (const Detection& det : detections) {
        int best = -1;
        double bestIou = 0.0;
        for (int i = 0; i < tracks->size(); ++i) {
            if (used.at(i) || frameNumber - tracks->at(i).lastFrame > 48) {
                continue;
            }
            const double score = iou(tracks->at(i).box, det.box);
            if (score > bestIou) {
                bestIou = score;
                best = i;
            }
        }
        if (best < 0 || bestIou < 0.12) {
            Track track;
            track.id = tracks->size();
            track.box = det.box;
            track.lastFrame = frameNumber;
            tracks->push_back(track);
            best = tracks->size() - 1;
            used.push_back(false);
        } else {
            (*tracks)[best].box = QRectF(
                ((*tracks)[best].box.x() * 0.65) + (det.box.x() * 0.35),
                ((*tracks)[best].box.y() * 0.65) + (det.box.y() * 0.35),
                ((*tracks)[best].box.width() * 0.65) + (det.box.width() * 0.35),
                ((*tracks)[best].box.height() * 0.65) + (det.box.height() * 0.35));
            (*tracks)[best].lastFrame = frameNumber;
        }
        used[best] = true;
        const double x = qBound(0.0, det.box.center().x() / qMax(1, frameSize.width()), 1.0);
        const double y = qBound(0.0, det.box.center().y() / qMax(1, frameSize.height()), 1.0);
        const double box = qBound(0.01,
                                  qMax(det.box.width(), det.box.height()) /
                                      static_cast<double>(qMax(1, qMin(frameSize.width(), frameSize.height()))),
                                  1.0);
        (*tracks)[best].detections.append(QJsonObject{
            {QStringLiteral("frame"), frameNumber},
            {QStringLiteral("x"), x},
            {QStringLiteral("y"), y},
            {QStringLiteral("box"), box},
            {QStringLiteral("score"), det.confidence}
        });
    }
}

QImage buildPreview(QImage image,
                    const QVector<Track>& tracks,
                    const QVector<Detection>& detections)
{
    QVector<QRect> boxes;
    boxes.reserve(detections.size());
    for (const Detection& detection : detections) {
        boxes.push_back(detection.box.toAlignedRect());
    }
    return jcut::boxstream::buildScanPreview(image, boxes, tracks.size());
}

bool sendPreviewFrame(QLocalSocket* socket, const QImage& image)
{
    if (!socket || socket->state() != QLocalSocket::ConnectedState || image.isNull()) {
        return false;
    }
    const QImage argb = image.convertToFormat(QImage::Format_ARGB32_Premultiplied);
    QByteArray payload;
    payload.resize(argb.sizeInBytes());
    std::memcpy(payload.data(), argb.constBits(), static_cast<size_t>(payload.size()));
    QDataStream stream(socket);
    stream.setVersion(QDataStream::Qt_6_0);
    stream << quint32(argb.width()) << quint32(argb.height()) << quint32(payload.size());
    const qint64 written = socket->write(payload);
    socket->flush();
    socket->waitForBytesWritten(100);
    return written == payload.size();
}

bool writeJson(const QString& path, const QJsonObject& object)
{
    QFile file(path);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        return false;
    }
    file.write(QJsonDocument(object).toJson(QJsonDocument::Indented));
    return true;
}

} // namespace

int main(int argc, char** argv)
{
    QApplication app(argc, argv);
    Options options;
    if (!parseArgs(argc, argv, &options)) {
        return 2;
    }
    if (!QFileInfo::exists(options.videoPath)) {
        std::cerr << "Video not found: " << options.videoPath.toStdString() << "\n";
        return 2;
    }
    QDir().mkpath(options.outputDir);
    const bool platformIsOffscreen =
        QString::fromLocal8Bit(qgetenv("QT_QPA_PLATFORM")).compare(QStringLiteral("offscreen"),
                                                                   Qt::CaseInsensitive) == 0;
    QLabel previewWindow;
    QLabel* previewWindowPtr = nullptr;
    if (options.livePreview && !platformIsOffscreen) {
        previewWindow.setWindowTitle(QStringLiteral("JCut Vulkan BoxStream Offscreen Preview"));
        previewWindow.setAlignment(Qt::AlignCenter);
        previewWindow.setMinimumSize(960, 540);
        previewWindow.setStyleSheet(QStringLiteral("background:#111; color:#ddd; border:1px solid #333;"));
        previewWindow.setText(QStringLiteral("Waiting for Vulkan BoxStream preview..."));
        previewWindow.show();
        previewWindowPtr = &previewWindow;
        app.processEvents();
    }

    editor::setDebugDecodePreference(options.decodePreference);
    editor::DecoderContext decoder(options.videoPath);
    if (!decoder.initialize()) {
        std::cerr << "Failed to initialize decoder: " << options.videoPath.toStdString() << "\n";
        return 2;
    }
    TimelineClip sourceClip;
    sourceClip.id = QStringLiteral("boxstream-offscreen-source");
    sourceClip.filePath = options.videoPath;
    sourceClip.mediaType = ClipMediaType::Video;
    sourceClip.videoEnabled = true;
    sourceClip.audioEnabled = false;
    sourceClip.hasAudio = false;
    sourceClip.startFrame = 0;
    sourceClip.sourceInFrame = 0;
    sourceClip.durationFrames = qMax<int64_t>(1, options.maxFrames);
    sourceClip.sourceDurationFrames = qMax<int64_t>(1, options.maxFrames);
    sourceClip.sourceFps = 30.0;
    sourceClip.playbackRate = 1.0;
    jcut::boxstream::VulkanFrameProvider appFrameProvider;
    const QSize renderSize = decoder.info().frameSize.isValid()
        ? decoder.info().frameSize
        : QSize(1920, 1080);
    QLocalSocket previewSocket;
    QLocalSocket* previewSocketPtr = nullptr;
    if (!options.previewSocket.isEmpty()) {
        previewSocket.connectToServer(options.previewSocket);
        if (previewSocket.waitForConnected(3000)) {
            previewSocketPtr = &previewSocket;
        } else {
            std::cerr << "Failed to connect preview socket: "
                      << options.previewSocket.toStdString() << "\n";
        }
    }

    QString error;
#if !JCUT_HAVE_OPENCV
    std::cerr << "OpenCV is not enabled in this build; Generate BoxStream native scan is unavailable.\n";
    return 2;
#else
    cv::CascadeClassifier faceCascade;
    cv::CascadeClassifier faceCascadeAlt;
    cv::CascadeClassifier faceCascadeProfile;
    const QString cascadePath = jcut::boxstream::findCascadeFile(QStringLiteral("haarcascade_frontalface_default.xml"));
    if (cascadePath.isEmpty() || !faceCascade.load(cascadePath.toStdString())) {
        std::cerr << "Failed to load Haar cascade for Generate BoxStream native scan.\n";
        return 2;
    }
    const QString altCascadePath = jcut::boxstream::findCascadeFile(QStringLiteral("haarcascade_frontalface_alt2.xml"));
    if (!altCascadePath.isEmpty()) {
        faceCascadeAlt.load(altCascadePath.toStdString());
    }
    const QString profileCascadePath = jcut::boxstream::findCascadeFile(QStringLiteral("haarcascade_profileface.xml"));
    if (!profileCascadePath.isEmpty()) {
        faceCascadeProfile.load(profileCascadePath.toStdString());
    }

    int decoded = 0;
    int processed = 0;
    int cpuFrames = 0;
    int hardwareFrames = 0;
    int totalDetections = 0;
    int previewWritten = 0;
    double renderDecodeMsTotal = 0.0;
    double renderCompositeMsTotal = 0.0;
    QVector<Track> tracks;
    QJsonArray frameRows;
    bool printedAppVulkanFailure = false;

    const auto wallStart = std::chrono::steady_clock::now();
    for (int frameNumber = 0; frameNumber < options.maxFrames; ++frameNumber) {
        ++decoded;
        if ((frameNumber % options.stride) != 0) {
            continue;
        }

        jcut::boxstream::VulkanFrameStats renderStats;
        QImage frameImage = jcut::boxstream::renderFrameWithVulkan(&appFrameProvider,
                                                                   sourceClip,
                                                                   options.videoPath,
                                                                   frameNumber,
                                                                   frameNumber,
                                                                   renderSize,
                                                                   &renderStats);
        const bool appVulkanFrame = !frameImage.isNull();
        if (appVulkanFrame) {
            ++hardwareFrames;
        } else {
            if (appFrameProvider.failed && !printedAppVulkanFailure) {
                std::cerr << "Application Vulkan frame path unavailable: "
                          << appFrameProvider.failureReason.toStdString() << "\n";
                printedAppVulkanFailure = true;
            }
            editor::FrameHandle frame = decoder.decodeFrame(frameNumber);
            if (frame.isNull()) {
                break;
            }
            if (frame.hasHardwareFrame() || frame.hasGpuTexture()) ++hardwareFrames;
            if (!frame.hasCpuImage()) {
                continue;
            }
            frameImage = frame.cpuImage();
            ++cpuFrames;
        }
        if (frameImage.isNull()) {
            continue;
        }

        const cv::Mat bgr = jcut::boxstream::qImageToBgrMat(frameImage);
        cv::Mat gray;
        cv::cvtColor(bgr, gray, cv::COLOR_BGR2GRAY);
        cv::equalizeHist(gray, gray);

        const int minSide = qMax(1, qMin(frameImage.width(), frameImage.height()));
        const double scaleFactor = 1.08;
        const int baseNeighbors = 5;
        const int minDivisor = 22;
        const double minWeight = 0.2;
        const cv::Size minFace(qMax(18, minSide / minDivisor), qMax(18, minSide / minDivisor));
        const cv::Size maxFace(qMax(minFace.width + 1, (minSide * 3) / 4),
                               qMax(minFace.height + 1, (minSide * 3) / 4));
        std::vector<jcut::boxstream::WeightedDetection> weightedDetections;
        auto runCascade = [&](cv::CascadeClassifier& cascade, int minNeighbors, double weightBias) {
            if (cascade.empty()) {
                return;
            }
            std::vector<cv::Rect> raw;
            std::vector<int> rejectLevels;
            std::vector<double> levelWeights;
            cascade.detectMultiScale(
                gray, raw, rejectLevels, levelWeights,
                scaleFactor, minNeighbors, cv::CASCADE_SCALE_IMAGE, minFace, maxFace, true);
            for (int i = 0; i < static_cast<int>(raw.size()); ++i) {
                const double weight = (i < static_cast<int>(levelWeights.size()) ? levelWeights[i] : 0.0) + weightBias;
                if (weight >= minWeight) {
                    weightedDetections.push_back({raw[i], weight});
                }
            }
        };
        runCascade(faceCascade, baseNeighbors, 0.0);
        runCascade(faceCascadeAlt, qMax(2, baseNeighbors - 1), -0.05);
        runCascade(faceCascadeProfile, qMax(2, baseNeighbors - 1), -0.10);

        const std::vector<jcut::boxstream::WeightedDetection> filtered =
            jcut::boxstream::filterAndSuppressDetections(std::move(weightedDetections), frameImage.size());
        QVector<Detection> detections;
        detections.reserve(static_cast<int>(filtered.size()));
        for (const jcut::boxstream::WeightedDetection& det : filtered) {
            detections.push_back({QRectF(det.box.x, det.box.y, det.box.width, det.box.height),
                                  static_cast<float>(det.weight)});
        }
        updateTracks(&tracks, detections, frameNumber, frameImage.size());

        ++processed;
        totalDetections += detections.size();
        renderDecodeMsTotal += renderStats.decodeMs;
        renderCompositeMsTotal += renderStats.compositeMs;
        frameRows.append(QJsonObject{
            {QStringLiteral("frame"), frameNumber},
            {QStringLiteral("detections"), detections.size()},
            {QStringLiteral("tracks"), tracks.size()},
            {QStringLiteral("app_vulkan_frame_path"), appVulkanFrame},
            {QStringLiteral("app_render_decode_ms"), renderStats.decodeMs},
            {QStringLiteral("app_render_texture_ms"), renderStats.textureMs},
            {QStringLiteral("app_render_composite_ms"), renderStats.compositeMs},
            {QStringLiteral("app_render_readback_ms"), renderStats.readbackMs},
            {QStringLiteral("generate_boxstream_native_hybrid_vulkan"), true}
        });

        const bool needsPreviewFrame =
            previewWindowPtr ||
            previewSocketPtr ||
            (options.writePreviewFiles && previewWritten < options.previewFrames);
        if (needsPreviewFrame) {
            const QImage preview = buildPreview(frameImage, tracks, detections);
            if (previewSocketPtr) {
                sendPreviewFrame(previewSocketPtr, preview);
            }
            if (previewWindowPtr && !preview.isNull()) {
                previewWindowPtr->setPixmap(QPixmap::fromImage(preview).scaled(
                    previewWindowPtr->size(),
                    Qt::KeepAspectRatio,
                    Qt::SmoothTransformation));
                previewWindowPtr->setWindowTitle(QStringLiteral(
                    "JCut Generate BoxStream Thin Wrapper - frame %1 tracks %2")
                    .arg(frameNumber)
                    .arg(tracks.size()));
                app.processEvents();
                if (!previewWindowPtr->isVisible()) {
                    options.livePreview = false;
                    previewWindowPtr = nullptr;
                }
            }
            if (options.writePreviewFiles && previewWritten < options.previewFrames) {
                const QString previewPath = QDir(options.outputDir).filePath(
                    QStringLiteral("preview_%1.png").arg(frameNumber, 6, 10, QChar('0')));
                preview.save(previewPath);
                ++previewWritten;
            }
        }
        std::cout << "frame=" << frameNumber
                  << " detections=" << detections.size()
                  << " tracks=" << tracks.size()
                  << " app_vulkan_frame_path=" << (appVulkanFrame ? 1 : 0)
                  << " render_decode_ms=" << renderStats.decodeMs
                  << " render_texture_ms=" << renderStats.textureMs
                  << " render_composite_ms=" << renderStats.compositeMs
                  << " render_readback_ms=" << renderStats.readbackMs
                  << " generate_boxstream_native_hybrid_vulkan=1\n";
    }
    const auto wallEnd = std::chrono::steady_clock::now();

#endif
    QJsonArray trackRows;
    for (const Track& track : tracks) {
        trackRows.append(QJsonObject{
            {QStringLiteral("track_id"), track.id},
            {QStringLiteral("last_frame"), track.lastFrame},
            {QStringLiteral("length"), track.detections.size()},
            {QStringLiteral("detections"), track.detections}
        });
    }
    writeJson(QDir(options.outputDir).filePath(QStringLiteral("tracks.json")), QJsonObject{
        {QStringLiteral("schema"), QStringLiteral("jcut_boxstream_offscreen_tracks_v1")},
        {QStringLiteral("video"), options.videoPath},
        {QStringLiteral("backend"), QStringLiteral("native_hybrid_vulkan_v1")},
        {QStringLiteral("tracks"), trackRows},
        {QStringLiteral("frames"), frameRows}
    });
    const QJsonArray streams = jcut::boxstream::buildContinuityStreams(
        trackRows,
        QJsonObject{},
        QStringLiteral("native_hybrid_vulkan_v1"),
        false);
    const QJsonObject continuityRoot = jcut::boxstream::buildContinuityRoot(
        QStringLiteral("offscreen"),
        false,
        0,
        qMax(0, options.maxFrames - 1),
        streams);
    writeJson(QDir(options.outputDir).filePath(QStringLiteral("continuity_boxstream.json")), QJsonObject{
        {QStringLiteral("schema"), QStringLiteral("jcut_boxstream_v1")},
        {QStringLiteral("continuity_boxstreams_by_clip"), QJsonObject{
            {QStringLiteral("boxstream-offscreen-source"), continuityRoot}
        }}
    });

    const bool decodeZeroCopy = false;
    const QJsonObject summary{
        {QStringLiteral("video"), options.videoPath},
        {QStringLiteral("output_dir"), QDir(options.outputDir).absolutePath()},
        {QStringLiteral("backend"), QStringLiteral("native_hybrid_vulkan_v1")},
        {QStringLiteral("decoded_frames"), decoded},
        {QStringLiteral("processed_frames"), processed},
        {QStringLiteral("cpu_frames"), cpuFrames},
        {QStringLiteral("hardware_frames"), hardwareFrames},
        {QStringLiteral("app_vulkan_decode_path_frames"), hardwareFrames},
        {QStringLiteral("app_vulkan_frame_path_failed"), appFrameProvider.failed},
        {QStringLiteral("app_vulkan_frame_path_failure"), appFrameProvider.failureReason},
        {QStringLiteral("total_detections"), totalDetections},
        {QStringLiteral("track_count"), tracks.size()},
        {QStringLiteral("preview_frames_written"), previewWritten},
        {QStringLiteral("avg_render_decode_ms"), processed ? renderDecodeMsTotal / processed : 0.0},
        {QStringLiteral("avg_render_composite_ms"), processed ? renderCompositeMsTotal / processed : 0.0},
        {QStringLiteral("wall_sec"), std::chrono::duration<double>(wallEnd - wallStart).count()},
        {QStringLiteral("decode_zero_copy"), decodeZeroCopy},
        {QStringLiteral("generate_boxstream_native_hybrid_vulkan"), true},
        {QStringLiteral("zero_copy_note"), QStringLiteral("This wrapper follows the Generate BoxStream Native Production Hybrid (Vulkan Decode Path): frames are composed by the app Vulkan renderer, then materialized for the same OpenCV continuity detector/preview used by the program option.")}
    };
    writeJson(QDir(options.outputDir).filePath(QStringLiteral("summary.json")), summary);
    std::cout << "summary " << QJsonDocument(summary).toJson(QJsonDocument::Compact).constData() << "\n";
    return processed > 0 ? 0 : 1;
}
