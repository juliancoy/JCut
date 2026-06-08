#include "vulkan_facedetections_offscreen_control_panel.h"

#include "detector_settings.h"
#include "imgui_preview_window.h"
#include "vulkan_facedetections_offscreen_tuning.h"

#include <QCheckBox>
#include <QComboBox>
#include <QDebug>
#include <QFont>
#include <QFormLayout>
#include <QFrame>
#include <QHBoxLayout>
#include <QLabel>
#include <QProgressBar>
#include <QPushButton>
#include <QScrollArea>
#include <QSlider>
#include <QVBoxLayout>
#include <QWidget>

#include <algorithm>
#include <cmath>
#include <functional>

QString livePipelineStateText(const DetectorLiveTelemetrySnapshot &snapshot) {
  return QStringLiteral("%1 pending slot(s), %2/%3 worker(s) busy, backlog %4")
      .arg(qMax(0, snapshot.pendingSlots))
      .arg(qMax(0, snapshot.busyWorkers))
      .arg(qMax(1, snapshot.detectorWorkersActive))
      .arg(qMax(0, snapshot.checkpointBacklog));
}

QString liveNcnnBreakdownText(const DetectorLiveTelemetrySnapshot &snapshot) {
  return QStringLiteral(
             "upload %1 ms | input %2 | ext %3 | l8 %4 | l16 %5 | l32 %6 | "
             "post %7 | total %8 | handoff ds %9/%10 img %11/%12 imp %13/%14 "
             "buf %15/%16 pipe %17 cuda sync %18 ms/%19 sem %20 ms/%21 | "
             "pre ds %22/%23 inf %24/%25 pipe %26")
      .arg(snapshot.avgDecoderUploadMs, 0, 'f', 2)
      .arg(snapshot.avgNcnnInputMs, 0, 'f', 2)
      .arg(snapshot.avgNcnnExtractMs, 0, 'f', 2)
      .arg(snapshot.avgNcnnExtractLevel8Ms, 0, 'f', 2)
      .arg(snapshot.avgNcnnExtractLevel16Ms, 0, 'f', 2)
      .arg(snapshot.avgNcnnExtractLevel32Ms, 0, 'f', 2)
      .arg(snapshot.avgNcnnPostMs, 0, 'f', 2)
      .arg(snapshot.avgNcnnTotalMs, 0, 'f', 2)
      .arg(snapshot.handoffDescriptorAllocs)
      .arg(snapshot.handoffDescriptorFrees)
      .arg(snapshot.handoffImageAllocs)
      .arg(snapshot.handoffImageFrees)
      .arg(snapshot.handoffImportedAllocs)
      .arg(snapshot.handoffImportedFrees)
      .arg(snapshot.handoffBufferAllocs)
      .arg(snapshot.handoffBufferFrees)
      .arg(snapshot.handoffPipelineCreates)
      .arg(snapshot.handoffCudaSyncMs, 0, 'f', 2)
      .arg(snapshot.handoffCudaSyncCalls)
      .arg(snapshot.handoffCudaSemaphoreSignalMs, 0, 'f', 2)
      .arg(snapshot.handoffCudaSemaphoreSignals)
      .arg(snapshot.preprocDescriptorAllocs)
      .arg(snapshot.preprocDescriptorFrees)
      .arg(snapshot.inferDescriptorAllocs)
      .arg(snapshot.inferDescriptorFrees)
      .arg(snapshot.preprocPipelineCreates);
}

QString percentText(float value) {
  return QStringLiteral("%1").arg(static_cast<double>(value), 0, 'f', 2);
}

QString areaRatioText(float value) {
  return QStringLiteral("%1%").arg(static_cast<double>(value * 100.0f), 0, 'f',
                                   3);
}

QString aspectText(float value) {
  return QStringLiteral("%1").arg(static_cast<double>(value), 0, 'f', 2);
}

QString millisecondsText(double value) {
  return QStringLiteral("%1 ms").arg(value, 0, 'f', 2);
}

void syncDetectorControlPanel(DetectorControlPanel *panel,
                              const RuntimeTuning &tuning) {
  if (!panel || !panel->window) {
    return;
  }
  *panel->syncing = true;
  if (panel->scrfdModelVariant) {
    const int index = panel->scrfdModelVariant->findData(
        jcut::facedetections::normalizeScrfdModelVariantId(
            tuning.scrfdModelVariant));
    if (index >= 0) {
      panel->scrfdModelVariant->setCurrentIndex(index);
    }
  }
  if (panel->stride)
    panel->stride->setValue(tuning.stride);
  if (panel->strideValue)
    panel->strideValue->setText(QString::number(tuning.stride));
  if (panel->maxDetections)
    panel->maxDetections->setValue(tuning.maxDetections);
  if (panel->maxDetectionsValue)
    panel->maxDetectionsValue->setText(QString::number(tuning.maxDetections));
  if (panel->scrfdTargetSize)
    panel->scrfdTargetSize->setValue(tuning.scrfdTargetSize);
  if (panel->scrfdTargetSizeValue)
    panel->scrfdTargetSizeValue->setText(
        QString::number(tuning.scrfdTargetSize));
  if (panel->threshold)
    panel->threshold->setValue(qRound(tuning.threshold * 100.0f));
  if (panel->thresholdValue)
    panel->thresholdValue->setText(percentText(tuning.threshold));
  if (panel->nms)
    panel->nms->setValue(qRound(tuning.nmsIouThreshold * 100.0f));
  if (panel->nmsValue)
    panel->nmsValue->setText(percentText(tuning.nmsIouThreshold));
  if (panel->trackMatch)
    panel->trackMatch->setValue(qRound(tuning.trackMatchIouThreshold * 100.0f));
  if (panel->trackMatchValue)
    panel->trackMatchValue->setText(percentText(tuning.trackMatchIouThreshold));
  if (panel->newTrack)
    panel->newTrack->setValue(qRound(tuning.newTrackMinConfidence * 100.0f));
  if (panel->newTrackValue)
    panel->newTrackValue->setText(percentText(tuning.newTrackMinConfidence));
  if (panel->roiX1)
    panel->roiX1->setValue(qRound(tuning.roiX1 * 100.0f));
  if (panel->roiX1Value)
    panel->roiX1Value->setText(percentText(tuning.roiX1));
  if (panel->roiY1)
    panel->roiY1->setValue(qRound(tuning.roiY1 * 100.0f));
  if (panel->roiY1Value)
    panel->roiY1Value->setText(percentText(tuning.roiY1));
  if (panel->roiX2)
    panel->roiX2->setValue(qRound(tuning.roiX2 * 100.0f));
  if (panel->roiX2Value)
    panel->roiX2Value->setText(percentText(tuning.roiX2));
  if (panel->roiY2)
    panel->roiY2->setValue(qRound(tuning.roiY2 * 100.0f));
  if (panel->roiY2Value)
    panel->roiY2Value->setText(percentText(tuning.roiY2));
  if (panel->minArea)
    panel->minArea->setValue(qRound(tuning.minFaceAreaRatio * 100000.0f));
  if (panel->minAreaValue)
    panel->minAreaValue->setText(areaRatioText(tuning.minFaceAreaRatio));
  if (panel->maxArea)
    panel->maxArea->setValue(qRound(tuning.maxFaceAreaRatio * 100000.0f));
  if (panel->maxAreaValue)
    panel->maxAreaValue->setText(areaRatioText(tuning.maxFaceAreaRatio));
  if (panel->minAspect)
    panel->minAspect->setValue(qRound(tuning.minAspect * 100.0f));
  if (panel->minAspectValue)
    panel->minAspectValue->setText(aspectText(tuning.minAspect));
  if (panel->maxAspect)
    panel->maxAspect->setValue(qRound(tuning.maxAspect * 100.0f));
  if (panel->maxAspectValue)
    panel->maxAspectValue->setText(aspectText(tuning.maxAspect));
  if (panel->maxFaces)
    panel->maxFaces->setValue(tuning.maxFacesPerFrame);
  if (panel->maxFacesValue) {
    panel->maxFacesValue->setText(
        tuning.maxFacesPerFrame == 0
            ? QStringLiteral("unlimited")
            : QString::number(tuning.maxFacesPerFrame));
  }
  if (panel->primaryFaceOnly)
    panel->primaryFaceOnly->setChecked(tuning.primaryFaceOnly);
  if (panel->smallFaceFallback)
    panel->smallFaceFallback->setChecked(tuning.smallFaceFallback);
  if (panel->scrfdTiled)
    panel->scrfdTiled->setChecked(tuning.scrfdTiled);
  *panel->syncing = false;
}

void syncDetectorPreviewPanel(DetectorControlPanel *panel,
                              ImGuiPreviewWindow *previewWindow, int minFrame,
                              int latestProcessedFrame) {
  if (!panel || !panel->window || !previewWindow) {
    return;
  }
  *panel->syncing = true;
  if (panel->pauseButton) {
    panel->pauseButton->setText(previewWindow->processingPausedRequested()
                                    ? QStringLiteral("Resume")
                                    : QStringLiteral("Pause"));
  }
  if (panel->previewPlayButton) {
    panel->previewPlayButton->setText(previewWindow->previewPlaybackActive()
                                          ? QStringLiteral("Pause Preview")
                                          : QStringLiteral("Play Preview"));
  }
  if (panel->previewFollowLatest) {
    panel->previewFollowLatest->setChecked(previewWindow->followLatest());
  }
  if (panel->previewShowDetections) {
    panel->previewShowDetections->setChecked(previewWindow->showDetections());
  }
  if (panel->previewShowTracks) {
    panel->previewShowTracks->setChecked(previewWindow->showTracks());
  }
  if (panel->previewShowLabels) {
    panel->previewShowLabels->setChecked(previewWindow->showLabels());
  }
  if (panel->previewShowConfirmed) {
    panel->previewShowConfirmed->setChecked(
        previewWindow->showConfirmedTracks());
  }
  if (panel->previewShowTentative) {
    panel->previewShowTentative->setChecked(
        previewWindow->showTentativeTracks());
  }
  if (panel->previewShowLost) {
    panel->previewShowLost->setChecked(previewWindow->showLostTracks());
  }
  if (panel->previewSeek) {
    panel->previewSeek->setRange(minFrame,
                                 qMax(minFrame, latestProcessedFrame));
    panel->previewSeek->setValue(previewWindow->requestedPreviewFrame());
  }
  if (panel->previewSeekValue) {
    panel->previewSeekValue->setText(
        QString::number(previewWindow->requestedPreviewFrame()));
  }
  if (panel->previewSpeed) {
    panel->previewSpeed->setValue(
        qRound(previewWindow->previewPlaybackSpeed() * 100.0f));
  }
  if (panel->previewSpeedValue) {
    panel->previewSpeedValue->setText(QStringLiteral("%1x").arg(
        previewWindow->previewPlaybackSpeed(), 0, 'f', 2));
  }
  if (panel->previewDetectionLine) {
    panel->previewDetectionLine->setValue(
        qRound(previewWindow->detectionLineThickness() * 10.0f));
  }
  if (panel->previewDetectionLineValue) {
    panel->previewDetectionLineValue->setText(QStringLiteral("%1").arg(
        previewWindow->detectionLineThickness(), 0, 'f', 1));
  }
  if (panel->previewTrackLine) {
    panel->previewTrackLine->setValue(
        qRound(previewWindow->trackLineThickness() * 10.0f));
  }
  if (panel->previewTrackLineValue) {
    panel->previewTrackLineValue->setText(QStringLiteral("%1").arg(
        previewWindow->trackLineThickness(), 0, 'f', 1));
  }
  if (panel->previewOpacity) {
    panel->previewOpacity->setValue(
        qRound(previewWindow->overlayOpacity() * 100.0f));
  }
  if (panel->previewOpacityValue) {
    panel->previewOpacityValue->setText(
        QStringLiteral("%1").arg(previewWindow->overlayOpacity(), 0, 'f', 2));
  }
  *panel->syncing = false;
}

void updateDetectorRuntimeStats(
    DetectorControlPanel *panel, int frameOffset, int totalFrames,
    int frameNumber, int processed, int totalDetections, int currentLiveTracks,
    int checkpointBacklog, int detectorWorkersRequested,
    int detectorWorkersActive, int detectorPipelineSlots, double elapsedSec,
    AdaptiveEtaTracker *etaTracker,
    const DetectorStageTimingTotals *stageTimings,
    const DetectorLiveTelemetrySnapshot *liveTelemetry) {
  if (!panel || !panel->window || !panel->runtimeProgress) {
    return;
  }
  const int current = qBound(0, frameOffset + 1, qMax(1, totalFrames));
  const int progressPercent =
      static_cast<int>(std::round((100.0 * static_cast<double>(current)) /
                                  static_cast<double>(qMax(1, totalFrames))));
  if (etaTracker) {
    etaTracker->observe(current, processed, elapsedSec);
  }
  const AdaptiveEtaEstimate eta =
      etaTracker
          ? etaTracker->estimate(totalFrames, current, processed, elapsedSec)
          : AdaptiveEtaEstimate{};
  const double decodeFps =
      eta.decodeFps > 0.0
          ? eta.decodeFps
          : (elapsedSec > 0.0 ? static_cast<double>(current) / elapsedSec
                              : 0.0);
  const double processFps =
      eta.processedFps > 0.0
          ? eta.processedFps
          : (elapsedSec > 0.0 ? static_cast<double>(processed) / elapsedSec
                              : 0.0);

  panel->runtimeProgress->setValue(qBound(0, progressPercent, 100));
  panel->runtimeFrame->setText(QStringLiteral("%1 / %2 (src frame %3)")
                                   .arg(current)
                                   .arg(totalFrames)
                                   .arg(frameNumber));
  panel->runtimeFps->setText(QStringLiteral("%1").arg(decodeFps, 0, 'f', 2));
  panel->runtimeProcessedFps->setText(
      QStringLiteral("%1").arg(processFps, 0, 'f', 2));
  panel->runtimeEta->setText(formatDuration(eta.etaSec));
  panel->runtimeElapsed->setText(formatDuration(elapsedSec));
  panel->runtimeDetections->setText(QStringLiteral("%1").arg(totalDetections));
  panel->runtimeTracks->setText(QStringLiteral("%1").arg(currentLiveTracks));
  if (panel->runtimeCheckpointBacklog) {
    panel->runtimeCheckpointBacklog->setText(
        QString::number(qMax(0, checkpointBacklog)));
  }
  if (panel->runtimeWorkers) {
    panel->runtimeWorkers->setText(
        QStringLiteral("%1 active / %2 requested, %3 slot(s)")
            .arg(qMax(1, detectorWorkersActive))
            .arg(qMax(1, detectorWorkersRequested))
            .arg(qMax(1, detectorPipelineSlots)));
  }
  if (liveTelemetry) {
    if (panel->runtimePipelineState) {
      panel->runtimePipelineState->setText(
          livePipelineStateText(*liveTelemetry));
    }
    if (panel->runtimeNcnnBreakdown) {
      panel->runtimeNcnnBreakdown->setText(
          liveNcnnBreakdownText(*liveTelemetry));
    }
  }
  if (stageTimings) {
    if (panel->runtimeDecodeMs) {
      panel->runtimeDecodeMs->setText(
          millisecondsText(stageTimings->avgDecodeMs()));
    }
    if (panel->runtimeHandoffMs) {
      panel->runtimeHandoffMs->setText(
          millisecondsText(stageTimings->avgHandoffMs()));
    }
    if (panel->runtimeInferenceMs) {
      panel->runtimeInferenceMs->setText(
          millisecondsText(stageTimings->avgInferenceWallMs()));
    }
    if (panel->runtimeTrackingMs) {
      panel->runtimeTrackingMs->setText(
          millisecondsText(stageTimings->avgTrackingMs()));
    }
    if (panel->runtimeCheckpointMs) {
      panel->runtimeCheckpointMs->setText(
          millisecondsText(stageTimings->avgCheckpointWriteMs()));
    }
  }
}

DetectorControlPanel createDetectorControlPanel(
    RuntimeTuning *tuning, const QString &detector, int scrfdTargetSize,
    const QString &settingsPath, bool *applyClipGrading,
    bool allowApplyClipGrading, ImGuiPreviewWindow *previewWindow, bool *paused,
    int detectorWorkersRequested, int detectorWorkersActive,
    int detectorPipelineSlots) {
  DetectorControlPanel panel;
  panel.window = new QWidget;
  panel.window->setWindowTitle(QStringLiteral("JCut Face Detection"));
  panel.window->setMinimumWidth(420);
  panel.window->resize(520, 760);
  auto *windowLayout = new QVBoxLayout(panel.window);
  windowLayout->setContentsMargins(0, 0, 0, 0);
  auto *scrollArea = new QScrollArea(panel.window);
  scrollArea->setWidgetResizable(true);
  scrollArea->setFrameShape(QFrame::NoFrame);
  scrollArea->setHorizontalScrollBarPolicy(Qt::ScrollBarAsNeeded);
  scrollArea->setVerticalScrollBarPolicy(Qt::ScrollBarAsNeeded);
  auto *content = new QWidget(scrollArea);
  auto *root = new QVBoxLayout(content);
  scrollArea->setWidget(content);
  windowLayout->addWidget(scrollArea);

  auto *statsTitle = new QLabel(QStringLiteral("Detection status"));
  QFont statsFont = statsTitle->font();
  statsFont.setBold(true);
  statsTitle->setFont(statsFont);
  root->addWidget(statsTitle);

  panel.runtimeProgress = new QProgressBar;
  panel.runtimeProgress->setRange(0, 100);
  panel.runtimeProgress->setValue(0);
  panel.runtimeProgress->setFormat(QStringLiteral("%p%"));
  root->addWidget(panel.runtimeProgress);

  auto *statsForm = new QFormLayout;
  panel.runtimeFrame = new QLabel(QStringLiteral("0 / 0"));
  panel.runtimeFps = new QLabel(QStringLiteral("0.00"));
  panel.runtimeProcessedFps = new QLabel(QStringLiteral("0.00"));
  panel.runtimeEta = new QLabel(QStringLiteral("--:--"));
  panel.runtimeElapsed = new QLabel(QStringLiteral("00:00"));
  panel.runtimeDetections = new QLabel(QStringLiteral("0"));
  panel.runtimeTracks = new QLabel(QStringLiteral("0"));
  panel.runtimeDecodeMs = new QLabel(QStringLiteral("0.00 ms"));
  panel.runtimeHandoffMs = new QLabel(QStringLiteral("0.00 ms"));
  panel.runtimeInferenceMs = new QLabel(QStringLiteral("0.00 ms"));
  panel.runtimeTrackingMs = new QLabel(QStringLiteral("0.00 ms"));
  panel.runtimeCheckpointMs = new QLabel(QStringLiteral("0.00 ms"));
  panel.runtimeCheckpointBacklog = new QLabel(QStringLiteral("0"));
  panel.runtimeWorkers =
      new QLabel(QStringLiteral("%1 active / %2 requested, %3 slot(s)")
                     .arg(qMax(1, detectorWorkersActive))
                     .arg(qMax(1, detectorWorkersRequested))
                     .arg(qMax(1, detectorPipelineSlots)));
  panel.runtimePipelineState = new QLabel(
      QStringLiteral("0 pending slot(s), 0/%1 worker(s) busy, backlog 0")
          .arg(qMax(1, detectorWorkersActive)));
  panel.runtimeNcnnBreakdown = new QLabel(
      QStringLiteral("upload 0.00 ms | input 0.00 | ext 0.00 | l8 0.00 | l16 "
                     "0.00 | l32 0.00 | post 0.00 | total 0.00"));
  panel.runtimePipelineState->setWordWrap(true);
  panel.runtimeNcnnBreakdown->setWordWrap(true);
  statsForm->addRow(QStringLiteral("Frame progress"), panel.runtimeFrame);
  statsForm->addRow(QStringLiteral("Workers"), panel.runtimeWorkers);
  statsForm->addRow(QStringLiteral("Pipeline state"),
                    panel.runtimePipelineState);
  statsForm->addRow(QStringLiteral("Decode FPS"), panel.runtimeFps);
  statsForm->addRow(QStringLiteral("Processed FPS"), panel.runtimeProcessedFps);
  statsForm->addRow(QStringLiteral("ETA"), panel.runtimeEta);
  statsForm->addRow(QStringLiteral("Elapsed"), panel.runtimeElapsed);
  statsForm->addRow(QStringLiteral("Detections"), panel.runtimeDetections);
  statsForm->addRow(QStringLiteral("Live tracks"), panel.runtimeTracks);
  statsForm->addRow(QStringLiteral("Avg decode"), panel.runtimeDecodeMs);
  statsForm->addRow(QStringLiteral("Avg handoff/prep"), panel.runtimeHandoffMs);
  statsForm->addRow(QStringLiteral("Avg inference wall"),
                    panel.runtimeInferenceMs);
  statsForm->addRow(QStringLiteral("Avg tracking"), panel.runtimeTrackingMs);
  statsForm->addRow(QStringLiteral("Avg checkpoint enqueue"),
                    panel.runtimeCheckpointMs);
  statsForm->addRow(QStringLiteral("Checkpoint backlog"),
                    panel.runtimeCheckpointBacklog);
  statsForm->addRow(QStringLiteral("Avg NCNN stages"),
                    panel.runtimeNcnnBreakdown);
  root->addLayout(statsForm);

  auto *settingsTitle = new QLabel(QStringLiteral("Detection settings"));
  QFont settingsFont = settingsTitle->font();
  settingsFont.setBold(true);
  settingsTitle->setFont(settingsFont);
  root->addWidget(settingsTitle);

  auto *form = new QFormLayout;
  root->addLayout(form);
  const std::shared_ptr<bool> syncing = panel.syncing;

  auto persist = [tuning, detector, scrfdTargetSize, settingsPath,
                  previewWindow]() {
    PreviewDebugSettings previewSettings;
    PreviewDebugSettings *previewSettingsPtr = nullptr;
    if (previewWindow) {
      previewSettings.followLatest = previewWindow->followLatest();
      previewSettings.showDetections = previewWindow->showDetections();
      previewSettings.showTracks = previewWindow->showTracks();
      previewSettings.showLabels = previewWindow->showLabels();
      previewSettings.showConfirmedTracks =
          previewWindow->showConfirmedTracks();
      previewSettings.showTentativeTracks =
          previewWindow->showTentativeTracks();
      previewSettings.showLostTracks = previewWindow->showLostTracks();
      previewSettings.playbackSpeed = previewWindow->previewPlaybackSpeed();
      previewSettings.detectionLineThickness =
          previewWindow->detectionLineThickness();
      previewSettings.trackLineThickness = previewWindow->trackLineThickness();
      previewSettings.overlayOpacity = previewWindow->overlayOpacity();
      previewSettingsPtr = &previewSettings;
    }
    QString error;
    if (!saveRuntimeTuningFile(settingsPath, *tuning, detector,
                               tuning && tuning->scrfdTargetSize > 0
                                   ? tuning->scrfdTargetSize
                                   : scrfdTargetSize,
                               previewSettingsPtr, &error) &&
        !error.isEmpty()) {
      qWarning().noquote() << error;
    }
  };
  auto applyProfile = [tuning, persist, &panel](
                          const jcut::facedetections::DetectorRuntimeSettings
                              &profileSettings) {
    if (!tuning) {
      return;
    }
    const QString currentScrfdModelVariant =
        jcut::facedetections::normalizeScrfdModelVariantId(
            tuning->scrfdModelVariant);
    *panel.syncing = true;
    *tuning = runtimeTuningFromDetectorSettings(profileSettings);
    tuning->scrfdModelVariant = currentScrfdModelVariant;
    syncDetectorControlPanel(&panel, *tuning);
    *panel.syncing = false;
    persist();
  };

  auto addSlider = [&](const QString &name, const QString &tooltip,
                       int minValue, int maxValue, int initialValue,
                       QSlider **sliderOut, QLabel **labelOut,
                       const std::function<QString(int)> &labelText,
                       const std::function<void(int)> &applyValue) {
    auto *row = new QWidget;
    row->setToolTip(tooltip);
    auto *layout = new QHBoxLayout(row);
    layout->setContentsMargins(0, 0, 0, 0);
    auto *slider = new QSlider(Qt::Horizontal);
    slider->setRange(minValue, maxValue);
    slider->setValue(initialValue);
    slider->setToolTip(tooltip);
    auto *valueLabel = new QLabel(labelText(initialValue));
    valueLabel->setMinimumWidth(72);
    valueLabel->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
    valueLabel->setToolTip(tooltip);
    layout->addWidget(slider, 1);
    layout->addWidget(valueLabel);
    form->addRow(name, row);
    const std::shared_ptr<bool> syncing = panel.syncing;
    QObject::connect(slider, &QSlider::valueChanged, panel.window,
                     [syncing, tuning, valueLabel, labelText, applyValue,
                      persist](int value) {
                       valueLabel->setText(labelText(value));
                       if (*syncing || !tuning) {
                         return;
                       }
                       applyValue(value);
                       persist();
                     });
    *sliderOut = slider;
    *labelOut = valueLabel;
  };

  auto *profileRow = new QWidget;
  auto *profileLayout = new QHBoxLayout(profileRow);
  profileLayout->setContentsMargins(0, 0, 0, 0);
  panel.profileCombo = new QComboBox(profileRow);
  auto *applyProfileButton =
      new QPushButton(QStringLiteral("Apply"), profileRow);
  profileLayout->addWidget(panel.profileCombo, 1);
  profileLayout->addWidget(applyProfileButton);
  form->addRow(QStringLiteral("Profile"), profileRow);
  panel.profileCombo->setToolTip(QStringLiteral(
      "Built-in immutable detector profiles. Applying one overwrites the live "
      "tuning and persists it to the active settings file."));
  applyProfileButton->setToolTip(panel.profileCombo->toolTip());
  const QVector<jcut::facedetections::DetectorSettingsProfileDefinition>
      profiles = jcut::facedetections::builtInDetectorProfiles();
  for (const jcut::facedetections::DetectorSettingsProfileDefinition &profile :
       profiles) {
    panel.profileCombo->addItem(profile.label, profile.id);
    const int index = panel.profileCombo->count() - 1;
    panel.profileCombo->setItemData(
        index,
        QStringLiteral(
            "%1\nstride %2 | target %3 | threshold %4 | max faces %5")
            .arg(profile.description)
            .arg(profile.settings.stride)
            .arg(profile.settings.scrfdTargetSize)
            .arg(static_cast<double>(profile.settings.threshold), 0, 'f', 2)
            .arg(profile.settings.maxFacesPerFrame),
        Qt::ToolTipRole);
  }
  QObject::connect(
      applyProfileButton, &QPushButton::clicked, panel.window,
      [applyProfile, &panel, profiles]() {
        if (!panel.profileCombo) {
          return;
        }
        const QString selectedId = panel.profileCombo->currentData().toString();
        for (const jcut::facedetections::DetectorSettingsProfileDefinition
                 &profile : profiles) {
          if (profile.id == selectedId) {
            applyProfile(profile.settings);
            return;
          }
        }
      });

  addSlider(
      QStringLiteral("Stride"),
      QStringLiteral("Run detection every Nth source frame. Lower values "
                     "improve recall and preview responsiveness; higher values "
                     "are faster but skip more frames."),
      1, 120, tuning->stride, &panel.stride, &panel.strideValue,
      [](int v) { return QString::number(v); },
      [tuning](int v) { tuning->stride = std::max(1, v); });
  addSlider(
      QStringLiteral("Max detections"),
      QStringLiteral("Maximum number of raw detector boxes kept before "
                     "post-filtering. Raise this for panels or crowds; lower "
                     "it to reduce noisy candidates."),
      1, 1024, tuning->maxDetections, &panel.maxDetections,
      &panel.maxDetectionsValue, [](int v) { return QString::number(v); },
      [tuning](int v) { tuning->maxDetections = std::max(1, v); });
  const bool scrfdFamily =
      detector.contains(QStringLiteral("scrfd"), Qt::CaseInsensitive) ||
      detector.compare(QStringLiteral("jcut-dnn"), Qt::CaseInsensitive) == 0;
  if (scrfdFamily) {
    panel.scrfdModelVariant = new QComboBox;
    panel.scrfdModelVariant->setToolTip(QStringLiteral(
        "SCRFD model family variant. Larger variants improve recall, "
        "especially for small or crowded faces, at higher GPU cost. Model "
        "changes apply on the next run, not mid-run."));
    const QVector<jcut::facedetections::ScrfdModelVariantDefinition> variants =
        jcut::facedetections::supportedScrfdModelVariants();
    for (const jcut::facedetections::ScrfdModelVariantDefinition &variant :
         variants) {
      panel.scrfdModelVariant->addItem(variant.label, variant.id);
      const int index = panel.scrfdModelVariant->count() - 1;
      panel.scrfdModelVariant->setItemData(index, variant.description,
                                           Qt::ToolTipRole);
    }
    panel.scrfdModelVariant->setEnabled(false);
    form->addRow(QStringLiteral("SCRFD model"), panel.scrfdModelVariant);
    addSlider(
        QStringLiteral("SCRFD target"),
        QStringLiteral(
            "Input size for the SCRFD model. Raise this to improve small-face "
            "recall at higher GPU cost; lower it for speed."),
        320, 1280, tuning->scrfdTargetSize, &panel.scrfdTargetSize,
        &panel.scrfdTargetSizeValue, [](int v) { return QString::number(v); },
        [tuning](int v) { tuning->scrfdTargetSize = std::max(320, v); });
  }
  addSlider(
      QStringLiteral("Confidence threshold"),
      QStringLiteral(
          "Minimum detector confidence accepted as a face. Lower this to catch "
          "smaller or blurrier faces; raise it to reject false positives such "
          "as text, hands, or background shapes."),
      1, 99, qRound(tuning->threshold * 100.0f), &panel.threshold,
      &panel.thresholdValue, [](int v) { return percentText(v / 100.0f); },
      [tuning](int v) { tuning->threshold = v / 100.0f; });
  addSlider(
      QStringLiteral("NMS IoU"),
      QStringLiteral(
          "Overlap threshold for merging duplicate face boxes. Lower values "
          "suppress nearby duplicate boxes more aggressively; higher values "
          "keep more overlapping detections."),
      0, 100, qRound(tuning->nmsIouThreshold * 100.0f), &panel.nms,
      &panel.nmsValue, [](int v) { return percentText(v / 100.0f); },
      [tuning](int v) { tuning->nmsIouThreshold = v / 100.0f; });
  addSlider(
      QStringLiteral("ROI left"),
      QStringLiteral(
          "Left edge of the allowed face region as a percent of frame width. "
          "Raise this to ignore detections on the far left."),
      0, 100, qRound(tuning->roiX1 * 100.0f), &panel.roiX1, &panel.roiX1Value,
      [](int v) { return percentText(v / 100.0f); },
      [tuning](int v) {
        tuning->roiX1 = v / 100.0f;
        if (tuning->roiX2 < tuning->roiX1)
          std::swap(tuning->roiX1, tuning->roiX2);
      });
  addSlider(
      QStringLiteral("ROI top"),
      QStringLiteral(
          "Top edge of the allowed face region as a percent of frame height. "
          "Raise this to ignore detections near the top."),
      0, 100, qRound(tuning->roiY1 * 100.0f), &panel.roiY1, &panel.roiY1Value,
      [](int v) { return percentText(v / 100.0f); },
      [tuning](int v) {
        tuning->roiY1 = v / 100.0f;
        if (tuning->roiY2 < tuning->roiY1)
          std::swap(tuning->roiY1, tuning->roiY2);
      });
  addSlider(
      QStringLiteral("ROI right"),
      QStringLiteral(
          "Right edge of the allowed face region as a percent of frame width. "
          "Lower this to ignore detections on the far right."),
      0, 100, qRound(tuning->roiX2 * 100.0f), &panel.roiX2, &panel.roiX2Value,
      [](int v) { return percentText(v / 100.0f); },
      [tuning](int v) {
        tuning->roiX2 = v / 100.0f;
        if (tuning->roiX2 < tuning->roiX1)
          std::swap(tuning->roiX1, tuning->roiX2);
      });
  addSlider(
      QStringLiteral("ROI bottom"),
      QStringLiteral(
          "Bottom edge of the allowed face region as a percent of frame "
          "height. Lower this to ignore detections near the bottom."),
      0, 100, qRound(tuning->roiY2 * 100.0f), &panel.roiY2, &panel.roiY2Value,
      [](int v) { return percentText(v / 100.0f); },
      [tuning](int v) {
        tuning->roiY2 = v / 100.0f;
        if (tuning->roiY2 < tuning->roiY1)
          std::swap(tuning->roiY1, tuning->roiY2);
      });
  addSlider(
      QStringLiteral("Min face area"),
      QStringLiteral(
          "Smallest accepted face box as a percent of frame area. Lower this "
          "for distant/small faces; raise it to reject tiny false positives."),
      0, 2000, qRound(tuning->minFaceAreaRatio * 100000.0f), &panel.minArea,
      &panel.minAreaValue, [](int v) { return areaRatioText(v / 100000.0f); },
      [tuning](int v) { tuning->minFaceAreaRatio = v / 100000.0f; });
  addSlider(
      QStringLiteral("Max face area"),
      QStringLiteral(
          "Largest accepted face box as a percent of frame area. Lower this to "
          "reject oversized false positives or partial-frame detections; raise "
          "it to allow very close faces."),
      0, 100000, qRound(tuning->maxFaceAreaRatio * 100000.0f), &panel.maxArea,
      &panel.maxAreaValue, [](int v) { return areaRatioText(v / 100000.0f); },
      [tuning](int v) { tuning->maxFaceAreaRatio = v / 100000.0f; });
  addSlider(
      QStringLiteral("Min aspect"),
      QStringLiteral(
          "Smallest allowed width/height ratio for a face box. Raise this to "
          "reject tall narrow boxes such as arms or fingers."),
      1, 1000, qRound(tuning->minAspect * 100.0f), &panel.minAspect,
      &panel.minAspectValue, [](int v) { return aspectText(v / 100.0f); },
      [tuning](int v) {
        tuning->minAspect = v / 100.0f;
        if (tuning->maxAspect < tuning->minAspect)
          std::swap(tuning->minAspect, tuning->maxAspect);
      });
  addSlider(
      QStringLiteral("Max aspect"),
      QStringLiteral(
          "Largest allowed width/height ratio for a face box. Lower this to "
          "reject wide non-face boxes such as hands or partial torsos."),
      1, 1000, qRound(tuning->maxAspect * 100.0f), &panel.maxAspect,
      &panel.maxAspectValue, [](int v) { return aspectText(v / 100.0f); },
      [tuning](int v) {
        tuning->maxAspect = v / 100.0f;
        if (tuning->maxAspect < tuning->minAspect)
          std::swap(tuning->minAspect, tuning->maxAspect);
      });
  addSlider(
      QStringLiteral("Max faces/frame"),
      QStringLiteral(
          "Maximum accepted faces per processed frame after filtering. 0 means "
          "unlimited. Use lower values for single-speaker videos and higher "
          "values for panels or crowds."),
      0, 64, tuning->maxFacesPerFrame, &panel.maxFaces, &panel.maxFacesValue,
      [](int v) {
        return v == 0 ? QStringLiteral("unlimited") : QString::number(v);
      },
      [tuning](int v) { tuning->maxFacesPerFrame = v; });

  panel.smallFaceFallback =
      new QCheckBox(QStringLiteral("Enable Res10 tiled fallback"));
  panel.smallFaceFallback->setToolTip(QStringLiteral(
      "Runs extra cropped Res10 passes when the zero-copy Res10 detector "
      "misses. This can help some small faces but is slower and may produce "
      "false positives; SCRFD usually handles small faces better."));
  panel.smallFaceFallback->setChecked(tuning->smallFaceFallback);
  form->addRow(QStringLiteral("Small-face fallback"), panel.smallFaceFallback);
  {
    const std::shared_ptr<bool> syncing = panel.syncing;
    QObject::connect(panel.smallFaceFallback, &QCheckBox::toggled, panel.window,
                     [syncing, tuning, persist](bool checked) {
                       if (*syncing || !tuning) {
                         return;
                       }
                       tuning->smallFaceFallback = checked;
                       persist();
                     });
  }
  if (scrfdFamily) {
    panel.scrfdTiled =
        new QCheckBox(QStringLiteral("Enable SCRFD 2x2 tiled pass"));
    panel.scrfdTiled->setToolTip(QStringLiteral(
        "Runs SCRFD on the full frame plus overlapping 2x2 tiles to improve "
        "recall on smaller panel faces at higher GPU cost."));
    panel.scrfdTiled->setChecked(tuning->scrfdTiled);
    form->addRow(QStringLiteral("SCRFD tiling"), panel.scrfdTiled);
    const std::shared_ptr<bool> syncing = panel.syncing;
    QObject::connect(panel.scrfdTiled, &QCheckBox::toggled, panel.window,
                     [syncing, tuning, persist](bool checked) {
                       if (*syncing || !tuning) {
                         return;
                       }
                       tuning->scrfdTiled = checked;
                       persist();
                     });
  }

  if (allowApplyClipGrading && applyClipGrading) {
    panel.applyClipGrading =
        new QCheckBox(QStringLiteral("Apply clip grading during detection"));
    panel.applyClipGrading->setToolTip(
        QStringLiteral("Use the clip grading payload from --clip-json for both "
                       "live preview and detection input."));
    panel.applyClipGrading->setChecked(*applyClipGrading);
    form->addRow(QStringLiteral("Clip grading"), panel.applyClipGrading);
    QObject::connect(panel.applyClipGrading, &QCheckBox::toggled, panel.window,
                     [applyClipGrading](bool checked) {
                       if (!applyClipGrading) {
                         return;
                       }
                       *applyClipGrading = checked;
                     });
  }

  if (paused) {
    panel.pauseButton = new QPushButton(*paused ? QStringLiteral("Resume")
                                                : QStringLiteral("Pause"));
    panel.pauseButton->setToolTip(
        QStringLiteral("Pause or resume FaceDetections detection while keeping "
                       "the runtime controls responsive."));
    root->addWidget(panel.pauseButton);
    QObject::connect(panel.pauseButton, &QPushButton::clicked, panel.window,
                     [paused, button = panel.pauseButton]() {
                       if (!paused || !button) {
                         return;
                       }
                       *paused = !*paused;
                       button->setText(*paused ? QStringLiteral("Resume")
                                               : QStringLiteral("Pause"));
                     });
  }

  if (previewWindow) {
    auto *previewTitle = new QLabel(QStringLiteral("Preview transport"));
    QFont previewFont = previewTitle->font();
    previewFont.setBold(true);
    previewTitle->setFont(previewFont);
    root->addWidget(previewTitle);

    auto *previewRow = new QHBoxLayout;
    panel.previewPlayButton = new QPushButton(QStringLiteral("Play Preview"));
    panel.previewFollowLatest = new QCheckBox(QStringLiteral("Follow Latest"));
    previewRow->addWidget(panel.previewPlayButton);
    previewRow->addWidget(panel.previewFollowLatest);
    previewRow->addStretch(1);
    root->addLayout(previewRow);

    auto *seekRow = new QHBoxLayout;
    panel.previewSeek = new QSlider(Qt::Horizontal);
    panel.previewSeek->setRange(0, 0);
    panel.previewSeekValue = new QLabel(QStringLiteral("0"));
    panel.previewSeekValue->setMinimumWidth(72);
    seekRow->addWidget(panel.previewSeek, 1);
    seekRow->addWidget(panel.previewSeekValue);
    root->addLayout(seekRow);

    auto *speedRow = new QHBoxLayout;
    auto *speedLabel = new QLabel(QStringLiteral("Preview speed"));
    panel.previewSpeed = new QSlider(Qt::Horizontal);
    panel.previewSpeed->setRange(25, 400);
    panel.previewSpeedValue = new QLabel(QStringLiteral("1.00x"));
    panel.previewSpeedValue->setMinimumWidth(72);
    speedRow->addWidget(speedLabel);
    speedRow->addWidget(panel.previewSpeed, 1);
    speedRow->addWidget(panel.previewSpeedValue);
    root->addLayout(speedRow);

    auto *overlayRow1 = new QHBoxLayout;
    panel.previewShowDetections = new QCheckBox(QStringLiteral("Detections"));
    panel.previewShowTracks = new QCheckBox(QStringLiteral("Tracks"));
    panel.previewShowLabels = new QCheckBox(QStringLiteral("Labels"));
    overlayRow1->addWidget(panel.previewShowDetections);
    overlayRow1->addWidget(panel.previewShowTracks);
    overlayRow1->addWidget(panel.previewShowLabels);
    overlayRow1->addStretch(1);
    root->addLayout(overlayRow1);

    auto *overlayRow2 = new QHBoxLayout;
    panel.previewShowConfirmed = new QCheckBox(QStringLiteral("Confirmed"));
    panel.previewShowTentative = new QCheckBox(QStringLiteral("Tentative"));
    panel.previewShowLost = new QCheckBox(QStringLiteral("Lost"));
    overlayRow2->addWidget(panel.previewShowConfirmed);
    overlayRow2->addWidget(panel.previewShowTentative);
    overlayRow2->addWidget(panel.previewShowLost);
    overlayRow2->addStretch(1);
    root->addLayout(overlayRow2);

    auto *overlayKnobRow1 = new QHBoxLayout;
    auto *detLineLabel = new QLabel(QStringLiteral("Det line"));
    panel.previewDetectionLine = new QSlider(Qt::Horizontal);
    panel.previewDetectionLine->setRange(10, 40);
    panel.previewDetectionLineValue = new QLabel(QStringLiteral("1.5"));
    panel.previewDetectionLineValue->setMinimumWidth(40);
    overlayKnobRow1->addWidget(detLineLabel);
    overlayKnobRow1->addWidget(panel.previewDetectionLine, 1);
    overlayKnobRow1->addWidget(panel.previewDetectionLineValue);
    root->addLayout(overlayKnobRow1);

    auto *overlayKnobRow2 = new QHBoxLayout;
    auto *trackLineLabel = new QLabel(QStringLiteral("Track line"));
    panel.previewTrackLine = new QSlider(Qt::Horizontal);
    panel.previewTrackLine->setRange(10, 50);
    panel.previewTrackLineValue = new QLabel(QStringLiteral("2.5"));
    panel.previewTrackLineValue->setMinimumWidth(40);
    overlayKnobRow2->addWidget(trackLineLabel);
    overlayKnobRow2->addWidget(panel.previewTrackLine, 1);
    overlayKnobRow2->addWidget(panel.previewTrackLineValue);
    root->addLayout(overlayKnobRow2);

    auto *overlayKnobRow3 = new QHBoxLayout;
    auto *opacityLabel = new QLabel(QStringLiteral("Opacity"));
    panel.previewOpacity = new QSlider(Qt::Horizontal);
    panel.previewOpacity->setRange(20, 100);
    panel.previewOpacityValue = new QLabel(QStringLiteral("1.00"));
    panel.previewOpacityValue->setMinimumWidth(40);
    overlayKnobRow3->addWidget(opacityLabel);
    overlayKnobRow3->addWidget(panel.previewOpacity, 1);
    overlayKnobRow3->addWidget(panel.previewOpacityValue);
    root->addLayout(overlayKnobRow3);

    QObject::connect(panel.previewPlayButton, &QPushButton::clicked,
                     panel.window, [syncing, previewWindow]() {
                       if (!previewWindow || !syncing || *syncing) {
                         return;
                       }
                       previewWindow->setPreviewPlaybackActive(
                           !previewWindow->previewPlaybackActive());
                     });
    QObject::connect(panel.previewFollowLatest, &QCheckBox::toggled,
                     panel.window, [syncing, previewWindow](bool checked) {
                       if (!previewWindow || !syncing || *syncing) {
                         return;
                       }
                       previewWindow->setFollowLatest(checked);
                     });
    QObject::connect(panel.previewSeek, &QSlider::valueChanged, panel.window,
                     [syncing, previewWindow](int value) {
                       if (!previewWindow || !syncing || *syncing) {
                         return;
                       }
                       previewWindow->setRequestedPreviewFrame(value);
                     });
    QObject::connect(panel.previewSpeed, &QSlider::valueChanged, panel.window,
                     [syncing, previewWindow](int value) {
                       if (!previewWindow || !syncing || *syncing) {
                         return;
                       }
                       previewWindow->setPreviewPlaybackSpeed(
                           static_cast<float>(value) / 100.0f);
                     });
    QObject::connect(panel.previewShowDetections, &QCheckBox::toggled,
                     panel.window, [syncing, previewWindow](bool checked) {
                       if (!previewWindow || !syncing || *syncing) {
                         return;
                       }
                       previewWindow->setShowDetections(checked);
                     });
    QObject::connect(panel.previewShowTracks, &QCheckBox::toggled, panel.window,
                     [syncing, previewWindow](bool checked) {
                       if (!previewWindow || !syncing || *syncing) {
                         return;
                       }
                       previewWindow->setShowTracks(checked);
                     });
    QObject::connect(panel.previewShowLabels, &QCheckBox::toggled, panel.window,
                     [syncing, previewWindow](bool checked) {
                       if (!previewWindow || !syncing || *syncing) {
                         return;
                       }
                       previewWindow->setShowLabels(checked);
                     });
    QObject::connect(panel.previewShowConfirmed, &QCheckBox::toggled,
                     panel.window, [syncing, previewWindow](bool checked) {
                       if (!previewWindow || !syncing || *syncing) {
                         return;
                       }
                       previewWindow->setShowConfirmedTracks(checked);
                     });
    QObject::connect(panel.previewShowTentative, &QCheckBox::toggled,
                     panel.window, [syncing, previewWindow](bool checked) {
                       if (!previewWindow || !syncing || *syncing) {
                         return;
                       }
                       previewWindow->setShowTentativeTracks(checked);
                     });
    QObject::connect(panel.previewShowLost, &QCheckBox::toggled, panel.window,
                     [syncing, previewWindow](bool checked) {
                       if (!previewWindow || !syncing || *syncing) {
                         return;
                       }
                       previewWindow->setShowLostTracks(checked);
                     });
    QObject::connect(panel.previewDetectionLine, &QSlider::valueChanged,
                     panel.window, [syncing, previewWindow](int value) {
                       if (!previewWindow || !syncing || *syncing) {
                         return;
                       }
                       previewWindow->setDetectionLineThickness(
                           static_cast<float>(value) / 10.0f);
                     });
    QObject::connect(panel.previewTrackLine, &QSlider::valueChanged,
                     panel.window, [syncing, previewWindow](int value) {
                       if (!previewWindow || !syncing || *syncing) {
                         return;
                       }
                       previewWindow->setTrackLineThickness(
                           static_cast<float>(value) / 10.0f);
                     });
    QObject::connect(panel.previewOpacity, &QSlider::valueChanged, panel.window,
                     [syncing, previewWindow](int value) {
                       if (!previewWindow || !syncing || *syncing) {
                         return;
                       }
                       previewWindow->setOverlayOpacity(
                           static_cast<float>(value) / 100.0f);
                     });
  }

  panel.settingsPath = new QLabel(settingsPath);
  panel.settingsPath->setToolTip(
      QStringLiteral("Detector settings are saved here automatically and "
                     "loaded the next time this video is opened."));
  panel.settingsPath->setTextInteractionFlags(Qt::TextSelectableByMouse);
  panel.settingsPath->setWordWrap(true);
  root->addWidget(panel.settingsPath);

  syncDetectorControlPanel(&panel, *tuning);
  return panel;
}
