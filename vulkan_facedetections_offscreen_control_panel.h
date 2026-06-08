#pragma once

#include "vulkan_facedetections_offscreen_progress.h"
#include "vulkan_facedetections_offscreen_tuning.h"

#include <QString>

#include <memory>

class QCheckBox;
class QComboBox;
class QLabel;
class QProgressBar;
class QPushButton;
class QSlider;
class QWidget;
class ImGuiPreviewWindow;

struct DetectorControlPanel {
  QWidget *window = nullptr;
  QComboBox *profileCombo = nullptr;
  QComboBox *scrfdModelVariant = nullptr;
  QSlider *stride = nullptr;
  QLabel *strideValue = nullptr;
  QSlider *maxDetections = nullptr;
  QLabel *maxDetectionsValue = nullptr;
  QSlider *scrfdTargetSize = nullptr;
  QLabel *scrfdTargetSizeValue = nullptr;
  QSlider *threshold = nullptr;
  QLabel *thresholdValue = nullptr;
  QSlider *nms = nullptr;
  QLabel *nmsValue = nullptr;
  QSlider *trackMatch = nullptr;
  QLabel *trackMatchValue = nullptr;
  QSlider *newTrack = nullptr;
  QLabel *newTrackValue = nullptr;
  QSlider *roiX1 = nullptr;
  QLabel *roiX1Value = nullptr;
  QSlider *roiY1 = nullptr;
  QLabel *roiY1Value = nullptr;
  QSlider *roiX2 = nullptr;
  QLabel *roiX2Value = nullptr;
  QSlider *roiY2 = nullptr;
  QLabel *roiY2Value = nullptr;
  QSlider *minArea = nullptr;
  QLabel *minAreaValue = nullptr;
  QSlider *maxArea = nullptr;
  QLabel *maxAreaValue = nullptr;
  QSlider *minAspect = nullptr;
  QLabel *minAspectValue = nullptr;
  QSlider *maxAspect = nullptr;
  QLabel *maxAspectValue = nullptr;
  QSlider *maxFaces = nullptr;
  QLabel *maxFacesValue = nullptr;
  QCheckBox *primaryFaceOnly = nullptr;
  QCheckBox *smallFaceFallback = nullptr;
  QCheckBox *scrfdTiled = nullptr;
  QCheckBox *applyClipGrading = nullptr;
  QPushButton *pauseButton = nullptr;
  QPushButton *previewPlayButton = nullptr;
  QCheckBox *previewFollowLatest = nullptr;
  QCheckBox *previewShowDetections = nullptr;
  QCheckBox *previewShowTracks = nullptr;
  QCheckBox *previewShowLabels = nullptr;
  QCheckBox *previewShowConfirmed = nullptr;
  QCheckBox *previewShowTentative = nullptr;
  QCheckBox *previewShowLost = nullptr;
  QSlider *previewSeek = nullptr;
  QLabel *previewSeekValue = nullptr;
  QSlider *previewSpeed = nullptr;
  QLabel *previewSpeedValue = nullptr;
  QSlider *previewDetectionLine = nullptr;
  QLabel *previewDetectionLineValue = nullptr;
  QSlider *previewTrackLine = nullptr;
  QLabel *previewTrackLineValue = nullptr;
  QSlider *previewOpacity = nullptr;
  QLabel *previewOpacityValue = nullptr;
  QProgressBar *runtimeProgress = nullptr;
  QLabel *runtimeFrame = nullptr;
  QLabel *runtimeFps = nullptr;
  QLabel *runtimeProcessedFps = nullptr;
  QLabel *runtimeEta = nullptr;
  QLabel *runtimeElapsed = nullptr;
  QLabel *runtimeDetections = nullptr;
  QLabel *runtimeTracks = nullptr;
  QLabel *runtimeDecodeMs = nullptr;
  QLabel *runtimeHandoffMs = nullptr;
  QLabel *runtimeInferenceMs = nullptr;
  QLabel *runtimeTrackingMs = nullptr;
  QLabel *runtimeCheckpointMs = nullptr;
  QLabel *runtimeCheckpointBacklog = nullptr;
  QLabel *runtimeWorkers = nullptr;
  QLabel *runtimePipelineState = nullptr;
  QLabel *runtimeNcnnBreakdown = nullptr;
  QLabel *settingsPath = nullptr;
  std::shared_ptr<bool> syncing = std::make_shared<bool>(false);
};

struct DetectorStageTimingTotals {
  double decodeMsTotal = 0.0;
  double handoffMsTotal = 0.0;
  double inferenceWallMsTotal = 0.0;
  double trackingMsTotal = 0.0;
  double checkpointWriteMsTotal = 0.0;
  int decodeSamples = 0;
  int handoffSamples = 0;
  int inferenceSamples = 0;
  int trackingSamples = 0;
  int checkpointWriteSamples = 0;

  static double average(double total, int samples) {
    return samples > 0 ? total / static_cast<double>(samples) : 0.0;
  }
  double avgDecodeMs() const { return average(decodeMsTotal, decodeSamples); }
  double avgHandoffMs() const {
    return average(handoffMsTotal, handoffSamples);
  }
  double avgInferenceWallMs() const {
    return average(inferenceWallMsTotal, inferenceSamples);
  }
  double avgTrackingMs() const {
    return average(trackingMsTotal, trackingSamples);
  }
  double avgCheckpointWriteMs() const {
    return average(checkpointWriteMsTotal, checkpointWriteSamples);
  }
};

struct DetectorLiveTelemetrySnapshot {
  double avgDecoderUploadMs = 0.0;
  double avgNcnnInputMs = 0.0;
  double avgNcnnExtractMs = 0.0;
  double avgNcnnExtractLevel8Ms = 0.0;
  double avgNcnnExtractLevel16Ms = 0.0;
  double avgNcnnExtractLevel32Ms = 0.0;
  double avgNcnnPostMs = 0.0;
  double avgNcnnTotalMs = 0.0;
  int pendingSlots = 0;
  int busyWorkers = 0;
  int detectorWorkersActive = 0;
  int checkpointBacklog = 0;
  quint64 handoffDescriptorAllocs = 0;
  quint64 handoffDescriptorFrees = 0;
  quint64 handoffImageAllocs = 0;
  quint64 handoffImageFrees = 0;
  quint64 handoffImportedAllocs = 0;
  quint64 handoffImportedFrees = 0;
  quint64 handoffBufferAllocs = 0;
  quint64 handoffBufferFrees = 0;
  quint64 handoffPipelineCreates = 0;
  quint64 handoffCudaSyncCalls = 0;
  double handoffCudaSyncMs = 0.0;
  quint64 handoffCudaSemaphoreSignals = 0;
  double handoffCudaSemaphoreSignalMs = 0.0;
  quint64 preprocDescriptorAllocs = 0;
  quint64 preprocDescriptorFrees = 0;
  quint64 inferDescriptorAllocs = 0;
  quint64 inferDescriptorFrees = 0;
  quint64 preprocPipelineCreates = 0;
};

QString livePipelineStateText(const DetectorLiveTelemetrySnapshot &snapshot);
QString liveNcnnBreakdownText(const DetectorLiveTelemetrySnapshot &snapshot);
QString percentText(float value);
QString areaRatioText(float value);
QString aspectText(float value);
QString millisecondsText(double value);
void syncDetectorControlPanel(DetectorControlPanel *panel,
                              const RuntimeTuning &tuning);
void syncDetectorPreviewPanel(DetectorControlPanel *panel,
                              ImGuiPreviewWindow *previewWindow, int minFrame,
                              int latestProcessedFrame);
void updateDetectorRuntimeStats(
    DetectorControlPanel *panel, int frameOffset, int totalFrames,
    int frameNumber, int processed, int totalDetections, int currentLiveTracks,
    int checkpointBacklog, int detectorWorkersRequested,
    int detectorWorkersActive, int detectorPipelineSlots, double elapsedSec,
    AdaptiveEtaTracker *etaTracker,
    const DetectorStageTimingTotals *stageTimings,
    const DetectorLiveTelemetrySnapshot *liveTelemetry);
DetectorControlPanel createDetectorControlPanel(
    RuntimeTuning *tuning, const QString &detector, int scrfdTargetSize,
    const QString &settingsPath, bool *applyClipGrading,
    bool allowApplyClipGrading, ImGuiPreviewWindow *previewWindow, bool *paused,
    int detectorWorkersRequested, int detectorWorkersActive,
    int detectorPipelineSlots);
