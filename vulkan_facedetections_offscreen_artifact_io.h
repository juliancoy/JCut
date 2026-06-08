#pragma once

#include "facedetections_tracking.h"

#include <QJsonObject>
#include <QSize>
#include <QVector>

class QFile;
class QString;

struct FaceStreamMetaRecord {
  QString video;
  QString backend;
  int startFrame = 0;
  int endFrame = 0;
  int stride = 1;
  qint64 createdUtcMs = 0;
};

struct FaceStreamFrameRecord {
  QString video;
  QString backend;
  QString detector;
  int frame = -1;
  QSize frameSize;
  QVector<jcut::facedetections::Detection> detections;
  QVector<jcut::facedetections::FrameTrackDetection> trackDetections;
  bool appVulkanFramePath = false;
  bool decoderDirectHandoff = false;
  bool decoderVulkanUploadFallback = false;
  bool hardwareDirectHandoff = false;
  QString hardwareDirectAttemptReason;
  bool hardwareInteropProbeSupported = false;
  bool hardwareInteropProbeFailed = false;
  QString hardwareInteropProbePath;
  QString hardwareInteropProbeReason;
  bool hardwareFrame = false;
  bool cpuFrame = false;
  bool qimageMaterialized = false;
  double appRenderDecodeMs = 0.0;
  double appRenderTextureMs = 0.0;
  double appRenderCompositeMs = 0.0;
  double appRenderReadbackMs = 0.0;
  double vulkanZeroCopyDetectionMs = 0.0;
  double decoderVulkanUploadMs = 0.0;
  double ncnnInputMs = 0.0;
  double ncnnExtractMs = 0.0;
  double ncnnExtractLevel8Ms = 0.0;
  double ncnnExtractLevel16Ms = 0.0;
  double ncnnExtractLevel32Ms = 0.0;
  double ncnnPostMs = 0.0;
  double ncnnTotalMs = 0.0;
};

bool writeJson(const QString &path, const QJsonObject &object);
bool writeBinaryJsonObject(const QString &path, const QJsonObject &object);
bool appendFaceStreamMetaRecord(QFile *file, const FaceStreamMetaRecord &record);
bool appendFaceStreamFrameRecord(QFile *file, const FaceStreamFrameRecord &record);
bool readFaceStreamRecord(QFile *file, FaceStreamMetaRecord *metaOut,
                          FaceStreamFrameRecord *frameOut, QString *error);
