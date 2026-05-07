#pragma once

#include "vulkan_zero_copy_face_detector.h"

#include <QRectF>
#include <QString>
#include <QVector>

#if JCUT_HAVE_OPENCV
#include <opencv2/core.hpp>
#endif

namespace jcut::vulkan_detector {

struct ScrfdDetection {
    QRectF box;
    float confidence = 0.0f;
};

class VulkanScrfdNcnnFaceDetector {
public:
    VulkanScrfdNcnnFaceDetector();
    ~VulkanScrfdNcnnFaceDetector();

    VulkanScrfdNcnnFaceDetector(const VulkanScrfdNcnnFaceDetector&) = delete;
    VulkanScrfdNcnnFaceDetector& operator=(const VulkanScrfdNcnnFaceDetector&) = delete;

    bool initialize(const QString& paramPath,
                    const QString& binPath,
                    bool useVulkan,
                    QString* errorMessage = nullptr);
    bool initialize(const VulkanDeviceContext& context,
                    const QString& paramPath,
                    const QString& binPath,
                    QString* errorMessage = nullptr);
    void release();

    bool isInitialized() const;

#if JCUT_HAVE_OPENCV
    QVector<ScrfdDetection> inferFromBgr(const cv::Mat& bgr,
                                         float threshold,
                                         int targetSize,
                                         QString* errorMessage = nullptr);
#endif
    QVector<ScrfdDetection> inferFromTensor(const VulkanTensorBuffer& inputTensor,
                                            const ScrfdTensorLayout& layout,
                                            int imageWidth,
                                            int imageHeight,
                                            float threshold,
                                            QString* errorMessage = nullptr);

    QString backendId() const;

private:
    struct Impl;
    Impl* m_impl = nullptr;
};

} // namespace jcut::vulkan_detector
