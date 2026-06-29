#include "offscreen_vulkan_renderer_backend.h"

#include "cpu_overlay_render_backend.h"
#include "editor_shared_effects.h"
#include "editor_shared_timing.h"
#include "offscreen_vulkan_renderer_helpers.h"
#include "preview_view_transform.h"
#include "render_internal.h"
#include "render_vulkan_shared.h"
#include "titles.h"
#include "vulkan_detector_frame_handoff.h"
#include "vulkan_text_renderer.h"

#include <QDateTime>
#include <QDebug>
#include <QDir>
#include <QFile>
#include <QImage>
#include <QScopeGuard>

#if JCUT_HAS_CUDA_DRIVER
extern "C" {
#include <libavutil/hwcontext.h>
#include <libavutil/hwcontext_cuda.h>
}
#include <cuda.h>
#endif
#include <cmath>
#include <cstring>
#include <unistd.h>
#include <vulkan/vulkan.h>

#if !defined(JCUT_SKIP_CUDA_EXTERNAL_MEMORY_DESTROY)
#define JCUT_SKIP_CUDA_EXTERNAL_MEMORY_DESTROY 0
#endif

namespace render_detail {
namespace {

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

struct MaskPreparePush {
  int outputSize[2];
  int inputSize[2];
  int invert = 0;
  int pad0 = 0;
};

struct MaskMorphBlurPush {
  int outputSize[2];
  int radius = 0;
  int mode = 0;
};

QImage rgbaMaskImageForUpload(const QImage &mask) {
  if (mask.isNull()) {
    return QImage();
  }
  const QImage gray = mask.convertToFormat(QImage::Format_Grayscale8);
  QImage rgba(gray.size(), QImage::Format_RGBA8888);
  for (int y = 0; y < gray.height(); ++y) {
    const uchar *src = gray.constScanLine(y);
    uchar *dst = rgba.scanLine(y);
    for (int x = 0; x < gray.width(); ++x) {
      const uchar v = src[x];
      dst[x * 4 + 0] = v;
      dst[x * 4 + 1] = v;
      dst[x * 4 + 2] = v;
      dst[x * 4 + 3] = 255;
    }
  }
  return rgba;
}

} // namespace

class OffscreenVulkanRendererPrivate {
public:
  static constexpr int kMaxLayerTextures = 12;
  static constexpr int kFrameSlots = 3;
  static constexpr int kMaskComputeDescriptorSetCount = 128;
  struct FrameSlot {
    VkCommandBuffer commandBuffer = VK_NULL_HANDLE;
    VkBuffer stagingBuffer = VK_NULL_HANDLE;
    VkDeviceMemory stagingMemory = VK_NULL_HANDLE;
    VkDeviceSize stagingAllocationSize = 0;
    void *stagingMapped = nullptr;
    VkBuffer cudaExportBuffer = VK_NULL_HANDLE;
    VkDeviceMemory cudaExportMemory = VK_NULL_HANDLE;
    VkDeviceSize cudaExportAllocationSize = 0;
#if JCUT_HAS_CUDA_DRIVER
    CUexternalMemory cudaExternalMemory = nullptr;
    CUdeviceptr cudaExternalDevicePtr = 0;
    CUcontext cudaImportContext = nullptr;
#endif
    VkFence fence = VK_NULL_HANDLE;
    bool stagingHostCoherent = false;
    bool inFlight = false;
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
  };
  ~OffscreenVulkanRendererPrivate() { release(); }

  bool initialize(const QSize &outputSize, QString *errorMessage) {
    release();

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

    VkDescriptorSetLayoutBinding descriptorBindings[4]{};
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

    VkDescriptorSetLayoutCreateInfo descriptorSetLayoutInfo{};
    descriptorSetLayoutInfo.sType =
        VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    descriptorSetLayoutInfo.bindingCount = 4;
    descriptorSetLayoutInfo.pBindings = descriptorBindings;

    if (vkCreateDescriptorSetLayout(m_device, &descriptorSetLayoutInfo, nullptr,
                                    &m_descriptorSetLayout) != VK_SUCCESS) {
      if (errorMessage) {
        *errorMessage =
            QStringLiteral("Failed to create Vulkan descriptor set layout.");
      }
      return false;
    }

    VkDescriptorPoolSize poolSize{};
    poolSize.type = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    poolSize.descriptorCount = (1 + kMaxLayerTextures) * 4;

    VkDescriptorPoolCreateInfo descriptorPoolInfo{};
    descriptorPoolInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    descriptorPoolInfo.poolSizeCount = 1;
    descriptorPoolInfo.pPoolSizes = &poolSize;
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
      VkWriteDescriptorSet writes[4]{};
      for (uint32_t binding = 0; binding < 4; ++binding) {
        writes[binding].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        writes[binding].dstSet = slot.descriptorSet;
        writes[binding].dstBinding = binding;
        writes[binding].descriptorCount = 1;
        writes[binding].descriptorType =
            VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        writes[binding].pImageInfo = &di[binding];
      }
      vkUpdateDescriptorSets(m_device, 4, writes, 0, nullptr);
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

    VkDescriptorSetLayoutBinding maskComputeBindings[2]{};
    maskComputeBindings[0].binding = 0;
    maskComputeBindings[0].descriptorType =
        VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    maskComputeBindings[0].descriptorCount = 1;
    maskComputeBindings[0].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
    maskComputeBindings[1].binding = 1;
    maskComputeBindings[1].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
    maskComputeBindings[1].descriptorCount = 1;
    maskComputeBindings[1].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
    VkDescriptorSetLayoutCreateInfo maskComputeLayoutInfo{};
    maskComputeLayoutInfo.sType =
        VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    maskComputeLayoutInfo.bindingCount = 2;
    maskComputeLayoutInfo.pBindings = maskComputeBindings;
    if (vkCreateDescriptorSetLayout(m_device, &maskComputeLayoutInfo, nullptr,
                                    &m_maskComputeDescriptorSetLayout) !=
        VK_SUCCESS) {
      if (errorMessage)
        *errorMessage =
            QStringLiteral("Failed to create Vulkan mask compute descriptor layout.");
      return false;
    }
    VkDescriptorPoolSize maskComputePoolSizes[2]{};
    maskComputePoolSizes[0].type = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    maskComputePoolSizes[0].descriptorCount = kMaskComputeDescriptorSetCount;
    maskComputePoolSizes[1].type = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
    maskComputePoolSizes[1].descriptorCount = kMaskComputeDescriptorSetCount;
    VkDescriptorPoolCreateInfo maskComputePoolInfo{};
    maskComputePoolInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    maskComputePoolInfo.poolSizeCount = 2;
    maskComputePoolInfo.pPoolSizes = maskComputePoolSizes;
    maskComputePoolInfo.maxSets = kMaskComputeDescriptorSetCount;
    if (vkCreateDescriptorPool(m_device, &maskComputePoolInfo, nullptr,
                               &m_maskComputeDescriptorPool) != VK_SUCCESS) {
      if (errorMessage)
        *errorMessage =
            QStringLiteral("Failed to create Vulkan mask compute descriptor pool.");
      return false;
    }
    VkDescriptorSetAllocateInfo maskComputeSetInfo{};
    maskComputeSetInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
    maskComputeSetInfo.descriptorPool = m_maskComputeDescriptorPool;
    std::array<VkDescriptorSetLayout, kMaskComputeDescriptorSetCount> maskComputeLayouts{};
    maskComputeLayouts.fill(m_maskComputeDescriptorSetLayout);
    maskComputeSetInfo.descriptorSetCount = kMaskComputeDescriptorSetCount;
    maskComputeSetInfo.pSetLayouts = maskComputeLayouts.data();
    if (vkAllocateDescriptorSets(m_device, &maskComputeSetInfo,
                                 m_maskComputeDescriptorSets.data()) != VK_SUCCESS) {
      if (errorMessage)
        *errorMessage =
            QStringLiteral("Failed to allocate Vulkan mask compute descriptor set.");
      return false;
    }

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
    VkPushConstantRange maskPreparePush{};
    maskPreparePush.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
    maskPreparePush.offset = 0;
    maskPreparePush.size = sizeof(MaskPreparePush);
    VkPipelineLayoutCreateInfo maskPrepareLayoutInfo{};
    maskPrepareLayoutInfo.sType =
        VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    maskPrepareLayoutInfo.setLayoutCount = 1;
    maskPrepareLayoutInfo.pSetLayouts = &m_maskComputeDescriptorSetLayout;
    maskPrepareLayoutInfo.pushConstantRangeCount = 1;
    maskPrepareLayoutInfo.pPushConstantRanges = &maskPreparePush;
    if (vkCreatePipelineLayout(m_device, &maskPrepareLayoutInfo, nullptr,
                               &m_maskPreparePipelineLayout) != VK_SUCCESS) {
      if (errorMessage)
        *errorMessage =
            QStringLiteral("Failed to create Vulkan mask prepare pipeline layout.");
      return false;
    }
    VkPushConstantRange maskMorphPush{};
    maskMorphPush.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
    maskMorphPush.offset = 0;
    maskMorphPush.size = sizeof(MaskMorphBlurPush);
    VkPipelineLayoutCreateInfo maskMorphLayoutInfo = maskPrepareLayoutInfo;
    maskMorphLayoutInfo.pPushConstantRanges = &maskMorphPush;
    if (vkCreatePipelineLayout(m_device, &maskMorphLayoutInfo, nullptr,
                               &m_maskMorphBlurPipelineLayout) != VK_SUCCESS) {
      if (errorMessage)
        *errorMessage =
            QStringLiteral("Failed to create Vulkan mask morph/blur pipeline layout.");
      return false;
    }

    const QString shaderDir = QStringLiteral(JCUT_VULKAN_SHADER_DIR);
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
    m_maskPrepareModule = createShaderModule(
        m_device,
        readBinaryFile(shaderDir + QStringLiteral("/mask_prepare.comp.spv")));
    m_maskMorphModule = createShaderModule(
        m_device,
        readBinaryFile(shaderDir + QStringLiteral("/mask_morph.comp.spv")));
    m_maskBlurModule = createShaderModule(
        m_device,
        readBinaryFile(shaderDir + QStringLiteral("/mask_blur.comp.spv")));

    if (m_effectsVertModule == VK_NULL_HANDLE ||
        m_effectsFragModule == VK_NULL_HANDLE ||
        m_maskVertModule == VK_NULL_HANDLE ||
        m_maskFragModule == VK_NULL_HANDLE ||
        m_nv12VertModule == VK_NULL_HANDLE ||
        m_nv12YFragModule == VK_NULL_HANDLE ||
        m_nv12UvFragModule == VK_NULL_HANDLE ||
        m_yuv420pUFragModule == VK_NULL_HANDLE ||
        m_yuv420pVFragModule == VK_NULL_HANDLE ||
        m_yuv420pComputeModule == VK_NULL_HANDLE ||
        m_maskPrepareModule == VK_NULL_HANDLE ||
        m_maskMorphModule == VK_NULL_HANDLE ||
        m_maskBlurModule == VK_NULL_HANDLE) {
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
    VkWriteDescriptorSet writes[4]{};
    for (uint32_t binding = 0; binding < 4; ++binding) {
      writes[binding].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
      writes[binding].dstSet = m_descriptorSet;
      writes[binding].dstBinding = binding;
      writes[binding].descriptorCount = 1;
      writes[binding].descriptorType =
          VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
      writes[binding].pImageInfo = &imageInfoDesc[binding];
    }
    vkUpdateDescriptorSets(m_device, 4, writes, 0, nullptr);

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

    auto createComputePipeline = [&](VkShaderModule module,
                                     VkPipelineLayout layout,
                                     VkPipeline *pipeline) -> bool {
      VkComputePipelineCreateInfo info{};
      info.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO;
      info.stage.sType =
          VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
      info.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
      info.stage.module = module;
      info.stage.pName = "main";
      info.layout = layout;
      return vkCreateComputePipelines(m_device, VK_NULL_HANDLE, 1, &info,
                                      nullptr, pipeline) == VK_SUCCESS;
    };
    if (!createComputePipeline(m_maskPrepareModule, m_maskPreparePipelineLayout,
                               &m_maskPreparePipeline) ||
        !createComputePipeline(m_maskMorphModule, m_maskMorphBlurPipelineLayout,
                               &m_maskMorphPipeline) ||
        !createComputePipeline(m_maskBlurModule, m_maskMorphBlurPipelineLayout,
                               &m_maskBlurPipeline)) {
      if (errorMessage) {
        *errorMessage =
            QStringLiteral("Failed to create Vulkan mask compute pipelines.");
      }
      return false;
    }

	    if (!createPipeline(m_effectsVertModule, m_effectsFragModule,
	                        m_effectsPipelineLayout, m_renderPass,
	                        &m_effectsPipeline) ||
        !createPipeline(m_maskVertModule, m_maskFragModule,
                        m_maskPipelineLayout, m_renderPass, &m_maskPipeline) ||
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
    m_speakerTextRenderer.reset();
    m_transcriptTextRenderer.reset();
    for (FrameSlot &slot : m_frameSlots) {
#if JCUT_HAS_CUDA_DRIVER
      if (slot.cudaExternalMemory) {
#if JCUT_SKIP_CUDA_EXTERNAL_MEMORY_DESTROY
        slot.cudaExternalMemory = nullptr;
        slot.cudaExternalDevicePtr = 0;
        slot.cudaImportContext = nullptr;
#else
        CUcontext previous = nullptr;
        if (slot.cudaImportContext) {
          cuCtxPushCurrent(slot.cudaImportContext);
        }
        cuDestroyExternalMemory(slot.cudaExternalMemory);
        if (slot.cudaImportContext) {
          cuCtxPopCurrent(&previous);
        }
        slot.cudaExternalMemory = nullptr;
        slot.cudaExternalDevicePtr = 0;
        slot.cudaImportContext = nullptr;
#endif
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
    if (m_maskPreparePipeline != VK_NULL_HANDLE) {
      vkDestroyPipeline(m_device, m_maskPreparePipeline, nullptr);
      m_maskPreparePipeline = VK_NULL_HANDLE;
    }
    if (m_maskMorphPipeline != VK_NULL_HANDLE) {
      vkDestroyPipeline(m_device, m_maskMorphPipeline, nullptr);
      m_maskMorphPipeline = VK_NULL_HANDLE;
    }
    if (m_maskBlurPipeline != VK_NULL_HANDLE) {
      vkDestroyPipeline(m_device, m_maskBlurPipeline, nullptr);
      m_maskBlurPipeline = VK_NULL_HANDLE;
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
    if (m_maskMorphBlurPipelineLayout != VK_NULL_HANDLE) {
      vkDestroyPipelineLayout(m_device, m_maskMorphBlurPipelineLayout, nullptr);
      m_maskMorphBlurPipelineLayout = VK_NULL_HANDLE;
    }
    if (m_maskPreparePipelineLayout != VK_NULL_HANDLE) {
      vkDestroyPipelineLayout(m_device, m_maskPreparePipelineLayout, nullptr);
      m_maskPreparePipelineLayout = VK_NULL_HANDLE;
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
    if (m_maskPrepareModule != VK_NULL_HANDLE) {
      vkDestroyShaderModule(m_device, m_maskPrepareModule, nullptr);
      m_maskPrepareModule = VK_NULL_HANDLE;
    }
    if (m_maskMorphModule != VK_NULL_HANDLE) {
      vkDestroyShaderModule(m_device, m_maskMorphModule, nullptr);
      m_maskMorphModule = VK_NULL_HANDLE;
    }
    if (m_maskBlurModule != VK_NULL_HANDLE) {
      vkDestroyShaderModule(m_device, m_maskBlurModule, nullptr);
      m_maskBlurModule = VK_NULL_HANDLE;
    }

    if (m_framebuffer != VK_NULL_HANDLE) {
      vkDestroyFramebuffer(m_device, m_framebuffer, nullptr);
      m_framebuffer = VK_NULL_HANDLE;
    }
    if (m_renderPass != VK_NULL_HANDLE) {
      vkDestroyRenderPass(m_device, m_renderPass, nullptr);
      m_renderPass = VK_NULL_HANDLE;
    }
    if (m_descriptorPool != VK_NULL_HANDLE) {
      vkDestroyDescriptorPool(m_device, m_descriptorPool, nullptr);
      m_descriptorPool = VK_NULL_HANDLE;
    }
    if (m_yuvComputeDescriptorPool != VK_NULL_HANDLE) {
      vkDestroyDescriptorPool(m_device, m_yuvComputeDescriptorPool, nullptr);
      m_yuvComputeDescriptorPool = VK_NULL_HANDLE;
    }
    if (m_maskComputeDescriptorPool != VK_NULL_HANDLE) {
      vkDestroyDescriptorPool(m_device, m_maskComputeDescriptorPool, nullptr);
      m_maskComputeDescriptorPool = VK_NULL_HANDLE;
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
    if (m_maskComputeDescriptorSetLayout != VK_NULL_HANDLE) {
      vkDestroyDescriptorSetLayout(m_device, m_maskComputeDescriptorSetLayout,
                                   nullptr);
      m_maskComputeDescriptorSetLayout = VK_NULL_HANDLE;
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
    m_graphicsQueue = VK_NULL_HANDLE;
    m_commandBuffer = VK_NULL_HANDLE;
    m_descriptorSet = VK_NULL_HANDLE;
    m_yuvComputeDescriptorSet = VK_NULL_HANDLE;
    m_maskComputeDescriptorSets.fill(VK_NULL_HANDLE);
    m_maskComputeDescriptorSetIndex = 0;
    m_graphicsQueueFamily = UINT32_MAX;
    m_initialized = false;
    m_yuv420pPlanesPrimed = false;
    m_colorImageLayout = VK_IMAGE_LAYOUT_UNDEFINED;
  }

  struct LayerInput {
    QImage image;
    QImage maskImage;
    QSize maskSourceSize;
    OverlayImage overlayImage;
    editor::FrameHandle frameHandle;
    QSize sourceSize;
    bool preferHardwareDirect = false;
    bool maskTextureEnabled = false;
    bool maskShowOnly = false;
    bool maskGradeEnabled = false;
    bool maskInvert = false;
    int maskErodeRadius = 0;
    int maskDilateRadius = 0;
    int maskBlurRadius = 0;
    QString cacheKey;
    QByteArray curveLutRgba = identityCurveLutBytes();
    QByteArray maskCurveLutRgba = identityCurveLutBytes();
    bool curveLutApplied = false;
    bool maskCurveLutApplied = false;
    float maskOpacity = 1.0f;
    float maskBrightness = 0.0f;
    float maskContrast = 1.0f;
    float maskSaturation = 1.0f;
    float brightness = 0.0f;
    float contrast = 1.0f;
    float saturation = 1.0f;
    float opacity = 1.0f;
    float shadows[4] = {0.0f, 0.0f, 0.0f, 0.0f};
    float midtones[4] = {0.0f, 0.0f, 0.0f, 0.0f};
    float highlights[4] = {0.0f, 0.0f, 0.0f, 0.0f};
    float mvp[16] = {1.f, 0.f, 0.f, 0.f, 0.f, 1.f, 0.f, 0.f,
                     0.f, 0.f, 1.f, 0.f, 0.f, 0.f, 0.f, 1.f};
  };

  struct TranscriptTextInput {
    TimelineClip clip;
    TranscriptOverlayLayout layout;
    QRectF outputRect;
    QString speakerTitle;
  };

  struct VulkanTextInputs {
    QVector<TranscriptTextInput> transcripts;
    bool hasSpeakerLabel = false;
    SpeakerLabelOverlaySpec speakerLabel;
  };

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
    auto *dst = reinterpret_cast<uint8_t *>(m_stagingMapped) + stagingOffset;
    const auto *src = reinterpret_cast<const uint8_t *>(overlay.rgbaPremultiplied.constData());
    for (int y = 0; y < overlay.height; ++y) {
      std::memcpy(dst + (static_cast<size_t>(y) * rowBytes),
                  src + (static_cast<size_t>(y) * rowBytes),
                  static_cast<size_t>(rowBytes));
    }
    return flushActiveStagingWrite(stagingOffset, static_cast<VkDeviceSize>(bytes));
  }

  bool writeRgbaImageToStagingTopLeft(const QImage &rgba,
                                      VkDeviceSize stagingOffset) {
    if (rgba.isNull() || !m_stagingMapped ||
        rgba.format() != QImage::Format_RGBA8888) {
      return false;
    }
    const int rowBytes = rgba.width() * 4;
    const size_t bytes = static_cast<size_t>(rowBytes) * static_cast<size_t>(rgba.height());
    auto *dst = reinterpret_cast<uint8_t *>(m_stagingMapped) + stagingOffset;
    for (int y = 0; y < rgba.height(); ++y) {
      std::memcpy(dst + (static_cast<size_t>(y) * rowBytes),
                  rgba.constScanLine(y),
                  static_cast<size_t>(rowBytes));
    }
    return flushActiveStagingWrite(stagingOffset, static_cast<VkDeviceSize>(bytes));
  }

  bool runMaskComputePass(VkPipeline pipeline,
                          VkPipelineLayout layout,
                          const void *pushData,
                          uint32_t pushDataSize,
                          VkImageView inputView,
                          VkImageView outputView,
                          VkImage outputImage,
                          VkImageLayout &outputLayout) {
    if (pipeline == VK_NULL_HANDLE || layout == VK_NULL_HANDLE ||
        m_maskComputeDescriptorSets[m_maskComputeDescriptorSetIndex] == VK_NULL_HANDLE ||
        inputView == VK_NULL_HANDLE || outputView == VK_NULL_HANDLE ||
        outputImage == VK_NULL_HANDLE) {
      return false;
    }
    VkDescriptorSet computeDescriptorSet =
        m_maskComputeDescriptorSets[m_maskComputeDescriptorSetIndex];
    m_maskComputeDescriptorSetIndex =
        (m_maskComputeDescriptorSetIndex + 1) % kMaskComputeDescriptorSetCount;
    transitionImageLayout(m_commandBuffer, outputImage, outputLayout,
                          VK_IMAGE_LAYOUT_GENERAL);
    outputLayout = VK_IMAGE_LAYOUT_GENERAL;

    VkDescriptorImageInfo inputInfo{};
    inputInfo.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    inputInfo.imageView = inputView;
    inputInfo.sampler = m_sampler;
    VkDescriptorImageInfo outputInfo{};
    outputInfo.imageLayout = VK_IMAGE_LAYOUT_GENERAL;
    outputInfo.imageView = outputView;
    VkWriteDescriptorSet writes[2]{};
    writes[0].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    writes[0].dstSet = computeDescriptorSet;
    writes[0].dstBinding = 0;
    writes[0].descriptorCount = 1;
    writes[0].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    writes[0].pImageInfo = &inputInfo;
    writes[1].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    writes[1].dstSet = computeDescriptorSet;
    writes[1].dstBinding = 1;
    writes[1].descriptorCount = 1;
    writes[1].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
    writes[1].pImageInfo = &outputInfo;
    vkUpdateDescriptorSets(m_device, 2, writes, 0, nullptr);

    vkCmdBindPipeline(m_commandBuffer, VK_PIPELINE_BIND_POINT_COMPUTE,
                      pipeline);
    vkCmdBindDescriptorSets(m_commandBuffer, VK_PIPELINE_BIND_POINT_COMPUTE,
                            layout, 0, 1, &computeDescriptorSet, 0,
                            nullptr);
    vkCmdPushConstants(m_commandBuffer, layout, VK_SHADER_STAGE_COMPUTE_BIT, 0,
                       pushDataSize, pushData);
    vkCmdDispatch(m_commandBuffer,
                  static_cast<uint32_t>((m_outputSize.width() + 15) / 16),
                  static_cast<uint32_t>((m_outputSize.height() + 15) / 16),
                  1);
    transitionImageLayout(m_commandBuffer, outputImage,
                          VK_IMAGE_LAYOUT_GENERAL,
                          VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    outputLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    return true;
  }

  bool preprocessLayerMask(LayerTextureSlot &slot, const LayerInput &layer) {
    MaskPreparePush prepare{};
    prepare.outputSize[0] = m_outputSize.width();
    prepare.outputSize[1] = m_outputSize.height();
    const QSize inputSize = layer.maskSourceSize.isValid() ? layer.maskSourceSize : m_outputSize;
    prepare.inputSize[0] = inputSize.width();
    prepare.inputSize[1] = inputSize.height();
    prepare.invert = layer.maskInvert ? 1 : 0;
    if (!runMaskComputePass(m_maskPreparePipeline,
                            m_maskPreparePipelineLayout, &prepare,
                            sizeof(prepare), slot.maskRawView, slot.maskView,
                            slot.maskImage, slot.maskLayout)) {
      return false;
    }

    VkImageView currentView = slot.maskView;
    bool currentIsFinal = true;
    auto dispatchMorphBlur = [&](VkPipeline pipeline, int radius,
                                 int mode) -> bool {
      VkImageView dstView = currentIsFinal ? slot.maskWorkView : slot.maskView;
      VkImage dstImage = currentIsFinal ? slot.maskWorkImage : slot.maskImage;
      VkImageLayout &dstLayout =
          currentIsFinal ? slot.maskWorkLayout : slot.maskLayout;
      MaskMorphBlurPush push{};
      push.outputSize[0] = m_outputSize.width();
      push.outputSize[1] = m_outputSize.height();
      push.radius = radius;
      push.mode = mode;
      if (!runMaskComputePass(pipeline, m_maskMorphBlurPipelineLayout,
                              &push, sizeof(push), currentView, dstView,
                              dstImage, dstLayout)) {
        return false;
      }
      currentView = dstView;
      currentIsFinal = !currentIsFinal;
      return true;
    };

    if (layer.maskErodeRadius > 0 &&
        !dispatchMorphBlur(m_maskMorphPipeline, layer.maskErodeRadius, 0)) {
      return false;
    }
    if (layer.maskDilateRadius > 0 &&
        !dispatchMorphBlur(m_maskMorphPipeline, layer.maskDilateRadius, 1)) {
      return false;
    }
    if (layer.maskBlurRadius > 0 &&
        (!dispatchMorphBlur(m_maskBlurPipeline, layer.maskBlurRadius, 1) ||
         !dispatchMorphBlur(m_maskBlurPipeline, layer.maskBlurRadius, 0))) {
      return false;
    }
    if (!currentIsFinal) {
      MaskPreparePush copy{};
      copy.outputSize[0] = m_outputSize.width();
      copy.outputSize[1] = m_outputSize.height();
      copy.inputSize[0] = m_outputSize.width();
      copy.inputSize[1] = m_outputSize.height();
      if (!runMaskComputePass(m_maskPreparePipeline,
                              m_maskPreparePipelineLayout, &copy,
                              sizeof(copy), currentView, slot.maskView,
                              slot.maskImage, slot.maskLayout)) {
        return false;
      }
    }
    slot.maskUploaded = true;
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
    submitInfo.commandBufferCount = 1;
    submitInfo.pCommandBuffers = &slot.commandBuffer;
    if (vkQueueSubmit(m_graphicsQueue, 1, &submitInfo, slot.fence) !=
        VK_SUCCESS) {
      return false;
    }
    slot.inFlight = true;
    return true;
  }

  bool waitSlot(int slotIndex) {
    if (slotIndex < 0 || slotIndex >= m_frameSlots.size()) {
      return false;
    }
    FrameSlot &slot = m_frameSlots[slotIndex];
    if (slot.inFlight) {
      if (vkWaitForFences(m_device, 1, &slot.fence, VK_TRUE, UINT64_MAX) !=
          VK_SUCCESS) {
        return false;
      }
      slot.inFlight = false;
    }
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
    if (slot.stagingHostCoherent || slot.stagingMemory == VK_NULL_HANDLE ||
        size == 0) {
      return true;
    }
    VkMappedMemoryRange range{};
    range.sType = VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE;
    range.memory = slot.stagingMemory;
    range.offset = offset;
    range.size = qMin(size, slot.stagingAllocationSize - offset);
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
                               bool readbackToImage) {
    if (!m_initialized || m_device == VK_NULL_HANDLE ||
        m_commandBuffer == VK_NULL_HANDLE) {
      return QImage();
    }
    if (!selectNextSlot()) {
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
    const VkDeviceSize layerImageBytes =
        static_cast<VkDeviceSize>(m_outputSize.width()) *
        static_cast<VkDeviceSize>(m_outputSize.height()) * 4;
    const VkDeviceSize layerStagingSize = (layerImageBytes * 2) + kCurveLutBytes;
    auto layerHasRenderableSource = [](const LayerInput &layer) {
      return !layer.overlayImage.isNull() || !layer.image.isNull() ||
             !layer.frameHandle.isNull();
    };
    auto updateLayerDescriptorSet = [&](LayerTextureSlot &slot,
                                        VkImageView sourceView,
                                        VkImageLayout sourceLayout) {
      VkDescriptorImageInfo di[4]{};
      di[0].imageLayout = sourceLayout;
      di[0].imageView = sourceView;
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
      VkWriteDescriptorSet writes[4]{};
      for (uint32_t binding = 0; binding < 4; ++binding) {
        writes[binding].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        writes[binding].dstSet = slot.descriptorSet;
        writes[binding].dstBinding = binding;
        writes[binding].descriptorCount = 1;
        writes[binding].descriptorType =
            VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        writes[binding].pImageInfo = &di[binding];
      }
      vkUpdateDescriptorSets(m_device, 4, writes, 0, nullptr);
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
      if (layer.preferHardwareDirect && !layer.frameHandle.isNull() &&
          layer.frameHandle.hasHardwareFrame()) {
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
          if (slot.hardwareFrameHandoff->uploadFrame(layer.frameHandle, false,
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
        } else if (!layer.frameHandle.isNull() &&
                   (layer.frameHandle.hasHardwareFrame() || layer.frameHandle.hasGpuTexture())) {
          return false;
        } else {
          rgba = frameHandleToCpuImage(layer.frameHandle);
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
    QSet<int> preparedTranscriptTextIndices;
    if (m_transcriptTextRenderer && m_transcriptTextRenderer->isReady()) {
      for (int i = 0; i < textInputs.transcripts.size(); ++i) {
        const TranscriptTextInput &text = textInputs.transcripts.at(i);
        if (m_transcriptTextRenderer->prepareTranscriptOverlayAtlas(
                m_commandBuffer, m_outputSize, text.clip, text.layout,
                text.outputRect, text.speakerTitle)) {
          preparedTranscriptTextIndices.insert(i);
        }
      }
    }
    const bool preparedSpeakerText =
        textInputs.hasSpeakerLabel && m_speakerTextRenderer &&
        m_speakerTextRenderer->isReady() &&
        m_speakerTextRenderer->prepareSpeakerLabelAtlas(
            m_commandBuffer, m_outputSize, textInputs.speakerLabel);

    int layerIndex = 0;
    while (layerIndex < layers.size()) {
      const int batchCount =
          qMin(kMaxLayerTextures, layers.size() - layerIndex);
      struct PreparedBatchLayer {
        VkImageView view = VK_NULL_HANDLE;
        VkImageLayout layout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
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

        const QByteArray curveBytes =
            layer.curveLutRgba.size() == static_cast<int>(kCurveLutBytes)
                ? layer.curveLutRgba
                : identityCurveLutBytes();
        if (!m_stagingMapped) {
          vkEndCommandBuffer(m_commandBuffer);
          return QImage();
        }
        const VkDeviceSize curveStagingOffset = stagingOffset + layerImageBytes;
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
            layer.maskCurveLutRgba.size() == static_cast<int>(kCurveLutBytes)
                ? layer.maskCurveLutRgba
                : identityCurveLutBytes();
        const VkDeviceSize maskCurveStagingOffset =
            stagingOffset + layerImageBytes + kCurveLutBytes;
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
        if (layer.maskTextureEnabled) {
          QImage maskUpload = layer.maskImage;
          if (maskUpload.isNull()) {
            vkEndCommandBuffer(m_commandBuffer);
            return QImage();
          }
          if (maskUpload.format() != QImage::Format_RGBA8888) {
            maskUpload = maskUpload.convertToFormat(QImage::Format_RGBA8888);
          }
          const VkDeviceSize maskStagingOffset =
              stagingOffset + layerImageBytes + (kCurveLutBytes * 2);
          if (!writeRgbaImageToStagingTopLeft(maskUpload, maskStagingOffset)) {
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
              static_cast<uint32_t>(maskUpload.width()),
              static_cast<uint32_t>(maskUpload.height()), 1};
          vkCmdCopyBufferToImage(m_commandBuffer, m_stagingBuffer,
                                 slot.maskRawImage,
                                 VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1,
                                 &maskUploadRegion);
          transitionImageLayout(m_commandBuffer, slot.maskRawImage,
                                VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                                VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
          slot.maskRawLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
          if (!preprocessLayerMask(slot, layer)) {
            vkEndCommandBuffer(m_commandBuffer);
            return QImage();
          }
        } else if (!slot.maskUploaded) {
          QImage whiteMask(m_outputSize, QImage::Format_RGBA8888);
          whiteMask.fill(Qt::white);
          const VkDeviceSize maskStagingOffset =
              stagingOffset + layerImageBytes + kCurveLutBytes;
          if (!writeRgbaImageToStagingTopLeft(whiteMask, maskStagingOffset)) {
            vkEndCommandBuffer(m_commandBuffer);
            return QImage();
          }
          transitionImageLayout(m_commandBuffer, slot.maskImage,
                                slot.maskLayout,
                                VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
          slot.maskLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
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
              static_cast<uint32_t>(m_outputSize.width()),
              static_cast<uint32_t>(m_outputSize.height()), 1};
          vkCmdCopyBufferToImage(m_commandBuffer, m_stagingBuffer,
                                 slot.maskImage,
                                 VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1,
                                 &maskUploadRegion);
          transitionImageLayout(m_commandBuffer, slot.maskImage,
                                VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                                VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
          slot.maskLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
          slot.maskUploaded = true;
        }
        updateLayerDescriptorSet(slot, preparedLayers[i].view,
                                 preparedLayers[i].layout);
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
        vkCmdBindDescriptorSets(
            m_commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS,
            m_effectsPipelineLayout, 0, 1, &slot.descriptorSet, 0, nullptr);
        auto drawLayer = [&](float brightness,
                             float contrast,
                             float saturation,
                             float opacity,
                             const float shadows[4],
                             const float midtones[4],
                             const float highlights[4],
                             float mode) {
          std::memcpy(push.mvp, layer.mvp, sizeof(push.mvp));
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
        if (layer.maskTextureEnabled && layer.maskShowOnly) {
          drawLayer(0.0f,
                    1.0f,
                    1.0f,
                    layer.maskOpacity,
                    layer.shadows,
                    layer.midtones,
                    layer.highlights,
                    kVulkanEffectModeMaskOnly);
          continue;
        }
        drawLayer(layer.brightness,
                  layer.contrast,
                  layer.saturation,
                  layer.opacity,
                  layer.shadows,
                  layer.midtones,
                  layer.highlights,
                  layer.shadows[3]);
        if (layer.maskTextureEnabled && layer.maskGradeEnabled) {
          float neutral[4] = {0.0f, 0.0f, 0.0f, 0.0f};
          float maskMidtones[4] = {0.0f, 0.0f, 0.0f,
                                   layer.maskCurveLutApplied
                                       ? kVulkanMaskGradeUseSelectedCurveLut
                                       : 0.0f};
          drawLayer(layer.maskBrightness,
                    layer.maskContrast,
                    layer.maskSaturation,
                    layer.maskOpacity,
                    neutral,
                    maskMidtones,
                    neutral,
                    kVulkanEffectModeMaskGrade);
        }
      }
      vkCmdEndRenderPass(m_commandBuffer);
      layerIndex += batchCount;
    }

    if (!preparedTranscriptTextIndices.isEmpty() || preparedSpeakerText) {
      vkCmdBeginRenderPass(m_commandBuffer, &renderPassBeginInfo,
                           VK_SUBPASS_CONTENTS_INLINE);
      const QRectF outputTargetRect(QPointF(0.0, 0.0), QSizeF(m_outputSize));
      for (int i = 0; i < textInputs.transcripts.size(); ++i) {
        if (!preparedTranscriptTextIndices.contains(i)) {
          continue;
        }
        const TranscriptTextInput &text = textInputs.transcripts.at(i);
        m_transcriptTextRenderer->drawTranscriptOverlay(
            m_commandBuffer, m_outputSize, m_outputSize, outputTargetRect,
            text.clip, text.layout, text.outputRect, text.speakerTitle);
      }
      if (preparedSpeakerText) {
        m_speakerTextRenderer->drawSpeakerLabel(
            m_commandBuffer, m_outputSize, m_outputSize, outputTargetRect,
            textInputs.speakerLabel);
      }
      vkCmdEndRenderPass(m_commandBuffer);
    }

    transitionImageLayout(m_commandBuffer, m_colorImage,
                          VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                          VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
    m_colorImageLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
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
    VkDescriptorImageInfo srcImageDesc{};
    srcImageDesc.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    srcImageDesc.imageView = m_colorImageView;
    srcImageDesc.sampler = m_sampler;
    VkWriteDescriptorSet srcWrite{};
    srcWrite.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    srcWrite.dstSet = m_descriptorSet;
    srcWrite.dstBinding = 0;
    srcWrite.descriptorCount = 1;
    srcWrite.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    srcWrite.pImageInfo = &srcImageDesc;
    vkUpdateDescriptorSets(m_device, 1, &srcWrite, 0, nullptr);

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
    vkCmdBindDescriptorSets(m_commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS,
                            m_nv12PipelineLayout, 0, 1, &m_descriptorSet, 0,
                            nullptr);
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
                            m_nv12PipelineLayout, 0, 1, &m_descriptorSet, 0,
                            nullptr);
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
    if (!framesCtx || !framesCtx->device_ctx || !framesCtx->device_ctx->hwctx) {
      return false;
    }
    auto *cudaDevice =
        reinterpret_cast<AVCUDADeviceContext *>(framesCtx->device_ctx->hwctx);
    CUcontext cudaContext = cudaDevice->cuda_ctx;
    if (!cudaContext) {
      return false;
    }

    CUcontext previous = nullptr;
    CUresult cuResult = cuInit(0);
    if (cuResult != CUDA_SUCCESS ||
        cuCtxPushCurrent(cudaContext) != CUDA_SUCCESS) {
      return false;
    }

    auto popContext = qScopeGuard([&]() { cuCtxPopCurrent(&previous); });

    if (slot.cudaExternalMemory && slot.cudaImportContext != cudaContext) {
      cuDestroyExternalMemory(slot.cudaExternalMemory);
      slot.cudaExternalMemory = nullptr;
      slot.cudaExternalDevicePtr = 0;
      slot.cudaImportContext = nullptr;
    }
    if (slot.cudaExternalMemory && slot.cudaExternalDevicePtr) {
      return true;
    }

    VkMemoryGetFdInfoKHR fdInfo{};
    fdInfo.sType = VK_STRUCTURE_TYPE_MEMORY_GET_FD_INFO_KHR;
    fdInfo.memory = slot.cudaExportMemory;
    fdInfo.handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_FD_BIT;
    int memoryFd = -1;
    if (m_vkGetMemoryFdKHR(m_device, &fdInfo, &memoryFd) != VK_SUCCESS ||
        memoryFd < 0) {
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
      return false;
    }

    CUDA_EXTERNAL_MEMORY_BUFFER_DESC bufferDesc{};
    bufferDesc.offset = 0;
    bufferDesc.size = slot.cudaExportAllocationSize;
    cuResult = cuExternalMemoryGetMappedBuffer(
        &slot.cudaExternalDevicePtr, slot.cudaExternalMemory, &bufferDesc);
    if (cuResult != CUDA_SUCCESS) {
      cuDestroyExternalMemory(slot.cudaExternalMemory);
      slot.cudaExternalMemory = nullptr;
      slot.cudaExternalDevicePtr = 0;
      return false;
    }
    slot.cudaImportContext = cudaContext;
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
    VkDescriptorImageInfo srcImageDesc{};
    srcImageDesc.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    srcImageDesc.imageView = m_colorImageView;
    srcImageDesc.sampler = m_sampler;
    VkWriteDescriptorSet srcWrite{};
    srcWrite.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    srcWrite.dstSet = m_descriptorSet;
    srcWrite.dstBinding = 0;
    srcWrite.descriptorCount = 1;
    srcWrite.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    srcWrite.pImageInfo = &srcImageDesc;
    vkUpdateDescriptorSets(m_device, 1, &srcWrite, 0, nullptr);

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
      const QVector<TimelineClip> &orderedClips) {
    QVector<TranscriptTextInput> inputs;
    for (const TimelineClip &clip : orderedClips) {
      const QString transcriptPath = activeTranscriptPathForClip(clip);
      QVector<TranscriptSection> sections = m_transcriptCache.value(transcriptPath);
      if (sections.isEmpty()) {
        sections = loadTranscriptSections(transcriptPath);
        m_transcriptCache.insert(transcriptPath, sections);
      }
      const ClipFrameMapping mapping =
          clipFrameMappingForClock(clip, clock, request.renderSyncMarkers);
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
      const int64_t clipStartSample = clipTimelineStartSamples(clip);
      const int64_t clipEndSample = clipTimelineEndSamples(clip);
      if (clip.filePath.trimmed().isEmpty() ||
          clock.timelineSample < clipStartSample ||
          clock.timelineSample >= clipEndSample ||
          (!clip.hasAudio && clip.mediaType != ClipMediaType::Audio)) {
        continue;
      }
      const QString transcriptPath = activeTranscriptPathForClip(clip);
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

  VkImage m_colorImage = VK_NULL_HANDLE;
  VkDeviceMemory m_colorImageMemory = VK_NULL_HANDLE;
  VkImageView m_colorImageView = VK_NULL_HANDLE;
  VkSampler m_sampler = VK_NULL_HANDLE;

  VkDescriptorSetLayout m_descriptorSetLayout = VK_NULL_HANDLE;
  VkDescriptorPool m_descriptorPool = VK_NULL_HANDLE;
  VkDescriptorSet m_descriptorSet = VK_NULL_HANDLE;
  VkDescriptorSetLayout m_yuvComputeDescriptorSetLayout = VK_NULL_HANDLE;
  VkDescriptorPool m_yuvComputeDescriptorPool = VK_NULL_HANDLE;
  VkDescriptorSet m_yuvComputeDescriptorSet = VK_NULL_HANDLE;
  VkDescriptorSetLayout m_maskComputeDescriptorSetLayout = VK_NULL_HANDLE;
  VkDescriptorPool m_maskComputeDescriptorPool = VK_NULL_HANDLE;
  std::array<VkDescriptorSet, kMaskComputeDescriptorSetCount> m_maskComputeDescriptorSets{};
  int m_maskComputeDescriptorSetIndex = 0;

  VkRenderPass m_renderPass = VK_NULL_HANDLE;

  VkFramebuffer m_framebuffer = VK_NULL_HANDLE;
  QVector<LayerTextureSlot> m_layerSlots;
  QHash<QString, QImage> m_preparedImageCache;
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
  VkPipelineLayout m_maskPreparePipelineLayout = VK_NULL_HANDLE;
  VkPipelineLayout m_maskMorphBlurPipelineLayout = VK_NULL_HANDLE;
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
  VkShaderModule m_maskPrepareModule = VK_NULL_HANDLE;
  VkShaderModule m_maskMorphModule = VK_NULL_HANDLE;
  VkShaderModule m_maskBlurModule = VK_NULL_HANDLE;
  VkPipeline m_effectsPipeline = VK_NULL_HANDLE;
  VkPipeline m_maskPipeline = VK_NULL_HANDLE;
  VkPipeline m_nv12YPipeline = VK_NULL_HANDLE;
  VkPipeline m_nv12UvPipeline = VK_NULL_HANDLE;
  VkPipeline m_yuv420pUPipeline = VK_NULL_HANDLE;
  VkPipeline m_yuv420pVPipeline = VK_NULL_HANDLE;
  VkPipeline m_yuv420pComputePipeline = VK_NULL_HANDLE;
  VkPipeline m_maskPreparePipeline = VK_NULL_HANDLE;
  VkPipeline m_maskMorphPipeline = VK_NULL_HANDLE;
  VkPipeline m_maskBlurPipeline = VK_NULL_HANDLE;
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
  QJsonArray *skippedClips = context.skippedClips;
  QJsonObject *skippedReasonCounts = context.skippedReasonCounts;
  QJsonObject *exportFaceTransformDiagnostics =
      context.exportFaceTransformDiagnostics;
  Q_UNUSED(asyncDecoder);
  Q_UNUSED(clipStageStats);
  Q_UNUSED(skippedClips);
  Q_UNUSED(skippedReasonCounts);

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
  const bool gpuOutputOnly = (readbackMs == nullptr);
  QVector<OffscreenVulkanRendererPrivate::LayerInput> layers;
  layers.reserve((orderedClips.size() * 2) + 1);
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
  bool backgroundFilled = false;
  int decodePathMissingCount = 0;
  int decodeNullCount = 0;
  int decodeConvertFailCount = 0;
  const RenderFrameClock frameClock =
      renderFrameClockForTimelinePosition(timelineFrame);
  QRectF transcriptLayerBounds;
  OffscreenVulkanRendererPrivate::VulkanTextInputs textInputs;
  for (const TimelineClip &clip : orderedClips) {
    if (timelineFrame < clip.startFrame ||
        timelineFrame >= clip.startFrame + clip.durationFrames) {
      continue;
    }
    if (!clipVisualPlaybackEnabled(clip, request.tracks)) {
      continue;
    }
    EffectiveVisualEffects effects =
        request.bypassGrading
            ? EffectiveVisualEffects{}
            : evaluateEffectiveVisualEffectsAtPosition(
                  clip, request.tracks, static_cast<qreal>(timelineFrame),
                  request.renderSyncMarkers);
    if (!request.correctionsEnabled) {
      effects.correctionPolygons.clear();
    }
    const TimelineClip::GradingKeyframe &grade = effects.grading;
    if (grade.opacity <= 0.001) {
      continue;
    }
    if (clip.mediaType == ClipMediaType::Title) {
      if (gpuOutputOnly) {
        qWarning().noquote()
            << QStringLiteral(
                   "[vulkan-compose] skipped CPU-raster title layer in GPU-only render pipeline at frame=%1")
                   .arg(timelineFrame);
        continue;
      }
      if (clip.titleKeyframes.isEmpty()) {
        continue;
      }
      const int64_t localFrame = qMax<int64_t>(
          0, static_cast<int64_t>(std::floor(timelineFrame - clip.startFrame)));
      const EvaluatedTitle evaluatedTitle =
          evaluateTitleAtLocalFrame(clip, localFrame);
      const EvaluatedTitle title = composeTitleWithOpacity(
          evaluatedTitle, static_cast<qreal>(grade.opacity));
      if (!title.valid || title.text.isEmpty() || title.opacity <= 0.001) {
        continue;
      }

      const OverlayImage titleImage = overlayRenderBackend().renderTitleOverlay(
          request.outputSize, title, request.outputSize);

      OffscreenVulkanRendererPrivate::LayerInput layer;
      layer.overlayImage = titleImage;
      layer.sourceSize = QSize(titleImage.width, titleImage.height);
      layer.opacity = 1.0f;
      layers.push_back(layer);
      continue;
    }
    const QString decodePath = playbackMediaPathForClip(clip);
    if (decodePath.isEmpty()) {
      ++decodePathMissingCount;
      continue;
    }
    ++visualClipCandidates;
    const ClipFrameMapping frameMapping =
        clipFrameMappingForClock(clip, frameClock, request.renderSyncMarkers);
    const int64_t localFrame = frameMapping.sourceFrame;
    const qint64 decodeStart = QDateTime::currentMSecsSinceEpoch();
    const editor::FrameHandle frame = decodeRenderFrame(
        decodePath, localFrame, decoders, asyncDecoder, asyncFrameCache);
    if (decodeMs) {
      *decodeMs += (QDateTime::currentMSecsSinceEpoch() - decodeStart);
    }
    if (frame.isNull()) {
      ++decodeNullCount;
      continue;
    }
    OffscreenVulkanRendererPrivate::LayerInput layer;
    layer.frameHandle = frame;
    layer.sourceSize = frame.size();
    layer.preferHardwareDirect = frame.hasHardwareFrame();
    if (!layer.preferHardwareDirect) {
      if (gpuOutputOnly) {
        ++decodeConvertFailCount;
        continue;
      }
      const QImage layerImage =
          frame.hasCpuImage() ? frame.cpuImage() : frameHandleToCpuImage(frame);
      if (layerImage.isNull()) {
        ++decodeConvertFailCount;
        continue;
      }
      layer.image = layerImage;
      layer.sourceSize = layerImage.size();
    }
    if (clip.mediaType == ClipMediaType::Image && !layer.preferHardwareDirect) {
      layer.cacheKey = clip.id + QStringLiteral(":prepared_rgba");
    }
    const bool gpuMaskEnabled =
        clip.maskEnabled && !clip.maskFramesDir.trimmed().isEmpty() &&
        (clip.maskShowOnly || clip.maskGradeEnabled);
    if (gpuMaskEnabled) {
      const QImage mask = rawClipMaskImage(clip, localFrame);
      const QImage maskRgba = rgbaMaskImageForUpload(mask);
      if (!maskRgba.isNull()) {
        layer.maskImage = maskRgba;
        layer.maskSourceSize = maskRgba.size();
        layer.maskTextureEnabled = true;
        layer.maskShowOnly = clip.maskShowOnly;
        layer.maskGradeEnabled = clip.maskGradeEnabled;
        layer.maskInvert = clip.maskInvert;
        layer.maskErodeRadius = qRound(qMax<qreal>(0.0, clip.maskErode));
        layer.maskDilateRadius = qRound(qMax<qreal>(0.0, clip.maskDilate));
        layer.maskBlurRadius = qRound(qMax<qreal>(clip.maskFeather, clip.maskBlur));
        layer.maskOpacity = static_cast<float>(qBound<qreal>(0.0, clip.maskOpacity, 1.0));
        layer.maskBrightness = static_cast<float>(clip.maskGradeBrightness);
        layer.maskContrast = static_cast<float>(clip.maskGradeContrast);
        layer.maskSaturation = static_cast<float>(clip.maskGradeSaturation);
        TimelineClip::GradingKeyframe maskGrade;
        maskGrade.brightness = clip.maskGradeBrightness;
        maskGrade.contrast = clip.maskGradeContrast;
        maskGrade.saturation = clip.maskGradeSaturation;
        maskGrade.curvePointsR = clip.maskGradeCurvePointsR;
        maskGrade.curvePointsG = clip.maskGradeCurvePointsG;
        maskGrade.curvePointsB = clip.maskGradeCurvePointsB;
        maskGrade.curvePointsLuma = clip.maskGradeCurvePointsLuma;
        maskGrade.curveSmoothingEnabled = clip.maskGradeCurveSmoothingEnabled;
        layer.maskCurveLutRgba = curveLutBytesForGrade(maskGrade);
        layer.maskCurveLutApplied = gradingUsesCurveLut(maskGrade);
      }
    }
    const VulkanDrawEffectState layerEffects = vulkanDrawEffectStateForGrade(grade);
    layer.opacity = layerEffects.opacity;
    layer.brightness = layerEffects.brightness;
    layer.contrast = layerEffects.contrast;
    layer.saturation = layerEffects.saturation;
    layer.shadows[0] = layerEffects.shadows[0];
    layer.shadows[1] = layerEffects.shadows[1];
    layer.shadows[2] = layerEffects.shadows[2];
    layer.shadows[3] = layerEffects.shadows[3];
    layer.midtones[0] = layerEffects.midtones[0];
    layer.midtones[1] = layerEffects.midtones[1];
    layer.midtones[2] = layerEffects.midtones[2];
    layer.midtones[3] = layerEffects.midtones[3];
    layer.highlights[0] = layerEffects.highlights[0];
    layer.highlights[1] = layerEffects.highlights[1];
    layer.highlights[2] = layerEffects.highlights[2];
    layer.highlights[3] = layerEffects.highlights[3];
    layer.curveLutApplied = gradingUsesCurveLut(grade);
    layer.shadows[3] = layer.curveLutApplied
                           ? kVulkanEffectModeCurve
                           : kVulkanEffectModeNormal;
    layer.curveLutRgba = curveLutBytesForGrade(grade);
    QJsonObject transformDiagnostics;
    const TimelineClip::TransformKeyframe transform =
        evaluateClipRenderTransformAtPosition(
            clip,
            static_cast<qreal>(timelineFrame),
            request.renderSyncMarkers,
            request.outputSize,
            &transformDiagnostics);
    const QSize sourceSize = clip.sourceFrameSize.isValid()
        ? clip.sourceFrameSize
        : (layer.sourceSize.isValid() ? layer.sourceSize : layer.image.size());
    const QRectF fitted = fitRectF(sourceSize, request.outputSize);
    QPointF exportVideoTranslation(transform.translationX, transform.translationY);
    PreviewClipGeometry layerGeometry = PreviewViewTransform::clipGeometry(
        fitted,
        QPointF(1.0, 1.0),
        exportVideoTranslation,
        transform.rotation,
        QPointF(transform.scaleX, transform.scaleY));
    if (exportFaceTransformDiagnostics && clip.speakerFramingEnabled) {
      transformDiagnostics.insert(QStringLiteral("clip_id"), clip.id);
      transformDiagnostics.insert(QStringLiteral("clip_label"), clip.label);
      transformDiagnostics.insert(QStringLiteral("timeline_frame_position"), timelineFrame);
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
    if (!backgroundFilled &&
        shouldDrawBlurredFillBackground(sourceSize, request.outputSize)) {
      OffscreenVulkanRendererPrivate::LayerInput backgroundLayer;
      backgroundLayer.sourceSize = request.outputSize;
      const BackgroundFillEffect fillEffect = request.backgroundFillEffect;
      const QRectF outputRect(QPointF(0.0, 0.0), QSizeF(request.outputSize));
      const QRectF sourceRectNorm(
          (layerGeometry.bounds.left() - outputRect.left()) / qMax<qreal>(1.0, outputRect.width()),
          (layerGeometry.bounds.top() - outputRect.top()) / qMax<qreal>(1.0, outputRect.height()),
          layerGeometry.bounds.width() / qMax<qreal>(1.0, outputRect.width()),
          layerGeometry.bounds.height() / qMax<qreal>(1.0, outputRect.height()));
      const VulkanDrawEffectState backgroundEffects =
          vulkanBackgroundFillEffectState(
              fillEffect,
              layerEffects,
              static_cast<float>(request.backgroundFillOpacity),
              static_cast<float>(request.backgroundFillBrightness),
              static_cast<float>(request.backgroundFillSaturation),
              request.backgroundFillEdgePixels,
              request.backgroundFillEdgeProgressive,
              static_cast<float>(request.backgroundFillEdgePower),
              sourceRectNorm);
      backgroundLayer.opacity = backgroundEffects.opacity;
      backgroundLayer.brightness = backgroundEffects.brightness;
      backgroundLayer.contrast = backgroundEffects.contrast;
      backgroundLayer.saturation = backgroundEffects.saturation;
      backgroundLayer.shadows[0] = backgroundEffects.shadows[0];
      backgroundLayer.shadows[1] = backgroundEffects.shadows[1];
      backgroundLayer.shadows[2] = backgroundEffects.shadows[2];
      backgroundLayer.shadows[3] = backgroundEffects.shadows[3];
      backgroundLayer.midtones[0] = backgroundEffects.midtones[0];
      backgroundLayer.midtones[1] = backgroundEffects.midtones[1];
      backgroundLayer.midtones[2] = backgroundEffects.midtones[2];
      backgroundLayer.midtones[3] = backgroundEffects.midtones[3];
      backgroundLayer.highlights[0] = backgroundEffects.highlights[0];
      backgroundLayer.highlights[1] = backgroundEffects.highlights[1];
      backgroundLayer.highlights[2] = backgroundEffects.highlights[2];
      backgroundLayer.highlights[3] = backgroundEffects.highlights[3];
      backgroundLayer.frameHandle = frame;
      backgroundLayer.sourceSize = sourceSize;
      backgroundLayer.preferHardwareDirect = frame.hasHardwareFrame();

      PreviewClipGeometry backgroundGeometry =
          fillEffect == BackgroundFillEffect::EdgeStretch
              ? PreviewViewTransform::clipGeometry(outputRect,
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
          !backgroundLayer.frameHandle.isNull()) {
        layers.push_back(backgroundLayer);
        backgroundFilled = true;
      }
    }
    layers.push_back(layer);
    ++visualLayersResolved;
  }
  if (hasTranscriptCandidate) {
    textInputs.transcripts = d->buildTranscriptTextInputs(
        request.outputSize, request,
        frameClock,
        transcriptOverlayClips);
    for (const OffscreenVulkanRendererPrivate::TranscriptTextInput &text : textInputs.transcripts) {
      transcriptLayerBounds = transcriptLayerBounds.united(text.outputRect);
    }
  }
  textInputs.hasSpeakerLabel = d->buildSpeakerLabelSpec(
      request,
      frameClock,
      orderedClips,
      &textInputs.speakerLabel);
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
  if (visualClipCandidates > 0 && visualLayersResolved == 0) {
    qWarning().noquote() << QStringLiteral(
                                "[vulkan-compose] no visual layers resolved at "
                                "frame=%1 candidates=%2 decode_path_missing=%3 "
                                "decode_null=%4 convert_fail=%5")
                                .arg(timelineFrame)
                                .arg(visualClipCandidates)
                                .arg(decodePathMissingCount)
                                .arg(decodeNullCount)
                                .arg(decodeConvertFailCount);
    return QImage();
  }
  if (layers.isEmpty()) {
    if (gpuOutputOnly) {
      qWarning().noquote()
          << QStringLiteral(
                 "[vulkan-compose] GPU-only render pipeline has no drawable GPU layers at frame=%1")
                 .arg(timelineFrame);
      return QImage();
    }
    OffscreenVulkanRendererPrivate::LayerInput black;
    black.image = QImage(request.outputSize, QImage::Format_RGBA8888);
    black.image.fill(Qt::black);
    black.sourceSize = black.image.size();
    black.opacity = 1.0f;
    layers.push_back(black);
  }
  const qint64 renderStartMs = QDateTime::currentMSecsSinceEpoch();
  const bool shouldReadbackToImage = (readbackMs != nullptr);
  const QImage output = d->renderFrameFromLayers(layers, textInputs, shouldReadbackToImage);
  if (compositeMs) {
    *compositeMs = (QDateTime::currentMSecsSinceEpoch() - renderStartMs);
  }
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
  output->cpuImage = renderFrame(frameContext);
  QString error;
  if (!lastRenderedVulkanFrame(&output->vulkanFrame, &error)) {
    output->vulkanFrame.valid = false;
    return readbackToCpuImage ? !output->cpuImage.isNull() : false;
  }
  return readbackToCpuImage ? !output->cpuImage.isNull()
                            : output->vulkanFrame.valid;
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
