#include "vulkan_facedetections_offscreen_opencv_dnn.h"

#include "facedetections_generation.h"

#include <QDir>
#include <QElapsedTimer>
#include <QRectF>

#include <algorithm>

#if JCUT_HAVE_OPENCV
bool hasVulkanDnnTarget() {
  const std::vector<cv::dnn::Target> targets =
      cv::dnn::getAvailableTargets(cv::dnn::DNN_BACKEND_VKCOM);
  return std::find(targets.begin(), targets.end(),
                   cv::dnn::DNN_TARGET_VULKAN) != targets.end();
}

bool initializeTrainedVulkanDnn(cv::dnn::Net *net, QString *error) {
  if (!net) {
    if (error)
      *error = QStringLiteral("Invalid trained Vulkan DNN runtime.");
    return false;
  }
  if (!hasVulkanDnnTarget()) {
    if (error)
      *error = QStringLiteral("OpenCV VKCOM/Vulkan DNN target is unavailable "
                              "in this build/runtime.");
    return false;
  }
  QString proto;
  QString model;
  if (!jcut::facedetections::ensureFaceDnnModel(QDir::currentPath(), &proto,
                                                &model)) {
    if (error)
      *error = QStringLiteral("OpenCV Res10 SSD face detector assets are "
                              "missing or could not be downloaded.");
    return false;
  }
  try {
    *net = cv::dnn::readNetFromCaffe(proto.toStdString(), model.toStdString());
    net->setPreferableBackend(cv::dnn::DNN_BACKEND_VKCOM);
    net->setPreferableTarget(cv::dnn::DNN_TARGET_VULKAN);
  } catch (const cv::Exception &e) {
    if (error)
      *error = QStringLiteral("Failed to initialize trained Vulkan DNN: %1")
                   .arg(e.what());
    return false;
  }
  return true;
}

QVector<Detection> detectTrainedVulkanDnn(cv::dnn::Net *net, const cv::Mat &bgr,
                                          float threshold, double *inferenceMs,
                                          QString *error) {
  QVector<Detection> out;
  if (!net || bgr.empty()) {
    return out;
  }
  QElapsedTimer timer;
  timer.start();
  try {
    cv::Mat blob =
        cv::dnn::blobFromImage(bgr, 1.0, cv::Size(300, 300),
                               cv::Scalar(104.0, 177.0, 123.0), false, false);
    net->setInput(blob);
    const cv::Mat detections = net->forward();
    if (detections.dims != 4 || detections.size[2] <= 0 ||
        detections.size[3] < 7) {
      if (error)
        *error = QStringLiteral("Unexpected trained Vulkan DNN output shape.");
      return out;
    }
    const int width = bgr.cols;
    const int height = bgr.rows;
    for (int i = 0; i < detections.size[2]; ++i) {
      const float *row = detections.ptr<float>(0, 0, i);
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
  } catch (const cv::Exception &e) {
    if (error)
      *error = QStringLiteral("Trained Vulkan DNN inference failed: %1")
                   .arg(e.what());
    return {};
  }
  if (inferenceMs) {
    *inferenceMs = static_cast<double>(timer.nsecsElapsed()) / 1'000'000.0;
  }
  std::sort(out.begin(), out.end(), [](const Detection &a, const Detection &b) {
    return a.confidence > b.confidence;
  });
  return out;
}
#endif
