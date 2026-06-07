#pragma once

#include "detector_settings.h"
#include "facedetections_tracking.h"

#include <QDateTime>
#include <QFileInfo>
#include <QJsonObject>
#include <QString>

struct RuntimeTuning {
  int stride = jcut::facedetections::kDefaultDetectorStride;
  int maxDetections = jcut::facedetections::kDefaultDetectorMaxDetections;
  int scrfdTargetSize = jcut::facedetections::kDefaultDetectorScrfdTargetSize;
  int maxFacesPerFrame = jcut::facedetections::kDefaultDetectorMaxFacesPerFrame;
  QString scrfdModelVariant =
      QString::fromLatin1(jcut::facedetections::kDefaultScrfdModelVariant);
  float threshold = jcut::facedetections::kDefaultDetectorThreshold;
  float nmsIouThreshold = jcut::facedetections::kDefaultDetectorNmsIouThreshold;
  float trackMatchIouThreshold =
      jcut::facedetections::kDefaultDetectorTrackMatchIouThreshold;
  float newTrackMinConfidence =
      jcut::facedetections::kDefaultDetectorNewTrackMinConfidence;
  bool primaryFaceOnly = jcut::facedetections::kDefaultDetectorPrimaryFaceOnly;
  bool smallFaceFallback =
      jcut::facedetections::kDefaultDetectorSmallFaceFallback;
  bool scrfdTiled = jcut::facedetections::kDefaultDetectorScrfdTiled;
  float roiX1 = jcut::facedetections::kDefaultDetectorRoiX1;
  float roiY1 = jcut::facedetections::kDefaultDetectorRoiY1;
  float roiX2 = jcut::facedetections::kDefaultDetectorRoiX2;
  float roiY2 = jcut::facedetections::kDefaultDetectorRoiY2;
  float minFaceAreaRatio =
      jcut::facedetections::kDefaultDetectorMinFaceAreaRatio;
  float maxFaceAreaRatio =
      jcut::facedetections::kDefaultDetectorMaxFaceAreaRatio;
  float minAspect = jcut::facedetections::kDefaultDetectorMinAspect;
  float maxAspect = jcut::facedetections::kDefaultDetectorMaxAspect;
};

struct PreviewDebugSettings {
  bool followLatest = true;
  bool showDetections = true;
  bool showTracks = true;
  bool showLabels = true;
  bool showConfirmedTracks = true;
  bool showTentativeTracks = true;
  bool showLostTracks = true;
  float playbackSpeed = 1.0f;
  float detectionLineThickness = 1.5f;
  float trackLineThickness = 2.5f;
  float overlayOpacity = 1.0f;
};

jcut::facedetections::ContinuityTrackingTuning
trackingTuningForRuntime(const RuntimeTuning &tuning);
QJsonObject runtimeTuningToJson(const RuntimeTuning &tuning,
                                const QString &detector, int scrfdTargetSize);
QJsonObject previewDebugSettingsToJson(const PreviewDebugSettings &settings);
void applyPreviewDebugSettingsObject(const QJsonObject &object,
                                     PreviewDebugSettings *settings);
bool saveRuntimeTuningFile(const QString &path, const RuntimeTuning &tuning,
                           const QString &detector, int scrfdTargetSize,
                           const PreviewDebugSettings *previewSettings,
                           QString *error);
bool applyRuntimeParamsFile(const QString &path, const QFileInfo &info,
                            RuntimeTuning *tuning,
                            PreviewDebugSettings *previewSettings,
                            QDateTime *lastAppliedMtime);
RuntimeTuning runtimeTuningFromDetectorSettings(
    const jcut::facedetections::DetectorRuntimeSettings &settings);
