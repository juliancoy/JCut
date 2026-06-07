#pragma once

#include "debug_controls.h"
#include "detector_settings.h"
#include "editor_shared.h"
#include "timeline_fps.h"

#include <QJsonObject>
#include <QString>
#include <QVector>

#include <unistd.h>

struct TimelineClip;

struct Options {
  QString videoPath =
      QStringLiteral("/mnt/Cancer/PanelVid2TikTok/Politics/"
                     "YTDown.com_YouTube_Meet-the-Candidates-for-Baltimore-"
                     "County_Media_Hho5MORgIj8_001_1080p.mp4");
  QString outputDir =
      QStringLiteral("testbench_assets/vulkan_facedetections_offscreen");
  int maxFrames = 0; // 0 => full video
  int startFrame = 0;
  int stride = jcut::facedetections::kDefaultDetectorStride;
  int maxDetections = jcut::facedetections::kDefaultDetectorMaxDetections;
  int maxFacesPerFrame =
      jcut::facedetections::kDefaultDetectorMaxFacesPerFrame; // 0 => no
                                                              // post-cap
  int scrfdTargetSize = jcut::facedetections::kDefaultDetectorScrfdTargetSize;
  QString scrfdModelVariant =
      QString::fromLatin1(jcut::facedetections::kDefaultScrfdModelVariant);
  int previewFrames = 24;
  int previewStride = 12;
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
  QString detector = QStringLiteral("jcut-dnn");
  QString res10ParamPath;
  QString res10BinPath;
  QString scrfdParamPath;
  QString scrfdBinPath;
  QString paramsFile;
  QString clipJsonPath;
  QString previewSocket;
  bool livePreview = false;
  bool controlWindow = false;
  bool writePreviewFiles = false;
  bool applyClipGrading = false;
  bool materializedGenerateFacestream = false;
  bool heuristicZeroCopyDetector = false;
  bool scrfdDetector = true;
  bool requireHardwareVulkanFramePath = false;
  bool requireZeroCopy = false;
  bool allowCpuUploadFallback = false;
  bool verbose = false;
  bool progress = isatty(STDOUT_FILENO);
  int logInterval = 0; // 0 => summary only unless --verbose is set
  bool preflight = false;
  int detectorPipelineSlots = 4;
  int detectorWorkers = 1;
  bool asyncCheckpointWriter = true;
  int checkpointWriterQueueCapacity = 256;
  bool benchmarkPipelineSlots = false;
  QVector<int> benchmarkPipelineSlotValues{1, 4, 8};
  editor::DecodePreference decodePreference =
      editor::DecodePreference::HardwareZeroCopy;
};

QString backendIdForOptions(const Options &options);
QString detectorSettingsPathForVideo(const QString &videoPath);
void usage(const char *argv0);
bool parseArgs(int argc, char **argv, Options *options);
bool loadClipFromJsonPath(const QString &path, TimelineClip *clipOut,
                          QString *errorOut = nullptr);
