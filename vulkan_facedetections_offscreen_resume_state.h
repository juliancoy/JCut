#pragma once

#include "vulkan_facedetections_offscreen_detection_filters.h"

#include <QHash>
#include <QJsonArray>
#include <QSet>
#include <QString>
#include <QVector>

struct FaceDetectionsResumeState {
  QSet<int> completedFrames;
  QJsonArray frameRows;
  QJsonArray rawDetectionFrames;
  QHash<int, int> rawDetectionFrameIndexByFrame;
  QHash<int, QJsonArray> trackDetectionsByFrame;
  QVector<Track> runtimeTracks;
  bool loadedFromCompactIndex = false;
  bool fullPayloadDeferred = false;
  int processed = 0;
  int totalDetections = 0;
  int appVulkanFramePathFrames = 0;
  int decoderDirectHandoffFrames = 0;
  int decoderVulkanUploadFallbackFrames = 0;
  int hardwareDirectHandoffFrames = 0;
  int hardwareInteropProbeSupportedFrames = 0;
  int hardwareInteropProbeFailedFrames = 0;
  int hardwareFrames = 0;
  int cpuFrames = 0;
  double renderDecodeMsTotal = 0.0;
  double renderCompositeMsTotal = 0.0;
  double renderReadbackMsTotal = 0.0;
  double decoderUploadMsTotal = 0.0;
  double vulkanDetectMsTotal = 0.0;
  double ncnnInputMsTotal = 0.0;
  double ncnnExtractMsTotal = 0.0;
  double ncnnExtractLevel8MsTotal = 0.0;
  double ncnnExtractLevel16MsTotal = 0.0;
  double ncnnExtractLevel32MsTotal = 0.0;
  double ncnnPostMsTotal = 0.0;
  double ncnnTotalMsTotal = 0.0;
};

QString faceDetectionsResumeIndexPath(const QString &faceStreamPath);
bool saveFaceDetectionsResumeIndex(
    const QString &indexPath, const QString &faceStreamPath,
    const QString &videoPath, const QString &backend, int startFrame,
    int endFrame, const FaceDetectionsResumeState &state,
    const QVector<Track> &runtimeTracks, QString *error);
bool loadFaceDetectionsResumeIndex(
    const QString &indexPath, const QString &faceStreamPath,
    const QString &videoPath, const QString &backend, int startFrame,
    int endFrame, FaceDetectionsResumeState *state, QString *error);
bool loadFaceDetectionsResume(const QString &path, const QString &videoPath,
                              const QString &backend, int startFrame,
                              int endFrame, FaceDetectionsResumeState *state,
                              QString *error);
bool repairFaceDetectionsResumeCheckpoint(const QString &path,
                                          const QString &videoPath,
                                          const QString &backend,
                                          int startFrame,
                                          int endFrame,
                                          FaceDetectionsResumeState *state,
                                          QString *message);
