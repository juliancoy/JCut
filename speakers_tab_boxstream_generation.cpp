#include "speakers_tab.h"
#include "speakers_tab_internal.h"
#include "speaker_flow_debug.h"

#include "boxstream_generation.h"
#include "boxstream_runtime.h"
#include "decoder_context.h"
#include "detector_settings.h"
#include "render_internal.h"
#include "transcript_engine.h"
#include "vulkan_scrfd_ncnn_face_detector.h"

#include <QApplication>
#include <QCheckBox>
#include <QCoreApplication>
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
#include <QProgressBar>
#include <QPushButton>
#include <QVBoxLayout>

#include <algorithm>
#include <cmath>
#include <limits>

#if JCUT_HAVE_OPENCV
#include <opencv2/core.hpp>
#include <opencv2/imgproc.hpp>
#endif

using namespace jcut::boxstream;

namespace {

QString findFaceStreamModelFile(const QString& fileName)
{
    const QStringList roots{
        QDir::currentPath(),
        QCoreApplication::applicationDirPath(),
        QDir(QCoreApplication::applicationDirPath()).absoluteFilePath(QStringLiteral(".."))
    };
    const QStringList rels{
        QStringLiteral("assets/models/%1").arg(fileName),
        QStringLiteral("testbench_assets/models/%1").arg(fileName),
        QStringLiteral("models/%1").arg(fileName)
    };
    for (const QString& root : roots) {
        for (const QString& rel : rels) {
            const QString candidate = QDir(root).absoluteFilePath(rel);
            if (QFileInfo::exists(candidate)) {
                return candidate;
            }
        }
    }
    return QDir::current().absoluteFilePath(QStringLiteral("assets/models/%1").arg(fileName));
}

} // namespace

void SpeakersTab::onSpeakerRunAutoTrackClicked()
{
    if (!activeCutMutable() || !m_loadedTranscriptDoc.isObject()) {
        return;
    }
    const TimelineClip* selectedClip = m_deps.getSelectedClip ? m_deps.getSelectedClip() : nullptr;
    if (!selectedClip) {
        QMessageBox::warning(nullptr, QStringLiteral("JCut DNN FaceStream Generator"), QStringLiteral("Select a clip first."));
        return;
    }

    bool onlyDialogue = false;
    {
        QDialog preflightDialog;
        preflightDialog.setWindowTitle(QStringLiteral("JCut DNN FaceStream Generator"));
        preflightDialog.setWindowFlag(Qt::Window, true);
        preflightDialog.resize(560, 220);
        auto* layout = new QVBoxLayout(&preflightDialog);
        layout->setContentsMargins(12, 12, 12, 12);
        layout->setSpacing(8);
        auto* infoLabel = new QLabel(
            QStringLiteral("This run is identity-agnostic and continuity-based.\n\n"
                           "JCut DNN FaceStream Generator creates independent FaceStreams for all detected face tracks."),
            &preflightDialog);
        infoLabel->setWordWrap(true);
        layout->addWidget(infoLabel);
        auto* windowsCheck =
            new QCheckBox(QStringLiteral("Only scan when dialogue is present"), &preflightDialog);
        windowsCheck->setChecked(false);
        layout->addWidget(windowsCheck);
        auto* detectorLabel = new QLabel(
            QStringLiteral("Detector: SCRFD ncnn Vulkan (fixed)"),
            &preflightDialog);
        detectorLabel->setToolTip(QStringLiteral(
            "FaceStream generation uses SCRFD ncnn Vulkan for small-face recall. No alternate detector path is available in this flow."));
        layout->addWidget(detectorLabel);
        auto* stepsLabel = new QLabel(
            QStringLiteral("Pipeline steps:\n"
                           "1. Detect faces each sample frame\n"
                           "2. Associate detections into tracks (IoU + ReID where available)\n"
                           "3. Temporal smoothing (center + size)\n"
                           "4. Write continuity FaceStreams\n"
                           "5. Preview superimposed overlays by tracker source"),
            &preflightDialog);
        stepsLabel->setWordWrap(true);
        stepsLabel->setStyleSheet(QStringLiteral("color: #8fa3b8; font-size: 11px;"));
        layout->addWidget(stepsLabel);

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
        QMessageBox::warning(nullptr, QStringLiteral("JCut DNN FaceStream Generator"),
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

    DetectorRuntimeSettings detectorSettings;
    detectorSettings.stride = 6;
    detectorSettings.maxDetections = 128;
    detectorSettings.maxFacesPerFrame = 32;
    detectorSettings.threshold = 0.45f;
    detectorSettings.trackMatchIouThreshold = 0.05f;
    detectorSettings.newTrackMinConfidence = 0.45f;
    detectorSettings.primaryFaceOnly = false;
    const QString detectorSettingsPath = detectorSettingsPathForVideo(mediaPath);
    QDateTime detectorSettingsMtime;
    loadDetectorRuntimeSettingsFile(detectorSettingsPath, &detectorSettings, &detectorSettingsMtime);
    const int stepFrames = qMax(1, detectorSettings.stride);
    const int maxCandidates = qMax(1, detectorSettings.maxDetections);
    QJsonObject request;
    request[QStringLiteral("run_id")] = debugRun.runId;
    request[QStringLiteral("mode")] = QStringLiteral("continuity_identity_agnostic");
    request[QStringLiteral("media_path")] = mediaPath;
    request[QStringLiteral("scan_start_frame")] = static_cast<qint64>(scanStart);
    request[QStringLiteral("scan_end_frame")] = static_cast<qint64>(scanEnd);
    request[QStringLiteral("only_dialogue")] = onlyDialogue;
    request[QStringLiteral("step_frames")] = stepFrames;
    request[QStringLiteral("max_candidates")] = maxCandidates;
    request[QStringLiteral("detector_settings_file")] = detectorSettingsPath;
    request[QStringLiteral("detector_settings")] =
        detectorRuntimeSettingsToJson(detectorSettings, QStringLiteral("speakers_facestream"), 640);
    request[QStringLiteral("engine")] = QStringLiteral("native_cpp_continuity_v1");
    QFile reqFile(requestPath);
    if (reqFile.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        reqFile.write(QJsonDocument(request).toJson(QJsonDocument::Indented));
        reqFile.close();
    }

    QString detectorMode = QStringLiteral("scrfd_500m_ncnn_vulkan_materialized_input_v1");
    QString processOutput;
    QJsonArray tracksJson;

#if !JCUT_HAVE_OPENCV
    speaker_flow_debug::persistIndex(
        indexPath, debugRun.runId, debugRun.clipToken, QFileInfo(selectedClip->filePath).fileName(),
        m_loadedTranscriptPath, QStringLiteral("stage_6_boxstream"), QStringLiteral("error"),
        QStringLiteral("OpenCV C++ not available in this build."), {requestPath});
    QMessageBox::warning(nullptr, QStringLiteral("JCut DNN FaceStream Generator"),
                         QStringLiteral("OpenCV is not linked in this build."));
    return;
#else
    editor::DecoderContext decoder(mediaPath);
    if (!decoder.initialize()) {
        speaker_flow_debug::persistIndex(
            indexPath, debugRun.runId, debugRun.clipToken, QFileInfo(selectedClip->filePath).fileName(),
            m_loadedTranscriptPath, QStringLiteral("stage_6_boxstream"), QStringLiteral("error"),
            QStringLiteral("Failed to initialize decoder for JCut DNN FaceStream Generator."), {requestPath});
        QMessageBox::warning(nullptr, QStringLiteral("JCut DNN FaceStream Generator"),
                             QStringLiteral("Could not open media for face scanning."));
        return;
    }
    jcut::vulkan_detector::VulkanScrfdNcnnFaceDetector scrfdDetector;
    const QString scrfdParamPath = findFaceStreamModelFile(QStringLiteral("scrfd_500m-opt2.param"));
    const QString scrfdBinPath = findFaceStreamModelFile(QStringLiteral("scrfd_500m-opt2.bin"));
    QString scrfdError;
    if (!scrfdDetector.initialize(scrfdParamPath, scrfdBinPath, true, &scrfdError)) {
        speaker_flow_debug::persistIndex(
            indexPath, debugRun.runId, debugRun.clipToken, QFileInfo(selectedClip->filePath).fileName(),
            m_loadedTranscriptPath, QStringLiteral("stage_6_boxstream"), QStringLiteral("error"),
            scrfdError.isEmpty()
                ? QStringLiteral("Failed to initialize SCRFD FaceStream detector.")
                : scrfdError,
            {requestPath});
        QMessageBox::warning(nullptr, QStringLiteral("JCut DNN FaceStream Generator"),
                             scrfdError.isEmpty()
                                 ? QStringLiteral("SCRFD FaceStream detector is unavailable.")
                                 : scrfdError);
        return;
    }
    detectorMode = scrfdDetector.backendId();

    QVector<ContinuityTrackState> continuityTracks;
    int nextTrackId = 0;
    int sampledFrames = 0;
    int totalDetections = 0;
    bool canceled = false;
    QDialog progressDialog;
    progressDialog.setWindowTitle(QStringLiteral("JCut DNN FaceStream Generator"));
    progressDialog.setWindowFlag(Qt::Window, true);
    progressDialog.resize(720, 560);
    auto* progressLayout = new QVBoxLayout(&progressDialog);
    progressLayout->setContentsMargins(10, 10, 10, 10);
    progressLayout->setSpacing(8);
    auto* progressLabel = new QLabel(QStringLiteral("Running SCRFD FaceStream generator..."), &progressDialog);
    progressLayout->addWidget(progressLabel);
    auto* frameLabel = new QLabel(QStringLiteral("Frame 0/0 | Tracks: 0"), &progressDialog);
    progressLayout->addWidget(frameLabel);
    auto* previewLabel = new QLabel(&progressDialog);
    previewLabel->setMinimumSize(640, 360);
    previewLabel->setAlignment(Qt::AlignCenter);
    previewLabel->setStyleSheet(QStringLiteral("background:#111; border:1px solid #444;"));
    progressLayout->addWidget(previewLabel, 1);
    DetectorSettingsPanel detectorSettingsPanel =
        createDetectorSettingsPanel(&detectorSettings, detectorMode, 640, detectorSettingsPath, &progressDialog);
    detectorSettingsPanel.widget->setToolTip(QStringLiteral(
        "Live detector controls. Changes apply to the remaining scan frames and are saved beside the source video."));
    progressLayout->addWidget(detectorSettingsPanel.widget);
    QString detectorSettingsSaveError;
    if (saveDetectorRuntimeSettingsFile(detectorSettingsPath,
                                        detectorSettings,
                                        detectorMode,
                                        640,
                                        &detectorSettingsSaveError)) {
        detectorSettingsMtime = QFileInfo(detectorSettingsPath).lastModified();
    } else if (!detectorSettingsSaveError.isEmpty()) {
        qWarning().noquote() << detectorSettingsSaveError;
    }
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
        if (loadDetectorRuntimeSettingsFile(detectorSettingsPath, &detectorSettings, &detectorSettingsMtime)) {
            syncDetectorSettingsPanel(&detectorSettingsPanel, detectorSettings);
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
        const editor::FrameHandle frame = decoder.decodeFrame(qMax<int64_t>(0, sourceFrame));
        QImage image = frame.hasCpuImage() ? frame.cpuImage() : QImage();
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
        QString frameScrfdError;
        const QVector<jcut::vulkan_detector::ScrfdDetection> scrfdDetections =
            scrfdDetector.inferFromBgr(ensureBgr(), detectorSettings.threshold, 640, &frameScrfdError);
        if (!frameScrfdError.isEmpty() && scrfdDetections.isEmpty()) {
            qWarning().noquote()
                << QStringLiteral("[boxstream] SCRFD detection failed: %1").arg(frameScrfdError);
        }
        std::vector<cv::Rect> detections;
        detections.reserve(static_cast<size_t>(scrfdDetections.size()));
        for (const auto& scrfdDetection : scrfdDetections) {
            QRectF box = scrfdDetection.box.intersected(QRectF(0, 0, image.width(), image.height()));
            cv::Rect rect(qRound(box.x()),
                          qRound(box.y()),
                          qRound(box.width()),
                          qRound(box.height()));
            rect &= cv::Rect(0, 0, image.width(), image.height());
            if (rect.width >= 4 && rect.height >= 4) {
                detections.push_back(rect);
            }
        }
        auto applyDetectorSettingsToRects = [&](std::vector<cv::Rect> rects) {
            const int width = qMax(1, image.width());
            const int height = qMax(1, image.height());
            const double frameArea = static_cast<double>(width) * static_cast<double>(height);
            const QRectF roiRect(detectorSettings.roiX1 * width,
                                 detectorSettings.roiY1 * height,
                                 (detectorSettings.roiX2 - detectorSettings.roiX1) * width,
                                 (detectorSettings.roiY2 - detectorSettings.roiY1) * height);
            std::vector<cv::Rect> filtered;
            filtered.reserve(rects.size());
            for (cv::Rect rect : rects) {
                rect &= cv::Rect(0, 0, width, height);
                if (rect.width <= 1 || rect.height <= 1) {
                    continue;
                }
                const QPointF center(rect.x + rect.width * 0.5, rect.y + rect.height * 0.5);
                if (!roiRect.contains(center)) {
                    continue;
                }
                const double areaRatio = static_cast<double>(rect.area()) / qMax(1.0, frameArea);
                if (areaRatio < detectorSettings.minFaceAreaRatio ||
                    areaRatio > detectorSettings.maxFaceAreaRatio) {
                    continue;
                }
                const double aspect = static_cast<double>(rect.width) / qMax(1, rect.height);
                if (aspect < detectorSettings.minAspect || aspect > detectorSettings.maxAspect) {
                    continue;
                }
                bool keep = true;
                for (const cv::Rect& accepted : filtered) {
                    if (iou(accepted, rect) > detectorSettings.nmsIouThreshold) {
                        keep = false;
                        break;
                    }
                }
                if (keep) {
                    filtered.push_back(rect);
                }
            }
            std::sort(filtered.begin(), filtered.end(), [](const cv::Rect& lhs, const cv::Rect& rhs) {
                return lhs.area() > rhs.area();
            });
            if (detectorSettings.primaryFaceOnly && filtered.size() > 1) {
                filtered.resize(1);
            }
            if (detectorSettings.maxFacesPerFrame > 0 &&
                static_cast<int>(filtered.size()) > detectorSettings.maxFacesPerFrame) {
                filtered.resize(static_cast<size_t>(detectorSettings.maxFacesPerFrame));
            }
            return filtered;
        };
        detections = applyDetectorSettingsToRects(std::move(detections));
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
        const int activeMaxCandidates = qMax(1, detectorSettings.maxDetections);
        if (static_cast<int>(detections.size()) > activeMaxCandidates) {
            detections.resize(activeMaxCandidates);
        }

        for (const cv::Rect& det : detections) {
            ++totalDetections;
            int bestTrackIndex = -1;
            double bestScore = -1.0;
            for (int i = 0; i < continuityTracks.size(); ++i) {
                const int maxGap = stepFrames * 5;
                if (timelineFrame - continuityTracks[i].lastTimelineFrame > maxGap) {
                    continue;
                }
                const double ax = continuityTracks[i].box.x + (continuityTracks[i].box.width * 0.5);
                const double ay = continuityTracks[i].box.y + (continuityTracks[i].box.height * 0.5);
                const double bx = det.x + (det.width * 0.5);
                const double by = det.y + (det.height * 0.5);
                const double centerDist = (std::abs(ax - bx) / qMax(1, image.width())) +
                                          (std::abs(ay - by) / qMax(1, image.height()));
                const double score = iou(continuityTracks[i].box, det) - (0.8 * centerDist);
                if (score > bestScore) {
                    bestScore = score;
                    bestTrackIndex = i;
                }
            }

            const double newTrackThreshold = detectorSettings.trackMatchIouThreshold;
            if (detectorSettings.primaryFaceOnly && bestTrackIndex < 0 && !continuityTracks.isEmpty()) {
                bestTrackIndex = 0;
                bestScore = qMax(bestScore, newTrackThreshold);
            }
            if (bestTrackIndex < 0 || bestScore < newTrackThreshold) {
                if (detectorSettings.primaryFaceOnly && !continuityTracks.isEmpty()) {
                    continue;
                }
                ContinuityTrackState state;
                state.id = nextTrackId++;
                state.box = det;
                state.lastTimelineFrame = timelineFrame;
                continuityTracks.push_back(state);
                bestTrackIndex = continuityTracks.size() - 1;
            } else {
                continuityTracks[bestTrackIndex].box = det;
                continuityTracks[bestTrackIndex].lastTimelineFrame = timelineFrame;
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
            QStringLiteral("JCut DNN FaceStream Generator canceled by user."), {requestPath, outputPath, logPath});
        QMessageBox::information(nullptr, QStringLiteral("JCut DNN FaceStream Generator"),
                                 QStringLiteral("JCut DNN FaceStream generation canceled."));
        return;
    }

    for (const ContinuityTrackState& track : continuityTracks) {
        if (track.detections.isEmpty()) {
            continue;
        }
        if (track.detections.size() < 2) {
            continue;
        }
        const int64_t startFrame = track.detections.first().toObject().value(QStringLiteral("frame")).toVariant().toLongLong();
        const int64_t endFrame = track.detections.last().toObject().value(QStringLiteral("frame")).toVariant().toLongLong();
        if ((endFrame - startFrame) < stepFrames) {
            continue;
        }
        QJsonObject outTrack;
        outTrack[QStringLiteral("track_id")] = track.id;
        outTrack[QStringLiteral("detections")] = track.detections;
        tracksJson.push_back(outTrack);
    }
    processOutput = QStringLiteral(
        "SCRFD continuity scan complete. frames_with_detections=%1 detections=%2 tracks=%3")
                        .arg(sampledFrames)
                        .arg(totalDetections)
                        .arg(tracksJson.size());
#endif

    if (tracksJson.isEmpty()) {
        QFile logFile(logPath);
        if (logFile.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
            logFile.write(processOutput.toUtf8());
            logFile.close();
        }
        const QString noTracksMessage =
            QStringLiteral("SCRFD FaceStream generator ran, but no stable FaceStream tracks passed filtering.");
        speaker_flow_debug::persistIndex(
            indexPath, debugRun.runId, debugRun.clipToken, QFileInfo(selectedClip->filePath).fileName(),
            m_loadedTranscriptPath, QStringLiteral("stage_6_boxstream"), QStringLiteral("error"),
            noTracksMessage, {requestPath, outputPath, logPath});
        QMessageBox::warning(nullptr, QStringLiteral("JCut DNN FaceStream Generator"), noTracksMessage);
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
            ? QStringLiteral("Continuity FaceStreams generated.")
            : QStringLiteral("Failed to save transcript after continuity FaceStream generation."),
        {requestPath, outputPath, logPath});

    if (!saved) {
        QMessageBox::warning(nullptr, QStringLiteral("JCut DNN FaceStream Generator"),
                             QStringLiteral("Generated continuity FaceStreams, but failed to save transcript."));
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
        QStringLiteral("JCut DNN FaceStream Generator"),
        QStringLiteral("Generated %1 continuity FaceStream(s).")
            .arg(streams.size()));
    refresh();
}
