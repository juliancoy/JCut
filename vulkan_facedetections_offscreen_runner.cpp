#include "vulkan_facedetections_offscreen_runner.h"
#include "clip_serialization.h"
#include "debug_controls.h"
#include "decoder_context.h"
#include "decoder_ffmpeg_utils.h"
#include "detector_settings.h"
#include "editor_shared.h"
#include "facedetections_generation.h"
#include "facedetections_runtime.h"
#include "facedetections_tracking.h"
#include "frame_handle.h"
#include "imgui_preview_window.h"
#include "json_io_utils.h"
#include "timeline_fps.h"
#include "vulkan_detector_frame_handoff.h"
#include "vulkan_facedetections_offscreen_artifact_io.h"
#include "vulkan_facedetections_offscreen_benchmark.h"
#include "vulkan_facedetections_offscreen_checkpoint_writer.h"
#include "vulkan_facedetections_offscreen_control_panel.h"
#include "vulkan_facedetections_offscreen_detection_filters.h"
#include "vulkan_facedetections_offscreen_opencv_dnn.h"
#include "vulkan_facedetections_offscreen_options.h"
#include "vulkan_facedetections_offscreen_preview_io.h"
#include "vulkan_facedetections_offscreen_progress.h"
#include "vulkan_facedetections_offscreen_resume_state.h"
#include "vulkan_facedetections_offscreen_tuning.h"
#include "vulkan_facedetections_offscreen_vulkan_harness.h"
#include "vulkan_res10_ncnn_face_detector.h"
#include "vulkan_scrfd_ncnn_face_detector.h"
#include "vulkan_zero_copy_face_detector.h"

#include <QApplication>
#include <QCheckBox>
#include <QComboBox>
#include <QDataStream>
#include <QDebug>
#include <QDir>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QFile>
#include <QFileInfo>
#include <QFormLayout>
#include <QFrame>
#include <QHBoxLayout>
#include <QHash>
#include <QImage>
#include <QJsonArray>
#include <QJsonObject>
#include <QLabel>
#include <QLocalSocket>
#include <QPainter>
#include <QProcess>
#include <QProgressBar>
#include <QPushButton>
#include <QScrollArea>
#include <QSet>
#include <QSlider>
#include <QString>
#include <QStringList>
#include <QVBoxLayout>
#include <QWidget>
#include <QtGlobal>

#include <vulkan/vulkan.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <fcntl.h>
#include <functional>
#include <future>
#include <iomanip>
#include <iostream>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <unistd.h>
#include <vector>

#if JCUT_HAVE_OPENCV
#include <opencv2/dnn.hpp>
#include <opencv2/imgproc.hpp>
#include <opencv2/objdetect.hpp>
#endif

int runVulkanFacestreamOffscreenWithArgv(int argc, char **argv) {
  QApplication *appPtr =
      qobject_cast<QApplication *>(QCoreApplication::instance());
  std::unique_ptr<QApplication> ownedApp;
  if (!appPtr) {
    ownedApp = std::make_unique<QApplication>(argc, argv);
    appPtr = ownedApp.get();
  }
  if (!appPtr) {
    std::cerr
        << "Failed to initialize QApplication for FaceDetections generator.\n";
    return 2;
  }
  editor::installFfmpegLogFilter();
  Options options;
  if (!parseArgs(argc, argv, &options)) {
    return 2;
  }
  if (!QFileInfo::exists(options.videoPath)) {
    std::cerr << "Video not found: " << options.videoPath.toStdString() << "\n";
    return 2;
  }
  if (options.benchmarkPipelineSlots) {
    return runPipelineSlotBenchmark(argc, argv, options);
  }
  if (options.preflight) {
    const bool platformIsOffscreen =
        QString::fromLocal8Bit(qgetenv("QT_QPA_PLATFORM"))
            .compare(QStringLiteral("offscreen"), Qt::CaseInsensitive) == 0;
    if (platformIsOffscreen) {
      std::cerr << "--preflight requires a GUI-capable Qt platform.\n";
      return 2;
    }
    jcut::facedetections::DetectorRuntimeSettings detectorSettings;
    detectorSettings.stride = options.stride;
    detectorSettings.maxDetections = options.maxDetections;
    detectorSettings.scrfdTargetSize = options.scrfdTargetSize;
    detectorSettings.maxFacesPerFrame = options.maxFacesPerFrame;
    detectorSettings.scrfdModelVariant =
        jcut::facedetections::normalizeScrfdModelVariantId(
            options.scrfdModelVariant);
    detectorSettings.threshold = options.threshold;
    detectorSettings.nmsIouThreshold = options.nmsIouThreshold;
    detectorSettings.trackMatchIouThreshold = options.trackMatchIouThreshold;
    detectorSettings.newTrackMinConfidence = options.newTrackMinConfidence;
    detectorSettings.primaryFaceOnly = options.primaryFaceOnly;
    detectorSettings.smallFaceFallback = options.smallFaceFallback;
    detectorSettings.scrfdTiled = options.scrfdTiled;

    const QString detectorSettingsPath =
        jcut::facedetections::detectorSettingsPathForVideo(options.videoPath);
    jcut::facedetections::loadDetectorRuntimeSettingsFile(
        detectorSettingsPath, &detectorSettings, nullptr);
    const jcut::facedetections::FaceDetectionsPreflightDialogResult preflight =
        jcut::facedetections::runFaceDetectionsPreflightDialog(
            &detectorSettings, options.detector, options.scrfdTargetSize,
            detectorSettingsPath,
            jcut::facedetections::FaceDetectionsPreflightDialogOptions{
                QStringLiteral("JCut DNN FaceDetections Generator"),
                QStringLiteral(
                    "This flow runs the standalone FaceDetections detector "
                    "directly against the selected media.\n\n"
                    "Detector: Vulkan-native face detection pipeline. Track "
                    "processing runs later."),
                QStringLiteral(
                    "The saved detector settings file is reused by both the "
                    "editor and the standalone runner for this source video."),
                QStringLiteral("Run"),
                QStringLiteral("Cancel"),
                QSize(760, 420),
                true,
                options.livePreview,
                false,
                !options.clipJsonPath.trimmed().isEmpty(),
                options.applyClipGrading,
                QStringLiteral("Apply clip grading during detection"),
                false,
                false,
                QStringLiteral(
                    "Restart from scratch (delete facedetections.part)"),
                false,
                false,
                QStringLiteral("Use proxy media as FaceDetections input"),
                true,
                options.detectorWorkers,
                1,
                10,
                QStringLiteral("Detector workers")});
    if (!preflight.accepted) {
      return 0;
    }
    if (!preflight.saveError.trimmed().isEmpty()) {
      std::cerr << preflight.saveError.toStdString() << "\n";
      return 2;
    }
    options.livePreview = preflight.livePreview;
    options.applyClipGrading = preflight.applyClipGrading;
    options.detectorWorkers = std::clamp(preflight.detectorWorkers, 1, 10);
    options.detectorPipelineSlots = std::clamp(
        qMax(options.detectorPipelineSlots, options.detectorWorkers), 1, 10);
    options.stride = detectorSettings.stride;
    options.maxDetections = detectorSettings.maxDetections;
    options.scrfdTargetSize = detectorSettings.scrfdTargetSize;
    options.maxFacesPerFrame = detectorSettings.maxFacesPerFrame;
    options.scrfdModelVariant =
        jcut::facedetections::normalizeScrfdModelVariantId(
            detectorSettings.scrfdModelVariant);
    options.threshold = detectorSettings.threshold;
    options.nmsIouThreshold = detectorSettings.nmsIouThreshold;
    options.trackMatchIouThreshold = detectorSettings.trackMatchIouThreshold;
    options.newTrackMinConfidence = detectorSettings.newTrackMinConfidence;
    options.primaryFaceOnly = detectorSettings.primaryFaceOnly;
    options.smallFaceFallback = detectorSettings.smallFaceFallback;
    options.scrfdTiled = detectorSettings.scrfdTiled;
  }
  auto applyScrfdModelVariantFromSettingsFile = [&](const QString &path) {
    if (path.trimmed().isEmpty()) {
      return;
    }
    QJsonObject object;
    if (!jcut::jsonio::readJsonFile(path, &object, nullptr)) {
      return;
    }
    if (object.contains(QStringLiteral("scrfd_model_variant"))) {
      options.scrfdModelVariant =
          jcut::facedetections::normalizeScrfdModelVariantId(
              object.value(QStringLiteral("scrfd_model_variant"))
                  .toString(options.scrfdModelVariant));
    }
  };
  if (!options.paramsFile.trimmed().isEmpty()) {
    applyScrfdModelVariantFromSettingsFile(options.paramsFile);
  }
  applyScrfdModelVariantFromSettingsFile(
      jcut::facedetections::detectorSettingsPathForVideo(options.videoPath));
  qputenv("JCUT_ALLOW_HEADLESS_HARDWARE_DECODE", "1");
  QDir().mkpath(options.outputDir);
  const bool platformIsOffscreen =
      QString::fromLocal8Bit(qgetenv("QT_QPA_PLATFORM"))
          .compare(QStringLiteral("offscreen"), Qt::CaseInsensitive) == 0;
  std::unique_ptr<ImGuiPreviewWindow> livePreviewWindow;
  const bool showControlWindow = options.controlWindow && !platformIsOffscreen;

  editor::setDebugDecodePreference(options.decodePreference);
  editor::DecoderContext decoder(options.videoPath);
  if (!decoder.initialize()) {
    std::cerr << "Failed to initialize decoder: "
              << options.videoPath.toStdString() << "\n";
    return 2;
  }
  TimelineClip sourceClip;
  sourceClip.id = QStringLiteral("facedetections-offscreen-source");
  sourceClip.filePath = options.videoPath;
  sourceClip.mediaType = ClipMediaType::Video;
  sourceClip.videoEnabled = true;
  sourceClip.audioEnabled = false;
  sourceClip.hasAudio = false;
  sourceClip.startFrame = options.startFrame;
  sourceClip.sourceInFrame = options.startFrame;
  const int64_t decoderDurationFrames =
      qMax<int64_t>(0, decoder.info().durationFrames);
  const int64_t availableFrames =
      qMax<int64_t>(1, decoderDurationFrames - options.startFrame);
  const int64_t targetFrames =
      options.maxFrames > 0
          ? qMin<int64_t>(static_cast<int64_t>(options.maxFrames),
                          availableFrames)
          : availableFrames;
  sourceClip.durationFrames = qMax<int64_t>(1, targetFrames);
  sourceClip.sourceDurationFrames = qMax<int64_t>(1, targetFrames);
  sourceClip.sourceFps = decoder.info().fps > 0.001
                             ? static_cast<qreal>(decoder.info().fps)
                             : static_cast<qreal>(kTimelineFps);
  sourceClip.playbackRate = 1.0;
  if (options.applyClipGrading && options.clipJsonPath.trimmed().isEmpty()) {
    std::cerr << "--apply-clip-grading requires --clip-json.\n";
    return 2;
  }
  const TimelineClip baseSourceClip = sourceClip;
  TimelineClip gradedSourceClip = sourceClip;
  if (!options.clipJsonPath.trimmed().isEmpty()) {
    TimelineClip gradedClip;
    QString clipLoadError;
    if (!loadClipFromJsonPath(options.clipJsonPath, &gradedClip,
                              &clipLoadError)) {
      std::cerr << clipLoadError.toStdString() << "\n";
      return 2;
    }
    gradedSourceClip.brightness = gradedClip.brightness;
    gradedSourceClip.contrast = gradedClip.contrast;
    gradedSourceClip.saturation = gradedClip.saturation;
    gradedSourceClip.opacity = gradedClip.opacity;
    gradedSourceClip.gradingKeyframes = gradedClip.gradingKeyframes;
    gradedSourceClip.opacityKeyframes = gradedClip.opacityKeyframes;
  }
  auto syncSourceClipGrading = [&]() {
    const TimelineClip &effectiveClip =
        options.applyClipGrading ? gradedSourceClip : baseSourceClip;
    sourceClip.brightness = effectiveClip.brightness;
    sourceClip.contrast = effectiveClip.contrast;
    sourceClip.saturation = effectiveClip.saturation;
    sourceClip.opacity = effectiveClip.opacity;
    sourceClip.gradingKeyframes = effectiveClip.gradingKeyframes;
    sourceClip.opacityKeyframes = effectiveClip.opacityKeyframes;
  };
  syncSourceClipGrading();
  jcut::facedetections::VulkanFrameProvider appFrameProvider;
  const QSize renderSize = decoder.info().frameSize.isValid()
                               ? decoder.info().frameSize
                               : QSize(1920, 1080);
  sourceClip.sourceFrameSize = renderSize;
  if (options.livePreview && !platformIsOffscreen) {
    livePreviewWindow = std::make_unique<ImGuiPreviewWindow>();
    if (!livePreviewWindow->initialize(
            QStringLiteral("JCut DNN FaceDetections Generator Preview"),
            QSize(1080, 720))) {
      std::cerr << "Failed to initialize Dear ImGui preview window: "
                << livePreviewWindow->failureReason().toStdString() << "\n";
      return 2;
    }
    livePreviewWindow->setStatusText(
        QStringLiteral("Waiting for JCut DNN FaceDetections preview..."));
  }
  QLocalSocket previewSocket;
  QLocalSocket *previewSocketPtr = nullptr;
  if (!options.previewSocket.isEmpty()) {
    previewSocket.connectToServer(options.previewSocket);
    if (previewSocket.waitForConnected(3000)) {
      previewSocketPtr = &previewSocket;
    } else {
      std::cerr << "Failed to connect preview socket: "
                << options.previewSocket.toStdString() << "\n";
    }
  }
  const bool preferDecoderDirectDetection =
      !options.materializedGenerateFacestream && !options.applyClipGrading;
  if (preferDecoderDirectDetection) {
    if (previewSocketPtr) {
      std::cerr << "Preview socket output requires QImage readback and is "
                   "disabled for decoder-direct zero-copy detection.\n";
      previewSocket.disconnectFromServer();
      previewSocket.close();
      previewSocketPtr = nullptr;
    }
    if (options.writePreviewFiles) {
      std::cerr << "Preview file output requires QImage readback and is "
                   "disabled for decoder-direct zero-copy detection.\n";
      options.writePreviewFiles = false;
    }
  }
  if (options.requireZeroCopy && !preferDecoderDirectDetection) {
    std::cerr
        << "--require-zero-copy requires the decoder-direct ungraded path. "
           "Disable clip grading and materialized compatibility mode.\n";
    return 2;
  }
  const bool blockingPreviewRequested =
      previewSocketPtr || options.writePreviewFiles;
  const bool previewRequested = blockingPreviewRequested;
  bool previewRequiresSynchronizedDetection = previewSocketPtr != nullptr;
  const int effectivePreviewStride = qMax(1, options.previewStride);
  bool previewPipelinePrimed = !previewRequested;
  QString lastPreviewStatusText;
  auto setPreviewStatusText = [&](const QString &text) {
    if (!livePreviewWindow || text == lastPreviewStatusText) {
      return;
    }
    livePreviewWindow->setStatusText(text);
    lastPreviewStatusText = text;
  };
  bool previewFailureLogged = false;
  auto disableSynchronizedPreview = [&](const QString &reason) {
    if (!previewFailureLogged) {
      std::cerr << "Preview synchronization disabled";
      if (!reason.trimmed().isEmpty()) {
        std::cerr << ": " << reason.toStdString();
      }
      std::cerr << "\n";
      previewFailureLogged = true;
    }
    previewRequiresSynchronizedDetection = false;
    previewPipelinePrimed = true;
    livePreviewWindow.reset();
  };

  QString error;
#if JCUT_HAVE_OPENCV
  cv::CascadeClassifier faceCascade;
  cv::CascadeClassifier faceCascadeAlt;
  cv::CascadeClassifier faceCascadeProfile;
  if (options.materializedGenerateFacestream) {
    const QString cascadePath = jcut::facedetections::findCascadeFile(
        QStringLiteral("haarcascade_frontalface_default.xml"));
    if (cascadePath.isEmpty() || !faceCascade.load(cascadePath.toStdString())) {
      std::cerr << "Failed to load Haar cascade for materialized Generate "
                   "FaceDetections scan.\n";
      return 2;
    }
    const QString altCascadePath = jcut::facedetections::findCascadeFile(
        QStringLiteral("haarcascade_frontalface_alt2.xml"));
    if (!altCascadePath.isEmpty()) {
      faceCascadeAlt.load(altCascadePath.toStdString());
    }
    const QString profileCascadePath = jcut::facedetections::findCascadeFile(
        QStringLiteral("haarcascade_profileface.xml"));
    if (!profileCascadePath.isEmpty()) {
      faceCascadeProfile.load(profileCascadePath.toStdString());
    }
  }
#else
  if (options.materializedGenerateFacestream) {
    std::cerr << "OpenCV is not enabled in this build; materialized Generate "
                 "FaceDetections scan is unavailable.\n";
    return 2;
  }
#endif
  const bool zeroCopyVulkanDetector = !options.materializedGenerateFacestream;
  const QString normalizedScrfdModelVariant =
      jcut::facedetections::normalizeScrfdModelVariantId(
          options.scrfdModelVariant);
  const QString res10ParamPath = findRes10NcnnModelFile(
      options.res10ParamPath, QStringLiteral("res10_300x300_ssd_ncnn.param"));
  const QString res10BinPath = findRes10NcnnModelFile(
      options.res10BinPath, QStringLiteral("res10_300x300_ssd_ncnn.bin"));
  QString scrfdParamPath;
  QString scrfdBinPath;
  if (options.scrfdDetector) {
    if (!options.scrfdParamPath.trimmed().isEmpty() ||
        !options.scrfdBinPath.trimmed().isEmpty()) {
      scrfdParamPath =
          findRes10NcnnModelFile(options.scrfdParamPath,
                                 scrfdModelFileName(normalizedScrfdModelVariant,
                                                    QStringLiteral(".param")));
      scrfdBinPath = findRes10NcnnModelFile(
          options.scrfdBinPath, scrfdModelFileName(normalizedScrfdModelVariant,
                                                   QStringLiteral(".bin")));
      if (!QFileInfo::exists(scrfdParamPath) ||
          !QFileInfo::exists(scrfdBinPath)) {
        std::cerr << "Explicit SCRFD model paths are missing for variant "
                  << normalizedScrfdModelVariant.toStdString()
                  << ". param=" << scrfdParamPath.toStdString()
                  << " bin=" << scrfdBinPath.toStdString() << "\n";
        return 2;
      }
    } else {
      if (!jcut::facedetections::ensureScrfdModelVariantAssets(
              normalizedScrfdModelVariant, &scrfdParamPath, &scrfdBinPath,
              &error)) {
        std::cerr << error.toStdString() << "\n";
        return 2;
      }
    }
  }
  if (options.requireZeroCopy && options.materializedGenerateFacestream) {
    std::cerr << "Materialized Generate FaceDetections mode cannot satisfy "
                 "--require-zero-copy.\n";
    return 2;
  }
  if (zeroCopyVulkanDetector && !options.heuristicZeroCopyDetector &&
      !options.scrfdDetector) {
    std::cout << "detector=res10_ssd_ncnn_vulkan_zero_copy_v1"
              << " name=\"JCut DNN FaceDetections Generator\""
              << " inference_backend=ncnn_vulkan" << " model_param=\""
              << res10ParamPath.toStdString() << "\"" << " model_bin=\""
              << res10BinPath.toStdString() << "\""
              << " qimage_materialized=0\n";
  } else if (options.heuristicZeroCopyDetector) {
    std::cout << "detector=native_jcut_heuristic_zero_copy_v1"
              << " name=\"JCut DNN FaceDetections Generator\""
              << " decode=hardware_zero_copy" << " qimage_materialized=0\n";
  } else if (options.scrfdDetector) {
    std::cout << "detector=scrfd_" << normalizedScrfdModelVariant.toStdString()
              << "_ncnn_vulkan_zero_copy_v1"
              << " name=\"JCut SCRFD FaceDetections Generator\""
              << " inference_backend=ncnn_vulkan" << " scrfd_model_variant=\""
              << normalizedScrfdModelVariant.toStdString() << "\""
              << " model_param=\"" << scrfdParamPath.toStdString() << "\""
              << " model_bin=\"" << scrfdBinPath.toStdString() << "\""
              << " qimage_materialized=0" << " zero_copy=1\n";
  }
  const QString backend = backendIdForOptions(options);

  int decoded = 0;
  int processed = 0;
  int cpuFrames = 0;
  int hardwareFrames = 0;
  int totalDetections = 0;
  int previewWritten = 0;
  int appVulkanFramePathFrames = 0;
  int decoderDirectHandoffFrames = 0;
  int decoderVulkanUploadFallbackFrames = 0;
  int hardwareDirectHandoffFrames = 0;
  int hardwareInteropProbeSupportedFrames = 0;
  int hardwareInteropProbeFailedFrames = 0;
  QString lastHardwareInteropProbeReason;
  QString lastHardwareInteropProbePath;
  QString lastHardwareDirectAttemptReason;
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
  QJsonArray frameRows;
  QJsonArray rawDetectionFrames;
  QHash<int, int> rawDetectionFrameIndexByFrame;
  QHash<int, QJsonArray> trackDetectionsByFrame;
  bool printedAppVulkanFailure = false;
  VulkanHarnessContext detectorContext;
  jcut::vulkan_detector::VulkanDetectorFrameHandoff decoderFrameHandoff;
  jcut::vulkan_detector::VulkanZeroCopyFaceDetector zeroCopyDetector;
  jcut::vulkan_detector::VulkanRes10NcnnFaceDetector res10Detector;
  jcut::vulkan_detector::VulkanScrfdNcnnFaceDetector scrfdDetector;
  const int decoderDirectPipelineSlots = std::clamp(
      options.detectorPipelineSlots, 1, kMaxDecoderDirectPipelineSlots);
  const int detectorWorkersActive =
      std::clamp(options.detectorWorkers, 1, kMaxDecoderDirectPipelineSlots);
  std::vector<std::unique_ptr<DecoderDetectorWorker>> decoderDetectorWorkers;
  decoderDetectorWorkers.reserve(detectorWorkersActive);
  for (int i = 0; i < detectorWorkersActive; ++i) {
    decoderDetectorWorkers.push_back(std::make_unique<DecoderDetectorWorker>());
  }
  PreparedDecoderDetectionSlot
      decoderDirectSlots[kMaxDecoderDirectPipelineSlots];
  std::deque<int> pendingDecoderDirectSlots;
  int nextDecoderDirectSlot = 0;
  RuntimeTuning tuning{options.stride,
                       options.maxDetections,
                       options.scrfdTargetSize,
                       options.maxFacesPerFrame,
                       jcut::facedetections::normalizeScrfdModelVariantId(
                           options.scrfdModelVariant),
                       options.threshold,
                       options.nmsIouThreshold,
                       options.trackMatchIouThreshold,
                       options.newTrackMinConfidence,
                       options.primaryFaceOnly,
                       options.smallFaceFallback,
                       options.scrfdTiled};
  PreviewDebugSettings previewDebugSettings;
  QDateTime paramsMtime;
  const QString detectorSettingsPath =
      detectorSettingsPathForVideo(options.videoPath);
  QDateTime detectorSettingsMtime;
  QFileInfo detectorSettingsInfo(detectorSettingsPath);
  if (applyRuntimeParamsFile(detectorSettingsPath, detectorSettingsInfo,
                             &tuning, &previewDebugSettings,
                             &detectorSettingsMtime) &&
      options.verbose) {
    std::cout << "loaded_detector_settings=\""
              << detectorSettingsPath.toStdString() << "\"\n";
  }
  DetectorControlPanel detectorControls;
  bool runtimePaused = false;
  if (livePreviewWindow) {
    livePreviewWindow->setFollowLatest(previewDebugSettings.followLatest);
    livePreviewWindow->setPreviewPlaybackSpeed(
        previewDebugSettings.playbackSpeed);
    livePreviewWindow->setShowDetections(previewDebugSettings.showDetections);
    livePreviewWindow->setShowTracks(previewDebugSettings.showTracks);
    livePreviewWindow->setShowLabels(previewDebugSettings.showLabels);
    livePreviewWindow->setShowConfirmedTracks(
        previewDebugSettings.showConfirmedTracks);
    livePreviewWindow->setShowTentativeTracks(
        previewDebugSettings.showTentativeTracks);
    livePreviewWindow->setShowLostTracks(previewDebugSettings.showLostTracks);
    livePreviewWindow->setDetectionLineThickness(
        previewDebugSettings.detectionLineThickness);
    livePreviewWindow->setTrackLineThickness(
        previewDebugSettings.trackLineThickness);
    livePreviewWindow->setOverlayOpacity(previewDebugSettings.overlayOpacity);
  }
  if (showControlWindow || livePreviewWindow) {
    detectorControls = createDetectorControlPanel(
        &tuning, options.detector, options.scrfdTargetSize,
        detectorSettingsPath, &options.applyClipGrading,
        !options.clipJsonPath.trimmed().isEmpty(), livePreviewWindow.get(),
        &runtimePaused, options.detectorWorkers, detectorWorkersActive,
        decoderDirectPipelineSlots);
    detectorControls.window->show();
    QString saveError;
    if (saveRuntimeTuningFile(detectorSettingsPath, tuning, options.detector,
                              options.scrfdTargetSize, &previewDebugSettings,
                              &saveError)) {
      detectorSettingsMtime = QFileInfo(detectorSettingsPath).lastModified();
    } else if (!saveError.isEmpty()) {
      std::cerr << saveError.toStdString() << "\n";
    }
    appPtr->processEvents();
  }

  auto currentRoiRect = [&](const QSize &size) -> QRectF {
    return QRectF(tuning.roiX1 * size.width(), tuning.roiY1 * size.height(),
                  (tuning.roiX2 - tuning.roiX1) * size.width(),
                  (tuning.roiY2 - tuning.roiY1) * size.height());
  };
  auto liveTrackCount = [&](const QVector<Track> &tracks) {
    int count = 0;
    for (const Track &track : tracks) {
      if (track.state != jcut::facedetections::ContinuityTrackState::Removed) {
        ++count;
      }
    }
    return count;
  };
  int livePreviewQueued = 0;
  int livePreviewPresented = 0;
  int livePreviewDropped = 0;
  int livePreviewDetectorThreadRenders = 0;
  int livePreviewPresentSamples = 0;
  double livePreviewPresentMsTotal = 0.0;
  auto presentLivePreviewWindow =
      [&](const render_detail::OffscreenVulkanFrame &frame, int frameNumber,
          const QVector<Track> &previewTracks,
          const QVector<Detection> &previewDetections,
          const QString &titlePrefix) -> bool {
    if (!livePreviewWindow) {
      return false;
    }
    livePreviewWindow->setWindowTitle(
        QStringLiteral("%1 - frame %2").arg(titlePrefix).arg(frameNumber));
    livePreviewWindow->setStatusText(
        QStringLiteral("%1 | frame %2 | detections %3 | live tracks %4")
            .arg(titlePrefix)
            .arg(frameNumber)
            .arg(previewDetections.size())
            .arg(liveTrackCount(previewTracks)));
    const bool presented = livePreviewWindow->presentFrame(
        frame, frameNumber, previewTracks, previewDetections,
        currentRoiRect(frame.size), previewDetections.size());
    return presented;
  };
  auto renderAndPresentLivePreviewWindow =
      [&](int frameNumber, const QVector<Track> &previewTracks,
          const QVector<Detection> &previewDetections,
          const QString &titlePrefix) -> bool {
    if (!livePreviewWindow) {
      return false;
    }
    QElapsedTimer previewTimer;
    previewTimer.start();
    render_detail::OffscreenVulkanFrame previewFrame;
    QString previewError;
    if (!jcut::facedetections::renderFrameToVulkan(
            &appFrameProvider, sourceClip, options.videoPath, frameNumber,
            frameNumber, renderSize, &previewFrame, nullptr, &previewError)) {
      setPreviewStatusText(previewError);
      return false;
    }
    ++livePreviewDetectorThreadRenders;
    const bool presented =
        presentLivePreviewWindow(previewFrame, frameNumber, previewTracks,
                                 previewDetections, titlePrefix);
    livePreviewPresentMsTotal +=
        static_cast<double>(previewTimer.nsecsElapsed()) / 1'000'000.0;
    ++livePreviewPresentSamples;
    return presented;
  };
  auto opportunisticLivePreviewDue = [&](int frameNumber) {
    return livePreviewWindow && !runtimePaused &&
           (frameNumber % effectivePreviewStride) == 0;
  };
  std::deque<LivePreviewSample> livePreviewQueue;
  constexpr int kLivePreviewQueueCapacity = 3;
  constexpr double kLivePreviewMinPresentIntervalSec = 0.35;
  auto lastLivePreviewPresentAt =
      std::chrono::steady_clock::now() - std::chrono::seconds(1);
  auto enqueueLivePreviewSample =
      [&](int frameNumber, const QVector<Track> &tracks,
          const QVector<Detection> &detections, const QString &titlePrefix) {
        if (!opportunisticLivePreviewDue(frameNumber)) {
          return;
        }
        while (static_cast<int>(livePreviewQueue.size()) >=
               kLivePreviewQueueCapacity) {
          livePreviewQueue.pop_front();
          ++livePreviewDropped;
        }
        livePreviewQueue.push_back(
            LivePreviewSample{frameNumber, tracks, detections, titlePrefix});
        ++livePreviewQueued;
      };
  auto drainLivePreviewQueue = [&](bool force = false) {
    if (!livePreviewWindow || livePreviewQueue.empty() || runtimePaused) {
      return;
    }
    const auto now = std::chrono::steady_clock::now();
    const double sinceLastSec =
        std::chrono::duration<double>(now - lastLivePreviewPresentAt).count();
    if (!force && sinceLastSec < kLivePreviewMinPresentIntervalSec) {
      return;
    }
    LivePreviewSample sample = std::move(livePreviewQueue.back());
    livePreviewDropped +=
        qMax(0, static_cast<int>(livePreviewQueue.size()) - 1);
    livePreviewQueue.clear();
    if (renderAndPresentLivePreviewWindow(sample.frameNumber, sample.tracks,
                                          sample.detections,
                                          sample.titlePrefix)) {
      ++livePreviewPresented;
      lastLivePreviewPresentAt = now;
    }
  };
  auto emitPreview = [&](const QImage &image, int frameNumber,
                         const QVector<Track> &tracks,
                         const QVector<Detection> &detections,
                         const QString &titlePrefix) -> bool {
    if (image.isNull()) {
      return false;
    }
    const bool previewFrameDue =
        !previewPipelinePrimed || ((frameNumber % effectivePreviewStride) == 0);
    const bool needsPreviewFrame =
        previewFrameDue &&
        (previewSocketPtr ||
         (options.writePreviewFiles && previewWritten < options.previewFrames));
    if (!needsPreviewFrame) {
      return false;
    }
    const QImage preview =
        buildPreview(image, tracks, detections, currentRoiRect(image.size()));
    bool emitted = false;
    if (previewSocketPtr) {
      emitted = sendPreviewFrame(previewSocketPtr, preview) || emitted;
    }
    if (options.writePreviewFiles && previewWritten < options.previewFrames) {
      const QString previewPath =
          QDir(options.outputDir)
              .filePath(QStringLiteral("preview_%1.png")
                            .arg(frameNumber, 6, 10, QChar('0')));
      if (preview.save(previewPath)) {
        ++previewWritten;
        emitted = true;
      }
    }
    previewPipelinePrimed = previewPipelinePrimed || emitted;
    return emitted;
  };
  auto requireSynchronizedPreview =
      [&](int frameNumber, const editor::FrameHandle &frameHandle,
          const QVector<Track> &tracks, const QVector<Detection> &detections,
          const QString &titlePrefix,
          const std::function<QImage()> &previewImageProvider) -> bool {
    Q_UNUSED(frameHandle);
    const bool previewFrameDue =
        !previewPipelinePrimed || ((frameNumber % effectivePreviewStride) == 0);
    if (!previewFrameDue) {
      return true;
    }
    if (!previewRequiresSynchronizedDetection) {
      if (previewSocketPtr || (options.writePreviewFiles &&
                               previewWritten < options.previewFrames)) {
        const QImage previewImage = previewImageProvider();
        if (!previewImage.isNull()) {
          emitPreview(previewImage, frameNumber, tracks, detections,
                      titlePrefix);
        }
      }
      appPtr->processEvents(QEventLoop::AllEvents, 1);
      return true;
    }
    bool emitted = false;
    if (livePreviewWindow) {
      emitted = renderAndPresentLivePreviewWindow(frameNumber, tracks,
                                                  detections, titlePrefix) ||
                emitted;
    }
    if (previewSocketPtr ||
        (options.writePreviewFiles && previewWritten < options.previewFrames)) {
      const QImage previewImage = previewImageProvider();
      if (!previewImage.isNull()) {
        emitted = emitPreview(previewImage, frameNumber, tracks, detections,
                              titlePrefix) ||
                  emitted;
      }
    }
    if (emitted) {
      previewPipelinePrimed = true;
      appPtr->processEvents(QEventLoop::AllEvents, 1);
      return true;
    }
    setPreviewStatusText(
        QStringLiteral("Preview sync failed at frame %1").arg(frameNumber));
    appPtr->processEvents();
    disableSynchronizedPreview(
        QStringLiteral("frame %1 preview upload failed").arg(frameNumber));
    return true;
  };
  auto requireSynchronizedOffscreenPreview =
      [&](int frameNumber, const render_detail::OffscreenVulkanFrame &frame,
          const QVector<Track> &tracks, const QVector<Detection> &detections,
          const QString &titlePrefix,
          const std::function<QImage()> &previewImageProvider) -> bool {
    Q_UNUSED(frame);
    const bool previewFrameDue =
        !previewPipelinePrimed || ((frameNumber % effectivePreviewStride) == 0);
    if (!previewFrameDue) {
      return true;
    }
    if (!previewRequiresSynchronizedDetection) {
      enqueueLivePreviewSample(frameNumber, tracks, detections, titlePrefix);
      if (previewSocketPtr || (options.writePreviewFiles &&
                               previewWritten < options.previewFrames)) {
        const QImage previewImage = previewImageProvider();
        if (!previewImage.isNull()) {
          emitPreview(previewImage, frameNumber, tracks, detections,
                      titlePrefix);
        }
      }
      appPtr->processEvents(QEventLoop::AllEvents, 1);
      return true;
    }
    bool emitted = false;
    if (livePreviewWindow) {
      emitted = presentLivePreviewWindow(frame, frameNumber, tracks, detections,
                                         titlePrefix) ||
                emitted;
    }
    if (previewSocketPtr ||
        (options.writePreviewFiles && previewWritten < options.previewFrames)) {
      const QImage previewImage = previewImageProvider();
      if (!previewImage.isNull()) {
        emitted = emitPreview(previewImage, frameNumber, tracks, detections,
                              titlePrefix) ||
                  emitted;
      }
    }
    if (emitted) {
      previewPipelinePrimed = true;
      appPtr->processEvents(QEventLoop::AllEvents, 1);
      return true;
    }
    setPreviewStatusText(
        QStringLiteral("Preview sync failed at frame %1").arg(frameNumber));
    appPtr->processEvents();
    disableSynchronizedPreview(
        QStringLiteral("frame %1 preview upload failed").arg(frameNumber));
    return true;
  };
  const int totalFrames = static_cast<int>(qMax<int64_t>(1, targetFrames));
  const int finalFrame = qMax(0, options.startFrame + totalFrames - 1);
  const QString faceStreamPath =
      QDir(options.outputDir).filePath(QStringLiteral("facedetections.part"));
  const QString resumeIndexPath = faceDetectionsResumeIndexPath(faceStreamPath);
  FaceDetectionsResumeState resume;
  QString resumeError;
  QElapsedTimer resumeLoadTimer;
  resumeLoadTimer.start();
  if (!loadFaceDetectionsResumeIndex(
          resumeIndexPath, faceStreamPath, options.videoPath, backend,
          options.startFrame, finalFrame, &resume, &resumeError)) {
    if (options.verbose && !resumeError.isEmpty()) {
      std::cerr << "Fast facedetections resume index unavailable: "
                << resumeError.toStdString()
                << "; falling back to checkpoint replay.\n";
    }
    resumeError.clear();
    if (!loadFaceDetectionsResume(faceStreamPath, options.videoPath, backend,
                                  options.startFrame, finalFrame, &resume,
                                  &resumeError)) {
      const QString stalePath = faceStreamPath + QStringLiteral(".stale");
      QFile::remove(stalePath);
      QFile::rename(faceStreamPath, stalePath);
      QFile::remove(resumeIndexPath);
      if (options.verbose && !resumeError.isEmpty()) {
        std::cerr << "Ignoring previous facedetections checkpoint: "
                  << resumeError.toStdString() << "\n";
      }
      resume = FaceDetectionsResumeState{};
    }
  } else if (options.verbose) {
    std::cout << "resumed_facedetections_index=\""
              << resumeIndexPath.toStdString()
              << "\" completed_frames=" << resume.completedFrames.size()
              << " load_ms=" << resumeLoadTimer.elapsed() << "\n";
  }
  int resumeStartOffset = 0;
  while (
      resumeStartOffset < totalFrames &&
      resume.completedFrames.contains(options.startFrame + resumeStartOffset)) {
    ++resumeStartOffset;
  }
  const int previewStartFrame =
      qMin(finalFrame, options.startFrame + qMax(0, resumeStartOffset));

  if (!previewPipelinePrimed) {
    auto tryWarmupPreview = [&]() -> bool {
      const QVector<Track> warmupTracks;
      const QVector<Detection> warmupDetections;
      bool emitted = false;
      if (livePreviewWindow) {
        emitted =
            renderAndPresentLivePreviewWindow(
                previewStartFrame, warmupTracks, warmupDetections,
                QStringLiteral(
                    "JCut DNN FaceDetections Generator (Preview Warmup)")) ||
            emitted;
      }
      if (previewSocketPtr || (options.writePreviewFiles &&
                               previewWritten < options.previewFrames)) {
        jcut::facedetections::VulkanFrameStats warmupStats;
        const QImage warmupImage = jcut::facedetections::renderFrameWithVulkan(
            &appFrameProvider, sourceClip, options.videoPath, previewStartFrame,
            previewStartFrame, renderSize, &warmupStats);
        if (!warmupImage.isNull() &&
            emitPreview(
                warmupImage, previewStartFrame, warmupTracks, warmupDetections,
                QStringLiteral(
                    "JCut DNN FaceDetections Generator (Preview Warmup)"))) {
          emitted = true;
        }
      }
      return emitted;
    };

    if (livePreviewWindow) {
      setPreviewStatusText(
          QStringLiteral("Waiting for JCut DNN FaceDetections preview before "
                         "detection starts..."));
      appPtr->processEvents();
    }
    QElapsedTimer previewWarmupTimer;
    previewWarmupTimer.start();
    constexpr qint64 kPreviewWarmupStatusIntervalMs = 3000;
    while (!previewPipelinePrimed) {
      if (tryWarmupPreview()) {
        break;
      }
      if (livePreviewWindow) {
        setPreviewStatusText(
            QStringLiteral("Waiting for preview frame %1 | %2 ms")
                .arg(previewStartFrame)
                .arg(previewWarmupTimer.elapsed()));
      }
      if (livePreviewWindow && livePreviewWindow->hasFailed()) {
        const QString failureReason =
            livePreviewWindow->failureReason().trimmed().isEmpty()
                ? QStringLiteral(
                      "The Dear ImGui preview window failed during warmup.")
                : livePreviewWindow->failureReason().trimmed();
        setPreviewStatusText(
            QStringLiteral("Preview warmup failed | %1").arg(failureReason));
        appPtr->processEvents();
        std::cerr << "Preview warmup failed before detection start: "
                  << failureReason.toStdString() << "\n";
        return 2;
      }
      appPtr->processEvents();
      usleep(50 * 1000);
    }
    if (previewPipelinePrimed && livePreviewWindow) {
      setPreviewStatusText(
          QStringLiteral("Preview ready. Starting FaceDetections detection."));
      appPtr->processEvents();
    }
  }

  auto wallStart = std::chrono::steady_clock::now();
  processed = resume.processed;
  totalDetections = resume.totalDetections;
  appVulkanFramePathFrames = resume.appVulkanFramePathFrames;
  decoderDirectHandoffFrames = resume.decoderDirectHandoffFrames;
  decoderVulkanUploadFallbackFrames = resume.decoderVulkanUploadFallbackFrames;
  hardwareDirectHandoffFrames = resume.hardwareDirectHandoffFrames;
  hardwareInteropProbeSupportedFrames =
      resume.hardwareInteropProbeSupportedFrames;
  hardwareInteropProbeFailedFrames = resume.hardwareInteropProbeFailedFrames;
  hardwareFrames = resume.hardwareFrames;
  cpuFrames = resume.cpuFrames;
  renderDecodeMsTotal = resume.renderDecodeMsTotal;
  renderCompositeMsTotal = resume.renderCompositeMsTotal;
  renderReadbackMsTotal = resume.renderReadbackMsTotal;
  decoderUploadMsTotal = resume.decoderUploadMsTotal;
  vulkanDetectMsTotal = resume.vulkanDetectMsTotal;
  frameRows = resume.frameRows;
  rawDetectionFrames = resume.rawDetectionFrames;
  trackDetectionsByFrame = resume.trackDetectionsByFrame;
  for (int i = 0; i < rawDetectionFrames.size(); ++i) {
    const int frameNumber = rawDetectionFrames.at(i)
                                .toObject()
                                .value(QStringLiteral("frame"))
                                .toInt(-1);
    if (frameNumber >= 0) {
      rawDetectionFrameIndexByFrame.insert(frameNumber, i);
    }
  }

  const bool faceStreamExists =
      QFileInfo::exists(faceStreamPath) && QFileInfo(faceStreamPath).size() > 0;
  QFile faceStreamFile;
  std::unique_ptr<AsyncFaceStreamWriter> faceStreamWriter;
  if (options.asyncCheckpointWriter) {
    faceStreamWriter = std::make_unique<AsyncFaceStreamWriter>(
        options.checkpointWriterQueueCapacity);
    QString writerError;
    if (!faceStreamWriter->open(faceStreamPath, &writerError)) {
      std::cerr << writerError.toStdString() << "\n";
      return 2;
    }
  } else {
    faceStreamFile.setFileName(faceStreamPath);
    if (!faceStreamFile.open(QIODevice::WriteOnly | QIODevice::Append |
                             QIODevice::Text)) {
      std::cerr << "Failed to open streaming facedetections checkpoint: "
                << faceStreamPath.toStdString() << "\n";
      return 2;
    }
  }
  int faceStreamRecordsSubmitted = 0;
  int faceStreamRecordsWrittenSync = 0;
  auto appendFaceStreamRecord = [&](const QJsonObject &object,
                                    QString *error) -> bool {
    if (faceStreamWriter) {
      if (!faceStreamWriter->enqueue(object, error)) {
        return false;
      }
      ++faceStreamRecordsSubmitted;
      return true;
    }
    if (!appendBinaryCborRecord(&faceStreamFile, object)) {
      if (error) {
        *error = QStringLiteral(
                     "Failed to append streaming facedetections checkpoint: %1")
                     .arg(faceStreamPath);
      }
      return false;
    }
    ++faceStreamRecordsSubmitted;
    ++faceStreamRecordsWrittenSync;
    return true;
  };
  if (!faceStreamExists) {
    QString writerError;
    if (!appendFaceStreamRecord(
            QJsonObject{{QStringLiteral("type"), QStringLiteral("meta")},
                        {QStringLiteral("schema"),
                         QStringLiteral("jcut_facedetections_part_v1")},
                        {QStringLiteral("video"), options.videoPath},
                        {QStringLiteral("backend"), backend},
                        {QStringLiteral("start_frame"), options.startFrame},
                        {QStringLiteral("end_frame"), finalFrame},
                        {QStringLiteral("stride"), tuning.stride},
                        {QStringLiteral("created_utc_ms"),
                         QDateTime::currentDateTimeUtc().toMSecsSinceEpoch()}},
            &writerError)) {
      std::cerr << writerError.toStdString() << "\n";
      return 2;
    }
  }
  if (options.verbose && !resume.completedFrames.isEmpty()) {
    std::cout << "resumed_facedetections_checkpoint=\""
              << faceStreamPath.toStdString()
              << "\" completed_frames=" << resume.completedFrames.size()
              << "\n";
  }
  int lastProgressPercent = -1;
  std::chrono::steady_clock::time_point lastProgressAt = wallStart;
  std::chrono::steady_clock::time_point lastRuntimeStatsAt =
      wallStart - std::chrono::seconds(1);
  std::chrono::steady_clock::time_point lastLiveTelemetryAt =
      wallStart - std::chrono::seconds(3);
  AdaptiveEtaTracker etaTracker;
  DetectorStageTimingTotals stageTimings;
  DetectorStageTimingTotals lastLiveTelemetryStageTimings;
  int latestProcessedFrame = options.startFrame - 1;
  int lastLiveTelemetryProcessed = processed;
  int lastLiveTelemetryTotalDetections = totalDetections;
  double lastLiveTelemetryDecoderUploadMsTotal = decoderUploadMsTotal;
  double lastLiveTelemetryNcnnInputMsTotal = ncnnInputMsTotal;
  double lastLiveTelemetryNcnnExtractMsTotal = ncnnExtractMsTotal;
  double lastLiveTelemetryNcnnExtractLevel8MsTotal = ncnnExtractLevel8MsTotal;
  double lastLiveTelemetryNcnnExtractLevel16MsTotal = ncnnExtractLevel16MsTotal;
  double lastLiveTelemetryNcnnExtractLevel32MsTotal = ncnnExtractLevel32MsTotal;
  double lastLiveTelemetryNcnnPostMsTotal = ncnnPostMsTotal;
  double lastLiveTelemetryNcnnTotalMsTotal = ncnnTotalMsTotal;
  for (int completedFrame : resume.completedFrames) {
    latestProcessedFrame = qMax(latestProcessedFrame, completedFrame);
  }
  syncDetectorPreviewPanel(&detectorControls, livePreviewWindow.get(),
                           options.startFrame,
                           qMax(options.startFrame, latestProcessedFrame));
  QVector<Track> runtimeTracks =
      resume.loadedFromCompactIndex
          ? resume.runtimeTracks
          : jcut::facedetections::buildContinuityTracksFromDetectionFrames(
                rawDetectionFrames, trackingTuningForRuntime(tuning));
  int lastResumeIndexSaveProcessed = resume.processed;
  auto saveCompactResumeIndex = [&](bool force = false) {
    if (resume.completedFrames.isEmpty()) {
      return;
    }
    if (!force && (processed - lastResumeIndexSaveProcessed) < 100) {
      return;
    }
    if (faceStreamWriter &&
        (faceStreamWriter->backlog() > 0 ||
         faceStreamWriter->recordsWritten() < faceStreamRecordsSubmitted)) {
      return;
    }
    if (faceStreamFile.isOpen()) {
      faceStreamFile.flush();
    }
    QString indexError;
    if (!saveFaceDetectionsResumeIndex(resumeIndexPath, faceStreamPath,
                                       options.videoPath, backend,
                                       options.startFrame, finalFrame, resume,
                                       runtimeTracks, &indexError)) {
      if (options.verbose && !indexError.isEmpty()) {
        std::cerr << "Failed to save facedetections resume index: "
                  << indexError.toStdString() << "\n";
      }
      return;
    }
    lastResumeIndexSaveProcessed = processed;
  };
  auto detectionsForFrame = [&](int frameNumber) {
    QVector<Detection> frameDetections;
    const int index = rawDetectionFrameIndexByFrame.value(frameNumber, -1);
    if (index < 0 || index >= rawDetectionFrames.size()) {
      return frameDetections;
    }
    const QJsonObject frameObject = rawDetectionFrames.at(index).toObject();
    const QJsonArray detectionRows =
        frameObject.value(QStringLiteral("detections")).toArray();
    frameDetections.reserve(detectionRows.size());
    for (const QJsonValue &detectionValue : detectionRows) {
      const QJsonObject detectionObject = detectionValue.toObject();
      Detection detection;
      detection.box =
          QRectF(detectionObject.value(QStringLiteral("x")).toDouble(),
                 detectionObject.value(QStringLiteral("y")).toDouble(),
                 detectionObject.value(QStringLiteral("w")).toDouble(),
                 detectionObject.value(QStringLiteral("h")).toDouble());
      detection.confidence = static_cast<float>(
          detectionObject.value(QStringLiteral("confidence")).toDouble());
      if (detection.box.isValid() && !detection.box.isEmpty()) {
        frameDetections.push_back(detection);
      }
    }
    return frameDetections;
  };
  auto tracksForFrame = [&](int frameNumber) {
    QVector<Track> frameTracks;
    const QJsonArray trackRows = trackDetectionsByFrame.value(frameNumber);
    frameTracks.reserve(trackRows.size());
    for (const QJsonValue &trackValue : trackRows) {
      const QJsonObject trackObject = trackValue.toObject();
      Track track;
      track.id = trackObject.value(QStringLiteral("track_id")).toInt(-1);
      track.box =
          QRectF(trackObject.value(QStringLiteral("track_box_x")).toDouble(),
                 trackObject.value(QStringLiteral("track_box_y")).toDouble(),
                 trackObject.value(QStringLiteral("track_box_w")).toDouble(),
                 trackObject.value(QStringLiteral("track_box_h")).toDouble());
      track.firstFrame =
          trackObject.value(QStringLiteral("first_frame")).toInt(frameNumber);
      track.lastFrame =
          trackObject.value(QStringLiteral("last_frame")).toInt(frameNumber);
      track.hits = trackObject.value(QStringLiteral("hits")).toInt(0);
      track.misses = trackObject.value(QStringLiteral("misses")).toInt(0);
      const QString state =
          trackObject.value(QStringLiteral("track_state")).toString();
      if (state == QStringLiteral("confirmed")) {
        track.state = jcut::facedetections::ContinuityTrackState::Confirmed;
      } else if (state == QStringLiteral("lost")) {
        track.state = jcut::facedetections::ContinuityTrackState::Lost;
      } else if (state == QStringLiteral("removed")) {
        track.state = jcut::facedetections::ContinuityTrackState::Removed;
      } else {
        track.state = jcut::facedetections::ContinuityTrackState::Tentative;
      }
      if (track.id >= 0 && track.box.isValid() && !track.box.isEmpty()) {
        frameTracks.push_back(track);
      }
    }
    return frameTracks;
  };
  auto refreshRuntimeStats = [&](int frameOffset, int frameNumber,
                                 int liveTracks, bool force = false) {
    if (!detectorControls.window) {
      return;
    }
    const auto now = std::chrono::steady_clock::now();
    const double sinceLastSec =
        std::chrono::duration<double>(now - lastRuntimeStatsAt).count();
    if (!force && sinceLastSec < 0.25) {
      return;
    }
    lastRuntimeStatsAt = now;
    const double elapsedSec =
        std::chrono::duration<double>(now - wallStart).count();
    const int busyWorkers = std::count_if(
        decoderDetectorWorkers.begin(), decoderDetectorWorkers.end(),
        [](const auto &worker) { return worker && worker->busy; });
    DetectorLiveTelemetrySnapshot liveTelemetry;
    for (int slotIndex = 0; slotIndex < decoderDirectPipelineSlots;
         ++slotIndex) {
      const auto handoffStats =
          decoderDirectSlots[slotIndex].handoff.resourceStats();
      liveTelemetry.handoffDescriptorAllocs +=
          handoffStats.descriptorAllocations;
      liveTelemetry.handoffDescriptorFrees += handoffStats.descriptorFrees;
      liveTelemetry.handoffImageAllocs += handoffStats.imageMemoryAllocations;
      liveTelemetry.handoffImageFrees += handoffStats.imageMemoryFrees;
      liveTelemetry.handoffImportedAllocs +=
          handoffStats.importedMemoryAllocations;
      liveTelemetry.handoffImportedFrees += handoffStats.importedMemoryFrees;
      liveTelemetry.handoffBufferAllocs +=
          handoffStats.stagingBufferAllocations;
      liveTelemetry.handoffBufferFrees += handoffStats.stagingBufferFrees;
      liveTelemetry.handoffPipelineCreates +=
          handoffStats.computePipelineCreations;
      const auto preprocessorStats =
          decoderDirectSlots[slotIndex].preprocessor.resourceStats();
      liveTelemetry.preprocDescriptorAllocs +=
          preprocessorStats.preprocessDescriptorAllocations;
      liveTelemetry.preprocDescriptorFrees +=
          preprocessorStats.preprocessDescriptorFrees;
      liveTelemetry.inferDescriptorAllocs +=
          preprocessorStats.inferenceDescriptorAllocations;
      liveTelemetry.inferDescriptorFrees +=
          preprocessorStats.inferenceDescriptorFrees;
      liveTelemetry.preprocPipelineCreates +=
          preprocessorStats.computePipelineCreations;
    }
    liveTelemetry.avgDecoderUploadMs =
        processed > 0 ? decoderUploadMsTotal / static_cast<double>(processed)
                      : 0.0;
    liveTelemetry.avgNcnnInputMs =
        processed > 0 ? ncnnInputMsTotal / static_cast<double>(processed) : 0.0;
    liveTelemetry.avgNcnnExtractMs =
        processed > 0 ? ncnnExtractMsTotal / static_cast<double>(processed)
                      : 0.0;
    liveTelemetry.avgNcnnExtractLevel8Ms =
        processed > 0
            ? ncnnExtractLevel8MsTotal / static_cast<double>(processed)
            : 0.0;
    liveTelemetry.avgNcnnExtractLevel16Ms =
        processed > 0
            ? ncnnExtractLevel16MsTotal / static_cast<double>(processed)
            : 0.0;
    liveTelemetry.avgNcnnExtractLevel32Ms =
        processed > 0
            ? ncnnExtractLevel32MsTotal / static_cast<double>(processed)
            : 0.0;
    liveTelemetry.avgNcnnPostMs =
        processed > 0 ? ncnnPostMsTotal / static_cast<double>(processed) : 0.0;
    liveTelemetry.avgNcnnTotalMs =
        processed > 0 ? ncnnTotalMsTotal / static_cast<double>(processed) : 0.0;
    liveTelemetry.pendingSlots =
        static_cast<int>(pendingDecoderDirectSlots.size());
    liveTelemetry.busyWorkers = busyWorkers;
    liveTelemetry.detectorWorkersActive = detectorWorkersActive;
    liveTelemetry.checkpointBacklog =
        faceStreamWriter ? faceStreamWriter->backlog() : 0;
    updateDetectorRuntimeStats(
        &detectorControls, frameOffset, totalFrames, frameNumber, processed,
        totalDetections, liveTracks,
        faceStreamWriter ? faceStreamWriter->backlog() : 0,
        options.detectorWorkers, detectorWorkersActive,
        decoderDirectPipelineSlots, elapsedSec, &etaTracker, &stageTimings,
        &liveTelemetry);
  };
  auto emitLiveStageTelemetry = [&](int frameOffset, int frameNumber,
                                    int liveTracks, bool force = false) {
    const auto now = std::chrono::steady_clock::now();
    const double sinceLastSec =
        std::chrono::duration<double>(now - lastLiveTelemetryAt).count();
    if (!force && sinceLastSec < 2.0) {
      return;
    }
    const int processedDelta = processed - lastLiveTelemetryProcessed;
    if (!force && processedDelta <= 0) {
      return;
    }
    lastLiveTelemetryAt = now;
    const double elapsedSec =
        std::chrono::duration<double>(now - wallStart).count();
    const int current = qBound(0, frameOffset + 1, qMax(1, totalFrames));
    const double decodeFps =
        elapsedSec > 0.0 ? static_cast<double>(current) / elapsedSec : 0.0;
    const double processedFps =
        elapsedSec > 0.0 ? static_cast<double>(processed) / elapsedSec : 0.0;
    const int detectionDelta =
        totalDetections - lastLiveTelemetryTotalDetections;
    const int busyWorkers = std::count_if(
        decoderDetectorWorkers.begin(), decoderDetectorWorkers.end(),
        [](const auto &worker) { return worker && worker->busy; });
    DetectorLiveTelemetrySnapshot resourceTelemetry;
    for (int slotIndex = 0; slotIndex < decoderDirectPipelineSlots;
         ++slotIndex) {
      const auto handoffStats =
          decoderDirectSlots[slotIndex].handoff.resourceStats();
      resourceTelemetry.handoffDescriptorAllocs +=
          handoffStats.descriptorAllocations;
      resourceTelemetry.handoffDescriptorFrees += handoffStats.descriptorFrees;
      resourceTelemetry.handoffImageAllocs +=
          handoffStats.imageMemoryAllocations;
      resourceTelemetry.handoffImageFrees += handoffStats.imageMemoryFrees;
      resourceTelemetry.handoffImportedAllocs +=
          handoffStats.importedMemoryAllocations;
      resourceTelemetry.handoffImportedFrees +=
          handoffStats.importedMemoryFrees;
      resourceTelemetry.handoffBufferAllocs +=
          handoffStats.stagingBufferAllocations;
      resourceTelemetry.handoffBufferFrees += handoffStats.stagingBufferFrees;
      resourceTelemetry.handoffPipelineCreates +=
          handoffStats.computePipelineCreations;
      const auto preprocessorStats =
          decoderDirectSlots[slotIndex].preprocessor.resourceStats();
      resourceTelemetry.preprocDescriptorAllocs +=
          preprocessorStats.preprocessDescriptorAllocations;
      resourceTelemetry.preprocDescriptorFrees +=
          preprocessorStats.preprocessDescriptorFrees;
      resourceTelemetry.inferDescriptorAllocs +=
          preprocessorStats.inferenceDescriptorAllocations;
      resourceTelemetry.inferDescriptorFrees +=
          preprocessorStats.inferenceDescriptorFrees;
      resourceTelemetry.preprocPipelineCreates +=
          preprocessorStats.computePipelineCreations;
    }
    const auto deltaAverage = [&](double totalNow, double totalThen) {
      const double delta = totalNow - totalThen;
      return processedDelta > 0 ? delta / static_cast<double>(processedDelta)
                                : 0.0;
    };
    const auto stageDeltaAverage =
        [&](double DetectorStageTimingTotals::*member) {
          const double totalNow = stageTimings.*member;
          const double totalThen = lastLiveTelemetryStageTimings.*member;
          return processedDelta > 0 ? (totalNow - totalThen) /
                                          static_cast<double>(processedDelta)
                                    : 0.0;
        };
    std::cout
        << "telemetry" << " frame=" << frameNumber << " current=" << current
        << " processed=" << processed << " live_tracks=" << liveTracks
        << " det_delta=" << detectionDelta << " decode_fps=" << std::fixed
        << std::setprecision(2) << decodeFps
        << " processed_fps=" << processedFps
        << " pending_slots=" << pendingDecoderDirectSlots.size()
        << " busy_workers=" << busyWorkers << " checkpoint_backlog="
        << (faceStreamWriter ? faceStreamWriter->backlog() : 0)
        << " avg_decode_ms="
        << stageDeltaAverage(&DetectorStageTimingTotals::decodeMsTotal)
        << " avg_handoff_ms="
        << stageDeltaAverage(&DetectorStageTimingTotals::handoffMsTotal)
        << " avg_infer_wall_ms="
        << stageDeltaAverage(&DetectorStageTimingTotals::inferenceWallMsTotal)
        << " avg_tracking_ms="
        << stageDeltaAverage(&DetectorStageTimingTotals::trackingMsTotal)
        << " avg_checkpoint_enqueue_ms="
        << stageDeltaAverage(&DetectorStageTimingTotals::checkpointWriteMsTotal)
        << " avg_decoder_upload_ms="
        << deltaAverage(decoderUploadMsTotal,
                        lastLiveTelemetryDecoderUploadMsTotal)
        << " avg_ncnn_input_ms="
        << deltaAverage(ncnnInputMsTotal, lastLiveTelemetryNcnnInputMsTotal)
        << " avg_ncnn_extract_ms="
        << deltaAverage(ncnnExtractMsTotal, lastLiveTelemetryNcnnExtractMsTotal)
        << " avg_ncnn_extract_level8_ms="
        << deltaAverage(ncnnExtractLevel8MsTotal,
                        lastLiveTelemetryNcnnExtractLevel8MsTotal)
        << " avg_ncnn_extract_level16_ms="
        << deltaAverage(ncnnExtractLevel16MsTotal,
                        lastLiveTelemetryNcnnExtractLevel16MsTotal)
        << " avg_ncnn_extract_level32_ms="
        << deltaAverage(ncnnExtractLevel32MsTotal,
                        lastLiveTelemetryNcnnExtractLevel32MsTotal)
        << " avg_ncnn_post_ms="
        << deltaAverage(ncnnPostMsTotal, lastLiveTelemetryNcnnPostMsTotal)
        << " avg_ncnn_total_ms="
        << deltaAverage(ncnnTotalMsTotal, lastLiveTelemetryNcnnTotalMsTotal)
        << " handoff_desc_allocs=" << resourceTelemetry.handoffDescriptorAllocs
        << " handoff_desc_frees=" << resourceTelemetry.handoffDescriptorFrees
        << " handoff_img_allocs=" << resourceTelemetry.handoffImageAllocs
        << " handoff_img_frees=" << resourceTelemetry.handoffImageFrees
        << " handoff_import_allocs=" << resourceTelemetry.handoffImportedAllocs
        << " handoff_import_frees=" << resourceTelemetry.handoffImportedFrees
        << " handoff_buf_allocs=" << resourceTelemetry.handoffBufferAllocs
        << " handoff_buf_frees=" << resourceTelemetry.handoffBufferFrees
        << " handoff_pipe_creates=" << resourceTelemetry.handoffPipelineCreates
        << " pre_desc_allocs=" << resourceTelemetry.preprocDescriptorAllocs
        << " pre_desc_frees=" << resourceTelemetry.preprocDescriptorFrees
        << " infer_desc_allocs=" << resourceTelemetry.inferDescriptorAllocs
        << " infer_desc_frees=" << resourceTelemetry.inferDescriptorFrees
        << " pre_pipe_creates=" << resourceTelemetry.preprocPipelineCreates
        << "\n";
    lastLiveTelemetryProcessed = processed;
    lastLiveTelemetryTotalDetections = totalDetections;
    lastLiveTelemetryDecoderUploadMsTotal = decoderUploadMsTotal;
    lastLiveTelemetryNcnnInputMsTotal = ncnnInputMsTotal;
    lastLiveTelemetryNcnnExtractMsTotal = ncnnExtractMsTotal;
    lastLiveTelemetryNcnnExtractLevel8MsTotal = ncnnExtractLevel8MsTotal;
    lastLiveTelemetryNcnnExtractLevel16MsTotal = ncnnExtractLevel16MsTotal;
    lastLiveTelemetryNcnnExtractLevel32MsTotal = ncnnExtractLevel32MsTotal;
    lastLiveTelemetryNcnnPostMsTotal = ncnnPostMsTotal;
    lastLiveTelemetryNcnnTotalMsTotal = ncnnTotalMsTotal;
    lastLiveTelemetryStageTimings = stageTimings;
  };
  auto advanceRuntimeTracks =
      [&](int frameNumber, const QVector<Detection> &detections,
          const QSize &detectionFrameSize) -> QVector<Track> {
    QElapsedTimer timer;
    timer.start();
    jcut::facedetections::updateContinuityTracks(
        &runtimeTracks, detections, frameNumber, detectionFrameSize,
        trackingTuningForRuntime(tuning));
    stageTimings.trackingMsTotal +=
        static_cast<double>(timer.nsecsElapsed()) / 1'000'000.0;
    ++stageTimings.trackingSamples;
    return runtimeTracks;
  };
  auto recordProcessedFrame =
      [&](int frameOffset, int frameNumber,
          const jcut::facedetections::VulkanFrameStats &renderStats,
          const QVector<Detection> &detections, const QVector<Track> &tracks,
          const QSize &detectionFrameSize, bool appVulkanFrame,
          bool decoderVulkanUploadFallback, bool hardwareDirectHandoff,
          double decoderUploadMs, double vulkanDetectMs,
          const jcut::vulkan_detector::NcnnInferenceStats &ncnnStats,
          const jcut::vulkan_detector::HardwareInteropProbeResult &handoffProbe,
          const QString &hardwareDirectAttemptReason) {
        ++processed;
        totalDetections += detections.size();
        renderDecodeMsTotal += renderStats.decodeMs;
        renderCompositeMsTotal += renderStats.compositeMs;
        renderReadbackMsTotal += renderStats.readbackMs;
        decoderUploadMsTotal += decoderUploadMs;
        vulkanDetectMsTotal += vulkanDetectMs;
        if (renderStats.decodeMs > 0.0) {
          stageTimings.decodeMsTotal += renderStats.decodeMs;
          ++stageTimings.decodeSamples;
        }
        if (vulkanDetectMs > 0.0) {
          stageTimings.inferenceWallMsTotal += vulkanDetectMs;
          ++stageTimings.inferenceSamples;
        }
        ncnnInputMsTotal += ncnnStats.inputMs;
        ncnnExtractMsTotal += ncnnStats.extractMs;
        ncnnExtractLevel8MsTotal += ncnnStats.extractLevel8Ms;
        ncnnExtractLevel16MsTotal += ncnnStats.extractLevel16Ms;
        ncnnExtractLevel32MsTotal += ncnnStats.extractLevel32Ms;
        ncnnPostMsTotal += ncnnStats.postMs;
        ncnnTotalMsTotal += ncnnStats.totalMs;
        const QString detectorId = backend;
        const bool qimageMaterialized = options.materializedGenerateFacestream;
        const QJsonObject rawDetectionFrame = buildRawDetectionFrameRecord(
            frameNumber, detectorId, detectionFrameSize, detections);
        rawDetectionFrames.append(rawDetectionFrame);
        rawDetectionFrameIndexByFrame.insert(frameNumber,
                                             rawDetectionFrames.size() - 1);
        const QJsonArray trackDetections =
            jcut::facedetections::frameTrackDetections(tracks, frameNumber);
        trackDetectionsByFrame.insert(frameNumber, trackDetections);
        latestProcessedFrame = qMax(latestProcessedFrame, frameNumber);
        const bool decoderDirectHandoff =
            !appVulkanFrame && hardwareDirectHandoff;
        const QJsonObject frameRow{
            {QStringLiteral("frame"), frameNumber},
            {QStringLiteral("detector"), detectorId},
            {QStringLiteral("detections"), detections.size()},
            {QStringLiteral("tracks"), trackDetections.size()},
            {QStringLiteral("app_vulkan_frame_path"), appVulkanFrame},
            {QStringLiteral("app_render_decode_ms"), renderStats.decodeMs},
            {QStringLiteral("app_render_texture_ms"), renderStats.textureMs},
            {QStringLiteral("app_render_composite_ms"),
             renderStats.compositeMs},
            {QStringLiteral("app_render_readback_ms"), renderStats.readbackMs},
            {QStringLiteral("vulkan_zero_copy_detection_ms"), vulkanDetectMs},
            {QStringLiteral("decoder_vulkan_upload_ms"), decoderUploadMs},
            {QStringLiteral("ncnn_input_ms"), ncnnStats.inputMs},
            {QStringLiteral("ncnn_extract_ms"), ncnnStats.extractMs},
            {QStringLiteral("ncnn_extract_level8_ms"),
             ncnnStats.extractLevel8Ms},
            {QStringLiteral("ncnn_extract_level16_ms"),
             ncnnStats.extractLevel16Ms},
            {QStringLiteral("ncnn_extract_level32_ms"),
             ncnnStats.extractLevel32Ms},
            {QStringLiteral("ncnn_post_ms"), ncnnStats.postMs},
            {QStringLiteral("ncnn_total_ms"), ncnnStats.totalMs},
            {QStringLiteral("decoder_vulkan_upload_fallback"),
             decoderVulkanUploadFallback},
            {QStringLiteral("hardware_direct_handoff"), hardwareDirectHandoff},
            {QStringLiteral("hardware_direct_attempt_reason"),
             hardwareDirectAttemptReason},
            {QStringLiteral("qimage_materialized"), qimageMaterialized}};
        frameRows.append(frameRow);
        QJsonObject streamRow = frameRow;
        streamRow.insert(QStringLiteral("type"), QStringLiteral("frame"));
        streamRow.insert(QStringLiteral("schema"),
                         QStringLiteral("jcut_facedetections_frame_v1"));
        streamRow.insert(QStringLiteral("video"), options.videoPath);
        streamRow.insert(QStringLiteral("backend"), backend);
        streamRow.insert(QStringLiteral("decoder_direct_handoff"),
                         decoderDirectHandoff);
        streamRow.insert(QStringLiteral("hardware_interop_probe_supported"),
                         handoffProbe.supported);
        streamRow.insert(QStringLiteral("hardware_interop_probe_failed"),
                         !handoffProbe.path.isEmpty() &&
                             !handoffProbe.supported);
        streamRow.insert(QStringLiteral("hardware_interop_probe_path"),
                         handoffProbe.path);
        streamRow.insert(QStringLiteral("hardware_interop_probe_reason"),
                         handoffProbe.reason);
        streamRow.insert(QStringLiteral("hardware_frame"),
                         appVulkanFrame || hardwareDirectHandoff);
        streamRow.insert(QStringLiteral("cpu_frame"),
                         qimageMaterialized || decoderVulkanUploadFallback);
        streamRow.insert(
            QStringLiteral("detection_boxes"),
            rawDetectionFrame.value(QStringLiteral("detections")).toArray());
        streamRow.insert(QStringLiteral("track_detections"), trackDetections);
        QElapsedTimer checkpointTimer;
        checkpointTimer.start();
        QString checkpointError;
        if (!appendFaceStreamRecord(streamRow, &checkpointError)) {
          std::cerr << checkpointError.toStdString() << "\n";
          return false;
        }
        stageTimings.checkpointWriteMsTotal +=
            static_cast<double>(checkpointTimer.nsecsElapsed()) / 1'000'000.0;
        ++stageTimings.checkpointWriteSamples;
        resume.completedFrames.insert(frameNumber);
        resume.processed = processed;
        resume.totalDetections = totalDetections;
        resume.appVulkanFramePathFrames = appVulkanFramePathFrames;
        resume.decoderDirectHandoffFrames = decoderDirectHandoffFrames;
        resume.decoderVulkanUploadFallbackFrames =
            decoderVulkanUploadFallbackFrames;
        resume.hardwareDirectHandoffFrames = hardwareDirectHandoffFrames;
        resume.hardwareInteropProbeSupportedFrames =
            hardwareInteropProbeSupportedFrames;
        resume.hardwareInteropProbeFailedFrames =
            hardwareInteropProbeFailedFrames;
        resume.hardwareFrames = hardwareFrames;
        resume.cpuFrames = cpuFrames;
        resume.renderDecodeMsTotal = renderDecodeMsTotal;
        resume.renderCompositeMsTotal = renderCompositeMsTotal;
        resume.renderReadbackMsTotal = renderReadbackMsTotal;
        resume.decoderUploadMsTotal = decoderUploadMsTotal;
        resume.vulkanDetectMsTotal = vulkanDetectMsTotal;
        resume.ncnnInputMsTotal = ncnnInputMsTotal;
        resume.ncnnExtractMsTotal = ncnnExtractMsTotal;
        resume.ncnnExtractLevel8MsTotal = ncnnExtractLevel8MsTotal;
        resume.ncnnExtractLevel16MsTotal = ncnnExtractLevel16MsTotal;
        resume.ncnnExtractLevel32MsTotal = ncnnExtractLevel32MsTotal;
        resume.ncnnPostMsTotal = ncnnPostMsTotal;
        resume.ncnnTotalMsTotal = ncnnTotalMsTotal;
        saveCompactResumeIndex(false);
        const bool logThisFrame =
            options.verbose ||
            (options.logInterval > 0 && (processed % options.logInterval) == 0);
        if (logThisFrame) {
          std::cout << "frame=" << frameNumber
                    << " detector=" << detectorId.toStdString()
                    << " detections=" << detections.size()
                    << " app_vulkan_frame_path=" << (appVulkanFrame ? 1 : 0)
                    << " render_decode_ms=" << renderStats.decodeMs
                    << " render_texture_ms=" << renderStats.textureMs
                    << " render_composite_ms=" << renderStats.compositeMs
                    << " render_readback_ms=" << renderStats.readbackMs
                    << " vulkan_zero_copy_detection_ms=" << vulkanDetectMs
                    << " decoder_vulkan_upload_ms=" << decoderUploadMs
                    << " ncnn_input_ms=" << ncnnStats.inputMs
                    << " ncnn_extract_ms=" << ncnnStats.extractMs
                    << " ncnn_extract_level8_ms=" << ncnnStats.extractLevel8Ms
                    << " ncnn_extract_level16_ms=" << ncnnStats.extractLevel16Ms
                    << " ncnn_extract_level32_ms=" << ncnnStats.extractLevel32Ms
                    << " ncnn_post_ms=" << ncnnStats.postMs
                    << " ncnn_total_ms=" << ncnnStats.totalMs
                    << " decoder_vulkan_upload_fallback="
                    << (decoderVulkanUploadFallback ? 1 : 0)
                    << " hardware_direct_handoff="
                    << (hardwareDirectHandoff ? 1 : 0)
                    << " hardware_direct_attempt_reason=\""
                    << hardwareDirectAttemptReason.toStdString() << "\""
                    << " qimage_materialized=" << (qimageMaterialized ? 1 : 0)
                    << "\n";
        }
        if (options.progress && !logThisFrame &&
            shouldRenderProgress(frameOffset, totalFrames, processed,
                                 &lastProgressPercent, &lastProgressAt)) {
          const auto now = std::chrono::steady_clock::now();
          const double elapsedSec =
              std::chrono::duration<double>(now - wallStart).count();
          renderProgressLine(frameOffset, totalFrames, frameNumber, processed,
                             totalDetections, detections.size(), elapsedSec,
                             processed > 0 ? vulkanDetectMsTotal / processed
                                           : 0.0,
                             &etaTracker);
        }
        refreshRuntimeStats(frameOffset, frameNumber, liveTrackCount(tracks));

        emitLiveStageTelemetry(frameOffset, frameNumber,
                               liveTrackCount(tracks));
        return true;
      };
  const auto emptyProbe = jcut::vulkan_detector::HardwareInteropProbeResult{};
  const auto emptyNcnnStats = jcut::vulkan_detector::NcnnInferenceStats{};
  const auto decoderDirectPipelineEnabled = [&]() {
    return preferDecoderDirectDetection && zeroCopyVulkanDetector &&
           !options.heuristicZeroCopyDetector &&
           (!options.scrfdDetector || !tuning.scrfdTiled);
  };
  auto prewarmDecoderDirectResources = [&]() -> bool {
    if (!decoderDirectPipelineEnabled() || resumeStartOffset >= totalFrames) {
      return true;
    }
    const int probeFrameNumber =
        options.startFrame + qMax(0, resumeStartOffset);
    editor::FrameHandle probeFrame = decoder.decodeFrame(probeFrameNumber);
    if (probeFrame.isNull()) {
      if (options.verbose) {
        std::cerr
            << "Decoder direct prewarm skipped: failed to decode probe frame "
            << probeFrameNumber << ".\n";
      }
      return true;
    }
    const QSize probeFrameSize =
        probeFrame.size().isValid() ? probeFrame.size() : renderSize;
    QElapsedTimer prewarmTimer;
    prewarmTimer.start();
    for (int slotIndex = 0; slotIndex < decoderDirectPipelineSlots;
         ++slotIndex) {
      PreparedDecoderDetectionSlot &slot = decoderDirectSlots[slotIndex];
      if (detectorWorkersActive > 1) {
        if (slot.context.device == VK_NULL_HANDLE &&
            !slot.context.initialize(&error)) {
          std::cerr << "Failed to prewarm independent Vulkan detector context "
                       "for slot "
                    << slotIndex << ": " << error.toStdString() << "\n";
          return false;
        }
      } else {
        if (detectorContext.device == VK_NULL_HANDLE &&
            !detectorContext.initialize(&error)) {
          std::cerr << "Failed to prewarm shared Vulkan detector context: "
                    << error.toStdString() << "\n";
          return false;
        }
        if (!slot.context.attachExternalDevice(
                detectorContext.detectorContext(), &error)) {
          std::cerr << "Failed to attach prewarm slot to shared Vulkan "
                       "detector context: "
                    << error.toStdString() << "\n";
          return false;
        }
      }
      if (!slot.handoff.isInitialized() &&
          !slot.handoff.initialize(slot.context.detectorContext(), &error)) {
        std::cerr
            << "Failed to prewarm decoder frame handoff resources for slot "
            << slotIndex << ": " << error.toStdString() << "\n";
        return false;
      }
      if (options.scrfdDetector) {
        if (!slot.preprocessor.isInitialized() &&
            !slot.preprocessor.initialize(slot.context.detectorContext(),
                                          &error)) {
          std::cerr << "Failed to prewarm SCRFD preprocessor for slot "
                    << slotIndex << ": " << error.toStdString() << "\n";
          return false;
        }
        if (!slot.scrfdDetector.isInitialized()) {
          ScopedStderrSilencer silence(!options.verbose);
          if (!slot.scrfdDetector.initialize(slot.context.detectorContext(),
                                             scrfdParamPath, scrfdBinPath,
                                             &error)) {
            std::cerr << "Failed to prewarm SCRFD detector for slot "
                      << slotIndex << ": " << error.toStdString() << "\n";
            return false;
          }
        }
        if (!slot.context.ensureDetectorBuffers(
                scrfdTensorBytesForSourceSize(probeFrameSize,
                                              tuning.scrfdTargetSize),
                1, &error)) {
          std::cerr << "Failed to prewarm SCRFD detector buffers for slot "
                    << slotIndex << ": " << error.toStdString() << "\n";
          return false;
        }
      } else {
        if (!slot.preprocessor.isInitialized() &&
            !slot.preprocessor.initialize(slot.context.detectorContext(),
                                          &error)) {
          std::cerr << "Failed to prewarm Res10 preprocessor for slot "
                    << slotIndex << ": " << error.toStdString() << "\n";
          return false;
        }
        if (!slot.res10Detector.isInitialized()) {
          ScopedStderrSilencer silence(!options.verbose);
          if (!slot.res10Detector.initialize(slot.context.detectorContext(),
                                             res10ParamPath, res10BinPath,
                                             &error)) {
            std::cerr << "Failed to prewarm Res10 detector for slot "
                      << slotIndex << ": " << error.toStdString() << "\n";
            return false;
          }
        }
        if (!slot.context.ensureDetectorBuffers(
                slot.preprocessor.tensorSpec().byteSize(), 1, &error)) {
          std::cerr << "Failed to prewarm Res10 detector buffers for slot "
                    << slotIndex << ": " << error.toStdString() << "\n";
          return false;
        }
      }
    }
    if (options.verbose) {
      std::cout << "decoder_direct_prewarm_ms=" << prewarmTimer.elapsed()
                << " slots=" << decoderDirectPipelineSlots
                << " workers=" << detectorWorkersActive
                << " frame=" << probeFrameNumber << "\n";
    }
    return true;
  };
  if (!prewarmDecoderDirectResources()) {
    return 2;
  }
  for (int slotIndex = 0; slotIndex < decoderDirectPipelineSlots; ++slotIndex) {
    decoderDirectSlots[slotIndex].handoff.resetResourceStats();
    decoderDirectSlots[slotIndex].preprocessor.resetResourceStats();
  }
  wallStart = std::chrono::steady_clock::now();
  lastProgressAt = wallStart;
  lastRuntimeStatsAt = wallStart - std::chrono::seconds(1);
  lastLiveTelemetryAt = wallStart - std::chrono::seconds(3);
  auto runPreparedDecoderDetection =
      [&](int slotIndex) -> PreparedDecoderDetectionResult {
    PreparedDecoderDetectionResult result;
    PreparedDecoderDetectionSlot &slot = decoderDirectSlots[slotIndex];
    if (!slot.active || slot.workerIndex < 0 ||
        slot.workerIndex >= static_cast<int>(decoderDetectorWorkers.size())) {
      result.error = QStringLiteral("Invalid prepared decoder detection slot.");
      return result;
    }
    QString localError;
    if (options.scrfdDetector) {
      result.detections = finalizePreparedScrfdDecoderFrame(
          &slot.preprocessor, &slot.scrfdDetector, &slot.context, &slot.handoff,
          slot.scrfdLayout, slot.detectionFrameSize, tuning.threshold,
          &result.vulkanDetectMs, &localError);
      result.ncnnStats = slot.scrfdDetector.lastInferenceStats();
    } else {
      result.detections = finalizePreparedRes10DecoderFrame(
          &slot.preprocessor, &slot.res10Detector, &slot.context, &slot.handoff,
          slot.detectionFrameSize, tuning.threshold, &result.vulkanDetectMs,
          &localError);
      result.ncnnStats = slot.res10Detector.lastInferenceStats();
    }
    result.error = localError;
    result.handoffProbe = slot.handoff.lastProbe();
    result.hardwareDirectAttemptReason =
        slot.handoff.lastHardwareDirectAttemptReason();
    result.ok = localError.isEmpty() || !result.detections.isEmpty();
    return result;
  };
  auto startPreparedDecoderDetection = [&](int slotIndex) {
    PreparedDecoderDetectionSlot &slot = decoderDirectSlots[slotIndex];
    slot.detectionRunning = true;
    decoderDetectorWorkers[slot.workerIndex]->busy = true;
    if (detectorWorkersActive > 1) {
      slot.detectionFuture = std::async(std::launch::async, [&, slotIndex]() {
        return runPreparedDecoderDetection(slotIndex);
      });
    } else {
      std::promise<PreparedDecoderDetectionResult> promise;
      promise.set_value(runPreparedDecoderDetection(slotIndex));
      slot.detectionFuture = promise.get_future();
    }
  };
  auto finalizePreparedDecoderSlot = [&](int slotIndex) -> bool {
    PreparedDecoderDetectionSlot &slot = decoderDirectSlots[slotIndex];
    if (!slot.active) {
      return true;
    }
    if (!slot.detectionRunning) {
      startPreparedDecoderDetection(slotIndex);
    }
    PreparedDecoderDetectionResult result = slot.detectionFuture.get();
    slot.detectionRunning = false;
    if (slot.workerIndex >= 0 &&
        slot.workerIndex < static_cast<int>(decoderDetectorWorkers.size())) {
      decoderDetectorWorkers[slot.workerIndex]->busy = false;
    }
    QVector<Detection> detections = result.detections;
    if (slot.decoderVulkanUploadFallback) {
      ++decoderVulkanUploadFallbackFrames;
    }
    if (!slot.hardwareDirectHandoff && !options.allowCpuUploadFallback) {
      const QString reason =
          result.hardwareDirectAttemptReason.isEmpty()
              ? (result.error.isEmpty()
                     ? QStringLiteral(
                           "No supported hardware-direct decoder-to-Vulkan "
                           "handoff path is available.")
                     : result.error)
              : result.hardwareDirectAttemptReason;
      lastHardwareDirectAttemptReason = reason;
      std::cerr << "CPU upload fallback requires explicit approval via "
                   "--allow-cpu-upload-fallback. "
                << "Hardware-direct reason: " << reason.toStdString() << "\n";
      slot.active = false;
      return false;
    }
    if (slot.hardwareDirectHandoff) {
      ++hardwareDirectHandoffFrames;
    }
    if (options.requireHardwareVulkanFramePath && !slot.hardwareDirectHandoff) {
      std::cerr << "Hardware Vulkan frame path required, but hardware-direct "
                   "handoff was not achieved at frame "
                << slot.frameNumber << ". Reason: "
                << result.hardwareDirectAttemptReason.toStdString() << "\n";
      slot.active = false;
      return false;
    }
    if (!result.error.isEmpty() && detections.isEmpty()) {
      lastHardwareDirectAttemptReason = result.hardwareDirectAttemptReason;
      std::cerr << "Decoder Vulkan handoff detection failed at frame "
                << slot.frameNumber << ": " << result.error.toStdString()
                << "\n";
      slot.active = false;
      return false;
    }
    lastHardwareDirectAttemptReason = result.hardwareDirectAttemptReason;
    const QVector<Detection> rawDetections = detections;
    DetectionSanitizeStats sanitizeStats;
    detections =
        sanitizeDetections(detections, slot.detectionFrameSize,
                           tuning.maxFacesPerFrame, tuning, &sanitizeStats);
    if (detections.isEmpty()) {
      logDetectionSanitizeStats(slot.frameNumber, slot.detectionFrameSize,
                                tuning, sanitizeStats, rawDetections);
    }
    const QVector<Track> detectionPreviewTracks = advanceRuntimeTracks(
        slot.frameNumber, detections, slot.detectionFrameSize);
    enqueueLivePreviewSample(
        slot.frameNumber, detectionPreviewTracks, detections,
        QStringLiteral("JCut DNN FaceDetections Generator"));
    if (!requireSynchronizedPreview(
            slot.frameNumber, slot.decodedFrame, detectionPreviewTracks,
            detections,
            QStringLiteral("JCut DNN FaceDetections Generator (Decoder Vulkan "
                           "Upload Fallback)"),
            [&]() -> QImage {
              return jcut::facedetections::renderFrameWithVulkan(
                  &appFrameProvider, sourceClip, options.videoPath,
                  slot.frameNumber, slot.frameNumber, renderSize, nullptr);
            })) {
      slot.active = false;
      return false;
    }
    ++decoderDirectHandoffFrames;
    const jcut::facedetections::VulkanFrameStats emptyRenderStats;
    const bool recorded = recordProcessedFrame(
        slot.frameOffset, slot.frameNumber, emptyRenderStats, detections,
        detectionPreviewTracks, slot.detectionFrameSize, false,
        slot.decoderVulkanUploadFallback, slot.hardwareDirectHandoff,
        slot.decoderUploadMs, result.vulkanDetectMs, result.ncnnStats,
        result.handoffProbe, result.hardwareDirectAttemptReason);
    slot.active = false;
    return recorded;
  };
  auto prepareDecoderDirectSlot = [&](int frameOffset, int frameNumber,
                                      int workerIndex) -> bool {
    PreparedDecoderDetectionSlot &slot =
        decoderDirectSlots[nextDecoderDirectSlot];
    if (slot.active) {
      std::cerr << "Decoder direct pipeline slot exhaustion at frame "
                << frameNumber << ".\n";
      return false;
    }
    QElapsedTimer decodeTimer;
    decodeTimer.start();
    slot.decodedFrame = decoder.decodeFrame(frameNumber);
    stageTimings.decodeMsTotal +=
        static_cast<double>(decodeTimer.nsecsElapsed()) / 1'000'000.0;
    ++stageTimings.decodeSamples;
    if (slot.decodedFrame.isNull()) {
      return false;
    }
    if (slot.decodedFrame.hasHardwareFrame() ||
        slot.decodedFrame.hasGpuTexture()) {
      ++hardwareFrames;
    }
    if (slot.decodedFrame.hasCpuImage()) {
      ++cpuFrames;
    }
    if (detectorWorkersActive > 1) {
      if (slot.context.device == VK_NULL_HANDLE &&
          !slot.context.initialize(&error)) {
        std::cerr << "Failed to initialize independent Vulkan detector context "
                     "for worker "
                  << workerIndex << " at frame " << frameNumber << ": "
                  << error.toStdString() << "\n";
        return false;
      }
    } else if (detectorContext.device == VK_NULL_HANDLE) {
      if (!detectorContext.initialize(&error)) {
        std::cerr << "Failed to initialize shared Vulkan detector context for "
                     "decoder handoff at frame "
                  << frameNumber << ": " << error.toStdString() << "\n";
        return false;
      }
    }
    if (detectorWorkersActive <= 1) {
      if (!slot.context.attachExternalDevice(detectorContext.detectorContext(),
                                             &error)) {
        std::cerr << "Failed to attach decoder pipeline slot to shared Vulkan "
                     "detector context at frame "
                  << frameNumber << ": " << error.toStdString() << "\n";
        return false;
      }
    }
    if (!slot.handoff.isInitialized()) {
      if (!slot.handoff.initialize(slot.context.detectorContext(), &error)) {
        std::cerr
            << "Failed to initialize decoder frame handoff module at frame "
            << frameNumber << ": " << error.toStdString() << "\n";
        return false;
      }
    }
    if (slot.decodedFrame.hasHardwareFrame()) {
      const auto probe = slot.handoff.probeHardwareInterop(slot.decodedFrame);
      lastHardwareInteropProbeReason = probe.reason;
      lastHardwareInteropProbePath = probe.path;
      if (probe.supported) {
        ++hardwareInteropProbeSupportedFrames;
      } else {
        ++hardwareInteropProbeFailedFrames;
      }
      if (options.requireHardwareVulkanFramePath && !probe.supported) {
        std::cerr << "Hardware direct Vulkan handoff probe failed at frame "
                  << frameNumber << ": " << probe.reason.toStdString() << "\n";
        return false;
      }
    }
    slot.frameNumber = frameNumber;
    slot.frameOffset = frameOffset;
    slot.detectionFrameSize = slot.decodedFrame.size().isValid()
                                  ? slot.decodedFrame.size()
                                  : renderSize;
    slot.decoderUploadMs = 0.0;
    slot.workerIndex = workerIndex;
    slot.hardwareDirectHandoff = false;
    slot.decoderVulkanUploadFallback = false;
    slot.detectionRunning = false;
    slot.scrfdLayout = {};
    error.clear();
    bool prepared = false;
    QElapsedTimer handoffTimer;
    handoffTimer.start();
    if (options.scrfdDetector) {
      prepared = prepareScrfdDecoderFrame(
          &slot.preprocessor, &slot.scrfdDetector, &slot.context, &slot.handoff,
          slot.decodedFrame, scrfdParamPath, scrfdBinPath,
          tuning.scrfdTargetSize, !options.verbose,
          options.allowCpuUploadFallback, &slot.scrfdLayout,
          &slot.decoderUploadMs, &slot.hardwareDirectHandoff,
          &slot.detectionFrameSize, &error);
    } else {
      prepared = prepareRes10DecoderFrame(
          &slot.preprocessor, &slot.res10Detector, &slot.context, &slot.handoff,
          slot.decodedFrame, res10ParamPath, res10BinPath, !options.verbose,
          options.allowCpuUploadFallback, &slot.decoderUploadMs,
          &slot.hardwareDirectHandoff, &slot.detectionFrameSize, &error);
    }
    stageTimings.handoffMsTotal +=
        static_cast<double>(handoffTimer.nsecsElapsed()) / 1'000'000.0;
    ++stageTimings.handoffSamples;
    if (!prepared) {
      if (!error.isEmpty()) {
        std::cerr << "Decoder Vulkan handoff preparation failed at frame "
                  << frameNumber << ": " << error.toStdString() << "\n";
      }
      return false;
    }
    slot.decoderVulkanUploadFallback = slot.handoff.usedCpuUpload();
    slot.active = true;
    pendingDecoderDirectSlots.push_back(nextDecoderDirectSlot);
    startPreparedDecoderDetection(nextDecoderDirectSlot);
    nextDecoderDirectSlot =
        (nextDecoderDirectSlot + 1) % decoderDirectPipelineSlots;
    return true;
  };
  auto freeDecoderWorkerIndex = [&]() -> int {
    for (int i = 0; i < static_cast<int>(decoderDetectorWorkers.size()); ++i) {
      if (!decoderDetectorWorkers[i]->busy) {
        return i;
      }
    }
    return -1;
  };
  auto finalizeOldestPendingDecoderSlot = [&]() -> bool {
    if (pendingDecoderDirectSlots.empty()) {
      return true;
    }
    const int slotIndex = pendingDecoderDirectSlots.front();
    pendingDecoderDirectSlots.pop_front();
    return finalizePreparedDecoderSlot(slotIndex);
  };

  for (int frameOffset = resumeStartOffset; frameOffset < totalFrames;
       ++frameOffset) {
    const int frameNumber = options.startFrame + frameOffset;
    if (resume.completedFrames.contains(frameNumber)) {
      ++decoded;
      const bool previewFrameDue =
          !previewPipelinePrimed ||
          ((frameNumber % effectivePreviewStride) == 0);
      if (blockingPreviewRequested && previewFrameDue) {
        const QVector<Track> &resumedTracks = runtimeTracks;
        const QVector<Detection> resumedDetections;
        if (preferDecoderDirectDetection) {
          const editor::FrameHandle resumedDecodedFrame =
              decoder.decodeFrame(frameNumber);
          if (resumedDecodedFrame.isNull() ||
              !requireSynchronizedPreview(
                  frameNumber, resumedDecodedFrame, resumedTracks,
                  resumedDetections,
                  QStringLiteral("JCut DNN FaceDetections Generator (Resume)"),
                  [&]() -> QImage {
                    const editor::FrameHandle imageFrame =
                        decoder.decodeFrame(frameNumber);
                    return imageFrame.hasCpuImage() ? imageFrame.cpuImage()
                                                    : QImage();
                  })) {
            std::cerr << "Preview synchronization failed while advancing "
                         "resumed frame "
                      << frameNumber << ".\n";
            return 2;
          }
        } else {
          render_detail::OffscreenVulkanFrame resumedFrame;
          jcut::facedetections::VulkanFrameStats resumedStats;
          QString resumedError;
          if (!jcut::facedetections::renderFrameToVulkan(
                  &appFrameProvider, sourceClip, options.videoPath, frameNumber,
                  frameNumber, renderSize, &resumedFrame, &resumedStats,
                  &resumedError) ||
              !requireSynchronizedOffscreenPreview(
                  frameNumber, resumedFrame, resumedTracks, resumedDetections,
                  QStringLiteral("JCut DNN FaceDetections Generator (Resume)"),
                  [&]() -> QImage {
                    return jcut::facedetections::renderFrameWithVulkan(
                        &appFrameProvider, sourceClip, options.videoPath,
                        frameNumber, frameNumber, renderSize, nullptr);
                  })) {
            std::cerr << "Preview synchronization failed while advancing "
                         "resumed frame "
                      << frameNumber << ". " << resumedError.toStdString()
                      << "\n";
            return 2;
          }
        }
      }
      refreshRuntimeStats(frameOffset, frameNumber, 0);
      if (options.progress &&
          shouldRenderProgress(frameOffset, totalFrames, processed,
                               &lastProgressPercent, &lastProgressAt)) {
        const auto now = std::chrono::steady_clock::now();
        const double elapsedSec =
            std::chrono::duration<double>(now - wallStart).count();
        renderProgressLine(
            frameOffset, totalFrames, frameNumber, processed, totalDetections,
            0, elapsedSec,
            processed > 0 ? vulkanDetectMsTotal / processed : 0.0, &etaTracker);
      }
      continue;
    }
    if (livePreviewWindow || detectorControls.window) {
      appPtr->processEvents();
      if (detectorControls.window && !detectorControls.window->isVisible()) {
        detectorControls.window = nullptr;
      }
      if (livePreviewWindow) {
        livePreviewWindow->setTimelineRange(
            options.startFrame, finalFrame,
            qMax(options.startFrame, latestProcessedFrame));
        livePreviewWindow->setProcessingPaused(runtimePaused);
        syncDetectorPreviewPanel(
            &detectorControls, livePreviewWindow.get(), options.startFrame,
            qMax(options.startFrame, latestProcessedFrame));
        livePreviewWindow->pumpEvents();
        runtimePaused = livePreviewWindow->processingPausedRequested();
        drainLivePreviewQueue();
      }
    }
    while (runtimePaused) {
      if (livePreviewWindow) {
        setPreviewStatusText(
            QStringLiteral("FaceDetections detection paused. Resume from the "
                           "runtime controls to continue."));
        livePreviewWindow->setTimelineRange(
            options.startFrame, finalFrame,
            qMax(options.startFrame, latestProcessedFrame));
        livePreviewWindow->setProcessingPaused(true);
        syncDetectorPreviewPanel(
            &detectorControls, livePreviewWindow.get(), options.startFrame,
            qMax(options.startFrame, latestProcessedFrame));
      }
      appPtr->processEvents(QEventLoop::AllEvents, 16);
      if (livePreviewWindow) {
        livePreviewWindow->pumpEvents();
        syncDetectorPreviewPanel(
            &detectorControls, livePreviewWindow.get(), options.startFrame,
            qMax(options.startFrame, latestProcessedFrame));
        runtimePaused = livePreviewWindow->processingPausedRequested();
        const int previewFrame = livePreviewWindow->requestedPreviewFrame();
        if (previewFrame >= options.startFrame &&
            previewFrame <= latestProcessedFrame &&
            (livePreviewWindow->lastPresentedSourceFrame() != previewFrame ||
             livePreviewWindow->previewRefreshRequested())) {
          livePreviewQueue.clear();
          renderAndPresentLivePreviewWindow(
              previewFrame, tracksForFrame(previewFrame),
              detectionsForFrame(previewFrame),
              QStringLiteral(
                  "JCut DNN FaceDetections Generator (History Preview)"));
        }
      }
      if (detectorControls.window && !detectorControls.window->isVisible()) {
        detectorControls.window = nullptr;
        runtimePaused = false;
        break;
      }
      usleep(16 * 1000);
    }
    syncSourceClipGrading();
    if (!options.paramsFile.isEmpty()) {
      QFileInfo paramsInfo(options.paramsFile);
      if (applyRuntimeParamsFile(options.paramsFile, paramsInfo, &tuning,
                                 nullptr, &paramsMtime)) {
        tuning.scrfdModelVariant = options.scrfdModelVariant;
        syncDetectorControlPanel(&detectorControls, tuning);
        if (options.verbose) {
          std::cout << "runtime_params" << " scrfd_model_variant="
                    << tuning.scrfdModelVariant.toStdString()
                    << " stride=" << tuning.stride
                    << " threshold=" << tuning.threshold
                    << " max_detections=" << tuning.maxDetections
                    << " max_faces_per_frame=" << tuning.maxFacesPerFrame
                    << " nms_iou_threshold=" << tuning.nmsIouThreshold
                    << " track_match_iou_threshold="
                    << tuning.trackMatchIouThreshold
                    << " new_track_min_confidence="
                    << tuning.newTrackMinConfidence
                    << " primary_face_only=" << (tuning.primaryFaceOnly ? 1 : 0)
                    << " small_face_fallback="
                    << (tuning.smallFaceFallback ? 1 : 0) << "\n";
        }
      }
    }
    QFileInfo liveSettingsInfo(detectorSettingsPath);
    if (applyRuntimeParamsFile(detectorSettingsPath, liveSettingsInfo, &tuning,
                               &previewDebugSettings, &detectorSettingsMtime)) {
      tuning.scrfdModelVariant = options.scrfdModelVariant;
      syncDetectorControlPanel(&detectorControls, tuning);
      if (livePreviewWindow) {
        livePreviewWindow->setFollowLatest(previewDebugSettings.followLatest);
        livePreviewWindow->setPreviewPlaybackSpeed(
            previewDebugSettings.playbackSpeed);
        livePreviewWindow->setShowDetections(
            previewDebugSettings.showDetections);
        livePreviewWindow->setShowTracks(previewDebugSettings.showTracks);
        livePreviewWindow->setShowLabels(previewDebugSettings.showLabels);
        livePreviewWindow->setShowConfirmedTracks(
            previewDebugSettings.showConfirmedTracks);
        livePreviewWindow->setShowTentativeTracks(
            previewDebugSettings.showTentativeTracks);
        livePreviewWindow->setShowLostTracks(
            previewDebugSettings.showLostTracks);
        livePreviewWindow->setDetectionLineThickness(
            previewDebugSettings.detectionLineThickness);
        livePreviewWindow->setTrackLineThickness(
            previewDebugSettings.trackLineThickness);
        livePreviewWindow->setOverlayOpacity(
            previewDebugSettings.overlayOpacity);
      }
      if (options.verbose) {
        std::cout << "detector_settings" << " scrfd_model_variant="
                  << tuning.scrfdModelVariant.toStdString()
                  << " threshold=" << tuning.threshold
                  << " max_faces_per_frame=" << tuning.maxFacesPerFrame
                  << " primary_face_only=" << (tuning.primaryFaceOnly ? 1 : 0)
                  << "\n";
      }
    }
    ++decoded;
    if ((frameNumber % tuning.stride) != 0) {
      refreshRuntimeStats(frameOffset, frameNumber, 0);
      continue;
    }

    jcut::facedetections::VulkanFrameStats renderStats;
    QVector<Detection> detections;
    QSize detectionFrameSize = renderSize;
    bool appVulkanFrame = false;
    bool decoderVulkanUploadFallback = false;
    bool hardwareDirectHandoff = false;
    double vulkanDetectMs = 0.0;
    double decoderUploadMs = 0.0;

    if (zeroCopyVulkanDetector) {
      auto processDecoderFrameDirectly = [&]() -> bool {
        QElapsedTimer decodeTimer;
        decodeTimer.start();
        editor::FrameHandle decodedFrame = decoder.decodeFrame(frameNumber);
        stageTimings.decodeMsTotal +=
            static_cast<double>(decodeTimer.nsecsElapsed()) / 1'000'000.0;
        ++stageTimings.decodeSamples;
        if (decodedFrame.isNull()) {
          return false;
        }
        if (decodedFrame.hasHardwareFrame() || decodedFrame.hasGpuTexture()) {
          ++hardwareFrames;
        }
        if (decodedFrame.hasCpuImage()) {
          ++cpuFrames;
        }
        if (detectorContext.device == VK_NULL_HANDLE) {
          if (!detectorContext.initialize(&error)) {
            std::cerr << "Failed to initialize Vulkan detector context for "
                         "decoder handoff at frame "
                      << frameNumber << ": " << error.toStdString() << "\n";
            return false;
          }
        }
        if (!decoderFrameHandoff.isInitialized()) {
          if (!decoderFrameHandoff.initialize(detectorContext.detectorContext(),
                                              &error)) {
            std::cerr
                << "Failed to initialize decoder frame handoff module at frame "
                << frameNumber << ": " << error.toStdString() << "\n";
            return false;
          }
        }
        if (decodedFrame.hasHardwareFrame()) {
          const auto probe =
              decoderFrameHandoff.probeHardwareInterop(decodedFrame);
          lastHardwareInteropProbeReason = probe.reason;
          lastHardwareInteropProbePath = probe.path;
          if (probe.supported) {
            ++hardwareInteropProbeSupportedFrames;
          } else {
            ++hardwareInteropProbeFailedFrames;
          }
          if (options.requireHardwareVulkanFramePath && !probe.supported) {
            std::cerr << "Hardware direct Vulkan handoff probe failed at frame "
                      << frameNumber << ": " << probe.reason.toStdString()
                      << "\n";
            return false;
          }
        }
        detectionFrameSize =
            decodedFrame.size().isValid() ? decodedFrame.size() : renderSize;
        error.clear();
        if (options.heuristicZeroCopyDetector) {
          error = QStringLiteral("Decoder fallback handoff is implemented for "
                                 "the Res10 Vulkan path only.");
          detections.clear();
        } else if (options.scrfdDetector) {
          detections = detectScrfdFromDecoderFrame(
              &zeroCopyDetector, &scrfdDetector, &detectorContext,
              &decoderFrameHandoff, decodedFrame, scrfdParamPath, scrfdBinPath,
              tuning.threshold, tuning.scrfdTargetSize, tuning.scrfdTiled,
              !options.verbose, options.allowCpuUploadFallback,
              &decoderUploadMs, &vulkanDetectMs, &hardwareDirectHandoff,
              &error);
        } else {
          detections = detectRes10FromDecoderFrame(
              &zeroCopyDetector, &res10Detector, &detectorContext,
              &decoderFrameHandoff, decodedFrame, res10ParamPath, res10BinPath,
              tuning.threshold, options.allowCpuUploadFallback,
              &decoderUploadMs, &vulkanDetectMs, &hardwareDirectHandoff,
              &error);
        }
        decoderVulkanUploadFallback = decoderFrameHandoff.usedCpuUpload();
        if (decoderVulkanUploadFallback) {
          ++decoderVulkanUploadFallbackFrames;
        }
        if (!hardwareDirectHandoff && !options.allowCpuUploadFallback) {
          const QString reason =
              decoderFrameHandoff.lastHardwareDirectAttemptReason().isEmpty()
                  ? (error.isEmpty()
                         ? QStringLiteral(
                               "No supported hardware-direct decoder-to-Vulkan "
                               "handoff path is available.")
                         : error)
                  : decoderFrameHandoff.lastHardwareDirectAttemptReason();
          lastHardwareDirectAttemptReason = reason;
          std::cerr << "CPU upload fallback requires explicit approval via "
                       "--allow-cpu-upload-fallback. "
                    << "Hardware-direct reason: " << reason.toStdString()
                    << "\n";
          return false;
        }
        decoderUploadMsTotal += decoderUploadMs;
        if (hardwareDirectHandoff) {
          ++hardwareDirectHandoffFrames;
        }
        if (options.requireHardwareVulkanFramePath && !hardwareDirectHandoff) {
          std::cerr << "Hardware Vulkan frame path required, but "
                       "hardware-direct handoff was not achieved at frame "
                    << frameNumber << ". Reason: "
                    << decoderFrameHandoff.lastHardwareDirectAttemptReason()
                           .toStdString()
                    << "\n";
          return false;
        }
        if (!error.isEmpty() && detections.isEmpty()) {
          lastHardwareDirectAttemptReason =
              decoderFrameHandoff.lastHardwareDirectAttemptReason();
          std::cerr << "Decoder Vulkan handoff detection failed at frame "
                    << frameNumber << ": " << error.toStdString() << "\n";
          return false;
        }
        lastHardwareDirectAttemptReason =
            decoderFrameHandoff.lastHardwareDirectAttemptReason();
        const QVector<Detection> rawDetections = detections;
        DetectionSanitizeStats sanitizeStats;
        detections =
            sanitizeDetections(detections, detectionFrameSize,
                               tuning.maxFacesPerFrame, tuning, &sanitizeStats);
        if (detections.isEmpty()) {
          logDetectionSanitizeStats(frameNumber, detectionFrameSize, tuning,
                                    sanitizeStats, rawDetections);
        }
        const QVector<Track> detectionPreviewTracks =
            advanceRuntimeTracks(frameNumber, detections, detectionFrameSize);
        enqueueLivePreviewSample(
            frameNumber, detectionPreviewTracks, detections,
            QStringLiteral("JCut DNN FaceDetections Generator"));
        if (!requireSynchronizedPreview(
                frameNumber, decodedFrame, detectionPreviewTracks, detections,
                QStringLiteral("JCut DNN FaceDetections Generator (Decoder "
                               "Vulkan Upload Fallback)"),
                [&]() -> QImage {
                  return jcut::facedetections::renderFrameWithVulkan(
                      &appFrameProvider, sourceClip, options.videoPath,
                      frameNumber, frameNumber, renderSize, nullptr);
                })) {
          return false;
        }
        return true;
      };

      render_detail::OffscreenVulkanFrame vulkanFrame;
      error.clear();
      bool frameProcessed = false;
      if (preferDecoderDirectDetection) {
        if (decoderDirectPipelineEnabled()) {
          while (pendingDecoderDirectSlots.size() >=
                     static_cast<size_t>(decoderDirectPipelineSlots) ||
                 freeDecoderWorkerIndex() < 0) {
            if (!finalizeOldestPendingDecoderSlot()) {
              continue;
            }
            drainLivePreviewQueue();
          }
          const int workerIndex = freeDecoderWorkerIndex();
          if (workerIndex < 0 || !prepareDecoderDirectSlot(
                                     frameOffset, frameNumber, workerIndex)) {
            continue;
          }
          if (pendingDecoderDirectSlots.size() <
                  static_cast<size_t>(decoderDirectPipelineSlots) &&
              freeDecoderWorkerIndex() >= 0) {
            refreshRuntimeStats(frameOffset, frameNumber, 0);
            continue;
          }
          if (!finalizeOldestPendingDecoderSlot()) {
            continue;
          }
          drainLivePreviewQueue();
          continue;
        }
        frameProcessed = processDecoderFrameDirectly();
        if (frameProcessed) {
          ++decoderDirectHandoffFrames;
        } else {
          continue;
        }
      } else if (!([&]() {
                   const bool synchronizedPreviewFrameNeeded =
                       previewRequiresSynchronizedDetection &&
                       (!previewPipelinePrimed ||
                        ((frameNumber % effectivePreviewStride) == 0));
                   Q_UNUSED(synchronizedPreviewFrameNeeded);
                   return jcut::facedetections::renderFrameToVulkan(
                       &appFrameProvider, sourceClip, options.videoPath,
                       frameNumber, frameNumber, renderSize, &vulkanFrame,
                       &renderStats, &error);
                 })()) {
        if (!printedAppVulkanFailure) {
          std::cerr << "Application Vulkan frame path unavailable: "
                    << error.toStdString() << "\n";
          printedAppVulkanFailure = true;
        }
        return 2;
      } else {
        appVulkanFrame = true;
        ++appVulkanFramePathFrames;
        ++hardwareFrames;
        detectionFrameSize = vulkanFrame.size;
        error.clear();
        if (options.heuristicZeroCopyDetector) {
          detections = detectVulkanFrame(
              &zeroCopyDetector, &detectorContext, vulkanFrame,
              tuning.maxDetections, tuning.threshold, &vulkanDetectMs, &error);
        } else if (options.scrfdDetector) {
          detections = detectScrfdVulkanFrame(
              &zeroCopyDetector, &scrfdDetector, &detectorContext, vulkanFrame,
              scrfdParamPath, scrfdBinPath, tuning.threshold,
              tuning.scrfdTargetSize, tuning.scrfdTiled, !options.verbose,
              &vulkanDetectMs, &error);
        } else {
          detections = detectRes10VulkanFrame(
              &zeroCopyDetector, &res10Detector, &detectorContext, vulkanFrame,
              res10ParamPath, res10BinPath, tuning.threshold,
              tuning.smallFaceFallback, !options.verbose, &vulkanDetectMs,
              &error);
        }
        if (!error.isEmpty() && detections.isEmpty()) {
          std::cerr << "Vulkan zero-copy detection failed at frame "
                    << frameNumber << ": " << error.toStdString() << "\n";
          return 2;
        }
        const QVector<Detection> rawDetections = detections;
        DetectionSanitizeStats sanitizeStats;
        detections =
            sanitizeDetections(detections, detectionFrameSize,
                               tuning.maxFacesPerFrame, tuning, &sanitizeStats);
        if (detections.isEmpty()) {
          logDetectionSanitizeStats(frameNumber, detectionFrameSize, tuning,
                                    sanitizeStats, rawDetections);
        }
        const QVector<Track> detectionPreviewTracks =
            advanceRuntimeTracks(frameNumber, detections, detectionFrameSize);
        const bool previewFrameDue =
            !previewPipelinePrimed ||
            ((frameNumber % effectivePreviewStride) == 0);
        const bool needsPreviewFrame =
            previewFrameDue &&
            (previewSocketPtr || (options.writePreviewFiles &&
                                  previewWritten < options.previewFrames));
        if (needsPreviewFrame) {
          if (!requireSynchronizedOffscreenPreview(
                  frameNumber, vulkanFrame, detectionPreviewTracks, detections,
                  QStringLiteral("JCut DNN FaceDetections Generator"),
                  [&]() -> QImage {
                    return jcut::facedetections::renderFrameWithVulkan(
                        &appFrameProvider, sourceClip, options.videoPath,
                        frameNumber, frameNumber, renderSize, nullptr);
                  })) {
            return 2;
          }
        }
        enqueueLivePreviewSample(
            frameNumber, detectionPreviewTracks, detections,
            QStringLiteral("JCut DNN FaceDetections Generator"));
      }
    } else {
#if JCUT_HAVE_OPENCV
      render_detail::OffscreenVulkanFrame previewFrame;
      QImage frameImage;
      QString materializedPreviewError;
      const bool materializedRenderOk =
          jcut::facedetections::renderFrameToVulkanWithPreviewImage(
              &appFrameProvider, sourceClip, options.videoPath, frameNumber,
              frameNumber, renderSize, &previewFrame, &frameImage, &renderStats,
              &materializedPreviewError);
      appVulkanFrame =
          materializedRenderOk && previewFrame.valid && !frameImage.isNull();
      if (appVulkanFrame) {
        ++hardwareFrames;
      } else {
        if (appFrameProvider.failed && !printedAppVulkanFailure) {
          std::cerr << "Application Vulkan frame path unavailable: "
                    << appFrameProvider.failureReason.toStdString() << "\n";
          printedAppVulkanFailure = true;
        } else if (!materializedPreviewError.isEmpty() &&
                   !printedAppVulkanFailure) {
          std::cerr << "Application Vulkan frame path unavailable: "
                    << materializedPreviewError.toStdString() << "\n";
          printedAppVulkanFailure = true;
        }
        std::cerr << "FaceDetections generator refuses implicit fallback from "
                     "app Vulkan render to decoder/CPU frame path.\n";
        return 2;
      }
      if (frameImage.isNull()) {
        std::cerr
            << "Application Vulkan frame path returned a null frame at frame "
            << frameNumber << ".\n";
        return 2;
      }
      detectionFrameSize = frameImage.size();

      const cv::Mat bgr = jcut::facedetections::qImageToBgrMat(frameImage);
      if (options.scrfdDetector) {
        if (!scrfdDetector.isInitialized()) {
          ScopedStderrSilencer silence(!options.verbose);
          if (!scrfdDetector.initialize(scrfdParamPath, scrfdBinPath, true,
                                        &error)) {
            std::cerr << "Failed to initialize SCRFD ncnn detector: "
                      << error.toStdString() << "\n";
            return 2;
          }
        }
        QElapsedTimer timer;
        timer.start();
        const QVector<jcut::vulkan_detector::ScrfdDetection> raw =
            scrfdDetector.inferFromBgr(bgr, tuning.threshold,
                                       tuning.scrfdTargetSize, &error);
        vulkanDetectMs =
            static_cast<double>(timer.nsecsElapsed()) / 1'000'000.0;
        detections.reserve(raw.size());
        for (const auto &det : raw) {
          detections.push_back({det.box, det.confidence});
        }
      } else {
        cv::Mat gray;
        cv::cvtColor(bgr, gray, cv::COLOR_BGR2GRAY);
        cv::equalizeHist(gray, gray);

        const int minSide =
            qMax(1, qMin(frameImage.width(), frameImage.height()));
        const double scaleFactor = 1.08;
        const int baseNeighbors = 5;
        const int minDivisor = 22;
        const double minWeight = 0.2;
        const cv::Size minFace(qMax(18, minSide / minDivisor),
                               qMax(18, minSide / minDivisor));
        const cv::Size maxFace(qMax(minFace.width + 1, (minSide * 3) / 4),
                               qMax(minFace.height + 1, (minSide * 3) / 4));
        std::vector<jcut::facedetections::WeightedDetection> weightedDetections;
        auto runCascade = [&](cv::CascadeClassifier &cascade, int minNeighbors,
                              double weightBias) {
          if (cascade.empty()) {
            return;
          }
          std::vector<cv::Rect> raw;
          std::vector<int> rejectLevels;
          std::vector<double> levelWeights;
          cascade.detectMultiScale(
              gray, raw, rejectLevels, levelWeights, scaleFactor, minNeighbors,
              cv::CASCADE_SCALE_IMAGE, minFace, maxFace, true);
          for (int i = 0; i < static_cast<int>(raw.size()); ++i) {
            const double weight =
                (i < static_cast<int>(levelWeights.size()) ? levelWeights[i]
                                                           : 0.0) +
                weightBias;
            if (weight >= minWeight) {
              weightedDetections.push_back({raw[i], weight});
            }
          }
        };
        runCascade(faceCascade, baseNeighbors, 0.0);
        runCascade(faceCascadeAlt, qMax(2, baseNeighbors - 1), -0.05);
        runCascade(faceCascadeProfile, qMax(2, baseNeighbors - 1), -0.10);

        const std::vector<jcut::facedetections::WeightedDetection> filtered =
            jcut::facedetections::filterAndSuppressDetections(
                std::move(weightedDetections), frameImage.size());
        detections.reserve(static_cast<int>(filtered.size()));
        for (const jcut::facedetections::WeightedDetection &det : filtered) {
          detections.push_back(
              {QRectF(det.box.x, det.box.y, det.box.width, det.box.height),
               static_cast<float>(det.weight)});
        }
      }
      const QVector<Detection> rawDetections = detections;
      DetectionSanitizeStats sanitizeStats;
      detections =
          sanitizeDetections(detections, detectionFrameSize,
                             tuning.maxFacesPerFrame, tuning, &sanitizeStats);
      if (detections.isEmpty()) {
        logDetectionSanitizeStats(frameNumber, detectionFrameSize, tuning,
                                  sanitizeStats, rawDetections);
      }
      const QVector<Track> detectionPreviewTracks =
          advanceRuntimeTracks(frameNumber, detections, detectionFrameSize);

      const bool previewFrameDue =
          !previewPipelinePrimed ||
          ((frameNumber % effectivePreviewStride) == 0);
      const bool needsPreviewFrame =
          previewFrameDue &&
          (previewSocketPtr || (options.writePreviewFiles &&
                                previewWritten < options.previewFrames));
      if (needsPreviewFrame) {
        if (!requireSynchronizedOffscreenPreview(
                frameNumber, previewFrame, detectionPreviewTracks, detections,
                QStringLiteral("JCut Materialized Generate FaceDetections"),
                [&]() -> QImage { return frameImage; })) {
          return 2;
        }
      }
      enqueueLivePreviewSample(
          frameNumber, detectionPreviewTracks, detections,
          QStringLiteral("JCut Materialized Generate FaceDetections"));
      if (needsPreviewFrame && !previewRequiresSynchronizedDetection) {
        emitPreview(
            frameImage, frameNumber, detectionPreviewTracks, detections,
            QStringLiteral("JCut Materialized Generate FaceDetections"));
      }
#endif
    }
    if (!recordProcessedFrame(
            frameOffset, frameNumber, renderStats, detections, runtimeTracks,
            detectionFrameSize, appVulkanFrame, decoderVulkanUploadFallback,
            hardwareDirectHandoff, decoderUploadMs, vulkanDetectMs,
            emptyNcnnStats, decoderFrameHandoff.lastProbe(),
            decoderFrameHandoff.lastHardwareDirectAttemptReason())) {
      return 2;
    }
    drainLivePreviewQueue();
  }
  while (!pendingDecoderDirectSlots.empty()) {
    if (!finalizeOldestPendingDecoderSlot()) {
      continue;
    }
    drainLivePreviewQueue();
  }
  drainLivePreviewQueue(true);
  if (options.progress) {
    std::cout << "\n";
  }
  refreshRuntimeStats(qMax(0, totalFrames - 1), finalFrame,
                      liveTrackCount(runtimeTracks), true);
  emitLiveStageTelemetry(qMax(0, totalFrames - 1), finalFrame,
                         liveTrackCount(runtimeTracks), true);
  if (faceStreamWriter) {
    QString writerCloseError;
    if (!faceStreamWriter->close(&writerCloseError)) {
      std::cerr << writerCloseError.toStdString() << "\n";
      return 2;
    }
  }
  saveCompactResumeIndex(true);
  if (resume.fullPayloadDeferred) {
    if (livePreviewWindow) {
      setPreviewStatusText(
          QStringLiteral("Finalizing FaceDetections artifacts: replaying "
                         "checkpoint payload..."));
      appPtr->processEvents();
    }
    FaceDetectionsResumeState fullResume;
    QString fullResumeError;
    QElapsedTimer fullReplayTimer;
    fullReplayTimer.start();
    if (!loadFaceDetectionsResume(faceStreamPath, options.videoPath, backend,
                                  options.startFrame, finalFrame, &fullResume,
                                  &fullResumeError)) {
      std::cerr
          << "Failed to replay facedetections checkpoint for final artifacts: "
          << fullResumeError.toStdString() << "\n";
      return 2;
    }
    frameRows = fullResume.frameRows;
    rawDetectionFrames = fullResume.rawDetectionFrames;
    trackDetectionsByFrame = fullResume.trackDetectionsByFrame;
    rawDetectionFrameIndexByFrame.clear();
    for (int i = 0; i < rawDetectionFrames.size(); ++i) {
      const int frameNumber = rawDetectionFrames.at(i)
                                  .toObject()
                                  .value(QStringLiteral("frame"))
                                  .toInt(-1);
      if (frameNumber >= 0) {
        rawDetectionFrameIndexByFrame.insert(frameNumber, i);
      }
    }
    if (options.verbose) {
      std::cout << "final_facedetections_checkpoint_replay_ms="
                << fullReplayTimer.elapsed()
                << " frames=" << rawDetectionFrames.size() << "\n";
    }
  }

  const auto wallEnd = std::chrono::steady_clock::now();
  struct PersistedTrackAccumulator {
    int trackId = -1;
    int firstFrame = std::numeric_limits<int>::max();
    int lastFrame = std::numeric_limits<int>::min();
    int hits = 0;
    int misses = 0;
    QString state;
    QJsonArray detections;
  };
  std::map<int, PersistedTrackAccumulator> persistedTracksById;
  QList<int> processedTrackFrames = trackDetectionsByFrame.keys();
  std::sort(processedTrackFrames.begin(), processedTrackFrames.end());
  for (int frameNumber : processedTrackFrames) {
    const QJsonArray trackRowsForFrame =
        trackDetectionsByFrame.value(frameNumber);
    for (const QJsonValue &trackValue : trackRowsForFrame) {
      const QJsonObject trackObject = trackValue.toObject();
      const int trackId =
          trackObject.value(QStringLiteral("track_id")).toInt(-1);
      if (trackId < 0) {
        continue;
      }
      PersistedTrackAccumulator &accumulator = persistedTracksById[trackId];
      accumulator.trackId = trackId;
      accumulator.firstFrame = qMin(
          accumulator.firstFrame,
          trackObject.value(QStringLiteral("first_frame")).toInt(frameNumber));
      accumulator.lastFrame = qMax(
          accumulator.lastFrame,
          trackObject.value(QStringLiteral("last_frame")).toInt(frameNumber));
      accumulator.hits = qMax(
          accumulator.hits, trackObject.value(QStringLiteral("hits")).toInt(0));
      accumulator.misses =
          trackObject.value(QStringLiteral("misses")).toInt(accumulator.misses);
      accumulator.state = trackObject.value(QStringLiteral("track_state"))
                              .toString(accumulator.state);
      accumulator.detections.append(QJsonObject{
          {QStringLiteral("frame"),
           trackObject.value(QStringLiteral("frame")).toInt(frameNumber)},
          {QStringLiteral("x"),
           trackObject.value(QStringLiteral("x")).toDouble()},
          {QStringLiteral("y"),
           trackObject.value(QStringLiteral("y")).toDouble()},
          {QStringLiteral("box"),
           trackObject.value(QStringLiteral("box")).toDouble()},
          {QStringLiteral("score"),
           trackObject.value(QStringLiteral("score")).toDouble()}});
    }
  }
  QJsonArray trackRows;
  for (const auto &[trackId, accumulator] : persistedTracksById) {
    if (trackId < 0 || accumulator.detections.isEmpty()) {
      continue;
    }
    trackRows.append(
        QJsonObject{{QStringLiteral("track_id"), trackId},
                    {QStringLiteral("first_frame"),
                     accumulator.firstFrame == std::numeric_limits<int>::max()
                         ? -1
                         : accumulator.firstFrame},
                    {QStringLiteral("last_frame"),
                     accumulator.lastFrame == std::numeric_limits<int>::min()
                         ? -1
                         : accumulator.lastFrame},
                    {QStringLiteral("length"), accumulator.detections.size()},
                    {QStringLiteral("hits"), accumulator.hits},
                    {QStringLiteral("misses"), accumulator.misses},
                    {QStringLiteral("state"), accumulator.state},
                    {QStringLiteral("detections"), accumulator.detections}});
  }
  const QDir outputDir(options.outputDir);
  const QString detectionsArtifactPath =
      outputDir.filePath(QStringLiteral("detections.idx"));
  const QString detectionsDataPath =
      outputDir.filePath(QStringLiteral("detections.dat"));
  const QString tracksArtifactPath =
      outputDir.filePath(QStringLiteral("tracks.idx"));
  const QString tracksDataPath =
      outputDir.filePath(QStringLiteral("tracks.dat"));
  const QString continuityArtifactPath =
      outputDir.filePath(QStringLiteral("continuity_facedetections.bin"));
  if (!jcut::facedetections::writeIndexedFrameArtifact(
          detectionsArtifactPath, detectionsDataPath, options.videoPath,
          backend, rawDetectionFrames)) {
    std::cerr << "Failed to write detections artifact: "
              << detectionsArtifactPath.toStdString() << "\n";
    return 2;
  }
  if (!jcut::facedetections::writeIndexedTrackArtifact(
          tracksArtifactPath, tracksDataPath, options.videoPath, backend,
          trackRows, frameRows)) {
    std::cerr << "Failed to write tracks artifact: "
              << tracksArtifactPath.toStdString() << "\n";
    return 2;
  }
  const QJsonObject continuityRoot{
      {QStringLiteral("run_id"), QStringLiteral("offscreen")},
      {QStringLiteral("updated_at_utc"),
       QDateTime::currentDateTimeUtc().toString(Qt::ISODate)},
      {QStringLiteral("only_dialogue"), false},
      {QStringLiteral("scan_start_frame"), 0},
      {QStringLiteral("scan_end_frame"),
       qMax(0, options.startFrame + totalFrames - 1)},
      {QStringLiteral("raw_tracks_artifact_path"), tracksArtifactPath},
      {QStringLiteral("raw_frames_artifact_path"), detectionsArtifactPath},
      {QStringLiteral("continuity_artifact_path"), continuityArtifactPath},
      {QStringLiteral("raw_tracks_count"), trackRows.size()},
      {QStringLiteral("raw_frames_count"), rawDetectionFrames.size()},
      {QStringLiteral("raw_tracks_schema"),
       QStringLiteral("jcut_facedetections_offscreen_tracks_v1")},
      {QStringLiteral("raw_frames_schema"),
       QStringLiteral("jcut_facedetections_offscreen_detections_v1")},
      {QStringLiteral("raw_tracks_frame_domain"),
       QStringLiteral("source_absolute")},
      {QStringLiteral("raw_frames_frame_domain"),
       QStringLiteral("source_absolute")},
      {QStringLiteral("streams_frame_domain"),
       QStringLiteral("source_absolute")},
      {QStringLiteral("detector_mode"), backend}};
  writeBinaryJsonObject(
      continuityArtifactPath,
      QJsonObject{
          {QStringLiteral("schema"), QStringLiteral("jcut_facedetections_v1")},
          {QStringLiteral("continuity_facedetections_by_clip"),
           QJsonObject{{QStringLiteral("facedetections-offscreen-source"),
                        continuityRoot}}}});

  const bool decodeZeroCopy =
      zeroCopyVulkanDetector && !options.materializedGenerateFacestream &&
      decoderDirectHandoffFrames == processed &&
      hardwareDirectHandoffFrames == processed &&
      appVulkanFramePathFrames == 0 && decoderVulkanUploadFallbackFrames == 0 &&
      processed > 0;
  quint64 handoffDescriptorAllocs = 0;
  quint64 handoffDescriptorFrees = 0;
  quint64 handoffImageAllocs = 0;
  quint64 handoffImageFrees = 0;
  quint64 handoffImportedAllocs = 0;
  quint64 handoffImportedFrees = 0;
  quint64 handoffBufferAllocs = 0;
  quint64 handoffBufferFrees = 0;
  quint64 handoffPipelineCreates = 0;
  quint64 preprocDescriptorAllocs = 0;
  quint64 preprocDescriptorFrees = 0;
  quint64 inferDescriptorAllocs = 0;
  quint64 inferDescriptorFrees = 0;
  quint64 preprocPipelineCreates = 0;
  for (int slotIndex = 0; slotIndex < decoderDirectPipelineSlots; ++slotIndex) {
    const auto handoffStats =
        decoderDirectSlots[slotIndex].handoff.resourceStats();
    handoffDescriptorAllocs += handoffStats.descriptorAllocations;
    handoffDescriptorFrees += handoffStats.descriptorFrees;
    handoffImageAllocs += handoffStats.imageMemoryAllocations;
    handoffImageFrees += handoffStats.imageMemoryFrees;
    handoffImportedAllocs += handoffStats.importedMemoryAllocations;
    handoffImportedFrees += handoffStats.importedMemoryFrees;
    handoffBufferAllocs += handoffStats.stagingBufferAllocations;
    handoffBufferFrees += handoffStats.stagingBufferFrees;
    handoffPipelineCreates += handoffStats.computePipelineCreations;
    const auto preprocessorStats =
        decoderDirectSlots[slotIndex].preprocessor.resourceStats();
    preprocDescriptorAllocs +=
        preprocessorStats.preprocessDescriptorAllocations;
    preprocDescriptorFrees += preprocessorStats.preprocessDescriptorFrees;
    inferDescriptorAllocs += preprocessorStats.inferenceDescriptorAllocations;
    inferDescriptorFrees += preprocessorStats.inferenceDescriptorFrees;
    preprocPipelineCreates += preprocessorStats.computePipelineCreations;
  }
  const QJsonObject summary{
      {QStringLiteral("video"), options.videoPath},
      {QStringLiteral("output_dir"), QDir(options.outputDir).absolutePath()},
      {QStringLiteral("generator_name"),
       QStringLiteral("JCut DNN FaceDetections Generator")},
      {QStringLiteral("backend"), backend},
      {QStringLiteral("detector"), backend},
      {QStringLiteral("max_frames"), totalFrames},
      {QStringLiteral("start_frame"), options.startFrame},
      {QStringLiteral("stride"), tuning.stride},
      {QStringLiteral("runtime_threshold"), tuning.threshold},
      {QStringLiteral("runtime_max_detections"), tuning.maxDetections},
      {QStringLiteral("runtime_max_faces_per_frame"), tuning.maxFacesPerFrame},
      {QStringLiteral("runtime_nms_iou_threshold"), tuning.nmsIouThreshold},
      {QStringLiteral("runtime_track_match_iou_threshold"),
       tuning.trackMatchIouThreshold},
      {QStringLiteral("runtime_new_track_min_confidence"),
       tuning.newTrackMinConfidence},
      {QStringLiteral("runtime_primary_face_only"), tuning.primaryFaceOnly},
      {QStringLiteral("runtime_small_face_fallback"), tuning.smallFaceFallback},
      {QStringLiteral("runtime_roi_x1"), tuning.roiX1},
      {QStringLiteral("runtime_roi_y1"), tuning.roiY1},
      {QStringLiteral("runtime_roi_x2"), tuning.roiX2},
      {QStringLiteral("runtime_roi_y2"), tuning.roiY2},
      {QStringLiteral("runtime_min_face_area_ratio"), tuning.minFaceAreaRatio},
      {QStringLiteral("runtime_max_face_area_ratio"), tuning.maxFaceAreaRatio},
      {QStringLiteral("runtime_min_aspect"), tuning.minAspect},
      {QStringLiteral("runtime_max_aspect"), tuning.maxAspect},
      {QStringLiteral("runtime_params_file"), options.paramsFile},
      {QStringLiteral("detector_settings_file"), detectorSettingsPath},
      {QStringLiteral("require_zero_copy"), options.requireZeroCopy},
      {QStringLiteral("require_hardware_vulkan_frame_path"),
       options.requireHardwareVulkanFramePath},
      {QStringLiteral("allow_cpu_upload_fallback"),
       options.allowCpuUploadFallback},
      {QStringLiteral("require_zero_copy_satisfied"),
       !options.requireZeroCopy || decodeZeroCopy},
      {QStringLiteral("require_hardware_vulkan_frame_path_satisfied"),
       !options.requireHardwareVulkanFramePath ||
           decoderVulkanUploadFallbackFrames == 0 ||
           hardwareDirectHandoffFrames > 0},
      {QStringLiteral("decoded_frames"), decoded},
      {QStringLiteral("processed_frames"), processed},
      {QStringLiteral("decoder_direct_pipeline_slots"),
       decoderDirectPipelineSlots},
      {QStringLiteral("detector_workers_requested"), options.detectorWorkers},
      {QStringLiteral("detector_workers_active"), detectorWorkersActive},
      {QStringLiteral("detector_workers_note"),
       detectorWorkersActive > 1
           ? QStringLiteral(
                 "Independent detector workers active; each prepared slot owns "
                 "an independent Vulkan device/queue and detector instance. "
                 "Worker permits bound concurrency while results are consumed "
                 "in frame order.")
           : QStringLiteral("Single detector worker active.")},
      {QStringLiteral("async_checkpoint_writer_enabled"),
       static_cast<bool>(faceStreamWriter)},
      {QStringLiteral("checkpoint_writer_queue_capacity"),
       options.checkpointWriterQueueCapacity},
      {QStringLiteral("checkpoint_writer_current_backlog"),
       faceStreamWriter ? faceStreamWriter->backlog() : 0},
      {QStringLiteral("checkpoint_writer_max_backlog"),
       faceStreamWriter ? faceStreamWriter->maxBacklog() : 0},
      {QStringLiteral("checkpoint_writer_records_queued"),
       faceStreamWriter ? faceStreamWriter->recordsQueued()
                        : faceStreamRecordsSubmitted},
      {QStringLiteral("checkpoint_writer_records_written"),
       faceStreamWriter ? faceStreamWriter->recordsWritten()
                        : faceStreamRecordsWrittenSync},
      {QStringLiteral("checkpoint_writer_backpressure_waits"),
       faceStreamWriter ? faceStreamWriter->backpressureWaits() : 0},
      {QStringLiteral("checkpoint_writer_backpressure_ms"),
       faceStreamWriter ? faceStreamWriter->backpressureMs() : 0.0},
      {QStringLiteral("checkpoint_writer_avg_write_ms"),
       (faceStreamWriter && faceStreamWriter->recordsWritten() > 0)
           ? faceStreamWriter->writeMsTotal() /
                 static_cast<double>(faceStreamWriter->recordsWritten())
           : stageTimings.avgCheckpointWriteMs()},
      {QStringLiteral("sampling_note"),
       QStringLiteral("processed_frames increments only on frames where "
                      "frame_number %% stride == 0")},
      {QStringLiteral("cpu_frames"), cpuFrames},
      {QStringLiteral("hardware_frames"), hardwareFrames},
      {QStringLiteral("app_vulkan_decode_path_frames"),
       appVulkanFramePathFrames},
      {QStringLiteral("decoder_direct_handoff_frames"),
       decoderDirectHandoffFrames},
      {QStringLiteral("app_vulkan_frame_path_failed"), appFrameProvider.failed},
      {QStringLiteral("app_vulkan_frame_path_failure"),
       appFrameProvider.failureReason},
      {QStringLiteral("decoder_vulkan_upload_fallback_frames"),
       decoderVulkanUploadFallbackFrames},
      {QStringLiteral("hardware_direct_handoff_frames"),
       hardwareDirectHandoffFrames},
      {QStringLiteral("hardware_interop_probe_supported_frames"),
       hardwareInteropProbeSupportedFrames},
      {QStringLiteral("hardware_interop_probe_failed_frames"),
       hardwareInteropProbeFailedFrames},
      {QStringLiteral("last_hardware_interop_probe_path"),
       lastHardwareInteropProbePath},
      {QStringLiteral("last_hardware_interop_probe_reason"),
       lastHardwareInteropProbeReason},
      {QStringLiteral("last_hardware_direct_attempt_reason"),
       lastHardwareDirectAttemptReason},
      {QStringLiteral("total_detections"), totalDetections},
      {QStringLiteral("track_count"), trackRows.size()},
      {QStringLiteral("preview_frames_written"), previewWritten},
      {QStringLiteral("preview_stride"), options.previewStride},
      {QStringLiteral("live_preview_enabled"),
       static_cast<bool>(livePreviewWindow)},
      {QStringLiteral("live_preview_on_detection_critical_path"),
       livePreviewDetectorThreadRenders > 0},
      {QStringLiteral("live_preview_detector_thread_app_renders"),
       livePreviewDetectorThreadRenders},
      {QStringLiteral("live_preview_buffer_capacity"),
       kLivePreviewQueueCapacity},
      {QStringLiteral("live_preview_min_present_interval_sec"),
       kLivePreviewMinPresentIntervalSec},
      {QStringLiteral("live_preview_samples_queued"), livePreviewQueued},
      {QStringLiteral("live_preview_samples_presented"), livePreviewPresented},
      {QStringLiteral("live_preview_samples_dropped"), livePreviewDropped},
      {QStringLiteral("live_preview_present_samples"),
       livePreviewPresentSamples},
      {QStringLiteral("live_preview_present_ms_total"),
       livePreviewPresentMsTotal},
      {QStringLiteral("live_preview_avg_present_ms"),
       livePreviewPresentSamples > 0
           ? livePreviewPresentMsTotal /
                 static_cast<double>(livePreviewPresentSamples)
           : 0.0},
      {QStringLiteral("preview_requires_synchronized_detection"),
       previewRequiresSynchronizedDetection},
      {QStringLiteral("scrfd_model_variant"), normalizedScrfdModelVariant},
      {QStringLiteral("avg_render_decode_ms"),
       processed ? renderDecodeMsTotal / processed : 0.0},
      {QStringLiteral("avg_render_composite_ms"),
       processed ? renderCompositeMsTotal / processed : 0.0},
      {QStringLiteral("avg_render_readback_ms"),
       processed ? renderReadbackMsTotal / processed : 0.0},
      {QStringLiteral("avg_decoder_vulkan_upload_ms"),
       processed ? decoderUploadMsTotal / processed : 0.0},
      {QStringLiteral("avg_vulkan_zero_copy_detection_ms"),
       processed ? vulkanDetectMsTotal / processed : 0.0},
      {QStringLiteral("avg_ncnn_input_ms"),
       processed ? ncnnInputMsTotal / processed : 0.0},
      {QStringLiteral("avg_ncnn_extract_ms"),
       processed ? ncnnExtractMsTotal / processed : 0.0},
      {QStringLiteral("avg_ncnn_extract_level8_ms"),
       processed ? ncnnExtractLevel8MsTotal / processed : 0.0},
      {QStringLiteral("avg_ncnn_extract_level16_ms"),
       processed ? ncnnExtractLevel16MsTotal / processed : 0.0},
      {QStringLiteral("avg_ncnn_extract_level32_ms"),
       processed ? ncnnExtractLevel32MsTotal / processed : 0.0},
      {QStringLiteral("avg_ncnn_post_ms"),
       processed ? ncnnPostMsTotal / processed : 0.0},
      {QStringLiteral("avg_ncnn_total_ms"),
       processed ? ncnnTotalMsTotal / processed : 0.0},
      {QStringLiteral("avg_stage_decode_ms"), stageTimings.avgDecodeMs()},
      {QStringLiteral("avg_stage_handoff_prepare_ms"),
       stageTimings.avgHandoffMs()},
      {QStringLiteral("avg_stage_inference_wall_ms"),
       stageTimings.avgInferenceWallMs()},
      {QStringLiteral("avg_stage_tracking_ms"), stageTimings.avgTrackingMs()},
      {QStringLiteral("avg_stage_checkpoint_enqueue_ms"),
       stageTimings.avgCheckpointWriteMs()},
      {QStringLiteral("stage_decode_samples"), stageTimings.decodeSamples},
      {QStringLiteral("stage_handoff_prepare_samples"),
       stageTimings.handoffSamples},
      {QStringLiteral("stage_inference_samples"),
       stageTimings.inferenceSamples},
      {QStringLiteral("stage_tracking_samples"), stageTimings.trackingSamples},
      {QStringLiteral("stage_checkpoint_enqueue_samples"),
       stageTimings.checkpointWriteSamples},
      {QStringLiteral("resource_handoff_descriptor_allocations"),
       static_cast<double>(handoffDescriptorAllocs)},
      {QStringLiteral("resource_handoff_descriptor_frees"),
       static_cast<double>(handoffDescriptorFrees)},
      {QStringLiteral("resource_handoff_image_allocations"),
       static_cast<double>(handoffImageAllocs)},
      {QStringLiteral("resource_handoff_image_frees"),
       static_cast<double>(handoffImageFrees)},
      {QStringLiteral("resource_handoff_imported_allocations"),
       static_cast<double>(handoffImportedAllocs)},
      {QStringLiteral("resource_handoff_imported_frees"),
       static_cast<double>(handoffImportedFrees)},
      {QStringLiteral("resource_handoff_buffer_allocations"),
       static_cast<double>(handoffBufferAllocs)},
      {QStringLiteral("resource_handoff_buffer_frees"),
       static_cast<double>(handoffBufferFrees)},
      {QStringLiteral("resource_handoff_pipeline_creations"),
       static_cast<double>(handoffPipelineCreates)},
      {QStringLiteral("resource_preprocess_descriptor_allocations"),
       static_cast<double>(preprocDescriptorAllocs)},
      {QStringLiteral("resource_preprocess_descriptor_frees"),
       static_cast<double>(preprocDescriptorFrees)},
      {QStringLiteral("resource_inference_descriptor_allocations"),
       static_cast<double>(inferDescriptorAllocs)},
      {QStringLiteral("resource_inference_descriptor_frees"),
       static_cast<double>(inferDescriptorFrees)},
      {QStringLiteral("resource_preprocess_pipeline_creations"),
       static_cast<double>(preprocPipelineCreates)},
      {QStringLiteral("wall_sec"),
       std::chrono::duration<double>(wallEnd - wallStart).count()},
      {QStringLiteral("decode_zero_copy"), decodeZeroCopy},
      {QStringLiteral("qimage_materialized"),
       options.materializedGenerateFacestream},
      {QStringLiteral("vulkan_path_uses_qimage"), false},
      {QStringLiteral("current_mode_uses_qimage"),
       options.materializedGenerateFacestream},
      {QStringLiteral("zero_copy_note"),
       options.materializedGenerateFacestream
           ? QStringLiteral("Materialized compatibility mode: frames are "
                            "rendered by Vulkan, read back to QImage, and "
                            "scanned by the legacy OpenCV continuity detector.")
           : (options.scrfdDetector
                  ? QStringLiteral(
                        "SCRFD ncnn Vulkan zero-copy path: frames are "
                        "preprocessed from Vulkan image to GPU tensor and "
                        "consumed by ncnn Vulkan without QImage/cv::Mat "
                        "detector input materialization.")
                  : (decoderVulkanUploadFallbackFrames > 0
                         ? QStringLiteral(
                               "Partial fallback mode: some frames used "
                               "decoder CPU-image upload into VkImage for "
                               "detector input after app Vulkan frame path "
                               "failures; this is not decode zero-copy.")
                         : (processed == 0
                                ? QStringLiteral("No frames were processed by "
                                                 "the Vulkan detector path.")
                                : QStringLiteral(
                                      "Res10 ncnn Vulkan path: frames remain "
                                      "as Vulkan images through "
                                      "preprocessing/inference; only compact "
                                      "detection metadata is read back. No "
                                      "QImage frame materialization is "
                                      "used."))))}};
  writeJson(QDir(options.outputDir).filePath(QStringLiteral("summary.json")),
            summary);
  std::cout << "summary " << jcut::jsonio::serializeCompact(summary).constData()
            << "\n";
  int resultCode = processed > 0 ? 0 : 1;
  if (options.requireZeroCopy && !decodeZeroCopy) {
    std::cerr << "--require-zero-copy was requested, but the run did not "
                 "complete with end-to-end zero-copy.\n";
    resultCode = 2;
  }
#ifdef JCUT_FACESTREAM_OFFSCREEN_STANDALONE
  if (livePreviewWindow || detectorControls.window) {
    std::cout.flush();
    std::cerr.flush();
    std::fflush(stdout);
    std::fflush(stderr);
    std::_Exit(resultCode);
  }
#endif
  return resultCode;
}

int runVulkanFacestreamOffscreen(const QStringList &args) {
  QVector<QByteArray> argvStorage;
  argvStorage.reserve(args.size() + 1);
  argvStorage.push_back(
      QByteArrayLiteral("jcut_vulkan_facedetections_offscreen"));
  for (const QString &arg : args) {
    argvStorage.push_back(arg.toLocal8Bit());
  }

  QVector<char *> argvPointers;
  argvPointers.reserve(argvStorage.size());
  for (QByteArray &token : argvStorage) {
    argvPointers.push_back(token.data());
  }
  return runVulkanFacestreamOffscreenWithArgv(argvPointers.size(),
                                              argvPointers.data());
}
