#include "vulkan_facedetections_offscreen_detection_filters.h"

#include <QJsonArray>
#include <QPointF>
#include <QRectF>
#include <QtGlobal>

#include <algorithm>
#include <cmath>
#include <iostream>

QJsonObject buildRawDetectionFrameRecord(int frameNumber,
                                         const QString &detectorId,
                                         const QSize &frameSize,
                                         const QVector<Detection> &detections) {
  QJsonArray detectionRows;
  for (const Detection &detection : detections) {
    detectionRows.append(
        jcut::facedetections::compactDetectionJson(detection, frameSize));
  }
  return QJsonObject{{QStringLiteral("frame"), frameNumber},
                     {QStringLiteral("detector"), detectorId},
                     {QStringLiteral("frame_width"), frameSize.width()},
                     {QStringLiteral("frame_height"), frameSize.height()},
                     {QStringLiteral("detection_count"), detections.size()},
                     {QStringLiteral("detections"), detectionRows}};
}

QVector<Detection> sanitizeDetections(const QVector<Detection> &raw,
                                      const QSize &frameSize,
                                      int maxFacesPerFrame,
                                      const RuntimeTuning &tuning,
                                      DetectionSanitizeStats *stats) {
  QVector<Detection> out;
  if (stats) {
    *stats = DetectionSanitizeStats{};
    stats->rawCount = raw.size();
  }
  if (!frameSize.isValid()) {
    if (stats) {
      stats->keptBeforeNms = raw.size();
      stats->keptAfterNms = raw.size();
    }
    return raw;
  }
  out.reserve(raw.size());
  const QRectF roiRect(tuning.roiX1 * frameSize.width(),
                       tuning.roiY1 * frameSize.height(),
                       (tuning.roiX2 - tuning.roiX1) * frameSize.width(),
                       (tuning.roiY2 - tuning.roiY1) * frameSize.height());
  const double frameArea = static_cast<double>(frameSize.width()) *
                           static_cast<double>(frameSize.height());
  for (const auto &d : raw) {
    if (!std::isfinite(d.confidence) || d.confidence < 0.0f ||
        d.confidence > 1.0f) {
      if (stats) {
        ++stats->rejectedInvalidConfidence;
      }
      continue;
    }
    QRectF box =
        d.box.intersected(QRectF(0, 0, frameSize.width(), frameSize.height()));
    if (box.width() <= 1.0 || box.height() <= 1.0) {
      if (stats) {
        ++stats->rejectedDegenerate;
      }
      continue;
    }
    const QPointF c = box.center();
    if (!roiRect.contains(c)) {
      if (stats) {
        ++stats->rejectedRoi;
      }
      continue;
    }
    const double areaRatio =
        (box.width() * box.height()) / qMax(1.0, frameArea);
    if (areaRatio < tuning.minFaceAreaRatio ||
        areaRatio > tuning.maxFaceAreaRatio) {
      if (stats) {
        ++stats->rejectedArea;
      }
      continue;
    }
    const double aspect = box.width() / qMax(1.0, box.height());
    if (aspect < tuning.minAspect || aspect > tuning.maxAspect) {
      if (stats) {
        ++stats->rejectedAspect;
      }
      continue;
    }
    out.push_back({box, d.confidence});
  }
  if (stats) {
    stats->keptBeforeNms = out.size();
  }
  std::sort(out.begin(), out.end(), [](const Detection &a, const Detection &b) {
    return a.confidence > b.confidence;
  });
  QVector<Detection> suppressed;
  suppressed.reserve(out.size());
  for (const Detection &candidate : out) {
    bool keep = true;
    for (const Detection &accepted : suppressed) {
      if (jcut::facedetections::continuityIou(candidate.box, accepted.box) >
          tuning.nmsIouThreshold) {
        if (stats) {
          ++stats->rejectedNms;
        }
        keep = false;
        break;
      }
    }
    if (keep) {
      suppressed.push_back(candidate);
    }
  }
  out = suppressed;
  if (stats) {
    stats->keptAfterNms = out.size();
  }
  if (tuning.primaryFaceOnly && out.size() > 1) {
    out.resize(1);
  }
  if (maxFacesPerFrame > 0 && out.size() > maxFacesPerFrame) {
    out.resize(maxFacesPerFrame);
  }
  return out;
}

void logDetectionSanitizeStats(int frameNumber, const QSize &frameSize,
                               const RuntimeTuning &tuning,
                               const DetectionSanitizeStats &stats,
                               const QVector<Detection> &raw) {
  std::cerr << "facedetections_debug" << " frame=" << frameNumber
            << " size=" << frameSize.width() << "x" << frameSize.height()
            << " raw=" << stats.rawCount
            << " kept_pre_nms=" << stats.keptBeforeNms
            << " kept_post_nms=" << stats.keptAfterNms
            << " reject_conf=" << stats.rejectedInvalidConfidence
            << " reject_degenerate=" << stats.rejectedDegenerate
            << " reject_roi=" << stats.rejectedRoi
            << " reject_area=" << stats.rejectedArea
            << " reject_aspect=" << stats.rejectedAspect
            << " reject_nms=" << stats.rejectedNms << " roi=[" << tuning.roiX1
            << "," << tuning.roiY1 << "," << tuning.roiX2 << "," << tuning.roiY2
            << "]" << " area=[" << tuning.minFaceAreaRatio << ","
            << tuning.maxFaceAreaRatio << "]" << " aspect=[" << tuning.minAspect
            << "," << tuning.maxAspect << "]" << "\n";
  const int sampleCount = std::min(static_cast<int>(raw.size()), 3);
  for (int i = 0; i < sampleCount; ++i) {
    const Detection &det = raw.at(i);
    std::cerr << "facedetections_debug_box" << " frame=" << frameNumber
              << " index=" << i << " conf=" << det.confidence << " box=["
              << det.box.x() << "," << det.box.y() << "," << det.box.width()
              << "," << det.box.height() << "]" << "\n";
  }
}

QRectF flipBoxVertically(const QRectF &box, int imageHeight) {
  const qreal height = box.height();
  const qreal flippedTop = static_cast<qreal>(imageHeight) - box.y() - height;
  return QRectF(box.x(), flippedTop, box.width(), height);
}

QVector<Detection> flipDetectionsVertically(const QVector<Detection> &raw,
                                            int imageWidth, int imageHeight) {
  QVector<Detection> out;
  out.reserve(raw.size());
  const QRectF bounds(0.0, 0.0, imageWidth, imageHeight);
  for (const Detection &det : raw) {
    Detection flipped = det;
    flipped.box = flipBoxVertically(det.box, imageHeight).intersected(bounds);
    out.push_back(flipped);
  }
  return out;
}
