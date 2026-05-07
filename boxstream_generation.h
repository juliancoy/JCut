#pragma once

#include "boxstream_runtime.h"
#include "decoder_context.h"
#include "editor_shared.h"
#include "frame_handle.h"
#include "render_internal.h"
#include "vulkan_zero_copy_face_detector.h"

#include <QDir>
#include <QElapsedTimer>
#include <QFileInfo>
#include <QImage>
#include <QJsonArray>
#include <QJsonObject>
#include <QProcess>
#include <QRectF>
#include <QString>
#include <QVector>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <memory>
#include <vector>

#if JCUT_HAVE_OPENCV
#include <opencv2/core.hpp>
#include <opencv2/dnn.hpp>
#include <opencv2/imgproc.hpp>
#include <opencv2/objdetect.hpp>
#if JCUT_HAVE_OPENCV_CONTRIB
#include <opencv2/tracking.hpp>
#include <opencv2/tracking/tracking_legacy.hpp>
#endif

#include <vulkan/vulkan.h>
#endif

namespace jcut::boxstream {
struct BoxstreamSmoothingSettings {
    bool smoothTranslation = false;
    bool smoothScale = false;
};
inline BoxstreamSmoothingSettings g_boxstreamSmoothingSettings;
struct BoxstreamContribTrackingSettings {
    int redetectStrideSamples = 5;
    bool allowTrackerOnlyPropagation = true;
};
inline BoxstreamContribTrackingSettings g_boxstreamContribTrackingSettings;

enum class BoxstreamDetectorPreset {
    HaarBalanced = 0,
    HaarSmallFaces = 1,
    HaarPrecision = 2,
    LbpSmallFaces = 3,
    PythonCompatible = 4,
    DnnAuto = 5,
    PythonLegacy = 6,
    LocalHybrid = 7,
    DockerDnn = 8,
    DockerInsightFace = 9,
    DockerYoloFace = 10,
    DockerMtcnn = 11,
    DockerHybrid = 12,
    Sam3Face = 13,
    ContribCsrt = 14,
    ContribKcf = 15,
    NativeHybridCpu = 16,
    NativeHybridVulkan = 17,
    NativeVulkanDnn = 18,
    NativeCudaDnn = 19,
    ScrfdNcnnVulkan = 20
};

inline bool isDnnBoxstreamPreset(BoxstreamDetectorPreset preset)
{
    return preset == BoxstreamDetectorPreset::DnnAuto ||
           preset == BoxstreamDetectorPreset::NativeVulkanDnn ||
           preset == BoxstreamDetectorPreset::NativeCudaDnn ||
           preset == BoxstreamDetectorPreset::ScrfdNcnnVulkan;
}

inline bool isRelaxedDnnBoxstreamPreset(BoxstreamDetectorPreset preset)
{
    return preset == BoxstreamDetectorPreset::NativeVulkanDnn ||
           preset == BoxstreamDetectorPreset::ScrfdNcnnVulkan;
}

inline QString formatEtaSeconds(double secondsRemaining)
{
    if (!std::isfinite(secondsRemaining) || secondsRemaining < 0.0) {
        return QStringLiteral("ETA --:--");
    }
    const int total = qMax(0, static_cast<int>(std::round(secondsRemaining)));
    const int mm = total / 60;
    const int ss = total % 60;
    return QStringLiteral("ETA %1:%2")
        .arg(mm, 2, 10, QChar('0'))
        .arg(ss, 2, 10, QChar('0'));
}

inline QString findCascadeFile(const QString& name)
{
    const QStringList roots = {
        QDir(QDir::currentPath()).absoluteFilePath(QStringLiteral("external/opencv/data/haarcascades")),
        QStringLiteral("/usr/share/opencv4/haarcascades"),
        QStringLiteral("/usr/local/share/opencv4/haarcascades")
    };
    for (const QString& root : roots) {
        const QString path = QDir(root).filePath(name);
        if (QFileInfo::exists(path)) {
            return path;
        }
    }
    return {};
}

#if JCUT_HAVE_OPENCV
struct VulkanBoxstreamFrameProvider {
    std::unique_ptr<render_detail::OffscreenVulkanRenderer> renderer;
    QHash<QString, editor::DecoderContext*> decoders;
    QHash<render_detail::RenderAsyncFrameKey, editor::FrameHandle> asyncFrameCache;
    QSize outputSize;
    bool initialized = false;
    bool failed = false;
    QString failureReason;

    ~VulkanBoxstreamFrameProvider()
    {
        qDeleteAll(decoders);
        decoders.clear();
    }

    bool ensureInitialized(const QSize& size)
    {
        const QSize normalized(qMax(16, size.width()), qMax(16, size.height()));
        if (initialized && outputSize == normalized) {
            return true;
        }
        renderer = std::make_unique<render_detail::OffscreenVulkanRenderer>();
        qDeleteAll(decoders);
        decoders.clear();
        asyncFrameCache.clear();
        outputSize = normalized;
        QString error;
        if (!renderer->initialize(outputSize, &error)) {
            renderer.reset();
            initialized = false;
            failed = true;
            failureReason = error.isEmpty()
                ? QStringLiteral("Vulkan FaceStream renderer initialization failed.")
                : error;
            return false;
        }
        initialized = true;
        failed = false;
        failureReason.clear();
        return true;
    }
};

inline QImage renderBoxstreamFrameWithVulkan(VulkanBoxstreamFrameProvider* provider,
                                      const TimelineClip& sourceClip,
                                      const QString& mediaPath,
                                      int64_t timelineFrame,
                                      int64_t sourceFrame,
                                      const QSize& outputSize)
{
    if (!provider || !provider->ensureInitialized(outputSize)) {
        return {};
    }

    TimelineClip clip = sourceClip;
    clip.id = sourceClip.id.trimmed().isEmpty()
        ? QStringLiteral("boxstream-vulkan-source")
        : sourceClip.id;
    clip.filePath = mediaPath;
    clip.proxyPath.clear();
    clip.useProxy = false;
    clip.mediaType = ClipMediaType::Video;
    clip.videoEnabled = true;
    clip.audioEnabled = false;
    clip.hasAudio = false;
    clip.startFrame = timelineFrame;
    clip.startSubframeSamples = 0;
    clip.sourceInFrame = qMax<int64_t>(0, sourceFrame);
    clip.sourceInSubframeSamples = 0;
    clip.durationFrames = 1;
    clip.sourceDurationFrames = qMax<int64_t>(clip.sourceInFrame + 1, sourceClip.sourceDurationFrames);
    clip.playbackRate = 1.0;
    clip.trackIndex = 0;
    clip.brightness = 0.0;
    clip.contrast = 1.0;
    clip.saturation = 1.0;
    clip.opacity = 1.0;
    clip.baseTranslationX = 0.0;
    clip.baseTranslationY = 0.0;
    clip.baseRotation = 0.0;
    clip.baseScaleX = 1.0;
    clip.baseScaleY = 1.0;
    clip.speakerFramingEnabled = false;
    clip.transformKeyframes.clear();
    clip.speakerFramingKeyframes.clear();
    clip.gradingKeyframes.clear();
    clip.opacityKeyframes.clear();
    clip.titleKeyframes.clear();
    clip.transcriptOverlay.enabled = false;
    clip.correctionPolygons.clear();

    RenderRequest request;
    request.outputPath = QStringLiteral("boxstream://vulkan");
    request.outputFormat = QStringLiteral("boxstream-preview");
    request.outputSize = outputSize;
    request.bypassGrading = true;
    request.correctionsEnabled = false;
    request.clips = QVector<TimelineClip>{clip};
    request.tracks = QVector<TimelineTrack>{TimelineTrack{}};
    request.exportStartFrame = timelineFrame;
    request.exportEndFrame = timelineFrame;

    qint64 decodeMs = 0;
    qint64 textureMs = 0;
    qint64 compositeMs = 0;
    qint64 readbackMs = 0;
    QImage frame = provider->renderer->renderFrame(request,
                                                   timelineFrame,
                                                   provider->decoders,
                                                   nullptr,
                                                   &provider->asyncFrameCache,
                                                   QVector<TimelineClip>{clip},
                                                   nullptr,
                                                   &decodeMs,
                                                   &textureMs,
                                                   &compositeMs,
                                                   &readbackMs,
                                                   nullptr,
                                                   nullptr);
    if (frame.isNull()) {
        provider->failed = true;
        provider->failureReason = QStringLiteral("Vulkan FaceStream frame render returned null.");
    }
    return frame;
}

struct ContinuityTrackState {
    int id = -1;
    cv::Rect box;
    int64_t lastTimelineFrame = -1;
    QJsonArray detections;
#if JCUT_HAVE_OPENCV_CONTRIB
    cv::Ptr<cv::legacy::Tracker> tracker;
#endif
};

struct WeightedDetection {
    cv::Rect box;
    double weight = 0.0;
};

struct DnnFaceDetectorRuntime {
    cv::dnn::Net net;
    bool loaded = false;
    bool usingCuda = false;
    bool usingVulkan = false;
    bool cpuFallbackApplied = false;
};

inline uint32_t findVulkanMemoryType(VkPhysicalDevice physicalDevice,
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

class NativeVulkanFacePreprocessRuntime {
public:
    struct DetectionCandidate {
        QRectF normalizedBox;
        float confidence = 0.0f;
    };

    ~NativeVulkanFacePreprocessRuntime() { release(); }

    bool initialize(QString* error)
    {
        if (m_initialized) {
            return true;
        }
        VkApplicationInfo app{};
        app.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
        app.pApplicationName = "JCut FaceStream Vulkan Face Preprocess";
        app.apiVersion = VK_API_VERSION_1_1;

        VkInstanceCreateInfo instanceInfo{};
        instanceInfo.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
        instanceInfo.pApplicationInfo = &app;
        if (vkCreateInstance(&instanceInfo, nullptr, &m_instance) != VK_SUCCESS) {
            setError(error, QStringLiteral("Failed to create Vulkan instance for FaceStream preprocessing."));
            return false;
        }

        uint32_t deviceCount = 0;
        vkEnumeratePhysicalDevices(m_instance, &deviceCount, nullptr);
        if (deviceCount == 0) {
            setError(error, QStringLiteral("No Vulkan physical devices found for FaceStream preprocessing."));
            return false;
        }
        QVector<VkPhysicalDevice> devices(static_cast<int>(deviceCount));
        vkEnumeratePhysicalDevices(m_instance, &deviceCount, devices.data());

        int bestScore = -10000;
        for (VkPhysicalDevice candidate : devices) {
            VkPhysicalDeviceProperties props{};
            vkGetPhysicalDeviceProperties(candidate, &props);
            uint32_t familyCount = 0;
            vkGetPhysicalDeviceQueueFamilyProperties(candidate, &familyCount, nullptr);
            QVector<VkQueueFamilyProperties> families(static_cast<int>(familyCount));
            vkGetPhysicalDeviceQueueFamilyProperties(candidate, &familyCount, families.data());
            for (uint32_t i = 0; i < familyCount; ++i) {
                if (!(families[static_cast<int>(i)].queueFlags & VK_QUEUE_COMPUTE_BIT)) {
                    continue;
                }
                int score = 0;
                if (props.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU) {
                    score += 1000;
                } else if (props.deviceType == VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU) {
                    score += 500;
                } else if (props.deviceType == VK_PHYSICAL_DEVICE_TYPE_CPU) {
                    score -= 1000;
                }
                if (score > bestScore) {
                    bestScore = score;
                    m_physicalDevice = candidate;
                    m_queueFamilyIndex = i;
                }
            }
        }
        if (m_physicalDevice == VK_NULL_HANDLE || m_queueFamilyIndex == UINT32_MAX) {
            setError(error, QStringLiteral("No Vulkan compute queue found for FaceStream preprocessing."));
            return false;
        }

        const float priority = 1.0f;
        VkDeviceQueueCreateInfo queueInfo{};
        queueInfo.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
        queueInfo.queueFamilyIndex = m_queueFamilyIndex;
        queueInfo.queueCount = 1;
        queueInfo.pQueuePriorities = &priority;

        VkDeviceCreateInfo deviceInfo{};
        deviceInfo.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
        deviceInfo.queueCreateInfoCount = 1;
        deviceInfo.pQueueCreateInfos = &queueInfo;
        if (vkCreateDevice(m_physicalDevice, &deviceInfo, nullptr, &m_device) != VK_SUCCESS) {
            setError(error, QStringLiteral("Failed to create Vulkan device for FaceStream preprocessing."));
            return false;
        }
        vkGetDeviceQueue(m_device, m_queueFamilyIndex, 0, &m_queue);

        VkCommandPoolCreateInfo poolInfo{};
        poolInfo.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
        poolInfo.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
        poolInfo.queueFamilyIndex = m_queueFamilyIndex;
        if (vkCreateCommandPool(m_device, &poolInfo, nullptr, &m_commandPool) != VK_SUCCESS) {
            setError(error, QStringLiteral("Failed to create Vulkan command pool for FaceStream preprocessing."));
            return false;
        }
        VkCommandBufferAllocateInfo allocInfo{};
        allocInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
        allocInfo.commandPool = m_commandPool;
        allocInfo.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        allocInfo.commandBufferCount = 1;
        if (vkAllocateCommandBuffers(m_device, &allocInfo, &m_commandBuffer) != VK_SUCCESS) {
            setError(error, QStringLiteral("Failed to allocate Vulkan command buffer for FaceStream preprocessing."));
            return false;
        }
        VkFenceCreateInfo fenceInfo{};
        fenceInfo.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
        if (vkCreateFence(m_device, &fenceInfo, nullptr, &m_fence) != VK_SUCCESS) {
            setError(error, QStringLiteral("Failed to create Vulkan fence for FaceStream preprocessing."));
            return false;
        }

        jcut::vulkan_detector::VulkanDeviceContext context{
            m_physicalDevice,
            m_device,
            m_queue,
            m_queueFamilyIndex
        };
        if (!m_detector.initialize(context, error)) {
            return false;
        }
        m_tensorBytes = m_detector.tensorSpec().byteSize();
        m_initialized = true;
        return true;
    }

    void release()
    {
        m_detector.release();
        if (m_device != VK_NULL_HANDLE) {
            vkDeviceWaitIdle(m_device);
            if (m_fence) vkDestroyFence(m_device, m_fence, nullptr);
            if (m_commandPool) vkDestroyCommandPool(m_device, m_commandPool, nullptr);
            if (m_imageView) vkDestroyImageView(m_device, m_imageView, nullptr);
            if (m_image) vkDestroyImage(m_device, m_image, nullptr);
            if (m_imageMemory) vkFreeMemory(m_device, m_imageMemory, nullptr);
            destroyBuffer(m_tensorBuffer, m_tensorMemory);
            destroyBuffer(m_detectionBuffer, m_detectionMemory);
            destroyBuffer(m_stagingBuffer, m_stagingMemory);
            vkDestroyDevice(m_device, nullptr);
        }
        if (m_instance != VK_NULL_HANDLE) {
            vkDestroyInstance(m_instance, nullptr);
        }
        m_instance = VK_NULL_HANDLE;
        m_physicalDevice = VK_NULL_HANDLE;
        m_device = VK_NULL_HANDLE;
        m_queue = VK_NULL_HANDLE;
        m_queueFamilyIndex = UINT32_MAX;
        m_commandPool = VK_NULL_HANDLE;
        m_commandBuffer = VK_NULL_HANDLE;
        m_fence = VK_NULL_HANDLE;
        m_image = VK_NULL_HANDLE;
        m_imageMemory = VK_NULL_HANDLE;
        m_imageView = VK_NULL_HANDLE;
        m_imageLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        m_imageSize = QSize();
        m_stagingBuffer = VK_NULL_HANDLE;
        m_stagingMemory = VK_NULL_HANDLE;
        m_stagingSize = 0;
        m_tensorBuffer = VK_NULL_HANDLE;
        m_tensorMemory = VK_NULL_HANDLE;
        m_tensorSize = 0;
        m_detectionBuffer = VK_NULL_HANDLE;
        m_detectionMemory = VK_NULL_HANDLE;
        m_detectionBytes = 0;
        m_tensorBytes = 0;
        m_initialized = false;
    }

    bool detectCpuImage(const QImage& image,
                        QVector<DetectionCandidate>* detections,
                        double* inferenceMs,
                        QString* error,
                        float confidenceThreshold = 0.28f)
    {
        if (!m_initialized && !initialize(error)) {
            return false;
        }
        if (detections) {
            detections->clear();
        }
        const QImage rgba = image.convertToFormat(QImage::Format_RGBA8888);
        if (!ensureResources(rgba.size(), error)) {
            return false;
        }

        const QElapsedTimer timer = [] {
            QElapsedTimer t;
            t.start();
            return t;
        }();

        void* mapped = nullptr;
        const VkDeviceSize bytes = static_cast<VkDeviceSize>(rgba.width()) * rgba.height() * 4;
        if (vkMapMemory(m_device, m_stagingMemory, 0, bytes, 0, &mapped) != VK_SUCCESS) {
            setError(error, QStringLiteral("Failed to map Vulkan staging buffer for FaceStream preprocessing."));
            return false;
        }
        for (int y = 0; y < rgba.height(); ++y) {
            std::memcpy(static_cast<uchar*>(mapped) + y * rgba.width() * 4,
                        rgba.constScanLine(y),
                        static_cast<size_t>(rgba.width() * 4));
        }
        vkUnmapMemory(m_device, m_stagingMemory);

        vkResetFences(m_device, 1, &m_fence);
        vkResetCommandBuffer(m_commandBuffer, 0);
        VkCommandBufferBeginInfo begin{};
        begin.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
        begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        if (vkBeginCommandBuffer(m_commandBuffer, &begin) != VK_SUCCESS) {
            setError(error, QStringLiteral("Failed to begin Vulkan upload command buffer for FaceStream preprocessing."));
            return false;
        }
        transitionImage(m_imageLayout, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
        VkBufferImageCopy copy{};
        copy.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        copy.imageSubresource.layerCount = 1;
        copy.imageExtent = {static_cast<uint32_t>(rgba.width()), static_cast<uint32_t>(rgba.height()), 1};
        vkCmdCopyBufferToImage(m_commandBuffer,
                               m_stagingBuffer,
                               m_image,
                               VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                               1,
                               &copy);
        transitionImage(VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
        if (vkEndCommandBuffer(m_commandBuffer) != VK_SUCCESS) {
            setError(error, QStringLiteral("Failed to end Vulkan upload command buffer for FaceStream preprocessing."));
            return false;
        }
        VkSubmitInfo submit{};
        submit.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
        submit.commandBufferCount = 1;
        submit.pCommandBuffers = &m_commandBuffer;
        if (vkQueueSubmit(m_queue, 1, &submit, m_fence) != VK_SUCCESS) {
            setError(error, QStringLiteral("Failed to submit Vulkan upload for FaceStream preprocessing."));
            return false;
        }
        if (vkWaitForFences(m_device, 1, &m_fence, VK_TRUE, 5'000'000'000ull) != VK_SUCCESS) {
            setError(error, QStringLiteral("Timed out waiting for Vulkan upload for FaceStream preprocessing."));
            return false;
        }
        m_imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

        jcut::vulkan_detector::VulkanExternalImage source;
        source.imageView = m_imageView;
        source.imageLayout = m_imageLayout;
        source.size = m_imageSize;
        jcut::vulkan_detector::VulkanTensorBuffer tensor{m_tensorBuffer, m_tensorBytes};
        if (!m_detector.preprocessToTensor(source, tensor, error)) {
            return false;
        }

        jcut::vulkan_detector::VulkanTensorBuffer detectionBuffer{m_detectionBuffer, m_detectionBytes};
        const float boundedThreshold = qBound(0.0f, confidenceThreshold, 1.0f);
        if (!m_detector.inferFromTensor(tensor, detectionBuffer, m_maxDetections, boundedThreshold, error)) {
            return false;
        }

        if (!readDetections(detections, boundedThreshold, error)) {
            return false;
        }
        if (inferenceMs) {
            *inferenceMs = static_cast<double>(timer.nsecsElapsed()) / 1'000'000.0;
        }
        return true;
    }

    QString backendId() const { return m_detector.backendId(); }

private:
    void setError(QString* error, const QString& text)
    {
        if (error) {
            *error = text;
        }
    }

    bool createBuffer(VkDeviceSize size,
                      VkBufferUsageFlags usage,
                      VkMemoryPropertyFlags properties,
                      VkBuffer* buffer,
                      VkDeviceMemory* memory,
                      QString* error)
    {
        VkBufferCreateInfo info{};
        info.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
        info.size = size;
        info.usage = usage;
        info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
        if (vkCreateBuffer(m_device, &info, nullptr, buffer) != VK_SUCCESS) {
            setError(error, QStringLiteral("Failed to create Vulkan buffer for FaceStream preprocessing."));
            return false;
        }
        VkMemoryRequirements req{};
        vkGetBufferMemoryRequirements(m_device, *buffer, &req);
        const uint32_t type = findVulkanMemoryType(m_physicalDevice, req.memoryTypeBits, properties);
        if (type == UINT32_MAX) {
            setError(error, QStringLiteral("No Vulkan memory type for FaceStream preprocessing buffer."));
            return false;
        }
        VkMemoryAllocateInfo alloc{};
        alloc.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
        alloc.allocationSize = req.size;
        alloc.memoryTypeIndex = type;
        if (vkAllocateMemory(m_device, &alloc, nullptr, memory) != VK_SUCCESS) {
            setError(error, QStringLiteral("Failed to allocate Vulkan buffer memory for FaceStream preprocessing."));
            return false;
        }
        vkBindBufferMemory(m_device, *buffer, *memory, 0);
        return true;
    }

    void destroyBuffer(VkBuffer& buffer, VkDeviceMemory& memory)
    {
        if (buffer) vkDestroyBuffer(m_device, buffer, nullptr);
        if (memory) vkFreeMemory(m_device, memory, nullptr);
        buffer = VK_NULL_HANDLE;
        memory = VK_NULL_HANDLE;
    }

    bool ensureResources(const QSize& size, QString* error)
    {
        if (m_image != VK_NULL_HANDLE && m_imageSize == size) {
            return true;
        }
        if (m_imageView) vkDestroyImageView(m_device, m_imageView, nullptr);
        if (m_image) vkDestroyImage(m_device, m_image, nullptr);
        if (m_imageMemory) vkFreeMemory(m_device, m_imageMemory, nullptr);
        m_imageView = VK_NULL_HANDLE;
        m_image = VK_NULL_HANDLE;
        m_imageMemory = VK_NULL_HANDLE;
        m_imageLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        m_imageSize = size;

        const VkDeviceSize stagingBytes = static_cast<VkDeviceSize>(size.width()) * size.height() * 4;
        if (stagingBytes > m_stagingSize) {
            destroyBuffer(m_stagingBuffer, m_stagingMemory);
            if (!createBuffer(stagingBytes,
                              VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
                              VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                              &m_stagingBuffer,
                              &m_stagingMemory,
                              error)) {
                return false;
            }
            m_stagingSize = stagingBytes;
        }
        if (m_tensorBytes > m_tensorSize) {
            destroyBuffer(m_tensorBuffer, m_tensorMemory);
            if (!createBuffer(m_tensorBytes,
                              VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
                              VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT,
                              &m_tensorBuffer,
                              &m_tensorMemory,
                              error)) {
                return false;
            }
            m_tensorSize = m_tensorBytes;
        }
        const VkDeviceSize detectionBytes = static_cast<VkDeviceSize>(16 + (m_maxDetections * 32));
        if (detectionBytes > m_detectionBytes) {
            destroyBuffer(m_detectionBuffer, m_detectionMemory);
            if (!createBuffer(detectionBytes,
                              VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                              VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                              &m_detectionBuffer,
                              &m_detectionMemory,
                              error)) {
                return false;
            }
            m_detectionBytes = detectionBytes;
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
        if (vkCreateImage(m_device, &imageInfo, nullptr, &m_image) != VK_SUCCESS) {
            setError(error, QStringLiteral("Failed to create Vulkan image for FaceStream preprocessing."));
            return false;
        }
        VkMemoryRequirements req{};
        vkGetImageMemoryRequirements(m_device, m_image, &req);
        const uint32_t type = findVulkanMemoryType(
            m_physicalDevice, req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
        if (type == UINT32_MAX) {
            setError(error, QStringLiteral("No Vulkan memory type for FaceStream preprocessing image."));
            return false;
        }
        VkMemoryAllocateInfo alloc{};
        alloc.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
        alloc.allocationSize = req.size;
        alloc.memoryTypeIndex = type;
        if (vkAllocateMemory(m_device, &alloc, nullptr, &m_imageMemory) != VK_SUCCESS) {
            setError(error, QStringLiteral("Failed to allocate Vulkan image memory for FaceStream preprocessing."));
            return false;
        }
        vkBindImageMemory(m_device, m_image, m_imageMemory, 0);

        VkImageViewCreateInfo viewInfo{};
        viewInfo.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
        viewInfo.image = m_image;
        viewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
        viewInfo.format = VK_FORMAT_R8G8B8A8_UNORM;
        viewInfo.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        viewInfo.subresourceRange.levelCount = 1;
        viewInfo.subresourceRange.layerCount = 1;
        if (vkCreateImageView(m_device, &viewInfo, nullptr, &m_imageView) != VK_SUCCESS) {
            setError(error, QStringLiteral("Failed to create Vulkan image view for FaceStream preprocessing."));
            return false;
        }
        return true;
    }

    void transitionImage(VkImageLayout oldLayout, VkImageLayout newLayout)
    {
        VkImageMemoryBarrier barrier{};
        barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
        barrier.oldLayout = oldLayout;
        barrier.newLayout = newLayout;
        barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        barrier.image = m_image;
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
        vkCmdPipelineBarrier(m_commandBuffer, src, dst, 0, 0, nullptr, 0, nullptr, 1, &barrier);
    }

    bool readDetections(QVector<DetectionCandidate>* detections, float confidenceThreshold, QString* error)
    {
        if (!detections) {
            return true;
        }
        void* mapped = nullptr;
        if (vkMapMemory(m_device, m_detectionMemory, 0, m_detectionBytes, 0, &mapped) != VK_SUCCESS) {
            setError(error, QStringLiteral("Failed to map Vulkan face detection output."));
            return false;
        }
        const uint32_t rawCount = *static_cast<const uint32_t*>(mapped);
        const uint32_t count = qMin<uint32_t>(rawCount, static_cast<uint32_t>(m_maxDetections));
        const float* det = reinterpret_cast<const float*>(static_cast<const uchar*>(mapped) + 16);
        detections->reserve(static_cast<int>(count));
        for (uint32_t i = 0; i < count; ++i) {
            const float x = det[i * 8 + 0];
            const float y = det[i * 8 + 1];
            const float w = det[i * 8 + 2];
            const float h = det[i * 8 + 3];
            const float confidence = det[i * 8 + 4];
            if (!std::isfinite(x) || !std::isfinite(y) || !std::isfinite(w) || !std::isfinite(h) ||
                confidence < confidenceThreshold || w <= 0.04f || h <= 0.04f) {
                continue;
            }
            DetectionCandidate candidate;
            candidate.normalizedBox = QRectF(qBound(0.0, static_cast<double>(x), 1.0),
                                             qBound(0.0, static_cast<double>(y), 1.0),
                                             qBound(0.0, static_cast<double>(w), 1.0),
                                             qBound(0.0, static_cast<double>(h), 1.0));
            candidate.confidence = confidence;
            detections->push_back(candidate);
        }
        vkUnmapMemory(m_device, m_detectionMemory);
        std::sort(detections->begin(), detections->end(), [](const DetectionCandidate& a, const DetectionCandidate& b) {
            return a.confidence > b.confidence;
        });
        return true;
    }

    jcut::vulkan_detector::VulkanZeroCopyFaceDetector m_detector;
    VkInstance m_instance = VK_NULL_HANDLE;
    VkPhysicalDevice m_physicalDevice = VK_NULL_HANDLE;
    VkDevice m_device = VK_NULL_HANDLE;
    VkQueue m_queue = VK_NULL_HANDLE;
    uint32_t m_queueFamilyIndex = UINT32_MAX;
    VkCommandPool m_commandPool = VK_NULL_HANDLE;
    VkCommandBuffer m_commandBuffer = VK_NULL_HANDLE;
    VkFence m_fence = VK_NULL_HANDLE;
    VkImage m_image = VK_NULL_HANDLE;
    VkDeviceMemory m_imageMemory = VK_NULL_HANDLE;
    VkImageView m_imageView = VK_NULL_HANDLE;
    VkImageLayout m_imageLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    QSize m_imageSize;
    VkBuffer m_stagingBuffer = VK_NULL_HANDLE;
    VkDeviceMemory m_stagingMemory = VK_NULL_HANDLE;
    VkDeviceSize m_stagingSize = 0;
    VkBuffer m_tensorBuffer = VK_NULL_HANDLE;
    VkDeviceMemory m_tensorMemory = VK_NULL_HANDLE;
    VkDeviceSize m_tensorSize = 0;
    VkBuffer m_detectionBuffer = VK_NULL_HANDLE;
    VkDeviceMemory m_detectionMemory = VK_NULL_HANDLE;
    VkDeviceSize m_detectionBytes = 0;
    VkDeviceSize m_tensorBytes = 0;
    int m_maxDetections = 256;
    bool m_initialized = false;
};

inline double iou(const cv::Rect& a, const cv::Rect& b)
{
    const int x1 = qMax(a.x, b.x);
    const int y1 = qMax(a.y, b.y);
    const int x2 = qMin(a.x + a.width, b.x + b.width);
    const int y2 = qMin(a.y + a.height, b.y + b.height);
    const int w = qMax(0, x2 - x1);
    const int h = qMax(0, y2 - y1);
    if (w <= 0 || h <= 0) {
        return 0.0;
    }
    const double inter = static_cast<double>(w) * static_cast<double>(h);
    const double uni = static_cast<double>(a.area() + b.area()) - inter;
    return uni > 0.0 ? (inter / uni) : 0.0;
}

inline cv::Mat qImageToBgrMat(const QImage& image)
{
    const QImage rgb = image.convertToFormat(QImage::Format_RGB888);
    cv::Mat rgbMat(rgb.height(), rgb.width(), CV_8UC3,
                   const_cast<uchar*>(rgb.constBits()),
                   static_cast<size_t>(rgb.bytesPerLine()));
    cv::Mat bgr;
    cv::cvtColor(rgbMat, bgr, cv::COLOR_RGB2BGR);
    return bgr;
}

inline QImage buildScanPreview(const QImage& source, const std::vector<cv::Rect>& detections, int activeTracks)
{
    QVector<QRect> boxes;
    boxes.reserve(static_cast<int>(detections.size()));
    for (const cv::Rect& det : detections) {
        boxes.push_back(QRect(det.x, det.y, det.width, det.height));
    }
    return jcut::boxstream::buildScanPreview(source, boxes, activeTracks);
}

inline std::vector<WeightedDetection> filterAndSuppressDetections(std::vector<WeightedDetection> detections,
                                                           const QSize& frameSize)
{
    const int width = qMax(1, frameSize.width());
    const int height = qMax(1, frameSize.height());
    std::sort(detections.begin(), detections.end(), [](const WeightedDetection& a, const WeightedDetection& b) {
        return a.weight > b.weight;
    });
    std::vector<WeightedDetection> filtered;
    filtered.reserve(detections.size());
    for (const WeightedDetection& det : detections) {
        const double aspect = det.box.height > 0
                                  ? static_cast<double>(det.box.width) / static_cast<double>(det.box.height)
                                  : 0.0;
        if (aspect < 0.55 || aspect > 1.65) {
            continue;
        }
        if (det.box.width < (width / 28) || det.box.height < (height / 28)) {
            continue;
        }
        if (det.box.x <= 0 || det.box.y <= 0 ||
            (det.box.x + det.box.width) >= width ||
            (det.box.y + det.box.height) >= height) {
            continue;
        }
        bool keep = true;
        for (const WeightedDetection& existing : filtered) {
            if (iou(existing.box, det.box) > 0.35) {
                keep = false;
                break;
            }
        }
        if (keep) {
            filtered.push_back(det);
        }
    }
    return filtered;
}

inline bool ensureFaceDnnModel(const QString& baseDir, QString* prototxtOut, QString* modelOut)
{
    if (!prototxtOut || !modelOut) {
        return false;
    }
    const QString prototxtPath = QDir(baseDir).absoluteFilePath(
        QStringLiteral("external/opencv/samples/dnn/face_detector/deploy.prototxt"));
    const QString modelPath = QDir(baseDir).absoluteFilePath(
        QStringLiteral("external/opencv/samples/dnn/face_detector/res10_300x300_ssd_iter_140000_fp16.caffemodel"));
    if (!QFileInfo::exists(modelPath)) {
        QDir().mkpath(QFileInfo(modelPath).absolutePath());
        QStringList args = {
            QStringLiteral("-L"),
            QStringLiteral("-o"), modelPath,
            QStringLiteral("https://raw.githubusercontent.com/opencv/opencv_3rdparty/dnn_samples_face_detector_20180205_fp16/res10_300x300_ssd_iter_140000_fp16.caffemodel")
        };
        QProcess proc;
        proc.start(QStringLiteral("curl"), args);
        proc.waitForFinished(-1);
        if (proc.exitStatus() != QProcess::NormalExit || proc.exitCode() != 0 || !QFileInfo::exists(modelPath)) {
            return false;
        }
    }
    if (!QFileInfo::exists(prototxtPath)) {
        return false;
    }
    *prototxtOut = prototxtPath;
    *modelOut = modelPath;
    return true;
}

inline std::vector<cv::Rect> runDnnFaceDetect(DnnFaceDetectorRuntime* runtime, const cv::Mat& bgr, float confThreshold = 0.5f)
{
    std::vector<cv::Rect> out;
    if (!runtime || !runtime->loaded || bgr.empty()) {
        return out;
    }
    cv::Mat detections;
    auto forwardOnce = [&]() {
        cv::Mat blob = cv::dnn::blobFromImage(
            bgr, 1.0, cv::Size(300, 300), cv::Scalar(104.0, 177.0, 123.0), false, false);
        runtime->net.setInput(blob);
        detections = runtime->net.forward();
    };
    try {
        forwardOnce();
    } catch (const cv::Exception&) {
        if (runtime->usingCuda && !runtime->cpuFallbackApplied) {
            runtime->net.setPreferableBackend(cv::dnn::DNN_BACKEND_OPENCV);
            runtime->net.setPreferableTarget(cv::dnn::DNN_TARGET_CPU);
            runtime->usingCuda = false;
            runtime->cpuFallbackApplied = true;
            try {
                forwardOnce();
            } catch (const cv::Exception&) {
                return out;
            }
        } else {
            return out;
        }
    }
    const int numDetections = detections.size[2];
    const int width = bgr.cols;
    const int height = bgr.rows;
    for (int i = 0; i < numDetections; ++i) {
        const float confidence = detections.ptr<float>(0, 0, i)[2];
        if (confidence < confThreshold) {
            continue;
        }
        int x1 = static_cast<int>(detections.ptr<float>(0, 0, i)[3] * width);
        int y1 = static_cast<int>(detections.ptr<float>(0, 0, i)[4] * height);
        int x2 = static_cast<int>(detections.ptr<float>(0, 0, i)[5] * width);
        int y2 = static_cast<int>(detections.ptr<float>(0, 0, i)[6] * height);
        x1 = qBound(0, x1, qMax(0, width - 1));
        y1 = qBound(0, y1, qMax(0, height - 1));
        x2 = qBound(0, x2, qMax(0, width - 1));
        y2 = qBound(0, y2, qMax(0, height - 1));
        const int w = qMax(0, x2 - x1);
        const int h = qMax(0, y2 - y1);
        if (w < 8 || h < 8) {
            continue;
        }
        out.emplace_back(x1, y1, w, h);
    }
    return out;
}

#if JCUT_HAVE_OPENCV_CONTRIB
inline cv::Ptr<cv::legacy::Tracker> createContribTracker(BoxstreamDetectorPreset preset)
{
    if (preset == BoxstreamDetectorPreset::ContribCsrt) {
        return cv::legacy::TrackerCSRT::create();
    }
    if (preset == BoxstreamDetectorPreset::ContribKcf) {
        return cv::legacy::TrackerKCF::create();
    }
    return {};
}
#endif
#endif
} // namespace jcut::boxstream
