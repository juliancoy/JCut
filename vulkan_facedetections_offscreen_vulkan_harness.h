#pragma once

#include "editor_shared.h"
#include "frame_handle.h"
#include "vulkan_detector_frame_handoff.h"
#include "vulkan_facedetections_offscreen_detection_filters.h"
#include "vulkan_res10_ncnn_face_detector.h"
#include "vulkan_scrfd_ncnn_face_detector.h"
#include "vulkan_zero_copy_face_detector.h"

#include <QImage>
#include <QSize>
#include <QString>
#include <QVector>
#include <QtGlobal>

#include <vulkan/vulkan.h>

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <future>
#include <vector>

#include <fcntl.h>
#include <unistd.h>

uint32_t findMemoryType(VkPhysicalDevice physicalDevice, uint32_t typeBits,
                        VkMemoryPropertyFlags properties);

class ScopedStderrSilencer {
public:
  explicit ScopedStderrSilencer(bool enabled) {
    if (!enabled) {
      return;
    }
    fflush(stderr);
    m_savedFd = dup(STDERR_FILENO);
    if (m_savedFd < 0) {
      return;
    }
    const int nullFd = open("/dev/null", O_WRONLY);
    if (nullFd < 0) {
      close(m_savedFd);
      m_savedFd = -1;
      return;
    }
    if (dup2(nullFd, STDERR_FILENO) < 0) {
      close(nullFd);
      close(m_savedFd);
      m_savedFd = -1;
      return;
    }
    close(nullFd);
  }

  ~ScopedStderrSilencer() {
    if (m_savedFd >= 0) {
      fflush(stderr);
      dup2(m_savedFd, STDERR_FILENO);
      close(m_savedFd);
    }
  }

  ScopedStderrSilencer(const ScopedStderrSilencer &) = delete;
  ScopedStderrSilencer &operator=(const ScopedStderrSilencer &) = delete;

private:
  int m_savedFd = -1;
};

class VulkanHarnessContext {
public:
  ~VulkanHarnessContext() { release(); }

  bool initialize(QString *error) {
    VkApplicationInfo app{};
    app.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
    app.pApplicationName = "jcut-dnn-facedetections-generator";
    app.apiVersion = VK_API_VERSION_1_1;

    VkInstanceCreateInfo instanceInfo{};
    instanceInfo.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
    instanceInfo.pApplicationInfo = &app;
    if (vkCreateInstance(&instanceInfo, nullptr, &instance) != VK_SUCCESS) {
      if (error)
        *error = QStringLiteral("Failed to create Vulkan instance.");
      return false;
    }

    uint32_t deviceCount = 0;
    vkEnumeratePhysicalDevices(instance, &deviceCount, nullptr);
    if (deviceCount == 0) {
      if (error)
        *error = QStringLiteral("No Vulkan physical devices found.");
      return false;
    }
    std::vector<VkPhysicalDevice> devices(deviceCount);
    vkEnumeratePhysicalDevices(instance, &deviceCount, devices.data());
    physicalDevice = devices.front();

    uint32_t queueFamilyCount = 0;
    vkGetPhysicalDeviceQueueFamilyProperties(physicalDevice, &queueFamilyCount,
                                             nullptr);
    std::vector<VkQueueFamilyProperties> families(queueFamilyCount);
    vkGetPhysicalDeviceQueueFamilyProperties(physicalDevice, &queueFamilyCount,
                                             families.data());
    for (uint32_t i = 0; i < queueFamilyCount; ++i) {
      if (families[i].queueFlags & VK_QUEUE_COMPUTE_BIT) {
        queueFamilyIndex = i;
        break;
      }
    }
    if (queueFamilyIndex == UINT32_MAX) {
      if (error)
        *error = QStringLiteral("No Vulkan compute queue found.");
      return false;
    }

    const float priority = 1.0f;
    VkDeviceQueueCreateInfo queueInfo{};
    queueInfo.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
    queueInfo.queueFamilyIndex = queueFamilyIndex;
    queueInfo.queueCount = 1;
    queueInfo.pQueuePriorities = &priority;

    uint32_t extensionCount = 0;
    vkEnumerateDeviceExtensionProperties(physicalDevice, nullptr,
                                         &extensionCount, nullptr);
    std::vector<VkExtensionProperties> extensionProperties(extensionCount);
    if (extensionCount) {
      vkEnumerateDeviceExtensionProperties(
          physicalDevice, nullptr, &extensionCount, extensionProperties.data());
    }
    auto hasDeviceExtension = [&](const char *name) {
      return std::any_of(extensionProperties.begin(), extensionProperties.end(),
                         [&](const VkExtensionProperties &ext) {
                           return std::strcmp(ext.extensionName, name) == 0;
                         });
    };
    std::vector<const char *> enabledExtensions;
    auto enableIfAvailable = [&](const char *name) {
      if (hasDeviceExtension(name)) {
        enabledExtensions.push_back(name);
      }
    };
    enableIfAvailable(VK_KHR_BIND_MEMORY_2_EXTENSION_NAME);
    enableIfAvailable(VK_KHR_EXTERNAL_MEMORY_EXTENSION_NAME);
#ifdef Q_OS_LINUX
    enableIfAvailable(VK_KHR_EXTERNAL_MEMORY_FD_EXTENSION_NAME);
#endif
    enableIfAvailable(VK_KHR_DESCRIPTOR_UPDATE_TEMPLATE_EXTENSION_NAME);
    enableIfAvailable(VK_KHR_GET_MEMORY_REQUIREMENTS_2_EXTENSION_NAME);
    enableIfAvailable(VK_KHR_MAINTENANCE1_EXTENSION_NAME);
    enableIfAvailable(VK_KHR_MAINTENANCE3_EXTENSION_NAME);

    VkDeviceCreateInfo deviceInfo{};
    deviceInfo.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
    deviceInfo.queueCreateInfoCount = 1;
    deviceInfo.pQueueCreateInfos = &queueInfo;
    deviceInfo.enabledExtensionCount =
        static_cast<uint32_t>(enabledExtensions.size());
    deviceInfo.ppEnabledExtensionNames =
        enabledExtensions.empty() ? nullptr : enabledExtensions.data();
    if (vkCreateDevice(physicalDevice, &deviceInfo, nullptr, &device) !=
        VK_SUCCESS) {
      if (error)
        *error = QStringLiteral("Failed to create Vulkan device.");
      return false;
    }
    vkGetDeviceQueue(device, queueFamilyIndex, 0, &queue);

    VkCommandPoolCreateInfo poolInfo{};
    poolInfo.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
    poolInfo.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    poolInfo.queueFamilyIndex = queueFamilyIndex;
    if (vkCreateCommandPool(device, &poolInfo, nullptr, &commandPool) !=
        VK_SUCCESS) {
      if (error)
        *error = QStringLiteral("Failed to create command pool.");
      return false;
    }

    VkCommandBufferAllocateInfo allocInfo{};
    allocInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    allocInfo.commandPool = commandPool;
    allocInfo.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    allocInfo.commandBufferCount = 1;
    if (vkAllocateCommandBuffers(device, &allocInfo, &commandBuffer) !=
        VK_SUCCESS) {
      if (error)
        *error = QStringLiteral("Failed to allocate command buffer.");
      return false;
    }

    VkFenceCreateInfo fenceInfo{};
    fenceInfo.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
    if (vkCreateFence(device, &fenceInfo, nullptr, &fence) != VK_SUCCESS) {
      if (error)
        *error = QStringLiteral("Failed to create fence.");
      return false;
    }
    return true;
  }

  bool attachExternalDevice(
      const jcut::vulkan_detector::VulkanDeviceContext &context,
      QString *error) {
    if (context.physicalDevice == VK_NULL_HANDLE ||
        context.device == VK_NULL_HANDLE || context.queue == VK_NULL_HANDLE ||
        context.queueFamilyIndex == UINT32_MAX) {
      if (error)
        *error = QStringLiteral("Invalid external Vulkan detector context.");
      return false;
    }
    if (physicalDevice == context.physicalDevice && device == context.device &&
        queue == context.queue &&
        queueFamilyIndex == context.queueFamilyIndex) {
      return true;
    }
    release();
    ownsInstanceAndDevice = false;
    physicalDevice = context.physicalDevice;
    device = context.device;
    queue = context.queue;
    queueFamilyIndex = context.queueFamilyIndex;
    return true;
  }

  void release() {
    if (device) {
      vkDeviceWaitIdle(device);
      if (fence)
        vkDestroyFence(device, fence, nullptr);
      if (commandPool)
        vkDestroyCommandPool(device, commandPool, nullptr);
      if (imageView)
        vkDestroyImageView(device, imageView, nullptr);
      if (image)
        vkDestroyImage(device, image, nullptr);
      if (imageMemory)
        vkFreeMemory(device, imageMemory, nullptr);
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

  bool ensureDetectorBuffers(VkDeviceSize tensorBytesIn, int maxDetectionsIn,
                             QString *error) {
    tensorBytes = tensorBytesIn;
    maxDetections = maxDetectionsIn;
    if (tensorBytes > tensorSize &&
        !createBuffer(tensorBytes,
                      VK_BUFFER_USAGE_STORAGE_BUFFER_BIT |
                          VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
                      VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, &tensorBuffer,
                      &tensorMemory, &tensorSize, error)) {
      return false;
    }
    const VkDeviceSize detectionBytes =
        static_cast<VkDeviceSize>(16 + maxDetections * 32);
    if (detectionBytes > detectionSize &&
        !createBuffer(detectionBytes,
                      VK_BUFFER_USAGE_STORAGE_BUFFER_BIT |
                          VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                      VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                          VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                      &detectionBuffer, &detectionMemory, &detectionSize,
                      error)) {
      return false;
    }
    return true;
  }

  bool ensureResources(const QSize &size, VkDeviceSize tensorBytes,
                       int maxDetections, QString *error) {
    const VkDeviceSize stagingBytes =
        static_cast<VkDeviceSize>(size.width()) * size.height() * 4;
    if (stagingBytes > stagingSize &&
        !createBuffer(stagingBytes, VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
                      VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                          VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                      &stagingBuffer, &stagingMemory, &stagingSize, error)) {
      return false;
    }
    if (tensorBytes > tensorSize &&
        !createBuffer(tensorBytes,
                      VK_BUFFER_USAGE_STORAGE_BUFFER_BIT |
                          VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
                      VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, &tensorBuffer,
                      &tensorMemory, &tensorSize, error)) {
      return false;
    }
    const VkDeviceSize detectionBytes =
        static_cast<VkDeviceSize>(16 + maxDetections * 32);
    if (detectionBytes > detectionSize &&
        !createBuffer(detectionBytes,
                      VK_BUFFER_USAGE_STORAGE_BUFFER_BIT |
                          VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                      VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                          VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                      &detectionBuffer, &detectionMemory, &detectionSize,
                      error)) {
      return false;
    }
    if (image && imageSize == size) {
      return true;
    }

    if (imageView)
      vkDestroyImageView(device, imageView, nullptr);
    if (image)
      vkDestroyImage(device, image, nullptr);
    if (imageMemory)
      vkFreeMemory(device, imageMemory, nullptr);
    imageView = VK_NULL_HANDLE;
    image = VK_NULL_HANDLE;
    imageMemory = VK_NULL_HANDLE;
    imageLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    imageSize = size;

    VkImageCreateInfo imageInfo{};
    imageInfo.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    imageInfo.imageType = VK_IMAGE_TYPE_2D;
    imageInfo.extent = {static_cast<uint32_t>(size.width()),
                        static_cast<uint32_t>(size.height()), 1};
    imageInfo.mipLevels = 1;
    imageInfo.arrayLayers = 1;
    imageInfo.format = VK_FORMAT_R8G8B8A8_UNORM;
    imageInfo.tiling = VK_IMAGE_TILING_OPTIMAL;
    imageInfo.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    imageInfo.usage =
        VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
    imageInfo.samples = VK_SAMPLE_COUNT_1_BIT;
    imageInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    if (vkCreateImage(device, &imageInfo, nullptr, &image) != VK_SUCCESS) {
      if (error)
        *error = QStringLiteral("Failed to create Vulkan image.");
      return false;
    }
    VkMemoryRequirements req{};
    vkGetImageMemoryRequirements(device, image, &req);
    const uint32_t type = findMemoryType(physicalDevice, req.memoryTypeBits,
                                         VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    if (type == UINT32_MAX) {
      if (error)
        *error = QStringLiteral("No Vulkan image memory type.");
      return false;
    }
    VkMemoryAllocateInfo alloc{};
    alloc.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    alloc.allocationSize = req.size;
    alloc.memoryTypeIndex = type;
    if (vkAllocateMemory(device, &alloc, nullptr, &imageMemory) != VK_SUCCESS) {
      if (error)
        *error = QStringLiteral("Failed to allocate Vulkan image memory.");
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
    if (vkCreateImageView(device, &viewInfo, nullptr, &imageView) !=
        VK_SUCCESS) {
      if (error)
        *error = QStringLiteral("Failed to create Vulkan image view.");
      return false;
    }
    return true;
  }

  bool uploadCpuImage(const QImage &imageIn, double *uploadMs, QString *error) {
    const QImage rgba = imageIn.convertToFormat(QImage::Format_RGBA8888);
    if (!ensureResources(rgba.size(), tensorBytes, maxDetections, error)) {
      return false;
    }
    QElapsedTimer timer;
    timer.start();
    void *mapped = nullptr;
    const VkDeviceSize bytes =
        static_cast<VkDeviceSize>(rgba.width()) * rgba.height() * 4;
    if (vkMapMemory(device, stagingMemory, 0, bytes, 0, &mapped) !=
        VK_SUCCESS) {
      if (error)
        *error = QStringLiteral("Failed to map staging memory.");
      return false;
    }
    for (int y = 0; y < rgba.height(); ++y) {
      std::memcpy(static_cast<unsigned char *>(mapped) + y * rgba.width() * 4,
                  rgba.constScanLine(y), static_cast<size_t>(rgba.width() * 4));
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
    copy.imageExtent = {static_cast<uint32_t>(rgba.width()),
                        static_cast<uint32_t>(rgba.height()), 1};
    vkCmdCopyBufferToImage(commandBuffer, stagingBuffer, image,
                           VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &copy);
    transition(VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
               VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    vkEndCommandBuffer(commandBuffer);
    VkSubmitInfo submit{};
    submit.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    submit.commandBufferCount = 1;
    submit.pCommandBuffers = &commandBuffer;
    vkQueueSubmit(queue, 1, &submit, fence);
    vkWaitForFences(device, 1, &fence, VK_TRUE, 5'000'000'000ull);
    imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    if (uploadMs)
      *uploadMs = static_cast<double>(timer.nsecsElapsed()) / 1'000'000.0;
    return true;
  }

  jcut::vulkan_detector::VulkanDeviceContext detectorContext() const {
    return {physicalDevice, device, queue, queueFamilyIndex};
  }

  bool createBuffer(VkDeviceSize bytes, VkBufferUsageFlags usage,
                    VkMemoryPropertyFlags properties, VkBuffer *buffer,
                    VkDeviceMemory *memory, VkDeviceSize *storedSize,
                    QString *error) {
    destroyBuffer(*buffer, *memory);
    VkBufferCreateInfo info{};
    info.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    info.size = bytes;
    info.usage = usage;
    info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    if (vkCreateBuffer(device, &info, nullptr, buffer) != VK_SUCCESS) {
      if (error)
        *error = QStringLiteral("Failed to create Vulkan buffer.");
      return false;
    }
    VkMemoryRequirements req{};
    vkGetBufferMemoryRequirements(device, *buffer, &req);
    const uint32_t type =
        findMemoryType(physicalDevice, req.memoryTypeBits, properties);
    if (type == UINT32_MAX) {
      if (error)
        *error = QStringLiteral("No Vulkan buffer memory type.");
      return false;
    }
    VkMemoryAllocateInfo alloc{};
    alloc.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    alloc.allocationSize = req.size;
    alloc.memoryTypeIndex = type;
    if (vkAllocateMemory(device, &alloc, nullptr, memory) != VK_SUCCESS) {
      if (error)
        *error = QStringLiteral("Failed to allocate Vulkan buffer memory.");
      return false;
    }
    vkBindBufferMemory(device, *buffer, *memory, 0);
    *storedSize = bytes;
    return true;
  }

  void destroyBuffer(VkBuffer &buffer, VkDeviceMemory &memory) {
    if (buffer)
      vkDestroyBuffer(device, buffer, nullptr);
    if (memory)
      vkFreeMemory(device, memory, nullptr);
    buffer = VK_NULL_HANDLE;
    memory = VK_NULL_HANDLE;
  }

  void transition(VkImageLayout oldLayout, VkImageLayout newLayout) {
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
    vkCmdPipelineBarrier(commandBuffer, src, dst, 0, 0, nullptr, 0, nullptr, 1,
                         &barrier);
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

QString findRes10NcnnModelFile(const QString &explicitPath,
                               const QString &fileName);
QString scrfdModelFileName(const QString &variantId, const QString &suffix);

QVector<Detection> readDetections(VulkanHarnessContext &vk, int imageWidth,
                                  int imageHeight);
QVector<Detection> detectVulkanFrame(
    jcut::vulkan_detector::VulkanZeroCopyFaceDetector *detector,
    VulkanHarnessContext *vk, const render_detail::OffscreenVulkanFrame &frame,
    int maxDetections, float threshold, double *vulkanMs, QString *error);
QVector<Detection> detectRes10VulkanFrame(
    jcut::vulkan_detector::VulkanZeroCopyFaceDetector *preprocessor,
    jcut::vulkan_detector::VulkanRes10NcnnFaceDetector *detector,
    VulkanHarnessContext *vk, const render_detail::OffscreenVulkanFrame &frame,
    const QString &paramPath, const QString &binPath, float threshold,
    bool smallFaceFallback, bool suppressNcnnInfo, double *vulkanMs,
    QString *error);
QVector<Detection> detectRes10FromDecoderFrame(
    jcut::vulkan_detector::VulkanZeroCopyFaceDetector *preprocessor,
    jcut::vulkan_detector::VulkanRes10NcnnFaceDetector *detector,
    VulkanHarnessContext *vk,
    jcut::vulkan_detector::VulkanDetectorFrameHandoff *handoff,
    const editor::FrameHandle &frame, const QString &paramPath,
    const QString &binPath, float threshold, bool allowCpuUploadFallback,
    double *uploadMs, double *vulkanMs, bool *hardwareDirectUsed,
    QString *error);
QVector<Detection> detectScrfdVulkanFrame(
    jcut::vulkan_detector::VulkanZeroCopyFaceDetector *preprocessor,
    jcut::vulkan_detector::VulkanScrfdNcnnFaceDetector *detector,
    VulkanHarnessContext *vk, const render_detail::OffscreenVulkanFrame &frame,
    const QString &paramPath, const QString &binPath, float threshold,
    int targetSize, bool tiledPass, bool suppressNcnnInfo, double *vulkanMs,
    QString *error);
QVector<Detection> detectScrfdFromDecoderFrame(
    jcut::vulkan_detector::VulkanZeroCopyFaceDetector *preprocessor,
    jcut::vulkan_detector::VulkanScrfdNcnnFaceDetector *detector,
    VulkanHarnessContext *vk,
    jcut::vulkan_detector::VulkanDetectorFrameHandoff *handoff,
    const editor::FrameHandle &frame, const QString &paramPath,
    const QString &binPath, float threshold, int targetSize, bool tiledPass,
    bool suppressNcnnInfo, bool allowCpuUploadFallback, double *uploadMs,
    double *vulkanMs, bool *hardwareDirectUsed, QString *error);

struct PreparedDecoderDetectionResult {
  QVector<Detection> detections;
  jcut::vulkan_detector::NcnnInferenceStats ncnnStats;
  jcut::vulkan_detector::HardwareInteropProbeResult handoffProbe;
  QString error;
  QString hardwareDirectAttemptReason;
  double vulkanDetectMs = 0.0;
  bool ok = false;
};

struct PreparedDecoderDetectionSlot {
  VulkanHarnessContext context;
  jcut::vulkan_detector::VulkanDetectorFrameHandoff handoff;
  jcut::vulkan_detector::VulkanZeroCopyFaceDetector preprocessor;
  jcut::vulkan_detector::VulkanRes10NcnnFaceDetector res10Detector;
  jcut::vulkan_detector::VulkanScrfdNcnnFaceDetector scrfdDetector;
  editor::FrameHandle decodedFrame;
  jcut::vulkan_detector::ScrfdTensorLayout scrfdLayout;
  int frameNumber = -1;
  int frameOffset = -1;
  QSize detectionFrameSize;
  double decoderUploadMs = 0.0;
  int workerIndex = 0;
  bool hardwareDirectHandoff = false;
  bool decoderVulkanUploadFallback = false;
  bool active = false;
  bool detectionRunning = false;
  std::future<PreparedDecoderDetectionResult> detectionFuture;
};

constexpr int kMaxDecoderDirectPipelineSlots = 10;

struct DecoderDetectorWorker {
  bool busy = false;
};

VkDeviceSize scrfdTensorBytesForSourceSize(const QSize &sourceSize,
                                           int targetSize);
bool prepareRes10DecoderFrame(
    jcut::vulkan_detector::VulkanZeroCopyFaceDetector *preprocessor,
    jcut::vulkan_detector::VulkanRes10NcnnFaceDetector *detector,
    VulkanHarnessContext *vk,
    jcut::vulkan_detector::VulkanDetectorFrameHandoff *handoff,
    const editor::FrameHandle &frame, const QString &paramPath,
    const QString &binPath, bool suppressNcnnInfo, bool allowCpuUploadFallback,
    double *uploadMs, bool *hardwareDirectUsed, QSize *detectionFrameSize,
    QString *error);
QVector<Detection> finalizePreparedRes10DecoderFrame(
    jcut::vulkan_detector::VulkanZeroCopyFaceDetector *preprocessor,
    jcut::vulkan_detector::VulkanRes10NcnnFaceDetector *detector,
    VulkanHarnessContext *vk,
    jcut::vulkan_detector::VulkanDetectorFrameHandoff *handoff,
    const QSize &detectionFrameSize, float threshold, double *vulkanMs,
    QString *error);
bool prepareScrfdDecoderFrame(
    jcut::vulkan_detector::VulkanZeroCopyFaceDetector *preprocessor,
    jcut::vulkan_detector::VulkanScrfdNcnnFaceDetector *detector,
    VulkanHarnessContext *vk,
    jcut::vulkan_detector::VulkanDetectorFrameHandoff *handoff,
    const editor::FrameHandle &frame, const QString &paramPath,
    const QString &binPath, int targetSize, bool suppressNcnnInfo,
    bool allowCpuUploadFallback,
    jcut::vulkan_detector::ScrfdTensorLayout *layout, double *uploadMs,
    bool *hardwareDirectUsed, QSize *detectionFrameSize, QString *error);
QVector<Detection> finalizePreparedScrfdDecoderFrame(
    jcut::vulkan_detector::VulkanZeroCopyFaceDetector *preprocessor,
    jcut::vulkan_detector::VulkanScrfdNcnnFaceDetector *detector,
    VulkanHarnessContext *vk,
    jcut::vulkan_detector::VulkanDetectorFrameHandoff *handoff,
    const jcut::vulkan_detector::ScrfdTensorLayout &layout,
    const QSize &detectionFrameSize, float threshold, double *vulkanMs,
    QString *error);
