#include "vulkan_facedetections_offscreen_vulkan_harness.h"

#include "detector_settings.h"
#include "render_internal.h"

#include <QCoreApplication>
#include <QDir>
#include <QElapsedTimer>
#include <QFileInfo>
#include <QRectF>

#include <algorithm>

namespace {

jcut::core::SizeI toSizeI(const QSize &size) {
  return {size.width(), size.height()};
}

} // namespace

ScopedStderrSilencer::ScopedStderrSilencer(bool enabled) {
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

ScopedStderrSilencer::~ScopedStderrSilencer() {
    if (m_savedFd >= 0) {
      fflush(stderr);
      dup2(m_savedFd, STDERR_FILENO);
      close(m_savedFd);
    }
  }

VulkanHarnessContext::~VulkanHarnessContext() { release(); }

bool VulkanHarnessContext::initialize(QString *error) {
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
    enableIfAvailable(VK_KHR_EXTERNAL_SEMAPHORE_EXTENSION_NAME);
    enableIfAvailable(VK_KHR_EXTERNAL_SEMAPHORE_FD_EXTENSION_NAME);
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

bool VulkanHarnessContext::attachExternalDevice(
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

void VulkanHarnessContext::release() {
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

bool VulkanHarnessContext::ensureDetectorBuffers(
    VkDeviceSize tensorBytesIn, int maxDetectionsIn, QString *error) {
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

bool VulkanHarnessContext::ensureResources(
    const jcut::core::SizeI &size, VkDeviceSize tensorBytes, int maxDetections,
    QString *error) {
    const VkDeviceSize stagingBytes =
        static_cast<VkDeviceSize>(size.width) * size.height * 4;
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
    imageInfo.extent = {static_cast<uint32_t>(size.width),
                        static_cast<uint32_t>(size.height), 1};
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

bool VulkanHarnessContext::uploadCpuImage(const QImage &imageIn,
                                            double *uploadMs, QString *error) {
    const QImage rgba = imageIn.convertToFormat(QImage::Format_RGBA8888);
    if (!ensureResources(toSizeI(rgba.size()), tensorBytes, maxDetections, error)) {
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

jcut::vulkan_detector::VulkanDeviceContext VulkanHarnessContext::detectorContext() const {
    return {physicalDevice, device, queue, queueFamilyIndex};
  }

bool VulkanHarnessContext::createBuffer(
    VkDeviceSize bytes, VkBufferUsageFlags usage,
    VkMemoryPropertyFlags properties, VkBuffer *buffer, VkDeviceMemory *memory,
    VkDeviceSize *storedSize, QString *error) {
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

void VulkanHarnessContext::destroyBuffer(VkBuffer &buffer,
                                         VkDeviceMemory &memory) {
    if (buffer)
      vkDestroyBuffer(device, buffer, nullptr);
    if (memory)
      vkFreeMemory(device, memory, nullptr);
    buffer = VK_NULL_HANDLE;
    memory = VK_NULL_HANDLE;
  }

void VulkanHarnessContext::transition(VkImageLayout oldLayout,
                                      VkImageLayout newLayout) {
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

QString findRes10NcnnModelFile(const QString &explicitPath,
                               const QString &fileName) {
  if (!explicitPath.isEmpty()) {
    return explicitPath;
  }
  const QStringList roots{QDir::currentPath(),
                          QCoreApplication::applicationDirPath(),
                          QDir(QCoreApplication::applicationDirPath())
                              .absoluteFilePath(QStringLiteral(".."))};
  const QStringList rels{
      QStringLiteral("assets/models/%1").arg(fileName),
      QStringLiteral("testbench_assets/models/%1").arg(fileName),
      QStringLiteral("models/%1").arg(fileName)};
  for (const QString &root : roots) {
    for (const QString &rel : rels) {
      const QString candidate = QDir(root).absoluteFilePath(rel);
      if (QFileInfo::exists(candidate)) {
        return candidate;
      }
    }
  }
  return QDir::current().absoluteFilePath(
      QStringLiteral("assets/models/%1").arg(fileName));
}

QString scrfdModelFileName(const QString &variantId, const QString &suffix) {
  jcut::facedetections::ScrfdModelVariantDefinition definition;
  if (jcut::facedetections::scrfdModelVariantById(variantId, &definition)) {
    return definition.modelStem + suffix;
  }
  return QStringLiteral("scrfd_500m-opt2") + suffix;
}

uint32_t findMemoryType(VkPhysicalDevice physicalDevice, uint32_t typeBits,
                        VkMemoryPropertyFlags properties) {
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

QVector<Detection> readDetections(VulkanHarnessContext &vk, int imageWidth,
                                  int imageHeight) {
  QVector<Detection> out;
  void *mapped = nullptr;
  if (vkMapMemory(vk.device, vk.detectionMemory, 0, vk.detectionSize, 0,
                  &mapped) != VK_SUCCESS) {
    return out;
  }
  const auto *bytes = static_cast<const unsigned char *>(mapped);
  const uint32_t count =
      qMin<uint32_t>(*reinterpret_cast<const uint32_t *>(bytes),
                     static_cast<uint32_t>(vk.maxDetections));
  const float *det = reinterpret_cast<const float *>(bytes + 16);
  out.reserve(static_cast<int>(count));
  for (uint32_t i = 0; i < count; ++i) {
    const float x = det[i * 8 + 0];
    const float y = det[i * 8 + 1];
    const float w = det[i * 8 + 2];
    const float h = det[i * 8 + 3];
    const float c = det[i * 8 + 4];
    QRectF box(x * imageWidth, y * imageHeight, w * imageWidth,
               h * imageHeight);
    box = box.intersected(QRectF(0, 0, imageWidth, imageHeight));
    if (box.width() >= 8.0 && box.height() >= 8.0) {
      out.push_back({box, c});
    }
  }
  vkUnmapMemory(vk.device, vk.detectionMemory);
  std::sort(out.begin(), out.end(), [](const Detection &a, const Detection &b) {
    return a.confidence > b.confidence;
  });
  return out;
}

QVector<Detection> detectVulkanFrame(
    jcut::vulkan_detector::VulkanZeroCopyFaceDetector *detector,
    VulkanHarnessContext *vk, const render_detail::OffscreenVulkanFrame &frame,
    int maxDetections, float threshold, double *vulkanMs, QString *error) {
  QVector<Detection> out;
  if (!detector || !vk || !frame.valid) {
    if (error)
      *error = QStringLiteral("Invalid Vulkan detector frame.");
    return out;
  }
  if (!vk->attachExternalDevice({frame.physicalDevice, frame.device,
                                 frame.queue, frame.queueFamilyIndex},
                                error)) {
    return out;
  }
  if (!detector->isInitialized() &&
      !detector->initialize(vk->detectorContext(), error)) {
    return out;
  }
  if (!vk->ensureDetectorBuffers(detector->tensorSpec().byteSize(),
                                 maxDetections, error)) {
    return out;
  }

  QElapsedTimer timer;
  timer.start();
  const jcut::vulkan_detector::VulkanExternalImage source{
      frame.imageView, frame.imageLayout, frame.size, false};
  const jcut::vulkan_detector::VulkanTensorBuffer tensor{
      vk->tensorBuffer, detector->tensorSpec().byteSize()};
  if (!detector->preprocessToTensor(source, tensor, error)) {
    return out;
  }
  const jcut::vulkan_detector::VulkanTensorBuffer detectionBuffer{
      vk->detectionBuffer, vk->detectionSize};
  if (!detector->inferFromTensor(tensor, detectionBuffer, maxDetections,
                                 threshold, error)) {
    return out;
  }
  out = readDetections(*vk, frame.size.width, frame.size.height);
  if (vulkanMs) {
    *vulkanMs = static_cast<double>(timer.nsecsElapsed()) / 1'000'000.0;
  }
  return out;
}

QVector<Detection> detectRes10VulkanFrame(
    jcut::vulkan_detector::VulkanZeroCopyFaceDetector *preprocessor,
    jcut::vulkan_detector::VulkanRes10NcnnFaceDetector *detector,
    VulkanHarnessContext *vk, const render_detail::OffscreenVulkanFrame &frame,
    const QString &paramPath, const QString &binPath, float threshold,
    bool smallFaceFallback, bool suppressNcnnInfo, double *vulkanMs,
    QString *error) {
  QVector<Detection> out;
  if (!preprocessor || !detector || !vk || !frame.valid) {
    if (error)
      *error = QStringLiteral("Invalid Vulkan Res10 detector frame.");
    return out;
  }
  if (!vk->attachExternalDevice({frame.physicalDevice, frame.device,
                                 frame.queue, frame.queueFamilyIndex},
                                error)) {
    return out;
  }
  if (!preprocessor->isInitialized() &&
      !preprocessor->initialize(vk->detectorContext(), error)) {
    return out;
  }
  if (!detector->isInitialized()) {
    ScopedStderrSilencer silence(suppressNcnnInfo);
    if (!detector->initialize(vk->detectorContext(), paramPath, binPath,
                              error)) {
      return out;
    }
  }
  if (!vk->ensureDetectorBuffers(preprocessor->tensorSpec().byteSize(), 1,
                                 error)) {
    return out;
  }

  QElapsedTimer timer;
  timer.start();
  const jcut::vulkan_detector::VulkanTensorBuffer tensor{
      vk->tensorBuffer, preprocessor->tensorSpec().byteSize()};
  auto runRoi = [&](const QRectF &roi, QVector<Detection> *dst) -> bool {
    const QRectF bounded = roi.intersected(QRectF(0.0, 0.0, 1.0, 1.0));
    if (bounded.width() <= 0.01 || bounded.height() <= 0.01) {
      return true;
    }
    const jcut::vulkan_detector::VulkanExternalImage source{
        frame.imageView,
        frame.imageLayout,
        frame.size,
        false,
        static_cast<float>(bounded.x()),
        static_cast<float>(bounded.y()),
        static_cast<float>(bounded.width()),
        static_cast<float>(bounded.height())};
    if (!preprocessor->preprocessToTensor(source, tensor, error)) {
      return false;
    }
    const int roiWidth = qMax(1, qRound(frame.size.width * bounded.width()));
    const int roiHeight =
        qMax(1, qRound(frame.size.height * bounded.height()));
    const QVector<jcut::vulkan_detector::Res10Detection> raw =
        detector->inferFromTensor(tensor, roiWidth, roiHeight, threshold,
                                  error);
    if (!preprocessor->finishPendingPreprocess(error)) {
      return false;
    }
    dst->reserve(dst->size() + raw.size());
    for (const auto &det : raw) {
      QRectF box(bounded.x() * frame.size.width + det.box.x(),
                 bounded.y() * frame.size.height + det.box.y(),
                 det.box.width(), det.box.height());
      dst->push_back({box, det.confidence});
    }
    return true;
  };
  if (!runRoi(QRectF(0.0, 0.0, 1.0, 1.0), &out)) {
    return out;
  }
  if (out.isEmpty() && smallFaceFallback) {
    static const QRectF rois[] = {
        QRectF(0.0, 0.0, 0.50, 1.0),   QRectF(0.25, 0.0, 0.50, 1.0),
        QRectF(0.50, 0.0, 0.50, 1.0),  QRectF(0.0, 0.0, 0.50, 0.65),
        QRectF(0.25, 0.0, 0.50, 0.65), QRectF(0.50, 0.0, 0.50, 0.65)};
    for (const QRectF &roi : rois) {
      if (!runRoi(roi, &out)) {
        return out;
      }
    }
  }
  if (vulkanMs) {
    *vulkanMs = static_cast<double>(timer.nsecsElapsed()) / 1'000'000.0;
  }
  return out;
}

QVector<Detection> detectRes10FromDecoderFrame(
    jcut::vulkan_detector::VulkanZeroCopyFaceDetector *preprocessor,
    jcut::vulkan_detector::VulkanRes10NcnnFaceDetector *detector,
    VulkanHarnessContext *vk,
    jcut::vulkan_detector::VulkanDetectorFrameHandoff *handoff,
    const editor::FrameHandle &frame, const QString &paramPath,
    const QString &binPath, float threshold, bool allowCpuUploadFallback,
    double *uploadMs, double *vulkanMs, bool *hardwareDirectUsed,
    QString *error) {
  QVector<Detection> out;
  if (!preprocessor || !detector || !vk || !handoff || frame.isNull()) {
    if (error)
      *error =
          QStringLiteral("Invalid decoder frame for Vulkan detector handoff.");
    return out;
  }
  std::string handoffError;
  if (!handoff->uploadFrame(frame, allowCpuUploadFallback, uploadMs, &handoffError)) {
    if (error) {
      *error = QString::fromStdString(handoffError);
    }
    return out;
  }
  if (hardwareDirectUsed) {
    *hardwareDirectUsed =
        handoff->lastMode() ==
        jcut::vulkan_detector::FrameHandoffMode::HardwareDirect;
  }
  const jcut::vulkan_detector::VulkanExternalImage source =
      handoff->externalImage();
  if (source.imageView == VK_NULL_HANDLE || !source.size.valid()) {
    if (error)
      *error = QStringLiteral("Frame handoff produced invalid external image.");
    return out;
  }
  render_detail::OffscreenVulkanFrame detectorFrame;
  detectorFrame.physicalDevice = vk->physicalDevice;
  detectorFrame.device = vk->device;
  detectorFrame.queue = vk->queue;
  detectorFrame.queueFamilyIndex = vk->queueFamilyIndex;
  detectorFrame.imageView = source.imageView;
  detectorFrame.imageLayout = source.imageLayout;
  detectorFrame.size = source.size;
  detectorFrame.queueSupportsCompute = true;
  detectorFrame.valid = true;
  out = detectRes10VulkanFrame(preprocessor, detector, vk, detectorFrame,
                               paramPath, binPath, threshold, false, false,
                               vulkanMs, error);
  handoffError.clear();
  if (!handoff->finishPendingUpload(nullptr, &handoffError)) {
    if (error) {
      *error = QString::fromStdString(handoffError);
    }
    out.clear();
    return out;
  }
  return out;
}

QVector<Detection> detectScrfdVulkanFrame(
    jcut::vulkan_detector::VulkanZeroCopyFaceDetector *preprocessor,
    jcut::vulkan_detector::VulkanScrfdNcnnFaceDetector *detector,
    VulkanHarnessContext *vk, const render_detail::OffscreenVulkanFrame &frame,
    const QString &paramPath, const QString &binPath, float threshold,
    int targetSize, bool tiledPass, bool suppressNcnnInfo, double *vulkanMs,
    QString *error) {
  QVector<Detection> out;
  if (!preprocessor || !detector || !vk || !frame.valid) {
    if (error)
      *error = QStringLiteral("Invalid Vulkan SCRFD detector frame.");
    return out;
  }
  if (!vk->attachExternalDevice({frame.physicalDevice, frame.device,
                                 frame.queue, frame.queueFamilyIndex},
                                error)) {
    return out;
  }
  if (!preprocessor->isInitialized() &&
      !preprocessor->initialize(vk->detectorContext(), error)) {
    return out;
  }
  if (!detector->isInitialized()) {
    ScopedStderrSilencer silence(suppressNcnnInfo);
    if (!detector->initialize(vk->detectorContext(), paramPath, binPath,
                              error)) {
      return out;
    }
  }

  const int sourceWidth = qMax(1, frame.size.width);
  const int sourceHeight = qMax(1, frame.size.height);
  targetSize = qMax(320, targetSize);
  float scale = 1.0f;
  int resizedW = sourceWidth;
  int resizedH = sourceHeight;
  if (resizedW > resizedH) {
    scale = static_cast<float>(targetSize) / static_cast<float>(resizedW);
    resizedW = targetSize;
    resizedH = qMax(1, qRound(static_cast<float>(resizedH) * scale));
  } else {
    scale = static_cast<float>(targetSize) / static_cast<float>(resizedH);
    resizedH = targetSize;
    resizedW = qMax(1, qRound(static_cast<float>(resizedW) * scale));
  }
  const VkDeviceSize tensorBytes =
      static_cast<VkDeviceSize>(((resizedW + 31) / 32) * 32) *
      static_cast<VkDeviceSize>(((resizedH + 31) / 32) * 32) *
      static_cast<VkDeviceSize>(3 * sizeof(float));
  if (!vk->ensureDetectorBuffers(tensorBytes, 1, error)) {
    return out;
  }

  QElapsedTimer timer;
  timer.start();
  const jcut::vulkan_detector::VulkanTensorBuffer tensor{vk->tensorBuffer,
                                                         vk->tensorSize};
  auto appendPass = [&](const QRectF &roiNorm) -> bool {
    const QRectF bounded = roiNorm.intersected(QRectF(0.0, 0.0, 1.0, 1.0));
    if (bounded.width() <= 0.05 || bounded.height() <= 0.05) {
      return true;
    }
    const jcut::vulkan_detector::VulkanExternalImage source{
        frame.imageView,
        frame.imageLayout,
        frame.size,
        false,
        static_cast<float>(bounded.x()),
        static_cast<float>(bounded.y()),
        static_cast<float>(bounded.width()),
        static_cast<float>(bounded.height())};
    jcut::vulkan_detector::ScrfdTensorLayout layout;
    if (!preprocessor->preprocessScrfdToTensor(source, tensor, targetSize,
                                               &layout, error)) {
      return false;
    }
    const int roiWidth =
        qMax(1, qRound(static_cast<double>(sourceWidth) * bounded.width()));
    const int roiHeight =
        qMax(1, qRound(static_cast<double>(sourceHeight) * bounded.height()));
    const QVector<jcut::vulkan_detector::ScrfdDetection> raw =
        detector->inferFromTensor(tensor, layout, roiWidth, roiHeight,
                                  threshold, error);
    if (!preprocessor->finishPendingPreprocess(error)) {
      return false;
    }
    out.reserve(out.size() + raw.size());
    for (const auto &det : raw) {
      QRectF box(bounded.x() * sourceWidth + det.box.x(),
                 bounded.y() * sourceHeight + det.box.y(), det.box.width(),
                 det.box.height());
      out.push_back({box, det.confidence});
    }
    return true;
  };
  if (!appendPass(QRectF(0.0, 0.0, 1.0, 1.0))) {
    return out;
  }
  if (tiledPass) {
    constexpr qreal kTileSize = 0.60;
    constexpr qreal kTileStep = 0.40;
    static const QRectF tileRois[] = {
        QRectF(0.00, 0.00, kTileSize, kTileSize),
        QRectF(kTileStep, 0.00, kTileSize, kTileSize),
        QRectF(0.00, kTileStep, kTileSize, kTileSize),
        QRectF(kTileStep, kTileStep, kTileSize, kTileSize)};
    for (const QRectF &roi : tileRois) {
      if (!appendPass(roi)) {
        return out;
      }
    }
  }
  if (vulkanMs) {
    *vulkanMs = static_cast<double>(timer.nsecsElapsed()) / 1'000'000.0;
  }
  return out;
}

QVector<Detection> detectScrfdFromDecoderFrame(
    jcut::vulkan_detector::VulkanZeroCopyFaceDetector *preprocessor,
    jcut::vulkan_detector::VulkanScrfdNcnnFaceDetector *detector,
    VulkanHarnessContext *vk,
    jcut::vulkan_detector::VulkanDetectorFrameHandoff *handoff,
    const editor::FrameHandle &frame, const QString &paramPath,
    const QString &binPath, float threshold, int targetSize, bool tiledPass,
    bool suppressNcnnInfo, bool allowCpuUploadFallback, double *uploadMs,
    double *vulkanMs, bool *hardwareDirectUsed, QString *error) {
  QVector<Detection> out;
  if (!preprocessor || !detector || !vk || !handoff || frame.isNull()) {
    if (error)
      *error =
          QStringLiteral("Invalid decoder frame for Vulkan SCRFD handoff.");
    return out;
  }
  std::string handoffError;
  if (!handoff->uploadFrame(frame, allowCpuUploadFallback, uploadMs, &handoffError)) {
    if (error) {
      *error = QString::fromStdString(handoffError);
    }
    return out;
  }
  if (hardwareDirectUsed) {
    *hardwareDirectUsed =
        handoff->lastMode() ==
        jcut::vulkan_detector::FrameHandoffMode::HardwareDirect;
  }
  const jcut::vulkan_detector::VulkanExternalImage source =
      handoff->externalImage();
  if (source.imageView == VK_NULL_HANDLE || !source.size.valid()) {
    if (error)
      *error = QStringLiteral("Frame handoff produced invalid external image.");
    return out;
  }
  render_detail::OffscreenVulkanFrame detectorFrame;
  detectorFrame.physicalDevice = vk->physicalDevice;
  detectorFrame.device = vk->device;
  detectorFrame.queue = vk->queue;
  detectorFrame.queueFamilyIndex = vk->queueFamilyIndex;
  detectorFrame.imageView = source.imageView;
  detectorFrame.imageLayout = source.imageLayout;
  detectorFrame.size = source.size;
  detectorFrame.queueSupportsCompute = true;
  detectorFrame.valid = true;
  out = detectScrfdVulkanFrame(preprocessor, detector, vk, detectorFrame,
                               paramPath, binPath, threshold, targetSize,
                               tiledPass, suppressNcnnInfo, vulkanMs, error);
  handoffError.clear();
  if (!handoff->finishPendingUpload(nullptr, &handoffError)) {
    if (error) {
      *error = QString::fromStdString(handoffError);
    }
    out.clear();
    return out;
  }
  return out;
}

VkDeviceSize scrfdTensorBytesForSourceSize(const jcut::core::SizeI &sourceSize,
                                           int targetSize) {
  const int sourceWidth = qMax(1, sourceSize.width);
  const int sourceHeight = qMax(1, sourceSize.height);
  targetSize = qMax(320, targetSize);
  int resizedW = sourceWidth;
  int resizedH = sourceHeight;
  if (resizedW > resizedH) {
    const float scale =
        static_cast<float>(targetSize) / static_cast<float>(resizedW);
    resizedW = targetSize;
    resizedH = qMax(1, qRound(static_cast<float>(resizedH) * scale));
  } else {
    const float scale =
        static_cast<float>(targetSize) / static_cast<float>(resizedH);
    resizedH = targetSize;
    resizedW = qMax(1, qRound(static_cast<float>(resizedW) * scale));
  }
  return static_cast<VkDeviceSize>(((resizedW + 31) / 32) * 32) *
         static_cast<VkDeviceSize>(((resizedH + 31) / 32) * 32) *
         static_cast<VkDeviceSize>(3 * sizeof(float));
}

bool prepareRes10DecoderFrame(
    jcut::vulkan_detector::VulkanZeroCopyFaceDetector *preprocessor,
    jcut::vulkan_detector::VulkanRes10NcnnFaceDetector *detector,
    VulkanHarnessContext *vk,
    jcut::vulkan_detector::VulkanDetectorFrameHandoff *handoff,
    const editor::FrameHandle &frame, const QString &paramPath,
    const QString &binPath, bool suppressNcnnInfo, bool allowCpuUploadFallback,
    double *uploadMs, bool *hardwareDirectUsed, jcut::core::SizeI *detectionFrameSize,
    QString *error) {
  if (!preprocessor || !detector || !vk || !handoff || frame.isNull()) {
    if (error)
      *error =
          QStringLiteral("Invalid decoder frame for Vulkan Res10 preparation.");
    return false;
  }
  std::string handoffError;
  if (!handoff->uploadFrame(frame, allowCpuUploadFallback, uploadMs, &handoffError)) {
    if (error) {
      *error = QString::fromStdString(handoffError);
    }
    return false;
  }
  if (hardwareDirectUsed) {
    *hardwareDirectUsed =
        handoff->lastMode() ==
        jcut::vulkan_detector::FrameHandoffMode::HardwareDirect;
  }
  const jcut::vulkan_detector::VulkanExternalImage source =
      handoff->externalImage();
  if (source.imageView == VK_NULL_HANDLE || !source.size.valid()) {
    if (error)
      *error = QStringLiteral("Frame handoff produced invalid external image.");
    return false;
  }
  if (!preprocessor->isInitialized() &&
      !preprocessor->initialize(vk->detectorContext(), error)) {
    return false;
  }
  if (!detector->isInitialized()) {
    ScopedStderrSilencer silence(suppressNcnnInfo);
    if (!detector->initialize(vk->detectorContext(), paramPath, binPath,
                              error)) {
      return false;
    }
  }
  if (!vk->ensureDetectorBuffers(preprocessor->tensorSpec().byteSize(), 1,
                                 error)) {
    return false;
  }
  const jcut::vulkan_detector::VulkanTensorBuffer tensor{
      vk->tensorBuffer, preprocessor->tensorSpec().byteSize()};
  if (!preprocessor->preprocessToTensor(source, tensor, error)) {
    return false;
  }
  if (detectionFrameSize) {
    *detectionFrameSize = source.size;
  }
  return true;
}

QVector<Detection> finalizePreparedRes10DecoderFrame(
    jcut::vulkan_detector::VulkanZeroCopyFaceDetector *preprocessor,
    jcut::vulkan_detector::VulkanRes10NcnnFaceDetector *detector,
    VulkanHarnessContext *vk,
    jcut::vulkan_detector::VulkanDetectorFrameHandoff *handoff,
    const jcut::core::SizeI &detectionFrameSize, float threshold, double *vulkanMs,
    QString *error) {
  QVector<Detection> out;
  if (!preprocessor || !detector || !vk || !handoff ||
      !detectionFrameSize.valid()) {
    if (error)
      *error = QStringLiteral("Invalid prepared Vulkan Res10 decoder frame.");
    return out;
  }
  QElapsedTimer timer;
  timer.start();
  const jcut::vulkan_detector::VulkanTensorBuffer tensor{
      vk->tensorBuffer, preprocessor->tensorSpec().byteSize()};
  const QVector<jcut::vulkan_detector::Res10Detection> raw =
      detector->inferFromTensor(tensor, detectionFrameSize.width,
                                detectionFrameSize.height, threshold, error);
  if (!preprocessor->finishPendingPreprocess(error)) {
    out.clear();
    return out;
  }
  std::string handoffError;
  if (!handoff->finishPendingUpload(nullptr, &handoffError)) {
    if (error) {
      *error = QString::fromStdString(handoffError);
    }
    out.clear();
    return out;
  }
  out.reserve(raw.size());
  for (const auto &det : raw) {
    out.push_back({det.box, det.confidence});
  }
  if (vulkanMs) {
    *vulkanMs = static_cast<double>(timer.nsecsElapsed()) / 1'000'000.0;
  }
  return out;
}

bool prepareScrfdDecoderFrame(
    jcut::vulkan_detector::VulkanZeroCopyFaceDetector *preprocessor,
    jcut::vulkan_detector::VulkanScrfdNcnnFaceDetector *detector,
    VulkanHarnessContext *vk,
    jcut::vulkan_detector::VulkanDetectorFrameHandoff *handoff,
    const editor::FrameHandle &frame, const QString &paramPath,
    const QString &binPath, int targetSize, bool suppressNcnnInfo,
    bool allowCpuUploadFallback,
    jcut::vulkan_detector::ScrfdTensorLayout *layout, double *uploadMs,
    bool *hardwareDirectUsed, jcut::core::SizeI *detectionFrameSize, QString *error) {
  if (!preprocessor || !detector || !vk || !handoff || frame.isNull() ||
      !layout) {
    if (error)
      *error =
          QStringLiteral("Invalid decoder frame for Vulkan SCRFD preparation.");
    return false;
  }
  std::string handoffError;
  if (!handoff->uploadFrame(frame, allowCpuUploadFallback, uploadMs, &handoffError)) {
    if (error) {
      *error = QString::fromStdString(handoffError);
    }
    return false;
  }
  if (hardwareDirectUsed) {
    *hardwareDirectUsed =
        handoff->lastMode() ==
        jcut::vulkan_detector::FrameHandoffMode::HardwareDirect;
  }
  const jcut::vulkan_detector::VulkanExternalImage source =
      handoff->externalImage();
  if (source.imageView == VK_NULL_HANDLE || !source.size.valid()) {
    if (error)
      *error = QStringLiteral("Frame handoff produced invalid external image.");
    return false;
  }
  if (!preprocessor->isInitialized() &&
      !preprocessor->initialize(vk->detectorContext(), error)) {
    return false;
  }
  if (!detector->isInitialized()) {
    ScopedStderrSilencer silence(suppressNcnnInfo);
    if (!detector->initialize(vk->detectorContext(), paramPath, binPath,
                              error)) {
      return false;
    }
  }
  const VkDeviceSize tensorBytes =
      scrfdTensorBytesForSourceSize(source.size, targetSize);
  if (!vk->ensureDetectorBuffers(tensorBytes, 1, error)) {
    return false;
  }
  const jcut::vulkan_detector::VulkanTensorBuffer tensor{vk->tensorBuffer,
                                                         vk->tensorSize};
  if (!preprocessor->preprocessScrfdToTensor(source, tensor, targetSize, layout,
                                             error)) {
    return false;
  }
  if (detectionFrameSize) {
    *detectionFrameSize = source.size;
  }
  return true;
}

QVector<Detection> finalizePreparedScrfdDecoderFrame(
    jcut::vulkan_detector::VulkanZeroCopyFaceDetector *preprocessor,
    jcut::vulkan_detector::VulkanScrfdNcnnFaceDetector *detector,
    VulkanHarnessContext *vk,
    jcut::vulkan_detector::VulkanDetectorFrameHandoff *handoff,
    const jcut::vulkan_detector::ScrfdTensorLayout &layout,
    const jcut::core::SizeI &detectionFrameSize, float threshold, double *vulkanMs,
    QString *error) {
  QVector<Detection> out;
  if (!preprocessor || !detector || !vk || !handoff ||
      !detectionFrameSize.valid()) {
    if (error)
      *error = QStringLiteral("Invalid prepared Vulkan SCRFD decoder frame.");
    return out;
  }
  QElapsedTimer timer;
  timer.start();
  const jcut::vulkan_detector::VulkanTensorBuffer tensor{vk->tensorBuffer,
                                                         vk->tensorSize};
  const QVector<jcut::vulkan_detector::ScrfdDetection> raw =
      detector->inferFromTensor(tensor, layout, detectionFrameSize.width,
                                detectionFrameSize.height, threshold, error);
  if (!preprocessor->finishPendingPreprocess(error)) {
    out.clear();
    return out;
  }
  std::string handoffError;
  if (!handoff->finishPendingUpload(nullptr, &handoffError)) {
    if (error) {
      *error = QString::fromStdString(handoffError);
    }
    out.clear();
    return out;
  }
  out.reserve(raw.size());
  for (const auto &det : raw) {
    out.push_back({det.box, det.confidence});
  }
  if (vulkanMs) {
    *vulkanMs = static_cast<double>(timer.nsecsElapsed()) / 1'000'000.0;
  }
  return out;
}
