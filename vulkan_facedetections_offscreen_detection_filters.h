#pragma once

#include "facedetections_tracking.h"
#include "vulkan_facedetections_offscreen_tuning.h"

#include <QJsonObject>
#include <QSize>
#include <QVector>

using Detection = jcut::facedetections::Detection;
using Track = jcut::facedetections::ContinuityTrack;

struct DetectionSanitizeStats {
  int rawCount = 0;
  int rejectedInvalidConfidence = 0;
  int rejectedDegenerate = 0;
  int rejectedRoi = 0;
  int rejectedArea = 0;
  int rejectedAspect = 0;
  int rejectedNms = 0;
  int keptBeforeNms = 0;
  int keptAfterNms = 0;
};

QJsonObject buildRawDetectionFrameRecord(int frameNumber,
                                         const QString &detectorId,
                                         const QSize &frameSize,
                                         const QVector<Detection> &detections);
QVector<Detection> sanitizeDetections(const QVector<Detection> &raw,
                                      const QSize &frameSize,
                                      int maxFacesPerFrame,
                                      const RuntimeTuning &tuning,
                                      DetectionSanitizeStats *stats = nullptr);
void logDetectionSanitizeStats(int frameNumber, const QSize &frameSize,
                               const RuntimeTuning &tuning,
                               const DetectionSanitizeStats &stats,
                               const QVector<Detection> &raw);
QVector<Detection> flipDetectionsVertically(const QVector<Detection> &raw,
                                            int imageWidth, int imageHeight);
