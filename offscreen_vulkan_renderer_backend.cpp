#include "offscreen_vulkan_renderer_backend.h"

#include "background_fill_effect.h"
#include "cpu_overlay_render_backend.h"
#include "editor_shared_effects.h"
#include "editor_shared_timing.h"
#include "offscreen_vulkan_renderer_helpers.h"
#include "preview_view_transform.h"
#include "render_internal.h"
#include "render_vulkan_shared.h"
#include "titles.h"
#include "vulkan_detector_frame_handoff.h"
#include "vulkan_mask_preprocessor.h"
#include "vulkan_shader_paths.h"
#include "vulkan_staging_flush_range.h"
#include "vulkan_text_renderer.h"

#include <QDateTime>
#include <QDebug>
#include <QDir>
#include <QFile>
#include <QHash>
#include <QImage>
#include <QScopeGuard>
#include <QSet>

#if JCUT_HAS_CUDA_DRIVER
extern "C" {
#include <libavutil/hwcontext.h>
#include <libavutil/hwcontext_cuda.h>
}
#include <cuda.h>
#endif
#include <cmath>
#include <cstring>
#include <limits>
#include <unistd.h>
#include <vulkan/vulkan.h>

namespace render_detail {
namespace {

constexpr std::uint64_t kExportGpuFenceTimeoutNs = 5'000'000'000ull;
std::atomic<std::uint64_t> g_nextOffscreenProducerSessionId{1};

QJsonObject rectDiagnosticObject(const QRectF &rect) {
  return QJsonObject{{QStringLiteral("x"), rect.x()},
                     {QStringLiteral("y"), rect.y()},
                     {QStringLiteral("width"), rect.width()},
                     {QStringLiteral("height"), rect.height()}};
}

QJsonArray rectDiagnosticArray(const QVector<QRectF> &rects) {
  QJsonArray array;
  for (const QRectF &rect : rects) {
    array.push_back(rectDiagnosticObject(rect));
  }
  return array;
}

QString renderTranscriptPath(const TimelineClip &clip) {
  return activeTranscriptPathForClip(clip);
}

VkRect2D scissorFromRect(const QRectF& rect, const QSize& outputSize) {
  const int outputWidth = qMax(1, outputSize.width());
  const int outputHeight = qMax(1, outputSize.height());
  const int left = qBound(0, static_cast<int>(std::floor(rect.left())), outputWidth);
  const int top = qBound(0, static_cast<int>(std::floor(rect.top())), outputHeight);
  const int right = qBound(left, static_cast<int>(std::ceil(rect.right())), outputWidth);
  const int bottom = qBound(top, static_cast<int>(std::ceil(rect.bottom())), outputHeight);
  VkRect2D scissor{};
  scissor.offset = {left, top};
  scissor.extent = {
      static_cast<uint32_t>(qMax(0, right - left)),
      static_cast<uint32_t>(qMax(0, bottom - top))};
  return scissor;
}

QRectF faceTargetRectFromTransformDiagnostics(const QJsonObject &diagnostics) {
  const QJsonObject target =
      diagnostics.value(QStringLiteral("target_box_norm")).toObject();
  const QJsonObject output =
      diagnostics.value(QStringLiteral("output_size")).toObject();
  const qreal outputWidth = qMax<qreal>(
      1.0, output.value(QStringLiteral("width")).toDouble(0.0));
  const qreal outputHeight = qMax<qreal>(
      1.0, output.value(QStringLiteral("height")).toDouble(0.0));
  const qreal outputMinSide = qMax<qreal>(1.0, qMin(outputWidth, outputHeight));
  const qreal targetSide =
      qMax<qreal>(1.0, target.value(QStringLiteral("box")).toDouble(0.0) *
                            outputMinSide);
  const QPointF center(qBound<qreal>(
                           0.0, target.value(QStringLiteral("x")).toDouble(0.0),
                           1.0) *
                           outputWidth,
                       qBound<qreal>(
                           0.0, target.value(QStringLiteral("y")).toDouble(0.0),
                           1.0) *
                           outputHeight);
  return QRectF(center.x() - (targetSide * 0.5),
                center.y() - (targetSide * 0.5), targetSide, targetSide);
}

struct FrameUniformData {
  float outputSizeAndInverse[4] = {1.0f, 1.0f, 1.0f, 1.0f};
  float backgroundShadows[4] = {0.0f, 0.0f, 0.0f, 0.0f};
  float backgroundMidtones[4] = {0.0f, 0.0f, 0.0f, 0.0f};
  float backgroundHighlights[4] = {0.0f, 0.0f, 0.0f, 0.0f};
  float backgroundGrade[4] = {0.0f, 1.0f, 1.0f, 0.0f};
  float effectParams[4] = {0.0f, 0.0f, 0.0f, 0.0f};
};

static_assert(sizeof(FrameUniformData) == sizeof(float) * 24);

constexpr int kFrameUniformRingCount = 4096;

VkDeviceSize frameUniformStrideForDevice(VkPhysicalDevice physicalDevice)
{
  VkPhysicalDeviceProperties properties{};
  vkGetPhysicalDeviceProperties(physicalDevice, &properties);
  const VkDeviceSize alignment = properties.limits.minUniformBufferOffsetAlignment;
  VkDeviceSize stride = sizeof(FrameUniformData);
  if (alignment > 0) {
    const VkDeviceSize remainder = stride % alignment;
    if (remainder != 0) {
      stride += alignment - remainder;
    }
  }
  return stride;
}

} // namespace

class OffscreenVulkanRendererPrivate {
public:
  static constexpr int kMaxLayerTextures = 12;
  static constexpr int kFrameSlots = 3;
  struct FrameSlot {
    VkCommandBuffer commandBuffer = VK_NULL_HANDLE;
    VkBuffer stagingBuffer = VK_NULL_HANDLE;
    VkDeviceMemory stagingMemory = VK_NULL_HANDLE;
    VkDeviceSize stagingBufferSize = 0;
    VkDeviceSize stagingAllocationSize = 0;
    void *stagingMapped = nullptr;
    VkBuffer cudaExportBuffer = VK_NULL_HANDLE;
    VkDeviceMemory cudaExportMemory = VK_NULL_HANDLE;
    VkDeviceSize cudaExportAllocationSize = 0;
#if JCUT_HAS_CUDA_DRIVER
    CUexternalMemory cudaExternalMemory = nullptr;
    CUdeviceptr cudaExternalDevicePtr = 0;
    CUcontext cudaImportContext = nullptr;
    AVBufferRef *cudaImportDeviceRef = nullptr;
#endif
    VkFence fence = VK_NULL_HANDLE;
    bool stagingHostCoherent = false;
    bool inFlight = false;
  };
  struct PreviewSlot {
    VkImage image = VK_NULL_HANDLE;
    VkDeviceMemory memory = VK_NULL_HANDLE;
    VkImageView view = VK_NULL_HANDLE;
    VkSemaphore readySemaphore = VK_NULL_HANDLE;
    VkSemaphore consumedSemaphore = VK_NULL_HANDLE;
    VkImageLayout layout = VK_IMAGE_LAYOUT_UNDEFINED;
    uint32_t memoryTypeIndex = UINT32_MAX;
    VkDeviceSize memoryAllocationSize = 0;
    std::uint64_t generation = 0;
    std::shared_ptr<OffscreenVulkanFrameConsumptionState> consumptionState =
        std::make_shared<OffscreenVulkanFrameConsumptionState>();
    bool published = false;
    bool handlesExported = false;
  };
  struct LayerTextureSlot {
    VkImage image = VK_NULL_HANDLE;
    VkDeviceMemory memory = VK_NULL_HANDLE;
    VkImageView view = VK_NULL_HANDLE;
    VkImage curveLutImage = VK_NULL_HANDLE;
    VkDeviceMemory curveLutMemory = VK_NULL_HANDLE;
    VkImageView curveLutView = VK_NULL_HANDLE;
    VkImage maskCurveLutImage = VK_NULL_HANDLE;
    VkDeviceMemory maskCurveLutMemory = VK_NULL_HANDLE;
    VkImageView maskCurveLutView = VK_NULL_HANDLE;
    VkImage maskImage = VK_NULL_HANDLE;
    VkDeviceMemory maskMemory = VK_NULL_HANDLE;
    VkImageView maskView = VK_NULL_HANDLE;
    VkImage maskRawImage = VK_NULL_HANDLE;
    VkDeviceMemory maskRawMemory = VK_NULL_HANDLE;
    VkImageView maskRawView = VK_NULL_HANDLE;
    QSize maskRawSize;
    VkFormat maskRawFormat = VK_FORMAT_UNDEFINED;
    VkImage maskWorkImage = VK_NULL_HANDLE;
    VkDeviceMemory maskWorkMemory = VK_NULL_HANDLE;
    VkImageView maskWorkView = VK_NULL_HANDLE;
    VkImageLayout maskLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    VkImageLayout maskRawLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    VkImageLayout maskWorkLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    VkDescriptorSet descriptorSet = VK_NULL_HANDLE;
    bool uploaded = false;
    bool curveUploaded = false;
    bool maskCurveUploaded = false;
    bool maskUploaded = false;
    std::shared_ptr<jcut::vulkan_detector::VulkanDetectorFrameHandoff>
        hardwareFrameHandoff;
    std::shared_ptr<jcut::vulkan_detector::VulkanDetectorFrameHandoff>
        referenceFrameHandoff;
  };
  ~OffscreenVulkanRendererPrivate() { release(); }

  bool initialize(const QSize &outputSize, QString *errorMessage) {
    release();
    m_producerSessionId =
        g_nextOffscreenProducerSessionId.fetch_add(
            1, std::memory_order_relaxed);

    m_outputSize =
        QSize(qMax(16, outputSize.width()), qMax(16, outputSize.height()));
    m_cudaExternalMemoryStatus = QStringLiteral("initializing");

    VkApplicationInfo appInfo{};
    appInfo.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
    appInfo.pApplicationName = "JCut Offscreen Vulkan Renderer";
    appInfo.applicationVersion = VK_MAKE_VERSION(1, 0, 0);
    appInfo.pEngineName = "JCut";
    appInfo.engineVersion = VK_MAKE_VERSION(1, 0, 0);
    appInfo.apiVersion = VK_API_VERSION_1_1;

    VkInstanceCreateInfo instanceInfo{};
    instanceInfo.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
    instanceInfo.pApplicationInfo = &appInfo;

    if (vkCreateInstance(&instanceInfo, nullptr, &m_instance) != VK_SUCCESS) {
      if (errorMessage) {
        *errorMessage = QStringLiteral("Failed to create Vulkan instance.");
      }
      return false;
    }

    uint32_t physicalDeviceCount = 0;
    vkEnumeratePhysicalDevices(m_instance, &physicalDeviceCount, nullptr);
    if (physicalDeviceCount == 0) {
      if (errorMessage) {
        *errorMessage = QStringLiteral("No Vulkan physical devices found.");
      }
      return false;
    }

    QVector<VkPhysicalDevice> devices(static_cast<int>(physicalDeviceCount));
    vkEnumeratePhysicalDevices(m_instance, &physicalDeviceCount,
                               devices.data());

    int bestScore = -1;
    VkPhysicalDevice bestDevice = VK_NULL_HANDLE;
    uint32_t bestQueueFamily = UINT32_MAX;
    VkPhysicalDeviceProperties bestProperties{};
    for (VkPhysicalDevice candidate : devices) {
      VkPhysicalDeviceProperties properties{};
      vkGetPhysicalDeviceProperties(candidate, &properties);
      uint32_t queueFamilyCount = 0;
      vkGetPhysicalDeviceQueueFamilyProperties(candidate, &queueFamilyCount,
                                               nullptr);
      QVector<VkQueueFamilyProperties> queueFamilies(
          static_cast<int>(queueFamilyCount));
      vkGetPhysicalDeviceQueueFamilyProperties(candidate, &queueFamilyCount,
                                               queueFamilies.data());
      for (uint32_t i = 0; i < queueFamilyCount; ++i) {
        if (queueFamilies[static_cast<int>(i)].queueFlags &
            VK_QUEUE_GRAPHICS_BIT) {
          int score = 0;
          if (properties.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU) {
            score += 1000;
          } else if (properties.deviceType ==
                     VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU) {
            score += 500;
          } else if (properties.deviceType == VK_PHYSICAL_DEVICE_TYPE_CPU) {
            score -= 1000;
          }
          if (properties.vendorID == 0x10de) {
            score += 100;
          }
          if (score > bestScore) {
            bestScore = score;
            bestDevice = candidate;
            bestQueueFamily = i;
            bestProperties = properties;
          }
        }
      }
    }
    m_physicalDevice = bestDevice;
    m_graphicsQueueFamily = bestQueueFamily;
    m_nonCoherentAtomSize =
        qMax<VkDeviceSize>(1, bestProperties.limits.nonCoherentAtomSize);
    m_storageBufferOffsetAlignment = qMax<VkDeviceSize>(
        16, bestProperties.limits.minStorageBufferOffsetAlignment);

    if (m_physicalDevice == VK_NULL_HANDLE) {
      if (errorMessage) {
        *errorMessage =
            QStringLiteral("No Vulkan graphics queue family found.");
      }
      return false;
    }
    qInfo().noquote() << QStringLiteral(
                             "Offscreen Vulkan device: %1 vendor=0x%2 type=%3")
                             .arg(QString::fromUtf8(bestProperties.deviceName))
                             .arg(bestProperties.vendorID, 0, 16)
                             .arg(static_cast<int>(bestProperties.deviceType));
    m_externalMemoryFdSupported =
        physicalDeviceSupportsExtension(
            m_physicalDevice, VK_KHR_EXTERNAL_MEMORY_EXTENSION_NAME) &&
        physicalDeviceSupportsExtension(
            m_physicalDevice, VK_KHR_EXTERNAL_MEMORY_FD_EXTENSION_NAME);
    m_externalSemaphoreFdSupported =
        physicalDeviceSupportsExtension(
            m_physicalDevice, VK_KHR_EXTERNAL_SEMAPHORE_EXTENSION_NAME) &&
        physicalDeviceSupportsExtension(
            m_physicalDevice, VK_KHR_EXTERNAL_SEMAPHORE_FD_EXTENSION_NAME);

    const float queuePriority = 1.0f;
    VkDeviceQueueCreateInfo queueInfo{};
    queueInfo.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
    queueInfo.queueFamilyIndex = m_graphicsQueueFamily;
    queueInfo.queueCount = 1;
    queueInfo.pQueuePriorities = &queuePriority;

    VkDeviceCreateInfo deviceInfo{};
    deviceInfo.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
    deviceInfo.queueCreateInfoCount = 1;
    deviceInfo.pQueueCreateInfos = &queueInfo;
    QVector<const char *> enabledDeviceExtensions;
    if (m_externalMemoryFdSupported) {
      enabledDeviceExtensions.push_back(VK_KHR_EXTERNAL_MEMORY_EXTENSION_NAME);
      enabledDeviceExtensions.push_back(
          VK_KHR_EXTERNAL_MEMORY_FD_EXTENSION_NAME);
    }
    if (m_externalSemaphoreFdSupported) {
      enabledDeviceExtensions.push_back(
          VK_KHR_EXTERNAL_SEMAPHORE_EXTENSION_NAME);
      enabledDeviceExtensions.push_back(
          VK_KHR_EXTERNAL_SEMAPHORE_FD_EXTENSION_NAME);
    }
    deviceInfo.enabledExtensionCount =
        static_cast<uint32_t>(enabledDeviceExtensions.size());
    deviceInfo.ppEnabledExtensionNames =
        enabledDeviceExtensions.isEmpty() ? nullptr
                                          : enabledDeviceExtensions.constData();

    if (vkCreateDevice(m_physicalDevice, &deviceInfo, nullptr, &m_device) !=
        VK_SUCCESS) {
      if (errorMessage) {
        *errorMessage =
            QStringLiteral("Failed to create Vulkan logical device.");
      }
      return false;
    }

    vkGetDeviceQueue(m_device, m_graphicsQueueFamily, 0, &m_graphicsQueue);
    uint32_t selectedQueueFamilyCount = 0;
    vkGetPhysicalDeviceQueueFamilyProperties(
        m_physicalDevice, &selectedQueueFamilyCount, nullptr);
    QVector<VkQueueFamilyProperties> selectedFamilies(
        static_cast<int>(selectedQueueFamilyCount));
    vkGetPhysicalDeviceQueueFamilyProperties(
        m_physicalDevice, &selectedQueueFamilyCount, selectedFamilies.data());
    m_graphicsQueueSupportsCompute =
        m_graphicsQueueFamily <
            static_cast<uint32_t>(selectedFamilies.size()) &&
        (selectedFamilies[static_cast<int>(m_graphicsQueueFamily)].queueFlags &
         VK_QUEUE_COMPUTE_BIT);
    m_vkGetMemoryFdKHR = reinterpret_cast<PFN_vkGetMemoryFdKHR>(
        vkGetDeviceProcAddr(m_device, "vkGetMemoryFdKHR"));
    m_vkGetSemaphoreFdKHR = reinterpret_cast<PFN_vkGetSemaphoreFdKHR>(
        vkGetDeviceProcAddr(m_device, "vkGetSemaphoreFdKHR"));
    qInfo().noquote()
        << QStringLiteral(
               "Vulkan/CUDA interop capability: external_memory_fd=%1 "
               "external_semaphore_fd=%2 get_memory_fd=%3 get_semaphore_fd=%4")
               .arg(m_externalMemoryFdSupported ? QStringLiteral("yes")
                                                : QStringLiteral("no"),
                    m_externalSemaphoreFdSupported ? QStringLiteral("yes")
                                                   : QStringLiteral("no"),
                    m_vkGetMemoryFdKHR ? QStringLiteral("yes")
                                       : QStringLiteral("no"),
                    m_vkGetSemaphoreFdKHR ? QStringLiteral("yes")
                                          : QStringLiteral("no"));

    VkCommandPoolCreateInfo poolInfo{};
    poolInfo.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
    poolInfo.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    poolInfo.queueFamilyIndex = m_graphicsQueueFamily;

    if (vkCreateCommandPool(m_device, &poolInfo, nullptr, &m_commandPool) !=
        VK_SUCCESS) {
      if (errorMessage) {
        *errorMessage = QStringLiteral("Failed to create Vulkan command pool.");
      }
      return false;
    }

    m_frameSlots.resize(kFrameSlots);
    QVector<VkCommandBuffer> commandBuffers(kFrameSlots);
    VkCommandBufferAllocateInfo allocInfo{};
    allocInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    allocInfo.commandPool = m_commandPool;
    allocInfo.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    allocInfo.commandBufferCount = kFrameSlots;

    if (vkAllocateCommandBuffers(m_device, &allocInfo, commandBuffers.data()) !=
        VK_SUCCESS) {
      if (errorMessage) {
        *errorMessage =
            QStringLiteral("Failed to allocate Vulkan command buffers.");
      }
      return false;
    }
    for (int i = 0; i < kFrameSlots; ++i) {
      m_frameSlots[i].commandBuffer = commandBuffers[i];
    }

    VkImageCreateInfo imageInfo{};
    imageInfo.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    imageInfo.imageType = VK_IMAGE_TYPE_2D;
    imageInfo.extent.width = static_cast<uint32_t>(m_outputSize.width());
    imageInfo.extent.height = static_cast<uint32_t>(m_outputSize.height());
    imageInfo.extent.depth = 1;
    imageInfo.mipLevels = 1;
    imageInfo.arrayLayers = 1;
    imageInfo.format = VK_FORMAT_B8G8R8A8_UNORM;
    imageInfo.tiling = VK_IMAGE_TILING_OPTIMAL;
    imageInfo.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    imageInfo.usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT |
                      VK_IMAGE_USAGE_TRANSFER_DST_BIT |
                      VK_IMAGE_USAGE_TRANSFER_SRC_BIT |
                      VK_IMAGE_USAGE_SAMPLED_BIT;
    imageInfo.samples = VK_SAMPLE_COUNT_1_BIT;
    imageInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    VkExternalMemoryImageCreateInfo externalImageInfo{};
    if (m_externalMemoryFdSupported && m_vkGetMemoryFdKHR) {
      externalImageInfo.sType =
          VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_IMAGE_CREATE_INFO;
      externalImageInfo.handleTypes =
          VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_FD_BIT;
      imageInfo.pNext = &externalImageInfo;
    }

    if (vkCreateImage(m_device, &imageInfo, nullptr, &m_colorImage) !=
        VK_SUCCESS) {
      if (errorMessage) {
        *errorMessage =
            QStringLiteral("Failed to create Vulkan offscreen image.");
      }
      return false;
    }

    VkMemoryRequirements memRequirements{};
    vkGetImageMemoryRequirements(m_device, m_colorImage, &memRequirements);

    const uint32_t memoryTypeIndex =
        findMemoryType(m_physicalDevice, memRequirements.memoryTypeBits,
                       VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    if (memoryTypeIndex == UINT32_MAX) {
      if (errorMessage) {
        *errorMessage = QStringLiteral(
            "No suitable Vulkan memory type for offscreen image.");
      }
      return false;
    }

    VkMemoryAllocateInfo allocMemoryInfo{};
    allocMemoryInfo.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    allocMemoryInfo.allocationSize = memRequirements.size;
    allocMemoryInfo.memoryTypeIndex = memoryTypeIndex;
    VkExportMemoryAllocateInfo exportAllocInfo{};
    if (m_externalMemoryFdSupported && m_vkGetMemoryFdKHR) {
      exportAllocInfo.sType = VK_STRUCTURE_TYPE_EXPORT_MEMORY_ALLOCATE_INFO;
      exportAllocInfo.handleTypes =
          VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_FD_BIT;
      allocMemoryInfo.pNext = &exportAllocInfo;
    }

    if (vkAllocateMemory(m_device, &allocMemoryInfo, nullptr,
                         &m_colorImageMemory) != VK_SUCCESS) {
      if (errorMessage) {
        *errorMessage =
            QStringLiteral("Failed to allocate Vulkan image memory.");
      }
      return false;
    }

    if (vkBindImageMemory(m_device, m_colorImage, m_colorImageMemory, 0) !=
        VK_SUCCESS) {
      if (errorMessage) {
        *errorMessage = QStringLiteral("Failed to bind Vulkan image memory.");
      }
      return false;
    }

    VkImageViewCreateInfo viewInfo{};
    viewInfo.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    viewInfo.image = m_colorImage;
    viewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
    viewInfo.format = VK_FORMAT_B8G8R8A8_UNORM;
    viewInfo.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    viewInfo.subresourceRange.baseMipLevel = 0;
    viewInfo.subresourceRange.levelCount = 1;
    viewInfo.subresourceRange.baseArrayLayer = 0;
    viewInfo.subresourceRange.layerCount = 1;

    if (vkCreateImageView(m_device, &viewInfo, nullptr, &m_colorImageView) !=
        VK_SUCCESS) {
      if (errorMessage) {
        *errorMessage = QStringLiteral("Failed to create Vulkan image view.");
      }
      return false;
    }

    if (m_externalMemoryFdSupported &&
        m_externalSemaphoreFdSupported &&
        m_vkGetMemoryFdKHR &&
        m_vkGetSemaphoreFdKHR) {
      m_previewSlots.resize(3);
      bool previewSlotsReady = true;
      for (PreviewSlot &slot : m_previewSlots) {
        VkImageCreateInfo previewImageInfo = imageInfo;
        previewImageInfo.pNext = &externalImageInfo;
        if (vkCreateImage(m_device, &previewImageInfo, nullptr, &slot.image) !=
            VK_SUCCESS) {
          previewSlotsReady = false;
          break;
        }
        VkMemoryRequirements previewRequirements{};
        vkGetImageMemoryRequirements(m_device, slot.image,
                                     &previewRequirements);
        slot.memoryAllocationSize = previewRequirements.size;
        const uint32_t previewMemoryType =
            findMemoryType(m_physicalDevice,
                           previewRequirements.memoryTypeBits,
                           VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
        slot.memoryTypeIndex = previewMemoryType;
        VkExportMemoryAllocateInfo previewExport{};
        previewExport.sType = VK_STRUCTURE_TYPE_EXPORT_MEMORY_ALLOCATE_INFO;
        previewExport.handleTypes =
            VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_FD_BIT;
        VkMemoryDedicatedAllocateInfo previewDedicated{};
        previewDedicated.sType =
            VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO;
        previewDedicated.image = slot.image;
        previewExport.pNext = &previewDedicated;
        VkMemoryAllocateInfo previewAllocation{};
        previewAllocation.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
        previewAllocation.pNext = &previewExport;
        previewAllocation.allocationSize = previewRequirements.size;
        previewAllocation.memoryTypeIndex = previewMemoryType;
        if (previewMemoryType == UINT32_MAX ||
            vkAllocateMemory(m_device, &previewAllocation, nullptr,
                             &slot.memory) != VK_SUCCESS ||
            vkBindImageMemory(m_device, slot.image, slot.memory, 0) !=
                VK_SUCCESS) {
          previewSlotsReady = false;
          break;
        }
        VkImageViewCreateInfo previewViewInfo = viewInfo;
        previewViewInfo.image = slot.image;
        if (vkCreateImageView(m_device, &previewViewInfo, nullptr,
                              &slot.view) != VK_SUCCESS) {
          previewSlotsReady = false;
          break;
        }
        VkExportSemaphoreCreateInfo semaphoreExport{};
        semaphoreExport.sType =
            VK_STRUCTURE_TYPE_EXPORT_SEMAPHORE_CREATE_INFO;
        semaphoreExport.handleTypes =
            VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_OPAQUE_FD_BIT;
        VkSemaphoreCreateInfo semaphoreInfo{};
        semaphoreInfo.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;
        semaphoreInfo.pNext = &semaphoreExport;
        if (vkCreateSemaphore(m_device, &semaphoreInfo, nullptr,
                              &slot.readySemaphore) != VK_SUCCESS ||
            vkCreateSemaphore(m_device, &semaphoreInfo, nullptr,
                              &slot.consumedSemaphore) != VK_SUCCESS) {
          previewSlotsReady = false;
          break;
        }
      }
      if (!previewSlotsReady) {
        qWarning() << "GPU export preview double buffers are unavailable.";
        destroyPreviewSlots();
      }
    }

    VkSamplerCreateInfo samplerInfo{};
    samplerInfo.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
    samplerInfo.magFilter = VK_FILTER_LINEAR;
    samplerInfo.minFilter = VK_FILTER_LINEAR;
    samplerInfo.mipmapMode = VK_SAMPLER_MIPMAP_MODE_LINEAR;
    samplerInfo.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    samplerInfo.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    samplerInfo.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    samplerInfo.maxAnisotropy = 1.0f;

    if (vkCreateSampler(m_device, &samplerInfo, nullptr, &m_sampler) !=
        VK_SUCCESS) {
      if (errorMessage) {
        *errorMessage = QStringLiteral("Failed to create Vulkan sampler.");
      }
      return false;
    }

    VkBufferCreateInfo frameUniformBufferInfo{};
    frameUniformBufferInfo.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    m_frameUniformStride = frameUniformStrideForDevice(m_physicalDevice);
    frameUniformBufferInfo.size = m_frameUniformStride * kFrameUniformRingCount;
    frameUniformBufferInfo.usage = VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT;
    frameUniformBufferInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    if (vkCreateBuffer(m_device, &frameUniformBufferInfo, nullptr, &m_frameUniformBuffer) != VK_SUCCESS) {
      return false;
    }
    VkMemoryRequirements frameUniformRequirements{};
    vkGetBufferMemoryRequirements(m_device, m_frameUniformBuffer, &frameUniformRequirements);
    VkMemoryAllocateInfo frameUniformAlloc{};
    frameUniformAlloc.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    frameUniformAlloc.allocationSize = frameUniformRequirements.size;
    frameUniformAlloc.memoryTypeIndex = findMemoryType(
        m_physicalDevice,
        frameUniformRequirements.memoryTypeBits,
        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    if (vkAllocateMemory(m_device, &frameUniformAlloc, nullptr, &m_frameUniformMemory) != VK_SUCCESS ||
        vkBindBufferMemory(m_device, m_frameUniformBuffer, m_frameUniformMemory, 0) != VK_SUCCESS ||
        vkMapMemory(m_device, m_frameUniformMemory, 0, frameUniformBufferInfo.size, 0, &m_frameUniformMapped) != VK_SUCCESS) {
      return false;
    }
    FrameUniformData frameUniformValues;
    frameUniformValues.outputSizeAndInverse[0] = static_cast<float>(qMax(1, m_outputSize.width()));
    frameUniformValues.outputSizeAndInverse[1] = static_cast<float>(qMax(1, m_outputSize.height()));
    frameUniformValues.outputSizeAndInverse[2] = 1.0f / frameUniformValues.outputSizeAndInverse[0];
    frameUniformValues.outputSizeAndInverse[3] = 1.0f / frameUniformValues.outputSizeAndInverse[1];
    std::memcpy(m_frameUniformMapped, &frameUniformValues, sizeof(frameUniformValues));

    VkDescriptorSetLayoutBinding descriptorBindings[5]{};
    descriptorBindings[0].binding = 0;
    descriptorBindings[0].descriptorType =
        VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    descriptorBindings[0].descriptorCount = 1;
    descriptorBindings[0].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
    descriptorBindings[1].binding = 1;
    descriptorBindings[1].descriptorType =
        VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    descriptorBindings[1].descriptorCount = 1;
    descriptorBindings[1].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
    descriptorBindings[2].binding = 2;
    descriptorBindings[2].descriptorType =
        VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    descriptorBindings[2].descriptorCount = 1;
    descriptorBindings[2].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
    descriptorBindings[3].binding = 3;
    descriptorBindings[3].descriptorType =
        VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    descriptorBindings[3].descriptorCount = 1;
    descriptorBindings[3].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
    descriptorBindings[4].binding = 4;
    descriptorBindings[4].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC;
    descriptorBindings[4].descriptorCount = 1;
    descriptorBindings[4].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;

    VkDescriptorSetLayoutCreateInfo descriptorSetLayoutInfo{};
    descriptorSetLayoutInfo.sType =
        VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    descriptorSetLayoutInfo.bindingCount = 5;
    descriptorSetLayoutInfo.pBindings = descriptorBindings;

    if (vkCreateDescriptorSetLayout(m_device, &descriptorSetLayoutInfo, nullptr,
                                    &m_descriptorSetLayout) != VK_SUCCESS) {
      if (errorMessage) {
        *errorMessage =
            QStringLiteral("Failed to create Vulkan descriptor set layout.");
      }
      return false;
    }

    VkDescriptorPoolSize poolSizes[2]{};
    poolSizes[0].type = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    poolSizes[0].descriptorCount = (1 + kMaxLayerTextures) * 4;
    poolSizes[1].type = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC;
    poolSizes[1].descriptorCount = 1 + kMaxLayerTextures;

    VkDescriptorPoolCreateInfo descriptorPoolInfo{};
    descriptorPoolInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    descriptorPoolInfo.poolSizeCount = 2;
    descriptorPoolInfo.pPoolSizes = poolSizes;
    descriptorPoolInfo.maxSets = 1 + kMaxLayerTextures;

    if (vkCreateDescriptorPool(m_device, &descriptorPoolInfo, nullptr,
                               &m_descriptorPool) != VK_SUCCESS) {
      if (errorMessage) {
        *errorMessage =
            QStringLiteral("Failed to create Vulkan descriptor pool.");
      }
      return false;
    }

    VkDescriptorSetAllocateInfo descriptorSetAllocInfo{};
    descriptorSetAllocInfo.sType =
        VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
    descriptorSetAllocInfo.descriptorPool = m_descriptorPool;
    descriptorSetAllocInfo.descriptorSetCount = 1;
    descriptorSetAllocInfo.pSetLayouts = &m_descriptorSetLayout;

    if (vkAllocateDescriptorSets(m_device, &descriptorSetAllocInfo,
                                 &m_descriptorSet) != VK_SUCCESS) {
      if (errorMessage) {
        *errorMessage =
            QStringLiteral("Failed to allocate Vulkan descriptor set.");
      }
      return false;
    }

    auto createDeviceImage =
        [&](uint32_t width, uint32_t height, VkFormat format,
            VkImageUsageFlags usage, VkImage *image, VkDeviceMemory *memory,
            VkImageView *view, const QString &failurePrefix,
            QString *err) -> bool {
      VkImageCreateInfo imageInfo{};
      imageInfo.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
      imageInfo.imageType = VK_IMAGE_TYPE_2D;
      imageInfo.extent.width = width;
      imageInfo.extent.height = height;
      imageInfo.extent.depth = 1;
      imageInfo.mipLevels = 1;
      imageInfo.arrayLayers = 1;
      imageInfo.format = format;
      imageInfo.tiling = VK_IMAGE_TILING_OPTIMAL;
      imageInfo.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
      imageInfo.usage = usage;
      imageInfo.samples = VK_SAMPLE_COUNT_1_BIT;
      imageInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
      if (vkCreateImage(m_device, &imageInfo, nullptr, image) != VK_SUCCESS) {
        if (err)
          *err = failurePrefix + QStringLiteral(": create image failed.");
        return false;
      }
      VkMemoryRequirements req{};
      vkGetImageMemoryRequirements(m_device, *image, &req);
      const uint32_t memType =
          findMemoryType(m_physicalDevice, req.memoryTypeBits,
                         VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
      if (memType == UINT32_MAX) {
        if (err)
          *err = failurePrefix + QStringLiteral(": no image memory type.");
        return false;
      }
      VkMemoryAllocateInfo ai{};
      ai.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
      ai.allocationSize = req.size;
      ai.memoryTypeIndex = memType;
      if (vkAllocateMemory(m_device, &ai, nullptr, memory) != VK_SUCCESS ||
          vkBindImageMemory(m_device, *image, *memory, 0) != VK_SUCCESS) {
        if (err)
          *err = failurePrefix +
                 QStringLiteral(": allocate/bind image memory failed.");
        return false;
      }
      VkImageViewCreateInfo vi{};
      vi.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
      vi.image = *image;
      vi.viewType = VK_IMAGE_VIEW_TYPE_2D;
      vi.format = format;
      vi.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
      vi.subresourceRange.levelCount = 1;
      vi.subresourceRange.layerCount = 1;
      if (vkCreateImageView(m_device, &vi, nullptr, view) != VK_SUCCESS) {
        if (err)
          *err = failurePrefix + QStringLiteral(": create image view failed.");
        return false;
      }
      return true;
    };

    auto createLayerSlot = [&](QString *err) -> bool {
      LayerTextureSlot slot;
      if (!createDeviceImage(static_cast<uint32_t>(m_outputSize.width()),
                             static_cast<uint32_t>(m_outputSize.height()),
                             VK_FORMAT_R8G8B8A8_UNORM,
                             VK_IMAGE_USAGE_TRANSFER_DST_BIT |
                                 VK_IMAGE_USAGE_SAMPLED_BIT,
                             &slot.image, &slot.memory, &slot.view,
                             QStringLiteral("Vulkan layer image"), err)) {
        return false;
      }
      if (!createDeviceImage(
              kCurveLutWidth, kCurveLutHeight, VK_FORMAT_R8G8B8A8_UNORM,
              VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
              &slot.curveLutImage, &slot.curveLutMemory, &slot.curveLutView,
              QStringLiteral("Vulkan layer curve LUT"), err)) {
        return false;
      }
      if (!createDeviceImage(
              kCurveLutWidth, kCurveLutHeight, VK_FORMAT_R8G8B8A8_UNORM,
              VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
              &slot.maskCurveLutImage, &slot.maskCurveLutMemory, &slot.maskCurveLutView,
              QStringLiteral("Vulkan layer mask curve LUT"), err)) {
        return false;
      }
      if (!createDeviceImage(static_cast<uint32_t>(m_outputSize.width()),
                             static_cast<uint32_t>(m_outputSize.height()),
                             VK_FORMAT_R8G8B8A8_UNORM,
                             VK_IMAGE_USAGE_TRANSFER_DST_BIT |
                                 VK_IMAGE_USAGE_STORAGE_BIT |
                                 VK_IMAGE_USAGE_SAMPLED_BIT,
                             &slot.maskImage, &slot.maskMemory, &slot.maskView,
                             QStringLiteral("Vulkan layer mask image"), err)) {
        return false;
      }
      if (!createDeviceImage(static_cast<uint32_t>(m_outputSize.width()),
                             static_cast<uint32_t>(m_outputSize.height()),
                             VK_FORMAT_R8G8B8A8_UNORM,
                             VK_IMAGE_USAGE_TRANSFER_DST_BIT |
                                 VK_IMAGE_USAGE_STORAGE_BIT |
                                 VK_IMAGE_USAGE_SAMPLED_BIT,
                             &slot.maskRawImage, &slot.maskRawMemory, &slot.maskRawView,
                             QStringLiteral("Vulkan layer raw mask image"), err)) {
        return false;
      }
      if (!createDeviceImage(static_cast<uint32_t>(m_outputSize.width()),
                             static_cast<uint32_t>(m_outputSize.height()),
                             VK_FORMAT_R8G8B8A8_UNORM,
                             VK_IMAGE_USAGE_TRANSFER_DST_BIT |
                                 VK_IMAGE_USAGE_STORAGE_BIT |
                                 VK_IMAGE_USAGE_SAMPLED_BIT,
                             &slot.maskWorkImage, &slot.maskWorkMemory, &slot.maskWorkView,
                             QStringLiteral("Vulkan layer work mask image"), err)) {
        return false;
      }
      VkDescriptorSetAllocateInfo dsi{};
      dsi.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
      dsi.descriptorPool = m_descriptorPool;
      dsi.descriptorSetCount = 1;
      dsi.pSetLayouts = &m_descriptorSetLayout;
      if (vkAllocateDescriptorSets(m_device, &dsi, &slot.descriptorSet) !=
          VK_SUCCESS) {
        if (err)
          *err =
              QStringLiteral("Failed to allocate Vulkan layer descriptor set.");
        return false;
      }
      VkDescriptorImageInfo di[4]{};
      di[0].imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
      di[0].imageView = slot.view;
      di[0].sampler = m_sampler;
      di[1].imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
      di[1].imageView = slot.curveLutView;
      di[1].sampler = m_sampler;
      di[2].imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
      di[2].imageView = slot.maskView;
      di[2].sampler = m_sampler;
      di[3].imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
      di[3].imageView = slot.maskCurveLutView;
      di[3].sampler = m_sampler;
      VkDescriptorBufferInfo frameUniformInfo{};
      frameUniformInfo.buffer = m_frameUniformBuffer;
      frameUniformInfo.range = sizeof(FrameUniformData);
      VkWriteDescriptorSet writes[5]{};
      for (uint32_t binding = 0; binding < 4; ++binding) {
        writes[binding].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        writes[binding].dstSet = slot.descriptorSet;
        writes[binding].dstBinding = binding;
        writes[binding].descriptorCount = 1;
        writes[binding].descriptorType =
            VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        writes[binding].pImageInfo = &di[binding];
      }
      writes[4].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
      writes[4].dstSet = slot.descriptorSet;
      writes[4].dstBinding = 4;
      writes[4].descriptorCount = 1;
      writes[4].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC;
      writes[4].pBufferInfo = &frameUniformInfo;
      vkUpdateDescriptorSets(m_device, 5, writes, 0, nullptr);
      m_layerSlots.push_back(slot);
      return true;
    };
    for (int i = 0; i < kMaxLayerTextures; ++i) {
      if (!createLayerSlot(errorMessage)) {
        return false;
      }
    }

    VkAttachmentDescription colorAttachment{};
    colorAttachment.format = VK_FORMAT_B8G8R8A8_UNORM;
    colorAttachment.samples = VK_SAMPLE_COUNT_1_BIT;
    colorAttachment.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
    colorAttachment.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    colorAttachment.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    colorAttachment.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    colorAttachment.initialLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    colorAttachment.finalLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;

    VkAttachmentReference colorAttachmentRef{};
    colorAttachmentRef.attachment = 0;
    colorAttachmentRef.layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;

    VkSubpassDescription subpass{};
    subpass.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
    subpass.colorAttachmentCount = 1;
    subpass.pColorAttachments = &colorAttachmentRef;

    VkRenderPassCreateInfo renderPassInfo{};
    renderPassInfo.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO;
    renderPassInfo.attachmentCount = 1;
    renderPassInfo.pAttachments = &colorAttachment;
    renderPassInfo.subpassCount = 1;
    renderPassInfo.pSubpasses = &subpass;

    if (vkCreateRenderPass(m_device, &renderPassInfo, nullptr, &m_renderPass) !=
        VK_SUCCESS) {
      if (errorMessage) {
        *errorMessage = QStringLiteral("Failed to create Vulkan render pass.");
      }
      return false;
    }

    VkImageView attachments[] = {m_colorImageView};
    VkFramebufferCreateInfo framebufferInfo{};
    framebufferInfo.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
    framebufferInfo.renderPass = m_renderPass;
    framebufferInfo.attachmentCount = 1;
    framebufferInfo.pAttachments = attachments;
    framebufferInfo.width = static_cast<uint32_t>(m_outputSize.width());
    framebufferInfo.height = static_cast<uint32_t>(m_outputSize.height());
    framebufferInfo.layers = 1;

    if (vkCreateFramebuffer(m_device, &framebufferInfo, nullptr,
                            &m_framebuffer) != VK_SUCCESS) {
      if (errorMessage) {
        *errorMessage = QStringLiteral("Failed to create Vulkan framebuffer.");
      }
      return false;
    }

    const VkDeviceSize layerStagingSize =
        static_cast<VkDeviceSize>(m_outputSize.width()) *
            static_cast<VkDeviceSize>(m_outputSize.height()) * 4 +
        kCurveLutBytes +
        kCurveLutBytes +
        static_cast<VkDeviceSize>(m_outputSize.width()) *
            static_cast<VkDeviceSize>(m_outputSize.height()) * 4;
    const VkDeviceSize stagingSize = layerStagingSize * kMaxLayerTextures;
    VkBufferCreateInfo stagingBufInfo{};
    stagingBufInfo.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    stagingBufInfo.size = stagingSize;
    stagingBufInfo.usage =
        VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT;
    stagingBufInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    VkFenceCreateInfo fenceInfo{};
    fenceInfo.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
    for (int i = 0; i < m_frameSlots.size(); ++i) {
      FrameSlot &slot = m_frameSlots[i];
      if (vkCreateBuffer(m_device, &stagingBufInfo, nullptr,
                         &slot.stagingBuffer) != VK_SUCCESS) {
        if (errorMessage) {
          *errorMessage =
              QStringLiteral("Failed to create Vulkan staging buffer.");
        }
        return false;
      }
      VkMemoryRequirements stagingReq{};
      vkGetBufferMemoryRequirements(m_device, slot.stagingBuffer, &stagingReq);
      VkMemoryPropertyFlags stagingFlags = 0;
      uint32_t stagingType = findMemoryTypePreferred(
          m_physicalDevice, stagingReq.memoryTypeBits,
          VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT,
          VK_MEMORY_PROPERTY_HOST_CACHED_BIT, &stagingFlags);
      if (stagingType == UINT32_MAX) {
        if (errorMessage) {
          *errorMessage =
              QStringLiteral("No suitable Vulkan staging memory type.");
        }
        return false;
      }
      VkMemoryAllocateInfo stagingAlloc{};
      stagingAlloc.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
      stagingAlloc.allocationSize = stagingReq.size;
      stagingAlloc.memoryTypeIndex = stagingType;
      slot.stagingAllocationSize = stagingReq.size;
      slot.stagingHostCoherent =
          (stagingFlags & VK_MEMORY_PROPERTY_HOST_COHERENT_BIT) != 0;
      if (vkAllocateMemory(m_device, &stagingAlloc, nullptr,
                           &slot.stagingMemory) != VK_SUCCESS ||
          vkBindBufferMemory(m_device, slot.stagingBuffer, slot.stagingMemory,
                             0) != VK_SUCCESS) {
        if (errorMessage) {
          *errorMessage =
              QStringLiteral("Failed to allocate/bind Vulkan staging memory.");
        }
        return false;
      }
      if (vkMapMemory(m_device, slot.stagingMemory, 0, VK_WHOLE_SIZE, 0,
                      &slot.stagingMapped) != VK_SUCCESS) {
        if (errorMessage) {
          *errorMessage = QStringLiteral(
              "Failed to persistently map Vulkan staging memory.");
        }
        return false;
      }
      slot.stagingBufferSize = stagingSize;
      if (m_externalMemoryFdSupported && m_vkGetMemoryFdKHR) {
        const VkDeviceSize yPlaneBytes =
            static_cast<VkDeviceSize>(m_outputSize.width()) *
            static_cast<VkDeviceSize>(m_outputSize.height());
        const VkDeviceSize uvPlaneOffset =
            (yPlaneBytes + 255u) & ~VkDeviceSize(255u);
        const VkDeviceSize uvPlaneBytes =
            static_cast<VkDeviceSize>(qMax(1, m_outputSize.width() / 2)) *
            static_cast<VkDeviceSize>(qMax(1, m_outputSize.height() / 2)) * 2;
        const VkDeviceSize nv12ExportBufferSize = uvPlaneOffset + uvPlaneBytes;
        VkExternalMemoryBufferCreateInfo externalBufferInfo{};
        externalBufferInfo.sType =
            VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_BUFFER_CREATE_INFO;
        externalBufferInfo.handleTypes =
            VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_FD_BIT;
        VkBufferCreateInfo exportBufferInfo = stagingBufInfo;
        exportBufferInfo.pNext = &externalBufferInfo;
        exportBufferInfo.size = nv12ExportBufferSize;
        exportBufferInfo.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT;
        const VkResult createExportBufferResult =
            vkCreateBuffer(m_device, &exportBufferInfo, nullptr,
                           &slot.cudaExportBuffer);
        if (createExportBufferResult == VK_SUCCESS) {
          VkMemoryRequirements exportReq{};
          vkGetBufferMemoryRequirements(m_device, slot.cudaExportBuffer,
                                        &exportReq);
          const uint32_t exportType =
              findMemoryType(m_physicalDevice, exportReq.memoryTypeBits,
                             VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
          VkExportMemoryAllocateInfo exportAllocInfo{};
          exportAllocInfo.sType = VK_STRUCTURE_TYPE_EXPORT_MEMORY_ALLOCATE_INFO;
          exportAllocInfo.handleTypes =
              VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_FD_BIT;
          VkMemoryAllocateInfo exportAlloc{};
          exportAlloc.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
          exportAlloc.pNext = &exportAllocInfo;
          exportAlloc.allocationSize = exportReq.size;
          exportAlloc.memoryTypeIndex = exportType;
          VkResult exportAllocResult = VK_ERROR_FEATURE_NOT_PRESENT;
          VkResult exportBindResult = VK_ERROR_FEATURE_NOT_PRESENT;
          if (exportType != UINT32_MAX) {
            exportAllocResult =
                vkAllocateMemory(m_device, &exportAlloc, nullptr,
                                 &slot.cudaExportMemory);
            if (exportAllocResult == VK_SUCCESS) {
              exportBindResult =
                  vkBindBufferMemory(m_device, slot.cudaExportBuffer,
                                     slot.cudaExportMemory, 0);
            }
          }
          if (exportType != UINT32_MAX &&
              exportAllocResult == VK_SUCCESS &&
              exportBindResult == VK_SUCCESS) {
            slot.cudaExportAllocationSize = exportReq.size;
            m_cudaExportBuffersReady = true;
            m_cudaExternalMemoryStatus =
                QStringLiteral("ready size=%1 memory_type=%2")
                    .arg(static_cast<qulonglong>(exportReq.size))
                    .arg(exportType);
          } else {
            m_cudaExternalMemoryStatus =
                QStringLiteral("export buffer memory failed: type=%1 alloc=%2 bind=%3 size=%4 bits=0x%5")
                    .arg(exportType == UINT32_MAX ? QStringLiteral("none") : QString::number(exportType),
                         QString::number(static_cast<int>(exportAllocResult)),
                         QString::number(static_cast<int>(exportBindResult)))
                    .arg(static_cast<qulonglong>(exportReq.size))
                    .arg(exportReq.memoryTypeBits, 0, 16);
            if (slot.cudaExportMemory != VK_NULL_HANDLE) {
              vkFreeMemory(m_device, slot.cudaExportMemory, nullptr);
              slot.cudaExportMemory = VK_NULL_HANDLE;
            }
            vkDestroyBuffer(m_device, slot.cudaExportBuffer, nullptr);
            slot.cudaExportBuffer = VK_NULL_HANDLE;
          }
        } else {
          m_cudaExternalMemoryStatus =
              QStringLiteral("export buffer create failed: result=%1 size=%2")
                  .arg(static_cast<int>(createExportBufferResult))
                  .arg(static_cast<qulonglong>(nv12ExportBufferSize));
        }
      } else if (!m_externalMemoryFdSupported) {
        m_cudaExternalMemoryStatus = QStringLiteral("VK_KHR_external_memory_fd unsupported");
      } else if (!m_vkGetMemoryFdKHR) {
        m_cudaExternalMemoryStatus = QStringLiteral("vkGetMemoryFdKHR unavailable");
      }
      if (vkCreateFence(m_device, &fenceInfo, nullptr, &slot.fence) !=
          VK_SUCCESS) {
        if (errorMessage) {
          *errorMessage =
              QStringLiteral("Failed to create Vulkan submit fence.");
        }
        return false;
      }
    }
    if (m_externalMemoryFdSupported && m_vkGetMemoryFdKHR && !m_cudaExportBuffersReady) {
      qWarning().noquote() << QStringLiteral(
          "Vulkan/CUDA interop capability present but exportable Vulkan "
          "buffers could not be allocated: %1")
          .arg(m_cudaExternalMemoryStatus);
    }
    if (m_cudaExportBuffersReady) {
      qInfo().noquote() << QStringLiteral("Vulkan/CUDA export buffer ready: %1")
                               .arg(m_cudaExternalMemoryStatus);
    }
    useSlot(0);

    auto createAttachment =
        [&](VkFormat format, uint32_t width, uint32_t height, VkImage *image,
            VkDeviceMemory *memory, VkImageView *view, QString *err) -> bool {
      VkImageCreateInfo ci{};
      ci.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
      ci.imageType = VK_IMAGE_TYPE_2D;
      ci.extent = {width, height, 1};
      ci.mipLevels = 1;
      ci.arrayLayers = 1;
      ci.format = format;
      ci.tiling = VK_IMAGE_TILING_OPTIMAL;
      ci.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
      ci.usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT |
                 VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_SAMPLED_BIT |
                 VK_IMAGE_USAGE_STORAGE_BIT;
      ci.samples = VK_SAMPLE_COUNT_1_BIT;
      ci.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
      if (vkCreateImage(m_device, &ci, nullptr, image) != VK_SUCCESS) {
        if (err)
          *err =
              QStringLiteral("Failed to create Vulkan NV12 attachment image.");
        return false;
      }
      VkMemoryRequirements req{};
      vkGetImageMemoryRequirements(m_device, *image, &req);
      const uint32_t memType =
          findMemoryType(m_physicalDevice, req.memoryTypeBits,
                         VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
      if (memType == UINT32_MAX) {
        if (err)
          *err = QStringLiteral("No Vulkan memory type for NV12 attachment.");
        return false;
      }
      VkMemoryAllocateInfo ai{};
      ai.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
      ai.allocationSize = req.size;
      ai.memoryTypeIndex = memType;
      if (vkAllocateMemory(m_device, &ai, nullptr, memory) != VK_SUCCESS ||
          vkBindImageMemory(m_device, *image, *memory, 0) != VK_SUCCESS) {
        if (err)
          *err = QStringLiteral(
              "Failed to allocate/bind Vulkan NV12 attachment memory.");
        return false;
      }
      VkImageViewCreateInfo vi{};
      vi.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
      vi.image = *image;
      vi.viewType = VK_IMAGE_VIEW_TYPE_2D;
      vi.format = format;
      vi.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
      vi.subresourceRange.levelCount = 1;
      vi.subresourceRange.layerCount = 1;
      if (vkCreateImageView(m_device, &vi, nullptr, view) != VK_SUCCESS) {
        if (err)
          *err =
              QStringLiteral("Failed to create Vulkan NV12 attachment view.");
        return false;
      }
      return true;
    };
    if (!createAttachment(
            VK_FORMAT_R8_UNORM, static_cast<uint32_t>(m_outputSize.width()),
            static_cast<uint32_t>(m_outputSize.height()), &m_nv12YImage,
            &m_nv12YImageMemory, &m_nv12YImageView, errorMessage)) {
      return false;
    }
    if (!createAttachment(
            VK_FORMAT_R8G8_UNORM,
            static_cast<uint32_t>(qMax(1, m_outputSize.width() / 2)),
            static_cast<uint32_t>(qMax(1, m_outputSize.height() / 2)),
            &m_nv12UvImage, &m_nv12UvImageMemory, &m_nv12UvImageView,
            errorMessage)) {
      return false;
    }
    if (!createAttachment(
            VK_FORMAT_R8_UNORM,
            static_cast<uint32_t>(qMax(1, m_outputSize.width() / 2)),
            static_cast<uint32_t>(qMax(1, m_outputSize.height() / 2)),
            &m_yuv420pUImage, &m_yuv420pUImageMemory, &m_yuv420pUImageView,
            errorMessage)) {
      return false;
    }
    if (!createAttachment(
            VK_FORMAT_R8_UNORM,
            static_cast<uint32_t>(qMax(1, m_outputSize.width() / 2)),
            static_cast<uint32_t>(qMax(1, m_outputSize.height() / 2)),
            &m_yuv420pVImage, &m_yuv420pVImageMemory, &m_yuv420pVImageView,
            errorMessage)) {
      return false;
    }

    auto createColorRenderPass = [&](VkFormat format, VkRenderPass *pass,
                                     QString *err) -> bool {
      VkAttachmentDescription a{};
      a.format = format;
      a.samples = VK_SAMPLE_COUNT_1_BIT;
      a.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
      a.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
      a.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
      a.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
      a.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
      a.finalLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
      VkAttachmentReference ref{};
      ref.attachment = 0;
      ref.layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
      VkSubpassDescription sub{};
      sub.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
      sub.colorAttachmentCount = 1;
      sub.pColorAttachments = &ref;
      VkRenderPassCreateInfo rp{};
      rp.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO;
      rp.attachmentCount = 1;
      rp.pAttachments = &a;
      rp.subpassCount = 1;
      rp.pSubpasses = &sub;
      if (vkCreateRenderPass(m_device, &rp, nullptr, pass) != VK_SUCCESS) {
        if (err)
          *err = QStringLiteral("Failed to create Vulkan NV12 render pass.");
        return false;
      }
      return true;
    };
    if (!createColorRenderPass(VK_FORMAT_R8_UNORM, &m_nv12YRenderPass,
                               errorMessage) ||
        !createColorRenderPass(VK_FORMAT_R8G8_UNORM, &m_nv12UvRenderPass,
                               errorMessage)) {
      return false;
    }
    VkImageView yAtt[] = {m_nv12YImageView};
    VkFramebufferCreateInfo yFb{};
    yFb.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
    yFb.renderPass = m_nv12YRenderPass;
    yFb.attachmentCount = 1;
    yFb.pAttachments = yAtt;
    yFb.width = static_cast<uint32_t>(m_outputSize.width());
    yFb.height = static_cast<uint32_t>(m_outputSize.height());
    yFb.layers = 1;
    if (vkCreateFramebuffer(m_device, &yFb, nullptr, &m_nv12YFramebuffer) !=
        VK_SUCCESS) {
      if (errorMessage)
        *errorMessage =
            QStringLiteral("Failed to create Vulkan NV12 Y framebuffer.");
      return false;
    }
    VkImageView uvAtt[] = {m_nv12UvImageView};
    VkFramebufferCreateInfo uvFb{};
    uvFb.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
    uvFb.renderPass = m_nv12UvRenderPass;
    uvFb.attachmentCount = 1;
    uvFb.pAttachments = uvAtt;
    uvFb.width = static_cast<uint32_t>(qMax(1, m_outputSize.width() / 2));
    uvFb.height = static_cast<uint32_t>(qMax(1, m_outputSize.height() / 2));
    uvFb.layers = 1;
    if (vkCreateFramebuffer(m_device, &uvFb, nullptr, &m_nv12UvFramebuffer) !=
        VK_SUCCESS) {
      if (errorMessage)
        *errorMessage =
            QStringLiteral("Failed to create Vulkan NV12 UV framebuffer.");
      return false;
    }
    VkImageView uAtt[] = {m_yuv420pUImageView};
    VkFramebufferCreateInfo uFb = uvFb;
    uFb.renderPass = m_nv12YRenderPass;
    uFb.pAttachments = uAtt;
    if (vkCreateFramebuffer(m_device, &uFb, nullptr, &m_yuv420pUFramebuffer) !=
        VK_SUCCESS) {
      if (errorMessage)
        *errorMessage =
            QStringLiteral("Failed to create Vulkan YUV420P U framebuffer.");
      return false;
    }
    VkImageView vAtt[] = {m_yuv420pVImageView};
    VkFramebufferCreateInfo vFb = uvFb;
    vFb.renderPass = m_nv12YRenderPass;
    vFb.pAttachments = vAtt;
    if (vkCreateFramebuffer(m_device, &vFb, nullptr, &m_yuv420pVFramebuffer) !=
        VK_SUCCESS) {
      if (errorMessage)
        *errorMessage =
            QStringLiteral("Failed to create Vulkan YUV420P V framebuffer.");
      return false;
    }

    VkDescriptorSetLayoutBinding yuvBindings[4]{};
    yuvBindings[0].binding = 0;
    yuvBindings[0].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    yuvBindings[0].descriptorCount = 1;
    yuvBindings[0].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
    for (uint32_t i = 1; i < 4; ++i) {
      yuvBindings[i].binding = i;
      yuvBindings[i].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
      yuvBindings[i].descriptorCount = 1;
      yuvBindings[i].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
    }
    VkDescriptorSetLayoutCreateInfo yuvLayoutInfo{};
    yuvLayoutInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    yuvLayoutInfo.bindingCount = 4;
    yuvLayoutInfo.pBindings = yuvBindings;
    if (vkCreateDescriptorSetLayout(m_device, &yuvLayoutInfo, nullptr,
                                    &m_yuvComputeDescriptorSetLayout) !=
        VK_SUCCESS) {
      if (errorMessage)
        *errorMessage = QStringLiteral(
            "Failed to create Vulkan YUV compute descriptor layout.");
      return false;
    }
    VkDescriptorPoolSize yuvPoolSizes[2]{};
    yuvPoolSizes[0].type = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    yuvPoolSizes[0].descriptorCount = 1;
    yuvPoolSizes[1].type = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
    yuvPoolSizes[1].descriptorCount = 3;
    VkDescriptorPoolCreateInfo yuvPoolInfo{};
    yuvPoolInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    yuvPoolInfo.poolSizeCount = 2;
    yuvPoolInfo.pPoolSizes = yuvPoolSizes;
    yuvPoolInfo.maxSets = 1;
    if (vkCreateDescriptorPool(m_device, &yuvPoolInfo, nullptr,
                               &m_yuvComputeDescriptorPool) != VK_SUCCESS) {
      if (errorMessage)
        *errorMessage = QStringLiteral(
            "Failed to create Vulkan YUV compute descriptor pool.");
      return false;
    }
    VkDescriptorSetAllocateInfo yuvSetInfo{};
    yuvSetInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
    yuvSetInfo.descriptorPool = m_yuvComputeDescriptorPool;
    yuvSetInfo.descriptorSetCount = 1;
    yuvSetInfo.pSetLayouts = &m_yuvComputeDescriptorSetLayout;
    if (vkAllocateDescriptorSets(m_device, &yuvSetInfo,
                                 &m_yuvComputeDescriptorSet) != VK_SUCCESS) {
      if (errorMessage)
        *errorMessage = QStringLiteral(
            "Failed to allocate Vulkan YUV compute descriptor set.");
      return false;
    }
    VkDescriptorImageInfo yuvSrc{};
    yuvSrc.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    yuvSrc.imageView = m_colorImageView;
    yuvSrc.sampler = m_sampler;
    VkDescriptorImageInfo yuvImages[3]{};
    yuvImages[0].imageLayout = VK_IMAGE_LAYOUT_GENERAL;
    yuvImages[0].imageView = m_nv12YImageView;
    yuvImages[1].imageLayout = VK_IMAGE_LAYOUT_GENERAL;
    yuvImages[1].imageView = m_yuv420pUImageView;
    yuvImages[2].imageLayout = VK_IMAGE_LAYOUT_GENERAL;
    yuvImages[2].imageView = m_yuv420pVImageView;
    VkWriteDescriptorSet yuvWrites[4]{};
    yuvWrites[0].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    yuvWrites[0].dstSet = m_yuvComputeDescriptorSet;
    yuvWrites[0].dstBinding = 0;
    yuvWrites[0].descriptorCount = 1;
    yuvWrites[0].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    yuvWrites[0].pImageInfo = &yuvSrc;
    for (uint32_t i = 1; i < 4; ++i) {
      yuvWrites[i].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
      yuvWrites[i].dstSet = m_yuvComputeDescriptorSet;
      yuvWrites[i].dstBinding = i;
      yuvWrites[i].descriptorCount = 1;
      yuvWrites[i].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
      yuvWrites[i].pImageInfo = &yuvImages[i - 1];
    }
    vkUpdateDescriptorSets(m_device, 4, yuvWrites, 0, nullptr);

     VkPushConstantRange effectsPushConstantRange{};
    effectsPushConstantRange.stageFlags =
        VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;
    effectsPushConstantRange.offset = 0;
    effectsPushConstantRange.size = 128;

    VkPipelineLayoutCreateInfo effectsLayoutInfo{};
    effectsLayoutInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    effectsLayoutInfo.setLayoutCount = 1;
    effectsLayoutInfo.pSetLayouts = &m_descriptorSetLayout;
    effectsLayoutInfo.pushConstantRangeCount = 1;
    effectsLayoutInfo.pPushConstantRanges = &effectsPushConstantRange;
    if (vkCreatePipelineLayout(m_device, &effectsLayoutInfo, nullptr,
                               &m_effectsPipelineLayout) != VK_SUCCESS) {
      if (errorMessage) {
        *errorMessage =
            QStringLiteral("Failed to create Vulkan effects pipeline layout.");
      }
      return false;
    }

    VkPushConstantRange maskPushConstantRange{};
    maskPushConstantRange.stageFlags = VK_SHADER_STAGE_VERTEX_BIT;
    maskPushConstantRange.offset = 0;
    maskPushConstantRange.size = 64;
    VkPipelineLayoutCreateInfo maskLayoutInfo{};
    maskLayoutInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    maskLayoutInfo.pushConstantRangeCount = 1;
    maskLayoutInfo.pPushConstantRanges = &maskPushConstantRange;
    if (vkCreatePipelineLayout(m_device, &maskLayoutInfo, nullptr,
                               &m_maskPipelineLayout) != VK_SUCCESS) {
      if (errorMessage) {
        *errorMessage =
            QStringLiteral("Failed to create Vulkan mask pipeline layout.");
      }
      return false;
    }

    VkPipelineLayoutCreateInfo nv12LayoutInfo{};
    nv12LayoutInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    nv12LayoutInfo.setLayoutCount = 1;
    nv12LayoutInfo.pSetLayouts = &m_descriptorSetLayout;
    if (vkCreatePipelineLayout(m_device, &nv12LayoutInfo, nullptr,
                               &m_nv12PipelineLayout) != VK_SUCCESS) {
      if (errorMessage) {
        *errorMessage =
            QStringLiteral("Failed to create Vulkan NV12 pipeline layout.");
      }
      return false;
    }
    VkPipelineLayoutCreateInfo yuvComputeLayoutInfo{};
    yuvComputeLayoutInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    yuvComputeLayoutInfo.setLayoutCount = 1;
    yuvComputeLayoutInfo.pSetLayouts = &m_yuvComputeDescriptorSetLayout;
    if (vkCreatePipelineLayout(m_device, &yuvComputeLayoutInfo, nullptr,
                               &m_yuvComputePipelineLayout) != VK_SUCCESS) {
      if (errorMessage) {
        *errorMessage = QStringLiteral(
            "Failed to create Vulkan YUV compute pipeline layout.");
      }
      return false;
    }
     const QString shaderDir = jcutVulkanShaderDirectory();
    m_effectsVertModule = createShaderModule(
        m_device,
        readBinaryFile(shaderDir + QStringLiteral("/effects.vert.spv")));
    m_effectsFragModule = createShaderModule(
        m_device,
        readBinaryFile(shaderDir + QStringLiteral("/effects.frag.spv")));
    m_maskVertModule = createShaderModule(
        m_device, readBinaryFile(shaderDir + QStringLiteral("/mask.vert.spv")));
    m_maskFragModule = createShaderModule(
        m_device, readBinaryFile(shaderDir + QStringLiteral("/mask.frag.spv")));
    m_nv12VertModule = createShaderModule(
        m_device, readBinaryFile(shaderDir + QStringLiteral("/nv12.vert.spv")));
    m_nv12YFragModule = createShaderModule(
        m_device,
        readBinaryFile(shaderDir + QStringLiteral("/nv12_y.frag.spv")));
    m_nv12UvFragModule = createShaderModule(
        m_device,
        readBinaryFile(shaderDir + QStringLiteral("/nv12_uv.frag.spv")));
    m_yuv420pUFragModule = createShaderModule(
        m_device,
        readBinaryFile(shaderDir + QStringLiteral("/yuv420p_u.frag.spv")));
    m_yuv420pVFragModule = createShaderModule(
        m_device,
        readBinaryFile(shaderDir + QStringLiteral("/yuv420p_v.frag.spv")));
    m_yuv420pComputeModule = createShaderModule(
        m_device,
        readBinaryFile(shaderDir + QStringLiteral("/yuv420p.comp.spv")));
    if (m_effectsVertModule == VK_NULL_HANDLE ||
        m_effectsFragModule == VK_NULL_HANDLE ||
        m_maskVertModule == VK_NULL_HANDLE ||
        m_maskFragModule == VK_NULL_HANDLE ||
        m_nv12VertModule == VK_NULL_HANDLE ||
        m_nv12YFragModule == VK_NULL_HANDLE ||
        m_nv12UvFragModule == VK_NULL_HANDLE ||
        m_yuv420pUFragModule == VK_NULL_HANDLE ||
        m_yuv420pVFragModule == VK_NULL_HANDLE ||
        m_yuv420pComputeModule == VK_NULL_HANDLE) {
      if (errorMessage) {
        *errorMessage = QStringLiteral(
            "Failed to load Vulkan SPIR-V shader modules. Ensure "
            "shader build step succeeded.");
      }
      return false;
    }

    VkDescriptorImageInfo imageInfoDesc[4]{};
    imageInfoDesc[0].imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    imageInfoDesc[0].imageView = m_colorImageView;
    imageInfoDesc[0].sampler = m_sampler;
    imageInfoDesc[1].imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    imageInfoDesc[1].imageView = !m_layerSlots.isEmpty()
                                     ? m_layerSlots.first().curveLutView
                                     : VK_NULL_HANDLE;
    imageInfoDesc[1].sampler = m_sampler;
    imageInfoDesc[2].imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    imageInfoDesc[2].imageView = !m_layerSlots.isEmpty()
                                     ? m_layerSlots.first().maskView
                                     : VK_NULL_HANDLE;
    imageInfoDesc[2].sampler = m_sampler;
    imageInfoDesc[3].imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    imageInfoDesc[3].imageView = !m_layerSlots.isEmpty()
                                     ? m_layerSlots.first().maskCurveLutView
                                     : VK_NULL_HANDLE;
    imageInfoDesc[3].sampler = m_sampler;
    VkDescriptorBufferInfo frameUniformInfo{};
    frameUniformInfo.buffer = m_frameUniformBuffer;
    frameUniformInfo.range = sizeof(FrameUniformData);
    VkWriteDescriptorSet writes[5]{};
    for (uint32_t binding = 0; binding < 4; ++binding) {
      writes[binding].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
      writes[binding].dstSet = m_descriptorSet;
      writes[binding].dstBinding = binding;
      writes[binding].descriptorCount = 1;
      writes[binding].descriptorType =
          VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
      writes[binding].pImageInfo = &imageInfoDesc[binding];
    }
    writes[4].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    writes[4].dstSet = m_descriptorSet;
    writes[4].dstBinding = 4;
    writes[4].descriptorCount = 1;
    writes[4].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC;
    writes[4].pBufferInfo = &frameUniformInfo;
    vkUpdateDescriptorSets(m_device, 5, writes, 0, nullptr);

    auto createPipeline = [&](VkShaderModule vert, VkShaderModule frag,
                              VkPipelineLayout layout, VkRenderPass renderPass,
                              VkPipeline *outPipeline) -> bool {
      VkPipelineShaderStageCreateInfo shaderStages[2]{};
      shaderStages[0].sType =
          VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
      shaderStages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
      shaderStages[0].module = vert;
      shaderStages[0].pName = "main";
      shaderStages[1].sType =
          VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
      shaderStages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
      shaderStages[1].module = frag;
      shaderStages[1].pName = "main";

      VkPipelineVertexInputStateCreateInfo vertexInput{};
      vertexInput.sType =
          VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;
      vertexInput.vertexBindingDescriptionCount = 0;
      vertexInput.vertexAttributeDescriptionCount = 0;

      VkPipelineInputAssemblyStateCreateInfo inputAssembly{};
      inputAssembly.sType =
          VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
      inputAssembly.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP;

      VkPipelineViewportStateCreateInfo viewportState{};
      viewportState.sType =
          VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
      viewportState.viewportCount = 1;
      viewportState.scissorCount = 1;

      VkPipelineRasterizationStateCreateInfo rasterizer{};
      rasterizer.sType =
          VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
      rasterizer.polygonMode = VK_POLYGON_MODE_FILL;
      rasterizer.lineWidth = 1.0f;
      rasterizer.cullMode = VK_CULL_MODE_NONE;
      rasterizer.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;

      VkPipelineMultisampleStateCreateInfo multisampling{};
      multisampling.sType =
          VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
      multisampling.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
      VkPipelineDepthStencilStateCreateInfo depthStencil{};
      depthStencil.sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO;
      depthStencil.depthTestEnable = VK_FALSE;
      depthStencil.depthWriteEnable = VK_FALSE;
      depthStencil.depthCompareOp = VK_COMPARE_OP_ALWAYS;
      depthStencil.depthBoundsTestEnable = VK_FALSE;
      depthStencil.stencilTestEnable = VK_FALSE;

      VkPipelineColorBlendAttachmentState colorBlendAttachment{};
      colorBlendAttachment.colorWriteMask =
          VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
          VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
      colorBlendAttachment.blendEnable = VK_TRUE;
      colorBlendAttachment.srcColorBlendFactor = VK_BLEND_FACTOR_ONE;
      colorBlendAttachment.dstColorBlendFactor =
          VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
      colorBlendAttachment.colorBlendOp = VK_BLEND_OP_ADD;
      colorBlendAttachment.srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
      colorBlendAttachment.dstAlphaBlendFactor =
          VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
      colorBlendAttachment.alphaBlendOp = VK_BLEND_OP_ADD;
      VkPipelineColorBlendStateCreateInfo colorBlending{};
      colorBlending.sType =
          VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
      colorBlending.attachmentCount = 1;
      colorBlending.pAttachments = &colorBlendAttachment;
      VkDynamicState dynamicStates[] = {VK_DYNAMIC_STATE_VIEWPORT,
                                        VK_DYNAMIC_STATE_SCISSOR};
      VkPipelineDynamicStateCreateInfo dynamicState{};
      dynamicState.sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO;
      dynamicState.dynamicStateCount = 2;
      dynamicState.pDynamicStates = dynamicStates;

      VkGraphicsPipelineCreateInfo pipelineInfo{};
      pipelineInfo.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
      pipelineInfo.stageCount = 2;
      pipelineInfo.pStages = shaderStages;
      pipelineInfo.pVertexInputState = &vertexInput;
      pipelineInfo.pInputAssemblyState = &inputAssembly;
      pipelineInfo.pViewportState = &viewportState;
      pipelineInfo.pRasterizationState = &rasterizer;
      pipelineInfo.pMultisampleState = &multisampling;
      pipelineInfo.pDepthStencilState = &depthStencil;
      pipelineInfo.pColorBlendState = &colorBlending;
      pipelineInfo.pDynamicState = &dynamicState;
      pipelineInfo.layout = layout;
      pipelineInfo.renderPass = renderPass;
      pipelineInfo.subpass = 0;

      return vkCreateGraphicsPipelines(m_device, VK_NULL_HANDLE, 1,
                                       &pipelineInfo, nullptr,
                                       outPipeline) == VK_SUCCESS;
    };

    VkComputePipelineCreateInfo computeInfo{};
    computeInfo.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO;
    computeInfo.stage.sType =
        VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    computeInfo.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
    computeInfo.stage.module = m_yuv420pComputeModule;
    computeInfo.stage.pName = "main";
    computeInfo.layout = m_yuvComputePipelineLayout;
    if (vkCreateComputePipelines(m_device, VK_NULL_HANDLE, 1, &computeInfo,
                                 nullptr,
                                 &m_yuv420pComputePipeline) != VK_SUCCESS) {
      if (errorMessage) {
        *errorMessage =
            QStringLiteral("Failed to create Vulkan YUV420P compute pipeline.");
      }
      return false;
    }

    if (!m_maskPreprocessor.initialize(
            m_physicalDevice, m_device, errorMessage)) {
      return false;
    }

	    if (!createPipeline(m_effectsVertModule, m_effectsFragModule,
	                        m_effectsPipelineLayout, m_renderPass,
	                        &m_effectsPipeline) ||
        !createPipeline(m_nv12VertModule, m_nv12YFragModule,
                        m_nv12PipelineLayout, m_nv12YRenderPass,
                        &m_nv12YPipeline) ||
        !createPipeline(m_nv12VertModule, m_nv12UvFragModule,
                        m_nv12PipelineLayout, m_nv12UvRenderPass,
                        &m_nv12UvPipeline) ||
        !createPipeline(m_nv12VertModule, m_yuv420pUFragModule,
                        m_nv12PipelineLayout, m_nv12YRenderPass,
                        &m_yuv420pUPipeline) ||
        !createPipeline(m_nv12VertModule, m_yuv420pVFragModule,
                        m_nv12PipelineLayout, m_nv12YRenderPass,
                        &m_yuv420pVPipeline)) {
      if (errorMessage) {
        *errorMessage =
            QStringLiteral("Failed to create Vulkan graphics pipelines.");
	      }
	      return false;
	    }

    m_transcriptTextRenderer = std::make_unique<VulkanTextRenderer>();
    if (!m_transcriptTextRenderer->initialize(m_physicalDevice, m_device, nullptr, m_renderPass, errorMessage)) {
      return false;
    }
    m_speakerTextRenderer = std::make_unique<VulkanTextRenderer>();
    if (!m_speakerTextRenderer->initialize(m_physicalDevice, m_device, nullptr, m_renderPass, errorMessage)) {
      return false;
    }

	    m_initialized = true;
	    return true;
	  }

	  void release() {
	    if (m_nv12ScratchFrame) {
	      av_frame_free(&m_nv12ScratchFrame);
	    }
    if (m_device != VK_NULL_HANDLE) {
      vkDeviceWaitIdle(m_device);
    }
    m_maskPreprocessor.destroy();
    destroyPreviewSlots();
    m_speakerTextRenderer.reset();
    m_transcriptTextRenderer.reset();
    for (FrameSlot &slot : m_frameSlots) {
#if JCUT_HAS_CUDA_DRIVER
      if (slot.cudaExternalMemory) {
        // Do not explicitly destroy the imported opaque-FD allocation from the
        // render worker. The import is tied to the CUDA device/context retained
        // in cudaImportDeviceRef; forcing the driver teardown entry point here
        // has proven unsafe across incremental export chunk boundaries.
        // Retiring the retained device reference lets the CUDA context own the
        // import lifetime without putting driver teardown on the hot path.
        slot.cudaExternalMemory = nullptr;
        slot.cudaExternalDevicePtr = 0;
        slot.cudaImportContext = nullptr;
      }
      if (slot.cudaImportDeviceRef) {
        av_buffer_unref(&slot.cudaImportDeviceRef);
      }
#endif
      if (slot.stagingMapped && slot.stagingMemory != VK_NULL_HANDLE) {
        vkUnmapMemory(m_device, slot.stagingMemory);
        slot.stagingMapped = nullptr;
      }
      if (slot.fence != VK_NULL_HANDLE) {
        vkDestroyFence(m_device, slot.fence, nullptr);
        slot.fence = VK_NULL_HANDLE;
      }
      if (slot.stagingBuffer != VK_NULL_HANDLE) {
        vkDestroyBuffer(m_device, slot.stagingBuffer, nullptr);
        slot.stagingBuffer = VK_NULL_HANDLE;
      }
      if (slot.stagingMemory != VK_NULL_HANDLE) {
        vkFreeMemory(m_device, slot.stagingMemory, nullptr);
        slot.stagingMemory = VK_NULL_HANDLE;
      }
      slot.stagingBufferSize = 0;
      slot.stagingAllocationSize = 0;
      if (slot.cudaExportBuffer != VK_NULL_HANDLE) {
        vkDestroyBuffer(m_device, slot.cudaExportBuffer, nullptr);
        slot.cudaExportBuffer = VK_NULL_HANDLE;
      }
      if (slot.cudaExportMemory != VK_NULL_HANDLE) {
        vkFreeMemory(m_device, slot.cudaExportMemory, nullptr);
        slot.cudaExportMemory = VK_NULL_HANDLE;
      }
      slot.commandBuffer = VK_NULL_HANDLE;
      slot.inFlight = false;
    }
    m_frameSlots.clear();
    m_activeSlotIndex = -1;
    m_pendingNv12SlotIndices.clear();
    m_pendingNv12CudaSlotIndices.clear();
    m_pendingYuvSlotIndices.clear();
    m_commandBuffer = VK_NULL_HANDLE;
    m_stagingBuffer = VK_NULL_HANDLE;
    m_stagingMemory = VK_NULL_HANDLE;
    m_stagingMapped = nullptr;
    m_submitFence = VK_NULL_HANDLE;
    m_cudaExportBuffersReady = false;
    for (LayerTextureSlot &slot : m_layerSlots) {
      if (slot.maskView != VK_NULL_HANDLE) {
        vkDestroyImageView(m_device, slot.maskView, nullptr);
        slot.maskView = VK_NULL_HANDLE;
      }
      if (slot.maskImage != VK_NULL_HANDLE) {
        vkDestroyImage(m_device, slot.maskImage, nullptr);
        slot.maskImage = VK_NULL_HANDLE;
      }
      if (slot.maskMemory != VK_NULL_HANDLE) {
        vkFreeMemory(m_device, slot.maskMemory, nullptr);
        slot.maskMemory = VK_NULL_HANDLE;
      }
      if (slot.maskRawView != VK_NULL_HANDLE) {
        vkDestroyImageView(m_device, slot.maskRawView, nullptr);
        slot.maskRawView = VK_NULL_HANDLE;
      }
      if (slot.maskRawImage != VK_NULL_HANDLE) {
        vkDestroyImage(m_device, slot.maskRawImage, nullptr);
        slot.maskRawImage = VK_NULL_HANDLE;
      }
      if (slot.maskRawMemory != VK_NULL_HANDLE) {
        vkFreeMemory(m_device, slot.maskRawMemory, nullptr);
        slot.maskRawMemory = VK_NULL_HANDLE;
      }
      if (slot.maskWorkView != VK_NULL_HANDLE) {
        vkDestroyImageView(m_device, slot.maskWorkView, nullptr);
        slot.maskWorkView = VK_NULL_HANDLE;
      }
      if (slot.maskWorkImage != VK_NULL_HANDLE) {
        vkDestroyImage(m_device, slot.maskWorkImage, nullptr);
        slot.maskWorkImage = VK_NULL_HANDLE;
      }
      if (slot.maskWorkMemory != VK_NULL_HANDLE) {
        vkFreeMemory(m_device, slot.maskWorkMemory, nullptr);
        slot.maskWorkMemory = VK_NULL_HANDLE;
      }
      if (slot.curveLutView != VK_NULL_HANDLE) {
        vkDestroyImageView(m_device, slot.curveLutView, nullptr);
        slot.curveLutView = VK_NULL_HANDLE;
      }
      if (slot.curveLutImage != VK_NULL_HANDLE) {
        vkDestroyImage(m_device, slot.curveLutImage, nullptr);
        slot.curveLutImage = VK_NULL_HANDLE;
      }
      if (slot.curveLutMemory != VK_NULL_HANDLE) {
        vkFreeMemory(m_device, slot.curveLutMemory, nullptr);
        slot.curveLutMemory = VK_NULL_HANDLE;
      }
      if (slot.maskCurveLutView != VK_NULL_HANDLE) {
        vkDestroyImageView(m_device, slot.maskCurveLutView, nullptr);
        slot.maskCurveLutView = VK_NULL_HANDLE;
      }
      if (slot.maskCurveLutImage != VK_NULL_HANDLE) {
        vkDestroyImage(m_device, slot.maskCurveLutImage, nullptr);
        slot.maskCurveLutImage = VK_NULL_HANDLE;
      }
      if (slot.maskCurveLutMemory != VK_NULL_HANDLE) {
        vkFreeMemory(m_device, slot.maskCurveLutMemory, nullptr);
        slot.maskCurveLutMemory = VK_NULL_HANDLE;
      }
      if (slot.view != VK_NULL_HANDLE) {
        vkDestroyImageView(m_device, slot.view, nullptr);
        slot.view = VK_NULL_HANDLE;
      }
      if (slot.image != VK_NULL_HANDLE) {
        vkDestroyImage(m_device, slot.image, nullptr);
        slot.image = VK_NULL_HANDLE;
      }
      if (slot.memory != VK_NULL_HANDLE) {
        vkFreeMemory(m_device, slot.memory, nullptr);
        slot.memory = VK_NULL_HANDLE;
      }
    }
    m_layerSlots.clear();
    if (m_effectsPipeline != VK_NULL_HANDLE) {
      vkDestroyPipeline(m_device, m_effectsPipeline, nullptr);
      m_effectsPipeline = VK_NULL_HANDLE;
    }
    if (m_maskPipeline != VK_NULL_HANDLE) {
      vkDestroyPipeline(m_device, m_maskPipeline, nullptr);
      m_maskPipeline = VK_NULL_HANDLE;
    }
    if (m_nv12YPipeline != VK_NULL_HANDLE) {
      vkDestroyPipeline(m_device, m_nv12YPipeline, nullptr);
      m_nv12YPipeline = VK_NULL_HANDLE;
    }
    if (m_nv12UvPipeline != VK_NULL_HANDLE) {
      vkDestroyPipeline(m_device, m_nv12UvPipeline, nullptr);
      m_nv12UvPipeline = VK_NULL_HANDLE;
    }
    if (m_yuv420pUPipeline != VK_NULL_HANDLE) {
      vkDestroyPipeline(m_device, m_yuv420pUPipeline, nullptr);
      m_yuv420pUPipeline = VK_NULL_HANDLE;
    }
    if (m_yuv420pVPipeline != VK_NULL_HANDLE) {
      vkDestroyPipeline(m_device, m_yuv420pVPipeline, nullptr);
      m_yuv420pVPipeline = VK_NULL_HANDLE;
    }
    if (m_yuv420pComputePipeline != VK_NULL_HANDLE) {
      vkDestroyPipeline(m_device, m_yuv420pComputePipeline, nullptr);
      m_yuv420pComputePipeline = VK_NULL_HANDLE;
    }
    if (m_nv12YFramebuffer != VK_NULL_HANDLE) {
      vkDestroyFramebuffer(m_device, m_nv12YFramebuffer, nullptr);
      m_nv12YFramebuffer = VK_NULL_HANDLE;
    }
    if (m_nv12UvFramebuffer != VK_NULL_HANDLE) {
      vkDestroyFramebuffer(m_device, m_nv12UvFramebuffer, nullptr);
      m_nv12UvFramebuffer = VK_NULL_HANDLE;
    }
    if (m_yuv420pUFramebuffer != VK_NULL_HANDLE) {
      vkDestroyFramebuffer(m_device, m_yuv420pUFramebuffer, nullptr);
      m_yuv420pUFramebuffer = VK_NULL_HANDLE;
    }
    if (m_yuv420pVFramebuffer != VK_NULL_HANDLE) {
      vkDestroyFramebuffer(m_device, m_yuv420pVFramebuffer, nullptr);
      m_yuv420pVFramebuffer = VK_NULL_HANDLE;
    }
    if (m_nv12YRenderPass != VK_NULL_HANDLE) {
      vkDestroyRenderPass(m_device, m_nv12YRenderPass, nullptr);
      m_nv12YRenderPass = VK_NULL_HANDLE;
    }
    if (m_nv12UvRenderPass != VK_NULL_HANDLE) {
      vkDestroyRenderPass(m_device, m_nv12UvRenderPass, nullptr);
      m_nv12UvRenderPass = VK_NULL_HANDLE;
    }
    if (m_nv12YImageView != VK_NULL_HANDLE) {
      vkDestroyImageView(m_device, m_nv12YImageView, nullptr);
      m_nv12YImageView = VK_NULL_HANDLE;
    }
    if (m_nv12UvImageView != VK_NULL_HANDLE) {
      vkDestroyImageView(m_device, m_nv12UvImageView, nullptr);
      m_nv12UvImageView = VK_NULL_HANDLE;
    }
    if (m_yuv420pUImageView != VK_NULL_HANDLE) {
      vkDestroyImageView(m_device, m_yuv420pUImageView, nullptr);
      m_yuv420pUImageView = VK_NULL_HANDLE;
    }
    if (m_yuv420pVImageView != VK_NULL_HANDLE) {
      vkDestroyImageView(m_device, m_yuv420pVImageView, nullptr);
      m_yuv420pVImageView = VK_NULL_HANDLE;
    }
    if (m_nv12YImage != VK_NULL_HANDLE) {
      vkDestroyImage(m_device, m_nv12YImage, nullptr);
      m_nv12YImage = VK_NULL_HANDLE;
    }
    if (m_nv12UvImage != VK_NULL_HANDLE) {
      vkDestroyImage(m_device, m_nv12UvImage, nullptr);
      m_nv12UvImage = VK_NULL_HANDLE;
    }
    if (m_yuv420pUImage != VK_NULL_HANDLE) {
      vkDestroyImage(m_device, m_yuv420pUImage, nullptr);
      m_yuv420pUImage = VK_NULL_HANDLE;
    }
    if (m_yuv420pVImage != VK_NULL_HANDLE) {
      vkDestroyImage(m_device, m_yuv420pVImage, nullptr);
      m_yuv420pVImage = VK_NULL_HANDLE;
    }
    if (m_nv12YImageMemory != VK_NULL_HANDLE) {
      vkFreeMemory(m_device, m_nv12YImageMemory, nullptr);
      m_nv12YImageMemory = VK_NULL_HANDLE;
    }
    if (m_nv12UvImageMemory != VK_NULL_HANDLE) {
      vkFreeMemory(m_device, m_nv12UvImageMemory, nullptr);
      m_nv12UvImageMemory = VK_NULL_HANDLE;
    }
    if (m_yuv420pUImageMemory != VK_NULL_HANDLE) {
      vkFreeMemory(m_device, m_yuv420pUImageMemory, nullptr);
      m_yuv420pUImageMemory = VK_NULL_HANDLE;
    }
    if (m_yuv420pVImageMemory != VK_NULL_HANDLE) {
      vkFreeMemory(m_device, m_yuv420pVImageMemory, nullptr);
      m_yuv420pVImageMemory = VK_NULL_HANDLE;
    }
    if (m_effectsPipelineLayout != VK_NULL_HANDLE) {
      vkDestroyPipelineLayout(m_device, m_effectsPipelineLayout, nullptr);
      m_effectsPipelineLayout = VK_NULL_HANDLE;
    }
    if (m_maskPipelineLayout != VK_NULL_HANDLE) {
      vkDestroyPipelineLayout(m_device, m_maskPipelineLayout, nullptr);
      m_maskPipelineLayout = VK_NULL_HANDLE;
    }
    if (m_nv12PipelineLayout != VK_NULL_HANDLE) {
      vkDestroyPipelineLayout(m_device, m_nv12PipelineLayout, nullptr);
      m_nv12PipelineLayout = VK_NULL_HANDLE;
    }
    if (m_yuvComputePipelineLayout != VK_NULL_HANDLE) {
      vkDestroyPipelineLayout(m_device, m_yuvComputePipelineLayout, nullptr);
      m_yuvComputePipelineLayout = VK_NULL_HANDLE;
    }
    if (m_effectsVertModule != VK_NULL_HANDLE) {
      vkDestroyShaderModule(m_device, m_effectsVertModule, nullptr);
      m_effectsVertModule = VK_NULL_HANDLE;
    }
    if (m_effectsFragModule != VK_NULL_HANDLE) {
      vkDestroyShaderModule(m_device, m_effectsFragModule, nullptr);
      m_effectsFragModule = VK_NULL_HANDLE;
    }
    if (m_maskVertModule != VK_NULL_HANDLE) {
      vkDestroyShaderModule(m_device, m_maskVertModule, nullptr);
      m_maskVertModule = VK_NULL_HANDLE;
    }
    if (m_maskFragModule != VK_NULL_HANDLE) {
      vkDestroyShaderModule(m_device, m_maskFragModule, nullptr);
      m_maskFragModule = VK_NULL_HANDLE;
    }
    if (m_nv12VertModule != VK_NULL_HANDLE) {
      vkDestroyShaderModule(m_device, m_nv12VertModule, nullptr);
      m_nv12VertModule = VK_NULL_HANDLE;
    }
    if (m_nv12YFragModule != VK_NULL_HANDLE) {
      vkDestroyShaderModule(m_device, m_nv12YFragModule, nullptr);
      m_nv12YFragModule = VK_NULL_HANDLE;
    }
    if (m_nv12UvFragModule != VK_NULL_HANDLE) {
      vkDestroyShaderModule(m_device, m_nv12UvFragModule, nullptr);
      m_nv12UvFragModule = VK_NULL_HANDLE;
    }
    if (m_yuv420pUFragModule != VK_NULL_HANDLE) {
      vkDestroyShaderModule(m_device, m_yuv420pUFragModule, nullptr);
      m_yuv420pUFragModule = VK_NULL_HANDLE;
    }
    if (m_yuv420pVFragModule != VK_NULL_HANDLE) {
      vkDestroyShaderModule(m_device, m_yuv420pVFragModule, nullptr);
      m_yuv420pVFragModule = VK_NULL_HANDLE;
    }
    if (m_yuv420pComputeModule != VK_NULL_HANDLE) {
      vkDestroyShaderModule(m_device, m_yuv420pComputeModule, nullptr);
      m_yuv420pComputeModule = VK_NULL_HANDLE;
    }
    if (m_framebuffer != VK_NULL_HANDLE) {
      vkDestroyFramebuffer(m_device, m_framebuffer, nullptr);
      m_framebuffer = VK_NULL_HANDLE;
    }
    if (m_renderPass != VK_NULL_HANDLE) {
      vkDestroyRenderPass(m_device, m_renderPass, nullptr);
      m_renderPass = VK_NULL_HANDLE;
    }
    if (m_frameUniformMapped && m_frameUniformMemory != VK_NULL_HANDLE) {
      vkUnmapMemory(m_device, m_frameUniformMemory);
      m_frameUniformMapped = nullptr;
    }
    if (m_frameUniformBuffer != VK_NULL_HANDLE) {
      vkDestroyBuffer(m_device, m_frameUniformBuffer, nullptr);
      m_frameUniformBuffer = VK_NULL_HANDLE;
    }
    if (m_frameUniformMemory != VK_NULL_HANDLE) {
      vkFreeMemory(m_device, m_frameUniformMemory, nullptr);
      m_frameUniformMemory = VK_NULL_HANDLE;
    }
    if (m_descriptorPool != VK_NULL_HANDLE) {
      vkDestroyDescriptorPool(m_device, m_descriptorPool, nullptr);
      m_descriptorPool = VK_NULL_HANDLE;
    }
    if (m_yuvComputeDescriptorPool != VK_NULL_HANDLE) {
      vkDestroyDescriptorPool(m_device, m_yuvComputeDescriptorPool, nullptr);
      m_yuvComputeDescriptorPool = VK_NULL_HANDLE;
    }
    if (m_descriptorSetLayout != VK_NULL_HANDLE) {
      vkDestroyDescriptorSetLayout(m_device, m_descriptorSetLayout, nullptr);
      m_descriptorSetLayout = VK_NULL_HANDLE;
    }
    if (m_yuvComputeDescriptorSetLayout != VK_NULL_HANDLE) {
      vkDestroyDescriptorSetLayout(m_device, m_yuvComputeDescriptorSetLayout,
                                   nullptr);
      m_yuvComputeDescriptorSetLayout = VK_NULL_HANDLE;
    }
    if (m_sampler != VK_NULL_HANDLE) {
      vkDestroySampler(m_device, m_sampler, nullptr);
      m_sampler = VK_NULL_HANDLE;
    }
    if (m_colorImageView != VK_NULL_HANDLE) {
      vkDestroyImageView(m_device, m_colorImageView, nullptr);
      m_colorImageView = VK_NULL_HANDLE;
    }
    if (m_colorImage != VK_NULL_HANDLE) {
      vkDestroyImage(m_device, m_colorImage, nullptr);
      m_colorImage = VK_NULL_HANDLE;
    }
    if (m_colorImageMemory != VK_NULL_HANDLE) {
      vkFreeMemory(m_device, m_colorImageMemory, nullptr);
      m_colorImageMemory = VK_NULL_HANDLE;
    }
    if (m_commandPool != VK_NULL_HANDLE) {
      vkDestroyCommandPool(m_device, m_commandPool, nullptr);
      m_commandPool = VK_NULL_HANDLE;
    }
    if (m_device != VK_NULL_HANDLE) {
      vkDestroyDevice(m_device, nullptr);
      m_device = VK_NULL_HANDLE;
    }
    if (m_instance != VK_NULL_HANDLE) {
      vkDestroyInstance(m_instance, nullptr);
      m_instance = VK_NULL_HANDLE;
    }

    m_physicalDevice = VK_NULL_HANDLE;
    m_nonCoherentAtomSize = 1;
    m_graphicsQueue = VK_NULL_HANDLE;
    m_commandBuffer = VK_NULL_HANDLE;
    m_descriptorSet = VK_NULL_HANDLE;
    m_yuvComputeDescriptorSet = VK_NULL_HANDLE;
    m_graphicsQueueFamily = UINT32_MAX;
    m_initialized = false;
    m_yuv420pPlanesPrimed = false;
    m_colorImageLayout = VK_IMAGE_LAYOUT_UNDEFINED;
  }

  struct LayerInput : public VulkanRenderLayerPacket {
    QImage image;
    OverlayImage overlayImage;
    bool preferHardwareDirect = false;
    QString cacheKey;
    float backgroundShadows[4] = {0.0f, 0.0f, 0.0f, 0.0f};
    float backgroundMidtones[4] = {0.0f, 0.0f, 0.0f, 0.0f};
    float backgroundHighlights[4] = {0.0f, 0.0f, 0.0f, 0.0f};
    float backgroundGrade[4] = {0.0f, 1.0f, 1.0f, 0.0f};
    float mvp[16] = {1.f, 0.f, 0.f, 0.f, 0.f, 1.f, 0.f, 0.f,
                     0.f, 0.f, 1.f, 0.f, 0.f, 0.f, 0.f, 1.f};
    bool presetScissorEnabled = false;
    QRectF presetScissorRect;
  };

  using TranscriptTextInput = VulkanRenderTranscriptInput;
  using VulkanTextInputs = VulkanRenderTextInputs;

  bool writeOverlayImageToStagingTopLeft(const OverlayImage &overlay,
                                         VkDeviceSize stagingOffset) {
    if (overlay.isNull() || !m_stagingMapped) {
      return false;
    }
    const int rowBytes = overlay.width * 4;
    const size_t bytes = static_cast<size_t>(rowBytes) * static_cast<size_t>(overlay.height);
    if (overlay.rgbaPremultiplied.size() < static_cast<qsizetype>(bytes)) {
      return false;
    }
    if (!activeStagingRangeAvailable(
            stagingOffset, static_cast<VkDeviceSize>(bytes))) {
      return false;
    }
    auto *dst = reinterpret_cast<uint8_t *>(m_stagingMapped) + stagingOffset;
    const auto *src = reinterpret_cast<const uint8_t *>(overlay.rgbaPremultiplied.constData());
    for (int y = 0; y < overlay.height; ++y) {
      std::memcpy(dst + (static_cast<size_t>(y) * rowBytes),
                  src + (static_cast<size_t>(y) * rowBytes),
                  static_cast<size_t>(rowBytes));
    }
    return flushActiveStagingWrite(stagingOffset, static_cast<VkDeviceSize>(bytes));
  }

  OverlayImage placementGuideOverlay(const QSize& outputSize,
                                     bool instagramSafeAreaGuides,
                                     bool alignmentGridGuides) {
    if (!instagramSafeAreaGuides && !alignmentGridGuides) {
      return {};
    }
    if (m_cachedPlacementGuideOverlaySize == outputSize &&
        m_cachedPlacementGuideInstagramSafeArea == instagramSafeAreaGuides &&
        m_cachedPlacementGuideAlignmentGrid == alignmentGridGuides &&
        !m_cachedPlacementGuideOverlay.isNull()) {
      return m_cachedPlacementGuideOverlay;
    }

    OverlayImage overlay;
    overlay.width = qMax(1, outputSize.width());
    overlay.height = qMax(1, outputSize.height());
    overlay.rgbaPremultiplied.resize(
        static_cast<qsizetype>(overlay.width) *
        static_cast<qsizetype>(overlay.height) * 4);
    overlay.rgbaPremultiplied.fill(char(0));

    auto setPixel = [&](int x, int y, int r, int g, int b, int a) {
      if (x < 0 || x >= overlay.width || y < 0 || y >= overlay.height) {
        return;
      }
      const qsizetype offset =
          (static_cast<qsizetype>(y) * overlay.width + x) * 4;
      overlay.rgbaPremultiplied[offset + 0] = static_cast<char>((r * a) / 255);
      overlay.rgbaPremultiplied[offset + 1] = static_cast<char>((g * a) / 255);
      overlay.rgbaPremultiplied[offset + 2] = static_cast<char>((b * a) / 255);
      overlay.rgbaPremultiplied[offset + 3] = static_cast<char>(a);
    };
    auto drawHorizontal = [&](int centerY, int thickness, int r, int g, int b, int a) {
      const int half = qMax(0, thickness / 2);
      for (int y = centerY - half; y <= centerY + half; ++y) {
        for (int x = 0; x < overlay.width; ++x) {
          setPixel(x, y, r, g, b, a);
        }
      }
    };
    auto drawVertical = [&](int centerX, int thickness, int r, int g, int b, int a) {
      const int half = qMax(0, thickness / 2);
      for (int x = centerX - half; x <= centerX + half; ++x) {
        for (int y = 0; y < overlay.height; ++y) {
          setPixel(x, y, r, g, b, a);
        }
      }
    };

    const int thin = qMax(1, qMin(overlay.width, overlay.height) / 720);
    const int thick = qMax(2, thin * 2);
    if (alignmentGridGuides) {
      for (int i = 1; i <= 2; ++i) {
        drawVertical(qRound((static_cast<qreal>(overlay.width) * i) / 3.0),
                     thin, 128, 209, 255, 210);
        drawHorizontal(qRound((static_cast<qreal>(overlay.height) * i) / 3.0),
                       thin, 128, 209, 255, 210);
      }
    }
    if (instagramSafeAreaGuides) {
      const int inset = qMin(250, overlay.height / 2);
      drawHorizontal(inset, thick, 255, 214, 64, 235);
      drawHorizontal(qMax(0, overlay.height - inset), thick, 255, 214, 64, 235);
    }

    m_cachedPlacementGuideOverlay = overlay;
    m_cachedPlacementGuideOverlaySize = outputSize;
    m_cachedPlacementGuideInstagramSafeArea = instagramSafeAreaGuides;
    m_cachedPlacementGuideAlignmentGrid = alignmentGridGuides;
    return m_cachedPlacementGuideOverlay;
  }

  bool writeRgbaImageToStagingTopLeft(const QImage &rgba,
                                      VkDeviceSize stagingOffset) {
    if (rgba.isNull() || !m_stagingMapped ||
        rgba.format() != QImage::Format_RGBA8888) {
      return false;
    }
    const int rowBytes = rgba.width() * 4;
    const size_t bytes = static_cast<size_t>(rowBytes) * static_cast<size_t>(rgba.height());
    if (!activeStagingRangeAvailable(
            stagingOffset, static_cast<VkDeviceSize>(bytes))) {
      return false;
    }
    auto *dst = reinterpret_cast<uint8_t *>(m_stagingMapped) + stagingOffset;
    for (int y = 0; y < rgba.height(); ++y) {
      std::memcpy(dst + (static_cast<size_t>(y) * rowBytes),
                  rgba.constScanLine(y),
                  static_cast<size_t>(rowBytes));
    }
    return flushActiveStagingWrite(stagingOffset, static_cast<VkDeviceSize>(bytes));
  }

  bool writeImageBufferToStagingTopLeft(const jcut::core::ImageBuffer &image,
                                        VkDeviceSize stagingOffset,
                                        int bytesPerPixel) {
    if (image.empty() || !m_stagingMapped || bytesPerPixel <= 0 ||
        image.strideBytes < image.size.width * bytesPerPixel) {
      return false;
    }
    const int rowBytes = image.size.width * bytesPerPixel;
    const size_t bytes =
        static_cast<size_t>(rowBytes) * static_cast<size_t>(image.size.height);
    if (!activeStagingRangeAvailable(
            stagingOffset, static_cast<VkDeviceSize>(bytes))) {
      return false;
    }
    auto *dst = reinterpret_cast<uint8_t *>(m_stagingMapped) + stagingOffset;
    for (int y = 0; y < image.size.height; ++y) {
      std::memcpy(dst + (static_cast<size_t>(y) * rowBytes),
                  image.bytes.data() +
                      (static_cast<size_t>(y) *
                       static_cast<size_t>(image.strideBytes)),
                  static_cast<size_t>(rowBytes));
    }
    return flushActiveStagingWrite(stagingOffset,
                                   static_cast<VkDeviceSize>(bytes));
  }

  bool preprocessLayerMask(LayerTextureSlot &slot,
                           const LayerInput &layer,
                           VkDeviceSize correctionStorageOffset,
                           VkDeviceSize correctionStorageCapacity) {
    VulkanMaskPreprocessor::Images images;
    images.sampler = m_sampler;
    images.inputSize = layer.maskSourceSize.isValid()
        ? layer.maskSourceSize
        : m_outputSize;
    images.outputSize = m_outputSize;
    images.inputView = slot.maskRawView;
    images.outputImage = slot.maskImage;
    images.outputView = slot.maskView;
    images.outputLayout = &slot.maskLayout;
    images.workImage = slot.maskWorkImage;
    images.workView = slot.maskWorkView;
    images.workLayout = &slot.maskWorkLayout;
    VulkanMaskPreprocessOptions options;
    options.outputSize = m_outputSize;
    options.sourceIdentity = layer.maskIdentity;
    options.correctionStorage = layer.maskCorrectionStorage;
    options.invert = layer.maskInvert;
    options.erodeRadius = qRound(qMax<qreal>(0.0, layer.maskErode));
    options.dilateRadius = qRound(qMax<qreal>(0.0, layer.maskDilate));
    options.blurRadius = qRound(qMax<qreal>(0.0, layer.maskBlur));
    slot.maskUploaded = m_maskPreprocessor.record(
        m_commandBuffer,
        images,
        options,
        [this, correctionStorageOffset, correctionStorageCapacity](
            const QByteArray& storage,
            VkDeviceSize alignment,
            VulkanMaskPreprocessor::StagedCorrectionStorage* staged) {
          if (!staged || alignment == 0 ||
              (correctionStorageOffset % alignment) != 0 ||
              static_cast<VkDeviceSize>(storage.size()) >
                  correctionStorageCapacity ||
              !activeStagingRangeAvailable(
                  correctionStorageOffset,
                  static_cast<VkDeviceSize>(storage.size()))) {
            return false;
          }
          std::memcpy(
              reinterpret_cast<std::uint8_t*>(m_stagingMapped) +
                  correctionStorageOffset,
              storage.constData(),
              static_cast<std::size_t>(storage.size()));
          if (!flushActiveStagingWrite(
                  correctionStorageOffset,
                  static_cast<VkDeviceSize>(storage.size()))) {
            return false;
          }
          staged->buffer = m_stagingBuffer;
          staged->offset = correctionStorageOffset;
          staged->bytes = static_cast<VkDeviceSize>(storage.size());
          return true;
        });
    return slot.maskUploaded;
  }

  bool ensureMaskRawImage(LayerTextureSlot &slot,
                          const QSize &size,
                          VkFormat format = VK_FORMAT_R8G8B8A8_UNORM) {
    if (!size.isValid() || size.isEmpty()) {
      return false;
    }
    if (slot.maskRawImage != VK_NULL_HANDLE &&
        slot.maskRawMemory != VK_NULL_HANDLE &&
        slot.maskRawView != VK_NULL_HANDLE &&
        slot.maskRawSize == size &&
        slot.maskRawFormat == format) {
      return true;
    }
    if (vkDeviceWaitIdle(m_device) != VK_SUCCESS) {
      return false;
    }
    if (slot.maskRawView != VK_NULL_HANDLE) {
      vkDestroyImageView(m_device, slot.maskRawView, nullptr);
    }
    if (slot.maskRawImage != VK_NULL_HANDLE) {
      vkDestroyImage(m_device, slot.maskRawImage, nullptr);
    }
    if (slot.maskRawMemory != VK_NULL_HANDLE) {
      vkFreeMemory(m_device, slot.maskRawMemory, nullptr);
    }
    slot.maskRawView = VK_NULL_HANDLE;
    slot.maskRawImage = VK_NULL_HANDLE;
    slot.maskRawMemory = VK_NULL_HANDLE;
    slot.maskRawLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    slot.maskRawSize = {};
    slot.maskRawFormat = VK_FORMAT_UNDEFINED;

    VkImageCreateInfo imageInfo{};
    imageInfo.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    imageInfo.imageType = VK_IMAGE_TYPE_2D;
    imageInfo.extent = {
        static_cast<uint32_t>(size.width()),
        static_cast<uint32_t>(size.height()), 1};
    imageInfo.mipLevels = 1;
    imageInfo.arrayLayers = 1;
    imageInfo.format = format;
    imageInfo.tiling = VK_IMAGE_TILING_OPTIMAL;
    imageInfo.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    imageInfo.usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT |
                      VK_IMAGE_USAGE_STORAGE_BIT |
                      VK_IMAGE_USAGE_SAMPLED_BIT;
    imageInfo.samples = VK_SAMPLE_COUNT_1_BIT;
    imageInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    if (vkCreateImage(
            m_device, &imageInfo, nullptr,
            &slot.maskRawImage) != VK_SUCCESS) {
      return false;
    }
    VkMemoryRequirements requirements{};
    vkGetImageMemoryRequirements(
        m_device, slot.maskRawImage, &requirements);
    const uint32_t memoryType =
        findMemoryType(
            m_physicalDevice,
            requirements.memoryTypeBits,
            VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    if (memoryType == UINT32_MAX) {
      return false;
    }
    VkMemoryAllocateInfo allocation{};
    allocation.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    allocation.allocationSize = requirements.size;
    allocation.memoryTypeIndex = memoryType;
    if (vkAllocateMemory(
            m_device, &allocation, nullptr,
            &slot.maskRawMemory) != VK_SUCCESS ||
        vkBindImageMemory(
            m_device, slot.maskRawImage,
            slot.maskRawMemory, 0) != VK_SUCCESS) {
      return false;
    }
    VkImageViewCreateInfo view{};
    view.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    view.image = slot.maskRawImage;
    view.viewType = VK_IMAGE_VIEW_TYPE_2D;
    view.format = format;
    view.subresourceRange.aspectMask =
        VK_IMAGE_ASPECT_COLOR_BIT;
    view.subresourceRange.levelCount = 1;
    view.subresourceRange.layerCount = 1;
    if (vkCreateImageView(
            m_device, &view, nullptr,
            &slot.maskRawView) != VK_SUCCESS) {
      return false;
    }
    slot.maskRawSize = size;
    slot.maskRawFormat = format;
    return true;
  }

  bool submitAndWait() {
    if (!submitActiveSlot()) {
      return false;
    }
    return waitSlot(m_activeSlotIndex);
  }

  bool submitActiveSlot() {
    if (m_activeSlotIndex < 0 || m_activeSlotIndex >= m_frameSlots.size() ||
        m_submitFence == VK_NULL_HANDLE) {
      return false;
    }
    FrameSlot &slot = m_frameSlots[m_activeSlotIndex];
    if (slot.commandBuffer == VK_NULL_HANDLE || slot.fence == VK_NULL_HANDLE) {
      return false;
    }
    vkResetFences(m_device, 1, &slot.fence);
    VkSubmitInfo submitInfo{};
    submitInfo.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    VkPipelineStageFlags waitStage = VK_PIPELINE_STAGE_TRANSFER_BIT;
    if (m_pendingPreviewWait != VK_NULL_HANDLE) {
      submitInfo.waitSemaphoreCount = 1;
      submitInfo.pWaitSemaphores = &m_pendingPreviewWait;
      submitInfo.pWaitDstStageMask = &waitStage;
    }
    submitInfo.commandBufferCount = 1;
    submitInfo.pCommandBuffers = &slot.commandBuffer;
    if (m_pendingPreviewSignal != VK_NULL_HANDLE) {
      submitInfo.signalSemaphoreCount = 1;
      submitInfo.pSignalSemaphores = &m_pendingPreviewSignal;
    }
    if (vkQueueSubmit(m_graphicsQueue, 1, &submitInfo, slot.fence) !=
        VK_SUCCESS) {
      return false;
    }
    m_pendingPreviewWait = VK_NULL_HANDLE;
    m_pendingPreviewSignal = VK_NULL_HANDLE;
    slot.inFlight = true;
    return true;
  }

  bool publishLastFrameForGpuPreview(OffscreenVulkanFrame *frame,
                                     QString *errorMessage) {
    if (frame) {
      *frame = OffscreenVulkanFrame{};
    }
    if (!frame || !m_commandBufferOpenForConversion ||
        m_previewSlots.size() < 3 || m_activeSlotIndex < 0) {
      if (errorMessage) {
        *errorMessage =
            QStringLiteral("GPU preview double buffers are unavailable.");
      }
      return false;
    }
    int previewIndex = -1;
    for (int offset = 1; offset <= m_previewSlots.size(); ++offset) {
      const int candidate =
          (m_lastPreviewSlotIndex + offset) % m_previewSlots.size();
      const PreviewSlot &candidateSlot = m_previewSlots[candidate];
      if (!candidateSlot.published ||
          (candidateSlot.consumptionState &&
           candidateSlot.consumptionState->completedGeneration.load(
               std::memory_order_acquire) >= candidateSlot.generation)) {
        previewIndex = candidate;
        break;
      }
    }
    if (previewIndex < 0) {
      if (errorMessage) {
        *errorMessage = QStringLiteral(
            "GPU export preview dropped: all optional preview slots are busy.");
      }
      return false;
    }
    PreviewSlot &slot = m_previewSlots[previewIndex];
    // Host acknowledgment proves that the prior consumed signal has already
    // been queued. Preserve the semaphore wait for device ownership without
    // ever enqueueing it for an unresponsive optional consumer.
    m_pendingPreviewWait =
        slot.published ? slot.consumedSemaphore : VK_NULL_HANDLE;
    m_pendingPreviewSignal = slot.readySemaphore;

    transitionImageLayout(m_commandBuffer, m_colorImage, m_colorImageLayout,
                          VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
    m_colorImageLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    if (slot.published) {
      VkImageMemoryBarrier acquire{};
      acquire.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
      acquire.srcAccessMask = 0;
      acquire.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
      acquire.oldLayout = slot.layout;
      acquire.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
      acquire.srcQueueFamilyIndex = VK_QUEUE_FAMILY_EXTERNAL;
      acquire.dstQueueFamilyIndex = m_graphicsQueueFamily;
      acquire.image = slot.image;
      acquire.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
      acquire.subresourceRange.levelCount = 1;
      acquire.subresourceRange.layerCount = 1;
      vkCmdPipelineBarrier(m_commandBuffer,
                           VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                           VK_PIPELINE_STAGE_TRANSFER_BIT,
                           0,
                           0,
                           nullptr,
                           0,
                           nullptr,
                           1,
                           &acquire);
    } else {
      transitionImageLayout(m_commandBuffer, slot.image, slot.layout,
                            VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
    }
    slot.layout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    VkImageCopy copy{};
    copy.srcSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    copy.srcSubresource.layerCount = 1;
    copy.dstSubresource = copy.srcSubresource;
    copy.extent = {
        static_cast<uint32_t>(m_outputSize.width()),
        static_cast<uint32_t>(m_outputSize.height()),
        1};
    vkCmdCopyImage(m_commandBuffer,
                   m_colorImage,
                   VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                   slot.image,
                   VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                   1,
                   &copy);
    VkImageMemoryBarrier release{};
    release.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    release.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    release.dstAccessMask = 0;
    release.oldLayout = slot.layout;
    release.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    release.srcQueueFamilyIndex = m_graphicsQueueFamily;
    release.dstQueueFamilyIndex = VK_QUEUE_FAMILY_EXTERNAL;
    release.image = slot.image;
    release.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    release.subresourceRange.levelCount = 1;
    release.subresourceRange.layerCount = 1;
    vkCmdPipelineBarrier(m_commandBuffer,
                         VK_PIPELINE_STAGE_TRANSFER_BIT,
                         VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT,
                         0,
                         0,
                         nullptr,
                         0,
                         nullptr,
                         1,
                         &release);
    slot.layout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    transitionImageLayout(m_commandBuffer, m_colorImage, m_colorImageLayout,
                          VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
    m_colorImageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;

    int readyFd = -1;
    int consumedFd = -1;
    if (!slot.handlesExported) {
      VkSemaphoreGetFdInfoKHR fdInfo{};
      fdInfo.sType = VK_STRUCTURE_TYPE_SEMAPHORE_GET_FD_INFO_KHR;
      fdInfo.handleType =
          VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_OPAQUE_FD_BIT;
      fdInfo.semaphore = slot.readySemaphore;
      if (m_vkGetSemaphoreFdKHR(m_device, &fdInfo, &readyFd) != VK_SUCCESS) {
        readyFd = -1;
      }
      fdInfo.semaphore = slot.consumedSemaphore;
      if (m_vkGetSemaphoreFdKHR(m_device, &fdInfo, &consumedFd) !=
          VK_SUCCESS) {
        consumedFd = -1;
      }
      if (readyFd < 0 || consumedFd < 0) {
        if (readyFd >= 0) {
          close(readyFd);
        }
        if (consumedFd >= 0) {
          close(consumedFd);
        }
        m_pendingPreviewWait = VK_NULL_HANDLE;
        m_pendingPreviewSignal = VK_NULL_HANDLE;
        if (errorMessage) {
          *errorMessage =
              QStringLiteral("Failed to export GPU preview semaphores.");
        }
        return false;
      }
      slot.handlesExported = true;
    }

    ++slot.generation;
    slot.published = true;
    m_lastPreviewSlotIndex = previewIndex;
    frame->physicalDevice = m_physicalDevice;
    frame->device = m_device;
    frame->queue = m_graphicsQueue;
    frame->queueFamilyIndex = m_graphicsQueueFamily;
    frame->image = slot.image;
    frame->imageView = slot.view;
    frame->imageMemory = slot.memory;
    frame->imageLayout = slot.layout;
    frame->imageFormat = VK_FORMAT_B8G8R8A8_UNORM;
    frame->readySemaphoreFd = readyFd;
    frame->consumedSemaphoreFd = consumedFd;
    frame->bufferIndex = static_cast<std::uint32_t>(previewIndex);
    frame->memoryTypeIndex = slot.memoryTypeIndex;
    frame->memoryAllocationSize = slot.memoryAllocationSize;
    frame->producerSessionId = m_producerSessionId;
    frame->generation = slot.generation;
    frame->consumptionState = slot.consumptionState;
    frame->size = {m_outputSize.width(), m_outputSize.height()};
    frame->queueSupportsCompute = m_graphicsQueueSupportsCompute;
    frame->valid = true;
    if (slot.generation == 1) {
      qInfo().noquote()
          << QStringLiteral(
                 "[render-export-preview] published GPU slot=%1 "
                 "producer_session=%2")
                 .arg(previewIndex)
                 .arg(m_producerSessionId);
    }
    return true;
  }

  void destroyPreviewSlots() {
    for (PreviewSlot &slot : m_previewSlots) {
      if (slot.readySemaphore != VK_NULL_HANDLE) {
        vkDestroySemaphore(m_device, slot.readySemaphore, nullptr);
      }
      if (slot.consumedSemaphore != VK_NULL_HANDLE) {
        vkDestroySemaphore(m_device, slot.consumedSemaphore, nullptr);
      }
      if (slot.view != VK_NULL_HANDLE) {
        vkDestroyImageView(m_device, slot.view, nullptr);
      }
      if (slot.image != VK_NULL_HANDLE) {
        vkDestroyImage(m_device, slot.image, nullptr);
      }
      if (slot.memory != VK_NULL_HANDLE) {
        vkFreeMemory(m_device, slot.memory, nullptr);
      }
    }
    m_previewSlots.clear();
    m_lastPreviewSlotIndex = -1;
    m_pendingPreviewWait = VK_NULL_HANDLE;
    m_pendingPreviewSignal = VK_NULL_HANDLE;
  }

  bool waitSlot(int slotIndex) {
    if (slotIndex < 0 || slotIndex >= m_frameSlots.size()) {
      return false;
    }
    FrameSlot &slot = m_frameSlots[slotIndex];
    if (slot.inFlight) {
      const VkResult waitResult = vkWaitForFences(
          m_device,
          1,
          &slot.fence,
          VK_TRUE,
          kExportGpuFenceTimeoutNs);
      if (waitResult != VK_SUCCESS) {
        qWarning().noquote()
            << QStringLiteral(
                   "[vulkan-sync-timeout] stage=export_frame_slot "
                   "slot=%1 timeout_ms=5000 result=%2 "
                   "preview_wait_pending=%3 preview_signal_pending=%4")
                   .arg(slotIndex)
                   .arg(static_cast<int>(waitResult))
                   .arg(m_pendingPreviewWait != VK_NULL_HANDLE)
                   .arg(m_pendingPreviewSignal != VK_NULL_HANDLE);
        return false;
      }
      slot.inFlight = false;
    }
    return true;
  }

  bool activeStagingRangeAvailable(VkDeviceSize offset,
                                   VkDeviceSize size) const {
    if (m_activeSlotIndex < 0 ||
        m_activeSlotIndex >= m_frameSlots.size()) {
      return false;
    }
    const FrameSlot &slot = m_frameSlots[m_activeSlotIndex];
    return slot.stagingBuffer != VK_NULL_HANDLE &&
        slot.stagingMemory != VK_NULL_HANDLE &&
        slot.stagingMapped != nullptr &&
        offset <= slot.stagingBufferSize &&
        size <= slot.stagingBufferSize - offset;
  }

  bool ensureActiveStagingCapacity(VkDeviceSize requiredSize) {
    if (requiredSize == 0 || m_activeSlotIndex < 0 ||
        m_activeSlotIndex >= m_frameSlots.size()) {
      return false;
    }
    FrameSlot &slot = m_frameSlots[m_activeSlotIndex];
    if (slot.inFlight) {
      return false;
    }
    if (slot.stagingBuffer != VK_NULL_HANDLE &&
        slot.stagingMemory != VK_NULL_HANDLE &&
        slot.stagingMapped != nullptr &&
        slot.stagingBufferSize >= requiredSize) {
      return true;
    }

    VkBufferCreateInfo bufferInfo{};
    bufferInfo.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    bufferInfo.size = requiredSize;
    bufferInfo.usage =
        VK_BUFFER_USAGE_TRANSFER_SRC_BIT |
        VK_BUFFER_USAGE_TRANSFER_DST_BIT |
        VK_BUFFER_USAGE_STORAGE_BUFFER_BIT;
    bufferInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    VkBuffer newBuffer = VK_NULL_HANDLE;
    if (vkCreateBuffer(
            m_device, &bufferInfo, nullptr, &newBuffer) != VK_SUCCESS) {
      return false;
    }

    VkMemoryRequirements requirements{};
    vkGetBufferMemoryRequirements(m_device, newBuffer, &requirements);
    VkMemoryPropertyFlags memoryFlags = 0;
    const uint32_t memoryType = findMemoryTypePreferred(
        m_physicalDevice, requirements.memoryTypeBits,
        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT,
        VK_MEMORY_PROPERTY_HOST_CACHED_BIT, &memoryFlags);
    if (memoryType == UINT32_MAX) {
      vkDestroyBuffer(m_device, newBuffer, nullptr);
      return false;
    }

    VkMemoryAllocateInfo allocation{};
    allocation.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    allocation.allocationSize = requirements.size;
    allocation.memoryTypeIndex = memoryType;
    VkDeviceMemory newMemory = VK_NULL_HANDLE;
    if (vkAllocateMemory(
            m_device, &allocation, nullptr, &newMemory) != VK_SUCCESS) {
      vkDestroyBuffer(m_device, newBuffer, nullptr);
      return false;
    }
    if (vkBindBufferMemory(
            m_device, newBuffer, newMemory, 0) != VK_SUCCESS) {
      vkFreeMemory(m_device, newMemory, nullptr);
      vkDestroyBuffer(m_device, newBuffer, nullptr);
      return false;
    }
    void *newMapped = nullptr;
    if (vkMapMemory(
            m_device, newMemory, 0, VK_WHOLE_SIZE, 0,
            &newMapped) != VK_SUCCESS) {
      vkDestroyBuffer(m_device, newBuffer, nullptr);
      vkFreeMemory(m_device, newMemory, nullptr);
      return false;
    }

    if (slot.stagingMapped && slot.stagingMemory != VK_NULL_HANDLE) {
      vkUnmapMemory(m_device, slot.stagingMemory);
    }
    if (slot.stagingBuffer != VK_NULL_HANDLE) {
      vkDestroyBuffer(m_device, slot.stagingBuffer, nullptr);
    }
    if (slot.stagingMemory != VK_NULL_HANDLE) {
      vkFreeMemory(m_device, slot.stagingMemory, nullptr);
    }
    slot.stagingBuffer = newBuffer;
    slot.stagingMemory = newMemory;
    slot.stagingBufferSize = requiredSize;
    slot.stagingAllocationSize = requirements.size;
    slot.stagingMapped = newMapped;
    slot.stagingHostCoherent =
        (memoryFlags & VK_MEMORY_PROPERTY_HOST_COHERENT_BIT) != 0;
    m_stagingBuffer = slot.stagingBuffer;
    m_stagingMemory = slot.stagingMemory;
    m_stagingMapped = slot.stagingMapped;
    return true;
  }

  bool invalidateSlotForHostRead(FrameSlot &slot) {
    if (slot.stagingHostCoherent || slot.stagingMemory == VK_NULL_HANDLE) {
      return true;
    }
    VkMappedMemoryRange range{};
    range.sType = VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE;
    range.memory = slot.stagingMemory;
    range.offset = 0;
    range.size = slot.stagingAllocationSize;
    return vkInvalidateMappedMemoryRanges(m_device, 1, &range) == VK_SUCCESS;
  }

  bool flushActiveStagingWrite(VkDeviceSize offset, VkDeviceSize size) {
    if (m_activeSlotIndex < 0 || m_activeSlotIndex >= m_frameSlots.size()) {
      return false;
    }
    FrameSlot &slot = m_frameSlots[m_activeSlotIndex];
    if (!activeStagingRangeAvailable(offset, size)) {
      return false;
    }
    if (slot.stagingHostCoherent || slot.stagingMemory == VK_NULL_HANDLE ||
        size == 0) {
      return true;
    }
    const auto flushRange = alignedVulkanStagingFlushRange(
        static_cast<std::uint64_t>(offset),
        static_cast<std::uint64_t>(size),
        static_cast<std::uint64_t>(slot.stagingAllocationSize),
        static_cast<std::uint64_t>(m_nonCoherentAtomSize));
    if (!flushRange.has_value()) {
      return false;
    }
    VkMappedMemoryRange range{};
    range.sType = VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE;
    range.memory = slot.stagingMemory;
    range.offset = static_cast<VkDeviceSize>(flushRange->offset);
    range.size = static_cast<VkDeviceSize>(flushRange->size);
    return vkFlushMappedMemoryRanges(m_device, 1, &range) == VK_SUCCESS;
  }

  void useSlot(int slotIndex) {
    FrameSlot &slot = m_frameSlots[slotIndex];
    m_activeSlotIndex = slotIndex;
    m_commandBuffer = slot.commandBuffer;
    m_stagingBuffer = slot.stagingBuffer;
    m_stagingMemory = slot.stagingMemory;
    m_stagingMapped = slot.stagingMapped;
    m_submitFence = slot.fence;
  }

  bool selectNextSlot() {
    if (m_frameSlots.isEmpty()) {
      return false;
    }
    const int next =
        (m_activeSlotIndex + 1 + m_frameSlots.size()) % m_frameSlots.size();
    // Staging memory/fences are per-slot. Render targets are still
    // queue-ordered shared images.
    if (!waitSlot(next)) {
      return false;
    }
    useSlot(next);
    return true;
  }

  QImage renderFrameFromLayers(const QVector<LayerInput> &layers,
                               const VulkanTextInputs &textInputs,
                               bool readbackToImage,
                               OffscreenVulkanFrame *gpuPreviewFrame = nullptr,
                               QString *gpuPreviewError = nullptr,
                               QString *failureReason = nullptr) {
    if (!m_initialized || m_device == VK_NULL_HANDLE ||
        m_commandBuffer == VK_NULL_HANDLE) {
      return QImage();
    }
    if (!selectNextSlot()) {
      return QImage();
    }
    const size_t activeTextFrameSlot =
        static_cast<size_t>(qMax(0, m_activeSlotIndex));
    const size_t textFrameSlotCount =
        static_cast<size_t>(qMax(1, m_frameSlots.size()));
    if (m_transcriptTextRenderer &&
        m_transcriptTextRenderer->isReady() &&
        !m_transcriptTextRenderer->beginFrameUploads(activeTextFrameSlot,
                                                     textFrameSlotCount)) {
      if (failureReason) {
        *failureReason = QStringLiteral(
            "Vulkan export text renderer failed to begin transcript/title "
            "atlas uploads for frame slot %1/%2.")
            .arg(activeTextFrameSlot)
            .arg(textFrameSlotCount);
      }
      return QImage();
    }
    if (m_speakerTextRenderer &&
        m_speakerTextRenderer->isReady() &&
        !m_speakerTextRenderer->beginFrameUploads(activeTextFrameSlot,
                                                  textFrameSlotCount)) {
      if (failureReason) {
        *failureReason = QStringLiteral(
            "Vulkan export text renderer failed to begin speaker-label atlas "
            "uploads for frame slot %1/%2.")
            .arg(activeTextFrameSlot)
            .arg(textFrameSlotCount);
      }
      return QImage();
    }

    const auto rgbaBytesForSize = [](const QSize &size) -> VkDeviceSize {
      if (!size.isValid() || size.isEmpty()) {
        return 0;
      }
      return static_cast<VkDeviceSize>(size.width()) *
          static_cast<VkDeviceSize>(size.height()) * 4;
    };
    const VkDeviceSize layerImageBytes = rgbaBytesForSize(m_outputSize);
    VkDeviceSize maxAuxiliaryImageBytes = layerImageBytes;
    VkDeviceSize maxCorrectionStorageBytes = sizeof(float) * 4;
    for (const LayerInput &layer : layers) {
      if (layer.maskTextureEnabled && layer.maskBuffer) {
        maxAuxiliaryImageBytes = qMax(
            maxAuxiliaryImageBytes,
            static_cast<VkDeviceSize>(layer.maskBuffer->size.width) *
                static_cast<VkDeviceSize>(layer.maskBuffer->size.height));
      }
      if (layer.differenceMatteEnabled &&
          !layer.differenceReferenceFrame.hasHardwareFrame()) {
        maxAuxiliaryImageBytes = qMax(
            maxAuxiliaryImageBytes,
            rgbaBytesForSize(layer.differenceReferenceFrame.size()));
      }
      maxCorrectionStorageBytes = qMax(
          maxCorrectionStorageBytes,
          static_cast<VkDeviceSize>(layer.maskCorrectionStorage.size()));
    }
    constexpr VkDeviceSize maxDeviceSize =
        std::numeric_limits<VkDeviceSize>::max();
    const VkDeviceSize curveBytes = kCurveLutBytes * 2;
    if (layerImageBytes > maxDeviceSize - curveBytes ||
        maxAuxiliaryImageBytes >
            maxDeviceSize - layerImageBytes - curveBytes) {
      return QImage();
    }
    const VkDeviceSize alignment = qMax<VkDeviceSize>(
        16, m_storageBufferOffsetAlignment);
    const VkDeviceSize unalignedCorrectionStorageOffset =
        layerImageBytes + curveBytes + maxAuxiliaryImageBytes;
    if (unalignedCorrectionStorageOffset >
        maxDeviceSize - (alignment - 1)) {
      return QImage();
    }
    const VkDeviceSize correctionStorageOffsetWithinLayer =
        ((unalignedCorrectionStorageOffset + alignment - 1) / alignment) *
        alignment;
    if (maxCorrectionStorageBytes >
        maxDeviceSize - correctionStorageOffsetWithinLayer) {
      return QImage();
    }
    const VkDeviceSize unalignedLayerStagingSize =
        correctionStorageOffsetWithinLayer + maxCorrectionStorageBytes;
    if (unalignedLayerStagingSize > maxDeviceSize - (alignment - 1)) {
      return QImage();
    }
    const VkDeviceSize layerStagingSize =
        ((unalignedLayerStagingSize + alignment - 1) / alignment) *
        alignment;
    const VkDeviceSize stagingLayerCount = static_cast<VkDeviceSize>(
        qMin(kMaxLayerTextures,
             qMax(1, static_cast<int>(layers.size()))));
    if (layerStagingSize > maxDeviceSize / stagingLayerCount) {
      return QImage();
    }
    const VkDeviceSize requiredStagingBytes =
        layerStagingSize * stagingLayerCount;
    if (!ensureActiveStagingCapacity(requiredStagingBytes)) {
      qWarning().noquote()
          << QStringLiteral(
                 "[vulkan-compose] unable to provide %1 bytes of per-frame "
                 "staging for raw GPU auxiliary preprocessing")
                 .arg(static_cast<qulonglong>(requiredStagingBytes));
      return QImage();
    }

    vkResetCommandBuffer(m_commandBuffer, 0);

    VkCommandBufferBeginInfo beginInfo{};
    beginInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    if (vkBeginCommandBuffer(m_commandBuffer, &beginInfo) != VK_SUCCESS) {
      return QImage();
    }

    struct Push {
      float mvp[16];
      float brightness;
      float contrast;
      float saturation;
      float opacity;
      float shadows[4];
      float midtones[4];
      float highlights[4];
    } push{};
    auto updateFrameUniformForDraw =
        [&](const LayerInput* layer = nullptr, const float* effectParams = nullptr) -> uint32_t {
      if (!m_frameUniformMapped || m_frameUniformStride == 0) {
        return 0u;
      }
      FrameUniformData values;
      values.outputSizeAndInverse[0] = static_cast<float>(qMax(1, m_outputSize.width()));
      values.outputSizeAndInverse[1] = static_cast<float>(qMax(1, m_outputSize.height()));
      values.outputSizeAndInverse[2] = 1.0f / values.outputSizeAndInverse[0];
      values.outputSizeAndInverse[3] = 1.0f / values.outputSizeAndInverse[1];
      if (layer) {
        std::memcpy(values.backgroundShadows,
                    layer->backgroundShadows,
                    sizeof(values.backgroundShadows));
        std::memcpy(values.backgroundMidtones,
                    layer->backgroundMidtones,
                    sizeof(values.backgroundMidtones));
        std::memcpy(values.backgroundHighlights,
                    layer->backgroundHighlights,
                    sizeof(values.backgroundHighlights));
        std::memcpy(values.backgroundGrade,
                    layer->backgroundGrade,
                    sizeof(values.backgroundGrade));
      }
      if (effectParams) {
        std::memcpy(values.effectParams, effectParams, sizeof(values.effectParams));
      }
      const VkDeviceSize offset =
          m_frameUniformStride * static_cast<VkDeviceSize>(m_frameUniformRingIndex);
      std::memcpy(static_cast<char*>(m_frameUniformMapped) + offset,
                  &values,
                  sizeof(values));
      m_frameUniformRingIndex = (m_frameUniformRingIndex + 1) % kFrameUniformRingCount;
      return static_cast<uint32_t>(offset);
    };
    transitionImageLayout(m_commandBuffer, m_colorImage, m_colorImageLayout,
                          VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
    m_colorImageLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    VkClearColorValue clearColor{};
    clearColor.float32[0] = 0.0f;
    clearColor.float32[1] = 0.0f;
    clearColor.float32[2] = 0.0f;
    clearColor.float32[3] = 1.0f;
    VkImageSubresourceRange clearRange{};
    clearRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    clearRange.baseMipLevel = 0;
    clearRange.levelCount = 1;
    clearRange.baseArrayLayer = 0;
    clearRange.layerCount = 1;
    vkCmdClearColorImage(m_commandBuffer, m_colorImage,
                         VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &clearColor, 1,
                         &clearRange);
    transitionImageLayout(m_commandBuffer, m_colorImage,
                          VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                          VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
    m_colorImageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;

    VkClearValue clearValue{};
    clearValue.color = {{0.0f, 0.0f, 0.0f, 1.0f}};
    VkRenderPassBeginInfo renderPassBeginInfo{};
    renderPassBeginInfo.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
    renderPassBeginInfo.renderPass = m_renderPass;
    renderPassBeginInfo.framebuffer = m_framebuffer;
    renderPassBeginInfo.renderArea.offset = {0, 0};
    renderPassBeginInfo.renderArea.extent = {
        static_cast<uint32_t>(m_outputSize.width()),
        static_cast<uint32_t>(m_outputSize.height())};
    renderPassBeginInfo.clearValueCount = 1;
    renderPassBeginInfo.pClearValues = &clearValue;
    VkViewport fullViewport{};
    fullViewport.x = 0.0f;
    fullViewport.y = 0.0f;
    fullViewport.width = static_cast<float>(m_outputSize.width());
    fullViewport.height = static_cast<float>(m_outputSize.height());
    fullViewport.minDepth = 0.0f;
    fullViewport.maxDepth = 1.0f;
    VkRect2D fullScissor{};
    fullScissor.offset = {0, 0};
    fullScissor.extent = {static_cast<uint32_t>(m_outputSize.width()),
                          static_cast<uint32_t>(m_outputSize.height())};
    auto layerHasRenderableSource = [](const LayerInput &layer) {
      return !layer.overlayImage.isNull() || !layer.image.isNull() ||
             !layer.frame.isNull();
    };
    auto updateLayerDescriptorSet = [&](LayerTextureSlot &slot,
                                        VkImageView sourceView,
                                        VkImageLayout sourceLayout,
                                        VkImageView auxiliaryView = VK_NULL_HANDLE,
                                        VkImageLayout auxiliaryLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL) {
      VkDescriptorImageInfo di[4]{};
      di[0].imageLayout = sourceLayout;
      di[0].imageView = sourceView;
      di[0].sampler = m_sampler;
      di[1].imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
      di[1].imageView = slot.curveLutView;
      di[1].sampler = m_sampler;
      di[2].imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
      di[2].imageView = auxiliaryView != VK_NULL_HANDLE ? auxiliaryView : slot.maskView;
      di[2].imageLayout = auxiliaryView != VK_NULL_HANDLE ? auxiliaryLayout : VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
      di[2].sampler = m_sampler;
      di[3].imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
      di[3].imageView = slot.maskCurveLutView;
      di[3].sampler = m_sampler;
      VkDescriptorBufferInfo frameUniformInfo{};
      frameUniformInfo.buffer = m_frameUniformBuffer;
      frameUniformInfo.range = sizeof(FrameUniformData);
      VkWriteDescriptorSet writes[5]{};
      for (uint32_t binding = 0; binding < 4; ++binding) {
        writes[binding].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        writes[binding].dstSet = slot.descriptorSet;
        writes[binding].dstBinding = binding;
        writes[binding].descriptorCount = 1;
        writes[binding].descriptorType =
            VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        writes[binding].pImageInfo = &di[binding];
      }
      writes[4].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
      writes[4].dstSet = slot.descriptorSet;
      writes[4].dstBinding = 4;
      writes[4].descriptorCount = 1;
      writes[4].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC;
      writes[4].pBufferInfo = &frameUniformInfo;
      vkUpdateDescriptorSets(m_device, 5, writes, 0, nullptr);
    };
    auto prepareLayerSource =
        [&](LayerTextureSlot &slot, const LayerInput &layer,
            VkDeviceSize stagingOffset, VkImageView *sourceViewOut,
            VkImageLayout *sourceLayoutOut) -> bool {
      if (sourceViewOut) {
        *sourceViewOut = VK_NULL_HANDLE;
      }
      if (sourceLayoutOut) {
        *sourceLayoutOut = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
      }
      if (layer.preferHardwareDirect && !layer.frame.isNull() &&
          layer.frame.hasHardwareFrame()) {
        if (!slot.hardwareFrameHandoff) {
          auto handoff = std::make_shared<
              jcut::vulkan_detector::VulkanDetectorFrameHandoff>();
          jcut::vulkan_detector::VulkanDeviceContext context;
          context.physicalDevice = m_physicalDevice;
          context.device = m_device;
          context.queue = m_graphicsQueue;
          context.queueFamilyIndex = m_graphicsQueueFamily;
          std::string handoffError;
          if (!handoff->initialize(context, &handoffError)) {
            qWarning().noquote()
                << QStringLiteral("[vulkan-compose] hardware frame handoff "
                                  "initialization failed: %1")
                       .arg(QString::fromStdString(handoffError));
          } else {
            slot.hardwareFrameHandoff = handoff;
          }
        }
        if (slot.hardwareFrameHandoff) {
          std::string uploadError;
          if (slot.hardwareFrameHandoff->uploadFrame(layer.frame, false,
                                                     nullptr, &uploadError)) {
            const auto external = slot.hardwareFrameHandoff->externalImage();
            if (sourceViewOut) {
              *sourceViewOut = external.imageView;
            }
            if (sourceLayoutOut) {
              *sourceLayoutOut = external.imageLayout;
            }
            return external.imageView != VK_NULL_HANDLE;
          }
          qWarning().noquote()
              << QStringLiteral(
                     "[vulkan-compose] hardware frame handoff failed and CPU "
                     "image fallback is disabled: %1")
                     .arg(QString::fromStdString(uploadError));
          return false;
        }
      }
      QImage rgba;
      if (!layer.cacheKey.isEmpty()) {
        rgba = m_preparedImageCache.value(layer.cacheKey);
      }
      if (rgba.isNull()) {
        if (!layer.overlayImage.isNull()) {
          if (layer.overlayImage.width == m_outputSize.width() &&
              layer.overlayImage.height == m_outputSize.height() &&
              m_stagingMapped) {
            if (!writeOverlayImageToStagingTopLeft(layer.overlayImage, stagingOffset)) {
              return false;
            }
            transitionImageLayout(m_commandBuffer, slot.image,
                                  slot.uploaded
                                      ? VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL
                                      : VK_IMAGE_LAYOUT_UNDEFINED,
                                  VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
            VkBufferImageCopy uploadRegion{};
            uploadRegion.bufferOffset = stagingOffset;
            uploadRegion.bufferRowLength = 0;
            uploadRegion.bufferImageHeight = 0;
            uploadRegion.imageSubresource.aspectMask =
                VK_IMAGE_ASPECT_COLOR_BIT;
            uploadRegion.imageSubresource.mipLevel = 0;
            uploadRegion.imageSubresource.baseArrayLayer = 0;
            uploadRegion.imageSubresource.layerCount = 1;
            uploadRegion.imageExtent = {
                static_cast<uint32_t>(m_outputSize.width()),
                static_cast<uint32_t>(m_outputSize.height()), 1};
            vkCmdCopyBufferToImage(m_commandBuffer, m_stagingBuffer, slot.image,
                                   VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1,
                                   &uploadRegion);
            transitionImageLayout(m_commandBuffer, slot.image,
                                  VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                                  VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
            slot.uploaded = true;
            if (sourceViewOut) {
              *sourceViewOut = slot.view;
            }
            if (sourceLayoutOut) {
              *sourceLayoutOut = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
            }
            return true;
          }
          const OverlayImage scaledOverlay =
              scaledOverlayImage(layer.overlayImage, m_outputSize);
          if (!scaledOverlay.isNull() && m_stagingMapped) {
            if (!writeOverlayImageToStagingTopLeft(scaledOverlay, stagingOffset)) {
              return false;
            }
            transitionImageLayout(m_commandBuffer, slot.image,
                                  slot.uploaded
                                      ? VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL
                                      : VK_IMAGE_LAYOUT_UNDEFINED,
                                  VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
            VkBufferImageCopy uploadRegion{};
            uploadRegion.bufferOffset = stagingOffset;
            uploadRegion.bufferRowLength = 0;
            uploadRegion.bufferImageHeight = 0;
            uploadRegion.imageSubresource.aspectMask =
                VK_IMAGE_ASPECT_COLOR_BIT;
            uploadRegion.imageSubresource.mipLevel = 0;
            uploadRegion.imageSubresource.baseArrayLayer = 0;
            uploadRegion.imageSubresource.layerCount = 1;
            uploadRegion.imageExtent = {
                static_cast<uint32_t>(m_outputSize.width()),
                static_cast<uint32_t>(m_outputSize.height()), 1};
            vkCmdCopyBufferToImage(m_commandBuffer, m_stagingBuffer, slot.image,
                                   VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1,
                                   &uploadRegion);
            transitionImageLayout(m_commandBuffer, slot.image,
                                  VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                                  VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
            slot.uploaded = true;
            if (sourceViewOut) {
              *sourceViewOut = slot.view;
            }
            if (sourceLayoutOut) {
              *sourceLayoutOut = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
            }
            return true;
          }
        }
      }
      if (rgba.isNull()) {
        if (!layer.image.isNull()) {
          rgba = layer.image;
        } else if (!layer.frame.isNull() &&
                   (layer.frame.hasHardwareFrame() || layer.frame.hasGpuTexture())) {
          return false;
        } else {
          rgba = frameHandleToCpuImage(layer.frame);
        }
        if (rgba.format() != QImage::Format_RGBA8888) {
          rgba = rgba.convertToFormat(QImage::Format_RGBA8888);
        }
        if (rgba.size() != m_outputSize) {
          rgba = rgba.scaled(m_outputSize, Qt::IgnoreAspectRatio,
                             Qt::SmoothTransformation);
        }
        if (!layer.cacheKey.isEmpty()) {
          m_preparedImageCache.insert(layer.cacheKey, rgba);
        }
      }
      if (rgba.isNull() || !m_stagingMapped) {
        return false;
      }
      if (!writeRgbaImageToStagingTopLeft(rgba, stagingOffset)) {
        return false;
      }
      transitionImageLayout(m_commandBuffer, slot.image,
                            slot.uploaded
                                ? VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL
                                : VK_IMAGE_LAYOUT_UNDEFINED,
                            VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
      VkBufferImageCopy uploadRegion{};
      uploadRegion.bufferOffset = stagingOffset;
      uploadRegion.bufferRowLength = 0;
      uploadRegion.bufferImageHeight = 0;
      uploadRegion.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
      uploadRegion.imageSubresource.mipLevel = 0;
      uploadRegion.imageSubresource.baseArrayLayer = 0;
      uploadRegion.imageSubresource.layerCount = 1;
      uploadRegion.imageExtent = {static_cast<uint32_t>(m_outputSize.width()),
                                  static_cast<uint32_t>(m_outputSize.height()),
                                  1};
      vkCmdCopyBufferToImage(m_commandBuffer, m_stagingBuffer, slot.image,
                             VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1,
                             &uploadRegion);
      transitionImageLayout(m_commandBuffer, slot.image,
                            VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                            VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
      slot.uploaded = true;
      if (sourceViewOut) {
        *sourceViewOut = slot.view;
      }
      if (sourceLayoutOut) {
        *sourceLayoutOut = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
      }
      return true;
    };
    int layerIndex = 0;
    while (layerIndex < layers.size()) {
      const int batchCount =
          qMin(kMaxLayerTextures, layers.size() - layerIndex);
      struct PreparedBatchLayer {
        VkImageView view = VK_NULL_HANDLE;
        VkImageLayout layout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        VkImageView auxiliaryView = VK_NULL_HANDLE;
        VkImageLayout auxiliaryLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
      };
      QVector<PreparedBatchLayer> preparedLayers(batchCount);
      for (int i = 0; i < batchCount; ++i) {
        const LayerInput &layer = layers.at(layerIndex + i);
        if (!layerHasRenderableSource(layer)) {
          continue;
        }
        LayerTextureSlot &slot = m_layerSlots[i];
        const VkDeviceSize stagingOffset = layerStagingSize * i;
        if (!prepareLayerSource(slot, layer, stagingOffset,
                                &preparedLayers[i].view,
                                &preparedLayers[i].layout)) {
          vkEndCommandBuffer(m_commandBuffer);
          return QImage();
        }
        if (layer.differenceMatteEnabled && layer.differenceReferenceFrame.hasHardwareFrame()) {
          if (!slot.referenceFrameHandoff) {
            auto handoff = std::make_shared<jcut::vulkan_detector::VulkanDetectorFrameHandoff>();
            jcut::vulkan_detector::VulkanDeviceContext context;
            context.physicalDevice = m_physicalDevice;
            context.device = m_device;
            context.queue = m_graphicsQueue;
            context.queueFamilyIndex = m_graphicsQueueFamily;
            std::string error;
            if (handoff->initialize(context, &error)) {
              slot.referenceFrameHandoff = handoff;
            } else {
              qWarning().noquote() << QStringLiteral("[vulkan-compose] difference reference handoff initialization failed: %1")
                                         .arg(QString::fromStdString(error));
            }
          }
          if (!slot.referenceFrameHandoff) {
            vkEndCommandBuffer(m_commandBuffer);
            return QImage();
          }
          std::string error;
          if (!slot.referenceFrameHandoff->uploadFrame(layer.differenceReferenceFrame, false, nullptr, &error)) {
            qWarning().noquote() << QStringLiteral("[vulkan-compose] difference reference handoff failed: %1")
                                       .arg(QString::fromStdString(error));
            vkEndCommandBuffer(m_commandBuffer);
            return QImage();
          }
          const auto external = slot.referenceFrameHandoff->externalImage();
          preparedLayers[i].auxiliaryView = external.imageView;
          preparedLayers[i].auxiliaryLayout = external.imageLayout;
        }

        const QByteArray curveBytes =
            layer.gradePayload.curveLutRgba.size() ==
                    static_cast<int>(kCurveLutBytes)
                ? layer.gradePayload.curveLutRgba
                : identityCurveLutBytes();
        if (!m_stagingMapped) {
          vkEndCommandBuffer(m_commandBuffer);
          return QImage();
        }
        const VkDeviceSize curveStagingOffset = stagingOffset + layerImageBytes;
        if (!activeStagingRangeAvailable(
                curveStagingOffset, kCurveLutBytes)) {
          vkEndCommandBuffer(m_commandBuffer);
          return QImage();
        }
        std::memcpy(
            reinterpret_cast<uint8_t *>(m_stagingMapped) + curveStagingOffset,
            curveBytes.constData(), static_cast<size_t>(kCurveLutBytes));
        if (!flushActiveStagingWrite(curveStagingOffset, kCurveLutBytes)) {
          vkEndCommandBuffer(m_commandBuffer);
          return QImage();
        }
        transitionImageLayout(m_commandBuffer, slot.curveLutImage,
                              slot.curveUploaded
                                  ? VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL
                                  : VK_IMAGE_LAYOUT_UNDEFINED,
                              VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
        VkBufferImageCopy curveUploadRegion{};
        curveUploadRegion.bufferOffset = curveStagingOffset;
        curveUploadRegion.bufferRowLength = 0;
        curveUploadRegion.bufferImageHeight = 0;
        curveUploadRegion.imageSubresource.aspectMask =
            VK_IMAGE_ASPECT_COLOR_BIT;
        curveUploadRegion.imageSubresource.mipLevel = 0;
        curveUploadRegion.imageSubresource.baseArrayLayer = 0;
        curveUploadRegion.imageSubresource.layerCount = 1;
        curveUploadRegion.imageExtent = {static_cast<uint32_t>(kCurveLutWidth),
                                         static_cast<uint32_t>(kCurveLutHeight),
                                         1};
        vkCmdCopyBufferToImage(
            m_commandBuffer, m_stagingBuffer, slot.curveLutImage,
            VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &curveUploadRegion);
        transitionImageLayout(m_commandBuffer, slot.curveLutImage,
                              VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                              VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
        slot.curveUploaded = true;

        const QByteArray maskCurveBytes =
            layer.maskGradePayload.curveLutRgba.size() ==
                    static_cast<int>(kCurveLutBytes)
                ? layer.maskGradePayload.curveLutRgba
                : identityCurveLutBytes();
        const VkDeviceSize maskCurveStagingOffset =
            stagingOffset + layerImageBytes + kCurveLutBytes;
        if (!activeStagingRangeAvailable(
                maskCurveStagingOffset, kCurveLutBytes)) {
          vkEndCommandBuffer(m_commandBuffer);
          return QImage();
        }
        std::memcpy(
            reinterpret_cast<uint8_t *>(m_stagingMapped) + maskCurveStagingOffset,
            maskCurveBytes.constData(), static_cast<size_t>(kCurveLutBytes));
        if (!flushActiveStagingWrite(maskCurveStagingOffset, kCurveLutBytes)) {
          vkEndCommandBuffer(m_commandBuffer);
          return QImage();
        }
        transitionImageLayout(m_commandBuffer, slot.maskCurveLutImage,
                              slot.maskCurveUploaded
                                  ? VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL
                                  : VK_IMAGE_LAYOUT_UNDEFINED,
                              VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
        VkBufferImageCopy maskCurveUploadRegion{};
        maskCurveUploadRegion.bufferOffset = maskCurveStagingOffset;
        maskCurveUploadRegion.bufferRowLength = 0;
        maskCurveUploadRegion.bufferImageHeight = 0;
        maskCurveUploadRegion.imageSubresource.aspectMask =
            VK_IMAGE_ASPECT_COLOR_BIT;
        maskCurveUploadRegion.imageSubresource.mipLevel = 0;
        maskCurveUploadRegion.imageSubresource.baseArrayLayer = 0;
        maskCurveUploadRegion.imageSubresource.layerCount = 1;
        maskCurveUploadRegion.imageExtent = {static_cast<uint32_t>(kCurveLutWidth),
                                             static_cast<uint32_t>(kCurveLutHeight),
                                             1};
        vkCmdCopyBufferToImage(
            m_commandBuffer, m_stagingBuffer, slot.maskCurveLutImage,
            VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &maskCurveUploadRegion);
        transitionImageLayout(m_commandBuffer, slot.maskCurveLutImage,
                              VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                              VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
        slot.maskCurveUploaded = true;
        if (layer.differenceMatteEnabled && !layer.differenceReferenceFrame.hasHardwareFrame()) {
          QImage reference = layer.differenceReferenceFrame.hasCpuImage()
              ? layer.differenceReferenceFrame.cpuImage()
              : frameHandleToCpuImage(layer.differenceReferenceFrame);
          if (reference.isNull()) {
            vkEndCommandBuffer(m_commandBuffer);
            return QImage();
          }
          reference = reference.convertToFormat(QImage::Format_RGBA8888);
          if (!ensureMaskRawImage(slot, reference.size())) {
            vkEndCommandBuffer(m_commandBuffer);
            return QImage();
          }
          const VkDeviceSize referenceOffset = stagingOffset + layerImageBytes + (kCurveLutBytes * 2);
          if (!writeRgbaImageToStagingTopLeft(reference, referenceOffset)) {
            vkEndCommandBuffer(m_commandBuffer);
            return QImage();
          }
          transitionImageLayout(m_commandBuffer, slot.maskRawImage,
                                slot.maskRawLayout,
                                VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
          slot.maskRawLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
          VkBufferImageCopy region{};
          region.bufferOffset = referenceOffset;
          region.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
          region.imageSubresource.layerCount = 1;
          region.imageExtent = {static_cast<uint32_t>(reference.width()),
                                static_cast<uint32_t>(reference.height()), 1};
          vkCmdCopyBufferToImage(m_commandBuffer, m_stagingBuffer,
                                 slot.maskRawImage,
                                 VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);
          transitionImageLayout(m_commandBuffer, slot.maskRawImage,
                                VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                                VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
          slot.maskRawLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
          preparedLayers[i].auxiliaryView = slot.maskRawView;
        } else if (layer.maskTextureEnabled && layer.maskBuffer) {
          const jcut::core::ImageBuffer &maskUpload = *layer.maskBuffer;
          if (maskUpload.empty() ||
              maskUpload.format != jcut::core::PixelFormat::Gray8) {
            vkEndCommandBuffer(m_commandBuffer);
            return QImage();
          }
          const QSize maskSize(maskUpload.size.width, maskUpload.size.height);
          if (!ensureMaskRawImage(slot, maskSize, VK_FORMAT_R8_UNORM)) {
            vkEndCommandBuffer(m_commandBuffer);
            return QImage();
          }
          const VkDeviceSize maskStagingOffset =
              stagingOffset + layerImageBytes + (kCurveLutBytes * 2);
          if (!writeImageBufferToStagingTopLeft(maskUpload,
                                                maskStagingOffset,
                                                1)) {
            vkEndCommandBuffer(m_commandBuffer);
            return QImage();
          }
          transitionImageLayout(m_commandBuffer, slot.maskRawImage,
                                slot.maskRawLayout,
                                VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
          slot.maskRawLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
          VkBufferImageCopy maskUploadRegion{};
          maskUploadRegion.bufferOffset = maskStagingOffset;
          maskUploadRegion.bufferRowLength = 0;
          maskUploadRegion.bufferImageHeight = 0;
          maskUploadRegion.imageSubresource.aspectMask =
              VK_IMAGE_ASPECT_COLOR_BIT;
          maskUploadRegion.imageSubresource.mipLevel = 0;
          maskUploadRegion.imageSubresource.baseArrayLayer = 0;
          maskUploadRegion.imageSubresource.layerCount = 1;
          maskUploadRegion.imageExtent = {
              static_cast<uint32_t>(maskSize.width()),
              static_cast<uint32_t>(maskSize.height()), 1};
          vkCmdCopyBufferToImage(m_commandBuffer, m_stagingBuffer,
                                 slot.maskRawImage,
                                 VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1,
                                 &maskUploadRegion);
          transitionImageLayout(m_commandBuffer, slot.maskRawImage,
                                VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                                VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
          slot.maskRawLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
          LayerInput maskLayer = layer;
          maskLayer.maskSourceSize = maskSize;
          const VkDeviceSize correctionStorageOffset =
              stagingOffset + correctionStorageOffsetWithinLayer;
          if (!preprocessLayerMask(
                  slot,
                  maskLayer,
                  correctionStorageOffset,
                  maxCorrectionStorageBytes)) {
            vkEndCommandBuffer(m_commandBuffer);
            return QImage();
          }
        } else if (!slot.maskUploaded) {
          transitionImageLayout(m_commandBuffer, slot.maskImage,
                                slot.maskLayout,
                                VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
          slot.maskLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
          VkClearColorValue whiteMaskClear{};
          whiteMaskClear.float32[0] = 1.0f;
          whiteMaskClear.float32[1] = 1.0f;
          whiteMaskClear.float32[2] = 1.0f;
          whiteMaskClear.float32[3] = 1.0f;
          VkImageSubresourceRange whiteMaskRange{};
          whiteMaskRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
          whiteMaskRange.baseMipLevel = 0;
          whiteMaskRange.levelCount = 1;
          whiteMaskRange.baseArrayLayer = 0;
          whiteMaskRange.layerCount = 1;
          vkCmdClearColorImage(
              m_commandBuffer, slot.maskImage,
              VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &whiteMaskClear, 1,
              &whiteMaskRange);
          transitionImageLayout(m_commandBuffer, slot.maskImage,
                                VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                                VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
          slot.maskLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
          slot.maskUploaded = true;
        }
        updateLayerDescriptorSet(slot, preparedLayers[i].view,
                                 preparedLayers[i].layout,
                                 preparedLayers[i].auxiliaryView,
                                 preparedLayers[i].auxiliaryLayout);
      }

      vkCmdBeginRenderPass(m_commandBuffer, &renderPassBeginInfo,
                           VK_SUBPASS_CONTENTS_INLINE);
      vkCmdBindPipeline(m_commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS,
                        m_effectsPipeline);
      vkCmdSetViewport(m_commandBuffer, 0, 1, &fullViewport);
      vkCmdSetScissor(m_commandBuffer, 0, 1, &fullScissor);
      for (int i = 0; i < batchCount; ++i) {
        const LayerInput &layer = layers.at(layerIndex + i);
        if (!layerHasRenderableSource(layer)) {
          continue;
        }
        LayerTextureSlot &slot = m_layerSlots[i];
        auto drawLayerWithMvp = [&](const float drawMvp[16],
                                    float brightness,
                                    float contrast,
                                    float saturation,
                                    float opacity,
                                    const float shadows[4],
                                    const float midtones[4],
                                    const float highlights[4],
                                    float mode,
                                    const float* effectParams = nullptr) {
          const uint32_t frameUniformOffset = updateFrameUniformForDraw(&layer, effectParams);
          vkCmdBindDescriptorSets(
              m_commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS,
              m_effectsPipelineLayout, 0, 1, &slot.descriptorSet, 1, &frameUniformOffset);
          std::memcpy(push.mvp, drawMvp, sizeof(push.mvp));
          push.brightness = brightness;
          push.contrast = contrast;
          push.saturation = saturation;
          push.opacity = qBound(0.0f, opacity, 1.0f);
          push.shadows[0] = shadows[0];
          push.shadows[1] = shadows[1];
          push.shadows[2] = shadows[2];
          push.shadows[3] = mode;
          push.midtones[0] = midtones[0];
          push.midtones[1] = midtones[1];
          push.midtones[2] = midtones[2];
          push.midtones[3] = midtones[3];
          push.highlights[0] = highlights[0];
          push.highlights[1] = highlights[1];
          push.highlights[2] = highlights[2];
          push.highlights[3] = highlights[3];
          vkCmdPushConstants(m_commandBuffer, m_effectsPipelineLayout,
                             VK_SHADER_STAGE_VERTEX_BIT |
                                 VK_SHADER_STAGE_FRAGMENT_BIT,
                             0, sizeof(Push), &push);
          vkCmdDraw(m_commandBuffer, 4, 1, 0, 0);
        };
        auto drawLayer = [&](float brightness,
                             float contrast,
                             float saturation,
                             float opacity,
                             const float shadows[4],
                             const float midtones[4],
                             const float highlights[4],
                             float mode) {
          drawLayerWithMvp(layer.mvp,
                           brightness,
                           contrast,
                           saturation,
                           opacity,
                           shadows,
                           midtones,
                           highlights,
                           mode);
        };
        const float packedMaskFalloff = static_cast<float>(
            layer.maskFeatherFalloff * 10) + layer.maskFeatherGamma;
        const VulkanDrawEffectState& layerEffects =
            layer.gradePayload.effects;
        const VulkanDrawEffectState& maskEffects =
            layer.maskGradePayload.effects;
        float maskHighlights[4];
        std::copy_n(layerEffects.highlights, 4, maskHighlights);
        maskHighlights[3] = packedMaskFalloff;
        if (layer.maskTextureEnabled && layer.maskDropShadowEnabled &&
            layer.maskDropShadowOpacity > 0.0f) {
          float shadowMvp[16];
          std::copy_n(layer.mvp, 16, shadowMvp);
          shadowMvp[12] += 2.0f * layer.maskDropShadowOffsetX /
                           static_cast<float>(std::max(1, m_outputSize.width()));
          shadowMvp[13] += 2.0f * layer.maskDropShadowOffsetY /
                           static_cast<float>(std::max(1, m_outputSize.height()));
          float neutral[4] = {0.0f, 0.0f, 0.0f, 0.0f};
          float shadowMidtones[4] = {0.0f, 0.0f, 0.0f,
                                     static_cast<float>(
                                         layer.maskDropShadowRadius)};
          drawLayerWithMvp(shadowMvp,
                           0.0f,
                           1.0f,
                           1.0f,
                           layerEffects.opacity * layer.maskDropShadowOpacity,
                           neutral,
                           shadowMidtones,
                           maskHighlights,
                           kVulkanEffectModeMaskShadow);
        }
        if (layer.maskTextureEnabled && layer.maskShowOnly) {
          drawLayer(0.0f,
                    1.0f,
                    1.0f,
                    layer.maskOpacity,
                    layerEffects.shadows,
                    layerEffects.midtones,
                    maskHighlights,
                    kVulkanEffectModeMaskOnly);
          continue;
        }
        if (!layer.effectPlan.generatedDraws.isEmpty()) {
          if (layer.presetScissorEnabled) {
            const VkRect2D presetScissor = scissorFromRect(layer.presetScissorRect, m_outputSize);
            vkCmdSetScissor(m_commandBuffer, 0, 1, &presetScissor);
          }
          for (const VulkanEffectPipelinePlan::DrawPass& drawPass : layer.effectPlan.generatedDraws) {
            float presetMvp[16];
            vulkanMvpForOutputRect(
                drawPass.outputRect,
                m_outputSize,
                drawPass.rotationDegrees,
                presetMvp);
            const float drawMode = layer.maskClipSource
                                       ? kVulkanEffectModeMaskGrade
                                       : drawPass.shaderMode;
            float drawShadows[4];
            float drawMidtones[4];
            float drawHighlights[4];
            std::copy_n(layerEffects.shadows, 4, drawShadows);
            std::copy_n(layerEffects.midtones, 4, drawMidtones);
            std::copy_n(layerEffects.highlights, 4, drawHighlights);
            if (drawMode == kVulkanEffectModeMaskGrade) {
              drawHighlights[3] = packedMaskFalloff;
            }
            if (drawMode >= kVulkanEffectModeSpeakerMaskDilation &&
                drawMode <= kVulkanEffectModeSpeakerMaskDilationRings) {
              std::copy_n(drawPass.palette, 3, drawShadows);
              std::copy_n(drawPass.palette + 3, 3, drawMidtones);
              std::copy_n(drawPass.palette + 6, 3, drawHighlights);
            }
            drawLayerWithMvp(presetMvp,
                             layerEffects.brightness,
                             layerEffects.contrast,
                             layerEffects.saturation,
                             layerEffects.opacity * drawPass.opacityMultiplier *
                                 (drawMode == kVulkanEffectModeMaskGrade && !layer.maskClipSource
                                      ? layer.maskOpacity
                                      : 1.0f),
                             drawShadows,
                             drawMidtones,
                             drawHighlights,
                             drawMode,
                             drawPass.effectParams);
          }
          if (layer.presetScissorEnabled) {
            vkCmdSetScissor(m_commandBuffer, 0, 1, &fullScissor);
          }
        } else {
          const float drawMode = layer.maskClipSource
                                     ? kVulkanEffectModeMaskGrade
                                     : layerEffects.shadows[3];
          drawLayer(layerEffects.brightness,
                    layerEffects.contrast,
                    layerEffects.saturation,
                    layerEffects.opacity,
                    layerEffects.shadows,
                    layerEffects.midtones,
                    drawMode == kVulkanEffectModeMaskGrade
                        ? maskHighlights
                        : layerEffects.highlights,
                    drawMode);
        }
        if (layer.maskTextureEnabled && layer.maskGradeEnabled && !layer.maskForegroundLayerEnabled) {
          float neutral[4] = {0.0f, 0.0f, 0.0f, 0.0f};
          float maskMidtones[4] = {0.0f, 0.0f, 0.0f,
                                   layer.maskGradePayload.curveLutApplied
                                       ? kVulkanMaskGradeUseSelectedCurveLut
                                       : 0.0f};
          float maskGradeHighlights[4] = {0.0f, 0.0f, 0.0f,
                                          packedMaskFalloff};
          drawLayer(maskEffects.brightness,
                    maskEffects.contrast,
                    maskEffects.saturation,
                    layer.maskOpacity,
                    neutral,
                    maskMidtones,
                    maskGradeHighlights,
                    kVulkanEffectModeMaskGrade);
        }
      }
      vkCmdEndRenderPass(m_commandBuffer);
      layerIndex += batchCount;
      if (layerIndex < layers.size()) {
        if (vkEndCommandBuffer(m_commandBuffer) != VK_SUCCESS ||
            !submitAndWait()) {
          return QImage();
        }
        vkResetCommandBuffer(m_commandBuffer, 0);
        VkCommandBufferBeginInfo nextBatchBegin{};
        nextBatchBegin.sType =
            VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
        if (vkBeginCommandBuffer(
                m_commandBuffer,
                &nextBatchBegin) != VK_SUCCESS) {
          return QImage();
        }
      }
    }

    const QRectF outputTargetRect(
        QPointF(0.0, 0.0), QSizeF(m_outputSize));
    auto finishTextDrawBeforeAtlasMutation = [&]() -> bool {
      if (vkEndCommandBuffer(m_commandBuffer) != VK_SUCCESS ||
          !submitAndWait()) {
        return false;
      }
      vkResetCommandBuffer(m_commandBuffer, 0);
      VkCommandBufferBeginInfo nextBegin{};
      nextBegin.sType =
          VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
      return vkBeginCommandBuffer(
                 m_commandBuffer, &nextBegin) == VK_SUCCESS;
    };
    if (!textInputs.transcripts.isEmpty() &&
        (!m_transcriptTextRenderer ||
         !m_transcriptTextRenderer->isReady())) {
      if (failureReason) {
        *failureReason = QStringLiteral(
            "Vulkan export refused to drop %1 subtitle overlay(s): "
            "transcript text renderer is unavailable.")
            .arg(textInputs.transcripts.size());
      }
      return QImage();
    }
    if (m_transcriptTextRenderer &&
        m_transcriptTextRenderer->isReady()) {
      bool textRendererDrawRecorded = false;
      for (const TranscriptTextInput& text :
           std::as_const(textInputs.transcripts)) {
        const bool atlasUploadRequired =
            m_transcriptTextRenderer->transcriptOverlayAtlasNeedsUpload(
                m_outputSize, text.clip, text.layout, text.outputRect,
                text.speakerTitle);
        if (textRendererDrawRecorded && atlasUploadRequired) {
          if (!finishTextDrawBeforeAtlasMutation()) {
            return QImage();
          }
          textRendererDrawRecorded = false;
        }
        if (!m_transcriptTextRenderer->prepareTranscriptOverlayAtlas(
                m_commandBuffer, m_outputSize, text.clip, text.layout,
                text.outputRect, text.speakerTitle)) {
          if (failureReason) {
            *failureReason = QStringLiteral(
                "Vulkan export refused to drop subtitle overlay for clip %1: "
                "%2")
                .arg(text.clip.id,
                     m_transcriptTextRenderer->lastFailureReason().isEmpty()
                         ? QStringLiteral("transcript_prepare_failed")
                         : m_transcriptTextRenderer->lastFailureReason());
          }
          vkEndCommandBuffer(m_commandBuffer);
          return QImage();
        }
        vkCmdBeginRenderPass(
            m_commandBuffer, &renderPassBeginInfo,
            VK_SUBPASS_CONTENTS_INLINE);
        const bool transcriptDrawn =
            m_transcriptTextRenderer->drawTranscriptOverlay(
                m_commandBuffer, m_outputSize, m_outputSize,
                outputTargetRect, text.clip, text.layout,
                text.outputRect, text.speakerTitle);
        vkCmdEndRenderPass(m_commandBuffer);
        if (!transcriptDrawn) {
          if (failureReason) {
            *failureReason = QStringLiteral(
                "Vulkan export refused to drop subtitle overlay for clip %1: "
                "%2")
                .arg(text.clip.id,
                     m_transcriptTextRenderer->lastFailureReason().isEmpty()
                         ? QStringLiteral("transcript_draw_failed")
                         : m_transcriptTextRenderer->lastFailureReason());
          }
          vkEndCommandBuffer(m_commandBuffer);
          return QImage();
        }
        textRendererDrawRecorded = true;
      }
      for (const EvaluatedTitle& title :
           std::as_const(textInputs.title3D)) {
        const bool atlasUploadRequired =
            m_transcriptTextRenderer->titleOverlayAtlasNeedsUpload(
                m_outputSize, title);
        if (textRendererDrawRecorded && atlasUploadRequired) {
          if (!finishTextDrawBeforeAtlasMutation()) {
            return QImage();
          }
          textRendererDrawRecorded = false;
        }
        if (!m_transcriptTextRenderer->prepareTitleOverlayAtlas(
                m_commandBuffer, m_outputSize, title)) {
          continue;
        }
        vkCmdBeginRenderPass(
            m_commandBuffer, &renderPassBeginInfo,
            VK_SUBPASS_CONTENTS_INLINE);
        m_transcriptTextRenderer->drawTitleOverlay3D(
            m_commandBuffer, m_outputSize, m_outputSize,
            outputTargetRect, title);
        vkCmdEndRenderPass(m_commandBuffer);
        textRendererDrawRecorded = true;
      }
    }
    if (textInputs.hasSpeakerLabel && m_speakerTextRenderer &&
        m_speakerTextRenderer->isReady() &&
        m_speakerTextRenderer->prepareSpeakerLabelAtlas(
            m_commandBuffer, m_outputSize,
            textInputs.speakerLabel)) {
      vkCmdBeginRenderPass(
          m_commandBuffer, &renderPassBeginInfo,
          VK_SUBPASS_CONTENTS_INLINE);
      m_speakerTextRenderer->drawSpeakerLabel(
          m_commandBuffer, m_outputSize, m_outputSize,
          outputTargetRect, textInputs.speakerLabel);
      vkCmdEndRenderPass(m_commandBuffer);
    }

    transitionImageLayout(m_commandBuffer, m_colorImage,
                          VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                          VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
    m_colorImageLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    if (gpuPreviewFrame) {
      m_commandBufferOpenForConversion = true;
      const bool previewPublished =
          publishLastFrameForGpuPreview(gpuPreviewFrame, gpuPreviewError);
      m_commandBufferOpenForConversion = false;
      if (previewPublished) {
        transitionImageLayout(m_commandBuffer, m_colorImage,
                              m_colorImageLayout,
                              VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
        m_colorImageLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
      }
    }
    if (readbackToImage) {
      VkBufferImageCopy readbackRegion{};
      readbackRegion.bufferOffset = 0;
      readbackRegion.bufferRowLength = 0;
      readbackRegion.bufferImageHeight = 0;
      readbackRegion.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
      readbackRegion.imageSubresource.mipLevel = 0;
      readbackRegion.imageSubresource.baseArrayLayer = 0;
      readbackRegion.imageSubresource.layerCount = 1;
      readbackRegion.imageExtent = {
          static_cast<uint32_t>(m_outputSize.width()),
          static_cast<uint32_t>(m_outputSize.height()), 1};
      vkCmdCopyImageToBuffer(m_commandBuffer, m_colorImage,
                             VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                             m_stagingBuffer, 1, &readbackRegion);
    }

    if (!readbackToImage) {
      m_commandBufferOpenForConversion = true;
      m_colorImagePrimed = true;
      return QImage();
    }

    if (vkEndCommandBuffer(m_commandBuffer) != VK_SUCCESS) {
      return QImage();
    }

    if (!submitAndWait()) {
      return QImage();
    }

    QImage out;
    if (readbackToImage) {
      if (!m_stagingMapped) {
        return QImage();
      }
      if (m_activeSlotIndex < 0 || m_activeSlotIndex >= m_frameSlots.size() ||
          !invalidateSlotForHostRead(m_frameSlots[m_activeSlotIndex])) {
        return QImage();
      }
      QImage readbackRgba(reinterpret_cast<const uchar *>(m_stagingMapped),
                          m_outputSize.width(), m_outputSize.height(),
                          m_outputSize.width() * 4, QImage::Format_ARGB32);
      out = readbackRgba.copy()
                .convertToFormat(QImage::Format_ARGB32_Premultiplied);
    }
    m_colorImagePrimed = true;
    return out;
  }

  bool finishLastFrameForExternalSampling(OffscreenVulkanFrame *frame,
                                          QString *errorMessage) const {
    if (!frame) {
      return false;
    }
    frame->valid = false;
    if (!m_initialized || m_device == VK_NULL_HANDLE ||
        m_colorImageView == VK_NULL_HANDLE || m_activeSlotIndex < 0 ||
        m_activeSlotIndex >= m_frameSlots.size()) {
      if (errorMessage) {
        *errorMessage =
            QStringLiteral("No rendered Vulkan frame is available.");
      }
      return false;
    }
    auto *self = const_cast<OffscreenVulkanRendererPrivate *>(this);
    if (m_commandBufferOpenForConversion) {
      transitionImageLayout(self->m_commandBuffer, self->m_colorImage,
                            self->m_colorImageLayout,
                            VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
      self->m_colorImageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
      if (vkEndCommandBuffer(self->m_commandBuffer) != VK_SUCCESS) {
        if (errorMessage) {
          *errorMessage =
              QStringLiteral("Failed to finish Vulkan render command buffer.");
        }
        return false;
      }
      if (!self->submitAndWait()) {
        if (errorMessage) {
          *errorMessage =
              QStringLiteral("Failed to submit Vulkan render command buffer.");
        }
        return false;
      }
      self->m_commandBufferOpenForConversion = false;
    } else if (m_colorImageLayout != VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL &&
               m_colorImageLayout != VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL) {
      if (errorMessage) {
        *errorMessage = QStringLiteral(
            "Rendered Vulkan frame is not in a sampleable state.");
      }
      return false;
    }

    frame->physicalDevice = m_physicalDevice;
    frame->device = m_device;
    frame->queue = m_graphicsQueue;
    frame->queueFamilyIndex = m_graphicsQueueFamily;
    frame->image = m_colorImage;
    frame->imageView = m_colorImageView;
    frame->imageMemory = m_colorImageMemory;
    frame->imageLayout = m_colorImageLayout;
    frame->imageFormat = VK_FORMAT_B8G8R8A8_UNORM;
    frame->readySemaphoreFd = -1;
    frame->size = {m_outputSize.width(), m_outputSize.height()};
    frame->queueSupportsCompute = m_graphicsQueueSupportsCompute;
    frame->valid = true;
    return true;
  }

  bool hasPendingGpuFrame() const {
    return m_initialized && m_commandBufferOpenForConversion &&
        m_activeSlotIndex >= 0 &&
        m_activeSlotIndex < m_frameSlots.size();
  }

  bool convertLastFrameToNv12(AVFrame *frame, qint64 *nv12ConvertMs,
                              qint64 *readbackMs) {
    return beginLastFrameToNv12Readback(nv12ConvertMs, readbackMs) &&
           finishLastFrameToNv12Readback(frame, nv12ConvertMs, readbackMs);
  }

  bool beginLastFrameToNv12Copy(VkBuffer targetBuffer,
                                QVector<int> *pendingSlots, qint64 *convertMs,
                                qint64 *transferMs) {
    if (!m_initialized || m_device == VK_NULL_HANDLE ||
        m_commandBuffer == VK_NULL_HANDLE || targetBuffer == VK_NULL_HANDLE ||
        !pendingSlots) {
      return false;
    }
    const qint64 startMs = QDateTime::currentMSecsSinceEpoch();
    VkCommandBufferBeginInfo beginInfo{};
    beginInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    if (!m_commandBufferOpenForConversion) {
      vkResetCommandBuffer(m_commandBuffer, 0);
      if (vkBeginCommandBuffer(m_commandBuffer, &beginInfo) != VK_SUCCESS) {
        return false;
      }
    }
    transitionImageLayout(m_commandBuffer, m_colorImage,
                          VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                          VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    VkViewport yViewport{};
    yViewport.x = 0.0f;
    yViewport.y = 0.0f;
    yViewport.width = static_cast<float>(m_outputSize.width());
    yViewport.height = static_cast<float>(m_outputSize.height());
    yViewport.minDepth = 0.0f;
    yViewport.maxDepth = 1.0f;
    VkRect2D yScissor{};
    yScissor.offset = {0, 0};
    yScissor.extent = {static_cast<uint32_t>(m_outputSize.width()),
                       static_cast<uint32_t>(m_outputSize.height())};
    VkClearValue clearY{};
    clearY.color = {{0.0625f, 0.0f, 0.0f, 1.0f}};
    VkRenderPassBeginInfo yPass{};
    yPass.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
    yPass.renderPass = m_nv12YRenderPass;
    yPass.framebuffer = m_nv12YFramebuffer;
    yPass.renderArea.offset = {0, 0};
    yPass.renderArea.extent = yScissor.extent;
    yPass.clearValueCount = 1;
    yPass.pClearValues = &clearY;
    vkCmdBeginRenderPass(m_commandBuffer, &yPass, VK_SUBPASS_CONTENTS_INLINE);
    vkCmdBindPipeline(m_commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS,
                      m_nv12YPipeline);
    vkCmdSetViewport(m_commandBuffer, 0, 1, &yViewport);
    vkCmdSetScissor(m_commandBuffer, 0, 1, &yScissor);
    const uint32_t frameUniformOffset = 0;
    vkCmdBindDescriptorSets(m_commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS,
                            m_nv12PipelineLayout, 0, 1, &m_descriptorSet, 1,
                            &frameUniformOffset);
    vkCmdDraw(m_commandBuffer, 4, 1, 0, 0);
    vkCmdEndRenderPass(m_commandBuffer);

    VkViewport uvViewport{};
    uvViewport.x = 0.0f;
    uvViewport.y = 0.0f;
    uvViewport.width = static_cast<float>(qMax(1, m_outputSize.width() / 2));
    uvViewport.height = static_cast<float>(qMax(1, m_outputSize.height() / 2));
    uvViewport.minDepth = 0.0f;
    uvViewport.maxDepth = 1.0f;
    VkRect2D uvScissor{};
    uvScissor.offset = {0, 0};
    uvScissor.extent = {
        static_cast<uint32_t>(qMax(1, m_outputSize.width() / 2)),
        static_cast<uint32_t>(qMax(1, m_outputSize.height() / 2))};
    VkClearValue clearUv{};
    clearUv.color = {{0.5f, 0.5f, 0.0f, 1.0f}};
    VkRenderPassBeginInfo uvPass{};
    uvPass.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
    uvPass.renderPass = m_nv12UvRenderPass;
    uvPass.framebuffer = m_nv12UvFramebuffer;
    uvPass.renderArea.offset = {0, 0};
    uvPass.renderArea.extent = uvScissor.extent;
    uvPass.clearValueCount = 1;
    uvPass.pClearValues = &clearUv;
    vkCmdBeginRenderPass(m_commandBuffer, &uvPass, VK_SUBPASS_CONTENTS_INLINE);
    vkCmdBindPipeline(m_commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS,
                      m_nv12UvPipeline);
    vkCmdSetViewport(m_commandBuffer, 0, 1, &uvViewport);
    vkCmdSetScissor(m_commandBuffer, 0, 1, &uvScissor);
    vkCmdBindDescriptorSets(m_commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS,
                            m_nv12PipelineLayout, 0, 1, &m_descriptorSet, 1,
                            &frameUniformOffset);
    vkCmdDraw(m_commandBuffer, 4, 1, 0, 0);
    vkCmdEndRenderPass(m_commandBuffer);

    const VkDeviceSize yPlaneBytes =
        static_cast<VkDeviceSize>(m_outputSize.width()) *
        static_cast<VkDeviceSize>(m_outputSize.height());
    const VkDeviceSize uvPlaneOffset =
        (yPlaneBytes + 255u) & ~VkDeviceSize(255u);
    VkBufferImageCopy yRegion{};
    yRegion.bufferOffset = 0;
    yRegion.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    yRegion.imageSubresource.mipLevel = 0;
    yRegion.imageSubresource.baseArrayLayer = 0;
    yRegion.imageSubresource.layerCount = 1;
    yRegion.imageExtent = {static_cast<uint32_t>(m_outputSize.width()),
                           static_cast<uint32_t>(m_outputSize.height()), 1};
    vkCmdCopyImageToBuffer(m_commandBuffer, m_nv12YImage,
                           VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, targetBuffer,
                           1, &yRegion);

    VkBufferImageCopy uvRegion{};
    uvRegion.bufferOffset = uvPlaneOffset;
    uvRegion.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    uvRegion.imageSubresource.mipLevel = 0;
    uvRegion.imageSubresource.baseArrayLayer = 0;
    uvRegion.imageSubresource.layerCount = 1;
    uvRegion.imageExtent = {
        static_cast<uint32_t>(qMax(1, m_outputSize.width() / 2)),
        static_cast<uint32_t>(qMax(1, m_outputSize.height() / 2)), 1};
    vkCmdCopyImageToBuffer(m_commandBuffer, m_nv12UvImage,
                           VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, targetBuffer,
                           1, &uvRegion);
    transitionImageLayout(m_commandBuffer, m_colorImage,
                          VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                          VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
    if (vkEndCommandBuffer(m_commandBuffer) != VK_SUCCESS) {
      return false;
    }
    // Queue ordering preserves the shared color/NV12 attachment sequence. Do
    // not wait here: the export loop keeps a bounded pending-frame queue and
    // finishLastFrameToNv12*() waits only when the specific slot's output
    // buffer is needed by the encoder.
    if (!submitActiveSlot()) {
      return false;
    }
    m_commandBufferOpenForConversion = false;
    pendingSlots->push_back(m_activeSlotIndex);
    if (convertMs) {
      *convertMs += QDateTime::currentMSecsSinceEpoch() - startMs;
    }
    Q_UNUSED(transferMs)
    return true;
  }

  bool beginLastFrameToNv12Readback(qint64 *convertMs, qint64 *readbackMs) {
    return beginLastFrameToNv12Copy(m_stagingBuffer, &m_pendingNv12SlotIndices,
                                    convertMs, readbackMs);
  }

  bool beginLastFrameToNv12CudaTransfer(qint64 *convertMs, qint64 *transferMs) {
    if (!supportsCudaExternalMemoryInterop() || m_activeSlotIndex < 0 ||
        m_activeSlotIndex >= m_frameSlots.size()) {
      return false;
    }
    FrameSlot &slot = m_frameSlots[m_activeSlotIndex];
    return beginLastFrameToNv12Copy(slot.cudaExportBuffer,
                                    &m_pendingNv12CudaSlotIndices, convertMs,
                                    transferMs);
  }

  bool finishLastFrameToNv12Readback(AVFrame *frame, qint64 *convertMs,
                                     qint64 *readbackMs) {
    if (!frame || frame->format != AV_PIX_FMT_NV12 || frame->width <= 0 ||
        frame->height <= 0 || m_pendingNv12SlotIndices.isEmpty()) {
      return false;
    }
    const qint64 startMs = QDateTime::currentMSecsSinceEpoch();
    const int slotIndex = m_pendingNv12SlotIndices.takeFirst();
    if (slotIndex < 0 || slotIndex >= m_frameSlots.size()) {
      return false;
    }
    if (!waitSlot(slotIndex)) {
      return false;
    }
    FrameSlot &slot = m_frameSlots[slotIndex];
    if (!slot.stagingMapped || !invalidateSlotForHostRead(slot)) {
      return false;
    }
    const uint8_t *bytes =
        reinterpret_cast<const uint8_t *>(slot.stagingMapped);
    const VkDeviceSize yPlaneBytes =
        static_cast<VkDeviceSize>(m_outputSize.width()) *
        static_cast<VkDeviceSize>(m_outputSize.height());
    const VkDeviceSize uvPlaneOffset =
        (yPlaneBytes + 255u) & ~VkDeviceSize(255u);
    for (int y = 0; y < frame->height; ++y) {
      memcpy(frame->data[0] + y * frame->linesize[0],
             bytes + y * m_outputSize.width(), frame->width);
    }
    const int uvWidthBytes = qMax(1, frame->width / 2) * 2;
    const uint8_t *uvMapped = bytes + uvPlaneOffset;
    for (int y = 0; y < qMax(1, frame->height / 2); ++y) {
      memcpy(frame->data[1] + y * frame->linesize[1],
             uvMapped + y * uvWidthBytes, uvWidthBytes);
    }
    if (convertMs) {
      *convertMs += QDateTime::currentMSecsSinceEpoch() - startMs;
    }
    if (readbackMs) {
      *readbackMs += QDateTime::currentMSecsSinceEpoch() - startMs;
    }
    return true;
  }

#if JCUT_HAS_CUDA_DRIVER
  bool ensureCudaExternalMemoryForSlot(FrameSlot &slot, AVFrame *cudaFrame) {
    if (!cudaFrame || cudaFrame->format != AV_PIX_FMT_CUDA ||
        !cudaFrame->hw_frames_ctx || slot.cudaExportMemory == VK_NULL_HANDLE ||
        slot.cudaExportAllocationSize == 0 || !m_vkGetMemoryFdKHR) {
      return false;
    }
    auto *framesCtx =
        reinterpret_cast<AVHWFramesContext *>(cudaFrame->hw_frames_ctx->data);
    if (!framesCtx || !framesCtx->device_ref || !framesCtx->device_ctx ||
        !framesCtx->device_ctx->hwctx) {
      return false;
    }
    auto *cudaDevice =
        reinterpret_cast<AVCUDADeviceContext *>(framesCtx->device_ctx->hwctx);
    CUcontext cudaContext = cudaDevice->cuda_ctx;
    if (!cudaContext) {
      return false;
    }

    AVBufferRef *retiredCudaDeviceRef = nullptr;
    auto releaseRetiredCudaDevice = qScopeGuard(
        [&]() { av_buffer_unref(&retiredCudaDeviceRef); });
    CUcontext previous = nullptr;
    CUresult cuResult = cuInit(0);
    if (cuResult != CUDA_SUCCESS ||
        cuCtxPushCurrent(cudaContext) != CUDA_SUCCESS) {
      return false;
    }

    auto popContext = qScopeGuard([&]() { cuCtxPopCurrent(&previous); });

    if (slot.cudaExternalMemory && slot.cudaImportContext != cudaContext) {
      // The previous encoder's CUDA context owns this opaque import. It can
      // already be in retirement when a new incremental chunk reaches this
      // renderer, so calling the driver destroy entry point from the new
      // context is invalid. Dropping our retained device reference retires the
      // old context and all of its imports together.
      slot.cudaExternalMemory = nullptr;
      slot.cudaExternalDevicePtr = 0;
      slot.cudaImportContext = nullptr;
      retiredCudaDeviceRef = slot.cudaImportDeviceRef;
      slot.cudaImportDeviceRef = nullptr;
    }
    if (slot.cudaExternalMemory && slot.cudaExternalDevicePtr) {
      return true;
    }

    AVBufferRef *cudaImportDeviceRef = av_buffer_ref(framesCtx->device_ref);
    if (!cudaImportDeviceRef) {
      return false;
    }

    VkMemoryGetFdInfoKHR fdInfo{};
    fdInfo.sType = VK_STRUCTURE_TYPE_MEMORY_GET_FD_INFO_KHR;
    fdInfo.memory = slot.cudaExportMemory;
    fdInfo.handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_FD_BIT;
    int memoryFd = -1;
    if (m_vkGetMemoryFdKHR(m_device, &fdInfo, &memoryFd) != VK_SUCCESS ||
        memoryFd < 0) {
      av_buffer_unref(&cudaImportDeviceRef);
      return false;
    }

    CUDA_EXTERNAL_MEMORY_HANDLE_DESC handleDesc{};
    handleDesc.type = CU_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_FD;
    handleDesc.handle.fd = memoryFd;
    handleDesc.size = slot.cudaExportAllocationSize;
    cuResult = cuImportExternalMemory(&slot.cudaExternalMemory, &handleDesc);
    if (cuResult != CUDA_SUCCESS) {
      close(memoryFd);
      slot.cudaExternalMemory = nullptr;
      av_buffer_unref(&cudaImportDeviceRef);
      return false;
    }

    CUDA_EXTERNAL_MEMORY_BUFFER_DESC bufferDesc{};
    bufferDesc.offset = 0;
    bufferDesc.size = slot.cudaExportAllocationSize;
    cuResult = cuExternalMemoryGetMappedBuffer(
        &slot.cudaExternalDevicePtr, slot.cudaExternalMemory, &bufferDesc);
    if (cuResult != CUDA_SUCCESS) {
      // Keep the driver-safe lifetime policy consistent with renderer teardown:
      // a failed map retires by dropping our references, not by synchronously
      // destroying the CUDA external-memory import from this worker thread.
      slot.cudaExternalMemory = nullptr;
      slot.cudaExternalDevicePtr = 0;
      av_buffer_unref(&cudaImportDeviceRef);
      return false;
    }
    slot.cudaImportContext = cudaContext;
    slot.cudaImportDeviceRef = cudaImportDeviceRef;
    return true;
  }
#endif

  bool finishLastFrameToNv12CudaTransfer(AVFrame *cudaFrame, qint64 *convertMs,
                                         qint64 *transferMs) {
#if JCUT_HAS_CUDA_DRIVER
    if (!cudaFrame || cudaFrame->format != AV_PIX_FMT_CUDA ||
        m_pendingNv12CudaSlotIndices.isEmpty()) {
      return false;
    }
    const qint64 startMs = QDateTime::currentMSecsSinceEpoch();
    const int slotIndex = m_pendingNv12CudaSlotIndices.takeFirst();
    if (slotIndex < 0 || slotIndex >= m_frameSlots.size()) {
      return false;
    }
    if (!waitSlot(slotIndex)) {
      return false;
    }
    FrameSlot &slot = m_frameSlots[slotIndex];
    if (!ensureCudaExternalMemoryForSlot(slot, cudaFrame)) {
      return false;
    }

    auto *framesCtx =
        reinterpret_cast<AVHWFramesContext *>(cudaFrame->hw_frames_ctx->data);
    auto *cudaDevice =
        reinterpret_cast<AVCUDADeviceContext *>(framesCtx->device_ctx->hwctx);
    CUcontext cudaContext = cudaDevice->cuda_ctx;
    CUcontext previous = nullptr;
    if (cuCtxPushCurrent(cudaContext) != CUDA_SUCCESS) {
      return false;
    }
    auto popContext = qScopeGuard([&]() { cuCtxPopCurrent(&previous); });

    const int width = qMin(cudaFrame->width, m_outputSize.width());
    const int height = qMin(cudaFrame->height, m_outputSize.height());
    const VkDeviceSize yPlaneBytes =
        static_cast<VkDeviceSize>(m_outputSize.width()) *
        static_cast<VkDeviceSize>(m_outputSize.height());
    const VkDeviceSize uvPlaneOffset =
        (yPlaneBytes + 255u) & ~VkDeviceSize(255u);

    CUDA_MEMCPY2D yCopy{};
    yCopy.srcMemoryType = CU_MEMORYTYPE_DEVICE;
    yCopy.srcDevice = slot.cudaExternalDevicePtr;
    yCopy.srcPitch = static_cast<size_t>(m_outputSize.width());
    yCopy.dstMemoryType = CU_MEMORYTYPE_DEVICE;
    yCopy.dstDevice = reinterpret_cast<CUdeviceptr>(cudaFrame->data[0]);
    yCopy.dstPitch = static_cast<size_t>(cudaFrame->linesize[0]);
    yCopy.WidthInBytes = static_cast<size_t>(width);
    yCopy.Height = static_cast<size_t>(height);
    if (cuMemcpy2DAsync(&yCopy, cudaDevice->stream) != CUDA_SUCCESS) {
      return false;
    }

    CUDA_MEMCPY2D uvCopy{};
    uvCopy.srcMemoryType = CU_MEMORYTYPE_DEVICE;
    uvCopy.srcDevice = slot.cudaExternalDevicePtr + uvPlaneOffset;
    uvCopy.srcPitch = static_cast<size_t>(m_outputSize.width());
    uvCopy.dstMemoryType = CU_MEMORYTYPE_DEVICE;
    uvCopy.dstDevice = reinterpret_cast<CUdeviceptr>(cudaFrame->data[1]);
    uvCopy.dstPitch = static_cast<size_t>(cudaFrame->linesize[1]);
    uvCopy.WidthInBytes = static_cast<size_t>(width);
    uvCopy.Height = static_cast<size_t>(qMax(1, height / 2));
    if (cuMemcpy2DAsync(&uvCopy, cudaDevice->stream) != CUDA_SUCCESS ||
        cuStreamSynchronize(cudaDevice->stream) != CUDA_SUCCESS) {
      return false;
    }
    const qint64 elapsed = QDateTime::currentMSecsSinceEpoch() - startMs;
    if (convertMs) {
      *convertMs += elapsed;
    }
    if (transferMs) {
      *transferMs += elapsed;
    }
    return true;
#else
    Q_UNUSED(cudaFrame)
    Q_UNUSED(convertMs)
    Q_UNUSED(transferMs)
    return false;
#endif
  }

  bool beginLastFrameToYuv420pReadback(qint64 *convertMs, qint64 *readbackMs) {
    if (!m_initialized || m_device == VK_NULL_HANDLE ||
        m_commandBuffer == VK_NULL_HANDLE) {
      return false;
    }
    const qint64 startMs = QDateTime::currentMSecsSinceEpoch();
    VkCommandBufferBeginInfo beginInfo{};
    beginInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    if (!m_commandBufferOpenForConversion) {
      vkResetCommandBuffer(m_commandBuffer, 0);
      if (vkBeginCommandBuffer(m_commandBuffer, &beginInfo) != VK_SUCCESS) {
        return false;
      }
    }

    transitionImageLayout(m_commandBuffer, m_colorImage,
                          VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                          VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    const uint32_t yWidth = static_cast<uint32_t>(m_outputSize.width());
    const uint32_t yHeight = static_cast<uint32_t>(m_outputSize.height());
    const uint32_t chromaWidth =
        static_cast<uint32_t>(qMax(1, m_outputSize.width() / 2));
    const uint32_t chromaHeight =
        static_cast<uint32_t>(qMax(1, m_outputSize.height() / 2));
    const VkImageLayout oldYuvLayout =
        m_yuv420pPlanesPrimed ? VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL
                              : VK_IMAGE_LAYOUT_UNDEFINED;
    transitionImageLayout(m_commandBuffer, m_nv12YImage, oldYuvLayout,
                          VK_IMAGE_LAYOUT_GENERAL);
    transitionImageLayout(m_commandBuffer, m_yuv420pUImage, oldYuvLayout,
                          VK_IMAGE_LAYOUT_GENERAL);
    transitionImageLayout(m_commandBuffer, m_yuv420pVImage, oldYuvLayout,
                          VK_IMAGE_LAYOUT_GENERAL);
    vkCmdBindPipeline(m_commandBuffer, VK_PIPELINE_BIND_POINT_COMPUTE,
                      m_yuv420pComputePipeline);
    vkCmdBindDescriptorSets(m_commandBuffer, VK_PIPELINE_BIND_POINT_COMPUTE,
                            m_yuvComputePipelineLayout, 0, 1,
                            &m_yuvComputeDescriptorSet, 0, nullptr);
    vkCmdDispatch(m_commandBuffer, (yWidth + 15u) / 16u, (yHeight + 15u) / 16u,
                  1);
    transitionImageLayout(m_commandBuffer, m_nv12YImage,
                          VK_IMAGE_LAYOUT_GENERAL,
                          VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
    transitionImageLayout(m_commandBuffer, m_yuv420pUImage,
                          VK_IMAGE_LAYOUT_GENERAL,
                          VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
    transitionImageLayout(m_commandBuffer, m_yuv420pVImage,
                          VK_IMAGE_LAYOUT_GENERAL,
                          VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);

    const VkDeviceSize yPlaneBytes =
        static_cast<VkDeviceSize>(m_outputSize.width()) *
        static_cast<VkDeviceSize>(m_outputSize.height());
    const VkDeviceSize uPlaneBytes = static_cast<VkDeviceSize>(chromaWidth) *
                                     static_cast<VkDeviceSize>(chromaHeight);
    const VkDeviceSize uPlaneOffset =
        (yPlaneBytes + 255u) & ~VkDeviceSize(255u);
    const VkDeviceSize vPlaneOffset =
        (uPlaneOffset + uPlaneBytes + 255u) & ~VkDeviceSize(255u);
    VkBufferImageCopy yRegion{};
    yRegion.bufferOffset = 0;
    yRegion.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    yRegion.imageSubresource.mipLevel = 0;
    yRegion.imageSubresource.baseArrayLayer = 0;
    yRegion.imageSubresource.layerCount = 1;
    yRegion.imageExtent = {yWidth, yHeight, 1};
    vkCmdCopyImageToBuffer(m_commandBuffer, m_nv12YImage,
                           VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                           m_stagingBuffer, 1, &yRegion);

    VkBufferImageCopy uRegion{};
    uRegion.bufferOffset = uPlaneOffset;
    uRegion.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    uRegion.imageSubresource.mipLevel = 0;
    uRegion.imageSubresource.baseArrayLayer = 0;
    uRegion.imageSubresource.layerCount = 1;
    uRegion.imageExtent = {chromaWidth, chromaHeight, 1};
    vkCmdCopyImageToBuffer(m_commandBuffer, m_yuv420pUImage,
                           VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                           m_stagingBuffer, 1, &uRegion);

    VkBufferImageCopy vRegion = uRegion;
    vRegion.bufferOffset = vPlaneOffset;
    vkCmdCopyImageToBuffer(m_commandBuffer, m_yuv420pVImage,
                           VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                           m_stagingBuffer, 1, &vRegion);
    transitionImageLayout(m_commandBuffer, m_colorImage,
                          VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                          VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
    if (vkEndCommandBuffer(m_commandBuffer) != VK_SUCCESS) {
      return false;
    }
    if (!submitActiveSlot()) {
      return false;
    }
    m_commandBufferOpenForConversion = false;
    m_yuv420pPlanesPrimed = true;
    m_pendingYuvSlotIndices.push_back(m_activeSlotIndex);
    if (convertMs) {
      *convertMs += QDateTime::currentMSecsSinceEpoch() - startMs;
    }
    Q_UNUSED(readbackMs)
    return true;
  }

  bool finishLastFrameToYuv420pReadback(AVFrame *frame, qint64 *convertMs,
                                        qint64 *readbackMs) {
    if (!frame || frame->format != AV_PIX_FMT_YUV420P || frame->width <= 0 ||
        frame->height <= 0 || m_pendingYuvSlotIndices.isEmpty()) {
      return false;
    }
    const qint64 startMs = QDateTime::currentMSecsSinceEpoch();
    const int slotIndex = m_pendingYuvSlotIndices.takeFirst();
    if (slotIndex < 0 || slotIndex >= m_frameSlots.size()) {
      return false;
    }
    if (!waitSlot(slotIndex)) {
      return false;
    }
    FrameSlot &slot = m_frameSlots[slotIndex];
    if (!slot.stagingMapped) {
      return false;
    }
    if (!invalidateSlotForHostRead(slot)) {
      return false;
    }
    const uint8_t *bytes =
        reinterpret_cast<const uint8_t *>(slot.stagingMapped);
    const int chromaWidth = qMax(1, m_outputSize.width() / 2);
    const int chromaHeight = qMax(1, m_outputSize.height() / 2);
    const VkDeviceSize yPlaneBytes =
        static_cast<VkDeviceSize>(m_outputSize.width()) *
        static_cast<VkDeviceSize>(m_outputSize.height());
    const VkDeviceSize uPlaneBytes = static_cast<VkDeviceSize>(chromaWidth) *
                                     static_cast<VkDeviceSize>(chromaHeight);
    const VkDeviceSize uPlaneOffset =
        (yPlaneBytes + 255u) & ~VkDeviceSize(255u);
    const VkDeviceSize vPlaneOffset =
        (uPlaneOffset + uPlaneBytes + 255u) & ~VkDeviceSize(255u);
    for (int y = 0; y < frame->height; ++y) {
      memcpy(frame->data[0] + y * frame->linesize[0],
             bytes + y * m_outputSize.width(), frame->width);
    }
    const int frameChromaWidth = qMax(1, frame->width / 2);
    const int frameChromaHeight = qMax(1, frame->height / 2);
    for (int y = 0; y < frameChromaHeight; ++y) {
      memcpy(frame->data[1] + y * frame->linesize[1],
             bytes + uPlaneOffset + y * chromaWidth, frameChromaWidth);
      memcpy(frame->data[2] + y * frame->linesize[2],
             bytes + vPlaneOffset + y * chromaWidth, frameChromaWidth);
    }
    if (convertMs) {
      *convertMs += QDateTime::currentMSecsSinceEpoch() - startMs;
    }
    if (readbackMs) {
      *readbackMs += QDateTime::currentMSecsSinceEpoch() - startMs;
    }
    return true;
  }

  bool convertLastFrameToYuv420p(AVFrame *frame, qint64 *convertMs,
                                 qint64 *readbackMs) {
    return beginLastFrameToYuv420pReadback(convertMs, readbackMs) &&
           finishLastFrameToYuv420pReadback(frame, convertMs, readbackMs);
  }

  bool copyLastFrameToBgra(AVFrame *frame, qint64 *readbackMs) {
    if (!frame || frame->width <= 0 || frame->height <= 0 || !m_initialized ||
        m_device == VK_NULL_HANDLE || m_commandBuffer == VK_NULL_HANDLE) {
      return false;
    }
    const qint64 startMs = QDateTime::currentMSecsSinceEpoch();
    VkCommandBufferBeginInfo beginInfo{};
    beginInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    if (!m_commandBufferOpenForConversion) {
      vkResetCommandBuffer(m_commandBuffer, 0);
      if (vkBeginCommandBuffer(m_commandBuffer, &beginInfo) != VK_SUCCESS) {
        return false;
      }
    }
    VkBufferImageCopy region{};
    region.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    region.imageSubresource.mipLevel = 0;
    region.imageSubresource.baseArrayLayer = 0;
    region.imageSubresource.layerCount = 1;
    region.imageExtent = {static_cast<uint32_t>(m_outputSize.width()),
                          static_cast<uint32_t>(m_outputSize.height()), 1};
    vkCmdCopyImageToBuffer(m_commandBuffer, m_colorImage,
                           VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                           m_stagingBuffer, 1, &region);
    if (vkEndCommandBuffer(m_commandBuffer) != VK_SUCCESS) {
      return false;
    }
    if (!submitAndWait()) {
      return false;
    }
    m_commandBufferOpenForConversion = false;

    if (!m_stagingMapped) {
      return false;
    }
    if (m_activeSlotIndex < 0 || m_activeSlotIndex >= m_frameSlots.size() ||
        !invalidateSlotForHostRead(m_frameSlots[m_activeSlotIndex])) {
      return false;
    }
    const int width = qMin(frame->width, m_outputSize.width());
    const int height = qMin(frame->height, m_outputSize.height());
    const int srcStride = m_outputSize.width() * 4;
    for (int y = 0; y < height; ++y) {
      memcpy(frame->data[0] + y * frame->linesize[0],
             reinterpret_cast<uint8_t *>(m_stagingMapped) + y * srcStride,
             static_cast<size_t>(width) * 4);
    }
    if (readbackMs) {
      *readbackMs += QDateTime::currentMSecsSinceEpoch() - startMs;
    }
    return true;
  }

  bool supportsCudaExternalMemoryInterop() const {
    return m_externalMemoryFdSupported &&
           m_vkGetMemoryFdKHR != nullptr && m_cudaExportBuffersReady;
  }

  QString cudaExternalMemoryStatus() const {
    return m_cudaExternalMemoryStatus;
  }

  QVector<TranscriptTextInput> buildTranscriptTextInputs(
      const QSize &imageSize, const RenderRequest &request,
      const RenderFrameClock &clock,
      const QVector<TimelineClip> &orderedClips,
      QStringList *subtitleFailures = nullptr) {
    QVector<TranscriptTextInput> inputs;
    for (const TimelineClip &clip : orderedClips) {
      if (!clip.transcriptOverlay.enabled ||
          (clip.mediaType != ClipMediaType::Audio && !clip.hasAudio) ||
          clock.timelineSample < clipTimelineStartSamples(clip) ||
          clock.timelineSample >= clipTimelineEndSamples(clip)) {
        continue;
      }
      const QString transcriptPath =
          renderTranscriptPath(clip);
      if (transcriptPath.trimmed().isEmpty()) {
        if (subtitleFailures) {
          subtitleFailures->push_back(QStringLiteral(
              "clip %1 has transcript overlay enabled but no active transcript path")
              .arg(clip.id));
        }
        continue;
      }
      QVector<TranscriptSection> sections = m_transcriptCache.value(transcriptPath);
      if (sections.isEmpty()) {
        sections = loadTranscriptSections(transcriptPath);
        m_transcriptCache.insert(transcriptPath, sections);
      }
      if (sections.isEmpty()) {
        if (subtitleFailures) {
          subtitleFailures->push_back(QStringLiteral(
              "clip %1 has transcript overlay enabled but transcript %2 has no readable sections")
              .arg(clip.id, transcriptPath));
        }
        continue;
      }
      const ClipFrameMapping mapping =
          clipFrameMappingForClock(
              clip, request.clips, clock,
              request.renderSyncMarkers);
      const TranscriptOverlayLayout layout =
          transcriptOverlayLayoutAtSourceFrame(
              clip,
              sections,
              mapping.transcriptFrame,
              TranscriptOverlayTiming{request.transcriptPrependMs,
                                      request.transcriptPostpendMs,
                                      request.transcriptOffsetMs});
      if (layout.lines.isEmpty()) {
        continue;
      }
      const QRectF outputRect = transcriptOverlayRectInOutputSpace(
          clip, imageSize, transcriptPath, sections, mapping.transcriptFrame);
      if (outputRect.isEmpty()) {
        if (subtitleFailures) {
          subtitleFailures->push_back(QStringLiteral(
              "clip %1 produced an empty subtitle output rectangle at transcript frame %2")
              .arg(clip.id)
              .arg(mapping.transcriptFrame));
        }
        continue;
      }
      const QString speakerTitle = clip.transcriptOverlay.showSpeakerTitle
          ? transcriptSpeakerTitleForSourceFrame(
                transcriptPath,
                sections,
                mapping.transcriptFrame,
                TranscriptOverlayTiming{request.transcriptPrependMs,
                                        request.transcriptPostpendMs,
                                        request.transcriptOffsetMs}).trimmed()
          : QString();
      inputs.push_back(TranscriptTextInput{clip, layout, outputRect, speakerTitle});
    }
    return inputs;
  }

  bool buildSpeakerLabelSpec(
      const RenderRequest &request,
      const RenderFrameClock &clock,
      const QVector<TimelineClip> &orderedClips,
      SpeakerLabelOverlaySpec *outSpec) {
    if (outSpec) {
      *outSpec = SpeakerLabelOverlaySpec{};
    }
    if (!request.showCurrentSpeakerName && !request.showCurrentSpeakerOrganization) {
      return false;
    }
    SpeakerLabelOverlaySpec spec;
    spec.showName = request.showCurrentSpeakerName;
    spec.showOrganization = request.showCurrentSpeakerOrganization;
    spec.nameTextScale = qBound<qreal>(0.25, request.currentSpeakerNameTextScale, 3.0);
    spec.organizationTextScale =
        qBound<qreal>(0.25, request.currentSpeakerOrganizationTextScale, 3.0);
    spec.nameVerticalPosition =
        qBound<qreal>(0.0, request.currentSpeakerNameVerticalPosition, 1.0);
    spec.organizationVerticalPosition =
        qBound<qreal>(0.0, request.currentSpeakerOrganizationVerticalPosition, 1.0);
    spec.nameColor = request.currentSpeakerNameColor;
    spec.organizationColor = request.currentSpeakerOrganizationColor;
    spec.backgroundColor = request.currentSpeakerBackgroundColor;
    spec.borderColor = request.currentSpeakerBorderColor;
    spec.backgroundCornerRadius =
        qBound<qreal>(0.0, request.currentSpeakerBackgroundCornerRadius, 128.0);
    spec.borderWidth = qBound<qreal>(0.0, request.currentSpeakerBorderWidth, 16.0);
    spec.showShadow = request.currentSpeakerShadowEnabled;
    spec.shadowColor = request.currentSpeakerShadowColor;

    for (const TimelineClip &clip : orderedClips) {
      if (clip.speakerTitleEngineActive) {
        continue;
      }
      const int64_t clipStartSample = clipTimelineStartSamples(clip);
      const int64_t clipEndSample = clipTimelineEndSamples(clip);
      if (clip.filePath.trimmed().isEmpty() ||
          clock.timelineSample < clipStartSample ||
          clock.timelineSample >= clipEndSample ||
          (!clip.hasAudio && clip.mediaType != ClipMediaType::Audio)) {
        continue;
      }
      const QString transcriptPath =
          renderTranscriptPath(clip);
      if (transcriptPath.trimmed().isEmpty()) {
        continue;
      }
      const ClipFrameMapping mapping =
          clipFrameMappingForClock(clip, clock, request.renderSyncMarkers);
      QVector<TranscriptSection> sections = m_transcriptCache.value(transcriptPath);
      if (sections.isEmpty()) {
        sections = loadTranscriptSections(transcriptPath);
        m_transcriptCache.insert(transcriptPath, sections);
      }
      const QString speakerId =
          transcriptOverlaySpeakerAtSourceFrame(
              sections,
              mapping.transcriptFrame,
              nullptr,
              TranscriptOverlayTiming{request.transcriptPrependMs,
                                      request.transcriptPostpendMs,
                                      request.transcriptOffsetMs}).trimmed();
      if (speakerId.isEmpty()) {
        continue;
      }

      QJsonDocument document;
      SpeakerProfile profile;
      profile.speakerId = speakerId;
      profile.name = speakerId;
      if (loadTranscriptJsonCached(transcriptPath, &document) && document.isObject()) {
        const QJsonObject profiles = document.object().value(QStringLiteral("speaker_profiles")).toObject();
        profile = speakerProfileFromJson(speakerId, profiles.value(speakerId).toObject());
        if (profile.speakerId.isEmpty()) {
          profile.speakerId = speakerId;
        }
        if (profile.name.trimmed().isEmpty()) {
          profile.name = speakerId;
        }
      }
      spec.name = profile.name.trimmed();
      spec.organization = profile.organization.trimmed();
      if (outSpec) {
        *outSpec = spec;
      }
      return true;
    }
    return false;
  }

  VulkanTextLayoutDebug speakerLabelLayoutDebug(
      const QSize &outputSize,
      const SpeakerLabelOverlaySpec &spec) const {
    if (!m_speakerTextRenderer || !m_speakerTextRenderer->isReady()) {
      return {};
    }
    return m_speakerTextRenderer->buildSpeakerLabelLayoutForTesting(outputSize, spec);
  }

private:
  QSize m_outputSize;
  bool m_initialized = false;

  VkInstance m_instance = VK_NULL_HANDLE;
  VkPhysicalDevice m_physicalDevice = VK_NULL_HANDLE;
  VkDeviceSize m_nonCoherentAtomSize = 1;
  VkDeviceSize m_storageBufferOffsetAlignment = 16;
  VkDevice m_device = VK_NULL_HANDLE;
  uint32_t m_graphicsQueueFamily = UINT32_MAX;
  VkQueue m_graphicsQueue = VK_NULL_HANDLE;
  bool m_externalMemoryFdSupported = false;
  bool m_externalSemaphoreFdSupported = false;
  bool m_graphicsQueueSupportsCompute = false;
  PFN_vkGetMemoryFdKHR m_vkGetMemoryFdKHR = nullptr;
  PFN_vkGetSemaphoreFdKHR m_vkGetSemaphoreFdKHR = nullptr;
  QString m_cudaExternalMemoryStatus;

  VkCommandPool m_commandPool = VK_NULL_HANDLE;
  VkCommandBuffer m_commandBuffer = VK_NULL_HANDLE;
  VkFence m_submitFence = VK_NULL_HANDLE;
  QVector<FrameSlot> m_frameSlots;
  int m_activeSlotIndex = -1;
  QVector<int> m_pendingNv12SlotIndices;
  QVector<int> m_pendingNv12CudaSlotIndices;
  QVector<int> m_pendingYuvSlotIndices;
  bool m_cudaExportBuffersReady = false;
  QVector<PreviewSlot> m_previewSlots;
  int m_lastPreviewSlotIndex = -1;
  VkSemaphore m_pendingPreviewWait = VK_NULL_HANDLE;
  VkSemaphore m_pendingPreviewSignal = VK_NULL_HANDLE;

  VkImage m_colorImage = VK_NULL_HANDLE;
  VkDeviceMemory m_colorImageMemory = VK_NULL_HANDLE;
  VkImageView m_colorImageView = VK_NULL_HANDLE;
  VkSampler m_sampler = VK_NULL_HANDLE;

  VkDescriptorSetLayout m_descriptorSetLayout = VK_NULL_HANDLE;
  VkDescriptorPool m_descriptorPool = VK_NULL_HANDLE;
  VkDescriptorSet m_descriptorSet = VK_NULL_HANDLE;
  VkBuffer m_frameUniformBuffer = VK_NULL_HANDLE;
  VkDeviceMemory m_frameUniformMemory = VK_NULL_HANDLE;
  void *m_frameUniformMapped = nullptr;
  VkDeviceSize m_frameUniformStride = 0;
  int m_frameUniformRingIndex = 0;
  VkDescriptorSetLayout m_yuvComputeDescriptorSetLayout = VK_NULL_HANDLE;
  VkDescriptorPool m_yuvComputeDescriptorPool = VK_NULL_HANDLE;
  VkDescriptorSet m_yuvComputeDescriptorSet = VK_NULL_HANDLE;

  VkRenderPass m_renderPass = VK_NULL_HANDLE;

  VkFramebuffer m_framebuffer = VK_NULL_HANDLE;
  QVector<LayerTextureSlot> m_layerSlots;
  QHash<QString, QImage> m_preparedImageCache;
  OverlayImage m_cachedPlacementGuideOverlay;
  QSize m_cachedPlacementGuideOverlaySize;
  bool m_cachedPlacementGuideInstagramSafeArea = false;
  bool m_cachedPlacementGuideAlignmentGrid = false;
  QHash<QString, QVector<TranscriptSection>> m_transcriptCache;
  std::unique_ptr<VulkanTextRenderer> m_transcriptTextRenderer;
  std::unique_ptr<VulkanTextRenderer> m_speakerTextRenderer;
  VkBuffer m_stagingBuffer = VK_NULL_HANDLE;
  VkDeviceMemory m_stagingMemory = VK_NULL_HANDLE;
  void *m_stagingMapped = nullptr;
  VkPipelineLayout m_effectsPipelineLayout = VK_NULL_HANDLE;
  VkPipelineLayout m_maskPipelineLayout = VK_NULL_HANDLE;
  VkPipelineLayout m_nv12PipelineLayout = VK_NULL_HANDLE;
  VkPipelineLayout m_yuvComputePipelineLayout = VK_NULL_HANDLE;
  VkShaderModule m_effectsVertModule = VK_NULL_HANDLE;
  VkShaderModule m_effectsFragModule = VK_NULL_HANDLE;
  VkShaderModule m_maskVertModule = VK_NULL_HANDLE;
  VkShaderModule m_maskFragModule = VK_NULL_HANDLE;
  VkShaderModule m_nv12VertModule = VK_NULL_HANDLE;
  VkShaderModule m_nv12YFragModule = VK_NULL_HANDLE;
  VkShaderModule m_nv12UvFragModule = VK_NULL_HANDLE;
  VkShaderModule m_yuv420pUFragModule = VK_NULL_HANDLE;
  VkShaderModule m_yuv420pVFragModule = VK_NULL_HANDLE;
  VkShaderModule m_yuv420pComputeModule = VK_NULL_HANDLE;
  VkPipeline m_effectsPipeline = VK_NULL_HANDLE;
  VkPipeline m_maskPipeline = VK_NULL_HANDLE;
  VkPipeline m_nv12YPipeline = VK_NULL_HANDLE;
  VkPipeline m_nv12UvPipeline = VK_NULL_HANDLE;
  VkPipeline m_yuv420pUPipeline = VK_NULL_HANDLE;
  VkPipeline m_yuv420pVPipeline = VK_NULL_HANDLE;
  VkPipeline m_yuv420pComputePipeline = VK_NULL_HANDLE;
  VulkanMaskPreprocessor m_maskPreprocessor;
  VkImage m_nv12YImage = VK_NULL_HANDLE;
  VkDeviceMemory m_nv12YImageMemory = VK_NULL_HANDLE;
  VkImageView m_nv12YImageView = VK_NULL_HANDLE;
  VkImage m_nv12UvImage = VK_NULL_HANDLE;
  VkDeviceMemory m_nv12UvImageMemory = VK_NULL_HANDLE;
  VkImageView m_nv12UvImageView = VK_NULL_HANDLE;
  VkImage m_yuv420pUImage = VK_NULL_HANDLE;
  VkDeviceMemory m_yuv420pUImageMemory = VK_NULL_HANDLE;
  VkImageView m_yuv420pUImageView = VK_NULL_HANDLE;
  VkImage m_yuv420pVImage = VK_NULL_HANDLE;
  VkDeviceMemory m_yuv420pVImageMemory = VK_NULL_HANDLE;
  VkImageView m_yuv420pVImageView = VK_NULL_HANDLE;
  VkRenderPass m_nv12YRenderPass = VK_NULL_HANDLE;
  VkRenderPass m_nv12UvRenderPass = VK_NULL_HANDLE;
  VkFramebuffer m_nv12YFramebuffer = VK_NULL_HANDLE;
  VkFramebuffer m_nv12UvFramebuffer = VK_NULL_HANDLE;
  VkFramebuffer m_yuv420pUFramebuffer = VK_NULL_HANDLE;
  VkFramebuffer m_yuv420pVFramebuffer = VK_NULL_HANDLE;
  AVFrame *m_nv12ScratchFrame = nullptr;
  bool m_colorImagePrimed = false;
  VkImageLayout m_colorImageLayout = VK_IMAGE_LAYOUT_UNDEFINED;
  bool m_yuv420pPlanesPrimed = false;
  bool m_commandBufferOpenForConversion = false;
  std::uint64_t m_producerSessionId = 0;
};

OffscreenVulkanRenderer::OffscreenVulkanRenderer()
    : d(std::make_unique<OffscreenVulkanRendererPrivate>()) {}

OffscreenVulkanRenderer::~OffscreenVulkanRenderer() = default;

bool OffscreenVulkanRenderer::initialize(const QSize &outputSize,
                                         QString *errorMessage) {
  return d->initialize(outputSize, errorMessage);
}

QImage OffscreenVulkanRenderer::renderFrame(
    const OffscreenRenderContext &context) {
  const RenderRequest &request = context.request;
  const qreal timelineFrame = context.timelineFrame;
  const qreal generatedEffectClockTimelineFrame =
      context.generatedEffectClockTimelineFrame >= 0.0
          ? context.generatedEffectClockTimelineFrame
          : timelineFrame;
  const qreal transformClockTimelineFrame = timelineFrame;
  QHash<QString, editor::DecoderContext *> &decoders = context.decoders;
  editor::AsyncDecoder *asyncDecoder = context.asyncDecoder;
  QHash<RenderAsyncFrameKey, editor::FrameHandle> *asyncFrameCache =
      context.asyncFrameCache;
  const QVector<TimelineClip> &orderedClips = context.orderedClips;
  QHash<QString, RenderClipStageStats> *clipStageStats =
      context.clipStageStats;
  qint64 *decodeMs = context.decodeMs;
  qint64 *textureMs = context.textureMs;
  qint64 *compositeMs = context.compositeMs;
  qint64 *readbackMs = context.readbackMs;
  qint64 *layerPlanMs = context.layerPlanMs;
  qint64 *textPrepMs = context.textPrepMs;
  qint64 *guideOverlayMs = context.guideOverlayMs;
  qint64 *gpuCompositeMs = context.gpuCompositeMs;
  QJsonArray *skippedClips = context.skippedClips;
  QJsonObject *skippedReasonCounts = context.skippedReasonCounts;
  QJsonObject *exportFaceTransformDiagnostics =
      context.exportFaceTransformDiagnostics;
  auto recordRenderFrameStageMetric =
      [&](const QString& id, const QString& label, qint64 elapsedMs) {
    if (!clipStageStats || elapsedMs <= 0) {
      return;
    }
    RenderClipStageStats& stats = (*clipStageStats)[id];
    if (stats.id.isEmpty()) {
      stats.id = id;
      stats.label = label;
    }
    stats.frames += 1;
    stats.compositeMs += elapsedMs;
  };
  QStringList unresolvedVisualLayers;
  auto recordUnresolvedLayer =
      [&](const TimelineClip& clip, const QString& reason) {
    unresolvedVisualLayers.push_back(
        QStringLiteral("%1: %2")
            .arg(clip.id.isEmpty() ? clip.label : clip.id, reason));
    if (skippedClips) {
      skippedClips->push_back(QJsonObject{
          {QStringLiteral("clip_id"), clip.id},
          {QStringLiteral("clip_label"), clip.label},
          {QStringLiteral("reason"), reason},
      });
    }
    if (skippedReasonCounts) {
      skippedReasonCounts->insert(
          reason,
          skippedReasonCounts->value(reason).toInt() + 1);
    }
  };

  if (decodeMs) {
    *decodeMs = 0;
  }
  if (textureMs) {
    *textureMs = 0;
  }
  if (compositeMs) {
    *compositeMs = 0;
  }
  if (readbackMs) {
    *readbackMs = 0;
  }
  if (layerPlanMs) {
    *layerPlanMs = 0;
  }
  if (textPrepMs) {
    *textPrepMs = 0;
  }
  if (guideOverlayMs) {
    *guideOverlayMs = 0;
  }
  if (gpuCompositeMs) {
    *gpuCompositeMs = 0;
  }
  qint64 renderFrameDecodeWaitMs = 0;
  qint64 renderFrameMaskResolveMs = 0;
  QVector<OffscreenVulkanRendererPrivate::LayerInput> layers;
  layers.reserve((orderedClips.size() * 2) + 1);
  QVector<OffscreenVulkanRendererPrivate::LayerInput> foregroundMaskLayers;
  bool hasTranscriptCandidate = false;
  const QVector<TimelineClip> transcriptOverlayClips =
      sortedTranscriptOverlayClips(request.clips, request.tracks);
  for (const TimelineClip &clip : transcriptOverlayClips) {
    if (timelineFrame >= clip.startFrame &&
        timelineFrame < clip.startFrame + clip.durationFrames &&
        (clip.mediaType == ClipMediaType::Audio || clip.hasAudio) &&
        clip.transcriptOverlay.enabled) {
      hasTranscriptCandidate = true;
      break;
    }
  }
  int visualClipCandidates = 0;
  int visualLayersResolved = 0;
  int decodePathMissingCount = 0;
  int decodeNullCount = 0;
  int decodeConvertFailCount = 0;
  const RenderFrameClock frameClock =
      renderFrameClockForTimelinePosition(timelineFrame);
  QHash<QString, QHash<int64_t, editor::FrameHandle>> decodedFramesByTimingOwner;
  auto decodeFrameForTimingOwner =
      [&](const TimelineClip& timingOwner,
          const QString& decodePath,
          int64_t sourceFrame) -> editor::FrameHandle {
    const QString ownerIdentity = timingOwner.id.trimmed().isEmpty()
        ? QStringLiteral("path:") + decodePath
        : QStringLiteral("id:") + timingOwner.id.trimmed() +
              QStringLiteral("\x1fpath:") + decodePath;
    QHash<int64_t, editor::FrameHandle>& ownerFrames =
        decodedFramesByTimingOwner[ownerIdentity];
    const auto cached = ownerFrames.constFind(sourceFrame);
    if (cached != ownerFrames.cend()) {
      return cached.value();
    }

    const qint64 decodeStart = QDateTime::currentMSecsSinceEpoch();
    const editor::FrameHandle decoded = decodeRenderFrame(
        decodePath,
        sourceFrame,
        decoders,
        asyncDecoder,
        asyncFrameCache,
        context.forceSoftwareDecode,
        context.preferHardwareFrames);
    const qint64 elapsed = QDateTime::currentMSecsSinceEpoch() - decodeStart;
    renderFrameDecodeWaitMs += elapsed;
    if (decodeMs) {
      *decodeMs += elapsed;
    }
    // Cache failures as well as successes for this render pass. A parent and
    // each virtual child must observe the same decode result for a source
    // frame instead of independently retrying or selecting different frames.
    ownerFrames.insert(sourceFrame, decoded);
    return decoded;
  };
  QRectF transcriptLayerBounds;
  OffscreenVulkanRendererPrivate::VulkanTextInputs textInputs;
  const qint64 layerBuildStartMs = QDateTime::currentMSecsSinceEpoch();
  for (const TimelineClip &clip : orderedClips) {
    const TimelineClip &timingSource =
        resolvedClipTimingSource(clip, request.clips);
    const TimelineClip &mediaOwner = timingSource;
    const TimelineClip &timingOwner = timingSource;
    const TimelineClip &effectsOwner = clip;
    const TimelineClip &matteOwner = clip;
    const bool titleClip = clip.mediaType == ClipMediaType::Title;
    // An orphaned or malformed virtual matte must never fall back to its
    // serialized media-path cache and decode as an independent full layer.
    // Normalization removes these clips, but export remains fail-closed when
    // handed an unnormalized request from an older or external caller.
    if (clip.clipRole == ClipRole::MaskMatte &&
        (timingSource.id.trimmed() != clip.linkedSourceClipId.trimmed() ||
         timingSource.clipRole != ClipRole::Media ||
         timingSource.mediaType != ClipMediaType::Video ||
         timingSource.filePath.trimmed().isEmpty())) {
      continue;
    }
    const TimelineClip &activeRangeClip = titleClip ? clip : timingSource;
    if (timelineFrame < activeRangeClip.startFrame ||
        timelineFrame >= activeRangeClip.startFrame + activeRangeClip.durationFrames) {
      continue;
    }
    const bool clipVisible = clipVisualPlaybackEnabled(clip, request.tracks);
    if (!clipVisible) {
      continue;
    }
    const TimelineClip visualEffectsClip =
        titleClip ? clip : clipWithResolvedTimingOwner(effectsOwner, request.clips);
    EffectiveVisualEffects effects =
        request.bypassGrading
            ? EffectiveVisualEffects{}
            : evaluateEffectiveVisualEffectsAtPosition(
                  visualEffectsClip, request.tracks,
                  static_cast<qreal>(timelineFrame),
                  request.renderSyncMarkers,
                  request.playbackTiming);
    if (!request.correctionsEnabled) {
      effects.correctionPolygons.clear();
    }
    const TimelineClip::GradingKeyframe &grade = effects.grading;
    if (grade.opacity <= 0.001) {
      continue;
    }
    if (titleClip) {
      if (clip.titleKeyframes.isEmpty()) {
        continue;
      }
      const EvaluatedTitle title = prepareRenderableTitleForVulkanText(
          clip,
          static_cast<qreal>(timelineFrame),
          request.playbackTiming,
          static_cast<qreal>(grade.opacity),
          request.outputSize);
      if (!title.valid) {
        continue;
      }
      textInputs.title3D.push_back(title);
      continue;
    }
    ++visualClipCandidates;
    const QString decodePath = playbackMediaPathForClip(mediaOwner);
    if (decodePath.isEmpty()) {
      ++decodePathMissingCount;
      recordUnresolvedLayer(clip, QStringLiteral("decode_path_missing"));
      continue;
    }
    const ClipFrameMapping frameMapping =
        clipFrameMappingForClock(
            clip, request.clips, frameClock, request.renderSyncMarkers);
    const int64_t localFrame = frameMapping.sourceFrame;
    const editor::FrameHandle frame =
        decodeFrameForTimingOwner(timingOwner, decodePath, localFrame);
    if (frame.isNull()) {
      ++decodeNullCount;
      recordUnresolvedLayer(clip, QStringLiteral("decode_frame_unavailable"));
      continue;
    }
    OffscreenVulkanRendererPrivate::LayerInput layer;
    layer.clipId = clip.id;
    layer.mediaOwnerClipId = mediaOwner.id;
    layer.timingOwnerClipId = timingOwner.id;
    layer.effectsOwnerClipId = effectsOwner.id;
    layer.matteOwnerClipId = matteOwner.id;
    layer.frame = frame;
    layer.frameSize = frame.size();
    layer.preferHardwareDirect = frame.hasHardwareFrame();
    if (!layer.preferHardwareDirect) {
      const QImage layerImage =
          frame.hasCpuImage() ? frame.cpuImage() : frameHandleToCpuImage(frame);
      if (layerImage.isNull()) {
        ++decodeConvertFailCount;
        recordUnresolvedLayer(clip, QStringLiteral("frame_conversion_failed"));
        continue;
      }
      layer.image = layerImage;
      layer.frameSize = layerImage.size();
    }
    if (clip.mediaType == ClipMediaType::Image && !layer.preferHardwareDirect) {
      layer.cacheKey = clip.id + QStringLiteral(":prepared_rgba");
    }
    const bool generatedMaskMatte = matteOwner.clipRole == ClipRole::MaskMatte;
    if (generatedMaskMatte &&
        (layer.mediaOwnerClipId != layer.timingOwnerClipId ||
         layer.effectsOwnerClipId != layer.clipId ||
         layer.matteOwnerClipId != layer.clipId)) {
      continue;
    }
    const bool gpuMaskEnabled =
        matteOwner.maskEnabled && !matteOwner.maskFramesDir.trimmed().isEmpty() &&
        (generatedMaskMatte || matteOwner.maskShowOnly ||
         matteOwner.maskForegroundLayerEnabled || matteOwner.maskRepeatEnabled);
    if (gpuMaskEnabled) {
      const qint64 maskResolveStartMs = QDateTime::currentMSecsSinceEpoch();
      // The matte follows the frame that was actually decoded/presented for
      // its parent. It must never combine a requested source key with a
      // different bounded decode result (TIME.md).
      QString maskIdentity;
      std::shared_ptr<const jcut::core::ImageBuffer> maskBuffer =
          frame.frameNumber() >= 0
              ? rawClipMaskBuffer(matteOwner, frame, &maskIdentity)
              : std::shared_ptr<const jcut::core::ImageBuffer>{};
      if (!maskBuffer && frame.frameNumber() >= 0) {
        maskBuffer = rawClipMaskBufferBlocking(
            matteOwner, frame, &maskIdentity);
      }
      if (maskBuffer) {
        layer.maskBuffer = maskBuffer;
        layer.maskIdentity = maskIdentity;
        layer.setCorrectionPolygons(
            generatedMaskMatte
                ? effects.correctionPolygons
                : QVector<TimelineClip::CorrectionPolygon>{});
        layer.maskSourceSize =
            QSize(maskBuffer->size.width, maskBuffer->size.height);
        layer.maskTextureEnabled = true;
        layer.maskClipSource = generatedMaskMatte;
        layer.maskShowOnly = matteOwner.maskShowOnly;
        layer.maskGradeEnabled = false;
        layer.maskForegroundLayerEnabled = matteOwner.maskForegroundLayerEnabled;
        layer.maskInvert = matteOwner.maskInvert;
        layer.maskErode = qRound(qMax<qreal>(0.0, matteOwner.maskErode));
        layer.maskDilate = qRound(qMax<qreal>(0.0, matteOwner.maskDilate));
        layer.maskBlur = qRound(qMax<qreal>(matteOwner.maskFeather, matteOwner.maskBlur));
        layer.maskFeatherGamma = static_cast<float>(
            qBound<qreal>(0.1, matteOwner.maskFeatherGamma, 5.0));
        layer.maskFeatherFalloff = qBound(0, matteOwner.maskFeatherFalloff, 5);
        layer.maskOpacity = static_cast<float>(qBound<qreal>(0.0, matteOwner.maskOpacity, 1.0));
        layer.maskDropShadowRadius = static_cast<float>(
            qBound<qreal>(0.0, matteOwner.maskDropShadowRadius, 200.0));
        layer.maskDropShadowOffsetX = static_cast<float>(matteOwner.maskDropShadowOffsetX);
        layer.maskDropShadowOffsetY = static_cast<float>(matteOwner.maskDropShadowOffsetY);
        layer.maskDropShadowOpacity = static_cast<float>(
            qBound<qreal>(0.0, matteOwner.maskDropShadowOpacity, 1.0));
        layer.maskDropShadowEnabled = generatedMaskMatte &&
            matteOwner.maskDropShadowEnabled && layer.maskDropShadowOpacity > 0.0f;
        TimelineClip::GradingKeyframe maskGrade;
        maskGrade.brightness = matteOwner.maskGradeBrightness;
        maskGrade.contrast = matteOwner.maskGradeContrast;
        maskGrade.saturation = matteOwner.maskGradeSaturation;
        maskGrade.curvePointsR = matteOwner.maskGradeCurvePointsR;
        maskGrade.curvePointsG = matteOwner.maskGradeCurvePointsG;
        maskGrade.curvePointsB = matteOwner.maskGradeCurvePointsB;
        maskGrade.curvePointsLuma = matteOwner.maskGradeCurvePointsLuma;
        maskGrade.curveSmoothingEnabled = matteOwner.maskGradeCurveSmoothingEnabled;
        layer.setMaskGrade(maskGrade);
      }
      renderFrameMaskResolveMs += QDateTime::currentMSecsSinceEpoch() - maskResolveStartMs;
    }
    // A generated matte is never allowed to fall back to a full-frame copy
    // when its sidecar has no sample for this frame.
    if (generatedMaskMatte && !layer.maskTextureEnabled) {
      --visualClipCandidates;
      continue;
    }
    layer.setGrading(grade);
    VulkanDrawEffectState& layerEffects = layer.gradePayload.effects;
    layerEffects.shadows[3] = layer.gradePayload.curveLutApplied
                           ? kVulkanEffectModeCurve
                           : kVulkanEffectModeNormal;
    if (generatedMaskMatte) {
      layerEffects.opacity *= layer.maskOpacity;
      layer.maskGrade = layer.grading;
      layer.maskGradePayload = layer.gradePayload;
      layerEffects.midtones[3] = layer.gradePayload.curveLutApplied
          ? kVulkanMaskGradeUseSelectedCurveLut : 0.0f;
    }
    QJsonObject transformDiagnostics;
    const TimelineClip::TransformKeyframe transform =
        evaluateClipRenderTransformWithSourceLockAtPosition(
            clip,
            request.clips,
            transformClockTimelineFrame,
            request.renderSyncMarkers,
            request.playbackTiming,
            request.outputSize,
            &transformDiagnostics);
    layer.transform = transform;
    const QSize sourceSize = timingSource.sourceFrameSize.isValid()
        ? timingSource.sourceFrameSize
        : (layer.frameSize.isValid() ? layer.frameSize : layer.image.size());
    const QRectF fitted = fitRectF(sourceSize, request.outputSize);
    QPointF exportVideoTranslation(transform.translationX, transform.translationY);
    PreviewClipGeometry layerGeometry = PreviewViewTransform::clipGeometry(
        fitted,
        QPointF(1.0, 1.0),
        exportVideoTranslation,
        transform.rotation,
        QPointF(transform.scaleX, transform.scaleY));
    const QRectF outputRect(QPointF(0.0, 0.0), QSizeF(request.outputSize));
    layer.targetRect = outputRect;
    layer.fittedRect = fitted;
    const TimelineClip effectClip = clipWithResolvedTimingOwner(
        evaluateClipEffectAnimationAtPosition(
            clipWithRenderableEffectSettings(effectsOwner, request.tracks),
            static_cast<qreal>(timelineFrame),
            request.renderSyncMarkers,
            request.playbackTiming),
        request.clips);
    if (effectClip.effectPreset == ClipEffectPreset::DifferenceMatte) {
      layer.differenceThreshold =
          qBound<qreal>(0.0, effectClip.differenceThreshold, 1.0);
      layer.differenceSoftness =
          qBound<qreal>(0.0, effectClip.differenceSoftness, 1.0);
      const int64_t referenceFrameNumber =
          qMax<int64_t>(0, localFrame - qBound(1, effectClip.differenceReferenceFrames, 300));
      layer.differenceReferenceFrame = decodeFrameForTimingOwner(
          timingSource, decodePath, referenceFrameNumber);
      if (!layer.differenceReferenceFrame.isNull()) {
        layer.differenceMatteEnabled = true;
        layerEffects.shadows[3] = kVulkanEffectModeDifferenceMatte;
        layerEffects.midtones[3] =
            static_cast<float>(layer.differenceThreshold);
        layerEffects.highlights[3] =
            static_cast<float>(layer.differenceSoftness);
        layer.maskTextureEnabled = false;
      } else {
        recordUnresolvedLayer(
            clip, QStringLiteral("difference_reference_unavailable"));
      }
    }
    const bool clipEdgeFillEffect =
        effectClip.edgeFillEffect != BackgroundFillEffect::None &&
        vulkanClipSupportsBackgroundFillSource(clip);
    OffscreenVulkanRendererPrivate::LayerInput bidirectionalEdgeLayer;
    bool bidirectionalEdgeLayerPending = false;
    TimelineClip foregroundEffectClip = effectClip;
    const QRectF effectBounds =
        (foregroundEffectClip.effectPreset == ClipEffectPreset::SourceTile ||
         foregroundEffectClip.maskRepeatEnabled)
            ? layerGeometry.bounds.intersected(outputRect)
            : outputRect;
    const VulkanEffectPipelinePlan effectPlan = vulkanEffectPipelinePlan(
        foregroundEffectClip,
        effectBounds,
        sourceSize,
        timelineFrame,
        clipEffectPlaybackFramePosition(foregroundEffectClip, request.clips, generatedEffectClockTimelineFrame,
                                        request.playbackTiming,
                                        request.tracks),
        request.playbackTiming);
    layer.effectPlan = effectPlan;
    if (foregroundEffectClip.effectPreset == ClipEffectPreset::SourceTile && effectBounds.isValid()) {
      layer.presetScissorEnabled = true;
      layer.presetScissorRect = effectBounds;
    }
    if (exportFaceTransformDiagnostics && clip.speakerFramingEnabled) {
      transformDiagnostics.insert(QStringLiteral("clip_id"), clip.id);
      transformDiagnostics.insert(QStringLiteral("clip_label"), clip.label);
      transformDiagnostics.insert(QStringLiteral("timeline_frame_position"), timelineFrame);
      transformDiagnostics.insert(QStringLiteral("transform_clock_timeline_frame_position"),
                                  transformClockTimelineFrame);
      transformDiagnostics.insert(QStringLiteral("timeline_sample"), static_cast<qint64>(frameClock.timelineSample));
      transformDiagnostics.insert(QStringLiteral("sync_clock_domain"), QStringLiteral("timeline_sample"));
      transformDiagnostics.insert(QStringLiteral("decode_source_frame"), static_cast<qint64>(localFrame));
      transformDiagnostics.insert(QStringLiteral("mapped_source_sample"), static_cast<qint64>(frameMapping.sourceSample));
      transformDiagnostics.insert(QStringLiteral("mapped_source_frame_position"), frameMapping.sourceFramePosition);
      transformDiagnostics.insert(QStringLiteral("mapped_transcript_frame"), static_cast<qint64>(frameMapping.transcriptFrame));
      transformDiagnostics.insert(QStringLiteral("renderer_texture_origin"), QStringLiteral("top_left"));
      transformDiagnostics.insert(QStringLiteral("renderer_texture_normalized"), true);
      transformDiagnostics.insert(QStringLiteral("export_video_translation"), QJsonObject{
          {QStringLiteral("x"), exportVideoTranslation.x()},
          {QStringLiteral("y"), exportVideoTranslation.y()}
      });
      transformDiagnostics.insert(QStringLiteral("output_path"), request.outputPath);
      const QRectF layerRect = layerGeometry.bounds;
      transformDiagnostics.insert(QStringLiteral("layer_center"), QJsonObject{
          {QStringLiteral("x"), layerRect.center().x()},
          {QStringLiteral("y"), layerRect.center().y()}
      });
      transformDiagnostics.insert(QStringLiteral("layer_size"), QJsonObject{
          {QStringLiteral("width"), layerRect.width()},
          {QStringLiteral("height"), layerRect.height()}
      });
      transformDiagnostics.insert(QStringLiteral("layer_rect"),
                                  rectDiagnosticObject(layerRect));
      transformDiagnostics.insert(QStringLiteral("face_target_rect"),
                                  rectDiagnosticObject(
                                      faceTargetRectFromTransformDiagnostics(
                                          transformDiagnostics)));
      *exportFaceTransformDiagnostics = transformDiagnostics;
    }
    vulkanMvpForExportVideoLayer(
        fitted,
        exportVideoTranslation,
        request.outputSize,
        transform.rotation,
        QPointF(transform.scaleX, transform.scaleY),
        layer.mvp);
    if (clipVisible && clipEdgeFillEffect) {
      OffscreenVulkanRendererPrivate::LayerInput backgroundLayer;
      backgroundLayer.frameSize = request.outputSize;
      const BackgroundFillEffect fillEffect = effectClip.edgeFillEffect;
      const int edgePixels = qBound(1, effectClip.edgeFillPixels, 512);
      const qreal edgePower =
          qBound<qreal>(0.25, effectClip.edgeFillPower, 8.0);
      const VulkanDrawEffectState backgroundEffects =
          vulkanBackgroundFillEffectState(
              fillEffect,
              static_cast<float>(effectClip.edgeFillOpacity),
              static_cast<float>(effectClip.edgeFillBrightness),
              static_cast<float>(effectClip.edgeFillSaturation),
              edgePixels,
              static_cast<float>(edgePower),
              frame.validTextureRectNormalized(),
              vulkanBackgroundFillMapping(
                  layerGeometry.clipToScreen,
                  layerGeometry.localRect,
                  request.outputSize));
      backgroundLayer.gradePayload = layer.gradePayload;
      backgroundLayer.gradePayload.effects = backgroundEffects;
      std::copy(std::begin(layerEffects.shadows),
                std::end(layerEffects.shadows),
                std::begin(backgroundLayer.backgroundShadows));
      std::copy(std::begin(layerEffects.midtones),
                std::end(layerEffects.midtones),
                std::begin(backgroundLayer.backgroundMidtones));
      std::copy(std::begin(layerEffects.highlights),
                std::end(layerEffects.highlights),
                std::begin(backgroundLayer.backgroundHighlights));
      backgroundLayer.backgroundGrade[0] = layerEffects.brightness;
      backgroundLayer.backgroundGrade[1] = layerEffects.contrast;
      backgroundLayer.backgroundGrade[2] = layerEffects.saturation;
      // Background fills are derived views of this source layer, not new
      // color owners. Inherit the complete canonical grade payload so preview
      // and export bind the same curve LUT as well as the same tonal values.
      backgroundLayer.frame = frame;
      backgroundLayer.image = layer.image;
      backgroundLayer.frameSize = sourceSize;
      backgroundLayer.preferHardwareDirect = frame.hasHardwareFrame();
      if (!layer.cacheKey.isEmpty()) {
        backgroundLayer.cacheKey =
            layer.cacheKey + QStringLiteral(":progressive_background");
      }

      const bool fullCanvasFill =
          fillEffect == BackgroundFillEffect::EdgeStretch ||
          fillEffect == BackgroundFillEffect::ProgressiveEdgeStretch ||
          fillEffect == BackgroundFillEffect::ProgressiveBidirectionalEdgeStretch ||
          fillEffect == BackgroundFillEffect::Tile ||
          fillEffect == BackgroundFillEffect::Mirror;
      PreviewClipGeometry backgroundGeometry =
          fullCanvasFill ? PreviewViewTransform::clipGeometry(outputRect,
                                                              QPointF(1.0, 1.0),
                                                              QPointF(),
                                                              0.0,
                                                              QPointF(1.0, 1.0))
                         : layerGeometry;
      if (fillEffect == BackgroundFillEffect::BlurCover) {
        const qreal coverScale = std::max<qreal>(
            1.0,
            std::max(outputRect.width() / qMax<qreal>(1.0, layerGeometry.bounds.width()),
                     outputRect.height() / qMax<qreal>(1.0, layerGeometry.bounds.height())));
        backgroundGeometry.clipToScreen.scale(coverScale * 1.08, coverScale * 1.08);
      }
      vulkanMvpForPreviewTransform(backgroundGeometry.clipToScreen,
                                   backgroundGeometry.localRect,
                                   request.outputSize,
                                   backgroundLayer.mvp);
      if (!backgroundLayer.image.isNull() ||
          !backgroundLayer.frame.isNull()) {
        if (fillEffect ==
            BackgroundFillEffect::ProgressiveBidirectionalEdgeStretch) {
          bidirectionalEdgeLayer = backgroundLayer;
          bidirectionalEdgeLayerPending = true;
        } else {
          layers.push_back(backgroundLayer);
        }
      }
    }
    QVector<int> temporalEchoOrdinals;
    if (clipVisible) {
      if (effectClip.effectPreset == ClipEffectPreset::TemporalEcho) {
        const int echoCount = qBound(1, effectClip.temporalEchoCount, 12);
        const int spacing = qBound(1, effectClip.temporalEchoSpacingFrames, 120);
        const qreal decay = qBound<qreal>(0.0, effectClip.temporalEchoDecay, 1.0);
        layer.temporalEchoDecay = decay;
        for (int echoIndex = 1; echoIndex <= echoCount; ++echoIndex) {
          const int64_t echoFrameNumber = qMax<int64_t>(0, localFrame - echoIndex * spacing);
          const editor::FrameHandle echoFrame = decodeFrameForTimingOwner(
              timingSource, decodePath, echoFrameNumber);
          if (echoFrame.isNull()) {
            recordUnresolvedLayer(
                clip, QStringLiteral("temporal_echo_frame_unavailable"));
            continue;
          }
          layer.temporalEchoFrames.push_back(echoFrame);
          temporalEchoOrdinals.push_back(echoIndex);
        }
      }
      layers.push_back(layer);
      if (effectClip.effectPreset == ClipEffectPreset::TemporalEcho) {
        const qreal decay = layer.temporalEchoDecay;
        for (int echoIndex = 0;
             echoIndex < layer.temporalEchoFrames.size();
             ++echoIndex) {
          const editor::FrameHandle& echoFrame =
              layer.temporalEchoFrames.at(echoIndex);
          OffscreenVulkanRendererPrivate::LayerInput echoLayer = layer;
          echoLayer.frame = echoFrame;
          echoLayer.differenceReferenceFrame = {};
          echoLayer.differenceMatteEnabled = false;
          echoLayer.image = echoFrame.hasCpuImage() ? echoFrame.cpuImage() : QImage();
          echoLayer.frameSize = echoFrame.size();
          echoLayer.preferHardwareDirect = echoFrame.hasHardwareFrame();
          echoLayer.cacheKey.clear();
          echoLayer.effectPlan.generatedDraws.clear();
          echoLayer.maskTextureEnabled = false;
          echoLayer.gradePayload.effects.opacity = static_cast<float>(
              qBound<qreal>(
                  0.0,
                  grade.opacity *
                      std::pow(decay,
                               temporalEchoOrdinals.value(
                                   echoIndex, echoIndex + 1)),
                  1.0));
          echoLayer.gradePayload.effects.shadows[3] =
              echoLayer.gradePayload.curveLutApplied
              ? kVulkanEffectModeCurve : kVulkanEffectModeNormal;
          layers.push_back(echoLayer);
        }
      }
    }
    if (bidirectionalEdgeLayerPending) {
      layers.push_back(bidirectionalEdgeLayer);
    }
    if (layer.maskTextureEnabled &&
        layer.maskForegroundLayerEnabled) {
      OffscreenVulkanRendererPrivate::LayerInput foregroundLayer = layer;
      const bool applyMaskGradeToForeground = foregroundLayer.maskGradeEnabled;
      foregroundLayer.effectPlan.generatedDraws.clear();
      foregroundLayer.maskShowOnly = false;
      foregroundLayer.maskGradeEnabled = false;
      VulkanDrawEffectState& foregroundEffects =
          foregroundLayer.gradePayload.effects;
      foregroundEffects.opacity = static_cast<float>(layer.maskOpacity);
      foregroundLayer.maskDropShadowEnabled =
          clip.maskDropShadowEnabled && layer.maskDropShadowOpacity > 0.0f;
      foregroundEffects.brightness = applyMaskGradeToForeground
          ? layer.maskGradePayload.effects.brightness : 0.0f;
      foregroundEffects.contrast = applyMaskGradeToForeground
          ? layer.maskGradePayload.effects.contrast : 1.0f;
      foregroundEffects.saturation = applyMaskGradeToForeground
          ? layer.maskGradePayload.effects.saturation : 1.0f;
      foregroundEffects.shadows[0] = 0.0f;
      foregroundEffects.shadows[1] = 0.0f;
      foregroundEffects.shadows[2] = 0.0f;
      foregroundEffects.shadows[3] = kVulkanEffectModeMaskGrade;
      foregroundEffects.midtones[0] = 0.0f;
      foregroundEffects.midtones[1] = 0.0f;
      foregroundEffects.midtones[2] = 0.0f;
      foregroundEffects.midtones[3] =
          applyMaskGradeToForeground &&
                  layer.maskGradePayload.curveLutApplied
              ? kVulkanMaskGradeUseSelectedCurveLut
              : 0.0f;
      foregroundEffects.highlights[0] = 0.0f;
      foregroundEffects.highlights[1] = 0.0f;
      foregroundEffects.highlights[2] = 0.0f;
      foregroundEffects.highlights[3] = static_cast<float>(
          foregroundLayer.maskFeatherFalloff * 10) +
          foregroundLayer.maskFeatherGamma;
      foregroundMaskLayers.push_back(foregroundLayer);
    }
    if (!clip.titleKeyframes.isEmpty()) {
      const EvaluatedTitle title = prepareRenderableTitleForVulkanText(
          clip,
          static_cast<qreal>(timelineFrame),
          request.playbackTiming,
          static_cast<qreal>(grade.opacity),
          request.outputSize);
      if (title.valid) {
        textInputs.title3D.push_back(title);
      }
    }
    ++visualLayersResolved;
  }
  const qint64 layerBuildElapsedMs =
      QDateTime::currentMSecsSinceEpoch() - layerBuildStartMs;
  const qint64 layerPlanElapsedMs =
      qMax<qint64>(0, layerBuildElapsedMs - renderFrameDecodeWaitMs -
                          renderFrameMaskResolveMs);
  if (layerPlanMs) {
    *layerPlanMs += layerPlanElapsedMs;
  }
  layers += foregroundMaskLayers;
  const qint64 textInputStartMs = QDateTime::currentMSecsSinceEpoch();
  QStringList subtitleFailures;
  if (hasTranscriptCandidate) {
    textInputs.transcripts = d->buildTranscriptTextInputs(
        request.outputSize, request,
        frameClock,
        transcriptOverlayClips,
        &subtitleFailures);
    for (const OffscreenVulkanRendererPrivate::TranscriptTextInput &text : textInputs.transcripts) {
      transcriptLayerBounds = transcriptLayerBounds.united(text.outputRect);
    }
  }
  if (!subtitleFailures.isEmpty()) {
    const QString failure = QStringLiteral(
        "Vulkan export refused to drop subtitle overlay(s) at frame %1: %2")
        .arg(timelineFrame)
        .arg(subtitleFailures.join(QStringLiteral("; ")));
    qWarning().noquote() << QStringLiteral(
                                "[vulkan-subtitle] definitive subtitle failure "
                                "frame=%1 failures=%2")
                                .arg(timelineFrame)
                                .arg(subtitleFailures.join(QStringLiteral(" | ")));
    if (context.frameFailureReason) {
      *context.frameFailureReason = failure;
    }
    return QImage();
  }
  textInputs.hasSpeakerLabel = d->buildSpeakerLabelSpec(
      request,
      frameClock,
      orderedClips,
      &textInputs.speakerLabel);
  const qint64 textInputElapsedMs =
      QDateTime::currentMSecsSinceEpoch() - textInputStartMs;
  if (textPrepMs) {
    *textPrepMs += textInputElapsedMs;
  }
  if (exportFaceTransformDiagnostics &&
      textInputs.hasSpeakerLabel &&
      !exportFaceTransformDiagnostics->isEmpty()) {
    const VulkanTextLayoutDebug speakerLabelDebug =
        d->speakerLabelLayoutDebug(request.outputSize, textInputs.speakerLabel);
    if (speakerLabelDebug.valid) {
      exportFaceTransformDiagnostics->insert(
          QStringLiteral("speaker_label_cards"),
          rectDiagnosticArray(speakerLabelDebug.cards));
      exportFaceTransformDiagnostics->insert(
          QStringLiteral("speaker_label_card_count"),
          speakerLabelDebug.cardCount);
      exportFaceTransformDiagnostics->insert(
          QStringLiteral("speaker_label_name"),
          textInputs.speakerLabel.name);
      exportFaceTransformDiagnostics->insert(
          QStringLiteral("speaker_label_organization"),
          textInputs.speakerLabel.organization);
    }
  }
  if (!unresolvedVisualLayers.isEmpty() ||
      visualLayersResolved != visualClipCandidates) {
    const QString failure = QStringLiteral(
        "Vulkan export refused an incomplete composition at frame %1 "
        "(resolved %2/%3): %4")
        .arg(timelineFrame)
        .arg(visualLayersResolved)
        .arg(visualClipCandidates)
        .arg(unresolvedVisualLayers.join(QStringLiteral("; ")));
    qWarning().noquote() << QStringLiteral(
                                "[vulkan-compose] incomplete visual layers at "
                                "frame=%1 candidates=%2 decode_path_missing=%3 "
                                "decode_null=%4 convert_fail=%5")
                                .arg(timelineFrame)
                                .arg(visualClipCandidates)
                                .arg(decodePathMissingCount)
                                .arg(decodeNullCount)
                                .arg(decodeConvertFailCount);
    if (context.frameFailureReason) {
      *context.frameFailureReason = failure;
    }
    return QImage();
  }
  if (layers.isEmpty()) {
    OffscreenVulkanRendererPrivate::LayerInput black;
    black.image = QImage(request.outputSize, QImage::Format_RGBA8888);
    black.image.fill(Qt::black);
    black.frameSize = black.image.size();
    black.gradePayload.effects.opacity = 1.0f;
    layers.push_back(black);
  }
  const qint64 guidePrepareStartMs = QDateTime::currentMSecsSinceEpoch();
  if (request.instagramSafeAreaGuides || request.alignmentGridGuides) {
    OffscreenVulkanRendererPrivate::LayerInput guides;
    guides.clipId = QStringLiteral("output-placement-guides");
    guides.overlayImage =
        d->placementGuideOverlay(request.outputSize,
                                 request.instagramSafeAreaGuides,
                                 request.alignmentGridGuides);
    guides.frameSize = request.outputSize;
    guides.gradePayload.effects.opacity = 1.0f;
    if (!guides.overlayImage.isNull()) {
      render_detail::vulkanMvpForOutputRect(
          QRectF(QPointF(0.0, 0.0), QSizeF(request.outputSize)),
          request.outputSize,
          0.0,
          guides.mvp);
      layers.push_back(guides);
    }
  }
  const qint64 guidePrepareElapsedMs =
      QDateTime::currentMSecsSinceEpoch() - guidePrepareStartMs;
  if (guideOverlayMs) {
    *guideOverlayMs += guidePrepareElapsedMs;
  }
  const qint64 renderStartMs = QDateTime::currentMSecsSinceEpoch();
  const bool shouldReadbackToImage = (readbackMs != nullptr);
  const QImage output = d->renderFrameFromLayers(layers,
                                                 textInputs,
                                                 shouldReadbackToImage,
                                                 context.gpuPreviewFrame,
                                                 context.gpuPreviewError,
                                                 context.frameFailureReason);
  const qint64 compositeElapsedMs =
      QDateTime::currentMSecsSinceEpoch() - renderStartMs;
  if (gpuCompositeMs) {
    *gpuCompositeMs += compositeElapsedMs;
  }
  if (compositeMs) {
    *compositeMs = compositeElapsedMs;
  }
  recordRenderFrameStageMetric(
      QStringLiteral("__render_frame_layer_build__"),
      QStringLiteral("__render_frame_layer_build__"),
      layerPlanElapsedMs);
  recordRenderFrameStageMetric(
      QStringLiteral("__render_frame_decode_wait__"),
      QStringLiteral("__render_frame_decode_wait__"),
      renderFrameDecodeWaitMs);
  recordRenderFrameStageMetric(
      QStringLiteral("__render_frame_mask_resolve__"),
      QStringLiteral("__render_frame_mask_resolve__"),
      renderFrameMaskResolveMs);
  recordRenderFrameStageMetric(
      QStringLiteral("__render_frame_text_inputs__"),
      QStringLiteral("__render_frame_text_inputs__"),
      textInputElapsedMs);
  recordRenderFrameStageMetric(
      QStringLiteral("__render_frame_guide_prepare__"),
      QStringLiteral("__render_frame_guide_prepare__"),
      guidePrepareElapsedMs);
  recordRenderFrameStageMetric(
      QStringLiteral("__render_frame_vulkan_composite_submit__"),
      QStringLiteral("__render_frame_vulkan_composite_submit__"),
      compositeElapsedMs);
  if (!shouldReadbackToImage) {
    return QImage();
  }
  if (hasTranscriptCandidate && !output.isNull() &&
      vulkanSubtitleDebugEnabled()) {
    const QRectF countBounds =
        transcriptLayerBounds.isValid()
            ? transcriptLayerBounds
            : QRectF(QPointF(0, 0), QSizeF(output.size()));
    const SubtitlePixelCounts counts = countSubtitlePixels(output, countBounds);
    qWarning().noquote()
        << QStringLiteral("[vulkan-subtitle-composite] frame=%1 "
                          "bounds=(%2,%3 %4x%5) pixels_dark=%6 "
                          "pixels_bright=%7 pixels_yellow=%8 pixels_alpha=%9")
               .arg(timelineFrame)
               .arg(countBounds.x(), 0, 'f', 1)
               .arg(countBounds.y(), 0, 'f', 1)
               .arg(countBounds.width(), 0, 'f', 1)
               .arg(countBounds.height(), 0, 'f', 1)
               .arg(counts.dark)
               .arg(counts.bright)
               .arg(counts.yellow)
               .arg(counts.nonTransparent);
    if (vulkanSubtitleDumpEnabled()) {
      const QString path = QDir::temp().filePath(
          QStringLiteral("jcut-vulkan-composited-frame-f%1.png")
              .arg(timelineFrame));
      output.save(path);
      qWarning().noquote()
          << QStringLiteral("[vulkan-subtitle-composite] dumped_frame=\"%1\"")
                 .arg(path);
    }
  }
  return output;
}

bool OffscreenVulkanRenderer::renderFrameToOutput(
    const OffscreenRenderContext &context, OffscreenRenderFrame *output,
    bool readbackToCpuImage) {
  if (!output) {
    return false;
  }
  *output = OffscreenRenderFrame{};
  OffscreenRenderContext frameContext = context;
  frameContext.readbackMs = readbackToCpuImage ? context.readbackMs : nullptr;
  frameContext.frameFailureReason = &output->failureReason;
  output->cpuImage = renderFrame(frameContext);
  if (!output->failureReason.isEmpty()) {
    return false;
  }
  if (!frameContext.externalVulkanOutput) {
    const bool rendered = readbackToCpuImage
        ? !output->cpuImage.isNull()
        : d->hasPendingGpuFrame();
    if (!rendered) {
      output->failureReason =
          QStringLiteral("Vulkan compositor produced no frame.");
    }
    return rendered;
  }
  QString error;
  if (!lastRenderedVulkanFrame(&output->vulkanFrame, &error)) {
    output->vulkanFrame.valid = false;
    output->failureReason = error.isEmpty()
        ? QStringLiteral("Vulkan compositor produced no external frame.")
        : error;
    return false;
  }
  const bool rendered = readbackToCpuImage
      ? !output->cpuImage.isNull()
      : output->vulkanFrame.valid;
  if (!rendered) {
    output->failureReason = readbackToCpuImage
        ? QStringLiteral("Vulkan compositor readback produced no CPU image.")
        : QStringLiteral("Vulkan compositor produced an invalid external frame.");
  }
  return rendered;
}

bool OffscreenVulkanRenderer::convertLastFrameToNv12(AVFrame *frame,
                                                     qint64 *nv12ConvertMs,
                                                     qint64 *readbackMs) {
  return d && d->convertLastFrameToNv12(frame, nv12ConvertMs, readbackMs);
}

bool OffscreenVulkanRenderer::beginLastFrameToNv12Readback(qint64 *convertMs,
                                                           qint64 *readbackMs) {
  return d && d->beginLastFrameToNv12Readback(convertMs, readbackMs);
}

bool OffscreenVulkanRenderer::finishLastFrameToNv12Readback(
    AVFrame *frame, qint64 *convertMs, qint64 *readbackMs) {
  return d && d->finishLastFrameToNv12Readback(frame, convertMs, readbackMs);
}

bool OffscreenVulkanRenderer::beginLastFrameToNv12CudaTransfer(
    qint64 *convertMs, qint64 *transferMs) {
  return d && d->beginLastFrameToNv12CudaTransfer(convertMs, transferMs);
}

bool OffscreenVulkanRenderer::finishLastFrameToNv12CudaTransfer(
    AVFrame *cudaFrame, qint64 *convertMs, qint64 *transferMs) {
  return d &&
         d->finishLastFrameToNv12CudaTransfer(cudaFrame, convertMs, transferMs);
}

bool OffscreenVulkanRenderer::convertLastFrameToYuv420p(AVFrame *frame,
                                                        qint64 *convertMs,
                                                        qint64 *readbackMs) {
  return d && d->convertLastFrameToYuv420p(frame, convertMs, readbackMs);
}

bool OffscreenVulkanRenderer::beginLastFrameToYuv420pReadback(
    qint64 *convertMs, qint64 *readbackMs) {
  return d && d->beginLastFrameToYuv420pReadback(convertMs, readbackMs);
}

bool OffscreenVulkanRenderer::finishLastFrameToYuv420pReadback(
    AVFrame *frame, qint64 *convertMs, qint64 *readbackMs) {
  return d && d->finishLastFrameToYuv420pReadback(frame, convertMs, readbackMs);
}

bool OffscreenVulkanRenderer::copyLastFrameToBgra(AVFrame *frame,
                                                  qint64 *readbackMs) {
  return d && d->copyLastFrameToBgra(frame, readbackMs);
}

bool OffscreenVulkanRenderer::publishLastFrameForGpuPreview(
    OffscreenVulkanFrame *frame, QString *errorMessage) {
  return d && d->publishLastFrameForGpuPreview(frame, errorMessage);
}

void OffscreenVulkanRenderer::finishGpuPreviewPublication() {
  if (!d || !d->hasPendingGpuFrame()) {
    return;
  }
  OffscreenVulkanFrame frame;
  QString error;
  d->finishLastFrameForExternalSampling(&frame, &error);
}

bool OffscreenVulkanRenderer::lastRenderedVulkanFrame(
    OffscreenVulkanFrame *frame, QString *errorMessage) const {
  return d && d->finishLastFrameForExternalSampling(frame, errorMessage);
}

bool OffscreenVulkanRenderer::supportsCudaExternalMemoryInterop() const {
  return d && d->supportsCudaExternalMemoryInterop();
}

bool OffscreenVulkanRenderer::supportsNv12CudaTransfer() const {
  return supportsCudaExternalMemoryInterop();
}

QString OffscreenVulkanRenderer::cudaExternalMemoryStatus() const {
  return d ? d->cudaExternalMemoryStatus() : QStringLiteral("renderer unavailable");
}

QString OffscreenVulkanRenderer::backendId() const {
  return QStringLiteral("vulkan");
}

} // namespace render_detail
