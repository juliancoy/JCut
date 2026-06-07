#include "vulkan_facedetections_offscreen_tuning.h"

#include "json_io_utils.h"

#include <QFileInfo>

#include <algorithm>

jcut::facedetections::ContinuityTrackingTuning
trackingTuningForRuntime(const RuntimeTuning &tuning) {
  jcut::facedetections::ContinuityTrackingTuning trackingTuning;
  trackingTuning.trackMatchIouThreshold = tuning.trackMatchIouThreshold;
  trackingTuning.newTrackMinConfidence = tuning.newTrackMinConfidence;
  trackingTuning.primaryFaceOnly = tuning.primaryFaceOnly;
  trackingTuning.staleTrackFrameWindow = 48;
  return trackingTuning;
}

QJsonObject runtimeTuningToJson(const RuntimeTuning &tuning,
                                const QString &detector, int scrfdTargetSize) {
  return {{QStringLiteral("detector"), detector},
          {QStringLiteral("scrfd_model_variant"),
           jcut::facedetections::normalizeScrfdModelVariantId(
               tuning.scrfdModelVariant)},
          {QStringLiteral("scrfd_target_size"), tuning.scrfdTargetSize > 0
                                                    ? tuning.scrfdTargetSize
                                                    : scrfdTargetSize},
          {QStringLiteral("stride"), tuning.stride},
          {QStringLiteral("max_detections"), tuning.maxDetections},
          {QStringLiteral("max_faces_per_frame"), tuning.maxFacesPerFrame},
          {QStringLiteral("threshold"), tuning.threshold},
          {QStringLiteral("nms_iou_threshold"), tuning.nmsIouThreshold},
          {QStringLiteral("track_match_iou_threshold"),
           tuning.trackMatchIouThreshold},
          {QStringLiteral("new_track_min_confidence"),
           tuning.newTrackMinConfidence},
          {QStringLiteral("primary_face_only"), tuning.primaryFaceOnly},
          {QStringLiteral("small_face_fallback"), tuning.smallFaceFallback},
          {QStringLiteral("scrfd_tiled"), tuning.scrfdTiled},
          {QStringLiteral("roi_x1"), tuning.roiX1},
          {QStringLiteral("roi_y1"), tuning.roiY1},
          {QStringLiteral("roi_x2"), tuning.roiX2},
          {QStringLiteral("roi_y2"), tuning.roiY2},
          {QStringLiteral("min_face_area_ratio"), tuning.minFaceAreaRatio},
          {QStringLiteral("max_face_area_ratio"), tuning.maxFaceAreaRatio},
          {QStringLiteral("min_aspect"), tuning.minAspect},
          {QStringLiteral("max_aspect"), tuning.maxAspect}};
}

QJsonObject previewDebugSettingsToJson(const PreviewDebugSettings &settings) {
  return {
      {QStringLiteral("follow_latest"), settings.followLatest},
      {QStringLiteral("show_detections"), settings.showDetections},
      {QStringLiteral("show_tracks"), settings.showTracks},
      {QStringLiteral("show_labels"), settings.showLabels},
      {QStringLiteral("show_confirmed_tracks"), settings.showConfirmedTracks},
      {QStringLiteral("show_tentative_tracks"), settings.showTentativeTracks},
      {QStringLiteral("show_lost_tracks"), settings.showLostTracks},
      {QStringLiteral("playback_speed"), settings.playbackSpeed},
      {QStringLiteral("detection_line_thickness"),
       settings.detectionLineThickness},
      {QStringLiteral("track_line_thickness"), settings.trackLineThickness},
      {QStringLiteral("overlay_opacity"), settings.overlayOpacity}};
}

void applyPreviewDebugSettingsObject(const QJsonObject &object,
                                     PreviewDebugSettings *settings) {
  if (!settings) {
    return;
  }
  settings->followLatest = object.value(QStringLiteral("follow_latest"))
                               .toBool(settings->followLatest);
  settings->showDetections = object.value(QStringLiteral("show_detections"))
                                 .toBool(settings->showDetections);
  settings->showTracks =
      object.value(QStringLiteral("show_tracks")).toBool(settings->showTracks);
  settings->showLabels =
      object.value(QStringLiteral("show_labels")).toBool(settings->showLabels);
  settings->showConfirmedTracks =
      object.value(QStringLiteral("show_confirmed_tracks"))
          .toBool(settings->showConfirmedTracks);
  settings->showTentativeTracks =
      object.value(QStringLiteral("show_tentative_tracks"))
          .toBool(settings->showTentativeTracks);
  settings->showLostTracks = object.value(QStringLiteral("show_lost_tracks"))
                                 .toBool(settings->showLostTracks);
  settings->playbackSpeed = std::clamp(
      static_cast<float>(object.value(QStringLiteral("playback_speed"))
                             .toDouble(settings->playbackSpeed)),
      0.25f, 4.0f);
  settings->detectionLineThickness =
      std::clamp(static_cast<float>(
                     object.value(QStringLiteral("detection_line_thickness"))
                         .toDouble(settings->detectionLineThickness)),
                 1.0f, 4.0f);
  settings->trackLineThickness = std::clamp(
      static_cast<float>(object.value(QStringLiteral("track_line_thickness"))
                             .toDouble(settings->trackLineThickness)),
      1.0f, 5.0f);
  settings->overlayOpacity = std::clamp(
      static_cast<float>(object.value(QStringLiteral("overlay_opacity"))
                             .toDouble(settings->overlayOpacity)),
      0.2f, 1.0f);
}

bool saveRuntimeTuningFile(const QString &path, const RuntimeTuning &tuning,
                           const QString &detector, int scrfdTargetSize,
                           const PreviewDebugSettings *previewSettings,
                           QString *errorMessage = nullptr) {
  if (path.isEmpty()) {
    return false;
  }
  QJsonObject root;
  jcut::jsonio::readJsonFile(path, &root, nullptr);
  root = runtimeTuningToJson(tuning, detector,
                             tuning.scrfdTargetSize > 0 ? tuning.scrfdTargetSize
                                                        : scrfdTargetSize);
  if (previewSettings) {
    root.insert(QStringLiteral("preview_debug"),
                previewDebugSettingsToJson(*previewSettings));
  }
  if (!jcut::jsonio::writeJsonFile(path, root, true, errorMessage)) {
    if (errorMessage && errorMessage->isEmpty()) {
      *errorMessage =
          QStringLiteral("failed to write detector settings: %1").arg(path);
    }
    return false;
  }
  return true;
}

bool applyRuntimeParamsFile(const QString &path, const QFileInfo &info,
                            RuntimeTuning *tuning,
                            PreviewDebugSettings *previewSettings,
                            QDateTime *lastAppliedMtime) {
  if (!tuning || !lastAppliedMtime || path.isEmpty()) {
    return false;
  }
  if (!info.exists()) {
    return false;
  }
  const QDateTime mtime = info.lastModified();
  if (lastAppliedMtime->isValid() && mtime <= *lastAppliedMtime) {
    return false;
  }

  QJsonObject o;
  if (!jcut::jsonio::readJsonFile(path, &o)) {
    return false;
  }
  if (o.contains(QStringLiteral("scrfd_model_variant"))) {
    tuning->scrfdModelVariant =
        jcut::facedetections::normalizeScrfdModelVariantId(
            o.value(QStringLiteral("scrfd_model_variant"))
                .toString(tuning->scrfdModelVariant));
  } else {
    tuning->scrfdModelVariant =
        jcut::facedetections::normalizeScrfdModelVariantId(
            tuning->scrfdModelVariant);
  }
  if (o.contains(QStringLiteral("stride"))) {
    tuning->stride =
        qMax(1, o.value(QStringLiteral("stride")).toInt(tuning->stride));
  }
  if (o.contains(QStringLiteral("max_detections"))) {
    tuning->maxDetections = qMax(
        1,
        o.value(QStringLiteral("max_detections")).toInt(tuning->maxDetections));
  }
  if (o.contains(QStringLiteral("scrfd_target_size"))) {
    tuning->scrfdTargetSize =
        qMax(320, o.value(QStringLiteral("scrfd_target_size"))
                      .toInt(tuning->scrfdTargetSize));
  }
  if (o.contains(QStringLiteral("max_faces_per_frame"))) {
    tuning->maxFacesPerFrame =
        qMax(0, o.value(QStringLiteral("max_faces_per_frame"))
                    .toInt(tuning->maxFacesPerFrame));
  }
  if (o.contains(QStringLiteral("threshold"))) {
    tuning->threshold = std::clamp(
        static_cast<float>(
            o.value(QStringLiteral("threshold")).toDouble(tuning->threshold)),
        0.0f, 1.0f);
  }
  if (o.contains(QStringLiteral("nms_iou_threshold"))) {
    tuning->nmsIouThreshold = std::clamp(
        static_cast<float>(o.value(QStringLiteral("nms_iou_threshold"))
                               .toDouble(tuning->nmsIouThreshold)),
        0.0f, 1.0f);
  }
  if (o.contains(QStringLiteral("track_match_iou_threshold"))) {
    tuning->trackMatchIouThreshold = std::clamp(
        static_cast<float>(o.value(QStringLiteral("track_match_iou_threshold"))
                               .toDouble(tuning->trackMatchIouThreshold)),
        0.0f, 1.0f);
  }
  if (o.contains(QStringLiteral("new_track_min_confidence"))) {
    tuning->newTrackMinConfidence = std::clamp(
        static_cast<float>(o.value(QStringLiteral("new_track_min_confidence"))
                               .toDouble(tuning->newTrackMinConfidence)),
        0.0f, 1.0f);
  }
  if (o.contains(QStringLiteral("primary_face_only"))) {
    tuning->primaryFaceOnly = o.value(QStringLiteral("primary_face_only"))
                                  .toBool(tuning->primaryFaceOnly);
  }
  if (o.contains(QStringLiteral("small_face_fallback"))) {
    tuning->smallFaceFallback = o.value(QStringLiteral("small_face_fallback"))
                                    .toBool(tuning->smallFaceFallback);
  }
  if (o.contains(QStringLiteral("scrfd_tiled"))) {
    tuning->scrfdTiled =
        o.value(QStringLiteral("scrfd_tiled")).toBool(tuning->scrfdTiled);
  }
  if (o.contains(QStringLiteral("roi_x1")))
    tuning->roiX1 = std::clamp(
        static_cast<float>(
            o.value(QStringLiteral("roi_x1")).toDouble(tuning->roiX1)),
        0.0f, 1.0f);
  if (o.contains(QStringLiteral("roi_y1")))
    tuning->roiY1 = std::clamp(
        static_cast<float>(
            o.value(QStringLiteral("roi_y1")).toDouble(tuning->roiY1)),
        0.0f, 1.0f);
  if (o.contains(QStringLiteral("roi_x2")))
    tuning->roiX2 = std::clamp(
        static_cast<float>(
            o.value(QStringLiteral("roi_x2")).toDouble(tuning->roiX2)),
        0.0f, 1.0f);
  if (o.contains(QStringLiteral("roi_y2")))
    tuning->roiY2 = std::clamp(
        static_cast<float>(
            o.value(QStringLiteral("roi_y2")).toDouble(tuning->roiY2)),
        0.0f, 1.0f);
  if (tuning->roiX2 < tuning->roiX1)
    std::swap(tuning->roiX1, tuning->roiX2);
  if (tuning->roiY2 < tuning->roiY1)
    std::swap(tuning->roiY1, tuning->roiY2);
  if (o.contains(QStringLiteral("min_face_area_ratio")))
    tuning->minFaceAreaRatio = std::clamp(
        static_cast<float>(o.value(QStringLiteral("min_face_area_ratio"))
                               .toDouble(tuning->minFaceAreaRatio)),
        0.0f, 1.0f);
  if (o.contains(QStringLiteral("max_face_area_ratio")))
    tuning->maxFaceAreaRatio = std::clamp(
        static_cast<float>(o.value(QStringLiteral("max_face_area_ratio"))
                               .toDouble(tuning->maxFaceAreaRatio)),
        0.0f, 1.0f);
  if (tuning->maxFaceAreaRatio < tuning->minFaceAreaRatio)
    std::swap(tuning->minFaceAreaRatio, tuning->maxFaceAreaRatio);
  if (o.contains(QStringLiteral("min_aspect")))
    tuning->minAspect = std::clamp(
        static_cast<float>(
            o.value(QStringLiteral("min_aspect")).toDouble(tuning->minAspect)),
        0.0f, 100.0f);
  if (o.contains(QStringLiteral("max_aspect")))
    tuning->maxAspect = std::clamp(
        static_cast<float>(
            o.value(QStringLiteral("max_aspect")).toDouble(tuning->maxAspect)),
        0.0f, 100.0f);
  if (tuning->maxAspect < tuning->minAspect)
    std::swap(tuning->minAspect, tuning->maxAspect);
  if (previewSettings) {
    applyPreviewDebugSettingsObject(
        o.value(QStringLiteral("preview_debug")).toObject(), previewSettings);
  }
  *lastAppliedMtime = mtime;
  return true;
}

RuntimeTuning runtimeTuningFromDetectorSettings(
    const jcut::facedetections::DetectorRuntimeSettings &settings) {
  RuntimeTuning tuning;
  tuning.stride = settings.stride;
  tuning.maxDetections = settings.maxDetections;
  tuning.scrfdTargetSize = settings.scrfdTargetSize;
  tuning.maxFacesPerFrame = settings.maxFacesPerFrame;
  tuning.scrfdModelVariant = jcut::facedetections::normalizeScrfdModelVariantId(
      settings.scrfdModelVariant);
  tuning.threshold = settings.threshold;
  tuning.nmsIouThreshold = settings.nmsIouThreshold;
  tuning.trackMatchIouThreshold = settings.trackMatchIouThreshold;
  tuning.newTrackMinConfidence = settings.newTrackMinConfidence;
  tuning.primaryFaceOnly = settings.primaryFaceOnly;
  tuning.smallFaceFallback = settings.smallFaceFallback;
  tuning.scrfdTiled = settings.scrfdTiled;
  tuning.roiX1 = settings.roiX1;
  tuning.roiY1 = settings.roiY1;
  tuning.roiX2 = settings.roiX2;
  tuning.roiY2 = settings.roiY2;
  tuning.minFaceAreaRatio = settings.minFaceAreaRatio;
  tuning.maxFaceAreaRatio = settings.maxFaceAreaRatio;
  tuning.minAspect = settings.minAspect;
  tuning.maxAspect = settings.maxAspect;
  return tuning;
}
