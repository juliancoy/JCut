#include "vulkan_res10_ncnn_face_detector.h"

#include <QFile>
#include <QFileInfo>

#if JCUT_HAVE_NCNN
#include <command.h>
#include <gpu.h>
#include <mat.h>
#include <net.h>
#endif

#include <algorithm>
#include <cmath>
#include <memory>

namespace jcut::vulkan_detector {
namespace {

void setError(QString* out, const QString& value)
{
    if (out) {
        *out = value;
    }
}

bool validContext(const VulkanDeviceContext& context)
{
    return context.physicalDevice != VK_NULL_HANDLE &&
           context.device != VK_NULL_HANDLE &&
           context.queue != VK_NULL_HANDLE;
}

#if JCUT_HAVE_NCNN
int matchingNcnnDeviceIndex(VkPhysicalDevice physicalDevice)
{
    const int count = ncnn::get_gpu_count();
    for (int i = 0; i < count; ++i) {
        const ncnn::GpuInfo& info = ncnn::get_gpu_info(i);
        if (info.physicalDevice() == physicalDevice) {
            return i;
        }
    }
    return ncnn::get_default_gpu_index();
}
#endif

} // namespace

struct VulkanRes10NcnnFaceDetector::Impl {
#if JCUT_HAVE_NCNN
    std::unique_ptr<ncnn::VulkanDevice> vkdev;
    ncnn::Net net;
    ncnn::VkBufferMemory inputMemory{};
#endif
    VulkanDeviceContext context;
    bool initialized = false;
};

VulkanRes10NcnnFaceDetector::VulkanRes10NcnnFaceDetector()
    : m_impl(new Impl)
{
}

VulkanRes10NcnnFaceDetector::~VulkanRes10NcnnFaceDetector()
{
    release();
    delete m_impl;
}

bool VulkanRes10NcnnFaceDetector::initialize(const VulkanDeviceContext& context,
                                             const QString& paramPath,
                                             const QString& binPath,
                                             QString* errorMessage)
{
    if (m_impl->initialized) {
        return true;
    }
    if (!validContext(context)) {
        setError(errorMessage, QStringLiteral("invalid Vulkan device context for Res10 ncnn detector"));
        return false;
    }
    if (!QFileInfo::exists(paramPath) || !QFileInfo::exists(binPath)) {
        setError(errorMessage,
                 QStringLiteral("missing Res10 ncnn model artifacts: %1 and %2").arg(paramPath, binPath));
        return false;
    }

#if !JCUT_HAVE_NCNN
    Q_UNUSED(context);
    Q_UNUSED(paramPath);
    Q_UNUSED(binPath);
    setError(errorMessage, QStringLiteral("JCut was built without ncnn; Vulkan Res10 zero-copy detector is unavailable."));
    return false;
#else
    ncnn::create_gpu_instance();
    const int deviceIndex = matchingNcnnDeviceIndex(context.physicalDevice);
    if (deviceIndex < 0) {
        setError(errorMessage, QStringLiteral("ncnn did not find a Vulkan-capable device."));
        return false;
    }

    m_impl->vkdev = std::make_unique<ncnn::VulkanDevice>(
        deviceIndex, context.device, context.queue, context.queueFamilyIndex);
    if (!m_impl->vkdev->is_valid()) {
        setError(errorMessage, QStringLiteral("failed to wrap JCut Vulkan device for ncnn Res10 inference"));
        m_impl->vkdev.reset();
        return false;
    }

    m_impl->net.opt.use_vulkan_compute = true;
    m_impl->net.opt.use_fp16_packed = false;
    m_impl->net.opt.use_fp16_storage = false;
    m_impl->net.opt.use_fp16_arithmetic = false;
    m_impl->net.set_vulkan_device(m_impl->vkdev.get());

    QFile paramFile(paramPath);
    if (!paramFile.open(QIODevice::ReadOnly)) {
        setError(errorMessage, QStringLiteral("failed to open Res10 ncnn param file: %1").arg(paramPath));
        release();
        return false;
    }
    QByteArray paramBytes = paramFile.readAll();
    // Keep DetectionOutput permissive; runtime thresholding happens after extraction.
    // The stock converted param uses 0.45, which prevents small/low-confidence faces
    // from ever reaching the FaceStream post-filter.
    paramBytes.replace("1=4.500000e-01", "1=5.000000e-02");
    paramBytes.append('\0');
    if (m_impl->net.load_param_mem(paramBytes.constData()) != 0) {
        setError(errorMessage, QStringLiteral("failed to load Res10 ncnn param file: %1").arg(paramPath));
        release();
        return false;
    }
    if (m_impl->net.load_model(binPath.toLocal8Bit().constData()) != 0) {
        setError(errorMessage, QStringLiteral("failed to load Res10 ncnn bin file: %1").arg(binPath));
        release();
        return false;
    }

    m_impl->context = context;
    m_impl->initialized = true;
    return true;
#endif
}

void VulkanRes10NcnnFaceDetector::release()
{
#if JCUT_HAVE_NCNN
    m_impl->net.clear();
    m_impl->vkdev.reset();
    m_impl->inputMemory = {};
#endif
    m_impl->context = {};
    m_impl->initialized = false;
}

bool VulkanRes10NcnnFaceDetector::isInitialized() const
{
    return m_impl->initialized;
}

QVector<Res10Detection> VulkanRes10NcnnFaceDetector::inferFromTensor(const VulkanTensorBuffer& inputTensor,
                                                                     int imageWidth,
                                                                     int imageHeight,
                                                                     float threshold,
                                                                     QString* errorMessage)
{
    QVector<Res10Detection> out;
    if (!m_impl->initialized) {
        setError(errorMessage, QStringLiteral("Res10 ncnn detector is not initialized"));
        return out;
    }
    if (inputTensor.buffer == VK_NULL_HANDLE || inputTensor.byteSize < FaceDetectorTensorSpec{}.byteSize()) {
        setError(errorMessage, QStringLiteral("Res10 ncnn input tensor buffer is missing or too small"));
        return out;
    }

#if !JCUT_HAVE_NCNN
    Q_UNUSED(inputTensor);
    Q_UNUSED(imageWidth);
    Q_UNUSED(imageHeight);
    Q_UNUSED(threshold);
    setError(errorMessage, QStringLiteral("JCut was built without ncnn; Vulkan Res10 zero-copy detector is unavailable."));
    return out;
#else
    m_impl->inputMemory.buffer = inputTensor.buffer;
    m_impl->inputMemory.offset = 0;
    m_impl->inputMemory.capacity = static_cast<size_t>(inputTensor.byteSize);
    m_impl->inputMemory.memory = VK_NULL_HANDLE;
    m_impl->inputMemory.mapped_ptr = nullptr;
    m_impl->inputMemory.memory_type_index = 0;
    m_impl->inputMemory.access_flags = VK_ACCESS_SHADER_WRITE_BIT;
    m_impl->inputMemory.stage_flags = VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT;
    m_impl->inputMemory.refcount = 0;

    ncnn::VkMat gpuInput(300, 300, 3, &m_impl->inputMemory, 4u, m_impl->net.opt.blob_vkallocator);
    ncnn::Extractor ex = m_impl->net.create_extractor();
    // External VkBufferMemory is owned by JCut and has no ncnn refcount.
    // ncnn light mode dereferences/refcounts intermediates, so keep it off
    // for this zero-copy bridge.
    ex.set_light_mode(false);
    if (ex.input("data", gpuInput) != 0) {
        setError(errorMessage, QStringLiteral("Res10 ncnn model does not expose expected input blob 'data'"));
        return out;
    }

    ncnn::Mat detections;
    if (ex.extract("detection_out", detections) != 0) {
        setError(errorMessage, QStringLiteral("Res10 ncnn model does not expose expected output blob 'detection_out'"));
        return out;
    }

    if (detections.empty()) {
        return out;
    }

    const int cols = detections.w >= 6 ? detections.w : 0;
    if (cols != 6 && cols != 7) {
        setError(errorMessage, QStringLiteral("Res10 ncnn detector produced unsupported detection row width: %1").arg(cols));
        return out;
    }
    const int rows = detections.h > 0 ? detections.h : detections.total() / cols;
    const float* values = static_cast<const float*>(detections.data);
    for (int i = 0; i < rows; ++i) {
        const float* row = values + (i * cols);
        const float confidence = cols == 6 ? row[1] : row[2];
        if (!std::isfinite(confidence) || confidence < threshold || confidence > 1.0f) {
            continue;
        }
        const float nx0 = cols == 6 ? row[2] : row[3];
        const float ny0 = cols == 6 ? row[3] : row[4];
        const float nx1 = cols == 6 ? row[4] : row[5];
        const float ny1 = cols == 6 ? row[5] : row[6];
        if (!std::isfinite(nx0) || !std::isfinite(ny0) ||
            !std::isfinite(nx1) || !std::isfinite(ny1)) {
            continue;
        }
        const float x0 = std::clamp(nx0, 0.0f, 1.0f) * imageWidth;
        const float y0 = std::clamp(ny0, 0.0f, 1.0f) * imageHeight;
        const float x1 = std::clamp(nx1, 0.0f, 1.0f) * imageWidth;
        const float y1 = std::clamp(ny1, 0.0f, 1.0f) * imageHeight;
        QRectF box(QPointF(x0, y0), QPointF(x1, y1));
        box = box.normalized().intersected(QRectF(0, 0, imageWidth, imageHeight));
        if (box.width() >= 8.0 && box.height() >= 8.0) {
            out.push_back({box, confidence});
        }
    }
    std::sort(out.begin(), out.end(), [](const Res10Detection& a, const Res10Detection& b) {
        return a.confidence > b.confidence;
    });
    return out;
#endif
}

QString VulkanRes10NcnnFaceDetector::backendId() const
{
    return QStringLiteral("res10_ssd_ncnn_vulkan_zero_copy_v1");
}

} // namespace jcut::vulkan_detector
