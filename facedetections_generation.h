#pragma once

#include "facedetections_runtime.h"

#include <QImage>
#include <QRect>
#include <QRectF>
#include <QString>
#include <QVector>

#include <vector>

#if JCUT_HAVE_OPENCV
#include <opencv2/core.hpp>
#endif

namespace jcut::facedetections {

struct FacestreamSmoothingSettings {
    bool smoothTranslation = false;
    bool smoothScale = false;
};

extern FacestreamSmoothingSettings g_facedetectionsSmoothingSettings;

struct FacestreamContribTrackingSettings {
    int redetectStrideSamples = 5;
    bool allowTrackerOnlyPropagation = true;
};

extern FacestreamContribTrackingSettings g_facedetectionsContribTrackingSettings;

enum class FacestreamDetectorPreset {
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

bool isDnnFacestreamPreset(FacestreamDetectorPreset preset);
bool isRelaxedDnnFacestreamPreset(FacestreamDetectorPreset preset);
QString formatEtaSeconds(double secondsRemaining);
QString findCascadeFile(const QString& name);

#if JCUT_HAVE_OPENCV
struct WeightedDetection {
    cv::Rect box;
    double weight = 0.0;
};

cv::Mat qImageToBgrMat(const QImage& image);
QImage buildScanPreview(const QImage& source,
                        const std::vector<cv::Rect>& detections,
                        int detectionCount,
                        const QRectF& roiRect = QRectF());
std::vector<WeightedDetection> filterAndSuppressDetections(
    std::vector<WeightedDetection> detections,
    const QSize& frameSize);
bool ensureFaceDnnModel(const QString& baseDir,
                        QString* prototxtOut,
                        QString* modelOut);
#endif

} // namespace jcut::facedetections
