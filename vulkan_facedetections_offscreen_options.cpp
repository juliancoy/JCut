#include "vulkan_facedetections_offscreen_options.h"

#include "clip_serialization.h"
#include "json_io_utils.h"

#include <QDir>
#include <QFileInfo>
#include <QJsonObject>

#include <algorithm>
#include <cstdlib>
#include <iostream>

QString backendIdForOptions(const Options &options) {
  if (options.materializedGenerateFacestream) {
    return QStringLiteral(
        "materialized_generate_facedetections_opencv_cascade_v1");
  }
  if (options.heuristicZeroCopyDetector) {
    return QStringLiteral("native_jcut_heuristic_zero_copy_v1");
  }
  if (options.scrfdDetector) {
    return QStringLiteral("scrfd_%1_ncnn_vulkan_zero_copy_v1")
        .arg(jcut::facedetections::normalizeScrfdModelVariantId(
            options.scrfdModelVariant));
  }
  return QStringLiteral("res10_ssd_ncnn_vulkan_zero_copy_v1");
}

QString detectorSettingsPathForVideo(const QString &videoPath) {
  const QFileInfo info(videoPath);
  const QString baseName = info.completeBaseName().isEmpty()
                               ? info.fileName()
                               : info.completeBaseName();
  return info.dir().absoluteFilePath(baseName +
                                     QStringLiteral("_detectorsettings.json"));
}

void usage(const char *argv0) {
  std::cerr
      << "Usage: " << argv0
      << " [video] [--out-dir DIR] [--max-frames N] [--stride N]"
      << " [--start-frame N]"
      << " [--threshold F] [--preview-frames N] [--preview-stride N]"
      << " [--full-video] [--max-detections N] [--max-faces-per-frame N]"
      << " [--primary-face-only] [--multi-face]"
      << " [--small-face-fallback] [--no-small-face-fallback]"
      << " [--nms-iou F] [--track-match-iou F] [--new-track-min-confidence F]"
      << " [--params-file PATH]" << " [--clip-json PATH] [--apply-clip-grading]"
      << " [--detector jcut-dnn|scrfd-ncnn-vulkan|jcut-heuristic-zero-copy]"
      << " [--res10-param PATH] [--res10-bin PATH] [--scrfd-param PATH] "
         "[--scrfd-bin PATH]"
      << " [--scrfd-model 500m|1g|2.5g|10g|34g]"
      << " [--scrfd-target-size N] [--scrfd-tiling] [--no-scrfd-tiling]"
      << " [--preview-window] [--no-preview-window] [--control-window] "
         "[--no-control-window] [--preview-files]"
      << " [--preflight]" << " [--materialized-generate-facedetections]"
      << " [--require-zero-copy]" << " [--require-hardware-vulkan-frame-path]"
      << " [--allow-cpu-upload-fallback]"
      << " [--detector-pipeline-slots N] (1-10)"
      << " [--detector-workers N] (1-10)"
      << " [--benchmark-pipeline-slots [CSV]] (default 1,2,4,8)"
      << " [--checkpoint-writer-queue N]"
      << " [--async-checkpoint-writer] [--sync-checkpoint-writer]"
      << " [--progress] [--no-progress]" << " [--verbose] [--log-interval N]"
      << " [--decode software|hardware|auto|hardware_zero_copy]\n";
}

bool parseArgs(int argc, char **argv, Options *options) {
  for (int i = 1; i < argc; ++i) {
    const std::string arg = argv[i];
    auto next = [&](const char *name) -> const char * {
      if (i + 1 >= argc) {
        std::cerr << "Missing value for " << name << "\n";
        return nullptr;
      }
      return argv[++i];
    };
    if (arg == "--help" || arg == "-h") {
      usage(argv[0]);
      return false;
    }
    if (arg == "--out-dir") {
      const char *v = next("--out-dir");
      if (!v)
        return false;
      options->outputDir = QString::fromLocal8Bit(v);
    } else if (arg == "--max-frames") {
      const char *v = next("--max-frames");
      if (!v)
        return false;
      options->maxFrames = std::max(0, std::atoi(v));
    } else if (arg == "--start-frame") {
      const char *v = next("--start-frame");
      if (!v)
        return false;
      options->startFrame = std::max(0, std::atoi(v));
    } else if (arg == "--stride") {
      const char *v = next("--stride");
      if (!v)
        return false;
      options->stride = std::max(1, std::atoi(v));
    } else if (arg == "--full-video") {
      options->maxFrames = 0;
    } else if (arg == "--max-detections") {
      const char *v = next("--max-detections");
      if (!v)
        return false;
      options->maxDetections = std::max(1, std::atoi(v));
    } else if (arg == "--max-faces-per-frame") {
      const char *v = next("--max-faces-per-frame");
      if (!v)
        return false;
      options->maxFacesPerFrame = std::max(0, std::atoi(v));
    } else if (arg == "--scrfd-target-size") {
      const char *v = next("--scrfd-target-size");
      if (!v)
        return false;
      options->scrfdTargetSize = std::max(320, std::atoi(v));
    } else if (arg == "--scrfd-model") {
      const char *v = next("--scrfd-model");
      if (!v)
        return false;
      options->scrfdModelVariant =
          jcut::facedetections::normalizeScrfdModelVariantId(
              QString::fromLocal8Bit(v));
    } else if (arg == "--scrfd-tiling") {
      options->scrfdTiled = true;
    } else if (arg == "--no-scrfd-tiling") {
      options->scrfdTiled = false;
    } else if (arg == "--primary-face-only") {
      options->primaryFaceOnly = true;
    } else if (arg == "--multi-face") {
      options->primaryFaceOnly = false;
    } else if (arg == "--small-face-fallback") {
      options->smallFaceFallback = true;
    } else if (arg == "--no-small-face-fallback") {
      options->smallFaceFallback = false;
    } else if (arg == "--nms-iou") {
      const char *v = next("--nms-iou");
      if (!v)
        return false;
      options->nmsIouThreshold =
          std::clamp(static_cast<float>(std::atof(v)), 0.0f, 1.0f);
    } else if (arg == "--track-match-iou") {
      const char *v = next("--track-match-iou");
      if (!v)
        return false;
      options->trackMatchIouThreshold =
          std::clamp(static_cast<float>(std::atof(v)), 0.0f, 1.0f);
    } else if (arg == "--new-track-min-confidence") {
      const char *v = next("--new-track-min-confidence");
      if (!v)
        return false;
      options->newTrackMinConfidence =
          std::clamp(static_cast<float>(std::atof(v)), 0.0f, 1.0f);
    } else if (arg == "--threshold") {
      const char *v = next("--threshold");
      if (!v)
        return false;
      options->threshold =
          std::clamp(static_cast<float>(std::atof(v)), 0.0f, 1.0f);
    } else if (arg == "--preview-frames") {
      const char *v = next("--preview-frames");
      if (!v)
        return false;
      options->previewFrames = std::max(0, std::atoi(v));
    } else if (arg == "--preview-stride") {
      const char *v = next("--preview-stride");
      if (!v)
        return false;
      options->previewStride = std::max(1, std::atoi(v));
    } else if (arg == "--detector") {
      const char *v = next("--detector");
      if (!v)
        return false;
      const QString detector = QString::fromLocal8Bit(v).trimmed().toLower();
      if (detector == QStringLiteral("jcut-heuristic-zero-copy") ||
          detector == QStringLiteral("vulkan-zero-copy-heuristic")) {
        options->heuristicZeroCopyDetector = true;
        options->scrfdDetector = false;
        options->detector = QStringLiteral("jcut-heuristic-zero-copy");
      } else if (detector == QStringLiteral("scrfd") ||
                 detector == QStringLiteral("scrfd-ncnn") ||
                 detector == QStringLiteral("scrfd-ncnn-vulkan")) {
        options->heuristicZeroCopyDetector = false;
        options->scrfdDetector = true;
        options->detector = QStringLiteral("scrfd-ncnn-vulkan");
      } else if (detector == QStringLiteral("jcut-dnn") ||
                 detector == QStringLiteral("jcut-dnn-vulkan") ||
                 detector == QStringLiteral("native-jcut-dnn") ||
                 detector == QStringLiteral("native_vulkan_dnn")) {
        options->heuristicZeroCopyDetector = false;
        options->scrfdDetector = true;
        options->detector = QStringLiteral("jcut-dnn");
      } else if (detector == QStringLiteral("res10-vulkan") ||
                 detector == QStringLiteral("res10-ncnn-vulkan") ||
                 detector == QStringLiteral("res10-ssd-ncnn-vulkan")) {
        options->heuristicZeroCopyDetector = false;
        options->scrfdDetector = false;
        options->detector = QStringLiteral("res10-vulkan");
      } else {
        std::cerr << "Unsupported --detector value: " << v
                  << ". Use jcut-dnn, scrfd-ncnn-vulkan, res10-vulkan, or "
                     "jcut-heuristic-zero-copy.\n";
        return false;
      }
    } else if (arg == "--res10-param") {
      const char *v = next("--res10-param");
      if (!v)
        return false;
      options->res10ParamPath = QString::fromLocal8Bit(v);
    } else if (arg == "--res10-bin") {
      const char *v = next("--res10-bin");
      if (!v)
        return false;
      options->res10BinPath = QString::fromLocal8Bit(v);
    } else if (arg == "--scrfd-param") {
      const char *v = next("--scrfd-param");
      if (!v)
        return false;
      options->scrfdParamPath = QString::fromLocal8Bit(v);
    } else if (arg == "--scrfd-bin") {
      const char *v = next("--scrfd-bin");
      if (!v)
        return false;
      options->scrfdBinPath = QString::fromLocal8Bit(v);
    } else if (arg == "--params-file") {
      const char *v = next("--params-file");
      if (!v)
        return false;
      options->paramsFile = QString::fromLocal8Bit(v);
    } else if (arg == "--clip-json") {
      const char *v = next("--clip-json");
      if (!v)
        return false;
      options->clipJsonPath = QString::fromLocal8Bit(v);
    } else if (arg == "--apply-clip-grading") {
      options->applyClipGrading = true;
    } else if (arg == "--preview-socket") {
      const char *v = next("--preview-socket");
      if (!v)
        return false;
      options->previewSocket = QString::fromLocal8Bit(v);
    } else if (arg == "--preview-window") {
      options->livePreview = true;
      options->controlWindow = true;
    } else if (arg == "--no-preview-window") {
      options->livePreview = false;
    } else if (arg == "--control-window") {
      options->controlWindow = true;
    } else if (arg == "--no-control-window") {
      options->controlWindow = false;
    } else if (arg == "--preflight") {
      options->preflight = true;
    } else if (arg == "--preview-files") {
      options->writePreviewFiles = true;
    } else if (arg == "--no-preview-files") {
      options->writePreviewFiles = false;
    } else if (arg == "--materialized-generate-facedetections") {
      options->materializedGenerateFacestream = true;
    } else if (arg == "--require-zero-copy") {
      options->requireZeroCopy = true;
    } else if (arg == "--require-hardware-vulkan-frame-path") {
      options->requireHardwareVulkanFramePath = true;
    } else if (arg == "--allow-cpu-upload-fallback") {
      options->allowCpuUploadFallback = true;
    } else if (arg == "--detector-pipeline-slots") {
      const char *v = next("--detector-pipeline-slots");
      if (!v)
        return false;
      options->detectorPipelineSlots = std::clamp(std::atoi(v), 1, 10);
    } else if (arg == "--detector-workers") {
      const char *v = next("--detector-workers");
      if (!v)
        return false;
      options->detectorWorkers = std::clamp(std::atoi(v), 1, 10);
    } else if (arg == "--benchmark-pipeline-slots") {
      options->benchmarkPipelineSlots = true;
      if (i + 1 < argc && argv[i + 1][0] != '-') {
        const QStringList parts =
            QString::fromLocal8Bit(argv[++i]).split(',', Qt::SkipEmptyParts);
        QVector<int> slotValues;
        slotValues.reserve(parts.size());
        for (const QString &part : parts) {
          bool ok = false;
          const int slotValue = part.trimmed().toInt(&ok);
          if (!ok) {
            std::cerr << "Invalid --benchmark-pipeline-slots value: "
                      << part.toStdString() << "\n";
            return false;
          }
          const int clamped = std::clamp(slotValue, 1, 10);
          if (!slotValues.contains(clamped)) {
            slotValues.push_back(clamped);
          }
        }
        if (slotValues.isEmpty()) {
          std::cerr << "--benchmark-pipeline-slots requires at least one slot "
                       "value.\n";
          return false;
        }
        options->benchmarkPipelineSlotValues = slotValues;
      }
    } else if (arg == "--checkpoint-writer-queue") {
      const char *v = next("--checkpoint-writer-queue");
      if (!v)
        return false;
      options->checkpointWriterQueueCapacity =
          std::clamp(std::atoi(v), 1, 4096);
    } else if (arg == "--async-checkpoint-writer") {
      options->asyncCheckpointWriter = true;
    } else if (arg == "--sync-checkpoint-writer") {
      options->asyncCheckpointWriter = false;
    } else if (arg == "--verbose") {
      options->verbose = true;
      options->progress = false;
    } else if (arg == "--progress") {
      options->progress = true;
    } else if (arg == "--no-progress") {
      options->progress = false;
    } else if (arg == "--quiet") {
      options->verbose = false;
      options->logInterval = 0;
    } else if (arg == "--log-interval") {
      const char *v = next("--log-interval");
      if (!v)
        return false;
      options->logInterval = std::max(0, std::atoi(v));
    } else if (arg == "--decode") {
      const char *v = next("--decode");
      if (!v)
        return false;
      editor::DecodePreference preference = editor::DecodePreference::Software;
      if (!editor::parseDecodePreference(QString::fromLocal8Bit(v),
                                         &preference)) {
        std::cerr << "Invalid --decode value: " << v << "\n";
        return false;
      }
      options->decodePreference = preference;
    } else if (!arg.empty() && arg[0] == '-') {
      std::cerr << "Unknown argument: " << arg << "\n";
      return false;
    } else {
      options->videoPath = QString::fromLocal8Bit(arg.c_str());
    }
  }
  if (options->requireZeroCopy) {
    options->requireHardwareVulkanFramePath = true;
    if (options->allowCpuUploadFallback) {
      std::cerr << "--require-zero-copy conflicts with "
                   "--allow-cpu-upload-fallback.\n";
      return false;
    }
    if (options->applyClipGrading) {
      std::cerr << "--require-zero-copy cannot be combined with "
                   "--apply-clip-grading.\n";
      return false;
    }
    if (options->materializedGenerateFacestream) {
      std::cerr << "--require-zero-copy cannot be combined with "
                   "--materialized-generate-facedetections.\n";
      return false;
    }
    if (!options->previewSocket.trimmed().isEmpty()) {
      std::cerr
          << "--require-zero-copy cannot be combined with --preview-socket "
             "because socket previews require QImage readback.\n";
      return false;
    }
    if (options->writePreviewFiles) {
      std::cerr << "--require-zero-copy cannot be combined with preview file "
                   "output because preview frames require QImage readback.\n";
      return false;
    }
  }
  return true;
}

bool loadClipFromJsonPath(const QString &path, TimelineClip *clipOut,
                          QString *errorOut) {
  if (clipOut) {
    *clipOut = TimelineClip{};
  }
  QJsonObject object;
  if (!jcut::jsonio::readJsonFile(path, &object, errorOut)) {
    return false;
  }
  if (clipOut) {
    *clipOut = editor::clipFromJson(object);
  }
  return true;
}
