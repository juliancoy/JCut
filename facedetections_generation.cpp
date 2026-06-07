#include "facedetections_generation.h"

#include "detector_settings.h"

#include <QDir>
#include <QFileInfo>
#include <QSize>

#include <algorithm>
#include <cmath>

#if JCUT_HAVE_OPENCV
#include <opencv2/imgproc.hpp>
#endif

namespace jcut::facedetections {

FacestreamSmoothingSettings g_facedetectionsSmoothingSettings;
FacestreamContribTrackingSettings g_facedetectionsContribTrackingSettings;

bool isDnnFacestreamPreset(FacestreamDetectorPreset preset)
{
    return preset == FacestreamDetectorPreset::DnnAuto ||
           preset == FacestreamDetectorPreset::NativeVulkanDnn ||
           preset == FacestreamDetectorPreset::NativeCudaDnn ||
           preset == FacestreamDetectorPreset::ScrfdNcnnVulkan;
}

bool isRelaxedDnnFacestreamPreset(FacestreamDetectorPreset preset)
{
    return preset == FacestreamDetectorPreset::NativeVulkanDnn ||
           preset == FacestreamDetectorPreset::ScrfdNcnnVulkan;
}

QString formatEtaSeconds(double secondsRemaining)
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

QString findCascadeFile(const QString& name)
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
namespace {

double iou(const cv::Rect& a, const cv::Rect& b)
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

} // namespace

cv::Mat qImageToBgrMat(const QImage& image)
{
    const QImage rgb = image.convertToFormat(QImage::Format_RGB888);
    cv::Mat rgbMat(rgb.height(), rgb.width(), CV_8UC3,
                   const_cast<uchar*>(rgb.constBits()),
                   static_cast<size_t>(rgb.bytesPerLine()));
    cv::Mat bgr;
    cv::cvtColor(rgbMat, bgr, cv::COLOR_RGB2BGR);
    return bgr;
}

QImage buildScanPreview(const QImage& source,
                        const std::vector<cv::Rect>& detections,
                        int detectionCount,
                        const QRectF& roiRect)
{
    QVector<QRect> boxes;
    boxes.reserve(static_cast<int>(detections.size()));
    for (const cv::Rect& det : detections) {
        boxes.push_back(QRect(det.x, det.y, det.width, det.height));
    }
    return jcut::facedetections::buildScanPreview(source, boxes, detectionCount, roiRect);
}

std::vector<WeightedDetection> filterAndSuppressDetections(
    std::vector<WeightedDetection> detections,
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

bool ensureFaceDnnModel(const QString& baseDir,
                        QString* prototxtOut,
                        QString* modelOut)
{
    return jcut::facedetections::ensureRes10FaceDnnModelAssets(baseDir, prototxtOut, modelOut, nullptr);
}
#endif

} // namespace jcut::facedetections
