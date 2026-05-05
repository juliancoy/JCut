#include "speakers_tab.h"
#include "speakers_tab_internal.h"
#include "speaker_flow_debug.h"

#include "boxstream_generation.h"
#include "boxstream_runtime.h"
#include "decoder_context.h"
#include "render_internal.h"
#include "transcript_engine.h"

#include <QApplication>
#include <QCheckBox>
#include <QComboBox>
#include <QDialog>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QHBoxLayout>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLabel>
#include <QMessageBox>
#include <QPlainTextEdit>
#include <QProcess>
#include <QProgressBar>
#include <QPushButton>
#include <QStandardItemModel>
#include <QTextCursor>
#include <QTimer>
#include <QVBoxLayout>

#include <algorithm>
#include <cmath>
#include <limits>

#if JCUT_HAVE_OPENCV
#include <opencv2/core.hpp>
#include <opencv2/dnn.hpp>
#include <opencv2/imgproc.hpp>
#include <opencv2/objdetect.hpp>
#endif

using namespace jcut::boxstream;

void SpeakersTab::onSpeakerRunAutoTrackClicked()
{
    if (!activeCutMutable() || !m_loadedTranscriptDoc.isObject()) {
        return;
    }
    const TimelineClip* selectedClip = m_deps.getSelectedClip ? m_deps.getSelectedClip() : nullptr;
    if (!selectedClip) {
        QMessageBox::warning(nullptr, QStringLiteral("Generate BoxStream"), QStringLiteral("Select a clip first."));
        return;
    }

    bool onlyDialogue = false;
    BoxstreamDetectorPreset detectorPreset = BoxstreamDetectorPreset::HaarBalanced;
    {
        QDialog preflightDialog;
        preflightDialog.setWindowTitle(QStringLiteral("Generate BoxStream Preflight"));
        preflightDialog.setWindowFlag(Qt::Window, true);
        preflightDialog.resize(560, 220);
        auto* layout = new QVBoxLayout(&preflightDialog);
        layout->setContentsMargins(12, 12, 12, 12);
        layout->setSpacing(8);
        auto* infoLabel = new QLabel(
            QStringLiteral("This run is identity-agnostic and continuity-based.\n\n"
                           "It generates independent BoxStreams for all detected face tracks."),
            &preflightDialog);
        infoLabel->setWordWrap(true);
        layout->addWidget(infoLabel);
        auto* windowsCheck =
            new QCheckBox(QStringLiteral("Only scan when dialogue is present"), &preflightDialog);
        windowsCheck->setChecked(false);
        layout->addWidget(windowsCheck);
        auto* algoRow = new QHBoxLayout;
        auto* algoLabel = new QLabel(QStringLiteral("Detection algorithm"), &preflightDialog);
        auto* algoCombo = new QComboBox(&preflightDialog);
        algoCombo->addItem(QStringLiteral("Haar Balanced (Recommended)"),
                           static_cast<int>(BoxstreamDetectorPreset::HaarBalanced));
        algoCombo->addItem(QStringLiteral("Haar Small Faces"),
                           static_cast<int>(BoxstreamDetectorPreset::HaarSmallFaces));
        algoCombo->addItem(QStringLiteral("Haar Precision"),
                           static_cast<int>(BoxstreamDetectorPreset::HaarPrecision));
        algoCombo->addItem(QStringLiteral("LBP Small Faces"),
                           static_cast<int>(BoxstreamDetectorPreset::LbpSmallFaces));
        algoCombo->addItem(QStringLiteral("Python Compatible"),
                           static_cast<int>(BoxstreamDetectorPreset::PythonCompatible));
        algoCombo->addItem(QStringLiteral("DNN Auto (CUDA/CPU)"),
                           static_cast<int>(BoxstreamDetectorPreset::DnnAuto));
        algoCombo->addItem(QStringLiteral("Native CUDA DNN Face Detector"),
                           static_cast<int>(BoxstreamDetectorPreset::NativeCudaDnn));
        const int cudaDnnIndex = algoCombo->count() - 1;
        algoCombo->addItem(QStringLiteral("Native Vulkan Face Preprocess (JCut Vulkan)"),
                           static_cast<int>(BoxstreamDetectorPreset::NativeVulkanDnn));
        algoCombo->addItem(QStringLiteral("Python Script (Legacy)"),
                           static_cast<int>(BoxstreamDetectorPreset::PythonLegacy));
        algoCombo->addItem(QStringLiteral("Local Production Hybrid (CPU, Non-Docker)"),
                           static_cast<int>(BoxstreamDetectorPreset::LocalHybrid));
        algoCombo->addItem(QStringLiteral("Native Production Hybrid (C++, CPU)"),
                           static_cast<int>(BoxstreamDetectorPreset::NativeHybridCpu));
        algoCombo->addItem(QStringLiteral("Native Production Hybrid (Vulkan Decode Path)"),
                           static_cast<int>(BoxstreamDetectorPreset::NativeHybridVulkan));
        algoCombo->addItem(QStringLiteral("Docker DNN Res10 (Volume Weights)"),
                           static_cast<int>(BoxstreamDetectorPreset::DockerDnn));
        const int dockerDnnIndex = algoCombo->count() - 1;
        algoCombo->addItem(QStringLiteral("Docker InsightFace RetinaFace"),
                           static_cast<int>(BoxstreamDetectorPreset::DockerInsightFace));
        const int dockerInsightFaceIndex = algoCombo->count() - 1;
        algoCombo->addItem(QStringLiteral("Docker Production Hybrid (RetinaFace + ReID + Smoothing) (Recommended)"),
                           static_cast<int>(BoxstreamDetectorPreset::DockerHybrid));
        const int dockerHybridIndex = algoCombo->count() - 1;
        algoCombo->addItem(QStringLiteral("Docker YOLOv8-Face"),
                           static_cast<int>(BoxstreamDetectorPreset::DockerYoloFace));
        const int dockerYoloIndex = algoCombo->count() - 1;
        algoCombo->addItem(QStringLiteral("Docker MTCNN"),
                           static_cast<int>(BoxstreamDetectorPreset::DockerMtcnn));
        const int dockerMtcnnIndex = algoCombo->count() - 1;
        algoCombo->addItem(QStringLiteral("SAM3 (prompt: face)"),
                           static_cast<int>(BoxstreamDetectorPreset::Sam3Face));
        const int contribCsrtIndex = algoCombo->count();
        algoCombo->addItem(QStringLiteral("OpenCV Contrib CSRT (Haar + Tracker)"),
                           static_cast<int>(BoxstreamDetectorPreset::ContribCsrt));
        const int contribKcfIndex = algoCombo->count();
        algoCombo->addItem(QStringLiteral("OpenCV Contrib KCF (Haar + Tracker)"),
                           static_cast<int>(BoxstreamDetectorPreset::ContribKcf));
        auto* comboModel = qobject_cast<QStandardItemModel*>(algoCombo->model());
        if (comboModel) {
            bool cudaDnnAvailable = false;
#if JCUT_HAVE_OPENCV
            const std::vector<cv::dnn::Target> cudaTargets =
                cv::dnn::getAvailableTargets(cv::dnn::DNN_BACKEND_CUDA);
            cudaDnnAvailable =
                std::find(cudaTargets.begin(), cudaTargets.end(), cv::dnn::DNN_TARGET_CUDA_FP16) != cudaTargets.end() ||
                std::find(cudaTargets.begin(), cudaTargets.end(), cv::dnn::DNN_TARGET_CUDA) != cudaTargets.end();
#endif
            if (QStandardItem* item = comboModel->item(cudaDnnIndex)) {
                item->setEnabled(cudaDnnAvailable);
                if (!cudaDnnAvailable) {
                    item->setText(QStringLiteral("Native CUDA DNN Face Detector (unavailable: rebuild OpenCV with CUDA/cuDNN)"));
                }
            }
            const QColor dockerBg(QStringLiteral("#cfe8ff"));
            for (const int idx : {dockerDnnIndex, dockerInsightFaceIndex, dockerHybridIndex, dockerYoloIndex, dockerMtcnnIndex}) {
                if (QStandardItem* item = comboModel->item(idx)) {
                    item->setData(dockerBg, Qt::BackgroundRole);
                }
            }
        }
        auto* stepsLabel = new QLabel(
            QStringLiteral("Pipeline steps:\n"
                           "1. Detect faces each sample frame\n"
                           "2. Associate detections into tracks (IoU + ReID where available)\n"
                           "3. Temporal smoothing (center + size)\n"
                           "4. Write continuity BoxStreams\n"
                           "5. Preview superimposed overlays by tracker source"),
            &preflightDialog);
        stepsLabel->setWordWrap(true);
        stepsLabel->setStyleSheet(QStringLiteral("color: #8fa3b8; font-size: 11px;"));
        layout->addWidget(stepsLabel);
#if !JCUT_HAVE_OPENCV_CONTRIB
        if (comboModel) {
            if (QStandardItem* csrtItem = comboModel->item(contribCsrtIndex)) {
                csrtItem->setEnabled(false);
                csrtItem->setToolTip(QStringLiteral("Unavailable in this build."));
            }
            if (QStandardItem* kcfItem = comboModel->item(contribKcfIndex)) {
                kcfItem->setEnabled(false);
                kcfItem->setToolTip(QStringLiteral("Unavailable in this build."));
            }
        }
#endif
        algoRow->addWidget(algoLabel);
        algoRow->addWidget(algoCombo, 1);
        layout->addLayout(algoRow);
        auto* buttons = new QHBoxLayout;
        buttons->addStretch(1);
        auto* cancelButton = new QPushButton(QStringLiteral("Cancel"), &preflightDialog);
        auto* proceedButton = new QPushButton(QStringLiteral("Proceed"), &preflightDialog);
        buttons->addWidget(cancelButton);
        buttons->addWidget(proceedButton);
        layout->addLayout(buttons);
        connect(cancelButton, &QPushButton::clicked, &preflightDialog, &QDialog::reject);
        connect(proceedButton, &QPushButton::clicked, &preflightDialog, &QDialog::accept);
        if (preflightDialog.exec() != QDialog::Accepted) {
            return;
        }
        onlyDialogue = windowsCheck->isChecked();
        detectorPreset = static_cast<BoxstreamDetectorPreset>(algoCombo->currentData().toInt());
    }

    auto resolveMediaPath = [&](const TimelineClip& clip) {
        QString candidate = interactivePreviewMediaPathForClip(clip);
        const QFileInfo candidateInfo(candidate);
        const bool candidateIsSequenceDir =
            !candidate.trimmed().isEmpty() &&
            candidateInfo.exists() &&
            candidateInfo.isDir() &&
            isImageSequencePath(candidate);
        const bool interactiveInvalid =
            candidate.trimmed().isEmpty() ||
            !candidateInfo.exists() ||
            (candidateInfo.isDir() && !candidateIsSequenceDir);
        if (!interactiveInvalid) {
            return candidate;
        }
        const QString sourcePath = clip.filePath.trimmed();
        const QFileInfo sourceInfo(sourcePath);
        if (!sourcePath.isEmpty() &&
            sourceInfo.exists() &&
            (sourceInfo.isFile() || isImageSequencePath(sourcePath))) {
            return sourcePath;
        }
        return QString();
    };
    const QString mediaPath = resolveMediaPath(*selectedClip);
    if (mediaPath.isEmpty()) {
        QMessageBox::warning(nullptr, QStringLiteral("Generate BoxStream"),
                             QStringLiteral("No playable media was found for this clip."));
        return;
    }

    QJsonObject transcriptRoot = m_loadedTranscriptDoc.object();
    int64_t scanStart = 0;
    int64_t scanEnd = 0;
    bool haveTranscriptRange = false;
    {
        const QJsonArray segments = transcriptRoot.value(QStringLiteral("segments")).toArray();
        double minStart = std::numeric_limits<double>::max();
        double maxEnd = -1.0;
        for (const QJsonValue& segValue : segments) {
            const QJsonObject segmentObj = segValue.toObject();
            const QJsonArray words = segmentObj.value(QStringLiteral("words")).toArray();
            for (const QJsonValue& wordValue : words) {
                const QJsonObject wordObj = wordValue.toObject();
                if (wordObj.value(QStringLiteral("skipped")).toBool(false)) {
                    continue;
                }
                const QString text = wordObj.value(QStringLiteral("word")).toString().trimmed();
                if (text.isEmpty()) {
                    continue;
                }
                const double startSeconds = wordObj.value(QStringLiteral("start")).toDouble(-1.0);
                const double endSeconds = wordObj.value(QStringLiteral("end")).toDouble(-1.0);
                if (startSeconds < 0.0 || endSeconds < startSeconds) {
                    continue;
                }
                minStart = qMin(minStart, startSeconds);
                maxEnd = qMax(maxEnd, endSeconds);
            }
        }
        if (maxEnd >= 0.0 && minStart <= maxEnd) {
            scanStart = qMax<int64_t>(0, static_cast<int64_t>(std::floor(minStart * kTimelineFps)));
            scanEnd = qMax<int64_t>(scanStart, static_cast<int64_t>(std::floor(maxEnd * kTimelineFps)));
            haveTranscriptRange = true;
        }
    }
    if (!haveTranscriptRange) {
        scanStart = qMax<int64_t>(0, selectedClip->sourceInFrame);
        scanEnd = qMax<int64_t>(scanStart, selectedClip->sourceInFrame + qMax<int64_t>(0, selectedClip->durationFrames - 1));
    }

    auto debugRun = speaker_flow_debug::openLatestOrCreateRun(
        m_loadedTranscriptPath,
        selectedClip->id.trimmed().isEmpty() ? QStringLiteral("unknown_clip") : selectedClip->id,
        QFileInfo(mediaPath).completeBaseName());
    const QString requestPath = QDir(debugRun.runDir).absoluteFilePath(
        QStringLiteral("%1_continuity_boxstream_request.json").arg(debugRun.videoStem));
    const QString outputPath = QDir(debugRun.runDir).absoluteFilePath(
        QStringLiteral("%1_continuity_boxstream_output.json").arg(debugRun.videoStem));
    const QString logPath = QDir(debugRun.runDir).absoluteFilePath(
        QStringLiteral("%1_continuity_boxstream_log.txt").arg(debugRun.videoStem));
    const QString indexPath = QDir(debugRun.runDir).absoluteFilePath(QStringLiteral("index.json"));

    const int stepFrames = detectorPreset == BoxstreamDetectorPreset::NativeVulkanDnn ? 12 : 6;
    const int maxCandidates = 128;
    bool localInsightfaceAvailable = true;
    if (detectorPreset == BoxstreamDetectorPreset::LocalHybrid) {
        QProcess probe;
        probe.setProgram(QStringLiteral("python3"));
        probe.setArguments({QStringLiteral("-c"), QStringLiteral("import insightface")});
        probe.start();
        const bool finished = probe.waitForFinished(4000);
        localInsightfaceAvailable = finished && probe.exitStatus() == QProcess::NormalExit && probe.exitCode() == 0;
        if (!localInsightfaceAvailable) {
            detectorPreset = BoxstreamDetectorPreset::NativeHybridCpu;
            qWarning().noquote()
                << "[boxstream] local_insightface_hybrid_v1 unavailable (insightface import failed); "
                   "falling back to native_hybrid_v1 (C++ CPU).";
        }
    }

    QJsonObject request;
    request[QStringLiteral("run_id")] = debugRun.runId;
    request[QStringLiteral("mode")] = QStringLiteral("continuity_identity_agnostic");
    request[QStringLiteral("media_path")] = mediaPath;
    request[QStringLiteral("scan_start_frame")] = static_cast<qint64>(scanStart);
    request[QStringLiteral("scan_end_frame")] = static_cast<qint64>(scanEnd);
    request[QStringLiteral("only_dialogue")] = onlyDialogue;
    request[QStringLiteral("step_frames")] = stepFrames;
    request[QStringLiteral("max_candidates")] = maxCandidates;
    request[QStringLiteral("engine")] = QStringLiteral("native_cpp_continuity_v1");
    QFile reqFile(requestPath);
    if (reqFile.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        reqFile.write(QJsonDocument(request).toJson(QJsonDocument::Indented));
        reqFile.close();
    }

    QString detectorMode = QStringLiteral("opencv_haar_v1");
    if (detectorPreset == BoxstreamDetectorPreset::HaarSmallFaces) {
        detectorMode = QStringLiteral("opencv_haar_smallfaces_v1");
    } else if (detectorPreset == BoxstreamDetectorPreset::HaarPrecision) {
        detectorMode = QStringLiteral("opencv_haar_precision_v1");
    } else if (detectorPreset == BoxstreamDetectorPreset::LbpSmallFaces) {
        detectorMode = QStringLiteral("opencv_lbp_smallfaces_v1");
    } else if (detectorPreset == BoxstreamDetectorPreset::PythonCompatible) {
        detectorMode = QStringLiteral("opencv_python_compatible_v1");
    } else if (detectorPreset == BoxstreamDetectorPreset::DnnAuto) {
        detectorMode = QStringLiteral("opencv_dnn_auto_v1");
    } else if (detectorPreset == BoxstreamDetectorPreset::NativeCudaDnn) {
        detectorMode = QStringLiteral("native_cuda_dnn_pending_v1");
    } else if (detectorPreset == BoxstreamDetectorPreset::NativeVulkanDnn) {
        detectorMode = QStringLiteral("native_vulkan_dnn_pending_v1");
    } else if (detectorPreset == BoxstreamDetectorPreset::PythonLegacy) {
        detectorMode = QStringLiteral("python_legacy_v1");
    } else if (detectorPreset == BoxstreamDetectorPreset::LocalHybrid) {
        detectorMode = QStringLiteral("local_insightface_hybrid_v1");
    } else if (detectorPreset == BoxstreamDetectorPreset::DockerDnn) {
        detectorMode = QStringLiteral("docker_dnn_v1");
    } else if (detectorPreset == BoxstreamDetectorPreset::DockerInsightFace) {
        detectorMode = QStringLiteral("docker_insightface_retinaface_v1");
    } else if (detectorPreset == BoxstreamDetectorPreset::DockerYoloFace) {
        detectorMode = QStringLiteral("docker_yolov8_face_v1");
    } else if (detectorPreset == BoxstreamDetectorPreset::DockerMtcnn) {
        detectorMode = QStringLiteral("docker_mtcnn_v1");
    } else if (detectorPreset == BoxstreamDetectorPreset::DockerHybrid) {
        detectorMode = QStringLiteral("docker_insightface_hybrid_v1");
    } else if (detectorPreset == BoxstreamDetectorPreset::Sam3Face) {
        detectorMode = QStringLiteral("sam3_face_v1");
    } else if (detectorPreset == BoxstreamDetectorPreset::ContribCsrt) {
        detectorMode = QStringLiteral("opencv_contrib_csrt_v1");
    } else if (detectorPreset == BoxstreamDetectorPreset::ContribKcf) {
        detectorMode = QStringLiteral("opencv_contrib_kcf_v1");
    } else if (detectorPreset == BoxstreamDetectorPreset::NativeHybridCpu) {
        detectorMode = QStringLiteral("native_hybrid_v1");
    } else if (detectorPreset == BoxstreamDetectorPreset::NativeHybridVulkan) {
        detectorMode = QStringLiteral("native_hybrid_vulkan_v1");
    }
    QString processOutput;
    QJsonArray tracksJson;

    if (detectorPreset == BoxstreamDetectorPreset::PythonLegacy ||
        (detectorPreset == BoxstreamDetectorPreset::LocalHybrid && localInsightfaceAvailable) ||
        detectorPreset == BoxstreamDetectorPreset::DockerDnn ||
        detectorPreset == BoxstreamDetectorPreset::DockerInsightFace ||
        detectorPreset == BoxstreamDetectorPreset::DockerHybrid ||
        detectorPreset == BoxstreamDetectorPreset::DockerYoloFace ||
        detectorPreset == BoxstreamDetectorPreset::DockerMtcnn ||
        detectorPreset == BoxstreamDetectorPreset::Sam3Face) {
        const QFileInfo mediaInfo(mediaPath);
        const QString faceScriptPath = QDir(QDir::currentPath()).absoluteFilePath(QStringLiteral("speaker_face_candidates.py"));
        const QString weightsHostDir = QDir(QDir::currentPath()).absoluteFilePath(
            QStringLiteral("external/opencv/samples/dnn/face_detector"));
        const QString hostOutputPath = outputPath;
        const QString progressHostPath =
            QDir(debugRun.runDir).absoluteFilePath(QStringLiteral("docker_progress.json"));
        const QString previewHostPath =
            QDir(debugRun.runDir).absoluteFilePath(QStringLiteral("docker_preview.jpg"));
        QString jsonPathToRead = hostOutputPath;

        QProcess proc;
        if (detectorPreset == BoxstreamDetectorPreset::PythonLegacy) {
            if (!QFileInfo::exists(faceScriptPath)) {
                QMessageBox::warning(nullptr, QStringLiteral("Generate BoxStream"),
                                     QStringLiteral("speaker_face_candidates.py was not found."));
                return;
            }
            proc.setProgram(QStringLiteral("python3"));
            proc.setArguments({
                faceScriptPath,
                QStringLiteral("--video"), mediaPath,
                QStringLiteral("--output-json"), hostOutputPath,
                QStringLiteral("--output-dir"), debugRun.runDir,
                QStringLiteral("--start-frame"), QString::number(scanStart),
                QStringLiteral("--end-frame"), QString::number(scanEnd),
                QStringLiteral("--step"), QString::number(stepFrames),
                QStringLiteral("--source-fps"), QString::number(selectedClip->sourceFps > 0.0 ? selectedClip->sourceFps : 30.0, 'f', 6),
                QStringLiteral("--max-candidates"), QString::number(maxCandidates),
                QStringLiteral("--crop-prefix"), QStringLiteral("continuity_track")
            });
        } else if (detectorPreset == BoxstreamDetectorPreset::LocalHybrid) {
            const QString runnerPath = QDir(QDir::currentPath()).absoluteFilePath(
                QStringLiteral("docker_face_detector.py"));
            if (!QFileInfo::exists(runnerPath)) {
                QMessageBox::warning(nullptr, QStringLiteral("Generate BoxStream"),
                                     QStringLiteral("docker_face_detector.py was not found."));
                return;
            }
            proc.setProgram(QStringLiteral("python3"));
            proc.setArguments({
                runnerPath,
                QStringLiteral("--backend"), QStringLiteral("insightface_hybrid"),
                QStringLiteral("--video"), mediaPath,
                QStringLiteral("--output-json"), hostOutputPath,
                QStringLiteral("--output-dir"), debugRun.runDir,
                QStringLiteral("--progress-json"), progressHostPath,
                QStringLiteral("--preview-jpg"), previewHostPath,
                QStringLiteral("--start-frame"), QString::number(scanStart),
                QStringLiteral("--end-frame"), QString::number(scanEnd),
                QStringLiteral("--step"), QString::number(stepFrames),
                QStringLiteral("--source-fps"), QString::number(selectedClip->sourceFps > 0.0 ? selectedClip->sourceFps : 30.0, 'f', 6),
                QStringLiteral("--max-candidates"), QString::number(maxCandidates),
                QStringLiteral("--crop-prefix"), QStringLiteral("continuity_track")
            });
        } else if (detectorPreset == BoxstreamDetectorPreset::Sam3Face) {
            const QString sam3Path = QDir(QDir::currentPath()).absoluteFilePath(QStringLiteral("sam3.sh"));
            if (!QFileInfo::exists(sam3Path)) {
                QMessageBox::warning(nullptr, QStringLiteral("Generate BoxStream"),
                                     QStringLiteral("sam3.sh was not found."));
                return;
            }
            proc.setProgram(QStringLiteral("/bin/bash"));
            proc.setArguments({
                sam3Path,
                mediaPath,
                QStringLiteral("--prompt"), QStringLiteral("face"),
                QStringLiteral("--out"), debugRun.runDir
            });
        } else {
            QString backendName = QStringLiteral("opencv_dnn");
            if (detectorPreset == BoxstreamDetectorPreset::DockerInsightFace) {
                backendName = QStringLiteral("insightface_retinaface");
            } else if (detectorPreset == BoxstreamDetectorPreset::DockerHybrid) {
                backendName = QStringLiteral("insightface_hybrid");
            } else if (detectorPreset == BoxstreamDetectorPreset::DockerYoloFace) {
                backendName = QStringLiteral("yolov8_face");
            } else if (detectorPreset == BoxstreamDetectorPreset::DockerMtcnn) {
                backendName = QStringLiteral("mtcnn");
            }
            QString imageName;
            if (detectorPreset == BoxstreamDetectorPreset::DockerInsightFace ||
                detectorPreset == BoxstreamDetectorPreset::DockerHybrid) {
                imageName = qEnvironmentVariable(
                    "JCUT_BOXSTREAM_IMAGE_INSIGHTFACE",
                    QStringLiteral("pytorch/pytorch:2.4.1-cuda12.1-cudnn9-runtime"));
            } else if (detectorPreset == BoxstreamDetectorPreset::DockerYoloFace) {
                imageName = qEnvironmentVariable(
                    "JCUT_BOXSTREAM_IMAGE_YOLOFACE",
                    QStringLiteral("ultralytics/ultralytics:latest"));
            } else if (detectorPreset == BoxstreamDetectorPreset::DockerMtcnn) {
                imageName = qEnvironmentVariable(
                    "JCUT_BOXSTREAM_IMAGE_MTCNN",
                    QStringLiteral("pytorch/pytorch:2.4.1-cuda12.1-cudnn9-runtime"));
            } else {
                imageName = qEnvironmentVariable(
                    "JCUT_BOXSTREAM_IMAGE_OPENCV_DNN",
                    QStringLiteral("python:3.11-slim"));
            }
            const QString mediaMount = QStringLiteral("/jcut_media");
            const QString outMount = QStringLiteral("/jcut_out");
            const QString weightsMount = QStringLiteral("/jcut_weights");
            const QString runnerMount = QStringLiteral("/jcut_runner");
            const QString mediaHostMount = mediaInfo.isDir() ? mediaInfo.absoluteFilePath() : mediaInfo.absolutePath();
            const QString mediaInContainer = mediaInfo.isDir()
                ? mediaMount
                : (mediaMount + QStringLiteral("/") + mediaInfo.fileName());
            const QString outputInContainer =
                outMount + QStringLiteral("/") + QFileInfo(hostOutputPath).fileName();
            const QString progressInContainer =
                outMount + QStringLiteral("/") + QFileInfo(progressHostPath).fileName();
            const QString previewInContainer =
                outMount + QStringLiteral("/") + QFileInfo(previewHostPath).fileName();
            const QString runnerHostPath = QDir(QDir::currentPath()).absoluteFilePath(
                QStringLiteral("docker_face_detector.py"));
            jsonPathToRead = hostOutputPath;
            if (!QFileInfo::exists(runnerHostPath)) {
                QMessageBox::warning(nullptr, QStringLiteral("Generate BoxStream"),
                                     QStringLiteral("docker_face_detector.py was not found."));
                return;
            }
            QStringList args = {
                QStringLiteral("run"), QStringLiteral("--rm"),
                QStringLiteral("--gpus"), qEnvironmentVariable("JCUT_BOXSTREAM_DOCKER_GPUS", QStringLiteral("all")),
                QStringLiteral("-v"), QStringLiteral("%1:%2:ro").arg(mediaHostMount, mediaMount),
                QStringLiteral("-v"), QStringLiteral("%1:%2").arg(debugRun.runDir, outMount),
                QStringLiteral("-v"), QStringLiteral("%1:%2:ro").arg(weightsHostDir, weightsMount),
                QStringLiteral("-v"), QStringLiteral("%1:%2:ro").arg(runnerHostPath, runnerMount + QStringLiteral("/docker_face_detector.py")),
                QStringLiteral("-e"), QStringLiteral("JCUT_DNN_WEIGHTS_DIR=%1").arg(weightsMount),
                QStringLiteral("-e"), QStringLiteral("JCUT_DNN_BACKEND=%1").arg(backendName),
                imageName
            };
            const QStringList detectorArgs = {
                QStringLiteral("--backend"), backendName,
                QStringLiteral("--video"), mediaInContainer,
                QStringLiteral("--output-json"), outputInContainer,
                QStringLiteral("--output-dir"), outMount,
                QStringLiteral("--progress-json"), progressInContainer,
                QStringLiteral("--preview-jpg"), previewInContainer,
                QStringLiteral("--start-frame"), QString::number(scanStart),
                QStringLiteral("--end-frame"), QString::number(scanEnd),
                QStringLiteral("--step"), QString::number(stepFrames),
                QStringLiteral("--source-fps"), QString::number(selectedClip->sourceFps > 0.0 ? selectedClip->sourceFps : 30.0, 'f', 6),
                QStringLiteral("--max-candidates"), QString::number(maxCandidates),
                QStringLiteral("--crop-prefix"), QStringLiteral("continuity_track")
            };
            QString bootstrap;
            if (detectorPreset == BoxstreamDetectorPreset::DockerYoloFace) {
                bootstrap = QStringLiteral(
                    "python3 -m pip install --no-cache-dir -q opencv-python-headless >/dev/null 2>&1 || true; ");
            } else if (detectorPreset == BoxstreamDetectorPreset::DockerInsightFace ||
                       detectorPreset == BoxstreamDetectorPreset::DockerHybrid) {
                bootstrap = QStringLiteral(
                    "python3 -m pip install --no-cache-dir -q opencv-python-headless insightface onnxruntime-gpu >/dev/null 2>&1; ");
            } else if (detectorPreset == BoxstreamDetectorPreset::DockerMtcnn) {
                bootstrap = QStringLiteral(
                    "python3 -m pip install --no-cache-dir -q opencv-python-headless facenet-pytorch >/dev/null 2>&1; ");
            } else {
                bootstrap = QStringLiteral(
                    "python3 -m pip install --no-cache-dir -q opencv-python-headless >/dev/null 2>&1; ");
            }
            QString command = bootstrap + QStringLiteral("python3 ") +
                runnerMount + QStringLiteral("/docker_face_detector.py");
            for (const QString& arg : detectorArgs) {
                QString escaped = arg;
                escaped.replace(QStringLiteral("'"), QStringLiteral("'\\''"));
                command += QStringLiteral(" '") + escaped + QStringLiteral("'");
            }
            args << QStringLiteral("/bin/bash") << QStringLiteral("-lc") << command;
            proc.setProgram(QStringLiteral("docker"));
            proc.setArguments(args);
        }

        const bool dockerTerminalMode =
            detectorPreset == BoxstreamDetectorPreset::DockerDnn ||
            detectorPreset == BoxstreamDetectorPreset::DockerInsightFace ||
            detectorPreset == BoxstreamDetectorPreset::DockerHybrid ||
            detectorPreset == BoxstreamDetectorPreset::DockerYoloFace ||
            detectorPreset == BoxstreamDetectorPreset::DockerMtcnn;
        QString mergedOutput;
        if (dockerTerminalMode) {
            QDialog terminalDialog;
            terminalDialog.setWindowTitle(QStringLiteral("BoxStream Docker Terminal"));
            terminalDialog.setWindowFlag(Qt::Window, true);
            terminalDialog.resize(980, 560);
            auto* terminalLayout = new QVBoxLayout(&terminalDialog);
            terminalLayout->setContentsMargins(12, 12, 12, 12);
            terminalLayout->setSpacing(8);
            auto* statusLabel = new QLabel(QStringLiteral("Running detector container..."), &terminalDialog);
            terminalLayout->addWidget(statusLabel);
            auto* stepsLabel = new QLabel(
                QStringLiteral("Steps: 1) Detect  2) Associate  3) Smooth  4) Write BoxStreams  5) Preview Overlay"),
                &terminalDialog);
            stepsLabel->setStyleSheet(QStringLiteral("color:#9fb3c8; font-size:11px;"));
            terminalLayout->addWidget(stepsLabel);
            auto* output = new QPlainTextEdit(&terminalDialog);
            output->setReadOnly(true);
            output->setLineWrapMode(QPlainTextEdit::NoWrap);
            output->setStyleSheet(QStringLiteral(
                "QPlainTextEdit { background: #05080c; color: #d6e2ee; border: 1px solid #24303c; "
                "border-radius: 8px; font-family: monospace; font-size: 12px; }"));
            terminalLayout->addWidget(output, 1);
            auto* previewLabel = new QLabel(&terminalDialog);
            previewLabel->setMinimumSize(640, 360);
            previewLabel->setAlignment(Qt::AlignCenter);
            previewLabel->setStyleSheet(QStringLiteral("background:#111; border:1px solid #444;"));
            terminalLayout->addWidget(previewLabel, 1);
            auto* buttonRow = new QHBoxLayout;
            buttonRow->setContentsMargins(0, 0, 0, 0);
            auto* cancelButton = new QPushButton(QStringLiteral("Cancel"), &terminalDialog);
            auto* closeButton = new QPushButton(QStringLiteral("Close"), &terminalDialog);
            closeButton->setEnabled(false);
            buttonRow->addWidget(cancelButton);
            buttonRow->addStretch(1);
            buttonRow->addWidget(closeButton);
            terminalLayout->addLayout(buttonRow);
            bool canceled = false;
            QObject::connect(cancelButton, &QPushButton::clicked, [&]() { canceled = true; });
            QObject::connect(closeButton, &QPushButton::clicked, &terminalDialog, &QDialog::accept);
            proc.setProcessChannelMode(QProcess::MergedChannels);
            proc.start();
            terminalDialog.show();
            QElapsedTimer etaTimer;
            etaTimer.start();
            QTimer previewTimer;
            QObject::connect(&previewTimer, &QTimer::timeout, [&]() {
                QImage img(previewHostPath);
                if (!img.isNull()) {
                    previewLabel->setPixmap(QPixmap::fromImage(
                        img.scaled(previewLabel->size(), Qt::KeepAspectRatio, Qt::SmoothTransformation)));
                }
                QFile pf(progressHostPath);
                if (pf.open(QIODevice::ReadOnly)) {
                    QJsonParseError pe;
                    const QJsonDocument pd = QJsonDocument::fromJson(pf.readAll(), &pe);
                    if (pe.error == QJsonParseError::NoError && pd.isObject()) {
                        const QJsonObject po = pd.object();
                        const int startFrame = po.value(QStringLiteral("start")).toInt(0);
                        const int frame = po.value(QStringLiteral("frame")).toInt(0);
                        const int end = po.value(QStringLiteral("end")).toInt(0);
                        const int det = po.value(QStringLiteral("detections")).toInt(0);
                        const int trk = po.value(QStringLiteral("tracks")).toInt(0);
                        double etaSec = -1.0;
                        const int done = qMax(0, frame - startFrame);
                        const int total = qMax(1, end - startFrame);
                        if (done > 0) {
                            const double elapsedSec = static_cast<double>(etaTimer.elapsed()) / 1000.0;
                            const double secPerFrame = elapsedSec / static_cast<double>(done);
                            etaSec = secPerFrame * static_cast<double>(qMax(0, total - done));
                        }
                        statusLabel->setText(
                            QStringLiteral("Step 1-3: Running detector container... frame=%1/%2 det=%3 tracks=%4  %5")
                                .arg(frame)
                                .arg(end)
                                .arg(det)
                                .arg(trk)
                                .arg(formatEtaSeconds(etaSec)));
                    }
                }
            });
            previewTimer.start(200);
            while (proc.state() != QProcess::NotRunning) {
                if (canceled) {
                    proc.kill();
                    proc.waitForFinished(1000);
                    break;
                }
                proc.waitForFinished(100);
                const QString chunk = QString::fromLocal8Bit(proc.readAllStandardOutput());
                if (!chunk.isEmpty()) {
                    mergedOutput.append(chunk);
                    output->moveCursor(QTextCursor::End);
                    output->insertPlainText(chunk);
                    output->moveCursor(QTextCursor::End);
                }
                QApplication::processEvents();
            }
            const QString tail = QString::fromLocal8Bit(proc.readAllStandardOutput());
            if (!tail.isEmpty()) {
                mergedOutput.append(tail);
                output->moveCursor(QTextCursor::End);
                output->insertPlainText(tail);
                output->moveCursor(QTextCursor::End);
            }
            previewTimer.stop();
            statusLabel->setText(QStringLiteral("Step 4 complete: detector output written. Step 5 ready in Preview."));
            cancelButton->setEnabled(false);
            closeButton->setEnabled(true);
            if (canceled) {
                QMessageBox::information(nullptr, QStringLiteral("Generate BoxStream"),
                                         QStringLiteral("Docker detector canceled."));
                return;
            }
            terminalDialog.exec();
        } else {
            proc.start();
            proc.waitForFinished(-1);
            mergedOutput = QString::fromUtf8(proc.readAllStandardOutput());
        }

        if (proc.exitStatus() != QProcess::NormalExit ||
            proc.exitCode() != 0) {
            processOutput = QString::fromUtf8(proc.readAllStandardError());
            if (processOutput.trimmed().isEmpty()) {
                processOutput = mergedOutput.trimmed();
            }
            if (processOutput.trimmed().isEmpty()) {
                processOutput = QStringLiteral("External detector process failed.");
            }
            speaker_flow_debug::persistIndex(
                indexPath, debugRun.runId, debugRun.clipToken, QFileInfo(selectedClip->filePath).fileName(),
                m_loadedTranscriptPath, QStringLiteral("stage_6_boxstream"), QStringLiteral("error"),
                processOutput, {requestPath, outputPath, logPath});
            QMessageBox::warning(nullptr, QStringLiteral("Generate BoxStream"), processOutput);
            return;
        }

        auto normalizeBox = [](const QJsonObject& obj, double* x, double* y, double* box) -> bool {
            if (!x || !y || !box) return false;
            if (obj.contains(QStringLiteral("x")) && obj.contains(QStringLiteral("y")) && obj.contains(QStringLiteral("box"))) {
                *x = obj.value(QStringLiteral("x")).toDouble(-1.0);
                *y = obj.value(QStringLiteral("y")).toDouble(-1.0);
                *box = obj.value(QStringLiteral("box")).toDouble(-1.0);
                return *x >= 0.0 && *y >= 0.0 && *box > 0.0;
            }
            auto pick = [&](const QStringList& keys, double fallback = -1.0) {
                for (const QString& k : keys) {
                    if (obj.contains(k)) return obj.value(k).toDouble(fallback);
                }
                return fallback;
            };
            double left = pick({QStringLiteral("left"), QStringLiteral("x1"), QStringLiteral("xmin"), QStringLiteral("box_left")});
            double top = pick({QStringLiteral("top"), QStringLiteral("y1"), QStringLiteral("ymin"), QStringLiteral("box_top")});
            double right = pick({QStringLiteral("right"), QStringLiteral("x2"), QStringLiteral("xmax"), QStringLiteral("box_right")});
            double bottom = pick({QStringLiteral("bottom"), QStringLiteral("y2"), QStringLiteral("ymax"), QStringLiteral("box_bottom")});
            if (left >= 0.0 && top >= 0.0 && right > left && bottom > top) {
                const double w = right - left;
                const double h = bottom - top;
                *x = left + (w * 0.5);
                *y = top + (h * 0.5);
                *box = qMax(w, h);
                return true;
            }
            if (obj.contains(QStringLiteral("bbox")) && obj.value(QStringLiteral("bbox")).isArray()) {
                const QJsonArray a = obj.value(QStringLiteral("bbox")).toArray();
                if (a.size() >= 4) {
                    left = a.at(0).toDouble(-1.0);
                    top = a.at(1).toDouble(-1.0);
                    const double w = a.at(2).toDouble(-1.0);
                    const double h = a.at(3).toDouble(-1.0);
                    if (left >= 0.0 && top >= 0.0 && w > 0.0 && h > 0.0) {
                        *x = left + (w * 0.5);
                        *y = top + (h * 0.5);
                        *box = qMax(w, h);
                        return true;
                    }
                }
            }
            return false;
        };
        auto loadTracksFromJson = [&](const QString& path, QJsonArray* outTracks) -> bool {
            if (!outTracks) return false;
            QFile f(path);
            if (!f.open(QIODevice::ReadOnly)) return false;
            QJsonParseError e;
            const QJsonDocument doc = QJsonDocument::fromJson(f.readAll(), &e);
            if (e.error != QJsonParseError::NoError || !doc.isObject()) return false;
            const QJsonObject root = doc.object();
            if (root.contains(QStringLiteral("tracks")) && root.value(QStringLiteral("tracks")).isArray()) {
                *outTracks = root.value(QStringLiteral("tracks")).toArray();
                return !outTracks->isEmpty();
            }
            const QJsonArray detections = root.value(QStringLiteral("detections")).toArray();
            if (!detections.isEmpty()) {
                QJsonObject track;
                track[QStringLiteral("track_id")] = 1;
                track[QStringLiteral("detections")] = detections;
                outTracks->push_back(track);
                return true;
            }
            return false;
        };
        if (detectorPreset != BoxstreamDetectorPreset::Sam3Face) {
            QFile outputFile(jsonPathToRead);
            if (!outputFile.open(QIODevice::ReadOnly)) {
                QMessageBox::warning(nullptr, QStringLiteral("Generate BoxStream"),
                                     QStringLiteral("Detector finished but no output JSON was produced."));
                return;
            }
            QJsonParseError parseError;
            const QJsonDocument outputDoc = QJsonDocument::fromJson(outputFile.readAll(), &parseError);
            if (parseError.error != QJsonParseError::NoError || !outputDoc.isObject()) {
                QMessageBox::warning(nullptr, QStringLiteral("Generate BoxStream"),
                                     QStringLiteral("Detector output JSON was invalid."));
                return;
            }
            tracksJson = outputDoc.object().value(QStringLiteral("tracks")).toArray();
        } else {
            QJsonArray loaded;
            if (!loadTracksFromJson(jsonPathToRead, &loaded)) {
                QDir runDir(debugRun.runDir);
                const QStringList jsonFiles = runDir.entryList({QStringLiteral("*.json")}, QDir::Files, QDir::Time);
                for (const QString& name : jsonFiles) {
                    const QString p = runDir.absoluteFilePath(name);
                    if (loadTracksFromJson(p, &loaded)) {
                        break;
                    }
                }
            }
            if (loaded.isEmpty()) {
                // Build detections from any bbox-like JSON objects.
                QDir runDir(debugRun.runDir);
                const QStringList jsonFiles = runDir.entryList({QStringLiteral("*.json")}, QDir::Files, QDir::Time);
                QJsonArray dets;
                for (const QString& name : jsonFiles) {
                    QFile jf(runDir.absoluteFilePath(name));
                    if (!jf.open(QIODevice::ReadOnly)) continue;
                    QJsonParseError pe;
                    const QJsonDocument jd = QJsonDocument::fromJson(jf.readAll(), &pe);
                    if (pe.error != QJsonParseError::NoError) continue;
                    QList<QJsonValue> stack;
                    if (jd.isObject()) stack.push_back(jd.object());
                    if (jd.isArray()) stack.push_back(jd.array());
                    while (!stack.isEmpty()) {
                        const QJsonValue v = stack.takeLast();
                        if (v.isObject()) {
                            const QJsonObject o = v.toObject();
                            double x = -1.0, y = -1.0, box = -1.0;
                            if (normalizeBox(o, &x, &y, &box)) {
                                const double frame = o.value(QStringLiteral("frame")).toDouble(
                                    o.value(QStringLiteral("frame_index")).toDouble(scanStart));
                                QJsonObject d;
                                d[QStringLiteral("frame")] = static_cast<qint64>(qMax<double>(0.0, frame));
                                d[QStringLiteral("x")] = qBound(0.0, x, 1.0);
                                d[QStringLiteral("y")] = qBound(0.0, y, 1.0);
                                d[QStringLiteral("box")] = qBound(0.01, box, 1.0);
                                d[QStringLiteral("score")] = 1.0;
                                dets.push_back(d);
                            }
                            for (auto it = o.constBegin(); it != o.constEnd(); ++it) stack.push_back(it.value());
                        } else if (v.isArray()) {
                            const QJsonArray a = v.toArray();
                            for (const QJsonValue& av : a) stack.push_back(av);
                        }
                    }
                    if (!dets.isEmpty()) break;
                }
                if (!dets.isEmpty()) {
                    QJsonObject t;
                    t[QStringLiteral("track_id")] = 1;
                    t[QStringLiteral("detections")] = dets;
                    loaded.push_back(t);
                }
            }
            tracksJson = loaded;
            if (tracksJson.isEmpty()) {
                QMessageBox::warning(nullptr, QStringLiteral("Generate BoxStream"),
                                     QStringLiteral("SAM3 completed but no bounding boxes were found in its JSON outputs."));
                return;
            }
        }
        processOutput = mergedOutput.trimmed();
        if (processOutput.isEmpty()) {
            processOutput = QStringLiteral("External detector complete. tracks=%1").arg(tracksJson.size());
        }
    } else {
#if !JCUT_HAVE_OPENCV
    speaker_flow_debug::persistIndex(
        indexPath, debugRun.runId, debugRun.clipToken, QFileInfo(selectedClip->filePath).fileName(),
        m_loadedTranscriptPath, QStringLiteral("stage_6_boxstream"), QStringLiteral("error"),
        QStringLiteral("OpenCV C++ not available in this build."), {requestPath});
    QMessageBox::warning(nullptr, QStringLiteral("Generate BoxStream"),
                         QStringLiteral("OpenCV is not linked in this build."));
    return;
#else
    QString cascadePath;
    const QStringList cascadeCandidates = {
        QDir(QDir::currentPath()).absoluteFilePath(
            QStringLiteral("external/opencv/data/haarcascades/haarcascade_frontalface_default.xml")),
        QStringLiteral("/usr/share/opencv4/haarcascades/haarcascade_frontalface_default.xml"),
        QStringLiteral("/usr/local/share/opencv4/haarcascades/haarcascade_frontalface_default.xml")
    };
    for (const QString& candidate : cascadeCandidates) {
        if (QFileInfo::exists(candidate)) {
            cascadePath = candidate;
            break;
        }
    }
    if (cascadePath.isEmpty()) {
        speaker_flow_debug::persistIndex(
            indexPath, debugRun.runId, debugRun.clipToken, QFileInfo(selectedClip->filePath).fileName(),
            m_loadedTranscriptPath, QStringLiteral("stage_6_boxstream"), QStringLiteral("error"),
            QStringLiteral("Haar cascade XML not found."), {requestPath});
        QMessageBox::warning(nullptr, QStringLiteral("Generate BoxStream"),
                             QStringLiteral("Haar cascade file not found. Expected it under external/opencv/data/haarcascades."));
        return;
    }

    cv::CascadeClassifier faceCascade;
    if (!faceCascade.load(cascadePath.toStdString())) {
        speaker_flow_debug::persistIndex(
            indexPath, debugRun.runId, debugRun.clipToken, QFileInfo(selectedClip->filePath).fileName(),
            m_loadedTranscriptPath, QStringLiteral("stage_6_boxstream"), QStringLiteral("error"),
            QStringLiteral("Failed to load Haar cascade XML."), {requestPath});
        QMessageBox::warning(nullptr, QStringLiteral("Generate BoxStream"),
                             QStringLiteral("Failed to load Haar cascade classifier."));
        return;
    }

    cv::CascadeClassifier faceCascadeAlt;
    cv::CascadeClassifier faceCascadeProfile;
    cv::CascadeClassifier lbpCascade;
    QString lbpPath;
    for (const QString& root : {
             QFileInfo(cascadePath).absolutePath(),
             QStringLiteral("/usr/share/opencv4/haarcascades"),
             QStringLiteral("/usr/local/share/opencv4/haarcascades")}) {
        if (root.isEmpty()) {
            continue;
        }
        const QString altPath = QDir(root).absoluteFilePath(QStringLiteral("haarcascade_frontalface_alt2.xml"));
        if (!faceCascadeAlt.empty() || !QFileInfo::exists(altPath)) {
        } else {
            faceCascadeAlt.load(altPath.toStdString());
        }
        const QString profilePath = QDir(root).absoluteFilePath(QStringLiteral("haarcascade_profileface.xml"));
        if (!faceCascadeProfile.empty() || !QFileInfo::exists(profilePath)) {
        } else {
            faceCascadeProfile.load(profilePath.toStdString());
        }
        const QString lbpCandidate = QDir(root).absoluteFilePath(QStringLiteral("../lbpcascades/lbpcascade_frontalface_improved.xml"));
        if (lbpPath.isEmpty() && QFileInfo::exists(lbpCandidate)) {
            lbpPath = lbpCandidate;
        }
    }
    if (!lbpPath.isEmpty()) {
        lbpCascade.load(lbpPath.toStdString());
    }

    editor::DecoderContext decoder(mediaPath);
    if (!decoder.initialize()) {
        speaker_flow_debug::persistIndex(
            indexPath, debugRun.runId, debugRun.clipToken, QFileInfo(selectedClip->filePath).fileName(),
            m_loadedTranscriptPath, QStringLiteral("stage_6_boxstream"), QStringLiteral("error"),
            QStringLiteral("Failed to initialize decoder for Generate BoxStream."), {requestPath});
        QMessageBox::warning(nullptr, QStringLiteral("Generate BoxStream"),
                             QStringLiteral("Could not open media for face scanning."));
        return;
    }
    const bool wantsVulkanBoxstream = detectorPreset == BoxstreamDetectorPreset::NativeHybridVulkan;
    VulkanBoxstreamFrameProvider vulkanFrameProvider;
    int vulkanFrameCount = 0;
    int vulkanFallbackFrameCount = 0;
    if (wantsVulkanBoxstream) {
        const QSize sourceSize = decoder.info().frameSize.isValid()
            ? decoder.info().frameSize
            : QSize(1920, 1080);
        if (!vulkanFrameProvider.ensureInitialized(sourceSize)) {
            ++vulkanFallbackFrameCount;
            qWarning().noquote()
                << QStringLiteral("[boxstream] native_hybrid_vulkan_v1 unavailable; falling back to CPU frame materialization: %1")
                       .arg(vulkanFrameProvider.failureReason);
        }
    }
    DnnFaceDetectorRuntime dnnRuntime;
    NativeVulkanFacePreprocessRuntime nativeVulkanPreprocess;
    const bool wantsNativeJcutVulkanDnn = detectorPreset == BoxstreamDetectorPreset::NativeVulkanDnn;
    int nativeVulkanPreprocessFrames = 0;
    int nativeVulkanPreprocessFailures = 0;
    double nativeVulkanPreprocessMsTotal = 0.0;
    const bool wantsContribTracker =
        detectorPreset == BoxstreamDetectorPreset::ContribCsrt ||
        detectorPreset == BoxstreamDetectorPreset::ContribKcf;
#if !JCUT_HAVE_OPENCV_CONTRIB
    if (wantsContribTracker) {
        speaker_flow_debug::persistIndex(
            indexPath, debugRun.runId, debugRun.clipToken, QFileInfo(selectedClip->filePath).fileName(),
            m_loadedTranscriptPath, QStringLiteral("stage_6_boxstream"), QStringLiteral("error"),
            QStringLiteral("OpenCV contrib tracking is not enabled in this build."),
            {requestPath});
        QMessageBox::warning(nullptr, QStringLiteral("Generate BoxStream"),
                             QStringLiteral("OpenCV contrib tracking is unavailable. Rebuild with JCUT_USE_OPENCV_CONTRIB=ON."));
        return;
    }
#endif
    const bool wantsDnnDetector = isDnnBoxstreamPreset(detectorPreset);
    const bool wantsOpenCvDnnDetector = wantsDnnDetector && !wantsNativeJcutVulkanDnn;
    if (wantsNativeJcutVulkanDnn) {
        QString vulkanError;
        if (!nativeVulkanPreprocess.initialize(&vulkanError)) {
            speaker_flow_debug::persistIndex(
                indexPath, debugRun.runId, debugRun.clipToken, QFileInfo(selectedClip->filePath).fileName(),
                m_loadedTranscriptPath, QStringLiteral("stage_6_boxstream"), QStringLiteral("error"),
                vulkanError.isEmpty()
                    ? QStringLiteral("Failed to initialize native JCut Vulkan face preprocessing.")
                    : vulkanError,
                {requestPath});
            QMessageBox::warning(nullptr, QStringLiteral("Generate BoxStream"),
                                 vulkanError.isEmpty()
                                     ? QStringLiteral("Native JCut Vulkan face preprocessing is unavailable.")
                                     : vulkanError);
            return;
        }
        detectorMode = nativeVulkanPreprocess.backendId();
    }
    if (wantsOpenCvDnnDetector) {
        QString dnnProto;
        QString dnnModel;
        if (!ensureFaceDnnModel(QDir::currentPath(), &dnnProto, &dnnModel)) {
            speaker_flow_debug::persistIndex(
                indexPath, debugRun.runId, debugRun.clipToken, QFileInfo(selectedClip->filePath).fileName(),
                m_loadedTranscriptPath, QStringLiteral("stage_6_boxstream"), QStringLiteral("error"),
                QStringLiteral("Failed to resolve/download OpenCV DNN face model."), {requestPath});
            QMessageBox::warning(nullptr, QStringLiteral("Generate BoxStream"),
                                 QStringLiteral("DNN model missing and download failed."));
            return;
        }
        try {
            dnnRuntime.net = cv::dnn::readNetFromCaffe(dnnProto.toStdString(), dnnModel.toStdString());
            dnnRuntime.net.setPreferableBackend(cv::dnn::DNN_BACKEND_OPENCV);
            dnnRuntime.net.setPreferableTarget(cv::dnn::DNN_TARGET_CPU);
            if (detectorPreset == BoxstreamDetectorPreset::NativeCudaDnn) {
                const std::vector<cv::dnn::Target> cudaTargets =
                    cv::dnn::getAvailableTargets(cv::dnn::DNN_BACKEND_CUDA);
                if (std::find(cudaTargets.begin(), cudaTargets.end(), cv::dnn::DNN_TARGET_CUDA_FP16) != cudaTargets.end()) {
                    dnnRuntime.net.setPreferableTarget(cv::dnn::DNN_TARGET_CUDA_FP16);
                } else if (std::find(cudaTargets.begin(), cudaTargets.end(), cv::dnn::DNN_TARGET_CUDA) != cudaTargets.end()) {
                    dnnRuntime.net.setPreferableTarget(cv::dnn::DNN_TARGET_CUDA);
                } else {
                    speaker_flow_debug::persistIndex(
                        indexPath, debugRun.runId, debugRun.clipToken, QFileInfo(selectedClip->filePath).fileName(),
                        m_loadedTranscriptPath, QStringLiteral("stage_6_boxstream"), QStringLiteral("error"),
                        QStringLiteral("OpenCV DNN CUDA backend is unavailable in this build/runtime."), {requestPath});
                    QMessageBox::warning(nullptr, QStringLiteral("Generate BoxStream"),
                                         QStringLiteral("OpenCV DNN CUDA backend is unavailable. Rebuild OpenCV with CUDA, cuBLAS, cuDNN, and OPENCV_DNN_CUDA enabled."));
                    return;
                }
                dnnRuntime.net.setPreferableBackend(cv::dnn::DNN_BACKEND_CUDA);
                dnnRuntime.usingCuda = true;
                detectorMode = QStringLiteral("native_cuda_dnn_v1");
            } else {
                const std::vector<cv::dnn::Target> cudaTargets =
                    cv::dnn::getAvailableTargets(cv::dnn::DNN_BACKEND_CUDA);
                if (std::find(cudaTargets.begin(), cudaTargets.end(), cv::dnn::DNN_TARGET_CUDA_FP16) != cudaTargets.end() ||
                    std::find(cudaTargets.begin(), cudaTargets.end(), cv::dnn::DNN_TARGET_CUDA) != cudaTargets.end()) {
                    try {
                        if (std::find(cudaTargets.begin(), cudaTargets.end(), cv::dnn::DNN_TARGET_CUDA_FP16) != cudaTargets.end()) {
                            dnnRuntime.net.setPreferableTarget(cv::dnn::DNN_TARGET_CUDA_FP16);
                        } else {
                            dnnRuntime.net.setPreferableTarget(cv::dnn::DNN_TARGET_CUDA);
                        }
                        dnnRuntime.net.setPreferableBackend(cv::dnn::DNN_BACKEND_CUDA);
                        dnnRuntime.usingCuda = true;
                    } catch (const cv::Exception&) {
                        dnnRuntime.net.setPreferableBackend(cv::dnn::DNN_BACKEND_OPENCV);
                        dnnRuntime.net.setPreferableTarget(cv::dnn::DNN_TARGET_CPU);
                        dnnRuntime.usingCuda = false;
                    }
                } else {
                    dnnRuntime.net.setPreferableBackend(cv::dnn::DNN_BACKEND_OPENCV);
                    dnnRuntime.net.setPreferableTarget(cv::dnn::DNN_TARGET_CPU);
                    dnnRuntime.usingCuda = false;
                }
                detectorMode = dnnRuntime.usingCuda
                                   ? QStringLiteral("opencv_dnn_cuda_v1")
                                   : QStringLiteral("opencv_dnn_cpu_v1");
            }
            dnnRuntime.loaded = true;
        } catch (const cv::Exception&) {
            speaker_flow_debug::persistIndex(
                indexPath, debugRun.runId, debugRun.clipToken, QFileInfo(selectedClip->filePath).fileName(),
                m_loadedTranscriptPath, QStringLiteral("stage_6_boxstream"), QStringLiteral("error"),
                QStringLiteral("Failed to load OpenCV DNN model."), {requestPath});
            QMessageBox::warning(nullptr, QStringLiteral("Generate BoxStream"),
                                 QStringLiteral("Could not initialize DNN face detector."));
            return;
        }
    }

    QVector<ContinuityTrackState> continuityTracks;
    int nextTrackId = 0;
    int sampledFrames = 0;
    int totalDetections = 0;
    bool canceled = false;
    int64_t contribSampleCounter = 0;

    QDialog progressDialog;
    progressDialog.setWindowTitle(QStringLiteral("BoxStream Progress"));
    progressDialog.setWindowFlag(Qt::Window, true);
    progressDialog.resize(720, 560);
    auto* progressLayout = new QVBoxLayout(&progressDialog);
    progressLayout->setContentsMargins(10, 10, 10, 10);
    progressLayout->setSpacing(8);
    auto* progressLabel = new QLabel(QStringLiteral("Scanning continuity tracks..."), &progressDialog);
    if (wantsNativeJcutVulkanDnn) {
        progressLabel->setText(QStringLiteral("Running JCut Vulkan face preprocessing. Native Vulkan inference is not implemented yet."));
    }
    progressLayout->addWidget(progressLabel);
    auto* frameLabel = new QLabel(QStringLiteral("Frame 0/0 | Tracks: 0"), &progressDialog);
    progressLayout->addWidget(frameLabel);
    auto* previewLabel = new QLabel(&progressDialog);
    previewLabel->setMinimumSize(640, 360);
    previewLabel->setAlignment(Qt::AlignCenter);
    previewLabel->setStyleSheet(QStringLiteral("background:#111; border:1px solid #444;"));
    progressLayout->addWidget(previewLabel, 1);
    auto* progressBar = new QProgressBar(&progressDialog);
    progressBar->setRange(0, 100);
    progressBar->setValue(0);
    progressLayout->addWidget(progressBar);
    auto* progressButtons = new QHBoxLayout;
    progressButtons->addStretch(1);
    auto* cancelButton = new QPushButton(QStringLiteral("Cancel"), &progressDialog);
    progressButtons->addWidget(cancelButton);
    progressLayout->addLayout(progressButtons);
    QObject::connect(cancelButton, &QPushButton::clicked, [&]() { canceled = true; });
    progressDialog.show();
    QApplication::processEvents();

    const int64_t totalSteps = qMax<int64_t>(1, ((scanEnd - scanStart) / stepFrames) + 1);
    int64_t stepIndex = 0;
    QElapsedTimer nativeEtaTimer;
    nativeEtaTimer.start();
    for (int64_t timelineFrame = scanStart; timelineFrame <= scanEnd; timelineFrame += stepFrames) {
        if (canceled) {
            break;
        }
        ++stepIndex;
        const int progressValue = static_cast<int>((stepIndex * 100) / totalSteps);
        progressBar->setValue(qBound(0, progressValue, 100));
        const double elapsedSec = static_cast<double>(nativeEtaTimer.elapsed()) / 1000.0;
        double etaSec = -1.0;
        if (stepIndex > 0) {
            const double secPerStep = elapsedSec / static_cast<double>(stepIndex);
            etaSec = secPerStep * static_cast<double>(qMax<int64_t>(0, totalSteps - stepIndex));
        }
        frameLabel->setText(QStringLiteral("Frame %1/%2 | Tracks: %3 | %4")
                                .arg(timelineFrame)
                                .arg(scanEnd)
                                .arg(continuityTracks.size())
                                .arg(formatEtaSeconds(etaSec)));
        if (onlyDialogue) {
            bool spoken = false;
            const QJsonArray segments = transcriptRoot.value(QStringLiteral("segments")).toArray();
            const double t = static_cast<double>(timelineFrame) / static_cast<double>(kTimelineFps);
            for (const QJsonValue& segValue : segments) {
                const QJsonObject segObj = segValue.toObject();
                const QJsonArray words = segObj.value(QStringLiteral("words")).toArray();
                for (const QJsonValue& wordValue : words) {
                    const QJsonObject wordObj = wordValue.toObject();
                    if (wordObj.value(QStringLiteral("skipped")).toBool(false)) {
                        continue;
                    }
                    const double ws = wordObj.value(QStringLiteral("start")).toDouble(-1.0);
                    const double we = wordObj.value(QStringLiteral("end")).toDouble(-1.0);
                    if (ws >= 0.0 && we >= ws && t >= ws && t <= we) {
                        spoken = true;
                        break;
                    }
                }
                if (spoken) {
                    break;
                }
            }
            if (!spoken) {
                QApplication::processEvents();
                continue;
            }
        }

        const int64_t sourceFrame = transcriptFrameForClipAtTimelineSample(
            *selectedClip, frameToSamples(timelineFrame), QVector<RenderSyncMarker>{});
        QImage image;
        if (wantsVulkanBoxstream && vulkanFrameProvider.initialized && !vulkanFrameProvider.failed) {
            const QSize sourceSize = decoder.info().frameSize.isValid()
                ? decoder.info().frameSize
                : QSize(1920, 1080);
            image = renderBoxstreamFrameWithVulkan(&vulkanFrameProvider,
                                                   *selectedClip,
                                                   mediaPath,
                                                   timelineFrame,
                                                   sourceFrame,
                                                   sourceSize);
            if (!image.isNull()) {
                ++vulkanFrameCount;
            } else {
                ++vulkanFallbackFrameCount;
            }
        }
        if (image.isNull()) {
            const editor::FrameHandle frame = decoder.decodeFrame(qMax<int64_t>(0, sourceFrame));
            image = frame.hasCpuImage() ? frame.cpuImage() : QImage();
        }
        if (image.isNull() || image.width() <= 0 || image.height() <= 0) {
            QApplication::processEvents();
            continue;
        }

        cv::Mat bgr;
        auto ensureBgr = [&]() -> const cv::Mat& {
            if (bgr.empty()) {
                bgr = qImageToBgrMat(image);
            }
            return bgr;
        };
        cv::Mat gray;
        auto ensureGray = [&]() {
            if (!gray.empty()) {
                return;
            }
            const cv::Mat& frameBgr = ensureBgr();
            cv::cvtColor(frameBgr, gray, cv::COLOR_BGR2GRAY);
            cv::equalizeHist(gray, gray);
        };
#if JCUT_HAVE_OPENCV_CONTRIB
        if (wantsContribTracker) {
            ++contribSampleCounter;
            const cv::Mat& frameBgr = ensureBgr();
            for (int i = 0; i < continuityTracks.size(); ++i) {
                if (!continuityTracks[i].tracker) {
                    continue;
                }
                cv::Rect2d trackedBox;
                if (continuityTracks[i].tracker->update(frameBgr, trackedBox) &&
                    trackedBox.width > 8.0 && trackedBox.height > 8.0) {
                    continuityTracks[i].box = cv::Rect(
                        static_cast<int>(std::round(trackedBox.x)),
                        static_cast<int>(std::round(trackedBox.y)),
                        static_cast<int>(std::round(trackedBox.width)),
                        static_cast<int>(std::round(trackedBox.height)));
                    continuityTracks[i].lastTimelineFrame = timelineFrame;
                }
            }
        }
#endif

        const int minSide = qMax(1, qMin(image.width(), image.height()));
        double scaleFactor = 1.08;
        int baseNeighbors = 5;
        int minDivisor = 22;
        double minWeight = 0.2;
        bool includeProfileCascade = true;
        if (detectorPreset == BoxstreamDetectorPreset::HaarSmallFaces) {
            scaleFactor = 1.04;
            baseNeighbors = 3;
            minDivisor = 38;
            minWeight = -0.1;
        } else if (detectorPreset == BoxstreamDetectorPreset::HaarPrecision) {
            scaleFactor = 1.12;
            baseNeighbors = 7;
            minDivisor = 20;
            minWeight = 0.35;
            includeProfileCascade = false;
        } else if (detectorPreset == BoxstreamDetectorPreset::LbpSmallFaces) {
            scaleFactor = 1.05;
            baseNeighbors = 3;
            minDivisor = 42;
            minWeight = -0.2;
        } else if (detectorPreset == BoxstreamDetectorPreset::PythonCompatible) {
            scaleFactor = 1.1;
            baseNeighbors = 4;
            minDivisor = 22;
            minWeight = -1.0;
            includeProfileCascade = false;
        }
        const cv::Size minFace(qMax(18, minSide / minDivisor), qMax(18, minSide / minDivisor));
        const cv::Size maxFace(qMax(minFace.width + 1, (minSide * 3) / 4),
                               qMax(minFace.height + 1, (minSide * 3) / 4));
        std::vector<cv::Rect> detections;
        std::vector<WeightedDetection> weightedDetections;
        auto runCascade = [&](cv::CascadeClassifier& cascade, int minNeighbors, double weightBias) {
            if (cascade.empty()) {
                return;
            }
            ensureGray();
            std::vector<cv::Rect> raw;
            std::vector<int> rejectLevels;
            std::vector<double> levelWeights;
            cascade.detectMultiScale(
                gray, raw, rejectLevels, levelWeights,
                scaleFactor, minNeighbors, cv::CASCADE_SCALE_IMAGE, minFace, maxFace, true);
            for (int i = 0; i < static_cast<int>(raw.size()); ++i) {
                const double w = (i < static_cast<int>(levelWeights.size()) ? levelWeights[i] : 0.0) + weightBias;
                if (w < minWeight) {
                    continue;
                }
                weightedDetections.push_back({raw[i], w});
            }
        };
        bool shouldRunDetector = true;
#if JCUT_HAVE_OPENCV_CONTRIB
        if (wantsContribTracker) {
            const int stride = qMax(1, g_boxstreamContribTrackingSettings.redetectStrideSamples);
            shouldRunDetector = (contribSampleCounter % stride) == 1 || continuityTracks.isEmpty();
        }
#endif
        if (!shouldRunDetector) {
            detections.clear();
        } else if (wantsNativeJcutVulkanDnn) {
            double preprocessMs = 0.0;
            QString preprocessError;
            QVector<NativeVulkanFacePreprocessRuntime::DetectionCandidate> nativeDetections;
            if (nativeVulkanPreprocess.detectCpuImage(image, &nativeDetections, &preprocessMs, &preprocessError)) {
                ++nativeVulkanPreprocessFrames;
                nativeVulkanPreprocessMsTotal += preprocessMs;
                for (const auto& nativeDetection : nativeDetections) {
                    const QRectF box = nativeDetection.normalizedBox;
                    cv::Rect rect(qRound(box.x() * image.width()),
                                  qRound(box.y() * image.height()),
                                  qRound(box.width() * image.width()),
                                  qRound(box.height() * image.height()));
                    rect &= cv::Rect(0, 0, image.width(), image.height());
                    if (rect.width >= 8 && rect.height >= 8) {
                        weightedDetections.push_back({rect, static_cast<double>(nativeDetection.confidence)});
                    }
                }
            } else {
                ++nativeVulkanPreprocessFailures;
                if (nativeVulkanPreprocessFailures == 1) {
                    qWarning().noquote()
                        << QStringLiteral("[boxstream] native JCut Vulkan preprocessing failed: %1")
                               .arg(preprocessError);
                }
            }
        } else if (detectorPreset == BoxstreamDetectorPreset::DnnAuto ||
                   detectorPreset == BoxstreamDetectorPreset::NativeCudaDnn) {
            detections = runDnnFaceDetect(&dnnRuntime, ensureBgr(), 0.45f);
        } else if (detectorPreset == BoxstreamDetectorPreset::PythonCompatible) {
            ensureGray();
            faceCascade.detectMultiScale(gray, detections, 1.1, 4, cv::CASCADE_SCALE_IMAGE, cv::Size(28, 28));
        } else if (detectorPreset == BoxstreamDetectorPreset::LbpSmallFaces && !lbpCascade.empty()) {
            runCascade(lbpCascade, baseNeighbors, 0.0);
        } else {
            runCascade(faceCascade, baseNeighbors, 0.0);
            runCascade(faceCascadeAlt, qMax(2, baseNeighbors - 1), -0.05);
            if (includeProfileCascade) {
                runCascade(faceCascadeProfile, qMax(2, baseNeighbors - 1), -0.10);
            }
        }
        if (detectorPreset != BoxstreamDetectorPreset::PythonCompatible) {
            const std::vector<WeightedDetection> filteredDetections =
                filterAndSuppressDetections(std::move(weightedDetections), image.size());
            detections.reserve(filteredDetections.size());
            for (const WeightedDetection& det : filteredDetections) {
                detections.push_back(det.box);
            }
        }
        if (wantsContribTracker && !shouldRunDetector &&
            g_boxstreamContribTrackingSettings.allowTrackerOnlyPropagation) {
            for (const ContinuityTrackState& t : continuityTracks) {
                if (t.lastTimelineFrame == timelineFrame &&
                    t.box.width > 8 && t.box.height > 8) {
                    detections.push_back(t.box);
                }
            }
        }
        const QImage previewImage = buildScanPreview(image, detections, continuityTracks.size());
        if (!previewImage.isNull()) {
            previewLabel->setPixmap(QPixmap::fromImage(
                previewImage.scaled(previewLabel->size(), Qt::KeepAspectRatio, Qt::SmoothTransformation)));
        }
        QApplication::processEvents();
        if (canceled) {
            break;
        }
        if (detections.empty()) {
            continue;
        }
        ++sampledFrames;

        std::sort(detections.begin(), detections.end(), [](const cv::Rect& lhs, const cv::Rect& rhs) {
            return lhs.area() > rhs.area();
        });
        if (static_cast<int>(detections.size()) > maxCandidates) {
            detections.resize(maxCandidates);
        }

        for (const cv::Rect& det : detections) {
            ++totalDetections;
            int bestTrackIndex = -1;
            double bestScore = (detectorPreset == BoxstreamDetectorPreset::PythonCompatible ||
                                isRelaxedDnnBoxstreamPreset(detectorPreset)) ? -1.0 : 0.0;
            for (int i = 0; i < continuityTracks.size(); ++i) {
                const int maxGap = (detectorPreset == BoxstreamDetectorPreset::PythonCompatible)
                                       ? qMax(stepFrames * 2, 90)
                                       : (stepFrames * 5);
                if (timelineFrame - continuityTracks[i].lastTimelineFrame > maxGap) {
                    continue;
                }
                double score = iou(continuityTracks[i].box, det);
                if (detectorPreset == BoxstreamDetectorPreset::PythonCompatible ||
                    isRelaxedDnnBoxstreamPreset(detectorPreset)) {
                    const double ax = continuityTracks[i].box.x + (continuityTracks[i].box.width * 0.5);
                    const double ay = continuityTracks[i].box.y + (continuityTracks[i].box.height * 0.5);
                    const double bx = det.x + (det.width * 0.5);
                    const double by = det.y + (det.height * 0.5);
                    const double centerDist = (std::abs(ax - bx) / qMax(1, image.width())) +
                                              (std::abs(ay - by) / qMax(1, image.height()));
                    score = score - (0.8 * centerDist);
                }
                if (score > bestScore) {
                    bestScore = score;
                    bestTrackIndex = i;
                }
            }

            const double newTrackThreshold =
                (detectorPreset == BoxstreamDetectorPreset::PythonCompatible ||
                 isRelaxedDnnBoxstreamPreset(detectorPreset)) ? 0.05 : 0.2;
            if (bestTrackIndex < 0 || bestScore < newTrackThreshold) {
                ContinuityTrackState state;
                state.id = nextTrackId++;
                state.box = det;
                state.lastTimelineFrame = timelineFrame;
#if JCUT_HAVE_OPENCV_CONTRIB
                if (wantsContribTracker) {
                    state.tracker = createContribTracker(detectorPreset);
                    if (state.tracker) {
                        state.tracker->init(ensureBgr(), det);
                    }
                }
#endif
                continuityTracks.push_back(state);
                bestTrackIndex = continuityTracks.size() - 1;
            } else {
                continuityTracks[bestTrackIndex].box = det;
                continuityTracks[bestTrackIndex].lastTimelineFrame = timelineFrame;
#if JCUT_HAVE_OPENCV_CONTRIB
                if (wantsContribTracker) {
                    continuityTracks[bestTrackIndex].tracker = createContribTracker(detectorPreset);
                    if (continuityTracks[bestTrackIndex].tracker) {
                        continuityTracks[bestTrackIndex].tracker->init(ensureBgr(), det);
                    }
                }
#endif
            }

            QJsonObject p;
            const double x = qBound(0.0, (det.x + (det.width * 0.5)) / static_cast<double>(image.width()), 1.0);
            const double y = qBound(0.0, (det.y + (det.height * 0.5)) / static_cast<double>(image.height()), 1.0);
            const double box = qBound(0.01, qMax(det.width, det.height) /
                                             static_cast<double>(qMax(1, qMin(image.width(), image.height()))),
                                      1.0);
            p[QStringLiteral("frame")] = static_cast<qint64>(timelineFrame);
            p[QStringLiteral("x")] = x;
            p[QStringLiteral("y")] = y;
            p[QStringLiteral("box")] = box;
            p[QStringLiteral("score")] = 1.0;
            continuityTracks[bestTrackIndex].detections.push_back(p);
        }
        const double elapsedSecTail = static_cast<double>(nativeEtaTimer.elapsed()) / 1000.0;
        double etaSecTail = -1.0;
        if (stepIndex > 0) {
            const double secPerStep = elapsedSecTail / static_cast<double>(stepIndex);
            etaSecTail = secPerStep * static_cast<double>(qMax<int64_t>(0, totalSteps - stepIndex));
        }
        frameLabel->setText(QStringLiteral("Frame %1/%2 | Tracks: %3 | %4")
                                .arg(timelineFrame)
                                .arg(scanEnd)
                                .arg(continuityTracks.size())
                                .arg(formatEtaSeconds(etaSecTail)));
        QApplication::processEvents();
    }
    progressDialog.close();

    if (canceled) {
        speaker_flow_debug::persistIndex(
            indexPath, debugRun.runId, debugRun.clipToken, QFileInfo(selectedClip->filePath).fileName(),
            m_loadedTranscriptPath, QStringLiteral("stage_6_boxstream"), QStringLiteral("error"),
            QStringLiteral("Generate BoxStream canceled by user."), {requestPath, outputPath, logPath});
        QMessageBox::information(nullptr, QStringLiteral("Generate BoxStream"),
                                 QStringLiteral("BoxStream generation canceled."));
        return;
    }

    for (const ContinuityTrackState& track : continuityTracks) {
        if (track.detections.isEmpty()) {
            continue;
        }
        if (detectorPreset != BoxstreamDetectorPreset::PythonCompatible) {
            const int minTrackDetections = isRelaxedDnnBoxstreamPreset(detectorPreset) ? 2 : 3;
            if (track.detections.size() < minTrackDetections) {
                continue;
            }
            const int64_t startFrame = track.detections.first().toObject().value(QStringLiteral("frame")).toVariant().toLongLong();
            const int64_t endFrame = track.detections.last().toObject().value(QStringLiteral("frame")).toVariant().toLongLong();
            const int64_t minTrackSpanFrames = isRelaxedDnnBoxstreamPreset(detectorPreset)
                                                   ? stepFrames
                                                   : (stepFrames * 2);
            if ((endFrame - startFrame) < minTrackSpanFrames) {
                continue;
            }
        }
        QJsonObject outTrack;
        outTrack[QStringLiteral("track_id")] = track.id;
        outTrack[QStringLiteral("detections")] = track.detections;
        tracksJson.push_back(outTrack);
    }
    const QString nativeLabel =
        detectorPreset == BoxstreamDetectorPreset::NativeHybridVulkan
            ? QStringLiteral("Native hybrid (Vulkan decode path)")
            : (detectorPreset == BoxstreamDetectorPreset::NativeCudaDnn
                   ? QStringLiteral("Native CUDA DNN")
                   : (detectorPreset == BoxstreamDetectorPreset::NativeVulkanDnn
                   ? QStringLiteral("Native JCut Vulkan preprocess")
                   : (detectorPreset == BoxstreamDetectorPreset::NativeHybridCpu
                          ? QStringLiteral("Native hybrid (C++ CPU)")
                          : QStringLiteral("OpenCV Haar"))));
    processOutput = QStringLiteral(
        "%1 continuity scan complete. frames_with_detections=%2 detections=%3 tracks=%4 vulkan_frames=%5 cpu_fallback_frames=%6 native_vulkan_inference_frames=%7 native_vulkan_inference_failures=%8 native_vulkan_inference_avg_ms=%9 native_vulkan_inference=jcut_compute_heuristic_v1")
                        .arg(nativeLabel)
                        .arg(sampledFrames)
                        .arg(totalDetections)
                        .arg(tracksJson.size())
                        .arg(vulkanFrameCount)
                        .arg(vulkanFallbackFrameCount)
                        .arg(nativeVulkanPreprocessFrames)
                        .arg(nativeVulkanPreprocessFailures)
                        .arg(nativeVulkanPreprocessFrames > 0
                                 ? nativeVulkanPreprocessMsTotal / nativeVulkanPreprocessFrames
                                 : 0.0,
                             0,
                             'f',
                             3);
#endif
    }

    if (tracksJson.isEmpty()) {
        QFile logFile(logPath);
        if (logFile.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
            logFile.write(processOutput.toUtf8());
            logFile.close();
        }
        const QString noTracksMessage = detectorPreset == BoxstreamDetectorPreset::NativeVulkanDnn
            ? QStringLiteral("Native JCut Vulkan inference ran, but no stable BoxStream tracks passed filtering.")
            : QStringLiteral("No face tracks detected by OpenCV continuity detector.");
        speaker_flow_debug::persistIndex(
            indexPath, debugRun.runId, debugRun.clipToken, QFileInfo(selectedClip->filePath).fileName(),
            m_loadedTranscriptPath, QStringLiteral("stage_6_boxstream"), QStringLiteral("error"),
            noTracksMessage, {requestPath, outputPath, logPath});
        QMessageBox::warning(nullptr, QStringLiteral("Generate BoxStream"),
                             detectorPreset == BoxstreamDetectorPreset::NativeVulkanDnn
                                 ? noTracksMessage
                                 : QStringLiteral("No face tracks were detected."));
        return;
    }
    QFile logFile(logPath);
    if (logFile.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        logFile.write(processOutput.toUtf8());
        logFile.close();
    }
    const QJsonArray streams = jcut::boxstream::buildContinuityStreams(
        tracksJson,
        transcriptRoot,
        detectorMode,
        onlyDialogue);

    const QString clipId = selectedClip->id.trimmed().isEmpty() ? QStringLiteral("unknown_clip") : selectedClip->id.trimmed();
    const QJsonObject continuityRoot = jcut::boxstream::buildContinuityRoot(
        debugRun.runId,
        onlyDialogue,
        scanStart,
        scanEnd,
        streams);
    QJsonObject artifactRoot;
    const bool saved = jcut::boxstream::saveContinuityArtifact(
        m_loadedTranscriptPath,
        clipId,
        continuityRoot,
        &artifactRoot);
    speaker_flow_debug::persistIndex(
        indexPath, debugRun.runId, debugRun.clipToken, QFileInfo(selectedClip->filePath).fileName(),
        m_loadedTranscriptPath, QStringLiteral("stage_6_boxstream"), saved ? QStringLiteral("ok") : QStringLiteral("error"),
        saved
            ? QStringLiteral("Continuity BoxStreams generated.")
            : QStringLiteral("Failed to save transcript after continuity BoxStream generation."),
        {requestPath, outputPath, logPath});

    if (!saved) {
        QMessageBox::warning(nullptr, QStringLiteral("Generate BoxStream"),
                             QStringLiteral("Generated continuity BoxStreams, but failed to save transcript."));
        refresh();
        return;
    }

    emit transcriptDocumentChanged();
    if (m_deps.scheduleSaveState) {
        m_deps.scheduleSaveState();
    }
    if (m_deps.pushHistorySnapshot) {
        m_deps.pushHistorySnapshot();
    }
    QMessageBox::information(
        nullptr,
        QStringLiteral("Generate BoxStream"),
        QStringLiteral("Generated %1 continuity BoxStream(s).")
            .arg(streams.size()));
    refresh();
}

