#pragma once

#include "vulkan_zero_copy_face_detector.h"

#include <QRectF>
#include <QString>
#include <QVector>

namespace jcut::vulkan_detector {

struct Res10Detection {
    QRectF box;
    float confidence = 0.0f;
};

class VulkanRes10NcnnFaceDetector {
public:
    VulkanRes10NcnnFaceDetector();
    ~VulkanRes10NcnnFaceDetector();

    VulkanRes10NcnnFaceDetector(const VulkanRes10NcnnFaceDetector&) = delete;
    VulkanRes10NcnnFaceDetector& operator=(const VulkanRes10NcnnFaceDetector&) = delete;

    bool initialize(const VulkanDeviceContext& context,
                    const QString& paramPath,
                    const QString& binPath,
                    QString* errorMessage = nullptr);
    void release();

    bool isInitialized() const;
    QVector<Res10Detection> inferFromTensor(const VulkanTensorBuffer& inputTensor,
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
