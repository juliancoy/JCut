#include "boxstream_generation.h"
#include "boxstream_runtime.h"
#include "decoder_context.h"
#include "debug_controls.h"
#include "editor_shared.h"
#include "frame_handle.h"
#include "vulkan_res10_ncnn_face_detector.h"
#include "vulkan_zero_copy_face_detector.h"

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
#include <QStringList>
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
#include <opencv2/dnn.hpp>
#include <opencv2/imgproc.hpp>
#include <opencv2/objdetect.hpp>
#endif

namespace {

struct Options {
    QString videoPath = QStringLiteral("/mnt/Cancer/PanelVid2TikTok/Politics/YTDown.com_YouTube_Meet-the-Candidates-for-Baltimore-County_Media_Hho5MORgIj8_001_1080p.mp4");
    QString outputDir = QStringLiteral("testbench_assets/vulkan_boxstream_offscreen");
    int maxFrames = 0; // 0 => full video
    int stride = 1;
    int maxDetections = 256;
    int maxFacesPerFrame = 0; // 0 => no post-cap
    int previewFrames = 24;
    int previewStride = 12;
    float threshold = 0.45f;
    QString detector = QStringLiteral("jcut-dnn");
    QString res10ParamPath;
    QString res10BinPath;
    QString paramsFile;
    QString previewSocket;
    bool livePreview = false;
    bool writePreviewFiles = false;
    bool materializedGenerateBoxstream = false;
    bool heuristicZeroCopyDetector = false;
    editor::DecodePreference decodePreference = editor::DecodePreference::HardwareZeroCopy;
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

struct RuntimeTuning {
    int stride = 1;
    int maxDetections = 256;
    int maxFacesPerFrame = 0;
    float threshold = 0.45f;
    float roiX1 = 0.0f;
    float roiY1 = 0.0f;
    float roiX2 = 1.0f;
    float roiY2 = 1.0f;
    float minFaceAreaRatio = 0.0f;
    float maxFaceAreaRatio = 1.0f;
    float minAspect = 0.0f;
    float maxAspect = 100.0f;
};

QVector<Detection> sanitizeDetections(const QVector<Detection>& raw,
                                      const QSize& frameSize,
                                      int maxFacesPerFrame,
                                      const RuntimeTuning& tuning)
{
    QVector<Detection> out;
    if (!frameSize.isValid()) {
        return raw;
    }
    out.reserve(raw.size());
    const QRectF roiRect(tuning.roiX1 * frameSize.width(),
                         tuning.roiY1 * frameSize.height(),
                         (tuning.roiX2 - tuning.roiX1) * frameSize.width(),
                         (tuning.roiY2 - tuning.roiY1) * frameSize.height());
    const double frameArea = static_cast<double>(frameSize.width()) * static_cast<double>(frameSize.height());
    for (const auto& d : raw) {
        QRectF box = d.box.intersected(QRectF(0, 0, frameSize.width(), frameSize.height()));
        if (box.width() <= 1.0 || box.height() <= 1.0) {
            continue;
        }
        const QPointF c = box.center();
        if (!roiRect.contains(c)) {
            continue;
        }
        const double areaRatio = (box.width() * box.height()) / qMax(1.0, frameArea);
        if (areaRatio < tuning.minFaceAreaRatio || areaRatio > tuning.maxFaceAreaRatio) {
            continue;
        }
        const double aspect = box.width() / qMax(1.0, box.height());
        if (aspect < tuning.minAspect || aspect > tuning.maxAspect) {
            continue;
        }
        out.push_back({box, d.confidence});
    }
    std::sort(out.begin(), out.end(), [](const Detection& a, const Detection& b) {
        return a.confidence > b.confidence;
    });
    if (maxFacesPerFrame > 0 && out.size() > maxFacesPerFrame) {
        out.resize(maxFacesPerFrame);
    }
    return out;
}

void usage(const char* argv0)
{
    std::cerr << "Usage: " << argv0
              << " [video] [--out-dir DIR] [--max-frames N] [--stride N]"
              << " [--threshold F] [--preview-frames N] [--preview-stride N]"
              << " [--full-video] [--max-faces-per-frame N]"
              << " [--params-file PATH]"
              << " [--detector jcut-dnn|jcut-heuristic-zero-copy]  # default: trained Vulkan DNN"
              << " [--res10-param PATH] [--res10-bin PATH]"
              << " [--preview-window] [--no-preview-window] [--preview-files]"
              << " [--materialized-generate-boxstream]"
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
            options->maxFrames = std::max(0, std::atoi(v));
        } else if (arg == "--stride") {
            const char* v = next("--stride");
            if (!v) return false;
            options->stride = std::max(1, std::atoi(v));
        } else if (arg == "--full-video") {
            options->maxFrames = 0;
        } else if (arg == "--max-faces-per-frame") {
            const char* v = next("--max-faces-per-frame");
            if (!v) return false;
            options->maxFacesPerFrame = std::max(0, std::atoi(v));
        } else if (arg == "--threshold") {
            const char* v = next("--threshold");
            if (!v) return false;
            options->threshold = std::clamp(static_cast<float>(std::atof(v)), 0.0f, 1.0f);
        } else if (arg == "--preview-frames") {
            const char* v = next("--preview-frames");
            if (!v) return false;
            options->previewFrames = std::max(0, std::atoi(v));
        } else if (arg == "--preview-stride") {
            const char* v = next("--preview-stride");
            if (!v) return false;
            options->previewStride = std::max(1, std::atoi(v));
        } else if (arg == "--detector") {
            const char* v = next("--detector");
            if (!v) return false;
            const QString detector = QString::fromLocal8Bit(v).trimmed().toLower();
            if (detector == QStringLiteral("jcut-heuristic-zero-copy") ||
                detector == QStringLiteral("vulkan-zero-copy-heuristic")) {
                options->heuristicZeroCopyDetector = true;
                options->detector = QStringLiteral("jcut-heuristic-zero-copy");
            } else if (detector != QStringLiteral("jcut-dnn") &&
                detector != QStringLiteral("jcut-dnn-vulkan") &&
                detector != QStringLiteral("res10-vulkan") &&
                detector != QStringLiteral("native-jcut-dnn") &&
                detector != QStringLiteral("native_vulkan_dnn")) {
                std::cerr << "Unsupported --detector value: " << v
                          << ". Use jcut-dnn or jcut-heuristic-zero-copy.\n";
                return false;
            } else {
                options->heuristicZeroCopyDetector = false;
                options->detector = QStringLiteral("jcut-dnn");
            }
        } else if (arg == "--res10-param") {
            const char* v = next("--res10-param");
            if (!v) return false;
            options->res10ParamPath = QString::fromLocal8Bit(v);
        } else if (arg == "--res10-bin") {
            const char* v = next("--res10-bin");
            if (!v) return false;
            options->res10BinPath = QString::fromLocal8Bit(v);
        } else if (arg == "--params-file") {
            const char* v = next("--params-file");
            if (!v) return false;
            options->paramsFile = QString::fromLocal8Bit(v);
        } else if (arg == "--preview-socket") {
            const char* v = next("--preview-socket");
            if (!v) return false;
            options->previewSocket = QString::fromLocal8Bit(v);
        } else if (arg == "--preview-window") {
            options->livePreview = true;
        } else if (arg == "--no-preview-window") {
            options->livePreview = false;
        } else if (arg == "--preview-files") {
            options->writePreviewFiles = true;
        } else if (arg == "--no-preview-files") {
            options->writePreviewFiles = false;
        } else if (arg == "--materialized-generate-boxstream") {
            options->materializedGenerateBoxstream = true;
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

bool applyRuntimeParamsFile(const QString& path,
                            const QFileInfo& info,
                            RuntimeTuning* tuning,
                            QDateTime* lastAppliedMtime)
{
    if (!tuning || !lastAppliedMtime || path.isEmpty()) {
        return false;
    }
    if (!info.exists()) {
        return false;
    }
    const QDateTime mtime = info.lastModified();
    if (lastAppliedMtime->isValid() && mtime <= *lastAppliedMtime) {
        return false;
    }

    QFile f(path);
    if (!f.open(QIODevice::ReadOnly)) {
        return false;
    }
    QJsonParseError parseError;
    const QJsonDocument doc = QJsonDocument::fromJson(f.readAll(), &parseError);
    if (parseError.error != QJsonParseError::NoError || !doc.isObject()) {
        return false;
    }
    const QJsonObject o = doc.object();
    if (o.contains(QStringLiteral("stride"))) {
        tuning->stride = qMax(1, o.value(QStringLiteral("stride")).toInt(tuning->stride));
    }
    if (o.contains(QStringLiteral("max_detections"))) {
        tuning->maxDetections = qMax(1, o.value(QStringLiteral("max_detections")).toInt(tuning->maxDetections));
    }
    if (o.contains(QStringLiteral("max_faces_per_frame"))) {
        tuning->maxFacesPerFrame = qMax(0, o.value(QStringLiteral("max_faces_per_frame")).toInt(tuning->maxFacesPerFrame));
    }
    if (o.contains(QStringLiteral("threshold"))) {
        tuning->threshold = std::clamp(static_cast<float>(o.value(QStringLiteral("threshold")).toDouble(tuning->threshold)),
                                       0.0f,
                                       1.0f);
    }
    if (o.contains(QStringLiteral("roi_x1"))) tuning->roiX1 = std::clamp(static_cast<float>(o.value(QStringLiteral("roi_x1")).toDouble(tuning->roiX1)), 0.0f, 1.0f);
    if (o.contains(QStringLiteral("roi_y1"))) tuning->roiY1 = std::clamp(static_cast<float>(o.value(QStringLiteral("roi_y1")).toDouble(tuning->roiY1)), 0.0f, 1.0f);
    if (o.contains(QStringLiteral("roi_x2"))) tuning->roiX2 = std::clamp(static_cast<float>(o.value(QStringLiteral("roi_x2")).toDouble(tuning->roiX2)), 0.0f, 1.0f);
    if (o.contains(QStringLiteral("roi_y2"))) tuning->roiY2 = std::clamp(static_cast<float>(o.value(QStringLiteral("roi_y2")).toDouble(tuning->roiY2)), 0.0f, 1.0f);
    if (tuning->roiX2 < tuning->roiX1) std::swap(tuning->roiX1, tuning->roiX2);
    if (tuning->roiY2 < tuning->roiY1) std::swap(tuning->roiY1, tuning->roiY2);
    if (o.contains(QStringLiteral("min_face_area_ratio"))) tuning->minFaceAreaRatio = std::clamp(static_cast<float>(o.value(QStringLiteral("min_face_area_ratio")).toDouble(tuning->minFaceAreaRatio)), 0.0f, 1.0f);
    if (o.contains(QStringLiteral("max_face_area_ratio"))) tuning->maxFaceAreaRatio = std::clamp(static_cast<float>(o.value(QStringLiteral("max_face_area_ratio")).toDouble(tuning->maxFaceAreaRatio)), 0.0f, 1.0f);
    if (tuning->maxFaceAreaRatio < tuning->minFaceAreaRatio) std::swap(tuning->minFaceAreaRatio, tuning->maxFaceAreaRatio);
    if (o.contains(QStringLiteral("min_aspect"))) tuning->minAspect = std::clamp(static_cast<float>(o.value(QStringLiteral("min_aspect")).toDouble(tuning->minAspect)), 0.0f, 100.0f);
    if (o.contains(QStringLiteral("max_aspect"))) tuning->maxAspect = std::clamp(static_cast<float>(o.value(QStringLiteral("max_aspect")).toDouble(tuning->maxAspect)), 0.0f, 100.0f);
    if (tuning->maxAspect < tuning->minAspect) std::swap(tuning->minAspect, tuning->maxAspect);
    *lastAppliedMtime = mtime;
    return true;
}

QString findRes10NcnnModelFile(const QString& explicitPath, const QString& fileName)
{
    if (!explicitPath.isEmpty()) {
        return explicitPath;
    }
    const QStringList roots{
        QDir::currentPath(),
        QCoreApplication::applicationDirPath(),
        QDir(QCoreApplication::applicationDirPath()).absoluteFilePath(QStringLiteral(".."))
    };
    const QStringList rels{
        QStringLiteral("assets/models/%1").arg(fileName),
        QStringLiteral("testbench_assets/models/%1").arg(fileName),
        QStringLiteral("models/%1").arg(fileName)
    };
    for (const QString& root : roots) {
        for (const QString& rel : rels) {
            const QString candidate = QDir(root).absoluteFilePath(rel);
            if (QFileInfo::exists(candidate)) {
                return candidate;
            }
        }
    }
    return QDir::current().absoluteFilePath(QStringLiteral("assets/models/%1").arg(fileName));
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
        app.pApplicationName = "jcut-dnn-facestream-generator";
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

        uint32_t extensionCount = 0;
        vkEnumerateDeviceExtensionProperties(physicalDevice, nullptr, &extensionCount, nullptr);
        std::vector<VkExtensionProperties> extensionProperties(extensionCount);
        if (extensionCount) {
            vkEnumerateDeviceExtensionProperties(physicalDevice, nullptr, &extensionCount, extensionProperties.data());
        }
        auto hasDeviceExtension = [&](const char* name) {
            return std::any_of(extensionProperties.begin(), extensionProperties.end(), [&](const VkExtensionProperties& ext) {
                return std::strcmp(ext.extensionName, name) == 0;
            });
        };
        std::vector<const char*> enabledExtensions;
        auto enableIfAvailable = [&](const char* name) {
            if (hasDeviceExtension(name)) {
                enabledExtensions.push_back(name);
            }
        };
        enableIfAvailable(VK_KHR_BIND_MEMORY_2_EXTENSION_NAME);
        enableIfAvailable(VK_KHR_DESCRIPTOR_UPDATE_TEMPLATE_EXTENSION_NAME);
        enableIfAvailable(VK_KHR_GET_MEMORY_REQUIREMENTS_2_EXTENSION_NAME);
        enableIfAvailable(VK_KHR_MAINTENANCE1_EXTENSION_NAME);
        enableIfAvailable(VK_KHR_MAINTENANCE3_EXTENSION_NAME);

        VkDeviceCreateInfo deviceInfo{};
        deviceInfo.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
        deviceInfo.queueCreateInfoCount = 1;
        deviceInfo.pQueueCreateInfos = &queueInfo;
        deviceInfo.enabledExtensionCount = static_cast<uint32_t>(enabledExtensions.size());
        deviceInfo.ppEnabledExtensionNames = enabledExtensions.empty() ? nullptr : enabledExtensions.data();
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
        if (context.physicalDevice == VK_NULL_HANDLE ||
            context.device == VK_NULL_HANDLE ||
            context.queue == VK_NULL_HANDLE ||
            context.queueFamilyIndex == UINT32_MAX) {
            if (error) *error = QStringLiteral("Invalid external Vulkan detector context.");
            return false;
        }
        if (!ownsInstanceAndDevice &&
            physicalDevice == context.physicalDevice &&
            device == context.device &&
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

QVector<Detection> detectVulkanFrame(jcut::vulkan_detector::VulkanZeroCopyFaceDetector* detector,
                                     VulkanHarnessContext* vk,
                                     const render_detail::OffscreenVulkanFrame& frame,
                                     int maxDetections,
                                     float threshold,
                                     double* vulkanMs,
                                     QString* error)
{
    QVector<Detection> out;
    if (!detector || !vk || !frame.valid) {
        if (error) *error = QStringLiteral("Invalid Vulkan detector frame.");
        return out;
    }
    if (!vk->attachExternalDevice({frame.physicalDevice,
                                   frame.device,
                                   frame.queue,
                                   frame.queueFamilyIndex}, error)) {
        return out;
    }
    if (!detector->isInitialized() &&
        !detector->initialize(vk->detectorContext(), error)) {
        return out;
    }
    if (!vk->ensureDetectorBuffers(detector->tensorSpec().byteSize(), maxDetections, error)) {
        return out;
    }

    QElapsedTimer timer;
    timer.start();
    const jcut::vulkan_detector::VulkanExternalImage source{
        frame.imageView,
        frame.imageLayout,
        frame.size,
        false
    };
    const jcut::vulkan_detector::VulkanTensorBuffer tensor{
        vk->tensorBuffer,
        detector->tensorSpec().byteSize()
    };
    if (!detector->preprocessToTensor(source, tensor, error)) {
        return out;
    }
    const jcut::vulkan_detector::VulkanTensorBuffer detectionBuffer{
        vk->detectionBuffer,
        vk->detectionSize
    };
    if (!detector->inferFromTensor(tensor, detectionBuffer, maxDetections, threshold, error)) {
        return out;
    }
    out = readDetections(*vk, frame.size.width(), frame.size.height());
    if (vulkanMs) {
        *vulkanMs = static_cast<double>(timer.nsecsElapsed()) / 1'000'000.0;
    }
    return out;
}

QVector<Detection> detectRes10VulkanFrame(jcut::vulkan_detector::VulkanZeroCopyFaceDetector* preprocessor,
                                          jcut::vulkan_detector::VulkanRes10NcnnFaceDetector* detector,
                                          VulkanHarnessContext* vk,
                                          const render_detail::OffscreenVulkanFrame& frame,
                                          const QString& paramPath,
                                          const QString& binPath,
                                          float threshold,
                                          double* vulkanMs,
                                          QString* error)
{
    QVector<Detection> out;
    if (!preprocessor || !detector || !vk || !frame.valid) {
        if (error) *error = QStringLiteral("Invalid Vulkan Res10 detector frame.");
        return out;
    }
    if (!vk->attachExternalDevice({frame.physicalDevice,
                                   frame.device,
                                   frame.queue,
                                   frame.queueFamilyIndex}, error)) {
        return out;
    }
    if (!preprocessor->isInitialized() &&
        !preprocessor->initialize(vk->detectorContext(), error)) {
        return out;
    }
    if (!detector->isInitialized() &&
        !detector->initialize(vk->detectorContext(), paramPath, binPath, error)) {
        return out;
    }
    if (!vk->ensureDetectorBuffers(preprocessor->tensorSpec().byteSize(), 1, error)) {
        return out;
    }

    QElapsedTimer timer;
    timer.start();
    const jcut::vulkan_detector::VulkanExternalImage source{
        frame.imageView,
        frame.imageLayout,
        frame.size,
        false
    };
    const jcut::vulkan_detector::VulkanTensorBuffer tensor{
        vk->tensorBuffer,
        preprocessor->tensorSpec().byteSize()
    };
    if (!preprocessor->preprocessToTensor(source, tensor, error)) {
        return out;
    }
    const QVector<jcut::vulkan_detector::Res10Detection> raw =
        detector->inferFromTensor(tensor, frame.size.width(), frame.size.height(), threshold, error);
    out.reserve(raw.size());
    for (const auto& det : raw) {
        out.push_back({det.box, det.confidence});
    }
    if (vulkanMs) {
        *vulkanMs = static_cast<double>(timer.nsecsElapsed()) / 1'000'000.0;
    }
    return out;
}

#if JCUT_HAVE_OPENCV
bool hasVulkanDnnTarget()
{
    const std::vector<cv::dnn::Target> targets =
        cv::dnn::getAvailableTargets(cv::dnn::DNN_BACKEND_VKCOM);
    return std::find(targets.begin(), targets.end(), cv::dnn::DNN_TARGET_VULKAN) != targets.end();
}

bool initializeTrainedVulkanDnn(cv::dnn::Net* net, QString* error)
{
    if (!net) {
        if (error) *error = QStringLiteral("Invalid trained Vulkan DNN runtime.");
        return false;
    }
    if (!hasVulkanDnnTarget()) {
        if (error) *error = QStringLiteral("OpenCV VKCOM/Vulkan DNN target is unavailable in this build/runtime.");
        return false;
    }
    QString proto;
    QString model;
    if (!jcut::boxstream::ensureFaceDnnModel(QDir::currentPath(), &proto, &model)) {
        if (error) *error = QStringLiteral("OpenCV Res10 SSD face detector assets are missing or could not be downloaded.");
        return false;
    }
    try {
        *net = cv::dnn::readNetFromCaffe(proto.toStdString(), model.toStdString());
        net->setPreferableBackend(cv::dnn::DNN_BACKEND_VKCOM);
        net->setPreferableTarget(cv::dnn::DNN_TARGET_VULKAN);
    } catch (const cv::Exception& e) {
        if (error) *error = QStringLiteral("Failed to initialize trained Vulkan DNN: %1").arg(e.what());
        return false;
    }
    return true;
}

QVector<Detection> detectTrainedVulkanDnn(cv::dnn::Net* net,
                                          const cv::Mat& bgr,
                                          float threshold,
                                          double* inferenceMs,
                                          QString* error)
{
    QVector<Detection> out;
    if (!net || bgr.empty()) {
        return out;
    }
    QElapsedTimer timer;
    timer.start();
    try {
        cv::Mat blob = cv::dnn::blobFromImage(
            bgr, 1.0, cv::Size(300, 300), cv::Scalar(104.0, 177.0, 123.0), false, false);
        net->setInput(blob);
        const cv::Mat detections = net->forward();
        if (detections.dims != 4 || detections.size[2] <= 0 || detections.size[3] < 7) {
            if (error) *error = QStringLiteral("Unexpected trained Vulkan DNN output shape.");
            return out;
        }
        const int width = bgr.cols;
        const int height = bgr.rows;
        for (int i = 0; i < detections.size[2]; ++i) {
            const float* row = detections.ptr<float>(0, 0, i);
            const float confidence = row[2];
            if (confidence < threshold) {
                continue;
            }
            int x1 = static_cast<int>(row[3] * width);
            int y1 = static_cast<int>(row[4] * height);
            int x2 = static_cast<int>(row[5] * width);
            int y2 = static_cast<int>(row[6] * height);
            x1 = qBound(0, x1, qMax(0, width - 1));
            y1 = qBound(0, y1, qMax(0, height - 1));
            x2 = qBound(0, x2, qMax(0, width - 1));
            y2 = qBound(0, y2, qMax(0, height - 1));
            QRectF box(x1, y1, qMax(0, x2 - x1), qMax(0, y2 - y1));
            box = box.intersected(QRectF(0, 0, width, height));
            if (box.width() >= 8.0 && box.height() >= 8.0) {
                out.push_back({box, confidence});
            }
        }
    } catch (const cv::Exception& e) {
        if (error) *error = QStringLiteral("Trained Vulkan DNN inference failed: %1").arg(e.what());
        return {};
    }
    if (inferenceMs) {
        *inferenceMs = static_cast<double>(timer.nsecsElapsed()) / 1'000'000.0;
    }
    std::sort(out.begin(), out.end(), [](const Detection& a, const Detection& b) {
        return a.confidence > b.confidence;
    });
    return out;
}
#endif

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
        previewWindow.setWindowTitle(QStringLiteral("JCut DNN FaceStream Generator Offscreen Preview"));
        previewWindow.setAlignment(Qt::AlignCenter);
        previewWindow.setMinimumSize(960, 540);
        previewWindow.setStyleSheet(QStringLiteral("background:#111; color:#ddd; border:1px solid #333;"));
        previewWindow.setText(QStringLiteral("Waiting for JCut DNN FaceStream preview..."));
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
    const int64_t decoderDurationFrames = qMax<int64_t>(0, decoder.info().durationFrames);
    const int64_t targetFrames = options.maxFrames > 0
        ? static_cast<int64_t>(options.maxFrames)
        : qMax<int64_t>(1, decoderDurationFrames);
    sourceClip.durationFrames = qMax<int64_t>(1, targetFrames);
    sourceClip.sourceDurationFrames = qMax<int64_t>(1, targetFrames);
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
#if JCUT_HAVE_OPENCV
    cv::CascadeClassifier faceCascade;
    cv::CascadeClassifier faceCascadeAlt;
    cv::CascadeClassifier faceCascadeProfile;
    if (options.materializedGenerateBoxstream) {
        const QString cascadePath = jcut::boxstream::findCascadeFile(QStringLiteral("haarcascade_frontalface_default.xml"));
        if (cascadePath.isEmpty() || !faceCascade.load(cascadePath.toStdString())) {
            std::cerr << "Failed to load Haar cascade for materialized Generate FaceStream scan.\n";
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
    }
#else
    if (options.materializedGenerateBoxstream) {
        std::cerr << "OpenCV is not enabled in this build; materialized Generate FaceStream scan is unavailable.\n";
        return 2;
    }
#endif
    const bool zeroCopyVulkanDetector = !options.materializedGenerateBoxstream;
    const QString res10ParamPath = findRes10NcnnModelFile(
        options.res10ParamPath, QStringLiteral("res10_300x300_ssd_ncnn.param"));
    const QString res10BinPath = findRes10NcnnModelFile(
        options.res10BinPath, QStringLiteral("res10_300x300_ssd_ncnn.bin"));
    if (zeroCopyVulkanDetector && !options.heuristicZeroCopyDetector) {
        std::cout << "detector=res10_ssd_ncnn_vulkan_zero_copy_v1"
                  << " name=\"JCut DNN FaceStream Generator\""
                  << " inference_backend=ncnn_vulkan"
                  << " model_param=\"" << res10ParamPath.toStdString() << "\""
                  << " model_bin=\"" << res10BinPath.toStdString() << "\""
                  << " qimage_materialized=0\n";
    } else if (options.heuristicZeroCopyDetector) {
        std::cout << "detector=native_jcut_heuristic_zero_copy_v1"
                  << " name=\"JCut DNN FaceStream Generator\""
                  << " decode=hardware_zero_copy"
                  << " qimage_materialized=0\n";
    }

    int decoded = 0;
    int processed = 0;
    int cpuFrames = 0;
    int hardwareFrames = 0;
    int totalDetections = 0;
    int previewWritten = 0;
    double renderDecodeMsTotal = 0.0;
    double renderCompositeMsTotal = 0.0;
    double renderReadbackMsTotal = 0.0;
    double vulkanDetectMsTotal = 0.0;
    QVector<Track> tracks;
    QJsonArray frameRows;
    bool printedAppVulkanFailure = false;
    VulkanHarnessContext detectorContext;
    jcut::vulkan_detector::VulkanZeroCopyFaceDetector zeroCopyDetector;
    jcut::vulkan_detector::VulkanRes10NcnnFaceDetector res10Detector;
    RuntimeTuning tuning{
        options.stride,
        options.maxDetections,
        options.maxFacesPerFrame,
        options.threshold
    };
    QDateTime paramsMtime;

    const auto wallStart = std::chrono::steady_clock::now();
    const int totalFrames = static_cast<int>(qMax<int64_t>(1, targetFrames));
    for (int frameNumber = 0; frameNumber < totalFrames; ++frameNumber) {
        if (!options.paramsFile.isEmpty()) {
            QFileInfo paramsInfo(options.paramsFile);
            if (applyRuntimeParamsFile(options.paramsFile, paramsInfo, &tuning, &paramsMtime)) {
                std::cout << "runtime_params"
                          << " stride=" << tuning.stride
                          << " threshold=" << tuning.threshold
                          << " max_detections=" << tuning.maxDetections
                          << " max_faces_per_frame=" << tuning.maxFacesPerFrame
                          << "\n";
            }
        }
        ++decoded;
        if ((frameNumber % tuning.stride) != 0) {
            continue;
        }

        jcut::boxstream::VulkanFrameStats renderStats;
        QVector<Detection> detections;
        QSize detectionFrameSize = renderSize;
        bool appVulkanFrame = false;
        double vulkanDetectMs = 0.0;

        if (zeroCopyVulkanDetector) {
            render_detail::OffscreenVulkanFrame vulkanFrame;
            error.clear();
            if (!jcut::boxstream::renderFrameToVulkan(&appFrameProvider,
                                                      sourceClip,
                                                      options.videoPath,
                                                      frameNumber,
                                                      frameNumber,
                                                      renderSize,
                                                      &vulkanFrame,
                                                      &renderStats,
                                                      &error)) {
                if (!printedAppVulkanFailure) {
                    std::cerr << "Application Vulkan frame path unavailable: "
                              << error.toStdString() << "\n";
                    printedAppVulkanFailure = true;
                }
                continue;
            }
            appVulkanFrame = true;
            ++hardwareFrames;
            detectionFrameSize = vulkanFrame.size;
            error.clear();
            if (options.heuristicZeroCopyDetector) {
                detections = detectVulkanFrame(&zeroCopyDetector,
                                               &detectorContext,
                                               vulkanFrame,
                                               tuning.maxDetections,
                                               tuning.threshold,
                                               &vulkanDetectMs,
                                               &error);
            } else {
                detections = detectRes10VulkanFrame(&zeroCopyDetector,
                                                    &res10Detector,
                                                    &detectorContext,
                                                    vulkanFrame,
                                                    res10ParamPath,
                                                    res10BinPath,
                                                    tuning.threshold,
                                                    &vulkanDetectMs,
                                                    &error);
            }
            if (!error.isEmpty() && detections.isEmpty()) {
                std::cerr << "Vulkan zero-copy detection failed at frame "
                          << frameNumber << ": " << error.toStdString() << "\n";
                continue;
            }
            detections = sanitizeDetections(detections, detectionFrameSize, tuning.maxFacesPerFrame, tuning);
            const bool previewFrameDue = (frameNumber % options.previewStride) == 0;
            const bool needsPreviewFrame =
                previewFrameDue &&
                (previewWindowPtr ||
                 previewSocketPtr ||
                 (options.writePreviewFiles && previewWritten < options.previewFrames));
            if (needsPreviewFrame) {
                QImage frameImage = jcut::boxstream::readLastRenderedVulkanFrameImage(&appFrameProvider,
                                                                                       &renderStats,
                                                                                       &error);
                if (!frameImage.isNull()) {
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
                            "JCut DNN FaceStream Generator - frame %1 tracks %2")
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
                } else if (!error.isEmpty() && !printedAppVulkanFailure) {
                    std::cerr << "Preview readback unavailable: " << error.toStdString() << "\n";
                    printedAppVulkanFailure = true;
                }
            }
        } else {
#if JCUT_HAVE_OPENCV
            QImage frameImage = jcut::boxstream::renderFrameWithVulkan(&appFrameProvider,
                                                                       sourceClip,
                                                                       options.videoPath,
                                                                       frameNumber,
                                                                       frameNumber,
                                                                       renderSize,
                                                                       &renderStats);
            appVulkanFrame = !frameImage.isNull();
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
            detectionFrameSize = frameImage.size();

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
            detections.reserve(static_cast<int>(filtered.size()));
            for (const jcut::boxstream::WeightedDetection& det : filtered) {
                detections.push_back({QRectF(det.box.x, det.box.y, det.box.width, det.box.height),
                                      static_cast<float>(det.weight)});
            }
            detections = sanitizeDetections(detections, detectionFrameSize, tuning.maxFacesPerFrame, tuning);

            const bool previewFrameDue = (frameNumber % options.previewStride) == 0;
            const bool needsPreviewFrame =
                previewFrameDue &&
                (previewWindowPtr ||
                 previewSocketPtr ||
                 (options.writePreviewFiles && previewWritten < options.previewFrames));
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
                        "JCut Materialized Generate FaceStream - frame %1 tracks %2")
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
#endif
        }
        updateTracks(&tracks, detections, frameNumber, detectionFrameSize);

        ++processed;
        totalDetections += detections.size();
        renderDecodeMsTotal += renderStats.decodeMs;
        renderCompositeMsTotal += renderStats.compositeMs;
        renderReadbackMsTotal += renderStats.readbackMs;
        vulkanDetectMsTotal += vulkanDetectMs;
        const QString detectorId = options.materializedGenerateBoxstream
            ? QStringLiteral("materialized_generate_facestream_opencv_cascade_v1")
            : (options.heuristicZeroCopyDetector
                   ? QStringLiteral("native_jcut_heuristic_zero_copy_v1")
                   : QStringLiteral("res10_ssd_ncnn_vulkan_zero_copy_v1"));
        const bool qimageMaterialized = options.materializedGenerateBoxstream;
        frameRows.append(QJsonObject{
            {QStringLiteral("frame"), frameNumber},
            {QStringLiteral("detector"), detectorId},
            {QStringLiteral("detections"), detections.size()},
            {QStringLiteral("tracks"), tracks.size()},
            {QStringLiteral("app_vulkan_frame_path"), appVulkanFrame},
            {QStringLiteral("app_render_decode_ms"), renderStats.decodeMs},
            {QStringLiteral("app_render_texture_ms"), renderStats.textureMs},
            {QStringLiteral("app_render_composite_ms"), renderStats.compositeMs},
            {QStringLiteral("app_render_readback_ms"), renderStats.readbackMs},
            {QStringLiteral("vulkan_zero_copy_detection_ms"), vulkanDetectMs},
            {QStringLiteral("qimage_materialized"), qimageMaterialized}
        });
        std::cout << "frame=" << frameNumber
                  << " detector=" << detectorId.toStdString()
                  << " detections=" << detections.size()
                  << " tracks=" << tracks.size()
                  << " app_vulkan_frame_path=" << (appVulkanFrame ? 1 : 0)
                  << " render_decode_ms=" << renderStats.decodeMs
                  << " render_texture_ms=" << renderStats.textureMs
                  << " render_composite_ms=" << renderStats.compositeMs
                  << " render_readback_ms=" << renderStats.readbackMs
                  << " vulkan_zero_copy_detection_ms=" << vulkanDetectMs
                  << " qimage_materialized=" << (qimageMaterialized ? 1 : 0)
                  << "\n";
    }
    const auto wallEnd = std::chrono::steady_clock::now();
    QJsonArray trackRows;
    for (const Track& track : tracks) {
        trackRows.append(QJsonObject{
            {QStringLiteral("track_id"), track.id},
            {QStringLiteral("last_frame"), track.lastFrame},
            {QStringLiteral("length"), track.detections.size()},
            {QStringLiteral("detections"), track.detections}
        });
    }
    const QString backend = options.materializedGenerateBoxstream
        ? QStringLiteral("materialized_generate_facestream_opencv_cascade_v1")
        : (options.heuristicZeroCopyDetector
               ? QStringLiteral("native_jcut_heuristic_zero_copy_v1")
               : QStringLiteral("res10_ssd_ncnn_vulkan_zero_copy_v1"));
    writeJson(QDir(options.outputDir).filePath(QStringLiteral("tracks.json")), QJsonObject{
        {QStringLiteral("schema"), QStringLiteral("jcut_boxstream_offscreen_tracks_v1")},
        {QStringLiteral("video"), options.videoPath},
        {QStringLiteral("backend"), backend},
        {QStringLiteral("tracks"), trackRows},
        {QStringLiteral("frames"), frameRows}
    });
    const QJsonArray streams = jcut::boxstream::buildContinuityStreams(
        trackRows,
        QJsonObject{},
        backend,
        false);
    const QJsonObject continuityRoot = jcut::boxstream::buildContinuityRoot(
        QStringLiteral("offscreen"),
        false,
        0,
        qMax(0, totalFrames - 1),
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
        {QStringLiteral("generator_name"), QStringLiteral("JCut DNN FaceStream Generator")},
        {QStringLiteral("backend"), backend},
        {QStringLiteral("detector"), backend},
        {QStringLiteral("max_frames"), totalFrames},
        {QStringLiteral("stride"), tuning.stride},
        {QStringLiteral("runtime_threshold"), tuning.threshold},
        {QStringLiteral("runtime_max_detections"), tuning.maxDetections},
        {QStringLiteral("runtime_max_faces_per_frame"), tuning.maxFacesPerFrame},
        {QStringLiteral("runtime_roi_x1"), tuning.roiX1},
        {QStringLiteral("runtime_roi_y1"), tuning.roiY1},
        {QStringLiteral("runtime_roi_x2"), tuning.roiX2},
        {QStringLiteral("runtime_roi_y2"), tuning.roiY2},
        {QStringLiteral("runtime_min_face_area_ratio"), tuning.minFaceAreaRatio},
        {QStringLiteral("runtime_max_face_area_ratio"), tuning.maxFaceAreaRatio},
        {QStringLiteral("runtime_min_aspect"), tuning.minAspect},
        {QStringLiteral("runtime_max_aspect"), tuning.maxAspect},
        {QStringLiteral("runtime_params_file"), options.paramsFile},
        {QStringLiteral("decoded_frames"), decoded},
        {QStringLiteral("processed_frames"), processed},
        {QStringLiteral("sampling_note"), QStringLiteral("processed_frames increments only on frames where frame_number %% stride == 0")},
        {QStringLiteral("cpu_frames"), cpuFrames},
        {QStringLiteral("hardware_frames"), hardwareFrames},
        {QStringLiteral("app_vulkan_decode_path_frames"), hardwareFrames},
        {QStringLiteral("app_vulkan_frame_path_failed"), appFrameProvider.failed},
        {QStringLiteral("app_vulkan_frame_path_failure"), appFrameProvider.failureReason},
        {QStringLiteral("total_detections"), totalDetections},
        {QStringLiteral("track_count"), tracks.size()},
        {QStringLiteral("preview_frames_written"), previewWritten},
        {QStringLiteral("preview_stride"), options.previewStride},
        {QStringLiteral("avg_render_decode_ms"), processed ? renderDecodeMsTotal / processed : 0.0},
        {QStringLiteral("avg_render_composite_ms"), processed ? renderCompositeMsTotal / processed : 0.0},
        {QStringLiteral("avg_render_readback_ms"), processed ? renderReadbackMsTotal / processed : 0.0},
        {QStringLiteral("avg_vulkan_zero_copy_detection_ms"), processed ? vulkanDetectMsTotal / processed : 0.0},
        {QStringLiteral("wall_sec"), std::chrono::duration<double>(wallEnd - wallStart).count()},
        {QStringLiteral("decode_zero_copy"), decodeZeroCopy},
        {QStringLiteral("qimage_materialized"), options.materializedGenerateBoxstream},
        {QStringLiteral("vulkan_path_uses_qimage"), false},
        {QStringLiteral("current_mode_uses_qimage"), options.materializedGenerateBoxstream},
        {QStringLiteral("zero_copy_note"), options.materializedGenerateBoxstream
            ? QStringLiteral("Materialized compatibility mode: frames are rendered by Vulkan, read back to QImage, and scanned by the legacy OpenCV continuity detector.")
            : QStringLiteral("Res10 ncnn Vulkan path: frames remain as Vulkan images through preprocessing/inference; only compact detection metadata is read back. No QImage frame materialization is used.")}
    };
    writeJson(QDir(options.outputDir).filePath(QStringLiteral("summary.json")), summary);
    std::cout << "summary " << QJsonDocument(summary).toJson(QJsonDocument::Compact).constData() << "\n";
    return processed > 0 ? 0 : 1;
}
