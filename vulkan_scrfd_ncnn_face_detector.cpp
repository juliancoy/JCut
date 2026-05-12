#include "vulkan_scrfd_ncnn_face_detector.h"

#include <QFileInfo>

#if JCUT_HAVE_NCNN
#include <command.h>
#include <gpu.h>
#include <mat.h>
#include <net.h>
#endif

#if JCUT_HAVE_OPENCV
#include <opencv2/imgproc.hpp>
#endif

#include <algorithm>
#include <cmath>
#include <memory>
#include <vector>

namespace jcut::vulkan_detector {
namespace {

void setError(QString* out, const QString& value)
{
    if (out) {
        *out = value;
    }
}

struct FaceObject {
    QRectF rect;
    float prob = 0.0f;
};

double intersectionArea(const FaceObject& a, const FaceObject& b)
{
    const QRectF inter = a.rect.intersected(b.rect);
    return inter.width() * inter.height();
}

void nmsSortedBboxes(const std::vector<FaceObject>& faceobjects,
                    std::vector<int>* picked,
                    float nmsThreshold)
{
    picked->clear();
    std::vector<double> areas(faceobjects.size());
    for (int i = 0; i < static_cast<int>(faceobjects.size()); ++i) {
        areas[i] = faceobjects[i].rect.width() * faceobjects[i].rect.height();
    }
    for (int i = 0; i < static_cast<int>(faceobjects.size()); ++i) {
        bool keep = true;
        for (int pickedIndex : *picked) {
            const double inter = intersectionArea(faceobjects[i], faceobjects[pickedIndex]);
            const double uni = areas[i] + areas[pickedIndex] - inter;
            if (uni > 0.0 && inter / uni > nmsThreshold) {
                keep = false;
                break;
            }
        }
        if (keep) {
            picked->push_back(i);
        }
    }
}

#if JCUT_HAVE_NCNN
bool validContext(const VulkanDeviceContext& context)
{
    return context.physicalDevice != VK_NULL_HANDLE &&
           context.device != VK_NULL_HANDLE &&
           context.queue != VK_NULL_HANDLE;
}

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

ncnn::Mat generateAnchors(int baseSize, const ncnn::Mat& ratios, const ncnn::Mat& scales)
{
    const int numRatio = ratios.w;
    const int numScale = scales.w;
    ncnn::Mat anchors;
    anchors.create(4, numRatio * numScale);
    for (int i = 0; i < numRatio; ++i) {
        const float ar = ratios[i];
        const int rw = qRound(baseSize / std::sqrt(ar));
        const int rh = qRound(rw * ar);
        for (int j = 0; j < numScale; ++j) {
            const float scale = scales[j];
            const float rsw = rw * scale;
            const float rsh = rh * scale;
            float* anchor = anchors.row(i * numScale + j);
            anchor[0] = -rsw * 0.5f;
            anchor[1] = -rsh * 0.5f;
            anchor[2] = rsw * 0.5f;
            anchor[3] = rsh * 0.5f;
        }
    }
    return anchors;
}

void generateProposals(const ncnn::Mat& anchors,
                       int featStride,
                       const ncnn::Mat& scoreBlob,
                       const ncnn::Mat& bboxBlob,
                       float threshold,
                       std::vector<FaceObject>* faceobjects)
{
    const int w = scoreBlob.w;
    const int h = scoreBlob.h;
    const int numAnchors = anchors.h;
    for (int q = 0; q < numAnchors; ++q) {
        const float* anchor = anchors.row(q);
        const ncnn::Mat score = scoreBlob.channel(q);
        const ncnn::Mat bbox = bboxBlob.channel_range(q * 4, 4);
        float anchorY = anchor[1];
        const float anchorW = anchor[2] - anchor[0];
        const float anchorH = anchor[3] - anchor[1];
        for (int y = 0; y < h; ++y) {
            float anchorX = anchor[0];
            for (int x = 0; x < w; ++x) {
                const int index = y * w + x;
                const float prob = score[index];
                if (prob >= threshold) {
                    const float dx = bbox.channel(0)[index] * featStride;
                    const float dy = bbox.channel(1)[index] * featStride;
                    const float dw = bbox.channel(2)[index] * featStride;
                    const float dh = bbox.channel(3)[index] * featStride;
                    const float cx = anchorX + anchorW * 0.5f;
                    const float cy = anchorY + anchorH * 0.5f;
                    const float x0 = cx - dx;
                    const float y0 = cy - dy;
                    const float x1 = cx + dw;
                    const float y1 = cy + dh;
                    FaceObject obj;
                    obj.rect = QRectF(QPointF(x0, y0), QPointF(x1 + 1.0f, y1 + 1.0f)).normalized();
                    obj.prob = prob;
                    faceobjects->push_back(obj);
                }
                anchorX += featStride;
            }
            anchorY += featStride;
        }
    }
}
#endif

} // namespace

struct VulkanScrfdNcnnFaceDetector::Impl {
#if JCUT_HAVE_NCNN
    std::unique_ptr<ncnn::VulkanDevice> vkdev;
    ncnn::Net net;
    ncnn::VkBufferMemory inputMemory{};
#endif
    VulkanDeviceContext context;
    bool initialized = false;
    bool zeroCopyInput = false;
};

VulkanScrfdNcnnFaceDetector::VulkanScrfdNcnnFaceDetector()
    : m_impl(new Impl)
{
}

VulkanScrfdNcnnFaceDetector::~VulkanScrfdNcnnFaceDetector()
{
    release();
    delete m_impl;
}

bool VulkanScrfdNcnnFaceDetector::initialize(const QString& paramPath,
                                             const QString& binPath,
                                             bool useVulkan,
                                             QString* errorMessage)
{
    if (m_impl->initialized) {
        return true;
    }
    if (!QFileInfo::exists(paramPath) || !QFileInfo::exists(binPath)) {
        setError(errorMessage,
                 QStringLiteral("missing SCRFD ncnn model artifacts: %1 and %2").arg(paramPath, binPath));
        return false;
    }
#if !JCUT_HAVE_NCNN
    Q_UNUSED(paramPath);
    Q_UNUSED(binPath);
    Q_UNUSED(useVulkan);
    setError(errorMessage, QStringLiteral("JCut was built without ncnn; SCRFD detector is unavailable."));
    return false;
#else
    if (useVulkan) {
        ncnn::create_gpu_instance();
        m_impl->net.opt.use_vulkan_compute = true;
    }
    if (m_impl->net.load_param(paramPath.toLocal8Bit().constData()) != 0) {
        setError(errorMessage, QStringLiteral("failed to load SCRFD ncnn param file: %1").arg(paramPath));
        release();
        return false;
    }
    if (m_impl->net.load_model(binPath.toLocal8Bit().constData()) != 0) {
        setError(errorMessage, QStringLiteral("failed to load SCRFD ncnn bin file: %1").arg(binPath));
        release();
        return false;
    }
    m_impl->initialized = true;
    m_impl->zeroCopyInput = false;
    return true;
#endif
}

bool VulkanScrfdNcnnFaceDetector::initialize(const VulkanDeviceContext& context,
                                             const QString& paramPath,
                                             const QString& binPath,
                                             QString* errorMessage)
{
    if (m_impl->initialized) {
        return true;
    }
    if (!QFileInfo::exists(paramPath) || !QFileInfo::exists(binPath)) {
        setError(errorMessage,
                 QStringLiteral("missing SCRFD ncnn model artifacts: %1 and %2").arg(paramPath, binPath));
        return false;
    }
#if !JCUT_HAVE_NCNN
    Q_UNUSED(context);
    Q_UNUSED(paramPath);
    Q_UNUSED(binPath);
    setError(errorMessage, QStringLiteral("JCut was built without ncnn; SCRFD detector is unavailable."));
    return false;
#else
    if (!validContext(context)) {
        setError(errorMessage, QStringLiteral("invalid Vulkan device context for SCRFD ncnn detector"));
        return false;
    }
    ncnn::create_gpu_instance();
    const int deviceIndex = matchingNcnnDeviceIndex(context.physicalDevice);
    if (deviceIndex < 0) {
        setError(errorMessage, QStringLiteral("ncnn did not find a Vulkan-capable device for SCRFD."));
        return false;
    }
    m_impl->vkdev = std::make_unique<ncnn::VulkanDevice>(
        deviceIndex, context.device, context.queue, context.queueFamilyIndex);
    if (!m_impl->vkdev->is_valid()) {
        setError(errorMessage, QStringLiteral("failed to wrap JCut Vulkan device for ncnn SCRFD inference"));
        m_impl->vkdev.reset();
        return false;
    }
    m_impl->net.opt.use_vulkan_compute = true;
    m_impl->net.opt.use_fp16_packed = false;
    m_impl->net.opt.use_fp16_storage = false;
    m_impl->net.opt.use_fp16_arithmetic = false;
    m_impl->net.set_vulkan_device(m_impl->vkdev.get());
    if (m_impl->net.load_param(paramPath.toLocal8Bit().constData()) != 0) {
        setError(errorMessage, QStringLiteral("failed to load SCRFD ncnn param file: %1").arg(paramPath));
        release();
        return false;
    }
    if (m_impl->net.load_model(binPath.toLocal8Bit().constData()) != 0) {
        setError(errorMessage, QStringLiteral("failed to load SCRFD ncnn bin file: %1").arg(binPath));
        release();
        return false;
    }
    m_impl->context = context;
    m_impl->initialized = true;
    m_impl->zeroCopyInput = true;
    return true;
#endif
}

void VulkanScrfdNcnnFaceDetector::release()
{
#if JCUT_HAVE_NCNN
    m_impl->net.clear();
    m_impl->vkdev.reset();
    m_impl->inputMemory = {};
#endif
    m_impl->context = {};
    m_impl->initialized = false;
    m_impl->zeroCopyInput = false;
}

bool VulkanScrfdNcnnFaceDetector::isInitialized() const
{
    return m_impl->initialized;
}

#if JCUT_HAVE_OPENCV
QVector<ScrfdDetection> VulkanScrfdNcnnFaceDetector::inferFromBgr(const cv::Mat& bgr,
                                                                  float threshold,
                                                                  int targetSize,
                                                                  QString* errorMessage)
{
    QVector<ScrfdDetection> out;
    if (!m_impl->initialized) {
        setError(errorMessage, QStringLiteral("SCRFD ncnn detector is not initialized"));
        return out;
    }
    if (bgr.empty()) {
        return out;
    }
#if !JCUT_HAVE_NCNN
    Q_UNUSED(bgr);
    Q_UNUSED(threshold);
    Q_UNUSED(targetSize);
    setError(errorMessage, QStringLiteral("JCut was built without ncnn; SCRFD detector is unavailable."));
    return out;
#else
    const int width = bgr.cols;
    const int height = bgr.rows;
    targetSize = qMax(320, targetSize);
    int resizedW = width;
    int resizedH = height;
    float scale = 1.0f;
    if (resizedW > resizedH) {
        scale = static_cast<float>(targetSize) / resizedW;
        resizedW = targetSize;
        resizedH = qMax(1, qRound(resizedH * scale));
    } else {
        scale = static_cast<float>(targetSize) / resizedH;
        resizedH = targetSize;
        resizedW = qMax(1, qRound(resizedW * scale));
    }

    ncnn::Mat input = ncnn::Mat::from_pixels_resize(bgr.data,
                                                    ncnn::Mat::PIXEL_BGR2RGB,
                                                    width,
                                                    height,
                                                    resizedW,
                                                    resizedH);
    const int wpad = ((resizedW + 31) / 32) * 32 - resizedW;
    const int hpad = ((resizedH + 31) / 32) * 32 - resizedH;
    ncnn::Mat inputPad;
    ncnn::copy_make_border(input,
                           inputPad,
                           hpad / 2,
                           hpad - hpad / 2,
                           wpad / 2,
                           wpad - wpad / 2,
                           ncnn::BORDER_CONSTANT,
                           0.0f);
    const float meanVals[3] = {127.5f, 127.5f, 127.5f};
    const float normVals[3] = {1.0f / 128.0f, 1.0f / 128.0f, 1.0f / 128.0f};
    inputPad.substract_mean_normalize(meanVals, normVals);

    ncnn::Extractor ex = m_impl->net.create_extractor();
    if (ex.input("input.1", inputPad) != 0) {
        setError(errorMessage, QStringLiteral("SCRFD model does not expose expected input blob 'input.1'"));
        return out;
    }

    std::vector<FaceObject> proposals;
    auto extractLevel = [&](const char* scoreName, const char* bboxName, int baseSize, int featStride) -> bool {
        ncnn::Mat scoreBlob;
        ncnn::Mat bboxBlob;
        if (ex.extract(scoreName, scoreBlob) != 0 || ex.extract(bboxName, bboxBlob) != 0) {
            setError(errorMessage,
                     QStringLiteral("SCRFD model does not expose expected output blobs %1/%2")
                         .arg(QString::fromLatin1(scoreName), QString::fromLatin1(bboxName)));
            return false;
        }
        ncnn::Mat ratios(1);
        ratios[0] = 1.0f;
        ncnn::Mat scales(2);
        scales[0] = 1.0f;
        scales[1] = 2.0f;
        const ncnn::Mat anchors = generateAnchors(baseSize, ratios, scales);
        generateProposals(anchors, featStride, scoreBlob, bboxBlob, threshold, &proposals);
        return true;
    };
    if (!extractLevel("412", "415", 16, 8) ||
        !extractLevel("474", "477", 64, 16) ||
        !extractLevel("536", "539", 256, 32)) {
        return out;
    }

    std::sort(proposals.begin(), proposals.end(), [](const FaceObject& a, const FaceObject& b) {
        return a.prob > b.prob;
    });
    std::vector<int> picked;
    nmsSortedBboxes(proposals, &picked, 0.45f);
    out.reserve(static_cast<int>(picked.size()));
    for (int index : picked) {
        FaceObject obj = proposals[index];
        const float x0 = std::clamp(static_cast<float>((obj.rect.x() - wpad / 2.0f) / scale), 0.0f, static_cast<float>(width - 1));
        const float y0 = std::clamp(static_cast<float>((obj.rect.y() - hpad / 2.0f) / scale), 0.0f, static_cast<float>(height - 1));
        const float x1 = std::clamp(static_cast<float>((obj.rect.right() - wpad / 2.0f) / scale), 0.0f, static_cast<float>(width - 1));
        const float y1 = std::clamp(static_cast<float>((obj.rect.bottom() - hpad / 2.0f) / scale), 0.0f, static_cast<float>(height - 1));
        QRectF box(QPointF(x0, y0), QPointF(x1, y1));
        box = box.normalized();
        if (box.width() >= 4.0 && box.height() >= 4.0) {
            out.push_back({box, obj.prob});
        }
    }
    return out;
#endif
}
#endif

QVector<ScrfdDetection> VulkanScrfdNcnnFaceDetector::inferFromTensor(const VulkanTensorBuffer& inputTensor,
                                                                     const ScrfdTensorLayout& layout,
                                                                     int imageWidth,
                                                                     int imageHeight,
                                                                     float threshold,
                                                                     QString* errorMessage)
{
    QVector<ScrfdDetection> out;
    if (!m_impl->initialized) {
        setError(errorMessage, QStringLiteral("SCRFD ncnn detector is not initialized"));
        return out;
    }
    if (inputTensor.buffer == VK_NULL_HANDLE || inputTensor.byteSize < layout.byteSize() ||
        layout.inputWidth <= 0 || layout.inputHeight <= 0 || layout.scale <= 0.0f) {
        setError(errorMessage, QStringLiteral("SCRFD zero-copy input tensor is missing or invalid"));
        return out;
    }

#if !JCUT_HAVE_NCNN
    Q_UNUSED(inputTensor);
    Q_UNUSED(layout);
    Q_UNUSED(imageWidth);
    Q_UNUSED(imageHeight);
    Q_UNUSED(threshold);
    setError(errorMessage, QStringLiteral("JCut was built without ncnn; SCRFD detector is unavailable."));
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

    ncnn::VkMat gpuInput(layout.inputWidth,
                         layout.inputHeight,
                         3,
                         &m_impl->inputMemory,
                         4u,
                         m_impl->net.opt.blob_vkallocator);
    ncnn::Extractor ex = m_impl->net.create_extractor();
    ex.set_light_mode(false);
    if (ex.input("input.1", gpuInput) != 0) {
        setError(errorMessage, QStringLiteral("SCRFD model does not expose expected input blob 'input.1'"));
        return out;
    }

    std::vector<FaceObject> proposals;
    auto extractLevel = [&](const char* scoreName, const char* bboxName, int baseSize, int featStride) -> bool {
        ncnn::Mat scoreBlob;
        ncnn::Mat bboxBlob;
        if (ex.extract(scoreName, scoreBlob) != 0 || ex.extract(bboxName, bboxBlob) != 0) {
            setError(errorMessage,
                     QStringLiteral("SCRFD model does not expose expected output blobs %1/%2")
                         .arg(QString::fromLatin1(scoreName), QString::fromLatin1(bboxName)));
            return false;
        }
        ncnn::Mat ratios(1);
        ratios[0] = 1.0f;
        ncnn::Mat scales(2);
        scales[0] = 1.0f;
        scales[1] = 2.0f;
        const ncnn::Mat anchors = generateAnchors(baseSize, ratios, scales);
        generateProposals(anchors, featStride, scoreBlob, bboxBlob, threshold, &proposals);
        return true;
    };
    if (!extractLevel("412", "415", 16, 8) ||
        !extractLevel("474", "477", 64, 16) ||
        !extractLevel("536", "539", 256, 32)) {
        return out;
    }

    std::sort(proposals.begin(), proposals.end(), [](const FaceObject& a, const FaceObject& b) {
        return a.prob > b.prob;
    });
    std::vector<int> picked;
    nmsSortedBboxes(proposals, &picked, 0.45f);
    out.reserve(static_cast<int>(picked.size()));
    for (int index : picked) {
        const FaceObject obj = proposals[index];
        const float x0 = std::clamp(static_cast<float>((obj.rect.x() - layout.padLeft) / layout.scale),
                                    0.0f,
                                    static_cast<float>(imageWidth - 1));
        const float y0 = std::clamp(static_cast<float>((obj.rect.y() - layout.padTop) / layout.scale),
                                    0.0f,
                                    static_cast<float>(imageHeight - 1));
        const float x1 = std::clamp(static_cast<float>((obj.rect.right() - layout.padLeft) / layout.scale),
                                    0.0f,
                                    static_cast<float>(imageWidth - 1));
        const float y1 = std::clamp(static_cast<float>((obj.rect.bottom() - layout.padTop) / layout.scale),
                                    0.0f,
                                    static_cast<float>(imageHeight - 1));
        QRectF box(QPointF(x0, y0), QPointF(x1, y1));
        box = box.normalized().intersected(QRectF(0, 0, imageWidth, imageHeight));
        if (box.width() >= 4.0 && box.height() >= 4.0) {
            out.push_back({box, obj.prob});
        }
    }
    return out;
#endif
}

QString VulkanScrfdNcnnFaceDetector::backendId() const
{
    if (m_impl->zeroCopyInput) {
        return QStringLiteral("scrfd_500m_ncnn_vulkan_zero_copy_v1");
    }
    return QStringLiteral("scrfd_500m_ncnn_vulkan_materialized_input_v1");
}

} // namespace jcut::vulkan_detector
