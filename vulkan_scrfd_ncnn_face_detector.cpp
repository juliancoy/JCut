#include "vulkan_scrfd_ncnn_face_detector.h"

#include <QElapsedTimer>
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
#include <array>
#include <cmath>
#include <memory>
#include <optional>
#include <vector>

#if !defined(JCUT_NCNN_CAN_WRAP_VULKAN_DEVICE)
#define JCUT_NCNN_CAN_WRAP_VULKAN_DEVICE 0
#endif

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

struct ScrfdOutputBinding {
    int featStride = 0;
    int baseSize = 0;
    std::vector<const char*> scoreBlobCandidates;
    std::vector<const char*> bboxBlobCandidates;
};

#if JCUT_HAVE_NCNN
struct ResolvedScrfdOutputBinding {
    int featStride = 0;
    int scoreBlobIndex = -1;
    int bboxBlobIndex = -1;
    ncnn::Mat anchors;
};
#endif

double intersectionArea(const FaceObject& a, const FaceObject& b)
{
    const QRectF inter = a.rect.intersected(b.rect);
    return inter.width() * inter.height();
}

std::array<ScrfdOutputBinding, 3> scrfdOutputBindingsForVariant(const QString& modelVariant)
{
    const bool legacyNumericFirst = (modelVariant == QStringLiteral("500m"));
    const std::array<ScrfdOutputBinding, 3> bindings{{
        {8,
         16,
         legacyNumericFirst ? std::vector<const char*>{"412", "score_8"} : std::vector<const char*>{"score_8", "412"},
         legacyNumericFirst ? std::vector<const char*>{"415", "bbox_8"} : std::vector<const char*>{"bbox_8", "415"}},
        {16,
         64,
         legacyNumericFirst ? std::vector<const char*>{"474", "score_16"} : std::vector<const char*>{"score_16", "474"},
         legacyNumericFirst ? std::vector<const char*>{"477", "bbox_16"} : std::vector<const char*>{"bbox_16", "477"}},
        {32,
         256,
         legacyNumericFirst ? std::vector<const char*>{"536", "score_32"} : std::vector<const char*>{"score_32", "536"},
         legacyNumericFirst ? std::vector<const char*>{"539", "bbox_32"} : std::vector<const char*>{"bbox_32", "539"}},
    }};
    return bindings;
}

template <typename BlobT>
bool extractFirstAvailableBlob(ncnn::Extractor& extractor,
                               const std::vector<const char*>& candidates,
                               BlobT* blobOut,
                               QString* resolvedNameOut)
{
    if (!blobOut) {
        return false;
    }
    for (const char* candidate : candidates) {
        BlobT blob;
        if (extractor.extract(candidate, blob) == 0) {
            *blobOut = blob;
            if (resolvedNameOut) {
                *resolvedNameOut = QString::fromLatin1(candidate);
            }
            return true;
        }
    }
    return false;
}

bool bindScrfdInput(ncnn::Extractor& extractor,
                    const ncnn::Mat& input,
                    QString* errorMessage)
{
    static const std::array<const char*, 3> candidates{{"input.1", "input", "data"}};
    for (const char* candidate : candidates) {
        if (extractor.input(candidate, input) == 0) {
            return true;
        }
    }
    setError(errorMessage,
             QStringLiteral("SCRFD model does not expose a supported input blob (tried input.1/input/data)"));
    return false;
}

bool bindScrfdInput(ncnn::Extractor& extractor,
                    const ncnn::VkMat& input,
                    QString* errorMessage)
{
    static const std::array<const char*, 3> candidates{{"input.1", "input", "data"}};
    for (const char* candidate : candidates) {
        if (extractor.input(candidate, input) == 0) {
            return true;
        }
    }
    setError(errorMessage,
             QStringLiteral("SCRFD model does not expose a supported input blob (tried input.1/input/data)"));
    return false;
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

int resolveFirstAvailableBlobIndex(const std::vector<int>& indexes,
                                   const std::vector<const char*>& names,
                                   const std::vector<const char*>& candidates)
{
    for (const char* candidate : candidates) {
        for (size_t i = 0; i < names.size() && i < indexes.size(); ++i) {
            if (QString::fromLatin1(names[i]) == QString::fromLatin1(candidate)) {
                return indexes[i];
            }
        }
    }
    return -1;
}

int resolveFirstAvailableInputBlobIndex(const ncnn::Net& net)
{
    static const std::array<const char*, 3> candidates{{"input.1", "input", "data"}};
    return resolveFirstAvailableBlobIndex(net.input_indexes(), net.input_names(),
                                          std::vector<const char*>(candidates.begin(), candidates.end()));
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
    int inputBlobIndex = -1;
    std::array<ResolvedScrfdOutputBinding, 3> resolvedOutputBindings{};
#endif
    VulkanDeviceContext context;
    bool initialized = false;
    bool zeroCopyInput = false;
    QString modelVariant = QStringLiteral("500m");
    NcnnInferenceStats lastStats;
};

#if JCUT_HAVE_NCNN
bool resolveScrfdNetworkBindings(const ncnn::Net& net,
                                 const QString& modelVariant,
                                 int* inputBlobIndexOut,
                                 std::array<ResolvedScrfdOutputBinding, 3>* outputBindingsOut,
                                 QString* errorMessage)
{
    if (!inputBlobIndexOut || !outputBindingsOut) {
        setError(errorMessage, QStringLiteral("SCRFD detector implementation is unavailable"));
        return false;
    }
    *inputBlobIndexOut = resolveFirstAvailableInputBlobIndex(net);
    if (*inputBlobIndexOut < 0) {
        setError(errorMessage,
                 QStringLiteral("SCRFD model does not expose a supported input blob (tried input.1/input/data)"));
        return false;
    }

    ncnn::Mat ratios(1);
    ratios[0] = 1.0f;
    ncnn::Mat scales(2);
    scales[0] = 1.0f;
    scales[1] = 2.0f;

    const auto bindings = scrfdOutputBindingsForVariant(modelVariant);
    for (size_t i = 0; i < bindings.size(); ++i) {
        const ScrfdOutputBinding& binding = bindings[i];
        ResolvedScrfdOutputBinding resolved;
        resolved.featStride = binding.featStride;
        resolved.scoreBlobIndex =
            resolveFirstAvailableBlobIndex(net.output_indexes(), net.output_names(),
                                           binding.scoreBlobCandidates);
        resolved.bboxBlobIndex =
            resolveFirstAvailableBlobIndex(net.output_indexes(), net.output_names(),
                                           binding.bboxBlobCandidates);
        if (resolved.scoreBlobIndex < 0 || resolved.bboxBlobIndex < 0) {
            setError(errorMessage,
                     QStringLiteral("SCRFD model does not expose supported output blobs for stride %1")
                         .arg(binding.featStride));
            return false;
        }
        resolved.anchors = generateAnchors(binding.baseSize, ratios, scales);
        (*outputBindingsOut)[i] = resolved;
    }
    return true;
}
#endif

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
    const QString lowerParamPath = QFileInfo(paramPath).fileName().toLower();
    if (lowerParamPath.contains(QStringLiteral("scrfd_1g"))) {
        m_impl->modelVariant = QStringLiteral("1g");
    } else if (lowerParamPath.contains(QStringLiteral("scrfd_2.5g"))) {
        m_impl->modelVariant = QStringLiteral("2.5g");
    } else if (lowerParamPath.contains(QStringLiteral("scrfd_10g"))) {
        m_impl->modelVariant = QStringLiteral("10g");
    } else if (lowerParamPath.contains(QStringLiteral("scrfd_34g"))) {
        m_impl->modelVariant = QStringLiteral("34g");
    } else {
        m_impl->modelVariant = QStringLiteral("500m");
    }
    if (!resolveScrfdNetworkBindings(m_impl->net, m_impl->modelVariant,
                                     &m_impl->inputBlobIndex,
                                     &m_impl->resolvedOutputBindings,
                                     errorMessage)) {
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
#if JCUT_NCNN_CAN_WRAP_VULKAN_DEVICE
    m_impl->vkdev = std::make_unique<ncnn::VulkanDevice>(
        deviceIndex, context.device, context.queue, context.queueFamilyIndex);
#else
    Q_UNUSED(deviceIndex);
    setError(errorMessage,
             QStringLiteral("ncnn build cannot wrap JCut's Vulkan device; SCRFD zero-copy detector is unavailable."));
    return false;
#endif
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
    const QString lowerParamPath = QFileInfo(paramPath).fileName().toLower();
    if (lowerParamPath.contains(QStringLiteral("scrfd_1g"))) {
        m_impl->modelVariant = QStringLiteral("1g");
    } else if (lowerParamPath.contains(QStringLiteral("scrfd_2.5g"))) {
        m_impl->modelVariant = QStringLiteral("2.5g");
    } else if (lowerParamPath.contains(QStringLiteral("scrfd_10g"))) {
        m_impl->modelVariant = QStringLiteral("10g");
    } else if (lowerParamPath.contains(QStringLiteral("scrfd_34g"))) {
        m_impl->modelVariant = QStringLiteral("34g");
    } else {
        m_impl->modelVariant = QStringLiteral("500m");
    }
    if (!resolveScrfdNetworkBindings(m_impl->net, m_impl->modelVariant,
                                     &m_impl->inputBlobIndex,
                                     &m_impl->resolvedOutputBindings,
                                     errorMessage)) {
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
    m_impl->inputBlobIndex = -1;
    m_impl->resolvedOutputBindings = {};
#endif
    m_impl->context = {};
    m_impl->initialized = false;
    m_impl->zeroCopyInput = false;
    m_impl->modelVariant = QStringLiteral("500m");
    m_impl->lastStats = {};
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
    if (m_impl->inputBlobIndex < 0 || ex.input(m_impl->inputBlobIndex, inputPad) != 0) {
        setError(errorMessage,
                 QStringLiteral("SCRFD model does not expose a supported input blob (tried input.1/input/data)"));
        return out;
    }

    std::vector<FaceObject> proposals;
    auto extractLevel = [&](const ResolvedScrfdOutputBinding& binding, double* stageMs) -> bool {
        QElapsedTimer stageTimer;
        stageTimer.start();
        ncnn::Mat scoreBlob;
        ncnn::Mat bboxBlob;
        if (binding.scoreBlobIndex < 0 || binding.bboxBlobIndex < 0 ||
            ex.extract(binding.scoreBlobIndex, scoreBlob) != 0 ||
            ex.extract(binding.bboxBlobIndex, bboxBlob) != 0) {
            setError(errorMessage,
                     QStringLiteral("SCRFD model does not expose supported output blobs for stride %1")
                         .arg(binding.featStride));
            return false;
        }
        generateProposals(binding.anchors, binding.featStride, scoreBlob, bboxBlob, threshold, &proposals);
        return true;
    };
    for (const ResolvedScrfdOutputBinding& binding : m_impl->resolvedOutputBindings) {
        if (!extractLevel(binding, nullptr)) {
            return out;
        }
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
    m_impl->lastStats = {};
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
    QElapsedTimer totalTimer;
    totalTimer.start();
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
    QElapsedTimer inputTimer;
    inputTimer.start();
    if (m_impl->inputBlobIndex < 0 || ex.input(m_impl->inputBlobIndex, gpuInput) != 0) {
        setError(errorMessage,
                 QStringLiteral("SCRFD model does not expose a supported input blob (tried input.1/input/data)"));
        return out;
    }
    m_impl->lastStats.inputMs = static_cast<double>(inputTimer.nsecsElapsed()) / 1'000'000.0;

    std::vector<FaceObject> proposals;
    auto extractLevel = [&](const ResolvedScrfdOutputBinding& binding, double* stageMs) -> bool {
        QElapsedTimer stageTimer;
        stageTimer.start();
        ncnn::Mat scoreBlob;
        ncnn::Mat bboxBlob;
        if (binding.scoreBlobIndex < 0 || binding.bboxBlobIndex < 0 ||
            ex.extract(binding.scoreBlobIndex, scoreBlob) != 0 ||
            ex.extract(binding.bboxBlobIndex, bboxBlob) != 0) {
            setError(errorMessage,
                     QStringLiteral("SCRFD model does not expose supported output blobs for stride %1")
                         .arg(binding.featStride));
            return false;
        }
        generateProposals(binding.anchors, binding.featStride, scoreBlob, bboxBlob, threshold, &proposals);
        if (stageMs) {
            *stageMs = static_cast<double>(stageTimer.nsecsElapsed()) / 1'000'000.0;
        }
        return true;
    };
    QElapsedTimer extractTimer;
    extractTimer.start();
    for (const ResolvedScrfdOutputBinding& binding : m_impl->resolvedOutputBindings) {
        double* stageMs = nullptr;
        if (binding.featStride == 8) {
            stageMs = &m_impl->lastStats.extractLevel8Ms;
        } else if (binding.featStride == 16) {
            stageMs = &m_impl->lastStats.extractLevel16Ms;
        } else if (binding.featStride == 32) {
            stageMs = &m_impl->lastStats.extractLevel32Ms;
        }
        if (!extractLevel(binding, stageMs)) {
            return out;
        }
    }
    m_impl->lastStats.extractMs = static_cast<double>(extractTimer.nsecsElapsed()) / 1'000'000.0;

    QElapsedTimer postTimer;
    postTimer.start();
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
    m_impl->lastStats.postMs = static_cast<double>(postTimer.nsecsElapsed()) / 1'000'000.0;
    m_impl->lastStats.totalMs = static_cast<double>(totalTimer.nsecsElapsed()) / 1'000'000.0;
    return out;
#endif
}

NcnnInferenceStats VulkanScrfdNcnnFaceDetector::lastInferenceStats() const
{
    return m_impl->lastStats;
}

QString VulkanScrfdNcnnFaceDetector::backendId() const
{
    if (m_impl->zeroCopyInput) {
        return QStringLiteral("scrfd_%1_ncnn_vulkan_zero_copy_v1").arg(m_impl->modelVariant);
    }
    return QStringLiteral("scrfd_%1_ncnn_vulkan_materialized_input_v1").arg(m_impl->modelVariant);
}

} // namespace jcut::vulkan_detector
